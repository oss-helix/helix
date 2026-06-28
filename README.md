# Helix

Distributed In-Memory State Runtime for High-Concurrency Applications.

Helix moves concurrency control from databases into an execution runtime.

## Why Helix?

Modern applications struggle with:

- database row locks
- transaction contention
- distributed locks
- ordering guarantees
- high write workloads

Helix provides a runtime layer that manages application state in memory.

```
Request
   |
Helix Runtime
   |
In-Memory State
   |
Replication
   |
Persistence
```

## Core Concepts

### Key-based Execution

Each state key owns an execution context.

Example:

```
order:100
```

is always processed by the same execution worker.

Guarantees:

- same key → sequential execution
- different keys → parallel execution

No distributed lock required.

---

## Architecture

```
            Client
              |
        Helix Runtime
              |
       +----------------+
       |   Event Loop   |
       +----------------+
              |
        State Engine
              |
         Replication
              |
         Persistence
```

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
- [x] Read-through TTL cache
- [x] Spring Boot + Kotlin reference client
- [x] Docker / docker-compose deployment

Planned:

- [ ] Leader / Follower replication
- [ ] Cluster membership
- [ ] Persistence adapters (Postgres / MySQL sinks)

---

## When to use Helix

Helix targets two problems that show up together in high-traffic backends:

**Write contention.** Multiple requests fight over the same row — seat
reservations, inventory decrements, balance updates, idempotency tokens.
The lease daemon serializes all writers for a given key, in submission
order, across every application instance. No DB row lock, no distributed
mutex library, no retry loop in application code.

**Read-heavy hotspots.** The same list / detail / discovery page hammered
by thousands of concurrent users. The TTL cache module serves repeated
reads from memory with per-entry expiry and cache-aside semantics, so
the DB sees one read per cache miss instead of one read per request.

Both run on the same daemon and share the same key space, so the
application gets a single coordination point for both problems.

---

## Design positioning

Helix is not:

- a database
- a message broker

Helix is:

> A distributed state execution runtime — per-key serialization for
> writes, TTL cache for reads, one daemon for both.

---

## Language

Helix is implemented in C.

Goals:

- predictable latency
- explicit memory management
- zero-copy where possible
- low overhead runtime

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

## Status

Early development.

APIs and architecture may change.

---

## License

Apache License 2.0
