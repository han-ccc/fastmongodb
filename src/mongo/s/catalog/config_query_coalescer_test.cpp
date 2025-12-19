/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    Config Query Coalescer 功能单元测试
 *
 *    测试内容:
 *    1. 基本功能：单请求、多请求
 *    2. 合并逻辑：同一 namespace 的请求被合并
 *    3. 版本过滤：结果按请求版本过滤
 *    4. 超时处理：超时请求正确返回错误
 *    5. 溢出处理：超过最大等待者数时独立执行
 *    6. 关闭处理：shutdown 时正确唤醒等待者
 *    7. 多 namespace：不同 namespace 的请求独立处理
 */

#include "mongo/platform/basic.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/catalog/config_query_coalescer.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

// ============================================================================
// 辅助函数
// ============================================================================

/**
 * 创建模拟的 chunk 数据
 */
BSONObj makeChunk(const std::string& ns, uint32_t major, uint32_t minor, const OID& epoch) {
    BSONObjBuilder builder;
    builder.append("ns", ns);
    builder.append("min", BSON("x" << static_cast<int>(major * 100 + minor)));
    builder.append("max", BSON("x" << static_cast<int>(major * 100 + minor + 1)));
    builder.appendTimestamp("lastmod",
        (static_cast<uint64_t>(major) << 32) | minor);
    builder.append("epoch", epoch);
    builder.append("shard", "shard0");
    return builder.obj();
}

/**
 * 创建一系列 chunks
 */
std::vector<BSONObj> makeChunks(const std::string& ns,
                                 uint32_t startMajor,
                                 uint32_t count,
                                 const OID& epoch) {
    std::vector<BSONObj> chunks;
    for (uint32_t i = 0; i < count; i++) {
        chunks.push_back(makeChunk(ns, startMajor + i, 0, epoch));
    }
    return chunks;
}

// ============================================================================
// 基本功能测试
// ============================================================================

TEST(ConfigQueryCoalescerTest, BasicSingleRequest) {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(10);
    config.adaptiveWindow = false;

    ConfigQueryCoalescer coalescer(config);

    OID epoch = OID::gen();
    std::string ns = "test.collection";

    // 设置查询执行器
    std::atomic<int> queryCount{0};
    coalescer.setQueryExecutor([&](const std::string& queryNs,
                                   const ChunkVersionLight& sinceVersion) {
        queryCount++;
        return makeChunks(queryNs, 0, 10, epoch);
    });

    // 执行单个请求
    ChunkVersionLight version(0, 0, epoch);
    auto result = coalescer.getChunks(ns, version);

    ASSERT_TRUE(result.isOK());
    ASSERT_EQ(result.getValue().size(), 10u);
    ASSERT_EQ(queryCount.load(), 1);

    auto stats = coalescer.getStats();
    ASSERT_EQ(stats.totalRequests, 1u);
    ASSERT_EQ(stats.actualQueries, 1u);
}

TEST(ConfigQueryCoalescerTest, MultipleRequestsSameNamespace) {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(50);  // 较长窗口便于测试
    config.adaptiveWindow = false;

    ConfigQueryCoalescer coalescer(config);

    OID epoch = OID::gen();
    std::string ns = "test.collection";

    std::atomic<int> queryCount{0};
    coalescer.setQueryExecutor([&](const std::string& queryNs,
                                   const ChunkVersionLight& sinceVersion) {
        queryCount++;
        // 模拟查询耗时
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return makeChunks(queryNs, sinceVersion.majorVersion, 20, epoch);
    });

    // 启动多个并发请求
    const int numRequests = 10;
    std::vector<std::thread> threads;
    std::vector<StatusWith<std::vector<BSONObj>>> results(numRequests,
        Status(ErrorCodes::InternalError, "Not set"));

    for (int i = 0; i < numRequests; i++) {
        threads.emplace_back([&, i]() {
            ChunkVersionLight version(i, 0, epoch);
            results[i] = coalescer.getChunks(ns, version);
        });
        // 稍微错开请求时间
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    for (auto& t : threads) {
        t.join();
    }

    // 验证结果
    for (int i = 0; i < numRequests; i++) {
        ASSERT_TRUE(results[i].isOK()) << "Request " << i << " failed: "
                                       << results[i].getStatus().toString();
    }

    // 应该只执行了 1-2 次查询（大部分被合并）
    ASSERT_LTE(queryCount.load(), 3);

    auto stats = coalescer.getStats();
    ASSERT_EQ(stats.totalRequests, static_cast<uint64_t>(numRequests));
    ASSERT_GT(stats.coalescedRequests, 0u);
}

// ============================================================================
// 版本过滤测试
// ============================================================================

TEST(ConfigQueryCoalescerTest, VersionFiltering) {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(50);
    config.adaptiveWindow = false;

    ConfigQueryCoalescer coalescer(config);

    OID epoch = OID::gen();
    std::string ns = "test.collection";

    // 查询返回版本 0-19 的 chunks
    coalescer.setQueryExecutor([&](const std::string& queryNs,
                                   const ChunkVersionLight& sinceVersion) {
        return makeChunks(queryNs, 0, 20, epoch);
    });

    // 两个请求，不同版本
    std::vector<BSONObj> result1, result2;
    std::thread t1([&]() {
        ChunkVersionLight version(5, 0, epoch);  // 请求 >= 5
        auto res = coalescer.getChunks(ns, version);
        if (res.isOK()) result1 = std::move(res.getValue());
    });

    std::thread t2([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ChunkVersionLight version(10, 0, epoch);  // 请求 >= 10
        auto res = coalescer.getChunks(ns, version);
        if (res.isOK()) result2 = std::move(res.getValue());
    });

    t1.join();
    t2.join();

    // 结果应该按版本过滤
    ASSERT_EQ(result1.size(), 15u);  // 版本 5-19
    ASSERT_EQ(result2.size(), 10u);  // 版本 10-19
}

// ============================================================================
// 不同 Namespace 测试
// ============================================================================

TEST(ConfigQueryCoalescerTest, DifferentNamespaces) {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(30);
    config.adaptiveWindow = false;

    ConfigQueryCoalescer coalescer(config);

    OID epoch1 = OID::gen();
    OID epoch2 = OID::gen();

    std::atomic<int> queryCount{0};
    coalescer.setQueryExecutor([&](const std::string& queryNs,
                                   const ChunkVersionLight& sinceVersion) {
        queryCount++;
        if (queryNs == "test.coll1") {
            return makeChunks(queryNs, 0, 10, epoch1);
        } else {
            return makeChunks(queryNs, 0, 5, epoch2);
        }
    });

    // 对两个不同 namespace 发起请求
    StatusWith<std::vector<BSONObj>> result1 =
        Status(ErrorCodes::InternalError, "Not set");
    StatusWith<std::vector<BSONObj>> result2 =
        Status(ErrorCodes::InternalError, "Not set");

    std::thread t1([&]() {
        ChunkVersionLight version(0, 0, epoch1);
        result1 = coalescer.getChunks("test.coll1", version);
    });

    std::thread t2([&]() {
        ChunkVersionLight version(0, 0, epoch2);
        result2 = coalescer.getChunks("test.coll2", version);
    });

    t1.join();
    t2.join();

    ASSERT_TRUE(result1.isOK());
    ASSERT_TRUE(result2.isOK());
    ASSERT_EQ(result1.getValue().size(), 10u);
    ASSERT_EQ(result2.getValue().size(), 5u);

    // 两个不同的 namespace 应该各执行一次查询
    ASSERT_EQ(queryCount.load(), 2);
}

// ============================================================================
// 超时测试
// ============================================================================

TEST(ConfigQueryCoalescerTest, RequestTimeout) {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(10);
    config.maxWaitTime = Milliseconds(50);  // 短超时
    config.adaptiveWindow = false;

    ConfigQueryCoalescer coalescer(config);

    OID epoch = OID::gen();

    // 查询执行器故意延迟很长时间
    coalescer.setQueryExecutor([&](const std::string& queryNs,
                                   const ChunkVersionLight& sinceVersion) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return makeChunks(queryNs, 0, 10, epoch);
    });

    // 第一个请求会触发查询
    std::thread t1([&]() {
        ChunkVersionLight version(0, 0, epoch);
        coalescer.getChunks("test.collection", version);
    });

    // 等待第一个请求开始
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    // 第二个请求应该超时
    ChunkVersionLight version(5, 0, epoch);
    auto result = coalescer.getChunks("test.collection", version);

    // 应该超时
    ASSERT_FALSE(result.isOK());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ExceededTimeLimit);

    t1.join();

    auto stats = coalescer.getStats();
    ASSERT_GT(stats.timeoutRequests, 0u);
}

// ============================================================================
// 溢出测试
// ============================================================================

TEST(ConfigQueryCoalescerTest, OverflowHandling) {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(100);
    config.maxWaitersPerGroup = 5;  // 很小的限制
    config.adaptiveWindow = false;

    ConfigQueryCoalescer coalescer(config);

    OID epoch = OID::gen();

    std::atomic<int> queryCount{0};
    coalescer.setQueryExecutor([&](const std::string& queryNs,
                                   const ChunkVersionLight& sinceVersion) {
        queryCount++;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return makeChunks(queryNs, 0, 10, epoch);
    });

    // 启动多个请求，超过 maxWaitersPerGroup
    const int numRequests = 10;
    std::vector<std::thread> threads;
    std::vector<StatusWith<std::vector<BSONObj>>> results(numRequests,
        Status(ErrorCodes::InternalError, "Not set"));

    for (int i = 0; i < numRequests; i++) {
        threads.emplace_back([&, i]() {
            ChunkVersionLight version(0, 0, epoch);
            results[i] = coalescer.getChunks("test.collection", version);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 所有请求都应该成功
    for (int i = 0; i < numRequests; i++) {
        ASSERT_TRUE(results[i].isOK()) << "Request " << i << " failed";
    }

    // 由于溢出，应该执行了多次查询
    ASSERT_GT(queryCount.load(), 1);

    auto stats = coalescer.getStats();
    ASSERT_GT(stats.overflowRequests, 0u);
}

// ============================================================================
// 关闭测试
// ============================================================================

TEST(ConfigQueryCoalescerTest, ShutdownWakesWaiters) {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(1000);  // 很长窗口
    config.adaptiveWindow = false;

    ConfigQueryCoalescer coalescer(config);

    OID epoch = OID::gen();

    coalescer.setQueryExecutor([&](const std::string& queryNs,
                                   const ChunkVersionLight& sinceVersion) {
        return makeChunks(queryNs, 0, 10, epoch);
    });

    // 启动请求（会等待很长时间）
    StatusWith<std::vector<BSONObj>> result =
        Status(ErrorCodes::InternalError, "Not set");
    std::thread t([&]() {
        ChunkVersionLight version(0, 0, epoch);
        result = coalescer.getChunks("test.collection", version);
    });

    // 等待请求开始等待
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 关闭合并器
    coalescer.shutdown();

    t.join();

    // 应该返回关闭错误
    ASSERT_FALSE(result.isOK());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::ShutdownInProgress);
    ASSERT_TRUE(coalescer.isShutdown());
}

// ============================================================================
// 查询失败传播测试
// ============================================================================

TEST(ConfigQueryCoalescerTest, QueryFailurePropagation) {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(30);
    config.adaptiveWindow = false;

    ConfigQueryCoalescer coalescer(config);

    OID epoch = OID::gen();

    // 查询执行器返回错误
    coalescer.setQueryExecutor([&](const std::string& queryNs,
                                   const ChunkVersionLight& sinceVersion)
        -> StatusWith<std::vector<BSONObj>> {
        return Status(ErrorCodes::HostUnreachable, "Config server unreachable");
    });

    // 启动多个请求
    const int numRequests = 5;
    std::vector<std::thread> threads;
    std::vector<StatusWith<std::vector<BSONObj>>> results(numRequests,
        Status(ErrorCodes::InternalError, "Not set"));

    for (int i = 0; i < numRequests; i++) {
        threads.emplace_back([&, i]() {
            ChunkVersionLight version(0, 0, epoch);
            results[i] = coalescer.getChunks("test.collection", version);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (auto& t : threads) {
        t.join();
    }

    // 所有请求都应该收到相同的错误
    for (int i = 0; i < numRequests; i++) {
        ASSERT_FALSE(results[i].isOK());
        ASSERT_EQ(results[i].getStatus().code(), ErrorCodes::HostUnreachable);
    }
}

// ============================================================================
// 统计信息测试
// ============================================================================

TEST(ConfigQueryCoalescerTest, StatsAccuracy) {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(50);
    config.adaptiveWindow = false;

    ConfigQueryCoalescer coalescer(config);

    OID epoch = OID::gen();

    coalescer.setQueryExecutor([&](const std::string& queryNs,
                                   const ChunkVersionLight& sinceVersion) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return makeChunks(queryNs, 0, 10, epoch);
    });

    // 执行一批请求
    const int numRequests = 20;
    std::vector<std::thread> threads;

    for (int i = 0; i < numRequests; i++) {
        threads.emplace_back([&]() {
            ChunkVersionLight version(0, 0, epoch);
            coalescer.getChunks("test.collection", version);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    for (auto& t : threads) {
        t.join();
    }

    auto stats = coalescer.getStats();

    ASSERT_EQ(stats.totalRequests, static_cast<uint64_t>(numRequests));
    ASSERT_EQ(stats.totalRequests, stats.actualQueries + stats.coalescedRequests);
    ASSERT_GT(stats.coalescingRate(), 0.5);  // 至少 50% 合并率
    ASSERT_GT(stats.querySavingRate(), 0.5); // 至少节省 50% 查询

    // 测试重置
    coalescer.resetStats();
    stats = coalescer.getStats();
    ASSERT_EQ(stats.totalRequests, 0u);
}

// ============================================================================
// ChunkVersionLight 测试
// ============================================================================

TEST(ChunkVersionLightTest, Comparison) {
    OID epoch1 = OID::gen();
    OID epoch2 = OID::gen();

    ChunkVersionLight v1(1, 0, epoch1);
    ChunkVersionLight v2(2, 0, epoch1);
    ChunkVersionLight v3(1, 5, epoch1);
    ChunkVersionLight v4(1, 0, epoch2);

    // 同 epoch 比较
    ASSERT_TRUE(v1 < v2);
    ASSERT_TRUE(v1 < v3);
    ASSERT_FALSE(v2 < v1);

    // 不同 epoch
    ASSERT_TRUE(v1 < v4);  // 不同 epoch 认为当前更旧

    // 相等
    ChunkVersionLight v5(1, 0, epoch1);
    ASSERT_TRUE(v1 == v5);
    ASSERT_TRUE(v1 >= v5);
}

TEST(ChunkVersionLightTest, BSONSerialization) {
    OID epoch = OID::gen();
    ChunkVersionLight original(10, 5, epoch);

    BSONObj bson = original.toBSON();
    ChunkVersionLight parsed = ChunkVersionLight::fromBSON(bson);

    ASSERT_EQ(original.majorVersion, parsed.majorVersion);
    ASSERT_EQ(original.minorVersion, parsed.minorVersion);
    ASSERT_EQ(original.epoch, parsed.epoch);
}

// ============================================================================
// 高并发压力测试
// ============================================================================

TEST(ConfigQueryCoalescerTest, HighConcurrencyStress) {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(20);
    config.maxWaitersPerGroup = 500;
    config.adaptiveWindow = true;

    ConfigQueryCoalescer coalescer(config);

    OID epoch = OID::gen();

    std::atomic<int> queryCount{0};
    coalescer.setQueryExecutor([&](const std::string& queryNs,
                                   const ChunkVersionLight& sinceVersion) {
        queryCount++;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return makeChunks(queryNs, sinceVersion.majorVersion, 100, epoch);
    });

    // 模拟 100 个 mongos 并发请求
    const int numMongos = 100;
    const int collectionsCount = 5;
    std::atomic<int> successCount{0};
    std::atomic<int> failCount{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < numMongos; i++) {
        threads.emplace_back([&, i]() {
            std::string ns = "test.coll" + std::to_string(i % collectionsCount);
            ChunkVersionLight version(i % 10, 0, epoch);
            auto result = coalescer.getChunks(ns, version);
            if (result.isOK()) {
                successCount++;
            } else {
                failCount++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 绝大部分应该成功
    ASSERT_GTE(successCount.load(), numMongos * 0.95);

    auto stats = coalescer.getStats();

    std::cout << "\n=== 高并发压力测试结果 ===" << std::endl;
    std::cout << "总请求数: " << stats.totalRequests << std::endl;
    std::cout << "实际查询数: " << stats.actualQueries << std::endl;
    std::cout << "合并请求数: " << stats.coalescedRequests << std::endl;
    std::cout << "合并率: " << (stats.coalescingRate() * 100) << "%" << std::endl;
    std::cout << "查询节省率: " << (stats.querySavingRate() * 100) << "%" << std::endl;
    std::cout << "峰值等待: " << stats.peakWaitingRequests << std::endl;

    // 验证合并效果
    ASSERT_GT(stats.coalescingRate(), 0.7);  // 至少 70% 合并率
}

}  // namespace
}  // namespace mongo
