# Sharding 路由优化计划

## 背景

500/1000 Shards 同时崩溃场景下，当前架构无法在 30s 内恢复。
根本原因：1TB 突发流量（1000 mongos × 200MB 全量路由）超出 5GB 网卡容量。

## 优化目标

| 指标 | 当前值 | 目标值 | 状态 |
|------|--------|--------|------|
| getChunks 响应大小 | 200MB | 100KB (增量) | 原有机制已支持 |
| Config 并发查询 | 5000 | 5-10 (合并后) | ✅ Coalescer 实现 |
| 恢复时间 (情况B) | >2500s | <30s | 待验证 |

---

## 优化模块总览

| 模块 | 状态 | 说明 |
|------|------|------|
| 模块1: 限流熔断 | ✅ 完成 | Rate Limiter + Circuit Breaker |
| 模块2: Config Server 请求合并 | ✅ 完成 | Config Query Coalescer (96.9% 合并率) |
| 模块3: 批量查询 | ✅ 完成 | Batch Query |
| 模块4: 分片键快速提取 | ✅ 完成 | Shard Key Fast Path (-28%) |
| ~~模块X: mongos侧请求合并~~ | ❌ 删除 | 原有 `_hitConfigServerLock` 已实现 |
| ~~模块X: 增量同步协议~~ | ❌ 删除 | 原有 `ConfigDiffTracker` 已实现 |

---

## 模块1: 限流熔断 (Rate Limiting / Circuit Breaker) ✅

**状态**: 已完成

**功能**:
- Config Server 端：并发请求超过阈值时快速拒绝
- Mongos 端：限制对 Config 的并发请求数
- 故障时快速失败，防止级联故障

**文件**:
- [x] `src/mongo/s/catalog/rate_limiter.h` - 限流器接口
- [x] `src/mongo/s/catalog/rate_limiter.cpp` - 限流器实现
- [x] `src/mongo/s/catalog/circuit_breaker.h` - 熔断器接口
- [x] `src/mongo/s/catalog/circuit_breaker.cpp` - 熔断器实现
- [x] `src/mongo/s/catalog/rate_limiter_test.cpp` - 单元测试

**测试**:
- [x] 基本限流功能
- [x] 熔断触发和恢复
- [x] 并发压力测试

---

## 模块2: Config Server 请求合并 (Config Query Coalescer) ✅

**状态**: 已完成

**功能**:
- 在 Config Server 侧合并来自多个 mongos 的路由查询请求
- 合并窗口 (5-20ms) 内同一 namespace 的请求合并为一次查询
- 版本过滤：使用最小版本执行查询，结果按各请求的版本过滤分发
- 自适应窗口：根据负载动态调整窗口大小

**核心价值**: 查询次数 = collection 数量，与 mongos 数量解耦

**文件**:
- [x] `src/mongo/s/catalog/config_query_coalescer.h` - 合并器接口
- [x] `src/mongo/s/catalog/config_query_coalescer.cpp` - 合并器实现
- [x] `src/mongo/s/catalog/config_query_coalescer_test.cpp` - 单元测试
- [x] `src/mongo/s/catalog/config_query_coalescer_benchmark.cpp` - 性能基准
- [x] `src/mongo/s/catalog/config_query_coalescer_standalone_test.cpp` - 独立验证

**测试结果**:

| 场景 | 请求数 | 实际查询 | 合并率 | 峰值并发 |
|------|--------|----------|--------|----------|
| 100 mongos, 5 coll | 100 | 5 | 95% | 5 |
| 灾难恢复 (100 mongos) | 3000 | 93 | 96.9% | 3 |
| 对比测试 (50 mongos) | 50 | 5 vs 50 | 90% | 5 vs 50 |

**服务端参数**:
```bash
mongos --setParameter shardingConfigCoalescerEnabled=true \
       --setParameter shardingConfigCoalescerWindowMS=10 \
       --setParameter shardingConfigCoalescerMaxWaiters=500
```

---

## 模块3: 批量查询 (Batch Query) ✅

**状态**: 已完成

**功能**:
- 支持单次请求查询多个 namespace 的路由
- 减少 RTT 开销

**文件**:
- [x] `src/mongo/s/catalog/batch_query.h` - 批量查询接口
- [x] `src/mongo/s/catalog/batch_query.cpp` - 批量查询实现
- [x] `src/mongo/s/catalog/batch_query_test.cpp` - 单元测试

**测试**:
- [x] 基本批量功能
- [x] 结果正确分发

---

## 模块4: 分片键快速提取 (Shard Key Fast Path) ✅

**状态**: 已完成
**Commit**: `1194148c605`

**功能**:
- 预计算 `_allTopLevel` 标志判断是否所有分片键字段都是顶级
- 顶级字段使用直接 `doc[fieldName]` 查找，避免 ElementPath/MatchableDocument 开销
- 预分配 BSONObjBuilder (64 bytes)

**文件**:
- [x] `src/mongo/s/shard_key_pattern.cpp` - 快速路径实现
- [x] `src/mongo/s/shard_key_pattern.h` - 新增 `_allTopLevel` 成员
- [x] `src/mongo/s/commands/cluster_shard_key_stats_cmd.cpp` - 监控命令

**微基准测试结果**:

| 场景 | v2.0.0 | v2.3.0 | 差异 |
|------|--------|--------|------|
| 1 key, 5 fields | 31ns | 30ns | -3.8% |
| 3 keys, 10 fields | 105ns | 81ns | -22.4% |
| 3 keys, 20 fields | 129ns | 93ns | **-27.5%** |

---

## 已删除的模块

### ~~mongos 侧请求合并 (Request Coalescer)~~

**删除原因**: MongoDB 原有 `_hitConfigServerLock` 机制已实现类似功能

```cpp
// src/mongo/s/config.cpp - getChunkManager()
stdx::lock_guard<stdx::mutex> lll(_hitConfigServerLock);
if (currentVersion <= ci.cm->getVersion()) {
    return ci.cm;  // 直接返回，不再查询
}
```

### ~~增量同步协议 (Chunk Delta)~~

**删除原因**: MongoDB 原有 `ConfigDiffTracker` 机制已实现增量查询

```cpp
// src/mongo/s/chunk_diff.cpp
ConfigDiffTracker.configDiffQuery():
  { ns: "...", lastmod: { $gte: oldVersion } }
// 使用索引 {ns: 1, lastmod: 1} 高效扫描
```

---

## 整合层

**文件**:
- [x] `src/mongo/s/catalog/sharding_routing_optimizer.cpp/h` - 统一管理优化模块
- [x] `src/mongo/s/catalog/sharding_optimization_parameters.cpp/h` - 服务端参数
- [x] `src/mongo/s/catalog/sharding_optimization_stats.cpp/h` - 统计信息
- [x] `src/mongo/s/catalog/integration_stress_test.cpp` - 集成压力测试

---

## 执行状态

### 完成进度

- [x] 计划制定完成
- [x] 模块1: 限流熔断
- [x] 模块2: Config Server 请求合并
- [x] 模块3: 批量查询
- [x] 模块4: 分片键快速提取
- [x] 整合层实现
- [ ] 端到端集成测试验证

### 提交记录

| 日期 | Commit | 内容 |
|------|--------|------|
| 2024-12-16 | - | Rate Limiter, Circuit Breaker 完成 |
| 2024-12-16 | - | Config Query Coalescer 完成 |
| 2024-12-16 | - | Batch Query 完成 |
| 2024-12-16 | - | 删除重复模块 (request_coalescer, chunk_delta) |
| 2024-12-20 | `1194148c605` | Shard Key Fast Path 完成 |

---

## 测试验证清单

### 单元测试
- [x] rate_limiter_test 全部通过
- [x] config_query_coalescer_test 全部通过
- [x] batch_query_test 全部通过

### 性能基准
- [x] 合并：100并发同ns请求合并率 >95%
- [x] 分片键提取：顶级字段 -28%

### 集成测试
- [ ] 本地小集群 (1 config + 2 mongos + 2 shard)
- [ ] StaleConfig 触发路由刷新
- [ ] 模拟 shard 故障

---

## 待评估/未来方向

1. **路由缓存持久化** - 解决 mongos 重启后全量加载问题
2. **推送式更新** - config server 主动推送路由变更

---

## 风险和回滚

### 风险点
1. 请求合并超时导致请求丢失
2. 限流过严影响正常请求

### 回滚策略
- 每个模块独立开关（服务端参数）
- 异常时自动降级
- 监控指标告警

---

## 相关文档

- `SHARDING_OPTIMIZATION_CHANGELOG.md` - 详细改动记录
- `.claude/docs/MICROBENCHMARK_CATALOG.md` - 微基准测试结果
