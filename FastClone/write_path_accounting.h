#pragma once

// Pure write-path accounting + routing decisions for the client small-file write path
// (optimize-small-file-write-path W-04/FR-10/FR-11/FR-12/FR-15, AC-02/AC-12/AC-13/AC-17).
//
// These are extracted out of sync_engine_client.cpp so the counting-attribution and the
// zero-byte dispatch decision are unit-testable WITHOUT a full network/disk integration harness
// (tester T-01..T-04). They are header-only, dependency-free and side-effect-free: the client
// applies exactly the deltas / route returned here, so a test that pins these functions pins the
// observable counting + routing behaviour.

#include <cstdint>

namespace fc {

// Result of draining ONE async write-pool completion (IoWriteResult{ok, fastPath}) on the main
// thread. The client applies these deltas verbatim so that:
//  - a finished file bumps compared/transferred exactly once (AC-12), and
//  - it is attributed to exactly one of the FR-15 diagnostics (driverPathFiles vs fastPathFiles,
//    AC-17), and
//  - a failed write (ok==false: content-write failure OR close/finalize failure, AC-02) is NEVER
//    counted as success -- all deltas are 0 and the caller routes it to retryOrFail instead.
struct WriteResultAccounting {
    uint32_t comparedDelta = 0;
    uint32_t transferredDelta = 0;
    uint32_t driverPathDelta = 0;
    uint32_t fastPathDelta = 0;
    // true  => success counting (erase retry state); false => route to retryOrFail (no counting).
    bool countedSuccess = false;
};

// `ok` is the worker result: for the driver path it is (writeOk && closeOk), so a close/finalize
// (SetEndOfFile/SetFileTime/CloseHandle or ftruncate/futimens/close) failure yields ok==false;
// for the fast path it is WriteSmallFileFastPath's bool. `fastPath` selects which FR-15 diagnostic
// the success is attributed to.
inline WriteResultAccounting AccountWriteResult(bool ok, bool fastPath) {
    WriteResultAccounting a;
    if (!ok) {
        return a;  // AC-02: no compared/transferred, no diagnostic bump; caller runs retryOrFail
    }
    a.comparedDelta = 1;
    a.transferredDelta = 1;
    a.driverPathDelta = fastPath ? 0u : 1u;
    a.fastPathDelta = fastPath ? 1u : 0u;
    a.countedSuccess = true;
    return a;
}

// How a completed batch entry is routed (FileBatchOpen / FileBatchChunk / FileBatchEnd all funnel
// through completeBatchEntry -> this decision).
enum class BatchWriteRoute {
    // Hand to the async write pool. This is the ONLY path that creates the on-disk file, INCLUDING
    // zero-byte files: the worker opens/creates + truncates to the exact size + stamps mtime, and
    // the count converges only through the ioResults drain (W-04/FR-10/FR-11). The main thread does
    // NOT synchronously create the file here (AC-13).
    Dispatch,
    // Synchronous finalize on the main thread for failure / not-to-write entries only. This path
    // never creates file content or sets mtime -- it just closes any open stream and marks the entry
    // for retry/fail (FR-12).
    SyncFail,
};

// `shouldWrite` == entry.serverOk AND the entry is still complete (not truncated at FileBatchEnd).
// A server-ok ZERO-BYTE entry has shouldWrite==true, so it routes to Dispatch -- proving the main
// thread never takes a synchronous zero-byte create path (AC-13). Server-unavailable or incomplete
// entries route to SyncFail.
inline BatchWriteRoute RouteBatchEntry(bool shouldWrite) {
    return shouldWrite ? BatchWriteRoute::Dispatch : BatchWriteRoute::SyncFail;
}

}  // namespace fc
