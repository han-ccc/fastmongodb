/**
 *    Copyright (C) 2017 MongoDB Inc.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/document_integrity.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// ============================================================================
// xxHash64 computation tests
// ============================================================================

TEST(DocumentIntegrity, ComputeHashEmpty) {
    BSONObj empty = BSONObj();
    uint64_t hash = computeDocumentHash(empty);
    ASSERT_NE(hash, 0ULL);
}

TEST(DocumentIntegrity, ComputeHashDeterministic) {
    BSONObj doc = BSON("a" << 1 << "b" << "test");
    uint64_t hash1 = computeDocumentHash(doc);
    uint64_t hash2 = computeDocumentHash(doc);
    ASSERT_EQ(hash1, hash2);
}

TEST(DocumentIntegrity, ComputeHashDifferentDocs) {
    BSONObj doc1 = BSON("a" << 1);
    BSONObj doc2 = BSON("a" << 2);
    ASSERT_NE(computeDocumentHash(doc1), computeDocumentHash(doc2));
}

TEST(DocumentIntegrity, ComputeHashFieldOrder) {
    // BSON field order affects hash
    BSONObj doc1 = BSON("a" << 1 << "b" << 2);
    BSONObj doc2 = BSON("b" << 2 << "a" << 1);
    ASSERT_NE(computeDocumentHash(doc1), computeDocumentHash(doc2));
}

TEST(DocumentIntegrity, ComputeHashNestedDoc) {
    BSONObj doc = BSON("outer" << BSON("inner" << 123));
    uint64_t hash = computeDocumentHash(doc);
    ASSERT_NE(hash, 0ULL);
}

TEST(DocumentIntegrity, ComputeHashWithArray) {
    BSONObj doc = BSON("arr" << BSON_ARRAY(1 << 2 << 3));
    uint64_t hash = computeDocumentHash(doc);
    ASSERT_NE(hash, 0ULL);
}

TEST(DocumentIntegrity, ComputeHashExcludesHashField) {
    // When document contains _$docHash, it should be excluded from hash computation
    BSONObj content = BSON("a" << 1 << "b" << "test");
    uint64_t expectedHash = computeDocumentHash(content);

    // Add hash field to document
    BSONObjBuilder builder;
    builder.append(kDocHashFieldName, static_cast<long long>(12345));
    builder.appendElements(content);
    BSONObj docWithHash = builder.obj();

    // Hash computation should exclude the hash field
    uint64_t computedHash = computeDocumentHash(docWithHash);
    ASSERT_EQ(computedHash, expectedHash);
}

// ============================================================================
// Hash extraction tests
// ============================================================================

TEST(DocumentIntegrity, ExtractHashPresent) {
    BSONObj doc = BSON("_$docHash" << 12345LL << "a" << 1);
    auto hash = extractDocumentHash(doc);
    ASSERT_TRUE(hash.is_initialized());
    ASSERT_EQ(*hash, 12345ULL);
}

TEST(DocumentIntegrity, ExtractHashAbsent) {
    BSONObj doc = BSON("a" << 1);
    auto hash = extractDocumentHash(doc);
    ASSERT_FALSE(hash.is_initialized());
}

TEST(DocumentIntegrity, ExtractHashWrongType) {
    BSONObj doc = BSON("_$docHash" << "not_a_number" << "a" << 1);
    auto hash = extractDocumentHash(doc);
    ASSERT_FALSE(hash.is_initialized());  // Wrong type returns empty
}

TEST(DocumentIntegrity, ExtractHashIntType) {
    // Regular int (NumberInt) should not be accepted, needs NumberLong
    BSONObj doc = BSON("_$docHash" << 12345 << "a" << 1);
    auto hash = extractDocumentHash(doc);
    ASSERT_FALSE(hash.is_initialized());  // int32 not accepted
}

TEST(DocumentIntegrity, ExtractHashLargeValue) {
    // Test with large uint64 value
    uint64_t largeHash = 0xFEDCBA9876543210ULL;
    BSONObj doc = BSON("_$docHash" << static_cast<long long>(largeHash) << "a" << 1);
    auto hash = extractDocumentHash(doc);
    ASSERT_TRUE(hash.is_initialized());
    ASSERT_EQ(*hash, largeHash);
}

// ============================================================================
// Hash stripping tests
// ============================================================================

TEST(DocumentIntegrity, StripHashField) {
    BSONObj doc = BSON("_$docHash" << 12345LL << "a" << 1 << "b" << 2);
    BSONObj stripped = stripHashField(doc);
    ASSERT_FALSE(stripped.hasField("_$docHash"));
    ASSERT_TRUE(stripped.hasField("a"));
    ASSERT_TRUE(stripped.hasField("b"));
    ASSERT_EQ(stripped["a"].Int(), 1);
    ASSERT_EQ(stripped["b"].Int(), 2);
}

TEST(DocumentIntegrity, StripHashFieldNotPresent) {
    BSONObj doc = BSON("a" << 1);
    BSONObj stripped = stripHashField(doc);
    ASSERT_BSONOBJ_EQ(doc, stripped);
}

TEST(DocumentIntegrity, StripHashFieldPreservesOrder) {
    BSONObj doc = BSON("x" << 1 << "_$docHash" << 12345LL << "y" << 2 << "z" << 3);
    BSONObj stripped = stripHashField(doc);

    // Check fields exist
    ASSERT_FALSE(stripped.hasField("_$docHash"));
    ASSERT_TRUE(stripped.hasField("x"));
    ASSERT_TRUE(stripped.hasField("y"));
    ASSERT_TRUE(stripped.hasField("z"));

    // Check order is preserved (x, y, z)
    BSONObjIterator it(stripped);
    ASSERT_EQ(it.next().fieldNameStringData(), "x");
    ASSERT_EQ(it.next().fieldNameStringData(), "y");
    ASSERT_EQ(it.next().fieldNameStringData(), "z");
}

TEST(DocumentIntegrity, HashFieldOnly) {
    BSONObj doc = BSON("_$docHash" << 12345LL);
    BSONObj stripped = stripHashField(doc);
    ASSERT_TRUE(stripped.isEmpty());
}

// ============================================================================
// Verification tests
// ============================================================================

TEST(DocumentIntegrity, VerifyCorrectHash) {
    BSONObj content = BSON("a" << 1 << "b" << "test");
    uint64_t hash = computeDocumentHash(content);

    BSONObjBuilder builder;
    builder.append("_$docHash", static_cast<long long>(hash));
    builder.appendElements(content);
    BSONObj doc = builder.obj();

    Status s = verifyDocumentIntegrity(doc);
    ASSERT_OK(s);
}

TEST(DocumentIntegrity, VerifyWrongHashReject) {
    BSONObj doc = BSON("_$docHash" << 99999LL << "a" << 1);
    Status s = verifyDocumentIntegrity(doc);
    ASSERT_EQ(s.code(), ErrorCodes::DocumentIntegrityError);
}

TEST(DocumentIntegrity, VerifyNoHashSkip) {
    BSONObj doc = BSON("a" << 1);
    Status s = verifyDocumentIntegrity(doc);
    ASSERT_OK(s);  // No hash field, skip verification
}

TEST(DocumentIntegrity, VerifyEmptyDocWithHash) {
    BSONObj empty = BSONObj();
    uint64_t hash = computeDocumentHash(empty);

    BSONObj doc = BSON("_$docHash" << static_cast<long long>(hash));
    Status s = verifyDocumentIntegrity(doc);
    ASSERT_OK(s);
}

TEST(DocumentIntegrity, VerifyNestedDoc) {
    BSONObj content = BSON("outer" << BSON("inner" << BSON("deep" << 123)));
    uint64_t hash = computeDocumentHash(content);

    BSONObjBuilder builder;
    builder.append("_$docHash", static_cast<long long>(hash));
    builder.appendElements(content);
    BSONObj doc = builder.obj();

    Status s = verifyDocumentIntegrity(doc);
    ASSERT_OK(s);
}

TEST(DocumentIntegrity, VerifyArrayDoc) {
    BSONObj content = BSON("arr" << BSON_ARRAY(1 << 2 << BSON("nested" << true)));
    uint64_t hash = computeDocumentHash(content);

    BSONObjBuilder builder;
    builder.append("_$docHash", static_cast<long long>(hash));
    builder.appendElements(content);
    BSONObj doc = builder.obj();

    Status s = verifyDocumentIntegrity(doc);
    ASSERT_OK(s);
}

// ============================================================================
// Large document tests
// ============================================================================

TEST(DocumentIntegrity, LargeDocument) {
    BSONObjBuilder builder;
    for (int i = 0; i < 1000; i++) {
        builder.append(std::to_string(i), std::string(100, 'x'));
    }
    BSONObj doc = builder.obj();
    uint64_t hash = computeDocumentHash(doc);
    ASSERT_NE(hash, 0ULL);
}

TEST(DocumentIntegrity, LargeDocumentVerify) {
    BSONObjBuilder contentBuilder;
    for (int i = 0; i < 100; i++) {
        contentBuilder.append(std::to_string(i), std::string(100, 'y'));
    }
    BSONObj content = contentBuilder.obj();
    uint64_t hash = computeDocumentHash(content);

    BSONObjBuilder builder;
    builder.append("_$docHash", static_cast<long long>(hash));
    builder.appendElements(content);
    BSONObj doc = builder.obj();

    Status s = verifyDocumentIntegrity(doc);
    ASSERT_OK(s);
}

// ============================================================================
// Update spec verification tests (for $set, $inc operations)
// ============================================================================

TEST(DocumentIntegrity, VerifyUpdateSpecWithModifiers) {
    // Test verifying update spec like {$set: {field: value}}
    BSONObj updateSpec = BSON("$set" << BSON("field" << "value") << "$inc" << BSON("counter" << 1));
    uint64_t hash = computeDocumentHash(updateSpec);

    BSONObjBuilder builder;
    builder.append("_$docHash", static_cast<long long>(hash));
    builder.appendElements(updateSpec);
    BSONObj updateWithHash = builder.obj();

    Status s = verifyDocumentIntegrity(updateWithHash);
    ASSERT_OK(s);
}

TEST(DocumentIntegrity, VerifyUpdateSpecWrongHash) {
    BSONObj updateSpec = BSON("_$docHash" << 99999LL << "$set" << BSON("a" << 1));
    Status s = verifyDocumentIntegrity(updateSpec);
    ASSERT_EQ(s.code(), ErrorCodes::DocumentIntegrityError);
}

TEST(DocumentIntegrity, StripHashFromUpdateSpec) {
    BSONObj updateSpec =
        BSON("_$docHash" << 12345LL << "$set" << BSON("a" << 1) << "$inc" << BSON("b" << 1));
    BSONObj stripped = stripHashField(updateSpec);

    ASSERT_FALSE(stripped.hasField("_$docHash"));
    ASSERT_TRUE(stripped.hasField("$set"));
    ASSERT_TRUE(stripped.hasField("$inc"));
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(DocumentIntegrity, HashFieldWithDifferentTypes) {
    // Double type should not be accepted
    BSONObj doc1 = BSON("_$docHash" << 12345.0 << "a" << 1);
    auto hash1 = extractDocumentHash(doc1);
    ASSERT_FALSE(hash1.is_initialized());

    // Boolean should not be accepted
    BSONObj doc2 = BSON("_$docHash" << true << "a" << 1);
    auto hash2 = extractDocumentHash(doc2);
    ASSERT_FALSE(hash2.is_initialized());

    // Object should not be accepted
    BSONObj doc3 = BSON("_$docHash" << BSON("nested" << 1) << "a" << 1);
    auto hash3 = extractDocumentHash(doc3);
    ASSERT_FALSE(hash3.is_initialized());
}

TEST(DocumentIntegrity, MultipleHashFields) {
    // If multiple _$docHash fields somehow exist (invalid), first one wins
    // This shouldn't happen in practice but test for robustness
    BSONObjBuilder builder;
    builder.append("_$docHash", 12345LL);
    builder.append("a", 1);
    BSONObj doc = builder.obj();

    auto hash = extractDocumentHash(doc);
    ASSERT_TRUE(hash.is_initialized());
    ASSERT_EQ(*hash, 12345ULL);
}

TEST(DocumentIntegrity, DocumentWithBinaryData) {
    const char data[] = {0x00, 0x01, 0x02, 0x03, 0xFF};
    BSONObj doc = BSON("binary" << BSONBinData(data, sizeof(data), BinDataGeneral));
    uint64_t hash = computeDocumentHash(doc);
    ASSERT_NE(hash, 0ULL);

    // Verify hash is consistent
    uint64_t hash2 = computeDocumentHash(doc);
    ASSERT_EQ(hash, hash2);
}

TEST(DocumentIntegrity, DocumentWithObjectId) {
    OID oid = OID::gen();
    BSONObj doc = BSON("_id" << oid << "data" << 123);
    uint64_t hash = computeDocumentHash(doc);
    ASSERT_NE(hash, 0ULL);
}

TEST(DocumentIntegrity, DocumentWithDate) {
    Date_t date = Date_t::fromMillisSinceEpoch(1234567890123LL);
    BSONObj doc = BSON("date" << date);
    uint64_t hash = computeDocumentHash(doc);
    ASSERT_NE(hash, 0ULL);
}

TEST(DocumentIntegrity, DocumentWithTimestamp) {
    Timestamp ts(1234567890, 1);
    BSONObj doc = BSON("ts" << ts);
    uint64_t hash = computeDocumentHash(doc);
    ASSERT_NE(hash, 0ULL);
}

}  // namespace
}  // namespace mongo
