/*
 * Lease registry implementation.
 *
 * Each call to lease_acquire():
 *   1. Allocates a `lease_t` and assigns it a fresh id.
 *   2. Inserts the lease into the global id -> lease_t table.
 *   3. Calls helix_execute() with park_handler — the runtime queues that
 *      task on the worker owning the key.
 *   4. Waits on `acquired_cv` until the handler signals it has the slot.
 *   5. Returns the id to the caller (now they hold the lease).
 *
 * The handler itself:
 *   1. Signals `acquired_cv` so the caller can proceed.
 *   2. Parks on `released_cv` with a TTL deadline.
 *   3. Returns once `released` is set OR the deadline fires. Either way the
 *      worker is now free to pick up the next task for this key.
 */
#include "helix/internal/lease.h"
#include "helix/internal/runtime.h"
#include "helix/internal/hashmap.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

typedef enum {
    LEASE_WAITING  = 0,
    LEASE_HELD     = 1,
    LEASE_RELEASED = 2,
    LEASE_EXPIRED  = 3,
} lease_state_t;

typedef struct lease {
    char            *id;
    pthread_mutex_t  mu;
    pthread_cond_t   acquired_cv;   /* signaled when handler picks up */
    pthread_cond_t   released_cv;   /* signaled by lease_release */
    pthread_cond_t   done_cv;       /* signaled by handler before return */
    lease_state_t    state;
    int              ttl_ms;
    int              handler_done;
} lease_t;

struct lease_registry {
    helix_runtime_t *rt;
    pthread_mutex_t  mu;
    hx_hashmap_t    *by_id;
    atomic_uint_least64_t next_id;
    atomic_size_t    active;        /* state == LEASE_HELD count */
    atomic_size_t    pending;       /* state == LEASE_WAITING count */
};

/* Hashmap doesn't own the values — we store lease_t* in entry->value. */

lease_registry_t *lease_registry_create(helix_runtime_t *rt) {
    lease_registry_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->rt = rt;
    pthread_mutex_init(&r->mu, NULL);
    r->by_id = hx_hashmap_create(256);
    if (!r->by_id) { pthread_mutex_destroy(&r->mu); free(r); return NULL; }
    atomic_store(&r->next_id, 1);
    atomic_store(&r->active, 0);
    atomic_store(&r->pending, 0);
    return r;
}

void lease_registry_destroy(lease_registry_t *r) {
    if (!r) return;
    /* The HTTP server is shut down before the registry, so no new acquires
     * are in flight. Outstanding leases are abandoned — the runtime's own
     * destroy joins workers and releases everything. */
    hx_hashmap_destroy(r->by_id);
    pthread_mutex_destroy(&r->mu);
    free(r);
}

static char *mint_id(lease_registry_t *r) {
    uint64_t n = atomic_fetch_add(&r->next_id, 1);
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "L%llu", (unsigned long long)n);
    if (len < 0) return NULL;
    return strdup(buf);
}

static void timespec_in_ms(struct timespec *ts, int ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += ts->tv_nsec / 1000000000L;
        ts->tv_nsec %= 1000000000L;
    }
}

static void park_handler(helix_state_t *s, void *args) {
    (void)s;
    lease_t *l = args;

    /* Step 1: tell the acquirer that the slot is theirs. */
    pthread_mutex_lock(&l->mu);
    l->state = LEASE_HELD;
    pthread_cond_signal(&l->acquired_cv);

    /* Step 2: park until released or TTL expires. */
    struct timespec deadline;
    timespec_in_ms(&deadline, l->ttl_ms);
    while (l->state == LEASE_HELD) {
        int rc = pthread_cond_timedwait(&l->released_cv, &l->mu, &deadline);
        if (rc == ETIMEDOUT) {
            l->state = LEASE_EXPIRED;
            break;
        }
    }
    /* Tell the releaser (if any) that we're about to return — after this
     * point we touch nothing on `l` and the releaser can free it. */
    l->handler_done = 1;
    pthread_cond_signal(&l->done_cv);
    pthread_mutex_unlock(&l->mu);
}

static void lease_free(lease_t *l) {
    pthread_mutex_destroy(&l->mu);
    pthread_cond_destroy(&l->acquired_cv);
    pthread_cond_destroy(&l->released_cv);
    pthread_cond_destroy(&l->done_cv);
    free(l->id);
    free(l);
}

char *lease_acquire(lease_registry_t *r, const char *key, int ttl_ms) {
    if (!r || !key) return NULL;
    if (ttl_ms <= 0) ttl_ms = 30000;

    lease_t *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->id = mint_id(r);
    if (!l->id) { free(l); return NULL; }
    pthread_mutex_init(&l->mu, NULL);
    pthread_cond_init(&l->acquired_cv, NULL);
    pthread_cond_init(&l->released_cv, NULL);
    pthread_cond_init(&l->done_cv, NULL);
    l->state  = LEASE_WAITING;
    l->ttl_ms = ttl_ms;
    l->handler_done = 0;

    pthread_mutex_lock(&r->mu);
    hx_entry_t *e = hx_hashmap_get_or_create(r->by_id, l->id);
    if (!e) { pthread_mutex_unlock(&r->mu); goto fail; }
    e->value = l;
    e->value_size = sizeof(*l);
    e->free_fn = NULL;
    pthread_mutex_unlock(&r->mu);

    atomic_fetch_add(&r->pending, 1);

    if (helix_execute(r->rt, key, park_handler, l) != 0) {
        atomic_fetch_sub(&r->pending, 1);
        pthread_mutex_lock(&r->mu);
        hx_hashmap_remove(r->by_id, l->id);
        pthread_mutex_unlock(&r->mu);
        goto fail;
    }

    /* Wait for the worker to pick up our task. We also bound this with the
     * TTL so callers don't hang forever if the key is hot and the queue is
     * very deep. */
    struct timespec deadline;
    timespec_in_ms(&deadline, ttl_ms);

    pthread_mutex_lock(&l->mu);
    int rc = 0;
    while (l->state == LEASE_WAITING && rc == 0) {
        rc = pthread_cond_timedwait(&l->acquired_cv, &l->mu, &deadline);
    }
    int acquired = (l->state == LEASE_HELD);
    pthread_mutex_unlock(&l->mu);

    atomic_fetch_sub(&r->pending, 1);
    if (!acquired) {
        /* The handler may still pick this up later; mark expired so when it
         * does, it short-circuits. */
        pthread_mutex_lock(&l->mu);
        if (l->state == LEASE_WAITING) l->state = LEASE_EXPIRED;
        pthread_mutex_unlock(&l->mu);
        pthread_mutex_lock(&r->mu);
        hx_hashmap_remove(r->by_id, l->id);
        pthread_mutex_unlock(&r->mu);
        goto fail;
    }

    atomic_fetch_add(&r->active, 1);
    return strdup(l->id);

fail:
    lease_free(l);
    return NULL;
}

int lease_release(lease_registry_t *r, const char *lease_id) {
    if (!r || !lease_id) return -1;
    pthread_mutex_lock(&r->mu);
    hx_entry_t *e = hx_hashmap_get(r->by_id, lease_id);
    if (!e || !e->value) { pthread_mutex_unlock(&r->mu); return -1; }
    lease_t *l = e->value;
    /* Detach from the registry so a duplicate release is a no-op. The
     * lease_t itself is freed below by us, not by the handler. */
    hx_hashmap_remove(r->by_id, lease_id);
    pthread_mutex_unlock(&r->mu);

    pthread_mutex_lock(&l->mu);
    int was_held = (l->state == LEASE_HELD);
    if (was_held) {
        l->state = LEASE_RELEASED;
        pthread_cond_signal(&l->released_cv);
    }
    /* Wait for the handler to finish accessing `l` so we can free it. */
    while (!l->handler_done) pthread_cond_wait(&l->done_cv, &l->mu);
    pthread_mutex_unlock(&l->mu);

    if (was_held) atomic_fetch_sub(&r->active, 1);
    lease_free(l);
    return was_held ? 0 : -1;
}

size_t lease_active_count(lease_registry_t *r)  { return atomic_load(&r->active); }
size_t lease_pending_count(lease_registry_t *r) { return atomic_load(&r->pending); }
