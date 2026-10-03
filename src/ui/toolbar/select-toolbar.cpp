// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Select toolbar
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *   Vaibhav Malik <vaibhavmalik2018@gmail.com>
 *
 * Copyright (C) 2003-2005 authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "select-toolbar.h"

#include <cmath>

#include <glibmm/i18n.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/togglebutton.h>

#include "desktop.h"
#include "document-undo.h"
#include "object/sp-item-transform.h"
#include "page-manager.h"
#include "selection.h"
#include "ui/builder-utils.h"
#include "ui/icon-names.h"
#include "ui/util.h"
#include "ui/widget/combo-tool-item.h"
#include "ui/widget/spinbutton.h"
#include "ui/widget/unit-tracker.h"

using Inkscape::UI::Widget::UnitTracker;
using Inkscape::Util::Unit;
using Inkscape::Util::Quantity;
using Inkscape::DocumentUndo;

namespace Inkscape::UI::Toolbar {

SelectToolbar::SelectToolbar()
    : SelectToolbar{create_builder("toolbar-select.ui")}
{}

SelectToolbar::SelectToolbar(Glib::RefPtr<Gtk::Builder> const &builder)
    : Toolbar{get_widget<Gtk::Box>(builder, "select-toolbar")}
    , _tracker{std::make_unique<UnitTracker>(Util::UNIT_TYPE_LINEAR)}
    , _action_prefix{"selector:toolbar:"}
    , _select_touch_btn{get_widget<Gtk::ToggleButton>(builder, "_select_touch_btn")}
    , _transform_stroke_btn{get_widget<Gtk::ToggleButton>(builder, "_transform_stroke_btn")}
    , _transform_corners_btn{get_widget<Gtk::ToggleButton>(builder, "_transform_corners_btn")}
    , _transform_gradient_btn{get_widget<Gtk::ToggleButton>(builder, "_transform_gradient_btn")}
    , _transform_pattern_btn{get_widget<Gtk::ToggleButton>(builder, "_transform_pattern_btn")}
    , _reference_selector{UI::Widget::AnchorSelector::Density::Compact}
    , _x_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_x_item")}
    , _y_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_y_item")}
    , _w_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_w_item")}
    , _h_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_h_item")}
    , _scale_w_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_scale_w_item")}
    , _scale_h_item{get_derived_widget<UI::Widget::SpinButton>(builder, "_scale_h_item")}
    , _lock_btn{get_widget<Gtk::ToggleButton>(builder, "_lock_btn")}
{
    auto prefs = Preferences::get();

    setup_derived_spin_button(_x_item, "X");
    setup_derived_spin_button(_y_item, "Y");
    setup_derived_spin_button(_w_item, "width");
    setup_derived_spin_button(_h_item, "height");
    setup_percentage_spin_button(_scale_w_item);
    setup_percentage_spin_button(_scale_h_item);

    auto const reference_index = prefs->getIntLimited(
        "/tools/select/toolbar_reference_point", 0, 0, 8);
    _reference_point = transform_reference_from_index(reference_index);
    _reference_selector.setSelection(reference_index);
    get_widget<Gtk::Box>(builder, "reference_point_box").append(_reference_selector);
    _reference_changed_conn = _reference_selector.connectSelectionChanged(
        sigc::mem_fun(*this, &SelectToolbar::reference_point_changed));

    _select_touch_btn.set_active(prefs->getBool("/tools/select/touch_box", false));
    _select_touch_btn.signal_toggled().connect(sigc::mem_fun(*this, &SelectToolbar::toggle_touch));

    // Use StyleContext to check if the child is a context item (an item that is disabled if there is no selection).
    for (auto &child : UI::children(_toolbar)) {
        if (child.has_css_class("context_item")) {
            _context_items.push_back(&child);
        }
    }

    _transform_stroke_btn.set_active(prefs->getBool("/options/transform/stroke", true));
    _transform_stroke_btn.signal_toggled().connect(sigc::mem_fun(*this, &SelectToolbar::toggle_stroke));

    _transform_corners_btn.set_active(prefs->getBool("/options/transform/rectcorners", true));
    _transform_corners_btn.signal_toggled().connect(sigc::mem_fun(*this, &SelectToolbar::toggle_corners));

    _transform_gradient_btn.set_active(prefs->getBool("/options/transform/gradient", true));
    _transform_gradient_btn.signal_toggled().connect(sigc::mem_fun(*this, &SelectToolbar::toggle_gradient));

    _transform_pattern_btn.set_active(prefs->getBool("/options/transform/pattern", true));
    _transform_pattern_btn.signal_toggled().connect(sigc::mem_fun(*this, &SelectToolbar::toggle_pattern));

    _lock_btn.signal_toggled().connect(sigc::mem_fun(*this, &SelectToolbar::toggle_lock));
    _lock_btn.set_active(prefs->getBool("/tools/select/lock_aspect_ratio", false));
    toggle_lock();

    _box_observer = prefs->createObserver("/tools/bounding_box", [this](const Preferences::Entry& entry) {
        if (_desktop) {
            layout_widget_update(_desktop->getSelection());
        }
    });

    _initMenuBtns();
}

SelectToolbar::~SelectToolbar() = default;

void SelectToolbar::setDesktop(SPDesktop *desktop)
{
    if (_desktop) {
        _selection_changed_conn.disconnect();
        _selection_modified_conn.disconnect();
    }

    Toolbar::setDesktop(desktop);

    if (_desktop) {
        auto sel = _desktop->getSelection();

        // Force update when selection changes.
        _selection_changed_conn = sel->connectChanged(sigc::mem_fun(*this, &SelectToolbar::_selectionChanged));
        _selection_modified_conn = sel->connectModified(sigc::mem_fun(*this, &SelectToolbar::_selectionModified));

        // Update now.
        layout_widget_update(sel);
        _sensitize();
    }
}

void SelectToolbar::setActiveUnit(Util::Unit const *unit)
{
    _tracker->setActiveUnit(unit);
    // The fields always use the document's display unit (the rulers' unit);
    // with no unit menu, the tooltips name it.
    auto const tip = [unit](char const *text) {
        return unit ? Glib::ustring::compose("%1 (%2)", _(text), unit->abbr) : Glib::ustring{_(text)};
    };
    _x_item.set_tooltip_text(tip("Horizontal coordinate of selection"));
    _y_item.set_tooltip_text(tip("Vertical coordinate of selection"));
    _w_item.set_tooltip_text(tip("Width of selection"));
    _h_item.set_tooltip_text(tip("Height of selection"));
}

void SelectToolbar::setup_derived_spin_button(UI::Widget::SpinButton &btn, Glib::ustring const &name)
{
    auto const path = "/tools/select/" + name;
    auto const val = Preferences::get()->getDouble(path, 0.0);
    auto const adj = btn.get_adjustment();
    adj->set_value(val);
    adj->signal_value_changed().connect(sigc::bind(sigc::mem_fun(*this, &SelectToolbar::any_value_changed), adj));
    _tracker->addAdjustment(adj->gobj());

    btn.addUnitTracker(_tracker.get());
    btn.setDefocusTarget(this);

    // select toolbar spin buttons increment by 1.0 with key up/down, and 0.1 with spinner buttons
    btn.set_increment(1.0);
}

void SelectToolbar::setup_percentage_spin_button(UI::Widget::SpinButton &btn)
{
    auto const adj = btn.get_adjustment();
    adj->signal_value_changed().connect(
        sigc::bind(sigc::mem_fun(*this, &SelectToolbar::percentage_value_changed), adj));

    btn.setDefocusTarget(this);
    btn.set_increment(1.0);
    btn.set_trim_zeros(false);
    btn.set_suffix("%");
}

void SelectToolbar::reset_percentage_spin_buttons()
{
    _scale_w_item.get_adjustment()->set_value(100.0);
    _scale_h_item.get_adjustment()->set_value(100.0);
}

void SelectToolbar::_sensitize()
{
    auto const selection = _desktop->getSelection();
    bool const sensitive = selection && !selection->isEmpty();
    for (auto item : _context_items) {
        item->set_sensitive(sensitive);
    }
}

void SelectToolbar::any_value_changed(Glib::RefPtr<Gtk::Adjustment> const &adj)
{
    // quit if run by the XML listener or a unit change
    if (_blocker.pending() || _tracker->isUpdating() || !_desktop) {
        return;
    }

    // in turn, prevent XML listener from responding
    auto guard = _blocker.block();

    auto selection = _desktop->getSelection();
    auto document = _desktop->getDocument();
    auto &pm = document->getPageManager();
    auto page = pm.getSelectedPageRect();
    auto page_correction = document->get_origin_follows_page();

    document->ensureUpToDate();

    Geom::OptRect bbox_user = selection->preferredBounds();

    if (!bbox_user) {
        return;
    }

    auto const unit = _tracker->getActiveUnit();

    double old_w = bbox_user->width();
    double old_h = bbox_user->height();
    double new_w, new_h, new_x, new_y = 0;
    auto const reference = normalized_reference(_reference_point);

    auto _adj_x = _x_item.get_adjustment();
    auto _adj_y = _y_item.get_adjustment();
    auto _adj_w = _w_item.get_adjustment();
    auto _adj_h = _h_item.get_adjustment();

    if (unit->type == Util::UNIT_TYPE_LINEAR) {
        new_w = Quantity::convert(_adj_w->get_value(), unit, "px");
        new_h = Quantity::convert(_adj_h->get_value(), unit, "px");
        new_x = Quantity::convert(_adj_x->get_value(), unit, "px");
        new_y = Quantity::convert(_adj_y->get_value(), unit, "px");

    } else {
        double old_x = bbox_user->min()[Geom::X] + old_w * reference.x();
        double old_y = bbox_user->min()[Geom::Y] + old_h * reference.y();

        // Adjust against selected page, so later correction isn't broken.
        if (page_correction) {
            old_x -= page.left();
            old_y -= page.top();
        }

        new_x = old_x * (_adj_x->get_value() / 100 / unit->factor);
        new_y = old_y * (_adj_y->get_value() / 100 / unit->factor);
        new_w = old_w * (_adj_w->get_value() / 100 / unit->factor);
        new_h = old_h * (_adj_h->get_value() / 100 / unit->factor);
    }

    // Keep proportions if lock is on, before calculating the anchored bounds.
    if (_lock_btn.get_active()) {
        if (adj == _adj_h) {
            if (old_h != 0.0) {
                new_w = new_h / old_h * old_w;
            }
        } else if (adj == _adj_w) {
            if (old_w != 0.0) {
                new_h = new_w / old_w * old_h;
            }
        }
    }

    auto const target_bounds = bounds_from_reference(
        Geom::Point{new_x, new_y}, new_w, new_h, _reference_point);
    double x0 = target_bounds.x0;
    double y0 = target_bounds.y0;

    // Adjust according to the selected page, if needed
    if (page_correction) {
        x0 += page.left();
        y0 += page.top();
    }

    double const x1 = x0 + new_w;
    double const y1 = y0 + new_h;

    apply_selection_transform(selection, x0, y0, x1, y1, unit);
}

void SelectToolbar::percentage_value_changed(Glib::RefPtr<Gtk::Adjustment> const &adj)
{
    if (_blocker.pending() || !_desktop) {
        return;
    }

    auto selection = _desktop->getSelection();
    bool transformed = false;

    {
        auto guard = _blocker.block();
        auto const percentage = adj->get_value();
        reset_percentage_spin_buttons();

        if (!selection || selection->isEmpty() || !std::isfinite(percentage)) {
            return;
        }

        auto const bbox = selection->preferredBounds();
        if (!bbox) {
            return;
        }

        bool const horizontal = adj == _scale_w_item.get_adjustment();
        bool const vertical = adj == _scale_h_item.get_adjustment();
        if (!horizontal && !vertical) {
            return;
        }

        auto scale_x = horizontal ? percentage / 100.0 : 1.0;
        auto scale_y = vertical ? percentage / 100.0 : 1.0;
        if (_lock_btn.get_active()) {
            scale_x = scale_y = percentage / 100.0;
        }

        // Match the Transform dialog: zero is represented by the smallest safe non-singular size.
        auto scaled_dimension = [] (double dimension, double factor) {
            auto result = dimension * factor;
            if (std::abs(result) < 1e-6) {
                result = std::copysign(1e-6, factor == 0.0 ? 1.0 : factor);
            }
            return result;
        };

        auto const old_w = bbox->width();
        auto const old_h = bbox->height();
        auto const new_w = scaled_dimension(old_w, scale_x);
        auto const new_h = scaled_dimension(old_h, scale_y);
        auto const target_bounds = resize_around_reference(
            *bbox, new_w, new_h, _reference_point);

        auto const *undo_key = _lock_btn.get_active()
                                 ? "selector:toolbar:scale-percent:locked"
                                 : horizontal
                                     ? "selector:toolbar:scale-percent:horizontal"
                                     : "selector:toolbar:scale-percent:vertical";
        transformed = apply_selection_transform(selection,
                                                target_bounds.x0,
                                                target_bounds.y0,
                                                target_bounds.x1,
                                                target_bounds.y1,
                                                nullptr,
                                                undo_key);
    }

    // Selection modification signals are blocked while applying. Refresh absolute W/H afterwards.
    if (transformed) {
        layout_widget_update(selection);
    }
}

bool SelectToolbar::apply_selection_transform(Selection *selection, double x0, double y0,
                                              double x1, double y1, Unit const *delta_unit,
                                              char const *undo_key_override)
{
    auto const bbox_vis = selection->visualBounds();
    auto const bbox_geom = selection->geometricBounds();
    auto const bbox_user = selection->preferredBounds();
    if (!bbox_vis || !bbox_geom || !bbox_user) {
        return false;
    }

    // Scales and moves are measured in px. Preserve the legacy threshold in the active linear unit.
    auto mh = std::abs(x0 - bbox_user->min()[Geom::X]);
    auto sh = std::abs(x1 - bbox_user->max()[Geom::X]);
    auto mv = std::abs(y0 - bbox_user->min()[Geom::Y]);
    auto sv = std::abs(y1 - bbox_user->max()[Geom::Y]);
    if (delta_unit && delta_unit->type == Util::UNIT_TYPE_LINEAR) {
        mh = Quantity::convert(mh, "px", delta_unit);
        sh = Quantity::convert(sh, "px", delta_unit);
        mv = Quantity::convert(mv, "px", delta_unit);
        sv = Quantity::convert(sv, "px", delta_unit);
    }

    char const *action_key = undo_key_override;
    if (action_key) {
        // Percentage fields have their own precision and must also work on artwork smaller than
        // the absolute toolbar's 0.0005-unit rounding threshold.
        constexpr auto relative_epsilon = 1e-12;
        if (mh <= relative_epsilon && sh <= relative_epsilon &&
            mv <= relative_epsilon && sv <= relative_epsilon) {
            return false;
        }
    } else {
        action_key = get_action_key(mh, sh, mv, sv);
        if (!action_key) {
            return false;
        }
    }

    auto prefs = Preferences::get();
    auto const transform_stroke = prefs->getBool("/options/transform/stroke", true);
    auto const preserve = prefs->getBool("/options/preservetransform/value", false);

    auto const scaler = selection_resize_affine(*selection, prefs->getInt("/tools/bounding_box") == 0,
                                                transform_stroke, preserve, x0, y0, x1, y1);
    // A refused request (zero-extent selection, see sp-item-transform.cpp) is the identity: change
    // nothing and record no Undo step.
    if (!scaler || scaler->isIdentity(1e-12)) {
        return false;
    }

    selection->applyAffine(*scaler);
    DocumentUndo::maybeDone(_desktop->getDocument(), action_key,
                            RC_("Undo", "Transform by toolbar"),
                            INKSCAPE_ICON("tool-pointer"));
    return true;
}

void SelectToolbar::layout_widget_update(Selection *sel)
{
    if (_blocker.pending()) {
        return;
    }

    auto guard = _blocker.block();
    reset_percentage_spin_buttons();

    if (sel && !sel->isEmpty()) {
        if (auto const bbox = sel->preferredBounds()) {
            auto const unit = _tracker->getActiveUnit();

            auto width = bbox->width();
            auto height = bbox->height();
            auto const reference = reference_position(*bbox, _reference_point);
            auto x = reference.x();
            auto y = reference.y();

            if (_desktop->getDocument()->get_origin_follows_page()) {
                auto &pm = _desktop->getDocument()->getPageManager();
                auto page = pm.getSelectedPageRect();
                x -= page.left();
                y -= page.top();
            }

            auto _adj_x = _x_item.get_adjustment();
            auto _adj_y = _y_item.get_adjustment();
            auto _adj_w = _w_item.get_adjustment();
            auto _adj_h = _h_item.get_adjustment();

            if (unit->type == Util::UNIT_TYPE_DIMENSIONLESS) {
                double const val = unit->factor * 100;
                _adj_x->set_value(val);
                _adj_y->set_value(val);
                _adj_w->set_value(val);
                _adj_h->set_value(val);
                _tracker->setFullVal(_adj_x->gobj(), x);
                _tracker->setFullVal(_adj_y->gobj(), y);
                _tracker->setFullVal(_adj_w->gobj(), width);
                _tracker->setFullVal(_adj_h->gobj(), height);
            } else {
                _adj_x->set_value(Quantity::convert(x, "px", unit));
                _adj_y->set_value(Quantity::convert(y, "px", unit));
                _adj_w->set_value(Quantity::convert(width, "px", unit));
                _adj_h->set_value(Quantity::convert(height, "px", unit));
            }
        }
    }
}

void SelectToolbar::reference_point_changed()
{
    auto const index = _reference_selector.getSelection();
    _reference_point = transform_reference_from_index(index);
    Preferences::get()->setInt("/tools/select/toolbar_reference_point", index);

    if (_desktop) {
        layout_widget_update(_desktop->getSelection());
    }
}

void SelectToolbar::_selectionChanged(Selection *selection)
{
    assert(_desktop->getSelection() == selection);
    layout_widget_update(selection);
    _sensitize();
}

void SelectToolbar::_selectionModified(Selection *selection, unsigned flags)
{
    assert(_desktop->getSelection() == selection);
    if (flags & (SP_OBJECT_MODIFIED_FLAG        |
                 SP_OBJECT_PARENT_MODIFIED_FLAG |
                 SP_OBJECT_CHILD_MODIFIED_FLAG  ))
    {
        layout_widget_update(selection);
    }
}

char const *SelectToolbar::get_action_key(double mh, double sh, double mv, double sv)
{
    // do the action only if one of the scales/moves is greater than half the last significant
    // digit in the spinbox (currently spinboxes have 3 fractional digits, so that makes 0.0005). If
    // the value was changed by the user, the difference will be at least that much; otherwise it's
    // just rounding difference between the spinbox value and actual value, so no action is
    // performed
    double const threshold = 5e-4;
    char const *const action = mh > threshold ? "move:horizontal:" :
                               sh > threshold ? "scale:horizontal:" :
                               mv > threshold ? "move:vertical:" :
                               sv > threshold ? "scale:vertical:" : nullptr;
    if (!action) {
        return nullptr;
    }
    _action_key = _action_prefix + action;
    return _action_key.c_str();
}

void SelectToolbar::toggle_lock()
{
    Preferences::get()->setBool("/tools/select/lock_aspect_ratio", _lock_btn.get_active());

    _lock_btn.set_image_from_icon_name(_lock_btn.get_active() ? "object-locked" : "object-unlocked");
}

void SelectToolbar::toggle_touch()
{
    Preferences::get()->setBool("/tools/select/touch_box", _select_touch_btn.get_active());
}

void SelectToolbar::toggle_stroke()
{
    bool active = _transform_stroke_btn.get_active();
    Preferences::get()->setBool("/options/transform/stroke", active);
    if (active) {
        _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Now <b>stroke width</b> is <b>scaled</b> when objects are scaled."));
    } else {
        _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Now <b>stroke width</b> is <b>not scaled</b> when objects are scaled."));
    }
}

void SelectToolbar::toggle_corners()
{
    bool active = _transform_corners_btn.get_active();
    Preferences::get()->setBool("/options/transform/rectcorners", active);
    if (active) {
        _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Now <b>rounded rectangle corners</b> are <b>scaled</b> when rectangles are scaled."));
    } else {
        _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Now <b>rounded rectangle corners</b> are <b>not scaled</b> when rectangles are scaled."));
    }
}

void SelectToolbar::toggle_gradient()
{
    bool active = _transform_gradient_btn.get_active();
    Preferences::get()->setBool("/options/transform/gradient", active);
    if (active) {
        _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Now <b>gradients</b> are <b>transformed</b> along with their objects when those are transformed (moved, scaled, rotated, or skewed)."));
    } else {
        _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Now <b>gradients</b> remain <b>fixed</b> when objects are transformed (moved, scaled, rotated, or skewed)."));
    }
}

void SelectToolbar::toggle_pattern()
{
    bool active = _transform_pattern_btn.get_active();
    Preferences::get()->setInt("/options/transform/pattern", active);
    if (active) {
        _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Now <b>patterns</b> are <b>transformed</b> along with their objects when those are transformed (moved, scaled, rotated, or skewed)."));
    } else {
        _desktop->messageStack()->flash(INFORMATION_MESSAGE, _("Now <b>patterns</b> remain <b>fixed</b> when objects are transformed (moved, scaled, rotated, or skewed)."));
    }
}

} // namespace Inkscape::UI::Toolbar

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
