// SPDX-License-Identifier: GPL-2.0-or-later
#include "bitmap-copy-dialog.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <glib.h>
#include <glibmm/i18n.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dialog.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include "ui/dialog-run.h"

namespace Inkscape::UI::Dialog {
BitmapCopySize bitmap_copy_size(Geom::Rect const &bounds_px, int dpi)
{
    dpi = std::clamp(dpi, 1, BitmapCopyOptions::max_dpi);
    Geom::Rect area = bounds_px;
    if (dpi == 96) area = area.roundOutwards();
    double const scale = dpi / 96.0;
    int const width = static_cast<int>(std::ceil(scale * area.width()));
    int const height = static_cast<int>(std::ceil(scale * area.height()));
    return {width, height, std::uint64_t(width) * std::uint64_t(height) * 4};
}
Glib::ustring bitmap_copy_size_text(BitmapCopySize const &size)
{
    auto *sz = g_format_size(size.bytes);
    auto result = Glib::ustring::compose("%1 × %2 px · %3", size.width, size.height, sz);
    g_free(sz);
    return result;
}
namespace {
struct Presentation {
    Glib::ustring title;
    Glib::ustring message;          ///< shown above the options when not empty
    bool show_keep_original = true;
    Glib::ustring cancel_label;
    Glib::ustring ok_label;
};

class BitmapCopyDialog : public Gtk::Dialog {
public:
    BitmapCopyDialog(Gtk::Window &parent, Geom::Rect const &bounds_px, BitmapCopyOptions const &initial,
                     Presentation const &presentation)
        : Dialog(presentation.title, parent, true), _bounds_px(bounds_px), _show_keep(presentation.show_keep_original)
    {
        if (!presentation.message.empty()) {
            _message.set_text(presentation.message);
            _message.set_wrap(true);
            _message.set_max_width_chars(48);
            _message.set_xalign(0);
            _message.set_margin(16);
            _message.set_margin_bottom(0);
            get_content_area()->append(_message);
        }
        _grid.set_row_spacing(8);
        _grid.set_column_spacing(8);
        _grid.set_margin(16);
        _resolution.set_halign(Gtk::Align::END);
        _grid.attach(_resolution, 0, 0);
        _dpi.set_adjustment(Gtk::Adjustment::create(initial.dpi, 1, BitmapCopyOptions::max_dpi, 1, 10));
        _dpi.set_digits(0);
        _grid.attach(_dpi, 1, 0);
        auto presets = Gtk::StringList::create();
        for (int value : preset_dpi) presets->append(std::to_string(value));
        _presets.set_model(presets);
        auto match = std::find(preset_dpi.begin(), preset_dpi.end(), initial.dpi);
        _presets.set_selected(match == preset_dpi.end() ? GTK_INVALID_LIST_POSITION
                                                         : static_cast<guint>(match - preset_dpi.begin()));
        _presets.property_selected().signal_changed().connect([this] {
            if (_syncing) return;
            auto index = _presets.get_selected();
            _syncing = true;
            if (index < preset_dpi.size()) _dpi.set_value(preset_dpi[index]);
            _syncing = false;
        });
        _grid.attach(_presets, 2, 0);
        _grid.attach(_dpi_unit, 3, 0);
        _antialias.set_active(initial.antialias);
        _transparent.set_active(initial.transparent);
        _keep_original.set_active(initial.keep_original);
        _keep_original.set_tooltip_text(_("Keep the selected objects and add the bitmap above them; unchecked, the bitmap replaces them"));
        _grid.attach(_antialias, 0, 1, 4, 1);
        _grid.attach(_transparent, 0, 2, 4, 1);
        if (_show_keep) {
            _grid.attach(_keep_original, 0, 3, 4, 1);
        }
        _size.set_halign(Gtk::Align::START);
        _grid.attach(_size, 0, 4, 4, 1);
        _dpi.signal_value_changed().connect([this] {
            _size.set_text(Glib::ustring(_("Size: ")) + bitmap_copy_size_text(bitmap_copy_size(_bounds_px, static_cast<int>(_dpi.get_value()))));
            if (!_syncing) {
                _syncing = true;
                auto match = std::find(preset_dpi.begin(), preset_dpi.end(), static_cast<int>(_dpi.get_value()));
                _presets.set_selected(match == preset_dpi.end() ? GTK_INVALID_LIST_POSITION
                                                                 : static_cast<guint>(match - preset_dpi.begin()));
                _syncing = false;
            }
        });
        _size.set_text(Glib::ustring(_("Size: ")) + bitmap_copy_size_text(bitmap_copy_size(_bounds_px, static_cast<int>(_dpi.get_value()))));
        get_content_area()->append(_grid);
        add_button(presentation.cancel_label, Gtk::ResponseType::CANCEL);
        add_button(presentation.ok_label, Gtk::ResponseType::OK);
        set_default_response(Gtk::ResponseType::OK);
    }
    BitmapCopyOptions options() const
    {
        const_cast<Gtk::SpinButton &>(_dpi).update();
        return {static_cast<int>(_dpi.get_value()), _antialias.get_active(),
                _transparent.get_active(), _show_keep && _keep_original.get_active()};
    }
private:
    static constexpr std::array<int, 6> preset_dpi{72, 96, 150, 200, 300, 600};
    bool _syncing = false;
    Geom::Rect _bounds_px;
    bool _show_keep = true;
    Gtk::Label _message;
    Gtk::Grid _grid;
    Gtk::Label _resolution{_("Resolution:")};
    Gtk::SpinButton _dpi;
    Gtk::DropDown _presets;
    Gtk::Label _dpi_unit{_("dpi")};
    Gtk::CheckButton _antialias{_("Anti-aliasing")};
    Gtk::CheckButton _transparent{_("Transparent background")};
    Gtk::CheckButton _keep_original{_("Keep original")};
    Gtk::Label _size;
};
} // namespace
std::optional<BitmapCopyOptions> run_bitmap_copy_dialog(Gtk::Window &parent, Geom::Rect const &bounds_px,
                                                        BitmapCopyOptions const &initial)
{
    BitmapCopyDialog dialog(parent, bounds_px, initial,
                            {_("Make a Bitmap Copy"), {}, true, _("_Cancel"), _("_OK")});
    int const response = UI::dialog_run(dialog);
    return response == Gtk::ResponseType::OK ? std::optional(dialog.options()) : std::nullopt;
}

std::optional<BitmapCopyOptions> run_clipped_bitmaps_dialog(Gtk::Window &parent, int count, int with_vectors,
                                                            int in_effect_groups, Geom::Rect const &largest_px,
                                                            BitmapCopyOptions const &initial, bool on_open)
{
    Glib::ustring message = on_open
        ? Glib::ustring::compose(
              ngettext("This file has %1 bitmap with a clip mask. Clip masks from CorelDRAW are slow to work with; "
                       "converting it makes it a plain image with the options below. This cannot be undone.",
                       "This file has %1 bitmaps with clip masks. Clip masks from CorelDRAW are slow to work with; "
                       "converting them makes each one a separate plain image with the options below. This cannot "
                       "be undone.",
                       count),
              count)
        : Glib::ustring::compose(
              ngettext("This document has %1 bitmap with a clip mask. Converting it makes it a plain image with "
                       "the options below.",
                       "This document has %1 bitmaps with clip masks. Converting them makes each one a separate "
                       "plain image with the options below.",
                       count),
              count);
    if (with_vectors > 0) {
        message += "\n\n" + Glib::ustring::compose(
                                 ngettext("%1 clip mask that also holds vectors or text is left unchanged.",
                                          "%1 clip masks that also hold vectors or text are left unchanged.",
                                          with_vectors),
                                 with_vectors);
    }
    if (in_effect_groups > 0) {
        message += "\n\n" + Glib::ustring::compose(
                                 ngettext("%1 clipped bitmap inside a group with transparency or effects is left "
                                          "unchanged.",
                                          "%1 clipped bitmaps inside groups with transparency or effects are left "
                                          "unchanged.",
                                          in_effect_groups),
                                 in_effect_groups);
    }
    auto options = initial;
    options.keep_original = false;
    BitmapCopyDialog dialog(parent, largest_px, options,
                            {_("Convert Clipped Bitmaps"), message, false,
                             on_open ? _("_Keep as They Are") : _("_Cancel"), _("C_onvert")});
    int const response = UI::dialog_run(dialog);
    return response == Gtk::ResponseType::OK ? std::optional(dialog.options()) : std::nullopt;
}
} // namespace Inkscape::UI::Dialog
