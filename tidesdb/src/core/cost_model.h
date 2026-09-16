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

/* server-free core for what an access path costs on an LSM tree.
 *
 * the servers this engine targets ask for cost in different shapes -- one wants a flat double, the
 * other a separated io and cpu pair -- but they are asking the same question, and the answer is the
 * same arithmetic over rows, ranges and read amplification. that arithmetic lives here so the two
 * adapters that present it stay thin, and so the numbers can be unit-tested without a server.
 *
 * every figure is in the server's own cost unit, where 1.0 is roughly one sequential page read. the
 * weights are calibrated constants rather than measurements, so treat a change to any of them as a
 * planner change and re-measure the plans it moves. */
#pragma once

namespace tidesdb
{
namespace cost_model
{

/**
 * key_read_cost
 * what reading `rows` rows through `ranges` index lookups costs, before the per-row read
 * amplification an LSM tree adds
 * @param rows the row count the plan expects to read
 * @param ranges the number of separate index lookups those rows arrive through
 * @param read_amp the levels a point lookup expects to touch; 1.0 when unsampled
 * @param per_row the per-row key-read weight
 * @param per_range the fixed setup weight each range costs
 * @return the cpu cost
 */
double key_read_cost(double rows, double ranges, double read_amp, double per_row, double per_range);

/**
 * row_read_cost
 * what reading `rows` rows by position costs -- each is a point lookup through the levels, so read
 * amplification applies in full
 * @param rows the row count
 * @param read_amp the levels a point lookup expects to touch; 1.0 when unsampled
 * @param per_row the per-row sequential-read weight
 * @return the cpu cost
 */
double row_read_cost(double rows, double read_amp, double per_row);

/**
 * scan_surcharge_io
 * the io a full scan pays for merging sorted runs, over and above the volume cost the server
 * already derives from row count and row length
 * @param overlap the number of sorted runs the scan merges
 * @param weight the per-run io weight
 * @return the io surcharge, zero when nothing overlaps
 */
double scan_surcharge_io(double overlap, double weight);

/**
 * scan_surcharge_cpu
 * the cpu counterpart of scan_surcharge_io, the merge-heap work a scan does per sorted run
 * @param overlap the number of sorted runs the scan merges
 * @param weight the per-run cpu weight
 * @return the cpu surcharge, zero when nothing overlaps
 */
double scan_surcharge_cpu(double overlap, double weight);

/**
 * combine
 * fold a separated io and cpu pair into the single figure a flat-cost server expects.  the two are
 * already in the same unit, so the fold is a sum; it exists as a named function so both adapters
 * agree on that and a future weighting changes one place
 * @param io the io component
 * @param cpu the cpu component
 * @return the combined cost
 */
double combine(double io, double cpu);

}  // namespace cost_model
}  // namespace tidesdb
