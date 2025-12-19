# 微基准测试目录

本文档记录所有性能优化的微基准测试用例及结果。

## 目录

1. [BSONElement::size() 优化](#1-bsonelement-size-优化)
2. [分片键提取优化](#2-分片键提取优化)
3. [端到端基准测试](#3-端到端基准测试)

---

## 1. BSONElement::size() 优化

### 优化内容

| 版本 | 实现方式 | Commit |
|------|---------|--------|
| v2.0.0 | 64行 switch 语句 | `04609e58010` |
| v2.3.0 | 查表 + 位掩码分组 | `73f3eb9deba` |

### 代码变更

**v2.0.0 (switch)**:
```cpp
switch (type()) {
    case NumberInt: x = 4; break;
    case NumberDouble: x = 8; break;
    case String: x = valuestrsize() + 4; break;
    // ... 64行
}
```

**v2.3.0 (lookup table)**:
```cpp
static constexpr uint8_t kFixedSizes[32] = {0,8,0,0,...};

if (t < 32 && kFixedSizes[t] != 0) {
    x = kFixedSizes[t];  // O(1) 查表
} else {
    // 位掩码分组处理变长类型
    uint32_t typeBit = 1u << t;
    if (typeBit & kStringSizeMask) x = valuestrsize() + 4;
    // ...
}
```

### 微基准测试

**测试文件**: `/tmp/size_by_type.cpp`

**编译运行**:
```bash
g++ -O2 -std=c++11 /tmp/size_by_type.cpp -o /tmp/size_by_type && /tmp/size_by_type
```

### 按类型测试结果 (2025-12-21)

#### 固定大小类型

| Type | v2.0.0 (switch) | v2.3.0 (lookup) | Diff | Winner |
|------|-----------------|-----------------|------|--------|
| NumberInt | 2.79 ns | 2.44 ns | **-12.8%** | LOOKUP |
| NumberDouble | 2.77 ns | 2.40 ns | **-13.3%** | LOOKUP |
| NumberLong | 2.77 ns | 2.39 ns | **-13.7%** | LOOKUP |
| Bool | 2.76 ns | 2.41 ns | **-12.6%** | LOOKUP |
| Date | 2.77 ns | 2.41 ns | **-12.9%** | LOOKUP |
| jstOID | 2.77 ns | 2.40 ns | **-13.6%** | LOOKUP |
| Timestamp | 2.82 ns | 2.43 ns | **-13.8%** | LOOKUP |
| NumberDecimal | 2.80 ns | 2.43 ns | **-13.3%** | LOOKUP |

#### 变长类型

| Type | v2.0.0 (switch) | v2.3.0 (lookup) | Diff | Winner |
|------|-----------------|-----------------|------|--------|
| String(20) | 2.81 ns | 2.77 ns | -1.3% | LOOKUP |
| String(100) | 2.77 ns | 2.77 ns | -0.1% | TIE |
| Object(50) | 2.76 ns | 2.60 ns | **-6.1%** | LOOKUP |
| Array(50) | 2.81 ns | 2.61 ns | **-7.3%** | LOOKUP |
| BinData(32) | 2.78 ns | 2.64 ns | **-5.1%** | LOOKUP |
| Code(30) | 2.82 ns | 2.81 ns | -0.2% | TIE |
| CodeWScope | 2.81 ns | 2.64 ns | **-6.1%** | LOOKUP |

#### 零大小类型

| Type | v2.0.0 (switch) | v2.3.0 (lookup) | Diff | Winner |
|------|-----------------|-----------------|------|--------|
| EOO | 2.60 ns | 2.60 ns | -0.2% | TIE |
| Null | 2.59 ns | 2.59 ns | +0.0% | TIE |
| Undefined | 2.58 ns | 2.60 ns | +0.9% | TIE |

### 综合场景测试结果

| 场景 | v2.0.0 | v2.3.0 | Diff |
|------|--------|--------|------|
| fixed (全固定类型) | 3.35 ns | 2.61 ns | **-22.0%** |
| variable (全变长类型) | 3.27 ns | 2.80 ns | **-14.2%** |
| mixed (60%固定+40%变长) | 3.35 ns | 2.63 ns | **-21.5%** |

### 不同优化级别对比

| 优化级别 | 场景 | switch | lookup | Diff |
|---------|------|--------|--------|------|
| -O0 | fixed | 8.45 ns | 7.92 ns | -6.3% |
| -O0 | variable | 8.34 ns | **9.54 ns** | **+14.4%** |
| -O0 | mixed | 8.51 ns | 8.41 ns | -1.2% |
| **-O2** | fixed | 3.35 ns | 2.61 ns | **-22%** |
| **-O2** | variable | 3.27 ns | 2.80 ns | **-14%** |
| **-O2** | mixed | 3.35 ns | 2.63 ns | **-22%** |
| -O3 | fixed | 3.50 ns | 1.72 ns | **-51%** |
| -O3 | variable | 3.33 ns | 2.21 ns | **-34%** |
| -O3 | mixed | 3.49 ns | 1.83 ns | **-48%** |

### 结论

- **固定类型**: lookup 快 **~13%** (-O2)
- **变长类型**: lookup 快 **5-7%** (Object/Array), 持平 (String)
- **综合场景**: lookup 快 **14-22%** (-O2)
- **无任何类型 lookup 更慢** (在 -O2 优化级别下)

---

## 2. 分片键提取优化

### 优化内容

| 版本 | 实现方式 | Commit |
|------|---------|--------|
| v2.0.0 | 总是用 MatchableDocument | `04609e58010` |
| v2.3.0 | 顶级字段快速路径 | `1194148c605` |

### 代码变更

**v2.0.0**:
```cpp
BSONObj ShardKeyPattern::extractShardKeyFromDoc(const BSONObj& doc) const {
    BSONMatchableDocument matchable(doc);  // 构造开销
    return extractShardKeyFromMatchable(matchable);  // ElementPath 遍历
}
```

**v2.3.0**:
```cpp
BSONObj ShardKeyPattern::extractShardKeyFromDoc(const BSONObj& doc) const {
    // 快速路径: 所有字段都是顶级
    if (_allTopLevel) {
        BSONObjBuilder keyBuilder(64);  // 预分配
        for (auto& field : _keyPatternPaths) {
            BSONElement el = doc[field->dottedField()];  // O(1) 哈希查找
            keyBuilder.appendAs(el, patternEl.fieldName());
        }
        return keyBuilder.obj();
    }
    // 回退到 MatchableDocument
    return extractShardKeyFromMatchable(BSONMatchableDocument(doc));
}
```

### 微基准测试

**测试文件**: `/tmp/shard_key_benchmark.cpp`

**编译运行**:
```bash
g++ -O2 -std=c++11 /tmp/shard_key_benchmark.cpp -o /tmp/shard_key_benchmark && /tmp/shard_key_benchmark
```

### 测试结果 (2025-12-21)

#### 顶级字段场景 (快速路径生效)

| 场景 | v2.0.0 | v2.3.0 | Diff | Winner |
|------|--------|--------|------|--------|
| 1 key, 5 fields | 31.3 ns | 30.1 ns | **-3.8%** | v2.3.0 |
| 1 key, 10 fields | 34.4 ns | 32.1 ns | **-6.9%** | v2.3.0 |
| 1 key, 20 fields | 40.0 ns | 34.4 ns | **-14.2%** | v2.3.0 |
| 3 keys, 5 fields | 97.2 ns | 76.6 ns | **-21.2%** | v2.3.0 |
| 3 keys, 10 fields | 104.6 ns | 81.2 ns | **-22.4%** | v2.3.0 |
| 3 keys, 20 fields | 128.6 ns | 93.2 ns | **-27.5%** | v2.3.0 |

#### 嵌套字段场景 (回退路径)

| 场景 | v2.0.0 | v2.3.0 | Diff | Winner |
|------|--------|--------|------|--------|
| 1 key (dotted) | 5.8 ns | 6.0 ns | +2.6% | TIE |
| 3 keys (dotted) | 6.2 ns | 5.9 ns | -5.4% | v2.3.0 |

### 结论

- **顶级字段 + 复合键**: v2.3.0 快 **21-28%**
- **文档字段越多，优势越大**: 20 fields 时达到最大收益
- **嵌套字段**: 无退化，回退路径正常工作

---

## 3. 端到端基准测试

### 测试方法: F方案

```python
def f_scheme_reset():
    drop("simple_docs")
    drop("complex_docs")
    fsync()
    sleep(3)  # 等待RocksDB稳定
```

### 测试用例稳定性分组

| 稳定等级 | 波动率 | 误差阈值 | 测试用例 |
|----------|--------|----------|----------|
| 极稳定 | <5% | ±3% | INSERT(batch), QUERY($or), QUERY(range scan), QUERY($in), QUERY(partial key) |
| 中等稳定 | 5-10% | ±5% | UPDATE(single), QUERY($and), UPDATE(indexed), DELETE(by index) |
| 不稳定 | >10% | 仅参考 | DELETE(multi), DELETE(single), INSERT(single), QUERY(exact key), QUERY($gt) |

### v2.0.0 vs v2.3.0 端到端对比

#### 简单表 (无索引)

| 测试用例 | v2.0.0 | v2.3.0 | 变化 | 稳定等级 |
|----------|--------|--------|------|----------|
| INSERT (single) | 496μs | 476μs | -4.0% | 不稳定 |
| INSERT (batch 10) | 557μs | 565μs | +1.4% | 极稳定 |
| QUERY (partial key) | 594μs | 580μs | -2.4% | 极稳定 |
| QUERY ($in) | 577μs | 595μs | +3.1% | 极稳定 |
| QUERY ($or) | 608μs | 607μs | -0.2% | 极稳定 |
| UPDATE (single) | 528μs | 514μs | -2.7% | 中等 |

#### 复杂表 (5索引)

| 测试用例 | v2.0.0 | v2.3.0 | 变化 | 稳定等级 |
|----------|--------|--------|------|----------|
| INSERT (batch 10) | 639μs | 664μs | +3.9% | 极稳定 |
| QUERY (partial key) | 603μs | 608μs | +0.8% | 极稳定 |
| QUERY ($or) | 620μs | 644μs | +3.9% | 极稳定 |
| QUERY (range scan) | 718μs | 716μs | -0.3% | 极稳定 |
| UPDATE (indexed) | 548μs | 515μs | **-6.0%** | 中等 |
| UPDATE (multi idx) | 553μs | 526μs | **-4.9%** | 中等 |

### 基线文件位置

| 版本 | 文件 | Commit | Runs |
|------|------|--------|------|
| v2.0.0 | `/tmp/baseline_v2.0.0.json` | `04609e58010` | 2 |
| v2.3.0 | `/tmp/baseline_v2.3.0.json` | `73f3eb9deba` | 4 |

---

## 附录: 微基准测试源码

### size_by_type.cpp

位置: `/tmp/size_by_type.cpp`

测试 BSONElement::size() 各类型性能。

### shard_key_benchmark.cpp

位置: `/tmp/shard_key_benchmark.cpp`

测试分片键提取性能。

### size_benchmark.cpp

位置: `/tmp/size_benchmark.cpp`

测试 size() 综合场景性能。
