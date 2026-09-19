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

/* per-table storage options for a server that cannot parse engine-specific option syntax.
 *
 * such a server still lets a table carry an attribute for its engine -- one JSON object, stored in
 * the dictionary verbatim and handed back on the table's share without ever being looked inside.
 * so the option names, their types and their ranges are the engine's to define and to check, which
 * is what happens here: the session defaults are laid down first, then whatever the table named is
 * read over them by the server-free reader in src/core/table_options.
 *
 * CMakeLists.txt compiles this only for that server.  Elsewhere the server binds each option itself
 * and the compat layer reads the struct it produced. */

#include <string>

#include "ha_tidesdb.h"
#include "sql_class.h"
#include "src/engine/ha_tidesdb_config.h"
#include "src/handler/ha_tidesdb_internal.h"

/* The options last read on this thread.
 *
 * A caller reads the options for one table before asking about another -- create() and open() each
 * hold a single table, and inplace ALTER reads the new definition throughout -- so one slot per
 * thread is enough, and it spares every call site a lifetime it would otherwise have to manage.
 * The contract is stated at the declaration: the pointer is valid until the next call on the same
 * thread. */
static thread_local ha_table_option_struct tdb_thread_options;

/**
 * tdb_read_attribute
 * lay down the session defaults, then read the table's attribute over them
 * @param tbl the table, or NULL for the defaults alone
 * @param out out -- the resulting options
 * @param error out -- set when the attribute could not be read, untouched otherwise
 * @return true when the attribute was understood, or there was none
 */
static bool tdb_read_attribute(const TABLE *tbl, ha_table_option_struct *out, std::string *error)
{
    tdb_table_option_defaults(current_thd, out);
    if (!tbl || !tbl->s) return true;

    const LEX_CSTRING &attr = tbl->s->engine_attribute;
    if (!attr.str || attr.length == 0) return true;

    return tidesdb::table_options::parse_attributes(attr.str, attr.length,
                                                    tdb_compression_option_names(),
                                                    tdb_isolation_option_names(), out, error);
}

const ha_table_option_struct *tdb_table_options(const TABLE *tbl)
{
    std::string error;
    if (!tdb_read_attribute(tbl, &tdb_thread_options, &error))
    {
        /* CREATE and ALTER refuse an attribute they cannot read, so reaching here means a table
           whose attribute was written by a build that understood more than this one does.  Opening
           it on the defaults keeps the data readable and says plainly that the options it was
           created with are not the ones in force. */
        sql_print_warning("[TIDESDB] table '%s.%s' has a storage attribute this build cannot read "
                          "(%s); it is open with the session default options",
                          tbl && tbl->s && tbl->s->db.str ? tbl->s->db.str : "?",
                          tbl && tbl->s && tbl->s->table_name.str ? tbl->s->table_name.str : "?",
                          error.c_str());
        tdb_table_option_defaults(current_thd, &tdb_thread_options);
    }
    return &tdb_thread_options;
}

bool tdb_table_options_error(const TABLE *tbl, std::string *error)
{
    ha_table_option_struct scratch;
    std::string why;
    if (tdb_read_attribute(tbl, &scratch, &why)) return true;

    if (error) *error = why;
    return false;
}

bool tdb_field_is_ttl_source(const TABLE *tbl, uint field_index)
{
    if (!tbl || !tbl->s || field_index >= tbl->s->fields) return false;

    const Field *field = tbl->s->field[field_index];
    if (!field) return false;

    const LEX_CSTRING &attr = field->m_engine_attribute;
    if (!attr.str || attr.length == 0) return false;

    bool is_ttl = false;
    std::string error;
    if (!tidesdb::table_options::parse_column_attributes(attr.str, attr.length, &is_ttl, &error))
    {
        sql_print_warning("[TIDESDB] column '%s' has an attribute this build cannot read (%s); "
                          "it is not treated as the row expiry source",
                          field->field_name ? field->field_name : "?", error.c_str());
        return false;
    }
    return is_ttl;
}

bool tdb_column_options_error(const TABLE *tbl, std::string *error)
{
    if (!tbl || !tbl->s) return true;

    int ttl_columns = 0;
    for (uint i = 0; i < tbl->s->fields; i++)
    {
        const Field *col = tbl->s->field[i];
        if (!col || !col->m_engine_attribute.str || col->m_engine_attribute.length == 0) continue;

        bool is_ttl = false;
        std::string why;
        if (!tidesdb::table_options::parse_column_attributes(
                col->m_engine_attribute.str, col->m_engine_attribute.length, &is_ttl, &why))
        {
            if (error)
                *error =
                    "column '" + std::string(col->field_name ? col->field_name : "?") + "': " + why;
            return false;
        }
        if (is_ttl) ttl_columns++;
    }

    /* Two columns each claiming to hold the row's expiry cannot both be it, and which one won
       would depend on the order the dictionary happened to return them in. */
    if (ttl_columns > 1)
    {
        if (error) *error = "more than one column is named as the row expiry source";
        return false;
    }
    return true;
}

/* the meta key one index's shape is stored under, prefix then the index name */
static std::string tdb_key_shape_meta_key(const std::string &index_name)
{
    std::string k((const char *)KEYSHAPE_META_PREFIX, KEYSHAPE_META_PREFIX_LEN);
    k.append(index_name);
    return k;
}

void tdb_key_shape_store(tidesdb_column_family_t *data_cf, const std::string &index_name,
                         const KEY *key_info)
{
    if (!data_cf || !key_info || index_name.empty() || !tdb_global) return;

    std::string val;
    val.push_back((char)KEYSHAPE_VERSION);
    const uint16 n = (uint16)key_info->user_defined_key_parts;
    val.push_back((char)(n & 0xff));
    val.push_back((char)((n >> 8) & 0xff));
    for (uint p = 0; p < n; p++)
    {
        const Field *f = key_info->key_part[p].field;
        val.push_back((char)(f && TDB_FIELD_IS_NULLABLE(f) ? 1 : 0));
    }

    const std::string key = tdb_key_shape_meta_key(index_name);

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return;

    if (tidesdb_txn_put(txn, data_cf, (const uint8_t *)key.data(), key.size(),
                        (const uint8_t *)val.data(), val.size(), TIDESDB_TTL_NONE) == TDB_SUCCESS)
        (void)tidesdb_txn_commit(txn);
    else
        (void)tidesdb_txn_rollback(txn);

    tidesdb_txn_free(txn);
}

bool tdb_key_shape_load(tidesdb_column_family_t *data_cf, const std::string &index_name,
                        std::vector<uint8> *out)
{
    if (!out) return false;
    out->clear();
    if (!data_cf || index_name.empty() || !tdb_global) return false;

    const std::string key = tdb_key_shape_meta_key(index_name);

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return false;

    uint8_t *val = NULL;
    size_t vlen = 0;
    tdb_owned_buf vg(val);
    const bool found = tidesdb_txn_get(txn, data_cf, (const uint8_t *)key.data(), key.size(), &val,
                                       &vlen) == TDB_SUCCESS &&
                       val && vlen >= 3;

    bool ok = false;
    if (found && val[0] == KEYSHAPE_VERSION)
    {
        const uint16 n = (uint16)(val[1] | (val[2] << 8));
        if (vlen >= (size_t)3 + n)
        {
            for (uint16 i = 0; i < n; i++) out->push_back(val[3 + i]);
            ok = true;
        }
    }

    (void)tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
    if (!ok) out->clear();
    return ok;
}

void tdb_table_options_store(tidesdb_column_family_t *cf, const ha_table_option_struct *opts)
{
    if (!cf || !opts || !tdb_global) return;

    const std::string text = tidesdb::table_options::serialize_attributes(
        *opts, tdb_compression_option_names(), tdb_isolation_option_names());

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return;

    if (tidesdb_txn_put(txn, cf, OPTIONS_META_KEY, OPTIONS_META_KEY_LEN,
                        (const uint8_t *)text.data(), text.size(), TIDESDB_TTL_NONE) == TDB_SUCCESS)
        (void)tidesdb_txn_commit(txn);
    else
        (void)tidesdb_txn_rollback(txn);

    tidesdb_txn_free(txn);
}

bool tdb_table_options_load(tidesdb_column_family_t *cf, ha_table_option_struct *out)
{
    if (!cf || !out || !tdb_global) return false;

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return false;

    uint8_t *val = NULL;
    size_t vlen = 0;
    tdb_owned_buf vg(val);
    const bool found = tidesdb_txn_get(txn, cf, OPTIONS_META_KEY, OPTIONS_META_KEY_LEN, &val,
                                       &vlen) == TDB_SUCCESS &&
                       val && vlen > 0;

    bool ok = false;
    if (found)
    {
        /* The stored object names every option, so it is read over a struct that holds nothing --
           anything missing from it would otherwise silently keep whatever was already there. */
        ha_table_option_struct parsed;
        memset(&parsed, 0, sizeof(parsed));

        std::string error;
        ok = tidesdb::table_options::parse_attributes(
            (const char *)val, vlen, tdb_compression_option_names(), tdb_isolation_option_names(),
            &parsed, &error);
        if (ok)
            *out = parsed;
        else
            sql_print_warning(
                "[TIDESDB] a table's stored options could not be read (%s); it is open with the "
                "session default options",
                error.c_str());
    }

    (void)tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
    return ok;
}
