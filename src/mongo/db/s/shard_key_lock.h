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

#pragma once

#include <map>
#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

/**
 * Comparator for BSONObj to use in std::map.
 */
struct BSONObjLessThan {
    bool operator()(const BSONObj& lhs, const BSONObj& rhs) const {
        return SimpleBSONObjComparator::kInstance.compare(lhs, rhs) < 0;
    }
};

class OperationContext;

/**
 * RAII-style lock for a specific shard key value within a collection.
 *
 * This lock provides fine-grained concurrency control at the shard key level,
 * allowing concurrent operations on different shard key values while serializing
 * operations on the same shard key value.
 *
 * Usage:
 *   auto lock = ShardKeyLock::acquire(txn, nss, shardKeyValue);
 *   // ... perform operations while holding the lock ...
 *   // lock is automatically released when it goes out of scope
 */
class ShardKeyLock {
    MONGO_DISALLOW_COPYING(ShardKeyLock);

public:
    /**
     * Acquires a lock for the specified shard key value.
     *
     * @param txn The operation context
     * @param nss The namespace of the collection
     * @param shardKeyValue The shard key value to lock
     * @return A unique_ptr to the acquired lock, or nullptr if shardKeyValue is empty
     */
    static std::unique_ptr<ShardKeyLock> acquire(OperationContext* txn,
                                                  const NamespaceString& nss,
                                                  const BSONObj& shardKeyValue);

    /**
     * Releases the lock when destroyed (RAII).
     */
    ~ShardKeyLock();

    /**
     * Move constructor.
     */
    ShardKeyLock(ShardKeyLock&& other) noexcept;

    /**
     * Move assignment operator.
     */
    ShardKeyLock& operator=(ShardKeyLock&& other) noexcept;

    /**
     * Returns the namespace this lock is associated with.
     */
    const NamespaceString& nss() const {
        return _nss;
    }

    /**
     * Returns the shard key value this lock is protecting.
     */
    const BSONObj& shardKeyValue() const {
        return _shardKeyValue;
    }

private:
    /**
     * Private constructor - use acquire() to create instances.
     */
    ShardKeyLock(const NamespaceString& nss,
                 const BSONObj& shardKeyValue,
                 stdx::unique_lock<stdx::mutex> lock);

    /**
     * Internal structure to hold per-shard-key mutex.
     */
    struct LockEntry {
        stdx::mutex mutex;
        uint32_t refCount = 0;
    };

    /**
     * Gets or creates a LockEntry for the given namespace and shard key.
     */
    static std::shared_ptr<LockEntry> getOrCreateLockEntry(const NamespaceString& nss,
                                                            const BSONObj& shardKeyValue);

    /**
     * Releases the LockEntry reference.
     */
    static void releaseLockEntry(const NamespaceString& nss, const BSONObj& shardKeyValue);

    // Global mutex protecting the lock map
    static stdx::mutex _globalMutex;

    // Map: collection namespace -> (shard key value -> lock entry)
    static std::map<std::string, std::map<BSONObj, std::shared_ptr<LockEntry>, BSONObjLessThan>> _lockMap;

    // The namespace this lock is associated with
    NamespaceString _nss;

    // The shard key value this lock is protecting
    BSONObj _shardKeyValue;

    // The held lock (empty after move)
    stdx::unique_lock<stdx::mutex> _lock;

    // Whether this lock is valid (not moved-from)
    bool _valid = true;
};

}  // namespace mongo
