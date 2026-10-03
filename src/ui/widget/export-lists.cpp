// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Anshudhar Kumar Singh <anshudhar2001@gmail.com>
 *
 * Copyright (C) 2021 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "export-lists.h"

#include <algorithm>
#include <glibmm/convert.h>
#include <glibmm/i18n.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/viewport.h>

#include "spinbutton.h"
#include "extension/db.h"
#include "extension/output.h"
#include "io/sys.h"
#include "ui/export-formats.h"
#include "ui/builder-utils.h"
#include "ui/icon-loader.h"
#include "util/units.h"

namespace Inkscape::UI::Dialog {

ExtensionList::ExtensionList()
{
    init();
}

ExtensionList::ExtensionList(BaseObjectType *cobject, const Glib::RefPtr<Gtk::Builder> &refGlade)
    : Gtk::ComboBoxText{cobject}
{
    init();
}

ExtensionList::~ExtensionList() = default;

void ExtensionList::init()
{
    _builder = create_builder("dialog-export-prefs.glade");
    _pref_button  = &get_widget<Gtk::MenuButton>(_builder, "pref_button");
    _pref_popover = &get_widget<Gtk::Popover>   (_builder, "pref_popover");
    _pref_holder  = &get_widget<Gtk::Viewport>  (_builder, "pref_holder");

    _popover_signal = _pref_popover->signal_show().connect([=, this]() {
        _pref_holder->unset_child();
        if (auto ext = getExtension()) {
            if (auto gui = ext->autogui(nullptr, nullptr)) {
                _pref_holder->set_child(*gui);
                _pref_popover->grab_focus();
            }
        }
    });

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    // A preference change (Preferences > Export formats, including Restore
    // Short List) ends a temporary "More formats..." expansion.
    _watch_pref = prefs->createObserver("/dialogs/export/show_all_extensions", [this]() { _show_all_formats = false; setup(); });
    // Each list separately: an observer on the folder misses its creation.
    _watch_formats = prefs->createObserver(UI::ExportFormats::order_pref, [this]() { _show_all_formats = false; setup(); });
    _watch_shown = prefs->createObserver(UI::ExportFormats::shown_pref, [this]() { _show_all_formats = false; setup(); });
    // limit size of the combobox
    auto cell_renderer = dynamic_cast<Gtk::CellRendererText*>(get_first_cell());
    cell_renderer->set_fixed_size(125, -1);
    cell_renderer->property_wrap_mode().set_value(Pango::WrapMode::WORD);
    cell_renderer->property_wrap_width().set_value(5);
}

void ExtensionList::on_changed()
{
    if (_populating) {
        return;
    }
    if (get_active_id() == more_formats_id) {
        // EXP-1: list every format for one pick and let the user choose.
        _show_all_formats = true;
        setup(_last_id); // one change only: straight back to the current format
        popup();
        return;
    }
    _last_id = get_active_id();
    if (!_reselecting && (_show_all_formats || (!_kept_hidden_id.empty() && _kept_hidden_id != _last_id))) {
        // The pick is made: back to the short list, which keeps the chosen
        // format listed while it is the current one (owner report 2026-09-28:
        // the short list was lost for the rest of the dialog). Leaving a kept
        // hidden format drops it from the list again.
        _show_all_formats = false;
        setup(_last_id);
        return;
    }
    bool has_prefs = false;
    if (auto ext = getExtension()) {
        has_prefs = (ext->widget_visible_count() > 0);
    }
    _pref_button->set_sensitive(has_prefs);
}

void ExtensionList::setup(std::string const &preferred)
{
    // See also create_export_filters().
    auto const previous = preferred.empty() ? std::string(get_active_id()) : preferred;
    _populating = true;
    this->remove_all();
    _listed_ids.clear();
    _kept_hidden_id.clear();
    ext_to_mod.clear();

    // EXP-1: the user's shown formats in the user's order; every other
    // available format stays reachable through "More formats...". The older
    // "Show all outputs" preference lists everything.
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool const show_all = _show_all_formats || prefs->getBool("/dialogs/export/show_all_extensions", false);
    bool hidden = false;
    auto const formats = UI::ExportFormats::formats();
    // The current format stays listed even when hidden (chosen through "More
    // formats..."), so the choice is visible and can be picked again.
    auto const is_listed = [&](UI::ExportFormats::Format const &format) {
        return show_all || format.shown || (!previous.empty() && format.id == previous);
    };
    // Filename-to-format map: listed formats first, in the user's order, then
    // hidden ones (some file endings, like .svg, have several formats).
    for (bool const listed : {true, false}) {
        for (auto const &format : formats) {
            if (is_listed(format) != listed) {
                continue;
            }
            auto omod = dynamic_cast<Inkscape::Extension::Output *>(Inkscape::Extension::db.get(format.id.c_str()));
            if (omod && !ext_to_mod[omod->get_extension()]) {
                ext_to_mod[omod->get_extension()] = omod;
            }
        }
    }
    for (auto const &format : formats) {
        auto omod = dynamic_cast<Inkscape::Extension::Output *>(Inkscape::Extension::db.get(format.id.c_str()));
        if (!omod) {
            continue;
        }
        if (is_listed(format)) {
            this->append(format.id, omod->get_filetypename());
            _listed_ids.push_back(format.id);
            if (!show_all && !format.shown) {
                _kept_hidden_id = format.id;
            }
        } else {
            hidden = true;
        }
    }
    if (hidden) {
        this->append(more_formats_id, _("More formats…"));
    }
    _populating = false;

    // The selection below is ours, not the user's pick from the list.
    auto const was_reselecting = _reselecting;
    _reselecting = true;
    if (!previous.empty() && previous != more_formats_id && selectFormat(previous)) {
        _reselecting = was_reselecting;
        return;
    }
    if (std::find(_listed_ids.begin(), _listed_ids.end(), SP_MODULE_KEY_RASTER_PNG) != _listed_ids.end()) {
        this->set_active_id(SP_MODULE_KEY_RASTER_PNG);
    } else if (!_listed_ids.empty()) {
        this->set_active_id(_listed_ids.front());
    } else {
        selectFormat(SP_MODULE_KEY_RASTER_PNG); // every format unchecked: never an empty choice
    }
    _reselecting = was_reselecting;
}

bool ExtensionList::selectFormat(std::string const &id)
{
    if (std::find(_listed_ids.begin(), _listed_ids.end(), id) == _listed_ids.end()) {
        // A hidden format the document or user already uses must still export.
        auto omod = dynamic_cast<Inkscape::Extension::Output *>(Inkscape::Extension::db.get(id.c_str()));
        if (!omod || omod->deactivated()) {
            return false;
        }
        _populating = true;
        this->insert(static_cast<int>(_listed_ids.size()), id, omod->get_filetypename());
        _listed_ids.push_back(id);
        _populating = false;
        _kept_hidden_id = id; // listed only while it is the current format
    }
    this->set_active_id(id);
    return true;
}

/**
 * Returns the Output extension currently selected in this dropdown.
 */
Inkscape::Extension::Output *ExtensionList::getExtension()
{
    return dynamic_cast<Inkscape::Extension::Output *>(Inkscape::Extension::db.get(this->get_active_id().c_str()));
}

/**
 * Returns the file extension (file ending) of the currently selected extension.
 */
std::string ExtensionList::getFileExtension()
{
    if (auto ext = getExtension()) {
        return Glib::filename_from_utf8(ext->get_extension());
    }
    return "";
}

/**
 * Removes the file extension, *if* it's one of the extensions in the list.
 */
void ExtensionList::removeExtension(std::string &filename)
{
    auto ext = Inkscape::IO::get_file_extension(filename);
    if (ext_to_mod[ext]) {
        filename.erase(filename.size()-ext.size());
    }
}

void ExtensionList::setExtensionFromFilename(std::string const &filename)
{
    auto ext = Inkscape::IO::get_file_extension(filename);
    if (ext != getFileExtension()) {
        if (auto omod = ext_to_mod[ext]) {
            selectFormat(omod->get_id());
        }
    }
}

void ExportList::setup()
{
    if (_initialised) {
        return;
    }
    _initialised = true;
    prefs = Inkscape::Preferences::get();
    default_dpi = prefs->getDouble("/dialogs/export/defaultxdpi/value", DPI_BASE);

    auto const add_button = Gtk::make_managed<Gtk::Button>();
    Glib::ustring label = _("Add Export");
    add_button->set_label(label);
    this->attach(*add_button, 0, 0, 5, 1);

    this->insert_row(0);

    auto const suffix_label = Gtk::make_managed<Gtk::Label>(_("Suffix"));
    this->attach(*suffix_label, _suffix_col, 0, 1, 1);
    suffix_label->set_visible(true);

    auto const extension_label = Gtk::make_managed<Gtk::Label>(_("Format"));
    this->attach(*extension_label, _extension_col, 0, 2, 1);
    extension_label->set_visible(true);

    auto const dpi_label = Gtk::make_managed<Gtk::Label>(_("DPI"));
    this->attach(*dpi_label, _dpi_col, 0, 1, 1);
    dpi_label->set_visible(true);

    append_row();

    add_button->signal_clicked().connect(sigc::mem_fun(*this, &ExportList::append_row));
    add_button->set_hexpand(true);
    add_button->set_visible(true);
}

void ExportList::removeExtension(std::string &filename)
{
    ExtensionList *extension_cb = dynamic_cast<ExtensionList *>(this->get_child_at(_extension_col, 1));
    if (extension_cb) {
        extension_cb->removeExtension(filename);
        return;
    }
}

void ExportList::append_row()
{
    int current_row = _num_rows + 1; // because we have label row at top
    this->insert_row(current_row);

    auto const suffix = Gtk::make_managed<Gtk::Entry>();
    this->attach(*suffix, _suffix_col, current_row, 1, 1);
    suffix->set_width_chars(2);
    suffix->set_hexpand(true);
    suffix->set_placeholder_text(_("Suffix"));
    suffix->set_visible(true);

    auto const extension = Gtk::make_managed<ExtensionList>();
    auto const dpi_sb = Gtk::make_managed<UI::Widget::SpinButton>();

    extension->setup();
    extension->set_visible(true);
    this->attach(*extension, _extension_col, current_row, 1, 1);
    this->attach(*extension->getPrefButton(), _prefs_col, current_row, 1, 1);

    // Disable DPI when not using a raster image output
    extension->signal_changed().connect([=]() {
        if (auto ext = extension->getExtension()) {
            dpi_sb->set_sensitive(ext->is_raster());
        }
    });

    dpi_sb->set_digits(2);
    dpi_sb->set_increments(0.1, 1.0);
    dpi_sb->set_range(1.0, 100000.0);
    dpi_sb->set_value(default_dpi);
    dpi_sb->set_sensitive(true);
    dpi_sb->set_width_chars(6);
    this->attach(*dpi_sb, _dpi_col, current_row, 1, 1);

    auto const pIcon = Gtk::manage(sp_get_icon_image("window-close", Gtk::IconSize::NORMAL));
    auto const delete_btn = Gtk::make_managed<Gtk::Button>();
    delete_btn->set_has_frame(false);
    delete_btn->set_child(*pIcon);
    this->attach(*delete_btn, _delete_col, current_row, 1, 1);
    delete_btn->signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &ExportList::delete_row), delete_btn));

    _num_rows++;
}

void ExportList::delete_row(Gtk::Widget *widget)
{
    if (widget == nullptr) {
        return;
    }
    if (_num_rows <= 1) {
        return;
    }
    int row, ignore;
    query_child(*widget, ignore, row, ignore, ignore);
    remove_row(row);
    _num_rows--;
    if (_num_rows <= 1) {
        auto const d_button_0 = this->get_child_at(_delete_col, 1);
        if (d_button_0) {
            d_button_0->set_visible(false);
        }
    }
}

std::string ExportList::get_suffix(int row)
{
    std::string suffix = "";
    Gtk::Entry *entry = dynamic_cast<Gtk::Entry *>(this->get_child_at(_suffix_col, row + 1));
    if (entry == nullptr) {
        return suffix;
    }
    suffix = Glib::filename_from_utf8(entry->get_text());
    return suffix;
}
Inkscape::Extension::Output *ExportList::getExtension(int row)
{
    ExtensionList *extension_cb = dynamic_cast<ExtensionList *>(this->get_child_at(_extension_col, row + 1));
    return extension_cb->getExtension();
}

double ExportList::get_dpi(int row)
{
    double dpi = default_dpi;
    auto spin_sb = dynamic_cast<UI::Widget::InkSpinButton *>(this->get_child_at(_dpi_col, row + 1));
    if (spin_sb == nullptr) {
        return dpi;
    }
    dpi = spin_sb->get_value();
    return dpi;
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
