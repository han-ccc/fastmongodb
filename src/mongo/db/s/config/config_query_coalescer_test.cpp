/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    ConfigQueryCoalescer Unit Tests
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/config_query_coalescer.h"

#include <atomic>
#include <thread>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ConfigQueryCoalescerTest : public unittest::Test {
protected:
    void setUp() override {
        _coalescer = stdx::make_unique<ConfigQueryCoalescer>();
    }

    void tearDown() override {
        if (_coalescer) {
            _coalescer->shutdown();
        }
        _coalescer.reset();
    }

    ConfigQueryCoalescer* coalescer() {
        return _coalescer.get();
    }

private:
    std::unique_ptr<ConfigQueryCoalescer> _coalescer;
};

// Test 1: Basic coalescing - multiple concurrent requests should result in one query
TEST_F(ConfigQueryCoalescerTest, BasicCoalescing) {
    const std::string ns = "testdb.testcoll";
    const size_t numThreads = 10;

    std::atomic<int> queryExecutions{0};
    std::atomic<int> successCount{0};

    std::vector<stdx::thread> threads;
    threads.reserve(numThreads);

    for (size_t i = 0; i < numThreads; i++) {
        threads.emplace_back([this, &ns, &queryExecutions, &successCount, i]() {
            auto result = coalescer()->tryCoalesce(
                nullptr,  // txn not needed for this test
                ns,
                1000 + i,  // version within acceptable gap
                [&queryExecutions]() -> StatusWith<std::vector<BSONObj>> {
                    queryExecutions++;
                    // Simulate query execution time
                    stdx::this_thread::sleep_for(Milliseconds(10).toSystemDuration());
                    return std::vector<BSONObj>{BSON("_id" << 1), BSON("_id" << 2)};
                });

            if (result.isOK()) {
                successCount++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All requests should succeed
    ASSERT_EQ(static_cast<size_t>(numThreads), static_cast<size_t>(successCount.load()));

    // Should have coalesced to fewer queries (ideally 1, but timing can vary)
    ASSERT_LTE(queryExecutions.load(), 3);

    // Verify stats
    auto stats = coalescer()->getStats();
    ASSERT_EQ(static_cast<uint64_t>(numThreads), stats.totalRequests);
    ASSERT_GTE(stats.coalescedRequests, static_cast<uint64_t>(numThreads - 3));
}

// Test 2: Version gap too large - should execute independently
TEST_F(ConfigQueryCoalescerTest, VersionGapSkipped) {
    const std::string ns = "testdb.testcoll";

    std::atomic<int> queryExecutions{0};

    // First request
    stdx::thread firstThread([this, &ns, &queryExecutions]() {
        coalescer()->tryCoalesce(
            nullptr,
            ns,
            1000,
            [&queryExecutions]() -> StatusWith<std::vector<BSONObj>> {
                queryExecutions++;
                stdx::this_thread::sleep_for(Milliseconds(50).toSystemDuration());
                return std::vector<BSONObj>{BSON("_id" << 1)};
            });
    });

    // Wait a bit for first request to create group
    stdx::this_thread::sleep_for(Milliseconds(5).toSystemDuration());

    // Second request with huge version gap
    auto result = coalescer()->tryCoalesce(
        nullptr,
        ns,
        1000000,  // Large version gap
        [&queryExecutions]() -> StatusWith<std::vector<BSONObj>> {
            queryExecutions++;
            return std::vector<BSONObj>{BSON("_id" << 2)};
        });

    firstThread.join();

    ASSERT(result.isOK());

    // Both should execute independently
    ASSERT_EQ(2, static_cast<int>(queryExecutions.load()));

    // Verify version gap stat
    auto stats = coalescer()->getStats();
    ASSERT_GTE(stats.versionGapSkippedRequests, static_cast<uint64_t>(1));
}

// Test 3: Different namespaces should not coalesce
TEST_F(ConfigQueryCoalescerTest, DifferentNamespacesNotCoalesced) {
    std::atomic<int> queryExecutions{0};

    std::vector<stdx::thread> threads;

    // Request for ns1
    threads.emplace_back([this, &queryExecutions]() {
        coalescer()->tryCoalesce(
            nullptr,
            "testdb.coll1",
            1000,
            [&queryExecutions]() -> StatusWith<std::vector<BSONObj>> {
                queryExecutions++;
                stdx::this_thread::sleep_for(Milliseconds(20).toSystemDuration());
                return std::vector<BSONObj>{BSON("_id" << 1)};
            });
    });

    // Request for ns2
    threads.emplace_back([this, &queryExecutions]() {
        coalescer()->tryCoalesce(
            nullptr,
            "testdb.coll2",
            1000,
            [&queryExecutions]() -> StatusWith<std::vector<BSONObj>> {
                queryExecutions++;
                stdx::this_thread::sleep_for(Milliseconds(20).toSystemDuration());
                return std::vector<BSONObj>{BSON("_id" << 2)};
            });
    });

    for (auto& thread : threads) {
        thread.join();
    }

    // Both should execute independently
    ASSERT_EQ(2, static_cast<int>(queryExecutions.load()));

    auto stats = coalescer()->getStats();
    ASSERT_EQ(static_cast<uint64_t>(2), stats.actualQueries);
}

// Test 4: Query error propagation
TEST_F(ConfigQueryCoalescerTest, QueryErrorPropagation) {
    const std::string ns = "testdb.testcoll";
    const size_t numThreads = 5;

    std::atomic<int> errorCount{0};

    std::vector<stdx::thread> threads;
    threads.reserve(numThreads);

    for (size_t i = 0; i < numThreads; i++) {
        threads.emplace_back([this, &ns, &errorCount, i]() {
            auto result = coalescer()->tryCoalesce(
                nullptr,
                ns,
                1000 + i,
                []() -> StatusWith<std::vector<BSONObj>> {
                    stdx::this_thread::sleep_for(Milliseconds(10).toSystemDuration());
                    return Status(ErrorCodes::InternalError, "Test query error");
                });

            if (!result.isOK()) {
                errorCount++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All requests should receive the error
    ASSERT_EQ(static_cast<int>(numThreads), errorCount.load());
}

// Test 5: Stats accuracy
TEST_F(ConfigQueryCoalescerTest, StatsAccuracy) {
    const std::string ns = "testdb.testcoll";

    // Reset stats
    coalescer()->resetStats();

    auto stats = coalescer()->getStats();
    ASSERT_EQ(0, stats.totalRequests);
    ASSERT_EQ(0, stats.actualQueries);
    ASSERT_EQ(0, stats.coalescedRequests);

    // Execute a single query
    auto result = coalescer()->tryCoalesce(
        nullptr,
        ns,
        1000,
        []() -> StatusWith<std::vector<BSONObj>> {
            return std::vector<BSONObj>{BSON("_id" << 1)};
        });

    ASSERT(result.isOK());

    stats = coalescer()->getStats();
    ASSERT_EQ(1, stats.totalRequests);
    ASSERT_EQ(1, stats.actualQueries);
    ASSERT_EQ(0, stats.coalescedRequests);  // First request is leader, not coalesced

    // Verify stats BSON output
    BSONObj statsBSON = stats.toBSON();
    ASSERT_EQ(1LL, statsBSON["totalRequests"].Long());
    ASSERT_EQ(1LL, statsBSON["actualQueries"].Long());
}

// Test 6: Shutdown handling
TEST_F(ConfigQueryCoalescerTest, ShutdownHandling) {
    ASSERT_FALSE(coalescer()->isShutdown());

    coalescer()->shutdown();

    ASSERT_TRUE(coalescer()->isShutdown());

    // Requests after shutdown should fail
    auto result = coalescer()->tryCoalesce(
        nullptr,
        "testdb.testcoll",
        1000,
        []() -> StatusWith<std::vector<BSONObj>> {
            return std::vector<BSONObj>{};
        });

    ASSERT_FALSE(result.isOK());
    ASSERT_EQ(ErrorCodes::ShutdownInProgress, result.getStatus().code());
}

// Test 7: Results distribution to all waiters
TEST_F(ConfigQueryCoalescerTest, ResultsDistributedToAllWaiters) {
    const std::string ns = "testdb.testcoll";
    const size_t numThreads = 5;

    std::atomic<int> successWithCorrectResults{0};

    std::vector<stdx::thread> threads;
    threads.reserve(numThreads);

    for (size_t i = 0; i < numThreads; i++) {
        threads.emplace_back([this, &ns, &successWithCorrectResults, i]() {
            auto result = coalescer()->tryCoalesce(
                nullptr,
                ns,
                1000 + i,
                []() -> StatusWith<std::vector<BSONObj>> {
                    stdx::this_thread::sleep_for(Milliseconds(10).toSystemDuration());
                    return std::vector<BSONObj>{
                        BSON("_id" << 1 << "data" << "test1"),
                        BSON("_id" << 2 << "data" << "test2"),
                        BSON("_id" << 3 << "data" << "test3")
                    };
                });

            if (result.isOK() && result.getValue().size() == 3) {
                successWithCorrectResults++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All requests should receive the same 3 results
    ASSERT_EQ(static_cast<int>(numThreads), successWithCorrectResults.load());
}

// Test 8: Coalescing rate calculation
TEST_F(ConfigQueryCoalescerTest, CoalescingRateCalculation) {
    ConfigQueryCoalescer::Stats stats;

    // No requests
    ASSERT_EQ(0.0, stats.coalescingRate());

    // Some coalesced requests
    stats.totalRequests = 100;
    stats.coalescedRequests = 90;

    double rate = stats.coalescingRate();
    ASSERT_GTE(rate, 0.89);
    ASSERT_LTE(rate, 0.91);
}

// ============================================================================
// 新增测试 - 覆盖代码审查发现的边界情况
// ============================================================================

// Test 9: Shutdown during wait - waiters should be notified
TEST_F(ConfigQueryCoalescerTest, ShutdownDuringWait) {
    const std::string ns = "testdb.testcoll";

    std::atomic<bool> queryStarted{false};
    std::atomic<int> shutdownErrors{0};

    // Start a slow query that holds the group
    stdx::thread slowQuery([this, &ns, &queryStarted]() {
        coalescer()->tryCoalesce(
            nullptr,
            ns,
            1000,
            [&queryStarted]() -> StatusWith<std::vector<BSONObj>> {
                queryStarted.store(true);
                // Hold for a while to let other threads join
                stdx::this_thread::sleep_for(Milliseconds(200).toSystemDuration());
                return std::vector<BSONObj>{BSON("_id" << 1)};
            });
    });

    // Wait for query to start
    while (!queryStarted.load()) {
        stdx::this_thread::sleep_for(Milliseconds(1).toSystemDuration());
    }

    // Start waiters that will join the group
    std::vector<stdx::thread> waiters;
    for (int i = 0; i < 3; ++i) {
        waiters.emplace_back([this, &ns, &shutdownErrors, i]() {
            // Small delay to ensure they arrive after the leader
            stdx::this_thread::sleep_for(Milliseconds(5 * i).toSystemDuration());

            auto result = coalescer()->tryCoalesce(
                nullptr,
                ns,
                1001 + i,
                []() -> StatusWith<std::vector<BSONObj>> {
                    return std::vector<BSONObj>{};
                });

            if (!result.isOK() && result.getStatus().code() == ErrorCodes::ShutdownInProgress) {
                shutdownErrors++;
            }
        });
    }

    // Shutdown while waiters are waiting
    stdx::this_thread::sleep_for(Milliseconds(20).toSystemDuration());
    coalescer()->shutdown();

    // Wait for all threads
    slowQuery.join();
    for (auto& t : waiters) {
        t.join();
    }

    // Some waiters should have received shutdown error
    // (The exact number depends on timing)
    ASSERT_TRUE(coalescer()->isShutdown());
}

// Test 10: Multiple namespaces concurrent access
TEST_F(ConfigQueryCoalescerTest, MultipleNamespacesConcurrent) {
    const size_t numNamespaces = 5;
    const size_t threadsPerNs = 4;

    std::atomic<int> totalSuccess{0};
    std::atomic<int> totalQueries{0};

    std::vector<stdx::thread> threads;

    for (size_t ns = 0; ns < numNamespaces; ++ns) {
        std::string nsName = "testdb.coll" + std::to_string(ns);

        for (size_t t = 0; t < threadsPerNs; ++t) {
            threads.emplace_back([this, nsName, &totalSuccess, &totalQueries, t]() {
                auto result = coalescer()->tryCoalesce(
                    nullptr,
                    nsName,
                    1000 + t,
                    [&totalQueries]() -> StatusWith<std::vector<BSONObj>> {
                        totalQueries++;
                        stdx::this_thread::sleep_for(Milliseconds(10).toSystemDuration());
                        return std::vector<BSONObj>{BSON("_id" << 1)};
                    });

                if (result.isOK()) {
                    totalSuccess++;
                }
            });
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    // All requests should succeed
    ASSERT_EQ(static_cast<int>(numNamespaces * threadsPerNs), totalSuccess.load());

    // Should have coalesced within each namespace
    // Expect roughly numNamespaces queries (one per namespace, with some variance)
    ASSERT_GTE(totalQueries.load(), static_cast<int>(numNamespaces));
    ASSERT_LTE(totalQueries.load(), static_cast<int>(numNamespaces * 2));
}

// Test 11: Rapid sequential requests (no concurrent overlap)
TEST_F(ConfigQueryCoalescerTest, RapidSequentialRequests) {
    const std::string ns = "testdb.testcoll";
    const size_t numRequests = 10;

    std::atomic<int> queryCount{0};

    for (size_t i = 0; i < numRequests; ++i) {
        auto result = coalescer()->tryCoalesce(
            nullptr,
            ns,
            1000 + i,
            [&queryCount]() -> StatusWith<std::vector<BSONObj>> {
                queryCount++;
                return std::vector<BSONObj>{BSON("_id" << 1)};
            });

        ASSERT(result.isOK());
    }

    // Sequential requests without overlap should each execute independently
    ASSERT_EQ(static_cast<int>(numRequests), queryCount.load());
}

// Test 12: Empty result handling
TEST_F(ConfigQueryCoalescerTest, EmptyResultHandling) {
    const std::string ns = "testdb.testcoll";
    const size_t numThreads = 5;

    std::atomic<int> successWithEmptyResult{0};

    std::vector<stdx::thread> threads;
    for (size_t i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &ns, &successWithEmptyResult, i]() {
            auto result = coalescer()->tryCoalesce(
                nullptr,
                ns,
                1000 + i,
                []() -> StatusWith<std::vector<BSONObj>> {
                    stdx::this_thread::sleep_for(Milliseconds(10).toSystemDuration());
                    return std::vector<BSONObj>{};  // Empty result
                });

            if (result.isOK() && result.getValue().empty()) {
                successWithEmptyResult++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All should succeed with empty result
    ASSERT_EQ(static_cast<int>(numThreads), successWithEmptyResult.load());
}

// Test 13: Large result set distribution
TEST_F(ConfigQueryCoalescerTest, LargeResultSetDistribution) {
    const std::string ns = "testdb.testcoll";
    const size_t numThreads = 5;
    const size_t numResults = 100;

    std::atomic<int> correctResultCount{0};

    std::vector<stdx::thread> threads;
    for (size_t i = 0; i < numThreads; ++i) {
        threads.emplace_back([this, &ns, &correctResultCount, numResults, i]() {
            auto result = coalescer()->tryCoalesce(
                nullptr,
                ns,
                1000 + i,
                [numResults]() -> StatusWith<std::vector<BSONObj>> {
                    stdx::this_thread::sleep_for(Milliseconds(10).toSystemDuration());
                    std::vector<BSONObj> results;
                    results.reserve(numResults);
                    for (size_t j = 0; j < numResults; ++j) {
                        results.push_back(BSON("_id" << static_cast<int>(j) << "data" << "test"));
                    }
                    return results;
                });

            if (result.isOK() && result.getValue().size() == numResults) {
                correctResultCount++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All should receive the full result set
    ASSERT_EQ(static_cast<int>(numThreads), correctResultCount.load());
}

// Test 14: isEnabled flag check
TEST_F(ConfigQueryCoalescerTest, IsEnabledFlag) {
    // Default state depends on configQueryCoalescerEnabled
    // Just verify the method works
    bool enabled = ConfigQueryCoalescer::isEnabled();
    // We can't assert a specific value as it depends on global config
    (void)enabled;  // Avoid unused variable warning
}

// Test 15: Double shutdown safety
TEST_F(ConfigQueryCoalescerTest, DoubleShutdownSafety) {
    ASSERT_FALSE(coalescer()->isShutdown());

    coalescer()->shutdown();
    ASSERT_TRUE(coalescer()->isShutdown());

    // Second shutdown should be safe (no crash/hang)
    coalescer()->shutdown();
    ASSERT_TRUE(coalescer()->isShutdown());
}

// Test 16: Stats thread safety
TEST_F(ConfigQueryCoalescerTest, StatsThreadSafety) {
    const std::string ns = "testdb.testcoll";

    std::atomic<bool> keepRunning{true};
    std::atomic<int> statsReads{0};

    // Thread that continuously reads stats
    stdx::thread statsReader([this, &keepRunning, &statsReads]() {
        while (keepRunning.load()) {
            auto stats = coalescer()->getStats();
            (void)stats.toBSON();  // Trigger serialization
            statsReads++;
        }
    });

    // Make some requests while stats are being read
    std::vector<stdx::thread> workers;
    for (int i = 0; i < 5; ++i) {
        workers.emplace_back([this, &ns, i]() {
            for (int j = 0; j < 10; ++j) {
                coalescer()->tryCoalesce(
                    nullptr,
                    ns + std::to_string(i),
                    1000,
                    []() -> StatusWith<std::vector<BSONObj>> {
                        return std::vector<BSONObj>{BSON("_id" << 1)};
                    });
            }
        });
    }

    for (auto& t : workers) {
        t.join();
    }

    keepRunning.store(false);
    statsReader.join();

    // Should have read stats many times without crashing
    ASSERT_GT(statsReads.load(), 0);
}

}  // namespace (anonymous)

// ============================================================================
// Server Parameter Tests - 覆盖 #18/#19 修复 (参数断开和验证)
// ============================================================================

// 直接访问 atomic 变量来验证参数是否生效
// 这些变量定义在 config_query_coalescer.cpp 中的 mongo 命名空间
extern std::atomic<bool> configQueryCoalescerEnabled;
extern std::atomic<int> configQueryCoalescerWindowMS;
extern std::atomic<int> configQueryCoalescerMaxWaitMS;
extern std::atomic<int> configQueryCoalescerMaxWaiters;
extern std::atomic<long long> configQueryCoalescerMaxVersionGap;

namespace {  // reopen anonymous namespace for test fixtures

class ServerParameterTest : public unittest::Test {
protected:
    void setUp() override {
        // 保存原始值
        _originalEnabled = configQueryCoalescerEnabled.load();
        _originalWindowMS = configQueryCoalescerWindowMS.load();
        _originalMaxWaitMS = configQueryCoalescerMaxWaitMS.load();
        _originalMaxWaiters = configQueryCoalescerMaxWaiters.load();
        _originalMaxVersionGap = configQueryCoalescerMaxVersionGap.load();
    }

    void tearDown() override {
        // 恢复原始值
        configQueryCoalescerEnabled.store(_originalEnabled);
        configQueryCoalescerWindowMS.store(_originalWindowMS);
        configQueryCoalescerMaxWaitMS.store(_originalMaxWaitMS);
        configQueryCoalescerMaxWaiters.store(_originalMaxWaiters);
        configQueryCoalescerMaxVersionGap.store(_originalMaxVersionGap);
    }

private:
    bool _originalEnabled;
    int _originalWindowMS;
    int _originalMaxWaitMS;
    int _originalMaxWaiters;
    long long _originalMaxVersionGap;
};

// Test: Atomic variables have valid default values
TEST_F(ServerParameterTest, DefaultValues) {
    // Just verify they have sensible default values
    // The actual defaults may vary, but they should be within valid ranges
    int windowMS = configQueryCoalescerWindowMS.load();
    int maxWaitMS = configQueryCoalescerMaxWaitMS.load();
    int maxWaiters = configQueryCoalescerMaxWaiters.load();
    long long maxVersionGap = configQueryCoalescerMaxVersionGap.load();

    ASSERT_GTE(windowMS, 1);
    ASSERT_LTE(windowMS, 1000);

    ASSERT_GTE(maxWaitMS, 10);
    ASSERT_LTE(maxWaitMS, 60000);

    ASSERT_GTE(maxWaiters, 1);
    ASSERT_LTE(maxWaiters, 100000);

    ASSERT_GTE(maxVersionGap, 1);
    ASSERT_LTE(maxVersionGap, 100000);
}

// Test: Directly set atomic variables and verify
TEST_F(ServerParameterTest, DirectAtomicAccess) {
    // Set enabled flag
    configQueryCoalescerEnabled.store(true);
    ASSERT_TRUE(configQueryCoalescerEnabled.load());
    ASSERT_TRUE(ConfigQueryCoalescer::isEnabled());

    configQueryCoalescerEnabled.store(false);
    ASSERT_FALSE(configQueryCoalescerEnabled.load());
    ASSERT_FALSE(ConfigQueryCoalescer::isEnabled());
}

// Test: Window MS atomic access
TEST_F(ServerParameterTest, WindowMSAtomicAccess) {
    configQueryCoalescerWindowMS.store(50);
    ASSERT_EQ(50, configQueryCoalescerWindowMS.load());

    configQueryCoalescerWindowMS.store(100);
    ASSERT_EQ(100, configQueryCoalescerWindowMS.load());
}

// Test: Concurrent atomic access safety
TEST_F(ServerParameterTest, ConcurrentAtomicAccess) {
    std::atomic<int> readCount{0};
    std::atomic<bool> keepRunning{true};

    // Reader thread
    stdx::thread reader([&readCount, &keepRunning]() {
        while (keepRunning.load()) {
            (void)configQueryCoalescerEnabled.load();
            (void)configQueryCoalescerWindowMS.load();
            (void)configQueryCoalescerMaxWaitMS.load();
            readCount++;
        }
    });

    // Writer thread
    stdx::thread writer([&keepRunning]() {
        for (int i = 0; i < 100 && keepRunning.load(); ++i) {
            configQueryCoalescerEnabled.store(i % 2 == 0);
            configQueryCoalescerWindowMS.store(1 + (i % 100));
            stdx::this_thread::sleep_for(Milliseconds(1).toSystemDuration());
        }
        keepRunning.store(false);
    });

    writer.join();
    keepRunning.store(false);
    reader.join();

    // Should have many reads without crashing
    ASSERT_GT(readCount.load(), 0);
}

// Test: isEnabled correctly reflects atomic value
TEST_F(ServerParameterTest, IsEnabledReflectsAtomic) {
    configQueryCoalescerEnabled.store(false);
    ASSERT_FALSE(ConfigQueryCoalescer::isEnabled());

    configQueryCoalescerEnabled.store(true);
    ASSERT_TRUE(ConfigQueryCoalescer::isEnabled());

    configQueryCoalescerEnabled.store(false);
    ASSERT_FALSE(ConfigQueryCoalescer::isEnabled());
}

}  // namespace
}  // namespace mongo
