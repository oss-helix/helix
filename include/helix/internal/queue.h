/*
 * Bounded multi-producer / single-consumer task queue.
 * One queue per worker. Producers are any caller of helix_execute*;
 * the consumer is the owning worker thread.
 *
 * Synchronization: pthread mutex + condvar. The hot path is short
 * (push: lock / memcpy / signal / unlock) and the queue is per-worker,
 * so contention is limited to the producers targeting one specific key shard.
 */
#ifndef HELIX_INTERNAL_QUEUE_H
#define HELIX_INTERNAL_QUEUE_H

#include <pthread.h>
#include <stddef.h>

#include "helix/helix.h"

/* A unit of work. `key` is owned (strdup'd) when enqueued and freed by
 * the consumer once the entry has been pulled. */
typedef struct {
    helix_handler_t   fn;
    void             *args;
    char             *key;

    /* Completion handshake — non-NULL only for sync submissions. */
    pthread_mutex_t  *sync_mu;
    pthread_cond_t   *sync_cv;
    int              *sync_done;
} hx_task_t;

typedef struct hx_queue hx_queue_t;

hx_queue_t *hx_queue_create(size_t capacity);
void        hx_queue_destroy(hx_queue_t *q);

/* Returns 0 on success, -1 if the queue is closed or full. Producer-side. */
int hx_queue_push(hx_queue_t *q, const hx_task_t *task);

/* Blocks until a task is available or the queue is closed.
 * Returns 0 on success, -1 if the queue is closed and empty. Consumer-side. */
int hx_queue_pop(hx_queue_t *q, hx_task_t *out);

/* Marks the queue closed. Pop returns -1 once drained. Idempotent. */
void hx_queue_close(hx_queue_t *q);

/* Approximate count, no lock. For metrics only. */
size_t hx_queue_size(const hx_queue_t *q);

#endif
