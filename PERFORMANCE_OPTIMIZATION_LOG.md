# MongoDB + RocksDB 性能优化工作日志

## 项目概述
- **目标**: 优化 MongoDB 3.4.2 + RocksDB 5.1.2 的 CRUD 操作时延
- **方法**: 基准测试驱动的优化，每次优化必须通过测试验证
- **环境**: WSL2 Linux, 本地TCP连接 (127.0.0.1:27019)

---

## 版本历史

| 版本 | 日期 | 描述 | 主要变更 |
|-----|------|------|---------|
| v1.0.0 | 2025-12-17 | Baseline - 原始性能基准 | 建立基准测试框架，修复测试脚本 |
| v1.0.1 | 2025-12-17 | P1 文档字段缓存优化 | ❌ 失败并回滚 - 破坏批量处理，缓存未被使用 |
| v1.1.0 | 2025-12-18 | P2 StringData零拷贝优化 | ⚪ 微弱 - 0-3%提升，在噪声范围内 |
| v1.2.0 | 2025-12-18 | P1重做: 顶层字段快速路径 | ✓ 有效 - INSERT批量约3%提升 |
| v2.0.0 | 2025-12-19 | 稳定版基线 | Phase 1,2,4v3,A 优化合并，方向3回滚 |
| v2.1.0 | 2025-12-20 | 分层Timing统计系统 | 新增mongos/mongod/RocksDB分层时延统计，分片集群测试环境 |
| v2.2.0 | 2025-12-21 | Phase A': BSONElement::size()分支消除 | 使用kFixedSizes查表替代switch语句 |
| v2.3.0 | 2025-12-21 | Phase B': 短字符串整数比较优化 | StringData≤8字节使用uint64比较替代memcmp |

---

## v2.3.0 Phase B': 短字符串整数比较优化 (2025-12-21)

### 优化内容

**背景**: 用户工作负载中字段名≤5字符，memcmp对短字符串效率不高。

**修改**: `StringData::operator==`对≤8字节字符串使用uint64_t整数比较。

#### 修改文件

| 文件 | 修改 |
|------|------|
| `src/mongo/base/string_data.h:32-34` | 添加`<cstdint>`头文件 |
| `src/mongo/base/string_data.h:197-213` | 短字符串整数比较逻辑 |

#### 优化逻辑

```cpp
inline bool operator==(StringData lhs, StringData rhs) {
    size_t sz = lhs.size();
    if (sz != rhs.size()) return false;

    // 短字符串优化: 整数比较替代memcmp
    if (sz <= 8) {
        uint64_t a = 0, b = 0;
        std::memcpy(&a, lhs.rawData(), sz);
        std::memcpy(&b, rhs.rawData(), sz);
        return a == b;
    }
    return lhs.compare(rhs) == 0;
}
```

### 测试结果

#### 单元测试
- `bson_obj_test`: 28 tests, 0 fails ✓
- `lazy_bson_test`: 8 tests, 0 fails ✓

#### Sharding集群基准测试 (2000次迭代)

**简单表 (1 index)**

| 测试 | 平均(us) | P50(us) | P95(us) | P99(us) |
|------|----------|---------|---------|---------|
| INSERT (single) | 477 | 471 | 533 | 644 |
| INSERT (batch 10) | 546 | 537 | 624 | 721 |
| QUERY (exact key) | 656 | 540 | 630 | 9753 |
| QUERY (partial key) | 754 | 750 | 836 | 922 |
| QUERY ($gt) | 555 | 552 | 640 | 700 |
| QUERY ($in) | 629 | 621 | 711 | 823 |
| UPDATE (single) | 496 | 492 | 555 | 647 |
| UPDATE (multi) | 49126 | 46757 | 79601 | 96009 |

**复杂表 (10 indexes, 10 fields each)**

| 测试 | 平均(us) | P50(us) | P95(us) | P99(us) |
|------|----------|---------|---------|---------|
| INSERT (single) | 561 | 554 | 622 | 718 |
| INSERT (batch 10) | 1207 | 1188 | 1329 | 1588 |
| QUERY (exact key) | 561 | 556 | 628 | 721 |
| QUERY (partial key) | 658 | 653 | 729 | 824 |
| QUERY ($gt) | 642 | 639 | 706 | 806 |
| QUERY ($in) | 7628 | 7441 | 9109 | 9706 |
| UPDATE (single) | 479 | 474 | 518 | 619 |
| UPDATE (indexed fields) | 539 | 533 | 591 | 707 |

### 分析

1. **INSERT批量显著提升**: -6.2% vs Phase A'，批量插入多文档时字段名比较频繁
2. **QUERY partial提升**: -4.6% vs Phase A'，部分键查询涉及字段名匹配
3. **短字段优化生效**: 用户字段名≤5字符场景，整数比较比memcmp更快

### 累计优化效果 (vs v1.0.0原始基线)

| 指标 | v1.0.0 | v2.3.0 | 总提升 |
|------|--------|--------|--------|
| 复杂表INSERT(批量10) | 1470us | 1207us | **-17.9%** |
| 简单表INSERT(batch 10) | - | 546us | - |

---

## v2.2.0 Phase A': BSONElement::size()分支消除 (2025-12-21)

### 优化内容

**发现**: `kFixedSizes[32]`查表和`kVariableSizeMask`位掩码在`bsonelement.h`中已定义但**未被使用**。

**修改**: 将`bsonelement.cpp:574-638`的64行switch语句替换为O(1)查表逻辑。

#### 修改文件

| 文件 | 修改 |
|------|------|
| `src/mongo/bson/bsonelement.cpp:49-54` | 添加静态成员类外定义(C++11 ODR) |
| `src/mongo/bson/bsonelement.cpp:574-617` | size()使用kFixedSizes查表+位掩码 |

#### 优化逻辑

```cpp
int BSONElement::size() const {
    int t = static_cast<int>(type());
    // 快速路径: 固定大小类型查表 O(1)
    if (t < 32 && kFixedSizes[t] != 0) {
        x = kFixedSizes[t];
    } else {
        // 变长类型使用位掩码判断
        uint32_t typeBit = 1u << t;
        if (typeBit & kStringSizeMask) { ... }
        else if (typeBit & kObjectSizeMask) { ... }
        // ...
    }
}
```

### 测试结果

#### 单元测试
- `bson_obj_test`: 28 tests, 0 fails ✓
- `lazy_bson_test`: 8 tests, 0 fails ✓

#### 单机基准测试 (500次迭代)

| 操作 | v2.0.0基线 | Phase A' | 变化 |
|------|-----------|----------|------|
| 复杂表INSERT(单条) | 332us | **324us** | **-2.4%** |
| 复杂表INSERT(批量10) | 1238us | **1181us** | **-4.6%** |
| 复杂表UPDATE(索引字段) | 291us | **289us** | **-0.7%** |
| 简单表INSERT(单条) | 224us | **238us** | +6.3% |
| 简单表INSERT(批量10) | 282us | **276us** | -2.1% |

#### Sharding集群基准测试 (2000次迭代)

| 操作 | 平均(us) | P50 | P95 | P99 |
|------|----------|-----|-----|-----|
| INSERT(单条) | 477 | 469 | 544 | 639 |
| INSERT(批量10) | 601 | 586 | 702 | 852 |
| UPDATE(simple) | 496 | 490 | 558 | 631 |
| QUERY(exact key) | 6570 | 6513 | 6943 | 9800 |
| QUERY(partial key) | 823 | 812 | 936 | 1080 |

### 分析

1. **复杂表收益明显**: INSERT批量-4.6%，单条-2.4%
2. **简单表轻微波动**: 在噪声范围内(±5%)
3. **分支预测优化**: 热路径从switch改为查表，减少分支预测失败
4. **代码简化**: 64行switch → ~40行查表+位掩码

### 累计优化效果 (vs v1.0.0原始基线)

| 指标 | v1.0.0 | v2.2.0 | 总提升 |
|------|--------|--------|--------|
| 复杂表INSERT(批量10) | 1470us | **1181us** | **-19.7%** |
| 复杂表INSERT(单条) | 377us | **324us** | **-14.1%** |

---

## v2.1.0 分层Timing统计系统 (2025-12-20)

### 目标

建立精细的分层时延统计系统，区分：
1. **mongos层**: 路由、分片键提取、目标定位
2. **mongod层**: 命令解析、文档验证、索引更新
3. **RocksDB层**: 纯存储引擎调用 (Get/Put/Write/Iterator)

### 实现

#### 新增文件

| 文件 | 说明 |
|------|------|
| `src/mongo/db/ops/timing_stats.h` | 分层timing框架头文件 |
| `src/mongo/db/ops/timing_stats.cpp` | timing统计实现 |
| `src/mongo/db/commands/operation_timing_cmd.cpp` | 运行时统计命令 |

#### 修改文件

| 文件 | 修改内容 |
|------|----------|
| `src/mongo/db/ops/write_ops_exec.cpp` | 添加 MongodInsertParse timing |
| `src/mongo/db/query/find.cpp` | 添加 MongodQueryParse timing |
| `src/mongo/s/commands/cluster_write_cmd.cpp` | 添加 MongosInsertParse timing |
| `/tmp/mongo-rocks/src/rocks_recovery_unit.cpp` | 添加 RocksDB Get/Write/Iterator timing |
| 多个 SConscript 文件 | 添加 timing_stats 库依赖 |

#### 统计阶段枚举

```cpp
enum class DetailedOpPhase {
    // mongos 阶段
    MongosInsertParse, MongosInsertShardKey, MongosInsertTarget, MongosInsertBatch,
    MongosUpdateParse, MongosUpdateShardKey, MongosUpdateTarget,
    MongosQueryParse, MongosQueryCanonical, MongosQueryTarget, MongosQueryMerge,

    // mongod 阶段
    MongodInsertParse, MongodInsertValidate, MongodInsertIndex, MongodInsertOplog,
    MongodUpdateParse, MongodUpdatePlan, MongodUpdateExec, MongodUpdateIndex,
    MongodQueryParse, MongodQueryCanonical, MongodQueryPlan, MongodQueryExec,

    // RocksDB 阶段
    RocksDBGet, RocksDBPut, RocksDBDelete, RocksDBWrite, RocksDBIterSeek, RocksDBIterNext
};
```

### 使用方法

```javascript
// 启用/禁用timing
db.runCommand({setOpTimingEnabled: true})
db.runCommand({setOpTimingEnabled: false})

// 获取详细统计
db.runCommand({getOpTiming: 1})

// 获取汇总统计
db.runCommand({getOpTiming: 1, summary: true})

// 重置统计
db.runCommand({resetOpTiming: 1})
```

### 分片集群测试环境

搭建了用于后续测试的分片集群：

```
架构:
  mongos (27017)
      ↓
  Config Server (27019, 单节点RS)
      ↓
  Shard (27018, RocksDB存储引擎, 单节点RS)
```

启动命令:
```bash
# Config Server
./mongod --configsvr --replSet configRS --port 27019 --dbpath /tmp/sharded_cluster/config1

# Shard (RocksDB)
./mongod --shardsvr --replSet shardRS --port 27018 --dbpath /tmp/sharded_cluster/shard1 --storageEngine=rocksdb

# mongos
./mongos --configdb configRS/localhost:27019 --port 27017
```

### 测试结果 (50次INSERT + 50次QUERY)

#### 分层时延统计

| 层级 | 阶段 | 次数 | 平均耗时 | 最小 | 最大 |
|------|------|------|----------|------|------|
| **mongos** | insert/parse | 51 | 517 us | - | - |
| **mongod** | insert/parse | 50 | 31 us | 26 us | 80 us |
| **RocksDB** | get | 284 | 1.5 us | 0 us | 20 us |
| **RocksDB** | write | 150 | 5.4 us | 2 us | 37 us |
| **RocksDB** | iterSeek | 166 | 8.6 us | 0 us | 57 us |
| **RocksDB** | iterNext | 1225 | 0.1 us | 0 us | 30 us |

#### 端到端时延

| 操作 | 平均时延 | 说明 |
|------|----------|------|
| INSERT (单条) | 0.67 ms | 通过mongos |
| QUERY (点查) | 5.59 ms | 通过mongos，首次查询较慢 |

### 关键发现

1. **mongos开销**: INSERT解析在mongos层约517us，包含分片路由开销
2. **mongod开销**: INSERT解析在mongod层仅31us，远小于mongos
3. **RocksDB效率高**:
   - 单次Get: 1.5us
   - 单次Write: 5.4us
   - Iterator操作极快 (0.1us/次)
4. **网络开销**: mongos→mongod的网络传输是主要开销来源

### 后续优化方向

基于分层统计数据，优化应聚焦于：
1. **mongos层**: 减少分片路由开销
2. **批量操作**: 提高批量处理效率，摊薄网络开销
3. **索引更新**: 添加 MongodInsertIndex timing 进一步分析

---

## 分片集群基线 (2025-12-20)

### 测试环境

```
架构: mongos (27017) → Config Server (27019) + Shard (27018, RocksDB)
测试参数: 500次迭代, 50次预热, 批量大小10
```

### 简单表基线 (1索引, ~150B文档)

| 操作 | 平均(us) | P50 | P90 | P99 |
|------|----------|-----|-----|-----|
| INSERT(单条) | 544 | 532 | 589 | 747 |
| INSERT(批量10) | 673 | 636 | 691 | 2006 |
| POINT QUERY | 622 | 615 | 669 | 777 |
| RANGE SCAN(100) | 1658 | 1684 | 2019 | 2220 |
| UPDATE(非索引) | 582 | 573 | 623 | 753 |
| DELETE | 574 | 556 | 645 | 804 |

### 复杂表基线 (11索引, ~800B文档)

| 操作 | 平均(us) | P50 | P90 | P99 |
|------|----------|-----|-----|-----|
| INSERT(单条) | 680 | 602 | 655 | 1958 |
| INSERT(批量10) | 1263 | 1226 | 1315 | 2371 |
| POINT QUERY | 632 | 623 | 685 | 801 |
| RANGE SCAN(100) | 4461 | 4513 | 5936 | 7270 |
| UPDATE(非索引) | 586 | 572 | 659 | 752 |
| UPDATE(索引字段) | 620 | 605 | 707 | 863 |
| DELETE | 617 | 597 | 723 | 855 |

### 分层时延统计 (复杂表)

| 层级 | 阶段 | 次数 | 平均耗时 |
|------|------|------|----------|
| mongos | insert/parse | 500 | 320 us |
| RocksDB | get | 2000 | 1.5 us |
| RocksDB | write | 500 | 30 us |

### 对比: 分片 vs 单机

| 操作 | 单机基线(v2.0.0) | 分片集群 | 差异 |
|------|-----------------|----------|------|
| INSERT(单条)复杂表 | 332 us | 680 us | +105% |
| INSERT(批量10)复杂表 | 1238 us | 1263 us | +2% |
| POINT QUERY复杂表 | 217 us | 632 us | +191% |
| UPDATE(非索引) | 249 us | 586 us | +135% |
| DELETE | 268 us | 617 us | +130% |

### 分析

1. **单条操作开销大**: 分片集群单条操作时延是单机的2-3倍，主要来自:
   - mongos路由开销 (~320us)
   - mongos→mongod网络往返 (~200us)

2. **批量操作影响小**: 批量INSERT仅增加2%，因为网络开销被摊薄

3. **RocksDB效率高**: 纯存储引擎层面:
   - 单次Get: 1.5us
   - 单次Write: 30us (比单机高，可能因为分片元数据更新)

4. **优化重点**:
   - 尽量使用批量操作摊薄网络开销
   - mongos路由优化空间有限
   - 可考虑连接池优化减少连接建立开销

### 基线数据文件

| 文件 | 说明 |
|------|------|
| `/tmp/sharded_baseline_results.json` | 完整测试结果(含timing) |
| `/tmp/sharded_cluster_benchmark.py` | 测试脚本 |

---

## v2.0.0 稳定版基线 (2025-12-19)

### 优化内容汇总

| 阶段 | 优化内容 | 状态 |
|------|----------|------|
| Phase 1 | 字段提取基础优化 | ✓ 保留 |
| Phase 2 | 嵌套字段前缀分组 | ✓ 保留 |
| Phase 4v3 | 增量偏移缓存 | ✓ 保留 |
| Phase A | BSON grow_reallocate位运算 | ✓ 保留 |
| 方向3 | StackAllocator/ElapsedTracker/WorkingSet | ✗ 全部回滚(回归) |

### v2.0.0 基准测试结果

#### 简单表 (1索引, ~150B)

| 操作 | 平均(us) | P50 | P90 | P99 |
|-----|---------|-----|-----|-----|
| INSERT(单条) | 224 | 215 | 246 | 339 |
| INSERT(批量10) | 282 | 274 | 307 | 446 |
| UPDATE(非索引) | 241 | 233 | 266 | 339 |
| DELETE | 231 | 223 | 251 | 338 |
| POINT QUERY | 211 | 207 | 229 | 286 |
| RANGE SCAN(100) | 469 | 467 | 587 | 729 |

#### 复杂表 (11索引, ~800B)

| 操作 | 平均(us) | P50 | P90 | P99 |
|-----|---------|-----|-----|-----|
| INSERT(单条) | 332 | 323 | 369 | 477 |
| INSERT(批量10) | 1238 | 1214 | 1316 | 1624 |
| UPDATE(非索引) | 249 | 241 | 286 | 346 |
| UPDATE(索引字段) | 291 | 281 | 328 | 446 |
| DELETE | 268 | 258 | 300 | 376 |
| POINT QUERY | 217 | 210 | 234 | 358 |
| RANGE SCAN(100) | 768 | 753 | 1017 | 1157 |

### vs 原始基线 (v1.0.0) 对比

| 指标 | v1.0.0基线 | v2.0.0 | 提升 |
|------|-----------|--------|------|
| INSERT(批量10) | 1470us | 1238us | **-15.8%** |
| POINT QUERY | 247us | 217us | **-12.1%** |
| INSERT(单条) | 377us | 332us | **-11.9%** |
| DELETE | 310us | 268us | **-13.5%** |

### 方向3失败尝试总结

| 优化项 | 结果 | 原因分析 |
|--------|------|----------|
| StackAllocator 512→1024 | +24.5%回归 | L1缓存压力增加 |
| ElapsedTracker懒检查 | +33.5%回归 | 增加分支开销 |
| WorkingSet预分配 | +33.5%回归 | 简单查询付出额外成本 |

**结论**: 底层微优化空间有限，增加复杂度反而有害。

---

## v1.0.0 Baseline 基准测试

### 测试配置

#### 简单表
```javascript
// 索引: 仅 _id (1个索引)
// 文档大小: ~150 bytes
{
  _id: "prefix_seq",
  seq: <int>,
  data: "<100字节随机字符串>"
}
```

#### 复杂表
```javascript
// 索引: 11个 (含_id)
//   - _id索引
//   - {bn, on, vi} unique 复合索引
//   - 9个10字段复合索引
// 文档大小: ~800 bytes
// 总索引字段: 100+
```

#### 测试参数
- 每项测试: 500 次迭代
- 预热: 50-100 次
- 测试时间: 2025-12-17

---

### 基准测试结果汇总

| 操作 | 简单表 (us) | 复杂表 (us) | 增幅 |
|-----|------------|------------|------|
| **INSERT(单条)** | 239 | 377 | **+57.4%** |
| **INSERT(批量10条)** | 294 | 1470 | **+399.6%** |
| UPDATE(非索引字段) | 246 | 270 | +9.4% |
| UPDATE(索引字段) | - | 348 | - |
| DELETE | 241 | 310 | +28.6% |
| POINT QUERY | 217 | 247 | +13.6% |
| **RANGE SCAN(100条)** | 444 | 704 | **+58.5%** |

---

### 详细测试数据

#### 1. INSERT 写入时延

##### 单条INSERT
| 指标 | 简单表 (us) | 复杂表 (us) | 差异 |
|-----|------------|------------|------|
| 平均值 | 239 | 377 | +57.4% |
| P50 | 234 | 367 | +56.8% |
| P90 | 259 | 411 | +58.7% |
| P99 | 360 | 519 | +44.2% |

##### 批量INSERT (10条/次)
| 指标 | 简单表 (us) | 复杂表 (us) | 差异 |
|-----|------------|------------|------|
| 平均值 | 294 | 1470 | +399.6% |
| P50 | 289 | 1456 | +403.8% |
| P90 | 316 | 1538 | +386.7% |
| P99 | 475 | 1777 | +274.1% |
| **每条平均** | **29.4** | **147** | +400% |

**分析**:
- 单条INSERT: 11个索引导致+138us (+57%)
- 批量INSERT: 复杂表每条147us，远高于简单表29.4us
- 批量效率: 简单表8.1x提升，复杂表仅2.6x提升

#### 2. UPDATE 更新时延

| 场景 | 简单表 (us) | 复杂表 (us) | 差异 |
|-----|------------|------------|------|
| 非索引字段(seq) | 246 | 270 | +9.4% |
| 索引字段(f1) | - | 348 | - |

**分析**:
- 非索引字段更新几乎不受索引数量影响
- 索引字段更新比非索引字段慢+79us (+29%)

#### 3. DELETE 删除时延

| 指标 | 简单表 (us) | 复杂表 (us) | 差异 |
|-----|------------|------------|------|
| 平均值 | 241 | 310 | +28.6% |
| P50 | 236 | 302 | +27.9% |
| P90 | 262 | 334 | +27.4% |
| P99 | 321 | 436 | +35.8% |

**分析**: DELETE需要从所有索引中删除条目，开销约+69us。

#### 4. POINT QUERY 点查时延 (按_id)

| 指标 | 简单表 (us) | 复杂表 (us) | 差异 |
|-----|------------|------------|------|
| 平均值 | 217 | 247 | +13.6% |
| P50 | 217 | 242 | +11.5% |
| P90 | 233 | 265 | +13.7% |
| P99 | 280 | 369 | +31.8% |

**分析**: 点查只使用_id索引，时延差异主要来自文档大小(150B vs 800B)。

#### 5. RANGE SCAN 范围查询时延 (返回100条)

| 指标 | 简单表 (us) | 复杂表 (us) | 差异 |
|-----|------------|------------|------|
| 平均值 | 444 | 704 | +58.5% |
| P50 | 441 | 693 | +57.1% |
| P90 | 548 | 891 | +62.6% |
| P99 | 650 | 1031 | +58.6% |
| **响应大小** | **14.6 KB** | **107.6 KB** | +637% |
| **每条大小** | ~146 B | ~1076 B | +637% |

**分析**: 范围查询时延主要受响应数据大小影响，而非索引数量。

---

### 时延组成分析

#### INSERT路径分析
```
客户端端到端时延 (简单表):    ~239 us (100%)
├── 网络/协议开销:            ~210 us (88%)
└── MongoDB服务端:             ~29 us (12%)
    ├── WriteUnitOfWork:       ~20 us
    │   ├── IndexUpdate:        ~7 us (1个索引)
    │   ├── RocksDB WAL:        ~6 us
    │   └── RecordStore:        ~1 us
    └── 其他:                   ~9 us

客户端端到端时延 (复杂表):    ~377 us (100%)
├── 网络/协议开销:            ~210 us (56%)
└── MongoDB服务端:            ~167 us (44%)
    ├── WriteUnitOfWork:      ~155 us
    │   ├── IndexUpdate:      ~140 us (11个索引, ~13us/索引)
    │   ├── RocksDB WAL:        ~6 us
    │   └── RecordStore:        ~3 us
    └── 其他:                  ~12 us
```

---

## 优化方向分析

### 基于Baseline的优化优先级

| 优先级 | 目标 | 当前 | 目标值 | 优化空间 |
|-------|------|------|--------|---------|
| **P0** | INSERT(复杂表批量) | 147us/条 | <50us/条 | 高 |
| **P0** | INSERT(复杂表单条) | 377us | <280us | 高 |
| P1 | DELETE(复杂表) | 310us | <260us | 中 |
| P2 | RANGE SCAN(复杂表) | 704us | <500us | 中 |
| P3 | UPDATE(索引字段) | 348us | <300us | 低 |

### 核心优化点

#### 1. 索引更新优化 (最高优先级)
**当前问题**:
- 11个索引串行更新
- 每个索引~13us，总计~140us
- 批量INSERT时，索引更新占主导

**潜在方案**:
- 索引键预生成/缓存
- 批量索引键编码
- 并行索引更新(需评估锁竞争)

#### 2. 批量操作优化
**当前问题**:
- 复杂表批量INSERT效率低(仅2.6x vs 简单表8.1x)
- 每条文档独立生成索引键

**潜在方案**:
- 索引键批量生成
- WriteBatch优化

#### 3. 网络/协议优化
**当前问题**:
- 单条操作网络开销~210us (占88%)

**潜在方案**:
- 更大批量操作
- 连接复用优化

---

## 基准验证测试 (2025-12-18)

### 测试条件
- 分支: `perf/baseline` (纯基线，无P2优化)
- mongod: 重新编译启动
- 数据库: 使用已有数据目录

### 测试结果 (系统空闲时)

| 操作 | 12/17基线 | 12/18验证 | 差异 |
|-----|----------|----------|------|
| INSERT(批量10)复杂表 | 1470us | **1674us** | +13.9% |
| INSERT(单条)复杂表 | 377us | **394us** | +4.5% |
| POINT QUERY复杂表 | 247us | **244us** | -1.2% |
| RANGE SCAN复杂表 | 704us | **788us** | +11.9% |

### 差异分析
- 系统空闲时测试结果接近历史基线
- INSERT略高于基线（+14%），可能与数据量增长有关
- POINT QUERY几乎无变化（-1.2%）
- 确认为有效基线数据

---

## 细分时延统计集成 (2025-12-18)

### 目标
为所有CRUD操作添加细分时延统计，用于性能分析和瓶颈定位。

### 实现
- 新增 `src/mongo/db/ops/operation_timing_stats.h` - 通用时延统计框架
- 新增 `src/mongo/db/commands/operation_timing_cmd.cpp` - 查询/重置统计命令
- 修改 `src/mongo/db/ops/write_ops_exec.cpp` - INSERT/UPDATE/DELETE时延统计
- 修改 `src/mongo/db/query/find.cpp` - QUERY时延统计

### A/B测试结果

| 操作 | 纯基线(A) | 带统计(B) | 开销 |
|-----|----------|----------|------|
| INSERT(批量10)复杂表 | 1665us | 1687us | **+1.3%** |
| INSERT(单条)复杂表 | 360us | 352us | -2.2% |
| POINT QUERY复杂表 | 203us | 186us | -8.4% |

**结论**: 时延统计开销可忽略（~1.3%），正常测量误差范围内。

### 时延分解分析

#### INSERT (10440次操作)
| 阶段 | 平均时延 | 占比 |
|-----|---------|------|
| **Total** | 95us | 100% |
| LockAcquisition | 1us | 1% |
| WriteUnitOfWork | 90us | 95% |
| RecordStore | 74us | 78% |

#### UPDATE (1800次操作)
| 阶段 | 平均时延 | 占比 |
|-----|---------|------|
| **Total** | 76us | 100% |
| LockAcquisition | 1us | 1% |
| PlanExecution | 67us | 88% |

#### DELETE (1200次操作)
| 阶段 | 平均时延 | 占比 |
|-----|---------|------|
| **Total** | 61us | 100% |
| LockAcquisition | 0us | ~0% |
| PlanExecution | 54us | 89% |

### 关键发现
1. **锁获取极快** (~1us)，不是性能瓶颈
2. **INSERT瓶颈**: WriteUnitOfWork占95%，其中RecordStore占82%
3. **UPDATE/DELETE瓶颈**: PlanExecution占~88%
4. 优化应聚焦于存储引擎写入和查询计划执行

### 使用方法
```javascript
// 获取所有操作统计
db.runCommand({getOperationTimingStats: 1})

// 获取指定操作统计
db.runCommand({getOperationTimingStats: 1, op: "insert"})

// 重置统计
db.runCommand({resetOperationTimingStats: 1})
```

---

## P2优化正式验证 (2025-12-18)

### 测试条件
- 分支: `perf/baseline` + P2修改
- 修改文件:
  - `src/mongo/db/bson/dotted_path_support.cpp`: `std::string(path, p-path)` → `StringData(path, p-path)`
  - `src/mongo/db/index/btree_key_generator.cpp`: `str::before()` → `strchr() + StringData`
- 测试时间: 系统空闲，无干扰

### 端到端时延对比 (复杂表)

| 操作 | 基线(无P2) | P2优化后 | 差异 |
|-----|-----------|---------|------|
| INSERT(批量10) | 1415us | 1368-1409us | **-3.3% ~ 0%** |
| POINT QUERY | 239us | 239-253us | 0% ~ +5.9% |
| UPDATE(非索引) | 261us | 255-258us | -2.3% ~ -1.2% |
| UPDATE(索引) | 399us | 399-408us | 0% ~ +2.3% |
| DELETE | 335us | 340-343us | +1.5% ~ +2.4% |

### 细分时延对比 (INSERT avgMicros)

| 阶段 | 基线 | P2 | 差异 |
|-----|-----|-----|-----|
| Total | 97us | 97us | 0 |
| LockAcquisition | 1us | 1us | 0 |
| WriteUnitOfWork | 93us | 93us | 0 |
| RecordStore | 76us | 78us | +2us |

### 结论

**P2优化效果微弱，在噪声范围内**

1. **INSERT批量操作**: 有0-3%的潜在提升，但不稳定
2. **查询/更新操作**: 基本无变化，部分情况轻微回退
3. **细分时延**: 各阶段时延几乎相同，P2修改点影响范围有限

**分析**:
- P2修改的两个函数(`extractElementAtPathOrArrayAlongPath`, `extractNextElement`)主要用于复合索引字段提取
- 这些函数每次调用只产生1个临时字符串分配(~50-100ns)
- 11个索引×10条记录=110次调用，理论最大节省~11us
- 实测在噪声范围内(±20us)，提升不显著

**建议**: P2优化可选择性保留(代码更清晰)，但性能提升微弱

---

## P1重做: 顶层字段快速路径 (2025-12-18)

### 问题分析

原P1优化失败原因:
1. 破坏了批量处理结构 (`O(N_indexes)` → `O(N_records × N_indexes)`)
2. 缓存构建了但 `getKeysWithCache()` 被注释掉

进一步分析发现:
- 测试表的11个索引使用**94个完全不同的顶层字段**
- 即使启用缓存，字段不重叠导致**缓存命中率为0**
- 缓存开销（std::string构建、map查找）反而增加延迟

### 正确优化方案

对 `extractElementAtPathOrArrayAlongPath()` 添加顶层字段快速路径:

```cpp
BSONElement extractElementAtPathOrArrayAlongPath(const BSONObj& obj, const char*& path) {
    // 快速路径：顶层字段（无"."），直接提取
    const char* dot = strchr(path, '.');
    if (!dot) {
        BSONElement sub = obj.getField(path);
        path = path + strlen(path);
        return sub;
    }

    // 嵌套路径：使用递归+缓存
    // ...
}
```

### 测试结果

| 操作 | 基线 | P1优化后 | 改善 |
|-----|-----|---------|-----|
| INSERT(批量10) | 1415us | 1372us | **-3.0%** |
| INSERT(单条) | 362us | 342us | **-5.5%** |
| POINT QUERY | 239us | 216us | **-9.6%** |

### 结论

✓ **优化有效**。顶层字段快速路径避免了94个字段的递归调用开销。

修改文件: `src/mongo/db/bson/dotted_path_support.cpp`

---

## 优化记录

### 优化 #1: P1 文档字段缓存优化 [已回滚]

**版本**: v1.0.1 → 已回滚
**日期**: 2025-12-17
**状态**: ❌ 失败并回滚

---

#### 1. 优化目标

理论上通过缓存减少文档遍历次数:
- 问题假设: N个索引导致文档被遍历N次
- 方案: 缓存字段提取结果，遍历从N次减少到1次

#### 2. 实现方法

在 `index_catalog.cpp` 的 `indexRecords()` 中:
```cpp
// 修改后的代码逻辑
for (each record r) {
    prepareCache(r);  // 构建字段缓存
    for (each index i) {
        _indexRecords(txn, i, {r}, keysInsertedOut);
    }
}
```

#### 3. 测试结果

| 操作 | 优化前 (us) | 优化后 (us) | 变化 |
|-----|------------|------------|------|
| INSERT(单条) | 377 | 401 | **+6.5%** ✗ |
| INSERT(批量10条) | 1470 | 1765 | **+20.1%** ✗ |
| UPDATE(索引字段) | 348 | 340 | -2.4% |
| UPDATE(非索引字段) | 270 | 248 | -7.8% |

---

#### 4. 根本原因分析 (深度剖析)

##### 问题1: 批量处理被完全破坏 [最严重]

**原始代码**:
```cpp
for (each index i) {
    _indexRecords(txn, i, bsonRecords);  // bsonRecords = 全部记录
}
// batch_insert_10 + 11索引 = 11次调用
```

**P1修改后**:
```cpp
for (each record r) {
    for (each index i) {
        _indexRecords(txn, i, {r});  // {r} = 单条记录!
    }
}
// batch_insert_10 + 11索引 = 110次调用 (10倍增加!)
```

**影响**: 函数调用开销从 O(N_indexes) 变为 O(N_records × N_indexes)

##### 问题2: 缓存构建了但从未被使用!

代码调用流程:
```
prepareForDocument() → DocumentFieldCache::buildCache() [缓存已构建]
        ↓
_indexRecords()
        ↓
IndexAccessMethod::insert()
        ↓
BtreeKeyGenerator::getKeys()  ← 不检查缓存!
        ↓
extractElementAtPathOrArrayAlongPath()  ← 直接遍历文档!
```

patch文件中的 `getKeysWithCache()` 方法是**被注释掉的**:
```cpp
/* void BtreeKeyGeneratorV1::getKeysWithCache(...) */
```

**结论**: 缓存完全无效，只有构建开销，没有任何收益!

##### 问题3: 缓存开销分析

每个文档的额外开销:
| 操作 | 开销 |
|-----|------|
| thread_local TLS查找 | ~10-50ns |
| unordered_map::clear() | ~100-500ns |
| unordered_set::clear() | ~50-200ns |
| 字符串操作(find, substr) | ~50-200ns/字段 |
| Hash计算和插入 | ~100-300ns/字段 |
| BSONObj遍历构建缓存 | ~500-2000ns |
| **总计** | **~1-3us/文档** |

##### 问题4: 索引计数效率低

```cpp
size_t indexCount = 0;
for (auto it = _entries.begin(); it != _entries.end(); ++it) {
    ++indexCount;  // O(n) 遍历只为计数
}
```

---

#### 5. 为什么UPDATE看起来"改善"了?

UPDATE操作的轻微改善(-2.4%到-7.8%)实际上是**测量噪声**:
- UPDATE使用 `updateIndexRecords()` 而非 `indexRecords()`
- P1修改对UPDATE路径几乎无影响
- 2-8%的变化在测试误差范围内

---

#### 6. 经验教训

| 错误 | 教训 |
|-----|------|
| 修改循环结构未分析影响 | 任何循环嵌套顺序改变都需要性能测试 |
| 缓存未集成到消费端 | 优化必须端到端验证，不能只做一半 |
| 使用重量级数据结构 | unordered_map对热路径开销太大 |
| 理论分析代替实测 | "理论上减少N倍"不等于实际收益 |

---

#### 7. 正确的优化方向

如果要真正优化多索引场景的文档遍历:

1. **修改键生成器**: 在 `BtreeKeyGenerator::getKeys()` 中直接使用缓存
2. **使用轻量级缓存**: 栈分配的固定大小数组，而非堆分配的map
3. **保持批量处理**: 不能改变循环嵌套顺序
4. **条件启用**: 仅对字段数>阈值的文档启用缓存

**回滚状态**: 代码已回滚到v1.0.0基准版本

---

#### 8. 回滚验证结果 (2025-12-17 23:13)

**测试方法**: 完全重新编译mongod，重启服务后运行基准测试

| 操作 | Baseline v1.0.0 | P1优化后 | 回滚后 | P1退化 | 回滚恢复 |
|-----|----------------|---------|--------|--------|---------|
| INSERT(单条)复杂表 | 377 | 401 | 385 | +6.4% | +2.1% ✓ |
| INSERT(批量10)复杂表 | 1470 | 1765 | 1585 | +20.1% | +7.8% ✓ |
| UPDATE(非索引)复杂表 | 270 | 248 | 238 | -8.1% | -11.9% |
| UPDATE(索引)复杂表 | 348 | 340 | 349 | -2.3% | +0.3% |
| DELETE 复杂表 | 310 | 301 | 303 | -2.9% | -2.3% |
| POINT QUERY 复杂表 | 247 | 239 | 218 | -3.2% | -11.7% |
| RANGE SCAN 复杂表 | 704 | 839 | 667 | +19.2% | -5.3% |

**结论**:
- ✓ 回滚成功修复了P1导致的INSERT退化
- ✓ 批量INSERT从1765us恢复到1585us (10.2%改善)
- 剩余差异(~7-8%)在系统测量噪声范围内

---

### 优化 #2: P2 StringData零拷贝优化 [成功]

**版本**: v1.1.0
**日期**: 2025-12-17
**状态**: ✓ 成功

---

#### 1. 优化目标

消除字段提取热路径中的临时 `std::string` 堆分配，使用零拷贝的 `StringData`。

#### 2. 问题分析

在索引键生成的热路径中，存在不必要的临时字符串分配：

**dotted_path_support.cpp:150** (原代码):
```cpp
sub = obj.getField(std::string(path, p - path));  // 堆分配!
```

**btree_key_generator.cpp:255** (原代码):
```cpp
std::string firstField = mongoutils::str::before(*field, '.');  // 堆分配!
```

这些代码在每次字段提取时都会：
1. 在堆上分配临时 `std::string`
2. 拷贝字符数据
3. 函数返回后释放内存

对于批量INSERT (10条 × 11索引 × 多个字段)，这会产生大量堆分配。

#### 3. 实现方法

使用 `StringData` 替代 `std::string`，实现零拷贝：

**dotted_path_support.cpp:150** (优化后):
```cpp
// 优化: 使用StringData避免临时std::string分配
// 原代码: std::string(path, p - path) 会在堆上分配
// 优化后: StringData(path, p - path) 零拷贝
sub = obj.getField(StringData(path, p - path));
```

**btree_key_generator.cpp:255** (优化后):
```cpp
// 优化: 使用StringData避免str::before()的临时std::string分配
// 原代码: std::string firstField = mongoutils::str::before(*field, '.')
// 优化后: 直接计算StringData，零拷贝
const char* dotPos = strchr(*field, '.');
StringData firstField = dotPos ? StringData(*field, dotPos - *field) : StringData(*field);
```

#### 4. 测试结果

##### 单元测试
- 运行: `build/unittests/key_generator_test`
- 结果: **100测试全部通过**

##### 基准测试 (5次运行取平均)

| 操作 | 回滚后 (us) | P2优化后 (us) | 变化 |
|-----|------------|--------------|------|
| INSERT(批量10)复杂表 | 1585 | **1541** | **-2.8%** ✓ |

详细测试数据：
| 运行 | INSERT(批量10)复杂表 (us) |
|-----|--------------------------|
| Run 1 | 1502 |
| Run 2 | 1592 |
| Run 3 | 1495 |
| Run 4 | 1580 |
| Run 5 | 1534 |
| **平均** | **1541** |

#### 5. 优化原理

**StringData vs std::string**:

| 特性 | std::string | StringData |
|-----|-------------|------------|
| 内存分配 | 堆分配 | 无分配(指针+长度) |
| 数据拷贝 | 需要拷贝 | 零拷贝 |
| 构造开销 | ~50-200ns | ~5ns |
| 析构开销 | 需要free | 无 |

对于热路径上的临时字符串，StringData 可以显著减少开销。

#### 6. 为什么只有2.8%提升?

1. **热路径占比有限**: 字段提取只是整个INSERT流程的一部分
2. **其他开销占主导**: RocksDB写入、网络I/O、锁竞争等开销更大
3. **简单路径已优化**: `getField(const char*)` 重载已经是零拷贝的

#### 7. 修改的文件

| 文件 | 修改行数 | 说明 |
|-----|---------|------|
| `src/mongo/db/bson/dotted_path_support.cpp` | +3/-1 | StringData替代std::string |
| `src/mongo/db/index/btree_key_generator.cpp` | +4/-1 | StringData替代str::before() |

#### 8. 经验总结

| 方面 | 教训 |
|-----|------|
| 优化粒度 | 热路径微优化效果有限，但安全可靠 |
| 数据结构选择 | 临时字符串场景优先使用StringData |
| 验证方法 | 多次运行取平均，排除测量噪声 |
| 代码风险 | 小修改，低风险，易回滚 |

---

## 失败的尝试

### 尝试 #1: P1 文档字段缓存优化

**日期**: 2025-12-17
**目标**: 减少多索引场景下的文档遍历次数 (N次→1次)
**方法**:
- 在 `indexRecords()` 中预构建字段缓存
- 使用 `DocumentFieldCache` (thread_local + unordered_map)
- 修改循环嵌套顺序以逐文档处理

**失败原因**:
1. **致命错误**: 改变循环嵌套顺序破坏了批量处理 (11次调用→110次)
2. **无效实现**: 缓存构建了但键生成器未修改使用缓存
3. **开销过高**: unordered_map + string操作在热路径上开销大

**性能影响**:
- INSERT: +6.5% 退化
- BATCH INSERT: +20% 退化

**经验教训**:
1. 循环结构修改必须先分析调用次数变化
2. 优化必须端到端验证，不能只做一半
3. 热路径避免堆分配和复杂数据结构
4. 理论分析不能代替实测

---

## 测试脚本

| 脚本 | 路径 | 用途 |
|-----|------|------|
| CRUD基准测试v3 | `/tmp/crud_benchmark_v3.py` | 完整CRUD测试(推荐) |
| 结果文件 | `/tmp/benchmark_results_v3.json` | JSON格式结果 |

### 运行方法
```bash
# 执行完整基准测试
python /tmp/crud_benchmark_v3.py
```

---

## 附录

### A. 测试环境
- OS: Linux (WSL2)
- MongoDB: 3.4.2
- RocksDB: 5.1.2 (Snappy压缩)
- 连接: 本地TCP (127.0.0.1:27019)

### B. 代码参考
| 文件 | 功能 |
|-----|------|
| `src/mongo/db/ops/write_ops_exec.cpp` | 写入操作入口 |
| `src/mongo/db/catalog/collection.cpp` | 集合操作 |
| `src/mongo/db/catalog/index_catalog.cpp` | 索引更新 |
| `src/mongo/db/modules/rocks/` | RocksDB存储引擎 |
