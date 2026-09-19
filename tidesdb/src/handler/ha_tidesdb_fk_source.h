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
 * these are the only two places the engine's foreign-key handling depends on how the server keeps
 * table definitions.  MySQL holds both in its data dictionary, so both are read from there, in
 * ha_tidesdb_fk_source_dd.cc.  everything the engine does afterwards -- naming the constraint,
 * finding a covering index, serialising the catalog entry, enforcing on insert, update and delete
 * -- is independent of that and lives in ha_tidesdb_fk.cc.
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
 * @param parent_index_name the parent key the server resolved the constraint against, empty when
 *                          the parent key is neither primary nor unique
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
    std::string parent_index_name;
    uint8 on_delete;
    uint8 on_update;
};

/**
 * tdb_fk_extract_specs
 * collect the foreign keys a CREATE or ALTER declares, in the order the statement gave them
 * @param thd the session
 * @param table_arg the table being created, for its database and table name
 * @param create_info the server's create information
 * @param out out -- receives one entry per foreign key, cleared first
 * @return true on success, including the common case of a table with no foreign keys at all
 *
 * The constraints are read off the table's own share, which the server fills from the dictionary
 * before it hands the table to either create() or open().
 */
bool tdb_fk_extract_specs(THD *thd, TABLE *table_arg, HA_CREATE_INFO *create_info,
                          std::vector<tdb_fk_spec> &out);

/**
 * tdb_fk_parent_key_shape
 * how the parent encoded the key this constraint references, so the child's probe rebuilds the
 * same bytes
 * @param parent_cf the referenced table's data column family
 * @param parent_index_name the referenced key, as the server resolved it
 * @param out out -- one entry per key part, non-zero where that part carries a null indicator;
 *            cleared when the shape is not known
 * @return true when a shape was read
 *
 * The answer comes from the parent's own index column family rather than from the server, because
 * what matters is the convention the stored keys were written under, not the table's shape now.
 * A primary key needs no lookup: its columns cannot be nullable, so an empty result is correct and
 * this is not called for one.
 */
bool tdb_fk_parent_key_shape(const std::string &parent_cf, const std::string &parent_index_name,
                             std::vector<uint8> &out);

#endif /* HA_TIDESDB_FK_SOURCE_H */
