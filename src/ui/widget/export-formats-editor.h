// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EXP-1: Preferences editor for the formats the Export dialog offers.
 *
 * One list with a check box per format, grouped under headings (Raster,
 * Vector, Text, Animation, Other); Move Up / Move Down reorder a format inside
 * its group; Restore Short List stores the default short list. Every change is
 * stored immediately (per user) and open Export dialogs follow it.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_WIDGET_EXPORT_FORMATS_EDITOR_H
#define INKSCAPE_UI_WIDGET_EXPORT_FORMATS_EDITOR_H

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/listbox.h>
#include <vector>

#include "ui/export-formats.h"

namespace Inkscape::UI::Widget {

class ExportFormatsEditor : public Gtk::Box
{
public:
    ExportFormatsEditor();

    /// Current model (for tests): the list as shown, in order.
    std::vector<ExportFormats::Format> const &formats() const { return _formats; }
    void setShown(std::size_t index, bool shown);
    /// Move the format at \a index one place up (-1) or down (+1) inside its group.
    bool move(std::size_t index, int direction);
    void restoreShortList();

private:
    void rebuild(int select = -1);
    void store();
    void updateButtons();
    int selectedIndex() const;

    std::vector<ExportFormats::Format> _formats;
    Gtk::ListBox _list;
    Gtk::Button _up;
    Gtk::Button _down;
    Gtk::Button _reset;
};

} // namespace Inkscape::UI::Widget

#endif // INKSCAPE_UI_WIDGET_EXPORT_FORMATS_EDITOR_H
