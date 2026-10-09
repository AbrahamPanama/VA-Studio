// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <locale>
#include <memory>
#include <string>
#include <sstream>
#include <span>
#include <string_view>
#include <vector>

#include <2geom/transforms.h>
#include <glibmm/main.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/settings.h>
#include <gtkmm/snapshot.h>
#include <gtkmm/window.h>
#include <glibmm/miscutils.h>

#include <cairo.h>
#include <gdkmm/surface.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "ui/cursor-utils.h"
#include "rubberband.h"

#include "desktop.h"
#include "display/control/canvas-item-drawing.h"
#include "display/control/canvas-item-group.h"
#include "display/control/canvas-item-picture.h"
#include "display/control/canvas-item-text.h"
#include "display/drawing.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "helper/png-write.h"
#include "object/sp-item.h"
#include "object/sp-namedview.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "selection.h"
#include "selection-dimensional-detents.h"
#include "selection-resize-feedback.h"
#include "selection-rotation-feedback.h"
#include "seltrans.h"
#include "seltrans-handles.h"
#include "snap.h"
#include "ui/tools/select-tool.h"
#include "ui/tools/pages-tool.h"
#include "ui/tools/selector-interaction.h"
#include "ui/modifiers.h"
#include "ui/knot/knot.h"
#include "ui/widget/canvas.h"
#include "ui/widget/events/canvas-event.h"
#include "util/units.h"
#include "xml/repr.h"
#include "xml/document.h"
#include "xml/node-observer.h"

namespace Inkscape {
class CanvasItemTextTestAccess
{
public:
    static Pango::Layout *layout(CanvasItemText const &item) { return item._layout.get(); }
    static Glib::ustring const &requested_text(CanvasItemText const &item) { return item._requested_text; }
    static double requested_fontsize(CanvasItemText const &item) { return item._requested_fontsize; }
    static double requested_border(CanvasItemText const &item) { return item._requested_border; }
};
} // namespace Inkscape

using namespace Inkscape;
using namespace Inkscape::UI::Tools;

namespace {

InkscapeApplication *initialize_gui()
{
    static auto *app = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "selectorinteractiontest", TRUE);
        auto result = new InkscapeApplication(); // Process-lifetime test fixture.
        // Run the normal GUI startup before any private SPDocument can create
        // the legacy singleton in headless mode. Real windows need the custom
        // GtkBuilder types registered by that startup (as in notebook tests).
        result->gio_app()->register_application();
        // Fatal test failures must terminate with their original signal, not
        // wait indefinitely in the application's interactive crash dialog.
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

void drain_main_context()
{
    auto context = Glib::MainContext::get_default();
    while (context->iteration(false)) {}
}

bool wait_for_paint(Gtk::Window &window)
{
    auto *clock = gtk_widget_get_frame_clock(GTK_WIDGET(window.gobj()));
    if (!clock) return false;
    bool painted = false;
    auto const connection = g_signal_connect_after(clock, "after-paint", G_CALLBACK(+[](GdkFrameClock *, gpointer data) {
        *static_cast<bool *>(data) = true;
    }), &painted);
    gtk_widget_queue_draw(GTK_WIDGET(window.gobj()));
    auto const deadline = g_get_monotonic_time() + 5000000;
    auto context = Glib::MainContext::get_default();
    while (!painted && g_get_monotonic_time() < deadline) {
        for (unsigned i = 0; i < 64 && !painted && context->iteration(false); ++i) {}
        if (!painted) g_usleep(1000);
    }
    g_signal_handler_disconnect(clock, connection);
    return painted;
}

bool save_canvas(Gtk::Window &window, Gtk::Widget &canvas, std::string const &path)
{
    if (!wait_for_paint(window)) return false;
    auto snapshot = Gtk::Snapshot::create();
    gtk_widget_snapshot_child(GTK_WIDGET(window.gobj()), GTK_WIDGET(canvas.gobj()), snapshot->gobj());
    auto *node = gtk_snapshot_to_node(snapshot->gobj());
    if (!node) return false;
    auto surface = window.get_surface();
    if (!surface) {
        gsk_render_node_unref(node);
        return false;
    }
    auto *renderer = gsk_renderer_new_for_surface(surface->gobj());
    if (!renderer) {
        gsk_render_node_unref(node);
        return false;
    }
    auto *texture = gsk_renderer_render_texture(renderer, node, nullptr);
    bool const saved = texture && gdk_texture_save_to_png(texture, path.c_str());
    if (texture) g_object_unref(texture);
    gsk_renderer_unrealize(renderer);
    g_object_unref(renderer);
    gsk_render_node_unref(node);
    return saved;
}

bool affine_near(Geom::Affine const &lhs, Geom::Affine const &rhs, double epsilon = 1e-8)
{
    for (unsigned i = 0; i < 6; ++i) {
        if (std::abs(lhs[i] - rhs[i]) > epsilon) {
            return false;
        }
    }
    return true;
}

struct DrawingPixels {
    Geom::IntRect area;
    int stride = 0;
    std::vector<uint32_t> pixels;

    std::array<unsigned, 4> at(Geom::Point point) const
    {
        auto const x = static_cast<int>(std::lround(point.x() - area.left()));
        auto const y = static_cast<int>(std::lround(point.y() - area.top()));
        auto const pixel = pixels.at(static_cast<size_t>(y) * (stride / 4) + x);
        return {static_cast<unsigned>((pixel >> 16) & 0xff),
                static_cast<unsigned>((pixel >> 8) & 0xff),
                static_cast<unsigned>(pixel & 0xff),
                static_cast<unsigned>((pixel >> 24) & 0xff)};
    }
};

DrawingPixels render_drawing(SPDesktop &desktop)
{
    auto *drawing = desktop.getCanvasDrawing()->get_drawing();
    drawing->update(Geom::IntRect::infinite(), desktop.doc2dt(), DrawingItem::STATE_ALL);
    auto const area = *drawing->root()->drawbox();
    DrawingSurface surface(area);
    DrawingContext context(surface);
    drawing->render(context, area);
    cairo_surface_flush(surface.raw());
    auto const stride = cairo_image_surface_get_stride(surface.raw());
    auto const *data = reinterpret_cast<uint32_t const *>(cairo_image_surface_get_data(surface.raw()));
    auto const count = static_cast<size_t>(stride / 4) * area.height();
    return {area, stride, std::vector<uint32_t>(data, data + count)};
}

class CommaDecimal final : public std::numpunct<char>
{
protected:
    char do_decimal_point() const override { return ','; }
    std::string do_grouping() const override { return {}; }
};

class DispatchObserver final : public XML::NodeObserver
{
public:
    std::function<void()> added;
    void notifyChildAdded(XML::Node &, XML::Node &, XML::Node *) override {
        if (added) added();
    }
};

// App-owned acceptance fixtures use the real close API. A privately owned
// document must instead remain owned until its synchronous operation returns.
struct RegisteredTestDocument {
    InkscapeApplication &application;
    SPDocument *document;
    SPDesktop *desktop = nullptr;
    unsigned destroyed = 0;
    sigc::connection destroy_connection;

    explicit RegisteredTestDocument(InkscapeApplication &app)
        : application(app)
        , document(app.document_add(SPDocument::createNewDocFromMem(
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect id="a" x="10" y="10" width="10" height="10"/><rect id="b" x="40" y="40" width="10" height="10"/></svg>)")))
    {
        document->ensureUpToDate();
        destroy_connection = document->connectDestroy([this] {
            ++destroyed;
            document = nullptr;
            desktop = nullptr;
        });
    }
    ~RegisteredTestDocument() {
        if (document) {
            document->setModifiedSinceSave(false);
            if (desktop) application.destroyDesktop(desktop);
            else application.document_close(document);
        }
        drain_main_context();
        destroy_connection.disconnect();
    }
};

Geom::Point lower_right(Geom::Rect const &bbox)
{
    return {bbox.right(), bbox.bottom()};
}

void begin_corner_resize(SelTrans &seltrans, SPDesktop &desktop, Geom::Rect const &bbox)
{
    // grab() accepts the handle coordinates before SelTrans applies the desktop Y convention.
    double const handle_y = desktop.yaxisdown() ? 0.0 : 1.0;
    seltrans.grab(lower_right(bbox), 1.0, handle_y, false, false, HANDLE_SCALE);
}

void begin_horizontal_stretch(SelTrans &seltrans, Geom::Rect const &bbox)
{
    seltrans.grab({bbox.right(), bbox.midpoint()[Geom::Y]}, 1.0, 0.5,
                  false, false, HANDLE_STRETCH);
}

void begin_vertical_stretch(SelTrans &seltrans, Geom::Rect const &bbox)
{
    seltrans.grab({bbox.midpoint()[Geom::X], bbox.bottom()}, 0.5, 0.0,
                  false, false, HANDLE_STRETCH);
}

Geom::Affine rotation_about(Geom::Point const &pivot, double degrees)
{
    return Geom::Translate(-pivot) * Geom::Rotate(Geom::rad_from_deg(degrees)) *
           Geom::Translate(pivot);
}

Geom::Point begin_rotation(SelTrans &seltrans, SPDesktop &desktop,
                           Geom::Point const &pivot, double radius_px = 100.0, bool stepped = true)
{
    auto const start = pivot + Geom::Point(radius_px / desktop.current_zoom(), 0);
    seltrans.resetState(SelTrans::STATE_ROTATE);
    seltrans.grab(start, 1.0, stepped ? 0.0 : 1.0, false, false, HANDLE_ROTATE);
    return start;
}

void enable_only_dimensional_snapping(SPDesktop &desktop)
{
    auto &snap_preferences = desktop.getNamedView()->snap_manager.snapprefs;
    snap_preferences.setSnapEnabledGlobally(true);
    snap_preferences.setSnapPostponedGlobally(false);
    snap_preferences.clearTargetMask(0);
    Preferences::get()->setBool("/options/snapdimensional/value", true);
}

void enable_dimensional_and_bbox_corner_snapping(SPDesktop &desktop)
{
    auto &snap_preferences = desktop.getNamedView()->snap_manager.snapprefs;
    snap_preferences.setSnapEnabledGlobally(true);
    snap_preferences.setSnapPostponedGlobally(false);
    snap_preferences.clearTargetMask(0);
    snap_preferences.setTargetMask(SNAPTARGET_BBOX_CATEGORY, 1);
    snap_preferences.setTargetMask(SNAPTARGET_BBOX_CORNER, 1);
    Preferences::get()->setBool("/options/snapdimensional/value", true);
}

void enable_dimensional_and_bbox_edge_midpoint_snapping(SPDesktop &desktop)
{
    auto &snap_preferences = desktop.getNamedView()->snap_manager.snapprefs;
    snap_preferences.setSnapEnabledGlobally(true);
    snap_preferences.setSnapPostponedGlobally(false);
    snap_preferences.clearTargetMask(0);
    snap_preferences.setTargetMask(SNAPTARGET_BBOX_CATEGORY, 1);
    snap_preferences.setTargetMask(SNAPTARGET_BBOX_EDGE_MIDPOINT, 1);
    Preferences::get()->setBool("/options/snapdimensional/value", true);
}

void enable_dimensional_and_special_point_snapping(SPDesktop &desktop)
{
    auto &snap_preferences = desktop.getNamedView()->snap_manager.snapprefs;
    snap_preferences.setSnapEnabledGlobally(true);
    snap_preferences.setSnapPostponedGlobally(false);
    snap_preferences.clearTargetMask(0);
    snap_preferences.setTargetMask(SNAPTARGET_NODE_CATEGORY, 1);
    snap_preferences.setTargetMask(SNAPTARGET_NODE_CUSP, 1);
    Preferences::get()->setBool("/options/snapdimensional/value", true);
}

class SelectorInteractionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!initialize_gui()) {
            GTEST_SKIP() << "GTK display unavailable; selector GUI fixture skipped";
        }
        if (!Application::exists()) Application::create(false);

        document = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="inside" x="10" y="10" width="10" height="10"/>
  <rect id="crossing" x="18" y="18" width="15" height="15"/>
  <path id="triangle" d="M 10,50 L 30,50 L 10,70 Z" style="fill:black;stroke:none"/>
  <ellipse id="second" cx="55" cy="60" rx="10" ry="10" style="fill:red;stroke:none"/>
  <image id="bitmap" x="40" y="10" width="10" height="10" preserveAspectRatio="none"
         href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLx9QAAAABJRU5ErkJggg=="/>
  <image id="alpha-bitmap" x="70" y="10" width="8" height="8" preserveAspectRatio="none"
         href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAGElEQVR4nGNgQAL/gQgdM1CuAJvgYFMAAAi/bZNkj2MnAAAAAElFTkSuQmCC"/>
</svg>)svg");
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        ASSERT_TRUE(desktop);

        // The production desktop widget normally allocates the canvas. This focused fixture has
        // no desktop chrome, but SnapManager still needs a real viewport for candidate culling.
        desktop->getCanvas()->size_allocate(Gtk::Allocation(0, 0, 256, 256), -1);
        ASSERT_GT(desktop->getCanvas()->get_dimensions().x(), 1);
        ASSERT_GT(desktop->getCanvas()->get_dimensions().y(), 1);

        tool = dynamic_cast<SelectTool *>(desktop->getTool());
        ASSERT_TRUE(tool);
        ASSERT_TRUE(item("inside"));
        desktop->getSelection()->set(item("inside"));
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Test setup"}, "");
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        document->setModifiedSinceSave(false);
        Preferences::get()->setBool("/options/snapdimensional/value", true);
        Preferences::get()->setBool("/tools/bounding_box", false);
        Preferences::get()->setBool("/options/transform/stroke", true);
        Preferences::get()->setBool("/options/preservetransform/value", false);
        Preferences::get()->setBool("/tools/select/lock_aspect_ratio", false);
        // These tests cover the live preview; the picture preview for moves
        // has its own tests (MovePicture*).
        Preferences::get()->setBool("/tools/select/fast_move_preview", false);
    }

    void TearDown() override
    {
        if (desktop) {
            desktop->getNamedView()->snap_manager.snapprefs.clearTargetMask();
        }
        Preferences::get()->setBool("/options/snapdimensional/value", true);
        Preferences::get()->setBool("/tools/bounding_box", false);
        Preferences::get()->setBool("/options/transform/stroke", true);
        Preferences::get()->setBool("/options/preservetransform/value", false);
        Preferences::get()->setBool("/tools/select/lock_aspect_ratio", false);
        Preferences::get()->remove("/tools/select/fast_move_preview");
        desktop.reset();
        document.reset();
        drain_main_context();
    }

    SPItem *item(char const *id) const
    {
        return document ? cast<SPItem>(document->getObjectById(id)) : nullptr;
    }

    void set_rect(char const *id, char const *x, char const *y,
                  char const *width, char const *height, char const *style = nullptr)
    {
        auto rect = item(id);
        ASSERT_TRUE(rect);
        {
            DocumentUndo::ScopedInsensitive no_undo(document.get());
            auto repr = rect->getRepr();
            repr->setAttribute("x", x);
            repr->setAttribute("y", y);
            repr->setAttribute("width", width);
            repr->setAttribute("height", height);
            if (style) {
                repr->setAttribute("style", style);
            }
        }
        document->ensureUpToDate();
        document->setModifiedSinceSave(false);
    }

    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    SelectTool *tool = nullptr;
};

// Picking applies a cursor tolerance (8 user units by default), which lets neighbouring
// fixture rects shadow each other. Tests that assert the exact hovered target pin the
// tolerance to zero for their duration and restore the previous value afterwards.
class ScopedCursorTolerance
{
public:
    explicit ScopedCursorTolerance(double value)
        : _saved(Preferences::get()->getDouble("/options/cursortolerance/value", 1.0))
    {
        Preferences::get()->setDouble("/options/cursortolerance/value", value);
    }

    ~ScopedCursorTolerance()
    {
        Preferences::get()->setDouble("/options/cursortolerance/value", _saved);
    }

    ScopedCursorTolerance(ScopedCursorTolerance const &) = delete;
    ScopedCursorTolerance &operator=(ScopedCursorTolerance const &) = delete;

private:
    double _saved;
};

} // namespace

TEST_F(SelectorInteractionTest, CanvasTextMeasurementUsesRequestedStateDuringSnapshot)
{
    CanvasItemContext context(desktop->getCanvas());
    auto *label = new CanvasItemText(context.root(), {0, 0}, "x");
    context.root()->update(false);

    auto *const initial_layout = CanvasItemTextTestAccess::layout(*label);
    ASSERT_NE(initial_layout, nullptr);
    auto const initial_size = label->get_text_size();

    context.snapshot();
    label->set_text("Canvas dimension width 1234.56 mm");
    label->set_fontsize(18);
    label->set_border(8);
    EXPECT_EQ(CanvasItemTextTestAccess::requested_text(*label), "Canvas dimension width 1234.56 mm");
    EXPECT_DOUBLE_EQ(CanvasItemTextTestAccess::requested_fontsize(*label), 18);
    EXPECT_DOUBLE_EQ(CanvasItemTextTestAccess::requested_border(*label), 8);
    auto const requested_size = label->get_text_size();

    EXPECT_GT(requested_size.width(), initial_size.width());
    EXPECT_GT(requested_size.height(), initial_size.height());
    EXPECT_EQ(CanvasItemTextTestAccess::layout(*label), initial_layout);

    context.unsnapshot();
    EXPECT_EQ(CanvasItemTextTestAccess::layout(*label), initial_layout);
    context.root()->update(false);

    auto *const updated_layout = CanvasItemTextTestAccess::layout(*label);
    ASSERT_NE(updated_layout, nullptr);
    EXPECT_NE(updated_layout, initial_layout);
    auto const extents = updated_layout->get_pixel_logical_extents();
    EXPECT_NEAR(requested_size.width(), extents.get_width() + 16, 1e-9);
    EXPECT_NEAR(requested_size.height(), extents.get_height() + 16, 1e-9);
}

TEST_F(SelectorInteractionTest, ResizeOverlayStaysStableDuringTileRendering)
{
    if (!g_getenv("INKSCAPE_TEST_GUI")) GTEST_SKIP() << "set INKSCAPE_TEST_GUI=1 to run the canvas tile stress case";

    auto *const canvas = desktop->getCanvas();
    Gtk::Window window;
    window.set_default_size(900, 700);
    window.set_child(*canvas);
    window.present();
    ASSERT_TRUE(wait_for_paint(window));

    desktop->getNamedView()->snap_manager.snapprefs.setSnapEnabledGlobally(false);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto &seltrans = *tool->_seltrans;
    begin_corner_resize(seltrans, *desktop, *bbox);

    constexpr int resize_steps = 2000;
    auto resize_to = [&](int i) {
        double const fraction = static_cast<double>(i) / (resize_steps - 1);
        double const sx = 0.5 + fraction * 1.5;
        double const sy = 2.0 - fraction * 1.25;
        auto point = bbox->min() + bbox->dimensions() * Geom::Scale(sx, sy);
        return seltrans.scaleRequest(point, 0);
    };
    ASSERT_TRUE(resize_to(0));
    ASSERT_TRUE(seltrans.hasResizeDimensionOverlay());

    // The mapped canvas starts tile rendering with the dimension overlay already present.
    canvas->redraw_all();
    drain_main_context();
    ASSERT_TRUE(wait_for_paint(window));

    for (int i = 1; i < resize_steps; ++i) {
        ASSERT_TRUE(resize_to(i)) << "resize step " << i;
        // Repeatedly schedule full canvas tiles while the selector updates the dimension labels.
        if ((i + 1) % 100 == 0) {
            canvas->redraw_all();
            drain_main_context();
        }
    }

    EXPECT_TRUE(seltrans.cancel());
    EXPECT_FALSE(seltrans.hasResizeDimensionOverlay());
    canvas->redraw_all();
    EXPECT_TRUE(wait_for_paint(window));
    window.unset_child();
    window.close();
    drain_main_context();
}

TEST_F(SelectorInteractionTest, TransformKnotsDeliverDirectionalCanvasCursors)
{
    auto *canvas = desktop->getCanvas();
    ASSERT_TRUE(canvas->get_cursor());
    auto &widget = dynamic_cast<Gtk::Widget &>(*canvas);
    auto check = [&](char const *view, char const *scale, char const *stretch, char const *skew) {
        SCOPED_TRACE(view);
        auto const theme = Gtk::IconTheme::get_for_display(widget.get_display());
        auto const paths = theme->get_search_path();
        auto const prefs = Preferences::get();
        std::vector<std::string> themes;
        auto preferred = prefs->getString("/theme/iconTheme", prefs->getString("/theme/defaultIconTheme", ""));
        if (!preferred.empty()) themes.push_back(preferred);
        Glib::ustring const system_theme = Gtk::Settings::get_default()->property_gtk_icon_theme_name();
        themes.push_back(system_theme.raw());
        themes.emplace_back("hicolor");
        for (auto const &[i, name] : {std::pair{1, scale}, {2, stretch}, {10, skew}}) {
            SCOPED_TRACE(i);
            auto *knot = tool->_seltrans->handleKnot(i);
            ASSERT_TRUE(knot);
            bool found = false;
            for (auto const &theme_name : themes) {
                for (auto const &path : paths) {
                    auto const filename = Glib::build_filename(path, theme_name, "cursors", name);
                    found |= g_file_test(filename.c_str(), G_FILE_TEST_EXISTS);
                }
            }
            EXPECT_TRUE(found) << name;
            if (knot->is_visible()) {
                auto expected = load_svg_cursor(widget, name);
                ASSERT_TRUE(expected);
                EXPECT_EQ(knot->_cursors[SP_KNOT_STATE_MOUSEOVER]->get_texture(), expected->get_texture());
                EXPECT_EQ(knot->_cursors[SP_KNOT_STATE_MOUSEOVER],
                          knot->_cursors[SP_KNOT_STATE_DRAGGING]);
            }
        }
    };
    check("y-down", "select-scale-nwse.svg", "select-stretch-vertical.svg", "select-skew-horizontal.svg");
    SPKnot released(desktop.get(), "Release cursor test", CANVAS_ITEM_CTRL_TYPE_ADJ_HANDLE);
    auto hover = load_svg_cursor(widget, "select-scale-nwse.svg");
    auto drag = load_svg_cursor(widget, "select-scale-nesw.svg");
    released.setCursor(SP_KNOT_STATE_MOUSEOVER, hover);
    released.setCursor(SP_KNOT_STATE_DRAGGING, drag);
    released.moveto({15, 15});
    released.show();
    released.ctrl->update(false);
    ASSERT_TRUE(released.ctrl->get_bounds());
    auto const release_pos = released.ctrl->get_bounds()->midpoint();
    ASSERT_TRUE(released.ctrl->contains(release_pos));
    EXPECT_TRUE(released.eventHandler(EnterEvent{}));
    released.startDragging(released.pos, {0, 0}, 0);
    ButtonReleaseEvent release;
    release.button = 1;
    release.pos = release_pos;
    EXPECT_TRUE(released.eventHandler(release));
    EXPECT_EQ(canvas->get_cursor(), hover);
    EXPECT_TRUE(released.eventHandler(LeaveEvent{}));
    desktop->waiting_cursor = true;
    canvas->set_cursor(drag);
    EXPECT_TRUE(released.eventHandler(EnterEvent{}));
    EXPECT_EQ(canvas->get_cursor(), drag);
    EXPECT_TRUE(released.eventHandler(LeaveEvent{}));
    desktop->waiting_cursor = false;
    desktop->getNamedView()->set_y_axis_down(false);
    document->ensureUpToDate();
    desktop->getSelection()->set(item("inside"));
    check("y-up", "select-scale-nwse.svg", "select-stretch-vertical.svg", "select-skew-horizontal.svg");
    desktop->getNamedView()->set_y_axis_down(true);
    document->ensureUpToDate();
    desktop->getSelection()->set(item("inside"));
    desktop->flip_relative_center_point({50, 50}, SPDesktop::FLIP_HORIZONTAL);
    check("horizontal flip", "select-scale-nesw.svg", "select-stretch-vertical.svg", "select-skew-horizontal.svg");
    auto *hovered = tool->_seltrans->handleKnot(2);
    ASSERT_TRUE(hovered);
    EXPECT_TRUE(hovered->eventHandler(EnterEvent{}));
    EXPECT_EQ(canvas->get_cursor(), hovered->_cursors[SP_KNOT_STATE_MOUSEOVER]);
    desktop->rotate_relative_center_point({50, 50}, Geom::rad_from_deg(90));
    EXPECT_EQ(canvas->get_cursor(), hovered->_cursors[SP_KNOT_STATE_MOUSEOVER]);
    EXPECT_TRUE(hovered->eventHandler(LeaveEvent{}));
    check("90 degree rotation", "select-scale-nesw.svg", "select-stretch-horizontal.svg", "select-skew-vertical.svg");

    GdkKeymapKey *keys = nullptr;
    int key_count = 0;
    ASSERT_TRUE(gdk_display_map_keyval(gdk_display_get_default(), GDK_KEY_Escape, &keys, &key_count));
    ASSERT_GT(key_count, 0);
    released.startDragging(released.pos, {0, 0}, 0);
    KeyPressEvent escape;
    escape.keyval = GDK_KEY_Escape;
    escape.keycode = keys[0].keycode;
    escape.group = keys[0].group;
    g_free(keys);
    escape.pos = release_pos;
    released.eventHandler(escape);
    EXPECT_EQ(canvas->get_cursor(), hover);
    released.moveto({30, 30});
    released.ctrl->update(false);
    ASSERT_TRUE(released.ctrl->get_bounds());
    escape.pos = released.ctrl->get_bounds()->midpoint();
    released.cancelled_signal.connect([](SPKnot *, unsigned) { return true; });
    released.startDragging(released.pos, {0, 0}, 0);
    released.moved = true;
    released.eventHandler(escape);
    EXPECT_NE(canvas->get_cursor(), hover);
}

TEST_F(SelectorInteractionTest, ObjectEnterUsesGrabCursorAndHandleOverridesIt)
{
    auto *canvas = desktop->getCanvas();
    auto &widget = dynamic_cast<Gtk::Widget &>(*canvas);
    auto const theme = Gtk::IconTheme::get_for_display(widget.get_display());
    bool file_exists = false;
    for (auto const &path : theme->get_search_path()) {
        for (auto const *name : {"Dash", "hicolor", "multicolor"}) {
            auto const file = Glib::build_filename(path, name, "cursors", "select-grab.svg");
            file_exists |= g_file_test(file.c_str(), G_FILE_TEST_IS_REGULAR);
        }
    }
    ASSERT_TRUE(file_exists) << "select-grab.svg is missing from the cursor theme paths";

    EXPECT_EQ(tool->cursor_filename(), "select.svg");
    EnterEvent enter;
    enter.pos = {15, 15};
    tool->item_handler(item("inside"), enter);
    EXPECT_EQ(tool->cursor_filename(), "select-grab.svg");
    auto grab_cursor = canvas->get_cursor();
    ASSERT_TRUE(grab_cursor);

    SPKnot handle(desktop.get(), "Object hover handle priority", CANVAS_ITEM_CTRL_TYPE_ADJ_HANDLE);
    auto handle_cursor = load_svg_cursor(widget, "select-scale-nwse.svg");
    ASSERT_TRUE(handle_cursor);
    handle.setCursor(SP_KNOT_STATE_MOUSEOVER, handle_cursor);
    handle.moveto({20, 20});
    handle.show();
    EXPECT_TRUE(handle.eventHandler(EnterEvent{}));
    EXPECT_EQ(canvas->get_cursor(), handle_cursor);
    EXPECT_NE(canvas->get_cursor(), grab_cursor);
    EXPECT_TRUE(handle.eventHandler(LeaveEvent{}));

    LeaveEvent leave;
    tool->item_handler(item("inside"), leave);
    EXPECT_EQ(tool->cursor_filename(), "select.svg");
    ASSERT_TRUE(canvas->get_cursor());
    EXPECT_NE(canvas->get_cursor(), grab_cursor);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_EQ(DocumentUndo::getUndoSensitive(document.get()), true);
}

TEST_F(SelectorInteractionTest, HoverOutlineShowsPlainClickTarget)
{
    ScopedCursorTolerance tolerance(0.0);
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    desktop->getSelection()->clear();
    auto *target = item("inside");
    ASSERT_TRUE(target);
    auto const bounds = target->desktopVisualBounds();
    ASSERT_TRUE(bounds);
    ASSERT_FALSE(desktop->getSelection()->includes(target));

    MotionEvent motion;
    motion.pos = bounds->midpoint();
    motion.modifiers = 0;
    tool->root_handler(motion);

    auto const outline = tool->hover_outline_rect_for_testing();
    ASSERT_TRUE(outline);
    EXPECT_EQ(*outline, *bounds);
}

TEST_F(SelectorInteractionTest, HoverTintUsesOnlyPaintedVectorPixelsAndClearsExactly)
{
    ScopedCursorTolerance tolerance(0.0);
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    desktop->getSelection()->clear();
    auto const baseline = render_drawing(*desktop);
    auto const inside = desktop->doc2dt({15, 55});
    auto const outside_shape_inside_bounds = desktop->doc2dt({28, 68});
    auto const original_inside = baseline.at(inside);
    auto const original_outside = baseline.at(outside_shape_inside_bounds);
    ASSERT_EQ(original_inside[3], 255u);

    MotionEvent motion;
    motion.pos = inside;
    tool->root_handler(motion);
    ASSERT_TRUE(tool->hover_outline_rect_for_testing());
    auto const tinted = render_drawing(*desktop);
    auto const blue_black = tinted.at(inside);
    EXPECT_NEAR(blue_black[0], 12, 2);
    EXPECT_NEAR(blue_black[1], 38, 2);
    EXPECT_NEAR(blue_black[2], 77, 2);
    EXPECT_EQ(blue_black[3], original_inside[3]);
    EXPECT_EQ(tinted.at(outside_shape_inside_bounds), original_outside)
        << "transparent space inside the triangle bounds stays unchanged";

    motion.pos = desktop->doc2dt({95, 95});
    tool->root_handler(motion);
    EXPECT_FALSE(tool->hover_outline_rect_for_testing());
    auto const restored = render_drawing(*desktop);
    EXPECT_EQ(restored.pixels, baseline.pixels)
        << "clearing hover restores every drawing pixel, including cached ancestors";
}

TEST_F(SelectorInteractionTest, HoverTintPreservesTransparentBitmapCorners)
{
    ScopedCursorTolerance tolerance(0.0);
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    desktop->getSelection()->clear();
    auto const baseline = render_drawing(*desktop);
    auto const transparent_corner = desktop->doc2dt({70.5, 10.5});
    auto const opaque_pixel = desktop->doc2dt({74.5, 14.5});
    ASSERT_EQ(baseline.at(opaque_pixel)[3], 255u);

    MotionEvent motion;
    motion.pos = opaque_pixel;
    tool->root_handler(motion);
    ASSERT_TRUE(tool->hover_outline_rect_for_testing());
    auto const tinted = render_drawing(*desktop);
    EXPECT_EQ(tinted.at(transparent_corner), baseline.at(transparent_corner));
    auto const red = tinted.at(opaque_pixel);
    EXPECT_NEAR(red[0], 190, 2);
    EXPECT_NEAR(red[1], 38, 2);
    EXPECT_NEAR(red[2], 77, 2);
    EXPECT_EQ(red[3], 255u);
}

TEST_F(SelectorInteractionTest, HoverTintMovesToOnlyTheSecondTarget)
{
    ScopedCursorTolerance tolerance(0.0);
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    desktop->getSelection()->clear();
    auto const baseline = render_drawing(*desktop);
    auto const triangle = desktop->doc2dt({15, 55});
    auto const ellipse = desktop->doc2dt({55, 60});

    MotionEvent motion;
    motion.pos = triangle;
    tool->root_handler(motion);
    auto const first = render_drawing(*desktop);
    EXPECT_NE(first.at(triangle), baseline.at(triangle));

    motion.pos = ellipse;
    tool->root_handler(motion);
    auto const second = render_drawing(*desktop);
    EXPECT_EQ(second.at(triangle), baseline.at(triangle));
    auto const red = second.at(ellipse);
    EXPECT_NEAR(red[0], 190, 2);
    EXPECT_NEAR(red[1], 38, 2);
    EXPECT_NEAR(red[2], 77, 2);
}

TEST_F(SelectorInteractionTest, HoverTintSkipsSelectedTargetsAndModifiers)
{
    ScopedCursorTolerance tolerance(0.0);
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    auto const probe = desktop->doc2dt({15, 55});
    auto const baseline = render_drawing(*desktop);

    desktop->getSelection()->set(item("triangle"));
    MotionEvent selected;
    selected.pos = probe;
    tool->root_handler(selected);
    EXPECT_FALSE(tool->hover_outline_rect_for_testing());
    EXPECT_EQ(render_drawing(*desktop).pixels, baseline.pixels);

    desktop->getSelection()->clear();
    MotionEvent modified;
    modified.pos = probe;
    modified.modifiers = GDK_SHIFT_MASK;
    tool->root_handler(modified);
    EXPECT_FALSE(tool->hover_outline_rect_for_testing());
    EXPECT_EQ(render_drawing(*desktop).pixels, baseline.pixels);
}

TEST_F(SelectorInteractionTest, HoverTintDoesNotChangeDocumentPngExport)
{
    ScopedCursorTolerance tolerance(0.0);
    desktop->getSelection()->clear();
    auto *tmp = g_dir_make_tmp("inkscape-hover-export-XXXXXX", nullptr);
    ASSERT_NE(tmp, nullptr);
    auto const before_path = Glib::build_filename(tmp, "before.png");
    auto const hover_path = Glib::build_filename(tmp, "hover.png");
    auto cleanup = [&] {
        g_remove(before_path.c_str());
        g_remove(hover_path.c_str());
        g_rmdir(tmp);
        g_free(tmp);
    };

    ASSERT_EQ(sp_export_png_file(document.get(), before_path.c_str(), 0, 0, 100, 100,
                                 100, 100, 96, 96, Colors::Color(0xffffffff), nullptr, nullptr), EXPORT_OK);
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    MotionEvent motion;
    motion.pos = desktop->doc2dt({15, 55});
    tool->root_handler(motion);
    ASSERT_TRUE(tool->hover_outline_rect_for_testing());
    ASSERT_EQ(sp_export_png_file(document.get(), hover_path.c_str(), 0, 0, 100, 100,
                                 100, 100, 96, 96, Colors::Color(0xffffffff), nullptr, nullptr), EXPORT_OK);

    auto read_bytes = [](std::string const &path) {
        std::ifstream input(path, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(input), {});
    };
    EXPECT_EQ(read_bytes(hover_path), read_bytes(before_path));
    cleanup();
}

TEST(SelectorHoverTintShots, CaptureOwnerVectorAndBitmapStates)
{
    auto const shot_dir = std::getenv("HOVER_TINT_SHOT_DIR");
    if (!shot_dir || !*shot_dir) GTEST_SKIP() << "set HOVER_TINT_SHOT_DIR to capture owner screenshots";
    ASSERT_TRUE(initialize_gui());
    if (!Application::exists()) Application::create(false);
    auto document = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="700" viewBox="0 0 1000 700">
  <path id="black" fill="#101010" d="M120 245 L430 190 L405 500 L255 555 Z"/>
  <ellipse id="color" cx="515" cy="365" rx="165" ry="145" fill="#e34b36"/>
  <image id="bitmap" x="625" y="270" width="250" height="250" preserveAspectRatio="none"
         href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAQElEQVR42u3WMQ0AIBADwJeCfwVIwgVIYGg+DFyTzr2xVZesOXbSSgMAANAOSAdiIAAAAAAAAMBzgEcEAPA94ABBMD152isIFgAAAABJRU5ErkJggg=="/>
</svg>)svg");
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto desktop = std::make_unique<SPDesktop>(document->getNamedView());
    ASSERT_TRUE(desktop);
    Application::instance().add_desktop(desktop.get());
    Gtk::Window window;
    window.set_default_size(1000, 700);
    window.set_child(*desktop->getCanvas());
    window.present();
    ASSERT_TRUE(wait_for_paint(window));
    ASSERT_GT(desktop->getCanvas()->get_width(), 900);
    ASSERT_GT(desktop->getCanvas()->get_height(), 600);
    desktop->zoom_absolute({500, 350}, 1.0);
    desktop->set_display_area({500, 350}, {desktop->getCanvas()->get_width() / 2.0,
                                           desktop->getCanvas()->get_height() / 2.0}, false);

    auto *tool = dynamic_cast<SelectTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    auto const mode = std::getenv("HOVER_TINT_SHOT_MODE");
    ASSERT_TRUE(mode && (std::string(mode) == "vector" || std::string(mode) == "bitmap" || std::string(mode) == "none"));
    bool const bitmap_mode = std::string(mode) == "bitmap";
    if (std::string(mode) == "none") {
        desktop->getCanvasDrawing()->update(false);
        drain_main_context();
        ASSERT_TRUE(save_canvas(window, *desktop->getCanvas(), Glib::build_filename(shot_dir, "hover-none.png")));
        window.unset_child();
        window.close();
        Application::instance().remove_desktop(desktop.get());
        drain_main_context();
        return;
    }
    MotionEvent motion;
    motion.pos = desktop->doc2dt(bitmap_mode ? Geom::Point{750, 395} : Geom::Point{515, 365});
    tool->root_handler(motion);
    auto *target = cast<SPItem>(document->getObjectById(bitmap_mode ? "bitmap" : "color"));
    ASSERT_TRUE(target);
    auto const target_bounds = target->desktopVisualBounds();
    ASSERT_TRUE(target_bounds);
    ASSERT_EQ(tool->hover_outline_rect_for_testing(), target_bounds);
    desktop->getCanvasDrawing()->update(false);
    drain_main_context();
    auto const output_name = bitmap_mode ? "hover-bitmap.png" : "hover-vector.png";
    ASSERT_TRUE(save_canvas(window, *desktop->getCanvas(), Glib::build_filename(shot_dir, output_name)));

    LeaveEvent leave;
    tool->root_handler(leave);
    window.unset_child();
    window.close();
    Application::instance().remove_desktop(desktop.get());
    drain_main_context();
}

TEST(SelectorHoverTintPerformance, OneHoverAndRedrawForLargeTargets)
{
    if (!std::getenv("HOVER_TINT_BENCHMARK")) GTEST_SKIP() << "set HOVER_TINT_BENCHMARK=1 to run performance measurements";
    ASSERT_TRUE(initialize_gui());
    if (!Application::exists()) Application::create(false);

    constexpr unsigned width = 3311;
    constexpr unsigned height = 1919;
    std::ostringstream paths;
    paths << "<svg xmlns='http://www.w3.org/2000/svg' width='" << width << "' height='" << height << "'>";
    paths << "<g id='paths' fill='#151515'>";
    for (unsigned i = 0; i < 2000; ++i) {
        auto const x = (i % 50) * 65;
        auto const y = (i / 50) * 48;
        paths << "<path d='M" << x << ' ' << y << "h24v24h-24z'/>";
    }
    paths << "</g></svg>";

    std::vector<unsigned char> png;
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    auto *cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.12, 0.36, 0.82, 0.8);
    cairo_paint(cr);
    cairo_destroy(cr);
    auto const png_status = cairo_surface_write_to_png_stream(surface, +[](void *closure, unsigned char const *data, unsigned int length) {
        auto &bytes = *static_cast<std::vector<unsigned char> *>(closure);
        bytes.insert(bytes.end(), data, data + length);
        return CAIRO_STATUS_SUCCESS;
    }, &png);
    cairo_surface_destroy(surface);
    ASSERT_EQ(png_status, CAIRO_STATUS_SUCCESS);
    auto *encoded_png = g_base64_encode(png.data(), png.size());
    ASSERT_NE(encoded_png, nullptr);
    auto const image_svg = "<svg xmlns='http://www.w3.org/2000/svg' width='3311' height='1919'>"
                           "<image id='image' width='3311' height='1919' preserveAspectRatio='none' href='data:image/png;base64," +
                           std::string(encoded_png) + "'/></svg>";
    g_free(encoded_png);

    auto measure = [&](std::string const &svg, char const *id) {
        auto document = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(), svg.size()));
        EXPECT_TRUE(document);
        if (!document) return 0.0;
        document->ensureUpToDate();
        auto desktop = std::make_unique<SPDesktop>(document->getNamedView());
        desktop->getCanvas()->size_allocate(Gtk::Allocation(0, 0, 900, 700), -1);
        auto *target = cast<SPItem>(document->getObjectById(id));
        EXPECT_TRUE(target);
        if (!target) return 0.0;
        auto *drawing_item = target->get_arenaitem(desktop->dkey);
        EXPECT_TRUE(drawing_item);
        if (!drawing_item) return 0.0;
        auto *drawing = desktop->getCanvasDrawing()->get_drawing();
        drawing->update(Geom::IntRect::infinite(), desktop->doc2dt(), DrawingItem::STATE_ALL);
        (void)render_drawing(*desktop); // Warm the untinted surface and candidate caches.
        auto const start = std::chrono::steady_clock::now();
        drawing_item->setHoverTint(0x277fff4d);
        (void)render_drawing(*desktop);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };

    auto const paths_ms = measure(paths.str(), "paths");
    auto const image_ms = measure(image_svg, "image");
    ASSERT_GT(paths_ms, 0.0);
    ASSERT_GT(image_ms, 0.0);
    std::cout << "HOVER_TINT_PERF 2000-path-group=" << paths_ms
              << " ms 3311x1919-RGBA-image=" << image_ms << " ms\n";
}

// Owner crash report 2026-09-28 (build 21, macOS): moving the pointer after
// an edit crashed in SPDesktop::find_items_at_point. This documents the
// existing guarantee that the desktop's pick cache (a flat list of raw item
// pointers) forgets a released item at once (object-bound signal), before any
// idle update, so a hover right after a deletion picks the remaining item.
// The crash itself is not reproduced (BUG-009).
TEST_F(SelectorInteractionTest, PickCacheForgetsAReleasedItemBeforeIdle)
{
    ScopedCursorTolerance tolerance(0.0);
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    desktop->getSelection()->clear();
    auto *inside = item("inside");
    auto *crossing = item("crossing");
    ASSERT_TRUE(inside && crossing);
    auto const inside_bounds = inside->desktopVisualBounds();
    auto const crossing_bounds = crossing->desktopVisualBounds();
    ASSERT_TRUE(inside_bounds && crossing_bounds);
    auto const overlap = *inside_bounds & *crossing_bounds;
    ASSERT_TRUE(overlap && !overlap->hasZeroArea());

    // The topmost item wins and fills the pick cache.
    MotionEvent motion;
    motion.pos = overlap->midpoint();
    motion.modifiers = 0;
    tool->root_handler(motion);
    auto outline = tool->hover_outline_rect_for_testing();
    ASSERT_TRUE(outline);
    ASSERT_EQ(*outline, *crossing_bounds);

    // Release it and hover again with no idle update in between.
    crossing->deleteObject();
    ASSERT_FALSE(item("crossing"));
    desktop->getCanvasDrawing()->update(false);
    motion.pos = overlap->midpoint() + Geom::Point(0.01, 0.01); // a fresh motion
    tool->root_handler(motion);
    outline = tool->hover_outline_rect_for_testing();
    ASSERT_TRUE(outline) << "the remaining item under the pointer is picked";
    EXPECT_EQ(*outline, *inside_bounds);
}

TEST_F(SelectorInteractionTest, HoverOutlineMatchesGroupClickTarget)
{
    ScopedCursorTolerance tolerance(0.0);
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    auto *selection = desktop->getSelection();
    selection->clear();
    // Group rect B with a sibling so the group's bounds differ from B's own bounds.
    ASSERT_TRUE(item("crossing"));
    ASSERT_TRUE(item("bitmap"));
    selection->add(item("crossing"));
    selection->add(item("bitmap"));
    selection->group();
    document->ensureUpToDate();
    desktop->clearNodeCache();
    desktop->getCanvasDrawing()->update(false);

    auto *member = item("crossing");
    ASSERT_TRUE(member);
    auto *group = cast<SPItem>(member->parent);
    ASSERT_TRUE(group);
    ASSERT_NE(group, member);
    auto const group_bounds = group->desktopVisualBounds();
    auto const member_bounds = member->desktopVisualBounds();
    ASSERT_TRUE(group_bounds);
    ASSERT_TRUE(member_bounds);
    ASSERT_NE(*group_bounds, *member_bounds);

    selection->clear();
    desktop->clearNodeCache();

    MotionEvent motion;
    motion.pos = member_bounds->midpoint();
    motion.modifiers = 0;
    tool->root_handler(motion);

    auto const outline = tool->hover_outline_rect_for_testing();
    ASSERT_TRUE(outline);
    EXPECT_EQ(*outline, *group_bounds);
}

TEST_F(SelectorInteractionTest, HoverOutlineHiddenForSelectedAndModifiers)
{
    ScopedCursorTolerance tolerance(0.0);
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    auto *target = item("inside");
    ASSERT_TRUE(target);
    auto const bounds = target->desktopVisualBounds();
    ASSERT_TRUE(bounds);
    ASSERT_TRUE(desktop->getSelection()->includes(target));
    auto const probe = bounds->midpoint();

    MotionEvent over_selected;
    over_selected.pos = probe;
    over_selected.modifiers = 0;
    tool->root_handler(over_selected);
    EXPECT_FALSE(tool->hover_outline_rect_for_testing());

    desktop->getSelection()->clear();
    MotionEvent with_shift;
    with_shift.pos = probe;
    with_shift.modifiers = GDK_SHIFT_MASK;
    tool->root_handler(with_shift);
    EXPECT_FALSE(tool->hover_outline_rect_for_testing());
}

TEST_F(SelectorInteractionTest, HoverOutlineHiddenOnPressAndLeave)
{
    ScopedCursorTolerance tolerance(0.0);
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    desktop->getSelection()->clear();
    auto *target = item("inside");
    ASSERT_TRUE(target);
    auto const bounds = target->desktopVisualBounds();
    ASSERT_TRUE(bounds);
    auto const probe = bounds->midpoint();

    MotionEvent motion;
    motion.pos = probe;
    motion.modifiers = 0;
    tool->root_handler(motion);
    ASSERT_TRUE(tool->hover_outline_rect_for_testing());

    ButtonPressEvent press;
    press.button = 1;
    press.pos = press.orig_pos = probe;
    tool->root_handler(press);
    EXPECT_FALSE(tool->hover_outline_rect_for_testing());

    // Release the pointer grab so a later motion can preview again. The click may select
    // the target, so clear the selection before re-hovering.
    ButtonReleaseEvent release;
    release.button = 1;
    release.pos = release.orig_pos = probe;
    tool->root_handler(release);
    desktop->getSelection()->clear();

    tool->root_handler(motion);
    ASSERT_TRUE(tool->hover_outline_rect_for_testing());

    LeaveEvent leave;
    tool->root_handler(leave);
    EXPECT_FALSE(tool->hover_outline_rect_for_testing());
}

TEST_F(SelectorInteractionTest, PageResizeKnotDeliversCanvasCursor)
{
    desktop->setTool("/tools/pages");
    auto *pages = dynamic_cast<PagesTool *>(desktop->getTool());
    ASSERT_TRUE(pages);
    auto *knot = pages->resizeKnot(0);
    ASSERT_TRUE(knot);
    auto *canvas = desktop->getCanvas();
    ASSERT_TRUE(knot->_cursors[SP_KNOT_STATE_MOUSEOVER]);
    EXPECT_TRUE(knot->eventHandler(EnterEvent{}));
    EXPECT_EQ(canvas->get_cursor(), knot->_cursors[SP_KNOT_STATE_MOUSEOVER]);
    EXPECT_TRUE(knot->eventHandler(LeaveEvent{}));
    ASSERT_TRUE(canvas->get_cursor());
    EXPECT_NE(canvas->get_cursor(), knot->_cursors[SP_KNOT_STATE_MOUSEOVER]);
}

TEST_F(SelectorInteractionTest, DrawingUpdateRepickMatchesStationaryPointerPick)
{
    auto *drawing_item = desktop->getCanvasDrawing();
    ASSERT_TRUE(drawing_item);
    drawing_item->setCursorTolerance(1.0);
    std::vector<std::pair<EventType, DrawingItem *>> crossings;
    auto connection = drawing_item->connect_drawing_event(
        [&](CanvasEvent const &event, DrawingItem *target) {
            if (event.type() == EventType::ENTER || event.type() == EventType::LEAVE) {
                crossings.emplace_back(event.type(), target);
            }
            return false;
        });

    EnterEvent enter;
    enter.pos = {80, 80};
    drawing_item->handle_event(enter);
    crossings.clear(); // Initial widget Enter has no drawing target.
    ASSERT_EQ(drawing_item->get_active(), nullptr);

    drawing_item->update(false);
    if (!crossings.empty()) {
        ADD_FAILURE() << "blank redraw emitted a drawing crossing";
        drawing_item->set_active(nullptr);
        connection.disconnect();
        return;
    }
    EXPECT_EQ(drawing_item->get_active(), nullptr);

    EXPECT_FALSE(drawing_item->contains(enter.pos));
    EXPECT_TRUE(crossings.empty());

    auto const selected = desktop->getSelection()->singleItem();
    set_rect("crossing", "75", "75", "15", "15");
    drawing_item->update(false);
    ASSERT_EQ(crossings.size(), 1u);
    EXPECT_EQ(crossings[0].first, EventType::ENTER);
    EXPECT_EQ(crossings[0].second, item("crossing")->get_arenaitem(desktop->dkey));
    ASSERT_NE(drawing_item->get_active(), nullptr);
    crossings.clear();
    EXPECT_TRUE(drawing_item->contains(enter.pos));
    EXPECT_TRUE(crossings.empty());

    set_rect("crossing", "18", "18", "15", "15");
    drawing_item->update(false);
    ASSERT_EQ(crossings.size(), 1u);
    EXPECT_EQ(crossings[0].first, EventType::LEAVE);
    EXPECT_EQ(drawing_item->get_active(), nullptr);
    bool blank_press_reached_root = false;
    auto press_connection = drawing_item->connect_drawing_event(
        [&](CanvasEvent const &event, DrawingItem *target) {
            if (event.type() == EventType::BUTTON_PRESS) blank_press_reached_root = target == nullptr;
            return false;
        });
    ButtonPressEvent press;
    press.button = 1;
    press.pos = enter.pos;
    drawing_item->handle_event(press);
    EXPECT_TRUE(blank_press_reached_root);
    EXPECT_TRUE(Rubberband::get(desktop.get())->isStarted());
    press_connection.disconnect();
    ButtonReleaseEvent release;
    release.button = 1;
    release.pos = enter.pos;
    drawing_item->handle_event(release);
    EXPECT_TRUE(DocumentUndo::getUndoSensitive(document.get()));
    EXPECT_FALSE(document->isModifiedSinceSave());
    connection.disconnect();
}

TEST(SelectorResizeDimensions, FormatsEverySupportedDisplayUnitWithTrailingZeros)
{
    auto &units = Util::UnitTable::get();
    struct Example {
        char const *unit;
        double value;
        char const *expected;
    };
    Example const examples[] = {
        {"px", 12.5, "12.50 px"},
        {"mm", 3.0, "3.00 mm"},
        {"cm", 4.5, "4.50 cm"},
        {"m", 1.25, "1.250 m"},
        {"in", 4.5, "4.50 in"},
        {"ft", 1.25, "1.250 ft"},
        {"pt", 12.0, "12.00 pt"},
        {"pc", 2.5, "2.500 pc"},
    };

    for (auto const &example : examples) {
        auto const unit = units.getUnit(example.unit);
        ASSERT_TRUE(unit) << example.unit;
        double const px = Util::Quantity::convert(example.value, unit, "px");
        EXPECT_EQ(format_selection_dimension(px, *unit, std::locale::classic()).raw(),
                  example.expected)
            << example.unit;
    }
}

TEST(SelectorResizeDimensions, HonorsLocaleAndSuppressesNegativeZero)
{
    auto const unit = Util::UnitTable::get().getUnit("mm");
    ASSERT_TRUE(unit);
    std::locale const comma{std::locale::classic(), new CommaDecimal};

    double const three_mm = Util::Quantity::convert(3.0, unit, "px");
    EXPECT_EQ(format_selection_dimension(three_mm, *unit, comma).raw(), "3,00 mm");
    EXPECT_EQ(format_selection_dimension(-1e-12, *unit, std::locale::classic()).raw(),
              "0.00 mm");

    double const large = Util::Quantity::convert(123456789.25, unit, "px");
    EXPECT_EQ(format_selection_dimension(large, *unit, std::locale::classic()).raw(),
              "123456789.25 mm");
}

TEST(SelectorResizeDimensions, TransformedBoundsPreserveAbsoluteSizeAcrossFlips)
{
    auto const bounds = Geom::Rect::from_xywh(10, 20, 30, 40);
    auto const affine = Geom::Translate(-10, -20) * Geom::Scale(-2.0, 0.5) *
                        Geom::Translate(100, 75);
    auto const transformed = transformed_selection_bounds(bounds, affine);

    EXPECT_NEAR(transformed.width(), 60.0, 1e-9);
    EXPECT_NEAR(transformed.height(), 20.0, 1e-9);
}

TEST(SelectorDimensionalDetents, CadenceMatchesEverySupportedDisplayUnit)
{
    auto &units = Util::UnitTable::get();
    struct Expected {
        char const *unit;
        double regular;
        double strong;
    };
    Expected const expected[] = {
        {"in", 0.5, 1.0},
        {"ft", 1.0 / 24.0, 1.0},
        {"mm", 1.0, 10.0},
        {"cm", 0.1, 1.0},
        {"m", 0.001, 0.01},
        {"px", 1.0, 10.0},
        {"pt", 1.0, 12.0},
        {"pc", 1.0 / 12.0, 1.0},
    };

    for (auto const &entry : expected) {
        auto const unit = units.getUnit(entry.unit);
        ASSERT_TRUE(unit) << entry.unit;
        auto const cadence = dimensional_cadence(*unit);
        ASSERT_TRUE(cadence) << entry.unit;
        EXPECT_DOUBLE_EQ(cadence->regular, entry.regular) << entry.unit;
        EXPECT_DOUBLE_EQ(cadence->strong, entry.strong) << entry.unit;
    }

    auto const percent = units.getUnit("%");
    ASSERT_TRUE(percent);
    EXPECT_FALSE(dimensional_cadence(*percent));
}

TEST(SelectorDimensionalDetents, NearestCandidatesRoundTripExactlyAtEveryCadence)
{
    auto &units = Util::UnitTable::get();
    char const *unit_names[] = {"in", "ft", "mm", "cm", "m", "px", "pt", "pc"};

    for (auto const *name : unit_names) {
        auto const unit = units.getUnit(name);
        ASSERT_TRUE(unit) << name;
        auto const cadence = dimensional_cadence(*unit);
        ASSERT_TRUE(cadence) << name;
        double const boundary = cadence->regular * 24.0;
        double const delta = cadence->regular * 1e-7;

        auto const below_px = Util::Quantity::convert(boundary - delta, unit, "px");
        auto const below = nearest_dimensional_candidates(below_px, *unit);
        ASSERT_EQ(below.size, 2u) << name;
        EXPECT_NEAR(below.values[0].display_value, boundary - cadence->regular, 1e-12)
            << name;
        EXPECT_NEAR(below.values[1].display_value, boundary, 1e-12) << name;
        EXPECT_NEAR(Util::Quantity::convert(below.values[1].document_value, "px", unit),
                    boundary, 1e-12) << name;

        auto const exact_px = Util::Quantity::convert(boundary, unit, "px");
        auto const exact = nearest_dimensional_candidates(exact_px, *unit);
        ASSERT_EQ(exact.size, 1u) << name;
        EXPECT_NEAR(exact.values[0].document_value, exact_px,
                    std::max(1.0, exact_px) * 1e-12) << name;

        auto const above_px = Util::Quantity::convert(boundary + delta, unit, "px");
        auto const above = nearest_dimensional_candidates(above_px, *unit);
        ASSERT_EQ(above.size, 2u) << name;
        EXPECT_NEAR(above.values[0].display_value, boundary, 1e-12) << name;
        EXPECT_NEAR(above.values[1].display_value, boundary + cadence->regular, 1e-12)
            << name;
    }
}

TEST(SelectorDimensionalDetents, EquivalentUnitFamiliesProduceIdenticalPhysicalCadence)
{
    auto &units = Util::UnitTable::get();
    auto interval_px = [&] (char const *name) {
        auto const unit = units.getUnit(name);
        EXPECT_TRUE(unit) << name;
        auto const cadence = dimensional_cadence(*unit);
        EXPECT_TRUE(cadence) << name;
        return Util::Quantity::convert(cadence->regular, unit, "px");
    };

    EXPECT_NEAR(interval_px("in"), interval_px("ft"), 1e-12);
    EXPECT_NEAR(interval_px("mm"), interval_px("cm"), 1e-12);
    EXPECT_NEAR(interval_px("mm"), interval_px("m"), 1e-12);
    EXPECT_NEAR(interval_px("pt"), interval_px("pc"), 1e-12);
}

TEST(SelectorDimensionalDetents, CaptureChoosesNearestEligibleCandidateAndMarksStrong)
{
    auto const unit = Util::UnitTable::get().getUnit("px");
    ASSERT_TRUE(unit);
    DimensionalDetentLatch latch;

    auto result = evaluate_dimensional_detent(9.2, *unit, 1.0, true, false, latch);
    ASSERT_TRUE(result.engaged);
    EXPECT_FALSE(result.strong); // 9 px is closer than the stronger 10 px candidate.
    EXPECT_DOUBLE_EQ(result.dimension, 9.0);

    latch.reset();
    result = evaluate_dimensional_detent(9.7, *unit, 1.0, true, false, latch);
    ASSERT_TRUE(result.engaged);
    EXPECT_TRUE(result.strong);
    EXPECT_DOUBLE_EQ(result.dimension, 10.0);

    latch.reset();
    result = evaluate_dimensional_detent(4.1, *unit, 1.0, true, false, latch);
    EXPECT_TRUE(result.engaged);
    EXPECT_FALSE(result.strong);
    EXPECT_DOUBLE_EQ(result.dimension, 4.0);
}

TEST(SelectorDimensionalDetents, LatchUsesReleaseHysteresisAcrossNoiseAndReversal)
{
    auto const unit = Util::UnitTable::get().getUnit("in");
    ASSERT_TRUE(unit);
    auto const one_inch = Util::Quantity::convert(1.0, unit, "px");
    DimensionalDetentLatch latch;

    auto result = evaluate_dimensional_detent(one_inch + 5.5, *unit, 1.0,
                                              true, false, latch);
    ASSERT_TRUE(result.engaged);
    ASSERT_TRUE(result.strong);
    EXPECT_NEAR(result.dimension, one_inch, 1e-12);

    for (double noise : {9.9, -9.9, 0.25, -0.25, 7.5}) {
        result = evaluate_dimensional_detent(one_inch + noise, *unit, 1.0,
                                             true, false, latch);
        EXPECT_TRUE(result.engaged) << noise;
        EXPECT_NEAR(result.dimension, one_inch, 1e-12) << noise;
    }

    result = evaluate_dimensional_detent(one_inch + 10.01, *unit, 1.0,
                                         true, false, latch);
    EXPECT_FALSE(result.engaged);
    EXPECT_FALSE(latch.engaged());

    // A high-velocity jump releases the old identity and may capture only the nearest new pair.
    auto const two_inches = Util::Quantity::convert(2.0, unit, "px");
    result = evaluate_dimensional_detent(two_inches + 1.0, *unit, 1.0,
                                         true, false, latch);
    ASSERT_TRUE(result.engaged);
    EXPECT_NEAR(result.dimension, two_inches, 1e-12);
}

TEST(SelectorDimensionalDetents, ScreenSpaceThresholdsAreZoomInvariant)
{
    auto const unit = Util::UnitTable::get().getUnit("px");
    ASSERT_TRUE(unit);

    for (double zoom : {0.01, 0.25, 1.0, 4.0, 256.0}) {
        DimensionalDetentLatch latch;
        double const target = 100.0;
        latch.candidate = DimensionalCandidate{100.0, target, true};

        auto result = evaluate_dimensional_detent(target + 9.9 / zoom, *unit, zoom,
                                             true, false, latch);
        EXPECT_TRUE(result.engaged) << zoom;
        EXPECT_DOUBLE_EQ(result.dimension, target) << zoom;
        EXPECT_NEAR(result.screen_distance, 9.9, 1e-9) << zoom;
        result = evaluate_dimensional_detent(target + 10.1 / zoom, *unit, zoom,
                                             true, false, latch);
        if (result.engaged) {
            // At low zoom a different nearest cadence may be captured immediately, but the old
            // candidate identity must still have released at the same ten-screen-pixel radius.
            EXPECT_NEAR(result.screen_distance, 0.0, 6.0) << zoom;
            EXPECT_NE(result.dimension, target) << zoom;
        } else {
            EXPECT_FALSE(latch.engaged()) << zoom;
        }
    }
}

TEST(SelectorDimensionalDetents, InvalidPriorityAndDisabledInputsClearTheLatch)
{
    auto const unit = Util::UnitTable::get().getUnit("px");
    ASSERT_TRUE(unit);
    DimensionalDetentLatch latch;

    ASSERT_TRUE(evaluate_dimensional_detent(10.5, *unit, 1.0, true, false, latch).engaged);
    EXPECT_FALSE(evaluate_dimensional_detent(10.5, *unit, 1.0, true, true, latch).engaged);
    EXPECT_FALSE(latch.engaged());

    ASSERT_TRUE(evaluate_dimensional_detent(10.5, *unit, 1.0, true, false, latch).engaged);
    EXPECT_FALSE(evaluate_dimensional_detent(10.5, *unit, 1.0, false, false, latch).engaged);
    EXPECT_FALSE(latch.engaged());

    for (double invalid : {0.0, -1.0, DIMENSIONAL_DETENT_SINGULAR_EPSILON,
                           std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_EQ(nearest_dimensional_candidates(invalid, *unit).size, 0u);
    }
}

TEST(SelectorDimensionalDetents, TenThousandMotionEventsRemainBounded)
{
    auto const unit = Util::UnitTable::get().getUnit("mm");
    ASSERT_TRUE(unit);
    DimensionalDetentLatch latch;
    double checksum = 0.0;

    for (unsigned i = 0; i < 10000; ++i) {
        auto const dimension = Util::Quantity::convert(1.0 + i * 0.013, unit, "px");
        auto const result = evaluate_dimensional_detent(dimension, *unit, 2.0,
                                                        true, false, latch);
        checksum += result.dimension;
    }
    EXPECT_TRUE(std::isfinite(checksum));
}

TEST(SelectorMarqueeClassifier, DirectionDeadZoneAndReversalAreStable)
{
    EXPECT_EQ(classify_marquee(4.0, MarqueeBehavior::Undetermined), MarqueeBehavior::Enclose);
    EXPECT_EQ(classify_marquee(-4.0, MarqueeBehavior::Undetermined), MarqueeBehavior::Crossing);
    EXPECT_EQ(classify_marquee(2.9, MarqueeBehavior::Undetermined), MarqueeBehavior::Undetermined);
    EXPECT_EQ(classify_marquee(-2.9, MarqueeBehavior::Enclose), MarqueeBehavior::Enclose);
    EXPECT_EQ(classify_marquee(-3.0, MarqueeBehavior::Enclose), MarqueeBehavior::Crossing);
    EXPECT_EQ(classify_marquee(3.0, MarqueeBehavior::Crossing), MarqueeBehavior::Enclose);
    EXPECT_EQ(classify_marquee(-0.01, MarqueeBehavior::Undetermined, -1.0),
              MarqueeBehavior::Crossing);
}

TEST(SelectorMarqueeClassifier, RightToLeftUsesPartialOverlapQuery)
{
    auto const left_to_right = classify_marquee(80.0, MarqueeBehavior::Undetermined);
    auto const right_to_left = classify_marquee(-80.0, MarqueeBehavior::Undetermined);

    EXPECT_EQ(left_to_right, MarqueeBehavior::Enclose);
    EXPECT_FALSE(marquee_selects_partial_overlap(left_to_right));
    EXPECT_EQ(right_to_left, MarqueeBehavior::Crossing);
    EXPECT_TRUE(marquee_selects_partial_overlap(right_to_left));
}

TEST_F(SelectorInteractionTest, EnclosingAndCrossingQueriesUseExistingGeometryPredicates)
{
    auto const box = Geom::Rect(Geom::Point(5, 5), Geom::Point(25, 25));
    auto enclosed = document->getItemsInBox(desktop->dkey, box);
    auto crossing = document->getItemsPartiallyInBox(desktop->dkey, box);

    auto contains = [] (auto const &items, SPItem *item) {
        return std::find(items.begin(), items.end(), item) != items.end();
    };
    EXPECT_TRUE(contains(enclosed, item("inside")));
    EXPECT_FALSE(contains(enclosed, item("crossing")));
    EXPECT_TRUE(contains(crossing, item("inside")));
    EXPECT_TRUE(contains(crossing, item("crossing")));
}

TEST_F(SelectorInteractionTest, RollbackTokenRestoresXmlAndKeepsDocumentClean)
{
    auto const before = sp_repr_save_buf(document->getReprDoc());
    auto interaction = DocumentUndo::beginRollbackableInteraction(document.get());
    ASSERT_TRUE(interaction);
    item("inside")->getRepr()->setAttribute("x", "42");
    interaction->rollback();
    document->ensureUpToDate();

    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before.raw());
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, RollbackTokenCommitCreatesExactlyOneUndoStep)
{
    auto interaction = DocumentUndo::beginRollbackableInteraction(document.get());
    ASSERT_TRUE(interaction);
    item("inside")->getRepr()->setAttribute("x", "42");
    interaction->commit(Util::Internal::ContextString{"Transactional edit"}, "tool-pointer");
    document->ensureUpToDate();

    EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "42");
    EXPECT_TRUE(document->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "10");
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, NoOpTransactionDoesNotDestroyRedo)
{
    item("inside")->getRepr()->setAttribute("x", "42");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Edit"}, "tool-pointer");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));

    auto interaction = DocumentUndo::beginRollbackableInteraction(document.get());
    ASSERT_TRUE(interaction);
    interaction->commit(Util::Internal::ContextString{"No-op"}, "tool-pointer");

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "42");
}

TEST_F(SelectorInteractionTest, RollbackPreservesChangesPendingBeforeTheInteraction)
{
    item("inside")->getRepr()->setAttribute("x", "15");
    auto interaction = DocumentUndo::beginRollbackableInteraction(document.get());
    ASSERT_TRUE(interaction);
    item("inside")->getRepr()->setAttribute("y", "55");
    interaction->rollback();
    document->ensureUpToDate();

    EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "15");
    EXPECT_STREQ(item("inside")->getRepr()->attribute("y"), "10");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Pending edit"}, "tool-pointer");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "10");
}

TEST_F(SelectorInteractionTest, EscapeCancellationRestoresLiveTransformWithoutUndo)
{
    auto original = item("inside")->i2dt_affine();
    auto const before = sp_repr_save_buf(document->getReprDoc());

    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    ASSERT_TRUE(tool->_seltrans->isGrabbed());
    tool->_seltrans->transform(Geom::Translate(25, 7), Geom::Point(0, 0));
    EXPECT_FALSE(affine_near(item("inside")->i2dt_affine(), original));
    ASSERT_TRUE(tool->_seltrans->cancel());

    document->ensureUpToDate();
    EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(), original));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before.raw());
    EXPECT_EQ(desktop->getSelection()->single(), item("inside"));
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(tool->_seltrans->cancel());
}

TEST_F(SelectorInteractionTest, ResizeHudIsTransientAndTracksIndependentDimensions)
{
    auto selection = desktop->getSelection();
    auto const bbox = selection->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto const before = sp_repr_save_buf(document->getReprDoc());
    desktop->getNamedView()->snap_manager.snapprefs.setSnapEnabledGlobally(false);

    begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
    EXPECT_FALSE(tool->_seltrans->hasResizeDimensionOverlay());
    EXPECT_EQ(tool->_seltrans->resizeDimensionCanvasItemCount(), 0u);

    auto point = bbox->min() + Geom::Point(20, 30);
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));
    ASSERT_TRUE(tool->_seltrans->hasResizeDimensionOverlay());
    EXPECT_EQ(tool->_seltrans->resizeDimensionCanvasItemCount(), 2u);
    tool->_seltrans->commitAbsoluteAffine();
    ASSERT_TRUE(tool->_seltrans->resizeDimensionLabels());
    EXPECT_EQ(tool->_seltrans->resizeDimensionLabels()->width.raw(), "20.00 px");
    EXPECT_EQ(tool->_seltrans->resizeDimensionLabels()->height.raw(), "30.00 px");

    // The live item transform and HUD remain view state until release.
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));

    ASSERT_TRUE(tool->_seltrans->cancel());
    EXPECT_FALSE(tool->_seltrans->hasResizeDimensionOverlay());
    EXPECT_EQ(tool->_seltrans->resizeDimensionCanvasItemCount(), 0u);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, ResizePressWithoutAcceptedMotionCreatesNoHudOrUndo)
{
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto const before = sp_repr_save_buf(document->getReprDoc());

    begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
    ASSERT_TRUE(tool->_seltrans->isGrabbed());
    EXPECT_FALSE(tool->_seltrans->hasResizeDimensionOverlay());
    EXPECT_EQ(tool->_seltrans->resizeDimensionCanvasItemCount(), 0u);

    tool->_seltrans->ungrab();
    EXPECT_FALSE(tool->_seltrans->hasResizeDimensionOverlay());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, ResizeHudFreezesDisplayUnitForOneDrag)
{
    auto selection = desktop->getSelection();
    auto const bbox = selection->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto namedview = desktop->getNamedView();
    auto &units = Util::UnitTable::get();
    namedview->display_units = units.getUnit("mm");

    begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
    EXPECT_FALSE(tool->_seltrans->hasResizeDimensionOverlay());

    // Simulate another view changing the document display unit during this transaction.
    namedview->display_units = units.getUnit("in");
    auto point = bbox->min() + bbox->dimensions() * Geom::Scale(1.5, 2.0);
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));
    ASSERT_TRUE(tool->_seltrans->resizeDimensionLabels());
    EXPECT_TRUE(tool->_seltrans->resizeDimensionLabels()->width.raw().ends_with(" mm"));
    EXPECT_TRUE(tool->_seltrans->resizeDimensionLabels()->height.raw().ends_with(" mm"));

    ASSERT_TRUE(tool->_seltrans->cancel());
    namedview->display_units = units.getUnit("px");
}

TEST_F(SelectorInteractionTest, ResizeHudDetentAccentKeepsNumericLabelsReadable)
{
    auto const unit = Util::UnitTable::get().getUnit("mm");
    ASSERT_TRUE(unit);

    SelectionDimensionOverlay overlay(*desktop, *unit, true, true);
    overlay.update(Geom::Rect::from_xywh(0, 0, 30, 40), true, true);

    EXPECT_TRUE(overlay.width_accented());
    EXPECT_TRUE(overlay.height_accented());
    EXPECT_FALSE(overlay.labels().width.empty());
    EXPECT_FALSE(overlay.labels().height.empty());
    EXPECT_GE(overlay.width_contrast_ratio(), 4.5);
    EXPECT_GE(overlay.height_contrast_ratio(), 4.5);
}

TEST_F(SelectorInteractionTest, ResizeHudReleaseCreatesOnlyTheExistingScaleUndo)
{
    auto selection = desktop->getSelection();
    auto const bbox = selection->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto const before = sp_repr_save_buf(document->getReprDoc());
    desktop->getNamedView()->snap_manager.snapprefs.setSnapEnabledGlobally(false);

    begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
    auto point = bbox->min() + bbox->dimensions() * Geom::Scale(1.5, 2.0);
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));
    tool->_seltrans->commitAbsoluteAffine();
    tool->_seltrans->ungrab();
    document->ensureUpToDate();

    EXPECT_FALSE(tool->_seltrans->hasResizeDimensionOverlay());
    EXPECT_NE(sp_repr_save_buf(document->getReprDoc()), before);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, CornerResizeDetentsBothActiveDimensionsAndAccentsHud)
{
    enable_only_dimensional_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto const original_affine = item("inside")->i2dt_affine();
    auto const original_xml = sp_repr_save_buf(document->getReprDoc());
    auto const zoom = desktop->current_zoom();
    ASSERT_GT(zoom, 0.0);
    auto const offset = std::min(0.25, 3.0 / zoom);

    begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
    auto point = bbox->min() + Geom::Point(20.0 - offset, 15.0 - offset);
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));

    auto const bounds = tool->_seltrans->resizeDimensionBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), 20.0, 1e-8);
    EXPECT_NEAR(bounds->height(), 15.0, 1e-8);
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{true, true}));
    ASSERT_TRUE(tool->_seltrans->resizeDimensionLabels());
    EXPECT_EQ(tool->_seltrans->resizeDimensionLabels()->width.raw(), "20.00 px");
    EXPECT_EQ(tool->_seltrans->resizeDimensionLabels()->height.raw(), "15.00 px");

    tool->_seltrans->commitAbsoluteAffine();
    EXPECT_FALSE(affine_near(item("inside")->i2dt_affine(), original_affine));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), original_xml);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_TRUE(tool->_seltrans->cancel());
    EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(), original_affine));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), original_xml);
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{false, false}));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, LockedAspectDetentsUseThePreLockSurvivingScaleAxis)
{
    enable_only_dimensional_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto const zoom = desktop->current_zoom();
    auto const coupled_factor = std::sqrt(2.0) * zoom;
    auto const offset = std::min(0.25, 2.0 / coupled_factor);
    auto const confine_state = static_cast<unsigned>(Modifiers::Modifier::get(
        Modifiers::Type::TRANS_CONFINE)->get_and_mask());

    begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
    // Existing ratio-lock math keeps the smaller Y scale and replaces X. The detent must
    // therefore evaluate Y, irrespective of which pointer component travelled farther.
    auto point = bbox->min() + Geom::Point(18.0, 15.0 - offset);
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, confine_state));

    auto const bounds = tool->_seltrans->resizeDimensionBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), 15.0, 1e-8);
    EXPECT_NEAR(bounds->height(), 15.0, 1e-8);
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{false, true}));
    EXPECT_TRUE(tool->_seltrans->cancel());
}

TEST_F(SelectorInteractionTest, LockedAspectWideSelectionUsesWidthAcrossNormalAndFlippedResize)
{
    set_rect("inside", "10", "10", "40", "10");
    enable_only_dimensional_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    ASSERT_NEAR(bbox->width(), 40.0, 1e-9);
    ASSERT_NEAR(bbox->height(), 10.0, 1e-9);
    auto const initial_affine = item("inside")->i2dt_affine();

    auto const confine_state = static_cast<unsigned>(Modifiers::Modifier::get(
        Modifiers::Type::TRANS_CONFINE)->get_and_mask());
    auto const screen_factor = Geom::L2(bbox->dimensions()) * desktop->current_zoom() /
                               bbox->width();
    ASSERT_GT(screen_factor, 0.0);
    auto const offset = std::min(0.25, 2.0 / screen_factor);

    auto verify = [&] (double width) {
        begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
        // The raw Y scale is deliberately much larger and its pointer displacement is also
        // larger. Existing ratio-lock math nevertheless keeps the smaller X scale.
        auto point = bbox->min() + Geom::Point(width, 12.0 * bbox->height());
        ASSERT_TRUE(tool->_seltrans->scaleRequest(point, confine_state));
        auto const bounds = tool->_seltrans->resizeDimensionBounds();
        ASSERT_TRUE(bounds);
        EXPECT_NEAR(bounds->width(), 60.0, 1e-8);
        EXPECT_NEAR(bounds->height(), 15.0, 1e-8);
        EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(),
                  (std::array<bool, 2>{true, false}));
        tool->_seltrans->commitAbsoluteAffine();
        EXPECT_EQ(item("inside")->i2dt_affine()[0] * initial_affine[0] < 0.0,
                  width < 0.0);
        EXPECT_TRUE(tool->_seltrans->cancel());
    };

    verify(60.0 - offset);
    verify(-60.0 + offset);
}

TEST_F(SelectorInteractionTest, LockedAspectTallSelectionUsesHeightAcrossNormalAndFlippedResize)
{
    set_rect("inside", "10", "10", "10", "40");
    enable_only_dimensional_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    ASSERT_NEAR(bbox->width(), 10.0, 1e-9);
    ASSERT_NEAR(bbox->height(), 40.0, 1e-9);
    auto const initial_affine = item("inside")->i2dt_affine();

    auto const confine_state = static_cast<unsigned>(Modifiers::Modifier::get(
        Modifiers::Type::TRANS_CONFINE)->get_and_mask());
    auto const screen_factor = Geom::L2(bbox->dimensions()) * desktop->current_zoom() /
                               bbox->height();
    ASSERT_GT(screen_factor, 0.0);
    auto const offset = std::min(0.25, 2.0 / screen_factor);

    auto verify = [&] (double height) {
        begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
        // The raw X scale and pointer displacement are deliberately larger. Existing
        // ratio-lock math keeps the smaller Y scale, so only height may own the detent.
        auto point = bbox->min() + Geom::Point(12.0 * bbox->width(), height);
        ASSERT_TRUE(tool->_seltrans->scaleRequest(point, confine_state));
        auto const bounds = tool->_seltrans->resizeDimensionBounds();
        ASSERT_TRUE(bounds);
        EXPECT_NEAR(bounds->width(), 15.0, 1e-8);
        EXPECT_NEAR(bounds->height(), 60.0, 1e-8);
        EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(),
                  (std::array<bool, 2>{false, true}));
        tool->_seltrans->commitAbsoluteAffine();
        EXPECT_EQ(item("inside")->i2dt_affine()[3] * initial_affine[3] < 0.0,
                  height < 0.0);
        EXPECT_TRUE(tool->_seltrans->cancel());
    };

    verify(60.0 - offset);
    verify(-60.0 + offset);
}

TEST_F(SelectorInteractionTest, GreenCornerSizeStepWinsOverNearbyBboxTarget)
{
    set_rect("crossing", "60.35", "40.65", "7", "9");
    enable_dimensional_and_bbox_corner_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);

    begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
    auto const offset = std::min(0.25, 2.0 / desktop->current_zoom());
    auto point = bbox->min() + Geom::Point(20.0 - offset, 15.0 - offset);
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(),
              (std::array<bool, 2>{true, true}));

    // A green handle gives a captured raw size priority over the nearby target.
    point = Geom::Point(60.10, 40.40);
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));

    auto const bounds = tool->_seltrans->resizeDimensionBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), 50.0, 1e-8);
    EXPECT_NEAR(bounds->height(), 30.0, 1e-8);
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(),
              (std::array<bool, 2>{true, true}));
    EXPECT_TRUE(tool->_seltrans->cancel());
}

TEST_F(SelectorInteractionTest, GreenSizeStepsDoNotMergeAnUnrelatedGeometricAxis)
{
    // The target's top edge midpoint is (30.35, 10). The selected rectangle's top edge
    // midpoint lies on the scaling origin's horizontal axis. A captured green
    // step must use the raw target, not merge that competing X correction.
    set_rect("crossing", "28.35", "10", "4", "7");
    set_rect("bitmap", "80", "80", "10", "10");
    enable_dimensional_and_bbox_edge_midpoint_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);

    begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
    auto const offset = std::min(0.25, 2.0 / desktop->current_zoom());
    auto point = Geom::Point(bbox->left() + 40.0 - offset, bbox->top() + 25.0 - offset);
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));

    auto const bounds = tool->_seltrans->resizeDimensionBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), 40.0, 1e-8);
    EXPECT_NEAR(bounds->height(), 25.0, 1e-8);
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(),
              (std::array<bool, 2>{true, true}));
    EXPECT_TRUE(tool->_seltrans->cancel());
}

TEST_F(SelectorInteractionTest, GreenCornerSizeStepWinsOverNearbyNodeTarget)
{
    set_rect("crossing", "60.35", "40.65", "7", "9");
    enable_dimensional_and_special_point_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);

    begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
    auto const offset = std::min(0.25, 2.0 / desktop->current_zoom());
    auto point = bbox->min() + Geom::Point(20.0 - offset, 15.0 - offset);
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(),
              (std::array<bool, 2>{true, true}));

    // Green steps precede the special-point domain and keep geometry/labels identical.
    point = Geom::Point(60.10, 40.40);
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));

    auto const bounds = tool->_seltrans->resizeDimensionBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), 50.0, 1e-8);
    EXPECT_NEAR(bounds->height(), 30.0, 1e-8);
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(),
              (std::array<bool, 2>{true, true}));
    EXPECT_TRUE(tool->_seltrans->cancel());
}

TEST(SelectorHandlePolicy, LowerAndRightResizeAndLowerRotationHandlesUseGreenSteps)
{
    unsigned resize = 0, rotation = 0;
    for (auto const &handle : hands) {
        bool const stepped = selection_handle_uses_steps(handle.type, handle.x, handle.y);
        if (handle.type == HANDLE_SCALE || handle.type == HANDLE_STRETCH) {
            EXPECT_EQ(stepped, handle.y == 0.0 || (handle.type == HANDLE_STRETCH && handle.x == 1.0));
            resize += stepped;
        } else if (handle.type == HANDLE_ROTATE) {
            EXPECT_EQ(stepped, handle.y == 0.0);
            rotation += stepped;
        } else {
            EXPECT_FALSE(stepped); // No midpoint skew/center/alignment reassignment.
        }
    }
    EXPECT_EQ(resize, 4u);
    EXPECT_EQ(rotation, 2u);
    EXPECT_EQ(SELECTION_STEP_HANDLE_COLOR, 0x169447ffu);
}

TEST_F(SelectorInteractionTest, ActualSideHandleDispatchDistinguishesFreeAndSteppedWithSnapDelay)
{
    enable_only_dimensional_snapping(*desktop);
    desktop->getNamedView()->snap_manager.snapprefs.setSnapPostponedGlobally(true);
    auto &trans = *tool->_seltrans;
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto const offset = std::min(0.25, 2.0 / desktop->current_zoom());
    for (auto index : {2, 4, 6, 8}) { // Top/right/bottom/left, same dispatch as real knots.
        auto const &handle = hands[index];
        SCOPED_TRACE(index);
        bool const stepped = selection_handle_uses_steps(handle.type, handle.x, handle.y);
        auto const axis = handle.x == 0.5 ? Geom::Y : Geom::X;
        Geom::Point const fraction(handle.x, (handle.y - 0.5) * (-desktop->yaxisdir()) + 0.5);
        auto const start = bbox->min() + bbox->dimensions() * Geom::Scale(fraction);
        SPKnot knot(desktop.get(), "Test handle", CANVAS_ITEM_CTRL_TYPE_ADJ_HANDLE);
        knot.moveto(start);
        knot.setFlag(SP_KNOT_GRABBED, true);
        trans.handleGrab(&knot, 0, handle);
        auto point = start;
        double const target = bbox->dimensions()[axis] + 5.0;
        point[axis] += (fraction[axis] == 0.0 ? -1.0 : 1.0) * (5.0 - offset);
        ASSERT_TRUE(trans.handleRequest(&knot, &point, 0, handle));
        auto bounds = trans.resizeDimensionBounds();
        ASSERT_TRUE(bounds);
        EXPECT_NEAR(bounds->dimensions()[axis], stepped ? target : target - offset, 1e-8);
        EXPECT_NEAR(bounds->dimensions()[1 - axis], bbox->dimensions()[1 - axis], 1e-8);
        EXPECT_EQ(trans.resizeDimensionAccents()[axis], stepped);
        trans.commitAbsoluteAffine();
        EXPECT_TRUE(trans.cancel());
        knot.setFlag(SP_KNOT_GRABBED, false);
    }
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, ActualRotationHandleDispatchDistinguishesFreeAndFifteenDegreeSteps)
{
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto const pivot = *desktop->getSelection()->center();
    desktop->zoom_absolute(pivot, 8.0);
    desktop->getNamedView()->snap_manager.snapprefs.setSnapPostponedGlobally(true);
    for (auto index : {9, 11, 13, 15}) {
        auto const &handle = hands[index];
        SCOPED_TRACE(index);
        bool const stepped = selection_handle_uses_steps(handle.type, handle.x, handle.y);
        trans.resetState(SelTrans::STATE_ROTATE);
        auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
        ASSERT_TRUE(bbox);
        Geom::Point const fraction(handle.x, (handle.y - 0.5) * (-desktop->yaxisdir()) + 0.5);
        auto const start = bbox->min() + bbox->dimensions() * Geom::Scale(fraction);
        SPKnot knot(desktop.get(), "Test rotation handle", CANVAS_ITEM_CTRL_TYPE_ADJ_ROTATE);
        knot.moveto(start);
        knot.setFlag(SP_KNOT_GRABBED, true);
        trans.handleGrab(&knot, 0, handle);
        auto point = start * rotation_about(pivot, 14.0);
        ASSERT_TRUE(trans.handleRequest(&knot, &point, 0, handle));
        ASSERT_TRUE(trans.rotationAngleDegrees());
        EXPECT_NEAR(*trans.rotationAngleDegrees(), stepped ? 15.0 : 14.0, 1e-8);
        ASSERT_TRUE(trans.rotationOverlay());
        EXPECT_EQ(trans.rotationOverlay()->accented(), stepped);
        trans.commitRelativeAffine();
        EXPECT_TRUE(trans.cancel());
        knot.setFlag(SP_KNOT_GRABBED, false);
    }
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, VerticalSideDetentPreservesWidthForMixedSelection)
{
    desktop->getSelection()->add(item("crossing"));
    enable_only_dimensional_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto const offset = std::min(0.25, 2.0 / desktop->current_zoom());

    begin_vertical_stretch(*tool->_seltrans, *bbox);
    auto point = Geom::Point(bbox->midpoint()[Geom::X], bbox->top() + 35.0 - offset);
    ASSERT_TRUE(tool->_seltrans->stretchRequest(point, 0, false));

    auto const bounds = tool->_seltrans->resizeDimensionBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), bbox->width(), 1e-8);
    EXPECT_NEAR(bounds->height(), 35.0, 1e-8);
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(),
              (std::array<bool, 2>{false, true}));
    tool->_seltrans->commitAbsoluteAffine();
    auto const live_bounds = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(live_bounds);
    EXPECT_NEAR(live_bounds->width(), bbox->width(), 1e-8);
    EXPECT_NEAR(live_bounds->height(), 35.0, 1e-8);
    EXPECT_TRUE(tool->_seltrans->cancel());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, VisualBboxHudAndDetentsUseCommittedTargetWithNonuniformStrokes)
{
    set_rect("inside", "15", "15", "5", "5",
             "fill:#fff;stroke:#000;stroke-width:2");
    set_rect("crossing", "5", "5", "40", "30",
             "fill:#fff;stroke:#000;stroke-width:10");
    desktop->getSelection()->add(item("crossing"));
    enable_only_dimensional_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::VISUAL_BBOX);
    ASSERT_TRUE(bbox);
    ASSERT_NEAR(bbox->width(), 50.0, 1e-8);
    ASSERT_NEAR(bbox->height(), 40.0, 1e-8);
    auto const offset = std::min(0.25, 2.0 / desktop->current_zoom());

    begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
    auto point = bbox->min() + Geom::Point(75.0 - offset, 60.0 - offset);
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));

    auto const target = tool->_seltrans->resizeDimensionBounds();
    ASSERT_TRUE(target);
    EXPECT_NEAR(target->width(), 75.0, 1e-8);
    EXPECT_NEAR(target->height(), 60.0, 1e-8);
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(),
              (std::array<bool, 2>{true, true}));

    // commitAbsoluteAffine() updates the live preview only. The normal handle-release path calls
    // ungrab(), which serializes that affine, commits the rollbackable interaction, and refreshes
    // selection bounds. Compare against the live document only after that complete lifecycle.
    tool->_seltrans->commitAbsoluteAffine();
    tool->_seltrans->ungrab();
    document->ensureUpToDate();
    auto const live_bounds = desktop->getSelection()->bounds(SPItem::VISUAL_BBOX);
    ASSERT_TRUE(live_bounds);
    EXPECT_NEAR(live_bounds->width(), target->width(), 1e-8);
    EXPECT_NEAR(live_bounds->height(), target->height(), 1e-8);
    EXPECT_FALSE(tool->_seltrans->hasResizeDimensionOverlay());

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    auto const restored_bounds = desktop->getSelection()->bounds(SPItem::VISUAL_BBOX);
    ASSERT_TRUE(restored_bounds);
    EXPECT_NEAR(restored_bounds->width(), bbox->width(), 1e-8);
    EXPECT_NEAR(restored_bounds->height(), bbox->height(), 1e-8);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, BitmapHorizontalStretchNeverChangesHeightForNearbyDetent)
{
    desktop->getSelection()->set(item("bitmap"));
    enable_only_dimensional_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto const original_height = bbox->height();
    auto const offset = std::min(0.25, 3.0 / desktop->current_zoom());

    begin_horizontal_stretch(*tool->_seltrans, *bbox);
    auto point = Geom::Point(bbox->left() + 20.0 - offset, bbox->midpoint()[Geom::Y]);
    ASSERT_TRUE(tool->_seltrans->stretchRequest(point, 0, true));

    auto const bounds = tool->_seltrans->resizeDimensionBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), 20.0, 1e-8);
    EXPECT_NEAR(bounds->height(), original_height, 1e-8);
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{true, false}));

    tool->_seltrans->commitAbsoluteAffine();
    auto const live_bounds = item("bitmap")->desktopBounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(live_bounds);
    EXPECT_NEAR(live_bounds->width(), 20.0, 1e-8);
    EXPECT_NEAR(live_bounds->height(), original_height, 1e-8);
    EXPECT_TRUE(tool->_seltrans->cancel());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, DimensionalDetentsPreserveScaleSignAcrossFlip)
{
    enable_only_dimensional_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto const offset = std::min(0.25, 3.0 / desktop->current_zoom());

    begin_horizontal_stretch(*tool->_seltrans, *bbox);
    auto point = Geom::Point(bbox->left() - 20.0 + offset, bbox->midpoint()[Geom::Y]);
    ASSERT_TRUE(tool->_seltrans->stretchRequest(point, 0, true));
    auto const bounds = tool->_seltrans->resizeDimensionBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width(), 20.0, 1e-8);
    tool->_seltrans->commitAbsoluteAffine();
    EXPECT_LT(item("inside")->i2dt_affine()[0], 0.0);
    EXPECT_TRUE(tool->_seltrans->cancel());
}

TEST_F(SelectorInteractionTest, PreferenceMasterAndTransformModifiersBypassAndClearDetents)
{
    enable_only_dimensional_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto const offset = std::min(0.25, 3.0 / desktop->current_zoom());
    auto const free_point = bbox->min() + Geom::Point(20.0 - offset, 15.0 - offset);

    begin_corner_resize(*tool->_seltrans, *desktop, *bbox);
    auto point = free_point;
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{true, true}));

    auto const no_snap_state = static_cast<unsigned>(Modifiers::Modifier::get(
        Modifiers::Type::TRANS_NO_SNAPPING)->get_and_mask());
    point = free_point;
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, no_snap_state));
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{false, false}));

    point = free_point;
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));
    ASSERT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{true, true}));
    Preferences::get()->setBool("/options/snapdimensional/value", false);
    point = free_point;
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{false, false}));

    Preferences::get()->setBool("/options/snapdimensional/value", true);
    point = free_point;
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));
    ASSERT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{true, true}));
    desktop->getNamedView()->snap_manager.snapprefs.setSnapEnabledGlobally(false);
    point = free_point;
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{false, false}));

    desktop->getNamedView()->snap_manager.snapprefs.setSnapEnabledGlobally(true);
    point = free_point;
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, 0));
    ASSERT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{true, true}));
    auto const increment_state = static_cast<unsigned>(Modifiers::Modifier::get(
        Modifiers::Type::TRANS_INCREMENT)->get_and_mask());
    point = free_point;
    ASSERT_TRUE(tool->_seltrans->scaleRequest(point, increment_state));
    EXPECT_EQ(tool->_seltrans->resizeDimensionAccents(), (std::array<bool, 2>{false, false}));

    EXPECT_TRUE(tool->_seltrans->cancel());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, CrossingOneHundredDetentsStillCreatesOneStableUndo)
{
    enable_only_dimensional_snapping(*desktop);
    auto const bbox = desktop->getSelection()->bounds(SPItem::GEOMETRIC_BBOX);
    ASSERT_TRUE(bbox);
    auto const before = sp_repr_save_buf(document->getReprDoc());
    auto const zoom = desktop->current_zoom();
    auto const offset = std::min(0.25, 2.0 / zoom);
    auto const stride = std::max(1u, static_cast<unsigned>(std::ceil(
        (DIMENSIONAL_DETENT_STRONG_RELEASE_PX + 2.0) / zoom)));
    double final_target = 0.0;

    begin_horizontal_stretch(*tool->_seltrans, *bbox);
    for (unsigned index = 1; index <= 100; ++index) {
        double const target = 10.0 + index * stride;
        auto point = Geom::Point(bbox->left() + target + offset,
                                 bbox->midpoint()[Geom::Y]);
        ASSERT_TRUE(tool->_seltrans->stretchRequest(point, 0, true));
        tool->_seltrans->commitAbsoluteAffine();
        final_target = target;
    }
    auto const final_bounds = tool->_seltrans->resizeDimensionBounds();
    ASSERT_TRUE(final_bounds);
    EXPECT_NEAR(final_bounds->width(), final_target, 1e-8);
    tool->_seltrans->ungrab();
    document->ensureUpToDate();

    auto const committed = sp_repr_save_buf(document->getReprDoc());
    EXPECT_NE(committed, before);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), committed);
}

TEST_F(SelectorInteractionTest, EmbeddedSvgDragsPreserveViewportAndUndoRedo)
{
    auto source = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" id="embedded" x="5" y="7"
     width="20" height="40" viewBox="0 0 10 20" preserveAspectRatio="none">
  <rect id="embedded-leaf" x="1" y="2" width="7" height="13"/>
</svg>)svg");
    auto repr = source->getReprRoot()->duplicate(document->getReprDoc());
    document->getReprRoot()->appendChild(repr);
    GC::release(repr);
    document->ensureUpToDate();
    desktop->getSelection()->set(item("embedded"));
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Test setup"}, "");
    DocumentUndo::clearUndo(document.get());
    auto before = sp_repr_save_buf(document->getReprDoc());
    auto baseline = item("embedded-leaf")->i2dt_affine();
    for (Geom::Affine motion : {Geom::Affine(Geom::Translate(25, 7)),
                               Geom::Affine(Geom::Scale(.6, 1.3)),
                               Geom::Affine(Geom::Rotate::from_degrees(37))}) {
        SCOPED_TRACE(motion);
        desktop->getSelection()->set(item("embedded"));
        tool->_seltrans->grab({10, 10}, -1, -1, false, true);
        tool->_seltrans->transform(motion, {0, 0});
        document->ensureUpToDate();
        EXPECT_TRUE(affine_near(item("embedded-leaf")->i2dt_affine(), baseline * motion));
        tool->_seltrans->ungrab();
        document->ensureUpToDate();
        EXPECT_TRUE(affine_near(item("embedded-leaf")->i2dt_affine(), baseline * motion));
        auto committed = sp_repr_save_buf(document->getReprDoc());
        auto reopened = SPDocument::createNewDocFromMem(committed.raw());
        ASSERT_TRUE(reopened);
        reopened->ensureUpToDate();
        auto leaf = cast<SPItem>(reopened->getObjectById("embedded-leaf"));
        ASSERT_TRUE(leaf);
        EXPECT_TRUE(affine_near(leaf->i2doc_affine(), item("embedded-leaf")->i2doc_affine()));
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        document->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
        ASSERT_TRUE(DocumentUndo::redo(document.get()));
        document->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), committed);
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        document->ensureUpToDate();
    }
}

TEST_F(SelectorInteractionTest, ReleaseCommitsOneTransformAndUndoRedoRoundTrips)
{
    auto const before = sp_repr_save_buf(document->getReprDoc());
    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    tool->_seltrans->transform(Geom::Translate(25, 7), Geom::Point(0, 0));
    tool->_seltrans->ungrab();
    document->ensureUpToDate();
    auto const committed = sp_repr_save_buf(document->getReprDoc());
    EXPECT_NE(committed, before);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before.raw());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), committed.raw());
}

TEST_F(SelectorInteractionTest, DuplicateDragCancellationRemovesDuplicateAndRestoresSelection)
{
    auto const before = sp_repr_save_buf(document->getReprDoc());
    ASSERT_TRUE(tool->_seltrans->beginInteraction());
    desktop->getSelection()->duplicate(true);
    document->ensureUpToDate();
    ASSERT_NE(desktop->getSelection()->single(), item("inside"));

    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    tool->_seltrans->transform(Geom::Translate(20, 0), Geom::Point(0, 0));
    ASSERT_TRUE(tool->_seltrans->cancel());
    document->ensureUpToDate();

    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before.raw());
    EXPECT_EQ(desktop->getSelection()->single(), item("inside"));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, CancelDoesNotConsumeAnExistingRedoStep)
{
    item("inside")->getRepr()->setAttribute("x", "42");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Edit"}, "tool-pointer");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));

    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    tool->_seltrans->transform(Geom::Translate(4, 3), Geom::Point(0, 0));
    ASSERT_TRUE(tool->_seltrans->cancel());

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "42");
}

TEST_F(SelectorInteractionTest, UndoCannotConsumeHistoryWhilePointerTransactionIsActive)
{
    item("crossing")->getRepr()->setAttribute("x", "40");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Earlier edit"}, "tool-pointer");
    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    tool->_seltrans->transform(Geom::Translate(4, 3), Geom::Point(0, 0));

    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    EXPECT_TRUE(tool->_seltrans->isGrabbed());
    ASSERT_TRUE(tool->_seltrans->cancel());

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_STREQ(item("crossing")->getRepr()->attribute("x"), "18");
}

TEST_F(SelectorInteractionTest, MultiObjectCancellationIsAtomic)
{
    desktop->getSelection()->add(item("crossing"));
    auto first = item("inside")->i2dt_affine();
    auto second = item("crossing")->i2dt_affine();
    auto const before = sp_repr_save_buf(document->getReprDoc());

    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    tool->_seltrans->transform(Geom::Scale(0.5, 1.75), Geom::Point(0, 0));
    ASSERT_TRUE(tool->_seltrans->cancel());

    document->ensureUpToDate();
    EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(), first));
    EXPECT_TRUE(affine_near(item("crossing")->i2dt_affine(), second));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
    EXPECT_EQ(desktop->getSelection()->size(), 2u);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, SelectionChangeCancelsActiveTransformAndRestoresBaselineSelection)
{
    auto const before = sp_repr_save_buf(document->getReprDoc());
    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    tool->_seltrans->transform(Geom::Translate(30, 0), Geom::Point(0, 0));

    desktop->getSelection()->set(item("crossing"));

    EXPECT_FALSE(tool->_seltrans->isGrabbed());
    EXPECT_EQ(desktop->getSelection()->single(), item("inside"));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, DocumentReplacementRollsBackAgainstCapturedOldDocument)
{
    auto const before_xml = sp_repr_save_buf(document->getReprDoc());
    auto const before_affine = item("inside")->i2dt_affine();
    auto old_item = item("inside");
    ASSERT_TRUE(old_item);

    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    tool->_seltrans->transform(Geom::Translate(30, 7), Geom::Point(0, 0));
    ASSERT_FALSE(affine_near(old_item->i2dt_affine(), before_affine));

    auto replacement = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50">
  <rect id="replacement" x="2" y="3" width="4" height="5"/>
</svg>)svg");
    ASSERT_TRUE(replacement);
    replacement->ensureUpToDate();

    desktop->setDocument(replacement.get());

    EXPECT_FALSE(tool->_seltrans->isGrabbed());
    EXPECT_FALSE(tool->_seltrans->hasResizeDimensionOverlay());
    EXPECT_TRUE(affine_near(old_item->i2dt_affine(), before_affine));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before_xml);
    EXPECT_TRUE(desktop->getSelection()->isEmpty());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(DocumentUndo::undo(replacement.get()));

    // Restore the fixture's document while both documents are alive so desktop teardown never
    // observes a dangling replacement document.
    desktop->setDocument(document.get());
}

TEST_F(SelectorInteractionTest, RotationDetentsAcceptBaselineGeometryAndReleaseInBothDirections)
{
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto const pivot = *desktop->getSelection()->center();
    auto const baseline = item("inside")->i2dt_affine();
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    for (double sign : {-1.0, 1.0}) {
        auto const start = begin_rotation(trans, *desktop, pivot);
        for (double raw : {14.0, 15.0, 16.0, 19.0, 20.0}) {
            auto point = start * rotation_about(pivot, sign * raw);
            ASSERT_TRUE(trans.rotateRequest(point, 0));
            trans.commitRelativeAffine();
            double const accepted = sign * (raw < 20 ? 15.0 : raw);
            ASSERT_TRUE(trans.rotationAngleDegrees());
            EXPECT_NEAR(*trans.rotationAngleDegrees(), accepted, 1e-9);
            EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(),
                                  baseline * rotation_about(pivot, accepted)));
            ASSERT_TRUE(trans.rotationOverlay());
            EXPECT_EQ(trans.rotationOverlay()->canvas_item_count(), 1u);
            EXPECT_TRUE(trans.rotationOverlay()->visible());
            EXPECT_EQ(trans.rotationOverlay()->accented(), raw < 20);
            EXPECT_GE(trans.rotationOverlay()->contrast_ratio(), 4.5);
            EXPECT_EQ(trans.rotationOverlay()->label().raw(), format_selection_rotation(accepted).raw());
            EXPECT_FALSE(trans.hasResizeDimensionOverlay());
            EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
        }
        ASSERT_TRUE(trans.cancel());
        EXPECT_FALSE(trans.rotationOverlay());
        EXPECT_FALSE(trans.rotationAngleDegrees());
        EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(), baseline));
    }
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, RotationMultipleTurnsRemainContinuousWithoutCumulativeGeometryDrift)
{
    desktop->getNamedView()->snap_manager.snapprefs.setSnapEnabledGlobally(false);
    auto &trans = *tool->_seltrans;
    auto const pivot = *desktop->getSelection()->center();
    auto const baseline = item("inside")->i2dt_affine();
    for (double sign : {-1.0, 1.0}) {
        auto const start = begin_rotation(trans, *desktop, pivot);
        for (int degree = 0; degree <= 1080; degree += 3) {
            auto point = start * rotation_about(pivot, sign * degree);
            ASSERT_TRUE(trans.rotateRequest(point, 0));
            trans.commitRelativeAffine();
            ASSERT_TRUE(trans.rotationAngleDegrees());
            EXPECT_NEAR(*trans.rotationAngleDegrees(), sign * degree, 1e-8);
            EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(),
                                  baseline * rotation_about(pivot, sign * degree)));
            EXPECT_FALSE(trans.rotationOverlay()->accented());
        }
        trans.ungrab();
        EXPECT_FALSE(trans.rotationOverlay());
        EXPECT_FALSE(DocumentUndo::undo(document.get()));
    }
}

TEST_F(SelectorInteractionTest, RotationSnappingToggleBypassAndExplicitConstraintsKeepLabelParity)
{
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto &snap = desktop->getNamedView()->snap_manager.snapprefs;
    auto const pivot = *desktop->getSelection()->center();
    auto const opposite = desktop->getSelection()->bounds(SPItem::VISUAL_BBOX)->min();
    auto const start = begin_rotation(trans, *desktop, pivot);
    auto verify = [&](double raw, unsigned state, double accepted, bool accented) {
        // Shift is both the default bypass and the existing opposite-pivot modifier.
        auto const origin = Modifiers::Modifier::get(Modifiers::Type::TRANS_OFF_CENTER)->active(state)
            ? opposite : pivot;
        auto point = start * rotation_about(origin, raw);
        ASSERT_TRUE(trans.rotateRequest(point, state));
        trans.commitRelativeAffine();
        ASSERT_TRUE(trans.rotationAngleDegrees());
        EXPECT_NEAR(*trans.rotationAngleDegrees(), accepted, 1e-9);
        ASSERT_TRUE(trans.rotationOverlay());
        EXPECT_EQ(trans.rotationOverlay()->accented(), accented);
        EXPECT_EQ(trans.rotationOverlay()->label().raw(), format_selection_rotation(accepted).raw());
        EXPECT_NEAR(Geom::L2(point - start * rotation_about(origin, accepted)), 0.0, 1e-9);
    };
    verify(14, 0, 15, true);
    snap.setSnapEnabledGlobally(false);
    verify(14, 0, 14, false);
    snap.setSnapEnabledGlobally(true);
    verify(14, 0, 15, true);
    auto const bypass = static_cast<unsigned>(Modifiers::Modifier::get(
        Modifiers::Type::TRANS_NO_SNAPPING)->get_and_mask());
    verify(14, bypass, 14, false);
    snap.setSnapPostponedGlobally(true);
    verify(14, 0, 15, true); // Cheap green steps remain live during geometric snap delay.
    snap.setSnapPostponedGlobally(false);

    auto const prefs = Preferences::get();
    double const old_snaps = prefs->getDouble("/options/rotationsnapsperpi/value", 12.0);
    prefs->setDouble("/options/rotationsnapsperpi/value", 8.0); // Existing 22.5-degree constraint.
    for (auto type : {Modifiers::Type::TRANS_INCREMENT, Modifiers::Type::TRANS_CONFINE}) {
        auto const state = static_cast<unsigned>(Modifiers::Modifier::get(type)->get_and_mask());
        verify(16, state, 22.5, false);
        snap.setSnapEnabledGlobally(false);
        verify(16, state, 22.5, false);
        snap.setSnapEnabledGlobally(true);
    }
    prefs->setDouble("/options/rotationsnapsperpi/value", old_snaps);
    ASSERT_TRUE(trans.cancel());
}

TEST_F(SelectorInteractionTest, RotationNearPivotRejectsNoiseAndReentersOnSameRevolution)
{
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto const pivot = *desktop->getSelection()->center();
    auto const start = begin_rotation(trans, *desktop, pivot);
    auto point = start * rotation_about(pivot, 14);
    ASSERT_TRUE(trans.rotateRequest(point, 0));
    trans.commitRelativeAffine();
    auto const accepted = item("inside")->i2dt_affine();
    for (double angle : {-170.0, 170.0, -90.0, 90.0}) {
        point = pivot + Geom::Point(1.0 / desktop->current_zoom(), 0) *
                        Geom::Rotate(Geom::rad_from_deg(angle));
        EXPECT_FALSE(trans.rotateRequest(point, 0));
        EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(), accepted));
        EXPECT_DOUBLE_EQ(*trans.rotationAngleDegrees(), 15.0);
        EXPECT_FALSE(trans.rotationOverlay()->accented());
    }
    point = pivot + Geom::Point(20.0 / desktop->current_zoom(), 0) *
                    Geom::Rotate(Geom::rad_from_deg(16));
    ASSERT_TRUE(trans.rotateRequest(point, 0));
    EXPECT_NEAR(*trans.rotationAngleDegrees(), 16, 1e-9);
    EXPECT_FALSE(trans.rotationOverlay()->accented());
    ASSERT_TRUE(trans.cancel());
}

TEST_F(SelectorInteractionTest, GreenRotationPriorityAndFreeHandleKeepGeometricFallback)
{
    set_rect("inside", "20", "20", "80", "80");
    set_rect("crossing", "20", "20", "80", "80");
    auto const pivot = *desktop->getSelection()->center();
    auto const baseline = item("inside")->i2dt_affine();
    auto target = item("crossing");
    target->set_i2d_affine(target->i2dt_affine() * rotation_about(pivot, 17));
    target->doWriteTransform(target->transform);
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Geometric rotation target"}, "");
    DocumentUndo::clearUndo(document.get());
    enable_dimensional_and_special_point_snapping(*desktop);

    auto &trans = *tool->_seltrans;
    auto &snap = desktop->getNamedView()->snap_manager.snapprefs;
    for (bool stepped : {true, false}) {
        SCOPED_TRACE(stepped);
        snap.setSnapEnabledGlobally(false);
        auto const start = begin_rotation(trans, *desktop, pivot, 100.0, stepped);
        auto point = start * rotation_about(pivot, 14);
        ASSERT_TRUE(trans.rotateRequest(point, 0));
        trans.commitRelativeAffine();
        EXPECT_NEAR(*trans.rotationAngleDegrees(), 14, 1e-9);
        snap.setSnapEnabledGlobally(true);
        point = start * rotation_about(pivot, 14);
        ASSERT_TRUE(trans.rotateRequest(point, 0));
        trans.commitRelativeAffine();
        ASSERT_TRUE(trans.rotationAngleDegrees());
        double const expected = stepped ? 15.0 : 17.0;
        EXPECT_NEAR(*trans.rotationAngleDegrees(), expected, 1e-4);
        EXPECT_EQ(trans.rotationOverlay()->accented(), stepped);
        EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(), baseline * rotation_about(pivot, expected), 1e-4));
        EXPECT_EQ(trans.rotationOverlay()->label().raw(), format_selection_rotation(*trans.rotationAngleDegrees()).raw());
        if (stepped) {
            // Outside the green latch's release band, ordinary geometry still wins.
            point = start * rotation_about(pivot, 20.5);
            ASSERT_TRUE(trans.rotateRequest(point, 0));
            trans.commitRelativeAffine();
            EXPECT_NEAR(*trans.rotationAngleDegrees(), 17.0, 1e-4);
            EXPECT_FALSE(trans.rotationOverlay()->accented());
        }
        ASSERT_TRUE(trans.cancel());
    }
}

TEST_F(SelectorInteractionTest, RotationCaptureIsScreenSpaceAcrossZoomAndContrastSurvivesThemeColors)
{
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto const pivot = *desktop->getSelection()->center();
    auto const style = desktop->getCanvas()->get_style_context();
    for (auto const background : {"#fafafa", "#181818"}) {
        auto css = Gtk::CssProvider::create();
        // Deliberately poor theme foregrounds exercise the shared contrast repair.
        css->load_from_data(std::string("@define-color theme_bg_color ") + background +
            "; @define-color theme_fg_color #808080;"
            " @define-color theme_selected_bg_color #888888;"
            " @define-color theme_selected_fg_color #888888;");
        style->add_provider(css, GTK_STYLE_PROVIDER_PRIORITY_USER);
        for (double zoom : {0.5, 1.0, 4.0}) {
            desktop->zoom_absolute(pivot, zoom);
            auto const start = begin_rotation(trans, *desktop, pivot, 100);
            for (double angle : {14.0, 19.0, 20.0}) {
                auto point = start * rotation_about(pivot, angle);
                ASSERT_TRUE(trans.rotateRequest(point, 0));
                trans.commitRelativeAffine();
                EXPECT_NEAR(*trans.rotationAngleDegrees(), angle < 20 ? 15 : 20, 1e-9);
                EXPECT_EQ(trans.rotationOverlay()->accented(), angle < 20);
                EXPECT_GE(trans.rotationOverlay()->contrast_ratio(), 4.5);
                EXPECT_TRUE(trans.rotationOverlay()->visible());
            }
            ASSERT_TRUE(trans.cancel());
        }
        style->remove_provider(css);
    }
}

TEST_F(SelectorInteractionTest, RotationTargetDeletionClearsFeedbackAndStopsGesture)
{
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    for (bool raw_xml_removal : {false, true}) {
        auto const pivot = *desktop->getSelection()->center();
        auto const start = begin_rotation(trans, *desktop, pivot);
        auto point = start * rotation_about(pivot, 14);
        ASSERT_TRUE(trans.rotateRequest(point, 0));
        trans.commitRelativeAffine();
        if (raw_xml_removal) {
            sp_repr_unparent(item("inside")->getRepr());
        } else {
            item("inside")->deleteObject();
        }
        EXPECT_FALSE(trans.rotationOverlay()); // Immediate cleanup, before deferred rollback.
        drain_main_context();
        EXPECT_FALSE(trans.isGrabbed());
        EXPECT_FALSE(trans.rotationOverlay());
        EXPECT_FALSE(trans.rotationAngleDegrees());
        ASSERT_TRUE(item("inside"));
        EXPECT_EQ(desktop->getSelection()->single(), item("inside"));
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
        EXPECT_FALSE(DocumentUndo::undo(document.get()));
    }
}

TEST_F(SelectorInteractionTest, RotationMovedCenterAndOffCenterModifierPreserveExistingPivot)
{
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    Geom::Point const pivot(70, 60);
    trans.setCenter(pivot);
    item("inside")->updateRepr();
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Moved pivot setup"}, "");
    DocumentUndo::clearUndo(document.get());
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto const baseline = item("inside")->i2dt_affine();
    auto const bbox = desktop->getSelection()->bounds(SPItem::VISUAL_BBOX);
    ASSERT_TRUE(bbox);
    auto const start = begin_rotation(trans, *desktop, pivot);
    auto point = start * rotation_about(pivot, 14);
    ASSERT_TRUE(trans.rotateRequest(point, 0));
    trans.commitRelativeAffine();
    EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(), baseline * rotation_about(pivot, 15)));
    // Our bottom/right handle's opposite is the bbox minimum, independent of the moved center.
    auto const off_center = static_cast<unsigned>(Modifiers::Modifier::get(
        Modifiers::Type::TRANS_OFF_CENTER)->get_and_mask());
    point = start * rotation_about(bbox->min(), 30);
    ASSERT_TRUE(trans.rotateRequest(point, off_center));
    trans.commitRelativeAffine();
    EXPECT_NEAR(*trans.rotationAngleDegrees(), 30, 1e-9);
    EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(), baseline * rotation_about(bbox->min(), 30)));
    ASSERT_TRUE(trans.cancel());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_NEAR(Geom::L2(item("inside")->getCenter() - pivot), 0, 1e-9);
}

TEST_F(SelectorInteractionTest, RotationReleaseIsOneUndoForMixedSelectionAndNoFeedbackXml)
{
    // Add only this test's group/text; existing snap fixtures remain untouched.
    auto extra = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <g id="rotated-group" transform="translate(5,7) skewX(13)">
    <rect id="child-vector" x="1" y="2" width="8" height="9" transform="rotate(9)"/>
    <text id="child-text" x="3" y="12" style="font-size:10px">Rotation</text>
  </g>
</svg>)svg");
    ASSERT_TRUE(extra);
    auto copy = extra->getObjectById("rotated-group")->getRepr()->duplicate(document->getReprDoc());
    document->getReprRoot()->appendChild(copy);
    Inkscape::GC::release(copy);
    document->ensureUpToDate();
    ASSERT_TRUE(item("rotated-group"));
    desktop->getSelection()->add(item("rotated-group"));
    desktop->getSelection()->add(item("bitmap"));
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Mixed rotation setup"}, "");
    DocumentUndo::clearUndo(document.get());
    document->setModifiedSinceSave(false);
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto const pivot = *desktop->getSelection()->center();
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto const child_transform = item("child-vector")->transform;
    std::array<char const *, 5> const ids = {"inside", "rotated-group", "child-vector", "child-text", "bitmap"};
    std::array<Geom::Affine, 5> baseline;
    for (unsigned i = 0; i < ids.size(); ++i) baseline[i] = item(ids[i])->i2dt_affine();
    auto const start = begin_rotation(trans, *desktop, pivot);
    for (double angle : {14.0, 16.0, 29.0}) {
        auto point = start * rotation_about(pivot, angle);
        ASSERT_TRUE(trans.rotateRequest(point, 0));
        trans.commitRelativeAffine();
    }
    EXPECT_NEAR(*trans.rotationAngleDegrees(), 30.0, 1e-9);
    for (unsigned i = 0; i < ids.size(); ++i) {
        EXPECT_TRUE(affine_near(item(ids[i])->i2dt_affine(), baseline[i] * rotation_about(pivot, 30)));
    }
    EXPECT_TRUE(affine_near(item("child-vector")->transform, child_transform));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    trans.ungrab();
    EXPECT_FALSE(trans.rotationOverlay());
    auto const committed = sp_repr_save_buf(document->getReprDoc());
    EXPECT_TRUE(committed.find("selector-rotation-angle") == Glib::ustring::npos);
    EXPECT_TRUE(affine_near(item("child-vector")->transform, child_transform));
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), committed);
    EXPECT_FALSE(DocumentUndo::redo(document.get()));
}

TEST_F(SelectorInteractionTest, RotationEscapeAndFullTurnNoOpPreserveRedo)
{
    item("inside")->getRepr()->setAttribute("x", "42");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Redo sentinel"}, "");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto const pivot = *desktop->getSelection()->center();
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto start = begin_rotation(trans, *desktop, pivot);
    auto point = start * rotation_about(pivot, 14);
    ASSERT_TRUE(trans.rotateRequest(point, 0));
    trans.commitRelativeAffine();
    KeyPressEvent escape;
    escape.keyval = GDK_KEY_Escape;
    GdkKeymapKey *keys = nullptr;
    int key_count = 0;
    ASSERT_TRUE(gdk_display_map_keyval(gdk_display_get_default(), GDK_KEY_Escape, &keys, &key_count));
    ASSERT_GT(key_count, 0);
    escape.keycode = keys[0].keycode;
    escape.group = keys[0].group;
    g_free(keys);
    EXPECT_TRUE(tool->root_handler(escape));
    EXPECT_FALSE(trans.isGrabbed());
    EXPECT_FALSE(trans.rotationOverlay());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);

    // A click/release without a motion event is not a center-edit transaction.
    begin_rotation(trans, *desktop, pivot);
    trans.ungrab();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);

    start = begin_rotation(trans, *desktop, pivot);
    for (int angle = 0; angle <= 360; angle += 30) {
        point = start * rotation_about(pivot, angle);
        ASSERT_TRUE(trans.rotateRequest(point, 0));
        trans.commitRelativeAffine();
    }
    EXPECT_NEAR(*trans.rotationAngleDegrees(), 360, 1e-9);
    trans.ungrab();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "42");
}

TEST_F(SelectorInteractionTest, RotationSelectionDocumentToolAndDesktopChangesCleanUp)
{
    enable_only_dimensional_snapping(*desktop);
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto begin = [&] {
        auto &trans = *tool->_seltrans;
        auto const pivot = *desktop->getSelection()->center();
        auto const start = begin_rotation(trans, *desktop, pivot);
        auto point = start * rotation_about(pivot, 14);
        ASSERT_TRUE(trans.rotateRequest(point, 0));
        trans.commitRelativeAffine();
        ASSERT_TRUE(trans.rotationOverlay());
    };
    begin();
    desktop->getSelection()->set(item("crossing"));
    EXPECT_FALSE(tool->_seltrans->rotationOverlay());
    EXPECT_FALSE(tool->_seltrans->isGrabbed());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);

    begin();
    auto replacement = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50"/>)");
    ASSERT_TRUE(replacement);
    desktop->setDocument(replacement.get());
    EXPECT_FALSE(tool->_seltrans->rotationOverlay());
    EXPECT_FALSE(tool->_seltrans->isGrabbed());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    desktop->setDocument(document.get());
    desktop->getSelection()->set(item("inside"));

    begin();
    desktop->setTool("/tools/nodes"); // Destroys the old SelTrans and its canvas item.
    tool = nullptr;
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    desktop->setTool("/tools/select");
    tool = dynamic_cast<SelectTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    EXPECT_FALSE(tool->_seltrans->rotationOverlay());
    begin();
    desktop.reset();
    tool = nullptr;
    drain_main_context();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, ExternalDoneSealsMovedTokenAndTransfersPreviousPartialOnce)
{
    item("inside")->getRepr()->setAttribute("x", "15"); // Predates the pointer boundary.
    auto original = DocumentUndo::beginRollbackableInteraction(document.get());
    ASSERT_TRUE(original);
    auto moved = std::move(*original);
    EXPECT_FALSE(original->active());
    item("inside")->getRepr()->setAttribute("y", "55");
    {
        DocumentUndo::ScopedInsensitive insensitive(document.get());
        item("inside")->getRepr()->setAttribute("data-insensitive", "retained");
    }
    EXPECT_TRUE(moved.active()); // Low-level commitUndoable must not seal.
    item("inside")->getRepr()->setAttribute("width", "25");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"External action"}, "");
    EXPECT_FALSE(moved.active());
    item("inside")->getRepr()->setAttribute("height", "17");
    moved.rollback();
    EXPECT_STREQ(item("inside")->getRepr()->attribute("height"), "17");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Later action"}, "");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_STREQ(item("inside")->getRepr()->attribute("height"), "10");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "10");
    EXPECT_STREQ(item("inside")->getRepr()->attribute("y"), "10");
    EXPECT_STREQ(item("inside")->getRepr()->attribute("width"), "10");
    EXPECT_STREQ(item("inside")->getRepr()->attribute("data-insensitive"), "retained");
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, NoLogDoneRetiresTokenWithoutConsumingRedoOrLaterXml)
{
    item("inside")->getRepr()->setAttribute("x", "42");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Redo sentinel"}, "");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    auto token = DocumentUndo::beginRollbackableInteraction(document.get());
    ASSERT_TRUE(token);
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"No log"}, "");
    EXPECT_FALSE(token->active());
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    item("inside")->getRepr()->setAttribute("y", "33");
    token.reset();
    EXPECT_STREQ(item("inside")->getRepr()->attribute("y"), "33");
}

TEST_F(SelectorInteractionTest, TokenDestructorWaitsForMutationAndInsensitiveBoundaries)
{
    auto token = DocumentUndo::beginRollbackableInteraction(document.get());
    ASSERT_TRUE(token);
    item("inside")->getRepr()->setAttribute("x", "42");
    {
        XML::Document::MutationScope mutation(*document->getReprDoc());
        token.reset();
        drain_main_context();
        EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "42");
        EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(document.get()));
    }
    {
        DocumentUndo::ScopedInsensitive insensitive(document.get());
        drain_main_context(); // An already queued idle must not spin or roll back here.
        EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "42");
    }
    drain_main_context();
    EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "10");
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, NestedDoneSealsOnceAndDefersCleanupPastAllCommitObservers)
{
    auto token = DocumentUndo::beginRollbackableInteraction(document.get());
    ASSERT_TRUE(token);
    item("inside")->getRepr()->setAttribute("x", "42");
    bool nested = false, cleaned = false;
    auto connection = document->connectBeforeCommit([&] {
        EXPECT_FALSE(token->active());
        EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(document.get()));
        if (nested) return;
        nested = true;
        DocumentUndo::deferInteractionCleanup(document.get(), [&](SPDocument &) { cleaned = true; });
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Nested action"}, "");
        drain_main_context();
        EXPECT_FALSE(cleaned);
    });
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Outer no-log tail"}, "");
    connection.disconnect();
    EXPECT_TRUE(nested);
    EXPECT_FALSE(cleaned);
    drain_main_context();
    EXPECT_TRUE(cleaned);
    token.reset();
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_STREQ(item("inside")->getRepr()->attribute("x"), "10");
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, SynthesisTraversalFencesNestedMainLoopCleanup)
{
    DispatchObserver observer;
    bool entered = false, cleaned = false;
    observer.added = [&] {
        if (entered) return;
        entered = true;
        DocumentUndo::deferInteractionCleanup(document.get(), [&](SPDocument &) { cleaned = true; });
        drain_main_context();
        EXPECT_FALSE(cleaned);
        EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(document.get()));
    };
    document->getReprRoot()->synthesizeEvents(observer);
    EXPECT_TRUE(entered);
    EXPECT_FALSE(cleaned);
    drain_main_context();
    EXPECT_TRUE(cleaned);
}

TEST_F(SelectorInteractionTest, DocumentClosingDisarmsMovedTokensBeforeDestroyObservers)
{
    auto other = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50"/>)");
    ASSERT_TRUE(other);
    other->ensureUpToDate();
    auto token = DocumentUndo::beginRollbackableInteraction(other.get());
    ASSERT_TRUE(token);
    auto moved = std::move(*token);
    auto scope = std::make_unique<XML::Document::MutationScope>(*other->getReprDoc());
    bool destroyed = false, cleaned = false;
    auto connection = other->connectDestroy([&] {
        destroyed = true;
        EXPECT_FALSE(moved.active());
        moved.rollback(); // Must be inert BEFORE arbitrary destroy observers run.
        drain_main_context();
        EXPECT_FALSE(cleaned);
    });
    DocumentUndo::deferInteractionCleanup(other.get(), [&](SPDocument &) { cleaned = true; });
    other.reset();
    EXPECT_TRUE(destroyed);
    scope.reset(); // Fence state outlives the uniquely owned document.
    token.reset();
    drain_main_context();
    EXPECT_FALSE(cleaned);
    connection.disconnect();
}

// This deliberately resets a unique document owner during its own synchronous
// operation, violating the required owner-retention precondition. It is a manual
// unsupported-scenario diagnostic and is excluded from acceptance.
#ifdef INKSCAPE_UNSUPPORTED_OWNER_DIAGNOSTIC
TEST_F(SelectorInteractionTest, DISABLED_InvalidPrivateOwnerResetDuringQueuedRollbackDiagnostic)
{
    // Unlike closing before the idle starts, this closes from an observer
    // invoked by settlement itself. EXPLICIT OPT-IN DIAGNOSTIC, not acceptance:
    // resetting a private unique_ptr during its synchronous member call violates
    // the owner-retention precondition. Use --gtest_also_run_disabled_tests with
    // this exact test filter only when investigating that unsupported scenario.
    auto other = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50"/>)");
    ASSERT_TRUE(other);
    other->ensureUpToDate();
    other->getReprRoot()->setAttribute("data-before", "pending");
    auto token = DocumentUndo::beginRollbackableInteraction(other.get());
    ASSERT_TRUE(token);
    other->getReprRoot()->setAttribute("data-during", "rollback");

    struct CloseObserver final : XML::NodeObserver {
        std::function<void()> close;
        void notifyAttributeChanged(XML::Node &node, GQuark name,
                                    Util::ptr_shared, Util::ptr_shared) override {
            if (name != g_quark_from_static_string("data-during")) return;
            node.removeObserver(*this);
            close();
        }
    } observer;
    bool entered = false, returned = false, later_cleanup = false;
    observer.close = [&] {
        entered = true;
        other.reset();
        EXPECT_FALSE(token->active());
        drain_main_context();
        EXPECT_FALSE(later_cleanup);
    };
    other->getReprRoot()->addObserver(observer);
    DocumentUndo::deferInteractionCleanup(other.get(), [&](SPDocument &) {
        token->rollback();
        returned = true;
    });
    DocumentUndo::deferInteractionCleanup(other.get(), [&](SPDocument &) { later_cleanup = true; });
    drain_main_context();
    if (other) other->getReprRoot()->removeObserver(observer);
    EXPECT_TRUE(entered);
    EXPECT_TRUE(returned);
    EXPECT_FALSE(other);
    EXPECT_FALSE(later_cleanup);
    token.reset();
}
#endif

TEST_F(SelectorInteractionTest, RegisteredCloseDuringQueuedAndSynchronousRollbackRetainsOwner)
{
    auto &app = *initialize_gui();
    app.gio_app()->register_application();
    for (bool queued : {false, true}) {
        SCOPED_TRACE(queued);
        RegisteredTestDocument owned(app);
        owned.document->getReprRoot()->setAttribute("data-before", "pending");
        auto token = DocumentUndo::beginRollbackableInteraction(owned.document);
        ASSERT_TRUE(token);
        owned.document->getReprRoot()->setAttribute("data-during", "rollback");
        struct Observer final : XML::NodeObserver {
            std::function<void()> close;
            void notifyAttributeChanged(XML::Node &node, GQuark name,
                                        Util::ptr_shared, Util::ptr_shared) override {
                if (name != g_quark_from_static_string("data-during")) return;
                node.removeObserver(*this);
                close();
            }
        } observer;
        bool entered = false, later_cleanup = false;
        observer.close = [&] {
            entered = true;
            EXPECT_GT(app.documentOperationCount(), 0u);
            app.document_close(owned.document);
            app.document_close(owned.document);
            EXPECT_TRUE(app.documentClosePending(owned.document));
            EXPECT_TRUE(app.documentHoldActive());
            EXPECT_FALSE(DocumentUndo::beginRollbackableInteraction(owned.document));
            drain_main_context();
            EXPECT_EQ(owned.destroyed, 0u);
        };
        owned.document->getReprRoot()->addObserver(observer);
        if (queued) {
            DocumentUndo::deferInteractionCleanup(owned.document, [&](SPDocument &) { token->rollback(); });
            DocumentUndo::deferInteractionCleanup(owned.document, [&](SPDocument &) { later_cleanup = true; });
            drain_main_context();
        } else {
            token->rollback();
            EXPECT_EQ(owned.destroyed, 0u); // Owner continuation never runs inline.
            drain_main_context();
        }
        if (owned.document) owned.document->getReprRoot()->removeObserver(observer);
        EXPECT_TRUE(entered);
        EXPECT_EQ(owned.destroyed, 1u);
        EXPECT_FALSE(later_cleanup);
        EXPECT_EQ(app.documentOperationCount(), 0u);
        EXPECT_FALSE(app.documentHoldActive());
    }
}

TEST_F(SelectorInteractionTest, RegisteredConfirmedClosePreemptsFreshRollbackAndTokenDestruction)
{
    auto &app = *initialize_gui();
    app.gio_app()->register_application();
    for (bool destroy_token : {false, true}) {
        SCOPED_TRACE(destroy_token);
        RegisteredTestDocument owned(app);
        owned.document->getReprRoot()->setAttribute("data-before", "pending");
        auto token = DocumentUndo::beginRollbackableInteraction(owned.document);
        ASSERT_TRUE(token);
        auto repr = owned.document->getReprRoot();
        repr->setAttribute("data-during", "retain-until-close");
        struct Observer final : XML::NodeObserver {
            unsigned callbacks = 0;
            void notifyAttributeChanged(XML::Node &, GQuark name,
                                        Util::ptr_shared, Util::ptr_shared) override {
                if (name == g_quark_from_static_string("data-during")) ++callbacks;
            }
        } observer;
        repr->addObserver(observer);
        {
            XML::Document::MutationScope mutation(*owned.document->getReprDoc());
            app.document_close(owned.document);
            EXPECT_TRUE(app.documentClosePending(owned.document));
        }
        // The XML stack has unwound, but the owner-close idle has NOT run.
        if (destroy_token) token.reset();
        else token->rollback();
        EXPECT_EQ(observer.callbacks, 0u);
        EXPECT_STREQ(repr->attribute("data-during"), "retain-until-close");
        EXPECT_EQ(owned.destroyed, 0u);
        repr->removeObserver(observer);
        drain_main_context();
        EXPECT_EQ(owned.destroyed, 1u);
        EXPECT_EQ(app.documentOperationCount(), 0u);
        EXPECT_FALSE(app.documentHoldActive());
        token.reset();
    }
}

TEST_F(SelectorInteractionTest, RegisteredCloseFromTaskCaptureDestructionRemainsLeased)
{
    auto &app = *initialize_gui();
    app.gio_app()->register_application();
    RegisteredTestDocument owned(app);
    struct Capture {
        std::function<void()> close;
        ~Capture() { close(); }
    };
    bool released = false, later_cleanup = false;
    auto capture = std::make_shared<Capture>();
    capture->close = [&] {
        released = true;
        EXPECT_GT(app.documentOperationCount(), 0u);
        app.document_close(owned.document);
        drain_main_context();
        EXPECT_EQ(owned.destroyed, 0u);
    };
    DocumentUndo::deferInteractionCleanup(owned.document, [capture](SPDocument &) {});
    DocumentUndo::deferInteractionCleanup(owned.document, [&](SPDocument &) { later_cleanup = true; });
    capture.reset();
    drain_main_context();
    EXPECT_TRUE(released);
    EXPECT_FALSE(later_cleanup);
    EXPECT_EQ(owned.destroyed, 1u);
    EXPECT_EQ(app.documentOperationCount(), 0u);
    EXPECT_FALSE(app.documentHoldActive());
}

TEST_F(SelectorInteractionTest, RegisteredUndoDisabledCloseAndScopedInsensitiveHaveDistinctBoundaries)
{
    auto &app = *initialize_gui();
    app.gio_app()->register_application();
    {
        RegisteredTestDocument owned(app);
        DocumentUndo::setUndoSensitive(owned.document, false);
        app.document_close(owned.document);
        EXPECT_EQ(owned.destroyed, 1u); // No synchronous insensitive scope remains.
        EXPECT_FALSE(app.documentHoldActive());
    }
    {
        RegisteredTestDocument owned(app);
        {
            DocumentUndo::ScopedInsensitive insensitive(owned.document);
            app.document_close(owned.document);
            EXPECT_TRUE(app.documentClosePending(owned.document));
            drain_main_context();
            EXPECT_EQ(owned.destroyed, 0u);
        } // Destructor still needs the retained document to restore sensitivity.
        EXPECT_EQ(owned.destroyed, 0u);
        drain_main_context();
        EXPECT_EQ(owned.destroyed, 1u);
        EXPECT_EQ(app.documentOperationCount(), 0u);
        EXPECT_FALSE(app.documentHoldActive());
    }
}

TEST_F(SelectorInteractionTest, RegisteredWindowCloseDuringDeferredAndNormalCancelRetainsOwner)
{
    auto &app = *initialize_gui();
    app.gio_app()->register_application();
    for (bool deletion : {false, true}) {
        SCOPED_TRACE(deletion);
        RegisteredTestDocument owned(app);
        owned.desktop = app.createDesktop(owned.document, false, true);
        ASSERT_TRUE(owned.desktop);
        auto selected = cast<SPItem>(owned.document->getObjectById("a"));
        auto survivor = cast<SPItem>(owned.document->getObjectById("b"));
        owned.desktop->getSelection()->set(selected);
        owned.desktop->getSelection()->add(survivor);
        selected->setCenter({7, 9}); // Synchronous cancel must cover relocated centers too.
        selected->updateRepr();
        DocumentUndo::done(owned.document, Util::Internal::ContextString{"Setup"}, "");
        enable_only_dimensional_snapping(*owned.desktop);
        auto selector = dynamic_cast<SelectTool *>(owned.desktop->getTool());
        ASSERT_TRUE(selector);
        auto &trans = *selector->_seltrans;
        auto const pivot = *owned.desktop->getSelection()->center();
        auto point = begin_rotation(trans, *owned.desktop, pivot) * rotation_about(pivot, 14);
        ASSERT_TRUE(trans.rotateRequest(point, 0));
        trans.commitRelativeAffine();
        if (deletion) selected->deleteObject(); // Queue first; observe settlement, not release.
        bool entered = false, later_cleanup = false;
        auto connection = owned.desktop->getSelection()->connectChanged([&](Selection *selection) {
            if (entered || !selection->isEmpty()) return;
            entered = true;
            EXPECT_GT(app.documentOperationCount(), 0u);
            auto closing_desktop = std::exchange(owned.desktop, nullptr);
            owned.document->setModifiedSinceSave(false);
            EXPECT_TRUE(app.destroyDesktop(closing_desktop)); // Actual window/desktop close path.
            EXPECT_TRUE(app.documentClosePending(owned.document));
            drain_main_context();
            EXPECT_EQ(owned.destroyed, 0u);
        });
        if (deletion) {
            DocumentUndo::deferInteractionCleanup(owned.document, [&](SPDocument &) { later_cleanup = true; });
        } else {
            EXPECT_TRUE(trans.cancel()); // May destroy SelTrans; never access trans again.
            EXPECT_TRUE(entered); // Ordinary Escape still settles synchronously.
        }
        drain_main_context();
        connection.disconnect();
        EXPECT_TRUE(entered);
        EXPECT_EQ(owned.destroyed, 1u);
        EXPECT_FALSE(later_cleanup);
        EXPECT_EQ(app.documentOperationCount(), 0u);
        EXPECT_FALSE(app.documentHoldActive());
    }
}

TEST_F(SelectorInteractionTest, RegisteredShutdownDoesNotSpinOnLeasedOrViewlessDocuments)
{
    auto &app = *initialize_gui();
    app.gio_app()->register_application();
    RegisteredTestDocument leased(app), viewless(app);
    bool entered = false, later_cleanup = false;
    DocumentUndo::deferInteractionCleanup(leased.document, [&](SPDocument &) {
        entered = true;
        EXPECT_TRUE(app.destroy_all());
        EXPECT_TRUE(app.destroy_all()); // Repeated request is bounded and idempotent.
        EXPECT_EQ(viewless.destroyed, 1u);
        EXPECT_EQ(leased.destroyed, 0u);
        drain_main_context();
        EXPECT_EQ(leased.destroyed, 0u);
    });
    DocumentUndo::deferInteractionCleanup(leased.document, [&](SPDocument &) { later_cleanup = true; });
    drain_main_context();
    EXPECT_TRUE(entered);
    EXPECT_EQ(leased.destroyed, 1u);
    EXPECT_FALSE(later_cleanup);
    EXPECT_EQ(app.documentOperationCount(), 0u);
    EXPECT_FALSE(app.documentHoldActive());
}

TEST_F(SelectorInteractionTest, RegisteredImmediateQuitWaitsForOperationButNotDisabledUndo)
{
    auto &app = *initialize_gui();
    app.gio_app()->register_application();
    RegisteredTestDocument owned(app);
    bool entered = false;
    DocumentUndo::deferInteractionCleanup(owned.document, [&](SPDocument &) {
        entered = true;
        app.on_quit_immediate(); // Batch and shell paths route through this same boundary.
        EXPECT_TRUE(app.quitPending());
        EXPECT_TRUE(app.documentHoldActive());
        drain_main_context();
        EXPECT_TRUE(app.quitPending());
    });
    drain_main_context();
    EXPECT_TRUE(entered);
    EXPECT_FALSE(app.quitPending());
    EXPECT_FALSE(app.documentHoldActive());
    EXPECT_EQ(app.documentOperationCount(), 0u);
    EXPECT_EQ(owned.destroyed, 1u);
    EXPECT_FALSE(owned.document);
    RegisteredTestDocument insensitive(app);
    DocumentUndo::setUndoSensitive(insensitive.document, false);
    app.on_quit_immediate();
    EXPECT_FALSE(app.quitPending());
    EXPECT_FALSE(app.documentHoldActive());
    EXPECT_EQ(insensitive.destroyed, 1u);
    EXPECT_FALSE(insensitive.document);
}

TEST_F(SelectorInteractionTest, RegisteredImmediateQuitUnregistersUnsavedViewsAfterOperation)
{
    auto &app = *initialize_gui();
    auto const original_desktops = INKSCAPE.get_desktops().size();
    RegisteredTestDocument owned(app), viewless(app);
    owned.desktop = app.createDesktop(owned.document, false, true);
    ASSERT_TRUE(owned.desktop);
    ASSERT_TRUE(app.createDesktop(owned.document, false, true));
    owned.document->setModifiedSinceSave();
    viewless.document->setModifiedSinceSave();
    DocumentUndo::setUndoSensitive(viewless.document, false);
    auto raw_document = owned.document;
    bool remained_modified = false;
    auto connection = raw_document->connectDestroy([&] {
        remained_modified = raw_document->isModifiedSinceSave();
    });
    bool entered = false, later_cleanup = false;
    DocumentUndo::deferInteractionCleanup(owned.document, [&](SPDocument &) {
        entered = true;
        app.on_quit_immediate();
        EXPECT_TRUE(app.quitPending());
        EXPECT_TRUE(app.documentHoldActive());
        drain_main_context();
        EXPECT_EQ(owned.destroyed, 0u);
        EXPECT_EQ(INKSCAPE.get_desktops().size(), original_desktops + 2);
    });
    DocumentUndo::deferInteractionCleanup(owned.document, [&](SPDocument &) { later_cleanup = true; });
    drain_main_context();
    connection.disconnect();
    EXPECT_TRUE(entered);
    EXPECT_FALSE(later_cleanup); // Confirmed Quit takes priority after the admitted operation.
    EXPECT_TRUE(remained_modified); // No dirty-bit clearing to suppress a save prompt.
    EXPECT_EQ(owned.destroyed, 1u);
    EXPECT_EQ(viewless.destroyed, 1u);
    EXPECT_EQ(INKSCAPE.get_desktops().size(), original_desktops);
    EXPECT_FALSE(app.quitPending());
    EXPECT_FALSE(app.documentHoldActive());
    EXPECT_EQ(app.documentOperationCount(), 0u);
}

TEST_F(SelectorInteractionTest, RegisteredImmediateQuitGuardsReentrantDesktopAndDocumentClose)
{
    auto &app = *initialize_gui();
    auto const original_desktops = INKSCAPE.get_desktops().size();
    RegisteredTestDocument owned(app);
    owned.desktop = app.createDesktop(owned.document, false, true);
    ASSERT_TRUE(owned.desktop);
    auto raw_desktop = owned.desktop;
    auto raw_document = owned.document;
    raw_desktop->getSelection()->set(cast<SPItem>(raw_document->getObjectById("a")));
    owned.document->setModifiedSinceSave();
    unsigned notifications = 0;
    auto connection = raw_desktop->getSelection()->connectChanged([&](Selection *selection) {
        if (!selection->isEmpty() || notifications) return;
        ++notifications;
        app.desktopClose(raw_desktop); // Same desktop is already being removed.
        app.document_close(raw_document);
        app.on_quit_immediate();
        drain_main_context();
        EXPECT_EQ(owned.destroyed, 0u);
        EXPECT_TRUE(app.documentClosePending(raw_document));
        EXPECT_TRUE(app.documentHoldActive());
    });
    app.on_quit_immediate();
    drain_main_context();
    connection.disconnect();
    EXPECT_EQ(notifications, 1u);
    EXPECT_EQ(owned.destroyed, 1u);
    EXPECT_EQ(INKSCAPE.get_desktops().size(), original_desktops);
    EXPECT_FALSE(app.quitPending());
    EXPECT_FALSE(app.documentHoldActive());
    EXPECT_EQ(app.documentOperationCount(), 0u);
}

TEST_F(SelectorInteractionTest, RegisteredPendingDocumentCloseUnregistersViewsBeforeImmediateQuit)
{
    auto &app = *initialize_gui();
    auto const original_desktops = INKSCAPE.get_desktops().size();
    // The common close path must work independently of Quit, as well as when a
    // pending close is already ahead of Quit's owner continuation.
    for (bool quit : {false, true}) {
        SCOPED_TRACE(quit);
        RegisteredTestDocument owned(app);
        owned.desktop = app.createDesktop(owned.document, false, true);
        ASSERT_TRUE(owned.desktop);
        auto second = app.createDesktop(owned.document, false, true);
        ASSERT_TRUE(second);
        auto raw_document = owned.document;
        owned.document->setModifiedSinceSave();
        unsigned views_destroyed = 0;
        auto on_view_destroy = [&](SPDesktop *) {
            ++views_destroyed;
            EXPECT_GT(app.documentOperationCount(), 0u);
            app.document_close(raw_document); // Reentrant close during admitted teardown.
            drain_main_context();
            EXPECT_EQ(owned.destroyed, 0u);
        };
        auto first_connection = owned.desktop->connectDestroy(on_view_destroy);
        auto second_connection = second->connectDestroy(on_view_destroy);
        bool unregistered_before_document_destroy = false;
        bool remained_modified = false;
        auto document_connection = raw_document->connectDestroy([&] {
            unregistered_before_document_destroy = INKSCAPE.get_desktops().size() == original_desktops;
            remained_modified = raw_document->isModifiedSinceSave();
            EXPECT_EQ(views_destroyed, 2u);
        });
        {
            auto operation = DocumentUndo::holdInteractionOperation(raw_document);
            app.document_close(raw_document);
            app.document_close(raw_document); // One pending close, not duplicate extraction.
            EXPECT_TRUE(app.documentClosePending(raw_document));
            if (quit) {
                app.on_quit_immediate();
                EXPECT_TRUE(app.quitPending());
            }
            drain_main_context();
            EXPECT_EQ(owned.destroyed, 0u);
            EXPECT_EQ(views_destroyed, 0u);
            EXPECT_EQ(INKSCAPE.get_desktops().size(), original_desktops + 2);
            EXPECT_TRUE(app.documentHoldActive());
        }
        drain_main_context();
        first_connection.disconnect();
        second_connection.disconnect();
        document_connection.disconnect();
        EXPECT_EQ(owned.destroyed, 1u);
        EXPECT_FALSE(owned.document);
        EXPECT_EQ(views_destroyed, 2u);
        EXPECT_TRUE(unregistered_before_document_destroy);
        EXPECT_TRUE(remained_modified);
        EXPECT_EQ(INKSCAPE.get_desktops().size(), original_desktops);
        EXPECT_FALSE(app.quitPending());
        EXPECT_FALSE(app.documentHoldActive());
        EXPECT_EQ(app.documentOperationCount(), 0u);
    }
}

TEST_F(SelectorInteractionTest, RegisteredSaveCancelDoesNotLetEarlierCloseQuitApplication)
{
    auto &app = *initialize_gui();
    app.gio_app()->register_application();
    RegisteredTestDocument leased(app), unsaved(app);
    unsaved.desktop = app.createDesktop(unsaved.document, false, true);
    ASSERT_TRUE(unsaved.desktop);
    unsaved.document->setModifiedSinceSave();
    bool cancelled_dialog = false;
    DocumentUndo::deferInteractionCleanup(leased.document, [&](SPDocument &) {
        app.document_close(leased.document); // A previously accepted close must still finish.
        auto response = Glib::signal_idle().connect([&] {
            auto windows = gtk_window_get_toplevels();
            for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
                auto window = g_list_model_get_item(windows, i);
                if (GTK_IS_MESSAGE_DIALOG(window)) {
                    cancelled_dialog = true;
                    gtk_dialog_response(GTK_DIALOG(window), GTK_RESPONSE_CANCEL);
                    g_object_unref(window);
                    return false;
                }
                g_object_unref(window);
            }
            return true;
        });
        app.on_quit();
        response.disconnect();
        EXPECT_TRUE(cancelled_dialog);
        EXPECT_FALSE(app.quitPending());
        EXPECT_EQ(unsaved.destroyed, 0u);
    });
    drain_main_context();
    EXPECT_EQ(leased.destroyed, 1u);
    EXPECT_EQ(unsaved.destroyed, 0u);
    EXPECT_FALSE(app.quitPending());
    EXPECT_FALSE(app.documentHoldActive());
}

TEST_F(SelectorInteractionTest, NoLogDoneImmediatelyBlocksOldGestureAndPreservesLaterEdit)
{
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto const pivot = *desktop->getSelection()->center();
    auto point = begin_rotation(trans, *desktop, pivot) * rotation_about(pivot, 14);
    ASSERT_TRUE(trans.rotateRequest(point, 0));
    trans.commitRelativeAffine();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"No log"}, "");
    EXPECT_FALSE(trans.rotationOverlay());
    EXPECT_FALSE(trans.beginInteraction());
    auto target = item("inside");
    target->set_i2d_affine(Geom::Translate(71, 83));
    target->doWriteTransform(target->transform);
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Later edit"}, "");
    auto const affine = target->i2dt_affine();
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    EXPECT_FALSE(trans.rotateRequest(point, 0));
    trans.transform(Geom::Translate(999, 999), {0, 0});
    trans.ungrab();
    drain_main_context();
    EXPECT_TRUE(affine_near(target->i2dt_affine(), affine));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_FALSE(trans.isGrabbed());
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, ExternalEditBeforeDoneCannotBecomeGesturePreviewProvenance)
{
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto const pivot = *desktop->getSelection()->center();
    auto point = begin_rotation(trans, *desktop, pivot) * rotation_about(pivot, 14);
    ASSERT_TRUE(trans.rotateRequest(point, 0));
    trans.commitRelativeAffine();
    auto target = item("inside");
    target->set_i2d_affine(Geom::Translate(93, 47));
    target->doWriteTransform(target->transform);
    target->setCenter({123, 87});
    target->updateRepr();
    auto const affine = target->i2dt_affine();
    auto const center = target->getCenter();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"External edit"}, "");
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    drain_main_context();
    EXPECT_TRUE(affine_near(target->i2dt_affine(), affine));
    EXPECT_EQ(target->getCenter(), center);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_FALSE(trans.isGrabbed());
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, TransformedParentRetirementWithdrawsOwnedPreviewAndPreservesExternalEdit)
{
    auto group = document->getReprDoc()->createElement("svg:g");
    group->setAttribute("transform", "translate(8.7,3.1) rotate(19) skewX(7) scale(1.3,0.8)");
    document->getReprRoot()->appendChild(group);
    for (auto id : {"inside", "crossing"}) {
        auto repr = item(id)->getRepr();
        Inkscape::GC::anchor(repr);
        repr->parent()->removeChild(repr);
        group->appendChild(repr);
        Inkscape::GC::release(repr);
    }
    Inkscape::GC::release(group);
    document->ensureUpToDate();
    desktop->getSelection()->set(item("inside"));
    desktop->getSelection()->add(item("crossing"));
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Transformed parent setup"}, "");
    DocumentUndo::clearUndo(document.get());
    enable_only_dimensional_snapping(*desktop);
    for (bool external_edit : {false, true}) {
        SCOPED_TRACE(external_edit);
        desktop->getSelection()->set(item("inside"));
        desktop->getSelection()->add(item("crossing"));
        auto &trans = *tool->_seltrans;
        auto expected = item("crossing")->i2dt_affine();
        auto const pivot = *desktop->getSelection()->center();
        auto point = begin_rotation(trans, *desktop, pivot) * rotation_about(pivot, 14);
        ASSERT_TRUE(trans.rotateRequest(point, 0));
        trans.commitRelativeAffine();
        auto survivor = item("crossing");
        if (external_edit) {
            survivor->set_i2d_affine(Geom::Translate(93, 47));
            survivor->doWriteTransform(survivor->transform);
            survivor->setCenter({123, 87});
            survivor->updateRepr();
            expected = survivor->i2dt_affine();
        }
        auto const expected_center = survivor->getCenter();
        item("inside")->deleteObject();
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Delete"}, "");
        auto const xml = sp_repr_save_buf(document->getReprDoc());
        drain_main_context();
        EXPECT_TRUE(affine_near(item("crossing")->i2dt_affine(), expected));
        if (external_edit) EXPECT_EQ(item("crossing")->getCenter(), expected_center);
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        EXPECT_TRUE(item("inside"));
        EXPECT_FALSE(DocumentUndo::undo(document.get()));
    }
}

TEST_F(SelectorInteractionTest, LegitimateDeleteActionRemainsExactlyOneUndo)
{
    enable_only_dimensional_snapping(*desktop);
    auto const baseline = sp_repr_save_buf(document->getReprDoc());
    auto const pivot = *desktop->getSelection()->center();
    auto point = begin_rotation(*tool->_seltrans, *desktop, pivot) * rotation_about(pivot, 14);
    ASSERT_TRUE(tool->_seltrans->rotateRequest(point, 0));
    tool->_seltrans->commitRelativeAffine();
    desktop->getSelection()->deleteItems(); // Includes selection clear, tool reset and done(Delete).
    tool = dynamic_cast<SelectTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    drain_main_context();
    EXPECT_FALSE(item("inside"));
    EXPECT_FALSE(tool->_seltrans->rotationOverlay());
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), baseline);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    EXPECT_FALSE(item("inside"));
    EXPECT_FALSE(DocumentUndo::redo(document.get()));
}

TEST_F(SelectorInteractionTest, ReleaseObserverReentryCannotForceRollbackInNestedLoop)
{
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto const pivot = *desktop->getSelection()->center();
    auto point = begin_rotation(trans, *desktop, pivot) * rotation_about(pivot, 14);
    ASSERT_TRUE(trans.rotateRequest(point, 0));
    trans.commitRelativeAffine();
    bool entered = false;
    auto connection = item("inside")->connectRelease([&](SPObject *) {
        entered = true;
        desktop->getSelection()->set(item("crossing")); // Second synchronous notification.
        EXPECT_TRUE(trans.cancel());
        EXPECT_FALSE(trans.beginInteraction());
        EXPECT_FALSE(trans.rotateRequest(point, 0));
        trans.ungrab();
        drain_main_context();
        EXPECT_TRUE(trans.isGrabbed()); // Consumed cancellation, not falsely completed.
        EXPECT_FALSE(trans.rotationOverlay());
        EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(document.get()));
    });
    item("inside")->deleteObject();
    connection.disconnect();
    EXPECT_TRUE(entered);
    drain_main_context();
    ASSERT_TRUE(item("inside"));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_EQ(desktop->getSelection()->single(), item("inside"));
    EXPECT_FALSE(trans.isGrabbed());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, DoneInsideReleaseObserverPreservesCommittedDelete)
{
    enable_only_dimensional_snapping(*desktop);
    auto &trans = *tool->_seltrans;
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto const pivot = *desktop->getSelection()->center();
    auto point = begin_rotation(trans, *desktop, pivot) * rotation_about(pivot, 14);
    ASSERT_TRUE(trans.rotateRequest(point, 0));
    trans.commitRelativeAffine();
    auto connection = item("inside")->connectRelease([&](SPObject *) {
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Delete"}, "");
        drain_main_context();
        EXPECT_FALSE(trans.rotationOverlay());
        EXPECT_FALSE(trans.beginInteraction());
        EXPECT_TRUE(trans.isGrabbed());
    });
    item("inside")->deleteObject();
    connection.disconnect();
    drain_main_context();
    EXPECT_FALSE(item("inside"));
    EXPECT_FALSE(trans.isGrabbed());
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, RetiredReleaseNeverOverwritesLaterSurvivorAffineOrCenter)
{
    enable_only_dimensional_snapping(*desktop);
    desktop->getSelection()->add(item("crossing"));
    auto &trans = *tool->_seltrans;
    auto const pivot = *desktop->getSelection()->center();
    auto point = begin_rotation(trans, *desktop, pivot) * rotation_about(pivot, 14);
    ASSERT_TRUE(trans.rotateRequest(point, 0));
    trans.commitRelativeAffine();
    item("inside")->deleteObject();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Delete"}, "");
    auto survivor = item("crossing");
    survivor->set_i2d_affine(Geom::Translate(93, 47));
    survivor->doWriteTransform(survivor->transform);
    survivor->setCenter({123, 87});
    survivor->updateRepr();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Later survivor edit"}, "");
    auto const affine = survivor->i2dt_affine();
    auto const center = survivor->getCenter();
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    EXPECT_FALSE(trans.beginInteraction());
    trans.grab({0, 0}, -1, -1, false, true);
    trans.transform(Geom::Translate(999, 999), {0, 0});
    drain_main_context();
    EXPECT_TRUE(affine_near(survivor->i2dt_affine(), affine));
    EXPECT_EQ(survivor->getCenter(), center);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_FALSE(item("inside"));
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_TRUE(item("inside"));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, RetiredReleaseWithdrawsUnchangedSurvivorPreview)
{
    enable_only_dimensional_snapping(*desktop);
    desktop->getSelection()->add(item("crossing"));
    auto &trans = *tool->_seltrans;
    auto const baseline = item("crossing")->i2dt_affine();
    auto const pivot = *desktop->getSelection()->center();
    auto point = begin_rotation(trans, *desktop, pivot) * rotation_about(pivot, 14);
    ASSERT_TRUE(trans.rotateRequest(point, 0));
    trans.commitRelativeAffine();
    item("inside")->deleteObject();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Delete"}, "");
    drain_main_context();
    EXPECT_TRUE(affine_near(item("crossing")->i2dt_affine(), baseline));
    EXPECT_FALSE(item("inside"));
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_TRUE(item("inside"));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST(DocumentOrphanLifecycle, TeardownCollectsAlreadyReleasedQueuedObject)
{
    ASSERT_TRUE(initialize_gui());
    auto doc = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="orphan" width="10" height="10"/></svg>)");
    ASSERT_TRUE(doc);
    auto object = doc->getObjectById("orphan");
    ASSERT_TRUE(object);
    auto unref = [](SPObject *value) { sp_object_unref(value, nullptr); };
    std::unique_ptr<SPObject, decltype(unref)> retained(sp_object_ref(object, nullptr), unref);
    unsigned releases = 0;
    auto connection = object->connectRelease([&](SPObject *) { ++releases; });
    doc->queueForOrphanCollection(object);
    // Root teardown releases the item before processing the queued orphan.
    doc.reset();
    EXPECT_EQ(releases, 1u);
    EXPECT_EQ(retained->document, nullptr);
    EXPECT_EQ(retained->getRepr(), nullptr);
    connection.disconnect();
}

TEST_F(SelectorInteractionTest, PendingReleaseCleanupSurvivesViewDestruction)
{
    enable_only_dimensional_snapping(*desktop);
    desktop->getSelection()->add(item("crossing"));
    auto const baseline = item("crossing")->i2dt_affine();
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto const pivot = *desktop->getSelection()->center();
    auto point = begin_rotation(*tool->_seltrans, *desktop, pivot) * rotation_about(pivot, 14);
    ASSERT_TRUE(tool->_seltrans->rotateRequest(point, 0));
    tool->_seltrans->commitRelativeAffine();
    auto connection = item("inside")->connectRelease([&](SPObject *) {
        desktop.reset();
        tool = nullptr;
        drain_main_context();
        EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(document.get()));
    });
    item("inside")->deleteObject();
    connection.disconnect();
    drain_main_context();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_TRUE(affine_near(item("crossing")->i2dt_affine(), baseline));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, NestedReleaseWithdrawsResourceViewsBeforeDesktopDestruction)
{
    // Exercise both the detached group's notification and a nested notification
    // reached later while the outer group's release() is still on the stack.
    for (auto notify_id : {"outer", "nested"}) {
        SCOPED_TRACE(notify_id);
        auto doc = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <defs>
    <clipPath id="clip"><g id="clip-group"><rect id="clip-leaf" width="40" height="40"/></g></clipPath>
    <mask id="mask" maskUnits="userSpaceOnUse" x="0" y="0" width="40" height="40">
      <g id="mask-group"><rect id="mask-leaf" width="40" height="40" fill="white"/></g>
    </mask>
    <pattern id="paint" patternUnits="userSpaceOnUse" width="5" height="5">
      <rect id="paint-leaf" width="5" height="5" fill="red"/>
    </pattern>
    <marker id="marker" markerWidth="3" markerHeight="3" refX="1" refY="1">
      <path id="marker-leaf" d="M 0,0 L 2,1 L 0,2 z"/>
    </marker>
  </defs>
  <g id="outer"><g id="nested">
    <path id="leaf" d="M 10,10 L 30,10 L 30,30 z" clip-path="url(#clip)" mask="url(#mask)"
          fill="url(#paint)" stroke="black" marker-end="url(#marker)"/>
  </g></g>
  <rect id="survivor" x="60" y="60" width="10" height="10"/>
</svg>)svg");
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        auto first = std::make_unique<SPDesktop>(doc->getNamedView());
        auto second = std::make_unique<SPDesktop>(doc->getNamedView());
        doc->ensureUpToDate();
        auto get = [&](char const *id) { return cast<SPItem>(doc->getObjectById(id)); };
        for (auto id : {"outer", "nested", "leaf", "clip-group", "clip-leaf",
                        "mask-group", "mask-leaf", "paint-leaf", "marker-leaf"}) {
            ASSERT_TRUE(get(id)) << id;
            ASSERT_FALSE(get(id)->views.empty()) << id;
        }
        ASSERT_EQ(get("leaf")->views.size(), 2u);
        ASSERT_EQ(get("survivor")->views.size(), 2u);
        DocumentUndo::done(doc.get(), Util::Internal::ContextString{"Resource view setup"}, "");
        DocumentUndo::clearUndo(doc.get());
        DocumentUndo::clearRedo(doc.get());
        auto const baseline = sp_repr_save_buf(doc->getReprDoc());
        unsigned notifications = 0;
        auto connection = get(notify_id)->connectRelease([&](SPObject *) {
            ++notifications;
            for (auto id : {"outer", "nested", "leaf", "clip-group", "clip-leaf",
                            "mask-group", "mask-leaf", "paint-leaf", "marker-leaf"}) {
                ASSERT_TRUE(get(id)) << id;
                EXPECT_TRUE(get(id)->views.empty()) << id;
            }
            first.reset();
            drain_main_context();
            EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(doc.get()));
            ASSERT_TRUE(get("survivor"));
            EXPECT_EQ(get("survivor")->views.size(), 1u);
        });
        get("outer")->deleteObject();
        connection.disconnect();
        EXPECT_EQ(notifications, 1u);
        EXPECT_FALSE(first);
        EXPECT_FALSE(get("outer"));
        DocumentUndo::done(doc.get(), Util::Internal::ContextString{"Delete"}, "");
        auto const deleted = sp_repr_save_buf(doc->getReprDoc());
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), baseline);
        ASSERT_TRUE(get("leaf"));
        EXPECT_EQ(get("leaf")->views.size(), 1u);
        EXPECT_FALSE(DocumentUndo::undo(doc.get()));
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), deleted);
        EXPECT_FALSE(DocumentUndo::redo(doc.get()));
        second.reset();
        drain_main_context();
    }
}

TEST_F(SelectorInteractionTest, ReleaseWithdrawsSnapshottedDrawingBeforeObserverDisposesIt)
{
    auto const key = SPItem::display_key_new(1);
    // Keep the same owner ordering on assertion-failure exits too.
    auto drawing = std::unique_ptr<Drawing, std::function<void(Drawing *)>>(
        new Drawing(), [&](Drawing *value) {
            if (value->snapshotted()) value->unsnapshot();
            document->getRoot()->invoke_hide(key);
            delete value;
        });
    drawing->setRoot(document->getRoot()->invoke_show(*drawing, key, SP_ITEM_SHOW_DISPLAY));
    ASSERT_TRUE(drawing->root());
    ASSERT_EQ(item("inside")->views.size(), 2u);
    auto const baseline = sp_repr_save_buf(document->getReprDoc());
    auto token = DocumentUndo::beginRollbackableInteraction(document.get());
    ASSERT_TRUE(token);
    bool notified = false;
    drawing->snapshot();
    auto connection = item("inside")->connectRelease([&](SPObject *released) {
        notified = true;
        EXPECT_TRUE(cast<SPItem>(released)->views.empty());
        // A real canvas deactivates its snapshot before destroying its drawing.
        // Explicitly retain this independent owner until its queued unlinking
        // finishes; do not destroy a Drawing with an active renderer snapshot.
        drawing->unsnapshot();
        drawing.reset();
        desktop.reset();
        tool = nullptr;
        drain_main_context();
        EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(document.get()));
    });
    item("inside")->deleteObject();
    connection.disconnect();
    EXPECT_TRUE(notified);
    EXPECT_FALSE(drawing);
    token->rollback();
    token.reset();
    drain_main_context();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), baseline);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, PendingReleaseDocumentSwitchNeverTouchesReplacement)
{
    enable_only_dimensional_snapping(*desktop);
    auto replacement = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50"/>)");
    ASSERT_TRUE(replacement);
    auto const xml = sp_repr_save_buf(document->getReprDoc());
    auto const pivot = *desktop->getSelection()->center();
    auto point = begin_rotation(*tool->_seltrans, *desktop, pivot) * rotation_about(pivot, 14);
    ASSERT_TRUE(tool->_seltrans->rotateRequest(point, 0));
    tool->_seltrans->commitRelativeAffine();
    auto connection = item("inside")->connectRelease([&](SPObject *) {
        desktop->setDocument(replacement.get());
        drain_main_context();
        EXPECT_FALSE(tool->_seltrans->rotationOverlay());
    });
    item("inside")->deleteObject();
    connection.disconnect();
    auto const replacement_xml = sp_repr_save_buf(replacement->getReprDoc());
    drain_main_context();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml);
    EXPECT_EQ(sp_repr_save_buf(replacement->getReprDoc()), replacement_xml);
    EXPECT_TRUE(desktop->getSelection()->isEmpty());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(DocumentUndo::undo(replacement.get()));
    desktop->setDocument(document.get());
}

TEST_F(SelectorInteractionTest, ClosingDocumentDropsPendingViewlessCleanup)
{
    enable_only_dimensional_snapping(*desktop);
    auto const pivot = *desktop->getSelection()->center();
    auto point = begin_rotation(*tool->_seltrans, *desktop, pivot) * rotation_about(pivot, 14);
    ASSERT_TRUE(tool->_seltrans->rotateRequest(point, 0));
    tool->_seltrans->commitRelativeAffine();
    item("inside")->deleteObject();
    desktop.reset();
    tool = nullptr;
    bool cleaned = false;
    DocumentUndo::deferInteractionCleanup(document.get(), [&](SPDocument &) { cleaned = true; });
    document.reset();
    drain_main_context();
    EXPECT_FALSE(cleaned);
}

TEST_F(SelectorInteractionTest, PendingCancelPreservesSelectionMadeInReplacementDocument)
{
    auto replacement = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50"><rect id="new" width="5" height="6"/></svg>)");
    ASSERT_TRUE(replacement);
    replacement->ensureUpToDate();
    auto selected = cast<SPItem>(replacement->getObjectById("new"));
    ASSERT_TRUE(selected);
    auto const old_xml = sp_repr_save_buf(document->getReprDoc());
    auto const new_xml = sp_repr_save_buf(replacement->getReprDoc());
    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    tool->_seltrans->transform(Geom::Translate(30, 7), {0, 0});
    {
        DocumentUndo::ScopedInsensitive insensitive(document.get());
        EXPECT_TRUE(tool->_seltrans->cancel());
        desktop->setDocument(replacement.get());
        desktop->getSelection()->set(selected);
    }
    drain_main_context();
    EXPECT_EQ(desktop->getSelection()->single(), selected);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), old_xml);
    EXPECT_EQ(sp_repr_save_buf(replacement->getReprDoc()), new_xml);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(DocumentUndo::undo(replacement.get()));
    desktop->setDocument(document.get());
}

TEST_F(SelectorInteractionTest, ReplacementCleanupStopsWhenObserverSelectsInThirdDocument)
{
    auto replacement = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50"/>)");
    auto third = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50"><rect id="third" width="5" height="6"/></svg>)");
    ASSERT_TRUE(replacement && third);
    replacement->ensureUpToDate();
    third->ensureUpToDate();
    auto selected = cast<SPItem>(third->getObjectById("third"));
    ASSERT_TRUE(selected);
    desktop->getSelection()->add(item("crossing"));
    auto const old_xml = sp_repr_save_buf(document->getReprDoc());
    auto const new_xml = sp_repr_save_buf(replacement->getReprDoc());
    auto const third_xml = sp_repr_save_buf(third->getReprDoc());
    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    tool->_seltrans->transform(Geom::Translate(30, 7), {0, 0});
    bool switched = false;
    sigc::connection connection;
    {
        DocumentUndo::ScopedInsensitive insensitive(document.get());
        EXPECT_TRUE(tool->_seltrans->cancel());
        connection = desktop->getSelection()->connectChanged([&](Selection *) {
            if (switched || desktop->getDocument() != replacement.get()) return;
            switched = true;
            desktop->setDocument(third.get());
            desktop->getSelection()->set(selected);
        });
        desktop->setDocument(replacement.get());
    }
    drain_main_context();
    connection.disconnect();
    EXPECT_TRUE(switched); // Prove an actual removal observer reentered cleanup.
    EXPECT_EQ(desktop->getDocument(), third.get());
    EXPECT_EQ(desktop->getSelection()->single(), selected);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), old_xml);
    EXPECT_EQ(sp_repr_save_buf(replacement->getReprDoc()), new_xml);
    EXPECT_EQ(sp_repr_save_buf(third->getReprDoc()), third_xml);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(DocumentUndo::undo(replacement.get()));
    EXPECT_FALSE(DocumentUndo::undo(third.get()));
    desktop->setDocument(document.get());
}

TEST_F(SelectorInteractionTest, CenterDragCancellationRestoresUnsetCenter)
{
    ASSERT_FALSE(item("inside")->isCenterSet());
    tool->_seltrans->grab(Geom::Point(15, 15), 0.5, 0.5, false, false);
    tool->_seltrans->setCenter(Geom::Point(70, 60));
    ASSERT_TRUE(item("inside")->isCenterSet());

    ASSERT_TRUE(tool->_seltrans->cancel());

    EXPECT_FALSE(item("inside")->isCenterSet());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, StampIsKeptWhenLaterMotionIsCanceled)
{
    auto const before = sp_repr_save_buf(document->getReprDoc());
    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    tool->_seltrans->transform(Geom::Translate(20, 0), Geom::Point(0, 0));
    tool->_seltrans->stamp();
    auto const stamped = sp_repr_save_buf(document->getReprDoc());
    ASSERT_NE(stamped, before);

    tool->_seltrans->transform(Geom::Translate(35, 5), Geom::Point(0, 0));
    ASSERT_TRUE(tool->_seltrans->cancel());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), stamped);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

// Fast move preview: a picture of the objects follows the pointer; the
// objects are hidden on this canvas and move once, on release.
namespace {
bool shown_on_canvas(SPDesktop &desktop, SPItem *item)
{
    auto arena = item ? item->get_arenaitem(desktop.dkey) : nullptr;
    return arena && arena->visible();
}
} // namespace

TEST_F(SelectorInteractionTest, MovePictureMovesTheObjectsOnceOnRelease)
{
    Preferences::get()->setBool("/tools/select/fast_move_preview", true);
    auto const original = item("inside")->i2dt_affine();
    auto const bounds = *item("inside")->desktopVisualBounds();
    auto const before = sp_repr_save_buf(document->getReprDoc());

    tool->_seltrans->grab(Geom::Point(15, 15), -1, -1, false, true);
    ASSERT_TRUE(tool->_seltrans->isGrabbed());
    ASSERT_TRUE(tool->_seltrans->movesPicture());
    EXPECT_FALSE(shown_on_canvas(*desktop, item("inside"))) << "the picture stands in for the object";
    EXPECT_TRUE(shown_on_canvas(*desktop, item("crossing"))) << "unselected objects stay";
    tool->_seltrans->transform(Geom::Translate(25, 7), Geom::Point(0, 0));
    tool->_seltrans->transform(Geom::Translate(30, 9), Geom::Point(0, 0));
    // Nothing in the document moves while dragging.
    EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(), original));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before.raw());

    tool->_seltrans->ungrab();
    document->ensureUpToDate();
    EXPECT_FALSE(tool->_seltrans->movesPicture());
    EXPECT_TRUE(shown_on_canvas(*desktop, item("inside")));
    auto const moved = *item("inside")->desktopVisualBounds();
    EXPECT_TRUE(Geom::are_near(moved.min(), bounds.min() + Geom::Point(30, 9), 1e-6));
    EXPECT_TRUE(Geom::are_near(moved.max(), bounds.max() + Geom::Point(30, 9), 1e-6));
    EXPECT_EQ(desktop->getSelection()->single(), item("inside"));

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_TRUE(affine_near(item("inside")->i2dt_affine(), original));
    EXPECT_TRUE(Geom::are_near(item("inside")->desktopVisualBounds()->min(), bounds.min(), 1e-6));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before.raw());
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "one Undo step";
}

TEST_F(SelectorInteractionTest, MovePictureEscapeLeavesTheDocumentUntouched)
{
    Preferences::get()->setBool("/tools/select/fast_move_preview", true);
    auto const before = sp_repr_save_buf(document->getReprDoc());

    tool->_seltrans->grab(Geom::Point(15, 15), -1, -1, false, true);
    ASSERT_TRUE(tool->_seltrans->movesPicture());
    tool->_seltrans->transform(Geom::Translate(25, 7), Geom::Point(0, 0));
    ASSERT_TRUE(tool->_seltrans->cancel());
    drain_main_context();
    document->ensureUpToDate();

    EXPECT_FALSE(tool->_seltrans->movesPicture());
    EXPECT_TRUE(shown_on_canvas(*desktop, item("inside")));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before.raw());
    EXPECT_EQ(desktop->getSelection()->single(), item("inside"));
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, MovePictureRendersOnlyTheSelectionWhereItIs)
{
    Preferences::get()->setBool("/tools/select/fast_move_preview", true);
    set_rect("inside", "10", "10", "10", "10", "fill:#ff0000");
    desktop->getSelection()->set(item("inside"));
    tool->_seltrans->grab(Geom::Point(15, 15), -1, -1, false, true);
    ASSERT_TRUE(tool->_seltrans->movesPicture());
    auto const picture = tool->_seltrans->movePicture();
    ASSERT_TRUE(picture);
    auto const surface = picture->surface();
    cairo_surface_flush(surface);
    auto const pixel = [&](Geom::Point const &desktop_point) -> uint32_t {
        auto const p = desktop_point * picture->surfaceToDesktop().inverse();
        int const x = std::floor(p.x()), y = std::floor(p.y());
        if (x < 0 || y < 0 || x >= cairo_image_surface_get_width(surface) ||
            y >= cairo_image_surface_get_height(surface)) {
            return 0xdeadbeef;
        }
        auto const row = cairo_image_surface_get_data(surface) + y * cairo_image_surface_get_stride(surface);
        return reinterpret_cast<uint32_t const *>(row)[x];
    };
    auto const dt = [&](double x, double y) { return desktop->doc2dt(Geom::Point(x, y)); };
    EXPECT_EQ(pixel(dt(15, 15)), 0xffff0000u) << "the selected red square";
    // Where the unselected black square overlaps it, the picture still shows red.
    EXPECT_EQ(pixel(dt(19.5, 19.5)), 0xffff0000u);
    tool->_seltrans->ungrab();
    EXPECT_EQ(tool->_seltrans->movePicture(), nullptr);
}

TEST_F(SelectorInteractionTest, MovePictureIsOnlyForMovesAndUnreferencedObjects)
{
    Preferences::get()->setBool("/tools/select/fast_move_preview", true);
    // Scaling shows the objects themselves.
    tool->_seltrans->grab(Geom::Point(20, 20), 1, 1, false, false, HANDLE_SCALE);
    EXPECT_FALSE(tool->_seltrans->movesPicture());
    ASSERT_TRUE(tool->_seltrans->cancel());
    drain_main_context();

    // An object with a clone elsewhere keeps the live preview (the clone follows).
    {
        DocumentUndo::ScopedInsensitive no_undo(document.get());
        auto use = document->getReprDoc()->createElement("svg:use");
        use->setAttribute("xlink:href", "#inside");
        use->setAttribute("x", "50");
        document->getRoot()->appendChild(use);
        Inkscape::GC::release(use);
    }
    document->ensureUpToDate();
    desktop->getSelection()->set(item("inside"));
    tool->_seltrans->grab(Geom::Point(15, 15), -1, -1, false, true);
    EXPECT_FALSE(tool->_seltrans->movesPicture());
    ASSERT_TRUE(tool->_seltrans->cancel());
    drain_main_context();

    // With the preference off, moves are live.
    Preferences::get()->setBool("/tools/select/fast_move_preview", false);
    desktop->getSelection()->set(item("crossing"));
    tool->_seltrans->grab(Geom::Point(25, 25), -1, -1, false, true);
    EXPECT_FALSE(tool->_seltrans->movesPicture());
    ASSERT_TRUE(tool->_seltrans->cancel());
    drain_main_context();
}

TEST_F(SelectorInteractionTest, MovePictureMovesAGroupAndAnItemInAnotherLayerOnce)
{
    Preferences::get()->setBool("/tools/select/fast_move_preview", true);
    auto *xml = document->getReprDoc();
    {
        DocumentUndo::ScopedInsensitive no_undo(document.get());
        for (auto const *layer_id : {"L1", "L2"}) {
            auto *layer = xml->createElement("svg:g");
            layer->setAttribute("id", layer_id);
            layer->setAttribute("inkscape:groupmode", "layer");
            document->getRoot()->getRepr()->appendChild(layer);
            Inkscape::GC::release(layer);
        }
        auto *group = xml->createElement("svg:g");
        group->setAttribute("id", "grp");
        for (auto const *x : {"60", "75"}) {
            auto *rect = xml->createElement("svg:rect");
            rect->setAttribute("x", x);
            rect->setAttribute("y", "60");
            rect->setAttribute("width", "10");
            rect->setAttribute("height", "10");
            group->appendChild(rect);
            Inkscape::GC::release(rect);
        }
        document->getObjectById("L1")->getRepr()->appendChild(group);
        Inkscape::GC::release(group);
        auto *other = xml->createElement("svg:rect");
        other->setAttribute("id", "other");
        other->setAttribute("x", "60");
        other->setAttribute("y", "80");
        other->setAttribute("width", "10");
        other->setAttribute("height", "10");
        document->getObjectById("L2")->getRepr()->appendChild(other);
        Inkscape::GC::release(other);
    }
    document->ensureUpToDate();
    DocumentUndo::clearUndo(document.get());
    auto const before = sp_repr_save_buf(document->getReprDoc());
    auto const group_bounds = *item("grp")->desktopVisualBounds();
    auto const other_bounds = *item("other")->desktopVisualBounds();
    desktop->getSelection()->setList(std::vector<SPItem *>{item("grp"), item("other")});

    tool->_seltrans->grab(Geom::Point(65, 65), -1, -1, false, true);
    ASSERT_TRUE(tool->_seltrans->movesPicture());
    EXPECT_FALSE(shown_on_canvas(*desktop, item("grp")));
    EXPECT_FALSE(shown_on_canvas(*desktop, item("other")));
    tool->_seltrans->transform(Geom::Translate(-12, 5), Geom::Point(0, 0));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before.raw());
    tool->_seltrans->ungrab();
    document->ensureUpToDate();

    for (auto const &[id, bounds] : {std::pair{"grp", group_bounds}, std::pair{"other", other_bounds}}) {
        SCOPED_TRACE(id);
        EXPECT_TRUE(shown_on_canvas(*desktop, item(id)));
        auto const moved = *item(id)->desktopVisualBounds();
        EXPECT_TRUE(Geom::are_near(moved.min(), bounds.min() + Geom::Point(-12, 5), 1e-6));
    }
    EXPECT_EQ(item("other")->parent, document->getObjectById("L2")) << "each object stays in its layer";
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before.raw());
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "one Undo step";
}

TEST_F(SelectorInteractionTest, MovePictureDuplicateDragCancellationRemovesDuplicate)
{
    Preferences::get()->setBool("/tools/select/fast_move_preview", true);
    auto const before = sp_repr_save_buf(document->getReprDoc());
    ASSERT_TRUE(tool->_seltrans->beginInteraction());
    desktop->getSelection()->duplicate(true);
    document->ensureUpToDate();
    ASSERT_NE(desktop->getSelection()->single(), item("inside"));

    tool->_seltrans->grab(Geom::Point(10, 10), -1, -1, false, true);
    ASSERT_TRUE(tool->_seltrans->movesPicture());
    EXPECT_TRUE(shown_on_canvas(*desktop, item("inside"))) << "the original stays in place";
    tool->_seltrans->transform(Geom::Translate(20, 0), Geom::Point(0, 0));
    ASSERT_TRUE(tool->_seltrans->cancel());
    drain_main_context();
    document->ensureUpToDate();

    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before.raw());
    EXPECT_EQ(desktop->getSelection()->single(), item("inside"));
    EXPECT_TRUE(shown_on_canvas(*desktop, item("inside")));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(SelectorInteractionTest, MovePictureStaysLiveForClonedChildrenAndOtherDisplayModes)
{
    Preferences::get()->setBool("/tools/select/fast_move_preview", true);
    {
        DocumentUndo::ScopedInsensitive no_undo(document.get());
        auto *xml = document->getReprDoc();
        auto *group = xml->createElement("svg:g");
        group->setAttribute("id", "holder");
        auto *rect = xml->createElement("svg:rect");
        rect->setAttribute("id", "cloned-child");
        rect->setAttribute("x", "60");
        rect->setAttribute("y", "60");
        rect->setAttribute("width", "10");
        rect->setAttribute("height", "10");
        group->appendChild(rect);
        Inkscape::GC::release(rect);
        document->getRoot()->getRepr()->appendChild(group);
        Inkscape::GC::release(group);
        auto *use = xml->createElement("svg:use");
        use->setAttribute("xlink:href", "#cloned-child");
        use->setAttribute("x", "20");
        document->getRoot()->getRepr()->appendChild(use);
        Inkscape::GC::release(use);
    }
    document->ensureUpToDate();
    // A clone of a child follows the group live.
    desktop->getSelection()->set(item("holder"));
    tool->_seltrans->grab(Geom::Point(65, 65), -1, -1, false, true);
    EXPECT_FALSE(tool->_seltrans->movesPicture());
    ASSERT_TRUE(tool->_seltrans->cancel());
    drain_main_context();

    // Outline display: the picture would not look like the canvas.
    desktop->getSelection()->set(item("inside"));
    desktop->getCanvas()->set_render_mode(RenderMode::OUTLINE);
    tool->_seltrans->grab(Geom::Point(15, 15), -1, -1, false, true);
    EXPECT_FALSE(tool->_seltrans->movesPicture());
    ASSERT_TRUE(tool->_seltrans->cancel());
    drain_main_context();
    desktop->getCanvas()->set_render_mode(RenderMode::NORMAL);
}
