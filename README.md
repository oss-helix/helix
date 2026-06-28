# Helix

**Distributed In-Memory State Runtime for High-Concurrency Applications**

Helix moves application state out of the database and into the runtime.
The database becomes a persistence layer; state lives in memory, replicated
across nodes, with an append-only log for durability.

```
Client → Runtime → Distributed Memory → Database (Persistence Only)
```

Built in C for maximum throughput and predictable latency.

## Why

Most backend systems are built around the database. As concurrency grows,
the bottleneck is always the same:

- Row locks
- Transactions
- Connection pools
- Optimistic-lock retries
- Deadlocks
- Hot rows

Kafka, Redis, and message queues soften the problem but do not remove the
database from the critical path. Helix does.

## Programming model

A key is owned by exactly one worker thread at a time. The runtime guarantees
serial execution per key — no locks, no transactions, no retries in user code.

```c
helix_runtime_t *rt = helix_runtime_create(&cfg);

helix_execute(rt, "order-42", pay_order, &args);  // serial w.r.t. "order-42"
helix_execute(rt, "order-42", cancel_order, &args);
helix_execute(rt, "order-42", refund_order, &args);
```

## What the runtime owns

- Routing (hash(key) → worker)
- Ordering (single thread per key)
- Concurrency (lock-free per-worker queues)
- Write-ahead log
- Snapshots
- Recovery (snapshot + log replay)
- Async persistence
- Replication interface (Phase 2)

The application writes business logic only.

## Status

Phase 1 (single-node core) — in progress. See `docs/ARCHITECTURE.md` and
`docs/ROADMAP.md`.

## Build

```sh
make
make test
make example
```

Requires a C11 compiler and POSIX threads. Tested on macOS and Linux.

## License

Apache License 2.0. See [LICENSE](LICENSE).
