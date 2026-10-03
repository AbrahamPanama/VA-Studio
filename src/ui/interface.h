// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SP_INTERFACE_H
#define SEEN_SP_INTERFACE_H

/*
 * Main UI stuff
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Frank Felfe <innerspace@iname.com>
 *   Abhishek Sharma
 *   Kris De Gussem <Kris.DeGussem@gmail.com>
 *
 * Copyright (C) 2012 Kris De Gussem
 * Copyright (C) 1999-2002 authors
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <glibmm/ustring.h>

class SPDesktop;
namespace Gtk { class Window; }

void sp_ui_error_dialog(char const *message);

/**
 * If necessary, ask the user if a file may be overwritten.
 *
 * @arg filename path to file.
 * Value is in platform-native encoding (see Glib::filename_to_utf8).
 * @returns true if it is okay to write to the file.
 * This means that the file does not exist yet or the user confirmed that overwriting is okay.
 */
bool sp_ui_overwrite_file(std::string const &filename);

/**
 * Explicit-parent variant of sp_ui_overwrite_file().
 *
 * The confirmation dialog is transient for @a parent and only succeeds while
 * that native window remains present in Gtk::Window::get_toplevels(). This
 * overload never consults the active desktop or active window globals.
 *
 * @arg filename path to file, in platform-native encoding.
 * @arg parent explicit parent window, or null.
 * @returns true if the target does not exist, or if @a parent stayed alive and
 * the user confirmed replacement. An existing target is never overwritten when
 * @a parent is null or is lost while the dialog is pending.
 */
bool sp_ui_overwrite_file(std::string const &filename, Gtk::Window *parent);

Glib::ustring getLayoutPrefPath(SPDesktop *desktop);

#endif // SEEN_SP_INTERFACE_H

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
