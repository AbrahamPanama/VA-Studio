// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Bitmap image belonging to an SVG drawing.
 *//*
 * Authors:
 *   Krzysztof Kosiński <tweenk.pl@gmail.com>
 *
 * Copyright (C) 2011 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_DISPLAY_DRAWING_IMAGE_H
#define INKSCAPE_DISPLAY_DRAWING_IMAGE_H

#include <memory>
#include <vector>
#include <2geom/transforms.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include <cstdint>
#include <sigc++/signal.h>

#include "display/drawing-item.h"

namespace Inkscape {
class Pixbuf;

namespace Bitmap {
using Generation = std::uint64_t;
// Tone denotes the qualified own tone filter, never an ancestor's renderer.
enum class SuppressedOwnEffects : std::uint8_t { None = 0, Tone = 1, Clip = 2, Opacity = 4 };
constexpr SuppressedOwnEffects operator|(SuppressedOwnEffects a, SuppressedOwnEffects b)
{
    return static_cast<SuppressedOwnEffects>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
struct ViewPixels {
    std::shared_ptr<Pixbuf const> pixels;
    std::weak_ptr<Pixbuf const> source;
    unsigned view_key = 0; // DrawingItem auxiliary incarnation, not display key
};
// Read-only observation of one view's bitmap state. incarnation is unique per DrawingImage
// (never its address); revision moves on every observable bitmap-state change. {0, 0} means
// unavailable (absent/hidden view or counter exhaustion); counters never wrap.
struct ViewDependencyStamp {
    std::uint64_t incarnation = 0, revision = 0;
    friend bool operator==(ViewDependencyStamp const &, ViewDependencyStamp const &) = default;
    bool available() const { return incarnation != 0; }
};
}

enum class DrawingImageEraseShape
{
    Round,
    Square
};

/**
 * One immutable, view-local bitmap eraser stamp.
 *
 * The unit brush occupies [-1, 1] in both axes. unit_to_pixel maps that
 * brush into the intrinsic pixel coordinates of the image. Keeping the
 * preview in intrinsic coordinates lets DrawingImage render it without
 * changing the canonical pixbuf or the SVG document.
 */
struct DrawingImageEraseStamp
{
    Geom::Affine unit_to_pixel;
    DrawingImageEraseShape shape = DrawingImageEraseShape::Round;
    double hardness = 1.0;
    double opacity = 1.0;
};

/**
 * Persistent preview chain. Each pointer is immutable after publication, so
 * DrawingImage render workers can read it safely while the UI thread appends
 * a new chunk without copying the complete stroke or bitmap.
 */
struct DrawingImageErasePreview
{
    std::shared_ptr<DrawingImageErasePreview const> previous;
    std::vector<DrawingImageEraseStamp> stamps;

    DrawingImageErasePreview() = default;
    DrawingImageErasePreview(DrawingImageErasePreview const &) = delete;
    DrawingImageErasePreview &operator=(DrawingImageErasePreview const &) = delete;

    /// Releases a long chain iteratively; the default member-wise destructor
    /// would recurse once per chunk and can overflow the stack.
    ~DrawingImageErasePreview();
};

class DrawingImage
    : public DrawingItem
{
public:
    DrawingImage(Drawing &drawing);
    int tag() const override { return tag_of<decltype(*this)>; }

    void setStyle(SPStyle const *style, SPStyle const *context_style = nullptr) override;

    void setPixbuf(std::shared_ptr<Inkscape::Pixbuf const> pb);
    void setCanonicalPixbuf(std::shared_ptr<Pixbuf const>);
    Bitmap::Generation composedGeneration(); // main-only; retires an earlier ticket
    /// Main-thread, read-only: never issues/retires a ticket or touches the queue.
    Bitmap::ViewDependencyStamp dependencyStamp() const;
    /// Main-thread notification on every revision event, including saturation.
    sigc::connection connectDependencyChanged(sigc::slot<void()> const &slot)
    {
        return _dependency_changed.connect(slot);
    }
    bool setComposedPixels(std::shared_ptr<Pixbuf const>, Bitmap::SuppressedOwnEffects, Bitmap::Generation,
                           std::unique_ptr<Filters::Filter> *renderer, bool replace_pixels);
    bool suppressesOwnEffect(Bitmap::SuppressedOwnEffects effect) const;
    void setErasePreview(std::shared_ptr<DrawingImageErasePreview const> preview);
    void setScale(double sx, double sy);
    void setOrigin(Geom::Point const &o);
    void setClipbox(Geom::Rect const &box);
    void setExtend(cairo_extend_t extend);
    Geom::Rect bounds() const;

protected:
    ~DrawingImage() override = default;

    unsigned _updateItem(Geom::IntRect const &area, UpdateContext const &ctx, unsigned flags, unsigned reset) override;
    unsigned _renderItem(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &area, unsigned flags, DrawingItem const *stop_at) const override;
    DrawingItem *_pickItem(Geom::Point const &p, double delta, Geom::OptIntRect const &area_world, unsigned flags) override;

    std::shared_ptr<Inkscape::Pixbuf const> _pixbuf;
    std::shared_ptr<DrawingImageErasePreview const> _erase_preview;

    // Pixel and suppression publication is one deferred drawing mutation.
    Bitmap::SuppressedOwnEffects _suppressed = Bitmap::SuppressedOwnEffects::None;
    // Ticket fields are main-thread only, never inspected by render workers.
    Bitmap::Generation _generation = 0, _pending_generation = 0, _accepted_generation = 0;
    struct PendingInstallation;
    std::shared_ptr<PendingInstallation> _installation; // main-only coalesced publication slot
    std::uint64_t _incarnation = 0, _revision = 0; // main-only dependency stamp state
    void _bumpRevision(); // saturates at UINT64_MAX, which reports unavailable

    SPImageRendering style_image_rendering;

    // TODO: the following three should probably be merged into a new Geom::Viewbox object
    Geom::Rect _clipbox; ///< for preserveAspectRatio
    Geom::Point _origin;
    Geom::Scale _scale;
    cairo_extend_t _extend;
private:
    sigc::signal<void()> _dependency_changed;
};

} // namespace Inkscape

#endif // INKSCAPE_DISPLAY_DRAWING_IMAGE_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
