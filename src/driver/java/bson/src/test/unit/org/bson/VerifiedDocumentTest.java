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

import org.bson.codecs.BsonDocumentCodec;
import org.bson.codecs.DocumentCodec;
import org.bson.codecs.EncoderContext;
import org.bson.io.BasicOutputBuffer;
import org.junit.Test;

import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

import static org.junit.Assert.*;

public class VerifiedDocumentTest {

    // ============================================================================
    // Basic lock tests
    // ============================================================================

    @Test
    public void testLockComputesHash() {
        VerifiedDocument doc = new VerifiedDocument("a", 1);
        assertNull(doc.getIntegrityHash());
        assertFalse(doc.isLocked());

        doc.lock();

        assertTrue(doc.isLocked());
        assertNotNull(doc.getIntegrityHash());
    }

    @Test(expected = IllegalStateException.class)
    public void testDoubleLockThrows() {
        VerifiedDocument doc = new VerifiedDocument("a", 1);
        doc.lock();
        doc.lock();  // Second lock should throw
    }

    // ============================================================================
    // Zero-overhead design tests
    // ============================================================================

    @Test
    public void testModifyAfterLockAllowed() {
        // New design: modifications after lock() are allowed (no enforcement)
        // Server will reject if hash doesn't match (fail-safe)
        VerifiedDocument doc = new VerifiedDocument("a", 1);
        doc.lock();
        Long hash = doc.getIntegrityHash();

        // These should NOT throw - zero overhead design
        doc.put("b", 2);
        doc.append("c", 3);

        // Hash is unchanged (computed at lock time)
        assertEquals(hash, doc.getIntegrityHash());
    }

    // ============================================================================
    // Hash determinism tests
    // ============================================================================

    @Test
    public void testHashDeterministic() {
        VerifiedDocument doc1 = new VerifiedDocument("a", 1);
        doc1.append("b", "test");

        VerifiedDocument doc2 = new VerifiedDocument("a", 1);
        doc2.append("b", "test");

        doc1.lock();
        doc2.lock();

        assertEquals(doc1.getIntegrityHash(), doc2.getIntegrityHash());
    }

    @Test
    public void testHashDifferentForDifferentDocs() {
        VerifiedDocument doc1 = new VerifiedDocument("a", 1);
        VerifiedDocument doc2 = new VerifiedDocument("a", 2);

        doc1.lock();
        doc2.lock();

        assertNotEquals(doc1.getIntegrityHash(), doc2.getIntegrityHash());
    }

    @Test
    public void testHashFieldOrderMatters() {
        VerifiedDocument doc1 = new VerifiedDocument();
        doc1.append("a", 1).append("b", 2);

        VerifiedDocument doc2 = new VerifiedDocument();
        doc2.append("b", 2).append("a", 1);

        doc1.lock();
        doc2.lock();

        // BSON field order affects hash
        assertNotEquals(doc1.getIntegrityHash(), doc2.getIntegrityHash());
    }

    // ============================================================================
    // Complex document tests
    // ============================================================================

    @Test
    public void testNestedDocument() {
        VerifiedDocument inner = new VerifiedDocument("x", 100);
        VerifiedDocument doc = new VerifiedDocument("outer", inner);

        doc.lock();

        assertNotNull(doc.getIntegrityHash());
    }

    @Test
    public void testArrayField() {
        VerifiedDocument doc = new VerifiedDocument("arr", Arrays.asList(1, 2, 3));
        doc.lock();
        assertNotNull(doc.getIntegrityHash());
    }

    @Test
    public void testEmptyDocument() {
        VerifiedDocument doc = new VerifiedDocument();
        doc.lock();
        assertNotNull(doc.getIntegrityHash());
    }

    @Test
    public void testLargeDocument() {
        VerifiedDocument doc = new VerifiedDocument();
        for (int i = 0; i < 100; i++) {
            StringBuilder sb = new StringBuilder();
            for (int j = 0; j < 100; j++) {
                sb.append("x");
            }
            doc.append("field_" + i, sb.toString());
        }
        doc.lock();
        assertNotNull(doc.getIntegrityHash());
    }

    @Test
    public void testMapConstructor() {
        Map<String, Object> map = new HashMap<String, Object>();
        map.put("a", 1);
        map.put("b", "test");

        VerifiedDocument doc = new VerifiedDocument(map);
        doc.lock();

        assertNotNull(doc.getIntegrityHash());
    }

    // ============================================================================
    // Encoding tests
    // ============================================================================

    @Test
    public void testEncodingIncludesHash() {
        VerifiedDocument doc = new VerifiedDocument("a", 1);
        doc.lock();

        // Encode using DocumentCodec
        BasicOutputBuffer buffer = new BasicOutputBuffer();
        BsonBinaryWriter writer = new BsonBinaryWriter(buffer);
        new DocumentCodec().encode(writer, doc, EncoderContext.builder().build());
        writer.close();

        // Decode and verify hash field is present
        BsonBinaryReader reader = new BsonBinaryReader(buffer.getByteBuffers().get(0).asNIO());
        BsonDocument bsonDoc = new BsonDocumentCodec().decode(reader, org.bson.codecs.DecoderContext.builder().build());

        assertTrue(bsonDoc.containsKey("_$docHash"));
        assertEquals(doc.getIntegrityHash().longValue(), bsonDoc.getInt64("_$docHash").getValue());
    }

    @Test
    public void testEncodingWithoutLock() {
        VerifiedDocument doc = new VerifiedDocument("a", 1);
        // Don't lock

        // Encode using DocumentCodec
        BasicOutputBuffer buffer = new BasicOutputBuffer();
        BsonBinaryWriter writer = new BsonBinaryWriter(buffer);
        new DocumentCodec().encode(writer, doc, EncoderContext.builder().build());
        writer.close();

        // Decode and verify hash field is NOT present
        BsonBinaryReader reader = new BsonBinaryReader(buffer.getByteBuffers().get(0).asNIO());
        BsonDocument bsonDoc = new BsonDocumentCodec().decode(reader, org.bson.codecs.DecoderContext.builder().build());

        assertFalse(bsonDoc.containsKey("_$docHash"));
    }

    // ============================================================================
    // Field order verification tests
    // ============================================================================

    @Test
    public void testHashFieldIsFirstInEncoding() {
        VerifiedDocument doc = new VerifiedDocument("a", 1);
        doc.append("b", 2);
        doc.lock();

        // Encode using DocumentCodec
        BasicOutputBuffer buffer = new BasicOutputBuffer();
        BsonBinaryWriter writer = new BsonBinaryWriter(buffer);
        new DocumentCodec().encode(writer, doc, EncoderContext.builder().build());
        writer.close();

        // Get the raw bytes and verify _$docHash is the first field
        byte[] bytes = buffer.toByteArray();

        // BSON structure: [4-byte size][element1][element2]...[0x00 terminator]
        // First element starts at offset 4
        // Element format: [1-byte type][name\0][value]
        // _$docHash should be the first element (type 0x12 = int64)

        int offset = 4;  // Skip size header
        byte firstType = bytes[offset];
        assertEquals("First field should be int64 (0x12)", 0x12, firstType);

        // Read field name (null-terminated string starting at offset+1)
        StringBuilder fieldName = new StringBuilder();
        int nameOffset = offset + 1;
        while (bytes[nameOffset] != 0) {
            fieldName.append((char) bytes[nameOffset]);
            nameOffset++;
        }

        assertEquals("First field should be _$docHash", "_$docHash", fieldName.toString());
    }

    @Test
    public void testHashFieldBeforeIdInEncoding() {
        // Test that _$docHash comes before _id even when _id is set
        VerifiedDocument doc = new VerifiedDocument("_id", "my_id");
        doc.append("name", "test");
        doc.lock();

        // Encode using DocumentCodec (collectible mode)
        BasicOutputBuffer buffer = new BasicOutputBuffer();
        BsonBinaryWriter writer = new BsonBinaryWriter(buffer);
        new DocumentCodec().encode(writer, doc, EncoderContext.builder()
            .isEncodingCollectibleDocument(true).build());
        writer.close();

        // Get the raw bytes
        byte[] bytes = buffer.toByteArray();

        // First element should still be _$docHash
        int offset = 4;  // Skip size header
        byte firstType = bytes[offset];
        assertEquals("First field should be int64 (0x12)", 0x12, firstType);

        StringBuilder fieldName = new StringBuilder();
        int nameOffset = offset + 1;
        while (bytes[nameOffset] != 0) {
            fieldName.append((char) bytes[nameOffset]);
            nameOffset++;
        }

        assertEquals("First field should be _$docHash", "_$docHash", fieldName.toString());
    }
}
