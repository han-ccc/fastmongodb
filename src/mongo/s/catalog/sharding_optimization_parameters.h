/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * Sharding Optimization Parameters
 */

#pragma once

#include <atomic>

#include "mongo/db/server_parameters.h"

namespace mongo {

// ConfigQueryCoalescer enabled flag
extern std::atomic<bool> shardingConfigCoalescerEnabled;

// Coalescing window in milliseconds
extern std::atomic<int> shardingConfigCoalescerWindowMS;

// Maximum wait time in milliseconds
extern std::atomic<int> shardingConfigCoalescerMaxWaitMS;

// Maximum waiters per group
extern std::atomic<int> shardingConfigCoalescerMaxWaiters;

}  // namespace mongo
