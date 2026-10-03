// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui/tools/offset-shapes-tool.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "preferences.h"
#include "selection.h"
#include "style.h"
#include "ui/toolbar/toolbars.h"
#include "ui/widget/events/canvas-event.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::UI::Tools;

namespace {

InkscapeApplication *initialize_gui()
{
    static auto *application = [] {
        g_setenv("INKSCAPE_APP_ID_TAG", "offsetshapestooltest", TRUE);
        return new InkscapeApplication();
    }();
    return application->gtk_app() ? application : nullptr;
}

KeyPressEvent key_press(unsigned keyval)
{
    KeyPressEvent event;
    event.keyval = keyval;
    GdkKeymapKey *keys = nullptr;
    int key_count = 0;
    if (gdk_display_map_keyval(gdk_display_get_default(), keyval, &keys, &key_count) && key_count > 0) {
        event.keycode = keys[0].keycode;
        event.group = keys[0].group;
    }
    g_free(keys);
    return event;
}

class OffsetShapesToolTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!initialize_gui()) {
            GTEST_SKIP() << "GTK display unavailable; Offset Shapes fixture skipped";
        }
        if (!Application::exists()) {
            Application::create(false);
        }
        auto *prefs = Preferences::get();
        prefs->setDouble("/tools/offsetshapes/distance", 5.0);
        prefs->setInt("/tools/offsetshapes/direction", 0);
        prefs->setInt("/tools/offsetshapes/corner", 0);
        prefs->setDouble("/tools/offsetshapes/miter_limit", 4.0);
        prefs->setBool("/tools/offsetshapes/outer_shapes_only", false);
        prefs->setBool("/tools/offsetshapes/select_results", true);
        prefs->setBool("/tools/offsetshapes/delete_originals", false);
        prefs->setBool("/tools/offsetshapes/simplify_results", false);

        document = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="300" height="200">
  <rect id="part1" x="20" y="30" width="40" height="20" style="fill:#ff0000;stroke:none"/>
  <rect id="part2" x="100" y="50" width="20" height="20" style="fill:#00ff00;stroke:none"/>
  <g id="group1"><rect id="nested1" x="150" y="30" width="20" height="20"/></g>
  <g id="group2"><rect id="nested2" x="180" y="30" width="20" height="20"/></g>
</svg>)svg");
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        ASSERT_TRUE(desktop);
        desktop->getSelection()->set(item("part1"));
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Initialize Offset Shapes fixture"},
                           "document-new");
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        document->setModifiedSinceSave(false);
    }

    void TearDown() override
    {
        if (desktop && desktop->getTool() && desktop->getTool()->getPrefsPath() == "/tools/offsetshapes") {
            desktop->setTool("/tools/select");
        }
        desktop.reset();
        document.reset();
    }

    SPItem *item(char const *id) const { return document ? cast<SPItem>(document->getObjectById(id)) : nullptr; }

    OffsetShapesTool *activate()
    {
        desktop->setTool("/tools/offsetshapes");
        return dynamic_cast<OffsetShapesTool *>(desktop->getTool());
    }

    std::size_t root_item_count() const { return document->getRoot()->item_list().size(); }

    bool selector_is_active() const
    {
        return desktop->getTool() && desktop->getTool()->getPrefsPath() == "/tools/select";
    }

    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
};

} // namespace

TEST_F(OffsetShapesToolTest, ActivationBuildsPreviewWithoutMutatingDocument)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();

    ASSERT_TRUE(tool);
    EXPECT_TRUE(tool->has_preview());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(OffsetShapesToolTest, EscapeCancelsWithoutDocumentOrUndoChanges)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);

    EXPECT_TRUE(tool->root_handler(key_press(GDK_KEY_Escape)));

    EXPECT_TRUE(selector_is_active());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(OffsetShapesToolTest, EnterCreatesOnePathAndOneUndoStep)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const before_count = root_item_count();
    auto *tool = activate();
    ASSERT_TRUE(tool);

    EXPECT_TRUE(tool->root_handler(key_press(GDK_KEY_Return)));

    EXPECT_TRUE(selector_is_active());
    document->ensureUpToDate();
    EXPECT_EQ(root_item_count(), before_count + 1);
    auto *result = desktop->getSelection()->singleItem();
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->documentGeometricBounds());
    EXPECT_NEAR(result->documentGeometricBounds()->left(), 15.0, 0.05);
    EXPECT_NEAR(result->documentGeometricBounds()->right(), 65.0, 0.05);
    ASSERT_TRUE(result->style->fill.isColor());
    EXPECT_EQ(result->style->fill.getColor().toRGBA(), 0xff0000ff);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(OffsetShapesToolTest, SelectResultsOffKeepsTheOriginalSelection)
{
    auto *original = item("part1");
    auto *tool = activate();
    ASSERT_TRUE(tool);
    auto options = tool->options();
    options.select_results = false;
    tool->set_options(options);

    ASSERT_TRUE(tool->apply());

    EXPECT_TRUE(selector_is_active());
    EXPECT_EQ(desktop->getSelection()->singleItem(), original);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(OffsetShapesToolTest, BothDirectionsAndDeleteOriginalsCommitAtomically)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const before_count = root_item_count();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    auto options = tool->options();
    options.direction = OffsetShapes::Direction::Both;
    options.delete_originals = true;
    tool->set_options(options);

    ASSERT_TRUE(tool->apply());

    EXPECT_TRUE(selector_is_active());
    document->ensureUpToDate();
    EXPECT_EQ(item("part1"), nullptr);
    EXPECT_EQ(root_item_count(), before_count + 1); // one source replaced by two offsets
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(OffsetShapesToolTest, OuterOnlyRejectsSourcesWithDifferentParentsAtomically)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    desktop->getSelection()->setList(std::vector<SPItem *>{item("nested1"), item("nested2")});
    auto *tool = activate();
    ASSERT_TRUE(tool);
    auto options = tool->options();
    options.outer_shapes_only = true;
    tool->set_options(options);

    EXPECT_FALSE(tool->apply());

    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(desktop->getTool(), tool);
}

TEST_F(OffsetShapesToolTest, SelectionChangeCancelsFrozenPreview)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    ASSERT_TRUE(activate());

    desktop->getSelection()->set(item("part2"));

    EXPECT_TRUE(selector_is_active());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(OffsetShapesToolTest, FailedCommitLeavesTheToolConnectedToSelectionChanges)
{
    // commit() arms the tool's _committing guard (and disconnects the selection) only right before
    // the first document mutation. A refusal on the outer-only different-parents rule happens before
    // that, so the tool must stay connected: a later selection change still cancels.
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    desktop->getSelection()->setList(std::vector<SPItem *>{item("nested1"), item("nested2")});
    auto *tool = activate();
    ASSERT_TRUE(tool);
    auto options = tool->options();
    options.outer_shapes_only = true;
    tool->set_options(options);

    EXPECT_FALSE(tool->apply());
    EXPECT_EQ(desktop->getTool(), tool); // no cancel yet: the refusal changed nothing
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);

    desktop->getSelection()->set(item("part2")); // the late change must still cancel

    EXPECT_TRUE(selector_is_active());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(OffsetShapesToolTest, ContextToolbarIsAvailable)
{
    auto *tool = activate();
    ASSERT_TRUE(tool);
    UI::Toolbar::Toolbars toolbars;

    EXPECT_NO_THROW(toolbars.setTool(tool));
    EXPECT_NE(toolbars.get_current_toolbar(), nullptr);
    EXPECT_NO_THROW(toolbars.setTool(nullptr));
}
