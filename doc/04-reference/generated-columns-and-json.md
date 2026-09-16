---
title: Generated Columns and JSON
description: Virtual and stored generated columns, indexing either, and the pattern for indexing JSON paths.
---

# Generated Columns and JSON

## Generated columns

The engine supports both `VIRTUAL` and `STORED` generated columns, and either can be indexed. A
virtual column is computed on read and never returned from storage; a stored column is computed on
write and persisted with the row, so it reads back without recomputation.

```sql
CREATE TABLE orders (
  id       INT PRIMARY KEY,
  price    DECIMAL(10,2),
  qty      INT,
  total    DECIMAL(10,2) AS (price * qty) VIRTUAL,
  category VARCHAR(10) AS (CASE WHEN price >= 100 THEN 'premium' ELSE 'standard' END) VIRTUAL,
  KEY total_idx (total)
) ENGINE=TIDESDB;

INSERT INTO orders (id, price, qty) VALUES (1, 49.99, 3);
SELECT * FROM orders;                     -- total = 149.97, category = 'standard'
SELECT id FROM orders WHERE total = 149.97;   -- uses total_idx
```

Indexing a virtual column costs storage for the index but not for the column: the value is
materialised into the index entries and recomputed for the row itself. Which to choose is the usual
trade — a stored column pays once on write and nothing on read, a virtual one the reverse.

The server evaluates a generated expression into the row before the row reaches the engine, on both
the write and the delete path, so the engine stores and indexes whatever the server computed and
never evaluates an expression itself.

A column default that is an expression rather than a constant works the same way:

```sql
CREATE TABLE t (
  id  INT PRIMARY KEY,
  tag VARCHAR(8) DEFAULT (CONCAT('n', 'a')),
  upd TIMESTAMP(6) DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6)
) ENGINE=TIDESDB;
```

## JSON

`JSON` is a native type here, stored in the server's binary JSON representation, and the JSON
functions — `JSON_VALUE()`, `JSON_EXTRACT()`, `JSON_SET()`, `JSON_CONTAINS()` and the rest — work
normally on TidesDB tables, evaluated by the server.

For efficient filtering on JSON paths, extract the paths you care about into generated columns and
index those:

```sql
CREATE TABLE docs (
  id   INT NOT NULL PRIMARY KEY,
  data JSON,
  name VARCHAR(100) AS (JSON_VALUE(data, '$.name')) STORED,
  age  INT          AS (JSON_VALUE(data, '$.age' RETURNING SIGNED)) STORED,
  KEY idx_name (name),
  KEY idx_age (age)
) ENGINE=TIDESDB;

INSERT INTO docs (id, data) VALUES
  (1, '{"name":"Alice","age":30,"tags":["admin","dev"]}'),
  (2, '{"name":"Bob","age":25,"tags":["dev"]}');

SELECT id, name FROM docs WHERE name = 'Alice';   -- uses idx_name
SELECT id, age  FROM docs WHERE age >= 30;        -- uses idx_age
```

`JSON_VALUE` returns a string unless told otherwise, so give a numeric path a `RETURNING` clause and
a matching column type — without it the extracted value is compared as text, and `age >= 30` orders
lexically rather than numerically.

This gives engine-native indexing through ordinary secondary indexes while keeping JSON manipulation
in standard SQL.
