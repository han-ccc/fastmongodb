/**
 * xxHash64 implementation for MongoDB
 *
 * This file enables the xxHash library by defining XXH_STATIC_LINKING_ONLY
 * and including the header which contains the implementation.
 */

#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include "xxhash.h"
