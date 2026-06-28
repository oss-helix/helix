/*
 * Internal runtime layout. Worker threads, queues, and per-worker state shards
 * are owned here. Only files inside src/ should include this header.
 */
#ifndef HELIX_INTERNAL_RUNTIME_H
#define HELIX_INTERNAL_RUNTIME_H

#include <pthread.h>
#include <stdatomic.h>

#include "helix/helix.h"
#include "helix/internal/hashmap.h"

/* Forward declarations — full definitions arrive in the event-loop and WAL
 * branches. Kept opaque here so the core compiles before those land. */
struct hx_queue;
struct hx_wal;

typedef struct hx_worker {
    pthread_t          thread;
    size_t             id;
    helix_runtime_t   *runtime;     /* parent */
    struct hx_queue   *queue;       /* submission queue, opaque for now */
    hx_hashmap_t      *shard;       /* state owned by this worker */
    struct hx_wal     *wal;         /* per-worker WAL, NULL when disabled */
    atomic_size_t      processed;   /* lifetime processed count, for metrics */
    size_t             since_snapshot;  /* commits since last snapshot */
} hx_worker_t;

struct helix_runtime {
    helix_config_t   cfg;
    size_t           worker_count;
    hx_worker_t     *workers;
    atomic_int       running;       /* 1 while workers should run */
};

/* helix_state_t is the per-call handle handed to handlers. It points into the
 * worker's hashmap entry without exposing the map itself. */
struct helix_state {
    hx_entry_t  *entry;
    hx_worker_t *worker;
};

#endif
