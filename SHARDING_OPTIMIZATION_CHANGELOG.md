# Sharding 路由优化改动记录

本文档记录 MongoDB 3.4.2 分片路由优化的所有改动，便于追踪和调整。

---

## 改动原则

1. **只做有增量价值的优化** - 不重复实现原有机制已有的功能
2. **每次改动都记录** - 包括新增、修改、删除
3. **记录原因** - 为什么做这个改动，解决什么问题
4. **定期评估** - 根据对代码的深入理解，及时调整

---

## 当前保留的优化模块

### 1. Rate Limiter（限流器）
- **文件**: `src/mongo/s/catalog/rate_limiter.cpp/h`
- **作用**: 限制对 config server 的并发请求数
- **原有机制**: 无
- **价值**: 高 - 防止容灾/重启场景下大量并发请求压垮 config server

### 2. Circuit Breaker（熔断器）
- **文件**: `src/mongo/s/catalog/circuit_breaker.cpp/h`
- **作用**: 当 config server 故障时快速失败，防止级联故障
- **原有机制**: 无
- **价值**: 高 - 提高系统韧性

### 3. Batch Query（批量查询）
- **文件**: `src/mongo/s/catalog/batch_query.cpp/h`
- **作用**: 一次查询多个 collection 的路由信息
- **原有机制**: 每个 collection 单独查询
- **价值**: 中 - 减少网络往返次数

### 4. Config Query Coalescer（Config Server 侧请求合并）- 新增
- **文件**: `src/mongo/s/catalog/config_query_coalescer.cpp/h`
- **新增日期**: 2024-12-16
- **作用**: 在 Config Server 侧合并来自多个 mongos 的路由查询请求
- **原有机制**: 无（mongos 侧的 `_hitConfigServerLock` 只能合并单个 mongos 内的请求）
- **价值**: 高 - 跨 mongos 请求合并，显著降低 Config Server 负载
- **测试结果**:
  - 100 mongos 场景: 95% 合并率，查询减少 95%
  - 灾难恢复模拟 (3000 请求): 96.9% 合并率，吞吐量 4076 req/s
  - Config Server 峰值并发降低 90%

### 5. 整合层和配置
- `sharding_routing_optimizer.cpp/h` - 统一管理优化模块
- `sharding_optimization_parameters.cpp/h` - 服务端参数配置
- `sharding_optimization_stats.cpp/h` - 统计信息

---

## 已删除的模块（与原有机制重复）

### 1. Request Coalescer（请求合并器）- 已删除
- **删除日期**: 2024-12-16
- **原因**: MongoDB 原有 `_hitConfigServerLock` 机制已实现类似功能
  - 原有机制在 `src/mongo/s/config.cpp` 的 `getChunkManager()` 中
  - 通过串行化 + 版本检查实现请求复用
- **教训**: 实现前应深入理解原有代码

### 2. Chunk Delta（增量同步）- 已删除
- **删除日期**: 2024-12-16
- **原因**: MongoDB 原有 `ConfigDiffTracker` 机制已实现增量查询
  - 原有机制在 `src/mongo/s/chunk_diff.cpp`
  - 通过 `lastmod >= oldVersion` 查询 + `{ns, lastmod}` 索引实现
- **教训**: 理解数据模型后再设计优化方案

---

## 改动历史

### 2024-12-16: 新增 Config Server 侧请求合并模块

**背景**: 之前删除的 Request Coalescer 是在 mongos 侧实现的，与原有 `_hitConfigServerLock` 重复。
但 Config Server 侧的请求合并是全新的优化点，可以跨多个 mongos 合并相同 collection 的查询请求。

**设计要点**:
- 合并窗口 (5-20ms): 窗口内同一 namespace 的请求被合并为一次查询
- 版本过滤: 使用最小版本执行查询，结果按各请求的版本过滤分发
- 自适应窗口: 根据负载动态调整窗口大小

**新增文件**:
- `config_query_coalescer.h` - 请求合并器头文件
- `config_query_coalescer.cpp` - 请求合并器实现
- `config_query_coalescer_test.cpp` - 功能单元测试
- `config_query_coalescer_benchmark.cpp` - 性能基准测试
- `config_query_coalescer_standalone_test.cpp` - 独立验证程序

**测试结果**:
```
| 场景                    | 请求数 | 实际查询 | 合并率  | 峰值并发 |
|------------------------|--------|----------|---------|----------|
| 100 mongos, 5 coll     | 100    | 5        | 95%     | 5        |
| 灾难恢复 (100 mongos)   | 3000   | 93       | 96.9%   | 3        |
| 对比测试 (50 mongos)    | 50     | 5 vs 50  | 90%     | 5 vs 50  |
```

**核心价值**: 将 Config Server 查询负载与 mongos 数量解耦，查询次数 = collection 数量。

---

### 2024-12-16: 回滚重复功能

**背景**: 深入分析 MongoDB 3.4.2 原有代码后发现：
1. `_hitConfigServerLock` 已实现 mongos 侧的请求串行化
2. `ConfigDiffTracker` 已实现基于版本的增量查询
3. `config.chunks` 有 `{ns, lastmod}` 索引支持高效增量查询

**操作**:
- 删除 `request_coalescer.cpp/h/test.cpp`
- 删除 `chunk_delta.cpp/h/test.cpp`
- 更新 `SConscript` 移除相关配置
- 更新 `sharding_routing_optimizer` 移除依赖
- 更新 `integration_stress_test` 移除依赖

**保留模块**: Rate Limiter, Circuit Breaker, Batch Query

---

## 原有机制分析总结

### 增量路由刷新机制
```
_initCollection: 获取 epoch（集合身份标识）
       ↓
_initChunks: 比较 epoch
       ↓
  epoch 相同 → 用 oldMetadata 的版本做增量查询
  epoch 不同 → 全量加载
       ↓
ConfigDiffTracker.configDiffQuery():
  { ns: "...", lastmod: { $gte: oldVersion } }
       ↓
config.chunks 索引 {ns: 1, lastmod: 1} 高效扫描
       ↓
calculateConfigDiff(): 删除重叠旧 chunks，插入新 chunks
```

### 请求合并机制
```
getChunkManager() {
    stdx::lock_guard<stdx::mutex> lll(_hitConfigServerLock);
    // 检查版本是否已被其他请求更新
    if (currentVersion <= ci.cm->getVersion()) {
        return ci.cm;  // 直接返回，不再查询
    }
    // 执行实际查询
}
```

### 2024-12-16: ConfigQueryCoalescer 集成到 ShardingRoutingOptimizer

**背景**: 完成 Config Server 侧请求合并模块开发后，将其集成到 ShardingRoutingOptimizer 统一管理。

**修改的文件**:
1. `sharding_optimization_parameters.h/cpp` - 添加 Config Coalescer 服务端参数
   - `shardingConfigCoalescerEnabled` (默认 true)
   - `shardingConfigCoalescerWindowMS` (默认 5ms)
   - `shardingConfigCoalescerMaxWaitMS` (默认 100ms)
   - `shardingConfigCoalescerMaxWaiters` (默认 1000)
   - `shardingConfigCoalescerAdaptiveWindow` (默认 true)

2. `sharding_routing_optimizer.cpp` - 集成 ConfigQueryCoalescer
   - 在 `Impl::initialize()` 中初始化 coalescer
   - 在 `Impl::shutdown()` 中清理 coalescer
   - 在 `Impl::getStats()` 中添加 coalescer 统计信息

3. `integration_stress_test.cpp` - 更新集成压力测试
   - 新增 `runWithCoalescerTest()` 测试函数
   - 对比三种模式: Baseline, RateLimiter, Coalescer+RateLimiter
   - 验证 Coalescer 显著减少实际查询次数

4. `SConscript` - 添加 `config_query_coalescer` 依赖
   - 添加到 `sharding_routing_optimizer` 库
   - 添加到 `integration_stress_test` 测试

**服务端参数用法**:
```bash
mongos --setParameter shardingConfigCoalescerEnabled=true \
       --setParameter shardingConfigCoalescerWindowMS=10 \
       --setParameter shardingConfigCoalescerMaxWaiters=500
```

---

### 2024-12-21: Config Server 端 Coalescer 端到端验证

**背景**: 之前的 ConfigQueryCoalescer 模块需要通过 mongos 代码路径调用。本次将 Coalescer
直接集成到 Config Server 的 `find_cmd.cpp`，使其在 Config Server 侧处理所有 `config.chunks`
查询时自动合并。

**修改的文件**:
1. `src/mongo/db/commands/find_cmd.cpp` - Config Server 侧 Coalescer 实现
   - 添加 `ConfigChunkCoalescer` 类 (行 85-134)
   - 使用 leader-follower 模式: 第一个请求成为 leader，等待 5ms 收集更多请求后执行
   - 后续请求等待 leader 结果，直接获得共享结果
   - 服务端参数: `configChunkCoalescerEnabled`, `configChunkCoalescerWindowMS`

2. `src/mongo/s/SConscript` - 添加 `local_sharding_info` 依赖

**端到端测试结果**:
```
场景: 真实 Config Server (MongoDB 3.4.2 + RocksDB 5.1.2)

| 场景                          | mongos 数 | 总耗时  | 5秒内完成 |
|-------------------------------|-----------|---------|-----------|
| 单一 ns (同查相同 collection)  | 4000      | 2.75s   | PASS ✓    |
| 5 大 collection (10K chunks)  | 2000      | 2.88s   | PASS ✓    |
| 100 混合 collection           | 200       | 3.27s   | PASS ✓    |

关键指标 (单一 ns 场景):
- P50 延迟: ~7ms (5ms 窗口 + 查询时间)
- 吞吐量: ~1500 req/s (受 Coalescer 合并效率影响)
- 4000 线程同时查询，总耗时 < 3秒
```

**协议兼容性说明**:
- 真实 mongos 使用 `exhaustiveFindOnConfig()` → `QueryRequest::asFindCommand()`
- 这会发送 find 命令到 `$cmd` collection，经过 `find_cmd.cpp`
- 直接使用 OP_QUERY 协议查询集合会绕过 find_cmd.cpp (仅影响测试脚本)

**验证方法**:
```bash
# 启动 Config Server
./mongod --storageEngine=rocksdb --dbpath=/tmp/mongo_data --port=27019

# 运行验证测试
python3 /tmp/coalescer_test.py           # 单一 ns 测试
python3 /tmp/real_config_benchmark.py scale  # 多 collection 测试
```

---

### 2024-12-23: ConfigQueryCoalescer 完整实现与测试验证

**背景**: 之前的 Config Server 侧 Coalescer 集成完成基本框架，但缺少版本差距区分合并逻辑和完整的单元测试。本次补全了所有待完成项。

**新增功能**:
1. **版本差距区分合并** - 当请求的版本差距过大时，独立执行查询而不合并
   - 新增服务端参数 `configQueryCoalescerMaxVersionGap` (默认 500)
   - 版本从查询过滤器 `{lastmod: {$gt: Timestamp(...)}}` 中提取
   - 统计 `versionGapSkippedRequests` 记录因版本差距跳过合并的请求数

2. **完整版本提取** - 从 find 命令的 filter 中提取真实版本号
   - 解析 `cmdObj.filter.lastmod.$gt` 获取 Timestamp
   - 使用 `Timestamp::asULL()` 转换为 uint64_t 版本号

**修改的文件**:
1. `src/mongo/db/s/config/config_query_coalescer.h`
   - `tryCoalesce()` 新增 `requestVersion` 参数
   - `Waiter` 结构新增 `requestedVersion` 字段
   - `CoalescingGroup` 新增 `minVersion`, `maxVersion` 字段
   - `Stats` 新增 `versionGapSkippedRequests` 统计

2. `src/mongo/db/s/config/config_query_coalescer.cpp`
   - 新增服务端参数 `configQueryCoalescerMaxVersionGap`
   - 版本差距检查逻辑: `if (newMaxVersion - newMinVersion > maxVersionGap)`
   - 版本范围更新: `group->minVersion = std::min(...)`, `group->maxVersion = std::max(...)`

3. `src/mongo/db/commands/find_cmd.cpp`
   - 从 filter 提取版本号的完整逻辑
   - 更新 `tryCoalesce()` 调用传递版本参数

4. `src/mongo/db/s/config/config_query_coalescer_test.cpp` (新增)
   - 8 个单元测试用例覆盖核心功能
   - BasicCoalescing: 验证基本合并功能
   - VersionGapSkipped: 验证版本差距过大独立执行
   - DifferentNamespacesNotCoalesced: 验证不同 namespace 不合并
   - QueryErrorPropagation: 验证错误传播
   - StatsAccuracy: 验证统计准确性
   - ShutdownHandling: 验证关闭处理
   - ResultsDistributedToAllWaiters: 验证结果分发
   - CoalescingRateCalculation: 验证合并率计算

5. `src/mongo/db/s/SConscript`
   - 新增 `config_query_coalescer_test` 单元测试配置

6. `src/mongo/s/catalog/coalescer_e2e_stress_test.cpp` (修复)
   - 改用外部 mongod (端口 27019) 而非自启动
   - 添加 `version_impl` 依赖解决 VersionInfoInterface 初始化问题
   - 减少线程数 (1000→100) 和测试时间 (30s→10s) 降低系统压力

**测试结果**:
```
单元测试 (8 tests):
  ConfigQueryCoalescerTest | tests: 8 | fails: 0 | time: 0.136s
  SUCCESS - All tests passed

E2E 压力测试 (100 线程, 50000 chunks):
  Total queries:   46138
  Success:         46138
  Failed:          0
  Success rate:    100.00%
  Avg latency:     1359 us
  Max latency:     8575 us
  Overall QPS:     4593
```

**核心价值**: ConfigQueryCoalescer 现在完整支持版本差距区分，可以智能地决定是否合并请求，避免版本差距过大时导致的无效合并。

---

## 待评估/未来方向

1. ~~**Config Server 侧请求合并**~~ - 已实现并端到端验证 (find_cmd.cpp)
2. **路由缓存持久化** - 解决 mongos 重启后全量加载问题
3. **推送式更新** - config server 主动推送路由变更

---

## 文件清单

```
src/mongo/s/catalog/
├── rate_limiter.cpp              # 限流器实现
├── rate_limiter.h
├── rate_limiter_test.cpp
├── circuit_breaker.cpp           # 熔断器实现
├── circuit_breaker.h
├── batch_query.cpp               # 批量查询实现
├── batch_query.h
├── batch_query_test.cpp
├── coalescer_e2e_stress_test.cpp # E2E 压力测试 (已修复)
├── sharding_routing_optimizer.cpp # 整合层
├── sharding_routing_optimizer.h
├── sharding_optimization_parameters.cpp # 参数
├── sharding_optimization_parameters.h
├── sharding_optimization_stats.cpp      # 统计
├── sharding_optimization_stats.h
├── integration_stress_test.cpp   # 压力测试
└── SHARDING_OPTIMIZATION_PATCH.md # 补丁说明

src/mongo/db/s/config/
├── config_query_coalescer.cpp    # Config Server 请求合并 (核心实现)
├── config_query_coalescer.h
└── config_query_coalescer_test.cpp # 单元测试 (8 tests)

src/mongo/db/commands/
└── find_cmd.cpp                  # Config Server 侧 Coalescer 集成点
```
