// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Widgets used in the stroke style dialog.
 */
/* Author:
 *   Lauris Kaplinski <lauris@ximian.com>
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2010 Jon A. Cruz
 * Copyright (C) 2001 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_DIALOGS_STROKE_STYLE_H
#define SEEN_DIALOGS_STROKE_STYLE_H

#include <cstdint>
#include <optional>
#include <set>
#include <string>

#include <gtkmm/box.h>
#include <gtkmm/togglebutton.h>

#include "object/sp-marker-loc.h"
#include "ui/stroke-width-command.h"
#include "ui/widget/spinbutton.h"
#include "ui/widget/style/paint-order.h"

class SPItem;
class SPStyle;
class SPCSSAttr;
class SPDesktop;
namespace Gtk {
class Adjustment;
class Button;
class Entry;
class EventControllerKey;
class MenuButton;
class Grid;
class Label;
} // namespace Gtk

class SPDocument;
class SPObject;
SPObject *getMarkerObj(gchar const *n, SPDocument *doc);
std::pair<std::vector<double>, double> getDashFromStyle(SPStyle *style);

namespace Inkscape {

namespace Util {
class Unit;
} // namespace Util

namespace UI::Widget {

void set_scaled_dash(SPCSSAttr* css, int ndash, const double *dash, double offset, double scale);
std::vector<double> parse_dash_pattern(const Glib::ustring& input);
double calc_scale_line_width(double width_typed, const SPItem* item, const Util::Unit* unit);

class DashSelector;
class MarkerComboBox;
class PopoverMenu;
class UnitMenu;

class StrokeStyleButton;

class StrokeStyle : public Gtk::Box
{
public:
    StrokeStyle();
    ~StrokeStyle() override;
    void setDesktop(SPDesktop *desktop);
    void updateLine();
    void selectionModifiedCB(guint flags);
    void selectionChangedCB();
    /// Close the preset list and refuse any activation already in flight. Called
    /// when the panel is hidden (page switch, unmap) and by every scope change.
    void closePopovers();
private:
    /** List of valid types for the stroke-style radio check-button widget */
    enum StrokeStyleButtonType {
        STROKE_STYLE_BUTTON_JOIN, ///< A button to set the line-join style
        STROKE_STYLE_BUTTON_CAP   ///< A button to set the line-cap style
    };

    /**
     * A custom radio check-button for setting the stroke style.  It can be configured
     * to set either the join or cap style by setting the button_type field.
     */
    class StrokeStyleButton : public Gtk::ToggleButton {
        public:
            StrokeStyleButton(Gtk::ToggleButton    *&grp,
                              char const            *icon,
                              StrokeStyleButtonType  button_type,
                              gchar const           *stroke_style);

            /** Get the type (line/cap) of the stroke-style button */
            inline StrokeStyleButtonType get_button_type() {return button_type;}

            /** Get the stroke style attribute associated with the button */
            inline gchar const * get_stroke_style() {return stroke_style;}

        private:
            StrokeStyleButtonType button_type; ///< The type (line/cap) of the button
            gchar const *stroke_style;         ///< The stroke style associated with the button
    };

    bool updateAllMarkers(std::vector<SPItem*> const &objects);
    void setDashSelectorFromStyle(DashSelector *dsel, SPStyle *style);
    void setJoinType (unsigned const jointype);
    void setCapType (unsigned const captype);
    void setPaintOrder (gchar const *paint_order, bool enable_markers);
    void setJoinButtons(Gtk::ToggleButton *active);
    void setCapButtons(Gtk::ToggleButton *active);
    void setStrokeWidth(bool from_value_signal = false);
    StrokeWidthCommandOutcome runWidthCommand(StrokeWidthIntent const &intent, Util::Internal::ContextString label);
    void setStrokeDash();
    void setStrokeMiter();
    bool isHairlineSelected() const;

    // --- width row (- field v + unit) ---
    Util::Unit const *linearUnit() const;
    StrokeWidthResult queryWidths() const;
    void refreshWidthRow();
    void refreshWidthField();
    void refreshWidthButtons();
    void onStep(int direction);
    bool onFieldKey(Gtk::EventControllerKey const &controller, unsigned keyval, unsigned keycode,
                    Gdk::ModifierType state);
    void onInvalidText();
    void onPresetChosen(StrokeWidthPreset const &preset, std::uint64_t generation);
    void rebuildPresetList();
    void rememberFocus();
    void restoreFocus();
    void focusCanvasAfterEdit();
    void resetRowScope();

    StrokeStyleButton * makeRadioButton(Gtk::ToggleButton    *&grp,
                                        char const            *icon,
                                        Gtk::Box              *hb,
                                        StrokeStyleButtonType  button_type,
                                        gchar const           *stroke_style);

    // Callback functions
    void unitChangedCB();
    bool areMarkersBeingUpdated();
    void markerSelectCB(MarkerComboBox *marker_combo, SPMarkerLoc const which);
    void buttonToggledCB(StrokeStyleButton *tb);


    MarkerComboBox *startMarkerCombo;
    MarkerComboBox *midMarkerCombo;
    MarkerComboBox *endMarkerCombo;
    Gtk::Grid *table;
    Gtk::Box *_miter_hb;
    Glib::RefPtr<Gtk::Adjustment> widthAdj;
    Glib::RefPtr<Gtk::Adjustment> miterLimitAdj;
    SpinButton *miterLimitSpin;
    SpinButton *widthSpin;
    Gtk::Entry *_width_entry = nullptr;
    Gtk::Button *_width_dec = nullptr;
    Gtk::Button *_width_inc = nullptr;
    Gtk::MenuButton *_width_presets = nullptr;
    PopoverMenu *_preset_popover = nullptr; ///< owned by _width_presets
    UnitMenu *unitSelector;
    //Gtk::ToggleButton *hairline;
    StrokeStyleButton *joinMiter;
    StrokeStyleButton *joinRound;
    StrokeStyleButton *joinBevel;
    StrokeStyleButton *capButt;
    StrokeStyleButton *capRound;
    StrokeStyleButton *capSquare;
    PaintOrderWidget *_paint_order;
    Gtk::Box* _align_label;
    DashSelector *dashSelector;
    Gtk::Entry* _pattern_entry = nullptr;
    Gtk::Label* _pattern_label = nullptr;
    void update_dash_entry(const std::vector<double> &dash_pattern);
    bool _editing_dash_pattern = false;

    gboolean update;
    /** Monotonic selection/document scope identity for the numeric commit. It is
     * captured by the prepared plan and compared again by apply/readiness so a
     * synchronous selection or document replacement fails readiness. */
    std::uint64_t _scope_generation = 0;
    bool _selection_hairline = false;
    /// Outcome of the last typed commit, read by the - and + handlers after a pending commit.
    StrokeWidthCommandState _last_typed_state = StrokeWidthCommandState::Unchanged;
    SPDesktop *desktop;
    sigc::connection startMarkerConn;
    sigc::connection midMarkerConn;
    sigc::connection endMarkerConn;
    sigc::connection paintOrderConn;
    
    Inkscape::Util::Unit const *_old_unit;
    /// The last linear unit shown: steps, digits and the preset list follow it
    /// while the unit menu is on % or Hairline.
    Inkscape::Util::Unit const *_last_linear_unit = nullptr;
    /// Cached policy of the last query, never re-queried by a display-only change.
    StrokeWidthRowState _row;
    /// Identity of the row for popover items: bumped by every scope change.
    std::uint64_t _row_generation = 0;
    std::set<unsigned> _held_keys; ///< field keys pressed and not yet released (auto-repeat guard)
    std::optional<std::string> _last_invalid_text; ///< the rejected text already reported (one report per text)
    GtkWidget *_focus_return = nullptr; ///< widget that had focus when the list opened (weak)
    bool _press_recorded = false;       ///< _focus_return was taken at the pointer press, before GTK moved focus
    sigc::scoped_connection _focus_idle;
    /// The value a refresh restored inside the adjustment's own value-changed
    /// emission. GtkAdjustment restarts that emission once its handler returns, so
    /// the restored value is reported again: a refresh, never a user edit.
    std::optional<double> _restored_value;
    sigc::scoped_connection _restored_idle;
    sigc::connection _unmap_conn;

    void _handleDocumentReplaced(SPDesktop *, SPDocument *);
    void enterEditMarkerMode(SPMarkerLoc editMarkerMode);
    sigc::connection _document_replaced_connection;
    unsigned int _hairline_item;
};

} // namespace UI::Widget

} // namespace Inkscape

#endif // SEEN_DIALOGS_STROKE_STYLE_H

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
