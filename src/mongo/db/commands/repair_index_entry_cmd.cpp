/**
 *    Copyright (C) 2025 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/shard_key_lock.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::stringstream;

namespace {

// Error codes for this command
const ErrorCodes::Error kAmbiguousMatch = ErrorCodes::Error(50000);
const ErrorCodes::Error kIndexEntryAlreadyExists = ErrorCodes::Error(50001);
const ErrorCodes::Error kIndexEntryNotFound = ErrorCodes::Error(50002);
const ErrorCodes::Error kDocumentStillExists = ErrorCodes::Error(50003);

/**
 * repairIndexEntry command - repair individual index entries for documents.
 *
 * Usage:
 *   db.runCommand({
 *       repairIndexEntry: "<collection>",
 *       action: "insert" | "remove",
 *       indexName: "<index_name>",
 *       _id: <value>,                    // optional
 *       shardKey: {...},                 // optional
 *       indexKey: {...},                 // optional
 *       recordId: <int64>,               // optional
 *       dryRun: <bool>                   // optional
 *   })
 */
class CmdRepairIndexEntry : public Command {
public:
    CmdRepairIndexEntry() : Command("repairIndexEntry") {}

    virtual bool slaveOk() const override {
        return false;  // Write operation, must be on primary
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual void help(stringstream& h) const override {
        h << "Repair individual index entries for documents.\n"
          << "Use 'insert' action to add missing index entries.\n"
          << "Use 'remove' action to delete orphan index entries.";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) override {
        ActionSet actions;
        actions.addAction(ActionType::repairIndexEntry);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     string& errmsg,
                     BSONObjBuilder& result) override {
        // Parse collection name
        const string collName = cmdObj.firstElement().valuestrsafe();
        if (collName.empty()) {
            errmsg = "collection name is required";
            return false;
        }
        const NamespaceString nss(dbname, collName);

        // Parse action
        const string action = cmdObj.getStringField("action");
        if (action != "insert" && action != "remove") {
            errmsg = "action must be 'insert' or 'remove'";
            return false;
        }
        const bool isInsert = (action == "insert");

        // Parse indexName
        const string indexName = cmdObj.getStringField("indexName");
        if (indexName.empty()) {
            errmsg = "indexName is required";
            return false;
        }

        // Parse location parameters
        BSONElement idElem = cmdObj.getField("_id");
        BSONObj shardKey = cmdObj.getObjectField("shardKey");
        BSONObj indexKey = cmdObj.getObjectField("indexKey");
        long long recordIdVal = cmdObj.getField("recordId").safeNumberLong();
        const bool dryRun = cmdObj.getBoolField("dryRun");

        const bool hasId = !idElem.eoo();
        const bool hasShardKey = !shardKey.isEmpty();
        const bool hasIndexKey = !indexKey.isEmpty();
        const bool hasRecordId = cmdObj.hasField("recordId");

        // Validate: must have at least one location parameter
        if (!hasId && !hasIndexKey) {
            errmsg = "must specify _id or indexKey";
            return false;
        }

        // For remove with indexKey, recordId is required
        if (!isInsert && hasIndexKey && !hasId && !hasRecordId) {
            errmsg = "recordId is required for remove with indexKey";
            return false;
        }

        // Check if primary
        auto replCoord = repl::getGlobalReplicationCoordinator();
        if (!replCoord->canAcceptWritesFor(nss)) {
            errmsg = "not primary";
            return false;
        }

        // Acquire locks: DB IX + Collection IX
        ScopedTransaction transaction(txn, MODE_IX);
        AutoGetDb autoDb(txn, nss.db(), MODE_IX);
        Lock::CollectionLock collLock(txn->lockState(), nss.ns(), MODE_IX);

        Database* db = autoDb.getDb();
        if (!db) {
            errmsg = str::stream() << "database not found: " << nss.db();
            return false;
        }

        Collection* collection = db->getCollection(nss);
        if (!collection) {
            errmsg = str::stream() << "collection not found: " << nss.ns();
            return false;
        }

        // Get index catalog and find the target index
        IndexCatalog* indexCatalog = collection->getIndexCatalog();
        IndexDescriptor* descriptor = indexCatalog->findIndexByName(txn, indexName);
        if (!descriptor) {
            errmsg = str::stream() << "index not found: " << indexName;
            return false;
        }

        IndexAccessMethod* accessMethod = indexCatalog->getIndex(descriptor);
        if (!accessMethod) {
            errmsg = str::stream() << "index access method not found: " << indexName;
            return false;
        }

        // Acquire ShardKey lock if provided
        std::unique_ptr<ShardKeyLock> shardKeyLock;
        if (hasShardKey) {
            shardKeyLock = ShardKeyLock::acquire(txn, nss, shardKey);
        }

        // Variables for document and recordId
        RecordId recordId;
        BSONObj document;
        bool docFound = false;

        // Locate document/record
        if (hasId) {
            // Find document by _id
            IndexDescriptor* idIndex = indexCatalog->findIdIndex(txn);
            if (!idIndex) {
                errmsg = "_id index not found";
                return false;
            }

            IndexAccessMethod* idAccessMethod = indexCatalog->getIndex(idIndex);
            BSONObj idKey = BSON("" << idElem);
            recordId = idAccessMethod->findSingle(txn, idKey);

            if (!recordId.isNormal()) {
                if (isInsert) {
                    errmsg = str::stream() << "document not found with _id: " << idElem;
                    return false;
                }
                // For remove, document not existing is expected (orphan index)
            } else {
                // Get the document
                Snapshotted<BSONObj> snapDoc;
                if (collection->findDoc(txn, recordId, &snapDoc)) {
                    document = snapDoc.value().getOwned();
                    docFound = true;
                }
            }
        } else if (hasRecordId) {
            // Use provided recordId directly
            recordId = RecordId(recordIdVal);

            // Try to find document
            Snapshotted<BSONObj> snapDoc;
            if (collection->findDoc(txn, recordId, &snapDoc)) {
                document = snapDoc.value().getOwned();
                docFound = true;
            }
        }

        // Perform action-specific validation and operation
        if (isInsert) {
            // INSERT: Add missing index entry
            return doInsert(txn, collection, accessMethod, descriptor,
                           document, docFound, recordId, indexKey,
                           hasIndexKey, dryRun, errmsg, result);
        } else {
            // REMOVE: Delete orphan index entry
            return doRemove(txn, collection, accessMethod, descriptor,
                           document, docFound, recordId, indexKey,
                           hasIndexKey, dryRun, errmsg, result);
        }
    }

private:
    bool doInsert(OperationContext* txn,
                  Collection* collection,
                  IndexAccessMethod* accessMethod,
                  IndexDescriptor* descriptor,
                  const BSONObj& document,
                  bool docFound,
                  const RecordId& recordId,
                  const BSONObj& indexKey,
                  bool hasIndexKey,
                  bool dryRun,
                  string& errmsg,
                  BSONObjBuilder& result) {
        // Validation: document must exist for insert
        if (!docFound) {
            errmsg = "document does not exist, cannot insert index entry";
            return false;
        }

        // Generate index keys from document
        BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
        MultikeyPaths multikeyPaths;
        accessMethod->getKeys(document, IndexAccessMethod::GetKeysMode::kEnforceConstraints,
                              &keys, &multikeyPaths);

        if (keys.empty()) {
            errmsg = "document generates no index keys";
            return false;
        }

        // Check for ambiguous match if document generates multiple keys and no indexKey specified
        if (keys.size() > 1 && !hasIndexKey) {
            errmsg = "document generates multiple index keys, please specify indexKey";
            result.append("code", kAmbiguousMatch);
            return false;
        }

        // Determine which key to insert
        BSONObj keyToInsert;
        if (hasIndexKey) {
            // Verify provided indexKey is one of the generated keys
            bool found = false;
            for (const auto& k : keys) {
                if (k.woCompare(indexKey) == 0) {
                    keyToInsert = k;
                    found = true;
                    break;
                }
            }
            if (!found) {
                errmsg = "provided indexKey does not match any key generated from document";
                return false;
            }
        } else {
            keyToInsert = *keys.begin();
        }

        // Check if index entry already exists
        auto cursor = accessMethod->newCursor(txn);
        cursor->setEndPosition(keyToInsert, true);
        auto entry = cursor->seek(keyToInsert, true);
        while (entry) {
            if (entry->loc == recordId) {
                errmsg = "index entry already exists, no repair needed";
                result.append("code", kIndexEntryAlreadyExists);
                return false;
            }
            entry = cursor->next();
            if (entry && entry->key.woCompare(keyToInsert) != 0) {
                break;
            }
        }

        if (dryRun) {
            result.append("dryRun", true);
            result.append("wouldInsert", keyToInsert);
            result.append("recordId", recordId.repr());
            return true;
        }

        // Perform the insert
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            WriteUnitOfWork wuow(txn);

            InsertDeleteOptions options;
            options.dupsAllowed = !descriptor->unique();

            int64_t numInserted = 0;
            Status status = accessMethod->insert(txn, document, recordId, options, &numInserted);
            if (!status.isOK()) {
                errmsg = str::stream() << "failed to insert index entry: " << status.reason();
                return false;
            }

            wuow.commit();

            result.append("keysInserted", numInserted);
            LOG(1) << "repairIndexEntry: inserted " << numInserted << " keys for "
                   << collection->ns() << " index " << descriptor->indexName();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "repairIndexEntry", collection->ns().ns());

        return true;
    }

    bool doRemove(OperationContext* txn,
                  Collection* collection,
                  IndexAccessMethod* accessMethod,
                  IndexDescriptor* descriptor,
                  const BSONObj& document,
                  bool docFound,
                  const RecordId& recordId,
                  const BSONObj& indexKey,
                  bool hasIndexKey,
                  bool dryRun,
                  string& errmsg,
                  BSONObjBuilder& result) {
        // If we have indexKey but document still exists, that's an error
        if (hasIndexKey && docFound) {
            errmsg = "document still exists, cannot remove as orphan index";
            result.append("code", kDocumentStillExists);
            return false;
        }

        BSONObj keyToRemove;
        RecordId locToRemove = recordId;

        if (hasIndexKey) {
            // Use provided indexKey directly
            keyToRemove = indexKey;

            // Verify the index entry exists
            auto cursor = accessMethod->newCursor(txn);
            cursor->setEndPosition(indexKey, true);
            auto entry = cursor->seek(indexKey, true);

            bool found = false;
            int matchCount = 0;
            RecordId firstMatch;

            while (entry && entry->key.woCompare(indexKey) == 0) {
                matchCount++;
                if (matchCount == 1) {
                    firstMatch = entry->loc;
                }
                if (entry->loc == recordId) {
                    found = true;
                    break;
                }
                entry = cursor->next();
            }

            if (!found && recordId.isNormal()) {
                errmsg = "index entry not found at specified recordId";
                result.append("code", kIndexEntryNotFound);
                return false;
            }

            // If recordId not provided but multiple matches, error
            if (!recordId.isNormal() && matchCount > 1) {
                errmsg = "multiple index entries match, please provide recordId";
                result.append("code", kAmbiguousMatch);
                result.append("matchCount", matchCount);
                return false;
            }

            if (!recordId.isNormal() && matchCount == 1) {
                locToRemove = firstMatch;
            }

            if (matchCount == 0) {
                errmsg = "index entry not found";
                result.append("code", kIndexEntryNotFound);
                return false;
            }
        } else if (docFound) {
            // Document exists, generate keys and remove
            BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
            MultikeyPaths multikeyPaths;
            accessMethod->getKeys(document, IndexAccessMethod::GetKeysMode::kEnforceConstraints,
                                  &keys, &multikeyPaths);

            if (keys.empty()) {
                errmsg = "document generates no index keys";
                return false;
            }

            if (keys.size() > 1) {
                errmsg = "document generates multiple index keys, please specify indexKey";
                result.append("code", kAmbiguousMatch);
                return false;
            }

            keyToRemove = *keys.begin();
        } else {
            errmsg = "cannot determine index key to remove";
            return false;
        }

        if (dryRun) {
            result.append("dryRun", true);
            result.append("wouldRemove", keyToRemove);
            result.append("recordId", locToRemove.repr());
            return true;
        }

        // Perform the remove
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            WriteUnitOfWork wuow(txn);

            bool dupsAllowed = !descriptor->unique();
            accessMethod->removeSingleKey(txn, keyToRemove, locToRemove, dupsAllowed);

            wuow.commit();

            result.append("keysRemoved", 1);
            LOG(1) << "repairIndexEntry: removed key for "
                   << collection->ns() << " index " << descriptor->indexName();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "repairIndexEntry", collection->ns().ns());

        return true;
    }

} cmdRepairIndexEntry;

}  // namespace

}  // namespace mongo
