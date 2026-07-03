// Unit tests for the server read-concurrency gate (delta-streaming-fix V-05 / AC-05 / FR-14 /
// FR-18 / R-04). Exercises the REAL fc::ReadGate / fc::ReadGateGuard types (header-only, so no
// network stack is pulled into the test target, design D-04): concurrency is capped at the limit,
// permits are always returned (including on the exception path), and acquire() aborts on `done`.

#include "read_gate.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(std::string("read_gate: ") + msg);
    }
}

// AC-05 / FR-14: with limit=3 and 16 contending threads, the observed concurrent-holder count
// never exceeds 3, and every thread eventually finishes (no deadlock).
void TestConcurrencyCapped() {
    constexpr uint32_t kLimit = 3;
    fc::ReadGate gate(kLimit);
    std::atomic<bool> done{false};
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> completed{0};

    std::vector<std::thread> threads;
    threads.reserve(16);
    for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&]() {
            fc::ReadGateGuard guard(gate, done);
            Require(static_cast<bool>(guard), "guard held when done never set");
            const int now = active.fetch_add(1) + 1;
            int prev = peak.load();
            while (now > prev && !peak.compare_exchange_weak(prev, now)) {
                // retry until peak >= now
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            active.fetch_sub(1);
            completed.fetch_add(1);
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    Require(peak.load() <= static_cast<int>(kLimit), "observed concurrency never exceeds limit");
    Require(peak.load() >= 1, "at least one holder observed");
    Require(completed.load() == 16, "all 16 tasks completed (no deadlock)");
}

// FR-18 / AC-09: when `done` is set and no permit is free, a blocked acquire() returns false
// (aborts) rather than waiting forever.
void TestDoneAbortsWait() {
    fc::ReadGate gate(1);
    std::atomic<bool> done{false};

    fc::ReadGateGuard held(gate, done);  // takes the only permit
    Require(static_cast<bool>(held), "first acquire holds the sole permit");

    std::atomic<bool> aborted{false};
    std::atomic<bool> finished{false};
    std::thread waiter([&]() {
        const bool got = gate.acquire(done);  // must block: no permit free
        aborted.store(!got);
        finished.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Require(!finished.load(), "waiter is blocked while a permit is unavailable");

    done.store(true);
    gate.notify_all_waiters();  // wake the blocked waiter so it re-checks the predicate
    waiter.join();
    Require(aborted.load(), "blocked acquire returns false once done is set (FR-18)");
}

// R-04: the RAII guard releases its permit even when the guarded region throws, so the gate is
// fully restored afterwards.
void TestGuardReleasesOnException() {
    fc::ReadGate gate(2);
    std::atomic<bool> done{false};

    try {
        fc::ReadGateGuard guard(gate, done);
        Require(static_cast<bool>(guard), "guard acquires a permit");
        throw std::runtime_error("boom");
    } catch (const std::exception&) {
        // guard destructor must have released the permit
    }

    // Both permits must be available again: acquire two without blocking.
    fc::ReadGateGuard g1(gate, done);
    fc::ReadGateGuard g2(gate, done);
    Require(static_cast<bool>(g1) && static_cast<bool>(g2),
            "both permits available after exception-path release (R-04)");
}

// release() hands the permit back early (before the destructor); the destructor is then a no-op
// and the permit count stays correct (no double release).
void TestExplicitReleaseNoDoubleFree() {
    fc::ReadGate gate(1);
    std::atomic<bool> done{false};
    {
        fc::ReadGateGuard guard(gate, done);
        Require(static_cast<bool>(guard), "guard acquires the permit");
        guard.release();  // early return; destructor must not release again
        fc::ReadGateGuard again(gate, done);
        Require(static_cast<bool>(again), "permit reusable immediately after explicit release");
    }
    // If release had double-counted, available_ would now be 2; verify only one permit exists.
    fc::ReadGateGuard only(gate, done);
    Require(static_cast<bool>(only), "single permit reacquired");
    std::atomic<bool> doneFlag{true};
    Require(!gate.acquire(doneFlag), "no second permit exists (done-abort proves count==1)");
}

}  // namespace

void RunReadGateTests() {
    TestConcurrencyCapped();
    TestDoneAbortsWait();
    TestGuardReleasesOnException();
    TestExplicitReleaseNoDoubleFree();
}
