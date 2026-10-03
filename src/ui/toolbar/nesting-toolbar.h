// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLBAR_NESTING_TOOLBAR_H
#define INKSCAPE_UI_TOOLBAR_NESTING_TOOLBAR_H

#include <memory>
#include <vector>

#include "preferences.h"
#include "toolbar.h"

namespace Gtk {
class Box;
class Builder;
class Button;
class CheckButton;
class DropDown;
class Label;
class SpinButton;
}

namespace Inkscape::UI::Widget {
class UnitTracker;
}

namespace Inkscape::UI::Toolbar {

/**
 * Controls bar of the nesting tool (tool work order 6). Every control reads
 * and writes the preference paths used by the Preferences page and the tool,
 * and follows changes made elsewhere through preference observers.
 */
class NestingToolbar final : public Toolbar
{
public:
    NestingToolbar();
    ~NestingToolbar() override;
    void setDesktop(SPDesktop *desktop) override;

    /// Test access to the controls.
    Gtk::SpinButton &spacing_for_testing() { return _spacing; }
    Gtk::Button &nest_button_for_testing() { return _nest; }
    Gtk::Label &sheet_label_for_testing() { return _sheet; }

private:
    explicit NestingToolbar(Glib::RefPtr<Gtk::Builder> const &builder);

    void sync_from_preferences();
    void write_lengths();
    void write_rotation();
    void write_time();
    void update_tool_state();

    std::unique_ptr<UI::Widget::UnitTracker> _tracker;
    Gtk::Box &_settings;
    Gtk::SpinButton &_spacing;
    Gtk::SpinButton &_margin;
    Gtk::SpinButton &_step;
    Gtk::SpinButton &_custom_time;
    Gtk::CheckButton &_show_labels;
    Gtk::Button &_nest;
    Gtk::Label &_sheet;
    Gtk::DropDown *_rotation = nullptr;
    Gtk::DropDown *_time = nullptr;
    std::vector<std::unique_ptr<Preferences::PreferencesObserver>> _observers;
    sigc::scoped_connection _tool_changed;
    sigc::scoped_connection _tool_state;
    bool _updating = false;
};

} // namespace Inkscape::UI::Toolbar

#endif // INKSCAPE_UI_TOOLBAR_NESTING_TOOLBAR_H
