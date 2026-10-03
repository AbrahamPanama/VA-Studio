// SPDX-License-Identifier: GPL-2.0-or-later

#include "offset-shapes-toolbar.h"

#include <algorithm>

#include <gtkmm/adjustment.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/togglebutton.h>

#include "desktop.h"
#include "object/sp-namedview.h"
#include "path/offset-shapes.h"
#include "preferences.h"
#include "ui/builder-utils.h"
#include "ui/tools/offset-shapes-tool.h"
#include "ui/widget/canvas.h"
#include "ui/widget/unit-tracker.h"
#include "util/units.h"

namespace Inkscape::UI::Toolbar {
namespace {

Tools::OffsetShapesTool *get_offset_tool(SPDesktop *desktop)
{
    return desktop ? dynamic_cast<Tools::OffsetShapesTool *>(desktop->getTool()) : nullptr;
}

} // namespace

OffsetShapesToolbar::OffsetShapesToolbar()
    : OffsetShapesToolbar{create_builder("toolbar-offset-shapes.ui")}
{}

OffsetShapesToolbar::OffsetShapesToolbar(Glib::RefPtr<Gtk::Builder> const &builder)
    : Toolbar{get_widget<Gtk::Box>(builder, "offset-shapes-toolbar")}
    , _tracker(std::make_unique<UI::Widget::UnitTracker>(Util::UNIT_TYPE_LINEAR))
    , _distance(get_widget<Gtk::SpinButton>(builder, "offset-distance"))
    , _miter_limit(get_widget<Gtk::SpinButton>(builder, "offset-miter-limit"))
    , _outward(get_widget<Gtk::ToggleButton>(builder, "offset-outward"))
    , _inward(get_widget<Gtk::ToggleButton>(builder, "offset-inward"))
    , _both(get_widget<Gtk::ToggleButton>(builder, "offset-both"))
    , _round(get_widget<Gtk::ToggleButton>(builder, "offset-round"))
    , _bevel(get_widget<Gtk::ToggleButton>(builder, "offset-bevel"))
    , _miter(get_widget<Gtk::ToggleButton>(builder, "offset-miter"))
    , _outer_only(get_widget<Gtk::CheckButton>(builder, "offset-outer-only"))
    , _select_results(get_widget<Gtk::CheckButton>(builder, "offset-select-results"))
    , _delete_originals(get_widget<Gtk::CheckButton>(builder, "offset-delete-originals"))
    , _simplify_results(get_widget<Gtk::CheckButton>(builder, "offset-simplify-results"))
    , _cancel(get_widget<Gtk::Button>(builder, "offset-cancel"))
    , _apply(get_widget<Gtk::Button>(builder, "offset-apply"))
{
    auto *unit_menu = _tracker->create_unit_dropdown();
    get_widget<Gtk::Box>(builder, "offset-unit-box").append(*unit_menu);
    _tracker->addAdjustment(_distance.get_adjustment()->gobj());
    unit_menu->signal_changed().connect(sigc::mem_fun(*this, &OffsetShapesToolbar::unit_changed));

    _distance.get_adjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &OffsetShapesToolbar::options_changed));
    _miter_limit.get_adjustment()->signal_value_changed().connect(
        sigc::mem_fun(*this, &OffsetShapesToolbar::options_changed));
    for (auto *button : {&_outward, &_inward, &_both, &_round, &_bevel, &_miter}) {
        button->signal_toggled().connect([this, button] {
            if (button->get_active()) options_changed();
        });
    }
    for (auto *button : {&_outer_only, &_select_results, &_delete_originals, &_simplify_results}) {
        button->signal_toggled().connect(sigc::mem_fun(*this, &OffsetShapesToolbar::options_changed));
    }
    _cancel.signal_clicked().connect([this] {
        if (auto *tool = get_offset_tool(_desktop)) tool->cancel();
    });
    _apply.signal_clicked().connect([this] {
        if (auto *tool = get_offset_tool(_desktop)) tool->apply();
    });

    _initMenuBtns();
}

OffsetShapesToolbar::~OffsetShapesToolbar() = default;

void OffsetShapesToolbar::setDesktop(SPDesktop *desktop)
{
    _preview_changed.disconnect();
    Toolbar::setDesktop(desktop);
    if (_desktop) {
        if (auto *tool = get_offset_tool(_desktop)) {
            _preview_changed = tool->signal_preview_changed().connect(
                [this](bool available) { _apply.set_sensitive(available); });
        }
        sync_from_preferences();
    }
}

void OffsetShapesToolbar::sync_from_preferences()
{
    _updating = true;
    auto *prefs = Preferences::get();
    auto const *default_unit = _desktop->getNamedView()->getDisplayUnit();
    auto const unit = prefs->getString("/tools/offsetshapes/unit", default_unit->abbr);
    _tracker->setActiveUnitByAbbr(unit.c_str());
    auto const *active_unit = _tracker->getActiveUnit();
    _distance.get_adjustment()->set_value(Util::Quantity::convert(
        std::max(0.001, prefs->getDouble("/tools/offsetshapes/distance",
                                         prefs->getDouble("/options/defaultoffsetwidth/value", 3.7795275591))),
        "px", active_unit));
    _miter_limit.get_adjustment()->set_value(
        std::max(1.0, prefs->getDouble("/tools/offsetshapes/miter_limit", 4.0)));

    auto const direction = std::clamp(prefs->getInt("/tools/offsetshapes/direction", 0), 0, 2);
    (direction == 0 ? _outward : direction == 1 ? _inward : _both).set_active(true);
    auto const corner = std::clamp(prefs->getInt("/tools/offsetshapes/corner", 2), 0, 2);
    (corner == 0 ? _round : corner == 1 ? _bevel : _miter).set_active(true);
    _outer_only.set_active(prefs->getBool("/tools/offsetshapes/outer_shapes_only", false));
    _select_results.set_active(prefs->getBool("/tools/offsetshapes/select_results", true));
    _delete_originals.set_active(prefs->getBool("/tools/offsetshapes/delete_originals", false));
    _simplify_results.set_active(prefs->getBool("/tools/offsetshapes/simplify_results", false));
    _miter_limit.set_sensitive(corner == 2);
    _updating = false;
    options_changed();
}

void OffsetShapesToolbar::unit_changed()
{
    if (_updating) return;
    Preferences::get()->setString("/tools/offsetshapes/unit", _tracker->getActiveUnit()->abbr);
    options_changed();
}

void OffsetShapesToolbar::options_changed()
{
    if (_updating || !_tracker->getActiveUnit()) return;

    OffsetShapes::Options options;
    options.distance_px = Util::Quantity::convert(_distance.get_adjustment()->get_value(),
                                                   _tracker->getActiveUnit(), "px");
    options.direction = _outward.get_active() ? OffsetShapes::Direction::Outward
                        : _inward.get_active() ? OffsetShapes::Direction::Inward
                                              : OffsetShapes::Direction::Both;
    options.corner = _round.get_active() ? OffsetShapes::Corner::Round
                     : _bevel.get_active() ? OffsetShapes::Corner::Bevel
                                           : OffsetShapes::Corner::Miter;
    options.miter_limit = _miter_limit.get_adjustment()->get_value();
    options.outer_shapes_only = _outer_only.get_active();
    options.select_results = _select_results.get_active();
    options.delete_originals = _delete_originals.get_active();
    options.simplify_results = _simplify_results.get_active();

    auto *prefs = Preferences::get();
    prefs->setDouble("/tools/offsetshapes/distance", options.distance_px);
    prefs->setInt("/tools/offsetshapes/direction", static_cast<int>(options.direction));
    prefs->setInt("/tools/offsetshapes/corner", static_cast<int>(options.corner));
    prefs->setDouble("/tools/offsetshapes/miter_limit", options.miter_limit);
    prefs->setBool("/tools/offsetshapes/outer_shapes_only", options.outer_shapes_only);
    prefs->setBool("/tools/offsetshapes/select_results", options.select_results);
    prefs->setBool("/tools/offsetshapes/delete_originals", options.delete_originals);
    prefs->setBool("/tools/offsetshapes/simplify_results", options.simplify_results);
    _miter_limit.set_sensitive(options.corner == OffsetShapes::Corner::Miter);

    if (auto *tool = get_offset_tool(_desktop)) {
        tool->set_options(options);
        _apply.set_sensitive(tool->has_preview());
    }
}

void OffsetShapesToolbar::focus_canvas()
{
    if (_desktop && _desktop->getCanvas()) {
        _desktop->getCanvas()->grab_focus();
    }
}

} // namespace Inkscape::UI::Toolbar
