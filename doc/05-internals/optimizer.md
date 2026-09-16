---
title: Statistics and the Optimizer
description: The cached statistics the engine keeps, how they feed the cost model, and how range cardinality is estimated.
---

# Statistics and the Optimizer

The engine keeps cached statistics refreshed at most every two seconds, the total key count, the
total data size, the average key and value sizes, the LSM B+tree read amplification factor, and a
per-index `rec_per_key` selectivity for each secondary index. They are atomic variables on the shared
table descriptor and feed the server's cost-based optimizer through `info()`, `scan_time()`,
`keyread_time()`, `rnd_pos_time()`, and `records_in_range()`.

Secondary-index `rec_per_key` is measured automatically. The first time a populated table, one that
has crossed a small row threshold, is asked for constant statistics, the engine samples the distinct
index prefixes of each secondary index on its own short read transaction and caches the result, once
per open table. A workload never has to run `ANALYZE TABLE` to get real selectivity; until the sample
lands a high-cardinality index falls back to a coarse fraction of the row count, which is what would
otherwise steer the optimizer into scanning an IN-list or a range rather than using the index. An
explicit `ANALYZE TABLE` still refreshes the same figures.

The cost model accounts for an LSM read consulting several levels. The read amplification factor,
from the library's statistics, scales the cost of point lookups and random-position reads. A higher
read amplification nudges the optimizer toward sequential scans, and when the data is well compacted
and the amplification is low, index lookups are cheap.

## Cost methods

- `scan_time()` starts from the base `handler::scan_time()` row-count cost and adds an LSM surcharge
  for the SSTable overlap the scan has to merge across. The surcharge is the count of overlapping
  SSTables weighted 90% as I/O and 10% as CPU, so the same row count costs more to scan when it is
  spread across many overlapping SSTables than when it sits in a compacted shape. The surcharge
  reflects overlap only, not compression or merge policy.
- `keyread_time()` models index reads as `rows * 0.00003 * read_amp + ranges * 0.0001`, since each
  point lookup touches `read_amp` levels and a range scan amortizes the merge-heap setup across rows.
- `rnd_pos_time()` models random-position lookups as `rows * 0.00005 * read_amp`, reflecting that
  each random fetch is a point-get through the full LSM stack.

## Range-aware cardinality

`records_in_range()` takes one of two paths.

For a point equality, where both bounds convert to identical comparable bytes such as
`WHERE k = 5`, a unique or primary key matches one row, and a non-unique index that ANALYZE or the
open-time pass has already sampled carries a trustworthy `rec_per_key`, so both read that cached
estimate directly. Only a non-unique index with no sample yet needs more. There the value bytes
encode the index value without its primary-key suffix, so every matching row stores a key with that
value as a prefix, and the matching rows are exactly the half-open range from the value to its
successor. The engine probes that span with `tidesdb_range_stats`, which counts a single value from
metadata and is right for a low- or high-cardinality index alike, so a never-analyzed table gets a
correct estimate rather than the `records / 10` fallback that reads one row per value and drives a
full scan. The successor is the value with its last byte below `0xFF` incremented and the trailing
bytes dropped, and an all-`0xFF` value that has no finite successor falls back to the cached
estimate.

For a range predicate the engine asks the library for a direct row estimate over the requested range
through `tidesdb_range_stats`, examining in-memory metadata, block indexes, SSTable min and max keys,
and entry counts with no disk I/O, and returns that estimate as an absolute cardinality. The estimate
is then clamped to the table's live row count, because a flushed SSTable can still carry superseded
MVCC versions of a key and the raw count would otherwise exceed the number of rows the table actually
holds. That clamp is what keeps a memtable-resident range from being estimated as a large fraction of
the table and steering the optimizer into a full scan. When the library cannot produce an estimate
the engine falls back to a quarter of the table's rows plus one. A narrow range returns a small
estimate and a wide one a proportionally larger estimate, which lets the optimizer choose indexes and
join order sensibly.

## Column histograms

Everything above describes statistics the engine keeps for itself. A column histogram is the other
kind: the server builds and owns it, and asks the engine only for rows to build it from. `ANALYZE
TABLE t UPDATE HISTOGRAM ON c` reads a sample of the table, and the engine takes that sample rather
than letting the server read every row and throw most away.

The server decides how large the sample should be. It divides `histogram_generation_max_mem_size`
by the bytes one distinct value costs it to hold, and divides that by the engine's row estimate: if
the table fits in the budget the sample is the whole table, and otherwise it is the fraction that
fits. The row estimate is the cached one, refreshed at most every two seconds, so a histogram built
immediately after a bulk load can divide by a stale count and sample more of the table than it
meant to. `ANALYZE TABLE t` on its own forces the estimate current, and running it before the
histogram command is what makes the fraction the intended one.

Given that fraction, the sampling itself runs in the engine. A scan walks keys in order, and a key
is cheap to read: the engine draws one variate per row from the generator the server seeded, and
fetches and decodes the row only when the draw falls within the fraction. A rejected row is never
read past its key, which on a table whose values are separated into the value log saves the second
read that decoding it would have cost. The generator, the seed and the draw order are the server's
own, so the rows returned are the rows the server's own sampling loop would have returned; only the
cost of skipping the rest is different.

The histogram this produces is the server's ordinary equi-height or singleton histogram, readable
through `information_schema.COLUMN_STATISTICS`, and it carries the `sampling-rate` the server chose.
A sampled histogram describes the column from a subset of it, so its outermost buckets need not
reach the column's true minimum and maximum — but every bound in it is a value that is really in the
table.
