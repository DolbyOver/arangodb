////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Andreas Streichardt
////////////////////////////////////////////////////////////////////////////////

#include "RemoveServer.h"

#include "Agency/AddFollower.h"
#include "Agency/Agent.h"
#include "Agency/Job.h"

using namespace arangodb::consensus;

RemoveServer::RemoveServer(Node const& snapshot, AgentInterface* agent,
                           std::string const& jobId, std::string const& creator,
                           std::string const& server)
    : Job(NOTFOUND, snapshot, agent, jobId, creator), _server(server) {}

RemoveServer::RemoveServer(Node const& snapshot, AgentInterface* agent,
                           JOB_STATUS status, std::string const& jobId)
    : Job(status, snapshot, agent, jobId) {
  // Get job details from agency:
  try {
    std::string path = pos[status] + _jobId + "/";
    _server = _snapshot(path + "server").getString();
    _creator = _snapshot(path + "creator").getString();
  } catch (std::exception const& e) {
    std::stringstream err;
    err << "Failed to find job " << _jobId << " in agency: " << e.what();
    LOG_TOPIC(ERR, Logger::SUPERVISION) << err.str();
    finish(_server, "", false, err.str());
    _status = FAILED;
  }
}

RemoveServer::~RemoveServer() {}

void RemoveServer::run() {
  runHelper(_server, "");
}

JOB_STATUS RemoveServer::status() {
  if (_status != PENDING) {
    return _status;
  }

  Node::Children const todos = _snapshot(toDoPrefix).children();
  Node::Children const pends = _snapshot(pendingPrefix).children();

  size_t found = 0;
  for (auto const& subJob : todos) {
    if (!subJob.first.compare(0, _jobId.size() + 1, _jobId + "-")) {
      found++;
    }
  }
  for (auto const& subJob : pends) {
    if (!subJob.first.compare(0, _jobId.size() + 1, _jobId + "-")) {
      found++;
    }
  }

  if (found > 0) {
    // mop: TODO check if the server has reappeared and abort shard moving if
    // server has reappeared
    return _status;
  }

  // mop: all addfollower jobs have been finished. Forcefully remove the
  // server from everything
  Node::Children const& planDatabases =
      _snapshot("/Plan/Collections").children();

  Builder desiredPlanState;
  Builder preconditions;

  desiredPlanState.openObject();
  preconditions.openObject();

  Builder trx;
  trx.openArray();
  trx.openObject();
  for (auto const& database : planDatabases) {
    for (auto const& collptr : database.second->children()) {
      Node const& collection = *(collptr.second);
      for (auto const& shard : collection("shards").children()) {
        VPackArrayIterator dbsit(shard.second->slice());

        bool found = false;
        Builder desiredServers;
        desiredServers.openArray();
        // Only shards, which are affected
        for (auto const& dbserver : dbsit) {
          std::string server = dbserver.copyString();
          if (server != _server) {
            desiredServers.add(VPackValue(server));
          } else {
            found = true;
          }
        }
        desiredServers.close();
        if (found == false) {
          continue;
        }

        std::string const& key("/Plan/Collections/" +
                               database.first + "/" + collptr.first +
                               "/shards/" + shard.first);

        trx.add(key, desiredServers.slice());
        preconditions.add(VPackValue(key));
        preconditions.openObject();
        preconditions.add("old", shard.second->slice());
        preconditions.close();
      }
    }
  }
  preconditions.close();

  trx.add(VPackValue("/Target/CleanedServers"));
  trx.openObject();
  trx.add("op", VPackValue("push"));
  trx.add("new", VPackValue(_server));
  trx.close();
  trx.add(VPackValue(planVersion));
  trx.openObject();
  trx.add("op", VPackValue("increment"));
  trx.close();

  trx.close();
  trx.add(preconditions.slice());
  trx.close();

  // Transact to agency
  write_ret_t res = transact(_agent, trx);

  if (res.accepted && res.indices.size() == 1 && res.indices[0] != 0) {
    LOG_TOPIC(INFO, Logger::SUPERVISION) << "Have reported " << _server
                                    << " in /Target/CleanedServers";
    if (finish(_server, "")) {
      return FINISHED;
    }
    return _status;
  }

  return _status;
}

// Only through shrink cluster
bool RemoveServer::create(std::shared_ptr<VPackBuilder> b) {  

  LOG_TOPIC(INFO, Logger::SUPERVISION) << "Todo: Remove server " + _server;

  std::string path = toDoPrefix + _jobId;

  _jb = std::make_shared<Builder>();
  _jb->openArray();
  _jb->openObject();
  _jb->add(path, VPackValue(VPackValueType::Object));
  _jb->add("type", VPackValue("removeServer"));
  _jb->add("server", VPackValue(_server));
  _jb->add("jobId", VPackValue(_jobId));
  _jb->add("creator", VPackValue(_creator));
  _jb->add("timeCreated",
           VPackValue(timepointToString(std::chrono::system_clock::now())));
  _jb->close();
  _jb->close();
  _jb->close();

  write_ret_t res = transact(_agent, *_jb);

  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    return true;
  }

  LOG_TOPIC(INFO, Logger::SUPERVISION) << "Failed to insert job " + _jobId;
  return false;
}

bool RemoveServer::start() {
  // Copy todo to pending
  Builder todo, pending;

  // Get todo entry
  todo.openArray();
  if (_jb == nullptr) {
    try {
      _snapshot(toDoPrefix + _jobId).toBuilder(todo);
    } catch (std::exception const&) {
      LOG_TOPIC(INFO, Logger::SUPERVISION)
        << "Failed to get key " + toDoPrefix + _jobId + " from agency snapshot";
      return false;
    }
  } else {
    todo.add(_jb->slice()[0].get(toDoPrefix + _jobId));
  }
  todo.close();

  // Enter pending, remove todo, block toserver
  pending.openArray();

  // --- Add pending
  pending.openObject();
  pending.add(pendingPrefix + _jobId,
              VPackValue(VPackValueType::Object));
  pending.add("timeStarted",
              VPackValue(timepointToString(std::chrono::system_clock::now())));
  for (auto const& obj : VPackObjectIterator(todo.slice()[0])) {
    pending.add(obj.key.copyString(), obj.value);
  }
  pending.close();

  // --- Delete todo
  pending.add(toDoPrefix + _jobId,
              VPackValue(VPackValueType::Object));
  pending.add("op", VPackValue("delete"));
  pending.close();

  // --- Block _server
  pending.add(blockedServersPrefix + _server,
              VPackValue(VPackValueType::Object));
  pending.add("jobId", VPackValue(_jobId));
  pending.close();

  pending.close();

  // Preconditions
  // --- Check that toServer not blocked
  pending.openObject();
  pending.add(blockedServersPrefix + _server,
              VPackValue(VPackValueType::Object));
  pending.add("oldEmpty", VPackValue(true));
  pending.close();

  pending.close();
  pending.close();

  // Transact to agency
  write_ret_t res = transact(_agent, pending);

  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    LOG_TOPIC(INFO, Logger::SUPERVISION) << "Pending: Removing server " + _server;

    // Check if we can get things done in the first place
    if (!checkFeasibility()) {
      finish(_server, "", false, "job not feasible");
      return false;
    }

    // Schedule shard relocations
    if (!scheduleAddFollowers()) {
      finish(_server, "", false, "Could not schedule add followers.");
      return false;
    }

    return true;
  }

  LOG_TOPIC(INFO, Logger::SUPERVISION)
      << "Precondition failed for starting job " + _jobId;

  return false;
}

bool RemoveServer::scheduleAddFollowers() {

  std::vector<std::string> servers = availableServers(_snapshot);

  // Minimum 1 DB server must remain
  if (servers.size() == 1) {
    LOG_TOPIC(ERR, Logger::SUPERVISION) << "DB server " << _server
                                   << " is the last standing db server.";
    return false;
  }

  Node::Children const& databases = _snapshot("/Plan/Collections").children();

  size_t sub = 0;
  for (auto const& database : databases) {

    for (auto const& collptr : database.second->children()) {

      Node const& collection = *(collptr.second);

      try { // distributeShardsLike entry means we only follow
        if (collection("distributeShardsLike").slice().copyString() != "") {
          continue;
        }
      } catch (...) {}

      uint64_t replFactor = collection("replicationFactor").getUInt();
      Node::Children const& shards = collection("shards").children();

      // mop: special case..we already have at least one more follower than we
      // should have...
      // we could simply kill the server now...
      // probably another removeserver job has failed previously
      if (shards.size() > replFactor) {
        continue;
      }
      
      for (auto const& shard : shards) {
        bool found = false;
        VPackArrayIterator dbsit(shard.second->slice());

        // Only shards, which are affected
        for (auto const& dbserver : dbsit) {
          std::string server = dbserver.copyString();
          if (server == _server) {
            found = true;
          }
        }
        if (!found) {
          continue;
        }

        // Only destinations, which are not already holding this shard
        for (auto const& dbserver : dbsit) {
          servers.erase(
            std::remove(servers.begin(), servers.end(), dbserver.copyString()),
            servers.end());
        }

        // Among those a random destination
        std::string newServer;
        if (servers.empty()) {
          LOG_TOPIC(ERR, Logger::SUPERVISION)
            << "No servers remain as target for RemoveServer";
          return false;
        }

        newServer = servers.at(rand() % servers.size());

#warning temporarily disabled
        // AddFollower(_snapshot, _agent, _jobId + "-" + std::to_string(sub++),
        //             _jobId, database.first, collptr.first,
        //             shard.first, {newServer});
      }
    }
  }

  return true;
}

bool RemoveServer::checkFeasibility() {
  // Server exists
  if (_snapshot.exists("/Plan/DBServers/" + _server).size() != 3) {
    LOG_TOPIC(ERR, Logger::SUPERVISION) << "No db server with id " << _server
                                   << " in plan.";
    return false;
  }

  // Server has not been cleaned already
  if (_snapshot.exists("/Target/CleanedServers").size() == 2) {
    for (auto const& srv :
         VPackArrayIterator(_snapshot("/Target/CleanedServers").slice())) {
      if (srv.copyString() == _server) {
        LOG_TOPIC(ERR, Logger::SUPERVISION) << _server
                                       << " has been cleaned out already!";
        return false;
      }
    }
  }

  std::vector<std::string> availServers;

  // Get servers from plan
  Node::Children const& dbservers = _snapshot("/Plan/DBServers").children();
  for (auto const& srv : dbservers) {
    availServers.push_back(srv.first);
  }

  // Remove cleaned from ist
  if (_snapshot.exists("/Target/CleanedServers").size() == 2) {
    for (auto const& srv :
         VPackArrayIterator(_snapshot("/Target/CleanedServers").slice())) {
      availServers.erase(std::remove(availServers.begin(), availServers.end(),
                                     srv.copyString()),
                         availServers.end());
    }
  }

  // Minimum 1 DB server must remain
  if (availServers.size() == 1) {
    LOG_TOPIC(ERR, Logger::SUPERVISION) << "DB server " << _server
                                   << " is the last standing db server.";
    return false;
  }

  // Remaning after clean out
  uint64_t numRemaining = availServers.size() - 1;

  // Find conflictings collections
  uint64_t maxReplFact = 1;
  std::vector<std::string> tooLargeCollections;
  std::vector<uint64_t> tooLargeFactors;
  Node::Children const& databases = _snapshot("/Plan/Collections").children();
  for (auto const& database : databases) {
    for (auto const& collptr : database.second->children()) {
      try {
        uint64_t replFact = (*collptr.second)("replicationFactor").getUInt();
        if (replFact > numRemaining) {
          tooLargeCollections.push_back(collptr.first);
          tooLargeFactors.push_back(replFact);
        }
        if (replFact > maxReplFact) {
          maxReplFact = replFact;
        }
      } catch (...) {
      }
    }
  }

  // Report if problems exist
  if (maxReplFact > numRemaining) {
    std::stringstream collections;
    std::stringstream factors;

    for (auto const collection : tooLargeCollections) {
      collections << collection << " ";
    }
    for (auto const factor : tooLargeFactors) {
      factors << std::to_string(factor) << " ";
    }

    LOG_TOPIC(ERR, Logger::SUPERVISION)
        << "Cannot accomodate shards " << collections.str()
        << "with replication factors " << factors.str()
        << "after cleaning out server " << _server;
    return false;
  }

  return true;
}

void RemoveServer::abort() {
  // TO BE IMPLEMENTED
}

