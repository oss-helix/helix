/*
 * Open-addressing hashmap, string keys, void* values.
 * Not thread-safe. One instance per worker (worker owns its shard).
 */
#ifndef HELIX_INTERNAL_HASHMAP_H
#define HELIX_INTERNAL_HASHMAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct hx_hashmap hx_hashmap_t;

typedef struct {
    const char *key;
    void       *value;
    size_t      value_size;
    void      (*free_fn)(void *);
} hx_entry_t;

hx_hashmap_t *hx_hashmap_create(size_t initial_capacity);
void          hx_hashmap_destroy(hx_hashmap_t *m);

/* Returns the entry for `key`, creating it if missing. value is NULL on creation. */
hx_entry_t *hx_hashmap_get_or_create(hx_hashmap_t *m, const char *key);

/* Returns the entry for `key`, or NULL. */
hx_entry_t *hx_hashmap_get(hx_hashmap_t *m, const char *key);

/* Drops the entry, invoking free_fn on its value if set. */
void hx_hashmap_remove(hx_hashmap_t *m, const char *key);

size_t hx_hashmap_size(const hx_hashmap_t *m);

/* Iterates all live entries. `ctx` is passed through. */
typedef void (*hx_hashmap_visit_fn)(const hx_entry_t *e, void *ctx);
void hx_hashmap_for_each(const hx_hashmap_t *m, hx_hashmap_visit_fn fn, void *ctx);

/* FNV-1a 64-bit hash. Exposed because the router uses it too. */
uint64_t hx_hash64(const char *s);

#endif
