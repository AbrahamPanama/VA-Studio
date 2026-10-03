// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_WIDGET_CORNER_ROUNDING_POPOVER_H
#define INKSCAPE_UI_WIDGET_CORNER_ROUNDING_POPOVER_H
#include <gtkmm/popover.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/label.h>
#include <gtkmm/entry.h>
#include <cstdint>
#include "live_effects/fillet-chamfer-edit.h"

namespace Inkscape::UI::Widget {
// The host owns target identity, units and Undo. Debounced input never outlives
// a synchronization (selection, units, tool, Undo) or closure.
class CornerRoundingPopover final : public Gtk::Popover {
public:
    CornerRoundingPopover();
    ~CornerRoundingPopover() override;
    using Scope = LivePathEffect::CornerEdit::Scope;
    using Summary = LivePathEffect::CornerEdit::Summary;
    using Request = LivePathEffect::CornerEdit::Request;
    Scope scope() const;
    void set_brush(double radius_in_display_units, bool inverse);
    sigc::signal<void()> exit_requested;
    void show_error(Glib::ustring const &text) { _reason.set_text(text); }
    // generation is owned by the host; every target/scope/tool/Undo change
    // invalidates outstanding input. A nonempty reason disables editing.
    void synchronize(Summary const &, double input_units_per_display_unit,
                     Glib::ustring const &unit, Glib::ustring const &reason, std::uint64_t generation);
    sigc::signal<void(Scope)> scope_changed;
    sigc::signal<void(Request const &, std::uint64_t)> apply_requested;
private:
    Gtk::Box _box{Gtk::Orientation::VERTICAL, 6};
    Gtk::Box _header{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Label _title;
    Gtk::Button _close;
    Gtk::Box _scopes{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::CheckButton _selected, _all;
    Gtk::Box _modes{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::CheckButton _round, _inverse;
    Gtk::Box _radius_row{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Label _radius_label;
    Gtk::Entry _radius;
    Gtk::Label _unit;
    Gtk::Label _reason;
    Gtk::Button _decrease, _increase;
    sigc::scoped_connection _pending;
    bool _syncing = false;
    bool _eligible = false;
    bool _mode_edited = false, _radius_edited = false;
    double _conversion = 1;
    std::optional<double> _original_radius;
    Glib::ustring _original_radius_text;
    std::uint64_t _generation = 0;
    void update_sensitivity();
    void schedule();
    void step(double delta);
    void apply();
};
} // namespace Inkscape::UI::Widget
#endif
