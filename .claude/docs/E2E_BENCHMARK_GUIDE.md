# 端到端基准测试规范

本文档定义 MongoDB + RocksDB 分片集群的端到端性能测试标准流程。

## 目录

1. [测试架构](#1-测试架构)
2. [F方案测试流程](#2-f方案测试流程)
3. [测试脚本](#3-测试脚本)
4. [稳定性分组](#4-稳定性分组)
5. [结果分析](#5-结果分析)
6. [版本基线管理](#6-版本基线管理)

---

## 1. 测试架构

### 分片集群拓扑

```
┌─────────────────────────────────────────────────────────────┐
│                      Client (benchmark)                      │
│                         port 27020                           │
└─────────────────────────┬───────────────────────────────────┘
                          │ OP_QUERY (opcode 2004)
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                        mongos                                │
│                      port 27020                              │
└──────────────┬────────────────────────────┬─────────────────┘
               │                            │
               ▼                            ▼
┌──────────────────────────┐    ┌──────────────────────────────┐
│    Config Server RS      │    │        Shard RS              │
│    (configRS)            │    │       (shardRS)              │
│    port 27019            │    │       port 27018             │
│    RocksDB               │    │       RocksDB                │
└──────────────────────────┘    └──────────────────────────────┘
```

### 启动命令

```bash
# 1. Config Server
./mongod --storageEngine=rocksdb \
    --dbpath=/tmp/mongo_config/db \
    --port=27019 \
    --configsvr \
    --replSet=configRS \
    --fork --logpath=/tmp/mongo_config/mongod.log

# 初始化 Config RS
mongo --port 27019 --eval 'rs.initiate({_id:"configRS", configsvr:true, members:[{_id:0,host:"localhost:27019"}]})'

# 2. Shard Server
./mongod --storageEngine=rocksdb \
    --dbpath=/tmp/mongo_shard/db \
    --port=27018 \
    --shardsvr \
    --replSet=shardRS \
    --fork --logpath=/tmp/mongo_shard/mongod.log

# 初始化 Shard RS
mongo --port 27018 --eval 'rs.initiate({_id:"shardRS", members:[{_id:0,host:"localhost:27018"}]})'

# 3. mongos
./mongos --configdb=configRS/localhost:27019 \
    --port=27020 \
    --fork --logpath=/tmp/mongos.log

# 4. 添加 Shard
mongo --port 27020 --eval 'sh.addShard("shardRS/localhost:27018")'
```

### 端口分配

| 组件 | 端口 | 说明 |
|------|------|------|
| Config Server | 27019 | configRS 主节点 |
| Shard Server | 27018 | shardRS 主节点 |
| mongos | 27020 | 路由入口 |

---

## 2. F方案测试流程

### 核心原理

F方案通过 `drop + fsync + sleep` 确保每次测试从干净状态开始：

```python
def f_scheme_reset(sock, db):
    """F方案重置 - 最稳定的测试方案"""
    # 1. 删除测试集合
    run_cmd(sock, db, "drop", "simple_docs", {})
    run_cmd(sock, db, "drop", "complex_docs", {})

    # 2. 强制刷盘
    run_cmd(sock, "admin", "fsync", None, {"fsync": 1})

    # 3. 等待 RocksDB 稳定
    time.sleep(3)
```

### 完整测试流程

```
┌─────────────────────────────────────────────────────────────┐
│  1. F方案重置 (drop + fsync + sleep 3s)                     │
├─────────────────────────────────────────────────────────────┤
│  2. 创建集合和索引                                           │
│     - simple_docs: 分片键 {userId: 1}                        │
│     - complex_docs: 分片键 + 5个二级索引                     │
├─────────────────────────────────────────────────────────────┤
│  3. 预热 (200次迭代, 4线程并发)                              │
├─────────────────────────────────────────────────────────────┤
│  4. 正式测试 (2000次迭代)                                    │
│     - 去除5%异常值                                           │
│     - 计算 avg, p50, p95, p99, min, max, stddev             │
├─────────────────────────────────────────────────────────────┤
│  5. 保存结果到 JSON                                          │
└─────────────────────────────────────────────────────────────┘
```

### 测试参数

| 参数 | 值 | 说明 |
|------|-----|------|
| ITERATIONS | 2000 | 正式测试迭代次数 |
| WARMUP | 200 | 预热迭代次数 |
| THREADS | 4 | 预热并发线程数 |
| OUTLIER_TRIM | 5% | 异常值剔除比例 |
| RESET_SLEEP | 3s | fsync后等待时间 |

---

## 3. 测试脚本

### 主测试脚本

位置: `/tmp/stability_benchmark_v6.py`

```bash
# 运行单次测试
python3 /tmp/stability_benchmark_v6.py

# 结果保存到
# → /tmp/stability_v6_result.json
```

### 脚本核心结构

```python
#!/usr/bin/env python3
"""
Stability Benchmark v6 - F方案端到端测试
"""

import socket
import struct
import time
import json
import random
from concurrent.futures import ThreadPoolExecutor

# OP_QUERY 协议实现 (MongoDB 3.4.2)
def send_op_query(sock, db, collection, query, ...): ...

# 命令执行
def run_cmd(sock, db, cmd_name, collection, cmd_args): ...

# F方案重置
def f_scheme_reset(sock, db): ...

# 测试用例
def test_insert_single(sock, db, coll, i): ...
def test_insert_batch(sock, db, coll, i): ...
def test_query_exact(sock, db, coll, i): ...
# ... 27个测试用例

# 主流程
def main():
    sock = connect("localhost", 27020)

    # Simple 表测试
    f_scheme_reset(sock, "shardtest")
    setup_simple_collection(sock)
    simple_results = run_all_tests(sock, "simple_docs")

    # Complex 表测试
    f_scheme_reset(sock, "shardtest")
    setup_complex_collection(sock)
    complex_results = run_all_tests(sock, "complex_docs")

    save_results(simple_results, complex_results)
```

### 结果 JSON 格式

```json
{
  "simple": [
    {
      "name": "INSERT (single)",
      "avg": 496.15,
      "p50": 495.15,
      "p95": 566.80,
      "p99": 657.55,
      "min": 416.40,
      "max": 896.60,
      "stddev": 37.10,
      "count": 2000
    },
    // ... 12个测试用例
  ],
  "complex": [
    // ... 15个测试用例
  ],
  "timestamp": "2025-12-21 14:48:22"
}
```

---

## 4. 稳定性分组

### 基于波动率分类

通过多次 F 方案运行的交叉验证确定稳定性等级：

| 稳定等级 | 波动率 | 误差阈值 | 可信度 |
|----------|--------|----------|--------|
| **极稳定** | <5% | ±3% | 高 - 可用于版本对比 |
| **中等稳定** | 5-10% | ±5% | 中 - 需多次验证 |
| **不稳定** | >10% | 仅参考 | 低 - 不可用于决策 |

### 测试用例分组

#### 极稳定 (可信)

| 表 | 测试用例 |
|---|---------|
| Simple | INSERT (batch 10), QUERY ($or), QUERY ($in), QUERY (partial key) |
| Complex | INSERT (batch 10), QUERY ($or), QUERY (range scan), QUERY ($in), QUERY (partial key) |

#### 中等稳定

| 表 | 测试用例 |
|---|---------|
| Simple | UPDATE (single), QUERY ($and) |
| Complex | UPDATE (single), UPDATE (indexed), UPDATE (multi idx), QUERY ($and), DELETE (by index) |

#### 不稳定 (仅参考)

| 表 | 测试用例 |
|---|---------|
| Simple | INSERT (single), QUERY (exact key), QUERY ($gt), DELETE (single), DELETE (multi) |
| Complex | INSERT (single), QUERY (exact key), QUERY ($gt), DELETE (single) |

---

## 5. 结果分析

### 多指标评估

| 指标 | 说明 | 用途 |
|------|------|------|
| avg | 平均值 (去除5%异常) | 主要对比指标 |
| p50 | 中位数 | 最稳定指标 |
| p95 | 95分位 | 尾部延迟 |
| p99 | 99分位 | 极端情况 |
| stddev | 标准差 | 稳定性判断 |

### 版本对比规则

```python
def compare_versions(v1, v2, test_name, stability):
    """对比两个版本的性能"""
    diff = (v2.avg - v1.avg) / v1.avg * 100

    if stability == "极稳定":
        threshold = 3.0
    elif stability == "中等稳定":
        threshold = 5.0
    else:
        return "仅参考"

    if diff < -threshold:
        return f"改进 {-diff:.1f}%"
    elif diff > threshold:
        return f"退化 {diff:.1f}%"
    else:
        return "持平"
```

### 结果呈现模板

```
## v2.0.0 vs v2.3.0 对比

### 可信结果 (极稳定+中等稳定)

| 测试用例 | v2.0.0 | v2.3.0 | 变化 | 结论 |
|----------|--------|--------|------|------|
| INSERT (batch 10) | 557μs | 565μs | +1.4% | 持平 |
| UPDATE (indexed) | 548μs | 515μs | -6.0% | **改进** |

### 统计

- 显著改进 (>阈值): X 个
- 持平: Y 个
- 显著退化: Z 个
```

---

## 6. 版本基线管理

### 版本-Commit 映射

| 版本 | Commit | 说明 | 基线文件 |
|------|--------|------|---------|
| v1.0.0 | `d14fea75771` | 原始无优化 | `/tmp/baseline_v1.0.0.json` |
| v2.0.0 | `04609e58010` | Phase 1,2,4v3,A | `/tmp/baseline_v2.0.0.json` |
| v2.3.0 | `73f3eb9deba` | Phase A'/B' | `/tmp/baseline_v2.3.0.json` |

### 基线生成流程

```bash
# 1. Checkout 目标版本
git checkout <commit>

# 2. 编译
scons mongod mongos --disable-warnings-as-errors -j24

# 3. 启动集群
./start_cluster.sh

# 4. 运行 F 方案测试 (至少2轮)
python3 /tmp/stability_benchmark_v6.py  # Run 1
python3 /tmp/stability_benchmark_v6.py  # Run 2

# 5. 合并基线
python3 /tmp/merge_baseline.py \
    /tmp/run1.json /tmp/run2.json \
    --output /tmp/baseline_vX.X.X.json \
    --version vX.X.X \
    --commit <commit>
```

### 基线 JSON 格式

```json
{
  "version": "v2.0.0",
  "commit": "04609e58010",
  "data": {
    "simple": [...],
    "complex": [...]
  },
  "runs": 2
}
```

---

## 附录: 快速命令

### 启动集群

```bash
# 完整启动
pkill -f mongod; pkill -f mongos
rm -rf /tmp/mongo_config/db /tmp/mongo_shard/db
mkdir -p /tmp/mongo_config/db /tmp/mongo_shard/db

./mongod --storageEngine=rocksdb --dbpath=/tmp/mongo_config/db --port=27019 --configsvr --replSet=configRS --fork --logpath=/tmp/mongo_config/mongod.log
sleep 2
mongo --port 27019 --eval 'rs.initiate({_id:"configRS",configsvr:true,members:[{_id:0,host:"localhost:27019"}]})'

./mongod --storageEngine=rocksdb --dbpath=/tmp/mongo_shard/db --port=27018 --shardsvr --replSet=shardRS --fork --logpath=/tmp/mongo_shard/mongod.log
sleep 2
mongo --port 27018 --eval 'rs.initiate({_id:"shardRS",members:[{_id:0,host:"localhost:27018"}]})'
sleep 3

./mongos --configdb=configRS/localhost:27019 --port=27020 --fork --logpath=/tmp/mongos.log
sleep 2
mongo --port 27020 --eval 'sh.addShard("shardRS/localhost:27018")'
```

### 停止集群

```bash
pkill -f "mongos.*27020"
pkill -f "mongod.*27018"
pkill -f "mongod.*27019"
```

### 检查状态

```bash
pgrep -fa mongod
pgrep -fa mongos
mongo --port 27020 --eval 'sh.status()'
```
