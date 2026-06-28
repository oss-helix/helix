package io.osshelix.demo

import com.fasterxml.jackson.databind.ObjectMapper
import org.springframework.beans.factory.annotation.Value
import org.springframework.http.MediaType
import org.springframework.stereotype.Component
import org.springframework.web.reactive.function.client.WebClient
import java.time.Duration

/**
 * Idempotency primitive backed by the Helix daemon.
 *
 *   helix.dedupe("payment:tx-abc", ttlMs = 60_000) {
 *       chargeCard(...)        // runs once per tx-abc
 *   }
 *
 * Implementation: the wrapper serializes `block`'s return value to JSON
 * and POSTs it to `/v1/idempotent?key=...`. The daemon stores the bytes
 * under `key` for `ttlMs` on the first call and returns 201; subsequent
 * calls within the window get 200 with the originally stored bytes —
 * so the side-effecting `block` runs at most once across the cluster
 * for the same key.
 *
 * **Important:** this is at-most-once only for the *cached response*.
 * If your block has side effects (DB writes, external API calls), call
 * `dedupe` *around* the side effect so retries don't repeat the work.
 */
@Component
class HelixIdempotentClient(
    @Value("\${helix.daemon-url:http://localhost:9099}") private val daemonUrl: String,
    private val mapper: ObjectMapper,
) {
    private val client: WebClient = WebClient.builder().baseUrl(daemonUrl).build()

    fun <T : Any> dedupe(key: String, ttlMs: Long, type: Class<T>, block: () -> T): T {
        val bytes = mapper.writeValueAsBytes(block())
        val storedBytes: ByteArray = client.post()
            .uri { it.path("/v1/idempotent")
                     .queryParam("key", key)
                     .queryParam("ttl_ms", ttlMs)
                     .build() }
            .contentType(MediaType.APPLICATION_OCTET_STREAM)
            .bodyValue(bytes)
            .retrieve()
            .bodyToMono(ByteArray::class.java)
            .block(Duration.ofSeconds(5))
            ?: error("Helix daemon returned no body for /v1/idempotent")

        // 201 = first call (echoes our bytes back); 200 = replay (returns the
        // originally stored bytes). Either way the body holds the canonical
        // response, so we just decode it.
        return mapper.readValue(storedBytes, type)
    }
}
