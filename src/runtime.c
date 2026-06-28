#include "helix/helix.h"
#include "helix/internal/runtime.h"
#include "helix/internal/hashmap.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

helix_config_t helix_config_default(void) {
    helix_config_t c;
    memset(&c, 0, sizeof(c));
    long n = -1;
#ifdef _SC_NPROCESSORS_ONLN
    n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    c.worker_count       = n > 0 ? (size_t)n : 4;
    c.queue_capacity     = 4096;
    c.data_dir           = NULL;
    c.wal_mode           = HELIX_WAL_OFF;
    c.wal_batch_size     = 64;
    c.snapshot_interval  = 0;
    return c;
}

helix_runtime_t *helix_runtime_create(const helix_config_t *user_cfg) {
    helix_config_t cfg = user_cfg ? *user_cfg : helix_config_default();
    if (cfg.worker_count == 0) cfg.worker_count = helix_config_default().worker_count;
    if (cfg.queue_capacity == 0) cfg.queue_capacity = 4096;

    helix_runtime_t *rt = calloc(1, sizeof(*rt));
    if (!rt) return NULL;
    rt->cfg = cfg;
    rt->worker_count = cfg.worker_count;
    atomic_store(&rt->running, 0);

    rt->workers = calloc(cfg.worker_count, sizeof(hx_worker_t));
    if (!rt->workers) { free(rt); return NULL; }

    for (size_t i = 0; i < cfg.worker_count; ++i) {
        hx_worker_t *w = &rt->workers[i];
        w->id = i;
        w->runtime = rt;
        w->shard = hx_hashmap_create(256);
        atomic_store(&w->processed, 0);
        if (!w->shard) {
            for (size_t j = 0; j < i; ++j) hx_hashmap_destroy(rt->workers[j].shard);
            free(rt->workers);
            free(rt);
            return NULL;
        }
        /* w->queue and w->thread are spun up by the event-loop module. */
    }

    /* Worker threads start when the event-loop module attaches itself.
     * For now `running` stays 0 and helix_execute returns -1. */
    return rt;
}

void helix_runtime_destroy(helix_runtime_t *rt) {
    if (!rt) return;
    /* Stop signal — the event loop module will react when it lands. */
    atomic_store(&rt->running, 0);
    for (size_t i = 0; i < rt->worker_count; ++i) {
        hx_hashmap_destroy(rt->workers[i].shard);
    }
    free(rt->workers);
    free(rt);
}

/* --- Stubs filled in by feature/event-loop ----------------------------- */

int helix_execute(helix_runtime_t *rt, const char *key,
                  helix_handler_t fn, void *args) {
    (void)rt; (void)key; (void)fn; (void)args;
    return -1;
}

int helix_execute_sync(helix_runtime_t *rt, const char *key,
                       helix_handler_t fn, void *args) {
    (void)rt; (void)key; (void)fn; (void)args;
    return -1;
}

/* --- State accessors --------------------------------------------------- */

void *helix_state_get(helix_state_t *s) {
    return s && s->entry ? s->entry->value : NULL;
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
