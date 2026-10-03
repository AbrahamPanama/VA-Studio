// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Main UI stuff.
 */
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Frank Felfe <innerspace@iname.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *   Kris De Gussem <Kris.DeGussem@gmail.com>
 *
 * Copyright (C) 2012 Kris De Gussem
 * Copyright (C) 2010 authors
 * Copyright (C) 1999-2005 authors
 * Copyright (C) 2004 David Turner
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "interface.h"

#include <giomm/listmodel.h>     // for ListModel
#include <glibmm/convert.h>      // for filename_to_utf8
#include <glibmm/i18n.h>         // for _
#include <glibmm/miscutils.h>    // for path_get_basename, path_get_dirname
#include <gtkmm/messagedialog.h> // for MessageDialog, ButtonsType
#include <gtkmm/window.h>        // for Window, get_toplevels
#include <sigc++/scoped_connection.h>

#include "desktop.h" // for SPDesktop
#include "inkscape-window.h"
#include "io/sys.h"                // for file_test, sanitizeString
#include "inkscape.h" // for Application, SP_ACTIVE_DOCUMENT
#include "ui/dialog-events.h" // for sp_transientize
#include "ui/dialog-run.h"    // for dialog_run
#include "util/scope_exit.h"  // for scope_exit

class SPDocument;

Glib::ustring getLayoutPrefPath(SPDesktop *desktop)
{
    if (desktop->is_focusMode()) {
        return "/focus/";
    } else if (desktop->is_fullscreen()) {
        return "/fullscreen/";
    } else {
        return "/window/";
    }
}

void sp_ui_error_dialog(char const *message)
{
    auto const safeMsg = Inkscape::IO::sanitizeString(message);

    auto dlg = Gtk::MessageDialog(safeMsg, true, Gtk::MessageType::ERROR, Gtk::ButtonsType::CLOSE);
    sp_transientize(dlg);

    Inkscape::UI::dialog_run(dlg);
}

/**
 * If necessary, ask the user if a file may be overwritten.
 * 
 * @arg filename path to file.
 * Value is in platform-native encoding (see Glib::filename_to_utf8).
 * @returns true if it is okay to write to the file.
 * This means that the file does not exist yet or the user confirmed that overwriting is okay.
 */
bool sp_ui_overwrite_file(std::string const &filename)
{
    auto desktop = Inkscape::Application::exists() ? SP_ACTIVE_DESKTOP : nullptr;

    // Null-safe: an absent window is forwarded as a null parent, which refuses
    // to overwrite an existing target rather than consulting active globals.
    return sp_ui_overwrite_file(filename, desktop ? desktop->getInkscapeWindow() : nullptr);
}

bool sp_ui_overwrite_file(std::string const &filename, Gtk::Window *parent)
{
    // An absent target never needs confirmation and needs no live window.
    if (!g_file_test(filename.c_str(), G_FILE_TEST_EXISTS)) {
        return true;
    }

    // An existing target is only ever confirmed against an explicit parent.
    // A missing parent refuses; it must never silently consent to overwriting.
    if (!parent) {
        return false;
    }

    auto const basename = Glib::filename_to_utf8(Glib::path_get_basename(filename));
    auto const dirname = Glib::filename_to_utf8(Glib::path_get_dirname(filename));
    auto const msg = Glib::ustring::compose(_("<span weight=\"bold\" size=\"larger\">A file named \"%1\" already exists. Do you want to replace it?</span>\n\n"
                                              "The file already exists in \"%2\". Replacing it will overwrite its contents."),
                                            basename, dirname);

    // The toplevel model is the only owned UI reference; @a identity is a
    // non-owning native pointer compared by value and never dereferenced.
    // GTK4 removes a window from this model at the native destruction boundary,
    // so model membership and signal_destroy together establish liveness.
    auto toplevels = Gtk::Window::get_toplevels();
    GtkWindow *const identity = parent->gobj();
    bool parent_alive = true;

    Gtk::MessageDialog *active_dialog = nullptr;

    auto membership = [&toplevels, identity]() -> bool {
        if (!toplevels) {
            return false;
        }
        auto const n_items = g_list_model_get_n_items(toplevels->gobj());
        for (guint i = 0; i < n_items; ++i) {
            gpointer item = g_list_model_get_item(toplevels->gobj(), i);
            bool const match = item == static_cast<gpointer>(identity);
            if (item) {
                g_object_unref(item);
            }
            if (match) {
                return true;
            }
        }
        return false;
    };

    // Latch loss first, then close a dialog that already exists. Callbacks
    // fired while the dialog is being constructed see a null pointer, and
    // callbacks fired while it is being destroyed see it cleared below.
    auto cancel = [&parent_alive, &active_dialog]() {
        parent_alive = false;
        if (active_dialog) {
            active_dialog->response(int(Gtk::ResponseType::NO));
        }
    };

    sigc::scoped_connection destroy_connection = parent->signal_destroy().connect(cancel);
    sigc::scoped_connection model_connection = toplevels->signal_items_changed().connect(
        [&membership, &cancel](guint, guint, guint) {
            // Unrelated additions, removals and hide events leave identity
            // present, so only actual absence of the parent cancels.
            if (!membership()) {
                cancel();
            }
        });

    // Require liveness before any dialog exists. Watches are already installed,
    // so a loss from here on is observed.
    if (!membership()) {
        return false;
    }

    // The modal response is recorded outside the dialog's own lifetime. The
    // dialog, the clear-active guard and the modal call live in an inner scope
    // so the decision below is evaluated only after the MessageDialog has been
    // destroyed. A native model removal listener fired during that destruction
    // can still revoke the parent before the final result is decided.
    int response = static_cast<int>(Gtk::ResponseType::NONE);
    {
        Gtk::MessageDialog dlg(msg, true, Gtk::MessageType::QUESTION, Gtk::ButtonsType::NONE);
        auto clear_active_dialog = scope_exit([&active_dialog] { active_dialog = nullptr; });
        active_dialog = &dlg;

        // A parent lost during construction is refused without touching it.
        if (!parent_alive || !membership()) {
            return false;
        }

        dlg.set_transient_for(*parent);
        dlg.add_button(_("_Cancel"), Gtk::ResponseType::NO);
        dlg.add_button(_("Replace"), Gtk::ResponseType::YES);
        dlg.set_default_response(Gtk::ResponseType::YES);

        // Recheck immediately before the modal loop.
        if (!parent_alive || !membership()) {
            return false;
        }

        response = Inkscape::UI::dialog_run(dlg);
    }

    // After the dialog has been destroyed the parent is never dereferenced:
    // liveness and model membership decide, so a late YES after cancellation,
    // or a parent removed while the dialog tears down, still refuses.
    return parent_alive && membership() && response == static_cast<int>(Gtk::ResponseType::YES);
}

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
