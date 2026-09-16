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

/* foreign-key extraction for a server that keeps table definitions in a data dictionary.
 *
 * MySQL 8.0 made foreign keys dictionary objects and stopped handing the parser's key list to the
 * engine, so both halves of this file read the dictionary instead: the constraints come off the
 * dd::Table the server passes into create(), and the parent's candidate key is read by acquiring
 * the parent's dd::Table through the session's dictionary client.
 *
 * the engine still persists and enforces its own copy.  the dictionary is authoritative for what
 * SHOW CREATE TABLE and information_schema report -- those handler methods no longer exist here --
 * while the engine's catalog is what the row paths enforce against, which keeps enforcement
 * identical across servers.
 *
 * the parser counterpart is ha_tidesdb_fk_source_parser.cc; CMakeLists.txt compiles exactly one. */

#include "src/handler/ha_tidesdb_fk_source.h"

#include <cstring>

#include "dd/cache/dictionary_client.h"
#include "dd/dd_table.h"
#include "dd/types/column.h"
#include "dd/types/foreign_key.h"
#include "dd/types/foreign_key_element.h"
#include "dd/types/index.h"
#include "dd/types/index_element.h"
#include "dd/types/table.h"
#include "sql_class.h"

/* the referential actions, as the engine stores them.  the dictionary numbers its rules from one
   and orders them differently from the parser enum the catalog format was defined against, so the
   mapping is explicit rather than a cast -- getting it wrong would silently turn a RESTRICT into a
   CASCADE on the next delete. */
static uint8 tdb_rule_to_fk_option(dd::Foreign_key::enum_rule rule)
{
    switch (rule)
    {
        case dd::Foreign_key::RULE_RESTRICT:
            return (uint8)FK_OPTION_RESTRICT;
        case dd::Foreign_key::RULE_CASCADE:
            return (uint8)FK_OPTION_CASCADE;
        case dd::Foreign_key::RULE_SET_NULL:
            return (uint8)FK_OPTION_SET_NULL;
        case dd::Foreign_key::RULE_SET_DEFAULT:
            return (uint8)FK_OPTION_DEFAULT;
        case dd::Foreign_key::RULE_NO_ACTION:
            return (uint8)FK_OPTION_NO_ACTION;
    }
    return (uint8)FK_OPTION_UNDEF;
}

bool tdb_fk_extract_specs(THD *, TABLE *table_arg, HA_CREATE_INFO *, const void *dd_table_def,
                          std::vector<tdb_fk_spec> &out)
{
    out.clear();

    /* A table created without any foreign key still reaches here, and so does one created on a
       path that carries no dictionary definition at all; neither is an error. */
    const dd::Table *dd_table = static_cast<const dd::Table *>(dd_table_def);
    if (!dd_table || !table_arg) return true;

    for (const dd::Foreign_key *fk : dd_table->foreign_keys())
    {
        if (!fk) continue;

        tdb_fk_spec spec;
        spec.name.assign(fk->name().c_str(), fk->name().length());
        spec.on_delete = tdb_rule_to_fk_option(fk->delete_rule());
        spec.on_update = tdb_rule_to_fk_option(fk->update_rule());

        spec.ref_db.assign(fk->referenced_table_schema_name().c_str(),
                           fk->referenced_table_schema_name().length());
        spec.ref_table.assign(fk->referenced_table_name().c_str(),
                              fk->referenced_table_name().length());

        /* Elements are the column pairs, and the dictionary keeps them in the constraint's declared
           order, which is the order the engine's catalog format and its key encoding both assume.
         */
        for (const dd::Foreign_key_element *el : fk->elements())
        {
            if (!el) continue;
            spec.child_columns.emplace_back(el->column().name().c_str(),
                                            el->column().name().length());
            spec.ref_columns.emplace_back(el->referenced_column_name().c_str(),
                                          el->referenced_column_name().length());
        }

        out.push_back(std::move(spec));
    }
    return true;
}

bool tdb_fk_resolve_parent_index(THD *thd, const std::string &ref_db, const std::string &ref_table,
                                 const std::vector<std::string> &ref_cols, bool &out_is_pk,
                                 std::string &out_index_name, bool &out_has_nullable)
{
    if (!thd || ref_db.empty() || ref_table.empty() || ref_cols.empty()) return false;

    /* The releaser returns whatever this lookup pins to the dictionary cache when it goes out of
       scope, so the parent definition is not held past the resolve. */
    dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

    const dd::Table *parent = nullptr;
    /* the dictionary uses its own string type, which does not implicitly convert */
    const dd::String_type dd_schema(ref_db.c_str(), ref_db.size());
    const dd::String_type dd_table(ref_table.c_str(), ref_table.size());
    if (thd->dd_client()->acquire(dd_schema, dd_table, &parent) || parent == nullptr) return false;

    const dd::Index *best = nullptr;
    for (const dd::Index *idx : parent->indexes())
    {
        if (!idx) continue;

        /* Only a primary or unique key identifies at most one parent row, which is what the child's
           existence probe depends on. */
        const bool is_pk = (idx->type() == dd::Index::IT_PRIMARY);
        if (!is_pk && idx->type() != dd::Index::IT_UNIQUE) continue;

        /* An index carries hidden trailing elements the dictionary adds for its own bookkeeping, so
           the comparison walks the user-visible prefix rather than the whole element list. */
        size_t matched = 0;
        bool match = true;
        for (const dd::Index_element *el : idx->elements())
        {
            if (!el || el->is_hidden()) continue;
            if (matched >= ref_cols.size())
            {
                /* the index is wider than the constraint, so it does not identify a single row by
                   the referenced columns alone */
                match = false;
                break;
            }
            const dd::String_type &cname = el->column().name();
            if (strcasecmp(cname.c_str(), ref_cols[matched].c_str()) != 0)
            {
                match = false;
                break;
            }
            matched++;
        }
        if (!match || matched != ref_cols.size()) continue;

        /* Prefer the primary key, matching what the parser-side resolver does, so both servers
           resolve the same constraint to the same parent key. */
        if (best == nullptr || is_pk) best = idx;
        if (is_pk) break;
    }

    if (best == nullptr) return false;

    out_is_pk = (best->type() == dd::Index::IT_PRIMARY);
    out_index_name.assign(best->name().c_str(), best->name().length());

    out_has_nullable = false;
    for (const dd::Index_element *el : best->elements())
    {
        if (!el || el->is_hidden()) continue;
        if (el->column().is_nullable()) out_has_nullable = true;
    }
    return true;
}
