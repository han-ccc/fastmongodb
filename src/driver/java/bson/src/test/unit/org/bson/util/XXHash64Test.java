/*
 * Copyright 2017 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.bson.util;

import net.jpountz.xxhash.XXHash64;
import net.jpountz.xxhash.XXHashFactory;
import org.junit.Test;

import java.nio.charset.StandardCharsets;

import static org.junit.Assert.*;

/**
 * Tests for xxHash64 using lz4-java library.
 * These tests verify the library works correctly for our document integrity use case.
 */
public class XXHash64Test {

    private static final XXHash64 XX_HASH_64 = XXHashFactory.fastestInstance().hash64();

    // ============================================================================
    // Basic functionality tests
    // ============================================================================

    @Test
    public void testEmptyData() {
        byte[] data = new byte[0];
        long hash = XX_HASH_64.hash(data, 0, 0, 0);
        // Empty data with seed 0 should produce a known hash (not zero)
        assertNotEquals(0L, hash);
    }

    @Test
    public void testSingleByte() {
        byte[] data = new byte[]{0x00};
        long hash = XX_HASH_64.hash(data, 0, 1, 0);
        assertNotEquals(0L, hash);
    }

    @Test
    public void testDeterministic() {
        byte[] data = "test data".getBytes(StandardCharsets.UTF_8);
        long hash1 = XX_HASH_64.hash(data, 0, data.length, 0);
        long hash2 = XX_HASH_64.hash(data, 0, data.length, 0);
        assertEquals(hash1, hash2);
    }

    @Test
    public void testDifferentDataDifferentHash() {
        byte[] data1 = "hello".getBytes(StandardCharsets.UTF_8);
        byte[] data2 = "world".getBytes(StandardCharsets.UTF_8);
        long hash1 = XX_HASH_64.hash(data1, 0, data1.length, 0);
        long hash2 = XX_HASH_64.hash(data2, 0, data2.length, 0);
        assertNotEquals(hash1, hash2);
    }

    @Test
    public void testDifferentSeedsDifferentHash() {
        byte[] data = "test".getBytes(StandardCharsets.UTF_8);
        long hash1 = XX_HASH_64.hash(data, 0, data.length, 0);
        long hash2 = XX_HASH_64.hash(data, 0, data.length, 1);
        assertNotEquals(hash1, hash2);
    }

    @Test
    public void testSameSeedSameHash() {
        byte[] data = "test data".getBytes(StandardCharsets.UTF_8);
        long hash1 = XX_HASH_64.hash(data, 0, data.length, 12345);
        long hash2 = XX_HASH_64.hash(data, 0, data.length, 12345);
        assertEquals(hash1, hash2);
    }

    // ============================================================================
    // Offset and length tests
    // ============================================================================

    @Test
    public void testWithOffset() {
        byte[] fullData = "prefix_test_suffix".getBytes(StandardCharsets.UTF_8);
        byte[] testOnly = "test".getBytes(StandardCharsets.UTF_8);

        // Hash just the "test" portion
        int offset = 7;  // "prefix_".length()
        int length = 4;  // "test".length()

        long hashFull = XX_HASH_64.hash(fullData, offset, length, 0);
        long hashDirect = XX_HASH_64.hash(testOnly, 0, testOnly.length, 0);

        assertEquals(hashFull, hashDirect);
    }

    @Test
    public void testPartialData() {
        byte[] data = "hello world".getBytes(StandardCharsets.UTF_8);
        long hashFull = XX_HASH_64.hash(data, 0, data.length, 0);
        long hashPartial = XX_HASH_64.hash(data, 0, 5, 0);  // Just "hello"

        assertNotEquals(hashFull, hashPartial);
    }

    // ============================================================================
    // Large data tests
    // ============================================================================

    @Test
    public void testLargeData() {
        // Test with data larger than 32 bytes (uses the main loop)
        byte[] data = new byte[1000];
        for (int i = 0; i < data.length; i++) {
            data[i] = (byte) (i % 256);
        }

        long hash = XX_HASH_64.hash(data, 0, data.length, 0);
        assertNotEquals(0L, hash);

        // Should be deterministic
        assertEquals(hash, XX_HASH_64.hash(data, 0, data.length, 0));
    }

    @Test
    public void testVeryLargeData() {
        // Test with 10KB of data
        byte[] data = new byte[10240];
        for (int i = 0; i < data.length; i++) {
            data[i] = (byte) ((i * 7) % 256);
        }

        long hash = XX_HASH_64.hash(data, 0, data.length, 0);
        assertNotEquals(0L, hash);
        assertEquals(hash, XX_HASH_64.hash(data, 0, data.length, 0));
    }

    // ============================================================================
    // Edge cases
    // ============================================================================

    @Test
    public void testExactly32Bytes() {
        byte[] data = new byte[32];
        for (int i = 0; i < 32; i++) {
            data[i] = (byte) i;
        }
        long hash = XX_HASH_64.hash(data, 0, data.length, 0);
        assertNotEquals(0L, hash);
    }

    @Test
    public void testExactly31Bytes() {
        byte[] data = new byte[31];
        for (int i = 0; i < 31; i++) {
            data[i] = (byte) i;
        }
        long hash = XX_HASH_64.hash(data, 0, data.length, 0);
        assertNotEquals(0L, hash);
    }

    @Test
    public void testExactly33Bytes() {
        byte[] data = new byte[33];
        for (int i = 0; i < 33; i++) {
            data[i] = (byte) i;
        }
        long hash = XX_HASH_64.hash(data, 0, data.length, 0);
        assertNotEquals(0L, hash);
    }

    @Test
    public void testAllZeros() {
        byte[] data = new byte[100];
        long hash = XX_HASH_64.hash(data, 0, data.length, 0);
        assertNotEquals(0L, hash);
    }

    @Test
    public void testAllOnes() {
        byte[] data = new byte[100];
        for (int i = 0; i < data.length; i++) {
            data[i] = (byte) 0xFF;
        }
        long hash = XX_HASH_64.hash(data, 0, data.length, 0);
        assertNotEquals(0L, hash);
    }

    // ============================================================================
    // Distribution tests
    // ============================================================================

    @Test
    public void testGoodDistribution() {
        // Test that small changes in input cause significant changes in output
        byte[] data1 = "test0".getBytes(StandardCharsets.UTF_8);
        byte[] data2 = "test1".getBytes(StandardCharsets.UTF_8);

        long hash1 = XX_HASH_64.hash(data1, 0, data1.length, 0);
        long hash2 = XX_HASH_64.hash(data2, 0, data2.length, 0);

        // The hashes should be very different (at least 16 bits different)
        long xor = hash1 ^ hash2;
        int bitsDifferent = Long.bitCount(xor);
        assertTrue("Expected at least 16 bits different, got " + bitsDifferent, bitsDifferent >= 16);
    }

    // ============================================================================
    // Thread safety test
    // ============================================================================

    @Test
    public void testThreadSafety() throws InterruptedException {
        final byte[] data = "thread safe test".getBytes(StandardCharsets.UTF_8);
        final long expectedHash = XX_HASH_64.hash(data, 0, data.length, 0);

        Thread[] threads = new Thread[10];
        final boolean[] results = new boolean[10];

        for (int i = 0; i < threads.length; i++) {
            final int index = i;
            threads[i] = new Thread(() -> {
                for (int j = 0; j < 1000; j++) {
                    long hash = XX_HASH_64.hash(data, 0, data.length, 0);
                    if (hash != expectedHash) {
                        results[index] = false;
                        return;
                    }
                }
                results[index] = true;
            });
        }

        for (Thread thread : threads) {
            thread.start();
        }
        for (Thread thread : threads) {
            thread.join();
        }

        for (boolean result : results) {
            assertTrue("Thread safety test failed", result);
        }
    }
}
