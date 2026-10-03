// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui/tools/nesting-tool.h"
#include "ui/tools/nesting-preview.h"
#include "nesting/sparrow-adapter.h"

#include <chrono>
#include <cmath>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <2geom/transforms.h>
#include <glibmm/main.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>
#include <gtest/gtest.h>

#include "desktop.h"
#include "display/control/canvas-item-drawing.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape.h"
#include "message-stack.h"
#include "object/sp-defs.h"
#include "object/sp-item.h"
#include "object/sp-namedview.h"
#include "preferences.h"
#include "util/scope_exit.h"
#include "selection.h"
#include "nesting/nesting-settings.h"
#include "ui/toolbar/nesting-toolbar.h"
#include "ui/toolbar/toolbars.h"
#include "ui/widget/canvas.h"
#include "ui/widget/events/canvas-event.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::UI::Tools;

namespace {

InkscapeApplication *initialize_gui()
{
    static auto *application = [] {
        g_setenv("INKSCAPE_APP_ID_TAG", "nestingtooltest", TRUE);
        return new InkscapeApplication(); // Process-lifetime test fixture.
    }();
    return application->gtk_app() ? application : nullptr;
}

void drain_main_context()
{
    auto context = Glib::MainContext::get_default();
    while (context->iteration(false)) {
    }
}

bool wait_until(std::function<bool()> const &predicate, std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    do {
        drain_main_context();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    drain_main_context();
    return predicate();
}

// The platform's primary modifier for sheet picking (tool work order 3.1).
#ifdef __APPLE__
constexpr unsigned TARGET_MASK = GDK_META_MASK;
constexpr unsigned TARGET_KEY = GDK_KEY_Meta_L;
#else
constexpr unsigned TARGET_MASK = GDK_CONTROL_MASK;
constexpr unsigned TARGET_KEY = GDK_KEY_Control_L;
#endif

ButtonPressEvent target_click()
{
    ButtonPressEvent click;
    click.button = 1;
    click.modifiers = TARGET_MASK;
    return click;
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

KeyReleaseEvent key_release(unsigned keyval, unsigned modifiers)
{
    KeyReleaseEvent event;
    event.keyval = keyval;
    event.modifiers = modifiers;
    return event;
}

class NestingToolTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!initialize_gui()) {
            GTEST_SKIP() << "GTK display unavailable; nesting tool fixture skipped";
        }
        if (!Application::exists()) {
            Application::create(false);
        }

        document = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="300" height="200">
  <rect id="container" x="0" y="0" width="60" height="30"/>
  <rect id="part1" x="100" y="80" width="10" height="10"/>
  <rect id="part2" x="140" y="100" width="10" height="10"/>
  <g id="invalid-container"><rect width="30" height="30"/></g>
</svg>)svg");
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        ASSERT_TRUE(desktop);
        Preferences::get()->setInt("/tools/nesting/time_limit_ms", 100);
        // Axis-aligned placements keep the bounding-box overlap oracles exact
        // (bounds of slightly rotated rectangles overlap when shapes do not).
        Preferences::get()->setInt("/tools/nesting/rotation_mode", 0);
        ASSERT_TRUE(item("container"));
        ASSERT_TRUE(item("part1"));
        ASSERT_TRUE(item("part2"));
        desktop->getSelection()->setList(std::vector<SPItem *>{item("part1"), item("part2")});
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Initialize nesting tool fixture"},
                           "document-new");
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        document->setModifiedSinceSave(false);
    }

    void TearDown() override
    {
        if (desktop && desktop->getTool() && desktop->getTool()->getPrefsPath() == "/tools/nesting") {
            desktop->setTool("/tools/select");
        }
        desktop.reset();
        document.reset();
        drain_main_context();
    }

    SPItem *item(char const *id) const { return document ? cast<SPItem>(document->getObjectById(id)) : nullptr; }

    NestingTool *activate()
    {
        desktop->setTool("/tools/nesting");
        return dynamic_cast<NestingTool *>(desktop->getTool());
    }

    bool selector_is_active() const
    {
        return desktop->getTool() && desktop->getTool()->getPrefsPath() == "/tools/select";
    }

    void load_circle_fixture()
    {
        desktop.reset();
        auto dir = std::filesystem::path(__FILE__).parent_path().parent_path() / "nesting/sparrow/circles80";
        document = SPDocument::createNewDoc((dir / "input.svg").string().c_str());
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        std::ifstream ids(dir / "ids.txt");
        std::string id;
        std::vector<SPItem *> parts;
        while (ids >> id) parts.push_back(item(id.c_str()));
        ASSERT_EQ(parts.size(), 80);
        circle_parts = parts;
        desktop->getSelection()->setList(parts);
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Initialize circle fixture"}, "");
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        document->setModifiedSinceSave(false);
        Preferences::get()->setInt("/tools/nesting/time_limit_ms", 15000);
    }

    bool nesting_is_active(NestingTool *tool) const { return desktop->getTool() == tool; }

    /// Wait for a run to end; the tool stays active afterwards (T1).
    bool wait_for_run(NestingTool *tool, std::chrono::milliseconds timeout = std::chrono::seconds(10))
    {
        return wait_until([&] { return !nesting_is_active(tool) || !tool->is_solving(); }, timeout) &&
               nesting_is_active(tool);
    }

    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    std::vector<SPItem *> circle_parts;
};

} // namespace

// Also the tool work order's TargetModifierClickNestsIntoSheetAndStaysInTool.
TEST_F(NestingToolTest, ContainerClickSolvesOffThreadCommitsOnceAndStaysInNestingTool)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    auto const ready_cursor = desktop->getCanvas()->get_cursor();
    ASSERT_TRUE(ready_cursor);
    auto const click = target_click();

    EXPECT_TRUE(tool->item_handler(item("container"), click));
    EXPECT_TRUE(tool->is_solving());
    auto const busy_cursor = desktop->getCanvas()->get_cursor();
    ASSERT_TRUE(busy_cursor);
    EXPECT_NE(busy_cursor, ready_cursor);
    EXPECT_FALSE(desktop->waiting_cursor); // Not the generic OS wait cursor.
    ASSERT_TRUE(wait_for_run(tool));
    EXPECT_NE(desktop->getCanvas()->get_cursor(), busy_cursor);
    EXPECT_EQ(tool->last_sheet(), item("container"));
    EXPECT_TRUE(tool->has_sheet_outline());

    document->ensureUpToDate();
    for (auto const *id : {"part1", "part2"}) {
        auto const bounds = item(id)->documentVisualBounds();
        ASSERT_TRUE(bounds);
        EXPECT_GE(bounds->left(), -0.001);
        EXPECT_GE(bounds->top(), -0.001);
        EXPECT_LE(bounds->right(), 60.001);
        EXPECT_LE(bounds->bottom(), 30.001);
    }
    auto const after = sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_NE(after, before);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, EscapeBeforeContainerClickCancelsWithoutDocumentChanges)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    auto const ready_cursor = desktop->getCanvas()->get_cursor();
    auto const escape = key_press(GDK_KEY_Escape);
    (void)ready_cursor;

    EXPECT_TRUE(tool->root_handler(escape));

    // Idle Esc is the Select tool's: it deselects and the tool stays.
    EXPECT_TRUE(nesting_is_active(tool));
    EXPECT_TRUE(desktop->getSelection()->isEmpty());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, SelectionChangeWhileIdleDoesNotCancel)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);

    desktop->getSelection()->set(item("part1"));

    EXPECT_TRUE(nesting_is_active(tool));
    EXPECT_FALSE(tool->is_solving());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, UnsupportedContainerKeepsToolActiveAndDoesNotMutateDocument)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    auto const click = target_click();

    EXPECT_TRUE(tool->item_handler(item("invalid-container"), click));

    EXPECT_EQ(desktop->getTool(), tool);
    EXPECT_FALSE(tool->is_solving());
    EXPECT_FALSE(desktop->waiting_cursor);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, RepeatedPrimaryPressIsConsumedWithoutStartingASecondOperation)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    auto repeated_press = target_click();
    repeated_press.num_press = 2;

    EXPECT_TRUE(tool->item_handler(item("container"), repeated_press));

    EXPECT_EQ(desktop->getTool(), tool);
    EXPECT_FALSE(tool->is_solving());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, ToolSwitchWhileResultIsPendingSuppressesLateCommit)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    auto const busy_cursor = desktop->getCanvas()->get_cursor();

    desktop->setTool("/tools/select");
    drain_main_context();

    EXPECT_TRUE(selector_is_active());
    EXPECT_NE(desktop->getCanvas()->get_cursor(), busy_cursor);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, UnlimitedRunContinuesUntilEscapeAndCancelsWithoutMutation)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    Preferences::get()->setInt("/tools/nesting/time_limit_ms", 0);
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(tool->is_solving());

    EXPECT_TRUE(tool->root_handler(key_press(GDK_KEY_Escape)));
    ASSERT_TRUE(wait_for_run(tool));

    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, SelectionChangeWhileSolvingCancels)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));

    desktop->getSelection()->set(item("container"));
    drain_main_context();

    EXPECT_TRUE(nesting_is_active(tool));
    EXPECT_FALSE(tool->is_solving());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, NestingToolShowsItsToolbar)
{
    auto *tool = activate();
    ASSERT_TRUE(tool);
    UI::Toolbar::Toolbars toolbars;

    toolbars.setTool(tool);
    auto *toolbar = dynamic_cast<UI::Toolbar::NestingToolbar *>(toolbars.get_current_toolbar());
    ASSERT_TRUE(toolbar);
    // Nest needs a remembered sheet.
    EXPECT_FALSE(toolbar->nest_button_for_testing().get_sensitive());
    ASSERT_TRUE(tool->nest_into(item("container")));
    EXPECT_FALSE(toolbar->nest_button_for_testing().get_sensitive()); // running
    ASSERT_TRUE(wait_for_run(tool));
    EXPECT_TRUE(toolbar->nest_button_for_testing().get_sensitive());
    EXPECT_NE(toolbar->sheet_label_for_testing().get_text().raw().find("container"), std::string::npos);
    EXPECT_NO_THROW(toolbars.setTool(nullptr));
}

TEST_F(NestingToolTest, NestingToolbarControlsShareThePreferencePaths)
{
    auto *prefs = Preferences::get();
    prefs->setDoubleUnit(Nesting::PART_SPACING_PREF_PATH, 1.0, "mm");
    auto *tool = activate();
    ASSERT_TRUE(tool);
    UI::Toolbar::Toolbars toolbars;
    toolbars.setTool(tool);
    auto *toolbar = dynamic_cast<UI::Toolbar::NestingToolbar *>(toolbars.get_current_toolbar());
    ASSERT_TRUE(toolbar);
    auto &spacing = toolbar->spacing_for_testing();
    EXPECT_NEAR(spacing.get_value(), 1.0, 1e-9);

    // Toolbar -> preference read by the tool (readLengthPreferencePx, as read_options).
    spacing.set_value(2.5);
    EXPECT_NEAR(Nesting::readLengthPreferencePx(*prefs, Nesting::PART_SPACING_PREF_PATH), 2.5 * 96.0 / 25.4, 1e-6);
    EXPECT_EQ(prefs->getUnit(Nesting::PART_SPACING_PREF_PATH), "mm");

    // Preference (as the Preferences page writes it) -> toolbar.
    prefs->setDoubleUnit(Nesting::PART_SPACING_PREF_PATH, 4.0, "mm");
    EXPECT_NEAR(spacing.get_value(), 4.0, 1e-9);

    // The time preset and the solver's time limit stay in step.
    prefs->setInt(Nesting::TIME_PRESET_PREF_PATH, static_cast<int>(Nesting::OptimizationTimePreset::Quick));
    Nesting::syncOptimizationTimeLimit(*prefs);
    EXPECT_EQ(prefs->getInt(Nesting::TIME_LIMIT_PREF_PATH), 1000);

    prefs->setDoubleUnit(Nesting::PART_SPACING_PREF_PATH, 0.0, "mm");
    prefs->setInt(Nesting::TIME_PRESET_PREF_PATH, static_cast<int>(Nesting::OptimizationTimePreset::Balanced));
    prefs->setInt(Nesting::TIME_LIMIT_PREF_PATH, 100);
    toolbars.setTool(nullptr);
}

TEST(NestingToolShortcut, ShiftNActivatesNestingTool)
{
    auto const path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "share/keys/inkscape.xml";
    std::ifstream file(path);
    ASSERT_TRUE(file) << path;
    std::string const keymap{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto const nesting = keymap.find("win.tool-switch('Nesting')");
    ASSERT_NE(nesting, std::string::npos);
    auto const line_end = keymap.find('\n', nesting);
    auto const line = keymap.substr(nesting, line_end - nesting);
    EXPECT_NE(line.find("keys=\"&lt;shift&gt;n\""), std::string::npos) << line;
    // Bound once, and no other default binding uses Shift+N.
    EXPECT_EQ(keymap.find("win.tool-switch('Nesting')", nesting + 1), std::string::npos);
    std::size_t uses = 0;
    for (auto const *keys : {"\"&lt;shift&gt;n\"", "\"&lt;shift&gt;N\"", "\"N\""}) {
        for (auto at = keymap.find(keys); at != std::string::npos; at = keymap.find(keys, at + 1)) {
            ++uses;
        }
    }
    EXPECT_EQ(uses, 1u);
    // The old binding collided with the Templates dialog on Windows and Linux.
    EXPECT_EQ(line.find("&lt;alt&gt;"), std::string::npos);
}

TEST_F(NestingToolTest, EscapeDuringSparrowRunLeavesDocumentAndUndoUntouched)
{
    if (!Nesting::sparrowAvailable()) GTEST_SKIP() << "Pinned helper unavailable on this platform";
    load_circle_fixture();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    auto const busy_cursor = desktop->getCanvas()->get_cursor();
    auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(wait_until([&] { return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(300); }));
    ASSERT_EQ(desktop->getTool(), tool);
    ASSERT_TRUE(tool->is_solving());
    EXPECT_EQ(desktop->getCanvas()->get_cursor(), busy_cursor); // Static, not animated.
    auto cancelled = std::chrono::steady_clock::now();
    EXPECT_TRUE(tool->root_handler(key_press(GDK_KEY_Escape)));
    ASSERT_TRUE(wait_for_run(tool));
    EXPECT_NE(desktop->getCanvas()->get_cursor(), busy_cursor);
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - cancelled).count(), 3);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, DocumentChangeDuringSparrowRunRejectsLateResult)
{
    if (!Nesting::sparrowAvailable()) GTEST_SKIP() << "Pinned helper unavailable on this platform";
    load_circle_fixture();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(wait_until([&] { return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(300); }));
    item("container")->getRepr()->setAttribute("width", "950");
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Change container"}, "");
    DocumentUndo::clearUndo(document.get());
    auto changed = sp_repr_save_buf(document->getReprDoc()).raw();
    ASSERT_TRUE(wait_for_run(tool, std::chrono::seconds(45)));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), changed);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, SparrowCirclesCommitAsSingleUndoAndRedo)
{
    if (!Nesting::sparrowAvailable()) GTEST_SKIP() << "Pinned helper unavailable on this platform";
    load_circle_fixture();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool, std::chrono::seconds(45)));
    document->ensureUpToDate();
    auto sheet = item("container")->documentGeometricBounds();
    ASSERT_TRUE(sheet);
    // The selection now holds only leftovers (T3); count the parts themselves.
    EXPECT_TRUE(desktop->getSelection()->isEmpty());
    unsigned placed = 0;
    for (auto selected : circle_parts) {
        auto b = selected->documentGeometricBounds();
        if (b && sheet->contains(*b)) ++placed;
    }
    EXPECT_EQ(placed, 80);
    auto const after = sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_NE(after, before);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after);
    EXPECT_FALSE(DocumentUndo::redo(document.get()));
}

// --- R3 (M7): incremental preview model, headless -----------------------------

namespace {

Nesting::PreparedDocumentNesting preview_snapshot(std::size_t parts, std::size_t points_per_part)
{
    Nesting::PreparedDocumentNesting snapshot;
    for (std::size_t i = 0; i < parts; ++i) {
        Nesting::CollisionComponent component;
        for (std::size_t k = 0; k < points_per_part; ++k) {
            double const a = 2.0 * M_PI * k / points_per_part;
            component.outer.push_back({10.0 * i + 5.0 + 4.0 * std::cos(a), 5.0 + 3.0 * std::sin(a)});
        }
        Nesting::PreparedPart part;
        part.id = i + 1;
        part.components.push_back(std::move(component));
        snapshot.parts.push_back(std::move(part));
    }
    return snapshot;
}

std::vector<Nesting::Placement> placements_for(std::size_t parts)
{
    std::vector<Nesting::Placement> placements;
    for (std::size_t i = 0; i < parts; ++i) {
        Nesting::Placement placement;
        placement.part_id = i + 1;
        placement.placed = true;
        placement.translation_x = 1.0 + i;
        placement.translation_y = 2.0;
        placement.rotation_degrees = 0.0;
        placements.push_back(placement);
    }
    return placements;
}

} // namespace

TEST(NestingPreviewModelTest, SamePlacementsTwiceProduceNoChanges)
{
    auto const snapshot = preview_snapshot(3, 8);
    UI::Tools::NestingPreviewModel model(snapshot);
    auto const placements = placements_for(3);
    EXPECT_EQ(model.update(placements).size(), 3u);
    EXPECT_TRUE(model.update(placements).empty());
    EXPECT_EQ(model.rebuilt_paths(), 3u);
}

TEST(NestingPreviewModelTest, OneMovedPartProducesOneChange)
{
    auto const snapshot = preview_snapshot(3, 8);
    UI::Tools::NestingPreviewModel model(snapshot);
    auto placements = placements_for(3);
    (void)model.update(placements);
    placements[1].translation_x += 0.5;
    auto const changes = model.update(placements);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].index, 1u);
    EXPECT_TRUE(changes[0].visible);
}

TEST(NestingPreviewModelTest, UnplacedPartIsHiddenOnce)
{
    auto const snapshot = preview_snapshot(3, 8);
    UI::Tools::NestingPreviewModel model(snapshot);
    auto placements = placements_for(3);
    (void)model.update(placements);
    placements[2].placed = false;
    auto const changes = model.update(placements);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].index, 2u);
    EXPECT_FALSE(changes[0].visible);
    EXPECT_TRUE(changes[0].path.empty());
    EXPECT_TRUE(model.update(placements).empty());
}

TEST(NestingPreviewModelTest, AboveThePointBudgetPartsUseTheirBounds)
{
    // 3 parts x 7000 points exceeds the 20000-point budget.
    auto const snapshot = preview_snapshot(3, 7000);
    UI::Tools::NestingPreviewModel model(snapshot);
    EXPECT_TRUE(model.uses_bounds());
    auto const changes = model.update(placements_for(3));
    ASSERT_EQ(changes.size(), 3u);
    ASSERT_EQ(changes[0].path.size(), 1u);
    EXPECT_EQ(changes[0].path.front().size_closed(), 4u);
    // The bounds contain the transformed outline.
    auto const bounds = changes[0].path.boundsExact();
    ASSERT_TRUE(bounds);
    for (auto const &point : snapshot.parts[0].components[0].outer) {
        EXPECT_TRUE(bounds->contains(Geom::Point(point.x + 1.0, point.y + 2.0)));
    }
    UI::Tools::NestingPreviewModel small(preview_snapshot(3, 8));
    EXPECT_FALSE(small.uses_bounds());
}

TEST(NestingPreviewModelTest, PathIsTheOutlineRotatedThenTranslated)
{
    auto const snapshot = preview_snapshot(1, 8);
    UI::Tools::NestingPreviewModel model(snapshot);
    auto placements = placements_for(1);
    placements[0].rotation_degrees = 90.0;
    placements[0].translation_x = 30.0;
    placements[0].translation_y = -7.0;
    auto const changes = model.update(placements);
    ASSERT_EQ(changes.size(), 1u);
    auto const &path = changes[0].path;
    ASSERT_EQ(path.size(), 1u);
    auto const &outer = snapshot.parts[0].components[0].outer;
    auto const transform = Geom::Rotate::from_degrees(90.0) * Geom::Translate(30.0, -7.0);
    ASSERT_EQ(path.front().size_closed(), outer.size());
    for (std::size_t k = 0; k < outer.size(); ++k) {
        auto const expected = Geom::Point(outer[k].x, outer[k].y) * transform;
        auto const actual = path.front().pointAt(static_cast<double>(k));
        EXPECT_NEAR(actual.x(), expected.x(), 1e-9);
        EXPECT_NEAR(actual.y(), expected.y(), 1e-9);
    }
}

TEST(NestingPreviewModelTest, ThrottledLayoutIsCarriedWithItsScoreAndCount)
{
    Nesting::Progress layout;
    layout.stage = 1;
    layout.best_score = 50.0;
    layout.placed_count = 7;
    layout.placements = placements_for(2);
    std::optional<Nesting::Progress> pending = layout;

    // A later heartbeat without a layout takes the layout, score and count.
    Nesting::Progress heartbeat;
    heartbeat.stage = 1;
    heartbeat.best_score = 60.0;
    heartbeat.placed_count = 9;
    UI::Tools::carry_pending_layout(heartbeat, pending);
    EXPECT_EQ(heartbeat.placements.size(), 2u);
    EXPECT_EQ(heartbeat.best_score, 50.0);
    EXPECT_EQ(heartbeat.placed_count, 7u);
    EXPECT_FALSE(pending);

    // A report with its own layout is newer and wins over the pending one.
    pending = layout;
    Nesting::Progress improved;
    improved.stage = 1;
    improved.best_score = 70.0;
    improved.placed_count = 8;
    improved.placements = placements_for(1);
    UI::Tools::carry_pending_layout(improved, pending);
    EXPECT_EQ(improved.placements.size(), 1u);
    EXPECT_EQ(improved.best_score, 70.0);
    EXPECT_FALSE(pending);

    // Nothing pending: the report is unchanged.
    Nesting::Progress plain;
    plain.best_score = 1.0;
    UI::Tools::carry_pending_layout(plain, pending);
    EXPECT_TRUE(plain.placements.empty());
    EXPECT_EQ(plain.best_score, 1.0);
}

TEST_F(NestingToolTest, PreviewUsesDesktopCoordinates)
{
    // A 15 s run on 80 parts: the preview appears long before the run ends.
    load_circle_fixture();
    std::vector<SPItem *> parts;
    for (auto *part : desktop->getSelection()->items()) {
        parts.push_back(part);
    }
    ASSERT_EQ(parts.size(), 80u);
    // C4: with the y axis pointing up, document and desktop coordinates differ.
    document->getNamedView()->set_y_axis_down(false);
    document->ensureUpToDate();
    ASSERT_LT(desktop->doc2dt()[3], 0.0);
    // Changing the axis orientation resets the selection; select the parts again.
    desktop->getSelection()->setList(parts);
    ASSERT_EQ(desktop->getSelection()->size(), 80u);
    auto *container = item("container");
    ASSERT_TRUE(container);
    auto const sheet = container->documentVisualBounds();
    ASSERT_TRUE(sheet);

    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(container));
    // Guard every read: a run that ends returns to the selector and destroys the tool.
    ASSERT_TRUE(wait_until([&] { return !selector_is_active() && tool->preview_document_bounds().has_value(); },
                           std::chrono::seconds(10)));

    auto const document_bounds = *tool->preview_document_bounds();
    auto const desktop_bounds = tool->preview_desktop_bounds();
    ASSERT_TRUE(desktop_bounds);
    auto const expected = document_bounds * desktop->doc2dt();
    EXPECT_NEAR(desktop_bounds->left(), expected.left(), 1e-6);
    EXPECT_NEAR(desktop_bounds->right(), expected.right(), 1e-6);
    EXPECT_NEAR(desktop_bounds->top(), expected.top(), 1e-6);
    EXPECT_NEAR(desktop_bounds->bottom(), expected.bottom(), 1e-6);
    EXPECT_GT(std::abs(desktop_bounds->top() - document_bounds.top()), 1.0) << "the y axis flip must be applied";
    // Independently of the mapping: the preview shows placed parts on the sheet.
    auto grown = *sheet;
    grown.expandBy(1e-3);
    EXPECT_TRUE(grown.contains(document_bounds));

    ASSERT_FALSE(selector_is_active());
    EXPECT_TRUE(tool->root_handler(key_press(GDK_KEY_Escape)));
    ASSERT_TRUE(wait_for_run(tool));
}

// --- M11: persistent Select-derived tool (tool work order 3-4, 9.1) -----------

TEST_F(NestingToolTest, PlainClickSelectsPartsAndDoesNotNest)
{
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    desktop->getCanvas()->size_allocate(Gtk::Allocation(0, 0, 400, 300), -1);
    auto *tool = activate();
    ASSERT_TRUE(tool);
    desktop->getSelection()->clear();
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    auto const bounds = item("part1")->desktopVisualBounds();
    ASSERT_TRUE(bounds);
    auto const pos = bounds->midpoint(); // as the selector tests probe

    ButtonPressEvent press;
    press.button = 1;
    press.pos = press.orig_pos = pos;
    tool->root_handler(press);
    ButtonReleaseEvent release;
    release.button = 1;
    release.pos = release.orig_pos = pos;
    tool->root_handler(release);

    EXPECT_FALSE(tool->is_solving());
    EXPECT_TRUE(nesting_is_active(tool));
    EXPECT_TRUE(desktop->getSelection()->includes(item("part1")));
    EXPECT_EQ(desktop->getSelection()->size(), 1u);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
}

TEST_F(NestingToolTest, ModifierChangesCursorWithoutMotion)
{
    auto *tool = activate();
    ASSERT_TRUE(tool);
    EXPECT_EQ(tool->cursor_filename(), "cursor-nesting.svg");

    tool->root_handler(key_press(TARGET_KEY));
    // Over empty canvas or a valid sheet the target cursor shows; no motion event.
    EXPECT_EQ(tool->cursor_filename(), "cursor-nesting-target.svg");

    tool->root_handler(key_release(TARGET_KEY, TARGET_MASK));
    EXPECT_EQ(tool->cursor_filename(), "cursor-nesting.svg");
    EXPECT_FALSE(tool->has_hover_outline());
}

TEST_F(NestingToolTest, InvalidSheetShowsInvalidCursorAndReason)
{
    auto *doc = document.get();
    auto *defs = doc->getDefs()->getRepr();
    auto *clip = doc->getReprDoc()->createElement("svg:clipPath");
    clip->setAttribute("id", "clip");
    auto *clip_rect = doc->getReprDoc()->createElement("svg:rect");
    clip_rect->setAttribute("x", "200");
    clip_rect->setAttribute("width", "10");
    clip_rect->setAttribute("height", "10");
    clip->appendChild(clip_rect);
    defs->appendChild(clip);
    auto *filter = doc->getReprDoc()->createElement("svg:filter");
    filter->setAttribute("id", "blur");
    auto *blur = doc->getReprDoc()->createElement("svg:feGaussianBlur");
    blur->setAttribute("stdDeviation", "1");
    filter->appendChild(blur);
    defs->appendChild(filter);
    for (auto *node : {clip_rect, clip, blur, filter}) {
        Inkscape::GC::release(node);
    }
    auto *root = doc->getReprRoot();
    auto add_rect = [&](char const *id, char const *attribute, char const *value) {
        auto *rect = doc->getReprDoc()->createElement("svg:rect");
        rect->setAttribute("id", id);
        rect->setAttribute("x", "200");
        rect->setAttribute("width", "40");
        rect->setAttribute("height", "40");
        rect->setAttribute(attribute, value);
        root->appendChild(rect);
        Inkscape::GC::release(rect);
    };
    add_rect("clipped", "clip-path", "url(#clip)");
    add_rect("filtered", "style", "filter:url(#blur)");
    document->ensureUpToDate();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();

    auto *tool = activate();
    ASSERT_TRUE(tool);
    for (auto const *id : {"invalid-container", "part1", "clipped", "filtered"}) {
        MotionEvent motion;
        motion.modifiers = TARGET_MASK;
        EXPECT_TRUE(tool->item_handler(item(id), motion)) << id;
        EXPECT_EQ(tool->cursor_filename(), "cursor-nesting-invalid.svg") << id;
        EXPECT_TRUE(tool->has_hover_outline()) << id;

        EXPECT_TRUE(tool->item_handler(item(id), target_click())) << id;
        EXPECT_FALSE(tool->is_solving()) << id;
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before) << id;
    }
    // The reasons are sheetIneligibility's, except the tool's own "selected part" rule.
    for (auto const *id : {"invalid-container", "clipped", "filtered"}) {
        EXPECT_TRUE(Nesting::sheetIneligibility(item(id))) << id;
    }
    EXPECT_FALSE(Nesting::sheetIneligibility(item("part1")));

    MotionEvent valid;
    valid.modifiers = TARGET_MASK;
    tool->item_handler(item("container"), valid);
    EXPECT_EQ(tool->cursor_filename(), "cursor-nesting-target.svg");
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, EnterWithoutSheetOrSelectionOnlyInforms)
{
    auto *tool = activate();
    ASSERT_TRUE(tool);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();

    // No last sheet yet.
    EXPECT_TRUE(tool->root_handler(key_press(GDK_KEY_Return)));
    EXPECT_FALSE(tool->is_solving());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);

    // A last sheet with nothing tracked on it and an empty selection: the
    // wide part does not fit, so the first run tracks nothing.
    auto *wide = item("part1");
    wide->getRepr()->setAttribute("width", "100");
    document->ensureUpToDate();
    desktop->getSelection()->set(wide);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    ASSERT_EQ(tool->last_sheet(), item("container"));
    EXPECT_TRUE(tool->memory().tracked.empty());
    desktop->getSelection()->clear();
    auto const after_run = sp_repr_save_buf(document->getReprDoc()).raw();

    EXPECT_TRUE(tool->root_handler(key_press(GDK_KEY_Return)));
    EXPECT_FALSE(tool->is_solving());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after_run);
}

TEST_F(NestingToolTest, LeftoversMoveBesideSheetInOneUndoStep)
{
    // Two parts too wide for the 60 x 30 sheet; one that fits.
    item("part1")->getRepr()->setAttribute("width", "100");
    item("part2")->getRepr()->setAttribute("width", "90");
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Widen parts"}, "");
    DocumentUndo::clearUndo(document.get());
    auto *small = cast<SPItem>(document->getObjectById("invalid-container")->firstChild()); // 30 x 30 rect
    ASSERT_TRUE(small);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    desktop->getSelection()->setList(std::vector<SPItem *>{item("part1"), item("part2")});
    Preferences::get()->setDouble("/tools/nesting/part_spacing", 0.0);

    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    document->ensureUpToDate();

    // Right of the sheet, top-aligned, gap = max(spacing, 5 mm = 18.9 px), in
    // selection order, a new column when the next part would pass the bottom.
    auto const first = item("part1")->documentVisualBounds();
    auto const second = item("part2")->documentVisualBounds();
    ASSERT_TRUE(first && second);
    EXPECT_NEAR(first->left(), 60 + 18.9, 1e-6);
    EXPECT_NEAR(first->top(), 0, 1e-6);
    EXPECT_NEAR(second->left(), first->right() + 18.9, 1e-6);
    EXPECT_NEAR(second->top(), 0, 1e-6);

    auto const after = sp_repr_save_buf(document->getReprDoc()).raw();
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after);
    Preferences::get()->setDouble("/tools/nesting/part_spacing", 0.0);
}

TEST_F(NestingToolTest, LeftoversAreSelectedAfterTheRun)
{
    item("part2")->getRepr()->setAttribute("width", "100");
    document->ensureUpToDate();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    ASSERT_EQ(desktop->getSelection()->size(), 1u);
    EXPECT_EQ(desktop->getSelection()->singleItem(), item("part2"));

    // Everything fits: the selection is empty afterwards.
    item("part2")->getRepr()->setAttribute("width", "10");
    document->ensureUpToDate();
    desktop->getSelection()->setList(std::vector<SPItem *>{item("part1"), item("part2")});
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    EXPECT_TRUE(desktop->getSelection()->isEmpty());
}

TEST_F(NestingToolTest, ToolSwitchForgetsLastSheetAndRemovesOutlines)
{
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    ASSERT_TRUE(tool->has_sheet_outline());

    // switching_away drops the outlines and the memory before the tool (and
    // with it every canvas item it owns) is destroyed.
    tool->switching_away("/tools/select");
    EXPECT_FALSE(tool->has_sheet_outline());
    EXPECT_FALSE(tool->has_hover_outline());
    EXPECT_EQ(tool->last_sheet(), nullptr);
    desktop->setTool("/tools/select");
    drain_main_context();

    // A new nesting tool starts without a remembered sheet.
    auto *fresh = activate();
    ASSERT_TRUE(fresh);
    EXPECT_EQ(fresh->last_sheet(), nullptr);
    EXPECT_TRUE(fresh->memory().tracked.empty());
    EXPECT_TRUE(fresh->memory().staged.empty());
}

TEST_F(NestingToolTest, ShiftEnterReNestsTrackedPartsWithSelection)
{
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    ASSERT_EQ(tool->memory().tracked.size(), 2u);
    DocumentUndo::clearUndo(document.get());

    // Two new parts, then Shift+Enter: all four are nested together.
    auto *root = document->getReprRoot();
    for (auto const *id : {"part3", "part4"}) {
        auto *rect = document->getReprDoc()->createElement("svg:rect");
        rect->setAttribute("id", id);
        rect->setAttribute("x", "200");
        rect->setAttribute("y", "100");
        rect->setAttribute("width", "12");
        rect->setAttribute("height", "12");
        root->appendChild(rect);
        Inkscape::GC::release(rect);
    }
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Add parts"}, "");
    DocumentUndo::clearUndo(document.get());
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    desktop->getSelection()->setList(std::vector<SPItem *>{item("part3"), item("part4")});

    auto shift_enter = key_press(GDK_KEY_Return);
    shift_enter.modifiers = GDK_SHIFT_MASK;
    EXPECT_TRUE(tool->root_handler(shift_enter));
    ASSERT_TRUE(tool->is_solving());
    ASSERT_TRUE(wait_for_run(tool));
    document->ensureUpToDate();

    auto const sheet = *item("container")->documentVisualBounds();
    std::vector<Geom::Rect> placed;
    for (auto const *id : {"part1", "part2", "part3", "part4"}) {
        auto const bounds = item(id)->documentVisualBounds();
        ASSERT_TRUE(bounds) << id;
        auto grown = sheet;
        grown.expandBy(1e-3);
        EXPECT_TRUE(grown.contains(*bounds)) << id;
        // Touching edges are allowed; the engine works in f32 (1e-3 tolerance).
        auto shrunk = *bounds;
        shrunk.expandBy(-1e-3);
        for (auto const &other : placed) {
            EXPECT_FALSE(other.interiorIntersects(shrunk)) << id << " " << *bounds << " overlaps " << other;
        }
        placed.push_back(*bounds);
    }
    EXPECT_EQ(tool->memory().tracked.size(), 4u);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, DraggedOffPartIsNoLongerTracked)
{
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    ASSERT_EQ(tool->memory().tracked.size(), 2u);

    // Move part2 well off the sheet, then re-nest the sheet (Shift+Enter).
    item("part2")->getRepr()->setAttribute("transform", "translate(500,500)");
    document->ensureUpToDate();
    auto const moved = item("part2")->i2doc_affine();
    desktop->getSelection()->clear();

    auto shift_enter = key_press(GDK_KEY_Return);
    shift_enter.modifiers = GDK_SHIFT_MASK;
    EXPECT_TRUE(tool->root_handler(shift_enter));
    ASSERT_TRUE(wait_for_run(tool));
    document->ensureUpToDate();
    EXPECT_EQ(item("part2")->i2doc_affine(), moved);
    ASSERT_EQ(tool->memory().tracked.size(), 1u);
    EXPECT_EQ(tool->memory().tracked.front().get(), item("part1"));
}

// --- M11 review fixes ----------------------------------------------------------

TEST_F(NestingToolTest, EditJustBeforeARunDoesNotCancelIt)
{
    // The Selection "modified" signal of this edit is still queued (emitted on
    // idle) when the run starts; it must not cancel the run.
    auto *tool = activate();
    ASSERT_TRUE(tool);
    item("part1")->getRepr()->setAttribute("width", "9");
    document->ensureUpToDate();
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    EXPECT_EQ(tool->last_sheet(), item("container"));
    EXPECT_EQ(tool->memory().tracked.size(), 2u);
}

TEST_F(NestingToolTest, ModifierPressedDuringASelectDragDoesNotStickTheDrag)
{
    // The object's own movement is the evidence that each motion reaches the
    // Select drag, so use the live preview (the fast move preview moves a
    // picture and the object only on release; the routing is the same).
    auto *prefs = Inkscape::Preferences::get();
    auto const fast_move = prefs->getEntry("/tools/select/fast_move_preview");
    std::optional<bool> const saved_fast_move =
        fast_move.isSet() ? std::optional<bool>(fast_move.getBool()) : std::nullopt;
    prefs->setBool("/tools/select/fast_move_preview", false);
    auto restore_fast_move = scope_exit([&] {
        if (saved_fast_move) {
            prefs->setBool("/tools/select/fast_move_preview", *saved_fast_move);
        } else {
            prefs->remove("/tools/select/fast_move_preview");
        }
    });
    desktop->getCanvas()->size_allocate(Gtk::Allocation(0, 0, 400, 300), -1);
    auto *tool = activate();
    ASSERT_TRUE(tool);
    desktop->getSelection()->set(item("part1"));
    document->ensureUpToDate();
    desktop->getCanvasDrawing()->update(false);
    auto const bounds = item("part1")->desktopVisualBounds();
    ASSERT_TRUE(bounds);
    auto const p0 = bounds->midpoint();

    ButtonPressEvent press;
    press.button = 1;
    press.pos = press.orig_pos = p0;
    tool->item_handler(item("part1"), press); // a press on an object reaches the item handler
    MotionEvent move;
    move.modifiers = GDK_BUTTON1_MASK;
    move.pos = p0 + Geom::Point(20, 0);
    tool->root_handler(move);
    ASSERT_TRUE(tool->moved) << "the Select tool did not start its drag";
    auto const after_move = item("part1")->i2doc_affine();

    // The user presses the nesting modifier part-way (the constrain habit):
    // the Select tool keeps receiving the gesture's events.
    tool->root_handler(key_press(TARGET_KEY));
    MotionEvent constrained;
    constrained.modifiers = GDK_BUTTON1_MASK | TARGET_MASK;
    constrained.pos = p0 + Geom::Point(40, 0);
    EXPECT_TRUE(tool->root_handler(constrained));
    EXPECT_NE(item("part1")->i2doc_affine(), after_move) << "the modified motion was swallowed";
    EXPECT_FALSE(tool->has_hover_outline());
    ButtonReleaseEvent release;
    release.button = 1;
    release.modifiers = GDK_BUTTON1_MASK | TARGET_MASK;
    release.pos = release.orig_pos = constrained.pos;
    tool->root_handler(release);
    tool->root_handler(key_release(TARGET_KEY, TARGET_MASK));
    drain_main_context();

    // The gesture ended in the Select tool (no stuck grab); nothing was nested.
    // (Whether this harness commits the move is the Select tool's concern; it
    // has no drag-commit test of its own.)
    EXPECT_EQ(tool->grabbed, nullptr);
    EXPECT_FALSE(tool->moved);
    EXPECT_FALSE(tool->is_solving());
    EXPECT_EQ(tool->last_sheet(), nullptr);
    // A later modifier-click still picks a sheet.
    desktop->getSelection()->set(item("part1"));
    EXPECT_TRUE(tool->item_handler(item("container"), target_click()));
    EXPECT_TRUE(tool->is_solving());
    ASSERT_TRUE(wait_for_run(tool));
}

// --- M12 review fixes -------------------------------------------------------------

TEST_F(NestingToolTest, PointingStateFollowsSelectionAndDocumentChanges)
{
    auto *tool = activate();
    ASSERT_TRUE(tool);
    MotionEvent over;
    over.modifiers = TARGET_MASK;
    tool->item_handler(item("container"), over);
    ASSERT_EQ(tool->cursor_filename(), "cursor-nesting-target.svg");

    // Selecting the sheet (Cmd+A while pointing) makes it one of the parts.
    desktop->getSelection()->add(item("container"));
    tool->item_handler(item("container"), over);
    EXPECT_EQ(tool->cursor_filename(), "cursor-nesting-invalid.svg");

    // Deselect it again, then lock it through the document (as an Undo could).
    desktop->getSelection()->remove(item("container"));
    tool->item_handler(item("container"), over);
    ASSERT_EQ(tool->cursor_filename(), "cursor-nesting-target.svg");
    item("container")->setLocked(true);
    document->ensureUpToDate();
    tool->item_handler(item("container"), over);
    EXPECT_EQ(tool->cursor_filename(), "cursor-nesting-invalid.svg");
}

TEST_F(NestingToolTest, EditDuringARunStillCancelsIt)
{
    Preferences::get()->setInt("/tools/nesting/time_limit_ms", 0);
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(tool->is_solving());

    // A real edit of a selected part after the parts were captured.
    item("part1")->getRepr()->setAttribute("width", "9");
    document->ensureUpToDate();
    ASSERT_TRUE(wait_for_run(tool));
    EXPECT_FALSE(tool->is_solving());
    EXPECT_EQ(tool->last_sheet(), nullptr) << "the cancelled run must not remember or apply anything";
}

TEST_F(NestingToolTest, LeftoversAlreadyBesideTheSheetAreNotReportedAsMoved)
{
    item("part1")->getRepr()->setAttribute("width", "100");
    document->ensureUpToDate();
    desktop->getSelection()->set(item("part1"));
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    auto const staged = item("part1")->i2doc_affine();

    // The same leftover again: it is already beside the sheet.
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    EXPECT_EQ(item("part1")->i2doc_affine(), staged);
    std::string const message = desktop->messageStack()->currentMessage() ? desktop->messageStack()->currentMessage() : "";
    EXPECT_NE(message.find("not changed"), std::string::npos) << message;
    EXPECT_EQ(message.find("moved"), std::string::npos) << message;
}

// --- M14: add-to-sheet modes and tracking (add-to-sheet work order 7, 10.2) -----

namespace {

SPItem *add_rect(SPDocument &document, char const *id, double x, double y, double w, double h)
{
    auto *rect = document.getReprDoc()->createElement("svg:rect");
    rect->setAttribute("id", id);
    rect->setAttributeSvgDouble("x", x);
    rect->setAttributeSvgDouble("y", y);
    rect->setAttributeSvgDouble("width", w);
    rect->setAttributeSvgDouble("height", h);
    document.getReprRoot()->appendChild(rect);
    Inkscape::GC::release(rect);
    document.ensureUpToDate();
    return cast<SPItem>(document.getObjectById(id));
}

void expect_no_overlap(SPDocument &document, std::vector<char const *> const &ids)
{
    std::vector<Geom::Rect> seen;
    for (auto const *id : ids) {
        auto const bounds = cast<SPItem>(document.getObjectById(id))->documentVisualBounds();
        ASSERT_TRUE(bounds) << id;
        auto shrunk = *bounds;
        shrunk.expandBy(-1e-3); // touching edges allowed; the engine works in f32
        for (auto const &other : seen) {
            EXPECT_FALSE(other.interiorIntersects(shrunk)) << id;
        }
        seen.push_back(*bounds);
    }
}

} // namespace

TEST_F(NestingToolTest, AddKeepsExistingPartsInPlace)
{
    // Run 1 nests four parts.
    std::vector<SPItem *> first;
    for (auto const *id : {"a", "b", "c", "d"}) {
        first.push_back(add_rect(*document, id, 200, 10, 8, 8));
    }
    desktop->getSelection()->setList(first);
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    ASSERT_EQ(tool->memory().tracked.size(), 4u);
    std::vector<Geom::Affine> placed;
    for (auto *part : first) {
        placed.push_back(part->i2doc_affine());
    }
    DocumentUndo::clearUndo(document.get());
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();

    // Two new parts, Enter: added around the four, which do not move.
    auto *e = add_rect(*document, "e", 200, 40, 8, 8);
    auto *f = add_rect(*document, "f", 220, 40, 8, 8);
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Add parts"}, "");
    DocumentUndo::clearUndo(document.get());
    auto const with_new = sp_repr_save_buf(document->getReprDoc()).raw();
    (void)before;
    desktop->getSelection()->setList(std::vector<SPItem *>{e, f});
    EXPECT_TRUE(tool->root_handler(key_press(GDK_KEY_Return)));
    ASSERT_TRUE(wait_for_run(tool));
    document->ensureUpToDate();

    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i]->i2doc_affine(), placed[i]) << first[i]->getId();
    }
    auto sheet = *item("container")->documentVisualBounds();
    sheet.expandBy(1e-3);
    EXPECT_TRUE(sheet.contains(*e->documentVisualBounds()));
    EXPECT_TRUE(sheet.contains(*f->documentVisualBounds()));
    expect_no_overlap(*document, {"a", "b", "c", "d", "e", "f"});
    EXPECT_EQ(tool->memory().tracked.size(), 6u);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), with_new);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingToolTest, CmdCtrlShiftClickReNests)
{
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    auto *e = add_rect(*document, "e", 200, 40, 12, 12);
    auto *f = add_rect(*document, "f", 220, 40, 12, 12);
    desktop->getSelection()->setList(std::vector<SPItem *>{e, f});

    auto click = target_click();
    click.modifiers |= GDK_SHIFT_MASK;
    EXPECT_TRUE(tool->item_handler(item("container"), click));
    ASSERT_TRUE(tool->is_solving());
    ASSERT_TRUE(wait_for_run(tool));
    document->ensureUpToDate();
    auto sheet = *item("container")->documentVisualBounds();
    sheet.expandBy(1e-3);
    for (auto const *id : {"part1", "part2", "e", "f"}) {
        EXPECT_TRUE(sheet.contains(*item(id)->documentVisualBounds())) << id;
    }
    expect_no_overlap(*document, {"part1", "part2", "e", "f"});
    EXPECT_EQ(tool->memory().tracked.size(), 4u);
}

TEST_F(NestingToolTest, ReNestOnUntrackedSheetBehavesLikeAdd)
{
    auto *second = add_rect(*document, "sheet2", 0, 100, 60, 30);
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    auto const first_run = item("part1")->i2doc_affine();

    auto *e = add_rect(*document, "e", 200, 40, 8, 8);
    desktop->getSelection()->set(e);
    auto click = target_click();
    click.modifiers |= GDK_SHIFT_MASK;
    EXPECT_TRUE(tool->item_handler(second, click));
    ASSERT_TRUE(wait_for_run(tool));
    document->ensureUpToDate();
    // No parts are tracked on sheet2: only the selection is nested there.
    EXPECT_EQ(item("part1")->i2doc_affine(), first_run);
    auto bounds = *second->documentVisualBounds();
    bounds.expandBy(1e-3);
    EXPECT_TRUE(bounds.contains(*e->documentVisualBounds()));
    EXPECT_EQ(tool->last_sheet(), second);
    ASSERT_EQ(tool->memory().tracked.size(), 1u);
    EXPECT_EQ(tool->memory().tracked.front().get(), e);
}

TEST_F(NestingToolTest, RepeatedLeftoversDoNotStack)
{
    auto *wide1 = add_rect(*document, "wide1", 200, 10, 100, 10);
    auto *wide2 = add_rect(*document, "wide2", 200, 40, 90, 10);
    Preferences::get()->setDouble("/tools/nesting/part_spacing", 0.0);
    auto *tool = activate();
    ASSERT_TRUE(tool);
    desktop->getSelection()->set(wide1);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    desktop->getSelection()->set(wide2);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    document->ensureUpToDate();
    auto const first = *wide1->documentVisualBounds();
    auto const second = *wide2->documentVisualBounds();
    EXPECT_GE(second.left(), first.right() + 18.9 - 1e-6) << "the second leftover column overlaps the first";
    EXPECT_EQ(tool->memory().staged.size(), 2u);
}

// --- M15: labels and outlines (add-to-sheet work order 8, 10.2) ---------------------

TEST_F(NestingToolTest, LabelsFollowTheRunLifecycle)
{
    using L = NestingTool::Label;
    Preferences::get()->setBool(Nesting::SHOW_LABELS_PREF_PATH, true);
    item("part2")->getRepr()->setAttribute("width", "100"); // a leftover
    document->ensureUpToDate();
    auto *tool = activate();
    ASSERT_TRUE(tool);

    // L2 with the modifier over a valid sheet; gone on release.
    MotionEvent over;
    over.modifiers = TARGET_MASK;
    tool->item_handler(item("container"), over);
    EXPECT_TRUE(tool->label_visible(L::Hover));
    EXPECT_NE(tool->label_text(L::Hover).raw().find("Add 2"), std::string::npos) << tool->label_text(L::Hover);
    tool->root_handler(key_release(TARGET_KEY, TARGET_MASK));
    EXPECT_FALSE(tool->label_visible(L::Hover));

    // L4 during the run.
    ASSERT_TRUE(tool->nest_into(item("container")));
    EXPECT_TRUE(tool->label_visible(L::Progress));
    ASSERT_TRUE(wait_for_run(tool));
    EXPECT_FALSE(tool->label_visible(L::Progress));

    // L1, L5 and L6 (with O7) after it.
    EXPECT_NE(tool->label_text(L::Sheet).raw().find("1 part "), std::string::npos) << tool->label_text(L::Sheet);
    EXPECT_NE(tool->label_text(L::Sheet).raw().find("% used"), std::string::npos) << tool->label_text(L::Sheet);
    EXPECT_EQ(tool->label_text(L::Sheet).raw().find("%%"), std::string::npos) << tool->label_text(L::Sheet);
    EXPECT_NE(tool->label_text(L::Result).raw().find("1 added"), std::string::npos) << tool->label_text(L::Result);
    EXPECT_NE(tool->label_text(L::Result).raw().find("1 did not fit"), std::string::npos);
    EXPECT_EQ(tool->label_text(L::Leftover), "Did not fit (1)");
    EXPECT_TRUE(tool->has_leftover_outline());

    // L5 ends on the next click.
    ButtonPressEvent click;
    click.button = 1;
    tool->root_handler(click);
    ButtonReleaseEvent release;
    release.button = 1;
    tool->root_handler(release);
    EXPECT_FALSE(tool->label_visible(L::Result));
    EXPECT_TRUE(tool->label_text(L::Result).empty());

    // Everything goes on a tool switch.
    tool->switching_away("/tools/select");
    for (auto label : {L::Hover, L::Progress, L::Hint, L::Sheet, L::Leftover, L::Result}) {
        EXPECT_FALSE(tool->label_visible(label));
    }
    EXPECT_FALSE(tool->has_leftover_outline());
    desktop->setTool("/tools/select");
    drain_main_context();
}

TEST_F(NestingToolTest, ShowLabelsOffHidesLabelsKeepsOutlines)
{
    using L = NestingTool::Label;
    auto *mark = document->getReprDoc()->createElement("svg:rect");
    mark->setAttribute("id", "mark");
    mark->setAttribute("x", "50");
    mark->setAttribute("y", "20");
    mark->setAttribute("width", "5");
    mark->setAttribute("height", "5");
    document->getReprRoot()->appendChild(mark);
    Inkscape::GC::release(mark);
    document->ensureUpToDate();
    Preferences::get()->setBool(Nesting::SHOW_LABELS_PREF_PATH, false);
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    EXPECT_FALSE(tool->label_visible(L::Sheet));
    EXPECT_FALSE(tool->label_text(L::Sheet).empty()) << "the label exists, hidden";
    EXPECT_TRUE(tool->has_sheet_outline());

    MotionEvent over;
    over.modifiers = TARGET_MASK;
    tool->item_handler(item("container"), over);
    EXPECT_FALSE(tool->label_visible(L::Hover));
    // The mark, the fixture's group at the origin and the two placed parts.
    EXPECT_EQ(tool->keepout_outline_count(), 4u);

    // Turning labels back on shows them immediately.
    Preferences::get()->setBool(Nesting::SHOW_LABELS_PREF_PATH, true);
    EXPECT_TRUE(tool->label_visible(L::Hover));
}

TEST_F(NestingToolTest, LabelsNeverOverlap)
{
    using L = NestingTool::Label;
    Preferences::get()->setBool(Nesting::SHOW_LABELS_PREF_PATH, true);
    // A very low sheet: L1 above its top and L5 inside its bottom would collide.
    item("container")->getRepr()->setAttribute("height", "12");
    item("part2")->getRepr()->setAttribute("width", "100");
    document->ensureUpToDate();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    ASSERT_FALSE(tool->label_text(L::Result).empty());

    std::vector<Geom::Rect> visible;
    for (auto label : {L::Hover, L::Progress, L::Hint, L::Sheet, L::Leftover, L::Result}) {
        if (!tool->label_visible(label)) {
            continue;
        }
        auto const rect = tool->label_screen_rect(label);
        ASSERT_TRUE(rect);
        for (auto const &other : visible) {
            EXPECT_FALSE(other.interiorIntersects(*rect));
        }
        visible.push_back(*rect);
    }
    // The higher-priority sheet summary wins over the result badge.
    EXPECT_TRUE(tool->label_visible(L::Sheet));
    EXPECT_FALSE(tool->label_visible(L::Result));
}

TEST_F(NestingToolTest, KeepOutOutlinesAreCachedUntilDocumentChanges)
{
    auto *mark = document->getReprDoc()->createElement("svg:rect");
    mark->setAttribute("id", "mark");
    mark->setAttribute("x", "50");
    mark->setAttribute("y", "20");
    mark->setAttribute("width", "5");
    mark->setAttribute("height", "5");
    document->getReprRoot()->appendChild(mark);
    Inkscape::GC::release(mark);
    document->ensureUpToDate();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    MotionEvent over;
    over.modifiers = TARGET_MASK;

    tool->item_handler(item("container"), over);
    // The mark and the fixture's group at the origin.
    EXPECT_EQ(tool->keepout_outline_count(), 2u);
    EXPECT_EQ(tool->obstacle_cache_builds(), 1u);
    // Release and press again over the same sheet: the cache is reused.
    tool->root_handler(key_release(TARGET_KEY, TARGET_MASK));
    tool->item_handler(item("container"), over);
    EXPECT_EQ(tool->obstacle_cache_builds(), 1u);
    EXPECT_EQ(tool->keepout_outline_count(), 2u);

    // A document edit invalidates it.
    item("mark")->getRepr()->setAttribute("x", "40");
    document->ensureUpToDate();
    tool->root_handler(key_release(TARGET_KEY, TARGET_MASK));
    tool->item_handler(item("container"), over);
    EXPECT_EQ(tool->obstacle_cache_builds(), 2u);
}

TEST_F(NestingToolTest, LockedTrackedPartStaysFixed)
{
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    ASSERT_EQ(tool->memory().tracked.size(), 2u);
    item("part1")->setLocked(true);
    document->ensureUpToDate();
    auto const locked_at = item("part1")->i2doc_affine();
    auto *e = add_rect(*document, "e", 200, 40, 12, 12);
    desktop->getSelection()->set(e);

    auto shift_enter = key_press(GDK_KEY_Return);
    shift_enter.modifiers = GDK_SHIFT_MASK;
    EXPECT_TRUE(tool->root_handler(shift_enter));
    ASSERT_TRUE(wait_for_run(tool));
    document->ensureUpToDate();
    // The locked part is a fixed object: not moved, and nothing lands on it.
    EXPECT_EQ(item("part1")->i2doc_affine(), locked_at);
    expect_no_overlap(*document, {"part1", "part2", "e"});
}

// --- M16 (R1): preparation on the worker ------------------------------------------

TEST_F(NestingToolTest, WorkerPreparationErrorLeavesDocumentUnchanged)
{
    // A self-intersecting sheet passes the capture checks and fails in the
    // worker's geometry phase.
    auto *bowtie = document->getReprDoc()->createElement("svg:path");
    bowtie->setAttribute("id", "bowtie");
    bowtie->setAttribute("d", "M 0,200 L 60,260 L 60,200 L 0,260 Z");
    document->getReprRoot()->appendChild(bowtie);
    Inkscape::GC::release(bowtie);
    document->ensureUpToDate();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("bowtie")));
    ASSERT_TRUE(wait_for_run(tool));
    EXPECT_FALSE(tool->is_solving());
    EXPECT_EQ(tool->last_sheet(), nullptr);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    std::string const message = desktop->messageStack()->currentMessage() ? desktop->messageStack()->currentMessage() : "";
    EXPECT_NE(message.find("could not continue"), std::string::npos) << message;
}

TEST_F(NestingToolTest, UnmeasurablePartOnTheSheetStopsTheRunUnchanged)
{
    // A line without stroke has zero-area bounds: preparation skips it, and
    // it sits on the sheet, so the run stops from the geometry callback
    // (closing the channel inside its own dispatch and joining the worker).
    auto *line = document->getReprDoc()->createElement("svg:path");
    line->setAttribute("id", "line");
    line->setAttribute("d", "M 5,5 L 25,5");
    line->setAttribute("style", "fill:none;stroke:none");
    document->getReprRoot()->appendChild(line);
    Inkscape::GC::release(line);
    document->ensureUpToDate();
    desktop->getSelection()->setList(std::vector<SPItem *>{item("part1"), item("line")});
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto *tool = activate();
    ASSERT_TRUE(tool);
    ASSERT_TRUE(tool->nest_into(item("container")));
    ASSERT_TRUE(wait_for_run(tool));
    EXPECT_FALSE(tool->is_solving());
    EXPECT_EQ(tool->last_sheet(), nullptr);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    std::string const message = desktop->messageStack()->currentMessage() ? desktop->messageStack()->currentMessage() : "";
    EXPECT_NE(message.find("cannot be nested"), std::string::npos) << message;
}
