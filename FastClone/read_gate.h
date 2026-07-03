#pragma once

// Server-side read-concurrency gate (delta-streaming-fix M7/FR-14/NFR-02).
//
// Caps how many BlockSigRequest memcache-miss tasks stream a file from disk at the same time,
// so the number of concurrent sequential file reads is bounded independently of the hash-pool
// worker count (which keeps its own semantics, FR-15/D6). Header-only on purpose: the server
// TU (sync_engine_server.cpp) includes it, and so does tests/test_read_gate.cpp, letting the
// unit test exercise the REAL type without dragging the network stack into the test target
// (design D-04). A mutex+condvar is used (rather than std::counting_semaphore) so acquire() can
// abort on session `done` instead of blocking forever (FR-18).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace fc {

class ReadGate {
public:
    explicit ReadGate(uint32_t limit) : available_(limit) {}

    ReadGate(const ReadGate&) = delete;
    ReadGate& operator=(const ReadGate&) = delete;

    // Block until a permit is available. If `done` is set and no permit is free, give up and
    // return false so the caller can treat the task as cancelled (FR-18/AC-09). Never holds any
    // session lock while waiting (R-05): the only lock taken here is this gate's own mutex.
    bool acquire(const std::atomic<bool>& done) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&]() { return available_ > 0 || done.load(); });
        if (available_ == 0) {
            return false;  // reached only when done is set and no permit is free
        }
        --available_;
        return true;
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++available_;
        }
        cv_.notify_one();
    }

    // Wake all waiters (e.g. when `done` flips) so aborted acquires re-check their predicate.
    void notify_all_waiters() { cv_.notify_all(); }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    uint32_t available_;
};

// RAII permit holder: acquires on construction, releases on destruction (R-04: success, failure,
// exception and cancellation paths all release exactly once). release() can hand the permit back
// early (before taking any session lock, design §3.3 step 5); the destructor is then a no-op.
class ReadGateGuard {
public:
    ReadGateGuard(ReadGate& gate, const std::atomic<bool>& done)
        : gate_(gate), held_(gate.acquire(done)) {}

    ReadGateGuard(const ReadGateGuard&) = delete;
    ReadGateGuard& operator=(const ReadGateGuard&) = delete;

    ~ReadGateGuard() {
        if (held_) {
            gate_.release();
        }
    }

    // True when a permit is held (acquire succeeded).
    explicit operator bool() const { return held_; }

    void release() {
        if (held_) {
            gate_.release();
            held_ = false;
        }
    }

private:
    ReadGate& gate_;
    bool held_;
};

}  // namespace fc
