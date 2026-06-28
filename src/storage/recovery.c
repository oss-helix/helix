/*
 * Recovery: load snapshot + replay WAL into a worker's hashmap shard.
 *
 * Called by helix_runtime_create before worker threads start, while only
 * the constructor thread has visibility into the shard.
 */
#include "helix/internal/snapshot.h"
#include "helix/internal/wal.h"
#include "helix/internal/hashmap.h"

#include <stdio.h>
#include <stdlib.h>

/* Reuses hx_snapshot_load to apply the WAL — record format is identical. */
int hx_recovery_load(const char *data_dir, size_t worker_id, hx_hashmap_t *shard) {
    char path[1024];

    int n = snprintf(path, sizeof(path), "%s/snapshot-%zu.snap", data_dir, worker_id);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    if (hx_snapshot_load(path, shard) != 0) return -1;

    n = snprintf(path, sizeof(path), "%s/wal-%zu.log", data_dir, worker_id);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    if (hx_snapshot_load(path, shard) != 0) return -1;

    return 0;
}
