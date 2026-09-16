---
title: Data-at-Rest Encryption
description: Per-row encryption, the two-tier key arrangement behind it, key rotation, and how it interacts with compression and indexes.
---

# Data-at-Rest Encryption

TidesDB can encrypt row data before it is written to the column family. Keys come from a keyring
component, so the server must be started with one loaded — `component_keyring_file` is the one the
server ships, and any component implementing the keyring services works the same way.

```sql
CREATE TABLE secrets (
  id INT NOT NULL PRIMARY KEY, val VARCHAR(100)
) ENGINE=TIDESDB ENGINE_ATTRIBUTE='{"encrypted": true}';
```

Each row is encrypted individually with AES-256-CBC. The on-disk record is a 4-byte little-endian
key version, then a 16-byte random IV, then the ciphertext. The key-version prefix is what lets the
engine read rows written under an older key after a rotation: the version stamped in the row names
the exact key to decrypt it with. On read the engine decrypts transparently.

You can choose the key id:

```sql
CREATE TABLE classified (
  id INT NOT NULL PRIMARY KEY, data TEXT
) ENGINE=TIDESDB ENGINE_ATTRIBUTE='{"encrypted": true, "encryption_key_id": 2}';
```

`encryption_key_id` defaults to 1 and ranges from 1 to 255.

## How keys are held

The keyring stores and retrieves keys but supplies no cipher primitives, so the engine uses the
same two-tier arrangement InnoDB does, assembled from the keyring component services and the
server's own AES routines.

**The master key** is one 32-byte key per server, held in the keyring under
`TIDESQLKey-<server uuid>-<sequence>` and generated on first use. It never encrypts a row; it only
wraps table keys.

**A table key** is 32 random bytes per (`encryption_key_id`, version), and is what rows are
actually encrypted with. It is stored wrapped by the master key with AES-256-ECB, so the plaintext
table key exists only in memory.

The wrapped table keys live in a reserved column family, `__tidesql_crypto`. That is the closest
thing this engine has to the tablespace header InnoDB keeps its encryption information in: a column
family is already the unit of storage, addressing and configuration here, so a reserved one is the
natural home for engine-owned metadata rather than a new file format.

What the arrangement buys is what it buys InnoDB — rotating the master key touches no row data.

## Rotation

Rotation is asked for through a system variable, because a system variable is the only surface a
storage engine has for a verb. Neither form reads or rewrites a single row.

```sql
-- give encryption key 1 a new version; rows written from now on use it
SET GLOBAL tidesdb_rotate_table_key = 1;

-- mint a new master key and re-wrap every stored table key under it
SET GLOBAL tidesdb_rotate_master_key = ON;
```

Rotating a **table key** mints the next version for that key id. Rows already written keep
decrypting under the version stamped in their own blob, so nothing has to be re-encrypted and the
rotation is instant. A key id records up to 4096 versions.

Rotating the **master key** re-wraps every stored table key under a new master key. The wrapped
keys change; the rows do not. If the keyring refuses, or a re-wrap fails, the previous master key
is left in place and the stored keys are unchanged, so a failed rotation leaves the database
exactly as it was.

`tidesdb_rotate_master_key` reports `OFF` again once the rotation is done: it names an action, not
a state.

## Enabling encryption on an existing table

```sql
ALTER TABLE existing_table ENGINE_ATTRIBUTE='{"encrypted": true}';
```

This rebuilds the data column family so the current rows are rewritten as ciphertext.

## What encryption composes with

Encryption composes with everything else, including secondary indexes, BLOB columns, and TTL. The
secondary-index keys are not encrypted, because they must stay comparable for seeking, but the row
data those keys point at is encrypted in the data column family. Because ciphertext does not
compress, the engine forces the data column family's compression to `NONE` regardless of the
table's `compression` option, and the index column families are unaffected. See
[Table Options](/reference/table-options) for the compression interaction.

## Failing closed

If a key cannot be fetched — a rotation hole, a keyring component that is not loaded, or a version
that never existed — the encrypt or decrypt call fails rather than proceeding. The engine logs the
failure and returns an error to the SQL layer rather than feeding uninitialized bytes into the
cipher. A row written with a key version no longer in the keyring is unreadable until the key is
restored, and the engine never silently mis-encrypts or returns zeroed plaintext.

A table whose key is unavailable at open reports that the key is missing rather than opening with
rows nobody can read.
