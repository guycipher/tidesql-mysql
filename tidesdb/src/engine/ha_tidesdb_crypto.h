/*
  Copyright (c) 2026 TidesDB Corp.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef HA_TIDESDB_CRYPTO_H
#define HA_TIDESDB_CRYPTO_H

#include "ha_tidesdb.h"

/* data-at-rest key custody and the block cipher, behind one interface.
 *
 * the engine encrypts a row as [key version (4 LE)] [IV (16)] [ciphertext], and recovers the key a
 * row was written under from the version stamped in the blob, so rows stay readable across a key
 * rotation.  that format is what this interface exists to preserve: it is on disk already, and the
 * two servers reach a key by very different routes.
 *
 * one server publishes a versioned-key encryption service the engine can call directly.  MySQL
 * publishes no such service -- its keyring stores and retrieves keys but supplies none of the
 * cipher primitives -- so on MySQL the same contract is met by the two-tier arrangement InnoDB
 * uses, assembled here from the keyring component services and the server's own AES routines:
 *
 *   tier 1, the master key.  one 32-byte key per server, held in the keyring under
 *           TIDESQLKey-<server uuid>-<sequence>, generated on first use.  it never encrypts a row;
 *           it only wraps table keys.
 *
 *   tier 2, the table key.  32 random bytes per (encryption_key_id, version).  this is the key
 *           rows are actually encrypted with.  it is stored wrapped by the master key, so the
 *           plaintext table key exists only in memory.
 *
 * the wrapped table keys live in a reserved column family, which is the closest thing this engine
 * has to the tablespace header InnoDB keeps its encryption information in -- a column family is
 * already the unit of storage, addressing and configuration here, so a reserved one is the natural
 * home for engine-owned metadata rather than a new file format.
 *
 * what the arrangement buys is the same thing it buys InnoDB: rotating the master key re-wraps the
 * table keys and touches no row data.  rotating a table key mints a new version, and rows written
 * before it keep decrypting under the version stamped in their own blob.
 *
 * every entry point fails closed.  a key that cannot be fetched returns failure rather than
 * leaving a caller's stack buffer uninitialised, because the caller would otherwise encrypt under
 * garbage and write rows nobody can read back.
 */

/* returned by tdb_crypto_latest_key_version when no key is available for the id */
#define TDB_CRYPTO_KEY_VERSION_INVALID (~(unsigned int)0)

/* direction flags for tdb_crypto_crypt */
#define TDB_CRYPTO_FLAG_DECRYPT 0
#define TDB_CRYPTO_FLAG_ENCRYPT 1

/* the status every entry point that returns one uses.  zero for success matches the convention of
   the key services these wrap, so a delegation can pass a service result straight back. */
#define TDB_CRYPTO_STATUS_OK 0
#define TDB_CRYPTO_STATUS_FAILED 1

/**
 * tdb_crypto_init
 * acquire whatever key custody the server offers, once at plugin init.  on MySQL this acquires the
 * keyring reader, writer and generator component services and opens the reserved column family the
 * wrapped table keys live in; elsewhere it is a no-op because the encryption service is resolved by
 * the plugin loader.
 * @return true on success, or when the server offers no keyring at all -- absence of a keyring is
 *         not an error at init, it is only an error when a table asks to be encrypted
 */
bool tdb_crypto_init();

/**
 * tdb_crypto_deinit
 * release the key custody handles acquired by tdb_crypto_init.  safe to call when init failed or
 * was never called
 */
void tdb_crypto_deinit();

/**
 * tdb_crypto_available
 * whether key custody is usable, so create() can reject ENCRYPTED=YES with a clear error at table
 * creation rather than failing on the first row write
 * @return true when a key can be fetched or minted
 */
bool tdb_crypto_available();

/**
 * tdb_crypto_latest_key_version
 * the current version for an encryption key id, minting the key on first use.  the engine caches
 * the result per statement, so this may open the keyring and is not on the per-row path
 * @param key_id the table's ENCRYPTION_KEY_ID
 * @return the version to stamp into rows written now, or TDB_CRYPTO_KEY_VERSION_INVALID when no key
 *         is available
 */
unsigned int tdb_crypto_latest_key_version(unsigned int key_id);

/**
 * tdb_crypto_get_key
 * fetch the key bytes for one (id, version), which is the exact key a row stamped with that version
 * was written under
 * @param key_id the table's ENCRYPTION_KEY_ID
 * @param key_version the version read back from the row, or the current one when writing
 * @param key out buffer for the key bytes
 * @param klen in: the buffer's capacity; out: the key length actually written
 * @return 0 on success, non-zero when the version is unknown, the buffer is too small, or no
 *         keyring is loaded -- the buffer is left untouched on every failure
 */
unsigned int tdb_crypto_get_key(unsigned int key_id, unsigned int key_version, unsigned char *key,
                                unsigned int *klen);

/**
 * tdb_crypto_encrypted_length
 * the ciphertext length a plaintext of slen bytes produces, so the caller sizes its output buffer
 * once rather than growing it
 * @param slen the plaintext length
 * @param key_id the table's ENCRYPTION_KEY_ID
 * @param key_version the key version the row will be stamped with
 * @return the ciphertext length, block-padded
 */
unsigned int tdb_crypto_encrypted_length(unsigned int slen, unsigned int key_id,
                                         unsigned int key_version);

/**
 * tdb_crypto_crypt
 * encrypt or decrypt one buffer under a table key
 * @param src the input bytes
 * @param slen the input length
 * @param dst the output buffer, sized by tdb_crypto_encrypted_length when encrypting
 * @param dlen in: the output buffer's capacity; out: the bytes written
 * @param key the table key from tdb_crypto_get_key
 * @param klen the table key length
 * @param iv the initialisation vector, per row when encrypting
 * @param ivlen the iv length
 * @param flags TDB_CRYPTO_FLAG_ENCRYPT or TDB_CRYPTO_FLAG_DECRYPT
 * @param key_id the table's ENCRYPTION_KEY_ID, for diagnostics and for servers whose cipher
 *               selection depends on it
 * @param key_version the key version, same
 * @return 0 on success, non-zero on cipher failure or a short output buffer
 */
int tdb_crypto_crypt(const unsigned char *src, unsigned int slen, unsigned char *dst,
                     unsigned int *dlen, const unsigned char *key, unsigned int klen,
                     const unsigned char *iv, unsigned int ivlen, int flags, unsigned int key_id,
                     unsigned int key_version);

/**
 * tdb_crypto_random_bytes
 * fill a buffer with cryptographically strong random bytes, for row IVs and for minting a table key
 * @param buf the buffer to fill
 * @param len the byte count
 * @return true on success; a caller that gets false must not encrypt, because a predictable IV
 *         under a reused key leaks plaintext structure
 */
bool tdb_crypto_random_bytes(unsigned char *buf, size_t len);

/**
 * tdb_crypto_rotate_master_key
 * mint a new master key and re-wrap every stored table key under it.  row data is untouched, which
 * is the point of holding table keys wrapped rather than encrypting rows with the master key
 * directly
 * @return 0 on success, non-zero when the keyring refuses or a re-wrap fails, in which case the
 *         previous master key is left in place and the stored keys are unchanged
 */
int tdb_crypto_rotate_master_key();

/**
 * tdb_crypto_rotate_table_key
 * mint the next version of one encryption key, so rows written from now on are stamped with it
 * @param key_id the encryption key id to advance
 * @return 0 on success, non-zero when the key cannot be minted or recorded, in which case the
 *         previous version stays current
 *
 * Rows already written keep decrypting under the version stamped in their own blob, so this costs
 * nothing at rotation time and no row is read or rewritten.  Where the server owns key custody it
 * also owns versions, and this does nothing.
 */
int tdb_crypto_rotate_table_key(unsigned int key_id);

#endif /* HA_TIDESDB_CRYPTO_H */
