package io.osshelix.demo

import org.springframework.beans.factory.annotation.Value
import org.springframework.stereotype.Component
import org.springframework.web.reactive.function.client.WebClient
import org.springframework.web.reactive.function.client.bodyToMono
import java.time.Duration

/**
 * Thin client that calls the Helix daemon's HTTP API to acquire a serialization
 * slot for a given key. The slot blocks all other callers competing for the
 * same key until [withLease] returns; concurrent calls for *different* keys
 * do not block each other.
 *
 * The contract is:
 *
 *   helix.withLease("seat-A1") {
 *       // read-modify-write for seat-A1 is single-threaded across the cluster
 *       repo.reserve("A1", userId)
 *   }
 *
 * The lease is released on the way out (including on exceptions).
 */
@Component
class HelixLeaseClient(
    @Value("\${helix.daemon-url:http://localhost:9099}") private val daemonUrl: String,
) {
    private val client: WebClient = WebClient.builder().baseUrl(daemonUrl).build()

    fun <T> withLease(key: String, ttlMs: Long = DEFAULT_TTL_MS, block: () -> T): T {
        val lease = acquire(key, ttlMs)
        try {
            return block()
        } finally {
            // Best-effort — if release fails the lease still expires via TTL.
            runCatching { release(lease.lease) }
        }
    }

    private fun acquire(key: String, ttlMs: Long): LeaseResponse {
        return client.post()
            .uri("/v1/lease")
            .bodyValue(mapOf("key" to key, "ttl_ms" to ttlMs))
            .retrieve()
            .bodyToMono<LeaseResponse>()
            .block(Duration.ofMillis(ttlMs + 1_000))
            ?: error("Helix daemon returned an empty body for /v1/lease")
    }

    private fun release(leaseId: String) {
        client.post()
            .uri("/v1/release")
            .bodyValue(mapOf("lease" to leaseId))
            .retrieve()
            .bodyToMono<Map<String, Any>>()
            .block(Duration.ofSeconds(5))
    }

    private companion object {
        const val DEFAULT_TTL_MS: Long = 30_000
    }

    data class LeaseResponse(val lease: String, val key: String, val ttl_ms: Long)
}
