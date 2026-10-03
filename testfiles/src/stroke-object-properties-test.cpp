// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <array>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <vector>
#include <gtkmm/window.h>
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-item.h"
#include "preferences.h"
#include "selection.h"
#include "style.h"
#include "text-editing.h"
#include "libnrtype/Layout-TNG.h"
#include "ui/widget/paint-attribute.h"
#include "ui/widget/canvas.h"
#include "util/scope_exit.h"
#include "xml/document.h"
#include "xml/node-observer.h"
#include "xml/repr.h"
#include "inkgc/gc-managed.h"
using namespace Inkscape;
namespace SW = Inkscape::UI;
namespace Inkscape::UI::Widget {
struct PaintAttributeTestAccess {
    static void remove(PaintAttribute &panel) { g_signal_emit_by_name(panel._stroke._clear.gobj(), "clicked"); }
};
}
namespace {
std::unique_ptr<SPDocument> fixture(std::string const &extra = "")
{
    auto doc = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" width="200" height="100">)" + extra +
        R"svg(<g id="bound" transform="scale(2)"><text id="owner" x="11.2345" y="22.6789" style="font-size:12px;stroke:#000000;stroke-width:7"><tspan id="a" class="local" sodipodi:role="line" x="11.2345" y="22.6789" style="stroke:#ff0000;stroke-width:2 !important;stroke-dasharray:4,8;stroke-dashoffset:-2;stroke-opacity:0.4;stroke-linejoin:bevel;stroke-linecap:square;stroke-miterlimit:5"> A </tspan><tspan id="b" x="11.2345" dy="15.4321" rotate="2 3" style="stroke:#00ff00;stroke-width:3;stroke-dasharray:3,6;stroke-dashoffset:1;vector-effect:non-scaling-stroke">B</tspan></text></g><path id="selected" d="M0,0 L10,10" style="stroke:#000000;stroke-width:9"/></svg>)svg");
    if (doc) {
        doc->ensureUpToDate();
        doc->getSelection()->set(cast<SPItem>(doc->getObjectById("selected")));
        DocumentUndo::clearUndo(doc.get());
        DocumentUndo::clearRedo(doc.get());
    }
    return doc;
}
std::string xml(SPDocument &doc)
{
    auto *copy = doc.getReprDoc()->duplicate(nullptr);
    auto result = sp_repr_save_buf(copy).raw();
    GC::release(copy);
    return result;
}
std::string outside(std::string const &input, std::string const &properties)
{
    std::regex styles(R"re(\s+style="([^"]*)")re");
    std::regex declaration("(^|;)(" + properties + "):[^;]*;?");
    std::string result;
    size_t last = 0;
    for (std::sregex_iterator i(input.begin(), input.end(), styles), end; i != end; ++i) {
        result += input.substr(last, i->position() - last);
        auto value = std::regex_replace((*i)[1].str(), declaration, "$1");
        // Adjacent whitelisted declarations must each be stripped.
        value = std::regex_replace(value, declaration, "$1");
        while (!value.empty() && value.back() == ';') value.pop_back();
        if (!value.empty()) result += " style=\"" + value + "\"";
        last = i->position() + i->length();
    }
    return result + input.substr(last);
}
struct Geometry {
    std::vector<Geom::Point> anchors;
    std::vector<std::pair<FontInstance *, std::array<double, 12>>> glyphs;
    bool operator==(Geometry const &) const = default;
};
Geometry geometry(SPDocument &doc)
{
    doc.ensureUpToDate();
    Geometry result;
    auto *layout = te_get_layout(cast<SPItem>(doc.getObjectById("owner")));
    if (layout) for (auto it = layout->begin(); it != layout->end(); it.nextCharacter())
        result.anchors.push_back(layout->characterAnchorPoint(it));
    if (layout) for (auto const &glyph : layout->glyphs()) {
        auto t = glyph.transform(*layout);
        result.glyphs.push_back({glyph.span(layout).font.get(), {double(glyph.glyph), double(glyph.in_character),
            double(glyph.hidden), double(glyph.orientation), glyph.advance, glyph.vertical_scale,
            t[0], t[1], t[2], t[3], t[4], t[5]}});
    }
    return result;
}
bool apply(SPDocument &doc, SW::StrokeWidthIntentKind kind, double value = 0,
           std::function<bool()> const &live = {})
{
    return SW::Widget::apply_object_properties_stroke(cast<SPItem>(doc.getObjectById("bound")),
                                                     {kind, value, true}, live);
}
class Callback : public XML::NodeObserver {
public:
    Callback(XML::Node *node, std::function<void()> action) : node(node), action(std::move(action)) { node->addObserver(*this); }
    ~Callback() override { if (node) node->removeObserver(*this); }
    void notifyAttributeChanged(XML::Node &, GQuark name, Util::ptr_shared, Util::ptr_shared) override {
        if (name != g_quark_from_static_string("style")) return;
        node->removeObserver(*this); node = nullptr; fired = true; action();
    }
    XML::Node *node; std::function<void()> action; bool fired = false;
};
// Defined first: the native GUI application must exist before headless fixtures.
TEST(StrokeObjectPropertiesGUI, ActualNoneButtonBoundOwnerAndCallbackRebinding)
{
    if (!g_getenv("INKSCAPE_TEST_GUI")) GTEST_SKIP() << "Requires the supervisor desktop session";
    static auto *app = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "strokeobjectpropertiestest", TRUE);
        auto *result = new InkscapeApplication();
        // Registration runs normal startup, including the legacy Application
        // singleton used when the paint widget loads its gradient SVG resource.
        if (result->gtk_app()) result->gio_app()->register_application();
        return result;
    }();
    ASSERT_TRUE(app->gtk_app());
    ASSERT_TRUE(app->gio_app()->is_registered());
    ASSERT_TRUE(Application::exists());
    ASSERT_TRUE(INKSCAPE.use_gui());
    Preferences::get()->setBool("/options/svgoutput/check_on_reading", false);
    Preferences::get()->setBool("/options/svgoutput/check_on_writing", false);
    Preferences::get()->setBool("/options/svgoutput/sort_attributes", false);
    for (int callback_mode : {0, 1, 2}) {
        auto owned_doc = fixture(); ASSERT_TRUE(owned_doc);
        auto *doc = app->document_add(std::move(owned_doc));
        SPDesktop *desktop = nullptr;
        auto close = scope_exit([&] {
            doc->setModifiedSinceSave(false);
            if (desktop) app->destroyDesktop(desktop);
            else app->document_close(doc);
        });
        // Paint controls also require the registered active desktop, as in the
        // real Object Properties dialog (including its shared color notebook).
        desktop = app->createDesktop(doc, false, true);
        ASSERT_TRUE(desktop);
        ASSERT_EQ(SP_ACTIVE_DESKTOP, desktop);
        auto before = xml(*doc);
        auto anchors = geometry(*doc);
        desktop->getSelection()->set(cast<SPItem>(doc->getObjectById("selected")));
        SW::Widget::InkPropertyGrid grid;
        SW::Widget::PaintAttribute panel(SW::Widget::PaintAttribute::AllParts, 0);
        panel.insert_widgets(grid);
        panel.set_document(doc);
        panel.set_desktop(desktop);
        panel.update_from_object(doc->getObjectById("owner"));
        std::unique_ptr<Callback> callback;
        if (callback_mode) callback = std::make_unique<Callback>(doc->getObjectById("a")->getRepr(), [&] {
            if (callback_mode == 1) panel.update_from_object(doc->getObjectById("selected"));
            else doc->getObjectById("owner")->deleteObject();
        });
        SW::Widget::PaintAttributeTestAccess::remove(panel);
        auto after = xml(*doc);
        EXPECT_EQ(geometry(*doc), anchors);
        if (callback_mode) {
            EXPECT_TRUE(callback->fired); EXPECT_EQ(after, before);
            EXPECT_FALSE(DocumentUndo::undo(doc));
            panel.update_from_object(nullptr);
            panel.set_desktop(nullptr);
            continue;
        }
        for (auto id : {"a", "b"}) EXPECT_TRUE(doc->getObjectById(id)->style->stroke.isNone());
        EXPECT_EQ(outside(before, "stroke"), outside(after, "stroke"));
        EXPECT_EQ(doc->getSelection()->singleItem(), doc->getObjectById("selected"));
        ASSERT_TRUE(DocumentUndo::undo(doc)); EXPECT_EQ(xml(*doc), before);
        EXPECT_FALSE(DocumentUndo::undo(doc));
        ASSERT_TRUE(DocumentUndo::redo(doc)); EXPECT_EQ(xml(*doc), after);
        panel.update_from_object(nullptr);
        panel.set_desktop(nullptr);
    }
}
class StrokeObjectProperties : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        if (!Application::exists()) Application::create(false);
        Preferences::get()->setBool("/options/svgoutput/check_on_reading", false);
        Preferences::get()->setBool("/options/svgoutput/check_on_writing", false);
        Preferences::get()->setBool("/options/svgoutput/sort_attributes", false);
    }
};
TEST_F(StrokeObjectProperties, BoundGroupNumericWidthsDashesPriorityAndExactUndo)
{
    auto doc = fixture("<style>.local{fill:#008000;font-family:sans-serif}</style>"); ASSERT_TRUE(doc);
    auto before = xml(*doc); auto anchors = geometry(*doc);
    ASSERT_FALSE(anchors.glyphs.empty());
    ASSERT_TRUE(apply(*doc, SW::StrokeWidthIntentKind::AbsoluteCssPx, 8));
    auto after = xml(*doc);
    EXPECT_EQ(doc->getObjectById("a")->style->stroke_width.computed, 4);
    EXPECT_EQ(doc->getObjectById("b")->style->stroke_width.computed, 8);
    for (auto id : {"a", "b"}) {
        auto const &dash = doc->getObjectById(id)->style->stroke_dasharray.values;
        ASSERT_EQ(dash.size(), 2u);
        EXPECT_EQ(dash[0].computed, 8); EXPECT_EQ(dash[1].computed, 16);
    }
    EXPECT_EQ(doc->getObjectById("a")->style->stroke_dashoffset.computed, -4);
    EXPECT_TRUE(doc->getObjectById("a")->style->stroke_width.important);
    EXPECT_EQ(geometry(*doc), anchors);
    EXPECT_EQ(outside(after, "stroke-width|stroke-dasharray|stroke-dashoffset"),
              outside(before, "stroke-width|stroke-dasharray|stroke-dashoffset"));
    EXPECT_EQ(doc->getSelection()->singleItem(), doc->getObjectById("selected"));
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(*doc), before);
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_EQ(xml(*doc), after);
    EXPECT_FALSE(apply(*doc, SW::StrokeWidthIntentKind::AbsoluteCssPx, 8));
    EXPECT_EQ(xml(*doc), after);
}
TEST_F(StrokeObjectProperties, NonePreservesEveryOtherDeclarationAndHairline)
{
    auto doc = fixture(); ASSERT_TRUE(doc);
    ASSERT_TRUE(apply(*doc, SW::StrokeWidthIntentKind::Hairline));
    for (auto id : {"a", "b"}) {
        EXPECT_EQ(doc->getObjectById(id)->style->stroke_width.computed, 1);
        EXPECT_TRUE(doc->getObjectById(id)->style->vector_effect.stroke);
    }
    DocumentUndo::clearUndo(doc.get());
    auto before = xml(*doc); auto anchors = geometry(*doc);
    ASSERT_TRUE(apply(*doc, SW::StrokeWidthIntentKind::RemoveStroke));
    auto after = xml(*doc);
    for (auto id : {"a", "b"}) EXPECT_TRUE(doc->getObjectById(id)->style->stroke.isNone());
    EXPECT_EQ(outside(before, "stroke"), outside(after, "stroke"));
    EXPECT_EQ(geometry(*doc), anchors);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(*doc), before);
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_EQ(xml(*doc), after);
}
TEST_F(StrokeObjectProperties, TwoDiscreteGesturesTwoStepsAndStylesheetPriority)
{
    auto doc = fixture("<style>.local{stroke-width:2 !important;fill:#008000}</style>"); ASSERT_TRUE(doc);
    // Let the stylesheet win over an ordinary inline declaration.
    auto *a = doc->getObjectById("a");
    std::string style = a->getRepr()->attribute("style");
    style.erase(style.find(" !important"), 11);
    a->getRepr()->setAttribute("style", style.c_str());
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), RC_("Undo", "Prepare stylesheet fixture"), "");
    DocumentUndo::clearUndo(doc.get());
    auto before = xml(*doc);
    auto const a_style = std::string(a->getRepr()->attribute("style"));
    ASSERT_TRUE(apply(*doc, SW::StrokeWidthIntentKind::AbsoluteCssPx, 8)); auto first = xml(*doc);
    EXPECT_EQ(a->style->stroke_width.computed, 2);
    EXPECT_EQ(a->getRepr()->attribute("style"), a_style);
    EXPECT_EQ(doc->getObjectById("b")->style->stroke_width.computed, 8);
    ASSERT_TRUE(apply(*doc, SW::StrokeWidthIntentKind::AbsoluteCssPx, 10));
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(*doc), first);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(*doc), before);
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
}
TEST_F(StrokeObjectProperties, RebindingAndDeletionDuringWriteRollBack)
{
    for (bool deletion : {false, true}) {
        auto doc = fixture(); ASSERT_TRUE(doc); auto before = xml(*doc);
        bool live = true;
        Callback callback(doc->getObjectById("a")->getRepr(), [&] {
            if (deletion) doc->getObjectById("owner")->deleteObject();
            else live = false;
        });
        EXPECT_FALSE(apply(*doc, SW::StrokeWidthIntentKind::AbsoluteCssPx, 8, [&] { return live; }));
        EXPECT_TRUE(callback.fired);
        EXPECT_EQ(xml(*doc), before);
        EXPECT_FALSE(DocumentUndo::undo(doc.get()));
    }
}
} // namespace
