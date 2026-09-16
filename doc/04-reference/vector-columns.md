---
title: Vector Columns
description: Why a VECTOR column works in a TidesDB table, what it actually costs, and why there is no similarity search over it.
---

# Vector Columns

A TidesDB table can hold `VECTOR` columns. That is worth stating plainly because it is not a feature
the engine implements — it is a consequence of how the server models the type.

`VECTOR` is a blob type. In the server, `Field_vector` derives from `Field_blob` and inherits its
`BLOB_FLAG`, and the engine keys its blob handling off exactly that flag. A vector therefore reaches
the row codec as the bytes it is and is stored, read back and re-encoded by the same code that
handles `BLOB` and `TEXT`. There is no vector-specific line anywhere in TideSQL, and none in the
TidesDB library either.

What follows from that is the useful part: everything true of a blob column is true of a vector one.

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
`FROM_VECTOR`) renders it back, and `VECTOR_DIM` reports the dimension. A vector whose dimension does
not match the column is refused with `ER_DATA_TOO_LONG`.

A vector survives updates, deletes and restarts byte for byte, sits happily in a table reached
through a secondary index on another column, and is covered by the row format's versioning, so a
table whose shape changed still reads its older rows back. A large vector crosses the value-log
threshold like any other large value, which means it is written once and left out of later merges;
see [Table Options](/reference/table-options) for the threshold and how to opt a table out of it.

## There is no similarity search

Neither half of an approximate nearest-neighbour search exists. The server has no vector index type
and no distance function, so the queries below have no SQL to be written in:

```sql
-- not available: the syntax does not parse
CREATE TABLE t (id INT PRIMARY KEY, v VECTOR(4) NOT NULL, VECTOR INDEX (v)) ENGINE=TIDESDB;

-- not available: no such function
SELECT VEC_DISTANCE_EUCLIDEAN(v, STRING_TO_VECTOR('[...]')) FROM t;
```

Indexing the column at all is refused, whichever spelling is used — `KEY (v)`, `FULLTEXT KEY (v)` and
`SPATIAL KEY (v)` are all rejected with `ER_NON_SCALAR_USED_AS_KEY` (6134), because a vector is not a
scalar the server can order.

The engine is not waiting on the server here. It has no vector code of its own to offer, so if MySQL
gains a vector index one day, what TideSQL would need depends entirely on how the server builds it —
whether the index lives in a table the engine merely stores, or in a structure the engine has to
maintain. The suite carries a test that fails if either half arrives, so the question gets asked
rather than missed.

Until then, ranking vectors held in TidesDB is an application-side job: read the candidates out and
compute distances in the client.

## What that costs

An exact scan over every vector is `O(rows)` and reads each row's full value, the value log included.
Narrowing first is what makes it affordable — a `WHERE` on an indexed column, a time window, a tenant
id — so the distance computation runs over a bounded candidate set rather than the whole table. The
[optimizer chapter](/internals/optimizer) covers how the engine costs those scans.
