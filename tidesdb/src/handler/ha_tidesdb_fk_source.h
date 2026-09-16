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

#ifndef HA_TIDESDB_FK_SOURCE_H
#define HA_TIDESDB_FK_SOURCE_H

#include <string>
#include <vector>

#include "ha_tidesdb.h"

/* where a foreign key's definition comes from at CREATE and ALTER time, and where the parent's
 * candidate key comes from when the constraint is resolved.
 *
 * these are the only two places the engine's foreign-key handling depends on how a server keeps
 * table definitions, and the two servers keep them in entirely different shapes: one hands the
 * engine the parser's own key list and reads the parent's definition off disk, while MySQL holds
 * both in its data dictionary.  everything the engine does afterwards -- naming the constraint,
 * finding a covering index, serialising the catalog entry, enforcing on insert, update and delete
 * -- is identical, and lives in ha_tidesdb_fk.cc.
 *
 * so only extraction is split.  the two implementations are ha_tidesdb_fk_source_parser.cc and
 * ha_tidesdb_fk_source_dd.cc, and CMakeLists.txt compiles exactly one, which is why neither
 * carries a preprocessor branch.
 */

/**
 * tdb_fk_spec
 * one foreign key exactly as the server declared it, before the engine resolves anything
 * @param name the constraint name, empty when the statement did not give one and the caller should
 *             synthesise it
 * @param child_columns the referencing columns, in declared order
 * @param ref_db the referenced database, empty when the statement omitted it and the child's own
 *               database applies
 * @param ref_table the referenced table
 * @param ref_columns the referenced columns, in declared order and matching child_columns
 * @param on_delete the delete action, as an enum_fk_option / fk_option value
 * @param on_update the update action, same encoding
 */
struct tdb_fk_spec
{
    std::string name;
    std::vector<std::string> child_columns;
    std::string ref_db;
    std::string ref_table;
    std::vector<std::string> ref_columns;
    uint8 on_delete;
    uint8 on_update;
};

/**
 * tdb_fk_extract_specs
 * collect the foreign keys a CREATE or ALTER declares, in the order the statement gave them
 * @param thd the session
 * @param table_arg the table being created, for its database and table name
 * @param create_info the server's create information
 * @param dd_table_def the data dictionary's definition of the table where the server keeps one,
 *                     and ignored where it does not; passed as void so this header stays free of
 *                     dictionary types
 * @param out out -- receives one entry per foreign key, cleared first
 * @return true on success, including the common case of a table with no foreign keys at all
 */
bool tdb_fk_extract_specs(THD *thd, TABLE *table_arg, HA_CREATE_INFO *create_info,
                          const void *dd_table_def, std::vector<tdb_fk_spec> &out);

/**
 * tdb_fk_resolve_parent_index
 * find the parent's candidate key a constraint references, so the child's existence probe targets
 * the parent's data family for a primary key or its index family for a unique key
 * @param thd the session
 * @param ref_db the referenced database
 * @param ref_table the referenced table
 * @param ref_cols the referenced columns
 * @param out_is_pk out -- whether the matched key is the parent's primary key
 * @param out_index_name out -- the matched key's name
 * @param out_has_nullable out -- whether any matched column is nullable, which decides whether the
 *                         rebuilt key prefix carries a null indicator
 * @return true when a primary or unique key covering exactly those columns was found; false leaves
 *         the caller to assume the primary key, which is the historical behaviour when a parent
 *         definition cannot be read
 */
bool tdb_fk_resolve_parent_index(THD *thd, const std::string &ref_db, const std::string &ref_table,
                                 const std::vector<std::string> &ref_cols, bool &out_is_pk,
                                 std::string &out_index_name, bool &out_has_nullable);

#endif /* HA_TIDESDB_FK_SOURCE_H */
