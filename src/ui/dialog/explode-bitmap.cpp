// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/dialog/explode-bitmap.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <limits>
#include <sstream>
#include "inkscape-window.h"
#include "ui/widget/canvas.h"
#include "ui/explode-bitmap-panel-preparation.h"
#include "util/scope_exit.h"
#include <gdkmm/frameclock.h>
#include <gtkmm/picture.h>
#include <gtkmm/adjustment.h>
#include <glibmm/i18n.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerlegacy.h>
#include "desktop.h"
#include "ui/bitmap-adjustments-controller.h"
#include "document.h"
#include "display/cairo-utils.h"
#include "display/drawing-image.h"
#include "display/control/canvas-item-buffer.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "selection.h"
#include "style.h"
#include "ui/explode-bitmap-publication.h"
#include "xml/node.h"
namespace Inkscape::UI::Dialog {
using namespace Bitmap;
using namespace Bitmap::PanelPreparation;
namespace {
// Main-thread process-session defaults; no document XML or persistent preferences.
ContourRecipe sessionContours;
ContourStyle sessionContourStyle;
constexpr unsigned fieldMax[] = {255, 127, 25};
constexpr double contourMin[] = {-10, 0, 0}, contourMax[] = {10, 100, 5};
double &contourValue(ContourRecipe &recipe, unsigned i)
{
    return i == 0 ? recipe.offsetMm : i == 1 ? recipe.smoothing : recipe.gapToleranceMm;
}
unsigned &fieldValue(SessionRecipe &recipe, unsigned i)
{
    return i == 0 ? recipe.threshold : i == 1 ? recipe.softness : recipe.faintFloor;
}
constexpr char const *floorTooltip = N_("Ignore pixels at or below this opacity: 0–25%. Applies before refinement and also when Refine transparency is off.");
constexpr char const *previewNote = N_("Preview only. Apply adjustment or Explode to keep these changes.");
constexpr char const *detailedPreviewNote = N_("Detailed preview is unavailable. The canvas shows the current image without this preview.");
constexpr char const *bakedNote = N_("Adjustment applied. Further adjustments change these pixels again. Use Undo to restore the previous image.");
constexpr char const *tooMany = N_("%1 pieces found. Explode Bitmap supports up to 150 pieces per operation. Adjust the transparency settings or simplify the image.");
constexpr char const *tooManyBound = N_("More than 20,000 pieces found. Explode Bitmap supports up to 150 pieces per operation. Adjust the transparency settings or simplify the image.");
constexpr char const *failure = N_("Could not complete the operation. No changes were made. Click Analyze to retry.");
constexpr char const *memoryFailure = N_("Not enough memory to complete this operation. No changes were made.");
constexpr char const *platformFailure = N_("Explode Bitmap is unavailable for this image in this build. No changes were made.");
constexpr char const *effectsFailure = N_("This image has effects, clipping or geometry that Explode Bitmap cannot process. No changes were made.");
constexpr char const *toneConflict = N_("Accept or cancel the tone preview in Bitmap Adjustments before applying, resizing or exploding.");
bool wholeNumber(Glib::ustring const &text, unsigned min, unsigned max, unsigned &value)
{
    auto const &raw = text.raw();
    auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == raw.data() + raw.size() && value >= min && value <= max;
}
// Use the user's numeric locale, without changing the process-wide C++ locale.
std::locale const &numericLocale()
{
    static auto locale = [] { try { return std::locale(""); } catch (...) { return std::locale::classic(); } }();
    return locale;
}
bool contourNumber(Glib::ustring const &text, unsigned i, double &value)
{
    if (i == 1) {
        unsigned whole = 0;
        if (!wholeNumber(text, 0, 100, whole)) return false;
        value = whole; return true;
    }
    std::istringstream stream(text.raw()); stream.imbue(numericLocale());
    stream >> std::noskipws >> value;
    auto units = i == 1 ? 1.0 : 10.0;
    return !stream.fail() && stream.eof() && std::isfinite(value) &&
           value >= contourMin[i] && value <= contourMax[i] &&
           std::abs(value * units - std::round(value * units)) < 1e-9;
}
Glib::ustring contourError(unsigned i)
{
    return Glib::ustring::compose(i == 1 ? _("Enter a whole number from %1 to %2.")
        : _("Enter a number from %1 to %2."), int(contourMin[i]), int(contourMax[i]));
}
std::string number(double value, unsigned precision = 0)
{
    std::ostringstream stream; stream.imbue(numericLocale());
    stream << std::fixed << std::setprecision(precision) << value;
    auto text = stream.str();
    if (precision) {
        while (text.back() == '0') text.pop_back();
        if (text.back() == std::use_facet<std::numpunct<char>>(numericLocale()).decimal_point()) text.pop_back();
    }
    return text;
}
std::string area(double value)
{
    return value > 0 && value < .01 ? "<" + number(.01, 2) : number(value, value >= 1 ? 1 : 2);
}
std::string percent(double value)
{
    return value > 0 && value < .1 ? "<" + number(.1, 1) : number(value, 1);
}
Glib::ustring sizeText(std::uint64_t bytes)
{
    if (!bytes) return _("0 kB");
    if (bytes < 1000) return _("<1 kB");
    auto formatted = g_format_size(bytes); Glib::ustring text(formatted); g_free(formatted); return text;
}
Glib::ustring failureText(Outcome const &outcome)
{
    // Shared memory failures already carry a gettext-formatted limit/need/available message.
    if (outcome.insufficientMemory) return Glib::ustring(outcome.diagnostic);
    auto diagnosticText = Glib::ustring(outcome.diagnostic).lowercase().raw();
    std::string_view diagnostic(diagnosticText);
    if (diagnostic.find("memory") != diagnostic.npos || diagnostic.find("allocation") != diagnostic.npos ||
        diagnostic.find("budget") != diagnostic.npos || diagnostic.find("ram") != diagnostic.npos ||
        diagnostic.find("headroom") != diagnostic.npos) return _(memoryFailure);
    if (diagnostic.find("platform") != diagnostic.npos || diagnostic.find("qualified") != diagnostic.npos ||
        diagnostic.find("observation") != diagnostic.npos) return _(platformFailure);
    if (diagnostic == "resize resolution no longer fits.")
        return _("The resize resolution no longer fits. Select the image again to recalculate it.");
    if (diagnostic.find("limit") != diagnostic.npos || diagnostic.find("work") != diagnostic.npos || diagnostic.find("expansion") != diagnostic.npos)
        return _("This image is too complex to process safely. No changes were made.");
    return _(failure);
}

}
ExplodeBitmapPanel::ExplodeBitmapPanel() : ExplodeBitmapPanel(Options{}) {}
ExplodeBitmapPanel::ExplodeBitmapPanel(Options options)
    : DialogBase("/dialogs/explode-bitmap", "ExplodeBitmap"), _options(options),
      _jobs([this](Ticket t, JobResult r) { receive(t, std::move(r)); }, [this](Ticket t, JobProgress p) {
          if (active() && t == _ticket && !_publicationPending && !_publishing) progress(p);
      }, options.now, options.automatic)
{
    _outlineVisibility = _overlay.connectVisibility([this](auto state) { outlineStatus(state); });
    _recipe = query({}); // Session defaults also apply before an image is analyzed.
    if (_options.rememberContours) { _contourRecipe = sessionContours; _contourStyle = sessionContourStyle; }
    _name = _("Explode Bitmap"); set_spacing(0);
    _scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    _scroll.set_vexpand(true); _scroll.set_child(_content); append(_scroll);
    _content.set_margin(8); _content.set_valign(Gtk::Align::START);
    for (auto label : {&_status, &_notice, &_details, &_metadata, &_resizeDescription}) {
        label->set_wrap(true); label->set_wrap_mode(Pango::WrapMode::WORD_CHAR); label->set_xalign(0);
        label->set_hexpand(true);
    }
    _status.add_css_class("dim-label"); _content.append(_status); _content.append(_progress);
    _before.set_label(_("Before")); _previewToggle.set_label(_("Preview"));
    _before.set_group(_previewToggle); _previewToggle.set_active(true);
    _before.set_hexpand(true); _previewToggle.set_hexpand(true);
    _before.set_tooltip_text(_("Show the current image without this preview. Applied adjustments remain applied."));
    _previewToggle.set_tooltip_text(_("Show the transparency preview and available piece outlines on the canvas."));
    _comparison.add_css_class("linked"); _comparison.set_homogeneous(true);
    _comparison.append(_before); _comparison.append(_previewToggle); _content.append(_comparison);
    _before.signal_toggled().connect([this] { if (!_updating) original(_before.get_active()); });
    _fit.set_label(_("Fit image")); _zoom.set_label(_("Smallest piece"));
    _fit.set_tooltip_text(_("Fit the selected image in the canvas. Use the canvas zoom controls or Ctrl+wheel to zoom; middle-drag to pan."));
    _zoom.set_tooltip_text(_("Fit the smallest piece in the canvas. Use the canvas zoom controls or Ctrl+wheel to zoom; middle-drag to pan."));
    _navigation.set_halign(Gtk::Align::START); _navigation.append(_fit); _navigation.append(_zoom); _content.append(_navigation);
    _transparency.set_label(_("Transparency")); _transparency.set_expanded(true);
    _transparency.set_child(_transparencyContent); _content.append(_transparency);
    _contour.set_label(_("Contour")); _contour.set_expanded(_contourRecipe.enabled);
    _contour.set_child(_contourContent); _content.append(_contour);
    _addContour.set_label(_("Add contour to each piece")); _addContour.set_active(_contourRecipe.enabled);
    _contourContent.append(_addContour); _contourContent.append(_contourParameters);
    _contourParameters.set_column_spacing(4); _contourParameters.set_row_spacing(6);
    char const *contourLabels[] = {N_("Offset (mm)"), N_("Smoothing (%)"), N_("Ignore holes and gaps under (mm)")};
    char const *contourTips[] = {N_("Positive grows the contour outward (cut line or white-ink border); negative insets it."),
        N_("Smoothing (%)"), N_("Holes and gaps narrower than this are closed before tracing, so a fine mesh traces as solid. The bitmap is not changed.")};
    double values[] = {_contourRecipe.offsetMm, _contourRecipe.smoothing, _contourRecipe.gapToleranceMm};
    for (unsigned i = 0; i < 3; ++i) {
        auto &label = _contourLabels[i]; auto &spin = _contourSpins[i]; auto &scale = _contourSliders[i];
        label.set_text(_(contourLabels[i])); label.set_xalign(0); label.set_wrap(true);
        label.set_wrap_mode(Pango::WrapMode::WORD); label.set_max_width_chars(8); label.set_mnemonic_widget(spin);
        _contourAdjustments[i] = Gtk::Adjustment::create(values[i], contourMin[i], contourMax[i], i == 1 ? 1 : .1, i == 1 ? 10 : 1);
        spin.set_adjustment(_contourAdjustments[i]); spin.set_digits(i == 1 ? 0 : 1);
        spin.set_numeric(true); spin.set_width_chars(4); spin.set_update_policy(Gtk::SpinButton::UpdatePolicy::IF_VALID);
        scale.set_adjustment(_contourAdjustments[i]); scale.set_draw_value(false); scale.set_round_digits(i == 1 ? 0 : 1); scale.set_hexpand(true); scale.set_size_request(80, -1);
        for (Gtk::Widget *widget : std::initializer_list<Gtk::Widget *>{&label, &spin, &scale}) widget->set_tooltip_text(_(contourTips[i]));
        _contourParameters.attach(label, 0, i); _contourParameters.attach(scale, 1, i); _contourParameters.attach(spin, 2, i);
        spin.signal_input().connect([this, i](double &value) {
            return contourNumber(_contourSpins[i].get_text(), i, value) ? 1 : GTK_INPUT_ERROR;
        }, false);
        spin.signal_output().connect([this, i] {
            double value = 0;
            return !_updating && _contourDirty[i] && !contourNumber(_contourSpins[i].get_text(), i, value);
        }, false);
        spin.signal_changed().connect([this, i] {
            if (!_updating) {
                _contourDirty[i] = true;
                double value = 0;
                if (!contourNumber(_contourSpins[i].get_text(), i, value)) rejectContour(i);
            }
        });
        spin.signal_activate().connect([this, i] { acceptContour(i); });
        auto focus = Gtk::EventControllerFocus::create();
        focus->signal_enter().connect([this, i] { _contourFocused[i] = true; _contourSpins[i].set_numeric(false); });
        focus->signal_leave().connect([this, i] {
            _contourFocused[i] = false; _contourSpins[i].set_numeric(true);
            if (!_updating && !acceptContour(i)) _contourSpins[i].grab_focus();
        });
        spin.add_controller(focus);
        _contourAdjustments[i]->signal_value_changed().connect([this, i] {
            if (_updating) return;
            double value = 0;
            if (_contourDirty[i] && !contourNumber(_contourSpins[i].get_text(), i, value)) { rejectContour(i); return; }
            auto units = i == 1 ? 1.0 : 10.0;
            value = std::round(_contourAdjustments[i]->get_value() * units) / units;
            _updating = true;
            _contourAdjustments[i]->set_value(value);
            _contourSpins[i].set_text(number(value, i == 1 ? 0 : 1));
            _updating = false; _contourDirty[i] = false;
            contourEdited();
        });
    }
    _contourColorLabel.set_text(_("Contour color")); _contourColorLabel.set_xalign(0);
    _contourColorLabel.set_wrap(true); _contourColorLabel.set_max_width_chars(8);
    _contourColor.set_rgba(Gdk::RGBA(_contourStyle.stroke)); _contourColor.set_use_alpha(false);
    _contourColor.set_halign(Gtk::Align::START); _contourColor.set_hexpand(false); _contourColor.set_size_request(48, -1);
    _contourParameters.attach(_contourColorLabel, 0, 3); _contourParameters.attach(_contourColor, 1, 3, 2);
    _contourColor.signal_color_set().connect([this] {
        auto c = _contourColor.get_rgba();
        char color[8]; g_snprintf(color, sizeof(color), "#%02x%02x%02x", unsigned(std::lround(c.get_red()*255)),
            unsigned(std::lround(c.get_green()*255)), unsigned(std::lround(c.get_blue()*255)));
        _contourStyle.stroke = color;
        if (_options.rememberContours) sessionContourStyle = _contourStyle;
        _contourOverlay.clear(); installPreview();
    });
    _createContour.set_label(_("Create contour only"));
    _createContour.set_tooltip_text(_("Add a contour around this bitmap without splitting it. One Undo step."));
    _contourEnd.set_halign(Gtk::Align::END); _contourEnd.append(_createContour); _contourContent.append(_contourEnd);
    _createContour.signal_clicked().connect(sigc::mem_fun(*this, &ExplodeBitmapPanel::createContourOnly));
    _addContour.signal_toggled().connect([this] { if (!_updating) contourEdited(true); });
    _contour.property_expanded().signal_changed().connect([this] {
        if (_contourRecipe.enabled && !_contour.get_expanded()) _contour.set_expanded(true);
    });
    _refine.set_label(_("Refine transparency")); _refine.set_active(true);
    _refine.set_tooltip_text(_("Adjust transparency using Alpha threshold and Edge softness."));
    _transparencyContent.append(_refine); _transparencyContent.append(_parameters);
    _parameters.set_column_spacing(4); _parameters.set_row_spacing(6); _parameters.set_margin_top(4);
    char const *labels[] = {N_("Alpha threshold"), N_("Edge softness"), N_("Ignore pixels up to (%)")};
    char const *tips[] = {N_("Alpha threshold: 0–255. Sets the alpha cutoff for refinement."),
                         N_("Edge softness: 0–127 alpha levels. Softens the transition around the threshold."), floorTooltip};
    for (unsigned i = 0; i < 3; ++i) {
        _labels[i].set_text(_(labels[i])); _labels[i].set_xalign(0);
        _labels[i].set_wrap(true); _labels[i].set_wrap_mode(Pango::WrapMode::WORD);
        _labels[i].set_max_width_chars(8); _labels[i].set_mnemonic_widget(_spins[i]);
        _adjustments[i] = Gtk::Adjustment::create(fieldValue(_recipe, i), 0, fieldMax[i], 1, i == 2 ? 5 : 10);
        _sliders[i].set_adjustment(_adjustments[i]); _sliders[i].set_draw_value(false); _sliders[i].set_hexpand(true);
        _sliders[i].set_size_request(80, -1);
        _spins[i].set_adjustment(_adjustments[i]); _spins[i].set_digits(0); _spins[i].set_numeric(true);
        _spins[i].set_width_chars(3); _spins[i].set_update_policy(Gtk::SpinButton::UpdatePolicy::IF_VALID);
        for (Gtk::Widget *widget : std::initializer_list<Gtk::Widget *>{&_labels[i], &_sliders[i], &_spins[i]}) widget->set_tooltip_text(_(tips[i]));
        _parameters.attach(_labels[i], 0, i); _parameters.attach(_sliders[i], 1, i); _parameters.attach(_spins[i], 2, i);
        // GTK's IF_VALID still emits output on rejection. Keep the raw text, so
        // decimals, negatives and overflow cannot be rounded into authorization.
        _spins[i].signal_input().connect([this, i](double &value) {
            unsigned parsed = 0;
            if (!wholeNumber(_spins[i].get_text(), 0, fieldMax[i], parsed)) return GTK_INPUT_ERROR;
            value = parsed; return 1;
        }, false);
        _spins[i].signal_output().connect([this, i] {
            unsigned value = 0;
            return !_updating && _dirty[i] && !wholeNumber(_spins[i].get_text(), 0, fieldMax[i], value);
        }, false);
        // Observe raw capture events: GtkRange claims its gesture and denies a
        // separate GestureClick, which then never receives released.
        auto pointer = Gtk::EventControllerLegacy::create(); pointer->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        pointer->signal_event().connect([this](auto const &event) {
            auto e = const_cast<GdkEvent *>(event->gobj());
            switch (gdk_event_get_event_type(e)) {
                case GDK_BUTTON_PRESS: if (gdk_button_event_get_button(e) == 1) drag(true); break;
                case GDK_BUTTON_RELEASE: if (gdk_button_event_get_button(e) == 1) drag(false); break;
                case GDK_TOUCH_BEGIN: drag(true); break;
                case GDK_TOUCH_END: drag(false); break;
                case GDK_TOUCH_CANCEL: case GDK_GRAB_BROKEN: drag(false, true); break;
                default: break;
            }
            return false;
        }, false);
        _sliders[i].add_controller(pointer);
        _spins[i].signal_changed().connect([this, i] { if (!_updating) _dirty[i] = true; });
        _spins[i].signal_activate().connect([this, i] { accept(i); });
        auto focus = Gtk::EventControllerFocus::create();
        // Numeric is the resting spin policy. While editing, allow literal text:
        // GTK's digits=0 filter otherwise silently drops a typed decimal point
        // (2.5 becomes 25). Strict input/output validation owns normalization.
        focus->signal_enter().connect([this, i] { _focused[i] = true; _spins[i].set_numeric(false); });
        focus->signal_leave().connect([this, i] {
            _focused[i] = false; _spins[i].set_numeric(true);
            if (!_updating && !accept(i)) _spins[i].grab_focus();
        });
        _spins[i].add_controller(focus);
        _adjustments[i]->signal_value_changed().connect([this, i] {
            if (_updating) return;
            unsigned value = 0;
            if (_dirty[i] && !wholeNumber(_spins[i].get_text(), 0, fieldMax[i], value)) { accept(i); return; }
            _spins[i].set_text(std::to_string(int(_adjustments[i]->get_value()))); accept(i);
        });
    }
    _refine.signal_toggled().connect([this] { if (!_updating) { _recipe.refine = _refine.get_active(); edited(); } });
    _applyRow.set_halign(Gtk::Align::END); _applyRow.append(_apply); _transparencyContent.append(_applyRow);
    _detailsExpander.set_label(_("Details")); _detailsExpander.set_expanded(false);
    _detailsExpander.set_child(_detailsContent); _content.append(_detailsExpander);
    _details.add_css_class("dim-label"); _notice.add_css_class("dim-label");
    _detailsContent.append(_details); _detailsContent.append(_notice);
    _metadata.add_css_class("dim-label");
    _metadata.set_text(_("Export hints, title and description will not be copied to pieces. Set export settings on the new pieces."));
    _detailsContent.append(_metadata);
    _proxy.set_size_request(128, 96); _proxy.set_visible(false); _detailsContent.append(_proxy);
    _resizeResolution.set_text(_("Resolution:")); _resizeResolution.set_xalign(0); _resizeUnit.set_text(_("dpi"));
    _resizeDpi.set_adjustment(Gtk::Adjustment::create(96, 1, BitmapCopyOptions::max_dpi, 1, 10));
    _resizeDpi.set_digits(0); _resizeDpi.set_numeric(true); _resizeDpi.set_width_chars(5);
    _resizeDpi.set_update_policy(Gtk::SpinButton::UpdatePolicy::IF_VALID);
    _resizeDpi.signal_input().connect([this](double &value) {
        unsigned parsed = 0;
        if (!wholeNumber(_resizeDpi.get_text(), 1, _resizeDpi.get_adjustment()->get_upper(), parsed)) return GTK_INPUT_ERROR;
        value = parsed; return 1;
    }, false);
    _resizeDpi.signal_output().connect([this] {
        unsigned value = 0;
        return !_updating && !wholeNumber(_resizeDpi.get_text(), 1, _resizeDpi.get_adjustment()->get_upper(), value);
    }, false);
    auto dpiFocus = Gtk::EventControllerFocus::create();
    dpiFocus->signal_enter().connect([this] { _resizeDpi.set_numeric(false); });
    dpiFocus->signal_leave().connect([this] { _resizeDpi.set_numeric(true); if (!_updating) { _resizeDpi.update(); resizeDetails(); } });
    _resizeDpi.add_controller(dpiFocus);
    _resizeDpi.signal_value_changed().connect([this] { if (!_updating) resizeDetails(); });
    _resizeAntialias.set_label(_("Anti-aliasing")); _resizeAntialias.set_active(true);
    _resizeGrid.set_column_spacing(8); _resizeGrid.set_row_spacing(6);
    _resizeGrid.attach(_resizeResolution, 0, 0); _resizeGrid.attach(_resizeDpi, 1, 0); _resizeGrid.attach(_resizeUnit, 2, 0);
    _resizeControls.append(_resizeGrid); _resizeControls.append(_resizeAntialias); _resizeControls.append(_resizeDescription);
    _content.append(_resizeControls);
    _apply.set_label(_("Apply adjustment")); _cancel.set_label(_("Cancel"));
    _apply.set_tooltip_text(_("Bake the transparency adjustment into this image. Undo restores the previous image."));
    _cancel.set_tooltip_text(_("End this session and discard unapplied changes. Applied changes remain; use Undo to restore them. With no session, close this panel."));
    _footer.set_margin(8); _footerSpace.set_hexpand(true); _primary.add_css_class("suggested-action");
    _footer.append(_cancel); _footer.append(_footerSpace); _footer.append(_primary); append(_footer);
    _apply.signal_clicked().connect([this] { activate(Intent::ApplyAdjustment); });
    _primary.signal_clicked().connect([this] {
        if (_state == State::Idle || _state == State::Stale || (_state == State::Failed && !_result.payload)) analyze();
        else if (_state == State::Resize) resize();
        else activate(Intent::Explode);
    });
    _cancel.signal_clicked().connect([this] {
        if (_state == State::Idle) { stop(); set_visible(false); }
        else endSession(eligible() ? _("Analysis canceled. Unapplied changes were discarded. Click Analyze to start again.") : Glib::ustring{});
    });
    _fit.signal_clicked().connect([this] {
        if (auto desktop = getDesktop(); desktop && eligible()) {
            if (auto window = desktop->getInkscapeWindow()) window->activate_action("canvas-zoom-selection");
            desktop->getCanvas()->grab_focus();
        }
    });
    _zoom.signal_clicked().connect([this] {
        if (!current() || !_result.payload) return;
        auto const &out = static_cast<Output const &>(*_result.payload);
        auto result = _overlay.zoomToPiece(out.smallest, _viewGeneration);
        // Encoded crops retain the same final-grid bounds, with a one-pixel
        // gutter. Reuse them when optional outline storage has been retired.
        if (!result.ok() && out.smallest < out.pieces.count()) {
            auto const &piece = out.pieces.piece(out.smallest);
            auto bounds = Geom::Rect(Geom::Point(piece.x + 1, piece.y + 1),
                                     Geom::Point(piece.x + piece.width - 1, piece.y + piece.height - 1));
            auto const &m = out.grid.pixelToDocument;
            bounds *= Geom::Affine(m[0], m[1], m[2], m[3], m[4], m[5]) * getDesktop()->doc2dt();
            getDesktop()->set_display_area(bounds, 32);
        }
        getDesktop()->getCanvas()->grab_focus();
    });
    _keys = Gtk::EventControllerKey::create(); _keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    _keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType m) { return key(k, m); }, false);
    add_controller(_keys); display(State::Idle);

}
ExplodeBitmapPanel::~ExplodeBitmapPanel() { _outlineVisibility.disconnect(); _sessionWatch.disconnect(); *_alive = false; _mapped = false; stop(); _jobs.close(); }
void ExplodeBitmapPanel::stop()
{
    cancelPublication(); _contourOnlyPublication = false; _deliveryTicket = 0;
    _jobs.cancel(_ticket); _preview.reset(); _overlay.clear(); _contourOverlay.clear(); _contribution = {};
    _proxy.set_paintable({}); _proxy.set_visible(false);
    _result = {}; _prepared = {}; _contourResult = {}; _contourAdmission = {}; _contourJob = false; _drawingRefused = false;
    if (_identity) invalidateSessionJobs(_identity);
}
bool ExplodeBitmapPanel::active() { return _mapped && get_mapped() && get_visible() && getShowing(); }
void ExplodeBitmapPanel::on_map()
{
    // Base mapping calls update/setDesktop; suppress their intermediate refreshes.
    _mapped = false; DialogBase::on_map(); _mapped = true; refresh();
}
void ExplodeBitmapPanel::on_unmap() { _mapped = false; _dragging = false; endSession(); DialogBase::on_unmap(); }
void ExplodeBitmapPanel::desktopReplaced()
{
    endSession(); _dependencies.reset(); _activation->retry();
    if (getDesktop()) _dependencies = std::make_unique<DependencyLease>(*getDesktop(), _options.evidence);
    refresh();
}
bool ExplodeBitmapPanel::sameSelection() const
{
    if (!getDocument() || !getSelection() || !_identity) return false;
    auto image = cast<SPImage>(getSelection()->singleItem());
    return image && logicalImageIdentity(*image) == _identity;
}
void ExplodeBitmapPanel::endSession(Glib::ustring const &message)
{
    _sessionWatch.disconnect(); _session = false; ++_sessionGeneration;
    _dragging = false; stop(); _identity = {}; _applyMessage.clear(); resetComparison();
    _idleMessage = message; display(State::Idle, _idleMessage);
}
void ExplodeBitmapPanel::stale()
{
    if (!_session) return;
    bool resize = _state == State::Resize || _resizing;
    auto image = getSelection() ? cast<SPImage>(getSelection()->singleItem()) : nullptr;
    bool viewUnavailable = image && getDesktop() && !image->viewDependencyStamp(getDesktop()->dkey).available();
    _sessionWatch.disconnect(); _session = false; ++_sessionGeneration;
    _dragging = false; stop(); resetComparison();
    display(State::Stale, getDesktop() && !compatibleExplodeBitmapTool(getDesktop()->getTool())
        ? _("Leave text editing, then click Analyze again.")
        : resize ? _("Image changed. Click Analyze to update the resize options.")
        : viewUnavailable ? _("Image preview is unavailable. Click Analyze when the image is available again.")
        : _("Image changed. Click Analyze to update the preview and piece count."));
}
void ExplodeBitmapPanel::selectionChanged(Selection *)
{
    if (!active() || _publishing) return;
    if (_identity && !sameSelection()) {
        endSession(eligible() ? _("Selection changed. Click Analyze to inspect this image.") : Glib::ustring{});
    } else if (!_identity) { _idleMessage.clear(); display(State::Idle); }
    else refresh();
}
void ExplodeBitmapPanel::selectionModified(Selection *, guint)
{
    if (!active() || _publishing) return;
    if (!_session && _state != State::Idle && _state != State::Stale && _state != State::Failed) { endSession(); return; }
    if (_state == State::Idle) _idleMessage.clear();
    refresh();
}
void ExplodeBitmapPanel::refresh()
{
    if (!active() || _publishing) return;
    if (_identity && !sameSelection()) {
        endSession(eligible() ? _("Selection changed. Click Analyze to inspect this image.") : Glib::ustring{});
    } else if (_session) {
        if (!current()) stale();
    } else if (_state == State::Idle) display(State::Idle, _idleMessage);
    else if (_state == State::Stale) display(State::Stale, _status.get_text());
}
void ExplodeBitmapPanel::analyze()
{
    if (!active() || _publishing || _publicationPending || !eligible()) return;
    if (_state != State::Idle && _state != State::Stale && _state != State::Failed) return;
    stop(); resetComparison(); _applyMessage.clear(); _idleMessage.clear(); _activation->retry();
    _identity = logicalImageIdentity(*cast<SPImage>(getSelection()->singleItem()));
    _session = true; ++_sessionGeneration;
    request();
    // The existing lease has no notification API. Coalesce its conservative
    // document/native/view generations on the main loop only while authorized.
    // Idle/Stale never inspect captured graphs or allocate analysis inputs.
    _sessionWatch.disconnect();
    if (_session) _sessionWatch = Glib::signal_timeout().connect([this] {
        refresh(); return _session;
    }, 50);
}
void ExplodeBitmapPanel::edited()
{
    if (!_session || !sameSelection() || !current()) { refresh(); return; }
    _recipe.bypassAlpha = false;
    if (!_dragging) remember(_identity, _recipe, _recipe.refine);
    _activation->retry(); stop(); request();
}
void ExplodeBitmapPanel::contourEdited(bool toggle)
{
    _contourRecipe = {_addContour.get_active(), _contourAdjustments[0]->get_value(),
                      _contourAdjustments[2]->get_value(), _contourAdjustments[1]->get_value()};
    if (_options.rememberContours) sessionContours = _contourRecipe;
    if (_contourRecipe.enabled) _contour.set_expanded(true);
    _contourOverlay.clear(); _prepared.contours = nullptr; _contourResult = {};
    _contourParameters.set_sensitive(_addContour.get_sensitive() && _contourRecipe.enabled);
    if (!_session || !sameSelection() || !current()) { refresh(); return; }
    if (!_contourRecipe.enabled) {
        // Retire an in-flight contour-only job, but keep the exact PNG analysis.
        // An already-running full analysis may finish; its contours are ignored.
        auto state = _contourJob ? _contourBaseState : _state;
        auto detail = state == State::Failed ? _status.get_text() : Glib::ustring{};
        if (_contourJob) { _jobs.cancel(_ticket); _deliveryTicket = 0; _contourJob = false; }
        display(state, detail);
        if (_result.payload && state != State::TooMany) installPreview();
        return;
    }
    if (toggle) { _activation->retry(); stop(); request(); }
    else requestContours();
}
void ExplodeBitmapPanel::requestContours() try
{
    auto out = _result.payload ? &static_cast<Output const &>(*_result.payload) : nullptr;
    if (!out || !out->analysis || out->analysis->identity != analysisIdentity(out->grid, _prepared.target)) {
        _activation->retry(); stop(); request(); return;
    }
    _jobs.cancel(_ticket); _deliveryTicket = 0;
    _overlay.clear(); _contourOverlay.clear(); _preview.reset();
    if (!recaptureView()) { stale(); return; }
    auto refused = [this](Outcome outcome) {
        _contourJob = false; _contourResult = {}; _contourResult.outcome = outcome;
        display(State::Ready); installPreview();
    };
    auto memory = _options.memory ? sampleMemory(*_options.memory) : sampleMemory();
    if (!memory.ok()) { refused(memory.outcome); return; }
    auto admission = admit(contourResources(_budget->reserved()), memory.value);
    if (!admission.ok()) { refused(admission.outcome); return; }
    JobInput job; job.storage.budget = _budget;
    auto check = _budget->acquire(Stage::input, sizeof(ContourInput) + 64, job.storage.reservation);
    if (!check.ok()) { refused(check); return; }
    auto input = std::make_shared<ContourInput>(); input->analysis = out->analysis;
    input->expectedIdentity = analysisIdentity(out->grid, _prepared.target); input->contour = _contourRecipe; input->observer = _options.observer;
    job.storage.payload = std::move(input); job.pixels = std::uint64_t(out->grid.width)*out->grid.height;
    job.stage = CliBitmapStage::Contour;
    job.work = _options.contourWork ? _options.contourWork : calculateContours;
    if (!_contourJob) _contourBaseState = _state;
    _activation->retry(); _contourJob = true; _consumed = 0;
    _dispatchGeneration = _sessionGeneration;
    _ticket = _deliveryTicket = _jobs.request(std::move(job));
    display(State::Counting, Glib::ustring::compose(_("Tracing contours… %1%% · Esc cancels"), 0));
}
catch (...) {
    // Retire typed delivery even if allocating the progress UI failed after dispatch.
    _jobs.cancel(_ticket); _deliveryTicket = 0; _contourJob = false;
    _contourResult.outcome = Outcome{Status::unavailable, "Contour allocation failed"}; display(State::Ready);
}
void ExplodeBitmapPanel::contourStatus()
{
    if (!_contourRecipe.enabled || _state != State::Ready || !_result.payload) return;
    auto count = static_cast<Output const &>(*_result.payload).count;
    if (!_contourResult.outcome.ok() || !_contourResult.product) {
        std::string reason = _contourResult.outcome.diagnostic[0] ? _contourResult.outcome.diagnostic : _("No contour product");
        if (!reason.empty() && reason.back() == '.') reason.pop_back();
        warning(Glib::ustring::compose(_("Contours unavailable: %1. Explode will create pieces without contours."), reason));
        return;
    }
    auto const &f = _contourResult.product->fitted;
    unsigned absent = 0;
    for (unsigned i = 0; i < f.pieceCount; ++i) absent += f.pieces()[i].noContour;
    setStatus(absent ? Glib::ustring::compose(_("%1 pieces · %2 without contour at these settings"), number(count), number(absent))
                    : Glib::ustring::compose(_("%1 pieces · contours ready"), number(count)));
}
void ExplodeBitmapPanel::createContourOnly()
{
    if (!_createContour.get_sensitive() || _state != State::Ready || !_contourResult.product) return;
    if (!acceptContour(0) || !acceptContour(1) || !acceptContour(2)) return;
    _contourOnlyPublication = true;
    activate(Intent::Explode);
    if (!_publicationPending) _contourOnlyPublication = false;
}
void ExplodeBitmapPanel::rejectContour(unsigned i)
{
    setStatus(contourError(i), true);
    _primary.set_sensitive(false); _apply.set_sensitive(false); _createContour.set_sensitive(false);
}
bool ExplodeBitmapPanel::acceptContour(unsigned i)
{
    double value = 0;
    if (!contourNumber(_contourSpins[i].get_text(), i, value)) { rejectContour(i); return false; }
    auto changed = contourValue(_contourRecipe, i) != value;
    _updating = true; _contourAdjustments[i]->set_value(value);
    _contourSpins[i].set_text(number(value, i == 1 ? 0 : 1)); _updating = false;
    _contourDirty[i] = false;
    if (changed) contourEdited();
    else display(_state);
    return true;
}
bool ExplodeBitmapPanel::accept(unsigned i)
{
    unsigned value = 0;
    if (!wholeNumber(_spins[i].get_text(), 0, fieldMax[i], value)) {
        setStatus(Glib::ustring::compose(_("Enter a whole number from %1 to %2."), 0, fieldMax[i]), true);
        _primary.set_sensitive(false); _apply.set_sensitive(false);
        _primary.set_label(_("Explode")); return false;
    }
    _updating = true; _adjustments[i]->set_value(value); _updating = false;
    auto &old = fieldValue(_recipe, i);
    bool changed = old != value || (_dirty[i] && _recipe.bypassAlpha); _dirty[i] = false;
    if (changed) { old = value; edited(); }
    else if (_state != State::Counting && _state != State::Processing) {
        // Redisplay restores control authorization after a rejected edit, but
        // does not reinstall the preview. Keep its existing fidelity warning.
        bool previewUnavailable = _status.get_text().find(_(detailedPreviewNote)) != Glib::ustring::npos ||
                                  (_proxy.get_visible() && !_showBefore);
        display(_state);
        if (previewUnavailable) warning(_(detailedPreviewNote));
    }
    return true;
}
void ExplodeBitmapPanel::drag(bool start, bool cancel)
{
    if (start) { if (!_dragging) { _dragRecipe = _recipe; _dragging = true; } }
    else if (std::exchange(_dragging, false)) {
        // Drag previews remain local. Cancel must not call remember(), which is
        // an explicit edit and clears an image's baked recognition.
        if (cancel) _recipe = _dragRecipe;
        else if (_identity && (_recipe.threshold != _dragRecipe.threshold || _recipe.softness != _dragRecipe.softness ||
                               _recipe.faintFloor != _dragRecipe.faintFloor || _recipe.refine != _dragRecipe.refine || _recipe.bypassAlpha != _dragRecipe.bypassAlpha)) {
            remember(_identity, _recipe, _recipe.refine);
        }
        if (_session && sameSelection() && valid(_prepared.target, *getDocument()) && valid(_prepared.dependencies)) { stop(); request(); }
        else refresh();
    }
}
bool ExplodeBitmapPanel::key(unsigned k, Gdk::ModifierType modifiers)
{
    bool pending = _dirty[0] || _dirty[1] || _dirty[2] || _contourDirty[0] || _contourDirty[1] || _contourDirty[2];
    auto discard = [this] {
        _updating = true;
        for (unsigned i = 0; i < 3; ++i) { _adjustments[i]->set_value(fieldValue(_recipe, i)); _spins[i].set_text(std::to_string(fieldValue(_recipe, i))); _dirty[i] = false; }
        for (unsigned i = 0; i < 3; ++i) {
            auto value = contourValue(_contourRecipe, i);
            _contourAdjustments[i]->set_value(value); _contourSpins[i].set_text(number(value, i == 1 ? 0 : 1)); _contourDirty[i] = false;
        }
        _updating = false;
    };
    if ((modifiers & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{} && (k == GDK_KEY_z || k == GDK_KEY_Z)) {
        discard(); _dragging = false; stale(); if (getDocument()) {
            if ((modifiers & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{}) DocumentUndo::redo(getDocument());
            else DocumentUndo::undo(getDocument());
        } refresh(); return true;
    }
    if (k == GDK_KEY_Escape) {
        if (_dragging) drag(false, true);
        else if (pending) { discard(); display(_state); }
        else if (_state != State::Idle) endSession(eligible() ? _("Analysis canceled. Unapplied changes were discarded. Click Analyze to start again.") : Glib::ustring{});
        else { stop(); set_visible(false); }
        return true;
    }
    for (unsigned i = 0; i < 3; ++i) if (_contourFocused[i]) {
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) { acceptContour(i); return true; }
    }
    for (unsigned i = 0; i < 3; ++i) if (_focused[i]) {
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) { accept(i); return true; }
        if (k == GDK_KEY_Up || k == GDK_KEY_Down) {
            auto n = int(fieldValue(_recipe, i)) + (k == GDK_KEY_Up ? 1 : -1);
            _spins[i].set_text(std::to_string(std::clamp(n, 0, int(fieldMax[i])))); accept(i); return true;
        }
    }
    if (_resizeDpi.has_focus() || (_resizeDpi.get_first_child() && _resizeDpi.get_first_child()->has_focus())) {
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) { _resizeDpi.update(); resizeDetails(); return true; }
    }
    return false; // Native Tab/Shift+Tab and button Enter/Space activation.

}
bool ExplodeBitmapPanel::eligible() const
{
    if (!getDocument() || !getSelection()) return false;
    auto image = cast<SPImage>(getSelection()->singleItem());
    if (!image) return false;
    auto href = image->getRepr()->attribute("href");
    if (!href) href = image->getRepr()->attribute("xlink:href");
    return href && g_str_has_prefix(href, "data:") && !g_str_has_prefix(href, "data:image/svg+xml");
}
void ExplodeBitmapPanel::resetComparison()
{
    auto updating = std::exchange(_updating, true);
    for (auto &dirty : _dirty) dirty = false;
    _showBefore = false; _comparisonAvailable = false; _previewToggle.set_active(true);
    _updating = updating;
}
void ExplodeBitmapPanel::setStatus(Glib::ustring const &text, bool warn)
{
    _status.set_text(text); _status.set_visible(!text.empty());
    if (warn) _status.remove_css_class("dim-label"); else _status.add_css_class("dim-label");
}
void ExplodeBitmapPanel::warning(Glib::ustring const &text)
{
    auto status = _status.get_text();
    if (status.find(text) == Glib::ustring::npos) setStatus(status.empty() ? text : status + "\n" + text, true);
}
void ExplodeBitmapPanel::resultDetails()
{
    if (!_result.payload) return;
    auto const &out = static_cast<Output const &>(*_result.payload);
    Glib::ustring details;
    auto add = [&details](Glib::ustring const &line) { if (!details.empty()) details += "\n"; details += line; };
    if (out.visible) add(Glib::ustring::compose(_("Hidden pixels: %1%% (%2 mm²)"), percent(100.0 * out.lost / out.visible), area(out.lostArea)));
    if (out.explodeOutcome.ok() && out.count && out.count <= MaxExplodePieces) {
        add(Glib::ustring::compose(_("Smallest piece: %1 mm²"), area(out.smallestArea)));
        add(Glib::ustring::compose(_("Estimated document growth: %1"), sizeText(out.pieces.hrefBytes)));
        add(Glib::ustring::compose(_("Initial islands: %1"), number(out.initial)));
        add(Glib::ustring::compose(_("Enclosed islands: %1"), number(out.enclosed)));
        add(Glib::ustring::compose(_("Joined specks: %1"), number(out.joined)));
        add(Glib::ustring::compose(_("Isolated specks: %1"), number(out.isolated)));
        add(_("Specks up to 2 mm² join within 1 mm. Enclosed islands stay together."));
    }
    _details.set_text(details); _details.set_visible(!details.empty());
    auto notice = _recipe.bypassAlpha ? _(bakedNote) : (_recipe.refine || out.lost) ? _(previewNote) : _("Original transparency is preserved.");
    _notice.set_text(notice); _notice.set_visible(true);
    if (out.alpha.reduced16) {
        auto note = _("The result will use 8 bits per channel. Undo preserves the 16-bit original.");
        _notice.set_text(_notice.get_text() + "\n" + note); warning(note);
    }
    if (out.grid.width && !out.exactSourceMapping)
        warning(_("This image will be resampled. The canvas preview may differ from the exploded pieces."));
    if (_drawingRefused && out.explodeOutcome.ok()) warning(_("Piece outlines are unavailable. The piece count is exact."));
    if (!out.adjustmentOutcome.ok()) {
        auto text = failureText(out.adjustmentOutcome);
        _apply.set_tooltip_text(text); warning(text);
    } else if (!_apply.get_sensitive()) _apply.set_tooltip_text(_("This adjustment would not change the image."));
    _detailsExpander.set_visible(!details.empty() || !_notice.get_text().empty());
}
void ExplodeBitmapPanel::resizeDetails()
{
    if (!getSelection()) return;
    unsigned dpi = 0;
    auto max = unsigned(_resizeDpi.get_adjustment()->get_upper());
    _resizeDpi.set_tooltip_text(Glib::ustring::compose(_("Choose a whole number from 1 to %1 dpi. The resized image must fit within 5000 × 5000 px."), max));
    if (!wholeNumber(_resizeDpi.get_text(), 1, max, dpi)) {
        setStatus(Glib::ustring::compose(_("Enter a whole number from %1 to %2."), 1, max), true);
        _primary.set_sensitive(false); return;
    }
    if (auto bounds = getSelection()->documentBounds(SPItem::VISUAL_BBOX)) {
        _resizeDescription.set_text(Glib::ustring::compose(_("Resized image: %1 × %2 px"),
            std::uint64_t(std::ceil(bounds->width() * dpi / 96)), std::uint64_t(std::ceil(bounds->height() * dpi / 96))) + "\n" +
            _("Resize replaces the selected image at its current position and size. Undo restores the original."));
    }
    if (_state == State::Resize) {
        _primary.set_sensitive(true);
        if (auto image = cast<SPImage>(getSelection()->singleItem()); image && image->pixbuf)
            setStatus(Glib::ustring::compose(_("This image is %1 × %2 px. Explode Bitmap supports images up to 5000 × 5000 px. Resize to continue."),
                image->pixbuf->width(), image->pixbuf->height()), true);
    }
}
void ExplodeBitmapPanel::display(State state, Glib::ustring const &detail)
{
    _state = state; _updating = true;
    if (state != State::Ready) _contourOverlay.clear();
    auto out = _result.payload ? &static_cast<Output const &>(*_result.payload) : nullptr;
    bool publishing = _publicationPending || _publishing;
    bool alphaReady = out && out->adjustment && (_recipe.refine || out->lost);
    bool partitionRefused = out && !out->explodeOutcome.ok();
    bool controls = state == State::Counting || (state == State::Processing && !publishing) || state == State::Ready || state == State::One || state == State::AdjustmentEmpty || state == State::TooMany || state == State::Applied || (state == State::Failed && (partitionRefused || _drawingRefused));
    bool sessionVisible = controls || state == State::Opaque || state == State::SourceEmpty || (state == State::Idle && eligible());
    _contour.set_visible(sessionVisible);
    _addContour.set_sensitive(controls && !publishing);
    _contourParameters.set_sensitive(controls && !publishing && _contourRecipe.enabled);
    _createContour.set_sensitive(!publishing && state == State::Ready && _contourRecipe.enabled && bool(_contourResult.product));
    _transparency.set_visible(sessionVisible); _comparison.set_visible(sessionVisible);
    _refine.set_sensitive(controls && !publishing); _refine.set_active(_recipe.refine);
    for (unsigned i = 0; i < 3; ++i) {
        bool sensitive = controls && !publishing && (i == 2 || _recipe.refine);
        _labels[i].set_sensitive(sensitive); _spins[i].set_sensitive(sensitive); _sliders[i].set_sensitive(sensitive);
        // A worker completion or another field's commit must not erase rejected text.
        if (!_dirty[i]) { _adjustments[i]->set_value(fieldValue(_recipe, i)); _spins[i].set_text(std::to_string(fieldValue(_recipe, i))); }
    }
    auto floorDescription = Glib::ustring::compose(_("Ignore pixels up to %1%% opacity"), fieldValue(_recipe, 2)) + "\n" + _(floorTooltip);
    for (Gtk::Widget *widget : std::initializer_list<Gtk::Widget *>{&_labels[2], &_sliders[2], &_spins[2]}) widget->set_tooltip_text(floorDescription);
    _resizeControls.set_visible(state == State::Resize);
    _resizeControls.set_sensitive(state == State::Resize && !publishing);
    _apply.set_sensitive(controls && !publishing && !_recipe.bypassAlpha && state != State::Counting && state != State::Processing && state != State::Applied && alphaReady);
    _apply.set_tooltip_text(_("Bake the transparency adjustment into this image. Undo restores the previous image."));
    _primary.set_sensitive(!publishing && (state == State::Ready || state == State::Resize || ((state == State::Idle || state == State::Stale || (state == State::Failed && !out)) && eligible())));
    _cancel.set_sensitive(!_publishing);
    _progress.set_visible((state == State::Counting || state == State::Processing) && !publishing);
    if (state == State::Counting || state == State::Processing) _progress.set_fraction(0);
    _fit.set_sensitive(eligible() && !publishing);
    _zoom.set_visible(state != State::Resize); _zoom.set_sensitive(!publishing && state == State::Ready && out && out->count && out->count <= MaxExplodePieces);
    _navigation.set_visible(eligible() || sessionVisible);
    bool compare = !publishing && (state == State::Ready || state == State::One || state == State::AdjustmentEmpty || (state == State::Failed && out) || ((state == State::Counting || state == State::Processing) && _comparisonAvailable));
    if (out && out->proxyWidth && compare) _comparisonAvailable = true;
    _comparison.set_sensitive(compare && _comparisonAvailable);
    _details.set_text(""); _notice.set_text(""); _detailsExpander.set_visible(false);
    bool analyze = state == State::Idle || state == State::Stale || state == State::Counting || state == State::Processing || (state == State::Failed && !out);
    _primary.set_label(analyze ? _("Analyze") : state == State::Resize ? _("Resize") : _("Explode"));
    // Longer actions must share the narrow footer with Cancel.
    if (auto label = dynamic_cast<Gtk::Label *>(_primary.get_child())) {
        label->set_wrap(true); label->set_wrap_mode(Pango::WrapMode::WORD);
        label->set_max_width_chars(16);
    }
    _primary.set_tooltip_text(analyze ? _("Analyze the selected embedded image. Selecting another object ends this session.") :
        state == State::Resize ? _("Resize replaces the selected image at its current position and size. Undo restores the original.") :
        _("Export hints, title and description will not be copied to pieces. Set export settings on the new pieces."));
    Glib::ustring message = detail;
    if (message.empty()) switch (state) {
        case State::Idle:
            if (!getDocument()) message = _("Open a document, then select one embedded image.");
            else if (!getSelection() || getSelection()->isEmpty()) message = _("Select one embedded image, then click Analyze.");
            else if (eligible()) message = _("Click Analyze to inspect the selected image.");
            else if (auto image = cast<SPImage>(getSelection()->singleItem())) {
                auto href = image->getRepr()->attribute("href");
                if (!href) href = image->getRepr()->attribute("xlink:href");
                message = href && g_str_has_prefix(href, "data:image/svg+xml")
                    ? _("This embedded image contains SVG artwork. Use an embedded raster image.")
                    : _("Embed this linked image before adjusting or exploding it.");
            }
            else if (getSelection()->size() > 1 || is<SPGroup>(getSelection()->singleItem())) message = _("Select one embedded image directly, then click Analyze.");
            else message = _("Explode Bitmap works on embedded images. For vectors, use Path > Break Apart.");
            break;
        case State::Stale: message = _("Image changed. Click Analyze to update the preview and piece count."); break;
        case State::Counting: message = Glib::ustring::compose(_("Analyzing image… %1%% · Esc cancels"), 0); break;
        case State::Ready:
            if (out) {
                message = Glib::ustring::compose(_recipe.bypassAlpha
                    ? ngettext("%1 piece. Adjustment applied.", "%1 pieces. Adjustment applied.", out->count)
                    : ngettext("%1 piece", "%1 pieces", out->count), number(out->count));
                _primary.set_label(Glib::ustring::compose(ngettext("Explode %1 piece", "Explode %1 pieces", out->count), number(out->count)));
            }
            break;
        case State::Opaque: message = _("This image has no transparency to separate."); break;
        case State::SourceEmpty: message = _("The original image has no visible pixels. No pieces created."); break;
        case State::AdjustmentEmpty: message = _("This adjustment hides all visible pixels. Choose Before or change the transparency settings. No pieces created."); break;
        case State::One: message = _("Only one piece remains: pixels touch, islands are enclosed, or specks were joined. Source unchanged."); break;
        case State::TooMany:
            message = out && out->explodeOutcome.ok() ? Glib::ustring::compose(_(tooMany), number(out->count)) : _(tooManyBound); break;
        case State::Processing: message = Glib::ustring::compose(_("Preparing pieces… %1%% · Esc cancels"), 0); break;
        case State::Applied: message = _(bakedNote); break;
        case State::Canceled: message = _("Operation canceled. Unapplied changes were discarded. Applied changes remain."); break;
        case State::Failed: message = _(failure); break;
        default: break;
    }
    bool warn = state == State::Failed || state == State::Unsupported || state == State::Referenced || state == State::Protected || state == State::TooMany || state == State::AdjustmentEmpty || state == State::Resize;
    if (!_applyMessage.empty() && _applyMessage != _(bakedNote) && state == State::Ready) message += "\n" + _applyMessage;
    setStatus(message, warn);
    contourStatus();
    if (out && (state == State::Ready || state == State::One || state == State::AdjustmentEmpty || state == State::Failed)) resultDetails();
    _metadata.set_visible(state != State::Done);
    if (state == State::Resize) resizeDetails();
    for (unsigned i = 0, value = 0; i < 3; ++i) if (_dirty[i] && !wholeNumber(_spins[i].get_text(), 0, fieldMax[i], value)) {
        _primary.set_sensitive(false); _apply.set_sensitive(false);
        warning(Glib::ustring::compose(_("Enter a whole number from %1 to %2."), 0, fieldMax[i]));
    }
    for (unsigned i = 0; i < 3; ++i) {
        double value = 0;
        if (_contourDirty[i] && !contourNumber(_contourSpins[i].get_text(), i, value)) rejectContour(i);
    }
    _updating = false;
}
void ExplodeBitmapPanel::progress(JobProgress p)
{
    auto state = p.stage == Stage::png || p.stage == Stage::prepared ? State::Processing : State::Counting;
    // Both worker states have the same controls; preserve any pending text edit.
    _state = state;
    auto fraction = p.total ? std::min(1.0, double(p.completed) / p.total) : 0;
    _progress.set_fraction(fraction);
    setStatus(Glib::ustring::compose(p.phase != JobPhase::preparation ? _("Tracing contours… %1%% · Esc cancels") : state == State::Counting
        ? _("Analyzing image… %1%% · Esc cancels") : _("Preparing pieces… %1%% · Esc cancels"), int(fraction * 100)));
}
void ExplodeBitmapPanel::request()
{
    std::uint64_t allocationNeed = sizeof(Input) + 64;
    try {
    if (!_session || !sameSelection() || !eligible()) return;
    auto releaseUncapturedSession = scope_exit([this] {
        // A preparation refusal is not evidence that the image changed. Without
        // a captured graph, end authorization while retaining the actionable status.
        if (!_prepared.dependencies) {
            _sessionWatch.disconnect(); _session = false; ++_sessionGeneration;
        }
    });
    _dispatchGeneration = _sessionGeneration;
    auto target = resolve(*getDesktop(), Intent::Explode);
    if (!target.ok()) {
        auto state = State::Unsupported; Glib::ustring message = Glib::ustring(_(effectsFailure));
        for (auto const &r : target.value.refusals) {
            if (r.reason == Refusal::ZeroOpacity) message = _("This image is fully hidden by object or layer opacity. Increase its opacity before exploding it.");
            if (r.reason == Refusal::MissingSource || r.reason == Refusal::InvalidIntake) message = _("The embedded image could not be read. Replace it with a supported raster image.");
            if (r.reason == Refusal::TextTool) { state = State::Stale; message = _("Leave text editing, then click Analyze again."); }
            if (r.reason == Refusal::LinkedSource) message = _("Embed this linked image before adjusting or exploding it.");
            if (std::string_view(r.diagnostic).find("CMYK") != std::string_view::npos) message = _("Convert this CMYK image to RGB and embed it before adjusting or exploding it.");
            if (r.reason == Refusal::Hidden || r.reason == Refusal::Locked) {
                state = State::Protected; message = Glib::ustring::compose(_("This image is inside locked or hidden object/layer “%1”. Unlock or show it first."), r.id); break;
            }
            if (r.reason == Refusal::CloneReference || r.reason == Refusal::HrefReference || r.reason == Refusal::UrlReference) {
                state = State::Referenced; message = _("This image or an ancestor is used by clones or references. Unlink those references before adjusting or exploding it."); break;
            }
        }
        if (auto image = cast<SPImage>(getSelection()->singleItem()); image && state == State::Unsupported) {
            auto href = image->getRepr()->attribute("href"); if (!href) href = image->getRepr()->attribute("xlink:href");
            if (href && std::string_view(href).starts_with("data:image/svg+xml")) message = _("This embedded image contains SVG artwork. Use an embedded raster image.");
            if (href && std::string_view(href).starts_with("file:")) {
                auto path = g_filename_from_uri(href, nullptr, nullptr);
                if (path && !g_file_test(path, G_FILE_TEST_IS_REGULAR)) message = _("The linked image is unavailable. Restore the file and embed it first.");
                g_free(path);
            }
        }
        display(state, message); return;
    }
    auto memory = _options.memory ? sampleMemory(*_options.memory) : sampleMemory();
    if (!memory.ok()) { outcome(memory.outcome, Intent::Explode); return; }
    if (!_budget || !_budget->reserved()) _budget = std::make_shared<Budget>(std::numeric_limits<std::uint64_t>::max());
    auto checked = _budget->recheckMeasured(memory.value); if (!checked.ok()) { outcome(checked, Intent::Explode); return; }
    _prepared.target = target.value; _prepared.activation = _activation;
    _prepared.dependencies = capture(target.value); _prepared.limits = measuredLimits(_options.evidence);
    if (!_prepared.dependencies) {
        display(State::Failed, _(platformFailure));
        return;
    }
    if (!_prepared.limits.workerQualified) { display(State::Failed, _(platformFailure)); return; }
    auto image = cast<SPImage>(getSelection()->singleItem()); if (!image) return;
    auto identity = logicalImageIdentity(*image);
    if (!_dragging || identity != _identity) _recipe = query(identity);
    if (identity != _identity) resetComparison();
    _identity = identity;
    _prepared.session = sessionJobIdentity(_identity, target.value);
    _prepared.requestRecipe = _recipe;
    if (image->pixbuf && (image->pixbuf->width() > int(MaxExplodeSourceAxis) || image->pixbuf->height() > int(MaxExplodeSourceAxis))) {
        auto bounds = getSelection()->documentBounds(SPItem::VISUAL_BBOX);
        int dpi = bounds ? maxResizeDpi(*bounds) : 0;
        if (dpi) {
            _updating = true; _resizeDpi.set_range(1, dpi); _resizeDpi.set_value(dpi); _resizeDpi.set_text(std::to_string(dpi)); _updating = false;
        }
        display(dpi ? State::Resize : State::Failed, Glib::ustring::compose(dpi
            ? _("This image is %1 × %2 px. Explode Bitmap supports images up to 5000 × 5000 px. Resize to continue.")
            : _("This image is %1 × %2 px. It cannot fit within 5000 × 5000 px at 1 dpi. Resize it outside this panel and try again."),
            image->pixbuf->width(), image->pixbuf->height(), MaxExplodeSourceAxis));
        return;
    }
    auto href = image->getRepr()->attribute("href"); if (!href) href = image->getRepr()->attribute("xlink:href");
    auto header = inspectHref(href ? href : "", {}, *_budget); if (!header.ok()) { outcome(header.outcome, Intent::Explode); return; }
    auto uri = inspectUri(href, {}); if (!uri.ok() || uri.value.kind != UriKind::Data) { outcome(uri.outcome, Intent::Explode); return; }
    if (!_prepared.limits.workerQualified) { display(State::Failed, _(platformFailure)); return; }
    auto pixels = std::uint64_t(header.value.width) * header.value.height;
    auto requestedContour = _contourRecipe; _contourAdmission = {};
    auto admission = admit(resources(header.value.width, header.value.height, uri.value.decodedBytes, _budget->reserved(), _contourRecipe), memory.value);
    bool displayAdmitted = admission.ok() && pixels*4 <= _options.displayByteLimit;
    if (!admission.ok()) {
        admission = admit(resources(header.value.width, header.value.height, uri.value.decodedBytes,
                                    _budget->reserved(), _contourRecipe, false), memory.value);
        if (!admission.ok() && requestedContour.enabled) {
            // Optional contour admission must not reject an admissible Explode.
            _contourAdmission = admission.outcome; requestedContour.enabled = false;
            admission = admit(resources(header.value.width, header.value.height, uri.value.decodedBytes,
                                        _budget->reserved()), memory.value);
            displayAdmitted = admission.ok() && pixels*4 <= _options.displayByteLimit;
            if (!admission.ok()) admission = admit(resources(header.value.width, header.value.height, uri.value.decodedBytes,
                                                            _budget->reserved(), {}, false), memory.value);
        }
        if (!admission.ok()) { outcome(admission.outcome, Intent::Explode); return; }
    }
    JobInput job; job.storage.budget = _budget;
    std::uint64_t envelope = sizeof(Input) + 64 + uri.value.payloadLength + target.value.contexts.size() * sizeof(TargetContext);
    for (auto const &c : target.value.contexts) envelope += c.id.size() + 1;
    envelope += (target.value.selection.size() + target.value.roots.size()) * sizeof(std::uintptr_t);
    allocationNeed = envelope;
    checked = _budget->acquire(Stage::input, envelope, job.storage.reservation); if (!checked.ok()) { outcome(checked, Intent::Explode); return; }
    auto input = std::make_shared<Input>(); input->target = std::move(target.value); input->recipe = _recipe; input->contour = requestedContour; input->observer = _options.observer;
    // Reserve optional geometry against the admitted whole-job peak before
    // dispatch. Required grid/topology growth can release this reservation and
    // retry without outlines; refusal leaves exact analysis/publication intact.
    auto outlineBytes = std::min(_options.outlineByteLimit, Bitmap::outlineByteLimit);
    if (outlineBytes <= admission.value.limit - admission.value.peak) {
        auto outlines = reserveOutlines(_budget, outlineBytes);
        if (outlines.ok()) input->outlineReservation = std::move(outlines.value);
    }
    if (displayAdmitted) {
        auto display = reserveAlphaDisplay(header.value.width, header.value.height, _budget);
        if (display.ok()) input->display = std::move(display.value);
    }
    auto rendering = image->style->image_rendering.computed;
    input->recipe.pixelated = rendering == SP_CSS_IMAGE_RENDERING_PIXELATED || rendering == SP_CSS_IMAGE_RENDERING_OPTIMIZESPEED || rendering == SP_CSS_IMAGE_RENDERING_CRISPEDGES;
    input->recipe.alphaPrepared = _recipe.bypassAlpha;
    input->recipe.bypassAlpha = !_recipe.refine || _recipe.bypassAlpha; input->decodedBytes = uri.value.decodedBytes;
    if (image->getClipObject()) {
        auto coverage = captureGridCoverage(*image, input->target, *_budget); if (!coverage.ok()) { outcome(coverage.outcome, Intent::Explode); return; }
        input->coverage = std::move(coverage.value);
    }
    if (_options.inputFault && _options.inputFault->fail()) throw std::bad_alloc();
    job.storage.bytes.assign(href + uri.value.payloadOffset, href + uri.value.payloadOffset + uri.value.payloadLength);
    job.storage.payload = std::move(input); job.pixels = pixels; job.work = _options.work ? _options.work : calculate;
    if (!_activation->check(_prepared.dependencies, DependencyCheck::Dispatch).ok()) { refresh(); return; }
    _ticket = _deliveryTicket = _jobs.request(std::move(job)); _overlay.beginGeneration(*getDesktop(), ++_viewGeneration); display(State::Counting);
    } catch (std::bad_alloc const &) {
        auto free = _budget && _budget->limit() > _budget->reserved()
            ? _budget->limit() - _budget->reserved() : 0;
        outcome(Bitmap::memoryFailure(N_("main-thread allocator / image input"), allocationNeed, free), Intent::Explode);
    } catch (...) { display(State::Failed, _(failure)); }
}
void ExplodeBitmapPanel::receive(Ticket ticket, JobResult result) try
{
    if (!active() || !_session || _dispatchGeneration != _sessionGeneration || ticket != _deliveryTicket || !getDesktop()) return;
    if (!_activation->check(_prepared.dependencies, DependencyCheck::Delivery).ok() || (_prepared.target.mode == TargetMode::SingleBitmap && !valid(_prepared.session))) {
        refresh(); return;
    }
    if (_contourJob) {
        _contourJob = false;
        _contourResult = {};
        if (result.ok()) _contourResult = static_cast<ContourResult const &>(*result.value.payload);
        else _contourResult.outcome = result.outcome;
        if (_contourResult.outcome.refusal == ContourRefusal::staleAnalysis) {
            _activation->retry(); stop(); request(); return;
        }
        _activation->completed(); display(State::Ready); installPreview(); return;
    }
    if (!result.ok()) { outcome(result.outcome, Intent::Explode); return; }
    _result = std::move(result.value); auto retained = _result.payload; auto const &out = static_cast<Output const &>(*retained);
    _contourResult = _contourRecipe.enabled ? out.contours : ContourResult{};
    if (_contourRecipe.enabled && !_contourAdmission.ok()) _contourResult.outcome = _contourAdmission;
    _prepared.grid = &out.grid; _prepared.pieces = &out.pieces; _prepared.budget = _budget.get(); _prepared.probe = _options.memory;
    auto state = !out.explodeOutcome.ok() ? (std::string_view(out.explodeOutcome.diagnostic) == "Final piece cap exceeded" ? State::TooMany : State::Failed) :
                 out.opaque ? State::Opaque : !out.visible ? State::SourceEmpty : out.count > MaxExplodePieces ? State::TooMany :
                 !out.count ? State::AdjustmentEmpty : State::Ready;
    display(state, state == State::Failed ? failureText(out.explodeOutcome) : Glib::ustring{});
    if (state == State::TooMany) { _activation->completed(); return; }
    if (state == State::Opaque || state == State::SourceEmpty) return;
    auto image = cast<SPImage>(getSelection()->singleItem());
    _contribution = {}; _contribution.layer = PreviewLayer::Alpha;
    _contribution.source = image ? image->pixbuf : nullptr;
    _contribution.backing = out.display;
    try { _contribution.pixels = wrapAlphaDisplay(out.display); }
    catch (...) { _contribution.backing.reset(); }
    _activation->completed();
    installPreview();
}
catch (...) { display(State::Failed, _(memoryFailure)); }
bool ExplodeBitmapPanel::current()
{
    return active() && getDesktop() && getDocument() && _activation->check(_prepared.dependencies, DependencyCheck::Confirmation).ok() &&
        valid(_prepared.target, *getDocument()) && (_prepared.target.mode == TargetMode::CollectiveConversion || valid(_prepared.session));
}
bool ExplodeBitmapPanel::recaptureView()
{
    if (!getDocument() || !valid(_prepared.target, *getDocument()) || (_prepared.target.mode == TargetMode::SingleBitmap && !valid(_prepared.session))) return false;
    _prepared.dependencies = capture(_prepared.target); return bool(_prepared.dependencies);
}
void ExplodeBitmapPanel::installPreview()
{
    if (!active() || _showBefore || !_result.payload || !current() ||
        (_state != State::Ready && _state != State::One && _state != State::AdjustmentEmpty && _state != State::TooMany && _state != State::Failed)) return;
    // Preview fallbacks may redisplay status; keep the optional-contour refusal.
    auto refusal = scope_exit([this] {
        if (_contourRecipe.enabled && !_contourResult.product) contourStatus();
    });
    auto retained = _result.payload; auto const &out = static_cast<Output const &>(*retained);
    ++_viewGeneration;
    auto image = cast<SPImage>(getSelection()->singleItem());
    bool canvas = image && image->pixbuf && _contribution.pixels &&
        image->pixbuf->width() == _contribution.pixels->width() &&
        image->pixbuf->height() == _contribution.pixels->height();
    _proxy.set_visible(false);
    if (!canvas) {
        _preview.reset();
        warning(_(detailedPreviewNote));
        // The explicitly disclosed panel fallback remains a small straight-RGBA
        // proxy. Never convert the immutable full-resolution Cairo backing.
        if (out.proxy.size()) {
            auto owner = new std::shared_ptr<JobPayload const>(retained);
            auto gdk = gdk_pixbuf_new_from_data(reinterpret_cast<guchar const *>(out.proxy.data()),
                GDK_COLORSPACE_RGB, true, 8, out.proxyWidth, out.proxyHeight, out.proxyWidth*4,
                [](guchar *, gpointer p) { delete static_cast<std::shared_ptr<JobPayload const> *>(p); }, owner);
            if (gdk) { _proxy.set_pixbuf(Glib::wrap(gdk)); _proxy.set_visible(true); }
            else delete owner;
        }
    }
    if (canvas && (_recipe.refine || out.lost) && !_recipe.bypassAlpha) {
        _preview = contribute({getSelection()->singleItem(), getDesktop()->dkey}, _contribution);
        auto installed = Bitmap::update(_preview, _viewGeneration);
        if (!installed.ok()) { _preview.reset(); warning(_(detailedPreviewNote)); }
    }
    if (!recaptureView()) { refresh(); return; }
    if (_contourRecipe.enabled && _contourResult.product && _state == State::Ready) {
        _overlay.clear();
        _contourOverlay.beginGeneration(*getDesktop(), _viewGeneration);
        auto c = _contourColor.get_rgba();
        auto rgba = (std::uint32_t(std::lround(c.get_red()*255)) << 24) |
                    (std::uint32_t(std::lround(c.get_green()*255)) << 16) |
                    (std::uint32_t(std::lround(c.get_blue()*255)) << 8) | 255;
        auto installed = _contourOverlay.install(*getDesktop(), _contourResult.product, out.grid.pixelToDocument,
                                                 _prepared.dependencies, _viewGeneration, rgba);
        if (installed.status == Status::canceled) refresh();
        return;
    }
    _contourOverlay.clear();
    if (out.explodeOutcome.ok() && out.count >= 1 && out.count <= MaxExplodePieces && !out.outlines.storage) {
        omitOutlines(); return;
    }
    if (out.explodeOutcome.ok() && out.count >= 1 && out.count <= MaxExplodePieces) {
        auto outlines = out.outlines;
        // Storage owns its ledgers/reservation independently of the optional
        // Output slot; retired canvas readers keep their immutable snapshot.
        outlines.dependencies = _prepared.dependencies; outlines.limits = _prepared.limits; outlines.generation = _viewGeneration;
        _overlay.beginGeneration(*getDesktop(), _viewGeneration);
        auto installed = _overlay.installOutlines(*getDesktop(), outlines, _viewGeneration);
        if (installed.status == Status::canceled) { refresh(); return; }
        if (!installed.ok()) { _drawingRefused = true; outlineStatus(OutlineVisibility::unavailable); }

    }
}
void ExplodeBitmapPanel::omitOutlines()
{
    _overlay.clear(); _contourOverlay.clear(); _drawingRefused = true;
    warning(_("Piece outlines are unavailable. The piece count is exact."));
}
void ExplodeBitmapPanel::outlineStatus(OutlineVisibility state)
{
    if (!_result.payload || _showBefore || (_contourRecipe.enabled && _contourResult.product)) return;
    auto text = _status.get_text();
    for (auto message : {_("Zoom in to see piece outlines."), _("Piece outlines are unavailable. The piece count is exact.")}) {
        auto at = text.find(message);
        if (at != Glib::ustring::npos) {
            auto start = at && text[at-1] == '\n' ? at-1 : at;
            text.erase(start, at + Glib::ustring(message).size() - start);
        }
    }
    setStatus(text);
    _drawingRefused = state == OutlineVisibility::unavailable;
    if (state == OutlineVisibility::tooDense) warning(_("Zoom in to see piece outlines."));
    else if (_drawingRefused) warning(_("Piece outlines are unavailable. The piece count is exact."));
}
void ExplodeBitmapPanel::original(bool held)
{
    if (!active() || _showBefore == held) return;
    _showBefore = held;
    if (held) { _proxy.set_visible(false); auto fresh = current(); _preview.reset(); _overlay.clear(); _contourOverlay.clear(); if (fresh) recaptureView(); }
    else installPreview();
}
void ExplodeBitmapPanel::activate(Intent intent)
{
    if (!active() || _publishing || _publicationPending || _consumed == _ticket) return;
    if (!acceptContour(0) || !acceptContour(1) || !acceptContour(2) || !accept(0) || !accept(1) || !accept(2)) return;
    if (!current()) { refresh(); return; }
    if (intent == Intent::ConversionCandidate) return;
    if (intent == Intent::ApplyAdjustment && !_apply.get_sensitive()) return;
    if (intent == Intent::Explode) {
        if (_state != State::Ready || !_result.payload) return;
        auto count = static_cast<Output const &>(*_result.payload).count;
        if (!count || count > MaxExplodePieces) return;
    }
    if (getDesktop()->bitmapAdjustmentsController().previewActive()) {
        setStatus(_(toneConflict), true); return;
    }
    _consumed = _ticket; // Latch before yielding to the frame clock or any publication callback.
    _publicationPending = true;
    auto count = static_cast<Output const &>(*_result.payload).count;
    display(State::Processing, intent == Intent::ApplyAdjustment ? _("Applying adjustment…")
        : Glib::ustring::compose(ngettext("Exploding %1 piece…", "Exploding %1 pieces…", count), number(count)));
    _resizing = false;
    _publicationIntent = intent;
    deferPublication();
}
void ExplodeBitmapPanel::deferPublication()
{
    // An idle alone can run before GTK paints. Wait for this queued frame's
    // after-paint, then leave that signal before synchronous native publication.
    _publicationClock = get_frame_clock();
    _afterPaint = g_signal_connect(_publicationClock->gobj(), "after-paint", G_CALLBACK((+[](GdkFrameClock *, gpointer data) {
        auto self = static_cast<ExplodeBitmapPanel *>(data);
        g_signal_handler_disconnect(self->_publicationClock->gobj(), self->_afterPaint);
        self->_afterPaint = 0;
        self->_publicationIdle = Glib::signal_idle().connect([self, intent = self->_publicationIntent, ticket = self->_ticket] {
            self->_publicationClock.reset();
            if (self->_resizing) self->publishResize();
            else self->publish(intent, ticket);
            return false;
        });
    })), this);
    queue_draw();
    _publicationClock->request_phase(Gdk::FrameClock::Phase::AFTER_PAINT);
}
void ExplodeBitmapPanel::cancelPublication()
{
    _publicationIdle.disconnect();
    if (_afterPaint) g_signal_handler_disconnect(_publicationClock->gobj(), _afterPaint);
    _afterPaint = 0; _publicationClock.reset(); _publicationPending = false; _resizing = false;
}
void ExplodeBitmapPanel::resize()
{
    if (!active() || _publishing || _publicationPending || _state != State::Resize) return;
    if (!current()) { refresh(); return; }
    if (getDesktop()->bitmapAdjustmentsController().previewActive()) {
        setStatus(_(toneConflict), true); return;
    }
    unsigned dpi = 0;
    if (!wholeNumber(_resizeDpi.get_text(), 1, _resizeDpi.get_adjustment()->get_upper(), dpi)) {
        resizeDetails(); _resizeDpi.grab_focus(); return;
    }
    _resizeDpi.update();
    _resizeOptions.dpi = dpi;
    _resizeOptions.antialias = _resizeAntialias.get_active();
    _resizeOptions.transparent = true; _resizeOptions.keep_original = false; _resizeOptions.commit_undo = false;
    _publicationPending = true; _resizing = true;
    display(State::Processing, _("Resizing image…"));
    deferPublication();
}
void ExplodeBitmapPanel::publishResize()
{
    if (!current()) { cancelPublication(); refresh(); return; }
    _publicationPending = false; _resizing = false; _publishing = true; _cancel.set_sensitive(false);
    auto alive = _alive; auto budget = _budget; auto doc = getDocument(); auto desktop = getDesktop();
    auto options = _resizeOptions; auto limits = _prepared.limits;
    Outcome result{Status::unavailable, "Resize is unavailable."};
    try {
        auto bounds = getSelection()->documentBounds(SPItem::VISUAL_BBOX);
        result = {};
        if (!bounds || options.dpi < 1 || options.dpi > maxResizeDpi(*bounds)) result = {Status::unavailable, "Resize resolution no longer fits."};
        if (result.ok()) {
            auto candidate = prepareBitmapCopy(*desktop->getSelection(), options, PlacementPolicy::SingleImageResize,
                                               *budget, {_options.memory, nullptr, nullptr, nullptr, &limits});
            result = candidate.outcome;
            if (candidate.ok() && (!*alive || !current())) result = {Status::canceled, "Resize canceled."};
            else if (candidate.ok()) {
                auto guard = DocumentUndo::beginAtomicInteraction(doc);
                if (!guard) result = {Status::unavailable, "Atomic bitmap admission refused."};
                else {
                    auto sourceId = std::string(desktop->getSelection()->singleItem()->getId());
                    bool committed = false;
                    auto restore = scope_exit([&] {
                        if (committed) return;
                        guard->rollback();
                        if (*alive && getDesktop() == desktop) desktop->getSelection()->set(doc->getObjectById(sourceId.c_str()));
                    });
                    auto published = publishBitmapCopy(*desktop->getSelection(), candidate.candidate, &*guard);
                    result = published.outcome;
                    if (published.ok()) {
                        committed = guard->commitAtomically(RC_("Undo", "Resize image for Explode Bitmap"), "",
                            [&] { return *alive && active() && getDesktop() == desktop && guard->validAtomicFor(doc) &&
                                reinterpret_cast<std::uintptr_t>(desktop->getSelection()->singleItem()) == published.image; });
                        if (!committed) result = {Status::canceled, "Resize canceled."};
                    }
                }
            }
        }
    } catch (...) { result = {Status::failed, "Bitmap resize failed."}; }
    if (!*alive) return;
    _publishing = false;
    if (!active()) return;
    if (result.status == Status::changed) endSession(_("Image resized. Click Analyze to inspect it."));
    else outcome(result, Intent::Explode);
}
void ExplodeBitmapPanel::publish(Intent intent, Ticket ticket)
{
    if (!active() || ticket != _ticket) { cancelPublication(); return; }
    // Revalidate the original capture BEFORE removing our preview and recapturing.
    // Otherwise an intervening edit could be silently blessed by recaptureView().
    if (!current()) {
        cancelPublication(); refresh(); return;
    }
    _publicationPending = false;
    auto alive = _alive; auto retained = _result.payload; auto budget = _budget;
    _publishing = true; _cancel.set_sensitive(false);
    auto count = _result.payload ? static_cast<Output const &>(*_result.payload).count : 0;
    _preview.reset(); _overlay.clear(); _contourOverlay.clear(); _proxy.set_visible(false);
    // Publication needs no display copy. Frozen readers keep their independent
    // reservation, but the panel and Output must not pin optional display RAM.
    bool contourNoop = _contourOnlyPublication && _contourResult.product && !_contourResult.product->fitted.ringCount;
    if (!contourNoop) {
        _contribution = {};
        if (_result.payload) static_cast<Output const &>(*_result.payload).display.reset();
    }
    // Retain exact geometry through a refused publication too. Successful
    // content/session invalidation releases Output; frozen readers own their
    // independent reference until the canvas retires the snapshot.
    if (!recaptureView()) {
        _publishing = false; refresh(); return;
    }
    Outcome result;
    try {
        auto prepared = _prepared;
        prepared.contours = intent == Intent::Explode && _contourRecipe.enabled && _contourResult.product ? &_contourResult.product->fitted : nullptr;
        prepared.contourStyle = _contourStyle;
        if (intent == Intent::ApplyAdjustment) {
            auto const &alpha = static_cast<Output const &>(*_result.payload).adjustment;
            if (!alpha) result = {Status::unavailable, "No prepared adjustment is available."};
            else { alpha->publication = prepared; result = publishAlpha(*getDesktop(), *alpha, _ticket); }
        }
        else if (_contourOnlyPublication) result = publishContourOnly(*getDesktop(), {prepared}, _ticket);
        else result = publishExplode(*getDesktop(), prepared, _ticket);
    } catch (...) { result = {Status::failed, "Publication failed"}; }
    if (!*alive) return;
    bool contourOnly = std::exchange(_contourOnlyPublication, false);
    _publishing = false; if (!active()) return;
    outcome(result, contourOnly && result.status == Status::unchanged ? Intent::ApplyAdjustment : intent);
    if (contourOnly && result.status == Status::changed) { endSession(_("Contour created. One Undo step restores the bitmap.")); return; }
    if (result.status == Status::changed && intent == Intent::Explode) {
        endSession();
        _idleMessage = Glib::ustring::compose(ngettext("Created and selected %1 piece.", "Created and selected %1 pieces.", count), number(count)) + "\n" + _status.get_text();
        setStatus(_idleMessage);
    }
}
void ExplodeBitmapPanel::outcome(Outcome result, Intent intent)
{
    if (result.status == Status::changed) {
        if (intent == Intent::ApplyAdjustment) { stop(); _activation->retry(); request(); _appliedTo = _identity; _notice.set_text(_(bakedNote));
            _applyMessage = _(bakedNote); }
        else endSession();
    }
    else if (result.status == Status::unchanged && intent == Intent::ApplyAdjustment) {
        if (!current()) { stale(); return; }
        // A fresh confirmation latch, without dispatching or changing the recipe.
        _consumed = 0; _activation = std::make_shared<DependencyRequest>(); _prepared.activation = _activation;
        display(State::Ready);
        _applyMessage = _("Image unchanged. No Undo step was added.");
        _status.set_text(_applyMessage);
        installPreview();
    }
    else if (result.status == Status::canceled) stale();
    else if (std::string_view(result.diagnostic) == "Final piece cap exceeded") display(State::TooMany, _(tooManyBound));
    else display(State::Failed, failureText(result));
}
}
