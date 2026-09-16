/* Copyright (c) 2026 TidesDB Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 */

/* unit tests for the pending schema-change record.  a record that is misread names the wrong table,
 * and acting on it removes storage that should have been kept, so these pin the parse's refusals as
 * carefully as its successes. */
#include <cstdint>
#include <cstring>
#include <string>

#include "../src/core/ddl_log_rec.h"
#include "test_utils.h"

using namespace tidesdb::ddl_log;

static int tests_passed = 0;
static int tests_failed = 0;

static record make(intent what, const char *path, const char *db, const char *table)
{
    record r;
    r.what = what;
    r.path = path;
    r.db = db;
    r.table = table;
    return r;
}

void test_key_roundtrip(void)
{
    uint8_t k[KEY_LEN];
    ASSERT_EQ(encode_key(0x0102030405060708ULL, 42, k), KEY_LEN);

    uint64_t boot = 0;
    ASSERT_TRUE(decode_key_boot_id(k, KEY_LEN, &boot));
    ASSERT_EQ(boot, (uint64_t)0x0102030405060708ULL);
}

void test_key_is_big_endian_so_it_sorts(void)
{
    /* the boot id leads the key so one prefix scan reaches every record a given server run wrote;
       that only holds if the encoding sorts numerically. */
    uint8_t a[KEY_LEN], b[KEY_LEN];
    encode_key(1, 0, a);
    encode_key(2, 0, b);
    ASSERT_TRUE(memcmp(a, b, KEY_LEN) < 0);

    encode_key(1, 1, a);
    encode_key(1, 2, b);
    ASSERT_TRUE(memcmp(a, b, KEY_LEN) < 0);
}

void test_key_rejects_wrong_length(void)
{
    uint8_t k[KEY_LEN];
    encode_key(7, 7, k);
    uint64_t boot = 99;
    ASSERT_FALSE(decode_key_boot_id(k, KEY_LEN - 1, &boot));
    ASSERT_FALSE(decode_key_boot_id(nullptr, KEY_LEN, &boot));
    ASSERT_EQ(boot, (uint64_t)99); /* left untouched */
}

void test_record_roundtrip(void)
{
    const record in = make(intent::drop, "./mydb/mytable", "mydb", "mytable");

    std::string bytes;
    ASSERT_TRUE(encode_record(in, bytes));
    ASSERT_TRUE(bytes.size() > 2);

    record out;
    ASSERT_TRUE(decode_record((const uint8_t *)bytes.data(), bytes.size(), &out));
    ASSERT_TRUE(out.what == intent::drop);
    ASSERT_TRUE(out.path == "./mydb/mytable");
    ASSERT_TRUE(out.db == "mydb");
    ASSERT_TRUE(out.table == "mytable");
}

void test_both_intents_survive(void)
{
    for (intent what : {intent::drop, intent::create})
    {
        std::string bytes;
        ASSERT_TRUE(encode_record(make(what, "./d/t", "d", "t"), bytes));
        record out;
        ASSERT_TRUE(decode_record((const uint8_t *)bytes.data(), bytes.size(), &out));
        ASSERT_TRUE(out.what == what);
    }
}

void test_truncated_record_is_rejected(void)
{
    std::string bytes;
    ASSERT_TRUE(encode_record(make(intent::drop, "./d/t", "d", "t"), bytes));

    /* every prefix short of the whole thing must be refused, because a partial parse would yield a
       plausible-looking path naming a different table. */
    for (size_t n = 0; n < bytes.size(); n++)
    {
        record out;
        ASSERT_FALSE(decode_record((const uint8_t *)bytes.data(), n, &out));
    }
}

void test_unknown_version_is_rejected(void)
{
    std::string bytes;
    ASSERT_TRUE(encode_record(make(intent::drop, "./d/t", "d", "t"), bytes));
    bytes[0] = (char)(FORMAT_VERSION + 1);

    record out;
    ASSERT_FALSE(decode_record((const uint8_t *)bytes.data(), bytes.size(), &out));
}

void test_unknown_intent_is_rejected(void)
{
    std::string bytes;
    ASSERT_TRUE(encode_record(make(intent::create, "./d/t", "d", "t"), bytes));
    bytes[1] = (char)99;

    record out;
    ASSERT_FALSE(decode_record((const uint8_t *)bytes.data(), bytes.size(), &out));
}

void test_empty_path_is_rejected(void)
{
    /* a record naming no table cannot be acted on safely. */
    std::string bytes;
    ASSERT_TRUE(encode_record(make(intent::drop, "", "d", "t"), bytes));

    record out;
    ASSERT_FALSE(decode_record((const uint8_t *)bytes.data(), bytes.size(), &out));
}

void test_null_arguments_are_safe(void)
{
    record out;
    ASSERT_FALSE(decode_record(nullptr, 10, &out));

    std::string bytes;
    ASSERT_TRUE(encode_record(make(intent::drop, "./d/t", "d", "t"), bytes));
    ASSERT_FALSE(decode_record((const uint8_t *)bytes.data(), bytes.size(), nullptr));

    ASSERT_EQ(encode_key(1, 1, nullptr), (size_t)0);
}

int main(int argc, char **argv)
{
    INIT_TEST_FILTER(argc, argv);

    RUN_TEST(test_key_roundtrip, tests_passed);
    RUN_TEST(test_key_is_big_endian_so_it_sorts, tests_passed);
    RUN_TEST(test_key_rejects_wrong_length, tests_passed);
    RUN_TEST(test_record_roundtrip, tests_passed);
    RUN_TEST(test_both_intents_survive, tests_passed);
    RUN_TEST(test_truncated_record_is_rejected, tests_passed);
    RUN_TEST(test_unknown_version_is_rejected, tests_passed);
    RUN_TEST(test_unknown_intent_is_rejected, tests_passed);
    RUN_TEST(test_empty_path_is_rejected, tests_passed);
    RUN_TEST(test_null_arguments_are_safe, tests_passed);

    PRINT_TEST_RESULTS(tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
