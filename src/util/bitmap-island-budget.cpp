// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/bitmap-island-budget.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace Inkscape::Bitmap {
namespace {
Outcome changed() noexcept { return {Status::changed, ""}; }
Outcome failed(char const *why) noexcept { return {Status::failed, why}; }
bool valid(Stage s) noexcept { return s < Stage::count; }
std::uint64_t stageLimit(Stage s) noexcept
{
    switch (s) {
        case Stage::header: return MiB;
        case Stage::topology: return 256 * MiB;
        case Stage::encoder: return 128 * MiB;
        case Stage::href: return 512 * MiB;
        case Stage::preview: return 4 * MiB;
        default: return std::numeric_limits<std::uint64_t>::max();
    }
}
}
bool checkedMul(std::uint64_t a, std::uint64_t b, std::uint64_t &out) noexcept
{
    if (b && a > std::numeric_limits<std::uint64_t>::max() / b) return false;
    out = a * b;
    return true;
}
bool checkedAdd(std::uint64_t a, std::uint64_t b, std::uint64_t &out) noexcept
{
    if (a > std::numeric_limits<std::uint64_t>::max() - b) return false;
    out = a + b;
    return true;
}
bool checked32(std::uint64_t v, std::uint32_t &out) noexcept { return checkedNarrow(v, out); }
bool checkedInt32(std::int64_t v, std::int32_t &out) noexcept
{
    if (v < INT32_MIN || v > INT32_MAX) return false;
    out = static_cast<std::int32_t>(v); return true;
}
bool checkedSize(std::uint64_t v, std::size_t &out) noexcept { return checkedNarrow(v, out); }
bool checkedUnsigned(std::int64_t v, std::uint64_t &out) noexcept
{
    if (v < 0) return false;
    out = static_cast<std::uint64_t>(v); return true;
}
bool checkedCeilDiv(std::uint64_t n, std::uint64_t d, std::uint64_t &out) noexcept
{
    if (!d) return false;
    out = n / d + (n % d != 0); return true;
}
bool checkedBase64Length(std::uint64_t n, std::uint64_t &out, std::uint64_t prefix) noexcept
{
    std::uint64_t groups, payload, total;
    if (!checkedCeilDiv(n, 3, groups) || !checkedMul(groups, 4, payload) ||
        !checkedAdd(payload, prefix, total)) return false;
    out = total; return true;
}
Outcome RgbaView::validate() const noexcept
{
    std::uint64_t row, offset, required, pixels;
    if (!width || !height || width > 16384 || height > 16384 || !data ||
        !checkedMul(width, height, pixels) || pixels > 100000000 ||
        !checkedMul(width, 4, row) || stride < row ||
        !checkedMul(height - 1, stride, offset) || !checkedAdd(offset, row, required) ||
        required > bytes || required > std::numeric_limits<std::size_t>::max()) {
        return {Status::incompatible, "Invalid RGBA dimensions, stride or extent"};
    }
    return {};
}
Outcome admissionLimit(Memory const &m, std::uint64_t &out) noexcept
{
    if (!m.measured || !m.physical || !m.available || !m.baseline) return {Status::unavailable, "RAM measurements unavailable"};
    constexpr auto recovery = 256 * MiB;
    constexpr auto process = 3072 * MiB;
    if (m.baseline >= process - recovery || m.available <= recovery) return failed("No recovery headroom");
    auto cap = std::min({1536 * MiB, m.physical / 4,
                         process - recovery - m.baseline, m.available - recovery});
    if (!cap) return failed("No operation headroom");
    out = cap;
    return changed();
}
void Budget::lock() const noexcept
{
#if defined(__APPLE__)
    os_unfair_lock_lock(&_lock);
#else
    _lock.lock();
#endif
}
void Budget::unlock() const noexcept
{
#if defined(__APPLE__)
    os_unfair_lock_unlock(&_lock);
#else
    _lock.unlock();
#endif
}
Outcome Budget::acquire(Stage stage, std::uint64_t bytes, Token &out) noexcept
{
    if (out) return failed("Reservation output already owns a token");
    return replace(out, stage, bytes);
}
Outcome Budget::replace(Token &token, Stage stage, std::uint64_t bytes) noexcept
{
    if (!valid(stage)) return failed("Invalid reservation stage");
    Guard guard(*this);
    auto old = token._bytes;
    auto index = static_cast<unsigned>(stage);
    auto stageBase = _stages[index] - (token._owner && token._stage == stage ? old : 0);
    std::uint64_t total, subtotal;
    bool growth = !token._owner || bytes > old;
    if (!checkedAdd(_total - old, bytes, total) || !checkedAdd(stageBase, bytes, subtotal))
        return failed("Reservation arithmetic overflow");
    // Retirement/shrink/transfer never needs renewed admission. Transfers move existing
    // storage; callers must acquire a fresh token before allocating in a capped stage.
    if (growth && (!_admitted || total > _limit || subtotal > stageLimit(stage)))
        return failed("Reservation exceeds current operation/stage budget");
    if (growth && (stage == Stage::crop || stage == Stage::encoder)) {
        std::uint64_t combined = _stages[static_cast<unsigned>(Stage::crop)] +
                                 _stages[static_cast<unsigned>(Stage::encoder)];
        if (token._owner && (token._stage == Stage::crop || token._stage == Stage::encoder)) combined -= old;
        if (!checkedAdd(combined, bytes, combined) || combined > 128 * MiB)
            return failed("Combined crop/encoder scratch cap exceeded");
    }
    if (token._owner) _stages[static_cast<unsigned>(token._stage)] -= old;
    _stages[index] = subtotal;
    _total = total;
    token._owner = this;
    token._stage = stage;
    token._bytes = bytes;
    return changed();
}
void Budget::release(Token &token) noexcept
{
    Guard guard(*this);
    _total -= token._bytes;
    _stages[static_cast<unsigned>(token._stage)] -= token._bytes;
    token._owner = nullptr;
    token._bytes = 0;
}
Outcome Budget::recheck(Memory const &memory) noexcept
{
    Guard guard(*this);
    Memory effective = memory;
    std::uint64_t cap = 0, recovered;
    Outcome outcome;
    if (!memory.measured || !memory.baseline || !memory.available)
        outcome = {Status::unavailable, "RAM measurements unavailable"};
    else if (memory.resident > _total || memory.resident >= memory.baseline ||
             !checkedAdd(memory.available, memory.resident, recovered) || recovered > memory.physical)
        outcome = failed("Invalid resident RAM measurement");
    else {
        effective.available = _start.measured ? std::min(_start.available, recovered) : recovered;
        effective.baseline = _start.measured ? std::max(_start.baseline, memory.baseline - memory.resident)
                                           : memory.baseline - memory.resident;
        outcome = admissionLimit(effective, cap);
        if (outcome.ok()) {
            if (!_start.measured) _start = effective;
            _limit = std::min(_ceiling, cap);
        }
    }
    _admitted = outcome.ok() && _total <= _limit;
    if (!outcome.ok()) return outcome; // retain last known cap for refusal diagnostics
    return _admitted ? changed() : failed("Live reservations exceed refreshed headroom");
}
std::uint64_t Budget::reserved() const noexcept { Guard guard(*this); return _total; }
std::uint64_t Budget::limit() const noexcept { Guard guard(*this); return _limit; }
std::uint64_t Budget::reserved(Stage s) const noexcept
{
    Guard guard(*this);
    return valid(s) ? _stages[static_cast<unsigned>(s)] : 0;
}
Budget::Token::Token(Token &&other) noexcept { *this = std::move(other); }
Budget::Token &Budget::Token::operator=(Token &&other) noexcept
{
    if (this != &other) {
        release();
        _owner = std::exchange(other._owner, nullptr);
        _stage = other._stage;
        _bytes = std::exchange(other._bytes, 0);
    }
    return *this;
}
void Budget::Token::release() noexcept { if (_owner) _owner->release(*this); }
Outcome Budget::Token::resize(Stage s, std::uint64_t n) noexcept
{
    return _owner ? _owner->replace(*this, s, n) : failed("No reservation token");
}
Outcome Budget::Token::split(std::uint64_t bytes, Token &out) noexcept
{
    if (!_owner || &out == this || out || bytes > _bytes) return failed("Invalid token split");
    Guard guard(*_owner);
    out._owner = _owner; out._stage = _stage; out._bytes = bytes;
    _bytes -= bytes; // total and stage charge remain unchanged
    return changed();
}
JobWork::JobWork(std::uint64_t pixels) noexcept
{
    std::uint64_t scan;
    if (pixels <= 100000000 && checkedMul(pixels, 8, scan)) checkedAdd(scan, 100000000, _limit);
}
JobWork::JobWork(std::uint64_t pixels, std::uint64_t perPixel) noexcept
{
    std::uint64_t scan;
    if (pixels <= 100000000 && checkedMul(pixels, perPixel, scan)) checkedAdd(scan, 100000000, _limit);
}
Outcome JobWork::advance(std::uint64_t n) noexcept
{
    auto old = _visits.load(std::memory_order_relaxed);
    for (;;) {
        std::uint64_t next;
        if (!checkedAdd(old, n, next)) return failed("Job visit counter overflow");
        if (_visits.compare_exchange_weak(old, next, std::memory_order_relaxed))
            return _limit && next <= _limit ? changed() : failed("Job visit limit");
    }
}
Outcome PhaseTimer::check(Stop stop, Clock::time_point now) const noexcept
{
    if (stop.requested()) return {Status::canceled, "Stop requested"};
    if (now - _start >= std::chrono::seconds(30)) return failed("Worker phase time limit");
    return {};
}
bool AllocationFault::fail() noexcept
{
    if (attempts == std::numeric_limits<std::uint64_t>::max()) return true;
    ++attempts;
    return failAt && attempts == failAt;
}
PlainBuffer::PlainBuffer(PlainBuffer &&other) noexcept { *this = std::move(other); }
PlainBuffer &PlainBuffer::operator=(PlainBuffer &&other) noexcept
{
    if (this != &other) {
        reset();
        _token = std::move(other._token);
        _data = std::exchange(other._data, nullptr);
        _size = std::exchange(other._size, 0);
    }
    return *this;
}
Outcome PlainBuffer::allocate(Budget &budget, Stage stage, std::uint64_t count,
                             std::uint64_t elementSize, AllocationFault *fault, Stop stop) noexcept
{
    if (_data || _token) return failed("Buffer is not empty");
    if (stop.requested()) return {Status::canceled, "Stop requested"};
    std::uint64_t bytes;
    std::size_t size;
    if (!valid(stage) || !checkedMul(count, elementSize, bytes) || !checkedSize(bytes, size)) return failed("Buffer size overflow or invalid stage");
    if (!size) return {};
    Budget::Token token;
    auto result = budget.acquire(stage, bytes, token);
    if (!result.ok()) return result;
    auto data = fault && fault->fail() ? nullptr : std::malloc(size);
    if (!data) return failed("Plain buffer allocation failed");
    if (stop.requested()) {
        std::free(data);
        return {Status::canceled, "Stop requested"};
    }
    _data = static_cast<std::byte *>(data);
    _size = size;
    _token = std::move(token);
    return changed();
}
Outcome PlainBuffer::grow(Budget &budget, Stage stage, std::uint64_t count,
                          std::uint64_t elementSize, AllocationFault *fault, Stop stop) noexcept
{
    std::uint64_t bytes;
    if (!checkedMul(count, elementSize, bytes)) return failed("Buffer growth overflow");
    if (bytes <= _size) return {Status::unchanged, ""};
    PlainBuffer replacement;
    auto outcome = replacement.allocate(budget, stage, count, elementSize, fault, stop);
    if (!outcome.ok()) return outcome;
    // Copy in bounded chunks so cancellation cannot strand a large overlap allocation.
    for (std::size_t offset = 0; offset < _size;) {
        if (stop.requested()) return {Status::canceled, "Stop requested"};
        auto n = std::min<std::size_t>(65536, _size - offset);
        std::memcpy(replacement.data() + offset, _data + offset, n); offset += n;
    }
    *this = std::move(replacement); // free old storage, then release its reservation
    return changed();
}
void PlainBuffer::reset() noexcept
{
    std::free(_data);
    _data = nullptr;
    _size = 0;
    _token.release();
}
} // namespace Inkscape::Bitmap
