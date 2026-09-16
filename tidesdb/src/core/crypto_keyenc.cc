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

#include "crypto_keyenc.h"

#include <cstring>

namespace tidesdb
{
namespace crypto_keyenc
{

/* bit positions of each byte in a big-endian 32-bit field, innermost first. */
static constexpr int BE32_SHIFT_0 = 24;
static constexpr int BE32_SHIFT_1 = 16;
static constexpr int BE32_SHIFT_2 = 8;
static constexpr uint32_t BE32_BYTE_MASK = 0xFFu;

void be32_encode(uint32_t v, uint8_t *out)
{
    if (!out) return;
    out[0] = (uint8_t)((v >> BE32_SHIFT_0) & BE32_BYTE_MASK);
    out[1] = (uint8_t)((v >> BE32_SHIFT_1) & BE32_BYTE_MASK);
    out[2] = (uint8_t)((v >> BE32_SHIFT_2) & BE32_BYTE_MASK);
    out[3] = (uint8_t)(v & BE32_BYTE_MASK);
}

uint32_t be32_decode(const uint8_t *in)
{
    if (!in) return 0;
    return ((uint32_t)in[0] << BE32_SHIFT_0) | ((uint32_t)in[1] << BE32_SHIFT_1) |
           ((uint32_t)in[2] << BE32_SHIFT_2) | (uint32_t)in[3];
}

/**
 * encode_key
 * the one key builder the three public encoders route through -- a namespace byte followed by as
 * many big-endian fields as that namespace carries
 * @param n the namespace
 * @param fields the field values, in order
 * @param field_count how many fields the namespace carries, 0 to 2
 * @param out out -- receives at most KEY_MAX_LEN bytes
 * @return the encoded length, or 0 when out is null
 */
static size_t encode_key(ns n, const uint32_t *fields, size_t field_count, uint8_t *out)
{
    if (!out) return 0;

    out[0] = (uint8_t)n;
    size_t len = 1;

    /* bounded by the namespace's own field count, which is a compile-time property of the layout
       rather than anything read back from the store. */
    for (size_t i = 0; i < field_count && i < 2; i++)
    {
        be32_encode(fields[i], out + len);
        len += FIELD_LEN;
    }
    return len;
}

size_t encode_master_seq_key(uint8_t *out)
{
    return encode_key(ns::master_seq, nullptr, 0, out);
}

size_t encode_latest_key(uint32_t key_id, uint8_t *out)
{
    const uint32_t fields[] = {key_id};
    return encode_key(ns::latest, fields, 1, out);
}

size_t encode_wrapped_key(uint32_t key_id, uint32_t version, uint8_t *out)
{
    const uint32_t fields[] = {key_id, version};
    return encode_key(ns::wrapped, fields, 2, out);
}

size_t frame_wrapped_value(uint32_t generation, const uint8_t *wrapped, size_t wrapped_len,
                           uint8_t *out)
{
    if (!out || !wrapped || wrapped_len == 0) return 0;

    be32_encode(generation, out);
    memcpy(out + WRAP_HEADER_LEN, wrapped, wrapped_len);
    return WRAP_HEADER_LEN + wrapped_len;
}

bool parse_wrapped_value(const uint8_t *stored, size_t stored_len, uint32_t *generation,
                         size_t *wrapped_off, size_t *wrapped_len)
{
    if (!stored || !generation || !wrapped_off || !wrapped_len) return false;

    /* a value carrying only a header holds no key.  rejecting it here rather than returning a
       zero-length key keeps the caller from unwrapping an empty buffer and treating whatever the
       cipher returns as key material. */
    if (stored_len <= WRAP_HEADER_LEN) return false;

    *generation = be32_decode(stored);
    *wrapped_off = WRAP_HEADER_LEN;
    *wrapped_len = stored_len - WRAP_HEADER_LEN;
    return true;
}

}  // namespace crypto_keyenc
}  // namespace tidesdb
