// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 VA Studio authors; released under GNU GPL v2+.
#include <gtest/gtest.h>
#include <future>
#include <atomic>
#include "ui/explode-bitmap-jobs.h"
#include <memory>
#include <cstring>
#include <cstdlib>
#include <gc/gc_mark.h>
#include "inkgc/gc-core.h"
#include "async/bitmap-job-reaper.h"
#include "desktop.h"
#include "display/cairo-utils.h"
#include "document.h"
#include "document-undo.h"
#include "event-log.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-image.h"
#include "selection.h"
#include "ui/explode-bitmap-session.h"
#include "ui/bitmap-preview-composer.h"
#include "object/sp-root.h"
#include "display/drawing.h"
#include "util/bitmap-islands.h"
#include "xml/document.h"
#include "xml/repr.h"
// Exercise actual C++ allocations as well as the engine's allocation-fault hook.
namespace { thread_local Inkscape::Bitmap::AllocationFault *allocationFault = nullptr; }
void *operator new(std::size_t size) {
    if (allocationFault && allocationFault->fail()) throw std::bad_alloc();
    if (auto p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
using namespace Inkscape;
using namespace Inkscape::Bitmap;
namespace {
std::atomic<bool> entered{false}, released{false};
JobClock::time_point fakeNow;
JobClock::time_point now() { return fakeNow; }
JobResult pausedJob(JobInput const &input, Stop stop, JobWork &, JobReporter &reporter) {
    reporter.progress({static_cast<Stage>(input.tag), 1, 2}); entered = true;
    auto deadline = JobClock::now() + std::chrono::seconds(2);
    while (!released && !stop.requested() && JobClock::now() < deadline) std::this_thread::yield();
    return {{Status::changed, "paused plain worker completed"}, {}, 0};
}
Recipe ts(unsigned t, unsigned s) { Recipe r; r.threshold = t; r.softness = s; return r; }
class SessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        static auto *app = [] {
            g_setenv("INKSCAPE_APP_ID_TAG", "bitmapsessiontest", TRUE);
            return new InkscapeApplication;
        }();
        ASSERT_TRUE(app->gtk_app());
        if (!Application::exists()) Application::create(false);
        recordBitmapMainThread();
        original = png(120); baked = png(91);
        doc = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' width='20' height='20'>" + image() + "</svg>");
        ASSERT_TRUE(doc); doc->ensureUpToDate();
        DocumentUndo::setUndoSensitive(doc.get(), true);
        desktop = std::make_unique<SPDesktop>(doc->getNamedView());
        desktop->getSelection()->set(im());
        doc->setModifiedSinceSave(false);
    }
    std::string png(unsigned alpha) {
        auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 2, 2);
        gdk_pixbuf_fill(gdk, 0x4070b000 | alpha);
        auto p = std::make_shared<Pixbuf>(gdk);
        auto uri = sp_image_encode_png_data_uri(*p);
        EXPECT_TRUE(uri); return uri ? *uri : "";
    }
    std::string image() { return "<image id='im' width='2' height='2' href='" + original + "'/>"; }
    SPImage *im(char const *id = "im") { return cast<SPImage>(doc->getObjectById(id)); }
    LogicalImageIdentity identity(char const *id = "im") { return logicalImageIdentity(*im(id)); }
    void done(char const *label) { doc->ensureUpToDate(); DocumentUndo::done(doc.get(), Util::Internal::ContextString{label}, ""); }
    void expect(LogicalImageIdentity id, unsigned t, unsigned s, bool bypass = false) {
        auto r = query(id); EXPECT_EQ(r.threshold, t); EXPECT_EQ(r.softness, s); EXPECT_EQ(r.bypassAlpha, bypass);
        EXPECT_EQ(r.coverage, nullptr); EXPECT_EQ(r.work, nullptr); EXPECT_EQ(r.fault, nullptr);
        EXPECT_EQ(r.checkpoint, nullptr); EXPECT_EQ(r.checkpointData, nullptr); EXPECT_FALSE(r.pixelated);
    }
    SessionJobIdentity job(LogicalImageIdentity id) {
        doc->ensureUpToDate(); auto target = resolve(*desktop, Intent::Explode);
        EXPECT_TRUE(target.ok()) << target.outcome.diagnostic;
        return sessionJobIdentity(id, target.value);
    }
    void apply(LogicalImageIdentity id) {
        im()->getRepr()->setAttribute("href", baked);
        auto op = prepareBaked(id, {id}); ASSERT_TRUE(op);
        done("Apply alpha adjustment"); EXPECT_TRUE(markBaked(op));
    }
    std::vector<LogicalImageIdentity> explode(LogicalImageIdentity source, AllocationFault *fault = nullptr, AllocationFault *commitFault = nullptr) {
        auto repr = im()->getRepr(); auto parent = repr->parent(); auto prev = repr->prev();
        parent->removeChild(repr);
        std::vector<LogicalImageIdentity> pieces;
        for (auto name : {"im", "piece2"}) {
            auto n = doc->getReprDoc()->createElement("svg:image");
            n->setAttribute("id", name); n->setAttribute("width", "2"); n->setAttribute("height", "2"); n->setAttribute("href", baked);
            parent->addChild(n, prev); prev = n; GC::release(n);
            pieces.push_back(identity(name));
        }
        desktop->getSelection()->set(im()); desktop->getSelection()->add(im("piece2"));
        auto op = prepareBaked(source, pieces, fault);
        if (!op) { DocumentUndo::cancel(doc.get()); return {}; }
        done("Explode bitmap"); allocationFault = commitFault;
        auto committed = markBaked(op); allocationFault = nullptr; EXPECT_TRUE(committed); return pieces;
    }
    __attribute__((noinline)) void discardBitmap() {
        desktop->getSelection()->clear();
        im()->getRepr()->setAttribute("data-large-payload", std::string(1024 * 1024, 'x'));
        im()->getRepr()->parent()->removeChild(im()->getRepr()); done("Delete large bitmap");
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get()); doc->ensureUpToDate();
    }
    std::string xml() { return sp_repr_save_buf(doc->getReprDoc()).raw(); }
    std::string original, baked;
    std::unique_ptr<SPDocument> doc;
    std::unique_ptr<SPDesktop> desktop;
};
TEST_F(SessionTest, T09IdReuseUndoRecreationAndQueuedStages) {
    auto a = identity(); remember(a, ts(211, 17)); auto old = job(a); ASSERT_TRUE(valid(old));
    auto repr = im()->getRepr(); auto parent = repr->parent();
    parent->removeChild(repr); done("Delete bitmap"); EXPECT_FALSE(valid(old));
    auto n = doc->getReprDoc()->createElement("svg:image");
    n->setAttribute("id", "im"); n->setAttribute("width", "2"); n->setAttribute("height", "2"); n->setAttribute("href", original);
    parent->appendChild(n); GC::release(n); done("Replacement");
    auto replacement = identity(); EXPECT_FALSE(replacement == a); expect(replacement, 128, 40);
    desktop->getSelection()->set(im()); auto replacementJob = job(replacement); ASSERT_TRUE(valid(replacementJob));
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_FALSE(valid(replacementJob));
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); ASSERT_TRUE(im()); EXPECT_TRUE(identity() == a); expect(a, 211, 17);
    desktop->getSelection()->set(im()); EXPECT_FALSE(valid(old)); ASSERT_TRUE(valid(job(a)));
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_TRUE(identity() == replacement); expect(replacement, 128, 40); EXPECT_FALSE(valid(old));
}
TEST_F(SessionTest, T10PanelReopenViewReplacementAndDocumentDeath) {
    auto a = identity(); remember(a, ts(19, 73)); auto pending = job(a);
    invalidateSessionJobs(a); EXPECT_FALSE(valid(pending)); expect(a, 19, 73);
    desktop->getSelection()->clear(); desktop->getSelection()->set(im()); expect(identity(), 19, 73);
    auto beforeClose = job(a); desktop.reset(); EXPECT_FALSE(valid(beforeClose));
    desktop = std::make_unique<SPDesktop>(doc->getNamedView()); desktop->getSelection()->set(im());
    expect(identity(), 19, 73); auto reopened = job(a); EXPECT_TRUE(valid(reopened));
    auto other = SPDocument::createNewDocFromMem(xml());
    auto otherId = logicalImageIdentity(*cast<SPImage>(other->getObjectById("im")));
    EXPECT_NE(a.document, otherId.document); expect(otherId, 128, 40);
    desktop->setDocument(other.get()); EXPECT_FALSE(valid(reopened)); expect(a, 19, 73);
    desktop.reset(); doc.reset(); EXPECT_FALSE(valid(reopened)); expect(a, 128, 40);
    remember(a, ts(250, 127)); expect(otherId, 128, 40);
}
TEST_F(SessionTest, T25ApplyUndoRedoAndExplicitEqualEdit) {
    auto a = identity(); remember(a, ts(128, 40)); auto originalXml = xml(); apply(a); auto bakedXml = xml();
    expect(a, 128, 40, true);
    auto pending = job(a); remember(a, ts(128, 40)); EXPECT_FALSE(valid(pending)); expect(a, 128, 40);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(), originalXml); expect(a, 128, 40);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_EQ(xml(), bakedXml); expect(a, 128, 40, true);
    // Exercise the real grid bypass against non-idempotent alpha, not a second formula.
    desktop->getSelection()->set(im()); auto target = resolve(*desktop, Intent::Explode); ASSERT_TRUE(target.ok());
    Budget b(Budget::FixedLimitForTest{}, 1024 * 1024); DecodedRaster raster; raster.width = raster.height = 2;
    ASSERT_TRUE(raster.pixels.allocate(b, Stage::decode, 4, 4).ok());
    unsigned char samples[16] = {64,112,176,91, 64,112,176,91, 64,112,176,91, 64,112,176,91};
    std::memcpy(raster.pixels.data(), samples, 16);
    auto bypass = prepareGrid(target.value, raster, query(a), b); ASSERT_TRUE(bypass.ok());
    EXPECT_EQ(std::memcmp(bypass.value.pixels.data(), samples, 16), 0);
    remember(a, ts(128, 40)); auto edited = prepareGrid(target.value, raster, query(a), b); ASSERT_TRUE(edited.ok());
    EXPECT_NE(std::memcmp(edited.value.pixels.data(), samples, 16), 0);
}
TEST_F(SessionTest, T25FreshPiecesAndRepeatedApplyExplodeHistory) {
    auto a = identity(); remember(a, ts(170, 50)); auto beforeApply = xml(); apply(a); auto afterApply = xml();
    remember(a, ts(33, 22)); auto pieces = explode(a); auto afterExplode = xml();
    expect(pieces[0], 33, 22, true); expect(pieces[1], 33, 22, true); EXPECT_FALSE(pieces[0] == a);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(), afterApply); EXPECT_TRUE(identity() == a); expect(a, 33, 22);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(), beforeApply); expect(a, 170, 50);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); expect(a, 170, 50, true);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_EQ(xml(), afterExplode);
    expect(identity(), 33, 22, true); expect(identity("piece2"), 33, 22, true);
    remember(pieces[1], ts(33, 22)); expect(pieces[1], 33, 22); expect(pieces[0], 33, 22, true);
    auto newSession = SPDocument::createNewDocFromMem(afterExplode);
    expect(logicalImageIdentity(*cast<SPImage>(newSession->getObjectById("im"))), 128, 40);
}
TEST_F(SessionTest, T30ConversionAdjacentAndUnrelatedHistory) {
    // Existing image stands in for EB2's published conversion; session never renders it.
    auto repr = im()->getRepr(); auto parent = repr->parent(); parent->removeChild(repr); done("Editable selection");
    parent->appendChild(repr); done("Convert selection to bitmap"); auto a = identity(); remember(a, ts(203, 9));
    doc->getReprRoot()->setAttribute("data-unrelated", "yes"); done("Unrelated edit");
    auto pieces = explode(a); ASSERT_TRUE(DocumentUndo::undo(doc.get())); expect(identity(), 203, 9);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); ASSERT_TRUE(im()); expect(a, 203, 9);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(im(), nullptr);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); expect(identity(), 203, 9);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); ASSERT_TRUE(DocumentUndo::redo(doc.get())); expect(pieces[0], 203, 9, true);
    // Removing only the unrelated entry establishes the adjacent two-step case.
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    explode(a); ASSERT_TRUE(DocumentUndo::undo(doc.get())); expect(identity(), 203, 9);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(im(), nullptr);
}
TEST_F(SessionTest, T30ToneApplyToneNoopAndOutputIsolation) {
    auto a = identity(); auto repr = im()->getRepr();
    repr->setAttribute("data-tone", "first"); done("Tone edit"); auto first = xml();
    remember(a, ts(83, 14)); apply(a); auto applied = xml();
    repr->setAttribute("data-tone", "second"); done("Tone edit");
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(), applied); expect(a, 83, 14, true);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(), first); expect(a, 83, 14);
    auto unchanged = xml(); auto serial = doc->get_event_log()->getCurrEventSerial(); doc->setModifiedSinceSave(false);
    for (unsigned t = 0; t < 256; ++t) { remember(a, ts(t, t % 128)); query(a); invalidateSessionJobs(a); }
    EXPECT_EQ(xml(), unchanged); EXPECT_FALSE(doc->isModifiedSinceSave()); EXPECT_EQ(doc->get_event_log()->getCurrEventSerial(), serial);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_EQ(xml(), applied); expect(a, 83, 14, true);
    auto reopened = SPDocument::createNewDocFromMem(xml()); expect(logicalImageIdentity(*cast<SPImage>(reopened->getObjectById("im"))), 128, 40);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(im()->getRepr()->attribute("data-tone"), nullptr);
}
TEST_F(SessionTest, T09StaleReceiptsRangeAndThreadRefusal) {
    auto a = identity(); remember(a, ts(55, 12)); im()->getRepr()->setAttribute("href", baked);
    auto op = prepareBaked(a, {a}); ASSERT_TRUE(op); done("Apply alpha adjustment"); markBaked(op); markBaked(op); expect(a, 55, 12, true);
    remember(a, ts(6, 1)); markBaked(op); expect(a, 6, 1);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); markBaked(op); expect(a, 55, 12);
    EXPECT_THROW(remember(a, ts(256, 1)), std::out_of_range); expect(a, 55, 12);
    EXPECT_THROW(remember(a, ts(1, 128)), std::out_of_range);
    EXPECT_TRUE(std::async(std::launch::async, [a] { try { query(a); } catch (std::logic_error const &) { return true; } return false; }).get());
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); expect(a, 55, 12, true);
    DocumentUndo::clearUndo(doc.get()); expect(a, 55, 12, true);
}
TEST_F(SessionTest, T10ComposerCloseReopenRetainsOnlySessionSettings) {
    auto a = identity(); remember(a, ts(90, 25)); auto before = xml();
    Drawing drawing; auto key = SPItem::display_key_new(1);
    drawing.setRoot(doc->getRoot()->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY));
    Contribution alpha; alpha.layer = PreviewLayer::Alpha; alpha.source = im()->pixbuf;
    auto pixels = std::make_shared<Pixbuf>(*im()->pixbuf); pixels->ensurePixelFormat(Pixbuf::PF_GDK);
    auto lut = alphaLut(90, 25);
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        auto p = pixels->pixels() + y * pixels->rowstride() + x * 4; p[3] = lut[p[3]];
    }
    auto budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 16);
    auto backing = reserveAlphaDisplay(2, 2, budget); ASSERT_TRUE(backing.ok());
    ASSERT_TRUE(prepareAlphaDisplay({pixels->pixels(), 16, 8, 2, 2}, *backing.value).ok());
    alpha.backing = backing.value; alpha.pixels = wrapAlphaDisplay(alpha.backing);
    auto client = contribute({im(), key}, alpha); ASSERT_TRUE(client);
    ASSERT_TRUE(update(client, 1).ok()); drawing.update();
    auto pending = job(a); invalidateSessionJobs(a); client.reset();
    EXPECT_FALSE(valid(pending)); expect(identity(), 90, 25); EXPECT_EQ(xml(), before);
    client = contribute({im(), key}, alpha); ASSERT_TRUE(update(client, 2).ok());
    expect(identity(), 90, 25); client.reset(); doc->getRoot()->invoke_hide(key);
    EXPECT_EQ(xml(), before); EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(SessionTest, T30RepeatedApplyAndDiscardedRedoReceipts) {
    auto a = identity(); remember(a, ts(180, 60)); auto firstOriginal = xml(); apply(a); auto firstBaked = xml();
    remember(a, ts(70, 8)); im()->getRepr()->setAttribute("href", png(190));
    auto second = prepareBaked(a, {a}); ASSERT_TRUE(second); done("Apply alpha adjustment"); markBaked(second); auto secondBaked = xml(); expect(a, 70, 8, true);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(), firstBaked); expect(a, 70, 8);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(), firstOriginal); expect(a, 180, 60);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); expect(a, 180, 60, true);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_EQ(xml(), secondBaked); expect(a, 70, 8, true);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->getReprRoot()->setAttribute("data-branch", "new"); done("New history branch");
    markBaked(second); expect(a, 70, 8); EXPECT_FALSE(DocumentUndo::redo(doc.get()));
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); expect(a, 70, 8);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); expect(a, 180, 60);
}
TEST_F(SessionTest, T09PendingXmlOrRecipeEditsRejectBakeReceipt) {
    auto a = identity(); remember(a, ts(95, 30));
    im()->getRepr()->setAttribute("href", baked);
    auto op = prepareBaked(a, {a}); ASSERT_TRUE(op); done("Apply alpha adjustment");
    remember(a, ts(96, 30)); EXPECT_FALSE(markBaked(op)); expect(a, 96, 30);
    im()->getRepr()->setAttribute("href", original);
    op = prepareBaked(a, {a}); ASSERT_TRUE(op); done("Apply alpha adjustment");
    im()->getRepr()->setAttribute("width", "3");
    EXPECT_FALSE(markBaked(op)); expect(a, 96, 30);
    DocumentUndo::cancel(doc.get());
    remember(a, ts(255, 127)); expect(a, 255, 127);
    remember(a, ts(0, 0)); expect(a, 0, 0);
}
TEST_F(SessionTest, Round2PrepareFaultRefusesPublicationAndCommitDoesNotAllocate) {
    static_assert(noexcept(markBaked(std::declval<OperationIdentity const &>())));
    auto a = identity(); remember(a, ts(171, 24));
    auto before = xml(); auto serial = doc->get_event_log()->getCurrEventSerial();
    std::vector<LogicalImageIdentity> results{a, a}; // duplicate must be folded
    for (unsigned k = 1; ; ++k) {
        im()->getRepr()->setAttribute("href", baked);
        AllocationFault fault{k}; auto op = prepareBaked(a, results, &fault);
        if (op) { DocumentUndo::cancel(doc.get()); break; }
        DocumentUndo::cancel(doc.get()); EXPECT_EQ(xml(), before); expect(a, 171, 24);
        EXPECT_EQ(doc->get_event_log()->getCurrEventSerial(), serial);
        ASSERT_LT(k, 32u);
    }
    for (unsigned k = 1; ; ++k) {
        im()->getRepr()->setAttribute("href", baked);
        AllocationFault fault{k}; allocationFault = &fault;
        auto op = prepareBaked(a, results); allocationFault = nullptr;
        if (op) {
            done("Apply allocation proof"); AllocationFault commitFault{1}; allocationFault = &commitFault;
            auto committed = markBaked(op); auto duplicate = markBaked(op); allocationFault = nullptr;
            EXPECT_TRUE(committed); EXPECT_FALSE(duplicate); EXPECT_EQ(commitFault.attempts, 0u); EXPECT_GT(k, 1u); break;
        }
        DocumentUndo::cancel(doc.get()); EXPECT_EQ(xml(), before); expect(a, 171, 24);
        EXPECT_EQ(doc->get_event_log()->getCurrEventSerial(), serial); ASSERT_LT(k, 32u);
    }
    expect(a, 171, 24, true); ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(), before);
    expect(a, 171, 24); ASSERT_TRUE(DocumentUndo::redo(doc.get())); expect(a, 171, 24, true);
}
TEST_F(SessionTest, Round2FreshPiecesPrepareAndCommitFaults) {
    auto a = identity(); remember(a, ts(62, 11)); auto before = xml();
    for (unsigned k = 1; ; ++k) {
        AllocationFault fault{k}, commitFault{1};
        auto pieces = explode(a, &fault, &commitFault);
        if (!pieces.empty()) {
            ASSERT_EQ(pieces.size(), 2u); EXPECT_EQ(commitFault.attempts, 0u);
            expect(pieces[0], 62, 11, true); expect(pieces[1], 62, 11, true); break;
        }
        EXPECT_EQ(xml(), before); expect(a, 62, 11); ASSERT_LT(k, 32u);
    }
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(), before); expect(a, 62, 11);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); expect(identity(), 62, 11, true); expect(identity("piece2"), 62, 11, true);
}
TEST_F(SessionTest, Round2DeletedPayloadCollectedWithLiveDocument) {
    auto a = identity(); remember(a, ts(239, 18)); discardBitmap();
    for (unsigned i = 0; i < 16; ++i) { GC_clear_stack(nullptr); GC::Core::gcollect(); }
    ASSERT_TRUE(doc); EXPECT_EQ(im(), nullptr); expect(a, 128, 40);
}
TEST_F(SessionTest, T09T10EveryWorkerStageRejectsLateDelivery) {
    auto a = identity(); remember(a, ts(104, 31));
    for (unsigned stage = 0; stage < static_cast<unsigned>(Stage::count); ++stage) {
        SCOPED_TRACE(stage); desktop->getSelection()->set(im()); auto token = job(a);
        entered = false; released = false; fakeNow = JobClock::time_point{};
        unsigned delivered = 0, accepted = 0;
        BitmapJobs jobs([&](Ticket, JobResult result) {
            EXPECT_TRUE(result.ok()); ++delivered; if (valid(token)) ++accepted;
        }, {}, now, false);
        JobInput input; input.work = pausedJob; input.pixels = 4; input.tag = stage;
        jobs.request(std::move(input)); fakeNow += std::chrono::milliseconds(250); jobs.poll();
        auto deadline = JobClock::now() + std::chrono::seconds(2);
        while (!entered && JobClock::now() < deadline) std::this_thread::yield();
        bool started = entered;
        if (stage % 3 == 0) { // genuine SPObject recreation, with identical id/payload
            auto repr = im()->getRepr(); repr->parent()->removeChild(repr); done("Delete while worker paused");
            EXPECT_FALSE(valid(token)); EXPECT_TRUE(DocumentUndo::undo(doc.get()));
            EXPECT_TRUE(identity() == a); expect(a, 104, 31);
        } else if (stage % 3 == 1) {
            invalidateSessionJobs(a); // panel/view detached; memory retained
        } else {
            desktop.reset(); // native view lifetime ends
            desktop = std::make_unique<SPDesktop>(doc->getNamedView());
        }
        released = true; deadline = JobClock::now() + std::chrono::seconds(2);
        while ((!delivered || jobs.active()) && JobClock::now() < deadline) {
            Glib::MainContext::get_default()->iteration(false); jobs.poll(); std::this_thread::yield();
        }
        EXPECT_TRUE(started); EXPECT_EQ(delivered, 1); EXPECT_EQ(accepted, 0);
        expect(a, 104, 31); jobs.close(); EXPECT_TRUE(waitForBitmapReaper(std::chrono::seconds(2)));
    }
}
} // namespace

TEST_F(SessionTest, RefineChoiceIsIndependentOfBakedBypassAndRestoredByHistory) {
    auto id = identity(); EXPECT_TRUE(query(id).refine);
    remember(id, ts(100, 20), false); EXPECT_FALSE(query(id).refine); EXPECT_FALSE(query(id).bypassAlpha);
    auto before = xml(); im()->getRepr()->setAttribute("href", baked.c_str());
    auto operation = prepareBaked(id, {id}); ASSERT_TRUE(operation); done("Apply alpha adjustment"); ASSERT_TRUE(markBaked(operation));
    EXPECT_FALSE(query(id).refine); EXPECT_TRUE(query(id).bypassAlpha);
    remember(id, ts(130, 40), true); EXPECT_TRUE(query(id).refine); EXPECT_FALSE(query(id).bypassAlpha);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(xml(), before); EXPECT_FALSE(query(id).refine); expect(id, 100, 20);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_FALSE(query(id).refine); expect(id, 100, 20, true);
    invalidateSessionJobs(id); EXPECT_FALSE(query(id).refine);
}

TEST_F(SessionTest, FaintFloorMemoryStalenessBakeAndUndoRedo)
{
    auto id = identity(); EXPECT_EQ(query(id).faintFloor, 5u);
    auto before = job(id); ASSERT_TRUE(valid(before));
    auto recipe = ts(111, 23); recipe.faintFloor = 17;
    remember(id, recipe, false);
    EXPECT_FALSE(valid(before)); EXPECT_EQ(query(id).faintFloor, 17u);
    EXPECT_EQ(query(id).threshold, 111u); EXPECT_EQ(query(id).softness, 23u); EXPECT_FALSE(query(id).refine);
    auto current = job(id); ASSERT_TRUE(valid(current));
    recipe.faintFloor = 0; remember(id, recipe, false); EXPECT_FALSE(valid(current));
    recipe.faintFloor = 25; remember(id, recipe, false); apply(id);
    EXPECT_EQ(query(id).faintFloor, 25u); EXPECT_TRUE(query(id).bypassAlpha);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(query(id).faintFloor, 25u); EXPECT_FALSE(query(id).bypassAlpha);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_EQ(query(id).faintFloor, 25u); EXPECT_TRUE(query(id).bypassAlpha);
    recipe.faintFloor = 26; EXPECT_THROW(remember(id, recipe), std::out_of_range);
    EXPECT_EQ(query(id).faintFloor, 25u);
}
