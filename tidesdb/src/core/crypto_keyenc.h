/* Copyright (c) 2026 TidesDB Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* server-free core for the key store the engine keeps its wrapped table keys in. the byte layouts
 * live here, away from any server or library call, so the encoding that a database's key material
 * is addressed by is unit-tested on its own and cannot drift between the two key-custody backends
 * that share it. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace tidesdb
{
namespace crypto_keyenc
{

/* the namespaces inside the key store. the leading byte separates them the way a data family's
 * leading byte separates meta keys from row keys, so one prefix scan reaches one namespace. */
enum class ns : uint8_t
{
    master_seq = 0x00, /* no fields    -> the master key generation in force  */
    wrapped = 0x01,    /* key id, ver  -> one wrapped table key              */
    latest = 0x02,     /* key id       -> that id's highest minted version   */
};

/* the widest encoded key: one namespace byte plus two 32-bit fields. */
static constexpr size_t FIELD_LEN = 4;
static constexpr size_t KEY_MAX_LEN = 1 + (2 * FIELD_LEN);

/* a stored wrapped key is framed with the master generation that wrapped it, so an unwrap can find
 * its master key without consulting the generation currently in force -- which is what lets a
 * rotation be interrupted and resumed without stranding keys. */
static constexpr size_t WRAP_HEADER_LEN = FIELD_LEN;

/* a master generation and a key version are both one-based; zero is the not-yet-resolved sentinel
 * and never appears in the store. */
static constexpr uint32_t GENERATION_NONE = 0;
static constexpr uint32_t GENERATION_FIRST = 1;

/**
 * be32_encode
 * write a 32-bit value big-endian, so encoded keys sort in numeric order and a prefix scan over one
 * key id walks its versions in sequence
 * @param v the value
 * @param out out -- receives FIELD_LEN bytes; the caller owns the buffer
 */
void be32_encode(uint32_t v, uint8_t *out);

/**
 * be32_decode
 * read back a value written by be32_encode
 * @param in FIELD_LEN bytes
 * @return the value
 */
uint32_t be32_decode(const uint8_t *in);

/**
 * encode_master_seq_key
 * the store key the master generation in force is held under
 * @param out out -- receives at most KEY_MAX_LEN bytes
 * @return the encoded length
 */
size_t encode_master_seq_key(uint8_t *out);

/**
 * encode_latest_key
 * the store key one encryption id's highest minted version is held under
 * @param key_id the table's encryption key id
 * @param out out -- receives at most KEY_MAX_LEN bytes
 * @return the encoded length
 */
size_t encode_latest_key(uint32_t key_id, uint8_t *out);

/**
 * encode_wrapped_key
 * the store key one wrapped table key is held under
 * @param key_id the table's encryption key id
 * @param version the table key version, as stamped into every row encrypted under it
 * @param out out -- receives at most KEY_MAX_LEN bytes
 * @return the encoded length
 */
size_t encode_wrapped_key(uint32_t key_id, uint32_t version, uint8_t *out);

/**
 * frame_wrapped_value
 * prefix a wrapped table key with the master generation that wrapped it
 * @param generation the master generation
 * @param wrapped the wrapped key bytes
 * @param wrapped_len the wrapped key length
 * @param out out -- receives WRAP_HEADER_LEN + wrapped_len bytes; the caller owns the buffer
 * @return the framed length
 */
size_t frame_wrapped_value(uint32_t generation, const uint8_t *wrapped, size_t wrapped_len,
                           uint8_t *out);

/**
 * parse_wrapped_value
 * split a stored value back into its master generation and wrapped key
 * @param stored the stored bytes
 * @param stored_len the stored length
 * @param generation out -- the master generation that wrapped the key
 * @param wrapped_off out -- the offset of the wrapped key within stored
 * @param wrapped_len out -- the wrapped key length
 * @return true when the value is long enough to carry a header and at least one wrapped byte;
 *         false leaves every out parameter untouched, so a truncated value cannot be mistaken for
 *         a generation-zero key
 */
bool parse_wrapped_value(const uint8_t *stored, size_t stored_len, uint32_t *generation,
                         size_t *wrapped_off, size_t *wrapped_len);

}  // namespace crypto_keyenc
}  // namespace tidesdb
