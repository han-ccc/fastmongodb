# 测试指南

## resmoke.py测试运行器

```bash
# 运行测试套件
python buildscripts/resmoke.py --suites=core

# 运行特定JS测试
python buildscripts/resmoke.py --suites=core jstests/core/basic1.js

# 运行C++单元测试
python buildscripts/resmoke.py --suites=unittests

# 常用套件: core, aggregation, sharding, replication, replica_sets, unittests
# 套件配置在: buildscripts/resmokeconfig/suites/

# 有用选项
--jobs=N                      # N个测试并行
--continueOnFailure           # 失败不停止
--shuffle                     # 随机化测试顺序
--storageEngine=rocksdb       # 指定存储引擎
```

## C++单元测试

```bash
# 运行所有C++单元测试
python buildscripts/resmoke.py --suites=unittests

# 编译并运行特定测试
scons build/unittests/<test_name> --disable-warnings-as-errors -j24
./build/unittests/<test_name>
```

## JavaScript测试目录

- `jstests/core/` - 核心功能测试
- `jstests/aggregation/` - 聚合框架测试
- `jstests/sharding/` - 分片测试
- `jstests/repl/` - 复制测试

## Linting

```bash
# C++ linting
python buildscripts/lint.py src/mongo/

# clang-format
python buildscripts/clang_format.py lint
python buildscripts/clang_format.py format  # 自动修复
```

## 性能基准测试

```bash
# 运行CRUD基准测试 (单节点)
python /tmp/crud_benchmark_v3.py

# 运行F方案端到端测试 (分片集群)
python3 buildscripts/benchmarks/stability_benchmark_v7.py

# 性能基线
# 复杂表 INSERT(批量10): ~1470us (v1.0.0)
# 复杂表 POINT QUERY: ~247us (v1.0.0)
```

### 端到端基准测试规范

详见 `.claude/docs/E2E_BENCHMARK_GUIDE.md`，包含：

| 内容 | 说明 |
|------|------|
| F方案流程 | drop + fsync + sleep 重置 |
| 分片集群拓扑 | Config(27019) + Shard(27018) + mongos(27020) |
| 稳定性分组 | 极稳定/中等/不稳定 |
| 版本基线管理 | JSON格式、合并流程 |

## 微基准测试规范 (强制)

**每次性能优化修改必须附带微基准测试验证！**

### 强制要求

| 要求 | 说明 |
|-----|------|
| **独立可编译** | 微基准必须能用 `g++ -O2` 独立编译运行 |
| **对比新旧实现** | 必须同时测试优化前后的代码路径 |
| **多场景覆盖** | 测试不同输入类型/规模的性能差异 |
| **稳定可复现** | 多次运行结果一致 (波动 <5%) |
| **文档记录** | 结果记录到 `MICROBENCHMARK_CATALOG.md` |

### 微基准测试模板

```cpp
/**
 * [功能名称] 微基准测试
 * g++ -O2 -std=c++11 xxx_benchmark.cpp -o xxx_benchmark && ./xxx_benchmark
 */

#include <chrono>
#include <cstdio>
using namespace std::chrono;

// 旧实现 (v2.0.0)
int old_impl(args) { ... }

// 新实现 (v2.3.0)
int new_impl(args) { ... }

void benchmark(const char* scenario, int iterations) {
    volatile int sum1 = 0, sum2 = 0;

    // 预热
    for (int i = 0; i < 10000; ++i) {
        sum1 += old_impl(args);
        sum2 += new_impl(args);
    }

    // 测试旧实现
    auto start1 = high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        sum1 += old_impl(args);
    }
    auto dur1 = duration_cast<nanoseconds>(high_resolution_clock::now() - start1).count();

    // 测试新实现
    auto start2 = high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        sum2 += new_impl(args);
    }
    auto dur2 = duration_cast<nanoseconds>(high_resolution_clock::now() - start2).count();

    double ns1 = (double)dur1 / iterations;
    double ns2 = (double)dur2 / iterations;
    double diff = (ns2 - ns1) / ns1 * 100;

    printf("%-20s | old: %6.2f ns | new: %6.2f ns | %+.1f%%\n",
           scenario, ns1, ns2, diff);
}

int main() {
    const int ITERATIONS = 10000000;
    benchmark("scenario1", ITERATIONS);
    benchmark("scenario2", ITERATIONS);
    return 0;
}
```

### 微基准测试文件位置

| 位置 | 说明 |
|-----|------|
| `/tmp/*_benchmark.cpp` | 临时微基准测试 |
| `src/mongo/*/xxx_benchmark.cpp` | 持久化微基准 (可选) |

### 基准脚本管理规范

**规则**: 基准测试脚本可以在 `/tmp` 目录开发调试，**稳定后必须提交到代码仓库**。

| 阶段 | 位置 | 说明 |
|------|------|------|
| 开发调试 | `/tmp/*.py` | 快速迭代，无需提交 |
| 稳定版本 | `buildscripts/benchmarks/*.py` | 纳入版本控制，可复现 |

当前稳定脚本:
- `buildscripts/benchmarks/stability_benchmark_v7.py` - 27用例端到端测试

### 结果呈现格式

```
Scenario   | v2.0.0 (old)    | v2.3.0 (new)    | Diff    | Winner
-----------|-----------------|-----------------|---------|-------
fixed_type | old:   3.35 ns  | new:   2.61 ns  | -22.0%  | NEW
var_type   | old:   3.27 ns  | new:   2.80 ns  | -14.2%  | NEW
```

### 微基准测试目录

详见 `.claude/docs/MICROBENCHMARK_CATALOG.md`

## 单元测试性能验证规范

**核心原则**: 单元测试必须对核心逻辑做基本性能验证，确保优化有效且无退化。

### 必须包含的性能验证

| 优化类型 | 单元测试验证要求 |
|---------|-----------------|
| 字段提取优化 | 测试O(n)字段匹配vs O(1)直接访问的加速比 |
| 缓存优化 | 测试缓存命中 vs 未命中的延迟差异 |
| 批量处理 | 测试批量 vs 逐条的吞吐量比 |
| 数据结构优化 | 测试新旧数据结构的操作延迟 |

### 性能测试模板

```cpp
// 在单元测试中添加基准验证
TEST(MyOptimizationTest, PerformanceBaseline) {
    const int ITERATIONS = 10000;

    // 基线测量
    auto start1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ITERATIONS; i++) {
        baseline_function();
    }
    auto baseline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - start1).count();

    // 优化测量
    auto start2 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ITERATIONS; i++) {
        optimized_function();
    }
    auto optimized_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - start2).count();

    // 验证加速比 (至少1.2x)
    double speedup = (double)baseline_ns / optimized_ns;
    ASSERT_GE(speedup, 1.2) << "优化未达到预期加速比";

    std::cout << "加速比: " << speedup << "x" << std::endl;
}
```

### 验证流程

1. **编写核心逻辑单元测试** - 功能正确性
2. **添加性能基准测试** - 验证优化有效
3. **记录预期加速比** - 文档化性能目标
4. **CI集成** - 防止性能退化

### 性能测试命名规范

- `*_PerformanceBaseline` - 基线对比测试
- `*_Throughput` - 吞吐量测试
- `*_Latency` - 延迟测试
- `*_ScaleTest` - 规模扩展测试
