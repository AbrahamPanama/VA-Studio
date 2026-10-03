// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   buliabyak@gmail.com
 *
 * Copyright (C) 2007 authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "rotateable.h"

#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/root.h>

#include "ui/controller.h"
#include "ui/tools/tool-base.h"

namespace Inkscape::UI::Widget {

Rotateable::Rotateable():
    axis(-M_PI/4),
    maxdecl(M_PI/4),
    dragging(false),
    working(false),
    scrolling(false),
    modifier(0),
    current_axis(axis)
{
    auto const click = Gtk::GestureDrag::create();
    click->set_button(1); // left
    click->signal_drag_begin().connect(Controller::use_state(sigc::mem_fun(*this, &Rotateable::on_click), *click));
    click->signal_drag_end().connect(Controller::use_state(sigc::mem_fun(*this, &Rotateable::on_release), *click));
    click->signal_drag_update().connect(Controller::use_state(sigc::mem_fun(*this, &Rotateable::on_motion), *click));
    add_controller(click);

    key_controller = Gtk::EventControllerKey::create();
    key_controller->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    key_controller->signal_key_pressed().connect([this](unsigned keyval, unsigned, Gdk::ModifierType) {
        return handle_drag_key(keyval);
    }, true);
    key_controller->signal_key_released().connect([this](unsigned keyval, unsigned, Gdk::ModifierType) {
        handle_drag_key_release(keyval);
    });

    auto const scroll = Gtk::EventControllerScroll::create();
    scroll->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    scroll->signal_scroll().connect([this, &scroll = *scroll](auto &&...args) { return on_scroll(scroll, args...); }, true);
    add_controller(scroll);
}

bool Rotateable::handle_drag_key(unsigned keyval)
{
    if (escape_held && !dragging && keyval == GDK_KEY_Escape) return true;
    if (!dragging || !working || !capture_keys_during_drag()) return false;
    switch (keyval) {
        case GDK_KEY_Shift_L: case GDK_KEY_Shift_R:
        case GDK_KEY_Control_L: case GDK_KEY_Control_R:
        case GDK_KEY_Alt_L: case GDK_KEY_Alt_R:
        case GDK_KEY_Meta_L: case GDK_KEY_Meta_R:
        case GDK_KEY_Super_L: case GDK_KEY_Super_R:
        case GDK_KEY_ISO_Level3_Shift: case GDK_KEY_Caps_Lock: case GDK_KEY_Num_Lock:
        case GDK_KEY_Hyper_L: case GDK_KEY_Hyper_R:
        return false;
    case GDK_KEY_Escape:
        if (cancel_drag_preview()) {
            escape_held = true;
            dragging = false;
            working = false;
            sync_key_capture();
        }
        return true;
    default:
        if (!blocked_key_reported) {
            blocked_key_reported = true;
            blocked_drag_key();
        }
        return true;
    }
}

void Rotateable::handle_drag_key_release(unsigned keyval)
{
    if (keyval != GDK_KEY_Escape || !escape_held) return;
    escape_held = false;
    sync_key_capture();
}

void Rotateable::sync_key_capture()
{
    // Key events target the focused canvas, so a controller on this status-bar
    // widget would not see its shortcuts. Install it at the window root only
    // for the live preview, retaining it through Escape release so repeats
    // cannot reach the canvas.
    auto *host = key_controller->get_widget();
    auto *root = dynamic_cast<Gtk::Widget *>(get_root());
    if ((!escape_held && (!dragging || !working || !capture_keys_during_drag())) || !root) {
        if (host) host->remove_controller(key_controller);
    } else if (host != root) {
        if (host) host->remove_controller(key_controller);
        root->add_controller(key_controller);
    }
}

Gtk::EventSequenceState Rotateable::on_click(Gtk::GestureDrag const &click, double x, double y)
{
    drag_started_x = x;
    drag_started_y = y;
    auto const state = click.get_current_event_state();
    modifier = get_single_modifier(modifier, unsigned(state));
    dragging = true;
    working = false;
    blocked_key_reported = false;
    escape_held = false;
    sync_key_capture();
    current_axis = axis;
    return Gtk::EventSequenceState::NONE; // immediately claiming would prevent non dragging clicks
}

unsigned Rotateable::get_single_modifier(unsigned old, unsigned state)
{
    if (old == 0 || old == 3) {
        if (state & GDK_CONTROL_MASK)
            return 1; // ctrl
        if (state & GDK_SHIFT_MASK)
            return 2; // shift
        if (state & GDK_ALT_MASK)
            return 3; // alt
        return 0;
    }

    if (!(state & GDK_CONTROL_MASK) && !(state & GDK_SHIFT_MASK)) {
        if (state & GDK_ALT_MASK)
            return 3; // alt
        else
            return 0; // none
    }

    if (old == 1) {
        if (state & GDK_SHIFT_MASK && !(state & GDK_CONTROL_MASK))
            return 2; // shift
        if (state & GDK_ALT_MASK && !(state & GDK_CONTROL_MASK))
           return 3; // alt
        return 1;
    }

    if (old == 2) {
        if (state & GDK_CONTROL_MASK && !(state & GDK_SHIFT_MASK))
            return 1; // ctrl
        if (state & GDK_ALT_MASK && !(state & GDK_SHIFT_MASK))
           return 3; // alt
        return 2;
    }

    return old;
}

Gtk::EventSequenceState Rotateable::on_motion(Gtk::GestureDrag const &motion, double x, double y)
{
    return on_motion_with_state(x, y, static_cast<unsigned>(motion.get_current_event_state()));
}

Gtk::EventSequenceState Rotateable::on_motion_with_state(double x, double y, unsigned state)
{
    if (!dragging) {
        return Gtk::EventSequenceState::NONE;
    }

    double dist = Geom::L2(Geom::Point(x, y));
    if (dist > 20) {
        working = true;

        double angle = atan2(y, x);
        double force = CLAMP (-(angle - current_axis)/maxdecl, -1, 1);
        if (fabs(force) < 0.002)
            force = 0; // snap to zero

        auto const new_modifier = get_single_modifier(modifier, state);
        if (modifier != new_modifier) {
            if (on_modifier_change(force, modifier, new_modifier)) {
                modifier = new_modifier;
                do_motion(force, modifier);
                sync_key_capture();
                return Gtk::EventSequenceState::CLAIMED;
            }
            // user has switched modifiers in mid drag, close past drag and start a new
            // one, redefining axis temporarily
            do_release(force, modifier);
            current_axis = angle;
            modifier = new_modifier;
        } else {
            do_motion(force, modifier);
        }
        sync_key_capture();
        return Gtk::EventSequenceState::CLAIMED;
    }

    Inkscape::UI::Tools::gobble_motion_events(GDK_BUTTON1_MASK);
    return Gtk::EventSequenceState::NONE;
}

Gtk::EventSequenceState Rotateable::on_release(Gtk::GestureDrag const & /*click*/, double x, double y)
{
    if (dragging && working) {
        double angle = atan2(y, x);
        double force = CLAMP(-(angle - current_axis) / maxdecl, -1, 1);
        if (fabs(force) < 0.002)
            force = 0; // snap to zero

        do_release(force, modifier);
        current_axis = axis;
        dragging = false;
        working = false;
        sync_key_capture();
        return Gtk::EventSequenceState::CLAIMED;
    }

    dragging = false;
    working = false;
    sync_key_capture();
    return Gtk::EventSequenceState::NONE;
}

bool Rotateable::on_scroll(Gtk::EventControllerScroll const &scroll, double /*dx*/, double dy)
{
    double change = 0.0;
    double delta_y_clamped = CLAMP(dy, -1.0, 1.0); // values > 1 result in excessive changes
    change = 1.0 * -delta_y_clamped;

    auto const state = scroll.get_current_event_state();
    modifier = get_single_modifier(modifier, static_cast<unsigned>(state));
    dragging = false;
    working = false;
    sync_key_capture();
    scrolling = true;
    current_axis = axis;

    do_scroll(change, modifier);

    dragging = false;
    working = false;
    scrolling = false;

    return true;
}

Rotateable::~Rotateable()
{
    if (auto *host = key_controller->get_widget()) host->remove_controller(key_controller);
}

} // namespace Inkscape::UI::Widget

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
