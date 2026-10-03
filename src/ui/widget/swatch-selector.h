// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * TODO: insert short description here
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef SEEN_SP_SWATCH_SELECTOR_H
#define SEEN_SP_SWATCH_SELECTOR_H

#include <gtkmm/box.h>

namespace Inkscape::Colors {
class ColorSet;
}

class SPDocument;
class SPGradient;

namespace Inkscape {
namespace UI {
namespace Widget {
class GradientSelector;

class SwatchSelector : public Gtk::Box
{
public:
    SwatchSelector();

    void setVector(SPDocument *doc, SPGradient *vector);

    GradientSelector *getGradientSelector() { return _gsel; }

private:
    void _changedCb();

    GradientSelector *_gsel = nullptr;
    std::shared_ptr<Colors::ColorSet> _colors;
    bool _updating_color = false;
};

} // namespace Widget
} // namespace UI
} // namespace Inkscape

#endif // SEEN_SP_SWATCH_SELECTOR_H

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

