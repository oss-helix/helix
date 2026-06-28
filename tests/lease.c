/*
 * Lease semantics:
 *   1. Calls to lease_acquire() for the same key are serialized — at any
 *      instant only one holder.
 *   2. Calls for different keys do not block each other.
 *   3. A lease that exceeds its TTL without release is reclaimed and a
 *      subsequent lease_release returns -1.
 */
#include "helix/helix.h"
#include "helix/internal/lease.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int g_max_concurrent_on_key = 0;
static atomic_int g_current_on_key        = 0;

typedef struct { lease_registry_t *reg; const char *key; int hold_us; } worker_arg_t;

static void *holder(void *vp) {
    worker_arg_t *a = vp;
    char *id = lease_acquire(a->reg, a->key, 5000);
    assert(id != NULL);

    int now = atomic_fetch_add(&g_current_on_key, 1) + 1;
    int prev_max = atomic_load(&g_max_concurrent_on_key);
    while (now > prev_max && !atomic_compare_exchange_weak(&g_max_concurrent_on_key, &prev_max, now)) {}

    usleep(a->hold_us);

    atomic_fetch_sub(&g_current_on_key, 1);
    int rc = lease_release(a->reg, id);
    assert(rc == 0);
    free(id);
    return NULL;
}

static void test_serializes_same_key(helix_runtime_t *rt) {
    lease_registry_t *reg = lease_registry_create(rt);
    assert(reg);

    enum { N = 8 };
    pthread_t th[N];
    worker_arg_t arg = { .reg = reg, .key = "seat-A1", .hold_us = 5000 };
    atomic_store(&g_max_concurrent_on_key, 0);
    atomic_store(&g_current_on_key, 0);

    for (int i = 0; i < N; ++i) assert(pthread_create(&th[i], NULL, holder, &arg) == 0);
    for (int i = 0; i < N; ++i) pthread_join(th[i], NULL);

    int peak = atomic_load(&g_max_concurrent_on_key);
    if (peak != 1) {
        fprintf(stderr, "FAIL: same-key peak concurrency=%d, expected 1\n", peak);
        exit(1);
    }
    lease_registry_destroy(reg);
}

static atomic_int g_per_key_peak[4];

typedef struct { lease_registry_t *reg; int key_idx; } pk_arg_t;

static void *holder_per_key(void *vp) {
    pk_arg_t *a = vp;
    char key[16]; snprintf(key, sizeof(key), "k-%d", a->key_idx);
    char *id = lease_acquire(a->reg, key, 5000);
    assert(id != NULL);

    int now = atomic_fetch_add(&g_per_key_peak[a->key_idx], 1) + 1;
    if (now > 1) { fprintf(stderr, "FAIL: key %d held twice\n", a->key_idx); exit(1); }
    usleep(2000);
    atomic_fetch_sub(&g_per_key_peak[a->key_idx], 1);

    int rc = lease_release(a->reg, id);
    assert(rc == 0);
    free(id);
    return NULL;
}

static void test_distinct_keys_parallel(helix_runtime_t *rt) {
    lease_registry_t *reg = lease_registry_create(rt);
    assert(reg);
    for (int i = 0; i < 4; ++i) atomic_store(&g_per_key_peak[i], 0);

    enum { N = 16 };
    pthread_t th[N];
    pk_arg_t  args[N];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; ++i) {
        args[i].reg = reg; args[i].key_idx = i % 4;
        assert(pthread_create(&th[i], NULL, holder_per_key, &args[i]) == 0);
    }
    for (int i = 0; i < N; ++i) pthread_join(th[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long long us = (t1.tv_sec - t0.tv_sec) * 1000000LL
                 + (t1.tv_nsec - t0.tv_nsec) / 1000LL;
    /* Serial baseline would be ~16 * 2ms = 32ms. With 4 distinct keys and 4
     * workers, expect roughly 4 * 2ms = 8ms plus overhead. Bound generously. */
    if (us > 25000) {
        fprintf(stderr, "FAIL: 16 leases on 4 keys took %lldus (serial would be ~32000)\n", us);
        exit(1);
    }
    lease_registry_destroy(reg);
}

static void test_ttl_reclaims(helix_runtime_t *rt) {
    lease_registry_t *reg = lease_registry_create(rt);
    assert(reg);
    char *id = lease_acquire(reg, "ttl-key", 50 /* ms */);
    assert(id != NULL);
    usleep(150 * 1000);  /* let TTL expire */
    int rc = lease_release(reg, id);
    /* Either the lease was reclaimed (rc != 0) or the release raced ahead;
     * release-on-expired returns -1. */
    if (rc == 0) {
        fprintf(stderr, "FAIL: release after TTL returned 0, expected -1\n");
        exit(1);
    }
    free(id);
    lease_registry_destroy(reg);
}

int main(void) {
    helix_config_t cfg = helix_config_default();
    cfg.worker_count   = 4;
    cfg.queue_capacity = 1024;
    helix_runtime_t *rt = helix_runtime_create(&cfg);
    assert(rt);

    test_serializes_same_key(rt);
    test_distinct_keys_parallel(rt);
    test_ttl_reclaims(rt);

    helix_runtime_destroy(rt);
    printf("OK: lease serializes per key, parallel across keys, TTL reclaim\n");
    return 0;
}
