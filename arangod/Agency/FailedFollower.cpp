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

#include "FailedFollower.h"

#include "Agency/Agent.h"
#include "Agency/Job.h"

using namespace arangodb::consensus;

FailedFollower::FailedFollower(Node const& snapshot, AgentInterface* agent,
                               std::string const& jobId,
                               std::string const& creator,
                               std::string const& database,
                               std::string const& collection,
                               std::string const& shard,
                               std::string const& from,
                               std::string const& to)
    : Job(NOTFOUND, snapshot, agent, jobId, creator),
      _database(database),
      _collection(collection),
      _shard(shard),
      _from(from),
      _to(to) {}

FailedFollower::FailedFollower(Node const& snapshot, AgentInterface* agent,
                               JOB_STATUS status, std::string const& jobId)
    : Job(status, snapshot, agent, jobId) {

  // Get job details from agency:
  try {
    std::string path = pos[status] + _jobId;
    _database = _snapshot(path + "database").getString();
    _collection = _snapshot(path + "collection").getString();
    _from = _snapshot(path + "fromServer").getString();
    _to = _snapshot(path + "toServer").getString();
    _shard = _snapshot(path + "shard").getString();
    _creator = _snapshot(path + "creator").slice().copyString();
  } catch (std::exception const& e) {
    std::stringstream err;
    err << "Failed to find job " << _jobId << " in agency: " << e.what();
    LOG_TOPIC(ERR, Logger::AGENCY) << err.str();
    finish("Shards/" + _shard, false, err.str());
    _status = FAILED;
  }
}

FailedFollower::~FailedFollower() {}

void FailedFollower::run() {
  runHelper("Shards/" + _shard);
}

bool FailedFollower::create(std::shared_ptr<VPackBuilder> envelope) {

  using namespace std::chrono;
  
  LOG_TOPIC(INFO, Logger::AGENCY) << "Todo: Handle follower failover for shard "
                                  << _shard << " from " << _from << " to " + _to;

  auto const& myClones = clones(_snapshot, _database, _collection, _shard);
  if (myClones.size() == 1) {   // leader is always in there
    size_t sub = 0;
    for (auto const& clone : myClones) {
      if (clone.collection != _collection || clone.shard != _shard) {
        FailedFollower(
          _snapshot, _agent, _jobId + "-" + std::to_string(sub++), _jobId,
          _database, clone.collection, clone.shard, _from, _to);
      }
    }
  }
   
  _jb = std::make_shared<Builder>();
  { VPackArrayBuilder a(_jb.get());
    // Operation -------------------------------------------------------
    { VPackObjectBuilder oper(_jb.get());
      // Todo entry
      _jb->add(VPackValue(toDoPrefix + _jobId));
      { VPackObjectBuilder td(_jb.get());
        _jb->add("creator", VPackValue(_creator));
        _jb->add("type", VPackValue("failedFollower"));
        _jb->add("database", VPackValue(_database));
        _jb->add("collection", VPackValue(_collection));
        _jb->add("shard", VPackValue(_shard));
        _jb->add("fromServer", VPackValue(_from));
        _jb->add("toServer", VPackValue(_to));
        _jb->add("jobId", VPackValue(_jobId));
        _jb->add("timeCreated",
                 VPackValue(timepointToString(system_clock::now()))); }
      // Add shard to /arango/Target/FailedServers/<server> array
      _jb->add(VPackValue(failedServersPrefix + "/" + _from));
      { VPackObjectBuilder (_jb.get());
        _jb->add("op", VPackValue("push"));
        _jb->add("new", VPackValue(_shard)); }}} // Operations ---------

  write_ret_t res = transact(_agent, *_jb);

  return (res.accepted && res.indices.size() == 1 && res.indices[0]);

}


bool FailedFollower::start() {

  // DBservers
  std::string planPath =
      planColPrefix + _database + "/" + _collection + "/shards/" + _shard;

  Node const& planned = _snapshot(planPath);

  // Copy todo to pending
  Builder todo;
  { VPackArrayBuilder a(&todo);
    if (_jb == nullptr) {
      try {
        _snapshot(toDoPrefix + _jobId).toBuilder(todo);
      } catch (std::exception const&) {
        LOG_TOPIC(INFO, Logger::AGENCY)
          << "Failed to get key " + toDoPrefix + _jobId + " from agency snapshot";
        return false;
      }
    } else {
    todo.add(_jb->slice().get(toDoPrefix + _jobId).valueAt(0));
    }}
  
  // FIXME: move to finished right away
  // FIXME: also handle multiple collections and shards at the same time

  // Transaction
  Builder pending;
  pending.openArray();

  // Apply
  // --- Add pending entry
  pending.openObject();
  pending.add(pendingPrefix + _jobId,
              VPackValue(VPackValueType::Object));
  pending.add("timeStarted",
              VPackValue(timepointToString(std::chrono::system_clock::now())));
  for (auto const& obj : VPackObjectIterator(todo.slice()[0])) {
    pending.add(obj.key.copyString(), obj.value);
  }
  pending.close();

  // --- Remove todo entry
  pending.add(toDoPrefix + _jobId,
              VPackValue(VPackValueType::Object));
  pending.add("op", VPackValue("delete"));
  pending.close();

  // --- Add new server to the list
  pending.add(planPath, VPackValue(VPackValueType::Array));
  for(const auto& i : VPackArrayIterator(planned.slice())) {
    if (i.copyString() != _from) {
      pending.add(i);
    } else {
      pending.add(VPackValue(_to));
    }
  }
  
  pending.close();

  // --- Block shard
  pending.add(blockedShardsPrefix + _shard,
              VPackValue(VPackValueType::Object));
  pending.add("jobId", VPackValue(_jobId));
  pending.close();

  // --- Increment Plan/Version
  pending.add(planVersion, VPackValue(VPackValueType::Object));
  pending.add("op", VPackValue("increment"));
  pending.close();

  pending.close();

  // Preconditions
  pending.openObject();
  
  // --- Check if shard is not blocked by other job
  pending.add(blockedShardsPrefix + _shard,
              VPackValue(VPackValueType::Object));
  pending.add("oldEmpty", VPackValue(true));
  pending.close();

  // FIXME: add precondition that the shards are not blocked and that
  // FIXME: the fromServer is still "FAILED", do nothing if precondition
  // FIXME: not fulfilled

  pending.close();
  pending.close();

  // Transact
  write_ret_t res = transact(_agent, pending);

  if (res.accepted && res.indices.size() == 1 && res.indices[0]) {
    LOG_TOPIC(INFO, Logger::AGENCY)
      << "Pending: Change followership " + _shard + " from " + _from + " to " + _to;
    return true;
  }

  LOG_TOPIC(INFO, Logger::AGENCY)
      << "Precondition failed for starting job " + _jobId;
  return false;
}

JOB_STATUS FailedFollower::status() {
  if (_status != PENDING) {
    return _status;
  }
  // FIXME: this is no more needed because all was done in start()
  // FIXME: do not wait for in sync any more, so status == PENDING
  // FIXME: has nothing to do any more
  // FIXME: do we need a list of shards in FailedServers???

  Node const& job = _snapshot(pendingPrefix + _jobId);
  std::string database = job("database").toJson(),
              collection = job("collection").toJson(),
              shard = job("shard").toJson();

  std::string planPath = planColPrefix + database + "/" + collection +
                         "/shards/" + shard,
              curPath = curColPrefix + database + "/" + collection + "/" +
                        shard + "/servers";

  Node const& planned = _snapshot(planPath);
  Node const& current = _snapshot(curPath);

  if (compareServerLists(planned.slice(), current.slice())) {
    // Remove shard from /arango/Target/FailedServers/<server> array
    Builder del;
    del.openArray();
    del.openObject();
    std::string path = failedServersPrefix + "/" + _from;
    del.add(path, VPackValue(VPackValueType::Object));
    del.add("op", VPackValue("erase"));
    del.add("val", VPackValue(_shard));
    del.close();
    del.close();
    del.close();
    write_ret_t res = transact(_agent, del);

    if (finish("Shards/" + shard)) {
      return FINISHED;
    }
  }

  return _status;
}

void FailedFollower::abort() {
  // FIXME: TO BE IMPLEMENTED
}

