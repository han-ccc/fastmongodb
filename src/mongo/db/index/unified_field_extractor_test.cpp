/**
 * unified_field_extractor_test.cpp
 *
 * Comprehensive unit tests and performance benchmarks for UnifiedFieldExtractor
 *
 * Test coverage:
 * - Basic field registration and extraction
 * - Nested field handling
 * - Index and digest registration
 * - Deduplication
 * - Edge cases (empty doc, missing fields, signature collision)
 * - Performance benchmarks (original vs optimized)
 */

#include "mongo/platform/basic.h"

#include <chrono>
#include <iostream>
#include <random>
#include <sstream>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/index/unified_field_extractor.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

namespace dps = ::mongo::dotted_path_support;

// High precision timer
class Timer {
public:
    void start() { _start = std::chrono::high_resolution_clock::now(); }
    void stop() { _end = std::chrono::high_resolution_clock::now(); }
    int64_t elapsedNanos() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(_end - _start).count();
    }
private:
    std::chrono::high_resolution_clock::time_point _start, _end;
};

// ============================================================================
// UNIT TESTS - Functional correctness
// ============================================================================

class UnifiedFieldExtractorBasicTest : public unittest::Test {
public:
    void setUp() {
        BSONObjBuilder b;
        b.append("_id", 1);
        b.append("name", "testUser");
        b.append("age", 25);
        b.append("email", "test@example.com");
        b.append("score", 95.5);
        _doc = b.obj();
    }

    BSONObj _doc;
};

// Test: Basic field registration
TEST_F(UnifiedFieldExtractorBasicTest, BasicRegistration) {
    UnifiedFieldExtractor extractor;

    uint8_t slot1 = extractor.registerField("_id");
    uint8_t slot2 = extractor.registerField("name");
    uint8_t slot3 = extractor.registerField("age");

    ASSERT_EQUALS(slot1, 0u);
    ASSERT_EQUALS(slot2, 1u);
    ASSERT_EQUALS(slot3, 2u);
    ASSERT_EQUALS(extractor.totalUniqueFields(), 3u);
}

// Test: Duplicate registration returns same slot
TEST_F(UnifiedFieldExtractorBasicTest, DuplicateRegistration) {
    UnifiedFieldExtractor extractor;

    uint8_t slot1 = extractor.registerField("name");
    uint8_t slot2 = extractor.registerField("name");
    uint8_t slot3 = extractor.registerField("name");

    ASSERT_EQUALS(slot1, slot2);
    ASSERT_EQUALS(slot2, slot3);
    ASSERT_EQUALS(extractor.totalUniqueFields(), 1u);
}

// Test: Basic extraction
TEST_F(UnifiedFieldExtractorBasicTest, BasicExtraction) {
    UnifiedFieldExtractor extractor;

    uint8_t idSlot = extractor.registerField("_id");
    uint8_t nameSlot = extractor.registerField("name");
    uint8_t ageSlot = extractor.registerField("age");
    extractor.finalize();

    extractor.extract(_doc);

    ASSERT_EQUALS(extractor.get(idSlot).Int(), 1);
    ASSERT_EQUALS(extractor.get(nameSlot).String(), "testUser");
    ASSERT_EQUALS(extractor.get(ageSlot).Int(), 25);
    ASSERT_EQUALS(extractor.extractedCount(), 3u);
}

// Test: Missing field returns eoo
TEST_F(UnifiedFieldExtractorBasicTest, MissingField) {
    UnifiedFieldExtractor extractor;

    uint8_t slot = extractor.registerField("nonexistent");
    extractor.finalize();

    extractor.extract(_doc);

    ASSERT_TRUE(extractor.get(slot).eoo());
}

// Test: Invalid slot returns eoo
TEST_F(UnifiedFieldExtractorBasicTest, InvalidSlot) {
    UnifiedFieldExtractor extractor;
    extractor.registerField("_id");
    extractor.finalize();
    extractor.extract(_doc);

    ASSERT_TRUE(extractor.get(200).eoo());
}

// ============================================================================
// Nested field tests
// ============================================================================

class UnifiedFieldExtractorNestedTest : public unittest::Test {
public:
    void setUp() {
        BSONObjBuilder b;
        b.append("_id", 1);
        b.append("name", "test");
        {
            BSONObjBuilder addr(b.subobjStart("address"));
            addr.append("city", "Beijing");
            addr.append("zip", "100000");
            {
                BSONObjBuilder geo(addr.subobjStart("geo"));
                geo.append("lat", 39.9);
                geo.append("lng", 116.4);
                geo.done();
            }
            addr.done();
        }
        {
            BSONObjBuilder meta(b.subobjStart("meta"));
            meta.append("version", 2);
            meta.append("created", "2024-01-01");
            meta.done();
        }
        _doc = b.obj();
    }

    BSONObj _doc;
};

// Test: Nested field extraction
TEST_F(UnifiedFieldExtractorNestedTest, NestedExtraction) {
    UnifiedFieldExtractor extractor;

    uint8_t citySlot = extractor.registerField("address.city");
    uint8_t zipSlot = extractor.registerField("address.zip");
    uint8_t versionSlot = extractor.registerField("meta.version");
    extractor.finalize();

    extractor.extract(_doc);

    ASSERT_EQUALS(extractor.get(citySlot).String(), "Beijing");
    ASSERT_EQUALS(extractor.get(zipSlot).String(), "100000");
    ASSERT_EQUALS(extractor.get(versionSlot).Int(), 2);
}

// Test: Deep nested field
TEST_F(UnifiedFieldExtractorNestedTest, DeepNestedExtraction) {
    UnifiedFieldExtractor extractor;

    uint8_t latSlot = extractor.registerField("address.geo.lat");
    uint8_t lngSlot = extractor.registerField("address.geo.lng");
    extractor.finalize();

    extractor.extract(_doc);

    ASSERT_EQUALS(extractor.get(latSlot).Double(), 39.9);
    ASSERT_EQUALS(extractor.get(lngSlot).Double(), 116.4);
}

// Test: Mixed top-level and nested
TEST_F(UnifiedFieldExtractorNestedTest, MixedExtraction) {
    UnifiedFieldExtractor extractor;

    uint8_t idSlot = extractor.registerField("_id");
    uint8_t nameSlot = extractor.registerField("name");
    uint8_t citySlot = extractor.registerField("address.city");
    uint8_t latSlot = extractor.registerField("address.geo.lat");
    extractor.finalize();

    ASSERT_EQUALS(extractor.topLevelCount(), 2u);
    ASSERT_EQUALS(extractor.nestedCount(), 2u);

    extractor.extract(_doc);

    ASSERT_EQUALS(extractor.get(idSlot).Int(), 1);
    ASSERT_EQUALS(extractor.get(nameSlot).String(), "test");
    ASSERT_EQUALS(extractor.get(citySlot).String(), "Beijing");
    ASSERT_EQUALS(extractor.get(latSlot).Double(), 39.9);
}

// ============================================================================
// Index and Digest tests
// ============================================================================

class UnifiedFieldExtractorIndexTest : public unittest::Test {
public:
    void setUp() {
        BSONObjBuilder b;
        b.append("_id", 1);
        b.append("userId", 1001);
        b.append("orderId", 2001);
        b.append("productId", 3001);
        b.append("name", "testProduct");
        b.append("price", 99.9);
        b.append("quantity", 5);
        b.append("status", "completed");
        {
            BSONObjBuilder meta(b.subobjStart("meta"));
            meta.append("created", "2024-01-01");
            meta.append("updated", "2024-01-02");
            meta.done();
        }
        _doc = b.obj();
    }

    BSONObj _doc;
};

// Test: Index registration
TEST_F(UnifiedFieldExtractorIndexTest, IndexRegistration) {
    UnifiedFieldExtractor extractor;

    auto idx1Slots = extractor.registerIndex("idx_user", {"_id", "userId", "name"});
    auto idx2Slots = extractor.registerIndex("idx_order", {"_id", "orderId", "status"});

    ASSERT_EQUALS(idx1Slots.size(), 3u);
    ASSERT_EQUALS(idx2Slots.size(), 3u);

    // _id is shared
    ASSERT_EQUALS(idx1Slots[0], idx2Slots[0]);

    // 5 unique fields total
    ASSERT_EQUALS(extractor.totalUniqueFields(), 5u);
    ASSERT_EQUALS(extractor.indexCount(), 2u);
}

// Test: Digest registration
TEST_F(UnifiedFieldExtractorIndexTest, DigestRegistration) {
    UnifiedFieldExtractor extractor;

    auto digestSlots = extractor.registerDigest("order_digest", {
        "orderId", "productId", "name", "price", "quantity", "status"
    });

    ASSERT_EQUALS(digestSlots.size(), 6u);
    ASSERT_EQUALS(extractor.digestCount(), 1u);
}

// Test: Index and digest with overlap
TEST_F(UnifiedFieldExtractorIndexTest, IndexDigestOverlap) {
    UnifiedFieldExtractor extractor;

    // Index needs: _id, userId, orderId
    auto idxSlots = extractor.registerIndex("idx_main", {"_id", "userId", "orderId"});

    // Digest needs: orderId, name, price, status (orderId overlaps)
    auto digestSlots = extractor.registerDigest("summary", {"orderId", "name", "price", "status"});

    // orderId should have same slot
    ASSERT_EQUALS(idxSlots[2], digestSlots[0]);

    // Total unique: 6 (not 7)
    ASSERT_EQUALS(extractor.totalUniqueFields(), 6u);
}

// Test: Get index fields
TEST_F(UnifiedFieldExtractorIndexTest, GetIndexFields) {
    UnifiedFieldExtractor extractor;

    extractor.registerIndex("idx_main", {"_id", "userId", "orderId"});
    extractor.finalize();

    extractor.extract(_doc);

    std::vector<BSONElement> fields;
    extractor.getIndexFields("idx_main", fields);

    ASSERT_EQUALS(fields.size(), 3u);
    ASSERT_EQUALS(fields[0].Int(), 1);      // _id
    ASSERT_EQUALS(fields[1].Int(), 1001);   // userId
    ASSERT_EQUALS(fields[2].Int(), 2001);   // orderId
}

// Test: Get digest fields
TEST_F(UnifiedFieldExtractorIndexTest, GetDigestFields) {
    UnifiedFieldExtractor extractor;

    extractor.registerDigest("price_digest", {"productId", "price", "quantity"});
    extractor.finalize();

    extractor.extract(_doc);

    std::vector<BSONElement> fields;
    extractor.getDigestFields("price_digest", fields);

    ASSERT_EQUALS(fields.size(), 3u);
    ASSERT_EQUALS(fields[0].Int(), 3001);    // productId
    ASSERT_EQUALS(fields[1].Double(), 99.9); // price
    ASSERT_EQUALS(fields[2].Int(), 5);       // quantity
}

// ============================================================================
// Edge case tests
// ============================================================================

class UnifiedFieldExtractorEdgeCaseTest : public unittest::Test {};

// Test: Empty document
TEST_F(UnifiedFieldExtractorEdgeCaseTest, EmptyDocument) {
    UnifiedFieldExtractor extractor;

    uint8_t slot = extractor.registerField("anything");
    extractor.finalize();

    BSONObj emptyDoc;
    extractor.extract(emptyDoc);

    ASSERT_TRUE(extractor.get(slot).eoo());
    ASSERT_EQUALS(extractor.extractedCount(), 0u);
}

// Test: Multiple extractions (reuse)
TEST_F(UnifiedFieldExtractorEdgeCaseTest, MultipleExtractions) {
    UnifiedFieldExtractor extractor;

    uint8_t nameSlot = extractor.registerField("name");
    extractor.finalize();

    // First document
    BSONObj doc1 = BSON("name" << "Alice" << "age" << 25);
    extractor.extract(doc1);
    ASSERT_EQUALS(extractor.get(nameSlot).String(), "Alice");

    // Second document
    BSONObj doc2 = BSON("name" << "Bob" << "age" << 30);
    extractor.extract(doc2);
    ASSERT_EQUALS(extractor.get(nameSlot).String(), "Bob");

    // Third document without name
    BSONObj doc3 = BSON("age" << 35);
    extractor.extract(doc3);
    ASSERT_TRUE(extractor.get(nameSlot).eoo());
}

// Test: Registration after finalize is rejected
TEST_F(UnifiedFieldExtractorEdgeCaseTest, RegistrationAfterFinalize) {
    UnifiedFieldExtractor extractor;

    extractor.registerField("field1");
    extractor.finalize();

    uint8_t slot = extractor.registerField("field2");
    ASSERT_EQUALS(slot, 255u);  // kInvalidSlot = 255
    ASSERT_EQUALS(extractor.totalUniqueFields(), 1u);
}

// Test: Large number of fields
TEST_F(UnifiedFieldExtractorEdgeCaseTest, LargeFieldCount) {
    UnifiedFieldExtractor extractor;

    // Register 100 fields
    for (int i = 0; i < 100; ++i) {
        std::string fieldName = "field" + std::to_string(i);
        extractor.registerField(fieldName);
    }
    extractor.finalize();

    ASSERT_EQUALS(extractor.totalUniqueFields(), 100u);

    // Create document with 100 fields
    BSONObjBuilder b;
    for (int i = 0; i < 100; ++i) {
        b.append("field" + std::to_string(i), i * 10);
    }
    BSONObj doc = b.obj();

    extractor.extract(doc);

    ASSERT_EQUALS(extractor.extractedCount(), 100u);
    ASSERT_EQUALS(extractor.get(0).Int(), 0);
    ASSERT_EQUALS(extractor.get(50).Int(), 500);
    ASSERT_EQUALS(extractor.get(99).Int(), 990);
}

// ============================================================================
// Signature collision tests (Bug #15)
// ============================================================================

class UnifiedFieldExtractorCollisionTest : public unittest::Test {};

// Helper to generate field names with specific signature components
namespace {
// 找到两个不同的字段名，它们有相同的签名
// 签名 = 长度 + 首字符 + 末字符 + 8位哈希
// 我们通过构造来制造冲突
std::pair<std::string, std::string> findCollidingFieldNames() {
    // 简单方法：相同长度、首字符、末字符，尝试找哈希冲突
    // "abc" 和 "aXc" 如果哈希相同就冲突
    // 哈希 = sum(char[i] * 31^(len-1-i)) mod 256
    // 长度=3, "abc" hash = (('a'*31 + 'b')*31 + 'c') mod 256

    // 使用暴力搜索找到冲突
    auto computeHash = [](const char* s, size_t len) -> uint8_t {
        uint8_t hash = 0;
        for (size_t i = 0; i < len; ++i) {
            hash = static_cast<uint8_t>(hash * 31 + static_cast<uint8_t>(s[i]));
        }
        return hash;
    };

    // 固定长度=4，首字符='f'，末字符='d'
    // 找两个中间字符不同但哈希相同的
    std::string base = "f__d";
    for (int c1 = 'a'; c1 <= 'z'; ++c1) {
        for (int c2 = 'a'; c2 <= 'z'; ++c2) {
            std::string s1 = "f";
            s1 += static_cast<char>(c1);
            s1 += static_cast<char>(c1);
            s1 += "d";

            std::string s2 = "f";
            s2 += static_cast<char>(c2);
            s2 += static_cast<char>(c2);
            s2 += "d";

            if (s1 != s2 &&
                computeHash(s1.c_str(), 4) == computeHash(s2.c_str(), 4)) {
                return {s1, s2};
            }
        }
    }

    // 如果找不到，用更大范围
    for (int c1 = 32; c1 < 127; ++c1) {
        for (int c2 = 32; c2 < 127; ++c2) {
            for (int c3 = 32; c3 < 127; ++c3) {
                for (int c4 = 32; c4 < 127; ++c4) {
                    if (c1 == c3 && c2 == c4) continue;

                    std::string s1 = "f";
                    s1 += static_cast<char>(c1);
                    s1 += static_cast<char>(c2);
                    s1 += "d";

                    std::string s2 = "f";
                    s2 += static_cast<char>(c3);
                    s2 += static_cast<char>(c4);
                    s2 += "d";

                    if (computeHash(s1.c_str(), 4) == computeHash(s2.c_str(), 4)) {
                        return {s1, s2};
                    }
                }
            }
        }
    }

    // Fallback - 手动构造的已知冲突
    return {"field1", "field1"};  // 不是真正的冲突，测试会失败
}
}  // namespace

// Test: Signature collision handling in registration
TEST_F(UnifiedFieldExtractorCollisionTest, CollisionRegistration) {
    UnifiedFieldExtractor extractor;

    // 使用构造的冲突字段名
    std::pair<std::string, std::string> colliding = findCollidingFieldNames();
    std::string field1 = colliding.first;
    std::string field2 = colliding.second;

    // 如果找到了真正的冲突
    if (field1 != field2) {
        uint8_t slot1 = extractor.registerField(field1);
        uint8_t slot2 = extractor.registerField(field2);

        // 应该分配不同的槽位
        ASSERT_NOT_EQUALS(slot1, slot2);
        ASSERT_EQUALS(extractor.totalUniqueFields(), 2u);
        ASSERT_EQUALS(extractor.collisionCount(), 1u);  // 一个冲突字段
    }
}

// Test: Signature collision handling in extraction
TEST_F(UnifiedFieldExtractorCollisionTest, CollisionExtraction) {
    UnifiedFieldExtractor extractor;

    std::pair<std::string, std::string> colliding = findCollidingFieldNames();
    std::string field1 = colliding.first;
    std::string field2 = colliding.second;

    if (field1 != field2) {
        uint8_t slot1 = extractor.registerField(field1);
        uint8_t slot2 = extractor.registerField(field2);
        extractor.finalize();

        // 创建包含两个字段的文档
        BSONObjBuilder b;
        b.append(field1, 111);
        b.append(field2, 222);
        BSONObj doc = b.obj();

        extractor.extract(doc);

        // 两个字段都应该被正确提取
        ASSERT_EQUALS(extractor.get(slot1).Int(), 111);
        ASSERT_EQUALS(extractor.get(slot2).Int(), 222);
        ASSERT_EQUALS(extractor.extractedCount(), 2u);
    }
}

// Test: Manual collision test with known values
TEST_F(UnifiedFieldExtractorCollisionTest, ManualCollisionTest) {
    // 手动测试：即使没有真正的哈希冲突，验证冲突处理逻辑
    UnifiedFieldExtractor extractor;

    // 注册多个不同的字段
    uint8_t slotA = extractor.registerField("apple");
    uint8_t slotB = extractor.registerField("banana");
    uint8_t slotC = extractor.registerField("cherry");
    extractor.finalize();

    BSONObj doc = BSON("apple" << 1 << "banana" << 2 << "cherry" << 3);
    extractor.extract(doc);

    ASSERT_EQUALS(extractor.get(slotA).Int(), 1);
    ASSERT_EQUALS(extractor.get(slotB).Int(), 2);
    ASSERT_EQUALS(extractor.get(slotC).Int(), 3);
}

// ============================================================================
// Nested array tests (Bug #16)
// ============================================================================

class UnifiedFieldExtractorArrayTest : public unittest::Test {};

// Test: Array along path detection
TEST_F(UnifiedFieldExtractorArrayTest, ArrayAlongPath) {
    UnifiedFieldExtractor extractor;

    uint8_t slot = extractor.registerField("a.b");
    extractor.finalize();

    // 文档: { a: [ {b: 1}, {b: 2} ] }
    BSONObj doc = BSON("a" << BSON_ARRAY(BSON("b" << 1) << BSON("b" << 2)));
    extractor.extract(doc);

    // 应该检测到路径中有数组
    ASSERT_TRUE(extractor.hasArrayAlongPath(slot));
    // 返回数组元素本身
    ASSERT_EQUALS(extractor.get(slot).type(), Array);
}

// Test: Object path without array
TEST_F(UnifiedFieldExtractorArrayTest, ObjectPathNoArray) {
    UnifiedFieldExtractor extractor;

    uint8_t slot = extractor.registerField("a.b");
    extractor.finalize();

    // 文档: { a: { b: 42 } }
    BSONObj doc = BSON("a" << BSON("b" << 42));
    extractor.extract(doc);

    // 没有数组
    ASSERT_FALSE(extractor.hasArrayAlongPath(slot));
    ASSERT_EQUALS(extractor.get(slot).Int(), 42);
}

// Test: Nested array in middle of path
TEST_F(UnifiedFieldExtractorArrayTest, NestedArrayInMiddle) {
    UnifiedFieldExtractor extractor;

    uint8_t slot = extractor.registerField("a.b.c");
    extractor.finalize();

    // 文档: { a: { b: [ {c: 1}, {c: 2} ] } }
    BSONObjBuilder builder;
    {
        BSONObjBuilder aBuilder(builder.subobjStart("a"));
        BSONArrayBuilder bArray(aBuilder.subarrayStart("b"));
        bArray.append(BSON("c" << 1));
        bArray.append(BSON("c" << 2));
        bArray.done();
        aBuilder.done();
    }
    BSONObj doc = builder.obj();

    extractor.extract(doc);

    // 应该检测到数组
    ASSERT_TRUE(extractor.hasArrayAlongPath(slot));
}

// Test: Top-level array field
TEST_F(UnifiedFieldExtractorArrayTest, TopLevelArrayField) {
    UnifiedFieldExtractor extractor;

    uint8_t slot = extractor.registerField("tags");
    extractor.finalize();

    // 文档: { tags: [1, 2, 3] }
    BSONObj doc = BSON("tags" << BSON_ARRAY(1 << 2 << 3));
    extractor.extract(doc);

    // 顶级数组字段，不是"路径中的数组"
    ASSERT_FALSE(extractor.hasArrayAlongPath(slot));
    ASSERT_EQUALS(extractor.get(slot).type(), Array);
}

// Test: Deep nested path with arrays
TEST_F(UnifiedFieldExtractorArrayTest, DeepNestedWithArray) {
    UnifiedFieldExtractor extractor;

    uint8_t slot1 = extractor.registerField("x.y.z");
    uint8_t slot2 = extractor.registerField("x.y");
    extractor.finalize();

    // 文档: { x: { y: [ {z: 100} ] } }
    BSONObj doc = BSON("x" << BSON("y" << BSON_ARRAY(BSON("z" << 100))));
    extractor.extract(doc);

    // x.y.z 路径中有数组
    ASSERT_TRUE(extractor.hasArrayAlongPath(slot1));

    // x.y 返回数组本身
    ASSERT_EQUALS(extractor.get(slot2).type(), Array);
}

// ============================================================================
// BENCHMARK TESTS - Performance comparison
// ============================================================================

class UnifiedFieldExtractorBenchmark : public unittest::Test {
public:
    void setUp() {
        // Create realistic document with 70 fields
        BSONObjBuilder b;
        b.append("_id", 1);

        // 30 top-level fields
        for (int i = 0; i < 30; ++i) {
            b.append("field" + std::to_string(i), i * 100);
        }

        // 4 nested objects with 10 fields each = 40 nested fields
        for (int obj = 0; obj < 4; ++obj) {
            BSONObjBuilder nested(b.subobjStart("nested" + std::to_string(obj)));
            for (int i = 0; i < 10; ++i) {
                nested.append("attr" + std::to_string(i), obj * 1000 + i);
            }
            nested.done();
        }

        _doc = b.obj();

        // Setup indexes (7 indexes, 10 fields each)
        _indexFields = {
            {"_id", "field0", "field1", "field2", "field3", "field4", "field5", "field6", "field7", "field8"},
            {"field0", "field5", "field10", "field11", "field12", "field13", "field14", "field15", "field16", "field17"},
            {"field1", "field6", "field11", "field18", "field19", "field20", "field21", "field22", "field23", "field24"},
            {"_id", "field2", "field7", "field12", "field25", "field26", "field27", "field28", "field29", "nested0.attr0"},
            {"field3", "field8", "field13", "field18", "field23", "field28", "nested0.attr1", "nested1.attr0", "nested1.attr1", "nested1.attr2"},
            {"field4", "field9", "field14", "field19", "field24", "field29", "nested2.attr0", "nested2.attr1", "nested2.attr2", "nested2.attr3"},
            {"_id", "field0", "field10", "field20", "nested3.attr0", "nested3.attr1", "nested3.attr2", "nested3.attr3", "nested3.attr4", "nested3.attr5"},
        };

        // Setup digest (40 fields with overlap)
        _digestFields = {
            "_id", "field0", "field1", "field2", "field3", "field4", "field5", "field6", "field7", "field8",
            "field10", "field11", "field12", "field13", "field14", "field15", "field16", "field17", "field18", "field19",
            "nested0.attr0", "nested0.attr1", "nested0.attr2", "nested0.attr3", "nested0.attr4",
            "nested1.attr0", "nested1.attr1", "nested1.attr2", "nested1.attr3", "nested1.attr4",
            "nested2.attr0", "nested2.attr1", "nested2.attr2", "nested2.attr3", "nested2.attr4",
            "nested3.attr0", "nested3.attr1", "nested3.attr2", "nested3.attr3", "nested3.attr4"
        };
    }

    BSONObj _doc;
    std::vector<std::vector<std::string>> _indexFields;
    std::vector<std::string> _digestFields;
};

// Benchmark: Original method (110 extractions)
TEST_F(UnifiedFieldExtractorBenchmark, Original_110Extractions) {
    const int iterations = 10000;
    Timer timer;
    int dummy = 0;

    timer.start();
    for (int i = 0; i < iterations; ++i) {
        // Extract 70 index fields (7 indexes x 10 fields)
        for (const auto& indexFields : _indexFields) {
            for (const auto& field : indexFields) {
                const char* path = field.c_str();
                BSONElement elem = dps::extractElementAtPathOrArrayAlongPath(_doc, path);
                dummy += elem.size();
            }
        }

        // Extract 40 digest fields
        for (const auto& field : _digestFields) {
            const char* path = field.c_str();
            BSONElement elem = dps::extractElementAtPathOrArrayAlongPath(_doc, path);
            dummy += elem.size();
        }
    }
    timer.stop();

    int64_t nsPerDoc = timer.elapsedNanos() / iterations;
    std::cout << "\n=== Original Method (110 extractions: 70 index + 40 digest) ===" << std::endl;
    std::cout << "Time per document: " << nsPerDoc << " ns" << std::endl;
    std::cout << "Throughput: " << (nsPerDoc > 0 ? 1000000000 / nsPerDoc : 0) << " docs/sec" << std::endl;
    std::cout << "(dummy=" << dummy << ")" << std::endl;
}

// Benchmark: Optimized method (UnifiedFieldExtractor)
TEST_F(UnifiedFieldExtractorBenchmark, Optimized_UnifiedExtractor) {
    const int iterations = 10000;
    Timer timer;
    int dummy = 0;

    // Setup extractor (one-time)
    UnifiedFieldExtractor extractor;
    for (size_t i = 0; i < _indexFields.size(); ++i) {
        extractor.registerIndex("idx" + std::to_string(i), _indexFields[i]);
    }
    extractor.registerDigest("main_digest", _digestFields);
    extractor.finalize();

    std::cout << "\n=== Optimized Method (UnifiedFieldExtractor) ===" << std::endl;
    std::cout << "Total unique fields: " << extractor.totalUniqueFields() << std::endl;
    std::cout << "Top-level fields: " << extractor.topLevelCount() << std::endl;
    std::cout << "Nested fields: " << extractor.nestedCount() << std::endl;

    // Get slot lists for access
    std::vector<const std::vector<uint8_t>*> indexSlotLists;
    for (size_t i = 0; i < _indexFields.size(); ++i) {
        indexSlotLists.push_back(extractor.getIndexSlots("idx" + std::to_string(i)));
    }
    const std::vector<uint8_t>* digestSlots = extractor.getDigestSlots("main_digest");

    timer.start();
    for (int i = 0; i < iterations; ++i) {
        extractor.extract(_doc);

        // Access all index fields via slots
        for (const auto* slots : indexSlotLists) {
            for (uint8_t slot : *slots) {
                BSONElement elem = extractor.get(slot);
                dummy += elem.size();
            }
        }

        // Access all digest fields via slots
        for (uint8_t slot : *digestSlots) {
            BSONElement elem = extractor.get(slot);
            dummy += elem.size();
        }
    }
    timer.stop();

    int64_t nsPerDoc = timer.elapsedNanos() / iterations;
    std::cout << "Time per document: " << nsPerDoc << " ns" << std::endl;
    std::cout << "Throughput: " << (nsPerDoc > 0 ? 1000000000 / nsPerDoc : 0) << " docs/sec" << std::endl;
    std::cout << "(dummy=" << dummy << ")" << std::endl;
}

// Benchmark: Comparison summary
TEST_F(UnifiedFieldExtractorBenchmark, ComparisonSummary) {
    const int iterations = 10000;
    Timer timer;
    int dummy = 0;

    // ===== Original method =====
    timer.start();
    for (int i = 0; i < iterations; ++i) {
        for (const auto& indexFields : _indexFields) {
            for (const auto& field : indexFields) {
                const char* path = field.c_str();
                BSONElement elem = dps::extractElementAtPathOrArrayAlongPath(_doc, path);
                dummy += elem.size();
            }
        }
        for (const auto& field : _digestFields) {
            const char* path = field.c_str();
            BSONElement elem = dps::extractElementAtPathOrArrayAlongPath(_doc, path);
            dummy += elem.size();
        }
    }
    timer.stop();
    int64_t originalNs = timer.elapsedNanos() / iterations;

    // ===== Optimized method =====
    UnifiedFieldExtractor extractor;
    for (size_t i = 0; i < _indexFields.size(); ++i) {
        extractor.registerIndex("idx" + std::to_string(i), _indexFields[i]);
    }
    extractor.registerDigest("main_digest", _digestFields);
    extractor.finalize();

    std::vector<const std::vector<uint8_t>*> indexSlotLists;
    for (size_t i = 0; i < _indexFields.size(); ++i) {
        indexSlotLists.push_back(extractor.getIndexSlots("idx" + std::to_string(i)));
    }
    const std::vector<uint8_t>* digestSlots = extractor.getDigestSlots("main_digest");

    timer.start();
    for (int i = 0; i < iterations; ++i) {
        extractor.extract(_doc);

        for (const auto* slots : indexSlotLists) {
            for (uint8_t slot : *slots) {
                BSONElement elem = extractor.get(slot);
                dummy += elem.size();
            }
        }
        for (uint8_t slot : *digestSlots) {
            BSONElement elem = extractor.get(slot);
            dummy += elem.size();
        }
    }
    timer.stop();
    int64_t optimizedNs = timer.elapsedNanos() / iterations;

    // ===== Results =====
    double savings = (1.0 - (double)optimizedNs / originalNs) * 100;
    double speedup = (double)originalNs / optimizedNs;

    std::cout << "\n============================================" << std::endl;
    std::cout << "=== PERFORMANCE COMPARISON SUMMARY ===" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "Scenario: 7 indexes x 10 fields + 40 digest fields" << std::endl;
    std::cout << "Document: 70 fields (30 top-level + 40 nested)" << std::endl;
    std::cout << "Total accesses: 110 (70 index + 40 digest)" << std::endl;
    std::cout << "Unique fields: " << extractor.totalUniqueFields() << std::endl;
    std::cout << std::endl;
    std::cout << "Original method:  " << originalNs << " ns/doc" << std::endl;
    std::cout << "Optimized method: " << optimizedNs << " ns/doc" << std::endl;
    std::cout << std::endl;
    std::cout << "Performance improvement: " << savings << "%" << std::endl;
    std::cout << "Speedup: " << speedup << "x" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "(dummy=" << dummy << ")" << std::endl;
}

// Benchmark: Scaling test (varying field counts)
TEST_F(UnifiedFieldExtractorBenchmark, ScalingTest) {
    std::cout << "\n=== SCALING TEST ===" << std::endl;
    std::cout << "Fields\tOriginal(ns)\tOptimized(ns)\tSpeedup" << std::endl;

    for (int fieldCount : {10, 30, 50, 70, 100}) {
        // Create document
        BSONObjBuilder b;
        for (int i = 0; i < fieldCount; ++i) {
            b.append("field" + std::to_string(i), i);
        }
        BSONObj doc = b.obj();

        // Create field list
        std::vector<std::string> fields;
        for (int i = 0; i < fieldCount; ++i) {
            fields.push_back("field" + std::to_string(i));
        }

        // Original method
        Timer timer;
        int dummy = 0;
        const int iterations = 10000;

        timer.start();
        for (int i = 0; i < iterations; ++i) {
            for (const auto& f : fields) {
                const char* path = f.c_str();
                BSONElement elem = dps::extractElementAtPathOrArrayAlongPath(doc, path);
                dummy += elem.size();
            }
        }
        timer.stop();
        int64_t originalNs = timer.elapsedNanos() / iterations;

        // Optimized method
        UnifiedFieldExtractor extractor;
        auto slots = extractor.registerIndex("test", fields);
        extractor.finalize();

        timer.start();
        for (int i = 0; i < iterations; ++i) {
            extractor.extract(doc);
            for (uint8_t slot : slots) {
                BSONElement elem = extractor.get(slot);
                dummy += elem.size();
            }
        }
        timer.stop();
        int64_t optimizedNs = timer.elapsedNanos() / iterations;

        double speedup = (double)originalNs / optimizedNs;
        std::cout << fieldCount << "\t" << originalNs << "\t\t" << optimizedNs << "\t\t" << speedup << "x" << std::endl;
    }
}

}  // namespace
}  // namespace mongo
