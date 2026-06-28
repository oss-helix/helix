#include "helix/internal/cache.h"
#include "helix/internal/hashmap.h"
#include "helix/internal/wal.h"

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
    /* Optional WAL backing — NULL if the cache is ephemeral. */
    hx_wal_t        *wal;
    pthread_mutex_t  wal_mu;
};

/* Forward declarations — definitions follow below. */
static bucket_t *bucket_for(hx_cache_t *c, const char *key);
static int       install_entry_locked(bucket_t *b, const char *key,
                                      const void *value, size_t len, int ttl_ms);

static void cache_entry_free(void *p) {
    cache_entry_t *e = p;
    if (!e) return;
    free(e->value);
    free(e);
}

/* Wall clock (CLOCK_REALTIME) so absolute expires_at_ms values survive a
 * process restart. Within a single run the difference vs monotonic is
 * imperceptible; the trade-off is sensitivity to NTP step adjustments,
 * which is acceptable for cache TTLs. */
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
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
    if (c->wal) {
        hx_wal_close(c->wal);
        pthread_mutex_destroy(&c->wal_mu);
    }
    for (size_t i = 0; i < c->n_buckets; ++i) {
        hx_hashmap_destroy(c->buckets[i].map);
        pthread_mutex_destroy(&c->buckets[i].mu);
    }
    free(c->buckets);
    free(c);
}

/* Persistent WAL record format for cache entries:
 *
 *   STATE_UPDATE: value bytes = [expires_at_ms (u64 LE)] [payload bytes]
 *                 expires_at_ms == 0 means "no expiry".
 *   STATE_DELETE: value bytes empty.
 *
 * Replayed in hx_cache_create_persistent below.
 */
static void le_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t rd_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void wal_persist_set(hx_cache_t *c, const char *key,
                            const void *value, size_t len, int64_t expires_at_ms) {
    if (!c->wal) return;
    size_t total = 8 + len;
    uint8_t *buf = malloc(total);
    if (!buf) return;
    uint64_t enc = (expires_at_ms == INT64_MAX) ? 0 : (uint64_t)expires_at_ms;
    le_u64(buf, enc);
    if (len) memcpy(buf + 8, value, len);
    size_t klen = strlen(key);
    if (klen <= UINT16_MAX && total <= UINT32_MAX) {
        pthread_mutex_lock(&c->wal_mu);
        (void)hx_wal_append(c->wal, HX_WAL_REC_STATE_UPDATE,
                            key, (uint16_t)klen,
                            buf, (uint32_t)total);
        pthread_mutex_unlock(&c->wal_mu);
    }
    free(buf);
}

static void wal_persist_delete(hx_cache_t *c, const char *key) {
    if (!c->wal) return;
    size_t klen = strlen(key);
    if (klen <= UINT16_MAX) {
        pthread_mutex_lock(&c->wal_mu);
        (void)hx_wal_append(c->wal, HX_WAL_REC_STATE_DELETE,
                            key, (uint16_t)klen, NULL, 0);
        pthread_mutex_unlock(&c->wal_mu);
    }
}

hx_cache_t *hx_cache_create_persistent(size_t bucket_count,
                                       const char *wal_path,
                                       helix_wal_mode_t mode,
                                       size_t batch_size) {
    hx_cache_t *c = hx_cache_create(bucket_count);
    if (!c) return NULL;

    /* Replay existing WAL into the buckets before opening for append. */
    hx_wal_reader_t *r = hx_wal_reader_open(wal_path);
    if (r) {
        int64_t cutoff = now_ms();
        for (;;) {
            hx_wal_rec_type_t type;
            char *key = NULL; uint16_t klen = 0;
            void *val = NULL; uint32_t vlen = 0;
            int rc = hx_wal_reader_next(r, &type, &key, &klen, &val, &vlen);
            if (rc <= 0) { free(key); free(val); break; }

            if (type == HX_WAL_REC_STATE_UPDATE && vlen >= 8) {
                uint64_t enc = rd_u64(val);
                int64_t expires = (enc == 0) ? INT64_MAX : (int64_t)enc;
                if (expires == INT64_MAX || expires > cutoff) {
                    /* Install bypassing the WAL — we're rebuilding state,
                     * not generating new records. */
                    bucket_t *b = bucket_for(c, key);
                    pthread_mutex_lock(&b->mu);
                    cache_entry_t *ce = malloc(sizeof(*ce));
                    if (ce) {
                        ce->len = vlen - 8;
                        ce->value = ce->len ? malloc(ce->len) : NULL;
                        if (ce->len && !ce->value) { free(ce); }
                        else {
                            if (ce->len) memcpy(ce->value, (uint8_t *)val + 8, ce->len);
                            ce->expires_at_ms = expires;
                            hx_entry_t *he = hx_hashmap_get_or_create(b->map, key);
                            if (he) {
                                int was_new = (he->value == NULL);
                                if (he->free_fn && he->value) he->free_fn(he->value);
                                he->value = ce; he->value_size = sizeof(*ce);
                                he->free_fn = cache_entry_free;
                                if (was_new) atomic_fetch_add(&b->size, 1);
                            } else cache_entry_free(ce);
                        }
                    }
                    pthread_mutex_unlock(&b->mu);
                }
            } else if (type == HX_WAL_REC_STATE_DELETE) {
                bucket_t *b = bucket_for(c, key);
                pthread_mutex_lock(&b->mu);
                hx_entry_t *he = hx_hashmap_get(b->map, key);
                if (he && he->value) {
                    hx_hashmap_remove(b->map, key);
                    atomic_fetch_sub(&b->size, 1);
                }
                pthread_mutex_unlock(&b->mu);
            }
            free(key); free(val);
        }
        hx_wal_reader_close(r);
    }

    c->wal = hx_wal_open(wal_path, mode, batch_size);
    if (!c->wal) { hx_cache_destroy(c); return NULL; }
    pthread_mutex_init(&c->wal_mu, NULL);
    return c;
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

    int64_t expires_at_ms = (ttl_ms > 0) ? (now_ms() + ttl_ms) : INT64_MAX;

    bucket_t *b = bucket_for(c, key);
    pthread_mutex_lock(&b->mu);
    int rc = install_entry_locked(b, key, value, len, ttl_ms);
    pthread_mutex_unlock(&b->mu);
    if (rc != 0) return -1;

    wal_persist_set(c, key, value, len, expires_at_ms);
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

    wal_persist_delete(c, key);
    return 0;
}

/* --- atomic ops, all of which hold the bucket mutex across read+write --- */

/* Reads the live entry under the held lock. Returns the cache_entry_t* or
 * NULL if missing/expired (and removes if expired). */
static cache_entry_t *live_entry_locked(bucket_t *b, const char *key) {
    hx_entry_t *he = hx_hashmap_get(b->map, key);
    if (!he || !he->value) return NULL;
    cache_entry_t *ce = he->value;
    if (ce->expires_at_ms != INT64_MAX && now_ms() >= ce->expires_at_ms) {
        hx_hashmap_remove(b->map, key);
        atomic_fetch_sub(&b->size, 1);
        return NULL;
    }
    return ce;
}

/* Replaces (or inserts) the entry for `key` with a fresh value. Caller owns
 * the byte buffer; this function copies it. Returns 0 / -1. */
static int install_entry_locked(bucket_t *b, const char *key,
                                const void *value, size_t len, int ttl_ms) {
    cache_entry_t *ce = malloc(sizeof(*ce));
    if (!ce) return -1;
    ce->len = len;
    ce->value = len ? malloc(len) : NULL;
    if (len && !ce->value) { free(ce); return -1; }
    if (len) memcpy(ce->value, value, len);
    ce->expires_at_ms = (ttl_ms > 0) ? (now_ms() + ttl_ms) : INT64_MAX;

    hx_entry_t *he = hx_hashmap_get_or_create(b->map, key);
    if (!he) { cache_entry_free(ce); return -1; }
    int was_new = (he->value == NULL);
    if (he->free_fn && he->value) he->free_fn(he->value);
    he->value = ce;
    he->value_size = sizeof(*ce);
    he->free_fn = cache_entry_free;
    if (was_new) atomic_fetch_add(&b->size, 1);
    return 0;
}

void *hx_cache_get_or_set(hx_cache_t *c, const char *key,
                          const void *value, size_t len, int ttl_ms,
                          size_t *out_len, int *was_new) {
    if (out_len)  *out_len  = 0;
    if (was_new)  *was_new  = 0;
    if (!c || !key) return NULL;

    bucket_t *b = bucket_for(c, key);
    pthread_mutex_lock(&b->mu);
    cache_entry_t *ce = live_entry_locked(b, key);
    if (ce) {
        void *copy = malloc(ce->len);
        if (!copy) { pthread_mutex_unlock(&b->mu); return NULL; }
        memcpy(copy, ce->value, ce->len);
        size_t n = ce->len;
        pthread_mutex_unlock(&b->mu);
        if (out_len) *out_len = n;
        atomic_fetch_add(&c->hits, 1);
        return copy;
    }
    int rc = install_entry_locked(b, key, value, len, ttl_ms);
    pthread_mutex_unlock(&b->mu);
    if (rc != 0) return NULL;
    if (was_new) *was_new = 1;
    atomic_fetch_add(&c->misses, 1);
    return NULL;
}

int hx_cache_atomic_incr(hx_cache_t *c, const char *key,
                         long long delta, int ttl_ms,
                         long long *out_value) {
    if (!c || !key) return -1;
    bucket_t *b = bucket_for(c, key);
    pthread_mutex_lock(&b->mu);

    long long current = 0;
    cache_entry_t *ce = live_entry_locked(b, key);
    if (ce) {
        char buf[32];
        size_t n = ce->len < sizeof(buf) - 1 ? ce->len : sizeof(buf) - 1;
        memcpy(buf, ce->value, n);
        buf[n] = '\0';
        char *end;
        current = strtoll(buf, &end, 10);
        if (end == buf) { pthread_mutex_unlock(&b->mu); return -1; }
    }
    long long next = current + delta;
    char out[32];
    int n = snprintf(out, sizeof(out), "%lld", next);
    if (n < 0 || (size_t)n >= sizeof(out)) { pthread_mutex_unlock(&b->mu); return -1; }
    int rc = install_entry_locked(b, key, out, (size_t)n, ttl_ms);
    pthread_mutex_unlock(&b->mu);
    if (rc != 0) return -1;
    if (out_value) *out_value = next;
    return 0;
}

int hx_cache_atomic_cas(hx_cache_t *c, const char *key,
                        const void *expected, size_t expected_len,
                        const void *next, size_t next_len,
                        int ttl_ms, int *swapped) {
    if (swapped) *swapped = 0;
    if (!c || !key) return -1;
    bucket_t *b = bucket_for(c, key);
    pthread_mutex_lock(&b->mu);

    cache_entry_t *ce = live_entry_locked(b, key);
    int match;
    if (!ce) {
        match = (expected_len == 0);  /* missing matches an empty expected */
    } else {
        match = (ce->len == expected_len) &&
                (expected_len == 0 || memcmp(ce->value, expected, expected_len) == 0);
    }
    if (!match) {
        pthread_mutex_unlock(&b->mu);
        return 0;
    }
    int rc = install_entry_locked(b, key, next, next_len, ttl_ms);
    pthread_mutex_unlock(&b->mu);
    if (rc != 0) return -1;
    if (swapped) *swapped = 1;
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
