// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLBAR_OFFSET_SHAPES_TOOLBAR_H
#define INKSCAPE_UI_TOOLBAR_OFFSET_SHAPES_TOOLBAR_H

#include <memory>

#include "toolbar.h"

namespace Gtk {
class Builder;
class Button;
class CheckButton;
class SpinButton;
class ToggleButton;
}

namespace Inkscape::UI::Widget {
class UnitTracker;
}

namespace Inkscape::UI::Toolbar {

class OffsetShapesToolbar final : public Toolbar
{
public:
    OffsetShapesToolbar();
    ~OffsetShapesToolbar() override;
    void setDesktop(SPDesktop *desktop) override;

private:
    explicit OffsetShapesToolbar(Glib::RefPtr<Gtk::Builder> const &builder);

    void options_changed();
    void unit_changed();
    void sync_from_preferences();
    void focus_canvas();

    std::unique_ptr<UI::Widget::UnitTracker> _tracker;
    Gtk::SpinButton &_distance;
    Gtk::SpinButton &_miter_limit;
    Gtk::ToggleButton &_outward;
    Gtk::ToggleButton &_inward;
    Gtk::ToggleButton &_both;
    Gtk::ToggleButton &_round;
    Gtk::ToggleButton &_bevel;
    Gtk::ToggleButton &_miter;
    Gtk::CheckButton &_outer_only;
    Gtk::CheckButton &_select_results;
    Gtk::CheckButton &_delete_originals;
    Gtk::CheckButton &_simplify_results;
    Gtk::Button &_cancel;
    Gtk::Button &_apply;
    sigc::scoped_connection _preview_changed;
    bool _updating = false;
};

} // namespace Inkscape::UI::Toolbar

#endif // INKSCAPE_UI_TOOLBAR_OFFSET_SHAPES_TOOLBAR_H
