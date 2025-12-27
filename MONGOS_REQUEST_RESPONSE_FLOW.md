# Mongos 请求响应处理流程分析

## 概述

本文档分析 MongoDB 3.4 中 mongos 如何处理来自 shard 的响应，以及响应到达后如何唤醒等待的线程。

## 核心组件

| 组件 | 文件 | 职责 |
|------|------|------|
| NetworkInterfaceASIO | `src/mongo/executor/network_interface_asio.cpp` | 异步网络 IO |
| AsyncOp | `src/mongo/executor/network_interface_asio_operation.cpp` | 单个操作状态机 |
| ThreadPoolTaskExecutor | `src/mongo/executor/thread_pool_task_executor.cpp` | 任务调度执行 |

## 请求/响应处理流程

```
┌─────────────────────────────────────────────────────────────────────────┐
│ mongos 发送请求                                                          │
│                                                                          │
│  startCommand()                                                          │
│     │                                                                    │
│     ▼                                                                    │
│  ConnectionPool 获取连接                                                  │
│     │                                                                    │
│     ▼                                                                    │
│  _asyncRunCommand() ─── ASIO 线程                                        │
│     │                                                                    │
│     ├─► Step 1: asyncSendMessage()     发送请求                          │
│     │                                                                    │
│     ├─► Step 2: asyncRecvMessageHeader() 接收响应头                       │
│     │                                                                    │
│     └─► Step 3: asyncRecvMessageBody()  接收响应体                        │
│                    │                                                     │
│                    ▼                                                     │
│              _completedOpCallback()                                      │
│                    │                                                     │
│                    ▼                                                     │
│              _completeOperation()                                        │
│                    │                                                     │
│                    ├─► op->finish(response)                              │
│                    │      │                                              │
│                    │      └─► _onFinish(rs)  ← 调用回调！                 │
│                    │                                                     │
│                    └─► signalWorkAvailable()  ← 唤醒！                   │
└─────────────────────────────────────────────────────────────────────────┘
```

## 关键代码路径

### 1. 异步命令执行 (_asyncRunCommand)

```cpp
// network_interface_asio_command.cpp:372-430
void NetworkInterfaceASIO::_asyncRunCommand(AsyncOp* op, NetworkOpHandler handler) {
    auto cmd = op->command();

    // Step 4: 最终回调
    auto recvMessageCallback = [this, cmd, handler, op](std::error_code ec, size_t bytes) {
        handler(ec, bytes);
    };

    // Step 3: 接收响应体
    auto recvHeaderCallback = [this, cmd, handler, recvMessageCallback, op](...) {
        _validateAndRun(op, ec, [this, op, recvMessageCallback, ...] {
            asyncRecvMessageBody(cmd->conn().stream(),
                                 &cmd->header(),
                                 &cmd->toRecv(),
                                 std::move(recvMessageCallback));
        });
    };

    // Step 2: 接收响应头
    auto sendMessageCallback = [this, cmd, handler, recvHeaderCallback, op](...) {
        _validateAndRun(op, ec, [this, cmd, op, recvHeaderCallback] {
            asyncRecvMessageHeader(cmd->conn().stream(),
                                   &cmd->header(),
                                   std::move(recvHeaderCallback));
        });
    };

    // Step 1: 发送请求
    asyncSendMessage(cmd->conn().stream(), &cmd->toSend(), std::move(sendMessageCallback));
}
```

### 2. 操作完成处理 (_completeOperation)

```cpp
// network_interface_asio_command.cpp:274-370
void NetworkInterfaceASIO::_completeOperation(AsyncOp* op, ResponseStatus resp) {
    // 取消超时定时器
    if (op->_timeoutAlarm) {
        op->_timeoutAlarm->cancel();
    }

    // 从 _inProgress 移除
    {
        stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
        auto iter = _inProgress.find(op);
        ownedOp = std::move(iter->second);
        _inProgress.erase(iter);
    }

    // 调用完成回调
    op->finish(std::move(resp));

    // 归还连接到连接池
    asioConn->indicateSuccess();

    // 唤醒等待的 executor
    signalWorkAvailable();
}
```

### 3. 完成回调执行 (finish)

```cpp
// network_interface_asio_operation.cpp:194-210
void NetworkInterfaceASIO::AsyncOp::finish(ResponseStatus&& rs) {
    _transitionToState(AsyncOp::State::kFinished);

    LOG(2) << "Request " << _request.id << " finished with response: "
           << redact(rs.isOK() ? rs.data.toString() : rs.status.toString());

    // 调用用户提供的回调函数
    _onFinish(rs);  // ← 这里执行实际的业务回调
}
```

### 4. 唤醒机制 (signalWorkAvailable)

```cpp
// network_interface_asio.cpp:206-216
void NetworkInterfaceASIO::signalWorkAvailable() {
    stdx::unique_lock<stdx::mutex> lk(_executorMutex);
    _signalWorkAvailable_inlock();
}

void NetworkInterfaceASIO::_signalWorkAvailable_inlock() {
    if (!_isExecutorRunnable) {
        _isExecutorRunnable = true;
        _isExecutorRunnableCondition.notify_one();  // 条件变量唤醒
    }
}
```

### 5. 等待机制 (waitForWork)

```cpp
// network_interface_asio.cpp:184-204
void NetworkInterfaceASIO::waitForWork() {
    stdx::unique_lock<stdx::mutex> lk(_executorMutex);
    while (!_isExecutorRunnable) {
        _isExecutorRunnableCondition.wait(lk);  // 阻塞等待
    }
    _isExecutorRunnable = false;
}

void NetworkInterfaceASIO::waitForWorkUntil(Date_t when) {
    stdx::unique_lock<stdx::mutex> lk(_executorMutex);
    while (!_isExecutorRunnable) {
        const Milliseconds waitTime(when - now());
        if (waitTime <= Milliseconds(0)) {
            break;
        }
        _isExecutorRunnableCondition.wait_for(lk, waitTime.toSystemDuration());
    }
    _isExecutorRunnable = false;
}
```

## 可能导致响应延迟的原因

### 1. ASIO 线程池繁忙

响应处理在 ASIO 线程中进行。如果 ASIO 线程忙于其他 IO 操作，`_completedOpCallback` 的调用会延迟。

```cpp
// 所有 IO 回调都在 ASIO 线程中执行
// 如果有大量并发连接，可能导致排队
```

### 2. _onFinish 回调执行慢

```cpp
// network_interface_asio_operation.cpp:209
_onFinish(rs);  // ← 如果这个回调执行慢，会阻塞后续处理
```

回调函数可能涉及：
- 结果解析和处理
- 锁竞争
- 内存分配

### 3. signalWorkAvailable 时锁竞争

```cpp
// 需要获取 _executorMutex
stdx::unique_lock<stdx::mutex> lk(_executorMutex);  // ← 可能竞争
```

如果有多个响应同时完成，或者其他线程持有该锁，会产生等待。

### 4. TaskExecutor 线程池调度延迟

```cpp
// thread_pool_task_executor.cpp:492
const auto status = _pool->schedule([this, cbState] { runCallback(...); });
// ThreadPool 可能已满，任务排队等待
```

### 5. 条件变量唤醒延迟

```cpp
_isExecutorRunnableCondition.notify_one();
```

- 操作系统调度延迟
- 被唤醒的线程可能不是立即执行

## 排查建议

### 1. 添加时间戳日志

在以下关键位置添加时间戳日志：

```cpp
// network_interface_asio_command.cpp
void NetworkInterfaceASIO::_completedOpCallback(AsyncOp* op) {
    auto t1 = now();  // 添加
    auto response = op->command()->response(...);
    auto t2 = now();  // 添加
    _completeOperation(op, response);
    auto t3 = now();  // 添加
    LOG(0) << "Response timing: decode=" << (t2-t1) << " complete=" << (t3-t2);
}

// network_interface_asio_operation.cpp
void NetworkInterfaceASIO::AsyncOp::finish(ResponseStatus&& rs) {
    auto t1 = now();
    _onFinish(rs);
    auto t2 = now();
    LOG(0) << "_onFinish took: " << (t2-t1);
}
```

### 2. 检查 ASIO 线程数

```cpp
// 查看 NetworkInterfaceASIO 构造时的线程配置
// 默认可能只有少量线程处理所有连接
```

### 3. 监控 ThreadPool 状态

```bash
# 检查 mongos 日志中是否有 ThreadPool 相关警告
grep -i "thread.*pool\|executor" mongos.log
```

### 4. 追踪 _onFinish 回调

确定 `startCommand` 时传入的 `onFinish` 回调具体做了什么：

```cpp
// 通常来自 ThreadPoolTaskExecutor::scheduleRemoteCommand
// 回调会将结果放入队列并调度后续处理
```

### 5. 检查锁竞争

使用性能分析工具检查：
- `_executorMutex` 竞争情况
- `_inProgressMutex` 竞争情况

## 数据流图

```
                    ┌─────────────┐
                    │   Client    │
                    └──────┬──────┘
                           │ request
                           ▼
                    ┌─────────────┐
                    │   mongos    │
                    └──────┬──────┘
                           │
          ┌────────────────┼────────────────┐
          │                │                │
          ▼                ▼                ▼
    ┌──────────┐    ┌──────────┐    ┌──────────┐
    │  shard0  │    │  shard1  │    │  shard2  │
    └────┬─────┘    └────┬─────┘    └────┬─────┘
         │               │               │
         │ response      │ response      │ response
         │               │               │
         └───────────────┼───────────────┘
                         │
                         ▼
              ┌─────────────────────┐
              │  ASIO IO Thread     │
              │  - recv response    │
              │  - decode           │
              │  - _completeOp      │
              └──────────┬──────────┘
                         │
                         │ _onFinish(rs)
                         │ signalWorkAvailable()
                         ▼
              ┌─────────────────────┐
              │  TaskExecutor       │
              │  - waitForWork()    │◄── notify_one()
              │  - runCallback()    │
              └──────────┬──────────┘
                         │
                         ▼
              ┌─────────────────────┐
              │  业务逻辑处理        │
              │  - 合并结果          │
              │  - 返回客户端        │
              └─────────────────────┘
```

## 相关文件

| 文件 | 说明 |
|------|------|
| `src/mongo/executor/network_interface_asio.h` | 异步网络接口头文件 |
| `src/mongo/executor/network_interface_asio.cpp` | 异步网络接口实现 |
| `src/mongo/executor/network_interface_asio_command.cpp` | 命令发送/接收 |
| `src/mongo/executor/network_interface_asio_operation.cpp` | AsyncOp 状态机 |
| `src/mongo/executor/thread_pool_task_executor.cpp` | 线程池任务执行器 |
| `src/mongo/executor/connection_pool_asio.cpp` | ASIO 连接池 |
