# Helix Direction

## Overview

Helix is a lightweight state runtime that combines:

- cache-speed reads
- safe per-key writes
- atomic state mutation
- request deduplication
- durable recovery

Helix reduces the need for applications to manually combine:

- Redis cache
- distributed lock
- idempotency table
- atomic counter
- retry logic
- hot row mitigation

into every service.

---

## Problem

Many backend services start simple:

```
Application
   |
PostgreSQL / MySQL
   |
Optional Redis
```

As traffic grows, developers introduce:

```
Redis Cache
Redis Lock
Database Transaction
Unique Constraint
Retry Logic
Idempotency Table
Atomic Update
```

The complexity moves into application code.

Common problems:

- hot row contention
- duplicate requests
- race conditions
- cache invalidation
- inconsistent counters
- lock timeout
- unnecessary database load

---

## Helix Philosophy

Helix moves concurrency control from storage layer to runtime layer.

Traditional:

```
Request
   |
Application
   |
Database Lock
   |
Storage
```

Helix:

```
Request
   |
Helix Runtime
   |
State Execution
   |
Persistence
```

The runtime owns state transitions.

---

## Core Model

### Read Path

Fast access through memory state.

```
Client
   |
Memory State
   |
Response
```

Features:

- TTL expiration
- hot state caching
- persistent cache recovery
- low latency reads

### Write Path

Writes are executed through ordered state transitions.

Example:

```
key = user:100
```

All mutations:

```
A
B
C
```

execute:

```
A -> B -> C
```

Different keys:

```
user:100
user:200
user:300
```

can execute concurrently.

---

## Key-Based Execution

Helix uses per-key serialization.

Instead of:

```
Thread A
   |
 lock
   |
update

Thread B
   |
 wait
```

Helix:

```
key:user:100
   |
Event Loop
   |
update
update
update
```

Benefits:

- no distributed lock
- deterministic ordering
- reduced database contention

---

## Feature Set

### 1. In-Memory State Engine

Provides runtime-managed state.

```
State
{
  count: 10,
  status: ACTIVE
}
```

The active state lives in memory. Database persistence is separated.

### 2. TTL State

Temporary state support.

Examples:

- cache
- sessions
- rate limit
- temporary tokens

```
user:session
expire: 30s
```

### 3. Atomic Operations

Built-in atomic mutations.

Supported:

```
INCR
DECR
CAS
UPDATE
```

Example:

```
unread_count++
```

without external locking.

### 4. Idempotency

Prevent duplicate execution.

Example:

```
POST /payment
request-id: abc123
```

Helix guarantees:

```
abc123 → execute once
```

Use cases:

- payment
- message sending
- friend request
- notification

### 5. Write-Ahead Log (WAL)

State changes are recorded before completion.

```
Mutation
   |
  WAL
   |
Memory Update
   |
 Commit
```

Provides:

- recovery
- durability
- crash safety

### 6. Snapshot Recovery

Memory state can be restored.

```
Snapshot + WAL Replay = Recovered State
```

### 7. Metrics

Built-in observability.

Provides:

- request count
- latency
- state size
- cache hit ratio
- queue depth
- errors

Compatible with Prometheus.

### 8. Graceful Shutdown

Safe deployment support.

Shutdown flow:

```
SIGTERM
   |
Stop accepting new writes
   |
Drain pending events
   |
Persist state
   |
Shutdown
```

---

## Target Users

Helix targets services that have:

```
1~2 application servers
PostgreSQL / MySQL
Optional Redis
```

and start experiencing:

- hot rows
- duplicate requests
- counter races
- cache complexity

---

## Example Use Cases

### Social Service

```
like_count++
unread_count++
notification_state
```

### Reservation

```
seat:A1
  reserve()
  cancel()
```

### Inventory

```
stock:item-1
  decrease()
  restore()
```

### Rate Limit

```
user:100
  request++
  TTL
```

---

## What Helix Is Not

Helix is not:

- a relational database
- a message broker
- a Redis replacement

Helix is:

> A runtime layer that manages application state safely and efficiently.

---

## Roadmap

### v0.x

- Event loop
- State engine
- TTL
- Atomic operations
- Idempotency
- WAL
- Snapshot
- Metrics

### v1.0

- Stable protocol
- Production deployment model
- Cluster replication
- High availability
- Client SDK ecosystem

---

## Long Term Vision

Helix aims to become the default state execution layer for small and
medium scale services.

Instead of assembling:

```
Redis
Lock
Transaction
Retry
Idempotency
```

developers use:

```
Helix Runtime
```
