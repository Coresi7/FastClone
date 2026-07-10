// Tests for the client small-file write-path accounting + routing pure helpers
// (optimize-small-file-write-path, tester gaps T-01..T-04 / AC-02/AC-12/AC-13/AC-17).
//
// These pin the exact counting attribution the main-thread ioResults drain applies and the
// zero-byte dispatch decision, WITHOUT a full network/disk integration harness: the client code
// in sync_engine_client.cpp applies these helpers verbatim (see the ioResults drain and
// completeBatchEntry), so pinning the helpers pins the observable behaviour.

#include "write_path_accounting.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void Require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(std::string("write-path-accounting: ") + msg);
    }
}

// Mirror of the main-thread drain reducer: fold a sequence of (ok, fastPath) results through
// AccountWriteResult exactly as the client does, so the aggregate counters can be asserted.
struct Counters {
    uint64_t compared = 0;
    uint64_t transferred = 0;
    uint64_t driverPathFiles = 0;
    uint64_t fastPathFiles = 0;
    uint64_t retried = 0;  // stands in for retryOrFail() invocations (failure path)
};

Counters Drain(const std::vector<std::pair<bool, bool>>& results) {
    Counters c;
    for (const auto& r : results) {
        const fc::WriteResultAccounting acc = fc::AccountWriteResult(r.first, r.second);
        c.compared += acc.comparedDelta;
        c.transferred += acc.transferredDelta;
        c.driverPathFiles += acc.driverPathDelta;
        c.fastPathFiles += acc.fastPathDelta;
        if (!acc.countedSuccess) {
            ++c.retried;
        }
    }
    return c;
}

// T-01 / AC-02: a write content failure OR a close/finalize failure surfaces as ok==false, which
// must NOT be counted as success -- no compared/transferred/diagnostic bump; it routes to retry.
void TC_FailedWriteNotCountedAsSuccess() {
    for (bool fastPath : {false, true}) {
        const fc::WriteResultAccounting acc = fc::AccountWriteResult(/*ok=*/false, fastPath);
        Require(acc.comparedDelta == 0, "AC-02: failed write bumps no compared");
        Require(acc.transferredDelta == 0, "AC-02: failed write bumps no transferred");
        Require(acc.driverPathDelta == 0 && acc.fastPathDelta == 0,
                "AC-02: failed write bumps no FR-15 diagnostic");
        Require(!acc.countedSuccess, "AC-02: failed write routes to retryOrFail, not success");
    }
    // Aggregate: two failures produce zero transferred and exactly two retry routings.
    const Counters c = Drain({{false, false}, {false, true}});
    Require(c.transferred == 0 && c.compared == 0, "AC-02: no success counting across failures");
    Require(c.retried == 2, "AC-02: each failure routes to retry exactly once");
    Require(c.driverPathFiles == 0 && c.fastPathFiles == 0, "AC-02: no diagnostics on failure");
}

// T-02 / AC-12: a successful (zero-byte or not) file bumps compared and transferred by EXACTLY 1
// each; a failure triggers exactly one retry/fail and no counting. Modelled as a single drained
// success/failure -- the zero-byte file flows through the same IoWriteResult as any other file.
void TC_SuccessCountsExactlyOnce() {
    const fc::WriteResultAccounting ok = fc::AccountWriteResult(/*ok=*/true, /*fastPath=*/false);
    Require(ok.comparedDelta == 1, "AC-12: success bumps compared by exactly 1");
    Require(ok.transferredDelta == 1, "AC-12: success bumps transferred by exactly 1");
    Require(ok.countedSuccess, "AC-12: success is counted, not retried");

    // A mixed batch: 3 successes + 1 failure => compared==transferred==3, retried==1.
    const Counters c = Drain({{true, false}, {true, true}, {false, false}, {true, false}});
    Require(c.compared == 3, "AC-12: compared counts each success once");
    Require(c.transferred == 3, "AC-12: transferred counts each success once");
    Require(c.retried == 1, "AC-12: the single failure retries exactly once");
}

// T-03 / AC-13: a server-ok entry -- INCLUDING a zero-byte one -- routes to the async write pool
// (Dispatch), never to the synchronous main-thread finalize/create path (SyncFail). Failure /
// incomplete entries route to SyncFail.
void TC_ZeroByteRoutesToAsyncPool() {
    // shouldWrite == serverOk && complete. A zero-byte server-ok entry has shouldWrite==true.
    Require(fc::RouteBatchEntry(/*shouldWrite=*/true) == fc::BatchWriteRoute::Dispatch,
            "AC-13: server-ok (incl. zero-byte) entry dispatches to the async pool");
    Require(fc::RouteBatchEntry(/*shouldWrite=*/false) == fc::BatchWriteRoute::SyncFail,
            "AC-13: server-unavailable / incomplete entry takes the sync fail path");
    // The Dispatch route is the ONLY file-creating path; SyncFail must never be it for a writable
    // entry, which is exactly what guarantees no synchronous zero-byte create on the main thread.
    Require(fc::RouteBatchEntry(true) != fc::BatchWriteRoute::SyncFail,
            "AC-13: a writable entry is never synchronously created on the main thread");
}

// T-04 / AC-17: driverPathFiles / fastPathFiles are attributed accurately -- a success goes to
// exactly one of them (fastPath ? fastPathFiles : driverPathFiles) and never both; failures go to
// neither.
void TC_DiagnosticAttribution() {
    const fc::WriteResultAccounting driver = fc::AccountWriteResult(true, /*fastPath=*/false);
    Require(driver.driverPathDelta == 1 && driver.fastPathDelta == 0,
            "AC-17: non-fast success attributes to driverPathFiles only");
    const fc::WriteResultAccounting fast = fc::AccountWriteResult(true, /*fastPath=*/true);
    Require(fast.fastPathDelta == 1 && fast.driverPathDelta == 0,
            "AC-17: fast-path success attributes to fastPathFiles only");

    // Aggregate over a representative drain: 2 driver successes, 3 fast successes, 1 failure.
    const Counters c = Drain({{true, false}, {true, true}, {true, true},
                              {true, false}, {true, true}, {false, false}});
    Require(c.driverPathFiles == 2, "AC-17: driverPathFiles counts non-fast successes exactly");
    Require(c.fastPathFiles == 3, "AC-17: fastPathFiles counts fast successes exactly");
    // Conservation: every counted success is attributed to exactly one diagnostic (AC-17).
    Require(c.driverPathFiles + c.fastPathFiles == c.transferred,
            "AC-17: diagnostics partition the transferred successes (no double / no drop)");
    Require(c.transferred == 5 && c.retried == 1, "AC-17: 5 successes counted, 1 failure retried");
}

}  // namespace

void RunWritePathAccountingTests() {
    TC_FailedWriteNotCountedAsSuccess();
    TC_SuccessCountsExactlyOnce();
    TC_ZeroByteRoutesToAsyncPool();
    TC_DiagnosticAttribution();
}
