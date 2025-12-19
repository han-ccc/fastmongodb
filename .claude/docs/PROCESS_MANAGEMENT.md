# 进程管理规范

## mongod进程管理

**原则**: 测试完成后必须停止mongod进程，避免资源浪费和端口冲突

### 常用命令

```bash
# 检查运行中的mongod
ps aux | grep mongod | grep -v grep

# 查看使用的端口
netstat -tlnp 2>/dev/null | grep mongod

# 停止特定端口的mongod
pkill -f "mongod.*27019"

# 停止所有mongod (谨慎)
pkill -9 mongod

# 优雅停止
kill $(pgrep -f "mongod.*27019")
```

### 标准测试端口分配

| 端口 | 用途 | 说明 |
|-----|------|------|
| 27017 | 默认端口 | 避免使用，可能冲突 |
| **27019** | 主测试端口 | 性能基准测试使用 |
| 27098 | 临时测试 | 对比测试，用完即停 |

## 测试流程规范

```bash
# 1. 测试前检查
ps aux | grep mongod | grep -v grep

# 2. 启动测试mongod
./mongod --storageEngine=rocksdb --dbpath=/tmp/mongo_data --port=27019 --fork --logpath=/tmp/mongod.log

# 3. 运行测试
python /tmp/crud_benchmark_v3.py

# 4. 测试完成后停止
pkill -f "mongod.*27019"

# 5. 清理数据目录 (如需要)
rm -rf /tmp/mongo_data/*
```

## 会话结束清理

**每次会话结束前，必须检查并清理**:

```bash
# 列出所有mongod
ps aux | grep mongod | grep -v grep

# 停止所有测试mongod
pkill -f "mongod.*(27019|27098)"
```
