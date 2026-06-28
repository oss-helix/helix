# Roadmap

## Phase 1 — Single-node core (in progress)

- [x] Public C API (`helix.h`)
- [x] Open-addressing hashmap
- [x] Runtime lifecycle (create / destroy)
- [ ] MPSC worker queues
- [ ] Event-loop worker threads with key routing
- [ ] Synchronous + async submission
- [ ] Append-only WAL with CRC32 + segment rotation
- [ ] Snapshot writer
- [ ] Recovery (snapshot + WAL replay)
- [ ] Order-aggregate example
- [ ] Unit tests

## Phase 2 — Replication

- [ ] Leader/follower transport (TCP framing)
- [ ] Log shipping with quorum ack
- [ ] Commit index propagation
- [ ] Manual failover

## Phase 3 — Cluster

- [ ] Raft-based leader election
- [ ] Membership changes
- [ ] Split-brain protection
- [ ] Automatic failover

## Phase 4 — Persistence sinks

- [ ] Pluggable async flush hooks
- [ ] Built-in adapters (Postgres, MySQL)
- [ ] At-least-once / idempotent flush
