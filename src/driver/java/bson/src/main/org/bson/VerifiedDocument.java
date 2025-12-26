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

package org.bson;

import net.jpountz.xxhash.XXHash64;
import net.jpountz.xxhash.XXHashFactory;
import org.bson.codecs.BsonTypeClassMap;
import org.bson.codecs.DocumentCodec;
import org.bson.codecs.EncoderContext;
import org.bson.io.BasicOutputBuffer;

import java.util.Map;

/**
 * A Document that supports integrity verification through xxHash64.
 * <p>
 * This class extends {@link Document} and implements {@link IntegrityVerifiedDocument},
 * providing a snapshot-based hash computation with zero synchronization overhead.
 * </p>
 *
 * <h3>Design Philosophy</h3>
 * <ul>
 *   <li><b>Zero overhead</b>: No synchronized, no volatile</li>
 *   <li><b>Single computation</b>: lock() can only be called once (helps debugging)</li>
 *   <li><b>Fail-safe</b>: If document is modified after lock(), server rejects it</li>
 * </ul>
 *
 * <h3>Thread Safety</h3>
 * <p>
 * This class is <b>NOT</b> thread-safe. Caller is responsible for external synchronization
 * if concurrent access is needed. Typical usage is single-threaded.
 * </p>
 *
 * <h3>Example usage</h3>
 * <pre>
 * VerifiedDocument doc = new VerifiedDocument("name", "test");
 * doc.put("value", 123);
 * doc.lock();  // Compute hash (one-time only)
 * collection.insertOne(doc);  // Hash is automatically included
 * </pre>
 *
 * @since 3.4.2
 */
public class VerifiedDocument extends Document implements IntegrityVerifiedDocument {

    private static final long serialVersionUID = 1L;

    /** xxHash64 instance from lz4-java (thread-safe, stateless) */
    private static final XXHash64 XX_HASH_64 = XXHashFactory.fastestInstance().hash64();

    /** Shared codec instance (thread-safe, stateless) */
    private static final DocumentCodec CODEC = new DocumentCodec(
            org.bson.codecs.configuration.CodecRegistries.fromProviders(
                    new org.bson.codecs.ValueCodecProvider(),
                    new org.bson.codecs.BsonValueCodecProvider(),
                    new org.bson.codecs.DocumentCodecProvider()),
            new BsonTypeClassMap());

    /** Hash value. Null means not yet computed. Non-null means already computed. */
    private Long integrityHash = null;

    /**
     * Creates an empty VerifiedDocument instance.
     */
    public VerifiedDocument() {
        super();
    }

    /**
     * Create a VerifiedDocument instance initialized with the given key/value pair.
     *
     * @param key   key
     * @param value value
     */
    public VerifiedDocument(final String key, final Object value) {
        super(key, value);
    }

    /**
     * Creates a VerifiedDocument instance initialized with the given map.
     *
     * @param map initial map
     */
    public VerifiedDocument(final Map<String, Object> map) {
        super(map);
    }

    /**
     * Compute the integrity hash for the current document state.
     * <p>
     * This method can only be called <b>once</b>. A second call throws
     * {@link IllegalStateException} to help identify caller logic errors.
     * </p>
     * <p>
     * After calling lock(), the document can still be modified (no enforcement),
     * but if modified, the server will reject the document due to hash mismatch.
     * This is a deliberate design choice for zero-overhead operation.
     * </p>
     *
     * @throws IllegalStateException if called more than once
     */
    @Override
    public void lock() {
        if (integrityHash != null) {
            throw new IllegalStateException(
                    "lock() already called. Hash can only be computed once. " +
                    "If you need to recompute, create a new VerifiedDocument.");
        }

        // Use try-with-resources to ensure proper resource cleanup
        try (BasicOutputBuffer buffer = new BasicOutputBuffer();
             BsonBinaryWriter writer = new BsonBinaryWriter(buffer)) {

            // Encode using shared codec instance
            CODEC.encode(writer, this, EncoderContext.builder().build());

            byte[] bson = buffer.toByteArray();

            // Hash ONLY element bytes (skip 4-byte size header, exclude 1-byte terminator)
            // Matches server's OPTIMIZED path: hash remaining elements after _$docHash
            int elementStart = 4;
            int elementLength = bson.length - 5;  // total - size(4) - terminator(1)

            this.integrityHash = (elementLength > 0)
                    ? XX_HASH_64.hash(bson, elementStart, elementLength, 0)
                    : XX_HASH_64.hash(new byte[0], 0, 0, 0);
        }
    }

    @Override
    public boolean isLocked() {
        return integrityHash != null;
    }

    @Override
    public Long getIntegrityHash() {
        return integrityHash;
    }

    // No overrides for put/remove/clear - zero overhead
    // Caller is responsible for not modifying after lock()
    // If they do, server will reject (fail-safe)

    @Override
    public String toString() {
        return "VerifiedDocument{"
                + "hash=" + integrityHash
                + ", " + super.toString()
                + '}';
    }
}
