// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * anchor-selector.h
 *
 *  Created on: Mar 22, 2012
 *      Author: denis
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef ANCHOR_SELECTOR_H
#define ANCHOR_SELECTOR_H

#include <array>
#include <gtkmm/box.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/grid.h>

namespace Glib {
class ustring;
} // namespace Glib

namespace Inkscape::UI::Widget {

class AnchorSelector final : public Gtk::Box
{
public:
    enum class Density { Normal, Compact };

    explicit AnchorSelector(Density density = Density::Normal);

    int getHorizontalAlignment() const { return _selection % 3; }
    int getVerticalAlignment  () const { return _selection / 3; }
    int getSelection() const { return _selection; }

    sigc::connection connectSelectionChanged(sigc::slot<void ()>);

    void setAlignment(int horizontal, int vertical, bool emit = false);
    void setSelection(int index, bool emit = false);

private:
    std::array<Gtk::ToggleButton, 9> _buttons;
    Density            _density;
    int                _selection;
    bool               _programmatic = false;
    Gtk::Grid          _container;

    sigc::signal<void ()> _selectionChanged;

    void setupButton(const Glib::ustring &icon, Gtk::ToggleButton &button, int index);
    void btn_activated(int index);
    void updateCompactLabels();
};

} // namespace Inkscape::UI::Widget

#endif // ANCHOR_SELECTOR_H

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
