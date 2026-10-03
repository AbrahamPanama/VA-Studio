// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * SVG <image> implementation
 *//*
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Edward Flick (EAF)
 *
 * Copyright (C) 1999-2005 Authors
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_INKSCAPE_SP_IMAGE_H
#define SEEN_INKSCAPE_SP_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <glibmm/ustring.h>
#include <2geom/pathvector.h>

#include "sp-dimensions.h"
#include "sp-item.h"
#include "viewbox.h"

#define SP_IMAGE_HREF_MODIFIED_FLAG SP_OBJECT_USER_MODIFIED_FLAG_A

namespace Inkscape { class Pixbuf; class URI; struct DrawingImageErasePreview; }
namespace Inkscape::Filters { class Filter; }
namespace Inkscape::Bitmap {
struct ViewPixels;
struct ViewDependencyStamp;
enum class SuppressedOwnEffects : std::uint8_t;
using Generation = std::uint64_t;
}
class SPImage final : public SPItem, public SPViewBox, public SPDimensions {
public:
    SPImage();
    ~SPImage() override;
    int tag() const override { return tag_of<decltype(*this)>; }

    Geom::Rect clipbox;
    double sx, sy;
    double ox, oy;
    double dpi;
    double prev_width, prev_height;

    std::optional<Geom::PathVector> curve; // This curve is at the image's boundary for snapping

    char *href;
    char *color_profile;

    std::shared_ptr<Inkscape::Pixbuf const> pixbuf;
    bool missing = true;

    void build(SPDocument *document, Inkscape::XML::Node *repr) override;
    void release() override;
    void set(SPAttr key, char const* value) override;
    void update(SPCtx *ctx, unsigned int flags) override;
    Inkscape::XML::Node* write(Inkscape::XML::Document *xml_doc, Inkscape::XML::Node *repr, unsigned int flags) override;
    void modified(unsigned int flags) override;

    Geom::OptRect bbox(Geom::Affine const &transform, SPItem::BBoxType type) const override;
    void print(SPPrintContext *ctx) override;
    const char* typeName() const override;
    const char* displayName() const override;
    char* description() const override;
    Inkscape::DrawingItem* show(Inkscape::Drawing &drawing, unsigned int key, unsigned int flags) override;
    void snappoints(std::vector<Inkscape::SnapCandidatePoint> &p, Inkscape::SnapPreferences const *snapprefs) const override;
    Geom::Affine set_transform(Geom::Affine const &transform) override;

    /** Transform intrinsic image pixels into document coordinates. */
    std::optional<Geom::Affine> pixelToDocumentAffine() const;

    /** Install a transient pixbuf in one display view without changing XML. */
    bool setViewPixbuf(unsigned display_key, std::shared_ptr<Inkscape::Pixbuf const> preview) const;

    /** Issue a main-thread installation ticket; zero means no display view. */
    Inkscape::Bitmap::Generation composedViewGeneration(unsigned dkey) const;
    /** Read-only main-thread view stamp; no ticket, queue or render effect. Zero if absent/hidden. */
    Inkscape::Bitmap::ViewDependencyStamp viewDependencyStamp(unsigned dkey) const;
    /** Install immutable source-grid pixels, suppressing only baked own effects.
     * A null pixel buffer restores CURRENT committed pixels with no suppression.
     * Tickets retire on refresh, newer preparation, installation or view departure.
     * An optional renderer transfers only on acceptance and publishes with pixels.
     * replace_pixels=false preserves legacy pixels/effects for tone-only preview.
     */
    bool setComposedView(unsigned dkey, Inkscape::Bitmap::ViewPixels const &,
                         Inkscape::Bitmap::SuppressedOwnEffects, Inkscape::Bitmap::Generation,
                         std::unique_ptr<Inkscape::Filters::Filter> *renderer = nullptr,
                         bool replace_pixels = true);

    /** Install a transient destructive-eraser preview in one display view. */
    bool setViewErasePreview(unsigned display_key,
                             std::shared_ptr<Inkscape::DrawingImageErasePreview const> preview) const;

    void apply_profile(Inkscape::Pixbuf *pixbuf);

    Geom::PathVector const *get_curve() const;
    void refresh_if_outdated();
    bool cropToArea(Geom::Rect area);
    bool cropToArea(const Geom::IntRect &area);

    Inkscape::URI getURI() const;
private:
    static Inkscape::Pixbuf *readImage(gchar const *href, gchar const *absref, gchar const *base, double svgdpi = 0);
    static Inkscape::Pixbuf *getBrokenImage(double width, double height);
};

/* Return duplicate of curve or NULL */
void sp_embed_image(Inkscape::XML::Node *imgnode, Inkscape::Pixbuf *pb);
void sp_embed_svg(Inkscape::XML::Node *image_node, std::string const &fn);

/** Encode edited pixels as an embedded PNG without modifying an XML node. */
std::optional<std::string> sp_image_encode_png_data_uri(Inkscape::Pixbuf const &pixbuf);

/**
 * Same, for a disposable pixbuf: it is converted in place (no full copy) and
 * released as soon as the PNG is encoded, before the base64 text is built.
 * Returns nullopt (and logs) on any encode or allocation failure.
 */
std::optional<std::string> sp_image_encode_png_data_uri(std::unique_ptr<Inkscape::Pixbuf> pixbuf);

/**
 * Deep-copy the pixels (and the metadata a re-encoded PNG keeps) of @p source.
 * Unlike the Inkscape::Pixbuf copy constructor this reports allocation failure
 * by returning null instead of building a broken object.
 */
std::unique_ptr<Inkscape::Pixbuf> sp_image_try_copy_pixbuf(Inkscape::Pixbuf const &source);

/** Number of successful sp_image_try_copy_pixbuf() calls so far (test seam). */
std::size_t sp_image_pixbuf_copy_count();

/**
 * Test seam: when set, called with a stage name ("copy", "png" or "base64")
 * before each large allocation of the copy/encode path. Returning false makes
 * that allocation fail as if the system were out of memory.
 */
using SPImageAllocationHook = bool (*)(char const *stage);
void sp_image_set_allocation_hook(SPImageAllocationHook hook);
/// Consults the seam above; callers outside sp-image.cpp use their own stage names (e.g. "apply").
bool sp_image_allocation_allowed(char const *stage);

/** Why the last sp_image_encode_png_data_uri() / sp_image_try_copy_pixbuf() call on this thread failed. */
enum class SPImageEncodeFailure { None, OutOfMemory, Other };
SPImageEncodeFailure sp_image_last_encode_failure();

#endif
