#include "helix/internal/hashmap.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Open addressing with linear probing. Single-threaded by contract. */

typedef enum { SLOT_EMPTY = 0, SLOT_LIVE = 1, SLOT_TOMB = 2 } slot_state_t;

typedef struct {
    slot_state_t state;
    uint64_t     hash;
    hx_entry_t   entry;  /* key is owned (strdup'd) when state == SLOT_LIVE */
} slot_t;

struct hx_hashmap {
    slot_t *slots;
    size_t  capacity;   /* always a power of two */
    size_t  size;
    size_t  tombs;
};

uint64_t hx_hash64(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p < 16 ? 16 : p;
}

hx_hashmap_t *hx_hashmap_create(size_t initial_capacity) {
    hx_hashmap_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->capacity = next_pow2(initial_capacity ? initial_capacity : 64);
    m->slots = calloc(m->capacity, sizeof(slot_t));
    if (!m->slots) { free(m); return NULL; }
    return m;
}

void hx_hashmap_destroy(hx_hashmap_t *m) {
    if (!m) return;
    for (size_t i = 0; i < m->capacity; ++i) {
        slot_t *s = &m->slots[i];
        if (s->state == SLOT_LIVE) {
            if (s->entry.free_fn && s->entry.value) s->entry.free_fn(s->entry.value);
            free((char *)s->entry.key);
        }
    }
    free(m->slots);
    free(m);
}

static slot_t *probe(const hx_hashmap_t *m, const char *key, uint64_t h, int *found) {
    size_t mask = m->capacity - 1;
    size_t i = (size_t)(h & mask);
    slot_t *first_tomb = NULL;
    for (;;) {
        slot_t *s = &m->slots[i];
        if (s->state == SLOT_EMPTY) {
            *found = 0;
            return first_tomb ? first_tomb : s;
        }
        if (s->state == SLOT_TOMB) {
            if (!first_tomb) first_tomb = s;
        } else if (s->hash == h && strcmp(s->entry.key, key) == 0) {
            *found = 1;
            return s;
        }
        i = (i + 1) & mask;
    }
}

static int rehash(hx_hashmap_t *m, size_t new_cap) {
    slot_t *old_slots = m->slots;
    size_t  old_cap   = m->capacity;
    slot_t *new_slots = calloc(new_cap, sizeof(slot_t));
    if (!new_slots) return -1;
    m->slots = new_slots;
    m->capacity = new_cap;
    m->size = 0;
    m->tombs = 0;

    for (size_t i = 0; i < old_cap; ++i) {
        slot_t *s = &old_slots[i];
        if (s->state != SLOT_LIVE) continue;
        int found;
        slot_t *dst = probe(m, s->entry.key, s->hash, &found);
        assert(!found);
        *dst = *s;
        m->size++;
    }
    free(old_slots);
    return 0;
}

static int maybe_grow(hx_hashmap_t *m) {
    if ((m->size + m->tombs) * 10 >= m->capacity * 7) {
        size_t target = m->size * 2 < 16 ? 16 : m->size * 2;
        return rehash(m, next_pow2(target));
    }
    return 0;
}

hx_entry_t *hx_hashmap_get(hx_hashmap_t *m, const char *key) {
    int found;
    slot_t *s = probe(m, key, hx_hash64(key), &found);
    return found ? &s->entry : NULL;
}

hx_entry_t *hx_hashmap_get_or_create(hx_hashmap_t *m, const char *key) {
    if (maybe_grow(m) != 0) return NULL;
    uint64_t h = hx_hash64(key);
    int found;
    slot_t *s = probe(m, key, h, &found);
    if (!found) {
        char *kdup = strdup(key);
        if (!kdup) return NULL;
        if (s->state == SLOT_TOMB) m->tombs--;
        s->state = SLOT_LIVE;
        s->hash  = h;
        s->entry.key        = kdup;
        s->entry.value      = NULL;
        s->entry.value_size = 0;
        s->entry.free_fn    = NULL;
        m->size++;
    }
    return &s->entry;
}

void hx_hashmap_remove(hx_hashmap_t *m, const char *key) {
    int found;
    slot_t *s = probe(m, key, hx_hash64(key), &found);
    if (!found) return;
    if (s->entry.free_fn && s->entry.value) s->entry.free_fn(s->entry.value);
    free((char *)s->entry.key);
    s->entry.key        = NULL;
    s->entry.value      = NULL;
    s->entry.value_size = 0;
    s->entry.free_fn    = NULL;
    s->state = SLOT_TOMB;
    m->size--;
    m->tombs++;
}

size_t hx_hashmap_size(const hx_hashmap_t *m) {
    return m ? m->size : 0;
}

void hx_hashmap_for_each(const hx_hashmap_t *m, hx_hashmap_visit_fn fn, void *ctx) {
    if (!m || !fn) return;
    for (size_t i = 0; i < m->capacity; ++i) {
        slot_t *s = &m->slots[i];
        if (s->state == SLOT_LIVE) fn(&s->entry, ctx);
    }
}
