#pragma once

// Task ci-once-multi-test-budget (59 design section 3.1, C3/D-01/D-02): centralized
// shutdown/cap write helpers for the client worker-pool queues.
//
// Defect being closed (lost wakeup): every stop/cap flag was stored OUTSIDE the queue
// mutex and then notified. A worker inside cv.wait(lock, pred) releases the mutex at the
// wait, re-acquires it to re-check the predicate, and blocks only if the predicate is
// still false -- but the flag STORE is not ordered against that check: if the writer
// stores between the worker's last predicate check and the worker actually entering the
// wait, the notify_all fires with no waiter yet parked and the worker then blocks on a
// predicate that only becomes true after ANOTHER notify -- i.e. the join hangs until an
// unrelated wakeup (the 120s idle watchdog). Storing the flag INSIDE the same mutex the
// waiter holds closes the window by construction: the store cannot interleave between
// the predicate check and the parked wait, because both the check and the store need the
// mutex.
//
// Contract (deliberately narrow, 59 D-02 -- no callback form):
//   - exactly ONE mutex is taken; no nesting, no allocation, no I/O, no join, no second
//     lock (so no new AB-BA surface; R-01);
//   - noexcept: a violation of the caller contract (calling while already holding `mu`)
//     surfaces as loud termination rather than a silent hang;
//   - the flag itself stays a relaxed atomic: the mutex provides the ordering, the
//     predicate reads it under the same mutex, and nothing else depends on the store's
//     memory order;
//   - CALLER CONTRACT: the calling thread must NOT already hold `mu` (all 11 client
//     call sites and both ComparePipeline methods verified against this; MSVC throws
//     system_error{resource_deadlock_would_occur} on recursive std::mutex acquisition).
//
// recvStop is deliberately NOT a user of these helpers: the recv threads block
// synchronously inside RecvFrame() on the socket, never on a condition variable whose
// predicate reads recvStop, and their wakeup is ShutdownBoth() -- see sync_engine_client
// (59 D-03; giving recvStop the incomingMu would also re-introduce the errc 36
// re-acquisition hazard documented at updateStallWatchdog's call sites).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace fc {

// Queue-stop write: store stop=true under the waiter's mutex, then wake every waiter.
// The notify is issued after the unlock (harmless: any waiter woken re-locks the mutex,
// which orders it after the completed store).
inline void ShutdownFlagged(std::mutex& mu, std::condition_variable& cv,
                            std::atomic<bool>& stop) noexcept {
    {
        std::lock_guard<std::mutex> lock(mu);
        stop.store(true, std::memory_order_relaxed);
    }
    cv.notify_all();
}

// Active-cap write (writeActiveCap / ComparePipeline::activeCap_): same one-mutex rule,
// relaxed store under the gate mutex, then wake the parked workers so a raised cap takes
// effect immediately (D-09).
inline void SetCapFlagged(std::mutex& mu, std::condition_variable& cv,
                          std::atomic<uint32_t>& cap, uint32_t value) noexcept {
    {
        std::lock_guard<std::mutex> lock(mu);
        cap.store(value, std::memory_order_relaxed);
    }
    cv.notify_all();
}

}  // namespace fc
