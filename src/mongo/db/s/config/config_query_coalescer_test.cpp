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

}  // namespace
}  // namespace mongo
