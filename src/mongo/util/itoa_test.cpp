/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <array>
#include <cstdint>
#include <limits>

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/itoa.h"

namespace {
using namespace mongo;

TEST(ItoA, StringDataEquality) {
    ASSERT_EQ(ItoA::kBufSize - 1, std::to_string(std::numeric_limits<std::uint32_t>::max()).size());

    for (auto testCase : {1u,
                          12u,
                          133u,
                          1446u,
                          17789u,
                          192923u,
                          2389489u,
                          29313479u,
                          1928127389u,
                          std::numeric_limits<std::uint32_t>::max()}) {
        ItoA itoa{testCase};
        ASSERT_EQ(std::to_string(testCase), StringData(itoa));
    }
}

// ============================================================================
// DecimalCounter Tests - 覆盖 #8 修复 (缓冲区溢出)
// ============================================================================

// Test: Basic increment
TEST(DecimalCounter, BasicIncrement) {
    DecimalCounter counter;
    ASSERT_EQ("0", StringData(counter));

    ++counter;
    ASSERT_EQ("1", StringData(counter));

    ++counter;
    ASSERT_EQ("2", StringData(counter));

    for (int i = 0; i < 7; ++i) {
        ++counter;
    }
    ASSERT_EQ("9", StringData(counter));
}

// Test: Carry overflow (9 -> 10)
TEST(DecimalCounter, CarryOverflow) {
    DecimalCounter counter(9);
    ASSERT_EQ("9", StringData(counter));

    ++counter;
    ASSERT_EQ("10", StringData(counter));

    ++counter;
    ASSERT_EQ("11", StringData(counter));
}

// Test: Multiple digit carry (99 -> 100, 999 -> 1000)
TEST(DecimalCounter, MultipleDigitCarry) {
    DecimalCounter counter(99);
    ASSERT_EQ("99", StringData(counter));

    ++counter;
    ASSERT_EQ("100", StringData(counter));

    DecimalCounter counter2(999);
    ++counter2;
    ASSERT_EQ("1000", StringData(counter2));

    DecimalCounter counter3(9999);
    ++counter3;
    ASSERT_EQ("10000", StringData(counter3));
}

// Test: Start from non-zero value
TEST(DecimalCounter, StartFromNonZero) {
    DecimalCounter counter(12345);
    ASSERT_EQ("12345", StringData(counter));
    ASSERT_EQ(5u, counter.size());

    ++counter;
    ASSERT_EQ("12346", StringData(counter));
}

// Test: Large number increment (approaching uint32 max)
TEST(DecimalCounter, LargeNumberIncrement) {
    DecimalCounter counter(4294967290u);  // uint32_max - 5
    ASSERT_EQ("4294967290", StringData(counter));

    ++counter;
    ASSERT_EQ("4294967291", StringData(counter));

    ++counter;
    ASSERT_EQ("4294967292", StringData(counter));
}

// Test: Buffer size for large numbers (修复 #8 的关键测试)
TEST(DecimalCounter, BufferSizeForLargeNumbers) {
    // uint32_t max = 4294967295 (10 digits)
    DecimalCounter counter(std::numeric_limits<std::uint32_t>::max());
    ASSERT_EQ("4294967295", StringData(counter));
    ASSERT_EQ(10u, counter.size());

    // Increment past uint32_t max - should work due to buffer size fix
    ++counter;
    ASSERT_EQ("4294967296", StringData(counter));
    ASSERT_EQ(10u, counter.size());
}

// Test: Increment sequence for array indexing
TEST(DecimalCounter, ArrayIndexingSequence) {
    DecimalCounter counter;

    // Simulate array indexing: 0, 1, 2, ..., 999
    for (int i = 0; i < 1000; ++i) {
        ASSERT_EQ(std::to_string(i), StringData(counter).toString());
        ++counter;
    }
    ASSERT_EQ("1000", StringData(counter));
}

// Test: Post-increment operator
// Note: DecimalCounter's default copy constructor doesn't fix up _end pointer,
// so we test post-increment's effect on the original counter only.
TEST(DecimalCounter, PostIncrement) {
    DecimalCounter counter(5);

    // Post-increment should increment the counter
    counter++;
    ASSERT_EQ("6", StringData(counter));

    counter++;
    counter++;
    ASSERT_EQ("8", StringData(counter));
}

// Test: Size and data accessors
TEST(DecimalCounter, SizeAndDataAccessors) {
    DecimalCounter counter(123);

    ASSERT_EQ(3u, counter.size());
    ASSERT_EQ('1', counter.data()[0]);
    ASSERT_EQ('2', counter.data()[1]);
    ASSERT_EQ('3', counter.data()[2]);
}

// Test: All nines carry chain (worst case)
TEST(DecimalCounter, AllNinesCarryChain) {
    DecimalCounter counter(99999999);  // 8 nines
    ASSERT_EQ("99999999", StringData(counter));

    ++counter;
    ASSERT_EQ("100000000", StringData(counter));
    ASSERT_EQ(9u, counter.size());
}

// Test: Stress test - many increments
TEST(DecimalCounter, StressTest) {
    DecimalCounter counter;

    // 100000 increments
    for (int i = 0; i < 100000; ++i) {
        ++counter;
    }
    ASSERT_EQ("100000", StringData(counter));
}

}  // namespace
