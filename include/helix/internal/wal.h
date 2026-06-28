/*
 * Write-ahead log.
 *
 * Phase-1 WAL: one append-only file per worker, single writer. After a handler
 * runs, the worker serializes (key, new_state_bytes) as a STATE_UPDATE record
 * and appends it. Recovery replays these records into the worker shards to
 * rebuild state.
 *
 * Wire format, little-endian, no padding:
 *
 *   record   := header value
 *   header   := magic(u32=0x48584C00 'HXL\0')
 *               type(u8)
 *               flags(u8)
 *               key_len(u16)
 *               value_len(u32)
 *               crc32(u32)            // CRC over (type, flags, key_len, value_len, key, value)
 *   value    := key_bytes value_bytes
 */
#ifndef HELIX_INTERNAL_WAL_H
#define HELIX_INTERNAL_WAL_H

#include <stddef.h>
#include <stdint.h>

#include "helix/helix.h"

#define HX_WAL_MAGIC 0x48584C00u  /* 'H','X','L','\0' */

typedef enum {
    HX_WAL_REC_STATE_UPDATE = 1,
    HX_WAL_REC_STATE_DELETE = 2,
} hx_wal_rec_type_t;

typedef struct hx_wal hx_wal_t;

/* Opens (or creates) the WAL file at `path`. Append mode. */
hx_wal_t *hx_wal_open(const char *path, helix_wal_mode_t mode, size_t batch_size);
void      hx_wal_close(hx_wal_t *w);

/* Append a record. `key`/`value` may be NULL only when the corresponding
 * length is zero. Returns 0 on success, -1 on I/O error. */
int hx_wal_append(hx_wal_t *w,
                  hx_wal_rec_type_t type,
                  const char *key, uint16_t key_len,
                  const void *value, uint32_t value_len);

/* Force an fsync regardless of policy. */
int hx_wal_sync(hx_wal_t *w);

/* CRC32 (IEEE 802.3) over `buf`. Exposed for tests. */
uint32_t hx_crc32(const void *buf, size_t len);

/* --- Reader API for recovery --- */

typedef struct hx_wal_reader hx_wal_reader_t;

hx_wal_reader_t *hx_wal_reader_open(const char *path);
void             hx_wal_reader_close(hx_wal_reader_t *r);

/* Reads the next record. On success returns 1 and populates the out params;
 * caller owns the `key_out` and `value_out` buffers (heap-allocated) and must
 * free() them. Returns 0 at clean EOF, -1 on corruption (truncated tail is
 * treated as EOF — the file is assumed to have been crash-truncated). */
int hx_wal_reader_next(hx_wal_reader_t *r,
                       hx_wal_rec_type_t *type_out,
                       char **key_out, uint16_t *key_len_out,
                       void **value_out, uint32_t *value_len_out);

#endif
