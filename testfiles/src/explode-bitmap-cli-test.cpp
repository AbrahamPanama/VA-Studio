// SPDX-License-Identifier: GPL-2.0-or-later
// Explode Bitmap command service: real immutable panel preparation and native document publication.
#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <thread>
#include <stdexcept>
#include "bitmap-explode-chemistry.h"
#include "desktop.h"
#include "display/cairo-utils.h"
#include "document.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-path.h"
#include "selection.h"
#include "ui/explode-bitmap-context.h"
#include "ui/explode-bitmap-panel-preparation.h"
#include "ui/explode-bitmap-undo.h"
#include "util/bitmap-input-header.h"
#include "xml/href-attribute-helper.h"
#include "xml/node.h"
#include "xml/repr.h"
using namespace Inkscape;
using namespace Inkscape::Bitmap;
namespace {
// Same separated-island RGB/alpha pattern as explode-bitmap-publish-test,
// encoded as the actual input rather than substituting a prepared synthetic grid.
std::string fixture(unsigned count = 3) {
    auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 120, 80);
    gdk_pixbuf_fill(gdk, 0);
    for (unsigned i = 0; i < count; ++i) for (unsigned y = 20; y < 40; ++y) for (unsigned x = 0; x < 20; ++x) {
        auto p = gdk_pixbuf_get_pixels(gdk) + y * gdk_pixbuf_get_rowstride(gdk) + (10 + 40*i + x)*4;
        p[0] = 64; p[1] = 90; p[2] = 120; p[3] = 150;
    }
    auto pixels = std::make_shared<Pixbuf>(gdk);
    auto uri = sp_image_encode_png_data_uri(*pixels);
    EXPECT_TRUE(uri);
    return "<svg xmlns='http://www.w3.org/2000/svg' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' width='200' height='120'>"
        "<g id='parent' transform='translate(4,8)'><rect id='before' width='2' height='3'/>"
        "<image id='im' x='-4' width='120' height='80' inkscape:label='Original' href='" + uri.value_or("") +
        "'><title>private title</title><desc>private description</desc></image>"
        "<rect id='after' width='4' height='5'/></g></svg>";
}
std::string xml(SPDocument &doc) { return sp_repr_save_buf(doc.getReprDoc()).raw(); }
std::string nodeXML(XML::Node *node) { return sp_repr_write_buf(node, 0, false, Glib::QueryQuark(0u), 0, 0).raw(); }
struct Probe final : MemoryProbe {
    bool read(RawMemory &m) const noexcept override { m = {16*1024*MiB, 8*1024*MiB, 128*MiB}; return true; }
};
struct PublicationFixture {
    std::unique_ptr<SPDocument> doc;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<DependencyLease> lease;
    std::unique_ptr<DocumentPublicationContext> context;
    std::shared_ptr<PanelPreparation::Output const> output;
    Prepared prepared;
    Probe probe;
    std::string before, imageBefore;
    SPImage *image() { return cast<SPImage>(doc->getObjectById("im")); }
    void open(bool gui, unsigned count = 3) {
        doc = SPDocument::createNewDocFromMem(fixture(count)); ASSERT_TRUE(doc); doc->ensureUpToDate();
        if (gui) {
            desktop = std::make_unique<SPDesktop>(doc->getNamedView()); desktop->getSelection()->set(image());
            doc->ensureUpToDate(); lease = std::make_unique<DependencyLease>(*desktop);
        } else {
            auto made = DocumentPublicationContext::headless(*doc, {"im"}, 17, 23);
            ASSERT_TRUE(made.ok()) << made.outcome.diagnostic; context = std::move(made.value);
        }
        before = xml(*doc); imageBefore = nodeXML(image()->getRepr());
    }
    void prepare(SessionRecipe recipe, bool contours = false, unsigned undoCount = 0) {
        auto target = context ? resolve(*context, Intent::Explode) : resolve(*desktop, Intent::Explode);
        ASSERT_TRUE(target.ok()) << target.outcome.diagnostic;
        prepared = {}; prepared.target = target.value; prepared.dependencies = capture(target.value);
        ASSERT_TRUE(prepared.dependencies);
        auto id = logicalImageIdentity(*image()); recipe = requestRecipe(id, recipe);
        prepared.session = sessionJobIdentity(id, target.value); prepared.requestRecipe = recipe;
        prepared.activation = context ? context->activation() : std::make_shared<DependencyRequest>();
        auto input = std::make_shared<PanelPreparation::Input>(); input->target = target.value;
        input->recipe = recipe; input->recipe.alphaPrepared = recipe.bypassAlpha;
        input->recipe.bypassAlpha = !recipe.refine || recipe.bypassAlpha;
        input->contour.enabled = contours; input->contour.gapToleranceMm = 0;
        auto href = getHrefAttribute(*image()->getRepr()).second; ASSERT_TRUE(href);
        auto uri = inspectUri(href, {}); ASSERT_TRUE(uri.ok()); input->decodedBytes = uri.value.decodedBytes;
        JobInput job; job.work = PanelPreparation::calculate; job.pixels = 9600;
        job.storage.budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 1536*MiB);
        job.storage.bytes.assign(href + uri.value.payloadOffset, href + uri.value.payloadOffset + uri.value.payloadLength);
        job.storage.payload = input;
        ASSERT_TRUE(job.storage.budget->acquire(Stage::input, job.storage.bytes.size()+sizeof(*input)+4096, job.storage.reservation).ok());
        recordBitmapMainThread(); bool delivered = false; JobResult result;
        BitmapJobs jobs([&](Ticket, JobResult r) { result = std::move(r); delivered = true; }, {}, JobClock::now, false);
        jobs.request(std::move(job)); auto deadline = JobClock::now() + std::chrono::seconds(10);
        while ((!delivered || jobs.active()) && JobClock::now() < deadline) {
            jobs.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        jobs.close(); ASSERT_TRUE(delivered); ASSERT_TRUE(result.ok()) << result.outcome.diagnostic;
        output = std::dynamic_pointer_cast<PanelPreparation::Output const>(result.value.payload); ASSERT_TRUE(output);
        ASSERT_TRUE(output->explodeOutcome.ok()) << output->explodeOutcome.diagnostic;
        prepared.grid = &output->grid; prepared.pieces = &output->pieces; prepared.budget = output->budget.get();
        prepared.probe = &probe; prepared.limits = measuredLimits(diagnosticDependencyEvidence());
        if (contours) {
            ASSERT_TRUE(output->contours.outcome.ok()) << output->contours.outcome.diagnostic;
            ASSERT_TRUE(output->contours.product); prepared.contours = &output->contours.product->fitted;
        }
        EXPECT_EQ(xml(*doc), before); EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, undoCount);
    }
    PublicationResult publish(unsigned intent, Ticket ticket = 1) {
        if (intent == 2) {
            if (!output->adjustment) return {{Status::failed, "Missing alpha payload"}};
            output->adjustment->publication = prepared;
            return context ? publishAlpha(*context, *output->adjustment, ticket) : PublicationResult(publishAlpha(*desktop, *output->adjustment, ticket));
        }
        if (intent == 1) return context ? publishContourOnly(*context, {prepared}, ticket) : PublicationResult(publishContourOnly(*desktop, {prepared}, ticket));
        return context ? publishExplode(*context, prepared, ticket) : PublicationResult(publishExplode(*desktop, prepared, ticket));
    }
    void history() {
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
        auto after = xml(*doc);
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate(); EXPECT_EQ(xml(*doc), before);
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 0u);
        ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate(); EXPECT_EQ(xml(*doc), after);
    }
};
class ExplodeBitmapCli : public ::testing::Test {
    void SetUp() override {
        static auto *app = [] { g_setenv("INKSCAPE_APP_ID_TAG", "seamp6", TRUE); return new InkscapeApplication; }();
        ASSERT_TRUE(app->gtk_app()); if (!Application::exists()) Application::create(false);
    }
};
SessionRecipe recipe() { SessionRecipe r; r.threshold = 128; r.softness = 40; r.faintFloor = 0; return r; }
TEST_F(ExplodeBitmapCli, HeadlessExplodeMatchesDesktopPiecesGeometryOrderAndHistory) {
    for (bool contours : {false, true}) {
        PublicationFixture headless, gui; headless.open(false); gui.open(true);
        headless.prepare(recipe(), contours); gui.prepare(recipe(), contours);
        ASSERT_EQ(headless.output->count, 3u); ASSERT_EQ(gui.output->count, 3u);
        auto a = headless.publish(0), b = gui.publish(0);
        ASSERT_EQ(a.status, Status::changed) << a.diagnostic; ASSERT_EQ(b.status, Status::changed) << b.diagnostic;
        ASSERT_EQ(a.selectionAfter.size(), 3u);
        auto selected = gui.desktop->getSelection()->items_vector(); ASSERT_EQ(selected.size(), 3u);
        for (unsigned i = 0; i < 3; ++i) {
            auto item = cast<SPItem>(headless.doc->getObjectById(a.selectionAfter[i])); ASSERT_TRUE(item);
            EXPECT_EQ(nodeXML(item->getRepr()), nodeXML(selected[i]->getRepr()));
            EXPECT_EQ(item->documentGeometricBounds(), selected[i]->documentGeometricBounds());
            EXPECT_EQ(is<SPGroup>(item), contours);
            EXPECT_EQ(item->getRepr()->prev()->attribute("id"), i ? a.selectionAfter[i-1] : "before");
        }
        headless.history(); gui.history();
    }
}
TEST_F(ExplodeBitmapCli, HeadlessContourOnlyPreservesExactBitmapAndMatchesDesktop) {
    PublicationFixture headless, gui; headless.open(false); gui.open(true);
    headless.prepare(recipe(), true); gui.prepare(recipe(), true);
    auto a = headless.publish(1), b = gui.publish(1);
    ASSERT_EQ(a.status, Status::changed) << a.diagnostic; ASSERT_EQ(b.status, Status::changed) << b.diagnostic;
    ASSERT_EQ(a.selectionAfter.size(), 1u);
    auto group = headless.doc->getObjectById(a.selectionAfter.front()); ASSERT_TRUE(is<SPGroup>(group));
    EXPECT_EQ(nodeXML(group->getRepr()), nodeXML(gui.desktop->getSelection()->singleItem()->getRepr()));
    EXPECT_EQ(cast<SPItem>(group)->documentGeometricBounds(), gui.desktop->getSelection()->singleItem()->documentGeometricBounds());
    EXPECT_EQ(nodeXML(group->getRepr()->firstChild()), headless.imageBefore);
    EXPECT_STREQ(group->getRepr()->prev()->attribute("id"), "before");
    EXPECT_STREQ(group->getRepr()->next()->attribute("id"), "after");
    ASSERT_TRUE(is<SPPath>(headless.doc->getObjectByRepr(group->getRepr()->firstChild()->next())));
    headless.history(); gui.history();
}
TEST_F(ExplodeBitmapCli, HeadlessAlphaMatchesDesktopRefinesOnceAndReplaysBakeLedger) {
    PublicationFixture headless, gui; headless.open(false); gui.open(true);
    auto r = recipe(); headless.prepare(r); gui.prepare(r);
    auto a = headless.publish(2), b = gui.publish(2);
    ASSERT_EQ(a.status, Status::changed) << a.diagnostic; ASSERT_EQ(b.status, Status::changed) << b.diagnostic;
    EXPECT_EQ(nodeXML(headless.image()->getRepr()), nodeXML(gui.image()->getRepr()));
    // smoothstep((150 - (128-40)) / 80) * 255 rounds to 222; a second pass gives 255.
    constexpr unsigned expected = 222;
    EXPECT_EQ(recipeAlphaLut(r)[expected], 255u);
    auto image = headless.image(); ASSERT_TRUE(image->pixbuf);
    // Decode the published PNG as straight RGBA. Drawing Pixbuf storage may be
    // premultiplied BGRA; its RGB bytes are not a lossless encoded-pixel oracle.
    auto href = getHrefAttribute(*image->getRepr()).second; ASSERT_TRUE(href);
    gsize length = 0;
    auto bytes = g_base64_decode(href + 22, &length);
    auto loader = std::unique_ptr<GdkPixbufLoader, void (*)(GdkPixbufLoader *)>(
        gdk_pixbuf_loader_new(), [](auto p) { g_object_unref(p); });
    auto loaded = gdk_pixbuf_loader_write(loader.get(), bytes, length, nullptr); g_free(bytes);
    ASSERT_TRUE(loaded); ASSERT_TRUE(gdk_pixbuf_loader_close(loader.get(), nullptr));
    auto decoded = gdk_pixbuf_loader_get_pixbuf(loader.get()); ASSERT_TRUE(decoded);
    ASSERT_EQ(gdk_pixbuf_get_width(decoded), 120); ASSERT_EQ(gdk_pixbuf_get_height(decoded), 80);
    ASSERT_EQ(gdk_pixbuf_get_n_channels(decoded), 4);
    for (unsigned y = 0; y < 80; ++y) for (unsigned x = 0; x < 120; ++x) {
        auto p = gdk_pixbuf_get_pixels(decoded) + y*gdk_pixbuf_get_rowstride(decoded) + x*4;
        bool island = y >= 20 && y < 40 && x % 40 >= 10 && x % 40 < 30;
        EXPECT_EQ(p[3], island ? expected : 0);
        if (island) { EXPECT_EQ(p[0], 64u); EXPECT_EQ(p[1], 90u); EXPECT_EQ(p[2], 120u); }
    }
    EXPECT_TRUE(query(logicalImageIdentity(*image)).bypassAlpha);
    headless.history(); gui.history();
    // A new generation after commit sees baked pixels without another refinement.
    headless.context.reset(); auto made = DocumentPublicationContext::headless(*headless.doc, {"im"}, 17, 24);
    ASSERT_TRUE(made.ok()); headless.context = std::move(made.value);
    headless.before = xml(*headless.doc);
    headless.prepare(r, false, 1);
    ASSERT_TRUE(headless.output); EXPECT_FALSE(headless.output->adjustment);
    for (unsigned i = 0; i < headless.output->proxy.size()/4; ++i) {
        auto alpha = std::to_integer<unsigned>(headless.output->proxy.data()[4*i+3]);
        EXPECT_TRUE(alpha == 0 || alpha == expected);
    }
    EXPECT_EQ(xml(*headless.doc), headless.before);
    auto snapshot = requestRecipe(logicalImageIdentity(*headless.image()), r);
    EXPECT_TRUE(snapshot.bypassAlpha);
}
TEST_F(ExplodeBitmapCli, StaleDocumentAndCancelRefuseWithoutMutationOrRemember) {
    for (bool cancel : {false, true}) {
        PublicationFixture run; run.open(false); auto id = logicalImageIdentity(*run.image()); auto saved = query(id);
        run.prepare(recipe());
        if (cancel) EXPECT_EQ(run.context->cancel().refusal, PublicationRefusal::canceled);
        else { run.doc->getObjectById("before")->getRepr()->setAttribute("width", "9"); run.doc->ensureUpToDate(); }
        auto before = xml(*run.doc); auto result = run.publish(0);
        EXPECT_FALSE(result.ok()); EXPECT_EQ(result.refusal, cancel ? PublicationRefusal::canceled : PublicationRefusal::staleTarget);
        EXPECT_EQ(xml(*run.doc), before); EXPECT_TRUE(result.selectionAfter.empty());
        auto now = query(id); EXPECT_EQ(now.threshold, saved.threshold); EXPECT_EQ(now.softness, saved.softness);
        EXPECT_EQ(now.faintFloor, saved.faintFloor); EXPECT_EQ(now.refine, saved.refine); EXPECT_EQ(now.bypassAlpha, saved.bypassAlpha);
        EXPECT_EQ(sessionJobIdentity(id, {}).recipeGeneration, run.prepared.session.recipeGeneration);
    }
}
TEST_F(ExplodeBitmapCli, ExplicitRecipeIgnoresOldPanelSettingsAndCommitsOnlyItsOwnBake) {
    PublicationFixture run; run.open(false); auto id = logicalImageIdentity(*run.image());
    remember(id, Recipe{250, 0, 25}, false); auto saved = query(id);
    run.prepare(recipe()); EXPECT_EQ(query(id).threshold, saved.threshold);
    auto result = run.publish(2); ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    EXPECT_EQ(query(id).threshold, 128u); EXPECT_TRUE(query(id).refine); EXPECT_TRUE(query(id).bypassAlpha);
    ASSERT_TRUE(DocumentUndo::undo(run.doc.get())); run.doc->ensureUpToDate();
    EXPECT_EQ(xml(*run.doc), run.before); EXPECT_EQ(query(id).threshold, saved.threshold); EXPECT_FALSE(query(id).refine);
}
TEST_F(ExplodeBitmapCli, ExplicitRefineOffIsNoOpEvenWithOldPanelRecipe) {
    PublicationFixture run; run.open(false); auto r = recipe(); r.refine = false;
    run.prepare(r);
    // An existing encoded alpha result must still honor the request-local off switch.
    PreparedAlpha alpha; std::thread worker([&] { auto result = prepareAlpha(run.output->grid, *run.output->budget); alpha = std::move(result.value); }); worker.join();
    alpha.publication = run.prepared;
    auto result = publishAlpha(*run.context, alpha, 1);
    EXPECT_EQ(result.status, Status::unchanged) << result.diagnostic;
    EXPECT_EQ(xml(*run.doc), run.before); EXPECT_FALSE(query(logicalImageIdentity(*run.image())).bypassAlpha);
}
TEST_F(ExplodeBitmapCli, ZeroPiecesIsNoOpAndOnePieceIsSupported) {
    for (unsigned count : {0u, 1u}) {
        PublicationFixture run; run.open(false, count); run.prepare(recipe()); ASSERT_EQ(run.output->count, count);
        auto result = run.publish(0); EXPECT_EQ(result.status, count ? Status::changed : Status::unchanged) << result.diagnostic;
        EXPECT_EQ(result.selectionAfter.size(), count);
        if (count) run.history();
        else { EXPECT_EQ(xml(*run.doc), run.before); EXPECT_EQ(preflightUndo(*run.doc, {false, 0, 1}).usage.undoCount, 0u); }
    }
}
TEST_F(ExplodeBitmapCli, ExplicitRootNeverReadsOrInstallsDesktopSelection) {
    PublicationFixture run; run.open(false);
    auto desktop = std::make_unique<SPDesktop>(run.doc->getNamedView());
    auto other = cast<SPItem>(run.doc->getObjectById("after")); desktop->getSelection()->set(other); run.doc->ensureUpToDate();
    run.before = xml(*run.doc); run.prepare(recipe());
    auto result = run.publish(0); ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    EXPECT_EQ(desktop->getSelection()->singleItem(), other); EXPECT_EQ(result.selectionAfter.size(), 3u);
}
TEST_F(ExplodeBitmapCli, ContextRefusesInvalidRootsWrongThreadAndRetiredGeneration) {
    auto doc = SPDocument::createNewDocFromMem(fixture()); ASSERT_TRUE(doc); doc->ensureUpToDate();
    auto before = xml(*doc);
    for (auto const &roots : std::vector<std::vector<std::string>>{{}, {"missing"}, {"parent"}, {"im", "after"}}) {
        auto refused = DocumentPublicationContext::headless(*doc, roots, 1, 1);
        EXPECT_FALSE(refused.ok()); EXPECT_EQ(refused.outcome.refusal, PublicationRefusal::invalidRoots);
        EXPECT_EQ(xml(*doc), before);
    }
    ContextResult<std::unique_ptr<DocumentPublicationContext>> offThread;
    std::thread worker([&] { offThread = DocumentPublicationContext::headless(*doc, {"im"}, 1, 1); }); worker.join();
    EXPECT_FALSE(offThread.ok()); EXPECT_EQ(offThread.outcome.refusal, PublicationRefusal::wrongThread);
    auto made = DocumentPublicationContext::headless(*doc, {"im"}, 1, 1); ASSERT_TRUE(made.ok());
    doc->getObjectById("before")->getRepr()->setAttribute("width", "9"); doc->ensureUpToDate();
    auto edited = xml(*doc);
    EXPECT_FALSE(resolve(*made.value, Intent::Explode).ok()); EXPECT_EQ(xml(*doc), edited);
}
TEST_F(ExplodeBitmapCli, DesktopContextKeepsNativeSelectionAndSettlement) {
    PublicationFixture run; run.open(true); run.lease.reset();
    auto made = DocumentPublicationContext::desktop(*run.desktop); ASSERT_TRUE(made.ok());
    run.context = std::move(made.value); run.prepare(recipe());
    auto result = run.publish(0); ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    EXPECT_EQ(result.selectionAfter.size(), 3u); EXPECT_EQ(run.desktop->getSelection()->size(), 3u);
    run.history();
}

TEST_F(ExplodeBitmapCli, NativeFailureRollsBackAllThreeHeadlessIntents) {
    for (unsigned intent : {0u, 1u, 2u}) {
        SCOPED_TRACE(intent);
        PublicationFixture run; run.open(false); run.prepare(recipe(), intent == 1);
        run.prepared.hooks = {[](PublishStage stage, unsigned, void *) {
            if (stage == PublishStage::Native) throw std::runtime_error("Injected native settlement failure");
        }, nullptr};
        auto result = run.publish(intent); EXPECT_EQ(result.status, Status::failed) << result.diagnostic;
        EXPECT_EQ(result.refusal, PublicationRefusal::nativePublication); EXPECT_TRUE(result.selectionAfter.empty());
        EXPECT_EQ(xml(*run.doc), run.before); EXPECT_EQ(preflightUndo(*run.doc, {false, 0, 1}).usage.undoCount, 0u);
        ASSERT_TRUE(run.image()); EXPECT_EQ(run.context->getSelection()->singleItem(), run.image());
        EXPECT_FALSE(query(logicalImageIdentity(*run.image())).bypassAlpha);
    }
}
TEST_F(ExplodeBitmapCli, NoOpTicketIsConsumedAndActivationOwnerCannotBeReplaced) {
    PublicationFixture run; run.open(false, 0); run.prepare(recipe());
    EXPECT_EQ(run.publish(0, 41).status, Status::unchanged);
    EXPECT_FALSE(run.publish(0, 41).ok());
    run.prepared.activation = std::make_shared<DependencyRequest>();
    EXPECT_EQ(run.publish(0, 42).refusal, PublicationRefusal::wrongActivation);
    EXPECT_EQ(xml(*run.doc), run.before); EXPECT_EQ(preflightUndo(*run.doc, {false, 0, 1}).usage.undoCount, 0u);
}

}

TEST_F(ExplodeBitmapCli, CollectiveCopyFactoryDoesNotWeakenStrictExplodeFactory) {
    auto doc = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg'><rect id='a' width='10' height='20'/><rect id='b' x='30' width='10' height='20'/></svg>");
    ASSERT_TRUE(doc); doc->ensureUpToDate(); auto before = xml(*doc);
    EXPECT_FALSE(DocumentPublicationContext::headless(*doc, {"a", "b"}, 1, 1).ok());
    auto copy = DocumentPublicationContext::headlessBitmapCopy(*doc, {"a", "b"}, 1, 1);
    ASSERT_TRUE(copy.ok()); EXPECT_EQ(copy.value->getSelection()->size(), 2u);
    EXPECT_NE(copy.value->drawingKey(), 0u); EXPECT_EQ(xml(*doc), before);
    auto identity = DocumentPublicationContext::headlessBitmapCopy(*doc, {"a"}, 0, 1);
    EXPECT_EQ(identity.outcome.refusal, PublicationRefusal::invalidIdentity);
    ASSERT_TRUE(identity.outcome.failure); EXPECT_EQ(identity.outcome.failure->reason, CliBitmapReason::InternalError);
    auto duplicate = DocumentPublicationContext::headlessBitmapCopy(*doc, {"a", "a"}, 1, 1);
    EXPECT_EQ(duplicate.outcome.refusal, PublicationRefusal::invalidRoots);
    EXPECT_EQ(xml(*doc), before);
}
TEST_F(ExplodeBitmapCli, PublicationMemoryReceiptSurvivesAllThreeIntentBoundaries) {
    struct LowMemory final : MemoryProbe {
        bool read(RawMemory &m) const noexcept override { m = {1024*MiB, MiB, 128*MiB}; return true; }
    } low;
    for (unsigned intent = 0; intent < 3; ++intent) {
        PublicationFixture f; f.open(false); f.prepare(recipe(), intent == 1);
        f.prepared.probe = &low;
        auto result = f.publish(intent);
        ASSERT_FALSE(result.ok()); ASSERT_TRUE(result.failure);
        EXPECT_EQ(result.failure->stage, CliBitmapStage::Publish);
        EXPECT_EQ(result.failure->reason, CliBitmapReason::MemoryAdmissionFailed);
        EXPECT_TRUE(result.selectionAfter.empty()); EXPECT_EQ(xml(*f.doc), f.before);
        EXPECT_EQ(preflightUndo(*f.doc, {false, 0, 1}).usage.undoCount, 0u);
    }
}
TEST_F(ExplodeBitmapCli, WorkerDeliveryRetainsTypedPreparationRefusal) {
    auto input = std::make_shared<PanelPreparation::Input>(); input->recipe.faintFloor = 26;
    JobInput job; job.work = PanelPreparation::calculate; job.storage.payload = input;
    job.storage.budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 16*MiB);
    recordBitmapMainThread(); bool delivered = false; JobResult result;
    BitmapJobs jobs([&](Ticket, JobResult r) { result = std::move(r); delivered = true; }, {}, JobClock::now, false);
    jobs.request(std::move(job)); auto deadline = JobClock::now() + std::chrono::seconds(10);
    while ((!delivered || jobs.active()) && JobClock::now() < deadline) {
        jobs.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    jobs.close(); ASSERT_TRUE(delivered); EXPECT_FALSE(result.ok()); ASSERT_TRUE(result.failure);
    EXPECT_EQ(result.failure->stage, CliBitmapStage::Analyze);
    EXPECT_EQ(result.failure->reason, CliBitmapReason::AnalysisFailed);
}

TEST_F(ExplodeBitmapCli, WorkerLaunchFailureKeepsExplicitContourStage) {
    recordBitmapMainThread();bool delivered=false;JobResult result;
    BitmapJobs jobs([&](Ticket,JobResult r) {result=std::move(r);delivered=true;},{},JobClock::now,false);
    JobInput job;job.stage=CliBitmapStage::Contour; // missing work is an actual worker admission branch
    jobs.request(std::move(job));auto deadline=JobClock::now()+std::chrono::seconds(10);
    while((!delivered || jobs.active()) && JobClock::now()<deadline) {jobs.poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    jobs.close();ASSERT_TRUE(delivered);ASSERT_TRUE(result.failure);
    EXPECT_EQ(result.failure->stage,CliBitmapStage::Contour);EXPECT_EQ(result.failure->reason,CliBitmapReason::InternalError);
}
