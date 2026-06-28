# Spring Boot + Helix: per-seat lease

A small Spring Boot Kotlin app that uses the Helix daemon as a per-key
serialization service.

**Scenario:** ticketing. Many concurrent clients race for the same seat.
Without coordination, a check-then-set on the seat record produces double
bookings. Without Helix you would normally reach for a DB row lock or a
distributed lock (Redis Redlock, ZooKeeper, etcd). Here, the runtime layer
itself owns the serialization — the DB sees no contention.

```
Client                Spring App                  Helix Daemon
  |                       |                            |
  | POST /seats/A1/reserve|                            |
  |---------------------->|                            |
  |                       | POST /v1/lease {seat-A1}   |
  |                       |--------------------------->|
  |                       |          (queues this      |
  |                       |           caller behind    |
  |                       |           any earlier      |
  |                       |           seat-A1 callers) |
  |                       | 200 {lease: L7, ...}       |
  |                       |<---------------------------|
  |                       |                            |
  |                       | repo.reserve("A1", user)   |   <-- single-threaded
  |                       |                            |       for this key
  |                       | POST /v1/release {L7}      |
  |                       |--------------------------->|
  |                       | 200 OK                     |
  |                       |<---------------------------|
  | 200 Seat              |                            |
  |<----------------------|                            |
```

Concurrent requests for **the same seat** all hash to the same Helix worker
and are processed strictly in order. The first caller wins; the rest see
`reserved=true` and the controller returns `409 Conflict`.

Concurrent requests for **different seats** hash to different workers and
run in parallel — there is no cross-key contention.

## Running

### Option 1 — Docker (zero local deps)

From the repo root:

```sh
docker compose up --build
```

Skip to **§3 Try a single reservation** below.

### Option 2 — Native

You need:

- A C toolchain to build the Helix daemon (`cc`, `make`, POSIX threads)
- JDK 17+ (uncomment `spring.threads.virtual` in `application.yml` for JDK 21+)
- Gradle 8.5+

### 1. Start the Helix daemon

```sh
cd ../..              # repo root
make example
./build/example_lease_server 9099 64    # port 9099, 64 workers
```

You should see:

```
helix lease daemon listening on :9099 (workers=64)
```

### 2. Start the Spring app

```sh
cd examples/spring-kotlin
gradle bootRun
```

It binds to `:8081` by default and points at `http://localhost:9099` for
Helix. Override with `--helix.daemon-url=...`.

### 3. Try a single reservation

```sh
curl -s -X POST localhost:8081/seats/VIP-001/reserve \
  -H 'Content-Type: application/json' \
  -d '{"userId":"alice"}'
# {"seat":{"id":"VIP-001","reserved":true,"reservedBy":"alice"},"waitMs":3}
```

A second attempt by a different user comes back as `409`:

```sh
curl -s -w '%{http_code}\n' -o /dev/null \
  -X POST localhost:8081/seats/VIP-001/reserve \
  -H 'Content-Type: application/json' \
  -d '{"userId":"bob"}'
# 409
```

### 4. Read-side caching (cache-aside)

The same Spring app uses `HelixCacheClient` for read-heavy endpoints:

```sh
curl -s localhost:8081/seats/VIP-001       # miss — fetched from repo, populated
curl -s localhost:8081/seats/VIP-001       # hit  — served from Helix in-memory cache
curl -s localhost:8081/seats                # list endpoint, same pattern
```

Stats:

```sh
curl -s http://localhost:9099/v1/stats
# {"active":0,"pending":0,"cache":{"size":2,"hits":2,"misses":2}}
```

The reservation endpoint invalidates the affected cache entries on success so
the next read observes the new state. TTLs default to 5 seconds; tune per
endpoint.

This is the standard cache-aside pattern — useful for "list" / "detail" page
queries that hit the same DB rows repeatedly under load. For write contention,
keep using the lease pattern (§5).

### 5. Concurrent load — 200 buyers on one seat

Reset and fire 200 reservations at the same seat in parallel:

```sh
curl -s -X POST localhost:8081/seats/reset >/dev/null

seq 1 200 | xargs -P 200 -I{} curl -s -o /dev/null -w '%{http_code}\n' \
  -X POST localhost:8081/seats/VIP-001/reserve \
  -H 'Content-Type: application/json' \
  -d '{"userId":"user-{}"}' | sort | uniq -c
```

Expected:

```
   1 200
 199 409
```

Exactly one winner. The other 199 lost the lease race deterministically —
they were behind in the Helix worker's FIFO and saw `reserved=true` by the
time they ran.

### 6. Concurrent load — 200 buyers across 50 seats

```sh
curl -s -X POST localhost:8081/seats/reset >/dev/null

seq 1 200 | xargs -P 200 -I{} sh -c '
  SEAT=$(( $1 % 50 ))
  curl -s -o /dev/null -w "%{http_code}\n" \
    -X POST localhost:8081/seats/$SEAT/reserve \
    -H "Content-Type: application/json" \
    -d "{\"userId\":\"user-$1\"}"
' _ {} | sort | uniq -c
```

Expected:

```
  50 200
 150 409
```

50 seats, 50 winners. Each seat had 4 contenders; one of each won. Different
seats ran in parallel across Helix workers.

## Why not just synchronize in the JVM?

Two reasons:

1. **Multiple Spring instances.** Scale the app horizontally and a
   `synchronized` block or in-process `Mutex` no longer serializes across
   instances. The Helix daemon is a single coordination point.
2. **The DB is still the bottleneck.** Even with a distributed lock, N app
   instances issue N writes that the DB serializes via row locks. Helix
   serializes at the runtime layer — at most one app instance, anywhere,
   holds the lease for a given seat at any moment, so the DB never sees a
   contended write for that row.

## What the code does, in one paragraph

`SeatController.reserve` calls `HelixLeaseClient.withLease("seat-$id") { ... }`.
That wrapper POSTs to `/v1/lease` on the daemon, which submits a park-task
to the runtime worker that owns `"seat-$id"`. Workers process tasks
strictly FIFO per key. When this caller's task is dequeued, the daemon
returns the lease ID; the wrapper runs the user block; on the way out it
POSTs `/v1/release`, which frees the worker for the next caller in line.
No DB row lock, no distributed lock library, no application-level
synchronization.
