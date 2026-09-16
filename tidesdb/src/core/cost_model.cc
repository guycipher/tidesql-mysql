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

#include "cost_model.h"

namespace tidesdb
{
namespace cost_model
{

/* a negative row, range or overlap count is not a cost the planner can act on, and clamping to
   zero here keeps a bad estimate upstream from producing a negative cost that reads as free. */
static double non_negative(double v)
{
    return v > 0.0 ? v : 0.0;
}

/* read amplification is a level count, so it never sensibly falls below one whole level; a caller
   with unsampled statistics passes 1.0, and anything lower would price a lookup below the single
   level it must always touch. */
static double at_least_one(double v)
{
    return v > 1.0 ? v : 1.0;
}

double key_read_cost(double rows, double ranges, double read_amp, double per_row, double per_range)
{
    return (non_negative(rows) * per_row * at_least_one(read_amp)) +
           (non_negative(ranges) * per_range);
}

double row_read_cost(double rows, double read_amp, double per_row)
{
    return non_negative(rows) * per_row * at_least_one(read_amp);
}

double scan_surcharge_io(double overlap, double weight)
{
    return non_negative(overlap) * weight;
}

double scan_surcharge_cpu(double overlap, double weight)
{
    return non_negative(overlap) * weight;
}

double combine(double io, double cpu)
{
    return io + cpu;
}

}  // namespace cost_model
}  // namespace tidesdb
