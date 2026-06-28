/*
 * Worker threads. Each worker drains its own queue and runs handlers serially.
 * The owner thread of a key is determined purely by hash(key) % worker_count,
 * giving us the single-thread-per-key invariant without any per-key locks.
 */
#include "helix/helix.h"
#include "helix/internal/runtime.h"
#include "helix/internal/queue.h"
#include "helix/internal/hashmap.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations of internal worker plumbing. */
static void *worker_main(void *arg);
static int   attach_workers(helix_runtime_t *rt);
static void  detach_workers(helix_runtime_t *rt);

/* Public: helix_runtime_create allocates shards in core/runtime.c, but worker
 * threads are spun up here. We piggyback on the runtime struct's `running`
 * flag and start workers lazily on first submission. A small once-flag ensures
 * we only attach once. */

static pthread_mutex_t g_attach_mu = PTHREAD_MUTEX_INITIALIZER;

static int ensure_started(helix_runtime_t *rt) {
    if (atomic_load_explicit(&rt->running, memory_order_acquire)) return 0;
    pthread_mutex_lock(&g_attach_mu);
    int rc = 0;
    if (!atomic_load_explicit(&rt->running, memory_order_acquire)) {
        rc = attach_workers(rt);
    }
    pthread_mutex_unlock(&g_attach_mu);
    return rc;
}

static int attach_workers(helix_runtime_t *rt) {
    for (size_t i = 0; i < rt->worker_count; ++i) {
        hx_worker_t *w = &rt->workers[i];
        w->queue = hx_queue_create(rt->cfg.queue_capacity);
        if (!w->queue) goto fail;
    }
    atomic_store_explicit(&rt->running, 1, memory_order_release);
    for (size_t i = 0; i < rt->worker_count; ++i) {
        hx_worker_t *w = &rt->workers[i];
        if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
            atomic_store_explicit(&rt->running, 0, memory_order_release);
            /* Close queues so any partially-started peer can exit cleanly. */
            for (size_t j = 0; j <= i; ++j) {
                if (rt->workers[j].queue) hx_queue_close(rt->workers[j].queue);
            }
            for (size_t j = 0; j < i; ++j) pthread_join(rt->workers[j].thread, NULL);
            goto fail;
        }
    }
    return 0;
fail:
    for (size_t i = 0; i < rt->worker_count; ++i) {
        if (rt->workers[i].queue) {
            hx_queue_destroy(rt->workers[i].queue);
            rt->workers[i].queue = NULL;
        }
    }
    return -1;
}

static void detach_workers(helix_runtime_t *rt) {
    if (!atomic_load_explicit(&rt->running, memory_order_acquire)) return;
    atomic_store_explicit(&rt->running, 0, memory_order_release);
    for (size_t i = 0; i < rt->worker_count; ++i) {
        if (rt->workers[i].queue) hx_queue_close(rt->workers[i].queue);
    }
    for (size_t i = 0; i < rt->worker_count; ++i) {
        pthread_join(rt->workers[i].thread, NULL);
        hx_queue_destroy(rt->workers[i].queue);
        rt->workers[i].queue = NULL;
    }
}

static void *worker_main(void *arg) {
    hx_worker_t *w = (hx_worker_t *)arg;
    hx_task_t task;
    while (hx_queue_pop(w->queue, &task) == 0) {
        hx_entry_t *e = hx_hashmap_get_or_create(w->shard, task.key);
        if (e) {
            helix_state_t st = { .entry = e, .worker = w };
            task.fn(&st, task.args);
        }
        atomic_fetch_add_explicit(&w->processed, 1, memory_order_relaxed);

        if (task.sync_mu) {
            pthread_mutex_lock(task.sync_mu);
            *task.sync_done = 1;
            pthread_cond_signal(task.sync_cv);
            pthread_mutex_unlock(task.sync_mu);
        }
        free(task.key);
    }
    return NULL;
}

static hx_worker_t *route(helix_runtime_t *rt, const char *key) {
    uint64_t h = hx_hash64(key);
    return &rt->workers[h % rt->worker_count];
}

int helix_execute(helix_runtime_t *rt, const char *key,
                  helix_handler_t fn, void *args) {
    if (!rt || !key || !fn) return -1;
    if (ensure_started(rt) != 0) return -1;
    char *kdup = strdup(key);
    if (!kdup) return -1;
    hx_task_t task = {
        .fn = fn, .args = args, .key = kdup,
        .sync_mu = NULL, .sync_cv = NULL, .sync_done = NULL,
    };
    int rc = hx_queue_push(route(rt, key)->queue, &task);
    if (rc != 0) free(kdup);
    return rc;
}

int helix_execute_sync(helix_runtime_t *rt, const char *key,
                       helix_handler_t fn, void *args) {
    if (!rt || !key || !fn) return -1;
    if (ensure_started(rt) != 0) return -1;
    char *kdup = strdup(key);
    if (!kdup) return -1;

    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
    int done = 0;

    hx_task_t task = {
        .fn = fn, .args = args, .key = kdup,
        .sync_mu = &mu, .sync_cv = &cv, .sync_done = &done,
    };
    int rc = hx_queue_push(route(rt, key)->queue, &task);
    if (rc != 0) {
        free(kdup);
        pthread_mutex_destroy(&mu);
        pthread_cond_destroy(&cv);
        return rc;
    }
    pthread_mutex_lock(&mu);
    while (!done) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    pthread_mutex_destroy(&mu);
    pthread_cond_destroy(&cv);
    return 0;
}

/* Called from helix_runtime_destroy in src/runtime.c via this hook. */
void hx_event_loop_shutdown(helix_runtime_t *rt) {
    detach_workers(rt);
}
