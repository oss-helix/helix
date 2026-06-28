package io.osshelix.demo

import org.springframework.stereotype.Repository
import java.util.concurrent.ConcurrentHashMap

data class Seat(val id: String, val reserved: Boolean, val reservedBy: String?)

class SeatAlreadyReservedException(val seatId: String) :
    RuntimeException("Seat $seatId already reserved")

/**
 * Stand-in for a real DB. The reservation logic is deliberately the classic
 * "check then set" race — without external serialization, two concurrent
 * callers can both see `reserved=false` and both write `reserved=true`.
 *
 * In this demo the `HelixLeaseClient.withLease` wrapper in
 * [SeatController.reserve] makes that race impossible by routing all callers
 * for the same seat through one Helix worker.
 */
@Repository
class SeatRepository {
    private val seats = ConcurrentHashMap<String, Seat>()

    fun find(id: String): Seat = seats.computeIfAbsent(id) { Seat(id, false, null) }

    fun findAll(): List<Seat> = seats.values.sortedBy { it.id }

    fun reserve(id: String, userId: String): Seat {
        val seat = find(id)
        if (seat.reserved) throw SeatAlreadyReservedException(id)
        val updated = Seat(id = id, reserved = true, reservedBy = userId)
        seats[id] = updated
        return updated
    }

    fun reset() = seats.clear()
}
