// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLBAR_BITMAP_ERASER_TOOLBAR_H
#define INKSCAPE_UI_TOOLBAR_BITMAP_ERASER_TOOLBAR_H

#include "toolbar.h"

namespace Gtk {
class Builder;
class ToggleButton;
} // namespace Gtk

namespace Inkscape::UI::Widget {
class SpinButton;
}

namespace Inkscape::UI::Toolbar {

class BitmapEraserToolbar final : public Toolbar
{
public:
    BitmapEraserToolbar();

private:
    explicit BitmapEraserToolbar(Glib::RefPtr<Gtk::Builder> const &builder);

    void on_shape_changed(int shape);
    void on_size_changed();
    void on_hardness_changed();
    void on_pressure_toggled();
    void focus_canvas();

    Gtk::ToggleButton &_round_btn;
    Gtk::ToggleButton &_square_btn;
    UI::Widget::SpinButton &_size_item;
    UI::Widget::SpinButton &_hardness_item;
    Gtk::ToggleButton &_pressure_btn;
};

} // namespace Inkscape::UI::Toolbar

#endif // INKSCAPE_UI_TOOLBAR_BITMAP_ERASER_TOOLBAR_H
