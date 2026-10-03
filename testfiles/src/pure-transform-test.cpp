// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <csignal>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

#include <2geom/transforms.h>
#include <glibmm/main.h>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-item.h"
#include "object/sp-item-transform.h"
#include "object/sp-namedview.h"
#include "pure-transform.h"
#include "selection.h"
#include "seltrans.h"
#include "seltrans-handles.h"
#include "snap.h"
#include "ui/knot/knot.h"
#include "ui/tools/select-tool.h"
#include "ui/widget/canvas.h"
#include "util/transform-objects.h"
#include "xml/node.h"

using namespace Inkscape;

namespace {

InkscapeApplication *initialize_gui()
{
    static auto *app = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "puretransformtest", TRUE);
        auto result = new InkscapeApplication(); // Process-lifetime test fixture.
        result->gio_app()->register_application();
        // A regression must fail normally, not wait in an interactive crash dialog.
        for (auto signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) {
            std::signal(signal, SIG_DFL);
        }
#ifndef _WIN32
        std::signal(SIGBUS, SIG_DFL);
#endif
        return result;
    }();
    return app->gtk_app() ? app : nullptr;
}

class PureTransformSnapTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!initialize_gui()) {
            GTEST_SKIP() << "GTK display unavailable; real snapping fixture skipped";
        }
        document = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <path id="target" d="M100,100 L140,100 L140,140 Z"/>
</svg>)svg");
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        // Match the selector fixture's real viewport for object candidate culling.
        desktop->getCanvas()->size_allocate(Gtk::Allocation(0, 0, 256, 256), -1);
        ASSERT_GT(desktop->getCanvas()->get_dimensions().x(), 1);
        ASSERT_GT(desktop->getCanvas()->get_dimensions().y(), 1);

        auto item = cast<SPItem>(document->getObjectById("target"));
        ASSERT_TRUE(item);
        target = Geom::Point(100, 100) * item->i2dt_affine();

        auto &snap = manager();
        snap.snapprefs.setSnapEnabledGlobally(true);
        snap.snapprefs.setSnapPostponedGlobally(false);
        snap.snapprefs.clearTargetMask(0);
        snap.snapprefs.setTargetMask(SNAPTARGET_NODE_CATEGORY, 1);
        snap.snapprefs.setTargetMask(SNAPTARGET_NODE_CUSP, 1);
        snap.snapprefs.setObjectTolerance(10);
        snap.setup(desktop.get(), false);
        ASSERT_TRUE(snap.someSnapperMightSnap());
    }

    void TearDown() override
    {
        if (desktop) {
            manager().unSetup();
        }
        desktop.reset();
        document.reset();
        auto context = Glib::MainContext::get_default();
        while (context->iteration(false)) {}
    }

    SnapManager &manager() { return desktop->getNamedView()->snap_manager; }

    void snap(PureTransform &transform, Geom::Point const &source, Geom::Point const &pointer)
    {
        // Keep a real, nonempty movable source set. Empty/masked-out source sets
        // bypass storeTransform() and cannot detect false geometric winners.
        std::vector<SnapCandidatePoint> const sources = {{source, SNAPSOURCE_NODE_CUSP}};
        manager().snapTransformed(sources, pointer, transform);
    }

    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    Geom::Point target;
};

} // namespace

TEST_F(PureTransformSnapTest, UnmatchedStretchRemainsUnsnappedWithMovableSources)
{
    auto const origin = target - Geom::Point(80, 80);
    auto const source = origin + Geom::Point(20, 20);
    for (auto axis : {Geom::X, Geom::Y}) {
        for (bool uniform : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "axis=" << axis << " uniform=" << uniform);
            Geom::Scale raw(1.47, 1.47);
            if (!uniform) raw[1 - axis] = 1;
            auto const pointer = (source - origin) * raw + origin;
            PureStretchConstrained transform(1.47, origin, axis, uniform);
            snap(transform, source, pointer);

            EXPECT_FALSE(transform.best_snapped_point.getSnapped());
            EXPECT_TRUE(std::isinf(transform.best_snapped_point.getSnapDistance()));
            EXPECT_EQ(transform.best_snapped_point.getTarget(), SNAPTARGET_CONSTRAINT);
            auto const accepted = transform.getStretchSnapped();
            EXPECT_DOUBLE_EQ(accepted[Geom::X], raw[Geom::X]);
            EXPECT_DOUBLE_EQ(accepted[Geom::Y], raw[Geom::Y]);
        }
    }
}

TEST_F(PureTransformSnapTest, UnmatchedRotationRemainsUnsnappedWithMovableSources)
{
    auto const origin = target - Geom::Point(80, 80);
    auto const source = origin + Geom::Point(20, 0);
    double const angle = 14 * std::numbers::pi / 180;
    auto const pointer = (source - origin) * Geom::Rotate(angle) + origin;
    PureRotateConstrained transform(angle, origin);
    snap(transform, source, pointer);

    EXPECT_FALSE(transform.best_snapped_point.getSnapped());
    EXPECT_TRUE(std::isinf(transform.best_snapped_point.getSnapDistance()));
    EXPECT_EQ(transform.best_snapped_point.getTarget(), SNAPTARGET_CONSTRAINT);
    EXPECT_DOUBLE_EQ(transform.getAngleSnapped(), angle);
}

TEST_F(PureTransformSnapTest, StretchKeepsRealMatchesIncludingZeroDistance)
{
    for (auto axis : {Geom::X, Geom::Y}) {
        for (bool uniform : {false, true}) {
            Geom::Point delta(30, 30);
            if (!uniform) delta[1 - axis] = 20;
            auto const origin = target - delta;
            auto const source = origin + Geom::Point(20, 20);
            for (double magnitude : {1.47, 1.5}) {
                SCOPED_TRACE(::testing::Message() << "axis=" << axis << " uniform=" << uniform
                                                 << " magnitude=" << magnitude);
                Geom::Scale raw(magnitude, magnitude);
                if (!uniform) raw[1 - axis] = 1;
                auto const pointer = (source - origin) * raw + origin;
                PureStretchConstrained transform(magnitude, origin, axis, uniform);
                snap(transform, source, pointer);

                ASSERT_TRUE(transform.best_snapped_point.getSnapped());
                EXPECT_NE(transform.best_snapped_point.getTarget(), SNAPTARGET_CONSTRAINT);
                EXPECT_NEAR(Geom::L2(transform.best_snapped_point.getPoint() - target), 0, 1e-8);
                auto const accepted = transform.getStretchSnapped();
                EXPECT_NEAR(accepted[axis], 1.5, 1e-9);
                EXPECT_NEAR(accepted[1 - axis], uniform ? 1.5 : 1.0, 1e-9);
                EXPECT_NEAR(transform.best_snapped_point.getSnapDistance(),
                            std::abs(1.5 - magnitude), 1e-9);
            }
        }
    }
}

TEST_F(PureTransformSnapTest, RotationKeepsRealMatchesIncludingZeroDistance)
{
    double const target_angle = 15 * std::numbers::pi / 180;
    auto const delta = Geom::Point(40, 0);
    auto const origin = target - delta * Geom::Rotate(target_angle);
    auto const source = origin + delta;
    for (double degrees : {14.0, 15.0}) {
        SCOPED_TRACE(degrees);
        double const angle = degrees * std::numbers::pi / 180;
        auto const pointer = delta * Geom::Rotate(angle) + origin;
        PureRotateConstrained transform(angle, origin);
        snap(transform, source, pointer);

        ASSERT_TRUE(transform.best_snapped_point.getSnapped());
        EXPECT_NE(transform.best_snapped_point.getTarget(), SNAPTARGET_CONSTRAINT);
        EXPECT_NEAR(Geom::L2(transform.best_snapped_point.getPoint() - target), 0, 1e-8);
        EXPECT_NEAR(transform.getAngleSnapped(), target_angle, 1e-9);
        EXPECT_NEAR(transform.best_snapped_point.getSnapDistance(),
                    std::abs(target_angle - angle), 1e-9);
    }
}

// ---------------------------------------------------------------------------------------------
// Degenerate geometry: zero-size objects, empty groups, flat lines and huge coordinates must be
// refused (document unchanged, no Undo step) or clamped; a transform is never written as NaN/Inf.
// ---------------------------------------------------------------------------------------------

namespace {

bool all_finite(Geom::Affine const &a)
{
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(a[i])) {
            return false;
        }
    }
    return true;
}

class DegenerateGeometryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!initialize_gui()) {
            GTEST_SKIP() << "GTK display unavailable; degenerate-geometry fixture skipped";
        }
        document = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
  <path id="zp" d="M 50,50 L 50,50" style="fill:none;stroke:none"/>
  <path id="hl" d="M 0,80 L 100,80" style="fill:none;stroke:none"/>
  <path id="vl" d="M 150,20 L 150,100" style="fill:none;stroke:none"/>
  <rect id="box" x="0" y="0" width="10" height="10"/>
</svg>)svg");
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        if (!Application::exists()) Application::create(false);
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        desktop->getCanvas()->size_allocate(Gtk::Allocation(0, 0, 256, 256), -1);
        // The desktop starts in the selector, whose SelTrans owns the real transform handles.
        auto *tool = dynamic_cast<Inkscape::UI::Tools::SelectTool *>(desktop->getTool());
        ASSERT_TRUE(tool);
        ASSERT_TRUE(tool->_seltrans);
        seltrans = tool->_seltrans;
    }

    void TearDown() override
    {
        desktop.reset();
        document.reset();
        auto context = Glib::MainContext::get_default();
        while (context->iteration(false)) {}
    }

    SPItem *item(char const *id) { return cast<SPItem>(document->getObjectById(id)); }
    std::string attr(char const *id, char const *name)
    {
        auto value = document->getObjectById(id)->getRepr()->attribute(name);
        return value ? value : "";
    }
    Inkscape::Selection *selection() { return desktop->getSelection(); }

    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    SelTrans *seltrans = nullptr;
};

} // namespace

// G3: W = 0 (and H = 0) typed into the select toolbar for a flat line divided 0/0.
TEST(DegenerateGeometryPure, ScaleTransformOfAFlatLineIsFiniteOrIdentity)
{
    Geom::Rect const flat(Geom::Point(0, 80), Geom::Point(100, 80)); // horizontal, zero height
    Geom::Rect const vertical(Geom::Point(150, 20), Geom::Point(150, 100));
    for (auto const &bbox : {flat, vertical}) {
        for (bool transform_stroke : {false, true}) {
            for (bool preserve : {false, true}) {
                for (auto const &target : {Geom::Rect(Geom::Point(0, 80), Geom::Point(0, 80)),
                                           Geom::Rect(Geom::Point(0, 80), Geom::Point(40, 80)),
                                           Geom::Rect(Geom::Point(0, 0), Geom::Point(0, 50))}) {
                    SCOPED_TRACE(::testing::Message() << "transform_stroke=" << transform_stroke
                                                      << " preserve=" << preserve);
                    EXPECT_TRUE(all_finite(get_scale_transform_for_uniform_stroke(
                        bbox, 0, 0, transform_stroke, preserve, target.min()[0], target.min()[1],
                        target.max()[0], target.max()[1])));
                    EXPECT_TRUE(all_finite(get_scale_transform_for_variable_stroke(
                        bbox, bbox, transform_stroke, preserve, target.min()[0], target.min()[1],
                        target.max()[0], target.max()[1])));
                }
            }
        }
    }
}

// Control for G3: an ordinary rectangle is still scaled exactly as before.
TEST(DegenerateGeometryPure, ScaleTransformOfARectangleIsUnchanged)
{
    Geom::Rect const box(Geom::Point(0, 0), Geom::Point(10, 20));
    auto const a = get_scale_transform_for_uniform_stroke(box, 0, 0, false, false, 0, 0, 20, 40);
    EXPECT_NEAR(a[0], 2, 1e-9);
    EXPECT_NEAR(a[3], 2, 1e-9);
    auto const b = get_scale_transform_for_variable_stroke(box, box, false, false, 0, 0, 20, 10);
    EXPECT_NEAR(b[0], 2, 1e-9);
    EXPECT_NEAR(b[3], 0.5, 1e-9);
}

// G3 (usability): a zero-extent axis keeps scale 1, the other axis scales normally.
TEST(DegenerateGeometryPure, WidthOfAHorizontalHairlineAndHeightOfAVerticalOneStillChange)
{
    Geom::Rect const horizontal(Geom::Point(0, 80), Geom::Point(100, 80));
    Geom::Rect const vertical(Geom::Point(150, 20), Geom::Point(150, 100));
    for (bool transform_stroke : {false, true}) {
        for (bool preserve : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "transform_stroke=" << transform_stroke << " preserve=" << preserve);
            for (int variant = 0; variant < 2; ++variant) {
                auto scale = [&](Geom::Rect const &box, double x0, double y0, double x1, double y1) {
                    return variant == 0
                        ? get_scale_transform_for_uniform_stroke(box, 0, 0, transform_stroke, preserve, x0, y0, x1, y1)
                        : get_scale_transform_for_variable_stroke(box, box, transform_stroke, preserve, x0, y0, x1, y1);
                };
                // W typed as 40 with H unchanged (0): the line becomes 40 wide and stays flat.
                auto a = scale(horizontal, 0, 80, 40, 80);
                ASSERT_TRUE(all_finite(a));
                auto w = horizontal * a;
                EXPECT_NEAR(w.width(), 40, 1e-9);
                EXPECT_NEAR(w.height(), 0, 1e-9);
                EXPECT_NEAR(w.left(), 0, 1e-9);
                // H typed as 30 on a vertical line: it becomes 30 high and stays flat.
                auto b = scale(vertical, 150, 20, 150, 50);
                ASSERT_TRUE(all_finite(b));
                auto h = vertical * b;
                EXPECT_NEAR(h.height(), 30, 1e-9);
                EXPECT_NEAR(h.width(), 0, 1e-9);
                EXPECT_NEAR(h.top(), 20, 1e-9);
            }
        }
    }
}

// Control for G3: W = 0 and H = 0 on a NORMAL rectangle give the same finite matrices as before the guard.
TEST(DegenerateGeometryPure, ZeroWidthOrHeightOnANormalRectangleBehavesAsBefore)
{
    Geom::Rect const box(Geom::Point(0, 0), Geom::Point(10, 20));
    // W = 0: flipped about the target edge and shifted by half the lost width (existing behaviour).
    auto w0 = get_scale_transform_for_uniform_stroke(box, 0, 0, false, false, 0, 0, 0, 20);
    EXPECT_NEAR(w0[0], -1, 1e-12);
    EXPECT_NEAR(w0[3], 1, 1e-12);
    EXPECT_NEAR(w0[4], 5, 1e-12);
    EXPECT_NEAR(w0[5], 0, 1e-12);
    // H = 0 likewise on the other axis.
    auto h0 = get_scale_transform_for_uniform_stroke(box, 0, 0, false, false, 0, 0, 10, 0);
    EXPECT_NEAR(h0[0], 1, 1e-12);
    EXPECT_NEAR(h0[3], -1, 1e-12);
    EXPECT_NEAR(h0[4], 0, 1e-12);
    EXPECT_NEAR(h0[5], 10, 1e-12);
    EXPECT_TRUE(all_finite(get_scale_transform_for_variable_stroke(box, box, true, false, 0, 0, 0, 20)));
    EXPECT_TRUE(all_finite(get_scale_transform_for_variable_stroke(box, box, true, false, 0, 0, 10, 0)));
}

// G2: the Grow key on a zero-extent object computed amount = 1 + 2/0 = inf.
TEST_F(DegenerateGeometryTest, GrowOfAZeroExtentObjectIsRefusedAndAFlatLineStillGrows)
{
    selection()->set(item("zp"));
    auto const mark = DocumentUndo::undoStackMark(document.get());
    selection()->scaleAnchored(2);
    selection()->scaleAnchored(-1);
    EXPECT_EQ(attr("zp", "d"), "M 50,50 L 50,50");
    EXPECT_EQ(attr("zp", "transform"), "");
    EXPECT_EQ(DocumentUndo::undoStackMark(document.get()), mark) << "a refused Grow must not add an Undo step";

    // Non-finite scales are rejected before they reach applyAffine, for any caller.
    selection()->scaleRelative(Geom::Point(50, 50), Geom::Scale(std::numeric_limits<double>::infinity()));
    selection()->scaleRelative(Geom::Point(50, 50), Geom::Scale(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_EQ(attr("zp", "d"), "M 50,50 L 50,50");

    // Control: a flat hairline has a non-zero extent (100), so Grow by 2 still works.
    selection()->set(item("hl"));
    selection()->scaleAnchored(2);
    document->ensureUpToDate();
    auto bbox = item("hl")->desktopVisualBounds();
    ASSERT_TRUE(bbox);
    EXPECT_NEAR(bbox->width(), 102, 1e-6);
}

// G3 through the helper the select toolbar uses: W changes a hairline; W = 0 on a flat hairline gives a finite
// matrix (the line keeps its length and stays flat) instead of NaN.
TEST_F(DegenerateGeometryTest, ToolbarResizeOfAHairlineChangesWidthAndZeroWidthIsFinite)
{
    selection()->set(item("hl"));
    auto refused = selection_resize_affine(*selection(), false, false, false, 0, 80, 0, 80);
    ASSERT_TRUE(refused);
    ASSERT_TRUE(all_finite(*refused));
    auto const before = item("hl")->desktopVisualBounds();
    ASSERT_TRUE(before);
    auto const same = *before * *refused;
    EXPECT_NEAR(same.width(), before->width(), 1e-6);
    EXPECT_NEAR(same.height(), 0, 1e-6);

    auto widen = selection_resize_affine(*selection(), false, false, false, 0, 80, 40, 80);
    ASSERT_TRUE(widen);
    EXPECT_FALSE(widen->isIdentity(1e-12));
    selection()->applyAffine(*widen);
    document->ensureUpToDate();
    auto bbox = item("hl")->desktopVisualBounds();
    ASSERT_TRUE(bbox);
    EXPECT_NEAR(bbox->width(), 40, 1e-6);
    EXPECT_NEAR(bbox->height(), 0, 1e-6);
}

// G6: an anchor that is not finite would stick and poison every later anchored transform.
TEST_F(DegenerateGeometryTest, SelectionRejectsANonFiniteAnchor)
{
    // The selector's SelTrans resets the anchor on every selection change; use a tool without one.
    desktop->setTool("/tools/nodes");
    seltrans = nullptr;
    selection()->set(item("box"));
    selection()->setAnchor(std::numeric_limits<double>::quiet_NaN(), 0.5);
    EXPECT_TRUE(std::isfinite(selection()->anchor.x()));
    selection()->setAnchor(0.5, std::numeric_limits<double>::infinity());
    EXPECT_TRUE(std::isfinite(selection()->anchor.y()));
    selection()->setAnchor(0.25, 0.75);
    EXPECT_DOUBLE_EQ(selection()->anchor.x(), 0.25);
    EXPECT_DOUBLE_EQ(selection()->anchor.y(), 0.75);
    selection()->setAnchor(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN());
    EXPECT_DOUBLE_EQ(selection()->anchor.x(), 0.25);
    EXPECT_DOUBLE_EQ(selection()->anchor.y(), 0.75);
}

// G6: selecting the centre handle of a zero-size selection made the anchor 0/0 = NaN.
TEST_F(DegenerateGeometryTest, CentreHandleOnAZeroSizeObjectAnchorsAtItsMiddle)
{
    selection()->set(item("zp"));
    ASSERT_FALSE(seltrans->isEmpty());
    SPKnot *centre = nullptr;
    for (int i = 0; i < NUMHANDS; ++i) {
        if (hands[i].type == HANDLE_CENTER) {
            centre = seltrans->handleKnot(i);
        }
    }
    ASSERT_TRUE(centre);
    centre->selectKnot(true);
    seltrans->increaseState(); // scale -> rotate state: shows the centre handle and sets the anchor
    ASSERT_TRUE(selection()->has_anchor);
    EXPECT_TRUE(std::isfinite(selection()->anchor.x()));
    EXPECT_TRUE(std::isfinite(selection()->anchor.y()));
    EXPECT_DOUBLE_EQ(selection()->anchor.x(), 0.5);
    EXPECT_DOUBLE_EQ(selection()->anchor.y(), 0.5);
}

// G5: dragging the side skew handle of a zero-width object divided 0/0 and returned a NaN point.
TEST_F(DegenerateGeometryTest, SkewDragOfAZeroWidthObjectIsRefused)
{
    selection()->set(item("vl")); // zero width, 80 high
    Geom::Point const start(150, 60);
    seltrans->grab(start, 1, 0.5, true, false, HANDLE_SKEW);
    ASSERT_TRUE(seltrans->isGrabbed());
    Geom::Point pt(160, 60);
    int const result = seltrans->skewRequest(pt, 0, true); // horizontal skew: dim_a = X, extent 0
    EXPECT_FALSE(result);
    EXPECT_TRUE(std::isfinite(pt.x()) && std::isfinite(pt.y()));
    seltrans->ungrab();
    EXPECT_EQ(attr("vl", "transform"), "");
    document->ensureUpToDate();
    auto bbox = item("vl")->desktopVisualBounds();
    ASSERT_TRUE(bbox);
    EXPECT_NEAR(bbox->left(), 150, 1e-9);
    EXPECT_NEAR(bbox->right(), 150, 1e-9);
    EXPECT_NEAR(bbox->top(), 20, 1e-9);
    EXPECT_NEAR(bbox->bottom(), 100, 1e-9);
}

// G4: an absolute skew on a flat object divided by its zero height/width (Inf).
TEST_F(DegenerateGeometryTest, AbsoluteSkewOfAFlatObjectIsRefusedAndOthersAreSkewed)
{
    auto const mark = DocumentUndo::undoStackMark(document.get());
    for (bool separately : {false, true}) {
        selection()->set(item("hl"));
        transform_skew(selection(), 5, 5, SkewUnits::Absolute, separately, 1.0);
        EXPECT_EQ(attr("hl", "d"), "M 0,80 L 100,80");
        EXPECT_EQ(attr("hl", "transform"), "") << "separately=" << separately;
        selection()->set(item("vl"));
        transform_skew(selection(), 5, 5, SkewUnits::Absolute, separately, 1.0);
        EXPECT_EQ(attr("vl", "transform"), "") << "separately=" << separately;
    }
    EXPECT_EQ(DocumentUndo::undoStackMark(document.get()), mark);

    // Applied separately, the degenerate member is skipped but a normal member is still skewed.
    selection()->set(item("hl"));
    selection()->add(item("box"));
    transform_skew(selection(), 5, 0, SkewUnits::Absolute, true, 1.0);
    EXPECT_EQ(attr("hl", "transform"), "");
    auto const box_transform = attr("box", "transform");
    EXPECT_FALSE(box_transform.empty());
    EXPECT_EQ(box_transform.find("nan"), std::string::npos);
    EXPECT_EQ(box_transform.find("inf"), std::string::npos);

    // Each term is guarded on its own: a vertical line sheared by x only, and a horizontal line sheared by y
    // only, need no extent along the zero displacement and are valid.
    selection()->set(item("vl"));
    transform_skew(selection(), 5, 0, SkewUnits::Absolute, false, 1.0);
    document->ensureUpToDate();
    auto vbox = item("vl")->desktopVisualBounds();
    ASSERT_TRUE(vbox);
    EXPECT_NEAR(vbox->width(), 5, 1e-6) << "x = 5, y = 0 shears a vertical line sideways by 5";
    selection()->set(item("hl"));
    transform_skew(selection(), 0, 5, SkewUnits::Absolute, false, 1.0);
    document->ensureUpToDate();
    auto hbox = item("hl")->desktopVisualBounds();
    ASSERT_TRUE(hbox);
    EXPECT_NEAR(hbox->height(), 5, 1e-6) << "x = 0, y = 5 shears a horizontal line vertically by 5";
}

// G7: a canvas position near 1e30 overflowed the int conversion in round().
TEST_F(DegenerateGeometryTest, CanvasPositionIsClampedAgainstIntOverflow)
{
    auto *canvas = desktop->getCanvas();
    constexpr int limit = 1 << 30;
    for (auto const &p : {Geom::Point(1e30, -1e30), Geom::Point(-1e300, 1e300),
                          Geom::Point(std::numeric_limits<double>::quiet_NaN(), 3e9)}) {
        canvas->set_pos(p);
        auto const pos = canvas->get_pos();
        EXPECT_LE(std::abs(pos.x()), limit);
        EXPECT_LE(std::abs(pos.y()), limit);
    }
    canvas->set_pos(Geom::Point(12.4, -7.6));
    EXPECT_EQ(canvas->get_pos(), Geom::IntPoint(12, -8));
}
