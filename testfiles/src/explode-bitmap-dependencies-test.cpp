// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <thread>
#include "bitmap-adjustment-chemistry.h"
#include "bitmap-copy-outcome.h"
#include "bitmap-explode-chemistry.h"
#include "desktop.h"
#include "display/cairo-utils.h"
#include "display/drawing-image.h"
#include "display/drawing.h"
#include "document.h"
#include "event-log.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "inkgc/gc-core.h"
#include "layer-manager.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "object/weakptr.h"
#include "selection.h"
#include "style.h"
#include "ui/explode-bitmap-dependencies.h"
#include "ui/explode-bitmap-publication.h"
#include "ui/explode-bitmap-undo.h"
#include "xml/node.h"
#include "xml/node-observer.h"
#include "xml/repr.h"
using namespace Inkscape;
using namespace Inkscape::Bitmap;
namespace {
constexpr auto png = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
std::string bitmap() { return std::string("<image id='im' width='10' height='10' href='") + png + "'/>"; }
std::uintptr_t identity(void const *p) { return reinterpret_cast<std::uintptr_t>(p); }
PlatformEvidence policyEvidence(EvidencePlatform platform = EvidencePlatform::Windows) {
    PlatformEvidence e; e.platform = platform; e.observationNs = e.clipNs = 20000; return e;
}
class Dependencies : public ::testing::Test {
protected:
    void SetUp() override {
        static auto *app = [] { g_setenv("INKSCAPE_APP_ID_TAG", "eb4dependencies", TRUE); return new InkscapeApplication; }();
        ASSERT_TRUE(app->gtk_app()); if (!Application::exists()) Application::create(false);
    }
    void open(std::string body = bitmap()) {
        lease.reset(); desktop.reset();
        document = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' width='40' height='30'>" + body + "</svg>");
        ASSERT_TRUE(document); document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        desktop->getSelection()->set(item("im")); document->ensureUpToDate();
        lease = std::make_unique<DependencyLease>(*desktop, policyEvidence());
    }
    SPItem *item(char const *name) { return cast<SPItem>(document->getObjectById(name)); }
    DependencyToken token() {
        document->ensureUpToDate(); auto target = resolve(*desktop, Intent::Explode);
        EXPECT_TRUE(target.ok()) << target.outcome.diagnostic;
        auto result = capture(target.value); EXPECT_TRUE(bool(result)); return result;
    }
    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<DependencyLease> lease;
};
TEST_F(Dependencies, IdenticalSelectionAndCompatibleToolsPreserveCapture) {
    open(); auto t = token(); ASSERT_TRUE(valid(t));
    auto selected = desktop->getSelection()->items_vector();
    desktop->getSelection()->addList(selected); EXPECT_TRUE(valid(t));
    for (auto tool : {"/tools/zoom", "/tools/select", "/tools/shapes/rect", "/tools/select"}) {
        desktop->setTool(tool); EXPECT_TRUE(valid(t)) << tool;
    }
    desktop->setTool("/tools/text"); EXPECT_FALSE(valid(t));
    desktop->setTool("/tools/select"); EXPECT_FALSE(valid(t));
}
TEST_F(Dependencies, SelectionDepartureAndReturnInvalidatesCapture) {
    open(); auto t = token();
    auto selected = desktop->getSelection()->items_vector();
    desktop->getSelection()->clear(); desktop->getSelection()->addList(selected);
    EXPECT_FALSE(valid(t));
}
TEST_F(Dependencies, T09DeleteSameIdUndoRedoAndExpiredLease) {
    open(); auto t = token(); auto before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto original = item("im")->getRepr(); GC::anchor(original);
    auto copy = original->duplicate(document->getReprDoc());
    item("im")->deleteObject(); document->getReprRoot()->appendChild(copy); GC::release(copy);
    document->ensureUpToDate(); desktop->getSelection()->set(item("im"));
    EXPECT_FALSE(valid(t)); auto replacement = token(); EXPECT_TRUE(valid(replacement));
    DocumentUndo::done(document.get(), Util::Internal::ContextString("Replace"), "");
    DocumentUndo::undo(document.get()); document->ensureUpToDate(); desktop->getSelection()->set(item("im"));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(valid(t)); EXPECT_FALSE(valid(replacement)); auto restored = token();
    DocumentUndo::redo(document.get()); EXPECT_FALSE(valid(restored)); GC::release(original);
    lease.reset(); EXPECT_FALSE(valid(restored)); EXPECT_FALSE(capture(resolve(*desktop, Intent::Explode).value));
}
TEST_F(Dependencies, T09ClosedDesktopAndReboundDocumentCannotBeDereferenced) {
    open(); auto t = token(); auto snapshot = resolve(*desktop, Intent::Explode).value;
    desktop.reset(); document.reset(); EXPECT_FALSE(valid(t)); EXPECT_FALSE(capture(snapshot));
    lease.reset(); EXPECT_FALSE(valid(t));
    open(); t = token(); auto other = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'/>");
    desktop->setDocument(other.get()); desktop->setDocument(document.get()); EXPECT_FALSE(valid(t));
}
TEST_F(Dependencies, T09RebindDuringLivePublicationDisconnectsSelection) {
    open(); auto t = token();
    auto other = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'><rect id='other'/></svg>");
    auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
    {
        DependencyPublication p(t, *guard, {}); ASSERT_NE(p.epoch().value, 0u);
        desktop->setDocument(other.get());
        desktop->getSelection()->set(cast<SPItem>(other->getObjectById("other")));
        desktop->getSelection()->clear(); // must not dereference the detached document
        EXPECT_FALSE(valid(t, p.epoch())); EXPECT_EQ(lease->nativeWatchCount(), 0u);
        guard->rollback(); guard.reset(); document.reset(); // free the original document
        desktop->getSelection()->set(cast<SPItem>(other->getObjectById("other")));
        desktop->getSelection()->clear(); EXPECT_FALSE(valid(t, p.epoch()));
    }
    desktop.reset(); // other outlives its desktop
}
TEST_F(Dependencies, T11AllCheckpointsConservativelyRejectDocumentDependencies) {
    // Exact original/readiness is checked at all four boundaries, not just admission.
    for (auto point : {DependencyCheck::Dispatch, DependencyCheck::Delivery, DependencyCheck::Admission, DependencyCheck::Settlement}) {
        for (auto attribute : {"href", "transform", "width", "height", "opacity", "style", "sodipodi:insensitive", "id"}) {
            open("<g id='g'>" + bitmap() + "</g><defs><clipPath id='clip'><rect id='resource' width='5' height='5'/></clipPath></defs>");
            auto t = token(); DependencyRequest request; ASSERT_TRUE(request.check(t, point).ok());
            item("im")->getRepr()->setAttribute(attribute, "changed"); EXPECT_FALSE(request.check(t, point).ok()) << attribute;
        }
        for (auto which : {"g", "resource"}) {
            open("<g id='g'>" + bitmap() + "</g><defs><clipPath id='clip'><rect id='resource' width='5' height='5'/></clipPath></defs>");
            auto t = token(); item(which)->getRepr()->setAttribute("transform", "translate(4,5)");
            EXPECT_FALSE(DependencyRequest{}.check(t, point).ok());
        }
        for (auto addition : {"<style>image {opacity:0.2}</style>", "<use href='#im'/>", "<rect aria-labelledby='im'/>", "<mask id='newmask'/>", "<filter id='newfilter'/>"}) {
            open(); auto t = token(); auto extra = SPDocument::createNewDocFromMem(std::string("<svg xmlns='http://www.w3.org/2000/svg'>") + addition + "</svg>");
            auto node = extra->getReprRoot()->lastChild()->duplicate(document->getReprDoc());
            document->getReprRoot()->appendChild(node); GC::release(node);
            EXPECT_FALSE(DependencyRequest{}.check(t, point).ok());
        }
    }
}
TEST_F(Dependencies, T11NativeChangeRevertRecipeSelectionToneAndView) {
    open("<g id='g'>" + bitmap() + "</g>");
    for (auto name : {"im", "g"}) {
        auto t = token(); auto old = item(name)->i2dt_affine();
        item(name)->set_i2d_affine(old * Geom::Translate(2, 3)); EXPECT_FALSE(valid(t));
        item(name)->set_i2d_affine(old); document->ensureUpToDate(); EXPECT_FALSE(valid(t));
    }
    auto t = token(); item("im")->style->opacity.read("0.5");
    item("im")->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG); EXPECT_FALSE(valid(t));
    t = token(); lease->invalidate(); EXPECT_FALSE(valid(t)); // explicit T/S generation
    t = token(); desktop->getSelection()->clear(); EXPECT_FALSE(valid(t)); desktop->getSelection()->set(item("im"));
    t = token(); Filters::BitmapToneSettings settings; settings.brightness = 0.3;
    ASSERT_TRUE(BitmapAdjustments::apply_tone(item("im"), settings)); EXPECT_FALSE(valid(t));
    t = token(); auto image = cast<SPImage>(item("im")); image->pixbuf.reset(); EXPECT_FALSE(valid(t));
}
TEST_F(Dependencies, T11SlotViewportClipAndNativeFilterGenerations) {
    open("<g id='g'>" + bitmap() + "<rect id='sibling' width='1' height='1'/></g>"
         "<defs><clipPath id='c'><rect width='2' height='2'/></clipPath></defs>");
    auto t = token(); auto image = item("im");
    image->getRepr()->setAttribute("clip-path", "url(#c)"); EXPECT_FALSE(valid(t));
    t = token(); image->getRepr()->setAttribute("preserveAspectRatio", "none"); EXPECT_FALSE(valid(t));
    t = token(); auto parent = image->getRepr()->parent();
    parent->changeOrder(image->getRepr(), item("sibling")->getRepr()); EXPECT_FALSE(valid(t));
    t = token(); document->getReprRoot()->setAttribute("viewBox", "0 0 80 60"); EXPECT_FALSE(valid(t));
    t = token(); item("g")->getRepr()->setAttribute("sodipodi:insensitive", "true"); EXPECT_FALSE(valid(t));
}
TEST_F(Dependencies, T28ConfirmationKeepsDocumentAndHistoryReadOnly) {
    open(); auto t = token(); auto before = sp_repr_save_buf(document->getReprDoc()).raw();
    document->setModifiedSinceSave(false); DependencyRequest r;
    EXPECT_TRUE(r.confirm(1, t).ok()); EXPECT_FALSE(r.confirm(1, t).ok());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    lease->invalidate(); EXPECT_FALSE(r.confirm(2, t).ok());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
}
TEST_F(Dependencies, T11EpochRefusesWrongValueAndSurvivesLeaseDestruction) {
    open(); auto t = token(); auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
    ExpectedMutation e{MutationKind::Attribute}; e.node = identity(item("im")->getRepr());
    e.key = g_quark_from_string("data-eb4"); e.after = "expected";
    DependencyPublication p(t, *guard, {e}); ASSERT_NE(p.epoch().value, 0u);
    item("im")->getRepr()->setAttribute("data-eb4", "other");
    item("im")->getRepr()->setAttribute("data-eb4", "expected"); // change-and-revert cannot repair epoch
    EXPECT_FALSE(valid(t, p.epoch())); lease.reset(); EXPECT_FALSE(valid(t, p.epoch()));
    guard->rollback();
}
TEST_F(Dependencies, T11ThreeDistinctInvalidationsPauseUntilExplicitRetry) {
    open(); DependencyRequest r;
    for (unsigned n = 0; n < 3; ++n) {
        auto t = token(); ASSERT_TRUE(r.check(t, DependencyCheck::Dispatch).ok()); lease->invalidate();
        EXPECT_FALSE(r.check(t, DependencyCheck::Delivery).ok());
        EXPECT_FALSE(r.check(t, DependencyCheck::Admission).ok()); EXPECT_EQ(r.paused(), n == 2);
    }
    EXPECT_NE(std::string(r.check(token(), DependencyCheck::Dispatch).diagnostic).find("paused"), std::string::npos);
    r.retry(); EXPECT_TRUE(r.check(token(), DependencyCheck::Dispatch).ok());
    r.completed(); EXPECT_FALSE(r.paused());
}
TEST_F(Dependencies, T11InterleavedInvalidationsCountDistinctResults) {
    open(); DependencyRequest r; auto a = token(); lease->invalidate();
    EXPECT_FALSE(r.check(a, DependencyCheck::Delivery).ok());
    auto b = token(); EXPECT_TRUE(r.check(b, DependencyCheck::Dispatch).ok()); lease->invalidate();
    EXPECT_FALSE(r.check(b, DependencyCheck::Delivery).ok());
    EXPECT_FALSE(r.check(a, DependencyCheck::Admission).ok()); EXPECT_FALSE(r.paused());
    auto c = token(); lease->invalidate();
    EXPECT_FALSE(r.check(c, DependencyCheck::Delivery).ok()); EXPECT_TRUE(r.paused());
    r.retry(); EXPECT_TRUE(r.check(token(), DependencyCheck::Dispatch).ok());
}
TEST_F(Dependencies, T11CollectiveBitmapViewAndSourceDependencies) {
    for (bool group : {false, true}) for (auto name : {"im", "second"}) for (unsigned mutation = 0; mutation < 3; ++mutation) {
        auto second = bitmap(); second.replace(second.find("id='im'"), 7, "id='second'");
        auto body = bitmap() + second;
        open(group ? "<g id='g'>" + body + "</g>" : body);
        if (group) desktop->getSelection()->set(item("g"));
        else desktop->getSelection()->setList(std::vector<SPItem *>{item("im"), item("second")});
        auto t = token(); ASSERT_TRUE(valid(t));
        auto image = cast<SPImage>(item(name));
        if (mutation == 0) {
            // Issue, capture, then accept the same ticket: installation alone invalidates.
            auto ticket = image->composedViewGeneration(desktop->dkey); t = token();
            auto view = image->get_arenaitem(desktop->dkey); ASSERT_TRUE(view);
            ViewPixels pixels{image->pixbuf, image->pixbuf, SPItem::ensure_key(view)};
            ASSERT_TRUE(image->setComposedView(desktop->dkey, pixels, SuppressedOwnEffects::None, ticket));
        } else if (mutation == 1) ASSERT_NE(image->composedViewGeneration(desktop->dkey), 0u);
        else image->pixbuf.reset();
        for (auto point : {DependencyCheck::Dispatch, DependencyCheck::Delivery, DependencyCheck::Admission, DependencyCheck::Settlement})
            EXPECT_FALSE(DependencyRequest{}.check(t, point).ok());
    }
}
TEST_F(Dependencies, T11EpochRejectsForeignViewChangeOnWatchedBitmap) {
    open(); auto t = token(); auto before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
    {
        DependencyPublication p(t, *guard, {}); ASSERT_NE(p.epoch().value, 0u);
        auto image = cast<SPImage>(item("im"));
        image->setViewPixbuf(desktop->dkey, image->pixbuf);
        EXPECT_FALSE(DependencyRequest{}.check(t, DependencyCheck::Settlement, p.epoch()).ok());
    }
    guard->rollback(); EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
}
TEST_F(Dependencies, T11UnavailableViewRefusesCaptureAndInvalidatesExistingToken) {
    open(); auto t = token(); auto snapshot = resolve(*desktop, Intent::Explode).value;
    auto image = item("im"); image->invoke_hide(desktop->dkey);
    EXPECT_FALSE(valid(t)); EXPECT_FALSE(capture(snapshot));
    EXPECT_FALSE(DependencyRequest{}.confirm(1, t).ok());
    // Departure of a selected descendant is equally unavailable in collective mode.
    open("<g id='g'>" + bitmap() + "</g>"); desktop->getSelection()->set(item("g"));
    t = token(); snapshot = resolve(*desktop, Intent::Explode).value;
    item("im")->invoke_hide(desktop->dkey);
    EXPECT_FALSE(valid(t)); EXPECT_FALSE(capture(snapshot));
}
TEST_F(Dependencies, T11EpochStillChecksTargetGeneration) {
    open(); auto t = token(); auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
    DependencyPublication p(t, *guard, {}); ASSERT_NE(p.epoch().value, 0u);
    // Pending native flags make EB2 advance its generation. Even after clearing
    // the flags, an empty script must never accept that changed target snapshot.
    item("im")->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
    EXPECT_FALSE(valid(t, p.epoch()));
    document->ensureUpToDate(); EXPECT_FALSE(valid(t, p.epoch())); guard->rollback();
}
TEST_F(Dependencies, T28StaleInlineConfirmationAndSingleUseTicket) {
    open(); DependencyRequest r; auto t = token();
    item("im")->getRepr()->setAttribute("opacity", "0.8");
    auto denied = r.confirm(42, t); EXPECT_FALSE(denied.ok());
    EXPECT_NE(std::string(denied.diagnostic).find("Confirmation is stale"), std::string::npos);
    t = token(); EXPECT_FALSE(r.confirm(42, t).ok()); EXPECT_TRUE(r.confirm(43, t).ok());
    EXPECT_FALSE(r.confirm(43, t).ok()); EXPECT_FALSE(r.confirm(44, t).ok());
    EXPECT_FALSE(r.confirm(41, t).ok());
    r.retry(); EXPECT_FALSE(r.confirm(43, t).ok());
}
TEST_F(Dependencies, T11PublicationEpochExactWritesAndUnexpectedCallbackEdit) {
    for (bool unexpected : {false, true}) {
        open(); auto t = token(); auto before = sp_repr_save_buf(document->getReprDoc()).raw();
        auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
        ExpectedMutation e{MutationKind::Attribute}; e.node = identity(item("im")->getRepr());
        e.key = g_quark_from_string("data-eb4"); e.after = "owned";
        {
            DependencyPublication p(t, *guard, {e}); ASSERT_NE(p.epoch().value, 0u);
            EXPECT_FALSE(valid(t)); EXPECT_TRUE(valid(t, p.epoch())); // safe callback prefix
            EXPECT_FALSE(DependencyRequest{}.check(t, DependencyCheck::Settlement, p.epoch()).ok());
            item("im")->getRepr()->setAttribute("data-eb4", "owned");
            if (unexpected) document->getReprRoot()->setAttribute("data-callback", "unexpected");
            EXPECT_EQ(valid(t, p.epoch()), !unexpected);
            EXPECT_FALSE(valid(t, {p.epoch().lease, p.epoch().value + 1}));
            DependencyPublication nested(t, *guard, {}); EXPECT_EQ(nested.epoch().value, 0u);
        }
        guard->rollback(); EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before); EXPECT_FALSE(valid(t));
    }
}
struct EarlyInsertionEdit final : XML::NodeObserver {
    std::function<void()> edit = [] {};
    void notifyChildAdded(XML::Node &, XML::Node &, XML::Node *) override { edit(); }
};
struct RemovalRecorder final : XML::NodeObserver {
    std::vector<ExpectedMutation> &script;
    explicit RemovalRecorder(std::vector<ExpectedMutation> &s) : script(s) {}
    void notifyChildRemoved(XML::Node &n, XML::Node &c, XML::Node *p) override {
        ExpectedMutation e{MutationKind::Remove}; e.node = identity(&n);
        e.child = identity(&c); e.previous = identity(p); script.push_back(e);
    }
};
// Test-only XML oracle; native cascades are deliberately absent.
std::vector<ExpectedMutation> insertionScript(SPDocument &doc, std::vector<XML::Node *> const &nodes, SPDesktop *deleteFirst = nullptr, char const *deleteId = "im") {
    std::vector<ExpectedMutation> script;
    auto trial = DocumentUndo::beginAtomicInteraction(&doc);
    EXPECT_TRUE(trial); if (!trial) return {};
    if (deleteFirst) {
        RemovalRecorder removal(script); doc.getReprRoot()->addSubtreeObserver(removal);
        sigc::scoped_connection selection = deleteFirst->getSelection()->connectChanged([&](auto) {
            ExpectedMutation e{MutationKind::Selection}; e.node = identity(deleteFirst); script.push_back(e);
        });
        doc.getObjectById(deleteId)->deleteObject();
        doc.getReprRoot()->removeSubtreeObserver(removal);
    }
    for (auto node : nodes) {
        ExpectedMutation e{MutationKind::Add}; e.node = identity(doc.getReprRoot());
        e.child = identity(node); e.previous = identity(doc.getReprRoot()->lastChild()); script.push_back(e);
        doc.getReprRoot()->appendChild(node);
    }
    doc.ensureUpToDate();
    trial->rollback(); doc.ensureUpToDate(); return script;
}
TEST_F(Dependencies, T11ScriptedPiecesSettleAndForeignEventsRefuse) {
    // Positive N-piece publication; unscripted insertion, wrong order, foreign
    // XML between checkpoints and unaccounted EB2 generation each refuse.
    for (unsigned mode = 0; mode < 5; ++mode) for (unsigned count : {1u, 3u, 8u}) {
        open(); auto root = document->getReprRoot();
        auto staging = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'><g id='insert'><rect id='leaf' width='1' height='1' style='opacity:0.8'/></g></svg>");
        std::vector<XML::Node *> nodes;
        for (unsigned n = 0; n < count; ++n) {
            auto original = staging->getReprRoot()->lastChild(); auto suffix = std::to_string(n);
            original->setAttribute("id", "insert" + suffix); original->firstChild()->setAttribute("id", "leaf" + suffix);
            nodes.push_back(original->duplicate(document->getReprDoc()));
        }
        auto script = insertionScript(*document, nodes); ASSERT_EQ(script.size(), count);
        if (mode == 1) script.erase(script.begin());
        if (mode == 2) script.front().previous = identity(nodes.back());
        auto foreign = document->getReprDoc()->createElement("svg:g"); // stage BEFORE capture
        auto t = token(); auto snapshot = resolve(*desktop, Intent::Explode).value;
        auto baseline = lease->nativeWatchCount();
        auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
        {
            DependencyPublication p(t, *guard, script); ASSERT_NE(p.epoch().value, 0u);
            ASSERT_TRUE(valid(t, p.epoch()));
            for (auto node : nodes) root->appendChild(node);
            if (mode == 4) EXPECT_FALSE(valid(snapshot, *document)); // EB2 pending bump
            document->ensureUpToDate();
            EXPECT_EQ(lease->nativeWatchCount(), baseline);
            EXPECT_EQ(DependencyRequest{}.check(t, DependencyCheck::Settlement, p.epoch()).ok(), mode == 0 || mode == 3);
            if (mode == 3) {
                // A foreign insertion after a successful checkpoint remains stale.
                root->appendChild(foreign); document->ensureUpToDate();
            }
            for (auto point : {DependencyCheck::Dispatch, DependencyCheck::Delivery, DependencyCheck::Admission, DependencyCheck::Settlement})
                EXPECT_EQ(DependencyRequest{}.check(t, point, p.epoch()).ok(), mode == 0);
        }
        guard->rollback(); for (auto node : nodes) GC::release(node); GC::release(foreign);
        EXPECT_EQ(item("insert0"), nullptr);
    }
}
TEST_F(Dependencies, T11StagedSelectionIdentityOrderBindingsAndRollback) {
    for (unsigned mode = 0; mode < 8; ++mode) {
        SCOPED_TRACE(mode); open(); auto root = document->getReprRoot();
        auto staging = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'><rect id='piece0' width='1' height='1'/><rect id='piece1' width='1' height='1'/></svg>");
        std::vector<XML::Node *> nodes;
        for (auto name : {"piece0", "piece1"}) nodes.push_back(staging->getObjectById(name)->getRepr()->duplicate(document->getReprDoc()));
        auto script = insertionScript(*document, nodes);
        ExpectedMutation selection{MutationKind::Selection}; selection.node = identity(desktop.get());
        selection.stagedSelection = std::vector<std::uintptr_t>{identity(nodes[0]), identity(nodes[1])};
        if (mode == 6) (*selection.stagedSelection)[0] = identity(staging->getObjectById("piece0")->getRepr());
        if (mode == 7) selection.selection.push_back(identity(item("im"))); // ambiguous form refuses
        script.push_back(selection); auto t = token();
        auto before = sp_repr_save_buf(document->getReprDoc()).raw();
        auto selectedBefore = desktop->getSelection()->items_vector();
        auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
        {
            DependencyPublication p(t, *guard, script);
            if (mode >= 6) { EXPECT_EQ(p.epoch().value, 0u); }
            else {
                ASSERT_NE(p.epoch().value, 0u);
                for (auto node : nodes) root->appendChild(node);
                document->ensureUpToDate();
                std::vector<SPItem *> selected{item("piece0"), item("piece1")};
                if (mode == 1) selected[0] = item("im");
                if (mode == 2) std::swap(selected[0], selected[1]);
                if (mode == 3) selected.pop_back();
                if (mode == 4) { // foreign native binding, same exact XML identity
                    selected[0]->document = staging.get();
                }
                sigc::scoped_connection collision;
                if (mode == 5) collision = desktop->getSelection()->connectChanged([&](auto) {
                    nodes[0]->setAttribute("id", "piece1"); nodes[0]->setAttribute("id", "piece0");
                });
                desktop->getSelection()->setList(selected);
                if (mode == 4) selected[0]->document = document.get();
                for (auto point : {DependencyCheck::Dispatch, DependencyCheck::Delivery, DependencyCheck::Admission, DependencyCheck::Settlement})
                    EXPECT_EQ(DependencyRequest{}.check(t, point, p.epoch()).ok(), mode == 0);
            }
        }
        guard->rollback(); document->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
        EXPECT_EQ(item("piece0"), nullptr); EXPECT_EQ(item("piece1"), nullptr);
        // Publisher owns selection restoration; XML rollback releases pieces.
        desktop->getSelection()->setList(selectedBefore);
        EXPECT_EQ(desktop->getSelection()->singleItem(), item("im"));
        for (auto node : nodes) GC::release(node);
    }
}
std::string alphaPng(std::uint32_t &preparedPixel) {
    auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 1, 1);
    gdk_pixbuf_fill(gdk, 0x4070b05b); Pixbuf pixels(gdk);
    auto uri = *sp_image_encode_png_data_uri(pixels);
    pixels.ensurePixelFormat(Pixbuf::PF_CAIRO);
    std::memcpy(&preparedPixel, pixels.pixels(), 4); return uri;
}
TEST_F(Dependencies, T25OwnedHrefRetiresOnlyItsWatchAndKeepsXMLExact) {
    for (unsigned mode = 0; mode < 7; ++mode) {
        SCOPED_TRACE(mode); open(); auto image = cast<SPImage>(item("im"));
        auto node = image->getRepr(); std::string oldHref = image->href;
        std::uint32_t pixel; auto replacement = alphaPng(pixel);
        ExpectedMutation href{MutationKind::Attribute}; href.node = identity(node);
        href.key = g_quark_from_string("href"); href.before = oldHref; href.after = replacement;
        auto t = token(); auto before = sp_repr_save_buf(document->getReprDoc()).raw();
        document->setModifiedSinceSave(false);
        auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
        {
            DependencyPublication p(t, *guard, mode == 1 ? std::vector<ExpectedMutation>{} : std::vector{href});
            ASSERT_NE(p.epoch().value, 0u);
            if (mode == 2) { // still watched until the owned event
                image->setViewPixbuf(desktop->dkey, image->pixbuf);
                EXPECT_FALSE(valid(t, p.epoch()));
            }
            if (mode != 2) node->setAttribute("href", replacement);
            document->ensureUpToDate();
            if (mode == 3) node->setAttribute("opacity", "0.3");
            if (mode == 4) { node->setAttribute("href", oldHref); node->setAttribute("href", replacement); }
            if (mode == 5) image->emitModified(SP_OBJECT_STYLE_MODIFIED_FLAG);
            if (mode == 6) { // declared href retires the stamp; saved XML is authoritative
                image->setViewPixbuf(desktop->dkey, image->pixbuf);
            }
            document->ensureUpToDate();
            EXPECT_EQ(DependencyRequest{}.check(t, DependencyCheck::Settlement, p.epoch()).ok(),
                      mode == 0 || mode == 5 || mode == 6);
        }
        guard->rollback(); document->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
        EXPECT_EQ(desktop->getSelection()->singleItem(), image); EXPECT_FALSE(valid(t));
        EXPECT_FALSE(document->isModifiedSinceSave());
    }
}
TEST_F(Dependencies, T11EarlierObserverXMLMismatchAndNativeOnlyDisplayRisk) {
    // Earlier local observer edits before the EB4 subtree Add notification.
    for (unsigned mode = 0; mode < 4; ++mode) {
        open(); lease.reset(); auto root = document->getReprRoot();
        EarlyInsertionEdit earlier; root->addObserver(earlier);
        lease = std::make_unique<DependencyLease>(*desktop, policyEvidence());
        auto staging = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'><g id='insert'><rect id='leaf' width='1' height='1' style='opacity:0.8'/></g></svg>");
        auto node = staging->getObjectById("insert")->getRepr()->duplicate(document->getReprDoc());
        auto script = insertionScript(*document, {node}); auto t = token();
        auto staged = sp_repr_save_buf(document->getReprDoc()).raw(); bool edited = false;
        auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
        {
            DependencyPublication p(t, *guard, script); ASSERT_NE(p.epoch().value, 0u);
            earlier.edit = [&] {
                ASSERT_TRUE(item("leaf")); auto leaf = item("leaf"); edited = true;
                if (mode == 0) leaf->getRepr()->setAttribute("width", "2");
                else if (mode == 1) leaf->getRepr()->setAttribute("style", "opacity:0.25");
                else if (mode == 2) leaf->getRepr()->setCodeUnsafe(g_quark_from_string("svg:circle"));
                else { leaf->style->opacity.read("0.25"); leaf->emitModified(SP_OBJECT_STYLE_MODIFIED_FLAG); }
            };
            root->appendChild(node); ASSERT_TRUE(edited); document->ensureUpToDate();
            for (auto point : {DependencyCheck::Dispatch, DependencyCheck::Delivery, DependencyCheck::Admission, DependencyCheck::Settlement})
                EXPECT_EQ(DependencyRequest{}.check(t, point, p.epoch()).ok(), mode == 3);
            if (mode == 3) {
                EXPECT_STREQ(node->firstChild()->attribute("style"), "opacity:0.8");
                EXPECT_NE(sp_repr_save_buf(document->getReprDoc()).raw().find("opacity:0.8"), std::string::npos);
            }
            if (mode == 2) { // No counter event: staged XML comparison must latch the failure.
                node->firstChild()->setCodeUnsafe(g_quark_from_string("svg:rect"));
                EXPECT_FALSE(valid(t, p.epoch()));
            }
        }
        root->removeObserver(earlier); guard->rollback(); GC::release(node);
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), staged);
    }
}
TEST_F(Dependencies, T11InsertionChurnRetainsNoNewWatchesAndRemovalPrunes) {
    open(bitmap() + "<g id='remove'><rect id='oldleaf' width='1' height='1'/></g>");
    auto t = token(); auto baseline = lease->nativeWatchCount(); ASSERT_GT(baseline, 2u);
    auto root = document->getReprRoot(); std::unordered_set<SPObject *> addresses; unsigned reused = 0;
    for (unsigned n = 0; n < 256; ++n) {
        auto node = document->getReprDoc()->createElement("svg:g");
        root->appendChild(node); auto object = document->getObjectByRepr(node); ASSERT_TRUE(object);
        if (!addresses.insert(object).second) ++reused;
        EXPECT_EQ(lease->nativeWatchCount(), baseline); EXPECT_FALSE(valid(t));
        root->removeChild(node); GC::release(node);
        EXPECT_EQ(lease->nativeWatchCount(), baseline);
    }
    RecordProperty("reused_native_addresses", reused); // allocator-independent bound
    auto removed = item("remove")->getRepr(); GC::anchor(removed);
    root->removeChild(removed);
    EXPECT_EQ(lease->nativeWatchCount(), baseline - 2); // parent AND captured descendant
    // A detached, still-live former dependency has no retained modified callback.
    GC::release(removed); document->ensureUpToDate();
    auto fresh = token(); EXPECT_TRUE(valid(fresh)); EXPECT_EQ(lease->nativeWatchCount(), baseline - 2);
}
TEST_F(Dependencies, T11DocumentCounterRejectsDetachedAndEarlierCallbackChanges) {
    open(); auto t = token(); auto detached = document->getReprDoc()->createElement("svg:g");
    detached->setAttribute("data-hidden", "changed"); // outside every subtree observer
    EXPECT_FALSE(valid(t)); GC::release(detached);
    t = token(); EXPECT_TRUE(valid(t));
}
TEST_F(Dependencies, T11ScriptedSourceRemovalStillRefusesPendingNativeUpdates) {
    open(); auto t = token(); auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
    auto node = item("im")->getRepr(); auto root = node->parent();
    ExpectedMutation selection{MutationKind::Selection}; selection.node = identity(desktop.get());
    ExpectedMutation removal{MutationKind::Remove}; removal.node = identity(root);
    removal.child = identity(node); removal.previous = identity(node->prev());
    DependencyPublication p(t, *guard, {removal, selection}); ASSERT_NE(p.epoch().value, 0u);
    item("im")->deleteObject(); EXPECT_EQ(item("im"), nullptr);
    EXPECT_TRUE(valid(t, p.epoch())); // deletion alone can already be quiescent
    document->getRoot()->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG);
    ASSERT_NE(document->getRoot()->uflags | document->getRoot()->mflags, 0u);
    // Explicitly pending native work must still refuse; never bypass EB2 validation.
    EXPECT_FALSE(valid(t, p.epoch())); guard->rollback(); EXPECT_FALSE(valid(t, p.epoch()));
}
TEST_F(Dependencies, T11DeleteFirstScriptSettlesAndUnscriptedDeletionRefuses) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        SCOPED_TRACE(mode); open(); auto root = document->getReprRoot();
        auto staging = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'>" + bitmap() + "</svg>");
        auto piece = staging->getObjectById("im")->getRepr();
        piece->setAttribute("id", "piece"); piece->setAttribute("style", "opacity:0.8");
        auto node = piece->duplicate(document->getReprDoc());
        auto replacement = item("im")->getRepr()->duplicate(document->getReprDoc());
        auto script = insertionScript(*document, {node}, desktop.get());
        desktop->getSelection()->set(item("im")); auto t = token(); auto watches = lease->nativeWatchCount();
        auto removal = std::find_if(script.begin(), script.end(), [](auto const &e) { return e.kind == MutationKind::Remove; });
        ASSERT_NE(removal, script.end()); ASSERT_EQ(removal->child, identity(item("im")->getRepr()));
        if (mode == 1) script.erase(removal); // deletion is unscripted
        if (mode == 2) std::swap(script[0], script[1]); // wrong order
        auto before = sp_repr_save_buf(document->getReprDoc()).raw();
        SPWeakPtr<SPImage> source(cast<SPImage>(item("im")));
        auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
        {
            DependencyPublication p(t, *guard, script); ASSERT_NE(p.epoch().value, 0u);
            ASSERT_TRUE(valid(t, p.epoch())); item("im")->deleteObject();
            EXPECT_EQ(item("im"), nullptr); EXPECT_FALSE(source);
            root->appendChild(node); document->ensureUpToDate();
            ASSERT_TRUE(cast<SPImage>(item("piece"))); EXPECT_STREQ(node->attribute("style"), "opacity:0.8");
            EXPECT_EQ(lease->nativeWatchCount(), watches - 1);
            for (auto point : {DependencyCheck::Dispatch, DependencyCheck::Delivery, DependencyCheck::Admission, DependencyCheck::Settlement})
                EXPECT_EQ(DependencyRequest{}.check(t, point, p.epoch()).ok(), mode == 0 || mode == 3);
            EXPECT_FALSE(valid(t)); // reconciliation exists only inside this epoch
            if (mode == 3) {
                root->appendChild(replacement); document->ensureUpToDate();
                ASSERT_TRUE(item("im")); EXPECT_FALSE(valid(t, p.epoch()));
            }
        }
        guard->rollback(); document->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
        EXPECT_TRUE(item("im")); EXPECT_EQ(item("piece"), nullptr); EXPECT_FALSE(valid(t));
        GC::release(node); GC::release(replacement);
    }
}
TEST_F(Dependencies, T11SubtreeRemovalRetiresOnlyScriptedDescendantDependencies) {
    for (unsigned mode = 0; mode < 5; ++mode) {
        SCOPED_TRACE(mode);
        auto second = bitmap(); second.replace(second.find("id='im'"), 7, "id='im2'");
        open("<g id='g'>" + bitmap() + "<g id='nested'>" + second + "</g></g>"
             "<g id='unrelated'><rect id='leaf' width='1' height='1'/></g>");
        desktop->getSelection()->set(item("g"));
        auto removedId = mode < 2 ? "g" : "unrelated";
        auto script = insertionScript(*document, {}, desktop.get(), removedId);
        auto removal = std::find_if(script.begin(), script.end(), [](auto const &e) { return e.kind == MutationKind::Remove; });
        ASSERT_NE(removal, script.end());
        ASSERT_EQ(std::count_if(script.begin(), script.end(), [](auto const &e) { return e.kind == MutationKind::Remove; }), 1);
        if (mode == 1) script.erase(removal); // the ancestor removal is unscripted
        desktop->getSelection()->set(item("g")); auto t = token();
        auto watches = lease->nativeWatchCount();
        auto before = sp_repr_save_buf(document->getReprDoc()).raw();
        SPWeakPtr<SPImage> first(cast<SPImage>(item("im"))), other(cast<SPImage>(item("im2")));
        auto guard = DocumentUndo::beginAtomicInteraction(document.get()); ASSERT_TRUE(guard);
        {
            DependencyPublication p(t, *guard, script); ASSERT_NE(p.epoch().value, 0u);
            item(removedId)->deleteObject(); document->ensureUpToDate();
            EXPECT_EQ(bool(first), mode >= 2); EXPECT_EQ(bool(other), mode >= 2);
            EXPECT_EQ(lease->nativeWatchCount(), watches - (mode < 2 ? 4 : 2));
            for (auto point : {DependencyCheck::Dispatch, DependencyCheck::Delivery, DependencyCheck::Admission, DependencyCheck::Settlement})
                EXPECT_EQ(DependencyRequest{}.check(t, point, p.epoch()).ok(), mode != 1);
            if (mode == 2 || mode == 3) {
                // Each surviving bitmap's source/stamp must still be checked.
                cast<SPImage>(item(mode == 2 ? "im" : "im2"))->pixbuf.reset();
                EXPECT_FALSE(valid(t, p.epoch()));
            }
            if (mode == 4) {
                item("nested")->emitModified(SP_OBJECT_STYLE_MODIFIED_FLAG);
                EXPECT_TRUE(valid(t, p.epoch())); // native-only notification is diagnostic
            }
        }
        guard->rollback(); document->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
        EXPECT_TRUE(item("g")); EXPECT_TRUE(item("im")); EXPECT_TRUE(item("im2"));
        EXPECT_FALSE(valid(t));
    }
}
TEST_F(Dependencies, MainAffinityAndMissingObservationEvidence) {
    open(); auto t = token(); auto snapshot = resolve(*desktop, Intent::Explode).value;
    bool result = true; DependencyToken captured;
    std::thread worker([&] { result = valid(t); captured = capture(snapshot); }); worker.join();
    EXPECT_FALSE(result); EXPECT_FALSE(bool(captured));
    lease.reset(); lease = std::make_unique<DependencyLease>(*desktop, PlatformEvidence{});
    EXPECT_FALSE(capture(snapshot));
    lease.reset(); lease = std::make_unique<DependencyLease>(*desktop);
    EXPECT_EQ(bool(capture(snapshot)), measuredLimits(macDependencyEvidence()).observationUnits > 0);
}
// Publisher outcomes use actual encoded pieces, native documents and the Mac
// fixed platform defaults. Synthetic RAM avoids machine-pressure-dependent refusals.
class FixedPublish : public Dependencies {
protected:
    struct Room : MemoryProbe {
        std::uint64_t available = 8*1024*MiB;
        bool read(RawMemory &m) const noexcept override { m={16*1024*MiB,available,128*MiB}; return true; }
    } room;
    Budget budget{Budget::FixedLimitForTest{},1536*MiB};
    PreparedBitmapCopy candidate;
    FinalGrid grid;
    EncodedPieces pieces;
    Prepared prepared;
    std::string sourceUri(unsigned w,unsigned h) {
        auto gdk=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,w,h);
        EXPECT_TRUE(gdk); if (!gdk) return {};
        gdk_pixbuf_fill(gdk,0x4070b0c8); Pixbuf pixels(gdk);
        auto uri=sp_image_encode_png_data_uri(pixels); EXPECT_TRUE(uri); return uri ? *uri : "";
    }
    void seedHistory() {
        auto root=document->getReprRoot();
        root->setAttribute("data-boundary-history","kept");
        DocumentUndo::done(document.get(),Util::Internal::ContextString("Existing Undo"),"");
        root->setAttribute("data-boundary-history","redo");
        DocumentUndo::done(document.get(),Util::Internal::ContextString("Existing Redo"),"");
        ASSERT_TRUE(DocumentUndo::undo(document.get())); document->ensureUpToDate();
        document->setModifiedSinceSave(false);
    }
    void encodePieces(unsigned count) {
        JobWork work(std::uint64_t(grid.width)*grid.height); AlphaLut lut;
        for (unsigned i=0;i<256;++i) lut[i]=i;
        auto regions=label(grid.view(),lut,budget,work); ASSERT_TRUE(regions.ok());
        auto partition=enclose(regions.value,budget,work); ASSERT_TRUE(partition.ok());
        auto encoded=encode(grid,partition.value,budget); ASSERT_TRUE(encoded.ok()) << encoded.outcome.diagnostic;
        pieces=std::move(encoded.value); ASSERT_EQ(pieces.count(),count);
    }
    void prepare(TargetSnapshot const &target) {
        lease.reset(); lease=std::make_unique<DependencyLease>(*desktop,macExplodePromotionEvidence());
        prepared={}; prepared.target=target; prepared.dependencies=capture(target); ASSERT_TRUE(prepared.dependencies);
        prepared.grid=&grid; prepared.pieces=&pieces; prepared.budget=&budget; prepared.probe=&room;
        prepared.limits=measuredLimits(macExplodePromotionEvidence());
        prepared.activation=std::make_shared<DependencyRequest>();
    }
    void direct(unsigned count,unsigned sw=2,unsigned sh=2,unsigned gw=600,unsigned gh=4) {
        candidate={}; pieces={}; grid={};
        open("<image id='im' width='20' height='20' href='"+sourceUri(sw,sh)+"'/>");
        seedHistory(); ASSERT_FALSE(HasFatalFailure());
        grid.width=gw; grid.height=gh; grid.dpiX=grid.dpiY=300;
        grid.pixelToParent=grid.pixelToDocument={.32,0,0,.32,0,0};
        ASSERT_TRUE(grid.pixels.allocate(budget,Stage::prepared,std::uint64_t(gw)*gh,4).ok());
        std::memset(grid.pixels.data(),0,grid.pixels.size());
        auto data=reinterpret_cast<unsigned char *>(grid.pixels.data());
        for(unsigned i=0;i<count;++i) {
            auto offset = gw == 1 ? std::uint64_t(3*i) * gw : (gh == 1 ? 0 : gw) + 3*i;
            data[4*offset+3]=200;
        }
        encodePieces(count); ASSERT_FALSE(HasFatalFailure());
        auto target=resolve(*desktop,Intent::Explode); ASSERT_TRUE(target.ok()) << target.outcome.diagnostic;
        prepare(target.value); ASSERT_FALSE(HasFatalFailure());
        prepared.session=sessionJobIdentity(logicalImageIdentity(*cast<SPImage>(item("im"))),target.value);
    }
    void conversion(unsigned count,unsigned sw=0,unsigned sh=0,ExplodeSourceSize extent={2,2}) {
        candidate={}; pieces={}; grid={};
        std::string body;
        for(unsigned i=0;i<count;++i) {
            auto id=i==0 ? "im" : "part"+std::to_string(i);
            if(sw) {
                auto uri=sourceUri(sw,sh);
                body+="<image id='"+id+"' x='"+std::to_string(20*i)+"' width='10' height='10' preserveAspectRatio='none' href='"+uri+"'/>";
            } else body+="<rect id='"+id+"' x='"+std::to_string((extent.width+4)*i)+
                "' width='"+std::to_string(extent.width)+"' height='"+std::to_string(extent.height)+"' fill='#369'/>";
        }
        open(body);
        for(unsigned i=1;i<count;++i) desktop->getSelection()->add(item(("part"+std::to_string(i)).c_str()));
        seedHistory(); ASSERT_FALSE(HasFatalFailure());
        auto result=prepareBitmapCopy(*desktop->getSelection(),{},PlacementPolicy::ContiguousReplacement,budget,{&room});
        ASSERT_TRUE(result.ok()) << result.outcome.diagnostic; candidate=std::move(result.candidate);
        auto input=candidate.gridInput(budget); ASSERT_TRUE(input.ok()); Recipe recipe; recipe.bypassAlpha=true;
        auto resultGrid=prepareGrid(input.value,recipe,budget); ASSERT_TRUE(resultGrid.ok()); grid=std::move(resultGrid.value);
        encodePieces(count); ASSERT_FALSE(HasFatalFailure());
        prepare(candidate.metadata().token);
    }
    void outcome(bool converted,unsigned expectedPieces,bool admitted,char const *diagnostic=nullptr,Status refusal=Status::unavailable) {
        auto before=sp_repr_save_buf(document->getReprDoc()).raw();
        auto selected=desktop->getSelection()->items_vector();
        auto serial=document->get_event_log()->getCurrEventSerial();
        auto usage=preflightUndo(*document,{false,0,1}).usage;
        auto key=document->action_key(); auto dirty=document->isModifiedSinceSave();
        ASSERT_GT(usage.undoCount,0u); ASSERT_GT(usage.redoCount,0u);
        auto result=converted ? convertAndExplode(*desktop,{candidate,prepared},1) : publishExplode(*desktop,prepared,1);
        ASSERT_EQ(result.status,admitted ? Status::changed : refusal) << result.diagnostic;
        if(diagnostic) EXPECT_STREQ(result.diagnostic,diagnostic);
        auto after=preflightUndo(*document,{false,0,1}).usage;
        if(admitted) {
            EXPECT_EQ(desktop->getSelection()->size(),expectedPieces);
            for(auto item:desktop->getSelection()->items()) EXPECT_TRUE(is<SPImage>(item));
            unsigned images=0;
            for(auto &child:document->getRoot()->children) if(is<SPImage>(&child)) ++images;
            EXPECT_EQ(images,expectedPieces);
            EXPECT_EQ(after.undoCount,usage.undoCount+(converted ? 2 : 1)); EXPECT_EQ(after.redoCount,0u);
        } else {
            EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(),before);
            EXPECT_EQ(desktop->getSelection()->items_vector(),selected);
            EXPECT_EQ(document->get_event_log()->getCurrEventSerial(),serial);
            EXPECT_EQ(document->action_key(),key); EXPECT_EQ(document->isModifiedSinceSave(),dirty);
            EXPECT_EQ(after.undoCount,usage.undoCount); EXPECT_EQ(after.undoBytes,usage.undoBytes);
            EXPECT_EQ(after.redoCount,usage.redoCount); EXPECT_EQ(after.redoBytes,usage.redoBytes);
        }
    }
};
TEST_F(FixedPublish, Direct150Published151Refused) {
    for(unsigned n:{150u,151u}) { SCOPED_TRACE(n); direct(n); ASSERT_FALSE(HasFatalFailure()); outcome(false,n,n==150); }
}
TEST_F(FixedPublish, Conversion150Published151Refused) {
    for(unsigned n:{150u,151u}) { SCOPED_TRACE(n); conversion(n); ASSERT_FALSE(HasFatalFailure()); outcome(true,n,n==150); }
}
TEST_F(FixedPublish, DirectBothAxesAtProductionCeilingPublished) {
    direct(150,5000,5000,5000,5000); ASSERT_FALSE(HasFatalFailure()); outcome(false,150,true);
}
TEST_F(FixedPublish, FullDomainAdmittedWithRecoveryHeadroom) {
    direct(150,5000,5000,5000,5000); ASSERT_FALSE(HasFatalFailure());
    room.available=768*MiB; // J=512 MiB; formerly A/4=192 MiB refused publication.
    outcome(false,150,true);
}
TEST_F(FixedPublish, FullDomainRefusedWithGenuinelyLowMemory) {
    direct(150,5000,5000,5000,5000); ASSERT_FALSE(HasFatalFailure());
    room.available=300*MiB; // J=44 MiB; cannot hold even the retained 100,000,000-byte grid.
    outcome(false,150,false,"Not enough memory: OS commit/footprint headroom - recovery; estimated need 193.11 MiB, available 44.00 MiB.",Status::failed);
}
TEST_F(FixedPublish, OversizedOriginalWithSmallerFinalGridRefused) {
    for(auto size : {ExplodeSourceSize{5001,1}, ExplodeSourceSize{1,5001}}) {
        direct(2,size.width,size.height); ASSERT_FALSE(HasFatalFailure()); outcome(false,2,false,ExplodeSourceSizeMessage);
    }
}
TEST_F(FixedPublish, OversizedFinalGridWithSmallOriginalRefused) {
    for(auto size : {ExplodeSourceSize{5001,1}, ExplodeSourceSize{1,5001}}) {
        direct(2,2,2,size.width,size.height); ASSERT_FALSE(HasFatalFailure()); outcome(false,2,false,ExplodeSourceSizeMessage);
    }
}
TEST_F(FixedPublish, ConversionChecksEachOriginalAxisWithoutSummingPixels) {
    for(auto size : {ExplodeSourceSize{5000,5000}, ExplodeSourceSize{5001,1}, ExplodeSourceSize{1,5001}}) {
        conversion(2,size.width,size.height); ASSERT_FALSE(HasFatalFailure());
        outcome(true,2,size.width==5000,size.width==5000 ? nullptr : ExplodeSourceSizeMessage);
    }
}
TEST_F(FixedPublish, ConversionFinalGridAxisRefusesBeforeEitherCommit) {
    for(auto size : {ExplodeSourceSize{5001,1}, ExplodeSourceSize{1,5001}}) {
        conversion(2); ASSERT_FALSE(HasFatalFailure());
        pieces={}; grid.pixels={}; grid.width=size.width; grid.height=size.height;
        ASSERT_TRUE(grid.pixels.allocate(budget,Stage::prepared,std::uint64_t(grid.width)*grid.height,4).ok());
        std::memset(grid.pixels.data(),0,grid.pixels.size());
        auto data=reinterpret_cast<unsigned char *>(grid.pixels.data()); data[3]=data[4*3+3]=200;
        encodePieces(2); ASSERT_FALSE(HasFatalFailure());
        outcome(true,2,false,ExplodeSourceSizeMessage);
    }
}
TEST_F(FixedPublish, OversizedConversionRasterWithSmallerFinalGridRefusesBeforeEitherCommit) {
    for(auto extent : {ExplodeSourceSize{2000,2}, ExplodeSourceSize{2,2000}}) {
        SCOPED_TRACE(extent.width);
        conversion(2,0,0,extent); ASSERT_FALSE(HasFatalFailure());
        auto const &metadata=candidate.metadata();
        ASSERT_GT(extent.width==2000 ? metadata.width : metadata.height,MaxExplodeSourceAxis);
        ASSERT_LE(extent.width==2000 ? metadata.height : metadata.width,MaxExplodeSourceAxis);
        // Preserve candidate identity/recipe while preparing a smaller final grid.
        pieces={}; grid.pixels={}; grid.width=600; grid.height=4;
        ASSERT_TRUE(grid.pixels.allocate(budget,Stage::prepared,std::uint64_t(grid.width)*grid.height,4).ok());
        std::memset(grid.pixels.data(),0,grid.pixels.size());
        auto data=reinterpret_cast<unsigned char *>(grid.pixels.data()); data[3]=data[4*3+3]=200;
        encodePieces(2); ASSERT_FALSE(HasFatalFailure());
        ASSERT_EQ(grid.candidateIdentity,metadata.candidateIdentity);
        ASSERT_LE(grid.width,MaxExplodeSourceAxis); ASSERT_LE(grid.height,MaxExplodeSourceAxis);
        outcome(true,2,false,ExplodeSourceSizeMessage);
    }
}
TEST_F(FixedPublish, InjectedLargeUnitLimitsDoNotRaiseProductCap) {
    direct(151); ASSERT_FALSE(HasFatalFailure());
    prepared.limits.publicationUnits=prepared.limits.rollbackUnits=prepared.limits.historyUnits=20000;
    outcome(false,151,false);
}

JobClock::time_point logicalNow;
JobClock::time_point now() { return logicalNow; }
PlatformEvidence qualified() { return diagnosticDependencyEvidence(); }
TEST(DependencyLatency, FixedMacAndWindowsDefaultsAdmitFullDomainWithoutEvidence) {
    for(auto platform : {EvidencePlatform::Mac,EvidencePlatform::Windows}) {
        for(bool corrupt : {false,true}) {
            PlatformEvidence e{platform};
            if(corrupt) { e.observationNs=e.renderNs=e.publicationNs=e.closeNs=UINT64_MAX; e.promotion.repetitions=17; }
            auto l=measuredLimits(e);
            EXPECT_EQ(l.observationUnits,UnlimitedExplodeObservationUnits); EXPECT_EQ(l.clipUnits,2500u);
            EXPECT_EQ(l.publicationUnits,150u); EXPECT_EQ(l.rollbackUnits,150u); EXPECT_EQ(l.historyUnits,150u);
            EXPECT_EQ(l.sourceAxis,5000u); EXPECT_EQ(l.outlineUnits,2000000u); EXPECT_GE(l.renderUnits,25000000u);
            EXPECT_EQ(l.stopNs,10000000u); EXPECT_EQ(l.stopPixels,1000000u); EXPECT_EQ(l.stopBytes,65536u);
            EXPECT_TRUE(l.workerQualified); EXPECT_FALSE(l.extremeStressForTest);
            LatencyWork w{20000,2500,25000000,150,150,150,2000000,25000000,50000000,5000,5000};
            EXPECT_TRUE(admitLatency(w,l).ok());
            for(auto member : {&LatencyWork::publication,&LatencyWork::rollback,&LatencyWork::history}) {
                auto large=w; ++(large.*member); EXPECT_FALSE(admitLatency(large,l).ok());
            }
            auto large=w; ++large.width; EXPECT_FALSE(admitLatency(large,l).ok());
            large=w; ++large.height; EXPECT_FALSE(admitLatency(large,l).ok());
        }
    }
}
TEST(DependencyLatency, UnknownPlatformRefuses) {
    for(auto platform : {EvidencePlatform::Missing,static_cast<EvidencePlatform>(99)}) {
        auto l=measuredLimits(PlatformEvidence{platform});
        EXPECT_FALSE(l.workerQualified); EXPECT_EQ(l.sourceAxis,0u);
        EXPECT_FALSE(admitLatency({0,0,0,2,2,2,0,9,2,3,3},l).ok());
    }
}
TEST(DependencyLatency, Stress300RequiresExplicitFlagAndReservations) {
    auto limits=measuredLimits(PlatformEvidence{EvidencePlatform::Mac});
    LatencyWork work{0,0,25000000,300,300,300,0,25000000,50000000,5000,5000};
    EXPECT_FALSE(admitLatency(work,limits).ok());
    limits.extremeStressForTest=true;
    EXPECT_FALSE(admitLatency(work,limits).ok());
    limits.publicationUnits=limits.rollbackUnits=limits.historyUnits=300;
    EXPECT_TRUE(admitLatency(work,limits).ok());
    for (auto member : {&LatencyWork::publication,&LatencyWork::rollback,&LatencyWork::history}) {
        auto tooMany=work; ++(tooMany.*member);
        EXPECT_FALSE(admitLatency(tooMany,limits).ok());
    }
    limits.extremeStressForTest=false;
    EXPECT_FALSE(admitLatency(work,limits).ok());
}
TEST(DependencyLatency, CeilingCropRatioRemainsBounded) {
    auto limits=measuredLimits(PlatformEvidence{EvidencePlatform::Mac});
    LatencyWork work{0,0,25000000,150,150,150,0,25000000,50000000,5000,5000};
    EXPECT_TRUE(admitLatency(work,limits).ok());
    ++work.piecePixels;
    EXPECT_FALSE(admitLatency(work,limits).ok());
    work.sourcePixels=40000000; work.piecePixels=64000000;
    EXPECT_TRUE(admitLatency(work,limits).ok());
    ++work.piecePixels;
    EXPECT_FALSE(admitLatency(work,limits).ok());
}
TEST(DependencyLatency, FitSizeKeepsAspectAndRoundsDown) {
    for(auto values : {std::array<unsigned,4>{5000,5000,5000,5000}, {6000,4000,5000,3333},
                      {4000,6000,3333,5000}, {5001,2000,5000,1999}, {2000,5001,1999,5000}, {100,50,100,50}}) {
        auto size=fitExplodeSourceSize(values[0],values[1]);
        EXPECT_EQ(size.width,values[2]); EXPECT_EQ(size.height,values[3]);
    }
    EXPECT_EQ(explodeSourceSizeMessage(5001,1),
        "This image is 5001 × 1 px. Explode Bitmap works with images up to 5000 × 5000 px.");
}
TEST(DependencyLatency, FitThinSizeKeepsPositiveAxes) {
    for(auto values : {std::array<unsigned,4>{5001,1,5000,1}, {1,5001,1,5000},
                      {std::numeric_limits<unsigned>::max(),1,5000,1},
                      {1,std::numeric_limits<unsigned>::max(),1,5000}}) {
        auto size=fitExplodeSourceSize(values[0],values[1]);
        EXPECT_EQ(size.width,values[2]); EXPECT_EQ(size.height,values[3]);
    }
}
TEST(DependencyLatency, ResizeDpiMatchesExactExtentCeilAnd600Cap) {
    auto box=[](double w,double h) { return Geom::Rect(Geom::Point(0,0),Geom::Point(w,h)); };
    EXPECT_EQ(maxResizeDpi(box(480,480)),600);
    EXPECT_EQ(maxResizeDpi(box(100,50)),600);
    EXPECT_EQ(maxResizeDpi(box(1600,480)),300);
    EXPECT_EQ(maxResizeDpi(box(1600.001,480)),299);
    EXPECT_EQ(maxResizeDpi(box(480,1600.001)),299);
    EXPECT_EQ(maxResizeDpi(box(5000,1500)),96);
    EXPECT_EQ(maxResizeDpi(Geom::Rect(Geom::Point(.5,.5),Geom::Point(5000.5,1500.5))),96);
    EXPECT_EQ(maxResizeDpi(box(0,10)),0);
}
TEST(DependencyLatency, ResizeDpi95And96BoundaryIgnoresFractionalOrigin) {
    for (double origin : {0.2, -0.2}) for (bool transpose : {false, true}) {
        auto box = [&](double extent) {
            return Geom::Rect::from_xywh(origin, origin, transpose ? 10.2 : extent, transpose ? extent : 10.2);
        };
        EXPECT_EQ(maxResizeDpi(box(4999.9)), 96);
        EXPECT_EQ(maxResizeDpi(box(5000)), 96);
        EXPECT_EQ(maxResizeDpi(box(5000.001)), 95);
    }
}
TEST(DependencyLatency, ResizeDpiHandlesExtentsAndOriginsOutsideIntegerRange) {
    EXPECT_EQ(maxResizeDpi(Geom::Rect(Geom::Point(0,0),Geom::Point(1e12,1))),0);
    EXPECT_EQ(maxResizeDpi(Geom::Rect(Geom::Point(0,0),Geom::Point(1,1e12))),0);
    EXPECT_EQ(maxResizeDpi(Geom::Rect(Geom::Point(-1e12,0),Geom::Point(1e12,1))),0);
    EXPECT_EQ(maxResizeDpi(Geom::Rect(Geom::Point(1e12,-1e12),Geom::Point(1e12+5000,-1e12+1500))),96);
    EXPECT_EQ(maxResizeDpi(Geom::Rect(Geom::Point(1e12+.5,-1e12+.5),Geom::Point(1e12+5000.5,-1e12+1500.5))),96);
}
TEST(DependencyLatency, T19StopAllThreeBoundsInjectedClockAndOverflow) {
    auto e = qualified(); auto l = measuredLimits(e); EXPECT_TRUE(l.workerQualified);
    logicalNow = {}; StopCadence cadence(l, now);
    EXPECT_TRUE(cadence.advance(l.stopPixels, l.stopBytes).ok()); logicalNow += std::chrono::milliseconds(10);
    EXPECT_TRUE(cadence.advance(0, 0).ok()); logicalNow += std::chrono::nanoseconds(1);
    EXPECT_FALSE(cadence.advance(0, 0).ok()); cadence.checked();
    EXPECT_FALSE(cadence.advance(l.stopPixels + 1, 0).ok()); cadence.checked();
    EXPECT_FALSE(cadence.advance(0, l.stopBytes + 1).ok()); cadence.checked();
    EXPECT_FALSE(cadence.advance(UINT64_MAX, UINT64_MAX).ok()); cadence.checked();
    auto flag = std::make_shared<std::atomic<bool>>(true);
    EXPECT_EQ(cadence.advance(0, 0, Stop(flag)).status, Status::canceled);
    auto missing=measuredLimits(PlatformEvidence{});
    StopCadence unavailable(missing,now); EXPECT_FALSE(unavailable.advance(0,0).ok());
    logicalNow -= std::chrono::seconds(1); EXPECT_FALSE(cadence.advance(0, 0).ok());
}
} // namespace
