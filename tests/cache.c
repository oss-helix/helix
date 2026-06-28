/*
 * Cache semantics:
 *   1. set / get / delete roundtrip.
 *   2. TTL eviction — a key whose TTL elapses returns NULL on the next get.
 *   3. Hits and misses are accounted correctly.
 *   4. Concurrent set + get from many threads on the same key does not
 *      crash, and the hits + misses sum equals the number of gets.
 */
#include "helix/internal/cache.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_basic(void) {
    hx_cache_t *c = hx_cache_create(16);
    assert(c);

    const char *val = "hello-world";
    assert(hx_cache_set(c, "k1", val, strlen(val), 0) == 0);

    size_t len = 0;
    void  *got = hx_cache_get(c, "k1", &len);
    assert(got);
    assert(len == strlen(val));
    assert(memcmp(got, val, len) == 0);
    free(got);

    /* overwrite */
    const char *v2 = "second-value-longer";
    assert(hx_cache_set(c, "k1", v2, strlen(v2), 0) == 0);
    got = hx_cache_get(c, "k1", &len);
    assert(got && len == strlen(v2));
    assert(memcmp(got, v2, len) == 0);
    free(got);

    /* delete */
    assert(hx_cache_delete(c, "k1") == 0);
    assert(hx_cache_get(c, "k1", &len) == NULL);
    assert(hx_cache_delete(c, "k1") == -1);

    /* miss */
    assert(hx_cache_get(c, "missing", &len) == NULL);

    hx_cache_destroy(c);
}

static void test_ttl(void) {
    hx_cache_t *c = hx_cache_create(8);
    assert(c);

    assert(hx_cache_set(c, "exp", "v", 1, 50 /* ms */) == 0);
    size_t len = 0;
    void *got = hx_cache_get(c, "exp", &len);
    assert(got);
    free(got);

    usleep(120 * 1000);  /* let TTL elapse */
    assert(hx_cache_get(c, "exp", &len) == NULL);

    hx_cache_destroy(c);
}

static void test_stats(void) {
    hx_cache_t *c = hx_cache_create(8);
    hx_cache_set(c, "a", "1", 1, 0);
    for (int i = 0; i < 5; ++i) { size_t l; free(hx_cache_get(c, "a", &l)); }
    for (int i = 0; i < 3; ++i) { size_t l; assert(hx_cache_get(c, "x", &l) == NULL); }
    assert(hx_cache_hits(c)   == 5);
    assert(hx_cache_misses(c) == 3);
    hx_cache_destroy(c);
}

#define CT_THREADS  16
#define CT_OPS      1000

typedef struct { hx_cache_t *c; int writer; } ct_arg_t;
static atomic_int ct_gets = 0;

static void *ct_worker(void *vp) {
    ct_arg_t *a = vp;
    const char body[] = "{\"payload\":42}";
    for (int i = 0; i < CT_OPS; ++i) {
        char key[8]; snprintf(key, sizeof(key), "k%d", i % 8);
        if (a->writer) {
            hx_cache_set(a->c, key, body, sizeof(body), 0);
        } else {
            size_t l;
            void *v = hx_cache_get(a->c, key, &l);
            atomic_fetch_add(&ct_gets, 1);
            free(v);
        }
    }
    return NULL;
}

static void test_concurrent(void) {
    hx_cache_t *c = hx_cache_create(0);
    pthread_t threads[CT_THREADS];
    ct_arg_t args[CT_THREADS];
    atomic_store(&ct_gets, 0);
    for (int i = 0; i < CT_THREADS; ++i) {
        args[i] = (ct_arg_t){ .c = c, .writer = (i < 4) };
        assert(pthread_create(&threads[i], NULL, ct_worker, &args[i]) == 0);
    }
    for (int i = 0; i < CT_THREADS; ++i) pthread_join(threads[i], NULL);

    int total_gets = atomic_load(&ct_gets);
    size_t h = hx_cache_hits(c), m = hx_cache_misses(c);
    if ((size_t)total_gets != h + m) {
        fprintf(stderr, "FAIL: gets=%d hits=%zu misses=%zu (h+m must == gets)\n",
                total_gets, h, m);
        exit(1);
    }
    hx_cache_destroy(c);
}

int main(void) {
    test_basic();
    test_ttl();
    test_stats();
    test_concurrent();
    printf("OK: cache basic/ttl/stats/concurrent\n");
    return 0;
}
