/*
 * Snapshot writer and loader.
 *
 * A snapshot is a point-in-time dump of one worker's hashmap. It reuses the
 * WAL record format: a sequence of STATE_UPDATE records, one per live key,
 * with no deletes. The reader is `hx_wal_reader_*`.
 *
 * The snapshot/WAL relationship:
 *   - hx_snapshot_write(...) atomically replaces snapshot-{id}.snap.
 *   - The caller is responsible for truncating wal-{id}.log afterwards so
 *     recovery does not double-apply events captured by the snapshot.
 *   - Recovery loads the snapshot first, then replays the WAL on top.
 *     STATE_UPDATE is idempotent (last write wins), so a partial truncate
 *     after a snapshot is safe.
 */
#ifndef HELIX_INTERNAL_SNAPSHOT_H
#define HELIX_INTERNAL_SNAPSHOT_H

#include "helix/internal/hashmap.h"

/* Write the current contents of `shard` to a file at `path`. Uses a temp file
 * + rename for atomic replacement. Returns 0 on success, -1 on error. */
int hx_snapshot_write(const char *path, const hx_hashmap_t *shard);

/* Load a snapshot at `path` into `shard`. Each STATE_UPDATE record allocates
 * a heap buffer for the value and installs it via the entry's free_fn = free.
 * Returns 0 on success (including when the file does not exist), -1 on
 * corruption that is not a torn tail. */
int hx_snapshot_load(const char *path, hx_hashmap_t *shard);

/* Convenience: applies snapshot-{worker_id}.snap followed by wal-{worker_id}.log
 * to `shard`. Missing files are treated as no-op. */
int hx_recovery_load(const char *data_dir, size_t worker_id, hx_hashmap_t *shard);

#endif
