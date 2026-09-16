/* Copyright (c) 2026 TidesDB Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 */

/* unit tests for the server-free key-store encoding. the layouts under test address a database's
 * key material, so a silent change to one of them makes existing encrypted rows unreadable; these
 * tests pin the byte-level format, not just round-trip behaviour. */
#include <cstdint>
#include <cstring>

#include "../src/core/crypto_keyenc.h"
#include "test_utils.h"

using namespace tidesdb::crypto_keyenc;

static int tests_passed = 0;
static int tests_failed = 0;

void test_be32_roundtrip(void)
{
    uint8_t b[FIELD_LEN];

    be32_encode(0, b);
    ASSERT_EQ(be32_decode(b), (uint32_t)0);

    be32_encode(1, b);
    ASSERT_EQ(be32_decode(b), (uint32_t)1);

    be32_encode(0xFFFFFFFFu, b);
    ASSERT_EQ(be32_decode(b), (uint32_t)0xFFFFFFFFu);

    be32_encode(0x01020304u, b);
    ASSERT_EQ(be32_decode(b), (uint32_t)0x01020304u);
}

void test_be32_is_big_endian(void)
{
    /* byte order is the property that makes a prefix scan walk versions in numeric order, so it is
       pinned explicitly rather than left to the round trip to imply. */
    uint8_t b[FIELD_LEN];
    be32_encode(0x01020304u, b);
    ASSERT_EQ(b[0], (uint8_t)0x01);
    ASSERT_EQ(b[1], (uint8_t)0x02);
    ASSERT_EQ(b[2], (uint8_t)0x03);
    ASSERT_EQ(b[3], (uint8_t)0x04);
}

void test_null_arguments_are_safe(void)
{
    /* every encoder is reachable from a path that has already failed to allocate, so a null out
       buffer must return zero rather than write through it. */
    ASSERT_EQ(encode_master_seq_key(nullptr), (size_t)0);
    ASSERT_EQ(encode_latest_key(1, nullptr), (size_t)0);
    ASSERT_EQ(encode_wrapped_key(1, 1, nullptr), (size_t)0);
    ASSERT_EQ(be32_decode(nullptr), (uint32_t)0);

    uint8_t out[KEY_MAX_LEN];
    ASSERT_EQ(frame_wrapped_value(1, nullptr, 0, out), (size_t)0);
}

void test_key_lengths_and_namespaces(void)
{
    uint8_t k[KEY_MAX_LEN];

    ASSERT_EQ(encode_master_seq_key(k), (size_t)1);
    ASSERT_EQ(k[0], (uint8_t)ns::master_seq);

    ASSERT_EQ(encode_latest_key(7, k), (size_t)(1 + FIELD_LEN));
    ASSERT_EQ(k[0], (uint8_t)ns::latest);
    ASSERT_EQ(be32_decode(k + 1), (uint32_t)7);

    ASSERT_EQ(encode_wrapped_key(7, 9, k), (size_t)(1 + (2 * FIELD_LEN)));
    ASSERT_EQ(k[0], (uint8_t)ns::wrapped);
    ASSERT_EQ(be32_decode(k + 1), (uint32_t)7);
    ASSERT_EQ(be32_decode(k + 1 + FIELD_LEN), (uint32_t)9);
    ASSERT_EQ(encode_wrapped_key(7, 9, k), KEY_MAX_LEN);
}

void test_namespaces_are_distinct(void)
{
    /* the three namespaces share a key space; if two ever encoded to the same leading byte, a
       version lookup could read a master generation. */
    uint8_t a[KEY_MAX_LEN], b[KEY_MAX_LEN], c[KEY_MAX_LEN];
    encode_master_seq_key(a);
    encode_latest_key(1, b);
    encode_wrapped_key(1, 1, c);
    ASSERT_TRUE(a[0] != b[0]);
    ASSERT_TRUE(b[0] != c[0]);
    ASSERT_TRUE(a[0] != c[0]);
}

void test_wrapped_value_roundtrip(void)
{
    const uint8_t key[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
    uint8_t framed[WRAP_HEADER_LEN + sizeof(key)];

    const size_t n = frame_wrapped_value(42, key, sizeof(key), framed);
    ASSERT_EQ(n, (size_t)(WRAP_HEADER_LEN + sizeof(key)));

    uint32_t generation = 0;
    size_t off = 0, len = 0;
    ASSERT_TRUE(parse_wrapped_value(framed, n, &generation, &off, &len));
    ASSERT_EQ(generation, (uint32_t)42);
    ASSERT_EQ(off, WRAP_HEADER_LEN);
    ASSERT_EQ(len, sizeof(key));
    ASSERT_EQ(memcmp(framed + off, key, len), 0);
}

void test_truncated_wrapped_value_is_rejected(void)
{
    /* a value holding only a header carries no key. accepting it would hand the unwrap an empty
       buffer and let whatever the cipher returned be used as key material. */
    uint8_t framed[WRAP_HEADER_LEN];
    be32_encode(3, framed);

    uint32_t generation = 99;
    size_t off = 99, len = 99;
    ASSERT_FALSE(parse_wrapped_value(framed, sizeof(framed), &generation, &off, &len));

    /* the out parameters must be left as they were, so a caller that ignores the return value
       cannot read a plausible-looking generation out of a rejected value. */
    ASSERT_EQ(generation, (uint32_t)99);
    ASSERT_EQ(off, (size_t)99);
    ASSERT_EQ(len, (size_t)99);

    ASSERT_FALSE(parse_wrapped_value(framed, 0, &generation, &off, &len));
    ASSERT_FALSE(parse_wrapped_value(nullptr, sizeof(framed), &generation, &off, &len));
    ASSERT_FALSE(parse_wrapped_value(framed, sizeof(framed), nullptr, &off, &len));
}

void test_generation_sentinels(void)
{
    /* zero is the not-yet-resolved sentinel the key store never holds, so it must stay distinct
       from the first real generation. */
    ASSERT_TRUE(GENERATION_NONE != GENERATION_FIRST);
    ASSERT_EQ(GENERATION_NONE, (uint32_t)0);
    ASSERT_EQ(GENERATION_FIRST, (uint32_t)1);
}

int main(int argc, char **argv)
{
    INIT_TEST_FILTER(argc, argv);

    RUN_TEST(test_be32_roundtrip, tests_passed);
    RUN_TEST(test_be32_is_big_endian, tests_passed);
    RUN_TEST(test_null_arguments_are_safe, tests_passed);
    RUN_TEST(test_key_lengths_and_namespaces, tests_passed);
    RUN_TEST(test_namespaces_are_distinct, tests_passed);
    RUN_TEST(test_wrapped_value_roundtrip, tests_passed);
    RUN_TEST(test_truncated_wrapped_value_is_rejected, tests_passed);
    RUN_TEST(test_generation_sentinels, tests_passed);

    PRINT_TEST_RESULTS(tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
