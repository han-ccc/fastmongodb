/*
 * Document Integrity Verification - End-to-End Test
 *
 * Tests the complete flow:
 * 1. Java Driver creates VerifiedDocument and calls lock()
 * 2. Document with _$docHash is sent to server
 * 3. Server verifies hash and strips field before storage
 */

package org.mongodb;

import com.mongodb.MongoClient;
import com.mongodb.MongoClientOptions;
import com.mongodb.ServerAddress;
import com.mongodb.client.MongoCollection;
import com.mongodb.client.MongoDatabase;
import com.mongodb.MongoWriteException;
import com.mongodb.MongoCommandException;

import org.bson.Document;
import org.bson.VerifiedDocument;
import org.bson.types.ObjectId;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.BeforeClass;
import org.junit.AfterClass;

import static org.junit.Assert.*;

/**
 * End-to-end tests for document integrity verification.
 *
 * Prerequisites:
 * - mongod running on localhost:27019
 * - --setParameter documentIntegrityVerification=true
 * - --storageEngine=rocksdb
 */
public class DocumentIntegrityE2ETest {

    private static MongoClient client;
    private static MongoDatabase db;
    private MongoCollection<Document> collection;

    @BeforeClass
    public static void setUpClass() {
        // Connect to test mongod
        client = new MongoClient(new ServerAddress("localhost", 27019));
        db = client.getDatabase("test_integrity");

        // Verify server is running with integrity verification enabled
        Document serverStatus = client.getDatabase("admin").runCommand(new Document("serverStatus", 1));
        System.out.println("Connected to MongoDB: " + serverStatus.get("version"));

        Document params = client.getDatabase("admin").runCommand(new Document("getParameter", 1).append("documentIntegrityVerification", 1));
        Boolean enabled = params.getBoolean("documentIntegrityVerification", false);
        System.out.println("Document integrity verification: " + (enabled ? "ENABLED" : "DISABLED"));

        if (!enabled) {
            System.err.println("WARNING: documentIntegrityVerification is not enabled!");
        }
    }

    @AfterClass
    public static void tearDownClass() {
        if (client != null) {
            client.close();
        }
    }

    @Before
    public void setUp() {
        collection = db.getCollection("test_" + System.currentTimeMillis());
    }

    @After
    public void tearDown() {
        if (collection != null) {
            collection.drop();
        }
    }

    // ==================== INSERT Tests ====================

    @Test
    public void testInsertWithoutHash() {
        // Regular document without hash should work
        Document doc = new Document("name", "no_hash").append("value", 123);
        collection.insertOne(doc);

        Document found = collection.find(new Document("name", "no_hash")).first();
        assertNotNull("Document should be inserted", found);
        assertEquals(123, found.getInteger("value").intValue());
    }

    @Test
    public void testInsertWithValidHash() {
        // VerifiedDocument with valid hash should work
        VerifiedDocument doc = new VerifiedDocument("name", "valid_hash");
        doc.put("value", 456);
        doc.lock();

        assertNotNull("Hash should be computed", doc.getIntegrityHash());
        System.out.println("Computed hash: " + doc.getIntegrityHash());

        collection.insertOne(doc);

        Document found = collection.find(new Document("name", "valid_hash")).first();
        assertNotNull("Document should be inserted", found);
        assertEquals(456, found.getInteger("value").intValue());

        // Hash field should be stripped
        assertFalse("_$docHash should be stripped", found.containsKey("_$docHash"));
    }

    @Test
    public void testInsertWithInvalidHash() {
        // Document with wrong hash should be rejected
        Document doc = new Document("_$docHash", 12345678901234L)
                .append("name", "invalid_hash")
                .append("value", 789);

        try {
            collection.insertOne(doc);
            fail("Should reject document with invalid hash");
        } catch (MongoWriteException e) {
            assertTrue("Error should be DocumentIntegrityError",
                    e.getMessage().contains("integrity") || e.getCode() == 203);
            System.out.println("Correctly rejected: " + e.getMessage());
        }
    }

    @Test
    public void testInsertWithWrongHashType() {
        // _$docHash with wrong type should be rejected
        Document doc = new Document("_$docHash", "not_a_number")
                .append("name", "wrong_type");

        try {
            collection.insertOne(doc);
            fail("Should reject document with wrong _$docHash type");
        } catch (MongoWriteException e) {
            System.out.println("Correctly rejected: " + e.getMessage());
        }
    }

    // ==================== UPDATE Tests ====================

    @Test
    public void testUpdateWithoutHash() {
        // Setup: insert a document
        collection.insertOne(new Document("_id", 1).append("name", "original").append("counter", 0));

        // Update without hash should work
        collection.updateOne(
                new Document("_id", 1),
                new Document("$set", new Document("counter", 1))
        );

        Document found = collection.find(new Document("_id", 1)).first();
        assertEquals(1, found.getInteger("counter").intValue());
    }

    @Test
    public void testReplaceWithValidHash() {
        // Setup
        collection.insertOne(new Document("_id", 1).append("name", "original"));

        // Replacement with valid hash
        VerifiedDocument replacement = new VerifiedDocument("_id", 1);
        replacement.put("name", "replaced");
        replacement.put("value", 999);
        replacement.lock();

        collection.replaceOne(new Document("_id", 1), replacement);

        Document found = collection.find(new Document("_id", 1)).first();
        assertEquals("replaced", found.getString("name"));
        assertEquals(999, found.getInteger("value").intValue());
        assertFalse("_$docHash should be stripped", found.containsKey("_$docHash"));
    }

    @Test
    public void testReplaceWithInvalidHash() {
        // Setup
        collection.insertOne(new Document("_id", 1).append("name", "original"));

        // Replacement with invalid hash
        Document replacement = new Document("_id", 1)
                .append("_$docHash", 9999999999L)
                .append("name", "bad_replace");

        try {
            collection.replaceOne(new Document("_id", 1), replacement);
            fail("Should reject replacement with invalid hash");
        } catch (MongoWriteException e) {
            assertTrue("Error should be DocumentIntegrityError",
                    e.getMessage().contains("integrity") || e.getCode() == 203);
        }
    }

    // ==================== findAndModify Tests ====================

    @Test
    public void testFindOneAndReplaceWithValidHash() {
        // Setup
        collection.insertOne(new Document("_id", 1).append("name", "fam_original").append("value", 100));

        // findOneAndReplace with valid hash
        VerifiedDocument replacement = new VerifiedDocument("_id", 1);
        replacement.put("name", "fam_replaced");
        replacement.put("value", 200);
        replacement.lock();

        Document old = collection.findOneAndReplace(new Document("_id", 1), replacement);

        assertNotNull("Should return old document", old);
        assertEquals("fam_original", old.getString("name"));

        Document found = collection.find(new Document("_id", 1)).first();
        assertEquals("fam_replaced", found.getString("name"));
        assertFalse("_$docHash should be stripped", found.containsKey("_$docHash"));
    }

    @Test
    public void testFindOneAndReplaceWithInvalidHash() {
        // Setup
        collection.insertOne(new Document("_id", 1).append("name", "fam_original"));

        // findOneAndReplace with invalid hash
        Document replacement = new Document("_id", 1)
                .append("_$docHash", 1111111111L)
                .append("name", "fam_bad");

        try {
            collection.findOneAndReplace(new Document("_id", 1), replacement);
            fail("Should reject findOneAndReplace with invalid hash");
        } catch (MongoCommandException e) {
            assertTrue("Error should be DocumentIntegrityError",
                    e.getMessage().contains("integrity") || e.getCode() == 203);
        }
    }

    @Test
    public void testFindOneAndUpdateWithModifiers() {
        // $inc and other modifiers don't need hash verification
        collection.insertOne(new Document("_id", 1).append("counter", 0));

        collection.findOneAndUpdate(
                new Document("_id", 1),
                new Document("$inc", new Document("counter", 1))
        );

        Document found = collection.find(new Document("_id", 1)).first();
        assertEquals(1, found.getInteger("counter").intValue());
    }

    // ==================== Batch Operations ====================

    @Test
    public void testBatchInsertWithValidHashes() {
        // Create multiple verified documents
        VerifiedDocument doc1 = new VerifiedDocument("batch_id", 0);
        doc1.put("data", "item_0");
        doc1.lock();

        VerifiedDocument doc2 = new VerifiedDocument("batch_id", 1);
        doc2.put("data", "item_1");
        doc2.lock();

        VerifiedDocument doc3 = new VerifiedDocument("batch_id", 2);
        doc3.put("data", "item_2");
        doc3.lock();

        collection.insertMany(java.util.Arrays.asList(doc1, doc2, doc3));

        assertEquals("All documents should be inserted", 3, collection.count());

        // Verify hash fields are stripped
        for (Document doc : collection.find()) {
            assertFalse("_$docHash should be stripped", doc.containsKey("_$docHash"));
        }
    }

    @Test
    public void testBatchInsertWithOneInvalidHash() {
        // One invalid hash in batch should fail
        VerifiedDocument doc1 = new VerifiedDocument("batch_id", 0);
        doc1.put("data", "item_0");
        doc1.lock();

        Document badDoc = new Document("batch_id", 1)
                .append("_$docHash", 777777777L)  // Invalid hash
                .append("data", "bad_item");

        VerifiedDocument doc3 = new VerifiedDocument("batch_id", 2);
        doc3.put("data", "item_2");
        doc3.lock();

        try {
            collection.insertMany(java.util.Arrays.asList(doc1, badDoc, doc3));
            fail("Batch with invalid hash should fail");
        } catch (Exception e) {
            System.out.println("Correctly rejected batch: " + e.getMessage());
        }
    }

    // ==================== Main ====================

    public static void main(String[] args) {
        System.out.println("=".repeat(60));
        System.out.println("Document Integrity Verification - Java E2E Tests");
        System.out.println("=".repeat(60));

        org.junit.runner.JUnitCore.main(DocumentIntegrityE2ETest.class.getName());
    }
}
