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
import org.bson.codecs.DocumentCodec;
import org.bson.codecs.EncoderContext;
import org.bson.codecs.configuration.CodecRegistry;
import org.bson.conversions.Bson;
import org.bson.io.BasicOutputBuffer;

import java.util.List;

/**
 * A verified update specification that supports integrity verification.
 * <p>
 * This class allows building update operations ($set, $inc, $unset, etc.)
 * with integrity verification through xxHash64.
 * </p>
 *
 * <p>Example usage:</p>
 * <pre>
 * VerifiedUpdate update = new VerifiedUpdate()
 *     .set("name", "test")
 *     .inc("counter", 1);
 * update.lock();  // Compute hash of the update spec
 * collection.updateOne(filter, update);  // Hash is included in the update
 * </pre>
 *
 * @since 3.4.2
 */
public class VerifiedUpdate implements Bson, IntegrityVerifiedDocument {

    /** Thread-safe xxHash64 instance from lz4-java (verified compatible with C++ xxHash) */
    private static final XXHash64 XX_HASH_64 = XXHashFactory.fastestInstance().hash64();

    private final Document updateSpec = new Document();
    private volatile boolean locked = false;
    private volatile Long integrityHash = null;

    /**
     * Creates an empty VerifiedUpdate instance.
     */
    public VerifiedUpdate() {
    }

    /**
     * Set a field to a value.
     *
     * @param field the field name
     * @param value the value
     * @return this
     */
    public synchronized VerifiedUpdate set(final String field, final Object value) {
        checkNotLocked();
        getOrCreateModifier("$set").put(field, value);
        return this;
    }

    /**
     * Increment a field by a value.
     *
     * @param field the field name
     * @param value the value to increment by
     * @return this
     */
    public synchronized VerifiedUpdate inc(final String field, final Number value) {
        checkNotLocked();
        getOrCreateModifier("$inc").put(field, value);
        return this;
    }

    /**
     * Unset a field (remove it from the document).
     *
     * @param field the field name
     * @return this
     */
    public synchronized VerifiedUpdate unset(final String field) {
        checkNotLocked();
        getOrCreateModifier("$unset").put(field, "");
        return this;
    }

    /**
     * Push a value to an array field.
     *
     * @param field the field name
     * @param value the value to push
     * @return this
     */
    public synchronized VerifiedUpdate push(final String field, final Object value) {
        checkNotLocked();
        getOrCreateModifier("$push").put(field, value);
        return this;
    }

    /**
     * Push multiple values to an array field.
     *
     * @param field  the field name
     * @param values the values to push
     * @return this
     */
    public synchronized VerifiedUpdate pushAll(final String field, final List<?> values) {
        checkNotLocked();
        getOrCreateModifier("$push").put(field, new Document("$each", values));
        return this;
    }

    /**
     * Pull a value from an array field.
     *
     * @param field the field name
     * @param value the value to pull
     * @return this
     */
    public synchronized VerifiedUpdate pull(final String field, final Object value) {
        checkNotLocked();
        getOrCreateModifier("$pull").put(field, value);
        return this;
    }

    /**
     * Add a value to an array field only if it doesn't already exist.
     *
     * @param field the field name
     * @param value the value to add
     * @return this
     */
    public synchronized VerifiedUpdate addToSet(final String field, final Object value) {
        checkNotLocked();
        getOrCreateModifier("$addToSet").put(field, value);
        return this;
    }

    /**
     * Pop the first or last element from an array field.
     *
     * @param field the field name
     * @param first true to pop first element, false to pop last
     * @return this
     */
    public synchronized VerifiedUpdate pop(final String field, final boolean first) {
        checkNotLocked();
        getOrCreateModifier("$pop").put(field, first ? -1 : 1);
        return this;
    }

    /**
     * Rename a field.
     *
     * @param oldName the old field name
     * @param newName the new field name
     * @return this
     */
    public synchronized VerifiedUpdate rename(final String oldName, final String newName) {
        checkNotLocked();
        getOrCreateModifier("$rename").put(oldName, newName);
        return this;
    }

    /**
     * Set a field to the minimum of its current value and the specified value.
     *
     * @param field the field name
     * @param value the value to compare
     * @return this
     */
    public synchronized VerifiedUpdate min(final String field, final Object value) {
        checkNotLocked();
        getOrCreateModifier("$min").put(field, value);
        return this;
    }

    /**
     * Set a field to the maximum of its current value and the specified value.
     *
     * @param field the field name
     * @param value the value to compare
     * @return this
     */
    public synchronized VerifiedUpdate max(final String field, final Object value) {
        checkNotLocked();
        getOrCreateModifier("$max").put(field, value);
        return this;
    }

    /**
     * Multiply a field by a value.
     *
     * @param field the field name
     * @param value the multiplier
     * @return this
     */
    public synchronized VerifiedUpdate mul(final String field, final Number value) {
        checkNotLocked();
        getOrCreateModifier("$mul").put(field, value);
        return this;
    }

    /**
     * Set the current date on a field.
     *
     * @param field the field name
     * @return this
     */
    public synchronized VerifiedUpdate currentDate(final String field) {
        checkNotLocked();
        getOrCreateModifier("$currentDate").put(field, true);
        return this;
    }

    /**
     * Set the current timestamp on a field.
     *
     * @param field the field name
     * @return this
     */
    public synchronized VerifiedUpdate currentTimestamp(final String field) {
        checkNotLocked();
        getOrCreateModifier("$currentDate").put(field, new Document("$type", "timestamp"));
        return this;
    }

    @Override
    public synchronized void lock() {
        if (locked) {
            return;
        }

        // Use try-with-resources to ensure proper resource cleanup
        try (BasicOutputBuffer buffer = new BasicOutputBuffer();
             BsonBinaryWriter writer = new BsonBinaryWriter(buffer)) {

            new DocumentCodec().encode(writer, updateSpec, EncoderContext.builder().build());

            // Compute xxHash64 using lz4-java (compatible with C++ xxHash)
            byte[] bytes = buffer.toByteArray();
            this.integrityHash = XX_HASH_64.hash(bytes, 0, bytes.length, 0);
            this.locked = true;
        }
    }

    @Override
    public boolean isLocked() {
        return locked;
    }

    @Override
    public Long getIntegrityHash() {
        return integrityHash;
    }

    @Override
    public <TDocument> BsonDocument toBsonDocument(final Class<TDocument> documentClass,
                                                    final CodecRegistry codecRegistry) {
        BsonDocument bson = new BsonDocument();

        // If locked, add the hash field first
        if (locked && integrityHash != null) {
            bson.append(DOC_HASH_FIELD_NAME, new BsonInt64(integrityHash));
        }

        // Add update operators
        for (String key : updateSpec.keySet()) {
            Object value = updateSpec.get(key);
            if (value instanceof Document) {
                BsonDocument nested = new BsonDocument();
                Document doc = (Document) value;
                for (String nestedKey : doc.keySet()) {
                    nested.append(nestedKey, toBsonValue(doc.get(nestedKey), codecRegistry));
                }
                bson.append(key, nested);
            } else {
                bson.append(key, toBsonValue(value, codecRegistry));
            }
        }

        return bson;
    }

    private BsonValue toBsonValue(final Object value, final CodecRegistry codecRegistry) {
        if (value == null) {
            return BsonNull.VALUE;
        } else if (value instanceof BsonValue) {
            return (BsonValue) value;
        } else if (value instanceof String) {
            return new BsonString((String) value);
        } else if (value instanceof Integer) {
            return new BsonInt32((Integer) value);
        } else if (value instanceof Long) {
            return new BsonInt64((Long) value);
        } else if (value instanceof Double) {
            return new BsonDouble((Double) value);
        } else if (value instanceof Boolean) {
            return new BsonBoolean((Boolean) value);
        } else if (value instanceof Document) {
            Document doc = (Document) value;
            return doc.toBsonDocument(BsonDocument.class, codecRegistry);
        } else if (value instanceof List) {
            BsonArray array = new BsonArray();
            for (Object item : (List<?>) value) {
                array.add(toBsonValue(item, codecRegistry));
            }
            return array;
        } else {
            // Fallback: use document wrapper
            return new BsonDocumentWrapper<>(value, codecRegistry.get((Class<Object>) value.getClass()));
        }
    }

    // checkNotLocked() is always called within synchronized methods, so no race condition
    private void checkNotLocked() {
        if (locked) {
            throw new IllegalStateException("Update is locked and cannot be modified");
        }
    }

    @SuppressWarnings("unchecked")
    private Document getOrCreateModifier(final String op) {
        Object existing = updateSpec.get(op);
        if (existing instanceof Document) {
            return (Document) existing;
        }
        Document modifier = new Document();
        updateSpec.put(op, modifier);
        return modifier;
    }

    @Override
    public String toString() {
        return "VerifiedUpdate{"
                + "locked=" + locked
                + ", hash=" + integrityHash
                + ", updateSpec=" + updateSpec
                + '}';
    }
}
