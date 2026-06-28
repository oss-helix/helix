#include "helix/internal/cache.h"
#include "helix/internal/hashmap.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_BUCKETS 64

typedef struct {
    void    *value;        /* heap-allocated copy of the user's bytes */
    size_t   len;
    int64_t  expires_at_ms; /* INT64_MAX = no expiry */
} cache_entry_t;

typedef struct {
    pthread_mutex_t mu;
    hx_hashmap_t   *map;
    atomic_size_t   size;
} bucket_t;

struct hx_cache {
    bucket_t        *buckets;
    size_t           n_buckets;
    atomic_size_t    hits;
    atomic_size_t    misses;
};

static void cache_entry_free(void *p) {
    cache_entry_t *e = p;
    if (!e) return;
    free(e->value);
    free(e);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

hx_cache_t *hx_cache_create(size_t bucket_count) {
    if (bucket_count == 0) bucket_count = DEFAULT_BUCKETS;
    hx_cache_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->n_buckets = bucket_count;
    c->buckets = calloc(bucket_count, sizeof(bucket_t));
    if (!c->buckets) { free(c); return NULL; }
    for (size_t i = 0; i < bucket_count; ++i) {
        pthread_mutex_init(&c->buckets[i].mu, NULL);
        c->buckets[i].map = hx_hashmap_create(128);
        atomic_store(&c->buckets[i].size, 0);
        if (!c->buckets[i].map) {
            for (size_t j = 0; j < i; ++j) {
                hx_hashmap_destroy(c->buckets[j].map);
                pthread_mutex_destroy(&c->buckets[j].mu);
            }
            free(c->buckets); free(c);
            return NULL;
        }
    }
    atomic_store(&c->hits, 0);
    atomic_store(&c->misses, 0);
    return c;
}

void hx_cache_destroy(hx_cache_t *c) {
    if (!c) return;
    for (size_t i = 0; i < c->n_buckets; ++i) {
        hx_hashmap_destroy(c->buckets[i].map);
        pthread_mutex_destroy(&c->buckets[i].mu);
    }
    free(c->buckets);
    free(c);
}

static bucket_t *bucket_for(hx_cache_t *c, const char *key) {
    return &c->buckets[hx_hash64(key) % c->n_buckets];
}

void *hx_cache_get(hx_cache_t *c, const char *key, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!c || !key) { atomic_fetch_add(&c->misses, 1); return NULL; }

    bucket_t *b = bucket_for(c, key);
    pthread_mutex_lock(&b->mu);
    hx_entry_t *he = hx_hashmap_get(b->map, key);
    if (!he || !he->value) {
        pthread_mutex_unlock(&b->mu);
        atomic_fetch_add(&c->misses, 1);
        return NULL;
    }
    cache_entry_t *ce = he->value;
    if (ce->expires_at_ms != INT64_MAX && now_ms() >= ce->expires_at_ms) {
        /* Expired: drop and report miss. */
        hx_hashmap_remove(b->map, key);
        atomic_fetch_sub(&b->size, 1);
        pthread_mutex_unlock(&b->mu);
        atomic_fetch_add(&c->misses, 1);
        return NULL;
    }
    void *copy = malloc(ce->len);
    if (!copy) {
        pthread_mutex_unlock(&b->mu);
        atomic_fetch_add(&c->misses, 1);
        return NULL;
    }
    memcpy(copy, ce->value, ce->len);
    size_t len = ce->len;
    pthread_mutex_unlock(&b->mu);
    if (out_len) *out_len = len;
    atomic_fetch_add(&c->hits, 1);
    return copy;
}

int hx_cache_set(hx_cache_t *c, const char *key,
                 const void *value, size_t len, int ttl_ms) {
    if (!c || !key || (len > 0 && !value)) return -1;

    cache_entry_t *ce = malloc(sizeof(*ce));
    if (!ce) return -1;
    ce->len = len;
    ce->value = len ? malloc(len) : NULL;
    if (len && !ce->value) { free(ce); return -1; }
    if (len) memcpy(ce->value, value, len);
    ce->expires_at_ms = (ttl_ms > 0) ? (now_ms() + ttl_ms) : INT64_MAX;

    bucket_t *b = bucket_for(c, key);
    pthread_mutex_lock(&b->mu);
    hx_entry_t *he = hx_hashmap_get_or_create(b->map, key);
    if (!he) {
        pthread_mutex_unlock(&b->mu);
        cache_entry_free(ce);
        return -1;
    }
    int was_new = (he->value == NULL);
    /* Replace any prior value via its free_fn. */
    if (he->free_fn && he->value) he->free_fn(he->value);
    he->value      = ce;
    he->value_size = sizeof(*ce);
    he->free_fn    = cache_entry_free;
    if (was_new) atomic_fetch_add(&b->size, 1);
    pthread_mutex_unlock(&b->mu);
    return 0;
}

int hx_cache_delete(hx_cache_t *c, const char *key) {
    if (!c || !key) return -1;
    bucket_t *b = bucket_for(c, key);
    pthread_mutex_lock(&b->mu);
    hx_entry_t *he = hx_hashmap_get(b->map, key);
    if (!he || !he->value) {
        pthread_mutex_unlock(&b->mu);
        return -1;
    }
    hx_hashmap_remove(b->map, key);
    atomic_fetch_sub(&b->size, 1);
    pthread_mutex_unlock(&b->mu);
    return 0;
}

size_t hx_cache_size(hx_cache_t *c) {
    if (!c) return 0;
    size_t total = 0;
    for (size_t i = 0; i < c->n_buckets; ++i) {
        total += atomic_load(&c->buckets[i].size);
    }
    return total;
}

size_t hx_cache_hits(hx_cache_t *c)   { return c ? atomic_load(&c->hits)   : 0; }
size_t hx_cache_misses(hx_cache_t *c) { return c ? atomic_load(&c->misses) : 0; }
