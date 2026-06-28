/*
 * Persistent cache:
 *   1. Set N keys with no expiry, destroy the cache, reopen against the
 *      same WAL → all N keys still present with original bytes.
 *   2. A key whose absolute expires_at_ms is in the past at replay time
 *      is dropped on reopen.
 *   3. Delete records are honored across reopen.
 */
#include "helix/helix.h"
#include "helix/internal/cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *PATH = "build/cache.wal";

static void test_roundtrip(void) {
    unlink(PATH);
    mkdir("build", 0755);

    hx_cache_t *c = hx_cache_create_persistent(8, PATH, HELIX_WAL_PER_WRITE, 1);
    assert(c);
    for (int i = 0; i < 50; ++i) {
        char k[16], v[32];
        snprintf(k, sizeof(k), "k-%d", i);
        snprintf(v, sizeof(v), "value-%d", i);
        assert(hx_cache_set(c, k, v, strlen(v), 0) == 0);
    }
    hx_cache_destroy(c);

    c = hx_cache_create_persistent(8, PATH, HELIX_WAL_PER_WRITE, 1);
    assert(c);
    for (int i = 0; i < 50; ++i) {
        char k[16], expected[32];
        snprintf(k, sizeof(k), "k-%d", i);
        snprintf(expected, sizeof(expected), "value-%d", i);
        size_t len;
        void *got = hx_cache_get(c, k, &len);
        if (!got || len != strlen(expected) || memcmp(got, expected, len) != 0) {
            fprintf(stderr, "FAIL: key %s not restored\n", k);
            exit(1);
        }
        free(got);
    }
    hx_cache_destroy(c);
    unlink(PATH);
}

static void test_expired_dropped(void) {
    unlink(PATH);
    hx_cache_t *c = hx_cache_create_persistent(8, PATH, HELIX_WAL_PER_WRITE, 1);
    assert(c);
    assert(hx_cache_set(c, "live", "L", 1, 60000) == 0);
    assert(hx_cache_set(c, "dead", "D", 1, 50) == 0);
    hx_cache_destroy(c);

    usleep(150 * 1000);

    c = hx_cache_create_persistent(8, PATH, HELIX_WAL_PER_WRITE, 1);
    size_t len;
    void *live = hx_cache_get(c, "live", &len);
    void *dead = hx_cache_get(c, "dead", &len);
    assert(live);
    assert(dead == NULL);
    free(live);
    hx_cache_destroy(c);
    unlink(PATH);
}

static void test_delete_persisted(void) {
    unlink(PATH);
    hx_cache_t *c = hx_cache_create_persistent(8, PATH, HELIX_WAL_PER_WRITE, 1);
    hx_cache_set(c, "removeme", "x", 1, 0);
    hx_cache_set(c, "keep",     "y", 1, 0);
    hx_cache_delete(c, "removeme");
    hx_cache_destroy(c);

    c = hx_cache_create_persistent(8, PATH, HELIX_WAL_PER_WRITE, 1);
    size_t len;
    void *r = hx_cache_get(c, "removeme", &len);
    void *k = hx_cache_get(c, "keep",     &len);
    assert(r == NULL);
    assert(k != NULL);
    free(k);
    hx_cache_destroy(c);
    unlink(PATH);
}

int main(void) {
    test_roundtrip();
    test_expired_dropped();
    test_delete_persisted();
    printf("OK: persistent cache roundtrip / expiry / delete\n");
    return 0;
}
