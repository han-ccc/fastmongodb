/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    Integration Stress Test - Disaster Recovery Scenario Simulation
 *    Tests rate limiter, batch query and config query coalescer under high concurrency
 */

#include "mongo/platform/basic.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/catalog/batch_query.h"
#include "mongo/s/catalog/config_query_coalescer.h"
#include "mongo/s/catalog/rate_limiter.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

// ============================================================================
// Mock Config Server
// ============================================================================

class MockConfigServer {
public:
    struct Config {
        Milliseconds baseLatency;
        Milliseconds latencyPerConcurrent;
        size_t maxConcurrent;
        double failureRateWhenOverloaded;

        Config()
            : baseLatency(Milliseconds(10)),
              latencyPerConcurrent(Milliseconds(5)),
              maxConcurrent(100),
              failureRateWhenOverloaded(0.3) {}
    };

    struct Stats {
        uint64_t totalRequests;
        uint64_t successRequests;
        uint64_t failedRequests;
        uint64_t peakConcurrent;
        uint64_t totalBytesTransferred;

        Stats()
            : totalRequests(0),
              successRequests(0),
              failedRequests(0),
              peakConcurrent(0),
              totalBytesTransferred(0) {}
    };

    explicit MockConfigServer(Config config = Config()) : _config(config) {
        resetStats();
    }

    StatusWith<size_t> getChunks(const std::string& ns, size_t numChunks = 1000) {
        return executeRequest([&]() { return numChunks * 200; });
    }

    Stats getStats() const {
        std::lock_guard<std::mutex> lock(_statsMutex);
        Stats s;
        s.totalRequests = _totalRequests;
        s.successRequests = _successRequests;
        s.failedRequests = _failedRequests;
        s.peakConcurrent = _peakConcurrent;
        s.totalBytesTransferred = _totalBytesTransferred;
        return s;
    }

    void resetStats() {
        std::lock_guard<std::mutex> lock(_statsMutex);
        _totalRequests = 0;
        _successRequests = 0;
        _failedRequests = 0;
        _currentConcurrent = 0;
        _peakConcurrent = 0;
        _totalBytesTransferred = 0;
    }

private:
    template<typename Func>
    StatusWith<size_t> executeRequest(Func&& func) {
        uint64_t concurrent;
        {
            std::lock_guard<std::mutex> lock(_statsMutex);
            _totalRequests++;
            concurrent = ++_currentConcurrent;
            if (concurrent > _peakConcurrent) {
                _peakConcurrent = concurrent;
            }
        }

        auto latency = _config.baseLatency +
            Milliseconds(_config.latencyPerConcurrent.count() * static_cast<long long>(concurrent));

        bool shouldFail = false;
        if (concurrent > _config.maxConcurrent) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_real_distribution<> dis(0.0, 1.0);
            shouldFail = dis(gen) < _config.failureRateWhenOverloaded;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(latency.count()));

        {
            std::lock_guard<std::mutex> lock(_statsMutex);
            _currentConcurrent--;
        }

        if (shouldFail) {
            std::lock_guard<std::mutex> lock(_statsMutex);
            _failedRequests++;
            return Status(ErrorCodes::ExceededTimeLimit, "Config server overloaded");
        }

        size_t bytes = func();
        {
            std::lock_guard<std::mutex> lock(_statsMutex);
            _successRequests++;
            _totalBytesTransferred += bytes;
        }
        return bytes;
    }

    Config _config;
    mutable std::mutex _statsMutex;
    uint64_t _totalRequests;
    uint64_t _successRequests;
    uint64_t _failedRequests;
    uint64_t _currentConcurrent;
    uint64_t _peakConcurrent;
    uint64_t _totalBytesTransferred;
};

struct TestResult {
    uint64_t totalRequests;
    uint64_t successRequests;
    uint64_t failedRequests;
    uint64_t peakConcurrent;
    uint64_t totalBytes;
    Milliseconds totalTime;
    Milliseconds p99Latency;
    double successRate;
};

// ============================================================================
// Baseline Test (No optimization)
// ============================================================================

TestResult runBaselineTest(MockConfigServer& configServer, size_t numClients, size_t requestsPerClient) {
    std::cout << "\n=== Baseline Test (No optimization) ===" << std::endl;
    configServer.resetStats();

    std::vector<Milliseconds> latencies;
    std::mutex latencyMutex;
    auto startTime = Date_t::now();

    std::vector<std::thread> clients;
    for (size_t i = 0; i < numClients; i++) {
        clients.emplace_back([&, i]() {
            for (size_t j = 0; j < requestsPerClient; j++) {
                auto reqStart = Date_t::now();
                configServer.getChunks("test.collection" + std::to_string(i % 10));
                auto reqEnd = Date_t::now();
                std::lock_guard<std::mutex> lock(latencyMutex);
                latencies.push_back(reqEnd - reqStart);
            }
        });
    }

    for (auto& client : clients) {
        client.join();
    }

    auto endTime = Date_t::now();
    auto stats = configServer.getStats();

    std::sort(latencies.begin(), latencies.end());
    Milliseconds p99 = latencies.empty() ? Milliseconds(0) : latencies[latencies.size() * 99 / 100];

    TestResult result;
    result.totalRequests = stats.totalRequests;
    result.successRequests = stats.successRequests;
    result.failedRequests = stats.failedRequests;
    result.peakConcurrent = stats.peakConcurrent;
    result.totalBytes = stats.totalBytesTransferred;
    result.totalTime = endTime - startTime;
    result.p99Latency = p99;
    result.successRate = stats.totalRequests > 0 ?
        static_cast<double>(stats.successRequests) / stats.totalRequests * 100 : 0;

    std::cout << "Requests: " << result.totalRequests << ", Peak concurrent: " << result.peakConcurrent
              << ", Success rate: " << result.successRate << "%, P99: " << p99.count() << "ms" << std::endl;
    return result;
}

// ============================================================================
// Rate Limiter Optimization Test
// ============================================================================

TestResult runWithRateLimiterTest(MockConfigServer& configServer, size_t numClients,
                                   size_t requestsPerClient, size_t maxConcurrent) {
    std::cout << "\n=== RateLimiter Test (max=" << maxConcurrent << ") ===" << std::endl;
    configServer.resetStats();

    RateLimiter rateLimiter(maxConcurrent);
    std::vector<Milliseconds> latencies;
    std::mutex latencyMutex;
    auto startTime = Date_t::now();

    std::vector<std::thread> clients;
    for (size_t i = 0; i < numClients; i++) {
        clients.emplace_back([&, i]() {
            for (size_t j = 0; j < requestsPerClient; j++) {
                auto reqStart = Date_t::now();
                auto guard = rateLimiter.tryAcquire(Milliseconds(30000));
                if (!guard) continue;
                configServer.getChunks("test.collection" + std::to_string(i % 10));
                auto reqEnd = Date_t::now();
                std::lock_guard<std::mutex> lock(latencyMutex);
                latencies.push_back(reqEnd - reqStart);
            }
        });
    }

    for (auto& client : clients) {
        client.join();
    }

    auto endTime = Date_t::now();
    auto stats = configServer.getStats();

    std::sort(latencies.begin(), latencies.end());
    Milliseconds p99 = latencies.empty() ? Milliseconds(0) : latencies[latencies.size() * 99 / 100];

    TestResult result;
    result.totalRequests = stats.totalRequests;
    result.successRequests = stats.successRequests;
    result.failedRequests = stats.failedRequests;
    result.peakConcurrent = stats.peakConcurrent;
    result.totalBytes = stats.totalBytesTransferred;
    result.totalTime = endTime - startTime;
    result.p99Latency = p99;
    result.successRate = stats.totalRequests > 0 ?
        static_cast<double>(stats.successRequests) / stats.totalRequests * 100 : 0;

    std::cout << "Requests: " << result.totalRequests << ", Peak concurrent: " << result.peakConcurrent
              << ", Success rate: " << result.successRate << "%, P99: " << p99.count() << "ms" << std::endl;
    return result;
}

// ============================================================================
// Config Query Coalescer Test
// ============================================================================

TestResult runWithCoalescerTest(MockConfigServer& configServer, size_t numClients,
                                 size_t requestsPerClient, size_t maxConcurrent) {
    std::cout << "\n=== Coalescer + RateLimiter Test (max=" << maxConcurrent << ") ===" << std::endl;
    configServer.resetStats();

    // Create coalescer with test configuration
    ConfigQueryCoalescer::Config coalescerConfig;
    coalescerConfig.coalescingWindow = Milliseconds(10);
    coalescerConfig.maxWaitTime = Milliseconds(200);
    coalescerConfig.maxWaitersPerGroup = 200;
    coalescerConfig.adaptiveWindow = true;

    ConfigQueryCoalescer coalescer(coalescerConfig);
    RateLimiter rateLimiter(maxConcurrent);

    // Set up query executor with rate limiting
    coalescer.setQueryExecutor(
        [&](const std::string& queryNs, const ChunkVersionLight& sinceVersion)
            -> StatusWith<std::vector<BSONObj>> {
            // Apply rate limiting
            auto guard = rateLimiter.tryAcquire(Milliseconds(30000));
            if (!guard) {
                return Status(ErrorCodes::ExceededTimeLimit, "Rate limit exceeded");
            }

            // Execute query on mock config server
            auto result = configServer.getChunks(queryNs);
            if (!result.isOK()) {
                return result.getStatus();
            }

            // Return mock chunk results
            std::vector<BSONObj> chunks;
            BSONObjBuilder builder;
            builder.append("ns", queryNs);
            builder.append("chunks", static_cast<long long>(result.getValue()));
            chunks.push_back(builder.obj());
            return chunks;
        });

    std::vector<Milliseconds> latencies;
    std::mutex latencyMutex;
    auto startTime = Date_t::now();

    std::vector<std::thread> clients;
    for (size_t i = 0; i < numClients; i++) {
        clients.emplace_back([&, i]() {
            std::string ns = "test.collection" + std::to_string(i % 10);

            for (size_t j = 0; j < requestsPerClient; j++) {
                auto reqStart = Date_t::now();

                // Use coalescer to get chunks
                ChunkVersionLight version;  // Default version (0,0)
                auto result = coalescer.getChunks(ns, version);

                auto reqEnd = Date_t::now();
                std::lock_guard<std::mutex> lock(latencyMutex);
                latencies.push_back(reqEnd - reqStart);
            }
        });
    }

    for (auto& client : clients) {
        client.join();
    }

    auto endTime = Date_t::now();
    auto stats = configServer.getStats();
    auto coalescerStats = coalescer.getStats();

    std::sort(latencies.begin(), latencies.end());
    Milliseconds p99 = latencies.empty() ? Milliseconds(0) : latencies[latencies.size() * 99 / 100];

    TestResult result;
    result.totalRequests = stats.totalRequests;
    result.successRequests = stats.successRequests;
    result.failedRequests = stats.failedRequests;
    result.peakConcurrent = stats.peakConcurrent;
    result.totalBytes = stats.totalBytesTransferred;
    result.totalTime = endTime - startTime;
    result.p99Latency = p99;
    result.successRate = stats.totalRequests > 0 ?
        static_cast<double>(stats.successRequests) / stats.totalRequests * 100 : 0;

    std::cout << "Requests: " << result.totalRequests << ", Peak concurrent: " << result.peakConcurrent
              << ", Success rate: " << result.successRate << "%, P99: " << p99.count() << "ms" << std::endl;
    std::cout << "Coalescer stats: totalRequests=" << coalescerStats.totalRequests
              << ", actualQueries=" << coalescerStats.actualQueries
              << ", coalescingRate=" << std::fixed << std::setprecision(1)
              << (coalescerStats.coalescingRate() * 100) << "%" << std::endl;

    return result;
}

// ============================================================================
// Main Test
// ============================================================================

TEST(IntegrationStressTest, DisasterRecoveryScenario) {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "    Disaster Recovery Scenario Stress Test" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    MockConfigServer::Config serverConfig;
    serverConfig.baseLatency = Milliseconds(10);
    serverConfig.latencyPerConcurrent = Milliseconds(2);
    serverConfig.maxConcurrent = 50;
    serverConfig.failureRateWhenOverloaded = 0.3;

    MockConfigServer configServer(serverConfig);

    const size_t numClients = 50;
    const size_t requestsPerClient = 3;

    auto baseline = runBaselineTest(configServer, numClients, requestsPerClient);
    auto withRateLimiter = runWithRateLimiterTest(configServer, numClients, requestsPerClient, 10);
    auto withCoalescer = runWithCoalescerTest(configServer, numClients, requestsPerClient, 10);

    // Output comparison
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "                    Test Results Comparison" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    std::cout << "\n| Metric | Baseline | RateLimiter | Coalescer |" << std::endl;
    std::cout << "|--------|----------|-------------|-----------|" << std::endl;
    std::cout << "| Requests | " << baseline.totalRequests
              << " | " << withRateLimiter.totalRequests
              << " | " << withCoalescer.totalRequests << " |" << std::endl;
    std::cout << "| Peak concurrent | " << baseline.peakConcurrent
              << " | " << withRateLimiter.peakConcurrent
              << " | " << withCoalescer.peakConcurrent << " |" << std::endl;
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "| Success rate | " << baseline.successRate << "%"
              << " | " << withRateLimiter.successRate << "%"
              << " | " << withCoalescer.successRate << "% |" << std::endl;
    std::cout << "| Transferred KB | " << baseline.totalBytes/1024
              << " | " << withRateLimiter.totalBytes/1024
              << " | " << withCoalescer.totalBytes/1024 << " |" << std::endl;

    // Optimization effects
    std::cout << "\nOptimization effects:" << std::endl;
    if (baseline.peakConcurrent > 0) {
        std::cout << "- RateLimiter peak concurrent reduction: "
                  << (1.0 - static_cast<double>(withRateLimiter.peakConcurrent) / baseline.peakConcurrent) * 100
                  << "%" << std::endl;
    }
    if (baseline.totalRequests > 0) {
        std::cout << "- Coalescer query reduction: "
                  << (1.0 - static_cast<double>(withCoalescer.totalRequests) / baseline.totalRequests) * 100
                  << "%" << std::endl;
    }

    // Assertions: RateLimiter should reduce peak concurrent
    ASSERT_LT(withRateLimiter.peakConcurrent, baseline.peakConcurrent);
    // Assertions: RateLimiter should maintain or improve success rate
    ASSERT_GTE(withRateLimiter.successRate, baseline.successRate);
    // Assertions: Coalescer should reduce actual queries significantly
    ASSERT_LT(withCoalescer.totalRequests, baseline.totalRequests);
}

}  // namespace
}  // namespace mongo
