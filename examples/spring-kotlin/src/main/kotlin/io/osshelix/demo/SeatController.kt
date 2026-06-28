package io.osshelix.demo

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
    private val repo: SeatRepository,
) {
    @PostMapping("/{id}/reserve")
    fun reserve(@PathVariable id: String, @RequestBody req: ReserveRequest): ResponseEntity<Any> {
        val started = System.currentTimeMillis()
        return try {
            // Every concurrent request for seat $id queues up FIFO inside
            // Helix; only one runs the read-modify-write at a time.
            val updated = helix.withLease("seat-$id") {
                repo.reserve(id, req.userId)
            }
            ResponseEntity.ok(ReserveResponse(updated, System.currentTimeMillis() - started))
        } catch (e: SeatAlreadyReservedException) {
            ResponseEntity.status(HttpStatus.CONFLICT)
                .body(mapOf("error" to e.message, "seatId" to e.seatId))
        }
    }

    @PostMapping("/reset")
    fun reset(): ResponseEntity<Map<String, Boolean>> {
        repo.reset()
        return ResponseEntity.ok(mapOf("ok" to true))
    }

    @GetMapping("/{id}")
    fun get(@PathVariable id: String): ResponseEntity<Seat> =
        ResponseEntity.ok(repo.find(id))
}
