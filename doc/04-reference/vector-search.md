---
title: Vector Columns
description: Storing VECTOR columns in TidesDB, what the server does and does not provide, and what that means for nearest-neighbour search.
---

# Vector Columns

A TidesDB table can hold `VECTOR` columns, and the engine stores and returns them like any other
value.

```sql
CREATE TABLE embeddings (
  id    INT NOT NULL PRIMARY KEY,
  title VARCHAR(200),
  v     VECTOR(384) NOT NULL,
  KEY title_idx (title)
) ENGINE=TIDESDB;

INSERT INTO embeddings VALUES (1, 'cat picture', STRING_TO_VECTOR('[0.1, 0.9, ...]'));

SELECT id, title, VECTOR_TO_STRING(v), VECTOR_DIM(v) FROM embeddings;
```

`STRING_TO_VECTOR` (also spelled `TO_VECTOR`) parses the text form, `VECTOR_TO_STRING` (also
`FROM_VECTOR`) renders it back, and `VECTOR_DIM` reports the dimension. A vector whose dimension
does not match the column is refused with `ER_DATA_TOO_LONG`.

Everything the engine is responsible for applies to a vector as it does to any other column: it
survives updates, deletes and restarts byte for byte, it can sit in a table reached through a
secondary index on another column, and it is covered by the row format's versioning so a table
whose shape changed still reads back.

## Nearest-neighbour search

There is no vector index and no distance function on this server. Both halves of an approximate
nearest-neighbour search are missing, so there is no query for the engine to answer:

```sql
-- not available: the syntax does not parse
CREATE TABLE t (id INT PRIMARY KEY, v VECTOR(4) NOT NULL, VECTOR INDEX (v)) ENGINE=TIDESDB;

-- not available: no such function
SELECT VEC_DISTANCE_EUCLIDEAN(v, STRING_TO_VECTOR('[...]')) FROM t;
```

This is a property of the server rather than of the engine. A vector index is built and searched by
the SQL layer, which asks the engine only to store the graph it builds; where a server provides one,
TidesDB stores it like any other index. Nothing in the engine has to change to gain the feature, and
the suite carries a test that fails if either half arrives, so it is noticed rather than missed.

Until then, a nearest-neighbour search over vectors held in TidesDB is an application-side scan:
read the candidates out and rank them in the client, or narrow the candidate set with an ordinary
secondary index first.

## What this costs

An exact scan over every vector is `O(rows)` and reads each row's full value. Narrowing first is
what makes that affordable — a `WHERE` on an indexed column, a time window, a tenant id — so the
distance computation runs over a bounded candidate set rather than the whole table. The
[optimizer chapter](/internals/optimizer) covers how the engine costs those scans.
