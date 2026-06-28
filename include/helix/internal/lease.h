/*
 * Lease registry — the bridge between an external client (e.g. a Spring app
 * over HTTP) and the Helix runtime.
 *
 * Programming model from the client's perspective:
 *
 *     id = lease_acquire("seat-A1", ttl_ms=30000)   // blocks until our turn
 *     ... do work that mutates seat-A1 ...
 *     lease_release(id)
 *
 * Internally, `lease_acquire` submits a long-running handler to the worker
 * that owns the key. The handler signals "you have the slot" once it starts,
 * then parks on a condvar until either `lease_release` is called or the TTL
 * fires. Because the handler runs on the worker for that key, the runtime's
 * single-thread-per-key invariant gives FIFO ordering across all clients
 * competing for the same key.
 */
#ifndef HELIX_INTERNAL_LEASE_H
#define HELIX_INTERNAL_LEASE_H

#include "helix/helix.h"

#include <stddef.h>

typedef struct lease_registry lease_registry_t;

lease_registry_t *lease_registry_create(helix_runtime_t *rt);
void              lease_registry_destroy(lease_registry_t *r);

/* Acquire a lease for `key`. Blocks until this caller is the active owner
 * of the key, or until `ttl_ms` elapses while waiting (returns NULL).
 * The caller must call `lease_release` with the returned id.
 *
 * Once acquired, the TTL re-applies as a max-hold time: if `lease_release`
 * isn't called within `ttl_ms` of acquisition, the runtime releases the slot
 * itself and a later `lease_release` for that id will return -1.
 *
 * Returned id is heap-allocated, caller frees with free(). */
char *lease_acquire(lease_registry_t *r, const char *key, int ttl_ms);

/* Release `lease_id`. Returns 0 if the lease was active and released, -1 if
 * unknown or already expired. */
int lease_release(lease_registry_t *r, const char *lease_id);

/* Number of leases currently held by clients. */
size_t lease_active_count(lease_registry_t *r);

/* Number of /v1/lease callers currently parked waiting for their turn. */
size_t lease_pending_count(lease_registry_t *r);

#endif
