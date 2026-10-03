// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui/tools/bitmap-eraser.h"
#include "ui/tools/bitmap-eraser-tool.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cairomm/surface.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glibmm/main.h>
#include <gtest/gtest.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <2geom/rect.h>

#include "desktop.h"
#include "display/cairo-utils.h"
#include "display/control/canvas-item-drawing.h"
#include "display/drawing-context.h"
#include "display/drawing-item.h"
#include "display/drawing-surface.h"
#include "display/drawing.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape.h"
#include "object/sp-image.h"
#include "preferences.h"
#include "selection.h"
#include "ui/toolbar/bitmap-eraser-toolbar.h"
#include "ui/tools/eraser-tool.h"
#include "ui/widget/gtk-registry.h"
#include "ui/widget/events/canvas-event.h"
#include "xml/href-attribute-helper.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::UI::Tools;

namespace {

std::unique_ptr<Pixbuf> solid_pixbuf(int width, int height, guint32 rgba)
{
    auto *raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    if (!raw)
        return {};
    gdk_pixbuf_fill(raw, rgba);
    return std::make_unique<Pixbuf>(raw); // Pixbuf takes ownership of raw.
}

std::string solid_png_uri(int width, int height, guint32 rgba)
{
    auto pixels = solid_pixbuf(width, height, rgba);
    if (!pixels)
        return {};

    gchar *png = nullptr;
    gsize size = 0;
    if (!gdk_pixbuf_save_to_buffer(pixels->getPixbufRaw(), &png, &size, "png", nullptr, nullptr) || !png) {
        if (png)
            g_free(png);
        return {};
    }

    auto *base64 = g_base64_encode(reinterpret_cast<guchar const *>(png), size);
    g_free(png);
    if (!base64)
        return {};

    std::string result = "data:image/png;base64,";
    result += base64;
    g_free(base64);
    return result;
}

Geom::PathVector rect_path(double left, double top, double right, double bottom)
{
    Geom::PathVector result;
    result.push_back(Geom::Path(Geom::Rect(left, top, right, bottom)));
    return result;
}

unsigned alpha_at(Pixbuf const &pixbuf, int x, int y)
{
    Pixbuf copy(pixbuf);
    copy.ensurePixelFormat(Pixbuf::PF_GDK);
    return copy.pixels()[static_cast<std::size_t>(y) * copy.rowstride() + static_cast<std::size_t>(x) * 4 + 3];
}

uint64_t desktop_checksum(SPDesktop *desktop, int width = 30, int height = 12)
{
    auto *canvas_drawing = desktop ? desktop->getCanvasDrawing() : nullptr;
    auto *drawing = canvas_drawing ? canvas_drawing->get_drawing() : nullptr;
    if (!drawing)
        return 0;

    drawing->update();
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, width, height);
    DrawingSurface target(surface->cobj(), Geom::IntPoint(0, 0));
    DrawingContext context(target);
    drawing->render(context, Geom::IntRect::from_xywh(0, 0, width, height));
    surface->flush();

    uint64_t result = 1469598103934665603ULL;
    auto const size = static_cast<std::size_t>(surface->get_stride()) * surface->get_height();
    auto const *data = surface->get_data();
    for (std::size_t i = 0; i < size; ++i) {
        result ^= data[i];
        result *= 1099511628211ULL;
    }
    return result;
}

InkscapeApplication *initialize_gui()
{
    static auto *app = [] {
        g_setenv("INKSCAPE_APP_ID_TAG", "bitmaperasertest", TRUE);
        auto *result = new InkscapeApplication(); // Process-lifetime test fixture.
        if (result->gtk_app()) {
            UI::Widget::register_all();
        }
        return result;
    }();
    return app->gtk_app() ? app : nullptr;
}

// Makes one named stage of the sp-image copy/encode path fail as if out of memory.
struct AllocationFailure
{
    explicit AllocationFailure(char const *failing_stage)
    {
        stage() = failing_stage;
        sp_image_set_allocation_hook(&AllocationFailure::allow);
    }
    ~AllocationFailure() { sp_image_set_allocation_hook(nullptr); }
    static std::string &stage()
    {
        static std::string value;
        return value;
    }
    static bool allow(char const *requested) { return stage() != requested; }
};

// Straight-alpha GDK pixbuf whose alpha runs over the edge values 0, 1, 127, 128, 254, 255.
std::unique_ptr<Pixbuf> alpha_edge_pixbuf()
{
    auto *raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 8, 3);
    if (!raw)
        return {};
    static constexpr guint8 alphas[8] = {0, 1, 2, 127, 128, 254, 255, 64};
    auto *pixels = gdk_pixbuf_get_pixels(raw);
    auto const stride = static_cast<std::size_t>(gdk_pixbuf_get_rowstride(raw));
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 8; ++x) {
            auto *pixel = pixels + y * stride + static_cast<std::size_t>(x) * 4;
            pixel[0] = static_cast<guint8>(31 * x + 7 * y + 13);
            pixel[1] = static_cast<guint8>(200 - 17 * x + y);
            pixel[2] = static_cast<guint8>(5 + 29 * x * (y + 1));
            pixel[3] = alphas[(x + y) % 8];
        }
    }
    return std::make_unique<Pixbuf>(raw);
}

std::vector<unsigned char> gdk_bytes(Pixbuf const &source)
{
    auto copy = sp_image_try_copy_pixbuf(source);
    EXPECT_TRUE(copy);
    if (!copy)
        return {};
    copy->ensurePixelFormat(Pixbuf::PF_GDK);
    std::vector<unsigned char> bytes;
    for (int y = 0; y < copy->height(); ++y) {
        auto const *row = copy->pixels() + static_cast<std::size_t>(y) * copy->rowstride();
        bytes.insert(bytes.end(), row, row + static_cast<std::size_t>(copy->width()) * 4);
    }
    return bytes;
}

void drain_main_context()
{
    auto context = Glib::MainContext::get_default();
    while (context->iteration(false)) {
    }
}

class BitmapEraseSessionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!initialize_gui()) {
            GTEST_SKIP() << "GTK display unavailable; bitmap eraser session fixture skipped";
        }
        if (!Application::exists())
            Application::create(false);

        source_uri = solid_png_uri(8, 8, 0x336699ff);
        ASSERT_FALSE(source_uri.empty());
        auto const svg = Glib::ustring::compose(
            R"svg(<svg xmlns="http://www.w3.org/2000/svg"
 xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" width="30" height="12">
 <image id="one" width="8" height="8" preserveAspectRatio="none"
        sodipodi:absref="/tmp/original.png" href="%1"/>
 <image id="two" x="10" width="8" height="8" preserveAspectRatio="none" href="%1"/>
 <rect id="vector" x="20" width="8" height="8" fill="#f00"/>
</svg>)svg",
            source_uri);
        document = SPDocument::createNewDocFromMem(svg.raw());
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        ASSERT_TRUE(desktop);
        ASSERT_TRUE(image("one"));
        ASSERT_TRUE(image("two"));
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Initialize bitmap eraser fixture"},
                           "document-new");
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        document->setModifiedSinceSave(false);
    }

    void TearDown() override
    {
        desktop.reset();
        document.reset();
        drain_main_context();
    }

    SPImage *image(char const *id) const { return document ? cast<SPImage>(document->getObjectById(id)) : nullptr; }

    Geom::PathVector pixel_rect_on_desktop(SPImage *target, double left, double top, double right, double bottom) const
    {
        auto transform = target->pixelToDocumentAffine();
        EXPECT_TRUE(transform);
        if (!transform)
            return {};
        return rect_path(left, top, right, bottom) * (*transform * desktop->doc2dt());
    }

    std::string href(SPImage *target) const
    {
        auto const value = getHrefAttribute(*target->getRepr()).second;
        return value ? value : "";
    }

    BitmapEraserTool *activate_bitmap_eraser(double size = 4.0, int shape = 0,
                                              double hardness = 100.0,
                                              bool use_pressure = false)
    {
        auto *prefs = Preferences::get();
        prefs->setDouble("/tools/bitmaperaser/size", size);
        prefs->setDouble("/tools/bitmaperaser/hardness", hardness);
        prefs->setDouble("/tools/bitmaperaser/spacing", 0.18);
        prefs->setInt("/tools/bitmaperaser/shape", shape);
        prefs->setBool("/tools/bitmaperaser/usepressure", use_pressure);
        desktop->setTool("/tools/bitmaperaser");
        return dynamic_cast<BitmapEraserTool *>(desktop->getTool());
    }

    EraserTool *activate_eraser(int mode)
    {
        auto *prefs = Preferences::get();
        prefs->setInt("/tools/eraser/mode", mode);
        prefs->setDouble("/tools/eraser/width", 15.0);
        prefs->setDouble("/tools/eraser/mass", 0.0);
        prefs->setDouble("/tools/eraser/wiggle", 0.0);
        prefs->setDouble("/tools/eraser/angle", 30.0);
        prefs->setDouble("/tools/eraser/thinning", 0.0);
        prefs->setDouble("/tools/eraser/tremor", 0.0);
        prefs->setDouble("/tools/eraser/flatness", 0.0);
        prefs->setDouble("/tools/eraser/cap_rounding", 1.0);
        prefs->setBool("/tools/eraser/usepressure", false);
        prefs->setBool("/tools/eraser/usetilt", false);
        prefs->setBool("/tools/eraser/abs_width", false);
        desktop->setTool("/tools/eraser");
        return dynamic_cast<EraserTool *>(desktop->getTool());
    }

    Geom::Point pixel_to_world(SPImage *target, Geom::Point const &pixel) const
    {
        auto transform = target->pixelToDocumentAffine();
        EXPECT_TRUE(transform);
        if (!transform)
            return {};
        return desktop->d2w(pixel * *transform * desktop->doc2dt());
    }

    template <typename Tool>
    bool press(Tool *tool, Geom::Point const &world, std::optional<double> pressure = std::nullopt)
    {
        ButtonPressEvent event;
        event.pos = event.orig_pos = world;
        event.button = 1;
        event.num_press = 1;
        event.extinput.pressure = pressure;
        return tool->root_handler(event);
    }

    template <typename Tool>
    bool move(Tool *tool, Geom::Point const &world, std::optional<double> pressure = std::nullopt)
    {
        MotionEvent event;
        event.pos = world;
        event.modifiers = GDK_BUTTON1_MASK;
        event.extinput.pressure = pressure;
        return tool->root_handler(event);
    }

    template <typename Tool>
    bool release(Tool *tool, Geom::Point const &world)
    {
        ButtonReleaseEvent event;
        event.pos = event.orig_pos = world;
        event.button = 1;
        event.modifiers = GDK_BUTTON1_MASK;
        return tool->root_handler(event);
    }

    std::string source_uri;
    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
};

} // namespace

TEST(BitmapEraserPixelsTest, ErasesCoveredPixelsAndPreservesOutside)
{
    auto pixels = solid_pixbuf(8, 8, 0x336699ff);
    ASSERT_TRUE(pixels);

    EXPECT_TRUE(erase_bitmap_path(*pixels, rect_path(2, 2, 6, 6)));
    EXPECT_EQ(alpha_at(*pixels, 4, 4), 0u);
    EXPECT_EQ(alpha_at(*pixels, 0, 0), 255u);
    EXPECT_EQ(alpha_at(*pixels, 7, 7), 255u);
}

TEST(BitmapEraserPixelsTest, NoIntersectionAndTransparentInputAreNoOps)
{
    auto opaque = solid_pixbuf(8, 8, 0x336699ff);
    auto opaque_before = std::make_unique<Pixbuf>(*opaque);
    ASSERT_TRUE(opaque);
    ASSERT_TRUE(opaque_before);
    EXPECT_FALSE(erase_bitmap_path(*opaque, rect_path(20, 20, 25, 25)));
    EXPECT_TRUE(bitmap_pixels_equal(*opaque, *opaque_before));

    auto transparent = solid_pixbuf(8, 8, 0x33669900);
    auto transparent_before = std::make_unique<Pixbuf>(*transparent);
    ASSERT_TRUE(transparent);
    ASSERT_TRUE(transparent_before);
    EXPECT_FALSE(erase_bitmap_path(*transparent, rect_path(2, 2, 6, 6)));
    EXPECT_TRUE(bitmap_pixels_equal(*transparent, *transparent_before));
}

TEST(BitmapEraserPixelsTest, PreservesExistingPartialTransparencyOutsideStroke)
{
    auto pixels = solid_pixbuf(8, 8, 0x99663380);
    ASSERT_TRUE(pixels);

    EXPECT_TRUE(erase_bitmap_path(*pixels, rect_path(2, 2, 6, 6)));
    EXPECT_EQ(alpha_at(*pixels, 4, 4), 0u);
    EXPECT_EQ(alpha_at(*pixels, 0, 0), 128u);
}

TEST(BitmapEraserPixelsTest, LargeBitmapOnlyChangesTheBoundedStrokeRegion)
{
    auto pixels = solid_pixbuf(2048, 2048, 0x336699ff);
    ASSERT_TRUE(pixels);

    EXPECT_TRUE(erase_bitmap_path(*pixels, rect_path(1000, 1000, 1016, 1016)));
    EXPECT_EQ(alpha_at(*pixels, 1008, 1008), 0u);
    EXPECT_EQ(alpha_at(*pixels, 0, 0), 255u);
    EXPECT_EQ(alpha_at(*pixels, 2047, 2047), 255u);
}

TEST(BitmapEraserPixelsTest, RoundAndSquareTipsHaveDifferentCornerCoverage)
{
    auto round = solid_pixbuf(9, 9, 0x336699ff);
    auto square = solid_pixbuf(9, 9, 0x336699ff);
    ASSERT_TRUE(round);
    ASSERT_TRUE(square);

    DrawingImageEraseStamp stamp;
    stamp.unit_to_pixel = Geom::Scale(3.0) * Geom::Translate(4.5, 4.5);
    stamp.hardness = 1.0;
    stamp.shape = DrawingImageEraseShape::Round;
    ASSERT_TRUE(erase_bitmap_stamps(*round, std::span{&stamp, 1u}));

    stamp.shape = DrawingImageEraseShape::Square;
    ASSERT_TRUE(erase_bitmap_stamps(*square, std::span{&stamp, 1u}));

    EXPECT_EQ(alpha_at(*round, 4, 4), 0u);
    EXPECT_EQ(alpha_at(*square, 4, 4), 0u);
    EXPECT_GT(alpha_at(*round, 2, 2), alpha_at(*square, 2, 2));
}

TEST(BitmapEraserPixelsTest, SoftTipProducesPartialAlphaWithoutChangingOutside)
{
    auto pixels = solid_pixbuf(11, 11, 0x336699ff);
    ASSERT_TRUE(pixels);

    DrawingImageEraseStamp stamp;
    stamp.unit_to_pixel = Geom::Scale(4.0) * Geom::Translate(5.5, 5.5);
    stamp.shape = DrawingImageEraseShape::Round;
    stamp.hardness = 0.25;
    ASSERT_TRUE(erase_bitmap_stamps(*pixels, std::span{&stamp, 1u}));

    EXPECT_EQ(alpha_at(*pixels, 5, 5), 0u);
    EXPECT_GT(alpha_at(*pixels, 8, 5), 0u);
    EXPECT_LT(alpha_at(*pixels, 8, 5), 255u);
    EXPECT_EQ(alpha_at(*pixels, 0, 0), 255u);
}

TEST(BitmapEraserPixelsTest, PngEncodingRoundTripsErasedAlpha)
{
    auto pixels = solid_pixbuf(8, 8, 0x336699ff);
    ASSERT_TRUE(pixels);
    ASSERT_TRUE(erase_bitmap_path(*pixels, rect_path(2, 2, 6, 6)));

    auto encoded = sp_image_encode_png_data_uri(*pixels);
    ASSERT_TRUE(encoded);
    ASSERT_EQ(encoded->find("data:image/png;base64,"), 0u);
    std::unique_ptr<Pixbuf> decoded(Pixbuf::create_from_data_uri(encoded->c_str() + 5));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(alpha_at(*decoded, 4, 4), 0u);
    EXPECT_EQ(alpha_at(*decoded, 0, 0), 255u);
}

// ST-H: the banded erase must give exactly the pixels of one single fill.
TEST(BitmapEraserPixelsTest, BandedEraseMatchesASingleFillOnAWideRegion)
{
    auto pixels = solid_pixbuf(1000, 1000, 0x99663380);
    ASSERT_TRUE(pixels);
    auto reference = sp_image_try_copy_pixbuf(*pixels);
    ASSERT_TRUE(reference);

    Geom::Path triangle(Geom::Point(100.3, 50.7));
    triangle.appendNew<Geom::LineSegment>(Geom::Point(900.2, 300.9));
    triangle.appendNew<Geom::LineSegment>(Geom::Point(400.5, 950.1));
    triangle.close();
    Geom::PathVector path;
    path.push_back(triangle);

    // The region is ~800 px wide: its 1 MiB band budget forces several bands.
    EXPECT_TRUE(erase_bitmap_path(*pixels, path));

    reference->ensurePixelFormat(Pixbuf::PF_CAIRO);
    auto *surface = reference->getSurfaceRaw();
    cairo_surface_flush(surface);
    auto *cr = cairo_create(surface);
    feed_pathvector_to_cairo(cr, path);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);
    cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OUT);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    EXPECT_TRUE(bitmap_pixels_equal(*pixels, *reference));
    EXPECT_EQ(alpha_at(*pixels, 400, 300), 0u);
    EXPECT_EQ(alpha_at(*pixels, 5, 5), 128u);
}

// ST-H: comparing across pixel layouts neither copies an image nor converts a caller's buffer.
TEST(BitmapEraserPixelsTest, PixelEqualityAcrossLayoutsNeitherCopiesNorMutates)
{
    auto gdk_side = solid_pixbuf(16, 8, 0x99663380);
    ASSERT_TRUE(gdk_side);
    auto cairo_side = sp_image_try_copy_pixbuf(*gdk_side);
    ASSERT_TRUE(cairo_side);
    cairo_side->ensurePixelFormat(Pixbuf::PF_CAIRO);

    auto const copies = sp_image_pixbuf_copy_count();
    EXPECT_TRUE(bitmap_pixels_equal(*gdk_side, *cairo_side));
    EXPECT_TRUE(bitmap_pixels_equal(*cairo_side, *gdk_side));
    EXPECT_EQ(sp_image_pixbuf_copy_count(), copies);
    EXPECT_EQ(gdk_side->pixelFormat(), Pixbuf::PF_GDK);
    EXPECT_EQ(cairo_side->pixelFormat(), Pixbuf::PF_CAIRO);

    cairo_side->pixels()[3 * 4 + 3] ^= 1; // one alpha byte differs
    EXPECT_FALSE(bitmap_pixels_equal(*gdk_side, *cairo_side));
    EXPECT_FALSE(bitmap_pixels_equal(*cairo_side, *gdk_side));
}

// ST-H: encode then decode is byte-identical, including alpha edge values.
TEST(BitmapEraserPixelsTest, EncoderRoundTripIsPixelIdenticalForAlphaEdges)
{
    auto source = alpha_edge_pixbuf();
    ASSERT_TRUE(source);
    auto const expected = gdk_bytes(*source);

    auto const copies = sp_image_pixbuf_copy_count();
    auto by_reference = sp_image_encode_png_data_uri(*source); // GDK layout: no copy at all
    ASSERT_TRUE(by_reference);
    EXPECT_EQ(sp_image_pixbuf_copy_count(), copies);
    EXPECT_EQ(source->pixelFormat(), Pixbuf::PF_GDK);

    auto by_value = sp_image_encode_png_data_uri(sp_image_try_copy_pixbuf(*source));
    ASSERT_TRUE(by_value);
    EXPECT_EQ(*by_reference, *by_value);
    EXPECT_EQ(by_reference->find("data:image/png;base64,"), 0u);
    EXPECT_EQ(by_reference->find('\n'), std::string::npos);

    std::unique_ptr<Pixbuf> decoded(Pixbuf::create_from_data_uri(by_reference->c_str() + 5));
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded->width(), 8);
    ASSERT_EQ(decoded->height(), 3);
    EXPECT_EQ(gdk_bytes(*decoded), expected);

    // The same text as the reference implementation (PNG, then g_base64_encode).
    gchar *png = nullptr;
    gsize size = 0;
    ASSERT_TRUE(gdk_pixbuf_save_to_buffer(source->getPixbufRaw(), &png, &size, "png", nullptr, nullptr));
    auto *base64 = g_base64_encode(reinterpret_cast<guchar const *>(png), size);
    EXPECT_EQ(*by_reference, std::string("data:image/png;base64,") + base64);
    g_free(base64);
    g_free(png);
}

TEST(BitmapEraserPixelsTest, EncoderOfCairoLayoutInputLeavesItUntouchedAndAgreesWithDisposableOverload)
{
    auto source = alpha_edge_pixbuf();
    ASSERT_TRUE(source);
    auto cairo_layout = sp_image_try_copy_pixbuf(*source);
    ASSERT_TRUE(cairo_layout);
    cairo_layout->ensurePixelFormat(Pixbuf::PF_CAIRO);
    auto const before = std::vector<unsigned char>(
        cairo_layout->pixels(), cairo_layout->pixels() + static_cast<std::size_t>(cairo_layout->rowstride()) * 3);

    auto const by_reference = sp_image_encode_png_data_uri(*cairo_layout);
    ASSERT_TRUE(by_reference);
    EXPECT_EQ(cairo_layout->pixelFormat(), Pixbuf::PF_CAIRO);
    EXPECT_TRUE(std::equal(before.begin(), before.end(), cairo_layout->pixels()));

    auto const disposable = sp_image_encode_png_data_uri(sp_image_try_copy_pixbuf(*cairo_layout));
    ASSERT_TRUE(disposable);
    EXPECT_EQ(*by_reference, *disposable);
}

// ST-H review fix 4: pixels with alpha 0 are equal whatever colour they carry (as the
// old premultiplied comparison had it), in every layout combination.
TEST(BitmapEraserPixelsTest, PixelEqualityIgnoresTheColourOfFullyTransparentPixels)
{
    auto a = solid_pixbuf(4, 2, 0x11223300);
    auto b = solid_pixbuf(4, 2, 0xaabbcc00);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_TRUE(bitmap_pixels_equal(*a, *b)); // both GDK, alpha 0, different colours

    auto b_cairo = sp_image_try_copy_pixbuf(*b);
    ASSERT_TRUE(b_cairo);
    b_cairo->ensurePixelFormat(Pixbuf::PF_CAIRO);
    EXPECT_TRUE(bitmap_pixels_equal(*a, *b_cairo));
    EXPECT_TRUE(bitmap_pixels_equal(*b_cairo, *a));

    auto c = solid_pixbuf(4, 2, 0x11223301); // alpha 1 is visible
    ASSERT_TRUE(c);
    EXPECT_FALSE(bitmap_pixels_equal(*a, *c));
    auto d = solid_pixbuf(4, 2, 0x11223380);
    auto e = solid_pixbuf(4, 2, 0x11223380);
    EXPECT_TRUE(bitmap_pixels_equal(*d, *e));
    gdk_pixbuf_get_pixels(e->getPixbufRaw())[0] ^= 0x40; // visible colour change at alpha 128
    EXPECT_FALSE(bitmap_pixels_equal(*d, *e));
}

TEST(BitmapEraserPixelsTest, EncodeFailureReasonDistinguishesMemoryFromOtherFailures)
{
    auto source = alpha_edge_pixbuf();
    ASSERT_TRUE(source);
    ASSERT_TRUE(sp_image_encode_png_data_uri(*source));
    EXPECT_EQ(sp_image_last_encode_failure(), SPImageEncodeFailure::None);
    {
        AllocationFailure fail("png");
        EXPECT_FALSE(sp_image_encode_png_data_uri(*source));
        EXPECT_EQ(sp_image_last_encode_failure(), SPImageEncodeFailure::OutOfMemory);
    }
    {
        AllocationFailure fail("base64");
        EXPECT_FALSE(sp_image_encode_png_data_uri(*source));
        EXPECT_EQ(sp_image_last_encode_failure(), SPImageEncodeFailure::OutOfMemory);
    }
    EXPECT_FALSE(sp_image_encode_png_data_uri(std::unique_ptr<Pixbuf>())); // not a memory problem
    EXPECT_EQ(sp_image_last_encode_failure(), SPImageEncodeFailure::Other);
}

TEST(BitmapEraserPixelsTest, CopyOfEitherLayoutIsPixelIdenticalAndIndependent)
{
    auto source = alpha_edge_pixbuf();
    ASSERT_TRUE(source);
    for (auto format : {Pixbuf::PF_GDK, Pixbuf::PF_CAIRO}) {
        source->ensurePixelFormat(format);
        auto copy = sp_image_try_copy_pixbuf(*source);
        ASSERT_TRUE(copy);
        EXPECT_EQ(copy->pixelFormat(), format);
        EXPECT_TRUE(bitmap_pixels_equal(*copy, *source));
        copy->pixels()[3] ^= 0x10;
        EXPECT_FALSE(bitmap_pixels_equal(*copy, *source)); // independent storage
    }
}

TEST(BitmapEraserPixelsTest, InjectedAllocationFailureInTheEncoderDeclinesCleanly)
{
    auto source = alpha_edge_pixbuf();
    ASSERT_TRUE(source);
    {
        AllocationFailure fail("png");
        EXPECT_FALSE(sp_image_encode_png_data_uri(*source));
        EXPECT_FALSE(sp_image_encode_png_data_uri(sp_image_try_copy_pixbuf(*source)));
    }
    {
        AllocationFailure fail("base64");
        EXPECT_FALSE(sp_image_encode_png_data_uri(*source));
    }
    {
        AllocationFailure fail("copy");
        EXPECT_FALSE(sp_image_try_copy_pixbuf(*source));
        auto cairo_layout = alpha_edge_pixbuf();
        cairo_layout->ensurePixelFormat(Pixbuf::PF_CAIRO);
        EXPECT_FALSE(sp_image_encode_png_data_uri(*cairo_layout)); // needs a conversion copy
        EXPECT_EQ(cairo_layout->pixelFormat(), Pixbuf::PF_CAIRO);
    }
    EXPECT_TRUE(sp_image_encode_png_data_uri(*source)); // the hook is gone
}

#ifdef __APPLE__
namespace {
struct ResidentMemory
{
    std::size_t current = 0;
    std::size_t peak = 0;
};

ResidentMemory resident_memory()
{
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return {};
    return {static_cast<std::size_t>(info.resident_size), static_cast<std::size_t>(info.resident_size_peak)};
}
} // namespace

// ST-H benchmark. Run as a fresh process per mode because the peak never
// decreases:  VACARDS_ERASER_MEMORY_BENCH=old|new test_bitmap-eraser
//             --gtest_filter=BitmapEraserMemory.*
// "old" replays the pre-ST-H commit sequence with public APIs (copy,
// erase, two-copy comparison, copying encoder, text copies); "new" runs the
// current sequence. Without the variable the test does nothing.
TEST(BitmapEraserMemory, PeakExtraResidentMemoryOfACommitOnA4000Square)
{
    auto const *mode = std::getenv("VACARDS_ERASER_MEMORY_BENCH");
    if (!mode)
        return;
    constexpr int kSize = 4000;
    auto canonical = solid_pixbuf(kSize, kSize, 0x336699ff);
    ASSERT_TRUE(canonical);
    // Incompressible noise so the PNG is as large as in a photograph.
    std::uint32_t state = 2463534242u;
    for (int y = 0; y < kSize; ++y) {
        auto *row = canonical->pixels() + static_cast<std::size_t>(y) * canonical->rowstride();
        for (int x = 0; x < kSize * 4; ++x) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            row[x] = static_cast<unsigned char>(state >> 8);
            if (x % 4 == 3)
                row[x] = 255;
        }
    }
    auto const stroke = rect_path(500, 500, 3500, 3500);
    // A real commit also keeps the live-preview buffer (BitmapEraseSession
    // working copy) alive, so hold one here: absolute peak = baseline + extra.
    auto working = sp_image_try_copy_pixbuf(*canonical);
    ASSERT_TRUE(working);

    auto const baseline = resident_memory();
    std::string href;
    if (std::string(mode) == "old") {
        auto pixels = std::make_shared<Pixbuf>(*canonical);
        ASSERT_TRUE(erase_bitmap_path(*pixels, stroke));
        {   // old bitmap_pixels_equal: deep copies of both images
            Pixbuf acopy(*pixels);
            Pixbuf bcopy(*canonical);
            acopy.ensurePixelFormat(Pixbuf::PF_CAIRO);
            bcopy.ensurePixelFormat(Pixbuf::PF_CAIRO);
            ASSERT_NE(std::memcmp(acopy.pixels(), bcopy.pixels(), 64), 1234567); // keep the copies alive
        }
        Pixbuf copy(*pixels); // old encoder's internal copy
        copy.ensurePixelFormat(Pixbuf::PF_GDK);
        gchar *png = nullptr;
        gsize size = 0;
        ASSERT_TRUE(gdk_pixbuf_save_to_buffer(copy.getPixbufRaw(), &png, &size, "png", nullptr, nullptr));
        auto *base64 = g_base64_encode(reinterpret_cast<guchar const *>(png), size);
        g_free(png);
        std::string uri = "data:image/png;base64,";
        uri += base64;
        g_free(base64);
        href = uri; // the XML attribute's own copy
    } else {
        auto pixels = sp_image_try_copy_pixbuf(*canonical);
        ASSERT_TRUE(pixels);
        ASSERT_TRUE(erase_bitmap_path(*pixels, stroke));
        auto uri = sp_image_encode_png_data_uri(std::move(pixels));
        ASSERT_TRUE(uri);
        href = *uri; // the XML attribute's own copy
        std::string().swap(*uri);
    }
    auto const after = resident_memory();
    auto const extra = after.peak > baseline.current ? after.peak - baseline.current : 0;
    std::printf("MEMBENCH mode=%s image=%dx%d (%zu MB RGBA) href=%zu MB baseline(canonical+working)=%zu MB "
                "peak_extra_resident=%zu MB\n",
                mode, kSize, kSize, static_cast<std::size_t>(kSize) * kSize * 4 >> 20, href.size() >> 20,
                baseline.current >> 20, extra >> 20);
}
#endif

TEST(BitmapEraserMappingTest, IntrinsicPixelsMapThroughViewportAndItemTransform)
{
    if (!Application::exists())
        Application::create(false);
    auto const uri = solid_png_uri(8, 8, 0x336699ff);
    ASSERT_FALSE(uri.empty());
    auto const svg = Glib::ustring::compose(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
 <image id="image" x="10" y="20" width="40" height="16"
        preserveAspectRatio="none" transform="matrix(2 0 0 3 5 7)" href="%1"/>
</svg>)svg",
        uri);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto *image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    auto transform = image->pixelToDocumentAffine();
    ASSERT_TRUE(transform);
    auto const top_left = Geom::Point(0, 0) * *transform;
    auto const bottom_right = Geom::Point(8, 8) * *transform;
    EXPECT_NEAR(top_left.x(), 25.0, 1e-9);
    EXPECT_NEAR(top_left.y(), 67.0, 1e-9);
    EXPECT_NEAR(bottom_right.x(), 105.0, 1e-9);
    EXPECT_NEAR(bottom_right.y(), 115.0, 1e-9);
}

TEST(BitmapEraserMappingTest, DefaultAspectRatioMapsOnlyPaintedImageArea)
{
    if (!Application::exists())
        Application::create(false);
    auto const uri = solid_png_uri(8, 8, 0x336699ff);
    ASSERT_FALSE(uri.empty());
    auto const svg = Glib::ustring::compose(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
 <image id="image" x="10" y="20" width="40" height="16" href="%1"/>
</svg>)svg",
        uri);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto *image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    auto transform = image->pixelToDocumentAffine();
    ASSERT_TRUE(transform);
    auto const top_left = Geom::Point(0, 0) * *transform;
    auto const bottom_right = Geom::Point(8, 8) * *transform;
    EXPECT_NEAR(top_left.x(), 22.0, 1e-9);
    EXPECT_NEAR(top_left.y(), 20.0, 1e-9);
    EXPECT_NEAR(bottom_right.x(), 38.0, 1e-9);
    EXPECT_NEAR(bottom_right.y(), 36.0, 1e-9);
}

TEST(BitmapEraserToolbarTest, BuilderDefinitionConstructsWithNativeControls)
{
    if (!initialize_gui()) {
        GTEST_SKIP() << "GTK display unavailable; bitmap eraser toolbar fixture skipped";
    }

    EXPECT_NO_THROW({
        auto toolbar = std::make_unique<Inkscape::UI::Toolbar::BitmapEraserToolbar>();
        EXPECT_TRUE(toolbar);
    });
}

TEST_F(BitmapEraseSessionTest, PreviewAndCancelDoNotTouchDocumentOrCanonicalPixels)
{
    auto *target = image("one");
    Pixbuf baseline(*target->pixbuf);
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());

    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({target}));
    EXPECT_EQ(session.targetCount(), 1u);
    EXPECT_TRUE(session.preview(pixel_rect_on_desktop(target, 2, 2, 6, 6)));
    EXPECT_TRUE(session.active());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_TRUE(bitmap_pixels_equal(*target->pixbuf, baseline));

    session.cancel();
    EXPECT_FALSE(session.active());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_TRUE(bitmap_pixels_equal(*target->pixbuf, baseline));
}

TEST_F(BitmapEraseSessionTest, PreviewIsLocalToInitiatingDesktop)
{
    auto second_desktop = std::make_unique<SPDesktop>(document->getNamedView());
    ASSERT_TRUE(second_desktop);
    auto *target = image("one");
    auto const baseline_first = desktop_checksum(desktop.get());
    auto const baseline_second = desktop_checksum(second_desktop.get());
    ASSERT_NE(baseline_first, 0u);
    ASSERT_EQ(baseline_first, baseline_second);

    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({target}));
    ASSERT_TRUE(session.preview(pixel_rect_on_desktop(target, 2, 2, 6, 6)));
    EXPECT_NE(desktop_checksum(desktop.get()), baseline_first);
    EXPECT_EQ(desktop_checksum(second_desktop.get()), baseline_second);

    session.cancel();
    EXPECT_EQ(desktop_checksum(desktop.get()), baseline_first);
    EXPECT_EQ(desktop_checksum(second_desktop.get()), baseline_second);
}

TEST_F(BitmapEraseSessionTest, RepeatedPreviewPublishesCumulativeSnapshots)
{
    auto *target = image("one");
    Pixbuf canonical(*target->pixbuf);
    auto const baseline = desktop_checksum(desktop.get());

    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({target}));
    ASSERT_TRUE(session.preview(pixel_rect_on_desktop(target, 1, 1, 3, 3)));
    auto const first_preview = desktop_checksum(desktop.get());
    ASSERT_NE(first_preview, baseline);

    ASSERT_TRUE(session.preview(pixel_rect_on_desktop(target, 5, 5, 7, 7)));
    auto const second_preview = desktop_checksum(desktop.get());
    EXPECT_NE(second_preview, first_preview);
    EXPECT_TRUE(bitmap_pixels_equal(*target->pixbuf, canonical));

    session.cancel();
    EXPECT_EQ(desktop_checksum(desktop.get()), baseline);
    EXPECT_TRUE(bitmap_pixels_equal(*target->pixbuf, canonical));
}

TEST_F(BitmapEraseSessionTest, RasterStampPreviewIsViewLocalAndCanonicalUntilCommit)
{
    auto second_desktop = std::make_unique<SPDesktop>(document->getNamedView());
    ASSERT_TRUE(second_desktop);
    auto *target = image("one");
    Pixbuf canonical(*target->pixbuf);
    auto const baseline_first = desktop_checksum(desktop.get());
    auto const baseline_second = desktop_checksum(second_desktop.get());

    auto const center = Geom::Point(4, 4) * *target->pixelToDocumentAffine() * desktop->doc2dt();
    BitmapBrushStamp stamp{
        .center_desktop = center,
        .diameter = 4.0,
        .hardness = 1.0,
        .opacity = 1.0,
        .shape = DrawingImageEraseShape::Round
    };

    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({target}));
    ASSERT_TRUE(session.preview(std::span{&stamp, 1u}));
    EXPECT_NE(desktop_checksum(desktop.get()), baseline_first);
    EXPECT_EQ(desktop_checksum(second_desktop.get()), baseline_second);
    EXPECT_TRUE(bitmap_pixels_equal(*target->pixbuf, canonical));

    ASSERT_TRUE(session.commit());
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Erase bitmap pixels"},
                       "draw-eraser-bitmap");
    document->ensureUpToDate();
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);
    EXPECT_NE(desktop_checksum(second_desktop.get()), baseline_second);
}

// H3: a long raster-brush stroke bakes its preview chain, keeps the pixels
// identical to the un-baked result, and releases without deep recursion.
TEST_F(BitmapEraseSessionTest, LongStampStrokeBakesChainAndMatchesUnbakedPixels)
{
    auto *target = image("one");
    Pixbuf canonical(*target->pixbuf);
    auto const to_desktop = *target->pixelToDocumentAffine() * desktop->doc2dt();

    constexpr int kChunks = 20000;
    auto run = [&](std::size_t bake_limit, std::size_t &max_chain, std::shared_ptr<Pixbuf> &pixels) {
        BitmapEraseSession session(desktop.get());
        ASSERT_TRUE(session.begin({target}));
        session.setBakeChunkLimit(bake_limit);
        max_chain = 0;
        for (int i = 0; i < kChunks; ++i) {
            // Sweep a soft round tip across the 8x8 bitmap; every 7th stamp
            // is soft so baking is checked against gradients too.
            double const t = static_cast<double>(i % 200) / 199.0;
            BitmapBrushStamp stamp{
                .center_desktop = Geom::Point(0.5 + 7.0 * t, 0.5 + 7.0 * ((i / 200) % 8) / 7.0) * to_desktop,
                .diameter = 0.6,
                .hardness = (i % 7 == 0) ? 0.3 : 1.0,
                .opacity = 0.05,
                .shape = (i % 5 == 0) ? DrawingImageEraseShape::Square : DrawingImageEraseShape::Round};
            ASSERT_TRUE(session.preview(std::span{&stamp, 1u}));
            max_chain = std::max(max_chain, session.previewChainLength());
        }
        pixels = session.effectivePreview(0);
        ASSERT_TRUE(pixels);
    }; // session destruction releases the chain

    std::size_t baked_max = 0, unbaked_max = 0;
    std::shared_ptr<Pixbuf> baked, unbaked;
    run(BitmapEraseSession::kDefaultBakeChunkLimit, baked_max, baked);
    run(static_cast<std::size_t>(kChunks) + 1, unbaked_max, unbaked);
    ASSERT_TRUE(baked);
    ASSERT_TRUE(unbaked);

    EXPECT_LE(baked_max, BitmapEraseSession::kDefaultBakeChunkLimit);
    EXPECT_EQ(unbaked_max, static_cast<std::size_t>(kChunks));
    EXPECT_FALSE(bitmap_pixels_equal(*baked, canonical)); // something was erased
    EXPECT_TRUE(bitmap_pixels_equal(*baked, *unbaked));
    EXPECT_TRUE(bitmap_pixels_equal(*target->pixbuf, canonical)); // canonical untouched
}

TEST(DrawingImageErasePreviewChain, ReleasingAVeryLongChainDoesNotOverflowTheStack)
{
    std::shared_ptr<DrawingImageErasePreview const> head;
    for (int i = 0; i < 1000000; ++i) {
        auto node = std::make_shared<DrawingImageErasePreview>();
        node->previous = std::move(head);
        head = std::move(node);
    }
    head.reset(); // recursive release of this chain crashes; must be iterative
    SUCCEED();
}

TEST_F(BitmapEraseSessionTest, ExternalTargetModificationCancelsPreviewSafely)
{
    auto *target = image("one");
    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({target}));
    ASSERT_TRUE(session.preview(pixel_rect_on_desktop(target, 2, 2, 6, 6)));
    ASSERT_TRUE(session.active());

    target->getRepr()->setAttribute("opacity", "0.5");
    document->ensureUpToDate();
    EXPECT_FALSE(session.active());
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Modify bitmap target"}, "object-properties");
}

TEST_F(BitmapEraseSessionTest, CommitPersistsPngAndUndoRedoRestoresExactPixels)
{
    auto *target = image("one");
    auto const href_before = href(target);
    auto const stroke = pixel_rect_on_desktop(target, 2, 2, 6, 6);

    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({target}));
    ASSERT_TRUE(session.preview(stroke));
    ASSERT_TRUE(session.commit(stroke));
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Erase bitmap pixels"}, "draw-eraser");
    document->ensureUpToDate();

    EXPECT_FALSE(session.active());
    EXPECT_NE(href(target), href_before);
    EXPECT_EQ(href(target).find("data:image/png;base64,"), 0u);
    EXPECT_EQ(target->getRepr()->attribute("sodipodi:absref"), nullptr);
    EXPECT_EQ(alpha_at(*target->pixbuf, 4, 4), 0u);
    EXPECT_EQ(alpha_at(*target->pixbuf, 0, 0), 255u);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    target = image("one");
    ASSERT_TRUE(target);
    EXPECT_EQ(href(target), href_before);
    ASSERT_NE(target->getRepr()->attribute("sodipodi:absref"), nullptr);
    EXPECT_STREQ(target->getRepr()->attribute("sodipodi:absref"), "/tmp/original.png");
    EXPECT_EQ(alpha_at(*target->pixbuf, 4, 4), 255u);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    target = image("one");
    ASSERT_TRUE(target);
    EXPECT_EQ(alpha_at(*target->pixbuf, 4, 4), 0u);

    auto const saved = sp_repr_save_buf(document->getReprDoc());
    auto reopened = SPDocument::createNewDocFromMem(saved.raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto *reopened_image = cast<SPImage>(reopened->getObjectById("one"));
    ASSERT_TRUE(reopened_image);
    ASSERT_TRUE(reopened_image->pixbuf);
    EXPECT_EQ(alpha_at(*reopened_image->pixbuf, 4, 4), 0u);
}

TEST_F(BitmapEraseSessionTest, MultipleImagesCommitAsOneUndoTransaction)
{
    auto *first = image("one");
    auto *second = image("two");
    auto const first_href = href(first);
    auto const second_href = href(second);
    auto const stroke = rect_path(2, 2, 16, 6) * desktop->doc2dt();

    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({first, second}));
    EXPECT_EQ(session.targetCount(), 2u);
    ASSERT_TRUE(session.commit(stroke));
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Erase bitmap pixels"}, "draw-eraser");
    document->ensureUpToDate();

    EXPECT_NE(href(image("one")), first_href);
    EXPECT_NE(href(image("two")), second_href);
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);
    EXPECT_EQ(alpha_at(*image("two")->pixbuf, 4, 4), 0u);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(image("one")), first_href);
    EXPECT_EQ(href(image("two")), second_href);
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 255u);
    EXPECT_EQ(alpha_at(*image("two")->pixbuf, 4, 4), 255u);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(BitmapEraseSessionTest, NonIntersectingCommitIsNoOpWithoutUndo)
{
    auto *target = image("one");
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());
    auto const outside = rect_path(20, 20, 25, 25) * desktop->doc2dt();

    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({target}));
    EXPECT_FALSE(session.commit(outside));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

// ST-H: a stroke that misses the bitmap, or can not change it, never copies it.
TEST_F(BitmapEraseSessionTest, NoOpStrokesDoNotCopyTheCanonicalImage)
{
    auto *target = image("one");
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());
    auto const copies = sp_image_pixbuf_copy_count();

    {
        BitmapEraseSession session(desktop.get());
        ASSERT_TRUE(session.begin({target}));
        EXPECT_FALSE(session.commit(rect_path(20, 20, 25, 25) * desktop->doc2dt()));
    }
    {
        // Raster brush entirely outside the bitmap: no stamp reaches it.
        BitmapEraseSession session(desktop.get());
        ASSERT_TRUE(session.begin({target}));
        BitmapBrushStamp outside{.center_desktop = Geom::Point(25, 25) * desktop->doc2dt(), .diameter = 2.0};
        EXPECT_FALSE(session.preview(std::span{&outside, 1u}));
        EXPECT_FALSE(session.commit());
    }
    {
        // Stamps with zero opacity can not change a pixel either.
        BitmapEraseSession session(desktop.get());
        ASSERT_TRUE(session.begin({target}));
        BitmapBrushStamp invisible{.center_desktop = Geom::Point(4, 4) * *target->pixelToDocumentAffine() *
                                                      desktop->doc2dt(),
                                   .diameter = 4.0, .opacity = 0.0};
        session.preview(std::span{&invisible, 1u});
        EXPECT_FALSE(session.commit());
    }
    EXPECT_EQ(sp_image_pixbuf_copy_count(), copies);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

// ST-H: a real commit makes exactly one working copy per changed bitmap.
TEST_F(BitmapEraseSessionTest, CommitMakesOneWorkingCopyOnlyForTheBitmapThatChanges)
{
    auto *one = image("one");
    auto *two = image("two");
    auto const two_href = href(two);
    auto const copies = sp_image_pixbuf_copy_count();

    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({one, two}));
    ASSERT_TRUE(session.commit(pixel_rect_on_desktop(one, 2, 2, 6, 6)));
    EXPECT_EQ(sp_image_pixbuf_copy_count(), copies + 1);
    EXPECT_EQ(href(two), two_href);
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 0, 0), 255u);
}

// ST-H: out of memory cancels the edit: no document change, no Undo step.
TEST_F(BitmapEraseSessionTest, AllocationFailureOnCommitCancelsWithoutDocumentChange)
{
    auto *target = image("one");
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());
    Pixbuf canonical(*target->pixbuf);

    for (char const *stage : {"copy", "png", "base64"}) {
        SCOPED_TRACE(stage);
        {
            AllocationFailure fail(stage);
            BitmapEraseSession session(desktop.get());
            ASSERT_TRUE(session.begin({target}));
            EXPECT_FALSE(session.commit(pixel_rect_on_desktop(target, 2, 2, 6, 6)));
            EXPECT_FALSE(session.active());
        }
        {
            AllocationFailure fail(stage);
            BitmapEraseSession session(desktop.get());
            ASSERT_TRUE(session.begin({target}));
            BitmapBrushStamp stamp{.center_desktop = Geom::Point(4, 4) * *target->pixelToDocumentAffine() *
                                                       desktop->doc2dt(),
                                   .diameter = 4.0};
            ASSERT_TRUE(session.preview(std::span{&stamp, 1u}));
            EXPECT_FALSE(session.commit());
            EXPECT_FALSE(session.active());
        }
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
        EXPECT_FALSE(document->isModifiedSinceSave());
        EXPECT_FALSE(DocumentUndo::undo(document.get()));
        EXPECT_TRUE(bitmap_pixels_equal(*image("one")->pixbuf, canonical));
    }

    // The failure left nothing behind: a normal erase afterwards works.
    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({target}));
    EXPECT_TRUE(session.commit(pixel_rect_on_desktop(target, 2, 2, 6, 6)));
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);
}

namespace {
int apply_calls_seen = 0;
bool fail_second_apply(char const *stage)
{
    return !(std::string(stage) == "apply" && ++apply_calls_seen == 2);
}
} // namespace

// ST-H review fix 1: an allocation failure while the second of two hrefs is being
// replaced rolls the first one back: no document change, no Undo step, session reset.
TEST_F(BitmapEraseSessionTest, FailureWhileApplyingTheSecondTargetRollsEverythingBack)
{
    auto *one = image("one");
    auto *two = image("two");
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());
    Pixbuf canonical_one(*one->pixbuf);
    Pixbuf canonical_two(*two->pixbuf);
    auto const href_one = href(one);
    auto const href_two = href(two);
    auto const both = rect_path(0, 0, 18, 8) * desktop->doc2dt();

    apply_calls_seen = 0;
    sp_image_set_allocation_hook(&fail_second_apply);
    {
        BitmapEraseSession session(desktop.get());
        ASSERT_TRUE(session.begin({one, two}));
        EXPECT_FALSE(session.commit(both));
        EXPECT_FALSE(session.active());
    }
    sp_image_set_allocation_hook(nullptr);
    EXPECT_EQ(apply_calls_seen, 2);

    document->ensureUpToDate();
    // The rollback restores every attribute value; only the position of the
    // re-added sodipodi:absref inside the element may differ, so compare values.
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).bytes(), xml_before.bytes()); // sizes only (see above)
    EXPECT_EQ(href(image("one")), href_one);
    EXPECT_EQ(href(image("two")), href_two);
    EXPECT_STREQ(image("one")->getRepr()->attribute("sodipodi:absref"), "/tmp/original.png");
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    EXPECT_TRUE(bitmap_pixels_equal(*image("one")->pixbuf, canonical_one));
    EXPECT_TRUE(bitmap_pixels_equal(*image("two")->pixbuf, canonical_two));

    // The same stroke works afterwards, and is one Undo step for both bitmaps.
    BitmapEraseSession again(desktop.get());
    ASSERT_TRUE(again.begin({image("one"), image("two")}));
    ASSERT_TRUE(again.commit(both));
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Erase bitmap pixels"}, "draw-eraser-bitmap");
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);
    EXPECT_EQ(alpha_at(*image("two")->pixbuf, 4, 4), 0u);
    EXPECT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

// ST-H review gap: a failed copy while baking cancels the session cleanly.
TEST_F(BitmapEraseSessionTest, AllocationFailureWhileBakingCancelsTheSession)
{
    auto *target = image("one");
    Pixbuf canonical(*target->pixbuf);
    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({target}));
    session.setBakeChunkLimit(2);
    BitmapBrushStamp stamp{.center_desktop = Geom::Point(4, 4) * *target->pixelToDocumentAffine() *
                                               desktop->doc2dt(),
                           .diameter = 4.0};
    ASSERT_TRUE(session.preview(std::span{&stamp, 1u})); // chunk 1: no bake yet
    {
        AllocationFailure fail("copy");
        EXPECT_FALSE(session.preview(std::span{&stamp, 1u})); // chunk 2 bakes and can not copy
    }
    EXPECT_FALSE(session.active());
    EXPECT_TRUE(bitmap_pixels_equal(*image("one")->pixbuf, canonical));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

// ST-H: a failed copy during a live preview cancels the session and restores the view.
TEST_F(BitmapEraseSessionTest, AllocationFailureDuringPreviewCancelsTheSession)
{
    auto *target = image("one");
    auto const checksum = desktop_checksum(desktop.get());
    Pixbuf canonical(*target->pixbuf);
    BitmapEraseSession session(desktop.get());
    ASSERT_TRUE(session.begin({target}));
    {
        AllocationFailure fail("copy");
        EXPECT_FALSE(session.preview(pixel_rect_on_desktop(target, 2, 2, 6, 6)));
    }
    EXPECT_FALSE(session.active());
    EXPECT_EQ(desktop_checksum(desktop.get()), checksum);
    EXPECT_TRUE(bitmap_pixels_equal(*image("one")->pixbuf, canonical));
}

// ST-H: baking the live preview must not change what is committed, and a
// bake after the second one reuses the spare buffer instead of copying.
TEST_F(BitmapEraseSessionTest, CommittedBytesAreIdenticalWithBakingForcedOnAndOff)
{
    auto const run = [&](char const *id, std::size_t bake_limit, std::size_t &copies_while_previewing) {
        auto *target = image(id);
        auto const to_desktop = *target->pixelToDocumentAffine() * desktop->doc2dt();
        BitmapEraseSession session(desktop.get());
        EXPECT_TRUE(session.begin({target}));
        session.setBakeChunkLimit(bake_limit);
        auto const copies_before = sp_image_pixbuf_copy_count();
        for (int i = 0; i < 400; ++i) {
            double const t = static_cast<double>(i % 50) / 49.0;
            BitmapBrushStamp stamp{
                .center_desktop = Geom::Point(0.5 + 7.0 * t, 0.5 + 7.0 * ((i / 50) % 8) / 7.0) * to_desktop,
                .diameter = 0.9,
                .hardness = (i % 3 == 0) ? 0.4 : 1.0,
                .opacity = 0.2,
                .shape = (i % 4 == 0) ? DrawingImageEraseShape::Square : DrawingImageEraseShape::Round};
            EXPECT_TRUE(session.preview(std::span{&stamp, 1u}));
        }
        copies_while_previewing = sp_image_pixbuf_copy_count() - copies_before;
        EXPECT_TRUE(session.commit());
        return href(image(id));
    };

    std::size_t baked_copies = 0, unbaked_copies = 0;
    auto const baked = run("one", 1, baked_copies);         // bake after every chunk
    auto const unbaked = run("two", 100000, unbaked_copies); // never bake
    ASSERT_FALSE(baked.empty());
    EXPECT_EQ(baked, unbaked); // identical PNG, hence identical pixels
    EXPECT_NE(baked, source_uri);
    EXPECT_EQ(unbaked_copies, 0u);
    // 400 bakes: the first two allocate (no spare yet), the rest reuse the spare.
    EXPECT_LE(baked_copies, 2u);
}

// ST-H: the automatic bake interval grows with the bitmap size.
TEST(BitmapEraseSessionPolicy, BakeIntervalScalesWithImageSize)
{
    EXPECT_EQ(BitmapEraseSession::bakeLimitFor(8, 8), BitmapEraseSession::kDefaultBakeChunkLimit);
    EXPECT_EQ(BitmapEraseSession::bakeLimitFor(1000, 1000), BitmapEraseSession::kDefaultBakeChunkLimit);
    EXPECT_EQ(BitmapEraseSession::bakeLimitFor(4000, 4000), 800u);
    EXPECT_EQ(BitmapEraseSession::bakeLimitFor(10000, 10000), BitmapEraseSession::kMaxAutoBakeChunkLimit);
    EXPECT_EQ(BitmapEraseSession::bakeLimitFor(0, 0), BitmapEraseSession::kDefaultBakeChunkLimit);
}

TEST_F(BitmapEraseSessionTest, NativeEraserClickCommitsAndUndoRedoRoundTrips)
{
    auto *target = image("one");
    desktop->getSelection()->set(target);
    auto *tool = activate_bitmap_eraser();
    ASSERT_TRUE(tool);
    auto const center = pixel_to_world(target, Geom::Point(4, 4));
    auto const href_before = href(target);

    EXPECT_TRUE(press(tool, center, 0.65));
    EXPECT_TRUE(release(tool, center));
    document->ensureUpToDate();
    EXPECT_NE(href(image("one")), href_before);
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(href(image("one")), href_before);
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 255u);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);
}

TEST_F(BitmapEraseSessionTest, NativeDragEditsMultipleSelectedBitmapsAsOneUndo)
{
    auto *first = image("one");
    auto *second = image("two");
    desktop->getSelection()->set(first);
    desktop->getSelection()->add(second);
    auto *tool = activate_bitmap_eraser();
    ASSERT_TRUE(tool);
    auto const start = pixel_to_world(first, Geom::Point(4, 4));
    auto const finish = pixel_to_world(second, Geom::Point(4, 4));

    {
        SCOPED_TRACE("button press");
        ASSERT_NO_THROW(ASSERT_TRUE(press(tool, start)));
    }
    for (int i = 1; i <= 12; ++i) {
        SCOPED_TRACE(::testing::Message() << "motion sample " << i);
        auto const t = static_cast<double>(i) / 12.0;
        ASSERT_NO_THROW(EXPECT_TRUE(move(tool, start + (finish - start) * t)));
    }
    {
        SCOPED_TRACE("button release");
        ASSERT_NO_THROW(EXPECT_TRUE(release(tool, finish)));
    }
    document->ensureUpToDate();
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);
    EXPECT_EQ(alpha_at(*image("two")->pixbuf, 4, 4), 0u);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 255u);
    EXPECT_EQ(alpha_at(*image("two")->pixbuf, 4, 4), 255u);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(BitmapEraseSessionTest, SparseHighSpeedMotionProducesContinuousStroke)
{
    auto *target = image("one");
    desktop->getSelection()->set(target);
    auto *tool = activate_bitmap_eraser(3.0);
    ASSERT_TRUE(tool);
    auto const start = pixel_to_world(target, Geom::Point(1, 4));
    auto const finish = pixel_to_world(target, Geom::Point(7, 4));

    ASSERT_TRUE(press(tool, start));
    ASSERT_TRUE(move(tool, finish));
    ASSERT_TRUE(release(tool, finish));
    document->ensureUpToDate();

    for (int x = 1; x <= 7; ++x) {
        EXPECT_LT(alpha_at(*image("one")->pixbuf, x, 4), 32u) << "gap at x=" << x;
    }
}

TEST_F(BitmapEraseSessionTest, PressureChangesTipDiameterWithoutChangingHardness)
{
    auto *first = image("one");
    desktop->getSelection()->set(first);
    auto *tool = activate_bitmap_eraser(6.0, 0, 100.0, true);
    ASSERT_TRUE(tool);
    // Align the brush center with the center of an intrinsic pixel. At 20%
    // pressure the 6 px brush is only 1.2 px wide, so centering it on a pixel
    // boundary would intentionally antialias four neighboring pixels instead
    // of fully clearing any one of them.
    auto const first_center = pixel_to_world(first, Geom::Point(4.5, 4.5));

    ASSERT_TRUE(press(tool, first_center, 0.20));
    ASSERT_TRUE(release(tool, first_center));
    document->ensureUpToDate();
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);
    EXPECT_GT(alpha_at(*image("one")->pixbuf, 6, 4), 240u);

    auto *second = image("two");
    desktop->getSelection()->set(second);
    auto const second_center = pixel_to_world(second, Geom::Point(4.5, 4.5));
    ASSERT_TRUE(press(tool, second_center, 1.0));
    ASSERT_TRUE(release(tool, second_center));
    document->ensureUpToDate();
    EXPECT_LT(alpha_at(*image("two")->pixbuf, 6, 4), 32u);
}

TEST_F(BitmapEraseSessionTest, SquareAndSoftSettingsCommitAndUndo)
{
    auto *target = image("one");
    desktop->getSelection()->set(target);
    auto *tool = activate_bitmap_eraser(6.0, 1, 35.0);
    ASSERT_TRUE(tool);
    EXPECT_EQ(tool->shape(), DrawingImageEraseShape::Square);
    EXPECT_NEAR(tool->hardness(), 0.35, 1e-9);
    auto const center = pixel_to_world(target, Geom::Point(4, 4));

    ASSERT_TRUE(press(tool, center));
    ASSERT_TRUE(release(tool, center));
    document->ensureUpToDate();
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);
    EXPECT_GT(alpha_at(*image("one")->pixbuf, 1, 1), 0u);
    EXPECT_LT(alpha_at(*image("one")->pixbuf, 1, 1), 255u);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 255u);
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 1, 1), 255u);
}

TEST_F(BitmapEraseSessionTest, EscapeCancelsNativeStrokeWithoutXmlOrUndo)
{
    auto *target = image("one");
    desktop->getSelection()->set(target);
    auto *tool = activate_bitmap_eraser();
    ASSERT_TRUE(tool);
    auto const start = pixel_to_world(target, Geom::Point(2, 4));
    auto const finish = pixel_to_world(target, Geom::Point(6, 4));
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());

    {
        SCOPED_TRACE("button press");
        ASSERT_NO_THROW(ASSERT_TRUE(press(tool, start)));
    }
    for (int i = 1; i <= 8; ++i) {
        SCOPED_TRACE(::testing::Message() << "motion sample " << i);
        auto const t = static_cast<double>(i) / 8.0;
        ASSERT_NO_THROW(EXPECT_TRUE(move(tool, start + (finish - start) * t)));
    }

    KeyPressEvent escape;
    escape.keyval = GDK_KEY_Escape;
    GdkKeymapKey *escape_keys = nullptr;
    int escape_key_count = 0;
    ASSERT_TRUE(gdk_display_map_keyval(gdk_display_get_default(), GDK_KEY_Escape, &escape_keys, &escape_key_count));
    ASSERT_GT(escape_key_count, 0);
    escape.keycode = escape_keys[0].keycode;
    escape.group = escape_keys[0].group;
    g_free(escape_keys);
    {
        SCOPED_TRACE("Escape key");
        ASSERT_NO_THROW(EXPECT_TRUE(tool->root_handler(escape)));
    }
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(BitmapEraseSessionTest, ToolSwitchCancelsPendingStrokeWithoutDocumentChange)
{
    auto *target = image("one");
    desktop->getSelection()->set(target);
    auto *tool = activate_bitmap_eraser();
    ASSERT_TRUE(tool);
    auto const start = pixel_to_world(target, Geom::Point(2, 4));
    auto const finish = pixel_to_world(target, Geom::Point(6, 4));
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());

    ASSERT_TRUE(press(tool, start));
    ASSERT_TRUE(move(tool, finish));
    desktop->setTool("/tools/select");
    document->ensureUpToDate();

    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(BitmapEraseSessionTest, SelectionChangeCancelsPendingStroke)
{
    auto *target = image("one");
    desktop->getSelection()->set(target);
    auto *tool = activate_bitmap_eraser();
    ASSERT_TRUE(tool);
    auto const start = pixel_to_world(target, Geom::Point(2, 4));
    auto const finish = pixel_to_world(target, Geom::Point(6, 4));
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());

    ASSERT_TRUE(press(tool, start));
    ASSERT_TRUE(move(tool, finish));
    desktop->getSelection()->set(image("two"));
    document->ensureUpToDate();

    EXPECT_FALSE(tool->isDrawing());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(BitmapEraseSessionTest, WithNoSelectionBitmapUnderPointerBecomesTarget)
{
    desktop->getSelection()->clear();
    auto *target = image("one");
    auto *tool = activate_bitmap_eraser();
    ASSERT_TRUE(tool);
    desktop->getCanvasDrawing()->get_drawing()->update();
    auto const center = pixel_to_world(target, Geom::Point(4, 4));

    ASSERT_TRUE(press(tool, center));
    EXPECT_TRUE(release(tool, center));
    document->ensureUpToDate();
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);
    EXPECT_TRUE(desktop->getSelection()->isEmpty());
}

TEST_F(BitmapEraseSessionTest, MixedVectorAndBitmapSelectionOnlyEditsBitmap)
{
    auto *target = image("one");
    auto *vector = cast<SPItem>(document->getObjectById("vector"));
    ASSERT_TRUE(target);
    ASSERT_TRUE(vector);
    auto const vector_fill = std::string(vector->getRepr()->attribute("fill"));
    auto const vector_x = std::string(vector->getRepr()->attribute("x"));
    auto const vector_width = std::string(vector->getRepr()->attribute("width"));

    desktop->getSelection()->set(target);
    desktop->getSelection()->add(vector);
    auto *tool = activate_bitmap_eraser();
    ASSERT_TRUE(tool);
    auto const center = pixel_to_world(target, Geom::Point(4, 4));

    ASSERT_TRUE(press(tool, center));
    ASSERT_TRUE(release(tool, center));
    document->ensureUpToDate();
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 0u);
    vector = cast<SPItem>(document->getObjectById("vector"));
    ASSERT_TRUE(vector);
    EXPECT_EQ(vector->getRepr()->attribute("fill"), vector_fill);
    EXPECT_EQ(vector->getRepr()->attribute("x"), vector_x);
    EXPECT_EQ(vector->getRepr()->attribute("width"), vector_width);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(alpha_at(*image("one")->pixbuf, 4, 4), 255u);
    EXPECT_TRUE(document->getObjectById("vector"));
}

TEST_F(BitmapEraseSessionTest, NativeNoOpOutsideSelectedBitmapPreservesRedo)
{
    auto *target = image("one");
    target->getRepr()->setAttribute("opacity", "0.75");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Prepare redo"}, "object-properties");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    document->setModifiedSinceSave(false);

    desktop->getSelection()->set(image("one"));
    auto *tool = activate_bitmap_eraser();
    ASSERT_TRUE(tool);
    auto const outside_document = Geom::Point(25, 10);
    auto const outside_world = desktop->d2w(outside_document * desktop->doc2dt());
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());

    ASSERT_TRUE(press(tool, outside_world));
    EXPECT_TRUE(release(tool, outside_world));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
}

TEST_F(BitmapEraseSessionTest, ExistingVectorCutModeStillCommitsAndUndoes)
{
    auto *vector = cast<SPItem>(document->getObjectById("vector"));
    ASSERT_TRUE(vector);
    desktop->getSelection()->set(vector);
    auto *tool = activate_eraser(1);
    ASSERT_TRUE(tool);
    auto const before = sp_repr_save_buf(document->getReprDoc());
    auto const start = desktop->d2w(Geom::Point(19, 4) * desktop->doc2dt());
    auto const finish = desktop->d2w(Geom::Point(29, 4) * desktop->doc2dt());

    ASSERT_TRUE(press(tool, start));
    for (int i = 1; i <= 12; ++i) {
        auto const t = static_cast<double>(i) / 12.0;
        ASSERT_TRUE(move(tool, start + (finish - start) * t));
    }
    ASSERT_TRUE(release(tool, finish));
    document->ensureUpToDate();
    EXPECT_NE(sp_repr_save_buf(document->getReprDoc()), before);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
    EXPECT_TRUE(document->getObjectById("vector"));
}
