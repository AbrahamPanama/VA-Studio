// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_CACHE_ARTWORK_LIBRARY_THUMBNAIL_H
#define INKSCAPE_UI_CACHE_ARTWORK_LIBRARY_THUMBNAIL_H

#include "io/artwork-library-svg-preflight.h"
#include "display/preview-render-budget.h"
#include <gdkmm/texture.h>
#include <glib.h>
#include <cstdint>
#include <compare>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Inkscape::UI::Cache {

struct ThumbnailSize {
    unsigned width = 128, height = 128; // Logical pixels; each 1..512.
    unsigned device_scale = 1; // Native integer device scale, 1..4.
    auto operator<=>(ThumbnailSize const &) const = default;
};
struct Thumbnail {
    // Immutable owning texture; no native document, surface or mutable bytes
    // escape. Retaining a result past eviction/service destruction is safe.
    Glib::RefPtr<Gdk::Texture> texture;
    std::size_t bytes = 0;
    PreviewRenderBudget::Stats render_stats;
    // Borrowed immutable views owned by texture, not by the pool/service.
    // Keep texture (or this result) alive while using these views.
    std::span<std::string_view const> warnings;
};
enum class ThumbnailFailure { None, Cancelled, Limits, Native };
struct ThumbnailResult {
    std::shared_ptr<Thumbnail const> image;
    ThumbnailFailure failure = ThumbnailFailure::None;
    // Static failure text. A rejected request must not allocate an uncharged
    // retained diagnostic string while the pool is already exhausted.
    char const *diagnostic = "";
};

enum class ThumbnailInvalidation { Close, Search, Resize, DocumentChange, CollectionChange, FontsChanged };
class ArtworkLibraryThumbnailPool final {
public:
    static constexpr std::size_t hard_limit = 128u * 1024 * 1024;
    struct Stats {
        std::size_t capacity = 0, charged = 0, reserved = 0, resident = 0;
        std::size_t externally_pinned = 0, cache_entries = 0, active_jobs = 0;
        // Actual output-backing lifetime observations, not Drawing transients.
        std::size_t pixel_bytes = 0, backings = 0;
        std::size_t backing_owners = 0; // Actual GBytes/Cairo owners, not charge handles.
        std::uint64_t backing_allocations = 0, backing_releases = 0;
        std::uint64_t texture_allocations = 0, texture_releases = 0;
    };
    // Host creates ONE pool on its native owner thread, then injects it into
    // every controller/service. Extra factories are for isolated fixtures;
    // do not create per-panel pools. Destroy the pool wrapper on its owner.
    static std::shared_ptr<ArtworkLibraryThumbnailPool> create(
        GMainContext *owner_context = nullptr, std::size_t capacity = hard_limit);
    ~ArtworkLibraryThumbnailPool();
    ArtworkLibraryThumbnailPool(ArtworkLibraryThumbnailPool const &) = delete;
    ArtworkLibraryThumbnailPool &operator=(ArtworkLibraryThumbnailPool const &) = delete;
    // Thread-safe ledger; charged == reserved + resident at every snapshot.
    // externally_pinned settles with concurrent GObject ref/unref callbacks;
    // it is a subset, never another charge or an early-release authority.
    Stats stats() const;
    void invalidate_all(ThumbnailInvalidation); // Owner thread. Live textures stay charged.
private:
    ArtworkLibraryThumbnailPool() = default;
    struct State;
    std::shared_ptr<State> _state;
    friend class ArtworkLibraryThumbnails;
    friend class ThumbnailAllocation;
};

// Synchronous owning-document-thread primitive. Requires native Application
// initialization by the host. No active desktop or GTK window is needed.
// Never pumps events, changes global rendering preferences, mutates a catalog
// or destination document, or falls back to an unfiltered/flattened source.
// Native primitives may dispatch pure pixel work to the existing worker pool.
// The process-wide gate rejects a nested native render with Limits even if
// an unrelated Drawing has masked the thread-local budget. Input is pinned.
ThumbnailResult render_library_thumbnail(
    std::shared_ptr<ArtworkLibraryThumbnailPool> pool,
    IO::ArtworkLibrary::ValidatedSvg svg, ThumbnailSize size,
    PreviewRenderBudget::Limits limits = {}, std::function<bool()> cancelled = {});

struct ThumbnailDemand {
    IO::ArtworkLibrary::ValidatedSvg svg;
    ThumbnailSize size;
};

// Presentation-independent visible-page service. Construct/use/destroy on the
// native owning thread; this context must be iterated on that same thread.
// set_visible REPLACES demand. Only caller-reported visible cells are serviced;
// there is no full-catalog enumeration or background pre-render.
class ArtworkLibraryThumbnails final {
public:
    using Ready = std::function<void(std::uint64_t generation, std::size_t visible_index,
                                    ThumbnailResult result)>;
    explicit ArtworkLibraryThumbnails(std::shared_ptr<ArtworkLibraryThumbnailPool> pool,
                                      PreviewRenderBudget::Limits render_limits = {});
    ~ArtworkLibraryThumbnails();
    ArtworkLibraryThumbnails(ArtworkLibraryThumbnails const &) = delete;
    ArtworkLibraryThumbnails &operator=(ArtworkLibraryThumbnails const &) = delete;

    // <=128 cells and <=64 MiB of immutable input bytes (conservative sum).
    // Prevalidation is transactional. No callbacks occur synchronously here.
    // Callback may replace demand, close, or destroy the service.
    std::uint64_t set_visible(std::vector<ThumbnailDemand> demand, Ready ready);
    void invalidate(ThumbnailInvalidation reason);
    std::uint64_t generation() const;
    std::size_t cache_bytes() const; // Global pool charged, including externally held textures.
    std::size_t pending_sources() const; // 0 or 1, not number of visible cells.
private:
    struct State;
    std::shared_ptr<State> _state;
};
} // namespace Inkscape::UI::Cache
#endif
