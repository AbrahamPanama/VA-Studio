// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <glibmm/main.h>

#include "bitmap-adjustment-chemistry.h"
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "selection.h"
#include "ui/bitmap-adjustments-controller.h"
#include "ui/dialog/bitmap-adjustments.h"
#include <gtkmm/scale.h>
#include <gtkmm/adjustment.h>
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::UI;

namespace {

constexpr auto image_data =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJ"
    "AAAADUlEQVQIHWP4z8DwHwAFgAI/ScLx9QAAAABJRU5ErkJggg==";

InkscapeApplication *initialize_gui()
{
    static auto *app = [] {
        g_setenv("INKSCAPE_APP_ID_TAG", "bitmapadjustmentstest", TRUE);
        return new InkscapeApplication(); // Process-lifetime test fixture.
    }();
    return app->gtk_app() ? app : nullptr;
}

void drain_main_context()
{
    auto context = Glib::MainContext::get_default();
    while (context->iteration(false)) {}
}

std::size_t property_index(Filters::BitmapToneProperty property)
{
    return static_cast<std::size_t>(property);
}

class BitmapAdjustmentsControllerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!initialize_gui()) {
            GTEST_SKIP() << "GTK display unavailable; controller GUI fixture skipped";
        }
        if (!Application::exists()) Application::create(false);
        auto const svg = Glib::ustring::compose(R"svg(<svg xmlns="http://www.w3.org/2000/svg"
 xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="40" height="30">
 <image id="one" width="10" height="10" href="%1"/>
 <image id="two" x="10" width="10" height="10" href="%1"/>
 <image id="missing" x="20" width="10" height="10" href="missing-file.png"/>
 <rect id="rect" y="10" width="8" height="8"/>
 <path id="path" d="M10,10h8v8z"/>
 <use id="clone" href="#path" x="10"/>
 <text id="text" x="0" y="28">Editable</text>
 <g id="group"><rect id="child" x="20" y="10" width="8" height="8"/></g>
 <g id="outer"><g id="inner"><rect id="nested" x="30" y="20" width="5" height="5"/></g></g>
</svg>)svg", image_data);
        document = SPDocument::createNewDocFromMem(svg.raw());
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        document->setModifiedSinceSave(false);
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        ASSERT_TRUE(desktop);
        ASSERT_TRUE(image("one"));
        desktop->getSelection()->set(image("one"));
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        document->setModifiedSinceSave(false);
    }

    void TearDown() override
    {
        desktop.reset();
        document.reset();
        drain_main_context();
    }

    SPImage *image(char const *id) const
    {
        return document ? cast<SPImage>(document->getObjectById(id)) : nullptr;
    }

    SPItem *item(char const *id) const
    {
        return document ? cast<SPItem>(document->getObjectById(id)) : nullptr;
    }

    BitmapAdjustmentsController &controller() const
    {
        return desktop->bitmapAdjustmentsController();
    }

    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
};

} // namespace

TEST_F(BitmapAdjustmentsControllerTest, QuerySupportsBitmapVectorAndMixedSelections)
{
    auto snapshot = controller().query();
    EXPECT_TRUE(snapshot.has_selection);
    EXPECT_TRUE(snapshot.has_targets);
    EXPECT_TRUE(snapshot.has_single_bitmap);
    EXPECT_EQ(snapshot.selected_count, 1u);
    EXPECT_EQ(snapshot.target_count, 1u);
    EXPECT_TRUE(snapshot.embedded);
    EXPECT_EQ(snapshot.pixel_width, 1u);
    EXPECT_EQ(snapshot.pixel_height, 1u);

    desktop->getSelection()->add(image("two"));
    snapshot = controller().query();
    EXPECT_TRUE(snapshot.has_targets);
    EXPECT_FALSE(snapshot.has_single_bitmap);
    EXPECT_EQ(snapshot.selected_count, 2u);
    EXPECT_EQ(snapshot.target_count, 2u);
    EXPECT_EQ(controller().targetImage(), nullptr);

    desktop->getSelection()->set(item("rect"));
    snapshot = controller().query();
    EXPECT_TRUE(snapshot.has_targets);
    EXPECT_FALSE(snapshot.has_single_bitmap);
    EXPECT_EQ(snapshot.target_count, 1u);

    desktop->getSelection()->set(image("missing"));
    snapshot = controller().query();
    EXPECT_TRUE(snapshot.has_selection);
    EXPECT_FALSE(snapshot.has_targets);
    EXPECT_TRUE(snapshot.missing);
}

TEST_F(BitmapAdjustmentsControllerTest, PreviewIsAtomicTransientAndCancelsOnContextChanges)
{
    desktop->getSelection()->add(item("rect"));
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());
    ASSERT_FALSE(document->isModifiedSinceSave());
    Filters::BitmapTonePatch patch;
    patch.brightness = 35;

    ASSERT_TRUE(controller().previewPatch(patch));
    EXPECT_FALSE(controller().previewActive());
    drain_main_context();
    EXPECT_TRUE(controller().previewActive());
    EXPECT_EQ(controller().targetCount(), 2u);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_FALSE(document->isModifiedSinceSave());

    desktop->getSelection()->set(image("two"));
    EXPECT_FALSE(controller().previewActive());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);

    ASSERT_TRUE(controller().previewPatch(patch));
    drain_main_context();
    desktop->setTool("/tools/nodes");
    EXPECT_FALSE(controller().previewActive());

    desktop->getSelection()->set(image("one"));
    ASSERT_TRUE(controller().previewPatch(patch));
    drain_main_context();
    image("one")->deleteObject();
    EXPECT_FALSE(controller().previewActive());
    EXPECT_EQ(controller().targetImage(), nullptr);
}

TEST_F(BitmapAdjustmentsControllerTest, SparseCommitPreservesPerObjectValuesAndUsesOneUndo)
{
    Filters::BitmapToneSettings first;
    first.contrast = 11;
    first.highlights = 22;
    Filters::BitmapToneSettings second;
    second.contrast = -33;
    second.midtones = 44;
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image("one"), first));
    ASSERT_TRUE(BitmapAdjustments::apply_tone(item("rect"), second));
    document->ensureUpToDate();
    DocumentUndo::done(document.get(),
                       Util::Internal::ContextString{"Prepare bitmap tone baseline"},
                       "shape-image");
    DocumentUndo::clearUndo(document.get());
    DocumentUndo::clearRedo(document.get());
    document->setModifiedSinceSave(false);

    desktop->getSelection()->set(image("one"));
    desktop->getSelection()->add(item("rect"));
    auto snapshot = controller().query();
    auto const brightness = snapshot.tone[property_index(Filters::BitmapToneProperty::Brightness)];
    auto const contrast = snapshot.tone[property_index(Filters::BitmapToneProperty::Contrast)];
    EXPECT_FALSE(brightness.mixed);
    EXPECT_DOUBLE_EQ(brightness.value, 0);
    EXPECT_TRUE(contrast.mixed);

    Filters::BitmapTonePatch patch;
    patch.brightness = 50;
    ASSERT_TRUE(controller().previewPatch(patch));
    drain_main_context();
    ASSERT_TRUE(controller().commitPatch(patch));

    auto adjusted_first = BitmapAdjustments::query_tone(image("one"));
    auto adjusted_second = BitmapAdjustments::query_tone(item("rect"));
    ASSERT_TRUE(adjusted_first);
    ASSERT_TRUE(adjusted_second);
    EXPECT_DOUBLE_EQ(adjusted_first->brightness, 50);
    EXPECT_DOUBLE_EQ(adjusted_second->brightness, 50);
    EXPECT_DOUBLE_EQ(adjusted_first->contrast, first.contrast);
    EXPECT_DOUBLE_EQ(adjusted_first->highlights, first.highlights);
    EXPECT_DOUBLE_EQ(adjusted_second->contrast, second.contrast);
    EXPECT_DOUBLE_EQ(adjusted_second->midtones, second.midtones);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    auto undone_first = BitmapAdjustments::query_tone(image("one"));
    auto undone_second = BitmapAdjustments::query_tone(item("rect"));
    ASSERT_TRUE(undone_first);
    ASSERT_TRUE(undone_second);
    EXPECT_DOUBLE_EQ(undone_first->brightness, 0);
    EXPECT_DOUBLE_EQ(undone_second->brightness, 0);
    EXPECT_DOUBLE_EQ(undone_first->contrast, first.contrast);
    EXPECT_DOUBLE_EQ(undone_second->contrast, second.contrast);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    adjusted_first = BitmapAdjustments::query_tone(image("one"));
    adjusted_second = BitmapAdjustments::query_tone(item("rect"));
    ASSERT_TRUE(adjusted_first);
    ASSERT_TRUE(adjusted_second);
    EXPECT_DOUBLE_EQ(adjusted_first->brightness, 50);
    EXPECT_DOUBLE_EQ(adjusted_second->brightness, 50);
}

TEST_F(BitmapAdjustmentsControllerTest, ContinuousCommitsCoalesceAndNoOpPreservesRedo)
{
    for (int value = 1; value <= 40; ++value) {
        Filters::BitmapTonePatch patch;
        patch.brightness = value;
        ASSERT_TRUE(controller().commitPatch(patch, true));
    }
    auto current = BitmapAdjustments::query_tone(image("one"));
    ASSERT_TRUE(current);
    EXPECT_DOUBLE_EQ(current->brightness, 40.0);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(BitmapAdjustments::query_tone(image("one")));

    Filters::BitmapTonePatch no_op;
    no_op.brightness = 0;
    EXPECT_FALSE(controller().commitPatch(no_op, true));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    current = BitmapAdjustments::query_tone(image("one"));
    ASSERT_TRUE(current);
    EXPECT_DOUBLE_EQ(current->brightness, 40.0);
}

TEST_F(BitmapAdjustmentsControllerTest, NormalizesAncestorDescendantAndSourceCloneSelections)
{
    desktop->getSelection()->set(item("group"));
    desktop->getSelection()->add(item("child"));
    auto snapshot = controller().query();
    // Selection itself canonicalizes this impossible UI state to the group.
    EXPECT_EQ(snapshot.selected_count, 1u);
    EXPECT_EQ(snapshot.target_count, 1u);

    desktop->getSelection()->set(item("path"));
    desktop->getSelection()->add(item("clone"));
    snapshot = controller().query();
    EXPECT_EQ(snapshot.selected_count, 2u);
    EXPECT_EQ(snapshot.target_count, 1u);
    EXPECT_EQ(snapshot.ignored_count, 0u); // Clone is covered, not incompatible.
}

TEST_F(BitmapAdjustmentsControllerTest, IgnoresUnavailableItemsButAdjustsCompatibleOnes)
{
    desktop->getSelection()->set(image("missing"));
    desktop->getSelection()->add(item("text"));
    auto snapshot = controller().query();
    EXPECT_EQ(snapshot.selected_count, 2u);
    EXPECT_EQ(snapshot.target_count, 1u);
    EXPECT_EQ(snapshot.ignored_count, 1u);
    EXPECT_FALSE(snapshot.has_single_bitmap);

    Filters::BitmapTonePatch patch;
    patch.intensity = 25;
    ASSERT_TRUE(controller().commitPatch(patch));
    auto text_tone = BitmapAdjustments::query_tone(item("text"));
    ASSERT_TRUE(text_tone);
    EXPECT_DOUBLE_EQ(text_tone->intensity, 25);
    EXPECT_FALSE(BitmapAdjustments::query_tone(image("missing")));
}

TEST_F(BitmapAdjustmentsControllerTest, DestructionWithScheduledAndVisiblePreviewIsSafe)
{
    Filters::BitmapTonePatch patch;
    patch.contrast = 50;
    ASSERT_TRUE(controller().previewPatch(patch));
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());
    desktop.reset();
    drain_main_context();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);
    EXPECT_FALSE(document->isModifiedSinceSave());
}

TEST_F(BitmapAdjustmentsControllerTest, FiveHundredCandidatesPublishOnlyTheLatestRequest)
{
    desktop->getSelection()->add(item("rect"));
    auto const xml_before = sp_repr_save_buf(document->getReprDoc());
    Filters::BitmapTonePatch patch;
    for (int candidate = 0; candidate < 500; ++candidate) {
        patch.brightness = -100.0 + (candidate % 201);
        ASSERT_TRUE(controller().previewPatch(patch));
    }
    EXPECT_FALSE(controller().previewActive());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), xml_before);

    drain_main_context();
    EXPECT_TRUE(controller().previewActive());
    ASSERT_TRUE(controller().commitPatch(patch));
    auto image_tone = BitmapAdjustments::query_tone(image("one"));
    auto vector_tone = BitmapAdjustments::query_tone(item("rect"));
    ASSERT_TRUE(image_tone);
    ASSERT_TRUE(vector_tone);
    EXPECT_DOUBLE_EQ(image_tone->brightness, -3.0);
    EXPECT_DOUBLE_EQ(vector_tone->brightness, -3.0);
}

TEST_F(BitmapAdjustmentsControllerTest, ResetExplicitlyClearsAllSixPropertiesAcrossTargets)
{
    Filters::BitmapToneSettings first{10, 20, 30, 40, 50, 60};
    Filters::BitmapToneSettings second{-10, -20, -30, -40, -50, -60};
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image("one"), first));
    ASSERT_TRUE(BitmapAdjustments::apply_tone(item("rect"), second));
    document->ensureUpToDate();
    DocumentUndo::done(document.get(),
                       Util::Internal::ContextString{"Prepare bitmap tone reset baseline"},
                       "shape-image");
    DocumentUndo::clearUndo(document.get());
    desktop->getSelection()->set(image("one"));
    desktop->getSelection()->add(item("rect"));

    ASSERT_TRUE(controller().commitPatch(Filters::BitmapTonePatch::all({})));
    EXPECT_FALSE(BitmapAdjustments::query_tone(image("one")));
    EXPECT_FALSE(BitmapAdjustments::query_tone(item("rect")));
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    auto restored_first = BitmapAdjustments::query_tone(image("one"));
    auto restored_second = BitmapAdjustments::query_tone(item("rect"));
    ASSERT_TRUE(restored_first);
    ASSERT_TRUE(restored_second);
    EXPECT_TRUE(Filters::bitmap_tone_settings_equal(*restored_first, first));
    EXPECT_TRUE(Filters::bitmap_tone_settings_equal(*restored_second, second));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(BitmapAdjustmentsControllerTest, SelectionChangeRejectsEveryScheduledStaleCandidate)
{
    Filters::BitmapTonePatch patch;
    patch.highlights = 80;
    ASSERT_TRUE(controller().previewPatch(patch));
    desktop->getSelection()->set(item("rect"));
    drain_main_context();
    EXPECT_FALSE(controller().previewActive());
    EXPECT_FALSE(BitmapAdjustments::query_tone(image("one")));
    EXPECT_FALSE(BitmapAdjustments::query_tone(item("rect")));
}

// All six controls must address both selected bitmaps, not just singleItem().
TEST_F(BitmapAdjustmentsControllerTest, EveryToneControlCommitsBothBitmapsWithOneUndo)
{
    desktop->getSelection()->add(image("two"));
    std::optional<double> Filters::BitmapTonePatch::*properties[] = {
        &Filters::BitmapTonePatch::brightness, &Filters::BitmapTonePatch::contrast,
        &Filters::BitmapTonePatch::intensity, &Filters::BitmapTonePatch::highlights,
        &Filters::BitmapTonePatch::shadows, &Filters::BitmapTonePatch::midtones
    };
    for (std::size_t i = 0; i < std::size(properties); ++i) {
        SCOPED_TRACE(i);
        Filters::BitmapTonePatch patch;
        patch.*properties[i] = 25;
        ASSERT_TRUE(controller().commitPatch(patch));
        for (auto id : {"one", "two"}) {
            auto tone = BitmapAdjustments::query_tone(image(id));
            ASSERT_TRUE(tone);
            EXPECT_DOUBLE_EQ(Filters::get_bitmap_tone_property(*tone,
                static_cast<Filters::BitmapToneProperty>(i)), 25);
        }
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        EXPECT_FALSE(BitmapAdjustments::query_tone(image("one")));
        EXPECT_FALSE(BitmapAdjustments::query_tone(image("two")));
        EXPECT_FALSE(DocumentUndo::undo(document.get()));
        ASSERT_TRUE(DocumentUndo::redo(document.get()));
        for (auto id : {"one", "two"}) {
            auto tone = BitmapAdjustments::query_tone(image(id));
            ASSERT_TRUE(tone);
            EXPECT_DOUBLE_EQ(Filters::get_bitmap_tone_property(*tone,
                static_cast<Filters::BitmapToneProperty>(i)), 25);
        }
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        DocumentUndo::clearRedo(document.get());
    }
}

namespace {
void collect_tone_scales(Gtk::Widget &widget, std::vector<Gtk::Scale *> &scales)
{
    if (auto scale = dynamic_cast<Gtk::Scale *>(&widget)) scales.push_back(scale);
    for (auto child = widget.get_first_child(); child; child = child->get_next_sibling()) {
        collect_tone_scales(*child, scales);
    }
}

// The GtkRange "move-slider" signal is the action keyboard bindings invoke on
// the real panel. This drives the same public action without faking native
// pointer/window events, so pointer-release and focus retention stay a GUI gap.
void emit_slider_steps(Gtk::Scale &scale, int steps)
{
    for (int i = 0; i < steps; ++i) {
        g_signal_emit_by_name(scale.gobj(), "move-slider", GTK_SCROLL_STEP_FORWARD);
    }
}
}

// Drive the GtkRange action used by keyboard bindings, through the actual panel.
// This does not synthesize native mouse/key delivery; that needs GUI verification.
TEST_F(BitmapAdjustmentsControllerTest, PanelDiscreteEditSurvivesImmediateSelectionChange)
{
    desktop->getSelection()->add(image("two"));
    Dialog::BitmapAdjustmentsPanel panel;
    panel.setDesktop(desktop.get());
    std::vector<Gtk::Scale *> scales;
    collect_tone_scales(panel, scales);
    ASSERT_EQ(scales.size(), 6u);

    for (int i = 0; i < 35; ++i) {
        g_signal_emit_by_name(scales[0]->gobj(), "move-slider", GTK_SCROLL_STEP_FORWARD);
    }
    ASSERT_DOUBLE_EQ(scales[0]->get_adjustment()->get_value(), 35);
    // No main-loop drain: deliberately change selection before a 300 ms timer.
    desktop->getSelection()->set(item("rect"));
    for (auto id : {"one", "two"}) {
        auto tone = BitmapAdjustments::query_tone(image(id));
        ASSERT_TRUE(tone) << id;
        EXPECT_DOUBLE_EQ(tone->brightness, 35);
    }
    EXPECT_FALSE(BitmapAdjustments::query_tone(item("rect")));
    desktop->getSelection()->set(image("one"));
    desktop->getSelection()->add(image("two"));
    EXPECT_DOUBLE_EQ(scales[0]->get_adjustment()->get_value(), 35);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(BitmapAdjustments::query_tone(image("one")));
    EXPECT_FALSE(BitmapAdjustments::query_tone(image("two")));
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    for (auto id : {"one", "two"}) {
        auto tone = BitmapAdjustments::query_tone(image(id));
        ASSERT_TRUE(tone);
        EXPECT_DOUBLE_EQ(tone->brightness, 35);
    }
    // Serialize and reopen a separate document: live renderer state is not enough.
    auto const saved = sp_repr_save_buf(document->getReprDoc());
    auto reopened = SPDocument::createNewDocFromMem(saved.raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    for (auto id : {"one", "two"}) {
        auto restored = cast<SPImage>(reopened->getObjectById(id));
        ASSERT_TRUE(restored);
        auto tone = BitmapAdjustments::query_tone(restored);
        ASSERT_TRUE(tone);
        EXPECT_DOUBLE_EQ(tone->brightness, 35);
    }
}

TEST_F(BitmapAdjustmentsControllerTest, PanelRapidDifferentControlsPreserveBothEdits)
{
    Filters::BitmapToneSettings first, second;
    first.intensity = 12;
    second.intensity = -18;
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image("one"), first));
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image("two"), second));
    document->ensureUpToDate();
    DocumentUndo::done(document.get(),
        Util::Internal::ContextString{"Prepare distinct image tones"}, "shape-image");
    DocumentUndo::clearUndo(document.get());
    desktop->getSelection()->add(image("two"));
    Dialog::BitmapAdjustmentsPanel panel;
    panel.setDesktop(desktop.get());
    std::vector<Gtk::Scale *> scales;
    collect_tone_scales(panel, scales);
    ASSERT_EQ(scales.size(), 6u);
    for (int i = 0; i < 35; ++i) {
        g_signal_emit_by_name(scales[0]->gobj(), "move-slider", GTK_SCROLL_STEP_FORWARD);
    }
    ASSERT_DOUBLE_EQ(scales[0]->get_adjustment()->get_value(), 35);
    for (int i = 0; i < 25; ++i) {
        g_signal_emit_by_name(scales[1]->gobj(), "move-slider", GTK_SCROLL_STEP_FORWARD);
    }
    ASSERT_DOUBLE_EQ(scales[1]->get_adjustment()->get_value(), 25);
    auto expect_tone = [this](char const *id, double brightness, double contrast, double intensity) {
        auto tone = BitmapAdjustments::query_tone(image(id));
        ASSERT_TRUE(tone) << id;
        EXPECT_DOUBLE_EQ(tone->brightness, brightness);
        EXPECT_DOUBLE_EQ(tone->contrast, contrast);
        EXPECT_DOUBLE_EQ(tone->intensity, intensity);
    };

    // A discrete edit must survive leaving the targets. Drain so a scheduled
    // preview or deferred candidate cannot write into the new selection; the
    // native pointer-drag release path stays a GUI-verified gap.
    desktop->getSelection()->set(item("rect"));
    drain_main_context();
    expect_tone("one", 35, 25, 12);
    expect_tone("two", 35, 25, -18);
    EXPECT_FALSE(BitmapAdjustments::query_tone(item("rect")));

    // Each control's rapid series is one undo step: undo contrast (prior 0) first.
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    expect_tone("one", 35, 0, 12);
    expect_tone("two", 35, 0, -18);

    // Then undo brightness; both return to their preexisting per-image tone.
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    expect_tone("one", 0, 0, 12);
    expect_tone("two", 0, 0, -18);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    expect_tone("one", 35, 0, 12);
    expect_tone("two", 35, 0, -18);

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    expect_tone("one", 35, 25, 12);
    expect_tone("two", 35, 25, -18);
    EXPECT_FALSE(DocumentUndo::redo(document.get()));

    // Serialize and reopen a separate document: live renderer state is not enough.
    auto const saved = sp_repr_save_buf(document->getReprDoc());
    auto reopened = SPDocument::createNewDocFromMem(saved.raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    {
        auto restored = cast<SPImage>(reopened->getObjectById("one"));
        ASSERT_TRUE(restored);
        auto tone = BitmapAdjustments::query_tone(restored);
        ASSERT_TRUE(tone);
        EXPECT_DOUBLE_EQ(tone->brightness, 35);
        EXPECT_DOUBLE_EQ(tone->contrast, 25);
        EXPECT_DOUBLE_EQ(tone->intensity, 12);
    }
    {
        auto restored = cast<SPImage>(reopened->getObjectById("two"));
        ASSERT_TRUE(restored);
        auto tone = BitmapAdjustments::query_tone(restored);
        ASSERT_TRUE(tone);
        EXPECT_DOUBLE_EQ(tone->brightness, 35);
        EXPECT_DOUBLE_EQ(tone->contrast, 25);
        EXPECT_DOUBLE_EQ(tone->intensity, -18);
    }
}

// BUG-001 panel regression: a completed keyboard-binding edit (the GtkRange
// move-slider action) must survive an immediate selection change for every one
// of the six actual panel controls and for single, multiple, group and nested
// group target sets under the documented legacy per-composite-root semantics.
TEST_F(BitmapAdjustmentsControllerTest, PanelKeyboardActionMatrixPersistsAllSixControlsAcrossTargetSets)
{
    struct TargetSet {
        char const *name;
        std::vector<char const *> selected;
        std::vector<char const *> targets;   // must carry the committed filter
        std::vector<char const *> untouched; // must stay unrelated/clean
    };
    std::array<TargetSet, 4> const sets = {{
        {"single image", {"one"}, {"one"}, {}},
        {"multiple images", {"one", "two"}, {"one", "two"}, {}},
        {"group", {"group"}, {"group"}, {"child"}},
        {"nested group", {"outer"}, {"outer"}, {"inner", "nested"}},
    }};

    Dialog::BitmapAdjustmentsPanel panel;
    panel.setDesktop(desktop.get());
    std::vector<Gtk::Scale *> scales;
    collect_tone_scales(panel, scales);
    ASSERT_EQ(scales.size(), 6u);

    auto select = [this](std::vector<char const *> const &ids) {
        desktop->getSelection()->set(item(ids.front()));
        for (std::size_t i = 1; i < ids.size(); ++i) desktop->getSelection()->add(item(ids[i]));
        drain_main_context();
    };

    for (auto const &set : sets) {
        for (std::size_t i = 0; i < scales.size(); ++i) {
            int const value = 10 + 5 * static_cast<int>(i);
            SCOPED_TRACE(std::string(set.name) + " control " + std::to_string(i));
            // Start from an unrelated object so selecting the targets always
            // triggers a fresh panel sync at a clean baseline.
            select({"rect"});
            select(set.selected);
            ASSERT_DOUBLE_EQ(scales[i]->get_adjustment()->get_value(), 0.0);
            emit_slider_steps(*scales[i], value);
            ASSERT_DOUBLE_EQ(scales[i]->get_adjustment()->get_value(), value);

            // Change selection with no main-loop drain: an older deferred
            // (debounced) commit would be canceled here and the edit lost.
            desktop->getSelection()->set(item("rect"));
            drain_main_context();
            for (auto id : set.untouched) {
                EXPECT_FALSE(BitmapAdjustments::query_tone(item(id))) << id;
            }
            EXPECT_FALSE(BitmapAdjustments::query_tone(item("rect")));

            // Reselect: the persisted property and the displayed panel value
            // must both show the completed edit; other controls stay neutral.
            select(set.selected);
            EXPECT_DOUBLE_EQ(scales[i]->get_adjustment()->get_value(), value);
            for (auto id : set.targets) {
                auto tone = BitmapAdjustments::query_tone(item(id));
                ASSERT_TRUE(tone) << id;
                EXPECT_DOUBLE_EQ(Filters::get_bitmap_tone_property(
                    *tone, static_cast<Filters::BitmapToneProperty>(i)), value) << id;
                for (std::size_t other = 0; other < scales.size(); ++other) {
                    if (other == i) continue;
                    EXPECT_DOUBLE_EQ(Filters::get_bitmap_tone_property(
                        *tone, static_cast<Filters::BitmapToneProperty>(other)), 0.0) << id;
                }
            }

            // One action gives exactly one Undo and Redo for the target set.
            ASSERT_TRUE(DocumentUndo::undo(document.get()));
            for (auto id : set.targets) {
                auto tone = BitmapAdjustments::query_tone(item(id));
                EXPECT_TRUE(!tone || Filters::get_bitmap_tone_property(
                    *tone, static_cast<Filters::BitmapToneProperty>(i)) == 0.0) << id;
            }
            ASSERT_TRUE(DocumentUndo::redo(document.get()));
            for (auto id : set.targets) {
                auto tone = BitmapAdjustments::query_tone(item(id));
                ASSERT_TRUE(tone) << id;
                EXPECT_DOUBLE_EQ(Filters::get_bitmap_tone_property(
                    *tone, static_cast<Filters::BitmapToneProperty>(i)), value) << id;
            }
            ASSERT_FALSE(DocumentUndo::redo(document.get()));
            ASSERT_TRUE(DocumentUndo::undo(document.get()));
            DocumentUndo::clearRedo(document.get());
        }
    }
}

// Group editing follows the documented legacy per-composite-root policy: the
// selected group itself receives one filter, unrelated child adjustments stay
// untouched, and members are not recursively rewritten. Reopen proves the
// persisted XML rather than live renderer state.
TEST_F(BitmapAdjustmentsControllerTest, PanelGroupEditKeepsChildSettingsAndSurvivesReopen)
{
    Filters::BitmapToneSettings child_settings;
    child_settings.contrast = 44;
    ASSERT_TRUE(BitmapAdjustments::apply_tone(item("child"), child_settings));
    Filters::BitmapToneSettings inner_settings;
    inner_settings.midtones = -21;
    ASSERT_TRUE(BitmapAdjustments::apply_tone(item("inner"), inner_settings));
    document->ensureUpToDate();
    DocumentUndo::done(document.get(),
                       Util::Internal::ContextString{"Prepare group child tones"},
                       "shape-image");
    DocumentUndo::clearUndo(document.get());
    DocumentUndo::clearRedo(document.get());
    document->setModifiedSinceSave(false);

    desktop->getSelection()->set(item("group"));
    Dialog::BitmapAdjustmentsPanel panel;
    panel.setDesktop(desktop.get());
    std::vector<Gtk::Scale *> scales;
    collect_tone_scales(panel, scales);
    ASSERT_EQ(scales.size(), 6u);

    emit_slider_steps(*scales[0], 30); // Brightness.
    ASSERT_DOUBLE_EQ(scales[0]->get_adjustment()->get_value(), 30);
    // No drain before the selection change: the synchronous discrete commit is
    // what keeps the edit; a deferred candidate would lose it here.
    desktop->getSelection()->set(item("rect"));
    drain_main_context();

    auto group_tone = BitmapAdjustments::query_tone(item("group"));
    ASSERT_TRUE(group_tone);
    EXPECT_DOUBLE_EQ(group_tone->brightness, 30);
    EXPECT_DOUBLE_EQ(group_tone->contrast, 0);

    auto child_tone = BitmapAdjustments::query_tone(item("child"));
    ASSERT_TRUE(child_tone);
    EXPECT_DOUBLE_EQ(child_tone->brightness, 0);
    EXPECT_DOUBLE_EQ(child_tone->contrast, 44);
    auto inner_tone = BitmapAdjustments::query_tone(item("inner"));
    ASSERT_TRUE(inner_tone);
    EXPECT_DOUBLE_EQ(inner_tone->brightness, 0);
    EXPECT_DOUBLE_EQ(inner_tone->midtones, -21);
    EXPECT_FALSE(BitmapAdjustments::query_tone(item("nested")));

    // Reselect shows the committed group value; one Undo removes only the group
    // edit and leaves the child's own contrast.
    desktop->getSelection()->set(item("group"));
    drain_main_context();
    EXPECT_DOUBLE_EQ(scales[0]->get_adjustment()->get_value(), 30);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(BitmapAdjustments::query_tone(item("group")));
    child_tone = BitmapAdjustments::query_tone(item("child"));
    ASSERT_TRUE(child_tone);
    EXPECT_DOUBLE_EQ(child_tone->contrast, 44);
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    group_tone = BitmapAdjustments::query_tone(item("group"));
    ASSERT_TRUE(group_tone);
    EXPECT_DOUBLE_EQ(group_tone->brightness, 30);

    // Serialize/reopen: group filter and untouched child settings both persist.
    auto const saved = sp_repr_save_buf(document->getReprDoc());
    auto reopened = SPDocument::createNewDocFromMem(saved.raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    {
        auto restored = cast<SPItem>(reopened->getObjectById("group"));
        ASSERT_TRUE(restored);
        auto tone = BitmapAdjustments::query_tone(restored);
        ASSERT_TRUE(tone);
        EXPECT_DOUBLE_EQ(tone->brightness, 30);
    }
    {
        auto restored = cast<SPItem>(reopened->getObjectById("child"));
        ASSERT_TRUE(restored);
        auto tone = BitmapAdjustments::query_tone(restored);
        ASSERT_TRUE(tone);
        EXPECT_DOUBLE_EQ(tone->brightness, 0);
        EXPECT_DOUBLE_EQ(tone->contrast, 44);
    }
    {
        auto restored = cast<SPItem>(reopened->getObjectById("inner"));
        ASSERT_TRUE(restored);
        auto tone = BitmapAdjustments::query_tone(restored);
        ASSERT_TRUE(tone);
        EXPECT_DOUBLE_EQ(tone->midtones, -21);
    }
}
