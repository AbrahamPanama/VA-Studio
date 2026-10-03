// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_DISPLAY_PREVIEW_RENDER_BUDGET_H
#define INKSCAPE_DISPLAY_PREVIEW_RENDER_BUDGET_H

#include <2geom/affine.h>
#include <2geom/rect.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Inkscape {

// Opt-in, synchronous owning-thread admission. Inactive for ordinary canvas,
// Symbols, export and print. Not a malloc interceptor or a process-RSS limit.
// Charges are cumulative for a render, never released/reused. Consequently
// repeated filters and pattern tiles cannot bypass the aggregate budget.
class PreviewRenderLimit : public std::runtime_error {
public:
    explicit PreviewRenderLimit(char const *why) : std::runtime_error(why) {}
};
class PreviewRenderCancelled : public std::runtime_error {
public:
    PreviewRenderCancelled() : std::runtime_error("Thumbnail generation cancelled") {}
};
class PreviewRenderBudget final {
public:
    struct Limits {
        std::size_t bytes = 256u * 1024 * 1024;
        std::size_t work = 512u * 1024 * 1024;
        unsigned dimension = 8192;
        double kernel = 4096;
    };
    struct Stats {
        std::size_t bytes = 0, work = 0, surfaces = 0, filters = 0;
        unsigned maximum_dimension = 0;
    };
    explicit PreviewRenderBudget(Limits limits, std::function<bool()> cancelled = {})
        : _limits(limits), _cancelled(std::move(cancelled))
    {
        if (_current) throw PreviewRenderLimit("Nested native preview render");
        if (!limits.bytes || limits.bytes > 256u * 1024 * 1024 ||
            !limits.work || limits.work > 512u * 1024 * 1024 ||
            !limits.dimension || limits.dimension > 8192 ||
            !std::isfinite(limits.kernel) || limits.kernel <= 0 || limits.kernel > 4096)
            throw PreviewRenderLimit("Invalid native preview limits");
        _current = this;
    }
    ~PreviewRenderBudget() { _current = nullptr; }
    PreviewRenderBudget(PreviewRenderBudget const &) = delete;
    PreviewRenderBudget &operator=(PreviewRenderBudget const &) = delete;
    static PreviewRenderBudget *current() { return _current; }
    void bind_drawing(void const *drawing) { _drawing = drawing; }
    // Do not impose a thumbnail policy on an unrelated Drawing rendered
    // reentrantly by a native observer. Pure pixel workers do not use TLS.
    class DrawingScope {
    public:
        explicit DrawingScope(void const *drawing) : previous(_current) {
            if (_current && _current->_drawing != drawing) _current = nullptr;
        }
        ~DrawingScope() { _current = previous; }
        DrawingScope(DrawingScope const &) = delete;
    private:
        PreviewRenderBudget *previous;
    };
    Stats stats() const { return _stats; }
    void checkpoint() const {
        if (_cancelled && _cancelled()) throw PreviewRenderCancelled();
    }
    static void point(double x, double y) {
        if (!_current) return;
        // Headroom for integer bbox inflation and addition in existing Drawing.
        if (!std::isfinite(x) || !std::isfinite(y) ||
            std::abs(x) > 1e7 || std::abs(y) > 1e7)
            throw PreviewRenderLimit("Native preview coordinate exceeds integer-safe domain");
    }
    static void affine(Geom::Affine const &a) {
        if (!_current) return;
        for (unsigned i = 0; i < 6; i += 2) point(a[i], a[i + 1]);
        if (!std::isfinite(a.det()) || a.isSingular(1e-18))
            throw PreviewRenderLimit("Native preview transform is singular");
    }
    static void rect(Geom::Rect const &r, bool positive = false) {
        if (!_current) return;
        point(r.min()[0], r.min()[1]); point(r.max()[0], r.max()[1]);
        if (positive && (r.width() <= 0 || r.height() <= 0))
            throw PreviewRenderLimit("Degenerate native filter region");
    }
    static void kernel(double x, double y) {
        if (!_current) return;
        if (!std::isfinite(x) || !std::isfinite(y) ||
            std::abs(x) > _current->_limits.kernel || std::abs(y) > _current->_limits.kernel)
            throw PreviewRenderLimit("Native preview kernel exceeds device-space bound");
    }
    std::size_t pixels(double width, double height, double scale = 1) {
        if (!std::isfinite(scale) || scale <= 0) throw PreviewRenderLimit("Invalid device scale");
        auto w = std::ceil(width * scale), h = std::ceil(height * scale);
        if (!std::isfinite(w) || !std::isfinite(h) || w < 0 || h < 0 ||
            w > _limits.dimension || h > _limits.dimension)
            throw PreviewRenderLimit("Native preview intermediate dimension limit");
        _stats.maximum_dimension = std::max(_stats.maximum_dimension, static_cast<unsigned>(std::max(w, h)));
        return static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    }
    void charge_bytes(std::size_t n) { charge(_stats.bytes, n, _limits.bytes); }
    void charge_work(std::size_t n) { charge(_stats.work, n, _limits.work); }
    static void surface(double w, double h, double scale = 1) {
        if (!_current) return;
        _current->checkpoint();
        auto p = _current->pixels(w, h, scale);
        _current->charge_bytes(p * 4); _current->charge_work(p);
        ++_current->_stats.surfaces;
    }
    void filter_envelope(std::size_t pixels, unsigned primitives, unsigned threads, unsigned longest, double radius) {
        // No callback after the caller has sampled the native pool/parameters.
        if (primitives > 64 || !threads || threads > 1024)
            throw PreviewRenderLimit("Native filter primitive/worker limit");
        // Slot source/alpha/background/result transforms plus <=3 surfaces per
        // admitted primitive. ARGB32 pessimistically covers alpha-only surfaces.
        // Scratch: per-worker IIR rows and morphology deques (generous block/
        // pair overhead), plus fixed FIR/LUT storage. This is an admission
        // envelope, not measured allocation bytes; see ALLOCATION-AUDIT.md.
        auto count = std::size_t(8) + 3 * primitives;
        charge_bytes(multiply(multiply(pixels, 4), count));
        charge_bytes(multiply(multiply(threads, std::size_t(longest) * 128 + 65536), primitives));
        charge_work(multiply(pixels, 32 + std::size_t(primitives) * 256));
        charge_work(multiply(multiply(multiply(longest, static_cast<std::size_t>(std::ceil(radius))), 8), primitives));
        _stats.surfaces += count; ++_stats.filters;
    }
private:
    static std::size_t multiply(std::size_t a, std::size_t b) {
        if (b && a > std::numeric_limits<std::size_t>::max() / b)
            throw PreviewRenderLimit("Native preview accounting overflow");
        return a * b;
    }
    static void charge(std::size_t &used, std::size_t n, std::size_t cap) {
        if (used > cap || n > cap - used) throw PreviewRenderLimit("Native preview aggregate budget");
        used += n;
    }
    inline static thread_local PreviewRenderBudget *_current = nullptr;
    Limits _limits;
    void const *_drawing = nullptr;
    Stats _stats;
    std::function<bool()> _cancelled;
};
} // namespace Inkscape
#endif
