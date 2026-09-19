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
/*
  Foreign-key support for the TidesDB storage engine.

  the server parses a FOREIGN KEY clause, hands it to the engine once at create
  time, prelocks the child tables using the lists the engine reports back, and
  displays whatever the engine describes.  It never checks a constraint itself.
  So this engine keeps its own catalog of the constraints, loads it on both sides
  at open, and runs the referential checks inside its own row operations.

  A child row whose referenced parent key is absent is rejected, and a delete or update of a parent
  row a child still references is either blocked or followed into the child, depending on the
  constraint's ON DELETE and ON UPDATE actions -- RESTRICT, NO ACTION, CASCADE and SET NULL are all
  carried out here.  The referenced columns may be the parent's primary key or any unique key,
  whether or not its columns are nullable; a foreign key column declared descending is the one shape
  still refused, because the child match is built on a forward sort key.
*/

#include "ha_tidesdb.h"

#include "src/handler/ha_tidesdb_fk_source.h"

#include <cstring>
#include <string>
#include <vector>

#include "key.h"
/* native_strcasecmp -- the server's portable spelling; plain strcasecmp is POSIX-only and MSVC
   has no such name. */
#include "m_string.h"
#include "sql_class.h"
#include "sql_table.h"
#include "src/core/cf_name.h"
#include "src/handler/ha_tidesdb_internal.h"

/* The engine keeps every table's foreign keys in one internal column family.
   Each constraint is stored twice, once under a child-side key so the
   referencing table finds it at open, and once under a parent-side key so the
   referenced table finds it too. */
static constexpr const char FK_CATALOG_CF[] = "__tidesdb_fk_catalog";
static constexpr uint8_t FK_REC_CHILD = 'c';
static constexpr uint8_t FK_REC_PARENT = 'p';
/* The catalog record format.  One format is current, and a record written by any other is refused
   rather than guessed at, which fk_load reports rather than passing over in silence. */
static constexpr uint8_t FK_SER_VERSION = 2;

/* ---- small serialization helpers -------------------------------------- */

static void fk_put_str(std::string &out, const std::string &s)
{
    uint16 n = (uint16)s.size();
    out.push_back((char)(n & 0xff));
    out.push_back((char)((n >> 8) & 0xff));
    out.append(s);
}

static bool fk_get_str(const uint8_t *&p, const uint8_t *end, std::string &out)
{
    if (p + 2 > end) return false;
    uint16 n = (uint16)(p[0] | (p[1] << 8));
    p += 2;
    if (p + n > end) return false;
    out.assign((const char *)p, n);
    p += n;
    return true;
}

static void fk_put_names(std::string &out, const std::vector<std::string> &v)
{
    uint16 n = (uint16)v.size();
    out.push_back((char)(n & 0xff));
    out.push_back((char)((n >> 8) & 0xff));
    for (const auto &s : v) fk_put_str(out, s);
}

static bool fk_get_names(const uint8_t *&p, const uint8_t *end, std::vector<std::string> &v)
{
    if (p + 2 > end) return false;
    uint16 n = (uint16)(p[0] | (p[1] << 8));
    p += 2;
    for (uint16 i = 0; i < n; i++)
    {
        std::string s;
        if (!fk_get_str(p, end, s)) return false;
        v.push_back(std::move(s));
    }
    return true;
}

/* A constraint as it travels through the catalog, with the column names kept as
   text so each side can resolve them against its own table at open. */
struct fk_catalog_entry
{
    std::string name;
    std::string child_cf;
    std::string child_db;
    std::string child_table;
    std::vector<std::string> child_columns;
    std::vector<uint8> child_nullable;
    std::string ref_db;
    std::string ref_table;
    std::string parent_cf;
    std::vector<std::string> ref_columns;
    std::string child_index_name;
    std::string parent_index_name;      /* parent key the fk references, resolved at create */
    std::vector<uint8> parent_nullable; /* whether each parent key column is nullable       */
    uint8 parent_is_pk;                 /* whether that key is the parent primary key       */
    uint8 on_delete;
    uint8 on_update;
};

static void fk_serialize(const fk_catalog_entry &e, std::string &out)
{
    out.clear();
    out.push_back((char)FK_SER_VERSION);
    fk_put_str(out, e.name);
    fk_put_str(out, e.child_cf);
    fk_put_str(out, e.child_db);
    fk_put_str(out, e.child_table);
    fk_put_names(out, e.child_columns);
    fk_put_str(out, e.ref_db);
    fk_put_str(out, e.ref_table);
    fk_put_str(out, e.parent_cf);
    fk_put_names(out, e.ref_columns);
    fk_put_str(out, e.child_index_name);
    fk_put_str(out, e.parent_index_name);
    out.push_back((char)e.parent_is_pk);
    out.push_back((char)e.on_delete);
    out.push_back((char)e.on_update);
    uint16 nn = (uint16)e.child_nullable.size();
    out.push_back((char)(nn & 0xff));
    out.push_back((char)((nn >> 8) & 0xff));
    for (uint8 b : e.child_nullable) out.push_back((char)b);
    uint16 pn = (uint16)e.parent_nullable.size();
    out.push_back((char)(pn & 0xff));
    out.push_back((char)((pn >> 8) & 0xff));
    for (uint8 b : e.parent_nullable) out.push_back((char)b);
}

static bool fk_deserialize(const uint8_t *p, size_t len, fk_catalog_entry &e)
{
    const uint8_t *end = p + len;
    if (p >= end || *p != FK_SER_VERSION) return false;
    p++;
    if (!fk_get_str(p, end, e.name)) return false;
    if (!fk_get_str(p, end, e.child_cf)) return false;
    if (!fk_get_str(p, end, e.child_db)) return false;
    if (!fk_get_str(p, end, e.child_table)) return false;
    if (!fk_get_names(p, end, e.child_columns)) return false;
    if (!fk_get_str(p, end, e.ref_db)) return false;
    if (!fk_get_str(p, end, e.ref_table)) return false;
    if (!fk_get_str(p, end, e.parent_cf)) return false;
    if (!fk_get_names(p, end, e.ref_columns)) return false;
    if (!fk_get_str(p, end, e.child_index_name)) return false;
    if (!fk_get_str(p, end, e.parent_index_name)) return false;
    if (p + 3 > end) return false;
    e.parent_is_pk = *p++;
    e.on_delete = *p++;
    e.on_update = *p++;
    if (p + 2 > end) return false;
    uint16 nn = (uint16)(p[0] | (p[1] << 8));
    p += 2;
    if (p + nn > end) return false;
    for (uint16 i = 0; i < nn; i++) e.child_nullable.push_back(*p++);
    if (p + 2 > end) return false;
    uint16 pn = (uint16)(p[0] | (p[1] << 8));
    p += 2;
    if (p + pn > end) return false;
    for (uint16 i = 0; i < pn; i++) e.parent_nullable.push_back(*p++);
    return true;
}

/* ---- catalog column family + record keys ------------------------------ */

static tidesdb_column_family_t *fk_catalog_cf()
{
    tidesdb_column_family_t *cf = tidesdb_get_column_family(tdb_global, FK_CATALOG_CF);
    if (cf) return cf;
    tidesdb_column_family_config_t cfg = tidesdb_default_column_family_config();
    if (tidesdb_create_column_family(tdb_global, FK_CATALOG_CF, &cfg) != TDB_SUCCESS)
        return tidesdb_get_column_family(tdb_global, FK_CATALOG_CF);
    return tidesdb_get_column_family(tdb_global, FK_CATALOG_CF);
}

/* record key "c" + child_cf + '\0' + name  (a table's outgoing constraints) */
static std::string fk_child_key(const std::string &child_cf, const std::string &name)
{
    std::string k;
    k.push_back((char)FK_REC_CHILD);
    k.append(child_cf);
    k.push_back('\0');
    k.append(name);
    return k;
}

/* record key "p" + parent_cf + '\0' + child_cf + '\0' + name (incoming ones) */
static std::string fk_parent_key(const std::string &parent_cf, const std::string &child_cf,
                                 const std::string &name)
{
    std::string k;
    k.push_back((char)FK_REC_PARENT);
    k.append(parent_cf);
    k.push_back('\0');
    k.append(child_cf);
    k.push_back('\0');
    k.append(name);
    return k;
}

/* ---- helpers over the server table definition ------------------------- */

/* the index whose leading parts are exactly these columns in order, or -1 */
static int fk_find_covering_index(TABLE *table, const std::vector<std::string> &cols)
{
    for (uint i = 0; i < table->s->keys; i++)
    {
        KEY *ki = &table->key_info[i];
        if (ki->user_defined_key_parts < cols.size()) continue;
        bool match = true;
        for (uint p = 0; p < cols.size(); p++)
        {
            const char *fn = TDB_FIELD_NAME(ki->key_part[p].field);
            if (!fn || strcmp(fn, cols[p].c_str()) != 0)
            {
                match = false;
                break;
            }
        }
        if (match) return (int)i;
    }
    return -1;
}

static int fk_field_index(TABLE *table, const std::string &col)
{
    for (uint i = 0; i < table->s->fields; i++)
    {
        const char *fn = TDB_FIELD_NAME(table->field[i]);
        if (fn && strcmp(fn, col.c_str()) == 0) return (int)i;
    }
    return -1;
}

/* ---- rename-time fixup ------------------------------------------------- */

/* The catalog addresses a table by its column-family name, in the record keys on both sides and in
   the records themselves, and it carries the database and table names the cascade path matches open
   tables by.  A rename changes all of those at once, so every record naming the table has to be
   rewritten and re-keyed, or it is left addressing a name nothing answers to: the renamed table's
   own load finds no constraint and stops enforcing it, and a table referencing the renamed one
   probes a column family that no longer exists.  ALTER TABLE reaches this too, since the copy
   algorithm builds the new table under a temporary name and renames it into place. */
/* static */
int ha_tidesdb::fk_rename_catalog(const char *from, const char *to)
{
    const std::string old_cf = path_to_cf_name(from);
    const std::string new_cf = path_to_cf_name(to);
    if (old_cf == new_cf) return 0;

    tidesdb_column_family_t *cf = tidesdb_get_column_family(tdb_global, FK_CATALOG_CF);
    if (!cf) return 0;

    /* The catalog holds the names the server knows the table by, which are not the names its path
       spells -- a character the filesystem cannot take is encoded on the way out.  Decoding brings
       back what an open table's share reports, which is what the cascade path compares against. */
    const tidesdb::cf_name::table_path dest = tidesdb::cf_name::split_table_path(to);
    char new_db_buf[FN_REFLEN];
    char new_table_buf[FN_REFLEN];
    filename_to_tablename(dest.db.c_str(), new_db_buf, sizeof(new_db_buf));
    filename_to_tablename(dest.table.c_str(), new_table_buf, sizeof(new_table_buf));
    const std::string new_db(new_db_buf);
    const std::string new_table(new_table_buf);

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return 0;

    /* Each constraint is stored twice, under a child-side key and a parent-side key, with the same
       record under both.  Materializing only the child-side copy visits each constraint once, and
       both keys are rebuilt from the record either way. */
    std::vector<std::string> stale_keys;
    std::vector<std::pair<std::string, std::string>> fresh;
    tidesdb_iter_t *it = NULL;
    if (tidesdb_iter_new(txn, cf, &it) == TDB_SUCCESS && it)
    {
        const uint8_t lo0 = 0;
        tidesdb_iter_seek(it, &lo0, 1);
        while (tidesdb_iter_valid(it))
        {
            uint8_t *k = NULL, *v = NULL;
            size_t ks = 0, vs = 0;
            if (tidesdb_iter_key(it, &k, &ks) == TDB_SUCCESS &&
                tidesdb_iter_value(it, &v, &vs) == TDB_SUCCESS)
            {
                fk_catalog_entry e;
                if (ks > 0 && k[0] == FK_REC_CHILD && fk_deserialize(v, vs, e) &&
                    (e.child_cf == old_cf || e.parent_cf == old_cf))
                {
                    stale_keys.push_back(fk_child_key(e.child_cf, e.name));
                    stale_keys.push_back(fk_parent_key(e.parent_cf, e.child_cf, e.name));

                    /* A self-reference names the table on both sides and updates both. */
                    if (e.child_cf == old_cf)
                    {
                        e.child_cf = new_cf;
                        e.child_db = new_db;
                        e.child_table = new_table;
                    }
                    if (e.parent_cf == old_cf)
                    {
                        e.parent_cf = new_cf;
                        e.ref_db = new_db;
                        e.ref_table = new_table;
                    }

                    std::string val;
                    fk_serialize(e, val);
                    fresh.emplace_back(fk_child_key(e.child_cf, e.name), val);
                    fresh.emplace_back(fk_parent_key(e.parent_cf, e.child_cf, e.name), val);
                }
                tidesdb_free(k);
                tidesdb_free(v);
            }
            tidesdb_iter_next(it);
        }
        tidesdb_iter_free(it);
    }

    /* Every stale key goes before any fresh one.  A constraint where only one side was renamed
       keeps one of its two keys unchanged, and writing after deleting is what leaves that key
       holding the updated record rather than nothing. */
    for (const auto &k : stale_keys)
        tidesdb_txn_delete(txn, cf, (const uint8_t *)k.data(), k.size());
    for (const auto &kv : fresh)
        tidesdb_txn_put(txn, cf, (const uint8_t *)kv.first.data(), kv.first.size(),
                        (const uint8_t *)kv.second.data(), kv.second.size(), TIDESDB_TTL_NONE);

    if (tidesdb_txn_commit(txn) != TDB_SUCCESS)
    {
        tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
        sql_print_error("[TIDESDB] could not move the foreign-key catalog from '%s' to '%s'; "
                        "constraints on the renamed table are NOT enforced until it is recreated",
                        old_cf.c_str(), new_cf.c_str());
        return HA_ERR_GENERIC;
    }
    tidesdb_txn_free(txn);
    return 0;
}

/* ---- create-time persistence ------------------------------------------ */

int ha_tidesdb::fk_persist_defs(const char *path, TABLE *table_arg, HA_CREATE_INFO *create_info)
{
    std::vector<tdb_fk_spec> specs;
    if (!tdb_fk_extract_specs(ha_thd(), table_arg, create_info, specs)) return HA_ERR_GENERIC;
    if (specs.empty()) return 0;

    const std::string child_cf = path_to_cf_name(path);

    std::vector<std::pair<std::string, std::string>> records; /* key, value */
    uint anon = 0;

    for (const tdb_fk_spec &spec : specs)
    {
        fk_catalog_entry e;
        e.child_cf = child_cf;
        if (table_arg->s->db.str) e.child_db.assign(table_arg->s->db.str, table_arg->s->db.length);
        if (table_arg->s->table_name.str)
            e.child_table.assign(table_arg->s->table_name.str, table_arg->s->table_name.length);

        e.on_delete = spec.on_delete;
        e.on_update = spec.on_update;
        e.child_columns = spec.child_columns;
        e.ref_columns = spec.ref_columns;
        e.ref_table = spec.ref_table;

        /* An unnamed constraint gets the same synthesised name on every server, so a table dumped
           from one and loaded into the other keeps its catalog keys. */
        if (!spec.name.empty())
            e.name = spec.name;
        else
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s_ibfk_%u", table_arg->s->table_name.str, ++anon);
            e.name = buf;
        }

        /* A statement that named no schema means the child's own. */
        e.ref_db =
            !spec.ref_db.empty()
                ? spec.ref_db
                : (table_arg->s->db.str ? std::string(table_arg->s->db.str, table_arg->s->db.length)
                                        : std::string());

        /* Record whether each referencing column is nullable so the parent side can rebuild the
           child index prefix, whose encoding carries a null indicator only for a nullable column.
         */
        for (const auto &c : e.child_columns)
        {
            const int fi = fk_field_index(table_arg, c);
            e.child_nullable.push_back(
                (uint8)(fi >= 0 && TDB_FIELD_IS_NULLABLE(table_arg->field[fi])));
        }

        /* build the parent cf name the same way create() builds the child one */
        const std::string parent_path = "./" + e.ref_db + "/" + e.ref_table;
        e.parent_cf = path_to_cf_name(parent_path.c_str());

        const int cidx = fk_find_covering_index(table_arg, e.child_columns);
        if (cidx < 0)
        {
            my_printf_error(ER_CANT_CREATE_TABLE,
                            "TidesDB requires an index on the foreign key columns of %s", MYF(0),
                            e.name.c_str());
            return HA_ERR_UNSUPPORTED;
        }
        e.child_index_name = TDB_KEY_NAME(&table_arg->key_info[cidx]);

        /* The parent-side scan rebuilds the child index prefix with a plain forward sort key, so a
           descending foreign-key column would not line up.  Reject it clearly rather than enforce
           it incorrectly. */
        for (uint kp = 0; kp < e.child_columns.size(); kp++)
            if (table_arg->key_info[cidx].key_part[kp].key_part_flag & HA_REVERSE_SORT)
            {
                my_printf_error(
                    ER_CANT_CREATE_TABLE,
                    "TidesDB does not support a descending column in the foreign key %s", MYF(0),
                    e.name.c_str());
                return HA_ERR_UNSUPPORTED;
            }

        /* Which parent key the constraint references decides where the child probes: the parent's
           data family for a primary key, its index family for a unique key.  The server resolved
           that already and named the key on the share.  A parent key it could not resolve leaves
           the name empty, and the primary key is assumed, which is the historical behaviour. */
        e.parent_index_name = spec.parent_index_name;
        e.parent_is_pk = (e.parent_index_name.empty() ||
                          native_strcasecmp(e.parent_index_name.c_str(), TDB_PRIMARY_KEY_NAME) == 0)
                             ? 1
                             : 0;

        /* A unique key with a nullable column stores an indicator byte in front of it, so the probe
           has to write one too.  The parent recorded which of its parts carry one when it built the
           index family, and that is what gets read here -- the convention the stored keys were
           actually written under.  A primary key needs no lookup: its columns cannot be nullable,
           so the empty vector left here is already correct for it. */
        if (!e.parent_is_pk)
            (void)tdb_fk_parent_key_shape(e.parent_cf, e.parent_index_name, e.parent_nullable);

        std::string val;
        fk_serialize(e, val);
        records.emplace_back(fk_child_key(e.child_cf, e.name), val);
        records.emplace_back(fk_parent_key(e.parent_cf, e.child_cf, e.name), val);
    }

    if (records.empty()) return 0;

    tidesdb_column_family_t *cf = fk_catalog_cf();
    if (!cf) return HA_ERR_GENERIC;

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return HA_ERR_GENERIC;
    for (auto &r : records)
    {
        if (tidesdb_txn_put(txn, cf, (const uint8_t *)r.first.data(), r.first.size(),
                            (const uint8_t *)r.second.data(), r.second.size(),
                            TIDESDB_TTL_NONE) != TDB_SUCCESS)
        {
            tidesdb_txn_rollback(txn);
            tidesdb_txn_free(txn);
            return HA_ERR_GENERIC;
        }
    }
    const int rc = tidesdb_txn_commit(txn);
    tidesdb_txn_free(txn);
    return rc == TDB_SUCCESS ? 0 : HA_ERR_GENERIC;
}

/* ---- drop-time purge -------------------------------------------------- */

int ha_tidesdb::fk_purge_catalog(const char *child_cf_name)
{
    tidesdb_column_family_t *cf = tidesdb_get_column_family(tdb_global, FK_CATALOG_CF);
    if (!cf) return 0;
    std::string cfn(child_cf_name);

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return 0;

    /* Collect every record naming this cf on either side, then delete them.  A
       full scan is fine because the catalog holds one small record per
       constraint and this only runs on DROP TABLE. */
    std::vector<std::string> to_delete;
    tidesdb_iter_t *it = NULL;
    if (tidesdb_iter_new(txn, cf, &it) == TDB_SUCCESS && it)
    {
        const uint8_t lo0 = 0;
        tidesdb_iter_seek(it, &lo0, 1);
        while (tidesdb_iter_valid(it))
        {
            uint8_t *k = NULL, *v = NULL;
            size_t ks = 0, vs = 0;
            if (tidesdb_iter_key(it, &k, &ks) == TDB_SUCCESS &&
                tidesdb_iter_value(it, &v, &vs) == TDB_SUCCESS)
            {
                fk_catalog_entry e;
                if (fk_deserialize(v, vs, e) && (e.child_cf == cfn || e.parent_cf == cfn))
                    to_delete.emplace_back((const char *)k, ks);
                tidesdb_free(k);
                tidesdb_free(v);
            }
            tidesdb_iter_next(it);
        }
        tidesdb_iter_free(it);
    }

    for (auto &k : to_delete) tidesdb_txn_delete(txn, cf, (const uint8_t *)k.data(), k.size());

    if (tidesdb_txn_commit(txn) != TDB_SUCCESS) tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
    return 0;
}

/* ---- open-time load into the share ------------------------------------ */

void ha_tidesdb::fk_load()
{
    if (!share || share->fk_loaded) return;
    share->fk_loaded = true;

    tidesdb_column_family_t *cf = tidesdb_get_column_family(tdb_global, FK_CATALOG_CF);
    if (!cf) return;

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return;

    const std::string &self = share->cf_name;

    /* The catalog is small, one record per constraint per side, so we walk it
       whole and route each entry by the column families it names.  A record
       whose child cf is this table feeds fk_child, one whose parent cf is this
       table feeds fk_parent, and a self-referencing constraint feeds both. */
    tidesdb_iter_t *it = NULL;
    if (tidesdb_iter_new(txn, cf, &it) == TDB_SUCCESS && it)
    {
        /* a fresh iterator is unpositioned, seek to the low end to start */
        const uint8_t lo0 = 0;
        tidesdb_iter_seek(it, &lo0, 1);
        while (tidesdb_iter_valid(it))
        {
            uint8_t *k = NULL, *v = NULL;
            size_t ks = 0, vs = 0;
            if (tidesdb_iter_key(it, &k, &ks) == TDB_SUCCESS &&
                tidesdb_iter_value(it, &v, &vs) == TDB_SUCCESS)
            {
                /* only the child-side record is materialized, so each constraint
                   is considered once even though it is stored on both sides */
                fk_catalog_entry e;
                bool readable = false;
                if (ks > 0 && k[0] == FK_REC_CHILD)
                {
                    readable = fk_deserialize(v, vs, e);
                    /* A record this build cannot read would otherwise be passed over, and the
                       constraint would stop being enforced with nothing said about it. */
                    if (!readable)
                        sql_print_error(
                            "[TIDESDB] a foreign-key catalog record is in a format this "
                            "build does not read; the constraint it describes is NOT "
                            "enforced on '%s'",
                            self.c_str());
                }
                if (readable)
                {
                    if (e.child_cf == self)
                    {
                        tdb_fk_def d;
                        d.name = e.name;
                        d.child_cf = e.child_cf;
                        d.child_db = e.child_db;
                        d.child_table = e.child_table;
                        d.ref_db = e.ref_db;
                        d.ref_table = e.ref_table;
                        d.parent_cf = e.parent_cf;
                        d.child_index_name = e.child_index_name;
                        d.parent_index_name = e.parent_index_name;
                        d.parent_nullable = e.parent_nullable;
                        d.ref_column_names = e.ref_columns;
                        d.child_nullable = e.child_nullable;
                        d.on_delete = e.on_delete;
                        d.on_update = e.on_update;
                        for (auto &c : e.child_columns)
                        {
                            int fi = fk_field_index(table, c);
                            if (fi >= 0) d.child_fields.push_back((uint16)fi);
                        }
                        d.child_key_no = fk_find_covering_index(table, e.child_columns);
                        d.parent_is_pk = (e.parent_is_pk != 0);
                        share->fk_child.push_back(std::move(d));
                    }
                    if (e.parent_cf == self)
                    {
                        tdb_fk_def d;
                        d.name = e.name;
                        d.child_cf = e.child_cf;
                        d.child_db = e.child_db;
                        d.child_table = e.child_table;
                        d.ref_db = e.ref_db;
                        d.ref_table = e.ref_table;
                        d.parent_cf = e.parent_cf;
                        d.child_index_name = e.child_index_name;
                        d.ref_column_names = e.ref_columns;
                        d.child_nullable = e.child_nullable;
                        d.on_delete = e.on_delete;
                        d.on_update = e.on_update;
                        for (auto &c : e.ref_columns)
                        {
                            int fi = fk_field_index(table, c);
                            if (fi >= 0) d.parent_fields.push_back((uint16)fi);
                        }
                        d.parent_key_no = fk_find_covering_index(table, e.ref_columns);
                        d.parent_is_pk = (table->s->primary_key != MAX_KEY &&
                                          d.parent_key_no == (int)table->s->primary_key);
                        share->fk_parent.push_back(std::move(d));
                    }
                }
            }
            if (k) tidesdb_free(k);
            if (v) tidesdb_free(v);
            tidesdb_iter_next(it);
        }
        tidesdb_iter_free(it);
    }

    tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
}

/* Describe one constraint for the server.  The referencing (foreign) table and
   the referenced table swap roles depending on which side is asking, since the
   same constraint is reported from the child by get_foreign_key_list and from
   the parent by get_parent_foreign_key_list.  Getting the foreign table right on
   the parent side is what lets prelocking open the child for a cascade. */

/* ---- enforcement ------------------------------------------------------ */

static bool fk_checks_off(THD *thd)
{
    return thd && thd_test_options(thd, OPTION_NO_FOREIGN_KEY_CHECKS);
}

int ha_tidesdb::fk_check_child(const uchar *new_row)
{
    if (!share || share->fk_child.empty()) return 0;
    /* A cascade writing this row already holds a valid parent value, so skip the
       existence probe, which would otherwise race the parent's own row update. */
    if (fk_in_cascade_) return 0;
    THD *thd = cached_thd_ ? cached_thd_ : ha_thd();
    if (fk_checks_off(thd)) return 0;
    tidesdb_txn_t *txn = stmt_txn;
    if (!txn) return 0;

    for (const auto &d : share->fk_child)
    {
        if (d.child_key_no < 0) continue;
        KEY *ki = &table->key_info[d.child_key_no];
        uint nparts = (uint)d.child_fields.size();

        /* MATCH SIMPLE, skip the check when any referencing column is null */
        bool any_null = false;
        for (uint16 fi : d.child_fields)
            if (table->field[fi]->is_null_in_record(new_row))
            {
                any_null = true;
                break;
            }
        if (any_null) continue;

        /* Encode the way the parent encoded its key, which carries an indicator byte for each
           column the parent declared nullable and none for the rest.  The child's own nullability
           has no say here -- the two tables are free to differ -- and the indicator is always
           NOT_NULL because the MATCH SIMPLE check above skipped the row if any value were null. */
        uchar comp[MAX_KEY_LENGTH];
        uint comp_len = make_comparable_key(ki, new_row, nparts, comp, &d.parent_nullable);

        bool present = false;
        if (d.parent_is_pk)
        {
            /* The parent stores its rows in the data family keyed by the data key
               of the comparable primary key, so a point probe answers existence. */
            tidesdb_column_family_t *pcf =
                tidesdb_get_column_family(tdb_global, d.parent_cf.c_str());
            if (!pcf) continue;
            uchar dk[DATA_KEY_BUF_LEN];
            uint dk_len = build_data_key(comp, comp_len, dk);
            int rc = tidesdb_txn_contains(txn, pcf, dk, dk_len);
            if (rc == TDB_SUCCESS)
                present = true;
            else if (rc != TDB_ERR_NOT_FOUND)
                return tdb_rc_to_ha(rc, "fk_check_child");
        }
        else
        {
            /* The parent stores a unique index entry keyed by the comparable
               index columns followed by its primary key, so we prefix-scan the
               parent index family for an entry that begins with the value. */
            std::string picf = d.parent_cf + CF_INDEX_INFIX + d.parent_index_name;
            tidesdb_column_family_t *pcf = tidesdb_get_column_family(tdb_global, picf.c_str());
            if (!pcf) continue;
            std::string hi((const char *)comp, comp_len);
            hi.push_back((char)0xff);
            tidesdb_iter_t *it = NULL;
            if (tidesdb_iter_new_range(txn, pcf, comp, comp_len, (const uint8_t *)hi.data(),
                                       hi.size(), &it) == TDB_SUCCESS &&
                it)
            {
                tidesdb_iter_seek(it, comp, comp_len);
                while (tidesdb_iter_valid(it))
                {
                    uint8_t *k = NULL;
                    size_t ks = 0;
                    if (tidesdb_iter_key(it, &k, &ks) == TDB_SUCCESS)
                    {
                        if (ks >= comp_len && memcmp(k, comp, comp_len) == 0) present = true;
                        tidesdb_free(k);
                    }
                    if (present) break;
                    tidesdb_iter_next(it);
                }
                tidesdb_iter_free(it);
            }
        }

        if (!present)
        {
            my_error(ER_NO_REFERENCED_ROW_2, MYF(0), d.name.c_str());
            return HA_ERR_NO_REFERENCED_ROW;
        }
    }
    return 0;
}

/* Encode the referenced values from a parent row the way the child index stored
   them, so a range scan of that index finds the referencing children.  The child
   index carries a null indicator only for a nullable child column, so we prepend
   one exactly where child_nullable says to, then the value bytes.  Types match by
   the foreign-key definition, so the parent field's sort_string yields the same
   value bytes the child wrote.  Descending and binary-varstring key parts are not
   handled here yet, matching this increment's primary-key referenced scope. */
static uint fk_encode_child_prefix(TABLE *table, KEY *key, uint nparts, const uchar *row,
                                   const std::vector<uint8> &child_nullable, uchar *out)
{
    uint pos = 0;
    my_ptrdiff_t ptrdiff = (my_ptrdiff_t)(row - table->record[0]);
    for (uint p = 0; p < nparts && p < key->user_defined_key_parts; p++)
    {
        KEY_PART_INFO *kp = &key->key_part[p];
        Field *field = kp->field;
        if (p < child_nullable.size() && child_nullable[p]) out[pos++] = SORT_KEY_NOT_NULL;
        field->move_field_offset(ptrdiff);
        TDB_FIELD_SORT_STRING(field, out + pos, kp->length);
        field->move_field_offset(-ptrdiff);
        pos += kp->length;
    }
    return pos;
}

/* Does any child row reference the key built from old_row for constraint d?
   Returns 1 referenced, 0 none, or a negative handler error. */
int ha_tidesdb::fk_child_ref_exists(const tdb_fk_def &d, const uchar *old_row)
{
    tidesdb_txn_t *txn = stmt_txn;
    if (!txn || d.parent_key_no < 0) return 0;
    KEY *ki = &table->key_info[d.parent_key_no];
    uint nparts = (uint)d.parent_fields.size();

    uchar comp[MAX_KEY_LENGTH];
    uint comp_len = fk_encode_child_prefix(table, ki, nparts, old_row, d.child_nullable, comp);

    std::string idx_cf_name = d.child_cf;
    idx_cf_name += CF_INDEX_INFIX;
    idx_cf_name += d.child_index_name;
    tidesdb_column_family_t *icf = tidesdb_get_column_family(tdb_global, idx_cf_name.c_str());
    if (!icf) return 0;

    std::string hi((const char *)comp, comp_len);
    hi.push_back((char)0xff);
    tidesdb_iter_t *it = NULL;
    if (tidesdb_iter_new_range(txn, icf, comp, comp_len, (const uint8_t *)hi.data(), hi.size(),
                               &it) != TDB_SUCCESS ||
        !it)
        return 0;
    tidesdb_iter_seek(it, comp, comp_len);

    bool referenced = false;
    while (tidesdb_iter_valid(it))
    {
        uint8_t *k = NULL;
        size_t ks = 0;
        if (tidesdb_iter_key(it, &k, &ks) == TDB_SUCCESS)
        {
            if (ks >= comp_len && memcmp(k, comp, comp_len) == 0) referenced = true;
            tidesdb_free(k);
        }
        if (referenced) break;
        tidesdb_iter_next(it);
    }
    tidesdb_iter_free(it);
    return referenced ? 1 : 0;
}

/* Find an already-open table by database and name in this statement's global
   table list, reached from the handler's own table.  Foreign-key prelocking
   opens the referencing children for a parent DML, appending them after the
   parent, so a forward walk from the parent finds them.  Returns NULL when the
   child is not open, and the caller then falls back to the restrict behaviour. */
static TABLE *fk_find_open_table(TABLE *self, const std::string &db, const std::string &tbl)
{
    if (!self || !self->pos_in_table_list) return NULL;
    for (TDB_TABLE_REF *tl = self->pos_in_table_list; tl; tl = tl->next_global)
    {
        TABLE *t = tl->table;
        if (!t || !t->s) continue;
        if (t->s->db.str && t->s->table_name.str && db.size() == t->s->db.length &&
            tbl.size() == t->s->table_name.length &&
            memcmp(db.data(), t->s->db.str, db.size()) == 0 &&
            memcmp(tbl.data(), t->s->table_name.str, tbl.size()) == 0)
            return t;
    }
    return NULL;
}

/* walk the child index for every row referencing the parent value now in the child's record[0],
   recording each one's position.  the positions are taken first and acted on afterwards so a
   delete or update cannot disturb the walk mid-scan.
   @return 0, or a handler error that is neither end-of-file nor key-not-found */
int ha_tidesdb::fk_collect_child_refs(TABLE *ct, int cidx, uint nparts, const uchar *keybuf,
                                      uint key_len, std::vector<std::string> &refs)
{
    int rc = ct->file->ha_index_init((uint)cidx, true);
    if (rc == 0)
    {
        rc = ct->file->ha_index_read_map(ct->record[0], keybuf, make_prev_keypart_map(nparts),
                                         HA_READ_KEY_EXACT);
        while (rc == 0)
        {
            ct->file->position(ct->record[0]);
            refs.emplace_back((const char *)ct->file->ref, ct->file->ref_length);
            rc = ct->file->ha_index_next_same(ct->record[0], keybuf, key_len);
        }
        ct->file->ha_index_end();
    }
    if (rc == HA_ERR_END_OF_FILE || rc == HA_ERR_KEY_NOT_FOUND) return 0;
    return rc;
}

/* apply the constraint's referential action to each collected child row -- remove it for
   ON DELETE CASCADE, otherwise rewrite its key columns to null or to the parent's new values.
   @return 0, or the first handler error, which stops the cascade */
int ha_tidesdb::fk_apply_cascade(const tdb_fk_def &d, TABLE *ct, KEY *ckey, uint nparts,
                                 const uchar *new_row, bool is_update, bool set_null,
                                 std::vector<std::string> &refs)
{
    int result = 0;
    /* Suppress the child's parent-existence check while we rewrite its rows, and
       restore it after.  The cascade only ever writes a valid parent value. */
    ha_tidesdb *child_ha = (ct->file->ht == ht) ? static_cast<ha_tidesdb *>(ct->file) : NULL;
    if (child_ha) child_ha->fk_in_cascade_ = true;
    if (!refs.empty() && ct->file->ha_rnd_init(false) == 0)
    {
        my_ptrdiff_t npd = is_update ? (my_ptrdiff_t)(new_row - table->record[0]) : 0;
        for (auto &r : refs)
        {
            if (ct->file->ha_rnd_pos(ct->record[0], (uchar *)r.data()) != 0) continue;

            if (!is_update && !set_null)
            {
                /* ON DELETE CASCADE removes the child, which recurses through the
                   child handler into its own foreign keys and indexes. */
                result = TDB_CASCADE_DELETE_ROW(child_ha, ct->file, ct->record[0]);
            }
            else
            {
                store_record(ct, record[1]);
                for (uint i = 0; i < nparts; i++)
                {
                    Field *cf = ckey->key_part[i].field;
                    if (set_null)
                        cf->set_null();
                    else
                    {
                        Field *pf = table->field[d.parent_fields[i]];
                        pf->move_field_offset(npd);
                        cf->set_notnull();
                        TDB_FIELD_CONV(cf, pf);
                        pf->move_field_offset(-npd);
                    }
                }
                result = TDB_CASCADE_UPDATE_ROW(child_ha, ct->file, ct->record[1], ct->record[0]);
            }
            if (result) break;
        }
        ct->file->ha_rnd_end();
    }
    if (child_ha) child_ha->fk_in_cascade_ = false;

    return result;
}

int ha_tidesdb::fk_cascade_children(const tdb_fk_def &d, const uchar *old_row, const uchar *new_row)
{
    const bool is_update = (new_row != NULL);
    const TDB_FK_OPTION act = (TDB_FK_OPTION)(is_update ? d.on_update : d.on_delete);
    const bool set_null = (act == FK_OPTION_SET_NULL);

    TABLE *ct = fk_find_open_table(table, d.child_db, d.child_table);

    /* Reaching the child is not the same as being allowed to write it.  Prelocking can leave a
       table open and visible here without the server having prepared it for this statement's
       writes, and cascading into one in that state corrupts the statement's row log rather than
       failing it.  Treat anything short of a fully prepared, write-locked child as unreachable. */
    if (ct && !TDB_TABLE_WRITABLE_IN_STMT(ct)) ct = NULL;

    if (!ct || !ct->file)
    {
        /* Cannot reach the child to cascade, so fall back to restrict rather than
           risk leaving an orphan.  This should not happen while prelocking is in
           effect, but it keeps the store consistent if it ever does. */
        int r = fk_child_ref_exists(d, old_row);
        if (r < 0) return -r;
        if (r > 0)
        {
            my_error(ER_ROW_IS_REFERENCED_2, MYF(0), d.name.c_str());
            return HA_ERR_ROW_IS_REFERENCED;
        }
        return 0;
    }

    /* Locate the child index that carries the foreign-key columns. */
    int cidx = -1;
    for (uint i = 0; i < ct->s->keys; i++)
        if (TDB_KEY_NAME(&ct->key_info[i]) && d.child_index_name == TDB_KEY_NAME(&ct->key_info[i]))
        {
            cidx = (int)i;
            break;
        }
    if (cidx < 0) return 0;
    KEY *ckey = &ct->key_info[cidx];
    uint nparts = (uint)d.parent_fields.size();
    if (nparts > ckey->user_defined_key_parts) nparts = ckey->user_defined_key_parts;

    /* We read and write every child column during the cascade, so widen the
       child's column maps for the duration and restore them after. */
    TDB_COLUMN_MAP_SAVED old_r = TDB_USE_ALL_COLUMNS(ct, read_set);
    TDB_COLUMN_MAP_SAVED old_w = TDB_USE_ALL_COLUMNS(ct, write_set);

    /* Set the child key columns from the parent's old referenced values so the
       index read lands on the referencing rows.  Types match by the constraint,
       so a raw copy of the field bytes reproduces the stored value. */
    my_ptrdiff_t opd = (my_ptrdiff_t)(old_row - table->record[0]);
    uint key_len = 0;
    for (uint i = 0; i < nparts; i++)
    {
        Field *pf = table->field[d.parent_fields[i]];
        Field *cf = ckey->key_part[i].field;
        pf->move_field_offset(opd);
        cf->set_notnull();
        TDB_FIELD_CONV(cf, pf);
        pf->move_field_offset(-opd);
        key_len += ckey->key_part[i].store_length;
    }

    uchar keybuf[MAX_KEY_LENGTH];
    key_copy(keybuf, ct->record[0], ckey, key_len);

    /* Collect the referencing children's positions first, then act on them, so a delete or update
       does not disturb the index walk mid-scan. */
    std::vector<std::string> refs;

    int rc = fk_collect_child_refs(ct, cidx, nparts, keybuf, key_len, refs);
    if (rc != 0)
    {
        TDB_RESTORE_COLUMN_MAP(ct, read_set, old_r);
        TDB_RESTORE_COLUMN_MAP(ct, write_set, old_w);
        return rc;
    }

    int result = 0;
    result = fk_apply_cascade(d, ct, ckey, nparts, new_row, is_update, set_null, refs);

    TDB_RESTORE_COLUMN_MAP(ct, read_set, old_r);
    TDB_RESTORE_COLUMN_MAP(ct, write_set, old_w);
    return result;
}

int ha_tidesdb::fk_enforce_parent_delete(const uchar *old_row)
{
    if (!share || share->fk_parent.empty()) return 0;
    THD *thd = cached_thd_ ? cached_thd_ : ha_thd();
    if (fk_checks_off(thd)) return 0;
    if (!stmt_txn) return 0;

    for (const auto &d : share->fk_parent)
    {
        if (d.parent_key_no < 0) continue;
        TDB_FK_OPTION act = (TDB_FK_OPTION)d.on_delete;
        if (act == FK_OPTION_CASCADE || act == FK_OPTION_SET_NULL)
        {
            int rc = fk_cascade_children(d, old_row, NULL);
            if (rc) return rc;
        }
        else
        {
            int r = fk_child_ref_exists(d, old_row);
            if (r < 0) return -r;
            if (r > 0)
            {
                my_error(ER_ROW_IS_REFERENCED_2, MYF(0), d.name.c_str());
                return HA_ERR_ROW_IS_REFERENCED;
            }
        }
    }
    return 0;
}

int ha_tidesdb::fk_enforce_parent_update(const uchar *old_row, const uchar *new_row)
{
    if (!share || share->fk_parent.empty()) return 0;
    THD *thd = cached_thd_ ? cached_thd_ : ha_thd();
    if (fk_checks_off(thd)) return 0;
    if (!stmt_txn) return 0;

    /* Only the constraints whose referenced columns actually changed can be
       affected by an update, so we compare the old and new referenced prefixes
       and skip the constraint when they match. */
    for (const auto &d : share->fk_parent)
    {
        if (d.parent_key_no < 0) continue;
        KEY *ki = &table->key_info[d.parent_key_no];
        uint nparts = (uint)d.parent_fields.size();

        uchar oldp[MAX_KEY_LENGTH], newp[MAX_KEY_LENGTH];
        uint ol = fk_encode_child_prefix(table, ki, nparts, old_row, d.child_nullable, oldp);
        uint nl = fk_encode_child_prefix(table, ki, nparts, new_row, d.child_nullable, newp);
        if (ol == nl && memcmp(oldp, newp, ol) == 0) continue;

        TDB_FK_OPTION act = (TDB_FK_OPTION)d.on_update;
        if (act == FK_OPTION_CASCADE || act == FK_OPTION_SET_NULL)
        {
            int rc = fk_cascade_children(d, old_row, new_row);
            if (rc) return rc;
        }
        else
        {
            int r = fk_child_ref_exists(d, old_row);
            if (r < 0) return -r;
            if (r > 0)
            {
                my_error(ER_ROW_IS_REFERENCED_2, MYF(0), d.name.c_str());
                return HA_ERR_ROW_IS_REFERENCED;
            }
        }
    }
    return 0;
}
