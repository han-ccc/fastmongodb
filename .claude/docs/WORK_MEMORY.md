# 工作记忆文档

**目的**: 帮助跨会话保持工作上下文，避免遗忘关键信息。

---

## 当前工作: 分层性能分析与优化

### 主文档
- 性能日志: `PERFORMANCE_OPTIMIZATION_LOG.md`
- 优化方案: `FIELD_EXTRACTION_OPTIMIZATION.md`
- 基准测试脚本: `/tmp/crud_benchmark_v3.py`

### 分层Timing统计系统 (v2.1.0)

已实现 mongos/mongod/RocksDB 分层时延统计:

| 文件 | 说明 |
|------|------|
| `src/mongo/db/ops/timing_stats.h` | 分层timing框架 |
| `src/mongo/db/ops/timing_stats.cpp` | 统计实现 |
| `src/mongo/db/commands/operation_timing_cmd.cpp` | 运行时命令 |

使用方法:
```javascript
db.runCommand({setOpTimingEnabled: true})   // 启用
db.runCommand({getOpTiming: 1})              // 获取统计
db.runCommand({resetOpTiming: 1})            // 重置
```

### 基线数据位置
| 文件 | 日期 | 说明 |
|-----|------|------|
| `/tmp/benchmark_results.json` | 2025-12-17 | 未优化基线，迭代2000次 |
| `/tmp/benchmark_results_v3.json` | 2025-12-18 | 阶段1优化后，迭代500次 |

### 阶段进度
| 阶段 | 状态 | 最后更新 |
|-----|------|---------|
| 阶段1 | ✅ 完成 | 端到端测试已完成，已记录基线对比 |
| 阶段2 | ✅ 完成 | 端到端测试完成，复杂INSERT -10.9% |
| 阶段3 | ⏸️ 暂缓 | 核心验证1.3x-1.6x，收益/风险比不佳 |
| 阶段4 | ❌ 已放弃 | 端到端测试退化9.4%，document-major破坏批量优化 |
| 阶段4v2 | ❌ 已放弃 | 端到端测试退化1.9%，缓存查找开销>收益 |
| 阶段4v3 | ✅ 完成 | **增量偏移量缓存，复杂INSERT -3.1% (vs Phase2)** |

### 总体成果 (阶段1+2+4v3)
| 指标 | 基线 | 优化后 | 改进 |
|-----|------|--------|------|
| 复杂表INSERT(单条) | 363us | **309us** | **-14.9%** |
| 复杂表INSERT(批量10) | 1470us | **1208us** | **-17.8%** |
| 复杂表UPDATE(索引字段) | 348us | 277us | **-20.4%** |

### 关键代码文件
- `src/mongo/db/index/btree_key_generator.h` - 阶段1&2数据结构
- `src/mongo/db/index/btree_key_generator.cpp` - 阶段1&2实现

### 端到端测试规范
**每个阶段必须包含**:
1. 简单表 + 复杂表测试
2. 与基线的对比表格
3. 所有操作: INSERT(单条/批量)、UPDATE(索引/非索引)、DELETE、POINT QUERY、RANGE SCAN

---

## 检查清单

### 开始工作前
- [ ] 检查当前分支: `git branch`
- [ ] 检查mongod状态: `ps aux | grep mongod`
- [ ] 阅读 `FIELD_EXTRACTION_OPTIMIZATION.md` 了解进度

### 提交代码前
- [ ] 运行单元测试
- [ ] 编译mongod
- [ ] 运行端到端基准测试: `python /tmp/crud_benchmark_v3.py`
- [ ] 更新文档中的测试结果（必须包含基线对比）

### 测试环境
- 测试端口: 27019 (主测试), 27098 (临时)
- 存储引擎: RocksDB
- 数据目录: `/tmp/mongo_data` 或 `/tmp/mongo_data_p1`

### 分片测试环境 (v2.1.0更新)
```
mongos (27017) → Config Server (27019) + Shard (27018, RocksDB)
```

| 组件 | 端口 | 说明 |
|------|------|------|
| Config Server | 27019 | replSet: configRS |
| Shard Server | 27018 | replSet: shardRS, RocksDB存储 |
| mongos | 27017 | 路由入口 |

数据目录: `/tmp/sharded_cluster/`

启动命令:
```bash
# 启动集群
./mongod --configsvr --replSet configRS --port 27019 --dbpath /tmp/sharded_cluster/config1 --fork --logpath /tmp/sharded_cluster/logs/config1.log
./mongod --shardsvr --replSet shardRS --port 27018 --dbpath /tmp/sharded_cluster/shard1 --storageEngine=rocksdb --fork --logpath /tmp/sharded_cluster/logs/shard1.log
./mongos --configdb configRS/localhost:27019 --port 27017 --fork --logpath /tmp/sharded_cluster/logs/mongos.log
```

---

## 常见问题

### Q: 基线数据在哪里？
A: `/tmp/benchmark_results.json` (2025-12-17未优化代码)

### Q: 为什么DELETE/RANGE SCAN性能差异大？
A: 测试条件不同（迭代次数、脚本版本），这些操作不是优化重点

### Q: 阶段2代码在哪里？
A: `btree_key_generator.cpp`中的`_prefixGroups`和getKeysImpl中的前缀分组逻辑

### Q: BatchFieldExtractor 分片测试结果如何？
A: 在3字段分片键场景下，性能差异在噪声范围内(±5%)。原因是分片键字段少、网络延迟主导。代码已保留，对更多字段场景可能有收益。

---

## 更新日志

| 日期 | 更新内容 |
|-----|---------|
| 2025-12-18 | 阶段1端到端测试完成，添加基线对比 |
| 2025-12-18 | 阶段2代码实现并完成端到端测试，复杂INSERT -10.9% |
| 2025-12-18 | 阶段3核心验证完成(1.3x-1.6x)，建议暂缓 |
| 2025-12-18 | 阶段4分析完成，核心类已实现，集成复杂度高，建议先提交阶段1+2 |
| 2025-12-18 | 阶段4端到端测试完成，退化9.4%，根因是document-major破坏批量优化，已回退代码 |
| 2025-12-19 | 阶段4v2完成: 保持index-major+单文档缓存，退化1.9%，缓存开销>收益，已回退 |
| 2025-12-19 | **阶段4v3成功**: 增量偏移量缓存，复杂INSERT 309us (-3.1% vs Phase2, -14.9% vs基线) |
| 2025-12-19 | 优化A+B测试: 前缀缓存扩展+短字段整数比较，退化14.2%，已回退。结论:额外优化边际收益为负 |
| 2025-12-20 | BatchFieldExtractor 集成到 ShardKeyPattern::extractShardKeyFromDoc()，等待分片场景测试 |
| 2025-12-20 | **分片场景严格对比测试**: INSERT -2.5%，UPDATE -3.0%，QUERY无影响(已验证代码路径独立) |
| 2025-12-20 | **v2.1.0 分层Timing系统**: 新增mongos/mongod/RocksDB分层时延统计，搭建分片集群测试环境 |
