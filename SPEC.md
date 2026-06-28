# Helix Specification

## Overview

Helix is a distributed in-memory state runtime.

The purpose is to provide high performance state execution with deterministic
concurrency control.

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
