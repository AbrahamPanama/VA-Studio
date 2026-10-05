// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief A widget that manages DialogNotebook's and other widgets inside a horizontal DialogMultipaned.
 *
 * Authors: see git history
 *   Tavmjong Bah
 *
 * Copyright (c) 2018 Tavmjong Bah, Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "dialog-container.h"
#include "ui/dialog/explode-bitmap.h"
#include "ui/explode-bitmap-feature.h"
#include "ui/spellcheck-feature.h"

#include "enums.h"
#include "inkscape.h"
#include "inkscape-window.h"
#include "message-stack.h"
#include "ui/dialog/artwork-library-controller.h"
#include "ui/dialog/bitmap-adjustments.h"
#include "ui/dialog/clonetiler.h"
#include "ui/dialog/debug.h"
#include "ui/dialog/dialog-data.h"
#include "ui/dialog/dialog-multipaned.h"
#include "ui/dialog/dialog-window.h"
#include "ui/dialog/document-properties.h"
#include "ui/dialog/document-resources.h"
#include "ui/dialog/export.h"
#include "ui/dialog/extensions-gallery.h"
#include "ui/dialog/fill-and-stroke.h"
#include "ui/dialog/filter-effects-dialog.h"
#include "ui/dialog/find.h"
#include "ui/dialog/font-collections-manager.h"
#include "ui/dialog/icon-preview.h"
#include "ui/dialog/inkscape-preferences.h"
#include "ui/dialog/livepatheffect-editor.h"
#include "ui/dialog/object-attributes.h"
#include "ui/dialog/objects.h"
#include "ui/dialog/selectorsdialog.h"
#if WITH_LIBSPELLING
#include "ui/dialog/spellcheck.h"
#endif
#include <gtkmm/viewport.h>

#include "ui/dialog/svg-fonts-dialog.h"
#include "ui/dialog/swatches.h"
#include "ui/dialog/symbols.h"
#include "ui/dialog/text-edit.h"
#include "ui/dialog/text-panel.h"
#include "ui/dialog/tile.h"
#include "ui/dialog/tracedialog.h"
#include "ui/dialog/transformation.h"
#include "ui/dialog/undo-history.h"
#include "ui/dialog/xml-tree.h"
#include "ui/themes.h"
#include "ui/util.h"
#include "ui/widget/canvas-grid.h"
#include "util/scope_exit.h"
#include <glibmm/markup.h>

namespace Inkscape::UI::Dialog {

namespace {
using ExplodeObserver = std::shared_ptr<Preferences::PreferencesObserver>;
// C++ container lifetime can outlast the GTK object during window teardown.
std::map<DialogContainer *, ExplodeObserver> explode_observers;
}

// Tear down children while the dialog map is still alive: disconnect column callbacks
// first, then empty the map, then explicitly reset _columns. Without the explicit reset,
// automatic member destruction would free `dialogs` before `_columns`, so
// ~DialogNotebook's unlink_dialog() would touch an already-destroyed map.
DialogContainer::~DialogContainer()
{
    explode_observers.erase(this);
    _connections.clear();
    dialogs.clear();
    _columns.reset();
}

DialogContainer::DialogContainer(InkscapeWindow* inkscape_window)
    : _inkscape_window(inkscape_window)
    , _drop_gtypes{UI::Widget::TabStrip::get_dnd_source_type()}
{
    g_assert(_inkscape_window != nullptr);

    set_name("DialogContainer");
    add_css_class("DialogContainer");

    _columns = std::make_unique<DialogMultipaned>(Gtk::Orientation::HORIZONTAL);
    setup_drag_and_drop(_columns.get());
    append(*_columns.get());

    // Each dock/floating container enforces opt-out immediately, independently
    // of the Preferences page or cached menu/search registration.
    auto observer = Preferences::get()->createObserver(Bitmap::explodeBitmapPreference, [this] {
        if (Bitmap::explodeBitmapEnabled()) {
            if (auto panel = get_dialog("ExplodeBitmap")) panel->set_sensitive(true);
            return;
        }
        // Closing the last floating tab can destroy this container. Retain the
        // observer through its own notification and stop if its owner dies.
        auto keep_alive = explode_observers.at(this);
        sigc::slot<bool()> owner_alive = sigc::track_object([] { return true; }, *this);
        while (!owner_alive.empty()) {
            auto panel = get_dialog("ExplodeBitmap");
            if (!panel) break;
            DialogNotebook *notebook = nullptr;
            for (auto parent = panel->get_parent(); parent; parent = parent->get_parent()) {
                if ((notebook = dynamic_cast<DialogNotebook *>(parent))) break;
            }
            if (!notebook) {
                panel->set_sensitive(false); // Also refuse access during reparenting.
                break;
            }
            notebook->close_tab(panel);
        }
    });
    explode_observers.emplace(this, std::move(observer));
}

std::unique_ptr<DialogMultipaned> DialogContainer::create_column()
{
    auto column = std::make_unique<DialogMultipaned>(Gtk::Orientation::VERTICAL);
    setup_drag_and_drop(column.get());
    _connections.emplace_back(column->signal_now_empty().connect(
        sigc::bind(sigc::mem_fun(*this, &DialogContainer::column_empty), column.get())));
    return column;
}

void DialogContainer::setup_drag_and_drop(DialogMultipaned* column) {
    // TODO: this is probably incorrect; for each new instance of column we remember subscriptions,
    // but don't seem to release them when this columns is destroyed
    _connections.emplace_back(column->signal_dock_dialog().connect([this,column](auto& page, auto& src_notebook, auto location, auto dest) {
        return dock_dialog(page, src_notebook, location, column, dest);
    }));
    _connections.emplace_back(column->signal_float_dialog().connect([this](auto& page, auto& src_notebook) {
        return src_notebook.float_tab(page) != nullptr;
    }));

    column->set_drop_gtypes(_drop_gtypes);
}

/**
 * Get an instance of a DialogBase dialog using the associated dialog name.
 */
std::unique_ptr<DialogBase> DialogContainer::dialog_factory(Glib::ustring const &dialog_type,
                                                          std::unique_ptr<ArtworkLibraryPanelReservation> &library_reservation)
{
    if (dialog_type == "ExplodeBitmap") {
        if (!Bitmap::explodeBitmapEnabled()) return nullptr;
        return std::make_unique<ExplodeBitmapPanel>();
    }
    if (dialog_type == "ArtworkLibrary") {
        sigc::slot<bool()> alive = sigc::track_object([] { return true; }, *this);
        try {
            auto host = InkscapeApplication::instance()->artworkLibraries(true);
            library_reservation = host->reserve_panel(_artwork_library_slot);
            if (alive.empty()) return nullptr;
            _artwork_library_slot = library_reservation->slot();
            return std::make_unique<ArtworkLibraryController>(*library_reservation);
        } catch (std::exception const &e) {
            if (!alive.empty() && _inkscape_window) {
                if (auto desktop = _inkscape_window->get_desktop())
                    desktop->messageStack()->flash(ERROR_MESSAGE, Glib::Markup::escape_text(e.what()));
            }
            return nullptr; // Failed construction never publishes a partial tab.
        }
    }
    // clang-format off
         if (dialog_type == "AlignDistribute")    return std::make_unique<ArrangeDialog>();
    else if (dialog_type == "BitmapAdjustments")  return std::make_unique<BitmapAdjustmentsPanel>();
    else if (dialog_type == "CloneTiler")         return std::make_unique<CloneTiler>();
    else if (dialog_type == "DebugWindow")        return std::make_unique<Debug>();
    else if (dialog_type == "DocumentProperties") return std::make_unique<DocumentProperties>();
    else if (dialog_type == "DocumentResources")  return std::make_unique<DocumentResources>();
    else if (dialog_type == "Export")             return std::make_unique<Export>();
    else if (dialog_type == "ExtensionsGallery")  return std::make_unique<ExtensionsGallery>(ExtensionsGallery::Effects);
    else if (dialog_type == "FillStroke")         return std::make_unique<FillAndStroke>();
    else if (dialog_type == "FilterEffects")      return std::make_unique<FilterEffectsDialog>();
    else if (dialog_type == "FilterGallery")      return std::make_unique<ExtensionsGallery>(ExtensionsGallery::Filters);
    else if (dialog_type == "Find")               return std::make_unique<Find>();
    else if (dialog_type == "FontBrowser")        return std::make_unique<TextEdit>(true);
    else if (dialog_type == "FontCollections")    return std::make_unique<FontCollectionsManager>();
    else if (dialog_type == "IconPreview")        return std::make_unique<IconPreviewPanel>();
    else if (dialog_type == "LivePathEffect")     return std::make_unique<LivePathEffectEditor>();
    else if (dialog_type == "ObjectProperties")   return std::make_unique<ObjectAttributes>();
    else if (dialog_type == "Objects")            return std::make_unique<ObjectsPanel>();
    else if (dialog_type == "Preferences")        return std::make_unique<InkscapePreferences>();
    else if (dialog_type == "Selectors")          return std::make_unique<SelectorsDialog>();
    else if (dialog_type == "SVGFonts")           return std::make_unique<SvgFontsDialog>();
    else if (dialog_type == "Swatches")           return std::make_unique<SwatchesPanel>(SwatchesPanel::Dialog);
    else if (dialog_type == "Symbols")            return std::make_unique<SymbolsDialog>();
    else if (dialog_type == "Text")               return std::make_unique<TextPanel>();
    else if (dialog_type == "Trace")              return TraceDialog::create();
    else if (dialog_type == "Transform")          return std::make_unique<Transformation>();
    else if (dialog_type == "UndoHistory")        return std::make_unique<UndoHistory>();
    else if (dialog_type == "XMLEditor")          return std::make_unique<XmlTree>();
#if WITH_LIBSPELLING
    else if (dialog_type == "Spellcheck" && UI::spellcheckUiEnabled) return std::make_unique<SpellCheck>();
#endif
#ifdef DEBUG
    else if (dialog_type == "Prototype")          return std::make_unique<Prototype>();
#endif
    else {
        std::cerr << "DialogContainer::dialog_factory: Unhandled dialog: " << dialog_type.raw() << std::endl;
        return nullptr;
    }
    // clang-format on
}

// find dialog's multipaned parent; is there a better way?
DialogMultipaned* get_dialog_parent(DialogBase* dialog) {
    if (!dialog) return nullptr;

    // dialogs are nested inside Gtk::Notebook
    if (auto notebook = DialogNotebook::get_page_notebook(*dialog)) {
        // notebooks are inside viewport, inside scrolled window
        if (auto viewport = dynamic_cast<Gtk::Viewport*>(notebook->get_parent())) {
            if (auto scroll = dynamic_cast<Gtk::ScrolledWindow*>(viewport->get_parent())) {
                // finally get the panel
                if (auto panel = dynamic_cast<DialogMultipaned*>(scroll->get_parent())) {
                    return panel;
                }
            }
        }
    }

    return nullptr;
}

/**
 * Add new dialog to the current container or in a floating window, based on preferences.
 */
void DialogContainer::new_dialog(const Glib::ustring& dialog_type)
{
    if (dialog_type == "ExplodeBitmap" && !Bitmap::explodeBitmapEnabled()) return;
    if (reuse_owned_preferences(dialog_type, true)) return;
    if (dialog_type == "ArtworkLibrary" && *_opening_artwork_library) return;
    sigc::slot<bool()> alive = sigc::track_object([] { return true; }, *this);
    // Open all dialogs as floating, if set in preferences
    auto prefs = Inkscape::Preferences::get();
    int dockable = prefs->getInt("/options/dialogtype/value", PREFS_DIALOGS_BEHAVIOR_DOCKABLE);
    bool floating = DialogManager::singleton().should_open_floating(dialog_type);
    if (dockable == PREFS_DIALOGS_BEHAVIOR_FLOATING || floating) {
        new_floating_dialog(dialog_type);
    } else {
        new_dialog(dialog_type, nullptr, true);
        return; // The explicit docked/menu path already restores focus.
    }

    if (alive.empty()) return;
    if (DialogBase* dialog = find_existing_dialog(dialog_type)) {
        dialog->focus_dialog();
    }
}

DialogBase* DialogContainer::find_existing_dialog(const Glib::ustring& dialog_type) {
    DialogBase *existing_dialog = get_dialog(dialog_type);
    if (!existing_dialog && dialog_type == "ArtworkLibrary") {
        auto host = InkscapeApplication::instance()->artworkLibraries();
        return host ? dynamic_cast<DialogBase *>(host->live_panel(_artwork_library_slot)) : nullptr;
    }
    if (!existing_dialog) {
        existing_dialog = DialogManager::singleton().find_floating_dialog(dialog_type);
    }
    return existing_dialog;
}

/**
 * Overloaded new_dialog
 */
void DialogContainer::new_dialog(const Glib::ustring& dialog_type, DialogNotebook* notebook, bool ensure_visibility)
{
    if (dialog_type == "ExplodeBitmap" && !Bitmap::explodeBitmapEnabled()) return;
    if (reuse_owned_preferences(dialog_type, ensure_visibility)) return;
    if (dialog_type == "ArtworkLibrary" && *_opening_artwork_library) return;
    sigc::slot<bool()> container_alive = sigc::track_object([] { return true; }, *this);
    sigc::slot<bool()> notebook_alive;
    if (notebook) notebook_alive = sigc::track_object([] { return true; }, *notebook);
    if (ensure_visibility) {
        _columns->ensure_multipaned_children();
        if (container_alive.empty() || (notebook && notebook_alive.empty())) return;
    }

    // Limit each container to containing one of any type of dialog.
    if (DialogBase* existing_dialog = find_existing_dialog(dialog_type)) {
        sigc::slot<bool()> alive = sigc::track_object([] { return true; }, *this, *existing_dialog);
        // make sure parent window is not hidden/collapsed
        if (auto panel = get_dialog_parent(existing_dialog)) {
            panel->set_visible(true);
        }
        if (alive.empty() || !DialogNotebook::get_page_notebook(*existing_dialog)) return;
        // found existing dialog; blink & exit
        existing_dialog->blink();
        // Explicit notebook-menu opens must also raise an unmapped floating
        // window. Automatic tab switching deliberately does not present one.
        if (ensure_visibility && !alive.empty() && existing_dialog->get_root()) existing_dialog->focus_dialog();
        return;
    }

    // should new dialog be floating?
    bool floating = DialogManager::singleton().should_open_floating(dialog_type);
    int dockable = Preferences::get()->getInt("/options/dialogtype/value", PREFS_DIALOGS_BEHAVIOR_DOCKABLE);
    if (!notebook && (floating || dockable == PREFS_DIALOGS_BEHAVIOR_FLOATING)) {
        new_floating_dialog(dialog_type);
        if (container_alive.empty()) return;
        if (DialogBase* dialog = find_existing_dialog(dialog_type)) {
            dialog->focus_dialog();
        }
        return;
    }

    auto opening = _opening_artwork_library;
    bool const library = dialog_type == "ArtworkLibrary";
    if (library) *opening = true;
    auto finish_opening = scope_exit{[opening, library] { if (library) *opening = false; }};
    // Create the dialog widget
    std::unique_ptr<ArtworkLibraryPanelReservation> library_reservation;
    auto owned_dialog = dialog_factory(dialog_type, library_reservation);
    if (container_alive.empty() || (notebook && notebook_alive.empty())) return;
    auto dialog = owned_dialog.release(); // Ownership transfers to GTK below.

    if (!dialog) {
        std::cerr << "DialogContainer::new_dialog(): couldn't find dialog for: " << dialog_type.raw() << std::endl;
        return;
    }

    // manage the dialog instance
    dialog = Gtk::manage(dialog);

    // If not from notebook menu add at top of last column.
    if (!notebook) {
        // Look to see if last column contains a multipane. If not, add one.
        auto last_column = dynamic_cast<DialogMultipaned*>(_columns->get_last_widget());
        if (!last_column) {
            auto col = create_column();
            last_column = col.get();
            _columns->append(std::move(col));
        }

        // Look to see if first widget in column is notebook, if not add one.
        notebook = dynamic_cast<DialogNotebook*>(last_column->get_first_widget());
        if (!notebook) {
            auto nb = std::make_unique<DialogNotebook>(this);
            notebook = nb.get();
            last_column->prepend(std::move(nb));
        }
    }

    // Add dialog
    sigc::slot<bool()> alive = sigc::track_object([] { return true; }, *this, *notebook, *dialog);
    notebook->add_page(*dialog);
    if (alive.empty()) return;

    if (auto panel = dynamic_cast<DialogMultipaned*>(notebook->get_parent())) {
        // if panel is collapsed, show it now, or else new dialog will be mysteriously missing
        panel->set_visible(true);
    }
    if (ensure_visibility && !alive.empty() && dialog->get_root()) dialog->focus_dialog();
}

[[nodiscard]] static auto get_key(std::size_t const notebook_idx)
{
    return Glib::ustring::compose("Notebook%1Dialogs", notebook_idx);
}
static auto library_slot_key(std::size_t notebook_idx) {
    return Glib::ustring::compose("Notebook%1ArtworkLibrarySlot", notebook_idx);
}
static void save_library_slot(Glib::KeyFile &state, Glib::ustring const &group, std::size_t index, DialogNotebook &notebook) {
    for (auto &page : UI::notebook_pages(*notebook.get_notebook())) {
        if (auto library = dynamic_cast<ArtworkLibraryController *>(&page)) {
            // Session hint only: never a filesystem path or authority to load a file.
            state.set_string(group, library_slot_key(index), library->host_slot());
            break;
        }
    }
}

// recreate dialogs hosted (docked) in a floating DialogWindow; window will be created
DialogContainer::RestoreResult DialogContainer::recreate_dialogs_from_state(InkscapeWindow* inkscape_window, const Glib::KeyFile* keyfile,
                                                std::string *artwork_library_slot)
{
    g_assert(inkscape_window != nullptr);

    auto restored = RestoreResult::NotApplicable;
    // Step 1: check if we want to load the state
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    int save_state = prefs->getInt("/options/savedialogposition/value", PREFS_DIALOGS_STATE_SAVE);
    if (save_state == PREFS_DIALOGS_STATE_NONE) {
        return restored; // User has turned off this feature in Preferences
    }

    // if it isn't dockable, all saved docked dialogs are made floating
    bool is_dockable =
        prefs->getInt("/options/dialogtype/value", PREFS_DIALOGS_BEHAVIOR_DOCKABLE) != PREFS_DIALOGS_BEHAVIOR_FLOATING;

    if (!is_dockable)
        return RestoreResult::NotApplicable; // not applicable if docking is off

    // Step 2: get the number of windows; should be 1
    int windows_count = 0;
    try {
        // we may have no 'Windows' initially when recreating floating dialog state (state is empty)
        if (keyfile->has_group("Windows") && keyfile->has_key("Windows", "Count")) {
            windows_count = keyfile->get_integer("Windows", "Count");
        }
    } catch (Glib::Error const &error) {
        std::cerr << G_STRFUNC << ": " << error.what() << std::endl;
    }

    // Step 3: for each window, load its state.
    for (int window_idx = 0; window_idx < windows_count; ++window_idx) {
        Glib::ustring group_name = "Window" + std::to_string(window_idx);

        // Step 3.0: get the window's container columns where we want to create the dialogs
        DialogWindow *dialog_window = new DialogWindow(inkscape_window, nullptr);
        DialogContainer *active_container = dialog_window->get_container();
        DialogMultipaned *active_columns = active_container ? active_container->get_columns() : nullptr;

        if (!active_container || !active_columns) {
            delete dialog_window;
            continue;
        }
        sigc::slot<bool()> alive = sigc::track_object([] { return true; }, *dialog_window, *active_container);

        // Step 3.1: read the window parameters
        bool has_position = keyfile->has_key(group_name, "Position") && keyfile->get_boolean(group_name, "Position");
        window_position_t pos;
        if (has_position) { // floating window position recorded?
            pos.x = keyfile->get_integer(group_name, "x");
            pos.y = keyfile->get_integer(group_name, "y");
            pos.width = keyfile->get_integer(group_name, "width");
            pos.height = keyfile->get_integer(group_name, "height");
            dm_restore_window_position(*dialog_window, pos);
        }
        int column_count = 0;
        try {
            column_count = keyfile->get_integer(group_name, "ColumnCount");
        } catch (Glib::Error const &error) {
            std::cerr << G_STRFUNC << ": " << error.what() << std::endl;
        }

        int owned_skipped = 0;
        bool has_non_owned_entries = false;

        // Step 3.2: for each column, load its state
        for (int column_idx = 0; column_idx < column_count; ++column_idx) {
            Glib::ustring column_group_name = group_name + "Column" + std::to_string(column_idx);

            // Step 3.2.0: read the column parameters
            int notebook_count = 0;
            bool before_canvas = false;
            try {
                notebook_count = keyfile->get_integer(column_group_name, "NotebookCount");
                if (keyfile->has_key(column_group_name, "BeforeCanvas")) {
                    before_canvas = keyfile->get_boolean(column_group_name, "BeforeCanvas");
                }
            } catch (Glib::Error const &error) {
                std::cerr << G_STRFUNC << ": " << error.what() << std::endl;
            }

            // Step 3.2.1: create the column
            auto col = active_container->create_column();
            auto column = col.get();

            before_canvas ? active_columns->prepend(std::move(col)) : active_columns->append(std::move(col));

            // Step 3.2.2: for each noteboook, load its dialogs
            for (int notebook_idx = 0; notebook_idx < notebook_count; ++notebook_idx) {
                auto const key = get_key(notebook_idx);

                // Step 3.2.2.0 read the list of dialogs in the current notebook
                std::vector<Glib::ustring> dialogs;
                try {
                    dialogs = keyfile->get_string_list(column_group_name, key);
                } catch (Glib::Error const &error) {
                    std::cerr << G_STRFUNC << ": " << error.what() << std::endl;
                }

                if (!dialogs.size()) {
                    continue;
                }

                // Drop an owned native Preferences entry before allocating any
                // notebook, so this silent restore never re-adds the retained host.
                for (auto it = dialogs.begin(); it != dialogs.end(); ) {
                    if ((*it == "ExplodeBitmap" && !Bitmap::explodeBitmapEnabled()) ||
                        (*it == "Spellcheck" && !UI::spellcheckUiEnabled) ||
                        active_container->reuse_owned_preferences(*it, false)) {
                        ++owned_skipped;
                        it = dialogs.erase(it);
                    } else {
                        // Any surviving original entry (including one later dropped
                        // as a duplicate, or an unknown type) keeps native behavior.
                        has_non_owned_entries = true;
                        ++it;
                    }
                }
                if (dialogs.empty()) {
                    continue;
                }

                DialogNotebook *notebook = nullptr;
                auto const &dialog_data = get_dialog_data();

                // Step 3.2.2.1 create each dialog in the current notebook
                for (auto type : dialogs) {
                    if (type != "ArtworkLibrary" && DialogManager::singleton().find_floating_dialog(type)) {
                        // avoid duplicates
                        continue;
                    }

                    if (dialog_data.find(type) != dialog_data.end()) {
                        if (!notebook) {
                            auto nb = std::make_unique<DialogNotebook>(active_container);
                            notebook = nb.get();
                            column->append(std::move(nb));
                        }
                        if (type == "ArtworkLibrary") {
                            auto key = library_slot_key(notebook_idx);
                            if (artwork_library_slot && !artwork_library_slot->empty())
                                active_container->_artwork_library_slot = *artwork_library_slot;
                            else if (keyfile->has_key(column_group_name, key))
                                active_container->_artwork_library_slot = keyfile->get_string(column_group_name, key).raw();
                        }
                        active_container->new_dialog(type, notebook, true);
                        if (alive.empty()) return RestoreResult::Aborted;
                        if (type == "ArtworkLibrary" && artwork_library_slot)
                            *artwork_library_slot = active_container->_artwork_library_slot;
                    } else {
                        std::cerr << "recreate_dialogs_from_state: invalid dialog type: " << type.raw() << std::endl;
                    }
                }
            }
        }

        if (active_container->get_dialogs().empty()) {
            delete dialog_window;
            // A window whose only saved panel was the filtered owned Preferences was
            // silently skipped; that is not a refused construction. Continue with any
            // later saved windows instead of aborting the whole restore.
            if (owned_skipped > 0 && !has_non_owned_entries) {
                continue;
            }
            return RestoreResult::Aborted; // Refused construction must not publish an empty window.
        }
        if (!has_position) {
            dialog_window->update_window_size_to_fit_children();
        }

        dialog_window->set_visible(true);
        if (alive.empty()) return RestoreResult::Aborted;

        // Set the style and icon theme of the new menu based on the desktop
        INKSCAPE.themecontext->getChangeThemeSignal().emit();
        INKSCAPE.themecontext->add_gtk_css(true);
        restored = RestoreResult::Restored;
    }

    return restored;
}

/**
 * Add a new floating dialog (or reuse existing one if it's already up)
 */
DialogWindow *DialogContainer::new_floating_dialog(const Glib::ustring& dialog_type)
{
    return create_new_floating_dialog(dialog_type, true);
}

// Reuse the exact receiving application's retained native Preferences host instead
// of building a duplicate widget. Type-gated and side-effect free when the host is
// absent or visibility was not requested. Returns true only when an owned host
// handled the request, in which case the caller must stop immediately.
bool DialogContainer::reuse_owned_preferences(Glib::ustring const &dialog_type, bool ensure_visibility)
{
    if (dialog_type != "Preferences") {
        return false;
    }

    // Origin window for parenting, and the exact creating application. A DialogWindow
    // root carries its fixed creating owner; a plain InkscapeWindow root uses its own
    // attached application owner; only an unrooted/plain-root container falls back to
    // the attached window.
    Gtk::Window *origin = dynamic_cast<Gtk::Window *>(get_root());

    InkscapeApplication *app = nullptr;
    if (auto dialog_window = dynamic_cast<DialogWindow *>(origin)) {
        app = dialog_window->get_application_owner();
    } else if (auto inkscape_window = dynamic_cast<InkscapeWindow *>(origin)) {
        app = inkscape_window->get_application_owner();
    } else if (_inkscape_window) {
        app = _inkscape_window->get_application_owner();
    }

    if (!app || !app->ownedPreferencesWindow()) {
        return false;
    }
    if (!ensure_visibility) {
        return true;
    }

    // presentPreferences may destroy or hide the host and run callbacks; do not
    // touch this container, its members or the application after this call.
    Gtk::Window *parent = origin ? origin : static_cast<Gtk::Window *>(_inkscape_window);
    app->presentPreferences(parent);
    return true;
}

DialogWindow *DialogContainer::create_new_floating_dialog(const Glib::ustring& dialog_type, bool blink)
{
    if (dialog_type == "ExplodeBitmap" && !Bitmap::explodeBitmapEnabled()) return nullptr;
    if (reuse_owned_preferences(dialog_type, blink)) return nullptr;
    if (dialog_type == "ArtworkLibrary" && *_opening_artwork_library) return nullptr;
    sigc::slot<bool()> container_alive = sigc::track_object([] { return true; }, *this);
    // check if this dialog is already open
    if (DialogBase* existing_dialog = find_existing_dialog(dialog_type)) {
        // found existing dialog; blink & exit
        if (blink) {
            sigc::slot<bool()> alive = sigc::track_object([] { return true; }, *existing_dialog);
            existing_dialog->blink();
            if (alive.empty()) return nullptr;
            // show its window if it is hidden
            if (auto window = dynamic_cast<DialogWindow *>(existing_dialog->get_root())) {
                DialogManager::singleton().set_floating_dialog_visibility(window, true);
            }
        }
        return nullptr;
    }

    auto opening = _opening_artwork_library;
    bool const library = dialog_type == "ArtworkLibrary";
    if (library) *opening = true;
    auto finish_opening = scope_exit{[opening, library] { if (library) *opening = false; }};
    // check if this dialog *was* open and floating; if so recreate its window
    if (auto state = DialogManager::singleton().find_dialog_state(dialog_type)) {
        auto slot = _artwork_library_slot;
        auto restored = recreate_dialogs_from_state(_inkscape_window, state.get(), library ? &slot : nullptr);
        if (container_alive.empty()) return nullptr;
        if (library) _artwork_library_slot = std::move(slot);
        if (restored != RestoreResult::NotApplicable) {
            return nullptr;
        }
    }

    // Create the dialog widget
    std::unique_ptr<ArtworkLibraryPanelReservation> library_reservation;
    auto owned_dialog = dialog_factory(dialog_type, library_reservation);
    if (container_alive.empty()) return nullptr;
    DialogBase *dialog = owned_dialog.release(); // Ownership transfers to GTK below.

    if (!dialog) {
        std::cerr << "DialogContainer::new_dialog(): couldn't find dialog for: " << dialog_type.raw() << std::endl;
        return nullptr;
    }

    // manage the dialog instance
    dialog = Gtk::manage(dialog);

    // New temporary noteboook
    auto const notebook = Gtk::make_managed<DialogNotebook>(this);
    sigc::slot<bool()> alive = sigc::track_object([] { return true; }, *this, *notebook, *dialog);
    notebook->add_page(*dialog);
    if (alive.empty()) return nullptr;
    return notebook->pop_tab(dialog);
}

// toggle dialogs (visibility) is invoked on a top container embedded in Inkscape window
void DialogContainer::toggle_dialogs()
{
    // check how many dialog panels are visible and how many are hidden
    // we use this info to decide what it means to toggle visibility
    int visible = 0;
    int hidden = 0;
    for (auto const &child : _columns->get_multipaned_children()) {
        // only examine panels, skip drop zones and handles
        if (auto panel = dynamic_cast<DialogMultipaned*>(child.get())) {
            if (panel->is_visible()) {
                ++visible;
            } else {
                ++hidden;
            }
        }
    }

    // next examine floating dialogs
    auto windows = DialogManager::singleton().get_all_floating_dialog_windows();
    for (auto wnd : windows) {
        if (wnd->is_visible()) {
            ++visible;
        }
        else {
            ++hidden;
        }
    }

    bool show_dialogs = true;
    // if some dialogs are hidden, toggle will first show them;
    // another option could be to hide all if some dialogs are visible
    if (hidden > 0) {
        show_dialogs = true;
    }
    else {
        // if everything's visible, hide them
        show_dialogs = false;
    }

    // set visibility of floating dialogs
    for (auto wnd : windows) {
        DialogManager::singleton().set_floating_dialog_visibility(wnd, show_dialogs);
    }

    // set visibility of docked dialogs
    _columns->toggle_multipaned_children(show_dialogs);
}

// Update dialogs
void DialogContainer::update_dialogs()
{
    for_each(dialogs.begin(), dialogs.end(), [&](auto dialog) { dialog.second->update(); });
}

void DialogContainer::set_inkscape_window(InkscapeWindow *inkscape_window)
{
    _inkscape_window = inkscape_window;
    auto desktop = _inkscape_window ? _inkscape_window->get_desktop() : nullptr;
    for_each(dialogs.begin(), dialogs.end(), [&](auto dialog) { dialog.second->setDesktop(desktop); });
}

bool DialogContainer::has_dialog_of_type(DialogBase *dialog)
{
    return (dialogs.find(dialog->get_type()) != dialogs.end());
}

DialogBase *DialogContainer::get_dialog(const Glib::ustring& dialog_type)
{
    auto found = dialogs.find(dialog_type);
    if (found != dialogs.end()) {
        return found->second;
    }
    return nullptr;
}

// Add dialog to list.
void DialogContainer::link_dialog(DialogBase *dialog)
{
    dialogs.insert(std::pair<Glib::ustring, DialogBase *>(dialog->get_type(), dialog));
    if (auto library = dynamic_cast<ArtworkLibraryController *>(dialog))
        _artwork_library_slot = library->host_slot();

    DialogWindow *window = dynamic_cast<DialogWindow *>(get_root());
    if (window) {
        window->update_dialogs();
    }
    else {
        // dialog without DialogWindow has been docked; remove it's floating state
        // so if user closes and reopens it, it shows up docked again, not floating
        DialogManager::singleton().remove_dialog_floating_state(dialog->get_type());
    }
}

// Remove dialog from list.
void DialogContainer::unlink_dialog(DialogBase *dialog)
{
    if (!dialog) {
        return;
    }

    auto range = dialogs.equal_range(dialog->get_type());
    auto found = range.first;
    for (; found != range.second; ++found) {
        if (found->second == dialog) {
            break;
        }
    }

    if (found == range.second) {
        return;
    }

    dialogs.erase(found);

    DialogWindow *window = dynamic_cast<DialogWindow *>(get_root());
    if (window) {
        window->update_dialogs();
    }
}

/**
 * Load last open window's dialog configuration state.
 *
 * For the keyfile format, check `save_container_state()`.
 */
void DialogContainer::load_container_state(Glib::KeyFile *keyfile, bool include_floating)
{
    assert(_inkscape_window);

    // Step 1: check if we want to load the state
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    // if it isn't dockable, all saved docked dialogs are made floating
    bool is_dockable =
        prefs->getInt("/options/dialogtype/value", PREFS_DIALOGS_BEHAVIOR_DOCKABLE) != PREFS_DIALOGS_BEHAVIOR_FLOATING;

    // Step 2: get the number of windows
    int windows_count = keyfile->get_integer("Windows", "Count");

    // Step 3: for each window, load its state. Only the first window is not floating (the others are DialogWindow)
    for (int window_idx = 0; window_idx < windows_count; ++window_idx) {
        if (window_idx > 0 && !include_floating)
            break;

        Glib::ustring group_name = "Window" + std::to_string(window_idx);

        // Step 3.0: read the window parameters
        int column_count = 0;
        bool floating = window_idx != 0;
        window_position_t pos;
        bool has_position = false;
        try {
            column_count = keyfile->get_integer(group_name, "ColumnCount");
            floating = keyfile->get_boolean(group_name, "Floating");
            if (keyfile->has_key(group_name, "Position") && keyfile->get_boolean(group_name, "Position")) {
                pos.x = keyfile->get_integer(group_name, "x");
                pos.y = keyfile->get_integer(group_name, "y");
                pos.width = keyfile->get_integer(group_name, "width");
                pos.height = keyfile->get_integer(group_name, "height");
                has_position = true;
            }
        } catch (Glib::Error const &error) {
            std::cerr << "DialogContainer::load_container_state: " << error.what() << std::endl;
        }

        int owned_skipped = 0;
        bool has_non_owned_entries = false;

        // Step 3.1: get the window's container columns where we want to create the dialogs
        DialogContainer *active_container = nullptr;
        DialogMultipaned *active_columns = nullptr;
        DialogWindow *dialog_window = nullptr;

        if (is_dockable) {
            if (floating) {
                dialog_window = new DialogWindow(_inkscape_window, nullptr);
                if (dialog_window) {
                    active_container = dialog_window->get_container();
                    active_columns = dialog_window->get_container()->get_columns();
                }
            } else {
                active_container = this;
                active_columns = _columns.get();
            }

            if (!active_container || !active_columns) {
                continue;
            }

            active_columns->ensure_multipaned_children();
        }

        // Step 3.2: for each column, load its state
        for (int column_idx = 0; column_idx < column_count; ++column_idx) {
            Glib::ustring column_group_name = group_name + "Column" + std::to_string(column_idx);

            // Step 3.2.0: read the column parameters
            int notebook_count = 0;
            bool before_canvas = false;
            try {
                notebook_count = keyfile->get_integer(column_group_name, "NotebookCount");
                before_canvas = keyfile->get_boolean(column_group_name, "BeforeCanvas");
            } catch (Glib::Error const &error) {
                std::cerr << "DialogContainer::load_container_state: " << error.what() << std::endl;
            }

            // Step 3.2.1: create the column
            DialogMultipaned *column = nullptr;
            if (is_dockable) {
                auto col = active_container->create_column();
                column = col.get();

                if (keyfile->has_key(column_group_name, "ColumnWidth")) {
                    auto width = keyfile->get_integer(column_group_name, "ColumnWidth");
                    col->set_restored_width(width);
                }

                before_canvas ? active_columns->prepend(std::move(col)) : active_columns->append(std::move(col));
            }

            // Step 3.2.2: for each noteboook, load its dialogs
            for (int notebook_idx = 0; notebook_idx < notebook_count; ++notebook_idx) {
                auto const key = get_key(notebook_idx);

                // Step 3.2.2.0 read the list of dialogs in the current notebook
                std::vector<Glib::ustring> dialogs;
                try {
                    dialogs = keyfile->get_string_list(column_group_name, key);
                } catch (Glib::Error const &error) {
                    std::cerr << "DialogContainer::load_container_state: " << error.what() << std::endl;
                }

                if (!dialogs.size()) {
                    continue;
                }

                // Drop an owned native Preferences entry before allocating any
                // notebook, so this restore never re-adds the retained host. Keep an
                // original-index -> surviving-index map for the saved active tab.
                auto *owner = active_container ? active_container : this;
                std::vector<int> index_map;
                index_map.reserve(dialogs.size());
                bool notebook_owned_filtered = false;
                {
                    std::vector<Glib::ustring> surviving;
                    surviving.reserve(dialogs.size());
                    for (auto const &type : dialogs) {
                        if ((type == "ExplodeBitmap" && !Bitmap::explodeBitmapEnabled()) ||
                            (type == "Spellcheck" && !UI::spellcheckUiEnabled) ||
                            owner->reuse_owned_preferences(type, false)) {
                            ++owned_skipped;
                            notebook_owned_filtered = true;
                            index_map.push_back(-1);
                        } else {
                            // Any surviving original entry (including one later dropped
                            // as a duplicate, or an unknown type) keeps native behavior.
                            has_non_owned_entries = true;
                            index_map.push_back(static_cast<int>(surviving.size()));
                            surviving.push_back(type);
                        }
                    }
                    dialogs.swap(surviving);
                }
                if (dialogs.empty()) {
                    continue;
                }

                DialogNotebook *notebook = nullptr;
                if (is_dockable) {
                    auto nb = std::make_unique<DialogNotebook>(active_container);
                    notebook = nb.get();
                    column->append(std::move(nb));
                }

                auto const &dialog_data = get_dialog_data();
                // Step 3.2.2.1 create each dialog in the current notebook
                for (auto type : dialogs) {

                    if (dialog_data.find(type) != dialog_data.end()) {
                        if (is_dockable) {
                            if (type == "ArtworkLibrary" && keyfile->has_key(column_group_name, library_slot_key(notebook_idx)))
                                active_container->_artwork_library_slot = keyfile->get_string(column_group_name, library_slot_key(notebook_idx)).raw();
                            active_container->new_dialog(type, notebook, false);
                        } else {
                            dialog_window = create_new_floating_dialog(type, false);
                        }
                    } else {
                        std::cerr << "load_container_state: invalid dialog type: " << type.raw() << std::endl;
                    }
                }

                if (notebook) {
                    Glib::ustring row = "Notebook" + std::to_string(notebook_idx) + "Height";
                    if (keyfile->has_key(column_group_name, row)) {
                        auto height = keyfile->get_integer(column_group_name, row);
                        notebook->set_requested_height(height);
                    }
                    Glib::ustring tab = "Notebook" + std::to_string(notebook_idx) + "ActiveTab";
                    if (keyfile->has_key(column_group_name, tab)) {
                        if (auto nb = notebook->get_notebook()) {
                            auto page = keyfile->get_integer(column_group_name, tab);
                            if (!notebook_owned_filtered) {
                                // Nothing was filtered out of this notebook: keep the exact
                                // native active-tab assignment.
                                nb->set_current_page(page);
                            } else {
                                int mapped = -1;
                                if (page >= 0 && page < static_cast<int>(index_map.size())) {
                                    if (index_map[page] >= 0) {
                                        mapped = index_map[page];
                                    } else {
                                        // Selected tab was the filtered owned host: choose the
                                        // nearest surviving native tab, next before previous.
                                        for (int i = page + 1; i < static_cast<int>(index_map.size()); ++i) {
                                            if (index_map[i] >= 0) { mapped = index_map[i]; break; }
                                        }
                                        if (mapped < 0) {
                                            for (int i = page - 1; i >= 0; --i) {
                                                if (index_map[i] >= 0) { mapped = index_map[i]; break; }
                                            }
                                        }
                                    }
                                }
                                nb->set_current_page(mapped >= 0 ? mapped : page);
                            }
                        }
                    }
                }
            }
            if (column) {
                if (keyfile->has_key(column_group_name, "Collapsed")) {
                    auto is_collapsed = keyfile->get_boolean(column_group_name, "Collapsed");
                    column->set_visible(!is_collapsed);
                }
            }
        }

        if (dialog_window) {
            // A docked floating wrapper that ends up empty only because its sole
            // saved panel was the filtered owned Preferences is never revealed.
            auto *win_container = dialog_window->get_container();
            if (is_dockable && owned_skipped > 0 && !has_non_owned_entries &&
                (!win_container || win_container->get_dialogs().empty())) {
                delete dialog_window;
                dialog_window = nullptr;
                continue;
            }
            if (has_position) {
                dm_restore_window_position(*dialog_window, pos);
            } else {
                dialog_window->update_window_size_to_fit_children();
            }

            dialog_window->set_visible(true);

            // Set the style and icon theme of the new menu based on the desktop
        }
    }
    INKSCAPE.themecontext->getChangeThemeSignal().emit();
    INKSCAPE.themecontext->add_gtk_css(true);
}

void save_wnd_position(Glib::KeyFile *keyfile, const Glib::ustring &group_name, const window_position_t *position)
{
    keyfile->set_boolean(group_name, "Position", position != nullptr);
    if (position) { // floating window position?
        keyfile->set_integer(group_name, "x", position->x);
        keyfile->set_integer(group_name, "y", position->y);
        keyfile->set_integer(group_name, "width", position->width);
        keyfile->set_integer(group_name, "height", position->height);
    }
}

[[nodiscard]] static auto get_notebook_dialogs(DialogNotebook &dialog_notebook)
{
    auto const notebook = dialog_notebook.get_notebook();
    g_assert(notebook);
    std::vector<Glib::ustring> dialogs;
    if (!notebook) return dialogs;

    for (auto &page : UI::notebook_pages(*notebook)) {
        if (auto const dialog = dynamic_cast<DialogBase *>(&page)) {
            dialogs.push_back(dialog->get_type());
        }
    }
    return dialogs;
}

// get *this* container's state only; store window 'position' in the state if given
std::shared_ptr<Glib::KeyFile> DialogContainer::get_container_state(const window_position_t *position) const
{
    static constexpr int window_idx = 0;

    auto const keyfile = Glib::KeyFile::create();

    // Step 2: save the number of windows
    keyfile->set_integer("Windows", "Count", 1);

    // Step 3.0: get all the multipanes of the window
    std::vector<DialogMultipaned const *> multipanes;

    for (auto const &column : _columns->get_multipaned_children()) {
        if (auto paned = dynamic_cast<DialogMultipaned *>(column.get())) {
            multipanes.push_back(paned);
        }
    }

    // Step 3.1: for each non-empty column, save its data.
    int column_count = 0; // non-empty columns count
    for (size_t column_idx = 0; column_idx < multipanes.size(); ++column_idx) {
        Glib::ustring group_name = "Window" + std::to_string(window_idx) + "Column" + std::to_string(column_idx);
        int notebook_count = 0; // non-empty notebooks count

        // Step 3.1.0: for each notebook, get its dialogs
        for (auto const &columns_widget : multipanes[column_idx]->get_multipaned_children()) {
            if (auto const dialog_notebook = dynamic_cast<DialogNotebook *>(columns_widget.get())) {
                // save the dialogs type
                auto const key = get_key(notebook_count);
                keyfile->set_string_list(group_name, key, get_notebook_dialogs(*dialog_notebook));
                save_library_slot(*keyfile, group_name, notebook_count, *dialog_notebook);

                // increase the notebook count
                notebook_count++;
            }
        }

        // Step 3.1.1: increase the column count
        if (notebook_count != 0) {
            column_count++;
        }

        // Step 3.1.2: Save the column's data
        keyfile->set_integer(group_name, "NotebookCount", notebook_count);
    }

    // Step 3.2: save the window group
    Glib::ustring group_name = "Window" + std::to_string(window_idx);
    keyfile->set_integer(group_name, "ColumnCount", column_count);
    save_wnd_position(keyfile.get(), group_name, position);

    return keyfile;
}

/**
 * Save container state. The configuration of open dialogs and the relative positions of the notebooks are saved.
 *
 * The structure of such a KeyFile is:
 *
 * There is a "Windows" group that records the number of the windows:
 * [Windows]
 * Count=1
 *
 * A "WindowX" group saves the number of columns the window's container has and whether the window is floating:
 *
 * [Window0]
 * ColumnCount=1
 * Floating=false
 *
 * For each column, we have a "WindowWColumnX" group, where X is the index of the column. "BeforeCanvas" checks
 * if the column is before the canvas or not. "NotebookCount" records how many notebooks are in each column and
 * "NotebookXDialogs" records a list of the types for the dialogs in notebook X.
 *
 * [Window0Column0]
 * Notebook0Dialogs=Text;
 * NotebookCount=2
 * BeforeCanvas=false
 *
 */
Glib::RefPtr<Glib::KeyFile> DialogContainer::save_container_state()
{
    auto keyfile = Glib::KeyFile::create();
    auto app = InkscapeApplication::instance();

    // Step 1: get all the container columns (in order, from the current container and all DialogWindow containers)
    std::vector<DialogMultipaned const *> windows       (1, _columns.get());
    std::vector<DialogWindow           *> dialog_windows(1, nullptr      );

    for (auto const &window : app->gtk_app()->get_windows()) {
        DialogWindow *dialog_window = dynamic_cast<DialogWindow *>(window);
        if (dialog_window) {
            windows.push_back(dialog_window->get_container()->get_columns());
            dialog_windows.push_back(dialog_window);
        }
    }

    // Step 2: save the number of windows
    keyfile->set_integer("Windows", "Count", windows.size());

    // Step 3: for each window, save its data. Only the first window is not floating (the others are DialogWindow)
    for (int window_idx = 0; window_idx < (int)windows.size(); ++window_idx) {
        // Step 3.0: get all the multipanes of the window
        std::vector<DialogMultipaned const *> multipanes;

        // used to check if the column is before or after canvas
        auto multipanes_it = multipanes.begin();
        bool canvas_seen = window_idx != 0; // no floating windows (window_idx > 0) have a canvas
        int before_canvas_columns_count = 0;

        for (auto const &column : windows[window_idx]->get_multipaned_children()) {
            if (!canvas_seen) {
                if (dynamic_cast<UI::Widget::CanvasGrid *>(column.get())) {
                    canvas_seen = true;
                } else {
                    if (auto paned = dynamic_cast<DialogMultipaned *>(column.get())) {
                        multipanes_it = multipanes.insert(multipanes_it, paned);
                        before_canvas_columns_count++;
                    }
                }
            } else {
                if (auto paned = dynamic_cast<DialogMultipaned *>(column.get())) {
                    multipanes.push_back(paned);
                }
            }
        }

        // Step 3.1: for each non-empty column, save its data.
        int column_count = 0; // non-empty columns count
        for (int column_idx = 0; column_idx < (int)multipanes.size(); ++column_idx) {
            Glib::ustring group_name = "Window" + std::to_string(window_idx) + "Column" + std::to_string(column_idx);
            int notebook_count = 0; // non-empty notebooks count
            int width = multipanes[column_idx]->get_allocated_width();
            auto collapsed = !multipanes[column_idx]->get_visible();

            // Step 3.1.0: for each notebook, get its dialogs' types
            for (auto const &columns_widget : multipanes[column_idx]->get_multipaned_children()) {
                if (auto const dialog_notebook = dynamic_cast<DialogNotebook *>(columns_widget.get())) {
                    // save the dialogs type
                    auto const key = get_key(notebook_count);
                    keyfile->set_string_list(group_name, key, get_notebook_dialogs(*dialog_notebook));
                    save_library_slot(*keyfile, group_name, notebook_count, *dialog_notebook);

                    // save height; useful when there are multiple "rows" of docked dialogs
                    Glib::ustring row = "Notebook" + std::to_string(notebook_count) + "Height";
                    keyfile->set_integer(group_name, row, dialog_notebook->get_allocated_height());
                    if (auto notebook = dialog_notebook->get_notebook()) {
                        Glib::ustring row = "Notebook" + std::to_string(notebook_count) + "ActiveTab";
                        keyfile->set_integer(group_name, row, notebook->get_current_page());
                    }

                    // increase the notebook count
                    notebook_count++;
                }
            }

            // Step 3.1.1: increase the column count
            if (notebook_count != 0) {
                column_count++;
            }

            keyfile->set_integer(group_name, "ColumnWidth", width);
            keyfile->set_boolean(group_name, "Collapsed", collapsed);

            // Step 3.1.2: Save the column's data
            keyfile->set_integer(group_name, "NotebookCount", notebook_count);
            keyfile->set_boolean(group_name, "BeforeCanvas", (column_idx < before_canvas_columns_count));
        }

        // Step 3.2: save the window group
        Glib::ustring group_name = "Window" + std::to_string(window_idx);
        keyfile->set_integer(group_name, "ColumnCount", column_count);
        keyfile->set_boolean(group_name, "Floating", window_idx != 0);
        if (window_idx != 0) { // floating?
            if (auto const wnd = dynamic_cast<DialogWindow *>(dialog_windows.at(window_idx))) {
                // store window position
                auto pos = dm_get_window_position(*wnd);
                save_wnd_position(keyfile.get(), group_name, pos ? &*pos : nullptr);
            }
        }
    }

    return keyfile;
}

// Signals -----------------------------------------------------

/**
 * No zombie windows. TODO: Need to work on this as it still leaves Gtk::Window! (?)
 */
void DialogContainer::on_unrealize() {
    // Disconnect all signals
    _connections.clear();

    remove(*_columns);
    _columns.reset();

    parent_type::on_unrealize();
}

// Create a new notebook and move page.
std::unique_ptr<DialogNotebook> DialogContainer::prepare_drop(Glib::ValueBase const &value) {
    if (auto source = UI::Widget::TabStrip::unpack_drop_source(value)) {
        auto tabs = source->first;
        auto pos = source->second;
        auto page = find_dialog_page(tabs, pos);
        if (!page) {
            std::cerr << "DialogContainer::prepare_drop: page not found!" << std::endl;
        }
        auto new_notebook = std::make_unique<DialogNotebook>(this);
        new_notebook->move_page(*page);
        return new_notebook;
    }
    return nullptr;
}

/**
 * If a DialogMultipaned column is empty and it can be removed, remove it
 */
void DialogContainer::column_empty(DialogMultipaned *column)
{
    DialogMultipaned *parent = dynamic_cast<DialogMultipaned *>(column->get_parent());
    if (parent) {
        parent->remove(*column);
    }

    DialogWindow *window = dynamic_cast<DialogWindow *>(get_root());
    if (window && parent) {
        auto const &children = parent->get_multipaned_children();
        // Close the DialogWindow if you're in an empty one
        if (children.size() == 3 && parent->has_empty_widget()) {
            window->close();
        }
    }
}

DialogMultipaned* DialogContainer::create_multipaned(bool left) {
    auto col = create_column();
    auto panel = col.get();
    if (left) {
        _columns->prepend(std::move(col));
    }
    else {
        _columns->append(std::move(col));
    }
    return panel;
}

DialogMultipaned* DialogContainer::get_create_multipaned(DialogMultipaned* multipaned, DockLocation location) {

    if (location == Middle || location == Start || location == End) {
        if (!multipaned) return multipaned;

        if (multipaned->get_orientation() == Gtk::Orientation::HORIZONTAL) {
            // a horizontal multipaned is a main panel spanning across the window;
            // add a new vertical one inside of it, at the start or beginning
            if (location == Middle) {
                // not a valid combination; that should be a floating dialog
                return nullptr;
            }
            return create_multipaned(location == Start);
        }
        else {
            // docking into existing vertical multipaned; it supports multiple dialog notebooks
            return multipaned;
        }
    }

    auto main = get_columns();

    // check right panel first
    if (location == TopRight || location == BottomRight) {
        auto panel = dynamic_cast<DialogMultipaned*>(_columns->get_last_widget());
        return panel ? panel : create_multipaned(false);
    }
    else if (location == TopLeft || location == BottomLeft) {
        // find left panel
        DialogMultipaned* panel = nullptr;
        auto& children = main->get_multipaned_children();
        for (auto& widget : children) {
            if (dynamic_cast<UI::Widget::CanvasGrid*>(widget.get())) {
                break;
            }
            if (auto multi = dynamic_cast<DialogMultipaned*>(widget.get())) {
                panel = multi;
            }
        }
        return panel ? panel : create_multipaned(true);
    }
    else {
        assert(false);
    }

    return nullptr;
}

DialogNotebook* DialogContainer::get_notebook(DialogMultipaned* pane, DockLocation location) {
    if (!pane) return nullptr;

    if (location == Start || location == End) {
        // create a new one
        return nullptr;
    }

    auto& children = pane->get_multipaned_children();

    // find top notebook
    DialogNotebook* top = nullptr;
    for (auto& child : children) {
        if (auto* const notebook = dynamic_cast<DialogNotebook*>(child.get())) {
            top = notebook;
            break;
        }
    }

    if (location == TopLeft || location == TopRight) {
        return top;
    }

    // find bottom notebook
    DialogNotebook* bottom = nullptr;
    for (auto& child : children | std::views::reverse) {
        if (auto* const notebook = dynamic_cast<DialogNotebook*>(child.get())) {
            bottom = notebook;
            break;
        }
    }

    if (bottom && bottom == top) {
        // there's only one notebook, so there's no bottom one yet;
        // return null, so that new notebook will be created
        bottom = nullptr;
    }

    return bottom;
}

// Takes a notebook page from existing docked dialog and docks it at the requested place,
// creating columns on the left/right or bottom as needed;
// Note: columns on left and right are DialogMultipaned widgets, whereas at the top/bottom
// we create new DialogNotebook rows.
bool DialogContainer::dock_dialog(Gtk::Widget& page, DialogNotebook& source, DockLocation location, DialogMultipaned* multipaned, DialogNotebook* notebook) {
    DialogMultipaned* panel = get_create_multipaned(multipaned, location);
    if (!panel) return false;

    _columns->ensure_multipaned_children();

    if (!notebook) {
        notebook =  get_notebook(panel, location);
    }
    if (notebook) {
        notebook->move_page(page);
        notebook->select_page(page);
    }
    else {
        // there's no notebook in requested location; create new notebook and move page
        auto new_notebook = std::make_unique<DialogNotebook>(this);
        new_notebook->move_page(page);

        if (location == TopLeft || location == TopRight) {
            // top
            panel->prepend(std::move(new_notebook));
        }
        else if (location == BottomLeft || location == BottomRight) {
            // if a new notebook is to be added at the bottom, then shrink existing one above it to make more room for it
            if (auto old = get_notebook(panel, location == BottomLeft ? TopLeft : TopRight)) {
                auto alloc = old->get_allocation();
                alloc.set_height(alloc.get_height() / 2);
                old->size_allocate(alloc, -1);
            }
            // bottom
            panel->append(std::move(new_notebook));
        }
        else if (location == Start) {
            panel->prepend(std::move(new_notebook));
        }
        else if (location == End) {
            panel->append(std::move(new_notebook));
        }
    }

    // close source panel if it is empty now
    if (source.get_notebook()->get_n_pages() == 0) {
        source.close_notebook();
    }

    return true;
}

} // namespace Inkscape::UI::Dialog

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
