// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UTIL_BITMAP_ISLAND_BUDGET_H
#define INKSCAPE_UTIL_BITMAP_ISLAND_BUDGET_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>
#if defined(__APPLE__)
#include <os/lock.h>
#endif
// No <windows.h> here: this header reaches files that also include gtkmm, whose enums collide with Windows
// macros. Windows therefore uses std::mutex, like the other non-Apple platforms.

namespace Inkscape::Bitmap {

// Static diagnostics: even an OOM result needs no allocation.
enum class Status { changed, unchanged, incompatible, unavailable, canceled, failed };
struct Outcome {
    Status status = Status::unchanged;
    char const *diagnostic = "";
    constexpr Outcome() noexcept = default;
    constexpr Outcome(Status s, char const *d) noexcept : status(s) {
        std::size_t i = 0;
        for (; i + 1 < message.size() && d[i]; ++i) message[i] = d[i];
        message[i] = 0; diagnostic = message.data();
    }
    constexpr Outcome(Outcome const &o) noexcept : status(o.status), diagnostic(o.diagnostic), message(o.message), insufficientMemory(o.insufficientMemory) {
        if (o.diagnostic == o.message.data()) diagnostic = message.data();
    }
    constexpr Outcome &operator=(Outcome const &o) noexcept {
        if (this != &o) { status = o.status; message = o.message; insufficientMemory = o.insufficientMemory;
            diagnostic = o.diagnostic == o.message.data() ? message.data() : o.diagnostic; }
        return *this;
    }
    std::array<char, 384> message{}; // owned diagnostic survives worker delivery without allocating
    bool insufficientMemory = false;
    bool ok() const noexcept { return status == Status::changed || status == Status::unchanged; }
};
Outcome memoryFailure(char const *limit, std::uint64_t need, std::uint64_t available) noexcept;
template <typename T> struct Result {
    Outcome outcome;
    T value{}; // Refused parsed metadata is diagnostic only; owned storage stays empty.
    std::uint64_t consumed = 0;
    bool ok() const noexcept { return outcome.ok(); }
};
struct RgbaView {
    std::uint8_t const *data = nullptr;
    std::uint64_t bytes = 0, stride = 0;
    std::uint32_t width = 0, height = 0;
    Outcome validate() const noexcept;
};
struct EncodedView {
    std::uint8_t const *data = nullptr;
    std::size_t size = 0;
    std::string_view mime;
};
enum class Stage : unsigned {
    header, input, decode, canonical, preview, composition, straighten,
    topology, crop, encoder, png, href, prepared, nodes, history, rollback, count
};
constexpr std::uint64_t MiB = 1048576;
bool checkedMul(std::uint64_t, std::uint64_t, std::uint64_t &) noexcept;
bool checkedAdd(std::uint64_t, std::uint64_t, std::uint64_t &) noexcept;
bool checked32(std::uint64_t, std::uint32_t &) noexcept;
bool checkedInt32(std::int64_t, std::int32_t &) noexcept;
bool checkedSize(std::uint64_t, std::size_t &) noexcept;

template <typename T> bool checkedNarrow(std::uint64_t v, T &out) noexcept
{
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    if (v > std::numeric_limits<T>::max()) return false;
    out = static_cast<T>(v); return true;
}
bool checkedUnsigned(std::int64_t, std::uint64_t &) noexcept;
bool checkedCeilDiv(std::uint64_t, std::uint64_t, std::uint64_t &) noexcept;
bool checkedBase64Length(std::uint64_t, std::uint64_t &, std::uint64_t prefix = 22) noexcept;
struct Memory {
    std::uint64_t physical = 0, available = 0, baseline = 0;
    bool measured = false;
    std::uint64_t resident = 0; // live job bytes included in current RSS and excluded from available
};
Outcome admissionLimit(Memory const &, std::uint64_t &) noexcept;

// All ledger operations are synchronized. Budget must outlive every Token/buffer.
// A Token itself has one owner; concurrent operations on the same Token are invalid.
class Budget {
public:
    class Token {
    public:
        Token() noexcept = default;
        ~Token() { release(); }
        Token(Token const &) = delete;
        Token &operator=(Token const &) = delete;
        Token(Token &&) noexcept;
        Token &operator=(Token &&) noexcept;
        void release() noexcept; // idempotent; only after corresponding storage dies
        Outcome transfer(Stage s) noexcept { return resize(s, _bytes); }
        Outcome split(std::uint64_t, Token &) noexcept;
        Outcome resize(Stage, std::uint64_t) noexcept; // atomic growth/shrink/transfer
        explicit operator bool() const noexcept { return _owner != nullptr; }
        std::uint64_t bytes() const noexcept { return _bytes; }
    private:
        friend class Budget;
        Budget *_owner = nullptr;
        Stage _stage = Stage::input;
        std::uint64_t _bytes = 0;
    };
    struct FixedLimitForTest {};
    explicit Budget(std::uint64_t limit) noexcept : _ceiling(limit), _limit(0) {}
    Budget(FixedLimitForTest, std::uint64_t limit) noexcept : _ceiling(limit), _limit(limit), _admitted(true) {}
    Budget(Budget const &) = delete;
    Budget &operator=(Budget const &) = delete;
    Outcome acquire(Stage, std::uint64_t, Token &) noexcept;
    // Call with fresh conservative measurements before every stage/publication.
    // Failure blocks new reservations but retains existing storage in the ledger.
    Outcome recheck(Memory const &) noexcept;
    // Native probes measure total process footprint. Restore its increase since
    // operation start, bounded by live job reservations, before anchoring headroom.
    Outcome recheckMeasured(Memory) noexcept;
    std::uint64_t reserved() const noexcept;
    std::uint64_t reserved(Stage) const noexcept;
    std::uint64_t limit() const noexcept;
private:
    struct Guard {
        explicit Guard(Budget const &b) noexcept : budget(b) { budget.lock(); }
        ~Guard() { budget.unlock(); }
        Budget const &budget;
    };
    void lock() const noexcept;
    void unlock() const noexcept;
    Outcome replace(Token &, Stage, std::uint64_t) noexcept;
    Outcome recheckLocked(Memory const &) noexcept;
    void release(Token &) noexcept;
#if defined(__APPLE__)
    mutable os_unfair_lock _lock = OS_UNFAIR_LOCK_INIT;
#else
    mutable std::mutex _lock;
#endif
    std::uint64_t const _ceiling;
    std::uint64_t _limit, _total = 0;
    bool _admitted = false;
    Memory _start{};
    std::array<std::uint64_t, static_cast<unsigned>(Stage::count)> _stages{};
};

// Construct the shared flag on the main thread; copies survive panel retirement.
class Stop {
public:
    Stop() noexcept = default;
    explicit Stop(std::shared_ptr<std::atomic<bool>> flag) noexcept : _flag(std::move(flag)) {}
    bool requested() const noexcept { return _flag && _flag->load(std::memory_order_acquire); }
private:
    std::shared_ptr<std::atomic<bool>> _flag;
};

// One meter per job, passed through every phase. Pixel scans get 8P visits;
// all topology/spatial clients share the remaining 100M allowance (no resets).
class JobWork {
public:
    explicit JobWork(std::uint64_t pixels) noexcept;
    JobWork(std::uint64_t pixels, std::uint64_t perPixel) noexcept;
    Outcome advance(std::uint64_t) noexcept;
    std::uint64_t visits() const noexcept { return _visits.load(std::memory_order_relaxed); }
    std::uint64_t limit() const noexcept { return _limit; }
private:
    std::uint64_t _limit = 0;
    std::atomic<std::uint64_t> _visits{0};
};
class PhaseTimer {
public:
    using Clock = std::chrono::steady_clock;
    explicit PhaseTimer(Clock::time_point start = Clock::now()) noexcept : _start(start) {}
    Outcome check(Stop = {}, Clock::time_point now = Clock::now()) const noexcept;
private:
    Clock::time_point _start;
};

// Caller-local test hook: fail the kth allocation (0 disables); no global state.
struct AllocationFault {
    std::uint64_t failAt = 0, attempts = 0;
    bool fail() noexcept;
};
// Raw uninitialized bytes, never objects. Single owner, movable across threads.
// Allocate into an empty buffer; replacement must separately reserve live overlap.
class PlainBuffer {
public:
    PlainBuffer() noexcept = default;
    ~PlainBuffer() { reset(); }
    PlainBuffer(PlainBuffer const &) = delete;
    PlainBuffer &operator=(PlainBuffer const &) = delete;
    PlainBuffer(PlainBuffer &&) noexcept;
    PlainBuffer &operator=(PlainBuffer &&) noexcept;
    Outcome allocate(Budget &, Stage, std::uint64_t count, std::uint64_t elementSize,
                     AllocationFault * = nullptr, Stop = {}) noexcept;
    Outcome grow(Budget &, Stage, std::uint64_t count, std::uint64_t elementSize,
                 AllocationFault * = nullptr, Stop = {}) noexcept;
    void reset() noexcept;
    std::byte *data() noexcept { return _data; }
    std::byte const *data() const noexcept { return _data; }
    std::size_t size() const noexcept { return _size; }
private:
    Budget::Token _token;
    std::byte *_data = nullptr;
    std::size_t _size = 0;
};
} // namespace Inkscape::Bitmap
#endif
