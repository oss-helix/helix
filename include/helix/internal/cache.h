/*
 * Read-through TTL cache.
 *
 * Separate concern from the per-key worker shards used by the lease /
 * execute path. The cache is a flat bucketed concurrent hashmap: any
 * thread can `get` / `set` / `delete` without going through the event
 * loop, so it's suitable for read-heavy workloads where serialization
 * would only add latency.
 *
 * Concurrency: N buckets, each with its own mutex + a private hashmap.
 * `hx_hash64(key) % N` picks the bucket. With N = 64 (default) and well-
 * distributed keys, the lock fights for a given operation only touch
 * roughly 1/64 of all callers.
 *
 * Expiration: lazy. Every `hx_cache_get` checks `expires_at_ms` against
 * the current monotonic clock; expired entries are removed inline. No
 * background reaper for the MVP — callers that never read a key still
 * leave the bytes resident, but a periodic sweep can be added later
 * without changing the API.
 */
#ifndef HELIX_INTERNAL_CACHE_H
#define HELIX_INTERNAL_CACHE_H

#include <stddef.h>
#include <stdint.h>

typedef struct hx_cache hx_cache_t;

/* Creates a cache with `bucket_count` shards. Pass 0 for the default (64). */
hx_cache_t *hx_cache_create(size_t bucket_count);

/* Persistent variant. Replays `wal_path` into memory at startup, then opens
 * the file for append. Subsequent set/delete operations are logged. Entries
 * carry absolute (wall-clock) expiry, so a record whose TTL has passed
 * while the daemon was down is skipped on replay. */
#include "helix/helix.h"   /* helix_wal_mode_t */
hx_cache_t *hx_cache_create_persistent(size_t bucket_count,
                                       const char *wal_path,
                                       helix_wal_mode_t mode,
                                       size_t batch_size);

void        hx_cache_destroy(hx_cache_t *c);

/* Looks up `key`. On hit returns a fresh heap-allocated copy of the value
 * and writes its length to *out_len; caller frees with free(). On miss
 * (absent or expired) returns NULL and *out_len = 0. */
void *hx_cache_get(hx_cache_t *c, const char *key, size_t *out_len);

/* Inserts (or replaces) `key` -> `value` with the given TTL in ms.
 * ttl_ms == 0 means "no expiry". The cache takes a copy of the bytes;
 * caller retains ownership of its buffer. Returns 0 on success, -1 on
 * allocation failure. */
int hx_cache_set(hx_cache_t *c, const char *key,
                 const void *value, size_t len, int ttl_ms);

/* Removes `key`. Returns 0 if it was present, -1 if absent. */
int hx_cache_delete(hx_cache_t *c, const char *key);

/* Atomic get-or-set. If `key` is present and not expired, returns its
 * value (heap-allocated copy, caller frees) and sets *was_new = 0.
 * Otherwise installs the provided value with the given TTL and returns
 * NULL with *was_new = 1.
 *
 * The bucket mutex is held across the get + set, so two concurrent
 * callers with the same key cannot both observe a miss and both insert.
 * This is the primitive that backs the idempotency endpoint. */
void *hx_cache_get_or_set(hx_cache_t *c, const char *key,
                          const void *value, size_t len, int ttl_ms,
                          size_t *out_len, int *was_new);

/* Atomic increment. Reads the current value as a signed int64 (parsed
 * from decimal text), adds `delta`, writes it back as text with the
 * given TTL, and returns the new value via *out_value. If the key is
 * missing the initial value is 0. Returns 0 on success, -1 if the
 * existing value is not a valid integer. */
int hx_cache_atomic_incr(hx_cache_t *c, const char *key,
                         long long delta, int ttl_ms,
                         long long *out_value);

/* Compare-and-swap. If the current value matches `expected` byte-for-byte,
 * replaces it with `next` (with the given TTL) and sets *swapped = 1.
 * Otherwise leaves the value alone and sets *swapped = 0. Missing keys
 * match an empty `expected` (len 0) — this lets a caller atomically
 * "set if absent". Returns 0 always (the *swapped flag carries the
 * outcome). */
int hx_cache_atomic_cas(hx_cache_t *c, const char *key,
                        const void *expected, size_t expected_len,
                        const void *next, size_t next_len,
                        int ttl_ms, int *swapped);

/* Stats — best-effort, no global lock. */
size_t hx_cache_size(hx_cache_t *c);
size_t hx_cache_hits(hx_cache_t *c);
size_t hx_cache_misses(hx_cache_t *c);

#endif
