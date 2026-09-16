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

/* server-free core for the per-table storage options.
 *
 * a table carries a handful of settings the engine reads when it builds the table's column
 * families: how it compresses, whether it keeps a bloom filter, what isolation its readers get,
 * how long a row lives.  where the server can parse engine-specific option syntax the values
 * arrive already typed; where it cannot, they arrive as one JSON object and this is what turns
 * that text into the same struct.
 *
 * the reader lives here, away from any server call, so it is unit-tested on its own -- a
 * misread option silently changes how a table is stored, and the failure shows up as an
 * unexplained performance or durability difference rather than as an error. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/* per-table storage options.
 *
 * the option list that binds these fields to their session-variable defaults lives next to the
 * variables it references; the struct itself is here so the column-family config builders, and the
 * reader below, can share it without either of them reaching for a server header. */
struct ha_table_option_struct
{
    unsigned long long btree_klog_block_size;
    unsigned long long level_size_ratio;
    unsigned long long min_levels;
    unsigned long long dividing_level_offset;
    unsigned long long bloom_fpr; /* parts per 10000 -- 100 = 1% */
    unsigned long long l1_file_count_trigger;
    unsigned int compression;
    unsigned int isolation_level;
    bool bloom_filter;
    bool keep_values_inline;              /* hold every value in the klog, ignoring the db
                                             value_separation_threshold */
    unsigned long long ttl;               /* default TTL in seconds (0 = no expiration) */
    bool encrypted;                       /* data-at-rest encryption */
    unsigned long long encryption_key_id; /* which key encrypts it (default 1) */
    /* Tombstone-density compaction trigger.  Stored as parts-per-10000 (e.g. 5000 = 0.50 ratio)
       so the option list can use integer storage; converted to a double at build_cf_config time. */
    unsigned long long tombstone_density_trigger;
    unsigned long long tombstone_density_min_entries;
};

namespace tidesdb
{
namespace table_options
{

/**
 * parse_attributes
 * read a JSON object of table options over a struct already holding the defaults
 * @param json the object text, as the server stored it
 * @param len its length
 * @param compression_names NULL-terminated list of the accepted COMPRESSION values, in the order
 *        the engine numbers them
 * @param isolation_names NULL-terminated list of the accepted ISOLATION_LEVEL values, likewise
 * @param inout in/out -- the options; every member the object does not name is left as it was
 * @param error out -- a message naming what was wrong, set only on failure
 * @return true when the whole object was understood
 *
 * Every member the object does not name keeps the default it arrived with, so a table naming one
 * option is not silently given library defaults for the other fourteen.  An option the parse does
 * not recognise, or a value outside what the engine accepts, fails the whole parse rather than
 * being skipped: a misspelled option that is quietly ignored produces a table stored differently
 * from the one that was asked for, and nothing later says so.
 */
bool parse_attributes(const char *json, size_t len, const char *const *compression_names,
                      const char *const *isolation_names, ha_table_option_struct *inout,
                      std::string *error);

/**
 * serialize_attributes
 * write a complete set of options as the object parse_attributes reads
 * @param opts the options
 * @param compression_names the COMPRESSION value names, as above
 * @param isolation_names the ISOLATION_LEVEL value names, as above
 * @return the object text, naming every option
 *
 * Every option is named, not just the ones a table asked for, because the point of writing this
 * down is that a table's storage is settled when it is created: an option left out would be
 * resolved again from whatever the session default happened to be at the next open, and the same
 * table would then behave differently depending on who opened it first.
 */
std::string serialize_attributes(const ha_table_option_struct &opts,
                                 const char *const *compression_names,
                                 const char *const *isolation_names);

/**
 * parse_column_attributes
 * read a JSON object of per-column options
 * @param json the object text
 * @param len its length
 * @param is_ttl_source out -- true when the column is named as the per-row expiry source
 * @param error out -- a message naming what was wrong, set only on failure
 * @return true when the whole object was understood
 *
 * A column has exactly one option, and the same rule applies as above: an option this build does
 * not recognise fails the parse rather than being skipped, so a mistyped one is refused at CREATE
 * instead of being stored and then ignored for the life of the table.
 */
bool parse_column_attributes(const char *json, size_t len, bool *is_ttl_source, std::string *error);

}  // namespace table_options
}  // namespace tidesdb
