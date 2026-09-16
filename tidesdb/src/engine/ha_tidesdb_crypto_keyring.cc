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

/* key custody for a server whose keyring stores keys but supplies no cipher.
 *
 * the contract in ha_tidesdb_crypto.h is met here by the two-tier arrangement InnoDB uses, built
 * from the keyring component services and the server's own AES routines:
 *
 *   tier 1  a master key per server, held in the keyring, which only ever wraps table keys
 *   tier 2  a table key per (encryption id, version), which is what rows are encrypted with, held
 *           wrapped so its plaintext exists only in memory
 *
 * the wrapped table keys live in a reserved column family.  a column family is already this
 * engine's unit of storage, addressing and configuration, so a reserved one is the natural home for
 * engine-owned metadata and the closest counterpart to the tablespace header InnoDB keeps its
 * encryption information in.  the byte layouts it is addressed by live in the server-free
 * src/core/crypto_keyenc module, where they are unit-tested on their own.
 *
 * rotating the master key re-wraps the table keys and reads no row data, which is the whole reason
 * table keys are held wrapped rather than rows being encrypted under the master key directly.
 *
 * the delegating backend lives in ha_tidesdb_crypto_service.cc; CMakeLists.txt compiles exactly one
 * of the two, which is why neither file carries a preprocessor branch. */

/* the engine interface first: it pulls in the server prelude the rest of this file depends on, and
   it declares size_t, which the keyring component headers below use without declaring it
   themselves.  including them ahead of it breaks the build on their own declarations. */
#include "src/engine/ha_tidesdb_crypto.h"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <mysql/components/services/keyring_generator.h>
#include <mysql/components/services/keyring_reader_with_status.h>
#include <mysql/components/services/keyring_writer.h>
#include <mysql/service_plugin_registry.h>

#include "log.h"
#include "my_aes.h"
#include "my_rnd.h"
#include "mysqld.h"
#include "src/core/crypto_keyenc.h"
#include "src/handler/ha_tidesdb_internal.h"

namespace keyenc = tidesdb::crypto_keyenc;

/* ******************** module state ******************** */

/* guards the master generation, the service handles and the reserved family handle.  every entry
   point that reaches key custody is off the per-row path -- the engine caches a statement's key
   version, and the row paths hold the key bytes for the statement -- so one mutex costs nothing and
   makes mint-on-first-use race-free. */
static std::mutex tdb_crypto_mtx;

static SERVICE_TYPE(registry) *tdb_registry = nullptr;
static SERVICE_TYPE(keyring_reader_with_status) *tdb_keyring_reader = nullptr;
static SERVICE_TYPE(keyring_writer) *tdb_keyring_writer = nullptr;
static SERVICE_TYPE(keyring_generator) *tdb_keyring_generator = nullptr;

/* the reserved family the wrapped table keys live in, opened once at init. */
static tidesdb_column_family_t *tdb_crypto_cf = nullptr;

/* the master generation in force, or GENERATION_NONE before it is read from the store. */
static uint32_t tdb_master_generation = keyenc::GENERATION_NONE;

/* ******************** names and sizes ******************** */

/* the reserved family's name.  a user table's family name is built as <db>__<table> from a path the
   server hands the handler and a database component never arrives empty, so a name that leads with
   the separator cannot collide with one. */
static constexpr const char TDB_CRYPTO_CF_NAME[] = "__tidesql_crypto";
static_assert(TIDESQL_RESERVED_CF_PREFIX_LEN == 2,
              "the reserved-family prefix must match what the status listing filters on");

/* the keyring scopes a key by (data id, auth id).  the engine owns its keys for the whole server
   rather than per user, so the auth id is empty. */
static constexpr const char TDB_KEYRING_AUTH_ID[] = "";

/* the keyring's type label.  a backend may key storage decisions off it, and a component offering
   the AES service must accept AES-typed data. */
static constexpr const char TDB_KEYRING_DATA_TYPE[] = "AES";

/* the master key's name stem.  the server uuid and a generation number complete it, so keys minted
   by different servers never collide in a shared keyring backend. */
static constexpr const char TDB_MASTER_KEY_PREFIX[] = "TIDESQLKey";
static constexpr const char TDB_MASTER_KEY_SEP[] = "-";

/* master and table keys are both AES-256. */
static constexpr size_t TDB_CRYPTO_KEY_LEN = 32;

/* the largest type label the keyring will hand back; anything longer is a backend the engine does
   not understand and the fetch is failed rather than truncated. */
static constexpr size_t TDB_KEYRING_TYPE_BUF_LEN = 32;

/* rows carry their own IV so CBC is the row cipher.  the wrap is one block-aligned random key with
   no structure for ECB to leak, which is the same split InnoDB makes. */
static constexpr my_aes_opmode TDB_ROW_CIPHER = my_aes_256_cbc;
static constexpr my_aes_opmode TDB_WRAP_CIPHER = my_aes_256_ecb;

/* my_aes_encrypt and my_aes_decrypt report failure with a negative length. */
static constexpr int TDB_AES_FAILED = 0;

/* ******************** helpers ******************** */

/**
 * tdb_master_key_name
 * the keyring identifier for one master generation
 * @param generation the master generation
 * @return the identifier, TIDESQLKey-<server uuid>-<generation>
 */
static std::string tdb_master_key_name(uint32_t generation)
{
    return std::string(TDB_MASTER_KEY_PREFIX) + TDB_MASTER_KEY_SEP + server_uuid +
           TDB_MASTER_KEY_SEP + std::to_string(generation);
}

/**
 * tdb_store_get
 * read one key from the reserved family on its own transaction
 * @param key the encoded store key
 * @param key_len the store key length
 * @param out out -- receives the value bytes, cleared first and left empty on any failure
 * @return true when the key was found and read
 */
static bool tdb_store_get(const uint8_t *key, size_t key_len, std::string &out)
{
    out.clear();
    if (!tdb_crypto_cf || !tdb_global || !key || key_len == 0) return false;

    tidesdb_txn_t *txn = nullptr;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return false;

    uint8_t *value = nullptr;
    size_t value_len = 0;
    const int rc = tidesdb_txn_get(txn, tdb_crypto_cf, key, key_len, &value, &value_len);
    if (rc == TDB_SUCCESS && value != nullptr && value_len > 0)
    {
        out.assign(reinterpret_cast<const char *>(value), value_len);
        tidesdb_free(value);
    }

    (void)tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
    return !out.empty();
}

/**
 * tdb_store_put
 * write one key to the reserved family on its own transaction, because key custody has to become
 * durable independently of whatever user transaction happens to be in flight
 * @param key the encoded store key
 * @param key_len the store key length
 * @param value the value bytes
 * @param value_len the value length
 * @return true when the write committed
 */
static bool tdb_store_put(const uint8_t *key, size_t key_len, const uint8_t *value,
                          size_t value_len)
{
    if (!tdb_crypto_cf || !tdb_global || !key || key_len == 0 || !value || value_len == 0)
        return false;

    tidesdb_txn_t *txn = nullptr;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return false;

    bool ok = tidesdb_txn_put(txn, tdb_crypto_cf, key, key_len, value, value_len,
                              TIDESDB_TTL_NONE) == TDB_SUCCESS;
    if (ok)
        ok = tidesdb_txn_commit(txn) == TDB_SUCCESS;
    else
        (void)tidesdb_txn_rollback(txn);

    tidesdb_txn_free(txn);
    return ok;
}

/**
 * tdb_keyring_fetch
 * read one key out of the keyring
 * @param name the keyring identifier
 * @param out out -- receives the key bytes, left empty on any failure
 * @return true when the key exists and was read whole
 */
static bool tdb_keyring_fetch(const std::string &name, std::vector<unsigned char> &out)
{
    out.clear();
    if (!tdb_keyring_reader) return false;

    my_h_keyring_reader_object reader = nullptr;
    if (tdb_keyring_reader->init(name.c_str(), TDB_KEYRING_AUTH_ID, &reader)) return false;

    /* a null reader on a successful init means the key simply does not exist, which is the normal
       answer on first use and not a failure to report. */
    if (reader == nullptr) return false;

    size_t data_len = 0;
    size_t type_len = 0;
    if (tdb_keyring_reader->fetch_length(reader, &data_len, &type_len) || data_len == 0 ||
        type_len >= TDB_KEYRING_TYPE_BUF_LEN)
    {
        (void)tdb_keyring_reader->deinit(reader);
        return false;
    }

    out.assign(data_len, 0);
    size_t fetched = 0;
    char data_type[TDB_KEYRING_TYPE_BUF_LEN] = {0};
    const bool failed = tdb_keyring_reader->fetch(reader, out.data(), out.size(), &fetched,
                                                  data_type, sizeof(data_type), &type_len);
    (void)tdb_keyring_reader->deinit(reader);

    if (failed || fetched == 0)
    {
        out.clear();
        return false;
    }
    out.resize(fetched);
    return true;
}

/**
 * tdb_master_key_get
 * fetch one master generation, optionally minting it when the keyring does not hold it
 * @param generation the master generation
 * @param mint whether to generate the key when it is absent
 * @param out out -- receives the master key bytes
 * @return true when the key is available
 */
static bool tdb_master_key_get(uint32_t generation, bool mint, std::vector<unsigned char> &out)
{
    const std::string name = tdb_master_key_name(generation);
    if (tdb_keyring_fetch(name, out)) return true;
    if (!mint || !tdb_keyring_generator) return false;

    /* generate inside the keyring rather than locally, so the material is minted by the backend
       that will hold it and only ever leaves it to wrap or unwrap a table key. */
    if (tdb_keyring_generator->generate(name.c_str(), TDB_KEYRING_AUTH_ID, TDB_KEYRING_DATA_TYPE,
                                        TDB_CRYPTO_KEY_LEN))
        return false;

    return tdb_keyring_fetch(name, out);
}

/**
 * tdb_master_generation_resolve
 * the master generation in force, read once from the store and seeded on a database that has never
 * held an encrypted table.  caller holds tdb_crypto_mtx
 * @return the generation, or GENERATION_NONE when the store is unavailable
 */
static uint32_t tdb_master_generation_resolve()
{
    if (tdb_master_generation != keyenc::GENERATION_NONE) return tdb_master_generation;

    uint8_t key[keyenc::KEY_MAX_LEN];
    const size_t key_len = keyenc::encode_master_seq_key(key);

    std::string stored;
    if (tdb_store_get(key, key_len, stored) && stored.size() == keyenc::FIELD_LEN)
    {
        tdb_master_generation =
            keyenc::be32_decode(reinterpret_cast<const uint8_t *>(stored.data()));
        return tdb_master_generation;
    }

    uint8_t value[keyenc::FIELD_LEN];
    keyenc::be32_encode(keyenc::GENERATION_FIRST, value);
    if (!tdb_store_put(key, key_len, value, sizeof(value))) return keyenc::GENERATION_NONE;

    tdb_master_generation = keyenc::GENERATION_FIRST;
    return tdb_master_generation;
}

/**
 * tdb_table_key_unwrap
 * recover a table key from its stored form
 * @param stored the stored value, a framed wrapped key
 * @param out out -- receives the plaintext table key
 * @return true on success
 */
static bool tdb_table_key_unwrap(const std::string &stored, std::vector<unsigned char> &out)
{
    out.clear();

    uint32_t generation = keyenc::GENERATION_NONE;
    size_t wrapped_off = 0;
    size_t wrapped_len = 0;
    if (!keyenc::parse_wrapped_value(reinterpret_cast<const uint8_t *>(stored.data()),
                                     stored.size(), &generation, &wrapped_off, &wrapped_len))
        return false;

    /* never mint while unwrapping.  a missing master generation means the keyring lost the key
       these rows were written under, and inventing a fresh one would produce a key that decrypts
       nothing while looking like success. */
    std::vector<unsigned char> master;
    if (!tdb_master_key_get(generation, false, master)) return false;

    out.assign(wrapped_len + MY_AES_BLOCK_SIZE, 0);
    const int len = my_aes_decrypt(
        reinterpret_cast<const unsigned char *>(stored.data()) + wrapped_off, (uint32)wrapped_len,
        out.data(), master.data(), (uint32)master.size(), TDB_WRAP_CIPHER, nullptr, false);
    if (len <= TDB_AES_FAILED)
    {
        out.clear();
        return false;
    }

    out.resize((size_t)len);
    return true;
}

/**
 * tdb_table_key_wrap
 * encrypt a table key under a master generation for storage
 * @param plain the plaintext table key
 * @param generation the master generation to wrap under, minted when absent
 * @param out out -- receives the framed wrapped key
 * @return true on success
 */
static bool tdb_table_key_wrap(const std::vector<unsigned char> &plain, uint32_t generation,
                               std::string &out)
{
    out.clear();

    std::vector<unsigned char> master;
    if (!tdb_master_key_get(generation, true, master)) return false;

    std::vector<unsigned char> wrapped(plain.size() + MY_AES_BLOCK_SIZE, 0);
    const int len =
        my_aes_encrypt(plain.data(), (uint32)plain.size(), wrapped.data(), master.data(),
                       (uint32)master.size(), TDB_WRAP_CIPHER, nullptr, false);
    if (len <= TDB_AES_FAILED) return false;

    std::vector<uint8_t> framed(keyenc::WRAP_HEADER_LEN + (size_t)len, 0);
    const size_t framed_len =
        keyenc::frame_wrapped_value(generation, wrapped.data(), (size_t)len, framed.data());
    if (framed_len == 0) return false;

    out.assign(reinterpret_cast<const char *>(framed.data()), framed_len);
    return true;
}

/**
 * tdb_table_key_mint
 * generate the first table key for an encryption id and record it.  caller holds tdb_crypto_mtx
 * @param key_id the table's encryption key id
 * @return the version minted, or TDB_CRYPTO_KEY_VERSION_INVALID on any failure
 */
static unsigned int tdb_table_key_mint(unsigned int key_id)
{
    const uint32_t generation = tdb_master_generation_resolve();
    if (generation == keyenc::GENERATION_NONE) return TDB_CRYPTO_KEY_VERSION_INVALID;

    std::vector<unsigned char> table_key(TDB_CRYPTO_KEY_LEN, 0);
    if (!tdb_crypto_random_bytes(table_key.data(), table_key.size()))
        return TDB_CRYPTO_KEY_VERSION_INVALID;

    std::string wrapped;
    if (!tdb_table_key_wrap(table_key, generation, wrapped)) return TDB_CRYPTO_KEY_VERSION_INVALID;

    uint8_t wrapped_key[keyenc::KEY_MAX_LEN];
    const size_t wrapped_key_len =
        keyenc::encode_wrapped_key(key_id, keyenc::GENERATION_FIRST, wrapped_key);
    if (!tdb_store_put(wrapped_key, wrapped_key_len,
                       reinterpret_cast<const uint8_t *>(wrapped.data()), wrapped.size()))
        return TDB_CRYPTO_KEY_VERSION_INVALID;

    /* the version pointer is written last, so a crash between the two writes strands an
       unreferenced wrapped key rather than leaving a version pointing at nothing. */
    uint8_t latest_key[keyenc::KEY_MAX_LEN];
    const size_t latest_key_len = keyenc::encode_latest_key(key_id, latest_key);
    uint8_t version[keyenc::FIELD_LEN];
    keyenc::be32_encode(keyenc::GENERATION_FIRST, version);
    if (!tdb_store_put(latest_key, latest_key_len, version, sizeof(version)))
        return TDB_CRYPTO_KEY_VERSION_INVALID;

    return keyenc::GENERATION_FIRST;
}

/* ******************** public interface ******************** */

bool tdb_crypto_init()
{
    std::lock_guard<std::mutex> lock(tdb_crypto_mtx);

    tdb_registry = mysql_plugin_registry_acquire();
    if (!tdb_registry) return true;

    my_h_service reader = nullptr;
    my_h_service writer = nullptr;
    my_h_service generator = nullptr;

    /* a server with no keyring component loaded is a normal configuration, not a failed init.  it
       only becomes an error when a table asks to be encrypted, which tdb_crypto_available lets
       create() detect and refuse up front. */
    if (tdb_registry->acquire("keyring_reader_with_status", &reader) ||
        tdb_registry->acquire_related("keyring_writer", reader, &writer) ||
        tdb_registry->acquire_related("keyring_generator", reader, &generator))
    {
        if (reader) tdb_registry->release(reader);
        if (writer) tdb_registry->release(writer);
        if (generator) tdb_registry->release(generator);
        sql_print_information(
            "[TIDESDB] no keyring component loaded; ENCRYPTED tables will be refused");
        return true;
    }

    tdb_keyring_reader = reinterpret_cast<SERVICE_TYPE(keyring_reader_with_status) *>(reader);
    tdb_keyring_writer = reinterpret_cast<SERVICE_TYPE(keyring_writer) *>(writer);
    tdb_keyring_generator = reinterpret_cast<SERVICE_TYPE(keyring_generator) *>(generator);

    tdb_crypto_cf = tidesdb_get_column_family(tdb_global, TDB_CRYPTO_CF_NAME);
    if (!tdb_crypto_cf)
    {
        tidesdb_column_family_config_t config = tidesdb_default_column_family_config();
        if (tidesdb_create_column_family(tdb_global, TDB_CRYPTO_CF_NAME, &config) == TDB_SUCCESS)
            tdb_crypto_cf = tidesdb_get_column_family(tdb_global, TDB_CRYPTO_CF_NAME);
    }
    if (!tdb_crypto_cf)
        sql_print_warning("[TIDESDB] could not open the reserved key column family '%s'",
                          TDB_CRYPTO_CF_NAME);

    return true;
}

void tdb_crypto_deinit()
{
    std::lock_guard<std::mutex> lock(tdb_crypto_mtx);
    if (!tdb_registry) return;

    using reader_t = SERVICE_TYPE_NO_CONST(keyring_reader_with_status);
    using writer_t = SERVICE_TYPE_NO_CONST(keyring_writer);
    using generator_t = SERVICE_TYPE_NO_CONST(keyring_generator);

    if (tdb_keyring_reader)
        tdb_registry->release(
            reinterpret_cast<my_h_service>(const_cast<reader_t *>(tdb_keyring_reader)));
    if (tdb_keyring_writer)
        tdb_registry->release(
            reinterpret_cast<my_h_service>(const_cast<writer_t *>(tdb_keyring_writer)));
    if (tdb_keyring_generator)
        tdb_registry->release(
            reinterpret_cast<my_h_service>(const_cast<generator_t *>(tdb_keyring_generator)));

    tdb_keyring_reader = nullptr;
    tdb_keyring_writer = nullptr;
    tdb_keyring_generator = nullptr;
    tdb_crypto_cf = nullptr;
    tdb_master_generation = keyenc::GENERATION_NONE;

    mysql_plugin_registry_release(tdb_registry);
    tdb_registry = nullptr;
}

bool tdb_crypto_available()
{
    return tdb_keyring_reader != nullptr && tdb_keyring_generator != nullptr &&
           tdb_crypto_cf != nullptr;
}

unsigned int tdb_crypto_latest_key_version(unsigned int key_id)
{
    if (!tdb_crypto_available()) return TDB_CRYPTO_KEY_VERSION_INVALID;

    std::lock_guard<std::mutex> lock(tdb_crypto_mtx);

    uint8_t latest_key[keyenc::KEY_MAX_LEN];
    const size_t latest_key_len = keyenc::encode_latest_key(key_id, latest_key);

    std::string stored;
    if (tdb_store_get(latest_key, latest_key_len, stored) && stored.size() == keyenc::FIELD_LEN)
        return keyenc::be32_decode(reinterpret_cast<const uint8_t *>(stored.data()));

    return tdb_table_key_mint(key_id);
}

unsigned int tdb_crypto_get_key(unsigned int key_id, unsigned int key_version, unsigned char *key,
                                unsigned int *klen)
{
    if (!key || !klen || !tdb_crypto_available()) return TDB_CRYPTO_STATUS_FAILED;

    std::lock_guard<std::mutex> lock(tdb_crypto_mtx);

    uint8_t store_key[keyenc::KEY_MAX_LEN];
    const size_t store_key_len = keyenc::encode_wrapped_key(key_id, key_version, store_key);

    std::string stored;
    if (!tdb_store_get(store_key, store_key_len, stored)) return TDB_CRYPTO_STATUS_FAILED;

    std::vector<unsigned char> plain;
    if (!tdb_table_key_unwrap(stored, plain)) return TDB_CRYPTO_STATUS_FAILED;

    /* fail rather than truncate.  a short buffer would hand back a partial key that encrypts
       without complaint and produces rows nothing can read back. */
    if (plain.empty() || plain.size() > *klen) return TDB_CRYPTO_STATUS_FAILED;

    memcpy(key, plain.data(), plain.size());
    *klen = (unsigned int)plain.size();
    return TDB_CRYPTO_STATUS_OK;
}

unsigned int tdb_crypto_encrypted_length(unsigned int slen, unsigned int, unsigned int)
{
    return (unsigned int)my_aes_get_size(slen, TDB_ROW_CIPHER);
}

int tdb_crypto_crypt(const unsigned char *src, unsigned int slen, unsigned char *dst,
                     unsigned int *dlen, const unsigned char *key, unsigned int klen,
                     const unsigned char *iv, unsigned int ivlen, int flags, unsigned int,
                     unsigned int)
{
    if (!src || !dst || !dlen || !key || !iv || ivlen < MY_AES_IV_SIZE)
        return TDB_CRYPTO_STATUS_FAILED;

    const int len = flags == TDB_CRYPTO_FLAG_ENCRYPT
                        ? my_aes_encrypt(src, slen, dst, key, klen, TDB_ROW_CIPHER, iv, true)
                        : my_aes_decrypt(src, slen, dst, key, klen, TDB_ROW_CIPHER, iv, true);
    if (len < TDB_AES_FAILED) return TDB_CRYPTO_STATUS_FAILED;

    *dlen = (unsigned int)len;
    return TDB_CRYPTO_STATUS_OK;
}

bool tdb_crypto_random_bytes(unsigned char *buf, size_t len)
{
    if (!buf || len == 0) return false;
    return my_rand_buffer(buf, len) == 0;
}

int tdb_crypto_rotate_table_key(unsigned int key_id)
{
    if (!tdb_crypto_available()) return TDB_CRYPTO_STATUS_FAILED;
    if (key_id < 1 || key_id > TIDESDB_MAX_ENCRYPTION_KEY_ID) return TDB_CRYPTO_STATUS_FAILED;

    std::lock_guard<std::mutex> lock(tdb_crypto_mtx);

    const uint32_t generation = tdb_master_generation_resolve();
    if (generation == keyenc::GENERATION_NONE) return TDB_CRYPTO_STATUS_FAILED;

    uint8_t latest_key[keyenc::KEY_MAX_LEN];
    const size_t latest_key_len = keyenc::encode_latest_key(key_id, latest_key);

    /* A key nobody has used yet has no version to advance from; minting its first one is the same
       thing a table encrypted with it would have done on its own. */
    std::string stored_latest;
    if (!tdb_store_get(latest_key, latest_key_len, stored_latest) ||
        stored_latest.size() != keyenc::FIELD_LEN)
        return tdb_table_key_mint(key_id) == TDB_CRYPTO_KEY_VERSION_INVALID
                   ? TDB_CRYPTO_STATUS_FAILED
                   : TDB_CRYPTO_STATUS_OK;

    const uint32_t current =
        keyenc::be32_decode(reinterpret_cast<const uint8_t *>(stored_latest.data()));
    if (current >= TIDESDB_MAX_ENCRYPTION_KEY_VERSIONS)
    {
        sql_print_error(
            "[TIDESDB] encryption key %u is at version %u, the most this build records; it was not "
            "rotated",
            key_id, current);
        return TDB_CRYPTO_STATUS_FAILED;
    }
    const uint32_t next = current + 1;

    std::vector<unsigned char> table_key(TDB_CRYPTO_KEY_LEN, 0);
    if (!tdb_crypto_random_bytes(table_key.data(), table_key.size()))
        return TDB_CRYPTO_STATUS_FAILED;

    std::string wrapped;
    if (!tdb_table_key_wrap(table_key, generation, wrapped)) return TDB_CRYPTO_STATUS_FAILED;

    uint8_t wrapped_key[keyenc::KEY_MAX_LEN];
    const size_t wrapped_key_len = keyenc::encode_wrapped_key(key_id, next, wrapped_key);
    if (!tdb_store_put(wrapped_key, wrapped_key_len,
                       reinterpret_cast<const uint8_t *>(wrapped.data()), wrapped.size()))
        return TDB_CRYPTO_STATUS_FAILED;

    /* the version pointer moves last, so a crash between the two writes strands an unreferenced
       key rather than pointing rows at one that was never stored. */
    uint8_t version[keyenc::FIELD_LEN];
    keyenc::be32_encode(next, version);
    if (!tdb_store_put(latest_key, latest_key_len, version, sizeof(version)))
        return TDB_CRYPTO_STATUS_FAILED;

    sql_print_information("[TIDESDB] encryption key %u rotated to version %u", key_id, next);
    return TDB_CRYPTO_STATUS_OK;
}

int tdb_crypto_rotate_master_key()
{
    if (!tdb_crypto_available()) return TDB_CRYPTO_STATUS_FAILED;

    std::lock_guard<std::mutex> lock(tdb_crypto_mtx);

    const uint32_t current = tdb_master_generation_resolve();
    if (current == keyenc::GENERATION_NONE) return TDB_CRYPTO_STATUS_FAILED;
    const uint32_t next = current + 1;

    /* mint the new generation first, so a keyring that refuses leaves the database exactly as it
       was rather than half-rotated. */
    std::vector<unsigned char> new_master;
    if (!tdb_master_key_get(next, true, new_master)) return TDB_CRYPTO_STATUS_FAILED;

    /* re-wrap every stored table key under the new generation.  no row is read or written here.
       both loops carry a statically known bound; the inner one is additionally stopped by the
       recorded latest version, whichever comes first. */
    for (unsigned int key_id = 1; key_id <= TIDESDB_MAX_ENCRYPTION_KEY_ID; key_id++)
    {
        uint8_t latest_key[keyenc::KEY_MAX_LEN];
        const size_t latest_key_len = keyenc::encode_latest_key(key_id, latest_key);

        std::string stored_latest;
        if (!tdb_store_get(latest_key, latest_key_len, stored_latest) ||
            stored_latest.size() != keyenc::FIELD_LEN)
            continue;

        const uint32_t latest =
            keyenc::be32_decode(reinterpret_cast<const uint8_t *>(stored_latest.data()));

        for (uint32_t version = keyenc::GENERATION_FIRST;
             version <= latest && version <= TIDESDB_MAX_ENCRYPTION_KEY_VERSIONS; version++)
        {
            uint8_t store_key[keyenc::KEY_MAX_LEN];
            const size_t store_key_len = keyenc::encode_wrapped_key(key_id, version, store_key);

            std::string stored;
            if (!tdb_store_get(store_key, store_key_len, stored)) continue;

            std::vector<unsigned char> plain;
            if (!tdb_table_key_unwrap(stored, plain)) return TDB_CRYPTO_STATUS_FAILED;

            std::string rewrapped;
            if (!tdb_table_key_wrap(plain, next, rewrapped)) return TDB_CRYPTO_STATUS_FAILED;

            if (!tdb_store_put(store_key, store_key_len,
                               reinterpret_cast<const uint8_t *>(rewrapped.data()),
                               rewrapped.size()))
                return TDB_CRYPTO_STATUS_FAILED;
        }
    }

    /* publish the new generation last.  a crash before this point leaves every table key readable
       under the old master key, which is still in the keyring, so the rotation is simply retried.
     */
    uint8_t master_key[keyenc::KEY_MAX_LEN];
    const size_t master_key_len = keyenc::encode_master_seq_key(master_key);
    uint8_t value[keyenc::FIELD_LEN];
    keyenc::be32_encode(next, value);
    if (!tdb_store_put(master_key, master_key_len, value, sizeof(value)))
        return TDB_CRYPTO_STATUS_FAILED;

    tdb_master_generation = next;
    sql_print_information("[TIDESDB] master key rotated to generation %u", next);
    return TDB_CRYPTO_STATUS_OK;
}
