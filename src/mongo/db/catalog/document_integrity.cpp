/**
 *    Copyright (C) 2017 MongoDB Inc.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/document_integrity.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"
#include "mongo/util/hex.h"

// Hardware-accelerated CRC32C (SSE4.2)
#include "third_party/crc32c/crc32c.h"

namespace mongo {

// Server parameter for document integrity verification
MONGO_EXPORT_SERVER_PARAMETER(documentIntegrityVerification, bool, false);

uint64_t computeDocumentHash(const BSONObj& doc) {
    // Fast path: no hash field, hash elements only (skip 4-byte header and 1-byte terminator)
    // This matches the Python client computation: raw[4:-1]
    if (!doc.hasField(kDocHashFieldName)) {
        const char* elements = doc.objdata() + 4;  // Skip 4-byte length header
        size_t elementsSize = doc.objsize() - 5;   // Exclude header (4) and terminator (1)
        if (elementsSize > 0) {
            return mongo::crc32c::compute(elements, elementsSize);
        } else {
            return 0;
        }
    }

    // Hybrid strategy: check if _$docHash is the first field
    BSONObjIterator it(doc);
    if (it.more()) {
        BSONElement firstElem = it.next();
        if (firstElem.fieldNameStringData() == kDocHashFieldName) {
            // Optimized path: _$docHash is first field (official driver)
            // Skip hash element, hash remaining bytes directly
            const char* afterHashElem = firstElem.rawdata() + firstElem.size();
            const char* docEnd = doc.objdata() + doc.objsize();
            size_t remainingSize = docEnd - afterHashElem - 1;  // -1 for terminator

            if (remainingSize > 0) {
                return mongo::crc32c::compute(afterHashElem, remainingSize);
            } else {
                return 0;
            }
        }
    }

    // Compatible path: _$docHash is not first field (third-party client)
    // Rebuild clean document and hash elements only
    BSONObjBuilder builder;
    for (const auto& elem : doc) {
        if (elem.fieldNameStringData() != kDocHashFieldName) {
            builder.append(elem);
        }
    }
    BSONObj cleanDoc = builder.obj();

    // Hash elements only (skip header and terminator)
    const char* elements = cleanDoc.objdata() + 4;
    size_t elementsSize = cleanDoc.objsize() - 5;
    if (elementsSize > 0) {
        return mongo::crc32c::compute(elements, elementsSize);
    } else {
        return 0;
    }
}

boost::optional<uint64_t> extractDocumentHash(const BSONObj& doc) {
    BSONElement hashElem = doc[kDocHashFieldName];
    if (hashElem.eoo()) {
        return boost::none;
    }

    // Hash must be a 64-bit integer (NumberLong in BSON)
    if (hashElem.type() != NumberLong) {
        return boost::none;
    }

    return static_cast<uint64_t>(hashElem.Long());
}

Status verifyDocumentIntegrity(const BSONObj& doc) {
    auto expectedHash = extractDocumentHash(doc);
    if (!expectedHash) {
        // Check if field exists but has wrong type (reserved field misuse)
        if (doc.hasField(kDocHashFieldName)) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << kDocHashFieldName
                                        << " is a reserved field and must be a NumberLong");
        }
        // No hash field present, skip verification
        return Status::OK();
    }

    uint64_t actualHash = computeDocumentHash(doc);

    if (actualHash != *expectedHash) {
        return Status(ErrorCodes::DocumentIntegrityError,
                      str::stream() << "Document integrity verification failed. Expected hash: "
                                    << *expectedHash << ", actual hash: " << actualHash);
    }

    return Status::OK();
}

BSONObj stripHashField(const BSONObj& doc) {
    if (!doc.hasField(kDocHashFieldName)) {
        return doc.copy();
    }

    BSONObjBuilder builder;
    for (const auto& elem : doc) {
        if (elem.fieldNameStringData() != kDocHashFieldName) {
            builder.append(elem);
        }
    }
    return builder.obj();
}

bool isIntegrityVerificationEnabled() {
    return documentIntegrityVerification.load();
}

}  // namespace mongo
