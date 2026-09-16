---
title: Replication and High Availability
description: How TideSQL participates in replication, what makes a replica crash-safe, and what high availability looks like without multi-primary clustering.
---

# Replication and High Availability

TideSQL stores data on one node. Replication comes from the layers above the engine, and within
those layers TideSQL is a first-class participant rather than a generic pass-through: it carries
the binlog capabilities replication needs, and it commits through the two-phase protocol that keeps
a replica crash-safe.

## Replication

A TideSQL table replicates like any other. The engine is both row and statement binlog capable, so
`binlog_format` of `ROW`, `STATEMENT`, or `MIXED` all work, with row-based replication the natural
fit for an optimistic engine because it ships the resulting row images rather than re-running a
statement whose outcome depends on commit ordering.

Commits go through the binlog two-phase protocol. The engine prepares durably before the binlog
writes, so a crash between the binlog and the engine recovers to a consistent point and a replica
built from that binlog is consistent with the source. Commits are ordered by the server's own
group-commit machinery, which also amortizes the durability cost across the transactions that
commit together — see [Transactions and Isolation](/concepts/transactions).

Nothing engine-specific has to be enabled. A table created with `ENGINE=TIDESDB` on the source
replicates to a replica running the same engine through the standard replication configuration.

GTIDs, semi-synchronous replication, delayed replicas and multi-source replication are all
properties of the replication layer rather than of the engine, and none of them require anything
from TideSQL beyond what it already provides.

## Crash safety on a replica

A replica applies its relay log through the same two-phase commit path. If it dies mid-apply, the
engine's prepared-but-undecided transactions are recovered at startup and resolved against the
binlog, so the replica resumes at a consistent position rather than with a half-applied
transaction. [Durability and Sync Modes](/concepts/durability) covers what each sync mode
guarantees at that boundary, and the recovery mechanism itself is described under
[Transactions and Isolation](/concepts/transactions).

## Multi-primary clustering

There is no multi-primary clustering for TideSQL on this server. The server carries no write-set
replication integration of any kind — no header to include and no engine hook to register — so
there is nothing for the engine to participate in, and the translation unit that implements that
participation is excluded from the build here rather than compiled to no effect.

What this rules out is a cluster in which several nodes accept writes to the same rows and certify
them against one another. What it does not rule out is high availability: a source with one or more
replicas, promoted on failure by whatever orchestration you already run, works normally and is the
supported shape.

## What an application sees under conflict

An optimistic write-write conflict is resolved locally, at commit, on the node that took the write.
The losing transaction is rolled back and the application retries it. The server does not retry it
for you, so any application writing concurrently to the same rows needs that retry — the exact
error and the reasoning behind it are in
[Transactions and Isolation](/concepts/transactions), and the operational consequences in
[Limitations](/appendix/limitations).

Because writes are accepted on one node, there is no cross-node conflict to resolve: a replica
applies what the source committed, in the order the source committed it.
