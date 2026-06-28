/*
 * Atomic ops semantics:
 *   1. hx_cache_atomic_incr: missing key starts at 0; consecutive incrs
 *      see prior result; non-integer existing value returns -1.
 *   2. hx_cache_atomic_cas: empty-expected matches absent; mismatched
 *      expected does not swap; matched expected swaps.
 *   3. Under concurrent incrs from N threads, the final value equals
 *      sum(deltas) — proves the bucket mutex is honored.
 */
#include "helix/internal/cache.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_incr_basic(void) {
    hx_cache_t *c = hx_cache_create(8);
    long long v = -1;
    assert(hx_cache_atomic_incr(c, "k", 1, 0, &v) == 0); assert(v == 1);
    assert(hx_cache_atomic_incr(c, "k", 4, 0, &v) == 0); assert(v == 5);
    assert(hx_cache_atomic_incr(c, "k", -2, 0, &v) == 0); assert(v == 3);

    /* If the existing value is not an integer, incr should refuse. */
    hx_cache_set(c, "bad", "not-a-number", 12, 0);
    assert(hx_cache_atomic_incr(c, "bad", 1, 0, &v) == -1);

    hx_cache_destroy(c);
}

static void test_cas_basic(void) {
    hx_cache_t *c = hx_cache_create(8);
    int swapped = 0;

    /* missing matches expected="" */
    assert(hx_cache_atomic_cas(c, "state", "", 0, "INIT", 4, 0, &swapped) == 0);
    assert(swapped == 1);

    /* wrong expected does not swap */
    assert(hx_cache_atomic_cas(c, "state", "wrong", 5, "OTHER", 5, 0, &swapped) == 0);
    assert(swapped == 0);

    /* correct expected swaps */
    assert(hx_cache_atomic_cas(c, "state", "INIT", 4, "PAID", 4, 0, &swapped) == 0);
    assert(swapped == 1);

    /* old expected on new state does not swap */
    assert(hx_cache_atomic_cas(c, "state", "INIT", 4, "REFUNDED", 8, 0, &swapped) == 0);
    assert(swapped == 0);

    hx_cache_destroy(c);
}

#define INCR_THREADS 16
#define INCR_PER_THREAD 1000

static void *incr_worker(void *vp) {
    hx_cache_t *c = vp;
    for (int i = 0; i < INCR_PER_THREAD; ++i) {
        long long out;
        assert(hx_cache_atomic_incr(c, "counter", 1, 0, &out) == 0);
    }
    return NULL;
}

static void test_incr_concurrent(void) {
    hx_cache_t *c = hx_cache_create(0);
    pthread_t threads[INCR_THREADS];
    for (int i = 0; i < INCR_THREADS; ++i)
        assert(pthread_create(&threads[i], NULL, incr_worker, c) == 0);
    for (int i = 0; i < INCR_THREADS; ++i) pthread_join(threads[i], NULL);

    size_t len;
    void *raw = hx_cache_get(c, "counter", &len);
    assert(raw && len > 0);
    char buf[32];
    memcpy(buf, raw, len < sizeof(buf) - 1 ? len : sizeof(buf) - 1);
    buf[len < sizeof(buf) - 1 ? len : sizeof(buf) - 1] = '\0';
    long long final_value = strtoll(buf, NULL, 10);
    free(raw);

    int expected = INCR_THREADS * INCR_PER_THREAD;
    if (final_value != expected) {
        fprintf(stderr, "FAIL: concurrent incr final=%lld expected=%d\n",
                final_value, expected);
        exit(1);
    }
    hx_cache_destroy(c);
}

int main(void) {
    test_incr_basic();
    test_cas_basic();
    test_incr_concurrent();
    printf("OK: atomic incr/cas/concurrent\n");
    return 0;
}
