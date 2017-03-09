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

#include "AddFollower.h"

#include "Agency/AgentInterface.h"
#include "Agency/Job.h"

using namespace arangodb::consensus;

AddFollower::AddFollower(Node const& snapshot, AgentInterface* agent,
                         std::string const& jobId, std::string const& creator,
                         std::string const& database,
                         std::string const& collection,
                         std::string const& shard,
                         std::initializer_list<std::string> const& newFollower)
    : Job(NOTFOUND, snapshot, agent, jobId, creator),
      _database(database),
      _collection(collection),
      _shard(shard),
      _newFollower(newFollower) {}

AddFollower::AddFollower(Node const& snapshot, AgentInterface* agent,
                         std::string const& jobId, std::string const& creator,
                         std::string const& database,
                         std::string const& collection,
                         std::string const& shard,
                         std::vector<std::string> const& newFollower)
    : Job(NOTFOUND, snapshot, agent, jobId, creator),
      _database(database),
      _collection(collection),
      _shard(shard),
      _newFollower(newFollower) {}

AddFollower::AddFollower(Node const& snapshot, AgentInterface* agent,
                         JOB_STATUS status, std::string const& jobId)
    : Job(status, snapshot, agent, jobId) {
  // Get job details from agency:
  try {
    std::string path = pos[status] + _jobId + "/";
    _database = _snapshot(path + "database").getString();
    _collection = _snapshot(path + "collection").getString();
    for (auto const& i :
           VPackArrayIterator(
             _snapshot(path + "newFollower").getArray())) {
      _newFollower.push_back(i.copyString());
    }
    _snapshot(path + "newFollower").getArray();
    _shard = _snapshot(path + "shard").getString();
    _creator = _snapshot(path + "creator").getString();
  } catch (std::exception const& e) {
    std::stringstream err;
    err << "Failed to find job " << _jobId << " in agency: " << e.what();
    LOG_TOPIC(ERR, Logger::AGENCY) << err.str();
    finish("Shards/" + _shard, false, err.str());
    _status = FAILED;
  }
}

AddFollower::~AddFollower() {}

void AddFollower::run() {
  runHelper("Shards/" + _shard);
}

bool AddFollower::create(std::shared_ptr<VPackBuilder> b) {
  LOG_TOPIC(INFO, Logger::AGENCY) << "Todo: AddFollower " << _newFollower
                                  << " to shard " + _shard;

  std::string path, now(timepointToString(std::chrono::system_clock::now()));

  // DBservers
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  std::string curPath =
    curColPrefix + _database + "/" + _collection + "/" + _shard + "/servers";

  Slice current = _snapshot(curPath).slice();

  TRI_ASSERT(current.isArray());
  TRI_ASSERT(current[0].isString());
#endif

  auto const& myClones = clones(_snapshot, _database, _collection, _shard);
  if (myClones.empty() != 1) {
    size_t sub = 0;
    for (auto const& clone : myClones) {
      if (clone.collection != _collection ||
          clone.shard != _shard) {
        AddFollower(_snapshot, _agent, _jobId + "-" + std::to_string(sub++),
                    _jobId, _database, clone.collection,
                    clone.shard, _newFollower);
      }
    }
  }
  
  _jb = std::make_shared<Builder>();
  _jb->openArray();
  _jb->openObject();

  path = toDoPrefix + _jobId;

  // FIXME: create a single larger AddFollower job with many shards

  _jb->add(path, VPackValue(VPackValueType::Object));
  _jb->add("creator", VPackValue(_creator));
  _jb->add("type", VPackValue("addFollower"));
  _jb->add("database", VPackValue(_database));
  _jb->add("collection", VPackValue(_collection));
  _jb->add("shard", VPackValue(_shard));
  _jb->add(VPackValue("newFollower"));
  {
    VPackArrayBuilder b(_jb.get());
    for (auto const& i : _newFollower) {
      _jb->add(VPackValue(i));
    }
  }
  _jb->add("jobId", VPackValue(_jobId));
  _jb->add("timeCreated", VPackValue(now));

  _jb->close();
  _jb->close();
  _jb->close();

  write_ret_t res = transact(_agent, *_jb);

  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    return true;
  }

  LOG_TOPIC(INFO, Logger::AGENCY) << "Failed to insert job " + _jobId;
  return false;
}

bool AddFollower::start() {
  // DBservers
  std::string planPath =
      planColPrefix + _database + "/" + _collection + "/shards/" + _shard;
  std::string curPath =
      curColPrefix + _database + "/" + _collection + "/" + _shard + "/servers";

  Slice current = _snapshot(curPath).slice();
  Slice planned = _snapshot(planPath).slice();

  TRI_ASSERT(current.isArray());
  TRI_ASSERT(planned.isArray());

  for (auto const& srv : VPackArrayIterator(current)) {
    TRI_ASSERT(srv.isString());
    if (srv.copyString() == _newFollower.front()) {
      finish("Shards/" + _shard, false,
             "newFollower must not be already holding the shard.");
      return false;
    }
  }
  for (auto const& srv : VPackArrayIterator(planned)) {
    TRI_ASSERT(srv.isString());
    if (srv.copyString() == _newFollower.front()) {
      finish("Shards/" + _shard, false,
             "newFollower must not be planned for shard already.");
      return false;
    }
  }

  // FIXME: move immediately to finished, since we no longer wait for Current

  // Copy todo to pending
  Builder todo, pending;

  // Get todo entry
  todo.openArray();
  if (_jb == nullptr) {
    try {
      _snapshot(toDoPrefix + _jobId).toBuilder(todo);
    } catch (std::exception const&) {
      LOG_TOPIC(INFO, Logger::AGENCY) << "Failed to get key " + toDoPrefix +
                                             _jobId + " from agency snapshot";
      return false;
    }
  } else {
    todo.add(_jb->slice()[0].valueAt(0));
  }
  todo.close();

  // FIXME: do no longer block toServer

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

  // --- Block shard
  pending.add(blockedShardsPrefix + _shard,
              VPackValue(VPackValueType::Object));
  pending.add("jobId", VPackValue(_jobId));
  pending.close();

  // --- Plan changes
  for (auto const& i : _newFollower) {
    pending.add(planPath, VPackValue(VPackValueType::Object));
    pending.add("op", VPackValue("push"));
    pending.add("new", VPackValue(i));
    pending.close();
  }

  // --- Increment Plan/Version
  pending.add(planVersion, VPackValue(VPackValueType::Object));
  pending.add("op", VPackValue("increment"));
  pending.close();

  pending.close(); // Operations

  // Preconditions
  
  // FIXME: is this check really necessary?
  // --- Check that Current servers are as we expect
  pending.openObject();
  pending.add(curPath, VPackValue(VPackValueType::Object));
  pending.add("old", current);
  pending.close();

  // --- Check that Plan servers are as we expect
  pending.add(planPath, VPackValue(VPackValueType::Object));
  pending.add("old", planned);
  pending.close();

  // --- Check if shard is not blocked
  pending.add(blockedShardsPrefix + _shard,
              VPackValue(VPackValueType::Object));
  pending.add("oldEmpty", VPackValue(true));
  pending.close();

  // FIXME: check also that toServer is not blocked in precondition

  pending.close();
  pending.close();

  // Transact to agency
  write_ret_t res = transact(_agent, pending);

  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    LOG_TOPIC(INFO, Logger::AGENCY)
      << "Pending: Addfollower " << _newFollower << " to shard " << _shard;
    return true;
  }

  LOG_TOPIC(INFO, Logger::AGENCY) << "Start precondition failed for " + _jobId;
  return false;
}

JOB_STATUS AddFollower::status() {
  if (_status != PENDING) {
    return _status;
  }

  // FIXME: delete this check, case PENDING does no longer happen in new spec
 
  std::string curPath = curColPrefix + _database + "/" + _collection + "/" +
                        _shard + "/servers";

  Slice current = _snapshot(curPath).slice();
  for (auto const& srv : VPackArrayIterator(current)) {
    if (srv.copyString() == _newFollower.front()) {
      if (finish("Shards/" + _shard)) {
        return FINISHED;
      }
    }
  }

  return _status;
}

void AddFollower::abort() {
  // FIXME: TO BE IMPLEMENTED
}

