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
/// @author Michael Hackstein
/// @author Daniel H. Larkin
////////////////////////////////////////////////////////////////////////////////

#include "LogicalCollection.h"

#include "Aql/QueryCache.h"
#include "Basics/LocalTaskQueue.h"
#include "Basics/ReadLocker.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Basics/Timers.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/encoding.h"
#include "Basics/process-utils.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ClusterMethods.h"
#include "Cluster/FollowerInfo.h"
#include "Cluster/ServerState.h"
#include "Indexes/Index.h"
#include "RestServer/DatabaseFeature.h"
#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "MMFiles/MMFilesDocumentOperation.h"
#include "MMFiles/MMFilesCollection.h" //remove
#include "MMFiles/MMFilesPrimaryIndex.h"
#include "MMFiles/MMFilesIndexElement.h"
#include "MMFiles/MMFilesToken.h"
#include "MMFiles/MMFilesTransactionState.h"
#include "MMFiles/MMFilesWalMarker.h" //crud marker -- TODO remove
#include "MMFiles/MMFilesWalSlots.h"  //TODO -- remove
#include "StorageEngine/StorageEngine.h"
#include "StorageEngine/TransactionState.h"
#include "Transaction/Helpers.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/CollectionReadLocker.h"
#include "Utils/CollectionWriteLocker.h"
#include "Utils/Events.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Utils/StandaloneTransactionContext.h"
#include "VocBase/DatafileStatisticsContainer.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/ManagedDocumentResult.h"
#include "VocBase/PhysicalCollection.h"
#include "VocBase/ticks.h"

#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using Helper = arangodb::basics::VelocyPackHelper;

/// @brief helper class for filling indexes
class IndexFillerTask : public basics::LocalTask {
 public:
  IndexFillerTask(
      basics::LocalTaskQueue* queue, transaction::Methods* trx,
      Index* idx,
      std::vector<std::pair<TRI_voc_rid_t, VPackSlice>> const& documents)
      : LocalTask(queue), _trx(trx), _idx(idx), _documents(documents) {}

  void run() {
    TRI_ASSERT(_idx->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);

    try {
      _idx->batchInsert(_trx, _documents, _queue);
    } catch (std::exception const&) {
      _queue->setStatus(TRI_ERROR_INTERNAL);
    }

    _queue->join();
  }

 private:
  transaction::Methods* _trx;
  Index* _idx;
  std::vector<std::pair<TRI_voc_rid_t, VPackSlice>> const& _documents;
};

namespace {

template <typename T>
static T ReadNumericValue(VPackSlice info, std::string const& name, T def) {
  if (!info.isObject()) {
    return def;
  }
  return Helper::getNumericValue<T>(info, name.c_str(), def);
}

template <typename T, typename BaseType>
static T ReadNumericValue(VPackSlice info, std::string const& name, T def) {
  if (!info.isObject()) {
    return def;
  }
  // nice extra conversion required for Visual Studio pickyness
  return static_cast<T>(Helper::getNumericValue<BaseType>(
      info, name.c_str(), static_cast<BaseType>(def)));
}

static bool ReadBooleanValue(VPackSlice info, std::string const& name,
                             bool def) {
  if (!info.isObject()) {
    return def;
  }
  return Helper::getBooleanValue(info, name.c_str(), def);
}

static TRI_voc_cid_t ReadCid(VPackSlice info) {
  if (!info.isObject()) {
    // ERROR CASE
    return 0;
  }

  // Somehow the cid is now propagated to dbservers
  TRI_voc_cid_t cid = Helper::extractIdValue(info);

  if (cid == 0) {
    if (ServerState::instance()->isDBServer()) {
      cid = ClusterInfo::instance()->uniqid(1);
    } else if (ServerState::instance()->isCoordinator()) {
      cid = ClusterInfo::instance()->uniqid(1);
    } else {
      cid = TRI_NewTickServer();
    }
  }
  return cid;
}

static TRI_voc_cid_t ReadPlanId(VPackSlice info, TRI_voc_cid_t cid) {
  if (!info.isObject()) {
    // ERROR CASE
    return 0;
  }
  VPackSlice id = info.get("planId");
  if (id.isNone()) {
    return cid;
  }

  if (id.isString()) {
    // string cid, e.g. "9988488"
    return arangodb::basics::StringUtils::uint64(id.copyString());
  } else if (id.isNumber()) {
    // numeric cid, e.g. 9988488
    return id.getNumericValue<uint64_t>();
  }
  // TODO Throw error for invalid type?
  return cid;
}

static std::string const ReadStringValue(VPackSlice info,
                                         std::string const& name,
                                         std::string const& def) {
  if (!info.isObject()) {
    return def;
  }
  return Helper::getStringValue(info, name, def);
}

static std::shared_ptr<arangodb::velocypack::Buffer<uint8_t> const>
CopySliceValue(VPackSlice info, std::string const& name) {
  if (!info.isObject()) {
    return nullptr;
  }
  info = info.get(name);
  if (info.isNone()) {
    return nullptr;
  }
  return VPackBuilder::clone(info).steal();
}
}

/// @brief This the "copy" constructor used in the cluster
///        it is required to create objects that survive plan
///        modifications and can be freed
///        Can only be given to V8, cannot be used for functionality.
LogicalCollection::LogicalCollection(LogicalCollection const& other)
    : _internalVersion(0),
      _cid(other.cid()),
      _planId(other.planId()),
      _type(other.type()),
      _name(other.name()),
      _distributeShardsLike(other.distributeShardsLike()),
      _avoidServers(other.avoidServers()),
      _isSmart(other.isSmart()),
      _status(other.status()),
      _isLocal(false),
      _isDeleted(other._isDeleted),
      _doCompact(other.doCompact()),
      _isSystem(other.isSystem()),
      _isVolatile(other.isVolatile()),
      _waitForSync(other.waitForSync()),
      _journalSize(static_cast<TRI_voc_size_t>(other.journalSize())),
      _keyOptions(other._keyOptions),
      _version(other._version),
      _indexBuckets(other.indexBuckets()),
      _indexes(),
      _replicationFactor(other.replicationFactor()),
      _numberOfShards(other.numberOfShards()),
      _allowUserKeys(other.allowUserKeys()),
      _shardIds(new ShardMap()),  // Not needed
      _vocbase(other.vocbase()),
      _cleanupIndexes(0),
      _persistentIndexes(0),
      _physical(EngineSelectorFeature::ENGINE->createPhysicalCollection(this)),
      _useSecondaryIndexes(true),
      _maxTick(0),
      _keyGenerator(),
      _isInitialIteration(false),
      _revisionError(false) {
  _keyGenerator.reset(KeyGenerator::factory(other.keyOptions()));

  if (ServerState::instance()->isDBServer() ||
      !ServerState::instance()->isRunningInCluster()) {
    _followers.reset(new FollowerInfo(this));
  }

  // Copy over index definitions
  _indexes.reserve(other._indexes.size());
  for (auto const& idx : other._indexes) {
    _indexes.emplace_back(idx);
  }

}

// @brief Constructor used in coordinator case.
// The Slice contains the part of the plan that
// is relevant for this collection.
LogicalCollection::LogicalCollection(TRI_vocbase_t* vocbase,
                                     VPackSlice const& info, bool isPhysical)
    : _internalVersion(0),
      _cid(ReadCid(info)),
      _planId(ReadPlanId(info, _cid)),
      _type(ReadNumericValue<TRI_col_type_e, int>(info, "type",
                                                  TRI_COL_TYPE_UNKNOWN)),
      _name(ReadStringValue(info, "name", "")),
      _distributeShardsLike(ReadStringValue(info, "distributeShardsLike", "")),
      _isSmart(ReadBooleanValue(info, "isSmart", false)),
      _status(ReadNumericValue<TRI_vocbase_col_status_e, int>(
          info, "status", TRI_VOC_COL_STATUS_CORRUPTED)),
      _isLocal(!ServerState::instance()->isCoordinator()),
      _isDeleted(ReadBooleanValue(info, "deleted", false)),
      _doCompact(ReadBooleanValue(info, "doCompact", true)),
      _isSystem(IsSystemName(_name) &&
                ReadBooleanValue(info, "isSystem", false)),
      _isVolatile(ReadBooleanValue(info, "isVolatile", false)),
      _waitForSync(ReadBooleanValue(info, "waitForSync", false)),
      _journalSize(ReadNumericValue<TRI_voc_size_t>(
          info, "maximalSize",  // Backwards compatibility. Agency uses
                                // journalSize. paramters.json uses maximalSize
          ReadNumericValue<TRI_voc_size_t>(info, "journalSize",
                                           TRI_JOURNAL_DEFAULT_SIZE))),
      _keyOptions(CopySliceValue(info, "keyOptions")),
      _version(ReadNumericValue<uint32_t>(info, "version", currentVersion())),
      _indexBuckets(ReadNumericValue<uint32_t>(
          info, "indexBuckets", DatabaseFeature::defaultIndexBuckets())),
      _replicationFactor(1),
      _numberOfShards(ReadNumericValue<size_t>(info, "numberOfShards", 1)),
      _allowUserKeys(ReadBooleanValue(info, "allowUserKeys", true)),
      _shardIds(new ShardMap()),
      _vocbase(vocbase),
      _cleanupIndexes(0),
      _persistentIndexes(0),
      _physical(EngineSelectorFeature::ENGINE->createPhysicalCollection(this)),
      _useSecondaryIndexes(true),
      _maxTick(0),
      _keyGenerator(),
      _isInitialIteration(false),
      _revisionError(false) {
  getPhysical()->setPath(ReadStringValue(info, "path", ""));
  if (!IsAllowedName(info)) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_ILLEGAL_NAME);
  }

  if (_version < minimumVersion()) {
    // collection is too "old"
    std::string errorMsg(std::string("collection '") + _name +
                         "' has a too old version. Please start the server "
                         "with the --database.auto-upgrade option.");

    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_FAILED, errorMsg);
  }

  if (_isVolatile && _waitForSync) {
    // Illegal collection configuration
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "volatile collections do not support the waitForSync option");
  }

  if (_journalSize < TRI_JOURNAL_MINIMAL_SIZE) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "<properties>.journalSize too small");
  }

  VPackSlice shardKeysSlice = info.get("shardKeys");

  bool const isCluster = ServerState::instance()->isRunningInCluster();
  // Cluster only tests
  if (ServerState::instance()->isCoordinator()) {
    if ((_numberOfShards == 0 && !_isSmart) || _numberOfShards > 1000) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "invalid number of shards");
    }

    VPackSlice keyGenSlice = info.get("keyOptions");
    if (keyGenSlice.isObject()) {
      keyGenSlice = keyGenSlice.get("type");
      if (keyGenSlice.isString()) {
        StringRef tmp(keyGenSlice);
        if (!tmp.empty() && tmp != "traditional") {
          THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_CLUSTER_UNSUPPORTED,
                                         "non-traditional key generators are "
                                         "not supported for sharded "
                                         "collections");
        }
      }
    }
  }

  auto replicationFactorSlice = info.get("replicationFactor");
  if (!replicationFactorSlice.isNone()) {
    bool isError = true;
    if (replicationFactorSlice.isNumber()) {
      _replicationFactor = replicationFactorSlice.getNumber<size_t>();
      // mop: only allow satellite collections to be created explicitly
      if (_replicationFactor > 0 && _replicationFactor <= 10) {
        isError = false;
#ifdef USE_ENTERPRISE
      } else if (_replicationFactor == 0) {
        isError = false;
#endif
      }
    }
#ifdef USE_ENTERPRISE
    else if (replicationFactorSlice.isString() &&
             replicationFactorSlice.copyString() == "satellite") {
      _replicationFactor = 0;
      _numberOfShards = 1;
      _distributeShardsLike = "";
      _avoidServers.clear();
      isError = false;
    }
#endif
    if (isError) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "invalid replicationFactor");
    }
  }

  if (shardKeysSlice.isNone() || isSatellite()) {
    // Use default.
    _shardKeys.emplace_back(StaticStrings::KeyString);
  } else {
    if (shardKeysSlice.isArray()) {
      for (auto const& sk : VPackArrayIterator(shardKeysSlice)) {
        if (sk.isString()) {
          std::string key = sk.copyString();
          // remove : char at the beginning or end (for enterprise)
          std::string stripped;
          if (!key.empty()) {
            if (key.front() == ':') {
              stripped = key.substr(1);
            } else if (key.back() == ':') {
              stripped = key.substr(0, key.size() - 1);
            } else {
              stripped = key;
            }
          }
          // system attributes are not allowed (except _key)
          if (!stripped.empty() && stripped != StaticStrings::IdString &&
              stripped != StaticStrings::RevString) {
            _shardKeys.emplace_back(key);
          }
        }
      }
      if (_shardKeys.empty() && !isCluster) {
        // Compatibility. Old configs might store empty shard-keys locally.
        // This is translated to ["_key"]. In cluster-case this always was
        // forbidden.
        _shardKeys.emplace_back(StaticStrings::KeyString);
      }
    }
  }

  if (_shardKeys.empty() || _shardKeys.size() > 8) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "invalid number of shard keys");
  }

  _keyGenerator.reset(KeyGenerator::factory(info.get("keyOptions")));

  auto shardsSlice = info.get("shards");
  if (shardsSlice.isObject()) {
    for (auto const& shardSlice : VPackObjectIterator(shardsSlice)) {
      if (shardSlice.key.isString() && shardSlice.value.isArray()) {
        ShardID shard = shardSlice.key.copyString();

        std::vector<ServerID> servers;
        for (auto const& serverSlice : VPackArrayIterator(shardSlice.value)) {
          servers.push_back(serverSlice.copyString());
        }
        _shardIds->emplace(shard, servers);
      }
    }
  }

  if (info.hasKey("avoidServers")) {
    auto avoidServersSlice = info.get("avoidServers");
    if (avoidServersSlice.isArray()) {
      for (const auto& i : VPackArrayIterator(avoidServersSlice)) {
        if (i.isString()) {
          _avoidServers.push_back(i.copyString());
        } else {
          LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "avoidServers must be a vector of strings we got " <<
            avoidServersSlice.toJson() << ". discarding!" ;
          _avoidServers.clear();
          break;
        }
      }
    }
  }

  if (_indexes.empty()) {
    createInitialIndexes();
  }

  auto indexesSlice = info.get("indexes");
  if (indexesSlice.isArray()) {
    StorageEngine* engine = EngineSelectorFeature::ENGINE;
    IndexFactory const* idxFactory = engine->indexFactory(); 
    TRI_ASSERT(idxFactory != nullptr);
    for (auto const& v : VPackArrayIterator(indexesSlice)) {
      if (arangodb::basics::VelocyPackHelper::getBooleanValue(v, "error",
                                                              false)) {
        // We have an error here.
        // Do not add index.
        // TODO Handle Properly
        continue;
      }

      auto idx = idxFactory->prepareIndexFromSlice(v, false, this, true);

      // TODO Mode IndexTypeCheck out
      if (idx->type() == Index::TRI_IDX_TYPE_PRIMARY_INDEX ||
          idx->type() == Index::TRI_IDX_TYPE_EDGE_INDEX) {
        continue;
      }

      if (isCluster) {
        addIndexCoordinator(idx, false);
      } else {
        addIndex(idx);
      }
    }
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (_indexes[0]->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "got invalid indexes for collection '" << _name << "'";
    for (auto const& it : _indexes) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "- " << it.get();
    }
  }
#endif

  if (!ServerState::instance()->isCoordinator() && isPhysical) {
    // If we are not in the coordinator we need a path
    // to the physical data.
    StorageEngine* engine = EngineSelectorFeature::ENGINE;
    if (getPhysical()->path().empty()) {
      std::string path = engine->createCollection(_vocbase, _cid, this);
      getPhysical()->setPath(path);
    }
  }

  int64_t count = ReadNumericValue<int64_t>(info, "count", -1);
  if (count != -1) {
    _physical->updateCount(count);
  }

  if (ServerState::instance()->isDBServer() ||
      !ServerState::instance()->isRunningInCluster()) {
    _followers.reset(new FollowerInfo(this));
  }

  // update server's tick value
  TRI_UpdateTickServer(static_cast<TRI_voc_tick_t>(_cid));

}

LogicalCollection::~LogicalCollection() {}

bool LogicalCollection::IsAllowedName(VPackSlice parameters) {
  bool allowSystem = ReadBooleanValue(parameters, "isSystem", false);
  std::string name = ReadStringValue(parameters, "name", "");
  if (name.empty()) {
    return false;
  }

  bool ok;
  char const* ptr;
  size_t length = 0;

  // check allow characters: must start with letter or underscore if system is
  // allowed
  for (ptr = name.c_str(); *ptr; ++ptr) {
    if (length == 0) {
      if (allowSystem) {
        ok = (*ptr == '_') || ('a' <= *ptr && *ptr <= 'z') ||
             ('A' <= *ptr && *ptr <= 'Z');
      } else {
        ok = ('a' <= *ptr && *ptr <= 'z') || ('A' <= *ptr && *ptr <= 'Z');
      }
    } else {
      ok = (*ptr == '_') || (*ptr == '-') || ('0' <= *ptr && *ptr <= '9') ||
           ('a' <= *ptr && *ptr <= 'z') || ('A' <= *ptr && *ptr <= 'Z');
    }

    if (!ok) {
      return false;
    }

    ++length;
  }

  // invalid name length
  if (length == 0 || length > TRI_COL_NAME_LENGTH) {
    return false;
  }

  return true;
}

/// @brief checks if a collection name is allowed
/// Returns true if the name is allowed and false otherwise
bool LogicalCollection::IsAllowedName(bool allowSystem,
                                      std::string const& name) {
  bool ok;
  char const* ptr;
  size_t length = 0;

  // check allow characters: must start with letter or underscore if system is
  // allowed
  for (ptr = name.c_str(); *ptr; ++ptr) {
    if (length == 0) {
      if (allowSystem) {
        ok = (*ptr == '_') || ('a' <= *ptr && *ptr <= 'z') ||
             ('A' <= *ptr && *ptr <= 'Z');
      } else {
        ok = ('a' <= *ptr && *ptr <= 'z') || ('A' <= *ptr && *ptr <= 'Z');
      }
    } else {
      ok = (*ptr == '_') || (*ptr == '-') || ('0' <= *ptr && *ptr <= '9') ||
           ('a' <= *ptr && *ptr <= 'z') || ('A' <= *ptr && *ptr <= 'Z');
    }

    if (!ok) {
      return false;
    }

    ++length;
  }

  // invalid name length
  if (length == 0 || length > TRI_COL_NAME_LENGTH) {
    return false;
  }

  return true;
}

/// @brief whether or not a collection is fully collected
bool LogicalCollection::isFullyCollected() {
  return getPhysical()->isFullyCollected();
}

uint64_t LogicalCollection::numberDocuments() const {
  // TODO Ask StorageEngine instead
  return primaryIndex()->size();
}

size_t LogicalCollection::journalSize() const { return _journalSize; }

uint32_t LogicalCollection::internalVersion() const { return _internalVersion; }

std::string LogicalCollection::cid_as_string() const {
  return basics::StringUtils::itoa(_cid);
}

TRI_voc_cid_t LogicalCollection::planId() const { return _planId; }

TRI_col_type_e LogicalCollection::type() const { return _type; }

std::string LogicalCollection::name() const {
  // TODO Activate this lock. Right now we have some locks outside.
  // READ_LOCKER(readLocker, _lock);
  return _name;
}

std::string const& LogicalCollection::distributeShardsLike() const {
  return _distributeShardsLike;
}

void LogicalCollection::distributeShardsLike(std::string const& cid) {
  _distributeShardsLike = cid;
}

std::vector<std::string> const& LogicalCollection::avoidServers() const {
  return _avoidServers;
}

void LogicalCollection::avoidServers(std::vector<std::string> const& a) {
  _avoidServers = a;
}

std::string LogicalCollection::dbName() const {
  TRI_ASSERT(_vocbase != nullptr);
  return _vocbase->name();
}

TRI_vocbase_col_status_e LogicalCollection::status() const { return _status; }

TRI_vocbase_col_status_e LogicalCollection::getStatusLocked() {
  READ_LOCKER(readLocker, _lock);
  return _status;
}

void LogicalCollection::executeWhileStatusLocked(
    std::function<void()> const& callback) {
  READ_LOCKER(readLocker, _lock);
  callback();
}

bool LogicalCollection::tryExecuteWhileStatusLocked(
    std::function<void()> const& callback) {
  TRY_READ_LOCKER(readLocker, _lock);
  if (!readLocker.isLocked()) {
    return false;
  }

  callback();
  return true;
}

TRI_vocbase_col_status_e LogicalCollection::tryFetchStatus(bool& didFetch) {
  TRY_READ_LOCKER(locker, _lock);
  if (locker.isLocked()) {
    didFetch = true;
    return _status;
  }
  didFetch = false;
  return TRI_VOC_COL_STATUS_CORRUPTED;
}

/// @brief returns a translation of a collection status
std::string LogicalCollection::statusString() {
  READ_LOCKER(readLocker, _lock);
  switch (_status) {
    case TRI_VOC_COL_STATUS_UNLOADED:
      return "unloaded";
    case TRI_VOC_COL_STATUS_LOADED:
      return "loaded";
    case TRI_VOC_COL_STATUS_UNLOADING:
      return "unloading";
    case TRI_VOC_COL_STATUS_DELETED:
      return "deleted";
    case TRI_VOC_COL_STATUS_LOADING:
      return "loading";
    case TRI_VOC_COL_STATUS_CORRUPTED:
    case TRI_VOC_COL_STATUS_NEW_BORN:
    default:
      return "unknown";
  }
}

// SECTION: Properties
TRI_voc_rid_t LogicalCollection::revision() const {
  // TODO CoordinatorCase
  return _physical->revision();
}

bool LogicalCollection::isLocal() const { return _isLocal; }

bool LogicalCollection::deleted() const { return _isDeleted; }

bool LogicalCollection::doCompact() const { return _doCompact; }

bool LogicalCollection::isSystem() const { return _isSystem; }

bool LogicalCollection::isVolatile() const { return _isVolatile; }

bool LogicalCollection::waitForSync() const { return _waitForSync; }

bool LogicalCollection::isSmart() const { return _isSmart; }

std::unique_ptr<FollowerInfo> const& LogicalCollection::followers() const {
  return _followers;
}

void LogicalCollection::setDeleted(bool newValue) { _isDeleted = newValue; }

/// @brief update statistics for a collection
void LogicalCollection::setRevision(TRI_voc_rid_t revision, bool force) {
  if (revision > 0) {
    // TODO Is this still true?
    /// note: Old version the write-lock for the collection must be held to call
    /// this
    _physical->setRevision(revision, force);
  }
}

// SECTION: Key Options
VPackSlice LogicalCollection::keyOptions() const {
  if (_keyOptions == nullptr) {
    return Helper::NullValue();
  }
  return VPackSlice(_keyOptions->data());
}

// SECTION: Indexes
uint32_t LogicalCollection::indexBuckets() const { return _indexBuckets; }

std::vector<std::shared_ptr<arangodb::Index>> const&
LogicalCollection::getIndexes() const {
  return _indexes;
}

/// @brief return the primary index
// WARNING: Make sure that this LogicalCollection Instance
// is somehow protected. If it goes out of all scopes
// or it's indexes are freed the pointer returned will get invalidated.
arangodb::MMFilesPrimaryIndex* LogicalCollection::primaryIndex() const {
  TRI_ASSERT(!_indexes.empty());

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (_indexes[0]->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "got invalid indexes for collection '" << _name << "'";
    for (auto const& it : _indexes) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "- " << it.get();
    }
  }
#endif

  TRI_ASSERT(_indexes[0]->type() ==
             Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);
  // the primary index must be the index at position #0
  return static_cast<arangodb::MMFilesPrimaryIndex*>(_indexes[0].get());
}

void LogicalCollection::getIndexesVPack(VPackBuilder& result,
                                        bool withFigures) const {
  result.openArray();
  for (auto const& idx : _indexes) {
    result.openObject();
    idx->toVelocyPack(result, withFigures);
    result.close();
  }
  result.close();
}

void LogicalCollection::getPropertiesVPack(VPackBuilder& result, bool translateCids) const {
  TRI_ASSERT(result.isOpenObject());
  result.add("id", VPackValue(std::to_string(_cid)));
  result.add("name", VPackValue(_name));
  result.add("type", VPackValue(static_cast<int>(_type)));
  result.add("status", VPackValue(_status));
  result.add("deleted", VPackValue(_isDeleted));
  result.add("doCompact", VPackValue(_doCompact));
  result.add("isSystem", VPackValue(_isSystem));
  result.add("isVolatile", VPackValue(_isVolatile));
  result.add("waitForSync", VPackValue(_waitForSync));
  result.add("journalSize", VPackValue(_journalSize));
  result.add("indexBuckets", VPackValue(_indexBuckets));
  result.add("replicationFactor", VPackValue(_replicationFactor));
  if (!_distributeShardsLike.empty()) {
    if (translateCids) {
      CollectionNameResolver resolver(_vocbase);
      result.add("distributeShardsLike",
                 VPackValue(resolver.getCollectionNameCluster(
                     static_cast<TRI_voc_cid_t>(
                         basics::StringUtils::uint64(_distributeShardsLike)))));
    } else {
      result.add("distributeShardsLike", VPackValue(_distributeShardsLike));
    }
  }

  if (_keyGenerator != nullptr) {
    result.add(VPackValue("keyOptions"));
    result.openObject();
    _keyGenerator->toVelocyPack(result);
    result.close();
  }

  result.add(VPackValue("shardKeys"));
  result.openArray();
  for (auto const& key : _shardKeys) {
    result.add(VPackValue(key));
  }
  result.close();  // shardKeys
}

// SECTION: Replication
int LogicalCollection::replicationFactor() const {
  return static_cast<int>(_replicationFactor);
}

// SECTION: Sharding
int LogicalCollection::numberOfShards() const {
  return static_cast<int>(_numberOfShards);
}

bool LogicalCollection::allowUserKeys() const { return _allowUserKeys; }

#ifndef USE_ENTERPRISE
bool LogicalCollection::usesDefaultShardKeys() const {
  return (_shardKeys.size() == 1 && _shardKeys[0] == StaticStrings::KeyString);
}
#endif

std::vector<std::string> const& LogicalCollection::shardKeys() const {
  return _shardKeys;
}

std::shared_ptr<ShardMap> LogicalCollection::shardIds() const {
  // TODO make threadsafe update on the cache.
  return _shardIds;
}

void LogicalCollection::setShardMap(std::shared_ptr<ShardMap>& map) {
  _shardIds = map;
}

// SECTION: Modification Functions

// asks the storage engine to rename the collection to the given name
// and persist the renaming info. It is guaranteed by the server
// that no other active collection with the same name and id exists in the same
// database when this function is called. If this operation fails somewhere in
// the middle, the storage engine is required to fully revert the rename
// operation
// and throw only then, so that subsequent collection creation/rename requests
// will
// not fail. the WAL entry for the rename will be written *after* the call
// to "renameCollection" returns

int LogicalCollection::rename(std::string const& newName) {
  // Should only be called from inside vocbase.
  // Otherwise caching is destroyed.
  TRI_ASSERT(!ServerState::instance()->isCoordinator());  // NOT YET IMPLEMENTED

  WRITE_LOCKER_EVENTUAL(locker, _lock, 1000);

  // Check for illeagal states.
  switch (_status) {
    case TRI_VOC_COL_STATUS_CORRUPTED:
      return TRI_ERROR_ARANGO_CORRUPTED_COLLECTION;
    case TRI_VOC_COL_STATUS_DELETED:
      return TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND;
    default:
      // Fall through intentional
      break;
  }

  // Check for duplicate name
  auto other = _vocbase->lookupCollection(newName);
  if (other != nullptr) {
    return TRI_ERROR_ARANGO_DUPLICATE_NAME;
  }

  switch (_status) {
    case TRI_VOC_COL_STATUS_UNLOADED:
    case TRI_VOC_COL_STATUS_LOADED:
    case TRI_VOC_COL_STATUS_UNLOADING:
    case TRI_VOC_COL_STATUS_LOADING: {
      break;
    }
    default:
      // Unknown status
      return TRI_ERROR_INTERNAL;
  }

  std::string oldName = _name;
  _name = newName;
  // Okay we can finally rename safely
  try {
    StorageEngine* engine = EngineSelectorFeature::ENGINE;
    bool const doSync =
        application_features::ApplicationServer::getFeature<DatabaseFeature>(
            "Database")
            ->forceSyncProperties();
    engine->changeCollection(_vocbase, _cid, this, doSync);
  } catch (basics::Exception const& ex) {
    // Engine Rename somehow failed. Reset to old name
    _name = oldName;
    return ex.code();
  } catch (...) {
    // Engine Rename somehow failed. Reset to old name
    _name = oldName;
    return TRI_ERROR_INTERNAL;
  }

  // CHECK if this ordering is okay. Before change the version was increased
  // after swapping in vocbase mapping.
  increaseInternalVersion();
  return TRI_ERROR_NO_ERROR;
}

int LogicalCollection::close() {
  // This was unload() in 3.0
  auto primIdx = primaryIndex();
  auto idxSize = primIdx->size();

  if (!_isDeleted &&
      _physical->initialCount() != static_cast<int64_t>(idxSize)) {
    _physical->updateCount(idxSize);

    // save new "count" value
    StorageEngine* engine = EngineSelectorFeature::ENGINE;
    bool const doSync =
        application_features::ApplicationServer::getFeature<DatabaseFeature>(
            "Database")
            ->forceSyncProperties();
    engine->changeCollection(_vocbase, _cid, this, doSync);
  }

  // We also have to unload the indexes.
  for (auto& idx : _indexes) {
    idx->unload();
  }

  return getPhysical()->close();
}

void LogicalCollection::unload() {
}

void LogicalCollection::drop() {
  // make sure collection has been closed
  this->close();

  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  engine->dropCollection(_vocbase, this);
  _isDeleted = true;

  // save some memory by freeing the indexes
  _indexes.clear();
  try {
    // close collection. this will also invalidate the revisions cache
    _physical->close();
  } catch (...) {
    // don't throw from here... dropping should succeed
  }
}

void LogicalCollection::setStatus(TRI_vocbase_col_status_e status) {
  _status = status;

  if (status == TRI_VOC_COL_STATUS_LOADED) {
    increaseInternalVersion();
  }
}

void LogicalCollection::toVelocyPackForAgency(VPackBuilder& result) {
  _status = TRI_VOC_COL_STATUS_LOADED;
  result.openObject();
  toVelocyPackInObject(result, false);

  result.close();  // Base Object
}

void LogicalCollection::toVelocyPackForClusterInventory(VPackBuilder& result,
                                                        bool useSystem) const {
  if (_isSystem && !useSystem) {
    return;
  }
  result.openObject();
  result.add(VPackValue("parameters"));
  result.openObject();
  toVelocyPackInObject(result, true);
  result.close();
  result.add(VPackValue("indexes"));
  getIndexesVPack(result, false);
  result.close(); // CollectionInfo
}

void LogicalCollection::toVelocyPack(VPackBuilder& result,
                                     bool withPath) const {
  result.openObject();
  toVelocyPackInObject(result, false);
  result.add(
      "cid",
      VPackValue(std::to_string(_cid)));  // export cid for compatibility, too
  result.add("planId",
             VPackValue(std::to_string(_planId)));  // export planId for cluster
  result.add("version", VPackValue(_version));
  result.add("count", VPackValue(_physical->initialCount()));

  if (withPath) {
    result.add("path", VPackValue(getPhysical()->path()));
  }
  result.add("allowUserKeys", VPackValue(_allowUserKeys));

  result.close();
}

// Internal helper that inserts VPack info into an existing object and leaves
// the object open
void LogicalCollection::toVelocyPackInObject(VPackBuilder& result, bool translateCids) const {
  getPropertiesVPack(result, translateCids);
  result.add("numberOfShards", VPackValue(_numberOfShards));

  if (!_avoidServers.empty()) {
    result.add(VPackValue("avoidServers"));
    VPackArrayBuilder b(&result);
    for (auto const& i : _avoidServers) {
      result.add(VPackValue(i));
    }
  }

  result.add(VPackValue("shards"));
  result.openObject();
  for (auto const& shards : *_shardIds) {
    result.add(VPackValue(shards.first));
    result.openArray();
    for (auto const& servers : shards.second) {
      result.add(VPackValue(servers));
    }
    result.close();  // server array
  }
  result.close();  // shards

  result.add(VPackValue("indexes"));
  getIndexesVPack(result, false);
}

void LogicalCollection::toVelocyPack(VPackBuilder& builder, bool includeIndexes,
                                     TRI_voc_tick_t maxTick) {
  TRI_ASSERT(!builder.isClosed());
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  engine->getCollectionInfo(_vocbase, _cid, builder, includeIndexes, maxTick);
}

void LogicalCollection::increaseInternalVersion() { ++_internalVersion; }

int LogicalCollection::updateProperties(VPackSlice const& slice, bool doSync) {
  // the following collection properties are intentionally not updated as
  // updating
  // them would be very complicated:
  // - _cid
  // - _name
  // - _type
  // - _isSystem
  // - _isVolatile
  // ... probably a few others missing here ...

  WRITE_LOCKER(writeLocker, _infoLock);

  // some basic validation...
  if (isVolatile() && arangodb::basics::VelocyPackHelper::getBooleanValue(
                          slice, "waitForSync", waitForSync())) {
    // the combination of waitForSync and isVolatile makes no sense
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "volatile collections do not support the waitForSync option");
  }

  if (isVolatile() != arangodb::basics::VelocyPackHelper::getBooleanValue(
                          slice, "isVolatile", isVolatile())) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "isVolatile option cannot be changed at runtime");
  }

  uint32_t tmp = arangodb::basics::VelocyPackHelper::getNumericValue<uint32_t>(
      slice, "indexBuckets",
      2 /*Just for validation, this default Value passes*/);
  if (tmp == 0 || tmp > 1024) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "indexBuckets must be a two-power between 1 and 1024");
  }
  // end of validation

  _doCompact = Helper::getBooleanValue(slice, "doCompact", _doCompact);
  _waitForSync = Helper::getBooleanValue(slice, "waitForSync", _waitForSync);
  if (slice.hasKey("journalSize")) {
    _journalSize = Helper::getNumericValue<TRI_voc_size_t>(slice, "journalSize",
                                                           _journalSize);
  } else {
    _journalSize = Helper::getNumericValue<TRI_voc_size_t>(slice, "maximalSize",
                                                           _journalSize);
  }
  _indexBuckets =
      Helper::getNumericValue<uint32_t>(slice, "indexBuckets", _indexBuckets);

  if (!_isLocal) {
    // We need to inform the cluster as well
    return ClusterInfo::instance()->setCollectionPropertiesCoordinator(
        _vocbase->name(), cid_as_string(), this);
  }

  int64_t count = arangodb::basics::VelocyPackHelper::getNumericValue<int64_t>(
      slice, "count", _physical->initialCount());
  if (count != _physical->initialCount()) {
    _physical->updateCount(count);
  }
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  engine->changeCollection(_vocbase, _cid, this, doSync);

  return TRI_ERROR_NO_ERROR;
}

/// @brief return the figures for a collection
std::shared_ptr<arangodb::velocypack::Builder> LogicalCollection::figures() {
  auto builder = std::make_shared<VPackBuilder>();

  if (ServerState::instance()->isCoordinator()) {
    builder->openObject();
    builder->close();
    int res = figuresOnCoordinator(dbName(), cid_as_string(), builder);

    if (res != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(res);
    }
  } else {
    builder->openObject();

    // add index information
    size_t sizeIndexes = getPhysical()->memory();
    size_t numIndexes = 0;
    for (auto const& idx : _indexes) {
      sizeIndexes += static_cast<size_t>(idx->memory());
      ++numIndexes;
    }

    builder->add("indexes", VPackValue(VPackValueType::Object));
    builder->add("count", VPackValue(numIndexes));
    builder->add("size", VPackValue(sizeIndexes));
    builder->close();  // indexes

    builder->add("lastTick", VPackValue(_maxTick));
    builder->add("uncollectedLogfileEntries",
                 VPackValue(
                   //MOVE TO PHYSICAL
                   static_cast<arangodb::MMFilesCollection*>(getPhysical())
                      ->uncollectedLogfileEntries()
                 )
                );

    // add engine-specific figures
    getPhysical()->figures(builder);
    builder->close();
  }

  return builder;
}

/// @brief opens an existing collection
void LogicalCollection::open(bool ignoreErrors) {
  VPackBuilder builder;
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  engine->getCollectionInfo(_vocbase, cid(), builder, true, 0);

  VPackSlice initialCount =
      builder.slice().get(std::vector<std::string>({"parameters", "count"}));
  if (initialCount.isNumber()) {
    int64_t count = initialCount.getNumber<int64_t>();
    if (count > 0) {
      _physical->updateCount(count);
    }
  }
  double start = TRI_microtime();

  LOG_TOPIC(TRACE, Logger::PERFORMANCE)
      << "open-document-collection { collection: " << _vocbase->name() << "/"
      << _name << " }";

  int res = openWorker(ignoreErrors);

  if (res != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        res,
        std::string("cannot open document collection from path '") + getPhysical()->path() +
            "': " + TRI_errno_string(res));
  }

  arangodb::SingleCollectionTransaction trx(
      arangodb::StandaloneTransactionContext::Create(_vocbase), cid(),
      AccessMode::Type::WRITE);

  // build the primary index
  double startIterate = TRI_microtime();

  LOG_TOPIC(TRACE, Logger::PERFORMANCE)
      << "iterate-markers { collection: " << _vocbase->name() << "/" << _name
      << " }";

  _isInitialIteration = true;

  // iterate over all markers of the collection
  res = getPhysical()->iterateMarkersOnLoad(&trx);

  LOG_TOPIC(TRACE, Logger::PERFORMANCE)
      << "[timer] " << Logger::FIXED(TRI_microtime() - startIterate)
      << " s, iterate-markers { collection: " << _vocbase->name() << "/"
      << _name << " }";

  if (res != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        res,
        std::string("cannot iterate data of document collection: ") +
            TRI_errno_string(res));
  }

  _isInitialIteration = false;

  // build the indexes meta-data, but do not fill the indexes yet
  {
    auto old = useSecondaryIndexes();

    // turn filling of secondary indexes off. we're now only interested in
    // getting
    // the indexes' definition. we'll fill them below ourselves.
    useSecondaryIndexes(false);

    try {
      detectIndexes(&trx);
      useSecondaryIndexes(old);
    } catch (basics::Exception const& ex) {
      useSecondaryIndexes(old);
      THROW_ARANGO_EXCEPTION_MESSAGE(
          ex.code(),
          std::string("cannot initialize collection indexes: ") + ex.what());
    } catch (std::exception const& ex) {
      useSecondaryIndexes(old);
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL,
          std::string("cannot initialize collection indexes: ") + ex.what());
    } catch (...) {
      useSecondaryIndexes(old);
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL,
          "cannot initialize collection indexes: unknown exception");
    }
  }

  if (!engine->inRecovery()) {
    // build the index structures, and fill the indexes
    fillIndexes(&trx, *(indexList()));
  }

  LOG_TOPIC(TRACE, Logger::PERFORMANCE)
      << "[timer] " << Logger::FIXED(TRI_microtime() - start)
      << " s, open-document-collection { collection: " << _vocbase->name()
      << "/" << _name << " }";

  // successfully opened collection. now adjust version number
  if (_version != VERSION_31 && !_revisionError &&
      application_features::ApplicationServer::server
          ->getFeature<DatabaseFeature>("Database")
          ->check30Revisions()) {
    _version = VERSION_31;
    bool const doSync =
        application_features::ApplicationServer::getFeature<DatabaseFeature>(
            "Database")
            ->forceSyncProperties();
    StorageEngine* engine = EngineSelectorFeature::ENGINE;
    engine->changeCollection(_vocbase, _cid, this, doSync);
  }

  TRI_UpdateTickServer(_cid);
}

/// @brief opens an existing collection
int LogicalCollection::openWorker(bool ignoreErrors) {
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  double start = TRI_microtime();

  LOG_TOPIC(TRACE, Logger::PERFORMANCE)
      << "open-collection { collection: " << _vocbase->name() << "/" << name()
      << " }";

  try {
    // check for journals and datafiles
    int res = engine->openCollection(_vocbase, this, ignoreErrors);

    if (res != TRI_ERROR_NO_ERROR) {
      LOG_TOPIC(DEBUG, arangodb::Logger::FIXME) << "cannot open '" << getPhysical()->path() << "', check failed";
      return res;
    }

    LOG_TOPIC(TRACE, Logger::PERFORMANCE)
        << "[timer] " << Logger::FIXED(TRI_microtime() - start)
        << " s, open-collection { collection: " << _vocbase->name() << "/"
        << name() << " }";

    return TRI_ERROR_NO_ERROR;
  } catch (basics::Exception const& ex) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "cannot load collection parameter file '" << getPhysical()->path()
             << "': " << ex.what();
    return ex.code();
  } catch (std::exception const& ex) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "cannot load collection parameter file '" << getPhysical()->path()
             << "': " << ex.what();
    return TRI_ERROR_INTERNAL;
  }
}

/// SECTION Indexes

std::shared_ptr<Index> LogicalCollection::lookupIndex(
    TRI_idx_iid_t idxId) const {
  for (auto const& idx : _indexes) {
    if (idx->id() == idxId) {
      return idx;
    }
  }
  return nullptr;
}

std::shared_ptr<Index> LogicalCollection::lookupIndex(
    VPackSlice const& info) const {
  if (!info.isObject()) {
    // Compatibility with old v8-vocindex.
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  // extract type
  VPackSlice value = info.get("type");

  if (!value.isString()) {
    // Compatibility with old v8-vocindex.
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  std::string tmp = value.copyString();
  arangodb::Index::IndexType const type = arangodb::Index::type(tmp.c_str());

  for (auto const& idx : _indexes) {
    if (idx->type() == type) {
      // Only check relevant indices
      if (idx->matchesDefinition(info)) {
        // We found an index for this definition.
        return idx;
      }
    }
  }
  return nullptr;
}

std::shared_ptr<Index> LogicalCollection::createIndex(transaction::Methods* trx,
                                                      VPackSlice const& info,
                                                      bool& created) {
  // TODO Get LOCK for the vocbase
  auto idx = lookupIndex(info);
  if (idx != nullptr) {
    created = false;
    // We already have this index.
    // Should we throw instead?
    return idx;
  }

  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  IndexFactory const* idxFactory = engine->indexFactory(); 
  TRI_ASSERT(idxFactory != nullptr);

  // We are sure that we do not have an index of this type.
  // We also hold the lock.
  // Create it

  idx = idxFactory->prepareIndexFromSlice(info, true, this, false);
  TRI_ASSERT(idx != nullptr);
  if (ServerState::instance()->isCoordinator()) {
    // In the coordinator case we do not fill the index
    // We only inform the others.
    addIndexCoordinator(idx, true);
    created = true;
    return idx;
  }

  TRI_ASSERT(idx.get()->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);
  std::vector<std::shared_ptr<arangodb::Index>> indexListLocal;
  indexListLocal.emplace_back(idx);
  int res = fillIndexes(trx, indexListLocal, false);

  if (res != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION(res);
  }

  bool const writeMarker = !engine->inRecovery();
  //    !MMFilesLogfileManager::instance()->isInRecovery();
  res = saveIndex(idx.get(), writeMarker);

  if (res != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION(res);
  }
  // Until here no harm is done if sth fails. The shared ptr will clean up. if
  // left before

  addIndex(idx);
  {
    VPackBuilder builder;
    bool const doSync =
        application_features::ApplicationServer::getFeature<DatabaseFeature>(
            "Database")
            ->forceSyncProperties();
    toVelocyPack(builder, false);
    updateProperties(builder.slice(), doSync);
  }
  created = true;
  return idx;
}

int LogicalCollection::restoreIndex(transaction::Methods* trx, VPackSlice const& info,
                                    std::shared_ptr<arangodb::Index>& idx) {
  // The coordinator can never get into this state!
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  idx.reset();  // Clear it to make sure.
  if (!info.isObject()) {
    return TRI_ERROR_INTERNAL;
  }

  // We create a new Index object to make sure that the index
  // is not handed out except for a successful case.
  std::shared_ptr<Index> newIdx;
  try {
    StorageEngine* engine = EngineSelectorFeature::ENGINE;
    IndexFactory const* idxFactory = engine->indexFactory(); 
    TRI_ASSERT(idxFactory != nullptr);
    newIdx = idxFactory->prepareIndexFromSlice(info, false, this, false);
  } catch (arangodb::basics::Exception const& e) {
    // Something with index creation went wrong.
    // Just report.
    return e.code();
  }
  TRI_ASSERT(newIdx != nullptr);

  TRI_UpdateTickServer(newIdx->id());

  TRI_ASSERT(newIdx.get()->type() !=
             Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);
  std::vector<std::shared_ptr<arangodb::Index>> indexListLocal;
  indexListLocal.emplace_back(newIdx);
  int res = fillIndexes(trx, indexListLocal, false);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  addIndex(newIdx);
  idx = newIdx;
  return TRI_ERROR_NO_ERROR;
}

/// @brief saves an index
int LogicalCollection::saveIndex(arangodb::Index* idx, bool writeMarker) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  std::shared_ptr<VPackBuilder> builder;
  try {
    builder = idx->toVelocyPack(false);
  } catch (arangodb::basics::Exception const& ex) {
    return ex.code();
  } catch (...) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "cannot save index definition";
    return TRI_ERROR_INTERNAL;
  }
  if (builder == nullptr) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "cannot save index definition";
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  engine->createIndex(_vocbase, cid(), idx->id(), builder->slice());
  
  int res = TRI_ERROR_NO_ERROR;
  engine->createIndexWalMarker(_vocbase, cid(), builder->slice(), writeMarker,res);
  return res;
}

/// @brief removes an index by id
bool LogicalCollection::removeIndex(TRI_idx_iid_t iid) {
  size_t const n = _indexes.size();

  for (size_t i = 0; i < n; ++i) {
    auto idx = _indexes[i];

    if (!idx->canBeDropped()) {
      continue;
    }

    if (idx->id() == iid) {
      // found!
      idx->drop();

      _indexes.erase(_indexes.begin() + i);

      // update statistics
      if (idx->type() == arangodb::Index::TRI_IDX_TYPE_FULLTEXT_INDEX) {
        --_cleanupIndexes;
      }
      if (idx->isPersistent()) {
        --_persistentIndexes;
      }

      return true;
    }
  }

  // not found
  return false;
}

/// @brief drops an index, including index file removal and replication
bool LogicalCollection::dropIndex(TRI_idx_iid_t iid, bool writeMarker) {
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  if (iid == 0) {
    // invalid index id or primary index
    events::DropIndex("", std::to_string(iid), TRI_ERROR_NO_ERROR);
    return true;
  }

  arangodb::aql::QueryCache::instance()->invalidate(_vocbase, name());
  if (!removeIndex(iid)) {
    // We tried to remove an index that does not exist
    events::DropIndex("", std::to_string(iid),
                      TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
    return false;
  }

  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  engine->dropIndex(_vocbase, cid(), iid);

  {
    VPackBuilder builder;
    bool const doSync =
        application_features::ApplicationServer::getFeature<DatabaseFeature>(
            "Database")
            ->forceSyncProperties();
    toVelocyPack(builder, false);
    updateProperties(builder.slice(), doSync);
  }

  if (writeMarker) {
    int res = TRI_ERROR_NO_ERROR;

    VPackBuilder markerBuilder;
    markerBuilder.openObject();
    markerBuilder.add("id", VPackValue(std::to_string(iid)));
    markerBuilder.close();
    engine->dropIndexWalMarker(_vocbase, cid(), markerBuilder.slice(),writeMarker,res);

    if(! res){
      events::DropIndex("", std::to_string(iid), TRI_ERROR_NO_ERROR);
    } else {
      LOG_TOPIC(WARN, arangodb::Logger::FIXME) << "could not save index drop marker in log: "
              << TRI_errno_string(res);
      events::DropIndex("", std::to_string(iid), res);
    }
  }
  return true;
}

/// @brief creates the initial indexes for the collection
void LogicalCollection::createInitialIndexes() {
  // TODO Properly fix this. The outside should make sure that only NEW
  // collections
  // try to create the indexes.
  if (!_indexes.empty()) {
    return;
  }

  std::vector<std::shared_ptr<arangodb::Index>> systemIndexes;
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  IndexFactory const* idxFactory = engine->indexFactory(); 
  TRI_ASSERT(idxFactory != nullptr);

  idxFactory->fillSystemIndexes(this, systemIndexes);
  for (auto const& it : systemIndexes) {
    addIndex(it);
  }
}

/// @brief iterator for index open
bool LogicalCollection::openIndex(VPackSlice const& description,
                                  transaction::Methods* trx) {
  // VelocyPack must be an index description
  if (!description.isObject()) {
    return false;
  }

  bool unused = false;
  auto idx = createIndex(trx, description, unused);

  if (idx == nullptr) {
    // error was already printed if we get here
    return false;
  }

  return true;
}

/// @brief enumerate all indexes of the collection, but don't fill them yet
int LogicalCollection::detectIndexes(transaction::Methods* trx) {
  StorageEngine* engine = EngineSelectorFeature::ENGINE;
  VPackBuilder builder;
  engine->getCollectionInfo(_vocbase, _cid, builder, true, UINT64_MAX);

  // iterate over all index files
  for (auto const& it : VPackArrayIterator(builder.slice().get("indexes"))) {
    bool ok = openIndex(it, trx);

    if (!ok) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "cannot load index for collection '" << name() << "'";
    }
  }

  return TRI_ERROR_NO_ERROR;
}

std::vector<std::shared_ptr<arangodb::Index>> const*
LogicalCollection::indexList() const {
  return &_indexes;
}

int LogicalCollection::fillIndexes(
    transaction::Methods* trx,
    std::vector<std::shared_ptr<arangodb::Index>> const& indexes,
    bool skipPersistent) {
  // distribute the work to index threads plus this thread
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  size_t const n = indexes.size();

  if (n == 0 || (n == 1 &&
                 indexes[0].get()->type() ==
                     Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX)) {
    return TRI_ERROR_NO_ERROR;
  }

  bool rolledBack = false;
  auto rollbackAll = [&]() -> void {
    for (size_t i = 0; i < n; i++) {
      auto idx = indexes[i].get();
      if (idx->type() == Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX) {
        continue;
      }
      if (idx->isPersistent()) {
        continue;
      }
      idx->unload();  // TODO: check is this safe? truncate not necessarily
                      // feasible
    }
  };

  double start = TRI_microtime();

  TRI_ASSERT(SchedulerFeature::SCHEDULER != nullptr);
  auto ioService = SchedulerFeature::SCHEDULER->ioService();
  TRI_ASSERT(ioService != nullptr);
  arangodb::basics::LocalTaskQueue queue(ioService);

  // only log performance infos for indexes with more than this number of
  // entries
  static size_t const NotificationSizeThreshold = 131072;
  auto primaryIndex = this->primaryIndex();

  if (primaryIndex->size() > NotificationSizeThreshold) {
    LOG_TOPIC(TRACE, Logger::PERFORMANCE)
        << "fill-indexes-document-collection { collection: " << _vocbase->name()
        << "/" << name() << " }, indexes: " << (n - 1);
  }

  TRI_ASSERT(n > 0);

  try {
    TRI_ASSERT(!ServerState::instance()->isCoordinator());

    // give the index a size hint
    auto nrUsed = primaryIndex->size();
    for (size_t i = 0; i < n; i++) {
      auto idx = indexes[i];
      if (idx->type() == Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX) {
        continue;
      }
      idx.get()->sizeHint(trx, nrUsed);
    }

    // process documents a million at a time
    size_t blockSize = 1024 * 1024 * 1;

    if (nrUsed < blockSize) {
      blockSize = nrUsed;
    }
    if (blockSize == 0) {
      blockSize = 1;
    }

    ManagedDocumentResult mmdr;

    std::vector<std::pair<TRI_voc_rid_t, VPackSlice>> documents;
    documents.reserve(blockSize);

    auto insertInAllIndexes = [&]() -> void {
      for (size_t i = 0; i < n; ++i) {
        auto idx = indexes[i];
        if (idx->type() == Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX) {
          continue;
        }
        fillIndex(&queue, trx, idx.get(), documents, skipPersistent);
      }

      queue.dispatchAndWait();

      if (queue.status() != TRI_ERROR_NO_ERROR) {
        rollbackAll();
        rolledBack = true;
      }
    };

    if (nrUsed > 0) {
      arangodb::basics::BucketPosition position;
      uint64_t total = 0;

      while (true) {
        MMFilesSimpleIndexElement element =
            primaryIndex->lookupSequential(trx, position, total);

        if (!element) {
          break;
        }

        TRI_voc_rid_t revisionId = element.revisionId();

        if (readRevision(trx, mmdr, revisionId)) {
          uint8_t const* vpack = mmdr.vpack();
          TRI_ASSERT(vpack != nullptr);
          documents.emplace_back(std::make_pair(revisionId, VPackSlice(vpack)));

          if (documents.size() == blockSize) {
            // now actually fill the secondary indexes
            insertInAllIndexes();
            if (queue.status() != TRI_ERROR_NO_ERROR) {
              break;
            }
            documents.clear();
          }
        }
      }
    }

    // process the remainder of the documents
    if (queue.status() == TRI_ERROR_NO_ERROR && !documents.empty()) {
      insertInAllIndexes();
    }

    // TODO: fix perf logging?
  } catch (arangodb::basics::Exception const& ex) {
    queue.setStatus(ex.code());
    LOG_TOPIC(WARN, arangodb::Logger::FIXME) << "caught exception while filling indexes: " << ex.what();
  } catch (std::bad_alloc const&) {
    queue.setStatus(TRI_ERROR_OUT_OF_MEMORY);
  } catch (std::exception const& ex) {
    LOG_TOPIC(WARN, arangodb::Logger::FIXME) << "caught exception while filling indexes: " << ex.what();
    queue.setStatus(TRI_ERROR_INTERNAL);
  } catch (...) {
    LOG_TOPIC(WARN, arangodb::Logger::FIXME) << "caught unknown exception while filling indexes";
    queue.setStatus(TRI_ERROR_INTERNAL);
  }

  if (queue.status() != TRI_ERROR_NO_ERROR && !rolledBack) {
    try {
      rollbackAll();
    } catch (...) {
    }
  }

  LOG_TOPIC(TRACE, Logger::PERFORMANCE)
      << "[timer] " << Logger::FIXED(TRI_microtime() - start)
      << " s, fill-indexes-document-collection { collection: "
      << _vocbase->name() << "/" << name() << " }, indexes: " << (n - 1);

  return queue.status();
}

void LogicalCollection::addIndex(std::shared_ptr<arangodb::Index> idx) {
  // primary index must be added at position 0
  TRI_ASSERT(idx->type() != arangodb::Index::TRI_IDX_TYPE_PRIMARY_INDEX ||
             _indexes.empty());

  auto const id = idx->id();
  for (auto const& it : _indexes) {
    if (it->id() == id) {
      // already have this particular index. do not add it again
      return;
    }
  }

  TRI_UpdateTickServer(static_cast<TRI_voc_tick_t>(id));

  _indexes.emplace_back(idx);

  // update statistics
  if (idx->type() == arangodb::Index::TRI_IDX_TYPE_FULLTEXT_INDEX) {
    ++_cleanupIndexes;
  }
  if (idx->isPersistent()) {
    ++_persistentIndexes;
  }
}

void LogicalCollection::addIndexCoordinator(
    std::shared_ptr<arangodb::Index> idx, bool distribute) {
  auto const id = idx->id();
  for (auto const& it : _indexes) {
    if (it->id() == id) {
      // already have this particular index. do not add it again
      return;
    }
  }

  _indexes.emplace_back(idx);
  if (distribute) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
  }
}

/// @brief garbage-collect a collection's indexes
int LogicalCollection::cleanupIndexes() {
  int res = TRI_ERROR_NO_ERROR;

  // cleaning indexes is expensive, so only do it if the flag is set for the
  // collection
  if (_cleanupIndexes > 0) {
    WRITE_LOCKER(writeLocker, _idxLock);

    for (auto& idx : _indexes) {
      if (idx->type() == arangodb::Index::TRI_IDX_TYPE_FULLTEXT_INDEX) {
        res = idx->cleanup();

        if (res != TRI_ERROR_NO_ERROR) {
          break;
        }
      }
    }
  }

  return res;
}

/// @brief reads an element from the document collection
int LogicalCollection::read(transaction::Methods* trx, std::string const& key,
                            ManagedDocumentResult& result, bool lock) {
  return read(trx, StringRef(key.c_str(), key.size()), result, lock);
}

int LogicalCollection::read(transaction::Methods* trx, StringRef const& key,
                            ManagedDocumentResult& result, bool lock) {
  transaction::BuilderLeaser builder(trx);
  builder->add(VPackValuePair(key.data(), key.size(), VPackValueType::String));
  return getPhysical()->read(trx, builder->slice(), result, lock);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief processes a truncate operation (note: currently this only clears
/// the read-cache
////////////////////////////////////////////////////////////////////////////////

int LogicalCollection::truncate(transaction::Methods* trx) {
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief inserts a document or edge into the collection
////////////////////////////////////////////////////////////////////////////////

int LogicalCollection::insert(transaction::Methods* trx, VPackSlice const slice,
                              ManagedDocumentResult& result,
                              OperationOptions& options,
                              TRI_voc_tick_t& resultMarkerTick, bool lock) {
  resultMarkerTick = 0;
  VPackSlice fromSlice;
  VPackSlice toSlice;

  bool const isEdgeCollection = (_type == TRI_COL_TYPE_EDGE);

  if (isEdgeCollection) {
    // _from:
    fromSlice = slice.get(StaticStrings::FromString);
    if (!fromSlice.isString()) {
      return TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE;
    }
    VPackValueLength len;
    char const* docId = fromSlice.getString(len);
    size_t split;
    if (!TRI_ValidateDocumentIdKeyGenerator(docId, static_cast<size_t>(len),
                                            &split)) {
      return TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE;
    }
    // _to:
    toSlice = slice.get(StaticStrings::ToString);
    if (!toSlice.isString()) {
      return TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE;
    }
    docId = toSlice.getString(len);
    if (!TRI_ValidateDocumentIdKeyGenerator(docId, static_cast<size_t>(len),
                                            &split)) {
      return TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE;
    }
  }

  transaction::BuilderLeaser builder(trx);
  VPackSlice newSlice;
  int res = TRI_ERROR_NO_ERROR;
  if (options.recoveryMarker == nullptr) {
    TIMER_START(TRANSACTION_NEW_OBJECT_FOR_INSERT);
    res = newObjectForInsert(trx, slice, fromSlice, toSlice, isEdgeCollection,
                             *builder.get(), options.isRestore);
    TIMER_STOP(TRANSACTION_NEW_OBJECT_FOR_INSERT);
    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
    newSlice = builder->slice();
  } else {
    TRI_ASSERT(slice.isObject());
    // we can get away with the fast hash function here, as key values are
    // restricted to strings
    newSlice = slice;
  }

  res = getPhysical()->insert(trx, newSlice, result, options, resultMarkerTick,
                              lock);

  return res;
}

/// @brief updates a document or edge in a collection
int LogicalCollection::update(transaction::Methods* trx, VPackSlice const newSlice,
                              ManagedDocumentResult& result,
                              OperationOptions& options,
                              TRI_voc_tick_t& resultMarkerTick, bool lock,
                              TRI_voc_rid_t& prevRev,
                              ManagedDocumentResult& previous) {
  resultMarkerTick = 0;

  if (!newSlice.isObject()) {
    return TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID;
  }

  prevRev = 0;

  TRI_voc_rid_t revisionId = 0;
  if (options.isRestore) {
    VPackSlice oldRev = TRI_ExtractRevisionIdAsSlice(newSlice);
    if (!oldRev.isString()) {
      return TRI_ERROR_ARANGO_DOCUMENT_REV_BAD;
    }
    bool isOld;
    VPackValueLength l;
    char const* p = oldRev.getString(l);
    revisionId = TRI_StringToRid(p, l, isOld, false);
    if (isOld) {
      // Do not tolerate old revision IDs
      revisionId = TRI_HybridLogicalClock();
    }
  } else {
    revisionId = TRI_HybridLogicalClock();
  }

  VPackSlice key = newSlice.get(StaticStrings::KeyString);
  if (key.isNone()) {
    return TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD;
  }

  return getPhysical()->update(trx, newSlice, result, options, resultMarkerTick,
                               lock, prevRev, previous, revisionId, key);
}

/// @brief replaces a document or edge in a collection
int LogicalCollection::replace(
    transaction::Methods* trx, VPackSlice const newSlice,
                               ManagedDocumentResult& result,
                               OperationOptions& options,
                               TRI_voc_tick_t& resultMarkerTick, bool lock,
                               TRI_voc_rid_t& prevRev,
                               ManagedDocumentResult& previous) {
  resultMarkerTick = 0;

  if (!newSlice.isObject()) {
    return TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID;
  }

  prevRev = 0;
  VPackSlice fromSlice;
  VPackSlice toSlice;


  if (type() == TRI_COL_TYPE_EDGE) {
    fromSlice = newSlice.get(StaticStrings::FromString);
    if (!fromSlice.isString()) {
      return TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE;
    }
    toSlice = newSlice.get(StaticStrings::ToString);
    if (!toSlice.isString()) {
      return TRI_ERROR_ARANGO_INVALID_EDGE_ATTRIBUTE;
    }
  }

  TRI_voc_rid_t revisionId = 0;
  if (options.isRestore) {
    VPackSlice oldRev = TRI_ExtractRevisionIdAsSlice(newSlice);
    if (!oldRev.isString()) {
      return TRI_ERROR_ARANGO_DOCUMENT_REV_BAD;
    }
    bool isOld;
    VPackValueLength l;
    char const* p = oldRev.getString(l);
    revisionId = TRI_StringToRid(p, l, isOld, false);
    if (isOld || revisionId == UINT64_MAX) {
      // Do not tolerate old revision ticks or invalid ones:
      revisionId = TRI_HybridLogicalClock();
    }
  } else {
    revisionId = TRI_HybridLogicalClock();
  }

  return getPhysical()->replace(trx, newSlice, result, options,
                                resultMarkerTick, lock, prevRev, previous,
                                revisionId, fromSlice, toSlice);
}

/// @brief removes a document or edge
int LogicalCollection::remove(transaction::Methods* trx,
                              VPackSlice const slice, OperationOptions& options,
                              TRI_voc_tick_t& resultMarkerTick, bool lock,
                              TRI_voc_rid_t& prevRev,
                              ManagedDocumentResult& previous) {
  resultMarkerTick = 0;

  // create remove marker
  TRI_voc_rid_t revisionId = 0;
  if (options.isRestore) {
    VPackSlice oldRev = TRI_ExtractRevisionIdAsSlice(slice);
    if (!oldRev.isString()) {
      revisionId = TRI_HybridLogicalClock();
    } else {
      bool isOld;
      VPackValueLength l;
      char const* p = oldRev.getString(l);
      revisionId = TRI_StringToRid(p, l, isOld, false);
      if (isOld || revisionId == UINT64_MAX) {
        // Do not tolerate old revisions or illegal ones
        revisionId = TRI_HybridLogicalClock();
      }
    }
  } else {
    revisionId = TRI_HybridLogicalClock();
  }

  transaction::BuilderLeaser builder(trx);
  newObjectForRemove(trx, slice, TRI_RidToString(revisionId), *builder.get());

  return getPhysical()->remove(trx, slice, previous, options, resultMarkerTick,
                               lock, revisionId, prevRev, builder->slice());
}

/// @brief removes a document or edge, fast path function for database documents
int LogicalCollection::remove(transaction::Methods* trx,
                              TRI_voc_rid_t oldRevisionId,
                              VPackSlice const oldDoc,
                              OperationOptions& options,
                              TRI_voc_tick_t& resultMarkerTick, bool lock) {
  resultMarkerTick = 0;

  TRI_voc_rid_t revisionId = TRI_HybridLogicalClock();

  // create remove marker
  transaction::BuilderLeaser builder(trx);
  newObjectForRemove(trx, oldDoc, TRI_RidToString(revisionId), *builder.get());
  return getPhysical()->removeFastPath(trx, oldRevisionId, oldDoc, options,
                                       resultMarkerTick, lock, revisionId,
                                       builder->slice());
}

/// @brief rolls back a document operation
int LogicalCollection::rollbackOperation(transaction::Methods* trx,
                                         TRI_voc_document_operation_e type,
                                         TRI_voc_rid_t oldRevisionId,
                                         VPackSlice const& oldDoc,
                                         TRI_voc_rid_t newRevisionId,
                                         VPackSlice const& newDoc) {
  if (type == TRI_VOC_DOCUMENT_OPERATION_INSERT) {
    TRI_ASSERT(oldRevisionId == 0);
    TRI_ASSERT(oldDoc.isNone());
    TRI_ASSERT(newRevisionId != 0);
    TRI_ASSERT(!newDoc.isNone());

    // ignore any errors we're getting from this
    deletePrimaryIndex(trx, newRevisionId, newDoc);
    deleteSecondaryIndexes(trx, newRevisionId, newDoc, true);
    return TRI_ERROR_NO_ERROR;
  }

  if (type == TRI_VOC_DOCUMENT_OPERATION_UPDATE ||
      type == TRI_VOC_DOCUMENT_OPERATION_REPLACE) {
    TRI_ASSERT(oldRevisionId != 0);
    TRI_ASSERT(!oldDoc.isNone());
    TRI_ASSERT(newRevisionId != 0);
    TRI_ASSERT(!newDoc.isNone());
    
    // remove the current values from the indexes
    deleteSecondaryIndexes(trx, newRevisionId, newDoc, true);
    // re-insert old state
    return insertSecondaryIndexes(trx, oldRevisionId, oldDoc, true);
  }

  if (type == TRI_VOC_DOCUMENT_OPERATION_REMOVE) {
    // re-insert old revision
    TRI_ASSERT(oldRevisionId != 0);
    TRI_ASSERT(!oldDoc.isNone());
    TRI_ASSERT(newRevisionId == 0);
    TRI_ASSERT(newDoc.isNone());
    
    int res = insertPrimaryIndex(trx, oldRevisionId, oldDoc);

    if (res == TRI_ERROR_NO_ERROR) {
      res = insertSecondaryIndexes(trx, oldRevisionId, oldDoc, true);
    } else {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "error rolling back remove operation";
    }
    return res;
  }

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "logic error. invalid operation type on rollback";
#endif
  return TRI_ERROR_INTERNAL;
}

void LogicalCollection::sizeHint(transaction::Methods* trx, int64_t hint) {
  if (hint <= 0) {
    return;
  }

  int res = primaryIndex()->resize(trx, static_cast<size_t>(hint * 1.1));

  if (res != TRI_ERROR_NO_ERROR) {
    return;
  }
}

/// @brief initializes an index with a set of existing documents
void LogicalCollection::fillIndex(
    arangodb::basics::LocalTaskQueue* queue, transaction::Methods* trx,
    arangodb::Index* idx,
    std::vector<std::pair<TRI_voc_rid_t, VPackSlice>> const& documents,
    bool skipPersistent) {
  TRI_ASSERT(idx->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  if (!useSecondaryIndexes()) {
    return;  // TRI_ERROR_NO_ERROR;
  }

  if (idx->isPersistent() && skipPersistent) {
    return;  // TRI_ERROR_NO_ERROR;
  }

  try {
    // move task into thread pool
    std::shared_ptr<IndexFillerTask> worker;
    worker.reset(new IndexFillerTask(queue, trx, idx, documents));
    queue->enqueue(worker);
  } catch (...) {
    // set error code
    queue->setStatus(TRI_ERROR_INTERNAL);
  }
}

/// @brief read unlocks a collection
int LogicalCollection::endRead(bool useDeadlockDetector) {
  if (transaction::Methods::_makeNolockHeaders != nullptr) {
    auto it = transaction::Methods::_makeNolockHeaders->find(name());
    if (it != transaction::Methods::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "EndRead blocked: " << _name << std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }

  if (useDeadlockDetector) {
    // unregister reader
    try {
      _vocbase->_deadlockDetector.unsetReader(this);
    } catch (...) {
    }
  }

  // LOCKING-DEBUG
  // std::cout << "EndRead: " << _name << std::endl;
  _idxLock.unlockRead();

  return TRI_ERROR_NO_ERROR;
}

/// @brief write unlocks a collection
int LogicalCollection::endWrite(bool useDeadlockDetector) {
  if (transaction::Methods::_makeNolockHeaders != nullptr) {
    auto it = transaction::Methods::_makeNolockHeaders->find(name());
    if (it != transaction::Methods::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "EndWrite blocked: " << _name <<
      // std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }

  if (useDeadlockDetector) {
    // unregister writer
    try {
      _vocbase->_deadlockDetector.unsetWriter(this);
    } catch (...) {
      // must go on here to unlock the lock
    }
  }

  // LOCKING-DEBUG
  // std::cout << "EndWrite: " << _name << std::endl;
  _idxLock.unlockWrite();

  return TRI_ERROR_NO_ERROR;
}

/// @brief read locks a collection, with a timeout (in µseconds)
int LogicalCollection::beginReadTimed(bool useDeadlockDetector,
                                      double timeout) {
  if (transaction::Methods::_makeNolockHeaders != nullptr) {
    auto it = transaction::Methods::_makeNolockHeaders->find(name());
    if (it != transaction::Methods::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "BeginReadTimed blocked: " << _name <<
      // std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }

  // LOCKING-DEBUG
  // std::cout << "BeginReadTimed: " << _name << std::endl;
  int iterations = 0;
  bool wasBlocked = false;
  double end = 0.0;

  while (true) {
    TRY_READ_LOCKER(locker, _idxLock);

    if (locker.isLocked()) {
      // when we are here, we've got the read lock
      if (useDeadlockDetector) {
        _vocbase->_deadlockDetector.addReader(this, wasBlocked);
      }

      // keep lock and exit loop
      locker.steal();
      return TRI_ERROR_NO_ERROR;
    }

    if (useDeadlockDetector) {
      try {
        if (!wasBlocked) {
          // insert reader
          wasBlocked = true;
          if (_vocbase->_deadlockDetector.setReaderBlocked(this) ==
              TRI_ERROR_DEADLOCK) {
            // deadlock
            LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "deadlock detected while trying to acquire read-lock "
                          "on collection '"
                       << name() << "'";
            return TRI_ERROR_DEADLOCK;
          }
          LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "waiting for read-lock on collection '" << name()
                     << "'";
          // fall-through intentional
        } else if (++iterations >= 5) {
          // periodically check for deadlocks
          TRI_ASSERT(wasBlocked);
          iterations = 0;
          if (_vocbase->_deadlockDetector.detectDeadlock(this, false) ==
              TRI_ERROR_DEADLOCK) {
            // deadlock
            _vocbase->_deadlockDetector.unsetReaderBlocked(this);
            LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "deadlock detected while trying to acquire read-lock "
                          "on collection '"
                       << name() << "'";
            return TRI_ERROR_DEADLOCK;
          }
        }
      } catch (...) {
        // clean up!
        if (wasBlocked) {
          _vocbase->_deadlockDetector.unsetReaderBlocked(this);
        }
        // always exit
        return TRI_ERROR_OUT_OF_MEMORY;
      }
    }

    if (end == 0.0) {
      // set end time for lock waiting
      if (timeout <= 0.0) {
        timeout = 15.0 * 60.0;
      }
      end = TRI_microtime() + timeout;
      TRI_ASSERT(end > 0.0);
    }

    std::this_thread::yield();

    TRI_ASSERT(end > 0.0);

    if (TRI_microtime() > end) {
      if (useDeadlockDetector) {
        _vocbase->_deadlockDetector.unsetReaderBlocked(this);
      }
      LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "timed out waiting for read-lock on collection '" << name()
                 << "'";
      return TRI_ERROR_LOCK_TIMEOUT;
    }
  }
}

/// @brief write locks a collection, with a timeout
int LogicalCollection::beginWriteTimed(bool useDeadlockDetector,
                                       double timeout) {
  if (transaction::Methods::_makeNolockHeaders != nullptr) {
    auto it = transaction::Methods::_makeNolockHeaders->find(name());
    if (it != transaction::Methods::_makeNolockHeaders->end()) {
      // do not lock by command
      // LOCKING-DEBUG
      // std::cout << "BeginWriteTimed blocked: " << _name <<
      // std::endl;
      return TRI_ERROR_NO_ERROR;
    }
  }

  // LOCKING-DEBUG
  // std::cout << "BeginWriteTimed: " << document->_info._name << std::endl;
  int iterations = 0;
  bool wasBlocked = false;
  double end = 0.0;

  while (true) {
    TRY_WRITE_LOCKER(locker, _idxLock);

    if (locker.isLocked()) {
      // register writer
      if (useDeadlockDetector) {
        _vocbase->_deadlockDetector.addWriter(this, wasBlocked);
      }
      // keep lock and exit loop
      locker.steal();
      return TRI_ERROR_NO_ERROR;
    }

    if (useDeadlockDetector) {
      try {
        if (!wasBlocked) {
          // insert writer
          wasBlocked = true;
          if (_vocbase->_deadlockDetector.setWriterBlocked(this) ==
              TRI_ERROR_DEADLOCK) {
            // deadlock
            LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "deadlock detected while trying to acquire "
                          "write-lock on collection '"
                       << name() << "'";
            return TRI_ERROR_DEADLOCK;
          }
          LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "waiting for write-lock on collection '" << name()
                     << "'";
        } else if (++iterations >= 5) {
          // periodically check for deadlocks
          TRI_ASSERT(wasBlocked);
          iterations = 0;
          if (_vocbase->_deadlockDetector.detectDeadlock(this, true) ==
              TRI_ERROR_DEADLOCK) {
            // deadlock
            _vocbase->_deadlockDetector.unsetWriterBlocked(this);
            LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "deadlock detected while trying to acquire "
                          "write-lock on collection '"
                       << name() << "'";
            return TRI_ERROR_DEADLOCK;
          }
        }
      } catch (...) {
        // clean up!
        if (wasBlocked) {
          _vocbase->_deadlockDetector.unsetWriterBlocked(this);
        }
        // always exit
        return TRI_ERROR_OUT_OF_MEMORY;
      }
    }

    std::this_thread::yield();

    if (end == 0.0) {
      // set end time for lock waiting
      if (timeout <= 0.0) {
        timeout = 15.0 * 60.0;
      }
      end = TRI_microtime() + timeout;
      TRI_ASSERT(end > 0.0);
    }

    std::this_thread::yield();

    TRI_ASSERT(end > 0.0);

    if (TRI_microtime() > end) {
      if (useDeadlockDetector) {
        _vocbase->_deadlockDetector.unsetWriterBlocked(this);
      }
      LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "timed out waiting for write-lock on collection '" << name()
                 << "'";
      return TRI_ERROR_LOCK_TIMEOUT;
    }
  }
}

/// @brief checks the revision of a document
int LogicalCollection::checkRevision(transaction::Methods* trx, TRI_voc_rid_t expected,
                                     TRI_voc_rid_t found) {
  if (expected != 0 && found != expected) {
    return TRI_ERROR_ARANGO_CONFLICT;
  }
  return TRI_ERROR_NO_ERROR;
}

/// @brief updates an existing document, low level worker
/// the caller must make sure the write lock on the collection is held
int LogicalCollection::updateDocument(
    transaction::Methods* trx, TRI_voc_rid_t oldRevisionId,
    VPackSlice const& oldDoc, TRI_voc_rid_t newRevisionId,
    VPackSlice const& newDoc, MMFilesDocumentOperation& operation,
    MMFilesWalMarker const* marker, bool& waitForSync) {
  // remove old document from secondary indexes
  // (it will stay in the primary index as the key won't change)
  int res = deleteSecondaryIndexes(trx, oldRevisionId, oldDoc, false);

  if (res != TRI_ERROR_NO_ERROR) {
    // re-enter the document in case of failure, ignore errors during rollback
    insertSecondaryIndexes(trx, oldRevisionId, oldDoc, true);
    return res;
  }

  // insert new document into secondary indexes
  res = insertSecondaryIndexes(trx, newRevisionId, newDoc, false);

  if (res != TRI_ERROR_NO_ERROR) {
    // rollback
    deleteSecondaryIndexes(trx, newRevisionId, newDoc, true);
    insertSecondaryIndexes(trx, oldRevisionId, oldDoc, true);
    return res;
  }

  // update the index element (primary index only - other index have been
  // adjusted)
  VPackSlice keySlice(transaction::Methods::extractKeyFromDocument(newDoc));
  MMFilesSimpleIndexElement* element = primaryIndex()->lookupKeyRef(trx, keySlice);
  if (element != nullptr && element->revisionId() != 0) {
    element->updateRevisionId(
        newRevisionId,
        static_cast<uint32_t>(keySlice.begin() - newDoc.begin()));
  }

  operation.indexed();
   
  if (oldRevisionId != newRevisionId) { 
    try {
      removeRevision(oldRevisionId, true);
    } catch (...) {
    }
  }
  
  TRI_IF_FAILURE("UpdateDocumentNoOperation") { return TRI_ERROR_DEBUG; }

  TRI_IF_FAILURE("UpdateDocumentNoOperationExcept") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  return static_cast<MMFilesTransactionState*>(trx->state())->addOperation(newRevisionId, operation, marker, waitForSync);
}

/// @brief creates a new entry in the primary index
int LogicalCollection::insertPrimaryIndex(transaction::Methods* trx,
                                          TRI_voc_rid_t revisionId,
                                          VPackSlice const& doc) {
  TRI_IF_FAILURE("InsertPrimaryIndex") { return TRI_ERROR_DEBUG; }

  // insert into primary index
  return primaryIndex()->insertKey(trx, revisionId, doc);
}

/// @brief deletes an entry from the primary index
int LogicalCollection::deletePrimaryIndex(transaction::Methods* trx,
                                          TRI_voc_rid_t revisionId,
                                          VPackSlice const& doc) {
  TRI_IF_FAILURE("DeletePrimaryIndex") { return TRI_ERROR_DEBUG; }

  return primaryIndex()->removeKey(trx, revisionId, doc);
}

/// @brief creates a new entry in the secondary indexes
int LogicalCollection::insertSecondaryIndexes(transaction::Methods* trx,
                                              TRI_voc_rid_t revisionId,
                                              VPackSlice const& doc,
                                              bool isRollback) {
  // Coordintor doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_IF_FAILURE("InsertSecondaryIndexes") { return TRI_ERROR_DEBUG; }

  bool const useSecondary = useSecondaryIndexes();
  if (!useSecondary && _persistentIndexes == 0) {
    return TRI_ERROR_NO_ERROR;
  }

  int result = TRI_ERROR_NO_ERROR;

  size_t const n = _indexes.size();

  for (size_t i = 1; i < n; ++i) {
    auto idx = _indexes[i];
    TRI_ASSERT(idx->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);

    if (!useSecondary && !idx->isPersistent()) {
      continue;
    }

    int res = idx->insert(trx, revisionId, doc, isRollback);

    // in case of no-memory, return immediately
    if (res == TRI_ERROR_OUT_OF_MEMORY) {
      return res;
    }
    if (res != TRI_ERROR_NO_ERROR) {
      if (res == TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED ||
          result == TRI_ERROR_NO_ERROR) {
        // "prefer" unique constraint violated
        result = res;
      }
    }
  }

  return result;
}

/// @brief deletes an entry from the secondary indexes
int LogicalCollection::deleteSecondaryIndexes(transaction::Methods* trx,
                                              TRI_voc_rid_t revisionId,
                                              VPackSlice const& doc,
                                              bool isRollback) {
  // Coordintor doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());

  bool const useSecondary = useSecondaryIndexes();
  if (!useSecondary && _persistentIndexes == 0) {
    return TRI_ERROR_NO_ERROR;
  }

  TRI_IF_FAILURE("DeleteSecondaryIndexes") { return TRI_ERROR_DEBUG; }

  int result = TRI_ERROR_NO_ERROR;

  size_t const n = _indexes.size();

  for (size_t i = 1; i < n; ++i) {
    auto idx = _indexes[i];
    TRI_ASSERT(idx->type() != Index::IndexType::TRI_IDX_TYPE_PRIMARY_INDEX);

    if (!useSecondary && !idx->isPersistent()) {
      continue;
    }

    int res = idx->remove(trx, revisionId, doc, isRollback);

    if (res != TRI_ERROR_NO_ERROR) {
      // an error occurred
      result = res;
    }
  }

  return result;
}

/// @brief new object for insert, computes the hash of the key
int LogicalCollection::newObjectForInsert(
    transaction::Methods* trx, VPackSlice const& value, VPackSlice const& fromSlice,
    VPackSlice const& toSlice, bool isEdgeCollection, VPackBuilder& builder,
    bool isRestore) {
  TRI_voc_tick_t newRev = 0;
  builder.openObject();

  // add system attributes first, in this order:
  // _key, _id, _from, _to, _rev

  // _key
  VPackSlice s = value.get(StaticStrings::KeyString);
  if (s.isNone()) {
    TRI_ASSERT(!isRestore);  // need key in case of restore
    newRev = TRI_HybridLogicalClock();
    std::string keyString = _keyGenerator->generate(TRI_NewTickServer());
    if (keyString.empty()) {
      return TRI_ERROR_ARANGO_OUT_OF_KEYS;
    }
    uint8_t* where =
        builder.add(StaticStrings::KeyString, VPackValue(keyString));
    s = VPackSlice(where);  // point to newly built value, the string
  } else if (!s.isString()) {
    return TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD;
  } else {
    std::string keyString = s.copyString();
    int res = _keyGenerator->validate(keyString, isRestore);
    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
    builder.add(StaticStrings::KeyString, s);
  }

  // _id
  uint8_t* p = builder.add(StaticStrings::IdString,
                           VPackValuePair(9ULL, VPackValueType::Custom));
  *p++ = 0xf3;  // custom type for _id
  if (ServerState::isDBServer(trx->serverRole()) && !_isSystem) {
    // db server in cluster, note: the local collections _statistics,
    // _statisticsRaw and _statistics15 (which are the only system
    // collections)
    // must not be treated as shards but as local collections
    encoding::storeNumber<uint64_t>(p, _planId, sizeof(uint64_t));
  } else {
    // local server
    encoding::storeNumber<uint64_t>(p, _cid, sizeof(uint64_t));
  }

  // _from and _to
  if (isEdgeCollection) {
    TRI_ASSERT(!fromSlice.isNone());
    TRI_ASSERT(!toSlice.isNone());
    builder.add(StaticStrings::FromString, fromSlice);
    builder.add(StaticStrings::ToString, toSlice);
  }

  // _rev
  std::string newRevSt;
  if (isRestore) {
    VPackSlice oldRev = TRI_ExtractRevisionIdAsSlice(value);
    if (!oldRev.isString()) {
      return TRI_ERROR_ARANGO_DOCUMENT_REV_BAD;
    }
    bool isOld;
    VPackValueLength l;
    char const* p = oldRev.getString(l);
    TRI_voc_rid_t oldRevision = TRI_StringToRid(p, l, isOld, false);
    if (isOld || oldRevision == UINT64_MAX) {
      oldRevision = TRI_HybridLogicalClock();
    }
    newRevSt = TRI_RidToString(oldRevision);
  } else {
    if (newRev == 0) {
      newRev = TRI_HybridLogicalClock();
    }
    newRevSt = TRI_RidToString(newRev);
  }
  builder.add(StaticStrings::RevString, VPackValue(newRevSt));

  // add other attributes after the system attributes
  TRI_SanitizeObjectWithEdges(value, builder);

  builder.close();
  return TRI_ERROR_NO_ERROR;
}

/// @brief new object for remove, must have _key set
void LogicalCollection::newObjectForRemove(transaction::Methods* trx,
                                           VPackSlice const& oldValue,
                                           std::string const& rev,
                                           VPackBuilder& builder) {
  // create an object consisting of _key and _rev (in this order)
  builder.openObject();
  if (oldValue.isString()) {
    builder.add(StaticStrings::KeyString, oldValue);
  } else {
    VPackSlice s = oldValue.get(StaticStrings::KeyString);
    TRI_ASSERT(s.isString());
    builder.add(StaticStrings::KeyString, s);
  }
  builder.add(StaticStrings::RevString, VPackValue(rev));
  builder.close();
}

bool LogicalCollection::readRevision(transaction::Methods* trx,
                                     ManagedDocumentResult& result,
                                     TRI_voc_rid_t revisionId) {
  uint8_t const* vpack = getPhysical()->lookupRevisionVPack(revisionId);
  if (vpack != nullptr) {
    result.addExisting(vpack, revisionId);
    return true;
  } 
  return false;
}

bool LogicalCollection::readRevisionConditional(transaction::Methods* trx,
                                                ManagedDocumentResult& result,
                                                TRI_voc_rid_t revisionId,
                                                TRI_voc_tick_t maxTick,
                                                bool excludeWal) {
  TRI_ASSERT(revisionId != 0);
  uint8_t const* vpack  = getPhysical()->lookupRevisionVPackConditional(revisionId, maxTick, excludeWal);
  if (vpack != nullptr) {
    result.addExisting(vpack, revisionId);
    return true;
  } 
  return false;
}

// TODO ONLY TEMP wrapper
bool LogicalCollection::readDocument(transaction::Methods* trx, ManagedDocumentResult& result, DocumentIdentifierToken const& token) {
  // TODO This only works for MMFiles Engine. Has to be moved => StorageEngine
  auto tkn = static_cast<MMFilesToken const*>(&token);
  return readRevision(trx, result, tkn->revisionId());
}

// TODO ONLY TEMP wrapper
bool LogicalCollection::readDocumentConditional(transaction::Methods* trx,
                                                ManagedDocumentResult& result,
                                                DocumentIdentifierToken const& token,
                                                TRI_voc_tick_t maxTick,
                                                bool excludeWal) {
  // TODO This only works for MMFiles Engine. Has to be moved => StorageEngine
  auto tkn = static_cast<MMFilesToken const*>(&token);
  return readRevisionConditional(trx, result, tkn->revisionId(), maxTick, excludeWal);
}

void LogicalCollection::insertRevision(TRI_voc_rid_t revisionId,
                                       uint8_t const* dataptr,
                                       TRI_voc_fid_t fid, bool isInWal) {
  // note: there is no need to insert into the cache here as the data points
  // to
  // temporary storage
  getPhysical()->insertRevision(revisionId, dataptr, fid, isInWal, true);
}

void LogicalCollection::updateRevision(TRI_voc_rid_t revisionId,
                                       uint8_t const* dataptr,
                                       TRI_voc_fid_t fid, bool isInWal) {
  // note: there is no need to modify the cache entry here as insertRevision
  // has
  // not inserted the document into the cache
  getPhysical()->updateRevision(revisionId, dataptr, fid, isInWal);
}

bool LogicalCollection::updateRevisionConditional(
    TRI_voc_rid_t revisionId, TRI_df_marker_t const* oldPosition,
    TRI_df_marker_t const* newPosition, TRI_voc_fid_t newFid, bool isInWal) {
  return getPhysical()->updateRevisionConditional(revisionId, oldPosition,
                                                  newPosition, newFid, isInWal);
}

void LogicalCollection::removeRevision(TRI_voc_rid_t revisionId,
                                       bool updateStats) {
  // and remove from storage engine
  getPhysical()->removeRevision(revisionId, updateStats);
}

/// @brief a method to skip certain documents in AQL write operations,
/// this is only used in the enterprise edition for smart graphs
#ifndef USE_ENTERPRISE
bool LogicalCollection::skipForAqlWrite(arangodb::velocypack::Slice document,
                                        std::string const& key) const {
  return false;
}
#endif

bool LogicalCollection::isSatellite() const { return _replicationFactor == 0; }
