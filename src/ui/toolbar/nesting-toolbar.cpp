// SPDX-License-Identifier: GPL-2.0-or-later

#include "nesting-toolbar.h"

#include <algorithm>
#include <cmath>

#include <glibmm/i18n.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>

#include "desktop.h"
#include "nesting/nesting-settings.h"
#include "nesting/nesting-types.h"
#include "object/sp-item.h"
#include "ui/builder-utils.h"
#include "ui/tools/nesting-tool.h"
#include "ui/widget/canvas.h"
#include "ui/widget/unit-tracker.h"
#include "util/units.h"

namespace Inkscape::UI::Toolbar {
namespace {

Tools::NestingTool *get_nesting_tool(SPDesktop *desktop)
{
    return desktop ? dynamic_cast<Tools::NestingTool *>(desktop->getTool()) : nullptr;
}

Gtk::DropDown *make_dropdown(std::vector<Glib::ustring> const &labels, Glib::ustring const &tooltip)
{
    auto list = Gtk::StringList::create(labels);
    auto *dropdown = Gtk::make_managed<Gtk::DropDown>(list);
    dropdown->set_tooltip_text(tooltip);
    return dropdown;
}

// Rotation choices in RotationMode order: None, RightAngles, Discrete, Free.
constexpr int ROTATION_CHOICES = 4;

// Time choices in OptimizationTimePreset order, as on the Preferences page.
constexpr int TIME_CHOICES = 6;

} // namespace

NestingToolbar::NestingToolbar()
    : NestingToolbar{create_builder("toolbar-nesting.ui")}
{}

NestingToolbar::NestingToolbar(Glib::RefPtr<Gtk::Builder> const &builder)
    : Toolbar{get_widget<Gtk::Box>(builder, "nesting-toolbar")}
    , _tracker(std::make_unique<UI::Widget::UnitTracker>(Util::UNIT_TYPE_LINEAR))
    , _settings(get_widget<Gtk::Box>(builder, "nesting-settings"))
    , _spacing(get_widget<Gtk::SpinButton>(builder, "nesting-spacing"))
    , _margin(get_widget<Gtk::SpinButton>(builder, "nesting-margin"))
    , _step(get_widget<Gtk::SpinButton>(builder, "nesting-step"))
    , _custom_time(get_widget<Gtk::SpinButton>(builder, "nesting-custom-time"))
    , _show_labels(get_widget<Gtk::CheckButton>(builder, "nesting-show-labels"))
    , _nest(get_widget<Gtk::Button>(builder, "nesting-nest"))
    , _sheet(get_widget<Gtk::Label>(builder, "nesting-sheet"))
{
    // One unit menu for both lengths; each is stored with its unit.
    auto *unit_menu = _tracker->create_unit_dropdown();
    get_widget<Gtk::Box>(builder, "nesting-unit-box").append(*unit_menu);
    _tracker->addAdjustment(_spacing.get_adjustment()->gobj());
    _tracker->addAdjustment(_margin.get_adjustment()->gobj());
    unit_menu->signal_changed().connect([this] {
        if (!_updating) write_lengths();
    });

    _rotation = make_dropdown({_("None"), _("90°"), _("Step"), _("Free")},
                              _("Which rotations the nesting engine may use"));
    auto &rotation_box = get_widget<Gtk::Box>(builder, "nesting-rotation-box");
    rotation_box.insert_child_after(*_rotation, *rotation_box.get_first_child());

    _time = make_dropdown({_("Quick (1 second)"), _("Balanced (5 seconds)"), _("Refined (15 seconds)"),
                           _("Maximum (60 seconds)"), _("Custom"), _("Unlimited")},
                          _("The nesting engine keeps refining until this time limit is reached"));
    auto &time_box = get_widget<Gtk::Box>(builder, "nesting-time-box");
    time_box.insert_child_after(*_time, *time_box.get_first_child());

    // While the unit menu converts the adjustments one by one, the values mix
    // units; the unit menu's own signal writes both once it is done.
    _spacing.get_adjustment()->signal_value_changed().connect([this] {
        if (!_updating && !_tracker->isUpdating()) write_lengths();
    });
    _margin.get_adjustment()->signal_value_changed().connect([this] {
        if (!_updating && !_tracker->isUpdating()) write_lengths();
    });
    _rotation->property_selected().signal_changed().connect([this] {
        if (!_updating) write_rotation();
    });
    _step.get_adjustment()->signal_value_changed().connect([this] {
        if (!_updating) write_rotation();
    });
    _time->property_selected().signal_changed().connect([this] {
        if (!_updating) write_time();
    });
    _custom_time.get_adjustment()->signal_value_changed().connect([this] {
        if (!_updating) write_time();
    });
    _show_labels.signal_toggled().connect([this] {
        if (!_updating) Preferences::get()->setBool(Nesting::SHOW_LABELS_PREF_PATH, _show_labels.get_active());
    });
    // C6: the Nest button acts like Enter.
    _nest.signal_clicked().connect([this] {
        if (auto *tool = get_nesting_tool(_desktop)) {
            tool->nest_last_sheet(Tools::NestMode::Add);
        }
        // Keys (Esc during the run) belong to the canvas, not the button.
        if (_desktop && _desktop->getCanvas()) {
            _desktop->getCanvas()->grab_focus();
        }
    });

    // Follow changes made on the Preferences page (and anywhere else).
    auto *prefs = Preferences::get();
    for (auto const *path : {Nesting::PART_SPACING_PREF_PATH, Nesting::CONTAINER_MARGIN_PREF_PATH,
                             Nesting::ROTATION_MODE_PREF_PATH, Nesting::ROTATION_STEP_PREF_PATH,
                             Nesting::TIME_PRESET_PREF_PATH, Nesting::CUSTOM_TIME_PREF_PATH,
                             Nesting::SHOW_LABELS_PREF_PATH}) {
        _observers.push_back(prefs->createObserver(path, [this] {
            if (!_updating) sync_from_preferences();
        }));
    }

    _initMenuBtns();
    sync_from_preferences();
}

NestingToolbar::~NestingToolbar() = default;

void NestingToolbar::setDesktop(SPDesktop *desktop)
{
    _tool_changed.disconnect();
    _tool_state.disconnect();
    Toolbar::setDesktop(desktop);
    if (_desktop) {
        // The toolbar outlives each NestingTool instance: follow the tool.
        _tool_changed = _desktop->connectEventContextChanged([this](SPDesktop *, Tools::ToolBase *) {
            update_tool_state();
        });
    }
    update_tool_state();
}

void NestingToolbar::sync_from_preferences()
{
    auto *prefs = Preferences::get();
    Nesting::migrateOptimizationTimePreset(*prefs);
    _updating = true;

    auto unit = prefs->getUnit(Nesting::PART_SPACING_PREF_PATH);
    if (unit.empty()) {
        unit = "mm";
    }
    _tracker->setActiveUnitByAbbr(unit.c_str());
    auto const *active_unit = _tracker->getActiveUnit();
    _spacing.get_adjustment()->set_value(Util::Quantity::convert(
        Nesting::readLengthPreferencePx(*prefs, Nesting::PART_SPACING_PREF_PATH), "px", active_unit));
    _margin.get_adjustment()->set_value(Util::Quantity::convert(
        Nesting::readLengthPreferencePx(*prefs, Nesting::CONTAINER_MARGIN_PREF_PATH), "px", active_unit));

    auto const rotation = std::clamp(
        prefs->getInt(Nesting::ROTATION_MODE_PREF_PATH, static_cast<int>(Nesting::RotationMode::Free)), 0,
        ROTATION_CHOICES - 1);
    _rotation->set_selected(rotation);
    _step.get_adjustment()->set_value(std::clamp(prefs->getDouble(Nesting::ROTATION_STEP_PREF_PATH, 15.0), 0.1, 360.0));
    _step.set_sensitive(rotation == static_cast<int>(Nesting::RotationMode::Discrete));

    auto const preset = static_cast<int>(Nesting::optimizationTimePresetFromInt(prefs->getInt(
        Nesting::TIME_PRESET_PREF_PATH, static_cast<int>(Nesting::OptimizationTimePreset::Balanced))));
    _time->set_selected(std::clamp(preset, 0, TIME_CHOICES - 1));
    _custom_time.get_adjustment()->set_value(std::clamp(prefs->getDouble(Nesting::CUSTOM_TIME_PREF_PATH, 30.0), 0.1, 600.0));
    _custom_time.set_visible(preset == static_cast<int>(Nesting::OptimizationTimePreset::Custom));

    _show_labels.set_active(prefs->getBool(Nesting::SHOW_LABELS_PREF_PATH, true));
    _updating = false;
}

void NestingToolbar::write_lengths()
{
    auto const *unit = _tracker->getActiveUnit();
    if (!unit) {
        return;
    }
    _updating = true;
    auto *prefs = Preferences::get();
    prefs->setDoubleUnit(Nesting::PART_SPACING_PREF_PATH, _spacing.get_adjustment()->get_value(), unit->abbr);
    prefs->setDoubleUnit(Nesting::CONTAINER_MARGIN_PREF_PATH, _margin.get_adjustment()->get_value(), unit->abbr);
    _updating = false;
}

void NestingToolbar::write_rotation()
{
    _updating = true;
    auto *prefs = Preferences::get();
    auto const rotation = static_cast<int>(_rotation->get_selected());
    prefs->setInt(Nesting::ROTATION_MODE_PREF_PATH, rotation);
    prefs->setDouble(Nesting::ROTATION_STEP_PREF_PATH, _step.get_adjustment()->get_value());
    _step.set_sensitive(rotation == static_cast<int>(Nesting::RotationMode::Discrete));
    _updating = false;
}

void NestingToolbar::write_time()
{
    _updating = true;
    auto *prefs = Preferences::get();
    auto const preset = static_cast<int>(_time->get_selected());
    prefs->setInt(Nesting::TIME_PRESET_PREF_PATH, preset);
    prefs->setDouble(Nesting::CUSTOM_TIME_PREF_PATH, _custom_time.get_adjustment()->get_value());
    Nesting::syncOptimizationTimeLimit(*prefs);
    _custom_time.set_visible(preset == static_cast<int>(Nesting::OptimizationTimePreset::Custom));
    _updating = false;
}

void NestingToolbar::update_tool_state()
{
    _tool_state.disconnect();
    auto *tool = get_nesting_tool(_desktop);
    if (tool) {
        _tool_state = tool->connect_state_changed([this] { update_tool_state(); });
    }
    auto const solving = tool && tool->is_solving();
    auto *sheet = tool ? tool->last_sheet() : nullptr;
    // Insensitive while a run is in progress; Nest needs a remembered sheet.
    _settings.set_sensitive(!solving);
    _nest.set_sensitive(tool && !solving && sheet);
    if (sheet) {
        auto const *label = sheet->defaultLabel();
        _sheet.set_text(Glib::ustring::compose(_("Sheet: %1"), label ? label : ""));
    } else {
#ifdef __APPLE__
        _sheet.set_text(_("Sheet: none (Cmd-click one)"));
#else
        _sheet.set_text(_("Sheet: none (Ctrl-click one)"));
#endif
    }
}

} // namespace Inkscape::UI::Toolbar
