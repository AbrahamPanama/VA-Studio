// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <regex>
#include <array>
#include <gtkmm/application.h>
#include <gtkmm/window.h>
#include <gtkmm/label.h>
#include "desktop.h"
#include "desktop-style.h"
#include "document.h"
#include "document-undo.h"
#include "extension/init.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-text.h"
#include "object/sp-item.h"
#include "selection.h"
#include "style.h"
#include "preferences.h"
#include "text-editing.h"
#include "ui/dialog/fill-and-stroke.h"
#include "ui/tools/text-tool.h"
#include "ui/widget/canvas.h"
#include "ui/widget/gtk-registry.h"
#include "ui/widget/selected-style.h"
#include "ui/widget/generic/popover-menu.h"
#include "xml/repr.h"
#include "xml/document.h"
#include "inkgc/gc-managed.h"
using namespace Inkscape;
namespace {
std::unique_ptr<SPDocument> parse(std::string const &body) {
    auto doc = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" xmlns:xlink="http://www.w3.org/1999/xlink" width="200" height="100">)" + body + "</svg>");
    if (doc) doc->ensureUpToDate();
    return doc;
}
std::string xml(SPDocument &doc) {
    auto const revision = doc.getReprDoc()->contentRevision();
    auto *copy = doc.getReprDoc()->duplicate(nullptr);
    auto result = sp_repr_save_buf(copy).raw(); GC::release(copy);
    EXPECT_EQ(doc.getReprDoc()->contentRevision(), revision);
    return result;
}
std::string outside(std::string const &input, std::string const &property) {
    std::regex const styles(R"re(\s+style="([^"]*)")re");
    std::regex const declaration("(^|;)(" + property + "):[^;]*;?");
    std::string result;
    size_t last = 0;
    for (std::sregex_iterator i(input.begin(), input.end(), styles), end; i != end; ++i) {
        result += input.substr(last, i->position() - last);
        auto value = std::regex_replace((*i)[1].str(), declaration, "$1");
        while (!value.empty() && value.back() == ';') value.pop_back();
        if (!value.empty()) result += " style=\"" + value + "\"";
        last = i->position() + i->length();
    }
    return result + input.substr(last);
}
struct Geometry {
    std::vector<Geom::Point> positions;
    std::vector<std::pair<FontInstance *, std::array<double, 12>>> glyphs;
    bool operator==(Geometry const &) const = default;
};
Geometry anchors(SPDocument &doc) {
    Geometry result;
    doc.ensureUpToDate();
    if (auto *layout = te_get_layout(cast<SPItem>(doc.getObjectById("owner")))) {
        for (auto it = layout->begin(); it != layout->end(); it.nextCharacter())
            result.positions.push_back(layout->characterAnchorPoint(it));
        for (auto const &glyph : layout->glyphs()) {
            auto const t = glyph.transform(*layout);
            result.glyphs.push_back({glyph.span(layout).font.get(),
                {double(glyph.glyph), double(glyph.in_character), double(glyph.hidden), double(glyph.orientation),
                 glyph.advance, glyph.vertical_scale, t[0], t[1], t[2], t[3], t[4], t[5]}});
        }
    }
    return result;
}
void roundtrip(SPDocument &doc, std::string const &before, std::string const &after) {
    ASSERT_NE(before, after);
    EXPECT_TRUE(DocumentUndo::undo(&doc));
    EXPECT_EQ(xml(doc), before);
    EXPECT_FALSE(DocumentUndo::undo(&doc)); // exactly one action
    EXPECT_TRUE(DocumentUndo::redo(&doc));
    EXPECT_EQ(xml(doc), after);
}
class StrokeLegacyRoutes : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        g_setenv("LANGUAGE", "en", TRUE);
        if (!Application::exists()) Application::create(false);
        Preferences::get()->setBool("/options/svgoutput/check_on_reading", false);
        Preferences::get()->setBool("/options/svgoutput/check_on_writing", false);
        Preferences::get()->setBool("/options/svgoutput/sort_attributes", false);
    }
};
TEST_F(StrokeLegacyRoutes, FrozenOriginsAcrossContainersAndStrokeOperations) {
    for (auto container : {"line", "flowPara", "flowDiv", "textPath"}) {
        for (auto origin : {"", "inline", "attribute", "class", "unrelated"}) {
            for (auto property : {"stroke", "paint-order", "stroke-linejoin", "stroke-linecap"}) {
                for (bool inherit : {false, true}) {
                    SCOPED_TRACE(std::string(container) + ":" + origin + ":" + property + (inherit ? ":inherit" : ""));
                    std::string const old = std::string(property) == "stroke" ? "#ff0000" :
                        std::string(property) == "paint-order" ? "stroke fill markers" :
                        std::string(property) == "stroke-linejoin" ? "bevel" : "square";
                    std::string const value = inherit ? "inherit" : std::string(property) == "stroke" ? "#0000ff" :
                        std::string(property) == "paint-order" ? "fill stroke markers" : "round";
                    std::string const declaration = std::string(property) + ":" + old;
                    std::string attributes = " id=\"line\"";
                    if (std::string(origin) == "inline") attributes += " style=\"" + declaration + "\"";
                    if (std::string(origin) == "attribute") attributes += " " + std::string(property) + "=\"" + old + "\"";
                    if (std::string(origin) == "class" || std::string(origin) == "unrelated") attributes += " class=\"local\"";
                    std::string const rules = "<style>.local{font-size:17px;fill:#008000;" +
                        (std::string(origin) == "class" ? declaration : "") + "}</style>";
                    std::string const tag = std::string(container) == "line" ? "tspan" : container;
                    if (tag == "tspan") attributes += " sodipodi:role=\"line\" x=\"11.2345\" y=\"20.6789\"";
                    if (tag == "textPath") attributes += " xlink:href=\"#path\"";
                    std::string body = rules + "<defs><path id=\"path\" d=\"M0 20h180\"/></defs>";
                    if (tag == "flowPara" || tag == "flowDiv")
                        body += "<flowRoot id=\"owner\"><flowRegion><rect width=\"180\" height=\"90\"/></flowRegion>";
                    else body += "<text id=\"owner\" x=\"10\" y=\"20\">";
                    body += "<" + tag + attributes + ">" + (tag == "flowDiv" ? "<flowPara>ABC</flowPara>" : "ABC") + "</" + tag + ">";
                    body += tag == "flowPara" || tag == "flowDiv" ? "</flowRoot>" : "</text>";
                    auto doc = parse(body); ASSERT_TRUE(doc);
                    auto *line = doc->getObjectById("line"); ASSERT_TRUE(line && line->style);
                    for (auto *p : line->style->properties())
                        if (std::string(origin) == "class" && p->name() == property) ASSERT_EQ(p->style_src, SPStyleSrc::STYLE_SHEET);
                    auto const font = line->style->font_size.computed;
                    auto const fill = line->style->fill.get_value();
                    auto const before = xml(*doc); auto const positions = anchors(*doc); ASSERT_FALSE(positions.glyphs.empty());
                    auto *css = sp_repr_css_attr_new(); sp_repr_css_set_property(css, property, value.c_str());
                    sp_desktop_apply_css_recursive(doc->getObjectById("owner"), css, true);
                    sp_repr_css_attr_unref(css); doc->ensureUpToDate();
                    auto const after = xml(*doc);
                    if (std::string(origin) == "class" || std::string(origin) == "unrelated") EXPECT_STREQ(line->getAttribute("class"), "local");
                    EXPECT_EQ(outside(before, property), outside(after, property));
                    EXPECT_EQ(anchors(*doc), positions);
                    EXPECT_EQ(line->style->font_size.computed, font);
                    EXPECT_EQ(line->style->fill.get_value(), fill);
                    EXPECT_EQ(bool(line->getAttribute("style")), std::string(origin) == "inline" ||
                        std::string(origin) == "attribute" || std::string(origin) == "class");
                    if (!inherit) {
                        if (std::string(property) == "stroke") EXPECT_EQ(line->style->stroke.get_value(), "#0000ff");
                        if (std::string(property) == "paint-order") EXPECT_EQ(line->style->paint_order.get_value(), "fill stroke markers");
                        if (std::string(property) == "stroke-linejoin") EXPECT_EQ(line->style->stroke_linejoin.computed, SP_STROKE_LINEJOIN_ROUND);
                        if (std::string(property) == "stroke-linecap") EXPECT_EQ(line->style->stroke_linecap.computed, SP_STROKE_LINECAP_ROUND);
                    } else if (std::string(origin) == "inline" || std::string(origin) == "attribute" || std::string(origin) == "class") {
                        auto *written = sp_repr_css_attr(line->getRepr(), "style");
                        EXPECT_STREQ(sp_repr_css_property(written, property, nullptr), "inherit");
                        sp_repr_css_attr_unref(written);
                    }
                    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Stroke route"), "");
                    roundtrip(*doc, before, after);
                }
            }
        }
    }
}
TEST_F(StrokeLegacyRoutes, PrioritiesProtectionReferencesAndUnset) {
    for (auto attributes : {"sodipodi:insensitive=\"true\"", "style=\"display:none\"", "class=\"important\""}) {
        SCOPED_TRACE(attributes);
        auto doc = parse("<style>.important {stroke:red !important}</style><text id=\"owner\"><tspan id=\"line\" " +
                         std::string(attributes) + ">ABC</tspan></text>"); ASSERT_TRUE(doc);
        if (std::string(attributes).starts_with("style")) ASSERT_TRUE(cast<SPItem>(doc->getObjectById("line"))->isHidden());
        if (std::string(attributes).starts_with("sodipodi")) ASSERT_FALSE(cast<SPItem>(doc->getObjectById("line"))->isSensitive());
        if (std::string(attributes).starts_with("class")) {
            ASSERT_STREQ(doc->getObjectById("line")->getAttribute("class"), "important");
            ASSERT_TRUE(doc->getObjectById("line")->style->stroke.important);
        }
        auto const before = xml(*doc); auto *css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "stroke", "blue");
        sp_desktop_apply_css_recursive(doc->getObjectById("owner"), css, true);
        EXPECT_EQ(xml(*doc), before) << attributes << "; sensitive=" << cast<SPItem>(doc->getObjectById("line"))->isSensitive()
            << "; source=" << int(doc->getObjectById("line")->style->stroke.style_src);
        sp_repr_css_attr_unref(css);
    }
    for (auto body : {R"(<g sodipodi:insensitive="true"><text id="owner">ABC</text></g>)",
                      R"(<text id="owner"><tref xlink:href="#source"/></text><text id="source">ABC</text>)",
                      R"(<g id="owner"><use xlink:href="#source"/></g><text id="source">ABC</text>)",
                      R"(<style>text[style]{font-size:30px}</style><text id="owner">ABC</text>)",
                      R"(<text id="owner"><tspan stroke="red">ABC</tspan></text>)"}) {
        auto doc = parse(body); auto const before = xml(*doc);
        auto *css = sp_repr_css_attr_new();
        if (std::string(body).find("stroke=\"red\"") != std::string::npos) sp_repr_css_unset_property(css, "stroke");
        else sp_repr_css_set_property(css, "stroke", "blue");
        sp_desktop_apply_css_recursive(doc->getObjectById("owner"), css, true);
        EXPECT_EQ(xml(*doc), before); sp_repr_css_attr_unref(css);
    }
    auto doc = parse(R"(<text id="owner"><tspan sodipodi:role="line" id="line" style="stroke:red !important;fill:green">ABC</tspan></text>)");
    auto *css = sp_repr_css_attr_new(); sp_repr_css_set_property(css, "stroke", "blue");
    sp_desktop_apply_css_recursive(doc->getObjectById("owner"), css, true);
    EXPECT_TRUE(doc->getObjectById("line")->style->stroke.important);
    EXPECT_EQ(doc->getObjectById("line")->style->stroke.get_value(), "blue");
    sp_repr_css_unset_property(css, "stroke");
    sp_desktop_apply_css_recursive(doc->getObjectById("owner"), css, true);
    EXPECT_FALSE(doc->getObjectById("line")->style->stroke.set);
    sp_repr_css_attr_unref(css);
}
template<typename T> void widgets(Gtk::Widget &root, std::vector<T *> &found) {
    if (auto *widget = dynamic_cast<T *>(&root)) found.push_back(widget);
    for (auto *child = root.get_first_child(); child; child = child->get_next_sibling()) widgets(*child, found);
}
class Statusbar : public UI::Widget::SelectedStyle {
public:
    void remove() {
        openStrokeWidthMenu(false);
        for (auto *widget : _popup_sw->get_items()) {
            auto *item = dynamic_cast<UI::Widget::PopoverMenuItem *>(widget);
            auto *label = item ? dynamic_cast<Gtk::Label *>(item->get_child()) : nullptr;
            if (label && label->get_text() == "Remove Stroke") { g_signal_emit_by_name(item->gobj(), "clicked"); return; }
        }
        FAIL() << "real Remove Stroke menu entry missing";
    }
};
class StrokeLegacyGui : public StrokeLegacyRoutes {
protected:
    static void SetUpTestSuite() {
        StrokeLegacyRoutes::SetUpTestSuite();
        g_setenv("INKSCAPE_APP_ID_TAG", "strokeLegacyRoutes", TRUE);
        // Match inkscape-main: PatternEditor/SwatchEditor request SearchEntry2
        // from GtkBuilder. The default SearchEntry wrapper cannot be cast to it.
        Gtk::Application::wrap_in_search_entry2();
        static auto *app = new InkscapeApplication();
        ASSERT_TRUE(app->gtk_app()); UI::Widget::register_all(); Extension::init();
    }
    void open(std::string const &body) {
        doc = parse(body);
        ASSERT_TRUE(doc);
        desktop = std::make_unique<SPDesktop>(doc->getNamedView());
        host = std::make_unique<Gtk::Window>(); host->set_child(*desktop->getCanvas());
        // A native text-style write updates its caret and scrolls if offscreen.
        // Supply the real viewport used by selector/pure-transform fixtures;
        // an unallocated canvas is empty and has no desktop chrome to scroll.
        desktop->getCanvas()->size_allocate(Gtk::Allocation(0, 0, 256, 256), -1);
        ASSERT_EQ(desktop->getCanvas()->get_dimensions(), Geom::Point(256, 256));
        desktop->set_display_area(desktop->doc2dt(Geom::Point(100, 50)), Geom::Point(128, 128), false);
        desktop->getSelection()->set(cast<SPItem>(doc->getObjectById("owner")));
        // StrokeStyle's constructor must see the live desktop, as in the row
        // and paint-order fixtures. Root the canvas and dialog before binding.
        Application::instance().add_desktop(desktop.get()); registered = true;
        ASSERT_EQ(SP_ACTIVE_DESKTOP, desktop.get());
        panel = std::make_unique<UI::Dialog::FillAndStroke>();
        panel_host = std::make_unique<Gtk::Window>(); panel_host->set_child(*panel);
        panel->setDesktop(desktop.get()); panel->showPageStrokePaint();
        statusbar = std::make_unique<Statusbar>(); statusbar->setDesktop(desktop.get());
        std::vector<UI::Widget::PaintSwitch *> switches; widgets(*panel, switches);
        ASSERT_EQ(switches.size(), 2u); stroke = switches[1];
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    }
    void TearDown() override {
        if (statusbar) statusbar->setDesktop(nullptr);
        statusbar.reset();
        if (panel) panel->setDesktop(nullptr);
        if (panel_host) panel_host->unset_child();
        panel.reset(); panel_host.reset(); stroke = nullptr;
        if (host) host->unset_child();
        host.reset();
        if (registered) Application::instance().remove_desktop(desktop.get());
        registered = false; desktop.reset(); doc.reset();
    }
    std::unique_ptr<SPDocument> doc;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<Gtk::Window> host, panel_host;
    std::unique_ptr<UI::Dialog::FillAndStroke> panel;
    std::unique_ptr<Statusbar> statusbar;
    UI::Widget::PaintSwitch *stroke = nullptr;
    bool registered = false;
};
TEST_F(StrokeLegacyGui, RealNoneAndStatusbarRemoveHaveIdenticalXmlAndSingleUndo) {
    open(R"(<text id="owner" x="10" y="20" style="stroke:red;stroke-width:3;stroke-dasharray:2,4;stroke-linejoin:bevel"><tspan id="line" sodipodi:role="line" stroke="green">ABC</tspan><tspan id="outside" stroke="black" fill="orange">DEF</tspan></text>)");
    ASSERT_TRUE(stroke);
    auto const before = xml(*doc); auto const positions = anchors(*doc);
    ASSERT_FALSE(positions.glyphs.empty());
    stroke->get_signal_mode_changed().emit(UI::Widget::PaintMode::None);
    doc->ensureUpToDate(); auto const after = xml(*doc);
    stroke->get_signal_mode_changed().emit(UI::Widget::PaintMode::None);
    EXPECT_EQ(xml(*doc), after); // an already-none command creates no second action
    EXPECT_EQ(outside(before, "stroke"), outside(after, "stroke")); EXPECT_EQ(anchors(*doc), positions);
    EXPECT_TRUE(doc->getObjectById("line")->style->stroke.isNone());
    roundtrip(*doc, before, after); ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    statusbar->remove();
    doc->ensureUpToDate(); EXPECT_EQ(xml(*doc), after); EXPECT_EQ(anchors(*doc), positions);
    roundtrip(*doc, before, after);
}
TEST_F(StrokeLegacyGui, SafeReversedRangeIsInterceptedAndKeepsOneUndoOwner) {
    // Native text-tool updateRepr serializes the owner style and first span's
    // position. Author this safe complete-leaf range in that form from load,
    // with the same width/dashes/join, so the exact XML oracle remains strict.
    // Removing role=line after load left an uncommitted setup change in Undo.
    open(R"(<text id="owner" x="10" y="20" style="stroke:red;stroke-width:3;stroke-linejoin:bevel;stroke-dasharray:2, 4"><tspan id="line" x="10" y="20" stroke="green">ABC</tspan><tspan id="outside" stroke="black" fill="orange">DEF</tspan></text>)");
    ASSERT_TRUE(stroke);
    desktop->setTool("/tools/text"); auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool()); ASSERT_TRUE(tool);
    auto *text = cast<SPText>(doc->getObjectById("owner"));
    tool->text_sel_end = text->layout.begin(); tool->text_sel_start = text->layout.charIndexToIterator(3);
    auto const before = xml(*doc); auto const positions = anchors(*doc);
    auto const untouched = doc->getObjectById("outside")->style->stroke.get_value();
    auto *css = sp_repr_css_attr_new(); sp_repr_css_set_property(css, "stroke", "blue");
    EXPECT_FALSE(sp_desktop_set_style(desktop.get(), css)); // tool owns completion
    sp_repr_css_attr_unref(css); doc->ensureUpToDate(); auto const after = xml(*doc);
    EXPECT_EQ(outside(before, "stroke"), outside(after, "stroke")); EXPECT_EQ(anchors(*doc), positions);
    EXPECT_EQ(doc->getObjectById("outside")->style->stroke.get_value(), untouched);
    EXPECT_EQ(text->layout.iteratorToCharIndex(tool->text_sel_start), 3u);
    EXPECT_EQ(text->layout.iteratorToCharIndex(tool->text_sel_end), 0u);
    roundtrip(*doc, before, after);
}
} // namespace
