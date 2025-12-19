# 代码架构

## Core Server Components

- **mongod** (`src/mongo/db/db.cpp`): 数据库服务器入口
- **mongos** (`src/mongo/s/server.cpp`): 分片路由器入口
- **mongo shell** (`src/mongo/shell/`): 交互式JavaScript shell

## 目录结构

```
src/mongo/
├── base/          # 基础工具, Status, StringData
├── bson/          # BSON实现
├── client/        # 客户端驱动代码
├── db/            # mongod服务器代码
│   ├── auth/      # 认证/授权
│   ├── catalog/   # 数据库/集合目录
│   ├── commands/  # 数据库命令
│   ├── concurrency/ # 锁子系统
│   ├── exec/      # 查询执行
│   ├── index/     # 索引实现
│   ├── ops/       # 写操作
│   ├── pipeline/  # 聚合管道
│   ├── query/     # 查询解析和规划
│   ├── repl/      # 复制
│   └── storage/   # 存储引擎抽象
├── executor/      # 任务执行框架
├── s/             # mongos/分片代码
│   ├── catalog/   # 分片目录
│   ├── client/    # 分片连接
│   └── query/     # 分布式查询执行
├── scripting/     # JavaScript引擎集成
├── transport/     # 网络传输层
├── unittest/      # 单元测试框架
└── util/          # 通用工具
```

## Storage Engine API

MongoDB有可插拔的存储引擎架构 (`src/mongo/db/storage/`):

| 接口 | 文件 | 说明 |
|-----|------|------|
| **KVEngine** | `kv/kv_engine.h` | 键值引擎接口 |
| **RecordStore** | `record_store.h` | 集合存储接口 |
| **SortedDataInterface** | `sorted_data_interface.h` | 索引存储接口 |
| **RecoveryUnit** | `recovery_unit.h` | 事务接口 |

存储引擎实现ACID语义和快照隔离。`WriteConflictException`处理乐观并发冲突。

## Build Configuration

- `SConstruct`: 主构建配置
- `src/mongo/**/SConscript`: 各目录构建规则
- `buildscripts/`: 构建和测试工具

## Dependencies

第三方代码在 `src/third_party/`:
- RocksDB存储引擎 (本项目使用)
- Boost库
- Mozilla SpiderMonkey (JS引擎)
- 其他依赖

## Writing Unit Tests

使用 `src/mongo/unittest/` 中的框架:

```cpp
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(MyTestSuite, TestName) {
    ASSERT_EQUALS(expected, actual);
    ASSERT_TRUE(condition);
}

class MyFixture : public unittest::Test {
    // setUp() and tearDown() available
};

TEST_F(MyFixture, TestWithFixture) {
    // ...
}

}  // namespace
}  // namespace mongo
```

在SConscript中注册: `env.CppUnitTest('test_name', 'test_file.cpp')`
