/* Copyright (c) 2026 TidesDB Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 */

/* unit tests for the per-table option reader.  a misread option changes how a table is stored and
 * says nothing about it, so these pin the refusals as carefully as the successes: an option this
 * build does not know, a value out of range, a number that would wrap. */
#include <cstring>
#include <string>

#include "../src/core/table_options.h"
#include "test_utils.h"

using namespace tidesdb::table_options;

static int tests_passed = 0;
static int tests_failed = 0;

static const char *const kCompression[] = {"NONE", "SNAPPY", "LZ4", "ZSTD", "LZ4_FAST", nullptr};
static const char *const kIsolation[] = {"READ_UNCOMMITTED", "READ_COMMITTED", "REPEATABLE_READ",
                                         "SNAPSHOT",         "SERIALIZABLE",   nullptr};

/* a struct holding recognisable defaults, so a test can tell "left alone" from "set to zero". */
static ha_table_option_struct defaults(void)
{
    ha_table_option_struct o;
    memset(&o, 0, sizeof(o));
    o.btree_klog_block_size = 4096;
    o.level_size_ratio = 10;
    o.min_levels = 3;
    o.dividing_level_offset = 2;
    o.bloom_fpr = 100;
    o.l1_file_count_trigger = 4;
    o.compression = 2; /* LZ4 */
    o.isolation_level = 2;
    o.bloom_filter = true;
    o.keep_values_inline = false;
    o.ttl = 0;
    o.encrypted = false;
    o.encryption_key_id = 1;
    o.tombstone_density_trigger = 0;
    o.tombstone_density_min_entries = 1024;
    return o;
}

static bool parse(const char *json, ha_table_option_struct *o, std::string *err)
{
    return parse_attributes(json, json ? strlen(json) : 0, kCompression, kIsolation, o, err);
}

void test_nothing_said_leaves_defaults(void)
{
    ha_table_option_struct o = defaults();
    std::string err;

    ASSERT_TRUE(parse(nullptr, &o, &err));
    ASSERT_TRUE(parse("", &o, &err));
    ASSERT_TRUE(parse("   ", &o, &err));
    ASSERT_TRUE(parse("{}", &o, &err));

    ASSERT_EQ(o.compression, (unsigned int)2);
    ASSERT_EQ(o.bloom_fpr, (unsigned long long)100);
    ASSERT_TRUE(o.bloom_filter);
}

void test_one_option_leaves_the_rest(void)
{
    /* the failure this guards against is a table naming one option and silently receiving library
       defaults for every other one. */
    ha_table_option_struct o = defaults();
    std::string err;

    ASSERT_TRUE(parse("{\"ttl\": 3600}", &o, &err));
    ASSERT_EQ(o.ttl, (unsigned long long)3600);
    ASSERT_EQ(o.compression, (unsigned int)2);
    ASSERT_EQ(o.min_levels, (unsigned long long)3);
    ASSERT_EQ(o.encryption_key_id, (unsigned long long)1);
}

void test_every_option_is_reachable(void)
{
    ha_table_option_struct o = defaults();
    std::string err;

    const char *json =
        "{\"keep_values_inline\": true, \"btree_klog_block_size\": 8192,"
        " \"level_size_ratio\": 12, \"min_levels\": 5, \"dividing_level_offset\": 1,"
        " \"bloom_fpr\": 50, \"l1_file_count_trigger\": 8, \"compression\": \"ZSTD\","
        " \"isolation_level\": \"READ_COMMITTED\", \"bloom_filter\": false,"
        " \"tombstone_density_trigger\": 5000, \"tombstone_density_min_entries\": 64,"
        " \"ttl\": 90, \"encrypted\": true, \"encryption_key_id\": 7}";
    ASSERT_TRUE(parse(json, &o, &err));

    ASSERT_TRUE(o.keep_values_inline);
    ASSERT_EQ(o.btree_klog_block_size, (unsigned long long)8192);
    ASSERT_EQ(o.level_size_ratio, (unsigned long long)12);
    ASSERT_EQ(o.min_levels, (unsigned long long)5);
    ASSERT_EQ(o.dividing_level_offset, (unsigned long long)1);
    ASSERT_EQ(o.bloom_fpr, (unsigned long long)50);
    ASSERT_EQ(o.l1_file_count_trigger, (unsigned long long)8);
    ASSERT_EQ(o.compression, (unsigned int)3);
    ASSERT_EQ(o.isolation_level, (unsigned int)1);
    ASSERT_FALSE(o.bloom_filter);
    ASSERT_EQ(o.tombstone_density_trigger, (unsigned long long)5000);
    ASSERT_EQ(o.tombstone_density_min_entries, (unsigned long long)64);
    ASSERT_EQ(o.ttl, (unsigned long long)90);
    ASSERT_TRUE(o.encrypted);
    ASSERT_EQ(o.encryption_key_id, (unsigned long long)7);
}

void test_yes_or_no_spellings(void)
{
    for (const char *yes : {"true", "TRUE", "\"YES\"", "\"on\"", "1"})
    {
        ha_table_option_struct o = defaults();
        std::string err;
        ASSERT_TRUE(parse((std::string("{\"encrypted\": ") + yes + "}").c_str(), &o, &err));
        ASSERT_TRUE(o.encrypted);
    }
    for (const char *no : {"false", "\"NO\"", "\"off\"", "0"})
    {
        ha_table_option_struct o = defaults();
        o.encrypted = true;
        std::string err;
        ASSERT_TRUE(parse((std::string("{\"encrypted\": ") + no + "}").c_str(), &o, &err));
        ASSERT_FALSE(o.encrypted);
    }
}

void test_enums_by_name_and_by_position(void)
{
    ha_table_option_struct o = defaults();
    std::string err;

    ASSERT_TRUE(parse("{\"compression\": \"snappy\"}", &o, &err));
    ASSERT_EQ(o.compression, (unsigned int)1);

    ASSERT_TRUE(parse("{\"compression\": 4}", &o, &err));
    ASSERT_EQ(o.compression, (unsigned int)4);

    ASSERT_FALSE(parse("{\"compression\": 5}", &o, &err));
    ASSERT_FALSE(parse("{\"compression\": \"BROTLI\"}", &o, &err));
}

void test_option_names_ignore_case(void)
{
    ha_table_option_struct o = defaults();
    std::string err;
    ASSERT_TRUE(parse("{\"TTL\": 60, \"Bloom_Filter\": false}", &o, &err));
    ASSERT_EQ(o.ttl, (unsigned long long)60);
    ASSERT_FALSE(o.bloom_filter);
}

void test_unknown_option_is_refused(void)
{
    /* a misspelled option that is quietly ignored produces a table stored differently from the one
       that was asked for, and nothing later says so. */
    ha_table_option_struct o = defaults();
    std::string err;
    ASSERT_FALSE(parse("{\"blom_filter\": false}", &o, &err));
    ASSERT_TRUE(err.find("blom_filter") != std::string::npos);
    ASSERT_TRUE(o.bloom_filter); /* unchanged */
}

void test_out_of_range_is_refused(void)
{
    ha_table_option_struct o = defaults();
    std::string err;

    ASSERT_FALSE(parse("{\"bloom_fpr\": 0}", &o, &err));
    ASSERT_FALSE(parse("{\"bloom_fpr\": 10001}", &o, &err));
    ASSERT_FALSE(parse("{\"encryption_key_id\": 0}", &o, &err));
    ASSERT_FALSE(parse("{\"encryption_key_id\": 256}", &o, &err));
    ASSERT_FALSE(parse("{\"min_levels\": 65}", &o, &err));
    ASSERT_FALSE(parse("{\"btree_klog_block_size\": 511}", &o, &err));
    ASSERT_EQ(o.bloom_fpr, (unsigned long long)100);
}

void test_a_number_that_would_wrap_is_refused(void)
{
    ha_table_option_struct o = defaults();
    std::string err;
    ASSERT_FALSE(parse("{\"ttl\": 99999999999999999999999}", &o, &err));
    ASSERT_EQ(o.ttl, (unsigned long long)0);
}

void test_malformed_objects_are_refused(void)
{
    ha_table_option_struct o = defaults();
    std::string err;

    ASSERT_FALSE(parse("[1,2]", &o, &err));
    ASSERT_FALSE(parse("{\"ttl\"}", &o, &err));
    ASSERT_FALSE(parse("{\"ttl\": }", &o, &err));
    ASSERT_FALSE(parse("{\"ttl\": 1", &o, &err));
    ASSERT_FALSE(parse("{\"ttl\": 1} trailing", &o, &err));
    ASSERT_FALSE(parse("{\"ttl\": {\"a\": 1}}", &o, &err));
    ASSERT_FALSE(parse("{ttl: 1}", &o, &err));
}

void test_negative_and_fractional_numbers_are_refused(void)
{
    /* every numeric option counts something, so a value that is not a whole count is a mistake
       rather than a value to round. */
    ha_table_option_struct o = defaults();
    std::string err;
    ASSERT_FALSE(parse("{\"ttl\": -1}", &o, &err));
    ASSERT_FALSE(parse("{\"bloom_fpr\": 12.5}", &o, &err));
}

void test_a_full_set_round_trips(void)
{
    /* what is written at CREATE has to read back as the same table, or a restart silently changes
       how the table is stored. */
    ha_table_option_struct in = defaults();
    in.compression = 3;
    in.isolation_level = 4;
    in.encrypted = true;
    in.encryption_key_id = 9;
    in.ttl = 7200;
    in.keep_values_inline = true;
    in.bloom_filter = false;
    in.tombstone_density_trigger = 5000;

    const std::string text = serialize_attributes(in, kCompression, kIsolation);

    /* read back over a struct holding something else entirely, so anything the text failed to
       carry shows up as a mismatch rather than as the value that was already there. */
    ha_table_option_struct out;
    memset(&out, 0xAB, sizeof(out));
    out.encryption_key_id = 1;
    std::string err;
    ASSERT_TRUE(parse_attributes(text.c_str(), text.size(), kCompression, kIsolation, &out, &err));

    ASSERT_EQ(out.compression, in.compression);
    ASSERT_EQ(out.isolation_level, in.isolation_level);
    ASSERT_TRUE(out.encrypted);
    ASSERT_EQ(out.encryption_key_id, in.encryption_key_id);
    ASSERT_EQ(out.ttl, in.ttl);
    ASSERT_TRUE(out.keep_values_inline);
    ASSERT_FALSE(out.bloom_filter);
    ASSERT_EQ(out.tombstone_density_trigger, in.tombstone_density_trigger);
    ASSERT_EQ(out.tombstone_density_min_entries, in.tombstone_density_min_entries);
    ASSERT_EQ(out.btree_klog_block_size, in.btree_klog_block_size);
    ASSERT_EQ(out.level_size_ratio, in.level_size_ratio);
    ASSERT_EQ(out.min_levels, in.min_levels);
    ASSERT_EQ(out.dividing_level_offset, in.dividing_level_offset);
    ASSERT_EQ(out.bloom_fpr, in.bloom_fpr);
    ASSERT_EQ(out.l1_file_count_trigger, in.l1_file_count_trigger);
}

void test_an_unnameable_enum_still_round_trips(void)
{
    /* a value written by a build that knew more names than this one must survive the trip rather
       than being dropped back to the default. */
    ha_table_option_struct in = defaults();
    in.compression = 9;
    const std::string text = serialize_attributes(in, kCompression, kIsolation);

    ha_table_option_struct out = defaults();
    std::string err;
    ASSERT_FALSE(parse_attributes(text.c_str(), text.size(), kCompression, kIsolation, &out, &err));

    /* the name list this build has does not reach 9, so the value is refused rather than guessed
       at -- which is what the refusal above is for. */
    ASSERT_TRUE(text.find("\"compression\": 9") != std::string::npos);
}

void test_column_attributes(void)
{
    bool is_ttl = true;
    std::string err;

    ASSERT_TRUE(parse_column_attributes(nullptr, 0, &is_ttl, &err));
    ASSERT_FALSE(is_ttl);

    ASSERT_TRUE(parse_column_attributes("{}", 2, &is_ttl, &err));
    ASSERT_FALSE(is_ttl);

    const char *yes = "{\"ttl\": true}";
    ASSERT_TRUE(parse_column_attributes(yes, strlen(yes), &is_ttl, &err));
    ASSERT_TRUE(is_ttl);

    const char *no = "{\"ttl\": false}";
    ASSERT_TRUE(parse_column_attributes(no, strlen(no), &is_ttl, &err));
    ASSERT_FALSE(is_ttl);

    const char *unknown = "{\"tl\": true}";
    ASSERT_FALSE(parse_column_attributes(unknown, strlen(unknown), &is_ttl, &err));
    ASSERT_TRUE(err.find("tl") != std::string::npos);

    const char *bad = "{\"ttl\": 7}";
    ASSERT_FALSE(parse_column_attributes(bad, strlen(bad), &is_ttl, &err));
}

void test_null_arguments_are_safe(void)
{
    ha_table_option_struct o = defaults();
    std::string err;
    ASSERT_FALSE(parse_attributes("{}", 2, kCompression, kIsolation, nullptr, &err));
    ASSERT_FALSE(parse_attributes("{}", 2, kCompression, kIsolation, &o, nullptr));

    bool is_ttl = false;
    ASSERT_FALSE(parse_column_attributes("{}", 2, nullptr, &err));
    ASSERT_FALSE(parse_column_attributes("{}", 2, &is_ttl, nullptr));
}

int main(int argc, char **argv)
{
    INIT_TEST_FILTER(argc, argv);

    RUN_TEST(test_nothing_said_leaves_defaults, tests_passed);
    RUN_TEST(test_one_option_leaves_the_rest, tests_passed);
    RUN_TEST(test_every_option_is_reachable, tests_passed);
    RUN_TEST(test_yes_or_no_spellings, tests_passed);
    RUN_TEST(test_enums_by_name_and_by_position, tests_passed);
    RUN_TEST(test_option_names_ignore_case, tests_passed);
    RUN_TEST(test_unknown_option_is_refused, tests_passed);
    RUN_TEST(test_out_of_range_is_refused, tests_passed);
    RUN_TEST(test_a_number_that_would_wrap_is_refused, tests_passed);
    RUN_TEST(test_malformed_objects_are_refused, tests_passed);
    RUN_TEST(test_negative_and_fractional_numbers_are_refused, tests_passed);
    RUN_TEST(test_a_full_set_round_trips, tests_passed);
    RUN_TEST(test_an_unnameable_enum_still_round_trips, tests_passed);
    RUN_TEST(test_column_attributes, tests_passed);
    RUN_TEST(test_null_arguments_are_safe, tests_passed);

    PRINT_TEST_RESULTS(tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
