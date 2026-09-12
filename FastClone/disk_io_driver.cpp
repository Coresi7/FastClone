#include "disk_io_driver.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace fc::io {

DiskIoDriver::DiskIoDriver(const IoDriverConfig& cfg)
    : DiskIoDriver(cfg, CreatePlatformBackend(cfg)) {}

DiskIoDriver::DiskIoDriver(const IoDriverConfig& cfg, std::unique_ptr<PlatformIoBackend> backend)
    : cfg_(cfg), backend_(std::move(backend)) {
    readCredit_ = cfg_.readWeight == 0 ? 1u : cfg_.readWeight;
    writeCredit_ = cfg_.writeWeight == 0 ? 1u : cfg_.writeWeight;
    scheduler_ = std::thread([this] { SchedulerLoop(); });
}

DiskIoDriver::~DiskIoDriver() {
    requestCancel();  // flush not-yet-submitted ops
    {
        std::lock_guard<std::mutex> lk(qmu_);
        stop_ = true;
    }
    qcv_.notify_all();
    if (scheduler_.joinable()) {
        scheduler_.join();
    }
    if (backend_) {
        backend_->shutdown();
    }
}

size_t DiskIoDriver::submit(std::vector<IoRequest>& batch) {
    size_t accepted = 0;
    // C1: batch the per-file outstanding deltas and merge them in ONE countMu_ critical
    // section below (same qmu_ -> countMu_ direction as the PickAndSubmit counter update) —
    // one lock pair per batch instead of one per op. The merge must complete before qmu_ is
    // released: only then can the scheduler see (and later settle) these ops, so the increment
    // always precedes the matching EnqueueCompletion decrement.
    std::unordered_map<uint64_t, uint32_t> outstandingDelta;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        if (cancelled_ || stop_) {
            return 0;
        }
        for (auto& req : batch) {
            const uint64_t fid = req.fileId;  // capture before the move below
            if (req.kind == OpKind::Read) {
                if (readQ_.size() >= cfg_.maxReadQueue) {
                    break;  // backpressure on the read queue (FR-27)
                }
                readQ_.push_back(std::move(req));
            } else {
                if (writeQ_.size() >= cfg_.maxWriteQueue) {
                    break;  // backpressure on the write queue (FR-27)
                }
                writeQ_.push_back(std::move(req));
            }
            ++accepted;
            ++outstandingDelta[fid];
        }
        if (!outstandingDelta.empty()) {
            std::lock_guard<std::mutex> clk(countMu_);
            for (const auto& kv : outstandingDelta) {
                fileOutstanding_[kv.first] += kv.second;
            }
        }
    }
    if (accepted > 0) {
        {
            std::lock_guard<std::mutex> lk(countMu_);
            counters_.queued += accepted;
        }
        batch.erase(batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(accepted));
        qcv_.notify_all();
    }
    return accepted;
}

// Pop the first Prio::Small op from a queue if present, else the front (small-op priority, FR-26).
static IoRequest PopPreferSmall(std::deque<IoRequest>& q) {
    for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->prio == Prio::Small) {
            IoRequest r = std::move(*it);
            q.erase(it);
            return r;
        }
    }
    IoRequest r = std::move(q.front());
    q.pop_front();
    return r;
}

void DiskIoDriver::EnqueueCompletion(IoCompletion completion) {
    std::lock_guard<std::mutex> lk(cmu_);
    const uint64_t fileId = completion.fileId;
    auto& q = completionsByFile_[fileId];  // list<PendingCompletion> (stable node addresses)
    q.push_back(PendingCompletion{std::move(completion), nullptr, nullptr});
    OrderLink(&q.back());  // B2: 1:1 order slot with the node; unlinked on delivery
    // B3: notify only when someone is actually waiting. The previous operator[] created a
    // permanent fileWait_ entry for EVERY file that ever completed anything, waiters or not —
    // one of the two unbounded growth paths (the other was waitForFile).
    auto wit = fileWait_.find(fileId);
    if (wit != fileWait_.end()) {
        wit->second->cv.notify_one();
    }
    // C1 delayed-release settlement — runs strictly AFTER the push above, so this completion
    // was always enqueued first (the push is unconditional; a release never suppresses it).
    // Exactly one decrement per delivered completion, which pairs with the one increment in
    // submit() — this covers normal reap, requestCancel's flush and PickAndSubmit's
    // hard-failure synthesis alike, since all three deliver through this function.
    // Lock order cmu_ -> countMu_ is the allowed direction; countMu_ is released before
    // ReleaseFileLocked touches the cmu_ containers again.
    bool doRelease = false;
    {
        std::lock_guard<std::mutex> clk(countMu_);
        auto oit = fileOutstanding_.find(fileId);
        if (oit != fileOutstanding_.end() && --oit->second == 0) {
            fileOutstanding_.erase(oit);
        }
        if (fileOutstanding_.find(fileId) == fileOutstanding_.end()) {
            doRelease = pendingRelease_.erase(fileId) > 0;
        }
    }
    if (doRelease) {
        ReleaseFileLocked(fileId);  // cmu_ still held; countMu_ already released
    }
}

// Both run under cmu_ (caller holds it). O(1), zero heap allocation.
void DiskIoDriver::OrderLink(PendingCompletion* pc) {
    pc->orderPrev = orderTail_;
    pc->orderNext = nullptr;
    if (orderTail_ != nullptr) {
        orderTail_->orderNext = pc;
    } else {
        orderHead_ = pc;
    }
    orderTail_ = pc;
    ++orderCount_;
}

void DiskIoDriver::OrderUnlink(PendingCompletion* pc) {
    // Idempotent guard: an already-unlinked node (all links null and not the head) must not
    // decrement orderCount_ a second time. Under invariant I every node in a per-file list is
    // linked exactly once, so the guard is defensive only.
    if (pc->orderPrev == nullptr && pc->orderNext == nullptr && orderHead_ != pc) {
        return;
    }
    if (pc->orderPrev != nullptr) {
        pc->orderPrev->orderNext = pc->orderNext;
    } else if (orderHead_ == pc) {
        orderHead_ = pc->orderNext;
    }
    if (pc->orderNext != nullptr) {
        pc->orderNext->orderPrev = pc->orderPrev;
    } else if (orderTail_ == pc) {
        orderTail_ = pc->orderPrev;
    }
    pc->orderPrev = nullptr;
    pc->orderNext = nullptr;
    --orderCount_;
}

bool DiskIoDriver::PickAndSubmit() {
    IoRequest op;
    bool isRead = false;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        if (cancelled_ || inFlight_ >= cfg_.maxInFlight) {
            return false;
        }
        const bool haveR = !readQ_.empty();
        const bool haveW = !writeQ_.empty();
        if (!haveR && !haveW) {
            return false;
        }
        if (haveR && haveW) {
            if (readCredit_ == 0 && writeCredit_ == 0) {
                readCredit_ = cfg_.readWeight == 0 ? 1u : cfg_.readWeight;
                writeCredit_ = cfg_.writeWeight == 0 ? 1u : cfg_.writeWeight;
            }
            isRead = readCredit_ > 0;
        } else {
            isRead = haveR;
        }
        if (isRead) {
            op = PopPreferSmall(readQ_);
            if (readCredit_ > 0) {
                --readCredit_;
            }
        } else {
            op = PopPreferSmall(writeQ_);
            if (writeCredit_ > 0) {
                --writeCredit_;
            }
        }
        ++inFlight_;
        {
            std::lock_guard<std::mutex> clk(countMu_);
            ++counters_.submitted;
            if (isRead) {
                ++counters_.readSubmitted;
            } else {
                ++counters_.writeSubmitted;
            }
            if (cfg_.recordSchedule) {
                scheduleLog_.push_back(isRead ? OpKind::Read : OpKind::Write);
            }
        }
    }
    // F2: capture the op's routing/identity fields before it is moved into submit(), so a hard
    // backend failure can synthesize a completion attributed to the original file (not fileId 0).
    const OpKind   opKind      = op.kind;
    const uint64_t opFileId    = op.fileId;
    const uint64_t opOffset    = op.offset;
    const uint32_t opRequested = op.length;
    const uint64_t opUserTag   = op.userTag;
    if (!backend_->submit(std::move(op))) {
        // Hard backend failure: synthesize an error completion and release the in-flight slot.
        IoCompletion c;
        c.kind      = opKind;
        c.fileId    = opFileId;
        c.offset    = opOffset;
        c.requested = opRequested;
        c.userTag   = opUserTag;
        c.status    = IoStatus::Error;
        EnqueueCompletion(std::move(c));
        std::lock_guard<std::mutex> lk(qmu_);
        --inFlight_;
    }
    return true;
}

void DiskIoDriver::SchedulerLoop() {
    std::vector<IoCompletion> comps;
    for (;;) {
        bool stopping = false;
        {
            std::unique_lock<std::mutex> lk(qmu_);
            const bool idle = readQ_.empty() && writeQ_.empty();
            // On stop, once the queues are flushed we exit even if ops are still in flight: the
            // backend->shutdown() in the destructor quiesces/frees any remaining in-flight ops
            // (design section 3.6). This keeps teardown deadlock-free regardless of reap timing.
            if (stop_ && idle) {
                return;
            }
            stopping = stop_;
            if (idle && inFlight_ == 0 && !stop_) {
                qcv_.wait_for(lk, std::chrono::milliseconds(50));
            }
        }

        // Submit as many ops as fairness + the in-flight cap allow.
        while (PickAndSubmit()) {
        }

        // Reap completions and route them per file. Block briefly only when work is outstanding
        // and we are not stopping (a stopping scheduler must not block in the backend).
        comps.clear();
        int timeout;
        {
            std::lock_guard<std::mutex> lk(qmu_);
            // Use a non-blocking reap ONLY when we could immediately submit more (a free in-flight
            // slot AND queued work): the next loop iteration will then top up the pipeline without
            // delay. Otherwise (saturated at maxInFlight, or drained but ops still in flight) block
            // briefly so the scheduler yields the core instead of busy-spinning until a completion
            // arrives. Any completion / new submit wakes waiters via per-file cv / qcv_.
            const bool canSubmitMore =
                (!readQ_.empty() || !writeQ_.empty()) && inFlight_ < cfg_.maxInFlight;
            timeout = (inFlight_ > 0 && !stopping) ? (canSubmitMore ? 0 : 2) : 0;
        }
        const size_t got = backend_->reap(comps, 128, timeout);
        if (got > 0) {
            uint64_t completed = 0, failed = 0, cancelled = 0;
            uint64_t bytesRead = 0, bytesWritten = 0;
            for (auto& c : comps) {
                if (c.status == IoStatus::Error) {
                    ++failed;
                } else if (c.status == IoStatus::Cancelled) {
                    ++cancelled;
                } else {
                    ++completed;
                    if (c.kind == OpKind::Read) {
                        bytesRead += c.transferred;
                    } else {
                        bytesWritten += c.transferred;
                    }
                }
                EnqueueCompletion(std::move(c));
            }
            {
                std::lock_guard<std::mutex> lk(qmu_);
                inFlight_ -= got;
            }
            {
                std::lock_guard<std::mutex> lk(countMu_);
                counters_.completed += completed;
                counters_.failed += failed;
                counters_.cancelled += cancelled;
                counters_.bytesRead += bytesRead;
                counters_.bytesWritten += bytesWritten;
            }
            qcv_.notify_all();
        }
    }
}

size_t DiskIoDriver::drainCompletions(std::vector<IoCompletion>& out) {
    std::lock_guard<std::mutex> lk(cmu_);
    size_t n = 0;
    while (orderHead_ != nullptr) {
        PendingCompletion* pc = orderHead_;
        OrderUnlink(pc);  // equivalent of the old completionOrder_.pop_front()
        const uint64_t fid = pc->completion.fileId;
        auto it = completionsByFile_.find(fid);
        if (it == completionsByFile_.end() || it->second.empty()) {
            continue;  // defensive: unreachable under invariant I (kept from the old code)
        }
        // Invariant I: the order-list head is the head of its file queue (same enqueue order),
        // so global drain delivers in EnqueueCompletion order — byte-for-byte the old behavior.
        out.push_back(std::move(it->second.front().completion));
        it->second.pop_front();
        if (it->second.empty()) {
            completionsByFile_.erase(it);  // same rule as drainCompletionsForFile (B1)
        }
        ++n;
    }
    return n;
}

size_t DiskIoDriver::drainCompletionsForFile(uint64_t fileId, std::vector<IoCompletion>& out) {
    std::lock_guard<std::mutex> lk(cmu_);
    auto it = completionsByFile_.find(fileId);
    if (it == completionsByFile_.end()) {
        return 0;  // never seen / already reclaimed: same visible result as an empty queue
    }
    size_t n = 0;
    while (!it->second.empty()) {
        PendingCompletion& pc = it->second.front();
        OrderUnlink(&pc);  // B2: O(1) removal of the global order slot
        out.push_back(std::move(pc.completion));
        it->second.pop_front();  // destroys the PendingCompletion, freeing the payload
        ++n;
    }
    // B1: the queue is now empty, so this fileId has NO arrived-but-undelivered completion left.
    // Drop the bookkeeping entry immediately; a later completion just recreates it in
    // EnqueueCompletion — erasing an EMPTY entry can never lose an in-flight completion (only
    // non-empty entries hold data, and this one holds none).
    completionsByFile_.erase(it);
    return n;
}

void DiskIoDriver::waitForFile(uint64_t fileId, int timeoutMs) {
    const auto ms = std::chrono::milliseconds(timeoutMs < 0 ? 1000 : timeoutMs);
    std::unique_lock<std::mutex> lk(cmu_);
    // Fast path: a completion is already deliverable -> return without creating any wait state
    // (keeps fileWait_ empty at rest; steady-state waitStates == 0).
    auto fit = completionsByFile_.find(fileId);
    if (fit != completionsByFile_.end() && !fit->second.empty()) {
        return;
    }
    auto& slot = fileWait_[fileId];
    if (!slot) {
        slot = std::make_shared<FileWaitState>();
    }
    // The waiter keeps its own reference: even if the map entry is erased while we are blocked
    // in wait_for (cmu_ released inside), the FileWaitState and its cv stay alive, so the
    // wake-up and destruction are always well-defined (B3 — correctness comes from the value
    // semantics of shared_ptr, not from the waiters count).
    std::shared_ptr<FileWaitState> ws = slot;
    ++ws->waiters;
    ws->cv.wait_for(lk, ms, [&] {
        if (ws->dead) {
            return true;  // release requested: leave promptly; the next drain reports nothing
        }
        auto it = completionsByFile_.find(fileId);
        return it != completionsByFile_.end() && !it->second.empty();
    });
    --ws->waiters;
    if (ws->waiters == 0) {
        // Last waiter out: reclaim the entry now so wait states never outlive the wait.
        auto it = fileWait_.find(fileId);
        if (it != fileWait_.end() && it->second.get() == ws.get()) {
            fileWait_.erase(it);  // *ws stays alive via the local shared_ptr until return
        }
    }
}

void DiskIoDriver::requestCancel() {
    std::deque<IoRequest> r, w;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        if (cancelled_) {
            return;
        }
        cancelled_ = true;
        r.swap(readQ_);
        w.swap(writeQ_);
    }
    uint64_t flushed = 0;
    auto flush = [&](std::deque<IoRequest>& q) {
        for (auto& req : q) {
            IoCompletion c;
            c.kind = req.kind;
            c.fileId = req.fileId;
            c.offset = req.offset;
            c.requested = req.length;
            c.userTag = req.userTag;
            c.status = IoStatus::Cancelled;
            EnqueueCompletion(std::move(c));
            ++flushed;
        }
    };
    flush(r);
    flush(w);
    if (flushed > 0) {
        std::lock_guard<std::mutex> lk(countMu_);
        counters_.cancelled += flushed;
    }
    qcv_.notify_all();
}

void DiskIoDriver::releaseFile(uint64_t fileId) {
    // Two SEPARATE critical sections (countMu_ first and released, then cmu_): never nested,
    // so the lock order stays acyclic (design §4.1-4). If ops are still outstanding the cleanup
    // is only REGISTERED here and runs in EnqueueCompletion's settlement after the LAST
    // completion is delivered — a file with ops in flight does not lose a single byte of
    // bookkeeping before that, which is what keeps in-flight completions undroppable.
    bool immediate = false;
    {
        std::lock_guard<std::mutex> lk(countMu_);
        if (fileOutstanding_.find(fileId) == fileOutstanding_.end()) {
            immediate = true;  // nothing outstanding -> clean up right now
        } else {
            pendingRelease_.insert(fileId);  // ops in flight -> defer to the last delivery
        }
    }
    if (immediate) {
        std::lock_guard<std::mutex> lk(cmu_);
        ReleaseFileLocked(fileId);
    }
}

void DiskIoDriver::releaseFiles(const std::vector<uint64_t>& fileIds) {
    for (const uint64_t fid : fileIds) {
        releaseFile(fid);
    }
}

void DiskIoDriver::ReleaseFileLocked(uint64_t fileId) {
    // cmu_ held by the caller. The ONLY place besides the drain paths that drops per-file
    // bookkeeping; it is gated by the releaseFile contract (no further submits, no live
    // reader) and by the outstanding-op accounting, so a live SequentialReader can never
    // lose a completion it is still waiting for (hard constraint 1).
    auto it = completionsByFile_.find(fileId);
    if (it != completionsByFile_.end()) {
        for (auto& pc : it->second) {
            OrderUnlink(&pc);  // drop every global order slot of this file
        }
        completionsByFile_.erase(it);  // frees all undelivered payload memory
    }
    auto wit = fileWait_.find(fileId);
    if (wit != fileWait_.end()) {
        wit->second->dead = true;
        wit->second->cv.notify_all();  // wake blocked waiters; they observe dead and leave
        if (wit->second->waiters == 0) {
            fileWait_.erase(wit);  // no waiter: drop now; else the last waiter erases it
        }
    }
}

IoCounters DiskIoDriver::counters() const {
    IoCounters c;
    {
        std::lock_guard<std::mutex> lk(countMu_);
        c = counters_;
    }
    {
        std::lock_guard<std::mutex> lk(qmu_);
        c.readPending = readQ_.size();
        c.writePending = writeQ_.size();
    }
    const BackendCounters bc = backend_->counters();
    c.directIo = bc.directIo;
    c.bufferedFallback = bc.bufferedFallback;
    c.ioUringFallback = bc.ioUringFallback;
    c.smallFileFallback = bc.smallFileFallback;
    c.tailZeroFallback = bc.tailZeroFallback;
    return c;
}

DiskIoDriver::RetentionSnapshot DiskIoDriver::retentionSnapshot() const {
    RetentionSnapshot snap;
    std::lock_guard<std::mutex> lk(cmu_);
    snap.trackedFiles = completionsByFile_.size();
    snap.waitStates = fileWait_.size();
    snap.completionOrder = orderCount_;  // new semantics: instantaneous undelivered backlog
    for (const auto& kv : completionsByFile_) {
        for (const auto& pc : kv.second) {
            snap.retainedBytes += pc.completion.data.size();
        }
    }
    {
        // cmu_ -> countMu_ is the allowed direction (design §4.1-5); countMu_ is innermost
        // and released immediately, matching the releaseFile/settlement discipline.
        std::lock_guard<std::mutex> clk(countMu_);
        snap.pendingReleases = pendingRelease_.size();
        snap.outstandingFiles = fileOutstanding_.size();
    }
    return snap;
}

std::vector<OpKind> DiskIoDriver::scheduleLog() const {
    std::lock_guard<std::mutex> lk(countMu_);
    return scheduleLog_;
}

std::string DiskIoDriver::backendName() const {
    switch (backend_->kind()) {
        case BackendKind::WinIocp:
            return "Windows IOCP (FILE_FLAG_NO_BUFFERING)";
        case BackendKind::LinuxUring:
            return "Linux io_uring (O_DIRECT)";
        case BackendKind::PosixThreadPool:
            // On Linux the pool is the io_uring fallback (probe failed or liburing absent); on macOS
            // it is the primary backend expressing unbuffered intent via F_NOCACHE.
            return backend_->counters().ioUringFallback
                       ? "POSIX pread/pwrite thread pool (io_uring fallback)"
                       : "POSIX pread/pwrite thread pool (F_NOCACHE)";
        case BackendKind::Mock:
            return "Mock";
    }
    return "unknown";
}

// -------------------------------------------------------------------------------------------------
// SequentialReader
// -------------------------------------------------------------------------------------------------

SequentialReader::SequentialReader(DiskIoDriver& driver, uint64_t fileId, uint64_t fileSize,
                                   uint32_t chunkBytes, uint32_t readAhead)
    : drv_(driver),
      fileId_(fileId),
      fileSize_(fileSize),
      chunk_(chunkBytes == 0 ? (1u << 20) : chunkBytes),
      readAhead_(readAhead == 0 ? 1u : readAhead) {}

void SequentialReader::refill() {
    std::vector<IoRequest> batch;
    std::vector<uint64_t> planOffsets;
    uint64_t off = nextSubmitOffset_;
    uint32_t planned = inFlight_;
    while (planned < readAhead_ && off < fileSize_) {
        const uint64_t remain = fileSize_ - off;
        const uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(chunk_, remain));
        IoRequest r;
        r.kind = OpKind::Read;
        r.fileId = fileId_;
        r.offset = off;
        r.length = len;
        r.prio = Prio::Large;
        r.userTag = off;
        batch.push_back(std::move(r));
        planOffsets.push_back(off);
        off += len;
        ++planned;
    }
    if (batch.empty()) {
        return;
    }
    const uint64_t endOff = off;
    const size_t took = drv_.submit(batch);  // accepts a prefix, erases accepted from `batch`
    inFlight_ += static_cast<uint32_t>(took);
    nextSubmitOffset_ = (took < planOffsets.size()) ? planOffsets[took] : endOff;
}

uint32_t SequentialReader::next(std::vector<uint8_t>& out, bool& ok) {
    ok = true;
    out.clear();
    if (error_) {
        ok = false;
        return 0;
    }
    if (nextYieldOffset_ >= fileSize_) {
        return 0;  // clean EOF
    }
    refill();
    for (;;) {
        auto it = reordered_.find(nextYieldOffset_);
        if (it != reordered_.end()) {
            IoCompletion c = std::move(it->second);
            reordered_.erase(it);
            // F4: an early Eof (file shorter than the planned fileSize_) is a failure, not a clean
            // EOF; treat it like Error. A clean EOF is only nextYieldOffset_ >= fileSize_ above.
            if (c.status == IoStatus::Error || c.status == IoStatus::Eof) {
                error_ = true;
                ok = false;
                return 0;
            }
            out = std::move(c.data);
            nextYieldOffset_ += out.size();
            refill();
            return static_cast<uint32_t>(out.size());
        }
        // Not yet available: reap this file's completions.
        std::vector<IoCompletion> comps;
        drv_.drainCompletionsForFile(fileId_, comps);
        if (comps.empty()) {
            drv_.waitForFile(fileId_, 1000);
            drv_.drainCompletionsForFile(fileId_, comps);
        }
        for (auto& c : comps) {
            if (inFlight_ > 0) {
                --inFlight_;
            }
            reordered_.emplace(c.offset, std::move(c));
        }
        if (comps.empty() && inFlight_ == 0 && nextSubmitOffset_ >= fileSize_) {
            // Nothing outstanding and nothing to yield -> treat as EOF/error guard.
            return 0;
        }
    }
}

size_t SequentialReader::pull(uint8_t* dst, size_t maxLen) {
    size_t written = 0;
    while (written < maxLen) {
        if (carryPos_ < carry_.size()) {
            const size_t avail = carry_.size() - carryPos_;
            const size_t take = std::min(avail, maxLen - written);
            std::memcpy(dst + written, carry_.data() + carryPos_, take);
            carryPos_ += take;
            written += take;
            continue;
        }
        bool ok = true;
        carry_.clear();
        carryPos_ = 0;
        const uint32_t n = next(carry_, ok);
        if (!ok || n == 0) {
            break;  // EOF or error
        }
    }
    return written;
}

}  // namespace fc::io
