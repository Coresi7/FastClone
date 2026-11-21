// Task ci-once-multi-test-budget (59 design section 3.10, C5): deterministic lost-wakeup
// reproduction + rescue proof for the centralized shutdown helpers, plus real
// ComparePipeline Stop/cap regression cases.
//
// The defect (59 section 2 [A]): the old write shape was `stop.store(true); cv.notify_all();`
// with BOTH statements outside the queue mutex. A worker inside cv.wait(lock, pred) that
// has already evaluated the predicate (false) but not yet parked loses that notify; it
// then parks and only a LATER notify (e.g. the 120s idle watchdog) can free it -- the
// join hangs. This file reproduces that window DETERMINISTICALLY (no 200-iteration
// probability loop, no fake helper copy):
//
//   worker:   lock(mu) -> manual predicate snapshot (false) -> handshake "predicate
//             checked" WHILE STILL HOLDING mu -> wait (on stageCv, still holding mu) for
//             the driver's "old-shape notify fired" -> cv.wait(lk) (the production
//             atomic release+park step; the predicate is NOT re-evaluated, exactly like
//             the production window where the check already happened).
//   driver:   wait for the handshake -> fire the OLD shape (store + notify_all, no lock;
//             the worker is provably parked on stageCv, NOT on cv, so the notify has no
//             waiter and is lost by construction) -> release the worker into its park.
//
// The worker then parks with no wakeup source left: 300ms later it must still be running
// (deterministic defect reproduction). The rescue is the REAL production helper
// fc::ShutdownFlagged (store under the SAME mu, then notify): the parked worker released
// mu when it parked, so the helper's lock acquisition is ordered after the park, and its
// notify is therefore delivered -- exit + JOIN must complete within 2s. A bounded
// same-mutex fallback (and a final raw notify_all, which always wakes an already-parked
// waiter) keeps the runner free of a permanent thread if the helper ever regresses; the
// recorded failure still fails the test.
//
// ComparePipeline cases use the REAL production component (whose Stop/SetActiveCap now
// store under taskMu_ through the same helpers): Stop wakes every parked worker,
// stop-mode drains the queued tasks without loss, and a mid-flight cap raise wakes the
// cap-parked workers.
//
// Discipline (59 section 3.10): no sleep-as-oracle (the 300ms probe IS the defect
// assertion), no detach, no kill, every wait bounded, single case < 2s, whole file < 10s.

#include "compare_pipeline.h"
#include "task_queue_shutdown.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace fc;

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test_task_queue_shutdown: " + message);
    }
}

constexpr auto kCaseDeadline = std::chrono::seconds(2);
constexpr auto kBarrierTimeout = std::chrono::seconds(2);

// ---------------------------------------------------------------------------
// Case 1: deterministic old-shape lost wakeup + real-helper rescue.
// ---------------------------------------------------------------------------
void TestShutdownFlaggedLostWakeupDeterministic() {
    struct Case {
        std::mutex mu;
        std::condition_variable cv;
        std::atomic<bool> stop{false};
        std::deque<int> q;  // production predicate shape: stop || !q.empty()

        std::mutex stageMu;
        std::condition_variable stageCv;
        bool predChecked = false;   // worker -> driver: predicate evaluated (false)
        bool goPark = false;        // driver -> worker: the old-shape notify has fired
        bool barrierTimedOut = false;  // worker -> driver: barrier itself failed

        std::mutex outputMu;
        bool exited = false;  // worker-side exit record (guarded by outputMu)
    } c;

    auto worker = [&c]() {
        std::unique_lock<std::mutex> lk(c.mu);
        // (1) Manual predicate snapshot -- the exact body of the production predicates
        //     (hash/dir/io/deltaPlan workers read their stop flag + queue here).
        const bool pred = c.stop.load() || !c.q.empty();
        // (2) Handshake while STILL HOLDING mu: the driver may now fire the old shape.
        {
            std::lock_guard<std::mutex> sl(c.stageMu);
            c.predChecked = true;
        }
        c.stageCv.notify_one();
        // (3) Hold the window open until the driver confirms the old-shape notify fired:
        //     parked on stageCv (NOT on cv) and still holding mu, so the cv notify below
        //     is guaranteed to have no waiter.
        {
            std::unique_lock<std::mutex> sl(c.stageMu);
            if (!c.stageCv.wait_for(sl, kBarrierTimeout, [&c] { return c.goPark; })) {
                c.barrierTimedOut = true;
                return;
            }
        }
        // (4) The production atomic step: release mu + park. The predicate is NOT
        //     re-evaluated -- that is precisely the production window between the
        //     predicate check and the parked wait.
        if (!pred) {
            c.cv.wait(lk);
        }
        // (5) Only reachable after a DELIVERED notify (the rescue).
        {
            std::lock_guard<std::mutex> ol(c.outputMu);
            c.exited = true;
        }
    };

    std::future<void> parked = std::async(std::launch::async, worker);

    // (a) Wait for the handshake (bounded).
    {
        std::unique_lock<std::mutex> sl(c.stageMu);
        const bool shook = c.stageCv.wait_for(sl, kBarrierTimeout, [&c] { return c.predChecked; });
        Expect(shook, "worker never reached the predicate-check handshake (barrier timeout)");
    }
    // (b) OLD defective shape: store + notify_all, both OUTSIDE the mutex. The worker is
    //     provably not parked on cv at this moment (it is waiting on stageCv for goPark),
    //     so this notify is lost by construction.
    c.stop.store(true);
    c.cv.notify_all();
    // (c) Release the worker into its park.
    {
        std::lock_guard<std::mutex> sl(c.stageMu);
        c.goPark = true;
    }
    c.stageCv.notify_one();

    // (d) DETERMINISTIC defect assertion: no wakeup source exists for cv, so the worker
    //     must still be parked 300ms later. (This sleep is the defect proof itself, not a
    //     readiness guess; 59 section 3.10 step 2.)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        std::lock_guard<std::mutex> sl(c.stageMu);
        Expect(!c.barrierTimedOut, "worker-side barrier timed out before the driver released it");
    }
    {
        std::lock_guard<std::mutex> ol(c.outputMu);
        Expect(!c.exited,
               "old-shape store+notify outside the mutex lost the wakeup: worker must still be parked");
    }

    // (e) Rescue with the REAL production helper: store under the SAME mu, then notify.
    fc::ShutdownFlagged(c.mu, c.cv, c.stop);

    // (f) Bounded exit + JOIN. Do NOT throw before the worker is joined (the async
    // future's destructor joins): on a helper regression, run the bounded same-mutex
    // fallback first, then a final raw notify_all (an already-parked waiter is always
    // woken by a notify), and only report after the join.
    const bool rescueOk = parked.wait_for(kCaseDeadline) == std::future_status::ready;
    if (!rescueOk) {
        c.stop.store(true);
        {
            std::lock_guard<std::mutex> lk(c.mu);
        }
        c.cv.notify_all();
        if (parked.wait_for(kCaseDeadline) != std::future_status::ready) {
            c.cv.notify_all();  // final raw wake: a parked waiter always leaves on notify
        }
    }
    parked.get();  // JOIN (std::async)
    {
        std::lock_guard<std::mutex> ol(c.outputMu);
        Expect(rescueOk && c.exited,
               "fc::ShutdownFlagged did not wake the parked worker within 2s (fallback had to fire)");
    }
}

// ---------------------------------------------------------------------------
// Case 2: SetCapFlagged wakes cap-parked workers under the same mutex (the cap twin of
// the lost-wakeup family: the io workers' predicate reads writeActiveCap while holding
// ioTaskMu). A worker parked at the cap must observe a raised cap within the deadline.
// ---------------------------------------------------------------------------
void TestSetCapFlaggedWakesCapParkedWorker() {
    struct Case {
        std::mutex mu;
        std::condition_variable cv;
        std::atomic<uint32_t> cap{1};
        std::atomic<uint32_t> active{1};  // one slot already taken (at the cap)
        std::atomic<bool> stop{false};
        std::mutex outputMu;
        bool exited = false;
    } c;

    auto worker = [&c]() {
        std::unique_lock<std::mutex> lk(c.mu);
        // Production io-worker predicate shape: stop || (active < cap).
        c.cv.wait(lk, [&c] {
            return c.stop.load(std::memory_order_relaxed) ||
                   c.active.load(std::memory_order_relaxed) < c.cap.load(std::memory_order_relaxed);
        });
        std::lock_guard<std::mutex> ol(c.outputMu);
        c.exited = true;
    };

    std::future<void> parked = std::async(std::launch::async, worker);
    // Until the cap is raised there is NO wakeup source: the worker cannot exit (a
    // spurious wakeup re-checks the still-false predicate and re-parks), so this is a
    // deterministic assertion, not a timing guess.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> ol(c.outputMu);
        Expect(!c.exited, "worker parked at the cap must not exit before the cap is raised");
    }
    // Raise the cap with the production helper (store under mu + notify).
    fc::SetCapFlagged(c.mu, c.cv, c.cap, 2);
    const bool rescueOk = parked.wait_for(kCaseDeadline) == std::future_status::ready;
    if (!rescueOk) {
        c.stop.store(true);
        {
            std::lock_guard<std::mutex> lk(c.mu);
        }
        c.cv.notify_all();
        if (parked.wait_for(kCaseDeadline) != std::future_status::ready) {
            c.cv.notify_all();  // final raw wake
        }
    }
    parked.get();  // JOIN
    {
        std::lock_guard<std::mutex> ol(c.outputMu);
        Expect(rescueOk && c.exited, "fc::SetCapFlagged did not wake the cap-parked worker within 2s");
    }
}

// Bounded result drain shared by the ComparePipeline cases: poll Drain until `expected`
// results arrived or the deadline hit (a hang surfaces as a test failure, never a stuck
// runner; the pipeline's own Stop-mode drain is what is under test).
std::vector<ComparedItem> DrainBounded(ComparePipeline& pipeline, std::size_t expected) {
    std::vector<ComparedItem> out;
    const auto deadline = std::chrono::steady_clock::now() + kCaseDeadline;
    while (out.size() < expected && std::chrono::steady_clock::now() < deadline) {
        pipeline.Drain(out);
        if (out.size() < expected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    return out;
}

FileEntry MakeEntry(const std::string& rel, uint64_t size, int64_t mtimeNs) {
    FileEntry e;
    e.relativePath = rel;
    e.isDirectory = false;
    e.fileSize = size;
    e.mtimeNs = mtimeNs;
    return e;
}

// ---------------------------------------------------------------------------
// Case 3: REAL ComparePipeline -- Stop (locked stop_ store via the same helper) wakes
// every parked worker, stop-mode drains the queued tasks without loss, Join is bounded.
// cap=1 + 4 workers (59 section 3.10 step 4).
// ---------------------------------------------------------------------------
void TestComparePipelineStopUnderMutex() {
    ComparePipelineConfig cfg;
    cfg.workerCount = 4;
    cfg.batchPop = 8;
    cfg.mode = CompareMode::Fast;

    // Fast scripted probe: every entry -> missing local -> Missing verdict, needHash=false.
    ComparePipeline pipeline(cfg,
                             [](const std::string&) { return std::optional<FileEntry>{}; },
                             []() {});
    pipeline.SetActiveCap(1);  // cap=1: at most one worker holds the slot at a time

    const std::size_t kEntries = 40;
    for (std::size_t i = 0; i < kEntries; ++i) {
        pipeline.Enqueue(MakeEntry("f" + std::to_string(i), 1, static_cast<int64_t>(i)));
    }
    pipeline.Flush();
    // Stop while work may still be queued: the component's stop-mode contract keeps
    // draining the remaining tasks (stop_ && tasks empty is the only worker exit), so
    // the locked Stop must wake all 4 parked/gated workers and Join must return.
    pipeline.Stop();
    pipeline.Join();
    std::vector<ComparedItem> out = DrainBounded(pipeline, kEntries);
    pipeline.Drain(out);
    Expect(out.size() == kEntries,
           "ComparePipeline stop-mode lost tasks: drained " + std::to_string(out.size()) +
               " of " + std::to_string(kEntries));
    for (const ComparedItem& item : out) {
        Expect(item.outcome.category == CompareCategory::Missing && !item.outcome.needHash,
               "ComparePipeline drained a verdict other than the scripted Missing");
    }
}

// ---------------------------------------------------------------------------
// Case 4: SetActiveCap on the REAL ComparePipeline raises the cap mid-flight and wakes
// the cap-parked workers (the compare twin of the io cap gate).
// ---------------------------------------------------------------------------
void TestComparePipelineCapRaiseWakesWorkers() {
    ComparePipelineConfig cfg;
    cfg.workerCount = 4;
    cfg.batchPop = 4;
    cfg.mode = CompareMode::Fast;
    ComparePipeline pipeline(cfg,
                             [](const std::string&) { return std::optional<FileEntry>{}; },
                             []() {});
    pipeline.SetActiveCap(1);

    const std::size_t kEntries = 24;
    for (std::size_t i = 0; i < kEntries; ++i) {
        pipeline.Enqueue(MakeEntry("c" + std::to_string(i), 2, static_cast<int64_t>(i)));
    }
    pipeline.Flush();
    // Mid-flight cap raise (1 -> 2): workers parked at the cap gate must wake and take
    // the freed slot; no task may be lost.
    pipeline.SetActiveCap(2);
    std::vector<ComparedItem> out = DrainBounded(pipeline, kEntries);
    pipeline.Stop();
    pipeline.Join();
    pipeline.Drain(out);
    Expect(out.size() == kEntries,
           "ComparePipeline cap-raise lost tasks: drained " + std::to_string(out.size()) +
               " of " + std::to_string(kEntries));
}

}  // namespace

void RunTaskQueueShutdownTests() {
    TestShutdownFlaggedLostWakeupDeterministic();
    TestSetCapFlaggedWakesCapParkedWorker();
    TestComparePipelineStopUnderMutex();
    TestComparePipelineCapRaiseWakesWorkers();
}
