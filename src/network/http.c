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
#include "helix/internal/cache.h"
#include "helix/internal/runtime.h"

#include <stdarg.h>

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
    atomic_int           draining;
    helix_runtime_t     *runtime;
    lease_registry_t    *registry;
    hx_cache_t          *cache;
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

/* Like send_response but for binary bodies of known length. */
static void send_bytes(int fd, int status, const char *reason,
                       const char *content_type,
                       const void *body, size_t body_len) {
    char hdr[512];
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

static void handle_stats(lease_registry_t *r, hx_cache_t *c, int fd) {
    char resp[256];
    size_t active  = r ? lease_active_count(r)  : 0;
    size_t pending = r ? lease_pending_count(r) : 0;
    size_t csize   = c ? hx_cache_size(c)       : 0;
    size_t chits   = c ? hx_cache_hits(c)       : 0;
    size_t cmisses = c ? hx_cache_misses(c)     : 0;
    snprintf(resp, sizeof(resp),
             "{\"active\":%zu,\"pending\":%zu,"
             "\"cache\":{\"size\":%zu,\"hits\":%zu,\"misses\":%zu}}",
             active, pending, csize, chits, cmisses);
    send_json(fd, 200, "OK", resp);
}

/* In-place URL %XX decoding. Lenient: a malformed escape is left as-is. */
static void url_decode_inplace(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Splits a request path of the form `/v1/cache/{key}[?ttl_ms=N]` into
 *   key (caller-provided buffer, NUL-terminated, URL-decoded in place)
 *   ttl_ms (0 if absent)
 * Returns 1 on success, 0 if the path doesn't match.
 *
 * The HTTP request path is mutated to nul-terminate the key in place. */
static int parse_cache_path(char *path, char **key_out, int *ttl_out) {
    const char prefix[] = "/v1/cache/";
    if (strncmp(path, prefix, sizeof(prefix) - 1) != 0) return 0;
    char *key = path + sizeof(prefix) - 1;
    if (*key == '\0') return 0;

    *ttl_out = 0;
    char *q = strchr(key, '?');
    if (q) {
        *q = '\0';
        char *p = q + 1;
        while (*p) {
            if (strncmp(p, "ttl_ms=", 7) == 0) {
                *ttl_out = atoi(p + 7);
                break;
            }
            char *amp = strchr(p, '&');
            if (!amp) break;
            p = amp + 1;
        }
    }
    url_decode_inplace(key);
    *key_out = key;
    return 1;
}

static void handle_cache_get(hx_cache_t *c, int fd, char *path) {
    char *key; int ttl_unused;
    if (!parse_cache_path(path, &key, &ttl_unused)) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"missing key\"}");
        return;
    }
    size_t len = 0;
    void *value = hx_cache_get(c, key, &len);
    if (!value) {
        send_json(fd, 404, "Not Found", "{\"error\":\"miss\"}");
        return;
    }
    send_bytes(fd, 200, "OK", "application/octet-stream", value, len);
    free(value);
}

static void handle_cache_put(hx_cache_t *c, int fd, char *path,
                             const char *body, size_t body_len) {
    char *key; int ttl_ms;
    if (!parse_cache_path(path, &key, &ttl_ms)) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"missing key\"}");
        return;
    }
    if (hx_cache_set(c, key, body, body_len, ttl_ms) != 0) {
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"set failed\"}");
        return;
    }
    send_response(fd, 204, "No Content", "application/json", NULL);
}

static void handle_cache_delete(hx_cache_t *c, int fd, char *path) {
    char *key; int ttl_unused;
    if (!parse_cache_path(path, &key, &ttl_unused)) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"missing key\"}");
        return;
    }
    if (hx_cache_delete(c, key) != 0) {
        send_json(fd, 404, "Not Found", "{\"error\":\"miss\"}");
        return;
    }
    send_response(fd, 204, "No Content", "application/json", NULL);
}

/* Extracts a named query param from a path of the form `...?a=1&b=2`.
 * Writes the (URL-decoded) value into `out` (capacity `out_cap`).
 * Returns 1 on success, 0 if absent. Non-destructive on `path`. */
static int query_param(const char *path, const char *name,
                       char *out, size_t out_cap) {
    const char *q = strchr(path, '?');
    if (!q) return 0;
    size_t nlen = strlen(name);
    const char *p = q + 1;
    while (*p) {
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            size_t i = 0;
            while (*v && *v != '&' && i + 1 < out_cap) out[i++] = *v++;
            out[i] = '\0';
            url_decode_inplace(out);
            return i > 0;
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

static void handle_idempotent(hx_cache_t *c, int fd, const char *path,
                              const char *body, size_t body_len) {
    char key[256], ttl_buf[16];
    if (!query_param(path, "key", key, sizeof(key))) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"missing key\"}");
        return;
    }
    int ttl_ms = 300000;  /* default 5 minutes */
    if (query_param(path, "ttl_ms", ttl_buf, sizeof(ttl_buf))) ttl_ms = atoi(ttl_buf);

    size_t stored_len = 0;
    int    was_new    = 0;
    void  *prior = hx_cache_get_or_set(c, key, body, body_len, ttl_ms,
                                       &stored_len, &was_new);
    if (was_new) {
        /* First call — body was stored, echo it back with 201. */
        send_bytes(fd, 201, "Created", "application/octet-stream", body, body_len);
    } else if (prior) {
        /* Replay — return the memoized response. */
        send_bytes(fd, 200, "OK", "application/octet-stream", prior, stored_len);
        free(prior);
    } else {
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"idempotency lookup failed\"}");
    }
}

static void handle_atomic_incr(hx_cache_t *c, int fd, const char *body) {
    char key[256], by_buf[24], ttl_buf[16];
    if (!json_field(body, "key", key, sizeof(key))) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"missing key\"}"); return;
    }
    long long by = 1;
    if (json_field(body, "by", by_buf, sizeof(by_buf))) by = strtoll(by_buf, NULL, 10);
    int ttl_ms = 0;
    if (json_field(body, "ttl_ms", ttl_buf, sizeof(ttl_buf))) ttl_ms = atoi(ttl_buf);

    long long out_value = 0;
    if (hx_cache_atomic_incr(c, key, by, ttl_ms, &out_value) != 0) {
        send_json(fd, 409, "Conflict", "{\"error\":\"current value is not an integer\"}");
        return;
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"value\":%lld}", out_value);
    send_json(fd, 200, "OK", resp);
}

static void handle_atomic_cas(hx_cache_t *c, int fd, const char *body) {
    char key[256], expected[512], next[512], ttl_buf[16];
    if (!json_field(body, "key", key, sizeof(key))) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"missing key\"}"); return;
    }
    /* expected may legitimately be empty — interpret as "absent". */
    expected[0] = '\0';
    json_field(body, "expected", expected, sizeof(expected));
    if (!json_field(body, "next", next, sizeof(next))) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"missing next\"}"); return;
    }
    int ttl_ms = 0;
    if (json_field(body, "ttl_ms", ttl_buf, sizeof(ttl_buf))) ttl_ms = atoi(ttl_buf);

    int swapped = 0;
    int rc = hx_cache_atomic_cas(c, key,
                                 expected, strlen(expected),
                                 next, strlen(next),
                                 ttl_ms, &swapped);
    if (rc != 0) {
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"cas failed\"}");
        return;
    }
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"swapped\":%s}", swapped ? "true" : "false");
    send_json(fd, 200, "OK", resp);
}

static void metrics_appendf(char *buf, size_t cap, size_t *off,
                            const char *fmt, ...) {
    if (*off >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < cap - *off) *off += (size_t)n;
}

static void handle_metrics(hx_http_server_t *s, int fd) {
    /* Prometheus text exposition format. Keep this small — no histograms
     * for the MVP, just gauges + counters from the live state. */
    char buf[4096];
    size_t off = 0;

    if (s->cache) {
        metrics_appendf(buf, sizeof(buf), &off,
            "# HELP helix_cache_size Number of entries currently in the cache\n"
            "# TYPE helix_cache_size gauge\n"
            "helix_cache_size %zu\n", hx_cache_size(s->cache));
        metrics_appendf(buf, sizeof(buf), &off,
            "# HELP helix_cache_hits_total Cumulative cache GET hits\n"
            "# TYPE helix_cache_hits_total counter\n"
            "helix_cache_hits_total %zu\n", hx_cache_hits(s->cache));
        metrics_appendf(buf, sizeof(buf), &off,
            "# HELP helix_cache_misses_total Cumulative cache GET misses\n"
            "# TYPE helix_cache_misses_total counter\n"
            "helix_cache_misses_total %zu\n", hx_cache_misses(s->cache));
    }
    if (s->registry) {
        metrics_appendf(buf, sizeof(buf), &off,
            "# HELP helix_lease_active Currently held leases\n"
            "# TYPE helix_lease_active gauge\n"
            "helix_lease_active %zu\n", lease_active_count(s->registry));
        metrics_appendf(buf, sizeof(buf), &off,
            "# HELP helix_lease_pending Callers parked in /v1/lease awaiting their turn\n"
            "# TYPE helix_lease_pending gauge\n"
            "helix_lease_pending %zu\n", lease_pending_count(s->registry));
    }
    if (s->runtime) {
        metrics_appendf(buf, sizeof(buf), &off,
            "# HELP helix_workers Number of event-loop worker threads\n"
            "# TYPE helix_workers gauge\n"
            "helix_workers %zu\n", s->runtime->worker_count);
    }
    metrics_appendf(buf, sizeof(buf), &off,
        "# HELP helix_draining 1 if the server has begun shutdown drain, else 0\n"
        "# TYPE helix_draining gauge\n"
        "helix_draining %d\n", atomic_load(&s->draining));

    send_response(fd, 200, "OK", "text/plain; version=0.0.4", buf);
}

/* Drain check — used to short-circuit write routes during shutdown. */
static int draining(const hx_http_server_t *s) {
    return atomic_load(&s->draining);
}

/* Splits "/v1/idempotent?..." into just the path prefix for matching. */
static int path_starts_with(const char *path, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 &&
           (path[n] == '\0' || path[n] == '?' || path[n] == '/');
}

static void *conn_thread(void *arg) {
    conn_ctx_t *ctx = arg;
    hx_request_t req;
    /* Large enough for cached payloads (e.g. cached JSON list responses). */
    char body[65536];
    if (read_request(ctx->fd, &req, body, sizeof(body)) == 0) {
        hx_http_server_t *s = ctx->srv;
        lease_registry_t *r = s->registry;
        hx_cache_t       *c = s->cache;
        int is_cache_path = strncmp(req.path, "/v1/cache/", 10) == 0;

        /* Write-y routes refuse new work while draining. */
        int is_write_route =
            (strcmp(req.method, "POST") == 0 &&
                (strcmp(req.path, "/v1/lease") == 0 ||
                 strcmp(req.path, "/v1/release") == 0 ||
                 path_starts_with(req.path, "/v1/idempotent") ||
                 path_starts_with(req.path, "/v1/atomic"))) ||
            (is_cache_path && (strcmp(req.method, "PUT") == 0 ||
                               strcmp(req.method, "DELETE") == 0));
        if (is_write_route && draining(s)) {
            send_json(ctx->fd, 503, "Service Unavailable",
                      "{\"error\":\"draining\"}");
            goto done;
        }

        if      (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/v1/lease")   == 0) handle_lease(r, ctx->fd, body);
        else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/v1/release") == 0) handle_release(r, ctx->fd, body);
        else if (c && strcmp(req.method, "POST") == 0 && path_starts_with(req.path, "/v1/idempotent"))
            handle_idempotent(c, ctx->fd, req.path, body, (size_t)req.content_length);
        else if (c && strcmp(req.method, "POST") == 0 && strcmp(req.path, "/v1/atomic/incr") == 0)
            handle_atomic_incr(c, ctx->fd, body);
        else if (c && strcmp(req.method, "POST") == 0 && strcmp(req.path, "/v1/atomic/cas") == 0)
            handle_atomic_cas(c, ctx->fd, body);
        else if (strcmp(req.method, "GET")  == 0 && strcmp(req.path, "/v1/health")  == 0) send_response(ctx->fd, 200, "OK", "text/plain", "ok");
        else if (strcmp(req.method, "GET")  == 0 && strcmp(req.path, "/v1/stats")   == 0) handle_stats(r, c, ctx->fd);
        else if (strcmp(req.method, "GET")  == 0 && strcmp(req.path, "/v1/metrics") == 0) handle_metrics(s, ctx->fd);
        else if (c && is_cache_path && strcmp(req.method, "GET")    == 0) handle_cache_get(c, ctx->fd, req.path);
        else if (c && is_cache_path && strcmp(req.method, "PUT")    == 0) handle_cache_put(c, ctx->fd, req.path, body, (size_t)req.content_length);
        else if (c && is_cache_path && strcmp(req.method, "DELETE") == 0) handle_cache_delete(c, ctx->fd, req.path);
        else send_json(ctx->fd, 404, "Not Found", "{\"error\":\"unknown route\"}");
    }
done:
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

hx_http_server_t *hx_http_server_start(helix_runtime_t *rt,
                                        lease_registry_t *r,
                                        hx_cache_t *c,
                                        int port) {
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
    s->runtime   = rt;
    s->registry  = r;
    s->cache     = c;
    atomic_store(&s->draining, 0);
    atomic_store(&s->running, 1);
    if (pthread_create(&s->accept_thread, NULL, accept_loop, s) != 0) {
        close(fd); free(s); return NULL;
    }
    return s;
}

void hx_http_server_drain(hx_http_server_t *s) {
    if (!s) return;
    atomic_store(&s->draining, 1);
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
