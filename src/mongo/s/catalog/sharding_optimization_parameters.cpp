/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * Sharding Optimization Parameters Implementation
 *
 * NOTE: 主要的 Coalescer 参数已移至 config_query_coalescer.cpp
 * 这里保留变量定义以保持头文件兼容性，但不再注册重复的 ServerParameter
 */

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/sharding_optimization_parameters.h"

namespace mongo {

// 这些变量保留用于向后兼容，但实际使用的是 config_query_coalescer.cpp 中的变量
// 推荐直接使用 configQueryCoalescer* 系列参数
std::atomic<bool> shardingConfigCoalescerEnabled{false};
std::atomic<int> shardingConfigCoalescerWindowMS{10};
std::atomic<int> shardingConfigCoalescerMaxWaitMS{200};
std::atomic<int> shardingConfigCoalescerMaxWaiters{500};

// ServerParameter 已移至 config_query_coalescer.cpp，避免重复注册

}  // namespace mongo
