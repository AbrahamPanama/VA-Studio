// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Tests for Explode Bitmap (Output): committed-only offscreen rendering (T18, T30).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <map>
#include <vector>
#include <glib.h>
#include <filesystem>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#include "colors/color.h"
#include "display/cairo-utils.h"
#include "display/drawing.h"
#include "display/drawing-context.h"
#include "display/preview-render-budget.h"
#include "document.h"
#include "helper/pixbuf-ops.h"
#include "extension/internal/cairo-render-context.h"
#include "extension/internal/cairo-renderer.h"
#include "helper/png-write.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "inkscape.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "util/units.h"

using namespace Inkscape;
using Bitmap::Budget;
using Bitmap::RenderRequest;
using Bitmap::Status;

namespace {

// 4x4 opaque red PNG.
constexpr char const *RED_PNG =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAEklEQVR4nGP4z8DwHxkzkC4AADxAH+HggXe0AAAAAElFTkSuQmCC";

std::string const IMAGE_SVG =
    std::string("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
                "width='40' height='40' viewBox='0 0 40 40'>"
                "<image id='img' x='0' y='0' width='40' height='40' preserveAspectRatio='none' "
                "xlink:href='data:image/png;base64,") + RED_PNG + "'/></svg>";

// A 40x40 rect filled by a 20x20 px pattern tile.
std::string const PATTERN_SVG =
    "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='40' viewBox='0 0 40 40'>"
    "<defs><pattern id='p' width='20' height='20' patternUnits='userSpaceOnUse'>"
    "<rect width='10' height='10' fill='#00ff00'/></pattern></defs>"
    "<rect width='40' height='40' style='fill:url(#p)'/></svg>";

std::unique_ptr<SPDocument> make_doc(std::string const &svg)
{
    if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(), svg.size()));
    if (doc) doc->ensureUpToDate(); // an update after a preview is installed would reset it
    return doc;
}

std::shared_ptr<Pixbuf const> solid_pixbuf(uint32_t argb)
{
    auto *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    auto *px = reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(s));
    for (int i = 0; i < 16; ++i) px[i] = argb;
    cairo_surface_mark_dirty(s);
    return std::make_shared<Pixbuf>(s);
}

uint32_t pixel_at(cairo_surface_t *s, int x, int y)
{
    cairo_surface_flush(s);
    auto const *row = cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s);
    return reinterpret_cast<uint32_t const *>(row)[x];
}
uint32_t pixel_at(Pixbuf const &pb, int x, int y) { return pixel_at(pb.getSurfaceRaw(), x, y); }

constexpr uint32_t RED = 0xffff0000, GREEN = 0xff00ff00;

RenderRequest request(SPDocument *doc)
{
    RenderRequest r;
    r.document = doc;
    r.area = Geom::Rect(Geom::Point(0, 0), Geom::Point(40, 40));
    r.dpi = 96.0;
    return r;
}

Budget &roomy()
{
    static Budget b(Budget::FixedLimitForTest{}, 256 * Bitmap::MiB);
    return b;
}

/// A canvas-like view of the document with a view-only preview installed on its own key.
struct PreviewView {
    PreviewView(SPDocument *doc, uint32_t preview_color) : doc(doc)
    {
        key = SPItem::display_key_new(1);
        drawing.setRoot(doc->getRoot()->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY));
        auto *image = cast<SPImage>(doc->getObjectById("img"));
        EXPECT_TRUE(image && image->setViewPixbuf(key, solid_pixbuf(preview_color)));
        drawing.update();
    }
    ~PreviewView() { doc->getRoot()->invoke_hide(key); }
    uint32_t viewPixel()
    {
        auto *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 40, 40);
        {
            DrawingContext dc(s, Geom::Point(0, 0));
            drawing.render(dc, Geom::IntRect::from_xywh(0, 0, 40, 40), DrawingItem::RENDER_BYPASS_CACHE);
        }
        auto p = pixel_at(s, 20, 20);
        cairo_surface_destroy(s);
        return p;
    }
    SPDocument *doc;
    Drawing drawing;
    unsigned key = 0;
};

cairo_surface_t *null_surface(int, int) { return nullptr; }
cairo_surface_t *error_surface(int, int) { return cairo_image_surface_create(CAIRO_FORMAT_ARGB32, -1, -1); }
cairo_surface_t *throwing_surface(int, int) { throw std::runtime_error("injected render failure"); }
// Models a pattern/filter cache allocation failing inside the render.
cairo_surface_t *oom_surface(int, int) { throw std::bad_alloc(); }

bool arena_clean(SPDocument *doc, unsigned key, char const *id)
{
    auto *item = cast<SPItem>(doc->getObjectById(id));
    return item && item->get_arenaitem(key) == nullptr;
}

struct TempDir {
    TempDir() : path(std::filesystem::temp_directory_path() / ("eb3-output-" + std::to_string(::getpid()))) {
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::filesystem::path path;
};

} // namespace

// T30: a view-only preview on a view key never reaches any output path.
TEST(ExplodeBitmapOutput, PreviewControlViewShowsPreview)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    PreviewView view(doc.get(), GREEN);
    EXPECT_EQ(view.viewPixel(), GREEN); // the preview really is active in the view
}

TEST(ExplodeBitmapOutput, CommittedRenderIgnoresViewPreview)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    PreviewView view(doc.get(), GREEN);

    auto out = Bitmap::renderCommitted(request(doc.get()), roomy());
    ASSERT_TRUE(out.ok()) << out.outcome.diagnostic;
    EXPECT_EQ(pixel_at(*out.pixbuf, 20, 20), RED);
    EXPECT_NE(out.output_key, 0u);
    EXPECT_NE(out.output_key, view.key); // independent output key
    EXPECT_TRUE(arena_clean(doc.get(), out.output_key, "img"));
    EXPECT_EQ(view.viewPixel(), GREEN); // output did not disturb the view preview
}

TEST(ExplodeBitmapOutput, CopyAndMakeBitmapCopyPathsUseCommittedPixels)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    PreviewView view(doc.get(), GREEN);
    auto *image = cast<SPImage>(doc->getObjectById("img"));
    ASSERT_TRUE(image);

    // Make Bitmap Copy / copy as bitmap: selected items, no opacity override.
    std::unique_ptr<Pixbuf> copy(sp_generate_internal_bitmap(doc.get(), Geom::Rect(Geom::Point(0, 0), Geom::Point(40, 40)),
                                                             96.0, {image}));
    ASSERT_TRUE(copy);
    EXPECT_EQ(pixel_at(*copy, 20, 20), RED);

    // PDF/print filtered-object path: set_opaque with the item list.
    std::unique_ptr<Pixbuf> opaque(sp_generate_internal_bitmap(doc.get(), Geom::Rect(Geom::Point(0, 0), Geom::Point(40, 40)),
                                                               96.0, {image}, true));
    ASSERT_TRUE(opaque);
    EXPECT_EQ(pixel_at(*opaque, 20, 20), RED);
    EXPECT_EQ(view.viewPixel(), GREEN);
}

TEST(ExplodeBitmapOutput, PngExportUsesCommittedPixels)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    PreviewView view(doc.get(), GREEN);
    TempDir dir;
    auto file = (dir.path / "out.png").string();

    auto result = sp_export_png_file(doc.get(), file.c_str(), 0, 0, 40, 40, 40, 40, 96.0, 96.0,
                                     Colors::Color(0xffffffff), nullptr, nullptr, true, {}, false, 6, 8, 1, 0);
    ASSERT_EQ(result, EXPORT_OK);
    std::unique_ptr<GdkPixbuf, decltype(&g_object_unref)> pb(gdk_pixbuf_new_from_file(file.c_str(), nullptr), g_object_unref);
    ASSERT_TRUE(pb);
    auto const *px = gdk_pixbuf_get_pixels(pb.get()) + 20 * gdk_pixbuf_get_rowstride(pb.get()) + 20 * 4;
    EXPECT_EQ(px[0], 255);
    EXPECT_EQ(px[1], 0);
    EXPECT_EQ(px[2], 0);
    EXPECT_EQ(view.viewPixel(), GREEN);
    EXPECT_TRUE(arena_clean(doc.get(), view.key + 1, "img")); // no leftover arena item on any later key
}

// T18: forced failures report, clean up and leave the document editable.
class RenderFailure : public ::testing::TestWithParam<cairo_surface_t *(*)(int, int)> {};

TEST_P(RenderFailure, FailedOutcomeWithFullCleanup)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    Budget budget(Budget::FixedLimitForTest{}, 256 * Bitmap::MiB);
    auto req = request(doc.get());
    req.create_surface = GetParam();
    unsigned key = 0;
    {
        auto out = Bitmap::renderCommitted(req, budget);
        key = out.output_key;
        EXPECT_FALSE(out.ok());
        EXPECT_EQ(out.outcome.status, Status::failed);
        EXPECT_FALSE(out.pixbuf); // never a blank success
        EXPECT_STRNE(out.outcome.diagnostic, "");
        EXPECT_EQ(budget.reserved(), 0u); // reservation released
    }
    EXPECT_NE(key, 0u);
    EXPECT_TRUE(arena_clean(doc.get(), key, "img")); // invoke_hide ran

    // Later edit and render are safe, and succeed.
    auto *image = cast<SPImage>(doc->getObjectById("img"));
    ASSERT_TRUE(image);
    image->setAttribute("x", "0");
    doc->ensureUpToDate();
    auto ok = Bitmap::renderCommitted(request(doc.get()), budget);
    ASSERT_TRUE(ok.ok()) << ok.outcome.diagnostic;
    EXPECT_EQ(pixel_at(*ok.pixbuf, 20, 20), RED);
}

INSTANTIATE_TEST_SUITE_P(Injected, RenderFailure, ::testing::Values(&null_surface, &error_surface, &throwing_surface, &oom_surface));

TEST(ExplodeBitmapOutput, BudgetRefusalAndRelease)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    {
        Budget tiny(Budget::FixedLimitForTest{}, 100); // a 40x40 ARGB buffer needs 6400 bytes
        auto out = Bitmap::renderCommitted(request(doc.get()), tiny);
        EXPECT_FALSE(out.ok());
        EXPECT_EQ(out.outcome.status, Status::failed);
        EXPECT_EQ(out.output_key, 0u); // refused before anything was shown
        EXPECT_EQ(tiny.reserved(), 0u);
    }
    {
        Budget budget(Budget::FixedLimitForTest{}, 256 * Bitmap::MiB);
        {
            auto out = Bitmap::renderCommitted(request(doc.get()), budget);
            ASSERT_TRUE(out.ok());
            // The 6400-byte bitmap plus the intermediate scratch ceiling (32 MiB floor) stay reserved.
            EXPECT_EQ(budget.reserved(), 6400u + 32 * Bitmap::MiB);
            EXPECT_EQ(out.token.bytes(), budget.reserved());
        }
        EXPECT_EQ(budget.reserved(), 0u);
    }
}

TEST(ExplodeBitmapOutput, EmptyAreaAndOversizeAreRefused)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    auto req = request(doc.get());
    req.area = Geom::Rect(Geom::Point(0, 0), Geom::Point(0, 40));
    EXPECT_FALSE(Bitmap::renderCommitted(req, roomy()).ok());
    req = request(doc.get());
    req.dpi = 96.0 * 2000; // 80000 px: beyond Cairo's surface limit
    EXPECT_FALSE(Bitmap::renderCommitted(req, roomy()).ok());
    EXPECT_EQ(roomy().reserved(), 0u);
    EXPECT_EQ(sp_generate_internal_bitmap(doc.get(), req.area, req.dpi), nullptr);
}

TEST(ExplodeBitmapOutput, PatternRendersThroughCommittedKey)
{
    auto doc = make_doc(PATTERN_SVG);
    ASSERT_TRUE(doc);
    auto out = Bitmap::renderCommitted(request(doc.get()), roomy());
    ASSERT_TRUE(out.ok()) << out.outcome.diagnostic;
    EXPECT_EQ(pixel_at(*out.pixbuf, 2, 2), GREEN);
    EXPECT_EQ(pixel_at(*out.pixbuf, 15, 15), 0u); // transparent between tiles' content
}

TEST(ExplodeBitmapOutput, PngExportReportsFailureAndLeavesNoFile)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    TempDir dir;
    auto file = dir.path.string(); // a directory: the writer cannot open it for writing
    ExportResult result;
    {
        result = sp_export_png_file(doc.get(), file.c_str(), 0, 0, 40, 40, 40, 40, 96.0, 96.0,
                                    Colors::Color(0xffffffff), nullptr, nullptr, true, {}, false, 6, 8, 1, 0);
    }
    EXPECT_EQ(result, EXPORT_ERROR);
    EXPECT_TRUE(std::filesystem::is_directory(file)); // the failure path did not remove it

    // The document is still usable afterwards: the same export to a valid path succeeds.
    file = (dir.path / "ok.png").string();
    result = sp_export_png_file(doc.get(), file.c_str(), 0, 0, 40, 40, 40, 40, 96.0, 96.0,
                                Colors::Color(0xffffffff), nullptr, nullptr, true, {}, false, 6, 8, 1, 0);
    EXPECT_EQ(result, EXPORT_OK);
    EXPECT_TRUE(std::filesystem::exists(file));
}

TEST(ExplodeBitmapOutput, PngExportCancelledMidwayLeavesNoPartialFile)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    TempDir dir;
    auto file = (dir.path / "cancel.png").string();
    // Stripes are 64 rows; a 200 px tall export gets several, and the second request is refused.
    auto result = sp_export_png_file(doc.get(), file.c_str(), 0, 0, 40, 40, 40, 200, 96.0, 96.0,
                                     Colors::Color(0xffffffff),
                                     [](float progress, void *) -> unsigned { return progress > 0.0f ? 0 : 1; },
                                     nullptr, true, {}, false, 6, 8, 1, 0);
    EXPECT_NE(result, EXPORT_OK);
    EXPECT_FALSE(std::filesystem::exists(file));
    EXPECT_TRUE(arena_clean(doc.get(), 0, "img")); // key 0 is never used; no view leaked by the cancel
}

// ---- Round 2: context status, cleanup failure, tile bound, real entry points ----

TEST(ExplodeBitmapOutput, ContextErrorIsNotASilentBlankSuccess)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    uint32_t const checker = 0xff808080;
    auto req = request(doc.get());
    req.checkerboard_color = &checker;
    req.device_scale = 0.0; // singular transform: the cairo_t errors while the surface stays fine
    auto out = Bitmap::renderCommitted(req, roomy());
    EXPECT_FALSE(out.ok());
    EXPECT_FALSE(out.pixbuf);
    EXPECT_EQ(roomy().reserved(), 0u);
}

namespace { int hide_calls = 0; void hide_fails_once() { if (hide_calls++ == 0) throw std::bad_alloc(); }
            void hide_always_fails() { throw std::bad_alloc(); } }

TEST(ExplodeBitmapOutput, InvokeHideFailureIsRetriedThenReported)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    Budget budget(Budget::FixedLimitForTest{}, 256 * Bitmap::MiB);
    auto req = request(doc.get());

    hide_calls = 0;
    req.hide_probe = &hide_fails_once; // transient: the retry succeeds, the render is good
    {
        auto out = Bitmap::renderCommitted(req, budget);
        ASSERT_TRUE(out.ok()) << out.outcome.diagnostic;
        EXPECT_EQ(pixel_at(*out.pixbuf, 20, 20), RED);
        EXPECT_TRUE(arena_clean(doc.get(), out.output_key, "img"));
    }
    EXPECT_EQ(budget.reserved(), 0u);

    req.hide_probe = &hide_always_fails; // persistent: no pixels are returned, failure is reported
    unsigned key = 0;
    {
        auto out = Bitmap::renderCommitted(req, budget);
        key = out.output_key;
        EXPECT_FALSE(out.ok());
        EXPECT_FALSE(out.pixbuf);
        EXPECT_EQ(out.outcome.status, Status::failed);
    }
    EXPECT_EQ(budget.reserved(), 0u);
    // The Drawing was intentionally leaked, so the view is still safe to use; detach it properly.
    auto *image = cast<SPItem>(doc->getObjectById("img"));
    ASSERT_TRUE(image);
    EXPECT_NE(image->get_arenaitem(key), nullptr);
    doc->getRoot()->invoke_hide(key);
    EXPECT_EQ(image->get_arenaitem(key), nullptr);
}

TEST(ExplodeBitmapOutput, TokenOutlivesPixbufStorage)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    Budget budget(Budget::FixedLimitForTest{}, 256 * Bitmap::MiB);
    auto out = Bitmap::renderCommitted(request(doc.get()), budget);
    ASSERT_TRUE(out.ok());
    // Reservation must be held while the pixels live; releasing the pixbuf first keeps it held.
    out.pixbuf.reset();
    EXPECT_GT(budget.reserved(), 0u);
    out.token.release();
    EXPECT_EQ(budget.reserved(), 0u);
}

// An anisotropic pattern rotated 45 degrees maps a 40x40 px output to a multi-megapixel tile-space
// rectangle even though only 6400 bytes were requested; the first tile must be refused.
TEST(ExplodeBitmapOutput, OversizedFirstPatternTileIsRefusedNotAllocated)
{
    auto doc = make_doc(
        "<svg xmlns='http://www.w3.org/2000/svg' width='40' height='40' viewBox='0 0 40 40'>"
        "<defs><pattern id='p' width='20' height='20' patternUnits='userSpaceOnUse' "
        "patternTransform='scale(100000,1) rotate(45)'><rect width='10' height='10' fill='#00ff00'/></pattern></defs>"
        "<rect id='r' width='40' height='40' style='fill:url(#p)'/></svg>");
    ASSERT_TRUE(doc);
    Budget budget(Budget::FixedLimitForTest{}, 256 * Bitmap::MiB);
    unsigned key = 0;
    {
        auto out = Bitmap::renderCommitted(request(doc.get()), budget);
        key = out.output_key;
        EXPECT_FALSE(out.ok());
        EXPECT_EQ(out.outcome.status, Status::failed);
        EXPECT_STREQ(out.outcome.diagnostic, "Intermediate render memory limit exceeded");
    }
    EXPECT_EQ(budget.reserved(), 0u);
    EXPECT_TRUE(arena_clean(doc.get(), key, "r"));
    // Legacy callers see the exception they always saw for render failures.
    EXPECT_ANY_THROW(sp_generate_internal_bitmap(doc.get(), request(doc.get()).area, 96.0));
}

// Make Bitmap Copy through the real ObjectSet entry point with a view preview installed.
TEST(ExplodeBitmapOutput, MakeBitmapCopyEntryPointUsesCommittedPixels)
{
    auto doc = make_doc(IMAGE_SVG);
    ASSERT_TRUE(doc);
    PreviewView view(doc.get(), GREEN);
    ObjectSet set(doc.get());
    set.set(doc->getObjectById("img"));
    BitmapCopyOptions options;
    options.dpi = 96;
    set.createBitmapCopy(options);
    auto *copy = cast<SPImage>(set.single());
    ASSERT_TRUE(copy);
    ASSERT_TRUE(copy->pixbuf);
    EXPECT_EQ(pixel_at(*copy->pixbuf, 20, 20), RED);
    EXPECT_EQ(view.viewPixel(), GREEN);
}

#ifdef CAIRO_HAS_PDF_SURFACE
// PDF through the real renderer: a rasterised filter succeeds normally and fails loudly when it cannot.
TEST(ExplodeBitmapOutput, PdfFilterRasterisationReportsFailure)
{
    auto doc = make_doc(
        "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='100' viewBox='0 0 400 100'>"
        "<defs><filter id='b'><feGaussianBlur stdDeviation='4'/></filter></defs>"
        "<rect id='blurred' width='400' height='100' style='fill:#ff0000;filter:url(#b)'/></svg>");
    ASSERT_TRUE(doc);
    TempDir dir;
    using namespace Inkscape::Extension::Internal;
    for (int res : {96, 10000}) {
        CairoRenderer renderer;
        CairoRenderContext ctx = renderer.createContext();
        ctx.setPDFLevel(0);
        ctx.setFilterToBitmap(true);
        ctx.setBitmapResolution(res);
        auto const pdf = (dir.path / ("r" + std::to_string(res) + ".pdf")).string();
        ASSERT_TRUE(ctx.setPdfTarget(("> " + pdf).c_str()));
        ASSERT_TRUE(renderer.setupDocument(&ctx, doc.get(), doc->getRoot()));
        renderer.renderPages(&ctx, doc.get(), false);
        bool const finished = ctx.finish();
        EXPECT_EQ(finished, res == 96) << renderer.failure();
        if (res != 96) EXPECT_NE(renderer.failure().find("blurred"), std::string::npos);
    }
}
#endif

// ---- Byte-identity golden: PNG export of ordinary documents vs the code BEFORE EB3-output ----
//
// Method: the references are SHA-256 hashes of PNG files produced by the pre-change
// sp_export_png_file. They were generated by compiling `git show HEAD:src/helper/png-write.cpp`
// from a scratch copy into this very test executable (its definition overrides the library one),
// running with EB3_GOLDEN_DUMP=<dir>, which writes the PNGs and prints "name sha256" lines. The
// same test run against the new code compares those hashes. Mac-only (fonts, libpng/zlib build).
namespace {
struct GoldenCase { char const *name; std::string svg; double dpi; bool interlace; int color_type; int depth; };

std::string svg_doc(std::string const &body, int w = 60, int h = 40)
{
    return "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' width='" +
           std::to_string(w) + "' height='" + std::to_string(h) + "' viewBox='0 0 " + std::to_string(w) + " " +
           std::to_string(h) + "'>" + body + "</svg>";
}

std::vector<std::pair<std::string, std::string>> golden_documents()
{
    return {
        {"shapes", svg_doc("<rect x='4' y='4' width='30' height='20' rx='3' style='fill:#3366cc;stroke:#000;stroke-width:2'/>"
                           "<circle cx='44' cy='24' r='10' style='fill:#ffcc00;stroke:#cc0000;stroke-width:1.5;opacity:.8'/>"
                           "<path d='M5 38 L55 36' style='stroke:#009900;stroke-width:1;stroke-dasharray:3 2;fill:none'/>")},
        {"text", svg_doc("<text x='3' y='26' style='font-family:sans-serif;font-size:16px;fill:#222'>Ab12</text>")},
        {"gradient", svg_doc("<defs><linearGradient id='l'><stop offset='0' stop-color='#f00'/><stop offset='1' stop-color='#00f'/></linearGradient>"
                             "<radialGradient id='q'><stop offset='0' stop-color='#fff'/><stop offset='1' stop-color='#080' stop-opacity='.3'/></radialGradient></defs>"
                             "<rect width='60' height='20' fill='url(#l)'/><circle cx='30' cy='30' r='10' fill='url(#q)'/>")},
        {"bitmap", svg_doc(std::string("<image x='5' y='3' width='38' height='30' preserveAspectRatio='none' transform='rotate(7 20 20)' "
                                       "xlink:href='data:image/png;base64,") + RED_PNG + "'/>")},
        {"filters", svg_doc("<defs><filter id='b'><feGaussianBlur stdDeviation='2'/></filter>"
                            "<filter id='s' x='-.3' y='-.3' width='1.7' height='1.7'><feGaussianBlur in='SourceAlpha' stdDeviation='1.5'/><feOffset dx='2' dy='2'/>"
                            "<feMerge><feMergeNode/><feMergeNode in='SourceGraphic'/></feMerge></filter></defs>"
                            "<rect x='4' y='4' width='24' height='24' style='fill:#c00;filter:url(#b)'/>"
                            "<rect x='34' y='8' width='20' height='20' style='fill:#06c;filter:url(#s)'/>")},
        {"pattern", svg_doc("<defs><pattern id='p' width='10' height='10' patternUnits='userSpaceOnUse' patternTransform='rotate(20)'>"
                            "<rect width='5' height='5' fill='#0a0'/><circle cx='7' cy='7' r='2' fill='#a0a'/></pattern></defs>"
                            "<rect width='60' height='40' style='fill:url(#p)'/>")},
        {"clipmask", svg_doc("<defs><clipPath id='c'><circle cx='20' cy='20' r='14'/></clipPath>"
                             "<mask id='m'><rect width='60' height='40' fill='#fff'/><circle cx='42' cy='20' r='10' fill='#444'/></mask></defs>"
                             "<rect width='60' height='40' style='fill:#c60;clip-path:url(#c)'/>"
                             "<rect x='25' width='35' height='40' style='fill:#06c;mask:url(#m)'/>")},
        {"wide", svg_doc("<rect width='40000' height='4' fill='#f00'/><rect x='32760' width='20' height='4' fill='#00f'/>", 40000, 4)},
    };
}

std::string sha256_file(std::string const &path)
{
    gchar *contents = nullptr; gsize len = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &len, nullptr)) return "";
    auto *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<guchar *>(contents), len);
    std::string out = sum; g_free(sum); g_free(contents);
    return out;
}

// SHA-256 of the PNG bytes written by the pre-change exporter ("name@dpi[/variant]").
std::map<std::string, std::string> const &golden_hashes()
{
    static std::map<std::string, std::string> const hashes = {
        {"shapes@96", "f8f06d4c8f923a6f3e9d18bfd71dc8d6374e79451667997975847e10cdc67d40"},
        {"shapes@300", "330adfcab568ac8ff6caef1e2706231e590b5f2d0f6d4a85a6b3e4ca4e6316b9"},
        {"shapes@96/interlaced", "8c6d521a648c620320e9b54f3569e24bd7fd0b4577808ea553885541392e948f"},
        {"shapes@96/gray", "0de1f54202893033cf9d9e283c96b2a6ecb5bc74ef4ae32d22a9a35a40cce544"},
        {"text@96", "655190e0277a8ba8257ae7c38ac89a9a12dcb5ecf5704a2aca3a33102289cdaf"},
        {"text@300", "e02586ead68d0685f137664a367fcfb529c721d34b48f2f120448c2467ef16b6"},
        {"gradient@96", "0ffac674951e6b7d98ef70e998b829d92dc973a219877db5d46d8e189b195824"},
        {"gradient@300", "51f657b7d44e43b0028d497d333a96a4914e7a0be6eaf4035c2b4e0cd08740d3"},
        {"bitmap@96", "82b1db7591f4df9dc37742f797244fb2bfbe26b72abfd0e67214bc122664fe25"},
        {"bitmap@300", "b7925a14e5feaadc1f27545b435f4a0c55637c9065f9ed8a37d5a739ba67051e"},
        {"filters@96", "a0d0013c2e5e1c3470ad22c0ebaee098a11e644c72eed95987344dc72a197165"},
        {"filters@300", "992253b305b36341938680eae445ba0e347c7caab4d55620bb02746bc4d7629b"},
        {"pattern@96", "1fd34785cf074596b3b7996db973edfa71441cbb0b2c186ea9cbf980c42830f6"},
        {"pattern@300", "9d87b940624f06d01e4cfb4b7b851b2f8c5ea085cb3e42212ce8024ff57b25b8"},
        {"clipmask@96", "a087155f79f43319e211883546e8fe29370fafcdd2c8a7db9fa9073fbf91e10f"},
        {"clipmask@300", "546792bef5b08134b3ef09205d176d5165d0fa1e0b824ef5ccbb4961f5ba6a1a"},
        {"wide@96", "2ccf320c21fd36df1dd2107c962665c08d1a13e417e71ac6bbc124a0e8c90d4d"},
    };
    return hashes;
}
} // namespace

TEST(ExplodeBitmapOutput, PngExportBytesMatchPreChangeExporter)
{
#if !defined(__APPLE__)
    GTEST_SKIP() << "golden hashes were generated on macOS";
#endif
    TempDir dir;
    char const *dump = g_getenv("EB3_GOLDEN_DUMP");
    struct Variant { char const *tag; double dpi; bool interlace; int color_type; };
    std::vector<Variant> variants = {{"96", 96, false, 6}, {"300", 300, false, 6}};
    int compared = 0;
    for (auto const &[name, svg] : golden_documents()) {
        auto doc = make_doc(svg);
        ASSERT_TRUE(doc) << name;
        double const w = doc->getWidth().value("px"), h = doc->getHeight().value("px");
        auto run = [&](std::string const &key, double dpi, bool interlace, int color_type) {
            unsigned long const pw = static_cast<unsigned long>(std::ceil(w * dpi / 96.0));
            unsigned long const ph = static_cast<unsigned long>(std::ceil(h * dpi / 96.0));
            auto file = (dir.path / (key + ".png")).string();
            std::replace(file.begin() + dir.path.string().size(), file.end(), '/', '_');
            ASSERT_EQ(sp_export_png_file(doc.get(), file.c_str(), 0, 0, w, h, pw, ph, dpi, dpi,
                                         Colors::Color(0xffffffff), nullptr, nullptr, true, {}, interlace, color_type, 8, 6, 0),
                      EXPORT_OK) << key;
            auto hash = sha256_file(file);
            if (dump) {
                printf("        {\"%s\", \"%s\"},\n", key.c_str(), hash.c_str());
            } else {
                auto it = golden_hashes().find(key);
                ASSERT_NE(it, golden_hashes().end()) << key;
                EXPECT_EQ(hash, it->second) << key << " differs from the pre-change exporter";
                ++compared;
            }
        };
        if (std::string(name) == "wide") { run(std::string(name) + "@96", 96, false, 6); continue; }
        for (auto const &v : variants) run(std::string(name) + "@" + v.tag, v.dpi, v.interlace, v.color_type);
        if (std::string(name) == "shapes") {
            run("shapes@96/interlaced", 96, true, 6);
            run("shapes@96/gray", 96, false, 0);
        }
    }
    if (!dump) EXPECT_EQ(compared, 8 * 2 - 1 + 2);
}
