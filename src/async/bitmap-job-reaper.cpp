// SPDX-License-Identifier: GPL-2.0-or-later
#include "async/bitmap-job-reaper.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <mutex>
#include <stdexcept>
#include "util/crash-handler-thread.h"

namespace Inkscape::Bitmap {
namespace { std::thread::id application_main; }
void recordBitmapMainThread() noexcept
{
    if (application_main == std::thread::id{}) application_main = std::this_thread::get_id();
    assertBitmapMainThread();
}
void assertBitmapMainThread(std::thread::id main)
{
    if (!Util::crash_handler_on_main_thread(main)) {
        std::fputs("Bitmap jobs: main-thread affinity violation\n", stderr); std::fflush(stderr);
        std::abort(); // remains enforced in release builds
    }
}
void assertBitmapMainThread() { assertBitmapMainThread(application_main); }
namespace {
void disposeThread(std::unique_ptr<std::thread> &thread, bool join) noexcept
{
    if (!thread) return;
    try { if (thread->joinable()) { if (join) thread->join(); else thread->detach(); } }
    catch (...) {
        std::fputs("Bitmap jobs: thread cleanup failed; retaining thread handle\n", stderr); std::fflush(stderr);
        try { if (thread->joinable()) thread->detach(); } catch (...) { (void)thread.release(); }
    }
    thread.reset();
}
std::thread startThread(std::function<void()> work) { return std::thread(std::move(work)); }
}
struct JobControl {
    std::shared_ptr<std::atomic<bool>> flag = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> finished{false}, retired{false}, reaped{false};
    std::unique_ptr<std::thread> worker;
    std::function<void(Stop)> work;
    std::function<void()> reclaim;
    ~JobControl() { disposeThread(worker, false); }
};
namespace {
// Owned by the reaper and workers, never by reference to application/service objects.
struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::list<JobHandle> jobs;
    std::atomic<unsigned> active{0};
    bool exiting = false, exited = false;
    void run() noexcept
    {
        try {
            std::unique_lock lock(mutex);
            for (;;) {
                changed.wait(lock, [this] {
                    if (exiting && jobs.empty()) return true;
                    for (auto &job : jobs) if (job->retired && job->finished) return true;
                    return false;
                });
                if (exiting && jobs.empty()) { exited = true; changed.notify_all(); return; }
                auto it = jobs.begin();
                while (it != jobs.end()) {
                    auto job = *it;
                    if (!job->retired || !job->finished) { ++it; continue; }
                    it = jobs.erase(it);
                    lock.unlock();
                    disposeThread(job->worker, true);
                    try { job->reclaim(); } catch (...) { std::fputs("Bitmap jobs: reclamation failed\n", stderr); std::fflush(stderr); }
                    job->reclaim = {};
                    job->work = {}; // buffers/tokens die before admission drops
                    lock.lock();
                    job->reaped.store(true, std::memory_order_release);
                    --active;
                    changed.notify_all();
                }
            }
        } catch (...) { std::fputs("Bitmap jobs: reaper failure contained\n", stderr); std::fflush(stderr); }
    }
};
class Reaper {
public:
    std::shared_ptr<State> state = std::make_shared<State>();
    std::unique_ptr<std::thread> thread;
    explicit Reaper(BitmapThreadStart start) : thread(std::make_unique<std::thread>())
    { *thread = start([state = state] { state->run(); }); }
    ~Reaper() { shutdown(); }
    void shutdown() noexcept
    {
        if (!thread) return;
        bool joined = false;
        try {
            std::unique_lock lock(state->mutex);
            state->exiting = true;
            for (auto &job : state->jobs) { stopBitmapJob(job); job->retired = true; }
            state->changed.notify_all();
            joined = state->changed.wait_for(lock, std::chrono::milliseconds(100), [this] { return state->exited; });
        } catch (...) { std::fputs("Bitmap jobs: shutdown failure contained\n", stderr); std::fflush(stderr); }
        if (!joined) { std::fputs("Bitmap jobs: incomplete drain; detached plain worker state\n", stderr); std::fflush(stderr); }
        disposeThread(thread, joined);
    }
};
std::unique_ptr<Reaper> &service() { static std::unique_ptr<Reaper> instance; return instance; }
}
bool bitmapReaperExists() noexcept { return bool(service()); }
void shutdownBitmapReaper() noexcept { if (auto &r = service()) r->shutdown(); }
JobHandle launchBitmapJob(std::function<void(Stop)> work, std::function<void()> reclaim, BitmapThreadStart start)
{
    assertBitmapMainThread(); // BEFORE allocation or first-use service construction
    if (!start) start = startThread;
    auto &instance = service();
    if (!instance) instance = std::make_unique<Reaper>(start);
    auto state = instance->state;
    auto job = std::make_shared<JobControl>();
    job->work = std::move(work);
    job->reclaim = std::move(reclaim);
    job->worker = std::make_unique<std::thread>();
    std::lock_guard lock(state->mutex);
    if (state->exiting) throw std::runtime_error("Bitmap service shut down");
    state->jobs.push_back(job); // allocate before launching: no unowned joinable thread
    try {
        *job->worker = start([job, state] {
            try { job->work(Stop(job->flag)); }
            catch (...) { /* second root boundary; runner supplies typed failures */ }
            try {
                std::lock_guard lock(state->mutex);
                job->finished.store(true, std::memory_order_release);
            } catch (...) { job->finished = true; }
            state->changed.notify_all();
        });
        ++state->active;
    } catch (...) { state->jobs.pop_back(); throw; }
    return job;
}
void stopBitmapJob(JobHandle const &job) noexcept
{
    if (job) job->flag->store(true, std::memory_order_release);
}
bool jobRetired(JobHandle const &job) noexcept { return job && job->retired.load(); }
bool jobFinished(JobHandle const &job) noexcept { return job && job->finished.load(std::memory_order_acquire); }
bool jobReaped(JobHandle const &job) noexcept { return !job || job->reaped.load(std::memory_order_acquire); }
void retire(JobHandle job)
{
    assertBitmapMainThread();
    if (!job) return;
    stopBitmapJob(job);
    if (auto &r = service()) {
        { std::lock_guard lock(r->state->mutex); job->retired = true; }
        r->state->changed.notify_all();
    }
}
unsigned activeBitmapJobs() noexcept { auto &r = service(); return r ? r->state->active.load() : 0; }
bool waitForBitmapReaper(std::chrono::milliseconds timeout)
{
    assertBitmapMainThread();
    auto &r = service();
    if (!r) return true;
    auto &s = *r->state;
    std::unique_lock lock(s.mutex);
    return s.changed.wait_for(lock, timeout, [&s] { return s.active == 0; });
}
void drainReaper() noexcept
{
    if (!bitmapReaperExists()) return; // quit must never allocate/start a service
    try {
        assertBitmapMainThread();
        auto &s = *service()->state;
        { std::lock_guard lock(s.mutex);
          for (auto &job : s.jobs) { stopBitmapJob(job); job->retired = true; } }
        s.changed.notify_all();
        waitForBitmapReaper(std::chrono::milliseconds(100));
    } catch (...) { std::fputs("Bitmap jobs: drain failure contained\n", stderr); std::fflush(stderr); }
}
} // namespace Inkscape::Bitmap
