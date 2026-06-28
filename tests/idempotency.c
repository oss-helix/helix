/*
 * Idempotency primitive (hx_cache_get_or_set):
 *   1. First call stores the value and returns NULL with was_new=1.
 *   2. Subsequent calls within TTL return the originally stored bytes
 *      (NOT the new bytes the second caller tried to set), was_new=0.
 *   3. After TTL expires, the next call re-installs and was_new=1 again.
 *   4. Under concurrent racers with the same key, exactly one is_new.
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
    hx_cache_t *c = hx_cache_create(8);

    size_t len;
    int    was_new;
    void  *r = hx_cache_get_or_set(c, "req-1", "first", 5, 0, &len, &was_new);
    assert(r == NULL && was_new == 1);

    /* Second call: should return the originally stored bytes. */
    r = hx_cache_get_or_set(c, "req-1", "second-attempt", 14, 0, &len, &was_new);
    assert(r != NULL);
    assert(was_new == 0);
    assert(len == 5);
    assert(memcmp(r, "first", 5) == 0);
    free(r);

    hx_cache_destroy(c);
}

static void test_ttl_reuses_slot(void) {
    hx_cache_t *c = hx_cache_create(8);

    size_t len; int was_new;
    hx_cache_get_or_set(c, "req", "v1", 2, 50, &len, &was_new);
    assert(was_new == 1);
    usleep(120 * 1000);
    void *r = hx_cache_get_or_set(c, "req", "v2", 2, 50, &len, &was_new);
    assert(r == NULL);
    assert(was_new == 1);  /* TTL expired, v2 installed */

    /* Now reads should observe v2, not v1. */
    r = hx_cache_get(c, "req", &len);
    assert(r && len == 2 && memcmp(r, "v2", 2) == 0);
    free(r);

    hx_cache_destroy(c);
}

#define RACE_THREADS 32

typedef struct { hx_cache_t *c; int id; atomic_int *winners; } race_arg_t;

static void *racer(void *vp) {
    race_arg_t *a = vp;
    char val[16]; snprintf(val, sizeof(val), "w-%d", a->id);
    size_t len; int was_new;
    void *r = hx_cache_get_or_set(a->c, "race", val, strlen(val), 0, &len, &was_new);
    if (was_new) atomic_fetch_add(a->winners, 1);
    free(r);
    return NULL;
}

static void test_concurrent_one_winner(void) {
    hx_cache_t *c = hx_cache_create(0);
    pthread_t  threads[RACE_THREADS];
    race_arg_t args[RACE_THREADS];
    atomic_int winners = 0;

    for (int i = 0; i < RACE_THREADS; ++i) {
        args[i] = (race_arg_t){ .c = c, .id = i, .winners = &winners };
        assert(pthread_create(&threads[i], NULL, racer, &args[i]) == 0);
    }
    for (int i = 0; i < RACE_THREADS; ++i) pthread_join(threads[i], NULL);
    int w = atomic_load(&winners);
    if (w != 1) {
        fprintf(stderr, "FAIL: %d winners, expected 1\n", w);
        exit(1);
    }
    hx_cache_destroy(c);
}

int main(void) {
    test_basic();
    test_ttl_reuses_slot();
    test_concurrent_one_winner();
    printf("OK: idempotency basic/ttl/single-winner\n");
    return 0;
}
