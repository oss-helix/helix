#include "helix/internal/queue.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct hx_queue {
    hx_task_t       *buf;
    size_t           cap;       /* power of two */
    size_t           mask;
    size_t           head;      /* next pop index */
    size_t           tail;      /* next push index */
    atomic_size_t    count;     /* approximate, also authoritative under mu */
    int              closed;
    pthread_mutex_t  mu;
    pthread_cond_t   not_empty;
};

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p < 16 ? 16 : p;
}

hx_queue_t *hx_queue_create(size_t capacity) {
    hx_queue_t *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->cap = next_pow2(capacity ? capacity : 1024);
    q->mask = q->cap - 1;
    q->buf = calloc(q->cap, sizeof(hx_task_t));
    if (!q->buf) { free(q); return NULL; }
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    atomic_store(&q->count, 0);
    return q;
}

void hx_queue_destroy(hx_queue_t *q) {
    if (!q) return;
    /* Caller is responsible for ensuring no producers/consumers remain. */
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    free(q->buf);
    free(q);
}

int hx_queue_push(hx_queue_t *q, const hx_task_t *task) {
    pthread_mutex_lock(&q->mu);
    if (q->closed) { pthread_mutex_unlock(&q->mu); return -1; }
    size_t c = atomic_load_explicit(&q->count, memory_order_relaxed);
    if (c == q->cap) { pthread_mutex_unlock(&q->mu); return -1; }
    q->buf[q->tail] = *task;
    q->tail = (q->tail + 1) & q->mask;
    atomic_store_explicit(&q->count, c + 1, memory_order_release);
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

int hx_queue_pop(hx_queue_t *q, hx_task_t *out) {
    pthread_mutex_lock(&q->mu);
    while (atomic_load_explicit(&q->count, memory_order_acquire) == 0 && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }
    size_t c = atomic_load_explicit(&q->count, memory_order_relaxed);
    if (c == 0) {
        /* closed and drained */
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) & q->mask;
    atomic_store_explicit(&q->count, c - 1, memory_order_release);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

void hx_queue_close(hx_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

size_t hx_queue_size(const hx_queue_t *q) {
    return atomic_load_explicit(&q->count, memory_order_relaxed);
}
