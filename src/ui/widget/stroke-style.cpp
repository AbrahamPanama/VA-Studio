// SPDX-License-Identifier: GPL-2.0-or-later
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Bryce Harrington <brycehar@bryceharrington.org>
 *   bulia byak <buliabyak@users.sf.net>
 *   Maximilian Albert <maximilian.albert@gmail.com>
 *   Josh Andler <scislac@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2001-2005 authors
 * Copyright (C) 2001 Ximian, Inc.
 * Copyright (C) 2004 John Cliff
 * Copyright (C) 2008 Maximilian Albert (gtkmm-ification)
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "stroke-style.h"

#include <cmath>
#include <unordered_set>
#include "attributes.h"
#include "object/sp-string.h"
#include "object/sp-use.h"
#include "object/sp-root.h"
#include "xml/repr.h"
#include "xml/sp-css-attr.h"
#include "xml/attribute-record.h"

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <gtk/gtk.h>
#include <gtkmm/accelerator.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/grid.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/window.h>
#include <gdkmm/display.h>

#include "actions/actions-tools.h"
#include "dash-selector.h"
#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "fill-or-stroke.h"
#include "inkscape.h"
#include "object/sp-namedview.h"
#include "object/sp-text.h"
#include "selection.h"
#include "style.h"
#include "svg/css-ostringstream.h"
#include "ui/dialog-events.h"
#include "ui/dialog/dialog-base.h"
#include "ui/icon-loader.h"
#include "ui/icon-names.h"
#include "ui/pack.h"
#include "ui/stroke-width-command.h"
#include "ui/stroke-width-controller.h"
#include "ui/tools/marker-tool.h"
#include "ui/tools/text-tool.h"
#include "ui/util.h"
#include "ui/widget/canvas.h"
#include "ui/widget/generic/popover-menu.h"
#include "ui/widget/style/marker-combo-box.h"
#include "ui/widget/unit-menu.h"
#include "widgets/style-utils.h"
#include "text-editing.h"

using Inkscape::DocumentUndo;

/**
 * Extract the actual name of the link
 * e.g. get mTriangle from url(#mTriangle).
 * \return Buffer containing the actual name, allocated from GLib;
 * the caller should free the buffer when they no longer need it.
 */
SPObject* getMarkerObj(gchar const *n, SPDocument *doc)
{
    gchar const *p = n;
    while (*p != '\0' && *p != '#') {
        p++;
    }

    if (*p == '\0' || p[1] == '\0') {
        return nullptr;
    }

    p++;
    int c = 0;
    while (p[c] != '\0' && p[c] != ')') {
        c++;
    }

    if (p[c] == '\0') {
        return nullptr;
    }

    gchar* b = g_strdup(p);
    b[c] = '\0';

    // FIXME: get the document from the object and let the caller pass it in
    SPObject *marker = doc->getObjectById(b);

    g_free(b);
    return marker;
}

/**
 * Get a dash array and offset from the style.
 *
 * Both values are de-scaled by the style's width if needed.
 */
std::pair<std::vector<double>, double> getDashFromStyle(SPStyle *style) {
    auto prefs = Inkscape::Preferences::get();

    std::vector<double> ret;
    size_t len = style->stroke_dasharray.values.size();

    double scaledash = 1.0;
    if (prefs->getBool("/options/dash/scale", true) && style->stroke_width.computed) {
        scaledash = style->stroke_width.computed;
    }

    double offset = style->stroke_dashoffset.value / scaledash;
    for (unsigned i = 0; i < len; i++) {
        ret.push_back(style->stroke_dasharray.values[i].value / scaledash);
    }

    return {std::move(ret), offset};
}

namespace Inkscape::UI::Widget {

/**
 * Creates a label widget with the given text, at the given col, row
 * position in the table.
 */
static Gtk::Label *spw_label(Gtk::Grid *table, const gchar *label_text, int col, int row,
                             Gtk::Widget* target)
{
  auto const label_widget = Gtk::make_managed<Gtk::Label>();
  g_assert(label_widget != nullptr);
  if (target != nullptr) {
    label_widget->set_text_with_mnemonic(label_text);
    label_widget->set_mnemonic_widget(*target);
  } else {
    label_widget->set_text(label_text);
  }
  label_widget->set_visible(true);

  label_widget->set_halign(Gtk::Align::START);
  label_widget->set_valign(Gtk::Align::CENTER);
  label_widget->set_margin_start(4);
  label_widget->set_margin_end(4);

  table->attach(*label_widget, col, row, 1, 1);

  return label_widget;
}

/**
 * Creates a horizontal layout manager with 4-pixel spacing between children
 * and space for 'width' columns.
 */
static Gtk::Box *spw_hbox(Gtk::Grid *table, int width, int col, int row)
{
    auto const hb = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    g_assert(hb != nullptr);
    hb->set_visible(true);
    hb->set_hexpand();
    hb->set_halign(Gtk::Align::FILL);
    hb->set_valign(Gtk::Align::CENTER);
    hb->add_css_class("linked");
    table->attach(*hb, col, row, width, 1);
    return hb;
}

/**
 * Construct a stroke-style radio button with a given icon
 *
 * \param[in] grp          The Gtk::CheckButton with which to group
 * \param[in] icon         The icon to use for the button
 * \param[in] button_type  The type of stroke-style radio button (join/cap)
 * \param[in] stroke_style The style attribute to associate with the button
 */
StrokeStyle::StrokeStyleButton::StrokeStyleButton(Gtk::ToggleButton    *&grp,
                                                  char const            *icon,
                                                  StrokeStyleButtonType  button_type,
                                                  gchar const           *stroke_style)
    : 
        button_type(button_type),
        stroke_style(stroke_style)
{
    if (!grp) {
        grp = this;
    } else {
        set_group(*grp);
    }
    set_visible(true);

    auto px = Gtk::manage(sp_get_icon_image(icon, Gtk::IconSize::NORMAL));
    g_assert(px != nullptr);
    px->set_visible(true);
    set_child(*px);
}

std::vector<double> parse_dash_pattern(const Glib::ustring& input) {
    std::vector<double> output;
    if (input.empty()) return output;

    std::istringstream stream(input.c_str());
    while (stream) {
        double val;
        stream >> val;
        if (stream) {
            output.push_back(val);
        }
    }

    return output;
}

namespace {

/// Explicit accessible name (the icon-only buttons have no text).
void set_accessible_label(Gtk::Widget &widget, char const *label)
{
    gtk_accessible_update_property(GTK_ACCESSIBLE(widget.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

/// A key held on a focused button activates it once: repeats are swallowed until release.
void guard_held_activation(Gtk::Widget &button)
{
    auto held = std::make_shared<std::set<unsigned>>();
    auto const activating = [](unsigned keyval) {
        return keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter || keyval == GDK_KEY_ISO_Enter ||
               keyval == GDK_KEY_space || keyval == GDK_KEY_KP_Space;
    };
    auto key = Gtk::EventControllerKey::create();
    key->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    key->signal_key_pressed().connect([held, activating](unsigned keyval, unsigned, Gdk::ModifierType) {
        if (!activating(keyval)) return false;
        return !held->insert(keyval).second; // a repeat while held is consumed
    }, false);
    key->signal_key_released().connect([held](unsigned keyval, unsigned, Gdk::ModifierType) { held->erase(keyval); });
    button.add_controller(key);
    auto focus = Gtk::EventControllerFocus::create();
    focus->signal_leave().connect([held] { held->clear(); });
    button.add_controller(focus);
}

/// Locale-independent shortest text of a step ("0.05", "0.00005").
std::string plain_number(double value)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buffer, sizeof buffer, "%.8f", value);
    std::string text = buffer;
    if (text.find('.') != std::string::npos) {
        while (text.back() == '0') text.pop_back();
        if (text.back() == '.') text.pop_back();
    }
    return text;
}

constexpr double max_width_px = 1e6; ///< typed widths above this are rejected

/// Compact icon buttons of the width row: as tall as the field, about 24 px wide.
void install_row_css()
{
    static Glib::RefPtr<Gtk::CssProvider> provider;
    if (provider) return;
    provider = Gtk::CssProvider::create();
    provider->load_from_data(R"=====(
#StrokeWidth-Decrease, #StrokeWidth-Increase, menubutton#StrokeWidth-Presets > button {
    min-height: 22px; min-width: 18px; padding: 0 3px; margin: 0;
}
)=====");
    Gtk::StyleContext::add_provider_for_display(Gdk::Display::get_default(), provider,
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

} // namespace

StrokeStyle::StrokeStyle() :
    Gtk::Box(),
    miterLimitSpin(),
    widthSpin(),
    unitSelector(),
    joinMiter(),
    joinRound(),
    joinBevel(),
    capButt(),
    capRound(),
    capSquare(),
    dashSelector(),
    update(false),
    desktop(nullptr),
    startMarkerConn(),
    midMarkerConn(),
    endMarkerConn(),
    _old_unit(nullptr)
{
    set_name("StrokeSelector");

    table = Gtk::make_managed<Gtk::Grid>();
    table->set_margin(4);
    table->set_row_spacing(4);
    table->set_hexpand(true);
    table->set_halign(Gtk::Align::FILL);
    table->set_visible(true);
    append(*table);

    Gtk::Box *hb;
    gint i = 0;

    //spw_label(t, C_("Stroke width", "_Width:"), 0, i);

    hb = spw_hbox(table, 3, 1, i);

// TODO: when this is gtkmmified, use a ScalarUnit instead of the separate
// spinbutton and unit selector for stroke width. In sp_stroke_style_line_update, use
// setHundredPercent to remember the averaged width corresponding to 100%. Then the
// stroke_width_set_unit will be removed (because ScalarUnit takes care of conversions itself)
    widthAdj = Gtk::Adjustment::create(1.0, 0.0, 1000.0, 0.1, 10.0, 0.0);
    widthSpin = Gtk::make_managed<SpinButton>(widthAdj, 0.1, 3);
    widthSpin->set_name("StrokeWidth-Field");
    widthSpin->set_mixed_stepping_disabled();
    // The row owns stepping: no value drag, wheel or hover arrows, no keyboard
    // stepping inside the spin button (the key controller below does it, so Page
    // keys and modified arrows reach the panel), and no field-local Undo (Ctrl/Cmd+Z
    // goes to the document). Only a changed, valid text commits.
    widthSpin->set_drag_sensitivity(0);
    widthSpin->set_has_arrows(false);
    widthSpin->set_commit_only_if_changed(true);
    widthSpin->set_input_validation(true);
    widthSpin->set_input_maximum(max_width_px);
    widthSpin->set_key_stepping(false);
    widthSpin->set_field_undo(false);
    widthSpin->set_min_size("999.999"); // the row does not jump between units
    widthSpin->set_tooltip_text(_("Stroke width"));
    widthSpin->set_visible(true);
    _width_entry = dynamic_cast<Gtk::Entry *>(find_widget_by_name(*widthSpin, "InkSpinButton-Entry", false));
    spw_label(table, C_("Stroke width", "_Width"), 0, i, widthSpin);

    sp_dialog_defocus_on_enter(*widthSpin);

    install_row_css();
    auto const make_step_button = [&](char const *icon, char const *name, char const *label) {
        auto const button = Gtk::make_managed<Gtk::Button>();
        button->set_icon_name(icon);
        button->set_name(name);
        button->set_focus_on_click(false); // the canvas or the text caret keeps the keyboard focus
        button->set_size_request(24, -1);
        button->set_valign(Gtk::Align::FILL);
        button->set_visible(true);
        set_accessible_label(*button, label);
        guard_held_activation(*button);
        return button;
    };
    _width_dec = make_step_button("list-remove-symbolic", "StrokeWidth-Decrease", _("Decrease stroke width"));
    _width_inc = make_step_button("list-add-symbolic", "StrokeWidth-Increase", _("Increase stroke width"));
    _width_presets = Gtk::make_managed<Gtk::MenuButton>();
    _width_presets->set_icon_name("pan-down-symbolic");
    _width_presets->set_name("StrokeWidth-Presets");
    _width_presets->set_focus_on_click(false);
    _width_presets->set_size_request(24, -1);
    _width_presets->set_valign(Gtk::Align::FILL);
    _width_presets->set_visible(true);
    set_accessible_label(*_width_presets, _("Preset stroke widths"));
    guard_held_activation(*_width_presets);
    _preset_popover = Gtk::make_managed<PopoverMenu>(Gtk::PositionType::BOTTOM);
    _preset_popover->set_name("StrokeWidth-PresetList");
    _width_presets->set_popover(*_preset_popover);

    // Tab order: - field v + unit.
    UI::pack_start(*hb, *_width_dec, false, false);
    UI::pack_start(*hb, *widthSpin, true, true);
    UI::pack_start(*hb, *_width_presets, false, false);
    UI::pack_start(*hb, *_width_inc, false, false);
    unitSelector = Gtk::make_managed<UnitMenu>();
    unitSelector->setUnitType(Inkscape::Util::UNIT_TYPE_LINEAR);
    SPDesktop *desktop = SP_ACTIVE_DESKTOP;

    unitSelector->addUnit(*Util::UnitTable::get().getUnit("%"));
    _hairline_item = unitSelector->append(_("Hairline"));
    _old_unit = unitSelector->getUnit();
    if (desktop) {
        unitSelector->setUnit(desktop->getNamedView()->display_units->abbr);
        _old_unit = desktop->getNamedView()->display_units;
    }
    _last_linear_unit = _old_unit && _old_unit->type == Inkscape::Util::UNIT_TYPE_LINEAR
                            ? _old_unit
                            : Util::UnitTable::get().getUnit("px");
    widthSpin->setUnitMenu(unitSelector);
    unitSelector->signal_changed().connect(sigc::mem_fun(*this, &StrokeStyle::unitChangedCB));
    unitSelector->set_visible(true);

    UI::pack_start(*hb, *unitSelector, FALSE, FALSE);
    // InkSpinButton emits its own change signal when a user resolves Mixed to
    // the adjustment's existing hidden number. GtkAdjustment does not emit in
    // that case, so listening to it would silently drop the first edit.
    // Every callback that a child can still deliver after this row is gone (a
    // retained list item, a late destroy) is tracked to the row: sigc drops it
    // when the row dies, so a dead receiver is never called.
    auto const tracked = [this](auto &&functor) {
        return sigc::track_object(std::forward<decltype(functor)>(functor), *this);
    };
    widthSpin->signal_value_changed().connect(tracked([this] { setStrokeWidth(true); }));
    widthSpin->signal_invalid_text().connect(sigc::mem_fun(*this, &StrokeStyle::onInvalidText));

    _width_dec->signal_clicked().connect(tracked([this] { onStep(-1); }));
    _width_inc->signal_clicked().connect(tracked([this] { onStep(+1); }));
    // Where the keyboard was before a pointer press moved it into the list.
    auto const press = Gtk::GestureClick::create();
    press->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    press->signal_pressed().connect(tracked([this](int, double, double) {
        rememberFocus();
        _press_recorded = true;
    }));
    _width_presets->add_controller(press);
    // Rebuild before the popover's own show handler sizes its items.
    _preset_popover->signal_show().connect(tracked([this] {
        if (!_press_recorded) rememberFocus(); // opened from the keyboard: the button itself
        _press_recorded = false;
        rebuildPresetList();
    }), false);
    _preset_popover->signal_closed().connect(tracked([this] { restoreFocus(); }));

    auto const field_key = Gtk::EventControllerKey::create();
    field_key->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    field_key->signal_key_pressed().connect(
        tracked([this, &key = *field_key](unsigned keyval, unsigned keycode, Gdk::ModifierType state) {
            return onFieldKey(key, keyval, keycode, state);
        }), false);
    field_key->signal_key_released().connect(
        tracked([this](unsigned keyval, unsigned, Gdk::ModifierType) { _held_keys.erase(keyval); }));
    widthSpin->add_controller(field_key);
    auto const field_focus = Gtk::EventControllerFocus::create();
    field_focus->signal_leave().connect(tracked([this] { _held_keys.clear(); }));
    widthSpin->add_controller(field_focus);

    // The children die before this object does: never touch a dead one from a late handler.
    auto const forget_when_destroyed = [&tracked](auto *&pointer) {
        pointer->signal_destroy().connect(tracked([&pointer] { pointer = nullptr; }));
    };
    forget_when_destroyed(_width_entry);
    forget_when_destroyed(widthSpin);
    forget_when_destroyed(_width_dec);
    forget_when_destroyed(_width_inc);
    forget_when_destroyed(_width_presets);
    forget_when_destroyed(_preset_popover);

    // Hidden (page switch, closed window, collapsed dock): nothing pending survives.
    _unmap_conn = signal_unmap().connect(tracked([this] { resetRowScope(); }));

    i++;

    /* Dash */
    spw_label(table, _("Dashes"), 0, i, nullptr); //no mnemonic for now
                                            //decide what to do:
                                            //   implement a set_mnemonic_source function in the
                                            //   DashSelector class, so that we do not have to
                                            //   expose any of the underlying widgets?
    dashSelector = Gtk::make_managed<DashSelector>();
    dashSelector->changed_signal.connect([this](auto){
        if (update || _editing_dash_pattern) {
            return;
        }
        _editing_dash_pattern = true;
        auto& dash_pattern = dashSelector->get_dash_pattern();
        update_dash_entry(dash_pattern);
        setStrokeDash();
        _editing_dash_pattern = false;
    });
    table->attach(*dashSelector, 1, i, 3, 1);

    i++;

    _pattern_entry = Gtk::make_managed<Gtk::Entry>();
    _pattern_entry->signal_changed().connect([this](){
        if (update || _editing_dash_pattern) {
            return;
        }
        _editing_dash_pattern = true;
        update = true;
        auto pattern = parse_dash_pattern(_pattern_entry->get_text());
        dashSelector->set_dash_pattern(pattern, dashSelector->get_offset());
        update = false;
        setStrokeDash();
        _editing_dash_pattern = false;
    });
    table->attach(*_pattern_entry, 1, i, 4, 1);

    _pattern_label = spw_label(table, _("_Pattern"), 0, i, _pattern_entry);
    _pattern_label->set_tooltip_text(_("Repeating \"dash gap ...\" pattern"));

    i++;

    /* Drop down marker selectors*/
    // TRANSLATORS: Path markers are an SVG feature that allows you to attach arbitrary shapes
    // (arrowheads, bullets, faces, whatever) to the start, end, or middle nodes of a path.

    spw_label(table, _("Markers"), 0, i, nullptr);

    hb = spw_hbox(table, 3, 1, i);
    i++;

    startMarkerCombo = Gtk::make_managed<MarkerComboBox>("marker-start", SP_MARKER_LOC_START);
    startMarkerCombo->set_flat(true);
    startMarkerCombo->set_tooltip_text(_("Start Markers are drawn on the first node of a path or shape"));
    startMarkerConn = startMarkerCombo->connect_changed([this]{ markerSelectCB(startMarkerCombo, SP_MARKER_LOC_START); });
    startMarkerCombo->connect_edit([this]{ enterEditMarkerMode(SP_MARKER_LOC_START); });
    startMarkerCombo->set_visible(true);

    UI::pack_start(*hb, *startMarkerCombo, true, true);

    midMarkerCombo = Gtk::make_managed<MarkerComboBox>("marker-mid", SP_MARKER_LOC_MID);
    midMarkerCombo->set_flat(true);
    midMarkerCombo->set_tooltip_text(_("Mid Markers are drawn on every node of a path or shape except the first and last nodes"));
    midMarkerConn = midMarkerCombo->connect_changed([this]{ markerSelectCB(midMarkerCombo, SP_MARKER_LOC_MID); });
    midMarkerCombo->connect_edit([this]{ enterEditMarkerMode(SP_MARKER_LOC_MID); });
    midMarkerCombo->set_visible(true);

    UI::pack_start(*hb, *midMarkerCombo, true, true);

    endMarkerCombo = Gtk::make_managed<MarkerComboBox>("marker-end", SP_MARKER_LOC_END);
    endMarkerCombo->set_flat(true);
    endMarkerCombo->set_tooltip_text(_("End Markers are drawn on the last node of a path or shape"));
    endMarkerConn = endMarkerCombo->connect_changed([this]{ markerSelectCB(endMarkerCombo, SP_MARKER_LOC_END); });
    endMarkerCombo->connect_edit([this]{ enterEditMarkerMode(SP_MARKER_LOC_END); });
    endMarkerCombo->set_visible(true);

    UI::pack_start(*hb, *endMarkerCombo, true, true);
    i++;

    /* Cap type */
    // TRANSLATORS: cap type specifies the shape for the ends of lines
    //spw_label(t, _("_Cap:"), 0, i);
    spw_label(table, _("Cap"), 0, i, nullptr);

    hb = spw_hbox(table, 3, 1, i);

    Gtk::ToggleButton *capGrp = nullptr;

    capButt = makeRadioButton(capGrp, INKSCAPE_ICON("stroke-cap-butt"),
                                hb, STROKE_STYLE_BUTTON_CAP, "butt");

    // TRANSLATORS: Butt cap: the line shape does not extend beyond the end point
    //  of the line; the ends of the line are square
    capButt->set_tooltip_text(_("Butt cap"));

    capRound = makeRadioButton(capGrp, INKSCAPE_ICON("stroke-cap-round"),
                                hb, STROKE_STYLE_BUTTON_CAP, "round");

    // TRANSLATORS: Round cap: the line shape extends beyond the end point of the
    //  line; the ends of the line are rounded
    capRound->set_tooltip_text(_("Round cap"));

    capSquare = makeRadioButton(capGrp, INKSCAPE_ICON("stroke-cap-square"),
                                hb, STROKE_STYLE_BUTTON_CAP, "square");

    // TRANSLATORS: Square cap: the line shape extends beyond the end point of the
    //  line; the ends of the line are square
    capSquare->set_tooltip_text(_("Square cap"));

    i++;

    /* Join type */
    // TRANSLATORS: The line join style specifies the shape to be used at the
    //  corners of paths. It can be "miter", "round" or "bevel".
    spw_label(table, _("Join"), 0, i, nullptr);

    hb = spw_hbox(table, 3, 1, i);

    Gtk::ToggleButton *joinGrp = nullptr;

    joinBevel = makeRadioButton(joinGrp, INKSCAPE_ICON("stroke-join-bevel"),
                                hb, STROKE_STYLE_BUTTON_JOIN, "bevel");

    // TRANSLATORS: Bevel join: joining lines with a blunted (flattened) corner.
    //  For an example, draw a triangle with a large stroke width and modify the
    //  "Join" option (in the Fill and Stroke dialog).
    joinBevel->set_tooltip_text(_("Bevel join"));

    joinRound = makeRadioButton(joinGrp, INKSCAPE_ICON("stroke-join-round"),
                                hb, STROKE_STYLE_BUTTON_JOIN, "round");

    // TRANSLATORS: Round join: joining lines with a rounded corner.
    //  For an example, draw a triangle with a large stroke width and modify the
    //  "Join" option (in the Fill and Stroke dialog).
    joinRound->set_tooltip_text(_("Round join"));

    joinMiter = makeRadioButton(joinGrp, INKSCAPE_ICON("stroke-join-miter"),
                                hb, STROKE_STYLE_BUTTON_JOIN, "miter");

    // TRANSLATORS: Miter join: joining lines with a sharp (pointed) corner.
    //  For an example, draw a triangle with a large stroke width and modify the
    //  "Join" option (in the Fill and Stroke dialog).
    joinMiter->set_tooltip_text(_("Miter join"));

    i++;

    _miter_hb = spw_hbox(table, 3, 1, i);

    /* Miterlimit  */
    // TRANSLATORS: Miter limit: only for "miter join", this limits the length
    //  of the sharp "spike" when the lines connect at too sharp an angle.
    // When two line segments meet at a sharp angle, a miter join results in a
    //  spike that extends well beyond the connection point. The purpose of the
    //  miter limit is to cut off such spikes (i.e. convert them into bevels)
    //  when they become too long.
    //spw_label(t, _("Miter _limit:"), 0, i);
    miterLimitAdj = Gtk::Adjustment::create(4.0, 0.0, 100000.0, 0.1, 10.0, 0.0);
    miterLimitSpin = Gtk::make_managed<SpinButton>(miterLimitAdj, 0.1, 2);
    miterLimitSpin->set_tooltip_text(_("Maximum length of the miter (in units of stroke width)"));
    miterLimitSpin->set_width_chars(6);
    miterLimitSpin->set_visible(true);
    sp_dialog_defocus_on_enter(*miterLimitSpin);

    UI::pack_start(*_miter_hb, *miterLimitSpin, true, true);
    miterLimitAdj->signal_value_changed().connect(sigc::mem_fun(*this, &StrokeStyle::setStrokeMiter));

    i++;
    // this is a hack to align label text with the first row in paint order selector
    auto& vbox = *Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    vbox.set_homogeneous();
    /* Paint order */
    // TRANSLATORS: Paint order determines the order the 'fill', 'stroke', and 'markers are painted.
    vbox.append(*Gtk::make_managed<Gtk::Label>(_("Order")));
    vbox.append(*Gtk::make_managed<Gtk::Box>());
    _align_label = Gtk::make_managed<Gtk::Box>();
    vbox.append(*_align_label);
    table->attach(vbox, 0, i);

    _paint_order = Gtk::make_managed<PaintOrderWidget>();
    paintOrderConn = _paint_order->signal_values_changed().connect(tracked([this]() {
        if (update || !this->desktop) return;
        // ScopedWidthUpdate is declared below the constructor; keep the same
        // set/restore lifetime here without moving code outside this handler.
        struct ScopedOrderUpdate {
            gboolean &flag;
            gboolean const saved;
            explicit ScopedOrderUpdate(gboolean &f) : flag(f), saved(f) { flag = true; }
            ~ScopedOrderUpdate() { flag = saved; }
        };
        ScopedOrderUpdate const updating(update);
        auto po = _paint_order->getValue();
        SPCSSAttr *css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "paint-order", po.get_value().c_str());
        sp_desktop_set_style(this->desktop, css);
        sp_repr_css_attr_unref(css);
        DocumentUndo::done(this->desktop->getDocument(), RC_("Undo", "Set paint order"),
                           INKSCAPE_ICON("dialog-fill-and-stroke"));
    }));
    table->attach(*_paint_order, 1, i, 3, 1);

    i++;
}

StrokeStyle::~StrokeStyle()
{
    _unmap_conn.disconnect();
    _focus_idle.disconnect();
    if (_focus_return) {
        g_object_remove_weak_pointer(G_OBJECT(_focus_return), reinterpret_cast<gpointer *>(&_focus_return));
        _focus_return = nullptr;
    }
    // The row's children are already gone when this runs (GObject finalize): the
    // list belongs to the MenuButton, which unparents it in its own dispose.
}

void StrokeStyle::setDesktop(SPDesktop *desktop)
{
    if (this->desktop != desktop) {

        if (this->desktop) {
            _document_replaced_connection.disconnect();
        }
        this->desktop = desktop;

        if (!desktop) {
            _handleDocumentReplaced(nullptr, nullptr);
            _row = {};
            refreshWidthRow();
            return;
        }

        _document_replaced_connection =
            desktop->connectDocumentReplaced(sigc::mem_fun(*this, &StrokeStyle::_handleDocumentReplaced));

        _handleDocumentReplaced(nullptr, desktop->getDocument());
        for (MarkerComboBox *combo : {startMarkerCombo, midMarkerCombo, endMarkerCombo}) {
            combo->setDesktop(desktop);
        }

        updateLine();
    }
}

void StrokeStyle::_handleDocumentReplaced(SPDesktop *, SPDocument *document)
{
    // A replaced document is a new scope identity even when the selection
    // callback does not fire; an in-flight numeric commit must not settle.
    ++_scope_generation;
    resetRowScope();
    for (MarkerComboBox *combo : { startMarkerCombo, midMarkerCombo, endMarkerCombo }) {
        combo->setDocument(document);
    }
}


/**
 * Helper function for creating stroke-style radio buttons.
 *
 * \param[in] grp           The Gtk::CheckButton with which to group
 * \param[in] icon          The icon for the button
 * \param[in] hb            The Gtk::Box container in which to add the button
 * \param[in] button_type   The type (join/cap) for the button
 * \param[in] stroke_style  The style attribute to associate with the button
 *
 * \details After instantiating the button, it is added to a container box and
 *          a handler for the toggle event is connected.
 */
StrokeStyle::StrokeStyleButton *
StrokeStyle::makeRadioButton(Gtk::ToggleButton    *&grp,
                             char const            *icon,
                             Gtk::Box              *hb,
                             StrokeStyleButtonType  button_type,
                             gchar const           *stroke_style)
{
    g_assert(icon != nullptr);
    g_assert(hb  != nullptr);

    auto const tb = Gtk::make_managed<StrokeStyleButton>(grp, icon, button_type, stroke_style);
    UI::pack_start(*hb, *tb, false, false);
    tb->signal_toggled().connect([this, tb]() { buttonToggledCB(tb); });
    tb->set_hexpand();
    tb->set_halign(Gtk::Align::FILL);
    return tb;
}

void StrokeStyle::enterEditMarkerMode(SPMarkerLoc _editMarkerMode)
{
    SPDesktop *desktop = this->desktop;

    if (desktop) {
        set_active_tool(desktop, "Marker");
        Inkscape::UI::Tools::MarkerTool *mt = dynamic_cast<Inkscape::UI::Tools::MarkerTool*>(desktop->getTool());

        if(mt) {
            mt->editMarkerMode = _editMarkerMode;
            mt->selection_changed(desktop->getSelection());
        }
    }
}


bool StrokeStyle::areMarkersBeingUpdated()
{
    return startMarkerCombo->in_update() || midMarkerCombo->in_update() || endMarkerCombo->in_update();
}

/**
 * Handles when user selects one of the markers from the marker combobox.
 * Gets the marker uri string and applies it to all selected
 * items in the current desktop.
 */
void StrokeStyle::markerSelectCB(MarkerComboBox *marker_combo, SPMarkerLoc const which)
{
    if (update || areMarkersBeingUpdated()) {
        return;
    }

    SPDocument *document = desktop->getDocument();
    if (!document) {
        return;
    }

    // Get marker ID; could be empty (to remove marker)
    std::string marker = marker_combo->get_active_marker_uri();

    update = true;

    SPCSSAttr *css = sp_repr_css_attr_new();
    gchar const *combo_id = marker_combo->get_id();
    sp_repr_css_set_property(css, combo_id, marker.c_str());

    for (auto item : desktop->getSelection()->items()) {
        if (!is<SPShape>(item)) {
            continue;
        }
        if (Inkscape::XML::Node* selrepr = item->getRepr()) {
            sp_repr_css_change_recursive(selrepr, css, "style");
        }

        item->requestModified(SP_OBJECT_MODIFIED_FLAG);
        item->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
        // perform update to make sure any previously referenced markers are released,
        // so they can be collected by DocumentUndo::done collect orphans
        document->ensureUpToDate();

        DocumentUndo::done(document, RC_("Undo", "Set markers"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }

    // edit marker mode - update
    if (auto mt = dynamic_cast<Inkscape::UI::Tools::MarkerTool*>(desktop->getTool())) {
        mt->editMarkerMode = which;
        mt->selection_changed(desktop->getSelection());
    }

    sp_repr_css_attr_unref(css);
    css = nullptr;

    update = false;
};

/**
 * Callback for when UnitMenu widget is modified.
 * Triggers update action.
 */
void StrokeStyle::unitChangedCB()
{
    Inkscape::Util::Unit const *new_unit = unitSelector->getUnit();

    if (_old_unit == new_unit)
        return;

    // A real unit transition: the preset list and any reported rejection belong
    // to the old unit. The last linear unit gives steps, digits and the list unit
    // while the menu is on % or Hairline.
    closePopovers();
    _last_invalid_text.reset();
    ++_scope_generation; // a command that captured the old unit must not commit
    if (!isHairlineSelected() && new_unit->type == Inkscape::Util::UNIT_TYPE_LINEAR) {
        _last_linear_unit = new_unit;
    }

    // Selecting Hairline is an explicit command. Retain the existing native
    // action for this narrow step; setStrokeWidth itself no-ops while an update
    // refresh is running.
    if (isHairlineSelected()) {
        _old_unit = new_unit;
        setStrokeWidth();
        refreshWidthRow(); // setStrokeWidth may have returned early on the update guard
        return;
    }

    // Every other unit transition (ordinary units and percent) is display-only.
    // Never write XML/Undo here, and never remove an existing
    // vector-effect/hairline style until the user commits an explicit number.
    bool const was_updating = update;
    update = true;

    Inkscape::Util::Unit const *old_unit = _old_unit;
    _old_unit = new_unit;

    if (old_unit && old_unit->type == Inkscape::Util::UNIT_TYPE_DIMENSIONLESS) {
        // Coming from percent (or from Hairline, whose selected unit is the
        // empty dimensionless unit): the displayed number has no absolute
        // meaning, so re-query and show the selection in the newly chosen unit.
        update = was_updating;
        updateLine();
        if (was_updating) refreshWidthRow(); // updateLine returned early: refresh from the cached query
        return;
    }

    // Bounds first, so the converted value is never clamped by the old range.
    refreshWidthField();
    if (!widthSpin->is_mixed()) {
        if (new_unit->type == Inkscape::Util::UNIT_TYPE_DIMENSIONLESS) {
            // Percent keeps the 100% display convention. Guarded so this
            // display change never reaches setStrokeWidth.
            widthAdj->set_value(100.0);
        } else {
            // Convert the displayed value to the new unit; remains display-only.
            widthAdj->set_value(
                Inkscape::Util::Quantity::convert(widthAdj->get_value(), old_unit, new_unit));
        }
    }
    refreshWidthButtons();

    update = was_updating;
}

/**
 * Callback for when stroke style widget is modified.
 * Triggers update action.
 */
void
StrokeStyle::selectionModifiedCB(guint flags)
{
    // We care deeply about only updating when the style is updated
    // if we update on other flags, we slow inkscape down when dragging
    if (flags & (SP_OBJECT_STYLE_MODIFIED_FLAG)) {
        // Undo, Redo and any style change: a list opened on the old widths is stale.
        // Never the scope generation: the command's own write lands here too.
        closePopovers();
        updateLine();
    }
}

/**
 * Callback for when stroke style widget is changed.
 * Triggers update action.
 */
void
StrokeStyle::selectionChangedCB()
{
    // Selection replacement is a new scope identity: any in-flight numeric
    // commit's apply/readiness must see the changed generation and roll back.
    ++_scope_generation;
    resetRowScope();
    updateLine();
}

/**
 * Sets selector widgets' dash style from an SPStyle object.
 */
void
StrokeStyle::setDashSelectorFromStyle(DashSelector *dsel, SPStyle *style)
{
    auto [dash_pattern, offset] = getDashFromStyle(style);
    dsel->set_dash_pattern(dash_pattern, offset);
    update_dash_entry(dash_pattern);
}

void StrokeStyle::update_dash_entry(const std::vector<double> &dash_pattern)
{
    if (_editing_dash_pattern || contains_focus(*_pattern_entry)) { /* The GtkText object has focus. The focus test is required
                                                                       as the StokeStyle widget is updated after all changed
                                                                       are made (unnecessarily via selectionModifiedCB()).
                                                                       Without this test, the cursor is placed at the beginning
                                                                       of the GtkEntry after each character is typed. */
        return;
    }

    std::ostringstream ost;
    for (auto d : dash_pattern) {
        ost << d << ' ';
    }
    _pattern_entry->set_text(ost.str().c_str());

    if (!dash_pattern.empty()) {
        _pattern_label->set_visible(true);
        _pattern_entry->set_visible(true);
    }
    else {
        _pattern_label->set_visible(false);
        _pattern_entry->set_visible(false);
    }
}

/**
 * Sets the join type for a line, and updates the stroke style widget's buttons
 */
void
StrokeStyle::setJoinType (unsigned const jointype)
{
    Gtk::ToggleButton *tb = nullptr;
    switch (jointype) {
        case SP_STROKE_LINEJOIN_MITER:
            tb = joinMiter;
            break;
        case SP_STROKE_LINEJOIN_ROUND:
            tb = joinRound;
            break;
        case SP_STROKE_LINEJOIN_BEVEL:
            tb = joinBevel;
            break;
        default:
            // Should not happen
            std::cerr << "StrokeStyle::setJoinType(): Invalid value: " << jointype << std::endl;
            tb = joinMiter;
            break;
    }
    setJoinButtons(tb);
}

/**
 * Sets the cap type for a line, and updates the stroke style widget's buttons
 */
void
StrokeStyle::setCapType (unsigned const captype)
{
    Gtk::ToggleButton *tb = nullptr;
    switch (captype) {
        case SP_STROKE_LINECAP_BUTT:
            tb = capButt;
            break;
        case SP_STROKE_LINECAP_ROUND:
            tb = capRound;
            break;
        case SP_STROKE_LINECAP_SQUARE:
            tb = capSquare;
            break;
        default:
            // Should not happen
            std::cerr << "StrokeStyle::setCapType(): Invalid value: " << captype << std::endl;
            tb = capButt;
            break;
    }
    setCapButtons(tb);
}

/**
 * Sets the cap type for a line, and updates the stroke style widget's buttons
 */
void
StrokeStyle::setPaintOrder (gchar const *paint_order, bool enable_markers)
{
    std::vector<std::string> compiled;
    SPIPaintOrder temp;
    temp.read( paint_order );
    _paint_order->setValue(temp, enable_markers);
}

/**
 * Callback for when stroke style widget is updated, including markers, cap type,
 * join type, etc.
 */
void
StrokeStyle::updateLine()
{
    if (update) {
        return;
    }

    auto *widg = get_parent()->get_parent()->get_parent()->get_parent(); 
    auto dialogbase = dynamic_cast<Inkscape::UI::Dialog::DialogBase*>(widg);
    if (dialogbase && !dialogbase->getShowing()) {
        return;
    }

    update = true;

    Inkscape::Selection *sel = desktop ? desktop->getSelection() : nullptr;

    if (!sel || sel->isEmpty()) {
        // Nothing selected, grey-out all controls in the stroke-style dialog
        _selection_hairline = false;
        table->set_sensitive(false);
        _row = {};
        refreshWidthRow(); // nothing to write: the row says why

        update = false;

        return;
    }

    FillOrStroke kind = STROKE;

    // create temporary style
    SPStyle query(SP_ACTIVE_DOCUMENT);
    // query into it
    // The native stroke-width query no longer feeds the width display; it is
    // retained only for the unrelated-control enablement below because its
    // stroke.noneSet side effect is what the join/cap/miter/dash/paint gate uses.
    int result_sw = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_STROKEWIDTH);
    int result_ml = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_STROKEMITERLIMIT);
    int result_cap = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_STROKECAP);
    int result_join = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_STROKEJOIN);
    int result_order = sp_desktop_query_style(desktop, &query, QUERY_STYLE_PROPERTY_PAINTORDER);

    SPIPaint &targPaint = *query.getFillOrStroke(kind == FILL);

    // Width display query: compatible eligible shapes/text only. Bitmaps,
    // protected, unavailable and covered members are excluded here, so an
    // unrelated selected bitmap can never make equal-width eligible owners look
    // Mixed. The row policy is cached: a display-only unit change never queries.
    StrokeWidthResult const widths = queryWidths();
    _row = stroke_width_row_state(widths);
    refreshWidthField(); // digits and bounds before any value is set: the display never lies
    // The menu is an editing mode, while marker/miter sensitivity follows the
    // document's actual cosmetic-stroke state. Switching to a numeric unit must
    // not make those controls active on an unchanged hairline selection.
    _selection_hairline = query.stroke_extensions.hairline;

    {
        table->set_sensitive(true);
        widthSpin->set_sensitive(true);
        widthSpin->set_tooltip_text(_("Stroke width"));
        Inkscape::Util::Unit const *unit = unitSelector->getUnit();

        if (isHairlineSelected()) {
            // Hairline is an explicit unit choice: keep its existing display
            // state. Do not touch the unit or the document.
            widthSpin->set_mixed(false);
            widthSpin->set_sensitive(false);
            widthAdj->set_value(1);
            widthSpin->set_tooltip_text(_("Hairline stroke"));
        } else if (widths.state == StrokeWidthQuery::Empty || _row.writable == 0) {
            // No eligible shape/text owner, or none the controller can write (a clone
            // alone): there is no width to display or change.
            widthSpin->set_mixed(false);
            widthSpin->set_sensitive(false);
            widthSpin->set_tooltip_text(
                _("No eligible shape or text with a stroke width in the selection"));
        } else if (widths.state == StrokeWidthQuery::Mixed) {
            // Mixed: show the placeholder, keep the chosen ordinary unit and
            // never substitute an average or force percent. Recorded so an
            // explicit commit is not discarded by the hidden adjustment value.
            widthSpin->set_sensitive(true);
            widthSpin->set_mixed(true);
            widthSpin->set_tooltip_text(_("Mixed stroke widths"));
        } else { // StrokeWidthQuery::Uniform
            widthSpin->set_mixed(false);
            if (widths.uniform_px) {
                if (unit->type == Inkscape::Util::UNIT_TYPE_LINEAR) {
                    widthAdj->set_value(
                        Inkscape::Util::Quantity::convert(*widths.uniform_px, "px", unit));
                } else {
                    // Percent display keeps the existing 100% convention: there
                    // is no single per-object reference to convert px into %.
                    widthAdj->set_value(100);
                }
                widthSpin->set_sensitive(true);
                widthSpin->set_tooltip_text(_("Stroke width"));
            } else {
                // Uniform but with no numeric value: every eligible member is a
                // cosmetic hairline while a numeric unit is selected. There is
                // no numeric value to display. Keep an indeterminate entry so
                // explicitly entering even the hidden adjustment's same value
                // can convert it; a refresh never removes the hairline style.
                widthSpin->set_sensitive(true);
                widthSpin->set_mixed(true);
                widthSpin->set_tooltip_text(_("Hairline stroke; enter a number to convert"));
            }
        }

        refreshWidthButtons();

        // if none of the selected objects has a stroke, than quite some controls should be disabled
        // These options should also be disabled for hairlines, since they don't make sense for
        // 0-width lines.
        // The markers might still be shown though, so marker and stroke-width widgets stay enabled
        bool is_enabled = (result_sw != QUERY_STYLE_NOTHING) && !targPaint.isNoneSet()
                           && !query.stroke_extensions.hairline;
        joinMiter->set_sensitive(is_enabled);
        joinRound->set_sensitive(is_enabled);
        joinBevel->set_sensitive(is_enabled);

        miterLimitSpin->set_sensitive(is_enabled);

        capButt->set_sensitive(is_enabled);
        capRound->set_sensitive(is_enabled);
        capSquare->set_sensitive(is_enabled);

        dashSelector->set_sensitive(is_enabled);
        _pattern_entry->set_sensitive(is_enabled);
        _paint_order->set_sensitive(is_enabled);
    }

    if (result_ml != QUERY_STYLE_NOTHING)
        miterLimitAdj->set_value(query.stroke_miterlimit.value); // TODO: reflect averagedness?

    using Inkscape::is_query_style_updateable;
    if (! is_query_style_updateable(result_join)) {
        setJoinType(query.stroke_linejoin.value);
    } else {
        setJoinButtons(nullptr);
    }

    if (! is_query_style_updateable(result_cap)) {
        setCapType (query.stroke_linecap.value);
    } else {
        setCapButtons(nullptr);
    }

    bool has_markers = false;
    auto const objects = sel->items_vector();
    if (!objects.empty()) {
        auto const style = objects.front()->style;
        /* Markers */
        has_markers = updateAllMarkers(objects); // FIXME: make this desktop query too

        /* Dash */
        setDashSelectorFromStyle(dashSelector, style); // FIXME: make this desktop query too
    }
    table->set_sensitive(true);

    if (! is_query_style_updateable(result_order)) {
        setPaintOrder (query.paint_order.value, has_markers);
        _align_label->set_visible(has_markers);
    } else {
        setPaintOrder (nullptr, true);
    }

    update = false;
}

/**
 * Sets a line's dash properties in a CSS style object.
 */
void set_scaled_dash(SPCSSAttr* css, int ndash, const double *dash, double offset, double scale) {
    if (ndash > 0) {
        Inkscape::CSSOStringStream osarray;
        for (int i = 0; i < ndash; i++) {
            osarray << dash[i] * scale;
            if (i < (ndash - 1)) {
                osarray << ",";
            }
        }
        sp_repr_css_set_property(css, "stroke-dasharray", osarray.str().c_str());

        Inkscape::CSSOStringStream osoffset;
        osoffset << offset * scale;
        sp_repr_css_set_property(css, "stroke-dashoffset", osoffset.str().c_str());
    } else {
        sp_repr_css_set_property(css, "stroke-dasharray", "none");
        sp_repr_css_set_property(css, "stroke-dashoffset", nullptr);
    }
}

double calc_scale_line_width(double width_typed, const SPItem* item, const Unit* unit) {
    if (unit->abbr == "%") {
        auto scale = item->i2doc_affine().descrim();;
        double old_w = item->style->stroke_width.computed;
        return (old_w * width_typed / 100) * scale;
    } else if (unit->type == Inkscape::Util::UNIT_TYPE_LINEAR) {
        return Inkscape::Util::Quantity::convert(width_typed, unit, "px");
    }
    return width_typed;
}

namespace {

/// RAII set/restore of StrokeStyle's reentrancy flag for a native transaction.
struct ScopedWidthUpdate {
    gboolean &flag;
    gboolean const saved;
    explicit ScopedWidthUpdate(gboolean &f) : flag(f), saved(f) { flag = true; }
    ~ScopedWidthUpdate() { flag = saved; }
};

/// Report a numeric-width outcome on the existing desktop status stack.
void flash_stroke_width_status(SPDesktop *desktop, Inkscape::MessageType type,
                               Glib::ustring const &message)
{
    if (desktop && desktop->messageStack()) {
        desktop->messageStack()->flash(type, message);
    }
}

} // namespace

/**
 * Set the stroke width and adjust the dash pattern if needed.
 *
 * Hairline and numeric ordinary/percent values use one shared controller plan and one
 * caller-owned atomic interaction: never `sp_desktop_set_style`,
 * `sp_desktop_apply_css_recursive`, `DocumentUndo::done` or the native TextTool
 * `_styleSet`.
 */
void StrokeStyle::setStrokeWidth(bool from_value_signal)
{
    double width_typed = widthAdj->get_value();

    // The widget emits on deliberate edits, including a same-number commit
    // from Mixed; programmatic refreshes are suppressed by this guard.
    if (update) {
        return;
    }

    // A refresh that restored the adjustment inside the previous emission makes
    // GtkAdjustment restart that emission: the restored value arrives here again
    // with the guard down. It is not an edit and must never be written (it could
    // renormalise members that are uniform only by tolerance).
    if (_restored_value) {
        bool const restored = *_restored_value == width_typed;
        _restored_value.reset();
        if (restored && from_value_signal) return;
    }

    if (!desktop) {
        return;
    }

    SPDocument *document = desktop->getDocument();
    Selection *selection = desktop->getSelection();
    if (!document || !selection) {
        return;
    }

    bool const hairline_selected = isHairlineSelected();
    if (!hairline_selected && (!std::isfinite(width_typed) || width_typed < 0.0)) {
        flash_stroke_width_status(desktop, Inkscape::WARNING_MESSAGE,
                                  _("Stroke width: the entered value is not a valid number"));
        return;
    }

    auto prefs = Inkscape::Preferences::get();
    Inkscape::Util::Unit const *unit = unitSelector->getUnit();

    // Explicit numeric intent. Absolute values convert once from the current unit
    // to document CSS px; percent is an explicit relative scale (200 => x2), never
    // an inferred mixed-state mode.
    StrokeWidthIntent intent;
    if (hairline_selected) {
        intent.kind = StrokeWidthIntentKind::Hairline;
    } else if (unit && (unit->type == Inkscape::Util::UNIT_TYPE_DIMENSIONLESS || unit->abbr == "%")) {
        intent.kind = StrokeWidthIntentKind::RelativePercent;
        intent.value = width_typed;
    } else if (unit && unit->type == Inkscape::Util::UNIT_TYPE_LINEAR) {
        intent.kind = StrokeWidthIntentKind::AbsoluteCssPx;
        intent.value = Inkscape::Util::Quantity::convert(width_typed, unit, "px");
    } else {
        flash_stroke_width_status(desktop, Inkscape::WARNING_MESSAGE,
                                  _("Stroke width: the selected unit is not supported"));
        return;
    }
    if (!std::isfinite(intent.value) || intent.value < 0.0) {
        flash_stroke_width_status(desktop, Inkscape::WARNING_MESSAGE,
                                  _("Stroke width: the entered value cannot be converted"));
        return;
    }
    intent.scale_dashes = prefs->getBool("/options/dash/scale", true);

    _last_typed_state = runWidthCommand(intent, RC_("Undo", "Set stroke width")).state;
    if (from_value_signal && widthAdj->get_value() != width_typed) {
        // The refresh restored the document's value while this emission is running.
        _restored_value = widthAdj->get_value();
        _restored_idle = Glib::signal_idle().connect([this] { _restored_value.reset(); return false; });
    }
}

/**
 * Run one width command for the current selection: the guard stops every
 * modification callback from re-entering the row, and the row refreshes from the
 * document afterwards, whatever the outcome.
 */
StrokeWidthCommandOutcome StrokeStyle::runWidthCommand(StrokeWidthIntent const &intent,
                                                       Util::Internal::ContextString label)
{
    StrokeWidthCommandOutcome outcome;
    {
        ScopedWidthUpdate const updating(update);
        StrokeWidthCommandRequest const request{intent, &_scope_generation, unitSelector->getUnit(), label,
                                                INKSCAPE_ICON("dialog-fill-and-stroke")};
        outcome = apply_stroke_widths_command(*desktop, request);
    }
    // Refresh the width display under its guard. A redundant `%` entry is
    // normalized to 100 by updateLine without a second write because every
    // programmatic value change is ignored while `update` is true there.
    updateLine();
    return outcome;
}

// ---------------------------------------------------------------------------
// The width row: - field v + unit (internal note STROKE_WIDTH_CONTROLS_PLAN §5)
// ---------------------------------------------------------------------------

/// The unit that gives steps, digits and the preset list: the shown unit when it
/// is linear, else the last linear unit (while the menu is on % or Hairline).
Util::Unit const *StrokeStyle::linearUnit() const
{
    auto const *unit = unitSelector->getUnit();
    if (!isHairlineSelected() && unit && unit->type == Util::UNIT_TYPE_LINEAR) return unit;
    return _last_linear_unit ? _last_linear_unit : Util::UnitTable::get().getUnit("px");
}

/// The read-only width query of the current selection (text range included).
StrokeWidthResult StrokeStyle::queryWidths() const
{
    StrokeWidthResult widths;
    if (!desktop) return widths;
    auto *document = desktop->getDocument();
    auto *selection = desktop->getSelection();
    if (!document || !selection) return widths;
    document->ensureUpToDate();
    auto const roots = selection->items_vector();
    if (auto const range = stroke_width_current_text_range(desktop)) {
        return query_stroke_widths(*document, *range, roots);
    }
    return query_stroke_widths(*document, roots);
}

/// Digits and bounds. Never writes a value. The upper bound is raised BEFORE any
/// value is set, so a queried width is never clamped; typed input is limited by
/// the maximum (1e6 px in the shown unit), not by the display range.
void StrokeStyle::refreshWidthField()
{
    if (!widthSpin) return; // torn down
    ScopedWidthUpdate const guard(update);
    auto const *unit = unitSelector->getUnit();
    auto const &linear = *linearUnit();
    widthSpin->set_digits(static_cast<int>(stroke_width_digits(linear)));
    bool const shown_linear = !isHairlineSelected() && unit && unit->type == Util::UNIT_TYPE_LINEAR;
    double const maximum = shown_linear ? Util::Quantity::convert(max_width_px, "px", unit) : max_width_px;
    double upper = maximum;
    if (shown_linear && _row.uniform_px) {
        upper = std::max(upper, Util::Quantity::convert(*_row.uniform_px, "px", unit));
    }
    widthAdj->set_upper(upper);
    widthSpin->set_input_maximum(maximum);
}

/// Sensitivity and tooltips of - + and the preset button from the cached query.
void StrokeStyle::refreshWidthButtons()
{
    if (!_width_dec || !_width_inc || !_width_presets) return; // torn down
    ScopedWidthUpdate const guard(update);
    auto const *unit = unitSelector->getUnit();
    auto const &linear = *linearUnit();
    bool const hairline_unit = isHairlineSelected();
    bool const percent = !hairline_unit && unit && unit->type != Util::UNIT_TYPE_LINEAR;
    bool const writable = _row.writable > 0;

    double const step = stroke_width_step(linear);
    double const step_px = Util::Quantity::convert(step, &linear, "px");
    auto const step_text = plain_number(step) + " " + linear.abbr.raw();

    bool dec_ok = false;
    bool inc_ok = false;
    bool presets_ok = false;
    Glib::ustring dec_tip;
    Glib::ustring inc_tip;
    Glib::ustring presets_tip = _("Preset stroke widths");

    if (!writable) {
        dec_tip = inc_tip = presets_tip = _("No stroke width to change in the selection");
    } else if (percent || hairline_unit) {
        presets_ok = true;
        dec_tip = inc_tip = percent ? _("Stroke width steps use absolute units only")
                                    : _("Choose a numeric unit to step the stroke width");
    } else {
        presets_ok = true;
        inc_ok = true;
        inc_tip = Glib::ustring::compose(_("Increase stroke width (%1 per click)"), step_text);
        auto const decrease = Glib::ustring::compose(_("Decrease stroke width (%1 per click)"), step_text);
        if (_row.state == StrokeWidthQuery::Uniform && _row.uniform_px) {
            dec_ok = stroke_width_can_decrease(*_row.uniform_px, step_px);
            dec_tip = dec_ok ? decrease : Glib::ustring(_("Already at the minimum step"));
        } else if (_row.state == StrokeWidthQuery::Uniform) {
            // Every eligible member is a hairline: + applies the smallest preset.
            auto const presets = stroke_width_presets(*stroke_width_list_unit(linear));
            if (presets.size() > 1) {
                inc_tip = Glib::ustring::compose(_("Apply the smallest preset width (%1 %2)"), presets[1].label,
                                                 presets[1].unit->abbr.raw());
            }
            dec_tip = _("Hairline stroke: press + for the smallest preset width");
        } else {
            dec_ok = stroke_width_can_decrease(_row.max_px, step_px);
            dec_tip = dec_ok ? decrease : Glib::ustring(_("Every selected width is at the minimum step"));
        }
    }

    _width_dec->set_sensitive(dec_ok);
    _width_dec->set_tooltip_text(dec_tip);
    _width_inc->set_sensitive(inc_ok);
    _width_inc->set_tooltip_text(inc_tip);
    bool const presets_were_ok = _width_presets->get_sensitive();
    _width_presets->set_sensitive(presets_ok);
    _width_presets->set_tooltip_text(presets_tip);
    if (presets_were_ok && !presets_ok) closePopovers();
}

void StrokeStyle::refreshWidthRow()
{
    refreshWidthField();
    refreshWidthButtons();
}

/// Everything a scope change invalidates: pending text, the open list, its items.
void StrokeStyle::resetRowScope()
{
    _last_invalid_text.reset();
    ++_row_generation;
    if (widthSpin && widthSpin->has_pending_edit()) {
        bool const focused = contains_focus(*widthSpin);
        widthSpin->discard_pending_edit(); // hides the entry without moving focus
        if (focused) focusCanvasAfterEdit();
    }
    closePopovers();
}

void StrokeStyle::closePopovers()
{
    ++_row_generation; // an activation already in flight is refused by its own generation
    if (_width_presets) _width_presets->popdown();
}

void StrokeStyle::rememberFocus()
{
    if (_focus_return) {
        g_object_remove_weak_pointer(G_OBJECT(_focus_return), reinterpret_cast<gpointer *>(&_focus_return));
        _focus_return = nullptr;
    }
    if (auto *root = gtk_widget_get_root(GTK_WIDGET(gobj()))) {
        auto *focus = gtk_root_get_focus(root);
        auto *popover = _preset_popover ? GTK_WIDGET(_preset_popover->gobj()) : nullptr;
        if (focus && popover && (focus == popover || gtk_widget_is_ancestor(focus, popover))) focus = nullptr;
        if (focus) {
            _focus_return = focus;
            g_object_add_weak_pointer(G_OBJECT(focus), reinterpret_cast<gpointer *>(&_focus_return));
        }
    }
}

/// The list closed (chosen item, Esc, click outside or closePopovers): hand keyboard
/// focus back to where it was when it opened (the canvas or the text caret with its
/// range, or the button itself after a keyboard open), or to the canvas when that
/// widget is gone. Only when focus is still lost inside the list: a click on another
/// widget keeps its own focus. One idle, so no handler grabs focus from inside a
/// signal that is still being emitted.
void StrokeStyle::restoreFocus()
{
    _focus_idle = Glib::signal_idle().connect([this] {
        auto *root = gtk_widget_get_root(GTK_WIDGET(gobj()));
        auto *now = root ? gtk_root_get_focus(root) : nullptr;
        auto *popover = _preset_popover ? GTK_WIDGET(_preset_popover->gobj()) : nullptr;
        auto *button = _width_presets ? GTK_WIDGET(_width_presets->gobj()) : nullptr;
        auto const inside = [](GtkWidget *widget, GtkWidget *ancestor) {
            return widget && ancestor && (widget == ancestor || gtk_widget_is_ancestor(widget, ancestor));
        };
        auto *target = _focus_return;
        // GTK leaves focus on the menu button after the list closes. That is right
        // after a keyboard open (focus was on the button), wrong after a pointer press.
        bool const lost = !now || inside(now, popover) || (inside(now, button) && !inside(target, button));
        if (!lost) return false;
        if (target && gtk_widget_get_mapped(target) && gtk_widget_is_sensitive(target)) {
            gtk_widget_grab_focus(target);
        } else if (desktop) {
            if (auto *canvas = desktop->getCanvas()) canvas->grab_focus();
        }
        return false;
    });
}

/// A discarded or undone edit hid the entry that had focus: the keyboard goes back
/// to the canvas (where the text caret lives) instead of nowhere (M2).
void StrokeStyle::focusCanvasAfterEdit()
{
    _focus_idle = Glib::signal_idle().connect([this] {
        auto *root = gtk_widget_get_root(GTK_WIDGET(gobj()));
        auto *now = root ? gtk_root_get_focus(root) : nullptr;
        if (desktop && (!now || (widthSpin && contains_focus(*widthSpin)))) {
            // A floating dialog: bring the document window forward, as defocusDialog does.
            if (auto *window = dynamic_cast<Gtk::Window *>(get_root())) sp_dialog_defocus(window);
            if (auto *canvas = desktop->getCanvas()) canvas->grab_focus();
            // The panel's own window keeps no focus on the hidden entry.
            if (root && widthSpin && contains_focus(*widthSpin)) gtk_root_set_focus(root, nullptr);
        }
        return false;
    });
}

/// The rejected text of a typed commit is reported once per text (M1): focus-out
/// and every later pointer-leave commit the same rejected text again.
void StrokeStyle::onInvalidText()
{
    std::string const text = _width_entry ? _width_entry->get_text().raw() : std::string();
    if (_last_invalid_text == text) return;
    _last_invalid_text = text;
    flash_stroke_width_status(
        desktop, Inkscape::WARNING_MESSAGE,
        text.find(',') != std::string::npos
            ? _("Stroke width: a number with a comma is ambiguous (decimals or thousands); type a point for "
                "decimals. Nothing was applied")
            : _("Stroke width: the entered value is not valid; nothing was applied"));
}

/// Unmodified Up/Down step like + and -; everything else (modified arrows, Page
/// keys, Home, End) is left alone so the panel scrolls. Ctrl/Cmd+Z and Redo are
/// only observed here: the wrapper forwards them to the document.
bool StrokeStyle::onFieldKey(Gtk::EventControllerKey const &controller, unsigned keyval, unsigned keycode,
                             Gdk::ModifierType state)
{
    // The same Latin-key translation as the wrapper's own Undo handling, with its
    // fallback for a signal that carries no event (a non-Latin layout still means Z).
    unsigned const latin = controller.get_current_event()
        ? Tools::get_latin_keyval(controller, keyval, keycode, state)
        : keyval;
    _last_invalid_text.reset(); // a key press is a deliberate action: report again
    _restored_value.reset();
    auto const mods = state & Gtk::Accelerator::get_default_mod_mask();
    bool primary = (mods & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
#ifdef __APPLE__
    primary = primary || (mods & Gdk::ModifierType::META_MASK) != Gdk::ModifierType{};
#endif
    bool const history_key = keyval == GDK_KEY_Undo || keyval == GDK_KEY_Redo ||
                             (primary && (latin == GDK_KEY_z || latin == GDK_KEY_Z || latin == GDK_KEY_y ||
                                          latin == GDK_KEY_Y));
    if (history_key) {
        // The wrapper discards the entry (which then owns no focus) and runs the
        // document action: give focus back to the canvas afterwards (M2).
        focusCanvasAfterEdit();
        return false;
    }

    int direction = 0;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) direction = +1;
    else if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) direction = -1;
    if (!direction || mods != Gdk::ModifierType{}) return false;

    // Auto-repeat of a held key is swallowed until the key is released.
    if (!_held_keys.insert(keyval).second) return true;
    onStep(direction);
    return true;
}

/// - and +: one Undo step each. Pending text is committed first as its own action.
void StrokeStyle::onStep(int direction)
{
    if (update || !desktop) return;
    auto *button = direction < 0 ? _width_dec : _width_inc;
    if (!button->is_sensitive()) return; // as a disabled button would

    if (widthSpin->has_pending_edit()) {
        _last_typed_state = StrokeWidthCommandState::Unchanged;
        _last_invalid_text.reset(); // an explicit action always reports
        auto const committed = widthSpin->commit_pending_edit();
        if (committed == SpinButton::CommitResult::Invalid) return; // reported by onInvalidText, nothing written
        // The commit ran the typed command synchronously. Stop unless it applied or
        // was a no-op; a refused, rejected or failed commit never steps.
        if (_last_typed_state != StrokeWidthCommandState::Applied &&
            _last_typed_state != StrokeWidthCommandState::Unchanged) {
            return;
        }
        if (update || !desktop || !button->is_sensitive()) return; // the commit changed what is possible
    }

    // Step from a fresh query, never from the rounded text or a cached width.
    auto const &linear = *linearUnit();
    auto const *unit = unitSelector->getUnit();
    if (isHairlineSelected() || !unit || unit->type != Util::UNIT_TYPE_LINEAR) return;
    auto const row = stroke_width_row_state(queryWidths());
    if (row.writable == 0) return;
    double const step_px = Util::Quantity::convert(stroke_width_step(linear), &linear, "px");
    if (!(step_px > 0.0)) return;

    StrokeWidthIntent intent;
    if (row.state == StrokeWidthQuery::Uniform && row.uniform_px) {
        double const width = *row.uniform_px;
        if (direction < 0 && !stroke_width_can_decrease(width, step_px)) {
            // The controller answers Unchanged with its floor note.
            intent.kind = StrokeWidthIntentKind::AdditiveCssPx;
            intent.value = -step_px;
        } else {
            intent.kind = StrokeWidthIntentKind::AbsoluteCssPx;
            intent.value = width + direction * step_px;
        }
    } else if (row.state == StrokeWidthQuery::Uniform) {
        // Uniform hairline: + applies the smallest preset, - has nothing to do.
        if (direction < 0) return;
        auto const presets = stroke_width_presets(*stroke_width_list_unit(linear));
        if (presets.size() < 2) return;
        intent.kind = StrokeWidthIntentKind::AbsoluteCssPx;
        intent.value = stroke_width_preset_px(presets[1]);
    } else if (row.state == StrokeWidthQuery::Mixed) {
        // Each object's own width moves by one step; the differences are kept.
        intent.kind = StrokeWidthIntentKind::AdditiveCssPx;
        intent.value = direction * step_px;
    } else {
        return;
    }
    runWidthCommand(intent, direction < 0 ? RC_("Undo", "Decrease stroke width")
                                          : RC_("Undo", "Increase stroke width"));
}

/// Items of the preset list from the cached query: no new query on open.
void StrokeStyle::rebuildPresetList()
{
    _preset_popover->remove_all();
    auto const &linear = *linearUnit();
    auto const presets = stroke_width_presets(*stroke_width_list_unit(linear));
    auto const generation = _row_generation;
    for (auto const &preset : presets) {
        bool checked = false;
        if (preset.kind == StrokeWidthPresetKind::Hairline) {
            checked = _row.state == StrokeWidthQuery::Uniform && !_row.uniform_px;
        } else if (_row.state == StrokeWidthQuery::Uniform && _row.uniform_px) {
            checked = stroke_width_same_width(*_row.uniform_px, stroke_width_preset_px(preset));
        }
        auto const check = Gtk::make_managed<Gtk::CheckButton>(preset.label);
        check->set_active(checked);
        check->set_can_target(false); // the item takes the click
        check->set_focusable(false);
        auto const item = Gtk::make_managed<PopoverMenuItem>();
        item->set_child(*check);
        // Tracked: an item retained past the row (a queued activation) must not call it.
        item->signal_activate().connect(sigc::track_object(
            [this, preset, generation] { onPresetChosen(preset, generation); }, *this));
        _preset_popover->append(*item);
    }
}

/// A preset is a command: the exact descriptor value, never an index or a display
/// rounding. The controller decides a no-op; the widget never does.
void StrokeStyle::onPresetChosen(StrokeWidthPreset const &preset, std::uint64_t generation)
{
    if (update || !desktop || generation != _row_generation) return; // stale: closed or scope changed
    StrokeWidthIntent intent;
    if (preset.kind == StrokeWidthPresetKind::Hairline) {
        intent.kind = StrokeWidthIntentKind::Hairline;
    } else {
        intent.kind = StrokeWidthIntentKind::AbsoluteCssPx;
        intent.value = stroke_width_preset_px(preset);
    }
    auto const outcome = runWidthCommand(intent, RC_("Undo", "Set stroke width"));

    // An absolute preset chosen on % or Hairline leaves that unit for the list unit
    // of the last linear unit, after the write scope closed so the refresh runs.
    // Unchanged or failed: the unit stays.
    if (outcome.state == StrokeWidthCommandState::Applied && intent.kind == StrokeWidthIntentKind::AbsoluteCssPx) {
        auto const *unit = unitSelector->getUnit();
        if (isHairlineSelected() || (unit && unit->type != Util::UNIT_TYPE_LINEAR)) {
            unitSelector->setUnit(stroke_width_list_unit(*linearUnit())->abbr);
        }
    }
}

namespace {
using DashCss = std::shared_ptr<SPCSSAttr>;
struct DashWrite {
    SPWeakPtr<SPObject> source;
    DashCss css;
    std::string array, offset;
    bool array_important, offset_important;
};
bool flat_dash_source(SPObject *source, SPItem *owner)
{
    if (!source || !source->firstChild()) return false;
    auto const name = std::string(source->getRepr()->name());
    if (source != owner && name != "svg:tspan" && name != "svg:flowSpan" &&
        name != "svg:flowPara" && name != "svg:textPath") return false;
    for (auto &child : source->children) if (!is<SPString>(&child)) return false;
    return true;
}
std::vector<std::string> dash_preservation(SPObject *root, std::unordered_set<SPObject *> const &written)
{
    std::vector<std::string> out;
    std::function<void(SPObject *)> visit = [&](SPObject *o) {
        auto *repr = o->getRepr();
        out.push_back(repr->name());
        out.push_back(repr->content() ? repr->content() : "");
        for (auto const &attr : repr->attributeList()) {
            auto const name = std::string(g_quark_to_string(attr.key));
            if (name != "style" || !written.count(o)) {
                out.push_back(name); out.emplace_back(attr.value);
            }
        }
        if (written.count(o)) {
            DashCss css(sp_repr_css_attr(repr, "style"), sp_repr_css_attr_unref);
            for (auto const &attr : css->attributeList()) {
                auto const name = std::string(g_quark_to_string(attr.key));
                if (name != "stroke-dasharray" && name != "stroke-dashoffset") {
                    out.push_back(name); out.emplace_back(attr.value);
                }
            }
        }
        if (o->style) for (auto *property : o->style->properties()) {
            if ((written.count(o) || is<SPUse>(o) || (is<SPString>(o) && written.count(o->parent))) &&
                (property->name() == "stroke-dasharray" ||
                                    property->name() == "stroke-dashoffset")) continue;
            out.push_back(property->get_value().raw());
        }
        if (!is<SPUse>(o)) for (auto &child : o->children) visit(&child);
        out.emplace_back("/end");
    };
    visit(root);
    return out;
}
} // namespace

void StrokeStyle::setStrokeDash()
{
    if (update || !desktop || !desktop->getDocument()) return;
    ScopedWidthUpdate guard(update);
    auto *document = desktop->getDocument();
    auto const dash = dashSelector->get_dash_pattern();
    auto const offset = dashSelector->get_offset();
    bool const scaling = Inkscape::Preferences::get()->getBool("/options/dash/scale", true);
    auto *active_tool = desktop->getTool();
    auto *tool = dynamic_cast<Inkscape::UI::Tools::TextTool *>(active_tool);
    auto *range_owner = tool && tool->text_sel_start != tool->text_sel_end ? tool->textItem() : nullptr;
    bool dead = false;
    sigc::scoped_connection const destroy = desktop->connectDestroy([&](auto &&...) { dead = true; });
    std::vector<SPItem *> roots;
    for (auto *item : desktop->getSelection()->items()) roots.push_back(item);
    auto *text_owner = tool ? tool->textItem() : nullptr;
    auto const *layout = text_owner ? te_get_layout(text_owner) : nullptr;
    auto const caret_index = layout && tool ? layout->iteratorToCharIndex(tool->text_sel_start) : 0;
    document->ensureUpToDate();
    if (dead || desktop->getDocument() != document) return;
    auto const query = query_stroke_widths(*document, roots);
    std::vector<DashWrite> writes;
    std::unordered_set<SPObject *> written;
    std::string exclusions;
    auto exclude = [&](SPItem *owner, char const *reason) {
        exclusions += " Stroke dash: ";
        exclusions += owner && owner->getId() ? owner->getId() : "owner";
        exclusions += ": "; exclusions += reason; exclusions += "; owner unchanged.";
    };
    for (auto const &target : query.excluded) {
        if (target.eligibility != StrokeWidthEligibility::Covered)
            exclude(target.owner.get(), "protected, referenced, empty or unavailable source");
    }
    for (auto const &target : query.targets) {
        auto *owner = target.owner.get();
        if (target.kind == StrokeWidthTargetKind::CloneInstance) {
            exclude(owner, "edit the original; clone instances are not dash targets"); continue;
        }
        if (owner == range_owner) {
            exclude(owner, "per-source dash range handling is not proved; use whole-owner selection or a caret");
            continue;
        }
        std::vector<DashWrite> member;
        std::unordered_set<SPObject *> sources;
        char const *reason = nullptr;
        for (auto const &run : target.runs) {
            auto *source = run.style_source.get();
            if (!sources.insert(source).second) continue;
            if (!source || run.eligibility != StrokeWidthEligibility::Eligible ||
                (target.kind == StrokeWidthTargetKind::TextOwner && !flat_dash_source(source, owner))) {
                reason = "protected or unproved complete text-source coverage"; break;
            }
            auto const &style = run.style;
            if ((style.dasharray_important && style.dash_style_src == unsigned(SPStyleSrc::STYLE_SHEET)) ||
                (style.dashoffset_important && style.dashoffset_style_src == unsigned(SPStyleSrc::STYLE_SHEET))) {
                reason = "stylesheet !important dash declaration"; break;
            }
            double const factor = scaling ? style.local_computed : 1.0;
            if (!std::isfinite(factor) || factor < 0 || !std::isfinite(offset * factor) ||
                std::any_of(dash.begin(), dash.end(), [&](double p) { return p < 0 || !std::isfinite(p * factor); })) {
                reason = "invalid local dash arithmetic"; break;
            }
            DashCss css(sp_repr_css_attr_new(), sp_repr_css_attr_unref);
            set_scaled_dash(css.get(), dash.size(), dash.data(), offset, factor);
            if (dash.empty()) sp_repr_css_set_property(css.get(), "stroke-dashoffset", "0");
            for (auto const *name : {"stroke-dasharray", "stroke-dashoffset"}) {
                bool const important = std::string(name) == "stroke-dasharray" ? style.dasharray_important : style.dashoffset_important;
                if (important) {
                    std::string value = sp_repr_css_property(css.get(), name, "");
                    sp_repr_css_set_property(css.get(), name, (value + " !important").c_str());
                }
            }
            SPStyle expected(document);
            expected.readIfUnset(SPAttr::STROKE_DASHARRAY, sp_repr_css_property(css.get(), "stroke-dasharray", "none"));
            expected.readIfUnset(SPAttr::STROKE_DASHOFFSET, sp_repr_css_property(css.get(), "stroke-dashoffset", "0"));
            if (style.dash_computed.size() == expected.stroke_dasharray.values.size() &&
                std::equal(style.dash_computed.begin(), style.dash_computed.end(), expected.stroke_dasharray.values.begin(),
                    [](double a, SPILength const &b) { return a == b.computed; }) &&
                style.dash_offset_computed == expected.stroke_dashoffset.computed) continue;
            member.push_back({SPWeakPtr<SPObject>(source), std::move(css), expected.stroke_dasharray.get_value().raw(), expected.stroke_dashoffset.get_value().raw(),
                              expected.stroke_dasharray.important, expected.stroke_dashoffset.important});
        }
        if (reason) { exclude(owner, reason); continue; }
        for (auto &write : member) if (written.insert(write.source.get()).second) writes.push_back(std::move(write));
    }
    bool applied = false;
    auto report = [&](std::string const &note) {
        if (!dead && !note.empty()) flash_stroke_width_status(desktop, Inkscape::WARNING_MESSAGE,
            note + (applied ? " Other compatible members changed." : " No members changed."));
    };
    if (writes.empty()) { report(exclusions); return; }
    auto const preserved = dash_preservation(document->getRoot(), written);
    auto token = DocumentUndo::beginAtomicInteraction(document);
    if (!token) { report("Stroke dash refused: pending document work; no members changed."); return; }
    auto ready = [&]() {
        if (dead || !token->validFor(document) || desktop->getDocument() != document) return false;
        std::vector<SPItem *> current;
        for (auto *item : desktop->getSelection()->items()) current.push_back(item);
        if (current != roots || desktop->getTool() != active_tool) return false;
        if (tool && (tool->textItem() != text_owner ||
            (text_owner && (!te_get_layout(text_owner) ||
             te_get_layout(text_owner)->iteratorToCharIndex(tool->text_sel_start) != caret_index)) ||
            (range_owner == nullptr && tool->text_sel_start != tool->text_sel_end))) return false;
        document->ensureUpToDate();
        if (dead || !token->validFor(document) || dash_preservation(document->getRoot(), written) != preserved) return false;
        for (auto const &write : writes) {
            auto *source = write.source.get();
            if (!source || source->style->stroke_dasharray.get_value() != write.array ||
                source->style->stroke_dashoffset.get_value() != write.offset ||
                source->style->stroke_dasharray.important != write.array_important ||
                source->style->stroke_dashoffset.important != write.offset_important) return false;
        }
        auto const after = query_stroke_widths(*document, roots);
        if (after.targets.size() != query.targets.size()) return false;
        for (size_t i = 0; i < query.targets.size(); ++i)
            if (after.targets[i].owner.get() != query.targets[i].owner.get() ||
                after.targets[i].glyph_signature != query.targets[i].glyph_signature) return false;
        return true;
    };
    for (auto const &write : writes) {
        if (dead || !token->validFor(document) || !write.source) break;
        write.source->changeCSS(write.css.get(), "style");
    }
    if (!ready() || !token->commitAtomically(RC_("Undo", "Set stroke dash"),
            INKSCAPE_ICON("dialog-fill-and-stroke"), ready)) {
        token->rollback();
        if (!dead) flash_stroke_width_status(desktop, Inkscape::WARNING_MESSAGE,
            "Stroke dash could not be verified; the entire action was rolled back.");
        return;
    }
    applied = true;
    // Selection edits never update insertion/default styles (SELECTION_CONTRACT).
    report(exclusions);
}
/**
 * Set the Miter Limit value only.
 */
void StrokeStyle::setStrokeMiter()
{
    if (update) return;
    update = true;

    SPCSSAttr *css = sp_repr_css_attr_new();
    auto const value = miterLimitAdj->get_value();
    sp_repr_css_set_property_double(css, "stroke-miterlimit", value);

    for (auto item : desktop->getSelection()->items()) {
        sp_desktop_apply_css_recursive(item, css, true);
    }
    sp_desktop_set_style (desktop, css, false);
    sp_repr_css_attr_unref(css);
    DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Set stroke miter"),
                       INKSCAPE_ICON("dialog-fill-and-stroke"));
    update = false;
}

/**
 * Returns whether the currently selected stroke width is "hairline"
 *
 */
bool
StrokeStyle::isHairlineSelected() const
{
    return unitSelector->get_selected() == _hairline_item;
}


/**
 * This routine handles toggle events for buttons in the stroke style dialog.
 *
 * When activated, this routine gets the data for the various widgets, and then
 * calls the respective routines to update css properties, etc.
 *
 */
void StrokeStyle::buttonToggledCB(StrokeStyleButton *tb)
{
    if (update) {
        return;
    }

    if (tb->get_active()) {
        /* TODO: Create some standardized method */
        SPCSSAttr *css = sp_repr_css_attr_new();

        switch (tb->get_button_type()) {
            case STROKE_STYLE_BUTTON_JOIN: 
                sp_repr_css_set_property(css, "stroke-linejoin", tb->get_stroke_style());
                sp_desktop_set_style (desktop, css);
                setJoinButtons(tb);
                break;
            case STROKE_STYLE_BUTTON_CAP:
                sp_repr_css_set_property(css, "stroke-linecap", tb->get_stroke_style());
                sp_desktop_set_style (desktop, css);
                setCapButtons(tb);
                break;
        }

        sp_repr_css_attr_unref(css);
        css = nullptr;

        DocumentUndo::done(desktop->getDocument(), RC_("Undo", "Set stroke style"), INKSCAPE_ICON("dialog-fill-and-stroke"));
    }
}

/**
 * Updates the join style toggle buttons
 */
void
StrokeStyle::setJoinButtons(Gtk::ToggleButton *active)
{
    joinMiter->set_active(active == joinMiter);
    miterLimitSpin->set_sensitive(active == joinMiter && !isHairlineSelected() && !_selection_hairline);
    joinRound->set_active(active == joinRound);
    joinBevel->set_active(active == joinBevel);
    // Hide entire miter row when not Miter join
    _miter_hb->set_visible(active == joinMiter);
}

/**
 * Updates the cap style toggle buttons
 */
void
StrokeStyle::setCapButtons(Gtk::ToggleButton *active)
{
    capButt->set_active(active == capButt);
    capRound->set_active(active == capRound);
    capSquare->set_active(active == capSquare);
}


/**
 * Recursively builds a simple list from an arbitrarily complex selection
 * of items and grouped items
 */
static void buildGroupedItemList(SPObject *element, std::vector<SPObject*> &simple_list)
{
    if (is<SPGroup>(element)) {
        for (SPObject *i = element->firstChild(); i; i = i->getNext()) {
            buildGroupedItemList(i, simple_list);
        }
    } else {
        simple_list.push_back(element);
    }
}


/**
 * Updates the marker combobox to highlight the appropriate marker and scroll to
 * that marker.
 *
 * @returns true if any of the objects, children of groups, have markers applied
 */
bool
StrokeStyle::updateAllMarkers(std::vector<SPItem*> const &objects)
{
    struct { MarkerComboBox *key; int loc; } const keyloc[] = {
            { startMarkerCombo, SP_MARKER_LOC_START },
            { midMarkerCombo, SP_MARKER_LOC_MID },
            { endMarkerCombo, SP_MARKER_LOC_END }
    };

    bool all_texts = true;

    auto simplified_list = std::vector<SPObject *>();
    for (SPItem *item : objects) {
        buildGroupedItemList(item, simplified_list);
    }

    for (SPObject *object : simplified_list) {
        if (!is<SPText>(object)) {
            all_texts = false;
            break;
        }
    }

    bool has_markers = false;

    // Use the first in the list that has the marker of each type, if any
    for (auto const &markertype : keyloc) {
        // For all three marker types,

        // find the corresponding combobox item
        MarkerComboBox *combo = markertype.key;

        // Quit if we're in update state
        if (combo->in_update()) {
            return true; // Assume markers
        }

        // Per SVG spec, text objects cannot have markers; disable combobox if only texts are selected
        // They should also be disabled for hairlines, since scaling against a 0-width line doesn't
        // make sense.
        combo->set_sensitive(!all_texts && !isHairlineSelected() && !_selection_hairline);

        SPObject *marker = nullptr;

        if (!all_texts && !isHairlineSelected() && !_selection_hairline) {
            for (SPObject *object : simplified_list) {
                char const *value = object->style->marker_ptrs[markertype.loc]->value();

                // If the object has this type of markers,
                if (value == nullptr)
                    continue;

                // Extract the name of the marker that the object uses
                marker = getMarkerObj(value, object->document);
                has_markers = true;
            }
        }

        // Scroll the combobox to that marker
        combo->set_current(marker);
    }

    return has_markers;
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
