// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * anchor-selector.cpp
 *
 *  Created on: Mar 22, 2012
 *      Author: denis
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/widget/anchor-selector.h"

#include <glibmm/i18n.h>
#include <gtkmm/image.h>

#include "ui/icon-loader.h"
#include "ui/icon-names.h"

namespace Inkscape::UI::Widget {

namespace {

char const *anchor_names[] = {
    N_("top left"),
    N_("top center"),
    N_("top right"),
    N_("center left"),
    N_("center"),
    N_("center right"),
    N_("bottom left"),
    N_("bottom center"),
    N_("bottom right")
};

} // namespace

void AnchorSelector::setupButton(const Glib::ustring &icon, Gtk::ToggleButton &button, int index)
{
    button.set_has_frame(false);
    button.set_tooltip_text(Glib::ustring::compose(
        _("Use %1 as the reference point for position and scaling"), _(anchor_names[index])));

    if (_density == Density::Compact) {
        button.set_label("□");
        button.set_focusable(true);
        button.add_css_class("reference-point-button");
    } else {
        auto const buttonIcon = Gtk::manage(sp_get_icon_image(icon, Gtk::IconSize::NORMAL));
        button.set_child(*buttonIcon);
        button.set_focusable(false);
    }
}

AnchorSelector::AnchorSelector(Density density)
    : _density(density)
    , _selection(density == Density::Compact ? 0 : 4)
{
    set_halign(Gtk::Align::CENTER);
    add_css_class(_density == Density::Compact ? "reference-point-selector" : "anchor-selector");
    setupButton(INKSCAPE_ICON("boundingbox_top_left"), _buttons[0], 0);
    setupButton(INKSCAPE_ICON("boundingbox_top"), _buttons[1], 1);
    setupButton(INKSCAPE_ICON("boundingbox_top_right"), _buttons[2], 2);
    setupButton(INKSCAPE_ICON("boundingbox_left"), _buttons[3], 3);
    setupButton(INKSCAPE_ICON("boundingbox_center"), _buttons[4], 4);
    setupButton(INKSCAPE_ICON("boundingbox_right"), _buttons[5], 5);
    setupButton(INKSCAPE_ICON("boundingbox_bottom_left"), _buttons[6], 6);
    setupButton(INKSCAPE_ICON("boundingbox_bottom"), _buttons[7], 7);
    setupButton(INKSCAPE_ICON("boundingbox_bottom_right"), _buttons[8], 8);

    _container.set_row_homogeneous();
    _container.set_column_homogeneous(true);
    _container.set_row_spacing(0);
    _container.set_column_spacing(0);

    for (std::size_t i = 0; i < _buttons.size(); ++i) {
        if (i > 0) {
            _buttons[i].set_group(_buttons[0]);
        }
        _buttons[i].signal_toggled().connect(
            sigc::bind(sigc::mem_fun(*this, &AnchorSelector::btn_activated), i));

        _container.attach(_buttons[i], i % 3, i / 3, 1, 1);
    }

    _buttons[_selection].set_active();
    updateCompactLabels();

    append(_container);
}

sigc::connection AnchorSelector::connectSelectionChanged(sigc::slot<void ()> slot)
{
    return _selectionChanged.connect(std::move(slot));
}

void AnchorSelector::btn_activated(int index)
{
    if (!_programmatic && _buttons[index].get_active() && _selection != index) {
        _selection = index;
        updateCompactLabels();
        _selectionChanged.emit();
    }
}

void AnchorSelector::setAlignment(int horizontal, int vertical, bool emit)
{
    setSelection(3 * vertical + horizontal, emit);
}

void AnchorSelector::setSelection(int index, bool emit)
{
    if (index < 0 || index >= static_cast<int>(_buttons.size()) || index == _selection) {
        return;
    }

    _programmatic = true;
    _selection = index;
    _buttons[_selection].set_active();
    _programmatic = false;
    updateCompactLabels();

    if (emit) {
        _selectionChanged.emit();
    }
}

void AnchorSelector::updateCompactLabels()
{
    if (_density != Density::Compact) {
        return;
    }

    for (std::size_t i = 0; i < _buttons.size(); ++i) {
        _buttons[i].set_label(static_cast<int>(i) == _selection ? "■" : "□");
    }
}

} // namespace Inkscape::UI::Widget

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
