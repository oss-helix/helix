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

/* Stats — best-effort, no global lock. */
size_t hx_cache_size(hx_cache_t *c);
size_t hx_cache_hits(hx_cache_t *c);
size_t hx_cache_misses(hx_cache_t *c);

#endif
