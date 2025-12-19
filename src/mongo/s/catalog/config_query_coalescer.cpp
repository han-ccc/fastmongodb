/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    Config Server Query Coalescer Implementation
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/config_query_coalescer.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

// ============================================================================
// ChunkVersionLight 实现
// ============================================================================

ChunkVersionLight ChunkVersionLight::fromBSON(const BSONObj& obj, const std::string& field) {
    ChunkVersionLight version;
    BSONElement elem = obj[field];
    if (elem.type() == bsonTimestamp) {
        Timestamp ts = elem.timestamp();
        version.majorVersion = ts.getSecs();
        version.minorVersion = ts.getInc();
    }
    BSONElement epochElem = obj["epoch"];
    if (epochElem.type() == jstOID) {
        version.epoch = epochElem.OID();
    }
    return version;
}

BSONObj ChunkVersionLight::toBSON() const {
    BSONObjBuilder builder;
    builder.appendTimestamp("lastmod",
        (static_cast<uint64_t>(majorVersion) << 32) | minorVersion);
    builder.append("epoch", epoch);
    return builder.obj();
}

// ============================================================================
// Stats 实现
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
    builder.append("waitingRequests", static_cast<long long>(waitingRequests));
    builder.append("peakWaitingRequests", static_cast<long long>(peakWaitingRequests));
    builder.append("coalescingRate", coalescingRate());
    builder.append("querySavingRate", querySavingRate());
    return builder.obj();
}

// ============================================================================
// ConfigQueryCoalescer 实现
// ============================================================================

ConfigQueryCoalescer::ConfigQueryCoalescer(Config config)
    : _config(std::move(config)) {}

ConfigQueryCoalescer::~ConfigQueryCoalescer() {
    shutdown();
}

void ConfigQueryCoalescer::setQueryExecutor(QueryExecutor executor) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _queryExecutor = std::move(executor);
}

StatusWith<std::vector<BSONObj>> ConfigQueryCoalescer::getChunks(
    const std::string& ns,
    const ChunkVersionLight& sinceVersion) {

    if (_shutdown) {
        return Status(ErrorCodes::ShutdownInProgress,
                      "ConfigQueryCoalescer is shutting down");
    }

    // 更新统计
    {
        stdx::lock_guard<stdx::mutex> lock(_statsMutex);
        _stats.totalRequests++;
    }

    std::vector<BSONObj> result;
    Status status = Status::OK();
    std::shared_ptr<std::vector<BSONObj>> sharedResult;  // 锁优化: 共享结果指针

    stdx::unique_lock<stdx::mutex> lock(_mutex);

    auto it = _groups.find(ns);

    // 情况1: 没有进行中的合并组，创建新组
    if (it == _groups.end()) {
        auto group = stdx::make_unique<CoalescingGroup>(ns);
        group->minVersion = sinceVersion;
        group->windowEnd = Date_t::now() + getAdaptiveWindow();

        // 添加自己为第一个等待者
        group->waiters.emplace_back(sinceVersion, &result, &status);

        _groups[ns] = std::move(group);

        // 更新统计
        {
            stdx::lock_guard<stdx::mutex> slock(_statsMutex);
            _stats.activeGroups = _groups.size();
            _stats.waitingRequests++;
            if (_stats.waitingRequests > _stats.peakWaitingRequests) {
                _stats.peakWaitingRequests = _stats.waitingRequests;
            }
        }

        // 等待合并窗口结束
        Milliseconds windowDuration = getAdaptiveWindow();
        _cv.wait_for(lock, windowDuration.toSystemDuration(), [this, &ns]() {
            return _shutdown || _groups.find(ns) == _groups.end() ||
                   _groups[ns]->queryCompleted;
        });

        // 如果我们还是第一个（窗口结束），执行查询
        it = _groups.find(ns);
        if (it != _groups.end() && !it->second->queryInProgress && !it->second->queryCompleted) {
            it->second->queryInProgress = true;
            lock.unlock();
            executeAndDistribute(ns);
            lock.lock();
        }

        // 等待查询完成并获取共享结果
        _cv.wait(lock, [this, &ns, &result, &sharedResult]() {
            auto git = _groups.find(ns);
            if (git == _groups.end()) return true;
            if (_shutdown) return true;

            // 检查自己是否已完成
            for (auto wit = git->second->waiters.begin(); wit != git->second->waiters.end(); ++wit) {
                if (wit->resultPtr == &result && wit->done) {
                    // 锁优化: 获取共享结果指针
                    sharedResult = wit->sharedResultPtr;
                    return true;
                }
            }
            return git->second->queryCompleted;
        });

        // 更新统计
        {
            stdx::lock_guard<stdx::mutex> slock(_statsMutex);
            _stats.waitingRequests--;
            _stats.actualQueries++;
        }

        // 释放锁后进行过滤
        lock.unlock();

        if (!status.isOK()) {
            return status;
        }

        // 锁优化: 在锁外过滤结果
        if (sharedResult) {
            return filterResults(*sharedResult, sinceVersion);
        }
        return result;
    }

    // 情况2: 已有合并组
    CoalescingGroup* group = it->second.get();

    // 检查是否溢出
    if (group->waiters.size() >= _config.maxWaitersPerGroup) {
        lock.unlock();

        // 溢出，直接执行独立查询
        {
            stdx::lock_guard<stdx::mutex> slock(_statsMutex);
            _stats.overflowRequests++;
            _stats.actualQueries++;
        }

        if (!_queryExecutor) {
            return Status(ErrorCodes::BadValue, "Query executor not set");
        }
        return _queryExecutor(ns, sinceVersion);
    }

    // 检查版本差距是否过大
    // 如果新请求的版本比组内最小版本新很多，说明加入这个组会拖慢新请求
    // 如果新请求的版本比组内最小版本老很多，说明这个老请求会拖慢组内其他请求
    uint64_t groupMinVersionLong = group->minVersion.toLong();
    uint64_t requestVersionLong = sinceVersion.toLong();
    uint64_t versionGap = (groupMinVersionLong > requestVersionLong) ?
        (groupMinVersionLong - requestVersionLong) : (requestVersionLong - groupMinVersionLong);

    if (versionGap > _config.maxVersionGap) {
        lock.unlock();

        // 版本差距过大，独立执行查询
        {
            stdx::lock_guard<stdx::mutex> slock(_statsMutex);
            _stats.versionGapSkippedRequests++;
            _stats.actualQueries++;
        }

        if (!_queryExecutor) {
            return Status(ErrorCodes::BadValue, "Query executor not set");
        }
        return _queryExecutor(ns, sinceVersion);
    }

    // 更新最小版本
    if (sinceVersion < group->minVersion) {
        group->minVersion = sinceVersion;
    }

    // 加入等待队列
    group->waiters.emplace_back(sinceVersion, &result, &status);

    // 更新统计
    {
        stdx::lock_guard<stdx::mutex> slock(_statsMutex);
        _stats.waitingRequests++;
        _stats.coalescedRequests++;
        if (_stats.waitingRequests > _stats.peakWaitingRequests) {
            _stats.peakWaitingRequests = _stats.waitingRequests;
        }
    }

    // 等待结果并获取共享结果
    bool timedOut = !_cv.wait_for(lock, _config.maxWaitTime.toSystemDuration(),
        [this, &ns, &result, &sharedResult]() {
            if (_shutdown) return true;
            auto git = _groups.find(ns);
            if (git == _groups.end()) return true;

            // 检查自己是否已完成
            for (auto& w : git->second->waiters) {
                if (w.resultPtr == &result && w.done) {
                    // 锁优化: 获取共享结果指针
                    sharedResult = w.sharedResultPtr;
                    return true;
                }
            }
            return false;
        });

    // 更新统计
    {
        stdx::lock_guard<stdx::mutex> slock(_statsMutex);
        _stats.waitingRequests--;
        if (timedOut) {
            _stats.timeoutRequests++;
        }
    }

    // 释放锁后进行错误检查和过滤
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

    // 锁优化: 在锁外过滤结果
    if (sharedResult) {
        return filterResults(*sharedResult, sinceVersion);
    }
    return result;
}

void ConfigQueryCoalescer::executeAndDistribute(const std::string& ns) {
    // 获取合并组信息
    ChunkVersionLight minVersion;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        auto it = _groups.find(ns);
        if (it == _groups.end()) {
            return;
        }
        minVersion = it->second->minVersion;
    }

    // 执行实际查询（不持有锁）
    StatusWith<std::vector<BSONObj>> queryResult =
        Status(ErrorCodes::BadValue, "Query executor not set");

    if (_queryExecutor) {
        queryResult = _queryExecutor(ns, minVersion);
    }

    // =========================================================================
    // 锁竞争优化: 使用共享指针分发结果
    //
    // 优化前 (O(waiters × chunks), 4000ms+):
    //   lock()
    //   for (waiter : waiters) {
    //       for (chunk : chunks) { waiter.result.push_back(chunk); }  // 拷贝!
    //   }
    //   unlock()
    //
    // 优化后 (O(waiters), <1ms):
    //   sharedResult = make_shared(queryResult)
    //   lock()
    //   for (waiter : waiters) { waiter.sharedPtr = sharedResult; }  // O(1)指针赋值
    //   unlock()
    //   // 等待者在锁外自行过滤
    // =========================================================================

    // 创建共享结果（锁外）
    std::shared_ptr<std::vector<BSONObj>> sharedResult;
    if (queryResult.isOK()) {
        sharedResult = std::make_shared<std::vector<BSONObj>>(
            std::move(queryResult.getValue()));
    }

    // 分发结果 - 仅传递指针，O(waiters) 复杂度
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        auto it = _groups.find(ns);
        if (it == _groups.end()) {
            return;
        }

        CoalescingGroup* group = it->second.get();
        group->queryCompleted = true;

        if (sharedResult) {
            group->sharedQueryResult = sharedResult;
            group->queryStatus = Status::OK();

            // O(waiters) - 仅指针赋值，不拷贝数据
            for (auto& waiter : group->waiters) {
                waiter.sharedResultPtr = sharedResult;
                *waiter.statusPtr = Status::OK();
                waiter.done = true;
            }
        } else {
            group->queryStatus = queryResult.getStatus();

            // 通知所有等待者失败
            for (auto& waiter : group->waiters) {
                *waiter.statusPtr = queryResult.getStatus();
                waiter.done = true;
            }
        }

        // 移除合并组
        _groups.erase(it);

        // 更新统计
        {
            stdx::lock_guard<stdx::mutex> slock(_statsMutex);
            _stats.activeGroups = _groups.size();
        }
    }

    // 唤醒所有等待者
    _cv.notify_all();
}

std::vector<BSONObj> ConfigQueryCoalescer::filterResults(
    const std::vector<BSONObj>& allResults,
    const ChunkVersionLight& requestedVersion) {

    std::vector<BSONObj> filtered;
    filtered.reserve(allResults.size());

    for (const auto& chunk : allResults) {
        ChunkVersionLight chunkVersion = ChunkVersionLight::fromBSON(chunk);
        if (chunkVersion >= requestedVersion) {
            filtered.push_back(chunk);
        }
    }

    return filtered;
}

Milliseconds ConfigQueryCoalescer::getAdaptiveWindow() const {
    if (!_config.adaptiveWindow) {
        return _config.coalescingWindow;
    }

    // 根据当前等待请求数调整窗口
    size_t waiting = 0;
    {
        stdx::lock_guard<stdx::mutex> lock(_statsMutex);
        waiting = _stats.waitingRequests;
    }

    if (waiting < 10) {
        return _config.minWindow;
    } else if (waiting < 50) {
        return _config.coalescingWindow;
    } else if (waiting < 100) {
        return Milliseconds(10);
    } else {
        return _config.maxWindow;
    }
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

    // 通知所有等待者
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

size_t ConfigQueryCoalescer::activeGroupCount() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _groups.size();
}

size_t ConfigQueryCoalescer::waitingRequestCount() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    size_t count = 0;
    for (const auto& pair : _groups) {
        count += pair.second->waiters.size();
    }
    return count;
}

}  // namespace mongo
