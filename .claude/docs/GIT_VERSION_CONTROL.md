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

## 提交信息格式

```
<type>(<scope>): <简短描述>

<详细描述>

<影响范围和测试说明>
```

**type类型**: `perf`, `feat`, `fix`, `test`, `docs`, `refactor`
**scope范围**: `bson`, `index`, `storage`, `query`, `sharding`
