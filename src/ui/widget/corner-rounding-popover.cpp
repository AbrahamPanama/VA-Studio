// SPDX-License-Identifier: GPL-2.0-or-later
#include "corner-rounding-popover.h"
#include <algorithm>
#include <cmath>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <gtkmm/eventcontrollerkey.h>
#include <glib.h>

namespace Inkscape::UI::Widget {
namespace CE = LivePathEffect::CornerEdit;
CornerRoundingPopover::CornerRoundingPopover()
{
    set_autohide(false); // Canvas node selection remains available.
    _box.set_margin(12); set_child(_box);
    _title.set_text(_("Corners")); _title.add_css_class("heading");
    _title.set_hexpand(true); _title.set_xalign(0);
    _close.set_icon_name("window-close-symbolic");
    _close.set_has_frame(false); _close.set_tooltip_text(_("Close corner controls"));
    _header.append(_title); _header.append(_close); _box.append(_header);
    _close.signal_clicked().connect([this] { _pending.disconnect(); popdown(); exit_requested.emit(); });
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect([this](guint key, guint, Gdk::ModifierType) {
        if (key != GDK_KEY_Escape) return false;
        _pending.disconnect(); popdown(); exit_requested.emit(); return true;
    }, false);
    add_controller(keys);
    signal_hide().connect([this] { _pending.disconnect(); });

    _round.set_label(_("_Round")); _round.set_use_underline(true);
    _inverse.set_label(_("_Inverse")); _inverse.set_use_underline(true);
    _inverse.set_group(_round); _round.set_active(true);
    _round.set_tooltip_text(_("Conventional tangent rounding."));
    _inverse.set_tooltip_text(_("An inward concave cutout."));
    _modes.append(_round); _modes.append(_inverse); _box.append(_modes);
    _radius_label.set_text_with_mnemonic(_("Radi_us:"));
    _radius_label.set_mnemonic_widget(_radius);
    _radius.set_name("corner-radius");
    _radius.set_width_chars(8); _radius.set_max_width_chars(12); _radius.set_max_length(64);
    _radius.set_tooltip_text(_("Radius in the displayed unit. Zero removes rounding. Changes appear on the canvas after a brief pause or Enter."));
    _decrease.set_label("−"); _increase.set_label("+");
    _decrease.set_tooltip_text(_("Decrease radius")); _increase.set_tooltip_text(_("Increase radius"));
    _decrease.set_focus_on_click(false); _increase.set_focus_on_click(false);
    _radius_row.append(_radius_label); _radius_row.append(_radius);
    _radius_row.append(_decrease); _radius_row.append(_increase); _radius_row.append(_unit);
    _box.append(_radius_row);
    _selected.set_label(_("_Selected")); _selected.set_use_underline(true);
    _all.set_label(_("_All corners")); _all.set_use_underline(true);
    _selected.set_name("corner-selected"); _all.set_name("corner-all");
    _all.set_group(_selected); _selected.set_active(true);
    _all.set_tooltip_text(_("All eligible corners of this one path, including its subpaths."));
    _selected.set_tooltip_text(_("Only selected original nodes. Click a corner to round it; Shift+click to select without rounding."));
    _scopes.append(_selected); _scopes.append(_all); _box.append(_scopes);
    _reason.set_xalign(0); _reason.set_wrap(true); _reason.set_max_width_chars(34); _box.append(_reason);
    _selected.signal_toggled().connect([this] {
        if (!_syncing && _selected.get_active()) { _pending.disconnect(); scope_changed.emit(Scope::Selected); }
    });
    _all.signal_toggled().connect([this] {
        if (!_syncing && _all.get_active()) { _pending.disconnect(); scope_changed.emit(Scope::All); }
    });
    for (auto button : {&_round, &_inverse}) button->signal_toggled().connect([this, button] {
        if (!_syncing && button->get_active()) { _mode_edited = true; apply(); }
    });
    _radius.signal_changed().connect([this] {
        if (!_syncing) { _radius_edited = true; schedule(); }
    });
    _radius.signal_activate().connect(sigc::mem_fun(*this, &CornerRoundingPopover::apply));
    _decrease.signal_clicked().connect([this] { step(-1); });
    _increase.signal_clicked().connect([this] { step(1); });
    update_sensitivity();
}
CornerRoundingPopover::~CornerRoundingPopover() { _pending.disconnect(); unset_child(); }
CornerRoundingPopover::Scope CornerRoundingPopover::scope() const
{
    return _all.get_active() ? Scope::All : Scope::Selected;
}
void CornerRoundingPopover::set_brush(double radius, bool inverse)
{
    _syncing = true;
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    _radius.set_text(g_ascii_formatd(buffer, sizeof(buffer), "%.6g", radius));
    _round.set_active(!inverse); _inverse.set_active(inverse);
    _syncing = false;
}
void CornerRoundingPopover::synchronize(Summary const &s, double conversion,
    Glib::ustring const &unit, Glib::ustring const &reason, std::uint64_t generation)
{
    _pending.disconnect(); _syncing = true;
    _generation = generation; _conversion = conversion;
    // A valid path with no selected corners still allows setting the click radius.
    _eligible = (s.status == CE::Status::Ready || s.status == CE::Status::NoCorners) &&
                reason.empty() && std::isfinite(conversion) && conversion > 0;
    if (s.count) {
        _round.set_active(s.mode && *s.mode == CE::Mode::Round);
        _inverse.set_active(s.mode && *s.mode == CE::Mode::InverseRound);
        char buffer[G_ASCII_DTOSTR_BUF_SIZE];
        _radius.set_text(s.radius ? g_ascii_formatd(buffer, sizeof(buffer), "%.6g", *s.radius / conversion) : "");
    }
    _radius.set_placeholder_text(_("Mixed"));
    _original_radius = s.radius; _original_radius_text = _radius.get_text();
    _unit.set_text(unit);
    _reason.set_text(!reason.empty() ? reason : !s.count
        ? _("Set a radius, then click a corner. Shift+click selects without applying.")
        : s.approximate ? _("Curved joins use the native approximation.")
        : Glib::ustring::compose(ngettext("%1 corner", "%1 corners", s.count), s.count));
    _mode_edited = _radius_edited = false; _syncing = false; update_sensitivity();
}
void CornerRoundingPopover::update_sensitivity()
{
    _radius_row.set_sensitive(_eligible);
    _modes.set_sensitive(_eligible);
}
void CornerRoundingPopover::schedule()
{
    _pending.disconnect();
    _pending = Glib::signal_timeout().connect([this] { apply(); return false; }, 250);
}
void CornerRoundingPopover::step(double delta)
{
    char *end = nullptr;
    auto value = g_strtod(_radius.get_text().c_str(), &end);
    if (!std::isfinite(value)) value = 0;
    // Smaller physical steps in inches/cm, without imposing an artificial unit.
    auto step = _unit.get_text() == "in" ? 0.01 : _unit.get_text() == "cm" ? 0.1 : 1.0;
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    _radius.set_text(g_ascii_formatd(buffer, sizeof(buffer), "%.6g", std::max(0.0, value + delta * step)));
}
void CornerRoundingPopover::apply()
{
    _pending.disconnect();
    if (!_eligible || _syncing) return;
    Request request; request.scope = scope();
    if (_mode_edited) {
        if (_round.get_active()) request.mode = CE::Mode::Round;
        if (_inverse.get_active()) request.mode = CE::Mode::InverseRound;
    }
    if (_radius_edited) {
        auto text = _radius.get_text();
        char *end = nullptr;
        auto entered = g_strtod(text.c_str(), &end);
        bool parsed = end != text.c_str();
        while (end && g_ascii_isspace(*end)) ++end;
        auto value = entered * _conversion;
        if (!parsed || !end || *end || !std::isfinite(entered) || entered < 0 || entered > 1000000 ||
            !std::isfinite(value) || value < 0) {
            _reason.set_text(_("Enter a valid nonnegative radius. No change was made."));
            return;
        }
        if (_original_radius && text == _original_radius_text) value = *_original_radius;
        request.radius = value;
    }
    if (!request.radius && !request.mode) return;
    auto signal = apply_requested;
    auto generation = _generation;
    signal.emit(request, generation);
}
}
