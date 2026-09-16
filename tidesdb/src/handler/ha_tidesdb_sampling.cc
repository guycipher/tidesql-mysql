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

/* row sampling, and the cheap emptiness question.
 *
 * the server samples a table to build a column histogram.  it opens an ordinary scan, seeds a
 * generator from a per-histogram seed, and keeps a row when the next variate falls within the
 * sampling fraction.  its own version of that loop reads every row and discards the ones the dice
 * reject, which costs a full decode per row to keep a few of them -- and on a table whose values
 * are separated into the value log, a decode is a second read.
 *
 * the engine can decide before it decodes.  a scan here walks keys, and a key is cheap: the dice
 * are rolled against the key alone and the value is fetched only for a row that survives.  the
 * generator and the fraction are the server's own, drawn from in the same order, so the rows this
 * returns are the rows the server's loop would have returned for that seed; what changes is what
 * it costs to skip the rest.
 *
 * only the middle of the three sampling methods is replaced.  the server's sample_init and
 * sample_end already open and close a plain table scan, which is exactly what this needs.
 *
 * CMakeLists.txt compiles this only for the server that asks for these methods. */

#include "ha_tidesdb.h"

#include <mysql/plugin.h>

#include <random>

#include "src/handler/ha_tidesdb_internal.h"

int ha_tidesdb::sample_next(void *scan_ctx [[maybe_unused]], uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::sample_next");

    /* The generator belongs to the caller, which seeds it before the scan opens.  Without one
       there is nothing to roll, and returning every row is slower than asked for but never
       wrong. */
    if (!m_random_number_engine) DBUG_RETURN(rnd_next(buf));

    /* A percentage outside the range is not a proportion; hold it to one rather than read a
       fraction of the table nobody asked for.  Sampling nothing is a valid request and answers
       without reading anything. */
    double keep_fraction = m_sampling_percentage / 100.0;
    if (keep_fraction <= 0.0) DBUG_RETURN(HA_ERR_END_OF_FILE);
    if (keep_fraction > 1.0) keep_fraction = 1.0;

    std::uniform_real_distribution<double> dice(0.0, 1.0);

    for (;;)
    {
        if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_QUERY_INTERRUPTED);
        if (!scan_iter) DBUG_RETURN(HA_ERR_END_OF_FILE);

        /* Step past whatever the previous call left the iterator on, the same way rnd_next does:
           rnd_init leaves it already positioned, so the first call must not advance. */
        if (scan_dir_ != DIR_NONE) tidesdb_iter_next(scan_iter);
        scan_dir_ = DIR_FORWARD;

        if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_END_OF_FILE);

        /* Read the key only.  This is what makes the sample cheaper than the server's loop:
           a row the dice reject is never fetched and never decoded. */
        uint8_t *key = NULL;
        size_t key_size = 0;
        tdb_owned_buf key_g(key);
        if (tidesdb_iter_key(scan_iter, &key, &key_size) != TDB_SUCCESS)
            DBUG_RETURN(HA_ERR_END_OF_FILE);

        /* The engine keeps its own state under a leading namespace that sorts before every row.
           Those are not rows and must not consume a variate, or the draw would fall out of step
           with the sequence the server's own loop would have seen. */
        if (!is_data_key(key, key_size)) continue;

        if (dice(*m_random_number_engine) > keep_fraction) continue;

        /* Selected: now pay for the row.  iter_read_current re-reads the entry the iterator is
           sitting on, which is the one just chosen. */
        DBUG_RETURN(iter_read_current(buf));
    }
}

/* whether the table holds a row at all, asked before choosing a plan that is only worth it on an
   empty table.  the one caller in the server today sits inside the bulk-load path, which a
   community build refuses before any engine is reached, so this answers nothing yet -- it is here
   because the question is cheap to answer correctly and the alternative is to answer it wrongly
   the moment something does ask. */
bool ha_tidesdb::is_table_empty() const
{
    /* Answering "no" is always safe -- it is what the base implementation says -- so every path
       that cannot reach a definite answer says that rather than guessing. */
    if (!tdb_global || !share || !share->cf) return false;

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return false;

    tidesdb_iter_t *iter = NULL;
    bool empty = false;
    if (tidesdb_iter_new(txn, share->cf, &iter) == TDB_SUCCESS && iter)
    {
        /* Seek to the first row rather than the first key: the engine's own entries live under a
           namespace that sorts ahead of every row, and a table holding only those is empty. */
        const uint8_t data_prefix = KEY_NS_DATA;
        tidesdb_iter_seek(iter, &data_prefix, 1);

        empty = true;
        if (tidesdb_iter_valid(iter))
        {
            uint8_t *key = NULL;
            size_t key_size = 0;
            tdb_owned_buf key_g(key);
            if (tidesdb_iter_key(iter, &key, &key_size) == TDB_SUCCESS && key &&
                is_data_key(key, key_size))
                empty = false;
        }
        tidesdb_iter_free(iter);
    }

    (void)tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
    return empty;
}
