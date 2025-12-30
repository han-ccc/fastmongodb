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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_key_lock.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"

namespace mongo {

// Static member initialization
stdx::mutex ShardKeyLock::_globalMutex;
std::map<std::string, std::map<BSONObj, std::shared_ptr<ShardKeyLock::LockEntry>, BSONObjLessThan>>
    ShardKeyLock::_lockMap;

std::unique_ptr<ShardKeyLock> ShardKeyLock::acquire(OperationContext* txn,
                                                     const NamespaceString& nss,
                                                     const BSONObj& shardKeyValue) {
    // If no shard key value provided, return nullptr (no lock needed)
    if (shardKeyValue.isEmpty()) {
        return nullptr;
    }

    // Get or create the lock entry for this shard key
    auto lockEntry = getOrCreateLockEntry(nss, shardKeyValue);

    // Acquire the per-shard-key mutex
    stdx::unique_lock<stdx::mutex> lock(lockEntry->mutex);

    LOG(2) << "ShardKeyLock acquired for " << nss.ns() << " shardKey: " << shardKeyValue;

    return std::unique_ptr<ShardKeyLock>(
        new ShardKeyLock(nss, shardKeyValue, std::move(lock)));
}

ShardKeyLock::ShardKeyLock(const NamespaceString& nss,
                           const BSONObj& shardKeyValue,
                           stdx::unique_lock<stdx::mutex> lock)
    : _nss(nss), _shardKeyValue(shardKeyValue.getOwned()), _lock(std::move(lock)), _valid(true) {}

ShardKeyLock::~ShardKeyLock() {
    if (_valid && _lock.owns_lock()) {
        _lock.unlock();
        releaseLockEntry(_nss, _shardKeyValue);
        LOG(2) << "ShardKeyLock released for " << _nss.ns() << " shardKey: " << _shardKeyValue;
    }
}

ShardKeyLock::ShardKeyLock(ShardKeyLock&& other) noexcept
    : _nss(std::move(other._nss)),
      _shardKeyValue(std::move(other._shardKeyValue)),
      _lock(std::move(other._lock)),
      _valid(other._valid) {
    other._valid = false;
}

ShardKeyLock& ShardKeyLock::operator=(ShardKeyLock&& other) noexcept {
    if (this != &other) {
        // Release current lock if held
        if (_valid && _lock.owns_lock()) {
            _lock.unlock();
            releaseLockEntry(_nss, _shardKeyValue);
        }

        _nss = std::move(other._nss);
        _shardKeyValue = std::move(other._shardKeyValue);
        _lock = std::move(other._lock);
        _valid = other._valid;
        other._valid = false;
    }
    return *this;
}

std::shared_ptr<ShardKeyLock::LockEntry> ShardKeyLock::getOrCreateLockEntry(
    const NamespaceString& nss, const BSONObj& shardKeyValue) {
    stdx::lock_guard<stdx::mutex> globalLock(_globalMutex);

    auto& collectionMap = _lockMap[nss.ns()];
    auto it = collectionMap.find(shardKeyValue);

    if (it == collectionMap.end()) {
        // Create new lock entry
        auto entry = std::make_shared<LockEntry>();
        entry->refCount = 1;
        collectionMap[shardKeyValue.getOwned()] = entry;
        return entry;
    } else {
        // Increment reference count
        it->second->refCount++;
        return it->second;
    }
}

void ShardKeyLock::releaseLockEntry(const NamespaceString& nss, const BSONObj& shardKeyValue) {
    stdx::lock_guard<stdx::mutex> globalLock(_globalMutex);

    auto collIt = _lockMap.find(nss.ns());
    if (collIt == _lockMap.end()) {
        return;
    }

    auto& collectionMap = collIt->second;
    auto entryIt = collectionMap.find(shardKeyValue);
    if (entryIt == collectionMap.end()) {
        return;
    }

    // Decrement reference count
    entryIt->second->refCount--;

    // Clean up if no more references
    if (entryIt->second->refCount == 0) {
        collectionMap.erase(entryIt);

        // Clean up collection map if empty
        if (collectionMap.empty()) {
            _lockMap.erase(collIt);
        }
    }
}

}  // namespace mongo
