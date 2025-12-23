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
std::atomic<long long> configQueryCoalescerMaxVersionGap{500};

// ============================================================================
// Custom ServerParameter classes - 修复 #18 (参数断开) 和 #19 (缺少验证)
// ============================================================================

// Bool parameter - 连接到 atomic<bool>
class CoalescerEnabledParameter : public ServerParameter {
public:
    CoalescerEnabledParameter()
        : ServerParameter(ServerParameterSet::getGlobal(),
                          "configQueryCoalescerEnabled",
                          true,   // allowedToChangeAtStartup
                          true) { // allowedToChangeAtRuntime
    }

    void append(OperationContext*, BSONObjBuilder& b, const std::string& name) override {
        b.append(name, configQueryCoalescerEnabled.load());
    }

    Status set(const BSONElement& newValueElement) override {
        if (newValueElement.type() != Bool) {
            return Status(ErrorCodes::TypeMismatch, "configQueryCoalescerEnabled must be a boolean");
        }
        configQueryCoalescerEnabled.store(newValueElement.Bool());
        return Status::OK();
    }

    Status setFromString(const std::string& str) override {
        if (str == "true" || str == "1") {
            configQueryCoalescerEnabled.store(true);
        } else if (str == "false" || str == "0") {
            configQueryCoalescerEnabled.store(false);
        } else {
            return Status(ErrorCodes::BadValue, "configQueryCoalescerEnabled must be 'true' or 'false'");
        }
        return Status::OK();
    }
} coalescerEnabledParameter;

// Int parameter with validation - 连接到 atomic<int>，带范围验证
template <std::atomic<int>* atomicVar, int minVal, int maxVal>
class CoalescerIntParameter : public ServerParameter {
public:
    CoalescerIntParameter(const std::string& name)
        : ServerParameter(ServerParameterSet::getGlobal(), name, true, true) {}

    void append(OperationContext*, BSONObjBuilder& b, const std::string& name) override {
        b.append(name, atomicVar->load());
    }

    Status set(const BSONElement& newValueElement) override {
        if (!newValueElement.isNumber()) {
            return Status(ErrorCodes::TypeMismatch, name() + " must be a number");
        }
        int newValue = newValueElement.numberInt();
        if (newValue < minVal || newValue > maxVal) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << name() << " must be between " << minVal << " and " << maxVal);
        }
        atomicVar->store(newValue);
        return Status::OK();
    }

    Status setFromString(const std::string& str) override {
        int newValue;
        try {
            newValue = std::stoi(str);
        } catch (...) {
            return Status(ErrorCodes::BadValue, name() + " must be a valid integer");
        }
        if (newValue < minVal || newValue > maxVal) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << name() << " must be between " << minVal << " and " << maxVal);
        }
        atomicVar->store(newValue);
        return Status::OK();
    }
};

// Long long parameter with validation
class CoalescerMaxVersionGapParameter : public ServerParameter {
public:
    CoalescerMaxVersionGapParameter()
        : ServerParameter(ServerParameterSet::getGlobal(),
                          "configQueryCoalescerMaxVersionGap",
                          true, true) {}

    void append(OperationContext*, BSONObjBuilder& b, const std::string& name) override {
        b.append(name, configQueryCoalescerMaxVersionGap.load());
    }

    Status set(const BSONElement& newValueElement) override {
        if (!newValueElement.isNumber()) {
            return Status(ErrorCodes::TypeMismatch, "configQueryCoalescerMaxVersionGap must be a number");
        }
        long long newValue = newValueElement.numberLong();
        if (newValue < 1 || newValue > 100000) {
            return Status(ErrorCodes::BadValue,
                          "configQueryCoalescerMaxVersionGap must be between 1 and 100000");
        }
        configQueryCoalescerMaxVersionGap.store(newValue);
        return Status::OK();
    }

    Status setFromString(const std::string& str) override {
        long long newValue;
        try {
            newValue = std::stoll(str);
        } catch (...) {
            return Status(ErrorCodes::BadValue, "configQueryCoalescerMaxVersionGap must be a valid integer");
        }
        if (newValue < 1 || newValue > 100000) {
            return Status(ErrorCodes::BadValue,
                          "configQueryCoalescerMaxVersionGap must be between 1 and 100000");
        }
        configQueryCoalescerMaxVersionGap.store(newValue);
        return Status::OK();
    }
} coalescerMaxVersionGapParameter;

// 实例化参数对象
CoalescerIntParameter<&configQueryCoalescerWindowMS, 1, 1000>
    coalescerWindowMSParameter("configQueryCoalescerWindowMS");

CoalescerIntParameter<&configQueryCoalescerMaxWaitMS, 10, 60000>
    coalescerMaxWaitMSParameter("configQueryCoalescerMaxWaitMS");

CoalescerIntParameter<&configQueryCoalescerMaxWaiters, 1, 100000>
    coalescerMaxWaitersParameter("configQueryCoalescerMaxWaiters");

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

    // 创建共享状态（在堆上，生命周期由 shared_ptr 管理）
    // 解决悬空指针问题：即使调用者提前返回，shared_ptr 保证内存不释放
    auto waiterState = std::make_shared<WaiterState>();

    // Use instance config
    Milliseconds maxWait = _config.maxWaitTime;
    size_t maxWaiters = _config.maxWaitersPerGroup;
    uint64_t maxVersionGap = _config.maxVersionGap;

    stdx::unique_lock<stdx::mutex> lock(_mutex);

    auto it = _groups.find(ns);

    // Case 1: No existing group → 创建 group，立即执行查询（无窗口等待）
    if (it == _groups.end()) {
        uint64_t myGeneration = ++_nextGeneration;
        auto group = stdx::make_unique<CoalescingGroup>(ns, myGeneration);
        group->minVersion = requestVersion;
        group->maxVersion = requestVersion;
        group->queryInProgress = true;  // 立即标记为执行中
        group->waiters.emplace_back(waiterState, requestVersion);

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

        // 检查 shutdown（修复 #12）
        if (_shutdown) {
            lock.unlock();
            return Status(ErrorCodes::ShutdownInProgress,
                          "ConfigQueryCoalescer is shutting down");
        }

        // 用 generation 验证是同一个 group（修复 #3）
        it = _groups.find(ns);
        if (it != _groups.end() && it->second->generation == myGeneration) {
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

            // 分发结果给所有等待者（通过 shared_ptr，安全）
            for (auto& w : grp->waiters) {
                w.state->result = resultPtr;
                w.state->status = resultStatus;
                w.state->done.store(true, std::memory_order_release);
            }

            // 删除 group
            _groups.erase(it);

            {
                stdx::lock_guard<stdx::mutex> slock(_statsMutex);
                _stats.actualQueries++;
                _stats.activeGroups = _groups.size();
            }
        }
        // 如果 generation 不匹配，说明 group 被重建了，我们的 waiters 已经被其他 leader 处理

        lock.unlock();
        _cv.notify_all();

        // 从自己的 waiterState 读取结果（安全，因为是 shared_ptr）
        if (!waiterState->status.isOK()) {
            return waiterState->status;
        }
        if (waiterState->result) {
            return *waiterState->result;
        }
        return std::vector<BSONObj>();
    }

    // Case 2: Existing group - 加入等待队列，复用结果
    CoalescingGroup* group = it->second.get();
    uint64_t groupGeneration = group->generation;  // 记住当前 generation

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
    group->waiters.emplace_back(waiterState, requestVersion);

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
            // 总超时 - 从等待队列中移除自己
            auto git = _groups.find(ns);
            if (git != _groups.end() && git->second->generation == groupGeneration) {
                git->second->waiters.remove_if([&waiterState](const Waiter& w) {
                    return w.state == waiterState;
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
            [&waiterState, this]() {
                if (_shutdown) return true;
                return waiterState->done.load(std::memory_order_acquire);
            });

        if (waiterState->done.load(std::memory_order_acquire) || _shutdown) {
            break;
        }

        // 单次超时但未完成，尝试升级为 leader
        if (timedOut) {
            auto git = _groups.find(ns);
            if (git != _groups.end() &&
                git->second->generation == groupGeneration &&
                !git->second->queryInProgress &&
                !git->second->queryCompleted) {

                // 升级为 leader
                git->second->queryInProgress = true;
                uint64_t myGeneration = git->second->generation;

                // 从等待队列中移除自己（作为 leader 直接返回结果）
                git->second->waiters.remove_if([&waiterState](const Waiter& w) {
                    return w.state == waiterState;
                });

                LOG(2) << "ConfigQueryCoalescer: follower promoted to leader for ns=" << ns;

                lock.unlock();
                auto queryResult = queryFunc();
                lock.lock();

                // 检查 shutdown（修复 #12）
                if (_shutdown) {
                    lock.unlock();
                    return Status(ErrorCodes::ShutdownInProgress,
                                  "ConfigQueryCoalescer is shutting down");
                }

                // 用 generation 验证是同一个 group（修复 #3）
                git = _groups.find(ns);
                if (git != _groups.end() && git->second->generation == myGeneration) {
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

                    // 分发结果给剩余等待者
                    for (auto& w : grp->waiters) {
                        w.state->result = resultPtr;
                        w.state->status = resultStatus;
                        w.state->done.store(true, std::memory_order_release);
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

    // 从自己的 waiterState 读取结果
    if (!waiterState->status.isOK()) {
        return waiterState->status;
    }
    if (waiterState->result) {
        return *waiterState->result;
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

    // 通过 shared_ptr 安全地通知所有等待者
    // 即使调用者已经返回，shared_ptr 保证内存有效
    for (auto& pair : _groups) {
        for (auto& waiter : pair.second->waiters) {
            waiter.state->status = Status(ErrorCodes::ShutdownInProgress,
                                          "ConfigQueryCoalescer is shutting down");
            waiter.state->done.store(true, std::memory_order_release);
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
