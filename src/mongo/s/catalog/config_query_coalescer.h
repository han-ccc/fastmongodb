/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    Config Server Query Coalescer
 *
 *    在 Config Server 侧合并来自多个 mongos 的路由查询请求
 *    核心优化：将同一 collection 的并发请求合并为一次查询，结果分发给所有等待者
 *
 *    设计目标：
 *    - 支持 1000+ mongos 并发请求
 *    - 合并率 95%+ (mongos 越多效果越好)
 *    - 平均延迟增加 < 10ms
 */

#pragma once

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * ChunkVersion 简化表示
 * 用于请求合并时的版本比较
 */
struct ChunkVersionLight {
    uint32_t majorVersion;
    uint32_t minorVersion;
    OID epoch;

    ChunkVersionLight() : majorVersion(0), minorVersion(0) {}
    ChunkVersionLight(uint32_t major, uint32_t minor, const OID& e)
        : majorVersion(major), minorVersion(minor), epoch(e) {}

    bool operator<(const ChunkVersionLight& other) const {
        if (epoch != other.epoch) {
            // epoch 不同，无法比较，认为当前版本更旧
            return true;
        }
        if (majorVersion != other.majorVersion) {
            return majorVersion < other.majorVersion;
        }
        return minorVersion < other.minorVersion;
    }

    bool operator>=(const ChunkVersionLight& other) const {
        return !(*this < other);
    }

    bool operator==(const ChunkVersionLight& other) const {
        return epoch == other.epoch &&
               majorVersion == other.majorVersion &&
               minorVersion == other.minorVersion;
    }

    uint64_t toLong() const {
        return (static_cast<uint64_t>(majorVersion) << 32) | minorVersion;
    }

    static ChunkVersionLight fromBSON(const BSONObj& obj, const std::string& field = "lastmod");
    BSONObj toBSON() const;
};

/**
 * ConfigQueryCoalescer
 *
 * Config Server 侧的请求合并器
 *
 * 工作原理：
 * 1. 请求到达时，检查是否有同一 namespace 的合并组
 * 2. 如果有，加入等待队列
 * 3. 如果没有，创建新的合并组，等待合并窗口
 * 4. 窗口结束后，使用最小版本执行一次查询
 * 5. 结果分发给所有等待者（每个等待者过滤出自己需要的版本）
 *
 * 线程安全：所有公开方法都是线程安全的
 */
class ConfigQueryCoalescer {
    MONGO_DISALLOW_COPYING(ConfigQueryCoalescer);

public:
    /**
     * 配置参数
     */
    struct Config {
        // 合并窗口大小（默认 5ms）
        // 窗口内到达的同一 collection 请求会被合并
        Milliseconds coalescingWindow;

        // 最大等待时间（默认 100ms）
        // 超过此时间的请求会超时返回
        Milliseconds maxWaitTime;

        // 每个合并组最大等待者数量（默认 1000）
        // 超过后新请求会独立执行
        size_t maxWaitersPerGroup;

        // 自适应窗口：是否根据负载动态调整窗口大小
        bool adaptiveWindow;

        // 自适应窗口的最小值
        Milliseconds minWindow;

        // 自适应窗口的最大值
        Milliseconds maxWindow;

        // 最大版本差距（默认 500）
        // 如果请求版本与组内最小版本差距超过此值，独立执行查询
        // 防止一个老版本 mongos 拖慢所有新版本 mongos
        uint32_t maxVersionGap;

        Config()
            : coalescingWindow(Milliseconds(5)),
              maxWaitTime(Milliseconds(100)),
              maxWaitersPerGroup(1000),
              adaptiveWindow(true),
              minWindow(Milliseconds(2)),
              maxWindow(Milliseconds(20)),
              maxVersionGap(500) {}
    };

    /**
     * 统计信息
     */
    struct Stats {
        // 总请求数
        uint64_t totalRequests{0};

        // 实际执行的查询数
        uint64_t actualQueries{0};

        // 合并的请求数（等待复用结果的请求）
        uint64_t coalescedRequests{0};

        // 超时的请求数
        uint64_t timeoutRequests{0};

        // 溢出的请求数（超过 maxWaitersPerGroup）
        uint64_t overflowRequests{0};

        // 版本差距过大独立执行的请求数
        uint64_t versionGapSkippedRequests{0};

        // 当前活跃的合并组数
        uint64_t activeGroups{0};

        // 当前等待中的请求数
        uint64_t waitingRequests{0};

        // 峰值等待请求数
        uint64_t peakWaitingRequests{0};

        // 平均合并率 (coalescedRequests / totalRequests)
        double coalescingRate() const {
            return totalRequests > 0 ?
                static_cast<double>(coalescedRequests) / totalRequests : 0.0;
        }

        // 查询节省率 (1 - actualQueries / totalRequests)
        double querySavingRate() const {
            return totalRequests > 0 ?
                1.0 - static_cast<double>(actualQueries) / totalRequests : 0.0;
        }

        BSONObj toBSON() const;
    };

    /**
     * 查询执行器类型
     * 实际执行查询的回调函数
     *
     * @param ns 命名空间
     * @param sinceVersion 查询的起始版本
     * @return 查询结果（chunk 列表）
     */
    using QueryExecutor = std::function<StatusWith<std::vector<BSONObj>>(
        const std::string& ns,
        const ChunkVersionLight& sinceVersion)>;

    /**
     * 构造函数
     *
     * @param config 配置参数
     */
    explicit ConfigQueryCoalescer(Config config = Config());

    /**
     * 析构函数
     */
    ~ConfigQueryCoalescer();

    /**
     * 设置查询执行器
     * 必须在调用 getChunks 之前设置
     */
    void setQueryExecutor(QueryExecutor executor);

    /**
     * 获取 chunks（带合并）
     *
     * 这是核心接口，会自动处理请求合并
     *
     * @param ns 命名空间
     * @param sinceVersion 请求的起始版本（查询 >= 此版本的 chunks）
     * @return chunks 列表
     */
    StatusWith<std::vector<BSONObj>> getChunks(
        const std::string& ns,
        const ChunkVersionLight& sinceVersion);

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
     * 会唤醒所有等待中的请求并返回错误
     */
    void shutdown();

    /**
     * 是否已关闭
     */
    bool isShutdown() const;

    /**
     * 获取当前活跃的合并组数
     */
    size_t activeGroupCount() const;

    /**
     * 获取当前等待的请求数
     */
    size_t waitingRequestCount() const;

private:
    /**
     * 等待者信息
     *
     * 锁竞争优化:
     * - 使用共享指针存储查询结果，避免在锁持有期间拷贝
     * - 等待者在锁外自行过滤结果，减少锁持有时间
     * - 将锁持有时间从 O(waiters × chunks) 降为 O(waiters)
     */
    struct Waiter {
        ChunkVersionLight requestedVersion;           // 请求的版本
        std::vector<BSONObj>* resultPtr;              // 结果存放位置
        Status* statusPtr;                            // 状态存放位置
        std::shared_ptr<std::vector<BSONObj>> sharedResultPtr;  // 共享结果指针 (锁优化)
        bool done{false};                             // 是否已完成

        Waiter(const ChunkVersionLight& ver, std::vector<BSONObj>* res, Status* stat)
            : requestedVersion(ver), resultPtr(res), statusPtr(stat) {}
    };

    /**
     * 合并组
     * 管理同一 namespace 的并发请求
     *
     * 锁竞争优化:
     * - 使用 shared_ptr 存储查询结果，分发时仅传递指针
     * - 锁持有时间从 O(waiters × chunks) 降为 O(waiters)
     */
    struct CoalescingGroup {
        std::string ns;                      // 命名空间
        ChunkVersionLight minVersion;        // 所有等待者中的最小版本
        ChunkVersionLight maxVersion;        // 所有等待者中的最大版本 (用于多组策略)
        Date_t windowStart;                  // 合并窗口开始时间
        Date_t windowEnd;                    // 合并窗口结束时间
        bool queryInProgress{false};         // 是否正在执行查询
        bool queryCompleted{false};          // 查询是否已完成
        std::shared_ptr<std::vector<BSONObj>> sharedQueryResult;  // 共享查询结果 (锁优化)
        Status queryStatus{Status::OK()};    // 查询状态
        std::list<Waiter> waiters;           // 等待者列表
        size_t groupId{0};                   // 组标识 (用于调试)

        explicit CoalescingGroup(const std::string& namespace_, size_t id = 0)
            : ns(namespace_), windowStart(Date_t::now()), groupId(id) {}
    };

    /**
     * 计算自适应合并窗口大小
     */
    Milliseconds getAdaptiveWindow() const;

    /**
     * 执行查询并分发结果
     */
    void executeAndDistribute(const std::string& ns);

    /**
     * 过滤结果，只保留 >= requestedVersion 的 chunks
     */
    static std::vector<BSONObj> filterResults(
        const std::vector<BSONObj>& allResults,
        const ChunkVersionLight& requestedVersion);

    /**
     * 更新统计信息
     */
    void updateStats(bool isCoalesced, bool isTimeout, bool isOverflow);

    /**
     * 查找或创建合适的合并组
     * 多组策略：允许同一 namespace 有多个组，根据版本范围选择最佳组
     */
    CoalescingGroup* findOrCreateGroup(const std::string& ns,
                                        const ChunkVersionLight& version);

    Config _config;
    QueryExecutor _queryExecutor;

    mutable stdx::mutex _mutex;
    stdx::condition_variable _cv;

    // namespace -> 合并组 (单组模式)
    std::map<std::string, std::unique_ptr<CoalescingGroup>> _groups;

    // 统计信息
    mutable stdx::mutex _statsMutex;
    Stats _stats;

    bool _shutdown{false};
};

}  // namespace mongo
