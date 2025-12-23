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

### 分片集群优化 (ConfigQueryCoalescer)

**测试场景说明**：

| 场景 | 版本分布 | 模拟场景 |
|------|----------|----------|
| SAME_VERSION | 所有请求使用相同版本 | mongos 集群冷启动，全部从 version=0 开始 |
| CLOSE_VERSIONS | 版本在 [base, base+100] 范围内 | 正常运行中的 mongos 集群，版本相近 |
| BOUNDARY_GAP | 版本在 [base, base+500] 范围内 | 部分 mongos 短暂离线后重连 |
| HOTSPOT_MIX | 80% 相近版本 + 20% 随机版本 | 热点集中场景 |
| RANDOM | 版本在 [1, numChunks] 均匀随机 | 压力测试基线，最差情况 |

**压测结果**：

| 场景 | QPS | 延迟 | CPU | 合并率 | 响应吞吐 | 文档数 |
|------|-----|------|-----|--------|----------|--------|
| SAME_VERSION | **25,186** | 38ms | 27% | 80% | 2,716 MB/s | 351M |
| CLOSE_VERSIONS | 25,125 | 38ms | 28% | 80% | 2,711 MB/s | 350M |
| BOUNDARY_GAP | 24,941 | 38ms | 28% | 80% | 2,688 MB/s | 347M |
| HOTSPOT_MIX | 18,263 | 53ms | 92% | 53% | 1,961 MB/s | 253M |
| RANDOM (基线) | 9,208 | 103ms | 94% | 8% | 971 MB/s | 125M |

**优化效果** (SAME_VERSION vs RANDOM)：

| 指标 | RANDOM | SAME_VERSION | 提升 |
|------|--------|--------------|------|
| QPS | 9,208 | 25,186 | **+174%** |
| 延迟 | 103ms | 38ms | **-63%** |
| CPU 使用率 | 94% | 27% | **-71%** |
| 响应吞吐 | 971 MB/s | 2,716 MB/s | **+180%** |
| 合并率 | 8% | 80% | **+72%** |

> 压测环境：104 collections, 100,000 chunks (每个 chunk 文档约 125 bytes), 1000 并发线程, 单机 mongod

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
- **无窗口等待**：第一个请求立即执行，后续请求复用结果
- **Leader/Follower 模式**：Leader 执行查询，Followers 共享结果
- **版本差距检测**：版本差距 > 500 的请求独立执行
- **连接池复用**：200-300 共享连接，减少连接开销

**核心指标**：
- QPS 提升 **2.74x** (9,208 → 25,186)
- 延迟降低 **63%** (103ms → 38ms)
- CPU 使用率降低 **71%** (94% → 27%)
- Config 响应吞吐提升 **180%** (971 → 2,716 MB/s)

**代码位置**：`src/mongo/db/s/config/config_query_coalescer.h/.cpp`

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
