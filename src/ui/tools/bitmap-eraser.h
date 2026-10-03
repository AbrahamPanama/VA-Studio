// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_BITMAP_ERASER_H
#define INKSCAPE_UI_TOOLS_BITMAP_ERASER_H

#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <2geom/pathvector.h>

#include "display/drawing-image.h"

class SPDesktop;
class SPImage;

namespace Inkscape {
class Pixbuf;
}

namespace Inkscape::UI::Tools {

struct BitmapBrushStamp
{
    Geom::Point center_desktop;
    double diameter = 1.0;
    double hardness = 1.0;
    double opacity = 1.0;
    Inkscape::DrawingImageEraseShape shape = Inkscape::DrawingImageEraseShape::Round;
};

/**
 * Erase the area covered by @p pixel_path from @p pixbuf.
 *
 * The pixbuf is converted to Cairo's premultiplied ARGB representation and
 * painted with CAIRO_OPERATOR_DEST_OUT. The function returns true only when
 * at least one pixel changed.
 */
bool erase_bitmap_path(Inkscape::Pixbuf &pixbuf, Geom::PathVector const &pixel_path);

/** Apply raster brush stamps expressed in intrinsic image-pixel coordinates. */
bool erase_bitmap_stamps(Inkscape::Pixbuf &pixbuf,
                         std::span<Inkscape::DrawingImageEraseStamp const> stamps);

/** Compare the visible pixel bytes of two equally sized Pixbufs. */
bool bitmap_pixels_equal(Inkscape::Pixbuf const &a, Inkscape::Pixbuf const &b);

/**
 * One transient, desktop-local bitmap eraser transaction.
 *
 * Preview buffers are installed only in the initiating desktop's drawing
 * tree. XML and canonical SPImage pixbufs remain untouched until commit().
 */
class BitmapEraseSession final
{
public:
    explicit BitmapEraseSession(SPDesktop *desktop);
    ~BitmapEraseSession();

    BitmapEraseSession(BitmapEraseSession const &) = delete;
    BitmapEraseSession &operator=(BitmapEraseSession const &) = delete;

    bool begin(std::vector<SPImage *> const &images);
    bool preview(Geom::PathVector const &stroke_desktop);
    bool commit(Geom::PathVector const &stroke_desktop);

    /** Append direct pointer samples to the transient raster-brush preview. */
    bool preview(std::span<BitmapBrushStamp const> stamps);

    /** Commit all raster-brush stamps accumulated through preview(). */
    bool commit();
    void cancel() noexcept;

    bool active() const { return !_targets.empty(); }
    std::size_t targetCount() const { return _targets.size(); }

    /**
     * The live raster-brush preview replays its un-baked chunks on every
     * render, so after this many chunks they are baked into a working pixbuf
     * (published through the same deferred path as the vector preview) and
     * the chain restarts. This bounds render cost and chain length.
     */
    static constexpr std::size_t kDefaultBakeChunkLimit = 128;
    /// Largest automatic bake interval (chunks); bounds render cost per frame.
    static constexpr std::size_t kMaxAutoBakeChunkLimit = 4096;

    /**
     * Automatic bake interval for a bitmap: a bake copies or re-renders the
     * whole bitmap on the main thread, so large images bake less often
     * (max(128, pixels / 20000), capped at kMaxAutoBakeChunkLimit).
     */
    static std::size_t bakeLimitFor(int width, int height);

    /** Longest live preview chain over all targets (chunks not yet baked). */
    std::size_t previewChainLength() const;
    /** Test hook: force the chunk count at which the chain is baked (default: automatic). */
    void setBakeChunkLimit(std::size_t limit) { _bake_chunk_limit = limit ? limit : 1; }
    /** Test hook: pixels the view shows for target @p index (baked pixbuf plus un-baked stamps). */
    std::shared_ptr<Inkscape::Pixbuf> effectivePreview(std::size_t index) const;

private:
    struct Target;

    Geom::PathVector toPixels(Target const &target, Geom::PathVector const &stroke_desktop) const;
    std::optional<Inkscape::DrawingImageEraseStamp> toPixels(Target const &target,
                                                             BitmapBrushStamp const &stamp) const;
    void restoreCanonical() noexcept;

    enum class BakeResult { Done, OutOfMemory, Failed };
    std::size_t bakeLimitFor(Target const &target) const;
    BakeResult bakeChain(Target &target, SPImage &image);
    void reportOutOfMemory() const;
    void reportEncodeFailure() const;

    bool previewPathImpl(Geom::PathVector const &stroke_desktop);
    bool previewStampsImpl(std::span<BitmapBrushStamp const> stamps);
    bool commitPathImpl(Geom::PathVector const &stroke_desktop);
    bool commitStampsImpl();

    SPDesktop *_desktop = nullptr;
    std::vector<std::unique_ptr<Target>> _targets;
    bool _committing = false;
    std::size_t _bake_chunk_limit = 0; ///< 0 = automatic (bakeLimitFor)
};

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_BITMAP_ERASER_H
