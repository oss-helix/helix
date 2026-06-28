#include "helix/helix.h"
#include "helix/internal/runtime.h"
#include "helix/internal/hashmap.h"
#include "helix/internal/wal.h"
#include "helix/internal/snapshot.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

    /* Best-effort mkdir for data_dir; we don't recurse — caller is responsible
     * for any parent directories. */
    if (cfg.data_dir && cfg.wal_mode != HELIX_WAL_OFF) {
        if (mkdir(cfg.data_dir, 0755) != 0 && errno != EEXIST) {
            free(rt->workers); free(rt);
            return NULL;
        }
    }

    for (size_t i = 0; i < cfg.worker_count; ++i) {
        hx_worker_t *w = &rt->workers[i];
        w->id = i;
        w->runtime = rt;
        w->shard = hx_hashmap_create(256);
        atomic_store(&w->processed, 0);
        if (!w->shard) {
            for (size_t j = 0; j < i; ++j) {
                hx_hashmap_destroy(rt->workers[j].shard);
                hx_wal_close(rt->workers[j].wal);
            }
            free(rt->workers); free(rt);
            return NULL;
        }
        if (cfg.data_dir) {
            /* Recover any prior snapshot + WAL into this worker's shard
             * before the WAL is opened for append. */
            if (hx_recovery_load(cfg.data_dir, i, w->shard) != 0) {
                for (size_t j = 0; j <= i; ++j) {
                    hx_hashmap_destroy(rt->workers[j].shard);
                    if (j < i) hx_wal_close(rt->workers[j].wal);
                }
                free(rt->workers); free(rt);
                return NULL;
            }
        }
        if (cfg.data_dir && cfg.wal_mode != HELIX_WAL_OFF) {
            char path[512];
            snprintf(path, sizeof(path), "%s/wal-%zu.log", cfg.data_dir, i);
            w->wal = hx_wal_open(path, cfg.wal_mode, cfg.wal_batch_size);
            if (!w->wal) {
                for (size_t j = 0; j <= i; ++j) {
                    hx_hashmap_destroy(rt->workers[j].shard);
                    if (j < i) hx_wal_close(rt->workers[j].wal);
                }
                free(rt->workers); free(rt);
                return NULL;
            }
        }
    }

    /* Worker threads start when the event-loop module attaches itself.
     * For now `running` stays 0 and helix_execute returns -1. */
    return rt;
}

/* Implemented in event_loop.c. Joins worker threads if any were started. */
void hx_event_loop_shutdown(helix_runtime_t *rt);

void helix_runtime_destroy(helix_runtime_t *rt) {
    if (!rt) return;
    hx_event_loop_shutdown(rt);
    for (size_t i = 0; i < rt->worker_count; ++i) {
        hx_wal_close(rt->workers[i].wal);
        hx_hashmap_destroy(rt->workers[i].shard);
    }
    free(rt->workers);
    free(rt);
}

/* State accessors live in src/core/state.c per SPEC.md §8. */
