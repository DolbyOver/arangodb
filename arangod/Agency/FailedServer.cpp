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

#include "FailedServer.h"
#include "JobContext.h"

#include "Agency/AgentInterface.h"
#include "Agency/FailedLeader.h"
#include "Agency/FailedFollower.h"
#include "Agency/Job.h"
#include "Agency/UnassumedLeadership.h"

using namespace arangodb::consensus;

FailedServer::FailedServer(Node const& snapshot, AgentInterface* agent,
                           std::string const& jobId, std::string const& creator,
                           std::string const& server)
    : Job(NOTFOUND, snapshot, agent, jobId, creator), _server(server) {}

FailedServer::FailedServer(Node const& snapshot, AgentInterface* agent,
                           JOB_STATUS status, std::string const& jobId)
    : Job(status, snapshot, agent, jobId) {
  // Get job details from jobId:
  try {
    std::string path = pos[status] + _jobId + "/";
    _server = _snapshot(path + "server").getString();
    _creator = _snapshot(path + "creator").getString();
  } catch (std::exception const& e) {
    std::stringstream err;
    err << "Failed to find job " << _jobId << " in agency: " << e.what();
    LOG_TOPIC(ERR, Logger::AGENCY) << err.str();
    finish("DBServers/" + _server, false, err.str());
    _status = FAILED;
  }
}

FailedServer::~FailedServer() {}

void FailedServer::run() {
  runHelper("DBServers/" + _server);
}

bool FailedServer::start() {
  
  using namespace std::chrono;

  // Fail job, if Health back to not FAILED
  if (_snapshot(healthPrefix + _server + "/Status").getString() != "FAILED") {
    std::stringstream reason;
    reason
      << "Server " << _server
      << " is no longer failed. Not starting FailedServer job";
    LOG_TOPIC(INFO, Logger::AGENCY) << reason.str();
    finish("DBServers/" + _server, false, reason.str());
    return false;
  }
  
  // Abort job blocking server if abortable
  try {
     std::string jobId = _snapshot(blockedServersPrefix + _server).getString();
    if (!abortable(_snapshot, jobId)) {
      return false;
    } else {
      JobContext(PENDING, jobId, _snapshot, _agent).abort();
    }
  } catch (...) {}
    
  // Todo entry
  Builder todo;
  { VPackArrayBuilder t(&todo);
    if (_jb == nullptr) {
      try {
        _snapshot(toDoPrefix + _jobId).toBuilder(todo);
      } catch (std::exception const&) {
        LOG_TOPIC(INFO, Logger::AGENCY)
          << "Failed to get key " + toDoPrefix + _jobId + " from agency snapshot";
        return false;
      }
    } else {
      todo.add(_jb->slice()[0].get(toDoPrefix + _jobId));
    }} // Todo entry
  
  // Pending entry
  Builder pending;
  { VPackArrayBuilder a(&pending);
    
    // Operations -------------->
    { VPackObjectBuilder oper(&pending); 
      // Add pending
      pending.add(VPackValue(pendingPrefix + _jobId));
      { VPackObjectBuilder ts(&pending);
        pending.add("timeStarted",
                    VPackValue(timepointToString(system_clock::now())));
        for (auto const& obj : VPackObjectIterator(todo.slice()[0])) {
          pending.add(obj.key.copyString(), obj.value);
        }
      }
      // Delete todo
      pending.add(VPackValue(toDoPrefix + _jobId));
      { VPackObjectBuilder del(&pending);
        pending.add("op", VPackValue("delete")); }
      // Block toServer
      pending.add(VPackValue(blockedServersPrefix + _server));
      { VPackObjectBuilder block(&pending);
        pending.add("jobId", VPackValue(_jobId));
      }
    } // <------------ Operations
    
    // Preconditions ----------->
    { VPackObjectBuilder prec(&pending);
      // Check that toServer not blocked
      pending.add(VPackValue(blockedServersPrefix + _server));
      { VPackObjectBuilder block(&pending);
        pending.add("oldEmpty", VPackValue(true));
      }
    } // <--------- Preconditions
  }

  
  // Transact to agency
  write_ret_t res = transact(_agent, pending);

  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    LOG_TOPIC(DEBUG, Logger::AGENCY)
      << "Pending job for failed DB Server " << _server;

    auto const& databases = _snapshot("/Plan/Collections").children();
    auto const& current = _snapshot("/Current/Collections").children();

    size_t sub = 0;

    // FIXME: looks OK, but only the non-clone shards are put into the job
    for (auto const& database : databases) {
      auto cdatabase = current.at(database.first)->children();

      for (auto const& collptr : database.second->children()) {
        auto const& collection = *(collptr.second);
        
        if (!cdatabase.find(collptr.first)->second->children().empty()) {

          auto const& collection = *(collptr.second);
          auto const& replicationFactor = collection("replicationFactor");

          if (replicationFactor.slice().getUInt() > 1) {

            bool isClone = false;
            try { // Clone
              if(!collection("distributeShardsLike").slice().copyString().empty()) {
                isClone = true;
              }
            } catch (...) {} // Not clone
            
            auto available = availableServers(_snapshot);
              
            for (auto const& shard : collection("shards").children()) {

              size_t pos = 0;
              bool found = false;
              
              for (auto const& it : VPackArrayIterator(shard.second->slice())) {

                auto dbs = it.copyString();

                available.erase(
                  std::remove(available.begin(), available.end(), dbs),
                  available.end());

                if (dbs == _server) {
                  if (pos == 0) {
                    FailedLeader(
                      _snapshot, _agent, _jobId + "-" + std::to_string(sub++),
                      _jobId, database.first, collptr.first,
                      shard.first, _server,
                      shard.second->slice()[1].copyString()).run();
                    continue;
                  } else {
                    found = true;
                  }
                }
                
                ++pos;
              }

              if (found && !available.empty() && !isClone) {
                auto randIt = available.begin();
                std::advance(randIt, std::rand() % available.size());
                FailedFollower(
                  _snapshot, _agent, _jobId + "-" + std::to_string(sub++),
                  _jobId, database.first, collptr.first,
                  shard.first, _server, *randIt).run();
              }
            }
          }
        } else {
          for (auto const& shard : collection("shards").children()) {
            UnassumedLeadership(
              _snapshot, _agent, _jobId + "-" + std::to_string(sub++), _jobId,
              database.first, collptr.first, shard.first, _server).run();
          }
        }
      }
    }

    return true;
  }

  LOG_TOPIC(INFO, Logger::AGENCY)
      << "Precondition failed for starting job " + _jobId;

  return false;
}

bool FailedServer::create(std::shared_ptr<VPackBuilder> envelope) {

  LOG_TOPIC(DEBUG, Logger::AGENCY)
    << "Todo: Handle failover for db server " + _server;

  LOG_TOPIC(INFO, Logger::AGENCY) << __FILE__ << __LINE__;
  
  using namespace std::chrono;
  bool selfCreate = (envelope == nullptr); // Do we create ourselves?

  if (selfCreate) {
    _jb = std::make_shared<Builder>();
  } else {
    _jb = envelope;
  }

  { VPackArrayBuilder a(_jb.get());

    // Operations
    { VPackObjectBuilder operations (_jb.get());
      
      // ToDo entry
      _jb->add(VPackValue(toDoPrefix + _jobId));
      { VPackObjectBuilder todo(_jb.get());
        _jb->add("type", VPackValue("failedServer"));
        _jb->add("server", VPackValue(_server));
        _jb->add("jobId", VPackValue(_jobId));
        _jb->add("creator", VPackValue(_creator));
        _jb->add("timeCreated", VPackValue(timepointToString(
                                             system_clock::now()))); } 
      // FailedServers entry []
      _jb->add(VPackValue(failedServersPrefix + "/" + _server));
      { VPackArrayBuilder failedServers(_jb.get()); }} // Operations

    //Preconditions
    { VPackObjectBuilder health(_jb.get());

      // Status should still be FAILED
      _jb->add(VPackValue(healthPrefix + _server + "/Status"));
      { VPackObjectBuilder old(_jb.get());
        _jb->add("old", VPackValue("BAD")); }

      // Target/FailedServers is still as in the snapshot
      _jb->add(VPackValue(failedServersPrefix));
      { VPackObjectBuilder old(_jb.get());
        _jb->add("old", _snapshot(failedServersPrefix).toBuilder().slice());
      }} // Preconditions
  }

  if (selfCreate) {
    write_ret_t res = transact(_agent, *_jb);
    if (!res.accepted || res.indices.size() != 1 || res.indices[0] == 0) {
      LOG_TOPIC(INFO, Logger::AGENCY) << "Failed to insert job " + _jobId;
      return false;
    }
  }

  return true;

}

JOB_STATUS FailedServer::status() {
  if (_status != PENDING) {
    return _status;
  }

  auto const& serverHealth =
    _snapshot(healthPrefix + _server + "/Status").getString();

  // mop: ohhh...server is healthy again!
  bool serverHealthy = serverHealth == Supervision::HEALTH_STATUS_GOOD;

  std::shared_ptr<Builder> deleteTodos;

  Node::Children const todos = _snapshot(toDoPrefix).children();
  Node::Children const pends = _snapshot(pendingPrefix).children();
  bool hasOpenChildTasks = false;

  for (auto const& subJob : todos) {
    if (!subJob.first.compare(0, _jobId.size() + 1, _jobId + "-")) {
      if (serverHealthy) {
        if (!deleteTodos) {
          deleteTodos.reset(new Builder());
          deleteTodos->openArray();
          deleteTodos->openObject();
        }
        deleteTodos->add( toDoPrefix + subJob.first,
          VPackValue(VPackValueType::Object));
        deleteTodos->add("op", VPackValue("delete"));
        deleteTodos->close();
      } else {
        hasOpenChildTasks = true;
      }
    }
  }

  for (auto const& subJob : pends) {
    if (!subJob.first.compare(0, _jobId.size() + 1, _jobId + "-")) {
      hasOpenChildTasks = true;
    }
  }

  // FIXME: sub-jobs should terminate themselves if server "GOOD" again
  // FIXME: thus the deleteTodos here is unnecessary

  if (deleteTodos) {
    LOG_TOPIC(INFO, Logger::AGENCY)
      << "Server " << _server << " is healthy again. Will try to delete"
      "any jobs which have not yet started!";
    deleteTodos->close();
    deleteTodos->close();
    // Transact to agency
    write_ret_t res = transact(_agent, *deleteTodos);

    if (!res.accepted || res.indices.size() != 1 || !res.indices[0]) {
      LOG_TOPIC(WARN, Logger::AGENCY)
        << "Server was healthy. Tried deleting subjobs but failed :(";
      return _status;
    }
  }

  // FIXME: what if some subjobs have failed, we should fail then
  if (!hasOpenChildTasks) {
    if (finish("DBServers/" + _server)) {
      return FINISHED;
    }
  }

  return _status;
}

void FailedServer::abort() {
  // FIXME: No abort procedure, simply throw error or so
}

