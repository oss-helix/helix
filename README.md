# Helix

> **A runtime layer that manages application state safely and efficiently.**

Helix is a lightweight state runtime that combines cache-speed reads,
safe per-key writes, atomic state mutation, request deduplication, and
durable recovery into one daemon.

See [`DIRECTION.md`](DIRECTION.md) for the full project direction —
problem framing, target users, feature set, roadmap, and long-term
vision.

```
                Application
                     |
                     v
                   Helix
   ┌─────────────────────────────────┐
   │  Read Path                      │
   │    In-memory state              │
   │    TTL / cache                  │
   ├─────────────────────────────────┤
   │  Write Path                     │
   │    Event loop                   │
   │    Single writer per key        │
   │    Submission-order ordering    │
   └─────────────────────────────────┘
                     |
                     v
                Persistence
```

## What Helix replaces

A typical small-to-mid backend looks like this:

```
Spring + Redis + DB
```

Engineers spend time on:

- cache key management
- TTL / eviction policies
- distributed lock implementation
- concurrency control across instances
- idempotency / dedup plumbing

With Helix the stack collapses to:

```
Spring + Helix + DB
```

The application reaches for two operations:

```kotlin
helix.put(key, value, ttl)         // read path — fast in-memory access
helix.execute(key) { ... }         // write path — per-key serialization
```

Read path is a memory lookup. Write path is a queued, single-threaded
execution per key — no DB row lock, no distributed mutex library, no
retry loop in application code.

## Design philosophy

The two paths are unified at the daemon boundary but distinct internally:

- **Cache** is a performance optimization. Stale-but-fast reads.
  Bucketed concurrent map, TTL eviction, no ordering guarantees.
- **State execution** is a correctness guarantee. Submission-order
  per-key serialization, exactly one writer at a time, durable via
  the write-ahead log.

Mixing the two as a single primitive invites subtle consistency bugs
under load. They share the same key space and the same daemon but the
underlying mechanics stay separate.

Helix is not a relational database, not a message broker, and not a
Redis replacement. It is a runtime layer for application state.

## Why this shape

The big-co stack — Kafka + Redis + ZooKeeper + Postgres + a row-lock
library — is already built. Helix isn't competing there.

Helix targets the **smaller service** that has:

- one or two app servers
- a Postgres / MySQL
- maybe a Redis used as a cache

and is starting to see hot rows, inventory races, double-bookings, or
duplicate-request bugs. Adding ZooKeeper + Redlock + custom Lua scripts
is a heavy step. Adding Helix is one container and two API methods.

---

## Features

Implemented:

- [x] Event loop execution
- [x] Key-based routing
- [x] In-memory state engine
- [x] Write-ahead log (CRC32, three fsync policies)
- [x] Snapshot writer + WAL rotation
- [x] Crash recovery (snapshot → WAL replay)
- [x] HTTP lease daemon (per-key serialization over the wire)
- [x] Read-through TTL cache (ephemeral or WAL-persistent)
- [x] Idempotency endpoint (`POST /v1/idempotent`)
- [x] Atomic ops (`POST /v1/atomic/{incr,cas}`)
- [x] Prometheus metrics (`GET /v1/metrics`)
- [x] Graceful shutdown / drain mode on SIGTERM
- [x] Spring Boot + Kotlin reference client (lease / cache / idempotent / atomic)
- [x] Docker / docker-compose deployment

Planned:

- [ ] Leader / Follower replication
- [ ] Cluster membership
- [ ] Persistence adapters (Postgres / MySQL sinks)

---

## Docker quick start

The repo ships a `docker-compose.yml` that runs the lease daemon and the
Spring Boot demo together. No local C toolchain or JDK required:

```sh
docker compose up --build
```

Then load-test:

```sh
seq 1 200 | xargs -P 50 -I{} curl -s -o /dev/null -w '%{http_code}\n' \
  -X POST localhost:8081/seats/VIP-001/reserve \
  -H 'Content-Type: application/json' \
  -d '{"userId":"user-{}"}' | sort | uniq -c
# 1 200
# 199 409
```

Exactly one winner — the other 199 lost the lease race deterministically.
See `examples/spring-kotlin/README.md` for the full walkthrough.

---

## Language

Helix is implemented in C.

Goals:

- predictable latency
- explicit memory management
- zero-copy where possible
- low overhead runtime

---

## Status

Early development.

APIs and architecture may change.

---

## License

Apache License 2.0
