// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_EXPLODE_BITMAP_H
#define INKSCAPE_UI_DIALOG_EXPLODE_BITMAP_H
#include <gtkmm/button.h>
#include <gtkmm/colorbutton.h>
#include "ui/explode-bitmap-panel-preparation.h"
#include <gtkmm/checkbutton.h>
#include <gtkmm/expander.h>
#include <gtkmm/grid.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/picture.h>
#include <gtkmm/scale.h>
#include <gtkmm/spinbutton.h>
#include "ui/dialog/dialog-base.h"
#include "bitmap-copy-outcome.h"
#include "object/object-set.h"
#include "bitmap-explode-chemistry.h"
#include "ui/explode-bitmap-overlay.h"
namespace Inkscape::UI::Dialog {
class ExplodeBitmapPanel final : public DialogBase {
public:
    using Intent = Bitmap::Intent;
    enum class State { Idle, Counting, Ready, Opaque, Unsupported, Referenced, Protected, SourceEmpty,
                       AdjustmentEmpty, One, Stale, Resize, TooMany, Processing, Done, Applied, Canceled, Failed };
    struct Options { // Deterministic test seams; production uses measured platform evidence and native RAM.
        Bitmap::PlatformEvidence evidence = Bitmap::macDependencyEvidence();
        Bitmap::JobNow now = Bitmap::JobClock::now;
        Bitmap::MemoryProbe const *memory = nullptr;
        Bitmap::AllocationFault *inputFault = nullptr; // main-thread href copy only
        bool automatic = true;
        std::uint64_t outlineByteLimit = Bitmap::outlineByteLimit; // lower-only optional reservation seam
        std::uint64_t displayByteLimit = 100000000; // lower-only allocation refusal seam
        Bitmap::PanelPreparation::Observer observer;
        Bitmap::JobFunction contourWork = nullptr;
        bool rememberContours = true;
        Bitmap::JobFunction work = nullptr; // Worker-entry observation; null uses normal preparation.
    };
    ExplodeBitmapPanel();
    explicit ExplodeBitmapPanel(Options);
    ~ExplodeBitmapPanel() override;
    void refresh();
    void update() override { refresh(); }
    void activate(Intent);
    bool requiresDesktop() const override { return false; }
private:
    friend struct ExplodeBitmapPanelTest;
    void desktopReplaced() override;
    void documentReplaced() override { desktopReplaced(); }
    void selectionChanged(Selection *) override;
    void selectionModified(Selection *, guint) override;
    void on_map() override;
    void on_unmap() override;
    bool active();
    void installPreview();
    void omitOutlines();
    void outlineStatus(Bitmap::OutlineVisibility);
    sigc::connection _outlineVisibility;
    void stop();
    void request();
    void analyze();
    void endSession(Glib::ustring const & = {});
    void stale();
    bool sameSelection() const;
    bool _session = false;
    std::uint64_t _sessionGeneration = 0, _dispatchGeneration = 0;
    sigc::connection _sessionWatch;
    void receive(Bitmap::Ticket, Bitmap::JobResult);
    void display(State, Glib::ustring const & = {});
    void edited();
    void contourEdited(bool toggle = false);
    void requestContours();
    void createContourOnly();
    void contourStatus();
    bool accept(unsigned);
    bool acceptContour(unsigned);
    void rejectContour(unsigned);
    bool key(unsigned, Gdk::ModifierType);
    void original(bool);
    void setStatus(Glib::ustring const &, bool warning = false);
    void warning(Glib::ustring const &);
    void resultDetails();
    void resizeDetails();
    bool eligible() const;
    void resetComparison();
    bool current();
    bool recaptureView();
    void outcome(Bitmap::Outcome, Intent);
    void publish(Intent, Bitmap::Ticket);
    void cancelPublication();
    void deferPublication();
    void resize();
    void publishResize();
    void progress(Bitmap::JobProgress);
    void drag(bool, bool cancel = false);
    Options _options;
    State _state = State::Idle;
    Glib::ustring _applyMessage, _idleMessage;
    bool _dirty[3] = {};
    bool _focused[3] = {};
    bool _updating = false, _publishing = false, _showBefore = false, _mapped = false;
    bool _comparisonAvailable = false;
    bool _dragging = false, _drawingRefused = false, _publicationPending = false, _resizing = false;
    BitmapCopyOptions _resizeOptions;
    Glib::RefPtr<Gdk::FrameClock> _publicationClock;
    unsigned long _afterPaint = 0;
    Intent _publicationIntent = Intent::Explode;
    sigc::connection _publicationIdle;
    Bitmap::SessionRecipe _dragRecipe;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    Bitmap::Ticket _ticket = 0, _consumed = 0, _deliveryTicket = 0;
    Bitmap::Generation _viewGeneration = 0;
    Bitmap::SessionRecipe _recipe;
    Bitmap::LogicalImageIdentity _identity, _appliedTo;
    Bitmap::Prepared _prepared;
    std::shared_ptr<Bitmap::DependencyRequest> _activation = std::make_shared<Bitmap::DependencyRequest>();
    std::unique_ptr<Bitmap::DependencyLease> _dependencies;
    Bitmap::JobBytes _result;
    std::shared_ptr<Bitmap::Budget> _budget;
    Bitmap::BitmapOverlay _overlay;
    Bitmap::ContourOverlay _contourOverlay;
    Bitmap::PanelPreparation::ContourRecipe _contourRecipe;
    Bitmap::PanelPreparation::ContourResult _contourResult;
    Bitmap::ContourStyle _contourStyle;
    Bitmap::Outcome _contourAdmission;
    bool _contourJob = false, _contourOnlyPublication = false;
    State _contourBaseState = State::Ready;
    bool _contourDirty[3] = {}, _contourFocused[3] = {};
    Gtk::Expander _contour;
    Gtk::Box _contourContent{Gtk::Orientation::VERTICAL, 6};
    Gtk::CheckButton _addContour;
    Gtk::Grid _contourParameters;
    Gtk::Label _contourLabels[3], _contourColorLabel;
    Glib::RefPtr<Gtk::Adjustment> _contourAdjustments[3];
    Gtk::SpinButton _contourSpins[3];
    Gtk::Scale _contourSliders[3];
    Gtk::ColorButton _contourColor;
    Gtk::Box _contourEnd{Gtk::Orientation::HORIZONTAL};
    Gtk::Button _createContour;
    Bitmap::ClientLease _preview;
    Bitmap::Contribution _contribution;
    Bitmap::BitmapJobs _jobs;
    Gtk::ScrolledWindow _scroll;
    Gtk::Box _content{Gtk::Orientation::VERTICAL, 6};
    Gtk::Box _comparison{Gtk::Orientation::HORIZONTAL};
    Gtk::ToggleButton _before, _previewToggle;
    Gtk::Box _navigation{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Expander _transparency, _detailsExpander;
    Gtk::Box _transparencyContent{Gtk::Orientation::VERTICAL, 6};
    Gtk::Grid _parameters;
    Gtk::CheckButton _refine;
    Gtk::Label _labels[3];
    Glib::RefPtr<Gtk::Adjustment> _adjustments[3];
    Gtk::SpinButton _spins[3];
    Gtk::Scale _sliders[3];
    Gtk::Box _applyRow{Gtk::Orientation::HORIZONTAL};
    Gtk::Box _detailsContent{Gtk::Orientation::VERTICAL, 6};
    Gtk::Box _resizeControls{Gtk::Orientation::VERTICAL, 6};
    Gtk::Grid _resizeGrid;
    Gtk::Label _resizeResolution, _resizeUnit, _resizeDescription;
    Gtk::SpinButton _resizeDpi;
    Gtk::CheckButton _resizeAntialias;
    Gtk::Label _status, _notice, _details, _metadata;
    Gtk::Box _footer{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Box _footerSpace;
    Gtk::Button _apply, _primary, _cancel, _fit, _zoom;
    Gtk::ProgressBar _progress;
    Glib::RefPtr<Gtk::EventControllerKey> _keys;
    Gtk::Picture _proxy;
};
}
#endif
