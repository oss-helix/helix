#include "helix/internal/wal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* --- CRC32 (IEEE 802.3, reflected) ----------------------------------- */

static uint32_t g_crc_table[256];
static int      g_crc_table_ready = 0;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
        g_crc_table[i] = c;
    }
    g_crc_table_ready = 1;
}

/* Streaming CRC32: start with HX_CRC_INIT, feed slices with crc_update,
 * finalize with HX_CRC_FINAL. */
#define HX_CRC_INIT  0xFFFFFFFFu
#define HX_CRC_FINAL(c) ((c) ^ 0xFFFFFFFFu)

static uint32_t crc_update(uint32_t c, const void *buf, size_t len) {
    if (!g_crc_table_ready) crc_init();
    const uint8_t *p = buf;
    for (size_t i = 0; i < len; ++i) c = g_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

uint32_t hx_crc32(const void *buf, size_t len) {
    return HX_CRC_FINAL(crc_update(HX_CRC_INIT, buf, len));
}

/* --- Writer ---------------------------------------------------------- */

struct hx_wal {
    int               fd;
    helix_wal_mode_t  mode;
    size_t            batch_size;
    size_t            unsynced;     /* records written since last fsync */
};

hx_wal_t *hx_wal_open(const char *path, helix_wal_mode_t mode, size_t batch_size) {
    if (mode == HELIX_WAL_OFF) return NULL;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return NULL;
    hx_wal_t *w = calloc(1, sizeof(*w));
    if (!w) { close(fd); return NULL; }
    w->fd = fd;
    w->mode = mode;
    w->batch_size = batch_size ? batch_size : 64;
    return w;
}

void hx_wal_close(hx_wal_t *w) {
    if (!w) return;
    if (w->fd >= 0) {
        if (w->unsynced) (void)fsync(w->fd);
        close(w->fd);
    }
    free(w);
}

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void le_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void le_u32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

int hx_wal_append(hx_wal_t *w,
                  hx_wal_rec_type_t type,
                  const char *key, uint16_t key_len,
                  const void *value, uint32_t value_len) {
    if (!w) return 0;  /* WAL off, no-op */
    /* Header (excluding crc): magic(4) type(1) flags(1) key_len(2) value_len(4) */
    uint8_t hdr[12];
    le_u32(hdr, HX_WAL_MAGIC);
    hdr[4] = (uint8_t)type;
    hdr[5] = 0;  /* flags */
    le_u16(hdr + 6, key_len);
    le_u32(hdr + 8, value_len);

    /* CRC over (type, flags, key_len, value_len, key, value). The magic is
     * excluded so a torn write that lands only the magic is still detected. */
    uint32_t c = HX_CRC_INIT;
    c = crc_update(c, hdr + 4, 8);
    if (key_len)   c = crc_update(c, key, key_len);
    if (value_len) c = crc_update(c, value, value_len);
    uint32_t crc = HX_CRC_FINAL(c);

    uint8_t crc_buf[4];
    le_u32(crc_buf, crc);

    if (write_all(w->fd, hdr, sizeof(hdr)) != 0) return -1;
    if (write_all(w->fd, crc_buf, sizeof(crc_buf)) != 0) return -1;
    if (key_len   && write_all(w->fd, key, key_len) != 0) return -1;
    if (value_len && write_all(w->fd, value, value_len) != 0) return -1;

    w->unsynced++;
    switch (w->mode) {
    case HELIX_WAL_PER_WRITE:
        if (fsync(w->fd) != 0) return -1;
        w->unsynced = 0;
        break;
    case HELIX_WAL_BATCHED:
        if (w->unsynced >= w->batch_size) {
            if (fsync(w->fd) != 0) return -1;
            w->unsynced = 0;
        }
        break;
    case HELIX_WAL_ASYNC:
    case HELIX_WAL_OFF:
        break;
    }
    return 0;
}

int hx_wal_sync(hx_wal_t *w) {
    if (!w) return 0;
    if (fsync(w->fd) != 0) return -1;
    w->unsynced = 0;
    return 0;
}

/* --- Reader ---------------------------------------------------------- */

struct hx_wal_reader {
    int fd;
};

hx_wal_reader_t *hx_wal_reader_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    hx_wal_reader_t *r = calloc(1, sizeof(*r));
    if (!r) { close(fd); return NULL; }
    r->fd = fd;
    return r;
}

void hx_wal_reader_close(hx_wal_reader_t *r) {
    if (!r) return;
    close(r->fd);
    free(r);
}

static int read_full(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n == 0) return (int)got;   /* short read at EOF */
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)n;
    }
    return (int)got;
}

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int hx_wal_reader_next(hx_wal_reader_t *r,
                       hx_wal_rec_type_t *type_out,
                       char **key_out, uint16_t *key_len_out,
                       void **value_out, uint32_t *value_len_out) {
    uint8_t hdr[12];
    int n = read_full(r->fd, hdr, sizeof(hdr));
    if (n == 0) return 0;                /* clean EOF */
    if (n != (int)sizeof(hdr)) return 0; /* torn header — treat as EOF */
    if (rd_u32(hdr) != HX_WAL_MAGIC) return -1;

    uint8_t  type  = hdr[4];
    uint16_t klen  = rd_u16(hdr + 6);
    uint32_t vlen  = rd_u32(hdr + 8);

    uint8_t crc_buf[4];
    if (read_full(r->fd, crc_buf, 4) != 4) return 0;
    uint32_t crc_expected = rd_u32(crc_buf);

    char  *key   = klen ? malloc(klen + 1) : NULL;
    void  *value = vlen ? malloc(vlen)     : NULL;
    if ((klen && !key) || (vlen && !value)) { free(key); free(value); return -1; }

    if (klen && read_full(r->fd, key, klen) != (int)klen) {
        free(key); free(value); return 0; /* torn tail */
    }
    if (vlen && read_full(r->fd, value, vlen) != (int)vlen) {
        free(key); free(value); return 0;
    }
    if (key) key[klen] = '\0';

    uint32_t c = HX_CRC_INIT;
    c = crc_update(c, hdr + 4, 8);
    if (klen) c = crc_update(c, key, klen);
    if (vlen) c = crc_update(c, value, vlen);
    uint32_t crc_actual = HX_CRC_FINAL(c);

    if (crc_actual != crc_expected) {
        free(key); free(value); return -1;
    }

    *type_out      = (hx_wal_rec_type_t)type;
    *key_out       = key;
    *key_len_out   = klen;
    *value_out     = value;
    *value_len_out = vlen;
    return 1;
}
