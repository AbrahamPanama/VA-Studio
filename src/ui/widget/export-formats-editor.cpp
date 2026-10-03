// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EXP-1: Preferences editor for the formats the Export dialog offers.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "export-formats-editor.h"

#include <glibmm/i18n.h>
#include <glibmm/markup.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>

namespace Inkscape::UI::Widget {

ExportFormatsEditor::ExportFormatsEditor()
    : Gtk::Box(Gtk::Orientation::VERTICAL, 6)
    , _up(_("Move _Up"), true)
    , _down(_("Move _Down"), true)
    , _reset(_("_Restore Short List"), true)
{
    auto const note = Gtk::make_managed<Gtk::Label>(
        _("Checked formats appear in the Export dialog, in this order. The others stay available under "
          "\"More formats…\". Save As is not affected."));
    note->set_wrap(true);
    note->set_xalign(0);
    append(*note);

    _list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    _list.set_header_func([this](Gtk::ListBoxRow *row, Gtk::ListBoxRow *before) {
        auto const index = row->get_index();
        if (index < 0 || static_cast<std::size_t>(index) >= _formats.size()) {
            return;
        }
        auto const group = _formats[index].group;
        bool const first = !before || before->get_index() < 0 ||
                           _formats[before->get_index()].group != group;
        if (!first) {
            row->unset_header();
            return;
        }
        auto const header = Gtk::make_managed<Gtk::Label>();
        header->set_markup("<b>" + Glib::Markup::escape_text(ExportFormats::group_label(group)) + "</b>");
        header->set_xalign(0);
        header->set_margin_top(6);
        row->set_header(*header);
    });
    _list.signal_selected_rows_changed().connect([this] { updateButtons(); });
    auto const scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_child(_list);
    scroll->set_min_content_height(320);
    scroll->set_vexpand(true);
    append(*scroll);

    auto const buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    buttons->append(_up);
    buttons->append(_down);
    buttons->append(_reset);
    append(*buttons);
    _up.signal_clicked().connect([this] {
        auto const index = selectedIndex();
        if (index >= 0) {
            move(index, -1);
        }
    });
    _down.signal_clicked().connect([this] {
        auto const index = selectedIndex();
        if (index >= 0) {
            move(index, +1);
        }
    });
    _reset.signal_clicked().connect([this] { restoreShortList(); });

    _formats = ExportFormats::formats();
    rebuild();
}

void ExportFormatsEditor::rebuild(int select)
{
    while (auto *row = _list.get_row_at_index(0)) {
        _list.remove(*row);
    }
    for (std::size_t index = 0; index < _formats.size(); ++index) {
        auto const &format = _formats[index];
        auto const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto const check = Gtk::make_managed<Gtk::CheckButton>();
        check->set_active(format.shown);
        check->set_tooltip_text(_("Show this format in the Export dialog"));
        check->signal_toggled().connect([this, index, check] { setShown(index, check->get_active()); });
        auto const name = Gtk::make_managed<Gtk::Label>(format.name);
        name->set_xalign(0);
        name->set_hexpand(true);
        auto const extension = Gtk::make_managed<Gtk::Label>(format.extension);
        extension->add_css_class("dim-label");
        box->append(*check);
        box->append(*name);
        box->append(*extension);
        _list.append(*box);
    }
    if (select >= 0) {
        if (auto *row = _list.get_row_at_index(select)) {
            _list.select_row(*row);
        }
    }
    updateButtons();
}

void ExportFormatsEditor::setShown(std::size_t index, bool shown)
{
    if (index >= _formats.size() || _formats[index].shown == shown) {
        return;
    }
    _formats[index].shown = shown;
    store();
}

bool ExportFormatsEditor::move(std::size_t index, int direction)
{
    auto const target = static_cast<long>(index) + direction;
    if (index >= _formats.size() || target < 0 || static_cast<std::size_t>(target) >= _formats.size() ||
        _formats[target].group != _formats[index].group) {
        return false;
    }
    std::swap(_formats[index], _formats[target]);
    store();
    rebuild(static_cast<int>(target));
    return true;
}

void ExportFormatsEditor::restoreShortList()
{
    // Stored explicitly (the defaults themselves, not the formats available
    // right now): on a profile that never changed the lists, removing them
    // notifies nothing, and an open Export dialog kept its "More formats..."
    // expansion (owner report 2026-09-28).
    ExportFormats::restore_short_list();
    _formats = ExportFormats::formats();
    rebuild();
}

void ExportFormatsEditor::store()
{
    ExportFormats::save(_formats);
}

int ExportFormatsEditor::selectedIndex() const
{
    auto const *row = _list.get_selected_row();
    return row ? row->get_index() : -1;
}

void ExportFormatsEditor::updateButtons()
{
    auto const index = selectedIndex();
    auto const same_group = [this, index](int target) {
        return index >= 0 && target >= 0 && static_cast<std::size_t>(target) < _formats.size() &&
               _formats[target].group == _formats[index].group;
    };
    _up.set_sensitive(same_group(index - 1));
    _down.set_sensitive(same_group(index + 1));
}

} // namespace Inkscape::UI::Widget
