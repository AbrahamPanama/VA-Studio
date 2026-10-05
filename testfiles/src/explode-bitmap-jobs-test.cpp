// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <atomic>
#include <latch>
#include <stdexcept>
#include <system_error>
#include <glibmm/init.h>
#include <gtkmm/application.h>
#include "inkscape-application.h"
#include "ui/explode-bitmap-jobs.h"

// Windows test executables use the GUI subsystem, so gtest cannot capture a death-test child's stderr (it reaches the
// parent's log instead): on Windows these tests check the death and exit code only, elsewhere the message too.
#ifdef _WIN32
#define VA_DEATH_MESSAGE(text) ""
#else
#define VA_DEATH_MESSAGE(text) text
#endif

using namespace Inkscape::Bitmap;
using namespace std::chrono_literals;
namespace {
std::atomic<long long> ticks{0};
JobClock::time_point now() { return JobClock::time_point(std::chrono::milliseconds(ticks.load())); }
// Every harness wait has a real-time watchdog, including worker-side gates.
struct Gate : std::latch {
    using std::latch::latch;
    void wait() const {
        auto deadline = JobClock::now() + 2s;
        while (!try_wait() && JobClock::now() < deadline) std::this_thread::yield();
        if (!try_wait()) throw std::runtime_error("bitmap test gate timed out");
    }
};
long long realTicks() { return JobClock::now().time_since_epoch().count(); }
struct Gates {
    Gate entered{1}, release{1}, acknowledged{1}, finished{1};
    std::atomic<unsigned> starts{0};
    std::thread::id worker;
    std::atomic<long long> stoppedAt{0};
};
Gates *gates = nullptr; // test-only plain worker harness; never production UI
JobResult run(JobInput const &input, Stop stop, JobWork &work, JobReporter &reporter)
{
    auto &g = *gates;
    g.worker = std::this_thread::get_id();
    ++g.starts;
    g.entered.count_down();
    g.release.wait();
    if (stop.requested()) {
        g.stoppedAt = realTicks();
        g.acknowledged.count_down();
    }
    if (!input.storage.bytes.empty()) {
        EXPECT_EQ(input.storage.bytes.size(), 64u);
        EXPECT_EQ(input.storage.bytes.front(), 17u);
    }
    // One logical bounded chunk. Engine phase loops own the same Stop/JobWork.
    auto outcome = work.advance(1);
    for (unsigned i = 0; i < 1000; ++i) reporter.progress({Stage::preview, i, 1000});
    g.finished.count_down();
    if (input.tag == 1) throw std::bad_alloc();
    if (input.tag == 2) throw std::length_error("fault");
    if (input.tag == 3) throw 42;
    JobResult result;
    result.outcome = outcome.ok() ? Outcome{Status::changed, "done"} : outcome;
    result.consumed = input.tag;
    return result;
}
JobInput input(std::uint64_t tag = 0) { JobInput value; value.pixels = 10; value.tag = tag; value.work = run; return value; }
class JobsTest : public ::testing::Test {
protected:
    void SetUp() override { Glib::init(); ticks = 0; recordBitmapMainThread(); GTEST_FLAG_SET(death_test_style, "threadsafe"); }
    void TearDown() override { drainReaper(); EXPECT_TRUE(waitForBitmapReaper(2s)); gates = nullptr; }
    template <typename Predicate> void pump(BitmapJobs &jobs, Predicate ready)
    {
        // Debounce is logical; real time bounds waits and measures Stop acknowledgement.
        auto deadline = JobClock::now() + 2s;
        auto context = Glib::MainContext::get_default();
        while (!ready() && JobClock::now() < deadline) {
            context->iteration(false);
            jobs.poll();
            std::this_thread::yield();
        }
        ASSERT_TRUE(ready());
    }
};
TEST_F(JobsTest, T07RootCatchAllAndMainDelivery)
{
    for (auto fault : {1u, 2u, 3u}) {
        Gates g; gates = &g;
        unsigned delivered = 0;
        auto main = std::this_thread::get_id();
        BitmapJobs jobs([&](Ticket ticket, JobResult result) {
            EXPECT_EQ(std::this_thread::get_id(), main);
            EXPECT_EQ(ticket, jobs.latest());
            EXPECT_EQ(result.outcome.status, Status::failed);
            if (fault == 1) EXPECT_NE(std::string(result.outcome.diagnostic).find("Not enough memory"), std::string::npos);
            ++delivered;
        }, {}, now, false);
        jobs.request(input(fault));
        ticks += 250; jobs.poll(); g.entered.wait(); g.release.count_down();
        pump(jobs, [&] { return delivered == 1 && !jobs.active(); });
        EXPECT_NE(g.worker, main);
    }
}
struct ThrowingCopy {
    bool *fail;
    unsigned *copies, *calls;
    explicit ThrowingCopy(bool &value, unsigned *copy = nullptr, unsigned *call = nullptr) : fail(&value), copies(copy), calls(call) {}
    ThrowingCopy(ThrowingCopy const &other) : fail(other.fail), copies(other.copies), calls(other.calls) {
        if (copies) ++*copies;
        if (*fail) throw std::bad_alloc();
    }
    template <typename T> void operator()(Ticket, T) const { if (calls) ++*calls; throw 42; }
};
int threadStarts = 0, failThreadAt = 0;
std::thread faultThread(std::function<void()> work)
{
    if (++threadStarts == failThreadAt) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    return std::thread(std::move(work));
}
TEST_F(JobsTest, T07ThreadCreationAndLaunchFailureDeliveryContained)
{
    EXPECT_EXIT({
        failThreadAt = 1;
        EXPECT_THROW(launchBitmapJob([](Stop) {}, [] {}, faultThread), std::system_error);
        EXPECT_FALSE(bitmapReaperExists());
        drainReaper(); shutdownBitmapReaper();
        failThreadAt = 3; // reaper succeeds; worker thread creation fails
        EXPECT_THROW(launchBitmapJob([](Stop) {}, [] {}, faultThread), std::system_error);
        EXPECT_EQ(activeBitmapJobs(), 0u);
        shutdownBitmapReaper();
        for (bool copyFault : {false, true}) {
            bool failCopy = false;
            BitmapJobs jobs(ThrowingCopy(failCopy), {}, now, false);
            jobs.request(input()); ticks += 250; failCopy = copyFault;
            EXPECT_NO_THROW(jobs.poll()); // shutdown service refuses launch; callback throws
        }
        std::exit(::testing::Test::HasFailure() ? 1 : 0);
    }, ::testing::ExitedWithCode(0), VA_DEATH_MESSAGE("failure contained"));
}
struct QuitApplication : InkscapeApplication {};
TEST_F(JobsTest, T10UnusedApplicationDestructionNeverStartsReaper)
{
    EXPECT_EXIT({
        EXPECT_FALSE(bitmapReaperExists());
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "bitmapquitunused", true);
        { QuitApplication app; }
        drainReaper(); shutdownBitmapReaper();
        EXPECT_FALSE(bitmapReaperExists());
        std::exit(::testing::Test::HasFailure() ? 1 : 0);
    }, ::testing::ExitedWithCode(0), "");
}
JobResult stallForever(JobInput const &, Stop, JobWork &, JobReporter &)
{
    gates->entered.count_down(); // intentionally uncooperative test function, no later harness/UI access
    for (;;) std::this_thread::sleep_for(10ms);
}
void stalledApplicationQuit(bool release)
{
    Gtk::Application::wrap_in_search_entry2();
    g_setenv("INKSCAPE_APP_ID_TAG", "bitmapquitstalled", true);
    auto app = std::make_unique<QuitApplication>();
    Gates g; gates = &g;
    unsigned callbacks = 0;
    BitmapJobs jobs([&](Ticket, JobResult) { ++callbacks; }, [&](Ticket, JobProgress) { ++callbacks; }, now);
    auto value = input(4);
    if (!release) value.work = stallForever;
    jobs.request(std::move(value)); ticks = 250; jobs.poll(); g.entered.wait();
    auto started = JobClock::now();
    app.reset(); // direct destructor: deliberately bypasses _finishQuitWhenReady()
    EXPECT_LT(JobClock::now() - started, 1s);
    EXPECT_EQ(activeBitmapJobs(), 1u);
    EXPECT_EQ(jobs.request(input()), 0u); // delivery has closed before application teardown
    if (release) {
        g.release.count_down(); g.acknowledged.wait();
        EXPECT_TRUE(waitForBitmapReaper(2s)); // detached reaper still reclaims safely
        while (Glib::MainContext::get_default()->iteration(false)) {}
        EXPECT_EQ(callbacks, 0u);
    }
    // std::exit keeps the permanently stalled gate's storage alive until process exit.
    std::exit(::testing::Test::HasFailure() ? 1 : 0);
}
TEST_F(JobsTest, T10StalledQuitDetachesAndReclaimsWithoutLateDelivery)
{
    EXPECT_EXIT(stalledApplicationQuit(true), ::testing::ExitedWithCode(0), VA_DEATH_MESSAGE("incomplete drain; detached plain worker state"));
}
TEST_F(JobsTest, T10PermanentlyStalledQuitDoesNotHangStaticDestruction)
{
    EXPECT_EXIT(stalledApplicationQuit(false), ::testing::ExitedWithCode(0), VA_DEATH_MESSAGE("incomplete drain; detached plain worker state"));
}
TEST_F(JobsTest, T08AffinityRemainsEnforcedInRelease)
{
    EXPECT_DEATH({
        EXPECT_FALSE(bitmapReaperExists()); // SetUp records application thread without service construction
        std::thread wrong([] { BitmapJobs jobs({}, {}, now, false); }); wrong.join();
    }, VA_DEATH_MESSAGE("main-thread affinity"));
    EXPECT_DEATH({
        BitmapJobs jobs({}, {}, now, false);
        std::thread wrong([&] { jobs.request(input()); }); wrong.join();
    }, VA_DEATH_MESSAGE("main-thread affinity"));
}
TEST_F(JobsTest, T10CloseDoesNotJoinAndNoLateCallbacks)
{
    Gates g; gates = &g;
    unsigned callbacks = 0;
    auto jobs = std::make_unique<BitmapJobs>([&](Ticket, JobResult) { ++callbacks; },
        [&](Ticket, JobProgress) { ++callbacks; }, now, false);
    jobs->request(input()); ticks = 250; jobs->poll(); g.entered.wait();
    jobs.reset(); // returns while worker is latched: proves teardown never waits/joins
    EXPECT_EQ(activeBitmapJobs(), 1u);
    EXPECT_FALSE(waitForBitmapReaper(0ms));
    g.release.count_down(); g.acknowledged.wait();
    ASSERT_TRUE(waitForBitmapReaper(2s));
    while (Glib::MainContext::get_default()->iteration(false)) {}
    EXPECT_EQ(callbacks, 0u);
}
TEST_F(JobsTest, T10QuitHasBoundedDrainAndRetainsStalledStorage)
{
    Gates g; gates = &g;
    BitmapJobs jobs({}, {}, now, false);
    jobs.request(input()); ticks = 250; jobs.poll(); g.entered.wait();
    drainReaper(); // default 100 ms timeout returns even though latch is still closed
    EXPECT_EQ(activeBitmapJobs(), 1u);
    EXPECT_FALSE(waitForBitmapReaper(0ms));
    jobs.close();
    g.release.count_down(); g.acknowledged.wait();
    drainReaper();
    ASSERT_TRUE(waitForBitmapReaper(2s));
    EXPECT_EQ(activeBitmapJobs(), 0u);
}
TEST_F(JobsTest, T12StopSharedFlagAcknowledgedAtNextBoundedChunk)
{
    Gates g; gates = &g;
    BitmapJobs jobs({}, {}, now, false);
    auto ticket = jobs.request(input()); ticks = 250; jobs.poll(); g.entered.wait();
    auto canceledAt = realTicks();
    jobs.cancel(ticket); g.release.count_down(); g.acknowledged.wait();
    EXPECT_LE(JobClock::duration(g.stoppedAt.load() - canceledAt), 100ms);
    jobs.close();
    ASSERT_TRUE(waitForBitmapReaper(2s));
}
TEST_F(JobsTest, T12BudgetTokenReleasesOnceAfterRetirement)
{
    Gates g; gates = &g;
    auto budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 1024);
    auto value = input(); value.storage.budget = budget;
    ASSERT_TRUE(budget->acquire(Stage::input, 64, value.storage.reservation).ok());
    value.storage.bytes.resize(64, 17);
    BitmapJobs jobs({}, {}, now, false);
    jobs.request(std::move(value)); ticks = 250; jobs.poll(); g.entered.wait();
    jobs.close();
    EXPECT_EQ(budget->reserved(), 64u); // retiring remains in admission's ledger
    EXPECT_EQ(activeBitmapJobs(), 1u);
    g.release.count_down(); g.acknowledged.wait();
    ASSERT_TRUE(waitForBitmapReaper(2s));
    EXPECT_EQ(budget->reserved(), 0u);
    Budget::Token full;
    EXPECT_TRUE(budget->acquire(Stage::input, 1024, full).ok());
    full.release(); full.release();
    EXPECT_EQ(budget->reserved(), 0u);
}
struct OutputGates { Gate prepared{1}, release{1}; };
OutputGates *outputGates = nullptr;
JobResult prepareOutput(JobInput const &input, Stop stop, JobWork &work, JobReporter &reporter)
{
    auto result = run(input, stop, work, reporter);
    result.value.budget = input.storage.budget;
    result.outcome = result.value.budget->acquire(Stage::prepared, 64, result.value.reservation);
    result.value.bytes.resize(64);
    outputGates->prepared.count_down(); outputGates->release.wait();
    return result;
}
TEST_F(JobsTest, T12RetirementOwnsBothInputAndPreparedOutput)
{
    Gates g; gates = &g;
    OutputGates output; outputGates = &output;
    auto budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 256);
    auto value = input(); value.work = prepareOutput; value.storage.budget = budget;
    ASSERT_TRUE(budget->acquire(Stage::input, 64, value.storage.reservation).ok());
    value.storage.bytes.resize(64, 17);
    BitmapJobs jobs({}, {}, now, false);
    jobs.request(std::move(value)); ticks = 250; jobs.poll(); g.entered.wait();
    g.release.count_down(); output.prepared.wait();
    jobs.close();
    EXPECT_EQ(budget->reserved(), 128u);
    EXPECT_EQ(activeBitmapJobs(), 1u);
    output.release.count_down();
    ASSERT_TRUE(waitForBitmapReaper(2s));
    EXPECT_EQ(budget->reserved(), 0u);
    outputGates = nullptr;
}
TEST_F(JobsTest, T29StormDebounceLatestWinsAndRetiringCountsActive)
{
    Gates first; gates = &first;
    unsigned delivered = 0;
    std::uint64_t final = 0;
    BitmapJobs jobs([&](Ticket ticket, JobResult result) {
        ++delivered; final = result.consumed; EXPECT_EQ(ticket, jobs.latest());
    }, {}, now, false);
    jobs.request(input(4)); ticks = 249; jobs.poll(); EXPECT_EQ(activeBitmapJobs(), 0u);
    ticks = 250; jobs.poll(); first.entered.wait();
    for (unsigned i = 0; i < 1000; ++i) { jobs.request(input(10 + i)); jobs.poll(); }
    ticks = 500; jobs.poll(); // deadline passed, but retiring worker still occupies the slot
    EXPECT_EQ(activeBitmapJobs(), 1u);
    EXPECT_EQ(first.starts, 1u);
    jobs.cancel(1); // stale cancel must not cancel latest pending ticket
    ticks = 499; // keep latest debounce pending while the old generation is reaped
    first.release.count_down(); first.acknowledged.wait(); first.finished.wait();
    // Retire old generation before switching the test worker's gate.
    pump(jobs, [&] { return activeBitmapJobs() == 0; });
    Gates last; gates = &last;
    ticks = 500; jobs.poll(); last.entered.wait(); last.release.count_down();
    pump(jobs, [&] { return delivered == 1 && !jobs.active(); });
    EXPECT_EQ(final, 1009u);
    EXPECT_EQ(last.starts, 1u);
}
TEST_F(JobsTest, T29ReopeningPanelCannotAccumulateRetiredWorkers)
{
    Gates old; gates = &old;
    BitmapJobs closing({}, {}, now, false);
    closing.request(input()); ticks = 250; closing.poll(); old.entered.wait(); closing.close();
    unsigned delivered = 0;
    BitmapJobs reopened([&](Ticket, JobResult r) { ++delivered; EXPECT_EQ(r.consumed, 1009u); }, {}, now, false);
    for (unsigned i = 0; i < 1000; ++i) reopened.request(input(10 + i));
    ticks = 500; reopened.poll();
    EXPECT_EQ(activeBitmapJobs(), 1u);
    EXPECT_EQ(old.starts.load(), 1u);
    old.release.count_down(); old.acknowledged.wait();
    ASSERT_TRUE(waitForBitmapReaper(2s));
    Gates current; gates = &current;
    reopened.poll(); current.entered.wait(); current.release.count_down();
    pump(reopened, [&] { return delivered == 1 && !reopened.active(); });
    EXPECT_EQ(current.starts.load(), 1u);
}
TEST_F(JobsTest, T29ProgressMailboxCoalescesAndThrottles)
{
    Gates g; gates = &g;
    unsigned progress = 0, completed = 0;
    BitmapJobs jobs([&](Ticket, JobResult) { ++completed; },
        [&](Ticket, JobProgress value) { ++progress; EXPECT_EQ(value.total, 1000u); }, now, false);
    jobs.request(input(4)); ticks = 250; jobs.poll(); g.entered.wait();
    g.release.count_down(); g.finished.wait();
    // Main loop was blocked throughout 1,000 worker progress calls at the same virtual time.
    pump(jobs, [&] { return completed == 1 && !jobs.active(); });
    EXPECT_EQ(progress, 1u);
}
TEST_F(JobsTest, T29TenThousandCanceledPendingRequestsLaunchNoWorker)
{
    BitmapJobs jobs({}, {}, now, false);
    for (unsigned i = 0; i < 10000; ++i) { auto ticket = jobs.request(input()); jobs.cancel(ticket); }
    ticks = 1000; jobs.poll();
    EXPECT_EQ(activeBitmapJobs(), 0u);
}
struct ProgressGates {
    Gate first{1}, next{1}, second{1}, finish{1};
};
ProgressGates *progressGates = nullptr;
JobResult progressRun(JobInput const &, Stop, JobWork &, JobReporter &reporter)
{
    auto &g = *progressGates;
    reporter.progress({Stage::preview, 0, 2});
    g.first.count_down(); g.next.wait();
    for (unsigned i = 0; i < 1000; ++i) reporter.progress({Stage::preview, 1, 2});
    g.second.count_down(); g.finish.wait();
    return {};
}
TEST_F(JobsTest, T07DeliveryCopiesAndCallbacksCannotEscapePollOrTimer)
{
    for (bool copyFault : {false, true}) {
        ProgressGates g; progressGates = &g;
        bool failCopy = false;
        unsigned copies = 0, calls = 0;
        BitmapJobs jobs(ThrowingCopy(failCopy, &copies, &calls), ThrowingCopy(failCopy, &copies, &calls), now);
        auto baseline = copies;
        auto value = input(); value.work = progressRun;
        jobs.request(std::move(value)); ticks += 250; jobs.poll(); g.first.wait();
        failCopy = copyFault;
        auto context = Glib::MainContext::get_default();
        auto deadline = JobClock::now() + 2s;
        while (copies == baseline && JobClock::now() < deadline) {
            context->iteration(false); std::this_thread::yield(); // GLib timer delivers progress
        }
        EXPECT_GT(copies, baseline);
        g.next.count_down(); g.second.wait(); g.finish.count_down();
        deadline = JobClock::now() + 2s;
        while (jobs.active() && JobClock::now() < deadline) {
            context->iteration(false); std::this_thread::yield(); // GLib timer delivers completion
        }
        EXPECT_FALSE(jobs.active());
        EXPECT_EQ(copies - baseline, 2u); // BOTH callback copies reached the catch boundary
        EXPECT_EQ(calls, copyFault ? 0u : 2u);
        EXPECT_NO_THROW(jobs.poll());
    }
    progressGates = nullptr;
}
TEST_F(JobsTest, T29ProgressDeliverySpacingWithDelayedMainLoop)
{
    ProgressGates g; progressGates = &g;
    unsigned delivered = 0, complete = 0;
    BitmapJobs jobs([&](Ticket, JobResult) { ++complete; }, [&](Ticket, JobProgress p) {
        EXPECT_EQ(p.completed, delivered); ++delivered;
    }, now, false);
    auto value = input(); value.work = progressRun;
    jobs.request(std::move(value)); ticks = 250; jobs.poll(); g.first.wait();
    ticks = 340; pump(jobs, [&] { return delivered == 1; });
    ticks = 350; g.next.count_down(); g.second.wait();
    while (Glib::MainContext::get_default()->iteration(false)) {}
    jobs.poll(); EXPECT_EQ(delivered, 1u); // worker emitted; main delivery still throttled
    ticks = 439; jobs.poll(); EXPECT_EQ(delivered, 1u);
    ticks = 440; pump(jobs, [&] { return delivered == 2; });
    g.finish.count_down();
    pump(jobs, [&] { return complete == 1 && !jobs.active(); });
    progressGates = nullptr;
}
TEST_F(JobsTest, StorageReplacementKeepsLedgerAliveUntilTokenRelease)
{
    JobBytes a, b;
    a.budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 128);
    ASSERT_TRUE(a.budget->acquire(Stage::input, 128, a.reservation).ok());
    a.bytes.resize(128);
    std::weak_ptr<Budget> old = a.budget;
    b.budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 256);
    ASSERT_TRUE(b.budget->acquire(Stage::input, 256, b.reservation).ok());
    b.bytes.resize(256);
    a = std::move(b); // no external owner pins the old ledger
    EXPECT_TRUE(old.expired());
    EXPECT_EQ(a.budget->reserved(), 256u);
    a = {}; // clear storage, then release token, then destroy ledger
}
TEST_F(JobsTest, AutomaticMainLoopTimerDeliversWithoutManualPolling)
{
    Gates g; gates = &g;
    unsigned delivered = 0;
    BitmapJobs jobs([&](Ticket, JobResult result) {
        EXPECT_EQ(result.consumed, 4u); ++delivered;
    }, {}, now);
    jobs.request(input(4)); ticks = 250; g.release.count_down();
    auto context = Glib::MainContext::get_default();
    auto deadline = JobClock::now() + 2s;
    while (!delivered && JobClock::now() < deadline) {
        context->iteration(false); std::this_thread::yield();
    }
    EXPECT_EQ(delivered, 1u);
}
TEST_F(JobsTest, CompletionMayDestroyController)
{
    Gates g; gates = &g;
    std::unique_ptr<BitmapJobs> jobs;
    jobs = std::make_unique<BitmapJobs>([&](Ticket, JobResult) { jobs.reset(); throw 42; }, BitmapJobs::Progress{}, now, false);
    jobs->request(input(4)); ticks = 250; jobs->poll(); g.entered.wait(); g.release.count_down();
    auto context = Glib::MainContext::get_default();
    auto deadline = JobClock::now() + 2s;
    while (jobs && JobClock::now() < deadline) { context->iteration(false); jobs->poll(); std::this_thread::yield(); }
    EXPECT_FALSE(jobs);
}
} // namespace

namespace {
struct PayloadProbe final : JobPayload {
    Budget *ledger; std::atomic<unsigned> *destroyed;
    ~PayloadProbe() override { EXPECT_GE(ledger->reserved(), sizeof(PayloadProbe)); ++*destroyed; }
};
JobInput payloadInput(std::shared_ptr<Budget> budget, std::atomic<unsigned> &destroyed, unsigned tag = 0) {
    auto result = input(tag); result.storage.budget = budget;
    EXPECT_TRUE(budget->acquire(Stage::prepared, sizeof(PayloadProbe), result.storage.reservation).ok());
    auto p = std::make_shared<PayloadProbe>(); p->ledger = budget.get(); p->destroyed = &destroyed;
    result.storage.payload = std::move(p); return result;
}
JobResult payloadOutput(JobInput const &input, Stop stop, JobWork &work, JobReporter &reporter) {
    JobResult result; result.value.budget = input.storage.budget;
    EXPECT_TRUE(result.value.budget->acquire(Stage::prepared, sizeof(PayloadProbe), result.value.reservation).ok());
    result.value.payload = input.storage.payload; // shares immutable result, never the input's reservation
    run(input, stop, work, reporter); return result;
}
}
TEST_F(JobsTest, PayloadPendingMoveExceptionCancelStaleAndCloseBalance) {
    for (unsigned mode = 0; mode < 6; ++mode) {
        auto budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, MiB); std::atomic<unsigned> destroyed{0};
        Gates g; gates = &g; unsigned delivered = 0;
        {
            BitmapJobs jobs([&](Ticket, JobResult r) { ++delivered; if (!mode) EXPECT_TRUE(r.value.payload); }, {}, now, false);
            auto a = payloadInput(budget, destroyed, mode == 1 ? 1 : 0);
            auto b = payloadInput(budget, destroyed); b.storage = std::move(a.storage); EXPECT_EQ(destroyed, 1u);
            if (mode == 1) b.tag = 1; b.work = payloadOutput;
            auto ticket = jobs.request(std::move(b));
            if (mode == 2) { jobs.request(input()); jobs.close(); }
            else {
                ticks += 250; jobs.poll(); g.entered.wait();
                if (mode == 3) jobs.cancel(ticket);
                if (mode == 4) jobs.request(input()); // old completion is stale
                if (mode == 5) jobs.close();
                g.release.count_down(); g.finished.wait();
                if (mode < 2) pump(jobs, [&] { return delivered == 1 && !jobs.active(); });
                jobs.close();
            }
        }
        drainReaper(); ASSERT_TRUE(waitForBitmapReaper(2s)); EXPECT_EQ(destroyed, 2u); EXPECT_EQ(budget->reserved(), 0u);
        if (mode > 1) EXPECT_EQ(delivered, 0u);
    }
}
