---
title: Status Variables
description: Every TideSQL status variable exposed through SHOW GLOBAL STATUS, grouped by what it measures.
---

# Status Variables

```sql
SHOW GLOBAL STATUS LIKE 'tidesdb%';
```

or from `performance_schema`, which is where this server keeps the status tables:

```sql
SELECT VARIABLE_NAME, VARIABLE_VALUE FROM performance_schema.global_status
WHERE VARIABLE_NAME LIKE 'tidesdb%';
```

These are the machine-readable counters for a monitoring agent such as a Prometheus exporter or
PMM. [Monitoring](/administration/monitoring) explains which of them matter and what healthy looks
like. They are refreshed on demand behind a short coalescing window, so reading many of them in one
statement costs a single stats pass.

## Identity

| Variable | Description |
|----------|-------------|
| `tidesdb_version` | TideSQL plugin version string, for example `2.0.0` |
| `tidesdb_version_hex` | Plugin version as an integer, major-minor-patch a byte each, for example `131072` for `0x020000` |
| `tidesdb_library_version` | Linked TidesDB library version string |

## Sequence and transactions

| Variable | Description |
|----------|-------------|
| `tidesdb_column_families` | Number of active column families |
| `tidesdb_global_sequence` | Global MVCC sequence number |
| `tidesdb_min_snapshot_sequence` | Oldest pinned snapshot, the floor compaction cannot reclaim past |
| `tidesdb_active_transactions` | Transactions currently joined to the MVCC registry |
| `tidesdb_txn_memory_bytes` | Memory held by in-flight transactions in bytes |

## Memory and storage

| Variable | Description |
|----------|-------------|
| `tidesdb_memtable_bytes` | Bytes in the active memtable |
| `tidesdb_total_sstables` | Total SSTable count across all column families |
| `tidesdb_open_sstables` | Open SSTable file handles |
| `tidesdb_data_size_bytes` | Total on-disk data size in bytes |
| `tidesdb_immutable_memtables` | Sealed memtables waiting to be flushed |
| `tidesdb_flush_pending` | Flushes pending (the immutable memtable queue depth) |
| `tidesdb_compaction_queue` | Compaction jobs queued for the worker pool |
| `tidesdb_memtable_is_flushing` | 1 while an immutable is queued or flushing |
| `tidesdb_wal_generation` | Current write-ahead-log generation counter |

## Value log

| Variable | Description |
|----------|-------------|
| `tidesdb_vlog_file_size` | On-disk size of the value log in bytes |
| `tidesdb_vlog_value_count` | Values the value log currently indexes |
| `tidesdb_vlog_used_bytes` | Uncompressed length those values represent |
| `tidesdb_vlog_bytes_written` | Lifetime bytes appended to the value log, output the flush and compaction counters do not see once values separate |

## Encoding

Aggregate codec-chain totals summed across every chain, so `logical` divided by `stored` is the realized compression ratio for each log. The per-chain codec breakdown prints in `SHOW ENGINE TIDESDB STATUS`.

| Variable | Description |
|----------|-------------|
| `tidesdb_klog_logical_bytes` | Key-log bytes before encoding, summed across chains |
| `tidesdb_klog_stored_bytes` | Key-log bytes after encoding as stored on disk |
| `tidesdb_vlog_encoded_logical_bytes` | Value-log bytes before encoding, summed across chains |
| `tidesdb_vlog_encoded_stored_bytes` | Value-log bytes after encoding as stored on disk |

## Device IO

Write accounting from the library's file-descriptor manager, which meters the SSTable and WAL devices. The value log keeps its own byte accounting in the value-log counters above. These counters are writes only, there is no read-side or syscall figure.

| Variable | Description |
|----------|-------------|
| `tidesdb_io_sstable_write_ops` | SSTable device writes issued since open |
| `tidesdb_io_sstable_write_bytes` | Bytes written to the SSTable device since open |
| `tidesdb_io_wal_write_ops` | WAL device writes issued since open |
| `tidesdb_io_wal_write_bytes` | Bytes written to the WAL device since open |

## Write amplification

| Variable | Description |
|----------|-------------|
| `tidesdb_user_bytes_written` | Logical committed bytes, the write-amplification denominator |
| `tidesdb_flush_bytes_written` | Bytes written to SSTables by flush jobs |
| `tidesdb_compaction_bytes_written` | Bytes written by compaction jobs |
| `tidesdb_compaction_bytes_read` | Bytes compaction read as input |
| `tidesdb_flush_count` | Flushes completed across all column families |
| `tidesdb_compaction_count` | Compactions completed across all column families |

## Write stalls

| Variable | Description |
|----------|-------------|
| `tidesdb_writes_throttled` | Commits the L0 admission policy made dwell before admitting |
| `tidesdb_writes_blocked` | Commits it made wait for the flush queue to drain |
| `tidesdb_write_stall_us` | Total microseconds commits spent held in admission |
| `tidesdb_write_stall_ceiling_hits` | Commits admitted only because the wait ceiling expired. Any sustained increase means flush is not keeping up with ingest |

The aggregate counters above sum admission stall time. These per-reason counts split how often a commit stalled by cause, so backpressure can be attributed. The per-reason stall time prints in `SHOW ENGINE TIDESDB STATUS`.

| Variable | Description |
|----------|-------------|
| `tidesdb_stall_wal_append` | Commits that stalled appending to the write-ahead log |
| `tidesdb_stall_rotate_lock` | Commits that stalled taking the memtable rotation lock |
| `tidesdb_stall_rotate_work` | Commits that stalled while a memtable rotation was in progress |
| `tidesdb_stall_admission` | Commits that stalled on the unflushed-backlog admission gate |
| `tidesdb_stall_manifest_commit` | Commits that stalled waiting for a manifest commit |

## Block cache

| Variable | Description |
|----------|-------------|
| `tidesdb_cache_entries` | Cached entry count |
| `tidesdb_cache_bytes` | Bytes used by the block cache |
| `tidesdb_cache_hits` | Cache hits since open |
| `tidesdb_cache_misses` | Cache misses since open |
| `tidesdb_cache_hit_rate` | Hit rate as a percentage |
| `tidesdb_cache_partitions` | Number of cache shards |

## Tombstones

| Variable | Description |
|----------|-------------|
| `tidesdb_total_tombstones` | Total tombstones summed across every column family |
| `tidesdb_tombstone_ratio` | Database-wide tombstone count divided by entry count, 0.0 to 1.0 |
| `tidesdb_max_sst_tombstone_density` | Worst single-SSTable tombstone density observed |
| `tidesdb_max_sst_tombstone_density_level` | 1-based LSM level where the worst SSTable sits |
