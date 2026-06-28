# cluster/

Leader election, replica management, and failover.
Lands in Phase 2 (see SPEC.md §4 and §7).

Planned modules:
- `leader.c` — log shipping, commit index, quorum tracking
- `replica.c` — follower-side log apply
- `election.c` — leader election protocol
