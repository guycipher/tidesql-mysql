---
title: Table Options
description: The per-table options that configure a table's column family at CREATE TABLE, how they are written, their defaults, and their session-default variables.
---

# Table Options

Per-table options are settled when the table is created and configure its column family from then
on. Most have a `tidesdb_default_*` session variable, so a deployment can set the policy once and
let every `CREATE TABLE` inherit it, with an explicit option overriding the default for one table.
Three have no session default and are off unless a table asks for them: `ttl`, `encrypted`, and
`encryption_key_id`.

The complete set is `compression`, `bloom_filter`, `bloom_fpr`, `keep_values_inline`,
`btree_klog_block_size`, `level_size_ratio`, `min_levels`, `dividing_level_offset`,
`l1_file_count_trigger`, `tombstone_density_trigger`, `tombstone_density_min_entries`, `ttl`,
`encrypted`, `encryption_key_id`, and `isolation_level`.

## How options are written

The server has no engine-specific `CREATE TABLE` grammar, so a table names its options in
`ENGINE_ATTRIBUTE`, a JSON object the server stores in the data dictionary and hands to the engine
without reading it:

```sql
CREATE TABLE archive (id INT PRIMARY KEY, data TEXT) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"compression": "ZSTD", "bloom_fpr": 50}';
```

Names are case-insensitive. A yes-or-no option accepts `true`/`false`, `"YES"`/`"NO"`,
`"ON"`/`"OFF"`, or `1`/`0`. An option with a fixed set of values accepts a name or its position.

Because the engine defines these names rather than the server, the engine is also what checks them.
An option this build does not recognise, or a value outside what it accepts, fails the statement
and says which one:

```sql
CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TIDESDB ENGINE_ATTRIBUTE='{"blom_filter": false}';
ERROR HY000: ENGINE_ATTRIBUTE: unknown option 'blom_filter'

CREATE TABLE t (id INT PRIMARY KEY) ENGINE=TIDESDB ENGINE_ATTRIBUTE='{"bloom_fpr": 0}';
ERROR HY000: ENGINE_ATTRIBUTE: '0' is out of range for 'bloom_fpr'
```

A misspelled option is refused rather than stored and ignored, because a table stored differently
from the one that was asked for reports nothing later.

`ALTER TABLE ... ENGINE_ATTRIBUTE='{...}'` restates a table's options and is checked the same way.

## What a table inherits, and when

An option a table does not name is resolved from the matching `tidesdb_default_*` session variable
**when the table is created**, and stays with the table from then on. Changing a session default
never reaches a table that already exists:

```sql
SET SESSION tidesdb_default_compression = 'ZSTD';
CREATE TABLE a (id INT PRIMARY KEY) ENGINE=TIDESDB;   -- ZSTD, permanently

SET SESSION tidesdb_default_compression = 'NONE';
CREATE TABLE b (id INT PRIMARY KEY) ENGINE=TIDESDB;   -- NONE; a is untouched
```

The server records only what a table actually named, so the engine records the resolved set in the
table's own storage to make the rest of that guarantee true. One consequence is visible:
`SHOW CREATE TABLE` shows the options a table named and not the ones it inherited.

## Compression

The choices are `NONE`, `SNAPPY`, `LZ4`, `ZSTD`, and `LZ4_FAST`, and the default is `LZ4`. `ZSTD`
gives the best ratio, `LZ4` and `LZ4_FAST` favor speed. An encrypted table is the exception: its
data column family is created with compression forced to `NONE` regardless of this option, because
the rows are already ciphertext by the time they reach the library and ciphertext does not compress.
The table's secondary-index column families hold unencrypted comparable keys and keep whatever
algorithm was selected. Session default `tidesdb_default_compression`.

Every backend except `NONE` has to be compiled into the linked TidesDB library, which is a build
option there, so a library built without a given backend rejects a table that asks for it and the
`CREATE` fails. `NONE` is always available. On a build that omits the default `LZ4`, set
`tidesdb_default_compression` to a backend the library carries, or to `NONE`, before creating tables.

## Bloom filters

```sql
CREATE TABLE no_bloom (id INT PRIMARY KEY, v INT) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"bloom_filter": false}';
CREATE TABLE precise  (id INT PRIMARY KEY, v INT) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"bloom_fpr": 10}';
```

Bloom filters let a point lookup skip SSTables that cannot contain the key. `bloom_filter` enables
them, on by default. `bloom_fpr` is the false-positive rate in parts per 10,000, default 100 for a
1% rate, and accepts 1 through 10000. Session defaults `tidesdb_default_bloom_filter` and
`tidesdb_default_bloom_fpr`.

## Keeping values inline

```sql
CREATE TABLE inline_vals (id INT PRIMARY KEY, val VARCHAR(200)) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"keep_values_inline": true}';
```

Each SSTable has its own key log, and the whole database shares one segmented value log. Value
separation is a database-wide policy set by
`tidesdb_value_separation_threshold`, so a value at or above that size goes to the shared value log
with a pointer left in the key log, and a smaller one stays inline. Separating a large value keeps
it out of every later merge, which is the whole point of the threshold, at the cost of one value-log
read per row on a scan. `keep_values_inline` overrides the policy for one table and holds every
value in the key log whatever its size, which is worth it for a table that is scanned far more than
it is merged. Session default `tidesdb_default_keep_values_inline`.

`btree_klog_block_size` sets the block size in bytes of the key log's B+tree nodes, default 4096,
minimum 512. The default matches the block manager's first-read window so a node is read in one go,
and sizing a node just above that window costs a second read on every access. Session default
`tidesdb_default_btree_klog_block_size`.

## LSM B+tree tuning

```sql
CREATE TABLE tuned (id INT PRIMARY KEY, v VARCHAR(200)) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"level_size_ratio": 8, "min_levels": 3,
                     "dividing_level_offset": 1, "l1_file_count_trigger": 4}';
```

`level_size_ratio` is how much larger each level is than the previous one, default 10, accepted
range 2 to 100. `min_levels` is the minimum tree depth, default 1, up to 64.
`dividing_level_offset` sets the offset used to compute the dividing level, the primary compaction
target, calculated as `num_levels - 1 - dividing_level_offset`, default 1, up to 64.
`l1_file_count_trigger` is how many SSTables may accumulate at level 1 before compaction merges
them down, default 4, up to 1024. TidesDB does not use a selectable compaction policy: it chooses
among full preemptive merge, dividing merge, and partitioned merge automatically from the tree's
state relative to the dividing level. Session defaults `tidesdb_default_level_size_ratio`,
`tidesdb_default_min_levels`, `tidesdb_default_dividing_level_offset`, and
`tidesdb_default_l1_file_count_trigger`.

## Tombstone density trigger

```sql
CREATE TABLE events (
  id BIGINT PRIMARY KEY, ts DATETIME, body TEXT, KEY (ts)
) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"tombstone_density_trigger": 5000,
                     "tombstone_density_min_entries": 2048}';
```

After each flush the engine can escalate compaction for any level-1 SSTable whose tombstone count
divided by entry count exceeds a ratio, provided the SSTable has enough entries to matter.
`tombstone_density_trigger` is that ratio in parts per 10,000, so `5000` means 0.50, and the default
`0` disables the check. `tombstone_density_min_entries` is the entry-count floor, default 1024, that
stops a tiny SSTable from firing compaction. Restating the option with `ALTER TABLE` updates the
live column family, so the new ratio applies on the next post-flush check without a restart.
Session defaults `tidesdb_default_tombstone_density_trigger` and
`tidesdb_default_tombstone_density_min_entries`. The [write-path chapter](/internals/write-path)
covers how this fits with the single-delete optimization.

## Options covered elsewhere

- `ttl` at the table level, and `{"ttl": true}` on a column, set row expiration. See
  [Time-To-Live](/reference/ttl).
- `encrypted` and `encryption_key_id` turn on data-at-rest encryption. See
  [Data-at-Rest Encryption](/reference/encryption).
- `isolation_level` pins the per-table isolation level. See
  [Transactions and Isolation](/concepts/transactions).

## Combining options

```sql
CREATE TABLE optimized (id INT PRIMARY KEY, val VARCHAR(100)) ENGINE=TIDESDB
  ENGINE_ATTRIBUTE='{"compression": "ZSTD", "bloom_filter": true, "bloom_fpr": 50,
                     "keep_values_inline": true, "isolation_level": "REPEATABLE_READ"}';
```
