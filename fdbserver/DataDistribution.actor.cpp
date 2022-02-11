/*
 * DataDistribution.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <set>
#include <sstream>
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/Knobs.h"
#include "fdbclient/StorageServerInterface.h"
#include "fdbclient/SystemData.h"
#include "fdbclient/DatabaseContext.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/RunTransaction.actor.h"
#include "fdbrpc/Replication.h"
#include "fdbserver/DataDistribution.actor.h"
#include "fdbserver/FDBExecHelper.actor.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/MoveKeys.actor.h"
#include "fdbserver/QuietDatabase.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/TLogInterface.h"
#include "fdbserver/WaitFailure.h"
#include "fdbserver/DDTeamCollection.h"
#include "flow/ActorCollection.h"
#include "flow/Arena.h"
#include "flow/BooleanParam.h"
#include "flow/Trace.h"
#include "flow/UnitTest.h"
#include "flow/actorcompiler.h" // This must be the last #include.
#include "flow/serialize.h"

// Read keyservers, return unique set of teams
ACTOR Future<Reference<InitialDataDistribution>> getInitialDataDistribution(Database cx,
                                                                            UID distributorId,
                                                                            MoveKeysLock moveKeysLock,
                                                                            std::vector<Optional<Key>> remoteDcIds,
                                                                            const DDEnabledState* ddEnabledState) {
	state Reference<InitialDataDistribution> result = makeReference<InitialDataDistribution>();
	state Key beginKey = allKeys.begin;

	state bool succeeded;

	state Transaction tr(cx);

	state std::map<UID, Optional<Key>> server_dc;
	state std::map<std::vector<UID>, std::pair<std::vector<UID>, std::vector<UID>>> team_cache;
	state std::vector<std::pair<StorageServerInterface, ProcessClass>> tss_servers;

	// Get the server list in its own try/catch block since it modifies result.  We don't want a subsequent failure
	// causing entries to be duplicated
	loop {
		server_dc.clear();
		succeeded = false;
		try {

			// Read healthyZone value which is later used to determine on/off of failure triggered DD
			tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::READ_LOCK_AWARE);
			Optional<Value> val = wait(tr.get(healthyZoneKey));
			if (val.present()) {
				auto p = decodeHealthyZoneValue(val.get());
				if (p.second > tr.getReadVersion().get() || p.first == ignoreSSFailuresZoneString) {
					result->initHealthyZoneValue = Optional<Key>(p.first);
				} else {
					result->initHealthyZoneValue = Optional<Key>();
				}
			} else {
				result->initHealthyZoneValue = Optional<Key>();
			}

			result->mode = 1;
			tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
			Optional<Value> mode = wait(tr.get(dataDistributionModeKey));
			if (mode.present()) {
				BinaryReader rd(mode.get(), Unversioned());
				rd >> result->mode;
			}
			if (!result->mode || !ddEnabledState->isDDEnabled()) {
				// DD can be disabled persistently (result->mode = 0) or transiently (isDDEnabled() = 0)
				TraceEvent(SevDebug, "GetInitialDataDistribution_DisabledDD").log();
				return result;
			}

			state Future<std::vector<ProcessData>> workers = getWorkers(&tr);
			state Future<RangeResult> serverList = tr.getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY);
			wait(success(workers) && success(serverList));
			ASSERT(!serverList.get().more && serverList.get().size() < CLIENT_KNOBS->TOO_MANY);

			std::map<Optional<Standalone<StringRef>>, ProcessData> id_data;
			for (int i = 0; i < workers.get().size(); i++)
				id_data[workers.get()[i].locality.processId()] = workers.get()[i];

			succeeded = true;

			for (int i = 0; i < serverList.get().size(); i++) {
				auto ssi = decodeServerListValue(serverList.get()[i].value);
				if (!ssi.isTss()) {
					result->allServers.emplace_back(ssi, id_data[ssi.locality.processId()].processClass);
					server_dc[ssi.id()] = ssi.locality.dcId();
				} else {
					tss_servers.emplace_back(ssi, id_data[ssi.locality.processId()].processClass);
				}
			}

			break;
		} catch (Error& e) {
			wait(tr.onError(e));

			ASSERT(!succeeded); // We shouldn't be retrying if we have already started modifying result in this loop
			TraceEvent("GetInitialTeamsRetry", distributorId).log();
		}
	}

	// If keyServers is too large to read in a single transaction, then we will have to break this process up into
	// multiple transactions. In that case, each iteration should begin where the previous left off
	while (beginKey < allKeys.end) {
		TEST(beginKey > allKeys.begin); // Multi-transactional getInitialDataDistribution
		loop {
			succeeded = false;
			try {
				tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
				wait(checkMoveKeysLockReadOnly(&tr, moveKeysLock, ddEnabledState));
				state RangeResult UIDtoTagMap = wait(tr.getRange(serverTagKeys, CLIENT_KNOBS->TOO_MANY));
				ASSERT(!UIDtoTagMap.more && UIDtoTagMap.size() < CLIENT_KNOBS->TOO_MANY);
				RangeResult keyServers = wait(krmGetRanges(&tr,
				                                           keyServersPrefix,
				                                           KeyRangeRef(beginKey, allKeys.end),
				                                           SERVER_KNOBS->MOVE_KEYS_KRM_LIMIT,
				                                           SERVER_KNOBS->MOVE_KEYS_KRM_LIMIT_BYTES));
				succeeded = true;

				std::vector<UID> src, dest, last;

				// for each range
				for (int i = 0; i < keyServers.size() - 1; i++) {
					DDShardInfo info(keyServers[i].key);
					decodeKeyServersValue(UIDtoTagMap, keyServers[i].value, src, dest);
					if (remoteDcIds.size()) {
						auto srcIter = team_cache.find(src);
						if (srcIter == team_cache.end()) {
							for (auto& id : src) {
								auto& dc = server_dc[id];
								if (std::find(remoteDcIds.begin(), remoteDcIds.end(), dc) != remoteDcIds.end()) {
									info.remoteSrc.push_back(id);
								} else {
									info.primarySrc.push_back(id);
								}
							}
							result->primaryTeams.insert(info.primarySrc);
							result->remoteTeams.insert(info.remoteSrc);
							team_cache[src] = std::make_pair(info.primarySrc, info.remoteSrc);
						} else {
							info.primarySrc = srcIter->second.first;
							info.remoteSrc = srcIter->second.second;
						}
						if (dest.size()) {
							info.hasDest = true;
							auto destIter = team_cache.find(dest);
							if (destIter == team_cache.end()) {
								for (auto& id : dest) {
									auto& dc = server_dc[id];
									if (std::find(remoteDcIds.begin(), remoteDcIds.end(), dc) != remoteDcIds.end()) {
										info.remoteDest.push_back(id);
									} else {
										info.primaryDest.push_back(id);
									}
								}
								result->primaryTeams.insert(info.primaryDest);
								result->remoteTeams.insert(info.remoteDest);
								team_cache[dest] = std::make_pair(info.primaryDest, info.remoteDest);
							} else {
								info.primaryDest = destIter->second.first;
								info.remoteDest = destIter->second.second;
							}
						}
					} else {
						info.primarySrc = src;
						auto srcIter = team_cache.find(src);
						if (srcIter == team_cache.end()) {
							result->primaryTeams.insert(src);
							team_cache[src] = std::pair<std::vector<UID>, std::vector<UID>>();
						}
						if (dest.size()) {
							info.hasDest = true;
							info.primaryDest = dest;
							auto destIter = team_cache.find(dest);
							if (destIter == team_cache.end()) {
								result->primaryTeams.insert(dest);
								team_cache[dest] = std::pair<std::vector<UID>, std::vector<UID>>();
							}
						}
					}
					result->shards.push_back(info);
				}

				ASSERT_GT(keyServers.size(), 0);
				beginKey = keyServers.end()[-1].key;
				break;
			} catch (Error& e) {
				TraceEvent("GetInitialTeamsKeyServersRetry", distributorId).error(e);

				wait(tr.onError(e));
				ASSERT(!succeeded); // We shouldn't be retrying if we have already started modifying result in this loop
			}
		}

		tr.reset();
	}

	// a dummy shard at the end with no keys or servers makes life easier for trackInitialShards()
	result->shards.push_back(DDShardInfo(allKeys.end));

	// add tss to server list AFTER teams are built
	for (auto& it : tss_servers) {
		result->allServers.push_back(it);
	}

	return result;
}

// add server to wiggling queue
void StorageWiggler::addServer(const UID& serverId, const StorageMetadataType& metadata) {
	// std::cout << "size: " << pq_handles.size() << " add " << serverId.toString() << " DC: "<< teamCollection->primary
	// << std::endl;
	ASSERT(!pq_handles.count(serverId));
	pq_handles[serverId] = wiggle_pq.emplace(metadata, serverId);
	nonEmpty.set(true);
}

void StorageWiggler::removeServer(const UID& serverId) {
	// std::cout << "size: " << pq_handles.size() << " remove " << serverId.toString() << " DC: "<<
	// teamCollection->primary <<std::endl;
	if (contains(serverId)) { // server haven't been popped
		auto handle = pq_handles.at(serverId);
		pq_handles.erase(serverId);
		wiggle_pq.erase(handle);
	}
	nonEmpty.set(!wiggle_pq.empty());
}

void StorageWiggler::updateMetadata(const UID& serverId, const StorageMetadataType& metadata) {
	//	std::cout << "size: " << pq_handles.size() << " update " << serverId.toString()
	//	          << " DC: " << teamCollection->primary << std::endl;
	auto handle = pq_handles.at(serverId);
	if ((*handle).first.createdTime == metadata.createdTime) {
		return;
	}
	wiggle_pq.update(handle, std::make_pair(metadata, serverId));
}

Optional<UID> StorageWiggler::getNextServerId() {
	if (!wiggle_pq.empty()) {
		auto [metadata, id] = wiggle_pq.top();
		wiggle_pq.pop();
		pq_handles.erase(id);
		return Optional<UID>(id);
	}
	return Optional<UID>();
}

Future<Void> StorageWiggler::resetStats() {
	auto newMetrics = StorageWiggleMetrics();
	newMetrics.smoothed_round_duration = metrics.smoothed_round_duration;
	newMetrics.smoothed_wiggle_duration = metrics.smoothed_wiggle_duration;
	return StorageWiggleMetrics::runSetTransaction(teamCollection->cx, teamCollection->primary, newMetrics);
}

Future<Void> StorageWiggler::restoreStats() {
	auto& metricsRef = metrics;
	auto assignFunc = [&metricsRef](Optional<Value> v) {
		if (v.present()) {
			metricsRef = BinaryReader::fromStringRef<StorageWiggleMetrics>(v.get(), IncludeVersion());
		}
		return Void();
	};
	auto readFuture = StorageWiggleMetrics::runGetTransaction(teamCollection->cx, teamCollection->primary);
	return map(readFuture, assignFunc);
}
Future<Void> StorageWiggler::startWiggle() {
	metrics.last_wiggle_start = timer_int();
	if (shouldStartNewRound()) {
		metrics.last_round_start = metrics.last_wiggle_start;
	}
	return StorageWiggleMetrics::runSetTransaction(teamCollection->cx, teamCollection->primary, metrics);
}

Future<Void> StorageWiggler::finishWiggle() {
	metrics.last_wiggle_finish = timer_int();
	metrics.finished_wiggle += 1;
	auto duration = metrics.last_wiggle_finish - metrics.last_wiggle_start;
	metrics.smoothed_wiggle_duration.setTotal((double)duration);

	if (shouldFinishRound()) {
		metrics.last_round_finish = metrics.last_wiggle_finish;
		metrics.finished_round += 1;
		duration = metrics.last_round_finish - metrics.last_round_start;
		metrics.smoothed_round_duration.setTotal((double)duration);
	}
	return StorageWiggleMetrics::runSetTransaction(teamCollection->cx, teamCollection->primary, metrics);
}

// Take a snapshot of necessary data structures from `DDTeamCollection` and print them out with yields to avoid slow
// task on the run loop.
ACTOR Future<Void> printSnapshotTeamsInfo(Reference<DDTeamCollection> self) {
	state DatabaseConfiguration configuration;
	state std::map<UID, Reference<TCServerInfo>> server_info;
	state std::map<UID, ServerStatus> server_status;
	state std::vector<Reference<TCTeamInfo>> teams;
	state std::map<Standalone<StringRef>, Reference<TCMachineInfo>> machine_info;
	state std::vector<Reference<TCMachineTeamInfo>> machineTeams;
	// state std::vector<std::string> internedLocalityRecordKeyNameStrings;
	// state int machineLocalityMapEntryArraySize;
	// state std::vector<Reference<LocalityRecord>> machineLocalityMapRecordArray;
	state int traceEventsPrinted = 0;
	state std::vector<const UID*> serverIDs;
	state double lastPrintTime = 0;
	state ReadYourWritesTransaction tr(self->cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			state Future<Void> watchFuture = tr.watch(triggerDDTeamInfoPrintKey);
			wait(tr.commit());
			wait(self->printDetailedTeamsInfo.onTrigger() || watchFuture);
			tr.reset();
			if (now() - lastPrintTime < SERVER_KNOBS->DD_TEAMS_INFO_PRINT_INTERVAL) {
				continue;
			}
			lastPrintTime = now();

			traceEventsPrinted = 0;

			double snapshotStart = now();

			configuration = self->configuration;
			server_info = self->server_info;
			teams = self->teams;
			// Perform deep copy so we have a consistent snapshot, even if yields are performed
			for (const auto& [machineId, info] : self->machine_info) {
				machine_info.emplace(machineId, info->clone());
			}
			machineTeams = self->machineTeams;
			// internedLocalityRecordKeyNameStrings = self->machineLocalityMap._keymap->_lookuparray;
			// machineLocalityMapEntryArraySize = self->machineLocalityMap.size();
			// machineLocalityMapRecordArray = self->machineLocalityMap.getRecordArray();
			std::vector<const UID*> _uids = self->machineLocalityMap.getObjects();
			serverIDs = _uids;

			auto const& keys = self->server_status.getKeys();
			for (auto const& key : keys) {
				// Add to or update the local server_status map
				server_status[key] = self->server_status.get(key);
			}

			TraceEvent("DDPrintSnapshotTeasmInfo", self->distributorId)
			    .detail("SnapshotSpeed", now() - snapshotStart)
			    .detail("Primary", self->primary);

			// Print to TraceEvents
			TraceEvent("DDConfig", self->distributorId)
			    .detail("StorageTeamSize", configuration.storageTeamSize)
			    .detail("DesiredTeamsPerServer", SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER)
			    .detail("MaxTeamsPerServer", SERVER_KNOBS->MAX_TEAMS_PER_SERVER)
			    .detail("Primary", self->primary);

			TraceEvent("ServerInfo", self->distributorId)
			    .detail("Size", server_info.size())
			    .detail("Primary", self->primary);
			state int i;
			state std::map<UID, Reference<TCServerInfo>>::iterator server = server_info.begin();
			for (i = 0; i < server_info.size(); i++) {
				TraceEvent("ServerInfo", self->distributorId)
				    .detail("ServerInfoIndex", i)
				    .detail("ServerID", server->first.toString())
				    .detail("ServerTeamOwned", server->second->teams.size())
				    .detail("MachineID", server->second->machine->machineID.contents().toString())
				    .detail("Primary", self->primary);
				server++;
				if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
					wait(yield());
				}
			}

			server = server_info.begin();
			for (i = 0; i < server_info.size(); i++) {
				const UID& uid = server->first;

				TraceEvent e("ServerStatus", self->distributorId);
				e.detail("ServerUID", uid)
				    .detail("MachineIsValid", server_info[uid]->machine.isValid())
				    .detail("MachineTeamSize",
				            server_info[uid]->machine.isValid() ? server_info[uid]->machine->machineTeams.size() : -1)
				    .detail("Primary", self->primary);

				// ServerStatus might not be known if server was very recently added and storageServerFailureTracker()
				// has not yet updated self->server_status
				// If the UID is not found, do not assume the server is healthy or unhealthy
				auto it = server_status.find(uid);
				if (it != server_status.end()) {
					e.detail("Healthy", !it->second.isUnhealthy());
				}

				server++;
				if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
					wait(yield());
				}
			}

			TraceEvent("ServerTeamInfo", self->distributorId)
			    .detail("Size", teams.size())
			    .detail("Primary", self->primary);
			for (i = 0; i < teams.size(); i++) {
				const auto& team = teams[i];
				TraceEvent("ServerTeamInfo", self->distributorId)
				    .detail("TeamIndex", i)
				    .detail("Healthy", team->isHealthy())
				    .detail("TeamSize", team->size())
				    .detail("MemberIDs", team->getServerIDsStr())
				    .detail("Primary", self->primary);
				if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
					wait(yield());
				}
			}

			TraceEvent("MachineInfo", self->distributorId)
			    .detail("Size", machine_info.size())
			    .detail("Primary", self->primary);
			state std::map<Standalone<StringRef>, Reference<TCMachineInfo>>::iterator machine = machine_info.begin();
			state bool isMachineHealthy = false;
			for (i = 0; i < machine_info.size(); i++) {
				Reference<TCMachineInfo> _machine = machine->second;
				if (!_machine.isValid() || machine_info.find(_machine->machineID) == machine_info.end() ||
				    _machine->serversOnMachine.empty()) {
					isMachineHealthy = false;
				}

				// Healthy machine has at least one healthy server
				for (auto& server : _machine->serversOnMachine) {
					// ServerStatus might not be known if server was very recently added and
					// storageServerFailureTracker() has not yet updated self->server_status If the UID is not found, do
					// not assume the server is healthy
					auto it = server_status.find(server->id);
					if (it != server_status.end() && !it->second.isUnhealthy()) {
						isMachineHealthy = true;
					}
				}

				isMachineHealthy = false;
				TraceEvent("MachineInfo", self->distributorId)
				    .detail("MachineInfoIndex", i)
				    .detail("Healthy", isMachineHealthy)
				    .detail("MachineID", machine->first.contents().toString())
				    .detail("MachineTeamOwned", machine->second->machineTeams.size())
				    .detail("ServerNumOnMachine", machine->second->serversOnMachine.size())
				    .detail("ServersID", machine->second->getServersIDStr())
				    .detail("Primary", self->primary);
				machine++;
				if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
					wait(yield());
				}
			}

			TraceEvent("MachineTeamInfo", self->distributorId)
			    .detail("Size", machineTeams.size())
			    .detail("Primary", self->primary);
			for (i = 0; i < machineTeams.size(); i++) {
				const auto& team = machineTeams[i];
				TraceEvent("MachineTeamInfo", self->distributorId)
				    .detail("TeamIndex", i)
				    .detail("MachineIDs", team->getMachineIDsStr())
				    .detail("ServerTeams", team->serverTeams.size())
				    .detail("Primary", self->primary);
				if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
					wait(yield());
				}
			}

			// TODO: re-enable the following logging or remove them.
			// TraceEvent("LocalityRecordKeyName", self->distributorId)
			//     .detail("Size", internedLocalityRecordKeyNameStrings.size())
			//     .detail("Primary", self->primary);
			// for (i = 0; i < internedLocalityRecordKeyNameStrings.size(); i++) {
			// 	TraceEvent("LocalityRecordKeyIndexName", self->distributorId)
			// 	    .detail("KeyIndex", i)
			// 	    .detail("KeyName", internedLocalityRecordKeyNameStrings[i])
			// 	    .detail("Primary", self->primary);
			// 	if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
			// 		wait(yield());
			// 	}
			// }

			// TraceEvent("MachineLocalityMap", self->distributorId)
			//     .detail("Size", machineLocalityMapEntryArraySize)
			//     .detail("Primary", self->primary);
			// for (i = 0; i < serverIDs.size(); i++) {
			// 	const auto& serverID = serverIDs[i];
			// 	Reference<LocalityRecord> record = machineLocalityMapRecordArray[i];
			// 	if (record.isValid()) {
			// 		TraceEvent("MachineLocalityMap", self->distributorId)
			// 		    .detail("LocalityIndex", i)
			// 		    .detail("UID", serverID->toString())
			// 		    .detail("LocalityRecord", record->toString())
			// 		    .detail("Primary", self->primary);
			// 	} else {
			// 		TraceEvent("MachineLocalityMap", self->distributorId)
			// 		    .detail("LocalityIndex", i)
			// 		    .detail("UID", serverID->toString())
			// 		    .detail("LocalityRecord", "[NotFound]")
			// 		    .detail("Primary", self->primary);
			// 	}
			// 	if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
			// 		wait(yield());
			// 	}
			// }
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

ACTOR Future<std::vector<std::pair<StorageServerInterface, ProcessClass>>> getServerListAndProcessClasses(
    Transaction* tr) {
	state Future<std::vector<ProcessData>> workers = getWorkers(tr);
	state Future<RangeResult> serverList = tr->getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY);
	wait(success(workers) && success(serverList));
	ASSERT(!serverList.get().more && serverList.get().size() < CLIENT_KNOBS->TOO_MANY);

	std::map<Optional<Standalone<StringRef>>, ProcessData> id_data;
	for (int i = 0; i < workers.get().size(); i++)
		id_data[workers.get()[i].locality.processId()] = workers.get()[i];

	std::vector<std::pair<StorageServerInterface, ProcessClass>> results;
	for (int i = 0; i < serverList.get().size(); i++) {
		auto ssi = decodeServerListValue(serverList.get()[i].value);
		results.emplace_back(ssi, id_data[ssi.locality.processId()].processClass);
	}

	return results;
}

ACTOR Future<Void> remoteRecovered(Reference<AsyncVar<ServerDBInfo> const> db) {
	TraceEvent("DDTrackerStarting").log();
	while (db->get().recoveryState < RecoveryState::ALL_LOGS_RECRUITED) {
		TraceEvent("DDTrackerStarting").detail("RecoveryState", (int)db->get().recoveryState);
		wait(db->onChange());
	}
	return Void();
}

// Keep track of servers and teams -- serves requests for getRandomTeam
ACTOR Future<Void> dataDistributionTeamCollection(
    Reference<DDTeamCollection> teamCollection,
    Reference<InitialDataDistribution> initData,
    TeamCollectionInterface tci,
    Reference<IAsyncListener<RequestStream<RecruitStorageRequest>>> recruitStorage,
    DDEnabledState const* ddEnabledState) {
	state DDTeamCollection* self = teamCollection.getPtr();
	state Future<Void> loggingTrigger = Void();
	state PromiseStream<Void> serverRemoved;
	state Future<Void> error = actorCollection(self->addActor.getFuture());

	try {
		wait(self->init(initData, ddEnabledState));
		initData = Reference<InitialDataDistribution>();
		self->addActor.send(self->serverGetTeamRequests(tci));

		TraceEvent("DDTeamCollectionBegin", self->distributorId).detail("Primary", self->primary);
		wait(self->readyToStart || error);
		TraceEvent("DDTeamCollectionReadyToStart", self->distributorId).detail("Primary", self->primary);

		// removeBadTeams() does not always run. We may need to restart the actor when needed.
		// So we need the badTeamRemover variable to check if the actor is ready.
		if (self->badTeamRemover.isReady()) {
			self->badTeamRemover = self->removeBadTeams();
			self->addActor.send(self->badTeamRemover);
		}

		self->addActor.send(self->machineTeamRemover());
		self->addActor.send(self->serverTeamRemover());

		if (self->wrongStoreTypeRemover.isReady()) {
			self->wrongStoreTypeRemover = self->removeWrongStoreType();
			self->addActor.send(self->wrongStoreTypeRemover);
		}

		self->traceTeamCollectionInfo();

		if (self->includedDCs.size()) {
			// start this actor before any potential recruitments can happen
			self->addActor.send(self->updateReplicasKey(self->includedDCs[0]));
		}

		// The following actors (e.g. storageRecruiter) do not need to be assigned to a variable because
		// they are always running.
		self->addActor.send(self->storageRecruiter(recruitStorage, ddEnabledState));
		self->addActor.send(self->monitorStorageServerRecruitment());
		self->addActor.send(self->waitServerListChange(serverRemoved.getFuture(), ddEnabledState));
		self->addActor.send(self->trackExcludedServers());
		self->addActor.send(self->monitorHealthyTeams());
		self->addActor.send(self->waitHealthyZoneChange());
		self->addActor.send(self->monitorPerpetualStorageWiggle());
		// SOMEDAY: Monitor FF/serverList for (new) servers that aren't in allServers and add or remove them

		loop choose {
			when(UID removedServer = waitNext(self->removedServers.getFuture())) {
				TEST(true); // Storage server removed from database
				self->removeServer(removedServer);
				serverRemoved.send(Void());

				self->restartRecruiting.trigger();
			}
			when(UID removedTSS = waitNext(self->removedTSS.getFuture())) {
				TEST(true); // TSS removed from database
				self->removeTSS(removedTSS);
				serverRemoved.send(Void());

				self->restartRecruiting.trigger();
			}
			when(wait(self->zeroHealthyTeams->onChange())) {
				if (self->zeroHealthyTeams->get()) {
					self->restartRecruiting.trigger();
					self->noHealthyTeams();
				}
			}
			when(wait(loggingTrigger)) {
				int highestPriority = 0;
				for (auto it : self->priority_teams) {
					if (it.second > 0) {
						highestPriority = std::max(highestPriority, it.first);
					}
				}

				TraceEvent("TotalDataInFlight", self->distributorId)
				    .detail("Primary", self->primary)
				    .detail("TotalBytes", self->getDebugTotalDataInFlight())
				    .detail("UnhealthyServers", self->unhealthyServers)
				    .detail("ServerCount", self->server_info.size())
				    .detail("StorageTeamSize", self->configuration.storageTeamSize)
				    .detail("HighestPriority", highestPriority)
				    .trackLatest(
				        self->primary
				            ? "TotalDataInFlight"
				            : "TotalDataInFlightRemote"); // This trace event's trackLatest lifetime is controlled by
				                                          // DataDistributorData::totalDataInFlightEventHolder or
				                                          // DataDistributorData::totalDataInFlightRemoteEventHolder.
				                                          // The track latest key we use here must match the key used in
				                                          // the holder.

				loggingTrigger = delay(SERVER_KNOBS->DATA_DISTRIBUTION_LOGGING_INTERVAL, TaskPriority::FlushTrace);
			}
			when(wait(self->serverTrackerErrorOut.getFuture())) {} // Propagate errors from storageServerTracker
			when(wait(error)) {}
		}
	} catch (Error& e) {
		if (e.code() != error_code_movekeys_conflict)
			TraceEvent(SevError, "DataDistributionTeamCollectionError", self->distributorId).error(e);
		throw e;
	}
}

ACTOR Future<Void> waitForDataDistributionEnabled(Database cx, const DDEnabledState* ddEnabledState) {
	state Transaction tr(cx);
	loop {
		wait(delay(SERVER_KNOBS->DD_ENABLED_CHECK_DELAY, TaskPriority::DataDistribution));

		try {
			Optional<Value> mode = wait(tr.get(dataDistributionModeKey));
			if (!mode.present() && ddEnabledState->isDDEnabled()) {
				TraceEvent("WaitForDDEnabledSucceeded").log();
				return Void();
			}
			if (mode.present()) {
				BinaryReader rd(mode.get(), Unversioned());
				int m;
				rd >> m;
				TraceEvent(SevDebug, "WaitForDDEnabled")
				    .detail("Mode", m)
				    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
				if (m && ddEnabledState->isDDEnabled()) {
					TraceEvent("WaitForDDEnabledSucceeded").log();
					return Void();
				}
			}

			tr.reset();
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

ACTOR Future<bool> isDataDistributionEnabled(Database cx, const DDEnabledState* ddEnabledState) {
	state Transaction tr(cx);
	loop {
		try {
			Optional<Value> mode = wait(tr.get(dataDistributionModeKey));
			if (!mode.present() && ddEnabledState->isDDEnabled())
				return true;
			if (mode.present()) {
				BinaryReader rd(mode.get(), Unversioned());
				int m;
				rd >> m;
				if (m && ddEnabledState->isDDEnabled()) {
					TraceEvent(SevDebug, "IsDDEnabledSucceeded")
					    .detail("Mode", m)
					    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
					return true;
				}
			}
			// SOMEDAY: Write a wrapper in MoveKeys.actor.h
			Optional<Value> readVal = wait(tr.get(moveKeysLockOwnerKey));
			UID currentOwner =
			    readVal.present() ? BinaryReader::fromStringRef<UID>(readVal.get(), Unversioned()) : UID();
			if (ddEnabledState->isDDEnabled() && (currentOwner != dataDistributionModeLock)) {
				TraceEvent(SevDebug, "IsDDEnabledSucceeded")
				    .detail("CurrentOwner", currentOwner)
				    .detail("DDModeLock", dataDistributionModeLock)
				    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
				return true;
			}
			TraceEvent(SevDebug, "IsDDEnabledFailed")
			    .detail("CurrentOwner", currentOwner)
			    .detail("DDModeLock", dataDistributionModeLock)
			    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
			return false;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

// Ensures that the serverKeys key space is properly coalesced
// This method is only used for testing and is not implemented in a manner that is safe for large databases
ACTOR Future<Void> debugCheckCoalescing(Database cx) {
	state Transaction tr(cx);
	loop {
		try {
			state RangeResult serverList = wait(tr.getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY));
			ASSERT(!serverList.more && serverList.size() < CLIENT_KNOBS->TOO_MANY);

			state int i;
			for (i = 0; i < serverList.size(); i++) {
				state UID id = decodeServerListValue(serverList[i].value).id();
				RangeResult ranges = wait(krmGetRanges(&tr, serverKeysPrefixFor(id), allKeys));
				ASSERT(ranges.end()[-1].key == allKeys.end);

				for (int j = 0; j < ranges.size() - 2; j++)
					if (ranges[j].value == ranges[j + 1].value)
						TraceEvent(SevError, "UncoalescedValues", id)
						    .detail("Key1", ranges[j].key)
						    .detail("Key2", ranges[j + 1].key)
						    .detail("Value", ranges[j].value);
			}

			TraceEvent("DoneCheckingCoalescing").log();
			return Void();
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

static std::set<int> const& normalDDQueueErrors() {
	static std::set<int> s;
	if (s.empty()) {
		s.insert(error_code_movekeys_conflict);
		s.insert(error_code_broken_promise);
	}
	return s;
}

ACTOR Future<Void> pollMoveKeysLock(Database cx, MoveKeysLock lock, const DDEnabledState* ddEnabledState) {
	loop {
		wait(delay(SERVER_KNOBS->MOVEKEYS_LOCK_POLLING_DELAY));
		state Transaction tr(cx);
		loop {
			try {
				wait(checkMoveKeysLockReadOnly(&tr, lock, ddEnabledState));
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	}
}

struct DataDistributorData : NonCopyable, ReferenceCounted<DataDistributorData> {
	Reference<AsyncVar<ServerDBInfo> const> dbInfo;
	UID ddId;
	PromiseStream<Future<Void>> addActor;
	DDTeamCollection* teamCollection;
	Reference<EventCacheHolder> initialDDEventHolder;
	Reference<EventCacheHolder> movingDataEventHolder;
	Reference<EventCacheHolder> totalDataInFlightEventHolder;
	Reference<EventCacheHolder> totalDataInFlightRemoteEventHolder;

	DataDistributorData(Reference<AsyncVar<ServerDBInfo> const> const& db, UID id)
	  : dbInfo(db), ddId(id), teamCollection(nullptr),
	    initialDDEventHolder(makeReference<EventCacheHolder>("InitialDD")),
	    movingDataEventHolder(makeReference<EventCacheHolder>("MovingData")),
	    totalDataInFlightEventHolder(makeReference<EventCacheHolder>("TotalDataInFlight")),
	    totalDataInFlightRemoteEventHolder(makeReference<EventCacheHolder>("TotalDataInFlightRemote")) {}
};

ACTOR Future<Void> monitorBatchLimitedTime(Reference<AsyncVar<ServerDBInfo> const> db, double* lastLimited) {
	loop {
		wait(delay(SERVER_KNOBS->METRIC_UPDATE_RATE));

		state Reference<GrvProxyInfo> grvProxies(new GrvProxyInfo(db->get().client.grvProxies, false));

		choose {
			when(wait(db->onChange())) {}
			when(GetHealthMetricsReply reply =
			         wait(grvProxies->size() ? basicLoadBalance(grvProxies,
			                                                    &GrvProxyInterface::getHealthMetrics,
			                                                    GetHealthMetricsRequest(false))
			                                 : Never())) {
				if (reply.healthMetrics.batchLimited) {
					*lastLimited = now();
				}
			}
		}
	}
}

// Runs the data distribution algorithm for FDB, including the DD Queue, DD tracker, and DD team collection
ACTOR Future<Void> dataDistribution(Reference<DataDistributorData> self,
                                    PromiseStream<GetMetricsListRequest> getShardMetricsList,
                                    const DDEnabledState* ddEnabledState) {
	state double lastLimited = 0;
	self->addActor.send(monitorBatchLimitedTime(self->dbInfo, &lastLimited));

	state Database cx = openDBOnServer(self->dbInfo, TaskPriority::DataDistributionLaunch, LockAware::True);
	cx->locationCacheSize = SERVER_KNOBS->DD_LOCATION_CACHE_SIZE;

	// cx->setOption( FDBDatabaseOptions::LOCATION_CACHE_SIZE, StringRef((uint8_t*)
	// &SERVER_KNOBS->DD_LOCATION_CACHE_SIZE, 8) ); ASSERT( cx->locationCacheSize ==
	// SERVER_KNOBS->DD_LOCATION_CACHE_SIZE
	// );

	// wait(debugCheckCoalescing(cx));
	state std::vector<Optional<Key>> primaryDcId;
	state std::vector<Optional<Key>> remoteDcIds;
	state DatabaseConfiguration configuration;
	state Reference<InitialDataDistribution> initData;
	state MoveKeysLock lock;
	state Reference<DDTeamCollection> primaryTeamCollection;
	state Reference<DDTeamCollection> remoteTeamCollection;
	state bool trackerCancelled;
	loop {
		trackerCancelled = false;

		// Stored outside of data distribution tracker to avoid slow tasks
		// when tracker is cancelled
		state KeyRangeMap<ShardTrackedData> shards;
		state Promise<UID> removeFailedServer;
		try {
			loop {
				TraceEvent("DDInitTakingMoveKeysLock", self->ddId).log();
				MoveKeysLock lock_ = wait(takeMoveKeysLock(cx, self->ddId));
				lock = lock_;
				TraceEvent("DDInitTookMoveKeysLock", self->ddId).log();

				DatabaseConfiguration configuration_ = wait(getDatabaseConfiguration(cx));
				configuration = configuration_;
				primaryDcId.clear();
				remoteDcIds.clear();
				const std::vector<RegionInfo>& regions = configuration.regions;
				if (configuration.regions.size() > 0) {
					primaryDcId.push_back(regions[0].dcId);
				}
				if (configuration.regions.size() > 1) {
					remoteDcIds.push_back(regions[1].dcId);
				}

				TraceEvent("DDInitGotConfiguration", self->ddId).detail("Conf", configuration.toString());

				state Transaction tr(cx);
				loop {
					try {
						tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
						tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

						RangeResult replicaKeys = wait(tr.getRange(datacenterReplicasKeys, CLIENT_KNOBS->TOO_MANY));

						for (auto& kv : replicaKeys) {
							auto dcId = decodeDatacenterReplicasKey(kv.key);
							auto replicas = decodeDatacenterReplicasValue(kv.value);
							if ((primaryDcId.size() && primaryDcId[0] == dcId) ||
							    (remoteDcIds.size() && remoteDcIds[0] == dcId && configuration.usableRegions > 1)) {
								if (replicas > configuration.storageTeamSize) {
									tr.set(kv.key, datacenterReplicasValue(configuration.storageTeamSize));
								}
							} else {
								tr.clear(kv.key);
							}
						}

						wait(tr.commit());
						break;
					} catch (Error& e) {
						wait(tr.onError(e));
					}
				}

				TraceEvent("DDInitUpdatedReplicaKeys", self->ddId).log();
				Reference<InitialDataDistribution> initData_ = wait(getInitialDataDistribution(
				    cx,
				    self->ddId,
				    lock,
				    configuration.usableRegions > 1 ? remoteDcIds : std::vector<Optional<Key>>(),
				    ddEnabledState));
				initData = initData_;
				if (initData->shards.size() > 1) {
					TraceEvent("DDInitGotInitialDD", self->ddId)
					    .detail("B", initData->shards.end()[-2].key)
					    .detail("E", initData->shards.end()[-1].key)
					    .detail("Src", describe(initData->shards.end()[-2].primarySrc))
					    .detail("Dest", describe(initData->shards.end()[-2].primaryDest))
					    .trackLatest(self->initialDDEventHolder->trackingKey);
				} else {
					TraceEvent("DDInitGotInitialDD", self->ddId)
					    .detail("B", "")
					    .detail("E", "")
					    .detail("Src", "[no items]")
					    .detail("Dest", "[no items]")
					    .trackLatest(self->initialDDEventHolder->trackingKey);
				}

				if (initData->mode && ddEnabledState->isDDEnabled()) {
					// mode may be set true by system operator using fdbcli and isDDEnabled() set to true
					break;
				}
				TraceEvent("DataDistributionDisabled", self->ddId).log();

				TraceEvent("MovingData", self->ddId)
				    .detail("InFlight", 0)
				    .detail("InQueue", 0)
				    .detail("AverageShardSize", -1)
				    .detail("UnhealthyRelocations", 0)
				    .detail("HighestPriority", 0)
				    .detail("BytesWritten", 0)
				    .detail("PriorityRecoverMove", 0)
				    .detail("PriorityRebalanceUnderutilizedTeam", 0)
				    .detail("PriorityRebalannceOverutilizedTeam", 0)
				    .detail("PriorityTeamHealthy", 0)
				    .detail("PriorityTeamContainsUndesiredServer", 0)
				    .detail("PriorityTeamRedundant", 0)
				    .detail("PriorityMergeShard", 0)
				    .detail("PriorityTeamUnhealthy", 0)
				    .detail("PriorityTeam2Left", 0)
				    .detail("PriorityTeam1Left", 0)
				    .detail("PriorityTeam0Left", 0)
				    .detail("PrioritySplitShard", 0)
				    .trackLatest(self->movingDataEventHolder->trackingKey);

				TraceEvent("TotalDataInFlight", self->ddId)
				    .detail("Primary", true)
				    .detail("TotalBytes", 0)
				    .detail("UnhealthyServers", 0)
				    .detail("HighestPriority", 0)
				    .trackLatest(self->totalDataInFlightEventHolder->trackingKey);
				TraceEvent("TotalDataInFlight", self->ddId)
				    .detail("Primary", false)
				    .detail("TotalBytes", 0)
				    .detail("UnhealthyServers", 0)
				    .detail("HighestPriority", configuration.usableRegions > 1 ? 0 : -1)
				    .trackLatest(self->totalDataInFlightRemoteEventHolder->trackingKey);

				wait(waitForDataDistributionEnabled(cx, ddEnabledState));
				TraceEvent("DataDistributionEnabled").log();
			}

			// When/If this assertion fails, Evan owes Ben a pat on the back for his foresight
			ASSERT(configuration.storageTeamSize > 0);

			state PromiseStream<RelocateShard> output;
			state PromiseStream<RelocateShard> input;
			state PromiseStream<Promise<int64_t>> getAverageShardBytes;
			state PromiseStream<Promise<int>> getUnhealthyRelocationCount;
			state PromiseStream<GetMetricsRequest> getShardMetrics;
			state Reference<AsyncVar<bool>> processingUnhealthy(new AsyncVar<bool>(false));
			state Reference<AsyncVar<bool>> processingWiggle(new AsyncVar<bool>(false));
			state Promise<Void> readyToStart;
			state Reference<ShardsAffectedByTeamFailure> shardsAffectedByTeamFailure(new ShardsAffectedByTeamFailure);

			state int shard = 0;
			for (; shard < initData->shards.size() - 1; shard++) {
				KeyRangeRef keys = KeyRangeRef(initData->shards[shard].key, initData->shards[shard + 1].key);
				shardsAffectedByTeamFailure->defineShard(keys);
				std::vector<ShardsAffectedByTeamFailure::Team> teams;
				teams.push_back(ShardsAffectedByTeamFailure::Team(initData->shards[shard].primarySrc, true));
				if (configuration.usableRegions > 1) {
					teams.push_back(ShardsAffectedByTeamFailure::Team(initData->shards[shard].remoteSrc, false));
				}
				if (g_network->isSimulated()) {
					TraceEvent("DDInitShard")
					    .detail("Keys", keys)
					    .detail("PrimarySrc", describe(initData->shards[shard].primarySrc))
					    .detail("RemoteSrc", describe(initData->shards[shard].remoteSrc))
					    .detail("PrimaryDest", describe(initData->shards[shard].primaryDest))
					    .detail("RemoteDest", describe(initData->shards[shard].remoteDest));
				}

				shardsAffectedByTeamFailure->moveShard(keys, teams);
				if (initData->shards[shard].hasDest) {
					// This shard is already in flight.  Ideally we should use dest in ShardsAffectedByTeamFailure and
					// generate a dataDistributionRelocator directly in DataDistributionQueue to track it, but it's
					// easier to just (with low priority) schedule it for movement.
					bool unhealthy = initData->shards[shard].primarySrc.size() != configuration.storageTeamSize;
					if (!unhealthy && configuration.usableRegions > 1) {
						unhealthy = initData->shards[shard].remoteSrc.size() != configuration.storageTeamSize;
					}
					output.send(RelocateShard(
					    keys, unhealthy ? SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY : SERVER_KNOBS->PRIORITY_RECOVER_MOVE));
				}
				wait(yield(TaskPriority::DataDistribution));
			}

			std::vector<TeamCollectionInterface> tcis;

			Reference<AsyncVar<bool>> anyZeroHealthyTeams;
			std::vector<Reference<AsyncVar<bool>>> zeroHealthyTeams;
			tcis.push_back(TeamCollectionInterface());
			zeroHealthyTeams.push_back(makeReference<AsyncVar<bool>>(true));
			int storageTeamSize = configuration.storageTeamSize;

			std::vector<Future<Void>> actors;
			if (configuration.usableRegions > 1) {
				tcis.push_back(TeamCollectionInterface());
				storageTeamSize = 2 * configuration.storageTeamSize;

				zeroHealthyTeams.push_back(makeReference<AsyncVar<bool>>(true));
				anyZeroHealthyTeams = makeReference<AsyncVar<bool>>(true);
				actors.push_back(anyTrue(zeroHealthyTeams, anyZeroHealthyTeams));
			} else {
				anyZeroHealthyTeams = zeroHealthyTeams[0];
			}

			actors.push_back(pollMoveKeysLock(cx, lock, ddEnabledState));
			actors.push_back(reportErrorsExcept(dataDistributionTracker(initData,
			                                                            cx,
			                                                            output,
			                                                            shardsAffectedByTeamFailure,
			                                                            getShardMetrics,
			                                                            getShardMetricsList,
			                                                            getAverageShardBytes.getFuture(),
			                                                            readyToStart,
			                                                            anyZeroHealthyTeams,
			                                                            self->ddId,
			                                                            &shards,
			                                                            &trackerCancelled),
			                                    "DDTracker",
			                                    self->ddId,
			                                    &normalDDQueueErrors()));
			actors.push_back(reportErrorsExcept(dataDistributionQueue(cx,
			                                                          output,
			                                                          input.getFuture(),
			                                                          getShardMetrics,
			                                                          processingUnhealthy,
			                                                          processingWiggle,
			                                                          tcis,
			                                                          shardsAffectedByTeamFailure,
			                                                          lock,
			                                                          getAverageShardBytes,
			                                                          getUnhealthyRelocationCount,
			                                                          self->ddId,
			                                                          storageTeamSize,
			                                                          configuration.storageTeamSize,
			                                                          &lastLimited,
			                                                          ddEnabledState),
			                                    "DDQueue",
			                                    self->ddId,
			                                    &normalDDQueueErrors()));

			std::vector<DDTeamCollection*> teamCollectionsPtrs;
			primaryTeamCollection = makeReference<DDTeamCollection>(
			    cx,
			    self->ddId,
			    lock,
			    output,
			    shardsAffectedByTeamFailure,
			    configuration,
			    primaryDcId,
			    configuration.usableRegions > 1 ? remoteDcIds : std::vector<Optional<Key>>(),
			    readyToStart.getFuture(),
			    zeroHealthyTeams[0],
			    IsPrimary::True,
			    processingUnhealthy,
			    processingWiggle,
			    getShardMetrics,
			    removeFailedServer,
			    getUnhealthyRelocationCount);
			teamCollectionsPtrs.push_back(primaryTeamCollection.getPtr());
			auto recruitStorage = IAsyncListener<RequestStream<RecruitStorageRequest>>::create(
			    self->dbInfo, [](auto const& info) { return info.clusterInterface.recruitStorage; });
			if (configuration.usableRegions > 1) {
				remoteTeamCollection =
				    makeReference<DDTeamCollection>(cx,
				                                    self->ddId,
				                                    lock,
				                                    output,
				                                    shardsAffectedByTeamFailure,
				                                    configuration,
				                                    remoteDcIds,
				                                    Optional<std::vector<Optional<Key>>>(),
				                                    readyToStart.getFuture() && remoteRecovered(self->dbInfo),
				                                    zeroHealthyTeams[1],
				                                    IsPrimary::False,
				                                    processingUnhealthy,
				                                    processingWiggle,
				                                    getShardMetrics,
				                                    removeFailedServer,
				                                    getUnhealthyRelocationCount);
				teamCollectionsPtrs.push_back(remoteTeamCollection.getPtr());
				remoteTeamCollection->teamCollections = teamCollectionsPtrs;
				actors.push_back(
				    reportErrorsExcept(dataDistributionTeamCollection(
				                           remoteTeamCollection, initData, tcis[1], recruitStorage, ddEnabledState),
				                       "DDTeamCollectionSecondary",
				                       self->ddId,
				                       &normalDDQueueErrors()));
				actors.push_back(printSnapshotTeamsInfo(remoteTeamCollection));
			}
			primaryTeamCollection->teamCollections = teamCollectionsPtrs;
			self->teamCollection = primaryTeamCollection.getPtr();
			actors.push_back(
			    reportErrorsExcept(dataDistributionTeamCollection(
			                           primaryTeamCollection, initData, tcis[0], recruitStorage, ddEnabledState),
			                       "DDTeamCollectionPrimary",
			                       self->ddId,
			                       &normalDDQueueErrors()));

			actors.push_back(printSnapshotTeamsInfo(primaryTeamCollection));
			actors.push_back(yieldPromiseStream(output.getFuture(), input));

			wait(waitForAll(actors));
			return Void();
		} catch (Error& e) {
			trackerCancelled = true;
			state Error err = e;
			TraceEvent("DataDistributorDestroyTeamCollections").error(e);
			state std::vector<UID> teamForDroppedRange;
			if (removeFailedServer.getFuture().isReady() && !removeFailedServer.getFuture().isError()) {
				// Choose a random healthy team to host the to-be-dropped range.
				const UID serverID = removeFailedServer.getFuture().get();
				std::vector<UID> pTeam = primaryTeamCollection->getRandomHealthyTeam(serverID);
				teamForDroppedRange.insert(teamForDroppedRange.end(), pTeam.begin(), pTeam.end());
				if (configuration.usableRegions > 1) {
					std::vector<UID> rTeam = remoteTeamCollection->getRandomHealthyTeam(serverID);
					teamForDroppedRange.insert(teamForDroppedRange.end(), rTeam.begin(), rTeam.end());
				}
			}
			self->teamCollection = nullptr;
			primaryTeamCollection = Reference<DDTeamCollection>();
			remoteTeamCollection = Reference<DDTeamCollection>();
			if (err.code() == error_code_actor_cancelled) {
				// When cancelled, we cannot clear asyncronously because
				// this will result in invalid memory access. This should only
				// be an issue in simulation.
				if (!g_network->isSimulated()) {
					TraceEvent(SevWarnAlways, "DataDistributorCancelled");
				}
				shards.clear();
				throw e;
			} else {
				wait(shards.clearAsync());
			}
			TraceEvent("DataDistributorTeamCollectionsDestroyed").error(err);
			if (removeFailedServer.getFuture().isReady() && !removeFailedServer.getFuture().isError()) {
				TraceEvent("RemoveFailedServer", removeFailedServer.getFuture().get()).error(err);
				wait(removeKeysFromFailedServer(
				    cx, removeFailedServer.getFuture().get(), teamForDroppedRange, lock, ddEnabledState));
				Optional<UID> tssPairID;
				wait(removeStorageServer(cx, removeFailedServer.getFuture().get(), tssPairID, lock, ddEnabledState));
			} else {
				if (err.code() != error_code_movekeys_conflict) {
					throw err;
				}

				bool ddEnabled = wait(isDataDistributionEnabled(cx, ddEnabledState));
				TraceEvent("DataDistributionMoveKeysConflict").detail("DataDistributionEnabled", ddEnabled).error(err);
				if (ddEnabled) {
					throw err;
				}
			}
		}
	}
}

static std::set<int> const& normalDataDistributorErrors() {
	static std::set<int> s;
	if (s.empty()) {
		s.insert(error_code_worker_removed);
		s.insert(error_code_broken_promise);
		s.insert(error_code_actor_cancelled);
		s.insert(error_code_please_reboot);
		s.insert(error_code_movekeys_conflict);
	}
	return s;
}

ACTOR template <class Req>
Future<Void> sendSnapReq(RequestStream<Req> stream, Req req, Error e) {
	ErrorOr<REPLY_TYPE(Req)> reply = wait(stream.tryGetReply(req));
	if (reply.isError()) {
		TraceEvent("SnapDataDistributor_ReqError")
		    .error(reply.getError(), true)
		    .detail("ConvertedErrorType", e.what())
		    .detail("Peer", stream.getEndpoint().getPrimaryAddress());
		throw e;
	}
	return Void();
}

ACTOR Future<Void> ddSnapCreateCore(DistributorSnapRequest snapReq, Reference<AsyncVar<ServerDBInfo> const> db) {
	state Database cx = openDBOnServer(db, TaskPriority::DefaultDelay, LockAware::True);
	state ReadYourWritesTransaction tr(cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			TraceEvent("SnapDataDistributor_WriteFlagAttempt")
			    .detail("SnapPayload", snapReq.snapPayload)
			    .detail("SnapUID", snapReq.snapUID);
			tr.set(writeRecoveryKey, writeRecoveryKeyTrue);
			wait(tr.commit());
			break;
		} catch (Error& e) {
			TraceEvent("SnapDataDistributor_WriteFlagError").error(e);
			wait(tr.onError(e));
		}
	}
	TraceEvent("SnapDataDistributor_SnapReqEnter")
	    .detail("SnapPayload", snapReq.snapPayload)
	    .detail("SnapUID", snapReq.snapUID);
	try {
		// disable tlog pop on local tlog nodes
		state std::vector<TLogInterface> tlogs = db->get().logSystemConfig.allLocalLogs(false);
		std::vector<Future<Void>> disablePops;
		disablePops.reserve(tlogs.size());
		for (const auto& tlog : tlogs) {
			disablePops.push_back(sendSnapReq(
			    tlog.disablePopRequest, TLogDisablePopRequest{ snapReq.snapUID }, snap_disable_tlog_pop_failed()));
		}
		wait(waitForAll(disablePops));

		TraceEvent("SnapDataDistributor_AfterDisableTLogPop")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// snap local storage nodes
		std::vector<WorkerInterface> storageWorkers =
		    wait(transformErrors(getStorageWorkers(cx, db, true /* localOnly */), snap_storage_failed()));
		TraceEvent("SnapDataDistributor_GotStorageWorkers")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		std::vector<Future<Void>> storageSnapReqs;
		storageSnapReqs.reserve(storageWorkers.size());
		for (const auto& worker : storageWorkers) {
			storageSnapReqs.push_back(sendSnapReq(worker.workerSnapReq,
			                                      WorkerSnapRequest(snapReq.snapPayload, snapReq.snapUID, "storage"_sr),
			                                      snap_storage_failed()));
		}
		wait(waitForAll(storageSnapReqs));

		TraceEvent("SnapDataDistributor_AfterSnapStorage")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// snap local tlog nodes
		std::vector<Future<Void>> tLogSnapReqs;
		tLogSnapReqs.reserve(tlogs.size());
		for (const auto& tlog : tlogs) {
			tLogSnapReqs.push_back(sendSnapReq(tlog.snapRequest,
			                                   TLogSnapRequest{ snapReq.snapPayload, snapReq.snapUID, "tlog"_sr },
			                                   snap_tlog_failed()));
		}
		wait(waitForAll(tLogSnapReqs));

		TraceEvent("SnapDataDistributor_AfterTLogStorage")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// enable tlog pop on local tlog nodes
		std::vector<Future<Void>> enablePops;
		enablePops.reserve(tlogs.size());
		for (const auto& tlog : tlogs) {
			enablePops.push_back(sendSnapReq(
			    tlog.enablePopRequest, TLogEnablePopRequest{ snapReq.snapUID }, snap_enable_tlog_pop_failed()));
		}
		wait(waitForAll(enablePops));

		TraceEvent("SnapDataDistributor_AfterEnableTLogPops")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// snap the coordinators
		std::vector<WorkerInterface> coordWorkers = wait(getCoordWorkers(cx, db));
		TraceEvent("SnapDataDistributor_GotCoordWorkers")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		std::vector<Future<Void>> coordSnapReqs;
		coordSnapReqs.reserve(coordWorkers.size());
		for (const auto& worker : coordWorkers) {
			coordSnapReqs.push_back(sendSnapReq(worker.workerSnapReq,
			                                    WorkerSnapRequest(snapReq.snapPayload, snapReq.snapUID, "coord"_sr),
			                                    snap_coord_failed()));
		}
		wait(waitForAll(coordSnapReqs));
		TraceEvent("SnapDataDistributor_AfterSnapCoords")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		tr.reset();
		loop {
			try {
				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				TraceEvent("SnapDataDistributor_ClearFlagAttempt")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				tr.clear(writeRecoveryKey);
				wait(tr.commit());
				break;
			} catch (Error& e) {
				TraceEvent("SnapDataDistributor_ClearFlagError").error(e);
				wait(tr.onError(e));
			}
		}
	} catch (Error& err) {
		state Error e = err;
		TraceEvent("SnapDataDistributor_SnapReqExit")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID)
		    .error(e, true /*includeCancelled */);
		if (e.code() == error_code_snap_storage_failed || e.code() == error_code_snap_tlog_failed ||
		    e.code() == error_code_operation_cancelled || e.code() == error_code_snap_disable_tlog_pop_failed) {
			// enable tlog pop on local tlog nodes
			std::vector<TLogInterface> tlogs = db->get().logSystemConfig.allLocalLogs(false);
			try {
				std::vector<Future<Void>> enablePops;
				enablePops.reserve(tlogs.size());
				for (const auto& tlog : tlogs) {
					enablePops.push_back(transformErrors(
					    throwErrorOr(tlog.enablePopRequest.tryGetReply(TLogEnablePopRequest(snapReq.snapUID))),
					    snap_enable_tlog_pop_failed()));
				}
				wait(waitForAll(enablePops));
			} catch (Error& error) {
				TraceEvent(SevDebug, "IgnoreEnableTLogPopFailure").log();
			}
		}
		throw e;
	}
	return Void();
}

ACTOR Future<Void> ddSnapCreate(DistributorSnapRequest snapReq,
                                Reference<AsyncVar<ServerDBInfo> const> db,
                                DDEnabledState* ddEnabledState) {
	state Future<Void> dbInfoChange = db->onChange();
	if (!ddEnabledState->setDDEnabled(false, snapReq.snapUID)) {
		// disable DD before doing snapCreate, if previous snap req has already disabled DD then this operation fails
		// here
		TraceEvent("SnapDDSetDDEnabledFailedInMemoryCheck").log();
		snapReq.reply.sendError(operation_failed());
		return Void();
	}
	double delayTime = g_network->isSimulated() ? 70.0 : SERVER_KNOBS->SNAP_CREATE_MAX_TIMEOUT;
	try {
		choose {
			when(wait(dbInfoChange)) {
				TraceEvent("SnapDDCreateDBInfoChanged")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				snapReq.reply.sendError(snap_with_recovery_unsupported());
			}
			when(wait(ddSnapCreateCore(snapReq, db))) {
				TraceEvent("SnapDDCreateSuccess")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				snapReq.reply.send(Void());
			}
			when(wait(delay(delayTime))) {
				TraceEvent("SnapDDCreateTimedOut")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				snapReq.reply.sendError(timed_out());
			}
		}
	} catch (Error& e) {
		TraceEvent("SnapDDCreateError")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID)
		    .error(e, true /*includeCancelled */);
		if (e.code() != error_code_operation_cancelled) {
			snapReq.reply.sendError(e);
		} else {
			// enable DD should always succeed
			bool success = ddEnabledState->setDDEnabled(true, snapReq.snapUID);
			ASSERT(success);
			throw e;
		}
	}
	// enable DD should always succeed
	bool success = ddEnabledState->setDDEnabled(true, snapReq.snapUID);
	ASSERT(success);
	return Void();
}

ACTOR Future<Void> ddExclusionSafetyCheck(DistributorExclusionSafetyCheckRequest req,
                                          Reference<DataDistributorData> self,
                                          Database cx) {
	TraceEvent("DDExclusionSafetyCheckBegin", self->ddId).log();
	std::vector<StorageServerInterface> ssis = wait(getStorageServers(cx));
	DistributorExclusionSafetyCheckReply reply(true);
	if (!self->teamCollection) {
		TraceEvent("DDExclusionSafetyCheckTeamCollectionInvalid", self->ddId).log();
		reply.safe = false;
		req.reply.send(reply);
		return Void();
	}
	// If there is only 1 team, unsafe to mark failed: team building can get stuck due to lack of servers left
	if (self->teamCollection->teams.size() <= 1) {
		TraceEvent("DDExclusionSafetyCheckNotEnoughTeams", self->ddId).log();
		reply.safe = false;
		req.reply.send(reply);
		return Void();
	}
	std::vector<UID> excludeServerIDs;
	// Go through storage server interfaces and translate Address -> server ID (UID)
	for (const AddressExclusion& excl : req.exclusions) {
		for (const auto& ssi : ssis) {
			if (excl.excludes(ssi.address()) ||
			    (ssi.secondaryAddress().present() && excl.excludes(ssi.secondaryAddress().get()))) {
				excludeServerIDs.push_back(ssi.id());
			}
		}
	}
	reply.safe = self->teamCollection->exclusionSafetyCheck(excludeServerIDs);
	TraceEvent("DDExclusionSafetyCheckFinish", self->ddId).log();
	req.reply.send(reply);
	return Void();
}

ACTOR Future<Void> waitFailCacheServer(Database* db, StorageServerInterface ssi) {
	state Transaction tr(*db);
	state Key key = storageCacheServerKey(ssi.id());
	wait(waitFailureClient(ssi.waitFailure));
	loop {
		tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
		try {
			tr.addReadConflictRange(storageCacheServerKeys);
			tr.clear(key);
			wait(tr.commit());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
	return Void();
}

ACTOR Future<Void> cacheServerWatcher(Database* db) {
	state Transaction tr(*db);
	state ActorCollection actors(false);
	state std::set<UID> knownCaches;
	loop {
		tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
		try {
			RangeResult range = wait(tr.getRange(storageCacheServerKeys, CLIENT_KNOBS->TOO_MANY));
			ASSERT(!range.more);
			std::set<UID> caches;
			for (auto& kv : range) {
				UID id;
				BinaryReader reader{ kv.key.removePrefix(storageCacheServersPrefix), Unversioned() };
				reader >> id;
				caches.insert(id);
				if (knownCaches.find(id) == knownCaches.end()) {
					StorageServerInterface ssi;
					BinaryReader reader{ kv.value, IncludeVersion() };
					reader >> ssi;
					actors.add(waitFailCacheServer(db, ssi));
				}
			}
			knownCaches = std::move(caches);
			tr.reset();
			wait(delay(5.0) || actors.getResult());
			ASSERT(!actors.getResult().isReady());
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

static int64_t getMedianShardSize(VectorRef<DDMetricsRef> metricVec) {
	std::nth_element(metricVec.begin(),
	                 metricVec.begin() + metricVec.size() / 2,
	                 metricVec.end(),
	                 [](const DDMetricsRef& d1, const DDMetricsRef& d2) { return d1.shardBytes < d2.shardBytes; });
	return metricVec[metricVec.size() / 2].shardBytes;
}

ACTOR Future<Void> ddGetMetrics(GetDataDistributorMetricsRequest req,
                                PromiseStream<GetMetricsListRequest> getShardMetricsList) {
	ErrorOr<Standalone<VectorRef<DDMetricsRef>>> result = wait(
	    errorOr(brokenPromiseToNever(getShardMetricsList.getReply(GetMetricsListRequest(req.keys, req.shardLimit)))));

	if (result.isError()) {
		req.reply.sendError(result.getError());
	} else {
		GetDataDistributorMetricsReply rep;
		if (!req.midOnly) {
			rep.storageMetricsList = result.get();
		} else {
			auto& metricVec = result.get();
			if (metricVec.empty())
				rep.midShardSize = 0;
			else {
				rep.midShardSize = getMedianShardSize(metricVec.contents());
			}
		}
		req.reply.send(rep);
	}

	return Void();
}

ACTOR Future<Void> dataDistributor(DataDistributorInterface di, Reference<AsyncVar<ServerDBInfo> const> db) {
	state Reference<DataDistributorData> self(new DataDistributorData(db, di.id()));
	state Future<Void> collection = actorCollection(self->addActor.getFuture());
	state PromiseStream<GetMetricsListRequest> getShardMetricsList;
	state Database cx = openDBOnServer(db, TaskPriority::DefaultDelay, LockAware::True);
	state ActorCollection actors(false);
	state DDEnabledState ddEnabledState;
	self->addActor.send(actors.getResult());
	self->addActor.send(traceRole(Role::DATA_DISTRIBUTOR, di.id()));

	try {
		TraceEvent("DataDistributorRunning", di.id());
		self->addActor.send(waitFailureServer(di.waitFailure.getFuture()));
		self->addActor.send(cacheServerWatcher(&cx));
		state Future<Void> distributor =
		    reportErrorsExcept(dataDistribution(self, getShardMetricsList, &ddEnabledState),
		                       "DataDistribution",
		                       di.id(),
		                       &normalDataDistributorErrors());

		loop choose {
			when(wait(distributor || collection)) {
				ASSERT(false);
				throw internal_error();
			}
			when(HaltDataDistributorRequest req = waitNext(di.haltDataDistributor.getFuture())) {
				req.reply.send(Void());
				TraceEvent("DataDistributorHalted", di.id()).detail("ReqID", req.requesterID);
				break;
			}
			when(GetDataDistributorMetricsRequest req = waitNext(di.dataDistributorMetrics.getFuture())) {
				actors.add(ddGetMetrics(req, getShardMetricsList));
			}
			when(DistributorSnapRequest snapReq = waitNext(di.distributorSnapReq.getFuture())) {
				actors.add(ddSnapCreate(snapReq, db, &ddEnabledState));
			}
			when(DistributorExclusionSafetyCheckRequest exclCheckReq =
			         waitNext(di.distributorExclCheckReq.getFuture())) {
				actors.add(ddExclusionSafetyCheck(exclCheckReq, self, cx));
			}
		}
	} catch (Error& err) {
		if (normalDataDistributorErrors().count(err.code()) == 0) {
			TraceEvent("DataDistributorError", di.id()).error(err, true);
			throw err;
		}
		TraceEvent("DataDistributorDied", di.id()).error(err, true);
	}

	return Void();
}

std::unique_ptr<DDTeamCollection> testTeamCollection(int teamSize,
                                                     Reference<IReplicationPolicy> policy,
                                                     int processCount) {
	Database database = DatabaseContext::create(
	    makeReference<AsyncVar<ClientDBInfo>>(), Never(), LocalityData(), EnableLocalityLoadBalance::False);

	DatabaseConfiguration conf;
	conf.storageTeamSize = teamSize;
	conf.storagePolicy = policy;

	auto collection =
	    std::unique_ptr<DDTeamCollection>(new DDTeamCollection(database,
	                                                           UID(0, 0),
	                                                           MoveKeysLock(),
	                                                           PromiseStream<RelocateShard>(),
	                                                           makeReference<ShardsAffectedByTeamFailure>(),
	                                                           conf,
	                                                           {},
	                                                           {},
	                                                           Future<Void>(Void()),
	                                                           makeReference<AsyncVar<bool>>(true),
	                                                           IsPrimary::True,
	                                                           makeReference<AsyncVar<bool>>(false),
	                                                           makeReference<AsyncVar<bool>>(false),
	                                                           PromiseStream<GetMetricsRequest>(),
	                                                           Promise<UID>(),
	                                                           PromiseStream<Promise<int>>()));

	for (int id = 1; id <= processCount; ++id) {
		UID uid(id, 0);
		StorageServerInterface interface;
		interface.uniqueID = uid;
		interface.locality.set(LiteralStringRef("machineid"), Standalone<StringRef>(std::to_string(id)));
		interface.locality.set(LiteralStringRef("zoneid"), Standalone<StringRef>(std::to_string(id % 5)));
		interface.locality.set(LiteralStringRef("data_hall"), Standalone<StringRef>(std::to_string(id % 3)));
		collection->server_info[uid] = makeReference<TCServerInfo>(
		    interface, collection.get(), ProcessClass(), true, collection->storageServerSet);
		collection->server_status.set(uid, ServerStatus(false, false, false, interface.locality));
		collection->checkAndCreateMachine(collection->server_info[uid]);
	}

	return collection;
}

std::unique_ptr<DDTeamCollection> testMachineTeamCollection(int teamSize,
                                                            Reference<IReplicationPolicy> policy,
                                                            int processCount) {
	Database database = DatabaseContext::create(
	    makeReference<AsyncVar<ClientDBInfo>>(), Never(), LocalityData(), EnableLocalityLoadBalance::False);

	DatabaseConfiguration conf;
	conf.storageTeamSize = teamSize;
	conf.storagePolicy = policy;

	auto collection =
	    std::unique_ptr<DDTeamCollection>(new DDTeamCollection(database,
	                                                           UID(0, 0),
	                                                           MoveKeysLock(),
	                                                           PromiseStream<RelocateShard>(),
	                                                           makeReference<ShardsAffectedByTeamFailure>(),
	                                                           conf,
	                                                           {},
	                                                           {},
	                                                           Future<Void>(Void()),
	                                                           makeReference<AsyncVar<bool>>(true),
	                                                           IsPrimary::True,
	                                                           makeReference<AsyncVar<bool>>(false),
	                                                           makeReference<AsyncVar<bool>>(false),
	                                                           PromiseStream<GetMetricsRequest>(),
	                                                           Promise<UID>(),
	                                                           PromiseStream<Promise<int>>()));

	for (int id = 1; id <= processCount; id++) {
		UID uid(id, 0);
		StorageServerInterface interface;
		interface.uniqueID = uid;
		int process_id = id;
		int dc_id = process_id / 1000;
		int data_hall_id = process_id / 100;
		int zone_id = process_id / 10;
		int machine_id = process_id / 5;

		printf("testMachineTeamCollection: process_id:%d zone_id:%d machine_id:%d ip_addr:%s\n",
		       process_id,
		       zone_id,
		       machine_id,
		       interface.address().toString().c_str());
		interface.locality.set(LiteralStringRef("processid"), Standalone<StringRef>(std::to_string(process_id)));
		interface.locality.set(LiteralStringRef("machineid"), Standalone<StringRef>(std::to_string(machine_id)));
		interface.locality.set(LiteralStringRef("zoneid"), Standalone<StringRef>(std::to_string(zone_id)));
		interface.locality.set(LiteralStringRef("data_hall"), Standalone<StringRef>(std::to_string(data_hall_id)));
		interface.locality.set(LiteralStringRef("dcid"), Standalone<StringRef>(std::to_string(dc_id)));
		collection->server_info[uid] = makeReference<TCServerInfo>(
		    interface, collection.get(), ProcessClass(), true, collection->storageServerSet);

		collection->server_status.set(uid, ServerStatus(false, false, false, interface.locality));
	}

	int totalServerIndex = collection->constructMachinesFromServers();
	printf("testMachineTeamCollection: construct machines for %d servers\n", totalServerIndex);

	return collection;
}

TEST_CASE("DataDistribution/AddTeamsBestOf/UseMachineID") {
	wait(Future<Void>(Void()));

	int teamSize = 3; // replication size
	int processSize = 60;
	int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;

	Reference<IReplicationPolicy> policy = Reference<IReplicationPolicy>(
	    new PolicyAcross(teamSize, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state std::unique_ptr<DDTeamCollection> collection = testMachineTeamCollection(teamSize, policy, processSize);

	collection->addTeamsBestOf(30, desiredTeams, maxTeams);

	ASSERT(collection->sanityCheckTeams() == true);

	return Void();
}

TEST_CASE("DataDistribution/AddTeamsBestOf/NotUseMachineID") {
	wait(Future<Void>(Void()));

	int teamSize = 3; // replication size
	int processSize = 60;
	int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;

	Reference<IReplicationPolicy> policy = Reference<IReplicationPolicy>(
	    new PolicyAcross(teamSize, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state std::unique_ptr<DDTeamCollection> collection = testMachineTeamCollection(teamSize, policy, processSize);

	if (collection == nullptr) {
		fprintf(stderr, "collection is null\n");
		return Void();
	}

	collection->addBestMachineTeams(30); // Create machine teams to help debug
	collection->addTeamsBestOf(30, desiredTeams, maxTeams);
	collection->sanityCheckTeams(); // Server team may happen to be on the same machine team, although unlikely

	return Void();
}

TEST_CASE("DataDistribution/AddAllTeams/isExhaustive") {
	Reference<IReplicationPolicy> policy =
	    Reference<IReplicationPolicy>(new PolicyAcross(3, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state int processSize = 10;
	state int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	state int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;
	state std::unique_ptr<DDTeamCollection> collection = testTeamCollection(3, policy, processSize);

	int result = collection->addTeamsBestOf(200, desiredTeams, maxTeams);

	// The maximum number of available server teams without considering machine locality is 120
	// The maximum number of available server teams with machine locality constraint is 120 - 40, because
	// the 40 (5*4*2) server teams whose servers come from the same machine are invalid.
	ASSERT(result == 80);

	return Void();
}

TEST_CASE("/DataDistribution/AddAllTeams/withLimit") {
	Reference<IReplicationPolicy> policy =
	    Reference<IReplicationPolicy>(new PolicyAcross(3, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state int processSize = 10;
	state int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	state int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;

	state std::unique_ptr<DDTeamCollection> collection = testTeamCollection(3, policy, processSize);

	int result = collection->addTeamsBestOf(10, desiredTeams, maxTeams);

	ASSERT(result >= 10);

	return Void();
}

TEST_CASE("/DataDistribution/AddTeamsBestOf/SkippingBusyServers") {
	wait(Future<Void>(Void()));
	Reference<IReplicationPolicy> policy =
	    Reference<IReplicationPolicy>(new PolicyAcross(3, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state int processSize = 10;
	state int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	state int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;
	state int teamSize = 3;
	// state int targetTeamsPerServer = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * (teamSize + 1) / 2;
	state std::unique_ptr<DDTeamCollection> collection = testTeamCollection(teamSize, policy, processSize);

	collection->addTeam(std::set<UID>({ UID(1, 0), UID(2, 0), UID(3, 0) }), true);
	collection->addTeam(std::set<UID>({ UID(1, 0), UID(3, 0), UID(4, 0) }), true);

	state int result = collection->addTeamsBestOf(8, desiredTeams, maxTeams);

	ASSERT(result >= 8);

	for (auto process = collection->server_info.begin(); process != collection->server_info.end(); process++) {
		auto teamCount = process->second->teams.size();
		ASSERT(teamCount >= 1);
		// ASSERT(teamCount <= targetTeamsPerServer);
	}

	return Void();
}

// Due to the randomness in choosing the machine team and the server team from the machine team, it is possible that
// we may not find the remaining several (e.g., 1 or 2) available teams.
// It is hard to conclude what is the minimum number of  teams the addTeamsBestOf() should create in this situation.
TEST_CASE("/DataDistribution/AddTeamsBestOf/NotEnoughServers") {
	wait(Future<Void>(Void()));

	Reference<IReplicationPolicy> policy =
	    Reference<IReplicationPolicy>(new PolicyAcross(3, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state int processSize = 5;
	state int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	state int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;
	state int teamSize = 3;
	state std::unique_ptr<DDTeamCollection> collection = testTeamCollection(teamSize, policy, processSize);

	collection->addTeam(std::set<UID>({ UID(1, 0), UID(2, 0), UID(3, 0) }), true);
	collection->addTeam(std::set<UID>({ UID(1, 0), UID(3, 0), UID(4, 0) }), true);

	collection->addBestMachineTeams(10);
	int result = collection->addTeamsBestOf(10, desiredTeams, maxTeams);

	if (collection->machineTeams.size() != 10 || result != 8) {
		collection->traceAllInfo(true); // Debug message
	}

	// NOTE: Due to the pure randomness in selecting a machine for a machine team,
	// we cannot guarantee that all machine teams are created.
	// When we chnage the selectReplicas function to achieve such guarantee, we can enable the following ASSERT
	ASSERT(collection->machineTeams.size() == 10); // Should create all machine teams

	// We need to guarantee a server always have at least a team so that the server can participate in data distribution
	for (auto process = collection->server_info.begin(); process != collection->server_info.end(); process++) {
		auto teamCount = process->second->teams.size();
		ASSERT(teamCount >= 1);
	}

	// If we find all available teams, result will be 8 because we prebuild 2 teams
	ASSERT(result == 8);

	return Void();
}
