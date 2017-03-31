////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "Supervision.h"

#include <thread>

#include "Agency/AddFollower.h"
#include "Agency/Agent.h"
#include "Agency/CleanOutServer.h"
#include "Agency/FailedServer.h"
#include "Agency/Job.h"
#include "Agency/JobContext.h"
#include "Agency/RemoveFollower.h"
#include "Agency/Store.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/ConditionLocker.h"
#include "Basics/MutexLocker.h"

using namespace arangodb;

using namespace arangodb::consensus;
using namespace arangodb::application_features;

// This is initialized in AgencyFeature:
std::string Supervision::_agencyPrefix = "/arango";

Supervision::Supervision()
  : arangodb::Thread("Supervision"),
  _agent(nullptr),
  _snapshot("Supervision"),
  _transient("Transient"),
  _frequency(1.),
  _gracePeriod(5.),
  _jobId(0),
  _jobIdMax(0),
  _selfShutdown(false),
  _ranOnceAfterLead(false) {}

Supervision::~Supervision() { shutdown(); };

static std::string const syncPrefix = "/Sync/ServerStates/";
static std::string const healthPrefix = "/Supervision/Health/";
static std::string const planDBServersPrefix = "/Plan/DBServers";
static std::string const planCoordinatorsPrefix = "/Plan/Coordinators";
static std::string const targetShortID = "/Target/MapUniqueToShortID/";
static std::string const currentServersRegisteredPrefix =
  "/Current/ServersRegistered";
static std::string const foxxmaster = "/Current/Foxxmaster";

// Upgrade agency, guarded by wakeUp
void Supervision::upgradeAgency() {
  Builder builder;
  Slice fails;
  { VPackArrayBuilder a(&builder);
    { VPackObjectBuilder o(&builder);
      builder.add(VPackValue(failedServersPrefix));
      { VPackObjectBuilder oo(&builder);
        try {
          fails = _snapshot(failedServersPrefix).slice();
          if (fails.isArray()) {
            for (auto const& fail : VPackArrayIterator(fails)) {
              builder.add(VPackValue(fail.copyString()));
              { VPackObjectBuilder ooo(&builder); }
            }
          }
        } catch (...) {}
      }
    }
  }
  if (!fails.isObject()) {
    singleWriteTransaction(_agent, builder);
  }
}

// Check all DB servers, guarded above doChecks
std::vector<check_t> Supervision::checkDBServers() {

  std::vector<check_t> ret;
  auto const& machinesPlanned = _snapshot(planDBServersPrefix).children();
  auto const& serversRegistered =
      _snapshot(currentServersRegisteredPrefix).children();
  
  std::vector<std::string> todelete;
  for (auto const& machine : _snapshot(healthPrefix).children()) {
    if (machine.first.substr(0, 2) == "PR") {
      todelete.push_back(machine.first);
    }
  }
  
  for (auto const& machine : machinesPlanned) {
    std::string lastHeartbeatTime, lastHeartbeatAcked, lastHeartbeatStatus,
      lastStatus, heartbeatTime, heartbeatStatus, serverID(machine.first),
      shortName("Unknown");
    bool good(false), reportPersistent(false),
      sync(_transient.has(syncPrefix + serverID));
    
    todelete.erase(
      std::remove(todelete.begin(), todelete.end(), serverID), todelete.end());
    shortName = _snapshot(targetShortID + serverID + "/ShortName").toJson();

    if (sync) {
      heartbeatTime = _transient(syncPrefix + serverID + "/time").toJson();
      heartbeatStatus = _transient(syncPrefix + serverID + "/status").toJson();
      lastHeartbeatTime =
        _transient(healthPrefix + serverID + "/LastHeartbeatSent").toJson();
      lastHeartbeatAcked =
        _transient(healthPrefix + serverID + "/LastHeartbeatAcked").toJson();
      lastHeartbeatStatus =
        _transient(healthPrefix + serverID + "/LastHeartbeatStatus").toJson();
      lastStatus = _transient(healthPrefix + serverID + "/Status").toJson();
      if (lastHeartbeatTime != heartbeatTime) {  // Update
        good = true;
      }
    } 

    auto report = std::make_shared<Builder>();
    
    { VPackArrayBuilder transaction(report.get());

      std::shared_ptr<VPackBuilder> envelope;
      { VPackObjectBuilder operation(report.get());

        report->add(VPackValue(healthPrefix + serverID));
        { VPackObjectBuilder oo(report.get());

          report->add("LastHeartbeatSent", VPackValue(heartbeatTime));
          report->add("LastHeartbeatStatus", VPackValue(heartbeatStatus));
          report->add("Role", VPackValue("DBServer"));
          report->add("ShortName", VPackValue(shortName));

          auto endpoint = serversRegistered.find(serverID);
          if (endpoint != serversRegistered.end()) {
            endpoint = endpoint->second->children().find("endpoint");
            if (endpoint != endpoint->second->children().end()) {
              if (endpoint->second->children().size() == 0) {
                VPackSlice epString = endpoint->second->slice();
                if (epString.isString()) {
                  report->add("Endpoint", epString);
                }
              }
            }
          }
          
          if (lastHeartbeatStatus != heartbeatStatus) {
            reportPersistent = true;
          }
          
          if (good) {
            
            if (lastStatus != Supervision::HEALTH_STATUS_GOOD) {
              reportPersistent = true;
            }
            report->add(
              "LastHeartbeatAcked",
              VPackValue(timepointToString(std::chrono::system_clock::now())));
            report->add("Status", VPackValue(Supervision::HEALTH_STATUS_GOOD));
            
            std::string failedServerPath = failedServersPrefix + "/" + serverID;
            if (_snapshot.exists(failedServerPath).size() == 3) {
              Builder del;
              { VPackArrayBuilder c(&del);
                { VPackObjectBuilder cc(&del);
                  del.add(VPackValue(failedServerPath));
                  { VPackObjectBuilder ccc(&del);
                    del.add("op", VPackValue("delete")); }}}
              singleWriteTransaction(_agent, del);
            }
            
          } else {
            
            auto elapsed = std::chrono::duration<double>(
              std::chrono::system_clock::now() -
              stringToTimepoint(lastHeartbeatAcked));
            
            if (elapsed.count() > _gracePeriod) {
              if (lastStatus == Supervision::HEALTH_STATUS_BAD) {
                reportPersistent = true;
                report->add(
                  "Status", VPackValue(Supervision::HEALTH_STATUS_FAILED));
                envelope = std::make_shared<VPackBuilder>();
                FailedServer(_snapshot, _agent, std::to_string(_jobId++),
                             "supervision", serverID).create(envelope);
              }
            } else {
              if (lastStatus != Supervision::HEALTH_STATUS_BAD) {
                reportPersistent = true;
                report->add(
                  "Status", VPackValue(Supervision::HEALTH_STATUS_BAD));
              }
            }
            
          }
          
        } // Supervision/Health
        
        if (envelope != nullptr) { // Failed server operation
          TRI_ASSERT(
            envelope->slice().isArray() && envelope->slice()[0].isObject());
          for (const auto& i : VPackObjectIterator(envelope->slice()[0])) {
            report->add(i.key.copyString(), i.value);
          }
        }
      } // Operation
      
      if (envelope != nullptr) { // Failed server precondition
        TRI_ASSERT(envelope->slice().isArray() && envelope->slice()[1].isObject());
        report->add(envelope->slice()[1]);
      }
      
    } // Transaction
    
    if (!this->isStopping()) {
      transient(_agent, *report);
      if (reportPersistent) {
        singleWriteTransaction(_agent, *report);
      }
    }
    
  }
    
  if (!todelete.empty()) {
    query_t del = std::make_shared<Builder>();
    { VPackArrayBuilder a(del.get());
      { VPackArrayBuilder aa(del.get());
        { VPackArrayBuilder aaa(del.get());
          for (auto const& srv : todelete) {
            del->add(VPackValue(_agencyPrefix + healthPrefix + srv));
            { VPackArrayBuilder aaaa(del.get());                     
              del->add("op", VPackValue("delete")); }}}}}
    _agent->write(del);
  }

  return ret;
}

// Check all coordinators, guarded above doChecks
std::vector<check_t> Supervision::checkCoordinators() {

  std::vector<check_t> ret;
  auto const& machinesPlanned = _snapshot(planCoordinatorsPrefix).children();
  auto const& serversRegistered =
      _snapshot(currentServersRegisteredPrefix).children();
  
  std::string currentFoxxmaster;
  try {
    currentFoxxmaster = _snapshot(foxxmaster).getString();
  } catch (...) {}

  std::string goodServerId;
  bool foxxmasterOk = false;
  std::vector<std::string> todelete;
  for (auto const& machine : _snapshot(healthPrefix).children()) {
    if (machine.first.substr(0, 2) == "CR") {
      todelete.push_back(machine.first);
    }
  }

  for (auto const& machine : machinesPlanned) {
    std::string lastHeartbeatTime, lastHeartbeatAcked, lastHeartbeatStatus,
      lastStatus, heartbeatTime, heartbeatStatus, serverID = machine.first,
      shortName = "Unknown";
    
    bool reportPersistent(false), good(false),
      sync(_transient.has(syncPrefix + serverID));
    
    todelete.erase(std::remove(todelete.begin(), todelete.end(), serverID),
                   todelete.end());
    
    shortName = _snapshot(targetShortID + serverID + "/ShortName").toJson();
    
    if (sync) {
      heartbeatTime = _transient(syncPrefix + serverID + "/time").toJson();
      heartbeatStatus = _transient(syncPrefix + serverID + "/status").toJson();
      lastHeartbeatTime =
        _transient(healthPrefix + serverID + "/LastHeartbeatSent").toJson();
      lastHeartbeatAcked =
        _transient(healthPrefix + serverID + "/LastHeartbeatAcked").toJson();
      lastHeartbeatStatus =
        _transient(healthPrefix + serverID + "/LastHeartbeatStatus").toJson();
      lastStatus = _transient(healthPrefix + serverID + "/Status").toJson();
      if (lastHeartbeatTime != heartbeatTime) {  // Update
        good = true;
      }
    } 
    
    query_t report = std::make_shared<Builder>();

    { VPackArrayBuilder transactions(report.get());
      { VPackArrayBuilder transaction(report.get());
        { VPackObjectBuilder operation(report.get());

          report->add(VPackValue(_agencyPrefix + healthPrefix + serverID));
          { VPackObjectBuilder oo(report.get());

            report->add("LastHeartbeatSent", VPackValue(heartbeatTime));
            report->add("LastHeartbeatStatus", VPackValue(heartbeatStatus));
            report->add("Role", VPackValue("Coordinator"));
            report->add("ShortName", VPackValue(shortName));
            auto endpoint = serversRegistered.find(serverID);
            if (endpoint != serversRegistered.end()) {
              endpoint = endpoint->second->children().find("endpoint");
              if (endpoint != endpoint->second->children().end()) {
                if (endpoint->second->children().size() == 0) {
                  VPackSlice epString = endpoint->second->slice();
                  if (epString.isString()) {
                    report->add("Endpoint", epString);
                  }
                }
              }
            }
            
            if (heartbeatStatus != lastHeartbeatStatus) {
              reportPersistent = true;
            }
            
            if (good) {
              if (lastStatus != Supervision::HEALTH_STATUS_GOOD) {
                reportPersistent = true;
              }
              if (goodServerId.empty()) {
                goodServerId = serverID;
              }
              if (serverID == currentFoxxmaster) {
                foxxmasterOk = true;
              }
              report->add(
                "LastHeartbeatAcked",
                VPackValue(timepointToString(std::chrono::system_clock::now())));
              report->add("Status", VPackValue(Supervision::HEALTH_STATUS_GOOD));
            } else {

              auto elapsed = std::chrono::duration<double>(
                std::chrono::system_clock::now() -
                stringToTimepoint(lastHeartbeatAcked)).count();
              
              if (elapsed > _gracePeriod || !sync) {
                if (lastStatus == Supervision::HEALTH_STATUS_BAD) {
                  report->add(
                    "Status", VPackValue(Supervision::HEALTH_STATUS_FAILED));
                  reportPersistent = true;
                }
              } else {
                report->add(
                  "Status", VPackValue(Supervision::HEALTH_STATUS_BAD));
              }
            }
          }
        }
      }
    }

    if (!this->isStopping()) {
      _agent->transient(report);
      if (reportPersistent) { // STATUS changes should be persisted
        _agent->write(report);
      }
    }
  }

  if (!todelete.empty()) {
    query_t del = std::make_shared<Builder>();
    { VPackArrayBuilder txs(del.get());
      { VPackArrayBuilder tx(del.get());
        { VPackObjectBuilder d(del.get());
          for (auto const& srv : todelete) {
            del->add(VPackValue(_agencyPrefix + healthPrefix + srv));
            { VPackObjectBuilder e(del.get());
              del->add("op", VPackValue("delete")); }}}}}
    _agent->write(del);
  }

  if (!foxxmasterOk && !goodServerId.empty()) {
    query_t create = std::make_shared<Builder>();
    { VPackArrayBuilder txs(create.get());
      { VPackArrayBuilder tx(create.get());
        { VPackObjectBuilder d(create.get());
          create->add(_agencyPrefix + foxxmaster, VPackValue(goodServerId)); }}}
    _agent->write(create);
  }

  return ret;
}

// Update local agency snapshot, guarded by callers
bool Supervision::updateSnapshot() {

  if (_agent == nullptr || this->isStopping()) {
    return false;
  }
  
  try {
    _snapshot = _agent->readDB().get(_agencyPrefix);
    _transient = _agent->transient().get(_agencyPrefix);
  } catch (...) {}
  
  return true;
  
}

// All checks, guarded by main thread
bool Supervision::doChecks() {
  checkDBServers();
  checkCoordinators();
  return true;
}


void Supervision::run() {
  // First wait until somebody has initialized the ArangoDB data, before
  // that running the supervision does not make sense and will indeed
  // lead to horrible errors:
  while (!this->isStopping()) {
    std::this_thread::sleep_for(std::chrono::duration<double>(5.0));
    MUTEX_LOCKER(locker, _lock);
    try {
      _snapshot = _agent->readDB().get(_agencyPrefix);
      if (_snapshot.children().size() > 0) {
        break;
      }
    } catch (...) {
    }
    LOG_TOPIC(DEBUG, Logger::SUPERVISION) << "Waiting for ArangoDB to "
      "initialize its data.";
  }

  bool shutdown = false;
  {
    CONDITION_LOCKER(guard, _cv);
    TRI_ASSERT(_agent != nullptr);

    while (!this->isStopping()) {

      // Get bunch of job IDs from agency for future jobs
      if (_agent->leading() && (_jobId == 0 || _jobId == _jobIdMax)) {
        getUniqueIds();  // cannot fail but only hang
      }

      {
        MUTEX_LOCKER(locker, _lock);

        updateSnapshot();

        if (_agent->leading()) {
          upgradeAgency();
          fixPrototypeChain();
          auto secondsSinceLeader = std::chrono::duration<double>(
            std::chrono::system_clock::now() - _agent->leaderSince()).count();
          if (secondsSinceLeader > _gracePeriod) {
            doChecks();
          }
        }

        if (isShuttingDown()) {
          handleShutdown();
        } else if (_selfShutdown) {
          shutdown = true;
          break;
        } else if (_agent->leading()) {
          if (!handleJobs()) {
            break;
          }
        }
      }
      _cv.wait(static_cast<uint64_t>(1000000 * _frequency));
    }
  }
  if (shutdown) {
    ApplicationServer::server->beginShutdown();
  }
}

// Guarded by caller
bool Supervision::isShuttingDown() {
  try {
    return _snapshot("/Shutdown").getBool();
  } catch (...) {
    return false;
  }
}

// Guarded by caller
std::string Supervision::serverHealth(std::string const& serverName) {
  try {
    std::string const serverStatus(healthPrefix + serverName + "/Status");
    auto const status = _snapshot(serverStatus).getString();
    return status;
  } catch (...) {
    LOG_TOPIC(WARN, Logger::SUPERVISION)
        << "Couldn't read server health status for server " << serverName;
    return "";
  }
}

// Guarded by caller
void Supervision::handleShutdown() {
  _selfShutdown = true;
  LOG_TOPIC(DEBUG, Logger::SUPERVISION) << "Waiting for clients to shut down";
  auto const& serversRegistered =
      _snapshot(currentServersRegisteredPrefix).children();
  bool serversCleared = true;
  for (auto const& server : serversRegistered) {
    if (server.first == "Version") {
      continue;
    }

    LOG_TOPIC(DEBUG, Logger::SUPERVISION)
      << "Waiting for " << server.first << " to shutdown";

    if (serverHealth(server.first) != HEALTH_STATUS_GOOD) {
      LOG_TOPIC(WARN, Logger::SUPERVISION)
        << "Server " << server.first << " did not shutdown properly it seems!";
      continue;
    }
    serversCleared = false;
  }

  if (serversCleared) {
    if (_agent->leading()) {
      auto del = std::make_shared<Builder>();
      { VPackArrayBuilder txs(del.get());
        { VPackArrayBuilder tx(del.get());
          { VPackObjectBuilder o(del.get());
            del->add(VPackValue(_agencyPrefix + "/Shutdown"));
            { VPackObjectBuilder oo(del.get());
              del->add("op", VPackValue("delete")); }}}}
      auto result = _agent->write(del);
      if (result.indices.size() != 1) {
        LOG(ERR) << "Invalid resultsize of " << result.indices.size()
                 << " found during shutdown";
      } else {
        if (!_agent->waitFor(result.indices.at(0))) {
          LOG(ERR) << "Result was not written to followers during shutdown";
        }
      }
    }
  }
}

// Guarded by caller 
bool Supervision::handleJobs() {
  // Do supervision
  
  shrinkCluster();
  enforceReplication();
  workJobs();

  return true;
}

// Guarded by caller
void Supervision::workJobs() {

  for (auto const& todoEnt : _snapshot(toDoPrefix).children()) {
    JobContext(
      TODO, (*todoEnt.second)("jobId").getString(), _snapshot, _agent).run();
  }

  for (auto const& pendEnt : _snapshot(pendingPrefix).children()) {
    JobContext(
      PENDING, (*pendEnt.second)("jobId").getString(), _snapshot, _agent).run();
  }
  
}


void Supervision::enforceReplication() {
  auto const& plannedDBs = _snapshot(planColPrefix).children();

  for (const auto& db_ : plannedDBs) { // Planned databases
    auto const& db = *(db_.second);
    for (const auto& col_ : db.children()) { // Planned collections
      auto const& col = *(col_.second);
      
      size_t replicationFactor;
      try {
        replicationFactor = col("replicationFactor").slice().getUInt();
      } catch (std::exception const&) {
        LOG_TOPIC(DEBUG, Logger::SUPERVISION)
          << "no replicationFactor entry in " << col.toJson();
        continue;
      }

      // mop: satellites => distribute to every server
      if (replicationFactor == 0) {
        auto available = Job::availableServers(_snapshot);
        replicationFactor = available.size();
      }
      
      bool clone = col.has("distributeShardsLike");

      if (!clone) {
        for (auto const& shard_ : col("shards").children()) { // Pl shards
          auto const& shard = *(shard_.second);
          
          size_t actualReplicationFactor = shard.slice().length();
          if (actualReplicationFactor != replicationFactor) {
            // Check that there is not yet an addFollower or removeFollower
            // or moveShard job in ToDo for this shard:
            auto const& todo = _snapshot(toDoPrefix).children();
            bool found = false;
            for (auto const& pair : todo) {
              auto const& job = pair.second;
              if (job->has("type") &&
                  ((*job)("type").getString() == "addFollower" ||
                   (*job)("type").getString() == "removeFollower" ||
                   (*job)("type").getString() == "moveShard") &&
                  job->has("shard") &&
                  (*job)("shard").getString() == shard_.first) {
                found = true;
                LOG_TOPIC(DEBUG, Logger::SUPERVISION) << "already found "
                  "addFollower or removeFollower job in ToDo, not scheduling "
                  "again for shard " << shard_.first;
                break;
              }
            }
            // Check that shard is not locked:
            if (_snapshot.has(blockedShardsPrefix + shard_.first)) {
              found = true;
            }
            if (!found) {
              if (actualReplicationFactor < replicationFactor) {
                AddFollower(
                  _snapshot, _agent, std::to_string(_jobId++), "supervision",
                  db_.first, col_.first, shard_.first).run();
              } else {
                RemoveFollower(
                  _snapshot, _agent, std::to_string(_jobId++), "supervision",
                  db_.first, col_.first, shard_.first).run();
              }
            }
          }
        }
      }
    }
  }
  
}

void Supervision::fixPrototypeChain() {

  std::function<std::string (std::string const&, std::string const&)> resolve;
  resolve = [&] (std::string const& db, std::string const& col) {
    Node* n = nullptr;
    try {
      n = &_snapshot(planColPrefix + db + "/" + col);
    } catch (...) {}
    std::string s;
    if (n != nullptr && n->has("distributeShardsLike")) {
      s = (*n)("distributeShardsLike").getString();
    }
    return (s.empty()) ? col : resolve(db, s);
  };
  
  VPackBuilder migrate;
  { VPackArrayBuilder trxs(&migrate);
    for (auto const& database : _snapshot(planColPrefix).children()) {
      for (auto const& collection : database.second->children()) {
        if (collection.second->has("distributeShardsLike")) {
          auto prototype = (*collection.second)("distributeShardsLike").getString();
          if (!prototype.empty()) {
            std::string u = resolve(database.first, prototype);
            if (u != prototype) {
              { VPackArrayBuilder trx(&migrate);
                { VPackObjectBuilder oper(&migrate);
                  migrate.add(
                    planColPrefix + database.first + "/" + collection.first + "/" +
                    "distributeShardsLike", VPackValue(u)); }
                { VPackObjectBuilder prec(&migrate);
                  migrate.add(
                    planColPrefix + database.first + "/" + collection.first + "/" +
                    "distributeShardsLike", VPackValue(prototype)); }
              }
            }
          }
        }
      }
    }
  }
  
  if (migrate.slice().length() > 0) {
    LOG_TOPIC (DEBUG, Logger::SUPERVISION) << migrate.toJson();
    trans_ret_t res = generalTransaction(_agent, migrate);
    for (const auto& failed : VPackArrayIterator(res.result->slice())) {
      if (failed.isObject()) { // Precondition failed for this one
        LOG_TOPIC(WARN, Logger::SUPERVISION)
          << "Plan has changed since resolution of distributeShardsLike for " <<
          failed.keyAt(0).copyString();
      }
    }
  }
  
}

// Shrink cluster if applicable, guarded by caller
void Supervision::shrinkCluster() {

  auto const& todo = _snapshot(toDoPrefix).children();
  auto const& pending = _snapshot(pendingPrefix).children();

  if (!todo.empty() || !pending.empty()) { // This is low priority
    return;
  }
  
  // Get servers from plan
  auto availServers = Job::availableServers(_snapshot);

  size_t targetNumDBServers;
  try {
    targetNumDBServers = _snapshot("/Target/NumberOfDBServers").getUInt();
  } catch (std::exception const& e) {
    LOG_TOPIC(TRACE, Logger::SUPERVISION)
        << "Targeted number of DB servers not set yet: " << e.what();
    return;
  }

  // Only if number of servers in target is smaller than the available
  if (targetNumDBServers < availServers.size()) {
    // Minimum 1 DB server must remain
    if (availServers.size() == 1) {
      LOG_TOPIC(DEBUG, Logger::SUPERVISION)
          << "Only one db server left for operation";
      return;
    }

    // mop: any failed server is first considered useless and may be cleared
    // from the list later on :O
    std::vector<std::string> uselessFailedServers;
    auto failedPivot = std::partition(
        availServers.begin(), availServers.end(), [this](std::string server) {
          return serverHealth(server) != HEALTH_STATUS_FAILED;
        });
    std::move(failedPivot, availServers.end(),
              std::back_inserter(uselessFailedServers));
    availServers.erase(failedPivot, availServers.end());

    /**
     * mop: TODO instead of using Plan/Collections we should watch out for
     * Plan/ReplicationFactor and Current...when the replicationFactor is not
     * fullfilled we should add a follower to the plan
     * When seeing more servers in Current than replicationFactor we should
     * remove a server.
     * RemoveServer then should be changed so that it really just kills a server
     * after a while...
     * this way we would have implemented changing the replicationFactor and
     * have an awesome new feature
     **/
    // Find greatest replication factor among all collections
    uint64_t maxReplFact = 1;
    auto const& databases = _snapshot(planColPrefix).children();
    for (auto const& database : databases) {
      for (auto const& collptr : database.second->children()) {
        uint64_t replFact{0};
        try {
          replFact = (*collptr.second)("replicationFactor").getUInt();
          if (replFact > maxReplFact) {
            maxReplFact = replFact;
          }
        } catch (std::exception const& e) {
          LOG_TOPIC(WARN, Logger::SUPERVISION)
            << "Cannot retrieve replication factor for collection "
            << collptr.first << ": " << e.what();
          return;
        }
        if (uselessFailedServers.size() > 0) {
          try {
            auto const& shards =
                (*collptr.second)("shards").children();
            for (auto const& shard : shards) {
              auto const& children = shard.second->children();
              for (size_t i = 0; i < children.size(); i++) {
                auto const& server =
                    children.at(std::to_string(i))->getString();
                auto found = std::find(uselessFailedServers.begin(),
                                       uselessFailedServers.end(), server);

                bool isLeader = i == 0;
                if (found != uselessFailedServers.end() &&
                    (isLeader || replFact >= availServers.size())) {
                  // mop: apparently it has been a lie :O it is not useless
                  uselessFailedServers.erase(found);
                }
              }
            }
          } catch (std::exception const& e) {
            LOG_TOPIC(WARN, Logger::SUPERVISION)
                << "Cannot retrieve shard information for " << collptr.first
                << ": " << e.what();
          } catch (...) {
            LOG_TOPIC(WARN, Logger::SUPERVISION)
                << "Cannot retrieve shard information for " << collptr.first;
          }
        }
      }
    }

    if (uselessFailedServers.size() > 0) {
      // Schedule last server for cleanout

      LOG_TOPIC(INFO, Logger::SUPERVISION) << "not starting RemoveServer job "
        "because it is deleted.";
#warning Disabled because RemoveServer job is deleted.
      //RemoveServer(_snapshot, _agent, std::to_string(_jobId++), "supervision",
      //             uselessFailedServers.back()).run();
      return;
    }
    // mop: do not account any failedservers in this calculation..the ones
    // having
    // a state of failed still have data of interest to us! We wait indefinately
    // for them to recover or for the user to remove them
    if (maxReplFact < availServers.size()) {
      // Clean out as long as number of available servers is bigger
      // than maxReplFactor and bigger than targeted number of db servers
      if (availServers.size() > maxReplFact &&
          availServers.size() > targetNumDBServers) {
        // Sort servers by name
        std::sort(availServers.begin(), availServers.end());

        // Schedule last server for cleanout
        CleanOutServer(_snapshot, _agent, std::to_string(_jobId++),
                       "supervision", availServers.back()).run();
      }
    }
  }
}

// Start thread
bool Supervision::start() {
  Thread::start();
  return true;
}

// Start thread with agent
bool Supervision::start(Agent* agent) {
  _agent = agent;
  _frequency = _agent->config().supervisionFrequency();
  _gracePeriod = _agent->config().supervisionGracePeriod();
  return start();
}

static std::string const syncLatest = "/Sync/LatestID";

void Supervision::getUniqueIds() {

  size_t n = 10000;

  std::string path = _agencyPrefix + "/Sync/LatestID";
  auto builder = std::make_shared<Builder>();
  { VPackArrayBuilder a(builder.get());
    { VPackArrayBuilder b(builder.get());
      { VPackObjectBuilder c(builder.get());
        {
          builder->add(VPackValue(path));
          VPackObjectBuilder b(builder.get());
          builder->add("op",VPackValue("increment"));
          builder->add("step",VPackValue(n));
        }
      }
    }
    { VPackArrayBuilder a(builder.get());
      builder->add(VPackValue(path)); }
  } // [[{path:{"op":"increment","step":n}}],[path]]
  
  auto ret = _agent->transact(builder);
  if (ret.accepted) {
    try {
      _jobIdMax = ret.result->slice()[1].get(
        std::vector<std::string>(
          {"arango", "Sync", "LatestID"})).getUInt();
      _jobId = _jobIdMax - n;
    } catch (std::exception const& e) {
      LOG_TOPIC(ERR, Logger::SUPERVISION)
        << "Failed to acquire job IDs from agency: "
        << e.what() << __FILE__ << __LINE__; 
    }
  }
  
}


void Supervision::beginShutdown() {
  // Personal hygiene
  Thread::beginShutdown();

  CONDITION_LOCKER(guard, _cv);
  guard.broadcast();
}


void Supervision::missingPrototype() {

  auto const& plannedDBs = _snapshot(planColPrefix).children();
  //auto available = Job::availableServers(_snapshot);
  
  // key: prototype, value: clone
  //std::multimap<std::string, std::string> likeness;
  
  for (const auto& db_ : plannedDBs) { // Planned databases
    auto const& db = *(db_.second);
    
    for (const auto& col_ : db.children()) { // Planned collections
      auto const& col = *(col_.second);
      
      auto prototype = col("distributeShardsLike").slice().copyString();
      if (prototype.empty()) {
        continue;
      }
      
    }
  }
}

