---
title: Partitioning
description: Why partitioned tables are not available to this engine on this server, and what to use instead.
---

# Partitioning

A TidesDB table cannot be partitioned on this server. Every partitioned `CREATE TABLE` is refused:

```sql
CREATE TABLE metrics (
  id INT NOT NULL, ts DATE NOT NULL, value DOUBLE, PRIMARY KEY (id, ts)
) ENGINE=TIDESDB PARTITION BY RANGE COLUMNS(ts) (
  PARTITION p_2024 VALUES LESS THAN ('2025-01-01'),
  PARTITION pmax   VALUES LESS THAN MAXVALUE
);
ERROR 42000: The storage engine for the table doesn't support native partitioning
```

`ALTER TABLE ... PARTITION BY` is refused the same way, so a partitioned table cannot come into
existence by any route, and the table it was attempted on is left untouched. Partition-management
statements against a table that is not partitioned report that instead:

```sql
ALTER TABLE metrics ADD PARTITION (PARTITION p2 VALUES LESS THAN (30));
ERROR HY000: Partition management on a not partitioned table is not possible
```

## Why

This server has no general partitioning layer. It was removed, and an engine that wants partitioned
tables implements the partitioning itself, as the built-in transactional engine does with a second
handler of its own. This engine does not implement one.

That is the right outcome rather than a gap papered over: the alternative would be a table the
server believes is partitioned and the engine stores as a single heap, where partition pruning
returns wrong answers and `DROP PARTITION` removes nothing.

## What to use instead

A column family is already the unit of storage, compaction and configuration here, and one table is
one column family. So the shape partitioning is usually reached for — keeping a large table's
maintenance and retention independent per slice — is available by using separate tables and a view
or application-side routing over them:

```sql
CREATE TABLE metrics_2025 (id INT NOT NULL, ts DATE NOT NULL, value DOUBLE,
                           PRIMARY KEY (id, ts)) ENGINE=TIDESDB;
CREATE TABLE metrics_2026 (id INT NOT NULL, ts DATE NOT NULL, value DOUBLE,
                           PRIMARY KEY (id, ts)) ENGINE=TIDESDB;

CREATE VIEW metrics AS
  SELECT * FROM metrics_2025 UNION ALL SELECT * FROM metrics_2026;
```

Dropping a slice is then `DROP TABLE`, which removes its column family outright — the same
constant-time reclaim `DROP PARTITION` would have given.

For time-series retention specifically, [Time-To-Live](/reference/ttl) expires rows without any of
this: a table-level `ttl` reclaims expired rows at compaction, which is cheaper than dropping a
partition because nothing has to be rewritten.

The suite pins this refusal, so if native partitioning is ever added to the engine the pinned tests
fail and ask for the partitioned-table tests to be run instead.
