/**
 * CRC32C implementation with SSE4.2 hardware acceleration
 * Based on RocksDB's crc32c implementation (BSD license)
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mongo {
namespace crc32c {

/**
 * Check if hardware-accelerated CRC32C is supported (SSE4.2)
 */
bool isHardwareSupported();

/**
 * Compute CRC32C of data[0,n-1]
 */
uint32_t compute(const char* data, size_t n);

/**
 * Extend CRC32C with additional data
 */
uint32_t extend(uint32_t crc, const char* data, size_t n);

}  // namespace crc32c
}  // namespace mongo
