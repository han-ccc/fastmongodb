# 编译指南

## Build System 基础

MongoDB使用SCons (Python构建系统)。

### 基础命令

```bash
scons all                      # 编译全部 (mongod, mongos, mongo shell, tests)
scons mongod                   # 只编译数据库服务器
scons mongos                   # 只编译路由器
scons mongo                    # 只编译shell
scons core                     # 编译核心二进制
scons --prefix=/opt/mongo install  # 安装到指定目录
```

### 常用选项

```bash
scons --ssl                    # 启用SSL
scons --wiredtiger=on          # 启用WiredTiger (本项目不使用)
scons --mmapv1=on              # 启用MMAPv1存储引擎
scons --release                # Release构建
scons -j24                     # 24线程并行编译
scons --disable-warnings-as-errors  # 禁用警告作为错误
scons --js-engine=none         # 不编译JS引擎 (加速)
```

### 推荐编译命令

```bash
# 标准编译 (RocksDB存储引擎)
scons mongod --disable-warnings-as-errors --js-engine=none -j24
```

## 编译效率优化

### 核心原则

1. **增量编译优先** - 只编译修改的文件
2. **缓存一致性** - 切换分支后处理编译缓存
3. **并行最大化** - 充分利用多核CPU

### 增量编译策略

```bash
# 修改单个源文件后，只编译该.o文件
scons build/opt/mongo/db/bson/dotted_path_support.o --disable-warnings-as-errors -j24
# 耗时: ~5秒 (vs 全量编译 ~10分钟)

# 修改后直接编译mongod (scons自动检测变更)
scons mongod --disable-warnings-as-errors --js-engine=none -j24

# 编译特定单元测试
scons build/unittests/bson_obj_test --disable-warnings-as-errors -j24
```

### 分支切换策略

```bash
# 切换分支后清理受影响的.o文件
git checkout perf/baseline
rm -f build/opt/mongo/db/bson/dotted_path_support.o
rm -f build/opt/mongo/db/index/btree_key_generator.o
scons mongod --disable-warnings-as-errors --js-engine=none -j24

# 遇到链接错误时
grep -l "enableLocalShardingInfo" build/opt/mongo/**/*.o  # 找问题.o
rm -f build/opt/mongo/s/*.o
rm -f build/opt/mongo/s/**/*.o

# 全量清理 (最后手段)
scons -c mongod --disable-warnings-as-errors --js-engine=none
# 或
rm -rf build/opt
```

### ccache加速

```bash
# ccache已安装，scons自动使用
ccache -s              # 查看状态
ccache -C              # 清理缓存
# 首次编译后，重复编译加速~50-70%
```

## 编译输出过滤

```bash
# 只看错误
scons mongod ... 2>&1 | grep -E "error:"

# 只看警告
scons mongod ... 2>&1 | grep -E "warning:"

# 提取错误文件和行号
scons mongod ... 2>&1 | grep -oE "^[^:]+:[0-9]+" | sort -u

# 检查编译成功
scons mongod ... 2>&1 | grep "scons: done building targets"
```

## 编译时间参考

| 操作 | 预计时间 | 说明 |
|-----|---------|------|
| 单个.cpp编译 | 3-10秒 | 取决于复杂度 |
| mongod链接 | 30-60秒 | 大量符号解析 |
| mongod全量编译 | 8-15分钟 | 首次或清理后 |
| mongod增量编译 | 1-3分钟 | 修改少量文件 |

## 后台编译监控

```bash
# 启动后台编译
scons mongod --disable-warnings-as-errors --js-engine=none -j24 > /tmp/build.log 2>&1 &

# 监控进度
tail -f /tmp/build.log | grep -E "Compiling|Linking|error:|done"

# 检查是否完成
ps aux | grep scons
ls -la mongod
```

## 常见错误快速修复

| 错误类型 | 症状 | 解决方案 |
|---------|------|---------|
| undefined reference | 链接时报错 | 清理相关.o文件重新编译 |
| redefinition | 重复定义 | 检查头文件include顺序 |
| no such file | 找不到头文件 | 检查SConscript依赖 |
| glibc兼容性 | 模板错误 | cherry-pick兼容性修复 |
