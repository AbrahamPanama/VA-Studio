// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Bounded native whole-drawing preview primitive for the Welcome screen (W4).
 *
 * This primitive is deliberately small and isolated:
 *   - It accepts an already-private, already-admitted SPDocument that the caller
 *     owns and has created on this thread. It never opens a file, never reads a
 *     path or byte buffer, and never creates a document.
 *   - It does not touch any desktop, selection, Undo stack, event loop, GTK
 *     widget or texture. It renders a private Drawing tree and returns an owned
 *     Cairo::ImageSurface.
 *   - Framing uses a conservative native documentVisualBounds() envelope (the
 *     same engine as export-area-drawing). It is not a second SVG geometry
 *     interpreter, and it is not a promise that the envelope equals the exact
 *     painted-minimal region. The bounded classifier admits normal styles,
 *     filters, clips and masks; it deliberately does not try to prove that a
 *     fully-transparent paint server/filter or a per-tspan alpha paints nothing.
 *   - A native PreviewRenderBudget is bound to the private Drawing before any
 *     update/render, so the existing native surface/filter/coordinate accounting
 *     is active. Output allocation is charged before it is created.
 *
 * CAPABILITY BOUNDARY (not a security boundary by itself):
 *   - This is rendering/framing only. It does not accept a path or byte buffer,
 *     does not create and does not load a document, and implements no loader.
 *   - It is NOT a promise of transitive zero I/O or secure input admission.
 *     ensureUpToDate() and native rendering can still trigger document resource
 *     loaders reachable from authored content (for example SPImage::update /
 *     readImage, external use/paint-server/clip/mask/filter references, feImage,
 *     CSS @import / @font-face, ICC color profiles, gzip and XML entities; see
 *     the W0 loader audit). Those loaders are not suppressed here.
 *   - Therefore the caller MUST already be W5's admitted helper/process context
 *     with its deny-by-default resource policy, isolated profile and lifetime
 *     guards, or the one recorded exception below. It is NOT safe to call from
 *     the Welcome UI on a document loaded for previewing, and it is not a
 *     substitute for W5 admission.
 *
 * The returned surface is valid after the document and Drawing are torn down.
 *
 * Recorded exception (THUMB-1, WELCOME_SCREEN_IMPLEMENTATION_PLAN.md section 5):
 * store_document_thumbnail() renders in process the user's own document,
 * already open in the editor with its resources loaded by the editor, right
 * after it was opened from or saved to the file. It admits no new input.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_CACHE_WELCOME_DRAWING_PREVIEW_H
#define INKSCAPE_UI_CACHE_WELCOME_DRAWING_PREVIEW_H

#include "display/preview-render-budget.h"

#include <2geom/rect.h>
#include <cairomm/surface.h>
#include <cstddef>
#include <atomic>
#include <functional>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <memory>
#include <string>

class SPDocument;

namespace Inkscape::UI::Cache {

/** Backdrop baked behind the whole preview, including the letterbox. */
enum class WelcomePreviewBackground {
    White,
    Checkerboard,
    DocumentColor,
};

/** How the requested target was framed. */
enum class WelcomePreviewFraming {
    Drawing,   ///< Native visible drawing envelope was available.
    EmptyPage, ///< No visible drawing; a valid page rectangle was framed.
};

/** Terminal outcome of one request. */
enum class WelcomePreviewStatus {
    Rendered,
    UnsupportedNativeSemantics,
    Cancelled,
    Limits,
    InvalidGeometry,
    NativeFailure,
};

/**
 * Fixed enum reason for a non-rendered result and for the conservative
 * classifier. This is intentionally not a free-form diagnostic channel.
 */
enum class WelcomePreviewLimitation {
    None,
    VisibilityNotVisible,
    ItemOpacityZero,
    ZeroPaintAlpha,
    ShapePaintsNothing,
    NestedViewport,
    ObjectCountLimit,
    MissingGeometry,
    NonFiniteGeometry,
    DegenerateGeometry,
    AllocationLimit,
};

struct WelcomeDrawingPreviewRequest {
    /** Physical output size in device pixels. Each in [1, 2048]. */
    unsigned width = 0;
    unsigned height = 0;
    WelcomePreviewBackground background = WelcomePreviewBackground::White;
    PreviewRenderBudget::Limits limits{};
    /** Optional cooperative cancellation; sampled at native checkpoints. */
    std::function<bool()> cancelled{};
};

struct WelcomeDrawingPreview {
    /** Owned physical pixels; null unless status == Rendered. */
    Cairo::RefPtr<Cairo::ImageSurface> surface;
    unsigned width = 0;
    unsigned height = 0;
    std::size_t stride = 0;
    std::size_t bytes = 0;
    /** Document-coordinate rect used for framing (native bounds or page). */
    Geom::Rect document_bounds;
    WelcomePreviewFraming framing = WelcomePreviewFraming::Drawing;
    WelcomePreviewStatus status = WelcomePreviewStatus::InvalidGeometry;
    WelcomePreviewLimitation limitation = WelcomePreviewLimitation::None;
    PreviewRenderBudget::Stats budget;
};

/** Fixed, stable name for a status. Never null. */
char const *welcomePreviewStatusName(WelcomePreviewStatus status) noexcept;

/** Fixed, stable name for a limitation. Never null. */
char const *welcomePreviewLimitationName(WelcomePreviewLimitation limitation) noexcept;

/**
 * Render an owned preview of \a document at the requested physical size.
 *
 * \a document must be a private, already-admitted document on its owning thread,
 * supplied by W5's admitted helper/process context with its resource policy
 * already applied. This function only frames and renders; it does not admit
 * input and does not suppress transitive resource loaders. The document, its
 * XML, its selection and its Undo stack are never mutated.
 */
WelcomeDrawingPreview renderWelcomeDrawingPreview(SPDocument &document,
                                                  WelcomeDrawingPreviewRequest const &request);

/** Hidden, single-request subprocess entry point. */
int run_welcome_preview_helper(int argc, char const *const *argv);

enum class WelcomePreviewLaunchError { Ok, Timeout, Crashed, Rejected, TooLarge, Failed };
struct WelcomePreviewLaunchResult {
    WelcomePreviewLaunchError error = WelcomePreviewLaunchError::Failed;
    GdkPixbuf *pixbuf = nullptr; // owned reference on Ok
};
using WelcomePreviewCallback = std::function<void(WelcomePreviewLaunchResult)>;
void launch_welcome_preview(std::string path, unsigned width, unsigned height,
                            unsigned deadline_ms, WelcomePreviewCallback callback,
                            std::shared_ptr<std::atomic<bool>> cancelled = {});

// Call on the owning main context after the Recent location probe says Available.
// Callbacks own their returned pixbuf reference and run on that context.
class WelcomeThumbnailService {
public:
    struct Stats { std::size_t memory_bytes, queued, active, renders; };
    explicit WelcomeThumbnailService(std::string cache_directory = {});
    ~WelcomeThumbnailService();
    void request(std::string path, unsigned width, unsigned height, WelcomePreviewCallback callback);
    void cancel(std::string const &path, unsigned width, unsigned height);
    Stats stats() const;
private:
    struct State;
    std::shared_ptr<State> _state;
};

/** Welcome card thumbnail size in logical pixels; requests multiply it by the scale factor. */
constexpr unsigned welcome_card_width = 150;
constexpr unsigned welcome_card_height = 113;

/**
 * Store the Welcome thumbnails of \a path at 1x and 2x card size, rendered from
 * the open \a document, into the cache WelcomeThumbnailService reads. This covers
 * files the helper cannot render (over its size cap, linked images, deadline).
 *
 * Call on the main thread right after \a document was saved to \a path or opened
 * from it. Everything runs on the main context: the file's size and
 * modification time are read asynchronously at once and again after
 * \a delay_ms, and the document is rendered only if both reads agree and it
 * still exists, its filename is \a path, it is not modified since save and
 * its XML did not change during the second read. Framing and background are
 * the helper's; blank documents are left to the helper. The render is bounded
 * to about 1 s; a document that exceeds it or the render budget is not retried
 * while it stays open. Entries already cached for this file version are kept.
 * Only .svg/.svgz paths are handled; 1x and 2x only (scale 3 uses the helper).
 *
 * An empty \a cache_directory selects the Welcome cache. When the environment
 * variable VACARDS_WELCOME_THUMBNAIL_TEST_CACHE is set it names the directory
 * instead, or "off" stores nothing (CTest sets "off" for unit tests); with
 * file-I/O test hooks enabled and the variable unset nothing is stored.
 * \a done, if given, runs once on the main context with whether both
 * thumbnails are now cached.
 *
 * \a finder_icon (VIEW-1, macOS only; ignored elsewhere): also set a 512 px
 * rendering, framed the same way, as the file's Finder custom icon. Only the
 * save path passes it: opening a file never writes to it. The file's bytes
 * and modification time are kept.
 */
void store_document_thumbnail(SPDocument &document, std::string path, unsigned delay_ms = 0,
                              std::string cache_directory = {}, std::function<void(bool)> done = {},
                              bool finder_icon = false);

} // namespace Inkscape::UI::Cache

#endif // INKSCAPE_UI_CACHE_WELCOME_DRAWING_PREVIEW_H
