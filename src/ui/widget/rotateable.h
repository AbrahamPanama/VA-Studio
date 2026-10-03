// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   buliabyak@gmail.com
 *
 * Copyright (C) 2007 authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_ROTATEABLE_H
#define INKSCAPE_UI_ROTATEABLE_H

#include <gtkmm/box.h>
#include <gtkmm/gesture.h>
#include <gtkmm/eventcontrollerkey.h>

namespace Gtk {
class GestureDrag;
class EventControllerScroll;
} // namespace Gtk

namespace Inkscape::UI::Widget {

/**
 * Widget adjustable by dragging it to rotate away from a zero-change axis.
 */
class Rotateable: public Gtk::Box
{
public:
    Rotateable();
    ~Rotateable() override;

    double axis;
    double current_axis;
    double maxdecl;
    bool scrolling;

protected:
    double drag_started_x;
    double drag_started_y;
    unsigned modifier;
    bool dragging;
    bool working;
    Glib::RefPtr<Gtk::EventControllerKey> key_controller;
    void sync_key_capture();
    bool handle_drag_key(unsigned keyval);
    void handle_drag_key_release(unsigned keyval);

    static unsigned get_single_modifier(unsigned old, unsigned state);

    Gtk::EventSequenceState on_click(Gtk::GestureDrag const &click, double x, double y);
    Gtk::EventSequenceState on_release(Gtk::GestureDrag const &click, double x, double y);
    Gtk::EventSequenceState on_motion(Gtk::GestureDrag const &motion, double x, double y);
    // Shared gesture path; a synthetic state also lets the regression exercise
    // the threshold and modifier transition without relying on a window server.
    Gtk::EventSequenceState on_motion_with_state(double x, double y, unsigned state);
    bool on_scroll(Gtk::EventControllerScroll const &scroll, double dx, double dy);

    virtual void do_motion (double /*by*/, unsigned /*state*/) {}
    virtual void do_release(double /*by*/, unsigned /*state*/) {}
    virtual void do_scroll (double /*by*/, unsigned /*state*/) {}
    // Return true when a control owns modifier transitions within one gesture.
    virtual bool on_modifier_change(double /*by*/, unsigned /*old*/, unsigned /*next*/) { return false; }
    // Only controls with a live document preview opt into shortcut suppression.
    virtual bool capture_keys_during_drag() const { return false; }
    virtual bool cancel_drag_preview() { return false; }
    virtual void blocked_drag_key() {}
private:
    bool blocked_key_reported = false;
    bool escape_held = false;
};

} // namespace Inkscape::UI::Widget

#endif // INKSCAPE_UI_ROTATEABLE_H

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
