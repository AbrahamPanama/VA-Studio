// SPDX-License-Identifier: GPL-2.0-or-later
// Full native GTK host tests. Main only: exact app/library build, isolated
// profile/fonts, real display, serial and fatal criticals. No skipped acceptance.
#include <gtest/gtest.h>
#include <algorithm>
#include <csignal>
#include <cmath>
#include <memory>
#include <chrono>
#include <thread>
#include <gtkmm/application.h>
#include <gtkmm/menubutton.h>
#include <gtk/gtk.h>
#include <2geom/elliptical-arc.h>
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape-application.h"
#include "inkscape.h"
#include "inkscape-window.h"
#include "live_effects/lpe-fillet-chamfer.h"
#include "object/sp-path.h"
#include "object/sp-rect.h"
#include "object/sp-root.h"
#include "object/sp-defs.h"
#include "selection.h"
#include "ui/tool/node.h"
#include "ui/tool/path-manipulator.h"
#include "ui/tool/control-point-selection.h"
#include "ui/toolbar/node-toolbar.h"
#include "ui/tools/node-tool.h"
#include "ui/tools/corner-rounding-controller.h"
#include "ui/widget/corner-rounding-popover.h"
#include "ui/widget/desktop-widget.h"
#include "ui/widget/events/canvas-event.h"
#include "xml/repr.h"
#include "xml/node-observer.h"
#include "util/units.h"

using namespace Inkscape;
namespace CE = LivePathEffect::CornerEdit;
using Controller = UI::Tools::CornerRoundingController;
namespace {
void drain()
{
    for (unsigned i = 0; i < 64 && g_main_context_pending(nullptr); ++i)
        g_main_context_iteration(nullptr, false);
}
GtkWidget *named(GtkWidget *widget, char const *name)
{
    if (g_strcmp0(gtk_widget_get_name(widget), name) == 0) return widget;
    for (auto child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (auto result = named(child, name)) return result;
    return nullptr;
}
UI::Widget::CornerRoundingPopover *corners(GtkWidget *widget, bool visible = false)
{
    if (auto pop = dynamic_cast<UI::Widget::CornerRoundingPopover *>(Glib::wrap(widget));
        pop && (!visible || pop->get_visible())) return pop;
    for (auto child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (auto pop = corners(child, visible)) return pop;
    return nullptr;
}
void settle()
{
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    do { drain(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    while (std::chrono::steady_clock::now() < end);
    drain();
}
void snapshot_widget(GtkWidget *widget, std::string const &path)
{
    int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    ASSERT_GT(w, 0); ASSERT_GT(h, 0);
    auto paintable = gtk_widget_paintable_new(widget);
    auto snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), w, h);
    auto node = gtk_snapshot_free_to_node(snapshot);
    g_object_unref(paintable);
    ASSERT_TRUE(node);
    auto native = gtk_widget_get_native(widget); ASSERT_TRUE(native);
    auto renderer = gtk_native_get_renderer(native); ASSERT_TRUE(renderer);
    graphene_rect_t rect = GRAPHENE_RECT_INIT(0, 0, float(w), float(h));
    auto texture = gsk_renderer_render_texture(renderer, node, &rect);
    gsk_render_node_unref(node); ASSERT_TRUE(texture);
    EXPECT_TRUE(gdk_texture_save_to_png(texture, path.c_str()));
    g_object_unref(texture);
}
InkscapeApplication &application()
{
    static auto app = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "cornerhosttest", true);
        auto a = new InkscapeApplication();
        a->gio_app()->register_application();
        for (auto signal : {SIGSEGV, SIGABRT, SIGILL, SIGFPE}) std::signal(signal, SIG_DFL);
        return a; // Existing process-lifetime native fixture convention.
    }();
    return *app;
}
class CornerRoundingHost : public testing::Test {
protected:
    InkscapeApplication *app = nullptr;
    SPDocument *doc = nullptr;
    SPDesktop *desktop = nullptr;
    std::unique_ptr<Controller> controller;
    sigc::scoped_connection destroyed;
    SPPath *path() { return cast<SPPath>(doc->getObjectById("p")); }
    UI::Tools::NodeTool *tool() { return dynamic_cast<UI::Tools::NodeTool *>(desktop->getTool()); }
    LivePathEffect::LPEFilletChamfer *effect() {
        return dynamic_cast<LivePathEffect::LPEFilletChamfer *>(
            path()->getFirstPathEffectOfType(LivePathEffect::FILLET_CHAMFER));
    }
    void select(std::vector<CE::Address> const &addresses) {
        auto nodes = tool()->_selected_nodes;
        nodes->clear();
        for (auto point : nodes->allPoints()) {
            auto node = dynamic_cast<UI::Node *>(point);
            if (!node) continue;
            auto &pm = node->nodeList().subpathList().pm();
            if (pm.item() != path()) continue;
            std::size_t p = 0;
            for (auto const &sub : pm.subpathList()) {
                std::size_t n = 0;
                for (auto const &candidate : *sub) {
                    if (&candidate == node &&
                        std::find(addresses.begin(), addresses.end(), CE::Address{p, n}) != addresses.end())
                        nodes->insert(node);
                    ++n;
                }
                ++p;
            }
        }
    }
    Controller::Result apply(CE::Scope scope, CE::Mode mode, double radius) {
        auto view = controller->inspect(scope);
        EXPECT_TRUE(view.context.snapshot) << view.context.reason;
        return controller->apply({scope, mode, radius}, view.generation);
    }
    void SetUp() override {
        app = &application();
        ASSERT_TRUE(app->gtk_app());
        doc = app->document_add(SPDocument::createNewDocFromMem(R"svg(
          <svg xmlns="http://www.w3.org/2000/svg" width="240" height="240" viewBox="0 0 240 240">
            <g transform="translate(20,30)"><path id="p" fill="#3875a5" fill-rule="evenodd"
              stroke="#123456" stroke-width="2" d="M0,0 H100 V100 H0 Z"/></g>
          </svg>)svg"));
        ASSERT_TRUE(doc);
        destroyed = doc->connectDestroy([this] { doc = nullptr; desktop = nullptr; });
        desktop = app->createDesktop(doc, false, true);
        ASSERT_TRUE(desktop);
        desktop->getSelection()->set(path());
        desktop->setTool("/tools/nodes");
        doc->ensureUpToDate(); drain();
        ASSERT_TRUE(tool()); ASSERT_EQ(SP_ACTIVE_DESKTOP, desktop);
        DocumentUndo::done(doc, Util::Internal::ContextString{"Fixture"}, "");
        DocumentUndo::clearUndo(doc); DocumentUndo::clearRedo(doc);
        doc->setModifiedSinceSave(false);
        controller = std::make_unique<Controller>(*desktop);
    }
    void TearDown() override {
        controller.reset();
        if (doc) {
            doc->setModifiedSinceSave(false);
            if (desktop) app->destroyDesktop(desktop);
            else app->document_close(doc);
        }
        drain();
    }
};

TEST_F(CornerRoundingHost, FreshSelectedRoundInverseNativeArcsIdentityAndOneUndo)
{
    select({{0, 1}});
    auto original = path();
    auto repr = original->getRepr();
    auto transform = original->i2doc_affine();
    std::optional<Geom::Point> midpoint;
    for (auto mode : {CE::Mode::Round, CE::Mode::InverseRound}) {
        ASSERT_EQ(apply(CE::Scope::Selected, mode, 10).outcome, Controller::Outcome::Applied);
        ASSERT_EQ(path(), original); EXPECT_EQ(path()->getRepr(), repr);
        EXPECT_EQ(desktop->getSelection()->singleItem(), original);
        EXPECT_EQ(path()->i2doc_affine(), transform);
        EXPECT_STREQ(repr->attribute("inkscape:original-d"), "M0,0 H100 V100 H0 Z");
        EXPECT_STREQ(repr->attribute("fill"), "#3875a5");
        ASSERT_TRUE(effect());
        EXPECT_EQ(path()->getPathEffects().size(), 1u);
        auto data = effect()->nodesatellites_param.data();
        ASSERT_EQ(data.size(), 1u); ASSERT_EQ(data[0].size(), 4u);
        EXPECT_GT(data[0][1].amount, 0);
        for (auto n : {0, 2, 3}) EXPECT_EQ(data[0][n].amount, 0);
        auto native = effect()->doEffect_path(effect()->pathvector_before_effect);
        unsigned arcs = 0;
        for (auto const &sub : native) for (auto const &curve : sub) {
            if (auto arc = dynamic_cast<Geom::EllipticalArc const *>(&curve)) {
                ++arcs; EXPECT_NEAR(arc->ray(Geom::X), 10, 1e-6);
                EXPECT_NEAR(arc->ray(Geom::Y), 10, 1e-6);
                if (mode == CE::Mode::Round) midpoint = arc->pointAt(.5);
                else { ASSERT_TRUE(midpoint); EXPECT_GT(Geom::distance(*midpoint, arc->pointAt(.5)), 1); }
            }
        }
        EXPECT_EQ(arcs, 1u);
        auto next = controller->inspect(CE::Scope::Selected);
        ASSERT_TRUE(next.context.snapshot);
        EXPECT_EQ(next.context.snapshot->selected, (std::vector<CE::Address>{{0, 1}}));
        auto rounded = std::string(repr->attribute("d"));
        ASSERT_TRUE(DocumentUndo::undo(doc));
        EXPECT_EQ(path(), original); EXPECT_FALSE(path()->hasPathEffect());
        EXPECT_EQ(repr->attribute("inkscape:original-d"), nullptr);
        EXPECT_STREQ(repr->attribute("d"), "M0,0 H100 V100 H0 Z");
        EXPECT_FALSE(DocumentUndo::undo(doc));
        ASSERT_TRUE(DocumentUndo::redo(doc));
        EXPECT_EQ(path()->getRepr()->attribute("d"), rounded);
        EXPECT_FALSE(DocumentUndo::redo(doc));
        ASSERT_TRUE(DocumentUndo::undo(doc));
        DocumentUndo::clearRedo(doc);
        select({{0, 1}});
    }
}

TEST_F(CornerRoundingHost, ExistingEffectUpdatesWithoutDuplicationAndPreservesOtherCorners)
{
    ASSERT_EQ(apply(CE::Scope::All, CE::Mode::Round, 8).outcome, Controller::Outcome::Applied);
    auto resource = effect()->getLPEObj();
    auto before = effect()->nodesatellites_param.data();
    select({{0, 1}});
    ASSERT_EQ(apply(CE::Scope::Selected, CE::Mode::InverseRound, 12).outcome, Controller::Outcome::Applied);
    ASSERT_EQ(effect()->getLPEObj(), resource);
    EXPECT_EQ(path()->getPathEffects().size(), 1u);
    auto after = effect()->nodesatellites_param.data();
    auto untouched = after; untouched[0][1] = before[0][1];
    EXPECT_TRUE(CE::equal(untouched, before));
    ASSERT_TRUE(DocumentUndo::undo(doc)); EXPECT_TRUE(CE::equal(effect()->nodesatellites_param.data(), before));
    ASSERT_TRUE(DocumentUndo::redo(doc)); EXPECT_TRUE(CE::equal(effect()->nodesatellites_param.data(), after));
    ASSERT_EQ(apply(CE::Scope::Selected, CE::Mode::Round, 0).outcome, Controller::Outcome::Applied);
    EXPECT_EQ(effect()->nodesatellites_param.data()[0][1].amount, 0);
}

TEST_F(CornerRoundingHost, OpenEmptySelectedZeroAndInvalidRadiusDoNotEditOrConsumeRedo)
{
    doc->getReprRoot()->setAttribute("data-sentinel", "redo");
    DocumentUndo::done(doc, Util::Internal::ContextString{"Sentinel"}, "");
    ASSERT_TRUE(DocumentUndo::undo(doc)); doc->setModifiedSinceSave(false);
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    for (unsigned i = 0; i < 3; ++i) {
        auto v = controller->inspect(CE::Scope::Selected);
        EXPECT_EQ(v.summary.status, CE::Status::NoCorners);
        EXPECT_EQ(controller->apply({CE::Scope::Selected, CE::Mode::Round, 10}, v.generation).outcome,
                  Controller::Outcome::Rejected);
    }
    EXPECT_EQ(apply(CE::Scope::All, CE::Mode::Round, 0).outcome, Controller::Outcome::NoChange);
    EXPECT_EQ(apply(CE::Scope::All, CE::Mode::Round, -1).outcome, Controller::Outcome::Rejected);
    EXPECT_EQ(apply(CE::Scope::All, CE::Mode::Round, 1000).outcome, Controller::Outcome::Rejected);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::redo(doc));
    EXPECT_STREQ(doc->getReprRoot()->attribute("data-sentinel"), "redo");
}

TEST_F(CornerRoundingHost, OldGenerationAfterNodeOrToolChangeNeverRetargets)
{
    select({{0, 1}});
    auto view = controller->inspect(CE::Scope::Selected);
    select({{0, 2}});
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    EXPECT_EQ(controller->apply({CE::Scope::Selected, CE::Mode::Round, 10}, view.generation).outcome,
              Controller::Outcome::Rejected);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    view = controller->inspect(CE::Scope::All);
    desktop->setTool("/tools/select");
    EXPECT_EQ(controller->apply({CE::Scope::All, CE::Mode::Round, 10}, view.generation).outcome,
              Controller::Outcome::Rejected);
    EXPECT_FALSE(path()->hasPathEffect());
}

TEST_F(CornerRoundingHost, ToolbarPopoverEditsLiveWithoutApplyAndCoalescesTyping)
{
    UI::Toolbar::NodeToolbar toolbar;
    toolbar.setDesktop(desktop);
    auto find = [](auto &&self, GtkWidget *widget) -> GtkMenuButton * {
        if (GTK_IS_MENU_BUTTON(widget)) {
            auto popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(widget));
            if (popover && dynamic_cast<UI::Widget::CornerRoundingPopover *>(Glib::wrap(popover)))
                return GTK_MENU_BUTTON(widget);
        }
        for (auto child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
            if (auto result = self(self, child)) return result;
        return nullptr;
    };
    auto menu = find(find, GTK_WIDGET(toolbar.gobj()));
    ASSERT_TRUE(menu);
    auto popover = dynamic_cast<UI::Widget::CornerRoundingPopover *>(
        Glib::wrap(gtk_menu_button_get_popover(menu)));
    ASSERT_TRUE(popover);
    auto box = dynamic_cast<Gtk::Box *>(popover->get_child());
    ASSERT_TRUE(box);
    auto all = dynamic_cast<Gtk::CheckButton *>(Glib::wrap(named(GTK_WIDGET(box->gobj()), "corner-all")));
    ASSERT_TRUE(all);
    toolbar.setActiveUnit(Util::UnitTable::get().getUnit("px"));
    all->set_active(true); // Actual toolbar scope callback captures the original path.
    auto radius = dynamic_cast<Gtk::Entry *>(Glib::wrap(named(GTK_WIDGET(box->gobj()), "corner-radius")));
    ASSERT_TRUE(radius);
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    radius->set_text("1");
    radius->set_text("10");
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    settle(); // No Apply button, no explicit commit signal.
    ASSERT_TRUE(effect()); EXPECT_EQ(path()->getPathEffects().size(), 1u);
    EXPECT_GT(effect()->nodesatellites_param.data()[0][0].amount, 0);
    ASSERT_TRUE(DocumentUndo::undo(doc)); EXPECT_FALSE(path()->hasPathEffect());
    EXPECT_FALSE(DocumentUndo::undo(doc)); // One edit for the complete number.
    toolbar.setDesktop(nullptr);
}

TEST_F(CornerRoundingHost, LeftToolboxClickRoundsRemovesAndEscapeExits)
{
    auto root = GTK_WIDGET(desktop->getDesktopWidget()->gobj());
    auto button = named(root, "corners-tool-button");
    ASSERT_TRUE(button); ASSERT_TRUE(GTK_IS_TOGGLE_BUTTON(button));
    auto before = sp_repr_save_buf(doc->getReprDoc());
    g_signal_emit_by_name(button, "clicked"); drain();
    ASSERT_TRUE(tool()); ASSERT_TRUE(tool()->corner_mode());
    EXPECT_TRUE(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)));
    auto pop = corners(root, true); ASSERT_TRUE(pop); ASSERT_TRUE(pop->get_visible());
    EXPECT_FALSE(pop->get_autohide());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    auto radius = GTK_ENTRY(named(GTK_WIDGET(pop->gobj()), "corner-radius")); ASSERT_TRUE(radius);
    gtk_editable_set_text(GTK_EDITABLE(radius), "5");
    g_signal_emit_by_name(radius, "activate"); // Set click radius, no node selected.
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    auto find_node = [&]() -> UI::Node * {
        for (auto point : tool()->_selected_nodes->allPoints()) {
            if (auto node = dynamic_cast<UI::Node *>(point);
                node && UI::Tools::corner_rounding_address(*node) == CE::Address{0, 1}) return node;
        }
        return nullptr;
    };
    auto click = [&] {
        auto node = find_node(); ASSERT_TRUE(node);
        UI::ControlPoint *point = node;
        ButtonPressEvent press; press.button = 1;
        EXPECT_TRUE(point->_eventHandler(tool(), press));
        ButtonReleaseEvent release; release.button = 1;
        EXPECT_TRUE(point->_eventHandler(tool(), release));
        drain(); doc->ensureUpToDate();
    };
    click(); ASSERT_TRUE(effect());
    EXPECT_GT(effect()->nodesatellites_param.data()[0][1].amount, 0);
    EXPECT_EQ(effect()->nodesatellites_param.data()[0][0].amount, 0);
    auto rounded = std::string(path()->getRepr()->attribute("d"));
    if (auto directory = std::getenv("VACARDS_CORNERS_SNAPSHOT_DIR")) {
        settle();
        snapshot_widget(root, std::string(directory) + "/corners-window.png");
        snapshot_widget(GTK_WIDGET(pop->gobj()), std::string(directory) + "/corners-controls.png");
        EXPECT_TRUE(sp_repr_save_file(doc->getReprDoc(), (std::string(directory) + "/corners-example.svg").c_str()));
    }
    click();
    EXPECT_EQ(effect()->nodesatellites_param.data()[0][1].amount, 0);
    ASSERT_TRUE(DocumentUndo::undo(doc));
    EXPECT_EQ(path()->getRepr()->attribute("d"), rounded);
    ASSERT_TRUE(DocumentUndo::undo(doc)); EXPECT_FALSE(path()->hasPathEffect());
    ASSERT_TRUE(DocumentUndo::redo(doc)); EXPECT_EQ(path()->getRepr()->attribute("d"), rounded);
    // Shift remains ordinary node selection and must release the native grab.
    auto node = find_node(); ASSERT_TRUE(node);
    UI::ControlPoint *point = node;
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    ButtonPressEvent shift_press; shift_press.button = 1; shift_press.modifiers = GDK_SHIFT_MASK;
    shift_press.pos = desktop->d2w(node->position());
    ButtonReleaseEvent shift_release; shift_release.button = 1; shift_release.modifiers = GDK_SHIFT_MASK;
    shift_release.pos = shift_press.pos;
    point->_eventHandler(tool(), shift_press);
    point->_eventHandler(tool(), shift_release); drain();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    EXPECT_EQ(tool()->pressed_corner, nullptr);
    KeyPressEvent escape; escape.keyval = GDK_KEY_Escape;
    EXPECT_TRUE(tool()->root_handler(escape)); drain();
    EXPECT_FALSE(tool()); EXPECT_FALSE(pop->get_visible());
}

TEST_F(CornerRoundingHost, PendingRadiusIsCancelledBySelectionChangeAndToolExit)
{
    UI::Toolbar::NodeToolbar toolbar;
    toolbar.setDesktop(desktop); toolbar.setActiveUnit(Util::UnitTable::get().getUnit("px"));
    auto pop = corners(GTK_WIDGET(toolbar.gobj())); ASSERT_TRUE(pop);
    auto all = GTK_CHECK_BUTTON(named(GTK_WIDGET(pop->gobj()), "corner-all")); ASSERT_TRUE(all);
    gtk_check_button_set_active(all, true);
    auto radius = GTK_ENTRY(named(GTK_WIDGET(pop->gobj()), "corner-radius")); ASSERT_TRUE(radius);
    auto before = sp_repr_save_buf(doc->getReprDoc());
    gtk_editable_set_text(GTK_EDITABLE(radius), "10");
    desktop->getSelection()->clear(); settle();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    desktop->getSelection()->set(path()); drain();
    gtk_editable_set_text(GTK_EDITABLE(radius), "10");
    toolbar.setDesktop(nullptr); settle();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_FALSE(DocumentUndo::undo(doc));
}

TEST_F(CornerRoundingHost, NativeRectangleAllAndClickedCornerDoNotConvertTheObject)
{
    auto repr = doc->getReprDoc()->createElement("svg:rect");
    for (auto [key, value] : {std::pair{"id", "rectangle"}, {"x", "20"}, {"y", "20"},
        {"width", "100"}, {"height", "80"}, {"fill", "#3875a5"}}) repr->setAttribute(key, value);
    doc->getReprRoot()->appendChild(repr); GC::release(repr);
    auto rect = cast<SPRect>(doc->getObjectById("rectangle")); ASSERT_TRUE(rect);
    desktop->getSelection()->set(rect); doc->ensureUpToDate(); drain();
    DocumentUndo::done(doc, Util::Internal::ContextString{"Fixture rectangle"}, "");
    DocumentUndo::clearUndo(doc); DocumentUndo::clearRedo(doc);
    auto before = sp_repr_save_buf(doc->getReprDoc());
    auto view = controller->inspect(CE::Scope::All);
    ASSERT_TRUE(view.context.snapshot) << view.context.reason;
    EXPECT_EQ(view.summary.count, 4u);
    ASSERT_EQ(controller->apply({CE::Scope::All, CE::Mode::InverseRound, 10}, view.generation).outcome,
              Controller::Outcome::Applied);
    EXPECT_EQ(doc->getObjectById("rectangle"), rect);
    // Inkscape serializes effect-bearing rectangles as svg:path with their
    // native shape tag and dimensions, not as destructively baked paths.
    EXPECT_STREQ(repr->attribute("sodipodi:type"), "rect");
    EXPECT_STREQ(repr->attribute("width"), "100");
    EXPECT_EQ(repr->attribute("inkscape:original-d"), nullptr);
    auto e = dynamic_cast<LivePathEffect::LPEFilletChamfer *>(
        rect->getFirstPathEffectOfType(LivePathEffect::FILLET_CHAMFER)); ASSERT_TRUE(e);
    ASSERT_EQ(e->nodesatellites_param.data()[0].size(), 4u);
    for (auto const &sat : e->nodesatellites_param.data()[0]) {
        EXPECT_GT(sat.amount, 0); EXPECT_EQ(sat.nodesatellite_type, INVERSE_FILLET);
    }
    auto saved = SPDocument::createNewDocFromMem(sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_TRUE(saved);
    EXPECT_TRUE(is<SPRect>(saved->getObjectById("rectangle")));
    ASSERT_TRUE(DocumentUndo::undo(doc)); EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    auto button = named(GTK_WIDGET(desktop->getDesktopWidget()->gobj()), "corners-tool-button"); ASSERT_TRUE(button);
    g_signal_emit_by_name(button, "clicked"); drain();
    ASSERT_TRUE(tool()->corner_mode());
    auto curve = rect->curveForEdit(); ASSERT_TRUE(curve);
    auto pos = desktop->d2w((*curve)[0][1].initialPoint() * rect->i2dt_affine());
    ButtonPressEvent press; press.button = 1; press.pos = pos;
    ButtonReleaseEvent release; release.button = 1; release.pos = pos;
    EXPECT_TRUE(tool()->root_handler(press));
    EXPECT_TRUE(tool()->root_handler(release)); drain();
    e = dynamic_cast<LivePathEffect::LPEFilletChamfer *>(
        rect->getFirstPathEffectOfType(LivePathEffect::FILLET_CHAMFER)); ASSERT_TRUE(e);
    EXPECT_GT(e->nodesatellites_param.data()[0][1].amount, 0);
    EXPECT_EQ(e->nodesatellites_param.data()[0][0].amount, 0);
    EXPECT_EQ(doc->getObjectById("rectangle"), rect);
    auto pop = corners(GTK_WIDGET(desktop->getDesktopWidget()->gobj()), true); ASSERT_TRUE(pop);
    auto radius = GTK_ENTRY(named(GTK_WIDGET(pop->gobj()), "corner-radius")); ASSERT_TRUE(radius);
    auto click_amount = e->nodesatellites_param.data()[0][1].amount;
    gtk_editable_set_text(GTK_EDITABLE(radius), "1");
    g_signal_emit_by_name(radius, "activate");
    EXPECT_NE(e->nodesatellites_param.data()[0][1].amount, click_amount);
    EXPECT_EQ(e->nodesatellites_param.data()[0][0].amount, 0);
    ASSERT_TRUE(DocumentUndo::undo(doc)); // Live edit of the selected rectangle corner.
    ASSERT_TRUE(DocumentUndo::undo(doc)); EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
}

TEST_F(CornerRoundingHost, ReapplySameExistingRadiusIsNoOpAndPreservesRedo)
{
    ASSERT_EQ(apply(CE::Scope::All, CE::Mode::Round, 10).outcome, Controller::Outcome::Applied);
    doc->getReprRoot()->setAttribute("data-sentinel", "redo");
    DocumentUndo::done(doc, Util::Internal::ContextString{"Sentinel"}, "");
    ASSERT_TRUE(DocumentUndo::undo(doc)); doc->setModifiedSinceSave(false);
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    EXPECT_EQ(apply(CE::Scope::All, CE::Mode::Round, 10).outcome, Controller::Outcome::NoChange);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::redo(doc));
    EXPECT_STREQ(doc->getReprRoot()->attribute("data-sentinel"), "redo");
}

TEST_F(CornerRoundingHost, OwnerDestroyedDuringPublicationRollsBackTheFreshEffect)
{
    auto view = controller->inspect(CE::Scope::All);
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    struct Observer final : XML::NodeObserver {
        std::unique_ptr<Controller> &owner;
        unsigned calls = 0;
        explicit Observer(std::unique_ptr<Controller> &value) : owner(value) {}
        void notifyAttributeChanged(XML::Node &node, GQuark key, Util::ptr_shared, Util::ptr_shared) override {
            if (key != g_quark_from_static_string("inkscape:path-effect")) return;
            node.removeObserver(*this); ++calls; owner.reset();
        }
    } observer(controller);
    path()->getRepr()->addObserver(observer);
    auto result = controller->apply({CE::Scope::All, CE::Mode::Round, 10}, view.generation);
    path()->getRepr()->removeObserver(observer);
    EXPECT_EQ(observer.calls, 1u);
    EXPECT_FALSE(controller);
    EXPECT_EQ(result.outcome, Controller::Outcome::Rejected);
    EXPECT_FALSE(path()->hasPathEffect());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    EXPECT_FALSE(DocumentUndo::undo(doc));
}

TEST_F(CornerRoundingHost, NativeParameterReplacementFailsVerificationAndRollsBack)
{
    ASSERT_EQ(apply(CE::Scope::All, CE::Mode::Round, 8).outcome, Controller::Outcome::Applied);
    DocumentUndo::clearUndo(doc);
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    struct Observer final : XML::NodeObserver {
        std::string baseline;
        unsigned calls = 0;
        void notifyAttributeChanged(XML::Node &node, GQuark key, Util::ptr_shared, Util::ptr_shared) override {
            if (key != g_quark_from_static_string("nodesatellites_param")) return;
            node.removeObserver(*this); ++calls;
            node.setAttribute("nodesatellites_param", baseline);
        }
    } observer;
    auto repr = effect()->getRepr();
    observer.baseline = repr->attribute("nodesatellites_param");
    repr->addObserver(observer);
    EXPECT_EQ(apply(CE::Scope::All, CE::Mode::InverseRound, 12).outcome, Controller::Outcome::Rejected);
    repr->removeObserver(observer);
    EXPECT_EQ(observer.calls, 1u);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    EXPECT_FALSE(DocumentUndo::undo(doc));
}

TEST_F(CornerRoundingHost, DeletedTargetOrClosedDesktopCannotBeUsedByOldRequest)
{
    auto view = controller->inspect(CE::Scope::All);
    path()->deleteObject();
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    EXPECT_EQ(controller->apply({CE::Scope::All, CE::Mode::Round, 10}, view.generation).outcome,
              Controller::Outcome::Rejected);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
    doc->setModifiedSinceSave(false);
    ASSERT_TRUE(app->destroyDesktop(desktop)); drain();
    EXPECT_EQ(controller->apply({CE::Scope::All, CE::Mode::Round, 10}, view.generation).outcome,
              Controller::Outcome::Rejected);
}

TEST_F(CornerRoundingHost, SaveBufferReopenRetainsEditableFreshParametersAndOutput)
{
    ASSERT_EQ(apply(CE::Scope::All, CE::Mode::InverseRound, 10).outcome, Controller::Outcome::Applied);
    auto data = effect()->nodesatellites_param.data();
    auto d = std::string(path()->getRepr()->attribute("d"));
    auto buffer = sp_repr_save_buf(doc->getReprDoc());
    auto reopened = SPDocument::createNewDocFromMem(buffer.raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto p = cast<SPPath>(reopened->getObjectById("p")); ASSERT_TRUE(p);
    auto e = dynamic_cast<LivePathEffect::LPEFilletChamfer *>(
        p->getFirstPathEffectOfType(LivePathEffect::FILLET_CHAMFER));
    ASSERT_TRUE(e);
    EXPECT_TRUE(CE::equal(e->nodesatellites_param.data(), data));
    EXPECT_EQ(p->getRepr()->attribute("d"), d);
    EXPECT_STREQ(p->getRepr()->attribute("inkscape:original-d"), "M0,0 H100 V100 H0 Z");
}

TEST_F(CornerRoundingHost, ClearingCompoundNodesPublishesEmptyTopologyBeforeSelectionCallbacks)
{
    path()->getRepr()->setAttribute("d", "M0,0 H100 V100 H0 Z M25,25 V75 H75 V25 Z M120,0 H180 V100");
    doc->ensureUpToDate();
    select({{0, 0}, {1, 0}, {2, 0}});
    ASSERT_FALSE(tool()->_selected_nodes->empty());
    auto node = dynamic_cast<UI::Node *>(*tool()->_selected_nodes->begin());
    ASSERT_TRUE(node);
    auto &manipulator = node->nodeList().subpathList().pm();
    ASSERT_EQ(manipulator.subpathList().size(), 3u);
    auto original = std::string(path()->getRepr()->attribute("d"));
    unsigned notifications = 0;
    sigc::scoped_connection changed = desktop->connect_control_point_selected([&](auto *) {
        ++notifications;
        EXPECT_TRUE(manipulator.subpathList().empty());
    });

    manipulator.clear();

    EXPECT_GT(notifications, 0u);
    EXPECT_TRUE(manipulator.subpathList().empty());
    EXPECT_TRUE(tool()->_selected_nodes->allPoints().empty());
    EXPECT_FALSE(controller->inspect(CE::Scope::All).context.snapshot);
    EXPECT_EQ(std::string(path()->getRepr()->attribute("d")), original);
}

TEST_F(CornerRoundingHost, CompoundPathAllScopePreservesOpenEndpointsAndFillRule)
{
    auto repr = path()->getRepr();
    std::string const original = "M0,0 H100 V100 H0 Z M25,25 V75 H75 V25 Z M120,0 H180 V100";
    repr->setAttribute("d", original);
    doc->ensureUpToDate();
    DocumentUndo::done(doc, Util::Internal::ContextString{"Compound fixture"}, "");
    DocumentUndo::clearUndo(doc);
    ASSERT_EQ(apply(CE::Scope::All, CE::Mode::Round, 5).outcome, Controller::Outcome::Applied);
    ASSERT_TRUE(effect());
    auto const &data = effect()->nodesatellites_param.data();
    ASSERT_EQ(data.size(), 3u);
    EXPECT_EQ(data[0].size(), 4u); EXPECT_EQ(data[1].size(), 4u); ASSERT_EQ(data[2].size(), 3u);
    EXPECT_EQ(data[2].front().amount, 0); EXPECT_EQ(data[2].back().amount, 0);
    EXPECT_GT(data[2][1].amount, 0);
    EXPECT_STREQ(repr->attribute("fill-rule"), "evenodd");
    EXPECT_STREQ(repr->attribute("stroke"), "#123456");
    EXPECT_EQ(repr->attribute("inkscape:original-d"), original);
    ASSERT_TRUE(DocumentUndo::undo(doc)); EXPECT_EQ(repr->attribute("d"), original);
    EXPECT_FALSE(DocumentUndo::undo(doc));
}

TEST_F(CornerRoundingHost, UniformReflectionKeepsTransformAndNonuniformIsExplicitlyRefused)
{
    auto repr = path()->getRepr();
    repr->setAttribute("transform", "translate(200,0) scale(-2,2)");
    doc->ensureUpToDate();
    auto v = controller->inspect(CE::Scope::All);
    ASSERT_TRUE(v.context.snapshot) << v.context.reason;
    ASSERT_TRUE(v.context.input_to_document_scale);
    EXPECT_DOUBLE_EQ(*v.context.input_to_document_scale, 2);
    auto transform = path()->i2doc_affine();
    ASSERT_EQ(controller->apply({CE::Scope::All, CE::Mode::InverseRound, 5}, v.generation).outcome,
              Controller::Outcome::Applied);
    EXPECT_EQ(path()->i2doc_affine(), transform);
    ASSERT_TRUE(DocumentUndo::undo(doc));
    repr->setAttribute("transform", "scale(2,1)");
    doc->ensureUpToDate();
    auto xml = sp_repr_save_buf(doc->getReprDoc());
    v = controller->inspect(CE::Scope::All);
    EXPECT_FALSE(v.context.snapshot); EXPECT_FALSE(v.context.reason.empty());
    EXPECT_EQ(controller->apply({CE::Scope::All, CE::Mode::Round, 5}, v.generation).outcome,
              Controller::Outcome::Rejected);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), xml);
}

// A native rectangle appended to the fixture document, like the command line's
// single native-shape target. The document capture resolves it without a Node
// tool, so these tests exercise apply_corner_plan directly.
SPRect *add_native_rect(SPDocument *doc, char const *id)
{
    auto repr = doc->getReprDoc()->createElement("svg:rect");
    for (auto [key, value] : {std::pair{"id", id}, {"x", "10"}, {"y", "10"},
        {"width", "40"}, {"height", "20"}, {"fill", "#112233"}}) repr->setAttribute(key, value);
    doc->getReprRoot()->appendChild(repr); GC::release(repr);
    return cast<SPRect>(doc->getObjectById(id));
}

TEST_F(CornerRoundingHost, CommandLineApplyRoundsNativeRectAndRecordsOneUndo)
{
    auto *rect = add_native_rect(doc, "cli-rect");
    ASSERT_TRUE(rect);
    desktop->getSelection()->set(rect); doc->ensureUpToDate(); drain();
    DocumentUndo::done(doc, Util::Internal::ContextString{"Fixture rectangle"}, "");
    DocumentUndo::clearUndo(doc); DocumentUndo::clearRedo(doc);

    auto context = UI::Tools::capture_corner_rounding_document(*rect, 0);
    ASSERT_TRUE(context.snapshot) << context.reason;
    ASSERT_TRUE(context.input_to_document_scale);
    auto before = sp_repr_save_buf(doc->getReprDoc());

    auto result = UI::Tools::apply_corner_plan(*rect, nullptr, *context.snapshot,
        CE::Request{CE::Scope::All, CE::Mode::Round, 2.0},
        UI::Tools::CornerCommitProtocol::CommandLine, [] { return true; });
    EXPECT_EQ(result.outcome, Controller::Outcome::Applied);
    EXPECT_TRUE(result.mutation_started);
    EXPECT_EQ(result.count, 4u);
    EXPECT_EQ(doc->getObjectById("cli-rect"), rect);
    // The rect stays a native sodipodi rect and references a fillet_chamfer LPE in defs.
    EXPECT_STREQ(rect->getRepr()->attribute("sodipodi:type"), "rect");
    char const *reference = rect->getRepr()->attribute("inkscape:path-effect");
    ASSERT_TRUE(reference);
    ASSERT_EQ(reference[0], '#');
    auto *effect = doc->getObjectById(reference + 1);
    ASSERT_TRUE(effect);
    EXPECT_EQ(effect->getRepr()->parent(), doc->getDefs()->getRepr());
    EXPECT_STREQ(effect->getRepr()->attribute("effect"), "fillet_chamfer");

    // Exactly one Undo step; nothing remains after it.
    ASSERT_TRUE(DocumentUndo::undo(doc));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_FALSE(rect->hasPathEffect());
    EXPECT_FALSE(DocumentUndo::undo(doc));
}

TEST_F(CornerRoundingHost, CommandLineApplyWorksWhileHoldingInteractionOperationLease)
{
    auto *rect = add_native_rect(doc, "cli-lease-rect");
    ASSERT_TRUE(rect);
    doc->ensureUpToDate();
    // Commit the fixture rectangle and forget it, so the one Undo step below is the
    // corner operation alone and never removes the rectangle itself.
    DocumentUndo::done(doc, Util::Internal::ContextString{"Fixture rectangle"}, "");
    DocumentUndo::clearUndo(doc); DocumentUndo::clearRedo(doc);
    auto context = UI::Tools::capture_corner_rounding_document(*rect, 0);
    ASSERT_TRUE(context.snapshot) << context.reason;

    // The operation lease alone must not block the command-line protocol: it
    // takes no rollbackable interaction token.
    auto lease = DocumentUndo::holdInteractionOperation(doc);
    ASSERT_TRUE(lease);
    auto result = UI::Tools::apply_corner_plan(*rect, nullptr, *context.snapshot,
        CE::Request{CE::Scope::All, CE::Mode::Round, 2.0},
        UI::Tools::CornerCommitProtocol::CommandLine, [] { return true; });
    EXPECT_EQ(result.outcome, Controller::Outcome::Applied);
    EXPECT_TRUE(rect->hasPathEffect());
    lease.reset();
    ASSERT_TRUE(DocumentUndo::undo(doc)); // exactly one step
    // Resolve the rectangle again by id: never read an object across an Undo.
    rect = cast<SPRect>(doc->getObjectById("cli-lease-rect"));
    ASSERT_TRUE(rect);
    EXPECT_FALSE(rect->hasPathEffect());
    EXPECT_FALSE(DocumentUndo::undo(doc)); // only the corner step was recorded
}

TEST_F(CornerRoundingHost, InteractionApplyIsRefusedWhileHoldingInteractionOperationLease)
{
    auto *rect = add_native_rect(doc, "cli-refuse-rect");
    ASSERT_TRUE(rect);
    doc->ensureUpToDate();
    auto context = UI::Tools::capture_corner_rounding_document(*rect, 0);
    ASSERT_TRUE(context.snapshot) << context.reason;
    auto before = sp_repr_save_buf(doc->getReprDoc());

    // A held operation lease makes beginRollbackableInteraction refuse admission,
    // so the live-tool protocol rejects before any mutation.
    auto lease = DocumentUndo::holdInteractionOperation(doc);
    ASSERT_TRUE(lease);
    auto result = UI::Tools::apply_corner_plan(*rect, nullptr, *context.snapshot,
        CE::Request{CE::Scope::All, CE::Mode::Round, 2.0},
        UI::Tools::CornerCommitProtocol::Interaction, [] { return true; });
    EXPECT_EQ(result.outcome, Controller::Outcome::Rejected);
    EXPECT_FALSE(result.mutation_started);
    EXPECT_FALSE(rect->hasPathEffect());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
}

TEST_F(CornerRoundingHost, CommandLineApplyRefusesDuringAnotherInteractionWithoutChange)
{
    auto *rect = add_native_rect(doc, "cli-busy-rect");
    ASSERT_TRUE(rect);
    doc->ensureUpToDate();
    auto context = UI::Tools::capture_corner_rounding_document(*rect, 0);
    ASSERT_TRUE(context.snapshot) << context.reason;
    auto before = sp_repr_save_buf(doc->getReprDoc());
    auto interaction = DocumentUndo::beginRollbackableInteraction(doc);
    ASSERT_TRUE(interaction);
    EXPECT_TRUE(UI::Tools::command_line_corner_refusal(*doc));
    auto result = UI::Tools::apply_corner_plan(*rect, nullptr, *context.snapshot,
        CE::Request{CE::Scope::All, CE::Mode::Round, 2.0},
        UI::Tools::CornerCommitProtocol::CommandLine, [] { return true; });
    EXPECT_EQ(result.outcome, Controller::Outcome::Rejected);
    EXPECT_TRUE(result.refused_by_document);
    EXPECT_FALSE(result.mutation_started);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    interaction->rollback();
}

TEST_F(CornerRoundingHost, CommandLineApplyRefusesWhenUndoRecordingIsOff)
{
    auto *rect = add_native_rect(doc, "cli-nounded-rect");
    ASSERT_TRUE(rect);
    doc->ensureUpToDate();
    auto context = UI::Tools::capture_corner_rounding_document(*rect, 0);
    ASSERT_TRUE(context.snapshot) << context.reason;
    auto before = sp_repr_save_buf(doc->getReprDoc());
    DocumentUndo::ScopedInsensitive off(doc);
    auto result = UI::Tools::apply_corner_plan(*rect, nullptr, *context.snapshot,
        CE::Request{CE::Scope::All, CE::Mode::Round, 2.0},
        UI::Tools::CornerCommitProtocol::CommandLine, [] { return true; });
    EXPECT_EQ(result.outcome, Controller::Outcome::Rejected);
    EXPECT_TRUE(result.refused_by_document);
    EXPECT_FALSE(result.mutation_started);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
}
}
