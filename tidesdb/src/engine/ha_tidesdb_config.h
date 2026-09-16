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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef HA_TIDESDB_CONFIG_H
#define HA_TIDESDB_CONFIG_H

/* translation of CREATE TABLE options and the session isolation level into the
   library column-family configuration.  create, open, truncate, and online DDL
   all build their column families through these helpers so the option-to-config
   mapping stays in one place.  include after ha_tidesdb.h so the table option
   struct and library config types are already visible. */

/* number of choices in each option enum, and thus the length of each map below.  named here so the
   maps declare a complete bound and the root translation unit can bound-check a resolved index
   against the extern arrays. */
constexpr uint TDB_COMPRESSION_CHOICE_COUNT = 5;
constexpr uint TDB_ISOLATION_CHOICE_COUNT = 5;
constexpr uint TDB_SYNC_MODE_CHOICE_COUNT = 3;

/* enum-index to library-constant maps for the COMPRESSION, ISOLATION_LEVEL, and db-level sync-mode
   options.  defined in ha_tidesdb_config.cc; the root translation unit reads the isolation and
   sync-mode maps directly when resolving a share's level and the database sync mode. */
extern const int tdb_compression_map[TDB_COMPRESSION_CHOICE_COUNT];
extern const int tdb_isolation_map[TDB_ISOLATION_CHOICE_COUNT];
extern const int tdb_sync_mode_map[TDB_SYNC_MODE_CHOICE_COUNT];

/**
 * resolve_effective_isolation
 * pick the library isolation level for a statement from the session level and the table default
 * @param thd the current server session
 * @param table_iso the table-level ISOLATION_LEVEL option value
 * @return the resolved library isolation level
 */
tidesdb_isolation_level_t resolve_effective_isolation(THD *thd,
                                                      tidesdb_isolation_level_t table_iso);

/**
 * tdb_table_option_defaults
 * fill a table's storage options from the session's tidesdb_default_* variables
 * @param thd the current server session, or NULL for the global values
 * @param out out -- every option set to what a table created now would inherit
 *
 * A server that can parse engine-specific option syntax applies these defaults itself as part of
 * binding each option; one that cannot has to start from them before reading whatever the table
 * actually named, or a table naming a single option would silently take library defaults for all
 * the rest.  The variables are declared beside the plugin's other system variables, which is why
 * this lives with them rather than with the reader that calls it.
 */
void tdb_table_option_defaults(THD *thd, ha_table_option_struct *out);

/**
 * tdb_compression_option_names
 * the accepted COMPRESSION values, NULL-terminated, in the order the engine numbers them
 * @return the name list, shared with the session variable of the same meaning
 */
const char *const *tdb_compression_option_names();

/**
 * tdb_isolation_option_names
 * the accepted ISOLATION_LEVEL values, NULL-terminated, likewise
 * @return the name list
 */
const char *const *tdb_isolation_option_names();

/**
 * tdb_table_options
 * the storage options in force for one table
 * @param tbl the table
 * @return the options, never NULL
 *
 * Defined only where the server has no option syntax of its own; elsewhere the compat layer reads
 * the struct the server itself bound.  The returned pointer is valid until the next call on the
 * same thread, which is all any caller needs: each reads the options for one table before asking
 * about another.
 */
const ha_table_option_struct *tdb_table_options(const TABLE *tbl);

/**
 * tdb_table_options_error
 * why a table's stored options could not be read
 * @param tbl the table
 * @param error out -- a message naming the offending option
 * @return true when the options are well formed and error is untouched
 *
 * CREATE TABLE and ALTER TABLE ask this so a mistyped option is refused with a message naming it,
 * rather than accepted and then quietly ignored for the life of the table.
 */
bool tdb_table_options_error(const TABLE *tbl, std::string *error);

/**
 * tdb_field_is_ttl_source
 * whether one column supplies each row's own expiry
 * @param dd_table_def the server's definition of the table, opaque here because only the servers
 *        that have one name the type
 * @param tbl the table
 * @param field_index the column
 * @return true when the column is named as the expiry source
 *
 * Defined only where a per-column option lives in the dictionary rather than on the Field.
 */
bool tdb_field_is_ttl_source(const void *dd_table_def, const TABLE *tbl, uint field_index);

/**
 * tdb_column_options_error
 * why a table's per-column attributes could not be accepted
 * @param dd_table_def the server's definition of the table
 * @param error out -- a message naming the offending column
 * @return true when every column attribute is well formed and at most one names the expiry source
 */
bool tdb_column_options_error(const void *dd_table_def, std::string *error);

/**
 * tdb_table_options_store
 * record a table's resolved storage options in its own column family
 * @param cf the table's data column family
 * @param opts the options, every one of them resolved
 *
 * Defined only where the server records just the options a table named, leaving the rest to be
 * resolved again at every open.  See OPTIONS_META_KEY.
 */
void tdb_table_options_store(tidesdb_column_family_t *cf, const ha_table_option_struct *opts);

/**
 * tdb_table_options_load
 * read back what tdb_table_options_store wrote
 * @param cf the table's data column family
 * @param out out -- the options, written only on success
 * @return true when a stored set was found and understood; false leaves the caller to fall back to
 *         the table's own attribute and the session defaults, which is what a table created before
 *         this was recorded needs
 */
bool tdb_table_options_load(tidesdb_column_family_t *cf, ha_table_option_struct *out);

/**
 * build_cf_config
 * build a library column-family config from a table's CREATE TABLE options
 * @param opts the table options, or NULL to take the library defaults
 * @return the populated column-family config
 */
tidesdb_column_family_config_t build_cf_config(const ha_table_option_struct *opts);

/**
 * data_cf_config
 * derive the data column family's config, forcing compression off when the table is encrypted
 * @param cfg the base config shared with the secondary-index families
 * @param encrypted true when the table stores rows encrypted
 * @return a copy of cfg with compression disabled for encrypted tables
 */
tidesdb_column_family_config_t data_cf_config(const tidesdb_column_family_config_t &cfg,
                                              bool encrypted);

/**
 * resolve_idx_cf
 * look up a secondary index column family by its derived name
 * @param db the open database handle
 * @param table_cf the table's base column-family name
 * @param key_name the index name
 * @param out_name receives the derived column-family name
 * @return the column family, or NULL when it does not exist
 */
tidesdb_column_family_t *resolve_idx_cf(tidesdb_t *db, const std::string &table_cf,
                                        const char *key_name, std::string &out_name);

#endif /* HA_TIDESDB_CONFIG_H */
