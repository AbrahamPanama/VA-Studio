// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_ASYNC_BITMAP_JOB_REAPER_H
#define INKSCAPE_ASYNC_BITMAP_JOB_REAPER_H
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include "util/bitmap-island-budget.h"

namespace Inkscape::Bitmap {
struct JobControl;
using JobHandle = std::shared_ptr<JobControl>;
// Internal launch hook: closures must own plain storage only. No UI references.
using BitmapThreadStart = std::thread (*)(std::function<void()>); // injectable launch failure
JobHandle launchBitmapJob(std::function<void(Stop)>, std::function<void()> reclaim, BitmapThreadStart = nullptr);
void retire(JobHandle);
bool jobRetired(JobHandle const &) noexcept;
bool jobFinished(JobHandle const &) noexcept;
bool jobReaped(JobHandle const &) noexcept;
void stopBitmapJob(JobHandle const &) noexcept;
unsigned activeBitmapJobs() noexcept; // includes finished/retiring storage
// Passive, bounded wait; zero is a deterministic nonblocking probe for tests.
bool waitForBitmapReaper(std::chrono::milliseconds);
bool bitmapReaperExists() noexcept;
void shutdownBitmapReaper() noexcept; // bounded wait, then detach owned plain state
void recordBitmapMainThread() noexcept; // application startup, never service first use
void drainReaper() noexcept; // stop all jobs; at most 100 ms; retry quit if still active
void assertBitmapMainThread(std::thread::id);
void assertBitmapMainThread(); // canonical thread, including controller construction
} // namespace Inkscape::Bitmap
#endif
