/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    Config Server Query Coalescer
 *
 *    在 Config Server 侧合并来自多个 mongos 的 config.chunks 查询请求
 *    核心优化：将同一 namespace 的并发请求合并为一次查询，结果分发给所有等待者
 */

#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/time_support.h"

namespace mongo {

class OperationContext;

/**
 * ConfigQueryCoalescer
 *
 * Config Server 侧的请求合并器
 *
 * 工作原理：
 * 1. 请求到达时，检查是否有同一 namespace 的合并组
 * 2. 如果有，加入等待队列
 * 3. 如果没有，创建新的合并组，等待合并窗口
 * 4. 窗口结束后，leader 调用 queryFunc 执行一次查询
 * 5. 结果分发给所有等待者
 *
 * 线程安全：所有公开方法都是线程安全的
 */
class ConfigQueryCoalescer {
    MONGO_DISALLOW_COPYING(ConfigQueryCoalescer);

public:
    /**
     * 获取全局单例
     */
    static ConfigQueryCoalescer& get();

    /**
     * 检查合并器是否启用
     */
    static bool isEnabled();

    /**
     * 查询函数类型
     */
    using QueryFunc = std::function<StatusWith<std::vector<BSONObj>>()>;

    /**
     * 统计信息
     */
    struct Stats {
        uint64_t totalRequests{0};
        uint64_t actualQueries{0};
        uint64_t coalescedRequests{0};
        uint64_t timeoutRequests{0};
        uint64_t overflowRequests{0};
        uint64_t versionGapSkippedRequests{0};  // 版本差距过大独立执行的请求
        uint64_t activeGroups{0};

        double coalescingRate() const {
            return totalRequests > 0 ?
                static_cast<double>(coalescedRequests) / totalRequests : 0.0;
        }

        BSONObj toBSON() const;
    };

    /**
     * 配置参数
     */
    struct Config {
        Milliseconds coalescingWindow{Milliseconds(5)};
        Milliseconds maxWaitTime{Milliseconds(100)};
        size_t maxWaitersPerGroup{1000};

        Config() = default;
    };

    ConfigQueryCoalescer();
    explicit ConfigQueryCoalescer(Config config);
    ~ConfigQueryCoalescer();

    /**
     * 尝试合并查询（回调模式）
     *
     * - 如果当前请求是 leader，等待合并窗口后调用 queryFunc 执行查询
     * - 如果当前请求是 follower，等待 leader 完成查询后复用结果
     * - 如果版本差距过大（超过 maxVersionGap），独立执行查询
     *
     * @param txn 操作上下文
     * @param ns 查询的 namespace（用于分组）
     * @param requestVersion 请求的版本号（从 lastmod.$gt 提取）
     * @param queryFunc 执行查询的回调函数
     * @return 查询结果
     */
    StatusWith<std::vector<BSONObj>> tryCoalesce(
        OperationContext* txn,
        const std::string& ns,
        uint64_t requestVersion,
        QueryFunc queryFunc);

    /**
     * 获取统计信息
     */
    Stats getStats() const;

    /**
     * 重置统计信息
     */
    void resetStats();

    /**
     * 关闭合并器
     */
    void shutdown();

    /**
     * 是否已关闭
     */
    bool isShutdown() const;

private:
    /**
     * 等待者信息
     */
    struct Waiter {
        std::vector<BSONObj>* resultPtr;
        Status* statusPtr;
        uint64_t requestedVersion{0};  // 请求的版本号
        bool done{false};

        Waiter(std::vector<BSONObj>* res, Status* stat, uint64_t version)
            : resultPtr(res), statusPtr(stat), requestedVersion(version) {}
    };

    /**
     * 合并组
     */
    struct CoalescingGroup {
        std::string ns;
        Date_t windowEnd;
        uint64_t minVersion{UINT64_MAX};  // 组内最小版本
        uint64_t maxVersion{0};           // 组内最大版本
        bool queryInProgress{false};
        bool queryCompleted{false};
        std::vector<BSONObj> queryResult;
        Status queryStatus{Status::OK()};
        std::list<Waiter> waiters;

        explicit CoalescingGroup(const std::string& namespace_)
            : ns(namespace_), windowEnd(Date_t::now()) {}
    };

    Config _config;
    mutable stdx::mutex _mutex;
    stdx::condition_variable _cv;
    std::map<std::string, std::unique_ptr<CoalescingGroup>> _groups;

    mutable stdx::mutex _statsMutex;
    Stats _stats;

    bool _shutdown{false};
};

}  // namespace mongo
