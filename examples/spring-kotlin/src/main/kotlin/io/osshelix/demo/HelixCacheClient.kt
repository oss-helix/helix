package io.osshelix.demo

import com.fasterxml.jackson.core.type.TypeReference
import com.fasterxml.jackson.databind.ObjectMapper
import org.springframework.beans.factory.annotation.Value
import org.springframework.http.HttpStatus
import org.springframework.http.MediaType
import org.springframework.stereotype.Component
import org.springframework.web.reactive.function.client.WebClient
import org.springframework.web.reactive.function.client.WebClientResponseException
import java.time.Duration

/**
 * Thin client for the Helix daemon's cache API.
 *
 *   GET    /v1/cache/{key}
 *   PUT    /v1/cache/{key}?ttl_ms=N
 *   DELETE /v1/cache/{key}
 *
 * For application code the [getOrLoad] helpers implement the standard
 * cache-aside pattern:
 *
 *   // single object
 *   val seat = cache.getOrLoad("seat:$id", 5_000, Seat::class.java) { repo.find(id) }
 *
 *   // generic / collection
 *   val seats = cache.getOrLoad(
 *       "seats:list", 5_000, object : TypeReference<List<Seat>>() {}
 *   ) { repo.findAll() }
 *
 * Useful for read-heavy endpoints (list / discovery / detail pages) where
 * the underlying DB query is the bottleneck. For write contention, use
 * [HelixLeaseClient] instead.
 */
@Component
class HelixCacheClient(
    @Value("\${helix.daemon-url:http://localhost:9099}") private val daemonUrl: String,
    private val mapper: ObjectMapper,
) {
    private val client: WebClient = WebClient.builder().baseUrl(daemonUrl).build()

    /** Returns the cached bytes, or null on miss. */
    fun get(key: String): ByteArray? {
        return try {
            client.get()
                .uri("/v1/cache/{key}", key)
                .retrieve()
                .toEntity(ByteArray::class.java)
                .block(Duration.ofSeconds(2))
                ?.body
        } catch (e: WebClientResponseException) {
            if (e.statusCode == HttpStatus.NOT_FOUND) null else throw e
        }
    }

    /** Stores `value` under `key` with the given TTL (0 = no expiry). */
    fun put(key: String, value: ByteArray, ttlMs: Long) {
        client.put()
            .uri { it.path("/v1/cache/$key").queryParam("ttl_ms", ttlMs).build() }
            .contentType(MediaType.APPLICATION_OCTET_STREAM)
            .bodyValue(value)
            .retrieve()
            .toBodilessEntity()
            .block(Duration.ofSeconds(2))
    }

    fun delete(key: String) {
        runCatching {
            client.delete()
                .uri("/v1/cache/{key}", key)
                .retrieve()
                .toBodilessEntity()
                .block(Duration.ofSeconds(2))
        }
    }

    /** Cache-aside for a single object. */
    fun <T : Any> getOrLoad(key: String, ttlMs: Long, type: Class<T>, loader: () -> T): T {
        val cached = get(key)
        if (cached != null) {
            return mapper.readValue(cached, type)
        }
        val fresh = loader()
        put(key, mapper.writeValueAsBytes(fresh), ttlMs)
        return fresh
    }

    /** Cache-aside for generic / collection types (use a TypeReference). */
    fun <T : Any> getOrLoad(key: String, ttlMs: Long, type: TypeReference<T>, loader: () -> T): T {
        val cached = get(key)
        if (cached != null) {
            return mapper.readValue(cached, type)
        }
        val fresh = loader()
        put(key, mapper.writeValueAsBytes(fresh), ttlMs)
        return fresh
    }
}
