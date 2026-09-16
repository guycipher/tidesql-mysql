---
title: How Data Is Stored
description: The physical layout of rows, keys, and secondary index entries inside a column family.
---

# How Data Is Stored

Understanding the physical layout helps when reading `ANALYZE TABLE` output or diagnosing
performance. The storage mechanics below the column family, SSTables, compaction, and recovery,
belong to the library and are covered in the [TidesDB library manual](/internals/architecture).

Each table's data lives in a column family, an independent LSM B+tree. Writes go to a skip-list
memtable. When the memtable fills it becomes immutable and is flushed as a sorted SSTable, and
compaction merges overlapping SSTables into higher levels while keeping the sorted invariant.

## Keys

Row keys inside a column family carry a namespace prefix byte, `0x01` for data rows and `0x00` for
metadata. The metadata namespace holds the auto-increment counter, under an `AINC` key as an 8-byte
big-endian value; the table's storage options, under an `OPTS` key as the JSON object the engine
reads back at open; and the FTS aggregate counters (document count and word count for BM25 scoring).
Because `0x00` sorts before `0x01`, a data scan that seeks to `0x01` naturally skips all of them, and
a scan looking for the last row is never handed a metadata key.

Primary-key bytes are encoded in a memcmp-comparable form. For a signed 32-bit integer the encoding
flips the sign bit and stores the result big-endian, so `-1` sorts before `0` and `0` before `1`
under a plain byte comparison. The same principle covers the other numeric types and string
collations. A table without an explicit primary key gets a hidden 8-byte big-endian row id from an
atomic counter, recovered at open by seeking the last key in the column family.

## Row values

Row values are a packed binary format. Each row begins with a 5-byte header, a magic byte `0xFE`
followed by the null bitmap size (2 bytes little-endian) and the field count (2 bytes little-endian)
as of the write. That header is what makes `ADD COLUMN` and `DROP COLUMN` instant, because the
deserializer can adapt to rows written under any prior schema. After the header comes the null
bitmap, then each non-null field serialized with `Field::pack()`. On read, `Field::unpack()`
restores the fields. A row written with fewer fields than the current schema, from before a column
was added, fills the missing fields with their `DEFAULT`, and a row written with more fields, from
before a column was dropped, has the extra data skipped. This is more compact than the raw record
buffer, especially for `VARCHAR` and `CHAR` columns.

## Secondary index entries

Every index other than the primary key lives in its own column family, and all three kinds put the
primary key at the end of the entry key so the entry identifies its row. What differs is what leads
the key and what, if anything, the value carries.

| Index | Key | Value |
|-------|-----|-------|
| Ordinary | comparable index-column bytes, then the comparable PK bytes | one zero byte |
| Spatial | the Hilbert code of the geometry's bounding-box centroid, then the PK bytes | the bounding box, as four doubles |
| Full-text | the term, then the PK bytes | the term frequency and the document's word count, for BM25 |

An ordinary index keeps everything in the key, which is what lets it answer from the key alone. To
resolve a lookup the engine seeks into the index CF, reads the key, splits off the trailing PK bytes,
and does a point-get into the data CF. When the query needs only indexed columns and each is of a
reconstructable type, integers, temporal types, or fixed `CHAR`/`BINARY` in binary or latin1, the row
is decoded straight from the index key bytes and the data-CF point-get is skipped.

The other two carry a value because their key is lossy by design. A Hilbert code orders geometries
for locality but does not describe them, so the bounding box in the value is what a spatial predicate
is actually filtered against; see [Spatial Indexes](/reference/spatial-indexes). A full-text entry
needs per-document term statistics that the term and the row id do not carry; see
[Full-Text Search](/reference/full-text-search).
