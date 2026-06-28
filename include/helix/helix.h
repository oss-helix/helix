/*
 * Helix — Distributed In-Memory State Runtime
 *
 * Copyright 2026 The Helix Authors
 * Licensed under the Apache License, Version 2.0.
 *
 * Public C API.
 */
#ifndef HELIX_H
#define HELIX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct helix_runtime helix_runtime_t;
typedef struct helix_state   helix_state_t;

/* WAL sync policies. */
typedef enum {
    HELIX_WAL_OFF        = 0,  /* no WAL */
    HELIX_WAL_ASYNC      = 1,  /* fsync on its own cadence */
    HELIX_WAL_BATCHED    = 2,  /* fsync after N writes */
    HELIX_WAL_PER_WRITE  = 3,  /* fsync after every write (slowest, strongest) */
} helix_wal_mode_t;

typedef struct {
    size_t            worker_count;       /* default: number of online CPUs */
    size_t            queue_capacity;     /* per-worker queue capacity, default 4096 */
    const char       *data_dir;           /* WAL/snapshot directory; NULL = ephemeral */
    helix_wal_mode_t  wal_mode;
    size_t            wal_batch_size;     /* events per fsync when BATCHED */
    size_t            snapshot_interval;  /* commits between snapshots; 0 = disabled */
} helix_config_t;

/* Returns a config initialized to defaults. */
helix_config_t helix_config_default(void);

/* Handler executed by the worker that owns `key`.
 * `state` is the owner-thread-local handle; only valid for the duration of the call. */
typedef void (*helix_handler_t)(helix_state_t *state, void *args);

helix_runtime_t *helix_runtime_create(const helix_config_t *cfg);
void             helix_runtime_destroy(helix_runtime_t *rt);

/* Schedule `fn` on the worker that owns `key`. Non-blocking.
 * Returns 0 on success, -1 if the queue is full or the runtime is shutting down. */
int helix_execute(helix_runtime_t *rt, const char *key, helix_handler_t fn, void *args);

/* Same as helix_execute but blocks until the handler returns. */
int helix_execute_sync(helix_runtime_t *rt, const char *key, helix_handler_t fn, void *args);

/* --- State accessors, only valid inside a handler. --- */

/* Returns the pointer previously installed via helix_state_set, or NULL. */
void *helix_state_get(helix_state_t *state);

/* Installs a value into the state slot. `free_fn` will be invoked on the prior
 * value (if any) and on the new value at runtime destruction. Pass NULL to
 * leave the value unmanaged. `size` is informational (used by snapshot/WAL). */
void  helix_state_set(helix_state_t *state, void *value, size_t size, void (*free_fn)(void *));

/* The key this handler is currently running for. */
const char *helix_state_key(const helix_state_t *state);

/* Size hint for the current value, or 0 if unset. */
size_t helix_state_value_size(const helix_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_H */
