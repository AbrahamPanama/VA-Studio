// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include <cairomm/surface.h>

#include "bitmap-adjustment-chemistry.h"
#include "display/bitmap-histogram.h"
#include "display/drawing-context.h"
#include "display/drawing-item.h"
#include "display/drawing-surface.h"
#include "display/drawing.h"
#include "display/nr-filter.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "object/sp-filter.h"
#include "object/filters/sp-filter-primitive.h"
#include "style.h"
#include "xml/repr.h"

using namespace Inkscape;

namespace {

constexpr auto image_data =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJ"
    "AAAADUlEQVQIHWP4z8DwHwAFgAI/ScLx9QAAAABJRU5ErkJggg==";

// 2 x 2 RGBA: transparent, two opaque grays, 50%-alpha brown.
constexpr auto transparent_image_data =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACAgMAAAAP2OW3AAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAAMUExURQAAAIBAIEBAQMDAwLgSRQ4AAAACdFJOUwCAmytOGAAAAAd0SU1FB+oIHREEEmM5K8wAAAAldEVYdGRhdGU6Y3JlYXRlADIwMjYtMDgtMjlUMTc6MDQ6MTgrMDA6MDBoWbZDAAAAJXRFWHRkYXRlOm1vZGlmeQAyMDI2LTA4LTI5VDE3OjA0OjE4KzAwOjAwGQQO/wAAACh0RVh0ZGF0ZTp0aW1lc3RhbXAAMjAyNi0wOC0yOVQxNzowNDoxOCswMDowME4RLyAAAAAMSURBVAjXY1BguAAAATQA8bBD2hMAAAAASUVORK5CYII=";

class TestDisplay
{
public:
    explicit TestDisplay(SPDocument *document, int width = 10, int height = 10)
        : _root(document->getRoot())
        , _key(SPItem::display_key_new(1))
        , _width(width)
        , _height(height)
    {
        auto root_item = _root->invoke_show(_drawing, _key, SP_ITEM_SHOW_DISPLAY);
        _drawing.setRoot(root_item);
        _drawing.update();
    }

    ~TestDisplay() { _root->invoke_hide(_key); }

    DrawingItem *item(SPItem *item) { return item->get_arenaitem(_key); }

    Cairo::RefPtr<Cairo::ImageSurface> render()
    {
        _drawing.update();
        auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,
                                                   _width, _height);
        DrawingSurface target(surface->cobj(), Geom::IntPoint(0, 0));
        DrawingContext context(target);
        _drawing.render(context, Geom::IntRect::from_xywh(0, 0, _width, _height));
        surface->flush();
        return surface;
    }

private:
    Drawing _drawing;
    SPRoot *_root;
    unsigned _key;
    int _width;
    int _height;
};

uint64_t checksum(Cairo::RefPtr<Cairo::ImageSurface> const &surface)
{
    uint64_t result = 1469598103934665603ULL;
    auto const size = static_cast<std::size_t>(surface->get_stride()) * surface->get_height();
    auto data = surface->get_data();
    for (std::size_t i = 0; i < size; ++i) {
        result ^= data[i];
        result *= 1099511628211ULL;
    }
    return result;
}

std::size_t visible_pixels(Cairo::RefPtr<Cairo::ImageSurface> const &surface)
{
    std::size_t result = 0;
    auto data = surface->get_data();
    for (int y = 0; y < surface->get_height(); ++y) {
        auto row = data + static_cast<std::size_t>(y) * surface->get_stride();
        for (int x = 0; x < surface->get_width(); ++x) {
            if (row[4 * x + 3]) ++result;
        }
    }
    return result;
}

std::size_t nonwhite_pixels(Cairo::RefPtr<Cairo::ImageSurface> const &surface)
{
    std::size_t result = 0;
    auto data = surface->get_data();
    for (int y = 0; y < surface->get_height(); ++y) {
        auto row = data + static_cast<std::size_t>(y) * surface->get_stride();
        for (int x = 0; x < surface->get_width(); ++x) {
            auto pixel = row + 4 * x;
            if (pixel[0] != 255 || pixel[1] != 255 || pixel[2] != 255) ++result;
        }
    }
    return result;
}

std::vector<unsigned char> alpha_plane(Cairo::RefPtr<Cairo::ImageSurface> const &surface)
{
    std::vector<unsigned char> result;
    result.reserve(static_cast<std::size_t>(surface->get_width()) * surface->get_height());
    auto data = surface->get_data();
    for (int y = 0; y < surface->get_height(); ++y) {
        auto row = data + static_cast<std::size_t>(y) * surface->get_stride();
        for (int x = 0; x < surface->get_width(); ++x) {
            result.push_back(row[4 * x + 3]);
        }
    }
    return result;
}

} // namespace

TEST(BitmapAdjustmentPreviewTest, IsViewLocalAndDoesNotTouchXml)
{
    if (!Application::exists()) Application::create(false);
    auto const svg = Glib::ustring::compose(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><image id="image" width="10" height="10" href="%1"/></svg>)",
        image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    TestDisplay first(document.get());
    TestDisplay second(document.get());
    auto const baseline_first = checksum(first.render());
    auto const baseline_second = checksum(second.render());
    ASSERT_EQ(baseline_first, baseline_second);
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());
    auto const modified_before = document->isModifiedSinceSave();

    Filters::BitmapToneSettings settings;
    auto const histogram = Filters::build_bitmap_histogram(*image->pixbuf);
    auto source_bin = std::find_if(histogram.luminance.begin(), histogram.luminance.end(),
                                   [](auto count) { return count != 0; });
    ASSERT_NE(source_bin, histogram.luminance.end());
    settings.brightness = std::distance(histogram.luminance.begin(), source_bin) > 127 ? -50 : 50;
    auto drawing_item = first.item(image);
    ASSERT_TRUE(drawing_item);
    drawing_item->setFilterRenderer(
        BitmapAdjustments::build_tone_preview_renderer(image, drawing_item, settings));

    EXPECT_NE(checksum(first.render()), baseline_first);
    EXPECT_EQ(checksum(second.render()), baseline_second);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_EQ(document->isModifiedSinceSave(), modified_before);
}

TEST(BitmapAdjustmentPreviewTest, CommittedToneFilterKeepsBitmapVisible)
{
    if (!Application::exists()) Application::create(false);
    auto const svg = Glib::ustring::compose(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><image id="image" width="10" height="10" href="%1"/></svg>)",
        image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    TestDisplay display(document.get());
    auto const baseline = display.render();
    ASSERT_GT(visible_pixels(baseline), 0u);

    Filters::BitmapToneSettings settings;
    settings.brightness = 10;
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image, settings));
    document->ensureUpToDate();
    auto const adjusted = display.render();

    EXPECT_GT(visible_pixels(adjusted), 0u);
    EXPECT_GT(nonwhite_pixels(adjusted), 0u);
    EXPECT_NE(checksum(adjusted), checksum(baseline));
}

TEST(BitmapAdjustmentPreviewTest, CommittedToneFilterRendersLargeScaledBitmap)
{
    if (!Application::exists()) Application::create(false);
    constexpr int width = 1058;
    constexpr int height = 1052;
    auto const svg = Glib::ustring::compose(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="%1" height="%2"><image id="image" width="%1" height="%2" href="%3"/></svg>)",
        width, height, image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    TestDisplay display(document.get(), width, height);
    auto const baseline = display.render();
    ASSERT_GT(nonwhite_pixels(baseline), 0u);

    Filters::BitmapToneSettings settings;
    settings.brightness = 3;
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image, settings));
    document->ensureUpToDate();
    auto const adjusted = display.render();

    EXPECT_GT(visible_pixels(adjusted), 0u);
    EXPECT_GT(nonwhite_pixels(adjusted), 0u);
    EXPECT_NE(checksum(adjusted), checksum(baseline));
}

TEST(BitmapAdjustmentPreviewTest, PreviewCommitResetAndReopenPreserveExactAlpha)
{
    if (!Application::exists()) Application::create(false);
    auto const svg = Glib::ustring::compose(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2"><image id="image" width="2" height="2" style="image-rendering:pixelated" href="%1"/></svg>)",
        transparent_image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    TestDisplay display(document.get(), 2, 2);
    auto const baseline = display.render();
    auto const baseline_alpha = alpha_plane(baseline);
    ASSERT_EQ(baseline_alpha, (std::vector<unsigned char>{0, 255, 255, 128}));

    Filters::BitmapToneSettings settings;
    settings.brightness = 35;
    settings.contrast = 20;
    auto drawing_item = display.item(image);
    ASSERT_TRUE(drawing_item);
    drawing_item->setFilterRenderer(
        BitmapAdjustments::build_tone_preview_renderer(image, drawing_item, settings));
    auto const preview = display.render();
    EXPECT_EQ(alpha_plane(preview), baseline_alpha);
    EXPECT_NE(checksum(preview), checksum(baseline));

    drawing_item->setFilterRenderer(nullptr);
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image, settings));
    document->ensureUpToDate();
    auto const committed = display.render();
    EXPECT_EQ(alpha_plane(committed), baseline_alpha);
    EXPECT_NE(checksum(committed), checksum(baseline));

    auto const serialized = sp_repr_save_buf(document->getReprDoc());
    auto reopened = SPDocument::createNewDocFromMem(serialized.raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto reopened_image = cast<SPImage>(reopened->getObjectById("image"));
    ASSERT_TRUE(reopened_image);
    TestDisplay reopened_display(reopened.get(), 2, 2);
    auto const reopened_render = reopened_display.render();
    EXPECT_EQ(alpha_plane(reopened_render), baseline_alpha);
    EXPECT_EQ(checksum(reopened_render), checksum(committed));

    ASSERT_TRUE(BitmapAdjustments::reset_tone(image));
    document->ensureUpToDate();
    auto const reset = display.render();
    EXPECT_EQ(alpha_plane(reset), baseline_alpha);
    EXPECT_EQ(checksum(reset), checksum(baseline));
}

TEST(BitmapAdjustmentPreviewTest, VectorPreviewIsViewLocalAndKeepsSourceVector)
{
    if (!Application::exists()) Application::create(false);
    auto document = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">
  <rect id="rect" width="10" height="10" fill="#4080c0"/>
</svg>)svg");
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto rect = cast<SPItem>(document->getObjectById("rect"));
    ASSERT_TRUE(rect);

    TestDisplay first(document.get());
    TestDisplay second(document.get());
    auto const baseline = checksum(first.render());
    ASSERT_EQ(baseline, checksum(second.render()));
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());

    Filters::BitmapToneSettings settings;
    settings.brightness = -50;
    auto drawing_item = first.item(rect);
    ASSERT_TRUE(drawing_item);
    drawing_item->setFilterRenderer(
        BitmapAdjustments::build_tone_preview_renderer(rect, drawing_item, settings));

    EXPECT_NE(checksum(first.render()), baseline);
    EXPECT_EQ(checksum(second.render()), baseline);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_EQ(std::string{rect->getRepr()->name()}, "svg:rect");
}

TEST(BitmapAdjustmentPreviewTest, PreviewReplacesManagedPrimitiveInPlace)
{
    if (!Application::exists()) Application::create(false);
    auto document = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     width="16" height="12">
  <defs>
    <filter id="chain" x="-20%" y="-20%" width="160%" height="160%">
      <feGaussianBlur stdDeviation="0.25" result="blur"/>
      <feComponentTransfer in="blur" result="tone"
          inkscape:bitmap-adjustment="tone-v1"
          inkscape:brightness="0" inkscape:contrast="0" inkscape:intensity="0"
          inkscape:highlights="0" inkscape:shadows="0" inkscape:midtones="0"/>
      <feOffset in="tone" dx="2" dy="0"/>
    </filter>
  </defs>
  <rect id="rect" x="1" y="1" width="8" height="8" fill="#604020"
        style="filter:url(#chain)"/>
</svg>)svg");
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto rect = cast<SPItem>(document->getObjectById("rect"));
    ASSERT_TRUE(rect);
    ASSERT_TRUE(rect->style->getFilter());

    TestDisplay display(document.get(), 16, 12);
    auto drawing_item = display.item(rect);
    ASSERT_TRUE(drawing_item);
    Filters::BitmapToneSettings settings;
    settings.brightness = 35;
    settings.contrast = 20;

    drawing_item->setFilterRenderer(
        BitmapAdjustments::build_tone_preview_renderer(rect, drawing_item, settings));
    auto const preview = checksum(display.render());

    drawing_item->setFilterRenderer(rect->style->getFilter()->build_renderer(drawing_item));
    display.render();
    ASSERT_TRUE(BitmapAdjustments::apply_tone(rect, settings));
    document->ensureUpToDate();
    drawing_item->setFilterRenderer(rect->style->getFilter()->build_renderer(drawing_item));
    auto const committed = checksum(display.render());

    EXPECT_EQ(preview, committed);
    auto managed = BitmapAdjustments::find_managed_tone_primitive(rect->style->getFilter());
    ASSERT_TRUE(managed);
    EXPECT_STREQ(managed->getRepr()->attribute("in"), "blur");
    EXPECT_STREQ(managed->getRepr()->attribute("result"), "tone");
    EXPECT_STREQ(managed->getRepr()->next()->name(), "svg:feOffset");
}

TEST(BitmapAdjustmentPreviewTest, MaskClipAndTransparencyMatchBetweenPreviewAndCommit)
{
    if (!Application::exists()) Application::create(false);
    auto document = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20">
  <defs>
    <linearGradient id="fade"><stop stop-color="white"/><stop offset="1" stop-color="black"/></linearGradient>
    <mask id="mask"><rect width="20" height="20" fill="url(#fade)"/></mask>
    <clipPath id="clip"><circle cx="10" cy="10" r="8"/></clipPath>
  </defs>
  <rect id="rect" x="1" y="1" width="18" height="18" fill="#2060a0"
        fill-opacity="0.65" mask="url(#mask)" clip-path="url(#clip)"/>
</svg>)svg");
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto rect = cast<SPItem>(document->getObjectById("rect"));
    ASSERT_TRUE(rect);

    TestDisplay display(document.get(), 20, 20);
    auto drawing_item = display.item(rect);
    ASSERT_TRUE(drawing_item);
    auto const baseline = display.render();
    auto const baseline_alpha = alpha_plane(baseline);
    Filters::BitmapToneSettings settings;
    settings.brightness = 45;
    settings.shadows = 20;

    drawing_item->setFilterRenderer(
        BitmapAdjustments::build_tone_preview_renderer(rect, drawing_item, settings));
    auto const preview = display.render();
    EXPECT_EQ(alpha_plane(preview), baseline_alpha);

    ASSERT_TRUE(BitmapAdjustments::apply_tone(rect, settings));
    document->ensureUpToDate();
    drawing_item->setFilterRenderer(rect->style->getFilter()->build_renderer(drawing_item));
    auto const committed = display.render();
    EXPECT_EQ(alpha_plane(committed), baseline_alpha);
    EXPECT_EQ(checksum(preview), checksum(committed));
    EXPECT_EQ(std::string{rect->getRepr()->name()}, "svg:rect");
    EXPECT_STREQ(rect->getRepr()->attribute("mask"), "url(#mask)");
    EXPECT_STREQ(rect->getRepr()->attribute("clip-path"), "url(#clip)");
}
