# BSON性能优化计划

## 执行摘要

基于代码分析和基准测试，本计划旨在实现BSON操作**整体20-40%性能提升**。

---

## 阶段概览

| 阶段 | 优化内容 | 预期收益 | 风险 | 工作量 |
|------|----------|----------|------|--------|
| **阶段1** | 内存分配优化 | 15-25% | 低 | 2天 |
| **阶段2** | 核心路径优化 | 10-20% | 中 | 2天 |
| **阶段3** | 迭代器与遍历优化 | 5-15% | 低 | 1天 |
| **阶段4** | 序列化优化 | 5-10% | 中 | 1天 |

**已完成优化**:
- BSONFieldIndex字段索引 (47x大对象查找提升)
- fastCompareElementValues (1.48x长整数比较提升)

---

## 阶段1: 内存分配优化 (高优先级)

### 1.1 BufBuilder自适应增长策略

**文件**: `src/mongo/bson/util/builder.h:322-334`

**现状问题**:
```cpp
void grow_reallocate(int minSize) {
    int a = 64;
    while (a < minSize)
        a = a * 2;  // 简单2倍增长 - 大对象时浪费内存
    // ...
}
```

**优化方案**:
```cpp
void grow_reallocate(int minSize) {
    int a = size;  // 从当前大小开始

    if (a < 4096) {
        // 小缓冲: 2倍增长，快速达到工作大小
        a = std::max(64, a * 2);
    } else if (a < 1024 * 1024) {
        // 中缓冲(4KB-1MB): 1.5倍增长，平衡速度和内存
        a = a + (a >> 1);
    } else {
        // 大缓冲(>1MB): 固定增量，避免过度分配
        a = a + (1024 * 1024);
    }

    // 确保达到minSize
    while (a < minSize) {
        if (a < 1024 * 1024) a = a + (a >> 1);
        else a += (1024 * 1024);
    }

    if (a > BufferMaxSize) { /* 错误处理 */ }
    _buf.realloc(a);
    size = a;
}
```

**预期收益**: 大对象内存使用减少20-30%，减少realloc次数

---

### 1.2 StackAllocator栈大小优化

**文件**: `src/mongo/bson/util/builder.h:103-140`

**现状问题**:
```cpp
enum { SZ = 512 };  // 512字节太小，常见对象会触发堆分配
```

**优化方案**:
```cpp
// 基于工作负载特征选择栈大小
#if defined(MONGO_CONFIG_OPTIMIZED_BUILD)
    enum { SZ = 1024 };  // 优化构建使用1KB
#elif defined(MONGO_CONFIG_DEBUG_BUILD)
    enum { SZ = 512 };   // 调试构建保守
#else
    enum { SZ = 1024 };  // 默认1KB
#endif
```

**预期收益**: 堆分配减少30-40%

---

### 1.3 appendStr小字符串快速路径

**文件**: `src/mongo/bson/util/builder.h:261-264`

**现状问题**:
```cpp
void appendStr(StringData str, bool includeEndingNull = true) {
    const int len = str.size() + (includeEndingNull ? 1 : 0);
    str.copyTo(grow(len), includeEndingNull);  // 所有字符串走同一路径
}
```

**优化方案**:
```cpp
void appendStr(StringData str, bool includeEndingNull = true) {
    const int len = str.size() + (includeEndingNull ? 1 : 0);
    char* dest = grow(len);

    // 快速路径: 小字符串直接内联复制
    if (len <= 32) {
        const char* src = str.rawData();
        // 手动展开小拷贝，避免函数调用
        switch (str.size()) {
            case 0: break;
            case 1: dest[0] = src[0]; break;
            case 2: memcpy(dest, src, 2); break;
            case 3: memcpy(dest, src, 3); break;
            case 4: memcpy(dest, src, 4); break;
            default: memcpy(dest, src, str.size()); break;
        }
        if (includeEndingNull) dest[str.size()] = '\0';
    } else {
        str.copyTo(dest, includeEndingNull);
    }
}
```

**预期收益**: 字段名追加速度提升2-3倍

---

## 阶段2: 核心路径优化 (高优先级)

### 2.1 集成快速比较到woCompare

**文件**: `src/mongo/bson/bsonobj.cpp:104-137`

**现状问题**:
```cpp
int x = l.woCompare(r, considerFieldName, comparator);  // 通用路径
```

**优化方案**:
```cpp
// 在bsonobj.cpp顶部添加
#include "mongo/bson/bson_fast_compare.h"

// 修改woCompare循环
int x;
// 快速路径：同类型数值比较
if (!considerFieldName &&
    comparator == nullptr &&
    canUseFastNumericCompare(l, r)) {
    x = fastCompareElementValues(l, r);
} else {
    x = l.woCompare(r, considerFieldName, comparator);
}
```

**预期收益**: 数值密集型比较提升10-20%

---

### 2.2 BSONElement::woCompare快速路径

**文件**: `src/mongo/bson/bsonelement.cpp:397-412`

**优化方案**:
```cpp
int BSONElement::woCompare(const BSONElement& e,
                           bool considerFieldName,
                           const StringData::ComparatorInterface* comparator) const {
    // 快速路径: 类型相同且不考虑字段名
    if (!considerFieldName && comparator == nullptr) {
        BSONType lt = type();
        BSONType rt = e.type();

        if (lt == rt) {
            switch (lt) {
                case NumberInt:
                    return fastCompareInts(*this, e);
                case NumberLong:
                    return fastCompareLongs(*this, e);
                case NumberDouble:
                    return fastCompareDoubles(*this, e);
                case String:
                    return fastCompareStrings(*this, e);
                default:
                    break;
            }
        }
    }

    // 原有逻辑
    int lt = (int)canonicalType();
    // ...
}
```

**预期收益**: 元素比较提升15-25%

---

### 2.3 getField优化集成

**文件**: `src/mongo/bson/bsonobj.h`

**优化方案**: 添加自动索引构建提示
```cpp
class BSONObj {
    // 添加可选的延迟索引
    mutable std::unique_ptr<BSONFieldIndex> _cachedIndex;
    mutable bool _indexAttempted = false;

public:
    // 新增：带缓存的getField
    BSONElement getFieldCached(StringData name) const {
        if (!_indexAttempted && nFields() > 10) {
            _cachedIndex = BSONFieldIndex::create(*this);
            _indexAttempted = true;
        }
        if (_cachedIndex) {
            int offset = _cachedIndex->findFieldOffset(name);
            if (offset >= 0) return BSONElement(objdata() + offset);
            return BSONElement();
        }
        return getField(name);
    }
};
```

**注意**: 这是可选的侵入式优化，需要评估内存开销

---

## 阶段3: 迭代器与遍历优化 (中优先级)

### 3.1 BSONObjIterator预计算优化

**文件**: `src/mongo/bson/bsonobj.h`

**现状问题**: 每次next()需要解析元素大小

**优化方案**: 添加批量迭代器
```cpp
// 新类：预计算大小的迭代器
class BSONObjIteratorFast {
public:
    explicit BSONObjIteratorFast(const BSONObj& obj) : _obj(obj) {
        // 第一次遍历预计算所有偏移量
        _offsets.reserve(16);  // 预留常见大小
        const char* p = _obj.objdata() + 4;
        const char* end = _obj.objdata() + _obj.objsize() - 1;
        while (p < end && *p) {
            _offsets.push_back(p - _obj.objdata());
            BSONElement e(p);
            p += e.size();
        }
        _current = 0;
    }

    bool more() const { return _current < _offsets.size(); }

    BSONElement next() {
        if (_current >= _offsets.size()) return BSONElement();
        return BSONElement(_obj.objdata() + _offsets[_current++]);
    }

    // 随机访问
    BSONElement at(size_t idx) const {
        if (idx >= _offsets.size()) return BSONElement();
        return BSONElement(_obj.objdata() + _offsets[idx]);
    }

    size_t count() const { return _offsets.size(); }

private:
    const BSONObj& _obj;
    std::vector<int> _offsets;
    size_t _current;
};
```

**预期收益**: 多次遍历场景提升30-50%

---

### 3.2 nFields()缓存

**文件**: `src/mongo/bson/bsonobj.h`

**现状问题**: 每次nFields()都要遍历整个对象

**优化方案**:
```cpp
class BSONObj {
    mutable int _cachedNFields = -1;  // -1表示未计算

public:
    int nFields() const {
        if (_cachedNFields >= 0) return _cachedNFields;
        int n = 0;
        BSONObjIterator i(*this);
        while (i.more()) { i.next(); n++; }
        _cachedNFields = n;
        return n;
    }

    // 重置缓存（如果对象被修改）
    void invalidateCache() const { _cachedNFields = -1; }
};
```

**预期收益**: 重复调用nFields()时O(1)

---

## 阶段4: 序列化优化 (中优先级)

### 4.1 BSONObjBuilder预大小估算

**文件**: `src/mongo/bson/bsonobjbuilder.h`

**优化方案**:
```cpp
class BSONObjBuilder {
public:
    // 基于模板对象估算大小
    static int estimateSize(const BSONObj& templateObj) {
        return templateObj.objsize() + 64;  // 预留扩展空间
    }

    // 基于字段数估算
    static int estimateSize(int numFields, int avgFieldNameLen = 8, int avgValueSize = 16) {
        return 4 + numFields * (1 + avgFieldNameLen + 1 + avgValueSize) + 1;
    }

    // 使用估算构造
    explicit BSONObjBuilder(const BSONObj& templateObj)
        : _b(estimateSize(templateObj)) {}
};
```

---

### 4.2 appendElements零拷贝优化

**当前**: 已经使用memcpy，相当高效

**可选优化**: 对于仅追加场景，考虑视图模式
```cpp
class BSONObjBuilderView {
    // 记录多个源对象，延迟合并
    std::vector<const BSONObj*> _sources;

    BSONObj finalize() {
        // 计算总大小，一次分配，一次复制
        int totalSize = 4;
        for (auto* src : _sources) totalSize += src->objsize() - 5;
        totalSize += 1;

        // 单次分配+复制
        // ...
    }
};
```

---

## 基准测试计划

### 测试用例扩展

```cpp
// 新增测试用例 (bson_benchmark.cpp)

// 测试1: BufBuilder增长效率
TEST(BSONBenchmark, BufBuilderGrowth) {
    // 测试小、中、大对象的构建性能
}

// 测试2: StackBufBuilder溢出率
TEST(BSONBenchmark, StackBufOverflow) {
    // 统计触发堆分配的频率
}

// 测试3: woCompare集成快速路径
TEST(BSONBenchmark, WoCompareFastPath) {
    // 对比启用/禁用快速路径的性能
}

// 测试4: 迭代器优化
TEST(BSONBenchmark, IteratorFast) {
    // 对比标准迭代器和预计算迭代器
}
```

---

## 实施顺序

### 第一批（立即实施，低风险高回报）

| 序号 | 任务 | 文件 | 代码行数 |
|------|------|------|----------|
| 1 | StackAllocator大小512→1024 | builder.h | ~5行 |
| 2 | appendStr小字符串优化 | builder.h | ~20行 |
| 3 | 集成fast_compare到woCompare | bsonobj.cpp | ~15行 |

### 第二批（一周内，中等风险）

| 序号 | 任务 | 文件 | 代码行数 |
|------|------|------|----------|
| 4 | BufBuilder自适应增长 | builder.h | ~30行 |
| 5 | BSONElement快速路径 | bsonelement.cpp | ~40行 |
| 6 | nFields缓存 | bsonobj.h | ~15行 |

### 第三批（后续迭代，需充分测试）

| 序号 | 任务 | 文件 | 代码行数 |
|------|------|------|----------|
| 7 | BSONObjIteratorFast | 新文件 | ~100行 |
| 8 | getField自动索引 | bsonobj.h | ~30行 |
| 9 | BSONObjBuilder预估大小 | bsonobjbuilder.h | ~25行 |

---

## 风险评估

| 优化项 | 风险等级 | 风险说明 | 缓解措施 |
|--------|----------|----------|----------|
| StackAllocator大小 | 低 | 增加栈使用 | 递归深度测试 |
| appendStr优化 | 低 | 边界条件 | 充分单元测试 |
| BufBuilder增长 | 中 | 内存行为变化 | A/B测试对比 |
| woCompare集成 | 中 | 语义一致性 | 全量对比测试 |
| 字段缓存 | 高 | 内存开销、并发安全 | 可选开启、线程安全审计 |

---

## 预期成果

### 性能提升汇总

| 场景 | 当前基线 | 优化后目标 | 提升比例 |
|------|----------|------------|----------|
| 小对象构建 | 40 ns | 30 ns | 25% |
| 大对象构建 | 700 ns | 550 ns | 20% |
| 字段查找(大对象) | 400 ns | 8 ns | **4700%** (已完成) |
| 对象比较(数值) | 150 ns | 100 ns | 33% |
| 对象遍历 | 230 ns | 180 ns | 20% |

### 内存使用改善

| 场景 | 当前 | 优化后 | 改善 |
|------|------|--------|------|
| 小对象堆分配率 | 60% | 30% | 50%减少 |
| 大对象内存浪费 | 50% | 20% | 60%减少 |

---

## 下一步行动

1. **立即开始**: 实施阶段1第一批优化
2. **创建分支**: `feature/bson-perf-phase1`
3. **测试覆盖**: 扩展bson_benchmark测试用例
4. **持续监控**: 每个commit后运行基准测试
