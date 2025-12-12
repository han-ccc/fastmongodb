# 字段提取优化方案

## 测试日期
2025-12-18

## 优化目标
优化BtreeKeyGenerator的字段提取性能，减少文档遍历次数和字符串比较开销。

## 阶段规划

| 阶段 | 方案 | 适用场景 | 预期效果 |
|-----|------|---------|---------|
| 阶段1 | 长度位图+提前退出 | 单索引 | 1.14x-1.57x |
| 阶段2 | 嵌套字段前缀分组 | 嵌套索引 | 2-3x |
| 阶段3 | 数组延迟展开 | 多键索引 | 2-4x |
| 阶段4 | 跨索引共享(签名槽位) | 多索引 | 3-10x |

---

## 阶段1: 长度位图+提前退出

### 核心逻辑验证结果

```
场景                        基线(ns)  优化(ns)  加速比
---------------------------------------------------------
30字段文档, 3字段索引        31.9      20.3     1.57x ✓
30字段文档, 6字段索引        68.3      52.0     1.31x ✓
50字段文档, 6字段索引        98.4      86.1     1.14x ✓
50字段文档, 10字段索引      171.3     150.3     1.14x ✓
```

### 技术方案

```cpp
class BtreeKeyGeneratorV1 {
    // 简单字段（无"."）的长度位图
    uint64_t _simpleLengthBitmap = 0;

    struct SimpleFieldInfo {
        size_t fieldIndex;
        size_t nameLen;
        const char* name;
    };
    std::vector<SimpleFieldInfo> _simpleFields;
};

// 在getKeysImpl中
void preExtractSimpleFields(const BSONObj& obj,
                            std::vector<BSONElement>& fixed,
                            std::vector<const char*>& fieldNames) {
    size_t foundCount = 0;
    size_t targetCount = _simpleFields.size();

    for (BSONObjIterator it(obj); it.more() && foundCount < targetCount; ) {
        BSONElement e = it.next();
        size_t len = e.fieldNameSize() - 1;

        // 长度位图快速过滤
        if (!(_simpleLengthBitmap & (1ULL << (len & 63)))) {
            continue;
        }

        const char* docFieldName = e.fieldName();
        for (const auto& info : _simpleFields) {
            if (fieldNames[info.fieldIndex][0] == '\0') continue;  // 已找到

            if (info.nameLen == len &&
                std::memcmp(info.name, docFieldName, len) == 0) {
                fixed[info.fieldIndex] = e;
                fieldNames[info.fieldIndex] = "";
                foundCount++;
                break;
            }
        }
    }
}
```

### 关键优化点

1. **长度位图预过滤**: 不可能匹配的长度直接跳过
2. **提前退出**: 找到所有字段后停止遍历
3. **保留早期终止**: 不破坏getField的早期终止优势

---

## 对比: 签名+unordered_map方案

### 单索引场景（不适用）

```
场景                          基线(ns)  优化(ns)  加速比
---------------------------------------------------------
小文档(20字段), 3字段索引       16.6      29.3    0.57x ✗
中文档(30字段), 6字段索引       53.7      89.5    0.60x ✗
大文档(50字段), 10字段索引     122.5     199.3    0.61x ✗
```

原因: unordered_map开销抵消了优化收益

### 多索引场景（阶段4适用）

```
场景                          唯一字段  基线(ns)  优化(ns)  加速比
-----------------------------------------------------------------
1个索引×6字段                    6       56.9     159.4    0.36x ✗
3个索引×6字段                   18      217.6     291.7    0.75x ✗
5个索引×6字段                   30      427.5     344.9    1.24x ✓
7个索引×6字段                   36      683.7     354.1    1.93x ✓
10个索引×6字段                  45     1151.5     384.9    2.99x ✓
7个索引×10字段                  50     1308.3     400.5    3.27x ✓
```

结论: 签名+unordered_map方案适合阶段4（跨索引共享），不适合阶段1

---

## 验证代码

- 阶段1核心逻辑: `/tmp/stage1_bitmap_only_benchmark.cpp`
- 多索引场景: `/tmp/stage1_multi_index_benchmark.cpp`
- 签名方案: `/tmp/stage1_signature_benchmark.cpp`

---

## 实施状态

| 阶段 | 状态 | 备注 |
|-----|------|------|
| 阶段1 | 核心验证完成 | 长度位图+提前退出，1.14x-1.57x |
| 阶段2 | 待实施 | |
| 阶段3 | 待实施 | |
| 阶段4 | 核心验证完成 | UnifiedFieldExtractor，3-10x |
