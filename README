TIDESQL
=======

A pluggable write and space optimized storage engine for MySQL, built on TidesDB.

A TideSQL table keeps its rows in a TidesDB LSM B+tree instead of InnoDB, reached through
ordinary SQL.  Moving a table onto it is a change to the ENGINE clause and nothing more.

    CREATE TABLE events (
      id   INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
      ts   DATETIME NOT NULL,
      kind VARCHAR(50),
      KEY kind_idx (kind)
    ) ENGINE=TidesDB;

WHAT YOU GET
============

Transactions with optimistic MVCC, so readers never block writers.  Primary and secondary
indexes, foreign keys enforced in the engine, auto-increment, virtual and stored generated
columns and indexes on either, savepoints, XA and crash recovery, row expiry (TTL), data-at-rest
encryption with key rotation, full-text and spatial indexes, online DDL, and online backup.

`VECTOR` columns work, though not because the engine does anything for them: the server models the
type as a blob and the engine stores blobs.  They round-trip faithfully and that is all -- there is
no similarity search, on either side.  See doc/06-appendix/limitations.md.

Per-table storage options -- compression, bloom filters, isolation, TTL, LSM tuning -- are named
through ENGINE_ATTRIBUTE:

    CREATE TABLE archive (id INT PRIMARY KEY, data TEXT) ENGINE=TidesDB
      ENGINE_ATTRIBUTE='{"compression": "ZSTD", "ttl": 604800}';

Concurrency is optimistic rather than lock-based, which is the one behavioural difference worth
reading about before porting a write-contended workload: a conflict surfaces at COMMIT and the
application retries.  See doc/02-concepts/transactions.md.

REQUIREMENTS
============

The TidesDB library, and a MySQL source tree to build the plugin against.  If the library was
linked against jemalloc, mimalloc or tcmalloc, the allocator must be preloaded at server start or
the plugin cannot be loaded at all -- doc/01-getting-started/install.md has the error and the fix.

DOCUMENTATION
=============

The manual is under /doc, in reading order, with doc/manual.json as its table of contents.
Start at doc/00-preface/preface.md, or doc/01-getting-started/install.md to build and load it.

doc/06-appendix/limitations.md is the short list of what this server does not offer an engine.
VERSIONING.md records which TidesDB library release a given TideSQL release links, and what a
major, minor or patch bump is allowed to change.

LICENSE
=======

GNU General Public License v2
