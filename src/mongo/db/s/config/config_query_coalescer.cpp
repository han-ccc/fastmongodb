/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    Config Server Query Coalescer Implementation
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/config_query_coalescer.h"

#include <algorithm>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameters.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

// Server parameters for runtime configuration
std::atomic<bool> configQueryCoalescerEnabled{false};
std::atomic<int> configQueryCoalescerWindowMS{5};
std::atomic<int> configQueryCoalescerMaxWaitMS{100};
std::atomic<int> configQueryCoalescerMaxWaiters{1000};
std::atomic<long long> configQueryCoalescerMaxVersionGap{500};  // 版本差距阈值

MONGO_EXPORT_SERVER_PARAMETER(configQueryCoalescerEnabledParam, bool, false);
MONGO_EXPORT_SERVER_PARAMETER(configQueryCoalescerWindowMSParam, int, 5);
MONGO_EXPORT_SERVER_PARAMETER(configQueryCoalescerMaxWaitMSParam, int, 100);
MONGO_EXPORT_SERVER_PARAMETER(configQueryCoalescerMaxWaitersParam, int, 1000);
MONGO_EXPORT_SERVER_PARAMETER(configQueryCoalescerMaxVersionGapParam, long long, 500);

// ============================================================================
// Stats Implementation
// ============================================================================

BSONObj ConfigQueryCoalescer::Stats::toBSON() const {
    BSONObjBuilder builder;
    builder.append("totalRequests", static_cast<long long>(totalRequests));
    builder.append("actualQueries", static_cast<long long>(actualQueries));
    builder.append("coalescedRequests", static_cast<long long>(coalescedRequests));
    builder.append("timeoutRequests", static_cast<long long>(timeoutRequests));
    builder.append("overflowRequests", static_cast<long long>(overflowRequests));
    builder.append("versionGapSkippedRequests", static_cast<long long>(versionGapSkippedRequests));
    builder.append("activeGroups", static_cast<long long>(activeGroups));
    builder.append("coalescingRate", coalescingRate());
    return builder.obj();
}

// ============================================================================
// Singleton Implementation
// ============================================================================

ConfigQueryCoalescer& ConfigQueryCoalescer::get() {
    static ConfigQueryCoalescer instance;
    return instance;
}

bool ConfigQueryCoalescer::isEnabled() {
    return configQueryCoalescerEnabled.load();
}

// ============================================================================
// ConfigQueryCoalescer Implementation
// ============================================================================

ConfigQueryCoalescer::ConfigQueryCoalescer() : ConfigQueryCoalescer(Config()) {}

ConfigQueryCoalescer::ConfigQueryCoalescer(Config config)
    : _config(std::move(config)) {}

ConfigQueryCoalescer::~ConfigQueryCoalescer() {
    shutdown();
}

StatusWith<std::vector<BSONObj>> ConfigQueryCoalescer::tryCoalesce(
    OperationContext* txn,
    const std::string& ns,
    uint64_t requestVersion,
    QueryFunc queryFunc) {

    if (_shutdown) {
        return Status(ErrorCodes::ShutdownInProgress,
                      "ConfigQueryCoalescer is shutting down");
    }

    // Update stats
    {
        stdx::lock_guard<stdx::mutex> lock(_statsMutex);
        _stats.totalRequests++;
    }

    std::vector<BSONObj> result;
    Status status = Status::OK();

    // Get config from server parameters
    Milliseconds windowDuration(configQueryCoalescerWindowMS.load());
    Milliseconds maxWait(configQueryCoalescerMaxWaitMS.load());
    size_t maxWaiters = static_cast<size_t>(configQueryCoalescerMaxWaiters.load());
    uint64_t maxVersionGap = static_cast<uint64_t>(configQueryCoalescerMaxVersionGap.load());

    stdx::unique_lock<stdx::mutex> lock(_mutex);

    auto it = _groups.find(ns);

    // Case 1: No existing group, create new one and become leader
    if (it == _groups.end()) {
        auto group = stdx::make_unique<CoalescingGroup>(ns);
        group->windowEnd = Date_t::now() + windowDuration;
        group->minVersion = requestVersion;
        group->maxVersion = requestVersion;
        group->waiters.emplace_back(&result, &status, requestVersion);

        _groups[ns] = std::move(group);

        {
            stdx::lock_guard<stdx::mutex> slock(_statsMutex);
            _stats.activeGroups = _groups.size();
        }

        // Wait for coalescing window
        _cv.wait_for(lock, windowDuration.toSystemDuration(), [this, &ns]() {
            return _shutdown || _groups.find(ns) == _groups.end() ||
                   _groups[ns]->queryCompleted;
        });

        // Check if we're still the leader
        it = _groups.find(ns);
        if (it != _groups.end() && !it->second->queryInProgress && !it->second->queryCompleted) {
            it->second->queryInProgress = true;

            // Release lock and execute query
            lock.unlock();

            LOG(2) << "ConfigQueryCoalescer: leader executing query for ns=" << ns;

            auto queryResult = queryFunc();

            lock.lock();

            // Store result and notify waiters
            it = _groups.find(ns);
            if (it != _groups.end()) {
                CoalescingGroup* group = it->second.get();
                group->queryCompleted = true;

                if (queryResult.isOK()) {
                    group->queryResult = std::move(queryResult.getValue());
                    group->queryStatus = Status::OK();

                    // Distribute results to all waiters
                    for (auto& waiter : group->waiters) {
                        *waiter.resultPtr = group->queryResult;
                        *waiter.statusPtr = Status::OK();
                        waiter.done = true;
                    }
                } else {
                    group->queryStatus = queryResult.getStatus();

                    for (auto& waiter : group->waiters) {
                        *waiter.statusPtr = queryResult.getStatus();
                        waiter.done = true;
                    }
                }

                // Update stats
                {
                    stdx::lock_guard<stdx::mutex> slock(_statsMutex);
                    _stats.actualQueries++;
                }

                // Remove group
                _groups.erase(it);

                {
                    stdx::lock_guard<stdx::mutex> slock(_statsMutex);
                    _stats.activeGroups = _groups.size();
                }
            }

            _cv.notify_all();
        } else {
            // Wait for result from another leader
            _cv.wait(lock, [this, &ns, &result]() {
                auto git = _groups.find(ns);
                if (git == _groups.end()) return true;
                if (_shutdown) return true;

                for (auto& w : git->second->waiters) {
                    if (w.resultPtr == &result && w.done) {
                        return true;
                    }
                }
                return git->second->queryCompleted;
            });
        }

        lock.unlock();

        if (!status.isOK()) {
            return status;
        }
        return result;
    }

    // Case 2: Existing group, join as follower
    CoalescingGroup* group = it->second.get();

    // Check version gap - if too large, execute independently
    uint64_t newMinVersion = std::min(group->minVersion, requestVersion);
    uint64_t newMaxVersion = std::max(group->maxVersion, requestVersion);
    if (newMaxVersion - newMinVersion > maxVersionGap) {
        lock.unlock();

        {
            stdx::lock_guard<stdx::mutex> slock(_statsMutex);
            _stats.versionGapSkippedRequests++;
            _stats.actualQueries++;
        }

        LOG(2) << "ConfigQueryCoalescer: version gap too large ("
               << (newMaxVersion - newMinVersion) << " > " << maxVersionGap
               << "), executing independent query for ns=" << ns;
        return queryFunc();
    }

    // Update version range
    group->minVersion = newMinVersion;
    group->maxVersion = newMaxVersion;

    // Check overflow
    if (group->waiters.size() >= maxWaiters) {
        lock.unlock();

        {
            stdx::lock_guard<stdx::mutex> slock(_statsMutex);
            _stats.overflowRequests++;
            _stats.actualQueries++;
        }

        LOG(2) << "ConfigQueryCoalescer: overflow, executing independent query for ns=" << ns;
        return queryFunc();
    }

    // Join waiting queue
    group->waiters.emplace_back(&result, &status, requestVersion);

    {
        stdx::lock_guard<stdx::mutex> slock(_statsMutex);
        _stats.coalescedRequests++;
    }

    // Wait for result
    bool timedOut = !_cv.wait_for(lock, maxWait.toSystemDuration(),
        [this, &ns, &result]() {
            if (_shutdown) return true;
            auto git = _groups.find(ns);
            if (git == _groups.end()) return true;

            for (auto& w : git->second->waiters) {
                if (w.resultPtr == &result && w.done) {
                    return true;
                }
            }
            return false;
        });

    if (timedOut) {
        stdx::lock_guard<stdx::mutex> slock(_statsMutex);
        _stats.timeoutRequests++;
    }

    lock.unlock();

    if (timedOut) {
        return Status(ErrorCodes::ExceededTimeLimit,
                      str::stream() << "Coalescing wait timed out for " << ns);
    }

    if (_shutdown) {
        return Status(ErrorCodes::ShutdownInProgress,
                      "ConfigQueryCoalescer is shutting down");
    }

    if (!status.isOK()) {
        return status;
    }

    return result;
}

ConfigQueryCoalescer::Stats ConfigQueryCoalescer::getStats() const {
    stdx::lock_guard<stdx::mutex> lock(_statsMutex);
    return _stats;
}

void ConfigQueryCoalescer::resetStats() {
    stdx::lock_guard<stdx::mutex> lock(_statsMutex);
    _stats = Stats();
}

void ConfigQueryCoalescer::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _shutdown = true;

    for (auto& pair : _groups) {
        for (auto& waiter : pair.second->waiters) {
            *waiter.statusPtr = Status(ErrorCodes::ShutdownInProgress,
                                       "ConfigQueryCoalescer is shutting down");
            waiter.done = true;
        }
    }

    _groups.clear();
    _cv.notify_all();
}

bool ConfigQueryCoalescer::isShutdown() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _shutdown;
}

}  // namespace mongo
