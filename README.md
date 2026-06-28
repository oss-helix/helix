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

Current:

- [ ] Event loop execution
- [ ] Key based routing
- [ ] In-memory state engine

Planned:

- [ ] WAL
- [ ] Snapshot
- [ ] Leader / Follower replication
- [ ] Recovery
- [ ] Cluster membership
- [ ] Persistence adapters

---

## Design Goal

Helix is not:

- a database
- a message broker
- a cache

Helix is:

> A distributed state execution runtime.

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
