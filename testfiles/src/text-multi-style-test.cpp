// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glibmm/keyfile.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <gtkmm/menubutton.h>
#include <gtkmm/expander.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/listview.h>
#include <gtkmm/window.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/dropdown.h>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "io/resource.h"
#include "object/sp-item.h"
#include "object/sp-text.h"
#include "object/sp-tspan.h"
#include "object/sp-flowtext.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "selection.h"
#include "style.h"
#include "style-text.h"
#include "text-editing.h"
#include "ui/dialog/text-panel.h"
#include "ui/text-font-preview.h"
#include "ui/text-target-utils.h"
#include "ui/toolbar/text-toolbar.h"
#include "ui/tools/text-tool.h"
#include "ui/widget/events/canvas-event.h"
#include "ui/widget/canvas.h"
#include "ui/widget/font-list.h"
#include "ui/widget/spinbutton.h"
#include "ui/widget/unit-menu.h"
#include "ui/text-style-controller.h"
#include "util/recently-used-fonts.h"
#include "xml/repr.h"
#include "util/font-discovery.h"

using namespace Inkscape;
using namespace std::literals;

namespace {

InkscapeApplication &testApplication()
{
    static auto application = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "textmultistyletest", true);
        auto result = new InkscapeApplication();
        result->gio_app()->register_application();
        // Preserve fatal failures (including shutdown) as test failures rather
        // than waiting in the application's interactive emergency dialog.
        for (auto signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) {
            std::signal(signal, SIG_DFL);
        }
#ifndef _WIN32
        std::signal(SIGBUS, SIG_DFL);
#endif
        return result;
    }();
    return *application;
}

void drainMainContext()
{
    // Bound diagnostics even if a broken idle keeps rescheduling itself.
    for (unsigned i = 0; i < 10000 && g_main_context_pending(nullptr); ++i) {
        g_main_context_iteration(nullptr, false);
    }
}

template <typename T>
T *findWidget(Gtk::Widget &root)
{
    if (auto found = dynamic_cast<T *>(&root)) return found;
    for (auto child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto found = findWidget<T>(*child)) return found;
    }
    return nullptr;
}

Gtk::Widget *fontTreeView(Gtk::Widget &root)
{
    if (GTK_IS_LIST_VIEW(root.gobj())) {
        auto selection = gtk_list_view_get_model(GTK_LIST_VIEW(root.gobj()));
        if (GTK_IS_SINGLE_SELECTION(selection) &&
            GTK_IS_TREE_LIST_MODEL(gtk_single_selection_get_model(GTK_SINGLE_SELECTION(selection)))) {
            return &root;
        }
    }
    for (auto child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto found = fontTreeView(*child)) return found;
    }
    return nullptr;
}

bool queueFontKeyboardPreview(Gtk::Widget &fonts)
{
    auto view = fontTreeView(fonts);
    if (!view) return false;
    auto controllers = gtk_widget_observe_controllers(view->gobj());
    bool emitted = false;
    for (unsigned i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        auto controller = G_OBJECT(g_list_model_get_item(controllers, i));
        if (GTK_IS_EVENT_CONTROLLER_KEY(controller) &&
            gtk_event_controller_get_propagation_phase(GTK_EVENT_CONTROLLER(controller)) == GTK_PHASE_CAPTURE) {
            gboolean handled = false;
            g_signal_emit_by_name(controller, "key-pressed", GDK_KEY_Down, 0u,
                                  GdkModifierType(0), &handled);
            emitted = true;
        }
        g_object_unref(controller);
        if (emitted) break;
    }
    g_object_unref(controllers);
    return emitted;
}

bool queueFontPointerLeave(Gtk::Widget &root)
{
    if (g_object_get_data(G_OBJECT(root.gobj()), "inkscape-font-list-item")) {
        auto controllers = gtk_widget_observe_controllers(root.gobj());
        bool emitted = false;
        for (unsigned i = 0; i < g_list_model_get_n_items(controllers); ++i) {
            auto controller = G_OBJECT(g_list_model_get_item(controllers, i));
            if (GTK_IS_EVENT_CONTROLLER_MOTION(controller)) {
                g_signal_emit_by_name(controller, "leave");
                emitted = true;
            }
            g_object_unref(controller);
            if (emitted) break;
        }
        g_object_unref(controllers);
        if (emitted) return true;
    }
    for (auto child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (queueFontPointerLeave(*child)) return true;
    }
    return false;
}

class TextMultiStyleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "Skipping GUI integration test: GUI testing not enabled";
        }
        auto &application = testApplication();
        ASSERT_TRUE(application.gtk_app());
        ASSERT_TRUE(Application::exists());

        constexpr auto svg = R"(<svg xmlns="http://www.w3.org/2000/svg"
              xmlns:xlink="http://www.w3.org/1999/xlink"
              xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
              xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <sodipodi:namedview id="namedview"/>
  <g id="group">
  <text id="first" style="font-family:serif;font-size:14px;font-weight:bold;fill:#123456">
    <tspan id="first-run" style="font-family:monospace;font-size:22px;font-style:italic;font-variant-caps:small-caps">First</tspan>
  </text>
  <g id="nested"><text id="second" style="font-family:cursive;font-size:31px;font-stretch:condensed;font-feature-settings:'liga' 0">Second</text></g>
  <rect id="vector" width="10" height="10" style="fill:#ff0000"/>
  </g>
  <g id="rich-group">
    <text id="rich" x="10" y="100" xml:lang="es" style="font-family:serif;font-size:12px;line-height:150%;letter-spacing:2px;word-spacing:3px;fill:#234567;stroke:#345678;opacity:0.8;-inkscape-font-specification:'serif Bold'">a<tspan id="rich-24" style="font-size:24px;font-weight:bold;text-decoration-line:underline overline;font-feature-settings:'liga' 0, 'ss01' 1;font-variation-settings:'wght' 650">b<tspan id="rich-36" style="font-size:36px;font-style:oblique;font-stretch:condensed;font-variant-caps:small-caps;font-variant-position:super;baseline-shift:3px">c</tspan>d</tspan>e</text>
    <g style="display:none"><text id="hidden">hidden</text></g>
    <g style="visibility:hidden"><text id="invisible">invisible</text></g>
    <g sodipodi:insensitive="1"><text id="locked">locked</text></g>
    <use id="clone" xlink:href="#first"/>
    <path id="outline" d="M0,0 L20,0 L20,20 Z"/>
    <image id="image" width="10" height="10"/>
    <defs><text id="definition">definition</text></defs>
    <clipPath id="clip"><text id="clip-text">clip</text></clipPath>
    <mask id="mask"><text id="mask-text">mask</text></mask>
    <symbol id="symbol"><text id="symbol-text">symbol</text></symbol>
  </g>
  <g id="unsupported"><path d="M0,0 L10,10"/><use xlink:href="#first"/></g>
  <g id="flow-group"><flowRoot id="flow" style="font-family:serif;font-size:12px"><flowRegion><rect id="flow-region" width="200" height="100"/></flowRegion><flowPara id="flow-para">Flow text</flowPara></flowRoot></g>
  <path id="baseline" d="M0,180 L400,180"/>
  <g id="path-group"><text id="path-text" style="font-family:serif;font-size:12px"><textPath xlink:href="#baseline">Path café í שלום</textPath></text></g>
  <g id="inline-group"><text id="inline-text" x="10" y="200" style="font-family:serif;font-size:12px;inline-size:100px;white-space:pre-wrap">Wrapped café í שלום words words words</text></g>
  <text id="line-text" x="200" y="300" style="font-family:sans-serif;font-size:20px"><tspan id="line-one" sodipodi:role="line" x="200" y="300">First line</tspan><tspan id="line-two" sodipodi:role="line" x="200" y="330">Second line</tspan></text>
  <text id="line-text-offset" x="200" y="400" style="font-family:sans-serif;font-size:20px"><tspan id="offset-one" sodipodi:role="line" x="200" y="400">First line</tspan><tspan id="offset-two" sodipodi:role="line" x="260" y="430">Second longer line</tspan></text>
  <text id="line-text-four" x="200" y="500" style="font-family:sans-serif;font-size:20px"><tspan id="four-one" sodipodi:role="line" x="200" y="500">First line</tspan><tspan id="four-two" sodipodi:role="line" x="200" y="530">Second line</tspan><tspan id="four-three" sodipodi:role="line" x="200" y="560">Third line</tspan><tspan id="four-four" sodipodi:role="line" x="200" y="590">Fourth line</tspan></text>
  <text id="wrapped-paragraphs" x="10" y="600" style="font-family:sans-serif;font-size:20px;line-height:30px;inline-size:120px;white-space:pre-wrap"><tspan id="wrapped-para-one" sodipodi:role="paragraph" style="line-height:30px">First authored paragraph wraps across several visual lines.</tspan><tspan id="wrapped-para-two" sodipodi:role="paragraph" style="line-height:30px">Second authored paragraph also wraps across lines.</tspan></text>
  <text id="direct-root-line" x="200" y="700" style="font-family:sans-serif;font-size:20px;line-height:50px;fill:#123456;white-space:pre">First line
<tspan id="direct-root-second" x="200" y="730" style="font-weight:bold;fill:#123456">Second line</tspan></text>
  <text id="wrapped-line" x="200" y="700" style="font-family:sans-serif;font-size:20px;line-height:50px;fill:#123456;white-space:pre"><tspan id="wrapped-first" x="200" y="700">First line
</tspan><tspan id="wrapped-second" x="200" y="730" style="font-weight:bold;fill:#123456">Second line</tspan></text>
</svg>)"sv;
        auto owned_document = SPDocument::createNewDocFromMem(svg);
        document = application.document_add(std::move(owned_document));
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        first = cast<SPText>(document->getObjectById("first"));
        second = cast<SPText>(document->getObjectById("second"));
        vector = cast<SPItem>(document->getObjectById("vector"));
        ASSERT_TRUE(first && second && vector);

        desktop = application.createDesktop(document, false, true);
        ASSERT_TRUE(desktop);
        desktop->getSelection()->setList(std::vector<SPItem *>{first, vector, second});
        drainMainContext();
        checkpoint();
    }

    void TearDown() override
    {
        if (document) document->setModifiedSinceSave(false);
        if (desktop) testApplication().destroyDesktop(desktop);
        desktop = nullptr;
        document = nullptr;
    }

    void expectNormalized(SPObject *object, double size) const
    {
        ASSERT_TRUE(object);
        ASSERT_TRUE(object->style);
        EXPECT_NEAR(object->style->font_size.computed, size, 0.01);
        EXPECT_EQ(object->style->font_weight.computed, 400);
        EXPECT_EQ(object->style->font_style.computed, SP_CSS_FONT_STYLE_NORMAL);
        EXPECT_EQ(object->style->font_stretch.computed, SP_CSS_FONT_STRETCH_NORMAL);
    }

    SPItem *item(char const *id) const { return cast<SPItem>(document->getObjectById(id)); }

    std::string xml() const { return sp_repr_save_buf(document->getReprDoc()).raw(); }

    void checkpoint()
    {
        // clearUndo alone does not finish the XML transaction. Otherwise the
        // first edit/Undo also owns startup metadata or fixture transforms.
        document->ensureUpToDate();
        DocumentUndo::done(document, RC_("Undo", "Prepare text fixture"), "draw-text");
        DocumentUndo::clearUndo(document);
        DocumentUndo::clearRedo(document);
        document->setModifiedSinceSave(false);
    }

    std::string subtree(char const *id) const
    {
        return sp_repr_write_buf(document->getObjectById(id)->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    }

    static Glib::ustring nonFamilyStyle(SPObject *object)
    {
        auto css = sp_repr_css_attr_new();
        sp_repr_css_attr_add_from_string(css, object->style->write(SP_STYLE_FLAG_ALWAYS).c_str());
        sp_repr_css_unset_property(css, "font-family");
        sp_repr_css_unset_property(css, "-inkscape-font-specification");
        Glib::ustring result;
        sp_repr_css_write_string(css, result);
        sp_repr_css_attr_unref(css);
        return result;
    }

    static FontChoice choice(Glib::ustring const &family = "sans-serif")
    {
        FontChoice result;
        result.phase = FontChoicePhase::Commit;
        result.origin = FontChoiceOrigin::Keyboard;
        result.family = family;
        // Face-specific FontList rows are deliberately included in Font only tests.
        result.face = "Regular";
        result.fontspec = get_fontspec(family, *result.face);
        result.available = true;
        return result;
    }

    void selectGroup(char const *id = "group") { desktop->getSelection()->set(item(id)); }

    static void expectSameBounds(Geom::OptRect const &a, Geom::OptRect const &b)
    {
        ASSERT_EQ(bool(a), bool(b));
        if (!a) return;
        for (unsigned axis = 0; axis < 2; ++axis) {
            EXPECT_NEAR(a->min()[axis], b->min()[axis], 1e-6);
            EXPECT_NEAR(a->max()[axis], b->max()[axis], 1e-6);
        }
    }

    // Drive the real TextPanel centre control on a fixture whose root already
    // authors centre alignment while its first hard line authors left, then
    // assert the panel resolves the conflict and a repeated request is a no-op.
    void expectPanelCenterResolvesRootCenterChildLeft(bool style_variant);

    // Same, but the first hard line authors only a conflicting text-anchor while
    // text-align stays inherited centre. The computed text-align alone reads as
    // settled centre; only the anchor reveals that the line renders uncentred.
    void expectPanelCenterResolvesRootCenterChildAnchorOnly(bool style_variant);

    SPDocument *document = nullptr;
    SPDesktop *desktop = nullptr;
    SPText *first = nullptr;
    SPText *second = nullptr;
    SPItem *vector = nullptr;
};

TEST_F(TextMultiStyleTest, NormalizesEverySelectedTextRunAndIgnoresVectors)
{
    UI::TextStylePatch patch;
    patch.family = "sans-serif";
    patch.face = "Regular";
    patch.fontspec = get_fontspec(*patch.family, *patch.face);
    patch.font_size_px = 40.0;

    auto const vector_style_before = std::string{vector->getRepr()->attribute("style")};
    ASSERT_TRUE(desktop->textStyleController().commit(
        patch, "test:multi-text", RC_("Undo", "Normalize selected text")));
    document->ensureUpToDate();

    expectNormalized(first, 40.0);
    expectNormalized(first->firstChild(), 40.0);
    expectNormalized(second, 40.0);
    EXPECT_EQ(vector->getRepr()->attribute("style"), vector_style_before);

    auto first_run = cast<SPTSpan>(first->firstChild());
    ASSERT_TRUE(first_run);
    EXPECT_EQ(first_run->style->font_variant_caps.computed, SP_CSS_FONT_VARIANT_CAPS_SMALL);
    EXPECT_STREQ(second->style->font_feature_settings.value(), "'liga' 0");
    ASSERT_TRUE(first->style->fill.isColor());
    EXPECT_EQ(first->style->fill.getColor().toRGBA(), 0x123456ff);
}

TEST_F(TextMultiStyleTest, OneUndoAndRedoCoverAllSelectedTextObjects)
{
    UI::TextStylePatch patch;
    patch.family = "sans-serif";
    patch.face = "Regular";
    patch.fontspec = get_fontspec(*patch.family, *patch.face);
    patch.font_size_px = 36.0;

    ASSERT_TRUE(desktop->textStyleController().commit(
        patch, "test:multi-text", RC_("Undo", "Normalize selected text")));
    ASSERT_TRUE(DocumentUndo::undo(document));
    EXPECT_FALSE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_NEAR(first->style->font_size.computed, 14.0, 0.01);
    EXPECT_NEAR(second->style->font_size.computed, 31.0, 0.01);

    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    expectNormalized(first, 36.0);
    expectNormalized(second, 36.0);
}

TEST_F(TextMultiStyleTest, ReapplyingTheExactUniformStyleCreatesNoUndoEntry)
{
    UI::TextStylePatch patch;
    patch.family = "sans-serif";
    patch.face = "Regular";
    patch.fontspec = get_fontspec(*patch.family, *patch.face);
    patch.font_size_px = 28.0;

    ASSERT_TRUE(desktop->textStyleController().commit(
        patch, "test:multi-text", RC_("Undo", "Normalize selected text")));
    auto const snapshot = desktop->textStyleController().query();
    ASSERT_TRUE(snapshot.family.valid);
    ASSERT_TRUE(snapshot.face.valid);
    EXPECT_FALSE(snapshot.family.mixed);
    EXPECT_FALSE(snapshot.face.mixed);
    EXPECT_EQ(snapshot.family.value, *patch.family);
    EXPECT_FALSE(desktop->textStyleController().commit(
        patch, "test:multi-text", RC_("Undo", "Normalize selected text")));
    EXPECT_TRUE(DocumentUndo::undo(document));
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(TextMultiStyleTest, RecursivelyCollectsOnlyEligibleRootsAndDeduplicates)
{
    auto targets = UI::collectTextItems({item("group"), first, item("nested"), second,
                                       item("rich-group"), item("rich")});
    ASSERT_EQ(targets.size(), 3);
    EXPECT_EQ(targets[0], first);
    EXPECT_EQ(targets[1], second);
    EXPECT_EQ(targets[2], item("rich"));
    for (auto id : {"hidden", "invisible", "locked", "definition", "clip-text", "mask-text", "symbol-text"}) {
        EXPECT_FALSE(UI::isEligibleTextItem(item(id))) << id;
        EXPECT_TRUE(UI::collectTextItems({item(id)}).empty()) << id;
    }
    EXPECT_TRUE(UI::collectTextItems({item("unsupported")}).empty());
}

TEST_F(TextMultiStyleTest, FamilyOnlyPreservesAllRunPropertiesAndExcludedArtwork)
{
    selectGroup("rich-group");
    std::map<std::string, Glib::ustring> before;
    for (auto id : {"rich", "rich-24", "rich-36"}) before[id] = nonFamilyStyle(item(id));
    std::map<std::string, std::string> excluded;
    for (auto id : {"hidden", "invisible", "locked", "clone", "outline", "image", "definition",
                    "clip-text", "mask-text", "symbol-text", "first"}) excluded[id] = subtree(id);
    auto const group_style = item("rich-group")->getAttribute("style");
    auto &controller = desktop->textStyleController();
    ASSERT_TRUE(controller.requestFontChoice(choice(), UI::FontChoicePolicy::FamilyOnly));
    document->ensureUpToDate();
    for (auto id : {"rich", "rich-24", "rich-36"}) {
        EXPECT_STREQ(item(id)->style->font_family.value(), "sans-serif");
        EXPECT_EQ(nonFamilyStyle(item(id)), before.at(id)) << id;
    }
    EXPECT_DOUBLE_EQ(item("rich")->style->font_size.computed, 12);
    EXPECT_DOUBLE_EQ(item("rich-24")->style->font_size.computed, 24);
    EXPECT_DOUBLE_EQ(item("rich-36")->style->font_size.computed, 36);
    EXPECT_EQ(item("rich-group")->getAttribute("style"), group_style);
    for (auto const &[id, xml] : excluded) EXPECT_EQ(subtree(id.c_str()), xml) << id;
    EXPECT_EQ(desktop->getSelection()->singleItem(), item("rich-group"));
}

TEST_F(TextMultiStyleTest, GroupAndDirectSelectionsHaveIdenticalPatchesUnderParentTransforms)
{
    for (auto transform : {"translate(15,25)", "scale(2)", "scale(2,3)",
                           "rotate(35)", "skewX(20)", "matrix(-2,0.3,0.4,3,10,20)"}) {
        item("group")->setAttribute("transform", transform);
        checkpoint();
        for (unsigned kind = 0; kind < 4; ++kind) {
            SCOPED_TRACE(std::string(transform) + " patch=" + std::to_string(kind));
            UI::TextStylePatch patch;
            if (kind != 2) patch.family = "sans-serif";
            if (kind == 1 || kind == 3) {
                patch.face = "Regular";
                patch.fontspec = get_fontspec(*patch.family, *patch.face);
            }
            if (kind >= 2) patch.font_size_px = 42;
            if (kind == 3) patch.underline = true;
            desktop->getSelection()->setList(std::vector<SPItem *>{first, second});
            auto &controller = desktop->textStyleController();
            auto const vector_xml = subtree("vector");
            ASSERT_TRUE(controller.commit(patch, "test:direct", RC_("Undo", "Style text")));
            document->ensureUpToDate();
            auto const direct_first = subtree("first");
            auto const direct_second = subtree("second");
            auto const direct_bounds = first->layout.bounds(Geom::Affine());
            ASSERT_TRUE(DocumentUndo::undo(document));
            document->ensureUpToDate();
            selectGroup();
            ASSERT_TRUE(controller.commit(patch, "test:group", RC_("Undo", "Style text")));
            document->ensureUpToDate();
            EXPECT_EQ(subtree("first"), direct_first);
            EXPECT_EQ(subtree("second"), direct_second);
            expectSameBounds(first->layout.bounds(Geom::Affine()), direct_bounds);
            EXPECT_EQ(subtree("vector"), vector_xml);
            EXPECT_STREQ(item("group")->getRepr()->attribute("transform"), transform);
            EXPECT_EQ(desktop->getSelection()->singleItem(), item("group"));
            ASSERT_TRUE(DocumentUndo::undo(document));
            document->ensureUpToDate();
        }
    }
}

TEST_F(TextMultiStyleTest, FamilyOnlyMultiGroupUndoRedoAndNoOpPreserveHistory)
{
    desktop->getSelection()->setList(std::vector<SPItem *>{item("group"), item("rich-group")});
    checkpoint();
    auto const original = xml();
    auto &controller = desktop->textStyleController();
    ASSERT_TRUE(controller.requestFontChoice(choice(), UI::FontChoicePolicy::FamilyOnly));
    auto const changed = xml();
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), original);
    EXPECT_FALSE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), changed);

    UI::TextStylePatch paint;
    paint.fill = "#abcdef";
    ASSERT_TRUE(controller.commit(paint, "test:paint", RC_("Undo", "Paint text")));
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    auto const before_no_op = xml();
    document->setModifiedSinceSave(false);
    EXPECT_FALSE(controller.requestFontChoice(choice(), UI::FontChoicePolicy::FamilyOnly));
    EXPECT_EQ(xml(), before_no_op);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_TRUE(DocumentUndo::redo(document));
}

TEST_F(TextMultiStyleTest, SameFamilyDoesNotCleanLegacyMetadataOrFlattenSpans)
{
    selectGroup("rich-group");
    auto const before = xml();
    auto &controller = desktop->textStyleController();
    document->setModifiedSinceSave(false);
    EXPECT_FALSE(controller.requestFontChoice(choice("serif"), UI::FontChoicePolicy::FamilyOnly));
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document));
}

TEST_F(TextMultiStyleTest, UnsupportedSelectionAndSingularTransformAreAtomic)
{
    auto &controller = desktop->textStyleController();
    selectGroup("unsupported");
    auto const before = xml();
    EXPECT_FALSE(controller.hasTargets());
    EXPECT_FALSE(controller.query().has_text_target);
    EXPECT_FALSE(controller.requestFontChoice(choice(), UI::FontChoicePolicy::FamilyOnly));
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document));

    item("nested")->setAttribute("transform", "scale(0,1)");
    checkpoint();
    selectGroup();
    auto const singular = xml();
    UI::TextStylePatch patch;
    patch.family = "sans-serif";
    EXPECT_FALSE(controller.commit(patch, "test:singular", RC_("Undo", "Style text")));
    EXPECT_EQ(xml(), singular);
    controller.preview(patch, UI::TextStyleOrigin::Direct);
    EXPECT_EQ(first->displayLayout(desktop->dkey), &first->layout);
    EXPECT_EQ(xml(), singular);
}

TEST_F(TextMultiStyleTest, FamilyOnlyLogicalRangePreservesUntargetedCharacters)
{
    selectGroup();
    auto &controller = desktop->textStyleController();
    auto const sibling = subtree("second");
    auto const original = xml();
    controller.begin({{SPWeakPtr<SPItem>(first), 1, 4, false}});
    UI::TextStylePatch patch;
    patch.family = "sans-serif";
    controller.preview(patch, UI::TextStyleOrigin::Direct);
    auto const preview = first->displayLayout(desktop->dkey)->bounds(Geom::Affine());
    EXPECT_EQ(xml(), original);
    ASSERT_TRUE(controller.commit(patch, "test:range", RC_("Undo", "Style text")));
    expectSameBounds(first->layout.bounds(Geom::Affine()), preview);
    EXPECT_EQ(subtree("second"), sibling);
    auto iter = first->layout.begin();
    for (unsigned i = 0; iter != first->layout.end(); ++i, iter.nextCharacter()) {
        SPObject *source = nullptr;
        first->layout.getSourceOfCharacter(iter, &source);
        while (source && !is<SPItem>(source)) source = source->parent;
        ASSERT_TRUE(source && source->style);
        EXPECT_STREQ(source->style->font_family.value(), i >= 1 && i < 4 ? "sans-serif" : "monospace");
        EXPECT_NEAR(source->style->font_size.computed, 22, 1e-6);
        EXPECT_EQ(source->style->font_style.computed, SP_CSS_FONT_STYLE_ITALIC);
    }
}

TEST_F(TextMultiStyleTest, TextToolSubstringTakesPrecedenceOverGroupedTargets)
{
    desktop->getSelection()->set(first);
    desktop->setTool("/tools/text");
    auto tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    ASSERT_EQ(tool->textItem(), first);
    tool->text_sel_start = first->layout.begin();
    tool->text_sel_start.nextCharacter();
    tool->text_sel_end = tool->text_sel_start;
    tool->text_sel_end.nextCharacter();
    auto const second_before = subtree("second");
    UI::TextStylePatch patch;
    patch.family = "sans-serif";
    auto &controller = desktop->textStyleController();
    controller.preview(patch, UI::TextStyleOrigin::Direct);
    ASSERT_NE(first->displayLayout(desktop->dkey), &first->layout);
    auto const preview = first->displayLayout(desktop->dkey)->bounds(Geom::Affine());
    ASSERT_TRUE(controller.commit(patch, "test:tool-range", RC_("Undo", "Style text")));
    expectSameBounds(first->layout.bounds(Geom::Affine()), preview);
    EXPECT_EQ(subtree("second"), second_before);
    EXPECT_STREQ(first->style->font_family.value(), "serif");
}

TEST_F(TextMultiStyleTest, TextToolReleasesImModuleSettingsHandlerWhenDestroyed)
{
    // BUG-014: GtkIMMulticontext drops its GtkSettings "notify::gtk-im-module"
    // handler only in set_client_widget(). A text tool destroyed without
    // unsetting its client widget left that handler on a freed context, and the
    // next input-language switch on Windows closed the application.
    auto *canvas = desktop->getCanvas();
    ASSERT_TRUE(canvas);
    auto *settings = gtk_widget_get_settings(GTK_WIDGET(canvas->Gtk::Widget::gobj()));
    ASSERT_TRUE(settings);
    guint const notify_id = g_signal_lookup("notify", G_TYPE_OBJECT);
    GQuark const detail = g_quark_from_static_string("gtk-im-module");
    auto const mask = static_cast<GSignalMatchType>(G_SIGNAL_MATCH_ID | G_SIGNAL_MATCH_DETAIL);
    auto const handlers = [&] {
        auto const count = g_signal_handlers_block_matched(settings, mask, notify_id, detail, nullptr, nullptr, nullptr);
        g_signal_handlers_unblock_matched(settings, mask, notify_id, detail, nullptr, nullptr, nullptr);
        return count;
    };
    // The text toolbar's own entries are created on first use and stay alive
    // with their input contexts; one warm-up round puts them in the baseline.
    desktop->setTool("/tools/text");
    desktop->setTool("/tools/select");
    auto const baseline = handlers();
    for (int round = 0; round < 3; ++round) {
        desktop->setTool("/tools/text");
        ASSERT_TRUE(dynamic_cast<UI::Tools::TextTool *>(desktop->getTool()));
        EXPECT_GT(handlers(), baseline) << "the text tool's input context follows IM module changes";
        desktop->setTool("/tools/select");
        EXPECT_EQ(handlers(), baseline) << "destroying the text tool must release its settings handler";
    }
    // Windows produces this notification on an input-language switch; it must
    // not reach a freed input context.
    g_object_notify(G_OBJECT(settings), "gtk-im-module");
}

TEST_F(TextMultiStyleTest, TextToolImClientWidgetFollowsCanvasRealizeAndUnrealize)
{
    // W1: moving the document tab to another window unrealizes the canvas and
    // realizes it in a new surface. GtkIMContextIME keeps a raw pointer to the
    // surface of the client widget, so the text tool must drop the client
    // widget on unrealize and set it again on realize, as GtkText does.
    // GtkIMMulticontext connects its GtkSettings "notify::gtk-im-module"
    // handler exactly while a client widget is set, so the handler count shows
    // the client widget state.
    auto *canvas = desktop->getCanvas();
    ASSERT_TRUE(canvas);
    auto *widget = GTK_WIDGET(canvas->Gtk::Widget::gobj());
    auto *settings = gtk_widget_get_settings(widget);
    ASSERT_TRUE(settings);
    guint const notify_id = g_signal_lookup("notify", G_TYPE_OBJECT);
    GQuark const detail = g_quark_from_static_string("gtk-im-module");
    auto const mask = static_cast<GSignalMatchType>(G_SIGNAL_MATCH_ID | G_SIGNAL_MATCH_DETAIL);
    auto const handlers = [&] {
        auto const count = g_signal_handlers_block_matched(settings, mask, notify_id, detail, nullptr, nullptr, nullptr);
        g_signal_handlers_unblock_matched(settings, mask, notify_id, detail, nullptr, nullptr, nullptr);
        return count;
    };
    auto *root = GTK_WIDGET(gtk_widget_get_root(widget));
    ASSERT_TRUE(root);
    gtk_widget_realize(root);
    gtk_widget_realize(widget);
    ASSERT_TRUE(gtk_widget_get_realized(widget));

    desktop->setTool("/tools/text");
    desktop->setTool("/tools/select");
    auto const baseline = handlers();
    desktop->setTool("/tools/text");
    ASSERT_TRUE(dynamic_cast<UI::Tools::TextTool *>(desktop->getTool()));
    auto const attached = handlers();
    ASSERT_GT(attached, baseline);

    for (int round = 0; round < 3; ++round) {
        gtk_widget_unrealize(widget); // the old window's surface goes away
        EXPECT_EQ(handlers(), baseline) << "the IM context must not keep the old surface's widget";
        gtk_widget_realize(widget); // realized in the new window
        EXPECT_EQ(handlers(), attached) << "the IM context follows the canvas into the new window";
    }

    // Destroying the tool while the canvas is unrealized must stay balanced.
    gtk_widget_unrealize(widget);
    desktop->setTool("/tools/select");
    EXPECT_EQ(handlers(), baseline);
    gtk_widget_realize(widget);
    EXPECT_EQ(handlers(), baseline);
    g_object_notify(G_OBJECT(settings), "gtk-im-module");
}

// T1: Undo after typing empties the text but keeps it alive. The tool's cursor
// iterators then pointed past the end of the layout when its listeners (the
// toolbar among them) read them: paragraphIndex and thisStartOfLine wrapped on
// the empty layout.
//
// The Text tool's own modified handler must validate the iterators before it
// emits the cursor-moved signal. With the toolbar attached its modified handler
// happens to re-validate them first through a style query, so the first case
// runs without a toolbar to isolate the tool.
TEST_F(TextMultiStyleTest, TextToolNotifiesWithValidIteratorsAfterUndoEmptiesTheText)
{
    unsigned notifications = 0;
    unsigned stale_notifications = 0;
    sigc::scoped_connection watch = desktop->connect_text_cursor_moved([&](UI::Tools::TextTool *moved) {
        ++notifications;
        auto const *moved_layout = moved->textItem() ? te_get_layout(moved->textItem()) : nullptr;
        if (!moved_layout) return;
        auto const size = moved_layout->iteratorToCharIndex(moved_layout->end());
        if (moved_layout->iteratorToCharIndex(moved->text_sel_start) > size ||
            moved_layout->iteratorToCharIndex(moved->text_sel_end) > size) {
            ++stale_notifications;
        }
    });

    desktop->getSelection()->clear();
    desktop->setTool("/tools/text");
    auto tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    checkpoint();
    tool->insertText("a");
    tool->insertText("b");
    drainMainContext();
    auto *typed = cast<SPText>(tool->textItem());
    ASSERT_TRUE(typed);
    auto const id = std::string{typed->getId()};
    ASSERT_EQ(typed->layout.iteratorToCharIndex(typed->layout.end()), 2);
    auto const before_undo = notifications;

    ASSERT_TRUE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::undo(document));
    // The text is empty but the cursor iterators still point at index 2.
    ASSERT_GT(typed->layout.iteratorToCharIndex(tool->text_sel_end), typed->layout.iteratorToCharIndex(typed->layout.end()));
    desktop->getSelection()->emitModified(); // what a live session delivers after the undo
    document->ensureUpToDate();
    drainMainContext();

    EXPECT_GT(notifications, before_undo);
    EXPECT_EQ(stale_notifications, 0u) << "cursor-moved was emitted with iterators past the end of the layout";
    auto *still = cast<SPText>(document->getObjectById(id));
    ASSERT_TRUE(still) << "the empty text stays alive";
    ASSERT_EQ(tool->textItem(), still);
    auto const *layout = &still->layout;
    EXPECT_EQ(layout->iteratorToCharIndex(layout->end()), 0);
    EXPECT_EQ(layout->iteratorToCharIndex(tool->text_sel_start), 0);
    EXPECT_EQ(layout->iteratorToCharIndex(tool->text_sel_end), 0);

    // Redo restores the typed characters and the iterators stay valid.
    ASSERT_TRUE(DocumentUndo::redo(document));
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    desktop->getSelection()->emitModified();
    drainMainContext();
    EXPECT_EQ(layout->iteratorToCharIndex(layout->end()), 2);
    EXPECT_EQ(stale_notifications, 0u);
}

// T1, toolbar side: TextToolbar::_cursorMoved must survive being handed
// iterators that are stale for an emptied layout and must not write a stale
// iterator back into the tool.
TEST_F(TextMultiStyleTest, TextToolbarCursorMovedSurvivesStaleIteratorsOnEmptiedText)
{
    Inkscape::UI::Toolbar::TextToolbar toolbar;
    toolbar.setDesktop(desktop);
    struct Detach {
        Inkscape::UI::Toolbar::TextToolbar &toolbar;
        ~Detach() { toolbar.setDesktop(nullptr); }
    } detach{toolbar};

    desktop->getSelection()->clear();
    desktop->setTool("/tools/text");
    auto tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    checkpoint();
    tool->insertText("a");
    tool->insertText("b");
    drainMainContext();
    auto *typed = cast<SPText>(tool->textItem());
    ASSERT_TRUE(typed);
    auto const id = std::string{typed->getId()};

    ASSERT_TRUE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::undo(document));
    auto const *layout = &typed->layout;
    ASSERT_EQ(layout->iteratorToCharIndex(layout->end()), 0);
    ASSERT_GT(layout->iteratorToCharIndex(tool->text_sel_end), 0) << "the iterators are stale";

    // Hand the toolbar the stale state directly, as the tool did before it validated.
    desktop->emit_text_cursor_moved(tool);
    EXPECT_EQ(layout->iteratorToCharIndex(tool->text_sel_start), 0) << "a stale start must never be written back";
    EXPECT_EQ(layout->iteratorToCharIndex(tool->text_sel_end), 0);

    document->ensureUpToDate();
    desktop->getSelection()->emitModified();
    drainMainContext();
    ASSERT_TRUE(document->getObjectById(id)) << "the empty text stays alive";
    EXPECT_EQ(tool->textItem(), typed);

    // Empty-layout iterator moves must not wrap around.
    auto probe = layout->end();
    EXPECT_FALSE(probe.thisStartOfLine());
    EXPECT_EQ(layout->iteratorToCharIndex(probe), 0);
}

// T3: a flowRoot without a frame shape (empty flowRegion) has no frame to
// clip the cursor-scroll test against.
TEST_F(TextMultiStyleTest, TextToolTypesIntoFlowRootWithoutFrameShape)
{
    auto *xml_doc = document->getReprDoc();
    auto *root_repr = xml_doc->createElement("svg:flowRoot");
    root_repr->setAttribute("id", "frameless-flow");
    auto *region = xml_doc->createElement("svg:flowRegion");
    root_repr->appendChild(region);
    auto *para = xml_doc->createElement("svg:flowPara");
    para->setAttribute("id", "frameless-para");
    root_repr->appendChild(para);
    document->getRoot()->getRepr()->appendChild(root_repr);
    Inkscape::GC::release(region);
    Inkscape::GC::release(para);
    Inkscape::GC::release(root_repr);
    document->ensureUpToDate();
    auto *flow = cast<SPFlowtext>(document->getObjectById("frameless-flow"));
    ASSERT_TRUE(flow);
    ASSERT_EQ(flow->get_frame(nullptr), nullptr);
    checkpoint();

    desktop->getSelection()->set(flow);
    desktop->setTool("/tools/text");
    auto tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    ASSERT_EQ(tool->textItem(), flow);
    tool->insertText("x");
    drainMainContext();
    document->ensureUpToDate();
    EXPECT_EQ(document->getObjectById("frameless-para")->getRepr()->firstChild()->content(), std::string{"x"});
}

// T4: hovering near a shape whose curve is empty must not dereference it.
TEST_F(TextMultiStyleTest, TextToolHoverNearEmptyAndNullCurveShapesIsHarmless)
{
    auto *xml_doc = document->getReprDoc();
    auto *empty_path = xml_doc->createElement("svg:path");
    empty_path->setAttribute("id", "empty-path");
    empty_path->setAttribute("d", "");
    empty_path->setAttribute("style", "fill:none;stroke:#000000;stroke-width:20");
    document->getRoot()->getRepr()->appendChild(empty_path);
    Inkscape::GC::release(empty_path);
    auto *lone_move = xml_doc->createElement("svg:path");
    lone_move->setAttribute("id", "lone-move");
    lone_move->setAttribute("d", "M 300,300");
    lone_move->setAttribute("style", "fill:none;stroke:#000000;stroke-width:20");
    document->getRoot()->getRepr()->appendChild(lone_move);
    Inkscape::GC::release(lone_move);
    document->ensureUpToDate();
    checkpoint();

    auto *empty_item = cast<SPShape>(document->getObjectById("empty-path"));
    auto *lone_item = cast<SPShape>(document->getObjectById("lone-move"));
    ASSERT_TRUE(empty_item && lone_item);

    desktop->getSelection()->clear();
    desktop->setTool("/tools/text");
    auto *tool = desktop->getTool();
    auto *text_tool = dynamic_cast<UI::Tools::TextTool *>(tool);
    ASSERT_TRUE(text_tool);

    // The hover code path itself, with shapes whose curve is null or empty:
    // they are skipped, so there is no text-on-path candidate.
    std::vector<SPItem *> const candidates{empty_item, lone_item};
    for (auto const &where : {Geom::Point(0, 0), Geom::Point(300, 300), Geom::Point(5, 5)}) {
        EXPECT_FALSE(UI::Tools::TextTool::hasTextOnPathCandidateForTesting(desktop, candidates, where, 1000.0));
    }
    // A real curve among them is still found.
    auto *baseline = cast<SPShape>(document->getObjectById("baseline"));
    ASSERT_TRUE(baseline);
    std::vector<SPItem *> const with_real{empty_item, baseline, lone_item};
    EXPECT_TRUE(UI::Tools::TextTool::hasTextOnPathCandidateForTesting(desktop, with_real, desktop->doc2dt(Geom::Point(10, 181)), 14.0));

    // And through the real pointer events.
    for (auto const &where : {Geom::Point(0, 0), Geom::Point(300, 300), Geom::Point(5, 5)}) {
        MotionEvent motion;
        motion.pos = desktop->d2w(where);
        motion.modifiers = 0;
        tool->root_handler(motion);
    }
    drainMainContext();
    EXPECT_TRUE(document->getObjectById("empty-path"));
    EXPECT_TRUE(document->getObjectById("lone-move"));
}

TEST_F(TextMultiStyleTest, TextToolFontPreviewKeepsPolicyTokenAndRealCursorMovesInvalidateIt)
{
    desktop->getSelection()->set(first);
    desktop->setTool("/tools/text");
    auto tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    ASSERT_EQ(tool->textItem(), first);
    auto &controller = desktop->textStyleController();

    for (auto policy : {UI::FontChoicePolicy::FamilyOnly, UI::FontChoicePolicy::NormalizeFace}) {
        tool->text_sel_start = first->layout.begin();
        tool->text_sel_start.nextCharacter();
        tool->text_sel_end = tool->text_sel_start;
        tool->text_sel_end.nextCharacter();
        checkpoint();
        auto const before = xml();
        auto const generation = controller.fontPolicyGeneration();
        auto pending = choice();
        pending.phase = FontChoicePhase::Preview;
        pending.interaction_id = 100;
        pending.generation = 1;
        unsigned cursor_notifications = 0;
        sigc::scoped_connection cursor = desktop->connect_text_cursor_moved(
            [&](auto *) { ++cursor_notifications; });

        controller.requestFontChoice(pending, policy, generation);
        drainMainContext();
        ASSERT_NE(first->displayLayout(desktop->dkey), &first->layout);
        EXPECT_GT(cursor_notifications, 0u); // Exercise the actual TextTool refresh.
        EXPECT_EQ(controller.fontPolicyGeneration(), generation);
        EXPECT_EQ(xml(), before);
        controller.cancelPreview();
        EXPECT_EQ(controller.fontPolicyGeneration(), generation);
        EXPECT_EQ(first->displayLayout(desktop->dkey), &first->layout);

        ++pending.generation;
        controller.requestFontChoice(pending, policy, generation);
        drainMainContext();
        ASSERT_NE(first->displayLayout(desktop->dkey), &first->layout);
        auto const preview = first->displayLayout(desktop->dkey)->bounds(Geom::Affine());
        auto confirmed = pending;
        confirmed.phase = FontChoicePhase::Commit;
        ++confirmed.generation;
        ASSERT_TRUE(controller.requestFontChoice(confirmed, policy, generation));
        expectSameBounds(first->layout.bounds(Geom::Affine()), preview);
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), before);

        // A genuine logical cursor/range change still rejects the open
        // popover's captured token, rather than retargeting its stale choice.
        auto const stale_generation = controller.fontPolicyGeneration();
        tool->text_sel_start = first->layout.begin();
        tool->text_sel_start.nextCharacter();
        tool->text_sel_end = tool->text_sel_start;
        tool->text_sel_end.nextCharacter();
        controller.requestFontChoice(pending, policy, stale_generation);
        drainMainContext();
        tool->text_sel_end.nextCharacter();
        desktop->emit_text_cursor_moved(tool);
        EXPECT_GT(controller.fontPolicyGeneration(), stale_generation);
        EXPECT_EQ(first->displayLayout(desktop->dkey), &first->layout);
        EXPECT_FALSE(controller.requestFontChoice(confirmed, policy, stale_generation));
        EXPECT_EQ(xml(), before);
    }
}

// Regression for BUG-007: a font hover in the Text panel must preview a
// character range selected *inside* a text block, driven through the real panel
// rather than by calling the controller directly. The panel used to destroy the
// preview it had just published: publishing refreshes the TextTool geometry,
// TextTool emits the desktop-wide text_cursor_moved signal, TextToolbar reacts
// by calling FontLister::selection_update(), and TextPanel's own FontLister
// update handler then cancelled the preview. The availability gate, the
// fontPolicyGeneration token and the layout were each measured and ruled out.
TEST_F(TextMultiStyleTest, PanelSubselectionFontPreviewReachesTheLayout)
{
    desktop->getSelection()->set(first);
    desktop->setTool("/tools/text");
    auto tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    ASSERT_EQ(tool->textItem(), first);
    auto &controller = desktop->textStyleController();

    tool->text_sel_start = first->layout.begin();
    tool->text_sel_start.nextCharacter();
    tool->text_sel_end = tool->text_sel_start;
    tool->text_sel_end.nextCharacter();
    desktop->emit_text_cursor_moved(tool);
    drainMainContext();

    Gtk::Window window;
    auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
    panel->setDesktop(desktop);
    window.set_child(*panel);
    window.present();
    drainMainContext();

    auto *fonts = findWidget<UI::Widget::FontList>(*panel);
    ASSERT_TRUE(fonts);

    // The popover must actually be mapped for the list to resolve a row as an
    // available choice; without this the emitted choice is available=false and
    // the controller rejects it before any preview logic runs.
    auto *popover = gtk_widget_get_ancestor(GTK_WIDGET(fonts->gobj()), GTK_TYPE_POPOVER);
    ASSERT_TRUE(popover);
    gtk_popover_popup(GTK_POPOVER(popover));
    drainMainContext();
    ASSERT_TRUE(panel->get_mapped());
    ASSERT_TRUE(fonts->get_mapped());

    // A row must be genuinely installed for the choice to survive
    // requestFontChoice()'s availability gate. set_current_font() cannot be used
    // for this: it calls add_font(spec, true), the injection path, whose element
    // is not "present". Scan the rows the popover's own discovery produced.
    auto *view = fontTreeView(*fonts);
    ASSERT_TRUE(view);
    auto selection = gtk_list_view_get_model(GTK_LIST_VIEW(view->gobj()));
    ASSERT_TRUE(selection && GTK_IS_SINGLE_SELECTION(selection));
    unsigned const rows = g_list_model_get_n_items(G_LIST_MODEL(selection));

    bool observed_available = false;
    bool preview_active_for_available = false;
    unsigned observed = 0;
    sigc::scoped_connection dump = fonts->signal_font_choice().connect([&](FontChoice const &c) {
        if (c.phase != FontChoicePhase::Preview) return;
        ++observed;
        observed_available |= c.available;
    });

    for (unsigned i = 0; i < rows && i < 10; ++i) {
        gtk_single_selection_set_selected(GTK_SINGLE_SELECTION(selection), i);
        drainMainContext();
        queueFontKeyboardPreview(*fonts);
        drainMainContext();
        if (first->displayLayout(desktop->dkey) != &first->layout) {
            preview_active_for_available = true;
        }
        controller.cancelPreview();
    }

    ASSERT_GT(rows, 0u) << "font list has no rows; test inconclusive";
    ASSERT_GT(observed, 0u) << "no preview choice was emitted for any row; test inconclusive";
    if (!observed_available) {
        GTEST_SKIP() << "every emitted choice reported available=false, so requestFontChoice() "
                        "rejects it before any preview logic runs; see BUG-007";
    }
    EXPECT_TRUE(preview_active_for_available)
        << "an installed-font choice was emitted but the panel produced no preview";

    window.unset_child();
}

TEST_F(TextMultiStyleTest, FamilyOnlyPreviewMatchesCommitAndRemainsViewLocal){
    selectGroup("rich-group");
    auto rich = cast<SPText>(item("rich"));
    auto second_desktop = testApplication().createDesktop(document, false, true);
    ASSERT_TRUE(second_desktop);
    auto &controller = desktop->textStyleController();
    auto const before = xml();
    auto const canonical = rich->layout.bounds(Geom::Affine());
    UI::TextStylePatch patch;
    patch.family = "sans-serif";
    controller.preview(patch, UI::TextStyleOrigin::Direct);
    EXPECT_EQ(xml(), before);
    EXPECT_EQ(rich->displayLayout(second_desktop->dkey), &rich->layout);
    expectSameBounds(rich->layout.bounds(Geom::Affine()), canonical);
    auto const preview = rich->displayLayout(desktop->dkey)->bounds(Geom::Affine());
    auto const changed = controller.commit(patch, "test:preview", RC_("Undo", "Style text"));
    expectSameBounds(rich->layout.bounds(Geom::Affine()), preview);
    EXPECT_EQ(rich->displayLayout(desktop->dkey), &rich->layout);
    testApplication().destroyDesktop(second_desktop);
    EXPECT_TRUE(changed);
}

TEST_F(TextMultiStyleTest, FontOnlyModeIsDesktopSessionStateAndRejectsOldPolicyEvents)
{
    auto &controller = desktop->textStyleController();
    EXPECT_FALSE(controller.panelFontOnly());
    auto const before = xml();
    auto const generation = controller.fontPolicyGeneration();
    auto preview_choice = choice();
    preview_choice.phase = FontChoicePhase::Preview;
    controller.requestFontChoice(preview_choice, UI::FontChoicePolicy::NormalizeFace, generation);
    controller.setPanelFontOnly(true);
    EXPECT_TRUE(controller.panelFontOnly());
    EXPECT_EQ(first->displayLayout(desktop->dkey), &first->layout);
    EXPECT_FALSE(controller.requestFontChoice(choice(), UI::FontChoicePolicy::NormalizeFace, generation));
    EXPECT_EQ(xml(), before);
    selectGroup("rich-group");
    EXPECT_TRUE(controller.panelFontOnly());
    auto other = testApplication().createDesktop(document, false, true);
    ASSERT_TRUE(other);
    EXPECT_FALSE(other->textStyleController().panelFontOnly());
    testApplication().destroyDesktop(other);
    // The independent Browser adapter has an explicit normal policy.
    UI::TextFontPreviewController browser;
    browser.setDesktop(desktop);
    EXPECT_TRUE(browser.commit(choice()));
    EXPECT_EQ(item("rich-24")->style->font_weight.computed, 400);
    EXPECT_EQ(item("rich-36")->style->font_style.computed, SP_CSS_FONT_STYLE_NORMAL);
    browser.setDesktop(nullptr);
}

TEST_F(TextMultiStyleTest, FlowRegionsTextPathsAndInlineSizeRetainTheirStructure)
{
    desktop->getSelection()->setList(std::vector<SPItem *>{item("flow-group"), item("path-group"), item("inline-group")});
    auto const region = subtree("flow-region");
    auto const baseline = subtree("baseline");
    auto &controller = desktop->textStyleController();
    ASSERT_TRUE(controller.requestFontChoice(choice(), UI::FontChoicePolicy::FamilyOnly));
    EXPECT_EQ(subtree("flow-region"), region);
    EXPECT_EQ(subtree("baseline"), baseline);
    for (auto id : {"flow", "path-text", "inline-text"}) {
        EXPECT_STREQ(item(id)->style->font_family.value(), "sans-serif");
        EXPECT_DOUBLE_EQ(item(id)->style->font_size.computed, 12);
    }
    EXPECT_TRUE(is<SPFlowtext>(item("flow")));
    EXPECT_NE(subtree("path-text").find("textPath"), std::string::npos);
    EXPECT_NE(subtree("inline-text").find("inline-size:100px"), std::string::npos);
}

TEST_F(TextMultiStyleTest, PreviewCancelsOnAncestorLockSelectionChangeAndToolSwitch)
{
    selectGroup();
    UI::TextStylePatch patch;
    patch.family = "sans-serif";
    auto &controller = desktop->textStyleController();
    controller.preview(patch, UI::TextStyleOrigin::Direct);
    ASSERT_NE(first->displayLayout(desktop->dkey), &first->layout);
    item("group")->setLocked(true);
    document->ensureUpToDate();
    EXPECT_EQ(first->displayLayout(desktop->dkey), &first->layout);
    auto const locked = xml();
    EXPECT_FALSE(controller.commit(patch, "test:locked", RC_("Undo", "Style text")));
    EXPECT_EQ(xml(), locked);
    item("group")->setLocked(false);
    document->ensureUpToDate();
    controller.preview(patch, UI::TextStyleOrigin::Direct);
    selectGroup("rich-group");
    EXPECT_EQ(first->displayLayout(desktop->dkey), &first->layout);
    selectGroup();
    controller.preview(patch, UI::TextStyleOrigin::Direct);
    desktop->setTool("/tools/text");
    EXPECT_EQ(first->displayLayout(desktop->dkey), &first->layout);
}

TEST_F(TextMultiStyleTest, PanelTogglePersistsOnReopenAndLeavesIndependentControlsEnabled)
{
    selectGroup("rich-group");
    auto const before = xml();
    document->setModifiedSinceSave(false);
    auto find_button = [](Gtk::Widget &root, Glib::ustring const &label) -> Gtk::ToggleButton * {
        std::vector<Gtk::Widget *> pending{&root};
        while (!pending.empty()) {
            auto widget = pending.back();
            pending.pop_back();
            if (auto button = dynamic_cast<Gtk::ToggleButton *>(widget); button && button->get_label() == label) return button;
            for (auto child = widget->get_first_child(); child; child = child->get_next_sibling()) pending.push_back(child);
        }
        return nullptr;
    };
    {
        UI::Dialog::TextPanel panel;
        panel.setDesktop(desktop);
        // This is the suite's first FontList. Wait for the actual catalog
        // completion before queuing input: discovery rebuilds cancel pending
        // choices and replace the selected row as part of normal startup.
        // This is a readiness wait, not a latency gate: the case asserts the
        // persistence behavior only. Cold-scan completeness and latency stay
        // covered explicitly by FontCatalogGuiTest, so the ceiling below exists
        // solely to terminate a hung setup and must not be read as a deadline.
        auto discovery_loop = Glib::MainLoop::create();
        bool catalog_received = false;
        bool discovery_finished = false;
        bool discovery_started = false;
        bool discovery_progress = false;
        auto const discovery_started_at = std::chrono::steady_clock::now();
        sigc::scoped_connection discovery = FontDiscovery::get().connect_to_fonts(
            [&](FontDiscovery::MessageType const &message) {
                if (std::get_if<Async::Msg::OperationStarted>(&message)) discovery_started = true;
                if (Async::Msg::get_progress(message)) discovery_progress = true;
                if (auto result = Async::Msg::get_result(message)) catalog_received = bool(*result);
                if (Async::Msg::is_finished(message)) {
                    discovery_finished = true;
                    discovery_loop->quit();
                }
            });
        if (!discovery_finished) {
            sigc::scoped_connection timeout = Glib::signal_timeout().connect([&] {
                discovery_loop->quit();
                return false;
            }, 300000);
            discovery_loop->run();
        }
        auto const discovery_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - discovery_started_at).count();
        ASSERT_TRUE(discovery_finished && catalog_received)
            << "Font discovery did not complete after " << discovery_elapsed_ms << " ms"
            << " (started=" << discovery_started << " progress=" << discovery_progress << ")";
        drainMainContext(); // Finish the catalog's chunked model population.

        Gtk::Window window;
        window.set_default_size(500, 700);
        window.set_child(panel);
        window.present();
        auto toggle = find_button(panel, "Font only");
        ASSERT_TRUE(toggle);
        EXPECT_FALSE(toggle->get_active());
        EXPECT_TRUE(toggle->get_focusable());
        auto fonts = findWidget<UI::Widget::FontList>(panel);
        ASSERT_TRUE(fonts);
        auto popover = gtk_widget_get_ancestor(GTK_WIDGET(fonts->gobj()), GTK_TYPE_POPOVER);
        ASSERT_TRUE(popover);
        gtk_popover_popup(GTK_POPOVER(popover));
        drainMainContext(); // Settle mapping, focus and initial pointer work.
        ASSERT_TRUE(panel.get_mapped());
        ASSERT_TRUE(fonts->get_mapped());
        // The event need not name an installed font to test the session
        // boundary: an injected row is deterministic on both platforms.
        fonts->set_current_font("TEXT001 queued font", "Regular");
        auto view = fontTreeView(*fonts);
        ASSERT_TRUE(view);
        auto selection = gtk_list_view_get_model(GTK_LIST_VIEW(view->gobj()));
        gtk_single_selection_set_selected(GTK_SINGLE_SELECTION(selection), 0);
        drainMainContext();
        ASSERT_TRUE(gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(selection)));
        ASSERT_EQ(fonts->get_fontspec(), get_fontspec("TEXT001 queued font", "Regular"));
        unsigned old_keyboard_previews = 0;
        sigc::scoped_connection choices = fonts->signal_font_choice().connect([&](FontChoice const &choice) {
            if (choice.phase == FontChoicePhase::Preview && choice.origin == FontChoiceOrigin::Keyboard) {
                ++old_keyboard_previews;
            }
        });
        ASSERT_TRUE(queueFontKeyboardPreview(*fonts));
        drainMainContext();
        ASSERT_EQ(old_keyboard_previews, 1u); // Positive control: queue really emits.
        old_keyboard_previews = 0;
        ASSERT_TRUE(queueFontKeyboardPreview(*fonts));
        toggle->set_active(true);
        drainMainContext();
        EXPECT_EQ(old_keyboard_previews, 0u); // Reconnect must not relabel old work.
        EXPECT_TRUE(desktop->textStyleController().panelFontOnly());
        auto bold = find_button(panel, "B");
        ASSERT_TRUE(bold);
        EXPECT_TRUE(bold->get_sensitive());
        EXPECT_EQ(xml(), before);
        EXPECT_FALSE(document->isModifiedSinceSave());
        EXPECT_FALSE(DocumentUndo::undo(document));
        std::vector<Gtk::Widget *> pending{&panel};
        bool found_face = false;
        while (!pending.empty()) {
            auto widget = pending.back();
            pending.pop_back();
            if (auto menu = dynamic_cast<Gtk::MenuButton *>(widget);
                menu && menu->get_tooltip_text().find("Font only") != Glib::ustring::npos) {
                found_face = true;
                EXPECT_FALSE(menu->get_sensitive());
            }
            for (auto child = widget->get_first_child(); child; child = child->get_next_sibling()) pending.push_back(child);
        }
        EXPECT_TRUE(found_face);
        window.unset_child();
    }
    UI::Dialog::TextPanel reopened;
    reopened.setDesktop(desktop);
    auto toggle = find_button(reopened, "Font only");
    ASSERT_TRUE(toggle);
    EXPECT_TRUE(toggle->get_active());
    EXPECT_EQ(xml(), before);
}

TEST_F(TextMultiStyleTest, FontListUnmapDiscardsQueuedKeyboardAndPointerEvents)
{
    UI::Widget::FontList fonts("/tests/text-font-list-queue", UI::Widget::FontListMode::CompactPopover);
    Gtk::Window window;
    window.set_child(fonts);
    window.present();
    drainMainContext();
    fonts.set_current_font("TEXT001 queued font", "Regular");
    auto view = fontTreeView(fonts);
    ASSERT_TRUE(view);
    auto selection = gtk_list_view_get_model(GTK_LIST_VIEW(view->gobj()));
    gtk_single_selection_set_selected(GTK_SINGLE_SELECTION(selection), 0);
    drainMainContext();
    ASSERT_TRUE(fonts.get_mapped());

    unsigned keyboard_previews = 0;
    unsigned pointer_cancels = 0;
    sigc::scoped_connection choices = fonts.signal_font_choice().connect([&](FontChoice const &choice) {
        if (choice.phase == FontChoicePhase::Preview && choice.origin == FontChoiceOrigin::Keyboard) {
            ++keyboard_previews;
        }
        if (choice.phase == FontChoicePhase::Cancel && choice.origin == FontChoiceOrigin::Pointer) {
            ++pointer_cancels;
        }
    });
    ASSERT_TRUE(queueFontKeyboardPreview(fonts));
    drainMainContext();
    ASSERT_EQ(keyboard_previews, 1u);
    auto const before_leave = pointer_cancels;
    ASSERT_TRUE(queueFontPointerLeave(fonts));
    drainMainContext();
    ASSERT_EQ(pointer_cancels, before_leave + 1);

    ASSERT_TRUE(queueFontKeyboardPreview(fonts));
    ASSERT_TRUE(queueFontPointerLeave(fonts));
    fonts.set_visible(false);
    ASSERT_FALSE(fonts.get_mapped());
    auto const after_unmap = pointer_cancels; // One synchronous cancel is allowed.
    drainMainContext();
    EXPECT_EQ(keyboard_previews, 1u);
    EXPECT_EQ(pointer_cancels, after_unmap);

    fonts.set_visible(true);
    drainMainContext();
    ASSERT_TRUE(queueFontKeyboardPreview(fonts));
    drainMainContext();
    EXPECT_EQ(keyboard_previews, 2u); // New session still works.
    window.unset_child();
}

TEST_F(TextMultiStyleTest, InvalidExplicitRangesAndStaleSelectionEventsCannotRetarget)
{
    auto &controller = desktop->textStyleController();
    auto const before = xml();
    controller.begin({{SPWeakPtr<SPItem>(first), 100, 200, false}});
    UI::TextStylePatch patch;
    patch.family = "sans-serif";
    EXPECT_FALSE(controller.commit(patch, "test:invalid", RC_("Undo", "Style text")));
    EXPECT_EQ(xml(), before);
    auto const generation = controller.fontPolicyGeneration();
    auto pending = choice();
    pending.phase = FontChoicePhase::Preview;
    controller.requestFontChoice(pending, UI::FontChoicePolicy::FamilyOnly, generation);
    selectGroup("rich-group");
    EXPECT_FALSE(controller.requestFontChoice(choice(), UI::FontChoicePolicy::FamilyOnly, generation));
    EXPECT_EQ(xml(), before);
    EXPECT_EQ(first->displayLayout(desktop->dkey), &first->layout);
}

TEST_F(TextMultiStyleTest, TransformedExplicitSizeNoOpPreservesRedo)
{
    item("group")->setAttribute("transform", "scale(2,3)");
    checkpoint();
    selectGroup();
    auto &controller = desktop->textStyleController();
    UI::TextStylePatch size;
    size.font_size_px = 48;
    ASSERT_TRUE(controller.commit(size, "test:size", RC_("Undo", "Set text size")));
    document->ensureUpToDate();
    // The shared CSS serializer rounds the inverse-scaled local size. Match
    // the controller's relative 1e-6 tolerance; history/XML remain exact below.
    EXPECT_NEAR(controller.query().font_size_px.value, 48, 48 * 1e-6);
    UI::TextStylePatch paint;
    paint.fill = "#abcdef";
    ASSERT_TRUE(controller.commit(paint, "test:paint", RC_("Undo", "Paint text")));
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    auto const before = xml();
    EXPECT_FALSE(controller.commit(size, "test:size", RC_("Undo", "Set text size")));
    EXPECT_EQ(xml(), before);
    EXPECT_TRUE(DocumentUndo::redo(document));
}

// Regression: multi-line "line text" (sodipodi:role="line" tspans) is a single
// layout paragraph whose lines are sibling tspans. Committing centre alignment
// must reach every line and must not restructure the span tree. The panel
// previously routed this through sp_te_apply_style() over the character range,
// which wraps/splits spans instead of inheriting the paragraph properties, so
// the lower line stayed left aligned.
TEST_F(TextMultiStyleTest, CenterAlignmentReachesEveryLineOfLineText)
{
    auto lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    desktop->getSelection()->set(lines);
    drainMainContext();

    auto const children_before = lines->childList(false).size();

    UI::TextParagraphPatch patch;
    patch.alignment = UI::TextParagraphAlignment::Center;
    ASSERT_TRUE(desktop->textStyleController().commitParagraph(
        patch, "test:line-center", RC_("Undo", "Center line text")));
    document->ensureUpToDate();

    // Every structural line must carry the paragraph alignment.
    std::vector<SPTSpan *> line_spans;
    for (auto child : lines->childList(false)) {
        if (auto tspan = cast<SPTSpan>(child)) line_spans.push_back(tspan);
    }
    ASSERT_EQ(line_spans.size(), 2u);
    for (auto *span : line_spans) {
        ASSERT_TRUE(span->style);
        EXPECT_EQ(span->style->text_anchor.computed, SP_CSS_TEXT_ANCHOR_MIDDLE);
        EXPECT_EQ(span->style->text_align.computed, SP_CSS_TEXT_ALIGN_CENTER);
    }

    // Outcome: every rendered line shares one horizontal centre.
    struct LineExtent {
        double min_x = 0.0;
        double max_x = 0.0;
        bool seen = false;
    };
    auto const &layout = lines->layout;
    std::map<unsigned, LineExtent> extents;
    for (auto it = layout.begin(); it != layout.end(); it.nextCharacter()) {
        auto const box = layout.characterBoundingBox(it);
        auto &extent = extents[layout.lineIndex(it)];
        if (!extent.seen) {
            extent = {box.min()[Geom::X], box.max()[Geom::X], true};
        } else {
            extent.min_x = std::min(extent.min_x, box.min()[Geom::X]);
            extent.max_x = std::max(extent.max_x, box.max()[Geom::X]);
        }
    }
    ASSERT_EQ(extents.size(), 2u);
    auto const &first_extent = extents.begin()->second;
    auto const &second_extent = std::next(extents.begin())->second;
    EXPECT_NEAR((first_extent.min_x + first_extent.max_x) / 2.0,
                (second_extent.min_x + second_extent.max_x) / 2.0, 0.5)
        << "line 1 [" << first_extent.min_x << "," << first_extent.max_x
        << "] line 2 [" << second_extent.min_x << "," << second_extent.max_x << "]";

    // Alignment is a style change; it must not restructure the lines.
    EXPECT_EQ(lines->childList(false).size(), children_before);
}

// Same as above but the authored lines carry different x anchors, which is what
// imported (CDR/PDF) multi-line text looks like. text-anchor:middle centres each
// chunk on its own x, so the lines do not share a centre and the lower one reads
// as left aligned even though the property was applied.
TEST_F(TextMultiStyleTest, CenterAlignmentSharesOneCentreWhenLinesCarryDifferentAnchors)
{
    auto lines = cast<SPText>(document->getObjectById("line-text-offset"));
    ASSERT_TRUE(lines);
    desktop->getSelection()->set(lines);
    drainMainContext();

    UI::TextParagraphPatch patch;
    patch.alignment = UI::TextParagraphAlignment::Center;
    ASSERT_TRUE(desktop->textStyleController().commitParagraph(
        patch, "test:line-center-offset", RC_("Undo", "Center offset line text")));
    document->ensureUpToDate();

    struct LineExtent {
        double min_x = 0.0;
        double max_x = 0.0;
        bool seen = false;
    };
    auto const &layout = lines->layout;
    std::map<unsigned, LineExtent> extents;
    for (auto it = layout.begin(); it != layout.end(); it.nextCharacter()) {
        auto const box = layout.characterBoundingBox(it);
        auto &extent = extents[layout.lineIndex(it)];
        if (!extent.seen) {
            extent = {box.min()[Geom::X], box.max()[Geom::X], true};
        } else {
            extent.min_x = std::min(extent.min_x, box.min()[Geom::X]);
            extent.max_x = std::max(extent.max_x, box.max()[Geom::X]);
        }
    }
    ASSERT_EQ(extents.size(), 2u);
    auto const &first_extent = extents.begin()->second;
    auto const &second_extent = std::next(extents.begin())->second;
    EXPECT_NEAR((first_extent.min_x + first_extent.max_x) / 2.0,
                (second_extent.min_x + second_extent.max_x) / 2.0, 0.5)
        << "line 1 [" << first_extent.min_x << "," << first_extent.max_x
        << "] line 2 [" << second_extent.min_x << "," << second_extent.max_x << "]"
        << "\nxml: " << xml();
}

// --- Centre-alignment outcome coverage through the real widgets -------------
//
// These cases drive the actual TextPanel and TextToolbar alignment controls
// instead of calling the shared alignment helper directly. The oracle is the
// rendered per-line extent and the object's native alignment anchor.

struct AlignmentLineSpan {
    double min_x = 0.0;
    double max_x = 0.0;
    bool seen = false;
};

std::map<unsigned, AlignmentLineSpan> alignmentLineSpans(SPText const &text)
{
    std::map<unsigned, AlignmentLineSpan> spans;
    auto const &layout = text.layout;
    for (auto it = layout.begin(); it != layout.end(); it.nextCharacter()) {
        auto const box = layout.characterBoundingBox(it);
        auto &span = spans[layout.lineIndex(it)];
        if (!span.seen) {
            span = {box.min()[Geom::X], box.max()[Geom::X], true};
        } else {
            span.min_x = std::min(span.min_x, box.min()[Geom::X]);
            span.max_x = std::max(span.max_x, box.max()[Geom::X]);
        }
    }
    return spans;
}

double alignmentLineCentre(AlignmentLineSpan const &span)
{
    return (span.min_x + span.max_x) / 2.0;
}

void expectCommonLineCentre(std::map<unsigned, AlignmentLineSpan> const &spans, unsigned expected_lines)
{
    ASSERT_EQ(spans.size(), expected_lines);
    auto const &first = spans.begin()->second;
    for (auto it = std::next(spans.begin()); it != spans.end(); ++it) {
        EXPECT_NEAR(alignmentLineCentre(first), alignmentLineCentre(it->second), 0.5)
            << "line " << it->first << " [" << it->second.min_x << "," << it->second.max_x
            << "] against [" << first.min_x << "," << first.max_x << "]";
    }
}

Gtk::ToggleButton *findAlignmentButton(Gtk::Widget &root, Glib::ustring const &tooltip)
{
    std::vector<Gtk::Widget *> pending{&root};
    while (!pending.empty()) {
        auto *widget = pending.back();
        pending.pop_back();
        if (auto *button = dynamic_cast<Gtk::ToggleButton *>(widget);
            button && button->get_tooltip_text() == tooltip) {
            return button;
        }
        for (auto child = widget->get_first_child(); child; child = child->get_next_sibling()) {
            pending.push_back(child);
        }
    }
    return nullptr;
}

// The panel's paragraph controls live inside a collapsed Expander; a real user
// expands it before using the centre control, so the fixture must too.
void expandPanelSection(Gtk::Widget &root, Glib::ustring const &label)
{
    std::vector<Gtk::Widget *> pending{&root};
    while (!pending.empty()) {
        auto *widget = pending.back();
        pending.pop_back();
        if (auto *expander = dynamic_cast<Gtk::Expander *>(widget); expander && expander->get_label() == label) {
            expander->set_expanded(true);
            return;
        }
        for (auto child = widget->get_first_child(); child; child = child->get_next_sibling()) {
            pending.push_back(child);
        }
    }
}

void placeCaretInFirstHardLine(SPDesktop *desktop, SPText *text, UI::Tools::TextTool *&tool)
{
    desktop->getSelection()->set(text);
    desktop->setTool("/tools/text");
    tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    ASSERT_EQ(tool->textItem(), text);
    tool->text_sel_start = text->layout.begin();
    tool->text_sel_end = tool->text_sel_start;
    desktop->emit_text_cursor_moved(tool);
    drainMainContext();
}

// --- Line-height differential coverage through the real widgets -------------
//
// Native rendered line baselines are the oracle. We measure the document-space
// vertical distance between consecutive rendered line baselines from the layout
// itself (never from ink bounds), then compare the real TextPanel and
// TextToolbar entry points applied to the same restored fixture. Mixed per-line
// heights can produce a half-sum, so no guessed absolute is asserted where the
// authored lines differ.

struct BaselineGap {
    std::vector<double> line_y;
    double gap = 0.0;
};

BaselineGap measureBaselineGap(SPText &text)
{
    BaselineGap result;
    auto const to_document = text.i2doc_affine();
    for (auto const &segment : text.layout.getBaselines()) {
        result.line_y.push_back((segment.initialPoint() * to_document)[Geom::Y]);
    }
    if (result.line_y.size() >= 2) {
        result.gap = result.line_y[1] - result.line_y[0];
    }
    return result;
}

Gtk::Widget *findWidgetByTooltip(Gtk::Widget &root, Glib::ustring const &tooltip)
{
    std::vector<Gtk::Widget *> pending{&root};
    while (!pending.empty()) {
        auto *widget = pending.back();
        pending.pop_back();
        if (widget->get_tooltip_text() == tooltip) return widget;
        for (auto child = widget->get_first_child(); child; child = child->get_next_sibling()) {
            pending.push_back(child);
        }
    }
    return nullptr;
}

Gtk::SpinButton *panelLineHeightSpin(Gtk::Widget &panel)
{
    return dynamic_cast<Gtk::SpinButton *>(findWidgetByTooltip(panel, "Distance between baselines"));
}

// The real Gtk::DropDown unit control for the panel line-height entry. A bare
// numeric value is only an absolute document-px request when this control is
// set to Px: the control defaults to "lines", and a mixed per-line snapshot
// leaves it visibly at that default, so 20 would mean 20 lines (400 px at a
// 20 px font) rather than 20 px. The tests must drive the unit control before
// entering the value, exactly as a user would.
Gtk::DropDown *panelLineHeightUnitDropdown(Gtk::Widget &panel)
{
    return dynamic_cast<Gtk::DropDown *>(findWidgetByTooltip(panel, "Line-height unit"));
}

UI::Widget::SpinButton *toolbarLineHeightSpin(Gtk::Widget &toolbar)
{
    auto *box = findWidgetByTooltip(toolbar, "Spacing between baselines");
    return box ? findWidget<UI::Widget::SpinButton>(*box) : nullptr;
}

// Convert the shared ordinary multiline fixture into a two-line line-height case.
// The hard-line y attributes are removed so the root strut governs the spacing,
// the unselected second line carries explicit bold/fill that must survive, and
// the unselected second line optionally overrides the root with 20px so the
// object has genuinely mixed per-line heights.
bool configureLineHeightFixture(SPDocument *document, char const *root_style, bool child_holds_twenty)
{
    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    if (!lines) return false;
    lines->setAttribute("style", root_style);
    for (auto const *id : {"line-one", "line-two"}) {
        auto *span = document->getObjectById(id);
        if (!span) return false;
        span->setAttribute("y", nullptr);
    }
    auto *line_one = document->getObjectById("line-one");
    auto *line_two = document->getObjectById("line-two");
    if (!line_one || !line_two) return false;
    (void)line_one;
    line_two->setAttribute("style",
        child_holds_twenty ? "line-height:20px;font-weight:bold;fill:#123456"
                           : "font-weight:bold;fill:#123456");
    return true;
}

void expectUnselectedRunStylePreserved(SPDocument *document)
{
    auto *line_two = cast<SPTSpan>(document->getObjectById("line-two"));
    ASSERT_TRUE(line_two);
    ASSERT_TRUE(line_two->style);
    EXPECT_EQ(line_two->style->font_weight.computed, 700);
    ASSERT_TRUE(line_two->style->fill.isColor());
    EXPECT_EQ(line_two->style->fill.getColor().toRGBA(), 0x123456ff);
}

double measureLineTextGap(SPDocument *document)
{
    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    EXPECT_TRUE(lines);
    return lines ? measureBaselineGap(*lines).gap : 0.0;
}

// Test-only artifact hook. When TEXT_PARITY_SNAPSHOT_DIR is set, preserve the
// exact fixture SVG for the named entry-point state under evidence; nothing is
// written otherwise, and no user original is ever touched. A test run is
// reproducible from these snapshots.
void writeEvidenceSvg(char const *name, std::string const &svg)
{
    char const *dir = std::getenv("TEXT_PARITY_SNAPSHOT_DIR");
    if (!dir || !*dir) return;
    std::ofstream out(std::string(dir) + "/" + name + ".svg", std::ios::binary | std::ios::trunc);
    out << svg;
}

// Character data shown by an element and all of its descendants. An element that
// has child nodes (e.g. a <tspan> holding a text node) exposes a null
// repr->content(), so a bare content() read reports "" and cannot prove that the
// authored string survived; this recurses to the text nodes instead.
void appendDescendantText(XML::Node *node, std::string &out)
{
    for (auto child = node->firstChild(); child; child = child->next()) {
        if (child->type() == XML::NodeType::TEXT_NODE) {
            if (auto const *content = child->content()) out += content;
        } else {
            appendDescendantText(child, out);
        }
    }
}

std::string descendantText(SPObject *object)
{
    std::string out;
    if (object) {
        if (auto repr = object->getRepr()) appendDescendantText(repr, out);
    }
    return out;
}

// Configure the two-line ordinary <text> for an explicit partial-range
// request: 50 px root strut at font-size 20 px, hard-line y attributes removed
// so the strut governs spacing, and an explicit, unrelated font/fill on the
// UNSELECTED first line that must survive an inner-range request on the last
// line.
bool configureSubrangeLineHeightFixture(SPDocument *document)
{
    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    if (!lines) return false;
    lines->setAttribute("style", "font-family:sans-serif;font-size:20px;line-height:50px");
    auto *line_one = document->getObjectById("line-one");
    auto *line_two = document->getObjectById("line-two");
    if (!line_one || !line_two) return false;
    for (auto *span : {line_one, line_two}) span->setAttribute("y", nullptr);
    line_one->setAttribute("style", "font-weight:bold;fill:#123456");
    line_two->setAttribute("style", nullptr);
    return true;
}

// Configure the four-hard-line ordinary <text> for the unselected-spacing
// preservation case: 50 px root strut at font-size 20 px, all hard-line y
// attributes removed, and the first two (unselected) lines authored at 80 px
// with unrelated bold/fill run styles.
bool configureFourLineLineHeightFixture(SPDocument *document)
{
    auto *lines = cast<SPText>(document->getObjectById("line-text-four"));
    if (!lines) return false;
    lines->setAttribute("style", "font-family:sans-serif;font-size:20px;line-height:50px");
    for (auto const *id : {"four-one", "four-two", "four-three", "four-four"}) {
        auto *span = document->getObjectById(id);
        if (!span) return false;
        span->setAttribute("y", nullptr);
    }
    for (auto const *id : {"four-one", "four-two"}) {
        document->getObjectById(id)->setAttribute(
            "style", "font-weight:bold;fill:#123456;line-height:80px");
    }
    return true;
}

void expectBoldFillPreserved(SPObject *object, char const *where)
{
    if (!object || !object->style) {
        EXPECT_TRUE(object && object->style) << where;
        return;
    }
    EXPECT_EQ(object->style->font_weight.computed, 700) << where;
    if (!object->style->fill.isColor()) {
        EXPECT_TRUE(object->style->fill.isColor()) << where;
        return;
    }
    EXPECT_EQ(object->style->fill.getColor().toRGBA(), 0x123456ff) << where;
}

// Place a real explicit one-character range inside the LAST rendered hard
// line: start != end (not a caret) and not the full text range, so ordinary
// line-height takes the native inner-range scope rather than whole-object.
bool placeCharacterSelectionInLastHardLine(SPDesktop *desktop, SPText *text,
                                           UI::Tools::TextTool *&tool)
{
    desktop->getSelection()->set(text);
    desktop->setTool("/tools/text");
    tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    if (!tool || tool->textItem() != text) return false;

    unsigned last_line = 0;
    bool any = false;
    for (auto it = text->layout.begin(); it != text->layout.end(); it.nextCharacter()) {
        last_line = std::max(last_line, text->layout.lineIndex(it));
        any = true;
    }
    if (!any) return false;

    auto start = text->layout.begin();
    while (start != text->layout.end() && text->layout.lineIndex(start) != last_line) {
        start.nextCharacter();
    }
    if (start == text->layout.end()) return false;

    tool->text_sel_start = start;
    tool->text_sel_end = start;
    tool->text_sel_end.nextCharacter();
    desktop->emit_text_cursor_moved(tool);
    drainMainContext();
    return true;
}

// Reversed variant: the same explicit one-character range inside the LAST hard
// line, but text_sel_start is the later character and text_sel_end the earlier
// one, so the range runs backwards exactly as a right-to-left drag.
bool placeReversedCharacterSelectionInLastHardLine(SPDesktop *desktop, SPText *text,
                                                   UI::Tools::TextTool *&tool)
{
    desktop->getSelection()->set(text);
    desktop->setTool("/tools/text");
    tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    if (!tool || tool->textItem() != text) return false;

    unsigned last_line = 0;
    bool any = false;
    for (auto it = text->layout.begin(); it != text->layout.end(); it.nextCharacter()) {
        last_line = std::max(last_line, text->layout.lineIndex(it));
        any = true;
    }
    if (!any) return false;

    auto start = text->layout.begin();
    while (start != text->layout.end() && text->layout.lineIndex(start) != last_line) {
        start.nextCharacter();
    }
    if (start == text->layout.end()) return false;

    tool->text_sel_end = start;
    tool->text_sel_start = start;
    tool->text_sel_start.nextCharacter();
    desktop->emit_text_cursor_moved(tool);
    drainMainContext();
    return true;
}

// Raw logical indices of the live TextTool selection, direction preserved: a
// reversed drag reads start > end.
struct RawSelectionRange {
    bool valid = false;
    int start = 0;
    int end = 0;
};

RawSelectionRange rawSelectionRange(SPDesktop *desktop)
{
    RawSelectionRange result;
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop ? desktop->getTool() : nullptr);
    if (!tool) return result;
    auto *text = cast<SPText>(tool->textItem());
    if (!text) return result;
    result.start = static_cast<int>(text->layout.iteratorToCharIndex(tool->text_sel_start));
    result.end = static_cast<int>(text->layout.iteratorToCharIndex(tool->text_sel_end));
    result.valid = true;
    return result;
}

// Drive the real TextPanel line-height control on the current TextTool
// selection. The real Px unit row is selected first, then every value in
// `values_px` is entered as a real control change. Passing {force, target} lets
// a caller reach `target` even when the control already displays it, because
// Gtk emits value-changed only when the numeric value actually changes. The
// final rendered baseline vector and document XML are returned.
bool driveRealPanelLineHeight(SPDocument *document, SPDesktop *desktop,
                              std::vector<double> const &values_px,
                              std::vector<double> &after_y, std::string &after_xml)
{
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop ? desktop->getTool() : nullptr);
    if (!tool || !cast<SPText>(tool->textItem())) return false;

    Gtk::Window window;
    auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
    panel->setDesktop(desktop);
    window.set_child(*panel);
    window.present();
    drainMainContext();
    expandPanelSection(*panel, "Paragraph");
    drainMainContext();

    auto *unit = panelLineHeightUnitDropdown(*panel);
    if (!unit) return false;
    unit->set_selected(static_cast<guint>(UI::TextLineHeightUnit::Px));
    drainMainContext();
    if (unit->get_selected() != static_cast<guint>(UI::TextLineHeightUnit::Px)) return false;

    auto *spin = panelLineHeightSpin(*panel);
    if (!spin) return false;
    for (double value : values_px) {
        spin->set_value(value);
        drainMainContext();
        document->ensureUpToDate();
    }

    auto *current = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    auto *text = current ? cast<SPText>(current->textItem()) : nullptr;
    if (!text) return false;
    after_y = measureBaselineGap(*text).line_y;
    after_xml = sp_repr_save_buf(document->getReprDoc()).raw();
    window.unset_child();
    return true;
}

// Native toolbar oracle for the same inner request: the restored fixture plus
// the forwarded or reversed explicit range is driven through the real
// TextToolbar, which is detached before the caller measures or asserts. Every
// value in `values_px` is entered as a real control change: a bare set_value to
// the value already displayed emits nothing, so callers that need a real change
// pass an intermediate value first, exactly as the panel path does.
bool driveNativeToolbarInnerLineHeight(SPDocument *document, SPDesktop *desktop,
                                       char const *text_id, bool reversed,
                                       std::vector<double> const &values_px,
                                       std::vector<double> &after_y, std::string &after_xml)
{
    auto *text = cast<SPText>(document->getObjectById(text_id));
    if (!text) return false;
    desktop->getSelection()->set(text);
    Inkscape::UI::Toolbar::TextToolbar toolbar;
    toolbar.setDesktop(desktop);
    UI::Tools::TextTool *tool = nullptr;
    bool const placed = reversed ? placeReversedCharacterSelectionInLastHardLine(desktop, text, tool)
                                 : placeCharacterSelectionInLastHardLine(desktop, text, tool);
    if (!placed) {
        toolbar.setDesktop(nullptr);
        return false;
    }
    auto *spin = toolbarLineHeightSpin(toolbar);
    if (!spin) {
        toolbar.setDesktop(nullptr);
        return false;
    }
    for (double value : values_px) {
        spin->set_value(value);
        drainMainContext();
        document->ensureUpToDate();
    }
    toolbar.setDesktop(nullptr);

    text = cast<SPText>(document->getObjectById(text_id));
    if (!text) return false;
    after_y = measureBaselineGap(*text).line_y;
    after_xml = sp_repr_save_buf(document->getReprDoc()).raw();
    return true;
}

// Two equivalent imported-multiline ordinary texts: one whose first line is a
// direct root text node (SPString) holding the literal "First line\n", and one
// whose first line is the same "First line\n" wrapped in a plain tspan. The
// second line is a plain tspan in both; neither uses role=line, so the hard
// break comes only from the preserved newline under white-space:pre rather than
// from special line semantics. Both author the same root 50px strut, 20px font
// and fill, so before any operation they must render identically.
bool configureDirectRootAndWrappedEquivalentFixture(SPDocument *document)
{
    char const *style =
        "font-family:sans-serif;font-size:20px;line-height:50px;fill:#123456;white-space:pre";
    for (auto const *id : {"direct-root-line", "wrapped-line"}) {
        auto *text = cast<SPText>(document->getObjectById(id));
        if (!text) return false;
        text->setAttribute("style", style);
    }
    for (auto const *id : {"direct-root-second", "wrapped-first", "wrapped-second"}) {
        auto *span = document->getObjectById(id);
        if (!span) return false;
        span->setAttribute("y", nullptr);
    }
    return true;
}

void TextMultiStyleTest::expectPanelCenterResolvesRootCenterChildLeft(bool style_variant)
{
    auto lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    auto first_line = document->getObjectById("line-one");
    ASSERT_TRUE(first_line);

    // Root already centre-aligned, first hard line overrides to left. The
    // override is authored either as a style property or as presentation
    // attributes; both must be detectable and removable.
    if (style_variant) {
        lines->setAttribute("style",
            "font-family:sans-serif;font-size:20px;text-align:center;text-anchor:middle");
        first_line->setAttribute("style", "text-align:left;text-anchor:start");
    } else {
        lines->setAttribute("text-align", "center");
        lines->setAttribute("text-anchor", "middle");
        first_line->setAttribute("text-align", "left");
        first_line->setAttribute("text-anchor", "start");
    }
    checkpoint();

    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    first_line = document->getObjectById("line-one");
    ASSERT_TRUE(first_line);
    ASSERT_TRUE(first_line->style);
    EXPECT_EQ(first_line->style->text_align.computed, SP_CSS_TEXT_ALIGN_LEFT);

    UI::Tools::TextTool *tool = nullptr;
    placeCaretInFirstHardLine(desktop, lines, tool);

    Gtk::Window window;
    auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
    panel->setDesktop(desktop);
    window.set_child(*panel);
    window.present();
    drainMainContext();

    expandPanelSection(*panel, "Paragraph");
    drainMainContext();

    // The mixed root/child alignment means no toggle is active; a real user
    // click on the centre control must resolve the child override.
    auto *center = findAlignmentButton(*panel, "Center paragraph");
    ASSERT_TRUE(center);
    center->set_active(true);
    drainMainContext();
    document->ensureUpToDate();

    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    first_line = document->getObjectById("line-one");
    ASSERT_TRUE(first_line);
    ASSERT_TRUE(first_line->style);
    EXPECT_EQ(first_line->style->text_align.computed, SP_CSS_TEXT_ALIGN_CENTER);
    EXPECT_EQ(first_line->style->text_anchor.computed, SP_CSS_TEXT_ANCHOR_MIDDLE);
    expectCommonLineCentre(alignmentLineSpans(*lines), 2);

    // A repeated identical request is a true no-op: same XML and no new Undo
    // entry. The panel connects to ::toggled, so re-emit that widget signal.
    auto const resolved_xml = xml();
    document->setModifiedSinceSave(false);
    g_signal_emit_by_name(center->gobj(), "toggled");
    drainMainContext();
    document->ensureUpToDate();
    EXPECT_EQ(xml(), resolved_xml);
    EXPECT_FALSE(document->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_FALSE(DocumentUndo::undo(document));
    window.unset_child();
}

void TextMultiStyleTest::expectPanelCenterResolvesRootCenterChildAnchorOnly(bool style_variant)
{
    auto lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    auto first_line = document->getObjectById("line-one");
    ASSERT_TRUE(first_line);

    // Root already centre-aligned, first hard line authors only a start anchor.
    // text-align stays inherited centre, so the computed alignment alone looks
    // settled while the rendered line is anchored start. The unrelated bold
    // weight must survive the alignment normalization.
    if (style_variant) {
        lines->setAttribute("style",
            "font-family:sans-serif;font-size:20px;text-align:center;text-anchor:middle");
        first_line->setAttribute("style", "text-anchor:start;font-weight:bold");
    } else {
        lines->setAttribute("text-align", "center");
        lines->setAttribute("text-anchor", "middle");
        first_line->setAttribute("text-anchor", "start");
        first_line->setAttribute("font-weight", "bold");
    }
    checkpoint();

    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    first_line = document->getObjectById("line-one");
    ASSERT_TRUE(first_line);
    ASSERT_TRUE(first_line->style);
    EXPECT_EQ(first_line->style->text_align.computed, SP_CSS_TEXT_ALIGN_CENTER);
    EXPECT_EQ(first_line->style->text_anchor.computed, SP_CSS_TEXT_ANCHOR_START);

    UI::Tools::TextTool *tool = nullptr;
    placeCaretInFirstHardLine(desktop, lines, tool);

    Gtk::Window window;
    auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
    panel->setDesktop(desktop);
    window.set_child(*panel);
    window.present();
    drainMainContext();

    expandPanelSection(*panel, "Paragraph");
    drainMainContext();

    // The anchor-only conflict must surface as mixed, so the centre control is
    // enabled and clickable rather than pre-selected as an already-settled value.
    auto *center = findAlignmentButton(*panel, "Center paragraph");
    ASSERT_TRUE(center);
    EXPECT_TRUE(center->get_sensitive());
    EXPECT_FALSE(center->get_active());
    center->set_active(true);
    drainMainContext();
    document->ensureUpToDate();

    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    first_line = document->getObjectById("line-one");
    ASSERT_TRUE(first_line);
    ASSERT_TRUE(first_line->style);
    EXPECT_EQ(first_line->style->text_align.computed, SP_CSS_TEXT_ALIGN_CENTER);
    EXPECT_EQ(first_line->style->text_anchor.computed, SP_CSS_TEXT_ANCHOR_MIDDLE);
    // Only the two alignment properties are removed; unrelated style survives.
    EXPECT_EQ(first_line->style->font_weight.computed, 700);
    expectCommonLineCentre(alignmentLineSpans(*lines), 2);

    // A repeated identical request is a true no-op: same XML and no new Undo
    // entry. The panel connects to ::toggled, so re-emit that widget signal.
    auto const resolved_xml = xml();
    document->setModifiedSinceSave(false);
    g_signal_emit_by_name(center->gobj(), "toggled");
    drainMainContext();
    document->ensureUpToDate();
    EXPECT_EQ(xml(), resolved_xml);
    EXPECT_FALSE(document->isModifiedSinceSave());
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_FALSE(DocumentUndo::undo(document));
    window.unset_child();
}

// (1) The real TextPanel centre control, with the caret in the first hard line,
// must centre every rendered line of the ordinary multiline <text>.
TEST_F(TextMultiStyleTest, PanelCenterButtonCentresEveryRenderedLine)
{
    auto lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    UI::Tools::TextTool *tool = nullptr;
    placeCaretInFirstHardLine(desktop, lines, tool);

    Gtk::Window window;
    auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
    panel->setDesktop(desktop);
    window.set_child(*panel);
    window.present();
    drainMainContext();

    expandPanelSection(*panel, "Paragraph");
    drainMainContext();

    auto *center = findAlignmentButton(*panel, "Center paragraph");
    ASSERT_TRUE(center);
    center->set_active(true);
    drainMainContext();
    document->ensureUpToDate();

    // Common per-line centre is the native outcome oracle. Native geometry may
    // move the visible ink bounds by about a pixel (the native frame width is
    // not the ink bounds), so pre/post ink-bbox identity is deliberately not
    // asserted; the two entry points are compared in test (2) instead.
    expectCommonLineCentre(alignmentLineSpans(*lines), 2);
    window.unset_child();
}

// (2) The native TextToolbar centre control and the TextPanel centre control
// must agree on the rendered per-line ink bounds and the native root anchor
// when both are applied to the same restored fixture, with exactly one
// Undo/Redo each and exact XML restoration.
TEST_F(TextMultiStyleTest, ToolbarAndPanelCenterAgreeAndUndoRedoRestoresPositions)
{
    auto lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    UI::Tools::TextTool *tool = nullptr;
    placeCaretInFirstHardLine(desktop, lines, tool);

    auto const initial_xml = xml();
    auto const initial_anchor = lines->attributes.firstXY();

    std::map<unsigned, AlignmentLineSpan> panel_spans;
    Geom::Point panel_anchor;
    {
        Gtk::Window window;
        auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
        panel->setDesktop(desktop);
        window.set_child(*panel);
        window.present();
        drainMainContext();

        expandPanelSection(*panel, "Paragraph");
        drainMainContext();

        auto *center = findAlignmentButton(*panel, "Center paragraph");
        ASSERT_TRUE(center);
        center->set_active(true);
        drainMainContext();
        document->ensureUpToDate();

        panel_spans = alignmentLineSpans(*lines);
        panel_anchor = lines->attributes.firstXY();
        window.unset_child();
    }
    expectCommonLineCentre(panel_spans, 2);

    // One Undo restores the authored XML and the native anchor exactly; the
    // object is reacquired because an undo may replace it.
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    EXPECT_EQ(xml(), initial_xml);
    EXPECT_NEAR(lines->attributes.firstXY()[Geom::X], initial_anchor[Geom::X], 1e-6);
    EXPECT_NEAR(lines->attributes.firstXY()[Geom::Y], initial_anchor[Geom::Y], 1e-6);

    // The native toolbar control from the same restored initial state.
    Inkscape::UI::Toolbar::TextToolbar toolbar;
    toolbar.setDesktop(desktop);
    auto *native_center = findAlignmentButton(toolbar, "Align center");
    ASSERT_TRUE(native_center);
    g_signal_emit_by_name(native_center->gobj(), "clicked");
    drainMainContext();
    document->ensureUpToDate();

    auto const toolbar_spans = alignmentLineSpans(*lines);
    expectCommonLineCentre(toolbar_spans, 2);
    // The panel and the toolbar are the same native operation, so from the same
    // restored fixture they must give identical rendered ink bounds and root
    // anchor. (The ~1px frame-vs-ink shift is between pre- and post-alignment,
    // never between the two entry points.)
    ASSERT_EQ(toolbar_spans.size(), panel_spans.size());
    for (auto const &[index, toolbar_span] : toolbar_spans) {
        ASSERT_TRUE(panel_spans.count(index));
        auto const &panel_span = panel_spans.at(index);
        EXPECT_NEAR(panel_span.min_x, toolbar_span.min_x, 1e-6);
        EXPECT_NEAR(panel_span.max_x, toolbar_span.max_x, 1e-6);
    }
    EXPECT_NEAR(lines->attributes.firstXY()[Geom::X], panel_anchor[Geom::X], 1e-6);
    EXPECT_NEAR(lines->attributes.firstXY()[Geom::Y], panel_anchor[Geom::Y], 1e-6);

    // One Undo restores; Redo restores the centred result exactly.
    auto const centered_xml = xml();
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    EXPECT_EQ(xml(), initial_xml);
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    EXPECT_EQ(xml(), centered_xml);
    auto const redone_spans = alignmentLineSpans(*lines);
    ASSERT_EQ(redone_spans.size(), toolbar_spans.size());
    for (auto const &[index, redone_span] : redone_spans) {
        ASSERT_TRUE(toolbar_spans.count(index));
        auto const &toolbar_span = toolbar_spans.at(index);
        EXPECT_NEAR(redone_span.min_x, toolbar_span.min_x, 1e-6);
        EXPECT_NEAR(redone_span.max_x, toolbar_span.max_x, 1e-6);
    }

    // The bare toolbar under test is not owned by a window/desktop. Detach it
    // explicitly: ~TextToolbar() is `= default` and never disconnects, so a
    // still-connected slot would fire during TearDown's destroyDesktop and
    // dereference the freed toolbar (observed as SIGSEGV in _cursorMoved).
    toolbar.setDesktop(nullptr);
}

// (3) A repeated centre request is a true no-op, and a per-line alignment
// override left by the old panel (first hard line centred, owning <text> still
// start) must not make the object-scoped request skip; every line centres.
TEST_F(TextMultiStyleTest, RepeatedCenterIsNoOpAndLineOverrideIsResolved)
{
    auto lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    auto first_line = document->getObjectById("line-one");
    ASSERT_TRUE(first_line);
    first_line->setAttribute("text-anchor", "middle");
    first_line->setAttribute("text-align", "center");
    checkpoint();

    UI::Tools::TextTool *tool = nullptr;
    placeCaretInFirstHardLine(desktop, lines, tool);

    Inkscape::UI::Toolbar::TextToolbar toolbar;
    toolbar.setDesktop(desktop);
    auto *center = findAlignmentButton(toolbar, "Align center");
    ASSERT_TRUE(center);

    g_signal_emit_by_name(center->gobj(), "clicked");
    drainMainContext();
    document->ensureUpToDate();
    ASSERT_TRUE(lines->style);
    EXPECT_EQ(lines->style->text_align.computed, SP_CSS_TEXT_ALIGN_CENTER);
    auto const centered_xml = xml();
    expectCommonLineCentre(alignmentLineSpans(*lines), 2);

    document->setModifiedSinceSave(false);
    g_signal_emit_by_name(center->gobj(), "clicked");
    drainMainContext();
    document->ensureUpToDate();
    EXPECT_EQ(xml(), centered_xml);
    EXPECT_FALSE(document->isModifiedSinceSave());
    // Only the first request owns an Undo entry; the repeated one added none.
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_FALSE(DocumentUndo::undo(document));

    // See test (2): detach the bare toolbar before TearDown destroys the
    // desktop, because ~TextToolbar() does not disconnect its signals.
    toolbar.setDesktop(nullptr);
}

// (4a) Root authors centre, first hard line authors a conflicting left style
// property. The panel centre control must resolve the conflict (drop the child
// override so the whole object shares one centre); a repeat is a no-op.
TEST_F(TextMultiStyleTest, PanelCenterResolvesRootCenterChildLeftStyleConflict)
{
    expectPanelCenterResolvesRootCenterChildLeft(true);
}

// (4b) Same conflict authored as presentation attributes.
TEST_F(TextMultiStyleTest, PanelCenterResolvesRootCenterChildLeftAttributeConflict)
{
    expectPanelCenterResolvesRootCenterChildLeft(false);
}

// (4c) Root authors centre, first hard line authors only a conflicting start
// text-anchor as a style property; inherited text-align still reads centre, so
// the computed alignment alone would wrongly skip the repair.
TEST_F(TextMultiStyleTest, PanelCenterResolvesRootCenterChildAnchorOnlyStyle)
{
    expectPanelCenterResolvesRootCenterChildAnchorOnly(true);
}

// (4d) Same anchor-only conflict authored as a presentation attribute.
TEST_F(TextMultiStyleTest, PanelCenterResolvesRootCenterChildAnchorOnlyAttribute)
{
    expectPanelCenterResolvesRootCenterChildAnchorOnly(false);
}

// (LH1) Ordinary multiline text authored with a 50 px root strut at font-size
// 20 px. A caret request for 20 px must reach the native whole-object scope, so
// the real TextPanel and the real native TextToolbar must produce the same
// document-space baseline gap from the same restored fixture. The unselected
// run's explicit font/fill must survive, one Undo must restore the authored XML,
// and save/reopen must preserve the rendered baselines.
TEST_F(TextMultiStyleTest, PanelAndToolbarLineHeightAgreeWithCaret)
{
    ASSERT_TRUE(configureLineHeightFixture(
        document, "font-family:sans-serif;font-size:20px;line-height:50px", false));
    checkpoint();

    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    auto const initial_xml = xml();
    auto const initial = measureBaselineGap(*lines);
    ASSERT_EQ(initial.line_y.size(), 2u);
    ASSERT_NEAR(initial.gap, 50.0, 0.01) << "authored 50px strut must start at 50 document px";

    double panel_gap = 0.0;
    {
        UI::Tools::TextTool *tool = nullptr;
        placeCaretInFirstHardLine(desktop, lines, tool);

        Gtk::Window window;
        auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
        panel->setDesktop(desktop);
        window.set_child(*panel);
        window.present();
        drainMainContext();
        expandPanelSection(*panel, "Paragraph");
        drainMainContext();

        // Select the real px unit before entering the absolute value; without
        // this the default "lines" unit makes 20 mean 20 lines, not 20 px.
        auto *unit = panelLineHeightUnitDropdown(*panel);
        ASSERT_TRUE(unit);
        unit->set_selected(static_cast<guint>(UI::TextLineHeightUnit::Px));
        drainMainContext();
        ASSERT_EQ(unit->get_selected(), static_cast<guint>(UI::TextLineHeightUnit::Px))
            << "line-height unit must be px before entering an absolute value";

        auto *spin = panelLineHeightSpin(*panel);
        ASSERT_TRUE(spin);
        spin->set_value(20.0);
        drainMainContext();
        document->ensureUpToDate();

        panel_gap = measureLineTextGap(document);
        expectUnselectedRunStylePreserved(document);
        std::cout << "[LH1] authored gap=" << initial.gap << " panel gap=" << panel_gap << std::endl;
        window.unset_child();
    }

    // One Undo restores the authored XML and baselines exactly; reacquire after
    // Undo because the object may be replaced.
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);
    EXPECT_NEAR(measureLineTextGap(document), initial.gap, 1e-6);

    double toolbar_gap = 0.0;
    std::string toolbar_xml;
    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        desktop->getSelection()->set(lines);
        Inkscape::UI::Toolbar::TextToolbar toolbar;
        toolbar.setDesktop(desktop);
        // Place the caret after the toolbar connects so its cursor-moved slot
        // selects the native whole-object scope.
        UI::Tools::TextTool *tool = nullptr;
        placeCaretInFirstHardLine(desktop, lines, tool);

        auto *spin = toolbarLineHeightSpin(toolbar);
        ASSERT_TRUE(spin);
        spin->set_value(20.0);
        drainMainContext();
        document->ensureUpToDate();

        toolbar_gap = measureLineTextGap(document);
        toolbar_xml = xml();
        expectUnselectedRunStylePreserved(document);
        std::cout << "[LH1] toolbar gap=" << toolbar_gap << std::endl;

        // One Undo restores; Redo restores the toolbar result exactly.
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), initial_xml);
        EXPECT_NEAR(measureLineTextGap(document), initial.gap, 1e-6);
        ASSERT_TRUE(DocumentUndo::redo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), toolbar_xml);
        EXPECT_NEAR(measureLineTextGap(document), toolbar_gap, 1e-6);

        // See ToolbarAndPanelCenterAgree...: detach the bare toolbar before
        // TearDown destroys the desktop, because ~TextToolbar() does not
        // disconnect its signals.
        toolbar.setDesktop(nullptr);
    }

    // The native toolbar is the oracle: an absolute 20px line-height on an
    // untransformed ordinary text is 20 document px between baselines.
    EXPECT_NEAR(toolbar_gap, 20.0, 0.1);
    EXPECT_NEAR(panel_gap, toolbar_gap, 0.1)
        << "panel=" << panel_gap << " toolbar=" << toolbar_gap;

    // Save/reopen preserves the rendered baseline distance.
    auto const saved = sp_repr_save_buf(document->getReprDoc()).raw();
    std::unique_ptr<SPDocument> reopened{SPDocument::createNewDocFromMem(saved)};
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    EXPECT_NEAR(measureLineTextGap(reopened.get()), toolbar_gap, 1e-6);
}

// (LH2) Root strut 50 px with the unselected second hard line authored at 20 px,
// so the object has genuinely mixed per-line heights. The mixed heights must not
// make the panel drop or misconstrue the object request for 20 px: the panel
// outcome must match the native toolbar on the same restored fixture. The
// mixed-line gap can be a half-sum, so only the panel/toolbar differential is
// asserted, never a guessed 20/50 value.
TEST_F(TextMultiStyleTest, PanelLineHeightNotSkippedWhenRootAndChildDiffer)
{
    ASSERT_TRUE(configureLineHeightFixture(
        document, "font-family:sans-serif;font-size:20px;line-height:50px", true));
    checkpoint();

    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    auto const initial_xml = xml();
    auto const initial = measureBaselineGap(*lines);
    ASSERT_EQ(initial.line_y.size(), 2u);

    double panel_gap = 0.0;
    bool panel_changed = false;
    {
        UI::Tools::TextTool *tool = nullptr;
        placeCaretInFirstHardLine(desktop, lines, tool);

        Gtk::Window window;
        auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
        panel->setDesktop(desktop);
        window.set_child(*panel);
        window.present();
        drainMainContext();
        expandPanelSection(*panel, "Paragraph");
        drainMainContext();

        // The mixed snapshot leaves the visible unit at "lines"; select the
        // real px unit before entering 20 so the request is an absolute 20 px.
        auto *unit = panelLineHeightUnitDropdown(*panel);
        ASSERT_TRUE(unit);
        unit->set_selected(static_cast<guint>(UI::TextLineHeightUnit::Px));
        drainMainContext();
        ASSERT_EQ(unit->get_selected(), static_cast<guint>(UI::TextLineHeightUnit::Px))
            << "line-height unit must be px before entering an absolute value";

        auto *spin = panelLineHeightSpin(*panel);
        ASSERT_TRUE(spin);
        spin->set_value(20.0);
        drainMainContext();
        document->ensureUpToDate();

        panel_gap = measureLineTextGap(document);
        panel_changed = xml() != initial_xml;
        // The object has mixed authored heights; the object request must not be
        // discarded as an already-settled no-op.
        EXPECT_TRUE(panel_changed) << "panel skipped the 20px request on mixed root/child heights";
        std::cout << "[LH2] authored gap=" << initial.gap << " panel gap=" << panel_gap
                  << " panel_changed=" << panel_changed << std::endl;
        window.unset_child();
    }

    if (panel_changed) {
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), initial_xml);
        EXPECT_NEAR(measureLineTextGap(document), initial.gap, 1e-6);
    }

    double toolbar_gap = 0.0;
    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        desktop->getSelection()->set(lines);
        Inkscape::UI::Toolbar::TextToolbar toolbar;
        toolbar.setDesktop(desktop);
        UI::Tools::TextTool *tool = nullptr;
        placeCaretInFirstHardLine(desktop, lines, tool);

        auto *spin = toolbarLineHeightSpin(toolbar);
        ASSERT_TRUE(spin);
        spin->set_value(20.0);
        drainMainContext();
        document->ensureUpToDate();

        toolbar_gap = measureLineTextGap(document);
        expectUnselectedRunStylePreserved(document);
        std::cout << "[LH2] toolbar gap=" << toolbar_gap << std::endl;
        toolbar.setDesktop(nullptr);
    }

    EXPECT_NEAR(toolbar_gap, 20.0, 0.1);
    EXPECT_NEAR(panel_gap, toolbar_gap, 0.1)
        << "panel=" << panel_gap << " toolbar=" << toolbar_gap;
}

// (LH3) The same absolute 20 px request on text scaled 2x must produce the same
// document-unit baseline gap through the panel as through the native toolbar,
// which pre-scales absolute CSS into the item's local coordinate system.
// Baselines are converted through i2doc_affine, so the comparison is in
// document units and does not depend on the local strut.
TEST_F(TextMultiStyleTest, ScaledTextPanelLineHeightMatchesNativeDocumentSpacing)
{
    ASSERT_TRUE(configureLineHeightFixture(
        document, "font-family:sans-serif;font-size:20px;line-height:50px", false));
    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    lines->setAttribute("transform", "scale(2)");
    checkpoint();

    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    ASSERT_NEAR(lines->i2doc_affine().descrim(), 2.0, 1e-6);
    auto const initial_xml = xml();
    auto const initial = measureBaselineGap(*lines);
    ASSERT_EQ(initial.line_y.size(), 2u);
    ASSERT_NEAR(initial.gap, 100.0, 0.01) << "50px local strut under 2x scale is 100 document px";

    double panel_gap = 0.0;
    {
        UI::Tools::TextTool *tool = nullptr;
        placeCaretInFirstHardLine(desktop, lines, tool);

        Gtk::Window window;
        auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
        panel->setDesktop(desktop);
        window.set_child(*panel);
        window.present();
        drainMainContext();
        expandPanelSection(*panel, "Paragraph");
        drainMainContext();

        // Select the real px unit before entering the absolute value so the
        // scaled-text request is measured in document px, not in lines.
        auto *unit = panelLineHeightUnitDropdown(*panel);
        ASSERT_TRUE(unit);
        unit->set_selected(static_cast<guint>(UI::TextLineHeightUnit::Px));
        drainMainContext();
        ASSERT_EQ(unit->get_selected(), static_cast<guint>(UI::TextLineHeightUnit::Px))
            << "line-height unit must be px before entering an absolute value";

        auto *spin = panelLineHeightSpin(*panel);
        ASSERT_TRUE(spin);
        spin->set_value(20.0);
        drainMainContext();
        document->ensureUpToDate();

        panel_gap = measureLineTextGap(document);
        std::cout << "[LH3] authored gap=" << initial.gap << " panel gap=" << panel_gap << std::endl;
        window.unset_child();
    }

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);
    EXPECT_NEAR(measureLineTextGap(document), initial.gap, 1e-6);

    double toolbar_gap = 0.0;
    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        desktop->getSelection()->set(lines);
        Inkscape::UI::Toolbar::TextToolbar toolbar;
        toolbar.setDesktop(desktop);
        UI::Tools::TextTool *tool = nullptr;
        placeCaretInFirstHardLine(desktop, lines, tool);

        auto *spin = toolbarLineHeightSpin(toolbar);
        ASSERT_TRUE(spin);
        spin->set_value(20.0);
        drainMainContext();
        document->ensureUpToDate();

        toolbar_gap = measureLineTextGap(document);
        expectUnselectedRunStylePreserved(document);
        std::cout << "[LH3] toolbar gap=" << toolbar_gap << std::endl;
        toolbar.setDesktop(nullptr);
    }

    // Native absolute 20 px under a 2x item transform is 20 document px.
    EXPECT_NEAR(toolbar_gap, 20.0, 0.1);
    EXPECT_NEAR(panel_gap, toolbar_gap, 0.1)
        << "panel=" << panel_gap << " toolbar=" << toolbar_gap;
}

// (LH4) Ordinary multiline <text>, 50 px root strut at font-size 20 px, with a
// real explicit one-character subrange inside the LAST hard line (not a caret,
// not the full range). Native inner-range semantics must make the real TextPanel
// and the real TextToolbar produce the SAME rendered baseline origins from the
// same restored fixture, preserve the unselected line's font/fill, and each own
// exactly one Undo. Before inner reuse the panel expands the partial range to
// the whole paragraph, so the origins are expected to diverge; the measured
// values are printed and preserved, not guessed.
TEST_F(TextMultiStyleTest, PanelInnerRangeLineHeightMatchesNativeBaselineOrigins)
{
    ASSERT_TRUE(configureSubrangeLineHeightFixture(document));
    checkpoint();

    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    auto const initial_xml = xml();
    auto const initial = measureBaselineGap(*lines);
    ASSERT_EQ(initial.line_y.size(), 2u);
    ASSERT_NEAR(initial.gap, 50.0, 0.01) << "authored 50px strut must start at 50 document px";
    writeEvidenceSvg("LH4-A-before", initial_xml);

    std::vector<double> panel_y;
    {
        UI::Tools::TextTool *tool = nullptr;
        ASSERT_TRUE(placeCharacterSelectionInLastHardLine(desktop, lines, tool));

        Gtk::Window window;
        auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
        panel->setDesktop(desktop);
        window.set_child(*panel);
        window.present();
        drainMainContext();
        expandPanelSection(*panel, "Paragraph");
        drainMainContext();

        // Select the real px unit before entering the absolute value; without
        // this the default "lines" unit makes 20 mean 20 lines, not 20 px.
        auto *unit = panelLineHeightUnitDropdown(*panel);
        ASSERT_TRUE(unit);
        unit->set_selected(static_cast<guint>(UI::TextLineHeightUnit::Px));
        drainMainContext();
        ASSERT_EQ(unit->get_selected(), static_cast<guint>(UI::TextLineHeightUnit::Px))
            << "line-height unit must be px before entering an absolute value";

        auto *spin = panelLineHeightSpin(*panel);
        ASSERT_TRUE(spin);
        spin->set_value(20.0);
        drainMainContext();
        document->ensureUpToDate();

        panel_y = measureBaselineGap(*lines).line_y;
        writeEvidenceSvg("LH4-A-panel-after", xml());
        expectBoldFillPreserved(document->getObjectById("line-one"), "panel unselected line-one");
        std::cout << "[LH4] authored y=";
        for (double y : initial.line_y) std::cout << y << ",";
        std::cout << " panel y=";
        for (double y : panel_y) std::cout << y << ",";
        std::cout << std::endl;
        window.unset_child();
    }

    // One Undo restores the authored XML and baselines exactly; no second entry.
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);
    {
        auto *restored = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(restored);
        auto const restored_y = measureBaselineGap(*restored).line_y;
        ASSERT_EQ(restored_y.size(), initial.line_y.size());
        for (size_t i = 0; i < initial.line_y.size(); ++i)
            EXPECT_NEAR(restored_y[i], initial.line_y[i], 1e-6);
        EXPECT_FALSE(DocumentUndo::undo(document));
    }

    std::vector<double> native_y;
    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        desktop->getSelection()->set(lines);
        Inkscape::UI::Toolbar::TextToolbar toolbar;
        toolbar.setDesktop(desktop);
        UI::Tools::TextTool *tool = nullptr;
        // Place the partial range after the toolbar connects so its
        // cursor-moved slot selects the native inner scope.
        if (!placeCharacterSelectionInLastHardLine(desktop, lines, tool)) {
            toolbar.setDesktop(nullptr);
            FAIL() << "could not place partial selection in the last hard line";
            return;
        }

        auto *spin = toolbarLineHeightSpin(toolbar);
        if (!spin) {
            toolbar.setDesktop(nullptr);
            FAIL() << "toolbar line-height spin not found";
            return;
        }
        spin->set_value(20.0);
        drainMainContext();
        document->ensureUpToDate();

        // Detach the bare toolbar before any fatal assertion can return early.
        toolbar.setDesktop(nullptr);

        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        native_y = measureBaselineGap(*lines).line_y;
        writeEvidenceSvg("LH4-A-native-after", xml());
        expectBoldFillPreserved(document->getObjectById("line-one"), "native unselected line-one");
        std::cout << "[LH4] native y=";
        for (double y : native_y) std::cout << y << ",";
        std::cout << std::endl;

        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), initial_xml);
        EXPECT_FALSE(DocumentUndo::undo(document));
    }

    // Every rendered baseline origin must agree between the two real entry
    // points; before inner reuse the panel expands the partial range and
    // diverges. Compare all origins, never a guessed mixed delta.
    ASSERT_EQ(panel_y.size(), native_y.size());
    for (size_t i = 0; i < panel_y.size(); ++i) {
        EXPECT_NEAR(panel_y[i], native_y[i], 0.1)
            << "line " << i << " panel=" << panel_y[i] << " native=" << native_y[i];
    }
}

// (LH5) Ordinary FOUR hard lines, 50 px root strut at font-size 20 px, with the
// first two UNSELECTED lines authored at an explicit 80 px strut. A native
// inner-range request for 20 px on one character of the LAST line must not
// renormalize the unselected lines: their rendered first-two baseline
// separation and authored 80 px/bold/fill run styles must be preserved. Source
// inspection suggests the native branch currently overwrites 80 -> 50; this is
// the runtime measurement before any repair, and the mixed lower-line deltas
// are measured from the fixture rather than guessed.
TEST_F(TextMultiStyleTest, NativeInnerRangePreservesUnselectedAuthoredLineHeights)
{
    ASSERT_TRUE(configureFourLineLineHeightFixture(document));
    checkpoint();

    auto *lines = cast<SPText>(document->getObjectById("line-text-four"));
    ASSERT_TRUE(lines);
    auto const initial_xml = xml();
    auto const initial = measureBaselineGap(*lines);
    ASSERT_EQ(initial.line_y.size(), 4u);
    double const initial_first_gap = initial.line_y[1] - initial.line_y[0];
    writeEvidenceSvg("LH5-B-before", initial_xml);
    std::cout << "[LH5] authored y=";
    for (double y : initial.line_y) std::cout << y << ",";
    std::cout << " first_gap=" << initial_first_gap << std::endl;
    EXPECT_NEAR(initial_first_gap, 80.0, 0.5)
        << "two unselected lines both author 80px; measured first gap " << initial_first_gap;

    {
        desktop->getSelection()->set(lines);
        Inkscape::UI::Toolbar::TextToolbar toolbar;
        toolbar.setDesktop(desktop);
        UI::Tools::TextTool *tool = nullptr;
        // One character inside the LAST hard line, after the toolbar connects.
        if (!placeCharacterSelectionInLastHardLine(desktop, lines, tool)) {
            toolbar.setDesktop(nullptr);
            FAIL() << "could not place partial selection in the last hard line";
            return;
        }

        auto *spin = toolbarLineHeightSpin(toolbar);
        if (!spin) {
            toolbar.setDesktop(nullptr);
            FAIL() << "toolbar line-height spin not found";
            return;
        }
        spin->set_value(20.0);
        drainMainContext();
        document->ensureUpToDate();

        // Detach before any fatal assertion can return early.
        toolbar.setDesktop(nullptr);

        lines = cast<SPText>(document->getObjectById("line-text-four"));
        ASSERT_TRUE(lines);
        auto const after = measureBaselineGap(*lines);
        double const after_first_gap =
            after.line_y.size() >= 2 ? after.line_y[1] - after.line_y[0] : 0.0;
        writeEvidenceSvg("LH5-B-native-after", xml());

        auto *one = document->getObjectById("four-one");
        auto *two = document->getObjectById("four-two");
        double const one_lh = one && one->style ? one->style->line_height.computed : -1.0;
        double const two_lh = two && two->style ? two->style->line_height.computed : -1.0;
        std::cout << "[LH5] native y=";
        for (double y : after.line_y) std::cout << y << ",";
        std::cout << " first_gap=" << after_first_gap << " four-one_lh=" << one_lh
                  << " four-two_lh=" << two_lh << std::endl;

        // Observed runtime proof for the current native behavior.
        EXPECT_NEAR(after_first_gap, initial_first_gap, 0.1)
            << "first-two baseline separation changed: authored=" << initial_first_gap
            << " native=" << after_first_gap;
        EXPECT_NEAR(one_lh, 80.0, 0.01) << "four-one authored 80px strut overwritten";
        EXPECT_NEAR(two_lh, 80.0, 0.01) << "four-two authored 80px strut overwritten";
        expectBoldFillPreserved(one, "four-one");
        expectBoldFillPreserved(two, "four-two");

        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), initial_xml);
        EXPECT_FALSE(DocumentUndo::undo(document));
    }
}

TEST(TextTargetCollectionTest, RecordsCollectionCostForOneHundredAndThousandDescendants)
{
    // Application::create() is first-call-wins and only registers the custom
    // GTK widget types when created with GUI enabled. A non-GUI fixture that
    // runs first (e.g. under --gtest_shuffle) must therefore not install the
    // non-GUI singleton for the whole binary, or later GUI fixtures fail while
    // loading statusbar.ui. In GUI mode follow the same initialization contract
    // as TextMultiStyleTest; the headless path still runs this case without GUI.
    auto const gui = std::getenv("INKSCAPE_TEST_GUI");
    if (gui && std::string(gui) == "1") {
        auto &application = testApplication();
        ASSERT_TRUE(application.gtk_app());
        ASSERT_TRUE(Application::exists());
    } else if (!Application::exists()) {
        Application::create(false);
    }
    for (unsigned count : {1u, 100u, 1000u}) {
        std::string svg = "<svg xmlns='http://www.w3.org/2000/svg'><g id='group'>";
        for (unsigned i = 0; i < count; ++i) {
            svg += "<g><text id='t" + std::to_string(i) + "'>text</text><path d='M0,0 L1,1'/></g>";
        }
        svg += "</g></svg>";
        auto document = SPDocument::createNewDocFromMem(svg);
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        std::vector<SPItem *> direct;
        for (unsigned i = 0; i < count; ++i) direct.push_back(cast<SPItem>(document->getObjectById("t" + std::to_string(i))));
        auto group = cast<SPItem>(document->getObjectById("group"));
        auto started = std::chrono::steady_clock::now();
        auto grouped = UI::collectTextItems({group});
        auto const group_time = std::chrono::steady_clock::now() - started;
        started = std::chrono::steady_clock::now();
        auto selected = UI::collectTextItems(direct);
        auto const direct_time = std::chrono::steady_clock::now() - started;
        EXPECT_EQ(grouped, direct);
        EXPECT_EQ(selected, direct);
        direct.insert(direct.begin(), group);
        EXPECT_EQ(UI::collectTextItems(direct), grouped);
        std::cout << "TEXT-001 collection count=" << count
                  << " grouped_us=" << std::chrono::duration_cast<std::chrono::microseconds>(group_time).count()
                  << " direct_us=" << std::chrono::duration_cast<std::chrono::microseconds>(direct_time).count() << '\n';
    }
}

TEST(GuiApplicationContractTest, GuiModeRegistersCustomWidgetTypes)
{
    auto const gui = std::getenv("INKSCAPE_TEST_GUI");
    if (!gui || std::string(gui) != "1") {
        GTEST_SKIP() << "Skipping GUI integration test: GUI testing not enabled";
    }
    // The GUI initialization contract must hold whichever fixture runs first.
    // If a non-GUI fixture has already won Application::create(false), the
    // custom widget types stay unregistered and this reports the root cause
    // instead of letting a later fixture abort while loading statusbar.ui.
    auto &application = testApplication();
    ASSERT_TRUE(application.gtk_app());
    ASSERT_TRUE(Application::exists());
    EXPECT_NE(g_type_from_name("gtkmm__CustomObject_InkSpinButton"), G_TYPE_INVALID)
        << "GUI application did not register InkSpinButton; Application was created without GUI";
}

// Source-independent face identity/metrics used to compare a measured catalog
// with one reloaded from disk or resumed from the metric cache. Runtime Pango
// handles are intentionally excluded. The source-derived `id`/`source_path`
// provenance is compared separately (describeCatalogProvenanceDrift): the
// per-face metric cache does not persist a source path, so a resumed scan can
// derive a different stable id for the same face without losing identity or
// metrics. That is pre-existing cache provenance behavior, not a metric loss.
struct FaceIdentity {
    std::string family;
    std::string face;
    std::string description;
    std::string fontspec;
    std::string variations;

    std::string key() const
    {
        return family + '\x1f' + face + '\x1f' + description + '\x1f' + fontspec + '\x1f' + variations;
    }

    std::string describe() const
    {
        return "fontspec='" + fontspec + "' family='" + family + "' face='" + face +
               "' description='" + description + "' variations='" + variations + "'";
    }
};

struct FaceMetrics {
    double weight = 0.0;
    double width = 0.0;
    unsigned short family_kind = 0;
    bool monospaced = false;
    bool oblique = false;
    bool variable_font = false;
    bool synthetic = false;
    bool available = false;

    // Exact locale-independent serialization for order-independent multiset
    // comparison; doubles use round-trip precision so no drift hides behind
    // string rounding.
    std::string key() const
    {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "weight=%.17g width=%.17g family-kind=%u flags[m=%d o=%d v=%d s=%d a=%d]",
                      weight, width, static_cast<unsigned>(family_kind),
                      monospaced ? 1 : 0, oblique ? 1 : 0, variable_font ? 1 : 0,
                      synthetic ? 1 : 0, available ? 1 : 0);
        return buffer;
    }
};

struct CatalogFace {
    FaceIdentity identity;
    FaceMetrics metrics;
    std::string id;
    std::string source_path;
};

std::vector<CatalogFace> catalogFaces(FontDiscovery::FontsPayload const &fonts)
{
    std::vector<CatalogFace> out;
    if (!fonts) return out;
    for (auto const &family : *fonts) {
        for (auto const &font : family) {
            out.push_back(CatalogFace{
                FaceIdentity{font.family_name.raw(), font.face_name.raw(), font.description.raw(),
                             font.fontspec.raw(), font.variations.raw()},
                FaceMetrics{font.weight, font.width, font.family_kind, font.monospaced, font.oblique,
                            font.variable_font, font.synthetic, font.available},
                font.id, font.source_path});
        }
    }
    return out;
}

std::size_t countCatalogFaces(FontDiscovery::FontsPayload const &fonts)
{
    return catalogFaces(fonts).size();
}

std::size_t countMissingSourcePaths(FontDiscovery::FontsPayload const &fonts)
{
    std::size_t missing = 0;
    if (fonts) {
        for (auto const &family : *fonts) {
            for (auto const &font : family) {
                if (font.source_path.empty()) ++missing;
            }
        }
    }
    return missing;
}

using FaceBucket = std::map<std::string, std::vector<CatalogFace const *>>;

// Duplicate source-independent identities are kept as buckets so multiplicity
// is compared, never silently collapsed into one face.
FaceBucket bucketFacesByIdentity(std::vector<CatalogFace> const &faces)
{
    FaceBucket buckets;
    for (auto const &face : faces) buckets[face.identity.key()].push_back(&face);
    return buckets;
}

std::multiset<std::string> metricKeys(std::vector<CatalogFace const *> const &faces)
{
    std::multiset<std::string> keys;
    for (auto const *face : faces) keys.insert(face->metrics.key());
    return keys;
}

std::size_t multisetSymmetricDifference(std::multiset<std::string> const &a,
                                        std::multiset<std::string> const &b)
{
    std::vector<std::string> diff;
    std::set_symmetric_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(diff));
    return diff.size();
}

std::string firstMetricDifference(std::multiset<std::string> const &ref,
                                  std::multiset<std::string> const &got)
{
    std::vector<std::string> only_ref;
    std::set_difference(ref.begin(), ref.end(), got.begin(), got.end(), std::back_inserter(only_ref));
    std::vector<std::string> only_got;
    std::set_difference(got.begin(), got.end(), ref.begin(), ref.end(), std::back_inserter(only_got));
    std::string out;
    if (!only_ref.empty()) out += only_ref.front();
    if (!only_got.empty()) out += (out.empty() ? "vs " : " vs ") + only_got.front();
    return out;
}

std::size_t duplicateIdentityExtras(std::vector<CatalogFace> const &faces)
{
    std::size_t extras = 0;
    for (auto const &[key, bucket] : bucketFacesByIdentity(faces)) {
        (void)key;
        if (bucket.size() > 1) extras += bucket.size() - 1;
    }
    return extras;
}

// Order-independent comparison of source-independent face identity and metrics.
// Every identity bucket is compared by multiplicity and metric multiset, so a
// duplicate identity on either side is a reported difference rather than a
// silently ignored or collapsed entry. Stable-id/source-path provenance is
// deliberately excluded from this verdict; describeCatalogProvenanceDrift()
// reports it separately. Returns "identical" or the first concrete difference
// as a short string so failures stay readable without dumping thousands of
// faces.
std::string describeCatalogDifference(FontDiscovery::FontsPayload const &reference,
                                      FontDiscovery::FontsPayload const &compared)
{
    auto ref = catalogFaces(reference);
    auto got = catalogFaces(compared);
    if (ref.size() != got.size()) {
        return "face count " + std::to_string(ref.size()) + " vs " + std::to_string(got.size());
    }
    auto ref_index = bucketFacesByIdentity(ref);
    auto got_index = bucketFacesByIdentity(got);
    for (auto const &[key, faces] : ref_index) {
        auto it = got_index.find(key);
        if (it == got_index.end()) {
            return "missing face identity " + faces.front()->identity.describe();
        }
        if (faces.size() != it->second.size()) {
            return "duplicate identity count " + faces.front()->identity.describe() + ": " +
                   std::to_string(faces.size()) + " vs " + std::to_string(it->second.size());
        }
        auto ref_metrics = metricKeys(faces);
        auto got_metrics = metricKeys(it->second);
        if (ref_metrics != got_metrics) {
            return "metric drift " + faces.front()->identity.describe() + " (" +
                   std::to_string(faces.size()) + " entries): " + firstMetricDifference(ref_metrics, got_metrics);
        }
    }
    return "identical";
}

// Separate provenance report: stable id and source path are derived from the
// source file (or from a metric-cache entry that does not persist it), so they
// may legitimately drift across a resumed scan. Values are reported for the
// record, never folded into the identity/metric verdict above.
std::string describeCatalogProvenanceDrift(FontDiscovery::FontsPayload const &reference,
                                           FontDiscovery::FontsPayload const &compared)
{
    auto ref = catalogFaces(reference);
    auto got = catalogFaces(compared);
    auto ref_index = bucketFacesByIdentity(ref);
    auto got_index = bucketFacesByIdentity(got);
    std::size_t id_drift = 0;
    std::size_t source_path_drift = 0;
    for (auto const &[key, faces] : ref_index) {
        auto it = got_index.find(key);
        if (it == got_index.end() || it->second.size() != faces.size()) continue;
        std::multiset<std::string> ref_ids, got_ids, ref_paths, got_paths;
        for (auto const *face : faces) {
            ref_ids.insert(face->id);
            ref_paths.insert(face->source_path);
        }
        for (auto const *face : it->second) {
            got_ids.insert(face->id);
            got_paths.insert(face->source_path);
        }
        id_drift += multisetSymmetricDifference(ref_ids, got_ids);
        source_path_drift += multisetSymmetricDifference(ref_paths, got_paths);
    }
    return "id_drift=" + std::to_string(id_drift) + " source_path_drift=" + std::to_string(source_path_drift) +
           " duplicate_identity_extras_ref=" + std::to_string(duplicateIdentityExtras(ref)) +
           " duplicate_identity_extras_cmp=" + std::to_string(duplicateIdentityExtras(got)) +
           " missing_source_ref=" + std::to_string(countMissingSourcePaths(reference)) +
           " missing_source_cmp=" + std::to_string(countMissingSourcePaths(compared));
}

std::filesystem::path fontCatalogPath()
{
    return std::filesystem::path(Glib::build_filename(
        Inkscape::IO::Resource::profile_path(), "font-catalog-v2.ini"));
}

std::filesystem::path fontSizeCachePath()
{
    return std::filesystem::path(Glib::build_filename(
        Inkscape::IO::Resource::profile_path(), "font-cache.ini"));
}

std::set<std::string> readFontCacheGroups(std::filesystem::path const &path)
{
    std::set<std::string> groups;
    if (!std::filesystem::exists(path)) return groups;
    try {
        auto keyfile = Glib::KeyFile::create();
        if (!keyfile->load_from_file(path.string())) return groups;
        for (auto const &group : keyfile->get_groups()) {
            if (group != "@font-cache@") groups.insert(group.raw());
        }
    }
    catch (Glib::Error const &) {
    }
    return groups;
}

struct CatalogScanResult {
    FontDiscovery::FontsPayload fonts;
    std::chrono::steady_clock::duration elapsed{};
    bool finished = false;
    bool saw_family_progress = false;
    unsigned progress_reports = 0; // measured-family progress messages observed
};

// Runs one discovery scan through the public event stream. stop_on_first_family
// quits as soon as a progress message carries a measured family (used to cancel
// mid-scan); otherwise it waits for OperationFinished.
CatalogScanResult runCatalogScan(bool stop_on_first_family, unsigned ceiling_ms)
{
    CatalogScanResult result;
    auto loop = Glib::MainLoop::create();
    auto const started = std::chrono::steady_clock::now();
    sigc::scoped_connection connection = FontDiscovery::get().connect_to_fonts(
        [&](FontDiscovery::MessageType const &message) {
            if (auto progress = Async::Msg::get_progress(message)) {
                bool const measured = !std::get<2>(*progress).empty();
                result.saw_family_progress = result.saw_family_progress || measured;
                if (measured) ++result.progress_reports;
            }
            if (auto payload = Async::Msg::get_result(message)) result.fonts = *payload;
            if (Async::Msg::is_finished(message)) {
                result.finished = true;
                loop->quit();
            }
            else if (stop_on_first_family && result.saw_family_progress) {
                loop->quit();
            }
        });
    sigc::scoped_connection ceiling = Glib::signal_timeout().connect([&] {
        loop->quit();
        return false;
    }, ceiling_ms);
    // Cached discovery replays synchronously while connecting. quit() before run()
    // does not stop a later run(), so do not enter the loop after reaching our goal.
    if (!result.finished && !(stop_on_first_family && result.saw_family_progress)) {
        loop->run();
    }
    ceiling.disconnect();
    result.elapsed = std::chrono::steady_clock::now() - started;
    return result;
}

// Bounded readiness wait for an asynchronous side effect (cache flush, file
// appearance). The ceiling only terminates a hung product path.
bool pollUntil(std::function<bool()> const &predicate, unsigned ceiling_ms)
{
    auto loop = Glib::MainLoop::create();
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ceiling_ms);
    bool satisfied = predicate();
    if (!satisfied) {
        sigc::scoped_connection poll = Glib::signal_timeout().connect([&] {
            satisfied = predicate();
            bool const done = satisfied || std::chrono::steady_clock::now() >= deadline;
            if (done) loop->quit();
            return !done;
        }, 100);
        loop->run();
        poll.disconnect();
    }
    return satisfied;
}

TEST(FontCatalogGuiTest, ReloadsThePersistentCatalogWithoutPangoFaceEnumeration)
{
    auto const gui = std::getenv("INKSCAPE_TEST_GUI");
    if (!gui || std::string(gui) != "1") {
        GTEST_SKIP() << "Skipping GUI integration test: GUI testing not enabled";
    }
    ASSERT_TRUE(testApplication().gtk_app());

    auto const filename = Glib::build_filename(
        Inkscape::IO::Resource::profile_path(), "font-catalog-v2.ini");
    auto const metric_cache = Glib::build_filename(
        Inkscape::IO::Resource::profile_path(), "font-cache.ini");
    // Genuinely cold whenever this case runs, not only when it happens to be
    // the first catalog test in the process: remove both the persistent catalog
    // and the per-face metric cache.
    std::filesystem::remove(std::filesystem::path(filename));
    std::filesystem::remove(std::filesystem::path(metric_cache));
    FontDiscovery::get().invalidate();

    auto wait_for_catalog = [] {
        struct Result {
            FontDiscovery::FontsPayload fonts;
            std::chrono::steady_clock::duration elapsed;
            bool finished = false;
        } result;
        auto loop = Glib::MainLoop::create();
        auto const started = std::chrono::steady_clock::now();
        auto timeout = Glib::signal_timeout().connect([&] {
            loop->quit();
            return false;
        // A first scan of very large system collections can legitimately take
        // tens of seconds because it computes metrics for every face. This is
        // the one-time path the persistent catalog removes from later starts.
        }, 120000);
        auto connection = FontDiscovery::get().connect_to_fonts([&](auto const &message) {
            if (auto payload = Async::Msg::get_result(message)) result.fonts = *payload;
            if (Async::Msg::is_finished(message)) {
                result.finished = true;
                loop->quit();
            }
        });
        loop->run();
        result.elapsed = std::chrono::steady_clock::now() - started;
        timeout.disconnect();
        return result;
    };

    auto cold = wait_for_catalog();
    ASSERT_TRUE(cold.finished);
    ASSERT_TRUE(cold.fonts);
    ASSERT_FALSE(cold.fonts->empty());
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(filename)));
    // The 120 s timeout above is the ceiling that makes this fail; assert the
    // measured elapsed honestly against the same retained bound.
    EXPECT_LT(cold.elapsed, std::chrono::seconds(120)) << "cold catalog scan exceeded the 120 s ceiling";

    FontDiscovery::get().invalidate();
    auto warm = wait_for_catalog();
    ASSERT_TRUE(warm.finished);
    ASSERT_TRUE(warm.fonts);
    ASSERT_EQ(warm.fonts->size(), cold.fonts->size());
    EXPECT_LT(warm.elapsed, std::chrono::seconds(1));
    // Every source-independent face identity and metric from the genuinely cold
    // scan must survive the catalog round trip. Stable-id/source-path
    // provenance is recorded separately (the catalog does persist both, so any
    // drift here is informative, not part of the identity/metric verdict).
    auto const identity_metric_diff = describeCatalogDifference(cold.fonts, warm.fonts);
    auto const provenance_drift = describeCatalogProvenanceDrift(cold.fonts, warm.fonts);
    EXPECT_EQ(identity_metric_diff, "identical");
    RecordProperty("catalog_identity_metric_diff", identity_metric_diff);
    RecordProperty("catalog_provenance_drift", provenance_drift);
    std::cout << "font catalog cold="
              << std::chrono::duration_cast<std::chrono::milliseconds>(cold.elapsed).count()
              << "ms warm="
              << std::chrono::duration_cast<std::chrono::milliseconds>(warm.elapsed).count()
              << "ms cold_faces=" << countCatalogFaces(cold.fonts)
              << " warm_faces=" << countCatalogFaces(warm.fonts)
              << " cold_missing_source=" << countMissingSourcePaths(cold.fonts)
              << " warm_missing_source=" << countMissingSourcePaths(warm.fonts)
              << " diff=" << identity_metric_diff
              << " provenance=" << provenance_drift << "\n";

    std::size_t faces = 0;
    for (auto const &family : *warm.fonts) {
        for (auto const &font : family) {
            ++faces;
            EXPECT_TRUE(font.available);
            EXPECT_FALSE(font.ff);
            EXPECT_FALSE(font.face);
            EXPECT_FALSE(font.fontspec.empty());
        }
    }
    EXPECT_GT(faces, 0);
}

TEST(FontCatalogGuiTest, PartialScanIsPersistedWhenDiscoveryIsCancelled)
{
    auto const gui = std::getenv("INKSCAPE_TEST_GUI");
    if (!gui || std::string(gui) != "1") {
        GTEST_SKIP() << "Skipping GUI integration test: GUI testing not enabled";
    }
    ASSERT_TRUE(testApplication().gtk_app());

    auto const catalog_path = fontCatalogPath();
    auto const cache_path = fontSizeCachePath();

    auto remove_scan_state = [&] {
        std::filesystem::remove(catalog_path);
        std::filesystem::remove(cache_path);
    };

    // Phase A: independent genuine-cold reference in this process. Both the
    // persistent catalog and the metric cache are absent, so every face is
    // measured; the resulting identities/metrics are this case's own oracle.
    remove_scan_state();
    FontDiscovery::get().invalidate();
    drainMainContext();
    auto reference = runCatalogScan(false, 300000);
    ASSERT_TRUE(reference.finished) << "cold reference scan did not finish within 300000 ms";
    ASSERT_TRUE(reference.fonts);
    ASSERT_FALSE(reference.fonts->empty());
    ASSERT_TRUE(std::filesystem::exists(catalog_path));
    auto const ref_groups = readFontCacheGroups(cache_path);
    ASSERT_FALSE(ref_groups.empty()) << "cold reference scan wrote no metric cache";
    ASSERT_FALSE(reference.fonts->front().empty());

    // Cache group keys of the first measured family, exactly as save_font_cache()
    // derives them, so phase B can remove only that family's metrics.
    std::set<std::string> first_family_groups;
    for (auto const &font : reference.fonts->front()) {
        first_family_groups.insert(get_font_description(font.ff, font.face).to_string().raw());
    }
    ASSERT_FALSE(first_family_groups.empty());
    for (auto const &group : first_family_groups) {
        ASSERT_TRUE(ref_groups.count(group)) << "reference metric cache lacks " << group;
    }

    // Phase B: existing unvisited entries must not be truncated by a partial
    // flush. Drop only the first family's metrics, force those faces to be
    // re-measured, then cancel right after that family completes. A flush that
    // writes only the families seen so far would delete every later entry.
    {
        std::filesystem::remove(catalog_path);
        auto keyfile = Glib::KeyFile::create();
        ASSERT_TRUE(keyfile->load_from_file(cache_path.string()));
        for (auto const &group : first_family_groups) keyfile->remove_group(group);
        ASSERT_TRUE(keyfile->save_to_file(cache_path.string()));
    }
    auto const trimmed_groups = readFontCacheGroups(cache_path);
    ASSERT_EQ(trimmed_groups.size(), ref_groups.size() - first_family_groups.size());
    FontDiscovery::get().invalidate();
    drainMainContext();
    auto recheck = runCatalogScan(true, 30000);
    ASSERT_TRUE(recheck.saw_family_progress) << "re-measuring scan reported no family within 30000 ms";
    // Sanity check only: this callback deliberately quits at the first measured
    // family, so finished=false is NOT cancellation evidence. The actual
    // evidence in this phase is the union preservation below (dropped == 0 on
    // a trimmed cache that still held every other family's metrics).
    EXPECT_FALSE(recheck.finished) << "re-measuring scan completed before it could be cancelled";
    FontDiscovery::get().invalidate();
    drainMainContext();
    bool const rewritten = pollUntil([&] { return readFontCacheGroups(cache_path) != trimmed_groups; }, 30000);
    auto const after_cancel_groups = readFontCacheGroups(cache_path);
    EXPECT_TRUE(rewritten) << "cancelled scan did not rewrite the metric cache";
    std::size_t dropped = 0;
    std::string dropped_example;
    for (auto const &group : ref_groups) {
        if (!after_cancel_groups.count(group)) {
            ++dropped;
            if (dropped_example.empty()) dropped_example = group;
        }
    }
    EXPECT_EQ(dropped, 0u) << "partial flush dropped " << dropped
                           << " cached metrics for faces the scan had not visited, e.g. " << dropped_example;

    // Phase C: with the catalog still absent, a normal scan resuming from the
    // metric cache must reproduce the complete reference catalog. The metric
    // cache does not persist source paths, so id/source-path provenance is
    // recorded separately from the identity/metric verdict.
    auto resumed = runCatalogScan(false, 300000);
    ASSERT_TRUE(resumed.finished) << "resumed scan did not finish within 300000 ms";
    ASSERT_TRUE(resumed.fonts);
    auto const resumed_diff = describeCatalogDifference(reference.fonts, resumed.fonts);
    auto const resumed_provenance = describeCatalogProvenanceDrift(reference.fonts, resumed.fonts);
    EXPECT_EQ(resumed_diff, "identical");
    RecordProperty("resumed_identity_metric_diff", resumed_diff);
    RecordProperty("resumed_provenance_drift", resumed_provenance);

    // Phase D: a genuinely cold partial scan that is cancelled must persist a
    // strict subset of the completed reference coverage, and the following
    // scan plus a catalog reload must both reproduce the reference catalog.
    // The interruption proof is the real persisted metric-cache groups, not
    // the callback's finished flag (which is false by construction because the
    // callback quits at the first measured family).
    remove_scan_state();
    FontDiscovery::get().invalidate();
    drainMainContext();
    auto partial = runCatalogScan(true, 15000);
    ASSERT_TRUE(partial.saw_family_progress) << "cold scan measured no family within 15000 ms";
    EXPECT_FALSE(partial.finished) << "cold scan completed before it could be cancelled";
    FontDiscovery::get().invalidate();
    drainMainContext();
    bool const persisted = pollUntil([&] { return !readFontCacheGroups(cache_path).empty(); }, 30000);
    auto const partial_groups = readFontCacheGroups(cache_path);
    EXPECT_TRUE(persisted) << "cancelled cold scan persisted no font-cache.ini metrics";
    // Genuine interruption evidence: real measured groups were persisted
    // (nonzero) but strictly fewer than the completed cold reference wrote, so
    // the fresh scan was actually cut short rather than merely reported as
    // unfinished by the callback.
    EXPECT_GT(partial_groups.size(), 0u)
        << "interrupted cold scan persisted no real metric-cache groups";
    EXPECT_LT(partial_groups.size(), ref_groups.size())
        << "interrupted cold scan persisted the full reference coverage (" << partial_groups.size()
        << " groups); not a genuine interruption";
    std::size_t unexpected_partial = 0;
    std::string unexpected_example;
    for (auto const &group : partial_groups) {
        if (!ref_groups.count(group)) {
            ++unexpected_partial;
            if (unexpected_example.empty()) unexpected_example = group;
        }
    }
    EXPECT_EQ(unexpected_partial, 0u) << "interrupted cold scan persisted " << unexpected_partial
                                      << " groups absent from the completed reference, e.g. " << unexpected_example;

    auto recovered = runCatalogScan(false, 300000);
    ASSERT_TRUE(recovered.finished) << "resumed cold scan did not finish within 300000 ms";
    ASSERT_TRUE(recovered.fonts);
    auto const recovered_diff = describeCatalogDifference(reference.fonts, recovered.fonts);
    auto const recovered_provenance = describeCatalogProvenanceDrift(reference.fonts, recovered.fonts);
    EXPECT_EQ(recovered_diff, "identical");
    RecordProperty("recovered_identity_metric_diff", recovered_diff);
    RecordProperty("recovered_provenance_drift", recovered_provenance);

    FontDiscovery::get().invalidate();
    drainMainContext();
    auto reloaded = runCatalogScan(false, 300000);
    ASSERT_TRUE(reloaded.finished) << "catalog reload did not finish within 300000 ms";
    ASSERT_TRUE(reloaded.fonts);
    auto const reloaded_diff = describeCatalogDifference(reference.fonts, reloaded.fonts);
    auto const reloaded_provenance = describeCatalogProvenanceDrift(reference.fonts, reloaded.fonts);
    EXPECT_EQ(reloaded_diff, "identical");
    RecordProperty("reloaded_identity_metric_diff", reloaded_diff);
    RecordProperty("reloaded_provenance_drift", reloaded_provenance);

    std::cout << "TEXT-003 ref_groups=" << ref_groups.size()
              << " after_cancel_groups=" << after_cancel_groups.size()
              << " partial_groups=" << partial_groups.size()
              << " partial_progress_reports=" << partial.progress_reports
              << " faces=" << countCatalogFaces(reference.fonts)
              << " refreshed_missing_source=" << countMissingSourcePaths(resumed.fonts)
              << " recovered_missing_source=" << countMissingSourcePaths(recovered.fonts)
              << " reference_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(reference.elapsed).count()
              << " resumed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(resumed.elapsed).count()
              << " recovered_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(recovered.elapsed).count()
              << " reload_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(reloaded.elapsed).count()
              << " resumed_provenance=" << resumed_provenance
              << '\n';
}

TEST(FontCatalogGuiTest, PartialCancelPreservesUnvisitedCachedEntries)
{
    auto const gui = std::getenv("INKSCAPE_TEST_GUI");
    if (!gui || std::string(gui) != "1") {
        GTEST_SKIP() << "Skipping GUI integration test: GUI testing not enabled";
    }
    ASSERT_TRUE(testApplication().gtk_app());

    auto const catalog_path = fontCatalogPath();
    auto const cache_path = fontSizeCachePath();
    // Provably unvisited: no installed face can produce this description key.
    std::string const sentinel = "ZZZ Unvisited Sentinel Face (no installed face maps here)";

    // The catalog must be absent so the metric path runs. Seed only the
    // sentinel, so the first measured family is a genuine cache miss and the
    // partial flush has to be a union rather than a rewrite of visited faces.
    std::filesystem::remove(catalog_path);
    std::filesystem::remove(cache_path);
    {
        auto keyfile = Glib::KeyFile::create();
        keyfile->set_double("@font-cache@", "version", 1.0);
        keyfile->set_double(sentinel, "weight", 0.375);
        keyfile->set_double(sentinel, "width", 0.625);
        keyfile->set_integer(sentinel, "family", 7);
        keyfile->set_integer(sentinel, "flags", 0);
        ASSERT_TRUE(keyfile->save_to_file(cache_path.string()));
    }

    FontDiscovery::get().invalidate();
    drainMainContext();
    auto partial = runCatalogScan(true, 15000);
    ASSERT_TRUE(partial.saw_family_progress) << "cold scan measured no family within 15000 ms";
    // Sanity check only: the callback quits at the first measured family, so
    // finished=false is not cancellation evidence. This case's evidence is the
    // preserved seeded sentinel below plus nonzero freshly computed metrics.
    EXPECT_FALSE(partial.finished) << "cold scan completed before it could be cancelled";
    FontDiscovery::get().invalidate();
    drainMainContext();
    bool const flushed = pollUntil([&] { return readFontCacheGroups(cache_path).size() > 1u; }, 30000);
    auto const after_cancel = readFontCacheGroups(cache_path);
    EXPECT_TRUE(flushed) << "partial cancel persisted no computed face metrics";
    EXPECT_TRUE(after_cancel.count(sentinel))
        << "partial flush removed a cached entry the scan had not visited";
    EXPECT_GT(after_cancel.size(), 1u);
    {
        ASSERT_TRUE(std::filesystem::exists(cache_path));
        auto keyfile = Glib::KeyFile::create();
        ASSERT_TRUE(keyfile->load_from_file(cache_path.string()));
        ASSERT_TRUE(keyfile->has_group(sentinel));
        EXPECT_DOUBLE_EQ(keyfile->get_double(sentinel, "weight"), 0.375);
        EXPECT_DOUBLE_EQ(keyfile->get_double(sentinel, "width"), 0.625);
        EXPECT_EQ(keyfile->get_integer(sentinel, "family"), 7);
        EXPECT_EQ(keyfile->get_integer(sentinel, "flags"), 0);
    }

    // A following complete scan must still finish with the full set and the
    // catalog it publishes must reload identically. The sentinel is not
    // expected to survive the complete publish path, so it is not asserted.
    auto complete = runCatalogScan(false, 300000);
    ASSERT_TRUE(complete.finished) << "complete scan did not finish within 300000 ms";
    ASSERT_TRUE(complete.fonts);
    ASSERT_FALSE(complete.fonts->empty());
    FontDiscovery::get().invalidate();
    drainMainContext();
    auto reload = runCatalogScan(false, 300000);
    ASSERT_TRUE(reload.finished) << "catalog reload did not finish within 300000 ms";
    ASSERT_TRUE(reload.fonts);
    auto const reload_diff = describeCatalogDifference(complete.fonts, reload.fonts);
    auto const reload_provenance = describeCatalogProvenanceDrift(complete.fonts, reload.fonts);
    EXPECT_EQ(reload_diff, "identical");
    RecordProperty("reload_identity_metric_diff", reload_diff);
    RecordProperty("reload_provenance_drift", reload_provenance);

    std::cout << "TEXT-004 sentinel_preserved=" << after_cancel.count(sentinel)
              << " after_cancel_groups=" << after_cancel.size()
              << " fresh_metric_groups=" << (after_cancel.size() - 1u)
              << " partial_progress_reports=" << partial.progress_reports
              << " faces=" << countCatalogFaces(complete.fonts)
              << " complete_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(complete.elapsed).count()
              << " reload_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(reload.elapsed).count()
              << " reload_provenance=" << reload_provenance
              << '\n';
}

// --- Line-height unit-switch coverage through the real TextPanel ------------
//
// After the native whole-object reuse, queryParagraph() reports ordinary
// whole-object absolute line heights in DOCUMENT units. A unit switch must
// therefore convert 60 document px with the DOCUMENT font size (40 for a 20 px
// local font at a 2x transform), not with the local 20 px font. The two cases
// below drive the real Gtk::DropDown unit control and compare every rendered
// document-space baseline origin (initialPoint() * i2doc_affine()) before and
// after the switch. Wrapped values are compared as measured vectors; no wrapped
// or mixed gap formula is guessed.

// The unit switch must be geometry-neutral: every rendered document baseline
// origin must match the captured vector, not merely the first gap.
void expectSameBaselinePositions(std::vector<double> const &expected,
                                 std::vector<double> const &actual,
                                 char const *where)
{
    ASSERT_EQ(actual.size(), expected.size()) << where;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(actual[i], expected[i], 1e-6)
            << where << " baseline " << i << " expected=" << expected[i]
            << " actual=" << actual[i];
    }
}

// UNIT1: ordinary two-line <text>, 20 px local font, root line-height 30 px
// local, 2x item transform. The query and the panel display the document-space
// 60 px. Changing the real unit dropdown to Lines must show 1.5 (60 document px
// at a 40 document font) and leave every rendered baseline origin unchanged;
// changing back to Px must show 60 with the same geometry. The two switches must
// undo/redo exactly and save/reopen must preserve the rendered geometry.
// Current source converts 60 with the local 20 px font, so it shows 3 and
// renders a 120 px gap.
TEST_F(TextMultiStyleTest, PanelUnitSwitchPreservesScaledOrdinaryLineHeight)
{
    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    lines->setAttribute("style", "font-family:sans-serif;font-size:20px;line-height:30px");
    for (auto const *id : {"line-one", "line-two"}) {
        auto *span = document->getObjectById(id);
        ASSERT_TRUE(span);
        span->setAttribute("y", nullptr);
    }
    lines->setAttribute("transform", "scale(2)");
    checkpoint();

    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    ASSERT_NEAR(lines->i2doc_affine().descrim(), 2.0, 1e-6);
    auto const initial_xml = xml();
    auto const initial = measureBaselineGap(*lines);
    ASSERT_EQ(initial.line_y.size(), 2u);
    ASSERT_NEAR(initial.gap, 60.0, 0.01)
        << "authored 30px local strut under a 2x transform must start at 60 document px";
    writeEvidenceSvg("UNIT1-ordinary-before", initial_xml);

    std::vector<double> const before_y = initial.line_y;

    UI::Tools::TextTool *tool = nullptr;
    placeCaretInFirstHardLine(desktop, lines, tool);

    Gtk::Window window;
    auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
    panel->setDesktop(desktop);
    window.set_child(*panel);
    window.present();
    drainMainContext();
    expandPanelSection(*panel, "Paragraph");
    drainMainContext();

    auto *unit = panelLineHeightUnitDropdown(*panel);
    ASSERT_TRUE(unit);
    auto *spin = panelLineHeightSpin(*panel);
    ASSERT_TRUE(spin);

    // Query and display start in document px at 60, not the local 30.
    {
        auto const query = desktop->textStyleController().queryParagraph();
        ASSERT_TRUE(query.line_height.valid);
        EXPECT_FALSE(query.line_height.mixed);
        EXPECT_EQ(query.line_height.value.unit, UI::TextLineHeightUnit::Px);
        EXPECT_NEAR(query.line_height.value.value, 60.0, 1e-6);
    }
    ASSERT_EQ(unit->get_selected(), static_cast<guint>(UI::TextLineHeightUnit::Px));
    EXPECT_NEAR(spin->get_value(), 60.0, 1e-6);

    // Explicitly select Lines: 60 document px / 40 document font = 1.5.
    unit->set_selected(static_cast<guint>(UI::TextLineHeightUnit::Lines));
    drainMainContext();
    document->ensureUpToDate();
    ASSERT_EQ(unit->get_selected(), static_cast<guint>(UI::TextLineHeightUnit::Lines));
    EXPECT_NEAR(spin->get_value(), 1.5, 1e-6)
        << "60 document px at a 40 document font must read 1.5 lines";
    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    expectSameBaselinePositions(before_y, measureBaselineGap(*lines).line_y,
                                "ordinary after switching to Lines");
    auto const lines_xml = xml();
    writeEvidenceSvg("UNIT1-ordinary-lines", lines_xml);

    // Explicitly select Px again: 60 and the same geometry.
    unit->set_selected(static_cast<guint>(UI::TextLineHeightUnit::Px));
    drainMainContext();
    document->ensureUpToDate();
    ASSERT_EQ(unit->get_selected(), static_cast<guint>(UI::TextLineHeightUnit::Px));
    EXPECT_NEAR(spin->get_value(), 60.0, 1e-6);
    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    expectSameBaselinePositions(before_y, measureBaselineGap(*lines).line_y,
                                "ordinary after switching back to Px");
    auto const px_xml = xml();
    writeEvidenceSvg("UNIT1-ordinary-px", px_xml);
    window.unset_child();

    // The two unit switches are two exact Undo/Redo transactions.
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), lines_xml) << "undoing the px switch must restore the Lines state";
    EXPECT_NEAR(measureLineTextGap(document), initial.gap, 1e-6);
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml) << "undoing the lines switch must restore the authored state";
    EXPECT_NEAR(measureLineTextGap(document), initial.gap, 1e-6);
    EXPECT_FALSE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), lines_xml);
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), px_xml);

    // Save/reopen preserves the final rendered geometry.
    auto const saved = sp_repr_save_buf(document->getReprDoc()).raw();
    std::unique_ptr<SPDocument> reopened{SPDocument::createNewDocFromMem(saved)};
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    EXPECT_NEAR(measureLineTextGap(reopened.get()), initial.gap, 1e-6);
}

// UNIT2: scaled wrapped text (inline-size) with authored role=paragraph tspans,
// 20 px local font, 30 px local line-height, 2x transform. The query must report
// 60 document px, and the real unit dropdown must round-trip Px -> Lines -> Px
// while every rendered document baseline origin, the authored paragraph roles,
// the text content and the wrapping frame survive. No wrapped/mixed gap formula
// is asserted: the measured vectors are compared directly.
TEST_F(TextMultiStyleTest, PanelUnitRoundTripPreservesScaledWrappedParagraphs)
{
    auto *wrapped = cast<SPText>(document->getObjectById("wrapped-paragraphs"));
    ASSERT_TRUE(wrapped);
    wrapped->setAttribute("transform", "scale(2)");
    checkpoint();

    wrapped = cast<SPText>(document->getObjectById("wrapped-paragraphs"));
    ASSERT_TRUE(wrapped);
    ASSERT_TRUE(wrapped->has_inline_size());
    ASSERT_NEAR(wrapped->i2doc_affine().descrim(), 2.0, 1e-6);

    auto const initial_xml = xml();
    auto const initial = measureBaselineGap(*wrapped);
    ASSERT_GE(initial.line_y.size(), 3u)
        << "wrapped authored paragraphs must render more than two baselines";
    writeEvidenceSvg("UNIT2-wrapped-before", initial_xml);

    // The authored strings are the contract. Capture them through descendant
    // text nodes and assert the exact fixture values and non-emptiness, so an
    // empty==empty comparison cannot hide lost text.
    std::string const expected_para_one = "First authored paragraph wraps across several visual lines.";
    std::string const expected_para_two = "Second authored paragraph also wraps across lines.";
    EXPECT_EQ(descendantText(document->getObjectById("wrapped-para-one")), expected_para_one);
    EXPECT_EQ(descendantText(document->getObjectById("wrapped-para-two")), expected_para_two);
    double const authored_inline_size = wrapped->style->inline_size.computed;

    auto expect_authored_structure = [&](SPDocument *doc, char const *where) {
        auto *text = cast<SPText>(doc->getObjectById("wrapped-paragraphs"));
        EXPECT_TRUE(text) << where;
        if (!text) return;
        EXPECT_TRUE(text->has_inline_size()) << where;
        EXPECT_NEAR(text->i2doc_affine().descrim(), 2.0, 1e-6) << where;
        if (text->style) {
            EXPECT_NEAR(text->style->inline_size.computed, authored_inline_size, 1e-6) << where;
        }
        for (auto const *id : {"wrapped-para-one", "wrapped-para-two"}) {
            auto *span = cast<SPTSpan>(doc->getObjectById(id));
            EXPECT_TRUE(span) << where;
            if (span) EXPECT_EQ(span->role, SP_TSPAN_ROLE_PARAGRAPH) << where;
        }
        std::string const one = descendantText(doc->getObjectById("wrapped-para-one"));
        std::string const two = descendantText(doc->getObjectById("wrapped-para-two"));
        EXPECT_FALSE(one.empty()) << where;
        EXPECT_FALSE(two.empty()) << where;
        EXPECT_EQ(one, expected_para_one) << where;
        EXPECT_EQ(two, expected_para_two) << where;
    };
    expect_authored_structure(document, "authored input");

    std::vector<double> const before_y = initial.line_y;

    UI::Tools::TextTool *tool = nullptr;
    placeCaretInFirstHardLine(desktop, wrapped, tool);

    Gtk::Window window;
    auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
    panel->setDesktop(desktop);
    window.set_child(*panel);
    window.present();
    drainMainContext();
    expandPanelSection(*panel, "Paragraph");
    drainMainContext();

    auto *unit = panelLineHeightUnitDropdown(*panel);
    ASSERT_TRUE(unit);
    auto *spin = panelLineHeightSpin(*panel);
    ASSERT_TRUE(spin);

    // Query/display must report the document-space 60, not the local 30.
    {
        auto const query = desktop->textStyleController().queryParagraph();
        ASSERT_TRUE(query.line_height.valid);
        EXPECT_FALSE(query.line_height.mixed);
        EXPECT_EQ(query.line_height.value.unit, UI::TextLineHeightUnit::Px);
        EXPECT_NEAR(query.line_height.value.value, 60.0, 1e-6)
            << "wrapped paragraph line-height must be reported in document px";
    }
    ASSERT_EQ(unit->get_selected(), static_cast<guint>(UI::TextLineHeightUnit::Px));
    EXPECT_NEAR(spin->get_value(), 60.0, 1e-6);
    writeEvidenceSvg("UNIT2-wrapped-query", xml());
    expect_authored_structure(document, "after panel query");

    // Round trip Px -> Lines -> Px. Compare measured vectors, never a formula.
    unit->set_selected(static_cast<guint>(UI::TextLineHeightUnit::Lines));
    drainMainContext();
    document->ensureUpToDate();
    ASSERT_EQ(unit->get_selected(), static_cast<guint>(UI::TextLineHeightUnit::Lines));
    EXPECT_NEAR(spin->get_value(), 1.5, 1e-6);
    wrapped = cast<SPText>(document->getObjectById("wrapped-paragraphs"));
    ASSERT_TRUE(wrapped);
    expectSameBaselinePositions(before_y, measureBaselineGap(*wrapped).line_y,
                                "wrapped after switching to Lines");
    expect_authored_structure(document, "after switching to Lines");
    writeEvidenceSvg("UNIT2-wrapped-lines", xml());

    unit->set_selected(static_cast<guint>(UI::TextLineHeightUnit::Px));
    drainMainContext();
    document->ensureUpToDate();
    ASSERT_EQ(unit->get_selected(), static_cast<guint>(UI::TextLineHeightUnit::Px));
    EXPECT_NEAR(spin->get_value(), 60.0, 1e-6);
    wrapped = cast<SPText>(document->getObjectById("wrapped-paragraphs"));
    ASSERT_TRUE(wrapped);
    expectSameBaselinePositions(before_y, measureBaselineGap(*wrapped).line_y,
                                "wrapped after switching back to Px");
    expect_authored_structure(document, "after switching back to Px");
    writeEvidenceSvg("UNIT2-wrapped-px", xml());
    window.unset_child();

    // Save/reopen preserves the round-tripped geometry and authored structure:
    // roles, paragraph text and the wrapping frame, not only the baseline vector.
    auto const saved = sp_repr_save_buf(document->getReprDoc()).raw();
    std::unique_ptr<SPDocument> reopened{SPDocument::createNewDocFromMem(saved)};
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto *reopened_wrapped = cast<SPText>(reopened->getObjectById("wrapped-paragraphs"));
    ASSERT_TRUE(reopened_wrapped);
    expectSameBaselinePositions(before_y, measureBaselineGap(*reopened_wrapped).line_y,
                                "wrapped save/reopen");
    expect_authored_structure(reopened.get(), "wrapped save/reopen");
}

// (EDGE1) Ordinary two-line <text>, root 50px local strut at font-size 20px, the
// LAST line explicitly authored at 20px, and a 2x item transform, so the root
// strut floors the authored child to 50 local = 100 document px before the
// operation. A REVERSED one-character partial selection inside the last line
// requests 40 document px (local 20 at 2x, the authored value) through the real
// Px panel spin. The rendered baseline vector must match the native toolbar
// inner request on the same restored fixture, the selected run must carry local
// 20px rather than a double-scaled 10px, and the reversed raw selection must
// survive. Both entry points must force a real 60->40 change (the spinner
// already displays 40, so a bare 40 emits nothing) that coalesces into exactly
// one Undo. From the ORIGINAL fixture a direct commitParagraph(40) is a real
// change that reduces the root floor and is undone exactly. Redo preservation is
// then checked meaningfully: a separate non-continuous 60 state is undone back
// to 40 leaving Redo=60, repeating 40 is a no-op that leaves the XML unchanged,
// and redo restores 60 exactly. No production code or tolerance is changed.
TEST_F(TextMultiStyleTest, PanelScaledReversedInnerLineHeightKeepsLocalScaleAndSelection)
{
    ASSERT_TRUE(configureLineHeightFixture(
        document, "font-family:sans-serif;font-size:20px;line-height:50px", true));
    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    lines->setAttribute("transform", "scale(2)");
    checkpoint();

    lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    ASSERT_NEAR(lines->i2doc_affine().descrim(), 2.0, 1e-6);
    auto const initial_xml = xml();
    auto const initial = measureBaselineGap(*lines);
    ASSERT_EQ(initial.line_y.size(), 2u);
    EXPECT_NEAR(initial.gap, 100.0, 0.5)
        << "root 50px local strut floors the authored 20px child to 50 local = 100 document px";

    // Native oracle first, on the restored fixture with the reversed range. The
    // toolbar spinner already displays the authored 40 document px, so a bare
    // set_value(40) emits nothing; force a real 60 then 40 exactly as the panel
    // path does. Both continuous edits coalesce into one Undo.
    std::vector<double> native_y;
    std::string native_xml;
    ASSERT_TRUE(driveNativeToolbarInnerLineHeight(
        document, desktop, "line-text", /*reversed=*/true, {60.0, 40.0}, native_y, native_xml));
    ASSERT_EQ(native_y.size(), 2u);
    // Mixed per-line heights: the requested last-line 40 document px (local 20px)
    // combines with the 50px local root floor, so the rendered gap is the
    // half-sum rather than a bare 40. The unit contract itself is asserted by the
    // controller query below (40 document px == local 20px, not double-scaled 10).
    EXPECT_LT(native_y[1] - native_y[0], initial.gap - 1.0)
        << "the native inner request must reduce the 100 document px root floor";
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml) << "one native inner request must be one Undo";
    EXPECT_FALSE(DocumentUndo::undo(document));

    // The real panel control already displays the last line's 40 document px, so
    // a bare set_value(40.0) would not emit value-changed. Force a real change to
    // 60 first, then enter 40. Both are continuous edits under the same undo key
    // and coalesce into one Undo.
    std::vector<double> panel_y;
    std::string panel_xml;
    RawSelectionRange raw_before;
    {
        auto *text = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(text);
        UI::Tools::TextTool *tool = nullptr;
        ASSERT_TRUE(placeReversedCharacterSelectionInLastHardLine(desktop, text, tool));
        raw_before = rawSelectionRange(desktop);
        ASSERT_TRUE(raw_before.valid);
        ASSERT_GT(raw_before.start, raw_before.end);
        ASSERT_TRUE(driveRealPanelLineHeight(document, desktop, {60.0, 40.0}, panel_y, panel_xml));
    }
    ASSERT_EQ(panel_y.size(), 2u);
    EXPECT_NEAR(panel_y[1] - panel_y[0], native_y[1] - native_y[0], 0.1)
        << "panel and native inner requests must render the same baseline gap";
    EXPECT_LT(panel_y[1] - panel_y[0], initial.gap - 1.0)
        << "the root strut must no longer keep the 100 document px gap";
    std::cout << "[EDGE1] authored gap=" << initial.gap
              << " native gap=" << native_y[1] - native_y[0]
              << " panel gap=" << panel_y[1] - panel_y[0] << std::endl;

    // 40 document px under a 2x item transform is local 20px. A double scale
    // would store local 10px and report 20 document px here.
    {
        auto const query = desktop->textStyleController().queryParagraph();
        ASSERT_TRUE(query.line_height.valid);
        EXPECT_FALSE(query.line_height.mixed);
        EXPECT_EQ(query.line_height.value.unit, UI::TextLineHeightUnit::Px);
        EXPECT_NEAR(query.line_height.value.value, 40.0, 1e-6)
            << "requested 40 document px = local 20px; local 10px would report 20";
    }
    // Raw logical indices and reversed direction survive the synchronous rebuild.
    {
        auto const raw = rawSelectionRange(desktop);
        ASSERT_TRUE(raw.valid);
        EXPECT_EQ(raw.start, raw_before.start) << "raw selection logical start must survive";
        EXPECT_EQ(raw.end, raw_before.end) << "raw selection logical end must survive";
        EXPECT_GT(raw.start, raw.end) << "reversed selection direction must survive";
        EXPECT_EQ(raw.start - raw.end, 1) << "exactly one character remains selected";
    }

    // The final 40 document px state owns exactly one Undo: undo restores the
    // authored fixture and redo restores the final 40 state.
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml) << "one panel inner request must be one Undo";
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), panel_xml) << "one redo must restore the final 40 document px state";

    UI::TextParagraphPatch commit40;
    commit40.line_height = UI::TextLineHeightValue{40.0, UI::TextLineHeightUnit::Px};
    UI::TextParagraphPatch commit60;
    commit60.line_height = UI::TextLineHeightValue{60.0, UI::TextLineHeightUnit::Px};

    // Undo rebuilds the layout, so reacquire the reversed raw range before every
    // selection-scoped commit below.
    auto reselect_reversed = [&]() {
        auto *text = cast<SPText>(document->getObjectById("line-text"));
        EXPECT_TRUE(text);
        if (!text) return;
        UI::Tools::TextTool *tool = nullptr;
        EXPECT_TRUE(placeReversedCharacterSelectionInLastHardLine(desktop, text, tool));
        auto const raw = rawSelectionRange(desktop);
        EXPECT_TRUE(raw.valid);
        EXPECT_GT(raw.start, raw.end);
    };

    // From the ORIGINAL authored fixture (root 50 local floor of 100 document px
    // still active), a direct 40 document px commit is a REAL change, not the
    // previously skipped no-op: it must reduce the root floor and one Undo must
    // restore the original.
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);
    reselect_reversed();
    ASSERT_TRUE(desktop->textStyleController().commitParagraph(
        commit40, "text-panel:line-height", RC_("Undo", "Set paragraph line height")))
        << "from the original 100 document px root floor a direct 40 request must change the document";
    document->ensureUpToDate();
    {
        auto *text = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(text);
        auto const direct = measureBaselineGap(*text);
        ASSERT_EQ(direct.line_y.size(), 2u);
        EXPECT_LT(direct.gap, initial.gap - 1.0)
            << "the direct 40 request must reduce the 100 document px root floor";
        EXPECT_NEAR(direct.gap, panel_y[1] - panel_y[0], 0.1)
            << "the direct and panel requests must render the same reduced gap";
    }
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml) << "undo of the direct 40 request must restore the original";

    // Meaningful Redo preservation: build a separate, non-continuous 60 state
    // from the current 40 state, undo back to 40 (leaving Redo=60), then repeat
    // 40. The repeat must be a no-op that neither changes the XML nor consumes
    // the Redo entry, and redo must restore the 60 state exactly.
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    reselect_reversed();
    ASSERT_TRUE(desktop->textStyleController().commitParagraph(
        commit60, "text-panel:line-height", RC_("Undo", "Set paragraph line height")))
        << "the separate 60 request from the 40 state must be a real change";
    document->ensureUpToDate();
    auto const state60_xml = xml();
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    auto const state40_xml = xml();
    EXPECT_NE(state40_xml, state60_xml) << "undo must leave the 40 document px state";
    {
        auto *text = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(text);
        auto const back40 = measureBaselineGap(*text);
        ASSERT_EQ(back40.line_y.size(), 2u);
        EXPECT_LT(back40.gap, initial.gap - 1.0)
            << "undo of 60 must leave the reduced 40 document px state";
    }
    reselect_reversed();
    EXPECT_FALSE(desktop->textStyleController().commitParagraph(
        commit40, "text-panel:line-height", RC_("Undo", "Set paragraph line height")))
        << "repeating the already-active 40 must be a no-op";
    EXPECT_EQ(xml(), state40_xml) << "the repeated 40 must not change the document";
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), state60_xml) << "the no-op must preserve the Redo entry (60 exact)";
}

// (EDGE2) Two ordinary imported-multiline texts under a 50px root strut at a
// 20px font that render identically before any operation: one whose first line
// is a direct root text node (SPString) holding "First line\n", and one whose
// first line is the same "First line\n" wrapped in a plain tspan. Both second
// lines are plain tspans; the hard break comes only from the preserved newline
// under white-space:pre (never role=line semantics). A real panel inner request
// for 20px on the LAST line must leave the two fixtures equivalent (rendered
// baseline vector and authored content/font/fill), own exactly one Undo, and
// survive save/reopen. Source hypothesis: root normalization skips the direct
// SPString child and then sets the root to 0, so the unselected first line loses
// its 50px height; the wrapped fixture is the equivalence oracle. This is a
// measured comparison, never a guessed number.
TEST_F(TextMultiStyleTest, PanelInnerLineHeightMatchesWrappedEquivalentForDirectTextNodeRoot)
{
    ASSERT_TRUE(configureDirectRootAndWrappedEquivalentFixture(document));
    checkpoint();

    char const *direct_id = "direct-root-line";
    char const *wrapped_id = "wrapped-line";

    auto baseline_vector = [&](char const *id) {
        auto *text = cast<SPText>(document->getObjectById(id));
        return text ? measureBaselineGap(*text).line_y : std::vector<double>{};
    };

    auto const direct_before = baseline_vector(direct_id);
    auto const wrapped_before = baseline_vector(wrapped_id);
    ASSERT_EQ(direct_before.size(), 2u);
    ASSERT_EQ(wrapped_before.size(), 2u);
    expectSameBaselinePositions(direct_before, wrapped_before,
                                "equivalent fixtures must start identical");
    EXPECT_EQ(descendantText(document->getObjectById(direct_id)), "First line\nSecond line");
    EXPECT_EQ(descendantText(document->getObjectById(wrapped_id)), "First line\nSecond line");

    auto run_panel_inner = [&](char const *id) {
        std::vector<double> after_y;
        std::string after_xml;
        auto *text = cast<SPText>(document->getObjectById(id));
        EXPECT_TRUE(text);
        if (!text) return std::pair<std::vector<double>, std::string>{};
        UI::Tools::TextTool *tool = nullptr;
        EXPECT_TRUE(placeCharacterSelectionInLastHardLine(desktop, text, tool));
        EXPECT_TRUE(driveRealPanelLineHeight(document, desktop, {20.0}, after_y, after_xml));
        return std::pair<std::vector<double>, std::string>{after_y, after_xml};
    };

    auto expect_authored_preserved = [&](char const *id, char const *second_id, char const *where) {
        auto *text = cast<SPText>(document->getObjectById(id));
        if (!text || !text->style) {
            EXPECT_TRUE(text && text->style) << where;
            return;
        }
        EXPECT_NEAR(text->style->font_size.computed, 20.0, 0.01) << where;
        EXPECT_TRUE(text->style->fill.isColor()) << where;
        if (text->style->fill.isColor()) {
            EXPECT_EQ(text->style->fill.getColor().toRGBA(), 0x123456ff) << where;
        }
        EXPECT_EQ(descendantText(document->getObjectById(id)), "First line\nSecond line") << where;
        if (auto *second = document->getObjectById(second_id)) {
            expectBoldFillPreserved(second, where);
        }
    };

    auto const direct_after = run_panel_inner(direct_id);
    ASSERT_EQ(direct_after.first.size(), 2u);
    expect_authored_preserved(direct_id, "direct-root-second", "direct-root after panel");
    writeEvidenceSvg("EDGE2-direct-after", xml());

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_FALSE(DocumentUndo::undo(document));
    expectSameBaselinePositions(direct_before, baseline_vector(direct_id),
                                "one Undo must restore the direct-root fixture");

    auto const wrapped_after = run_panel_inner(wrapped_id);
    ASSERT_EQ(wrapped_after.first.size(), 2u);
    expect_authored_preserved(wrapped_id, "wrapped-second", "wrapped after panel");
    writeEvidenceSvg("EDGE2-wrapped-after", xml());

    // Equivalence is the oracle: the direct-root text must end in the same
    // rendered state as the identical text whose first line is a plain tspan.
    expectSameBaselinePositions(wrapped_after.first, direct_after.first,
                                "direct-root must match the wrapped equivalent");
    EXPECT_NEAR(direct_after.first[1] - direct_after.first[0],
                wrapped_after.first[1] - wrapped_after.first[0], 1e-6);
    std::cout << "[EDGE2] direct before gap=" << direct_before[1] - direct_before[0]
              << " direct after gap=" << direct_after.first[1] - direct_after.first[0]
              << " wrapped after gap=" << wrapped_after.first[1] - wrapped_after.first[0]
              << std::endl;

    // Save/reopen preserves the direct-root result geometry and content. The
    // direct-after XML was captured above, before the wrapping run.
    std::unique_ptr<SPDocument> reopened{
        SPDocument::createNewDocFromMem(direct_after.second)};
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto *reopened_direct = cast<SPText>(reopened->getObjectById(direct_id));
    ASSERT_TRUE(reopened_direct);
    expectSameBaselinePositions(direct_after.first, measureBaselineGap(*reopened_direct).line_y,
                                "direct-root save/reopen");
    EXPECT_EQ(descendantText(reopened->getObjectById(direct_id)), "First line\nSecond line");

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_FALSE(DocumentUndo::undo(document));
    expectSameBaselinePositions(wrapped_before, baseline_vector(wrapped_id),
                                "one Undo must restore the wrapped fixture");
}

// --- Panel percent spacing placed on a scaled ordinary two-line text --------
//
// The native toolbar's px spacing is document-space, while panel percent
// spacing is computed from the run's LOCAL space advance. The whole-object
// per-run route writes the local px directly (correct); the partial per-run
// route forwards the same local px to sp_te_apply_style(), which rescales by
// 1/descrim() because it takes document-space input (text-editing.cpp:2405-2410).
// At scale(2) a 50% partial request therefore stores half the correct local
// advance and the query reports 25%. Whole-object is the reference: the partial
// route must render the SAME physical spacing on the selected last line, and a
// native toolbar request for (observed whole local spacing * actual scale) must
// agree. The native px oracle is measured from the whole result, never copied
// from the font algorithm.

// Document-space X of every character in [first,last) of a text layout.
// characterBoundingBox() is item-local, so it is transformed by i2doc_affine.
std::vector<double> documentGlyphX(SPText &text, unsigned first, unsigned last)
{
    std::vector<double> xs;
    auto const to_document = text.i2doc_affine();
    auto const &layout = text.layout;
    auto it = layout.begin();
    for (unsigned i = 0; i < first && it != layout.end(); ++i) it.nextCharacter();
    for (unsigned i = first; i < last && it != layout.end(); ++i, it.nextCharacter()) {
        auto const box = layout.characterBoundingBox(it);
        xs.push_back((box.min() * to_document)[Geom::X]);
    }
    return xs;
}

// [first,last) character indices of the LAST rendered hard line.
bool lastHardLineRange(SPText &text, unsigned &first, unsigned &last)
{
    auto const &layout = text.layout;
    unsigned line = 0;
    bool any = false;
    for (auto it = layout.begin(); it != layout.end(); it.nextCharacter()) {
        line = std::max(line, layout.lineIndex(it));
        any = true;
    }
    if (!any) return false;
    unsigned index = 0;
    bool seen = false;
    for (auto it = layout.begin(); it != layout.end(); it.nextCharacter(), ++index) {
        if (layout.lineIndex(it) == line) {
            if (!seen) {
                first = index;
                seen = true;
            }
            last = index + 1;
        }
    }
    return seen;
}

// Select the WHOLE last hard line (forward or reversed) so a word-spacing
// request on the last line includes the embedded space character.
bool placeLastHardLineSelection(SPDesktop *desktop, SPText *text,
                                UI::Tools::TextTool *&tool, bool reversed)
{
    desktop->getSelection()->set(text);
    desktop->setTool("/tools/text");
    tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    if (!tool || tool->textItem() != text) return false;

    unsigned first = 0;
    unsigned last = 0;
    if (!lastHardLineRange(*text, first, last)) return false;

    auto begin = text->layout.begin();
    for (unsigned i = 0; i < first; ++i) begin.nextCharacter();
    auto end = begin;
    for (unsigned i = first; i < last; ++i) end.nextCharacter();

    if (reversed) {
        tool->text_sel_start = end;
        tool->text_sel_end = begin;
    } else {
        tool->text_sel_start = begin;
        tool->text_sel_end = end;
    }
    desktop->emit_text_cursor_moved(tool);
    drainMainContext();
    return true;
}

// Ordinary two-line text at font-size 20 under scale(2), with an explicitly
// authored unselected first line whose content/font/fill must survive a
// last-line-only request.
bool configureSpacingFixture(SPDocument *document)
{
    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    if (!lines) return false;
    lines->setAttribute("style", "font-family:sans-serif;font-size:20px;fill:#123456");
    auto *one = document->getObjectById("line-one");
    auto *two = document->getObjectById("line-two");
    if (!one || !two) return false;
    one->setAttribute("style", "font-weight:bold;fill:#123456");
    two->setAttribute("style", nullptr);
    lines->setAttribute("transform", "scale(2)");
    return true;
}

void expectUnselectedFirstLinePreserved(SPDocument *document, char const *where)
{
    auto *one = cast<SPTSpan>(document->getObjectById("line-one"));
    if (!one || !one->style) {
        EXPECT_TRUE(one && one->style) << where;
        return;
    }
    EXPECT_EQ(one->style->font_weight.computed, 700) << where;
    EXPECT_NEAR(one->style->font_size.computed, 20.0, 0.01) << where;
    if (!one->style->fill.isColor()) {
        EXPECT_TRUE(one->style->fill.isColor()) << where;
        return;
    }
    EXPECT_EQ(one->style->fill.getColor().toRGBA(), 0x123456ff) << where;
    EXPECT_EQ(descendantText(one), "First line") << where;
}

Gtk::SpinButton *panelSpacingSpin(Gtk::Widget &panel, char const *tooltip)
{
    return dynamic_cast<Gtk::SpinButton *>(findWidgetByTooltip(panel, tooltip));
}

UI::Widget::SpinButton *toolbarSpacingSpin(Gtk::Widget &toolbar, char const *box_tooltip)
{
    auto *box = findWidgetByTooltip(toolbar, box_tooltip);
    return box ? findWidget<UI::Widget::SpinButton>(*box) : nullptr;
}

// Panel character-spacing 50% on a scale(2) ordinary two-line text: whole-object
// vs last-line-only vs the equivalent native toolbar document px.
TEST_F(TextMultiStyleTest, ScaledWholeAndLastLinePanelCharacterSpacingMatchNative)
{
    ASSERT_TRUE(configureSpacingFixture(document));
    checkpoint();

    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    ASSERT_NEAR(lines->i2doc_affine().descrim(), 2.0, 1e-6);
    auto const initial_xml = xml();
    unsigned first = 0;
    unsigned last = 0;
    ASSERT_TRUE(lastHardLineRange(*lines, first, last));
    auto const initial_x = documentGlyphX(*lines, first, last);
    ASSERT_GE(initial_x.size(), 2u);
    writeEvidenceSvg("SP1-char-before", initial_xml);

    auto last_line_x = [&]() {
        auto *text = cast<SPText>(document->getObjectById("line-text"));
        unsigned f = 0;
        unsigned l = 0;
        if (!text || !lastHardLineRange(*text, f, l)) return std::vector<double>{};
        return documentGlyphX(*text, f, l);
    };

    auto const spacing_tooltip = "Character spacing relative to the effective space advance";

    // Whole-object panel route (reference).
    std::vector<double> whole_x;
    std::string whole_xml;
    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        UI::Tools::TextTool *tool = nullptr;
        placeCaretInFirstHardLine(desktop, lines, tool);

        Gtk::Window window;
        auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
        panel->setDesktop(desktop);
        window.set_child(*panel);
        window.present();
        drainMainContext();

        auto *spin = panelSpacingSpin(*panel, spacing_tooltip);
        ASSERT_TRUE(spin);
        spin->set_value(50.0);
        drainMainContext();
        document->ensureUpToDate();

        whole_x = last_line_x();
        whole_xml = xml();
        window.unset_child();
    }
    ASSERT_FALSE(whole_x.empty());
    ASSERT_NE(whole_x, initial_x) << "the whole-object 50% request must change the last-line glyphs";
    writeEvidenceSvg("SP1-char-whole", whole_xml);

    auto const whole_query = desktop->textStyleController().query();
    ASSERT_TRUE(whole_query.character_spacing_percent.valid);
    EXPECT_FALSE(whole_query.character_spacing_percent.mixed);
    EXPECT_NEAR(whole_query.character_spacing_percent.value, 50.0, 1e-4)
        << "whole-object percent must query 50";

    auto *line_two = document->getObjectById("line-two");
    ASSERT_TRUE(line_two && line_two->style);
    double const whole_local_spacing = line_two->style->letter_spacing.normal
        ? 0.0 : line_two->style->letter_spacing.computed;
    double const scale = lines->i2doc_affine().descrim();
    double const native_px = whole_local_spacing * scale;
    ASSERT_GT(whole_local_spacing, 0.0);
    ASSERT_NEAR(native_px, whole_local_spacing * 2.0, 1e-9)
        << "observed whole local spacing times the actual scale is the document px oracle";

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);
    EXPECT_FALSE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), whole_xml);
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);

    // Native toolbar document px route on the restored fixture.
    std::vector<double> native_x;
    std::string native_xml;
    double native_query_value = -1.0;
    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        desktop->getSelection()->set(lines);
        Inkscape::UI::Toolbar::TextToolbar toolbar;
        toolbar.setDesktop(desktop);
        UI::Tools::TextTool *tool = nullptr;
        if (!placeLastHardLineSelection(desktop, lines, tool, /*reversed=*/false)) {
            toolbar.setDesktop(nullptr);
            FAIL() << "could not place last-line selection for native route";
            return;
        }
        auto *spin = toolbarSpacingSpin(toolbar, "Spacing between letters (px)");
        if (!spin) {
            toolbar.setDesktop(nullptr);
            FAIL() << "toolbar letter-spacing spin not found";
            return;
        }
        // The spinner already displays the authored value, so force a real
        // change: an intermediate then the oracle document px. Both coalesce.
        spin->set_value(native_px + 1.0);
        drainMainContext();
        document->ensureUpToDate();
        spin->set_value(native_px);
        drainMainContext();
        document->ensureUpToDate();
        toolbar.setDesktop(nullptr);

        native_x = last_line_x();
        native_xml = xml();
        expectUnselectedFirstLinePreserved(document, "character-space native route");

        auto const native_query = desktop->textStyleController().query();
        ASSERT_TRUE(native_query.character_spacing_percent.valid);
        native_query_value = native_query.character_spacing_percent.value;
        EXPECT_NEAR(native_query.character_spacing_percent.value, 50.0, 1e-4)
            << "the native document px equivalent to whole 50% must query 50";
    }
    ASSERT_FALSE(native_x.empty());
    writeEvidenceSvg("SP1-char-native", native_xml);
    ASSERT_EQ(native_x.size(), whole_x.size());
    for (size_t i = 0; i < native_x.size(); ++i) {
        EXPECT_NEAR(native_x[i], whole_x[i], 0.5)
            << "last-line glyph " << i << " whole=" << whole_x[i] << " native=" << native_x[i];
    }
    {
        double max_native = 0.0;
        for (size_t i = 0; i < whole_x.size() && i < native_x.size(); ++i) {
            double d = native_x[i] - whole_x[i];
            if (d < 0) d = -d;
            if (d > max_native) max_native = d;
        }
        std::cout << "[SP1] whole_local=" << whole_local_spacing << " scale=" << scale
                  << " native_px=" << native_px
                  << " whole_q=" << whole_query.character_spacing_percent.value
                  << " native_q=" << native_query_value
                  << " max|whole-native|=" << max_native << std::endl;
    }

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml) << "the two native edits must coalesce into one Undo";
    EXPECT_FALSE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), native_xml);
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);

    // Partial last-line panel route.
    std::vector<double> partial_x;
    std::string partial_xml;
    RawSelectionRange raw_before;
    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        UI::Tools::TextTool *tool = nullptr;
        ASSERT_TRUE(placeLastHardLineSelection(desktop, lines, tool, /*reversed=*/true));
        raw_before = rawSelectionRange(desktop);
        ASSERT_TRUE(raw_before.valid);
        ASSERT_GT(raw_before.start, raw_before.end);

        Gtk::Window window;
        auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
        panel->setDesktop(desktop);
        window.set_child(*panel);
        window.present();
        drainMainContext();

        auto *spin = panelSpacingSpin(*panel, spacing_tooltip);
        ASSERT_TRUE(spin);
        spin->set_value(50.0);
        drainMainContext();
        document->ensureUpToDate();

        partial_x = last_line_x();
        partial_xml = xml();
        window.unset_child();
    }
    ASSERT_FALSE(partial_x.empty());
    writeEvidenceSvg("SP1-char-partial", partial_xml);

    auto const partial_query = desktop->textStyleController().query();
    ASSERT_TRUE(partial_query.character_spacing_percent.valid);
    EXPECT_FALSE(partial_query.character_spacing_percent.mixed);
    EXPECT_NEAR(partial_query.character_spacing_percent.value, 50.0, 1e-4)
        << "partial percent must query 50; 25 proves the local px was divided again";

    ASSERT_EQ(partial_x.size(), whole_x.size());
    for (size_t i = 0; i < partial_x.size(); ++i) {
        EXPECT_NEAR(partial_x[i], whole_x[i], 0.5)
            << "last-line glyph " << i << " whole=" << whole_x[i] << " partial=" << partial_x[i];
    }
    {
        double max_partial = 0.0;
        for (size_t i = 0; i < whole_x.size() && i < partial_x.size(); ++i) {
            double d = partial_x[i] - whole_x[i];
            if (d < 0) d = -d;
            if (d > max_partial) max_partial = d;
        }
        std::cout << "[SP1] partial_q=" << partial_query.character_spacing_percent.value
                  << " partial0=" << partial_x.front() << " whole0=" << whole_x.front()
                  << " partial_last=" << partial_x.back() << " whole_last=" << whole_x.back()
                  << " max|whole-partial|=" << max_partial << std::endl;
    }
    expectUnselectedFirstLinePreserved(document, "character-space partial route");
    {
        auto const raw = rawSelectionRange(desktop);
        ASSERT_TRUE(raw.valid);
        EXPECT_EQ(raw.start, raw_before.start) << "raw logical start must survive";
        EXPECT_EQ(raw.end, raw_before.end) << "raw logical end must survive";
        EXPECT_GT(raw.start, raw.end) << "reversed selection direction must survive";
    }

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml) << "one partial request must be one Undo";
    EXPECT_FALSE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), partial_xml);

    // Repeated-request no-op and Redo preservation. Build a separate 75% state
    // with its own undo key, undo back to the 50% state (leaving Redo=75),
    // repeat 50%: it must be a real no-op that neither changes the XML nor
    // consumes Redo, and Redo must restore the 75% state exactly.
    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        UI::Tools::TextTool *tool = nullptr;
        ASSERT_TRUE(placeLastHardLineSelection(desktop, lines, tool, /*reversed=*/false));
        UI::TextStylePatch patch75;
        patch75.character_spacing_percent = 75.0;
        ASSERT_TRUE(desktop->textStyleController().commit(
            patch75, "test:character-spacing-75", RC_("Undo", "Set character spacing 75")));
        document->ensureUpToDate();
        auto const state75_xml = xml();

        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        auto const state50_xml = xml();
        EXPECT_NE(state50_xml, state75_xml);

        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        ASSERT_TRUE(placeLastHardLineSelection(desktop, lines, tool, /*reversed=*/false));
        auto const state50_query = desktop->textStyleController().query();
        ASSERT_TRUE(state50_query.character_spacing_percent.valid);
        EXPECT_NEAR(state50_query.character_spacing_percent.value, 50.0, 1e-4)
            << "the no-op prestate must genuinely be the equivalent 50% state";

        UI::TextStylePatch patch50;
        patch50.character_spacing_percent = 50.0;
        EXPECT_FALSE(desktop->textStyleController().commitContinuous(
            patch50, "text-panel:character-spacing", RC_("Undo", "Set character spacing")))
            << "repeating the already-active 50 must be a no-op";
        EXPECT_EQ(xml(), state50_xml) << "the no-op must not change the document";
        ASSERT_TRUE(DocumentUndo::redo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), state75_xml) << "the no-op must preserve the Redo entry";
    }
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);

    // Save/reopen preserves the reference whole and the partial last-line glyphs.
    {
        std::unique_ptr<SPDocument> reopened{SPDocument::createNewDocFromMem(whole_xml)};
        ASSERT_TRUE(reopened);
        reopened->ensureUpToDate();
        auto *rt = cast<SPText>(reopened->getObjectById("line-text"));
        ASSERT_TRUE(rt);
        unsigned rf = 0;
        unsigned rl = 0;
        ASSERT_TRUE(lastHardLineRange(*rt, rf, rl));
        auto const rx = documentGlyphX(*rt, rf, rl);
        ASSERT_EQ(rx.size(), whole_x.size());
        for (size_t i = 0; i < rx.size(); ++i) EXPECT_NEAR(rx[i], whole_x[i], 1e-6);
        expectUnselectedFirstLinePreserved(reopened.get(), "character-space whole save/reopen");
    }
    {
        std::unique_ptr<SPDocument> reopened{SPDocument::createNewDocFromMem(partial_xml)};
        ASSERT_TRUE(reopened);
        reopened->ensureUpToDate();
        auto *rt = cast<SPText>(reopened->getObjectById("line-text"));
        ASSERT_TRUE(rt);
        unsigned rf = 0;
        unsigned rl = 0;
        ASSERT_TRUE(lastHardLineRange(*rt, rf, rl));
        auto const rx = documentGlyphX(*rt, rf, rl);
        ASSERT_EQ(rx.size(), partial_x.size());
        for (size_t i = 0; i < rx.size(); ++i) EXPECT_NEAR(rx[i], partial_x[i], 1e-6);
    }
}

// Panel word-spacing 150% on the same scaled ordinary two-line text. Word
// spacing only affects the embedded space, so the WHOLE last hard line is
// selected (a one-character selection could miss the space). Same oracle: the
// whole-object route is the reference, the partial last-line route must match
// its physical spacing, and the native toolbar document px derived from the
// whole local spacing must agree.
TEST_F(TextMultiStyleTest, ScaledWholeAndLastLinePanelWordSpacingMatchNative)
{
    ASSERT_TRUE(configureSpacingFixture(document));
    checkpoint();

    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    ASSERT_NEAR(lines->i2doc_affine().descrim(), 2.0, 1e-6);
    auto const initial_xml = xml();
    unsigned first = 0;
    unsigned last = 0;
    ASSERT_TRUE(lastHardLineRange(*lines, first, last));
    auto const initial_x = documentGlyphX(*lines, first, last);
    ASSERT_GE(initial_x.size(), 2u);
    writeEvidenceSvg("SP2-word-before", initial_xml);

    auto last_line_x = [&]() {
        auto *text = cast<SPText>(document->getObjectById("line-text"));
        unsigned f = 0;
        unsigned l = 0;
        if (!text || !lastHardLineRange(*text, f, l)) return std::vector<double>{};
        return documentGlyphX(*text, f, l);
    };

    auto const spacing_tooltip = "Word spacing; 100% is normal";

    std::vector<double> whole_x;
    std::string whole_xml;
    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        UI::Tools::TextTool *tool = nullptr;
        placeCaretInFirstHardLine(desktop, lines, tool);

        Gtk::Window window;
        auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
        panel->setDesktop(desktop);
        window.set_child(*panel);
        window.present();
        drainMainContext();

        auto *spin = panelSpacingSpin(*panel, spacing_tooltip);
        ASSERT_TRUE(spin);
        spin->set_value(150.0);
        drainMainContext();
        document->ensureUpToDate();

        whole_x = last_line_x();
        whole_xml = xml();
        window.unset_child();
    }
    ASSERT_FALSE(whole_x.empty());
    ASSERT_NE(whole_x, initial_x) << "the whole-object 150% request must change the last-line glyphs";
    writeEvidenceSvg("SP2-word-whole", whole_xml);

    auto const whole_query = desktop->textStyleController().query();
    ASSERT_TRUE(whole_query.word_spacing_percent.valid);
    EXPECT_FALSE(whole_query.word_spacing_percent.mixed);
    EXPECT_NEAR(whole_query.word_spacing_percent.value, 150.0, 1e-4)
        << "whole-object word spacing must query 150";

    auto *line_two = document->getObjectById("line-two");
    ASSERT_TRUE(line_two && line_two->style);
    double const whole_local_spacing = line_two->style->word_spacing.normal
        ? 0.0 : line_two->style->word_spacing.computed;
    double const scale = lines->i2doc_affine().descrim();
    double const native_px = whole_local_spacing * scale;
    ASSERT_GT(whole_local_spacing, 0.0);
    ASSERT_NEAR(native_px, whole_local_spacing * 2.0, 1e-9)
        << "observed whole local word spacing times the actual scale is the oracle";

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);
    EXPECT_FALSE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), whole_xml);
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);

    std::vector<double> native_x;
    std::string native_xml;
    double native_query_value = -1.0;
    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        desktop->getSelection()->set(lines);
        Inkscape::UI::Toolbar::TextToolbar toolbar;
        toolbar.setDesktop(desktop);
        UI::Tools::TextTool *tool = nullptr;
        if (!placeLastHardLineSelection(desktop, lines, tool, /*reversed=*/false)) {
            toolbar.setDesktop(nullptr);
            FAIL() << "could not place last-line selection for native route";
            return;
        }
        auto *spin = toolbarSpacingSpin(toolbar, "Spacing between words (px)");
        if (!spin) {
            toolbar.setDesktop(nullptr);
            FAIL() << "toolbar word-spacing spin not found";
            return;
        }
        spin->set_value(native_px + 1.0);
        drainMainContext();
        document->ensureUpToDate();
        spin->set_value(native_px);
        drainMainContext();
        document->ensureUpToDate();
        toolbar.setDesktop(nullptr);

        native_x = last_line_x();
        native_xml = xml();
        expectUnselectedFirstLinePreserved(document, "word-space native route");

        auto const native_query = desktop->textStyleController().query();
        ASSERT_TRUE(native_query.word_spacing_percent.valid);
        native_query_value = native_query.word_spacing_percent.value;
        EXPECT_NEAR(native_query.word_spacing_percent.value, 150.0, 1e-4)
            << "the native document px equivalent to whole 150% must query 150";
    }
    ASSERT_FALSE(native_x.empty());
    writeEvidenceSvg("SP2-word-native", native_xml);
    ASSERT_EQ(native_x.size(), whole_x.size());
    for (size_t i = 0; i < native_x.size(); ++i) {
        EXPECT_NEAR(native_x[i], whole_x[i], 0.5)
            << "last-line glyph " << i << " whole=" << whole_x[i] << " native=" << native_x[i];
    }
    {
        double max_native = 0.0;
        for (size_t i = 0; i < whole_x.size() && i < native_x.size(); ++i) {
            double d = native_x[i] - whole_x[i];
            if (d < 0) d = -d;
            if (d > max_native) max_native = d;
        }
        std::cout << "[SP2] whole_local=" << whole_local_spacing << " scale=" << scale
                  << " native_px=" << native_px
                  << " whole_q=" << whole_query.word_spacing_percent.value
                  << " native_q=" << native_query_value
                  << " max|whole-native|=" << max_native << std::endl;
    }

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml) << "the two native edits must coalesce into one Undo";
    EXPECT_FALSE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), native_xml);
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);

    std::vector<double> partial_x;
    std::string partial_xml;
    RawSelectionRange raw_before;
    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        UI::Tools::TextTool *tool = nullptr;
        ASSERT_TRUE(placeLastHardLineSelection(desktop, lines, tool, /*reversed=*/true));
        raw_before = rawSelectionRange(desktop);
        ASSERT_TRUE(raw_before.valid);
        ASSERT_GT(raw_before.start, raw_before.end);

        Gtk::Window window;
        auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
        panel->setDesktop(desktop);
        window.set_child(*panel);
        window.present();
        drainMainContext();

        auto *spin = panelSpacingSpin(*panel, spacing_tooltip);
        ASSERT_TRUE(spin);
        spin->set_value(150.0);
        drainMainContext();
        document->ensureUpToDate();

        partial_x = last_line_x();
        partial_xml = xml();
        window.unset_child();
    }
    ASSERT_FALSE(partial_x.empty());
    writeEvidenceSvg("SP2-word-partial", partial_xml);

    auto const partial_query = desktop->textStyleController().query();
    ASSERT_TRUE(partial_query.word_spacing_percent.valid);
    EXPECT_FALSE(partial_query.word_spacing_percent.mixed);
    EXPECT_NEAR(partial_query.word_spacing_percent.value, 150.0, 1e-4)
        << "partial word spacing must query 150; 125 proves the local px was divided again";

    ASSERT_EQ(partial_x.size(), whole_x.size());
    for (size_t i = 0; i < partial_x.size(); ++i) {
        EXPECT_NEAR(partial_x[i], whole_x[i], 0.5)
            << "last-line glyph " << i << " whole=" << whole_x[i] << " partial=" << partial_x[i];
    }
    {
        double max_partial = 0.0;
        for (size_t i = 0; i < whole_x.size() && i < partial_x.size(); ++i) {
            double d = partial_x[i] - whole_x[i];
            if (d < 0) d = -d;
            if (d > max_partial) max_partial = d;
        }
        std::cout << "[SP2] partial_q=" << partial_query.word_spacing_percent.value
                  << " partial0=" << partial_x.front() << " whole0=" << whole_x.front()
                  << " partial_last=" << partial_x.back() << " whole_last=" << whole_x.back()
                  << " max|whole-partial|=" << max_partial << std::endl;
    }
    expectUnselectedFirstLinePreserved(document, "word-space partial route");
    {
        auto const raw = rawSelectionRange(desktop);
        ASSERT_TRUE(raw.valid);
        EXPECT_EQ(raw.start, raw_before.start) << "raw logical start must survive";
        EXPECT_EQ(raw.end, raw_before.end) << "raw logical end must survive";
        EXPECT_GT(raw.start, raw.end) << "reversed selection direction must survive";
    }

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml) << "one partial request must be one Undo";
    EXPECT_FALSE(DocumentUndo::undo(document));
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), partial_xml);

    {
        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        UI::Tools::TextTool *tool = nullptr;
        ASSERT_TRUE(placeLastHardLineSelection(desktop, lines, tool, /*reversed=*/false));
        UI::TextStylePatch patch175;
        patch175.word_spacing_percent = 175.0;
        ASSERT_TRUE(desktop->textStyleController().commit(
            patch175, "test:word-spacing-175", RC_("Undo", "Set word spacing 175")));
        document->ensureUpToDate();
        auto const state175_xml = xml();

        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        auto const state150_xml = xml();
        EXPECT_NE(state150_xml, state175_xml);

        lines = cast<SPText>(document->getObjectById("line-text"));
        ASSERT_TRUE(lines);
        ASSERT_TRUE(placeLastHardLineSelection(desktop, lines, tool, /*reversed=*/false));
        auto const state150_query = desktop->textStyleController().query();
        ASSERT_TRUE(state150_query.word_spacing_percent.valid);
        EXPECT_NEAR(state150_query.word_spacing_percent.value, 150.0, 1e-4)
            << "the no-op prestate must genuinely be the equivalent 150% state";

        UI::TextStylePatch patch150;
        patch150.word_spacing_percent = 150.0;
        EXPECT_FALSE(desktop->textStyleController().commitContinuous(
            patch150, "text-panel:word-spacing", RC_("Undo", "Set word spacing")))
            << "repeating the already-active 150 must be a no-op";
        EXPECT_EQ(xml(), state150_xml) << "the no-op must not change the document";
        ASSERT_TRUE(DocumentUndo::redo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), state175_xml) << "the no-op must preserve the Redo entry";
    }
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), initial_xml);

    {
        std::unique_ptr<SPDocument> reopened{SPDocument::createNewDocFromMem(whole_xml)};
        ASSERT_TRUE(reopened);
        reopened->ensureUpToDate();
        auto *rt = cast<SPText>(reopened->getObjectById("line-text"));
        ASSERT_TRUE(rt);
        unsigned rf = 0;
        unsigned rl = 0;
        ASSERT_TRUE(lastHardLineRange(*rt, rf, rl));
        auto const rx = documentGlyphX(*rt, rf, rl);
        ASSERT_EQ(rx.size(), whole_x.size());
        for (size_t i = 0; i < rx.size(); ++i) EXPECT_NEAR(rx[i], whole_x[i], 1e-6);
        expectUnselectedFirstLinePreserved(reopened.get(), "word-space whole save/reopen");
    }
    {
        std::unique_ptr<SPDocument> reopened{SPDocument::createNewDocFromMem(partial_xml)};
        ASSERT_TRUE(reopened);
        reopened->ensureUpToDate();
        auto *rt = cast<SPText>(reopened->getObjectById("line-text"));
        ASSERT_TRUE(rt);
        unsigned rf = 0;
        unsigned rl = 0;
        ASSERT_TRUE(lastHardLineRange(*rt, rf, rl));
        auto const rx = documentGlyphX(*rt, rf, rl);
        ASSERT_EQ(rx.size(), partial_x.size());
        for (size_t i = 0; i < rx.size(); ++i) EXPECT_NEAR(rx[i], partial_x[i], 1e-6);
    }
}

// Percentage character/word spacing on a scaled ordinary two-line text whose
// LAST hard line is a nested tspan carrying its own scale(3) under the root
// scale(2). The rendered layout honours only the root scale, so selecting ONLY
// that nested last span through text.end must reach the same local spacing as
// the whole-object reference. The partial route goes through
// sp_te_apply_style(), whose exclusive end-at-text-end resolves the common
// ancestor to the root, while the controller pre-multiplies the run's nested
// span scale; the two must still agree. The unselected first line is the
// prefix that keeps this a genuine subrange rather than a whole-text target.
TEST_F(TextMultiStyleTest, NestedSpanTransformPartialPercentSpacingMatchesWholeRoute)
{
    ASSERT_TRUE(configureSpacingFixture(document));
    auto *line_two = document->getObjectById("line-two");
    ASSERT_TRUE(line_two);
    line_two->setAttribute("transform", "scale(3)");
    document->ensureUpToDate();
    checkpoint();

    auto *lines = cast<SPText>(document->getObjectById("line-text"));
    ASSERT_TRUE(lines);
    ASSERT_NEAR(lines->i2doc_affine().descrim(), 2.0, 1e-6);
    auto *nested = cast<SPItem>(document->getObjectById("line-two"));
    ASSERT_TRUE(nested);
    ASSERT_NEAR(nested->i2doc_affine().descrim(), 6.0, 1e-6)
        << "fixture precondition: nested scale(3) under root scale(2)";

    std::string const baseline_xml = xml();
    unsigned first = 0;
    unsigned last = 0;
    ASSERT_TRUE(lastHardLineRange(*lines, first, last));
    auto const baseline_x = documentGlyphX(*lines, first, last);
    ASSERT_GE(baseline_x.size(), 2u);
    writeEvidenceSvg("NEST-before", baseline_xml);

    struct SpacingCase {
        char const *label;
        char const *tooltip;
        double percent;
        UI::TextStyleValue<double> UI::TextStyleSnapshot::*query_field;
    };
    SpacingCase const cases[] = {
        {"char50", "Character spacing relative to the effective space advance", 50.0,
         &UI::TextStyleSnapshot::character_spacing_percent},
        {"word150", "Word spacing; 100% is normal", 150.0,
         &UI::TextStyleSnapshot::word_spacing_percent},
    };

    for (auto const &c : cases) {
        SCOPED_TRACE(c.label);
        ASSERT_EQ(xml(), baseline_xml) << "each case must start from the restored fixture";

        auto last_line_x = [&]() {
            auto *text = cast<SPText>(document->getObjectById("line-text"));
            unsigned f = 0;
            unsigned l = 0;
            if (!text || !lastHardLineRange(*text, f, l)) return std::vector<double>{};
            return documentGlyphX(*text, f, l);
        };

        // Whole-object reference: caret in the unselected first line, so the
        // request targets the whole text object rather than a subrange.
        std::vector<double> whole_x;
        std::string whole_xml;
        UI::TextStyleSnapshot whole_query;
        {
            auto *text = cast<SPText>(document->getObjectById("line-text"));
            ASSERT_TRUE(text);
            UI::Tools::TextTool *tool = nullptr;
            placeCaretInFirstHardLine(desktop, text, tool);

            Gtk::Window window;
            auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
            panel->setDesktop(desktop);
            window.set_child(*panel);
            window.present();
            drainMainContext();

            auto *spin = panelSpacingSpin(*panel, c.tooltip);
            ASSERT_TRUE(spin);
            spin->set_value(c.percent);
            drainMainContext();
            document->ensureUpToDate();

            whole_x = last_line_x();
            whole_xml = xml();
            whole_query = desktop->textStyleController().query();
            window.unset_child();
        }
        ASSERT_FALSE(whole_x.empty());
        writeEvidenceSvg((std::string("NEST-") + c.label + "-whole").c_str(), whole_xml);
        ASSERT_NE(whole_x, baseline_x)
            << "the whole-object request must change the last-line glyphs";
        ASSERT_TRUE((whole_query.*c.query_field).valid);
        EXPECT_FALSE((whole_query.*c.query_field).mixed);
        EXPECT_NEAR((whole_query.*c.query_field).value, c.percent, 1e-4)
            << "whole-object percent is the reference";
        {
            auto *text = cast<SPText>(document->getObjectById("line-text"));
            ASSERT_TRUE(text);
            UI::Tools::TextTool *tool = nullptr;
            placeCaretInFirstHardLine(desktop, text, tool);
            UI::TextStylePatch same;
            if (c.query_field == &UI::TextStyleSnapshot::character_spacing_percent) {
                same.character_spacing_percent = c.percent;
            } else {
                same.word_spacing_percent = c.percent;
            }
            EXPECT_FALSE(desktop->textStyleController().commitContinuous(
                same, "text-panel:spacing", RC_("Undo", "Set spacing")))
                << "repeating the reached whole-object percent must be a no-op";
            EXPECT_EQ(xml(), whole_xml) << "the no-op must not change the document";
        }

        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), baseline_xml);
        EXPECT_FALSE(DocumentUndo::undo(document));
        ASSERT_TRUE(DocumentUndo::redo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), whole_xml);
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), baseline_xml);

        // Partial route: select ONLY the nested last span through text.end.
        std::vector<double> partial_x;
        std::string partial_xml;
        UI::TextStyleSnapshot partial_query;
        RawSelectionRange raw_before;
        {
            auto *text = cast<SPText>(document->getObjectById("line-text"));
            ASSERT_TRUE(text);
            UI::Tools::TextTool *tool = nullptr;
            ASSERT_TRUE(placeLastHardLineSelection(desktop, text, tool, /*reversed=*/true));
            raw_before = rawSelectionRange(desktop);
            ASSERT_TRUE(raw_before.valid);
            ASSERT_GT(raw_before.start, raw_before.end);

            Gtk::Window window;
            auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
            panel->setDesktop(desktop);
            window.set_child(*panel);
            window.present();
            drainMainContext();

            auto *spin = panelSpacingSpin(*panel, c.tooltip);
            ASSERT_TRUE(spin);
            spin->set_value(c.percent);
            drainMainContext();
            document->ensureUpToDate();

            partial_x = last_line_x();
            partial_xml = xml();
            partial_query = desktop->textStyleController().query();
            window.unset_child();
        }
        ASSERT_FALSE(partial_x.empty());
        writeEvidenceSvg((std::string("NEST-") + c.label + "-partial").c_str(), partial_xml);

        EXPECT_TRUE((partial_query.*c.query_field).valid);
        EXPECT_FALSE((partial_query.*c.query_field).mixed);
        EXPECT_NEAR((partial_query.*c.query_field).value, c.percent, 1e-4)
            << "nested-span partial percent must equal the whole-object reference";
        ASSERT_EQ(partial_x.size(), whole_x.size());
        for (size_t i = 0; i < partial_x.size(); ++i) {
            EXPECT_NEAR(partial_x[i], whole_x[i], 0.5)
                << "last-line glyph " << i << " whole=" << whole_x[i]
                << " partial=" << partial_x[i];
        }

        expectUnselectedFirstLinePreserved(document, "nested-span partial route");
        {
            auto *tspan = cast<SPTSpan>(document->getObjectById("line-two"));
            ASSERT_TRUE(tspan);
            EXPECT_EQ(descendantText(tspan), "Second line");
            char const *tr = tspan->getAttribute("transform");
            ASSERT_TRUE(tr);
            EXPECT_EQ(std::string(tr), "scale(3)");
        }
        {
            auto const raw = rawSelectionRange(desktop);
            ASSERT_TRUE(raw.valid);
            EXPECT_EQ(raw.start, raw_before.start) << "raw logical start must survive";
            EXPECT_EQ(raw.end, raw_before.end) << "raw logical end must survive";
            EXPECT_GT(raw.start, raw.end) << "reversed selection direction must survive";
        }

        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), baseline_xml) << "one partial request must be one Undo";
        EXPECT_FALSE(DocumentUndo::undo(document));
        ASSERT_TRUE(DocumentUndo::redo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), partial_xml);
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), baseline_xml);
    }
}

// ---- F4: heterogeneous paragraph/frame dropdown state -----------------------

Gtk::DropDown *panelDropdown(Gtk::Widget &panel, Glib::ustring const &tooltip)
{
    return dynamic_cast<Gtk::DropDown *>(findWidgetByTooltip(panel, tooltip));
}

Glib::ustring dropdownItem(Gtk::DropDown &dropdown, guint index)
{
    auto model = dropdown.get_model();
    if (!model || !GTK_IS_STRING_LIST(model->gobj())) return Glib::ustring{};
    auto const *text = gtk_string_list_get_string(GTK_STRING_LIST(model->gobj()), index);
    return text ? Glib::ustring{text} : Glib::ustring{};
}

guint dropdownItemCount(Gtk::DropDown &dropdown)
{
    return dropdown.get_model() ? dropdown.get_model()->get_n_items() : 0u;
}

// The mixed indication is a presentation-only item appended after the real
// options. It is selected only while the selection is heterogeneous and can
// never be committed.
bool dropdownShowsMixedSentinel(Gtk::DropDown &dropdown)
{
    auto const count = dropdownItemCount(dropdown);
    return count > 0 && dropdownItem(dropdown, count - 1) == "Multiple values"
        && dropdown.get_selected() == count - 1;
}

// Drive the same public GtkDropDown::selected notification a real selection
// change produces, while the current index stays put, and return the number of
// slots that received it. GtkSingleSelection rejects
// GTK_INVALID_LIST_POSITION on a non-empty model (can-unselect=false,
// autoselect=true), so moving off the sentinel and back cannot reliably reach
// the commit handler; an explicit in-place notification does. The returned
// count must be 1, proving the notification was delivered to the panel handler
// connected to this signal.
unsigned notifyDropdownSelected(Gtk::DropDown &dropdown)
{
    unsigned deliveries = 0;
    auto connection = dropdown.property_selected().signal_changed().connect(
        [&deliveries] { ++deliveries; });
    g_object_notify(G_OBJECT(dropdown.gobj()), "selected");
    connection.disconnect();
    return deliveries;
}

// A heterogeneous selection must show the sentinel, must not mutate the
// document by synchronizing, and must clear a stale value when there is no text
// target. The real CSS `mixed` glyph orientation stays a distinct real option.
TEST_F(TextMultiStyleTest, MixedParagraphDropdownsShowSentinelWithoutMutatingDocument)
{
    auto *a = cast<SPText>(document->getObjectById("first"));
    auto *b = cast<SPText>(document->getObjectById("second"));
    ASSERT_TRUE(a && b);
    a->getRepr()->setAttribute("style",
        "font-family:serif;direction:ltr;writing-mode:horizontal-tb;text-orientation:mixed");
    b->getRepr()->setAttribute("style",
        "font-family:serif;direction:rtl;writing-mode:vertical-rl;text-orientation:sideways");
    checkpoint();
    auto const baseline = xml();

    desktop->getSelection()->set(a);
    drainMainContext();

    Gtk::Window window;
    auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
    panel->setDesktop(desktop);
    window.set_child(*panel);
    window.present();
    drainMainContext();
    expandPanelSection(*panel, "Paragraph");
    drainMainContext();

    auto *direction = panelDropdown(*panel, "Paragraph text direction");
    auto *writing = panelDropdown(*panel, "Text writing mode");
    auto *orientation = panelDropdown(*panel, "Glyph orientation in vertical text");
    ASSERT_TRUE(direction && writing && orientation);

    // Uniform: only the real options, and the real CSS `mixed` orientation is
    // index 0 of the orientation dropdown, not the heterogeneity indication.
    EXPECT_EQ(direction->get_selected(), 0u);
    EXPECT_FALSE(dropdownShowsMixedSentinel(*direction));
    EXPECT_EQ(writing->get_selected(), 0u);
    EXPECT_FALSE(dropdownShowsMixedSentinel(*writing));
    EXPECT_EQ(orientation->get_selected(), 0u);
    EXPECT_FALSE(dropdownShowsMixedSentinel(*orientation));
    ASSERT_EQ(dropdownItemCount(*orientation), 3u);
    EXPECT_EQ(dropdownItem(*orientation, 0), "Auto glyphs");

    // Heterogeneous: all three show the sentinel selected, with the real
    // options still present at their original indices.
    desktop->getSelection()->setList(std::vector<SPItem *>{a, b, vector});
    drainMainContext();
    EXPECT_TRUE(dropdownShowsMixedSentinel(*direction));
    EXPECT_TRUE(dropdownShowsMixedSentinel(*writing));
    EXPECT_TRUE(dropdownShowsMixedSentinel(*orientation));
    ASSERT_EQ(dropdownItemCount(*orientation), 4u);
    EXPECT_EQ(dropdownItem(*orientation, 0), "Auto glyphs");
    EXPECT_EQ(dropdownItem(*orientation, 3), "Multiple values");

    // The presentation-only sentinel is the current index. GTK's
    // SingleSelection rejects GTK_INVALID_LIST_POSITION on a non-empty model
    // (can-unselect=false, autoselect=true), so moving off the sentinel and back
    // cannot reliably exercise the commit guard. Instead emit the public
    // GtkDropDown::selected notification in place and count deliveries to the
    // same signal the panel handler listens to: the handler must reject the
    // out-of-range index before it can build a patch, so XML and history stay
    // untouched.
    auto const mixed_before = xml();
    EXPECT_EQ(notifyDropdownSelected(*direction), 1u)
        << "the selected notification must reach the panel handler";
    EXPECT_EQ(notifyDropdownSelected(*writing), 1u)
        << "the selected notification must reach the panel handler";
    EXPECT_EQ(notifyDropdownSelected(*orientation), 1u)
        << "the selected notification must reach the panel handler";
    drainMainContext();
    EXPECT_TRUE(dropdownShowsMixedSentinel(*direction));
    EXPECT_TRUE(dropdownShowsMixedSentinel(*writing));
    EXPECT_TRUE(dropdownShowsMixedSentinel(*orientation));
    EXPECT_EQ(xml(), mixed_before) << "selecting the mixed sentinel must not mutate the document";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "selecting the sentinel must not add an Undo entry";
    EXPECT_FALSE(DocumentUndo::redo(document)) << "selecting the sentinel must not add a Redo entry";

    // Showing the mixed sentinel through synchronization is read-only: the
    // document is still the pre-selection baseline and no history was created.
    EXPECT_EQ(xml(), baseline) << "showing the mixed sentinel must not mutate the document";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "sync must not add an Undo entry";
    EXPECT_FALSE(DocumentUndo::redo(document)) << "sync must not add a Redo entry";

    // No text target: the control must not keep the previous selection's value.
    // The model is emptied so the native selection truly becomes invalid, not
    // merely asked to clear on a non-empty model (which GTK rejects).
    desktop->getSelection()->set(vector);
    drainMainContext();
    EXPECT_EQ(dropdownItemCount(*direction), 0u) << "no-target control must be truly empty";
    EXPECT_EQ(dropdownItemCount(*writing), 0u) << "no-target control must be truly empty";
    EXPECT_EQ(dropdownItemCount(*orientation), 0u) << "no-target control must be truly empty";
    EXPECT_EQ(direction->get_selected(), GTK_INVALID_LIST_POSITION);
    EXPECT_EQ(writing->get_selected(), GTK_INVALID_LIST_POSITION);
    EXPECT_EQ(orientation->get_selected(), GTK_INVALID_LIST_POSITION);
    window.unset_child();

    // Synchronizing the display is read-only: same XML, no Undo and no Redo.
    EXPECT_EQ(xml(), baseline);
    EXPECT_FALSE(DocumentUndo::undo(document)) << "sync must not add an Undo entry";
    EXPECT_FALSE(DocumentUndo::redo(document)) << "sync must not add a Redo entry";
}

// Choosing the value the dropdown showed before the selection became mixed must
// apply to every compatible text target, preserve incompatible artwork, and
// fold into exactly one Undo/Redo pair even though the displayed index did not
// change from the user's point of view.
TEST_F(TextMultiStyleTest, MixedParagraphDropdownsApplyFormerlyDisplayedValueWithOneUndoRedo)
{
    auto run = [&](Glib::ustring const &tooltip,
                   char const *first_style, char const *second_style,
                   unsigned real_index, std::function<void(bool)> const &expect_state) {
        auto *a = cast<SPText>(document->getObjectById("first"));
        auto *b = cast<SPText>(document->getObjectById("second"));
        ASSERT_TRUE(a && b);
        a->getRepr()->setAttribute("style", first_style);
        b->getRepr()->setAttribute("style", second_style);
        checkpoint();
        auto const baseline = xml();
        auto const vector_before = subtree("vector");

        desktop->getSelection()->set(a);
        drainMainContext();

        Gtk::Window window;
        auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
        panel->setDesktop(desktop);
        window.set_child(*panel);
        window.present();
        drainMainContext();
        expandPanelSection(*panel, "Paragraph");
        drainMainContext();

        auto *dropdown = panelDropdown(*panel, tooltip);
        ASSERT_TRUE(dropdown);
        expect_state(false);
        EXPECT_EQ(dropdown->get_selected(), real_index)
            << "uniform selection must display its real value";

        // No text target: the control must clear to an empty model and an
        // invalid selection without creating XML or a history entry. History is
        // still empty here (checkpoint cleared it), so a read-only sync leaves
        // both Undo and Redo false.
        desktop->getSelection()->set(vector);
        drainMainContext();
        EXPECT_EQ(dropdownItemCount(*dropdown), 0u) << "no-target control must be truly empty";
        EXPECT_EQ(dropdown->get_selected(), GTK_INVALID_LIST_POSITION);
        EXPECT_EQ(xml(), baseline) << "no-target sync must not mutate the document";
        EXPECT_FALSE(DocumentUndo::undo(document)) << "no-target sync must not add an Undo entry";
        EXPECT_FALSE(DocumentUndo::redo(document)) << "no-target sync must not add a Redo entry";

        // Re-selecting a real text target repopulates the same model identity
        // with the real value at its stable index.
        desktop->getSelection()->set(a);
        drainMainContext();
        expect_state(false);
        EXPECT_EQ(dropdown->get_selected(), real_index)
            << "uniform selection must display its real value after a no-target clear";

        // The formerly displayed value stays the same index, but the selection
        // is now mixed and the control must show the sentinel instead.
        desktop->getSelection()->setList(std::vector<SPItem *>{a, b, vector});
        drainMainContext();
        EXPECT_TRUE(dropdownShowsMixedSentinel(*dropdown));
        expect_state(true);
        EXPECT_EQ(xml(), baseline) << "showing the mixed sentinel must not mutate the document";

        // Picking the formerly displayed real value is a real change from the
        // sentinel, so it must commit to every text target.
        dropdown->set_selected(real_index);
        drainMainContext();
        document->ensureUpToDate();
        expect_state(false);
        EXPECT_EQ(subtree("vector"), vector_before) << "incompatible artwork must be preserved";

        auto const applied = xml();

        // Save/reopen preserves the applied state at the document level.
        auto const saved = sp_repr_save_buf(document->getReprDoc()).raw();
        std::unique_ptr<SPDocument> reopened{SPDocument::createNewDocFromMem(saved)};
        ASSERT_TRUE(reopened);
        reopened->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(reopened->getReprDoc()).raw(), saved);

        // One Undo restores the exact original differences; Redo applies once.
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), baseline);
        expect_state(true);
        ASSERT_TRUE(DocumentUndo::redo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), applied);
        expect_state(false);
        ASSERT_TRUE(DocumentUndo::undo(document));
        document->ensureUpToDate();
        EXPECT_EQ(xml(), baseline);
        window.unset_child();
    };

    auto check_direction = [&](bool mixed) {
        auto const value = desktop->textStyleController().queryParagraph().direction;
        ASSERT_TRUE(value.valid);
        EXPECT_EQ(value.mixed, mixed);
        if (!mixed) EXPECT_EQ(value.value, UI::TextParagraphDirection::LeftToRight);
    };
    run("Paragraph text direction",
        "font-family:serif;direction:ltr;writing-mode:horizontal-tb;text-orientation:mixed",
        "font-family:serif;direction:rtl;writing-mode:horizontal-tb;text-orientation:mixed",
        0u, check_direction);

    auto check_writing = [&](bool mixed) {
        auto const value = desktop->textStyleController().queryParagraph().writing_mode;
        ASSERT_TRUE(value.valid);
        EXPECT_EQ(value.mixed, mixed);
        if (!mixed) EXPECT_EQ(value.value, UI::TextParagraphWritingMode::Horizontal);
    };
    run("Text writing mode",
        "font-family:serif;direction:ltr;writing-mode:horizontal-tb;text-orientation:mixed",
        "font-family:serif;direction:ltr;writing-mode:vertical-rl;text-orientation:mixed",
        0u, check_writing);

    auto check_orientation = [&](bool mixed) {
        auto const value = desktop->textStyleController().queryParagraph().orientation;
        ASSERT_TRUE(value.valid);
        EXPECT_EQ(value.mixed, mixed);
        if (!mixed) EXPECT_EQ(value.value, UI::TextParagraphOrientation::Mixed);
    };
    run("Glyph orientation in vertical text",
        "font-family:serif;direction:ltr;writing-mode:horizontal-tb;text-orientation:mixed",
        "font-family:serif;direction:ltr;writing-mode:horizontal-tb;text-orientation:sideways",
        0u, check_orientation);
}

// Frame vertical alignment is only enabled for single-column native frames. The
// mixed case must use real eligible frames, show the sentinel, and still apply
// the formerly displayed value in one Undo.
TEST_F(TextMultiStyleTest, MixedFrameVerticalAlignmentShowsSentinelAndApplies)
{
    auto *a = cast<SPText>(document->getObjectById("first"));
    auto *b = cast<SPText>(document->getObjectById("second"));
    ASSERT_TRUE(a && b);

    // Build genuine single-column native text frames through the same production
    // entry point the text panel uses, so the fixture carries the real
    // shape-inside rectangle in svg:defs and the inkscape:text-frame-* state
    // (generated/width/height/columns/gap/align) instead of attribute-only
    // stand-ins. Width/height are explicit fixture values; x/y come from the
    // text's own geometric bounds inside setTextFrameSettings.
    UI::TextFrameSettings frame;
    frame.width = 100.0;
    frame.height = 50.0;
    frame.columns = 1;
    frame.gap = 0.0;
    frame.vertical_alignment = UI::TextFrameVerticalAlignment::Top;
    ASSERT_TRUE(UI::setTextFrameSettings(*document, std::vector<SPText *>{a}, frame));
    frame.vertical_alignment = UI::TextFrameVerticalAlignment::Middle;
    ASSERT_TRUE(UI::setTextFrameSettings(*document, std::vector<SPText *>{b}, frame));
    checkpoint();
    auto const baseline = xml();
    auto const vector_before = subtree("vector");

    desktop->getSelection()->set(a);
    drainMainContext();

    Gtk::Window window;
    auto *panel = Gtk::make_managed<UI::Dialog::TextPanel>();
    panel->setDesktop(desktop);
    window.set_child(*panel);
    window.present();
    drainMainContext();
    expandPanelSection(*panel, "Text frame & columns");
    drainMainContext();

    auto *dropdown = panelDropdown(*panel, "Vertical alignment within a single text frame");
    ASSERT_TRUE(dropdown);
    EXPECT_TRUE(dropdown->get_sensitive()) << "single-column frame must stay enabled";
    EXPECT_EQ(dropdown->get_selected(), 0u);
    EXPECT_FALSE(dropdownShowsMixedSentinel(*dropdown));

    desktop->getSelection()->setList(std::vector<SPItem *>{a, b, vector});
    drainMainContext();
    EXPECT_TRUE(dropdownShowsMixedSentinel(*dropdown));
    EXPECT_TRUE(dropdown->get_sensitive());

    // Synchronizing the mixed frame state is read-only. GTK's SingleSelection
    // rejects GTK_INVALID_LIST_POSITION on a non-empty model, so drive the same
    // handler with an explicit public selected notification while the sentinel
    // is current and prove delivery with a counter.
    auto const mixed_before = xml();
    EXPECT_EQ(xml(), baseline) << "showing the mixed frame sentinel must not mutate the document";
    EXPECT_EQ(notifyDropdownSelected(*dropdown), 1u)
        << "the selected notification must reach the panel handler";
    drainMainContext();
    EXPECT_TRUE(dropdownShowsMixedSentinel(*dropdown));
    EXPECT_EQ(xml(), mixed_before) << "frame sync and sentinel selection must not mutate the document";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "frame sync and sentinel selection must not add an Undo entry";
    EXPECT_FALSE(DocumentUndo::redo(document)) << "frame sync and sentinel selection must not add a Redo entry";

    // No text target: the frame control must clear to an empty model and an
    // invalid selection without mutating the document or history.
    desktop->getSelection()->set(vector);
    drainMainContext();
    EXPECT_EQ(dropdownItemCount(*dropdown), 0u) << "no-target frame control must be truly empty";
    EXPECT_EQ(dropdown->get_selected(), GTK_INVALID_LIST_POSITION);
    EXPECT_EQ(xml(), baseline) << "no-target frame sync must not mutate the document";
    EXPECT_FALSE(DocumentUndo::undo(document)) << "no-target frame sync must not add an Undo entry";
    EXPECT_FALSE(DocumentUndo::redo(document)) << "no-target frame sync must not add a Redo entry";

    // Restore the two native frames with mixed alignment before applying.
    desktop->getSelection()->setList(std::vector<SPItem *>{a, b, vector});
    drainMainContext();
    EXPECT_TRUE(dropdownShowsMixedSentinel(*dropdown));

    dropdown->set_selected(0u);
    drainMainContext();
    document->ensureUpToDate();
    auto const applied = xml();
    auto const value = desktop->textStyleController().queryFrame().vertical_alignment;
    ASSERT_TRUE(value.valid);
    EXPECT_FALSE(value.mixed);
    EXPECT_EQ(value.value, UI::TextFrameVerticalAlignment::Top);
    EXPECT_EQ(subtree("vector"), vector_before);

    // Save/reopen preserves the applied native frame state at document level.
    auto const saved = sp_repr_save_buf(document->getReprDoc()).raw();
    std::unique_ptr<SPDocument> reopened{SPDocument::createNewDocFromMem(saved)};
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(reopened->getReprDoc()).raw(), saved);

    ASSERT_TRUE(DocumentUndo::undo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), baseline);
    ASSERT_TRUE(DocumentUndo::redo(document));
    document->ensureUpToDate();
    EXPECT_EQ(xml(), applied);
    window.unset_child();
}

TEST_F(TextMultiStyleTest, PanelKeepsFaceRowsAcrossSameFamilyCursorUpdates)
{
    desktop->getSelection()->set(first);
    UI::TextStylePatch initial;
    initial.family = "Sans";
    initial.face = "Normal";
    initial.fontspec = "Sans";
    ASSERT_TRUE(desktop->textStyleController().commit(initial, "st-q:initial", RC_("Undo", "ST-Q initial")));
    desktop->textStyleController().setPanelFontOnly(false);
    UI::Dialog::TextPanel panel;
    panel.setDesktop(desktop);
    // Settle the initial document-font refresh before measuring repeated updates.
    desktop->emit_text_cursor_moved(nullptr);
    panel.update();
    auto menu = dynamic_cast<Gtk::MenuButton *>(findWidgetByTooltip(panel, "Font style or face"));
    if (!menu) {
        menu = dynamic_cast<Gtk::MenuButton *>(findWidgetByTooltip(
            panel, "The requested style is unavailable; a fallback face may be used."));
    }
    ASSERT_TRUE(menu);
    auto popover = menu->get_popover();
    ASSERT_TRUE(popover);
    auto faces = findWidget<Gtk::ListBox>(*popover);
    ASSERT_TRUE(faces);
    auto row = faces->get_row_at_index(0);
    ASSERT_TRUE(row);
    // Keep the old row alive so a destroy/recreate cannot reuse its address.
    g_object_ref(row->gobj());
    auto retained = std::unique_ptr<GObject, decltype(&g_object_unref)>(G_OBJECT(row->gobj()), g_object_unref);
    auto const before = xml();
    unsigned retained_updates = 0;
    for (unsigned i = 0; i < 20; ++i) {
        desktop->emit_text_cursor_moved(nullptr);
        panel.update();
        if (faces->get_row_at_index(0) == row) ++retained_updates;
    }
    EXPECT_EQ(retained_updates, 20u) << "same-family updates rebuilt face rows";
    EXPECT_EQ(xml(), before);

    // A different family must replace the rows, even if its styles are identical.
    UI::TextStylePatch patch;
    patch.family = "ST-Q unavailable family";
    desktop->textStyleController().commit(patch, "st-q:family", RC_("Undo", "ST-Q family"));
    panel.update();
    EXPECT_NE(faces->get_row_at_index(0), row);
}

} // namespace
