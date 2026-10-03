// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Fill and Stroke width row and the shared stroke-width command
 * (internal note STROKE_WIDTH_CONTROLS_PLAN v2.2, sections 3 and 5).
 *
 * A GUI suite (INKSCAPE_TEST_GUI=1). It drives the real StrokeStyle panel on a
 * real SPDesktop and reads document state (XML, Undo labels, widths), never only
 * a return value. Emitting a controller signal tests our handlers, not native
 * event propagation: cases that need a desktop session are named "Desktop" in
 * a comment and only assert their headless part.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <clocale>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glib.h>
#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>

#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "event-log.h"
#include "extension/init.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "message-stack.h"
#include "object/sp-item.h"
#include "object/sp-text.h"
#include "preferences.h"
#include "selection.h"
#include "style.h"
#include "ui/stroke-width-command.h"
#include "ui/stroke-width-controller.h"
#include "ui/tools/text-tool.h"
#include "ui/util.h"
#include "ui/widget/generic/popover-menu.h"
#include "ui/widget/canvas.h"
#include "ui/widget/gtk-registry.h"
#include "ui/widget/spinbutton.h"
#include "ui/widget/stroke-style.h"
#include "ui/widget/style/paint-order.h"
#include "ui/widget/unit-menu.h"
#include "xml/node-observer.h"
#include "xml/repr.h"

using namespace Inkscape;
using Inkscape::UI::Widget::PopoverMenu;
using Inkscape::UI::Widget::PopoverMenuItem;
using Inkscape::UI::Widget::SpinButton;
using Inkscape::UI::Widget::StrokeStyle;
using Inkscape::UI::Widget::UnitMenu;

namespace {

// ---------------------------------------------------------------------------
// Application and widget lookup
// ---------------------------------------------------------------------------

InkscapeApplication *gui()
{
    static auto *app = [] {
        g_setenv("INKSCAPE_APP_ID_TAG", "strokeWidthRowTest", TRUE);
        // English msgids and "." decimals: set before any widget exists.
        g_setenv("LC_ALL", "C", TRUE);
        std::setlocale(LC_ALL, "C");
        auto *result = new InkscapeApplication();
        if (result->gtk_app()) {
            UI::Widget::register_all();
            Extension::init(); // marker previews open SVGs through the native input extension
        }
        return result;
    }();
    return app->gtk_app() ? app : nullptr;
}

template <typename T>
T *find_widget(Gtk::Widget &root)
{
    if (auto *widget = dynamic_cast<T *>(&root)) return widget;
    for (auto *child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto *widget = find_widget<T>(*child)) return widget;
    }
    return nullptr;
}

template <typename T>
T *named(Gtk::Widget &root, char const *name)
{
    if (root.get_name() == name) {
        if (auto *widget = dynamic_cast<T *>(&root)) return widget;
    }
    for (auto *child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto *widget = named<T>(*child, name)) return widget;
    }
    return nullptr;
}

SpinButton *spin_with_tooltip(Gtk::Widget &root, char const *tip)
{
    if (auto *spin = dynamic_cast<SpinButton *>(&root)) {
        if (spin->get_tooltip_text() == tip) return spin;
    }
    for (auto *child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto *spin = spin_with_tooltip(*child, tip)) return spin;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Signal emission on the real controllers
// ---------------------------------------------------------------------------

struct ControllerRefs
{
    std::vector<GObject *> items;
    ControllerRefs() = default;
    ControllerRefs(ControllerRefs const &) = delete;
    ~ControllerRefs()
    {
        for (auto *item : items) g_object_unref(item);
    }
};

/// Every controller of \a type (optionally only CAPTURE phase), newest first,
/// which is the order GTK runs them.
void collect(Gtk::Widget &widget, GType type, bool capture_only, ControllerRefs &out)
{
    auto *list = gtk_widget_observe_controllers(widget.gobj());
    for (guint i = 0; i < g_list_model_get_n_items(list); ++i) {
        auto *item = G_OBJECT(g_list_model_get_item(list, i));
        bool keep = g_type_is_a(G_OBJECT_TYPE(item), type);
        if (keep && capture_only) {
            keep = gtk_event_controller_get_propagation_phase(GTK_EVENT_CONTROLLER(item)) == GTK_PHASE_CAPTURE;
        }
        if (keep) out.items.push_back(item);
        else g_object_unref(item);
    }
    g_object_unref(list);
}

/// Emit a no-argument controller signal ("enter"/"leave" of focus, "leave" of
/// motion) on every controller of the type. Returns how many received it.
int emit_all(Gtk::Widget &widget, GType type, char const *signal)
{
    ControllerRefs refs;
    collect(widget, type, false, refs);
    for (auto *item : refs.items) g_signal_emit_by_name(item, signal);
    return static_cast<int>(refs.items.size());
}

guint keycode_of(guint keyval)
{
    GdkKeymapKey *keys = nullptr;
    int count = 0;
    gdk_display_map_keyval(gdk_display_get_default(), keyval, &keys, &count);
    guint const code = count ? keys[0].keycode : 0;
    g_free(keys);
    return code;
}

/// Emulates GTK's capture walk for a key press aimed at a spin button: the
/// widget's own CAPTURE key controllers, then its entry's. Returns "handled".
bool dispatch_key(SpinButton &field, guint keyval, guint state = 0, bool release = false)
{
    auto *entry = named<Gtk::Entry>(field, "InkSpinButton-Entry");
    ControllerRefs refs;
    collect(field, GTK_TYPE_EVENT_CONTROLLER_KEY, true, refs);
    if (entry) collect(*entry, GTK_TYPE_EVENT_CONTROLLER_KEY, true, refs);
    guint const code = keycode_of(keyval);
    gboolean handled = FALSE;
    for (auto *item : refs.items) {
        if (release) {
            g_signal_emit_by_name(item, "key-released", keyval, code, static_cast<GdkModifierType>(state));
        } else {
            g_signal_emit_by_name(item, "key-pressed", keyval, code, static_cast<GdkModifierType>(state), &handled);
            if (handled) break;
        }
    }
    return handled;
}

bool wheel(Gtk::Widget &widget, double dy)
{
    ControllerRefs refs;
    collect(widget, GTK_TYPE_EVENT_CONTROLLER_SCROLL, false, refs);
    gboolean handled = FALSE;
    for (auto *item : refs.items) {
        g_signal_emit_by_name(item, "scroll", 0.0, dy, &handled);
        if (handled) break;
    }
    return handled;
}

/// A button's "clicked" without going through the pointer (ignores sensitivity).
void click(Gtk::Widget *button)
{
    ASSERT_NE(button, nullptr);
    g_signal_emit_by_name(button->gobj(), "clicked");
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

struct MessageLog
{
    struct Entry
    {
        Inkscape::MessageType type;
        std::string text;
    };
    std::vector<Entry> entries;
    sigc::connection connection;

    void attach(SPDesktop &desktop)
    {
        connection.disconnect();
        connection = desktop.messageStack()->connectChanged([this](Inkscape::MessageType type, char const *text) {
            entries.push_back({type, text ? text : ""});
        });
    }
    ~MessageLog() { connection.disconnect(); }
    void clear() { entries.clear(); }
    std::vector<std::string> warnings() const
    {
        std::vector<std::string> result;
        for (auto const &entry : entries) {
            if (entry.type == Inkscape::WARNING_MESSAGE && !entry.text.empty()) result.push_back(entry.text);
        }
        return result;
    }
    bool has_warning_containing(std::string const &needle) const
    {
        for (auto const &text : warnings()) {
            if (text.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    std::string all() const
    {
        std::string result;
        for (auto const &entry : entries) result += "[" + std::to_string(entry.type) + "] " + entry.text + "\n";
        return result;
    }
};

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// One rectangle with a stroke width in user units.
std::string R(std::string const &id, std::string const &sw, std::string const &extra = "")
{
    return "<rect id=\"" + id + "\" x=\"0\" y=\"0\" width=\"10\" height=\"10\" "
           "style=\"fill:#0000ff;stroke:#008000;stroke-width:" + sw + "\" " + extra + "/>";
}

char const *const BMP =
    R"(<image id="bitmap" x="52" y="2" width="1" height="1" xlink:href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/lXcAAAAASUVORK5CYII="/>)";

char const *const TXT =
    R"(<text id="text" x="2" y="45" style="font-family:sans-serif;font-size:20px;fill:none;stroke:black">)"
    R"(<tspan id="zero" style="stroke-width:0">A</tspan><tspan id="two" style="stroke-width:2">B</tspan>)"
    R"(<tspan id="eight" style="stroke-width:8">C</tspan></text>)";

std::string svg(std::string const &body, bool mm = false)
{
    std::string const head =
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" )"
        R"(xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" )"
        R"(xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" )";
    if (mm) {
        return head + R"(width="200mm" height="100mm" viewBox="0 0 200 100">)"
               R"(<sodipodi:namedview id="nv" inkscape:document-units="mm"/>)" + body + "</svg>";
    }
    return head + R"(width="200" height="100">)" + body + "</svg>";
}

constexpr double MM = 96.0 / 25.4; // px per mm
constexpr double PT = 96.0 / 72.0; // px per pt
constexpr double IN = 96.0;        // px per inch

struct Step
{
    std::string label;
    std::string icon;
};

struct Snapshot
{
    std::string xml;
    bool dirty_save = false;
    bool dirty_autosave = false;
    std::string defaults;
    std::uint64_t serial = 0;
};

class RowTest : public ::testing::Test
{
public:
    // GTK must exist before the fixture's own widgets are constructed.
    static void SetUpTestSuite() { gui(); }

    void SetUp() override
    {
        if (!gui()) GTEST_SKIP() << "GTK display unavailable";
        if (!Application::exists()) Application::create(false);
    }

    void TearDown() override { close(); }

    // ---- lifecycle ----

    /// Build document, desktop and panel; select \a ids; pin the unit.
    void open(std::string const &body, std::vector<std::string> const &ids = {}, bool mm = false,
              char const *unit = "px")
    {
        close();
        document = SPDocument::createNewDocFromMem(svg(body, mm));
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        canvas_host = std::make_unique<Gtk::Window>();
        canvas_host->set_child(*desktop->getCanvas());
        selection_set(ids, false);
        // The constructor reads the ACTIVE desktop for its initial unit.
        Application::instance().add_desktop(desktop.get());
        registered = true;
        panel = std::make_unique<StrokeStyle>();
        // Match updateLine's four-parent lookup without a full dialog.
        inner.append(*panel);
        middle.append(inner);
        outer.append(middle);
        panel_host = std::make_unique<Gtk::Window>();
        panel_host->set_child(outer);
        panel_host->insert_action_group("doc", document->getActionGroup());
        panel->setDesktop(desktop.get());
        units = find_widget<UnitMenu>(*panel);
        ASSERT_TRUE(units);
        field = find_widget<SpinButton>(*units->get_parent());
        ASSERT_TRUE(field);
        entry = named<Gtk::Entry>(*field, "InkSpinButton-Entry");
        value_label = named<Gtk::Label>(*field, "InkSpinButton-Value");
        ASSERT_TRUE(entry && value_label);
        if (unit) {
            units->setUnit(unit);
            ASSERT_EQ(units->getUnitAbbr(), unit);
        }
        panel->updateLine();
        messages.attach(*desktop);
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
    }

    /// Destroy the panel (and everything that hangs on it) while the document lives on.
    void destroy_panel()
    {
        if (panel) {
            close_popovers();
            panel->setDesktop(nullptr);
        }
        if (panel_host) {
            panel_host->insert_action_group("doc", {});
            panel_host->unset_child();
        }
        if (panel && panel->get_parent() == &inner) inner.remove(*panel);
        if (inner.get_parent() == &middle) middle.remove(inner);
        if (middle.get_parent() == &outer) outer.remove(middle);
        field = nullptr;
        entry = nullptr;
        value_label = nullptr;
        units = nullptr;
        panel.reset();
    }

    void close()
    {
        messages.connection.disconnect();
        if (entry) entry->set_visible(false); // never commit on a teardown focus-leave
        // A widgetless test desktop cannot scroll: a live text tool would crash on the
        // queued selection-modified idle when the loop runs below.
        if (desktop && dynamic_cast<UI::Tools::TextTool *>(desktop->getTool())) desktop->setTool("/tools/select");
        drain();
        destroy_panel();
        panel_host.reset();
        if (canvas_host_b) canvas_host_b->unset_child();
        canvas_host_b.reset();
        if (registered_b) Application::instance().remove_desktop(desktop_b.get());
        registered_b = false;
        desktop_b.reset();
        document_b.reset();
        if (canvas_host) canvas_host->unset_child();
        canvas_host.reset();
        if (registered) Application::instance().remove_desktop(desktop.get());
        registered = false;
        desktop.reset();
        document.reset();
    }

    void close_popovers();

    // ---- popover ----

    template <typename F>
    static bool wait_until(F &&predicate, double seconds = 5.0)
    {
        auto context = Glib::MainContext::get_default();
        auto const deadline = g_get_monotonic_time() + static_cast<gint64>(seconds * G_USEC_PER_SEC);
        while (!predicate() && g_get_monotonic_time() < deadline) {
            while (context->pending()) context->iteration(false);
            g_usleep(2000);
        }
        return predicate();
    }

    Gtk::MenuButton *menu_button() { return dynamic_cast<Gtk::MenuButton *>(presets()); }
    PopoverMenu *popover() { return menu_button() ? dynamic_cast<PopoverMenu *>(menu_button()->get_popover()) : nullptr; }

    /// Open the preset list (the window must be presented) and return its items.
    std::vector<Gtk::Widget *> open_presets()
    {
        auto *button = menu_button();
        EXPECT_TRUE(button && button->get_popover());
        if (!button || !popover()) return {};
        button->popup();
        EXPECT_TRUE(wait_until([&] { return popover()->get_mapped(); })) << "the preset list did not open";
        return popover()->get_items();
    }

    static std::string item_label(Gtk::Widget *item)
    {
        auto *check = item ? find_widget<Gtk::CheckButton>(*item) : nullptr;
        return check ? check->get_label().raw() : "";
    }

    static bool item_checked(Gtk::Widget *item)
    {
        auto *check = item ? find_widget<Gtk::CheckButton>(*item) : nullptr;
        return check && check->get_active();
    }

    std::vector<std::string> item_labels(std::vector<Gtk::Widget *> const &items)
    {
        std::vector<std::string> labels;
        for (auto *item : items) labels.push_back(item_label(item));
        return labels;
    }

    Gtk::Widget *item_named(std::vector<Gtk::Widget *> const &items, std::string const &label)
    {
        for (auto *item : items) {
            if (item_label(item) == label) return item;
        }
        ADD_FAILURE() << "no preset item labelled " << label;
        return nullptr;
    }

    static void activate(Gtk::Widget *item)
    {
        ASSERT_NE(item, nullptr);
        g_signal_emit_by_name(item->gobj(), "clicked");
    }

    bool popover_closed()
    {
        return popover() && wait_until([&] { return !popover()->get_mapped(); });
    }

    void close_presets()
    {
        if (auto *list = popover()) list->popdown();
    }

    /// A second desktop on its own document, registered as the active one.
    void open_second_desktop(std::string const &body, std::vector<std::string> const &ids)
    {
        document_b = SPDocument::createNewDocFromMem(svg(body));
        ASSERT_TRUE(document_b);
        document_b->ensureUpToDate();
        desktop_b = std::make_unique<SPDesktop>(document_b->getNamedView());
        canvas_host_b = std::make_unique<Gtk::Window>();
        canvas_host_b->set_child(*desktop_b->getCanvas());
        std::vector<SPItem *> list;
        for (auto const &id : ids) list.push_back(cast<SPItem>(document_b->getObjectById(id)));
        desktop_b->getSelection()->setList(list);
        Application::instance().add_desktop(desktop_b.get());
        registered_b = true;
    }

    std::string xml_b() const { return sp_repr_save_buf(document_b->getReprDoc()).raw(); }

    /// Type text into the entry without committing (what a user does before clicking + or -).
    void pend(std::string const &text)
    {
        entry->set_text(text);
        entry->set_visible(true);
    }

    /// Run pending idle callbacks (enter_edit queues a focus request with a raw this).
    static void drain()
    {
        auto context = Glib::MainContext::get_default();
        for (int i = 0; i < 128 && context->pending(); ++i) context->iteration(false);
    }

    /// Map the panel (needed for routes that test is_visible(), for popovers and focus).
    void present()
    {
        panel_host->present();
        auto context = Glib::MainContext::get_default();
        auto const deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
        while (!field->get_mapped() && g_get_monotonic_time() < deadline) {
            while (context->pending()) context->iteration(false);
            g_usleep(5000);
        }
        ASSERT_TRUE(field->get_mapped()) << "the panel window did not map within 10 s";
        drain();
        // The window's first focusable widget is the field, which opens its entry.
        // Close it and clear the focus, so no real keystroke can land in the
        // panel and no routed focus-leave commit fires later.
        entry->set_visible(false);
        panel_host->unset_focus();
        drain();
    }

    // ---- selection ----

    std::vector<SPItem *> items(std::vector<std::string> const &ids) const
    {
        std::vector<SPItem *> result;
        for (auto const &id : ids) {
            auto *item = cast<SPItem>(document->getObjectById(id));
            EXPECT_TRUE(item) << "no item " << id;
            if (item) result.push_back(item);
        }
        return result;
    }

    void selection_set(std::vector<std::string> const &ids, bool notify)
    {
        auto list = items(ids);
        if (list.empty()) desktop->getSelection()->clear();
        else desktop->getSelection()->setList(list);
        if (notify && panel) panel->selectionChangedCB();
    }

    void select(std::vector<std::string> const &ids) { selection_set(ids, true); }

    // ---- reading the document ----

    std::string xml() const { return sp_repr_save_buf(document->getReprDoc()).raw(); }

    std::string attr(char const *id, char const *name = "style") const
    {
        auto *object = document->getObjectById(id);
        auto const *value = object && object->getRepr() ? object->getRepr()->attribute(name) : nullptr;
        return value ? value : "";
    }

    /// Effective stroke width in document CSS px.
    double px_width(char const *id) const
    {
        document->ensureUpToDate();
        auto *object = document->getObjectById(id);
        EXPECT_TRUE(object && object->style) << id;
        if (!object || !object->style) return -1;
        double const scale = is<SPItem>(object) ? cast<SPItem>(object)->i2doc_affine().descrim() : 1.0;
        return object->style->stroke_width.computed * scale;
    }

    bool hairline(char const *id) const
    {
        auto *object = document->getObjectById(id);
        return object && object->style && object->style->stroke_extensions.hairline;
    }

    Snapshot snap() const
    {
        Snapshot s;
        s.xml = xml();
        s.dirty_save = document->isModifiedSinceSave();
        s.dirty_autosave = document->isModifiedSinceAutoSave();
        s.defaults = Preferences::get()->getString("/desktop/style").raw();
        s.serial = document->get_event_log()->getCurrEventSerial();
        return s;
    }

    /// No XML, dirty flag, default style or history change since \a before.
    void expect_same(Snapshot const &before, std::string const &what = "") const
    {
        auto const now = snap();
        EXPECT_EQ(now.xml, before.xml) << what;
        EXPECT_EQ(now.dirty_save, before.dirty_save) << what;
        EXPECT_EQ(now.dirty_autosave, before.dirty_autosave) << what;
        EXPECT_EQ(now.defaults, before.defaults) << what;
        EXPECT_EQ(now.serial, before.serial) << what << ": history step added";
    }

    Step current_step() const
    {
        auto const &columns = EventLog::getColumns();
        auto row = document->get_event_log()->getCurrEvent();
        return {Glib::ustring((*row)[columns.description]).raw(), Glib::ustring((*row)[columns.icon_name]).raw()};
    }

    /// Undo labels, newest first; Undo all, then Redo the same number of times.
    std::vector<std::string> undo_labels()
    {
        std::vector<std::string> labels;
        while (true) {
            auto const label = current_step().label;
            if (!DocumentUndo::undo(document.get())) break;
            labels.push_back(label);
        }
        for (std::size_t i = 0; i < labels.size(); ++i) EXPECT_TRUE(DocumentUndo::redo(document.get()));
        return labels;
    }

    // ---- widgets ----

    std::string label_text() const { return value_label->get_text().raw(); }
    std::string tooltip(Gtk::Widget *widget) const { return widget ? widget->get_tooltip_text().raw() : ""; }
    Gtk::Widget *dec() { return named<Gtk::Widget>(*panel, "StrokeWidth-Decrease"); }
    Gtk::Widget *inc() { return named<Gtk::Widget>(*panel, "StrokeWidth-Increase"); }
    Gtk::Widget *presets() { return named<Gtk::Widget>(*panel, "StrokeWidth-Presets"); }

    /// What a user does: type \a text into the open entry and press Enter.
    bool type(std::string const &text)
    {
        entry->set_text(text);
        entry->set_visible(true);
        bool const handled = dispatch_key(*field, GDK_KEY_Return);
        entry->set_visible(false);
        return handled;
    }

    /// Open the entry with its own text (a click into the field) and nothing typed.
    void open_entry()
    {
        entry->set_visible(true);
    }

    void unit(char const *abbr)
    {
        units->setUnit(abbr);
        ASSERT_EQ(units->getUnitAbbr(), abbr);
    }

    void unit_hairline() { units->set_selected(units->get_item_count() - 1); }

    // ---- state ----

    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<SPDocument> document_b;
    std::unique_ptr<SPDesktop> desktop_b;
    std::unique_ptr<Gtk::Window> canvas_host_b;
    std::unique_ptr<Gtk::Window> canvas_host, panel_host;
    Gtk::Box outer, middle, inner;
    std::unique_ptr<StrokeStyle> panel;
    UnitMenu *units = nullptr;
    SpinButton *field = nullptr;
    Gtk::Entry *entry = nullptr;
    Gtk::Label *value_label = nullptr;
    MessageLog messages;
    bool registered = false;
    bool registered_b = false;
};

void RowTest::close_popovers()
{
    if (panel) panel->closePopovers();
}

// ===========================================================================
// CH: characterization of the typed commit, Mixed, unit, Hairline and % paths.
// These pass on the committed code and must still pass after the row change.
// ===========================================================================

TEST_F(RowTest, CH01_TypedCommitUniform)
{
    open(R("a", "2"), {"a"});
    ASSERT_FALSE(field->is_mixed());
    EXPECT_DOUBLE_EQ(field->get_value(), 2);
    EXPECT_EQ(label_text(), "2");
    auto const before = xml();
    type("5");
    EXPECT_NEAR(px_width("a"), 5, 1e-6);
    auto const style = attr("a");
    EXPECT_NE(style.find("fill:#0000ff"), std::string::npos) << style;
    EXPECT_NE(style.find("stroke:#008000"), std::string::npos) << style;
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Set stroke width"}));
    EXPECT_EQ(current_step().icon, "dialog-fill-and-stroke");
    EXPECT_EQ(label_text(), "5");
    EXPECT_DOUBLE_EQ(field->get_value(), 5);
    auto const after = xml();
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    EXPECT_EQ(xml(), after);
}

TEST_F(RowTest, CH02_TypedUnitExpression)
{
    open(R("a", "2"), {"a"});
    type("1.5 mm");
    EXPECT_NEAR(px_width("a"), 1.5 * MM, 1e-6);
    EXPECT_EQ(undo_labels().size(), 1u);
}

TEST_F(RowTest, CH03_EnterOnExactlyDisplayedTextIsNoOp)
{
    open(R("a", "2"), {"a"});
    auto const before = snap();
    open_entry();
    ASSERT_EQ(entry->get_text(), "2");
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_Return));
    entry->set_visible(false);
    expect_same(before);
    EXPECT_TRUE(undo_labels().empty());
}

TEST_F(RowTest, CH04_MixedPlaceholder)
{
    open(R("a", "2") + R("b", "8"), {"a", "b"});
    EXPECT_TRUE(field->is_mixed());
    EXPECT_TRUE(field->has_css_class("mixed"));
    EXPECT_EQ(label_text(), "\xE2\x80\x94");
    EXPECT_EQ(entry->get_text(), "");
    EXPECT_NE(tooltip(field).find("Mixed"), std::string::npos) << tooltip(field);
    EXPECT_TRUE(field->is_sensitive());
    EXPECT_DOUBLE_EQ(field->get_value(), 1.0) << "the hidden number keeps its previous value";
    auto const before = snap();
    panel->updateLine();
    expect_same(before);
}

TEST_F(RowTest, CH05_SameNumberCommitFromMixed)
{
    open(R("a", "2") + R("b", "8"), {"a"});
    ASSERT_DOUBLE_EQ(field->get_value(), 2);
    select({"a", "b"});
    ASSERT_TRUE(field->is_mixed());
    auto const before = xml();
    auto const style_a = attr("a");
    type("2");
    EXPECT_NEAR(px_width("a"), 2, 1e-6);
    EXPECT_NEAR(px_width("b"), 2, 1e-6);
    EXPECT_EQ(attr("a"), style_a) << "the member already at 2 must not be renormalised";
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Set stroke width"}));
    EXPECT_FALSE(field->is_mixed());
    EXPECT_EQ(label_text(), "2");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(xml(), before);
}

TEST_F(RowTest, CH06_DisplayOnlyUnitChange)
{
    open(R("a", "2"), {"a"});
    struct Expect
    {
        char const *unit;
        double value;
        char const *label;
    };
    auto const before = snap();
    for (auto const &step : {Expect{"mm", 2 / MM, "0.529"}, Expect{"pt", 1.5, "1.5"}, Expect{"in", 2 / IN, "0.021"},
                             Expect{"px", 2, "2"}}) {
        unit(step.unit);
        panel->updateLine();
        EXPECT_NEAR(field->get_value(), step.value, 1e-6) << step.unit;
        EXPECT_EQ(label_text(), step.label) << step.unit;
        expect_same(before, step.unit);
    }
    // Mixed variant: the unit change keeps the placeholder.
    open(R("a", "2") + R("b", "8"), {"a", "b"});
    auto const mixed_before = snap();
    unit("mm");
    EXPECT_TRUE(field->is_mixed());
    EXPECT_EQ(label_text(), "\xE2\x80\x94");
    expect_same(mixed_before);
}

TEST_F(RowTest, CH07_HairlineUnitItem)
{
    open(R("a", "2") + R("b", "3"), {"a", "b"});
    auto const original = xml();
    unit_hairline();
    EXPECT_TRUE(hairline("a") && hairline("b"));
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Set stroke width"}));
    EXPECT_FALSE(field->get_sensitive());
    EXPECT_DOUBLE_EQ(field->get_value(), 1);
    EXPECT_EQ(tooltip(field), "Hairline stroke");
    EXPECT_FALSE(field->is_mixed());
    auto const hairline_state = snap();
    panel->updateLine();
    expect_same(hairline_state);
    // Leaving the dimensionless unit re-queries without writing; the uniform
    // hairline shows an indeterminate entry that a typed number converts.
    unit("mm");
    expect_same(hairline_state);
    EXPECT_TRUE(field->is_mixed());
    EXPECT_EQ(tooltip(field), "Hairline stroke; enter a number to convert");
    EXPECT_TRUE(field->get_sensitive());
    EXPECT_DOUBLE_EQ(field->get_value(), 1);
    type("1");
    EXPECT_FALSE(hairline("a"));
    EXPECT_NEAR(px_width("a"), MM, 1e-6);
    EXPECT_NEAR(px_width("b"), MM, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Set stroke width", "Set stroke width"}));
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(xml(), original);
}

TEST_F(RowTest, CH08_PercentRelativeCommit)
{
    open(R("a", "4") + R("b", "4"), {"a", "b"});
    auto const before = snap();
    unit("%");
    EXPECT_DOUBLE_EQ(field->get_value(), 100);
    EXPECT_FALSE(field->is_mixed());
    expect_same(before);
    type("50");
    EXPECT_NEAR(px_width("a"), 2, 1e-6);
    EXPECT_NEAR(px_width("b"), 2, 1e-6);
    EXPECT_DOUBLE_EQ(field->get_value(), 100) << "the % display returns to 100 after a commit";
    EXPECT_EQ(units->getUnitAbbr(), "%");
    EXPECT_EQ(undo_labels().size(), 1u);
    type("200");
    EXPECT_NEAR(px_width("a"), 4, 1e-6);
    EXPECT_NEAR(px_width("b"), 4, 1e-6);
    EXPECT_EQ(undo_labels().size(), 2u);
    auto const settled = snap();
    type("100");
    expect_same(settled, "100 % is the current width");
    EXPECT_EQ(undo_labels().size(), 2u);

    // Mixed: % keeps the differences and the placeholder.
    open(R("a", "2") + R("b", "8"), {"a", "b"});
    unit("%");
    EXPECT_TRUE(field->is_mixed());
    type("200");
    EXPECT_NEAR(px_width("a"), 4, 1e-6);
    EXPECT_NEAR(px_width("b"), 16, 1e-6);
    EXPECT_TRUE(field->is_mixed());
    unit("px");
    EXPECT_TRUE(field->is_mixed());
}

// A synchronous selection change during the write invalidates the captured scope.
struct GenerationBumper final : XML::NodeObserver
{
    std::function<void()> action;
    unsigned calls = 0;
    XML::Node *node = nullptr;
    void notifyAttributeChanged(XML::Node &, GQuark key, Util::ptr_shared, Util::ptr_shared) override
    {
        if (key != g_quark_from_static_string("style") || calls) return;
        ++calls;
        action();
    }
};

/// Watches a node: counts every attribute and child change and runs \a action once, on the first.
struct WriteObserver final : XML::NodeObserver
{
    std::function<void()> action;
    unsigned events = 0;
    bool fired = false;
    void note()
    {
        ++events;
        if (fired) return;
        fired = true;
        if (action) action();
    }
    void notifyAttributeChanged(XML::Node &, GQuark, Util::ptr_shared, Util::ptr_shared) override { note(); }
    void notifyChildAdded(XML::Node &, XML::Node &, XML::Node *) override { note(); }
    void notifyChildRemoved(XML::Node &, XML::Node &, XML::Node *) override { note(); }
};

TEST_F(RowTest, CH09_StaleScopeDuringWriteRollsBack)
{
    open(R("a", "2"), {"a"});
    auto const before = snap();
    GenerationBumper observer;
    observer.action = [&] { panel->selectionChangedCB(); };
    auto *repr = document->getObjectById("a")->getRepr();
    repr->addObserver(observer);
    messages.clear();
    type("5");
    repr->removeObserver(observer);
    EXPECT_EQ(observer.calls, 1u);
    expect_same(before);
    EXPECT_TRUE(undo_labels().empty());
    EXPECT_EQ(messages.warnings().size(), 1u) << messages.all();
    EXPECT_TRUE(messages.has_warning_containing("rolled back")) << messages.all();
}

TEST_F(RowTest, CH10_CommitActsOnTheLiveSelection)
{
    open(R("a", "2") + R("b", "4"), {"a"});
    selection_set({"b"}, false); // no selectionChangedCB
    type("5");
    EXPECT_NEAR(px_width("b"), 5, 1e-6);
    EXPECT_NEAR(px_width("a"), 2, 1e-6);
}

std::string group_fixture()
{
    return "<g id=\"g\">" + R("thin", "2") +
           R"(<rect id="prot" x="0" y="0" width="10" height="10" sodipodi:insensitive="true" )"
           R"(style="fill:none;stroke:black;stroke-width:3"/>)" + BMP + "</g>";
}

TEST_F(RowTest, CH11_PartialCompatibility)
{
    open(group_fixture(), {"g"});
    auto const prot = attr("prot");
    auto const bitmap_before = std::string(document->getObjectById("bitmap")->getRepr()->attribute("xlink:href"));
    auto const group_style = attr("g");
    messages.clear();
    type("5");
    EXPECT_NEAR(px_width("thin"), 5, 1e-6);
    EXPECT_EQ(attr("prot"), prot);
    EXPECT_EQ(std::string(document->getObjectById("bitmap")->getRepr()->attribute("xlink:href")), bitmap_before);
    EXPECT_EQ(attr("g"), group_style);
    EXPECT_EQ(attr("g").find("stroke-width"), std::string::npos);
    EXPECT_EQ(undo_labels().size(), 1u);
    EXPECT_TRUE(messages.has_warning_containing("incompatible or protected items were skipped")) << messages.all();
}

TEST_F(RowTest, CH12_NoEligibleMember)
{
    open(BMP, {"bitmap"});
    EXPECT_FALSE(field->is_sensitive());
    EXPECT_EQ(tooltip(field), "No eligible shape or text with a stroke width in the selection");
    auto const before = snap();
    messages.clear();
    type("5"); // the key path ignores sensitivity
    expect_same(before);
    EXPECT_TRUE(messages.has_warning_containing("Stroke width")) << messages.all();
    // Empty selection: the whole table is insensitive.
    open(R("a", "2"), {"a"});
    select({});
    EXPECT_FALSE(field->is_sensitive());
}

TEST_F(RowTest, CH13_RefreshPathsNeverWrite)
{
    open(R("a", "2"), {"a"});
    auto const before = snap();
    panel->updateLine();
    panel->selectionChangedCB();
    panel->selectionModifiedCB(SP_OBJECT_STYLE_MODIFIED_FLAG);
    panel->selectionModifiedCB(0);
    panel->setDesktop(desktop.get());
    expect_same(before, "same-desktop refreshes");
    panel->setDesktop(nullptr);
    panel->setDesktop(desktop.get());
    expect_same(before, "desktop rebinding");
    for (auto abbr : {"mm", "pt", "%", "px", "in"}) {
        unit(abbr);
        panel->updateLine();
        expect_same(before, abbr);
    }
    EXPECT_TRUE(undo_labels().empty());
}

// ===========================================================================
// B22p: the width field's opt-ins must not leak to the shared spin button.
// The miter-limit spin in the same panel keeps every default.
// ===========================================================================

TEST_F(RowTest, B22p_MiterSpinKeepsSharedDefaults)
{
    open(R("a", "2"), {"a"});
    auto *miter = spin_with_tooltip(*panel, "Maximum length of the miter (in units of stroke width)");
    ASSERT_TRUE(miter);
    ASSERT_DOUBLE_EQ(miter->get_value(), 4);
    // Up steps by the adjustment step and writes the selection.
    EXPECT_TRUE(dispatch_key(*miter, GDK_KEY_Up));
    EXPECT_NEAR(miter->get_value(), 4.1, 1e-9);
    EXPECT_NE(attr("a").find("stroke-miterlimit:4.1"), std::string::npos) << attr("a");
    // The wheel is handled and steps.
    auto const before_wheel = miter->get_value();
    EXPECT_TRUE(wheel(*miter, -5.0));
    EXPECT_GT(miter->get_value(), before_wheel);
    // Hover arrows appear.
    auto *plus = named<Gtk::Widget>(*miter, "InkSpinButton-Plus");
    ASSERT_TRUE(plus);
    ControllerRefs motion;
    collect(*miter, GTK_TYPE_EVENT_CONTROLLER_MOTION, false, motion);
    ASSERT_FALSE(motion.items.empty());
    for (auto *item : motion.items) g_signal_emit_by_name(item, "enter", 1.0, 1.0);
    EXPECT_TRUE(plus->get_visible());
    for (auto *item : motion.items) g_signal_emit_by_name(item, "leave");
    // Ctrl+Z restores the focus-in value through the wrapper.
    emit_all(*miter, GTK_TYPE_EVENT_CONTROLLER_FOCUS, "enter");
    auto const seeded = miter->get_value();
    miter->set_value(seeded + 3);
    EXPECT_TRUE(dispatch_key(*miter, GDK_KEY_z, GDK_CONTROL_MASK));
    EXPECT_NEAR(miter->get_value(), seeded, 1e-9);
    // Typed 5 commits and writes.
    auto *miter_entry = named<Gtk::Entry>(*miter, "InkSpinButton-Entry");
    ASSERT_TRUE(miter_entry);
    miter_entry->set_text("5");
    miter_entry->set_visible(true);
    EXPECT_TRUE(dispatch_key(*miter, GDK_KEY_Return));
    miter_entry->set_visible(false);
    EXPECT_NEAR(miter->get_value(), 5, 1e-9);
    EXPECT_NE(attr("a").find("stroke-miterlimit:5"), std::string::npos) << attr("a");
}

// ===========================================================================
// B: the new buttons, keys, focus and state matrix (plan section 7 B01-B21)
// ===========================================================================

TEST_F(RowTest, B01_MinusInsensitiveAtTheFloor)
{
    open(R("a", "0.05"), {"a"}, true, "mm");
    ASSERT_TRUE(dec() && inc() && presets());
    EXPECT_FALSE(dec()->is_sensitive());
    EXPECT_FALSE(tooltip(dec()).empty());
    EXPECT_NE(tooltip(dec()), tooltip(inc()));
    EXPECT_TRUE(inc()->is_sensitive());
    EXPECT_TRUE(presets()->is_sensitive());
    auto const before = snap();
    click(dec()); // a direct signal ignores sensitivity; the floor still holds
    expect_same(before);
}

TEST_F(RowTest, B02_PlusFromTheExactValueNotTheRoundedText)
{
    open(R("a", "0.66666667"), {"a"}, false, "in"); // 0.5 pt, shown as 0.007 in
    ASSERT_TRUE(inc());
    auto const before = xml();
    click(inc());
    EXPECT_NEAR(px_width("a"), 0.66666667 + 0.001 * IN, 1e-6)
        << "the rounded text route would give 0.768 px";
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width"}));
    EXPECT_EQ(current_step().icon, "dialog-fill-and-stroke");
    EXPECT_EQ(label_text(), "0.008");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(xml(), before);
}

TEST_F(RowTest, B03a_EnterOnTheAutoFilledTextDoesNothing)
{
    open(R("a", "0.66666667"), {"a"}, false, "in");
    open_entry();
    ASSERT_EQ(entry->get_text(), "0.007");
    auto const before = snap();
    dispatch_key(*field, GDK_KEY_Return);
    entry->set_visible(false);
    expect_same(before);
}

TEST_F(RowTest, B03b_LeavingFocusOnTheAutoFilledTextDoesNothing)
{
    open(R("a", "0.66666667"), {"a"}, false, "in");
    present();
    open_entry();
    auto const before = snap();
    EXPECT_GT(emit_all(*field, GTK_TYPE_EVENT_CONTROLLER_FOCUS, "leave"), 0);
    expect_same(before);
}

TEST_F(RowTest, B03c_LeavingThePointerOnTheAutoFilledTextDoesNothing)
{
    open(R("a", "0.66666667"), {"a"}, false, "in");
    present();
    open_entry();
    auto const before = snap();
    EXPECT_GT(emit_all(*field, GTK_TYPE_EVENT_CONTROLLER_MOTION, "leave"), 0);
    expect_same(before);
}

TEST_F(RowTest, B03d_PlusWithTheEntryOpenIsOneStepFromTheExactValue)
{
    open(R("a", "0.66666667"), {"a"}, false, "in");
    ASSERT_TRUE(inc());
    open_entry();
    click(inc());
    EXPECT_NEAR(px_width("a"), 0.66666667 + 0.001 * IN, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width"}));
}

TEST_F(RowTest, B04a_MixedBlankEntryThenPlusIsAnAdditiveStep)
{
    open(R("a", "2") + R("b", "8"), {"a", "b"});
    ASSERT_TRUE(inc());
    pend("");
    messages.clear();
    click(inc());
    EXPECT_NEAR(px_width("a"), 2.1, 1e-6);
    EXPECT_NEAR(px_width("b"), 8.1, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width"}));
    EXPECT_TRUE(messages.warnings().empty()) << messages.all();
    EXPECT_TRUE(field->is_mixed()) << "the differences are kept";
}

TEST_F(RowTest, B04b_MixedUpKeyIsTheSameStep)
{
    open(R("a", "2") + R("b", "8"), {"a", "b"});
    pend("");
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_Up));
    EXPECT_NEAR(px_width("a"), 2.1, 1e-6);
    EXPECT_NEAR(px_width("b"), 8.1, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width"}));
}

TEST_F(RowTest, B05a_InvalidPendingTextThenPlusWritesNothing)
{
    for (std::string const bad : {"abc", ".", "1e999", "-2", "5000000", ""}) {
        SCOPED_TRACE("text '" + bad + "'");
        open(R("a", "2"), {"a"});
        ASSERT_TRUE(inc());
        pend(bad);
        auto const before = snap();
        messages.clear();
        click(inc());
        expect_same(before);
        EXPECT_EQ(messages.warnings().size(), 1u) << messages.all();
        EXPECT_TRUE(messages.has_warning_containing("Stroke width")) << messages.all();
        EXPECT_TRUE(entry->get_visible());
        EXPECT_EQ(entry->get_text(), bad);
        EXPECT_DOUBLE_EQ(field->get_value(), 2) << "the widget must not clamp";
    }
}

TEST_F(RowTest, B05b_InvalidTypedTextWithEnterIsReportedNotClamped)
{
    for (std::string const bad : {"abc", "-2", "5000000", "1e999", "1,000"}) {
        SCOPED_TRACE("text '" + bad + "'");
        open(R("a", "2"), {"a"});
        auto const before = snap();
        messages.clear();
        type(bad);
        expect_same(before);
        EXPECT_EQ(messages.warnings().size(), 1u) << messages.all();
        EXPECT_TRUE(messages.has_warning_containing("Stroke width")) << messages.all();
    }
}

TEST_F(RowTest, B05c_ARejectedTextIsReportedOncePerText)
{
    // Focus-out and every later pointer-leave commit the same rejected text again.
    open(R("a", "2"), {"a"});
    present();
    pend("abc");
    messages.clear();
    emit_all(*field, GTK_TYPE_EVENT_CONTROLLER_FOCUS, "leave");
    emit_all(*field, GTK_TYPE_EVENT_CONTROLLER_MOTION, "leave");
    emit_all(*field, GTK_TYPE_EVENT_CONTROLLER_MOTION, "leave");
    drain();
    EXPECT_EQ(messages.warnings().size(), 1u) << messages.all();
    EXPECT_TRUE(entry->get_visible()) << "the rejected text stays for correction";
    pend("abd");
    emit_all(*field, GTK_TYPE_EVENT_CONTROLLER_FOCUS, "leave");
    drain();
    EXPECT_EQ(messages.warnings().size(), 2u) << "a different text is a new report: " << messages.all();
}

TEST_F(RowTest, E09_DecimalSeparatorsAndTheAmbiguousComma)
{
    open(R("a", "2"), {"a"});
    for (auto const &[text, expected] : {std::pair<std::string, double>{"0,5", 0.5}, {",5", 0.5}, {".5", 0.5},
                                         {"0.5", 0.5}, {"1,25 mm", 1.25 * MM}, {"1.25 mm", 1.25 * MM}}) {
        SCOPED_TRACE(text);
        open(R("a", "2"), {"a"});
        type(text);
        EXPECT_NEAR(px_width("a"), expected, 1e-6);
    }
    open(R("a", "2"), {"a"});
    auto const before = snap();
    messages.clear();
    type("1,000");
    expect_same(before);
    ASSERT_EQ(messages.warnings().size(), 1u) << messages.all();
    EXPECT_NE(messages.warnings()[0].find("ambiguous"), std::string::npos) << messages.warnings()[0];
}

TEST_F(RowTest, B06_PendingValidTextIsCommittedThenStepped)
{
    open(R("a", "2"), {"a"});
    ASSERT_TRUE(inc());
    auto const before = xml();
    pend("2.5");
    click(inc());
    EXPECT_NEAR(px_width("a"), 2.6, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width", "Set stroke width"}));
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_NEAR(px_width("a"), 2.5, 1e-6);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(xml(), before);
}

TEST_F(RowTest, B07_RejectedPendingCommitStopsTheStep)
{
    open(R("src", "2") + R"(<use id="missing" xlink:href="#nope" x="20"/>)", {"src", "missing"});
    ASSERT_TRUE(inc());
    pend("2.5");
    auto const before = snap();
    messages.clear();
    click(inc());
    expect_same(before);
    EXPECT_EQ(messages.warnings().size(), 1u) << messages.all();
    EXPECT_TRUE(messages.has_warning_containing("linked clone")) << messages.all();
    EXPECT_NEAR(px_width("src"), 2, 1e-9);
}

TEST_F(RowTest, B07b_ARolledBackCommitIsNeverFollowedByAPhantomWrite)
{
    // Uniform only by tolerance: a phantom write of the restored 2 would renormalise b.
    open(R("a", "2") + R("b", "2.0005"), {"a", "b"});
    auto const before = snap();
    GenerationBumper observer;
    observer.action = [&] { panel->selectionChangedCB(); };
    auto *repr = document->getObjectById("a")->getRepr();
    repr->addObserver(observer);
    messages.clear();
    type("5");
    repr->removeObserver(observer);
    EXPECT_EQ(observer.calls, 1u);
    expect_same(before);
    EXPECT_EQ(messages.warnings().size(), 1u) << messages.all();
    EXPECT_EQ(attr("b"), std::string("fill:#0000ff;stroke:#008000;stroke-width:2.0005"));
    // The next deliberate commit of a different number still works.
    type("3");
    EXPECT_NEAR(px_width("a"), 3, 1e-6);
    EXPECT_NEAR(px_width("b"), 3, 1e-6);
    EXPECT_EQ(undo_labels().size(), 1u);
}

TEST_F(RowTest, B08a_SelectionChangeDiscardsPendingText)
{
    open(R("a", "2") + R("b", "4"), {"a"});
    ASSERT_TRUE(inc());
    pend("9");
    select({"b"});
    EXPECT_FALSE(entry->get_visible()) << "the pending text is discarded with the old selection";
    click(inc());
    EXPECT_NEAR(px_width("b"), 4.1, 1e-6);
    EXPECT_NEAR(px_width("a"), 2, 1e-9);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width"}));
}

TEST_F(RowTest, B08b_DocumentReplacementDiscardsPendingText)
{
    open(R("a", "2"), {"a"});
    ASSERT_TRUE(inc());
    pend("9");
    auto replacement = SPDocument::createNewDocFromMem(svg(R("a", "2")));
    ASSERT_TRUE(replacement);
    auto const before = xml();
    desktop->setDocument(replacement.get());
    EXPECT_FALSE(entry->get_visible());
    EXPECT_EQ(xml(), before) << "nothing is written into the old document";
    desktop->setDocument(document.get());
    select({"a"});
    click(inc());
    EXPECT_NEAR(px_width("a"), 2.1, 1e-6) << "the discarded 9 must not be committed";
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width"}));
}

TEST_F(RowTest, B08c_DesktopChangeDiscardsPendingText)
{
    open(R("a", "2"), {"a"});
    ASSERT_TRUE(inc());
    open_second_desktop(R("a", "2"), {"a"});
    pend("9");
    auto const before_a = xml();
    panel->setDesktop(desktop_b.get());
    EXPECT_FALSE(entry->get_visible());
    click(inc());
    EXPECT_EQ(xml(), before_a) << "desktop A must not receive B's step";
    EXPECT_NEAR(px_width("a"), 2, 1e-9);
    auto *b = document_b->getObjectById("a");
    ASSERT_TRUE(b && b->style);
    EXPECT_NEAR(b->style->stroke_width.computed, 2.1, 1e-6);
    panel->setDesktop(desktop.get());
}

TEST_F(RowTest, B09a_UndoAndRedoKeysGoToTheDocumentNotTheFieldHistory)
{
    open(R("a", "2"), {"a"});
    auto const before = xml();
    click(inc());
    auto const after = xml();
    ASSERT_NE(before, after);
    emit_all(*field, GTK_TYPE_EVENT_CONTROLLER_FOCUS, "enter"); // seeds the focus-in value
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_z, GDK_CONTROL_MASK));
    EXPECT_EQ(xml(), before) << "Undo is the document's, and no stale value is re-committed";
    EXPECT_TRUE(undo_labels().empty());
    panel->selectionModifiedCB(SP_OBJECT_STYLE_MODIFIED_FLAG); // what the dialog forwards
    EXPECT_EQ(label_text(), "2");
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_z, GDK_CONTROL_MASK | GDK_SHIFT_MASK));
    EXPECT_EQ(xml(), after);
    panel->selectionModifiedCB(SP_OBJECT_STYLE_MODIFIED_FLAG);
    EXPECT_EQ(label_text(), "2.1");
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_y, GDK_CONTROL_MASK)); // nothing left to redo
    EXPECT_EQ(xml(), after);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width"}));
}

TEST_F(RowTest, B09b_UndoKeyNeverFlattensAMixedSelection)
{
    open(R("a", "2") + R("b", "8"), {"a", "b"});
    click(inc());
    EXPECT_NEAR(px_width("a"), 2.1, 1e-6);
    emit_all(*field, GTK_TYPE_EVENT_CONTROLLER_FOCUS, "enter");
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_z, GDK_CONTROL_MASK));
    EXPECT_NEAR(px_width("a"), 2, 1e-9);
    EXPECT_NEAR(px_width("b"), 8, 1e-9);
    EXPECT_TRUE(undo_labels().empty()) << "no new step from the key";
    panel->selectionModifiedCB(SP_OBJECT_STYLE_MODIFIED_FLAG);
    EXPECT_TRUE(field->is_mixed());
}

TEST_F(RowTest, B09c_PendingTextIsDiscardedAndFocusReturnsToTheCanvas)
{
    open(R("a", "2"), {"a"});
    auto *canvas = desktop->getCanvas();
    canvas->set_focusable(true); // the CanvasGrid does this in a real window
    canvas_host->present();
    present();
    ASSERT_TRUE(wait_until([&] { return canvas->get_mapped(); })) << "the canvas window did not map";
    field->grab_focus();
    drain();
    ASSERT_TRUE(UI::contains_focus(*field)) << "the field must hold focus for this test to mean anything";
    pend("7");
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_z, GDK_CONTROL_MASK));
    drain();
    EXPECT_FALSE(entry->get_visible()) << "the pending text is discarded";
    EXPECT_FALSE(UI::contains_focus(*field)) << "focus must not stay on a hidden entry";
    auto *root = gtk_widget_get_root(GTK_WIDGET(canvas->gobj()));
    ASSERT_TRUE(root);
    EXPECT_EQ(gtk_root_get_focus(root), GTK_WIDGET(canvas->gobj())) << "the canvas (and the text caret) gets focus";
}

TEST_F(RowTest, B10_HeldKeyStepsOncePerPress)
{
    open(R("a", "3"), {"a"});
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_Up));
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_Up)) << "an auto-repeat is swallowed";
    dispatch_key(*field, GDK_KEY_Up, 0, true);
    EXPECT_NEAR(px_width("a"), 3.1, 1e-6);
    EXPECT_EQ(undo_labels().size(), 1u);
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_Up));
    dispatch_key(*field, GDK_KEY_Up, 0, true);
    EXPECT_NEAR(px_width("a"), 3.2, 1e-6);
    EXPECT_EQ(undo_labels().size(), 2u);
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_Down));
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_Down)) << "an auto-repeat of Down is swallowed";
    dispatch_key(*field, GDK_KEY_Down, 0, true);
    EXPECT_NEAR(px_width("a"), 3.1, 1e-6);
    EXPECT_TRUE(dispatch_key(*field, GDK_KEY_KP_Down)) << "a new physical press steps again";
    dispatch_key(*field, GDK_KEY_KP_Down, 0, true);
    EXPECT_NEAR(px_width("a"), 3.0, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Decrease stroke width", "Decrease stroke width",
                                                       "Increase stroke width", "Increase stroke width"}));
    // Desktop session only: Enter or Space held on a focused button (GTK re-arms
    // activation after 250 ms). Headless, ten emitted clicks give ten steps (B15a).
}

TEST_F(RowTest, B10b_AHeldEnterOrSpaceOnAButtonActivatesOnce)
{
    // The capture controller of each button swallows repeats until release. GTK's own
    // 250 ms re-arm is a desktop-session check; this proves our guard.
    open(R("a", "2"), {"a"});
    for (auto *button : {dec(), inc(), presets()}) {
        ASSERT_TRUE(button);
        ControllerRefs keys;
        collect(*button, GTK_TYPE_EVENT_CONTROLLER_KEY, true, keys);
        ASSERT_FALSE(keys.items.empty()) << button->get_name();
        for (guint keyval : {guint(GDK_KEY_Return), guint(GDK_KEY_space)}) {
            auto press = [&] {
                gboolean handled = FALSE;
                for (auto *key : keys.items) {
                    g_signal_emit_by_name(key, "key-pressed", keyval, keycode_of(keyval), static_cast<GdkModifierType>(0),
                                          &handled);
                    if (handled) break;
                }
                return handled != FALSE;
            };
            EXPECT_FALSE(press()) << button->get_name() << ": the first press activates";
            EXPECT_TRUE(press()) << button->get_name() << ": a repeat while held is swallowed";
            EXPECT_TRUE(press());
            for (auto *key : keys.items) g_signal_emit_by_name(key, "key-released", keyval, keycode_of(keyval), static_cast<GdkModifierType>(0));
            EXPECT_FALSE(press()) << button->get_name() << ": pressed again after release";
            for (auto *key : keys.items) g_signal_emit_by_name(key, "key-released", keyval, keycode_of(keyval), static_cast<GdkModifierType>(0));
        }
        EXPECT_FALSE(button->get_name().empty());
    }
}

TEST_F(RowTest, B11_ModifiedAndPageKeysNeverChangeTheWidth)
{
    open(R("a", "2"), {"a"});
    auto const before = snap();
    messages.clear();
    for (guint mods : {guint(GDK_SHIFT_MASK), guint(GDK_CONTROL_MASK), guint(GDK_ALT_MASK), guint(GDK_META_MASK)}) {
        for (guint key : {guint(GDK_KEY_Up), guint(GDK_KEY_Down), guint(GDK_KEY_KP_Up), guint(GDK_KEY_KP_Down)}) {
            EXPECT_FALSE(dispatch_key(*field, key, mods)) << "key " << key << " mods " << mods;
            dispatch_key(*field, key, mods, true);
        }
    }
    for (guint key : {guint(GDK_KEY_Page_Up), guint(GDK_KEY_Page_Down), guint(GDK_KEY_Home), guint(GDK_KEY_End)}) {
        EXPECT_FALSE(dispatch_key(*field, key)) << "key " << key;
    }
    expect_same(before);
    EXPECT_TRUE(messages.warnings().empty()) << messages.all();
    open(R("a", "2") + R("b", "8"), {"a", "b"});
    auto const mixed = snap();
    EXPECT_FALSE(dispatch_key(*field, GDK_KEY_Page_Up));
    EXPECT_FALSE(dispatch_key(*field, GDK_KEY_Up, GDK_SHIFT_MASK));
    expect_same(mixed);
}

TEST_F(RowTest, B12_WheelAndDragDoNothingAndClickStillEdits)
{
    open(R("a", "2"), {"a"});
    auto const before = snap();
    EXPECT_FALSE(wheel(*field, -5.0));
    EXPECT_FALSE(wheel(*field, 5.0));
    EXPECT_DOUBLE_EQ(field->get_value(), 2);
    expect_same(before);
    // Click-to-edit is kept: a drag that does not move takes the click path.
    ControllerRefs drags;
    collect(*value_label, GTK_TYPE_GESTURE_DRAG, false, drags);
    ASSERT_FALSE(drags.items.empty());
    for (auto *drag : drags.items) g_signal_emit_by_name(drag, "end", nullptr);
    EXPECT_TRUE(entry->get_visible());
    drain(); // the click path queues a focus request with a raw this
    entry->set_visible(false);
    expect_same(before);
    // Desktop session only: real wheel and touchpad delivery, real drag offsets, panel scrolling.
}

TEST_F(RowTest, B13_ButtonsNeverTakeKeyboardFocus)
{
    open(R("a", "2"), {"a"});
    for (auto *button : {dec(), inc(), presets()}) {
        ASSERT_TRUE(button);
        EXPECT_FALSE(button->get_focus_on_click());
        EXPECT_TRUE(button->get_can_focus()) << "keyboard users still tab to it";
    }
    // Desktop session only: a real press with the canvas or the text caret focused.
}

TEST_F(RowTest, B14a_StructureNamesIconsAndAccessibleLabels)
{
    open(R("a", "2"), {"a"});
    ASSERT_TRUE(dec() && inc() && presets());
    std::vector<Gtk::Widget *> row;
    for (auto *child = units->get_parent()->get_first_child(); child; child = child->get_next_sibling()) {
        row.push_back(child);
    }
    ASSERT_EQ(row.size(), 5u);
    EXPECT_EQ(row[0], dec());
    EXPECT_EQ(row[1], field);
    EXPECT_EQ(row[2], presets());
    EXPECT_EQ(row[3], inc());
    EXPECT_EQ(row[4], units);
    EXPECT_EQ(dynamic_cast<Gtk::Button *>(dec())->get_icon_name(), "list-remove-symbolic");
    EXPECT_EQ(dynamic_cast<Gtk::Button *>(inc())->get_icon_name(), "list-add-symbolic");
    ASSERT_TRUE(menu_button());
    EXPECT_EQ(menu_button()->get_icon_name(), "pan-down-symbolic");
    ASSERT_TRUE(popover());
    EXPECT_EQ(popover()->get_name(), "StrokeWidth-PresetList");
    for (auto const &[widget, label] : {std::pair<Gtk::Widget *, char const *>{dec(), "Decrease stroke width"},
                                        {inc(), "Increase stroke width"},
                                        {presets(), "Preset stroke widths"}}) {
        char *mismatch = gtk_test_accessible_check_property(GTK_ACCESSIBLE(widget->gobj()),
                                                            GTK_ACCESSIBLE_PROPERTY_LABEL, label);
        EXPECT_EQ(mismatch, nullptr) << label << ": " << (mismatch ? mismatch : "");
        g_free(mismatch);
    }
    EXPECT_NE(tooltip(inc()).find("0.1 px"), std::string::npos) << tooltip(inc());
    EXPECT_NE(tooltip(dec()).find("0.1 px"), std::string::npos) << tooltip(dec());
    EXPECT_EQ(field->get_name(), "StrokeWidth-Field");
}

TEST_F(RowTest, B14b_ButtonMetrics)
{
    // Soft, theme and font dependent. Clipping and scaling (E11) are desktop-session checks.
    open(R("a", "2"), {"a"});
    ASSERT_TRUE(dec() && inc() && presets());
    int const field_height = field->measure(Gtk::Orientation::VERTICAL, -1).sizes.natural;
    for (auto *button : {dec(), inc(), presets()}) {
        EXPECT_GE(button->measure(Gtk::Orientation::HORIZONTAL, -1).sizes.natural, 22);
        EXPECT_NEAR(button->measure(Gtk::Orientation::VERTICAL, -1).sizes.natural, field_height, 3);
    }
}

TEST_F(RowTest, B15a_TenClicksAreTenSteps)
{
    open(R("a", "2"), {"a"});
    ASSERT_TRUE(inc());
    auto const before = xml();
    for (int i = 0; i < 10; ++i) click(inc());
    EXPECT_NEAR(px_width("a"), 3.0, 1e-6);
    auto const labels = undo_labels();
    EXPECT_EQ(labels, std::vector<std::string>(10, "Increase stroke width"));
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(RowTest, B15b_AClickDuringACommandIsIgnored)
{
    open(R("a", "2"), {"a"});
    ASSERT_TRUE(inc());
    GenerationBumper observer;
    observer.action = [&] { click(inc()); };
    auto *repr = document->getObjectById("a")->getRepr();
    repr->addObserver(observer);
    click(inc());
    repr->removeObserver(observer);
    EXPECT_EQ(observer.calls, 1u);
    EXPECT_NEAR(px_width("a"), 2.1, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width"}));
    EXPECT_EQ(label_text(), "2.1");
}

TEST_F(RowTest, B16a_WidthAboveTheOldFieldRange)
{
    open(R("a", "5000"), {"a"});
    ASSERT_TRUE(inc() && dec());
    EXPECT_GE(field->get_adjustment()->get_upper(), 5000);
    EXPECT_DOUBLE_EQ(field->get_value(), 5000);
    EXPECT_EQ(label_text(), "5000");
    click(inc());
    EXPECT_NEAR(px_width("a"), 5000.1, 1e-6);
    click(dec());
    EXPECT_NEAR(px_width("a"), 5000.0, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Decrease stroke width", "Increase stroke width"}));
}

TEST_F(RowTest, B16b_LargeWidthInMillimetres)
{
    open(R("a", "2000"), {"a"}, true, "mm");
    ASSERT_TRUE(inc() && dec());
    EXPECT_DOUBLE_EQ(field->get_value(), 2000);
    click(inc());
    EXPECT_NEAR(px_width("a"), 2000.05 * MM, 1e-5);
    click(dec());
    EXPECT_NEAR(px_width("a"), 2000.0 * MM, 1e-5);
}

TEST_F(RowTest, B16c_OverRangeTypingIsRejectedAndNeverClamped)
{
    open(R("a", "2"), {"a"});
    auto const before = snap();
    messages.clear();
    type("2000000");
    expect_same(before);
    EXPECT_EQ(messages.warnings().size(), 1u) << messages.all();
    open(R("a", "2"), {"a"}, false, "mm");
    auto const mm_before = snap();
    messages.clear();
    type("300 m"); // 1.13e6 px
    expect_same(mm_before);
    EXPECT_EQ(messages.warnings().size(), 1u) << messages.all();
    EXPECT_NEAR(px_width("a"), 2, 1e-9);
}

TEST_F(RowTest, B16d_TypingBetweenTheOldBoundAndTheMaximumIsAccepted)
{
    open(R("a", "2"), {"a"});
    type("5000");
    EXPECT_NEAR(px_width("a"), 5000, 1e-6);
    open(R("a", "2"), {"a"}, false, "mm");
    type("250000"); // below 1e6 px = 264583 mm
    EXPECT_NEAR(px_width("a"), 250000 * MM, 1e-3);
}

TEST_F(RowTest, B17_TinyWidth)
{
    open(R("a", "0.000001"), {"a"});
    ASSERT_TRUE(inc() && dec());
    EXPECT_FALSE(dec()->is_sensitive());
    EXPECT_EQ(label_text(), "0");
    click(inc());
    EXPECT_NEAR(px_width("a"), 0.100001, 1e-7);
    EXPECT_EQ(undo_labels().size(), 1u);
}

TEST_F(RowTest, B18_UniformByToleranceUsesTheFirstValue)
{
    open(R("a", "2") + R("b", "2.0005"), {"a", "b"});
    ASSERT_TRUE(inc());
    auto const roots = items({"a", "b"});
    auto const queried = UI::query_stroke_widths(*document, roots);
    ASSERT_EQ(queried.state, UI::StrokeWidthQuery::Uniform);
    ASSERT_TRUE(queried.uniform_px);
    click(inc());
    EXPECT_NEAR(px_width("a"), *queried.uniform_px + 0.1, 1e-6);
    EXPECT_NEAR(px_width("b"), *queried.uniform_px + 0.1, 1e-6);
    EXPECT_EQ(undo_labels().size(), 1u);
}

TEST_F(RowTest, B19a_TheClickUsesTheUnitAtClickTime)
{
    open(R("a", "2"), {"a"}, false, "mm");
    ASSERT_TRUE(inc());
    unit("in"); // no refresh call in between
    EXPECT_NE(tooltip(inc()).find("0.001 in"), std::string::npos) << tooltip(inc());
    EXPECT_EQ(field->get_digits(), 3);
    click(inc());
    EXPECT_NEAR(px_width("a"), 2 + 0.001 * IN, 1e-6);
}

TEST_F(RowTest, B19b_FromPercentToPixels)
{
    open(R("a", "2"), {"a"});
    ASSERT_TRUE(inc() && dec());
    unit("%");
    EXPECT_FALSE(inc()->is_sensitive());
    unit("px");
    EXPECT_TRUE(inc()->is_sensitive());
    EXPECT_TRUE(dec()->is_sensitive());
    click(inc());
    EXPECT_NEAR(px_width("a"), 2.1, 1e-6);
}

TEST_F(RowTest, B19c_FromHairlineToMillimetres)
{
    open(R("a", "2"), {"a"});
    ASSERT_TRUE(inc());
    unit_hairline();
    ASSERT_TRUE(hairline("a"));
    unit("mm");
    EXPECT_TRUE(inc()->is_sensitive());
    EXPECT_FALSE(dec()->is_sensitive());
    click(inc()); // the smallest preset of the mm list: 0.1 mm
    EXPECT_FALSE(hairline("a"));
    EXPECT_NEAR(px_width("a"), 0.1 * MM, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width", "Set stroke width"}));
}

char const *const HAIRLINE_RECT =
    R"(<rect id="a" x="0" y="0" width="10" height="10" style="fill:#0000ff;stroke:#008000;stroke-width:2;-inkscape-stroke:hairline"/>)";

TEST_F(RowTest, B20a_PercentDisablesStepsAndAPresetSwitchesTheUnit)
{
    open(R("a", "4"), {"a"}, true, nullptr); // the initial linear unit is the document's mm
    ASSERT_TRUE(inc() && dec() && presets());
    ASSERT_EQ(units->getUnitAbbr(), "mm");
    unit("%");
    EXPECT_FALSE(dec()->is_sensitive());
    EXPECT_FALSE(inc()->is_sensitive());
    EXPECT_NE(tooltip(dec()).find("absolute units only"), std::string::npos) << tooltip(dec());
    EXPECT_NE(tooltip(inc()).find("absolute units only"), std::string::npos) << tooltip(inc());
    EXPECT_TRUE(presets()->is_sensitive());
    present();
    auto items = open_presets();
    ASSERT_EQ(items.size(), 12u) << "the mm list";
    activate(item_named(items, "0.25"));
    EXPECT_NEAR(px_width("a"), 0.25 * MM, 1e-6);
    EXPECT_EQ(units->getUnitAbbr(), "mm") << "the unit switches after the write scope closed";
    EXPECT_NEAR(field->get_value(), 0.25, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Set stroke width"}));
    // A preset that changes nothing leaves the unit alone.
    unit("%");
    items = open_presets();
    auto const same = snap();
    activate(item_named(items, "0.25"));
    expect_same(same);
    EXPECT_EQ(units->getUnitAbbr(), "%");
}

TEST_F(RowTest, B20a2_PercentInAPixelDocumentUsesThePointList)
{
    open(R("a", "4"), {"a"});
    unit("%");
    present();
    auto const items = open_presets();
    ASSERT_EQ(items.size(), 13u);
    activate(item_named(items, "1"));
    EXPECT_NEAR(px_width("a"), 1 * PT, 1e-6);
    EXPECT_EQ(units->getUnitAbbr(), "pt") << "the list unit of the last linear unit (px)";
}

TEST_F(RowTest, B20b_HairlineUnitItem)
{
    open(R("a", "2") + R("c", "3"), {"a"});
    ASSERT_TRUE(inc() && dec() && presets());
    unit_hairline();
    ASSERT_TRUE(hairline("a"));
    select({"c"});
    ASSERT_EQ(units->get_selected(), units->get_item_count() - 1) << "the unit stays Hairline";
    EXPECT_FALSE(dec()->is_sensitive());
    EXPECT_FALSE(inc()->is_sensitive());
    EXPECT_TRUE(presets()->is_sensitive());
    present();
    auto items = open_presets();
    activate(item_named(items, "1"));
    EXPECT_NEAR(px_width("c"), 1 * PT, 1e-6);
    EXPECT_EQ(units->getUnitAbbr(), "pt") << "the unit switches to the list unit";
    // Choosing Hairline keeps the Hairline unit.
    open(R("a", "2") + R("c", "3"), {"a"});
    unit_hairline();
    select({"c"});
    present();
    items = open_presets();
    ASSERT_FALSE(items.empty());
    activate(items.front());
    EXPECT_TRUE(hairline("c"));
    EXPECT_EQ(units->get_selected(), units->get_item_count() - 1);
}

TEST_F(RowTest, B20c_UniformHairlineWithANumericUnit)
{
    open(HAIRLINE_RECT, {"a"});
    ASSERT_TRUE(inc() && dec() && presets());
    EXPECT_TRUE(field->is_mixed()) << "indeterminate: there is no number";
    EXPECT_FALSE(dec()->is_sensitive());
    EXPECT_TRUE(inc()->is_sensitive());
    EXPECT_TRUE(presets()->is_sensitive());
    present();
    auto items = open_presets();
    ASSERT_FALSE(items.empty());
    EXPECT_TRUE(item_checked(items.front())) << "Hairline is checked";
    close_presets();
    ASSERT_TRUE(popover_closed());
    click(inc());
    EXPECT_FALSE(hairline("a"));
    EXPECT_NEAR(px_width("a"), 0.25 * PT, 1e-6) << "the smallest preset of the pt list";
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width"}));
}

void expect_nothing_writable(RowTest &t, char const *what)
{
    SCOPED_TRACE(what);
    ASSERT_TRUE(t.dec() && t.inc() && t.presets());
    for (auto *widget : {static_cast<Gtk::Widget *>(t.field), t.dec(), t.inc(), t.presets()}) {
        EXPECT_FALSE(widget->is_sensitive()) << widget->get_name();
    }
    for (auto *button : {t.dec(), t.inc(), t.presets()}) {
        EXPECT_FALSE(t.tooltip(button).empty()) << "a reason for " << button->get_name();
    }
    auto const before = t.snap();
    click(t.dec());
    click(t.inc());
    t.expect_same(before, what);
    EXPECT_TRUE(t.undo_labels().empty());
}

TEST_F(RowTest, B21a_OnlyABitmap)
{
    open(BMP, {"bitmap"});
    expect_nothing_writable(*this, "bitmap");
}

TEST_F(RowTest, B21b_OnlyLockedOrHiddenMembers)
{
    open(R"(<rect id="locked" x="0" y="0" width="10" height="10" sodipodi:insensitive="true" )"
         R"(style="fill:none;stroke:#008000;stroke-width:2"/>)", {"locked"});
    expect_nothing_writable(*this, "locked");
    open(R"(<rect id="hidden" x="0" y="0" width="10" height="10" )"
         R"(style="display:none;fill:none;stroke:#008000;stroke-width:2"/>)", {"hidden"});
    expect_nothing_writable(*this, "hidden");
}

TEST_F(RowTest, B21c_OnlyALayer)
{
    open(R"(<g id="layer" inkscape:groupmode="layer" inkscape:label="Layer">)" + R("inlayer", "2") + "</g>",
         {"layer"});
    expect_nothing_writable(*this, "layer");
}

TEST_F(RowTest, B21d_EmptySelection)
{
    open(R("a", "2"), {});
    expect_nothing_writable(*this, "empty selection");
}

TEST_F(RowTest, B21e_OnlyACloneWithoutItsSource)
{
    open(R("src", "2") + R"(<use id="u" xlink:href="#src" x="20"/>)", {"u"});
    expect_nothing_writable(*this, "clone alone");
}

// ===========================================================================
// S: the state matrix
// ===========================================================================

TEST_F(RowTest, S02_FloorBandAtWidgetLevel)
{
    struct Case
    {
        char const *width;
        bool can_decrease;
    };
    for (auto const &c : {Case{"0.1", false}, Case{"0.15", false}, Case{"0.199999", false}, Case{"0.2", true},
                          Case{"0.3", true}}) {
        SCOPED_TRACE(std::string("px ") + c.width);
        open(R("a", c.width), {"a"});
        ASSERT_TRUE(dec());
        EXPECT_EQ(dec()->is_sensitive(), c.can_decrease);
        auto const before = xml();
        double const w = px_width("a");
        click(dec());
        if (c.can_decrease) EXPECT_NEAR(px_width("a"), w - 0.1, 1e-6);
        else EXPECT_EQ(xml(), before);
    }
    for (auto const &c : {Case{"0.05", false}, Case{"0.075", false}, Case{"0.0999", false}, Case{"0.1", true},
                          Case{"0.15", true}}) {
        SCOPED_TRACE(std::string("mm ") + c.width);
        open(R("a", c.width), {"a"}, true, "mm");
        ASSERT_TRUE(dec());
        EXPECT_EQ(dec()->is_sensitive(), c.can_decrease);
        auto const before = xml();
        double const w = px_width("a");
        click(dec());
        if (c.can_decrease) EXPECT_NEAR(px_width("a"), w - 0.05 * MM, 1e-6);
        else EXPECT_EQ(xml(), before);
    }
}

TEST_F(RowTest, S03a_MixedMatrixWithNoDecreasableMember)
{
    open(R("a", "0.1") + R("b", "0.15"), {"a", "b"});
    ASSERT_TRUE(dec() && inc());
    EXPECT_TRUE(field->is_mixed());
    EXPECT_FALSE(dec()->is_sensitive());
    EXPECT_TRUE(inc()->is_sensitive());
}

TEST_F(RowTest, S03b_MixedMinusChangesOnlyMembersThatCanDecrease)
{
    open(R("a", "0.1") + R("b", "0.15") + R("c", "0.3"), {"a", "b", "c"});
    ASSERT_TRUE(dec());
    EXPECT_TRUE(dec()->is_sensitive());
    auto const style_a = attr("a");
    auto const style_b = attr("b");
    click(dec());
    EXPECT_EQ(attr("a"), style_a);
    EXPECT_EQ(attr("b"), style_b);
    EXPECT_NEAR(px_width("c"), 0.2, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Decrease stroke width"}));
}

TEST_F(RowTest, S03c_MixedPlusSkipsAndReportsHairlineMembers)
{
    open(R("a", "2") + R"(<rect id="h" x="0" y="0" width="10" height="10" )"
         R"(style="fill:none;stroke:#008000;stroke-width:1;-inkscape-stroke:hairline"/>)", {"a", "h"});
    ASSERT_TRUE(inc());
    auto const style_h = attr("h");
    messages.clear();
    click(inc());
    EXPECT_NEAR(px_width("a"), 2.1, 1e-6);
    EXPECT_EQ(attr("h"), style_h);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width"}));
    EXPECT_TRUE(messages.has_warning_containing("incompatible or protected items were skipped")) << messages.all();
}

TEST_F(RowTest, S04_StepDigitsAndTooltipsPerUnit)
{
    struct Case
    {
        char const *unit;
        char const *step;
        double step_px;
        int digits;
    };
    for (auto const &c : {Case{"px", "0.1", 0.1, 3}, Case{"pt", "0.1", 0.1 * PT, 3}, Case{"pc", "0.01", 0.01 * 16, 3},
                          Case{"mm", "0.05", 0.05 * MM, 3}, Case{"cm", "0.005", 0.005 * 10 * MM, 3},
                          Case{"in", "0.001", 0.001 * IN, 3}, Case{"m", "0.00005", 0.00005 * 1000 * MM, 5},
                          Case{"ft", "0.0001", 0.0001 * 1152, 6}}) {
        SCOPED_TRACE(c.unit);
        open(R("a", "2"), {"a"}, false, c.unit);
        ASSERT_TRUE(inc() && dec());
        EXPECT_EQ(field->get_digits(), c.digits);
        auto const text = std::string(c.step) + " " + c.unit;
        EXPECT_NE(tooltip(inc()).find(text), std::string::npos) << tooltip(inc());
        EXPECT_NE(tooltip(dec()).find(text), std::string::npos) << tooltip(dec());
        click(inc());
        EXPECT_NEAR(px_width("a"), 2 + c.step_px, 1e-6);
    }
    // % and Hairline keep the last linear unit's digits and step.
    open(R("a", "2"), {"a"}, false, "m");
    unit("%");
    EXPECT_EQ(field->get_digits(), 5);
    EXPECT_NE(tooltip(presets()).find("Preset"), std::string::npos) << tooltip(presets());
    unit_hairline();
    EXPECT_EQ(field->get_digits(), 5);
}

TEST_F(RowTest, S05_EveryUnitChangeBranchRefreshesTheRowWithoutWriting)
{
    open(R("a", "2"), {"a"});
    ASSERT_TRUE(dec() && inc() && presets());
    auto const start = snap();
    unit("mm"); // display-only
    EXPECT_TRUE(dec()->is_sensitive());
    EXPECT_TRUE(inc()->is_sensitive());
    EXPECT_NE(tooltip(inc()).find("0.05 mm"), std::string::npos) << tooltip(inc());
    EXPECT_EQ(field->get_digits(), 3);
    unit("%");
    EXPECT_FALSE(dec()->is_sensitive());
    EXPECT_FALSE(inc()->is_sensitive());
    EXPECT_TRUE(presets()->is_sensitive());
    EXPECT_NE(tooltip(inc()).find("absolute units only"), std::string::npos) << tooltip(inc());
    unit("px"); // from a dimensionless unit: re-query
    EXPECT_TRUE(dec()->is_sensitive());
    EXPECT_TRUE(inc()->is_sensitive());
    EXPECT_NE(tooltip(inc()).find("0.1 px"), std::string::npos) << tooltip(inc());
    expect_same(start, "unit changes never write");
    unit_hairline(); // the one explicit write
    EXPECT_TRUE(hairline("a"));
    EXPECT_EQ(undo_labels().size(), 1u);
    EXPECT_FALSE(dec()->is_sensitive());
    EXPECT_FALSE(inc()->is_sensitive());
    EXPECT_TRUE(presets()->is_sensitive());
    EXPECT_FALSE(field->is_sensitive());
    unit("mm"); // Hairline to a numeric unit
    EXPECT_FALSE(dec()->is_sensitive());
    EXPECT_TRUE(inc()->is_sensitive());
    EXPECT_TRUE(presets()->is_sensitive());
    EXPECT_TRUE(field->is_sensitive());
    EXPECT_NE(tooltip(inc()).find("smallest"), std::string::npos) << tooltip(inc());
    EXPECT_EQ(undo_labels().size(), 1u) << "no refresh added a step";
}

TEST_F(RowTest, S06a_WritableMembersDecideSensitivity)
{
    open(R("a", "2") + R"(<rect id="prot" x="0" y="0" width="10" height="10" sodipodi:insensitive="true" )"
         R"(style="fill:none;stroke:black;stroke-width:3"/>)" + BMP, {"a", "prot", "bitmap"});
    ASSERT_TRUE(dec() && inc() && presets());
    EXPECT_TRUE(inc()->is_sensitive());
    EXPECT_TRUE(presets()->is_sensitive());
    auto const prot = attr("prot");
    auto const bitmap = std::string(document->getObjectById("bitmap")->getRepr()->attribute("xlink:href"));
    messages.clear();
    click(inc());
    EXPECT_NEAR(px_width("a"), 2.1, 1e-6);
    EXPECT_EQ(attr("prot"), prot);
    EXPECT_EQ(std::string(document->getObjectById("bitmap")->getRepr()->attribute("xlink:href")), bitmap);
    EXPECT_TRUE(messages.has_warning_containing("incompatible or protected items were skipped")) << messages.all();
}

TEST_F(RowTest, S06b_ACloneFollowsItsSelectedSource)
{
    open(R("src", "2") + R"(<use id="u" xlink:href="#src" x="20"/>)", {"src", "u"});
    ASSERT_TRUE(inc());
    EXPECT_TRUE(inc()->is_sensitive());
    auto const clone = attr("u", "style") + "|" + attr("u", "x") + "|" + attr("u", "xlink:href");
    messages.clear();
    click(inc());
    EXPECT_NEAR(px_width("src"), 2.1, 1e-6);
    EXPECT_EQ(attr("u", "style") + "|" + attr("u", "x") + "|" + attr("u", "xlink:href"), clone)
        << "the clone is never written or detached";
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Increase stroke width"}));
    EXPECT_TRUE(messages.has_warning_containing("follow")) << messages.all();
}

TEST_F(RowTest, S01_PresetListsPerUnit)
{
    std::vector<std::string> const metric = {"0.1", "0.2", "0.25", "0.35", "0.5", "0.75", "1", "1.5", "2", "2.5", "3"};
    std::vector<std::string> const points = {"0.25", "0.5", "0.75", "1", "1.5", "2", "3", "4", "6", "8", "10", "12"};
    for (auto const *abbr : {"px", "pt", "pc", "in", "ft", "mm", "cm", "m"}) {
        SCOPED_TRACE(abbr);
        open(R("a", "2"), {"a"}, false, abbr);
        present();
        auto const before = snap();
        auto const items = open_presets();
        bool const is_metric = std::string(abbr) == "mm" || std::string(abbr) == "cm" || std::string(abbr) == "m";
        auto const &expected = is_metric ? metric : points;
        ASSERT_EQ(items.size(), expected.size() + 1);
        EXPECT_EQ(item_label(items.front()), units->get_string(units->get_item_count() - 1).raw())
            << "the Hairline label is the unit menu's";
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_DOUBLE_EQ(g_ascii_strtod(item_label(items[i + 1]).c_str(), nullptr),
                             g_ascii_strtod(expected[i].c_str(), nullptr)) << item_label(items[i + 1]);
        }
        expect_same(before, "opening the list writes nothing");
        close_presets();
        popover_closed();
    }
}

// ===========================================================================
// C: presets and the popover
// ===========================================================================

TEST_F(RowTest, C01_PresetEqualToTheCurrentWidthIsAControllerNoOp)
{
    open(R("a", "2"), {"a"});
    present();
    auto items = open_presets();
    activate(item_named(items, "1"));
    EXPECT_NEAR(px_width("a"), 1 * PT, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Set stroke width"}));
    ASSERT_TRUE(popover_closed());
    items = open_presets();
    auto const before = snap();
    messages.clear();
    activate(item_named(items, "1"));
    expect_same(before);
    EXPECT_TRUE(messages.warnings().empty()) << messages.all();
    EXPECT_EQ(undo_labels().size(), 1u);
}

TEST_F(RowTest, C02_CheckMarkUsesTheWidthNotTheDisplayDigits)
{
    open(R("a", "0.68") + R("b", "0.667"), {"a"}, false, "in"); // 0.51 pt
    present();
    auto items = open_presets();
    for (auto *item : items) EXPECT_FALSE(item_checked(item)) << item_label(item);
    close_presets();
    ASSERT_TRUE(popover_closed());
    select({"b"}); // 3.3e-4 px from 0.5 pt
    items = open_presets();
    for (auto *item : items) EXPECT_EQ(item_checked(item), item_label(item) == "0.5") << item_label(item);
    close_presets();
    ASSERT_TRUE(popover_closed());
    open(HAIRLINE_RECT, {"a"});
    present();
    items = open_presets();
    ASSERT_FALSE(items.empty());
    for (auto *item : items) EXPECT_EQ(item_checked(item), item == items.front()) << item_label(item);
}

TEST_F(RowTest, C03a_MetricListInCentimetresAndMetres)
{
    for (auto const &[abbr, label] : {std::pair<char const *, char const *>{"cm", "0.05"}, {"m", "0.0005"}}) {
        SCOPED_TRACE(abbr);
        open(R("a", "2"), {"a"}, false, abbr);
        present();
        auto const items = open_presets();
        activate(item_named(items, "0.5"));
        EXPECT_NEAR(px_width("a"), 0.5 * MM, 1e-6);
        EXPECT_EQ(label_text(), label);
        EXPECT_EQ(units->getUnitAbbr(), abbr) << "only % and Hairline switch the unit";
    }
}

TEST_F(RowTest, C03b_PointListInInchesAndFeet)
{
    for (auto const &[abbr, label] : {std::pair<char const *, char const *>{"in", "0.028"}, {"ft", "0.002315"}}) {
        SCOPED_TRACE(abbr);
        open(R("a", "1"), {"a"}, false, abbr);
        present();
        auto const items = open_presets();
        activate(item_named(items, "2"));
        EXPECT_NEAR(px_width("a"), 2 * PT, 1e-6);
        EXPECT_EQ(label_text(), label);
        EXPECT_EQ(units->getUnitAbbr(), abbr);
    }
}

/// Open the list, do \a event, expect the list closed, then a stale activation writes nothing.
template <typename F>
void expect_stale_activation_refused(RowTest &t, F &&event, char const *what)
{
    SCOPED_TRACE(what);
    auto items = t.open_presets();
    ASSERT_FALSE(items.empty());
    auto *kept = t.item_named(items, "1");
    ASSERT_NE(kept, nullptr);
    g_object_ref(kept->gobj());
    event();
    EXPECT_TRUE(t.popover_closed()) << "the list must close";
    auto const before = t.snap();
    auto const before_b = t.document_b ? t.xml_b() : std::string();
    RowTest::activate(kept);
    t.expect_same(before, "a stale activation");
    if (t.document_b) EXPECT_EQ(t.xml_b(), before_b);
    g_object_unref(kept->gobj());
}

TEST_F(RowTest, C04a_SelectionChangeClosesTheList)
{
    open(R("a", "2") + R("b", "4"), {"a"});
    present();
    expect_stale_activation_refused(*this, [&] { select({"b"}); }, "selection change");
}

TEST_F(RowTest, C04b_UndoAndRedoCloseTheList)
{
    open(R("a", "2"), {"a"});
    present();
    click(inc());
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    expect_stale_activation_refused(*this, [&] {
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        panel->selectionModifiedCB(SP_OBJECT_STYLE_MODIFIED_FLAG);
    }, "undo");
    expect_stale_activation_refused(*this, [&] {
        ASSERT_TRUE(DocumentUndo::redo(document.get()));
        panel->selectionModifiedCB(SP_OBJECT_STYLE_MODIFIED_FLAG);
    }, "redo");
}

TEST_F(RowTest, C04c_DocumentReplacementClosesTheList)
{
    open(R("a", "2"), {"a"});
    present();
    auto replacement = SPDocument::createNewDocFromMem(svg(R("a", "2")));
    ASSERT_TRUE(replacement);
    expect_stale_activation_refused(*this, [&] { desktop->setDocument(replacement.get()); }, "document replaced");
    EXPECT_EQ(sp_repr_save_buf(replacement->getReprDoc()).raw(),
              sp_repr_save_buf(SPDocument::createNewDocFromMem(svg(R("a", "2")))->getReprDoc()).raw());
    desktop->setDocument(document.get());
}

TEST_F(RowTest, C04d_DesktopChangeClosesTheList)
{
    open(R("a", "2"), {"a"});
    present();
    open_second_desktop(R("a", "2"), {"a"});
    expect_stale_activation_refused(*this, [&] { panel->setDesktop(desktop_b.get()); }, "desktop changed");
    panel->setDesktop(desktop.get());
}

TEST_F(RowTest, C04e_ClosePopoversClosesTheListAndRefusesStaleActivations)
{
    // The hook FillAndStroke::_onSwitchPage and the panel's unmap call.
    open(R("a", "2"), {"a"});
    present();
    expect_stale_activation_refused(*this, [&] { panel->closePopovers(); }, "closePopovers");
}

TEST_F(RowTest, C04h_HidingThePanelClosesTheListAndDiscardsPendingText)
{
    // The unmap hook: a hidden notebook page, a closed floating window, a collapsed dock.
    open(R("a", "2"), {"a"});
    present();
    expect_stale_activation_refused(*this, [&] { panel->set_visible(false); }, "panel hidden");
    panel->set_visible(true);
    ASSERT_TRUE(wait_until([&] { return panel->get_mapped(); }));
    pend("9");
    panel->set_visible(false);
    ASSERT_TRUE(wait_until([&] { return !panel->get_mapped(); }));
    EXPECT_FALSE(entry->get_visible());
    panel->set_visible(true);
    // Desktop session only: the same through the real FillAndStroke notebook (_onSwitchPage).
}

TEST_F(RowTest, C04f_UnitChangeClosesTheList)
{
    open(R("a", "2"), {"a"});
    present();
    expect_stale_activation_refused(*this, [&] { unit("mm"); }, "unit change");
}

TEST_F(RowTest, C04g_LossOfSensitivityClosesTheList)
{
    open(R("a", "2") + BMP, {"a"});
    present();
    ASSERT_TRUE(presets());
    expect_stale_activation_refused(*this, [&] { select({"bitmap"}); }, "nothing writable");
    EXPECT_FALSE(presets()->is_sensitive());
}

TEST_F(RowTest, C05_APointerPresetHandsFocusBackToTheCanvas)
{
    open(R("a", "2"), {"a"});
    auto *canvas = desktop->getCanvas();
    canvas->set_focusable(true); // the CanvasGrid does this in a real window
    canvas_host->present();
    present();
    ASSERT_TRUE(wait_until([&] { return canvas->get_mapped(); }));
    auto *canvas_root = gtk_widget_get_root(GTK_WIDGET(canvas->gobj()));
    ASSERT_TRUE(canvas_root);
    gtk_root_set_focus(canvas_root, nullptr);
    auto const items = open_presets();
    activate(item_named(items, "2"));
    ASSERT_TRUE(popover_closed());
    drain();
    EXPECT_NEAR(px_width("a"), 2 * PT, 1e-6);
    EXPECT_EQ(gtk_root_get_focus(canvas_root), GTK_WIDGET(canvas->gobj()))
        << "the keyboard goes back to the canvas (or the text caret), not to the list or nowhere";
}

TEST_F(RowTest, C05b_AKeyboardOpenedListReturnsFocusToTheButton)
{
    open(R("a", "2"), {"a"});
    present();
    ASSERT_TRUE(menu_button());
    menu_button()->grab_focus();
    drain();
    auto *root = gtk_widget_get_root(GTK_WIDGET(panel->gobj()));
    auto *focus = gtk_root_get_focus(root);
    ASSERT_TRUE(focus && (focus == GTK_WIDGET(menu_button()->gobj()) ||
                          gtk_widget_is_ancestor(focus, GTK_WIDGET(menu_button()->gobj()))))
        << "the button must hold focus for this test to mean anything";
    open_presets();
    close_presets(); // what Esc does
    ASSERT_TRUE(popover_closed());
    drain();
    focus = gtk_root_get_focus(root);
    EXPECT_TRUE(focus && (focus == GTK_WIDGET(menu_button()->gobj()) ||
                          gtk_widget_is_ancestor(focus, GTK_WIDGET(menu_button()->gobj()))))
        << "Esc returns focus to the preset button";
    // Desktop session only: the real Esc key and the visible focus ring.
}

TEST_F(RowTest, C06_TextRangeIsTheOnlyTargetAndTheToolStateSurvives)
{
    open(TXT, {"text"});
    auto *text = cast<SPText>(document->getObjectById("text"));
    ASSERT_TRUE(text);
    // The list opens under the selection tool: a widgetless test desktop cannot
    // run the main loop with a live text tool (its cursor update scrolls the view).
    present();
    auto const items = open_presets();
    ASSERT_FALSE(items.empty());
    auto *preset = item_named(items, "3"); // 3 pt = 4 px
    desktop->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    ASSERT_EQ(tool->textItem(), text);
    tool->text_sel_start = text->layout.begin();
    tool->text_sel_start.nextCharacter();
    tool->text_sel_end = tool->text_sel_start;
    tool->text_sel_end.nextCharacter();
    auto const first = text->layout.iteratorToCharIndex(tool->text_sel_start);
    auto const last = text->layout.iteratorToCharIndex(tool->text_sel_end);
    auto const zero = attr("zero");
    auto const eight = attr("eight");
    activate(preset);
    EXPECT_NEAR(px_width("two"), 4, 1e-5);
    EXPECT_EQ(attr("zero"), zero);
    EXPECT_EQ(attr("eight"), eight);
    EXPECT_EQ(attr("text").find("stroke-width"), std::string::npos) << attr("text");
    EXPECT_EQ(desktop->getTool(), tool);
    EXPECT_EQ(tool->textItem(), text);
    EXPECT_EQ(text->layout.iteratorToCharIndex(tool->text_sel_start), first);
    EXPECT_EQ(text->layout.iteratorToCharIndex(tool->text_sel_end), last);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Set stroke width"}));
    // Desktop session only: typing continues without a canvas click (native key routing).
}

// ===========================================================================
// K: the shared command, driven directly (no panel involved)
// ===========================================================================

UI::StrokeWidthIntent absolute_px(double px)
{
    UI::StrokeWidthIntent intent;
    intent.kind = UI::StrokeWidthIntentKind::AbsoluteCssPx;
    intent.value = px;
    return intent;
}

UI::StrokeWidthCommandOutcome run_command(RowTest &t, UI::StrokeWidthIntent intent, std::uint64_t *generation,
                                          char const *label = "Probe label", std::string icon = "dialog-fill-and-stroke")
{
    UI::StrokeWidthCommandRequest const request{intent, generation, t.units->getUnit(),
                                                Util::Internal::ContextString(label), icon};
    return UI::apply_stroke_widths_command(*t.desktop, request);
}

TEST_F(RowTest, K01_AppliedIsOneStepWithTheCallersLabelAndIcon)
{
    open(R("a", "2") + R("b", "8"), {"a", "b"});
    std::uint64_t generation = 7;
    auto const before = xml();
    auto outcome = run_command(*this, absolute_px(5), &generation);
    EXPECT_EQ(outcome.state, UI::StrokeWidthCommandState::Applied);
    EXPECT_TRUE(outcome.committed);
    EXPECT_EQ(outcome.changed, 2u);
    EXPECT_TRUE(outcome.note.empty()) << "a success with nothing to report is silent: " << outcome.note;
    EXPECT_NEAR(px_width("a"), 5, 1e-6);
    EXPECT_NEAR(px_width("b"), 5, 1e-6);
    EXPECT_EQ(current_step().label, "Probe label");
    EXPECT_EQ(current_step().icon, "dialog-fill-and-stroke");
    EXPECT_EQ(undo_labels().size(), 1u);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(xml(), before);
    // Label and icon are parameters, not constants.
    outcome = run_command(*this, absolute_px(6), &generation, "Other label", "edit-select-all");
    EXPECT_TRUE(outcome.committed);
    EXPECT_EQ(current_step().label, "Other label");
    EXPECT_EQ(current_step().icon, "edit-select-all");
}

TEST_F(RowTest, K02_UnchangedSettlesNothingAndKeepsRedo)
{
    open(R("a", "2"), {"a"});
    std::uint64_t generation = 1;
    ASSERT_TRUE(run_command(*this, absolute_px(3), &generation).committed);
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    auto const before = snap();
    auto const outcome = run_command(*this, absolute_px(2), &generation);
    EXPECT_EQ(outcome.state, UI::StrokeWidthCommandState::Unchanged);
    EXPECT_FALSE(outcome.committed);
    EXPECT_EQ(outcome.changed, 0u);
    expect_same(before);
    EXPECT_TRUE(DocumentUndo::redo(document.get())) << "the Redo branch survives an unchanged request";
    EXPECT_NEAR(px_width("a"), 3, 1e-6);
}

TEST_F(RowTest, K03a_AGenerationBumpDuringTheWriteRollsBack)
{
    open(R("a", "2"), {"a"});
    std::uint64_t generation = 1;
    GenerationBumper observer;
    observer.action = [&] { ++generation; };
    auto *repr = document->getObjectById("a")->getRepr();
    repr->addObserver(observer);
    auto const before = snap();
    messages.clear();
    auto const outcome = run_command(*this, absolute_px(5), &generation);
    repr->removeObserver(observer);
    EXPECT_EQ(observer.calls, 1u);
    EXPECT_FALSE(outcome.committed);
    EXPECT_EQ(outcome.state, UI::StrokeWidthCommandState::Failed);
    expect_same(before);
    EXPECT_TRUE(messages.has_warning_containing("rolled back")) << messages.all();
}

TEST_F(RowTest, K03b_ASelectionChangeDuringTheWriteRollsBackAndTheNewRootIsNeverEdited)
{
    open(R("a", "2") + R("b", "4"), {"a"});
    std::uint64_t generation = 1;
    GenerationBumper observer;
    observer.action = [&] { desktop->getSelection()->set(cast<SPItem>(document->getObjectById("b"))); };
    auto *repr = document->getObjectById("a")->getRepr();
    repr->addObserver(observer);
    auto const before = xml();
    auto const outcome = run_command(*this, absolute_px(5), &generation);
    repr->removeObserver(observer);
    EXPECT_EQ(observer.calls, 1u);
    EXPECT_FALSE(outcome.committed);
    EXPECT_EQ(xml(), before);
    EXPECT_NEAR(px_width("b"), 4, 1e-9);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(RowTest, K04a_ExcludedMembersAreCountedAndReported)
{
    open(group_fixture(), {"g"});
    std::uint64_t generation = 1;
    auto const outcome = run_command(*this, absolute_px(5), &generation);
    EXPECT_TRUE(outcome.committed);
    EXPECT_GE(outcome.excluded, 2u);
    EXPECT_NE(outcome.note.find("incompatible or protected items were skipped"), std::string::npos) << outcome.note;
}

TEST_F(RowTest, K04b_FollowingClonesAreCountedAndReported)
{
    open(R("src", "2") + R"(<use id="u" xlink:href="#src" x="20"/>)", {"src", "u"});
    std::uint64_t generation = 1;
    auto const outcome = run_command(*this, absolute_px(5), &generation);
    EXPECT_TRUE(outcome.committed);
    EXPECT_EQ(outcome.following_clones, 1u);
    EXPECT_NE(outcome.note.find("follow"), std::string::npos) << outcome.note;
}

char const *const LPE_BODY =
    R"(<defs><inkscape:path-effect effect="spiro" id="effect"/></defs>)"
    R"(<path id="lpepath" inkscape:path-effect="#effect" inkscape:original-d="M2 8 H40" d="M2 8 H40" )"
    R"(style="stroke:black;stroke-width:0.5"/>)";

TEST_F(RowTest, K05_APendingAutomaticUpdateIsSettledBeforeTheCommandsOwnStep)
{
    // An XML change outside the history is what an automatic update leaves behind.
    open(R("a", "2"), {"a"});
    std::uint64_t generation = 1;
    document->getObjectById("a")->getRepr()->setAttribute("data-automatic", "1");
    if (auto probe = DocumentUndo::beginAtomicInteraction(document.get())) {
        probe->rollback();
        FAIL() << "the fixture must leave a pending automatic change, or this test proves nothing";
    }
    auto const outcome = run_command(*this, absolute_px(5), &generation, "Probe B");
    ASSERT_TRUE(outcome.committed) << outcome.note;
    EXPECT_NEAR(px_width("a"), 5, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Probe B", "Automatic update"}))
        << "the settlement is its own step, older than the command's";
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_NEAR(px_width("a"), 2, 1e-6);
    EXPECT_EQ(attr("a", "data-automatic"), "1") << "undoing the command keeps the settled change";
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(attr("a", "data-automatic"), "") << "the settlement step owned it";
}

/// Logs which attribute changed and whether the settlement had already been published.
struct KeyLogObserver final : XML::NodeObserver
{
    bool *settled = nullptr;
    std::vector<std::pair<bool, std::string>> log;
    void notifyAttributeChanged(XML::Node &, GQuark key, Util::ptr_shared, Util::ptr_shared) override
    {
        log.emplace_back(settled && *settled, g_quark_to_string(key));
    }
};

char const *const POWERSTROKE_BODY =
    R"(<defs><inkscape:path-effect id="effect" effect="powerstroke" is_visible="true" lpeversion="1" )"
    R"(offset_points="0.2,6 | 1,6 | 1.8,6" sort_points="true" interpolator_type="CubicBezierSmooth" )"
    R"(interpolator_beta="0.2" start_linecap_type="zerowidth" end_linecap_type="zerowidth" )"
    R"(linejoin_type="extrp_arc" miter_limit="4" scale_width="1"/></defs>)"
    R"(<path id="lpe" inkscape:original-d="M 20,50 C 40,10 90,90 140,50" inkscape:path-effect="#effect" )"
    R"(d="M 20,50 C 40,10 90,90 140,50" style="fill:none;stroke:#123456;stroke-width:2"/>)";

/// Leaves a pending automatic change, then lets \a during_settlement run inside the settlement's commit.
struct SettlementRig
{
    bool settled = false;
    KeyLogObserver log;
    sigc::scoped_connection connection;
    XML::Node *repr = nullptr;
    void attach(RowTest &t, char const *id, std::function<void()> during_settlement)
    {
        repr = t.document->getObjectById(id)->getRepr();
        repr->setAttribute("data-automatic", "1"); // pending: forces the settlement
        if (auto probe = DocumentUndo::beginAtomicInteraction(t.document.get())) {
            probe->rollback();
            ADD_FAILURE() << "the fixture must leave a pending automatic change";
        }
        log.settled = &settled;
        repr->addObserver(log);
        connection = t.document->connectCommit([this, hook = std::move(during_settlement)] {
            if (settled) return;
            settled = true;
            hook();
        });
    }
    ~SettlementRig()
    {
        if (repr) repr->removeObserver(log);
    }
    int first_after_settlement(char const *key) const
    {
        for (std::size_t i = 0; i < log.log.size(); ++i) {
            if (log.log[i].first && log.log[i].second == key) return static_cast<int>(i);
        }
        return -1;
    }
};

TEST_F(RowTest, K05b_ASettlementThatRegeneratesThePathOutputIsPermitted)
{
    // The settlement changes a path-effect PARAMETER: the effect recomputes `d` between the two
    // preparations while `inkscape:original-d` and the effect reference stay identical.
    open(POWERSTROKE_BODY, {"lpe"});
    auto const source = attr("lpe", "inkscape:original-d");
    SettlementRig rig;
    rig.attach(*this, "lpe", [&] {
        document->getObjectById("effect")->getRepr()->setAttribute("offset_points", "0.2,3 | 1,3 | 1.8,3");
    });
    std::uint64_t generation = 1;
    auto const outcome = run_command(*this, absolute_px(5), &generation, "Probe B");
    ASSERT_TRUE(rig.settled) << "the settlement branch must run";
    EXPECT_TRUE(outcome.committed) << outcome.note;
    EXPECT_EQ(attr("lpe", "inkscape:original-d"), source) << "the source geometry was never touched";
    int const regenerated = rig.first_after_settlement("d");
    int const style_write = rig.first_after_settlement("style");
    EXPECT_GE(regenerated, 0) << "the effect must regenerate d after the settlement";
    EXPECT_GE(style_write, 0) << "the command's style write must be observed";
    EXPECT_LT(regenerated, style_write) << "regeneration is between the preparations, before the write";
    EXPECT_NEAR(px_width("lpe"), 5, 1e-6);
    auto const labels = undo_labels();
    ASSERT_GE(labels.size(), 2u);
    EXPECT_EQ(labels.front(), "Probe B");
    EXPECT_EQ(labels[1], "Automatic update");
}

TEST_F(RowTest, K05c_ASettlementThatChangesTheSourceGeometryRefusesTheCommand)
{
    open(POWERSTROKE_BODY, {"lpe"});
    SettlementRig rig;
    rig.attach(*this, "lpe", [&] {
        rig.repr->setAttribute("inkscape:original-d", "M 20,50 C 40,10 90,90 160,50");
    });
    std::uint64_t generation = 1;
    auto const outcome = run_command(*this, absolute_px(5), &generation, "Probe B");
    ASSERT_TRUE(rig.settled);
    EXPECT_EQ(outcome.state, UI::StrokeWidthCommandState::Refused);
    EXPECT_FALSE(outcome.committed);
    EXPECT_EQ(rig.first_after_settlement("style"), -1) << "no width was written";
    EXPECT_NEAR(px_width("lpe"), 2, 1e-6);
    EXPECT_EQ(undo_labels(), (std::vector<std::string>{"Automatic update"})) << "only the settlement, no command step";
}

TEST_F(RowTest, K06_NoTextCompositionByDefault)
{
    open(TXT, {"text"});
    desktop->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    EXPECT_FALSE(tool->isComposing());
    std::uint64_t generation = 1;
    auto const outcome = run_command(*this, absolute_px(3), &generation);
    EXPECT_NE(outcome.state, UI::StrokeWidthCommandState::Refused) << outcome.note;
    // Desktop session only: an IME composition or Ctrl+Shift+U entry in progress
    // makes the command refuse with a message (the state is private to the tool).
}

// ===========================================================================
// K (fix round): lifetime, live scope, stale roots, unit, patches, tracking
// ===========================================================================

/// An LPE path whose source path was edited outside the history: the next refresh
/// recomputes its path (attribute callbacks during ensureUpToDate).
void prepare_refresh_callback(RowTest &t, std::uint64_t *)
{
    t.document->getObjectById("lpepath")->getRepr()->setAttribute("inkscape:original-d", "M2 8 H50");
}

TEST_F(RowTest, K07a_ASelectionChangeDuringARefreshRefuses)
{
    open(std::string(LPE_BODY) + R("other", "4"), {"lpepath"});
    std::uint64_t generation = 1;
    prepare_refresh_callback(*this, &generation);
    WriteObserver observer;
    observer.action = [&] { desktop->getSelection()->set(cast<SPItem>(document->getObjectById("other"))); };
    auto *repr = document->getObjectById("lpepath")->getRepr();
    repr->addObserver(observer);
    auto const before = snap();
    auto const outcome = run_command(*this, absolute_px(0.9), &generation, "Probe B");
    repr->removeObserver(observer);
    ASSERT_GE(observer.events, 1u) << "the first refresh must call back, or this test proves nothing";
    EXPECT_EQ(outcome.state, UI::StrokeWidthCommandState::Refused);
    EXPECT_FALSE(outcome.committed);
    EXPECT_NEAR(px_width("other"), 4, 1e-9) << "the new selection is never the target";
    EXPECT_NEAR(px_width("lpepath"), 0.5, 1e-6);
    // A settled automatic update may have added its own step; the command wrote nothing.
    EXPECT_EQ(undo_labels().front(), "Automatic update");
    (void)before;
}

TEST_F(RowTest, K07b_ADocumentReplacedDuringARefreshIsRefusedQuietly)
{
    open(std::string(LPE_BODY), {"lpepath"});
    std::uint64_t generation = 1;
    prepare_refresh_callback(*this, &generation);
    auto replacement = SPDocument::createNewDocFromMem(svg(R("x", "1")));
    ASSERT_TRUE(replacement);
    WriteObserver observer;
    observer.action = [&] { desktop->setDocument(replacement.get()); };
    auto *repr = document->getObjectById("lpepath")->getRepr();
    repr->addObserver(observer);
    messages.clear();
    auto const outcome = run_command(*this, absolute_px(0.9), &generation, "Probe B");
    repr->removeObserver(observer);
    ASSERT_GE(observer.events, 1u);
    EXPECT_EQ(outcome.state, UI::StrokeWidthCommandState::Refused);
    EXPECT_TRUE(outcome.note.empty());
    EXPECT_TRUE(messages.warnings().empty()) << messages.all();
    EXPECT_NEAR(px_width("lpepath"), 0.5, 1e-6);
    desktop->setDocument(document.get());
}

TEST_F(RowTest, K07c_ADesktopDestroyedDuringARefreshTouchesNothing)
{
    open(std::string(LPE_BODY), {"lpepath"});
    std::uint64_t generation = 1;
    prepare_refresh_callback(*this, &generation);
    WriteObserver observer;
    observer.action = [&] {
        panel->setDesktop(nullptr);
        messages.connection.disconnect();
        canvas_host->unset_child();
        Application::instance().remove_desktop(desktop.get());
        registered = false;
        desktop.reset();
    };
    auto *repr = document->getObjectById("lpepath")->getRepr();
    repr->addObserver(observer);
    UI::StrokeWidthCommandOutcome outcome;
    {
        // run_command reads the desktop through the fixture: call the command directly.
        UI::StrokeWidthCommandRequest const request{absolute_px(0.9), &generation, units->getUnit(),
                                                    Util::Internal::ContextString("Probe B"),
                                                    "dialog-fill-and-stroke"};
        outcome = UI::apply_stroke_widths_command(*desktop, request);
    }
    repr->removeObserver(observer);
    ASSERT_GE(observer.events, 1u);
    EXPECT_EQ(outcome.state, UI::StrokeWidthCommandState::Refused);
    EXPECT_FALSE(outcome.committed);
    EXPECT_TRUE(outcome.note.empty());
    EXPECT_FALSE(desktop);
    EXPECT_NEAR(px_width("lpepath"), 0.5, 1e-6);
}

/// A text selection with the tool on the second character, plus observers on every node.
struct TextWriteRig
{
    RowTest &t;
    UI::Tools::TextTool *tool = nullptr;
    SPText *text = nullptr;
    WriteObserver observer;
    std::vector<std::pair<XML::Node *, WriteObserver *>> watched;
    std::vector<std::unique_ptr<WriteObserver>> extra;

    explicit TextWriteRig(RowTest &test) : t(test) {}
    void attach(std::function<void()> action)
    {
        observer.action = std::move(action);
        for (auto id : {"text", "zero", "two", "eight"}) {
            auto *repr = t.document->getObjectById(id)->getRepr();
            if (std::string(id) == "text") {
                repr->addObserver(observer);
                watched.emplace_back(repr, &observer);
            } else {
                auto shared = std::make_unique<WriteObserver>();
                shared->action = [this] { if (!observer.fired) { observer.fired = true; observer.action(); } };
                repr->addObserver(*shared);
                watched.emplace_back(repr, shared.get());
                extra.push_back(std::move(shared));
            }
        }
    }
    ~TextWriteRig()
    {
        for (auto &[repr, obs] : watched) repr->removeObserver(*obs);
    }
    unsigned events() const
    {
        unsigned n = 0;
        for (auto const &[repr, obs] : watched) n += obs->events;
        return n;
    }
};

TEST_F(RowTest, K08a_ARangeMovedDuringTheWriteIsNeverCommitted)
{
    open(TXT, {"text"});
    auto *text = cast<SPText>(document->getObjectById("text"));
    ASSERT_TRUE(text);
    desktop->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    tool->text_sel_start = text->layout.begin();
    tool->text_sel_start.nextCharacter();
    tool->text_sel_end = tool->text_sel_start;
    tool->text_sel_end.nextCharacter();
    std::uint64_t generation = 1;
    auto const before = snap();
    TextWriteRig rig(*this);
    rig.attach([&] { tool->text_sel_end = tool->text_sel_start; }); // the range collapses mid-write
    auto const outcome = run_command(*this, absolute_px(4), &generation);
    ASSERT_GE(rig.events(), 1u) << "the write must call back, or this test proves nothing";
    EXPECT_FALSE(outcome.committed) << outcome.note;
    expect_same(before);
}

TEST_F(RowTest, K08b_ACaretMovedDuringTheWriteIsNeverCommitted)
{
    // A collapsed caret is part of the scope identity too (no range, whole object).
    open(TXT, {"text"});
    auto *text = cast<SPText>(document->getObjectById("text"));
    ASSERT_TRUE(text);
    desktop->setTool("/tools/text");
    auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    tool->text_sel_start = text->layout.begin();
    tool->text_sel_start.nextCharacter();
    tool->text_sel_end = tool->text_sel_start;
    std::uint64_t generation = 1;
    auto const before = snap();
    TextWriteRig rig(*this);
    rig.attach([&] {
        tool->text_sel_start.nextCharacter();
        tool->text_sel_end = tool->text_sel_start;
    });
    auto const outcome = run_command(*this, absolute_px(4), &generation);
    ASSERT_GE(rig.events(), 1u) << "the write must call back, or this test proves nothing";
    EXPECT_FALSE(outcome.committed) << outcome.note;
    expect_same(before);
}

TEST_F(RowTest, K09_ARetainedPresetItemCannotCallADestroyedRow)
{
    open(R("a", "2"), {"a"});
    present();
    auto const items = open_presets();
    ASSERT_FALSE(items.empty());
    auto *kept = item_named(items, "1");
    ASSERT_NE(kept, nullptr);
    g_object_ref(kept->gobj());
    auto const before = snap();
    // The row is destroyed while it still points at its desktop (no orderly setDesktop(nullptr)):
    // a call through a stale receiver would find a live desktop and write.
    panel_host->insert_action_group("doc", {});
    panel_host->unset_child();
    inner.remove(*panel);
    field = nullptr;
    entry = nullptr;
    value_label = nullptr;
    units = nullptr;
    panel.reset();
    RowTest::activate(kept); // the item lives on through the reference
    expect_same(before, "an item activated after its row died must write nothing");
    g_object_unref(kept->gobj());
}

TEST_F(RowTest, K10_AWriteObserverThatChangesTheSelectionStopsTheRemainingWrites)
{
    open(R("a", "2") + R("b", "3") + R("c", "4"), {"a", "b"});
    std::uint64_t generation = 1;
    auto const before = snap();
    WriteObserver first;
    WriteObserver second;
    auto const change_selection = [&] { desktop->getSelection()->set(cast<SPItem>(document->getObjectById("c"))); };
    first.action = change_selection;
    second.action = change_selection;
    auto *a = document->getObjectById("a")->getRepr();
    auto *b = document->getObjectById("b")->getRepr();
    a->addObserver(first);
    b->addObserver(second);
    auto const outcome = run_command(*this, absolute_px(5), &generation);
    a->removeObserver(first);
    b->removeObserver(second);
    EXPECT_FALSE(outcome.committed);
    EXPECT_EQ((first.events == 0) != (second.events == 0), true)
        << "exactly one member may have been written before the change was noticed: " << first.events << "/"
        << second.events;
    expect_same(before);
    EXPECT_NEAR(px_width("c"), 4, 1e-9);
}

TEST_F(RowTest, K11_AUnitChangedDuringTheWriteIsNeverCommitted)
{
    open(R("a", "2"), {"a"});
    WriteObserver observer;
    observer.action = [&] { unit("mm"); };
    auto *repr = document->getObjectById("a")->getRepr();
    repr->addObserver(observer);
    auto const before = snap();
    type("5");
    repr->removeObserver(observer);
    ASSERT_GE(observer.events, 1u);
    expect_same(before);
    EXPECT_EQ(units->getUnitAbbr(), "mm");
}

TEST_F(RowTest, K12_ASettlementThatChangesADashPatchRefusesTheCommand)
{
    open(R"(<rect id="a" x="0" y="0" width="10" height="10" )"
         R"(style="fill:#0000ff;stroke:#008000;stroke-width:2;stroke-dasharray:4,2"/>)", {"a"});
    std::uint64_t generation = 1;
    auto *repr = document->getObjectById("a")->getRepr();
    repr->setAttribute("data-automatic", "1"); // pending settlement
    if (auto probe = DocumentUndo::beginAtomicInteraction(document.get())) {
        probe->rollback();
        FAIL() << "the fixture must leave a pending automatic change";
    }
    bool changed = false;
    auto connection = document->connectCommit([&] {
        if (changed) return;
        changed = true; // the settlement itself edits the dash: widths stay, the dash patch differs
        repr->setAttribute("style", "fill:#0000ff;stroke:#008000;stroke-width:2;stroke-dasharray:8,4");
    });
    auto const outcome = run_command(*this, absolute_px(5), &generation);
    connection.disconnect();
    ASSERT_TRUE(changed) << "the settlement must run, or this test proves nothing";
    EXPECT_EQ(outcome.state, UI::StrokeWidthCommandState::Refused);
    EXPECT_FALSE(outcome.committed);
    EXPECT_NEAR(px_width("a"), 2, 1e-6);
    EXPECT_NE(attr("a").find("8,4"), std::string::npos) << attr("a");
}

TEST_F(RowTest, C05c_APointerPresetReturnsFocusToTheWidgetThatHadIt)
{
    // The real pointer controller on the button records the focus BEFORE GTK moves it
    // into the list; a genuinely focused widget (not the canvas fallback) gets it back.
    open(R("a", "2"), {"a"});
    present();
    auto *target = find_widget<Gtk::ToggleButton>(*panel);
    ASSERT_TRUE(target);
    target->grab_focus();
    drain();
    auto *root = gtk_widget_get_root(GTK_WIDGET(panel->gobj()));
    ASSERT_EQ(gtk_root_get_focus(root), GTK_WIDGET(target->gobj())) << "the target must hold focus";
    ASSERT_TRUE(menu_button());
    ControllerRefs presses;
    collect(*menu_button(), GTK_TYPE_GESTURE_CLICK, false, presses);
    ASSERT_FALSE(presses.items.empty());
    for (auto *press : presses.items) g_signal_emit_by_name(press, "pressed", 1, 5.0, 5.0);
    auto const items = open_presets(); // what the press opens
    drain();
    auto *inside = gtk_root_get_focus(root);
    auto *list = GTK_WIDGET(popover()->gobj());
    EXPECT_TRUE(inside != GTK_WIDGET(target->gobj()) && (inside == nullptr || inside == list ||
                                                          gtk_widget_is_ancestor(inside, list) ||
                                                          gtk_widget_is_ancestor(inside, GTK_WIDGET(menu_button()->gobj())) ||
                                                          inside == GTK_WIDGET(menu_button()->gobj())))
        << "focus must leave the return target while the list is open";
    EXPECT_NE(gtk_root_get_focus(root), GTK_WIDGET(target->gobj()));
    activate(item_named(items, "2"));
    ASSERT_TRUE(popover_closed());
    drain();
    EXPECT_NEAR(px_width("a"), 2 * PT, 1e-6);
    EXPECT_EQ(gtk_root_get_focus(root), GTK_WIDGET(target->gobj()))
        << "focus returns to the widget that had it when the list opened";
}

TEST_F(RowTest, K12b_ASettlementThatTranslatesATargetRefusesTheCommand)
{
    // Same scale, same style, same patches: only the position (the affine) moves.
    open(R("a", "2"), {"a"});
    std::uint64_t generation = 1;
    auto *repr = document->getObjectById("a")->getRepr();
    repr->setAttribute("data-automatic", "1");
    if (auto probe = DocumentUndo::beginAtomicInteraction(document.get())) {
        probe->rollback();
        FAIL() << "the fixture must leave a pending automatic change";
    }
    bool moved = false;
    auto connection = document->connectCommit([&] {
        if (moved) return;
        moved = true;
        repr->setAttribute("transform", "translate(5,5)");
    });
    auto const outcome = run_command(*this, absolute_px(5), &generation);
    connection.disconnect();
    ASSERT_TRUE(moved);
    EXPECT_EQ(outcome.state, UI::StrokeWidthCommandState::Refused);
    EXPECT_FALSE(outcome.committed);
    EXPECT_NEAR(px_width("a"), 2, 1e-6);
}

TEST_F(RowTest, K12c_ASettlementThatReparentsATargetRefusesTheCommand)
{
    open(R("a", "2"), {"a"});
    std::uint64_t generation = 1;
    auto *repr = document->getObjectById("a")->getRepr();
    repr->setAttribute("data-automatic", "1");
    if (auto probe = DocumentUndo::beginAtomicInteraction(document.get())) {
        probe->rollback();
        FAIL() << "the fixture must leave a pending automatic change";
    }
    bool moved = false;
    auto connection = document->connectCommit([&] {
        if (moved) return;
        moved = true;
        auto *parent = repr->parent();
        auto *group = parent->document()->createElement("svg:g");
        parent->appendChild(group);
        parent->removeChild(repr);
        group->appendChild(repr);
    });
    auto const outcome = run_command(*this, absolute_px(5), &generation);
    connection.disconnect();
    ASSERT_TRUE(moved);
    EXPECT_FALSE(outcome.committed);
    EXPECT_NE(outcome.state, UI::StrokeWidthCommandState::Applied);
    EXPECT_NEAR(px_width("a"), 2, 1e-6);
}

TEST_F(RowTest, K13_ADocumentDestroyedByTheSettlementIsNeverTouchedAgain)
{
    open(R("a", "2"), {"a"});
    std::uint64_t generation = 1;
    auto *repr = document->getObjectById("a")->getRepr();
    repr->setAttribute("data-automatic", "1");
    if (auto probe = DocumentUndo::beginAtomicInteraction(document.get())) {
        probe->rollback();
        FAIL() << "the fixture must leave a pending automatic change";
    }
    bool destroyed = false;
    auto connection = document->connectCommit([&] {
        if (destroyed) return;
        destroyed = true;
        panel->setDesktop(nullptr);
        messages.connection.disconnect();
        canvas_host->unset_child();
        Application::instance().remove_desktop(desktop.get());
        registered = false;
        desktop.reset();
        document.reset(); // inside the settlement's own commit
    });
    UI::StrokeWidthCommandRequest const request{absolute_px(5), &generation, units->getUnit(),
                                                Util::Internal::ContextString("Probe"), "dialog-fill-and-stroke"};
    auto const outcome = UI::apply_stroke_widths_command(*desktop, request);
    connection.disconnect();
    ASSERT_TRUE(destroyed);
    EXPECT_EQ(outcome.state, UI::StrokeWidthCommandState::Refused);
    EXPECT_FALSE(outcome.committed);
    EXPECT_TRUE(outcome.note.empty());
    EXPECT_FALSE(document);
}

/// A tool with a range or caret on the second character of the text, plus a pre-commit hook.
struct PreCommitRig
{
    RowTest &t;
    SPText *text = nullptr;
    UI::Tools::TextTool *tool = nullptr;
    unsigned hooked = 0;
    sigc::scoped_connection connection;
    explicit PreCommitRig(RowTest &test) : t(test)
    {
        t.open(TXT, {"text"});
        text = cast<SPText>(t.document->getObjectById("text"));
        t.desktop->setTool("/tools/text");
        tool = dynamic_cast<UI::Tools::TextTool *>(t.desktop->getTool());
    }
    bool ready() const { return text && tool; }
    void range() // second character
    {
        tool->text_sel_start = text->layout.begin();
        tool->text_sel_start.nextCharacter();
        tool->text_sel_end = tool->text_sel_start;
        tool->text_sel_end.nextCharacter();
    }
    void caret()
    {
        tool->text_sel_start = text->layout.begin();
        tool->text_sel_start.nextCharacter();
        tool->text_sel_end = tool->text_sel_start;
    }
    void before_commit(std::function<void()> action)
    {
        connection = t.document->connectBeforeCommit([this, action = std::move(action)] {
            if (hooked++) return;
            action();
        });
    }
};

TEST_F(RowTest, K08c_ARangeReversedBeforeTheCommitIsNeverCommitted)
{
    PreCommitRig rig(*this);
    ASSERT_TRUE(rig.ready());
    rig.range();
    rig.before_commit([&] { std::swap(rig.tool->text_sel_start, rig.tool->text_sel_end); });
    std::uint64_t generation = 1;
    auto const before = snap();
    auto const outcome = run_command(*this, absolute_px(4), &generation);
    ASSERT_GE(rig.hooked, 1u) << "the pre-commit hook must run";
    EXPECT_FALSE(outcome.committed) << outcome.note;
    expect_same(before);
}

TEST_F(RowTest, K08d_ARangeCollapsedBeforeTheCommitIsNeverCommitted)
{
    PreCommitRig rig(*this);
    ASSERT_TRUE(rig.ready());
    rig.range();
    rig.before_commit([&] { rig.tool->text_sel_end = rig.tool->text_sel_start; });
    std::uint64_t generation = 1;
    auto const before = snap();
    auto const outcome = run_command(*this, absolute_px(4), &generation);
    ASSERT_GE(rig.hooked, 1u);
    EXPECT_FALSE(outcome.committed) << outcome.note;
    expect_same(before);
}

TEST_F(RowTest, K08e_ACaretMovedBeforeTheCommitIsNeverCommitted)
{
    PreCommitRig rig(*this);
    ASSERT_TRUE(rig.ready());
    rig.caret();
    rig.before_commit([&] {
        rig.tool->text_sel_start.nextCharacter();
        rig.tool->text_sel_end = rig.tool->text_sel_start;
    });
    std::uint64_t generation = 1;
    auto const before = snap();
    auto const outcome = run_command(*this, absolute_px(4), &generation);
    ASSERT_GE(rig.hooked, 1u);
    EXPECT_FALSE(outcome.committed) << outcome.note;
    expect_same(before);
}

TEST_F(RowTest, K14_TheStrictPlanComparisonSeesEveryWriteStringAndProvenance)
{
    open(TXT + std::string(R("r", "2")), {"text"});
    auto roots = items({"text"});
    UI::StrokeWidthIntent intent = absolute_px(5);
    auto const base = UI::prepare_stroke_widths(*document, roots, intent, 1);
    ASSERT_EQ(base.state, UI::StrokeWidthPlanState::Prepared);
    ASSERT_FALSE(base.members.empty());
    ASSERT_FALSE(base.members[0].text_runs.empty());
    auto copy = base;
    EXPECT_TRUE(UI::stroke_width_plans_match(base, copy));
    // Either actual text write string.
    copy.members[0].text_runs[0].width_css = "5";
    auto other = copy;
    EXPECT_TRUE(UI::stroke_width_plans_match(copy, other));
    other.members[0].text_runs[0].width_css = "6";
    EXPECT_FALSE(UI::stroke_width_plans_match(copy, other)) << "width_css";
    other = copy;
    copy.members[0].text_runs[0].dashoffset_css = "1";
    other.members[0].text_runs[0].dashoffset_css = "2";
    EXPECT_FALSE(UI::stroke_width_plans_match(copy, other)) << "dashoffset_css";
    // Provenance and authored attributes.
    auto moved = base;
    moved.members[0].i2doc_affine[4] += 3;
    EXPECT_FALSE(UI::stroke_width_plans_match(base, moved)) << "translation, same scale";
    auto reparented = base;
    reparented.members[0].original_parent.reset();
    EXPECT_FALSE(UI::stroke_width_plans_match(base, reparented)) << "parent";
    auto restyled = base;
    restyled.members[0].inline_style = "stroke:red";
    EXPECT_FALSE(UI::stroke_width_plans_match(base, restyled)) << "inline style";
    // Path output: only `d`, and only with the same effect AND the same source geometry.
    auto plain_a = base;
    auto plain_b = base;
    plain_a.members[0].non_style_attributes = {{"d", "M0 0"}};
    plain_b.members[0].non_style_attributes = {{"d", "M0 1"}};
    EXPECT_FALSE(UI::stroke_width_plans_match(plain_a, plain_b)) << "path data without a path effect";
    plain_a.members[0].non_style_attributes = {{"d", "M0 0"}, {"inkscape:path-effect", "#e"}, {"inkscape:original-d", "M5 5"}};
    plain_b.members[0].non_style_attributes = {{"d", "M0 1"}, {"inkscape:path-effect", "#e"}, {"inkscape:original-d", "M5 5"}};
    EXPECT_TRUE(UI::stroke_width_plans_match(plain_a, plain_b)) << "d regenerated by an unchanged effect";
    plain_b.members[0].non_style_attributes = {{"d", "M0 1"}, {"inkscape:path-effect", "#e"}, {"inkscape:original-d", "M6 6"}};
    EXPECT_FALSE(UI::stroke_width_plans_match(plain_a, plain_b)) << "a changed source geometry";
    plain_b.members[0].non_style_attributes = {{"d", "M0 0"}, {"inkscape:path-effect", "#e"}, {"inkscape:original-d", "M6 6"}};
    EXPECT_FALSE(UI::stroke_width_plans_match(plain_a, plain_b)) << "a changed source geometry alone";
    plain_b.members[0].non_style_attributes = {{"d", "M0 1"}, {"inkscape:path-effect", "#other"}, {"inkscape:original-d", "M5 5"}};
    EXPECT_FALSE(UI::stroke_width_plans_match(plain_a, plain_b)) << "a different effect";
    plain_b.members[0].non_style_attributes = {{"d", "M0 1"}, {"inkscape:path-effect", "#e"}, {"inkscape:original-d", "M5 5"}, {"x", "1"}};
    EXPECT_FALSE(UI::stroke_width_plans_match(plain_a, plain_b)) << "any other attribute";
}

TEST_F(RowTest, K09b_RetainedRowChildrenCannotCallADestroyedRow)
{
    open(R("a", "2"), {"a"});
    auto *paint_order = find_widget<UI::Widget::PaintOrderWidget>(*panel);
    ASSERT_TRUE(paint_order && dec() && inc() && field);
    std::vector<GObject *> kept = {G_OBJECT(dec()->gobj()), G_OBJECT(inc()->gobj()),
                                   G_OBJECT(static_cast<Gtk::Widget *>(paint_order)->gobj()),
                                   G_OBJECT(static_cast<Gtk::Widget *>(field)->gobj())};
    for (auto *object : kept) g_object_ref(object);
    Gtk::Widget *dec_button = dec();
    Gtk::Widget *inc_button = inc();
    auto *spin = field;
    auto *order = paint_order;
    auto const before = snap();
    panel_host->insert_action_group("doc", {});
    panel_host->unset_child();
    inner.remove(*panel);
    field = nullptr;
    entry = nullptr;
    value_label = nullptr;
    units = nullptr;
    panel.reset();
    g_signal_emit_by_name(dec_button->gobj(), "clicked");
    g_signal_emit_by_name(inc_button->gobj(), "clicked");
    order->signal_values_changed().emit();
    ControllerRefs keys;
    collect(*spin, GTK_TYPE_EVENT_CONTROLLER_KEY, true, keys);
    for (auto *key : keys.items) {
        gboolean handled = FALSE;
        g_signal_emit_by_name(key, "key-pressed", guint(GDK_KEY_Up), keycode_of(GDK_KEY_Up),
                              static_cast<GdkModifierType>(0), &handled);
    }
    spin->get_adjustment()->set_value(spin->get_adjustment()->get_value() + 1);
    expect_same(before, "children retained past the row must not reach it");
    for (auto *object : kept) g_object_unref(object);
}

} // namespace
