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

#ifndef INKSCAPE_UI_DIALOG_CONTAINER_H
#define INKSCAPE_UI_DIALOG_CONTAINER_H

#include <gtkmm/box.h>

#include "dialog-manager.h"
#include "desktop.h"

namespace Glib {
class ValueBase;
} // namespace Glib

namespace Inkscape::UI::Dialog {

class DialogBase;
class DialogNotebook;
class DialogMultipaned;
class DialogWindow;
class ArtworkLibraryPanelReservation;

/**
 * A widget that manages DialogNotebook's and other widgets inside a
 * horizontal DialogMultipaned containing vertical DialogMultipaned's or other widgets.
 */
class DialogContainer : public Gtk::Box
{
    using parent_type = Gtk::Box;

public:
    DialogContainer(InkscapeWindow* inkscape_window);
    ~DialogContainer() override;

    // Columns-related functions
    DialogMultipaned *get_columns() { return _columns.get(); }
    std::unique_ptr<DialogMultipaned> create_column();

    // Dialog-related functions
    void new_dialog(const Glib::ustring& dialog_type);
    void new_dialog(const Glib::ustring& dialog_type, DialogNotebook* notebook, bool ensure_visibility);
    DialogWindow* new_floating_dialog(const Glib::ustring& dialog_type);
    bool has_dialog_of_type(DialogBase *dialog);
    DialogBase *get_dialog(const Glib::ustring& dialog_type);
    void link_dialog(DialogBase *dialog);
    void unlink_dialog(DialogBase *dialog);
    std::multimap<Glib::ustring, DialogBase *> const &get_dialogs() const { return dialogs; }
    void toggle_dialogs();
    void update_dialogs(); // Update all linked dialogs
    void set_inkscape_window(InkscapeWindow *inkscape_window);
    InkscapeWindow *get_inkscape_window() { return _inkscape_window; }
    enum DockLocation {
        TopLeft,    // at global level, top-left
        BottomLeft, // at global level, bottom-left
        TopRight,   // at global level, top-right
        BottomRight,// at global level, bottom-right
        Start,      // dock dialog at the top (or left) of existing multipaned
        End,        // dock dialog at the bottom (or right) of existing multipaned
        Middle      // dock in the existing notebook in the middle of existing multipaned
    };
    bool dock_dialog(Gtk::Widget& page, DialogNotebook& source, DockLocation location, DialogMultipaned* multipaned, DialogNotebook* notebook);

    // State saving functionality
    Glib::RefPtr<Glib::KeyFile> save_container_state();
    void load_container_state(Glib::KeyFile* keyfile, bool include_floating);

    void restore_window_position(DialogWindow* window);
    void store_window_position(DialogWindow* window);

    // get this container's state; provide window position for container embedded in DialogWindow
    std::shared_ptr<Glib::KeyFile> get_container_state(const window_position_t* position) const;
    void load_container_state(Glib::KeyFile& state, const std::string& window_id);

private:
    InkscapeWindow *_inkscape_window = nullptr; // Every container is attached to an InkscapeWindow.
    std::unique_ptr<DialogMultipaned> _columns ; // The main widget inside which other children are kept.
    std::vector<GType> _drop_gtypes     ; // What kind of object can be dropped.

    /**
     * Due to the way Gtk handles dragging between notebooks, one can
     * either allow multiple instances of the same dialog in a notebook
     * or restrict dialogs to docks tied to a particular document
     * window. (More explicitly, use one group name for all notebooks or
     * use a unique group name for each document window with related
     * floating docks.) For the moment we choose the former which
     * requires a multimap here as we use the dialog type as a key.
     */
    std::multimap<Glib::ustring, DialogBase *> dialogs;
    // Session identity follows a library when its tab is moved. Unlike Symbols,
    // unrelated windows must not be redirected to any panel of the same type.
    std::string _artwork_library_slot;
    std::shared_ptr<bool> _opening_artwork_library = std::make_shared<bool>(false);

    void setup_drag_and_drop(DialogMultipaned* column);
    std::unique_ptr<DialogBase> dialog_factory(Glib::ustring const &dialog_type,
                                             std::unique_ptr<ArtworkLibraryPanelReservation> &library_reservation);
    DialogWindow *create_new_floating_dialog(Glib::ustring const &dialog_type, bool blink);
    // Reuse the receiving application's retained native Preferences host instead
    // of constructing a duplicate. Returns true when the request was handled.
    bool reuse_owned_preferences(Glib::ustring const &dialog_type, bool ensure_visibility);
    // get existing or create if needed requested panel
    DialogMultipaned* get_create_multipaned(DialogMultipaned* multipaned, DockLocation location);
    DialogMultipaned* create_multipaned(bool left);
    DialogNotebook* get_notebook(DialogMultipaned* pane, DockLocation location);

    // Signal connections
    std::vector<sigc::scoped_connection> _connections;

    // Handlers
    void on_unrealize() override;
    std::unique_ptr<DialogNotebook> prepare_drop(Glib::ValueBase const &value);
    void column_empty(DialogMultipaned *column);
    DialogBase* find_existing_dialog(const Glib::ustring& dialog_type);
    enum class RestoreResult { NotApplicable, Restored, Aborted };
    static RestoreResult recreate_dialogs_from_state(InkscapeWindow* inkscape_window, const Glib::KeyFile* keyfile,
                                           std::string *artwork_library_slot = nullptr);
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_CONTAINER_H

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
