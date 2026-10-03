// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRAW-1: a newly drawn object is never invisible. When the style a drawing
 * tool would apply shows nothing on screen, the object gets a 0.1 mm black
 * stroke; visible styles are left untouched.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>
#include <gtkmm/application.h>
#include <glib/gstdio.h>

#include <csignal>
#include <cstdlib>
#include <string>
#include <string_view>

#include <2geom/svg-path-parser.h>
#include "ui/tools/pen-tool.h"
#include "document-undo.h"
#include "selection.h"
#include "desktop.h"
#include "desktop-style.h"
#include "document.h"
#include "inkscape-application.h"
#include "layer-manager.h"
#include "object/sp-rect.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "style.h"
#include "ui/tools/tool-base.h"
#include "ui/widget/events/canvas-event.h"
#include "xml/repr.h"

#include <map>
#include <optional>

namespace {

std::string property(SPCSSAttr *css, char const *name)
{
    auto const *value = css ? sp_repr_css_property(css, name, nullptr) : nullptr;
    return value ? value : "";
}

SPCSSAttr *style(char const *text)
{
    auto *css = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(css, text);
    return css;
}

TEST(NewObjectVisibility, InvisibleStylesAreRecognized)
{
    for (auto const *text : {"fill:none;stroke:none", "fill:none", "fill:transparent;stroke:none",
                             "fill:#ff000000", "fill:red;fill-opacity:0", "fill:none;stroke:black;stroke-width:0",
                             "fill:none;stroke:black;stroke-opacity:0", "fill:none;stroke:#00000000",
                             "opacity:0;fill:red", "fill:red;display:none", "fill:red;visibility:hidden",
                             "fill:red;fill-opacity:0%"}) {
        SCOPED_TRACE(text);
        auto *css = style(text);
        EXPECT_TRUE(sp_css_attr_paints_nothing(css));
        sp_repr_css_attr_unref(css);
    }
}

TEST(NewObjectVisibility, VisibleStylesAreRecognized)
{
    for (auto const *text : {"", "fill:red", "fill:none;stroke:blue", "stroke-width:0",
                             "fill:none;stroke:black;stroke-width:0.01", "fill:url(#gradient)",
                             "fill:none;stroke:currentColor", "fill:#ff0000;fill-opacity:0.2",
                             "opacity:0.1;fill:red", "fill:none;stroke:red;stroke-width:1px"}) {
        SCOPED_TRACE(text);
        auto *css = style(text);
        EXPECT_FALSE(sp_css_attr_paints_nothing(css));
        sp_repr_css_attr_unref(css);
    }
}

TEST(NewObjectVisibility, EnsureVisibleAddsAThinBlackStrokeOnlyWhenNeeded)
{
    auto *invisible = style("fill:none;stroke:none;opacity:0;display:none");
    EXPECT_TRUE(sp_css_attr_ensure_visible(invisible, 0.25));
    EXPECT_EQ(property(invisible, "stroke"), "#000000");
    EXPECT_EQ(property(invisible, "stroke-opacity"), "1");
    EXPECT_DOUBLE_EQ(sp_repr_css_double_property(invisible, "stroke-width", 0), 0.25);
    EXPECT_EQ(property(invisible, "opacity"), "1");
    EXPECT_EQ(property(invisible, "display"), "inline");
    EXPECT_EQ(property(invisible, "fill"), "none");
    EXPECT_FALSE(sp_css_attr_paints_nothing(invisible));
    sp_repr_css_attr_unref(invisible);

    auto *visible = style("fill:#123456;stroke:none");
    EXPECT_FALSE(sp_css_attr_ensure_visible(visible, 0.25));
    EXPECT_EQ(property(visible, "stroke"), "none");
    EXPECT_EQ(property(visible, "fill"), "#123456");
    sp_repr_css_attr_unref(visible);
}

// Through the real tool style lookup of a desktop on a millimetre document.
class NewObjectVisibilityDesktop : public ::testing::Test
{
protected:
    static InkscapeApplication &application()
    {
        static auto *instance = [] {
            Gtk::Application::wrap_in_search_entry2();
            g_setenv("INKSCAPE_APP_ID_TAG", "newobjectvisibilitytest", true);
            auto *result = new InkscapeApplication();
            result->gio_app()->register_application();
            for (auto signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) {
                std::signal(signal, SIG_DFL);
            }
            return result;
        }();
        return *instance;
    }

    void SetUp() override
    {
        auto const *gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "GUI testing not enabled";
        }
        // Preferences this test writes; restored in TearDown.
        auto *prefs = Inkscape::Preferences::get();
        for (auto const *path : {"/tools/shapes/rect/usecurrent", "/tools/shapes/rect/style",
                                 "/tools/freehand/pen/usecurrent", "/desktop/path/style",
                                 "/tools/freehand/pen/shape", "/tools/freehand/pen/freehand-mode",
                                 "/tools/freehand/pen/simplify"}) {
            auto const entry = prefs->getEntry(path);
            saved[path] = entry.isSet() ? std::optional<Glib::ustring>(entry.getString()) : std::nullopt;
        }
        document = application().document_add(SPDocument::createNewDocFromMem(std::string_view{
            "<svg xmlns='http://www.w3.org/2000/svg' width='210mm' height='297mm' viewBox='0 0 210 297'/>"}));
        ASSERT_TRUE(document);
        desktop = application().createDesktop(document, false, true);
        ASSERT_TRUE(desktop);
    }

    void TearDown() override
    {
        Inkscape::UI::Tools::set_clipboard_paste_hook_for_testing({});
        auto *prefs = Inkscape::Preferences::get();
        for (auto const &[path, value] : saved) {
            if (value) {
                prefs->setString(path, *value);
            } else {
                prefs->remove(path);
            }
        }
        // Close what SetUp opened so the application ends with no live desktop.
        if (document) {
            document->setModifiedSinceSave(false);
        }
        if (desktop) {
            application().destroyDesktop(desktop);
            desktop = nullptr;
        }
        if (document) {
            application().document_close(document);
            document = nullptr;
        }
        for (unsigned i = 0; i < 10000 && g_main_context_pending(nullptr); ++i) {
            g_main_context_iteration(nullptr, false);
        }
    }

    void set_style(Glib::ustring const &path, char const *text)
    {
        auto *css = style(text);
        Inkscape::Preferences::get()->setStyle(path, css);
        sp_repr_css_attr_unref(css);
    }

    SPDocument *document = nullptr;
    SPDesktop *desktop = nullptr;
    std::map<Glib::ustring, std::optional<Glib::ustring>> saved;
};

TEST_F(NewObjectVisibilityDesktop, InvisibleToolStylesDrawWithATenthMillimetreBlackStroke)
{
    auto *prefs = Inkscape::Preferences::get();
    // Rectangle tool with its own style; pen tool with the last path style.
    prefs->setString("/tools/shapes/rect/usecurrent", "0");
    set_style("/tools/shapes/rect/style", "fill:none;stroke:none");
    prefs->setString("/tools/freehand/pen/usecurrent", "path");
    set_style("/desktop/path/style", "fill:#ff000000;stroke:#0000ff;stroke-opacity:0");

    for (auto const *tool : {"/tools/shapes/rect", "/tools/freehand/pen"}) {
        SCOPED_TRACE(tool);
        auto *css = desktop->getNewObjectStyle(tool, false);
        ASSERT_TRUE(css);
        EXPECT_EQ(property(css, "stroke"), "#000000");
        EXPECT_EQ(property(css, "stroke-opacity"), "1");
        // Tool styles are in document pixels: 0.1 mm.
        EXPECT_NEAR(sp_repr_css_double_property(css, "stroke-width", 0), 0.1 * 96.0 / 25.4, 1e-9);
        EXPECT_FALSE(sp_css_attr_paints_nothing(css));
        sp_repr_css_attr_unref(css);

        // The plain lookup, used by tools that restyle existing objects (tweak,
        // paint bucket on an object) and by the style swatch, is unchanged.
        auto *raw = desktop->getCurrentOrToolStyle(tool, false);
        ASSERT_TRUE(raw);
        EXPECT_TRUE(sp_css_attr_paints_nothing(raw));
        EXPECT_NE(property(raw, "stroke"), "#000000");
        sp_repr_css_attr_unref(raw);
    }
    // The remembered tool style is not rewritten.
    EXPECT_EQ(prefs->getString("/tools/shapes/rect/style"), "fill:none;stroke:none");

    // A visible tool style is applied exactly as configured.
    set_style("/tools/shapes/rect/style", "fill:#00ff00;stroke:none");
    auto *css = desktop->getNewObjectStyle("/tools/shapes/rect", false);
    ASSERT_TRUE(css);
    EXPECT_EQ(property(css, "fill"), "#00ff00");
    EXPECT_EQ(property(css, "stroke"), "none");
    sp_repr_css_attr_unref(css);
}

TEST_F(NewObjectVisibilityDesktop, RectangleDrawnWithAnInvisibleStyleGetsTheStroke)
{
    auto *prefs = Inkscape::Preferences::get();
    prefs->setString("/tools/shapes/rect/usecurrent", "0");
    set_style("/tools/shapes/rect/style", "fill:none;stroke:none");
    desktop->setTool("/tools/shapes/rect");
    auto *tool = desktop->getTool();
    ASSERT_TRUE(tool);

    auto count_rects = [this] {
        unsigned rects = 0;
        for (auto &child : desktop->layerManager().currentLayer()->children) {
            rects += is<SPRect>(&child) ? 1 : 0;
        }
        return rects;
    };
    ASSERT_EQ(count_rects(), 0u);

    ButtonPressEvent press;
    press.button = 1;
    press.num_press = 1;
    press.pos = press.orig_pos = Geom::Point(100, 100);
    tool->root_handler(press);
    for (int step = 1; step <= 10; ++step) {
        MotionEvent motion;
        motion.modifiers = GDK_BUTTON1_MASK;
        motion.pos = Geom::Point(100 + 12 * step, 100 + 8 * step);
        tool->root_handler(motion);
    }
    ButtonReleaseEvent release;
    release.button = 1;
    release.pos = release.orig_pos = Geom::Point(220, 180);
    tool->root_handler(release);
    document->ensureUpToDate();

    SPRect *drawn = nullptr;
    for (auto &child : desktop->layerManager().currentLayer()->children) {
        if (auto *rect = cast<SPRect>(&child)) {
            drawn = rect;
        }
    }
    ASSERT_TRUE(drawn) << "the rectangle tool drew nothing";
    ASSERT_TRUE(drawn->style);
    EXPECT_TRUE(drawn->style->stroke.isColor());
    EXPECT_EQ(drawn->style->stroke.getColor().toRGBA(), 0x000000ffu);
    // The tool converted the stroke into the rectangle's units: one user unit
    // is one millimetre in this document.
    EXPECT_NEAR(drawn->style->stroke_width.computed, 0.1, 1e-6);
    EXPECT_TRUE(drawn->style->fill.isNone());
    desktop->setTool("/tools/select");
}

// Exercise the real flush, _finish and button-event continuation. The injected wait
// invalidates an item or destroys the pen tool exactly where a foreign clipboard
// owner's nested event loop can do so.
TEST_F(NewObjectVisibilityDesktop, ClipboardWaitInvalidationStopsPenEvent)
{
    using namespace Inkscape::UI::Tools;
    for (auto const shape : {CLIPBOARD, BEND_CLIPBOARD}) {
        for (bool switch_tool : {false, true}) {
            auto *prefs = Inkscape::Preferences::get();
            prefs->setInt("/tools/freehand/pen/shape", shape);
            prefs->setInt("/tools/freehand/pen/freehand-mode", 0);
            prefs->setInt("/tools/freehand/pen/simplify", 0);
            desktop->setTool("/tools/freehand/pen");
            auto *pen = dynamic_cast<PenTool *>(desktop->getTool());
            ASSERT_TRUE(pen);
            *pen->green_curve = Geom::parse_svg_path("M 10,10 L 40,40");
            pen->npoints = 2;
            unsigned calls = 0;
            set_clipboard_paste_hook_for_testing([&](SPDesktop *target) {
                ++calls;
                if (switch_tool) {
                    target->setTool("/tools/select");
                } else {
                    // white_item is the newly attached path, before selection->set().
                    pen->white_item->deleteObject();
                }
                return false;
            });
            ButtonPressEvent finish;
            finish.button = 3;
            finish.num_press = 1;
            finish.pos = finish.orig_pos = Geom::Point(100, 100);
            auto *tool = desktop->getTool();
            EXPECT_TRUE(tool->root_handler(finish));
            EXPECT_EQ(calls, 1u) << "no second LPE pass after invalidation";
            set_clipboard_paste_hook_for_testing({});
            if (!switch_tool) {
                // The invalidated event deliberately performed no further pen writes.
                EXPECT_EQ(pen->npoints, 2);
                pen->npoints = 0;
                desktop->setTool("/tools/select");
            }
            EXPECT_FALSE(Inkscape::DocumentUndo::undo(document));
        }
    }
}

TEST_F(NewObjectVisibilityDesktop, RevertRefusesHeldDocumentBeforeLoading)
{
    auto *directory = g_dir_make_tmp("stl-revert-XXXXXX", nullptr);
    ASSERT_TRUE(directory);
    auto path = std::string(directory) + "/saved.svg";
    ASSERT_TRUE(g_file_set_contents(path.c_str(),
        "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'/>", -1, nullptr));
    document->setDocumentFilename(path.c_str());
    auto hold = Inkscape::DocumentUndo::holdInteractionOperation(document);
    EXPECT_FALSE(application().document_revert(document));
    EXPECT_EQ(desktop->getDocument(), document);
    EXPECT_FALSE(Inkscape::DocumentUndo::interactionIsQuiescent(document));
    hold.reset();
    EXPECT_TRUE(Inkscape::DocumentUndo::interactionIsQuiescent(document));
    g_remove(path.c_str());
    g_rmdir(directory);
    g_free(directory);
}

} // namespace
