/*
 * Helix lease daemon — exposes the runtime to non-C clients (Spring Boot,
 * Go, anything that speaks HTTP) as a per-key serialization service.
 *
 * Behavior:
 *   POST /v1/lease   {"key": "...", "ttl_ms": N}   -> blocks until your turn
 *   POST /v1/release {"lease": "..."}              -> frees the slot
 *   GET  /v1/health                                -> "ok"
 *   GET  /v1/stats                                 -> {"active": N, "pending": M}
 *
 * With W workers, at most W keys are "in service" simultaneously. New leases
 * for an already-active key queue up FIFO on the owning worker. This is the
 * waiting-room-during-traffic-spike behavior the daemon exists to provide.
 *
 * Usage:
 *   ./build/example_lease_server [port] [workers]
 */
#include "helix/helix.h"
#include "helix/internal/lease.h"
#include "helix/internal/http.h"
#include "helix/internal/cache.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

int main(int argc, char **argv) {
    int port    = (argc > 1) ? atoi(argv[1]) : 8080;
    int workers = (argc > 2) ? atoi(argv[2]) : 64;

    helix_config_t cfg = helix_config_default();
    cfg.worker_count   = (size_t)workers;
    cfg.queue_capacity = 8192;

    helix_runtime_t *rt = helix_runtime_create(&cfg);
    if (!rt) { fprintf(stderr, "runtime_create failed\n"); return 1; }

    lease_registry_t *reg = lease_registry_create(rt);
    if (!reg) { fprintf(stderr, "lease_registry_create failed\n"); helix_runtime_destroy(rt); return 1; }

    hx_cache_t *cache = hx_cache_create(0);
    if (!cache) {
        fprintf(stderr, "cache_create failed\n");
        lease_registry_destroy(reg); helix_runtime_destroy(rt); return 1;
    }

    hx_http_server_t *srv = hx_http_server_start(reg, cache, port);
    if (!srv) {
        fprintf(stderr, "http_server_start failed on :%d (port in use?)\n", port);
        hx_cache_destroy(cache);
        lease_registry_destroy(reg);
        helix_runtime_destroy(rt);
        return 1;
    }

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);
    fprintf(stderr, "helix lease daemon listening on :%d (workers=%d)\n", port, workers);

    while (!g_stop) sleep(1);

    fprintf(stderr, "\nshutting down...\n");
    hx_http_server_stop(srv);
    hx_cache_destroy(cache);
    lease_registry_destroy(reg);
    helix_runtime_destroy(rt);
    return 0;
}
