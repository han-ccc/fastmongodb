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

/**
 * Interface for documents that support integrity verification through xxHash64.
 * <p>
 * Documents implementing this interface can be locked using {@link #lock()},
 * which computes an xxHash64 hash of the document's BSON representation.
 * Once locked, the document becomes immutable and the hash is transmitted
 * to the server for verification.
 * </p>
 *
 * @since 3.4.2
 */
public interface IntegrityVerifiedDocument {

    /**
     * The field name used to store the integrity hash in the BSON document.
     */
    String DOC_HASH_FIELD_NAME = "_$docHash";

    /**
     * Lock the document and compute its integrity hash.
     * <p>
     * After calling this method, the document becomes immutable.
     * Any attempt to modify the document will throw an {@link IllegalStateException}.
     * </p>
     * <p>
     * This method is idempotent - calling it multiple times has no additional effect.
     * </p>
     */
    void lock();

    /**
     * Check if the document has been locked.
     *
     * @return true if the document has been locked, false otherwise
     */
    boolean isLocked();

    /**
     * Get the integrity hash of this document.
     *
     * @return the xxHash64 hash value, or null if the document has not been locked
     */
    Long getIntegrityHash();
}
