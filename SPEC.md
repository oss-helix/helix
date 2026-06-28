# Helix Specification

## 1. Overview

Helix is a distributed in-memory state runtime designed for high-concurrency
systems.

The goal of Helix is to move concurrency control from the database layer to
the runtime layer.

Traditional architecture:

```
Request
   |
Application
   |
Database Lock / Transaction
   |
Storage
```

Helix architecture:

```
Request
   |
Helix Runtime
   |
In-Memory State Engine
   |
Replication
   |
Persistence Layer
```

Helix manages:

- state execution
- concurrency control
- ordering
- replication
- recovery

---

## 2. Core Philosophy

The database should not be the primary concurrency coordinator.

**Database responsibilities:**

- durability
- recovery
- long-term storage

**Helix responsibilities:**

- state mutation
- ordering
- concurrent execution
- consistency

---

## 3. Core Components

### 3.1 Event Loop Engine

Helix uses an event-loop-based execution model. Each state key is assigned
to an execution context.

```
key = user:100
   |
   v
EventLoop-3
   |
   v
Sequential execution
```

Properties:

- no global lock
- single writer per key
- deterministic execution order

### 3.2 State Engine

Helix maintains application state in memory.

```
State Store
   order:100
   { status: PAID, amount: 10000 }
```

Memory is the active state. Database persistence is asynchronous.

### 3.3 Key Routing

Every operation must provide a routing key.

```
order-100
seat-A1
inventory-10
```

Routing:

```
hash(key)
   |
Execution Worker
```

Guarantee — for the same key:

```
A
B
C
```

executes as `A -> B -> C`. Different keys execute concurrently.

---

## 4. Replication Model

Helix uses leader/follower replication.

```
              Cluster

               Leader
                 |
       +---------+---------+
       |                   |
    Replica             Replica
```

Only the leader accepts writes.

Flow:

```
Client
  |
Leader
  |
Memory Update
  |
Replication
  |
Follower ACK
  |
Commit
```

---

## 5. Log System

Memory alone cannot guarantee recovery. Helix uses append-only logs.

```
State Change
   |
Append Log
   |
Replication
   |
Commit Offset
```

Recovery:

```
Snapshot + Log Replay = State Restore
```

---

## 6. Persistence

Persistence is decoupled.

Options:

- snapshot
- write-ahead log
- external database

```
Memory
   |
Async Flush
   |
Database
```

---

## 7. Failure Recovery

When the leader fails:

```
Leader Down
   |
Replica Election
   |
New Leader
   |
Continue Processing
```

Requirements:

- no lost committed state
- prevent split brain
- maintain ordering

---

## 8. C Implementation Architecture

```
helix/
  src/
    core/
      runtime.c
      state.c
      hashmap.c
    eventloop/
      event_loop.c
      queue.c
    cluster/
      leader.c
      replica.c
      election.c
    storage/
      wal.c
      snapshot.c
      recovery.c
    network/
      protocol.c
      transport.c
```

Phase-1 source code lives in `core/`, `eventloop/`, and `storage/`.
`cluster/` and `network/` arrive in Phase 2.

---

## 9. Memory Management

Requirements:

- zero-copy where possible
- explicit allocation control
- memory pool
- configurable eviction

No garbage collector dependency.

---

## 10. API Concept

```c
helix_runtime_t *runtime;

helix_execute(runtime, "order-100", update_order, args);
```

Execution guarantee:

- same key → sequential
- different key → parallel

---

## 11. Non-Goals

Helix is not:

- a relational database
- a message broker
- a cache replacement

Helix is: **a distributed state execution runtime.**

---

## 12. Roadmap

- **v0.1** — event loop, key routing, in-memory state
- **v0.2** — WAL, snapshot, recovery
- **v0.3** — replication, leader election
- **v1.0** — production distributed state runtime
