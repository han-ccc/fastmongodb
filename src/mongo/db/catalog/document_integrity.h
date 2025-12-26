/**
 *    Copyright (C) 2017 MongoDB Inc.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"

#include <boost/optional.hpp>
#include <cstdint>

namespace mongo {

/**
 * Field name for document integrity hash.
 * This field is stripped before storage.
 */
constexpr StringData kDocHashFieldName = "_$docHash"_sd;

/**
 * Compute xxHash64 of a BSON document, excluding the _$docHash field itself.
 *
 * @param doc The BSON document to hash
 * @return The 64-bit hash value
 */
uint64_t computeDocumentHash(const BSONObj& doc);

/**
 * Verify document integrity by comparing the embedded hash with computed hash.
 *
 * If the document doesn't contain a _$docHash field, verification is skipped
 * and Status::OK() is returned. If hash mismatch, returns DocumentIntegrityError.
 *
 * @param doc The document to verify (may contain _$docHash field)
 * @return Status::OK() if verified or no hash present;
 *         DocumentIntegrityError if hash mismatch
 */
Status verifyDocumentIntegrity(const BSONObj& doc);

/**
 * Extract the integrity hash from a document if present.
 *
 * @param doc The document to extract hash from
 * @return The hash value if present and valid type; boost::none otherwise
 */
boost::optional<uint64_t> extractDocumentHash(const BSONObj& doc);

/**
 * Create a copy of the document with the _$docHash field removed.
 *
 * If the document doesn't contain _$docHash, returns a copy of the original.
 *
 * @param doc The document to strip
 * @return A new BSONObj without the hash field
 */
BSONObj stripHashField(const BSONObj& doc);

/**
 * Check if document integrity verification is enabled via server parameter.
 */
bool isIntegrityVerificationEnabled();

}  // namespace mongo
