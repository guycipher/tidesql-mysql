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

/* optimizer cost for a server that costs in a single double.
 *
 * three methods carry the whole cost surface here: a full scan, an index read that also fetches
 * rows, and an index-only read.  the arithmetic is src/core/cost_model's; this file is the adapter
 * that folds the engine's separated io and cpu figures into the one number the planner wants.
 *
 * the counterpart adapter for a server wanting a structured pair is ha_tidesdb_cost_iocpu.cc, and
 * CMakeLists.txt compiles exactly one of the two, which is why neither carries a preprocessor
 * branch. */

#include "ha_tidesdb.h"

#include "src/core/cost_model.h"

namespace cost = tidesdb::cost_model;

double ha_tidesdb::scan_time()
{
    /* Start from the volume-based cost the base handler derives from the row count and mean row
       length info() publishes, so a large table is never mispriced as cheap and the planner keeps
       preferring an index lookup wherever one is genuinely cheaper.  On top of that, add the
       merge-fanout surcharge, which volume alone cannot see. */
    const double base = handler::scan_time();
    const double overlap = scan_overlap();

    return base + cost::combine(cost::scan_surcharge_io(overlap, TIDESDB_SCAN_IO_WEIGHT),
                                cost::scan_surcharge_cpu(overlap, TIDESDB_SCAN_CPU_WEIGHT));
}

double ha_tidesdb::read_time(uint, uint ranges, ha_rows rows)
{
    /* An index read that fetches rows pays the key walk plus a point lookup per row, both scaled by
       how many levels a lookup expects to touch. */
    const double amp = read_amp();
    const double keys = cost::key_read_cost((double)rows, (double)ranges, amp,
                                            TIDESDB_COST_KEY_READ, TIDESDB_COST_RANGE_SETUP);
    const double fetch = cost::row_read_cost((double)rows, amp, TIDESDB_COST_SEQ_READ);

    return cost::combine(0.0, keys + fetch);
}

double ha_tidesdb::index_only_read_time(uint, double records)
{
    /* A covering read never leaves the index, so it pays the key walk over one range and no row
       fetch at all -- which is the whole reason the planner asks separately. */
    return cost::combine(0.0, cost::key_read_cost(records, 1.0, read_amp(), TIDESDB_COST_KEY_READ,
                                                  TIDESDB_COST_RANGE_SETUP));
}
