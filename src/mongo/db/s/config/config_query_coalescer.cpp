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

    // 所有数据都在调用者栈上，可以在锁外安全访问
    SharedResult sharedResult;
    Status status = Status::OK();
    std::atomic<bool> done{false};

    // Use instance config
    Milliseconds windowDuration = _config.coalescingWindow;
    Milliseconds maxWait = _config.maxWaitTime;
    size_t maxWaiters = _config.maxWaitersPerGroup;
    uint64_t maxVersionGap = _config.maxVersionGap;

    stdx::unique_lock<stdx::mutex> lock(_mutex);

    auto it = _groups.find(ns);

    // Case 1: No existing group → 创建 group，立即执行查询（无窗口等待）
    if (it == _groups.end()) {
        auto group = stdx::make_unique<CoalescingGroup>(ns);
        group->minVersion = requestVersion;
        group->maxVersion = requestVersion;
        group->queryInProgress = true;  // 立即标记为执行中
        group->waiters.emplace_back(&sharedResult, &status, &done, requestVersion);

        _groups[ns] = std::move(group);

        {
            stdx::lock_guard<stdx::mutex> slock(_statsMutex);
            _stats.activeGroups = _groups.size();
        }

        // ========== 立即执行查询（无窗口等待）==========
        lock.unlock();

        LOG(2) << "ConfigQueryCoalescer: leader executing query for ns=" << ns;
        auto queryResult = queryFunc();

        // ========== 重新获取锁，分发结果 ==========
        lock.lock();

        it = _groups.find(ns);
        if (it != _groups.end()) {
            CoalescingGroup* grp = it->second.get();
            grp->queryCompleted = true;

            // 创建 shared_ptr 存储结果
            SharedResult resultPtr;
            Status resultStatus = Status::OK();
            if (queryResult.isOK()) {
                resultPtr = std::make_shared<std::vector<BSONObj>>(
                    std::move(queryResult.getValue()));
            } else {
                resultStatus = queryResult.getStatus();
            }

            // 分发结果给所有等待者（包括自己）
            for (auto& w : grp->waiters) {
                if (resultPtr) {
                    *w.sharedResultPtr = resultPtr;
                }
                *w.statusPtr = resultStatus;
                w.donePtr->store(true, std::memory_order_release);
            }

            // 删除 group
            _groups.erase(it);

            {
                stdx::lock_guard<stdx::mutex> slock(_statsMutex);
                _stats.actualQueries++;
                _stats.activeGroups = _groups.size();
            }
        }

        lock.unlock();
        _cv.notify_all();

        // 返回结果
        if (!status.isOK()) {
            return status;
        }
        if (sharedResult) {
            return *sharedResult;
        }
        return std::vector<BSONObj>();
    }

    // Case 2: Existing group - 加入等待队列，复用结果
    CoalescingGroup* group = it->second.get();

    // 检查版本差距 - 如果太大，独立执行
    uint64_t newMinVersion = std::min(group->minVersion, requestVersion);
    uint64_t newMaxVersion = std::max(group->maxVersion, requestVersion);
    if (newMaxVersion - newMinVersion > maxVersionGap) {
        lock.unlock();

        {
            stdx::lock_guard<stdx::mutex> slock(_statsMutex);
            _stats.versionGapSkippedRequests++;
            _stats.actualQueries++;
        }

        LOG(2) << "ConfigQueryCoalescer: version gap too large, executing independent query for ns=" << ns;
        return queryFunc();
    }

    // 检查等待者数量限制
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

    // 加入等待队列，复用 leader 的查询结果
    group->waiters.emplace_back(&sharedResult, &status, &done, requestVersion);

    {
        stdx::lock_guard<stdx::mutex> slock(_statsMutex);
        _stats.coalescedRequests++;
    }

    // 等待 leader 完成查询
    Milliseconds maxTotalWait = _config.maxTotalWaitTime;
    Date_t startTime = Date_t::now();

    while (true) {
        Milliseconds elapsed = Date_t::now() - startTime;
        if (elapsed >= maxTotalWait) {
            // 总超时
            auto git = _groups.find(ns);
            if (git != _groups.end()) {
                git->second->waiters.remove_if([&done](const Waiter& w) {
                    return w.donePtr == &done;
                });
            }

            {
                stdx::lock_guard<stdx::mutex> slock(_statsMutex);
                _stats.timeoutRequests++;
            }

            lock.unlock();
            return Status(ErrorCodes::ExceededTimeLimit,
                          str::stream() << "Coalescing wait timed out for " << ns);
        }

        Milliseconds remainingTime = maxTotalWait - elapsed;
        Milliseconds waitTime = std::min(maxWait, remainingTime);

        bool timedOut = !_cv.wait_for(lock, waitTime.toSystemDuration(),
            [&done, this]() {
                if (_shutdown) return true;
                return done.load(std::memory_order_acquire);
            });

        if (done.load(std::memory_order_acquire) || _shutdown) {
            break;
        }

        // 单次超时但未完成，尝试升级为 leader
        if (timedOut) {
            auto git = _groups.find(ns);
            if (git != _groups.end() && !git->second->queryInProgress && !git->second->queryCompleted) {
                // 升级为 leader
                git->second->queryInProgress = true;

                git->second->waiters.remove_if([&done](const Waiter& w) {
                    return w.donePtr == &done;
                });

                LOG(2) << "ConfigQueryCoalescer: follower promoted to leader for ns=" << ns;

                lock.unlock();
                auto queryResult = queryFunc();
                lock.lock();

                git = _groups.find(ns);
                if (git != _groups.end()) {
                    CoalescingGroup* grp = git->second.get();
                    grp->queryCompleted = true;

                    SharedResult resultPtr;
                    Status resultStatus = Status::OK();
                    if (queryResult.isOK()) {
                        resultPtr = std::make_shared<std::vector<BSONObj>>(
                            std::move(queryResult.getValue()));
                    } else {
                        resultStatus = queryResult.getStatus();
                    }

                    for (auto& w : grp->waiters) {
                        if (resultPtr) {
                            *w.sharedResultPtr = resultPtr;
                        }
                        *w.statusPtr = resultStatus;
                        w.donePtr->store(true, std::memory_order_release);
                    }

                    _groups.erase(git);

                    {
                        stdx::lock_guard<stdx::mutex> slock(_statsMutex);
                        _stats.actualQueries++;
                        _stats.activeGroups = _groups.size();
                    }
                }

                lock.unlock();
                _cv.notify_all();

                return queryResult;
            }
        }
    }

    lock.unlock();

    if (_shutdown) {
        return Status(ErrorCodes::ShutdownInProgress,
                      "ConfigQueryCoalescer is shutting down");
    }

    if (!status.isOK()) {
        return status;
    }
    if (sharedResult) {
        return *sharedResult;
    }
    return std::vector<BSONObj>();
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
            waiter.donePtr->store(true, std::memory_order_release);
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
