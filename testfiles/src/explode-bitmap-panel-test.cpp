// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <latch>
#include <map>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#ifdef __APPLE__
#include <dlfcn.h>
#include <sys/resource.h>
#include <gdk/macos/gdkmacos.h>
#endif
#include "inkgc/gc-core.h"
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include "ui/explode-bitmap-panel-preparation.h"
#include <png.h>
#include <lcms2.h>
#include <zlib.h>
#include "bitmap-adjustment-chemistry.h"
#include <gtkmm/window.h>
#include <gtkmm/settings.h>
#include <gtkmm/snapshot.h>
#include <gdkmm/frameclock.h>
#include <gsk/gsk.h>
#include "util/scope_exit.h"
#include <gtkmm/adjustment.h>
#include "display/control/canvas-item.h"
#include "display/control/canvas-item-context.h"
#include "display/control/canvas-item-ptr.h"
#include "ui/dialog/explode-bitmap.h"
#include "ui/widget/canvas.h"
#include "desktop.h"
#include "document.h"
#include "event-log.h"
#include "display/cairo-utils.h"
#include "display/drawing.h"
#include "display/drawing-item.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "object/sp-root.h"
#include "inkscape.h"
#include "object/sp-image.h"
#include "selection.h"
#include "ui/explode-bitmap-undo.h"
#include "xml/repr.h"
#include "xml/attribute-record.h"
#include <2geom/transforms.h>
namespace Inkscape::UI::Dialog {
using namespace Bitmap;
using namespace Bitmap::PanelPreparation;
namespace {
class PanelSnapshotProbe final : public CanvasItem {
public:
    using CanvasItem::CanvasItem;
    CanvasItemContext &context() { return *_context; }
private:
    void _update(bool) override {}
    void _render(CanvasItemBuffer &) const override {}
};
std::atomic<long long> ticks{0}; JobClock::time_point now() { return JobClock::time_point(std::chrono::milliseconds(ticks.load())); }
std::atomic<unsigned> workerStarts{0}, contourStarts{0}, fullWithContours{0};
std::atomic<unsigned> phases[6];
void observePreparation(PreparationPhase phase, void *) noexcept { ++phases[unsigned(phase)]; }
JobResult countedContours(JobInput const &input, Stop stop, JobWork &work, JobReporter &reporter) {
    ++contourStarts; return calculateContours(input, stop, work, reporter);
}
JobResult refusedContours(JobInput const &, Stop, JobWork &, JobReporter &) {
    JobResult result; auto value = std::make_shared<ContourResult>();
    value->outcome = Outcome{Status::unavailable, "Test contour refusal"}; result.value.payload = value; return result;
}
JobResult countedCalculate(JobInput const &input, Stop stop, JobWork &work, JobReporter &reporter) {
    ++workerStarts;
    if (static_cast<Input const &>(*input.storage.payload).contour.enabled) ++fullWithContours;
    return calculate(input, stop, work, reporter);
}
}
struct ExplodeBitmapPanelTest : testing::Test {
    using Panel = ExplodeBitmapPanel; using State = Panel::State;
    std::unique_ptr<SPDocument> doc; std::unique_ptr<SPDesktop> desktop; std::unique_ptr<Panel> panel;
    struct Host : Gtk::Window {
        std::function<void()> painted;
        void snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const &snapshot) override {
            Gtk::Window::snapshot_vfunc(snapshot);
            if (painted) painted();
        }
    };
    std::unique_ptr<Host> host;
    void SetUp() override {
        static auto app = [] { g_setenv("INKSCAPE_APP_ID_TAG", "eb6panel", true); return new InkscapeApplication; }();
        ASSERT_TRUE(app->gtk_app()); if (!Application::exists()) Application::create(false); recordBitmapMainThread(); ticks = 0;
        host = std::make_unique<Host>();
    }
    void closePanel() { host->unset_child(); panel.reset(); }
    void TearDown() override {
        // A fatal SetUp assertion can leave the host and main-thread setup absent.
        if (!host) return;
        closePanel(); drainReaper(); EXPECT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
        if (desktop) Application::instance().remove_desktop(desktop.get());
    }
    void open(unsigned count = 2, unsigned alpha = 128, bool opaque = false, std::string attributes = {}, std::string suppliedUri = {}, MemoryProbe const *memory = nullptr, unsigned strong = 0, bool present = true, bool analyzeOnOpen = true) {
        closePanel(); drainReaper(); ASSERT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
        if (desktop) Application::instance().remove_desktop(desktop.get());
        desktop.reset(); doc.reset();
        unsigned w = std::max(16u, std::min(count, 144u) * 3), h = std::max(4u, ((count + 143) / 144) * 3);
        auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, w, h); gdk_pixbuf_fill(gdk, opaque ? 0x4070b0ff : 0);
        for (unsigned i = 0; i < count; ++i) {
            auto p = gdk_pixbuf_get_pixels(gdk) + (i / 144 * 3) * gdk_pixbuf_get_rowstride(gdk) + (i % 144 * 3) * 4;
            p[0] = 64; p[1] = 112; p[2] = 176; p[3] = i < strong ? 255 : alpha;
        }
        Pixbuf pixels(gdk); auto uri = sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(uri);
        if (!suppliedUri.empty()) *uri = std::move(suppliedUri);
        doc = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200'><image id='im' width='" + std::to_string(w*4) + "' height='" + std::to_string(h*4) + "' " + attributes + " href='" + *uri + "'/><rect id='v' width='4' height='5'/><rect id='w' x='30' width='4' height='5'/></svg>");
        ASSERT_TRUE(doc); doc->ensureUpToDate(); desktop = std::make_unique<SPDesktop>(doc->getNamedView());
        desktop->zoom_absolute({0, 0}, 0.1); desktop->getSelection()->set(image()); doc->ensureUpToDate();
        Panel::Options o; o.now = now; o.automatic = false; o.memory = memory;
        o.work = countedCalculate; o.contourWork = countedContours; o.rememberContours = false;
        o.observer = {observePreparation, nullptr};
        o.evidence = diagnosticDependencyEvidence();
        panel = std::make_unique<Panel>(o); panel->setDesktop(desktop.get());
        EXPECT_EQ(panel->_ticket, 0u); // Unmapped panels cannot dispatch.
        Application::instance().add_desktop(desktop.get());
        if (present) { host->set_child(*panel); host->present(); ASSERT_TRUE(panel->get_mapped());
            EXPECT_EQ(ticket(), 0u); EXPECT_EQ(state(), State::Idle);
            if (analyzeOnOpen) clickPrimary();
        }
    }
    void imported(std::string const &path) {
        gchar *data = nullptr; gsize size = 0;
        ASSERT_TRUE(g_file_get_contents(path.c_str(), &data, &size, nullptr));
        auto base64 = g_base64_encode(reinterpret_cast<guchar const *>(data), size);
        std::string uri = std::string("data:image/png;base64,") + base64;
        g_free(data); g_free(base64);
        open(2, 255, false, "preserveAspectRatio='none'", uri);
        auto raw = image()->pixbuf->getPixbufRaw();
        auto dpi = gdk_pixbuf_get_option(raw, "x-dpi");
        double density = dpi ? g_ascii_strtod(dpi, nullptr) : 96;
        if (density <= 0) density = 96;
        auto root = doc->getReprRoot();
        root->setAttribute("width", "210mm"); root->setAttribute("height", "297mm");
        root->setAttribute("viewBox", "0 0 210 297");
        auto number = [](double n) { std::ostringstream s; s << std::setprecision(10) << n; return s.str(); };
        auto repr = image()->getRepr();
        repr->setAttribute("width", number(image()->pixbuf->width()*25.4/density));
        repr->setAttribute("height", number(image()->pixbuf->height()*25.4/density));
        repr->setAttribute("x", "-60.79017417"); repr->setAttribute("y", "-17.39375");
        doc->ensureUpToDate();
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Imported fixture"), "");
        DocumentUndo::clearUndo(doc.get()); doc->setModifiedSinceSave(false);
        inspect();
    }
    SPImage *image() { return cast<SPImage>(doc->getObjectById("im")); }
    std::string xml() { return sp_repr_save_buf(doc->getReprDoc()).raw(); }
    void inspect() {
        panel->refresh();
        if (!panel->eligible()) { panel->endSession(); return; }
        if (state() == State::Stale || state() == State::Failed || state() == State::Idle) {
            if (state() == State::Failed) panel->endSession();
            clickPrimary();
        }
    }
    void finish() {
        ticks += 250; auto ticket = panel->_ticket; auto until = JobClock::now() + std::chrono::seconds(8);
        while ((panel->_state == State::Counting || panel->_state == State::Processing) && JobClock::now() < until) {
            if (ticket != panel->_ticket) { ticket = panel->_ticket; ticks += 250; }
            panel->_jobs.poll(); Glib::MainContext::get_default()->iteration(false); std::this_thread::yield();
        }
        ASSERT_NE(panel->_state, State::Counting) << message();
        ASSERT_NE(panel->_state, State::Processing) << message();
    }
    State state() { return panel->_state; }
    // Assert exact UTF-8 bytes, independent of locale-sensitive ustring collation.
    std::string message() { return panel->_status.get_text().raw(); }
    void field(unsigned i, char const *text) { pending(i, text); g_signal_emit_by_name(panel->_spins[i].gobj(), "activate"); }
    void refine(bool on) { panel->_refine.set_active(on); }
    bool refineEnabled() { return panel->_refine.get_sensitive(); }
    bool applyEnabled() { return panel->_apply.get_sensitive(); }
    bool explodeEnabled() { return panel->_primary.get_sensitive(); }
    std::string notice() { return panel->_notice.get_text().raw(); }
    bool enabled(unsigned i) { return panel->_spins[i].get_sensitive(); }
    void key(unsigned k, Gdk::ModifierType m = {}, bool released = false) {
        auto c = panel->_keys->gobj(); gboolean handled = false;
        if (released) g_signal_emit_by_name(c, "key-released", k, 0, static_cast<GdkModifierType>(m));
        else g_signal_emit_by_name(c, "key-pressed", k, 0, static_cast<GdkModifierType>(m), &handled);
    }
    void compare(bool before) { (before ? panel->_before : panel->_previewToggle).set_active(true); }
    bool comparing() { return panel->_before.get_active(); }
    bool comparisonEnabled() { return panel->_comparison.get_sensitive(); }
    bool routesKey(unsigned k) { return panel->key(k, {}); }
    void nativeTab() {
        EXPECT_FALSE(routesKey(GDK_KEY_Tab));
        EXPECT_TRUE(gtk_widget_child_focus(GTK_WIDGET(panel->gobj()), GTK_DIR_TAB_FORWARD));
    }
    void nativeCompareActivation() {
        g_signal_emit_by_name(panel->_before.gobj(), "activate");
        auto deadline = JobClock::now() + std::chrono::seconds(3);
        while (!comparing() && JobClock::now() < deadline) Glib::MainContext::get_default()->iteration(false);
        ASSERT_TRUE(comparing());
    }

    bool preview() { return bool(panel->_preview); }
    OutlineCamera outlineCameraState() { return panel->_overlay.camera(); }
    bool outlineCallbackRegistered() { return panel->_overlay.cameraCallbackRegistered(); }
    void competeWithRequiredTopology(Budget::Token &retained) {
        panel->_budget = std::make_shared<Budget>(384 * MiB);
        ASSERT_TRUE(panel->_budget->recheck(sampleMemory().value).ok());
        // The panel replaces an empty ledger. Pin one byte so this ceiling
        // survives dispatch; the real display and outline reservations follow.
        ASSERT_TRUE(panel->_budget->acquire(Stage::prepared,1,retained).ok());
        panel->_options.work = +[](JobInput const &job, Stop stop, JobWork &work, JobReporter &reporter) {
            auto const &input = static_cast<Input const &>(*job.storage.payload);
            EXPECT_FALSE(input.contour.enabled);
            EXPECT_EQ(job.storage.budget->limit(), 384 * MiB);
            EXPECT_TRUE(input.display);
            EXPECT_EQ(input.display->reservation.bytes(), 100000000u);
            EXPECT_TRUE(input.outlineReservation);
            if (input.outlineReservation) EXPECT_EQ(input.outlineReservation->reservation.bytes(), 64 * MiB);
            auto result = calculate(job, stop, work, reporter);
            if (input.outlineReservation) EXPECT_FALSE(input.outlineReservation->reservation);
            return result;
        };
    }
    bool outlines() { return panel->_overlay.ready(); }
    CanvasItem *outlineItem() { return panel->_overlay.canvasItem(); }
    bool drawOutlines() {
        if (!panel->_overlay.ready()) return false;
        auto probe=make_canvasitem<PanelSnapshotProbe>(desktop->getCanvasTemp());
        probe->context().setAffine(desktop->d2w());
        auto item=panel->_overlay.canvasItem(); item->update(false);
        if (!item->get_bounds()) return false;
        auto rect=item->get_bounds()->roundOutwards();
        auto surface=Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,rect.width()*2,rect.height()*2);
        auto cr=Cairo::Context::create(surface); cr->scale(2,2);
        CanvasItemBuffer b{rect,2,cr,false}; item->render(b); surface->flush();
        auto bytes=surface->get_data();
        return panel->_overlay.ready() && std::any_of(bytes,bytes+surface->get_stride()*surface->get_height(),[](auto v){return v!=0;});
    }
    void publicationFrame() {
        auto until = JobClock::now() + std::chrono::seconds(3);
        while (panel && panel->_publicationPending && JobClock::now() < until)
            Glib::MainContext::get_default()->iteration(false);
        ASSERT_FALSE(panel && panel->_publicationPending);
    }
    void activate(Intent intent) {
        auto publishing = panel->_publishing;
        panel->activate(intent);
        if (!publishing) publicationFrame();
    }
    void apply() { activate(Intent::ApplyAdjustment); }
    void explode() { activate(Intent::Explode); }
    void click(Intent intent) { g_signal_emit_by_name(intent == Intent::ApplyAdjustment ? panel->_apply.gobj() : panel->_primary.gobj(), "clicked"); }
    bool publicationPending() { return panel->_publicationPending; }
    double fraction() { return panel->_progress.get_fraction(); }
    void noObserver() { panel->stop(); panel->_dependencies.reset(); inspect(); }
    void queuedPublicationIdle() {
        auto until = JobClock::now() + std::chrono::seconds(3);
        while (panel->_afterPaint && JobClock::now() < until) Glib::MainContext::get_default()->iteration(false);
        ASSERT_EQ(panel->_afterPaint, 0u);
        ASSERT_TRUE(panel->_publicationIdle.connected()); ASSERT_TRUE(publicationPending());
    }
    void drainEvents() {
        auto until = JobClock::now() + std::chrono::milliseconds(120);
        while (JobClock::now() < until) Glib::MainContext::get_default()->iteration(false);
    }
    void expectNoWorkerStarts() {
        auto const starts = workerStarts.load();
        ticks += 251; // Cross the real runner's 250 ms debounce, even in Idle/Stale.
        auto until = JobClock::now() + std::chrono::milliseconds(120);
        do {
            panel->_jobs.poll();
            Glib::MainContext::get_default()->iteration(false);
            std::this_thread::yield();
        } while (JobClock::now() < until);
        // Reaping must not silently defer an uncanceled pending request past the oracle.
        ASSERT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
        panel->_jobs.poll();
        EXPECT_EQ(workerStarts.load(), starts);
        EXPECT_FALSE(working());
        EXPECT_EQ(activeBitmapJobs(), 0u);
    }
    struct SeededHistory { std::string baseline, first, second; HistoryUsage usage; };
    SeededHistory seedHistory() {
        SeededHistory h; h.baseline = xml();
        doc->getObjectById("v")->getRepr()->setAttribute("x", "11");
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("First fixture edit"), ""); h.first = xml();
        doc->getObjectById("w")->getRepr()->setAttribute("x", "41");
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Second fixture edit"), ""); h.second = xml();
        DocumentUndo::undo(doc.get()); doc->ensureUpToDate();
        h.usage = preflightUndo(*doc, {false, 0, 1}).usage;
        EXPECT_EQ(h.usage.undoCount, 1u); EXPECT_EQ(h.usage.redoCount, 1u);
        if (panel) { inspect(); if (state() != State::Resize) finish(); }
        return h;
    }
    void preservedHistory(SeededHistory const &h) {
        EXPECT_EQ(xml(), h.first);
        auto usage = preflightUndo(*doc, {false, 0, 1}).usage;
        EXPECT_EQ(usage.undoCount, h.usage.undoCount); EXPECT_EQ(usage.redoCount, h.usage.redoCount);
        EXPECT_EQ(usage.undoBytes, h.usage.undoBytes); EXPECT_EQ(usage.redoBytes, h.usage.redoBytes);
        DocumentUndo::redo(doc.get()); EXPECT_EQ(xml(), h.second);
        DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(), h.first);
        DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(), h.baseline);
    }
    void largeSource(unsigned w = 5001, unsigned h = 20, MemoryProbe const *memory = nullptr) {
        auto raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, w, h); gdk_pixbuf_fill(raw, 0);
        // Two broad islands survive downsampling and both antialias modes.
        auto data = gdk_pixbuf_get_pixels(raw); auto stride = gdk_pixbuf_get_rowstride(raw);
        for (unsigned y = h/4; y < 3*h/4; ++y) for (unsigned x = w/8; x < 7*w/8; ++x) {
            if (x > 3*w/8 && x < 5*w/8) continue;
            auto p = data + y*stride + 4*x; p[0] = 40; p[1] = 80; p[2] = 160; p[3] = 255;
        }
        Pixbuf pix(raw); auto uri = sp_image_encode_png_data_uri(pix); ASSERT_TRUE(uri);
        open(2, 255, false, "preserveAspectRatio='none'", *uri, memory);
        auto repr = image()->getRepr(); repr->setAttribute("x", "0.4"); repr->setAttribute("y", "0.4");
        repr->setAttribute("width", "4999.2"); repr->setAttribute("height", "10.2");
        doc->ensureUpToDate();
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture geometry"), "");
        DocumentUndo::clearUndo(doc.get()); doc->setModifiedSinceSave(false);
        inspect();
    }
    int resizeDpi() { return panel->_resizeDpi.get_value_as_int(); }
    double resizeMaximum() { return panel->_resizeDpi.get_adjustment()->get_upper(); }
    void resizeDpi(double dpi) { panel->_resizeDpi.set_value(dpi); }
    void resizeAntialias(bool on) { panel->_resizeAntialias.set_active(on); }
    void clickResize() { g_signal_emit_by_name(panel->_primary.gobj(), "clicked"); }
    std::uint64_t reserved() { return panel->_budget ? panel->_budget->reserved() : 0; }
    void refuseResize() { panel->_prepared.limits.renderUnits = 1; }
    void progressJob(JobInput job) {
        panel->_sessionWatch.disconnect(); panel->stop(); drainReaper(); ASSERT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
        panel->_ticket = panel->_jobs.request(std::move(job)); panel->display(State::Counting);
    }
    template <typename F> bool pumpUntil(F done) {
        auto until = JobClock::now() + std::chrono::seconds(3);
        while (!done() && JobClock::now() < until) { poll(); Glib::MainContext::get_default()->iteration(false); std::this_thread::yield(); }
        return done();
    }
    Ticket ticket() { return panel->_ticket; }
    bool resultReady() { return bool(panel->_result.payload); }
    bool working() { return panel->_jobs.active(); }
    void poll() { panel->_jobs.poll(); }
    void selectionCallbacks() { panel->selectionChanged(desktop->getSelection()); panel->selectionModified(desktop->getSelection(), 0); }
    void retirementSnapshot(bool close) {
        open(); refine(false); finish(); ASSERT_EQ(state(), State::Ready) << message();
        drainReaper(); ASSERT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
        auto probe = make_canvasitem<PanelSnapshotProbe>(desktop->getCanvasTemp());
        ASSERT_TRUE(outlines()); panel->_overlay.canvasItem()->update(false);
        std::weak_ptr<OutlineStorage const> output = this->output().outlines.storage;
        std::weak_ptr<Budget> budget = panel->_budget;
        probe->context().snapshot();
        if (close) closePanel(); else { panel->endSession(); }
        // Only the deferred canvas drawing can retain the retired output now.
        EXPECT_FALSE(output.expired()); EXPECT_FALSE(budget.expired());
        probe->context().unsnapshot();
        EXPECT_TRUE(output.expired()); EXPECT_EQ(budget.expired(), close);
        if (!close) { clickPrimary(); finish(); EXPECT_EQ(state(), State::Ready) << message(); EXPECT_TRUE(outlines()); }
    }
    Output const &output() { return static_cast<Output const &>(*panel->_result.payload); }
    void hooks(PublishHooks h) { panel->_prepared.hooks = h; }
    void focus(unsigned i) { if (i < 2) panel->_spins[i].grab_focus(); else panel->_before.grab_focus(); }
    bool focused(unsigned i) { return panel->_focused[i]; }
    void pending(unsigned i, char const *v) {
        // Inject raw editing text just as the focus controller permits, then
        // exercise GTK's real input/output normalization with numeric restored.
        bool numeric = panel->_spins[i].get_numeric(); panel->_spins[i].set_numeric(false);
        panel->_spins[i].set_text(v); panel->_spins[i].set_numeric(numeric);
    }
    void drag(bool start) { panel->drag(start); }
    void slider(unsigned i, double value) { panel->_sliders[i].set_value(value); }
    void worker(JobFunction work) { panel->_options.work = work; }
    void inputFault(AllocationFault *fault) { panel->_options.inputFault = fault; }
    std::uint64_t memoryLimit() { return panel->_budget->limit(); }
    void refuseOutlineReservation(std::uint64_t bytes=0) { panel->_options.outlineByteLimit=bytes; }
    void refuseDisplay() { panel->_options.displayByteLimit = 0; }
    bool proxyVisible() { return panel->_proxy.get_visible(); }
    void refuseCandidateDrawing() { panel->_prepared.limits.outlineUnits = 0; compare(true); compare(false); }
    void refuseExtentDrawing() { desktop->zoom_absolute({0,0},10000); compare(true); compare(false); }
    void omitWithFrozenReader() {
        auto probe=make_canvasitem<PanelSnapshotProbe>(desktop->getCanvasTemp());
        panel->_overlay.canvasItem()->update(false);
        auto ledger=output().budget;
        auto before=ledger->reserved(Stage::prepared);
        std::weak_ptr<OutlineStorage const> storage=output().outlines.storage;
        probe->context().snapshot();
        refuseCandidateDrawing();
        EXPECT_TRUE(output().outlines.storage); EXPECT_TRUE(outlines());
        EXPECT_FALSE(storage.expired()); EXPECT_EQ(ledger->reserved(Stage::prepared),before);
        probe->context().unsnapshot();
        EXPECT_FALSE(storage.expired()); EXPECT_EQ(ledger->reserved(Stage::prepared),before);
    }
    void selectVectors(unsigned count = 2, bool mixedAlpha = false) {
        std::vector<SPItem *> items;
        for (unsigned i = 0; i < count; ++i) {
            auto repr = doc->getReprDoc()->createElement("svg:rect");
            repr->setAttribute("x", std::to_string((i % 20)*20)); repr->setAttribute("y", std::to_string((i / 20)*20));
            repr->setAttribute("width", "6"); repr->setAttribute("height", "6");
            repr->setAttribute("opacity", mixedAlpha && i < MaxExplodePieces ? "1" : "0.5");
            doc->getReprRoot()->appendChild(repr); items.push_back(cast<SPItem>(doc->getObjectByRepr(repr))); GC::release(repr);
        }
        doc->ensureUpToDate(); DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture"), "");
        DocumentUndo::clearUndo(doc.get()); desktop->getSelection()->setList(items); inspect();
    }
#ifdef __APPLE__
    void nativeSlider(unsigned i, unsigned type, double fraction) {
        if (type == 1) {
            // The redesigned panel scrolls: its minimum window height does not
            // include the controls. Give native pointer tests a visible viewport
            // before waiting for allocation and establishing GTK pointer focus.
            host->set_default_size(300, 600);
            nativeSlider(i, 5, fraction);
            if (::testing::Test::HasFatalFailure()) return;
        }
        // Public Cocoa events enter GTK's normal range gesture arbitration via
        // its native event queue. Dynamic calls keep this C++ test target unchanged.
        auto send = dlsym(RTLD_DEFAULT, "objc_msgSend");
        auto cls = reinterpret_cast<void *(*)(char const *)>(dlsym(RTLD_DEFAULT, "objc_getClass"));
        auto sel = reinterpret_cast<void *(*)(char const *)>(dlsym(RTLD_DEFAULT, "sel_registerName"));
        ASSERT_TRUE(send && cls && sel);
        auto get = reinterpret_cast<void *(*)(void *, void *)>(send);
        auto app = get(cls("NSApplication"), sel("sharedApplication"));
        auto surface = gtk_native_get_surface(GTK_NATIVE(host->gobj()));
        auto window = gdk_macos_surface_get_native_window(GDK_MACOS_SURFACE(surface));
        reinterpret_cast<void (*)(void *, void *, void *)>(send)(window, sel("makeKeyAndOrderFront:"), nullptr);
        auto number = reinterpret_cast<long (*)(void *, void *)>(send)(window, sel("windowNumber"));
        auto &scale = panel->_sliders[i];
        gtk_test_widget_wait_for_draw(GTK_WIDGET(scale.gobj()));
        GdkRectangle range{}; gtk_range_get_range_rect(GTK_RANGE(scale.gobj()), &range);
        graphene_point_t at{float(range.x + range.width*fraction), float(range.y + range.height/2.0)}, to{};
        ASSERT_TRUE(gtk_widget_compute_point(GTK_WIDGET(scale.gobj()), GTK_WIDGET(host->gobj()), &at, &to));
        auto picked = gtk_widget_pick(GTK_WIDGET(host->gobj()), to.x, to.y, GTK_PICK_DEFAULT);
        SCOPED_TRACE(::testing::Message() << "native type=" << type
            << " host=" << host->get_width() << "x" << host->get_height()
            << " viewport=" << panel->_scroll.get_width() << "x" << panel->_scroll.get_height()
            << " point=" << to.x << "," << to.y);
        ASSERT_TRUE(scale.get_mapped()); ASSERT_TRUE(scale.get_sensitive());
        ASSERT_GT(range.width, 0); ASSERT_GT(range.height, 0);
        ASSERT_TRUE(picked && (picked == GTK_WIDGET(scale.gobj()) ||
            gtk_widget_is_ancestor(picked, GTK_WIDGET(scale.gobj()))))
            << "Native event point must hit the visible GtkRange";
        double offsetX = 0, offsetY = 0; gtk_native_get_surface_transform(GTK_NATIVE(host->gobj()), &offsetX, &offsetY);
        struct Point { double x, y; } point{to.x-offsetX, gdk_surface_get_height(surface)-(to.y-offsetY)};
        auto make = reinterpret_cast<void *(*)(void *, void *, unsigned long, Point, unsigned long, double, long, void *, long, long, float)>(send);
        auto event = make(cls("NSEvent"), sel("mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:clickCount:pressure:"),
                          type, point, 0, double(g_get_monotonic_time())/1000000, number, nullptr, 1, 1, type == 2 ? 0.f : 1.f);
        ASSERT_TRUE(event);
        // Posted Cocoa events do not change the physical button state, which
        // GDK polls separately for motion. Inject that device reading only while
        // this native event is being dispatched; GTK's range/gesture path is real.
        auto classMethod = reinterpret_cast<void *(*)(void *, void *)>(dlsym(RTLD_DEFAULT, "class_getClassMethod"));
        auto setMethod = reinterpret_cast<void *(*)(void *, void *)>(dlsym(RTLD_DEFAULT, "method_setImplementation"));
        ASSERT_TRUE(classMethod && setMethod);
        auto method = classMethod(cls("NSEvent"), sel("pressedMouseButtons")); ASSERT_TRUE(method);
        auto down = +[](void *, void *) -> unsigned long { return 1; };
        auto up = +[](void *, void *) -> unsigned long { return 0; };
        auto old = setMethod(method, reinterpret_cast<void *>(type == 1 || type == 6 ? down : up));
        struct Restore { void *method, *old; decltype(setMethod) set; ~Restore() { set(method, old); } } restore{method, old, setMethod};
        struct Delivery { GdkEventType type; unsigned count = 0; } delivered{
            type == 1 ? GDK_BUTTON_PRESS : type == 2 ? GDK_BUTTON_RELEASE : GDK_MOTION_NOTIFY};
        auto controller = gtk_event_controller_legacy_new(); gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
        auto signal = g_signal_connect(controller, "event", G_CALLBACK(+[](GtkEventControllerLegacy *, GdkEvent *e, gpointer data) -> gboolean {
            auto &delivery = *static_cast<Delivery *>(data);
            if (gdk_event_get_event_type(e) == delivery.type) ++delivery.count;
            return false;
        }), &delivered);
        gtk_widget_add_controller(GTK_WIDGET(scale.gobj()), controller);
        reinterpret_cast<void (*)(void *, void *, void *, bool)>(send)(app, sel("postEvent:atStart:"), event, false);
        auto deadline = JobClock::now() + std::chrono::seconds(3);
        while (!delivered.count && JobClock::now() < deadline) Glib::MainContext::get_default()->iteration(false);
        g_signal_handler_disconnect(controller, signal); gtk_widget_remove_controller(GTK_WIDGET(scale.gobj()), controller);
        ASSERT_GT(delivered.count, 0u) << "Native mouse event did not reach GtkRange";
    }
    struct LegacyGestureProbe {
        Glib::RefPtr<Gtk::GestureClick> gesture = Gtk::GestureClick::create();
        unsigned pressed = 0, released = 0;
    };
    std::shared_ptr<LegacyGestureProbe> legacyGestureProbe(unsigned i) {
        auto probe = std::make_shared<LegacyGestureProbe>();
        probe->gesture->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        // Separate counters avoid retaining the gesture from its own handlers.
        auto weak = std::weak_ptr<LegacyGestureProbe>(probe);
        probe->gesture->signal_pressed().connect([weak](int, double, double) { if (auto p = weak.lock()) ++p->pressed; });
        probe->gesture->signal_released().connect([weak](int, double, double) { if (auto p = weak.lock()) ++p->released; });
        panel->_sliders[i].add_controller(probe->gesture); return probe;
    }
    bool dragging() { return panel->_dragging; }
    unsigned threshold() { return panel->_recipe.threshold; }
#endif
    std::string floorPhrase() {
        return panel->_labels[2].get_text().raw() + " [" + panel->_spins[2].get_text().raw() + "]";
    }
    std::string floorTooltip() { return panel->_spins[2].get_tooltip_text().raw(); }
    void floorFocus() { panel->_spins[2].grab_focus(); }
    void unmapped() { open(2, 128, false, {}, {}, nullptr, 0, false); }
    std::shared_ptr<Output> seed(State state, unsigned count = 23, bool baked = false) {
        panel->stop();
        for (auto &dirty : panel->_dirty) dirty = false;
        panel->_recipe = query({}); panel->_recipe.bypassAlpha = baked;
        auto out = std::make_shared<Output>();
        out->count = count; out->initial = count; out->visible = 10000;
        out->smallestArea = 1142.2; out->pieces.hrefBytes = 530410;
        out->proxyWidth = 16; out->proxyHeight = 4;
        out->grid.width = 16; out->grid.height = 4; out->exactSourceMapping = true;
        if (state == State::Ready || state == State::AdjustmentEmpty) out->adjustment = std::make_shared<PreparedAlpha>();
        if (state == State::Ready || state == State::TooMany || state == State::AdjustmentEmpty) panel->_result.payload = out;
        panel->_drawingRefused = baked;
        if (state == State::Resize) {
            panel->_updating = true;
            panel->_resizeDpi.set_range(1, 96); panel->_resizeDpi.set_value(96); panel->_resizeDpi.set_text("96");
            panel->_updating = false;
        }
        panel->display(state, state == State::Resize
            ? "This image is 5001 × 20 px. Explode Bitmap supports images up to 5000 × 5000 px. Resize to continue." : "");
        if (state == State::Counting) panel->progress({Stage::input, 37, 100});
        return out;
    }
    std::string primary() { return panel->_primary.get_label().raw(); }
    std::string details() { return panel->_details.get_text().raw(); }
    void idle() { panel->endSession(); }
    void deliverObsolete(Ticket t) { panel->receive(t, {{Status::failed, "obsolete"}, {}, 0}); }
    void clickPrimary() { g_signal_emit_by_name(panel->_primary.gobj(), "clicked"); }
    void nativeSpinUpdate(unsigned i) { panel->_spins[i].update(); }
    std::string spinText(unsigned i) { return panel->_spins[i].get_text().raw(); }
    void checkNarrowDockMinimumWidth() {
        unmapped();
        auto minimumWidth = [](Gtk::Widget &widget) {
            int minimum = 0, natural = 0;
            gtk_widget_measure(widget.gobj(), GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural, nullptr, nullptr);
            return minimum;
        };
        for (auto state : {State::Idle, State::Counting, State::Ready, State::Stale, State::Resize,
                           State::AdjustmentEmpty, State::TooMany, State::Failed}) {
            SCOPED_TRACE(static_cast<int>(state));
            seed(state, 150); panel->_detailsExpander.set_expanded(true);
            EXPECT_LE(minimumWidth(*panel), 282);
            for (unsigned i = 0; i < 3; ++i) {
                SCOPED_TRACE(i);
                EXPECT_GE(minimumWidth(panel->_sliders[i]), 80);
            }
        }
        EXPECT_FALSE(panel->get_mapped()); EXPECT_FALSE(host->get_visible());
    }
    void checkUnchangedPreviewWarning() {
        unmapped();
        auto out = seed(State::Ready);
        auto const note = "Detailed preview is unavailable. The canvas shows the current image without this preview.";
        // Model the retained proxy installed when the canvas cannot show the preview.
        panel->_proxy.set_visible(true); panel->warning(note);
        auto before = xml(); auto oldTicket = ticket(); auto generation = panel->_viewGeneration;
        auto status = message();
        for (unsigned i = 0; i < 3; ++i) {
            auto value = spinText(i);
            field(i, value.c_str());
            EXPECT_EQ(message(), status); EXPECT_FALSE(panel->_status.has_css_class("dim-label"));
            EXPECT_EQ(ticket(), oldTicket); EXPECT_EQ(panel->_viewGeneration, generation);
            EXPECT_EQ(panel->_result.payload.get(), out.get()); EXPECT_EQ(state(), State::Ready);
            EXPECT_EQ(xml(), before);
        }
        // Correcting invalid input to the unchanged value restores authorization
        // and the same warning, without installing a new preview or analyzing.
        field(0, "2.5"); EXPECT_FALSE(explodeEnabled());
        field(0, "128"); EXPECT_TRUE(explodeEnabled()); EXPECT_TRUE(applyEnabled());
        EXPECT_EQ(message(), status); EXPECT_EQ(ticket(), oldTicket);
        EXPECT_EQ(panel->_viewGeneration, generation); EXPECT_EQ(panel->_result.payload.get(), out.get());
        // Before hides the proxy but keeps the session's fidelity warning.
        panel->_showBefore = true; panel->_proxy.set_visible(false);
        field(1, "40"); EXPECT_EQ(message(), status);
        EXPECT_EQ(ticket(), oldTicket); EXPECT_EQ(panel->_viewGeneration, generation);
    }
    void checkStructure() {
        EXPECT_FALSE(panel->get_mapped()); EXPECT_FALSE(host->get_visible());
        EXPECT_EQ(panel->_scroll.get_parent(), panel.get()); EXPECT_EQ(panel->_footer.get_parent(), panel.get());
        EXPECT_EQ(panel->_content.get_parent()->get_parent(), &panel->_scroll);
        EXPECT_EQ(panel->_content.get_margin_start(), 8); EXPECT_EQ(panel->_content.get_spacing(), 6);
        EXPECT_EQ(panel->_parameters.get_column_spacing(), 4); EXPECT_EQ(panel->_parameters.get_row_spacing(), 6);
        EXPECT_TRUE(panel->_transparency.get_expanded()); EXPECT_FALSE(panel->_detailsExpander.get_expanded());
        EXPECT_TRUE(panel->_primary.has_css_class("suggested-action")); EXPECT_TRUE(panel->_comparison.has_css_class("linked"));
        EXPECT_TRUE(panel->_comparison.get_homogeneous()); EXPECT_TRUE(panel->_previewToggle.get_active());
        EXPECT_FALSE(panel->_before.get_active()); EXPECT_EQ(panel->_applyRow.get_halign(), Gtk::Align::END);
        EXPECT_TRUE(panel->_footerSpace.get_hexpand());
        Gtk::PolicyType h, v; panel->_scroll.get_policy(h, v);
        EXPECT_EQ(h, Gtk::PolicyType::NEVER); EXPECT_EQ(v, Gtk::PolicyType::AUTOMATIC);
        unsigned max[] = {255, 127, 25}, values[] = {128, 40, 5};
        for (unsigned i = 0; i < 3; ++i) {
            EXPECT_EQ(panel->_spins[i].get_adjustment(), panel->_sliders[i].get_adjustment());
            EXPECT_EQ(panel->_adjustments[i]->get_lower(), 0); EXPECT_EQ(panel->_adjustments[i]->get_upper(), max[i]);
            EXPECT_EQ(panel->_spins[i].get_value_as_int(), values[i]);
            EXPECT_EQ(panel->_spins[i].get_digits(), 0u); EXPECT_TRUE(panel->_spins[i].get_numeric());
            EXPECT_EQ(panel->_spins[i].get_width_chars(), 3); EXPECT_FALSE(panel->_sliders[i].get_draw_value());
            EXPECT_EQ(panel->_labels[i].get_max_width_chars(), 8); EXPECT_TRUE(panel->_sliders[i].get_hexpand());
            EXPECT_EQ(panel->_labels[i].get_wrap_mode(), Pango::WrapMode::WORD);
        }
        EXPECT_EQ(floorPhrase(), "Ignore pixels up to (%) [5]");
        EXPECT_NE(floorTooltip().find("Ignore pixels up to 5% opacity"), std::string::npos);
        EXPECT_NE(floorTooltip().find("at or below"), std::string::npos);
        EXPECT_NE(floorTooltip().find("also when Refine transparency is off"), std::string::npos);
    }
    void checkIdleAndStates() {
        idle(); EXPECT_EQ(message(), "Click Analyze to inspect the selected image.");
        EXPECT_EQ(primary(), "Analyze"); EXPECT_TRUE(explodeEnabled()); EXPECT_FALSE(enabled(0)); EXPECT_FALSE(enabled(2));
        EXPECT_FALSE(comparisonEnabled()); EXPECT_FALSE(panel->_zoom.get_sensitive()); EXPECT_TRUE(panel->_fit.get_sensitive());
        EXPECT_TRUE(panel->_status.has_css_class("dim-label")); EXPECT_FALSE(panel->_detailsExpander.get_visible());
        seed(State::Counting); EXPECT_EQ(message(), "Analyzing image… 37% · Esc cancels");
        EXPECT_EQ(primary(), "Analyze"); EXPECT_FALSE(explodeEnabled()); EXPECT_TRUE(enabled(2));
        EXPECT_DOUBLE_EQ(fraction(), .37); EXPECT_TRUE(panel->_progress.get_visible());
        seed(State::Ready); EXPECT_EQ(message(), "23 pieces"); EXPECT_EQ(primary(), "Explode 23 pieces");
        EXPECT_TRUE(applyEnabled()); EXPECT_TRUE(explodeEnabled()); EXPECT_TRUE(panel->_zoom.get_sensitive());
        EXPECT_TRUE(panel->_detailsExpander.get_visible()); EXPECT_FALSE(panel->_progress.get_visible());
        seed(State::Ready, 1, true); EXPECT_EQ(primary(), "Explode 1 piece");
        EXPECT_EQ(message(), "1 piece. Adjustment applied.\nPiece outlines are unavailable. The piece count is exact.");
        EXPECT_FALSE(applyEnabled()); EXPECT_TRUE(panel->_zoom.get_sensitive()); EXPECT_FALSE(panel->_status.has_css_class("dim-label"));
        EXPECT_EQ(notice(), "Adjustment applied. Further adjustments change these pixels again. Use Undo to restore the previous image.");
        seed(State::TooMany, 151); EXPECT_EQ(message(), "151 pieces found. Explode Bitmap supports up to 150 pieces per operation. Adjust the transparency settings or simplify the image.");
        EXPECT_EQ(primary(), "Explode"); EXPECT_FALSE(explodeEnabled()); EXPECT_FALSE(comparisonEnabled());
        EXPECT_FALSE(panel->_zoom.get_sensitive()); EXPECT_FALSE(panel->_detailsExpander.get_visible());
        seed(State::Resize); EXPECT_EQ(primary(), "Resize"); EXPECT_TRUE(explodeEnabled()); EXPECT_FALSE(applyEnabled());
        auto before = xml(); auto resizeTicket = ticket();
        panel->activate(Intent::Explode); EXPECT_EQ(xml(), before); EXPECT_EQ(ticket(), resizeTicket);
        EXPECT_FALSE(publicationPending()); EXPECT_EQ(state(), State::Resize);
        EXPECT_FALSE(panel->_transparency.get_visible());
        EXPECT_FALSE(panel->_comparison.get_visible()); EXPECT_TRUE(panel->_resizeControls.get_visible());
        EXPECT_NE(panel->_resizeDescription.get_text().raw().find("Undo restores the original."), std::string::npos);
        seed(State::Failed); EXPECT_EQ(message(), "Could not complete the operation. No changes were made. Click Analyze to retry.");
        EXPECT_EQ(primary(), "Analyze"); EXPECT_TRUE(explodeEnabled()); EXPECT_FALSE(panel->_status.has_css_class("dim-label"));
        desktop->getSelection()->clear(); idle();
        EXPECT_EQ(message(), "Select one embedded image, then click Analyze."); EXPECT_FALSE(explodeEnabled());
        EXPECT_FALSE(panel->_transparency.get_visible()); EXPECT_FALSE(panel->_comparison.get_visible());
        panel->setDesktop(nullptr); idle(); EXPECT_EQ(message(), "Open a document, then select one embedded image.");
    }
    void checkStatistics() {
        auto out = seed(State::Ready);
        EXPECT_NE(details().find("Hidden pixels: 0% (0 mm²)"), std::string::npos);
        auto growth = g_format_size(530410); // GLib may use a nonbreaking unit separator.
        EXPECT_NE(details().find(std::string("Estimated document growth: ") + growth), std::string::npos) << details();
        EXPECT_NE(std::string(growth).find("530.4"), std::string::npos); g_free(growth);
        EXPECT_EQ(details().find("bytes"), std::string::npos);
        out->visible = 100000; out->lost = 1; out->lostArea = .001; out->smallestArea = .005; out->pieces.hrefBytes = 999;
        panel->display(State::Ready);
        EXPECT_NE(details().find("Hidden pixels: <0.1% (<0.01 mm²)"), std::string::npos);
        EXPECT_NE(details().find("Smallest piece: <0.01 mm²"), std::string::npos);
        EXPECT_NE(details().find("Estimated document growth: <1 kB"), std::string::npos);
        out->lostArea = .25; out->smallestArea = 12.34; out->pieces.hrefBytes = 0;
        panel->display(State::Ready);
        EXPECT_NE(details().find("0.25 mm²"), std::string::npos); EXPECT_NE(details().find("12.3 mm²"), std::string::npos);
        EXPECT_NE(details().find("Estimated document growth: 0 kB"), std::string::npos);
        out->visible = 0; out->count = 0; panel->display(State::AdjustmentEmpty);
        EXPECT_TRUE(details().empty()); // Missing statistics must not become invented zero rows.
        out->alpha.reduced16 = true; out->grid.sampling = GridSampling::Good; out->exactSourceMapping = false; panel->display(State::AdjustmentEmpty);
        EXPECT_NE(message().find("The result will use 8 bits per channel. Undo preserves the 16-bit original."), std::string::npos);
        EXPECT_NE(message().find("This image will be resampled. The canvas preview may differ from the exploded pieces."), std::string::npos);
        EXPECT_FALSE(panel->_status.has_css_class("dim-label"));
    }
    void checkRawValidation() {
        for (unsigned i = 0; i < 3; ++i) for (auto raw : {"", "2.5", "-1", "999", "999999999999999999999999"}) {
            SCOPED_TRACE(i);
            SCOPED_TRACE(raw); seed(State::Ready);
            pending(i, raw); nativeSpinUpdate(i); field(i, raw);
            EXPECT_EQ(spinText(i), raw); EXPECT_FALSE(explodeEnabled()); EXPECT_FALSE(applyEnabled());
            EXPECT_NE(message().find("Enter a whole number from 0 to "), std::string::npos);
            panel->display(State::Ready); // Simulate delivery during a rejected edit.
            EXPECT_EQ(spinText(i), raw); EXPECT_FALSE(explodeEnabled()); EXPECT_FALSE(applyEnabled());
            key(GDK_KEY_Escape); EXPECT_EQ(spinText(i), i == 0 ? "128" : i == 1 ? "40" : "5");
        }
        seed(State::Ready); field(0, "141"); EXPECT_EQ(panel->_recipe.threshold, 141u);
        EXPECT_EQ(panel->_sliders[0].get_value(), 141); EXPECT_EQ(spinText(0), "141");
        field(2, "6"); EXPECT_EQ(panel->_recipe.faintFloor, 6u);
        EXPECT_EQ(panel->_sliders[2].get_value(), 6); EXPECT_FALSE(routesKey(GDK_KEY_space));
        EXPECT_FALSE(routesKey(GDK_KEY_Return)); EXPECT_FALSE(routesKey(GDK_KEY_Tab));
    }
    void checkAllocation(int width, int height) {
        // Native window frames may take a couple of pixels from the requested size (macOS: 300 -> 298).
        ASSERT_NEAR(panel->get_allocated_width(), width, 2); ASSERT_NEAR(panel->get_allocated_height(), height, 2);
        ASSERT_NEAR(host->get_allocated_width(), width, 2); ASSERT_NEAR(host->get_allocated_height(), height, 2);
        auto horizontal = panel->_scroll.get_hadjustment();
        EXPECT_LE(horizontal->get_upper(), horizontal->get_page_size() + 1); // No concealed horizontal overflow.
        auto check = [&](Gtk::Widget &widget, Gtk::Widget &container, bool vertical) {
            if (!widget.get_mapped()) return;
            graphene_rect_t bounds{};
            ASSERT_TRUE(gtk_widget_compute_bounds(widget.gobj(), container.gobj(), &bounds));
            EXPECT_GE(bounds.origin.x, -.5f) << widget.get_name();
            EXPECT_LE(bounds.origin.x + bounds.size.width, container.get_width() + .5f) << widget.get_name();
            if (vertical) {
                EXPECT_GE(bounds.origin.y, -.5f) << widget.get_name();
                EXPECT_LE(bounds.origin.y + bounds.size.height, container.get_height() + .5f) << widget.get_name();
            }
            int minimum = 0, natural = 0;
            gtk_widget_measure(widget.gobj(), GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural, nullptr, nullptr);
            // measure() includes widget margins and CSS padding/borders;
            // get_width() is content only, allocated size includes CSS only.
            auto outerWidth = widget.get_allocated_width() + widget.get_margin_start() + widget.get_margin_end();
            auto outerHeight = widget.get_allocated_height() + widget.get_margin_top() + widget.get_margin_bottom();
            EXPECT_GE(outerWidth, minimum) << widget.get_name();
            gtk_widget_measure(widget.gobj(), GTK_ORIENTATION_VERTICAL, outerWidth, &minimum, &natural, nullptr, nullptr);
            EXPECT_GE(outerHeight, minimum) << widget.get_name();
        };
        for (Gtk::Widget *widget : std::initializer_list<Gtk::Widget *>{&panel->_status, &panel->_progress, &panel->_comparison,
             &panel->_navigation, &panel->_transparency, &panel->_contour, &panel->_detailsExpander, &panel->_resizeControls}) check(*widget, panel->_content, true);
        for (unsigned i = 0; i < 3; ++i) for (Gtk::Widget *widget : std::initializer_list<Gtk::Widget *>{&panel->_labels[i], &panel->_sliders[i], &panel->_spins[i]}) check(*widget, panel->_parameters, true);
        for (Gtk::Widget *widget : std::initializer_list<Gtk::Widget *>{&panel->_before, &panel->_previewToggle}) check(*widget, panel->_comparison, true);
        for (Gtk::Widget *widget : std::initializer_list<Gtk::Widget *>{&panel->_fit, &panel->_zoom}) check(*widget, panel->_navigation, true);
        for (Gtk::Widget *widget : std::initializer_list<Gtk::Widget *>{&panel->_resizeResolution, &panel->_resizeDpi, &panel->_resizeUnit}) check(*widget, panel->_resizeGrid, true);
        for (unsigned i=0; i<3; ++i) for (Gtk::Widget *widget : std::initializer_list<Gtk::Widget *>{&panel->_contourLabels[i], &panel->_contourSliders[i], &panel->_contourSpins[i]}) check(*widget, panel->_contourParameters, true);
        check(panel->_contourColor, panel->_contourParameters, true); check(panel->_createContour, panel->_contourEnd, true);
        check(panel->_refine, panel->_transparencyContent, true); check(panel->_apply, panel->_applyRow, true);
        check(panel->_metadata, panel->_detailsContent, true);
        check(panel->_details, panel->_detailsContent, true); check(panel->_notice, panel->_detailsContent, true);
        check(panel->_resizeDescription, panel->_resizeControls, true); check(panel->_resizeAntialias, panel->_resizeControls, true);
        check(panel->_footer, *panel, true); check(panel->_cancel, panel->_footer, true); check(panel->_primary, panel->_footer, true);
        // The footer remains anchored outside the viewport even when Details scrolls.
        graphene_rect_t footer{}; ASSERT_TRUE(gtk_widget_compute_bounds(GTK_WIDGET(panel->_footer.gobj()), GTK_WIDGET(panel->gobj()), &footer));
        EXPECT_NEAR(footer.origin.y + footer.size.height, panel->get_allocated_height() - 8, 1);
    }
    void paintedFrame() {
        auto clock = host->get_frame_clock(); ASSERT_TRUE(clock);
        bool painted = false;
        auto connection = g_signal_connect_after(clock->gobj(), "after-paint", G_CALLBACK(+[](GdkFrameClock *, gpointer data) {
            *static_cast<bool *>(data) = true;
        }), &painted);
        auto disconnect = scope_exit{[&] { g_signal_handler_disconnect(clock->gobj(), connection); }};
        host->queue_draw(); clock->request_phase(Gdk::FrameClock::Phase::AFTER_PAINT);
        auto deadline = g_get_monotonic_time() + 5000000;
        auto context = Glib::MainContext::get_default();
        while (!painted && g_get_monotonic_time() < deadline) {
            for (unsigned i = 0; i < 64 && !painted && context->iteration(false); ++i) {}
            if (!painted) g_usleep(1000);
        }
        ASSERT_TRUE(painted) << "Native panel frame did not finish painting";
    }
    void renderPanels(std::filesystem::path const &directory) {
        auto settings = Gtk::Settings::get_default(); ASSERT_TRUE(settings);
        bool originalDark = settings->property_gtk_application_prefer_dark_theme();
        auto restore = scope_exit{[&] { settings->property_gtk_application_prefer_dark_theme() = originalDark; closePanel(); host->close(); }};
        std::filesystem::create_directories(directory);
        struct Example { char const *name; State state; unsigned count = 23; bool baked = false; };
        Example examples[] = {{"Idle", State::Idle}, {"Analyzing", State::Counting}, {"Ready", State::Ready},
            {"Ready-contour", State::Ready}, {"Contour-refused", State::Ready},
            {"Ready-1-piece-baked", State::Ready, 1, true}, {"TooMany", State::TooMany, 151}, {"Resize", State::Resize}, {"Error", State::Failed}};
        for (bool dark : {false, true}) for (int width : {300, 500}) for (auto const &example : examples) {
            SCOPED_TRACE(example.name);
            SCOPED_TRACE(width);
            SCOPED_TRACE(dark);
            closePanel(); host->close(); host = std::make_unique<Host>();
            settings->property_gtk_application_prefer_dark_theme() = dark;
            host->set_decorated(false); host->set_resizable(false); host->set_default_size(width, 700);
            if (example.state == State::Resize) {
                auto raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 5001, 20); ASSERT_TRUE(raw); gdk_pixbuf_fill(raw, 0);
                Pixbuf pixels(raw); auto uri = sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(uri);
                open(2, 128, false, {}, *uri, nullptr, 0, false);
            } else unmapped();
            host->set_child(*panel); host->present(); paintedFrame(); ASSERT_FALSE(HasFatalFailure());
            // Same Options/fake-clock seam as behavioral tests. Mapping is Idle;
            // seed presentation without authorizing any analysis worker.
            seed(example.state, example.count, example.baked);
            if (std::string_view(example.name) == "Ready-contour" || std::string_view(example.name) == "Contour-refused") {
                panel->_contourRecipe.enabled = true; panel->_addContour.set_active(true); panel->_contour.set_expanded(true);
                if (std::string_view(example.name) == "Ready-contour") panel->_contourResult.product = std::make_shared<ContourProduct>();
                else panel->_contourResult.outcome = Outcome{Status::unavailable, "Contour memory limit"};
                panel->display(State::Ready);
            }
            auto originalXml = xml(); auto originalTicket = ticket();
            paintedFrame(); ASSERT_FALSE(HasFatalFailure()); checkAllocation(width, 700);
            EXPECT_EQ(ticket(), originalTicket); EXPECT_FALSE(working()); EXPECT_EQ(xml(), originalXml);
            auto snapshot = Gtk::Snapshot::create();
            gtk_widget_snapshot_child(GTK_WIDGET(host->gobj()), GTK_WIDGET(panel->gobj()), snapshot->gobj());
            auto node = gtk_snapshot_to_node(snapshot->gobj()); ASSERT_TRUE(node);
            auto dropNode = scope_exit{[&] { gsk_render_node_unref(node); }};
            auto surface = host->get_surface(); ASSERT_TRUE(surface);
            auto renderer = gsk_renderer_new_for_surface(surface->gobj()); ASSERT_TRUE(renderer);
            auto dropRenderer = scope_exit{[&] { gsk_renderer_unrealize(renderer); g_object_unref(renderer); }};
            graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, float(width), 700);
            auto texture = gsk_renderer_render_texture(renderer, node, &viewport); ASSERT_TRUE(texture);
            auto dropTexture = scope_exit{[&] { g_object_unref(texture); }};
            EXPECT_EQ(gdk_texture_get_width(texture), width); EXPECT_EQ(gdk_texture_get_height(texture), 700);
            auto path = directory / (std::string(example.name) + "-" + std::to_string(width) + "-" + (dark ? "dark" : "light") + ".png");
            EXPECT_TRUE(gdk_texture_save_to_png(texture, path.string().c_str()));
            // Also qualify the collapsed controls after expansion, with the
            // scroll body at both ends, without adding undocumented PNG states.
            if (panel->_detailsExpander.get_visible()) {
                panel->_detailsExpander.set_expanded(true); paintedFrame(); checkAllocation(width, 700);
                auto vertical = panel->_scroll.get_vadjustment(); vertical->set_value(vertical->get_upper());
                paintedFrame(); checkAllocation(width, 700);
            }
        }
    }
    void smallest() { g_signal_emit_by_name(panel->_zoom.gobj(), "clicked"); }
    void checkResizeValidation() {
        seed(State::Resize);
        for (auto raw : {"2.5", "-1", "0", "97", "999999999999999999999"}) {
            SCOPED_TRACE(raw);
            panel->_resizeDpi.set_numeric(false); panel->_resizeDpi.set_text(raw); panel->_resizeDpi.set_numeric(true);
            panel->_resizeDpi.update(); panel->resizeDetails();
            EXPECT_EQ(panel->_resizeDpi.get_text().raw(), raw); EXPECT_FALSE(explodeEnabled());
            EXPECT_EQ(message(), "Enter a whole number from 1 to 96.");
        }
        panel->_resizeDpi.set_text("72"); panel->_resizeDpi.update(); panel->resizeDetails();
        EXPECT_EQ(resizeDpi(), 72); EXPECT_TRUE(explodeEnabled());
        EXPECT_NE(panel->_resizeDescription.get_text().raw().find("Resized image: 48 × 12 px"), std::string::npos);
    }
    void contours(bool enabled) { panel->_addContour.set_active(enabled); }
    bool contourPreview() { return panel->_contourOverlay.ready(); }
    void contourField(unsigned i, double value) { panel->_contourAdjustments[i]->set_value(value); }
    void contourColor(char const *color) {
        panel->_contourColor.set_rgba(Gdk::RGBA(color)); g_signal_emit_by_name(panel->_contourColor.gobj(), "color-set");
    }
    void contourOnly() { g_signal_emit_by_name(panel->_createContour.gobj(), "clicked"); publicationFrame(); }
    void refuseContours() { panel->_options.contourWork = refusedContours; contourField(0, .2); finish(); }
    void checkContourControls() {
        EXPECT_FALSE(panel->_addContour.get_active()); EXPECT_FALSE(panel->_contour.get_expanded());
        EXPECT_FALSE(panel->_contourParameters.get_sensitive()); EXPECT_FALSE(panel->_createContour.get_sensitive());
        for (unsigned i=0; i<3; ++i) {
            EXPECT_EQ(panel->_contourSpins[i].get_adjustment(), panel->_contourSliders[i].get_adjustment());
            EXPECT_EQ(panel->_contourSpins[i].get_digits(), i == 1 ? 0u : 1u);
        }
        EXPECT_DOUBLE_EQ(panel->_contourAdjustments[0]->get_lower(), -10);
        EXPECT_DOUBLE_EQ(panel->_contourAdjustments[0]->get_upper(), 10);
        EXPECT_DOUBLE_EQ(panel->_contourAdjustments[1]->get_value(), 50);
        EXPECT_DOUBLE_EQ(panel->_contourAdjustments[2]->get_value(), .5);
        EXPECT_EQ(panel->_contourColor.get_rgba(), Gdk::RGBA("#ff00ff"));
    }
    void contourGeometryFixture() {
        auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 96, 64); gdk_pixbuf_fill(gdk, 0);
        for (unsigned y=0; y<64; ++y) for (unsigned x=0; x<96; ++x) {
            auto radius = std::hypot(double(x)-26, double(y)-32);
            if ((radius < 23 && radius > 10) || std::hypot(double(x)-76, double(y)-32) < 15) {
                auto p = gdk_pixbuf_get_pixels(gdk)+y*gdk_pixbuf_get_rowstride(gdk)+4*x;
                p[0]=64; p[1]=112; p[2]=176; p[3]=255;
            }
        }
        Pixbuf pixels(gdk); auto uri = sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(uri);
        open(2,255,false,{},*uri); finish(); contours(true); finish();
    }
    void checkContourPreviewDrawing() {
        ASSERT_TRUE(contourPreview()) << message(); EXPECT_FALSE(outlines());
        auto const &f = panel->_contourResult.product->fitted;
        ASSERT_EQ(f.pieceCount, 2u);
        bool hole=false, cubic=false;
        for (unsigned r=0; r<f.ringCount; ++r) hole |= f.rings()[r].depth % 2;
        for (unsigned j=0; j<f.segmentCount; ++j) cubic |= f.segments()[j].cubic;
        ASSERT_TRUE(hole); ASSERT_TRUE(cubic);
        auto probe = make_canvasitem<PanelSnapshotProbe>(desktop->getCanvasTemp());
        auto starts = workerStarts.load()+contourStarts.load();
        auto evaluate = [](ContourPoint a, ContourSegment const &s, double t) {
            double u=1-t;
            return s.cubic ? ContourPoint{u*u*u*a.x+3*u*u*t*s.c1.x+3*u*t*t*s.c2.x+t*t*t*s.end.x,
                                          u*u*u*a.y+3*u*u*t*s.c1.y+3*u*t*t*s.c2.y+t*t*t*s.end.y}
                           : ContourPoint{u*a.x+t*s.end.x,u*a.y+t*s.end.y};
        };
        for (double zoom : {1.0, 4.0}) for (int scale : {1,2}) {
            SCOPED_TRACE(zoom);
            SCOPED_TRACE(scale);
            desktop->zoom_absolute({5, 7}, zoom); probe->context().setAffine(desktop->d2w());
            auto item = panel->_contourOverlay.canvasItem(); ASSERT_TRUE(item); item->update(true);
            ASSERT_TRUE(item->get_bounds()); auto rect = item->get_bounds()->roundOutwards();
            auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, rect.width()*scale, rect.height()*scale);
            auto cr = Cairo::Context::create(surface); cr->scale(scale,scale);
            CanvasItemBuffer buffer{rect,scale,cr,false}; item->render(buffer); surface->flush();
            auto reference = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, surface->get_width(), surface->get_height());
            auto ref = Cairo::Context::create(reference); ref->set_source_rgba(1,0,1,1); ref->set_line_width(1.5);
            ref->set_line_join(Cairo::Context::LineJoin::ROUND);
            auto const &m=output().grid.pixelToDocument;
            auto project = [&](ContourPoint p) {
                // Independent source -> document -> desktop -> device projection.
                auto q=Geom::Point(m[0]*p.x+m[2]*p.y+m[4],m[1]*p.x+m[3]*p.y+m[5]);
                return (q*desktop->doc2dt()*desktop->d2w()-rect.min())*scale;
            };
            auto alpha = [](auto const &image, int x, int y) {
                if (x<0 || y<0 || x>=image->get_width() || y>=image->get_height()) return 0.0;
                std::uint32_t pixel; std::memcpy(&pixel,image->get_data()+y*image->get_stride()+4*x,4);
                return double(pixel >> 24)/255;
            };
            for (unsigned i=0; i<f.pieceCount; ++i) {
                auto isolated = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,surface->get_width(),surface->get_height());
                auto isolatedCr=Cairo::Context::create(isolated); isolatedCr->scale(scale,scale);
                CanvasItemBuffer isolatedBuffer{rect,scale,isolatedCr,false};
                panel->_contourOverlay.renderPieceForTest(isolatedBuffer,i); isolated->flush();
                auto pieceReference = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,surface->get_width(),surface->get_height());
                auto pieceCr=Cairo::Context::create(pieceReference); pieceCr->set_source_rgba(1,0,1,1); pieceCr->set_line_width(1.5);
                pieceCr->set_line_join(Cairo::Context::LineJoin::ROUND);
                auto const &piece=f.pieces()[i]; pieceCr->begin_new_path();
                Geom::OptRect pieceBounds;
                double deviceLength=0;
                for (unsigned r=piece.ringBegin; r<piece.ringEnd; ++r) {
                    auto const &ring=f.rings()[r]; auto a=ring.start; auto q=project(a); pieceCr->move_to(q.x(),q.y());
                    if (pieceBounds) pieceBounds->expandTo(q); else pieceBounds=Geom::Rect(q,q);
                    for (unsigned j=ring.begin; j<ring.end; ++j) {
                        auto const &segment=f.segments()[j];
                        // Dense Bezier evaluation, no Cairo curve_to in the oracle.
                        unsigned steps=segment.cubic ? 512 : 1;
                        for (unsigned k=1; k<=steps; ++k) {
                            auto next=project(evaluate(a,segment,double(k)/steps));
                            deviceLength+=Geom::distance(q,next); q=next; pieceBounds->expandTo(q);
                            pieceCr->line_to(q.x(),q.y());
                        }
                        for (double t : {.25,.5,.75}) {
                            auto sample=project(evaluate(a,segment,t)); double peak=0;
                            for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx)
                                peak=std::max(peak,alpha(isolated,int(std::floor(sample.x()))+dx,int(std::floor(sample.y()))+dy));
                            EXPECT_GT(peak,.25) << "piece " << i << " ring " << r << " segment " << j;
                        }
                        a=segment.end;
                    }
                    pieceCr->close_path();
                }
                pieceCr->stroke(); pieceReference->flush();
                ref->set_source(pieceReference,0,0); ref->paint(); reference->flush();
                ASSERT_TRUE(pieceBounds); pieceBounds->expandBy(3); auto box=pieceBounds->roundOutwards();
                double coverage=0, referenceCoverage=0;
                for (int y=box.top(); y<box.bottom(); ++y) for (int x=box.left(); x<box.right(); ++x) {
                    coverage+=alpha(isolated,x,y); referenceCoverage+=alpha(pieceReference,x,y);
                }
                EXPECT_NEAR(coverage,referenceCoverage,referenceCoverage*.015) << "piece " << i;
                EXPECT_NEAR(coverage/deviceLength,1.5,.12) << "device-pixel width for piece " << i;
            }
            reference->flush();
            double actualCoverage=0, expectedCoverage=0, difference=0;
            for (int y=0; y<surface->get_height(); ++y) for (int x=0; x<surface->get_width(); ++x) {
                auto actual=alpha(surface,x,y), expected=alpha(reference,x,y);
                actualCoverage+=actual; expectedCoverage+=expected; difference+=std::abs(actual-expected);
            }
            // Coverage corresponds to a 1.5 DEVICE-pixel stroke at both zooms/scales.
            EXPECT_NEAR(actualCoverage,expectedCoverage,expectedCoverage*.015);
            // Cairo adaptively flattens cubics at its default .1-pixel tolerance;
            // the independent dense reference allows the resulting edge antialiasing difference.
            EXPECT_LT(difference,expectedCoverage*.04);
        }
        EXPECT_EQ(workerStarts.load()+contourStarts.load(), starts);
    }
    void checkContourValidation() {
        for (unsigned i=0; i<3; ++i) for (auto text : {"", "-11", "101", "nan", "999999999999999999999", "0.11", "1.5", "1.0"}) {
            if (i != 1 && (std::string_view(text)=="1.5" || std::string_view(text)=="1.0")) continue;
            for (unsigned action=0; action<3; ++action) {
                SCOPED_TRACE(i);
                SCOPED_TRACE(text);
                SCOPED_TRACE(action);
                open(); finish(); contours(true); finish(); auto before=xml(); auto old=contourValueForTest(i);
                panel->_contourSpins[i].set_numeric(false); panel->_contourSpins[i].set_text(text);
                panel->_contourSpins[i].update();
                if (action==0) explode(); else if (action==1) contourOnly(); else apply();
                EXPECT_FALSE(publicationPending()); EXPECT_EQ(xml(),before); EXPECT_EQ(state(),State::Ready);
                EXPECT_EQ(panel->_contourSpins[i].get_text(),text);
                EXPECT_NE(message().find(i==1 ? "Enter a whole number from 0 to 100." : i==0 ? "Enter a number from -10 to 10." : "Enter a number from 0 to 5."),std::string::npos);
                EXPECT_FALSE(explodeEnabled()); EXPECT_FALSE(applyEnabled()); EXPECT_FALSE(contourOnlyEnabled());
                panel->display(State::Ready); EXPECT_EQ(panel->_contourSpins[i].get_text(),text); EXPECT_FALSE(explodeEnabled());
                key(GDK_KEY_Escape); EXPECT_DOUBLE_EQ(panel->_contourSpins[i].get_value(),old);
                EXPECT_TRUE(explodeEnabled()); EXPECT_TRUE(contourOnlyEnabled());
                panel->_contourSpins[i].set_text(i==1 ? "65" : "0.2"); panel->acceptContour(i); finish();
                EXPECT_DOUBLE_EQ(contourValueForTest(i),i==1 ? 65 : .2); EXPECT_TRUE(explodeEnabled());
            }
        }
    }
    double contourValueForTest(unsigned i) { return i==0 ? panel->_contourRecipe.offsetMm : i==1 ? panel->_contourRecipe.smoothing : panel->_contourRecipe.gapToleranceMm; }
    void checkContourQuantization() {
        open(); finish(); contours(true); finish(); panel->_options.rememberContours=true;
        for (auto [i,value,expected] : {std::tuple{0u,.26,.3},std::tuple{0u,-.26,-.3},std::tuple{1u,65.4,65.},std::tuple{2u,.46,.5}}) {
            contourField(i,value); EXPECT_DOUBLE_EQ(contourValueForTest(i),expected);
            EXPECT_DOUBLE_EQ(panel->_contourAdjustments[i]->get_value(),expected);
            EXPECT_DOUBLE_EQ(panel->_contourSpins[i].get_value(),expected);
            double displayed=0; std::istringstream(panel->_contourSpins[i].get_text().raw()) >> displayed; EXPECT_DOUBLE_EQ(displayed,expected);
        }
        Panel::Options options; options.rememberContours=true; Panel remembered(options);
        EXPECT_DOUBLE_EQ(remembered._contourRecipe.offsetMm,-.3); EXPECT_DOUBLE_EQ(remembered._contourRecipe.smoothing,65);
        EXPECT_DOUBLE_EQ(remembered._contourRecipe.gapToleranceMm,.5);
        // Restore the process defaults for subsequent isolated tests.
        contourField(0,0); contourField(1,50); panel->_addContour.set_active(false); panel->_options.rememberContours=false;
    }
    void checkContourOffClassification() {
        for (auto expected : {State::TooMany, State::Opaque, State::AdjustmentEmpty, State::Failed}) {
            open(expected==State::TooMany ? 151 : 2, expected==State::AdjustmentEmpty ? 1 : expected==State::Opaque ? 255 : 128, expected==State::Opaque,
                 {},{},nullptr,expected==State::TooMany ? 150 : 0);
            contours(true); finish();
            if (expected==State::Failed) {
                const_cast<Output &>(output()).explodeOutcome=Outcome{Status::unavailable,"Test retained failure"};
                panel->display(State::Failed,"Test retained failure");
            }
            ASSERT_EQ(state(),expected) << message(); auto previous=message(); auto count=workerStarts.load(); auto before=xml();
            contours(false); EXPECT_EQ(state(),expected); EXPECT_FALSE(explodeEnabled());
            EXPECT_NE(message().find(previous),std::string::npos); EXPECT_FALSE(panel->_contourParameters.get_sensitive());
            explode(); EXPECT_EQ(xml(),before); EXPECT_EQ(workerStarts,count);
        }
    }
    void checkContourOffDuringInitialAnalysis() {
        open(); contours(true); ASSERT_EQ(state(),State::Counting);
        ASSERT_TRUE(panel->_contourParameters.get_sensitive()); contours(false);
        for (unsigned i=0; i<3; ++i) { EXPECT_FALSE(panel->_contourSpins[i].is_sensitive()); EXPECT_FALSE(panel->_contourSliders[i].is_sensitive()); }
        EXPECT_FALSE(panel->_contourColor.is_sensitive()); EXPECT_EQ(state(),State::Counting); finish();
    }
    void checkContourRefusalWithPreviewWarnings() {
        open(); finish(); contours(true); finish(); refuseContours(); panel->omitOutlines(); panel->installPreview();
        EXPECT_NE(message().find("Contours unavailable: Test contour refusal. Explode will create pieces without contours."),std::string::npos);
        EXPECT_NE(message().find("Piece outlines are unavailable. The piece count is exact."),std::string::npos);
        EXPECT_TRUE(explodeEnabled());
    }
    void refreshPanel() { panel->refresh(); }
    void checkContourLayouts() {
        auto settings = Gtk::Settings::get_default(); bool originalDark = settings->property_gtk_application_prefer_dark_theme();
        auto restore = scope_exit{[&] { settings->property_gtk_application_prefer_dark_theme() = originalDark; }};
        for (bool dark : {false,true}) for (int width : {300,500}) {
            SCOPED_TRACE(width);
            SCOPED_TRACE(dark);
            closePanel(); host->close(); host = std::make_unique<Host>();
            settings->property_gtk_application_prefer_dark_theme() = dark;
            host->set_decorated(false); host->set_resizable(false); host->set_default_size(width,700);
            open(2,255); finish(); contours(true); finish(); paintedFrame(); checkAllocation(width,700);
            EXPECT_EQ(panel->_contourColor.get_halign(),Gtk::Align::START);
            EXPECT_GE(panel->_contourColor.get_width(),48); EXPECT_LE(panel->_contourColor.get_width(),64);
            EXPECT_LT(panel->_contourColor.get_width(),panel->_contourSliders[0].get_width());
            refuseContours(); paintedFrame(); checkAllocation(width,700);
        }
    }
    bool contourOnlyEnabled() { return panel->_createContour.get_sensitive(); }
    void staleContourIdentity() {
        ++const_cast<Output &>(output()).grid.recipeHash; // retained identity cannot authorize this request
        contourField(0, .3);
    }
    void storm() { for (unsigned i = 0; i < 10000; ++i) panel->_sliders[0].set_value(i % 200); }
};
TEST_F(ExplodeBitmapPanelTest, NativeWidgetTreeAndSharedAdjustmentsWithoutWindow) { unmapped(); checkStructure(); }
TEST_F(ExplodeBitmapPanelTest, IdleAndResultPresentationWithoutWindow) { unmapped(); checkIdleAndStates(); }
TEST_F(ExplodeBitmapPanelTest, HumanStatisticsAndVisibleWarningsWithoutWindow) { unmapped(); checkStatistics(); }
TEST_F(ExplodeBitmapPanelTest, SpinRejectionSurvivesGtkNormalizationWithoutWindow) { unmapped(); checkRawValidation(); }
TEST_F(ExplodeBitmapPanelTest, ResizeSpinValidationAndLiveDimensionsWithoutWindow) { unmapped(); checkResizeValidation(); }
TEST_F(ExplodeBitmapPanelTest, NarrowDockMinimumWidthWithoutWindow) { checkNarrowDockMinimumWidth(); }
TEST_F(ExplodeBitmapPanelTest, UnchangedCommitRetainsPreviewFidelityWarningWithoutWindow) { checkUnchangedPreviewWarning(); }
TEST_F(ExplodeBitmapPanelTest, SpanishPanelLabelsAndPluralMessagesWithoutWindow) {
    std::ifstream po(std::filesystem::path(INKSCAPE_TESTS_DIR).parent_path() / "po/es.po");
    ASSERT_TRUE(po); std::string translations{std::istreambuf_iterator<char>(po), {}};
    for (auto entry : {
        "msgid \"Ignore pixels up to (%)\"\nmsgstr \"Ignorar píxeles hasta (%)\"",
        "msgid \"Ignore pixels up to %1%% opacity\"\nmsgstr \"Ignorar píxeles hasta %1%% de opacidad\"",
        "msgid \"Analyze\"\nmsgstr \"Analizar\"",
        "msgid \"Explode %1 piece\"\nmsgid_plural \"Explode %1 pieces\"\nmsgstr[0] \"Explotar %1 pieza\"\nmsgstr[1] \"Explotar %1 piezas\""
    }) EXPECT_NE(translations.find(entry), std::string::npos) << entry;
}
TEST_F(ExplodeBitmapPanelTest, PanelCanBeRenderedForVisualRegressionReview) {
    auto directory = std::getenv("INKSCAPE_EB_PANEL_RENDER_DIR");
    if (!directory || !*directory) GTEST_SKIP() << "Set INKSCAPE_EB_PANEL_RENDER_DIR on an isolated display to render the real panel.";
    renderPanels(directory);
}
TEST_F(ExplodeBitmapPanelTest, AnalysisOnDemandIdleStaleCancelAndDoneDispatchCounters) {
    open(2, 128, false, {}, {}, nullptr, 0, true, false);
    auto before = xml(); EXPECT_EQ(ticket(), 0u); EXPECT_FALSE(enabled(0));
    panel->update(); panel->refresh(); selectionCallbacks(); drainEvents(); expectNoWorkerStarts();
    EXPECT_EQ(ticket(), 0u); EXPECT_FALSE(resultReady()); EXPECT_EQ(xml(), before);
    desktop->getSelection()->clear(); desktop->getSelection()->set(image());
    image()->getRepr()->setAttribute("x", "2"); DocumentUndo::done(doc.get(), Util::Internal::ContextString("Move"), "");
    DocumentUndo::undo(doc.get()); DocumentUndo::redo(doc.get()); doc->ensureUpToDate();
    panel->refresh(); drainEvents(); expectNoWorkerStarts(); EXPECT_EQ(ticket(), 0u);
    clickPrimary(); EXPECT_EQ(ticket(), 1u); finish(); ASSERT_EQ(state(), State::Ready) << message();
    ASSERT_TRUE(preview()); ASSERT_TRUE(outlines());
    auto installed = outlineItem(); ASSERT_NE(installed, nullptr);
    auto selected = desktop->getSelection()->items_vector();
    desktop->getSelection()->addList(selected); drainEvents(); expectNoWorkerStarts();
    EXPECT_TRUE(preview()); EXPECT_TRUE(outlines()); EXPECT_EQ(outlineItem(), installed);
    EXPECT_EQ(ticket(), 1u); EXPECT_EQ(state(), State::Ready); EXPECT_TRUE(resultReady());
    image()->getRepr()->setAttribute("x", "3"); DocumentUndo::done(doc.get(), Util::Internal::ContextString("Second move"), ""); drainEvents();
    EXPECT_EQ(state(), State::Stale); EXPECT_EQ(ticket(), 1u); EXPECT_FALSE(resultReady());
    EXPECT_FALSE(preview()); EXPECT_FALSE(outlines()); EXPECT_TRUE(details().empty());
    field(0, "150"); panel->update(); panel->refresh(); EXPECT_EQ(ticket(), 1u);
    deliverObsolete(1); expectNoWorkerStarts(); EXPECT_EQ(state(), State::Stale); EXPECT_FALSE(resultReady());
    doc->ensureUpToDate(); clickPrimary(); EXPECT_EQ(ticket(), 2u); finish(); ASSERT_EQ(state(), State::Ready) << message();
    key(GDK_KEY_Escape); EXPECT_EQ(state(), State::Idle); EXPECT_EQ(ticket(), 2u); EXPECT_TRUE(panel->get_visible());
    panel->refresh(); drainEvents(); expectNoWorkerStarts(); EXPECT_EQ(ticket(), 2u);
    clickPrimary(); EXPECT_EQ(ticket(), 3u); finish(); explode();
    EXPECT_EQ(state(), State::Idle) << message(); EXPECT_EQ(ticket(), 3u); EXPECT_FALSE(resultReady());
    EXPECT_NE(message().find("Created and selected 2 pieces."), std::string::npos);
    panel->refresh(); drainEvents(); expectNoWorkerStarts(); EXPECT_EQ(ticket(), 3u);
}
TEST_F(ExplodeBitmapPanelTest, RecipeEditPreparationFailureSurvivesWatcherAndAnalyzeRetries) {
    struct Memory : MemoryProbe {
        bool available = true;
        bool read(RawMemory &m) const noexcept override {
            m = {16384*MiB, 8192*MiB, 256*MiB}; return available;
        }
    } memory;
    open(2, 128, false, {}, {}, &memory); finish(); ASSERT_EQ(state(), State::Ready);
    auto old = ticket(); auto starts = workerStarts.load(); auto before = xml();
    memory.available = false; field(0, "140");
    ASSERT_EQ(state(), State::Failed); auto failure = message();
    EXPECT_NE(failure.find("memory"), std::string::npos) << failure;
    EXPECT_EQ(primary(), "Analyze"); EXPECT_TRUE(explodeEnabled());
    EXPECT_FALSE(preview()); EXPECT_FALSE(outlines()); EXPECT_FALSE(resultReady());
    drainEvents(); panel->refresh(); selectionCallbacks(); expectNoWorkerStarts();
    EXPECT_EQ(state(), State::Failed); EXPECT_EQ(message(), failure); EXPECT_EQ(ticket(), old);
    EXPECT_EQ(workerStarts.load(), starts); EXPECT_EQ(xml(), before);
    memory.available = true; clickPrimary(); EXPECT_EQ(ticket(), old + 1); finish();
    EXPECT_EQ(workerStarts.load(), starts + 1); EXPECT_EQ(state(), State::Ready) << message();
    EXPECT_EQ(query(logicalImageIdentity(*image())).threshold, 140u); EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, NativeUndoRedoAndLateDeliveryRequireExplicitAnalyze) {
    open(); finish(); auto old = ticket();
    image()->getRepr()->setAttribute("x", "5");
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Move image"), "");
    doc->ensureUpToDate(); drainEvents(); expectNoWorkerStarts(); ASSERT_EQ(state(), State::Stale); EXPECT_EQ(ticket(), old);
    clickPrimary(); ASSERT_EQ(ticket(), ++old); finish(); ASSERT_EQ(state(), State::Ready);
    DocumentUndo::undo(doc.get()); doc->ensureUpToDate(); drainEvents(); expectNoWorkerStarts();
    EXPECT_EQ(state(), State::Stale); EXPECT_EQ(ticket(), old); EXPECT_FALSE(resultReady());
    clickPrimary(); ASSERT_EQ(ticket(), ++old); // Invalidate a result while its delivery is still pending.
    DocumentUndo::redo(doc.get()); doc->ensureUpToDate(); drainEvents();
    deliverObsolete(old); expectNoWorkerStarts(); finish(); EXPECT_EQ(state(), State::Stale); EXPECT_EQ(ticket(), old);
    EXPECT_FALSE(resultReady()); EXPECT_FALSE(preview()); EXPECT_FALSE(outlines());
    clickPrimary(); ASSERT_EQ(ticket(), ++old); finish(); EXPECT_EQ(state(), State::Ready);
}
TEST_F(ExplodeBitmapPanelTest, AnotherEligibleBitmapEndsSessionUntilAnalyze) {
    open(); finish();
    auto copy = image()->getRepr()->duplicate(doc->getReprDoc()); copy->setAttribute("id", "second");
    doc->getReprRoot()->appendChild(copy); GC::release(copy); doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Add second bitmap"), "");
    inspect(); finish(); auto old = ticket();
    desktop->getSelection()->set(doc->getObjectById("second"));
    EXPECT_EQ(state(), State::Idle); EXPECT_EQ(ticket(), old); EXPECT_FALSE(resultReady());
    EXPECT_EQ(message(), "Selection changed. Click Analyze to inspect this image.");
    panel->refresh(); expectNoWorkerStarts(); EXPECT_EQ(ticket(), old); clickPrimary(); EXPECT_EQ(ticket(), old + 1); finish();
    EXPECT_EQ(state(), State::Ready);
}
TEST_F(ExplodeBitmapPanelTest, SelectionChangeCancelsQueuedPublicationAndNeverReanalyzes) {
    open(); finish(); auto before = xml(); auto old = ticket();
    click(Intent::Explode); ASSERT_TRUE(publicationPending());
    desktop->getSelection()->set(doc->getObjectById("v"));
    publicationFrame(); expectNoWorkerStarts(); EXPECT_EQ(state(), State::Idle); EXPECT_EQ(ticket(), old); EXPECT_EQ(xml(), before);
    EXPECT_EQ(message(), "Explode Bitmap works on embedded images. For vectors, use Path > Break Apart.");
    desktop->getSelection()->set(image()); panel->refresh(); expectNoWorkerStarts(); EXPECT_EQ(state(), State::Idle); EXPECT_EQ(ticket(), old);
    EXPECT_FALSE(resultReady()); EXPECT_FALSE(enabled(0));
}
TEST_F(ExplodeBitmapPanelTest, CompatibleToolsKeepSessionTextInvalidatesWithoutDispatch) {
    open(); finish(); ASSERT_EQ(state(), State::Ready); auto old = ticket();
    for (auto tool : {"/tools/zoom", "/tools/select", "/tools/shapes/rect", "/tools/select"}) {
        desktop->setTool(tool); drainEvents(); expectNoWorkerStarts(); EXPECT_EQ(state(), State::Ready) << tool << " " << message(); EXPECT_EQ(ticket(), old);
    }
    desktop->setTool("/tools/text"); drainEvents(); expectNoWorkerStarts(); EXPECT_EQ(state(), State::Stale); EXPECT_EQ(ticket(), old);
    EXPECT_FALSE(resultReady()); desktop->setTool("/tools/select"); drainEvents(); expectNoWorkerStarts(); EXPECT_EQ(state(), State::Stale);
}
TEST_F(ExplodeBitmapPanelTest, DirectVectorRejectionPreservesXmlAndUndoAtEverySize) {
    for (unsigned count : {1u, 2u, 151u, 300u}) {
        open(); selectVectors(count, true); auto before = xml(); auto old = ticket();
        auto history = preflightUndo(*doc, {false, 0, 1}).usage;
        panel->refresh(); clickPrimary(); panel->activate(Intent::ConversionCandidate); expectNoWorkerStarts(); finish();
        EXPECT_EQ(state(), State::Idle); EXPECT_FALSE(resultReady()); EXPECT_EQ(ticket(), old); EXPECT_EQ(xml(), before);
        EXPECT_FALSE(explodeEnabled()); EXPECT_FALSE(enabled(0));
        if (count == 1) EXPECT_EQ(message(), "Explode Bitmap works on embedded images. For vectors, use Path > Break Apart.");
        auto after = preflightUndo(*doc, {false, 0, 1}).usage;
        EXPECT_EQ(after.undoCount, history.undoCount); EXPECT_EQ(after.redoCount, history.redoCount);
    }
}
TEST_F(ExplodeBitmapPanelTest, AnalyzeButtonUsesExistingRefresh) {
    open(); finish(); auto before = xml(); auto old = ticket();
    idle(); EXPECT_EQ(primary(), "Analyze"); EXPECT_TRUE(explodeEnabled());
    clickPrimary(); EXPECT_GT(ticket(), old); EXPECT_EQ(state(), State::Counting);
    EXPECT_EQ(xml(), before); finish(); EXPECT_EQ(state(), State::Ready); EXPECT_EQ(output().count, 2u);
}
TEST_F(ExplodeBitmapPanelTest, SmallestPieceNavigatesOnePieceWithoutOutlinesOrAnalysis) {
    open(1); finish(); ASSERT_EQ(state(), State::Ready); compare(true); ASSERT_FALSE(outlines());
    auto before = xml(); auto old = ticket(); auto selected = desktop->getSelection()->singleItem();
    auto zoom = desktop->current_zoom(); smallest(); expectNoWorkerStarts(); EXPECT_NE(desktop->current_zoom(), zoom);
    EXPECT_EQ(ticket(), old); EXPECT_EQ(xml(), before); EXPECT_EQ(desktop->getSelection()->singleItem(), selected);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
}
TEST_F(ExplodeBitmapPanelTest, RetiredRefineOffOutputSurvivesCloseSnapshot) { retirementSnapshot(true); }
TEST_F(ExplodeBitmapPanelTest, RetiredRefineOffOutputSurvivesRefreshSnapshot) { retirementSnapshot(false); }
TEST_F(ExplodeBitmapPanelTest, HiddenAndUnmappedSelectionAndUpdateDoNotDispatch) {
    open(); finish(); ASSERT_EQ(state(), State::Ready) << message(); auto before = xml();
    key(GDK_KEY_Escape); EXPECT_EQ(state(), State::Idle); key(GDK_KEY_Escape); ASSERT_FALSE(panel->get_visible()); ASSERT_FALSE(panel->get_mapped()); auto old = ticket();
    desktop->getSelection()->clear(); desktop->getSelection()->set(image());
    selectionCallbacks();
    panel->update(); panel->refresh(); expectNoWorkerStarts();
    EXPECT_EQ(ticket(), old); EXPECT_FALSE(working()); EXPECT_FALSE(preview()); EXPECT_FALSE(outlines());
    panel->set_visible(true); ASSERT_TRUE(panel->get_mapped()); EXPECT_EQ(ticket(), old); clickPrimary();
    finish(); EXPECT_EQ(ticket(), old + 1); EXPECT_TRUE(preview()); EXPECT_TRUE(outlines()); EXPECT_EQ(xml(), before);
    host->set_visible(false); ASSERT_TRUE(panel->get_visible()); ASSERT_FALSE(panel->get_mapped()); old = ticket();
    desktop->getSelection()->clear(); desktop->getSelection()->set(image()); panel->update(); panel->refresh();
    expectNoWorkerStarts(); EXPECT_EQ(ticket(), old); EXPECT_FALSE(preview()); EXPECT_FALSE(outlines());
    host->present(); ASSERT_TRUE(panel->get_mapped()); EXPECT_EQ(ticket(), old); clickPrimary();
    finish(); EXPECT_EQ(ticket(), old + 1); EXPECT_TRUE(preview()); EXPECT_TRUE(outlines());
}
TEST_F(ExplodeBitmapPanelTest, T25BeforeTogglePersistsThroughRecipeUpdateAndPreviewRestoresWithoutAnalysis) {
    open(); ASSERT_EQ(state(), State::Counting); EXPECT_FALSE(comparisonEnabled()); finish();
    ASSERT_TRUE(comparisonEnabled()); auto before = xml();
    compare(true); field(0, "140"); ASSERT_EQ(state(), State::Counting); auto old = ticket();
    EXPECT_TRUE(comparing()); EXPECT_TRUE(comparisonEnabled()); finish(); ASSERT_EQ(state(), State::Ready) << message();
    EXPECT_TRUE(comparing());
    EXPECT_EQ(ticket(), old); EXPECT_TRUE(resultReady()); EXPECT_FALSE(preview()); EXPECT_FALSE(outlines());
    compare(false); expectNoWorkerStarts(); EXPECT_EQ(ticket(), old); EXPECT_TRUE(preview()); EXPECT_TRUE(outlines()); EXPECT_EQ(xml(), before);
    inspect(); compare(true); finish(); ASSERT_EQ(state(), State::Ready) << message();
    desktop->getSelection()->clear(); compare(false); EXPECT_FALSE(preview()); EXPECT_FALSE(outlines()); EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, T25T30PreviewOriginalRefineAndNativeUndoRedo) {
    open(); auto before = xml(); finish(); ASSERT_EQ(state(), State::Ready) << message();
    EXPECT_TRUE(preview()); EXPECT_TRUE(outlines()); compare(true); EXPECT_FALSE(preview()); EXPECT_FALSE(outlines());
    compare(false); EXPECT_TRUE(preview()); EXPECT_TRUE(outlines()); EXPECT_EQ(xml(), before);
    refine(false); finish(); ASSERT_EQ(state(), State::Ready) << message(); EXPECT_FALSE(enabled(0)); EXPECT_FALSE(enabled(1)); EXPECT_TRUE(enabled(2)); EXPECT_FALSE(preview()); EXPECT_FALSE(applyEnabled());
    EXPECT_FALSE(query(logicalImageIdentity(*image())).refine); explode(); ASSERT_EQ(state(), State::Idle) << message();
    ASSERT_EQ(desktop->getSelection()->size(), 2u); auto after = xml(); explode(); EXPECT_EQ(xml(), after);
    for (auto item : desktop->getSelection()->items()) {
        auto im = cast<SPImage>(item); ASSERT_TRUE(im); auto p = std::make_shared<Pixbuf>(*im->pixbuf); p->ensurePixelFormat(Pixbuf::PF_GDK);
        bool found = false; for (int y = 0; y < p->height(); ++y) for (int x = 0; x < p->width(); ++x) {
            auto a = p->pixels()[y*p->rowstride()+4*x+3]; if (a) { EXPECT_EQ(a, 128); found = true; }
        } EXPECT_TRUE(found);
    }
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); desktop->getSelection()->set(image()); finish(); EXPECT_EQ(xml(), before);
    EXPECT_FALSE(query(logicalImageIdentity(*image())).refine);
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK); EXPECT_EQ(xml(), after);
}
TEST_F(ExplodeBitmapPanelTest, T30FieldsDebounceEscapeAndApplyOutcome) {
    open(); auto before = xml(); finish(); ASSERT_EQ(state(), State::Ready) << message();
    field(0, "999"); EXPECT_NE(message().find("whole number"), std::string::npos); EXPECT_EQ(xml(), before);
    key(GDK_KEY_Escape); EXPECT_EQ(state(), State::Ready); field(0, "140"); EXPECT_EQ(state(), State::Counting);
    EXPECT_EQ(query(logicalImageIdentity(*image())).threshold, 140u); EXPECT_EQ(xml(), before); finish();
    ASSERT_EQ(state(), State::Ready) << message(); apply(); ASSERT_EQ(state(), State::Counting) << message();
    EXPECT_NE(xml(), before); EXPECT_NE(notice().find("Adjustment applied. Further adjustments"), std::string::npos); inspect(); key(GDK_KEY_Escape); EXPECT_EQ(state(), State::Idle);
    key(GDK_KEY_Escape); EXPECT_FALSE(panel->get_visible()); EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
}
TEST_F(ExplodeBitmapPanelTest, StatesEmptyOpaqueSourceEmptyAdjustmentEmptyAndOne) {
    open(0); finish(); EXPECT_EQ(state(), State::SourceEmpty); EXPECT_NE(message().find("original image has no visible pixels"), std::string::npos);
    open(2, 255, true); finish(); EXPECT_EQ(state(), State::Opaque); EXPECT_FALSE(enabled(0));
    open(2, 1); finish(); EXPECT_EQ(state(), State::AdjustmentEmpty); EXPECT_TRUE(enabled(0)); auto before = xml(); explode(); EXPECT_EQ(xml(), before);
    open(1); finish(); EXPECT_EQ(state(), State::Ready); EXPECT_EQ(output().count, 1u); EXPECT_TRUE(explodeEnabled());
    desktop->getSelection()->clear(); EXPECT_EQ(state(), State::Idle); EXPECT_EQ(message(), "Select one embedded image, then click Analyze.");
}
TEST_F(ExplodeBitmapPanelTest, ProductLimitReadyPublishesExactlyCap) {
    open(MaxExplodePieces); auto before = xml(); finish(); ASSERT_EQ(state(), State::Ready) << message();
    EXPECT_EQ(output().count, MaxExplodePieces); EXPECT_EQ(output().pieces.count(), MaxExplodePieces); EXPECT_TRUE(outlines());
    EXPECT_TRUE(explodeEnabled()); explode(); ASSERT_EQ(state(), State::Idle) << message();
    EXPECT_EQ(desktop->getSelection()->size(), MaxExplodePieces);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
    DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, ProductLimitRefusesCapPlusOneAndTwiceCapBeforePngAndControlsRecover) {
    for (unsigned count : {MaxExplodePieces + 1, 2 * MaxExplodePieces}) {
        open(count, 128, false, {}, {}, nullptr, MaxExplodePieces);
        image()->getRepr()->setAttribute("x", "3"); DocumentUndo::done(doc.get(), Util::Internal::ContextString("Move"), "");
        DocumentUndo::undo(doc.get()); inspect(); auto before = xml();
        auto history = preflightUndo(*doc, {false, 0, 1}).usage;
        finish(); ASSERT_EQ(state(), State::TooMany) << message();
        EXPECT_EQ(message(), std::to_string(count) + " pieces found. Explode Bitmap supports up to 150 pieces per operation. Adjust the transparency settings or simplify the image.");
        EXPECT_EQ(output().count, count); EXPECT_FALSE(output().pngStarted); EXPECT_FALSE(output().adjustment);
        EXPECT_EQ(output().pieces.count(), 0u); EXPECT_FALSE(output().outlines.storage); EXPECT_FALSE(outlines());
        EXPECT_TRUE(enabled(0)); EXPECT_TRUE(enabled(1)); EXPECT_TRUE(refineEnabled()); EXPECT_FALSE(explodeEnabled());
        explode(); apply(); EXPECT_EQ(xml(), before);
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, history.undoCount);
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.redoCount, history.redoCount);
        slider(0, 200); finish(); ASSERT_EQ(state(), State::Ready) << message();
        EXPECT_EQ(output().count, MaxExplodePieces); EXPECT_TRUE(explodeEnabled()); EXPECT_TRUE(outlines()); EXPECT_EQ(xml(), before);
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, history.undoCount);
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.redoCount, history.redoCount);
    }
}
TEST_F(ExplodeBitmapPanelTest, T29SliderStormOnlyLatestRecipeAndT28HardCap) {
    open(); auto before = xml(); storm(); EXPECT_EQ(state(), State::Counting); EXPECT_EQ(activeBitmapJobs(), 0u);
    field(0, "128"); finish(); EXPECT_EQ(state(), State::Ready) << message(); EXPECT_EQ(xml(), before);
    open(20001); finish(); EXPECT_EQ(state(), State::TooMany) << message(); EXPECT_TRUE(enabled(0)); before = xml(); explode(); EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, EngineCapRemainsARefusalWithoutEncoding) {
    open(20001); auto before = xml(); finish();
    ASSERT_EQ(state(), State::TooMany) << message(); EXPECT_FALSE(applyEnabled()); EXPECT_FALSE(explodeEnabled());
    EXPECT_EQ(message(), "More than 20,000 pieces found. Explode Bitmap supports up to 150 pieces per operation. Adjust the transparency settings or simplify the image.");
    EXPECT_FALSE(output().pngStarted); EXPECT_EQ(output().pieces.count(), 0u); EXPECT_FALSE(output().outlines.storage);
    EXPECT_TRUE(enabled(0)); EXPECT_TRUE(enabled(1)); explode(); apply(); EXPECT_EQ(xml(), before);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
}
TEST_F(ExplodeBitmapPanelTest, Round3ApplySourcePixelsAndICCDespiteUnsafeGridExpansion) {
    std::vector<unsigned char> samples(256 * 256 * 4), bytes, icc;
    unsigned char alphas[] = {0, 64, 128, 192, 255};
    for (unsigned y = 0; y < 256; ++y) for (unsigned x = 0; x < 256; ++x) {
        auto p = samples.data() + (y * 256 + x) * 4;
        p[0] = x; p[1] = y; p[2] = x ^ y; p[3] = alphas[(x + 2*y) % 5];
    }
    auto profile = cmsCreate_sRGBProfile(); cmsUInt32Number n = 0; cmsSaveProfileToMem(profile, nullptr, &n);
    icc.resize(n); cmsSaveProfileToMem(profile, icc.data(), &n); cmsCloseProfile(profile);
    auto png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    auto info = png_create_info_struct(png); ASSERT_EQ(setjmp(png_jmpbuf(png)), 0);
    png_set_write_fn(png, &bytes, [](png_structp p, png_bytep data, png_size_t size) {
        auto &out = *static_cast<std::vector<unsigned char> *>(png_get_io_ptr(p)); out.insert(out.end(), data, data + size);
    }, [](png_structp) {});
    png_set_IHDR(png, info, 256, 256, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_set_iCCP(png, info, "source", 0, icc.data(), icc.size()); png_write_info(png, info);
    for (unsigned y = 0; y < 256; ++y) png_write_row(png, samples.data() + y * 1024);
    png_write_end(png, info); png_destroy_write_struct(&png, &info);
    auto base64 = g_base64_encode(bytes.data(), bytes.size());
    std::string uri = std::string("data:image/png;base64,") + base64; g_free(base64);
    open(1, 128, false, "transform='matrix(1,0,100,1,0,0)'", uri);
    image()->getRepr()->setAttribute("width", "256"); image()->getRepr()->setAttribute("height", "256");
    doc->ensureUpToDate(); DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture"), "");
    DocumentUndo::clearUndo(doc.get()); inspect(); field(0, "100"); field(1, "0"); finish();
    ASSERT_EQ(state(), State::Failed) << message(); ASSERT_TRUE(applyEnabled());
    EXPECT_EQ(message(), "This image is too complex to process safely. No changes were made.");
    EXPECT_STREQ(output().explodeOutcome.diagnostic, "Unsafe final-grid expansion.");
    EXPECT_TRUE(preview()); EXPECT_TRUE(enabled(0)); EXPECT_FALSE(explodeEnabled());
    auto before = xml(); auto object = image(); explode(); EXPECT_EQ(xml(), before);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
    apply(); ASSERT_EQ(state(), State::Counting) << message(); auto after = xml(); EXPECT_NE(after, before);
    EXPECT_EQ(image(), object); EXPECT_EQ(desktop->getSelection()->singleItem(), object);
    EXPECT_STREQ(image()->getRepr()->attribute("transform"), "matrix(1,0,100,1,0,0)");
    EXPECT_STREQ(image()->getRepr()->attribute("width"), "256"); EXPECT_STREQ(image()->getRepr()->attribute("height"), "256");
    auto href = image()->getRepr()->attribute("href"); gsize size;
    auto encoded = g_base64_decode(href + 22, &size);
    Budget budget(Budget::FixedLimitForTest{}, 1536 * MiB); auto result = decode({{encoded, size, {}}, {}}, budget); g_free(encoded);
    ASSERT_TRUE(result.ok()) << result.outcome.diagnostic;
    EXPECT_EQ(result.value.width, 256u); EXPECT_EQ(result.value.height, 256u); EXPECT_EQ(result.value.orientation, 1u);
    ASSERT_EQ(result.value.profileBytes, icc.size()); EXPECT_EQ(std::memcmp(result.value.profile.data(), icc.data(), icc.size()), 0);
    auto rgba = reinterpret_cast<unsigned char const *>(result.value.pixels.data());
    for (unsigned i = 0; i < 256*256; ++i) {
        ASSERT_EQ(std::memcmp(rgba + 4*i, samples.data() + 4*i, 3), 0) << i;
        ASSERT_EQ(rgba[4*i+3], samples[4*i+3] < 100 ? 0 : 255) << i;
    }
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
    finish(); EXPECT_FALSE(applyEnabled()); EXPECT_FALSE(explodeEnabled()); EXPECT_TRUE(enabled(0));
    EXPECT_EQ(message(), "This image is too complex to process safely. No changes were made.");
    EXPECT_STREQ(output().explodeOutcome.diagnostic, "Unsafe final-grid expansion.");
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); inspect(); finish(); EXPECT_EQ(xml(), before); EXPECT_TRUE(applyEnabled());
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK); inspect(); finish(); EXPECT_EQ(xml(), after);
    field(0, "140"); finish(); EXPECT_TRUE(applyEnabled());
    field(2, "0"); refine(false); finish(); EXPECT_FALSE(applyEnabled()); EXPECT_TRUE(refineEnabled());
}
TEST_F(ExplodeBitmapPanelTest, Round2ApplyThenExplodeUsesBakedPixelsWithoutReselection) {
    open(); field(0, "140"); finish(); ASSERT_TRUE(applyEnabled());
    apply(); ASSERT_EQ(state(), State::Counting) << message(); finish();
    ASSERT_EQ(state(), State::Ready) << message(); EXPECT_TRUE(explodeEnabled()); EXPECT_FALSE(applyEnabled());
    EXPECT_TRUE(query(logicalImageIdentity(*image())).bypassAlpha);
    EXPECT_NE(notice().find("Adjustment applied."), std::string::npos);
    inspect(); finish(); // A late dependency/view refresh must retain the applied disclosure.
    EXPECT_NE(message().find("2 pieces. Adjustment applied."), std::string::npos);
    explode(); ASSERT_EQ(state(), State::Idle) << message(); ASSERT_EQ(desktop->getSelection()->size(), 2u);
    for (auto item : desktop->getSelection()->items()) {
        auto im = cast<SPImage>(item); ASSERT_TRUE(im); Pixbuf p(*im->pixbuf); p.ensurePixelFormat(Pixbuf::PF_GDK);
        bool visible = false;
        for (int y = 0; y < p.height(); ++y) for (int x = 0; x < p.width(); ++x) {
            auto a = p.pixels()[y * p.rowstride() + 4 * x + 3];
            if (a) { EXPECT_EQ(a, 72); visible = true; } // T=140/S=40 maps 128 to 72; a second pass hides it.
        }
        EXPECT_TRUE(visible);
    }
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 2u);
}
TEST_F(ExplodeBitmapPanelTest, Round2ByteIdenticalApplyKeepsEditingAndFreshExplode) {
    open(); field(0, "100"); field(1, "0"); finish(); apply(); finish();
    ASSERT_EQ(state(), State::Ready) << message(); auto baked = xml();
    doc->getObjectById("v")->getRepr()->setAttribute("x", "7");
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Move fixture"), "");
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); inspect(); finish(); EXPECT_EQ(xml(), baked);
    field(1, "1"); finish(); EXPECT_FALSE(query(logicalImageIdentity(*image())).bypassAlpha);
    field(0, "101"); finish(); ASSERT_TRUE(applyEnabled());
    EXPECT_FALSE(query(logicalImageIdentity(*image())).bypassAlpha); // Editing starts a new adjustment.
    auto history = preflightUndo(*doc, {false, 0, 1}).usage;
    ASSERT_EQ(history.redoCount, 1u);
    auto old = ticket(); apply(); EXPECT_EQ(ticket(), old); ASSERT_EQ(state(), State::Ready) << message();
    EXPECT_NE(message().find("unchanged"), std::string::npos); EXPECT_TRUE(enabled(0));
    finish(); ASSERT_EQ(state(), State::Ready) << message(); EXPECT_EQ(xml(), baked);
    EXPECT_NE(message().find("No Undo step"), std::string::npos);
    EXPECT_TRUE(enabled(0)); EXPECT_TRUE(enabled(1)); EXPECT_TRUE(applyEnabled()); EXPECT_TRUE(explodeEnabled());
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, history.undoCount);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.redoCount, history.redoCount);
    explode(); EXPECT_EQ(state(), State::Idle) << message(); EXPECT_EQ(desktop->getSelection()->size(), 2u);
}
TEST_F(ExplodeBitmapPanelTest, UnsupportedReferencedAndProtectedBeforeControls) {
    open(2,128,false,"style='display:none'"); EXPECT_EQ(state(), State::Protected) << message(); EXPECT_FALSE(enabled(0));
    open(2,128,false,"mask='url(#unknown)'"); EXPECT_NE(state(), State::Ready);
    open(); image()->getRepr()->setAttribute("href", "file:///unavailable-panel-source.png"); doc->ensureUpToDate(); inspect();
    EXPECT_EQ(state(), State::Idle); EXPECT_FALSE(enabled(0));
}
TEST_F(ExplodeBitmapPanelTest, T25ApplyPixelsEffectsRepeatedUndoRedoAndRefineOff) {
    open(2, 128, false, "opacity='0.6' transform='matrix(1,0.2,0,1,3,4)'"); finish();
    Filters::BitmapToneSettings tone; tone.brightness = 12;
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image(), tone));
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture"), ""); DocumentUndo::clearUndo(doc.get());
    inspect(); finish(); ASSERT_EQ(state(), State::Ready) << message(); auto before = xml();
    auto id = logicalImageIdentity(*image()); auto object = image();
    apply(); ASSERT_EQ(state(), State::Counting) << message(); auto once = xml();
    EXPECT_EQ(image(), object); EXPECT_FALSE(preview()); EXPECT_TRUE(query(id).bypassAlpha); EXPECT_TRUE(enabled(0));
    EXPECT_FALSE(applyEnabled()); apply(); EXPECT_EQ(xml(), once); finish(); EXPECT_TRUE(explodeEnabled());
    auto decodeCurrent = [&] {
        auto uri = inspectUri(image()->getRepr()->attribute("href"), {}); EXPECT_TRUE(uri.ok());
        gsize n; auto bytes = g_base64_decode(image()->getRepr()->attribute("href") + uri.value.payloadOffset, &n);
        auto b = std::make_unique<Budget>(Budget::FixedLimitForTest{}, 1536 * MiB);
        auto decoded = decode({{bytes, n, {}}, {}}, *b); g_free(bytes); EXPECT_TRUE(decoded.ok());
        if (decoded.ok()) {
            auto rgba = reinterpret_cast<unsigned char const *>(decoded.value.pixels.data());
            EXPECT_EQ(rgba[0], 64); EXPECT_EQ(rgba[1], 112); EXPECT_EQ(rgba[2], 176);
            EXPECT_EQ(rgba[3], 128); // Independent T=128/S=40 midpoint; no tone or opacity baked.
            EXPECT_EQ(decoded.value.width, 16u); EXPECT_EQ(decoded.value.height, 4u);
        }
    }; decodeCurrent();
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); inspect(); finish(); EXPECT_EQ(xml(), before); EXPECT_FALSE(query(id).bypassAlpha);
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK); inspect(); finish();
    EXPECT_EQ(xml(), once); EXPECT_TRUE(query(id).bypassAlpha); EXPECT_FALSE(preview());
    field(0, "100"); finish(); EXPECT_TRUE(applyEnabled()); apply(); ASSERT_EQ(state(), State::Counting) << message();
    auto twice = xml(); EXPECT_NE(twice, once); EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 2u);
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); inspect(); finish(); EXPECT_EQ(xml(), once);
    field(2, "0"); refine(false); finish(); EXPECT_FALSE(applyEnabled()); auto history = preflightUndo(*doc, {false, 0, 1}).usage;
    apply(); EXPECT_EQ(xml(), once); EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.redoCount, history.redoCount);
}
TEST_F(ExplodeBitmapPanelTest, T25ApplyFullyHiddenResultKeepsSourceAndUndo) {
    open(2, 1); auto before = xml(); finish(); ASSERT_EQ(state(), State::AdjustmentEmpty) << message();
    ASSERT_TRUE(applyEnabled()); apply(); ASSERT_EQ(state(), State::Counting) << message();
    ASSERT_TRUE(image()); EXPECT_EQ(desktop->getSelection()->singleItem(), image());
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); finish(); EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, T25ApplySixteenBitWithUnchangedICCAndOriginalByteUndo) {
    std::vector<unsigned char> bytes, icc;
    auto profile = cmsCreate_sRGBProfile(); cmsUInt32Number n = 0; cmsSaveProfileToMem(profile, nullptr, &n);
    icc.resize(n); cmsSaveProfileToMem(profile, icc.data(), &n); cmsCloseProfile(profile);
    auto png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr); auto info = png_create_info_struct(png);
    ASSERT_EQ(setjmp(png_jmpbuf(png)), 0);
    png_set_write_fn(png, &bytes, [](png_structp p, png_bytep data, png_size_t size) {
        auto &out = *static_cast<std::vector<unsigned char> *>(png_get_io_ptr(p)); out.insert(out.end(), data, data + size);
    }, [](png_structp) {});
    png_set_IHDR(png, info, 16, 4, 16, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_set_iCCP(png, info, "source", 0, icc.data(), icc.size()); png_write_info(png, info);
    unsigned char row[128] = {}; row[0] = 64; row[2] = 112; row[4] = 176; row[6] = 128;
    for (unsigned y = 0; y < 4; ++y) png_write_row(png, row);
    png_write_end(png, info); png_destroy_write_struct(&png, &info);
    auto base64 = g_base64_encode(bytes.data(), bytes.size()); std::string uri = std::string("data:image/png;base64,") + base64; g_free(base64);
    open(1, 128, false, {}, uri); auto before = xml(); finish(); ASSERT_EQ(state(), State::Ready) << message();
    EXPECT_NE(notice().find("The result will use 8 bits per channel. Undo preserves the 16-bit original."), std::string::npos);
    apply(); ASSERT_EQ(state(), State::Counting) << message(); auto after = xml();
    gsize size; auto encoded = g_base64_decode(image()->getRepr()->attribute("href") + 22, &size);
    Budget budget(Budget::FixedLimitForTest{}, 1536 * MiB); auto result = decode({{encoded, size, {}}, {}}, budget); g_free(encoded);
    ASSERT_TRUE(result.ok()) << result.outcome.diagnostic; EXPECT_FALSE(result.value.reduced16);
    ASSERT_EQ(result.value.profileBytes, icc.size()); EXPECT_EQ(std::memcmp(result.value.profile.data(), icc.data(), icc.size()), 0);
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); finish(); EXPECT_EQ(xml(), before); EXPECT_STREQ(image()->getRepr()->attribute("href"), uri.c_str());
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK); finish(); EXPECT_EQ(xml(), after);
}
TEST_F(ExplodeBitmapPanelTest, ProxyAdmissionFlowersAnd100MPWithPressureAndOverlap) {
    Memory ample{16384*MiB, 8192*MiB, 256*MiB, true};
    for (auto dimensions : {std::pair{1125u, 844u}, std::pair{10000u, 10000u}}) {
        auto [w, h] = dimensions; auto size = proxySize(w, h);
        EXPECT_LE(size.width, 1024u); EXPECT_LE(size.height, 1024u); EXPECT_LE(size.bytes*2, 4*MiB);
        auto plan = resources(w, h, MiB, 0); auto admitted = admit(plan, ample);
        ASSERT_TRUE(admitted.ok()) << admitted.outcome.diagnostic;
        EXPECT_EQ(admitted.value.termCaps[unsigned(Term::preview)], size.bytes*2);
        EXPECT_GT(admitted.value.termCaps[unsigned(Term::topology)], size.bytes*2);
        EXPECT_TRUE(admit(resources(w, h, MiB, 1536*MiB), ample).ok());
        EXPECT_FALSE(admit(resources(w, h, MiB, 8192*MiB), ample).ok());
    }
    EXPECT_FALSE(admit(resources(10000, 10000, MiB, 0), {4096*MiB, 1024*MiB, 256*MiB, true}).ok());
    EXPECT_EQ(proxySize(16385, 10).bytes, 0u);
    EXPECT_FALSE(admit(resources(0, 10, MiB, 0), ample).ok());
    EXPECT_FALSE(admit(resources(~0u, ~0u, MiB, 0), ample).ok());
    EXPECT_FALSE(admit(resources(16, 4, UINT64_MAX, 0), ample).ok());
}
TEST_F(ExplodeBitmapPanelTest, RealImageTopologyAdmissionOnEightGiBAndGrowthIsBounded) {
    // Keep the original 192 MiB pressure oracle under the shared tree's
    // available-minus-recovery admission policy (concurrent budget task).
    Memory normal{8192*MiB, 448*MiB, 512*MiB, true};
    std::uint64_t limit = 0; ASSERT_TRUE(admissionLimit(normal, limit).ok()); ASSERT_EQ(limit, 192*MiB);
    for (auto [w, h] : {std::pair{1254u, 1254u}, {3000u, 3000u}}) {
        auto accepted = admit(resources(w, h, 10*MiB, 0), normal);
        ASSERT_TRUE(accepted.ok()) << accepted.outcome.diagnostic;
        EXPECT_LT(accepted.value.termCaps[unsigned(Term::topology)], 5*MiB);
    }
    // The raised source ceiling does not waive RAM admission: 8P alone is
    // 200,000,000 bytes, before proxy/topology/recovery, against J=192 MiB.
    EXPECT_FALSE(admit(resources(5000, 5000, 10*MiB, 0), normal).ok());
    Budget budget(Budget::FixedLimitForTest{}, 1024);
    PlainBuffer held, next; ASSERT_TRUE(held.allocate(budget, Stage::topology, 1000, 1).ok());
    AllocationFault fault;
    EXPECT_FALSE(next.allocate(budget, Stage::topology, 25, 1, &fault).ok());
    EXPECT_EQ(fault.attempts, 0u); EXPECT_EQ(budget.reserved(), 1000u);
    EXPECT_TRUE(next.allocate(budget, Stage::topology, 24, 1).ok());
}
TEST_F(ExplodeBitmapPanelTest, ProxySamplesFaultStopAndReservationRetirement) {
    auto budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 1536*MiB);
    std::vector<unsigned char> source(1125*844*4, 128); auto original = source;
    RgbaView view{source.data(), source.size(), 1125*4, 1125, 844}; auto lut = alphaLut(140, 40);
    {
        Output out; out.budget = budget; AllocationFault fault{1};
        EXPECT_FALSE(makeProxy(view, lut, false, out, {}, &fault).ok()); EXPECT_EQ(budget->reserved(), 0u);
        auto stopped = std::make_shared<std::atomic<bool>>(true);
        EXPECT_EQ(makeProxy(view, lut, false, out, Stop(stopped)).status, Status::canceled);
    }
    {
        Output out; out.budget = budget; ASSERT_TRUE(makeProxy(view, lut, false, out).ok());
        EXPECT_LE(budget->reserved(Stage::preview), 4*MiB); EXPECT_EQ(budget->reserved(), out.proxy.size()*2);
        auto rgba = reinterpret_cast<unsigned char const *>(out.proxy.data());
        EXPECT_EQ(rgba[0], 128); EXPECT_EQ(rgba[3], 72); EXPECT_EQ(source, original);
    }
    EXPECT_EQ(budget->reserved(), 0u);
    Output tight; tight.budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, proxySize(1125, 844).bytes);
    EXPECT_FALSE(makeProxy(view, lut, false, tight).ok());
    EXPECT_EQ(tight.budget->reserved(), 0u); EXPECT_EQ(tight.proxy.size(), 0u);
}
TEST_F(ExplodeBitmapPanelTest, P2RefusedDisplayKeepsPublicationAndCanonicalView) {
    open(2, 128, false, {}, {}, nullptr, 0, true, false);
    auto before = xml(); refuseDisplay();
    clickPrimary(); finish();
    ASSERT_EQ(state(), State::Ready) << message(); EXPECT_FALSE(preview()); EXPECT_TRUE(proxyVisible());
    EXPECT_FALSE(output().display); EXPECT_TRUE(applyEnabled()); EXPECT_TRUE(explodeEnabled());
    EXPECT_NE(message().find("Detailed preview is unavailable. The canvas shows the current image without this preview."), std::string::npos);
    EXPECT_EQ(xml(), before); auto starts = workerStarts.load(); smallest(); expectNoWorkerStarts();
    EXPECT_EQ(workerStarts.load(), starts); compare(true); compare(false); EXPECT_FALSE(preview());
    key(GDK_KEY_Escape); EXPECT_EQ(xml(), before); EXPECT_FALSE(preview());
}
TEST_F(ExplodeBitmapPanelTest, FlowersSizeProxyAndFullGridAreSeparateAndNeverSerialized) {
    auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 1125, 844); gdk_pixbuf_fill(gdk, 0);
    auto data = gdk_pixbuf_get_pixels(gdk); data[3] = 128; data[4*1124+3] = 128;
    Pixbuf pixels(gdk); auto uri = sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(uri);
    open(2, 128, false, {}, *uri); auto before = xml(); finish();
    ASSERT_EQ(state(), State::Ready) << message(); EXPECT_FALSE(proxyVisible()); EXPECT_TRUE(preview());
    EXPECT_EQ(output().grid.width, 1125u); EXPECT_EQ(output().grid.height, 844u);
    EXPECT_LT(output().proxyWidth, 1125u); EXPECT_EQ(output().count, 2u); EXPECT_TRUE(outlines());
    EXPECT_EQ(output().budget->reserved(Stage::preview), output().proxy.size()*2);
    EXPECT_GE(output().budget->reserved(Stage::composition), 1125u*844u*4);
    EXPECT_GE(output().budget->reserved(Stage::prepared), output().outlines.storage->bytes());
    EXPECT_EQ(xml(), before); EXPECT_EQ(message().find("Detailed preview is unavailable."), std::string::npos);
    ASSERT_TRUE(output().display); EXPECT_EQ(output().display->width, 1125u); EXPECT_EQ(output().display->height, 844u);
    auto backing = output().display;
    auto starts = workerStarts.load(); auto generation = ticket();
    desktop->zoom_absolute({0, 0}, 8); desktop->set_display_area(Geom::Point(0, 0), Geom::Point(13, -7));
    drainEvents(); expectNoWorkerStarts();
    EXPECT_EQ(workerStarts.load(), starts); EXPECT_EQ(ticket(), generation); EXPECT_EQ(output().display, backing);
    auto firstNotice = notice();
    compare(true); EXPECT_FALSE(proxyVisible()); EXPECT_FALSE(outlines()); compare(false); EXPECT_FALSE(proxyVisible()); EXPECT_TRUE(preview());
    EXPECT_EQ(notice(), firstNotice);
}
TEST_F(ExplodeBitmapPanelTest, WindowNativeTabTogglesArrowsEnterAndDragEscapeKeepHistoryClean) {
    open(); finish(); auto before = xml(); focus(0); pending(0, "141"); nativeTab();
    EXPECT_FALSE(focused(0)); EXPECT_EQ(query(logicalImageIdentity(*image())).threshold, 141u);
    focus(0); for (unsigned i = 0; i < 20; ++i) key(GDK_KEY_Up);
    key(GDK_KEY_Return); finish(); EXPECT_EQ(query(logicalImageIdentity(*image())).threshold, 161u);
    drag(true); slider(0, 180); slider(1, 10); key(GDK_KEY_Escape); finish();
    EXPECT_EQ(query(logicalImageIdentity(*image())).threshold, 161u); EXPECT_EQ(query(logicalImageIdentity(*image())).softness, 40u);
    EXPECT_EQ(xml(), before); EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
    focus(2); EXPECT_FALSE(routesKey(GDK_KEY_space)); EXPECT_FALSE(routesKey(GDK_KEY_Return));
    nativeCompareActivation(); EXPECT_TRUE(comparing()); EXPECT_FALSE(preview());
    focus(0); EXPECT_TRUE(comparing()); EXPECT_FALSE(preview()); // Persistent across focus loss.
    compare(false); EXPECT_TRUE(preview());
    focus(0); pending(0, "999"); focus(1); EXPECT_NE(message().find("whole number"), std::string::npos);
    key(GDK_KEY_Escape); EXPECT_TRUE(panel->get_visible());
}
TEST_F(ExplodeBitmapPanelTest, T28PublicationReentryTeardownAndDeferredOutput) {
    for (bool close : {false, true}) {
        open(); finish(); ASSERT_EQ(state(), State::Ready) << message();
        auto before = xml();
        struct Context { ExplodeBitmapPanelTest *test; bool close, called = false; std::vector<std::string> saved; } c{this, close};
        hooks({[](PublishStage stage, unsigned, void *data) {
            auto &c = *static_cast<Context *>(data); if (stage != PublishStage::Insert || c.called) return;
            c.called = true; c.test->explode();
            EXPECT_TRUE(DocumentUndo::whenPublicationCompletable(c.test->doc.get(), [&c](SPDocument &d) {
                c.saved.push_back(sp_repr_save_buf(d.getReprDoc()).raw());
            }, [](SPDocument &) {}));
            if (c.close) c.test->closePanel();
        }, &c});
        explode(); EXPECT_TRUE(c.called); ASSERT_EQ(desktop->getSelection()->size(), close ? 1u : 2u);
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, close ? 0u : 1u);
        if (close) EXPECT_EQ(xml(), before); // Closing invalidates the session; native publication rolls back.
        DocumentUndo::flushPublicationCompletions(doc.get());
        ASSERT_EQ(c.saved.size(), 1u); EXPECT_EQ(c.saved.front(), xml());
    }
}
TEST_F(ExplodeBitmapPanelTest, HistoryApplyPreviewExplodeAndPreviousEditRemainSeparate) {
    open(); finish(); auto original = xml();
    image()->getRepr()->setAttribute("x", "3"); DocumentUndo::done(doc.get(), Util::Internal::ContextString("Move"), "");
    inspect(); field(0, "140"); finish(); auto moved = xml(); apply(); finish(); auto baked = xml();
    field(0, "90"); finish(); explode(); ASSERT_EQ(state(), State::Idle) << message(); auto pieces = xml();
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); desktop->getSelection()->set(image()); finish(); EXPECT_EQ(xml(), baked);
    EXPECT_EQ(query(logicalImageIdentity(*image())).threshold, 90u);
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); finish(); EXPECT_EQ(xml(), moved);
    EXPECT_EQ(query(logicalImageIdentity(*image())).threshold, 140u);
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); finish(); EXPECT_EQ(xml(), original);
    for (unsigned i = 0; i < 3; ++i) key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK);
    EXPECT_EQ(xml(), pieces);
}
TEST_F(ExplodeBitmapPanelTest, HistoryToneApplyToneAndZeroOneCancelPreserveRedo) {
    open(); finish(); auto original = xml();
    auto tone = [&](double v) { Filters::BitmapToneSettings t; t.brightness = v; ASSERT_TRUE(BitmapAdjustments::apply_tone(image(), t));
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Tone"), ""); inspect(); finish(); };
    tone(10); auto firstTone = xml(); apply(); finish(); auto applied = xml(); tone(20);
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); finish(); EXPECT_EQ(xml(), applied);
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); finish(); EXPECT_EQ(xml(), firstTone);
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); finish(); EXPECT_EQ(xml(), original);
    for (unsigned n : {0u, 1u}) {
        open(n); image()->getRepr()->setAttribute("x", "7"); DocumentUndo::done(doc.get(), Util::Internal::ContextString("Move"), "");
        inspect(); finish(); key(GDK_KEY_Escape); DocumentUndo::undo(doc.get()); auto before = xml();
        auto redo = preflightUndo(*doc, {false, 0, 1}).usage.redoCount;
        panel->set_visible(true); finish(); if (!n) explode(); key(GDK_KEY_Escape);
        EXPECT_EQ(xml(), before); EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.redoCount, redo);
    }
}
TEST_F(ExplodeBitmapPanelTest, ExactLinkedSvgReferenceProtectionAndResamplingMessages) {
    open(2, 128, false, "opacity='0'"); EXPECT_EQ(message(), "This image is fully hidden by object or layer opacity. Increase its opacity before exploding it.");
    open(); image()->getRepr()->setAttribute("href", "file:///missing-eb6.png"); inspect();
    EXPECT_EQ(message(), "Embed this linked image before adjusting or exploding it.");
    image()->getRepr()->setAttribute("href", "data:image/svg+xml;base64,PHN2Zy8+"); inspect();
    EXPECT_EQ(message(), "This embedded image contains SVG artwork. Use an embedded raster image.");
    open(2, 128, false, "style='display:none'"); EXPECT_EQ(message(), "This image is inside locked or hidden object/layer “im”. Unlock or show it first.");
    open(2, 128, false, "transform='rotate(45)'"); finish(); EXPECT_NE(message().find("This image will be resampled. The canvas preview may differ from the exploded pieces."), std::string::npos);
}

TEST_F(ExplodeBitmapPanelTest, PreparationLatchedStopCloseRetainsLedgerUntilWorkerRetires) {
    open(); finish(); drainReaper(); ASSERT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
    struct Gates { std::latch entered{1}, release{1}; } gates;
    auto budget = std::make_shared<Budget>(1536*MiB); ASSERT_TRUE(budget->recheck(sampleMemory().value).ok());
    auto input = std::make_shared<Input>(); input->outlineReservation = reserveOutlines(budget).value; input->target = resolve(*desktop, Intent::Explode).value;
    input->recipe = Recipe{128, 40}; input->recipe.checkpointData = &gates;
    input->recipe.checkpoint = [](GridPhase, void *data) {
        auto &g = *static_cast<Gates *>(data); if (g.entered.try_wait()) return;
        g.entered.count_down(); auto deadline = JobClock::now() + std::chrono::seconds(3);
        while (!g.release.try_wait() && JobClock::now() < deadline) std::this_thread::yield();
    };
    auto href = image()->getRepr()->attribute("href"); auto uri = inspectUri(href, {}).value; input->decodedBytes = uri.decodedBytes;
    JobInput job; job.storage.budget = budget; job.pixels = 64; job.work = calculate;
    ASSERT_TRUE(budget->acquire(Stage::input, uri.payloadLength + 8192, job.storage.reservation).ok());
    job.storage.bytes.assign(href + uri.payloadOffset, href + uri.payloadOffset + uri.payloadLength);
    job.storage.payload = input; std::weak_ptr<Input> weak = input; input.reset();
    bool delivered = false; BitmapJobs jobs([&](Ticket, JobResult) { delivered = true; }, {}, now, false);
    jobs.request(std::move(job)); ticks += 250; jobs.poll();
    auto deadline = JobClock::now() + std::chrono::seconds(2);
    while (!gates.entered.try_wait() && JobClock::now() < deadline) std::this_thread::yield();
    EXPECT_TRUE(gates.entered.try_wait()); jobs.close(); closePanel();
    EXPECT_FALSE(gates.release.try_wait()); EXPECT_FALSE(weak.expired()); EXPECT_GT(budget->reserved(), 0u);
    gates.release.count_down(); drainReaper(); ASSERT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
    EXPECT_TRUE(weak.expired()); EXPECT_EQ(budget->reserved(), 0u); EXPECT_FALSE(delivered);
}

TEST_F(ExplodeBitmapPanelTest, ExactCmykLinkedAndReferenceMessages) {
    open(); unsigned char cmyk[] = {255,216,255,192,0,20,8,0,8,0,8,4,1,17,0,2,17,0,3,17,0,4,17,0,255,218,0,8,1,1,0,0,63,0,0,255,217};
    auto encoded = g_base64_encode(cmyk, sizeof(cmyk)); auto href = std::string("data:image/jpeg;base64,") + encoded; g_free(encoded);
    image()->getRepr()->setAttribute("href", href); inspect();
    EXPECT_EQ(message(), "Convert this CMYK image to RGB and embed it before adjusting or exploding it.");
    image()->getRepr()->setAttribute("href", "file://" INKSCAPE_TESTS_DIR "/../COPYING"); inspect();
    EXPECT_EQ(message(), "Embed this linked image before adjusting or exploding it.");
    open(); auto clone = doc->getReprDoc()->createElement("svg:use"); clone->setAttribute("href", "#im");
    doc->getReprRoot()->appendChild(clone); GC::release(clone); doc->ensureUpToDate(); inspect();
    EXPECT_EQ(state(), State::Referenced);
    EXPECT_EQ(message(), "This image or an ancestor is used by clones or references. Unlink those references before adjusting or exploding it.");

}
TEST_F(ExplodeBitmapPanelTest, PublicationAllocationFaultsPreserveXmlHistoryAndAllowRetry) {
    for (auto stage : {PublishStage::Node, PublishStage::Insert, PublishStage::Selection, PublishStage::Readiness}) {
        open(); finish(); auto before = xml(); bool reached = false;
        struct Fault { PublishStage stage; bool *reached; } f{stage, &reached};
        hooks({[](PublishStage stage, unsigned, void *data) {
            auto &f = *static_cast<Fault *>(data); if (stage == f.stage) { *f.reached = true; throw std::bad_alloc(); }
        }, &f});
        explode(); EXPECT_TRUE(reached); EXPECT_EQ(state(), State::Failed); EXPECT_EQ(xml(), before);
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u); EXPECT_EQ(desktop->getSelection()->singleItem(), image());
        inspect(); finish(); explode(); EXPECT_EQ(state(), State::Idle) << message();
    }
}

TEST_F(ExplodeBitmapPanelTest, SharedHeadroomAdmitsJobAboveFormerPanelCeiling) {
    struct Room : MemoryProbe {
        bool read(RawMemory &m) const noexcept override {
            // Simulate macOS physical RAM minus a 2.5 GiB footprint.
            m = {16384*MiB, (16384-2560)*MiB, 2560*MiB}; return true;
        }
    } room;
    auto sample = sampleMemory(room); ASSERT_TRUE(sample.ok());
    Budget oldPanel(1536*MiB); ASSERT_TRUE(oldPanel.recheck(sample.value).ok());
    Budget::Token oldReservation;
    EXPECT_FALSE(oldPanel.acquire(Stage::topology, 2048*MiB, oldReservation).ok());
    open(2, 128, false, {}, {}, &room, 0, true, false);
    auto before = xml();
    worker(+[](JobInput const &job, Stop stop, JobWork &work, JobReporter &reporter) {
        // Reserve simulated large-job storage without allocating a 2 GiB buffer.
        Budget::Token large;
        auto admitted = job.storage.budget->acquire(Stage::topology, 2048*MiB, large);
        EXPECT_TRUE(admitted.ok()) << admitted.diagnostic;
        if (!admitted.ok()) return JobResult{admitted};
        return countedCalculate(job, stop, work, reporter);
    });
    clickPrimary(); EXPECT_EQ(state(), State::Counting) << message();
    EXPECT_EQ(memoryLimit(), (16384-2560-256)*MiB);
    finish(); ASSERT_EQ(state(), State::Ready) << message();
    EXPECT_TRUE(explodeEnabled()); EXPECT_EQ(output().count, 2u);
    EXPECT_EQ(xml(), before); EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
    closePanel(); // Probe outlives every callback.
}
TEST_F(ExplodeBitmapPanelTest, WorkerMemoryRefusalShowsLimitNeedAndAvailable) {
    open(2, 128, false, {}, {}, nullptr, 0, true, false);
    auto before = xml();
    worker(+[](JobInput const &, Stop, JobWork &, JobReporter &) {
        return JobResult{Bitmap::memoryFailure("OS headroom / operation budget", 2048*MiB, 1024*MiB)};
    });
    clickPrimary(); finish(); ASSERT_EQ(state(), State::Failed);
    EXPECT_EQ(message(), "Not enough memory: OS headroom / operation budget; estimated need 2048.00 MiB, available 1024.00 MiB.");
    EXPECT_FALSE(resultReady()); EXPECT_FALSE(preview()); EXPECT_EQ(xml(), before);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
}

TEST_F(ExplodeBitmapPanelTest, RetiredLedgerUsesFreshMemoryAdmissionOnReopen) {
    struct Room : MemoryProbe {
        std::uint64_t available = 1024*MiB;
        bool read(RawMemory &m) const noexcept override { m = {16384*MiB, available, 256*MiB}; return true; }
    } room;
    open(2, 128, false, {}, {}, &room); finish(); ASSERT_EQ(state(), State::Ready) << message(); auto before = xml();
    // Shared OS headroom minus the 256 MiB recovery reserve binds at 768 MiB.
    EXPECT_EQ(memoryLimit(), 768*MiB); panel->set_visible(false);
    drainReaper(); ASSERT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
    room.available = 8192*MiB; panel->set_visible(true); EXPECT_EQ(state(), State::Idle); clickPrimary();
    EXPECT_EQ(state(), State::Counting); EXPECT_EQ(memoryLimit(), (8192-256)*MiB); EXPECT_EQ(xml(), before);
    panel->set_visible(false); drainReaper();
    ASSERT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
    // Fresh admission must still refuse when A cannot cover the 256 MiB reserve.
    room.available = 256*MiB; panel->set_visible(true); EXPECT_EQ(state(), State::Idle); clickPrimary();
    EXPECT_EQ(state(), State::Failed); EXPECT_EQ(reserved(), 0u);
    EXPECT_EQ(message(), "Not enough memory: OS headroom / recovery reserve; estimated need 256.00 MiB, available 256.00 MiB.");
    EXPECT_FALSE(working()); EXPECT_FALSE(resultReady());
    EXPECT_FALSE(applyEnabled()); EXPECT_EQ(primary(), "Analyze");
    explode(); EXPECT_FALSE(publicationPending()); EXPECT_EQ(reserved(), 0u); EXPECT_EQ(xml(), before);
    closePanel(); // Probe lifetime covers every panel callback.
}

TEST_F(ExplodeBitmapPanelTest, InvalidNumericFieldCannotPublishPreviousExactResult) {
    for (unsigned i : {0u, 1u, 2u}) for (auto value : {"", "2.5", "-1", "99999999999999999999", i == 2 ? "26" : "999"}) {
        for (auto intent : {Intent::Explode, Intent::ApplyAdjustment}) {
            SCOPED_TRACE(i);
            SCOPED_TRACE(value);
            SCOPED_TRACE(static_cast<int>(intent));
            open(); finish(); auto history = seedHistory();
            ASSERT_EQ(state(), State::Ready); ASSERT_TRUE(applyEnabled()); ASSERT_TRUE(explodeEnabled());
            auto id = logicalImageIdentity(*image());
            pending(i, value); activate(intent);
            EXPECT_FALSE(publicationPending()); EXPECT_EQ(state(), State::Ready);
            EXPECT_NE(message().find("whole number"), std::string::npos);
            EXPECT_EQ(query(id).faintFloor, 5u);
            preservedHistory(history);
        }
    }
}
TEST_F(ExplodeBitmapPanelTest, SelectionChangeCannotRollBackAnotherImagesRecipe) {
    open(); finish(); auto copy = image()->getRepr()->duplicate(doc->getReprDoc());
    copy->setAttribute("id", "second"); doc->getReprRoot()->appendChild(copy); GC::release(copy);
    doc->ensureUpToDate(); auto second = cast<SPImage>(doc->getObjectById("second")); ASSERT_TRUE(second);
    auto id = logicalImageIdentity(*second); remember(id, Recipe{100, 20}, true);
    inspect(); finish(); drag(true); slider(0, 180);
    desktop->getSelection()->set(second); finish(); auto before = xml(); key(GDK_KEY_Escape);
    EXPECT_EQ(query(id).threshold, 100u); EXPECT_EQ(query(id).softness, 20u); EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, ExactUnsupportedEffectMessagesPreserveSourceAndHistory) {
    for (auto [attributes, reason] : {
        std::pair{"mask='url(#missing)'", "Mask dependency is unsupported."},
        std::pair{"clip-path='url(#missing)'", "Clip dependency is unresolved."},
        std::pair{"filter='url(#missing)'", "Only canonical managed tone filters are supported."},
        std::pair{"style='mix-blend-mode:multiply'", "Blend/isolation context is unsupported."},
        std::pair{"transform='scale(0)'", "Target transform is singular or non-finite."},
        std::pair{"marker-start='url(#missing)'", "Marker resource dependencies are unsupported."}}) {
        open(2, 128, false, attributes); auto before = xml();
        auto refused = resolve(*desktop, Intent::Explode);
        EXPECT_TRUE(std::any_of(refused.value.refusals.begin(), refused.value.refusals.end(),
            [reason](auto const &r) { return std::string_view(r.diagnostic) == reason; }));
        EXPECT_EQ(state(), State::Unsupported); EXPECT_EQ(message(), "This image has effects, clipping or geometry that Explode Bitmap cannot process. No changes were made.");
        apply(); explode(); EXPECT_EQ(xml(), before); EXPECT_FALSE(enabled(0));
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
    }
}
TEST_F(ExplodeBitmapPanelTest, DefaultPreviewExplodeFreshPieceAndOriginalUndoRedo) {
    open(); finish(); auto original = xml(); explode(); ASSERT_EQ(state(), State::Idle) << message(); auto pieces = xml();
    auto piece = cast<SPImage>(*desktop->getSelection()->items().begin()); ASSERT_TRUE(piece);
    desktop->getSelection()->set(piece); clickPrimary(); finish(); EXPECT_TRUE(query(logicalImageIdentity(*piece)).bypassAlpha); EXPECT_FALSE(preview());
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK); desktop->getSelection()->set(image()); clickPrimary(); finish();
    EXPECT_EQ(xml(), original); EXPECT_EQ(query(logicalImageIdentity(*image())).threshold, 128u);
    EXPECT_EQ(query(logicalImageIdentity(*image())).softness, 40u); EXPECT_TRUE(preview());
    key(GDK_KEY_z, Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK); EXPECT_EQ(xml(), pieces);
}
TEST_F(ExplodeBitmapPanelTest, WholeImageApplyCapDoesNotDiscardExactExplodePreparation) {
    open(); finish(); auto budget = std::make_shared<Budget>(1536*MiB); ASSERT_TRUE(budget->recheck(sampleMemory().value).ok());
    auto input = std::make_shared<Input>(); input->outlineReservation = reserveOutlines(budget).value; input->target = resolve(*desktop, Intent::Explode).value;
    input->recipe = Recipe{128, 40}; input->adjustmentPixelLimit = 1;
    auto href = image()->getRepr()->attribute("href"); auto uri = inspectUri(href, {}).value; input->decodedBytes = uri.decodedBytes;
    JobInput job; job.storage.budget = budget; job.pixels = 64; job.work = calculate;
    ASSERT_TRUE(budget->acquire(Stage::input, uri.payloadLength + 8192, job.storage.reservation).ok());
    job.storage.bytes.assign(href + uri.payloadOffset, href + uri.payloadOffset + uri.payloadLength); job.storage.payload = input;
    JobResult result; bool delivered = false;
    BitmapJobs jobs([&](Ticket, JobResult r) { result = std::move(r); delivered = true; }, {}, now, false);
    jobs.request(std::move(job)); ticks += 250; auto deadline = JobClock::now() + std::chrono::seconds(3);
    while (!delivered && JobClock::now() < deadline) { jobs.poll(); std::this_thread::yield(); }
    ASSERT_TRUE(delivered); ASSERT_TRUE(result.ok()) << result.outcome.diagnostic;
    auto const &out = static_cast<Output const &>(*result.value.payload);
    EXPECT_FALSE(out.adjustment); EXPECT_FALSE(out.adjustmentOutcome.ok()); EXPECT_TRUE(out.explodeOutcome.ok());
    ASSERT_TRUE(out.adjustmentFailure); EXPECT_EQ(out.adjustmentFailure->stage,CliBitmapStage::Encode);
    EXPECT_EQ(out.adjustmentFailure->reason,CliBitmapReason::EncodingFailed);
    EXPECT_FALSE(out.explodeFailure);
    EXPECT_EQ(out.grid.width, 16u); EXPECT_EQ(out.count, 2u); EXPECT_EQ(out.pieces.count(), 2u); EXPECT_TRUE(out.outlines.storage);
}

TEST_F(ExplodeBitmapPanelTest, ObservationFailureKeepsGenericMessage) {
    open(); finish(); auto before = xml();
    noObserver(); EXPECT_EQ(message(), "Explode Bitmap is unavailable for this image in this build. No changes were made.");
    EXPECT_EQ(xml(), before); EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
}
TEST_F(ExplodeBitmapPanelTest, WorkerProgressAdvancesAtMostTenHzAndEscapeRestoresEditing) {
    struct Updates final : JobPayload {
        std::atomic<unsigned> request{0};
        mutable std::atomic<unsigned> reported{0};
        mutable std::atomic<bool> stopped{false};
    };
    open(); finish(); auto before = xml();
    auto updates = std::make_shared<Updates>(); JobInput job; job.storage.payload = updates;
    job.work = +[](JobInput const &job, Stop stop, JobWork &, JobReporter &reporter) -> JobResult {
        auto const &u = static_cast<Updates const &>(*job.storage.payload);
        unsigned last = 0;
        while (!stop.requested()) {
            auto n = u.request.load();
            if (n != last) {
                last = n;
                reporter.progress({n < 4 ? Stage::topology : Stage::prepared, n == 1 ? 1u : n < 4 ? 2u : n - 3, 4});
                u.reported = n;
            }
            std::this_thread::yield();
        }
        u.stopped = true; return {{Status::canceled, "Canceled"}, {}, 0};
    };
    progressJob(std::move(job));
    EXPECT_EQ(message(), "Analyzing image… 0% · Esc cancels"); EXPECT_DOUBLE_EQ(fraction(), 0);
    ticks += 250; updates->request = 1;
    ASSERT_TRUE(pumpUntil([&] { return message() == "Analyzing image… 25% · Esc cancels"; }));
    EXPECT_DOUBLE_EQ(fraction(), .25);
    ticks += 99; updates->request = 2;
    ASSERT_TRUE(pumpUntil([&] { return updates->reported == 2; }));
    EXPECT_EQ(message(), "Analyzing image… 25% · Esc cancels"); EXPECT_DOUBLE_EQ(fraction(), .25);
    ticks += 1; updates->request = 3;
    ASSERT_TRUE(pumpUntil([&] { return message() == "Analyzing image… 50% · Esc cancels"; }));
    EXPECT_DOUBLE_EQ(fraction(), .5);
    ticks += 100; updates->request = 4;
    ASSERT_TRUE(pumpUntil([&] { return message() == "Preparing pieces… 25% · Esc cancels"; }));
    EXPECT_EQ(state(), State::Processing); EXPECT_DOUBLE_EQ(fraction(), .25);
    ticks += 100; updates->request = 5;
    ASSERT_TRUE(pumpUntil([&] { return message() == "Preparing pieces… 50% · Esc cancels"; }));
    EXPECT_DOUBLE_EQ(fraction(), .5); key(GDK_KEY_Escape);
    ASSERT_TRUE(pumpUntil([&] { return updates->stopped.load(); }));
    EXPECT_EQ(state(), State::Idle); EXPECT_FALSE(enabled(0)); EXPECT_FALSE(enabled(1)); EXPECT_FALSE(refineEnabled());
    EXPECT_EQ(xml(), before); EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
    clickPrimary(); field(0, "140"); finish(); EXPECT_EQ(state(), State::Ready) << message(); EXPECT_EQ(xml(), before);
    idle(); clickPrimary(); EXPECT_EQ(fraction(), 0); key(GDK_KEY_Escape); // Counting cancels too.
    expectNoWorkerStarts(); EXPECT_EQ(state(), State::Idle); EXPECT_FALSE(enabled(0)); EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, AnnouncementIsPaintedBeforeSinglePublicationOnDoubleClick) {
    for (auto intent : {Intent::Explode, Intent::ApplyAdjustment}) {
        open(); finish(); auto before = xml();
        std::string announcement = intent == Intent::Explode ? "Exploding 2 pieces…" : "Applying adjustment…";
        struct Observed { bool painted = false; unsigned publications = 0; } observed;
        host->painted = [&] {
            if (message() == announcement) { EXPECT_EQ(xml(), before); observed.painted = true; }
        };
        hooks({[](PublishStage stage, unsigned, void *data) {
            auto &o = *static_cast<Observed *>(data);
            if (stage == PublishStage::Admission) { EXPECT_TRUE(o.painted); ++o.publications; }
        }, &observed});
        click(intent); click(intent);
        EXPECT_TRUE(publicationPending()); EXPECT_EQ(message(), announcement); EXPECT_EQ(xml(), before);
        EXPECT_FALSE(enabled(0)); EXPECT_FALSE(applyEnabled()); EXPECT_FALSE(explodeEnabled());
        publicationFrame(); host->painted = {};
        EXPECT_TRUE(observed.painted); EXPECT_EQ(observed.publications, 1u); EXPECT_NE(xml(), before);
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
        EXPECT_EQ(desktop->getSelection()->size(), intent == Intent::Explode ? 2u : 1u);
        DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(), before);
    }
}
TEST_F(ExplodeBitmapPanelTest, PendingPublicationRevalidatesAndCancelsWithoutMutation) {
    for (auto intent : {Intent::Explode, Intent::ApplyAdjustment}) {
        for (unsigned change = 0; change < 4; ++change) {
            open(); finish(); unsigned publications = 0;
            hooks({[](PublishStage stage, unsigned, void *data) {
                if (stage == PublishStage::Admission) ++*static_cast<unsigned *>(data);
            }, &publications});
            auto dispatched = ticket(); panel->activate(intent); ASSERT_TRUE(publicationPending());
            if (change == 0) image()->getRepr()->setAttribute("x", "9");
            else if (change == 1) { image()->getRepr()->setAttribute("x", "9"); image()->getRepr()->setAttribute("x", nullptr); }
            else if (change == 2) key(GDK_KEY_Escape);
            else panel->set_visible(false);
            auto before = xml(); publicationFrame(); expectNoWorkerStarts();
            EXPECT_EQ(publications, 0u); EXPECT_EQ(ticket(), dispatched); EXPECT_EQ(xml(), before);
            EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
            EXPECT_EQ(desktop->getSelection()->singleItem(), image());
            if (change < 2) { EXPECT_EQ(message(), "Image changed. Click Analyze to update the preview and piece count."); finish(); EXPECT_EQ(state(), State::Stale) << message(); }
            else if (change == 2) { EXPECT_EQ(state(), State::Idle); EXPECT_FALSE(enabled(0)); }
        }
    }
}
TEST_F(ExplodeBitmapPanelTest, QueuedPublicationCancellationPreservesRealUndoAndRedo) {
    for (auto intent : {Intent::Explode, Intent::ApplyAdjustment}) for (unsigned cause = 0; cause < 4; ++cause) {
        open(); finish(); auto h = seedHistory(); unsigned publications = 0;
        hooks({[](PublishStage stage, unsigned, void *data) {
            if (stage == PublishStage::Admission) ++*static_cast<unsigned *>(data);
        }, &publications});
        click(intent); ASSERT_TRUE(publicationPending()); queuedPublicationIdle();
        if (cause == 0) key(GDK_KEY_Escape);
        else if (cause == 1) closePanel();
        else if (cause == 2) desktop->getSelection()->set(doc->getObjectById("v"));
        else {
            Application::instance().remove_desktop(desktop.get()); desktop.reset();
        }
        drainEvents(); if (panel) expectNoWorkerStarts(); EXPECT_EQ(publications, 0u); preservedHistory(h);
    }
}
TEST_F(ExplodeBitmapPanelTest, OversizedSourceOffersExactDpiBeforeExplodeReservation) {
    largeSource(); auto h = seedHistory(); auto before = xml();
    ASSERT_EQ(state(), State::Resize) << message();
    EXPECT_EQ(message(), "This image is 5001 × 20 px. Explode Bitmap supports images up to 5000 × 5000 px. Resize to continue.");
    expectNoWorkerStarts(); EXPECT_FALSE(working()); EXPECT_FALSE(resultReady()); EXPECT_EQ(ticket(), 0u); EXPECT_EQ(reserved(), 0u);
    EXPECT_TRUE(explodeEnabled()); EXPECT_EQ(primary(), "Resize"); EXPECT_FALSE(applyEnabled());
    auto bounds = desktop->getSelection()->documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    EXPECT_EQ(resizeDpi(), maxResizeDpi(*bounds)); EXPECT_DOUBLE_EQ(resizeMaximum(), maxResizeDpi(*bounds));
    EXPECT_EQ(resizeDpi(), 96); resizeDpi(600); EXPECT_EQ(resizeDpi(), 96);
    explode(); EXPECT_EQ(xml(), before); EXPECT_EQ(reserved(), 0u); preservedHistory(h);
}
TEST_F(ExplodeBitmapPanelTest, HeightAloneAndNoFittingDpiNeverDispatch) {
    largeSource(20, 5001); EXPECT_EQ(state(), State::Resize);
    EXPECT_EQ(message(), "This image is 20 × 5001 px. Explode Bitmap supports images up to 5000 × 5000 px. Resize to continue.");
    expectNoWorkerStarts(); EXPECT_EQ(ticket(), 0u); EXPECT_EQ(reserved(), 0u);
    image()->getRepr()->setAttribute("width", "6000"); doc->ensureUpToDate(); inspect();
    auto bounds = desktop->getSelection()->documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    // 10ef60ce8: maxResizeDpi requires ceil(axis * dpi / 96) <= 5000
    // on both axes. At width 6000, 80 gives 5000 px; 81 gives 5063 px.
    EXPECT_DOUBLE_EQ(bounds->width(), 6000);
    EXPECT_EQ(std::ceil(bounds->width()*80/96), MaxExplodeSourceAxis);
    EXPECT_LE(std::ceil(bounds->height()*80/96), MaxExplodeSourceAxis);
    EXPECT_GT(std::ceil(bounds->width()*81/96), MaxExplodeSourceAxis);
    EXPECT_EQ(maxResizeDpi(*bounds), 80);
    EXPECT_EQ(resizeDpi(), 80); EXPECT_EQ(resizeMaximum(), 80);
    image()->getRepr()->setAttribute("width", "1000000"); doc->ensureUpToDate(); inspect();
    EXPECT_EQ(state(), State::Failed); EXPECT_EQ(ticket(), 0u); EXPECT_EQ(reserved(), 0u);
    auto before = xml(); clickResize(); drainEvents(); expectNoWorkerStarts(); EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, ResizeUsesCallerDpiExactExtentTransparentReplacementAndOneUndo) {
    for (bool antialias : {false, true}) for (int dpi : {72, 96}) {
        largeSource(); auto before = xml(); auto original = image(); auto parent = original->parent;
        auto slot = original->getRepr()->position(); auto bounds = original->documentVisualBounds(); ASSERT_TRUE(bounds);
        resizeDpi(dpi); resizeAntialias(antialias); bool painted = false;
        host->painted = [&] { if (message() == "Resizing image…") { EXPECT_EQ(xml(), before); painted = true; } };
        clickResize(); clickResize(); EXPECT_EQ(message(), "Resizing image…"); EXPECT_EQ(xml(), before);
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
        publicationFrame(); host->painted = {}; EXPECT_TRUE(painted);
        ASSERT_FALSE(image()); auto replacement = cast<SPImage>(desktop->getSelection()->singleItem()); ASSERT_TRUE(replacement) << message();
        ASSERT_TRUE(replacement->pixbuf); EXPECT_EQ(replacement->parent, parent); EXPECT_EQ(replacement->getRepr()->position(), slot);
        auto afterBounds = replacement->documentVisualBounds(); ASSERT_TRUE(afterBounds);
        EXPECT_NEAR(afterBounds->left(), bounds->left(), 1e-12); EXPECT_NEAR(afterBounds->top(), bounds->top(), 1e-12);
        EXPECT_NEAR(afterBounds->width(), bounds->width(), 1e-12); EXPECT_NEAR(afterBounds->height(), bounds->height(), 1e-12);
        EXPECT_EQ(replacement->pixbuf->width(), int(std::ceil(bounds->width()*dpi/96)));
        EXPECT_EQ(replacement->pixbuf->height(), int(std::ceil(bounds->height()*dpi/96)));
        EXPECT_LE(replacement->pixbuf->width(), int(MaxExplodeSourceAxis)); EXPECT_LE(replacement->pixbuf->height(), int(MaxExplodeSourceAxis));
        Pixbuf p(*replacement->pixbuf); p.ensurePixelFormat(Pixbuf::PF_GDK); bool transparent = false, visible = false;
        for (int y = 0; y < p.height(); ++y) for (int x = 0; x < p.width(); ++x) {
            auto px = p.pixels() + y*p.rowstride() + 4*x;
            transparent |= px[3] == 0; visible |= px[3] > 0;
            if (px[3]) EXPECT_FALSE(px[0] == 255 && px[1] == 255 && px[2] == 255);
        }
        EXPECT_TRUE(transparent); EXPECT_TRUE(visible);
        auto resized = xml(); auto dispatched = ticket(); finish(); EXPECT_EQ(state(), State::Idle); EXPECT_EQ(ticket(), dispatched);
        EXPECT_EQ(message(), "Image resized. Click Analyze to inspect it."); clickPrimary(); finish(); EXPECT_EQ(state(), State::Ready) << message(); EXPECT_EQ(output().count, 2u);
        Glib::ustring undoLabel = (*doc->get_event_log()->getCurrEvent())[EventLog::getColumns().description];
        EXPECT_EQ(undoLabel.raw(), "Resize image for Explode Bitmap");
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
        DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(), before); ASSERT_TRUE(image());
        EXPECT_EQ(image()->pixbuf->width(), 5001);
        DocumentUndo::redo(doc.get()); EXPECT_EQ(xml(), resized);
    }
}
TEST_F(ExplodeBitmapPanelTest, ResizePreservesRenderedCornerMarksAndCentredSquare) {
    // Five source columns and ten rows per document pixel keep the source axes
    // inside the decoder limit; preserveAspectRatio=none makes the square 6 x 6.
    // Inspect rendered colour regions in the published PNG, mapped back through
    // its placement. Element bounds alone cannot detect a padded/squeezed raster.
    struct Mark { int x, y, w, h; std::array<unsigned char, 3> rgb; };
    std::array<Mark, 5> const marks{{
        {0, 0, 15, 20, {255, 0, 0}}, {14981, 0, 15, 20, {0, 255, 0}},
        {0, 82, 15, 20, {0, 0, 255}}, {14981, 82, 15, 20, {255, 0, 255}},
        {7483, 21, 30, 60, {0, 255, 255}}
    }};
    for (bool transpose : {false, true}) for (bool antialias : {false, true}) {
        SCOPED_TRACE(testing::Message() << "transpose=" << transpose << " antialias=" << antialias);
        auto raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, transpose ? 102 : 14996, transpose ? 14996 : 102);
        ASSERT_TRUE(raw); gdk_pixbuf_fill(raw, 0);
        for (auto const &mark : marks) for (int y = mark.y; y < mark.y + mark.h; ++y) for (int x = mark.x; x < mark.x + mark.w; ++x) {
            auto p = gdk_pixbuf_get_pixels(raw) + (transpose ? x : y)*gdk_pixbuf_get_rowstride(raw) + 4*(transpose ? y : x);
            std::copy(mark.rgb.begin(), mark.rgb.end(), p); p[3] = 255;
        }
        Pixbuf source(raw); auto uri = sp_image_encode_png_data_uri(source); ASSERT_TRUE(uri);
        open(2, 255, false, "preserveAspectRatio='none'", *uri);
        // Keep the full pattern within the existing 256 px overlay envelope.
        desktop->zoom_absolute({0, 0}, 0.05);
        auto repr = image()->getRepr(); repr->setAttribute("x", "0.4"); repr->setAttribute("y", "0.4");
        repr->setAttribute("width", transpose ? "10.2" : "2999.2");
        repr->setAttribute("height", transpose ? "2999.2" : "10.2");
        doc->ensureUpToDate();
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Pattern fixture"), "");
        DocumentUndo::clearUndo(doc.get()); inspect();
        ASSERT_EQ(state(), State::Resize) << message();
        resizeDpi(96); resizeAntialias(antialias); clickResize(); publicationFrame();
        ASSERT_FALSE(image()); auto replacement = cast<SPImage>(desktop->getSelection()->singleItem());
        ASSERT_TRUE(replacement) << message(); ASSERT_TRUE(replacement->pixbuf);
        Pixbuf rendered(*replacement->pixbuf); rendered.ensurePixelFormat(Pixbuf::PF_GDK);
        ASSERT_EQ(rendered.width(), transpose ? 11 : 3000); ASSERT_EQ(rendered.height(), transpose ? 3000 : 11);
        auto placement = replacement->documentVisualBounds(); ASSERT_TRUE(placement);
        for (auto const &mark : marks) {
            SCOPED_TRACE(testing::Message() << "mark=" << mark.x << ',' << mark.y);
            int left = rendered.width(), top = rendered.height(), right = 0, bottom = 0;
            for (int y = 0; y < rendered.height(); ++y) for (int x = 0; x < rendered.width(); ++x) {
                auto p = rendered.pixels() + y*rendered.rowstride() + 4*x;
                if (p[3] < 128 || !std::equal(mark.rgb.begin(), mark.rgb.end(), p)) continue;
                left = std::min(left, x); top = std::min(top, y);
                right = std::max(right, x + 1); bottom = std::max(bottom, y + 1);
            }
            ASSERT_LT(left, right); ASSERT_LT(top, bottom);
            double x0 = placement->left() + left*placement->width()/rendered.width();
            double x1 = placement->left() + right*placement->width()/rendered.width();
            double y0 = placement->top() + top*placement->height()/rendered.height();
            double y1 = placement->top() + bottom*placement->height()/rendered.height();
            // Half a document pixel covers raster edge quantization at 96 DPI.
            EXPECT_NEAR(x0, 0.4 + (transpose ? mark.y/10.0 : mark.x/5.0), 0.5);
            EXPECT_NEAR(x1, 0.4 + (transpose ? (mark.y + mark.h)/10.0 : (mark.x + mark.w)/5.0), 0.5);
            EXPECT_NEAR(y0, 0.4 + (transpose ? mark.x/5.0 : mark.y/10.0), 0.5);
            EXPECT_NEAR(y1, 0.4 + (transpose ? (mark.x + mark.w)/5.0 : (mark.y + mark.h)/10.0), 0.5);
            if (&mark == &marks.back()) {
                EXPECT_NEAR(x1 - x0, 6.0, 0.5); EXPECT_NEAR(y1 - y0, 6.0, 0.5);
                EXPECT_NEAR(x1 - x0, y1 - y0, 0.5);
            }
        }
        EXPECT_EQ(state(), State::Idle); clickPrimary(); finish(); ASSERT_EQ(state(), State::Ready) << message(); EXPECT_EQ(output().count, 5u);
    }
}
TEST_F(ExplodeBitmapPanelTest, ResizeOfferUsesExactExtentAt95And96DpiBoundary) {
    for (bool transpose : {false, true}) {
        largeSource();
        for (auto extent : {"4999.9", "5000", "5000.001"}) {
            auto repr = image()->getRepr(); repr->setAttribute("x", "0.2"); repr->setAttribute("y", "0.2");
            repr->setAttribute("width", transpose ? "10.2" : extent);
            repr->setAttribute("height", transpose ? extent : "10.2");
            doc->ensureUpToDate(); auto before = xml(); inspect();
            ASSERT_EQ(state(), State::Resize) << message();
            int expected = std::strcmp(extent, "5000.001") == 0 ? 95 : 96;
            EXPECT_EQ(resizeDpi(), expected); EXPECT_EQ(resizeMaximum(), expected);
            expectNoWorkerStarts(); EXPECT_EQ(xml(), before); EXPECT_EQ(ticket(), 0u); EXPECT_EQ(reserved(), 0u);
        }
    }
}
TEST_F(ExplodeBitmapPanelTest, ResizeRefusalAndQueuedCancellationPreserveHistory) {
    for (unsigned cause = 0; cause < 6; ++cause) {
        largeSource(); auto h = seedHistory();
        if (cause == 4) refuseResize();
        if (cause == 5) DocumentUndo::setAtomicSettlementFaultForTesting(+[](DocumentUndo::AtomicSettlementStage stage) {
            return stage == DocumentUndo::AtomicSettlementStage::EventConstruction;
        });
        clickResize(); queuedPublicationIdle();
        if (cause == 0) key(GDK_KEY_Escape);
        else if (cause == 1) closePanel();
        else if (cause == 2) desktop->getSelection()->set(doc->getObjectById("v"));
        else if (cause == 3) { Application::instance().remove_desktop(desktop.get()); desktop.reset(); }
        drainEvents(); if (panel) expectNoWorkerStarts(); DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
        if (cause == 4) EXPECT_EQ(message(), "Explode Bitmap is unavailable for this image in this build. No changes were made.");
        if (cause == 5) EXPECT_EQ(desktop->getSelection()->singleItem(), image());
        preservedHistory(h);
    }
}
TEST_F(ExplodeBitmapPanelTest, OversizedVectorRejectsBeforeRasterReservationOrWorker) {
    open(); key(GDK_KEY_Escape); drainReaper(); ASSERT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
    auto v = doc->getObjectById("v"); v->getRepr()->setAttribute("width", "2000");
    doc->ensureUpToDate(); DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture size"), ""); DocumentUndo::clearUndo(doc.get());
    auto h = seedHistory(); auto beforeTicket = ticket(); desktop->getSelection()->set(v);
    EXPECT_EQ(state(), State::Idle);
    EXPECT_EQ(message(), "Explode Bitmap works on embedded images. For vectors, use Path > Break Apart.");
    expectNoWorkerStarts(); EXPECT_FALSE(working()); EXPECT_EQ(ticket(), beforeTicket); EXPECT_EQ(reserved(), 0u); preservedHistory(h);
}
TEST_F(ExplodeBitmapPanelTest, OptInBitmapCopyIsSingleTargetAndRollbackPreservesHistory) {
    largeSource(); auto h = seedHistory(); Budget budget(1536*MiB);
    BitmapCopyOptions options; options.dpi = 72; options.transparent = false; options.keep_original = true;
    auto candidate = prepareBitmapCopy(*desktop->getSelection(), options, PlacementPolicy::SingleImageResize, budget);
    ASSERT_TRUE(candidate.ok()) << candidate.outcome.diagnostic;
    EXPECT_EQ(candidate.candidate.metadata().requestedDpi, 72); EXPECT_EQ(candidate.candidate.metadata().renderDpi, 72);
    EXPECT_FALSE(publishBitmapCopy(*desktop->getSelection(), candidate.candidate).ok()); EXPECT_EQ(xml(), h.first);
    candidate = {};
    candidate = prepareBitmapCopy(*desktop->getSelection(), options, PlacementPolicy::SingleImageResize, budget,
        {nullptr, nullptr, nullptr, +[] { throw std::bad_alloc(); }});
    ASSERT_TRUE(candidate.ok()) << candidate.outcome.diagnostic;
    auto token = DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
    auto result = publishBitmapCopy(*desktop->getSelection(), candidate.candidate, &*token);
    EXPECT_FALSE(result.ok()); token->rollback(); EXPECT_EQ(desktop->getSelection()->singleItem(), image());
    preservedHistory(h);
    desktop->getSelection()->setList<SPItem>({image(), cast<SPItem>(doc->getObjectById("v"))});
    auto before = xml();
    auto rejected = prepareBitmapCopy(*desktop->getSelection(), options, PlacementPolicy::SingleImageResize, budget);
    EXPECT_FALSE(rejected.ok()); EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, OptInResizeRetainsTransformedParentSlotAndSaveReopenExtent) {
    closePanel(); open(); key(GDK_KEY_Escape);
    auto group = doc->getReprDoc()->createElement("svg:g"); group->setAttribute("id", "parent");
    group->setAttribute("transform", "matrix(1.25,0.2,-0.1,0.8,12.3,7.1)");
    auto root = image()->getRepr()->parent(); root->addChild(group, nullptr); GC::release(group);
    auto repr = image()->getRepr(); Inkscape::GC::anchor(repr); root->removeChild(repr); group->addChild(repr, nullptr); GC::release(repr);
    repr->setAttribute("x", "0.123456789123"); repr->setAttribute("y", "0.987654321987");
    doc->ensureUpToDate(); desktop->getSelection()->set(image());
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Nested fixture"), ""); DocumentUndo::clearUndo(doc.get());
    auto before = xml(); auto bounds = image()->documentVisualBounds(); ASSERT_TRUE(bounds); auto slot = repr->position();
    Budget budget(1536*MiB); BitmapCopyOptions options; options.dpi = 96;
    auto candidate = prepareBitmapCopy(*desktop->getSelection(), options, PlacementPolicy::SingleImageResize, budget);
    ASSERT_TRUE(candidate.ok()) << candidate.outcome.diagnostic;
    auto token = DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
    auto result = publishBitmapCopy(*desktop->getSelection(), candidate.candidate, &*token); ASSERT_TRUE(result.ok()) << result.outcome.diagnostic;
    EXPECT_TRUE(token->commitAtomically(Util::Internal::ContextString("Resize fixture"), "", [] { return true; }));
    auto resized = cast<SPImage>(desktop->getSelection()->singleItem()); ASSERT_TRUE(resized);
    EXPECT_EQ(resized->parent, doc->getObjectById("parent")); EXPECT_EQ(resized->getRepr()->position(), slot);
    auto reopened = SPDocument::createNewDocFromMem(xml()); ASSERT_TRUE(reopened); reopened->ensureUpToDate();
    auto saved = cast<SPImage>(reopened->getObjectById(result.imageId.c_str())); ASSERT_TRUE(saved);
    auto extent = saved->documentVisualBounds(); ASSERT_TRUE(extent);
    EXPECT_NEAR(extent->left(), bounds->left(), 1e-11); EXPECT_NEAR(extent->top(), bounds->top(), 1e-11);
    EXPECT_NEAR(extent->width(), bounds->width(), 1e-11); EXPECT_NEAR(extent->height(), bounds->height(), 1e-11);
    EXPECT_EQ(saved->pixbuf->width(), resized->pixbuf->width()); EXPECT_EQ(saved->pixbuf->height(), resized->pixbuf->height());
    DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, ExactSourceDomainGoesStraightToCounting) {
    struct Room : MemoryProbe {
        bool read(RawMemory &m) const noexcept override { m = {16384*MiB, 8192*MiB, 256*MiB}; return true; }
    } room;
    largeSource(5000, 5000, &room); EXPECT_EQ(state(), State::Counting) << message();
    EXPECT_NE(ticket(), 0u); EXPECT_GT(reserved(), 0u); key(GDK_KEY_Escape); drainEvents();
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u); closePanel();
}
TEST_F(ExplodeBitmapPanelTest, FiftyPiecesAt3000SquareFitEightGiBWithBoundedMemory) {
    struct NormalEightGiB : MemoryProbe {
        bool read(RawMemory &m) const noexcept override { m = {8192*MiB, 768*MiB, 512*MiB}; return true; }
    } normal;
    auto raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 3000, 3000); ASSERT_TRUE(raw); gdk_pixbuf_fill(raw, 0);
    for (unsigned i = 0; i < 50; ++i) for (unsigned y = 0; y < 300; ++y) for (unsigned x = 0; x < 200; ++x) {
        auto p = gdk_pixbuf_get_pixels(raw) + (100 + (i/10)*600 + y)*gdk_pixbuf_get_rowstride(raw) + 4*(50 + (i%10)*300 + x);
        p[0] = 64; p[1] = 128; p[2] = 192; p[3] = 254;
    }
    Pixbuf pix(raw); auto uri = sp_image_encode_png_data_uri(pix); ASSERT_TRUE(uri);
    open(2, 255, false, "preserveAspectRatio='none'", *uri, &normal);
    auto repr = image()->getRepr(); repr->setAttribute("width", "3000"); repr->setAttribute("height", "3000");
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Large fixture"), "");
    DocumentUndo::clearUndo(doc.get()); inspect(); auto before = xml(); finish();
    ASSERT_EQ(state(), State::Ready) << message(); EXPECT_EQ(output().count, 50u);
    // f4587a8f9: min(1536, 8192/4, 3072 - 512 - 256, 768 - 256)
    // is 512 MiB; available RAM is no longer divided by four.
    EXPECT_EQ(memoryLimit(), 512*MiB); EXPECT_LT(output().topologyPeak, 4*MiB);
    EXPECT_TRUE(output().outlines.storage); EXPECT_LT(output().budget->reserved(Stage::prepared), 4*MiB);
    EXPECT_TRUE(outlines()); EXPECT_TRUE(explodeEnabled()); EXPECT_EQ(xml(), before);
    EXPECT_EQ(message().find("Piece outlines are unavailable. The piece count is exact."), std::string::npos);
    explode(); ASSERT_EQ(state(), State::Idle) << message(); EXPECT_EQ(desktop->getSelection()->size(), 50u);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
    DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, P3OldDrawingRefusalsKeepStorageAndPublication) {
    for (bool extent : {false,true}) {
        open(); finish(); ASSERT_EQ(state(),State::Ready); ASSERT_TRUE(outlines());
        drainReaper(); ASSERT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
        auto before=xml(); auto ledger=output().budget; auto old=ticket();
        auto reservedBefore=ledger->reserved(Stage::prepared);
        auto storage=output().outlines.storage;
        if (extent) refuseExtentDrawing(); else refuseCandidateDrawing();
        expectNoWorkerStarts(); EXPECT_EQ(ticket(),old);
        EXPECT_TRUE(outlines()); EXPECT_EQ(output().outlines.storage,storage);
        EXPECT_EQ(ledger->reserved(Stage::prepared),reservedBefore);
        EXPECT_TRUE(explodeEnabled()); EXPECT_EQ(xml(),before);
        explode(); ASSERT_EQ(state(),State::Idle) << message();
        DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(),before);
    }
}
TEST_F(ExplodeBitmapPanelTest, P3WindowCameraBudgetZoomAndPanRecoverWithoutAnalysis) {
    auto raw=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,2000,1000); ASSERT_TRUE(raw); gdk_pixbuf_fill(raw,0);
    for(unsigned y=0;y<1000;++y) for(unsigned x=5;x<1000;x+=10)
        gdk_pixbuf_get_pixels(raw)[y*gdk_pixbuf_get_rowstride(raw)+x*4+3]=255;
    gdk_pixbuf_get_pixels(raw)[500*gdk_pixbuf_get_rowstride(raw)+1900*4+3]=255;
    Pixbuf pixels(raw); auto uri=sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(uri);
    open(2,255,false,"preserveAspectRatio='none'",*uri);
    image()->getRepr()->setAttribute("width","2000"); image()->getRepr()->setAttribute("height","1000");
    doc->ensureUpToDate(); inspect(); finish(); ASSERT_EQ(state(),State::Ready) << message();
    auto storage=output().outlines.storage; ASSERT_TRUE(storage); ASSERT_GT(storage->count,150000u);
    auto jobs=workerStarts.load(); auto old=ticket(); auto before=xml();
    Gtk::Window view; view.set_default_size(800,600); view.set_child(*desktop->getCanvas()); view.present();
    auto unparent=scope_exit([&]{view.unset_child();});
    // Realization schedules the desktop's initial fit; let that finish before
    // setting the camera whose automatic recovery this test exercises.
    auto settled=JobClock::now()+std::chrono::milliseconds(150);
    while(JobClock::now()<settled) { Glib::MainContext::get_default()->iteration(false); std::this_thread::yield(); }
    ASSERT_GT(desktop->getCanvas()->get_width(),0);
    auto waitState=[&](OutlineVisibility expected) {
        auto end=JobClock::now()+std::chrono::seconds(3);
        do { Glib::MainContext::get_default()->iteration(false); std::this_thread::yield(); }
        while(outlineCameraState().visibility!=expected && JobClock::now()<end);
        EXPECT_EQ(outlineCameraState().visibility,expected) << message() << " visible=" << outlineCameraState().visibleSegments;
    };
    desktop->zoom_absolute({500,500},.6); desktop->getCanvas()->set_pos(Geom::IntPoint(0,0));
    waitState(OutlineVisibility::tooDense);
    EXPECT_NE(message().find("Zoom in to see piece outlines."),std::string::npos);
    EXPECT_TRUE(outlines()); EXPECT_TRUE(explodeEnabled());
    auto item=outlineItem(); auto surface=Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,64,64);
    auto cr=Cairo::Context::create(surface); CanvasItemBuffer b{Geom::IntRect(0,0,64,64),1,cr,false};
    item->render(b); surface->flush();
    EXPECT_TRUE(std::all_of(surface->get_data(),surface->get_data()+surface->get_stride()*64,[](auto c){return c==0;}));
    desktop->zoom_absolute({50,50},4); waitState(OutlineVisibility::exact);
    EXPECT_EQ(message().find("Zoom in to see piece outlines."),std::string::npos);
    desktop->zoom_absolute({500,500},.6); desktop->getCanvas()->set_pos(Geom::IntPoint(0,0)); waitState(OutlineVisibility::tooDense);
    // Same affine, only scroll position changes. No explicit item update.
    desktop->getCanvas()->set_pos(Geom::IntPoint(600,0)); waitState(OutlineVisibility::exact);
    EXPECT_GT(outlineCameraState().visibleSegments,0u); EXPECT_LT(outlineCameraState().visibleSegments,150000u);
    desktop->getCanvas()->set_pos(Geom::IntPoint(0,0)); waitState(OutlineVisibility::tooDense);
    EXPECT_EQ(workerStarts,jobs); EXPECT_EQ(ticket(),old); EXPECT_EQ(output().outlines.storage,storage);
    EXPECT_EQ(xml(),before); EXPECT_TRUE(explodeEnabled());
    // Both operations retain the same dense camera. Each new installation
    // must publish its visibility even when it matches the retired item.
    field(0,"129"); finish(); ASSERT_EQ(state(),State::Ready) << message();
    ASSERT_TRUE(pumpUntil([&] { return message().find("Zoom in to see piece outlines.")!=std::string::npos; }));
    EXPECT_EQ(outlineCameraState().visibility,OutlineVisibility::tooDense);
    contours(true); finish(); ASSERT_EQ(state(),State::Ready) << message();
    ASSERT_TRUE(contourPreview()) << message(); EXPECT_FALSE(outlines());
    EXPECT_EQ(message().find("Zoom in to see piece outlines."),std::string::npos);
    contours(false);
    ASSERT_TRUE(pumpUntil([&] { return message().find("Zoom in to see piece outlines.")!=std::string::npos; }));
    EXPECT_EQ(outlineCameraState().visibility,OutlineVisibility::tooDense);
    EXPECT_EQ(xml(),before); EXPECT_TRUE(explodeEnabled());
}
TEST_F(ExplodeBitmapPanelTest, P3R2RequiredTopologyWinsRealOutlineReservationCompetition) {
    auto raw=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,5000,5000); ASSERT_TRUE(raw); gdk_pixbuf_fill(raw,0);
    for(unsigned y=0;y<5000;++y) for(unsigned i=0;i<150;++i)
        gdk_pixbuf_get_pixels(raw)[y*gdk_pixbuf_get_rowstride(raw)+4*(16+33*i)+3]=255;
    Pixbuf pixels(raw); auto uri=sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(uri);
    open(2,255,false,"preserveAspectRatio='none'",*uri,nullptr,0,true,false);
    image()->getRepr()->setAttribute("width","5000"); image()->getRepr()->setAttribute("height","5000");
    doc->ensureUpToDate(); DocumentUndo::done(doc.get(),Util::Internal::ContextString("R2 strips fixture"),"");
    DocumentUndo::clearUndo(doc.get()); Budget::Token retained; competeWithRequiredTopology(retained);
    auto before=xml(); clickPrimary(); finish();
    ASSERT_EQ(state(),State::Ready) << message(); EXPECT_EQ(output().count,150u);
    EXPECT_TRUE(output().explodeOutcome.ok()); EXPECT_EQ(output().pieces.count(),150u);
    ASSERT_TRUE(output().display); EXPECT_EQ(output().display->reservation.bytes(),100000000u);
    EXPECT_FALSE(output().outlineOutcome.ok()); EXPECT_FALSE(output().outlines.storage);
    EXPECT_TRUE(explodeEnabled()); EXPECT_EQ(xml(),before);
    EXPECT_NE(message().find("Piece outlines are unavailable. The piece count is exact."),std::string::npos);
    explode(); ASSERT_EQ(state(),State::Idle) << message(); EXPECT_EQ(desktop->getSelection()->size(),150u);
    DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(),before);
}
TEST_F(ExplodeBitmapPanelTest, P3R2CancelAndHideRemoveOutlineCameraCallback) {
    for(bool hide:{false,true}) {
        open(); finish(); ASSERT_TRUE(outlines()); ASSERT_TRUE(outlineCallbackRegistered());
        if(hide) panel->set_visible(false); else key(GDK_KEY_Escape);
        EXPECT_FALSE(outlines()); EXPECT_FALSE(outlineCallbackRegistered());
    }
}
TEST_F(ExplodeBitmapPanelTest, P3OptionalOutlineReservationRefusalKeepsCountAndExplode) {
    auto metadata=sizeof(OutlineReservation)+sizeof(OutlineStorage)+sizeof(Budget)+256;
    // Missing reservation and an admitted one-byte index ledger both fail only
    // the optional outline branch, before and during worker preparation.
    for(std::uint64_t bytes:{std::uint64_t(0),std::uint64_t(metadata+1)}) {
        open(2,128,false,{}, {},nullptr,0,true,false); refuseOutlineReservation(bytes); clickPrimary(); finish();
        ASSERT_EQ(state(),State::Ready) << message();
        EXPECT_FALSE(output().outlines.storage); EXPECT_EQ(output().count,2u);
        EXPECT_TRUE(explodeEnabled());
        EXPECT_NE(message().find("Piece outlines are unavailable. The piece count is exact."),std::string::npos);
        auto before=xml(); explode(); EXPECT_EQ(state(),State::Idle);
        DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(),before);
    }
}
TEST_F(ExplodeBitmapPanelTest, P3OldRefusalRetainsFrozenReaderAndOutput) {
    open(); finish(); ASSERT_EQ(state(),State::Ready); ASSERT_TRUE(outlines());
    drainReaper(); ASSERT_TRUE(waitForBitmapReaper(std::chrono::seconds(3)));
    omitWithFrozenReader(); EXPECT_TRUE(explodeEnabled());
    explode(); EXPECT_EQ(state(),State::Idle) << message();
}
TEST_F(ExplodeBitmapPanelTest, OneHundredFiftyFullHeightStripsAt96DpiPublishWithDefaultRefinement) {
    auto raw=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,5000,5000); ASSERT_TRUE(raw); gdk_pixbuf_fill(raw,0);
    for (unsigned y=0;y<5000;++y) for (unsigned i=0;i<150;++i) {
        auto p=gdk_pixbuf_get_pixels(raw)+y*gdk_pixbuf_get_rowstride(raw)+4*(16+33*i);
        p[0]=64; p[1]=128; p[2]=192; p[3]=255;
    }
    Pixbuf pix(raw); auto uri=sp_image_encode_png_data_uri(pix); ASSERT_TRUE(uri);
    open(2,255,false,"preserveAspectRatio='none'",*uri);
    image()->getRepr()->setAttribute("width","5000"); image()->getRepr()->setAttribute("height","5000");
    doc->ensureUpToDate(); DocumentUndo::done(doc.get(),Util::Internal::ContextString("Strips fixture"),"");
    DocumentUndo::clearUndo(doc.get()); inspect(); auto before=xml(); finish();
    ASSERT_EQ(state(),State::Ready) << message(); EXPECT_EQ(output().count,150u);
    EXPECT_EQ(output().grid.dpiX,96); EXPECT_EQ(output().grid.dpiY,96);
    ASSERT_TRUE(output().display); EXPECT_TRUE(preview()); EXPECT_FALSE(proxyVisible());
    EXPECT_EQ(output().display->width, 5000u); EXPECT_EQ(output().display->height, 5000u);
    EXPECT_EQ(output().display->reservation.bytes(), 100000000u);
    EXPECT_NE(output().display->pixels.data(), output().alpha.pixels.data());
    EXPECT_NE(output().display->pixels.data(), output().grid.pixels.data());
#ifdef __APPLE__
    struct rusage usage{}; ASSERT_EQ(getrusage(RUSAGE_SELF, &usage), 0);
    std::cout << "P2_PANEL_MEASURE source=5000x5000 pieces=150 process_peak_rss_bytes=" << usage.ru_maxrss << std::endl;
#endif
    explode(); ASSERT_EQ(state(),State::Idle) << message(); EXPECT_EQ(desktop->getSelection()->size(),150u);
    DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(),before);
}
// The two real-photo sheets (sheet-*.png) are not distributed with the public
// source tree. Tests skip their cases, with a message, when a file is absent.
static bool explodeFixturePresent(char const *file)
{
    return std::filesystem::exists(std::filesystem::path(INKSCAPE_TESTS_DIR) / "data/explode-bitmap" / file);
}
TEST_F(ExplodeBitmapPanelTest, RealImportedFixturesReachExactResultsAndPublish) {
    std::string absent;
    for (auto [file, count] : {std::pair{"sheet-flowers-10.png", 10u}, {"sheet-stickers-30.png", 30u},
                             {"geo-spiral-star-line.png", 1u}, {"geo-interleaved-star-spiral-3arms.png", 3u},
                             {"geo-grid-64.png", 64u}}) {
        if (!explodeFixturePresent(file)) {
            absent += (absent.empty() ? "" : ", ") + std::string(file);
            continue;
        }
        SCOPED_TRACE(file);
        imported(std::string(INKSCAPE_TESTS_DIR) + "/data/explode-bitmap/" + file);
        auto before = xml(); finish();
        ASSERT_TRUE(resultReady()) << message();
        EXPECT_EQ(xml(), before);
        EXPECT_EQ(output().count, count) << message();
        if (count > MaxExplodePieces) {
            EXPECT_EQ(state(), State::TooMany) << message(); EXPECT_FALSE(explodeEnabled());
            explode(); EXPECT_EQ(xml(), before); continue;
        }
        ASSERT_EQ(state(), State::Ready) << message(); ASSERT_TRUE(explodeEnabled());
        EXPECT_EQ(output().pieces.count(), count); EXPECT_TRUE(drawOutlines());
        explode(); ASSERT_EQ(state(), State::Idle) << message();
        EXPECT_NE(xml(), before); EXPECT_EQ(desktop->getSelection()->size(), count);
        auto published = xml();
        auto reopened = SPDocument::createNewDocFromMem(published); ASSERT_TRUE(reopened);
        for (auto item : desktop->getSelection()->items()) {
            auto piece = cast<SPImage>(item); ASSERT_TRUE(piece); ASSERT_TRUE(piece->pixbuf);
            EXPECT_TRUE(cast<SPImage>(reopened->getObjectById(piece->getId())));
        }
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
        DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(), before);
        DocumentUndo::redo(doc.get()); EXPECT_EQ(xml(), published);
    }
    if (!absent.empty()) {
        GTEST_SKIP() << "Ran the fixtures that are present; not run because the image is absent: " << absent
                     << " (the real-photo sheets are not distributed with the public source tree).";
    }
}
TEST_F(ExplodeBitmapPanelTest, FaintFloorControlsMemoryDebounceAndExactStrings) {
    open(2, 15); auto before = xml(); finish();
    EXPECT_EQ(floorPhrase(), "Ignore pixels up to (%) [5]");
    EXPECT_NE(floorTooltip().find("Ignore pixels up to 5% opacity"), std::string::npos);
    std::ifstream po(std::filesystem::path(INKSCAPE_TESTS_DIR).parent_path() / "po/es.po");
    ASSERT_TRUE(po); std::string translations{std::istreambuf_iterator<char>(po), {}};
    EXPECT_NE(translations.find("msgid \"Ignore pixels up to (%)\"\nmsgstr \"Ignorar píxeles hasta (%)\""), std::string::npos);
    refine(false); finish(); ASSERT_EQ(state(), State::Ready) << message();
    EXPECT_TRUE(enabled(2)); EXPECT_FALSE(enabled(0)); EXPECT_FALSE(enabled(1));
    EXPECT_FALSE(preview()); EXPECT_FALSE(applyEnabled()); EXPECT_EQ(output().count, 2u); EXPECT_EQ(xml(), before);
    auto id = logicalImageIdentity(*image()); auto old = ticket();
    field(2, "26"); EXPECT_EQ(ticket(), old); EXPECT_EQ(query(id).faintFloor, 5u);
    field(2, "5.5"); EXPECT_EQ(ticket(), old);
    field(2, "6"); EXPECT_GT(ticket(), old); EXPECT_FALSE(working()); // Same 250 ms debounce as T/S.
    EXPECT_EQ(query(id).faintFloor, 6u); finish(); EXPECT_EQ(state(), State::AdjustmentEmpty);
    field(2, "0"); finish(); EXPECT_EQ(state(), State::Ready); EXPECT_FALSE(preview()); EXPECT_FALSE(applyEnabled());
    field(2, "5"); finish(); floorFocus(); pending(2, "20"); key(GDK_KEY_Escape);
    EXPECT_EQ(query(id).faintFloor, 5u); EXPECT_EQ(floorPhrase(), "Ignore pixels up to (%) [5]");
    key(GDK_KEY_Escape); panel->set_visible(true); clickPrimary(); finish();
    EXPECT_EQ(query(id).faintFloor, 5u); EXPECT_FALSE(query(id).refine); EXPECT_TRUE(enabled(2));
    EXPECT_EQ(xml(), before);
}
TEST_F(ExplodeBitmapPanelTest, FaintFloorNoOpOffDoesNotPreparePreviewApplyOrChangeHistory) {
    for (bool seeded : {false, true}) {
        SCOPED_TRACE(seeded);
        open(2, 15); refine(false); finish();
        SeededHistory history;
        if (seeded) history = seedHistory();
        auto before = xml();
        ASSERT_EQ(state(), State::Ready) << message();
        EXPECT_EQ(output().lost, 0u); EXPECT_EQ(output().count, 2u);
        EXPECT_FALSE(output().adjustment); EXPECT_FALSE(preview()); EXPECT_FALSE(applyEnabled());
        EXPECT_EQ(notice(), "Original transparency is preserved."); EXPECT_TRUE(explodeEnabled());
        apply(); drainEvents();
        EXPECT_FALSE(publicationPending()); EXPECT_EQ(state(), State::Ready);
        EXPECT_EQ(xml(), before);
        if (seeded) preservedHistory(history);
        else {
            auto usage = preflightUndo(*doc, {false, 0, 1}).usage;
            EXPECT_EQ(usage.undoCount, 0u); EXPECT_EQ(usage.redoCount, 0u);
        }
    }
}
TEST_F(ExplodeBitmapPanelTest, FaintFloorApplyOffBakesOnlyDustAndPreservesSixPercent) {
    auto raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 16, 4); gdk_pixbuf_fill(raw, 0);
    unsigned values[] = {1, 12, 13, 15, 255};
    for (unsigned i = 0; i < 5; ++i) {
        auto p = gdk_pixbuf_get_pixels(raw) + 4*i*3; p[0]=64; p[1]=112; p[2]=176; p[3]=values[i];
    }
    Pixbuf pixels(raw); auto uri = sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(uri);
    open(2, 255, false, {}, *uri); refine(false); auto before=xml(); finish();
    ASSERT_EQ(state(), State::Ready) << message(); EXPECT_EQ(output().count, 3u);
    EXPECT_EQ(output().lost, 2u); ASSERT_TRUE(applyEnabled()); EXPECT_TRUE(preview());
    apply(); finish(); ASSERT_FALSE(applyEnabled()); EXPECT_FALSE(preview());
    auto baked=xml(); EXPECT_NE(baked,before);
    auto href=image()->getRepr()->attribute("href"); ASSERT_TRUE(href); gsize bytes=0;
    auto encoded=g_base64_decode(href+22,&bytes);
    Budget budget(Budget::FixedLimitForTest{},16*MiB);
    auto decoded=decode({{encoded,bytes,{}},{}},budget); g_free(encoded); ASSERT_TRUE(decoded.ok());
    for (unsigned i=0;i<5;++i) {
        auto p=decoded.value.view().data+4*i*3; EXPECT_EQ(p[3],i<2 ? 0u : values[i]);
        EXPECT_EQ(p[0],64); EXPECT_EQ(p[1],112); EXPECT_EQ(p[2],176);
    }
    auto id=logicalImageIdentity(*image()); EXPECT_EQ(query(id).faintFloor,5u); EXPECT_TRUE(query(id).bypassAlpha);
    EXPECT_EQ(output().count,3u); EXPECT_EQ(preflightUndo(*doc,{false,0,1}).usage.undoCount,1u);
    key(GDK_KEY_z,Gdk::ModifierType::CONTROL_MASK); finish(); EXPECT_EQ(xml(),before);
    key(GDK_KEY_z,Gdk::ModifierType::CONTROL_MASK|Gdk::ModifierType::SHIFT_MASK); finish(); EXPECT_EQ(xml(),baked);
}
TEST_F(ExplodeBitmapPanelTest, FaintFloorDoesNotApplyTwiceAfterSoftRefinement) {
    open(2, 15); field(0,"25"); field(1,"15"); finish();
    // T26/S15 maps source alpha 15 to 12; that output must not be floored again.
    field(0,"26"); finish(); ASSERT_EQ(state(),State::Ready) << message();
    ASSERT_EQ(output().grid.view().data[3],12); // Input survives; refined output is allowed below the floor.
    apply(); finish(); ASSERT_EQ(state(),State::Ready) << message();
    EXPECT_EQ(output().grid.view().data[3],12); EXPECT_FALSE(applyEnabled());
    explode(); ASSERT_EQ(state(),State::Idle) << message(); EXPECT_EQ(desktop->getSelection()->size(),2u);
}
TEST_F(ExplodeBitmapPanelTest, FaintFloorRealPiecesMoveIndependentlyAndUndoAllPositions) {
    for (auto file : {"sheet-flowers-10.png", "sheet-stickers-30.png"}) {
        if (!explodeFixturePresent(file)) {
            GTEST_SKIP() << "Fixture image " << file << " is absent (the real-photo sheets are not distributed "
                            "with the public source tree).";
        }
    }
    for (bool on : {false,true}) for (auto [file,count] : {std::pair{"sheet-flowers-10.png",10u},{"sheet-stickers-30.png",30u}}) {
        SCOPED_TRACE(file);
        SCOPED_TRACE(on);
        imported(std::string(INKSCAPE_TESTS_DIR)+"/data/explode-bitmap/"+file); refine(on); finish();
        ASSERT_EQ(state(),State::Ready) << message(); ASSERT_EQ(output().count,count);
        ASSERT_TRUE(drawOutlines()); explode(); ASSERT_EQ(state(),State::Idle) << message();
        std::vector<SPImage *> pieces;
        for (auto item:desktop->getSelection()->items()) { auto im=cast<SPImage>(item); ASSERT_TRUE(im); pieces.push_back(im); }
        ASSERT_EQ(pieces.size(),count); closePanel();
        auto attributes=[](SPImage *im) {
            std::map<std::string,std::string> result;
            for (auto const &a:im->getRepr()->attributeList()) result.emplace(g_quark_to_string(a.key),a.value);
            return result;
        };
        auto rgba=[](SPImage *im) {
            Pixbuf p(*im->pixbuf); p.ensurePixelFormat(Pixbuf::PF_GDK); std::vector<unsigned char> bytes;
            for (int y=0;y<p.height();++y) bytes.insert(bytes.end(),p.pixels()+y*p.rowstride(),p.pixels()+y*p.rowstride()+p.width()*4);
            return bytes;
        };
        std::vector<std::vector<unsigned char>> pixels;
        std::vector<Geom::Affine> positions;
        std::vector<std::map<std::string,std::string>> originalAttrs;
        for (auto im:pieces) {
            pixels.push_back(rgba(im)); positions.push_back(im->c2p * im->i2dt_affine()); originalAttrs.push_back(attributes(im));
        }
        auto published=xml();
        for (unsigned i=0;i<count;++i) {
            std::vector<std::map<std::string,std::string>> attrs;
            for (auto im:pieces) attrs.push_back(attributes(im));
            desktop->getSelection()->set(pieces[i]);
            // The same native selection transform used by a drag; each piece gets a distinct offset.
            desktop->getSelection()->moveRelative(11.0*(i+1),-7.0*(i+1)); doc->ensureUpToDate();
            for (unsigned j=0;j<count;++j) {
                auto transform=pieces[j]->c2p * pieces[j]->i2dt_affine();
                for (unsigned k=0;k<6;++k) {
                    auto delta=j<=i ? k==4 ? 11.0*(j+1) : k==5 ? -7.0*(j+1) : 0 : 0;
                    // Native SVG writes use ten significant digits; bound their roundoff in desktop pixels.
                    ASSERT_NEAR(transform[k],positions[j][k]+delta,1e-6) << i << ":" << j << ":" << k;
                }
                EXPECT_TRUE(rgba(pieces[j])==pixels[j]);
                if (j!=i) EXPECT_TRUE(attributes(pieces[j])==attrs[j]);
                else {
                    auto after=attributes(pieces[j]);
                    for (auto key:{"transform","x","y","width","height"}) { after.erase(key); attrs[j].erase(key); }
                    EXPECT_TRUE(after==attrs[j]);
                }
            }
        }
        DocumentUndo::done(doc.get(),Util::Internal::ContextString("Move each exploded piece"),"");
        auto moved=xml(); EXPECT_NE(moved,published);
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        for (unsigned j=0;j<count;++j) {
            EXPECT_TRUE(attributes(pieces[j])==originalAttrs[j]); EXPECT_TRUE(rgba(pieces[j])==pixels[j]);
            EXPECT_TRUE(Geom::are_near(pieces[j]->c2p * pieces[j]->i2dt_affine(),positions[j],1e-9));
        }
        ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate(); EXPECT_TRUE(xml()==moved);
    }
}
// Explicit opt-in corpus runner uses the identical imported document, real worker,
// panel delivery and publication. Kept out of normal CTest because originals live
// outside the repository. EB_REAL1_FILE selects one file per isolated test process.
TEST_F(ExplodeBitmapPanelTest, DISABLED_RealImageCorpus) {
    auto path = g_getenv("EB_REAL1_FILE"); ASSERT_TRUE(path);
    imported(path);
    if (auto t = g_getenv("EB_REAL1_THRESHOLD")) field(0, t);
    if (auto s = g_getenv("EB_REAL1_SOFTNESS")) field(1, s);
    if (auto floor = g_getenv("EB_FLOOR_PERCENT")) field(2, floor);
    if (g_getenv("EB_REAL1_RAW")) refine(false);
    auto before = xml(); finish(); EXPECT_EQ(xml(), before);
    std::cout << "EB-REAL1 | " << std::filesystem::path(path).filename().string() << " | " << message();
    if (resultReady()) {
        auto const &out = output();
        std::cout << " | count=" << out.count << " initial=" << out.initial << " joins=" << out.joined;
        std::cout << " topology=" << out.topologyPeak << " runs=" << out.topologyRuns;
        std::cout << " outlines=" << (out.outlines.storage ? out.outlines.storage->count : 0) << " shown=" << (state()==State::Ready && drawOutlines());
    }
    std::cout << std::endl;
    if (state() == State::Ready) {
        ASSERT_TRUE(drawOutlines()) << notice();
        auto count = output().count; explode();
        EXPECT_EQ(state(), State::Idle) << message();
        EXPECT_EQ(desktop->getSelection()->size(), count);
        std::cout << "EB-REAL1 publication | " << message() << std::endl;
    }
}

#ifdef __APPLE__
TEST_F(ExplodeBitmapPanelTest, NativeSliderReleaseCommitsAndLaterEscapeDoesNotRollback) {
    open(); finish(); auto before = xml(); auto legacy = legacyGestureProbe(0);
    nativeSlider(0, 1, 0.5); ASSERT_TRUE(dragging());
    nativeSlider(0, 6, 0.6); auto value = threshold();
    EXPECT_EQ(legacy->pressed, 1u);
    EXPECT_EQ(gtk_gesture_get_sequence_state(GTK_GESTURE(legacy->gesture->gobj()), nullptr), GTK_EVENT_SEQUENCE_DENIED);
    nativeSlider(0, 2, 0.6); EXPECT_EQ(legacy->released, 0u); ASSERT_NE(value, 128u); EXPECT_FALSE(dragging()); finish();
    EXPECT_EQ(query(logicalImageIdentity(*image())).threshold, value);
    key(GDK_KEY_Escape); EXPECT_EQ(state(), State::Idle); EXPECT_TRUE(panel->get_visible());
    key(GDK_KEY_Escape); EXPECT_FALSE(panel->get_visible());
    EXPECT_EQ(query(logicalImageIdentity(*image())).threshold, value); EXPECT_EQ(xml(), before);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
}
TEST_F(ExplodeBitmapPanelTest, AppliedNativeDragEscapePreservesBakedBypassAndPixels) {
    open(); field(0, "140"); finish(); apply(); finish(); auto baked = xml();
    ASSERT_TRUE(query(logicalImageIdentity(*image())).bypassAlpha); ASSERT_FALSE(applyEnabled());
    nativeSlider(0, 1, 140.0/255); ASSERT_TRUE(dragging()); nativeSlider(0, 6, 0.7);
    key(GDK_KEY_Escape); nativeSlider(0, 2, 0.7); finish();
    EXPECT_EQ(query(logicalImageIdentity(*image())).threshold, 140u);
    EXPECT_TRUE(query(logicalImageIdentity(*image())).bypassAlpha); EXPECT_FALSE(preview());
    ASSERT_EQ(state(), State::Ready) << message(); EXPECT_FALSE(applyEnabled()); EXPECT_TRUE(explodeEnabled());
    apply(); EXPECT_EQ(xml(), baked); EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
    panel->set_visible(false); panel->set_visible(true); EXPECT_EQ(state(), State::Idle); clickPrimary(); finish(); EXPECT_FALSE(applyEnabled());
    explode(); ASSERT_EQ(state(), State::Idle) << message();
    for (auto item : desktop->getSelection()->items()) {
        auto im = cast<SPImage>(item); ASSERT_TRUE(im); Pixbuf p(*im->pixbuf); p.ensurePixelFormat(Pixbuf::PF_GDK);
        for (int y = 0; y < p.height(); ++y) for (int x = 0; x < p.width(); ++x) {
            auto alpha = p.pixels()[y*p.rowstride()+4*x+3]; if (alpha) EXPECT_EQ(alpha, 72);
        }
    }
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 2u);
}
#endif

TEST_F(ExplodeBitmapPanelTest, P2MagnifiedExactPreviewMatchesApplyWithLiveEffectsBothOrders)
{
    for (bool alphaFirst : {false, true}) {
        auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 16, 4);
        gdk_pixbuf_fill(gdk, 0x4070b000);
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 16; ++x)
            gdk_pixbuf_get_pixels(gdk)[y*gdk_pixbuf_get_rowstride(gdk)+4*x+3] =
                x == 1 ? 90 : x == 2 ? 128 : x == 3 ? 166 : x < 12 ? 255 : 0;
        Pixbuf pixels(gdk); auto uri = sp_image_encode_png_data_uri(pixels); ASSERT_TRUE(uri);
        open(1, 128, false, "preserveAspectRatio='none'", *uri, nullptr, 0, true, false);
        auto repr = image()->getRepr(); repr->setAttribute("width", "16"); repr->setAttribute("height", "4");
        auto xmlDoc = doc->getReprDoc();
        auto defs = xmlDoc->createElement("svg:defs"); doc->getReprRoot()->appendChild(defs);
        for (auto id : {"own", "ancestor"}) {
            auto clip = xmlDoc->createElement("svg:clipPath"); clip->setAttribute("id", id); defs->appendChild(clip);
            auto rect = xmlDoc->createElement("svg:rect"); rect->setAttribute("width", "11"); rect->setAttribute("height", "3"); clip->appendChild(rect);
            GC::release(rect); GC::release(clip);
        }
        GC::release(defs);
        auto group = xmlDoc->createElement("svg:g"); doc->getReprRoot()->appendChild(group);
        group->setAttribute("id", "parent"); group->setAttribute("opacity", "0.6");
        GC::anchor(repr); repr->parent()->removeChild(repr); group->appendChild(repr); GC::release(repr); GC::release(group);
        desktop->getSelection()->set(image());
        repr->setAttribute("opacity", "0.5"); repr->setAttribute("clip-path", "url(#own)");
        Filters::BitmapToneSettings tone; tone.brightness = 20;
        ASSERT_TRUE(BitmapAdjustments::apply_tone(image(), tone));
        doc->ensureUpToDate(); DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture"), "");
        DocumentUndo::clearUndo(doc.get()); doc->setModifiedSinceSave(false);
        clickPrimary(); finish(); ASSERT_EQ(state(), State::Ready) << message();
        ASSERT_TRUE(output().display); ASSERT_TRUE(output().exactSourceMapping);
        Drawing drawing; auto key = SPItem::display_key_new(1);
        drawing.setRoot(doc->getRoot()->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY));
        auto hide = scope_exit{[&] { doc->getRoot()->invoke_hide(key); }};
        drawing.root()->setTransform(Geom::Scale(8));
        auto render = [&] {
            drawing.update(); auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 128, 32);
            DrawingSurface target(surface->cobj(), Geom::IntPoint(0,0)); DrawingContext context(target);
            drawing.render(context, Geom::IntRect::from_xywh(0,0,128,32)); surface->flush();
            return std::vector<unsigned char>(surface->get_data(), surface->get_data()+surface->get_stride()*32);
        };
        auto original = render(); auto before = xml();
        Contribution alpha; alpha.layer = PreviewLayer::Alpha; alpha.source = image()->pixbuf;
        alpha.backing = output().display; alpha.pixels = wrapAlphaDisplay(alpha.backing);
        Contribution liveTone; tone.brightness = 20; liveTone.tone = tone;
        ClientLease ac, tc;
        auto addAlpha = [&] { ac = contribute({image(),key}, alpha); EXPECT_TRUE(Bitmap::update(ac,1).ok()); };
        auto addTone = [&] { tc = contribute({image(),key}, liveTone); EXPECT_TRUE(Bitmap::update(tc,1).ok()); };
        if (alphaFirst) { addAlpha(); addTone(); } else { addTone(); addAlpha(); }
        auto previewPixels = render(); EXPECT_NE(previewPixels, original);
        EXPECT_EQ(xml(), before); EXPECT_EQ(preflightUndo(*doc, {false,0,1}).usage.undoCount, 0u);
        ac.reset(); tc.reset(); EXPECT_EQ(render(), original); EXPECT_EQ(xml(), before);
        apply(); EXPECT_NE(xml(), before);
        EXPECT_EQ(render(), previewPixels);
        EXPECT_EQ(preflightUndo(*doc, {false,0,1}).usage.undoCount, 1u);

        // The target resolver deliberately refuses ancestor clip/filter during
        // Analyze. Exercise their live render composition on copies of the real
        // pre-Apply and committed documents, without broadening target policy.
        auto sourceDoc = SPDocument::createNewDocFromMem(before);
        auto appliedDoc = SPDocument::createNewDocFromMem(xml());
        for (auto d : {sourceDoc.get(), appliedDoc.get()}) {
            auto parent = cast<SPItem>(d->getObjectById("parent"));
            parent->getRepr()->setAttribute("clip-path", "url(#ancestor)");
            tone.brightness = -15; ASSERT_TRUE(BitmapAdjustments::apply_tone(parent, tone));
            d->ensureUpToDate();
        }
        auto sourceImage = cast<SPImage>(sourceDoc->getObjectById("im"));
        Drawing previewDrawing, committedDrawing, exportDrawing;
        auto pk = SPItem::display_key_new(1), ck = SPItem::display_key_new(1), ek = SPItem::display_key_new(1);
        previewDrawing.setRoot(sourceDoc->getRoot()->invoke_show(previewDrawing, pk, SP_ITEM_SHOW_DISPLAY));
        committedDrawing.setRoot(appliedDoc->getRoot()->invoke_show(committedDrawing, ck, SP_ITEM_SHOW_DISPLAY));
        exportDrawing.setRoot(sourceDoc->getRoot()->invoke_show(exportDrawing, ek, SP_ITEM_SHOW_DISPLAY));
        auto hideCopies = scope_exit{[&] { sourceDoc->getRoot()->invoke_hide(pk); sourceDoc->getRoot()->invoke_hide(ek); appliedDoc->getRoot()->invoke_hide(ck); }};
        auto magnified = [](Drawing &d) {
            d.root()->setTransform(Geom::Scale(8)); d.update();
            auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,128,32);
            DrawingSurface target(surface->cobj(),Geom::IntPoint(0,0)); DrawingContext context(target);
            d.render(context,Geom::IntRect::from_xywh(0,0,128,32)); surface->flush();
            return std::vector<unsigned char>(surface->get_data(),surface->get_data()+surface->get_stride()*32);
        };
        auto exportBefore = magnified(exportDrawing);
        auto sourceXml = sp_repr_save_buf(sourceDoc->getReprDoc());
        alpha.source = sourceImage->pixbuf;
        ClientLease copiedAlpha, copiedTone;
        auto ca = [&] { copiedAlpha = contribute({sourceImage,pk},alpha); EXPECT_TRUE(Bitmap::update(copiedAlpha,1).ok()); };
        auto ct = [&] { copiedTone = contribute({sourceImage,pk},liveTone); EXPECT_TRUE(Bitmap::update(copiedTone,1).ok()); };
        if (alphaFirst) { ca(); ct(); } else { ct(); ca(); }
        EXPECT_EQ(magnified(previewDrawing),magnified(committedDrawing));
        EXPECT_EQ(magnified(exportDrawing),exportBefore);
        EXPECT_EQ(sp_repr_save_buf(sourceDoc->getReprDoc()),sourceXml);
        copiedAlpha.reset(); copiedTone.reset();
        EXPECT_EQ(magnified(previewDrawing),exportBefore);
    }
}

TEST_F(ExplodeBitmapPanelTest, ContourControlsDefaultOffWithoutWindow) { unmapped(); checkContourControls(); }
TEST_F(ExplodeBitmapPanelTest, ContourDispatchFullThenOnlyDebounceOffAndColor) {
    open(2,255); finish(); ASSERT_EQ(state(), State::Ready); EXPECT_FALSE(output().analysis);
    auto full = workerStarts.load(), only = contourStarts.load(), enabledFull = fullWithContours.load();
    contours(true); finish(); ASSERT_EQ(state(), State::Ready) << message();
    EXPECT_EQ(workerStarts, full+1); EXPECT_EQ(fullWithContours, enabledFull+1); ASSERT_TRUE(output().analysis);
    ASSERT_TRUE(contourPreview()) << message(); EXPECT_NE(message().find("contours ready"), std::string::npos) << message();
    for (auto &phase : phases) phase = 0;
    contourField(0, .1); contourField(0, .2); contourField(1, 65); contourField(2, .4);
    EXPECT_EQ(contourStarts, only); EXPECT_FALSE(contourPreview()); finish();
    EXPECT_EQ(contourStarts, only+1); EXPECT_EQ(workerStarts, full+1);
    for (unsigned i=0; i<4; ++i) EXPECT_EQ(phases[i], 0u);
    EXPECT_GT(phases[4], 0u); EXPECT_GT(phases[5], 0u);
    auto currentTicket = ticket(); contourColor("#00ff00"); EXPECT_EQ(ticket(), currentTicket); EXPECT_TRUE(contourPreview());
    contours(false); EXPECT_EQ(ticket(), currentTicket); EXPECT_FALSE(contourPreview()); expectNoWorkerStarts();
    EXPECT_EQ(contourStarts, only+1);
    contours(true); finish(); field(0,"129"); finish();
    EXPECT_EQ(workerStarts, full+3); EXPECT_EQ(fullWithContours, enabledFull+3);
}
TEST_F(ExplodeBitmapPanelTest, ContourPreviewLifecycleZoomAndNoXml) {
    contourGeometryFixture(); auto before = xml();
    checkContourPreviewDrawing(); EXPECT_EQ(xml(), before);
    compare(true); EXPECT_FALSE(contourPreview()); compare(false); EXPECT_TRUE(contourPreview());
    contourField(0,.1); EXPECT_FALSE(contourPreview()); finish(); EXPECT_TRUE(contourPreview());
    image()->getRepr()->setAttribute("x","3"); DocumentUndo::done(doc.get(), Util::Internal::ContextString("Move contour source"), ""); doc->ensureUpToDate(); refreshPanel(); EXPECT_EQ(state(),State::Stale); EXPECT_FALSE(contourPreview());
    inspect(); finish(); EXPECT_TRUE(contourPreview()) << message(); key(GDK_KEY_Escape); EXPECT_FALSE(contourPreview()); EXPECT_EQ(state(),State::Idle);
    inspect(); finish(); EXPECT_TRUE(contourPreview()) << message(); host->unset_child(); EXPECT_FALSE(contourPreview());
}
TEST_F(ExplodeBitmapPanelTest, ContourPublicationGroupsAndContourOnlyOneUndo) {
    for (bool only : {false,true}) {
        open(2,255); finish(); auto before = xml(); contours(true); finish(); ASSERT_TRUE(contourPreview()) << message();
        contourColor("#00ff00");
        if (only) contourOnly(); else explode();
        EXPECT_EQ(state(),State::Idle) << message(); EXPECT_FALSE(contourPreview());
        EXPECT_EQ(desktop->getSelection()->size(), only ? 1u : 2u);
        for (auto item : desktop->getSelection()->items()) {
            ASSERT_TRUE(is<SPGroup>(item)); auto child=item->getRepr()->firstChild(); ASSERT_TRUE(child);
            EXPECT_STREQ(child->name(),"svg:image"); ASSERT_TRUE(child->next()); EXPECT_STREQ(child->next()->name(),"svg:path");
            EXPECT_NE(std::string(child->next()->attribute("style")).find("#00ff00"),std::string::npos);
        }
        EXPECT_EQ(preflightUndo(*doc,{false,0,1}).usage.undoCount,1u);
        auto published = xml();
        DocumentUndo::undo(doc.get()); EXPECT_EQ(xml(),before); DocumentUndo::redo(doc.get());
        EXPECT_EQ(xml(), published);
    }
    open(2,255); finish(); explode(); for (auto item : desktop->getSelection()->items()) EXPECT_TRUE(is<SPImage>(item));
}
TEST_F(ExplodeBitmapPanelTest, ContourRefusalStillPublishesPieces) {
    open(2,255); finish(); contours(true); finish(); refuseContours();
    ASSERT_EQ(state(),State::Ready) << message(); EXPECT_TRUE(explodeEnabled()); EXPECT_FALSE(contourPreview());
    EXPECT_NE(message().find("Contours unavailable: Test contour refusal. Explode will create pieces without contours."),std::string::npos);
    explode(); EXPECT_EQ(state(),State::Idle); for (auto item : desktop->getSelection()->items()) EXPECT_TRUE(is<SPImage>(item));
}
TEST_F(ExplodeBitmapPanelTest, ContourOnWithoutRetainedStateAlwaysUsesFullAnalysis) {
    open(2,255); finish(); auto starts = workerStarts.load(); contours(true); finish(); EXPECT_EQ(workerStarts,starts+1);
    ASSERT_EQ(state(),State::Ready) << message(); auto only = contourStarts.load(); staleContourIdentity(); finish(); EXPECT_EQ(workerStarts,starts+2); EXPECT_EQ(contourStarts,only);
}

TEST_F(ExplodeBitmapPanelTest, ContourStatesFitNarrowWideLightDarkWindows) { checkContourLayouts(); }
TEST_F(ExplodeBitmapPanelTest, ContourAbsentStatusAndOffCancelsPendingOnlyJob) {
    open(2,255); finish(); contours(true); finish(); contourField(0,-10); finish();
    ASSERT_EQ(state(),State::Ready) << message(); EXPECT_TRUE(explodeEnabled());
    EXPECT_NE(message().find("2 pieces · 2 without contour at these settings"), std::string::npos);
    EXPECT_TRUE(contourOnlyEnabled()); EXPECT_FALSE(outlines());
    auto before = xml(); contourOnly(); EXPECT_EQ(state(),State::Ready); EXPECT_EQ(xml(), before);
    EXPECT_TRUE(explodeEnabled()); EXPECT_EQ(preflightUndo(*doc,{false,0,1}).usage.undoCount,0u);
    EXPECT_NE(message().find("No Undo step was added"),std::string::npos);
    auto full = workerStarts.load(), only = contourStarts.load();
    contourField(0,0); contours(false); finish(); expectNoWorkerStarts();
    EXPECT_EQ(workerStarts,full); EXPECT_EQ(contourStarts,only); EXPECT_FALSE(contourPreview()); EXPECT_TRUE(explodeEnabled());
}
TEST_F(ExplodeBitmapPanelTest, ContourAdmissionRefusalKeepsExplodeAvailable) {
    struct Room : MemoryProbe { bool read(RawMemory &m) const noexcept override { m={16384*MiB,700*MiB,256*MiB}; return true; } } room;
    open(2,255,false,{},{},&room); finish(); ASSERT_EQ(state(),State::Ready) << message();
    contours(true); finish(); ASSERT_EQ(state(),State::Ready) << message();
    EXPECT_TRUE(explodeEnabled()); EXPECT_FALSE(contourPreview()); EXPECT_FALSE(contourOnlyEnabled());
    EXPECT_NE(message().find("Contours unavailable:"),std::string::npos);
}

TEST_F(ExplodeBitmapPanelTest, ContourStrictTextGuardsAllPublicationAndEscape) { checkContourValidation(); }
TEST_F(ExplodeBitmapPanelTest, ContourSliderQuantizationMatchesDisplayAndSession) { checkContourQuantization(); }
TEST_F(ExplodeBitmapPanelTest, ContourOffPreservesAnalysisClassification) { checkContourOffClassification(); }
TEST_F(ExplodeBitmapPanelTest, ContourOffDisablesControlsDuringInitialAnalysis) { checkContourOffDuringInitialAnalysis(); }
TEST_F(ExplodeBitmapPanelTest, ContourRefusalSurvivesPreviewWarnings) { checkContourRefusalWithPreviewWarnings(); }

TEST_F(ExplodeBitmapPanelTest, R3MainThreadHrefAllocationRefusalPreservesDocumentAndAllowsRetry) {
    auto pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 16, 4); gdk_pixbuf_fill(pix, 0);
    auto pixels = gdk_pixbuf_get_pixels(pix);
    for (unsigned x : {0u, 3u}) { pixels[x*4] = 64; pixels[x*4+3] = 128; }
    Pixbuf source(pix); auto href = sp_image_encode_png_data_uri(source); ASSERT_TRUE(href);
    *href += std::string(2*MiB, ' '); // valid base64 whitespace makes the main-thread copy large
    open(2, 128, false, {}, *href, nullptr, 0, true, false);
    auto before = xml(); auto starts = workerStarts.load();
    AllocationFault fault{1}; inputFault(&fault);
    clickPrimary(); ASSERT_EQ(state(), State::Failed) << message();
    EXPECT_EQ(fault.attempts, 1u); EXPECT_EQ(workerStarts.load(), starts);
    EXPECT_NE(message().find("main-thread allocator / image input"), std::string::npos);
    EXPECT_NE(message().find("estimated need 2.00 MiB"), std::string::npos);
    EXPECT_NE(message().find("estimated need"), std::string::npos);
    EXPECT_NE(message().find("available"), std::string::npos);
    EXPECT_NE(message().find("MiB"), std::string::npos);
    EXPECT_EQ(reserved(), 0u); EXPECT_FALSE(resultReady()); EXPECT_FALSE(preview());
    EXPECT_EQ(xml(), before); EXPECT_EQ(desktop->getSelection()->singleItem(), image());
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
    inputFault(nullptr); clickPrimary(); finish(); ASSERT_EQ(state(), State::Ready) << message();
    EXPECT_EQ(output().count, 2u); EXPECT_EQ(xml(), before);
}

namespace {
std::string r4ProfilePng(std::size_t profileSize) {
    std::vector<unsigned char> raw(profileSize - 1024);
    std::uint32_t random = 0x12345678;
    for (auto &byte : raw) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; byte = random; }
    auto profile = cmsCreate_sRGBProfile();
    EXPECT_TRUE(cmsWriteRawTag(profile, static_cast<cmsTagSignature>(0x74347374), raw.data(), raw.size()));
    cmsUInt32Number size = 0; EXPECT_TRUE(cmsSaveProfileToMem(profile,nullptr,&size));
    std::vector<unsigned char> icc(size); EXPECT_TRUE(cmsSaveProfileToMem(profile,icc.data(),&size)); cmsCloseProfile(profile);
    uLongf compressedSize = compressBound(icc.size()); std::vector<unsigned char> compressed(compressedSize);
    EXPECT_EQ(compress2(compressed.data(),&compressedSize,icc.data(),icc.size(),Z_BEST_SPEED), Z_OK);
    compressed.resize(compressedSize);
    EXPECT_GT(compressed.size(), MiB); EXPECT_LE(compressed.size()+4, 4*MiB);
    std::vector<unsigned char> bytes{137,80,78,71,13,10,26,10};
    auto integer = [](auto &out, std::uint32_t v) { for (int shift=24;shift>=0;shift-=8) out.push_back(v>>shift); };
    auto chunk = [&](char const *name, auto const &data) {
        integer(bytes,data.size()); auto from=bytes.size(); bytes.insert(bytes.end(),name,name+4);
        bytes.insert(bytes.end(),data.begin(),data.end()); integer(bytes,crc32(0,bytes.data()+from,bytes.size()-from));
    };
    std::vector<unsigned char> ihdr; integer(ihdr,5000); integer(ihdr,5000); ihdr.insert(ihdr.end(),{8,6,0,0,0}); chunk("IHDR",ihdr);
    std::vector<unsigned char> iccp{'r','4',0,0}; iccp.insert(iccp.end(),compressed.begin(),compressed.end()); chunk("iCCP",iccp);
    // One opaque component inside a transparent border, with bounded row storage.
    std::vector<unsigned char> row(5000*4+1,0), border(row.size(),0);
    for (unsigned x=1;x<4999;++x) row[1+x*4+3]=255;
    z_stream stream{}; EXPECT_EQ(deflateInit(&stream,Z_BEST_SPEED), Z_OK);
    std::vector<unsigned char> idat; unsigned char buffer[65536];
    for (unsigned y=0;y<5000;++y) {
        stream.next_in=(y==0 || y==4999) ? border.data() : row.data(); stream.avail_in=row.size();
        do {
            stream.next_out=buffer; stream.avail_out=sizeof(buffer);
            EXPECT_EQ(deflate(&stream,Z_NO_FLUSH), Z_OK); idat.insert(idat.end(),buffer,buffer+sizeof(buffer)-stream.avail_out);
        } while (stream.avail_in);
    }
    int result;
    do { stream.next_out=buffer; stream.avail_out=sizeof(buffer); result=deflate(&stream,Z_FINISH);
         idat.insert(idat.end(),buffer,buffer+sizeof(buffer)-stream.avail_out); } while (result==Z_OK);
    EXPECT_EQ(result,Z_STREAM_END); deflateEnd(&stream); chunk("IDAT",idat); chunk("IEND",std::vector<unsigned char>{});
    auto encoded=g_base64_encode(bytes.data(),bytes.size()); std::string uri=std::string("data:image/png;base64,")+encoded; g_free(encoded); return uri;
}
}

TEST_F(ExplodeBitmapPanelTest, R4PermittedLargeProfilesReachAnalysis) {
    struct Room : MemoryProbe { bool read(RawMemory &m) const noexcept override { m={16384*MiB,8192*MiB,256*MiB}; return true; } } room;
    for (auto size : {2*MiB,39*MiB/10}) {
        SCOPED_TRACE(size); auto uri=r4ProfilePng(size);
        open(1,255,true,{},uri,&room,0,true,false);
        ASSERT_TRUE(resolve(*desktop, Intent::Explode).ok()) << message(); auto before=xml(); auto starts=workerStarts.load();
        EXPECT_FALSE(image()->missing); ASSERT_EQ(image()->pixbuf->width(),5000); ASSERT_EQ(image()->pixbuf->height(),5000);
        clickPrimary(); EXPECT_NE(ticket(),0u); finish();
        EXPECT_GT(workerStarts.load(),starts); ASSERT_EQ(state(),State::Ready) << message();
        EXPECT_EQ(output().count,1u); EXPECT_EQ(xml(),before);
        EXPECT_EQ(preflightUndo(*doc,{false,0,1}).usage.undoCount,0u);
    }
}

}
