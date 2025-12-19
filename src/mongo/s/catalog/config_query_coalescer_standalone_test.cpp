/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    Config Query Coalescer Standalone Test
 *
 *    独立测试程序，不依赖 MongoDB unittest 框架
 *    用于验证请求合并器的核心功能
 *
 *    编译: g++ -std=c++11 -pthread -I src -I build/opt -I src/third_party/boost-1.60.0 \
 *          -Wno-deprecated-declarations -o coalescer_test \
 *          src/mongo/s/catalog/config_query_coalescer_standalone_test.cpp \
 *          -DSTANDALONE_TEST
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// C++11 doesn't have make_unique, provide our own
template<typename T, typename... Args>
std::unique_ptr<T> make_unique_helper(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// ============================================================================
// 简化版实现 (不依赖 MongoDB 类型)
// ============================================================================

using Milliseconds = std::chrono::milliseconds;
using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

struct ChunkVersionLight {
    uint32_t majorVersion;
    uint32_t minorVersion;

    ChunkVersionLight() : majorVersion(0), minorVersion(0) {}
    ChunkVersionLight(uint32_t major, uint32_t minor)
        : majorVersion(major), minorVersion(minor) {}

    bool operator<(const ChunkVersionLight& other) const {
        if (majorVersion != other.majorVersion) {
            return majorVersion < other.majorVersion;
        }
        return minorVersion < other.minorVersion;
    }

    bool operator>=(const ChunkVersionLight& other) const {
        return !(*this < other);
    }
};

struct ChunkData {
    std::string ns;
    uint32_t version;
    std::string data;

    ChunkData(const std::string& n, uint32_t v, const std::string& d)
        : ns(n), version(v), data(d) {}
};

class ConfigQueryCoalescer {
public:
    struct Config {
        Milliseconds coalescingWindow{5};
        Milliseconds maxWaitTime{100};
        size_t maxWaitersPerGroup{1000};

        Config() {}
        Config(Milliseconds window, Milliseconds maxWait, size_t maxWaiters)
            : coalescingWindow(window), maxWaitTime(maxWait), maxWaitersPerGroup(maxWaiters) {}
    };

    struct Stats {
        std::atomic<uint64_t> totalRequests{0};
        std::atomic<uint64_t> actualQueries{0};
        std::atomic<uint64_t> coalescedRequests{0};
        std::atomic<uint64_t> timeoutRequests{0};

        double coalescingRate() const {
            uint64_t total = totalRequests.load();
            return total > 0 ? static_cast<double>(coalescedRequests.load()) / total : 0.0;
        }

        double querySavingRate() const {
            uint64_t total = totalRequests.load();
            return total > 0 ? 1.0 - static_cast<double>(actualQueries.load()) / total : 0.0;
        }
    };

    using QueryExecutor = std::function<std::vector<ChunkData>(
        const std::string& ns,
        const ChunkVersionLight& sinceVersion)>;

    explicit ConfigQueryCoalescer(Config config = Config())
        : _config(std::move(config)), _shutdown(false) {}

    ~ConfigQueryCoalescer() {
        shutdown();
    }

    void setQueryExecutor(QueryExecutor executor) {
        std::lock_guard<std::mutex> lock(_mutex);
        _queryExecutor = std::move(executor);
    }

    std::vector<ChunkData> getChunks(const std::string& ns,
                                      const ChunkVersionLight& sinceVersion) {
        if (_shutdown) {
            return {};
        }

        _stats.totalRequests++;

        std::vector<ChunkData> result;
        bool isLeader = false;

        std::unique_lock<std::mutex> lock(_mutex);

        auto it = _groups.find(ns);

        // No existing group, create one
        if (it == _groups.end()) {
            auto group = make_unique_helper<CoalescingGroup>(ns);
            group->minVersion = sinceVersion;
            group->windowEnd = SteadyClock::now() + _config.coalescingWindow;

            group->waiters.emplace_back(sinceVersion, &result);
            _groups[ns] = std::move(group);
            isLeader = true;

            // Wait for coalescing window
            lock.unlock();
            std::this_thread::sleep_for(_config.coalescingWindow);
            lock.lock();

            // Execute query if still leader
            it = _groups.find(ns);
            if (it != _groups.end() && !it->second->queryInProgress) {
                it->second->queryInProgress = true;
                ChunkVersionLight minVer = it->second->minVersion;
                lock.unlock();

                // Execute actual query
                std::vector<ChunkData> queryResult;
                if (_queryExecutor) {
                    queryResult = _queryExecutor(ns, minVer);
                }
                _stats.actualQueries++;

                // Distribute results
                lock.lock();
                it = _groups.find(ns);
                if (it != _groups.end()) {
                    it->second->queryResult = std::move(queryResult);
                    it->second->queryCompleted = true;

                    // Filter and distribute to all waiters
                    for (auto& waiter : it->second->waiters) {
                        for (const auto& chunk : it->second->queryResult) {
                            ChunkVersionLight chunkVer(chunk.version, 0);
                            if (chunkVer >= waiter.requestedVersion) {
                                waiter.resultPtr->push_back(chunk);
                            }
                        }
                        waiter.done = true;
                    }

                    _groups.erase(it);
                }
                _cv.notify_all();
            }

            return result;
        }

        // Existing group, join as waiter
        CoalescingGroup* group = it->second.get();

        if (group->waiters.size() >= _config.maxWaitersPerGroup) {
            // Overflow, execute directly
            lock.unlock();
            _stats.actualQueries++;
            if (_queryExecutor) {
                return _queryExecutor(ns, sinceVersion);
            }
            return {};
        }

        // Update min version if needed
        if (sinceVersion < group->minVersion) {
            group->minVersion = sinceVersion;
        }

        group->waiters.emplace_back(sinceVersion, &result);
        _stats.coalescedRequests++;

        // Wait for results
        bool timedOut = !_cv.wait_for(lock, _config.maxWaitTime, [this, &ns, &result]() {
            if (_shutdown) return true;
            auto git = _groups.find(ns);
            if (git == _groups.end()) return true;
            for (auto& w : git->second->waiters) {
                if (w.resultPtr == &result && w.done) {
                    return true;
                }
            }
            return git->second->queryCompleted;
        });

        if (timedOut) {
            _stats.timeoutRequests++;
        }

        return result;
    }

    const Stats& stats() const { return _stats; }

    void shutdown() {
        std::lock_guard<std::mutex> lock(_mutex);
        _shutdown = true;
        _groups.clear();
        _cv.notify_all();
    }

private:
    struct Waiter {
        ChunkVersionLight requestedVersion;
        std::vector<ChunkData>* resultPtr;
        bool done{false};

        Waiter(const ChunkVersionLight& ver, std::vector<ChunkData>* res)
            : requestedVersion(ver), resultPtr(res) {}
    };

    struct CoalescingGroup {
        std::string ns;
        ChunkVersionLight minVersion;
        TimePoint windowEnd;
        bool queryInProgress{false};
        bool queryCompleted{false};
        std::vector<ChunkData> queryResult;
        std::list<Waiter> waiters;

        explicit CoalescingGroup(const std::string& n) : ns(n) {}
    };

    Config _config;
    QueryExecutor _queryExecutor;

    mutable std::mutex _mutex;
    std::condition_variable _cv;

    std::map<std::string, std::unique_ptr<CoalescingGroup>> _groups;
    Stats _stats;
    bool _shutdown;
};

// ============================================================================
// Mock Config Server
// ============================================================================

class MockConfigServer {
public:
    struct Stats {
        std::atomic<uint64_t> totalQueries{0};
        std::atomic<uint64_t> peakConcurrent{0};
        std::atomic<uint64_t> currentConcurrent{0};
    };

    MockConfigServer(Milliseconds latency = Milliseconds(5))
        : _latency(latency) {}

    std::vector<ChunkData> getChunks(const std::string& ns,
                                      const ChunkVersionLight& sinceVersion) {
        _stats.totalQueries++;

        uint64_t concurrent = ++_stats.currentConcurrent;
        uint64_t peak = _stats.peakConcurrent.load();
        while (concurrent > peak) {
            _stats.peakConcurrent.compare_exchange_weak(peak, concurrent);
            peak = _stats.peakConcurrent.load();
        }

        // Simulate query latency
        std::this_thread::sleep_for(_latency);

        --_stats.currentConcurrent;

        // Generate mock chunks
        std::vector<ChunkData> chunks;
        for (uint32_t i = sinceVersion.majorVersion; i < sinceVersion.majorVersion + 100; i++) {
            chunks.emplace_back(ns, i, "chunk_" + std::to_string(i));
        }
        return chunks;
    }

    const Stats& stats() const { return _stats; }

    void reset() {
        _stats.totalQueries = 0;
        _stats.peakConcurrent = 0;
        _stats.currentConcurrent = 0;
    }

private:
    Milliseconds _latency;
    Stats _stats;
};

// ============================================================================
// Test Framework
// ============================================================================

class TestRunner {
public:
    void run(const std::string& name, std::function<bool()> test) {
        std::cout << "Running: " << name << "... " << std::flush;
        try {
            if (test()) {
                std::cout << "PASSED" << std::endl;
                _passed++;
            } else {
                std::cout << "FAILED" << std::endl;
                _failed++;
            }
        } catch (const std::exception& e) {
            std::cout << "EXCEPTION: " << e.what() << std::endl;
            _failed++;
        }
        _total++;
    }

    void summary() {
        std::cout << "\n========================================" << std::endl;
        std::cout << "Total: " << _total << ", Passed: " << _passed
                  << ", Failed: " << _failed << std::endl;
        std::cout << "========================================" << std::endl;
    }

    int exitCode() const { return _failed > 0 ? 1 : 0; }

private:
    int _total = 0;
    int _passed = 0;
    int _failed = 0;
};

#define ASSERT(cond) if (!(cond)) { std::cerr << "Assertion failed: " #cond << std::endl; return false; }
#define ASSERT_EQ(a, b) if ((a) != (b)) { std::cerr << "Assertion failed: " #a " == " #b << " (" << (a) << " != " << (b) << ")" << std::endl; return false; }
#define ASSERT_GT(a, b) if (!((a) > (b))) { std::cerr << "Assertion failed: " #a " > " #b << std::endl; return false; }
#define ASSERT_LT(a, b) if (!((a) < (b))) { std::cerr << "Assertion failed: " #a " < " #b << std::endl; return false; }
#define ASSERT_GTE(a, b) if (!((a) >= (b))) { std::cerr << "Assertion failed: " #a " >= " #b << std::endl; return false; }

// ============================================================================
// Tests
// ============================================================================

bool testBasicSingleRequest() {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(10);

    ConfigQueryCoalescer coalescer(config);
    MockConfigServer server;

    coalescer.setQueryExecutor([&](const std::string& ns,
                                   const ChunkVersionLight& ver) {
        return server.getChunks(ns, ver);
    });

    ChunkVersionLight version(0, 0);
    auto result = coalescer.getChunks("test.collection", version);

    ASSERT_EQ(result.size(), 100u);
    ASSERT_EQ(server.stats().totalQueries.load(), 1u);
    ASSERT_EQ(coalescer.stats().totalRequests.load(), 1u);

    return true;
}

bool testMultipleRequestsCoalescing() {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(50);

    ConfigQueryCoalescer coalescer(config);
    MockConfigServer server;

    coalescer.setQueryExecutor([&](const std::string& ns,
                                   const ChunkVersionLight& ver) {
        return server.getChunks(ns, ver);
    });

    const int numRequests = 10;
    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < numRequests; i++) {
        threads.emplace_back([&, i]() {
            ChunkVersionLight version(i, 0);
            auto result = coalescer.getChunks("test.collection", version);
            if (!result.empty()) {
                successCount++;
            }
        });
        std::this_thread::sleep_for(Milliseconds(3));
    }

    for (auto& t : threads) {
        t.join();
    }

    ASSERT_EQ(successCount.load(), numRequests);
    // Should have significantly fewer actual queries than requests
    ASSERT_LT(server.stats().totalQueries.load(), static_cast<uint64_t>(numRequests));
    ASSERT_GT(coalescer.stats().coalescingRate(), 0.3);

    std::cout << "\n  Coalescing rate: " << (coalescer.stats().coalescingRate() * 100) << "%" << std::endl;
    std::cout << "  Actual queries: " << server.stats().totalQueries.load()
              << " / " << numRequests << std::endl;

    return true;
}

bool testDifferentNamespaces() {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(30);

    ConfigQueryCoalescer coalescer(config);
    MockConfigServer server;

    coalescer.setQueryExecutor([&](const std::string& ns,
                                   const ChunkVersionLight& ver) {
        return server.getChunks(ns, ver);
    });

    std::vector<std::vector<ChunkData>> results(2);
    std::thread t1([&]() {
        ChunkVersionLight version(0, 0);
        results[0] = coalescer.getChunks("test.coll1", version);
    });

    std::thread t2([&]() {
        ChunkVersionLight version(0, 0);
        results[1] = coalescer.getChunks("test.coll2", version);
    });

    t1.join();
    t2.join();

    ASSERT(!results[0].empty());
    ASSERT(!results[1].empty());
    ASSERT_EQ(server.stats().totalQueries.load(), 2u);

    return true;
}

bool testVersionFiltering() {
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(50);

    ConfigQueryCoalescer coalescer(config);

    coalescer.setQueryExecutor([](const std::string& ns,
                                  const ChunkVersionLight& ver) {
        // Return chunks with version 0-99
        std::vector<ChunkData> chunks;
        for (uint32_t i = 0; i < 100; i++) {
            chunks.emplace_back(ns, i, "chunk_" + std::to_string(i));
        }
        return chunks;
    });

    std::vector<ChunkData> result1, result2;

    std::thread t1([&]() {
        ChunkVersionLight version(10, 0);  // Request >= 10
        result1 = coalescer.getChunks("test.collection", version);
    });

    std::thread t2([&]() {
        std::this_thread::sleep_for(Milliseconds(5));
        ChunkVersionLight version(50, 0);  // Request >= 50
        result2 = coalescer.getChunks("test.collection", version);
    });

    t1.join();
    t2.join();

    // result1 should have versions 10-99 (90 chunks)
    ASSERT_EQ(result1.size(), 90u);
    // result2 should have versions 50-99 (50 chunks)
    ASSERT_EQ(result2.size(), 50u);

    return true;
}

bool testHighConcurrency() {
    std::cout << "\n";

    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(20);
    config.maxWaitersPerGroup = 500;

    ConfigQueryCoalescer coalescer(config);
    MockConfigServer server(Milliseconds(5));

    coalescer.setQueryExecutor([&](const std::string& ns,
                                   const ChunkVersionLight& ver) {
        return server.getChunks(ns, ver);
    });

    const int numMongos = 100;
    const int numCollections = 5;
    std::atomic<int> successCount{0};

    auto start = SteadyClock::now();

    std::vector<std::thread> threads;
    for (int m = 0; m < numMongos; m++) {
        threads.emplace_back([&, m]() {
            std::string ns = "test.coll" + std::to_string(m % numCollections);
            ChunkVersionLight version(m % 20, 0);
            auto result = coalescer.getChunks(ns, version);
            if (!result.empty()) {
                successCount++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = SteadyClock::now();
    auto duration = std::chrono::duration_cast<Milliseconds>(end - start);

    std::cout << "  Total requests: " << numMongos << std::endl;
    std::cout << "  Successful: " << successCount.load() << std::endl;
    std::cout << "  Config server queries: " << server.stats().totalQueries.load() << std::endl;
    std::cout << "  Peak concurrent: " << server.stats().peakConcurrent.load() << std::endl;
    std::cout << "  Coalescing rate: " << (coalescer.stats().coalescingRate() * 100) << "%" << std::endl;
    std::cout << "  Query saving rate: " << (coalescer.stats().querySavingRate() * 100) << "%" << std::endl;
    std::cout << "  Duration: " << duration.count() << "ms" << std::endl;

    ASSERT_GTE(successCount.load(), numMongos * 0.95);
    ASSERT_GT(coalescer.stats().coalescingRate(), 0.7);

    return true;
}

bool testDisasterRecoverySimulation() {
    std::cout << "\n  Simulating disaster recovery: 100 mongos, 10 collections\n";

    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(10);
    config.maxWaitersPerGroup = 1000;

    ConfigQueryCoalescer coalescer(config);
    MockConfigServer server(Milliseconds(5));

    coalescer.setQueryExecutor([&](const std::string& ns,
                                   const ChunkVersionLight& ver) {
        return server.getChunks(ns, ver);
    });

    const int numMongos = 100;
    const int numCollections = 10;
    const int requestsPerMongosPerCollection = 3;
    std::atomic<int> successCount{0};

    auto start = SteadyClock::now();

    std::vector<std::thread> threads;
    for (int m = 0; m < numMongos; m++) {
        threads.emplace_back([&, m]() {
            for (int c = 0; c < numCollections; c++) {
                for (int r = 0; r < requestsPerMongosPerCollection; r++) {
                    std::string ns = "test.coll" + std::to_string(c);
                    ChunkVersionLight version(m % 50, 0);
                    auto result = coalescer.getChunks(ns, version);
                    if (!result.empty()) {
                        successCount++;
                    }
                    std::this_thread::sleep_for(Milliseconds(1));
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = SteadyClock::now();
    auto duration = std::chrono::duration_cast<Milliseconds>(end - start);

    int totalRequests = numMongos * numCollections * requestsPerMongosPerCollection;

    std::cout << "  Total requests: " << totalRequests << std::endl;
    std::cout << "  Successful: " << successCount.load() << std::endl;
    std::cout << "  Config server queries: " << server.stats().totalQueries.load() << std::endl;
    std::cout << "  Peak concurrent: " << server.stats().peakConcurrent.load() << std::endl;
    std::cout << "  Coalescing rate: " << (coalescer.stats().coalescingRate() * 100) << "%" << std::endl;
    std::cout << "  Query saving rate: " << (coalescer.stats().querySavingRate() * 100) << "%" << std::endl;
    std::cout << "  Duration: " << duration.count() << "ms" << std::endl;
    std::cout << "  Throughput: " << (successCount.load() * 1000 / duration.count()) << " req/s" << std::endl;

    ASSERT_GTE(successCount.load(), totalRequests * 0.9);

    return true;
}

bool testComparisonWithoutCoalescing() {
    std::cout << "\n  Comparing WITH vs WITHOUT coalescing:\n";

    const int numMongos = 50;
    const int numCollections = 5;

    // Without coalescing
    MockConfigServer serverNoCoalesce(Milliseconds(5));
    std::atomic<int> successNoCoalesce{0};

    auto startNoCoalesce = SteadyClock::now();
    {
        std::vector<std::thread> threads;
        for (int m = 0; m < numMongos; m++) {
            threads.emplace_back([&, m]() {
                std::string ns = "test.coll" + std::to_string(m % numCollections);
                ChunkVersionLight version(m % 20, 0);
                auto result = serverNoCoalesce.getChunks(ns, version);
                if (!result.empty()) {
                    successNoCoalesce++;
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
    }
    auto endNoCoalesce = SteadyClock::now();
    auto durationNoCoalesce = std::chrono::duration_cast<Milliseconds>(endNoCoalesce - startNoCoalesce);

    // With coalescing
    ConfigQueryCoalescer::Config config;
    config.coalescingWindow = Milliseconds(10);
    ConfigQueryCoalescer coalescer(config);
    MockConfigServer serverWithCoalesce(Milliseconds(5));

    coalescer.setQueryExecutor([&](const std::string& ns,
                                   const ChunkVersionLight& ver) {
        return serverWithCoalesce.getChunks(ns, ver);
    });

    std::atomic<int> successWithCoalesce{0};

    auto startWithCoalesce = SteadyClock::now();
    {
        std::vector<std::thread> threads;
        for (int m = 0; m < numMongos; m++) {
            threads.emplace_back([&, m]() {
                std::string ns = "test.coll" + std::to_string(m % numCollections);
                ChunkVersionLight version(m % 20, 0);
                auto result = coalescer.getChunks(ns, version);
                if (!result.empty()) {
                    successWithCoalesce++;
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
    }
    auto endWithCoalesce = SteadyClock::now();
    auto durationWithCoalesce = std::chrono::duration_cast<Milliseconds>(endWithCoalesce - startWithCoalesce);

    std::cout << "\n  | Metric                | Without | With    | Improvement |" << std::endl;
    std::cout << "  |----------------------|---------|---------|-------------|" << std::endl;
    std::cout << "  | Config Server Queries | " << std::setw(7) << serverNoCoalesce.stats().totalQueries.load()
              << " | " << std::setw(7) << serverWithCoalesce.stats().totalQueries.load()
              << " | " << std::setw(9) << std::fixed << std::setprecision(1)
              << (1.0 - static_cast<double>(serverWithCoalesce.stats().totalQueries.load()) /
                  serverNoCoalesce.stats().totalQueries.load()) * 100 << "% |" << std::endl;
    std::cout << "  | Peak Concurrent       | " << std::setw(7) << serverNoCoalesce.stats().peakConcurrent.load()
              << " | " << std::setw(7) << serverWithCoalesce.stats().peakConcurrent.load()
              << " | " << std::setw(9) << std::fixed << std::setprecision(1)
              << (1.0 - static_cast<double>(serverWithCoalesce.stats().peakConcurrent.load()) /
                  serverNoCoalesce.stats().peakConcurrent.load()) * 100 << "% |" << std::endl;
    std::cout << "  | Duration (ms)         | " << std::setw(7) << durationNoCoalesce.count()
              << " | " << std::setw(7) << durationWithCoalesce.count()
              << " |             |" << std::endl;

    // With coalescing should have significantly fewer queries
    ASSERT_LT(serverWithCoalesce.stats().totalQueries.load(),
              serverNoCoalesce.stats().totalQueries.load());

    return true;
}

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Config Query Coalescer Test Suite" << std::endl;
    std::cout << "========================================\n" << std::endl;

    TestRunner runner;

    runner.run("Basic Single Request", testBasicSingleRequest);
    runner.run("Multiple Requests Coalescing", testMultipleRequestsCoalescing);
    runner.run("Different Namespaces", testDifferentNamespaces);
    runner.run("Version Filtering", testVersionFiltering);
    runner.run("High Concurrency (100 mongos)", testHighConcurrency);
    runner.run("Disaster Recovery Simulation", testDisasterRecoverySimulation);
    runner.run("Comparison With/Without Coalescing", testComparisonWithoutCoalescing);

    runner.summary();

    return runner.exitCode();
}
