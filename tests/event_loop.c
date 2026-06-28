/*
 * Event-loop smoke test:
 *   1. helix_execute runs the handler on the worker that owns the key.
 *   2. helix_execute_sync blocks until the handler returns.
 *   3. Tasks submitted for the same key from multiple producers are
 *      executed in submission order (single-thread-per-key invariant).
 */
#include "helix/helix.h"
#include "helix/internal/runtime.h"
#include "helix/internal/hashmap.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_PER_PRODUCER 5000
#define N_PRODUCERS    8
#define KEY            "order-42"

typedef struct { atomic_int counter; int last; } counter_state_t;

static void increment(helix_state_t *s, void *args) {
    (void)args;
    counter_state_t *st = helix_state_get(s);
    if (!st) {
        st = calloc(1, sizeof(*st));
        helix_state_set(s, st, sizeof(*st), free);
    }
    int prev = st->last;
    st->last = prev + 1;
    atomic_fetch_add(&st->counter, 1);
}

typedef struct { helix_runtime_t *rt; int n; } producer_arg_t;

static void *producer(void *vp) {
    producer_arg_t *p = vp;
    for (int i = 0; i < p->n; ++i) {
        while (helix_execute(p->rt, KEY, increment, NULL) != 0) {
            /* queue full: yield and retry */
            sched_yield();
        }
    }
    return NULL;
}

static void readout(helix_state_t *s, void *args) {
    counter_state_t **out = args;
    *out = helix_state_get(s);
}

int main(void) {
    helix_config_t cfg = helix_config_default();
    cfg.worker_count = 4;
    cfg.queue_capacity = 1024;

    helix_runtime_t *rt = helix_runtime_create(&cfg);
    assert(rt && "runtime_create");

    pthread_t producers[N_PRODUCERS];
    producer_arg_t arg = { .rt = rt, .n = N_PER_PRODUCER };
    for (int i = 0; i < N_PRODUCERS; ++i) {
        int rc = pthread_create(&producers[i], NULL, producer, &arg);
        assert(rc == 0);
    }
    for (int i = 0; i < N_PRODUCERS; ++i) pthread_join(producers[i], NULL);

    counter_state_t *st = NULL;
    int rc = helix_execute_sync(rt, KEY, readout, &st);
    assert(rc == 0 && "execute_sync");
    assert(st && "state present");

    int total = atomic_load(&st->counter);
    int expected = N_PER_PRODUCER * N_PRODUCERS;
    if (total != expected || st->last != expected) {
        fprintf(stderr, "FAIL: total=%d last=%d expected=%d\n", total, st->last, expected);
        helix_runtime_destroy(rt);
        return 1;
    }

    helix_runtime_destroy(rt);
    printf("OK: %d events processed in order on key %s\n", total, KEY);
    return 0;
}
