/*
 * WAL roundtrip:
 *   1. Open a WAL, append a mix of STATE_UPDATE and STATE_DELETE records.
 *   2. Close, reopen with the reader, replay every record.
 *   3. Tamper with one byte and verify the reader reports corruption.
 */
#include "helix/internal/wal.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *PATH = "build/test.wal";

static void write_records(void) {
    unlink(PATH);
    hx_wal_t *w = hx_wal_open(PATH, HELIX_WAL_PER_WRITE, 1);
    assert(w);

    for (int i = 0; i < 100; ++i) {
        char key[32]; snprintf(key, sizeof(key), "order-%d", i);
        char val[64]; snprintf(val, sizeof(val), "{state:%d}", i);
        int rc = hx_wal_append(w, HX_WAL_REC_STATE_UPDATE,
                               key, (uint16_t)strlen(key),
                               val, (uint32_t)strlen(val));
        assert(rc == 0);
    }
    /* one delete */
    int rc = hx_wal_append(w, HX_WAL_REC_STATE_DELETE, "order-50", 8, NULL, 0);
    assert(rc == 0);
    hx_wal_close(w);
}

static void read_records(void) {
    hx_wal_reader_t *r = hx_wal_reader_open(PATH);
    assert(r);

    int updates = 0, deletes = 0;
    for (;;) {
        hx_wal_rec_type_t type;
        char *key = NULL; uint16_t klen = 0;
        void *val = NULL; uint32_t vlen = 0;
        int n = hx_wal_reader_next(r, &type, &key, &klen, &val, &vlen);
        if (n == 0) break;
        assert(n == 1);
        if (type == HX_WAL_REC_STATE_UPDATE) updates++;
        else if (type == HX_WAL_REC_STATE_DELETE) deletes++;
        free(key); free(val);
    }
    hx_wal_reader_close(r);
    assert(updates == 100);
    assert(deletes == 1);
}

static void corruption_detected(void) {
    /* Flip a byte deep inside the file. */
    int fd = open(PATH, O_RDWR);
    assert(fd >= 0);
    struct stat st;
    fstat(fd, &st);
    off_t target = st.st_size / 2;
    uint8_t b;
    pread(fd, &b, 1, target);
    b ^= 0xFF;
    pwrite(fd, &b, 1, target);
    close(fd);

    hx_wal_reader_t *r = hx_wal_reader_open(PATH);
    assert(r);
    int saw_corruption = 0;
    for (;;) {
        hx_wal_rec_type_t type;
        char *key = NULL; uint16_t klen = 0;
        void *val = NULL; uint32_t vlen = 0;
        int n = hx_wal_reader_next(r, &type, &key, &klen, &val, &vlen);
        free(key); free(val);
        if (n == 0) break;
        if (n == -1) { saw_corruption = 1; break; }
    }
    hx_wal_reader_close(r);
    assert(saw_corruption);
}

int main(void) {
    mkdir("build", 0755);
    write_records();
    read_records();
    corruption_detected();
    unlink(PATH);
    printf("OK: wal roundtrip + corruption detection\n");
    return 0;
}
