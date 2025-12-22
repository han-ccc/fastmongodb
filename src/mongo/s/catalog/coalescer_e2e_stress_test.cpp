/**
 * Coalescer E2E Stress Test - Enhanced Version
 *
 * Multi-collection high-concurrency stress test for ConfigQueryCoalescer
 *
 * Features:
 * - Multi-collection simulation (100k total chunks)
 * - Progressive concurrency exploration (1000 -> limit, step +1000)
 * - CPU/Memory/Network monitoring via /proc
 * - Weighted random query distribution
 *
 * Collection distribution:
 * - 1 x 50,000 chunks (large_coll)
 * - 2 x 20,000 chunks (medium_coll_1, medium_coll_2)
 * - 1 x 9,000 chunks (small_coll)
 * - 100 x 10 chunks (tiny_coll_001 ~ tiny_coll_100)
 *
 * Run:
 *   ./build/opt/mongo/mongod --dbpath=/tmp/mongo_data --port=27019 --fork --logpath=/tmp/mongod.log
 *   ./build/opt/mongo/s/catalog/coalescer_e2e_stress_test
 */

#include "mongo/platform/basic.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

// ============================================================================
// Configuration
// ============================================================================

// Version scenario for testing coalescer behavior
enum class VersionScenario {
    RANDOM,         // Uniform random [1, numChunks] - baseline
    SAME_VERSION,   // All requests use same version - max coalescing
    CLOSE_VERSIONS, // Versions within [base, base+100] - high coalescing
    BOUNDARY_GAP,   // Versions within [base, base+500] - boundary test
    HOTSPOT_MIX     // 80% close versions, 20% random - realistic
};

const char* versionScenarioName(VersionScenario s) {
    switch (s) {
        case VersionScenario::RANDOM: return "RANDOM";
        case VersionScenario::SAME_VERSION: return "SAME_VERSION";
        case VersionScenario::CLOSE_VERSIONS: return "CLOSE_VERSIONS";
        case VersionScenario::BOUNDARY_GAP: return "BOUNDARY_GAP";
        case VersionScenario::HOTSPOT_MIX: return "HOTSPOT_MIX";
    }
    return "UNKNOWN";
}

struct TestConfig {
    int port = 27019;

    // Concurrency exploration
    size_t startConcurrency = 1000;
    size_t concurrencyStep = 1000;
    size_t maxConcurrency = 20000;

    // Test parameters
    size_t testDurationSec = 30;
    double maxFailRate = 0.01;  // 1% fail rate = limit reached

    // Version scenario
    VersionScenario versionScenario = VersionScenario::RANDOM;
};

// Collection configuration
struct CollectionInfo {
    std::string ns;
    size_t numChunks;
};

const std::vector<CollectionInfo> kMainCollections = {
    {"testdb.large_coll", 50000},
    {"testdb.medium_coll_1", 20000},
    {"testdb.medium_coll_2", 20000},
    {"testdb.small_coll", 9000},
};

const size_t kNumTinyCollections = 100;
const size_t kTinyCollectionChunks = 10;

// ============================================================================
// Resource Monitor (reads /proc)
// ============================================================================

struct ResourceStats {
    std::atomic<double> peakCpuPercent{0};
    std::atomic<uint64_t> peakMemoryMB{0};
    std::atomic<uint64_t> networkRxBytes{0};
    std::atomic<uint64_t> networkTxBytes{0};
    std::atomic<uint64_t> initialRxBytes{0};
    std::atomic<uint64_t> initialTxBytes{0};

    void updatePeakCpu(double cpu) {
        double current = peakCpuPercent.load();
        while (cpu > current && !peakCpuPercent.compare_exchange_weak(current, cpu)) {}
    }

    void updatePeakMemory(uint64_t mem) {
        uint64_t current = peakMemoryMB.load();
        while (mem > current && !peakMemoryMB.compare_exchange_weak(current, mem)) {}
    }
};

class ResourceMonitor {
public:
    ResourceMonitor(ResourceStats& stats, int mongodPid)
        : _stats(stats), _mongodPid(mongodPid), _running(false) {}

    void start() {
        _running = true;
        // Record initial network stats
        auto netStats = getNetworkStats();
        _stats.initialRxBytes = netStats.first;
        _stats.initialTxBytes = netStats.second;

        _thread = std::thread([this]() { monitorLoop(); });
    }

    void stop() {
        _running = false;
        if (_thread.joinable()) {
            _thread.join();
        }
        // Calculate total network usage
        auto netStats = getNetworkStats();
        _stats.networkRxBytes = netStats.first - _stats.initialRxBytes.load();
        _stats.networkTxBytes = netStats.second - _stats.initialTxBytes.load();
    }

private:
    void monitorLoop() {
        uint64_t prevTotal = 0, prevIdle = 0;

        while (_running) {
            // CPU usage
            auto cpuStats = getCpuStats();
            if (prevTotal > 0) {
                uint64_t totalDiff = cpuStats.first - prevTotal;
                uint64_t idleDiff = cpuStats.second - prevIdle;
                if (totalDiff > 0) {
                    double cpuPercent = 100.0 * (totalDiff - idleDiff) / totalDiff;
                    _stats.updatePeakCpu(cpuPercent);
                }
            }
            prevTotal = cpuStats.first;
            prevIdle = cpuStats.second;

            // Memory usage
            uint64_t memMB = getProcessMemoryMB(_mongodPid);
            _stats.updatePeakMemory(memMB);

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    std::pair<uint64_t, uint64_t> getCpuStats() {
        std::ifstream stat("/proc/stat");
        std::string line;
        if (std::getline(stat, line)) {
            if (line.substr(0, 3) == "cpu") {
                std::istringstream iss(line.substr(4));
                uint64_t user, nice, system, idle, iowait, irq, softirq;
                iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq;
                uint64_t total = user + nice + system + idle + iowait + irq + softirq;
                return {total, idle};
            }
        }
        return {0, 0};
    }

    uint64_t getProcessMemoryMB(int pid) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        std::ifstream status(path);
        std::string line;
        while (std::getline(status, line)) {
            if (line.substr(0, 6) == "VmRSS:") {
                std::istringstream iss(line.substr(6));
                uint64_t kb;
                iss >> kb;
                return kb / 1024;
            }
        }
        return 0;
    }

    std::pair<uint64_t, uint64_t> getNetworkStats() {
        std::ifstream netdev("/proc/net/dev");
        std::string line;
        uint64_t totalRx = 0, totalTx = 0;

        while (std::getline(netdev, line)) {
            // Skip header lines
            if (line.find(':') == std::string::npos) continue;
            if (line.find("lo:") != std::string::npos) continue;  // Skip loopback

            size_t colonPos = line.find(':');
            std::istringstream iss(line.substr(colonPos + 1));
            uint64_t rx, tx;
            uint64_t dummy;
            // Format: rx_bytes rx_packets ... tx_bytes tx_packets ...
            iss >> rx >> dummy >> dummy >> dummy >> dummy >> dummy >> dummy >> dummy >> tx;
            totalRx += rx;
            totalTx += tx;
        }
        return {totalRx, totalTx};
    }

    ResourceStats& _stats;
    int _mongodPid;
    std::atomic<bool> _running;
    std::thread _thread;
};

// ============================================================================
// Test Statistics
// ============================================================================

struct TestStats {
    std::atomic<uint64_t> totalQueries{0};
    std::atomic<uint64_t> successQueries{0};
    std::atomic<uint64_t> failedQueries{0};
    std::atomic<uint64_t> totalLatencyUs{0};
    std::atomic<uint64_t> maxLatencyUs{0};

    // Per-collection stats
    std::atomic<uint64_t> largeCollQueries{0};
    std::atomic<uint64_t> mediumCollQueries{0};
    std::atomic<uint64_t> smallCollQueries{0};
    std::atomic<uint64_t> tinyCollQueries{0};

    void reset() {
        totalQueries = 0;
        successQueries = 0;
        failedQueries = 0;
        totalLatencyUs = 0;
        maxLatencyUs = 0;
        largeCollQueries = 0;
        mediumCollQueries = 0;
        smallCollQueries = 0;
        tinyCollQueries = 0;
    }

    void recordSuccess(int64_t latencyUs) {
        // Guard against negative latency (clock issues)
        if (latencyUs < 0) latencyUs = 0;

        totalQueries++;
        successQueries++;
        totalLatencyUs += static_cast<uint64_t>(latencyUs);

        uint64_t latency = static_cast<uint64_t>(latencyUs);
        uint64_t current = maxLatencyUs.load();
        while (latency > current && !maxLatencyUs.compare_exchange_weak(current, latency)) {}
    }

    void recordFailure() {
        totalQueries++;
        failedQueries++;
    }

    double getSuccessRate() const {
        uint64_t total = totalQueries.load();
        return total > 0 ? 100.0 * successQueries.load() / total : 0;
    }

    uint64_t getAvgLatencyUs() const {
        uint64_t success = successQueries.load();
        return success > 0 ? totalLatencyUs.load() / success : 0;
    }
};

// ============================================================================
// Collection Selector (weighted random)
// ============================================================================

class CollectionSelector {
public:
    CollectionSelector(VersionScenario scenario = VersionScenario::RANDOM)
        : _gen(_rd()), _scenario(scenario) {}

    struct Selection {
        std::string ns;
        size_t maxVersion;
    };

    Selection select(TestStats& stats) {
        int roll = _gen() % 100;

        if (roll < 60) {
            // 60% large collection
            stats.largeCollQueries++;
            return {"testdb.large_coll", 50000};
        } else if (roll < 80) {
            // 20% medium collections
            stats.mediumCollQueries++;
            return {(_gen() % 2 == 0) ? "testdb.medium_coll_1" : "testdb.medium_coll_2", 20000};
        } else if (roll < 90) {
            // 10% small collection
            stats.smallCollQueries++;
            return {"testdb.small_coll", 9000};
        } else {
            // 10% tiny collections
            stats.tinyCollQueries++;
            char ns[64];
            snprintf(ns, sizeof(ns), "testdb.tiny_coll_%03d", 1 + static_cast<int>(_gen() % kNumTinyCollections));
            return {ns, kTinyCollectionChunks};
        }
    }

    // Get version based on scenario
    size_t getVersion(size_t maxVersion) {
        size_t baseVersion = maxVersion / 2;  // Middle of range

        switch (_scenario) {
            case VersionScenario::SAME_VERSION:
                // All requests use same version - maximum coalescing potential
                return baseVersion;

            case VersionScenario::CLOSE_VERSIONS:
                // Versions within [base, base+100] - gap < maxVersionGap(500)
                return baseVersion + (_gen() % 100);

            case VersionScenario::BOUNDARY_GAP:
                // Versions within [base, base+500] - at boundary of maxVersionGap
                return baseVersion + (_gen() % 500);

            case VersionScenario::HOTSPOT_MIX:
                // 80% close versions, 20% random - realistic scenario
                if ((_gen() % 100) < 80) {
                    return baseVersion + (_gen() % 100);
                } else {
                    return 1 + (_gen() % maxVersion);
                }

            case VersionScenario::RANDOM:
            default:
                // Uniform random - baseline, many will skip coalescing
                return 1 + (_gen() % maxVersion);
        }
    }

private:
    std::random_device _rd;
    std::mt19937 _gen;
    VersionScenario _scenario;
};

// ============================================================================
// Data Generator
// ============================================================================

class DataGenerator {
public:
    static bool insertAllCollections(int port) {
        std::cout << "\n[Data Setup] Inserting test data (100,000 total chunks)..." << std::endl;

        try {
            HostAndPort server("localhost", port);
            DBClientConnection conn;
            std::string errmsg;

            if (!conn.connect(server, "coalescer_data_gen", errmsg)) {
                std::cerr << "Connect failed: " << errmsg << std::endl;
                std::cerr << "Please start mongod: ./build/opt/mongo/mongod --dbpath=/tmp/mongo_data --port="
                          << port << " --fork --logpath=/tmp/mongod.log" << std::endl;
                return false;
            }

            // Drop all test collections
            conn.dropCollection("config.chunks");

            auto startTime = Date_t::now();
            size_t totalInserted = 0;

            // Insert main collections
            for (const auto& coll : kMainCollections) {
                insertChunks(conn, coll.ns, coll.numChunks, totalInserted);
                totalInserted += coll.numChunks;
                std::cout << "  [" << coll.ns << "] " << coll.numChunks << " chunks" << std::endl;
            }

            // Insert tiny collections
            std::cout << "  [tiny collections] " << kNumTinyCollections << " x "
                      << kTinyCollectionChunks << " chunks..." << std::endl;
            for (size_t i = 1; i <= kNumTinyCollections; i++) {
                char ns[64];
                snprintf(ns, sizeof(ns), "testdb.tiny_coll_%03d", static_cast<int>(i));
                insertChunks(conn, ns, kTinyCollectionChunks, totalInserted);
                totalInserted += kTinyCollectionChunks;
            }

            auto elapsed = Date_t::now() - startTime;
            std::cout << "  Total: " << totalInserted << " chunks in " << elapsed.count() << "ms" << std::endl;

            // Create index
            conn.createIndex("config.chunks", BSON("ns" << 1 << "lastmod" << 1));
            std::cout << "  Index created: {ns: 1, lastmod: 1}" << std::endl;

            return true;
        } catch (const std::exception& e) {
            std::cerr << "Insert failed: " << e.what() << std::endl;
            return false;
        }
    }

private:
    static void insertChunks(DBClientConnection& conn, const std::string& ns,
                             size_t numChunks, size_t idOffset) {
        const size_t batchSize = 1000;
        std::vector<BSONObj> batch;
        batch.reserve(batchSize);

        for (size_t i = 0; i < numChunks; i++) {
            size_t globalId = idOffset + i;
            BSONObjBuilder builder;
            builder.append("_id", static_cast<long long>(globalId));
            builder.append("ns", ns);
            builder.append("min", BSON("_id" << static_cast<long long>(i * 1000)));
            builder.append("max", BSON("_id" << static_cast<long long>((i + 1) * 1000)));
            builder.append("shard", "shard" + std::to_string(i % 10));
            builder.appendTimestamp("lastmod", (static_cast<unsigned long long>(i + 1) << 32));

            batch.push_back(builder.obj());

            if (batch.size() >= batchSize) {
                conn.insert("config.chunks", batch);
                batch.clear();
            }
        }

        if (!batch.empty()) {
            conn.insert("config.chunks", batch);
        }
    }
};

// ============================================================================
// Query Worker
// ============================================================================

class QueryWorker {
public:
    QueryWorker(int port, TestStats& stats, std::atomic<bool>& running, VersionScenario scenario)
        : _port(port), _stats(stats), _running(running), _scenario(scenario) {}

    void run() {
        HostAndPort server("localhost", _port);
        CollectionSelector selector(_scenario);

        try {
            DBClientConnection conn;
            std::string errmsg;

            if (!conn.connect(server, "coalescer_worker", errmsg)) {
                _stats.recordFailure();
                return;
            }

            while (_running.load()) {
                auto selection = selector.select(_stats);
                size_t version = selector.getVersion(selection.maxVersion);

                auto queryStart = std::chrono::high_resolution_clock::now();

                try {
                    BSONObj query = BSON("ns" << selection.ns
                                         << "lastmod" << BSON("$gt" << Timestamp(version, 0)));

                    auto cursor = conn.query("config.chunks", query, 1000);

                    while (cursor->more()) {
                        cursor->next();
                    }

                    auto queryEnd = std::chrono::high_resolution_clock::now();
                    auto latencyUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        queryEnd - queryStart).count();

                    _stats.recordSuccess(latencyUs);

                } catch (const std::exception&) {
                    _stats.recordFailure();
                }
            }

        } catch (const std::exception&) {
            // Worker failed to start
        }
    }

private:
    int _port;
    TestStats& _stats;
    std::atomic<bool>& _running;
    VersionScenario _scenario;
};

// ============================================================================
// Result Display
// ============================================================================

class ResultPrinter {
public:
    static void printHeader(size_t concurrency, size_t durationSec, VersionScenario scenario) {
        std::cout << "\n";
        printLine();
        std::cout << "  Coalescer E2E Stress Test - " << concurrency << " threads" << std::endl;
        std::cout << "  Duration: " << durationSec << "s | Collections: 104 | Chunks: 100,000" << std::endl;
        std::cout << "  Version Scenario: " << versionScenarioName(scenario) << std::endl;
        printLine();
    }

    static void printResults(size_t concurrency, const TestStats& stats,
                             const ResourceStats& resources, uint64_t durationMs) {
        uint64_t qps = stats.totalQueries.load() * 1000 / durationMs;

        std::cout << "\n";
        printLine();
        std::cout << "  RESULTS: " << concurrency << " concurrent threads" << std::endl;
        printLine();

        // Performance
        std::cout << "  Performance:" << std::endl;
        std::cout << "    Total Queries:    " << std::setw(12) << stats.totalQueries.load() << std::endl;
        std::cout << "    Success:          " << std::setw(12) << stats.successQueries.load() << std::endl;
        std::cout << "    Failed:           " << std::setw(12) << stats.failedQueries.load() << std::endl;
        std::cout << "    Success Rate:     " << std::setw(11) << std::fixed << std::setprecision(2)
                  << stats.getSuccessRate() << "%" << std::endl;
        std::cout << "    QPS:              " << std::setw(12) << qps << std::endl;
        std::cout << "    Avg Latency:      " << std::setw(10) << stats.getAvgLatencyUs() << " us" << std::endl;
        std::cout << "    Max Latency:      " << std::setw(10) << stats.maxLatencyUs.load() << " us" << std::endl;

        // Resource usage
        std::cout << "  Resources (Peak):" << std::endl;
        std::cout << "    CPU Usage:        " << std::setw(11) << std::fixed << std::setprecision(1)
                  << resources.peakCpuPercent.load() << "%" << std::endl;
        std::cout << "    Memory:           " << std::setw(10) << resources.peakMemoryMB.load() << " MB" << std::endl;
        std::cout << "    Network RX:       " << std::setw(10) << (resources.networkRxBytes.load() / 1024 / 1024) << " MB" << std::endl;
        std::cout << "    Network TX:       " << std::setw(10) << (resources.networkTxBytes.load() / 1024 / 1024) << " MB" << std::endl;

        // Query distribution
        std::cout << "  Query Distribution:" << std::endl;
        std::cout << "    Large (50k):      " << std::setw(12) << stats.largeCollQueries.load() << std::endl;
        std::cout << "    Medium (20k x2):  " << std::setw(12) << stats.mediumCollQueries.load() << std::endl;
        std::cout << "    Small (9k):       " << std::setw(12) << stats.smallCollQueries.load() << std::endl;
        std::cout << "    Tiny (10 x100):   " << std::setw(12) << stats.tinyCollQueries.load() << std::endl;

        printLine();
    }

    static void printSummary(const std::vector<std::pair<size_t, uint64_t>>& results) {
        std::cout << "\n";
        printLine();
        std::cout << "  CONCURRENCY EXPLORATION SUMMARY" << std::endl;
        printLine();
        std::cout << "  Threads       QPS       Status" << std::endl;
        std::cout << "  -------  ----------  ----------" << std::endl;

        for (size_t i = 0; i < results.size(); i++) {
            const char* status = (i == results.size() - 1) ? "LIMIT" : "OK";
            std::cout << "  " << std::setw(7) << results[i].first
                      << "  " << std::setw(10) << results[i].second
                      << "  " << status << std::endl;
        }
        printLine();
    }

private:
    static void printLine() {
        std::cout << "  " << std::string(56, '=') << std::endl;
    }
};

// ============================================================================
// Get mongod PID
// ============================================================================

int getMongodPid(int port) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "pgrep -f 'mongod.*%d' 2>/dev/null", port);
    FILE* pipe = popen(cmd, "r");
    if (pipe) {
        char buffer[32];
        if (fgets(buffer, sizeof(buffer), pipe)) {
            pclose(pipe);
            return atoi(buffer);
        }
        pclose(pipe);
    }
    return -1;
}

// ============================================================================
// Run Single Test Round
// ============================================================================

struct TestResult {
    uint64_t qps;
    double successRate;
    uint64_t avgLatencyUs;
};

TestResult runTestRound(const TestConfig& config, size_t numThreads, TestStats& stats, int mongodPid) {
    stats.reset();
    ResourceStats resources;

    ResultPrinter::printHeader(numThreads, config.testDurationSec, config.versionScenario);

    // Start resource monitor
    ResourceMonitor monitor(resources, mongodPid);
    monitor.start();

    // Start worker threads
    std::atomic<bool> running{true};
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    auto testStart = Date_t::now();

    for (size_t i = 0; i < numThreads; i++) {
        workers.emplace_back([&config, &stats, &running]() {
            QueryWorker worker(config.port, stats, running, config.versionScenario);
            worker.run();
        });
    }

    // Progress display
    for (size_t sec = 0; sec < config.testDurationSec; sec++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        uint64_t currentQps = stats.totalQueries.load() / (sec + 1);
        double failRate = stats.totalQueries.load() > 0 ?
            100.0 * stats.failedQueries.load() / stats.totalQueries.load() : 0;

        std::cout << "\r  [" << std::setw(2) << (sec + 1) << "s] "
                  << "QPS: " << std::setw(6) << currentQps
                  << " | Total: " << std::setw(8) << stats.totalQueries.load()
                  << " | Fail: " << std::setw(5) << stats.failedQueries.load()
                  << " (" << std::fixed << std::setprecision(2) << failRate << "%)"
                  << std::flush;
    }
    std::cout << std::endl;

    // Stop test
    running.store(false);
    for (auto& worker : workers) {
        worker.join();
    }

    monitor.stop();

    auto testEnd = Date_t::now();
    uint64_t durationMs = (testEnd - testStart).count();

    // Print results
    ResultPrinter::printResults(numThreads, stats, resources, durationMs);

    return {
        stats.totalQueries.load() * 1000 / durationMs,
        stats.getSuccessRate(),
        stats.getAvgLatencyUs()
    };
}

// ============================================================================
// Main Test
// ============================================================================

TEST(CoalescerE2EStressTest, ConcurrencyExploration) {
    TestConfig config;
    TestStats stats;

    std::cout << "\n";
    std::cout << "  ============================================================" << std::endl;
    std::cout << "    ConfigQueryCoalescer E2E Stress Test" << std::endl;
    std::cout << "    Port: " << config.port << " | Start: " << config.startConcurrency
              << " | Step: +" << config.concurrencyStep << " | Max: " << config.maxConcurrency << std::endl;
    std::cout << "  ============================================================" << std::endl;

    // Step 1: Insert test data
    if (!DataGenerator::insertAllCollections(config.port)) {
        FAIL("Failed to insert test data. Is mongod running?");
        return;
    }

    // Step 2: Get mongod PID for resource monitoring
    int mongodPid = getMongodPid(config.port);
    if (mongodPid <= 0) {
        std::cout << "[Warning] Could not find mongod PID, resource monitoring disabled" << std::endl;
        mongodPid = 1;  // Use init process as fallback
    } else {
        std::cout << "[Info] Monitoring mongod PID: " << mongodPid << std::endl;
    }

    // Step 3: Progressive concurrency exploration
    std::vector<std::pair<size_t, uint64_t>> results;
    size_t concurrency = config.startConcurrency;

    while (concurrency <= config.maxConcurrency) {
        auto result = runTestRound(config, concurrency, stats, mongodPid);

        results.push_back({concurrency, result.qps});

        // Check if limit reached
        double failRate = 100.0 - result.successRate;
        if (failRate > config.maxFailRate * 100) {
            std::cout << "\n  [LIMIT REACHED] Fail rate " << std::fixed << std::setprecision(2)
                      << failRate << "% > " << (config.maxFailRate * 100) << "% threshold" << std::endl;
            break;
        }

        concurrency += config.concurrencyStep;

        // Brief pause between rounds
        if (concurrency <= config.maxConcurrency) {
            std::cout << "\n  [Next round in 3 seconds...]" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }

    // Step 4: Print summary
    ResultPrinter::printSummary(results);

    // Assertions
    ASSERT_FALSE(results.empty());
    ASSERT_GTE(results[0].second, static_cast<uint64_t>(100));  // At least 100 QPS at 1000 threads

    std::cout << "\n  [PASS] Concurrency exploration completed!" << std::endl;
    std::cout << "  Maximum stable concurrency: " << results.back().first << " threads" << std::endl;
    std::cout << "  Peak QPS: " << results.back().second << std::endl;
}

// ============================================================================
// Version Scenario Test - Compare different version distributions
// ============================================================================

TEST(CoalescerE2EStressTest, VersionScenarioComparison) {
    TestConfig config;
    config.startConcurrency = 1000;
    config.maxConcurrency = 1000;  // Single concurrency level
    config.testDurationSec = 15;   // Shorter duration per scenario

    TestStats stats;

    std::cout << "\n";
    std::cout << "  ============================================================" << std::endl;
    std::cout << "    Version Scenario Comparison Test" << std::endl;
    std::cout << "    Concurrency: " << config.startConcurrency << " threads" << std::endl;
    std::cout << "    Duration: " << config.testDurationSec << "s per scenario" << std::endl;
    std::cout << "  ============================================================" << std::endl;

    // Insert test data once
    if (!DataGenerator::insertAllCollections(config.port)) {
        FAIL("Failed to insert test data. Is mongod running?");
        return;
    }

    int mongodPid = getMongodPid(config.port);
    if (mongodPid <= 0) {
        mongodPid = 1;
    }

    // Test each scenario
    std::vector<VersionScenario> scenarios = {
        VersionScenario::SAME_VERSION,
        VersionScenario::CLOSE_VERSIONS,
        VersionScenario::BOUNDARY_GAP,
        VersionScenario::HOTSPOT_MIX,
        VersionScenario::RANDOM
    };

    std::vector<std::tuple<const char*, uint64_t, uint64_t>> scenarioResults;

    for (auto scenario : scenarios) {
        config.versionScenario = scenario;

        std::cout << "\n  >>> Testing scenario: " << versionScenarioName(scenario) << std::endl;

        auto result = runTestRound(config, config.startConcurrency, stats, mongodPid);
        scenarioResults.push_back({versionScenarioName(scenario), result.qps, result.avgLatencyUs});

        // Pause between scenarios
        std::cout << "  [Pause 3 seconds before next scenario...]" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    // Print comparison summary
    std::cout << "\n";
    std::cout << "  ========================================================" << std::endl;
    std::cout << "    VERSION SCENARIO COMPARISON RESULTS" << std::endl;
    std::cout << "  ========================================================" << std::endl;
    std::cout << "  Scenario            QPS       Avg Latency" << std::endl;
    std::cout << "  ----------------  -------  -------------" << std::endl;

    for (const auto& r : scenarioResults) {
        std::cout << "  " << std::left << std::setw(16) << std::get<0>(r)
                  << "  " << std::right << std::setw(7) << std::get<1>(r)
                  << "  " << std::setw(10) << std::get<2>(r) << " us" << std::endl;
    }
    std::cout << "  ========================================================" << std::endl;

    std::cout << "\n  [PASS] Version scenario comparison completed!" << std::endl;
}

}  // namespace
}  // namespace mongo
