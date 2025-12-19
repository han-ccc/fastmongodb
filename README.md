# FastMongoDB

> MongoDB 3.4.2 + RocksDB 5.1.2 深度性能优化版本

## 项目简介

FastMongoDB 是对 MongoDB 3.4.2 + RocksDB 5.1.2 存储引擎的深度性能优化版本。通过对 BSON 解析、索引字段提取、分片路由等核心路径的优化，在复杂表场景下实现了 **15-20% 的性能提升**。

## 性能优化成果

### 单机 CRUD 性能

| 操作 | 基线 (us) | 优化后 (us) | 提升 |
|-----|----------|------------|------|
| 复杂表 INSERT (批量10条) | 1470 | **1207** | **-17.9%** |
| 复杂表 INSERT (单条) | 377 | **309** | **-14.9%** |
| 复杂表 UPDATE (索引字段) | 348 | **277** | **-20.4%** |
| 复杂表 POINT QUERY | 247 | **197** | **-20.2%** |
| 复杂表 DELETE | 310 | **258** | **-16.8%** |

> 测试环境：复杂表含 11 个索引，每个索引 10 个字段，文档约 800 字节

### 分片集群优化

| 指标 | 优化前 | 优化后 |
|-----|--------|--------|
| Config Server 查询减少率 | - | **95%+** |
| 支撑 mongos 并发数 | ~100 | **2000+** |
| 5万 chunks 接管时间 | 分钟级 | **秒级** |

## 优化内容

### 1. 字段提取优化 (Phase 1/2/4v3)

**问题**：多索引场景下，每个索引独立遍历文档提取字段，存在大量重复工作。

**优化**：
- **Phase 1**: 长度位图 + 提前退出，减少无效字段比较
- **Phase 2**: 嵌套字段前缀分组，共享中间路径遍历
- **Phase 4v3**: 增量偏移量缓存，跨索引共享字段提取结果

**代码位置**：`src/mongo/db/index/btree_key_generator.cpp`

### 2. BSON 优化 (Phase A'/B')

**Phase A' - BSONElement::size() 分支消除**
- 将 64 行 switch 语句替换为 O(1) 查表
- 使用 `kFixedSizes[32]` 查表 + 位掩码判断

**Phase B' - 短字符串整数比较**
- ≤8 字节字符串使用 `uint64_t` 整数比较替代 `memcmp`
- 用户字段名通常 ≤5 字符，优化效果显著

**代码位置**：
- `src/mongo/bson/bsonelement.cpp`
- `src/mongo/base/string_data.h`

### 3. ConfigQueryCoalescer - 分片请求合并

**问题**：大规模分片集群中，数千 mongos 同时刷新路由表会压垮 Config Server。

**优化**：
- 合并同一 namespace 的并发 chunks 查询请求
- 版本过滤：每个等待者只获取自己需要的增量
- 自适应窗口：根据负载动态调整合并窗口

**核心指标**：
- 1000+ mongos 并发，查询减少 95%+
- 5 万 chunks 在 5 秒内完成接管
- P99 延迟 < 100ms

**代码位置**：`src/mongo/s/catalog/config_query_coalescer.h/.cpp`

## 项目结构

```
fastmongodb/
├── mongo/                    # MongoDB 3.4.2 优化版 (本仓库)
├── mongo-rocks/              # RocksDB 存储引擎适配层 (需单独克隆)
├── rocksdb/                  # RocksDB 5.1.2 (需单独下载)
└── docs/                     # 优化文档
    ├── FIELD_EXTRACTION_OPTIMIZATION.md
    ├── BSON_PERF_PLAN.md
    ├── SHARDING_OPTIMIZATION_PLAN.md
    ├── PERFORMANCE_OPTIMIZATION_LOG.md
    └── .claude/docs/
        ├── BUILD_GUIDE.md
        ├── TESTING_GUIDE.md
        └── ...
```

## 快速开始

### 依赖组件

```bash
# 1. 克隆本仓库 (MongoDB 优化版)
git clone https://github.com/<your-username>/fastmongodb.git
cd fastmongodb

# 2. 获取 mongo-rocks (RocksDB 存储引擎适配层)
git clone https://github.com/mongodb-partners/mongo-rocks.git src/mongo/db/modules/rocks

# 3. 获取 RocksDB 5.1.2
wget https://github.com/facebook/rocksdb/archive/v5.1.2.tar.gz
tar -xzf v5.1.2.tar.gz
export CPPPATH=$PWD/rocksdb-5.1.2/include
export LIBPATH=$PWD/rocksdb-5.1.2

# 4. 编译 RocksDB
cd rocksdb-5.1.2
make static_lib -j24
cd ..
```

### 编译 MongoDB

```bash
scons mongod --disable-warnings-as-errors --js-engine=none -j24
```

### 运行

```bash
# 单机模式
./mongod --storageEngine=rocksdb --dbpath=/tmp/mongo_data --port=27019

# 分片集群
# 参见 .claude/docs/E2E_BENCHMARK_GUIDE.md
```

### 基准测试

```bash
# 单机 CRUD 测试
python /tmp/crud_benchmark_v3.py

# 分片压测 (需编译测试)
scons build/unittests/large_scale_coalescer_test
./build/unittests/large_scale_coalescer_test
```

## 技术文档

| 文档 | 说明 |
|-----|------|
| [字段提取优化](FIELD_EXTRACTION_OPTIMIZATION.md) | Phase 1/2/4v3 详细方案与测试结果 |
| [BSON 优化计划](BSON_PERF_PLAN.md) | BSON 解析优化方案 |
| [分片优化计划](SHARDING_OPTIMIZATION_PLAN.md) | ConfigQueryCoalescer 设计 |
| [性能日志](PERFORMANCE_OPTIMIZATION_LOG.md) | 完整的优化过程记录 |
| [编译指南](.claude/docs/BUILD_GUIDE.md) | 构建系统说明 |
| [测试指南](.claude/docs/TESTING_GUIDE.md) | 单元测试与基准测试 |

## 适用场景

- 多索引复杂表的高频写入场景
- 大规模分片集群（1000+ mongos）
- 需要 RocksDB 存储引擎的场景（高压缩比、高写入吞吐）

## 版本信息

- MongoDB: 3.4.2
- RocksDB: 5.1.2
- mongo-rocks: 适配版本

## 分支说明

| 分支 | 说明 |
|-----|------|
| `perf/baseline` | 主优化分支，包含所有性能优化 |
| `master` | 原始 MongoDB 3.4.2 代码 |

## License

本项目基于 MongoDB 和 RocksDB 的原始许可证。
- MongoDB: [GNU AGPL v3.0](https://www.gnu.org/licenses/agpl-3.0.html)
- RocksDB: [Apache 2.0 / GPLv2](https://github.com/facebook/rocksdb/blob/main/LICENSE.Apache)

## 致谢

- MongoDB Inc. - 原始 MongoDB 代码
- Facebook/Meta - RocksDB 存储引擎
- MongoDB 社区 - mongo-rocks 适配层
