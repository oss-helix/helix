package io.osshelix.demo

import com.fasterxml.jackson.core.type.TypeReference
import org.springframework.http.HttpStatus
import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.RequestBody
import org.springframework.web.bind.annotation.RequestMapping
import org.springframework.web.bind.annotation.RestController

data class ReserveRequest(val userId: String)
data class ReserveResponse(val seat: Seat, val waitMs: Long)

@RestController
@RequestMapping("/seats")
class SeatController(
    private val helix: HelixLeaseClient,
    private val cache: HelixCacheClient,
    private val repo: SeatRepository,
) {
    /**
     * Write path — lease-protected. Every concurrent request for the same
     * seat queues up FIFO inside Helix; only one runs the read-modify-write
     * at a time. The cache entry for the seat is invalidated on success so
     * subsequent reads observe the new state.
     */
    @PostMapping("/{id}/reserve")
    fun reserve(@PathVariable id: String, @RequestBody req: ReserveRequest): ResponseEntity<Any> {
        val started = System.currentTimeMillis()
        return try {
            val updated = helix.withLease("seat-$id") {
                repo.reserve(id, req.userId)
            }
            cache.delete("seat:$id")
            cache.delete("seats:list")
            ResponseEntity.ok(ReserveResponse(updated, System.currentTimeMillis() - started))
        } catch (e: SeatAlreadyReservedException) {
            ResponseEntity.status(HttpStatus.CONFLICT)
                .body(mapOf("error" to e.message, "seatId" to e.seatId))
        }
    }

    @PostMapping("/reset")
    fun reset(): ResponseEntity<Map<String, Boolean>> {
        repo.reset()
        cache.delete("seats:list")
        return ResponseEntity.ok(mapOf("ok" to true))
    }

    /**
     * Read path — cache-aside. Hot detail pages are served from Helix's
     * in-memory cache; a miss falls through to the repo and repopulates.
     */
    @GetMapping("/{id}")
    fun get(@PathVariable id: String): ResponseEntity<Seat> {
        val seat = cache.getOrLoad("seat:$id", 5_000, Seat::class.java) { repo.find(id) }
        return ResponseEntity.ok(seat)
    }

    /**
     * Read path — list endpoint backed by the cache. This is the
     * "show all seats" page that read-heavy traffic hits hardest.
     * One DB hit per cache miss; everything else is served from memory.
     */
    @GetMapping
    fun list(): ResponseEntity<List<Seat>> {
        val seats = cache.getOrLoad(
            "seats:list", 5_000, object : TypeReference<List<Seat>>() {}
        ) { repo.findAll() }
        return ResponseEntity.ok(seats)
    }
}
