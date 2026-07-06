#include "compare_pipeline.h"

#include <algorithm>
#include <utility>

namespace fc {

ComparePipeline::ComparePipeline(const ComparePipelineConfig& cfg,
                                 LocalProbeFn probe,
                                 std::function<void()> onResultsReady)
    : cfg_(cfg), probe_(std::move(probe)), onResultsReady_(std::move(onResultsReady)) {
    const std::size_t count = std::max<std::size_t>(1, cfg_.workerCount);
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this]() { WorkerLoop(); });
    }
}

ComparePipeline::~ComparePipeline() {
    Stop();
    Join();
}

void ComparePipeline::Enqueue(const FileEntry& remote) {
    // Single-producer buffer; counted as issued immediately so InFlight() gating stays accurate even
    // before the buffer is flushed into the task queue (mirrors the legacy compareTasksIssued site).
    dispatchBuffer_.push_back(remote);
    issued_.fetch_add(1, std::memory_order_relaxed);
}

void ComparePipeline::Flush() {
    if (dispatchBuffer_.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(taskMu_);
        for (FileEntry& e : dispatchBuffer_) {
            tasks_.push_back(std::move(e));
        }
    }
    taskCv_.notify_all();
    dispatchBuffer_.clear();
}

std::size_t ComparePipeline::Drain(std::vector<ComparedItem>& out) {
    std::deque<ComparedItem> ready;
    {
        std::lock_guard<std::mutex> lock(resultMu_);
        ready.swap(results_);
    }
    const std::size_t n = ready.size();
    if (n == 0) {
        return 0;
    }
    pending_.fetch_sub(n, std::memory_order_relaxed);
    drained_.fetch_add(n, std::memory_order_relaxed);
    out.reserve(out.size() + n);
    for (ComparedItem& item : ready) {
        out.push_back(std::move(item));
    }
    return n;
}

std::size_t ComparePipeline::InFlight() const noexcept {
    const std::size_t issued = issued_.load(std::memory_order_relaxed);
    const std::size_t drained = drained_.load(std::memory_order_relaxed);
    return (issued >= drained) ? (issued - drained) : 0;
}

std::size_t ComparePipeline::QueuedTasks() const {
    std::lock_guard<std::mutex> lock(taskMu_);
    return tasks_.size();
}

std::size_t ComparePipeline::PendingResults() const noexcept {
    return pending_.load(std::memory_order_relaxed);
}

bool ComparePipeline::HasResults() const noexcept {
    return pending_.load(std::memory_order_relaxed) != 0;
}

void ComparePipeline::Stop() noexcept {
    stop_.store(true, std::memory_order_relaxed);
    taskCv_.notify_all();
}

void ComparePipeline::Join() {
    for (std::thread& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
}

void ComparePipeline::WorkerLoop() {
    std::vector<FileEntry> taskBatch;
    taskBatch.reserve(cfg_.batchPop);
    std::vector<ComparedItem> resultBatch;
    resultBatch.reserve(cfg_.batchPop);
    while (true) {
        taskBatch.clear();
        {
            std::unique_lock<std::mutex> lock(taskMu_);
            taskCv_.wait(lock, [this]() {
                return stop_.load(std::memory_order_relaxed) || !tasks_.empty();
            });
            // Drain any queued tasks before exiting on stop (same semantics as the legacy compare
            // worker: stop_ && queue empty is the only exit). In-flight is bounded by the caller's gate.
            if (stop_.load(std::memory_order_relaxed) && tasks_.empty()) {
                break;
            }
            const std::size_t take = std::min<std::size_t>(cfg_.batchPop, tasks_.size());
            for (std::size_t j = 0; j < take; ++j) {
                taskBatch.push_back(std::move(tasks_.front()));
                tasks_.pop_front();
            }
        }
        resultBatch.clear();
        for (const FileEntry& remote : taskBatch) {
            std::optional<FileEntry> local;
            try {
                local = probe_(remote.relativePath);  // probe exception -> nullopt (-> Missing)
            } catch (...) {
                local = std::nullopt;
            }
            CompareOutcome outcome = DecideCompare(cfg_.mode, local, remote);  // FR-05 single source of truth
            resultBatch.push_back(ComparedItem{remote, std::move(local), outcome});
        }
        if (!resultBatch.empty()) {
            const std::size_t n = resultBatch.size();
            {
                std::lock_guard<std::mutex> lock(resultMu_);
                for (ComparedItem& item : resultBatch) {
                    results_.push_back(std::move(item));
                }
            }
            pending_.fetch_add(n, std::memory_order_relaxed);
            if (onResultsReady_) {
                onResultsReady_();
            }
        }
    }
}

}  // namespace fc
