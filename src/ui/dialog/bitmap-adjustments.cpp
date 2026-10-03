// SPDX-License-Identifier: GPL-2.0-or-later

#include "bitmap-adjustments.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>

#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerlegacy.h>

#include <gio/gio.h>
// Umbrella header: gtk_widget_observe_controllers()/gtk_event_controller_reset()
// and the GTK_IS_GESTURE()/GTK_EVENT_CONTROLLER() macros are not declared by the
// gtkmm wrappers this file otherwise uses.
#include <gtk/gtk.h>

#include <gdk/gdkevents.h>

#include "desktop.h"
#include "object/sp-image.h"
#include "ui/bitmap-adjustments-controller.h"
#include "ui/util.h"

namespace Inkscape::UI::Dialog {
namespace {

struct ToneDefinition {
    char const *label;
    char const *tooltip;
    double Filters::BitmapToneSettings::*member;
    Filters::BitmapToneProperty property;
};

constexpr std::array<ToneDefinition, 6> tone_definitions = {{
    {N_("Brightness"), N_("Move all tones lighter or darker."),
     &Filters::BitmapToneSettings::brightness, Filters::BitmapToneProperty::Brightness},
    {N_("Contrast"), N_("Increase or reduce separation around middle gray."),
     &Filters::BitmapToneSettings::contrast, Filters::BitmapToneProperty::Contrast},
    {N_("Intensity"), N_("Apply an exposure-like light adjustment."),
     &Filters::BitmapToneSettings::intensity, Filters::BitmapToneProperty::Intensity},
    {N_("Highlights"), N_("Adjust the brightest tonal range."),
     &Filters::BitmapToneSettings::highlights, Filters::BitmapToneProperty::Highlights},
    {N_("Shadows"), N_("Adjust the darkest tonal range."),
     &Filters::BitmapToneSettings::shadows, Filters::BitmapToneProperty::Shadows},
    {N_("Midtones"), N_("Adjust middle tones while protecting the endpoints."),
     &Filters::BitmapToneSettings::midtones, Filters::BitmapToneProperty::Midtones}
}};

// GtkRange drives the slider through range-owned GtkGesture controllers. We only
// observe raw pointer events, so when a drag is canceled we must reset those
// gestures: gtk_event_controller_reset() ends the sequence and the range stops
// applying later pointer motion to the adjustment. Public GTK API only.
void reset_range_gestures(Gtk::Widget &widget)
{
    GListModel *controllers = gtk_widget_observe_controllers(widget.gobj());
    if (!controllers) return;
    auto const count = g_list_model_get_n_items(controllers);
    for (guint i = 0; i < count; ++i) {
        gpointer item = g_list_model_get_item(controllers, i);
        if (!item) continue;
        if (GTK_IS_GESTURE(item)) {
            gtk_event_controller_reset(GTK_EVENT_CONTROLLER(item));
        }
        g_object_unref(item);
    }
    g_object_unref(controllers);
}

// True while the widget still owns a recognized pointer gesture. Used as a
// belt-and-suspenders guard so a canceled drag whose range gesture somehow did
// not stop can never fall through to a synchronous discrete commit.
bool range_gesture_active(Gtk::Widget &widget)
{
    GListModel *controllers = gtk_widget_observe_controllers(widget.gobj());
    if (!controllers) return false;
    auto const count = g_list_model_get_n_items(controllers);
    bool active = false;
    for (guint i = 0; i < count && !active; ++i) {
        gpointer item = g_list_model_get_item(controllers, i);
        if (!item) continue;
        if (GTK_IS_GESTURE(item)) {
            active = gtk_gesture_is_active(GTK_GESTURE(item));
        }
        g_object_unref(item);
    }
    g_object_unref(controllers);
    return active;
}

} // namespace

BitmapAdjustmentsPanel::BitmapAdjustmentsPanel()
    : DialogBase("/dialogs/bitmap-adjustments", "BitmapAdjustments")
{
    set_name("BitmapAdjustmentsPanel");
    buildInterface();
    connectSignals();
    set_defocus_target(this, this);

    auto escape = Gtk::EventControllerKey::create();
    escape->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    escape->signal_key_pressed().connect([this](auto keyval, auto, auto) {
        if (keyval != GDK_KEY_Escape) return false;
        // End the range's own drag gesture as well as our bookkeeping: without
        // this a still-held pointer would keep emitting valuesChanged and the
        // next motion would fall through as a synchronous discrete commit.
        cancelScaleDrag(true);
        syncFromSelection();
        defocus_dialog();
        return true;
    }, true);
    add_controller(escape);
}

BitmapAdjustmentsPanel::~BitmapAdjustmentsPanel()
{
    if (auto ctl = controller()) ctl->cancelPreview();
}

void BitmapAdjustmentsPanel::focus_dialog()
{
    DialogBase::focus_dialog();

    // Native menu activation on macOS can restore focus to the canvas after
    // the action callback. Reassert panel focus on the next UI turn so the
    // adjustment controls are reachable without a pointer.
    Glib::signal_idle().connect_once(
        sigc::mem_fun(*this, &BitmapAdjustmentsPanel::restoreFocusAfterActivation),
        Glib::PRIORITY_HIGH_IDLE);
}

void BitmapAdjustmentsPanel::restoreFocusAfterActivation()
{
    if (!get_mapped()) return;

    if (_before.get_sensitive()) {
        _before.grab_focus();
    } else {
        DialogBase::focus_dialog();
    }
}

void BitmapAdjustmentsPanel::buildInterface()
{
    _scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _scroll.set_propagate_natural_height();
    _scroll.set_child(_content);
    append(_scroll);

    _content.set_margin(8);
    _status.set_halign(Gtk::Align::START);
    _status.set_wrap();
    _status.add_css_class("dim-label");
    _content.append(_status);

    _before.set_label(_("Before"));
    _preview.set_label(_("Preview"));
    _before.set_group(_preview);
    _preview.set_active(true);
    _before.set_hexpand(true);
    _preview.set_hexpand(true);
    _before.set_tooltip_text(
        _("Temporarily show the bitmap without this panel's tone adjustment."));
    _preview.set_tooltip_text(_("Show the current adjustment live on the canvas."));
    _comparison.add_css_class("linked");
    _comparison.append(_before);
    _comparison.append(_preview);
    _content.append(_comparison);

    _histogram_view.set_content_height(104);
    _histogram_view.set_hexpand(true);
    _histogram_view.set_tooltip_text(_("Luminance histogram for the selected bitmap."));
    _histogram_view.set_draw_func(
        sigc::mem_fun(*this, &BitmapAdjustmentsPanel::drawHistogram));
    _content.append(_histogram_view);

    _histogram_status.set_halign(Gtk::Align::START);
    _histogram_status.add_css_class("dim-label");
    _content.append(_histogram_status);

    _clipping_warning.set_label(_("Highlight clipped shadows and highlights"));
    _clipping_warning.set_tooltip_text(
        _("Blue marks clipped shadows; magenta marks clipped highlights. This aid is canvas-only and is never exported."));
    _content.append(_clipping_warning);

    _tone.set_label(_("Tone"));
    _tone.set_expanded(true);
    _tone.set_child(_tone_grid);
    _tone_grid.set_column_spacing(8);
    _tone_grid.set_row_spacing(6);
    _tone_grid.set_margin_top(4);
    _content.append(_tone);

    for (std::size_t i = 0; i < _rows.size(); ++i) {
        auto row = std::make_unique<ToneRow>();
        auto const &definition = tone_definitions[i];
        row->label.set_text(_(definition.label));
        row->label.set_halign(Gtk::Align::START);
        row->label.set_tooltip_text(_(definition.tooltip));
        row->adjustment = Gtk::Adjustment::create(0.0, -100.0, 100.0, 1.0, 10.0);
        row->scale.set_adjustment(row->adjustment);
        row->scale.set_draw_value(false);
        row->scale.set_hexpand(true);
        row->scale.set_tooltip_text(_(definition.tooltip));
        row->spin.set_adjustment(row->adjustment);
        row->spin.set_digits(0);
        row->spin.set_numeric(true);
        row->spin.set_width_chars(5);
        row->spin.set_tooltip_text(_(definition.tooltip));
        row->label.set_mnemonic_widget(row->spin);
        _tone_grid.attach(row->label, 0, static_cast<int>(i), 1, 1);
        _tone_grid.attach(row->scale, 1, static_cast<int>(i), 1, 1);
        _tone_grid.attach(row->spin, 2, static_cast<int>(i), 1, 1);
        _rows[i] = std::move(row);
    }

    _reset.set_label(_("Reset tone"));
    _reset.set_tooltip_text(_("Return all tone controls to their neutral values."));
    _actions.set_halign(Gtk::Align::END);
    _actions.append(_reset);
    _content.append(_actions);
}

void BitmapAdjustmentsPanel::connectSignals()
{
    _before.signal_toggled().connect([this] {
        if (_updating.pending() || !_before.get_active()) return;
        if (auto ctl = controller()) ctl->setPreviewMode(BitmapPreviewMode::Before);
        updateHistogram();
    });
    _preview.signal_toggled().connect([this] {
        if (_updating.pending() || !_preview.get_active()) return;
        if (auto ctl = controller()) ctl->setPreviewMode(BitmapPreviewMode::Preview);
        updateHistogram();
    });
    _clipping_warning.signal_toggled().connect([this] {
        if (_updating.pending()) return;
        if (auto ctl = controller()) ctl->setClippingWarning(_clipping_warning.get_active());
        _histogram_view.queue_draw();
    });
    _reset.signal_clicked().connect(sigc::mem_fun(*this, &BitmapAdjustmentsPanel::resetTone));

    for (std::size_t i = 0; i < _rows.size(); ++i) {
        auto const &row = _rows[i];
        row->adjustment->signal_value_changed().connect(
            [this, i] { valuesChanged(i); });

        // Observe the primary button/touch stream on the scale without claiming
        // the primary sequence. A stateful Gtk::GestureClick can be cancelled by
        // GTK's gesture arbitration without ever emitting released, which would
        // leave a preview-only drag uncommitted. A Gtk::EventControllerLegacy in
        // CAPTURE phase sees the raw events before the internal GtkRange drag
        // gesture (a GTK_PHASE_BUBBLE GtkGesture) and returns false for every
        // primary sequence, so that gesture keeps driving the slider. It returns
        // true only for overlapping begins/releases that belong to some other
        // gesture: a second finger or an emulated-pointer duplicate must not
        // start a competing drag on another row or finalize the active one.
        auto scale_buttons = Gtk::EventControllerLegacy::create();
        scale_buttons->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        auto *scale_widget = &row->scale;
        scale_buttons->signal_event().connect(
            [this, scale_widget](Glib::RefPtr<Gdk::Event const> const &event) {
                auto const type = event->get_event_type();
                auto const sequence = [&]() -> GdkEventSequence * {
                    return gdk_event_get_event_sequence(
                        const_cast<GdkEvent *>(event->gobj()));
                };
                if (type == Gdk::Event::Type::BUTTON_PRESS &&
                    event->get_button() == GDK_BUTTON_PRIMARY) {
                    if (_active_touch) {
                        // Pointer-emulated duplicate of the active touch: drop it
                        // so it neither clears the sparse patch nor starts a
                        // second range drag.
                        return true;
                    }
                    if (_pointer_adjusting) {
                        // One gesture owns the interaction; ignore extra presses.
                        return true;
                    }
                    // Drag in progress: stream previews; commit once on release.
                    _pointer_adjusting = true;
                    _active_scale = scale_widget;
                    _pending_patch = {};
                } else if (type == Gdk::Event::Type::BUTTON_RELEASE &&
                           event->get_button() == GDK_BUTTON_PRIMARY) {
                    if (_active_touch) {
                        // Emulated duplicate release: the matching touch END or
                        // CANCEL owns finalization, so committing here would end
                        // the touch drag prematurely.
                        return true;
                    }
                    if (_pointer_adjusting && scale_widget == _active_scale) {
                        endScaleDrag();
                    }
                } else if (type == Gdk::Event::Type::TOUCH_BEGIN) {
                    if (_pointer_adjusting) {
                        // An active gesture already owns the interaction. Consume
                        // the extra sequence at capture so this scale's GtkRange
                        // cannot start a competing drag that would overwrite the
                        // active gesture's sparse patch.
                        return true;
                    }
                    // WinPointer delivers touch as GDK_TOUCH_* with no parallel
                    // button event, so the range drag would otherwise never
                    // commit. Latch the first sequence: that is the one
                    // GtkRange's gesture sticks to and previews through.
                    _active_touch = sequence();
                    _active_scale = scale_widget;
                    _pointer_adjusting = true;
                    _pending_patch = {};
                } else if (type == Gdk::Event::Type::TOUCH_END) {
                    if (_active_touch && sequence() == _active_touch &&
                        scale_widget == _active_scale) {
                        endScaleDrag();
                    }
                } else if (type == Gdk::Event::Type::TOUCH_CANCEL) {
                    if (_active_touch && sequence() == _active_touch &&
                        scale_widget == _active_scale) {
                        cancelScaleDrag(true);
                        syncFromSelection();
                    }
                } else if (type == Gdk::Event::Type::GRAB_BROKEN) {
                    // The implicit pointer grab was taken away mid-drag. GTK
                    // cancels the internal range drag without a release, so this
                    // must cancel the preview, never commit it. Only the owning
                    // scale may cancel the active gesture.
                    if (_pointer_adjusting && scale_widget == _active_scale) {
                        cancelScaleDrag(true);
                        syncFromSelection();
                    }
                }
                // Primary sequences stay nonconsumed, so the internal GtkRange
                // gesture keeps driving the active drag's preview.
                return false;
            },
            false); // REQUIRED: bool SignalProxy::connect has no default `after`
        row->scale.add_controller(scale_buttons);

        auto spin_focus = Gtk::EventControllerFocus::create();
        spin_focus->signal_leave().connect(
            sigc::mem_fun(*this, &BitmapAdjustmentsPanel::commitValues));
        row->spin.add_controller(spin_focus);
        row->spin.signal_activate().connect(
            sigc::mem_fun(*this, &BitmapAdjustmentsPanel::commitValues));
    }
}

Filters::BitmapToneSettings BitmapAdjustmentsPanel::settingsFromControls() const
{
    Filters::BitmapToneSettings settings;
    for (std::size_t i = 0; i < _rows.size(); ++i) {
        settings.*tone_definitions[i].member = _rows[i]->adjustment->get_value();
    }
    return settings.clamped();
}

void BitmapAdjustmentsPanel::settingsToControls(BitmapAdjustmentSnapshot const &snapshot)
{
    for (std::size_t i = 0; i < _rows.size(); ++i) {
        auto &row = *_rows[i];
        auto const &value = snapshot.tone[i];
        row.mixed = value.mixed;
        if (value.mixed) {
            row.adjustment->set_value(0.0);
            row.spin.set_text("—");
            row.spin.add_css_class("mixed");
            row.scale.add_css_class("mixed");
        } else {
            // set_value() also refreshes the entry text when the underlying
            // adjustment is already zero after a previous mixed state.
            row.spin.set_value(value.value);
            row.spin.remove_css_class("mixed");
            row.scale.remove_css_class("mixed");
        }
    }
}

void BitmapAdjustmentsPanel::valuesChanged(std::size_t index)
{
    if (_updating.pending()) return;
    auto ctl = controller();
    if (!ctl || index >= _rows.size()) return;
    auto &row = *_rows[index];
    row.mixed = false;
    row.spin.remove_css_class("mixed");
    row.scale.remove_css_class("mixed");
    Filters::BitmapTonePatch patch;
    Filters::set_bitmap_tone_patch_property(
        patch, tone_definitions[index].property, row.adjustment->get_value());
    _pending_patch = patch;
    if (!_preview.get_active()) {
        auto block = _updating.block();
        _preview.set_active(true);
    }
    ctl->previewPatch(patch);
    ctl->setPreviewMode(BitmapPreviewMode::Preview);
    _reset.set_sensitive(true);
    updateHistogram();

    if (!_pointer_adjusting && !range_gesture_active(row.scale)) {
        // Discrete (keyboard/action/spin) edits commit synchronously. Live preview
        // is transient, and changing selection cancels it, so a value queued on a
        // deferred timer would be dropped before it could be committed. Pointer
        // drags still preview only and commit once on release via commitValues().
        // The gesture check prevents a canceled drag that GTK has not fully torn
        // down from being mistaken for a discrete edit.
        commitValuesContinuous();
    }
}

void BitmapAdjustmentsPanel::commitValues()
{
    if (_updating.pending()) return;
    if (auto ctl = controller()) {
        // Snapshot the sparse patch before the controller call and clear the member.
        // Committing tone can synchronously re-enter this panel through document or
        // selection signals, and syncFromSelection() clears _pending_patch; the
        // controller also keeps its patch argument by reference. A local copy keeps
        // the exact per-property patch and DocumentUndo key intact regardless.
        auto pending = _pending_patch;
        _pending_patch = {};
        if (!pending.empty()) ctl->commitPatch(pending);
        auto block = _updating.block();
        _clipping_warning.set_active(false);
        _preview.set_active(true);
    }
    syncFromSelection();
}

void BitmapAdjustmentsPanel::commitValuesContinuous()
{
    if (_updating.pending()) return;
    if (auto ctl = controller()) {
        // See commitValues(): snapshot before the reentrant controller call so a
        // nested syncFromSelection() cannot clear or replace the patch being committed.
        auto pending = _pending_patch;
        _pending_patch = {};
        if (!pending.empty()) ctl->commitPatch(pending, true);
        auto block = _updating.block();
        _clipping_warning.set_active(false);
        _preview.set_active(true);
    }
    syncFromSelection();
}

void BitmapAdjustmentsPanel::endScaleDrag()
{
    // Only a drag we observed on BUTTON_PRESS or TOUCH_BEGIN may finalize: a
    // stray release or grab-broken event with no outstanding press must not commit.
    if (!_pointer_adjusting) return;
    _pointer_adjusting = false;
    _active_touch = nullptr;
    _active_scale = nullptr;
    commitValues();
}

void BitmapAdjustmentsPanel::cancelScaleDrag(bool cancel_preview)
{
    // Reset the range-owned gesture controllers first: this ends the internal
    // GtkRange drag so later pointer motion cannot reach valuesChanged at all.
    // Clearing _pointer_adjusting afterwards is therefore not a blind reset of
    // physical state, and keyboard/spin edits still commit synchronously.
    for (auto &row : _rows) {
        reset_range_gestures(row->scale);
    }
    _pointer_adjusting = false;
    _active_touch = nullptr;
    _active_scale = nullptr;
    _pending_patch = {};
    if (cancel_preview) {
        if (auto ctl = controller()) ctl->cancelPreview();
    }
}

void BitmapAdjustmentsPanel::resetTone()
{
    auto ctl = controller();
    if (!ctl) return;
    {
        auto block = _updating.block();
        for (auto &row : _rows) {
            row->mixed = false;
            row->adjustment->set_value(0.0);
            row->spin.remove_css_class("mixed");
            row->scale.remove_css_class("mixed");
        }
        _preview.set_active(true);
    }
    auto const reset = Filters::BitmapTonePatch::all({});
    ctl->previewPatch(reset);
    ctl->commitPatch(reset);
    _pending_patch = {};
    syncFromSelection();
}

void BitmapAdjustmentsPanel::updateHistogram()
{
    if (!_histogram_available) {
        _display_histogram = {};
        return;
    }
    if (_before.get_active()) {
        _display_histogram = _source_histogram;
    } else {
        _display_histogram = Filters::remap_bitmap_histogram(
            _source_histogram, settingsFromControls());
    }

    if (_display_histogram.empty()) {
        _histogram_status.set_text(_("Histogram unavailable"));
    } else {
        auto const shadows = 100.0 * _display_histogram.shadow_clipped /
                             _display_histogram.sampled_pixels;
        auto const highlights = 100.0 * _display_histogram.highlight_clipped /
                                _display_histogram.sampled_pixels;
        _histogram_status.set_text(Glib::ustring::compose(
            _("Clipping: shadows %1%% · highlights %2%%"),
            Glib::ustring::format(std::fixed, std::setprecision(1), shadows),
            Glib::ustring::format(std::fixed, std::setprecision(1), highlights)));
    }
    _histogram_view.queue_draw();
}

void BitmapAdjustmentsPanel::drawHistogram(Cairo::RefPtr<Cairo::Context> const &ctx,
                                           int width, int height)
{
    ctx->set_source_rgba(0.5, 0.5, 0.5, 0.12);
    ctx->rectangle(0, 0, width, height);
    ctx->fill();
    auto const peak = _display_histogram.peak();
    if (!peak || width <= 0 || height <= 0) return;

    auto const bar_width = static_cast<double>(width) / 256.0;
    for (unsigned i = 0; i < 256; ++i) {
        auto const normalized = std::log1p(_display_histogram.luminance[i]) /
                                std::log1p(static_cast<double>(peak));
        auto const bar_height = normalized * (height - 2.0);
        if (_clipping_warning.get_active() && i <= 2) {
            ctx->set_source_rgba(0.0, 0.44, 1.0, 0.95);
        } else if (_clipping_warning.get_active() && i >= 253) {
            ctx->set_source_rgba(1.0, 0.0, 0.69, 0.95);
        } else {
            ctx->set_source_rgba(0.82, 0.82, 0.82, 0.82);
        }
        ctx->rectangle(i * bar_width, height - bar_height,
                       std::max(1.0, bar_width), bar_height);
        ctx->fill();
    }
}

void BitmapAdjustmentsPanel::syncFromSelection()
{
    // A context reset (selection/document/desktop change, Escape, unmap) during
    // an observed pointer drag must cancel that drag: reset the range gestures
    // before dropping bookkeeping so the still-held pointer cannot alter the new
    // selection. An ordinary commit clears _pointer_adjusting first, so the
    // transient preview being committed is never canceled here.
    if (_pointer_adjusting) {
        cancelScaleDrag(true);
    }
    auto scoped = _updating.block();
    auto ctl = controller();
    auto const snapshot = ctl ? ctl->query() : BitmapAdjustmentSnapshot{};
    auto const enabled = snapshot.has_targets;
    _comparison.set_sensitive(enabled);
    _clipping_warning.set_sensitive(enabled);
    _tone.set_sensitive(enabled);
    auto const any_adjustment = std::any_of(snapshot.tone.begin(), snapshot.tone.end(),
        [](auto const &value) { return value.mixed || std::abs(value.value) > 1e-9; });
    _reset.set_sensitive(enabled && any_adjustment);

    if (!snapshot.has_selection) {
        _status.set_visible(true);
        _status.set_text(_("Select objects to adjust."));
    } else if (snapshot.missing) {
        _status.set_visible(true);
        _status.set_text(_("The selected bitmap source is missing."));
    } else if (!snapshot.has_targets) {
        _status.set_visible(true);
        _status.set_text(Glib::ustring::compose(
            _("No editable objects. Missing images: %1; hidden, locked or unavailable objects: %2."),
            snapshot.missing_source_count, snapshot.unavailable_count));
    } else if (snapshot.ignored_count) {
        _status.set_visible(true);
        _status.set_text(Glib::ustring::compose(
            _("Ready to adjust %1 objects. Skipped: %2 missing images, %3 hidden, locked or unavailable objects."),
            snapshot.target_count, snapshot.missing_source_count, snapshot.unavailable_count));
    } else if (snapshot.has_single_bitmap) {
        _status.set_visible(true);
        _status.set_text(Glib::ustring::compose(
            _("%1 × %2 px · %3"), snapshot.pixel_width, snapshot.pixel_height,
            snapshot.embedded ? _("Embedded") : _("Linked")));
    } else {
        _status.set_text({});
        _status.set_visible(false);
    }

    settingsToControls(snapshot);
    _pending_patch = {};
    _before.set_active(false);
    _preview.set_active(true);
    _clipping_warning.set_active(false);
    _source_histogram = {};
    _histogram_available = snapshot.has_single_bitmap;
    _histogram_view.set_visible(_histogram_available);
    _histogram_status.set_visible(_histogram_available);
    _histogram_view.set_sensitive(_histogram_available);
    if (_histogram_available) {
        if (auto image = ctl->targetImage(); image && image->pixbuf) {
            _source_histogram = Filters::build_bitmap_histogram(*image->pixbuf);
        }
    }
    updateHistogram();
}

BitmapAdjustmentsController *BitmapAdjustmentsPanel::controller() const
{
    return getDesktop() ? &getDesktop()->bitmapAdjustmentsController() : nullptr;
}

void BitmapAdjustmentsPanel::update() { syncFromSelection(); }
void BitmapAdjustmentsPanel::desktopReplaced() { syncFromSelection(); }
void BitmapAdjustmentsPanel::documentReplaced() { syncFromSelection(); }
void BitmapAdjustmentsPanel::selectionChanged(Selection *) { syncFromSelection(); }
void BitmapAdjustmentsPanel::selectionModified(Selection *, guint) { syncFromSelection(); }

void BitmapAdjustmentsPanel::on_unmap()
{
    cancelScaleDrag(true);
    DialogBase::on_unmap();
}

} // namespace Inkscape::UI::Dialog
