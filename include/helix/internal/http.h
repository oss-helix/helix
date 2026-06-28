/*
 * Tiny HTTP/1.1 server for the lease daemon. One thread per accepted
 * connection; Connection: close after each response. Just enough to expose
 * `lease_acquire` / `lease_release` over the wire.
 *
 * Endpoints:
 *
 *   POST /v1/lease
 *       request:  {"key": "...", "ttl_ms": 30000}
 *       response: 200 {"lease": "...", "key": "...", "ttl_ms": ...}
 *                 408 if no slot is acquired before the TTL
 *
 *   POST /v1/release
 *       request:  {"lease": "..."}
 *       response: 200 {"ok": true}
 *                 404 if the lease is unknown or already expired
 *
 *   GET    /v1/cache/{key}              -> 200 + body, or 404
 *   PUT    /v1/cache/{key}?ttl_ms=N     -> 204; body is the value (any bytes)
 *   DELETE /v1/cache/{key}              -> 204, or 404
 *
 *   GET /v1/health   -> 200 "ok"
 *   GET /v1/stats    -> 200 {"active": N, "pending": M, "cache": {...}}
 */
#ifndef HELIX_INTERNAL_HTTP_H
#define HELIX_INTERNAL_HTTP_H

#include "helix/internal/lease.h"
#include "helix/internal/cache.h"

typedef struct hx_http_server hx_http_server_t;

/* Binds to 0.0.0.0:port and starts the accept loop in a background thread.
 * Either `r` or `c` may be NULL to disable that subsystem's routes.
 * Returns NULL on bind/listen failure. */
hx_http_server_t *hx_http_server_start(lease_registry_t *r, hx_cache_t *c, int port);

/* Stops the accept loop and joins the listener thread. In-flight handlers
 * are best-effort completed; the registry can be destroyed afterwards. */
void hx_http_server_stop(hx_http_server_t *s);

#endif
