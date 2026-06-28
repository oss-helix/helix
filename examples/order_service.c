/*
 * Order-aggregate example.
 *
 * Demonstrates the Helix programming model:
 *   - one C struct per aggregate, stored as the per-key state.
 *   - one C function per business operation (pay / cancel / refund).
 *   - operations are submitted by routing key; Helix guarantees that all
 *     operations for the same key (e.g. "order-42") run serially on a
 *     single worker thread, so the handlers contain no locks.
 *
 * Persistence is enabled via cfg.data_dir + cfg.wal_mode; on restart the
 * runtime replays its snapshot + WAL so existing orders survive.
 *
 * Build:   make example
 * Run:     ./build/example_order_service
 */
#include "helix/helix.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    ORDER_CREATED  = 0,
    ORDER_PAID     = 1,
    ORDER_CANCELED = 2,
    ORDER_REFUNDED = 3,
} order_status_t;

typedef struct {
    order_status_t status;
    int            amount;   /* cents */
} order_t;

typedef struct { int amount; } pay_args_t;

static order_t *ensure_order(helix_state_t *s) {
    order_t *o = helix_state_get(s);
    if (!o) {
        o = calloc(1, sizeof(*o));
        o->status = ORDER_CREATED;
        helix_state_set(s, o, sizeof(*o), free);
    }
    return o;
}

static void pay(helix_state_t *s, void *args) {
    pay_args_t *a = args;
    order_t *o = ensure_order(s);
    if (o->status == ORDER_CREATED) {
        o->status = ORDER_PAID;
        o->amount = a->amount;
        printf("[%s] paid: %d\n", helix_state_key(s), o->amount);
    }
    free(a);  /* args were heap-allocated by the caller */
}

static void cancel(helix_state_t *s, void *args) {
    (void)args;
    order_t *o = ensure_order(s);
    if (o->status != ORDER_CREATED) return;
    o->status = ORDER_CANCELED;
    printf("[%s] canceled\n", helix_state_key(s));
}

static void refund(helix_state_t *s, void *args) {
    (void)args;
    order_t *o = ensure_order(s);
    if (o->status != ORDER_PAID) return;
    o->status = ORDER_REFUNDED;
    printf("[%s] refunded: %d\n", helix_state_key(s), o->amount);
}

typedef struct { order_status_t status; int amount; } readout_t;
static void readout(helix_state_t *s, void *args) {
    readout_t *out = args;
    order_t *o = helix_state_get(s);
    if (o) { out->status = o->status; out->amount = o->amount; }
    else   { out->status = ORDER_CREATED; out->amount = 0; }
}

int main(void) {
    helix_config_t cfg = helix_config_default();
    cfg.worker_count       = 4;
    cfg.data_dir           = "build/data_example";
    cfg.wal_mode           = HELIX_WAL_BATCHED;
    cfg.wal_batch_size     = 16;
    cfg.snapshot_interval  = 1000;

    helix_runtime_t *rt = helix_runtime_create(&cfg);
    if (!rt) { fprintf(stderr, "runtime_create failed\n"); return 1; }

    /* 1000 orders. Roughly:
     *   - every 5th is canceled before payment (no-op on pay afterwards)
     *   - the rest are paid; half of those are refunded.
     * All ops on the same order land on the same worker — no locks anywhere
     * in user code. */
    for (int i = 0; i < 1000; ++i) {
        char key[32]; snprintf(key, sizeof(key), "order-%d", i);
        if (i % 5 == 0) {
            helix_execute(rt, key, cancel, NULL);
        }
        /* `pay` is async, so the args must outlive the loop iteration.
         * The handler takes ownership and frees. */
        pay_args_t *pa = malloc(sizeof(*pa));
        pa->amount = 1000 + i;
        helix_execute(rt, key, pay, pa);
        if (i % 2 == 0) helix_execute(rt, key, refund, NULL);
    }

    /* Synchronous readout to make sure all enqueued ops have drained, then
     * tally what we observe. */
    int paid = 0, refunded = 0, canceled = 0;
    for (int i = 0; i < 1000; ++i) {
        char key[32]; snprintf(key, sizeof(key), "order-%d", i);
        readout_t r = {0};
        helix_execute_sync(rt, key, readout, &r);
        if (r.status == ORDER_PAID)     paid++;
        if (r.status == ORDER_REFUNDED) refunded++;
        if (r.status == ORDER_CANCELED) canceled++;
    }
    printf("\nfinal: paid=%d refunded=%d canceled=%d total=%d\n",
           paid, refunded, canceled, paid + refunded + canceled);

    helix_runtime_destroy(rt);
    return 0;
}
