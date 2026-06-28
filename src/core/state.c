/*
 * Per-handler state accessors. The runtime hands a `helix_state_t` to the
 * handler; this file implements the operations on it.
 *
 * No synchronization here — the handler runs on the worker that owns the key,
 * and that worker is the only thread touching this entry.
 */
#include "helix/helix.h"
#include "helix/internal/runtime.h"

void *helix_state_get(helix_state_t *s) {
    return (s && s->entry) ? s->entry->value : NULL;
}

void helix_state_set(helix_state_t *s, void *value, size_t size, void (*free_fn)(void *)) {
    if (!s || !s->entry) return;
    if (s->entry->free_fn && s->entry->value && s->entry->value != value) {
        s->entry->free_fn(s->entry->value);
    }
    s->entry->value      = value;
    s->entry->value_size = size;
    s->entry->free_fn    = free_fn;
}

const char *helix_state_key(const helix_state_t *s) {
    return (s && s->entry) ? s->entry->key : NULL;
}

size_t helix_state_value_size(const helix_state_t *s) {
    return (s && s->entry) ? s->entry->value_size : 0;
}
