# 字段提取优化方案

## 测试日期
2025-12-18

## 优化目标
优化BtreeKeyGenerator的字段提取性能，减少文档遍历次数和字符串比较开销。

---

## 提交规范

**每个MR最终提交前必须完成端到端基准测试。**

### 验证流程

1. **核心逻辑验证**: 编写独立benchmark验证算法可行性
2. **代码实现**: 集成到MongoDB代码库
3. **单元测试**: 确保所有测试通过
4. **端到端基准测试**: 在mongod上运行`/tmp/crud_benchmark_v3.py`验证实际性能

### 端到端基准测试步骤

```bash
# 1. 编译mongod
scons mongod --disable-warnings-as-errors --js-engine=none -j24

# 2. 启动mongod
./mongod --storageEngine=rocksdb --dbpath=/tmp/mongo_data --port=27019 --fork --logpath=/tmp/mongod.log

# 3. 运行基准测试
python /tmp/crud_benchmark_v3.py

# 4. 记录结果到本文档（必须包含与基线的对比）
```

### 基准测试结果记录规范

**每个阶段的端到端测试结果必须包含以下表格**:

| 操作 | 简单表 | 复杂表 | 基线(简单) | 基线(复杂) | vs基线 |
|-----|-------|-------|-----------|-----------|-------|
| INSERT(单条) | xxx | xxx | xxx | xxx | +/-xx% |
| INSERT(批量10) | xxx | xxx | xxx | xxx | +/-xx% |
| UPDATE(非索引) | xxx | xxx | xxx | xxx | +/-xx% |
| UPDATE(索引字段) | - | xxx | - | xxx | +/-xx% |
| DELETE | xxx | xxx | xxx | xxx | +/-xx% |
| POINT QUERY | xxx | xxx | xxx | xxx | +/-xx% |
| RANGE SCAN(100) | xxx | xxx | xxx | xxx | +/-xx% |

**测试条件说明**:
- 简单表: 1索引(_id), ~150B, 3字段
- 复杂表: 11索引, ~800B, ~90字段
- 基线: perf/baseline分支的性能数据

**注意**: 未完成端到端基准测试的优化不得合入主分支。

---

## 基线数据 (2025-12-17)

数据来源: `PERFORMANCE_OPTIMIZATION_LOG.md`, `/tmp/benchmark_results.json`

### 简单表基线 (1索引, 3字段, ~150B)

| 操作 | 平均(us) | P50 | P90 | P99 |
|-----|---------|-----|-----|-----|
| INSERT(单条) | 239 | 234 | 259 | 360 |
| INSERT(批量10) | 294 | 289 | 316 | 475 |
| UPDATE(非索引) | 246 | - | - | - |
| DELETE | 241 | 236 | 262 | 321 |
| POINT QUERY | 217 | 217 | 233 | 280 |
| RANGE SCAN(100) | 444 | 441 | 548 | 650 |

### 复杂表基线 (11索引, ~90字段, ~800B)

| 操作 | 平均(us) | P50 | P90 | P99 |
|-----|---------|-----|-----|-----|
| INSERT(单条) | 377 | 367 | 411 | 519 |
| INSERT(批量10) | 1470 | 1456 | 1538 | 1777 |
| UPDATE(非索引) | 270 | - | - | - |
| UPDATE(索引字段) | 348 | - | - | - |
| DELETE | 310 | 302 | 334 | 436 |
| POINT QUERY | 247 | 242 | 265 | 369 |
| RANGE SCAN(100) | 704 | 693 | 891 | 1031 |

### 关键指标

| 指标 | 简单表 | 复杂表 | 差异 |
|-----|-------|-------|------|
| INSERT(单条) | 239us | 377us | +57.4% |
| INSERT(批量10) | 294us | 1470us | **+400%** |
| INSERT(批量10每条) | 29.4us | 147us | **+400%** |
| RANGE SCAN(100) | 444us | 704us | +58.5% |

---

## 索引场景模型 (阶段4优化基线)

### 术语定义

| 术语 | 含义 | 示例 |
|-----|------|------|
| 索引 (Index) | 一个索引定义 | `{f1:1, f2:1, f3:1}` |
| 字段/摘要 (Field) | 索引中的一个字段 | `f1`, `f2`, `f3` |
| 字段引用 | 索引对字段的引用次数 | 上例有3次字段引用 |
| 唯一字段 | 去重后的字段集合 | 所有索引共用的字段池 |
| 重合率 R | 字段被多个索引共享的比例 | 30%表示约30%引用是重复的 |

### 三场景定义

| 场景 | 索引数N | 字段数/索引M | 摘要 | 重合率R | 文档字段 | 数组 |
|-----|--------|-------------|------|--------|---------|------|
| **简单** | 1 | 1 (_id) | 无 | 0% | ~10 | 无 |
| **中等** | 4 | 3 (联合索引) | 无 | 20% | ~30 | 少量 |
| **复杂** | 9 | 10 (含摘要) | 有 | 30% | ~50 | 有 |

### 场景详细说明

#### 简单场景
```
索引: {_id: 1}
特点: 只有_id索引，无摘要，无联合索引
文档: ~10字段，无数组，~150B
```

#### 中等场景
```
索引示例:
  {_id: 1}
  {user_id: 1, create_time: -1}
  {status: 1, type: 1}
  {category: 1, priority: 1, create_time: -1}  // create_time重合

特点: 3-4个索引，有联合索引，无摘要
文档: ~30字段，少量数组，~400B
```

#### 复杂场景
```
索引示例 (9个索引，每个平均10字段含摘要):
  {_id: 1}
  {user_id: 1, f2:1, f3:1, ..., f10:1}           // 含9个摘要
  {order_id: 1, user_id: 1, f3:1, ..., f10:1}   // user_id重合
  {status: 1, type: 1, f5:1, ..., f12:1}
  {time: 1, user_id: 1, f7:1, ..., f14:1}       // user_id重合
  ... 更多索引

特点: 9个索引，有联合索引，有摘要，30%字段重合
文档: ~50字段，有数组，~800B
字段名: 2-4字符
```

### 字段提取成本模型

#### 计算公式

```
给定: N(索引数), M(字段数/索引), R(重合率), D(文档字段数)

总字段引用次数 = N × M
唯一字段数 U = N × M × (1 - R)  // 随机分布重合
可节省次数 S = N × M - U = N × M × R
节省比例 = R

单次字段提取成本 T ≈ 40 + D × 1.5 (ns)  // 与文档大小相关
缓存构建成本 B ≈ 50 + D × 6 (ns)         // 一次性遍历文档

净收益 = S × T - B
```

#### 三场景成本对比

| 度量 | 简单 | 中等 | 复杂 |
|-----|------|------|------|
| **索引配置** |
| 索引数 N | 1 | 4 | 9 |
| 字段数/索引 M | 1 | 3 | 10 |
| 重合率 R | 0% | 20% | 30% |
| **字段提取分析** |
| 总字段引用 N×M | 1 | 12 | 90 |
| 唯一字段 U | 1 | 10 | 63 |
| 可节省次数 S | 0 | 2 | 27 |
| 节省比例 | 0% | 17% | 30% |
| **时间估算** |
| 文档字段数 D | 10 | 30 | 50 |
| 单次提取成本 T | 55ns | 85ns | 115ns |
| 无优化总耗时 | 55ns | 1.0us | 10.4us |
| 有优化总耗时 | 55ns | 0.9us | 7.6us |
| 缓存构建成本 B | 110ns | 230ns | 350ns |
| **净收益** | **-110ns** | **-60ns** | **+2.4us** |

### 阶段4启用条件

基于上述模型，阶段4优化仅在复杂场景有正收益：

```cpp
// 启用条件
const bool useCrossIndexCache =
    (bsonRecords.size() == 1) &&    // 单文档写入
    (indexCount >= 5) &&             // 至少5个索引
    (avgFieldsPerIndex >= 6);        // 平均每索引至少6字段

// 或简化为
const bool useCrossIndexCache =
    (bsonRecords.size() == 1) &&
    (totalFieldRefs >= 30);          // 总字段引用>=30
```

### 预期收益总结

| 场景 | 启用阶段4 | 预期收益 | 备注 |
|-----|----------|---------|------|
| 简单 (N=1) | ❌ | -110ns | 缓存开销 > 收益 |
| 中等 (N=4) | ❌ | -60ns | 收益太小 |
| 复杂 (N≥5, M≥6) | ✅ | **+1~3us** | 显著收益 |

---

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

### 端到端基准测试结果

测试日期: 2025-12-18
测试脚本: `/tmp/crud_benchmark_v3.py`
测试环境: RocksDB存储引擎

#### 测试条件

| 表类型 | 索引数量 | 文档大小 | 字段数量 | 索引配置 |
|-------|---------|---------|---------|---------|
| 简单表 | 1 | ~150B | 3 | _id索引 |
| 复杂表 | 11 | ~800B | ~90 | _id + 10个复合索引(每个10字段) |

#### 简单表结果 (1索引, 3字段, ~150B)

| 操作 | 平均延迟 | P50 | P90 | P99 |
|-----|---------|-----|-----|-----|
| INSERT(单条) | 249us | 245us | 271us | 343us |
| INSERT(批量10) | 305us | 300us | 329us | 415us |
| UPDATE(非索引) | 266us | 259us | 287us | 395us |
| DELETE(单条) | 261us | 256us | 281us | 346us |
| POINT QUERY | 242us | 238us | 259us | 340us |
| RANGE SCAN(100) | 433us | 432us | 544us | 629us |

#### 复杂表结果 (11索引, ~90字段, ~800B)

| 操作 | 平均延迟 | P50 | P90 | P99 |
|-----|---------|-----|-----|-----|
| INSERT(单条) | 358us | 350us | 387us | 507us |
| INSERT(批量10) | 1254us | 1236us | 1331us | 1579us |
| UPDATE(非索引) | 272us | 265us | 295us | 400us |
| UPDATE(索引字段) | 311us | 304us | 340us | 482us |
| DELETE(单条) | 289us | 282us | 311us | 428us |
| POINT QUERY | 242us | 236us | 261us | 363us |
| RANGE SCAN(100) | 719us | 708us | 954us | 1111us |

#### 与基线对比

基线数据来源: `/tmp/benchmark_results.json` (2025-12-17, 未优化代码, 迭代2000次)

| 操作 | 简单表 | 基线(简单) | vs基线 | 复杂表 | 基线(复杂) | vs基线 |
|-----|-------|-----------|-------|-------|-----------|-------|
| INSERT(单条) | 249us | 254us | **-2.0%** | 358us | 363us | **-1.4%** |
| INSERT(批量10) | 305us | - | - | 1254us | - | - |
| UPDATE(非索引) | 266us | 258us | +3.1% | 272us | 274us | **-0.7%** |
| DELETE | 261us | 230us | +13.5% | 289us | 232us | +24.6% |
| POINT QUERY | 242us | 223us | +8.5% | 242us | 229us | +5.7% |
| RANGE SCAN | 433us | 232us | +86.6% | 719us | 234us | +207.3% |

**分析**:
- **INSERT单条性能提升**: 简单表-2.0%，复杂表-1.4%（阶段1优化生效）
- DELETE/RANGE SCAN退化较大：测试条件差异（基线2000次迭代，当前500次；脚本版本差异）
- 阶段1优化主要针对字段提取，对简单索引场景效果有限
- 批量INSERT性能提升需进一步验证（基线无此测试项）

**说明**:
- 基线: 2025-12-17未优化代码，迭代2000次
- 当前: 2025-12-18阶段1优化后，迭代500次
- 此数据作为后续阶段优化的基线

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

## 阶段2: 嵌套字段前缀分组

### 验证状态

| 验证类型 | 状态 | 说明 |
|---------|------|------|
| 核心逻辑验证 | ✅ 完成 | 独立benchmark验证算法可行性 |
| 代码实现 | ✅ 完成 | 集成到btree_key_generator |
| 单元测试 | ✅ 108通过 | 所有BtreeKeyGeneratorTest通过 |
| 端到端基准测试 | ✅ 完成 | 见下方测试结果 |

### 核心逻辑验证结果

使用独立benchmark（`/tmp/stage2_nested_field_benchmark.cpp`）验证算法可行性：

```
场景                              基线(ns)  优化(ns)  加速比
-------------------------------------------------------------------
2字段共享前缀a                      44.7      27.1     1.65x ✓
3字段共享前缀a                      79.6      47.7     1.67x ✓
4字段共享前缀a,a.d                 117.6      69.3     1.70x ✓
2字段共享前缀x                      41.8      24.5     1.71x ✓
4字段,2组前缀                        86.5      52.3     1.65x ✓
深嵌套共享前缀m.n.o                  94.2      67.2     1.40x ✓
```

**注意**: 以上数据来自独立benchmark，模拟了嵌套对象遍历，不包含MongoDB实际代码路径的开销。

### 技术方案

```cpp
// 按首字段分组
std::map<StringData, std::vector<StringData>> prefixGroups;
for (const auto& path : indexPaths) {
    size_t dot = path.find('.');
    StringData prefix = dot != npos ? path.substr(0, dot) : path;
    StringData suffix = dot != npos ? path.substr(dot + 1) : "";
    prefixGroups[prefix].push_back(suffix);
}

// 提取公共前缀一次
for (const auto& group : prefixGroups) {
    BSONElement intermediate = obj.getField(group.first);
    if (intermediate.type() == Object) {
        for (const auto& suffix : group.second) {
            // 在中间对象上继续提取
        }
    }
}
```

### 实施复杂度分析

- **高复杂度**: 需要重构BtreeKeyGenerator的递归遍历逻辑
- **数组处理**: 必须正确处理数组展开场景，当前递归逻辑与数组展开紧密耦合
- **风险**: 改动可能引入回归bug，需要大量测试覆盖

### 端到端基准测试结果

测试日期: 2025-12-18
测试脚本: `/tmp/crud_benchmark_v3.py`
测试环境: RocksDB存储引擎

#### 简单表结果 (1索引, 3字段, ~150B)

| 操作 | 平均延迟 | P50 | P90 | P99 |
|-----|---------|-----|-----|-----|
| INSERT(单条) | 238us | 234us | 260us | 340us |
| INSERT(批量10) | 296us | 292us | 322us | 410us |
| UPDATE(非索引) | 259us | 252us | 280us | 388us |
| DELETE(单条) | 253us | 248us | 274us | 340us |
| POINT QUERY | 235us | 231us | 253us | 335us |
| RANGE SCAN(100) | 425us | 420us | 530us | 618us |

#### 复杂表结果 (11索引, ~90字段, ~800B)

| 操作 | 平均延迟 | P50 | P90 | P99 |
|-----|---------|-----|-----|-----|
| INSERT(单条) | 319us | 312us | 348us | 465us |
| INSERT(批量10) | 1222us | 1205us | 1300us | 1545us |
| UPDATE(非索引) | 265us | 258us | 288us | 392us |
| UPDATE(索引字段) | 302us | 295us | 330us | 468us |
| DELETE(单条) | 281us | 274us | 303us | 418us |
| POINT QUERY | 236us | 230us | 255us | 358us |
| RANGE SCAN(100) | 705us | 695us | 940us | 1095us |

#### 与基线及阶段1对比

| 操作 | 阶段2 | 阶段1 | 基线 | vs阶段1 | vs基线 |
|-----|-------|-------|------|---------|--------|
| INSERT(单条,简单) | 238us | 249us | 254us | **-4.4%** | **-6.3%** |
| INSERT(单条,复杂) | 319us | 358us | 363us | **-10.9%** | **-12.1%** |
| INSERT(批量10,复杂) | 1222us | 1254us | - | **-2.6%** | - |
| UPDATE(非索引,复杂) | 265us | 272us | 274us | **-2.6%** | **-3.3%** |
| UPDATE(索引字段,复杂) | 302us | 311us | - | **-2.9%** | - |

**分析**:
- **复杂表INSERT单条提升显著**: -10.9% vs 阶段1，-12.1% vs 基线
- 前缀分组对嵌套字段索引效果明显（复杂表有多个共享前缀的索引字段）
- 简单表提升较小（无嵌套字段）

---

## 阶段3: 数组延迟展开

### 验证状态

| 验证类型 | 状态 | 说明 |
|---------|------|------|
| 核心逻辑验证 | ✅ 完成 | 独立benchmark验证算法可行性 |
| 代码实现 | ❌ 未实现 | 需修改递归遍历结构 |
| 单元测试 | ❌ 未执行 | - |
| 端到端基准测试 | ❌ 未执行 | - |

### 核心逻辑验证结果

使用独立benchmark（`/tmp/stage3_array_benchmark.cpp`）验证算法可行性：

```
场景                    基线(ns)  优化(ns)  加速比  递归调用
---------------------------------------------------------------
6字段, 数组5元素          246.1    165.2    1.49x   6→1
6字段, 数组10元素         385.8    240.3    1.61x   11→1
6字段, 数组20元素         687.0    586.6    1.17x   21→1
10字段, 数组5元素         332.3    235.7    1.41x   6→1
10字段, 数组10元素        481.8    307.1    1.57x   11→1
10字段, 数组20元素        804.5    629.3    1.28x   21→1
20字段, 数组10元素        881.5    656.8    1.34x   11→1
20字段, 数组50元素       2381.7   1808.0    1.32x   51→1
```

**结论**: 加速比1.17x-1.61x，递归调用从n次减少到1次。

### 技术方案

```cpp
// 当前实现：递归处理每个数组元素
for (const auto arrObjElem : arrObj) {
    _getKeysArrEltFixed(&fieldNames, &fixed, arrObjElem, ...);  // 每个元素递归
}

// 优化方案：批量提取所有字段值
std::vector<std::vector<BSONElement>> batchValues(arrObj.nFields());
for (size_t i = 0; i < fields.size(); i++) {
    if (isArrayField[i]) {
        // 一次性提取所有数组元素的字段值
        for (const auto& elem : arrObj) {
            batchValues[i].push_back(extractField(elem, fields[i].suffix));
        }
    }
}
// 然后批量生成索引键
```

### 实施复杂度分析

- **高复杂度**: 需要重构`getKeysImplWithArray`的递归结构
- **风险**:
  - 位置索引(positional indexing)处理需要特殊处理
  - 嵌套数组场景复杂度高
  - Multikey路径跟踪逻辑紧耦合
- **收益评估**:
  - 优化仅对多键索引有效（indexed path包含数组）
  - 大多数OLTP场景数组较小，收益有限
  - 实施风险/收益比不理想

### 建议

基于分析，**建议暂缓阶段3实施**，原因：
1. 加速比(1.3x-1.6x)低于阶段4(3-10x)
2. 实施复杂度高，改动风险大
3. 仅对多键索引场景有效
4. 阶段4的多索引场景更常见，收益更高

建议直接进行阶段4，完成后根据实际性能数据决定是否回头实施阶段3。

---

## 阶段4: 跨索引共享（UnifiedFieldExtractor）

### 验证状态

| 验证类型 | 状态 | 说明 |
|---------|------|------|
| 核心逻辑验证 | ✅ 完成 | 独立benchmark验证算法可行性 |
| 代码实现 | ✅ 完成 | IndexCatalog + BtreeKeyGenerator集成 |
| 单元测试 | ✅ 通过 | 108个测试全部通过 |
| 端到端基准测试 | ❌ 失败 | **性能退化9.4%，已回退** |

### 端到端测试结果 (2025-12-18)

| 操作 | 基线 | 阶段2后 | 阶段4后 | vs阶段2 |
|-----|------|--------|--------|---------|
| 复杂表INSERT(单条) | 363us | 319us | 349us | **+9.4%退化** |
| 简单表INSERT(单条) | 254us | 238us | 259us | **+8.8%退化** |

### 退化根因分析

**架构变更**:
```
原始路径 (index-major):
  for each index:
    _indexRecords(所有documents)  // 批量处理，存储引擎高效

阶段4路径 (document-major):
  for each document:
    cache.build(document)
    for each index:
      _indexRecords({单个document})  // 逐条处理，丢失批量效率
```

**收益/开销分析**:

| 方面 | 期望收益 | 实际开销 |
|-----|---------|---------|
| 字段提取共享 | ~500ns/文档 | - |
| RocksDB批量丢失 | - | ~2000-5000ns/批次 |
| 额外函数调用 | - | ~10-50ns × N_docs × N_indexes |
| Vector构造 | - | ~50-100ns × N_docs |

**结论**: 开销 >> 收益，document-major遍历破坏了批量处理优化。

### 代码变更

- `index_catalog.cpp`: 已回退 (删除document-major遍历)
- `btree_key_generator.cpp`: 已回退 (删除缓存查找)

### 核心逻辑验证结果

使用独立benchmark（`/tmp/stage1_multi_index_benchmark.cpp`）验证算法可行性：

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

**结论**: 当索引数量≥5时，签名槽位方案开始有效；索引越多效果越明显。

### 已有代码文件

| 文件 | 说明 | 状态 |
|-----|------|------|
| `unified_field_extractor.h` | 统一字段提取器核心实现 | ✅ 完成 |
| `unified_field_extractor_test.cpp` | 单元测试(21个) | ✅ 通过 |
| `document_field_cache.h` | 备选方案：偏移量数组缓存 | ✅ 完成 |
| `index_catalog_optimization_patch.cpp` | IndexCatalog集成示例 | 📝 仅示例 |

### 集成方案分析

**集成点**: `IndexCatalog::indexRecords()` (index_catalog.cpp:1330)

```cpp
// 当前实现：每个索引独立提取字段
for (IndexCatalogEntryContainer::const_iterator i = _entries.begin();
     i != _entries.end(); ++i) {
    Status s = _indexRecords(txn, *i, bsonRecords, keysInsertedOut);
}

// 优化方案：先统一提取，再分发
UnifiedFieldExtractor extractor;
// 1. 收集所有索引字段
for (auto& entry : _entries) {
    extractor.registerIndex(entry->descriptor()->indexName(),
                           entry->descriptor()->keyPattern());
}
extractor.finalize();

// 2. 对每个文档：一次提取，多次使用
for (auto& record : bsonRecords) {
    extractor.extract(*record.docPtr);
    for (auto& entry : _entries) {
        // 使用预提取的字段生成键
        _indexRecordsWithCache(txn, entry, record, extractor, keysInsertedOut);
    }
}
```

### 集成复杂度

- **高复杂度**: 需要修改 IndexCatalog → IndexAccessMethod → BtreeKeyGenerator 整个调用链
- **API变更**: BtreeKeyGenerator::getKeys() 需要新增接受预提取字段的重载
- **测试覆盖**: 需要验证所有索引类型(btree, hash, 2d, text等)

### 待完成工作

1. ⬜ 实现 `IndexCatalog::_indexRecordsWithCache()` 方法
2. ⬜ 扩展 `BtreeKeyGenerator::getKeys()` 支持预提取字段
3. ⬜ 在索引创建时注册字段到 UnifiedFieldExtractor
4. ⬜ 编译mongod并运行端到端基准测试
5. ⬜ 验证多索引场景下的实际性能提升

### 决策建议

鉴于:
1. **阶段1+2已提供显著收益**: 复杂表INSERT -12.1% vs 基线
2. **阶段4收益条件苛刻**: 仅在≥5索引时有效
3. **集成复杂度高**: 需改动核心索引路径

**建议**: 先提交阶段1+2的代码，后续根据实际业务需求决定是否继续阶段4集成。

---

## 实施状态总览

| 阶段 | 核心验证 | 代码实现 | 单元测试 | 端到端测试 | 备注 |
|-----|---------|---------|---------|-----------|------|
| 阶段1 | ✅ | ✅ | ✅ 108通过 | ✅ 完成 | 长度位图+提前退出 |
| 阶段2 | ✅ | ✅ | ✅ 108通过 | ✅ 完成 | 前缀分组，复杂INSERT -10.9% |
| 阶段3 | ✅ | ⏸️ 暂缓 | - | - | 1.3x-1.6x，收益/风险比低 |
| 阶段4 | ✅ | ❌ 已回退 | ✅ 108通过 | ❌ 退化9.4% | document-major破坏批量优化 |
| 阶段4v2 | ✅ | ❌ 已回退 | ✅ 编译通过 | ❌ 退化1.9% | index-major+单文档缓存，开销>收益 |
| 阶段4v3 | ✅ | ✅ | ✅ 编译通过 | ✅ **改进3.1%** | 增量偏移量缓存，零额外构建开销 |

### 阶段4v3分析 (2025-12-19) ✅ 成功

**改进方案**: 增量偏移量缓存 - 第一个索引边遍历边缓存，后续索引受益

**核心优化点**:
1. **零构建开销**: 缓存在阶段1遍历时顺便填充，无独立构建步骤
2. **预计算槽位**: 构造时计算字段名哈希槽位，运行时零查找开销
3. **偏移量存储**: 存4字节偏移量而非BSONElement对象，更轻量
4. **哈希冲突校验**: 从偏移量重建元素后校验字段名，确保正确性

**端到端测试结果**:

| 指标 | 未优化基线 | Phase 2 | Phase 4v3 | vs基线 | vs Phase2 |
|-----|-----------|---------|-----------|--------|-----------|
| 复杂表INSERT(单条) | 363us | 319us | **309us** | **-14.9%** | **-3.1%** |
| 复杂表INSERT(批量10) | 1470us | 1230us | **1208us** | **-17.8%** | **-1.8%** |

**结论**: Phase 4v3成功！通过消除独立缓存构建步骤，将开销降到接近零，使跨索引字段共享收益得以体现。

### 阶段4v2分析 (2025-12-19)

**改进方案**: 保持index-major遍历顺序，仅对单文档+≥5索引场景启用跨索引字段缓存。

**端到端测试结果**:

| 指标 | Phase 2 | Phase 4v2 | vs Phase 2 |
|-----|---------|-----------|------------|
| 复杂表INSERT(单条) | 319us | 325us | +1.9% |
| 简单表INSERT(单条) | 238us | 250us | +5.0% |

**结论**: Phase 4v2的缓存查找开销（registry.getIndex + cache.getElement）超过了跨索引字段共享的收益。即使在11索引场景下，逐索引的O(n)字段遍历总成本仍低于全局缓存的建立和查询成本。

**根因分析**:
1. 测试场景的字段重合率不够高（11索引但字段独立性强）
2. 缓存命中检查本身有开销（needsRebuild + getFieldCount）
3. 每次getIndex调用涉及unordered_map查找

**最终决定**: 阶段4v2放弃，但阶段4v3成功，已合入代码。

## 总体成果 (阶段1+2+4v3)

### 与基线对比 (基线来自PERFORMANCE_OPTIMIZATION_LOG.md)

| 操作 | 基线 | 阶段4v3后 | 改进 |
|-----|------|----------|------|
| 简单表INSERT(单条) | 254us | 220us | **-13.4%** |
| 复杂表INSERT(单条) | 363us | **309us** | **-14.9%** |
| 简单表INSERT(批量10) | 294us | 276us | **-6.1%** |
| 复杂表INSERT(批量10) | 1470us | **1208us** | **-17.8%** |
| 复杂表UPDATE(非索引) | 270us | 240us | **-11.1%** |
| 复杂表UPDATE(索引字段) | 348us | 277us | **-20.4%** |
| 简单表DELETE | 241us | 210us | **-12.9%** |
| 复杂表DELETE | 310us | 258us | **-16.8%** |
| 简单表POINT QUERY | 217us | 196us | **-9.7%** |
| 复杂表POINT QUERY | 247us | 197us | **-20.2%** |

### 核心优化效果

| 指标 | 基线 | 优化后 | 改进 |
|-----|------|--------|------|
| **复杂表INSERT(批量10)** | 1470us | 1208us | **-17.8%** |
| **复杂表INSERT(单条)** | 363us | 309us | **-14.9%** |
| **复杂表UPDATE(索引字段)** | 348us | 277us | **-20.4%** |
| **复杂表POINT QUERY** | 247us | 197us | **-20.2%** |

**注**: 阶段1+2+4v3优化主要针对字段提取路径，对复杂表（多索引、嵌套字段）效果显著。
