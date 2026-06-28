/*
 * Minimal HTTP/1.1 server. Hand-rolled because pulling in libmicrohttpd or
 * mongoose just for four endpoints would be heavier than the runtime itself.
 *
 * The parser is deliberately strict and tiny: it reads until CRLFCRLF for
 * headers, then up to Content-Length bytes for the body. No keep-alive,
 * no chunked encoding, no streaming.
 */
#include "helix/internal/http.h"
#include "helix/internal/lease.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define HX_HTTP_MAX_REQUEST 8192

struct hx_http_server {
    int                  listen_fd;
    int                  port;
    pthread_t            accept_thread;
    atomic_int           running;
    lease_registry_t    *registry;
};

typedef struct {
    hx_http_server_t *srv;
    int               fd;
} conn_ctx_t;

/* --- tiny JSON helpers ---------------------------------------------- */

/* Find the value for `"<field>"` in a JSON-ish blob and copy it into `out`.
 * Handles string values ("...") and unquoted numeric values. Returns 1 on
 * success, 0 if not found. Not a real JSON parser; sufficient for our
 * known 2-3 field bodies. */
static int json_field(const char *body, const char *field, char *out, size_t out_cap) {
    char needle[64];
    int  n = snprintf(needle, sizeof(needle), "\"%s\"", field);
    if (n < 0 || (size_t)n >= sizeof(needle)) return 0;
    const char *p = strstr(body, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p && (*p == ' ' || *p == ':')) ++p;
    size_t i = 0;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"' && i + 1 < out_cap) out[i++] = *p++;
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ' ' && i + 1 < out_cap) out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

/* --- response builders ---------------------------------------------- */

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

static void send_response(int fd, int status, const char *reason,
                          const char *content_type, const char *body) {
    char hdr[512];
    size_t body_len = body ? strlen(body) : 0;
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason, content_type, body_len);
    if (n < 0) return;
    if (write_all(fd, hdr, (size_t)n) != 0) return;
    if (body_len) write_all(fd, body, body_len);
}

static void send_json(int fd, int status, const char *reason, const char *body) {
    send_response(fd, status, reason, "application/json", body);
}

/* --- request parsing ------------------------------------------------ */

typedef struct {
    char method[8];
    char path[128];
    int  content_length;
    /* body is read into a separate buffer */
} hx_request_t;

static int read_request(int fd, hx_request_t *req, char *body, size_t body_cap) {
    char buf[HX_HTTP_MAX_REQUEST];
    size_t total = 0;
    /* Read until CRLFCRLF or buffer full. */
    while (total < sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    char *header_end = strstr(buf, "\r\n\r\n");
    if (!header_end) return -1;

    /* Parse request line. */
    if (sscanf(buf, "%7s %127s", req->method, req->path) != 2) return -1;

    /* Parse Content-Length. Case-insensitive scan; strcasestr is BSD/GNU and
     * not portable under strict POSIX flags. */
    req->content_length = 0;
    const char *cl = NULL;
    for (char *p = buf; p < header_end; ++p) {
        if (strncasecmp(p, "content-length:", 15) == 0) { cl = p; break; }
    }
    if (cl) {
        cl += 15;
        while (*cl == ' ') ++cl;
        req->content_length = atoi(cl);
    }

    /* Copy the body bytes we already read. */
    size_t header_bytes = (size_t)(header_end - buf) + 4;
    size_t body_already = total - header_bytes;
    if ((size_t)req->content_length >= body_cap) return -1;
    memcpy(body, buf + header_bytes, body_already);
    /* Read the rest. */
    while (body_already < (size_t)req->content_length) {
        ssize_t n = read(fd, body + body_already,
                         (size_t)req->content_length - body_already);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        body_already += (size_t)n;
    }
    body[req->content_length] = '\0';
    return 0;
}

/* --- route handlers ------------------------------------------------- */

static void handle_lease(lease_registry_t *r, int fd, const char *body) {
    char key[256], ttl_buf[16];
    if (!json_field(body, "key", key, sizeof(key))) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"missing key\"}");
        return;
    }
    int ttl_ms = 30000;
    if (json_field(body, "ttl_ms", ttl_buf, sizeof(ttl_buf))) ttl_ms = atoi(ttl_buf);

    char *lease_id = lease_acquire(r, key, ttl_ms);
    if (!lease_id) {
        send_json(fd, 408, "Request Timeout",
                  "{\"error\":\"could not acquire lease within ttl\"}");
        return;
    }
    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"lease\":\"%s\",\"key\":\"%s\",\"ttl_ms\":%d}",
             lease_id, key, ttl_ms);
    send_json(fd, 200, "OK", resp);
    free(lease_id);
}

static void handle_release(lease_registry_t *r, int fd, const char *body) {
    char lease_id[64];
    if (!json_field(body, "lease", lease_id, sizeof(lease_id))) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"missing lease\"}");
        return;
    }
    int rc = lease_release(r, lease_id);
    if (rc != 0) {
        send_json(fd, 404, "Not Found", "{\"error\":\"unknown or expired lease\"}");
        return;
    }
    send_json(fd, 200, "OK", "{\"ok\":true}");
}

static void handle_stats(lease_registry_t *r, int fd) {
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"active\":%zu,\"pending\":%zu}",
             lease_active_count(r), lease_pending_count(r));
    send_json(fd, 200, "OK", resp);
}

static void *conn_thread(void *arg) {
    conn_ctx_t *ctx = arg;
    hx_request_t req;
    char body[2048];
    if (read_request(ctx->fd, &req, body, sizeof(body)) == 0) {
        lease_registry_t *r = ctx->srv->registry;
        if      (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/v1/lease")   == 0) handle_lease(r, ctx->fd, body);
        else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/v1/release") == 0) handle_release(r, ctx->fd, body);
        else if (strcmp(req.method, "GET")  == 0 && strcmp(req.path, "/v1/health")  == 0) send_response(ctx->fd, 200, "OK", "text/plain", "ok");
        else if (strcmp(req.method, "GET")  == 0 && strcmp(req.path, "/v1/stats")   == 0) handle_stats(r, ctx->fd);
        else send_json(ctx->fd, 404, "Not Found", "{\"error\":\"unknown route\"}");
    }
    close(ctx->fd);
    free(ctx);
    return NULL;
}

/* --- accept loop ---------------------------------------------------- */

static void *accept_loop(void *arg) {
    hx_http_server_t *s = arg;
    while (atomic_load(&s->running)) {
        int cfd = accept(s->listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (!atomic_load(&s->running)) break;
            continue;
        }
        int yes = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        conn_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) { close(cfd); continue; }
        ctx->srv = s;
        ctx->fd  = cfd;
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, ctx) != 0) {
            close(cfd); free(ctx); continue;
        }
        pthread_detach(t);
    }
    return NULL;
}

hx_http_server_t *hx_http_server_start(lease_registry_t *r, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return NULL; }
    if (listen(fd, 128) < 0) { close(fd); return NULL; }

    /* Don't let a write to a closed socket kill the process. */
    signal(SIGPIPE, SIG_IGN);

    hx_http_server_t *s = calloc(1, sizeof(*s));
    if (!s) { close(fd); return NULL; }
    s->listen_fd = fd;
    s->port      = port;
    s->registry  = r;
    atomic_store(&s->running, 1);
    if (pthread_create(&s->accept_thread, NULL, accept_loop, s) != 0) {
        close(fd); free(s); return NULL;
    }
    return s;
}

void hx_http_server_stop(hx_http_server_t *s) {
    if (!s) return;
    atomic_store(&s->running, 0);
    /* Wake the accept by shutting down the listener. */
    shutdown(s->listen_fd, SHUT_RDWR);
    close(s->listen_fd);
    pthread_join(s->accept_thread, NULL);
    free(s);
}
