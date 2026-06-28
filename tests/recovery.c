/*
 * Recovery integration test:
 *   1. Open a runtime with a data_dir + WAL on, submit N events that
 *      mutate state under several keys, destroy the runtime.
 *   2. Reopen the runtime with the same data_dir; assert state was restored.
 *   3. Repeat with snapshot_interval set so the snapshot path is exercised.
 */
#include "helix/helix.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { int counter; } account_t;

static void deposit(helix_state_t *s, void *args) {
    int amount = *(int *)args;
    account_t *a = helix_state_get(s);
    if (!a) {
        a = calloc(1, sizeof(*a));
        helix_state_set(s, a, sizeof(*a), free);
    }
    a->counter += amount;
}

static void readout(helix_state_t *s, void *args) {
    int *out = args;
    account_t *a = helix_state_get(s);
    *out = a ? a->counter : -1;
}

static void rm_rf(const char *path) {
    /* Lazy: rely on the four files we know we created. */
    char p[256];
    for (size_t i = 0; i < 4; ++i) {
        snprintf(p, sizeof(p), "%s/wal-%zu.log", path, i);          unlink(p);
        snprintf(p, sizeof(p), "%s/snapshot-%zu.snap", path, i);    unlink(p);
        snprintf(p, sizeof(p), "%s/snapshot-%zu.snap.tmp", path, i); unlink(p);
    }
    rmdir(path);
}

static void seed_and_close(const char *dir, size_t snapshot_interval) {
    rm_rf(dir);
    helix_config_t cfg = helix_config_default();
    cfg.worker_count       = 4;
    cfg.queue_capacity     = 1024;
    cfg.data_dir           = dir;
    cfg.wal_mode           = HELIX_WAL_PER_WRITE;
    cfg.snapshot_interval  = snapshot_interval;

    helix_runtime_t *rt = helix_runtime_create(&cfg);
    assert(rt);

    int one = 1;
    for (int i = 0; i < 1000; ++i) {
        char key[16]; snprintf(key, sizeof(key), "acct-%d", i % 10);
        int rc = helix_execute_sync(rt, key, deposit, &one);
        assert(rc == 0);
    }
    helix_runtime_destroy(rt);
}

static void reopen_and_verify(const char *dir, int expected_per_account) {
    helix_config_t cfg = helix_config_default();
    cfg.worker_count       = 4;
    cfg.queue_capacity     = 1024;
    cfg.data_dir           = dir;
    cfg.wal_mode           = HELIX_WAL_PER_WRITE;

    helix_runtime_t *rt = helix_runtime_create(&cfg);
    assert(rt);

    int total = 0;
    for (int i = 0; i < 10; ++i) {
        char key[16]; snprintf(key, sizeof(key), "acct-%d", i);
        int v = -1;
        int rc = helix_execute_sync(rt, key, readout, &v);
        assert(rc == 0);
        assert(v == expected_per_account);
        total += v;
    }
    helix_runtime_destroy(rt);
    assert(total == expected_per_account * 10);
}

int main(void) {
    /* Phase 1: WAL-only recovery (no snapshot). */
    seed_and_close("build/data_wal", 0);
    reopen_and_verify("build/data_wal", 100);
    rm_rf("build/data_wal");

    /* Phase 2: snapshot triggers mid-run, WAL gets rotated, recovery still
     * restores the same total. */
    seed_and_close("build/data_snap", 50);
    reopen_and_verify("build/data_snap", 100);
    rm_rf("build/data_snap");

    printf("OK: recovery from WAL and from snapshot+WAL\n");
    return 0;
}
