/*
 * Hashmap unit tests:
 *   - insert / lookup / overwrite
 *   - remove / tombstone behavior
 *   - growth under load
 *   - iteration coverage
 */
#include "helix/internal/hashmap.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_basic(void) {
    hx_hashmap_t *m = hx_hashmap_create(0);
    assert(m);

    hx_entry_t *e = hx_hashmap_get_or_create(m, "alpha");
    assert(e);
    e->value = (void *)1;
    e->value_size = 8;

    hx_entry_t *e2 = hx_hashmap_get(m, "alpha");
    assert(e2 && e2->value == (void *)1);
    assert(hx_hashmap_size(m) == 1);

    /* get_or_create on existing key must not duplicate */
    hx_entry_t *e3 = hx_hashmap_get_or_create(m, "alpha");
    assert(e3 == e);
    assert(hx_hashmap_size(m) == 1);

    assert(hx_hashmap_get(m, "missing") == NULL);

    hx_hashmap_destroy(m);
}

static void test_remove_and_tombstone(void) {
    hx_hashmap_t *m = hx_hashmap_create(0);
    for (int i = 0; i < 64; ++i) {
        char key[16]; snprintf(key, sizeof(key), "k-%d", i);
        hx_entry_t *e = hx_hashmap_get_or_create(m, key);
        assert(e);
        e->value_size = (size_t)i;
    }
    assert(hx_hashmap_size(m) == 64);

    /* Remove every other entry. */
    for (int i = 0; i < 64; i += 2) {
        char key[16]; snprintf(key, sizeof(key), "k-%d", i);
        hx_hashmap_remove(m, key);
    }
    assert(hx_hashmap_size(m) == 32);

    /* Lookups still find the survivors past tombstones. */
    for (int i = 1; i < 64; i += 2) {
        char key[16]; snprintf(key, sizeof(key), "k-%d", i);
        hx_entry_t *e = hx_hashmap_get(m, key);
        assert(e && e->value_size == (size_t)i);
    }
    hx_hashmap_destroy(m);
}

static void test_growth(void) {
    hx_hashmap_t *m = hx_hashmap_create(8);
    const int N = 10000;
    for (int i = 0; i < N; ++i) {
        char key[24]; snprintf(key, sizeof(key), "key-%d", i);
        hx_entry_t *e = hx_hashmap_get_or_create(m, key);
        assert(e);
        e->value = (void *)(intptr_t)i;
    }
    assert(hx_hashmap_size(m) == (size_t)N);

    for (int i = 0; i < N; ++i) {
        char key[24]; snprintf(key, sizeof(key), "key-%d", i);
        hx_entry_t *e = hx_hashmap_get(m, key);
        assert(e);
        assert(e->value == (void *)(intptr_t)i);
    }
    hx_hashmap_destroy(m);
}

static int visit_count;
static void count_visit(const hx_entry_t *e, void *ctx) {
    (void)e; (void)ctx;
    visit_count++;
}

static void test_iteration(void) {
    hx_hashmap_t *m = hx_hashmap_create(0);
    for (int i = 0; i < 50; ++i) {
        char key[12]; snprintf(key, sizeof(key), "%d", i);
        hx_hashmap_get_or_create(m, key);
    }
    visit_count = 0;
    hx_hashmap_for_each(m, count_visit, NULL);
    assert(visit_count == 50);
    hx_hashmap_destroy(m);
}

int main(void) {
    test_basic();
    test_remove_and_tombstone();
    test_growth();
    test_iteration();
    printf("OK: hashmap basic/remove/growth/iteration\n");
    return 0;
}
