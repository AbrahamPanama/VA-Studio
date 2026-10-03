// SPDX-License-Identifier: GPL-2.0-or-later

#include "bitmap-eraser-toolbar.h"

#include <algorithm>
#include <gtkmm/togglebutton.h>

#include "desktop.h"
#include "preferences.h"
#include "ui/builder-utils.h"
#include "ui/widget/canvas.h"
#include "ui/widget/spinbutton.h"

namespace Inkscape::UI::Toolbar {

BitmapEraserToolbar::BitmapEraserToolbar()
    : BitmapEraserToolbar{create_builder("toolbar-bitmap-eraser.ui")}
{}

BitmapEraserToolbar::BitmapEraserToolbar(Glib::RefPtr<Gtk::Builder> const &builder)
    : Toolbar{get_widget<Gtk::Box>(builder, "bitmap-eraser-toolbar")}
    , _round_btn(get_widget<Gtk::ToggleButton>(builder, "round-tip"))
    , _square_btn(get_widget<Gtk::ToggleButton>(builder, "square-tip"))
    , _size_item(get_derived_widget<UI::Widget::SpinButton>(builder, "bitmap-eraser-size"))
    , _hardness_item(get_derived_widget<UI::Widget::SpinButton>(builder, "bitmap-eraser-hardness"))
    , _pressure_btn(get_widget<Gtk::ToggleButton>(builder, "bitmap-eraser-pressure"))
{
    auto *prefs = Preferences::get();
    auto const shape = prefs->getInt("/tools/bitmaperaser/shape", 0);
    _round_btn.set_active(shape != 1);
    _square_btn.set_active(shape == 1);
    _size_item.get_adjustment()->set_value(std::clamp(prefs->getDouble("/tools/bitmaperaser/size", 40.0), 1.0, 2000.0));
    _hardness_item.get_adjustment()->set_value(
        std::clamp(prefs->getDouble("/tools/bitmaperaser/hardness", 100.0), 0.0, 100.0));
    _pressure_btn.set_active(prefs->getBool("/tools/bitmaperaser/usepressure", false));

    _size_item.set_custom_numeric_menu_data(
        {{1, ""}, {5, ""}, {10, ""}, {20, ""}, {40, ""}, {80, ""}, {160, ""}, {320, ""}, {640, ""}});
    _hardness_item.set_custom_numeric_menu_data({{0, ""}, {25, ""}, {50, ""}, {75, ""}, {100, ""}});
    _size_item.setDefocusTarget(this);
    _hardness_item.setDefocusTarget(this);

    _round_btn.signal_toggled().connect([this] {
        if (_round_btn.get_active())
            on_shape_changed(0);
    });
    _square_btn.signal_toggled().connect([this] {
        if (_square_btn.get_active())
            on_shape_changed(1);
    });
    _size_item.get_adjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &BitmapEraserToolbar::on_size_changed));
    _hardness_item.get_adjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &BitmapEraserToolbar::on_hardness_changed));
    _pressure_btn.signal_toggled().connect(sigc::mem_fun(*this, &BitmapEraserToolbar::on_pressure_toggled));

    _initMenuBtns();
}

void BitmapEraserToolbar::focus_canvas()
{
    if (_desktop && _desktop->getCanvas())
        _desktop->getCanvas()->grab_focus();
}

void BitmapEraserToolbar::on_shape_changed(int shape)
{
    Preferences::get()->setInt("/tools/bitmaperaser/shape", shape);
    focus_canvas();
}

void BitmapEraserToolbar::on_size_changed()
{
    Preferences::get()->setDouble("/tools/bitmaperaser/size", _size_item.get_adjustment()->get_value());
}

void BitmapEraserToolbar::on_hardness_changed()
{
    Preferences::get()->setDouble("/tools/bitmaperaser/hardness", _hardness_item.get_adjustment()->get_value());
}

void BitmapEraserToolbar::on_pressure_toggled()
{
    Preferences::get()->setBool("/tools/bitmaperaser/usepressure", _pressure_btn.get_active());
    focus_canvas();
}

} // namespace Inkscape::UI::Toolbar
