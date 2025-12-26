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

import org.bson.codecs.configuration.CodecRegistries;
import org.bson.codecs.configuration.CodecRegistry;
import org.junit.Test;

import java.util.Arrays;

import static org.junit.Assert.*;

public class VerifiedUpdateTest {

    private static final CodecRegistry CODEC_REGISTRY = CodecRegistries.fromProviders(
            new org.bson.codecs.ValueCodecProvider(),
            new org.bson.codecs.BsonValueCodecProvider(),
            new org.bson.codecs.DocumentCodecProvider()
    );

    // ============================================================================
    // Basic lock tests
    // ============================================================================

    @Test
    public void testLockComputesHash() {
        VerifiedUpdate update = new VerifiedUpdate().set("field", "value");

        assertNull(update.getIntegrityHash());
        assertFalse(update.isLocked());

        update.lock();

        assertTrue(update.isLocked());
        assertNotNull(update.getIntegrityHash());
    }

    @Test
    public void testLockIdempotent() {
        VerifiedUpdate update = new VerifiedUpdate().set("a", 1);
        update.lock();
        Long hash1 = update.getIntegrityHash();

        update.lock();
        Long hash2 = update.getIntegrityHash();

        assertEquals(hash1, hash2);
    }

    // ============================================================================
    // Immutability tests
    // ============================================================================

    @Test(expected = IllegalStateException.class)
    public void testSetAfterLockThrows() {
        VerifiedUpdate update = new VerifiedUpdate().set("a", 1);
        update.lock();
        update.set("b", 2);
    }

    @Test(expected = IllegalStateException.class)
    public void testIncAfterLockThrows() {
        VerifiedUpdate update = new VerifiedUpdate().inc("a", 1);
        update.lock();
        update.inc("a", 1);
    }

    @Test(expected = IllegalStateException.class)
    public void testUnsetAfterLockThrows() {
        VerifiedUpdate update = new VerifiedUpdate().set("a", 1);
        update.lock();
        update.unset("a");
    }

    @Test(expected = IllegalStateException.class)
    public void testPushAfterLockThrows() {
        VerifiedUpdate update = new VerifiedUpdate().set("a", 1);
        update.lock();
        update.push("arr", 1);
    }

    // ============================================================================
    // Hash determinism tests
    // ============================================================================

    @Test
    public void testHashDeterministic() {
        VerifiedUpdate u1 = new VerifiedUpdate().set("a", 1).inc("b", 2);
        VerifiedUpdate u2 = new VerifiedUpdate().set("a", 1).inc("b", 2);

        u1.lock();
        u2.lock();

        assertEquals(u1.getIntegrityHash(), u2.getIntegrityHash());
    }

    @Test
    public void testDifferentOpsHash() {
        VerifiedUpdate u1 = new VerifiedUpdate().set("a", 1);
        VerifiedUpdate u2 = new VerifiedUpdate().set("a", 2);

        u1.lock();
        u2.lock();

        assertNotEquals(u1.getIntegrityHash(), u2.getIntegrityHash());
    }

    @Test
    public void testDifferentOperatorsHash() {
        VerifiedUpdate u1 = new VerifiedUpdate().set("a", 1);
        VerifiedUpdate u2 = new VerifiedUpdate().inc("a", 1);

        u1.lock();
        u2.lock();

        assertNotEquals(u1.getIntegrityHash(), u2.getIntegrityHash());
    }

    // ============================================================================
    // Multiple modifiers tests
    // ============================================================================

    @Test
    public void testMultipleModifiers() {
        VerifiedUpdate update = new VerifiedUpdate()
                .set("name", "test")
                .inc("counter", 1)
                .unset("old");

        update.lock();
        assertNotNull(update.getIntegrityHash());
    }

    @Test
    public void testAllModifiers() {
        VerifiedUpdate update = new VerifiedUpdate()
                .set("field1", "value")
                .inc("field2", 1)
                .unset("field3")
                .push("field4", "item")
                .pull("field5", "item")
                .addToSet("field6", "item")
                .pop("field7", true)
                .rename("field8", "field8_new")
                .min("field9", 10)
                .max("field10", 100)
                .mul("field11", 2)
                .currentDate("field12");

        update.lock();
        assertNotNull(update.getIntegrityHash());
    }

    @Test
    public void testPushAll() {
        VerifiedUpdate update = new VerifiedUpdate()
                .pushAll("arr", Arrays.asList(1, 2, 3));

        update.lock();
        assertNotNull(update.getIntegrityHash());
    }

    // ============================================================================
    // Encoding tests
    // ============================================================================

    @Test
    public void testEncodingIncludesHash() {
        VerifiedUpdate update = new VerifiedUpdate().set("a", 1);
        update.lock();

        BsonDocument bson = update.toBsonDocument(BsonDocument.class, CODEC_REGISTRY);

        assertTrue(bson.containsKey("_$docHash"));
        assertTrue(bson.containsKey("$set"));
        assertEquals(update.getIntegrityHash().longValue(), bson.getInt64("_$docHash").getValue());
    }

    @Test
    public void testEncodingWithoutLock() {
        VerifiedUpdate update = new VerifiedUpdate().set("a", 1);
        // Don't lock

        BsonDocument bson = update.toBsonDocument(BsonDocument.class, CODEC_REGISTRY);

        assertFalse(bson.containsKey("_$docHash"));
        assertTrue(bson.containsKey("$set"));
    }

    @Test
    public void testEncodingMultipleOperators() {
        VerifiedUpdate update = new VerifiedUpdate()
                .set("name", "test")
                .inc("counter", 1)
                .unset("old");
        update.lock();

        BsonDocument bson = update.toBsonDocument(BsonDocument.class, CODEC_REGISTRY);

        assertTrue(bson.containsKey("_$docHash"));
        assertTrue(bson.containsKey("$set"));
        assertTrue(bson.containsKey("$inc"));
        assertTrue(bson.containsKey("$unset"));
    }

    // ============================================================================
    // Empty update tests
    // ============================================================================

    @Test
    public void testEmptyUpdate() {
        VerifiedUpdate update = new VerifiedUpdate();
        update.lock();
        assertNotNull(update.getIntegrityHash());
    }
}
