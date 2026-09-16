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

/* server-free core for the record a pending schema change is written as.
 *
 * the engine cannot finish a create or a drop until it knows whether the statement that asked for
 * it committed, and it must not forget what it was asked to do if the server dies in between. the
 * record below is what makes the second part true: it is written to a reserved column family before
 * anything is touched, and removed once the change has been carried out or abandoned.
 *
 * the layout lives here, away from any server or library call, so it is unit-tested on its own and
 * so a record written by one release still parses in the next. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tidesdb
{
namespace ddl_log
{

/* what the statement asked the engine to do. */
enum class intent : uint8_t
{
    drop = 1,   /* the families go once the drop is known to have committed  */
    create = 2, /* the families go if the create turns out not to have       */
};

/* the record's leading byte, so a future layout change is detectable rather than misparsed. */
static constexpr uint8_t FORMAT_VERSION = 1;

/* a key is [boot id (8 BE)][sequence (8 BE)].  the boot id distinguishes a record this server run
 * wrote -- which belongs to a statement that may still be in flight -- from one left behind by an
 * earlier run, which is by definition abandoned and safe to resolve. */
static constexpr size_t KEY_LEN = 16;

/**
 * record
 * one pending schema change
 * @param what whether the families are to be dropped or were just created
 * @param path the server table path, verbatim, so the families are named exactly as they were
 * @param db the database, for asking the dictionary what became of the statement
 * @param table the table name, same
 */
struct record
{
    intent what;
    std::string path;
    std::string db;
    std::string table;
};

/**
 * encode_key
 * the reserved-family key one pending record is held under
 * @param boot_id identifies the server run that wrote it
 * @param sequence distinguishes records within that run
 * @param out out -- receives KEY_LEN bytes
 * @return the encoded length, or 0 when out is null
 */
size_t encode_key(uint64_t boot_id, uint64_t sequence, uint8_t *out);

/**
 * decode_key_boot_id
 * the server run a stored key belongs to
 * @param key KEY_LEN bytes
 * @param key_len the key length, checked
 * @param out_boot_id out -- the boot id
 * @return true when the key is well formed
 */
bool decode_key_boot_id(const uint8_t *key, size_t key_len, uint64_t *out_boot_id);

/**
 * encode_record
 * serialise a pending record
 * @param rec the record
 * @param out out -- receives the encoded bytes, cleared first
 * @return true on success
 */
bool encode_record(const record &rec, std::string &out);

/**
 * decode_record
 * parse a record written by encode_record
 * @param data the stored bytes
 * @param len the stored length
 * @param out out -- the parsed record, untouched on failure
 * @return true when the bytes are a well-formed record of a version this build understands; a
 *         record that fails to parse must be left alone rather than acted on, because acting on a
 *         misread path would remove the wrong table's storage
 */
bool decode_record(const uint8_t *data, size_t len, record *out);

}  // namespace ddl_log
}  // namespace tidesdb
