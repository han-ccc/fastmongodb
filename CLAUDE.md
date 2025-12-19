# CLAUDE.md

MongoDB 3.4.2 + RocksDB 5.1.2 性能优化项目指南

## 项目概述

| 项目 | 值 |
|-----|-----|
| **存储引擎** | RocksDB 5.1.2 (不考虑WiredTiger) |
| **基准测试** | `/tmp/crud_benchmark_v3.py` |
| **INSERT基线** | ~1470us (批量10) |
| **QUERY基线** | ~247us (点查) |

## 快速命令

```bash
# 编译mongod (推荐)
scons mongod --disable-warnings-as-errors --js-engine=none -j24

# 启动测试mongod
./mongod --storageEngine=rocksdb --dbpath=/tmp/mongo_data --port=27019 --fork --logpath=/tmp/mongod.log

# 运行性能测试
python /tmp/crud_benchmark_v3.py

# 停止mongod
pkill -f "mongod.*27019"
```

## 强制规范

**必须严格按照本文档（CLAUDE.md）及子文档（.claude/docs/*.md）的规范执行所有操作。**

- 执行任何任务前，先查阅相关文档
- 不得跳过验证流程中的任何步骤
- 性能优化必须先验证核心逻辑，记录到md文档后再集成
- 遇到不确定的情况，优先查阅文档或询问用户

## 工作规范

| 规范 | 说明 |
|-----|------|
| **文档优先** | 所有操作必须符合CLAUDE.md及子文档规范 |
| **后台任务监控** | 最长30秒检查一次 |
| **提交行数限制** | 单次 ≤2000行 |
| **IO链路修改** | 必须先过性能基线 |
| **测试端口** | 27019 (主测试), 27098 (临时) |

## 验证流程

1. **微基准测试** → 2. **单元测试** → 3. **端到端编译** → 4. **基准测试** → 5. **记录到日志**

### 微基准测试 (强制)

**每次性能优化必须先通过微基准验证！**

```bash
# 编写独立微基准
g++ -O2 -std=c++11 /tmp/xxx_benchmark.cpp -o /tmp/xxx_benchmark

# 运行验证
./xxx_benchmark

# 记录结果到文档
# → .claude/docs/MICROBENCHMARK_CATALOG.md
```

| 要求 | 说明 |
|-----|------|
| 独立编译 | 不依赖 MongoDB 构建系统 |
| 新旧对比 | 同时测试优化前后代码 |
| 多场景 | 覆盖不同输入类型/规模 |
| 稳定性 | 多次运行波动 <5% |

## 文档索引

| 文档 | 说明 | 路径 |
|-----|------|------|
| **工作记忆** | 跨会话上下文、进度追踪 | `.claude/docs/WORK_MEMORY.md` |
| **编译指南** | 构建系统、增量编译、ccache | `.claude/docs/BUILD_GUIDE.md` |
| **测试指南** | resmoke、单元测试、微基准 | `.claude/docs/TESTING_GUIDE.md` |
| **微基准目录** | 微基准测试用例及结果 | `.claude/docs/MICROBENCHMARK_CATALOG.md` |
| **端到端基准** | F方案、分片集群测试规范 | `.claude/docs/E2E_BENCHMARK_GUIDE.md` |
| **代码架构** | 目录结构、存储引擎API | `.claude/docs/CODE_ARCHITECTURE.md` |
| **进程管理** | mongod管理、端口分配 | `.claude/docs/PROCESS_MANAGEMENT.md` |
| **Git规范** | 分支策略、IO链路规则 | `.claude/docs/GIT_VERSION_CONTROL.md` |
| **改动规则** | 记录要求、检查清单 | `.claude/docs/CHANGE_RECORD_RULES.md` |
| **字段提取优化** | 当前主要工作，阶段1-4计划 | `FIELD_EXTRACTION_OPTIMIZATION.md` |
| **BSON优化** | BSON性能优化详细计划 | `BSON_PERF_PLAN.md` |
| **分片优化** | 分片路由优化计划 | `SHARDING_OPTIMIZATION_PLAN.md` |
| **性能日志** | 优化验证记录 | `PERFORMANCE_OPTIMIZATION_LOG.md` |

## 当前工作上下文

**每次会话开始时，先阅读 `.claude/docs/WORK_MEMORY.md` 恢复上下文。**

### 基线数据位置
- 未优化基线: `/tmp/benchmark_results.json` (2025-12-17)
- 当前数据: `/tmp/benchmark_results_v3.json`

## Git分支

| 分支 | 说明 | 状态 |
|-----|------|------|
| `perf/baseline` | 纯净基线 | 可用 |
| `perf/p2-stringdata` | StringData零拷贝 | 验证中 |
| `master` | 原始代码 | 参考 |

## 关键目录

```
src/mongo/
├── bson/         # BSON实现
├── db/           # mongod服务器
│   ├── catalog/  # 数据库目录
│   ├── exec/     # 查询执行
│   ├── index/    # 索引实现
│   ├── ops/      # 写操作
│   └── storage/  # 存储引擎
└── s/            # 分片代码
```

## 禁止事项

- 跳过微基准测试直接提交性能优化
- 跳过单元测试直接端到端编译
- 未验证性能就声称优化有效
- 在master上直接修改优化代码
- 超过30秒不检查后台任务
- 性能优化未记录到 MICROBENCHMARK_CATALOG.md

