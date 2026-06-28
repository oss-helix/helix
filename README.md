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

Helix is purpose-built for **write contention**. Reach for it when:

- Multiple requests fight over the same row (seat reservations, inventory
  decrements, balance updates, idempotency tokens).
- You'd otherwise reach for SELECT ... FOR UPDATE, Redis Redlock, or a
  per-row mutex spread across app instances.
- You want serialized writes per key but parallelism across keys, without
  paying a DB-lock round trip.

The TTL cache on top is a small read-side amenity for apps that already
run Helix for write contention — not a Redis replacement.

**Helix is not the right tool for:**

- Pure read-heavy caching of HTTP responses → use a CDN (Cloudflare Cache
  Rules, Fastly) or Redis with `@Cacheable`.
- Pub/sub fan-out → Kafka, Redis Pub/Sub, NATS.
- General-purpose KV / session store with large datasets → Redis.
- Full-text search, analytics, time series → use specialized stores.

If your bottleneck is "the list endpoint is slow", Helix is the wrong
answer. If your bottleneck is "two users both bought the last seat",
Helix is exactly the right answer.

---

## Design positioning

Helix is not:

- a database
- a message broker
- a Redis replacement

Helix is:

> A distributed state execution runtime with a per-key serialization
> guarantee.

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
