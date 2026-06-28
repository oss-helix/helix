# Helix Specification

## Overview

Helix is a distributed in-memory state runtime.

The purpose is to provide high performance state execution with deterministic
concurrency control.

---

## Two-path architecture

Helix exposes two operational paths from one daemon. They share a key
space and a process boundary; their internal mechanics are deliberately
distinct.

### Read path — cache

- Purpose: **performance optimization**
- Storage: bucketed concurrent hashmap, lock per bucket
- Semantics: stale-but-fast. TTL-based eviction, lazy on read.
- No ordering guarantees across writers; last-writer-wins.
- Endpoints: `GET / PUT / DELETE /v1/cache/{key}`

### Write path — state execution

- Purpose: **correctness guarantee**
- Storage: per-worker hashmap shards owned by a single thread
- Semantics: submission-order per-key serialization, exactly one
  writer at a time. Durable via write-ahead log.
- Endpoints: `POST /v1/lease`, `POST /v1/release`, and the underlying
  `helix_execute` C API.

### Why they stay separate

A single primitive that tries to be both fast (skip ordering) and
correct (enforce ordering) produces subtle bugs under load — for
example, a cache value that's stale w.r.t. an in-flight serialized
write. Keeping the two paths as independent subsystems lets the
application choose, per call, which guarantee it needs.

The shared key space lets the application invalidate cache entries
from the write path (`POST /v1/release` callers typically delete the
matching cache entry), but the two subsystems do not implicitly
synchronize with each other.

---

## Execution Model

Helix uses event-loop based execution.

Each state key is assigned to a worker.

Example:

```
hash(key)
   |
worker
```

The worker executes operations sequentially.

---

## Concurrency Model

Guarantee:

Same key:

```
A
B
C
```

executes:

```
A -> B -> C
```

Different keys:

```
order-1
order-2
order-3
```

can execute concurrently.

---

## State Model

Memory is the active state.

Example:

```
user:100
{
  balance: 1000,
  status: active
}
```

Database persistence is separated.

---

## Replication

Helix uses leader/follower replication.

```
   Leader
     |
+----+----+
|         |
Follower  Follower
```

Leader handles writes.

Followers maintain replicated state.

---

## Commit

State changes are committed after replication.

```
State Update
     |
Replication
     |
  Commit
     |
 Response
```

---

## Recovery

Helix uses:

- append log
- snapshot

Recovery:

```
Snapshot + Log Replay = State Restore
```

---

## Non-Goals

Helix is not intended to replace:

- SQL databases
- Kafka
- Redis

It provides an execution layer above storage systems.
