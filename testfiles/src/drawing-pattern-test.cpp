// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Test drawing_pattern_test
 */
/*
 * Authors:
 *   PBS <pbs3141@gmail.com>
 *
 * Copyright (C) 2022 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#include <gtest/gtest.h>

#include <cairomm/surface.h>
#include <2geom/int-rect.h>
#include <2geom/int-point.h>

#include "inkscape.h"
#include "document.h"
#include "object/sp-root.h"
#include "display/drawing.h"
#include "display/drawing-pattern.h"
#include "display/drawing-surface.h"
#include "display/drawing-context.h"

namespace Inkscape {

TEST(DrawingPatternTest, fragments)
{
    if (!Inkscape::Application::exists()) {
        Inkscape::Application::create(false);
    }

    auto doc = std::unique_ptr<SPDocument>(SPDocument::createNewDoc(INKSCAPE_TESTS_DIR "/rendering_tests/drawing-pattern-test.svg"));
    ASSERT_TRUE((bool)doc);
    ASSERT_TRUE((bool)doc->getRoot());

    doc->ensureUpToDate();

    class Display
    {
    public:
        Display(SPDocument *doc) {
            root = doc->getRoot();
            dkey = SPItem::display_key_new(1);
            rootitem = root->invoke_show(drawing, dkey, SP_ITEM_SHOW_DISPLAY);
            drawing.setRoot(rootitem);
            drawing.update();
        }

        ~Display()
        {
            root->invoke_hide(dkey);
        }

        auto draw(Geom::IntRect const &rect)
        {
            auto cs = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, rect.width(), rect.height());
            auto ds = Inkscape::DrawingSurface(cs->cobj(), rect.min());
            auto dc = Inkscape::DrawingContext(ds);
            drawing.render(dc, rect);
            return cs;
        }

    private:
        Inkscape::Drawing drawing;
        SPRoot *root;
        Inkscape::DrawingItem *rootitem;
        unsigned dkey;
    };

    auto const tile = Geom::IntPoint(30, 30);
    auto const area = Geom::IntRect::from_xywh(0, 0, 100, 100);

    auto const reference = Display(doc.get()).draw(area);

    uint32_t state = 0;
    auto rand = [&] {
        state = (state * 1103515245) + 12345;
        return state;
    };
    rand();

    auto randrect = [&] {
        int w = rand() % tile.x() / 3 + 1;
        int h = rand() % tile.y() / 3 + 1;
        int x = rand() % (area.width() - w + 1);
        int y = rand() % (area.height() - h + 1);
        return Geom::IntRect::from_xywh(x, y, w, h);
    };

    int maxdiff = 0;
    auto compare = [&] (Cairo::RefPtr<Cairo::ImageSurface> const &part, Geom::IntPoint const &off) {
        for (int y = 0; y < part->get_height(); y++) {
            auto p = reference->get_data() + (off.y() + y) * reference->get_stride() + off.x() * 4;
            auto q = part->get_data() + y * part->get_stride();
            for (int x = 0; x < part->get_width(); x++) {
                for (int c = 0; c < 4; c++) {
                    auto diff = std::abs((int)p[c] - (int)q[c]);
                    maxdiff = std::max(maxdiff, diff);
                }
                p += 4;
                q += 4;
            }
        }
    };

    for (int j = 0; j < 5; j++) {
        auto d = Display(doc.get());
        for (int i = 0; i < 20; i++) {
            auto const rect = randrect();
            auto const part = d.draw(rect);
            compare(part, rect.min());
        }
    }

    ASSERT_LE(maxdiff, 10);
}

struct TestDrawingPattern : public DrawingPattern
{
    TestDrawingPattern()
        : DrawingPattern{[]() -> Drawing & {
            static Drawing fakeDrawing;
            return fakeDrawing;
        }()}
    {}
    ~TestDrawingPattern() override = default;

    /// Pretend the pattern was updated to a tile of this many pixels.
    void setResolution(Geom::IntPoint const &resolution) { _pattern_resolution = resolution; }
};

/// Regression test for https://gitlab.com/inkscape/inkscape/-/issues/5677
TEST(DrawingPatternTest, ZeroRankPatternMatrix)
{
    RenderContext fakeContext{
        .outline_color = Colors::Color(0xff),
    };

    TestDrawingPattern testPattern;
    testPattern.setTileRect(Geom::Rect::from_xywh(0, 0, 1, 1));

    EXPECT_FALSE(testPattern.renderPattern(fakeContext, Geom::IntRect::from_xywh(0, 0, 1, 1), 1.0, 1));
}

/// ST-S R5: the pattern cache must not merge every requested area into one ever-growing surface
/// (an export asks for one stripe after another; the union would reach page size).
TEST(DrawingPatternTest, CachedSurfacesStayWithinThePixelLimit)
{
    RenderContext context{
        .outline_color = Colors::Color(0xff),
    };

    auto march = [&] (size_t limit) {
        TestDrawingPattern pattern;
        pattern.setTileRect(Geom::Rect::from_xywh(0, 0, 100, 100));
        pattern.setResolution({100, 100});
        pattern.setCachedPixelLimit(limit);
        size_t peak = 0;
        size_t last_surfaces = 0;
        // Ten adjacent 10 x 10 requests: each touches the previous one, so each used to be merged.
        for (int i = 0; i < 10; ++i) {
            cairo_pattern_t *p = pattern.renderPattern(context, Geom::IntRect::from_xywh(10 * i, 0, 10, 10), 1.0, 1);
            EXPECT_TRUE(p) << "request " << i << " must still be served";
            if (p) cairo_pattern_destroy(p);
            auto const [surfaces, pixels] = pattern.cacheUsage();
            peak = std::max(peak, pixels);
            last_surfaces = surfaces;
        }
        return std::make_pair(peak, last_surfaces);
    };

    // Without a cap (the old behaviour) the union grows to the whole 100 x 10 strip.
    auto const [unbounded_peak, unbounded_surfaces] = march(size_t(-1));
    EXPECT_GE(unbounded_peak, 1000u);
    EXPECT_EQ(unbounded_surfaces, 1u);

    // With a cap of 250 pixels the cache never holds more than the limit, and still has a surface.
    auto const [bounded_peak, bounded_surfaces] = march(250);
    EXPECT_LE(bounded_peak, 250u);
    EXPECT_GE(bounded_surfaces, 1u);
}

} // namespace Inkscape
