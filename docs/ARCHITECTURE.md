# Helix Architecture

## Layers

```
+----------------------------------------------------+
|                  Application (C API)               |
+----------------------------------------------------+
|                Programming Model                   |
|   helix_execute(key, fn, args)  — serial per key   |
+----------------------------------------------------+
|                Routing                             |
|   hash(key) % worker_count -> worker N             |
+----------------------------------------------------+
|                Event Loop                          |
|   per-worker MPSC queue, single-thread executor    |
+----------------------------------------------------+
|                State                               |
|   per-worker hashmap shard (no synchronization)    |
+----------------------------------------------------+
|                Durability                          |
|   append-only WAL + periodic snapshot              |
+----------------------------------------------------+
|                Replication  (Phase 2)              |
|   leader → followers, quorum ack, log shipping     |
+----------------------------------------------------+
|                Persistence sink   (optional)       |
|   async flush to RDBMS / blob store                |
+----------------------------------------------------+
```

## Single-thread-per-key invariant

Every key is owned by exactly one worker. The owning worker is computed
deterministically:

```
owner(key) = hx_hash64(key) % runtime->worker_count
```

A key never migrates between workers. The state shard for `key` therefore
lives in exactly one worker's hashmap and is mutated only by that worker's
thread. This eliminates locks on state access and gives the system its
strong ordering property:

> Any two calls submitted for the same key are executed in submission order.

## Submission

`helix_execute` is non-blocking. The router computes the worker, then enqueues
`(handler, args, key)` onto that worker's MPSC queue. Producers are any
threads; consumer is the worker. Backpressure is signaled by a non-zero
return when the queue is full.

`helix_execute_sync` enqueues a wrapper that signals a condvar after running
the user's handler. The submitter blocks on the condvar.

## State lifecycle

A state entry is created lazily on first reference inside a handler
(`hx_hashmap_get_or_create`). The application installs its value with
`helix_state_set`; the runtime calls the registered `free_fn` when:

- The application installs a new value over an old one, or
- The runtime is destroyed.

The state pointer is opaque to the runtime. WAL/snapshot serialize an
application-provided byte representation; see `docs/WAL.md` (Phase 1.5).

## Durability flow

```
handler returns
   |
   v
WAL append (handler op + serialized state)
   |
   v
WAL fsync per policy
   |
   v
ack to caller (sync) / fire-and-forget (async)
   |
   v
snapshot writer (background, periodic)
```

Recovery:

1. Load latest snapshot into worker shards (round-robin restore by key hash).
2. Replay WAL records past the snapshot's commit offset.
3. Open WAL for append.

## Replication (Phase 2)

Each WAL append is shipped to followers. The leader holds the commit
index; an entry is committed once a quorum of followers ack the append.
Failover promotes the most-up-to-date follower.

The Phase 1 codebase exposes the WAL interface in a way that lets Phase 2
intercept appends without touching the event loop.
