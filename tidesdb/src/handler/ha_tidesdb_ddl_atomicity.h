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

#ifndef HA_TIDESDB_DDL_ATOMICITY_H
#define HA_TIDESDB_DDL_ATOMICITY_H

#include <string>

#include "ha_tidesdb.h"

/* when a table's column families are actually created and destroyed relative to the statement that
 * asked for it.
 *
 * a server that resolves DDL through a transactional data dictionary expects an engine to make its
 * own changes part of that outcome: a rolled-back CREATE must leave nothing behind, and a
 * rolled-back DROP must leave the table intact.  a server without that machinery expects the engine
 * to act immediately, because there is nothing to roll back against.
 *
 * the difference is confined to these two entry points.  everything else about a create or a drop
 * -- which families a table owns, what their names are, purging the foreign-key catalog -- is the
 * same either way and lives in ha_tidesdb_lifecycle.cc.
 *
 * the two implementations are ha_tidesdb_ddl_deferred.cc and ha_tidesdb_ddl_immediate.cc, and
 * CMakeLists.txt compiles exactly one, which is why neither carries a preprocessor branch.
 *
 * how the deferred implementation knows the outcome is worth stating, because it is the part that
 * makes atomicity real rather than assumed.  it does not keep a write-ahead log of its own.  the
 * post-DDL hook runs after the dictionary transaction has committed or rolled back, so it simply
 * asks the dictionary whether the table exists and acts on the answer:
 *
 *     dropped, and the dictionary no longer has the table   -> the drop committed, remove the families
 *     dropped, and the dictionary still has the table       -> the drop rolled back, keep them
 *     created, and the dictionary has the table             -> the create committed, keep them
 *     created, and the dictionary no longer has the table   -> the create rolled back, remove them
 *
 * the dictionary is the authority on whether the statement happened, which removes any question of
 * the engine's log and the server's decision disagreeing.
 */

/**
 * tdb_ddl_drop_table
 * remove the column families backing a table, or arrange for them to be removed once the
 * statement's dictionary changes are durable
 * @param thd the session, used to reach the dictionary and to hold pending work; may be null on
 *            paths with no session, in which case the removal is immediate
 * @param path the server table path, as handed to the handler
 * @return 0 on success, or a handler error when an immediate removal fails.  a deferred removal
 *         reports success here: it has not happened yet, and a later failure is reported to the
 *         error log because the statement has already committed by then
 */
int tdb_ddl_drop_table(THD *thd, const char *path);

/**
 * tdb_ddl_note_created
 * record that a table's column families now exist, so they can be removed again if the statement
 * that created them does not commit.  a no-op where DDL is not transactional
 * @param thd the session; may be null, in which case nothing is recorded
 * @param path the server table path, as handed to the handler
 */
void tdb_ddl_note_created(THD *thd, const char *path);

/**
 * tdb_ddl_register_hooks
 * wire whatever the server needs to resolve deferred DDL, and declare the capability if the engine
 * provides it.  called once from plugin init
 * @param hton the engine's handlerton
 */
void tdb_ddl_register_hooks(handlerton *hton);

/**
 * tdb_ddl_drop_table_cfs_now
 * remove a table's column families immediately, with no regard to any statement outcome.  this is
 * the shared body both implementations reach: one calls it inline, the other from the post-DDL
 * hook once the dictionary has confirmed the drop
 * @param path the server table path
 * @return 0 on success, or the library error that prevented it
 */
int tdb_ddl_drop_table_cfs_now(const char *path);

#endif /* HA_TIDESDB_DDL_ATOMICITY_H */
