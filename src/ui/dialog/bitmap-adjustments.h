// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_BITMAP_ADJUSTMENTS_H
#define INKSCAPE_UI_DIALOG_BITMAP_ADJUSTMENTS_H

#include <array>
#include <memory>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/expander.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/togglebutton.h>

#include "display/bitmap-histogram.h"
#include "ui/dialog/dialog-base.h"
#include "ui/operation-blocker.h"

namespace Inkscape::UI {
class BitmapAdjustmentsController;
struct BitmapAdjustmentSnapshot;
}

namespace Inkscape::UI::Dialog {

class BitmapAdjustmentsPanel final : public DialogBase
{
public:
    BitmapAdjustmentsPanel();
    ~BitmapAdjustmentsPanel() final;

    void update() final;
    void focus_dialog() final;

private:
    struct ToneRow {
        Gtk::Label label;
        Glib::RefPtr<Gtk::Adjustment> adjustment;
        Gtk::Scale scale{Gtk::Orientation::HORIZONTAL};
        Gtk::SpinButton spin;
        bool mixed = false;
    };

    void desktopReplaced() final;
    void documentReplaced() final;
    void selectionChanged(Selection *) final;
    void selectionModified(Selection *, guint) final;
    void on_unmap() override;

    void buildInterface();
    void connectSignals();
    void restoreFocusAfterActivation();
    void syncFromSelection();
    void updateHistogram();
    void drawHistogram(Cairo::RefPtr<Cairo::Context> const &, int width, int height);
    void valuesChanged(std::size_t row);
    void commitValues();
    void commitValuesContinuous();
    // Finalizes an in-progress scale drag observed via raw button events and
    // commits the pending preview once; a no-op without an outstanding press.
    void endScaleDrag();
    // Ends any in-progress GtkRange pointer drag and drops our drag bookkeeping.
    // Resets the range-owned gesture controllers so later pointer motion cannot
    // keep driving the adjustment after the drag was canceled.
    void cancelScaleDrag(bool cancel_preview);
    void resetTone();
    Filters::BitmapToneSettings settingsFromControls() const;
    void settingsToControls(BitmapAdjustmentSnapshot const &);
    BitmapAdjustmentsController *controller() const;

    Gtk::ScrolledWindow _scroll;
    Gtk::Box _content{Gtk::Orientation::VERTICAL, 8};
    Gtk::Label _status;
    Gtk::Box _comparison{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::ToggleButton _before;
    Gtk::ToggleButton _preview;
    Gtk::DrawingArea _histogram_view;
    Gtk::Label _histogram_status;
    Gtk::CheckButton _clipping_warning;
    Gtk::Expander _tone;
    Gtk::Grid _tone_grid;
    std::array<std::unique_ptr<ToneRow>, 6> _rows;
    Gtk::Box _actions{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Button _reset;

    Filters::BitmapHistogram _source_histogram;
    Filters::BitmapHistogram _display_histogram;
    Filters::BitmapTonePatch _pending_patch;
    bool _histogram_available = false;
    bool _pointer_adjusting = false;
    // Single active gesture (a mouse drag or the first touch sequence), tracked
    // with the scale that owns it. _active_touch is the latched touch sequence
    // for a touch-driven gesture, null for a mouse-driven one. Overlapping
    // begins are consumed in capture so no other scale's GtkRange can start a
    // competing drag, and only this owner pair may commit or cancel the gesture.
    GdkEventSequence *_active_touch = nullptr;
    Gtk::Widget *_active_scale = nullptr;
    OperationBlocker _updating;
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_BITMAP_ADJUSTMENTS_H
