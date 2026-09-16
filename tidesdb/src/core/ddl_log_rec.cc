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

#include "ddl_log_rec.h"

#include <cstring>

namespace tidesdb
{
namespace ddl_log
{

/* a length-prefixed string carries a 32-bit length, which bounds every parse loop below. */
static constexpr size_t LEN_PREFIX = 4;

/* no server path, database or table name approaches this; a stored length beyond it means the
   record is not one of ours and the parse stops rather than allocating on a bad number. */
static constexpr uint32_t MAX_FIELD_LEN = 65535;

static void put_u64(uint64_t v, uint8_t *out)
{
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)((v >> ((7 - i) * 8)) & 0xFFu);
}

static uint64_t get_u64(const uint8_t *in)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | (uint64_t)in[i];
    return v;
}

static void put_str(std::string &out, const std::string &s)
{
    const uint32_t n = (uint32_t)s.size();
    char len[LEN_PREFIX];
    for (int i = 0; i < 4; i++) len[i] = (char)((n >> ((3 - i) * 8)) & 0xFFu);
    out.append(len, LEN_PREFIX);
    out.append(s);
}

/**
 * get_str
 * read one length-prefixed string, advancing the cursor
 * @param p in/out -- the cursor
 * @param end one past the last readable byte
 * @param out out -- the string
 * @return true when a whole string was available and its length was plausible
 */
static bool get_str(const uint8_t *&p, const uint8_t *end, std::string &out)
{
    if ((size_t)(end - p) < LEN_PREFIX) return false;
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) n = (n << 8) | (uint32_t)p[i];
    p += LEN_PREFIX;

    if (n > MAX_FIELD_LEN) return false;
    if ((size_t)(end - p) < n) return false;

    out.assign(reinterpret_cast<const char *>(p), n);
    p += n;
    return true;
}

size_t encode_key(uint64_t boot_id, uint64_t sequence, uint8_t *out)
{
    if (!out) return 0;
    put_u64(boot_id, out);
    put_u64(sequence, out + 8);
    return KEY_LEN;
}

bool decode_key_boot_id(const uint8_t *key, size_t key_len, uint64_t *out_boot_id)
{
    if (!key || !out_boot_id || key_len != KEY_LEN) return false;
    *out_boot_id = get_u64(key);
    return true;
}

bool encode_record(const record &rec, std::string &out)
{
    out.clear();
    if (rec.path.size() > MAX_FIELD_LEN || rec.db.size() > MAX_FIELD_LEN ||
        rec.table.size() > MAX_FIELD_LEN)
        return false;

    out.push_back((char)FORMAT_VERSION);
    out.push_back((char)rec.what);
    put_str(out, rec.path);
    put_str(out, rec.db);
    put_str(out, rec.table);
    return true;
}

bool decode_record(const uint8_t *data, size_t len, record *out)
{
    if (!data || !out || len < 2) return false;
    if (data[0] != FORMAT_VERSION) return false;

    const uint8_t what = data[1];
    if (what != (uint8_t)intent::drop && what != (uint8_t)intent::create) return false;

    const uint8_t *p = data + 2;
    const uint8_t *const end = data + len;

    record parsed;
    parsed.what = (intent)what;
    if (!get_str(p, end, parsed.path)) return false;
    if (!get_str(p, end, parsed.db)) return false;
    if (!get_str(p, end, parsed.table)) return false;

    /* A record with no path names nothing to remove, and acting on it could only remove the wrong
       thing, so it is rejected rather than passed on. */
    if (parsed.path.empty()) return false;

    *out = std::move(parsed);
    return true;
}

}  // namespace ddl_log
}  // namespace tidesdb
