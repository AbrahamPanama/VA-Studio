// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Outcome tests for the Welcome whole-drawing preview primitive (W4).
 *
 * Expectations here are derived by hand from the fixture geometry and the
 * documented 5%-per-side containment framing. They do not call the production
 * framing code to compute the oracle. Pixel checks use exact colors on interior
 * pixels; edges are never asserted because raster antialiasing is not part of
 * the contract.
 *
 * Native documents are created in memory from synthetic fixtures. No path or
 * file is ever passed to the production primitive.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/cache/welcome-drawing-preview.h"

#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "io/stream/bufferstream.h"
#include "object/object-set.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "page-manager.h"
#include "xml/attribute-record.h"
#include "xml/node.h"

#include <2geom/rect.h>
#include <cairomm/surface.h>
#include <glib.h>
#include <gtest/gtest.h>
#include <gtkmm/application.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

#ifndef WELCOME_DRAWING_PREVIEW_FIXTURES
#define WELCOME_DRAWING_PREVIEW_FIXTURES "testfiles/data/welcome-drawing-preview"
#endif

using namespace Inkscape;
using namespace Inkscape::UI::Cache;

namespace {

struct Rgba {
    unsigned char r = 0, g = 0, b = 0, a = 0;
};

std::string fixture(std::string const &name)
{
    std::string const path = std::string(WELCOME_DRAWING_PREVIEW_FIXTURES) + "/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read preview fixture: " + path);
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::unique_ptr<SPDocument> load(std::string const &text)
{
    return SPDocument::createNewDocFromMem(std::span<char const>(text.data(), text.size()));
}

std::string checksum(std::string const &data)
{
    gchar *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                             reinterpret_cast<guchar const *>(data.data()),
                                             data.size());
    std::string result(sum ? sum : "");
    g_free(sum);
    return result;
}

std::string xml_identity(XML::Node const *node)
{
    std::string out;
    auto field = [&](char const *value) {
        std::string const s(value ? value : "");
        out += std::to_string(s.size()) + ":" + s;
    };
    field(node->name());
    field(node->content());
    for (auto const &a : node->attributeList()) {
        field(g_quark_to_string(a.key));
        field(static_cast<char const *>(a.value));
    }
    out += "[";
    for (auto c = node->firstChild(); c; c = c->next()) {
        out += xml_identity(c);
    }
    return out + "]";
}

Rgba pixel(Cairo::RefPtr<Cairo::ImageSurface> const &surface, int x, int y)
{
    unsigned char const *row =
        surface->get_data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(surface->get_stride());
    unsigned char const *p = row + static_cast<std::size_t>(x) * 4;
    // Cairo ARGB32 memory order on little-endian is B,G,R,A.
    return Rgba{p[2], p[1], p[0], p[3]};
}

bool near_channel(unsigned char value, unsigned char expected, int tolerance = 8)
{
    return std::abs(static_cast<int>(value) - static_cast<int>(expected)) <= tolerance;
}

bool is_rgb(Rgba const &c, unsigned char r, unsigned char g, unsigned char b, int tolerance = 8)
{
    return near_channel(c.r, r, tolerance) && near_channel(c.g, g, tolerance) &&
           near_channel(c.b, b, tolerance);
}

bool is_white(Rgba const &c)
{
    return is_rgb(c, 255, 255, 255);
}

bool all_opaque(Cairo::RefPtr<Cairo::ImageSurface> const &surface)
{
    for (int y = 0; y < surface->get_height(); ++y) {
        for (int x = 0; x < surface->get_width(); ++x) {
            if (pixel(surface, x, y).a != 255) {
                return false;
            }
        }
    }
    return true;
}

bool all_white(Cairo::RefPtr<Cairo::ImageSurface> const &surface)
{
    for (int y = 0; y < surface->get_height(); ++y) {
        for (int x = 0; x < surface->get_width(); ++x) {
            if (!is_white(pixel(surface, x, y))) {
                return false;
            }
        }
    }
    return true;
}

bool contains_rgb(Cairo::RefPtr<Cairo::ImageSurface> const &surface, unsigned char r, unsigned char g,
                  unsigned char b)
{
    for (int y = 0; y < surface->get_height(); ++y) {
        for (int x = 0; x < surface->get_width(); ++x) {
            if (is_rgb(pixel(surface, x, y), r, g, b)) {
                return true;
            }
        }
    }
    return false;
}

WelcomeDrawingPreview render(SPDocument &doc, unsigned width = 200, unsigned height = 150,
                             WelcomePreviewBackground background = WelcomePreviewBackground::White)
{
    WelcomeDrawingPreviewRequest request;
    request.width = width;
    request.height = height;
    request.background = background;
    return renderWelcomeDrawingPreview(doc, request);
}

class WelcomeDrawingPreviewNative : public ::testing::Test {
protected:
    void SetUp() override
    {
        IO::enable_file_io_test_hooks();
        // Native Application initialization only; no window, desktop or display is required.
        static auto gtk = Gtk::Application::create("org.inkscape.vacards.welcomepreviewtest",
                                                   Gio::Application::Flags::NON_UNIQUE);
        if (!Application::exists()) {
            Application::create(false);
        }
    }
};

} // namespace

TEST_F(WelcomeDrawingPreviewNative, OffPageUnionFramesBothLimitingRectangles)
{
    auto doc = load(fixture("offpage.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 200, 150);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);

    // Union (-40,-30)..(140,70): w=180, h=100, s=min(180/180,135/100)=1, so the
    // bounds centre (50,20) maps exactly to (100,75) and translation is (50,55).
    EXPECT_NEAR(result.document_bounds.min()[Geom::X], -40.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.min()[Geom::Y], -30.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::X], 140.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::Y], 70.0, 1e-6);
    EXPECT_EQ(result.framing, WelcomePreviewFraming::Drawing);
    EXPECT_GT(result.budget.surfaces, 0u);

    // Red rect doc(-40,-30) maps to (10,25); blue rect doc(120,60) maps to (170,115).
    EXPECT_TRUE(is_rgb(pixel(result.surface, 14, 35), 255, 0, 0));
    EXPECT_TRUE(is_rgb(pixel(result.surface, 180, 120), 0, 0, 255));
    // Width is the limiting axis: exactly 10px white letterbox on left/right.
    EXPECT_TRUE(is_white(pixel(result.surface, 2, 35)));
    EXPECT_TRUE(is_white(pixel(result.surface, 5, 35)));
    EXPECT_TRUE(is_white(pixel(result.surface, 197, 120)));
    // Vertical padding is larger here (height is not limiting).
    EXPECT_TRUE(is_white(pixel(result.surface, 100, 2)));
    EXPECT_TRUE(all_opaque(result.surface));
}

TEST_F(WelcomeDrawingPreviewNative, OffPageUnionHalfSizePixelsFollowScaleThenTranslate)
{
    auto doc = load(fixture("offpage.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 100, 75);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);

    // s=0.5, T=(25,27.5); scale-then-translate maps union min/centre/max to
    // (5,12.5)/(50,37.5)/(95,62.5). Translate-then-scale maps p -> 0.5p +
    // (12.5,13.75), so this non-unit target is a transform-order regression.
    // Red source [-40,-30]x[-30,-10] -> dev [5,10]x[12.5,22.5]; interior (7,18).
    EXPECT_TRUE(is_rgb(pixel(result.surface, 7, 18), 255, 0, 0));
    // Blue source [120,140]x[60,70] -> dev [85,95]x[57.5,62.5]; interior (90,60).
    EXPECT_TRUE(is_rgb(pixel(result.surface, 90, 60), 0, 0, 255));
    // Left letterbox padding at centre height stays opaque white.
    EXPECT_TRUE(is_white(pixel(result.surface, 1, 37)));
    EXPECT_TRUE(all_opaque(result.surface));
}

TEST_F(WelcomeDrawingPreviewNative, OffPageUnionDoubleSizePixelsFollowScaleThenTranslate)
{
    auto doc = load(fixture("offpage.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 400, 300);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);

    // s=2, T=(100,110); scale-then-translate maps union min/centre/max to
    // (20,50)/(200,150)/(380,250). Translate-then-scale maps p -> 2p +
    // (200,220) and wrongly inflates/offsets the content.
    // Red source [-40,-30]x[-30,-10] -> dev [20,40]x[50,90]; interior (28,70).
    EXPECT_TRUE(is_rgb(pixel(result.surface, 28, 70), 255, 0, 0));
    // Blue source [120,140]x[60,70] -> dev [340,380]x[230,250]; interior (360,240).
    EXPECT_TRUE(is_rgb(pixel(result.surface, 360, 240), 0, 0, 255));
    // Left letterbox padding at centre height stays opaque white.
    EXPECT_TRUE(is_white(pixel(result.surface, 5, 150)));
    EXPECT_TRUE(all_opaque(result.surface));
}

TEST_F(WelcomeDrawingPreviewNative, NegativeOnlyRectangleCentersWithoutPageClip)
{
    auto doc = load(fixture("negative-only.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 100, 100);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    EXPECT_NEAR(result.document_bounds.min()[Geom::X], -60.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.min()[Geom::Y], -40.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::X], -40.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::Y], -20.0, 1e-6);
    // s=4.5, rect maps to (5,5)..(95,95); the negative-origin centre is honoured.
    EXPECT_TRUE(is_rgb(pixel(result.surface, 50, 50), 0, 255, 0));
    EXPECT_TRUE(is_white(pixel(result.surface, 2, 2)));
    EXPECT_TRUE(is_white(pixel(result.surface, 97, 97)));
}

TEST_F(WelcomeDrawingPreviewNative, DistantSecondObjectIsNotCropped)
{
    auto doc = load(fixture("distant.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 200, 150);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    EXPECT_GT(result.document_bounds.width(), 500.0);
    EXPECT_GT(result.document_bounds.height(), 300.0);
    EXPECT_TRUE(contains_rgb(result.surface, 255, 0, 0));
    EXPECT_TRUE(contains_rgb(result.surface, 0, 0, 255));
}

TEST_F(WelcomeDrawingPreviewNative, ThickStrokeExpandsNativeBounds)
{
    auto doc = load(fixture("thick-stroke.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 100, 100);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    // rect 20..60 with stroke-width 10 gives visible bounds 15..65 on both axes.
    EXPECT_NEAR(result.document_bounds.min()[Geom::X], 15.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.min()[Geom::Y], 15.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::X], 65.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::Y], 65.0, 1e-6);
    // s=1.8: stroke occupies dev 5..23, fill interior at centre.
    EXPECT_TRUE(is_rgb(pixel(result.surface, 10, 50), 0, 0, 0, 20));
    EXPECT_TRUE(is_rgb(pixel(result.surface, 50, 50), 0, 255, 0));
}

TEST_F(WelcomeDrawingPreviewNative, ViewBoxAndTransformedGroupComposeInDocumentCoordinates)
{
    auto doc = load(fixture("viewbox-transform.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 200, 200);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    // viewBox 100 -> 200 (scale 2) then group translate(10 10): rect 10..30 becomes doc 40..80.
    EXPECT_NEAR(result.document_bounds.min()[Geom::X], 40.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.min()[Geom::Y], 40.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::X], 80.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::Y], 80.0, 1e-6);
    EXPECT_TRUE(is_rgb(pixel(result.surface, 100, 100), 0, 255, 0));
    EXPECT_TRUE(is_white(pixel(result.surface, 5, 5)));
}

TEST_F(WelcomeDrawingPreviewNative, ExplicitClipIsPreserved)
{
    auto doc = load(fixture("clip.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 200, 100);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    // green clipped to 10..50, blue at 120..140; union 10..140 (width 130).
    EXPECT_NEAR(result.document_bounds.min()[Geom::X], 10.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::X], 140.0, 1e-6);
    // doc(20,20) green, doc(80,20) was inside the unclipped rect but must be white.
    EXPECT_TRUE(is_rgb(pixel(result.surface, 24, 22), 0, 255, 0));
    EXPECT_TRUE(is_white(pixel(result.surface, 107, 22)));
    // blue unclipped object remains.
    EXPECT_TRUE(is_rgb(pixel(result.surface, 169, 22), 0, 0, 255));
}

TEST_F(WelcomeDrawingPreviewNative, MaskIsPreserved)
{
    auto doc = load(fixture("mask.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 200, 100);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    // Mask geometric extent 0..50 intersected with the 10..110 rect gives 10..50.
    EXPECT_NEAR(result.document_bounds.min()[Geom::X], 10.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::X], 50.0, 1e-6);
    // Bounds 10..50 x 10..70, so s=1.5 and T=(55,-10). Green inside at
    // doc(20,20) -> (85,20); doc(80,20) is outside the mask -> (175,20) white.
    EXPECT_TRUE(is_rgb(pixel(result.surface, 85, 20), 0, 255, 0));
    EXPECT_TRUE(is_white(pixel(result.surface, 175, 20)));
}

TEST_F(WelcomeDrawingPreviewNative, FilterIsAppliedWithNativeAccounting)
{
    auto doc = load(fixture("filter-blur.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 200, 200);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    // The native filter path must have been charged, not silently dropped.
    EXPECT_GE(result.budget.filters, 1u);
    EXPECT_GT(result.budget.bytes, 0u);
    // Blur bleed outside the 80..120 geometric rect: dev ~45 is doc ~75.6.
    // The red/blue channels are pulled below white by the green tint.
    Rgba const bleed = pixel(result.surface, 45, 100);
    EXPECT_LT(bleed.r, 250);
    EXPECT_LT(bleed.b, 250);
    EXPECT_GT(static_cast<int>(bleed.g), static_cast<int>(bleed.r));
    EXPECT_NEAR(bleed.a, 255, 0);
}

TEST_F(WelcomeDrawingPreviewNative, EmptyPageUsesPageFallbackAndOpaqueBackdrop)
{
    auto doc = load(fixture("empty-defs.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 200, 100);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    EXPECT_EQ(result.framing, WelcomePreviewFraming::EmptyPage);
    EXPECT_NEAR(result.document_bounds.width(), 120.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.height(), 80.0, 1e-6);
    // Unused defs never contribute content: the whole target is opaque white.
    EXPECT_TRUE(all_white(result.surface));
}

TEST_F(WelcomeDrawingPreviewNative, NonzeroOriginSelectedPageFallbackUsesDocumentCoordinates)
{
    auto doc = load(fixture("empty-defs.svg"));
    ASSERT_TRUE(doc);
    Geom::Rect const page_rect(Geom::Point(100.0, 50.0), Geom::Point(220.0, 130.0));
    SPPage *page = doc->getPageManager().newDocumentPage(page_rect);
    ASSERT_TRUE(page);
    ASSERT_TRUE(doc->getPageManager().selectPage(page));
    ASSERT_EQ(doc->getPageManager().getSelected(), page);
    auto result = render(*doc, 200, 100);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    EXPECT_EQ(result.framing, WelcomePreviewFraming::EmptyPage);
    // pageBounds()/getDesktopRect() would flip/translate Y; getDocumentRect() must not.
    EXPECT_NEAR(result.document_bounds.min()[Geom::X], 100.0, 1e-3);
    EXPECT_NEAR(result.document_bounds.min()[Geom::Y], 50.0, 1e-3);
}

TEST_F(WelcomeDrawingPreviewNative, PresentDegenerateEnvelopeIsInvalidNotBlank)
{
    auto doc = load(fixture("degenerate-zero-area.svg"));
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    // The native envelope is present but zero-area: this is not an absent/blank
    // document, so the page fallback must not run.
    Geom::OptRect const native = doc->getRoot()->documentVisualBounds();
    ASSERT_TRUE(native);
    EXPECT_TRUE(native->hasZeroArea());
    auto result = render(*doc, 100, 100);
    EXPECT_EQ(result.status, WelcomePreviewStatus::InvalidGeometry);
    EXPECT_EQ(result.limitation, WelcomePreviewLimitation::DegenerateGeometry);
    EXPECT_FALSE(result.surface);
    EXPECT_NE(result.framing, WelcomePreviewFraming::EmptyPage);
}

TEST_F(WelcomeDrawingPreviewNative, InvalidSelectedPageDoesNotPreventFirstValidFallback)
{
    auto doc = load(fixture("empty-defs.svg"));
    ASSERT_TRUE(doc);
    auto &pages = doc->getPageManager();
    Geom::Rect const invalid(Geom::Point(10.0, 10.0), Geom::Point(10.0, 10.0));
    SPPage *bad = pages.newDocumentPage(invalid);
    ASSERT_TRUE(bad);
    ASSERT_TRUE(pages.selectPage(bad));
    ASSERT_EQ(pages.getSelected(), bad);
    // The selected native page must actually be invalid; otherwise the fallback
    // path under test is not exercised. A failing precondition is a readiness
    // concern, not a case to skip.
    ASSERT_TRUE(bad->getDocumentRect().hasZeroArea());

    // Capture the first valid native page rectangle while iterating so the
    // rendered fallback bounds can be compared to that exact expected rect.
    Geom::Rect expected;
    bool have_expected = false;
    for (SPPage *page : pages.getPages()) {
        if (!page) {
            continue;
        }
        Geom::Rect const rect = page->getDocumentRect();
        if (rect.isFinite() && !rect.hasZeroArea()) {
            expected = rect;
            have_expected = true;
            break;
        }
    }
    ASSERT_TRUE(have_expected);
    auto result = render(*doc, 200, 100);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    EXPECT_EQ(result.framing, WelcomePreviewFraming::EmptyPage);
    EXPECT_TRUE(result.document_bounds.isFinite());
    EXPECT_FALSE(result.document_bounds.hasZeroArea());
    EXPECT_NEAR(result.document_bounds.min()[Geom::X], expected.min()[Geom::X], 1e-3);
    EXPECT_NEAR(result.document_bounds.min()[Geom::Y], expected.min()[Geom::Y], 1e-3);
    EXPECT_NEAR(result.document_bounds.max()[Geom::X], expected.max()[Geom::X], 1e-3);
    EXPECT_NEAR(result.document_bounds.max()[Geom::Y], expected.max()[Geom::Y], 1e-3);
}

TEST_F(WelcomeDrawingPreviewNative, BackdropsWhiteCheckerboardAndDocumentColor)
{
    auto white_doc = load(fixture("empty-defs.svg"));
    ASSERT_TRUE(white_doc);
    auto white = render(*white_doc, 120, 80, WelcomePreviewBackground::White);
    ASSERT_EQ(white.status, WelcomePreviewStatus::Rendered);
    ASSERT_TRUE(white.surface);
    EXPECT_TRUE(all_white(white.surface));
    EXPECT_TRUE(all_opaque(white.surface));

    auto checker_doc = load(fixture("empty-defs.svg"));
    ASSERT_TRUE(checker_doc);
    auto checkerboard = render(*checker_doc, 120, 80, WelcomePreviewBackground::Checkerboard);
    ASSERT_EQ(checkerboard.status, WelcomePreviewStatus::Rendered);
    ASSERT_TRUE(checkerboard.surface);
    EXPECT_TRUE(all_opaque(checkerboard.surface));
    // An empty 120x80 page maps to a smaller rect with padding, so the baked
    // checkerboard is visible in the letterbox and is not pure white.
    EXPECT_FALSE(all_white(checkerboard.surface));

    auto color_doc = load(fixture("document-color.svg"));
    ASSERT_TRUE(color_doc);
    // Precondition: the fixture must carry the canonical Sodipodi namespace so
    // the native parser binds the author's namedview. With a wrong namespace the
    // parser synthesizes a default namedview, which would still be non-null but
    // would not carry this fixture's pagecolor/pageopacity.
    XML::Node *namedview = color_doc->getReprNamedView();
    ASSERT_TRUE(namedview);
    ASSERT_STREQ(namedview->attribute("pagecolor"), "#ff0000");
    ASSERT_STREQ(namedview->attribute("inkscape:pageopacity"), "0.5");
    auto document_color = render(*color_doc, 120, 80, WelcomePreviewBackground::DocumentColor);
    ASSERT_EQ(document_color.status, WelcomePreviewStatus::Rendered);
    ASSERT_TRUE(document_color.surface);
    // pagecolor #ff0000 at pageopacity 0.5 composited onto white is (255,127,127).
    Rgba const corner = pixel(document_color.surface, 1, 1);
    EXPECT_NEAR(corner.r, 255, 4);
    EXPECT_NEAR(corner.g, 127, 8);
    EXPECT_NEAR(corner.b, 127, 8);
    EXPECT_EQ(corner.a, 255);
}

TEST_F(WelcomeDrawingPreviewNative, WhiteArtOnCheckerboardShowsGrayLetterbox)
{
    auto doc = load(fixture("white-art-on-page.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 120, 80, WelcomePreviewBackground::Checkerboard);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    EXPECT_TRUE(all_opaque(result.surface));
    // The white artwork paints over the checkerboard at the framed centre.
    EXPECT_TRUE(is_white(pixel(result.surface, 60, 40)));
    // The letterbox keeps the native checkerboard; a gray cell (0xC4C4C4) shows.
    EXPECT_TRUE(contains_rgb(result.surface, 196, 196, 196));
}

TEST_F(WelcomeDrawingPreviewNative, VisibilityHiddenIsUnsupportedNotBlank)
{
    auto doc = load(fixture("visibility-hidden.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 100, 100);
    EXPECT_EQ(result.status, WelcomePreviewStatus::UnsupportedNativeSemantics);
    EXPECT_EQ(result.limitation, WelcomePreviewLimitation::VisibilityNotVisible);
    EXPECT_FALSE(result.surface);
    EXPECT_NE(result.framing, WelcomePreviewFraming::EmptyPage);
}

TEST_F(WelcomeDrawingPreviewNative, ZeroOpacityFarObjectIsUnsupportedNotBlank)
{
    auto doc = load(fixture("opacity-zero.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 100, 100);
    EXPECT_EQ(result.status, WelcomePreviewStatus::UnsupportedNativeSemantics);
    EXPECT_EQ(result.limitation, WelcomePreviewLimitation::ItemOpacityZero);
    EXPECT_FALSE(result.surface);
    EXPECT_NE(result.framing, WelcomePreviewFraming::EmptyPage);
}

TEST_F(WelcomeDrawingPreviewNative, ZeroFillOpacityIsUnsupported)
{
    auto doc = load(fixture("fill-opacity-zero.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 100, 100);
    EXPECT_EQ(result.status, WelcomePreviewStatus::UnsupportedNativeSemantics);
    EXPECT_EQ(result.limitation, WelcomePreviewLimitation::ZeroPaintAlpha);
    EXPECT_FALSE(result.surface);
}

TEST_F(WelcomeDrawingPreviewNative, ZeroStrokeOpacityIsUnsupported)
{
    auto doc = load(fixture("stroke-opacity-zero.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 100, 100);
    EXPECT_EQ(result.status, WelcomePreviewStatus::UnsupportedNativeSemantics);
    EXPECT_EQ(result.limitation, WelcomePreviewLimitation::ZeroPaintAlpha);
    EXPECT_FALSE(result.surface);
}

TEST_F(WelcomeDrawingPreviewNative, TextDirectFillOpacityZeroIsUnsupported)
{
    auto doc = load(fixture("text-fill-opacity-zero.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 120, 80);
    EXPECT_EQ(result.status, WelcomePreviewStatus::UnsupportedNativeSemantics);
    EXPECT_EQ(result.limitation, WelcomePreviewLimitation::ZeroPaintAlpha);
    EXPECT_FALSE(result.surface);
    // No font-dependent pixel oracle is asserted.
}

TEST_F(WelcomeDrawingPreviewNative, VisibleTextIsAdmittedWithoutGeometryOracle)
{
    auto doc = load(fixture("text-visible.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 120, 80);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    EXPECT_TRUE(result.surface);
    // Deliberately no exact text geometry or font-dependent pixel assertion.
}

TEST_F(WelcomeDrawingPreviewNative, NestedViewportIsUnsupported)
{
    auto doc = load(fixture("nested-viewport.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 100, 100);
    EXPECT_EQ(result.status, WelcomePreviewStatus::UnsupportedNativeSemantics);
    EXPECT_EQ(result.limitation, WelcomePreviewLimitation::NestedViewport);
    EXPECT_FALSE(result.surface);
}

TEST_F(WelcomeDrawingPreviewNative, ShapeWithNoPaintIsUnsupported)
{
    auto doc = load(fixture("fill-none-stroke-none.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 100, 100);
    EXPECT_EQ(result.status, WelcomePreviewStatus::UnsupportedNativeSemantics);
    EXPECT_EQ(result.limitation, WelcomePreviewLimitation::ShapePaintsNothing);
    EXPECT_FALSE(result.surface);
}

TEST_F(WelcomeDrawingPreviewNative, DisplayNoneOutlierIsExcludedAndSucceeds)
{
    auto doc = load(fixture("display-none-outlier.svg"));
    ASSERT_TRUE(doc);
    auto result = render(*doc, 100, 100);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);
    // Only the 0..20 visible rect remains; the 900..920 display:none rect is skipped.
    EXPECT_NEAR(result.document_bounds.max()[Geom::X], 20.0, 1e-6);
    EXPECT_NEAR(result.document_bounds.max()[Geom::Y], 20.0, 1e-6);
}

TEST_F(WelcomeDrawingPreviewNative, UnusedDefsDoNotRejectVisibleArtAndUseCloneIsClassified)
{
    auto visible_doc = load(fixture("use-visible-defs.svg"));
    ASSERT_TRUE(visible_doc);
    auto visible = render(*visible_doc, 120, 80);
    ASSERT_EQ(visible.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(visible.status);
    ASSERT_TRUE(visible.surface);
    // Visible <use> clone (red) and ordinary blue rect both render.
    EXPECT_TRUE(contains_rgb(visible.surface, 255, 0, 0));
    EXPECT_TRUE(contains_rgb(visible.surface, 0, 0, 255));

    auto unpainted_doc = load(fixture("use-unpainted-defs.svg"));
    ASSERT_TRUE(unpainted_doc);
    auto unpainted = render(*unpainted_doc, 120, 80);
    EXPECT_EQ(unpainted.status, WelcomePreviewStatus::UnsupportedNativeSemantics);
    EXPECT_EQ(unpainted.limitation, WelcomePreviewLimitation::ShapePaintsNothing);
    EXPECT_FALSE(unpainted.surface);
}

TEST_F(WelcomeDrawingPreviewNative, SymbolUseCloneIsAdmittedAndOpacityZeroDescendantRejected)
{
    auto visible_doc = load(fixture("use-symbol-visible.svg"));
    ASSERT_TRUE(visible_doc);
    auto visible = render(*visible_doc, 120, 80);
    ASSERT_EQ(visible.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(visible.status);
    ASSERT_TRUE(visible.surface);
    // The cloned visible symbol paints its red interior.
    EXPECT_TRUE(contains_rgb(visible.surface, 255, 0, 0));

    auto zero_doc = load(fixture("use-symbol-opacity-zero.svg"));
    ASSERT_TRUE(zero_doc);
    auto zero = render(*zero_doc, 120, 80);
    EXPECT_EQ(zero.status, WelcomePreviewStatus::UnsupportedNativeSemantics);
    EXPECT_EQ(zero.limitation, WelcomePreviewLimitation::ItemOpacityZero);
    EXPECT_FALSE(zero.surface);
}

TEST_F(WelcomeDrawingPreviewNative, PrimitiveDoesNotMutateDocumentOrFixture)
{
    std::string const before = fixture("offpage.svg");
    auto doc = load(before);
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    XML::Node *repr = doc->getRoot()->getRepr();
    ASSERT_TRUE(repr);
    DocumentUndo::setUndoSensitive(doc.get(), true);

    // A real native edit plus undo leaves a live Redo branch on this private
    // document before the preview is rendered.
    repr->setAttribute("data-preview-edit", "one");
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"welcome preview history fixture"}, "");
    DocumentUndo::undo(doc.get());

    // Snapshot only after ensureUpToDate and the edit/undo.
    std::string const before_xml = xml_identity(doc->getReprRoot());
    bool const before_dirty = doc->isModifiedSinceSave();
    bool const before_sensitive = DocumentUndo::getUndoSensitive(doc.get());
    std::string const before_source_hash = checksum(fixture("offpage.svg"));
    std::string const before_root_id = doc->getRoot()->getId() ? doc->getRoot()->getId() : "";

    ObjectSet selection(doc.get());
    selection.add(repr);
    auto const selected = selection.items_vector();
    ASSERT_FALSE(selected.empty());

    auto result = render(*doc, 200, 150);
    ASSERT_EQ(result.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(result.status);
    ASSERT_TRUE(result.surface);

    // Same private document: XML, dirty/history state, source bytes and
    // selection are all unchanged by rendering.
    EXPECT_EQ(xml_identity(doc->getReprRoot()), before_xml);
    EXPECT_EQ(doc->isModifiedSinceSave(), before_dirty);
    EXPECT_EQ(DocumentUndo::getUndoSensitive(doc.get()), before_sensitive);
    EXPECT_EQ(checksum(fixture("offpage.svg")), before_source_hash);
    EXPECT_EQ(doc->getRoot()->getId() ? doc->getRoot()->getId() : "", before_root_id);
    EXPECT_EQ(selection.items_vector(), selected);

    // The retained Redo branch is preserved and still reversible.
    DocumentUndo::redo(doc.get());
    EXPECT_STREQ(repr->attribute("data-preview-edit"), "one");
    DocumentUndo::undo(doc.get());
    EXPECT_EQ(xml_identity(doc->getReprRoot()), before_xml);
}

TEST_F(WelcomeDrawingPreviewNative, InvalidDimensionsAreRejectedBeforeAllocation)
{
    auto doc = load(fixture("offpage.svg"));
    ASSERT_TRUE(doc);
    for (auto dims : {std::array<unsigned, 2>{0, 10}, std::array<unsigned, 2>{10, 0},
                      std::array<unsigned, 2>{5000, 10}, std::array<unsigned, 2>{2049, 2049}}) {
        WelcomeDrawingPreviewRequest request;
        request.width = dims[0];
        request.height = dims[1];
        auto result = renderWelcomeDrawingPreview(*doc, request);
        EXPECT_EQ(result.status, WelcomePreviewStatus::Limits)
            << dims[0] << "x" << dims[1] << " " << welcomePreviewStatusName(result.status);
        EXPECT_EQ(result.limitation, WelcomePreviewLimitation::AllocationLimit);
        EXPECT_FALSE(result.surface);
    }
}

TEST_F(WelcomeDrawingPreviewNative, AggregateBudgetFailureIsBoundedAndCleanupAllowsRetry)
{
    auto doc = load(fixture("offpage.svg"));
    ASSERT_TRUE(doc);
    WelcomeDrawingPreviewRequest request;
    request.width = 64;
    request.height = 64;
    request.limits.bytes = 1; // Valid for the constructor, too small for the output surface.
    auto failed = renderWelcomeDrawingPreview(*doc, request);
    EXPECT_EQ(failed.status, WelcomePreviewStatus::Limits);
    EXPECT_FALSE(failed.surface);

    // A following request with default limits proves the failed request released
    // all native/budget state.
    auto ok = render(*doc, 64, 64);
    ASSERT_EQ(ok.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(ok.status);
    EXPECT_TRUE(ok.surface);
}

TEST_F(WelcomeDrawingPreviewNative, ReentrantNativeBudgetFailureIsExplicitAndRecovers)
{
    auto doc = load(fixture("offpage.svg"));
    ASSERT_TRUE(doc);
    {
        PreviewRenderBudget outer{PreviewRenderBudget::Limits{}};
        auto nested = render(*doc, 64, 64);
        EXPECT_EQ(nested.status, WelcomePreviewStatus::Limits);
        EXPECT_FALSE(nested.surface);
    }
    auto recovered = render(*doc, 64, 64);
    ASSERT_EQ(recovered.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(recovered.status);
    EXPECT_TRUE(recovered.surface);
}

TEST_F(WelcomeDrawingPreviewNative, CancellationReturnsWithoutSurface)
{
    auto doc = load(fixture("offpage.svg"));
    ASSERT_TRUE(doc);
    WelcomeDrawingPreviewRequest request;
    request.width = 64;
    request.height = 64;
    request.cancelled = [] { return true; };
    auto result = renderWelcomeDrawingPreview(*doc, request);
    EXPECT_EQ(result.status, WelcomePreviewStatus::Cancelled);
    EXPECT_FALSE(result.surface);
    EXPECT_EQ(result.width, 0u);
    EXPECT_EQ(result.height, 0u);
}

TEST_F(WelcomeDrawingPreviewNative, MidClassifierCancellationStopsTraversalAndSameDocumentRetries)
{
    auto doc = load(fixture("traversal-cancel.svg"));
    ASSERT_TRUE(doc);
    // Flush pending native work before arming cancellation so the checkpoint
    // count reflects this bounded traversal only.
    doc->ensureUpToDate();
    WelcomeDrawingPreviewRequest request;
    request.width = 120;
    request.height = 120;
    // The first two checkpoints are the pre-classifier ones. Let two further
    // traversal iterations pass, then cancel on the 5th callback: that is the
    // classifier checkpoint, before any item is shown, so root->views is still
    // empty. Cancelling at a later checkpoint (e.g. after drawing.update) would
    // record views_empty_at_cancel == false, and removing loop checkpoints
    // entirely would not reach 5 calls: either way this test must fail.
    auto *root = doc->getRoot();
    ASSERT_TRUE(root);
    ASSERT_TRUE(root->views.empty());
    unsigned calls = 0;
    bool views_empty_at_cancel = false;
    request.cancelled = [&] {
        ++calls;
        if (calls < 5) {
            return false;
        }
        views_empty_at_cancel = root->views.empty();
        return true;
    };
    auto cancelled = renderWelcomeDrawingPreview(*doc, request);
    EXPECT_EQ(cancelled.status, WelcomePreviewStatus::Cancelled);
    EXPECT_FALSE(cancelled.surface);
    // Exact checkpoint identity: cancellation comes from the classifier
    // traversal checkpoint while no render item has been shown yet.
    EXPECT_EQ(calls, 5u);
    EXPECT_TRUE(views_empty_at_cancel);
    EXPECT_TRUE(root->views.empty());

    // The same private document still renders when cancellation is removed.
    WelcomeDrawingPreviewRequest retry_request;
    retry_request.width = 120;
    retry_request.height = 120;
    auto retry = renderWelcomeDrawingPreview(*doc, retry_request);
    ASSERT_EQ(retry.status, WelcomePreviewStatus::Rendered) << welcomePreviewStatusName(retry.status);
    EXPECT_TRUE(retry.surface);
}

namespace {
WelcomePreviewLaunchResult launch_file(std::string const &text, char const *extension = ".svg",
                                        unsigned width = 120, unsigned height = 80,
                                        unsigned deadline = 5000)
{
    auto directory = g_dir_make_tmp("vacards-welcome-helper-XXXXXX", nullptr);
    if (!directory) throw std::runtime_error("cannot create helper test directory");
    auto path = std::string(directory) + "/drawing" + extension;
    g_free(directory);
    if (!g_file_set_contents(path.c_str(), text.data(), text.size(), nullptr))
        throw std::runtime_error("cannot write helper fixture");
    bool done = false;
    WelcomePreviewLaunchResult result;
    launch_welcome_preview(path, width, height, deadline, [&](WelcomePreviewLaunchResult r) {
        result = r;
        done = true;
    });
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done && std::chrono::steady_clock::now() < end)
        g_main_context_iteration(nullptr, true);
    std::filesystem::remove(path);
    std::filesystem::remove(std::filesystem::path(path).parent_path());
    if (!done) throw std::runtime_error("helper callback did not complete");
    return result;
}

std::string gzip(std::string const &source)
{
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS,
                     8, Z_DEFAULT_STRATEGY) != Z_OK) throw std::runtime_error("gzip init");
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(source.data()));
    stream.avail_in = static_cast<uInt>(source.size());
    std::string compressed;
    char buffer[4096];
    int status;
    do {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);
        status = deflate(&stream, Z_FINISH);
        compressed.append(buffer, sizeof(buffer) - stream.avail_out);
    } while (status == Z_OK);
    deflateEnd(&stream);
    if (status != Z_STREAM_END) throw std::runtime_error("gzip failed");
    return compressed;
}
} // namespace

TEST_F(WelcomeDrawingPreviewNative, HelperRendersValidAndOffPageAndSvgz)
{
    auto svg = fixture("offpage.svg");
    for (auto const &input : {std::pair{svg, ".svg"}, std::pair{gzip(svg), ".svgz"}}) {
        auto result = launch_file(input.first, input.second);
        ASSERT_EQ(result.error, WelcomePreviewLaunchError::Ok);
        ASSERT_NE(result.pixbuf, nullptr);
        EXPECT_EQ(gdk_pixbuf_get_width(result.pixbuf), 120);
        EXPECT_EQ(gdk_pixbuf_get_height(result.pixbuf), 80);
        // Both off-page rectangles must be in the framed image.
        bool red = false, blue = false;
        for (int y = 0; y < 80; ++y) for (int x = 0; x < 120; ++x) {
            auto p = gdk_pixbuf_get_pixels(result.pixbuf) + y * gdk_pixbuf_get_rowstride(result.pixbuf) +
                     x * gdk_pixbuf_get_n_channels(result.pixbuf);
            red |= p[0] > 240 && p[1] < 20 && p[2] < 20;
            blue |= p[2] > 240 && p[0] < 20 && p[1] < 20;
        }
        EXPECT_TRUE(red);
        EXPECT_TRUE(blue);
        g_object_unref(result.pixbuf);
    }
}

#ifdef __APPLE__
TEST_F(WelcomeDrawingPreviewNative, HelperUsesFontInstalledOnlyInHome)
{
    auto home = std::filesystem::temp_directory_path() / ("vacards-font-home-" + std::to_string(g_get_real_time()));
    auto fonts = home / "Library/Fonts";
    std::filesystem::create_directories(fonts);
    auto font = std::filesystem::path(WELCOME_DRAWING_PREVIEW_FIXTURES).parent_path().parent_path() /
                "rendering_tests/fonts/PressStart2P.ttf";
    std::filesystem::copy_file(font, fonts / "PressStart2P.ttf");
    auto prior = g_getenv("HOME") ? std::string(g_getenv("HOME")) : std::string();
    g_setenv("HOME", home.c_str(), true);
    auto render = [](char const *family) {
        auto svg = std::string("<svg xmlns='http://www.w3.org/2000/svg' width='300' height='80'>") +
            "<text x='3' y='57' font-size='48' font-family='" + family + "'>MMMM</text></svg>";
        return launch_file(svg, ".svg", 300, 80);
    };
    auto installed = render("Press Start 2P");
    auto fallback = render("sans-serif");
    if (prior.empty()) g_unsetenv("HOME"); else g_setenv("HOME", prior.c_str(), true);
    std::filesystem::remove_all(home);
    ASSERT_EQ(installed.error, WelcomePreviewLaunchError::Ok);
    ASSERT_EQ(fallback.error, WelcomePreviewLaunchError::Ok);
    ASSERT_NE(installed.pixbuf, nullptr);
    ASSERT_NE(fallback.pixbuf, nullptr);
    auto bytes = gdk_pixbuf_get_rowstride(installed.pixbuf) * gdk_pixbuf_get_height(installed.pixbuf);
    EXPECT_NE(std::memcmp(gdk_pixbuf_get_pixels(installed.pixbuf), gdk_pixbuf_get_pixels(fallback.pixbuf), bytes), 0);
    g_object_unref(installed.pixbuf);
    g_object_unref(fallback.pixbuf);
}
#endif

TEST_F(WelcomeDrawingPreviewNative, HelperRejectsExternalAndEntitiesAndLimits)
{
    auto svg = std::string("<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'>") +
               "<image href='file:///tmp/secret.png' width='10' height='10'/></svg>";
    EXPECT_EQ(launch_file(svg).error, WelcomePreviewLaunchError::Rejected);
    EXPECT_EQ(launch_file("<!DOCTYPE svg [<!ENTITY x 'boom'>]><svg xmlns='http://www.w3.org/2000/svg'>&x;</svg>").error,
              WelcomePreviewLaunchError::Rejected);
    EXPECT_EQ(launch_file(std::string(4u * 1024u * 1024u + 1, 'x')).error,
              WelcomePreviewLaunchError::TooLarge);
}

TEST_F(WelcomeDrawingPreviewNative, HelperDeadlineAndCrashAreContained)
{
    g_setenv("VACARDS_FILE_IO_TEST_HOOKS", "1", true);
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "sleep", true);
    EXPECT_EQ(launch_file(fixture("offpage.svg"), ".svg", 120, 80, 30).error,
              WelcomePreviewLaunchError::Timeout);
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "slow-reap", true);
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(launch_file(fixture("offpage.svg"), ".svg", 120, 80, 30).error,
              WelcomePreviewLaunchError::Timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(450));
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "stdout", true);
    auto printed = launch_file(fixture("offpage.svg"));
    EXPECT_EQ(printed.error, WelcomePreviewLaunchError::Ok);
    if (printed.pixbuf) g_object_unref(printed.pixbuf);
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "abort", true);
    EXPECT_EQ(launch_file(fixture("offpage.svg")).error, WelcomePreviewLaunchError::Crashed);
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "exit3", true);
    EXPECT_EQ(launch_file(fixture("offpage.svg")).error, WelcomePreviewLaunchError::Crashed);
    g_unsetenv("VACARDS_PREVIEW_TEST_ACTION");
    g_unsetenv("VACARDS_FILE_IO_TEST_HOOKS");
}


namespace {
void wait_for(std::function<bool()> ready)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!ready() && std::chrono::steady_clock::now() < end) {
        while (g_main_context_iteration(nullptr, false)) {}
        g_usleep(1000);
    }
    ASSERT_TRUE(ready());
}
struct ThumbnailFixture {
    std::filesystem::path dir, path, cache;
    ThumbnailFixture() {
        auto name = g_dir_make_tmp("vacards-welcome-cache-XXXXXX", nullptr);
        if (!name) throw std::runtime_error("cannot create cache fixture");
        dir = name; g_free(name);
        path = dir / "source.svg"; cache = dir / "cache";
        write(fixture("offpage.svg"));
    }
    void write(std::string const &text) {
        if (!g_file_set_contents(path.string().c_str(), text.data(), text.size(), nullptr))
            throw std::runtime_error("cannot write cache fixture");
    }
    ~ThumbnailFixture() { std::filesystem::remove_all(dir); }
};
}

TEST_F(WelcomeDrawingPreviewNative, CancelledRenderReleasesSlotForNextPreview)
{
    ThumbnailFixture files;
    WelcomeThumbnailService service(files.cache.string());
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "sleep", true);
    bool cancelled_callback = false;
    service.request(files.path.string(), 120, 80, [&](auto result) {
        cancelled_callback = true;
        if (result.pixbuf) g_object_unref(result.pixbuf);
    });
    wait_for([&] { return service.stats().renders == 1; });
    service.cancel(files.path.string(), 120, 80);
    g_unsetenv("VACARDS_PREVIEW_TEST_ACTION");
    bool done = false;
    service.request(files.path.string(), 121, 80, [&](auto result) {
        EXPECT_EQ(result.error, WelcomePreviewLaunchError::Ok);
        if (result.pixbuf) g_object_unref(result.pixbuf);
        done = true;
    });
    wait_for([&] { return done; });
    EXPECT_FALSE(cancelled_callback);
}

TEST_F(WelcomeDrawingPreviewNative, CancelStopsChildBeforeDeadline)
{
    ThumbnailFixture files;
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "sleep", true);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    bool done = false;
    auto start = std::chrono::steady_clock::now();
    launch_welcome_preview(files.path.string(), 120, 80, 5000, [&](auto result) {
        EXPECT_NE(result.error, WelcomePreviewLaunchError::Ok);
        if (result.pixbuf) g_object_unref(result.pixbuf);
        done = true;
    }, cancelled);
    *cancelled = true;
    wait_for([&] { return done; });
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
    g_unsetenv("VACARDS_PREVIEW_TEST_ACTION");
}

TEST_F(WelcomeDrawingPreviewNative, ThumbnailCacheKeyInvalidationAndCorruptRecovery)
{
    ThumbnailFixture files;
    auto ask = [&](WelcomeThumbnailService &service) {
        bool done = false; WelcomePreviewLaunchError error = WelcomePreviewLaunchError::Failed;
        service.request(files.path.string(), 120, 80, [&](auto result) {
            error = result.error; if (result.pixbuf) g_object_unref(result.pixbuf); done = true;
        });
        wait_for([&] { return done; });
        EXPECT_EQ(error, WelcomePreviewLaunchError::Ok);
    };
    WelcomeThumbnailService service(files.cache.string());
    ask(service);
    EXPECT_EQ(service.stats().renders, 1u);
    ask(service);
    EXPECT_EQ(service.stats().renders, 1u);
    auto later = std::filesystem::last_write_time(files.path) + std::chrono::seconds(3);
    std::filesystem::last_write_time(files.path, later);
    ask(service);
    EXPECT_EQ(service.stats().renders, 2u);
    files.write(fixture("offpage.svg") + " ");
    ask(service);
    EXPECT_EQ(service.stats().renders, 3u);
    // Wait for three finished entries and no write in flight: a temporary file
    // renamed after the corruption below would be a valid entry.
    wait_for([&] {
        if (!std::filesystem::exists(files.cache)) return false;
        std::size_t finished = 0;
        for (auto const &entry : std::filesystem::directory_iterator(files.cache)) {
            if (entry.path().extension() != ".png") return false;
            ++finished;
        }
        return finished >= 3;
    });
    for (auto const &entry : std::filesystem::directory_iterator(files.cache)) {
        std::ofstream output(entry.path(), std::ios::trunc); output << "bad PNG";
    }
    WelcomeThumbnailService fresh(files.cache.string());
    ask(fresh);
    EXPECT_EQ(fresh.stats().renders, 1u);
}

TEST_F(WelcomeDrawingPreviewNative, ThumbnailSchedulerDeduplicatesCancelsAndSerializes)
{
    ThumbnailFixture files;
    WelcomeThumbnailService service(files.cache.string());
    unsigned callbacks = 0;
    for (int i = 0; i < 2; ++i) service.request(files.path.string(), 120, 80, [&](auto result) {
        EXPECT_EQ(result.error, WelcomePreviewLaunchError::Ok);
        if (result.pixbuf) g_object_unref(result.pixbuf);
        ++callbacks;
    });
    EXPECT_EQ(service.stats().active, 1u);
    wait_for([&] { return callbacks == 2; });
    EXPECT_EQ(service.stats().renders, 1u);
    bool cancelled_callback = false;
    service.request(files.path.string(), 121, 80, [&](auto result) {
        if (result.pixbuf) g_object_unref(result.pixbuf);
        cancelled_callback = true;
    });
    service.cancel(files.path.string(), 121, 80);
    bool next_done = false;
    service.request(files.path.string(), 122, 80, [&](auto result) {
        EXPECT_EQ(result.error, WelcomePreviewLaunchError::Ok);
        if (result.pixbuf) g_object_unref(result.pixbuf);
        next_done = true;
    });
    wait_for([&] { return next_done; });
    EXPECT_FALSE(cancelled_callback);
    EXPECT_LE(service.stats().active, 1u);
}

TEST_F(WelcomeDrawingPreviewNative, TwoServicesShareRenderGateAndDestroyedOwnerDoesNotWrite)
{
    ThumbnailFixture files;
    auto other = files.dir / "other.svg";
    std::filesystem::copy_file(files.path, other);
    auto owner_cache = files.dir / "owner-cache";
    auto first = std::make_unique<WelcomeThumbnailService>(owner_cache.string());
    WelcomeThumbnailService second(files.cache.string());
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "sleep", true);
    g_setenv("VACARDS_PREVIEW_TEST_DISK_WRITE_DELAY", "1", true);
    bool first_done = false;
    first->request(files.path.string(), 120, 80, [&](auto result) {
        first_done = result.error == WelcomePreviewLaunchError::Ok;
        if (result.pixbuf) g_object_unref(result.pixbuf);
    });
    wait_for([&] { return first->stats().renders == 1; });
    bool done = false;
    second.request(other.string(), 120, 80, [&](auto result) {
        EXPECT_EQ(result.error, WelcomePreviewLaunchError::Ok);
        if (result.pixbuf) g_object_unref(result.pixbuf);
        done = true;
    });
    EXPECT_EQ(second.stats().renders, 0u);
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < until) g_main_context_iteration(nullptr, false);
    EXPECT_EQ(second.stats().renders, 0u);
    g_unsetenv("VACARDS_PREVIEW_TEST_ACTION");
    // The first helper was already launched with "sleep"; cancel it so the
    // owner can be destroyed immediately after an Ok result from a new request.
    first->cancel(files.path.string(), 120, 80);
    first->request(files.path.string(), 121, 80, [&](auto result) {
        first_done = result.error == WelcomePreviewLaunchError::Ok;
        if (result.pixbuf) g_object_unref(result.pixbuf);
    });
    wait_for([&] { return first_done; });
    first.reset();
    wait_for([&] { return done; });
    EXPECT_EQ(second.stats().renders, 1u);
    // The delayed owner write must have passed its 500 ms test barrier.
    g_usleep(600000);
    EXPECT_FALSE(std::filesystem::exists(owner_cache));
    g_unsetenv("VACARDS_PREVIEW_TEST_DISK_WRITE_DELAY");
}

TEST_F(WelcomeDrawingPreviewNative, ThumbnailSuppressesStaleResultAndQueuesOneRender)
{
    ThumbnailFixture files;
    auto second = files.dir / "second.svg";
    std::filesystem::copy_file(files.path, second);
    WelcomeThumbnailService service(files.cache.string());
    g_setenv("VACARDS_PREVIEW_TEST_ACTION", "delay", true);
    unsigned done = 0;
    WelcomePreviewLaunchError first = WelcomePreviewLaunchError::Ok;
    service.request(files.path.string(), 120, 80, [&](auto result) {
        first = result.error; if (result.pixbuf) g_object_unref(result.pixbuf); ++done;
    });
    service.request(second.string(), 120, 80, [&](auto result) {
        EXPECT_EQ(result.error, WelcomePreviewLaunchError::Ok);
        if (result.pixbuf) g_object_unref(result.pixbuf); ++done;
    });
    wait_for([&] { return service.stats().renders == 1; });
    EXPECT_EQ(service.stats().active, 1u);
    EXPECT_EQ(service.stats().queued, 1u);
    files.write(fixture("offpage.svg") + "<!-- changed during render -->");
    wait_for([&] { return done == 2; });
    g_unsetenv("VACARDS_PREVIEW_TEST_ACTION");
    EXPECT_NE(first, WelcomePreviewLaunchError::Ok);
    EXPECT_EQ(service.stats().renders, 2u);
}

TEST_F(WelcomeDrawingPreviewNative, ThumbnailByteCapsEvictOldEntries)
{
    ThumbnailFixture files;
    std::filesystem::create_directories(files.cache);
    auto old = files.cache / "old.png";
    { std::ofstream output(old, std::ios::binary); output.seekp(65u * 1024u * 1024u); output.put('x'); }
    WelcomeThumbnailService service(files.cache.string());
    for (unsigned i = 0; i < 17; ++i) {
        auto path = files.dir / (std::to_string(i) + ".svg");
        std::filesystem::copy_file(files.path, path);
        bool done = false;
        service.request(path.string(), 512, 512, [&](auto result) {
            EXPECT_EQ(result.error, WelcomePreviewLaunchError::Ok);
            if (result.pixbuf) g_object_unref(result.pixbuf);
            done = true;
        });
        wait_for([&] { return done; });
        EXPECT_LE(service.stats().memory_bytes, 16u * 1024u * 1024u);
    }
    wait_for([&] { return !std::filesystem::exists(old); });
    std::uintmax_t total = 0;
    for (auto const &entry : std::filesystem::directory_iterator(files.cache))
        total += std::filesystem::file_size(entry);
    EXPECT_LE(total, 64u * 1024u * 1024u);
}

namespace {
// A red 100x50 rectangle padded past the helper's 4 MiB input cap, as cards
// with embedded photos are.
std::string const large_card = std::string(
    "<svg xmlns='http://www.w3.org/2000/svg' width='300' height='200' viewBox='0 0 300 200'>"
    "<rect x='50' y='50' width='100' height='50' fill='#ff0000'/><!-- ") +
    std::string(5u * 1024u * 1024u, 'x') + " --></svg>";

struct ThumbnailAnswer { WelcomePreviewLaunchError error = WelcomePreviewLaunchError::Failed; GdkPixbuf *pixbuf = nullptr; };
ThumbnailAnswer ask_thumbnail(WelcomeThumbnailService &service, std::string const &path, unsigned w, unsigned h)
{
    ThumbnailAnswer answer; bool done = false;
    service.request(path, w, h, [&](auto result) { answer = {result.error, result.pixbuf}; done = true; });
    wait_for([&] { return done; });
    return answer;
}
std::array<unsigned, 3> pixel(GdkPixbuf *pixbuf, int x, int y)
{
    auto const *p = gdk_pixbuf_get_pixels(pixbuf) + y * gdk_pixbuf_get_rowstride(pixbuf) +
                    x * gdk_pixbuf_get_n_channels(pixbuf);
    return {p[0], p[1], p[2]};
}
std::size_t png_count(std::filesystem::path const &directory)
{
    if (!std::filesystem::exists(directory)) return 0;
    return std::count_if(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{},
                         [](auto const &entry) { return entry.path().extension() == ".png"; });
}
} // namespace

TEST_F(WelcomeDrawingPreviewNative, OpenDocumentThumbnailServesFilesTheHelperRefuses)
{
    ThumbnailFixture files;
    files.write(large_card);
    WelcomeThumbnailService before((files.dir / "before").string());
    auto refused = ask_thumbnail(before, files.path.string(), 300, 226);
    EXPECT_EQ(refused.error, WelcomePreviewLaunchError::TooLarge);
    if (refused.pixbuf) g_object_unref(refused.pixbuf);

    auto doc = SPDocument::createNewDoc(files.path.string().c_str(), false);
    ASSERT_TRUE(doc);
    bool done = false, stored = false;
    store_document_thumbnail(*doc, files.path.string(), 0, files.cache.string(), [&](bool ok) { stored = ok; done = true; });
    wait_for([&] { return done; });
    EXPECT_TRUE(stored);

    WelcomeThumbnailService service(files.cache.string());
    for (unsigned scale : {1u, 2u}) {
        unsigned const w = welcome_card_width * scale, h = welcome_card_height * scale;
        auto answer = ask_thumbnail(service, files.path.string(), w, h);
        ASSERT_EQ(answer.error, WelcomePreviewLaunchError::Ok);
        ASSERT_TRUE(answer.pixbuf);
        EXPECT_EQ(gdk_pixbuf_get_width(answer.pixbuf), static_cast<int>(w));
        EXPECT_EQ(gdk_pixbuf_get_height(answer.pixbuf), static_cast<int>(h));
        // Framed on the rectangle with 5% margin: 90% of the width, centred.
        EXPECT_EQ(pixel(answer.pixbuf, w / 2, h / 2), (std::array<unsigned, 3>{255, 0, 0}));
        EXPECT_EQ(pixel(answer.pixbuf, w * 8 / 100, h / 2), (std::array<unsigned, 3>{255, 0, 0}));
        EXPECT_EQ(pixel(answer.pixbuf, w * 3 / 100, h / 2), (std::array<unsigned, 3>{255, 255, 255}));
        EXPECT_EQ(pixel(answer.pixbuf, w / 2, h / 10), (std::array<unsigned, 3>{255, 255, 255}));
        g_object_unref(answer.pixbuf);
    }
    EXPECT_EQ(service.stats().renders, 0u);

    // A second call for the same file version keeps the entries.
    done = stored = false;
    store_document_thumbnail(*doc, files.path.string(), 0, files.cache.string(), [&](bool ok) { stored = ok; done = true; });
    wait_for([&] { return done; });
    EXPECT_TRUE(stored);
    EXPECT_EQ(png_count(files.cache), 2u);
}

TEST_F(WelcomeDrawingPreviewNative, DocumentThumbnailIsNotStoredForADocumentThatDiffersFromTheFile)
{
    ThumbnailFixture files;
    auto doc = SPDocument::createNewDoc(files.path.string().c_str(), false);
    ASSERT_TRUE(doc);
    auto attempt = [&](std::string const &path) {
        bool done = false, stored = true;
        store_document_thumbnail(*doc, path, 0, files.cache.string(), [&](bool ok) { stored = ok; done = true; });
        wait_for([&] { return done; });
        return stored;
    };
    doc->setModifiedSinceSave(true);
    EXPECT_FALSE(attempt(files.path.string()));
    doc->setModifiedSinceSave(false);
    auto copy = files.dir / "copy.svg"; // Save a Copy: the document keeps its own file
    std::filesystem::copy_file(files.path, copy);
    EXPECT_FALSE(attempt(copy.string()));
    auto png = files.dir / "export.png"; // only .svg/.svgz files have Welcome thumbnails
    std::filesystem::copy_file(files.path, png);
    doc->setDocumentFilename(png.string().c_str());
    EXPECT_FALSE(attempt(png.string()));
    EXPECT_EQ(png_count(files.cache), 0u);
}

TEST_F(WelcomeDrawingPreviewNative, DocumentThumbnailIsNotStoredAfterTheDocumentCloses)
{
    ThumbnailFixture files;
    auto doc = SPDocument::createNewDoc(files.path.string().c_str(), false);
    ASSERT_TRUE(doc);
    bool done = false, stored = true;
    store_document_thumbnail(*doc, files.path.string(), 200, files.cache.string(), [&](bool ok) { stored = ok; done = true; });
    doc.reset();
    wait_for([&] { return done; });
    EXPECT_FALSE(stored);
    EXPECT_EQ(png_count(files.cache), 0u);
}
