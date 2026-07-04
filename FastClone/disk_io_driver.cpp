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
    {
        std::lock_guard<std::mutex> lk(qmu_);
        if (cancelled_ || stop_) {
            return 0;
        }
        for (auto& req : batch) {
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
        {
            std::lock_guard<std::mutex> lk(cmu_);
            completionsByFile_[opFileId].push_back(std::move(c));
            completionOrder_.push_back(opFileId);
        }
        ccv_.notify_all();
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
            // arrives. Any completion / new submit wakes the wait below via ccv_/qcv_.
            const bool canSubmitMore =
                (!readQ_.empty() || !writeQ_.empty()) && inFlight_ < cfg_.maxInFlight;
            timeout = (inFlight_ > 0 && !stopping) ? (canSubmitMore ? 0 : 2) : 0;
        }
        const size_t got = backend_->reap(comps, 128, timeout);
        if (got > 0) {
            uint64_t completed = 0, failed = 0, cancelled = 0;
            {
                std::lock_guard<std::mutex> lk(cmu_);
                for (auto& c : comps) {
                    if (c.status == IoStatus::Error) {
                        ++failed;
                    } else if (c.status == IoStatus::Cancelled) {
                        ++cancelled;
                    } else {
                        ++completed;
                    }
                    completionOrder_.push_back(c.fileId);
                    completionsByFile_[c.fileId].push_back(std::move(c));
                }
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
            }
            ccv_.notify_all();
            qcv_.notify_all();
        }
    }
}

size_t DiskIoDriver::drainCompletions(std::vector<IoCompletion>& out) {
    std::lock_guard<std::mutex> lk(cmu_);
    size_t n = 0;
    while (!completionOrder_.empty()) {
        const uint64_t fid = completionOrder_.front();
        completionOrder_.pop_front();
        auto it = completionsByFile_.find(fid);
        if (it == completionsByFile_.end() || it->second.empty()) {
            continue;  // already drained via drainCompletionsForFile
        }
        out.push_back(std::move(it->second.front()));
        it->second.pop_front();
        ++n;
    }
    return n;
}

size_t DiskIoDriver::drainCompletionsForFile(uint64_t fileId, std::vector<IoCompletion>& out) {
    std::lock_guard<std::mutex> lk(cmu_);
    auto it = completionsByFile_.find(fileId);
    if (it == completionsByFile_.end()) {
        return 0;
    }
    size_t n = 0;
    while (!it->second.empty()) {
        out.push_back(std::move(it->second.front()));
        it->second.pop_front();
        ++n;
    }
    return n;
}

void DiskIoDriver::waitForFile(uint64_t fileId, int timeoutMs) {
    std::unique_lock<std::mutex> lk(cmu_);
    ccv_.wait_for(lk, std::chrono::milliseconds(timeoutMs < 0 ? 1000 : timeoutMs), [&] {
        auto it = completionsByFile_.find(fileId);
        return it != completionsByFile_.end() && !it->second.empty();
    });
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
    {
        std::lock_guard<std::mutex> lk(cmu_);
        auto flush = [&](std::deque<IoRequest>& q) {
            for (auto& req : q) {
                IoCompletion c;
                c.kind = req.kind;
                c.fileId = req.fileId;
                c.offset = req.offset;
                c.requested = req.length;
                c.userTag = req.userTag;
                c.status = IoStatus::Cancelled;
                completionOrder_.push_back(c.fileId);
                completionsByFile_[c.fileId].push_back(std::move(c));
                ++flushed;
            }
        };
        flush(r);
        flush(w);
    }
    if (flushed > 0) {
        std::lock_guard<std::mutex> lk(countMu_);
        counters_.cancelled += flushed;
    }
    ccv_.notify_all();
    qcv_.notify_all();
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
