// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <gtkmm/window.h>
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "extension/init.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-text.h"
#include "preferences.h"
#include "selection.h"
#include "style.h"
#include "ui/tools/text-tool.h"
#include "ui/widget/canvas.h"
#include "ui/widget/dash-selector.h"
#include "ui/widget/gtk-registry.h"
#include "ui/widget/stroke-style.h"
#include "xml/repr.h"
using namespace Inkscape;
namespace {
template<class T> T *find(Gtk::Widget &w) {
    if (auto *p = dynamic_cast<T *>(&w)) return p;
    for (auto *c = w.get_first_child(); c; c = c->get_next_sibling()) if (auto *p = find<T>(*c)) return p;
    return nullptr;
}
class DashText : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        g_setenv("INKSCAPE_TEST_GUI", "1", TRUE);
        g_setenv("INKSCAPE_APP_ID_TAG", "strokeDashTextTest", TRUE);
        static auto *app = new InkscapeApplication();
        ASSERT_TRUE(app->gtk_app());
        UI::Widget::register_all(); Extension::init();
        if (!Application::exists()) Application::create(false);
        Preferences::get()->setBool("/options/svgoutput/sort_attributes", false);
    }
    std::unique_ptr<SPDocument> doc;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<UI::Widget::StrokeStyle> panel;
    Gtk::Box inner, middle, outer;
    Gtk::Window host, canvas;
    void open(std::string const &body, std::vector<std::string> const &ids = {"owner"}) {
        doc = SPDocument::createNewDocFromMem(R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="200" height="100">)" + body + "</svg>");
        ASSERT_TRUE(doc); doc->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(doc->getNamedView()); canvas.set_child(*desktop->getCanvas());
        std::vector<SPItem *> selected;
        for (auto const &id : ids) selected.push_back(cast<SPItem>(doc->getObjectById(id)));
        desktop->getSelection()->setList(selected); Application::instance().add_desktop(desktop.get());
        panel = std::make_unique<UI::Widget::StrokeStyle>();
        inner.append(*panel); middle.append(inner); outer.append(middle); host.set_child(outer);
        host.insert_action_group("doc", doc->getActionGroup()); panel->setDesktop(desktop.get()); panel->updateLine();
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture setup"), "");
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    }
    void TearDown() override {
        if (!desktop) return;
        desktop->setTool("/tools/select"); panel->setDesktop(nullptr);
        host.insert_action_group("doc", {}); host.unset_child();
        inner.remove(*panel); middle.remove(inner); outer.remove(middle); panel.reset();
        canvas.unset_child(); Application::instance().remove_desktop(desktop.get()); desktop.reset(); doc.reset();
    }
    std::string xml() { return sp_repr_save_buf(doc->getReprDoc()).raw(); }
    void apply(bool scaling) {
        Preferences::get()->setBool("/options/dash/scale", scaling);
        auto *selector = find<UI::Widget::DashSelector>(*panel); ASSERT_TRUE(selector);
        selector->set_dash_pattern({2, 3}, -1); selector->changed_signal.emit(UI::Widget::DashSelector::Dash);
        doc->ensureUpToDate();
    }
    void expected(char const *id, double width, bool scaling) {
        auto *s = doc->getObjectById(id)->style; ASSERT_TRUE(s);
        EXPECT_EQ(s->stroke_width.computed, width);
        double const factor = scaling ? width : 1;
        if (factor == 0) EXPECT_TRUE(s->stroke_dasharray.values.empty()); // SVG zero pattern canonicalizes to solid
        else { ASSERT_EQ(s->stroke_dasharray.values.size(), 2u);
            EXPECT_EQ(s->stroke_dasharray.values[0].computed, 2 * factor);
            EXPECT_EQ(s->stroke_dasharray.values[1].computed, 3 * factor); }
        EXPECT_EQ(s->stroke_dashoffset.computed, -factor);
    }
    void undo(std::string const &before) {
        auto const after = xml(); ASSERT_NE(before, after);
        EXPECT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(), before);
        EXPECT_FALSE(DocumentUndo::undo(doc.get())); EXPECT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_EQ(xml(), after);
    }
};
TEST_F(DashText, PerSourceWidthsPatternsTransformsPrioritiesAndUndo) {
    for (bool scaling : {true, false}) for (auto transform : {"scale(2)", "scale(2,3)", "skewX(15)", "scale(-2,2)"}) {
        SCOPED_TRACE(transform);
        SCOPED_TRACE(scaling);
        open(std::string(R"(<style>.line{fill:green;font-size:17px}</style><g id="owner" style="stroke-width:91"><text id="text" transform=")") + transform + R"(" style="font-family:sans-serif;stroke:black;stroke-width:77"><tspan id="a" sodipodi:role="line" x="11.2345" y="20.6789" class="line" style="stroke-width:2 !important;stroke-dasharray:9,8 !important;stroke-dashoffset:7">AB</tspan><tspan id="b" sodipodi:role="line" x="11.2345" y="40.6789" style="stroke-width:5;stroke-dasharray:1,4;stroke-dashoffset:3 !important;vector-effect:non-scaling-stroke">CD</tspan><tspan id="z" style="stroke-width:0">E</tspan><tspan id="h" style="stroke-width:1;-inkscape-stroke:hairline;vector-effect:non-scaling-stroke">F</tspan></text></g>)");
        auto *layout = &cast<SPText>(doc->getObjectById("text"))->layout;
        std::vector<Geom::Point> anchors;
        for (auto it = layout->begin(); it != layout->end(); it.nextCharacter()) anchors.push_back(layout->characterAnchorPoint(it));
        auto const before = xml(); auto const defaults = Preferences::get()->getString("/desktop/style");
        apply(scaling);
        unsigned index = 0;
        for (auto it = layout->begin(); it != layout->end(); it.nextCharacter()) EXPECT_EQ(layout->characterAnchorPoint(it), anchors.at(index++));
        EXPECT_EQ(index, anchors.size());
        EXPECT_STREQ(doc->getObjectById("a")->getAttribute("x"), "11.2345");
        EXPECT_STREQ(doc->getObjectById("a")->getAttribute("y"), "20.6789");
        expected("a", 2, scaling); expected("b", 5, scaling); expected("z", 0, scaling); expected("h", 1, scaling);
        EXPECT_TRUE(doc->getObjectById("a")->style->stroke_dasharray.important);
        EXPECT_FALSE(doc->getObjectById("a")->style->stroke_dashoffset.important);
        EXPECT_FALSE(doc->getObjectById("b")->style->stroke_dasharray.important);
        EXPECT_TRUE(doc->getObjectById("b")->style->stroke_dashoffset.important);
        EXPECT_TRUE(doc->getObjectById("h")->style->stroke_extensions.hairline);
        EXPECT_EQ(Preferences::get()->getString("/desktop/style"), defaults); undo(before); TearDown();
    }
}
TEST_F(DashText, SelectionOrderAndIndividualControls) {
    std::string const body = R"svg(<text id="a" transform="scale(2)" style="stroke:black;stroke-width:2">A</text><text id="b" transform="scale(3)" style="stroke:black;stroke-width:5;vector-effect:non-scaling-stroke">B</text>)svg";
    for (bool scaling : {true, false}) {
        open(body, {"a", "b"}); apply(scaling); auto const forward = xml(); TearDown();
        open(body, {"b", "a"}); apply(scaling); EXPECT_EQ(xml(), forward); TearDown();
        for (auto id : {"a", "b"}) { open(body, {id}); apply(scaling); expected(id, std::string(id) == "a" ? 2 : 5, scaling); TearDown(); }
    }
}
TEST_F(DashText, RangeRefusesCaretAppliesAndNoopKeepsRedo) {
    open(R"(<text id="owner" style="stroke:black;stroke-width:2;stroke-dasharray:4px,6px;stroke-dashoffset:-2px"><tspan id="a">AB</tspan><tspan id="b" style="stroke-width:5">CD</tspan></text>)");
    desktop->setTool("/tools/text"); auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool()); ASSERT_TRUE(tool);
    auto *text = cast<SPText>(doc->getObjectById("owner")); tool->text_sel_start = text->layout.begin();
    tool->text_sel_end = tool->text_sel_start; tool->text_sel_end.nextCharacter();
    auto const before = xml(); auto const defaults = Preferences::get()->getString("/desktop/style");
    apply(true); EXPECT_EQ(xml(), before); EXPECT_FALSE(DocumentUndo::undo(doc.get()));
    std::swap(tool->text_sel_start, tool->text_sel_end); apply(true); EXPECT_EQ(xml(), before);
    tool->text_sel_end = tool->text_sel_start; apply(true); expected("a", 2, true); expected("b", 5, true);
    EXPECT_STREQ(doc->getObjectById("a")->getAttribute("style"), nullptr);
    EXPECT_EQ(Preferences::get()->getString("/desktop/style"), defaults); undo(before);
    auto const after = xml(); apply(true); EXPECT_EQ(xml(), after); // unchanged request adds no step
    EXPECT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_FALSE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate(); tool->text_sel_start = text->layout.begin();
    tool->text_sel_end = tool->text_sel_start; tool->text_sel_end.nextCharacter();
    apply(true); EXPECT_EQ(xml(), before); EXPECT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_EQ(xml(), after);
}
TEST_F(DashText, StylesheetImportantExcludesOwnerKeepsCompatibleShape) {
    open(R"(<style>.protected{stroke-dashoffset:8 !important}</style><g id="owner"><text id="text" style="stroke:black"><tspan id="a" class="protected">A</tspan><tspan id="b">B</tspan></text><rect id="r" width="10" height="10" style="stroke:black;stroke-width:3"/></g>)");
    auto const before = xml(); apply(true); expected("r", 3, true);
    EXPECT_STREQ(doc->getObjectById("b")->getAttribute("style"), nullptr);
    EXPECT_EQ(doc->getObjectById("a")->style->stroke_dashoffset.computed, 8); undo(before);
}
} // namespace
