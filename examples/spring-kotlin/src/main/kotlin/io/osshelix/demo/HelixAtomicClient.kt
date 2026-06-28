package io.osshelix.demo

import org.springframework.beans.factory.annotation.Value
import org.springframework.stereotype.Component
import org.springframework.web.reactive.function.client.WebClient
import java.time.Duration

/**
 * Atomic operations against the Helix daemon's cache. Both calls hold the
 * cache bucket mutex across read+write, so the operations are cluster-wide
 * atomic — no Lua scripts, no MULTI/EXEC.
 *
 *   atomic.incr("clicks:home", by = 1, ttlMs = 86_400_000)
 *   atomic.cas("order:42:status", expected = "PAID", next = "REFUNDED")
 */
@Component
class HelixAtomicClient(
    @Value("\${helix.daemon-url:http://localhost:9099}") private val daemonUrl: String,
) {
    private val client: WebClient = WebClient.builder().baseUrl(daemonUrl).build()

    data class IncrResult(val value: Long)
    data class CasResult(val swapped: Boolean)

    /** Atomically adds `by` to the integer stored at `key`. Missing key = 0. */
    fun incr(key: String, by: Long = 1, ttlMs: Long = 0): Long {
        val body = mapOf("key" to key, "by" to by, "ttl_ms" to ttlMs)
        val r = client.post().uri("/v1/atomic/incr")
            .bodyValue(body)
            .retrieve()
            .bodyToMono(IncrResult::class.java)
            .block(Duration.ofSeconds(2))
            ?: error("incr returned no body")
        return r.value
    }

    /**
     * Compare-and-swap. Sets `key`=`next` iff its current value equals
     * `expected`. Pass `expected = ""` to mean "absent". Returns true
     * if the swap happened.
     */
    fun cas(key: String, expected: String, next: String, ttlMs: Long = 0): Boolean {
        val body = mapOf("key" to key, "expected" to expected, "next" to next, "ttl_ms" to ttlMs)
        val r = client.post().uri("/v1/atomic/cas")
            .bodyValue(body)
            .retrieve()
            .bodyToMono(CasResult::class.java)
            .block(Duration.ofSeconds(2))
            ?: error("cas returned no body")
        return r.swapped
    }
}
