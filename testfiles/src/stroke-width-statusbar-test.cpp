// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glib.h>
#include <gtkmm/label.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/window.h>
#include <gtkmm/box.h>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "event-log.h"
#include "preferences.h"
#include "style.h"
#include "ui/widget/generic/popover-menu.h"
#include "ui/widget/image-properties.h"
#include "util/units.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "message-stack.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-lpe-item.h"
#include "object/sp-text.h"
#include "selection.h"
#include "ui/tools/text-tool.h"
#include "ui/widget/canvas.h"
#include "ui/widget/gtk-registry.h"
#include "ui/widget/selected-style.h"
#include "xml/repr.h"

using namespace Inkscape;

namespace {

// Real GTK signal emission exercises the installed handlers, not removed
// RotateableStrokeWidth methods. Native pointer routing/modifiers and wheel
// propagation require the separate desktop-session cases recorded in WP3-plan.
class StatusbarProbe : public UI::Widget::SelectedStyle {
public:
    std::string readout() const { return stroke_width->get_text().raw(); }
    Gtk::Widget &width() { return *stroke_width_box; }
    Gtk::Widget &strokeSwatch() { return *swatch[UI::Widget::SS_STROKE]; }
    Gtk::Widget &fillSwatch() { return *swatch[UI::Widget::SS_FILL]; }
    void open() { openStrokeWidthMenu(false); } // same opening/list path, no native popup placement
    bool opening() const { return bool(_sw_opening); }
    UI::Widget::PopoverMenu &menu() { return *_popup_sw; }
    UI::Widget::PopoverMenu &strokeMenu() { return *_popup[UI::Widget::SS_STROKE]; }
    std::uint64_t generation() const { return scopeGeneration(); }
    bool delayedPaste(char const *text, char const *type, SPDesktop *target, std::uint64_t generation) { return _apply_pasted_text(text, type, target, generation); }
    bool paste(char const *text, char const *type, SPDesktop *target) { return _apply_pasted_text(text, type, target, scopeGeneration()); }
};

using MenuItem = UI::Widget::PopoverMenuItem;

MenuItem *find_item(UI::Widget::PopoverMenu &menu, std::string const &label)
{
    for (auto *widget : menu.get_items()) {
        auto *item = dynamic_cast<MenuItem *>(widget);
        auto *text = item ? dynamic_cast<Gtk::Label *>(item->get_child()) : nullptr;
        if (text && text->get_text() == label) return item;
    }
    return nullptr;
}

void activate(MenuItem *item)
{
    ASSERT_NE(item, nullptr);
    g_signal_emit_by_name(item->gobj(), "clicked");
}

// Keep both the GTK object and its managed gtkmm wrapper alive after rebuilding.
struct RetainedItem {
    explicit RetainedItem(MenuItem *item) : item(item) { if (item) g_object_ref(item->gobj()); }
    ~RetainedItem() { if (item) g_object_unref(item->gobj()); }
    MenuItem *item;
};

template <typename F> void controllers(Gtk::Widget &widget, F visit)
{
    auto *model = gtk_widget_observe_controllers(widget.gobj());
    for (unsigned i = 0; i < g_list_model_get_n_items(model); ++i) {
        auto *controller = G_OBJECT(g_list_model_get_item(model, i));
        visit(controller);
        g_object_unref(controller);
    }
    g_object_unref(model);
}

InkscapeApplication *gui()
{
    static auto *app = [] {
        g_setenv("LANGUAGE", "en", TRUE);
        g_setenv("INKSCAPE_APP_ID_TAG", "strokeWidthStatusbarTest", TRUE);
        auto *result = new InkscapeApplication();
        if (result->gtk_app()) UI::Widget::register_all();
        return result;
    }();
    return app->gtk_app() ? app : nullptr;
}

class StrokeWidthStatusbarTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!gui()) GTEST_SKIP() << "GTK display unavailable";
        if (!Application::exists()) Application::create(false);
        auto const path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                          "data/stroke-width/mixed-members.svg";
        auto const path_utf8 = std::string(reinterpret_cast<char const *>(path.u8string().c_str()));
        gchar *bytes = nullptr;
        gsize length = 0;
        ASSERT_TRUE(g_file_get_contents(path_utf8.c_str(), &bytes, &length, nullptr));
        document = SPDocument::createNewDocFromMem(std::string(bytes, length));
        g_free(bytes);
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        ASSERT_TRUE(desktop);
        // The canvas lives in a window in the application. The text tool's
        // input method needs that on Windows (GtkIMContextIME reads the
        // canvas's native surface; without a window it is a fatal
        // Gtk-CRITICAL). The window is never shown.
        host = std::make_unique<Gtk::Window>();
        host->set_child(*desktop->getCanvas());
        auto *mixed = dynamic_cast<SPItem *>(document->getObjectById("mixed"));
        ASSERT_TRUE(mixed);
        desktop->getSelection()->set(mixed);
        widget = std::make_unique<StatusbarProbe>();
        widget->setDesktop(desktop.get());
        widget->open();
        choose_unit("pt");
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
    }
    void TearDown() override {
        if (widget) widget->setDesktop(nullptr);
        widget.reset();
        if (host) host->unset_child(); // the desktop owns the canvas
        host.reset(); desktop.reset(); document.reset();
    }
    std::string xml() const { return sp_repr_save_buf(document->getReprDoc()).raw(); }
    std::string style(char const *id) const {
        auto *object = document->getObjectById(id);
        auto *value = object && object->getRepr() ? object->getRepr()->attribute("style") : nullptr;
        return value ? value : "";
    }
    void one_undo(std::string const &before, std::string const &after) {
        ASSERT_NE(before, after);
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        EXPECT_EQ(xml(), before);
        EXPECT_FALSE(DocumentUndo::undo(document.get()));
        ASSERT_TRUE(DocumentUndo::redo(document.get()));
        EXPECT_EQ(xml(), after);
    }
    void select(char const *id) {
        auto *item = dynamic_cast<SPItem *>(document->getObjectById(id));
        ASSERT_TRUE(item);
        desktop->getSelection()->set(item);
        widget->update();
    }
    void choose_unit(char const *abbr) {
        for (auto *child : widget->menu().get_items()) {
            auto *item = dynamic_cast<MenuItem *>(child);
            auto *radio = item ? dynamic_cast<Gtk::CheckButton *>(item->get_child()) : nullptr;
            if (radio && radio->get_label() == abbr) {
                activate(item); // actual radio item activation, including rebuild
                return;
            }
        }
        FAIL() << "Missing unit " << abbr;
    }
    void preset(char const *label) {
        widget->open();
        activate(find_item(widget->menu(), label));
    }
    double width(char const *id) {
        document->ensureUpToDate();
        return document->getObjectById(id)->style->stroke_width.computed;
    }
    struct Snapshot {
        std::string xml, defaults, current;
        bool dirty, autosave, virgin;
        unsigned long serial;
    };
    Snapshot snap() const {
        Glib::ustring current;
        if (desktop->current) sp_repr_css_write_string(desktop->current, current);
        return {xml(), Preferences::get()->getString("/desktop/style").raw(), current.raw(),
                document->isModifiedSinceSave(), document->isModifiedSinceAutoSave(), document->getVirgin(),
                document->get_event_log()->getCurrEventSerial()};
    }
    void unchanged(Snapshot const &before) const {
        auto const after = snap();
        EXPECT_EQ(after.xml, before.xml);
        EXPECT_EQ(after.defaults, before.defaults);
        EXPECT_EQ(after.current, before.current);
        EXPECT_EQ(after.dirty, before.dirty);
        EXPECT_EQ(after.autosave, before.autosave);
        EXPECT_EQ(after.virgin, before.virgin);
        EXPECT_EQ(after.serial, before.serial);
        EXPECT_FALSE(DocumentUndo::interactionActive(document.get()));
    }
    void expect_label(char const *label) {
        auto const &columns = EventLog::getColumns();
        auto row = document->get_event_log()->getCurrEvent();
        EXPECT_EQ(Glib::ustring((*row)[columns.description]), label);
        EXPECT_EQ(Glib::ustring((*row)[columns.icon_name]), "dialog-fill-and-stroke");
    }
    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<Gtk::Window> host;
    std::unique_ptr<StatusbarProbe> widget;
};

TEST_F(StrokeWidthStatusbarTest, D01D02WidthHasNoGestureAdmissionAndPreservesRedo)
{
    select("outside");
    preset("6");
    auto const redo_xml = xml();
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    for (auto const *id : {"mixed", "bitmap", "text"}) {
        select(id);
        auto before = snap();
        auto *root_controllers = gtk_widget_observe_controllers(GTK_WIDGET(host->gobj()));
        auto const root_count = g_list_model_get_n_items(root_controllers);
        unsigned messages = 0;
        auto connection = desktop->messageStack()->connectChanged(
            [&](MessageType, char const *) { ++messages; });
        unsigned click_count = 0;
        controllers(widget->width(), [&](GObject *controller) {
            EXPECT_FALSE(GTK_IS_GESTURE_DRAG(controller)); // no begin/update/end callbacks or token path
            EXPECT_FALSE(GTK_IS_EVENT_CONTROLLER_SCROLL(controller)); // wheel cannot be consumed
            EXPECT_FALSE(GTK_IS_EVENT_CONTROLLER_KEY(controller)); // no shortcut capture
            if (GTK_IS_GESTURE_CLICK(controller)) {
                ++click_count;
                g_signal_emit_by_name(controller, "pressed", 1, 0.0, 0.0);
                g_signal_emit_by_name(controller, "released", 1, 30.0, 30.0);
                EXPECT_EQ(gtk_gesture_get_sequence_state(GTK_GESTURE(controller), nullptr), GTK_EVENT_SEQUENCE_NONE);
            }
        });
        EXPECT_EQ(click_count, 1u);
        EXPECT_EQ(g_list_model_get_n_items(root_controllers), root_count);
        g_object_unref(root_controllers);
        EXPECT_EQ(messages, 0u);
        connection.disconnect();
        unchanged(before);
    }
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    EXPECT_EQ(xml(), redo_xml);
}

TEST_F(StrokeWidthStatusbarTest, D02WheelSignalsPropagateWithoutWidthMutation)
{
    Gtk::Box parent;
    parent.append(*widget);
    auto wheel = Gtk::EventControllerScroll::create();
    wheel->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
    unsigned observed = 0;
    wheel->signal_scroll().connect([&](double, double) { ++observed; return false; }, true);
    parent.add_controller(wheel);
    auto const before = snap();
    for (double delta : {1.0, -1.0, 0.1, -0.1, 1.0}) {
        gboolean consumed = TRUE;
        g_signal_emit_by_name(wheel->gobj(), "scroll", 0.0, delta, &consumed);
        EXPECT_FALSE(consumed);
        unchanged(before);
    }
    EXPECT_EQ(observed, 5u);
    parent.remove(*widget);
    // Ancestor signal delivery is supplemental. Native wheel hit-testing is desktop-only.
}

TEST_F(StrokeWidthStatusbarTest, D03SwatchControllersAndWheelBehaviourRemain)
{
    select("thin");
    for (auto *swatch : {&widget->fillSwatch(), &widget->strokeSwatch()}) {
        unsigned drag = 0, scroll = 0;
        controllers(*swatch, [&](GObject *controller) {
            if (GTK_IS_GESTURE_DRAG(controller)) ++drag;
            if (GTK_IS_EVENT_CONTROLLER_SCROLL(controller)) ++scroll;
        });
        EXPECT_EQ(drag, 1u);
        EXPECT_EQ(scroll, 1u);
    }
    auto const before = xml();
    auto const width_before = width("thin");
    controllers(widget->strokeSwatch(), [&](GObject *controller) {
        if (GTK_IS_EVENT_CONTROLLER_SCROLL(controller)) {
            gboolean consumed = FALSE;
            g_signal_emit_by_name(controller, "scroll", 0.0, -1.0, &consumed);
            EXPECT_TRUE(consumed);
        }
    });
    EXPECT_DOUBLE_EQ(width("thin"), width_before);
    one_undo(before, xml());
}

TEST_F(StrokeWidthStatusbarTest, D04WidthClickSequenceRemainsUnclaimed)
{
    auto before = snap();
    controllers(widget->width(), [&](GObject *controller) {
        if (GTK_IS_GESTURE_CLICK(controller)) {
            EXPECT_EQ(gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(controller)), 0u);
            g_signal_emit_by_name(controller, "released", 1, 0.0, 0.0);
            EXPECT_EQ(gtk_gesture_get_sequence_state(GTK_GESTURE(controller), nullptr), GTK_EVENT_SEQUENCE_NONE);
        }
    });
    unchanged(before);
    // A signal without a native event has current_button=0. Actual left-click
    // Stroke Style routing and inert width middle-click remain desktop-session gates.
}

TEST_F(StrokeWidthStatusbarTest, D05ExactPresetListsAndUnitRadioRebuild)
{
    std::vector<std::string> const mm{"Hairline", "0.1", "0.2", "0.25", "0.35", "0.5", "0.75",
        "1", "1.5", "2", "2.5", "3", "Remove Stroke"};
    std::vector<std::string> const pt{"Hairline", "0.25", "0.5", "0.75", "1", "1.5", "2", "3",
        "4", "6", "8", "10", "12", "Remove Stroke"};
    for (auto const *abbr : {"mm", "cm", "m", "pt", "px", "in", "ft"}) {
        widget->open();
        choose_unit(abbr);
        std::vector<std::string> actual;
        for (auto *child : widget->menu().get_items()) {
            auto *item = dynamic_cast<MenuItem *>(child);
            auto *label = item ? dynamic_cast<Gtk::Label *>(item->get_child()) : nullptr;
            if (label && item->get_sensitive()) actual.push_back(label->get_text().raw());
        }
        EXPECT_EQ(actual, abbr == std::string("mm") || abbr == std::string("cm") || abbr == std::string("m") ? mm : pt);
    }
}

TEST_F(StrokeWidthStatusbarTest, D05EveryNumericDescriptorUsesItsOwnUnitAndOneUndo)
{
    select("outside");
    document->getObjectById("outside")->getRepr()->setAttribute("style", "fill:none;stroke:#abc;stroke-width:13");
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), Util::Internal::ContextString("Setup"), "");
    std::vector<std::pair<char const *, double>> const mm{{"0.1", .1}, {"0.2", .2}, {"0.25", .25},
        {"0.35", .35}, {"0.5", .5}, {"0.75", .75}, {"1", 1}, {"1.5", 1.5}, {"2", 2}, {"2.5", 2.5}, {"3", 3}};
    std::vector<std::pair<char const *, double>> const pt{{"0.25", .25}, {"0.5", .5}, {"0.75", .75},
        {"1", 1}, {"1.5", 1.5}, {"2", 2}, {"3", 3}, {"4", 4}, {"6", 6}, {"8", 8}, {"10", 10}, {"12", 12}};
    for (auto const *abbr : {"mm", "cm", "m", "pt", "px", "in", "ft"}) {
        widget->open(); choose_unit(abbr);
        bool const metric = std::string(abbr) == "mm" || std::string(abbr) == "cm" || std::string(abbr) == "m";
        for (auto const &[label, value] : metric ? mm : pt) {
            SCOPED_TRACE(std::string(abbr) + ":" + label);
            DocumentUndo::clearUndo(document.get()); DocumentUndo::clearRedo(document.get());
            auto const before = xml();
            preset(label);
            EXPECT_NEAR(width("outside"), value * (metric ? 96.0 / 25.4 : 96.0 / 72.0), 1e-5);
            expect_label("Set stroke width");
            auto const after = xml();
            one_undo(before, after);
            ASSERT_TRUE(DocumentUndo::undo(document.get())); // restore original fixture for the next case
        }
    }
}

TEST_F(StrokeWidthStatusbarTest, D05PresetMixedSelectionOneUndo)
{
    EXPECT_NE(widget->readout().find("Mixed"), std::string::npos);
    auto const before = xml();
    preset("3");
    EXPECT_EQ(style("bitmap"), "");
    EXPECT_NE(style("protected").find("stroke-width:3"), std::string::npos);
    expect_label("Set stroke width");
    one_undo(before, xml());
}

TEST_F(StrokeWidthStatusbarTest, D05HairlineAndRemovalAreDistinctCommands)
{
    select("outside");
    auto const before = xml();
    preset("Hairline");
    EXPECT_TRUE(document->getObjectById("outside")->style->stroke_extensions.hairline);
    expect_label("Set stroke width");
    one_undo(before, xml());
    DocumentUndo::clearUndo(document.get()); DocumentUndo::clearRedo(document.get());
    auto const hairline_xml = xml();
    preset("Remove Stroke");
    EXPECT_NE(style("outside").find("stroke:none"), std::string::npos);
    expect_label("Remove stroke");
    one_undo(hairline_xml, xml());
}

TEST_F(StrokeWidthStatusbarTest, PopupRemoveOneUndo)
{
    auto const before = xml();
    std::vector<std::string> texts;
    auto connection = desktop->messageStack()->connectChanged(
        [&](MessageType, char const *text) { if (text) texts.emplace_back(text); });
    preset("Remove Stroke");
    connection.disconnect();
    // The requested none paint is not a warning; only the skipped members are reported.
    EXPECT_NE(std::find(texts.begin(), texts.end(), "Stroke removed; incompatible or protected items were skipped"),
              texts.end());
    for (auto const &text : texts) EXPECT_EQ(text.find("no stroke colour"), std::string::npos) << text;
    EXPECT_NE(style("thin").find("stroke:none"), std::string::npos);
    EXPECT_NE(style("protected").find("stroke:black"), std::string::npos);
    one_undo(before, xml());
}

TEST_F(StrokeWidthStatusbarTest, C05OldMenuItemRefusedAfterUnitRadioChange)
{
    select("outside");
    widget->open(); choose_unit("mm");
    for (auto const *next : {"pt", "cm"}) {
        widget->open();
        RetainedItem old(find_item(widget->menu(), "0.25"));
        ASSERT_TRUE(old.item);
        choose_unit(next);
        auto const before = snap();
        activate(old.item);
        unchanged(before);
        EXPECT_FALSE(widget->opening());
        EXPECT_FALSE(widget->menu().get_visible());
        preset("0.25");
        EXPECT_NEAR(width("outside"), .25 * (std::string(next) == "pt" ? 96.0 / 72.0 : 96.0 / 25.4), 1e-5);
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        widget->open(); choose_unit("mm");
    }
}

TEST_F(StrokeWidthStatusbarTest, StaleSelectionStyleDismissalAndReopeningRefused)
{
    for (unsigned change = 0; change < 4; ++change) {
        select("outside"); widget->open();
        RetainedItem old(find_item(widget->menu(), "3")); ASSERT_TRUE(old.item);
        if (change == 0) select("thin");
        if (change == 1) { document->getObjectById("outside")->getRepr()->setAttribute("style", "fill:none;stroke:blue;stroke-width:4"); document->ensureUpToDate(); widget->update(); }
        if (change == 2) g_signal_emit_by_name(widget->menu().gobj(), "closed");
        if (change == 3) widget->open();
        auto const before = snap();
        activate(old.item);
        unchanged(before);
        EXPECT_FALSE(widget->opening());
    }
}

TEST_F(StrokeWidthStatusbarTest, StaleDesktopDetachAndToolChangeRefused)
{
    for (bool detach : {false, true}) {
        select("outside"); widget->open();
        RetainedItem old(find_item(widget->menu(), "3")); ASSERT_TRUE(old.item);
        if (detach) widget->setDesktop(nullptr);
        else desktop->setTool("/tools/text");
        auto const before = snap();
        activate(old.item); unchanged(before);
        EXPECT_FALSE(widget->opening());
        widget->setDesktop(desktop.get());
    }
}

TEST_F(StrokeWidthStatusbarTest, DocumentReplacementRefusesRetainedMenuItem)
{
    widget->open();
    RetainedItem old(find_item(widget->menu(), "3")); ASSERT_TRUE(old.item);
    auto replacement = SPDocument::createNewDocFromMem(xml()); ASSERT_TRUE(replacement);
    desktop->setDocument(replacement.get());
    auto const before = xml();
    auto const replacement_xml = sp_repr_save_buf(replacement->getReprDoc());
    activate(old.item);
    EXPECT_EQ(xml(), before);
    EXPECT_EQ(sp_repr_save_buf(replacement->getReprDoc()), replacement_xml);
    EXPECT_FALSE(widget->opening());
    desktop->setDocument(document.get());
}

TEST_F(StrokeWidthStatusbarTest, RepeatedActivationDoesNotCommitTwice)
{
    select("outside"); widget->open();
    RetainedItem item(find_item(widget->menu(), "6")); ASSERT_TRUE(item.item);
    auto const before = xml();
    activate(item.item);
    auto const after = snap();
    activate(item.item); unchanged(after);
    one_undo(before, xml());
}

TEST_F(StrokeWidthStatusbarTest, ExactPresetNoOpPreservesRedoAndDefaults)
{
    select("outside"); preset("3");
    auto const baseline = xml();
    preset("6"); auto const redo_xml = xml();
    ASSERT_TRUE(DocumentUndo::undo(document.get())); EXPECT_EQ(xml(), baseline);
    auto const before = snap();
    unsigned messages = 0;
    auto connection = desktop->messageStack()->connectChanged([&](MessageType, char const *) { ++messages; });
    preset("3"); unchanged(before);
    EXPECT_EQ(messages, 0u); connection.disconnect();
    ASSERT_TRUE(DocumentUndo::redo(document.get())); EXPECT_EQ(xml(), redo_xml);
}

TEST_F(StrokeWidthStatusbarTest, BusyCommandRefusesThenFreshOpeningSucceeds)
{
    auto token = DocumentUndo::beginRollbackableInteraction(document.get()); ASSERT_TRUE(token);
    auto const before = xml(); preset("3"); EXPECT_EQ(xml(), before);
    token->rollback();
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    preset("3"); one_undo(before, xml());
}

TEST_F(StrokeWidthStatusbarTest, BUG012StrokeLastSetColorLeavesFillAlone)
{
    select("thin");
    sp_repr_css_set_property(desktop->current, "stroke", "#ff0000");
    auto const fill = document->getObjectById("thin")->style->fill;
    auto const before = xml();
    activate(find_item(widget->strokeMenu(), "Last Set Color"));
    EXPECT_EQ(document->getObjectById("thin")->style->stroke.getColor().toString(false), "#ff0000");
    EXPECT_EQ(document->getObjectById("thin")->style->fill, fill);
    one_undo(before, xml());
}

TEST_F(StrokeWidthStatusbarTest, D04BUG012StrokeSwatchToggleRemovesThenRestoresStrokeOnly)
{
    select("thin");
    auto const fill = document->getObjectById("thin")->style->fill;
    auto const before = xml();
    widget->onStrokeMiddleClick(); // native middle-button routing is desktop-only
    EXPECT_NE(style("thin").find("stroke:none"), std::string::npos);
    one_undo(before, xml());
    DocumentUndo::clearUndo(document.get()); DocumentUndo::clearRedo(document.get());
    widget->update();
    ASSERT_EQ(widget->_mode[UI::Widget::SS_STROKE], UI::Widget::SS_NONE);
    sp_repr_css_set_property(desktop->current, "stroke", "#ff0000");
    auto const removed = xml();
    widget->onStrokeMiddleClick();
    EXPECT_EQ(document->getObjectById("thin")->style->stroke.getColor().toString(false), "#ff0000");
    EXPECT_EQ(document->getObjectById("thin")->style->fill, fill);
    one_undo(removed, xml());
}

TEST_F(StrokeWidthStatusbarTest, BUG012StrokeOpaqueUsesStrokeUndoLabel)
{
    select("thin");
    document->getObjectById("thin")->getRepr()->setAttribute("style", "fill:none;stroke:#123456;stroke-opacity:0.5;stroke-width:2");
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), Util::Internal::ContextString("Setup"), "");
    DocumentUndo::clearUndo(document.get()); DocumentUndo::clearRedo(document.get());
    auto const before = xml();
    activate(find_item(widget->strokeMenu(), "Make Stroke Opaque"));
    expect_label("Make stroke opaque");
    EXPECT_DOUBLE_EQ(document->getObjectById("thin")->style->stroke_opacity.value, SP_SCALE24_MAX);
    one_undo(before, xml());
}

TEST_F(StrokeWidthStatusbarTest, TextToolPresetKeepsExplicitReversedSubrange)
{
    auto *text = dynamic_cast<SPText *>(document->getObjectById("text")); ASSERT_TRUE(text);
    select("text"); desktop->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool()); ASSERT_TRUE(tool);
    tool->text_sel_end = text->layout.begin(); tool->text_sel_end.nextCharacter();
    tool->text_sel_start = tool->text_sel_end; tool->text_sel_start.nextCharacter();
    widget->update();
    auto const before = xml(); auto const zero = style("zero"), eight = style("eight");
    preset("3");
    EXPECT_NE(style("two"), "stroke-width:2");
    EXPECT_EQ(style("zero"), zero); EXPECT_EQ(style("eight"), eight);
    EXPECT_NE(style("text").find("stroke:black"), std::string::npos);
    EXPECT_NE(style("text").find("fill:none"), std::string::npos);
    EXPECT_NE(style("text").find("font-size:20px"), std::string::npos);
    EXPECT_EQ(style("text").find("stroke-width"), std::string::npos);
    EXPECT_EQ(text->layout.iteratorToCharIndex(tool->text_sel_start), 2);
    EXPECT_EQ(text->layout.iteratorToCharIndex(tool->text_sel_end), 1);
    one_undo(before, xml());
}

TEST_F(StrokeWidthStatusbarTest, RawCaretAndRangeChangeRefuseStaleMenuWithoutNotification)
{
    select("text"); desktop->setTool("/tools/text");
    auto *text = dynamic_cast<SPText *>(document->getObjectById("text")); ASSERT_TRUE(text);
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool()); ASSERT_TRUE(tool);
    for (bool caret : {false, true}) {
        tool->text_sel_start = text->layout.begin();
        tool->text_sel_end = tool->text_sel_start;
        if (!caret) tool->text_sel_end.nextCharacter();
        widget->open();
        RetainedItem old(find_item(widget->menu(), "3")); ASSERT_TRUE(old.item);
        tool->text_sel_start.nextCharacter();
        tool->text_sel_end.nextCharacter();
        auto const before = snap();
        activate(old.item); unchanged(before);
        EXPECT_FALSE(widget->opening());
    }
}

TEST_F(StrokeWidthStatusbarTest, TextToolAddingShapeDropsSubrangeAndMenuRemovesBoth)
{
    select("text"); desktop->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool()); ASSERT_TRUE(tool);
    desktop->getSelection()->add(dynamic_cast<SPItem *>(document->getObjectById("outside")));
    EXPECT_EQ(tool->textItem(), nullptr);
    widget->update(); auto const before = xml();
    preset("Remove Stroke");
    for (auto id : {"zero", "two", "eight", "outside"}) EXPECT_NE(style(id).find("stroke:none"), std::string::npos) << id;
    one_undo(before, xml());
}

TEST_F(StrokeWidthStatusbarTest, LivePathEffectPresetCommitsOneTransaction)
{
    widget->setDesktop(nullptr); host->unset_child(); desktop.reset(); document.reset();
    auto const path = std::filesystem::path(__FILE__).parent_path().parent_path() / "data/stroke-width/live-path-effect.svg";
    auto const utf8 = std::string(reinterpret_cast<char const *>(path.u8string().c_str()));
    document = SPDocument::createNewDoc(utf8.c_str(), true); ASSERT_TRUE(document);
    document->ensureUpToDate();
    desktop = std::make_unique<SPDesktop>(document->getNamedView()); host->set_child(*desktop->getCanvas());
    select("lpe");
    auto *lpe = dynamic_cast<SPLPEItem *>(document->getObjectById("lpe")); ASSERT_TRUE(lpe && lpe->hasPathEffect());
    // Settle the derived LPE geometry before capturing an exact Undo baseline.
    sp_lpe_item_update_patheffect(lpe, true, true);
    document->ensureUpToDate();
    DocumentUndo::done(document.get(), Util::Internal::ContextString("Setup LPE"), "");
    widget->setDesktop(desktop.get());
    DocumentUndo::clearUndo(document.get()); DocumentUndo::clearRedo(document.get());
    auto const before = xml(), baseline_style = style("lpe");
    preset("3");
    EXPECT_NE(style("lpe"), baseline_style);
    expect_label("Set stroke width");
    one_undo(before, xml());
}

TEST_F(StrokeWidthStatusbarTest, NonAtomicRollbackRestoresSaveFlags)
{
    auto const dirty = document->isModifiedSinceSave(), autosave = document->isModifiedSinceAutoSave();
    auto token = DocumentUndo::beginRollbackableInteraction(document.get()); ASSERT_TRUE(token);
    document->setModifiedSinceSave(!dirty);
    EXPECT_NE(document->isModifiedSinceSave(), dirty);
    EXPECT_NE(document->isModifiedSinceAutoSave(), autosave);
    token->rollback();
    EXPECT_EQ(document->isModifiedSinceSave(), dirty);
    EXPECT_EQ(document->isModifiedSinceAutoSave(), autosave);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

// A9: the status-bar paste writes the property it names, and an asynchronous result is dropped
// when the widget's desktop is no longer the one the request came from.
TEST_F(StrokeWidthStatusbarTest, PasteStrokeWritesStrokeAndFillWritesFill)
{
    select("outside");
    auto const before = xml();
    ASSERT_TRUE(widget->paste("#ff0000", "stroke", desktop.get()));
    document->ensureUpToDate();
    auto *outside = document->getObjectById("outside");
    EXPECT_EQ(outside->style->stroke.getColor().toString(), "#ff0000");
    EXPECT_TRUE(outside->style->fill.isNone()) << "a stroke paste must not touch the fill: " << style("outside");
    auto const after_stroke = xml();
    ASSERT_NE(after_stroke, before);
    ASSERT_TRUE(DocumentUndo::undo(document.get())) << "one Paste stroke step";
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));

    ASSERT_TRUE(widget->paste("#00ff00", "fill", desktop.get()));
    document->ensureUpToDate();
    EXPECT_EQ(outside->style->fill.getColor().toString(), "#00ff00");
    EXPECT_EQ(outside->style->stroke.getColor().toString(), "#aabbcc") << "a fill paste must not touch the stroke";
}

TEST_F(StrokeWidthStatusbarTest, PasteResultIsDroppedWhenTheDesktopChanged)
{
    select("outside");
    auto const before = xml();
    SPDesktop *other = reinterpret_cast<SPDesktop *>(uintptr_t(0x1000)); // never dereferenced
    EXPECT_FALSE(widget->paste("#ff0000", "stroke", other)) << "request from another tab";
    auto *target = desktop.get();
    widget->setDesktop(nullptr);
    EXPECT_FALSE(widget->paste("#ff0000", "stroke", target)) << "widget no longer has a desktop";
    EXPECT_FALSE(widget->paste("#ff0000", "stroke", nullptr));
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(StrokeWidthStatusbarTest, DelayedPasteRefusesChangedSelectionAndSwitchBack)
{
    select("outside");
    auto const generation = widget->generation();
    auto const before = xml();
    desktop->getSelection()->clear();
    select("outside");
    EXPECT_FALSE(widget->delayedPaste("#ff0000", "stroke", desktop.get(), generation));
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));

    auto const next = widget->generation();
    widget->setDesktop(nullptr);
    widget->setDesktop(desktop.get());
    EXPECT_FALSE(widget->delayedPaste("#ff0000", "fill", desktop.get(), next));
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(StrokeWidthStatusbarTest, DelayedPasteRefusesReplacedDocument)
{
    select("outside");
    auto const generation = widget->generation();
    auto const before = xml();
    auto replacement = SPDocument::createNewDocFromMem(before);
    ASSERT_TRUE(replacement);
    desktop->setDocument(replacement.get());
    auto const replacement_xml = sp_repr_save_buf(replacement->getReprDoc());
    EXPECT_FALSE(widget->delayedPaste("#ff0000", "stroke", desktop.get(), generation));
    EXPECT_EQ(sp_repr_save_buf(replacement->getReprDoc()), replacement_xml);
    EXPECT_FALSE(DocumentUndo::undo(replacement.get()));
    desktop->setDocument(document.get());
    EXPECT_FALSE(widget->delayedPaste("#ff0000", "stroke", desktop.get(), generation));
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

// A1: "Change Image" must apply the chooser result only to the image it was started for.
struct ChangeImageFixture {
    explicit ChangeImageFixture(SPDocument &document)
    {
        auto *bitmap = document.getObjectById("bitmap");
        auto *repr = bitmap->getRepr();
        auto *copy = repr->duplicate(repr->document());
        copy->setAttribute("id", "bitmap2");
        copy->setAttribute("x", "60");
        repr->parent()->appendChild(copy);
        Inkscape::GC::release(copy);
        document.ensureUpToDate();
        DocumentUndo::done(&document, Util::Internal::ContextString("Duplicate image"), "");
        DocumentUndo::clearUndo(&document);
        DocumentUndo::clearRedo(&document);
        first = cast<SPImage>(bitmap);
        second = cast<SPImage>(document.getObjectById("bitmap2"));
        window.set_child(panel);
    }
    ~ChangeImageFixture()
    {
        UI::Widget::ImageProperties::set_chooser_hook_for_testing({});
        window.unset_child();
    }
    SPImage *first = nullptr, *second = nullptr;
    Gtk::Window window;
    UI::Widget::ImageProperties panel;
    std::function<void(Glib::RefPtr<Gio::File>)> finish;
    void start()
    {
        UI::Widget::ImageProperties::set_chooser_hook_for_testing(
            [this](Gtk::Window &, std::function<void(Glib::RefPtr<Gio::File>)> done) { finish = std::move(done); });
        panel.link_image();
    }
};

TEST_F(StrokeWidthStatusbarTest, ChangeImageAppliesToTheImageItWasStartedFor)
{
    ChangeImageFixture f(*document);
    ASSERT_TRUE(f.first && f.second);
    f.panel.update(f.first);
    auto const before = xml();
    f.start();
    ASSERT_TRUE(f.finish);
    EXPECT_FALSE(DocumentUndo::interactionIsQuiescent(document.get())) << "the document is held while the chooser is up";
    f.finish(Gio::File::create_for_uri("file:///nonexistent/replacement.png"));
    f.finish = nullptr;
    EXPECT_TRUE(DocumentUndo::interactionIsQuiescent(document.get())) << "the hold is released afterwards";
    EXPECT_NE(xml(), before);
    EXPECT_NE(std::string(f.first->getRepr()->attribute("xlink:href")).find("replacement.png"), std::string::npos);
    EXPECT_EQ(std::string(f.second->getRepr()->attribute("xlink:href")).find("replacement.png"), std::string::npos);
    EXPECT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(xml(), before);
}

TEST_F(StrokeWidthStatusbarTest, ChangeImageChooserResultIsIgnoredWhenSelectionWasCleared)
{
    ChangeImageFixture f(*document);
    ASSERT_TRUE(f.first);
    f.panel.update(f.first);
    f.start();
    ASSERT_TRUE(f.finish);
    f.panel.update(nullptr); // selection changed while the chooser sheet was open
    auto const before = xml();
    f.finish(Gio::File::create_for_uri("file:///nonexistent/replacement.png")); // crashed (null _image) before the fix
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(StrokeWidthStatusbarTest, ChangeImageChooserResultIsIgnoredWhenAnotherImageIsSelected)
{
    ChangeImageFixture f(*document);
    ASSERT_TRUE(f.first && f.second);
    f.panel.update(f.first);
    f.start();
    ASSERT_TRUE(f.finish);
    f.panel.update(f.second); // the panel now shows another image
    auto const before = xml();
    f.finish(Gio::File::create_for_uri("file:///nonexistent/replacement.png")); // changed the wrong image before
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(StrokeWidthStatusbarTest, ChangeImageChooserResultIsIgnoredWhenTheImageWasDeleted)
{
    ChangeImageFixture f(*document);
    ASSERT_TRUE(f.first);
    f.panel.update(f.first);
    f.start();
    ASSERT_TRUE(f.finish);
    f.first->deleteObject(); // e.g. menu-bar Delete while the sheet is open
    f.panel.update(nullptr);
    document->ensureUpToDate();
    auto const before = xml();
    f.finish(Gio::File::create_for_uri("file:///nonexistent/replacement.png"));
    EXPECT_EQ(xml(), before);
}

} // namespace
