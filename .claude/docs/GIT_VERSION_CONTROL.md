# Git版本控制规范 (性能优化专用)

## 核心原则

**每个优化必须是独立的、可追溯的、可回滚的Git提交/分支**

## 分支策略

```
master (原始MongoDB 3.4.2代码)
  │
  ├── perf/baseline          # 纯净基线，无任何优化
  │
  ├── perf/p1-field-cache    # P1优化：文档字段缓存 (已废弃)
  │
  ├── perf/p2-stringdata     # P2优化：StringData零拷贝
  │
  └── perf/current           # 当前生产版本
```

## 创建优化分支

```bash
# 1. 从干净基线创建新优化分支
git checkout perf/baseline
git checkout -b perf/p3-new-optimization

# 2. 实现优化代码
# ... 编写代码 ...

# 3. 提交优化
git add -A
git commit -m "perf(scope): 优化描述

详细说明:
- 修改了什么
- 为什么这样修改
- 预期效果

影响文件:
- file1.cpp
- file2.h"

# 4. 测试验证
scons mongod --disable-warnings-as-errors --js-engine=none -j24
./mongod --storageEngine=rocksdb --dbpath=/tmp/mongo_data --port=27019 --fork --logpath=/tmp/mongod.log
python /tmp/crud_benchmark_v3.py
```

## 回滚优化

```bash
# 方法1: 切换到基线分支
git checkout perf/baseline
scons mongod --disable-warnings-as-errors --js-engine=none -j24

# 方法2: 回滚特定文件
git checkout perf/baseline -- src/mongo/db/path/to/file.cpp
scons mongod --disable-warnings-as-errors --js-engine=none -j24

# 方法3: 完全重置
git reset --hard perf/baseline
```

## 合并优化到生产

```bash
# 只有验证通过的优化才能合并
git checkout perf/current
git merge perf/p2-stringdata

# 解决冲突后重新测试
scons mongod --disable-warnings-as-errors --js-engine=none -j24
python /tmp/crud_benchmark_v3.py
```

## 禁止事项

1. **禁止在master上直接修改优化代码**
2. **禁止未测试就合并优化**
3. **禁止多个优化混在一个提交中**
4. **禁止修改后不提交就切换分支**

## 优化版本对照表

| 分支/Tag | 描述 | 状态 | INSERT(批量10) |
|---------|------|------|----------------|
| `perf/baseline` | 纯净基线 | 可用 | ~1470us |
| `perf/p1-field-cache` | 文档字段缓存 | 废弃 | ~1765us (退化) |
| `perf/p2-stringdata` | StringData零拷贝 | 验证中 | ~1537us |

## IO链路提交规则 (强制)

**所有涉及IO链路的代码变更，必须通过性能基线验证后才能提交**

### IO链路文件范围

```
# 写入路径
src/mongo/db/ops/write_ops_exec.cpp
src/mongo/db/catalog/collection.cpp
src/mongo/db/catalog/index_catalog.cpp
src/mongo/db/index/btree_key_generator.cpp
src/mongo/db/index/index_access_method.cpp

# 读取路径
src/mongo/db/exec/*.cpp
src/mongo/db/query/*.cpp
src/mongo/db/bson/dotted_path_support.cpp

# 存储引擎
src/mongo/db/modules/rocks/src/*.cpp
src/mongo/db/storage/*.cpp
```

### 提交前验证流程

```bash
# 1. 运行基准测试
python /tmp/crud_benchmark_v3.py

# 2. 记录结果
# 复杂表 INSERT(批量10): ____us (基线: 1470us)
# 复杂表 POINT QUERY: ____us (基线: 247us)

# 3. 验证无性能退化
# - 与基线对比，退化不超过3%才能提交
# - 如有退化，必须在提交信息中说明原因

# 4. 更新性能日志
# 记录到 PERFORMANCE_OPTIMIZATION_LOG.md
```

## 提交信息格式 (Conventional Commits)

### 基本格式

```
<type>(<scope>): <简短描述>

<详细描述>

<footer>
```

### Type 类型 (英文，必填)

| Type | 说明 | 示例 |
|------|------|------|
| `feat` | 新功能 | 添加查询缓存 |
| `fix` | Bug 修复 | 修复内存泄漏 |
| `perf` | 性能优化 | 优化索引查找 |
| `refactor` | 重构 | 重构字段提取逻辑 |
| `test` | 测试 | 补充单元测试 |
| `docs` | 文档 | 更新 README |
| `chore` | 构建/工具 | 更新 SConscript |
| `style` | 代码格式 | 修正缩进 |

### Scope 范围 (英文，可选)

`bson`, `index`, `storage`, `query`, `sharding`, `util`, `db`

### 语言规范 (中英混用)

**原则**: type/scope 用英文，描述用中文，术语变量保持英文

```bash
# ✅ 推荐
fix(bson): 修复 BSONElement 类型检查越界问题
perf(index): 优化 UnifiedFieldExtractor 签名计算
test: 补充 DecimalCounter 溢出边界测试

# ❌ 不推荐
fix(bson): fix BSONElement type check out of bounds  # 全英文难读
fix(bson): 修复BSON元素类型检查越界问题  # 术语也翻译了
```

### 完整示例

```bash
git commit -m "$(cat <<'EOF'
fix(sharding): 修复 ConfigQueryCoalescer 参数断开问题

问题:
- ServerParameter 修改后 atomic 变量未同步
- 缺少参数范围验证

修复:
- 自定义 ServerParameter 类直接更新 atomic 变量
- 添加范围验证 (windowMS: 1-1000, maxWaitMS: 1-10000)

影响文件:
- config_query_coalescer.cpp
EOF
)"
```

### 标题行要求

| 规则 | 说明 |
|------|------|
| 长度 | ≤ 50 字符 (中文约 25 字) |
| 时态 | 使用祈使句 (修复/添加/优化) |
| 结尾 | 不加句号 |
| 大小写 | type 小写，描述首字母可大写 |
