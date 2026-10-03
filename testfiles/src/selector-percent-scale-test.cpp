// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <memory>

#include <glibmm/main.h>
#include <gtk/gtk.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/settings.h>
#include <gtkmm/snapshot.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "io/resource.h"
#include "object/sp-item.h"
#include "selection.h"
#include "ui/toolbar/select-toolbar.h"
#include "ui/toolbar/transform-reference.h"
#include "ui/widget/gtk-registry.h"
#include "ui/widget/spinbutton.h"
#include "util/scope_exit.h"
#include "util/units.h"
#include "xml/repr.h"

using namespace Inkscape;

namespace Inkscape::UI::Toolbar {

struct SelectToolbarTestAccess
{
    static void set_horizontal_scale(SelectToolbar &toolbar, double percentage)
    {
        toolbar._scale_w_item.set_value(percentage);
    }

    static void set_vertical_scale(SelectToolbar &toolbar, double percentage)
    {
        toolbar._scale_h_item.set_value(percentage);
    }

    static double horizontal_scale(SelectToolbar &toolbar)
    {
        return toolbar._scale_w_item.get_value();
    }

    static double vertical_scale(SelectToolbar &toolbar)
    {
        return toolbar._scale_h_item.get_value();
    }

    static double absolute_width(SelectToolbar &toolbar)
    {
        return toolbar._w_item.get_value();
    }

    static Glib::ustring width_tooltip(SelectToolbar &toolbar)
    {
        return toolbar._w_item.get_tooltip_text();
    }

    static double absolute_height(SelectToolbar &toolbar)
    {
        return toolbar._h_item.get_value();
    }

    static double absolute_x(SelectToolbar &toolbar)
    {
        return toolbar._x_item.get_value();
    }

    static double absolute_y(SelectToolbar &toolbar)
    {
        return toolbar._y_item.get_value();
    }

    static void set_absolute_width(SelectToolbar &toolbar, double value)
    {
        toolbar._w_item.set_value(value);
    }

    static void set_absolute_height(SelectToolbar &toolbar, double value)
    {
        toolbar._h_item.set_value(value);
    }

    static void set_absolute_x(SelectToolbar &toolbar, double value)
    {
        toolbar._x_item.set_value(value);
    }

    static void set_locked(SelectToolbar &toolbar, bool locked)
    {
        toolbar._lock_btn.set_active(locked);
    }

    static void set_reference(SelectToolbar &toolbar, int index)
    {
        toolbar._reference_selector.setSelection(index, true);
    }

    static int reference(SelectToolbar &toolbar)
    {
        return toolbar._reference_selector.getSelection();
    }

    static bool reference_is_before_coordinates(SelectToolbar &toolbar)
    {
        auto *reference_box = toolbar._reference_selector.get_parent();
        auto *x_box = toolbar._x_item.get_parent();
        return reference_box && x_box && reference_box->get_next_sibling() == x_box;
    }

    static bool controls_are_between_height_and_lock(SelectToolbar &toolbar)
    {
        auto *height_box = toolbar._h_item.get_parent();
        auto *horizontal_scale_box = toolbar._scale_w_item.get_parent();
        auto *vertical_scale_box = toolbar._scale_h_item.get_parent();
        return height_box && horizontal_scale_box && vertical_scale_box &&
               height_box->get_next_sibling() == horizontal_scale_box &&
               horizontal_scale_box->get_next_sibling() == vertical_scale_box &&
               vertical_scale_box->get_next_sibling() == &toolbar._lock_btn;
    }

    static bool percentage_controls_are_sensitive(SelectToolbar &toolbar)
    {
        return toolbar._scale_w_item.is_sensitive() && toolbar._scale_h_item.is_sensitive();
    }
};

} // namespace Inkscape::UI::Toolbar

namespace {

using Inkscape::UI::Toolbar::SelectToolbar;
using Inkscape::UI::Toolbar::SelectToolbarTestAccess;
using Inkscape::UI::Toolbar::TransformReferencePoint;

InkscapeApplication *initialize_gui()
{
    static auto *app = [] {
        g_setenv("INKSCAPE_APP_ID_TAG", "selectorpercentscaletest", TRUE);
        auto *result = new InkscapeApplication(); // Process-lifetime test fixture.
        if (result->gtk_app()) {
            UI::Widget::register_all();
        }
        return result;
    }();
    return app->gtk_app() ? app : nullptr;
}

void drain_main_context()
{
    auto context = Glib::MainContext::get_default();
    while (context->iteration(false)) {}
}

class SelectorPercentScaleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!initialize_gui()) {
            GTEST_SKIP() << "GTK display unavailable; selector toolbar fixture skipped";
        }
        if (!Application::exists()) Application::create(false);

        document = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
  <rect id="first" x="10" y="20" width="40" height="20"/>
  <rect id="second" x="90" y="20" width="20" height="20"/>
</svg>)svg");
        ASSERT_TRUE(document);
        document->ensureUpToDate();

        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        ASSERT_TRUE(desktop);
        ASSERT_TRUE(item("first"));
        ASSERT_TRUE(item("second"));

        selection = desktop->getSelection();
        selection->set(item("first"));
        // The toolbar reference must remain independent of SelTrans/keyboard anchor state.
        selection->setAnchor(0.5, 0.5);

        Preferences::get()->setInt("/tools/select/toolbar_reference_point", 0);
        toolbar = std::make_unique<SelectToolbar>();
        toolbar->setDesktop(desktop.get());
        toolbar->setActiveUnit(Util::UnitTable::get().unit("px"));
        SelectToolbarTestAccess::set_locked(*toolbar, false);

        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        document->setModifiedSinceSave(false);
    }

    void TearDown() override
    {
        if (toolbar) toolbar->setDesktop(nullptr);
        toolbar.reset();
        desktop.reset();
        document.reset();
        drain_main_context();
    }

    SPItem *item(char const *id) const
    {
        return document ? cast<SPItem>(document->getObjectById(id)) : nullptr;
    }

    Geom::Rect bounds() const
    {
        auto result = selection->preferredBounds();
        EXPECT_TRUE(result);
        return result ? *result : Geom::Rect{};
    }

    void expect_size(double width, double height, double tolerance = 1e-6) const
    {
        auto const bbox = bounds();
        EXPECT_NEAR(bbox.width(), width, tolerance);
        EXPECT_NEAR(bbox.height(), height, tolerance);
    }

    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<SelectToolbar> toolbar;
    Selection *selection = nullptr;
};

} // namespace

TEST(TransformReferenceGeometryTest, AllNineMappingsAreStable)
{
    using namespace Inkscape::UI::Toolbar;

    for (int index = 0; index < 9; ++index) {
        auto const point = transform_reference_from_index(index);
        auto const normalized = normalized_reference(point);
        EXPECT_DOUBLE_EQ(normalized.x(), (index % 3) * 0.5);
        EXPECT_DOUBLE_EQ(normalized.y(), (index / 3) * 0.5);
        EXPECT_EQ(transform_reference_index(point), index);
    }

    EXPECT_EQ(transform_reference_from_index(-1), TransformReferencePoint::TopLeft);
    EXPECT_EQ(transform_reference_from_index(9), TransformReferencePoint::TopLeft);
}

TEST(TransformReferenceGeometryTest, ResizeKeepsTopLeftCenterAndBottomRightFixed)
{
    using namespace Inkscape::UI::Toolbar;

    Geom::Rect const bounds{Geom::Point{10, 20}, Geom::Point{110, 70}};

    auto top_left = resize_around_reference(bounds, 200, 100,
                                            TransformReferencePoint::TopLeft);
    EXPECT_DOUBLE_EQ(top_left.x0, 10);
    EXPECT_DOUBLE_EQ(top_left.y0, 20);
    EXPECT_DOUBLE_EQ(top_left.x1, 210);
    EXPECT_DOUBLE_EQ(top_left.y1, 120);

    auto center = resize_around_reference(bounds, 200, 100,
                                          TransformReferencePoint::Center);
    EXPECT_DOUBLE_EQ(center.x0, -40);
    EXPECT_DOUBLE_EQ(center.y0, -5);
    EXPECT_DOUBLE_EQ(center.x1, 160);
    EXPECT_DOUBLE_EQ(center.y1, 95);

    auto bottom_right = resize_around_reference(bounds, 200, 100,
                                                TransformReferencePoint::BottomRight);
    EXPECT_DOUBLE_EQ(bottom_right.x0, -90);
    EXPECT_DOUBLE_EQ(bottom_right.y0, -30);
    EXPECT_DOUBLE_EQ(bottom_right.x1, 110);
    EXPECT_DOUBLE_EQ(bottom_right.y1, 70);
}

TEST_F(SelectorPercentScaleTest, ControlsAreInlineBetweenHeightAndAspectLock)
{
    EXPECT_TRUE(SelectToolbarTestAccess::controls_are_between_height_and_lock(*toolbar));
    EXPECT_DOUBLE_EQ(SelectToolbarTestAccess::horizontal_scale(*toolbar), 100.0);
    EXPECT_DOUBLE_EQ(SelectToolbarTestAccess::vertical_scale(*toolbar), 100.0);
}

TEST_F(SelectorPercentScaleTest, ReferenceControlPrecedesCoordinatesAndDefaultsTopLeft)
{
    EXPECT_TRUE(SelectToolbarTestAccess::reference_is_before_coordinates(*toolbar));
    EXPECT_EQ(SelectToolbarTestAccess::reference(*toolbar), 0);
    EXPECT_NEAR(SelectToolbarTestAccess::absolute_x(*toolbar), 10.0, 1e-6);
    EXPECT_NEAR(SelectToolbarTestAccess::absolute_y(*toolbar), 20.0, 1e-6);
}

TEST_F(SelectorPercentScaleTest, ChoosingReferenceOnlyUpdatesReadoutAndPreference)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const selection_anchor = selection->anchor;
    auto const selection_has_anchor = selection->has_anchor;

    SelectToolbarTestAccess::set_reference(*toolbar, 8);

    EXPECT_EQ(SelectToolbarTestAccess::reference(*toolbar), 8);
    EXPECT_NEAR(SelectToolbarTestAccess::absolute_x(*toolbar), 50.0, 1e-6);
    EXPECT_NEAR(SelectToolbarTestAccess::absolute_y(*toolbar), 40.0, 1e-6);
    EXPECT_EQ(Preferences::get()->getInt("/tools/select/toolbar_reference_point"), 8);
    EXPECT_EQ(selection->has_anchor, selection_has_anchor);
    EXPECT_EQ(selection->anchor, selection_anchor);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorPercentScaleTest, EveryReferenceRemainsFixedDuringPercentageScaling)
{
    for (int index = 0; index < 9; ++index) {
        SelectToolbarTestAccess::set_reference(*toolbar, index);
        auto const before = bounds();
        auto const point = Inkscape::UI::Toolbar::reference_position(
            before, Inkscape::UI::Toolbar::transform_reference_from_index(index));

        SelectToolbarTestAccess::set_horizontal_scale(*toolbar, 200.0);
        document->ensureUpToDate();

        auto const after = bounds();
        auto const after_point = Inkscape::UI::Toolbar::reference_position(
            after, Inkscape::UI::Toolbar::transform_reference_from_index(index));
        EXPECT_NEAR(after.width(), before.width() * 2.0, 1e-6) << index;
        EXPECT_NEAR(after.height(), before.height(), 1e-6) << index;
        EXPECT_NEAR(after_point.x(), point.x(), 1e-6) << index;
        EXPECT_NEAR(after_point.y(), point.y(), 1e-6) << index;

        ASSERT_TRUE(DocumentUndo::undo(document.get())) << index;
        document->ensureUpToDate();
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
    }
}

TEST_F(SelectorPercentScaleTest, AbsoluteLockedScalingKeepsCenterFixed)
{
    SelectToolbarTestAccess::set_reference(*toolbar, 4);
    SelectToolbarTestAccess::set_locked(*toolbar, true);
    auto const before = bounds();
    auto const center = before.midpoint();

    SelectToolbarTestAccess::set_absolute_width(*toolbar, 80.0);
    document->ensureUpToDate();

    auto const after = bounds();
    EXPECT_NEAR(after.width(), 80.0, 1e-6);
    EXPECT_NEAR(after.height(), 40.0, 1e-6);
    EXPECT_NEAR(after.midpoint().x(), center.x(), 1e-6);
    EXPECT_NEAR(after.midpoint().y(), center.y(), 1e-6);
}

TEST_F(SelectorPercentScaleTest, AbsolutePositionUsesSelectedReference)
{
    SelectToolbarTestAccess::set_reference(*toolbar, 8);
    SelectToolbarTestAccess::set_absolute_x(*toolbar, 100.0);
    document->ensureUpToDate();

    auto const after = bounds();
    EXPECT_NEAR(after.right(), 100.0, 1e-6);
    EXPECT_NEAR(after.bottom(), 40.0, 1e-6);
}

TEST_F(SelectorPercentScaleTest, HorizontalScaleIsRelativeAndUndoRedoRoundTrips)
{
    expect_size(40.0, 20.0);
    SelectToolbarTestAccess::set_horizontal_scale(*toolbar, 150.0);
    document->ensureUpToDate();

    expect_size(60.0, 20.0);
    EXPECT_NEAR(SelectToolbarTestAccess::absolute_width(*toolbar), 60.0, 1e-6);
    EXPECT_NEAR(SelectToolbarTestAccess::absolute_height(*toolbar), 20.0, 1e-6);
    EXPECT_DOUBLE_EQ(SelectToolbarTestAccess::horizontal_scale(*toolbar), 100.0);
    EXPECT_DOUBLE_EQ(SelectToolbarTestAccess::vertical_scale(*toolbar), 100.0);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    expect_size(40.0, 20.0);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    expect_size(60.0, 20.0);
}

TEST_F(SelectorPercentScaleTest, VerticalScaleLeavesWidthUnchangedWhenUnlocked)
{
    SelectToolbarTestAccess::set_vertical_scale(*toolbar, 50.0);
    document->ensureUpToDate();

    expect_size(40.0, 10.0);
    EXPECT_DOUBLE_EQ(SelectToolbarTestAccess::horizontal_scale(*toolbar), 100.0);
    EXPECT_DOUBLE_EQ(SelectToolbarTestAccess::vertical_scale(*toolbar), 100.0);
}

TEST_F(SelectorPercentScaleTest, AspectLockUsesTheEditedPercentageForBothAxes)
{
    SelectToolbarTestAccess::set_locked(*toolbar, true);
    SelectToolbarTestAccess::set_horizontal_scale(*toolbar, 150.0);
    document->ensureUpToDate();

    expect_size(60.0, 30.0);
}

TEST_F(SelectorPercentScaleTest, MultipleObjectsScaleAtomicallyAsOneSelection)
{
    selection->add(item("second"));
    selection->setAnchor(0.5, 0.5);
    auto const before = bounds();
    DocumentUndo::clearUndo(document.get());
    DocumentUndo::clearRedo(document.get());

    SelectToolbarTestAccess::set_horizontal_scale(*toolbar, 50.0);
    document->ensureUpToDate();

    auto const after = bounds();
    EXPECT_NEAR(after.width(), before.width() * 0.5, 1e-6);
    EXPECT_NEAR(after.height(), before.height(), 1e-6);
    EXPECT_EQ(selection->size(), 2u);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    auto const restored = bounds();
    EXPECT_NEAR(restored.width(), before.width(), 1e-6);
    EXPECT_NEAR(restored.height(), before.height(), 1e-6);
    EXPECT_EQ(selection->size(), 2u);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorPercentScaleTest, ToolbarReferenceIsIndependentFromSelectionAnchor)
{
    selection->setAnchor(1.0, 1.0);
    SelectToolbarTestAccess::set_reference(*toolbar, 0);
    auto const before = bounds();

    SelectToolbarTestAccess::set_horizontal_scale(*toolbar, 200.0);
    document->ensureUpToDate();

    auto const after = bounds();
    EXPECT_NEAR(after.left(), before.left(), 1e-6);
    EXPECT_NEAR(after.top(), before.top(), 1e-6);
    EXPECT_NEAR(after.width(), before.width() * 2.0, 1e-6);
    EXPECT_NEAR(after.height(), before.height(), 1e-6);
}

TEST_F(SelectorPercentScaleTest, EmptySelectionDisablesControlsAndCannotModifyDocument)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    selection->clear();
    drain_main_context();

    EXPECT_FALSE(SelectToolbarTestAccess::percentage_controls_are_sensitive(*toolbar));
    SelectToolbarTestAccess::set_horizontal_scale(*toolbar, 150.0);

    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_DOUBLE_EQ(SelectToolbarTestAccess::horizontal_scale(*toolbar), 100.0);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorPercentScaleTest, RelativeScaleAndAbsoluteMoveRemainSeparateUndoSteps)
{
    auto const initial = bounds();
    SelectToolbarTestAccess::set_horizontal_scale(*toolbar, 150.0);
    document->ensureUpToDate();
    expect_size(60.0, 20.0);
    EXPECT_NEAR(bounds().left(), initial.left(), 1e-6);

    SelectToolbarTestAccess::set_absolute_x(*toolbar, 20.0);
    document->ensureUpToDate();
    expect_size(60.0, 20.0);
    EXPECT_NEAR(bounds().left(), 20.0, 1e-6);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    expect_size(60.0, 20.0);
    EXPECT_NEAR(bounds().left(), initial.left(), 1e-6);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    expect_size(40.0, 20.0);
    EXPECT_NEAR(bounds().left(), initial.left(), 1e-6);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorPercentScaleTest, ZeroPercentUsesSafeNonSingularDimension)
{
    SelectToolbarTestAccess::set_horizontal_scale(*toolbar, 0.0);
    document->ensureUpToDate();

    auto const bbox = bounds();
    EXPECT_GT(bbox.width(), 0.0);
    EXPECT_LT(bbox.width(), 1e-4);
    EXPECT_NEAR(bbox.height(), 20.0, 1e-6);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    expect_size(40.0, 20.0);
}

TEST_F(SelectorPercentScaleTest, RelativeScalingWorksBelowAbsoluteFieldRoundingThreshold)
{
    item("first")->getRepr()->setAttribute("width", "0.0001");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Tiny test setup"}, "");
    document->ensureUpToDate();
    DocumentUndo::clearUndo(document.get());
    DocumentUndo::clearRedo(document.get());

    expect_size(0.0001, 20.0, 1e-9);
    SelectToolbarTestAccess::set_horizontal_scale(*toolbar, 200.0);
    document->ensureUpToDate();

    expect_size(0.0002, 20.0, 1e-9);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    expect_size(0.0001, 20.0, 1e-9);
}

TEST_F(SelectorPercentScaleTest, ToolbarCanBeRenderedForVisualRegressionReview)
{
    auto settings = Gtk::Settings::get_default();
    ASSERT_TRUE(settings);
    settings->property_gtk_application_prefer_dark_theme() = true;

    auto display = Gdk::Display::get_default();
    ASSERT_TRUE(display);
    auto icon_theme = Gtk::IconTheme::get_for_display(display);
    auto icon_paths = icon_theme->get_search_path();
    icon_paths.insert(icon_paths.begin(),
                      IO::Resource::get_path_string(IO::Resource::SYSTEM,
                                                    IO::Resource::ICONS));
    icon_theme->set_search_path(icon_paths);

    Gtk::Window window;
    auto cleanup = scope_exit{[&] {
        window.unset_child();
        window.close();
        drain_main_context();
    }};
    window.set_default_size(1600, 72);
    window.set_child(*toolbar);
    window.present();

    // An old allocation alone does not mean the newly presented window has
    // completed layout. Snapshot only after its native frame has been painted.
    auto *clock = gtk_widget_get_frame_clock(GTK_WIDGET(window.gobj()));
    ASSERT_TRUE(clock);
    bool painted = false;
    auto connection = g_signal_connect_after(clock, "after-paint", G_CALLBACK(+[](GdkFrameClock *, gpointer data) {
        *static_cast<bool *>(data) = true;
    }), &painted);
    auto disconnect = scope_exit{[&] { g_signal_handler_disconnect(clock, connection); }};
    gtk_widget_queue_draw(GTK_WIDGET(window.gobj()));
    auto const deadline = g_get_monotonic_time() + 5000000;
    auto context = Glib::MainContext::get_default();
    while (!painted && g_get_monotonic_time() < deadline) {
        for (unsigned i = 0; i < 64 && !painted && context->iteration(false); ++i) {}
        if (!painted) g_usleep(1000);
    }
    ASSERT_TRUE(painted) << "Native toolbar frame did not finish painting";

    ASSERT_GT(toolbar->get_allocated_width(), 1);
    ASSERT_GT(toolbar->get_allocated_height(), 1);

    auto snapshot = Gtk::Snapshot::create();
    gtk_widget_snapshot_child(GTK_WIDGET(window.gobj()), toolbar->gobj(), snapshot->gobj());
    auto *node = gtk_snapshot_to_node(snapshot->gobj());
    ASSERT_TRUE(node);

    auto surface = window.get_surface();
    ASSERT_TRUE(surface);
    auto *renderer = gsk_renderer_new_for_surface(surface->gobj());
    ASSERT_TRUE(renderer);
    auto *texture = gsk_renderer_render_texture(renderer, node, nullptr);
    ASSERT_TRUE(texture);

    if (auto const *path = std::getenv("INKSCAPE_SELECTOR_PERCENT_SCREENSHOT")) {
        EXPECT_TRUE(gdk_texture_save_to_png(texture, path));
    }

    g_object_unref(texture);
    gsk_renderer_unrealize(renderer);
    g_object_unref(renderer);
    gsk_render_node_unref(node);
}

TEST_F(SelectorPercentScaleTest, FieldsFollowTheDocumentUnitWithoutAUnitMenu)
{
    auto has_unit_menu = [](auto &&self, Gtk::Widget *widget) -> bool {
        if (widget->get_name() == "unit-tracker") return true;
        for (auto *child = widget->get_first_child(); child; child = child->get_next_sibling()) {
            if (self(self, child)) return true;
        }
        return false;
    };
    EXPECT_FALSE(has_unit_menu(has_unit_menu, toolbar.get()));

    toolbar->setActiveUnit(Util::UnitTable::get().unit("mm"));
    drain_main_context();
    EXPECT_NEAR(SelectToolbarTestAccess::absolute_width(*toolbar), 40 * 25.4 / 96, 1e-3);
    EXPECT_EQ(SelectToolbarTestAccess::width_tooltip(*toolbar), "Width of selection (mm)");
}
