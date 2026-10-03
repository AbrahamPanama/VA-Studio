// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/explode-bitmap-jobs.h"
#include <limits>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <stdexcept>

namespace Inkscape::Bitmap {
using namespace std::chrono_literals;
namespace { BitmapJobs *controllers = nullptr; }
struct JobMailbox : std::enable_shared_from_this<JobMailbox> {
    JobInput input;
    std::mutex mutex;
    std::optional<JobProgress> progress;
    JobResult result;
    bool queued = false, notified = false;
    std::atomic<bool> deliveryClosed{false};
    Async::Channel::Source source;
    void wake() // caller holds mutex; at most one Channel callback outstanding
    {
        if (deliveryClosed || queued) return;
        queued = true;
        source.run([box = shared_from_this()] { std::lock_guard lock(box->mutex); box->notified = true; });
    }
};
JobReporter::JobReporter(JobMailbox &box, JobNow now) : _box(box), _now(now) {}
void JobReporter::progress(JobProgress value)
{
    auto now = _now();
    if (_last && now - *_last < 100ms) return;
    _last = now;
    std::lock_guard lock(_box.mutex);
    _box.progress = value;
    _box.wake();
}
BitmapJobs::BitmapJobs(Complete complete, Progress progress, JobNow now, bool automatic)
    : _main(std::this_thread::get_id()), _complete(std::move(complete)),
      _progress(std::move(progress)), _now(now ? now : JobClock::now)
{
    assertBitmapMainThread(); // application records affinity before service first use
    if (automatic) _timer = Glib::signal_timeout().connect([this, alive = _alive] { poll(); return *alive; }, 10);
    _next = controllers; controllers = this;
}
BitmapJobs::~BitmapJobs()
{
    auto **link = &controllers;
    while (*link != this) link = &(*link)->_next;
    *link = _next;
    try { close(); } catch (...) { std::fputs("Bitmap jobs: close failure contained\n", stderr); std::fflush(stderr); }
    *_alive = false;
}
void closeBitmapJobDelivery() noexcept
{
    for (auto *job = controllers; job; job = job->_next) {
        try { job->close(); } catch (...) { std::fputs("Bitmap jobs: delivery shutdown failure contained\n", stderr); std::fflush(stderr); }
    }
}
void BitmapJobs::checkThread() const { assertBitmapMainThread(_main); }
Ticket BitmapJobs::request(JobInput input)
{
    checkThread();
    if (_closed) return 0;
    if (_ticket == std::numeric_limits<Ticket>::max()) throw std::overflow_error("bitmap ticket exhausted");
    ++_ticket;
    stopBitmapJob(_job);
    _pending = std::move(input); // replaces, never accumulates a queue
    _due = _now() + 250ms;
    return _ticket;
}
void BitmapJobs::cancel(Ticket ticket)
{
    checkThread();
    if (_closed || ticket != _ticket) return;
    _pending.reset();
    if (_running == ticket) stopBitmapJob(_job);
    // Invalidate delivery without consuming a generation (ticket is still cancelable).
    _running = 0;
}
void BitmapJobs::close()
{
    checkThread();
    if (_closed) return;
    _closed = true; // latch before closing delivery
    if (_box) _box->deliveryClosed = true; // worker wake cannot use GLib after teardown
    _timer.disconnect();
    _destination.close();
    _pending.reset();
    retire(std::move(_job));
    _box.reset(); // reaper still owns worker state and queued result
}
void BitmapJobs::poll() noexcept
try {
    checkThread();
    if (_closed) return;
    auto alive = _alive; // callbacks can destroy this controller
    if (_job && !_retiring) {
        std::optional<JobProgress> progress;
        {
            std::lock_guard lock(_box->mutex);
            if (_box->notified && (!_lastDelivered || _now() - *_lastDelivered >= 100ms)) {
                progress = std::exchange(_box->progress, {});
                _box->queued = _box->notified = false;
            }
        }
        if (!jobRetired(_job) && _running == _ticket && progress && _progress) {
            _lastDelivered = _now();
            try { auto callback = _progress; callback(_running, *progress); }
            catch (...) { std::fputs("Bitmap jobs: progress callback failure contained\n", stderr); std::fflush(stderr); }
            if (!*alive) return;
        }
        if (jobFinished(_job)) {
            auto ticket = _running;
            std::optional<JobResult> result;
            if (_complete && !jobRetired(_job) && ticket && ticket == _ticket) result = std::move(_box->result);
            _destination.close(); // cancel any outstanding plain mailbox wake
            retire(_job);
            _box.reset(); // stale results/storage die on the reaper
            _retiring = true;
            if (result && _complete) {
                try { auto callback = _complete; callback(ticket, std::move(*result)); }
                catch (...) { std::fputs("Bitmap jobs: completion callback failure contained\n", stderr); std::fflush(stderr); }
                if (!*alive) return;
            }
        }
    }
    if (_closed) return;
    if (_job && jobReaped(_job)) { _job.reset(); _retiring = false; }
    if (_job || !_pending || _now() < _due || activeBitmapJobs() != 0) return;
    _running = _ticket;
    try {
        _box = std::make_shared<JobMailbox>();
        _box->input = std::move(*_pending);
        _pending.reset();
        auto [source, dest] = Async::Channel::create();
        _box->source = std::move(source);
        _destination = std::move(dest);
        _job = launchBitmapJob([box = _box, now = _now](Stop stop) {
            try {
                JobWork work(box->input.pixels);
                JobReporter reporter(*box, now);
                if (stop.requested()) box->result.outcome = {Status::canceled, "Canceled"};
                else if (!box->input.work) box->result.outcome = {Status::failed, "Missing job function"};
                else box->result = box->input.work(box->input, stop, work, reporter);
                if (stop.requested()) box->result.outcome = {Status::canceled, "Canceled"};
            } catch (...) { box->result.outcome = {Status::failed, "Bitmap worker exception"}; }
            // Wake failures (e.g. allocation) cannot escape the worker root or strand a result.
            try { std::lock_guard lock(box->mutex); box->wake(); } catch (...) {}
        }, [box = _box] {
            std::lock_guard lock(box->mutex);
            box->input = {};
            box->result = {};
            box->progress.reset();
            box->source.close();
        });
    } catch (...) {
        _destination.close();
        _pending.reset();
        _box.reset();
        if (_complete) { auto callback = _complete; callback(_running, {{Status::failed, "Bitmap launch failed"}, {}, 0}); }
    }
}
catch (...) { std::fputs("Bitmap jobs: main-thread delivery failure contained\n", stderr); std::fflush(stderr); }
} // namespace Inkscape::Bitmap
