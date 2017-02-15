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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "MMFilesCollection.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/FileUtils.h"
#include "Basics/ReadLocker.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/encoding.h"
#include "Basics/process-utils.h"
#include "Cluster/ClusterMethods.h"
#include "Logger/Logger.h"
#include "MMFiles/MMFilesDatafile.h"
#include "MMFiles/MMFilesDatafileHelper.h"
#include "MMFiles/MMFilesDocumentOperation.h"
#include "MMFiles/MMFilesDocumentPosition.h"
#include "MMFiles/MMFilesIndexElement.h"
#include "MMFiles/MMFilesLogfileManager.h"
#include "MMFiles/MMFilesPrimaryIndex.h"
#include "MMFiles/MMFilesTransactionState.h"
#include "RestServer/DatabaseFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/CollectionReadLocker.h"
#include "Utils/CollectionWriteLocker.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Utils/StandaloneTransactionContext.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"

using namespace arangodb;

namespace {

/// @brief find a statistics container for a given file id
static DatafileStatisticsContainer* FindDatafileStats(
    MMFilesCollection::OpenIteratorState* state, TRI_voc_fid_t fid) {
  auto it = state->_stats.find(fid);

  if (it != state->_stats.end()) {
    return (*it).second;
  }

  auto stats = std::make_unique<DatafileStatisticsContainer>();
  state->_stats.emplace(fid, stats.get());
  return stats.release();
}

} // namespace

/// @brief process a document (or edge) marker when opening a collection
int MMFilesCollection::OpenIteratorHandleDocumentMarker(TRI_df_marker_t const* marker,
                                                        MMFilesDatafile* datafile,
                                                        MMFilesCollection::OpenIteratorState* state) {
  LogicalCollection* collection = state->_collection;
  MMFilesCollection* c = static_cast<MMFilesCollection*>(collection->getPhysical());
  transaction::Methods* trx = state->_trx;
  TRI_ASSERT(trx != nullptr);

  VPackSlice const slice(reinterpret_cast<char const*>(marker) + MMFilesDatafileHelper::VPackOffset(TRI_DF_MARKER_VPACK_DOCUMENT));
  uint8_t const* vpack = slice.begin();

  VPackSlice keySlice;
  TRI_voc_rid_t revisionId;

  transaction::Methods::extractKeyAndRevFromDocument(slice, keySlice, revisionId);

  c->setRevision(revisionId, false);

  if (state->_trackKeys) {
    VPackValueLength length;
    char const* p = keySlice.getString(length);
    collection->keyGenerator()->track(p, length);
  }

  ++state->_documents;
 
  TRI_voc_fid_t const fid = datafile->fid();
  if (state->_fid != fid) {
    // update the state
    state->_fid = fid; // when we're here, we're looking at a datafile
    state->_dfi = FindDatafileStats(state, fid);
  }

  // no primary index lock required here because we are the only ones reading
  // from the index ATM
  MMFilesSimpleIndexElement* found = state->_primaryIndex->lookupKeyRef(trx, keySlice, state->_mmdr);

  // it is a new entry
  if (found == nullptr || found->revisionId() == 0) {
    c->insertRevision(revisionId, vpack, fid, false, false); 

    // insert into primary index
    int res = state->_primaryIndex->insertKey(trx, revisionId, VPackSlice(vpack), state->_mmdr);

    if (res != TRI_ERROR_NO_ERROR) {
      c->removeRevision(revisionId, false);
      LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "inserting document into primary index failed with error: " << TRI_errno_string(res);

      return res;
    }

    // update the datafile info
    state->_dfi->numberAlive++;
    state->_dfi->sizeAlive += MMFilesDatafileHelper::AlignedMarkerSize<int64_t>(marker);
  }

  // it is an update
  else {
    TRI_voc_rid_t const oldRevisionId = found->revisionId();
    // update the revision id in primary index
    found->updateRevisionId(revisionId, static_cast<uint32_t>(keySlice.begin() - vpack));

    MMFilesDocumentPosition const old = c->lookupRevision(oldRevisionId);

    // remove old revision
    c->removeRevision(oldRevisionId, false);

    // insert new revision
    c->insertRevision(revisionId, vpack, fid, false, false);

    // update the datafile info
    DatafileStatisticsContainer* dfi;
    if (old.fid() == state->_fid) {
      dfi = state->_dfi;
    } else {
      dfi = FindDatafileStats(state, old.fid());
    }

    if (old.dataptr() != nullptr) { 
      uint8_t const* vpack = static_cast<uint8_t const*>(old.dataptr());
      int64_t size = static_cast<int64_t>(MMFilesDatafileHelper::VPackOffset(TRI_DF_MARKER_VPACK_DOCUMENT) + VPackSlice(vpack).byteSize());

      dfi->numberAlive--;
      dfi->sizeAlive -= encoding::alignedSize<int64_t>(size);
      dfi->numberDead++;
      dfi->sizeDead += encoding::alignedSize<int64_t>(size);
    }

    state->_dfi->numberAlive++;
    state->_dfi->sizeAlive += MMFilesDatafileHelper::AlignedMarkerSize<int64_t>(marker);
  }

  return TRI_ERROR_NO_ERROR;
}

/// @brief process a deletion marker when opening a collection
int MMFilesCollection::OpenIteratorHandleDeletionMarker(TRI_df_marker_t const* marker,
                                                        MMFilesDatafile* datafile,
                                                        MMFilesCollection::OpenIteratorState* state) {
  LogicalCollection* collection = state->_collection;
  MMFilesCollection* c = static_cast<MMFilesCollection*>(collection->getPhysical());
  transaction::Methods* trx = state->_trx;

  VPackSlice const slice(reinterpret_cast<char const*>(marker) + MMFilesDatafileHelper::VPackOffset(TRI_DF_MARKER_VPACK_REMOVE));
  
  VPackSlice keySlice;
  TRI_voc_rid_t revisionId;

  transaction::Methods::extractKeyAndRevFromDocument(slice, keySlice, revisionId);
  
  c->setRevision(revisionId, false);
  if (state->_trackKeys) {
    VPackValueLength length;
    char const* p = keySlice.getString(length);
    collection->keyGenerator()->track(p, length);
  }

  ++state->_deletions;

  if (state->_fid != datafile->fid()) {
    // update the state
    state->_fid = datafile->fid();
    state->_dfi = FindDatafileStats(state, datafile->fid());
  }

  // no primary index lock required here because we are the only ones reading
  // from the index ATM
  MMFilesSimpleIndexElement found = state->_primaryIndex->lookupKey(trx, keySlice, state->_mmdr);

  // it is a new entry, so we missed the create
  if (!found) {
    // update the datafile info
    state->_dfi->numberDeletions++;
  }

  // it is a real delete
  else {
    TRI_voc_rid_t oldRevisionId = found.revisionId();

    MMFilesDocumentPosition const old = c->lookupRevision(oldRevisionId);
    
    // update the datafile info
    DatafileStatisticsContainer* dfi;

    if (old.fid() == state->_fid) {
      dfi = state->_dfi;
    } else {
      dfi = FindDatafileStats(state, old.fid());
    }

    TRI_ASSERT(old.dataptr() != nullptr);

    uint8_t const* vpack = static_cast<uint8_t const*>(old.dataptr());
    int64_t size = encoding::alignedSize<int64_t>(MMFilesDatafileHelper::VPackOffset(TRI_DF_MARKER_VPACK_DOCUMENT) + VPackSlice(vpack).byteSize());

    dfi->numberAlive--;
    dfi->sizeAlive -= encoding::alignedSize<int64_t>(size);
    dfi->numberDead++;
    dfi->sizeDead += encoding::alignedSize<int64_t>(size);
    state->_dfi->numberDeletions++;

    state->_primaryIndex->removeKey(trx, oldRevisionId, VPackSlice(vpack), state->_mmdr);

    c->removeRevision(oldRevisionId, true);
  }

  return TRI_ERROR_NO_ERROR;
}

/// @brief iterator for open
bool MMFilesCollection::OpenIterator(TRI_df_marker_t const* marker, MMFilesCollection::OpenIteratorState* data,
                                     MMFilesDatafile* datafile) {
  TRI_voc_tick_t const tick = marker->getTick();
  TRI_df_marker_type_t const type = marker->getType();

  int res;

  if (type == TRI_DF_MARKER_VPACK_DOCUMENT) {
    res = OpenIteratorHandleDocumentMarker(marker, datafile, data);

    if (datafile->_dataMin == 0) {
      datafile->_dataMin = tick;
    }

    if (tick > datafile->_dataMax) {
      datafile->_dataMax = tick;
    }
    
    if (++data->_operations % 1024 == 0) {
      data->_mmdr.clear();
    }
  } else if (type == TRI_DF_MARKER_VPACK_REMOVE) {
    res = OpenIteratorHandleDeletionMarker(marker, datafile, data);
    if (++data->_operations % 1024 == 0) {
      data->_mmdr.clear();
    }
  } else {
    if (type == TRI_DF_MARKER_HEADER) {
      // ensure there is a datafile info entry for each datafile of the
      // collection
      FindDatafileStats(data, datafile->fid());
    }

    LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "skipping marker type " << TRI_NameMarkerDatafile(marker);
    res = TRI_ERROR_NO_ERROR;
  }

  if (datafile->_tickMin == 0) {
    datafile->_tickMin = tick;
  }

  if (tick > datafile->_tickMax) {
    datafile->_tickMax = tick;
  }

  if (tick > data->_collection->maxTick()) {
    if (type != TRI_DF_MARKER_HEADER &&
        type != TRI_DF_MARKER_FOOTER &&
        type != TRI_DF_MARKER_COL_HEADER &&
        type != TRI_DF_MARKER_PROLOGUE) {
      data->_collection->maxTick(tick);
    }
  }

  return (res == TRI_ERROR_NO_ERROR);
}

MMFilesCollection::MMFilesCollection(LogicalCollection* collection)
    : PhysicalCollection(collection)
    , _ditches(collection)
    , _initialCount(0), _lastRevision(0)
    , _uncollectedLogfileEntries(0)
    ,  _nextCompactionStartIndex(0)
    ,  _lastCompactionStatus(nullptr)
    ,  _lastCompactionStamp(0.0)
    {
      setCompactionStatus("compaction not yet started");
    }

MMFilesCollection::~MMFilesCollection() { 
  try {
    close(); 
  } catch (...) {
    // dtor must not propagate exceptions
  }
}

TRI_voc_rid_t MMFilesCollection::revision() const { 
  return _lastRevision; 
}

void MMFilesCollection::setRevision(TRI_voc_rid_t revision, bool force) {
  if (force || revision > _lastRevision) {
    _lastRevision = revision;
  }
}

int64_t MMFilesCollection::initialCount() const { 
  return _initialCount;
}

void MMFilesCollection::updateCount(int64_t count) {
  _initialCount = count;
}
  
/// @brief closes an open collection
int MMFilesCollection::close() {
  {
    WRITE_LOCKER(writeLocker, _filesLock);

    // close compactor files
    closeDatafiles(_compactors);
    for (auto& it : _compactors) {
      delete it;
    }
    _compactors.clear();

    // close journal files
    closeDatafiles(_journals);
    for (auto& it : _journals) {
      delete it;
    }
    _journals.clear();

    // close datafiles
    closeDatafiles(_datafiles);
    for (auto& it : _datafiles) {
      delete it;
    }
    _datafiles.clear();
  }

  _lastRevision = 0;

  // clear revisions lookup table
  _revisionsCache.clear();

  return TRI_ERROR_NO_ERROR;
}

/// @brief seal a datafile
int MMFilesCollection::sealDatafile(MMFilesDatafile* datafile, bool isCompactor) {
  int res = datafile->seal();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "failed to seal journal '" << datafile->getName()
             << "': " << TRI_errno_string(res);
    return res;
  }

  if (!isCompactor && datafile->isPhysical()) {
    // rename the file
    std::string dname("datafile-" + std::to_string(datafile->fid()) + ".db");
    std::string filename = arangodb::basics::FileUtils::buildFilename(path(), dname);

    res = datafile->rename(filename);

    if (res == TRI_ERROR_NO_ERROR) {
      LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "closed file '" << datafile->getName() << "'";
    } else {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "failed to rename datafile '" << datafile->getName()
               << "' to '" << filename << "': " << TRI_errno_string(res);
    }
  }

  return res;
}

/// @brief rotate the active journal - will do nothing if there is no journal
int MMFilesCollection::rotateActiveJournal() {
  WRITE_LOCKER(writeLocker, _filesLock);

  // note: only journals need to be handled here as the journal is the
  // only place that's ever written to. if a journal is full, it will have been
  // sealed and synced already
  if (_journals.empty()) {
    return TRI_ERROR_ARANGO_NO_JOURNAL;
  }

  MMFilesDatafile* datafile = _journals[0];
  TRI_ASSERT(datafile != nullptr);

  // make sure we have enough room in the target vector before we go on
  _datafiles.reserve(_datafiles.size() + 1);

  int res = sealDatafile(datafile, false);
 
  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }
   
  // shouldn't throw as we reserved enough space before
  _datafiles.emplace_back(datafile);


  TRI_ASSERT(!_journals.empty());
  TRI_ASSERT(_journals.back() == datafile);
  _journals.erase(_journals.begin());
  TRI_ASSERT(_journals.empty());

  return res;
}

/// @brief sync the active journal - will do nothing if there is no journal
/// or if the journal is volatile
int MMFilesCollection::syncActiveJournal() {
  WRITE_LOCKER(writeLocker, _filesLock);

  // note: only journals need to be handled here as the journal is the
  // only place that's ever written to. if a journal is full, it will have been
  // sealed and synced already
  if (_journals.empty()) {
    // nothing to do
    return TRI_ERROR_NO_ERROR;
  }

  MMFilesDatafile* datafile = _journals[0];
  TRI_ASSERT(datafile != nullptr);

  int res = TRI_ERROR_NO_ERROR;

  // we only need to care about physical datafiles
  // anonymous regions do not need to be synced
  if (datafile->isPhysical()) {
    char const* synced = datafile->_synced;
    char* written = datafile->_written;

    if (synced < written) {
      bool ok = datafile->sync(synced, written);

      if (ok) {
        LOG_TOPIC(TRACE, Logger::COLLECTOR) << "msync succeeded "
                                            << (void*)synced << ", size "
                                            << (written - synced);
        datafile->_synced = written;
      } else {
        res = TRI_errno();
        if (res == TRI_ERROR_NO_ERROR) {
          // oops, error code got lost
          res = TRI_ERROR_INTERNAL;
        }

        LOG_TOPIC(ERR, Logger::COLLECTOR)
            << "msync failed with: " << TRI_last_error();
        datafile->setState(TRI_DF_STATE_WRITE_ERROR);
      }
    }
  }

  return res;
}

/// @brief reserve space in the current journal. if no create exists or the
/// current journal cannot provide enough space, close the old journal and
/// create a new one
int MMFilesCollection::reserveJournalSpace(TRI_voc_tick_t tick,
                                           TRI_voc_size_t size,
                                           char*& resultPosition,
                                           MMFilesDatafile*& resultDatafile) {

  // reset results
  resultPosition = nullptr;
  resultDatafile = nullptr;

  WRITE_LOCKER(writeLocker, _filesLock);

  // start with configured journal size
  TRI_voc_size_t targetSize = static_cast<TRI_voc_size_t>(_logicalCollection->journalSize());

  // make sure that the document fits
  while (targetSize - 256 < size) {
    targetSize *= 2;
  }

  while (true) {
    // no need to go on if the collection is already deleted
    if (_logicalCollection->status() == TRI_VOC_COL_STATUS_DELETED) {
      return TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND;
    }

    MMFilesDatafile* datafile = nullptr;

    if (_journals.empty()) {
      // create enough room in the journals vector
      _journals.reserve(_journals.size() + 1);

      try {
        std::unique_ptr<MMFilesDatafile> df(createDatafile(tick, targetSize, false));

        // shouldn't throw as we reserved enough space before
        _journals.emplace_back(df.get());
        df.release();
      } catch (basics::Exception const& ex) {
        LOG_TOPIC(ERR, Logger::COLLECTOR) << "cannot select journal: " << ex.what();
        return ex.code();
      } catch (std::exception const& ex) {
        LOG_TOPIC(ERR, Logger::COLLECTOR) << "cannot select journal: " << ex.what();
        return TRI_ERROR_INTERNAL;
      } catch (...) {
        LOG_TOPIC(ERR, Logger::COLLECTOR) << "cannot select journal: unknown exception";
        return TRI_ERROR_INTERNAL;
      }
    } 
      
    // select datafile
    TRI_ASSERT(!_journals.empty());
    datafile = _journals[0];

    TRI_ASSERT(datafile != nullptr);

    // try to reserve space in the datafile
    TRI_df_marker_t* position = nullptr;
    int res = datafile->reserveElement(size, &position, targetSize);

    // found a datafile with enough space left
    if (res == TRI_ERROR_NO_ERROR) {
      datafile->_written = ((char*)position) + size;
      // set result
      resultPosition = reinterpret_cast<char*>(position);
      resultDatafile = datafile;
      return TRI_ERROR_NO_ERROR;
    }

    if (res != TRI_ERROR_ARANGO_DATAFILE_FULL) {
      // some other error
      LOG_TOPIC(ERR, Logger::COLLECTOR) << "cannot select journal: '"
                                        << TRI_last_error() << "'";
      return res;
    }

    // TRI_ERROR_ARANGO_DATAFILE_FULL...
    // journal is full, close it and sync
    LOG_TOPIC(DEBUG, Logger::COLLECTOR) << "closing full journal '"
                                        << datafile->getName() << "'";

    // make sure we have enough room in the target vector before we go on
    _datafiles.reserve(_datafiles.size() + 1);

    res = sealDatafile(datafile, false);
    
    // move journal into _datafiles vector
    // this shouldn't fail, as we have reserved space before already
    _datafiles.emplace_back(datafile);

    // and finally erase it from _journals vector
    TRI_ASSERT(!_journals.empty());
    TRI_ASSERT(_journals.back() == datafile);
    _journals.erase(_journals.begin());
    TRI_ASSERT(_journals.empty());

    if (res != TRI_ERROR_NO_ERROR) {
      // an error occurred, we must stop here
      return res;
    }
  }  // otherwise, next iteration!

  return TRI_ERROR_ARANGO_NO_JOURNAL;
}

/// @brief create compactor file
MMFilesDatafile* MMFilesCollection::createCompactor(TRI_voc_fid_t fid,
                                                   TRI_voc_size_t maximalSize) {
  WRITE_LOCKER(writeLocker, _filesLock);

  TRI_ASSERT(_compactors.empty());
  // reserve enough space for the later addition
  _compactors.reserve(_compactors.size() + 1);

  std::unique_ptr<MMFilesDatafile> compactor(createDatafile(fid, static_cast<TRI_voc_size_t>(maximalSize), true));

  // should not throw, as we've reserved enough space before
  _compactors.emplace_back(compactor.get());
  return compactor.release();
}

/// @brief close an existing compactor
int MMFilesCollection::closeCompactor(MMFilesDatafile* datafile) {
  WRITE_LOCKER(writeLocker, _filesLock);

  if (_compactors.size() != 1) {
    return TRI_ERROR_ARANGO_NO_JOURNAL;
  }

  MMFilesDatafile* compactor = _compactors[0];

  if (datafile != compactor) {
    // wrong compactor file specified... should not happen
    return TRI_ERROR_INTERNAL;
  }

  return sealDatafile(datafile, true);
}

/// @brief replace a datafile with a compactor
int MMFilesCollection::replaceDatafileWithCompactor(MMFilesDatafile* datafile,
                                                    MMFilesDatafile* compactor) {
  TRI_ASSERT(datafile != nullptr);
  TRI_ASSERT(compactor != nullptr);

  WRITE_LOCKER(writeLocker, _filesLock);

  TRI_ASSERT(!_compactors.empty());
  
  for (size_t i = 0; i < _datafiles.size(); ++i) {
    if (_datafiles[i]->fid() == datafile->fid()) {
      // found!
      // now put the compactor in place of the datafile
      _datafiles[i] = compactor;

      // remove the compactor file from the list of compactors
      TRI_ASSERT(_compactors[0] != nullptr);
      TRI_ASSERT(_compactors[0]->fid() == compactor->fid());

      _compactors.erase(_compactors.begin());
      TRI_ASSERT(_compactors.empty());

      return TRI_ERROR_NO_ERROR;
    }
  }

  return TRI_ERROR_INTERNAL;
}

/// @brief creates a datafile
MMFilesDatafile* MMFilesCollection::createDatafile(TRI_voc_fid_t fid,
                                                  TRI_voc_size_t journalSize,
                                                  bool isCompactor) {
  TRI_ASSERT(fid > 0);

  // create an entry for the new datafile
  try {
    _datafileStatistics.create(fid);
  } catch (...) {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY);
  }

  std::unique_ptr<MMFilesDatafile> datafile;

  if (_logicalCollection->isVolatile()) {
    // in-memory collection
    datafile.reset(MMFilesDatafile::create(StaticStrings::Empty, fid, journalSize, true));
  } else {
    // construct a suitable filename (which may be temporary at the beginning)
    std::string jname;
    if (isCompactor) {
      jname = "compaction-";
    } else {
      jname = "temp-";
    }

    jname.append(std::to_string(fid) + ".db");
    std::string filename = arangodb::basics::FileUtils::buildFilename(path(), jname);

    TRI_IF_FAILURE("CreateJournalDocumentCollection") {
      // simulate disk full
      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_FILESYSTEM_FULL);
    }

    // remove an existing temporary file first
    if (TRI_ExistsFile(filename.c_str())) {
      // remove an existing file first
      TRI_UnlinkFile(filename.c_str());
    }

    datafile.reset(MMFilesDatafile::create(filename, fid, journalSize, true));
  }

  if (datafile == nullptr) {
    if (TRI_errno() == TRI_ERROR_OUT_OF_MEMORY_MMAP) {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_OUT_OF_MEMORY_MMAP);
    }
    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_NO_JOURNAL);
  }

  // datafile is there now
  TRI_ASSERT(datafile != nullptr);

  if (isCompactor) {
    LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "created new compactor '" << datafile->getName()
               << "'";
  } else {
    LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "created new journal '" << datafile->getName() << "'";
  }

  // create a collection header, still in the temporary file
  TRI_df_marker_t* position;
  int res = datafile->reserveElement(sizeof(TRI_col_header_marker_t), &position, journalSize);

  TRI_IF_FAILURE("CreateJournalDocumentCollectionReserve1") {
    res = TRI_ERROR_DEBUG;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "cannot create collection header in file '"
             << datafile->getName() << "': " << TRI_errno_string(res);

    // close the journal and remove it
    std::string temp(datafile->getName());
    datafile.reset();
    TRI_UnlinkFile(temp.c_str());

    THROW_ARANGO_EXCEPTION(res);
  }

  TRI_col_header_marker_t cm;
  MMFilesDatafileHelper::InitMarker(
      reinterpret_cast<TRI_df_marker_t*>(&cm), TRI_DF_MARKER_COL_HEADER,
      sizeof(TRI_col_header_marker_t), static_cast<TRI_voc_tick_t>(fid));
  cm._cid = _logicalCollection->cid();

  res = datafile->writeCrcElement(position, &cm.base, false);

  TRI_IF_FAILURE("CreateJournalDocumentCollectionReserve2") {
    res = TRI_ERROR_DEBUG;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    int res = datafile->_lastError;
    LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "cannot create collection header in file '"
             << datafile->getName() << "': " << TRI_last_error();

    // close the datafile and remove it
    std::string temp(datafile->getName());
    datafile.reset();
    TRI_UnlinkFile(temp.c_str());

    THROW_ARANGO_EXCEPTION(res);
  }

  TRI_ASSERT(fid == datafile->fid());

  // if a physical file, we can rename it from the temporary name to the correct
  // name
  if (!isCompactor && datafile->isPhysical()) {
    // and use the correct name
    std::string jname("journal-" + std::to_string(datafile->fid()) + ".db");
    std::string filename = arangodb::basics::FileUtils::buildFilename(path(), jname);

    int res = datafile->rename(filename);

    if (res != TRI_ERROR_NO_ERROR) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "failed to rename journal '" << datafile->getName()
               << "' to '" << filename << "': " << TRI_errno_string(res);

      std::string temp(datafile->getName());
      datafile.reset();
      TRI_UnlinkFile(temp.c_str());

      THROW_ARANGO_EXCEPTION(res);
    } 
      
    LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "renamed journal from '" << datafile->getName()
               << "' to '" << filename << "'";
  }

  return datafile.release();
}

/// @brief remove a compactor file from the list of compactors
bool MMFilesCollection::removeCompactor(MMFilesDatafile* df) {
  TRI_ASSERT(df != nullptr);

  WRITE_LOCKER(writeLocker, _filesLock);

  for (auto it = _compactors.begin(); it != _compactors.end(); ++it) {
    if ((*it) == df) {
      // and finally remove the file from the _compactors vector
      _compactors.erase(it);
      return true;
    }
  }

  // not found
  return false;
}

/// @brief remove a datafile from the list of datafiles
bool MMFilesCollection::removeDatafile(MMFilesDatafile* df) {
  TRI_ASSERT(df != nullptr);

  WRITE_LOCKER(writeLocker, _filesLock);

  for (auto it = _datafiles.begin(); it != _datafiles.end(); ++it) {
    if ((*it) == df) {
      // and finally remove the file from the _compactors vector
      _datafiles.erase(it);
      return true;
    }
  }

  // not found
  return false;
}

/// @brief iterates over a collection
bool MMFilesCollection::iterateDatafiles(std::function<bool(TRI_df_marker_t const*, MMFilesDatafile*)> const& cb) {
  if (!iterateDatafilesVector(_datafiles, cb) ||
      !iterateDatafilesVector(_compactors, cb) ||
      !iterateDatafilesVector(_journals, cb)) {
    return false;
  }
  return true;
}

/// @brief iterate over all datafiles in a vector
bool MMFilesCollection::iterateDatafilesVector(std::vector<MMFilesDatafile*> const& files,
                                               std::function<bool(TRI_df_marker_t const*, MMFilesDatafile*)> const& cb) {
  for (auto const& datafile : files) {
    datafile->sequentialAccess();
    datafile->willNeed();

    if (!TRI_IterateDatafile(datafile, cb)) {
      return false;
    }

    if (datafile->isPhysical() && datafile->isSealed()) {
      datafile->randomAccess();
    }
  }

  return true;
}

/// @brief closes the datafiles passed in the vector
bool MMFilesCollection::closeDatafiles(std::vector<MMFilesDatafile*> const& files) {
  bool result = true;

  for (auto const& datafile : files) {
    TRI_ASSERT(datafile != nullptr);
    if (datafile->state() == TRI_DF_STATE_CLOSED) {
      continue;
    }
    
    int res = datafile->close();
    
    if (res != TRI_ERROR_NO_ERROR) {
      result = false;
    }
  }
  
  return result;
}
  
void MMFilesCollection::figures(std::shared_ptr<arangodb::velocypack::Builder>& builder) {
    
    // fills in compaction status
    char const* lastCompactionStatus = "-";
    char lastCompactionStampString[21];
    lastCompactionStampString[0] = '-';
    lastCompactionStampString[1] = '\0';

    double lastCompactionStamp;

    {
      MUTEX_LOCKER(mutexLocker, _compactionStatusLock);
      lastCompactionStatus = _lastCompactionStatus;
      lastCompactionStamp = _lastCompactionStamp;
    }

    if (lastCompactionStatus != nullptr) {
      if (lastCompactionStamp == 0.0) {
        lastCompactionStamp = TRI_microtime();
      }
      struct tm tb;
      time_t tt = static_cast<time_t>(lastCompactionStamp);
      TRI_gmtime(tt, &tb);
      strftime(&lastCompactionStampString[0], sizeof(lastCompactionStampString),
               "%Y-%m-%dT%H:%M:%SZ", &tb);
    }

    builder->add("compactionStatus", VPackValue(VPackValueType::Object));
    builder->add("message", VPackValue(lastCompactionStatus));
    builder->add("time", VPackValue(&lastCompactionStampString[0]));
    builder->close();  // compactionStatus

  builder->add("documentReferences", VPackValue(_ditches.numDocumentDitches()));
  
  char const* waitingForDitch = _ditches.head();
  builder->add("waitingFor", VPackValue(waitingForDitch == nullptr ? "-" : waitingForDitch));
  
  // add datafile statistics
  DatafileStatisticsContainer dfi = _datafileStatistics.all();

  builder->add("alive", VPackValue(VPackValueType::Object));
  builder->add("count", VPackValue(dfi.numberAlive));
  builder->add("size", VPackValue(dfi.sizeAlive));
  builder->close(); // alive
  
  builder->add("dead", VPackValue(VPackValueType::Object));
  builder->add("count", VPackValue(dfi.numberDead));
  builder->add("size", VPackValue(dfi.sizeDead));
  builder->add("deletion", VPackValue(dfi.numberDeletions));
  builder->close(); // dead

  // add file statistics
  READ_LOCKER(readLocker, _filesLock); 
  
  size_t sizeDatafiles = 0;
  builder->add("datafiles", VPackValue(VPackValueType::Object));
  for (auto const& it : _datafiles) {
    sizeDatafiles += it->initSize();
  }

  builder->add("count", VPackValue(_datafiles.size()));
  builder->add("fileSize", VPackValue(sizeDatafiles));
  builder->close(); // datafiles
  
  size_t sizeJournals = 0;
  for (auto const& it : _journals) {
    sizeJournals += it->initSize();
  }
  builder->add("journals", VPackValue(VPackValueType::Object));
  builder->add("count", VPackValue(_journals.size()));
  builder->add("fileSize", VPackValue(sizeJournals));
  builder->close(); // journals
  
  size_t sizeCompactors = 0;
  for (auto const& it : _compactors) {
    sizeCompactors += it->initSize();
  }
  builder->add("compactors", VPackValue(VPackValueType::Object));
  builder->add("count", VPackValue(_compactors.size()));
  builder->add("fileSize", VPackValue(sizeCompactors));
  builder->close(); // compactors
  
  builder->add("revisions", VPackValue(VPackValueType::Object));
  builder->add("count", VPackValue(_revisionsCache.size()));
  builder->add("size", VPackValue(_revisionsCache.memoryUsage()));
  builder->close(); // revisions
}

/// @brief iterate over a vector of datafiles and pick those with a specific
/// data range
std::vector<MMFilesCollection::DatafileDescription> MMFilesCollection::datafilesInRange(TRI_voc_tick_t dataMin, TRI_voc_tick_t dataMax) {
  std::vector<DatafileDescription> result;

  auto apply = [&dataMin, &dataMax, &result](MMFilesDatafile const* datafile, bool isJournal) {
    DatafileDescription entry = {datafile, datafile->_dataMin, datafile->_dataMax, datafile->_tickMax, isJournal};
    LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "checking datafile " << datafile->fid() << " with data range " << datafile->_dataMin << " - " << datafile->_dataMax << ", tick max: " << datafile->_tickMax;

    if (datafile->_dataMin == 0 || datafile->_dataMax == 0) {
      // datafile doesn't have any data
      return;
    }

    TRI_ASSERT(datafile->_tickMin <= datafile->_tickMax);
    TRI_ASSERT(datafile->_dataMin <= datafile->_dataMax);

    if (dataMax < datafile->_dataMin) {
      // datafile is newer than requested range
      return;
    }

    if (dataMin > datafile->_dataMax) {
      // datafile is older than requested range
      return;
    }

    result.emplace_back(entry);
  };

  READ_LOCKER(readLocker, _filesLock); 

  for (auto& it : _datafiles) {
    apply(it, false);
  }
  for (auto& it : _journals) {
    apply(it, true);
  }

  return result;
}

bool MMFilesCollection::applyForTickRange(TRI_voc_tick_t dataMin, TRI_voc_tick_t dataMax,
                        std::function<bool(TRI_voc_tick_t foundTick, TRI_df_marker_t const* marker)> const& callback) {
  LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "getting datafiles in data range " << dataMin << " - " << dataMax;

  std::vector<DatafileDescription> datafiles = datafilesInRange(dataMin, dataMax);
  // now we have a list of datafiles...
 
  size_t const n = datafiles.size();

  for (size_t i = 0; i < n; ++i) {
    auto const& e = datafiles[i];
    MMFilesDatafile const* datafile = e._data;

    // we are reading from a journal that might be modified in parallel
    // so we must read-lock it
    CONDITIONAL_READ_LOCKER(readLocker, _filesLock, e._isJournal); 
    
    if (!e._isJournal) {
      TRI_ASSERT(datafile->isSealed());
    }
    
    char const* ptr = datafile->_data;
    char const* end = ptr + datafile->currentSize();

    while (ptr < end) {
      auto const* marker = reinterpret_cast<TRI_df_marker_t const*>(ptr);

      if (marker->getSize() == 0) {
        // end of datafile
        break;
      }
      
      TRI_df_marker_type_t type = marker->getType();
        
      if (type <= TRI_DF_MARKER_MIN) {
        break;
      }

      ptr += MMFilesDatafileHelper::AlignedMarkerSize<size_t>(marker);

      if (type == TRI_DF_MARKER_BLANK) {
        // fully ignore these marker types. they don't need to be replicated,
        // but we also cannot stop iteration if we find one of these
        continue;
      }

      // get the marker's tick and check whether we should include it
      TRI_voc_tick_t foundTick = marker->getTick();

      if (foundTick <= dataMin) {
        // marker too old
        continue; 
      }

      if (foundTick > dataMax) {
        // marker too new
        return false; // hasMore = false 
      }

      if (type != TRI_DF_MARKER_VPACK_DOCUMENT &&
          type != TRI_DF_MARKER_VPACK_REMOVE) {
        // found a non-data marker...

        // check if we can abort searching
        if (foundTick >= dataMax || (foundTick > e._tickMax && i == (n - 1))) {
          // fetched the last available marker
          return false; // hasMore = false
        }

        continue;
      }

      // note the last tick we processed
      bool doAbort = false;
      if (!callback(foundTick, marker)) {
        doAbort = true;
      } 
      
      if (foundTick >= dataMax || (foundTick >= e._tickMax && i == (n - 1))) {
        // fetched the last available marker
        return false; // hasMore = false
      }

      if (doAbort) {
        return true; // hasMore = true
      }
    } // next marker in datafile
  } // next datafile

  return false; // hasMore = false
}
  
/// @brief report extra memory used by indexes etc.
size_t MMFilesCollection::memory() const {
  return 0; // TODO
}

/// @brief disallow compaction of the collection 
void MMFilesCollection::preventCompaction() {
  _compactionLock.readLock();
}
  
/// @brief try disallowing compaction of the collection 
bool MMFilesCollection::tryPreventCompaction() {
  return _compactionLock.tryReadLock();
}

/// @brief re-allow compaction of the collection 
void MMFilesCollection::allowCompaction() {
  _compactionLock.unlock();
}
  
/// @brief exclusively lock the collection for compaction
void MMFilesCollection::lockForCompaction() {
  _compactionLock.writeLock();
}
  
/// @brief try to exclusively lock the collection for compaction
bool MMFilesCollection::tryLockForCompaction() {
  return _compactionLock.tryWriteLock();
}

/// @brief signal that compaction is finished
void MMFilesCollection::finishCompaction() {
  _compactionLock.unlock();
}

/// @brief iterate all markers of the collection
int MMFilesCollection::iterateMarkersOnLoad(transaction::Methods* trx) {
  // initialize state for iteration
  OpenIteratorState openState(_logicalCollection, trx);

  if (_initialCount != -1) {
    _revisionsCache.sizeHint(_initialCount);
    _logicalCollection->sizeHint(trx, _initialCount);
    openState._initialCount = _initialCount;
  }

  // read all documents and fill primary index
  auto cb = [&openState](TRI_df_marker_t const* marker, MMFilesDatafile* datafile) -> bool {
    return OpenIterator(marker, &openState, datafile); 
  };

  iterateDatafiles(cb);
    
  LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "found " << openState._documents << " document markers, " 
             << openState._deletions << " deletion markers for collection '" << _logicalCollection->name() << "'";
  
  if (_logicalCollection->version() <= LogicalCollection::VERSION_30 && 
      _lastRevision >= static_cast<TRI_voc_rid_t>(2016ULL - 1970ULL) * 1000ULL * 60ULL * 60ULL * 24ULL * 365ULL &&
      application_features::ApplicationServer::server->getFeature<DatabaseFeature>("Database")->check30Revisions()) {
    // a collection from 3.0 or earlier with a _rev value that is higher than we can handle safely
    _logicalCollection->setRevisionError();

    LOG_TOPIC(WARN, arangodb::Logger::FIXME) << "collection '" << _logicalCollection->name() << "' contains _rev values that are higher than expected for an ArangoDB 3.1 database. If this collection was created or used with a pre-release or development version of ArangoDB 3.1, please restart the server with option '--database.check-30-revisions false' to suppress this warning. If this collection was created with an ArangoDB 3.0, please dump the 3.0 database with arangodump and restore it in 3.1 with arangorestore.";
    if (application_features::ApplicationServer::server->getFeature<DatabaseFeature>("Database")->fail30Revisions()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_CORRUPTED_DATAFILE, std::string("collection '") + _logicalCollection->name() + "' contains _rev values from 3.0 and needs to be migrated using dump/restore");
    }
  }
  
  // update the real statistics for the collection
  try {
    for (auto& it : openState._stats) {
      createStats(it.first, *(it.second));
    }
  } catch (basics::Exception const& ex) {
    return ex.code();
  } catch (...) {
    return TRI_ERROR_INTERNAL;
  }

  return TRI_ERROR_NO_ERROR;
}

int MMFilesCollection::read(transaction::Methods* trx, VPackSlice const key,
                            ManagedDocumentResult& result, bool lock) {
  TRI_IF_FAILURE("ReadDocumentNoLock") {
    // test what happens if no lock can be acquired
    return TRI_ERROR_DEBUG;
  }

  TRI_IF_FAILURE("ReadDocumentNoLockExcept") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  bool const useDeadlockDetector =
      (lock && !trx->isSingleOperationTransaction());
  CollectionReadLocker collectionLocker(_logicalCollection, useDeadlockDetector,
                                        lock);

  int res = lookupDocument(trx, key, result);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  // we found a document
  return TRI_ERROR_NO_ERROR;
}


int MMFilesCollection::insert(transaction::Methods* trx,
                              VPackSlice const newSlice,
                              ManagedDocumentResult& result,
                              OperationOptions& options,
                              TRI_voc_tick_t& resultMarkerTick, bool lock) {
  // create marker
  MMFilesCrudMarker insertMarker(
      TRI_DF_MARKER_VPACK_DOCUMENT,
      static_cast<MMFilesTransactionState*>(trx->state())->idForMarker(), newSlice);

  MMFilesWalMarker const* marker;
  if (options.recoveryMarker == nullptr) {
    marker = &insertMarker;
  } else {
    marker = options.recoveryMarker;
  }

  // now insert into indexes
  TRI_IF_FAILURE("InsertDocumentNoLock") {
    // test what happens if no lock can be acquired
    return TRI_ERROR_DEBUG;
  }

  // TODO Do we need a LogicalCollection here?
  MMFilesDocumentOperation operation(_logicalCollection,
                                     TRI_VOC_DOCUMENT_OPERATION_INSERT);

  TRI_IF_FAILURE("InsertDocumentNoHeader") {
    // test what happens if no header can be acquired
    return TRI_ERROR_DEBUG;
  }

  TRI_IF_FAILURE("InsertDocumentNoHeaderExcept") {
    // test what happens if no header can be acquired
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  TRI_voc_rid_t revisionId = transaction::Methods::extractRevFromDocument(newSlice);
  VPackSlice doc(marker->vpack());
  operation.setRevisions(DocumentDescriptor(),
                         DocumentDescriptor(revisionId, doc.begin()));

  try {
    insertRevision(revisionId, marker->vpack(), 0, true, true);
    // and go on with the insertion...
  } catch (basics::Exception const& ex) {
    return ex.code();
  } catch (std::bad_alloc const&) {
    return TRI_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return TRI_ERROR_INTERNAL;
  }

  int res = TRI_ERROR_NO_ERROR;
  {
    // use lock?
    bool const useDeadlockDetector =
        (lock && !trx->isSingleOperationTransaction());
    try {
      // TODO Do we use the CollectionLocker on LogicalCollections
      // or do we use it on the SE specific one?
      arangodb::CollectionWriteLocker collectionLocker(
          _logicalCollection, useDeadlockDetector, lock);

      try {
        // insert into indexes
        res = insertDocument(trx, revisionId, doc, operation, marker,
                             options.waitForSync);
      } catch (basics::Exception const& ex) {
        res = ex.code();
      } catch (std::bad_alloc const&) {
        res = TRI_ERROR_OUT_OF_MEMORY;
      } catch (...) {
        res = TRI_ERROR_INTERNAL;
      }
    } catch (...) {
      // the collectionLocker may have thrown in its constructor...
      // if it did, then we need to manually remove the revision id
      // from the list of revisions
      try {
        removeRevision(revisionId, false);
      } catch (...) {
      }
      throw;
    }

    if (res != TRI_ERROR_NO_ERROR) {
      operation.revert(trx);
    }
  }

  if (res == TRI_ERROR_NO_ERROR) {
    uint8_t const* vpack = lookupRevisionVPack(revisionId);
    if (vpack != nullptr) {
      result.addExisting(vpack, revisionId);
    }

    // store the tick that was used for writing the document
    resultMarkerTick = operation.tick();
  }
  return res;
}

bool MMFilesCollection::isFullyCollected() const {
  int64_t uncollected = _uncollectedLogfileEntries.load();
  return (uncollected == 0);
}

MMFilesDocumentPosition MMFilesCollection::lookupRevision(TRI_voc_rid_t revisionId) const {
  TRI_ASSERT(revisionId != 0);
  MMFilesDocumentPosition const old = _revisionsCache.lookup(revisionId);
  if (old) {
    return old;
  }
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "got invalid revision value on lookup");
}

uint8_t const* MMFilesCollection::lookupRevisionVPack(TRI_voc_rid_t revisionId) const {
  TRI_ASSERT(revisionId != 0);

  MMFilesDocumentPosition const old = _revisionsCache.lookup(revisionId);
  if (old) {
    uint8_t const* vpack = static_cast<uint8_t const*>(old.dataptr());
    TRI_ASSERT(VPackSlice(vpack).isObject());
    return vpack;
  }
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "got invalid vpack value on lookup");
}
  
uint8_t const* MMFilesCollection::lookupRevisionVPackConditional(TRI_voc_rid_t revisionId, TRI_voc_tick_t maxTick, bool excludeWal) const {
  TRI_ASSERT(revisionId != 0);

  MMFilesDocumentPosition const old = _revisionsCache.lookup(revisionId);
  if (!old) {
    return nullptr;
  }
  if (excludeWal && old.pointsToWal()) {
    return nullptr;
  }

  uint8_t const* vpack = static_cast<uint8_t const*>(old.dataptr());

  if (maxTick > 0) {
    TRI_df_marker_t const* marker = reinterpret_cast<TRI_df_marker_t const*>(vpack - MMFilesDatafileHelper::VPackOffset(TRI_DF_MARKER_VPACK_DOCUMENT));
    if (marker->getTick() > maxTick) {
      return nullptr;
    }
  }

  return vpack; 
}

void MMFilesCollection::insertRevision(TRI_voc_rid_t revisionId, uint8_t const* dataptr, TRI_voc_fid_t fid, bool isInWal, bool shouldLock) {
  TRI_ASSERT(revisionId != 0);
  TRI_ASSERT(dataptr != nullptr);
  _revisionsCache.insert(revisionId, dataptr, fid, isInWal, shouldLock);
}

void MMFilesCollection::updateRevision(TRI_voc_rid_t revisionId, uint8_t const* dataptr, TRI_voc_fid_t fid, bool isInWal) {
  TRI_ASSERT(revisionId != 0);
  TRI_ASSERT(dataptr != nullptr);
  _revisionsCache.update(revisionId, dataptr, fid, isInWal);
}
  
bool MMFilesCollection::updateRevisionConditional(TRI_voc_rid_t revisionId, TRI_df_marker_t const* oldPosition, TRI_df_marker_t const* newPosition, TRI_voc_fid_t newFid, bool isInWal) {
  TRI_ASSERT(revisionId != 0);
  TRI_ASSERT(newPosition != nullptr);
  return _revisionsCache.updateConditional(revisionId, oldPosition, newPosition, newFid, isInWal);
}

void MMFilesCollection::removeRevision(TRI_voc_rid_t revisionId, bool updateStats) {
  TRI_ASSERT(revisionId != 0);
  if (updateStats) {
    MMFilesDocumentPosition const old = _revisionsCache.fetchAndRemove(revisionId);
    if (old && !old.pointsToWal() && old.fid() != 0) {
      TRI_ASSERT(old.dataptr() != nullptr);
      uint8_t const* vpack = static_cast<uint8_t const*>(old.dataptr());
      int64_t size = encoding::alignedSize<int64_t>(MMFilesDatafileHelper::VPackOffset(TRI_DF_MARKER_VPACK_DOCUMENT) + VPackSlice(vpack).byteSize());
      _datafileStatistics.increaseDead(old.fid(), 1, size);
    }
  } else {
    _revisionsCache.remove(revisionId);
  }
}

/// @brief creates a new entry in the primary index
int MMFilesCollection::insertPrimaryIndex(transaction::Methods* trx,
                                          TRI_voc_rid_t revisionId,
                                          VPackSlice const& doc) {
  TRI_IF_FAILURE("InsertPrimaryIndex") { return TRI_ERROR_DEBUG; }

  // insert into primary index
  return _logicalCollection->primaryIndex()->insertKey(trx, revisionId, doc);
}

/// @brief deletes an entry from the primary index
int MMFilesCollection::deletePrimaryIndex(arangodb::transaction::Methods* trx,
                                          TRI_voc_rid_t revisionId,
                                          VPackSlice const& doc) {
  TRI_IF_FAILURE("DeletePrimaryIndex") { return TRI_ERROR_DEBUG; }

  return _logicalCollection->primaryIndex()->removeKey(trx, revisionId, doc);
}

/// @brief creates a new entry in the secondary indexes
int MMFilesCollection::insertSecondaryIndexes(arangodb::transaction::Methods* trx,
                                              TRI_voc_rid_t revisionId,
                                              VPackSlice const& doc,
                                              bool isRollback) {
  // Coordintor doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());
  TRI_IF_FAILURE("InsertSecondaryIndexes") { return TRI_ERROR_DEBUG; }

  bool const useSecondary = _logicalCollection->useSecondaryIndexes();
  if (!useSecondary && _logicalCollection->_persistentIndexes == 0) {
    return TRI_ERROR_NO_ERROR;
  }

  int result = TRI_ERROR_NO_ERROR;

  // TODO FIXME
  auto indexes = _logicalCollection->getIndexes();
  size_t const n = indexes.size();

  for (size_t i = 1; i < n; ++i) {
    auto idx = indexes[i];
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
int MMFilesCollection::deleteSecondaryIndexes(arangodb::transaction::Methods* trx,
                                              TRI_voc_rid_t revisionId,
                                              VPackSlice const& doc,
                                              bool isRollback) {
  // Coordintor doesn't know index internals
  TRI_ASSERT(!ServerState::instance()->isCoordinator());

  bool const useSecondary = _logicalCollection->useSecondaryIndexes();
  if (!useSecondary && _logicalCollection->_persistentIndexes == 0) {
    return TRI_ERROR_NO_ERROR;
  }

  TRI_IF_FAILURE("DeleteSecondaryIndexes") { return TRI_ERROR_DEBUG; }

  int result = TRI_ERROR_NO_ERROR;

  // TODO FIXME
  auto indexes = _logicalCollection->getIndexes();
  size_t const n = indexes.size();

  for (size_t i = 1; i < n; ++i) {
    auto idx = indexes[i];
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

/// @brief insert a document into all indexes known to
///        this collection.
///        This function guarantees all or nothing,
///        If it returns NO_ERROR all indexes are filled.
///        If it returns an error no documents are inserted
int MMFilesCollection::insertIndexes(arangodb::transaction::Methods* trx,
                                     TRI_voc_rid_t revisionId,
                                     VPackSlice const& doc) {
  // insert into primary index first
  int res = insertPrimaryIndex(trx, revisionId, doc);

  if (res != TRI_ERROR_NO_ERROR) {
    // insert has failed
    return res;
  }

  // insert into secondary indexes
  res = insertSecondaryIndexes(trx, revisionId, doc, false);

  if (res != TRI_ERROR_NO_ERROR) {
    deleteSecondaryIndexes(trx, revisionId, doc, true);
    deletePrimaryIndex(trx, revisionId, doc);
  }
  return res;
}

/// @brief insert a document, low level worker
/// the caller must make sure the write lock on the collection is held
int MMFilesCollection::insertDocument(arangodb::transaction::Methods* trx,
                                      TRI_voc_rid_t revisionId,
                                      VPackSlice const& doc,
                                      MMFilesDocumentOperation& operation,
                                      MMFilesWalMarker const* marker,
                                      bool& waitForSync) {
  int res = insertIndexes(trx, revisionId, doc);
  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }
  operation.indexed();

  TRI_IF_FAILURE("InsertDocumentNoOperation") { return TRI_ERROR_DEBUG; }

  TRI_IF_FAILURE("InsertDocumentNoOperationExcept") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  return static_cast<MMFilesTransactionState*>(trx->state())->addOperation(revisionId, operation, marker, waitForSync);
}

int MMFilesCollection::update(arangodb::transaction::Methods* trx,
                              VPackSlice const newSlice,
                              ManagedDocumentResult& result,
                              OperationOptions& options,
                              TRI_voc_tick_t& resultMarkerTick, bool lock,
                              TRI_voc_rid_t& prevRev,
                              ManagedDocumentResult& previous,
                              TRI_voc_rid_t const& revisionId,
                              VPackSlice const key) {
  bool const isEdgeCollection =
      (_logicalCollection->type() == TRI_COL_TYPE_EDGE);
  TRI_IF_FAILURE("UpdateDocumentNoLock") { return TRI_ERROR_DEBUG; }

  bool const useDeadlockDetector =
      (lock && !trx->isSingleOperationTransaction());
  arangodb::CollectionWriteLocker collectionLocker(_logicalCollection,
                                                   useDeadlockDetector, lock);

  // get the previous revision
  int res = lookupDocument(trx, key, previous);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  uint8_t const* vpack = previous.vpack();
  VPackSlice oldDoc(vpack);
  TRI_voc_rid_t oldRevisionId =
      transaction::Methods::extractRevFromDocument(oldDoc);
  prevRev = oldRevisionId;

  TRI_IF_FAILURE("UpdateDocumentNoMarker") {
    // test what happens when no marker can be created
    return TRI_ERROR_DEBUG;
  }

  TRI_IF_FAILURE("UpdateDocumentNoMarkerExcept") {
    // test what happens when no marker can be created
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  // Check old revision:
  if (!options.ignoreRevs) {
    TRI_voc_rid_t expectedRev = 0;
    if (newSlice.isObject()) {
      expectedRev = TRI_ExtractRevisionId(newSlice);
    }
    int res = _logicalCollection->checkRevision(trx, expectedRev, prevRev);
    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
  }

  if (newSlice.length() <= 1) {
    // no need to do anything
    result = previous;
    return TRI_ERROR_NO_ERROR;
  }

  // merge old and new values
  transaction::BuilderLeaser builder(trx);
  if (options.recoveryMarker == nullptr) {
    mergeObjectsForUpdate(trx, oldDoc, newSlice, isEdgeCollection,
                          TRI_RidToString(revisionId), options.mergeObjects,
                          options.keepNull, *builder.get());

    if (ServerState::isDBServer(trx->serverRole())) {
      // Need to check that no sharding keys have changed:
      if (arangodb::shardKeysChanged(_logicalCollection->dbName(),
                                     trx->resolver()->getCollectionNameCluster(
                                         _logicalCollection->planId()),
                                     oldDoc, builder->slice(), false)) {
        return TRI_ERROR_CLUSTER_MUST_NOT_CHANGE_SHARDING_ATTRIBUTES;
      }
    }
  }

  // create marker
  MMFilesCrudMarker updateMarker(
      TRI_DF_MARKER_VPACK_DOCUMENT,
      static_cast<MMFilesTransactionState*>(trx->state())->idForMarker(), builder->slice());

  MMFilesWalMarker const* marker;
  if (options.recoveryMarker == nullptr) {
    marker = &updateMarker;
  } else {
    marker = options.recoveryMarker;
  }

  VPackSlice const newDoc(marker->vpack());

  MMFilesDocumentOperation operation(_logicalCollection,
                                     TRI_VOC_DOCUMENT_OPERATION_UPDATE);

  try {
    insertRevision(revisionId, marker->vpack(), 0, true, true);
    
    operation.setRevisions(DocumentDescriptor(oldRevisionId, oldDoc.begin()),
                           DocumentDescriptor(revisionId, newDoc.begin()));
    
    if (oldRevisionId == revisionId) {
      // update with same revision id => can happen if isRestore = true
      result.clear();
    }

    res = _logicalCollection->updateDocument(trx, oldRevisionId, oldDoc,
                                             revisionId, newDoc, operation,
                                             marker, options.waitForSync);
  } catch (basics::Exception const& ex) {
    res = ex.code();
  } catch (std::bad_alloc const&) {
    res = TRI_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    res = TRI_ERROR_INTERNAL;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    operation.revert(trx);
  } else {
    _logicalCollection->readRevision(trx, result, revisionId);

    if (options.waitForSync) {
      // store the tick that was used for writing the new document
      resultMarkerTick = operation.tick();
    }
  }

  return res;

}

int MMFilesCollection::replace(
    transaction::Methods* trx, VPackSlice const newSlice,
    ManagedDocumentResult& result, OperationOptions& options,
    TRI_voc_tick_t& resultMarkerTick, bool lock, TRI_voc_rid_t& prevRev,
    ManagedDocumentResult& previous, TRI_voc_rid_t const revisionId,
    VPackSlice const fromSlice, VPackSlice const toSlice) {
  bool const isEdgeCollection = (_logicalCollection->type() == TRI_COL_TYPE_EDGE);
  TRI_IF_FAILURE("ReplaceDocumentNoLock") { return TRI_ERROR_DEBUG; }

  // get the previous revision
  VPackSlice key = newSlice.get(StaticStrings::KeyString);
  if (key.isNone()) {
    return TRI_ERROR_ARANGO_DOCUMENT_HANDLE_BAD;
  }

  bool const useDeadlockDetector =
      (lock && !trx->isSingleOperationTransaction());
  arangodb::CollectionWriteLocker collectionLocker(_logicalCollection, useDeadlockDetector,
                                                   lock);

  // get the previous revision
  int res = lookupDocument(trx, key, previous);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  TRI_IF_FAILURE("ReplaceDocumentNoMarker") {
    // test what happens when no marker can be created
    return TRI_ERROR_DEBUG;
  }

  TRI_IF_FAILURE("ReplaceDocumentNoMarkerExcept") {
    // test what happens when no marker can be created
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  uint8_t const* vpack = previous.vpack();
  VPackSlice oldDoc(vpack);
  TRI_voc_rid_t oldRevisionId = transaction::Methods::extractRevFromDocument(oldDoc);
  prevRev = oldRevisionId;

  // Check old revision:
  if (!options.ignoreRevs) {
    TRI_voc_rid_t expectedRev = 0;
    if (newSlice.isObject()) {
      expectedRev = TRI_ExtractRevisionId(newSlice);
    }
    int res = _logicalCollection->checkRevision(trx, expectedRev, prevRev);
    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
  }

  // merge old and new values
  transaction::BuilderLeaser builder(trx);
  newObjectForReplace(trx, oldDoc, newSlice, fromSlice, toSlice,
                      isEdgeCollection, TRI_RidToString(revisionId),
                      *builder.get());

  if (ServerState::isDBServer(trx->serverRole())) {
    // Need to check that no sharding keys have changed:
    if (arangodb::shardKeysChanged(_logicalCollection->dbName(),
                                   trx->resolver()->getCollectionNameCluster(
                                       _logicalCollection->planId()),
                                   oldDoc, builder->slice(), false)) {
      return TRI_ERROR_CLUSTER_MUST_NOT_CHANGE_SHARDING_ATTRIBUTES;
    }
  }

  // create marker
  MMFilesCrudMarker replaceMarker(
      TRI_DF_MARKER_VPACK_DOCUMENT,
      static_cast<MMFilesTransactionState*>(trx->state())->idForMarker(), builder->slice());

  MMFilesWalMarker const* marker;
  if (options.recoveryMarker == nullptr) {
    marker = &replaceMarker;
  } else {
    marker = options.recoveryMarker;
  }

  VPackSlice const newDoc(marker->vpack());

  MMFilesDocumentOperation operation(_logicalCollection, TRI_VOC_DOCUMENT_OPERATION_REPLACE);

  try {
    insertRevision(revisionId, marker->vpack(), 0, true, true);
    
    operation.setRevisions(DocumentDescriptor(oldRevisionId, oldDoc.begin()),
                           DocumentDescriptor(revisionId, newDoc.begin()));
    
    if (oldRevisionId == revisionId) {
      // update with same revision id => can happen if isRestore = true
      result.clear();
    }

    res = _logicalCollection->updateDocument(trx, oldRevisionId, oldDoc,
                                             revisionId, newDoc, operation,
                                             marker, options.waitForSync);
  } catch (basics::Exception const& ex) {
    res = ex.code();
  } catch (std::bad_alloc const&) {
    res = TRI_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    res = TRI_ERROR_INTERNAL;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    operation.revert(trx);
  } else {
    if (oldRevisionId == revisionId) {
      // update with same revision id => can happen if isRestore = true
      result.clear();
    }
    _logicalCollection->readRevision(trx, result, revisionId);

    if (options.waitForSync) {
      // store the tick that was used for writing the new document
      resultMarkerTick = operation.tick();
    }
  }

  return res;
}

int MMFilesCollection::remove(arangodb::transaction::Methods* trx, VPackSlice const slice,
                              ManagedDocumentResult& previous,
                              OperationOptions& options,
                              TRI_voc_tick_t& resultMarkerTick, bool lock,
                              TRI_voc_rid_t const& revisionId,
                              TRI_voc_rid_t& prevRev,
                              VPackSlice const toRemove) {
  prevRev = 0;

  TRI_IF_FAILURE("RemoveDocumentNoMarker") {
    // test what happens when no marker can be created
    return TRI_ERROR_DEBUG;
  }

  TRI_IF_FAILURE("RemoveDocumentNoMarkerExcept") {
    // test what happens if no marker can be created
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  // create marker
  MMFilesCrudMarker removeMarker(
      TRI_DF_MARKER_VPACK_REMOVE,
      static_cast<MMFilesTransactionState*>(trx->state())->idForMarker(),
      toRemove);

  MMFilesWalMarker const* marker;
  if (options.recoveryMarker == nullptr) {
    marker = &removeMarker;
  } else {
    marker = options.recoveryMarker;
  }

  TRI_IF_FAILURE("RemoveDocumentNoLock") {
    // test what happens if no lock can be acquired
    return TRI_ERROR_DEBUG;
  }

  VPackSlice key;
  if (slice.isString()) {
    key = slice;
  } else {
    key = slice.get(StaticStrings::KeyString);
  }
  TRI_ASSERT(!key.isNone());

  MMFilesDocumentOperation operation(_logicalCollection,
                                     TRI_VOC_DOCUMENT_OPERATION_REMOVE);

  bool const useDeadlockDetector =
      (lock && !trx->isSingleOperationTransaction());
  arangodb::CollectionWriteLocker collectionLocker(_logicalCollection,
                                                   useDeadlockDetector, lock);

  // get the previous revision
  int res = lookupDocument(trx, key, previous);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  uint8_t const* vpack = previous.vpack();
  VPackSlice oldDoc(vpack);
  TRI_voc_rid_t oldRevisionId = arangodb::transaction::Methods::extractRevFromDocument(oldDoc);
  prevRev = oldRevisionId;

  // Check old revision:
  if (!options.ignoreRevs && slice.isObject()) {
    TRI_voc_rid_t expectedRevisionId = TRI_ExtractRevisionId(slice);
    int res = _logicalCollection->checkRevision(trx, expectedRevisionId,
                                                oldRevisionId);

    if (res != TRI_ERROR_NO_ERROR) {
      return res;
    }
  }

  // we found a document to remove
  try {
    operation.setRevisions(DocumentDescriptor(oldRevisionId, oldDoc.begin()),
                           DocumentDescriptor());

    // delete from indexes
    res = deleteSecondaryIndexes(trx, oldRevisionId, oldDoc, false);

    if (res != TRI_ERROR_NO_ERROR) {
      insertSecondaryIndexes(trx, oldRevisionId, oldDoc, true);
      THROW_ARANGO_EXCEPTION(res);
    }

    res = deletePrimaryIndex(trx, oldRevisionId, oldDoc);

    if (res != TRI_ERROR_NO_ERROR) {
      insertSecondaryIndexes(trx, oldRevisionId, oldDoc, true);
      THROW_ARANGO_EXCEPTION(res);
    }

    operation.indexed();

    TRI_IF_FAILURE("RemoveDocumentNoOperation") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }
    
    try {
      removeRevision(oldRevisionId, true);
    } catch (...) {
    }

    TRI_IF_FAILURE("RemoveDocumentNoOperationExcept") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }

    res =
        static_cast<MMFilesTransactionState*>(trx->state())
            ->addOperation(revisionId, operation, marker, options.waitForSync);
  } catch (basics::Exception const& ex) {
    res = ex.code();
  } catch (std::bad_alloc const&) {
    res = TRI_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    res = TRI_ERROR_INTERNAL;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    operation.revert(trx);
  } else {
    // store the tick that was used for removing the document
    resultMarkerTick = operation.tick();
  }
  return res;
}

/// @brief removes a document or edge, fast path function for database documents
int MMFilesCollection::removeFastPath(arangodb::transaction::Methods* trx,
                                      TRI_voc_rid_t oldRevisionId,
                                      VPackSlice const oldDoc,
                                      OperationOptions& options,
                                      TRI_voc_tick_t& resultMarkerTick,
                                      bool lock,
                                      TRI_voc_rid_t const& revisionId,
                                      VPackSlice const toRemove) {
  TRI_IF_FAILURE("RemoveDocumentNoMarker") {
    // test what happens when no marker can be created
    return TRI_ERROR_DEBUG;
  }

  TRI_IF_FAILURE("RemoveDocumentNoMarkerExcept") {
    // test what happens if no marker can be created
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  // create marker
  MMFilesCrudMarker removeMarker(
      TRI_DF_MARKER_VPACK_REMOVE,
      static_cast<MMFilesTransactionState*>(trx->state())->idForMarker(),
      toRemove);

  MMFilesWalMarker const* marker = &removeMarker;

  TRI_IF_FAILURE("RemoveDocumentNoLock") {
    // test what happens if no lock can be acquired
    return TRI_ERROR_DEBUG;
  }

  VPackSlice key = arangodb::transaction::Methods::extractKeyFromDocument(oldDoc);
  TRI_ASSERT(!key.isNone());

  MMFilesDocumentOperation operation(_logicalCollection,
                                     TRI_VOC_DOCUMENT_OPERATION_REMOVE);

  bool const useDeadlockDetector =
      (lock && !trx->isSingleOperationTransaction());
  arangodb::CollectionWriteLocker collectionLocker(_logicalCollection,
                                                   useDeadlockDetector, lock);

  operation.setRevisions(DocumentDescriptor(oldRevisionId, oldDoc.begin()),
                         DocumentDescriptor());

  // delete from indexes
  int res;
  try {
    res = deleteSecondaryIndexes(trx, oldRevisionId, oldDoc, false);

    if (res != TRI_ERROR_NO_ERROR) {
      insertSecondaryIndexes(trx, oldRevisionId, oldDoc, true);
      THROW_ARANGO_EXCEPTION(res);
    }

    res = deletePrimaryIndex(trx, oldRevisionId, oldDoc);

    if (res != TRI_ERROR_NO_ERROR) {
      insertSecondaryIndexes(trx, oldRevisionId, oldDoc, true);
      THROW_ARANGO_EXCEPTION(res);
    }

    operation.indexed();
  
    try {
      removeRevision(oldRevisionId, true);
    } catch (...) {
    }

    TRI_IF_FAILURE("RemoveDocumentNoOperation") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }

    TRI_IF_FAILURE("RemoveDocumentNoOperationExcept") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }

    res =
        static_cast<MMFilesTransactionState*>(trx->state())
            ->addOperation(revisionId, operation, marker, options.waitForSync);
  } catch (basics::Exception const& ex) {
    res = ex.code();
  } catch (std::bad_alloc const&) {
    res = TRI_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    res = TRI_ERROR_INTERNAL;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    operation.revert(trx);
  } else {
    // store the tick that was used for removing the document
    resultMarkerTick = operation.tick();
  }

  return res;
}

/// @brief looks up a document by key, low level worker
/// the caller must make sure the read lock on the collection is held
/// the key must be a string slice, no revision check is performed
int MMFilesCollection::lookupDocument(transaction::Methods* trx,
                                      VPackSlice const key,
                                      ManagedDocumentResult& result) {
  if (!key.isString()) {
    return TRI_ERROR_ARANGO_DOCUMENT_KEY_BAD;
  }

  MMFilesSimpleIndexElement element =
      _logicalCollection->primaryIndex()->lookupKey(trx, key, result);
  if (element) {
    _logicalCollection->readRevision(trx, result, element.revisionId());
    return TRI_ERROR_NO_ERROR;
  }

  return TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND;
}


