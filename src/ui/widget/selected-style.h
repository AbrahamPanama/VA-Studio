// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   buliabyak@gmail.com
 *   scislac@users.sf.net
 *
 * Copyright (C) 2005 authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef SEEN_INKSCAPE_UI_SELECTED_STYLE_H
#define SEEN_INKSCAPE_UI_SELECTED_STYLE_H

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "ui/stroke-width-command.h"

#include <gtkmm/gesture.h> // Gtk::EventSequenceState
#include <gtkmm/grid.h>

#include "colors/color.h"
#include "rotateable.h"
#include "ui/defocus-target.h"
#include "ui/popup-menu.h"
#include "ui/widget/generic/popover-bin.h"

namespace Gtk {
class Adjustment;
class GestureClick;
class CheckButton;
class Label;
} // namespace Gtk

class SPDesktop;
class SPDocument;
class SPItem;

namespace Inkscape {

namespace Util {
class Unit;
} // namespace Util

namespace UI::Widget {

class InkSpinButton;
class PopoverMenu;
class PopoverMenuItem;

enum PaintType {
    SS_NA,
    SS_NONE,
    SS_UNSET,
    SS_MANY,
    SS_PATTERN,
    SS_HATCH,
    SS_LGRADIENT,
    SS_RGRADIENT,
    SS_MGRADIENT,
    SS_COLOR
};

enum FillOrStroke {
    SS_FILL,
    SS_STROKE
};

class ColorPreview;
class GradientImage;
class SelectedStyle;
class SelectedStyleDropTracker;

class RotateableSwatch : public Rotateable {
  public:
    RotateableSwatch(SelectedStyle *parent, guint mode);
    ~RotateableSwatch() override;

    std::pair<double, double> color_adjust(Colors::Color const &cc, double by, guint state);

    void do_motion (double by, guint state) override;
    void do_release (double by, guint state) override;
    void do_scroll (double by, guint state) override;

private:
    guint fillstroke;

    SelectedStyle *parent;

    std::optional<Colors::Color> startcolor;

    gchar const *undokey = "ssrot1";

    int cursor_state = -1;
};

/**
 * Selected style indicator (fill, stroke, opacity).
 */
class SelectedStyle
    : public UI::Widget::PopoverBin
    , private DefocusTarget
{
public:
    SelectedStyle();
    ~SelectedStyle() override;

    void setDesktop(SPDesktop *desktop);
    SPDesktop *getDesktop() {return _desktop;}
    void update();

    /// Widget scope identity for the bounded status-bar stroke-width
    /// transaction. A selection or document replacement starts a new scope so a
    /// plan prepared before it can never be applied or committed to stale targets.
    void beginNewScope();
    std::uint64_t scopeGeneration() const { return _scope_generation; }
    void applyStrokeWidth(StrokeWidthIntent const &intent);
    void closeStrokeWidthMenu();
    void onStrokeMiddleClick();

    std::optional<Colors::Color> _lastselected[2];
    std::optional<Colors::Color> _thisselected[2];

    guint _mode[2];

    Inkscape::Util::Unit const *_sw_unit = nullptr; // points to object in UnitTable, do not delete

protected:
    SPDesktop *_desktop = nullptr;

    /// Live plan scope identity shared by prepare, apply and the commit
    /// readiness callback of `on_popup_preset`.
    std::uint64_t _scope_generation = 0;

    // Widgets
    Gtk::Grid  *grid;

    Gtk::Label *label[2];    // 'Fill' and 'Stroke'
    Gtk::Label *tag[2];      // 'a', 'm', or empty.

    std::unique_ptr<Gtk::Label> type_label[2]; // 'L', 'R', 'M', or empty.
    std::unique_ptr<GradientImage> gradient_preview[2];
    std::unique_ptr<ColorPreview> color_preview[2];
    RotateableSwatch *swatch[2]; // // Wraps one or two of: "type_label", "gradient_preview", "color_preview"

    Gtk::Label *stroke_width; // Stroke width
    Gtk::Box *stroke_width_box = nullptr;

    Glib::RefPtr<Gtk::Adjustment> opacity_adjustment;
    Inkscape::UI::Widget::InkSpinButton *opacity_sb;

    Glib::ustring _paintserver_id[2];

    // Signals
    sigc::scoped_connection selection_changed_connection;
    sigc::scoped_connection selection_modified_connection;
    sigc::scoped_connection _document_replaced_connection;
    sigc::scoped_connection _desktop_destroy_connection;
    sigc::scoped_connection _tool_changed_connection;
    sigc::scoped_connection _text_cursor_connection;

    void _handleDocumentReplaced(SPDesktop *desktop, SPDocument *document);

    Gtk::EventSequenceState on_fill_click   (Gtk::GestureClick const &click,
                                             int n_press, double x, double y);
    Gtk::EventSequenceState on_stroke_click (Gtk::GestureClick const &click,
                                             int n_press, double x, double y);
    Gtk::EventSequenceState on_opacity_click(Gtk::GestureClick const &click,
                                             int n_press, double x, double y);
    Gtk::EventSequenceState on_sw_click     (Gtk::GestureClick const &click,
                                             int n_press, double x, double y);

    bool _opacity_blocked = false;

    std::unique_ptr<UI::Widget::PopoverMenu> _popup_opacity;
    void make_popup_opacity();
    void on_opacity_changed(double value);
    bool on_opacity_popup(PopupMenuOptionalClick);
    void opacity_0();
    void opacity_025();
    void opacity_05();
    void opacity_075();
    void opacity_1();

    void on_fill_remove();
    void on_stroke_remove();
    void on_fill_lastused();
    void on_stroke_lastused();
    void on_fill_lastselected();
    void on_stroke_lastselected();
    void on_fill_unset();
    void on_stroke_unset();
    void on_fill_edit();
    void on_stroke_edit();
    void on_fillstroke_swap();
    void on_fill_invert();
    void on_stroke_invert();
    void on_fill_white();
    void on_stroke_white();
    void on_fill_black();
    void on_stroke_black();
    void on_fill_copy();
    void on_stroke_copy();
    void on_fill_paste();
    void on_stroke_paste();
    void on_fill_opaque();
    void on_stroke_opaque();
    void _on_paste_callback(Glib::RefPtr<Gio::AsyncResult> &result, Glib::ustring typepaste, SPDesktop *target, std::uint64_t generation);
    /// Apply clipboard colour only while the captured desktop and scope generation remain current.
    bool _apply_pasted_text(Glib::ustring const &text, Glib::ustring const &typepaste, SPDesktop *target, std::uint64_t generation);

    std::unique_ptr<UI::Widget::PopoverMenu> _popup[2];
    UI::Widget::PopoverMenuItem *_popup_copy[2]{};
    void make_popup(FillOrStroke i);

    std::unique_ptr<UI::Widget::PopoverMenu> _popup_sw;
    std::vector<Gtk::CheckButton *> _unit_mis;
    void make_popup_units();
    void on_popup_units(Inkscape::Util::Unit const *u);
    struct StrokeWidthMenuIdentity;
    std::shared_ptr<StrokeWidthMenuIdentity const> _sw_opening;
    std::uint64_t _sw_menu_generation = 0;
    std::vector<Gtk::Widget *> _sw_width_items;
    std::shared_ptr<StrokeWidthMenuIdentity const> captureStrokeWidthMenuIdentity() const;
    bool sameStrokeWidthMenuIdentity(StrokeWidthMenuIdentity const &identity) const;
    void openStrokeWidthMenu(bool show = true);
    void rebuildStrokeWidthList();
    void activateStrokeWidthMenu(StrokeWidthIntent const &intent,
                                std::shared_ptr<StrokeWidthMenuIdentity const> const &opening);

    std::unique_ptr<SelectedStyleDropTracker> drop[2];
    bool dropEnabled[2] = {false, false};

private:
    void onDefocus() override;
};

} // namespace UI::Widget

} // namespace Inkscape

#endif // SEEN_INKSCAPE_UI_SELECTED_STYLE_H

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
