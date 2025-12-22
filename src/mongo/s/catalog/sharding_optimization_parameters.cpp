/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * Sharding Optimization Parameters Implementation
 */

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/sharding_optimization_parameters.h"

#include "mongo/db/server_parameters.h"

namespace mongo {

// ConfigQueryCoalescer enabled flag - default disabled
std::atomic<bool> shardingConfigCoalescerEnabled{false};

// Coalescing window in milliseconds - default 10ms
std::atomic<int> shardingConfigCoalescerWindowMS{10};

// Maximum wait time in milliseconds - default 200ms
std::atomic<int> shardingConfigCoalescerMaxWaitMS{200};

// Maximum waiters per group - default 500
std::atomic<int> shardingConfigCoalescerMaxWaiters{500};

// Server parameters for runtime configuration
MONGO_EXPORT_SERVER_PARAMETER(shardingConfigCoalescerEnabledParam, bool, false);
MONGO_EXPORT_SERVER_PARAMETER(shardingConfigCoalescerWindowMSParam, int, 10);
MONGO_EXPORT_SERVER_PARAMETER(shardingConfigCoalescerMaxWaitMSParam, int, 200);
MONGO_EXPORT_SERVER_PARAMETER(shardingConfigCoalescerMaxWaitersParam, int, 500);

}  // namespace mongo
