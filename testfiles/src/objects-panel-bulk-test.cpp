// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <chrono>
#include <csignal>
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <cstdlib>
#include <tuple>
#include <glibmm/main.h>
#include <gtkmm/window.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/treestore.h>
#include "desktop.h"
#include "display/drawing.h"
#include "display/drawing-item.h"
#include "display/drawing-context.h"
#include "document.h"
#include "document-undo.h"
#include "extension/init.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-item.h"
#include "object/sp-item-group.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "selection.h"
#include "selection-chemistry.h"
#include "ui/dialog/objects.h"
#include "ui/widget/canvas.h"
#include "ui/widget/gtk-registry.h"
#include "ui/widget/selected-style.h"
#include "xml/repr.h"
#include "xml/document.h"
#include "layer-manager.h"
#include <gtkmm/searchentry2.h>

namespace Inkscape::UI::Dialog {
struct ObjectsPanelBulkTestAccess {
    static bool drop(ObjectsPanel &p) {
        Glib::Value<Glib::ustring> value; value.init(G_TYPE_STRING); value.set("ObjectsPanelDrag");
        return p.on_drag_drop(value, -10, -10);
    }
    static unsigned long flushes(ObjectsPanel &p) { return p._structural_flushes; }
    static bool rebuilding(ObjectsPanel &p) { return p._rebuilding; }
    static bool pending(ObjectsPanel &p) { return !p._dirty_parents.empty(); }
    static void flushNow(ObjectsPanel &p) { p._structural_idle.disconnect(); p.flushStructuralRefresh(); }
    static void search(ObjectsPanel &p, Glib::ustring const &term) {
        p._searchBox.set_text(term);
        p.setRootWatcher();
    }
};
}
using PanelAccess = Inkscape::UI::Dialog::ObjectsPanelBulkTestAccess;

namespace {
template<typename T> T *find_widget(Gtk::Widget &w) {
    if (auto p = dynamic_cast<T *>(&w)) return p;
    for (auto c = w.get_first_child(); c; c = c->get_next_sibling())
        if (auto p = find_widget<T>(*c)) return p;
    return nullptr;
}
void settle() {
    auto context = Glib::MainContext::get_default();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    auto quiet = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        if (context->pending()) {
            context->iteration(false);
            quiet = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - quiet > std::chrono::milliseconds(50)) {
            return; // Include frame-clock layout/paint, not just the first idle.
        } else {
            g_usleep(1000);
        }
    }
    FAIL() << "GTK did not settle within 20 seconds";
}
std::string native_load() {
#ifdef _WIN32
    return "unavailable";
#else
    double load[3]{};
    if (getloadavg(load, 3) != 3) return "unavailable";
    std::ostringstream out; out << load[0] << "," << load[1] << "," << load[2];
    return out.str();
#endif
}
void settle_native(SPDocument &doc) {
    doc.ensureUpToDate();
    // The canvas window need not paint to qualify settlement: explicitly drain
    // every live root drawing, then GTK/selection publication and a quiet frame.
    for (auto const &view : doc.getRoot()->views) view.drawingitem->drawing().update();
    settle();
}
std::string native_pixels(SPDocument &doc) {
    std::string pixels;
    for (auto const &view : doc.getRoot()->views) {
        auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 256, 192);
        {
            Inkscape::DrawingContext context(surface, Geom::Point(0, 0));
            view.drawingitem->drawing().render(context, Geom::IntRect::from_xywh(0, 0, 256, 192));
        }
        cairo_surface_flush(surface);
        pixels.append(reinterpret_cast<char *>(cairo_image_surface_get_data(surface)),
                      cairo_image_surface_get_stride(surface) * 192);
        cairo_surface_destroy(surface);
    }
    return pixels;
}
Inkscape::XML::Node *repr(Gtk::TreeModel::ConstRow const &row) {
    Inkscape::XML::Node *node = nullptr;
    row.get_value(0, node);
    return node;
}
class ObjectsPanelBulk : public ::testing::TestWithParam<std::tuple<int,int,bool,bool>> {
protected:
    static void SetUpTestSuite() {
        if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
        static auto app = [] {
            Gtk::Application::wrap_in_search_entry2();
            g_setenv("INKSCAPE_APP_ID_TAG", "objectsPanelBulkTest", TRUE);
            auto app = new InkscapeApplication();
            app->gio_app()->register_application();
            for (auto signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) std::signal(signal, SIG_DFL);
#ifndef _WIN32
            std::signal(SIGBUS, SIG_DFL);
#endif
            if (app->gtk_app()) Inkscape::UI::Widget::register_all();
            return app;
        }();
        ASSERT_TRUE(app->gtk_app());
        Inkscape::Extension::init();
    }
    void SetUp() override {
        auto [n, groups, expanded, sources_expanded] = GetParam();
        create(n, groups, expanded, sources_expanded);
    }
    void create(int n, int groups, bool expanded, bool sources_expanded = false, bool rich = false) {
        auto prefs = Inkscape::Preferences::get();
        prefs->setBool("/dialogs/objects/layers_only", false);
        prefs->setBool("/dialogs/objects/expand_to_layer", false);
        std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100"><rect id="outside-before" width="1" height="1"/><g id="destination"><rect id="before" width="1" height="1"/>)";
        if (rich) {
            svg += R"(<defs><clipPath id="group-clip"><rect width="90" height="90"/></clipPath><clipPath id="child-clip"><circle r="4"/></clipPath></defs><metadata>native gate metadata</metadata>)";
        }
        for (int g=0; g<groups; ++g) {
            svg += "<g id=\"g"+std::to_string(g)+"\"" + (rich ? " transform=\"translate(3,7) rotate(12)\" style=\"fill:#123456;stroke:#abcdef;stroke-width:2\" clip-path=\"url(#group-clip)\"" : "") + ">";
            for (int i=g*n/groups; i<(g+1)*n/groups; ++i) {
                auto id = "p" + std::to_string(i);
                if (rich && i % 3 == 0) svg += "<g id=\"" + id + "\" clip-path=\"url(#child-clip)\"><rect width=\"8\" height=\"9\"/></g>";
                else if (rich && i % 3 == 1) svg += "<text id=\"" + id + "\" x=\"3\" y=\"5\">Aa</text>";
                else svg += "<path id=\"" + id + "\" d=\"M0 0h1v1z\"/>";
            }
            svg += "</g>";
        }
        svg += R"(<rect id="after" width="1" height="1"/></g><rect id="outside-after" width="1" height="1"/></svg>)";
        doc = SPDocument::createNewDocFromMem(svg); ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        cast<SPItem>(doc->getObjectById("destination"))->setExpanded(expanded);
        for (int g = 0; g < groups; ++g) {
            cast<SPItem>(doc->getObjectById(("g" + std::to_string(g)).c_str()))->setExpanded(sources_expanded);
        }
        std::vector<std::string> ids;
        for (int g = 0; g < groups; ++g) ids.push_back("g" + std::to_string(g));
        attach(ids, expanded);
    }
    void attach(std::vector<std::string> const &ids, bool expanded = true) {
        desktop = std::make_unique<SPDesktop>(doc->getNamedView());
        canvas = std::make_unique<Gtk::Window>(); canvas->set_child(*desktop->getCanvas());
        Inkscape::Application::instance().add_desktop(desktop.get());
        panel = std::make_unique<Inkscape::UI::Dialog::ObjectsPanel>();
        host = std::make_unique<Gtk::Window>(); host->set_default_size(400, 600); host->set_child(*panel);
        panel->setDesktop(desktop.get()); host->present();
        tree = find_widget<Gtk::TreeView>(*panel); ASSERT_TRUE(tree); ASSERT_TRUE(tree->get_model());
        std::vector<SPItem *> selected;
        for (auto const &id : ids) selected.push_back(cast<SPItem>(doc->getObjectById(id)));
        desktop->getSelection()->setList(selected);
        settle();
        if (!expanded) {
            for (auto const &row : tree->get_model()->children()) {
                if (repr(row) == doc->getObjectById("destination")->getRepr())
                    tree->collapse_row(tree->get_model()->get_path(row.get_iter()));
            }
        }
        settle();
        DocumentUndo::done(doc.get(),Inkscape::Util::Internal::ContextString("Prepare bulk fixture"), "");
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    }
    void TearDown() override {
        if (panel) panel->setDesktop(nullptr);
        if (host) host->unset_child(); panel.reset(); host.reset();
        if (canvas) canvas->unset_child(); canvas.reset();
        if (desktop) Inkscape::Application::instance().remove_desktop(desktop.get());
        desktop.reset(); doc.reset(); settle();
    }
    std::string xml() { return sp_repr_save_buf(doc->getReprDoc()).raw(); }
    void check_order(int n) {
        std::vector<std::string> unrelated_order;
        for (auto const &row : tree->get_model()->children()) {
            auto node = repr(row); ASSERT_TRUE(node);
            std::string id = node->attribute("id");
            if (id == "outside-after" || id == "destination" || id == "outside-before") unrelated_order.push_back(id);
        }
        EXPECT_EQ(unrelated_order, (std::vector<std::string>{"outside-after", "destination", "outside-before"}));
        auto dest = doc->getObjectById("destination");
        std::vector<std::string> expected{"before"};
        for (int i=0;i<n;++i) expected.push_back("p"+std::to_string(i));
        expected.push_back("after");
        std::vector<std::string> actual;
        for (auto &child : dest->children) if (is<SPItem>(&child)) actual.emplace_back(child.getId());
        ASSERT_EQ(actual, expected);
        for (auto const &row : tree->get_model()->children()) {
            if (repr(row) == dest->getRepr()) {
                actual.clear();
                for (auto const &child : row.children()) {
                    auto node=repr(child); ASSERT_TRUE(node);
                    actual.emplace_back(node->attribute("id"));
                }
                std::reverse(expected.begin(), expected.end());
                ASSERT_EQ(actual, expected); return;
            }
        }
        FAIL() << "destination absent from attached model";
    }
    std::unique_ptr<SPDocument> doc;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<Gtk::Window> canvas, host;
    std::unique_ptr<Inkscape::UI::Dialog::ObjectsPanel> panel;
    Gtk::TreeView *tree=nullptr;
};
TEST_P(ObjectsPanelBulk, UngroupOrderSelectionUndoRedo) {
    auto [n,groups,expanded,sources_expanded] = GetParam();
    auto before=xml(); int reorders=0;
    auto flushes=PanelAccess::flushes(*panel);
    auto layer=desktop->layerManager().currentLayer();
    auto connection=tree->get_model()->signal_rows_reordered().connect([&](auto const &,auto const &,int *){++reorders;});
    std::cout << "BULK_START n=" << n << " groups=" << groups << " expanded=" << expanded << std::endl;
    auto start=std::chrono::steady_clock::now();
    desktop->getSelection()->ungroup();
    auto native_end = std::chrono::steady_clock::now();
    if (g_getenv("BUG026_PROFILE_FLUSH")) {
        PanelAccess::flushNow(*panel);
        std::cout << "DIAGNOSTIC flush_seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - native_end).count() << std::endl;
    }
    settle();
    double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"BULK n="<<n<<" groups="<<groups<<" expanded="<<expanded<<" sources_expanded="<<sources_expanded<<" seconds="<<seconds<<" native_seconds="<<std::chrono::duration<double>(native_end-start).count()<<" reorders="<<reorders<<std::endl;
    EXPECT_LT(seconds,10.0); EXPECT_LE(reorders,groups+1);
    EXPECT_EQ(PanelAccess::flushes(*panel)-flushes,1);
    EXPECT_FALSE(PanelAccess::pending(*panel));
    EXPECT_EQ(desktop->layerManager().currentLayer(),layer);
    EXPECT_EQ(cast<SPItem>(doc->getObjectById("destination"))->isExpanded(),expanded);
    check_order(n);
    EXPECT_EQ(desktop->getSelection()->size(),n);
    for(int i=0;i<n;++i) EXPECT_TRUE(desktop->getSelection()->includes(cast<SPItem>(doc->getObjectById(("p"+std::to_string(i)).c_str()))));
    auto after=xml();
    EXPECT_TRUE(DocumentUndo::undo(doc.get())); settle(); EXPECT_EQ(xml(),before);
    EXPECT_TRUE(DocumentUndo::redo(doc.get())); settle(); EXPECT_EQ(xml(),after); check_order(n);
    EXPECT_EQ(PanelAccess::flushes(*panel)-flushes,3);
    EXPECT_LE(reorders,3*(groups+1));
    connection.disconnect();
}
class ObjectsPanelSafety : public ObjectsPanelBulk {
    void SetUp() override { create(30, 1, true); }
};
TEST_F(ObjectsPanelSafety, DeleteDirtyAncestorBeforeIdle) {
    desktop->getSelection()->ungroup();
    ASSERT_TRUE(PanelAccess::pending(*panel));
    doc->getObjectById("destination")->deleteObject();
    settle();
    EXPECT_FALSE(PanelAccess::pending(*panel));
    std::vector<std::string> ids;
    for (auto const &row : tree->get_model()->children()) {
        ASSERT_TRUE(repr(row)); ids.emplace_back(repr(row)->attribute("id"));
    }
    EXPECT_EQ(ids,(std::vector<std::string>{"outside-after","outside-before"}));
}
TEST_F(ObjectsPanelSafety, CloseDocumentBeforeIdle) {
    desktop->getSelection()->ungroup();
    ASSERT_TRUE(PanelAccess::pending(*panel));
    // Exercise the desktop destruction signal with the panel still bound.
    canvas->unset_child();
    Inkscape::Application::instance().remove_desktop(desktop.get());
    desktop.reset(); doc.reset();
    settle();
    EXPECT_FALSE(PanelAccess::pending(*panel));
    EXPECT_TRUE(tree->get_model()->children().empty());
}
TEST_F(ObjectsPanelSafety, SwitchDocumentBeforeIdle) {
    auto other = SPDocument::createNewDocFromMem(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="replacement" width="2" height="3"/></svg>)");
    auto other_desktop = std::make_unique<SPDesktop>(other->getNamedView());
    Gtk::Window other_canvas; other_canvas.set_child(*other_desktop->getCanvas());
    desktop->getSelection()->ungroup();
    ASSERT_TRUE(PanelAccess::pending(*panel));
    panel->setDesktop(other_desktop.get());
    settle();
    EXPECT_FALSE(PanelAccess::pending(*panel));
    auto rows=tree->get_model()->children(); ASSERT_EQ(rows.size(),1);
    EXPECT_EQ(repr(*rows.begin()),other->getObjectById("replacement")->getRepr());
    panel->setDesktop(nullptr); other_canvas.unset_child();
}
TEST_F(ObjectsPanelSafety, SearchSurvivesStructuralRefresh) {
    PanelAccess::search(*panel,"p1"); settle();
    desktop->getSelection()->ungroup(); settle();
    auto rows=tree->get_model()->children(); ASSERT_EQ(rows.size(),1);
    auto destination=*rows.begin();
    EXPECT_STREQ(repr(destination)->attribute("id"),"destination");
    std::vector<std::string> actual;
    for (auto const &row : destination.children()) actual.emplace_back(repr(row)->attribute("id"));
    EXPECT_EQ(actual,(std::vector<std::string>{"p19","p18","p17","p16","p15","p14","p13","p12","p11","p10","p1"}));
}
TEST_F(ObjectsPanelSafety, SingleEditRefreshesOnNextIdle) {
    auto node=doc->getReprDoc()->createElement("svg:rect");
    node->setAttribute("id","single");
    auto start=PanelAccess::flushes(*panel);
    doc->getRoot()->getRepr()->appendChild(node); Inkscape::GC::release(node);
    EXPECT_TRUE(PanelAccess::pending(*panel)); settle();
    EXPECT_EQ(PanelAccess::flushes(*panel)-start,1);
    EXPECT_STREQ(repr(*tree->get_model()->children().begin())->attribute("id"),"single");
}
TEST_F(ObjectsPanelSafety, LayersOnlyFilterAndCurrentLayerSurvive) {
    auto dest=doc->getObjectById("destination");
    dest->getRepr()->setAttribute("inkscape:groupmode","layer");
    desktop->layerManager().setCurrentLayer(dest);
    Inkscape::Preferences::get()->setBool("/dialogs/objects/layers_only",true);
    settle();
    desktop->getSelection()->ungroup(); settle();
    EXPECT_EQ(desktop->layerManager().currentLayer(),dest);
    auto rows=tree->get_model()->children(); ASSERT_EQ(rows.size(),1);
    EXPECT_EQ(repr(*rows.begin()),dest->getRepr());
    EXPECT_TRUE(rows.begin()->children().empty());
    EXPECT_EQ(desktop->getSelection()->size(),30);
}
TEST_F(ObjectsPanelSafety, ExpandedRecreatedGroupAndHighlightSurvive) {
    auto nested=doc->getReprDoc()->createElement("svg:g");
    nested->setAttribute("id","nested");
    nested->setAttribute("inkscape:highlight-color","#ff0000");
    auto child=doc->getReprDoc()->createElement("svg:rect");
    child->setAttribute("id","nested-child"); child->setAttribute("width","1"); child->setAttribute("height","1");
    nested->appendChild(child); Inkscape::GC::release(child);
    doc->getObjectById("g0")->getRepr()->appendChild(nested); Inkscape::GC::release(nested);
    settle(); tree->expand_all(); settle();
    auto item=cast<SPItem>(doc->getObjectById("nested"));
    ASSERT_TRUE(item->isExpanded()); auto color=item->highlight_color().toRGBA();
    ASSERT_EQ(color, 0xff0000ffu);
    desktop->getSelection()->ungroup(); settle();
    item=cast<SPItem>(doc->getObjectById("nested")); ASSERT_TRUE(item);
    EXPECT_TRUE(item->isExpanded()); EXPECT_EQ(item->highlight_color().toRGBA(),color);
    bool found=false;
    for (auto const &row : tree->get_model()->children()) {
        if (repr(row) != doc->getObjectById("destination")->getRepr()) continue;
        for (auto const &nested_row : row.children()) {
            if (repr(nested_row) != item->getRepr()) continue;
            found=true;
            EXPECT_TRUE(tree->row_expanded(tree->get_model()->get_path(nested_row.get_iter())));
            unsigned int model_color=0; nested_row.get_value(3,model_color);
            EXPECT_EQ(model_color,color);
        }
    }
    EXPECT_TRUE(found);
}
TEST_F(ObjectsPanelSafety, TwoDisjointParentsFlushOnceEach) {
    auto parent=doc->getReprDoc()->createElement("svg:g"); parent->setAttribute("id","second-destination");
    auto group=doc->getReprDoc()->createElement("svg:g"); group->setAttribute("id","second-source");
    auto child=doc->getReprDoc()->createElement("svg:rect"); child->setAttribute("id","second-child");
    group->appendChild(child); parent->appendChild(group); doc->getRoot()->getRepr()->appendChild(parent);
    Inkscape::GC::release(child); Inkscape::GC::release(group); Inkscape::GC::release(parent);
    settle(); tree->expand_all(); settle();
    desktop->getSelection()->setList(std::vector<SPItem *>{cast<SPItem>(doc->getObjectById("g0")),cast<SPItem>(doc->getObjectById("second-source"))});
    settle(); auto flushes=PanelAccess::flushes(*panel);
    desktop->getSelection()->ungroup(); settle();
    EXPECT_EQ(PanelAccess::flushes(*panel)-flushes,2);
    EXPECT_EQ(desktop->getSelection()->size(),31);
    EXPECT_EQ(doc->getObjectById("second-child")->parent,doc->getObjectById("second-destination"));
    check_order(30);
}
TEST_F(ObjectsPanelSafety, ScrollAnchorSurvivesRecreatedChildren) {
    tree->expand_all(); settle();
    tree->set_cursor(Gtk::TreeModel::Path("2")); settle();
    auto scroller=find_widget<Gtk::ScrolledWindow>(*panel); ASSERT_TRUE(scroller);
    scroller->get_vadjustment()->set_value(250); settle();
    Gtk::TreeModel::Path first,last;
    ASSERT_TRUE(tree->get_visible_range(first,last));
    auto node=repr(*tree->get_model()->get_iter(first)); ASSERT_TRUE(node);
    std::string id=node->attribute("id"); ASSERT_TRUE(id.starts_with("p"));
    Gdk::Rectangle before; tree->get_background_area(first,*tree->get_column(0),before);
    desktop->getSelection()->ungroup(); settle();
    ASSERT_TRUE(tree->get_visible_range(first,last));
    node=repr(*tree->get_model()->get_iter(first)); ASSERT_TRUE(node);
    EXPECT_EQ(std::string(node->attribute("id")),id);
    Gdk::Rectangle after; tree->get_background_area(first,*tree->get_column(0),after);
    EXPECT_NEAR(after.get_y(),before.get_y(),1);
    Gtk::TreeModel::Path cursor; Gtk::TreeViewColumn *column = nullptr;
    tree->get_cursor(cursor, column); ASSERT_TRUE(cursor);
    EXPECT_STREQ(repr(*tree->get_model()->get_iter(cursor))->attribute("id"), "outside-before");
}
TEST_F(ObjectsPanelSafety, TrackingDoesNotExpandCollapsedDestinationDuringRefresh) {
    Inkscape::Preferences::get()->setBool("/dialogs/objects/expand_to_layer", true);
    tree->collapse_all(); settle();
    desktop->getSelection()->ungroup(); settle();
    EXPECT_FALSE(cast<SPItem>(doc->getObjectById("destination"))->isExpanded());
    check_order(30);
    desktop->getSelection()->set(cast<SPItem>(doc->getObjectById("outside-after")));
    settle(); // Root-level selection has no ancestor row to expand.
    EXPECT_EQ(desktop->getSelection()->size(), 1);
}
TEST_F(ObjectsPanelSafety, UnrelatedTransformsAndStylesSurvive) {
    std::vector<std::string> ids{"outside-before", "before", "after", "outside-after"};
    for (auto const &id : ids) {
        auto node = doc->getObjectById(id)->getRepr();
        node->setAttribute("transform", "translate(3,7)");
        node->setAttribute("style", "fill:#123456;stroke:#abcdef;stroke-width:2");
    }
    settle(); desktop->getSelection()->ungroup(); settle();
    for (auto const &id : ids) {
        auto item = doc->getObjectById(id); ASSERT_TRUE(item);
        EXPECT_STREQ(item->getRepr()->attribute("transform"), "translate(3,7)");
        EXPECT_STREQ(item->getRepr()->attribute("style"), "fill:#123456;stroke:#abcdef;stroke-width:2");
    }
    check_order(30);
}
TEST_F(ObjectsPanelSafety, SelectionHighlightsMatchGroupMemberAndClearWithoutXmlWrites) {
    tree->expand_all(); settle();
    auto before = xml();
    auto alpha = [&](char const *id) {
        double result = -1;
        auto visit = [&](auto &&self, auto const &rows) -> void {
            for (auto const &row : rows) {
                if (auto node = repr(row); node && node->attribute("id") && std::string(node->attribute("id")) == id) {
                    Gdk::RGBA color; row.get_value(5, color); result = color.get_alpha();
                }
                self(self, row.children());
            }
        };
        visit(visit, tree->get_model()->children());
        EXPECT_GE(result, 0); return result;
    };
    auto inherited = alpha("p0");
    EXPECT_GT(inherited, 0); EXPECT_GT(alpha("g0"), inherited);
    desktop->getSelection()->set(cast<SPItem>(doc->getObjectById("p0"))); settle();
    EXPECT_GT(alpha("p0"), inherited); EXPECT_EQ(alpha("g0"), 0); EXPECT_EQ(alpha("p1"), 0);
    desktop->getSelection()->clear(); settle();
    EXPECT_EQ(alpha("p0"), 0); EXPECT_EQ(alpha("g0"), 0);
    EXPECT_EQ(xml(), before);
}
TEST_F(ObjectsPanelSafety, VariableHeightLabelsKeepFullLayoutAndRecoverFastMode) {
    ASSERT_TRUE(tree->get_fixed_height_mode());
    auto node = doc->getObjectById("outside-after")->getRepr();
    node->setAttribute("inkscape:label", "first line\nsecond line"); settle();
    EXPECT_FALSE(tree->get_fixed_height_mode());
    auto row = *tree->get_model()->children().begin();
    Glib::ustring label; row.get_value(1, label); EXPECT_EQ(label, "first line\nsecond line");
    Gdk::Rectangle multi, single;
    tree->get_background_area(Gtk::TreeModel::Path("0"), *tree->get_column(0), multi);
    tree->get_background_area(Gtk::TreeModel::Path("2"), *tree->get_column(0), single);
    EXPECT_GT(multi.get_height(), single.get_height());
    node->setAttribute("inkscape:label", "Unicode é"); settle();
    EXPECT_FALSE(tree->get_fixed_height_mode());
    node->setAttribute("inkscape:label", "single line"); settle();
    EXPECT_TRUE(tree->get_fixed_height_mode());
    desktop->getSelection()->ungroup(); settle(); check_order(30);
}

// Round 2: drive real GTK model callbacks, not a synthetic flush hook.
class ObjectsPanelReentrant : public ObjectsPanelSafety {
protected:
    bool contains(char const *id, Gtk::TreeView *view = nullptr) {
        bool found = false;
        auto visit = [&](auto &&self, auto const &rows) -> void {
            for (auto const &row : rows) {
                auto node = repr(row);
                if (node && node->attribute("id") && std::string(node->attribute("id")) == id) found = true;
                self(self, row.children());
            }
        };
        visit(visit, (view ? view : tree)->get_model()->children());
        return found;
    }
    void add(char const *parent, char const *id, bool layer = false) {
        auto node = doc->getReprDoc()->createElement(layer ? "svg:g" : "svg:rect");
        node->setAttribute("id", id);
        if (layer) node->setAttribute("inkscape:groupmode", "layer");
        doc->getObjectById(parent)->getRepr()->appendChild(node);
        Inkscape::GC::release(node);
    }
};
TEST_F(ObjectsPanelReentrant, MutationDuringRowDeletionReschedules) {
    bool fired = false;
    auto connection = tree->get_model()->signal_row_deleted().connect([&](auto const &) {
        if (fired) return;
        fired = true;
        add("destination", "during-flush");
        // A modal loop must not recursively flush the half-built model.
        auto loop = Glib::MainLoop::create();
        Glib::signal_timeout().connect_once([&] { loop->quit(); }, 10);
        loop->run();
    });
    desktop->getSelection()->ungroup(); settle(); connection.disconnect();
    ASSERT_TRUE(fired);
    EXPECT_TRUE(contains("during-flush"));
    EXPECT_FALSE(PanelAccess::pending(*panel));
    EXPECT_TRUE(tree->get_sensitive());
    EXPECT_EQ(desktop->getSelection()->size(), 30);
}
TEST_F(ObjectsPanelReentrant, SwapDuringRowInsertion) {
    auto other = SPDocument::createNewDocFromMem(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="replacement" width="2" height="3"/></svg>)");
    auto other_desktop = std::make_unique<SPDesktop>(other->getNamedView());
    Gtk::Window other_canvas; other_canvas.set_child(*other_desktop->getCanvas());
    bool fired = false;
    auto connection = tree->get_model()->signal_row_inserted().connect([&](auto const &, auto const &) {
        if (fired) return;
        fired = true;
        panel->setDesktop(other_desktop.get());
    });
    desktop->getSelection()->ungroup(); settle(); connection.disconnect();
    ASSERT_TRUE(fired);
    EXPECT_TRUE(contains("replacement")); EXPECT_FALSE(contains("destination"));
    EXPECT_EQ(tree->get_model()->children().size(), 1);
    EXPECT_FALSE(PanelAccess::pending(*panel));
    panel->setDesktop(nullptr); other_canvas.unset_child();
}
TEST_F(ObjectsPanelReentrant, CloseDuringRowChange) {
    bool fired = false;
    auto connection = tree->get_model()->signal_row_changed().connect([&](auto const &, auto const &) {
        if (fired || !PanelAccess::rebuilding(*panel)) return;
        fired = true;
        canvas->unset_child();
        Inkscape::Application::instance().remove_desktop(desktop.get());
        desktop.reset(); doc.reset();
    });
    desktop->getSelection()->ungroup(); settle(); connection.disconnect();
    ASSERT_TRUE(fired);
    EXPECT_TRUE(tree->get_model()->children().empty());
    EXPECT_FALSE(PanelAccess::pending(*panel));
}
TEST_F(ObjectsPanelReentrant, NestedMutationScopesHoldRefreshAcrossModalLoop) {
    auto flushes = PanelAccess::flushes(*panel);
    {
        Inkscape::XML::Document::MutationScope outer(*doc->getReprDoc());
        desktop->getSelection()->ungroup();
        {
            Inkscape::XML::Document::MutationScope inner(*doc->getReprDoc());
            auto loop = Glib::MainLoop::create();
            Glib::signal_timeout().connect_once([&] { loop->quit(); }, 20);
            loop->run();
            EXPECT_EQ(PanelAccess::flushes(*panel), flushes);
            EXPECT_FALSE(tree->get_sensitive());
        }
        settle();
        EXPECT_EQ(PanelAccess::flushes(*panel), flushes);
        EXPECT_TRUE(PanelAccess::pending(*panel));
    }
    settle();
    EXPECT_EQ(PanelAccess::flushes(*panel) - flushes, 1);
    EXPECT_TRUE(tree->get_sensitive()); check_order(30);
}
TEST_F(ObjectsPanelReentrant, FilterResetInsideMutationFenceWaits) {
    {
        Inkscape::XML::Document::MutationScope bulk(*doc->getReprDoc());
        desktop->getSelection()->ungroup();
        PanelAccess::search(*panel, "p1");
        auto loop = Glib::MainLoop::create();
        Glib::signal_timeout().connect_once([&] { loop->quit(); }, 20);
        loop->run();
        EXPECT_FALSE(tree->get_sensitive());
        EXPECT_TRUE(PanelAccess::pending(*panel));
    }
    settle();
    EXPECT_TRUE(tree->get_sensitive());
    EXPECT_FALSE(PanelAccess::pending(*panel));
    EXPECT_TRUE(contains("p1")); EXPECT_FALSE(contains("p0"));
    EXPECT_FALSE(contains("g0"));
    EXPECT_EQ(desktop->getSelection()->size(), 30);
}
TEST_F(ObjectsPanelReentrant, DropDuringBulkChangeCannotUseStaleRows) {
    desktop->getSelection()->ungroup();
    ASSERT_TRUE(PanelAccess::pending(*panel));
    auto before = xml();
    EXPECT_FALSE(PanelAccess::drop(*panel));
    EXPECT_EQ(xml(), before);
    settle(); check_order(30);
    EXPECT_EQ(desktop->getSelection()->size(), 30);
}
TEST_F(ObjectsPanelReentrant, SearchOmittedBranchAddRemove) {
    PanelAccess::search(*panel, "logo"); settle();
    EXPECT_FALSE(contains("g0"));
    add("g0", "logo"); settle();
    EXPECT_TRUE(contains("g0")); EXPECT_TRUE(contains("logo"));
    doc->getObjectById("logo")->deleteObject(); settle();
    EXPECT_FALSE(contains("g0")); EXPECT_FALSE(contains("logo"));
}
TEST_F(ObjectsPanelReentrant, SearchOmittedBranchRenameBothWays) {
    PanelAccess::search(*panel, "logo"); settle();
    auto node = doc->getObjectById("p0")->getRepr();
    node->setAttribute("inkscape:label", "logo"); settle();
    EXPECT_TRUE(contains("p0")); EXPECT_TRUE(contains("g0"));
    node->setAttribute("inkscape:label", "ordinary"); settle();
    EXPECT_FALSE(contains("p0")); EXPECT_FALSE(contains("g0"));
    node->setAttribute("id", "logo-id"); settle(); EXPECT_TRUE(contains("logo-id"));
    node->setAttribute("id", "p0"); settle(); EXPECT_FALSE(contains("p0"));
}
TEST_F(ObjectsPanelReentrant, LayersOmittedBranchAddRemove) {
    Inkscape::Preferences::get()->setBool("/dialogs/objects/layers_only", true); settle();
    EXPECT_FALSE(contains("g0"));
    add("g0", "new-layer", true); settle();
    EXPECT_TRUE(contains("g0")); EXPECT_TRUE(contains("new-layer"));
    doc->getObjectById("new-layer")->deleteObject(); settle();
    EXPECT_FALSE(contains("g0")); EXPECT_FALSE(contains("new-layer"));
}
TEST_F(ObjectsPanelReentrant, LayersOmittedBranchModeBothWays) {
    Inkscape::Preferences::get()->setBool("/dialogs/objects/layers_only", true); settle();
    auto node = doc->getObjectById("g0")->getRepr();
    node->setAttribute("inkscape:groupmode", "layer"); settle();
    EXPECT_TRUE(contains("g0")); EXPECT_TRUE(contains("destination"));
    node->setAttribute("inkscape:groupmode", "group"); settle();
    EXPECT_FALSE(contains("g0")); EXPECT_FALSE(contains("destination"));
}
TEST_F(ObjectsPanelReentrant, TwoDesktopsBulkUngroupAndCloseOneDirty) {
    auto second = std::make_unique<SPDesktop>(doc->getNamedView());
    Gtk::Window second_canvas; second_canvas.set_child(*second->getCanvas());
    Inkscape::Application::instance().add_desktop(second.get());
    auto second_panel = std::make_unique<Inkscape::UI::Dialog::ObjectsPanel>();
    Gtk::Window second_host; second_host.set_child(*second_panel); second_host.present();
    second_panel->setDesktop(second.get());
    auto second_tree = find_widget<Gtk::TreeView>(*second_panel);
    second->getSelection()->set(cast<SPItem>(doc->getObjectById("outside-before")));
    settle(); tree->expand_all(); second_tree->collapse_all(); settle();
    auto before = xml();
    desktop->getSelection()->ungroup(); settle();
    check_order(30);
    EXPECT_EQ(desktop->getSelection()->size(), 30);
    EXPECT_EQ(second->getSelection()->singleItem(), doc->getObjectById("outside-before"));
    EXPECT_FALSE(second_tree->row_expanded(Gtk::TreeModel::Path("1")));
    EXPECT_TRUE(tree->row_expanded(Gtk::TreeModel::Path("1")));
    second_tree->expand_all(); settle();
    EXPECT_TRUE(contains("p0", second_tree)); EXPECT_TRUE(contains("p29", second_tree));
    auto after = xml();
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); settle(); EXPECT_EQ(xml(), before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    ASSERT_TRUE(PanelAccess::pending(*panel)); ASSERT_TRUE(PanelAccess::pending(*second_panel));
    second_canvas.unset_child();
    Inkscape::Application::instance().remove_desktop(second.get()); second.reset();
    settle(); EXPECT_TRUE(second_tree->get_model()->children().empty());
    EXPECT_EQ(xml(), after); check_order(30);
    second_host.unset_child(); second_panel.reset();
}
class ObjectsPanelClosedControl : public ObjectsPanelBulk {
    void SetUp() override { create(12000, 4, false); }
};
TEST_F(ObjectsPanelClosedControl, NativeUngroupWithoutAttachedPanel) {
    panel->setDesktop(nullptr);
    auto before = xml();
    auto start = std::chrono::steady_clock::now();
    desktop->getSelection()->ungroup(); settle();
    auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "PANEL_CLOSED n=12000 groups=4 seconds=" << seconds << std::endl;
    EXPECT_EQ(desktop->getSelection()->size(), 12000);
    auto after = xml();
    EXPECT_TRUE(DocumentUndo::undo(doc.get())); settle(); EXPECT_EQ(xml(), before);
    EXPECT_TRUE(DocumentUndo::redo(doc.get())); settle(); EXPECT_EQ(xml(), after);
}
class ObjectsPanelClosedOneGroupControl : public ObjectsPanelBulk {
    void SetUp() override { create(12000, 1, false); }
};
TEST_F(ObjectsPanelClosedOneGroupControl, NativeUngroupWithoutAttachedPanel) {
    panel->setDesktop(nullptr);
    auto before = xml();
    auto start = std::chrono::steady_clock::now();
    desktop->getSelection()->ungroup(); settle();
    auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "PANEL_CLOSED n=12000 groups=1 seconds=" << seconds << std::endl;
    EXPECT_EQ(desktop->getSelection()->size(), 12000);
    auto after = xml();
    EXPECT_TRUE(DocumentUndo::undo(doc.get())); settle(); EXPECT_EQ(xml(), before);
    EXPECT_TRUE(DocumentUndo::redo(doc.get())); settle(); EXPECT_EQ(xml(), after);
}

// Native performance/equivalence gates. Capture these against unchanged native
// chemistry first; later runs compare complete bytes and ordered selection IDs.
class NativeUngroupCost : public ObjectsPanelBulk {
protected:
    void SetUp() override {
        auto [n, groups, beginning, end] = GetParam();
        create(n, groups, false, false, n == 30);
        panel->setDesktop(nullptr);
        auto parent = doc->getObjectById("destination")->getRepr();
        if (beginning) parent->changeOrder(doc->getObjectById("before")->getRepr(), parent->lastChild());
        if (end) parent->changeOrder(doc->getObjectById("after")->getRepr(), nullptr);
        settle_native(*doc);
        DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString("Prepare native gate"), "");
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    }
    void exercise(int n, int groups, bool beginning, bool end);
    std::string selection_ids() {
        std::string result;
        for (auto item : desktop->getSelection()->items()) result += std::string(item->getId()) + "\n";
        return result;
    }
    void reference(std::string const &key, std::string const &bytes) {
        if (auto dir = g_getenv("UNGROUP_CAPTURE")) {
            std::ofstream out(std::string(dir) + "/" + key, std::ios::binary); ASSERT_TRUE(out);
            out << bytes;
        }
        if (auto dir = g_getenv("UNGROUP_COMPARE")) {
            std::ifstream in(std::string(dir) + "/" + key, std::ios::binary); ASSERT_TRUE(in) << key;
            std::string expected((std::istreambuf_iterator<char>(in)), {});
            // Only the root inkscape:version value embeds the build hash; no other root build-identity attribute differed.
            static const std::regex root_build_version(R"(^([\s\S]*?<svg:svg\b[^>]*?\binkscape:version=")[^"]*("))");
            auto without_build_version = [&](std::string const &xml) {
                return std::regex_replace(xml, root_build_version, "$1$2");
            };
            EXPECT_TRUE(without_build_version(bytes) == without_build_version(expected)) << "Byte mismatch: " << key;
        }
    }
};
void NativeUngroupCost::exercise(int n, int groups, bool beginning, bool end) {
    auto key = "n" + std::to_string(n) + "-g" + std::to_string(groups) + (beginning ? "-begin" : end ? "-end" : "-middle");
    auto before = xml(); auto before_pixels = native_pixels(*doc);
    if (n <= 30) reference(key + "-before.argb", before_pixels);
    reference(key + "-before.svg", before);
    reference(key + "-before.selection", selection_ids());
    auto load = native_load();
    auto start = std::chrono::steady_clock::now();
    std::cout << "UNGROUP_BEGIN" << std::endl;
    desktop->getSelection()->ungroup();
    auto sync = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    settle_native(*doc);
    auto settled = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "NATIVE_GATE " << key << " sync=" << sync << " settled=" << settled
              << " load=" << load << std::endl;
    // With the O(1) child placement these gates have about 10x headroom on the shared Mac, so they are
    // enforced regardless of machine load (capture mode records baselines and skips them).
    if (!g_getenv("UNGROUP_CAPTURE")) {
        double limit = n <= 3000 ? 2 : n <= 12000 ? 5 : 10;
        EXPECT_LT(sync, limit); EXPECT_LT(settled, limit);
    }
    EXPECT_EQ(desktop->getSelection()->size(), n);
    auto after = xml(); auto selection = selection_ids();
    auto after_pixels = native_pixels(*doc);
    if (n <= 30) reference(key + "-after.argb", after_pixels);
    reference(key + "-after.svg", after);
    reference(key + "-after.selection", selection);
    EXPECT_TRUE(DocumentUndo::undo(doc.get())); settle_native(*doc);
    EXPECT_TRUE(xml() == before); EXPECT_TRUE(native_pixels(*doc) == before_pixels);
    EXPECT_TRUE(DocumentUndo::redo(doc.get())); settle_native(*doc);
    EXPECT_TRUE(xml() == after); EXPECT_TRUE(native_pixels(*doc) == after_pixels);
    reference(key + "-redo.selection", selection_ids());
}

TEST_P(NativeUngroupCost, ClosedPanelGates) {
    auto [n, groups, beginning, end] = GetParam();
    exercise(n, groups, beginning, end);
}
class NativeUngroupBindings : public NativeUngroupCost {
protected:
    std::unique_ptr<Inkscape::Drawing> second_drawing;
    unsigned second_key = 0;
    void SetUp() override {
        doc = SPDocument::createNewDocFromMem(R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="200" height="100"><defs><clipPath id="clip"><rect width="40" height="30"/></clipPath></defs><g id="destination"><rect id="before" width="4" height="6" fill="red"/><!-- predecessor gap --><metadata>gap</metadata><g id="g0" transform="translate(3,5)" style="fill:#112233" clip-path="url(#clip)"><rect id="p0" width="12" height="11"/><text id="p1" x="4" y="8">Ab</text></g><rect id="after" width="8" height="9" fill="blue"/></g><use id="clone" xlink:href="#g0" x="30"/><g id="kept-layer" inkscape:groupmode="layer"><rect id="layer-child" width="2" height="2"/></g><rect id="mixed" width="2" height="3"/></svg>)SVG");
        ASSERT_TRUE(doc); doc->ensureUpToDate(); attach({"g0", "clone", "kept-layer", "mixed"});
        panel->setDesktop(nullptr);
        second_drawing = std::make_unique<Inkscape::Drawing>();
        second_key = SPItem::display_key_new(1);
        second_drawing->setRoot(doc->getRoot()->invoke_show(*second_drawing, second_key, SP_ITEM_SHOW_DISPLAY));
        settle_native(*doc);
    }
    void TearDown() override {
        if (doc && second_drawing) doc->getRoot()->invoke_hide(second_key);
        second_drawing.reset(); ObjectsPanelBulk::TearDown();
    }
};
TEST_F(NativeUngroupBindings, CommentsCloneLayerMixedSelectionAndTwoDrawings) {
    exercise(5, 1, false, false);
    EXPECT_TRUE(doc->getObjectById("kept-layer")); EXPECT_TRUE(doc->getObjectById("layer-child"));
}
TEST_F(NativeUngroupBindings, DeferredSecondDrawingCanBeClosedDuringUngroup) {
    auto before = xml();
    second_drawing->snapshot();
    desktop->getSelection()->ungroup();
    doc->getRoot()->invoke_hide(second_key);
    second_drawing->unsnapshot();
    EXPECT_EQ(second_drawing->root(), nullptr);
    settle_native(*doc);
    auto after = xml();
    reference("deferred-after.svg", after);
    reference("deferred-after.selection", selection_ids());
    reference("deferred-after.argb", native_pixels(*doc));
    EXPECT_TRUE(DocumentUndo::undo(doc.get())); settle_native(*doc); EXPECT_TRUE(xml() == before);
    EXPECT_TRUE(DocumentUndo::redo(doc.get())); settle_native(*doc); EXPECT_TRUE(xml() == after);
}
class NativeUngroupExistingVector : public NativeUngroupCost {
    void SetUp() override { create(30, 1, false, false, true); panel->setDesktop(nullptr); }
};
TEST_F(NativeUngroupExistingVector, PreservesPrefixAndMixedChildOrder) {
    auto before = xml();
    std::vector<SPItem *> result{cast<SPItem>(doc->getObjectById("outside-before")),
                                cast<SPItem>(doc->getObjectById("outside-after"))};
    sp_item_group_ungroup(cast<SPGroup>(doc->getObjectById("g0")), result);
    DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString("Ungroup direct"), "");
    settle_native(*doc);
    ASSERT_EQ(result.size(), 32);
    EXPECT_STREQ(result[30]->getId(), "outside-before"); EXPECT_STREQ(result[31]->getId(), "outside-after");
    std::string ids; for (auto item : result) ids += std::string(item->getId()) + "\n";
    reference("existing-vector-after.selection", ids);
    auto after = xml(); reference("existing-vector-after.svg", after);
    EXPECT_TRUE(DocumentUndo::undo(doc.get())); settle(); EXPECT_TRUE(xml() == before);
    EXPECT_TRUE(DocumentUndo::redo(doc.get())); settle(); EXPECT_TRUE(xml() == after);
}
INSTANTIATE_TEST_SUITE_P(NativeMatrix, NativeUngroupCost, ::testing::Values(
    std::make_tuple(30, 1, true, false),
    std::make_tuple(30, 4, false, false),
    std::make_tuple(3000, 1, true, false),
    std::make_tuple(3000, 1, false, false),
    std::make_tuple(3000, 1, false, true),
    std::make_tuple(3000, 4, true, false),
    std::make_tuple(3000, 4, false, false),
    std::make_tuple(3000, 4, false, true),
    std::make_tuple(12000, 1, true, false),
    std::make_tuple(12000, 1, false, false),
    std::make_tuple(12000, 1, false, true),
    std::make_tuple(12000, 4, true, false),
    std::make_tuple(12000, 4, false, false),
    std::make_tuple(12000, 4, false, true),
    std::make_tuple(23000, 1, true, false),
    std::make_tuple(23000, 1, false, false),
    std::make_tuple(23000, 1, false, true),
    std::make_tuple(23000, 4, true, false),
    std::make_tuple(23000, 4, false, false),
    std::make_tuple(23000, 4, false, true)));
// The owner's large fixture stays outside the source tree. When explicitly
// supplied, register a real outcome test; absence is not counted as a pass/skip.
class ObjectsPanelOwner : public ObjectsPanelBulk {
public:
    void SetUp() override {
        doc = SPDocument::createNewDoc(g_getenv("BUG026_OWNER_FIXTURE")); ASSERT_TRUE(doc);
        std::vector<std::string> ids{"g1087-0", "g1087-0-0", "g1087-0-4", "g1087-0-4-5"};
        auto layer = doc->getObjectById("layer-MC0"); ASSERT_TRUE(layer);
        std::vector<SPObject *> remove;
        for (auto &child : layer->children) {
            if (!is<SPItem>(&child)) continue;
            auto id = child.getId();
            if (!id || std::find(ids.begin(), ids.end(), id) == ids.end()) remove.push_back(&child);
        }
        for (auto child : remove) child->deleteObject();
        cast<SPItem>(layer)->setExpanded(true);
        doc->ensureUpToDate();
        attach(ids);
    }
    void TestBody() override {
        std::vector<std::string> ids;
        for (auto group : desktop->getSelection()->items())
            for (auto &child : group->children)
                if (is<SPItem>(&child)) ids.emplace_back(child.getId());
        ASSERT_GT(ids.size(), 11000);
        auto before = xml();
        auto flushes = PanelAccess::flushes(*panel);
        int reorders = 0;
        auto connection = tree->get_model()->signal_rows_reordered().connect([&](auto const &, auto const &, int *) { ++reorders; });
        auto start = std::chrono::steady_clock::now();
        desktop->getSelection()->ungroup(); settle();
        auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "OWNER_HALFTONE n=" << ids.size() << " seconds=" << seconds << " reorders=" << reorders << std::endl;
        EXPECT_LT(seconds, 10.0); EXPECT_EQ(reorders, 0);
        EXPECT_EQ(PanelAccess::flushes(*panel) - flushes, 1);
        EXPECT_EQ(desktop->getSelection()->size(), ids.size());
        for (auto const &id : ids) ASSERT_TRUE(doc->getObjectById(id)) << id;
        auto layer = doc->getObjectById("layer-MC0");
        std::vector<Inkscape::XML::Node *> expected, actual;
        for (auto &child : layer->children) if (is<SPItem>(&child)) expected.push_back(child.getRepr());
        std::reverse(expected.begin(), expected.end());
        for (auto const &row : tree->get_model()->children())
            if (repr(row) == layer->getRepr())
                for (auto const &child : row.children()) actual.push_back(repr(child));
        EXPECT_EQ(actual, expected);
        auto after = xml();
        EXPECT_TRUE(DocumentUndo::undo(doc.get())); settle(); EXPECT_EQ(xml(), before);
        EXPECT_TRUE(DocumentUndo::redo(doc.get())); settle(); EXPECT_EQ(xml(), after);
        EXPECT_EQ(PanelAccess::flushes(*panel) - flushes, 3);
        connection.disconnect();
    }
};

class NativeUngroupOwner : public ObjectsPanelOwner {
public:
    void SetUp() override { ObjectsPanelOwner::SetUp(); panel->setDesktop(nullptr); }
    void TestBody() override {
        auto reference = [&](std::string const &key, std::string const &bytes) {
            if (auto dir = g_getenv("UNGROUP_CAPTURE")) {
                std::ofstream out(std::string(dir) + "/owner-" + key, std::ios::binary); ASSERT_TRUE(out); out << bytes;
            }
            if (auto dir = g_getenv("UNGROUP_COMPARE")) {
                std::ifstream in(std::string(dir) + "/owner-" + key, std::ios::binary); ASSERT_TRUE(in);
                std::string expected((std::istreambuf_iterator<char>(in)), {});
                EXPECT_TRUE(bytes == expected) << "Owner byte mismatch: " << key;
            }
        };
        auto selection_ids = [&] {
            std::string ids;
            for (auto item : desktop->getSelection()->items()) ids += std::string(item->getId()) + "\n";
            return ids;
        };
        auto before = xml(); reference("before.svg", before); reference("before.selection", selection_ids());
        auto load = native_load();
        auto start = std::chrono::steady_clock::now();
        desktop->getSelection()->ungroup();
        auto sync = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        settle_native(*doc);
        auto settled = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "NATIVE_OWNER n=" << desktop->getSelection()->size() << " sync=" << sync << " settled=" << settled
                  << " load=" << load << std::endl;
        if (!g_getenv("UNGROUP_CAPTURE")) { EXPECT_LT(sync, 5); EXPECT_LT(settled, 5); }
        EXPECT_EQ(desktop->getSelection()->size(), 11696);
        auto after = xml(); reference("after.argb", native_pixels(*doc)); reference("after.svg", after); reference("after.selection", selection_ids());
        EXPECT_TRUE(DocumentUndo::undo(doc.get())); settle(); EXPECT_TRUE(xml() == before);
        EXPECT_TRUE(DocumentUndo::redo(doc.get())); settle(); EXPECT_TRUE(xml() == after);
        reference("redo.selection", selection_ids());
    }
};
[[maybe_unused]] bool const owner_registered = [] {
    if (g_getenv("BUG026_OWNER_FIXTURE")) {
        ::testing::RegisterTest("NativeUngroupOwner", "ClosedPanel", nullptr, nullptr,
                               __FILE__, __LINE__, []() -> NativeUngroupOwner * { return new NativeUngroupOwner; });
        ::testing::RegisterTest("ObjectsPanelOwner", "HalftoneR2", nullptr, nullptr,
                               __FILE__, __LINE__, []() -> ObjectsPanelOwner * { return new ObjectsPanelOwner; });
    }
    return true;
}();
INSTANTIATE_TEST_SUITE_P(Matrix,ObjectsPanelBulk,::testing::Combine(::testing::Values(3000,6000,12000),::testing::Values(1,4),::testing::Bool(),::testing::Bool()));

// BUG-026b reproduces the status-bar listener that is present in the installed
// UI but absent from the original bulk-panel fixture.
class UndoFixUngroup : public ObjectsPanelBulk {
protected:
    void SetUp() override {
        auto [n, groups, _, __] = GetParam();
        create(n, groups, false, false);
        statusbar = std::make_unique<Inkscape::UI::Widget::SelectedStyle>();
        statusbar->setDesktop(desktop.get());
    }
    void TearDown() override {
        statusbar->setDesktop(nullptr);
        statusbar.reset();
        ObjectsPanelBulk::TearDown();
    }
    std::unique_ptr<Inkscape::UI::Widget::SelectedStyle> statusbar;
};

// Reproduce an ordinary O(selection) observer alongside the UndoFix UI
// listeners. The observer models style/bounds consumers that scan the entire
// live selection on every synchronous selection notification.
struct ReplayCostObserver {
    int calls = 0;
    double bounds_work = 0;

    void changed(Inkscape::Selection *selection) {
        ++calls;
        for (auto item : selection->items()) {
            if (auto bounds = item->visualBounds()) {
                bounds_work += bounds->min()[Geom::X] + bounds->min()[Geom::Y] +
                               bounds->max()[Geom::X] + bounds->max()[Geom::Y];
            }
        }
    }
};

std::string selected_ids(Inkscape::Selection *selection) {
    std::vector<std::string> ids;
    for (auto item : selection->items()) ids.emplace_back(item->getId());
    std::sort(ids.begin(), ids.end());
    std::ostringstream result;
    result << "[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) result << ",";
        result << ids[i];
    }
    result << "]";
    return result.str();
}

void report_final_selection(char const *phase, Inkscape::Selection *selection) {
    std::cout << "BATCH_FINAL_SELECTION phase=" << phase << " size=" << selection->size()
              << " ids=" << selected_ids(selection) << std::endl;
}

template <typename Operation, typename Xml>
void measure_replay_batch(std::unique_ptr<SPDocument> const &doc, Inkscape::Selection *selection,
                          Xml xml, char const *label, int n, int groups, Operation operation) {
    ReplayCostObserver observer;
    auto connection = selection->connectChanged([&](auto current) { observer.changed(current); });
    auto const before = xml();
    operation();
    settle();
    auto const after = xml();
    ASSERT_NE(before, after);
    auto const operation_calls = observer.calls;
    observer.calls = 0;
    observer.bounds_work = 0;
    auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    settle();
    auto const undo_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_EQ(xml(), before);
    report_final_selection("undo", selection);
    auto const undo_calls = observer.calls;
    EXPECT_TRUE(selection->isEmpty());
    observer.calls = 0;
    observer.bounds_work = 0;
    start = std::chrono::steady_clock::now();
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    settle();
    auto const redo_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_EQ(xml(), after);
    report_final_selection("redo", selection);
    EXPECT_TRUE(selection->isEmpty());
    std::cout << "BATCH_PROFILE case=" << label << " n=" << n << " groups=" << groups
              << " operation_listener_calls=" << operation_calls << " undo_seconds=" << undo_seconds
              << " undo_listener_calls=" << undo_calls
              << " redo_seconds=" << redo_seconds << " redo_listener_calls=" << observer.calls
              << " bounds_work=" << observer.bounds_work << std::endl;
    EXPECT_LE(undo_calls, 2);
    EXPECT_LE(observer.calls, 2);
    if (n == 12000) {
        EXPECT_LE(undo_seconds, 10.0);
        EXPECT_LE(redo_seconds, 10.0);
    }
    connection.disconnect();
}

TEST_P(UndoFixUngroup, ReplayUndoRedo) {
    auto [n, groups, _, __] = GetParam();
    auto const before = xml();
    auto start = std::chrono::steady_clock::now();
    desktop->getSelection()->ungroup();
    settle();
    double op = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    auto const after = xml();
    ASSERT_NE(before, after);
    start = std::chrono::steady_clock::now();
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    settle();
    double undo = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_EQ(xml(), before);
    start = std::chrono::steady_clock::now();
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    settle();
    double redo = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_EQ(xml(), after);
    std::cout << "UNDO_FIX ungroup-n" << n << "-g" << groups << " op=" << op << " undo=" << undo
              << " redo=" << redo << " listeners=ObjectsPanel,SelectedStyle,ApplicationActiveSelection" << std::endl;
}
TEST_P(UndoFixUngroup, ReplayBatchBaseline) {
    auto [n, groups, _, __] = GetParam();
    measure_replay_batch(doc, desktop->getSelection(), [&] { return xml(); }, "ungroup", n, groups,
                         [&] { desktop->getSelection()->ungroup(); });
}
INSTANTIATE_TEST_SUITE_P(UndoFix, UndoFixUngroup, ::testing::Combine(
    ::testing::Values(3000, 6000, 12000), ::testing::Values(1, 4),
    ::testing::Values(false), ::testing::Values(false)));

class UndoFixBreakApart : public ObjectsPanelBulk {
protected:
    void SetUp() override {
        auto [n, _, __, ___] = GetParam();
        std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100"><path id="donor" d=")";
        for (int i = 0; i < n; ++i) {
            auto x = (i % 100) * 2;
            auto y = (i / 100) * 2;
            svg += "M" + std::to_string(x) + "," + std::to_string(y) + "h1v1h-1z";
        }
        svg += R"("/></svg>)";
        doc = SPDocument::createNewDocFromMem(svg);
        ASSERT_TRUE(doc);
        doc->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(doc->getNamedView());
        canvas = std::make_unique<Gtk::Window>(); canvas->set_child(*desktop->getCanvas());
        Inkscape::Application::instance().add_desktop(desktop.get());
        panel = std::make_unique<Inkscape::UI::Dialog::ObjectsPanel>();
        host = std::make_unique<Gtk::Window>(); host->set_child(*panel);
        panel->setDesktop(desktop.get()); host->present();
        tree = find_widget<Gtk::TreeView>(*panel); ASSERT_TRUE(tree);
        desktop->getSelection()->set(cast<SPItem>(doc->getObjectById("donor")));
        settle();
        if (auto row = tree->get_model()->get_iter("0")) tree->collapse_row(tree->get_model()->get_path(row));
        settle();
        statusbar = std::make_unique<Inkscape::UI::Widget::SelectedStyle>();
        statusbar->setDesktop(desktop.get());
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    }
    void TearDown() override {
        statusbar->setDesktop(nullptr); statusbar.reset();
        ObjectsPanelBulk::TearDown();
    }
    std::unique_ptr<Inkscape::UI::Widget::SelectedStyle> statusbar;
};
TEST_P(UndoFixBreakApart, ReplayUndoRedo) {
    auto [n, _, __, ___] = GetParam();
    auto const before = xml();
    auto start = std::chrono::steady_clock::now();
    desktop->getSelection()->breakApart();
    settle();
    double op = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    auto const after = xml();
    ASSERT_NE(before, after);
    start = std::chrono::steady_clock::now();
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    settle();
    double undo = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_EQ(xml(), before);
    start = std::chrono::steady_clock::now();
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    settle();
    double redo = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_EQ(xml(), after);
    std::cout << "UNDO_FIX break-apart-n" << n << " op=" << op << " undo=" << undo << " redo=" << redo
              << " listeners=ObjectsPanel,SelectedStyle,ApplicationActiveSelection" << std::endl;
}
TEST_P(UndoFixBreakApart, ReplayBatchBaseline) {
    auto [n, _, __, ___] = GetParam();
    measure_replay_batch(doc, desktop->getSelection(), [&] { return xml(); }, "break-apart", n, 1,
                         [&] { desktop->getSelection()->breakApart(); });
}
INSTANTIATE_TEST_SUITE_P(UndoFix, UndoFixBreakApart, ::testing::Combine(
    ::testing::Values(3000, 12000), ::testing::Values(0), ::testing::Values(false), ::testing::Values(false)));

// Real owner-sheet profiling is opt-in through GTEST_ALSO_RUN_DISABLED_TESTS.
// Each case loads a disposable copy supplied through VACARDS_OWNER_SHEET.
class RealOwnerSheet : public ObjectsPanelBulk {
protected:
    static std::vector<std::string> const r1_ids;
    static std::vector<std::string> const r2_ids;
    static std::vector<std::string> const r3_ids;

    void SetUp() override {}

    void load_rung(std::vector<std::string> const &ids) {
        auto path = g_getenv("VACARDS_OWNER_SHEET");
        ASSERT_NE(path, nullptr) << "VACARDS_OWNER_SHEET must name a disposable owner-file copy";
        doc = SPDocument::createNewDoc(path);
        ASSERT_TRUE(doc);
        auto layer = doc->getObjectById("layer-MC0");
        ASSERT_TRUE(layer);
        std::vector<SPObject *> remove;
        for (auto &child : layer->children) {
            if (!is<SPItem>(&child)) continue;
            auto id = child.getId();
            if (!id || std::find(ids.begin(), ids.end(), id) == ids.end()) remove.push_back(&child);
        }
        for (auto child : remove) child->deleteObject();
        cast<SPItem>(layer)->setExpanded(true);
        doc->ensureUpToDate();
        attach(ids);
        // Keep the same ObjectsPanel listener bound as UndoFix while the panel
        // window is hidden, matching the installed panel-closed measurements.
        host->set_visible(false);
        statusbar = std::make_unique<Inkscape::UI::Widget::SelectedStyle>();
        statusbar->setDesktop(desktop.get());
        DocumentUndo::clearUndo(doc.get());
        DocumentUndo::clearRedo(doc.get());
    }

    void TearDown() override {
        if (statusbar) statusbar->setDesktop(nullptr);
        statusbar.reset();
        ObjectsPanelBulk::TearDown();
    }

    std::size_t path_count() const {
        std::size_t result = 0;
        auto visit = [&](auto &&self, SPObject const *object) -> void {
            if (is<SPPath>(object)) ++result;
            for (auto const &child : object->children) self(self, &child);
        };
        visit(visit, doc->getRoot());
        return result;
    }

    void run_union(std::vector<std::string> const &ids, std::string const &cell) {
        load_rung(ids);
        desktop->getSelection()->ungroup();
        settle();
        Inkscape::SelectionHelper::selectAll(desktop.get());
        settle();
        record_operation(cell, "Union", [&] {
            auto selection = desktop->getSelection();
            selection->removeLPESRecursive(true);
            selection->unlinkRecursive(true);
            selection->pathUnion();
        });
    }

    void record_operation(std::string const &cell, std::string const &op, auto operation) {
        auto const before = xml();
        auto const paths_before = path_count();
        std::cout << "REAL_START " << cell << " " << op << std::endl;
        auto start = std::chrono::steady_clock::now();
        operation();
        settle();
        auto const after = xml();
        auto const paths_after = path_count();
        ASSERT_NE(before, after) << cell << " " << op << " did not change XML";
        // The root's inkscape:version names the build commit; hash without its value so
        // results stay comparable across commits.
        auto normalized = after;
        constexpr std::string_view version_attribute = "inkscape:version=\"";
        if (auto start = normalized.find(version_attribute); start != std::string::npos) {
            start += version_attribute.size();
            if (auto end = normalized.find('"', start); end != std::string::npos) {
                normalized.erase(start, end - start);
            }
        }
        auto const hash = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, reinterpret_cast<guchar const *>(normalized.data()), normalized.size());
        std::cout << "REAL_RESULT " << cell << " sha256=" << hash << std::endl;
        g_free(hash);
        std::cout << "REAL " << cell << " " << op << " seconds="
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                  << " paths_before=" << paths_before << " paths_after=" << paths_after << std::endl;

        std::cout << "REAL_START " << cell << " " << op << "Undo" << std::endl;
        start = std::chrono::steady_clock::now();
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        settle();
        auto const undo_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        ASSERT_EQ(xml(), before);
        std::cout << "REAL " << cell << " " << op << "Undo seconds=" << undo_seconds
                  << " paths_before=" << paths_after << " paths_after=" << paths_before << std::endl;

        std::cout << "REAL_START " << cell << " " << op << "Redo" << std::endl;
        start = std::chrono::steady_clock::now();
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        settle();
        auto const redo_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        ASSERT_EQ(xml(), after);
        std::cout << "REAL " << cell << " " << op << "Redo seconds=" << redo_seconds
                  << " paths_before=" << paths_before << " paths_after=" << paths_after << std::endl;
    }

    std::unique_ptr<Inkscape::UI::Widget::SelectedStyle> statusbar;
};

std::vector<std::string> const RealOwnerSheet::r1_ids{"g1087"};
std::vector<std::string> const RealOwnerSheet::r2_ids{"g1087-0", "g1087-0-0", "g1087-0-4", "g1087-0-4-5"};
std::vector<std::string> const RealOwnerSheet::r3_ids{
    "g1087", "g1087-0", "g1087-2", "g1087-0-4", "g1087-1", "g1087-0-0", "g1087-2-3", "g1087-0-4-5"};

TEST_F(RealOwnerSheet, DISABLED_UngroupR1) {
    load_rung(r1_ids);
    record_operation("UngroupR1", "Ungroup", [&] { desktop->getSelection()->ungroup(); });
}
TEST_F(RealOwnerSheet, DISABLED_UngroupR2) {
    load_rung(r2_ids);
    record_operation("UngroupR2", "Ungroup", [&] { desktop->getSelection()->ungroup(); });
}
TEST_F(RealOwnerSheet, DISABLED_UngroupR3) {
    load_rung(r3_ids);
    record_operation("UngroupR3", "Ungroup", [&] { desktop->getSelection()->ungroup(); });
}

TEST_F(RealOwnerSheet, DISABLED_UngroupSelectAllUnionR1) { run_union(r1_ids, "UnionR1"); }
TEST_F(RealOwnerSheet, DISABLED_UngroupSelectAllUnionR2) { run_union(r2_ids, "UnionR2"); }
TEST_F(RealOwnerSheet, DISABLED_UngroupSelectAllUnionR3) { run_union(r3_ids, "UnionR3"); }

TEST_F(RealOwnerSheet, DISABLED_CombineR2) {
    load_rung(r2_ids);
    desktop->getSelection()->ungroup();
    settle();
    Inkscape::SelectionHelper::selectAll(desktop.get());
    settle();
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    record_operation("CombineR2", "Combine", [&] { desktop->getSelection()->combine(); });
}
TEST_F(RealOwnerSheet, DISABLED_BreakApartR2) {
    load_rung(r2_ids);
    desktop->getSelection()->ungroup();
    settle();
    Inkscape::SelectionHelper::selectAll(desktop.get());
    settle();
    desktop->getSelection()->combine();
    settle();
    DocumentUndo::clearUndo(doc.get());
    DocumentUndo::clearRedo(doc.get());
    record_operation("BreakApartR2", "BreakApart", [&] { desktop->getSelection()->breakApart(); });
}

// SPGroup::child_added places a child inserted in the middle of a group directly after the drawing item of its
// previous SPItem sibling (Ungroup performance). Oracle: the incremental drawing order in every view must render
// byte-for-byte like a fresh load of the same XML. That covers a non-item sibling between, insertion at the front,
// insertion after a freshly inserted item, and two independent views.
std::string render_drawing(Inkscape::Drawing &drawing, int width, int height) {
    auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    {
        Inkscape::DrawingContext context(surface, Geom::Point(0, 0));
        drawing.render(context, Geom::IntRect::from_xywh(0, 0, width, height));
    }
    cairo_surface_flush(surface);
    std::string pixels(reinterpret_cast<char *>(cairo_image_surface_get_data(surface)),
                       cairo_image_surface_get_stride(surface) * height);
    cairo_surface_destroy(surface);
    return pixels;
}
TEST(GroupChildDrawingOrder, MiddleInsertionsRenderLikeAFreshLoadInEveryView) {
    if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    constexpr int width = 40, height = 10;
    std::string const svg = R"(<svg xmlns='http://www.w3.org/2000/svg' width='40' height='10' viewBox='0 0 40 10'>
        <g id='layer'>
          <rect id='a' x='0' y='0' width='20' height='10' fill='#ff0000'/>
          <title id='t'>between</title>
          <rect id='b' x='10' y='0' width='20' height='10' fill='#0000ff'/>
          <rect id='c' x='25' y='0' width='15' height='10' fill='#ffff00'/>
        </g></svg>)";
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(), svg.size()));
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto root = doc->getRoot();
    Inkscape::Drawing first, second;
    auto const first_key = SPItem::display_key_new(1), second_key = SPItem::display_key_new(1);
    first.setRoot(root->invoke_show(first, first_key, SP_ITEM_SHOW_DISPLAY));
    second.setRoot(root->invoke_show(second, second_key, SP_ITEM_SHOW_DISPLAY));
    auto layer = doc->getObjectById("layer")->getRepr();
    auto add = [&](char const *id, char const *x, char const *fill, char const *after) {
        auto node = doc->getReprDoc()->createElement("svg:rect");
        node->setAttribute("id", id); node->setAttribute("x", x); node->setAttribute("y", "0");
        node->setAttribute("width", "12"); node->setAttribute("height", "10"); node->setAttribute("fill", fill);
        layer->addChild(node, after ? doc->getObjectById(after)->getRepr() : nullptr);
        Inkscape::GC::release(node);
    };
    add("x1", "5", "#00ff00", "t");    // after a non-item sibling; the previous SPItem is a
    add("x2", "15", "#ff00ff", "a");   // directly after an original item
    add("x3", "30", "#00ffff", nullptr); // at the front
    add("x4", "2", "#808080", "x1");   // after a freshly inserted item
    doc->ensureUpToDate();
    first.update(); second.update();
    auto const first_pixels = render_drawing(first, width, height);
    auto const second_pixels = render_drawing(second, width, height);

    auto const xml = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto fresh = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
    ASSERT_TRUE(fresh);
    fresh->ensureUpToDate();
    Inkscape::Drawing reference;
    auto const reference_key = SPItem::display_key_new(1);
    reference.setRoot(fresh->getRoot()->invoke_show(reference, reference_key, SP_ITEM_SHOW_DISPLAY));
    reference.update();
    auto const expected = render_drawing(reference, width, height);
    EXPECT_TRUE(first_pixels == expected);
    EXPECT_TRUE(second_pixels == expected);

    fresh->getRoot()->invoke_hide(reference_key);
    root->invoke_hide(first_key);
    root->invoke_hide(second_key);
}
}
