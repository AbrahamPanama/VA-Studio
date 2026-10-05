// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_EXPLODE_BITMAP_JOBS_H
#define INKSCAPE_UI_EXPLODE_BITMAP_JOBS_H
#include <functional>
#include <optional>
#include <vector>
#include <glibmm/main.h>
#include "async/bitmap-job-reaper.h"
#include "async/channel.h"

namespace Inkscape::Bitmap {
// Owner-local typed failure receipt. Never infer a reason from diagnostic text.
enum class CliBitmapStage { Resolve, Analyze, Contour, Encode, Publish };
enum class CliBitmapReason {
    None, UnsupportedTarget, MemoryAdmissionFailed, AnalysisFailed, ContourFailed,
    EncodingFailed, PublicationFailed, DocumentBusy, StaleCapture, InternalError, Canceled, MissingSource, EngineLimit
};
struct CliBitmapFailure {
    CliBitmapStage stage;
    CliBitmapReason reason;
    bool operator==(CliBitmapFailure const &) const = default;
};
inline std::optional<CliBitmapFailure> bitmapFailure(Outcome const &o, CliBitmapStage stage,
                                                    CliBitmapReason reason) noexcept {
    if (o.ok()) return {};
    return CliBitmapFailure{stage, o.status == Status::canceled ? CliBitmapReason::Canceled :
        o.insufficientMemory ? CliBitmapReason::MemoryAdmissionFailed : reason};
}
struct CliBitmapOutcome : Outcome {
    std::optional<CliBitmapFailure> failure;
    bool rolledBack = false;
    CliBitmapOutcome(Outcome o = {}, CliBitmapStage stage = CliBitmapStage::Publish,
                     CliBitmapReason reason = CliBitmapReason::InternalError)
        : Outcome(o), failure(bitmapFailure(o, stage, reason)) {}
    CliBitmapOutcome(Status status, char const *message,
                     CliBitmapReason reason = CliBitmapReason::InternalError)
        : CliBitmapOutcome(Outcome{status, message}, CliBitmapStage::Publish, reason) {}
};
using Ticket = std::uint64_t;
using JobClock = std::chrono::steady_clock;
using JobNow = JobClock::time_point (*)(); // injection cannot capture live UI
// Plain immutable data only; destruction must be safe on main, worker or reaper.
struct JobPayload { virtual ~JobPayload() = default; };
struct JobBytes {
    JobBytes() = default;
    JobBytes(JobBytes &&) noexcept = default;
    JobBytes &operator=(JobBytes &&other) noexcept
    {
        if (this != &other) {
            std::vector<std::uint8_t>().swap(bytes); // storage dies before its reservation
            payload.reset(); // payload buffers die while the old ledger is pinned
            reservation.release(); // ledger still pinned, including replacement/clear
            budget = std::move(other.budget);
            reservation = std::move(other.reservation);
            payload = std::move(other.payload);
            bytes = std::move(other.bytes);
        }
        return *this;
    }
    // Destruction order: bytes, payload, reservation, ledger. Single storage/token owner.
    std::shared_ptr<Budget> budget;
    Budget::Token reservation;
    std::shared_ptr<JobPayload const> payload; // allocations must be admitted before construction
    std::vector<std::uint8_t> bytes;
};
enum class JobPhase { preparation, contourTrace, contourFit };
struct JobProgress {
    Stage stage = Stage::input;
    std::uint64_t completed = 0, total = 0;
    JobPhase phase = JobPhase::preparation;
};
struct JobMailbox;
class JobReporter {
public:
    void progress(JobProgress); // worker only, coalesced; no client callback
private:
    friend class BitmapJobs;
    JobReporter(JobMailbox &, JobNow);
    JobMailbox &_box;
    JobNow _now;
    std::optional<JobClock::time_point> _last;
};
struct JobInput;
struct JobResult {
    Outcome outcome;
    JobBytes value;
    std::uint64_t consumed = 0;
    std::optional<CliBitmapFailure> failure;
    bool ok() const noexcept { return outcome.ok(); }
};
// Function pointer, not std::function: no captured document, XML, GTK or live pointer.
// Only qualified cooperative functions: check Stop <=10 ms/1 MP/64 KiB, stop <=100 ms,
// phase <=30 s. Pass the same JobWork through phases; never reset its visit allowance.
// Reserve storage using admit()/Budget::recheck before allocation; pin that ledger here.
using JobFunction = JobResult (*)(JobInput const &, Stop, JobWork &, JobReporter &);
struct JobInput {
    JobBytes storage;
    std::uint64_t pixels = 0, tag = 0;
    JobFunction work = nullptr;
    CliBitmapStage stage = CliBitmapStage::Analyze; // Explicit work stage survives launch/cancel/exception delivery.
};
void closeBitmapJobDelivery() noexcept; // application destruction, before worker detach
class BitmapJobs {
public:
    using Complete = std::function<void(Ticket, JobResult)>;
    using Progress = std::function<void(Ticket, JobProgress)>;
    explicit BitmapJobs(Complete, Progress = {}, JobNow = JobClock::now, bool automatic = true);
    ~BitmapJobs(); // close only; never joins
    BitmapJobs(BitmapJobs const &) = delete;
    BitmapJobs &operator=(BitmapJobs const &) = delete;
    Ticket request(JobInput);
    void cancel(Ticket);
    void close();
    void poll() noexcept; // main loop; explicit pump with injected clocks for deterministic tests
    Ticket latest() const { checkThread(); return _ticket; }
    bool active() const { checkThread(); return !jobReaped(_job); }
private:
    friend void closeBitmapJobDelivery() noexcept;
    BitmapJobs *_next = nullptr; // intrusive main-thread controller registry
    void checkThread() const;
    std::thread::id const _main;
    Complete _complete;
    Progress _progress;
    JobNow _now;
    Ticket _ticket = 0, _running = 0;
    bool _closed = false, _retiring = false;
    std::optional<JobInput> _pending;
    JobClock::time_point _due;
    std::optional<JobClock::time_point> _lastDelivered;
    JobHandle _job;
    std::shared_ptr<JobMailbox> _box;
    Async::Channel::Dest _destination; // created/closed exclusively on main
    sigc::connection _timer;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};
} // namespace Inkscape::Bitmap
#endif
