// SPDX-License-Identifier: GPL-2.0-or-later
// EB6-register: exercise the real dialog actions, menu model, factory and session restore.
// The same entry points prove that spell checking stays hidden (ui/spellcheck-feature.h).
#include <gtest/gtest.h>
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <giomm/menu.h>
#include <gtkmm/builder.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/notebook.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/textview.h>
#include <gtkmm/window.h>
#include "actions/actions-dialogs.h"
#include "document.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "io/resource.h"
#include "object/sp-item.h"
#include "ui/contextmenu.h"
#include "ui/dialog/dialog-container.h"
#include "ui/dialog/command-palette.h"
#include "ui/dialog/dialog-data.h"
#include "ui/dialog/dialog-notebook.h"
#include "ui/dialog/dialog-window.h"
#include "ui/dialog/explode-bitmap.h"
#include "ui/dialog/inkscape-preferences.h"
#include "ui/explode-bitmap-feature.h"
#include "ui/shortcuts.h"
#include "ui/spellcheck-feature.h"

using namespace Inkscape;
using namespace Inkscape::UI::Dialog;
namespace {
constexpr auto dialog = "ExplodeBitmap";
constexpr auto action = "dialog-explode-bitmap";
constexpr auto detailed = "win.dialog-explode-bitmap";

void drain()
{
    for (unsigned i = 0; i < 10000 && g_main_context_pending(nullptr); ++i)
        g_main_context_iteration(nullptr, false);
}

InkscapeApplication &application()
{
    static auto app = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "explodebitmapregistertest", true);
        auto result = new InkscapeApplication();
        result->gio_app()->register_application();
        for (auto signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) std::signal(signal, SIG_DFL);
#ifndef _WIN32
        std::signal(SIGBUS, SIG_DFL);
#endif
        return result;
    }();
    return *app;
}

template <typename T>
T *findWidget(Gtk::Widget &root, Glib::ustring const &text)
{
    if (auto widget = dynamic_cast<T *>(&root); widget && widget->get_label() == text) return widget;
    for (auto child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto found = findWidget<T>(*child, text)) return found;
    }
    return nullptr;
}

bool visibleLabel(Gtk::Widget &root, Glib::ustring const &text)
{
    for (auto child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (!child->get_visible()) continue;
        if (auto label = dynamic_cast<Gtk::Label *>(child); label && label->get_text() == text) return true;
        if (visibleLabel(*child, text)) return true;
    }
    return false;
}

Gtk::Widget *namedWidget(Gtk::Widget &root, Glib::ustring const &name)
{
    if (root.get_name() == name) return &root;
    for (auto child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto found = namedWidget(*child, name)) return found;
    }
    return nullptr;
}

Gtk::ListBoxRow *actionRow(Gtk::Widget &root)
{
    auto label = findWidget<Gtk::Label>(root, detailed);
    for (auto parent = label ? label->get_parent() : nullptr; parent; parent = parent->get_parent()) {
        if (auto row = dynamic_cast<Gtk::ListBoxRow *>(parent)) return row;
    }
    return nullptr;
}

bool enterHistory(Gtk::Widget &filter)
{
    auto controllers = gtk_widget_observe_controllers(filter.gobj());
    gboolean handled = false;
    for (unsigned i = 0; i < g_list_model_get_n_items(controllers) && !handled; ++i) {
        auto controller = g_list_model_get_item(controllers, i);
        if (GTK_IS_EVENT_CONTROLLER_KEY(controller))
            g_signal_emit_by_name(controller, "key-pressed", GDK_KEY_Up, 0, GdkModifierType(0), &handled);
        g_object_unref(controller);
    }
    g_object_unref(controllers);
    return handled;
}

class ExplodeBitmapRegister : public testing::Test {
protected:
    void SetUp() override
    {
        // Missing desktop opt-in is a failing gate, never a successful skip.
        ASSERT_STREQ(std::getenv("INKSCAPE_TEST_GUI"), "1");
        prefs = Preferences::get();
        saved = prefs->getString(Bitmap::explodeBitmapPreference);
        had = prefs->getEntry(Bitmap::explodeBitmapPreference).isSet();
        prefs->setBool(Bitmap::explodeBitmapPreference, false);
        auto &app = application();
        // Rebuild the startup metadata for each isolated flag scenario. The
        // production palette caches it; the Preferences label requires restart.
        app.get_action_extra_data() = InkActionExtraData{};
        add_actions_dialogs(&app);
        prefs->setInt("/options/dialogtype/value", 1);
        prefs->setInt("/options/savedialogposition/value", 1);
    }
    void TearDown() override
    {
        if (desktop) {
            desktop->getDocument()->setModifiedSinceSave(false);
            application().destroyDesktop(desktop);
            desktop = nullptr;
            drain();
        }
        if (!prefs) return;
        if (had) prefs->setString(Bitmap::explodeBitmapPreference, saved);
        else prefs->remove(Bitmap::explodeBitmapPreference);
    }
    InkscapeWindow *window(bool enabled,
                           std::string_view svg = "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'/>")
    {
        prefs->setBool(Bitmap::explodeBitmapPreference, enabled);
        auto &app = application();
        auto doc = app.document_add(SPDocument::createNewDocFromMem(svg));
        desktop = app.createDesktop(doc, false, true);
        drain();
        return desktop->getInkscapeWindow();
    }
    bool searchable()
    {
        auto entries = Shortcuts::getInstance().list_all_detailed_action_names();
        return std::find(entries.begin(), entries.end(), detailed) != entries.end();
    }
    void registry(bool enabled)
    {
            auto list = get_dialog_data_list();
        EXPECT_EQ(std::count_if(list.begin(), list.end(), [](auto const &d) { return d.key == dialog; }), enabled ? 1 : 0);
    }
    void closePanel()
    {
        if (auto panel = desktop->getContainer()->get_dialog(dialog)) {
            for (auto parent = panel->get_parent(); parent; parent = parent->get_parent()) {
                if (auto notebook = dynamic_cast<DialogNotebook *>(parent)) {
                    notebook->close_tab(panel);
                    break;
                }
            }
            drain();
        }
    }
    Preferences *prefs = nullptr;
    Glib::ustring saved;
    bool had = false;
    SPDesktop *desktop = nullptr;
};

TEST_F(ExplodeBitmapRegister, DefaultOffHidesMenuSearchFactoryAndRefusesDirectAction)
{
    prefs->remove(Bitmap::explodeBitmapPreference);
    EXPECT_FALSE(Bitmap::explodeBitmapEnabled());
    auto win = window(false);
    registry(false);
    EXPECT_FALSE(searchable());
    EXPECT_FALSE(win->lookup_action(action));
    EXPECT_FALSE(application().gio_app()->lookup_action(action)); // No CLI action.

    auto builder = Gtk::Builder::create_from_file(IO::Resource::get_filename(IO::Resource::UIS, "menus.ui"));
    auto model = builder->get_object<Gio::Menu>("object-menu");
    ASSERT_TRUE(model);
    Gtk::PopoverMenu menu(model);
    gtk_widget_insert_action_group(GTK_WIDGET(menu.gobj()), "win", G_ACTION_GROUP(win->gobj()));
    drain();
    EXPECT_FALSE(visibleLabel(menu, "Explode Bitmap..."));

    auto container = desktop->getContainer();
    win->lookup_action("dialog-open")->activate(Glib::ustring(dialog));
    container->new_dialog(dialog);
    container->new_dialog(dialog, nullptr, false);
    EXPECT_EQ(container->new_floating_dialog(dialog), nullptr);
    EXPECT_EQ(container->get_dialog(dialog), nullptr);
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog(dialog), nullptr);
}

TEST_F(ExplodeBitmapRegister, EnabledMenuAndSearchOpenPanelAndStaleActionIsRefused)
{
    auto win = window(true);
    registry(true);
    ASSERT_TRUE(searchable());
    ASSERT_TRUE(win->lookup_action(action));
    auto builder = Gtk::Builder::create_from_file(IO::Resource::get_filename(IO::Resource::UIS, "menus.ui"));
    auto model = builder->get_object<Gio::Menu>("object-menu");
    ASSERT_TRUE(model);
    Gtk::PopoverMenu menu(model);
    gtk_widget_insert_action_group(GTK_WIDGET(menu.gobj()), "win", G_ACTION_GROUP(win->gobj()));
    drain();
    EXPECT_TRUE(visibleLabel(menu, "Explode Bitmap..."));
    // Activate the action actually present in the menu/search registry.
    EXPECT_TRUE(gtk_widget_activate_action_variant(GTK_WIDGET(menu.gobj()), detailed, nullptr));
    ASSERT_TRUE(dynamic_cast<ExplodeBitmapPanel *>(desktop->getContainer()->get_dialog(dialog)));
    closePanel();
    win->lookup_action(action)->activate();
    ASSERT_TRUE(dynamic_cast<ExplodeBitmapPanel *>(desktop->getContainer()->get_dialog(dialog)));
    closePanel();
    win->lookup_action("dialog-open")->activate(Glib::ustring(dialog));
    ASSERT_TRUE(desktop->getContainer()->get_dialog(dialog));
    closePanel();

    win->lookup_action(action)->activate();
    auto panel = desktop->getContainer()->get_dialog(dialog);
    ASSERT_TRUE(panel);
    prefs->setBool(Bitmap::explodeBitmapPreference, false);
    // Opt-out must close the panel synchronously, before a main-loop/restart.
    EXPECT_EQ(desktop->getContainer()->get_dialog(dialog), nullptr);
    registry(false); // Includes the already initialized discovery list.
    win->lookup_action(action)->activate(); // Old menu or cached search row cannot bypass opt-in.
    win->lookup_action("dialog-open")->activate(Glib::ustring(dialog));
    EXPECT_EQ(desktop->getContainer()->get_dialog(dialog), nullptr);
    prefs->setBool(Bitmap::explodeBitmapPreference, true);
    win->lookup_action(action)->activate();
    EXPECT_NE(desktop->getContainer()->get_dialog(dialog), nullptr);
}

TEST_F(ExplodeBitmapRegister, PaletteSearchThenUnregisteredHistoryIsHarmless)
{
    auto win = window(true);
    win->present();
    drain();
    auto base = namedWidget(*win, "CommandPaletteBase");
    ASSERT_TRUE(base);
    auto filter = namedWidget(*base, "CPFilter");
    auto suggestions = dynamic_cast<Gtk::ListBox *>(namedWidget(*base, "CPSuggestions"));
    ASSERT_TRUE(filter);
    ASSERT_TRUE(suggestions);
    // Open and search through the real window palette, not action metadata.
    win->lookup_action("canvas-command-palette")->activate();
    drain(); // The normal palette action opens on idle to avoid menu focus stealing.
    gtk_editable_set_text(GTK_EDITABLE(filter->gobj()), "Explode Bitmap");
    g_signal_emit_by_name(filter->gobj(), "search-changed");
    auto row = actionRow(*suggestions);
    ASSERT_TRUE(row);
    ASSERT_TRUE(row->get_child_visible());
    g_signal_emit_by_name(suggestions->gobj(), "row-activated", row->gobj());
    ASSERT_NE(desktop->getContainer()->get_dialog(dialog), nullptr);
    win->lookup_action("canvas-command-palette")->activate();
    drain();
    ASSERT_TRUE(enterHistory(*filter));
    Gtk::ListBoxRow *history_row = nullptr;
    // The history scroll is the suggestions scroll's sibling.
    auto scroll = suggestions->get_parent()->get_parent();
    ASSERT_TRUE(scroll->get_next_sibling());
    history_row = actionRow(*scroll->get_next_sibling());
    ASSERT_TRUE(history_row);
    auto history = dynamic_cast<Gtk::ListBox *>(history_row->get_parent());
    ASSERT_TRUE(history);

    prefs->setBool(Bitmap::explodeBitmapPreference, false);
    add_actions_dialogs(win); // Simulate restart registration with opt-in off.
    ASSERT_FALSE(win->lookup_action(action));
    g_signal_emit_by_name(history->gobj(), "row-activated", history_row->gobj());
    EXPECT_EQ(desktop->getContainer()->get_dialog(dialog), nullptr);
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog(dialog), nullptr);
    // The persisted history keeps the entry.
    CPHistoryXML persisted;
    auto operations = persisted.get_operation_history();
    ASSERT_TRUE(std::any_of(operations.begin(), operations.end(), [](auto const &entry) {
        return entry.history_type == HistoryType::ACTION && entry.data == detailed;
    }));
    // A newly constructed palette may still list it: history is restored before window actions exist, as upstream does.
    // Activation re-resolves the action by name and does nothing while it is unregistered.
    CommandPalette restarted;
    if (auto row = actionRow(restarted.get_base_widget())) {
        auto list = dynamic_cast<Gtk::ListBox *>(row->get_parent());
        ASSERT_TRUE(list);
        g_signal_emit_by_name(list->gobj(), "row-activated", row->gobj());
    }
    EXPECT_EQ(desktop->getContainer()->get_dialog(dialog), nullptr);
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog(dialog), nullptr);
}

TEST_F(ExplodeBitmapRegister, OptOutClosesFloatingPanelAndPreservesOtherDialogs)
{
    window(true);
    for (bool other_tab : {true, false}) {
        prefs->setBool(Bitmap::explodeBitmapPreference, true);
        DialogManager::singleton().remove_dialog_floating_state(dialog);
        auto floating = desktop->getContainer()->new_floating_dialog(dialog);
        ASSERT_TRUE(floating);
        if (other_tab) {
            floating->get_container()->new_dialog("FillStroke", nullptr, false);
            ASSERT_NE(floating->get_container()->get_dialog("FillStroke"), nullptr);
        }
        prefs->setBool(Bitmap::explodeBitmapPreference, false);
        EXPECT_EQ(DialogManager::singleton().find_floating_dialog(dialog), nullptr);
        EXPECT_EQ(desktop->getContainer()->new_floating_dialog(dialog), nullptr);
        if (other_tab) {
            EXPECT_EQ(floating->get_container()->get_dialog(dialog), nullptr);
            EXPECT_NE(floating->get_container()->get_dialog("FillStroke"), nullptr);
            floating->close();
        }
        drain();
    }
    DialogManager::singleton().remove_dialog_floating_state(dialog);
}

TEST_F(ExplodeBitmapRegister, SessionRoundTripRestoresOnlyWhenEnabled)
{
    auto win = window(true);
    win->lookup_action(action)->activate();
    ASSERT_TRUE(desktop->getContainer()->get_dialog(dialog));
    auto state = desktop->getContainer()->save_container_state();
    ASSERT_NE(state->to_data().raw().find(dialog), std::string::npos);
    closePanel();
    for (bool enabled : {false, true, false}) {
        prefs->setBool(Bitmap::explodeBitmapPreference, enabled);
        Gtk::Window host;
        DialogContainer restored(win);
        host.set_child(restored);
        gtk_widget_realize(GTK_WIDGET(host.gobj()));
        restored.load_container_state(state.get(), false);
        EXPECT_EQ(restored.get_dialog(dialog) != nullptr, enabled);
        EXPECT_FALSE(host.get_visible()); // Restoration does not focus/present a hidden host.
    }
}

TEST_F(ExplodeBitmapRegister, FilteredRestoreKeepsTheOtherActiveTab)
{
    auto win = window(false);
    auto state = Glib::KeyFile::create();
    state->load_from_data(
        "[Windows]\nCount=1\n[Window0]\nColumnCount=1\nFloating=false\n"
        "[Window0Column0]\nNotebookCount=1\nBeforeCanvas=false\n"
        "Notebook0Dialogs=ExplodeBitmap;FillStroke;\nNotebook0ActiveTab=1\n");
    Gtk::Window host;
    DialogContainer restored(win);
    host.set_child(restored);
    gtk_widget_realize(GTK_WIDGET(host.gobj()));
    restored.load_container_state(state.get(), false);
    EXPECT_EQ(restored.get_dialog(dialog), nullptr);
    auto fill = restored.get_dialog("FillStroke");
    ASSERT_TRUE(fill);
    auto notebook = DialogNotebook::get_page_notebook(*fill);
    EXPECT_EQ(notebook->get_nth_page(notebook->get_current_page()), fill);
}

TEST_F(ExplodeBitmapRegister, DisabledFloatingRestoreDoesNotPublishAnEmptyWindow)
{
    auto win = window(false);
    auto state = Glib::KeyFile::create();
    state->load_from_data(
        "[Windows]\nCount=2\n[Window0]\nColumnCount=0\nFloating=false\n"
        "[Window1]\nColumnCount=1\nFloating=true\n"
        "[Window1Column0]\nNotebookCount=1\nBeforeCanvas=false\n"
        "Notebook0Dialogs=ExplodeBitmap;\nNotebook0ActiveTab=0\n");
    auto before = application().gtk_app()->get_windows().size();
    desktop->getContainer()->load_container_state(state.get(), true);
    drain();
    EXPECT_EQ(application().gtk_app()->get_windows().size(), before);
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog(dialog), nullptr);
    EXPECT_EQ(desktop->getContainer()->get_dialog(dialog), nullptr);
    EXPECT_FALSE(win->lookup_action(action));
}

TEST_F(ExplodeBitmapRegister, PreferencesCheckboxControlsOptInAndDisclosesRestart)
{
    auto win = window(false);
    prefs->setInt("/dialogs/preferences/page", PREFS_PAGE_BEHAVIOR);
    InkscapePreferences panel;
    Gtk::Window host;
    host.set_child(panel);
    host.present();
    drain();
    auto checkbox = findWidget<Gtk::CheckButton>(panel, "Explode Bitmap (internal test)");
    ASSERT_TRUE(checkbox);
    EXPECT_FALSE(checkbox->get_active());
    ASSERT_TRUE(findWidget<Gtk::Label>(panel, "Restart VA Studio to update menus and action search."));
    checkbox->set_active(true);
    EXPECT_TRUE(Bitmap::explodeBitmapEnabled());
    add_actions_dialogs(win);
    win->lookup_action(action)->activate();
    ASSERT_NE(desktop->getContainer()->get_dialog(dialog), nullptr);
    checkbox->set_active(false);
    EXPECT_FALSE(Bitmap::explodeBitmapEnabled());
    EXPECT_EQ(desktop->getContainer()->get_dialog(dialog), nullptr);
}

TEST_F(ExplodeBitmapRegister, CombineAndBreakApartShortcutsRemainUnchanged)
{
    window(false);
    auto &keys = Shortcuts::getInstance();
    auto breaks = keys.get_triggers("app.break-apart");
    auto combines = keys.get_triggers("app.path-combine");
#ifdef __APPLE__
    auto modifier = Gdk::ModifierType::META_MASK; // Existing platform mapping of <primary>.
#else
    auto modifier = Gdk::ModifierType::CONTROL_MASK;
#endif
    EXPECT_EQ(breaks, (std::vector<Glib::ustring>{Gtk::AccelKey('k', modifier).get_abbrev()}));
    EXPECT_EQ(combines, (std::vector<Glib::ustring>{Gtk::AccelKey('l', modifier).get_abbrev()}));
    prefs->setBool(Bitmap::explodeBitmapPreference, true);
    add_actions_dialogs(desktop->getInkscapeWindow());
    EXPECT_EQ(keys.get_triggers("app.break-apart"), breaks);
    EXPECT_EQ(keys.get_triggers("app.path-combine"), combines);
    EXPECT_TRUE(keys.get_triggers(detailed).empty());
    EXPECT_TRUE(keys.get_triggers("win.dialog-open('ExplodeBitmap')").empty());
}

// True when a menu item (at any depth) opens `dialog`, in GtkBuilder form (action + target)
// or as a detailed action name, as the canvas context menu builds it.
bool opensDialog(GMenuModel *model, std::string const &dialog)
{
    auto string = [&](int i, char const *attribute) {
        std::string result;
        if (auto value = g_menu_model_get_item_attribute_value(model, i, attribute, G_VARIANT_TYPE_STRING)) {
            result = g_variant_get_string(value, nullptr);
            g_variant_unref(value);
        }
        return result;
    };
    for (int i = 0; model && i < g_menu_model_get_n_items(model); ++i) {
        auto action = string(i, G_MENU_ATTRIBUTE_ACTION);
        if ((action == "win.dialog-open" && string(i, G_MENU_ATTRIBUTE_TARGET) == dialog) ||
            action == "win.dialog-open('" + dialog + "')") return true;
        for (auto link : {G_MENU_LINK_SECTION, G_MENU_LINK_SUBMENU}) {
            if (auto child = g_menu_model_get_item_link(model, i, link)) {
                bool found = opensDialog(child, dialog);
                g_object_unref(child);
                if (found) return true;
            }
        }
    }
    return false;
}

void menuActions(GMenuModel *model, std::vector<std::string> &actions)
{
    for (int i = 0; model && i < g_menu_model_get_n_items(model); ++i) {
        if (auto value = g_menu_model_get_item_attribute_value(model, i, G_MENU_ATTRIBUTE_ACTION, G_VARIANT_TYPE_STRING)) {
            actions.emplace_back(g_variant_get_string(value, nullptr));
            g_variant_unref(value);
        }
        for (auto link : {G_MENU_LINK_SECTION, G_MENU_LINK_SUBMENU}) {
            if (auto child = g_menu_model_get_item_link(model, i, link)) {
                menuActions(child, actions);
                g_object_unref(child);
            }
        }
    }
}

void textViews(Gtk::Widget &root, std::vector<Gtk::TextView *> &views)
{
    if (auto view = dynamic_cast<Gtk::TextView *>(&root)) views.push_back(view);
    for (auto child = root.get_first_child(); child; child = child->get_next_sibling()) textViews(*child, views);
}

int notebooks(Gtk::Widget &root)
{
    int count = dynamic_cast<DialogNotebook *>(&root) ? 1 : 0;
    for (auto child = root.get_first_child(); child; child = child->get_next_sibling()) count += notebooks(*child);
    return count;
}

constexpr auto spellcheck = "Spellcheck";
class SpellcheckHidden : public ExplodeBitmapRegister {};

TEST_F(SpellcheckHidden, NoRegistryMenuSearchShortcutContextMenuOrFactory)
{
    static_assert(!UI::spellcheckUiEnabled);
    auto win = window(false, "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
                             "<text id='t' x='10' y='20'>Spell</text></svg>");
    auto list = get_dialog_data_list();
    EXPECT_EQ(std::count_if(list.begin(), list.end(), [](auto const &d) { return d.key == spellcheck; }), 0);
    EXPECT_EQ(get_dialog_data().count(spellcheck), 0u);

    auto builder = Gtk::Builder::create_from_file(IO::Resource::get_filename(IO::Resource::UIS, "menus.ui"));
    auto text_menu = builder->get_object<Gio::MenuModel>("text-menu");
    ASSERT_TRUE(text_menu);
    EXPECT_TRUE(opensDialog(text_menu->gobj(), "Text"));
    EXPECT_FALSE(opensDialog(text_menu->gobj(), spellcheck));

    auto &keys = Shortcuts::getInstance();
    auto entries = keys.list_all_detailed_action_names();
    EXPECT_EQ(std::count(entries.begin(), entries.end(), "win.dialog-open('Spellcheck')"), 0);
    EXPECT_TRUE(keys.get_triggers("win.dialog-open('Spellcheck')").empty());

    auto text = cast<SPItem>(desktop->getDocument()->getObjectById("t"));
    ASSERT_TRUE(text);
    ContextMenu menu(desktop, text, {text});
    EXPECT_TRUE(opensDialog(menu.get_menu_model()->gobj(), "Text"));
    EXPECT_FALSE(opensDialog(menu.get_menu_model()->gobj(), spellcheck));

    auto container = desktop->getContainer();
    win->lookup_action("dialog-open")->activate(Glib::ustring(spellcheck));
    container->new_dialog(spellcheck);
    container->new_dialog(spellcheck, nullptr, false);
    EXPECT_EQ(container->new_floating_dialog(spellcheck), nullptr);
    EXPECT_EQ(container->get_dialog(spellcheck), nullptr);
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog(spellcheck), nullptr);
}

TEST_F(SpellcheckHidden, SavedDockedTabsRestoreWithoutAnEmptyNotebook)
{
    auto win = window(false);
    auto state = Glib::KeyFile::create();
    state->load_from_data(
        "[Windows]\nCount=1\n[Window0]\nColumnCount=1\nFloating=false\n"
        "[Window0Column0]\nNotebookCount=2\nBeforeCanvas=false\n"
        "Notebook0Dialogs=Spellcheck;FillStroke;\nNotebook0ActiveTab=1\n"
        "Notebook1Dialogs=Spellcheck;\nNotebook1ActiveTab=0\n");
    Gtk::Window host;
    DialogContainer restored(win);
    host.set_child(restored);
    gtk_widget_realize(GTK_WIDGET(host.gobj()));
    restored.load_container_state(state.get(), false);
    EXPECT_EQ(restored.get_dialog(spellcheck), nullptr);
    auto fill = restored.get_dialog("FillStroke");
    ASSERT_TRUE(fill);
    auto notebook = DialogNotebook::get_page_notebook(*fill);
    EXPECT_EQ(notebook->get_nth_page(notebook->get_current_page()), fill);
    EXPECT_EQ(notebooks(restored), 1); // The Spellcheck-only notebook is not restored empty.
}

TEST_F(SpellcheckHidden, SavedFloatingWindowIsSkippedAndLaterWindowsRestore)
{
    window(false);
    DialogManager::singleton().remove_dialog_floating_state("FillStroke");
    auto state = Glib::KeyFile::create();
    state->load_from_data(
        "[Windows]\nCount=3\n[Window0]\nColumnCount=0\nFloating=false\n"
        "[Window1]\nColumnCount=1\nFloating=true\n"
        "[Window1Column0]\nNotebookCount=1\nBeforeCanvas=false\n"
        "Notebook0Dialogs=Spellcheck;\nNotebook0ActiveTab=0\n"
        "[Window2]\nColumnCount=1\nFloating=true\n"
        "[Window2Column0]\nNotebookCount=1\nBeforeCanvas=false\n"
        "Notebook0Dialogs=FillStroke;\nNotebook0ActiveTab=0\n");
    auto before = application().gtk_app()->get_windows().size();
    desktop->getContainer()->load_container_state(state.get(), true);
    drain();
    EXPECT_EQ(DialogManager::singleton().find_floating_dialog(spellcheck), nullptr);
    auto fill = DialogManager::singleton().find_floating_dialog("FillStroke");
    ASSERT_TRUE(fill); // Skipping the Spellcheck-only window does not abort the restore.
    EXPECT_EQ(application().gtk_app()->get_windows().size(), before + 1); // No empty window.
    if (auto floating = dynamic_cast<DialogWindow *>(fill->get_root())) floating->close();
    drain();
    DialogManager::singleton().remove_dialog_floating_state("FillStroke");
}

TEST_F(SpellcheckHidden, PreferencesHaveNoSpellcheckPageAndASavedOneOpensTools)
{
    window(false);
    prefs->setInt("/dialogs/preferences/page", PREFS_PAGE_SPELLCHECK);
    InkscapePreferences panel;
    Gtk::Window host;
    host.set_child(panel);
    host.present();
    drain();
    EXPECT_EQ(prefs->getInt("/dialogs/preferences/page", -1), PREFS_PAGE_TOOLS);
    EXPECT_EQ(findWidget<Gtk::CheckButton>(panel, "Ignore words with digits"), nullptr);
    EXPECT_EQ(findWidget<Gtk::CheckButton>(panel, "Ignore words in ALL CAPITALS"), nullptr);
}

TEST_F(SpellcheckHidden, FontBrowserTextHasNoSpellingMenu)
{
    window(false);
    auto container = desktop->getContainer();
    container->new_dialog("FontBrowser");
    auto browser = container->get_dialog("FontBrowser");
    ASSERT_TRUE(browser);
    std::vector<Gtk::TextView *> views;
    textViews(*browser, views);
    ASSERT_FALSE(views.empty());
    // The libspelling adapter adds its "spelling.*" suggestions to the text view's extra menu.
    for (auto view : views) {
        std::vector<std::string> actions;
        if (auto extra = view->get_extra_menu()) menuActions(extra->gobj(), actions);
        std::string listed;
        for (auto const &action : actions) listed += action + " ";
        // GtkSourceView's own "source.change-case" entry is expected.
        EXPECT_EQ(std::count_if(actions.begin(), actions.end(), [](auto const &a) { return a.starts_with("spelling."); }), 0)
            << G_OBJECT_TYPE_NAME(view->gobj()) << " extra menu: " << listed;
    }
}
} // namespace
