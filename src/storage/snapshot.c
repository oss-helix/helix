#include "helix/internal/snapshot.h"
#include "helix/internal/wal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    hx_wal_t *w;
    int       err;
} writer_ctx_t;

static void visit_emit(const hx_entry_t *e, void *vp) {
    writer_ctx_t *ctx = vp;
    if (ctx->err) return;
    if (!e->value || e->value_size == 0) return;
    size_t klen = strlen(e->key);
    if (klen > UINT16_MAX) { ctx->err = -1; return; }
    if (hx_wal_append(ctx->w, HX_WAL_REC_STATE_UPDATE,
                      e->key, (uint16_t)klen,
                      e->value, (uint32_t)e->value_size) != 0) {
        ctx->err = -1;
    }
}

int hx_snapshot_write(const char *path, const hx_hashmap_t *shard) {
    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;

    /* Remove any leftover tmp from a previous crash. */
    (void)unlink(tmp);

    hx_wal_t *w = hx_wal_open(tmp, HELIX_WAL_PER_WRITE, 1);
    if (!w) return -1;

    writer_ctx_t ctx = { .w = w, .err = 0 };
    hx_hashmap_for_each(shard, visit_emit, &ctx);
    if (ctx.err) { hx_wal_close(w); unlink(tmp); return -1; }

    if (hx_wal_sync(w) != 0) { hx_wal_close(w); unlink(tmp); return -1; }
    hx_wal_close(w);

    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int hx_snapshot_load(const char *path, hx_hashmap_t *shard) {
    /* No-op if the snapshot file does not exist. */
    if (access(path, F_OK) != 0) return 0;

    hx_wal_reader_t *r = hx_wal_reader_open(path);
    if (!r) return -1;

    for (;;) {
        hx_wal_rec_type_t type;
        char *key = NULL; uint16_t klen = 0;
        void *val = NULL; uint32_t vlen = 0;
        int rc = hx_wal_reader_next(r, &type, &key, &klen, &val, &vlen);
        if (rc == 0) break;          /* clean EOF / torn tail */
        if (rc < 0)  { hx_wal_reader_close(r); return -1; }

        hx_entry_t *e = hx_hashmap_get_or_create(shard, key);
        free(key);
        if (!e) { free(val); hx_wal_reader_close(r); return -1; }
        if (type == HX_WAL_REC_STATE_UPDATE) {
            /* Caller is the runtime, before workers start. We own `val`. */
            if (e->free_fn && e->value) e->free_fn(e->value);
            e->value      = val;
            e->value_size = vlen;
            e->free_fn    = free;
        } else if (type == HX_WAL_REC_STATE_DELETE) {
            free(val);
            if (e->free_fn && e->value) e->free_fn(e->value);
            e->value = NULL;
            e->value_size = 0;
            e->free_fn = NULL;
            /* Logical delete: we leave the slot but with no value.
             * A full delete would touch hashmap internals; not necessary for
             * correctness since helix_state_get would return NULL. */
        } else {
            free(val);
        }
    }
    hx_wal_reader_close(r);
    return 0;
}
