// SPDX-License-Identifier: GPL-2.0-or-later
// EB5-publish: atomic Explode and alpha-only Apply.
#include <gtest/gtest.h>
#include <memory>
#include <new>
#include <thread>
#include <lcms2.h>
#include <cstring>
#include <functional>
#include <unordered_set>
#include <stdexcept>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include "xml/node-observer.h"
#include "xml/document.h"
#include <sigc++/scoped_connection.h>
#include "bitmap-explode-chemistry.h"
#include "bitmap-adjustment-chemistry.h"
#include "desktop.h"
#include "display/cairo-utils.h"
#include "document.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-image.h"
#include "object/sp-item-group.h"
#include "object/sp-path.h"
#include <2geom/svg-path-parser.h>
#include <2geom/bezier-curve.h>
#include "colors/color.h"
#include <cmath>
#include "object/sp-filter.h"
#include "object/sp-root.h"
#include "selection.h"
#include "seltrans.h"
#include "style.h"
#include "ui/explode-bitmap-dependencies.h"
#include "ui/explode-bitmap-undo.h"
#include "ui/explode-bitmap-panel-preparation.h"
#include "ui/bitmap-adjustments-controller.h"
#include "ui/tools/select-tool.h"
#include "xml/node.h"
#include "xml/repr.h"
#include "xml/href-attribute-helper.h"
using namespace Inkscape;
using namespace Inkscape::Bitmap;

// Fail the actual sigc dispatch allocation, before any observer is entered.
// This is local to this test executable and armed only at the scope boundary.
namespace { thread_local bool failNextAllocation = false; }
void *operator new(std::size_t n) {
    if (std::exchange(failNextAllocation, false)) throw std::bad_alloc();
    if (auto p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void *p) noexcept { ::operator delete(p); }

namespace {
std::uintptr_t address(void const *p) { return reinterpret_cast<std::uintptr_t>(p); }
std::string alphaPng(unsigned alpha) {
    auto gdk = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 2, 2);
    gdk_pixbuf_fill(gdk, 0x4070b000 | alpha);
    auto pixbuf = std::make_shared<Pixbuf>(gdk);
    auto uri = sp_image_encode_png_data_uri(*pixbuf);
    EXPECT_TRUE(uri); return uri ? *uri : "";
}

struct Probe final : MemoryProbe {
    bool read(RawMemory &m) const noexcept override { m = {16 * 1024 * MiB, 8 * 1024 * MiB, 128 * MiB}; return true; }
};
class Publish : public ::testing::Test {
protected:
    void SetUp() override {
        static auto *app = [] { g_setenv("INKSCAPE_APP_ID_TAG", "eb5publish", TRUE); return new InkscapeApplication; }();
        ASSERT_TRUE(app->gtk_app()); if (!Application::exists()) Application::create(false);
    }
    void open(unsigned count = 2, bool nested = false, bool clip = false,
              std::string layout = {}, std::string const &attributes = {}, bool legacyHref = false) {
        lease.reset(); desktop.reset(); doc.reset();
        auto source = "<image id='im' width='20' height='20' inkscape:label='Original' opacity='0.8' x='-4' href='" + alphaPng(120) +
                      "'><title>private title</title><desc>private description</desc></image>";
        if (legacyHref) source.replace(source.find(" href="), 6, " xlink:href=");
        if (clip) source.insert(source.find(" width="), " clip-path='url(#clip)'");
        source.insert(source.find(" width="), attributes);
        std::string body = "<rect id='before' width='2' height='3'/>" + source + "<rect id='after' width='4' height='5'/>";
        if (clip) body = "<defs><clipPath id='clip' inkscape:collect='always'><rect width='20' height='20'/></clipPath></defs>" + body;
        if (nested) body = "<g id='outer' transform='translate(4,8)'><g id='parent' transform='scale(2)'>" + body + "</g></g>";
        if (!layout.empty()) { layout.replace(layout.find("@image@"), 7, source); body = std::move(layout); }
        doc = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' xmlns:xlink='http://www.w3.org/1999/xlink' width='80' height='80'>" + body + "</svg>");
        ASSERT_TRUE(doc); doc->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(doc->getNamedView());
        desktop->getSelection()->set(image()); doc->ensureUpToDate();
        std::memcpy(oldPixel.data(), image()->pixbuf->pixels(), oldPixel.size());
        original = image()->getRepr(); parent = original->parent(); previous = original->prev();
        pieces.reset(); grid = {};
        budget = std::make_unique<Budget>(Budget::FixedLimitForTest{}, 1536 * MiB);
        grid = {}; grid.width = std::max(4u, std::min(count, 200u) * 3); grid.height = std::max(4u, ((count + 199) / 200) * 3);
        grid.dpiX = grid.dpiY = 300;
        grid.pixelToParent = {0.1234567890123456, 0, 0, -0.9876543210987654, -14.123456789012345, 200.00000000000003};
        ASSERT_TRUE(grid.pixels.allocate(*budget, Stage::prepared, grid.width * grid.height, 4).ok());
        std::memset(grid.pixels.data(), 0, grid.pixels.size());
        auto bytes = reinterpret_cast<std::uint8_t *>(grid.pixels.data());
        for (unsigned i = 0; i < count; ++i) {
            auto p = bytes + ((i / 200) * 3 * grid.width + (i % 200) * 3) * 4; p[0] = 64; p[1] = 90; p[2] = 120; p[3] = 180;
        }
        JobWork work(grid.width * grid.height); AlphaLut lut;
        for (unsigned i = 0; i < 256; ++i) lut[i] = i;
        auto labelled = label(grid.view(), lut, *budget, work); ASSERT_TRUE(labelled.ok());
        auto partition = enclose(labelled.value, *budget, work); ASSERT_TRUE(partition.ok());
        if (count) {
            auto encoded = encode(grid, partition.value, *budget); ASSERT_TRUE(encoded.ok()) << encoded.outcome.diagnostic;
            pieces = std::move(encoded.value);
        }
        ASSERT_EQ(pieces.count(), count);
        PlatformEvidence e; e.platform = EvidencePlatform::Mac; e.observationNs = 1;
        lease = std::make_unique<DependencyLease>(*desktop, e);
        prepared = {};
        auto target = resolve(*desktop, Intent::Explode); ASSERT_TRUE(target.ok()) << target.outcome.diagnostic;
        prepared.target = target.value; prepared.dependencies = capture(target.value); ASSERT_TRUE(prepared.dependencies);
        sourceIdentity = logicalImageIdentity(*image());
        prepared.session = sessionJobIdentity(sourceIdentity, target.value);
        prepared.grid = &grid; prepared.pieces = &pieces; prepared.budget = budget.get(); prepared.probe = &probe;
        prepared.limits.publicationUnits = prepared.limits.rollbackUnits = prepared.limits.historyUnits = 20000;
        prepared.activation = std::make_shared<DependencyRequest>();
        before = sp_repr_save_buf(doc->getReprDoc()).raw(); doc->setModifiedSinceSave(false);
    }
    FittedContourSet fitted(bool empty = false) {
        JobWork work(25000000);
        auto labelled = label(grid.view(), alphaLut(0, 0), *budget, work);
        EXPECT_TRUE(labelled.ok());
        auto partition = enclose(labelled.value, *budget, work); EXPECT_TRUE(partition.ok());
        // A valid noContour result can arise when offset/alpha eliminates all support.
        if (empty) std::memset(grid.pixels.data(), 0, grid.pixels.size());
        auto traced = traceContours(grid.view(), partition.value, *budget, work); EXPECT_TRUE(traced.ok());
        auto fitted = fitContours(traced.value, {50}, *budget, work); EXPECT_TRUE(fitted.ok()) << fitted.outcome.diagnostic;
        auto const &m = grid.pixelToParent;
        auto toDocument = Geom::Affine(m[0], m[1], m[2], m[3], m[4], m[5]) *
                          cast<SPItem>(doc->getObjectByRepr(parent))->i2doc_affine();
        for (unsigned i = 0; i < 6; ++i) grid.pixelToDocument[i] = toDocument[i];
        return std::move(fitted.value);
    }
    PreparedAlpha alphaResult(unsigned alpha) {
        FinalGrid refined; refined.width = refined.height = 2; refined.dpiX = refined.dpiY = 300;
        EXPECT_TRUE(refined.pixels.allocate(*budget, Stage::prepared, 4, 4).ok());
        auto pixels = reinterpret_cast<std::uint8_t *>(refined.pixels.data());
        for (unsigned i = 0; i < 4; ++i) {
            pixels[4*i] = 64; pixels[4*i+1] = 112; pixels[4*i+2] = 176; pixels[4*i+3] = alpha;
        }
        Result<PreparedAlpha> result;
        std::thread worker([&] { result = prepareAlpha(refined, *budget); }); worker.join();
        EXPECT_TRUE(result.ok()) << result.outcome.diagnostic; result.value.publication = prepared; return std::move(result.value);
    }
    SPImage *image() { return cast<SPImage>(doc->getObjectById("im")); }
    void recapture(bool select = true) {
        if (select) desktop->getSelection()->set(image());
        doc->ensureUpToDate();
        auto target = resolve(*desktop, Intent::Explode); ASSERT_TRUE(target.ok());
        prepared.target = target.value; prepared.dependencies = capture(target.value); ASSERT_TRUE(prepared.dependencies);
        prepared.session = sessionJobIdentity(sourceIdentity, target.value);
        before = sp_repr_save_buf(doc->getReprDoc()).raw(); doc->setModifiedSinceSave(false);
    }
    void unchanged(std::uint64_t redo = 0) {
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        ASSERT_TRUE(image()); EXPECT_EQ(image()->getRepr(), original);
        EXPECT_EQ(original->parent(), parent); EXPECT_EQ(original->prev(), previous);
        EXPECT_EQ(desktop->getSelection()->singleItem(), image());
        ASSERT_TRUE(image()->pixbuf); EXPECT_EQ(image()->pixbuf->width(), 2); EXPECT_EQ(image()->pixbuf->height(), 2);
        EXPECT_EQ(std::memcmp(image()->pixbuf->pixels(), oldPixel.data(), oldPixel.size()), 0);
        EXPECT_EQ(image()->width.computed, 20); EXPECT_EQ(image()->x.computed, -4);
        EXPECT_NE(image()->get_arenaitem(desktop->dkey), nullptr);
        EXPECT_FALSE(doc->isModifiedSinceSave());
        auto usage = preflightUndo(*doc, {false, 0, 1}).usage;
        EXPECT_EQ(usage.undoCount, 0u); EXPECT_EQ(usage.redoCount, redo);
        EXPECT_EQ(doc->getObjectById("im-piece-2"), nullptr);
    }
    void redoBranch() {
        doc->getObjectById("after")->getRepr()->setAttribute("x", "42");
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Earlier"), "");
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate(); recapture();
    }
    void handlesMatch(Geom::OptRect const &expected) {
        auto tool = dynamic_cast<UI::Tools::SelectTool *>(desktop->getTool());
        ASSERT_TRUE(tool); ASSERT_TRUE(tool->_seltrans);
        auto &transform = *tool->_seltrans;
        EXPECT_EQ(transform.isEmpty(), !expected);
        unsigned visible = 0;
        Geom::OptRect handleBounds;
        for (unsigned i = 0; i < NUMHANDS; ++i) {
            auto knot = transform.handleKnot(i);
            ASSERT_TRUE(knot);
            if (!knot->is_visible()) continue;
            ++visible;
            EXPECT_TRUE(hands[i].type == HANDLE_SCALE || hands[i].type == HANDLE_STRETCH);
            if (handleBounds) handleBounds->expandTo(knot->pos);
            else handleBounds = Geom::Rect(knot->pos, knot->pos);
        }
        EXPECT_EQ(visible, expected ? 8u : 0u);
        ASSERT_EQ(bool(handleBounds), bool(expected));
        if (expected) {
            for (unsigned axis = 0; axis < 2; ++axis) {
                EXPECT_NEAR(handleBounds->min()[axis], expected->min()[axis], 1e-8);
                EXPECT_NEAR(handleBounds->max()[axis], expected->max()[axis], 1e-8);
            }
        }
    }
    // No oracle mutates the publication input to learn a production script.
    std::unique_ptr<SPDocument> doc;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<DependencyLease> lease;
    // Buffers die before their ledger, including repeated open().
    std::unique_ptr<Budget> budget;
    FinalGrid grid;
    EncodedPieces pieces;
    Probe probe;
    Prepared prepared;
    LogicalImageIdentity sourceIdentity;
    XML::Node *original = nullptr, *parent = nullptr, *previous = nullptr;
    std::string before;
    std::array<unsigned char, 4> oldPixel;
};

// Opt-in timing harness: real publication/history and a core-only image control.
// Run with --gtest_also_run_disabled_tests --gtest_filter=Publish.DISABLED_Bug017UndoScaling.
TEST_F(Publish, DISABLED_Bug017UndoScaling) {
    using Clock = std::chrono::steady_clock;
    auto measure = [&](SPDocument &document, SPDesktop &view, std::string const &initial,
                       unsigned n, char const *kind, double publicationMs) {
        auto selection = view.getSelection();
        ASSERT_EQ(selection->size(), n);
        auto const published = sp_repr_save_buf(document.getReprDoc()).raw();
        if (g_getenv("BUG017_DESELECT")) selection->clear();
        unsigned changed = 0;
        unsigned displayChanged = 0;
        sigc::scoped_connection connection = selection->connectChanged([&](Selection *) { ++changed; });
        sigc::scoped_connection displayConnection = selection->connectChangedForDisplay([&](Selection *) { ++displayChanged; });
        std::printf("BUG017 begin_undo kind=%s n=%u\n", kind, n); std::fflush(stdout);
        auto start = Clock::now();
        ASSERT_TRUE(DocumentUndo::undo(&document)); document.ensureUpToDate();
        auto undoMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        auto undoChanged = changed;
        EXPECT_EQ(sp_repr_save_buf(document.getReprDoc()).raw(), initial);
        EXPECT_TRUE(selection->isEmpty());
        start = Clock::now();
        ASSERT_TRUE(DocumentUndo::redo(&document)); document.ensureUpToDate();
        auto redoMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        EXPECT_EQ(sp_repr_save_buf(document.getReprDoc()).raw(), published);
        std::printf("BUG017 kind=%s n=%u selected=%d publication_ms=%.3f undo_ms=%.3f redo_ms=%.3f undo_changed=%u display_changed=%u\n",
                    kind, n, !g_getenv("BUG017_DESELECT"), publicationMs, undoMs, redoMs, undoChanged, displayChanged);
        std::fflush(stdout);
    };
    for (auto n : {100u, 200u, 1000u, 5000u}) {
        if (n == 5000 && !g_getenv("BUG017_N")) continue;
        if (auto only = g_getenv("BUG017_N"); only && n != std::strtoul(only, nullptr, 10)) continue;
        open(n);
        auto start = Clock::now();
        auto result = publishExplode(*desktop, prepared, 1);
        ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
        auto ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        measure(*doc, *desktop, before, n, "explode", ms);

        // Fail after the pieces have been selected, then time native recovery
        // from the existing Rollback checkpoint through publication return.
        open(n);
        struct Recovery { Clock::time_point start; bool entered = false; } recovery;
        prepared.hooks = {[](PublishStage stage, unsigned, void *data) {
            auto &r = *static_cast<Recovery *>(data);
            if (stage == PublishStage::Selection) throw std::runtime_error("BUG017 rollback");
            if (stage == PublishStage::Rollback) { r.start = Clock::now(); r.entered = true; }
        }, &recovery};
        result = publishExplode(*desktop, prepared, 1);
        auto rollbackMs = std::chrono::duration<double, std::milli>(Clock::now() - recovery.start).count();
        ASSERT_TRUE(recovery.entered);
        ASSERT_EQ(result.status, Status::failed) << result.diagnostic;
        unchanged();
        std::printf("BUG017 kind=explode n=%u rollback_ms=%.3f\n", n, rollbackMs);
        std::fflush(stdout);
        lease.reset(); desktop.reset(); doc.reset();

        // No Explode dependency lease, session, panel, overlay or publisher.
        auto control = SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' width='80' height='80'><rect id='before' width='2' height='3'/><rect id='after' width='4' height='5'/></svg>");
        ASSERT_TRUE(control); control->ensureUpToDate();
        auto view = std::make_unique<SPDesktop>(control->getNamedView());
        auto initial = sp_repr_save_buf(control->getReprDoc()).raw();
        auto uri = alphaPng(180);
        std::vector<SPItem *> selected;
        auto previous = control->getObjectById("before")->getRepr();
        start = Clock::now();
        for (unsigned i = 0; i < n; ++i) {
            auto repr = control->getReprDoc()->createElement("svg:image");
            repr->setAttribute("width", "1"); repr->setAttribute("height", "1");
            repr->setAttribute("xlink:href", uri);
            control->getRoot()->getRepr()->addChild(repr, previous);
            previous = repr;
            selected.push_back(cast<SPItem>(control->getObjectByRepr(repr)));
            GC::release(repr);
        }
        view->getSelection()->setList(selected);
        DocumentUndo::done(control.get(), Util::Internal::ContextString("BUG017 image siblings"), "");
        control->ensureUpToDate();
        ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        measure(*control, *view, initial, n, "plain", ms);
    }
}
TEST_F(Publish, Bug017HistoryRemovalBatchesDisplayAndPreservesImmediateObservers) {
    constexpr unsigned n = 150;
    for (bool nested : {false, true}) {
        open(n, nested);
        ASSERT_EQ(publishExplode(*desktop, prepared, 1).status, Status::changed);
        auto published = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto selection = desktop->getSelection();
        auto survivor = cast<SPItem>(doc->getObjectById("before"));
        selection->add(survivor);
        unsigned changed = 0, displayChanged = 0, remaining = 0;
        sigc::scoped_connection immediate = selection->connectChanged([&](Selection *s) {
            ++changed;
            remaining += s->size();
            EXPECT_TRUE(s->includes(survivor));
            // Released objects must disappear immediately, even inside replay.
            for (auto item : s->items()) EXPECT_EQ(doc->getObjectByRepr(item->getRepr()), item);
        });
        sigc::scoped_connection display = selection->connectChangedForDisplay([&](Selection *s) {
            ++displayChanged;
            EXPECT_EQ(s->singleItem(), survivor);
        });
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_EQ(selection->singleItem(), survivor);
        // BUG-026c: the members the replay releases leave the selection in ONE
        // immediate change before the replay (formerly one change per object).
        EXPECT_EQ(changed, 1u);
        EXPECT_EQ(remaining, 1u);
        EXPECT_EQ(displayChanged, 1u);
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), published);

        // Redo can also be the removal direction. Exercise the same core path
        // with a plain delete transaction, independently of the publisher.
        immediate.disconnect(); display.disconnect();
        std::vector<SPItem *> pieces;
        for (auto node = previous->next(); node && node != doc->getObjectById("after")->getRepr(); node = node->next())
            pieces.push_back(cast<SPItem>(doc->getObjectByRepr(node)));
        ASSERT_EQ(pieces.size(), n);
        for (auto item : pieces) item->deleteObject();
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("BUG017 delete"), "");
        auto removed = sp_repr_save_buf(doc->getReprDoc()).raw();
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), published);
        pieces.clear();
        for (auto node = previous->next(); node && node != doc->getObjectById("after")->getRepr(); node = node->next())
            pieces.push_back(cast<SPItem>(doc->getObjectByRepr(node)));
        pieces.push_back(survivor);
        selection->setList(pieces);
        changed = displayChanged = 0;
        immediate = selection->connectChanged([&](Selection *) { ++changed; });
        display = selection->connectChangedForDisplay([&](Selection *s) {
            ++displayChanged;
            EXPECT_EQ(s->singleItem(), survivor);
        });
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), removed);
        EXPECT_EQ(selection->singleItem(), survivor);
        EXPECT_EQ(changed, 1u); // BUG-026c: one batch change before the replay.
        EXPECT_EQ(displayChanged, 1u);
    }
}

TEST_F(Publish, Bug017RollbackDisplaySettlesBeforeReturn) {
    open(150);
    struct Counts { unsigned immediate = 0, display = 0; } counts;
    auto selection = desktop->getSelection();
    sigc::scoped_connection immediate = selection->connectChanged([&](Selection *) { ++counts.immediate; });
    sigc::scoped_connection display = selection->connectChangedForDisplay([&](Selection *) { ++counts.display; });
    prepared.hooks = {[](PublishStage stage, unsigned, void *data) {
        if (stage == PublishStage::Selection) {
            *static_cast<Counts *>(data) = {};
            throw std::runtime_error("BUG017 rollback");
        }
    }, &counts};
    ASSERT_EQ(publishExplode(*desktop, prepared, 1).status, Status::failed);
    unchanged();
    EXPECT_EQ(counts.immediate, 151u); // 150 releases, then restore original selection.
    EXPECT_EQ(counts.display, 2u);     // End of replay, then restored original selection.
    handlesMatch(image()->desktopVisualBounds());
}

TEST_F(Publish, Bug017OuterMutationRefreshesActualHandlesAfterUndoAndRedo) {
    for (bool nested : {false, true}) {
        for (bool keepSurvivor : {false, true}) {
            for (bool removeOnRedo : {false, true}) {
                SCOPED_TRACE(::testing::Message() << "nested groups=" << nested
                    << " survivor=" << keepSurvivor << " redo=" << removeOnRedo);
                open(5, nested);
                ASSERT_EQ(publishExplode(*desktop, prepared, 1).status, Status::changed);
                auto selection = desktop->getSelection();
                auto expectedXml = before;
                if (removeOnRedo) {
                    for (auto item : selection->items_vector()) item->deleteObject();
                    DocumentUndo::done(doc.get(), Util::Internal::ContextString("BUG017 delete handles"), "");
                    expectedXml = sp_repr_save_buf(doc->getReprDoc()).raw();
                    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
                    std::vector<SPItem *> restored;
                    auto end = doc->getObjectById("after")->getRepr();
                    for (auto node = previous->next(); node != end; node = node->next()) {
                        ASSERT_TRUE(node);
                        restored.push_back(cast<SPItem>(doc->getObjectByRepr(node)));
                    }
                    ASSERT_EQ(restored.size(), 5u);
                    selection->setList(restored);
                }
                auto survivor = cast<SPItem>(doc->getObjectById("before"));
                if (keepSurvivor) selection->add(survivor);
                auto expectedBounds = keepSurvivor ? survivor->desktopVisualBounds() : Geom::OptRect{};
                handlesMatch(selection->visualBounds());
                unsigned displayCount = 0;
                sigc::scoped_connection display = selection->connectChangedForDisplay([&](Selection *) { ++displayCount; });
                {
                    XML::Document::MutationScope outerMutation(*doc->getReprDoc());
                    ASSERT_TRUE(removeOnRedo ? DocumentUndo::redo(doc.get()) : DocumentUndo::undo(doc.get()));
                    EXPECT_FALSE(doc->isSeeking());
                    EXPECT_EQ(selection->size(), keepSurvivor ? 1u : 0u);
                    EXPECT_EQ(displayCount, 0u);
                }
                // No main-loop/modified notification is allowed to repair this
                // oracle: the actual knobs must already reflect the final bounds.
                handlesMatch(expectedBounds);
                EXPECT_EQ(displayCount, 1u);
                EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), expectedXml);
            }
        }
    }
}

TEST_F(Publish, Bug017DisplayExceptionsDuringUnwindingKeepOutstandingRefreshes) {
    open(5);
    ASSERT_EQ(publishExplode(*desktop, prepared, 1).status, Status::changed);
    unsigned attempts = 0, later = 0, quiescent = 0;
    auto selection = desktop->getSelection();
    sigc::scoped_connection throwing = selection->connectChangedForDisplay([&](Selection *s) {
        EXPECT_TRUE(s->isEmpty());
        if (++attempts <= 2) throw std::bad_alloc();
    });
    sigc::scoped_connection healthy = selection->connectChangedForDisplay([&](Selection *s) {
        EXPECT_TRUE(s->isEmpty());
        ++later;
    });
    sigc::scoped_connection finished = doc->getReprDoc()->signalMutationFinished().connect([&] { ++quiescent; });
    try {
        XML::Document::MutationScope outerMutation(*doc->getReprDoc());
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        EXPECT_EQ(attempts, 0u);
        throw std::runtime_error("unwind outer mutation");
    } catch (std::runtime_error const &error) {
        EXPECT_STREQ(error.what(), "unwind outer mutation");
    }
    EXPECT_EQ(attempts, 1u);
    EXPECT_EQ(later, 1u); // One bad observer cannot starve the later observer.
    EXPECT_EQ(quiescent, 1u);
    handlesMatch({});
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    for (unsigned i = 0; i < 100 && g_main_context_iteration(nullptr, false); ++i) {}
    EXPECT_EQ(attempts, 2u); // A failed idle retry must not spin or lose work.
    EXPECT_EQ(later, 2u);
    { XML::Document::MutationScope retry(*doc->getReprDoc()); }
    EXPECT_EQ(attempts, 3u);
    EXPECT_EQ(later, 3u);
    { XML::Document::MutationScope settled(*doc->getReprDoc()); }
    EXPECT_EQ(attempts, 3u);
}

TEST_F(Publish, Bug017DisplayFailureRetriesWithoutAnotherMutation) {
    open(5);
    ASSERT_EQ(publishExplode(*desktop, prepared, 1).status, Status::changed);
    unsigned attempts = 0;
    sigc::scoped_connection throwing = desktop->getSelection()->connectChangedForDisplay([&](Selection *) {
        if (++attempts == 1) throw std::runtime_error("transient display failure");
    });
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(attempts, 1u);
    for (unsigned i = 0; i < 100 && g_main_context_iteration(nullptr, false); ++i) {}
    EXPECT_EQ(attempts, 2u);
    { XML::Document::MutationScope settled(*doc->getReprDoc()); }
    EXPECT_EQ(attempts, 2u);
    handlesMatch({});
}

TEST_F(Publish, Bug017DestroyedSelectionDisconnectsPendingDisplayAndRetry) {
    for (bool failedDelivery : {false, true}) {
        open(5);
        ASSERT_EQ(publishExplode(*desktop, prepared, 1).status, Status::changed);
        auto selection = std::make_unique<Selection>(doc.get());
        selection->setList(desktop->getSelection()->items_vector());
        unsigned attempts = 0;
        sigc::scoped_connection display = selection->connectChangedForDisplay([&](Selection *) {
            ++attempts;
            throw std::bad_alloc();
        });
        {
            XML::Document::MutationScope outerMutation(*doc->getReprDoc());
            ASSERT_TRUE(DocumentUndo::undo(doc.get()));
            EXPECT_TRUE(selection->isEmpty());
            EXPECT_EQ(attempts, 0u);
            if (!failedDelivery) selection.reset();
        }
        EXPECT_EQ(attempts, failedDelivery ? 1u : 0u);
        selection.reset(); // Also cancels an idle retry after failed delivery.
        { XML::Document::MutationScope afterDestruction(*doc->getReprDoc()); }
        for (unsigned i = 0; i < 100 && g_main_context_iteration(nullptr, false); ++i) {}
        EXPECT_EQ(attempts, failedDelivery ? 1u : 0u);
        EXPECT_FALSE(display.connected());
    }
}

TEST_F(Publish, Bug017DisplayObserverCanDestroySelectionDuringDelivery) {
    enum class Delivery { MutationEnd, IdleRetry, Immediate };
    for (auto delivery : {Delivery::MutationEnd, Delivery::IdleRetry, Delivery::Immediate}) {
        for (bool throwAfterDestruction : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "delivery=" << static_cast<int>(delivery)
                << " throw after destruction=" << throwAfterDestruction);
            open(5);
            ASSERT_EQ(publishExplode(*desktop, prepared, 1).status, Status::changed);
            auto selection = std::make_unique<Selection>(doc.get());
            selection->setList(desktop->getSelection()->items_vector());
            unsigned attempts = 0, later = 0, immediate = 0;
            sigc::scoped_connection ordinary = selection->connectChanged([&](Selection *) { ++immediate; });
            sigc::scoped_connection destroying = selection->connectChangedForDisplay([&](Selection *s) {
                EXPECT_EQ(s, selection.get());
                EXPECT_TRUE(s->isEmpty());
                if (++attempts == 1 && delivery == Delivery::IdleRetry) throw std::bad_alloc();
                selection.reset();
                if (throwAfterDestruction) throw std::bad_alloc();
            });
            sigc::scoped_connection trailing = selection->connectChangedForDisplay([&](Selection *) { ++later; });
            if (delivery == Delivery::Immediate) {
                selection->clear();
                EXPECT_EQ(immediate, 0u); // Delivery must not resume on the deleted Selection.
            } else {
                {
                    XML::Document::MutationScope outerMutation(*doc->getReprDoc());
                    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
                    EXPECT_EQ(attempts, 0u);
                    // Live membership observers remain immediate: BUG-026c delivers the
                    // five replay removals as one change before the replay.
                    EXPECT_EQ(immediate, 1u);
                }
                if (delivery == Delivery::IdleRetry) {
                    ASSERT_TRUE(selection);
                    EXPECT_EQ(attempts, 1u);
                    EXPECT_EQ(later, 1u); // A failed live observer still permits later observers.
                    for (unsigned i = 0; i < 100 && g_main_context_iteration(nullptr, false); ++i) {}
                }
            }
            EXPECT_FALSE(selection);
            auto expectedAttempts = delivery == Delivery::IdleRetry ? 2u : 1u;
            auto expectedLater = delivery == Delivery::IdleRetry ? 1u : 0u;
            EXPECT_EQ(attempts, expectedAttempts);
            EXPECT_EQ(later, expectedLater);
            EXPECT_FALSE(destroying.connected());
            EXPECT_FALSE(trailing.connected());
            EXPECT_FALSE(ordinary.connected());
            { XML::Document::MutationScope afterDestruction(*doc->getReprDoc()); }
            for (unsigned i = 0; i < 100 && g_main_context_iteration(nullptr, false); ++i) {}
            EXPECT_EQ(attempts, expectedAttempts);
            EXPECT_EQ(later, expectedLater);
        }
    }
}

TEST_F(Publish, Bug017QuiescentDispatchAllocationFailureRetainsDisplayRefresh) {
    for (bool unwind : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "unwind=" << unwind);
        open(5);
        ASSERT_EQ(publishExplode(*desktop, prepared, 1).status, Status::changed);
        unsigned display = 0, quiescent = 0;
        sigc::scoped_connection displayConnection = desktop->getSelection()->connectChangedForDisplay([&](Selection *s) {
            EXPECT_TRUE(s->isEmpty());
            ++display;
        });
        sigc::scoped_connection finished = doc->getReprDoc()->signalMutationFinished().connect([&] { ++quiescent; });
        try {
            XML::Document::MutationScope outerMutation(*doc->getReprDoc());
            ASSERT_TRUE(DocumentUndo::undo(doc.get()));
            EXPECT_EQ(display, 0u);
            EXPECT_EQ(quiescent, 0u);
            // Arm after construction of the exception, immediately before the
            // outer MutationScope destructor, on both normal and unwind paths.
            struct ArmDispatchFailure {
                ~ArmDispatchFailure() { failNextAllocation = true; }
            } arm;
            if (unwind) throw std::runtime_error("unwind through quiescent allocation failure");
        } catch (std::runtime_error const &error) {
            EXPECT_TRUE(unwind);
            EXPECT_STREQ(error.what(), "unwind through quiescent allocation failure");
        }
        auto faultNotConsumed = std::exchange(failNextAllocation, false);
        EXPECT_FALSE(faultNotConsumed);
        EXPECT_FALSE(doc->getReprDoc()->mutationActive());
        EXPECT_EQ(quiescent, 0u); // The real allocation failed before the first slot.
        EXPECT_EQ(display, 0u);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        // The failed dispatch must leave both the subscription and the pending
        // selection request intact, even though no flush callback was entered.
        { XML::Document::MutationScope retry(*doc->getReprDoc()); }
        EXPECT_EQ(quiescent, 1u);
        EXPECT_EQ(display, 1u);
        handlesMatch({});
        { XML::Document::MutationScope settled(*doc->getReprDoc()); }
        for (unsigned i = 0; i < 100 && g_main_context_iteration(nullptr, false); ++i) {}
        EXPECT_EQ(display, 1u);
    }
}

TEST_F(Publish, T25EmptyApplyRefusesBeforeEveryWrite) {
    open(); redoBranch();
    auto pixels = image()->pixbuf; auto recipe = query(sourceIdentity);
    auto revision = doc->getReprDoc()->contentRevision();
    for (auto ticket : {0u, 1u, 1u, 2u}) {
        auto result = publishAlpha(*desktop, {}, ticket);
        EXPECT_EQ(result.status, Status::unavailable);
        EXPECT_STREQ(result.diagnostic, "No prepared adjustment is available.");
        unchanged(1); EXPECT_EQ(image()->pixbuf, pixels);
        EXPECT_EQ(doc->getReprDoc()->contentRevision(), revision);
        EXPECT_EQ(query(sourceIdentity).bypassAlpha, recipe.bypassAlpha);
    }
}
TEST_F(Publish, T13T16T30ExactSlotGeometryBytesAndSingleUndoRedo) {
    for (bool nested : {false, true}) {
        open(3, nested); redoBranch();
        remember(sourceIdentity, Recipe{130, 41}); recapture();
        auto result = publishExplode(*desktop, prepared, 1);
        ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
        auto after = sp_repr_save_buf(doc->getReprDoc()).raw(); EXPECT_NE(after, before);
        auto items = desktop->getSelection()->items_vector(); ASSERT_EQ(items.size(), 3u);
        char matrix[256]; auto size = serializeGridTransform(grid.pixelToParent, matrix, sizeof(matrix));
        auto node = previous->next();
        std::vector<LogicalImageIdentity> ids;
        for (unsigned i = 0; i < 3; ++i, node = node->next()) {
            ASSERT_TRUE(node); EXPECT_EQ(node, items[i]->getRepr());
            EXPECT_EQ(node->parent(), parent); EXPECT_EQ(node->firstChild(), nullptr);
            EXPECT_STREQ(node->attribute("transform"), std::string(matrix, size).c_str());
            EXPECT_STREQ(node->attribute("preserveAspectRatio"), "none");
            EXPECT_EQ(node->attribute("opacity"), nullptr); EXPECT_EQ(node->attribute("style"), nullptr);
            EXPECT_EQ(node->attribute("filter"), nullptr); EXPECT_EQ(node->attribute("href"), nullptr);
            EXPECT_EQ(std::stoi(node->attribute("x")), pieces.piece(i).x);
            EXPECT_EQ(std::stoi(node->attribute("width")), pieces.piece(i).width);
            auto href = node->attribute("xlink:href"); ASSERT_TRUE(href);
            gsize bytes = 0; auto decoded = g_base64_decode(href + 22, &bytes);
            ASSERT_EQ(bytes, pieces.piece(i).size); EXPECT_EQ(std::memcmp(decoded, pieces.piece(i).data, bytes), 0); g_free(decoded);
            auto object = cast<SPImage>(items[i]); ASSERT_TRUE(object); ASSERT_TRUE(object->pixbuf);
            EXPECT_EQ(object->pixbuf->width(), pieces.piece(i).width);
            ids.push_back(logicalImageIdentity(*object));
            EXPECT_TRUE(query(ids.back()).bypassAlpha); EXPECT_EQ(query(ids.back()).threshold, 130u);
        }
        ASSERT_TRUE(node); EXPECT_STREQ(node->attribute("id"), "after");
        EXPECT_STREQ(items[0]->getId(), "im"); EXPECT_STREQ(items[1]->getId(), "im-piece-2");
        auto usage = preflightUndo(*doc, {false, 0, 1}).usage;
        EXPECT_EQ(usage.undoCount, 1u); EXPECT_EQ(usage.redoCount, 0u);
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_FALSE(query(sourceIdentity).bypassAlpha);
        ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
        for (auto id : ids) EXPECT_TRUE(query(id).bypassAlpha);
        auto reopened = SPDocument::createNewDocFromMem(after); ASSERT_TRUE(reopened);
        EXPECT_EQ(sp_repr_save_buf(reopened->getReprDoc()).raw(), after);
    }
}
struct Fault {
    PublishStage stage; unsigned index = 0, kind = 0; bool fired = false;
    std::function<void()> action;
    static void call(PublishStage stage, unsigned index, void *data) {
        auto &f = *static_cast<Fault *>(data);
        if (stage != f.stage || index != f.index) return;
        f.fired = true; if (f.action) { f.action(); return; }
        if (f.kind == 0) throw std::bad_alloc();
        if (f.kind == 1) throw std::length_error("publisher fault");
        throw 7;
    }
};
TEST_F(Publish, T07T13EveryPublisherStageRestoresXMLSelectionHistoryRedoAndLedger) {
    for (auto stage : {PublishStage::Admission, PublishStage::Href, PublishStage::Node, PublishStage::Script,
                      PublishStage::Guard, PublishStage::Delete, PublishStage::Resources, PublishStage::Insert, PublishStage::Binding,
                      PublishStage::Native, PublishStage::Selection, PublishStage::Bake, PublishStage::Settlement,
                      PublishStage::Readiness}) {
        for (unsigned kind = 0; kind < 3; ++kind) {
            SCOPED_TRACE(static_cast<int>(stage));
            SCOPED_TRACE(kind); open(3, true); redoBranch();
            auto baseline = budget->reserved(); auto history = preflightUndo(*doc, {false, 0, 1}).usage;
            Fault f{stage, stage == PublishStage::Insert || stage == PublishStage::Binding ? 1u : 0u, kind};
            prepared.hooks = {Fault::call, &f};
            EXPECT_EQ(publishExplode(*desktop, prepared, 1).status, Status::failed);
            EXPECT_TRUE(f.fired); unchanged(1); EXPECT_EQ(budget->reserved(), baseline);
            EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.redoBytes, history.redoBytes);
            ASSERT_TRUE(DocumentUndo::redo(doc.get()));
            EXPECT_STREQ(doc->getObjectById("after")->getRepr()->attribute("x"), "42");
        }
    }
}
TEST_F(Publish, T13RollbackPressureStillRunsReservedRecovery) {
    open(); Fault f{PublishStage::Rollback};
    prepared.hooks = {[](PublishStage stage, unsigned i, void *p) {
        if (stage == PublishStage::Insert) throw std::bad_alloc(); Fault::call(stage, i, p);
    }, &f};
    EXPECT_EQ(publishExplode(*desktop, prepared, 1).status, Status::failed); EXPECT_TRUE(f.fired); unchanged();
}
TEST_F(Publish, T22ZeroPiecesPreserveRedo) {
    open(0); redoBranch(); auto baseline = budget->reserved();
    EXPECT_EQ(publishExplode(*desktop, prepared, 1).status, Status::unchanged);
    unchanged(1); EXPECT_EQ(budget->reserved(), baseline);
}
TEST_F(Publish, SinglePiecePublishesAndOneUndoRestoresOriginal) {
    open(1); auto originalXml = before;
    auto result = publishExplode(*desktop, prepared, 1);
    ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    EXPECT_EQ(desktop->getSelection()->size(), 1u);
    auto published = sp_repr_save_buf(doc->getReprDoc()).raw(); EXPECT_NE(published, originalXml);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
    DocumentUndo::undo(doc.get()); EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), originalXml);
    DocumentUndo::redo(doc.get()); EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), published);
}
TEST_F(Publish, T28TicketAndNestedActivationPublishAtMostOnce) {
    open(); Fault f{PublishStage::Delete};
    f.action = [&] { EXPECT_FALSE(publishExplode(*desktop, prepared, 1).ok()); EXPECT_FALSE(publishExplode(*desktop, prepared, 2).ok()); };
    prepared.hooks = {Fault::call, &f};
    ASSERT_EQ(publishExplode(*desktop, prepared, 1).status, Status::changed); EXPECT_TRUE(f.fired);
    auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
    EXPECT_FALSE(publishExplode(*desktop, prepared, 1).ok()); EXPECT_FALSE(publishExplode(*desktop, prepared, 3).ok());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
}
TEST_F(Publish, T16BoundAndStagedIdReservationAndReleaseCollision) {
    open();
    auto extra = doc->getObjectById("after")->getRepr(); extra->setAttribute("id", "im-piece-2");
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Reserve"), "");
    recapture(); ASSERT_EQ(publishExplode(*desktop, prepared, 1).status, Status::changed);
    auto items = desktop->getSelection()->items_vector(); ASSERT_EQ(items.size(), 2u);
    EXPECT_STREQ(items[1]->getId(), "im-piece-3"); EXPECT_EQ(doc->getObjectById("im-piece-2")->getRepr(), extra);
    open(); Fault collision{PublishStage::Delete};
    collision.action = [&] {
        auto n = doc->getReprDoc()->createElement("svg:image"); n->setAttribute("id", "im"); parent->addChild(n, previous); GC::release(n);
    };
    prepared.hooks = {Fault::call, &collision};
    EXPECT_EQ(publishExplode(*desktop, prepared, 1).status, Status::failed); EXPECT_TRUE(collision.fired); unchanged();
}
TEST_F(Publish, T13ForeignSelectionStagedXMLAndPendingCloseRefuseSettlement) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        open(); Fault foreign{PublishStage::Insert};
        foreign.action = [&] {
            if (mode == 0) desktop->getSelection()->set(cast<SPItem>(doc->getObjectById("before")));
            if (mode == 1) image()->getRepr()->setAttribute("width", "99");
            if (mode == 3) remember(sourceIdentity, Recipe{111, 40});
            if (mode == 2) DocumentUndo::deferUntilInteractionQuiescent(doc.get(), [](SPDocument &) {}, true);
        };
        prepared.hooks = {Fault::call, &foreign};
        EXPECT_EQ(publishExplode(*desktop, prepared, 1).status, Status::failed); EXPECT_TRUE(foreign.fired); unchanged();
    }
}

TEST_F(Publish, T07EventAndHistoryPreparationFailuresRetainExactRedo) {
    static DocumentUndo::AtomicSettlementStage failAt;
    for (auto stage : {DocumentUndo::AtomicSettlementStage::EventConstruction,
                       DocumentUndo::AtomicSettlementStage::HistoryInsertion}) {
        open(); redoBranch(); auto baseline = budget->reserved(); failAt = stage;
        DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) { return s == failAt; });
        auto result = publishExplode(*desktop, prepared, 1);
        DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
        EXPECT_EQ(result.status, Status::failed); unchanged(1); EXPECT_EQ(budget->reserved(), baseline);
    }
}
TEST_F(Publish, T07T13FirstMiddleLastInsertionAndBindingFaults) {
    for (auto stage : {PublishStage::Insert, PublishStage::Binding}) for (unsigned index : {0u, 4u, 9u}) {
        open(10); Fault fault{stage, index}; prepared.hooks = {Fault::call, &fault};
        EXPECT_EQ(publishExplode(*desktop, prepared, 1).status, Status::failed);
        EXPECT_TRUE(fault.fired); unchanged();
        prepared.hooks = {}; recapture();
        // A fresh captured result and ticket can succeed after recovery.
        EXPECT_EQ(publishExplode(*desktop, prepared, 2).status, Status::changed);
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    }
}
TEST_F(Publish, T16ProductCapStagedIdsUndoRedoAndReopen) {
    open(150); unsigned ticket = 1;
    for (unsigned index : {0u, 74u, 149u}) {
        Fault f{PublishStage::Insert, index}; prepared.hooks = {Fault::call, &f};
        EXPECT_EQ(publishExplode(*desktop, prepared, ticket++).status, Status::failed);
        EXPECT_TRUE(f.fired); unchanged(); recapture();
    }
    prepared.hooks = {};
    auto result = publishExplode(*desktop, prepared, ticket);
    ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    auto selected = desktop->getSelection()->items_vector(); ASSERT_EQ(selected.size(), 150u);
    std::unordered_set<std::string> ids;
    for (auto item : selected) {
        ASSERT_TRUE(ids.emplace(item->getId()).second);
        ASSERT_EQ(doc->getObjectById(item->getId()), item);
    }
    auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
    auto reopened = SPDocument::createNewDocFromMem(after); ASSERT_TRUE(reopened);
    EXPECT_EQ(sp_repr_save_buf(reopened->getReprDoc()).raw(), after);
}
TEST_F(Publish, T16GraphReferencesAndStaleGenerationRefuseBeforeWrite) {
    open(); auto use = doc->getReprDoc()->createElement("svg:use");
    use->setAttribute("href", "#im"); doc->getReprRoot()->appendChild(use); GC::release(use);
    doc->ensureUpToDate(); DocumentUndo::done(doc.get(), Util::Internal::ContextString("Reference"), "");
    auto baseline = sp_repr_save_buf(doc->getReprDoc()).raw();
    EXPECT_FALSE(resolve(*desktop, Intent::Explode).ok());
    EXPECT_FALSE(publishExplode(*desktop, prepared, 1).ok());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), baseline);
    open(); remember(sourceIdentity, Recipe{129, 40});
    EXPECT_FALSE(publishExplode(*desktop, prepared, 1).ok()); unchanged();
}
TEST_F(Publish, T30OutputCannotObserveAdmittedPartialPublication) {
    open(); std::vector<std::string> outputs;
    struct Output { SPDocument *doc; std::vector<std::string> *bytes; } output{doc.get(), &outputs};
    prepared.hooks = {[](PublishStage stage, unsigned, void *data) {
        auto &o = *static_cast<Output *>(data);
        if (stage == PublishStage::Delete || stage == PublishStage::Insert || stage == PublishStage::Selection) {
            EXPECT_FALSE(DocumentUndo::publicationCompletable(o.doc));
            EXPECT_TRUE(DocumentUndo::whenPublicationCompletable(o.doc, [bytes = o.bytes](SPDocument &d) {
                bytes->push_back(sp_repr_save_buf(d.getReprDoc()).raw());
            }, [](SPDocument &) {}));
            EXPECT_TRUE(DocumentUndo::publicationPending(o.doc)); EXPECT_TRUE(o.bytes->empty());
        }
    }, &output};
    auto result = publishExplode(*desktop, prepared, 1);
    ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    EXPECT_TRUE(DocumentUndo::publicationCompletable(doc.get()));
    DocumentUndo::flushPublicationCompletions(doc.get());
    ASSERT_EQ(outputs.size(), 4u);
    for (auto const &bytes : outputs) EXPECT_EQ(bytes, sp_repr_save_buf(doc->getReprDoc()).raw());
    EXPECT_FALSE(DocumentUndo::publicationPending(doc.get()));
}
TEST_F(Publish, T13T25OwnedClipCleanupBakesOnceAndRollsBackExactly) {
    for (bool fault : {false, true}) {
        open(3, false, true); Fault f{PublishStage::Resources};
        if (fault) prepared.hooks = {Fault::call, &f};
        auto result = publishExplode(*desktop, prepared, 1);
        if (fault) { EXPECT_EQ(result.status, Status::failed); EXPECT_TRUE(f.fired); unchanged(); }
        else {
            ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
            EXPECT_EQ(doc->getObjectById("clip"), nullptr);
            for (auto piece : desktop->getSelection()->items_vector()) {
                EXPECT_EQ(piece->getRepr()->attribute("clip-path"), nullptr);
                EXPECT_TRUE(query(logicalImageIdentity(*cast<SPImage>(piece))).bypassAlpha);
            }
            ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
            EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        }
    }
}
TEST_F(Publish, T13T16SurvivingPredecessorAndBoundarySlots) {
    std::string clip = "<clipPath id='clip' inkscape:collect='always'><rect width='20' height='20'/></clipPath>";
    std::string pattern = "<pattern id='paint' inkscape:collect='always' width='2' height='2'><rect width='1' height='1'/></pattern>";
    for (unsigned shape = 0; shape < 8; ++shape) for (bool fault : {false, true}) {
        SCOPED_TRACE(shape);
        SCOPED_TRACE(fault);
        auto resource = shape == 0 || shape == 4 || shape == 7 ? clip :
                        shape == 1 ? "<mask id='mask' inkscape:collect='always'><rect width='20' height='20'/></mask>" : shape == 3 ? pattern : "";
        auto attrs = shape == 0 || shape == 4 || shape == 7 ? " clip-path='url(#clip)'" : "";
        open(3, false, false, "<rect id='before'/>" + resource + "@image@<rect id='after'" +
             (shape == 1 ? " mask='url(#mask)'" : shape == 3 ? " fill='url(#paint)'" : "") + "/>", attrs);
        std::string filterId;
        if (shape == 2 || shape == 4) {
            Filters::BitmapToneSettings tone; tone.brightness = 12;
            ASSERT_TRUE(BitmapAdjustments::apply_tone(image(), tone));
            auto filter = image()->style->getFilter()->getRepr();
            filter->setAttribute("inkscape:collect", "always"); filterId = filter->attribute("id");
            GC::anchor(filter); filter->parent()->removeChild(filter); parent->addChild(filter, original->prev()); GC::release(filter);
        }
        if (shape == 5) parent->changeOrder(original, nullptr);
        if (shape == 6) parent->changeOrder(original, parent->lastChild());
        if (shape == 7) { auto clipRepr = doc->getObjectById("clip")->getRepr(); parent->changeOrder(clipRepr, nullptr); parent->changeOrder(original, clipRepr); }
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture"), "");
        DocumentUndo::clearUndo(doc.get()); recapture(); previous = original->prev();
        if (shape == 5) ASSERT_EQ(previous, nullptr);
        if (shape == 6) ASSERT_EQ(original->next(), nullptr);
        auto next = original->next();
        auto surviving = shape == 7 ? nullptr : shape == 0 || shape == 2 || shape == 4 ? doc->getObjectById("before")->getRepr() : previous;
        Fault f{PublishStage::Insert, 1}; if (fault) prepared.hooks = {Fault::call, &f};
        auto result = publishExplode(*desktop, prepared, 1);
        if (fault) { EXPECT_EQ(result.status, Status::failed); EXPECT_TRUE(f.fired); unchanged(); continue; }
        ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
        auto items = desktop->getSelection()->items_vector(); ASSERT_EQ(items.size(), 3u);
        EXPECT_EQ(items.front()->getRepr()->prev(), surviving); EXPECT_EQ(items.back()->getRepr()->next(), next);
        if (shape == 0 || shape == 4 || shape == 7) EXPECT_EQ(doc->getObjectById("clip"), nullptr);
        if (!filterId.empty()) EXPECT_EQ(doc->getObjectById(filterId), nullptr);
        if (shape == 3) EXPECT_NE(doc->getObjectById("paint"), nullptr);
        if (shape == 1) EXPECT_NE(doc->getObjectById("mask"), nullptr); // Masks remain unsupported on the source.
        auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        ASSERT_TRUE(image()); EXPECT_EQ(image()->getRepr(), original); EXPECT_EQ(original->prev(), previous);
        ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
    }
}
TEST_F(Publish, T13OrdinaryDocumentCascadesSettleWithExactXML) {
    open(2, false, false, R"svg(<rect id='paint-before' width='2' height='2' fill='url(#linear)'/><defs>
      <filter id='retained-filter' filterUnits='userSpaceOnUse'><feComponentTransfer id='transfer'>
        <feFuncR id='red' type='gamma' amplitude='1'/><feFuncG type='identity'/><feFuncB type='identity'/><feFuncA type='identity'/>
      </feComponentTransfer><feMerge><feMergeNode in='SourceGraphic'/></feMerge>
      <feDiffuseLighting><feDistantLight/></feDiffuseLighting><feSpecularLighting><fePointLight/></feSpecularLighting></filter>
      <filter id='linked' filterUnits='userSpaceOnUse' xlink:href='#retained-filter'/><filter id='chain' filterUnits='userSpaceOnUse' xlink:href='#linked'/>
      <linearGradient id='linear'><stop offset='0'/><stop offset='1'/></linearGradient>
      <linearGradient id='linked-gradient' xlink:href='#linear'/>
      <radialGradient id='radial' xlink:href='#linked-gradient'><stop offset='0'/><stop offset='1'/></radialGradient>
      <pattern id='retained-pattern' width='2' height='2'><g><rect width='1' height='1'/></g></pattern>
      <marker id='marker'><path d='M0,0L1,1'/></marker><symbol id='symbol'><rect width='2' height='2'/></symbol>
      <clipPath id='retained-clip'><rect width='2' height='2'/></clipPath><mask id='retained-mask'><rect width='2' height='2'/></mask>
    <inkscape:path-effect effect='spiro' id='lpe' is_visible='true'/>
    </defs><use xlink:href='#symbol'/><path d='M 0,0 3,3' inkscape:original-d='M 0,0 3,3' inkscape:path-effect='#lpe'/>
    <rect id='before'/>@image@<rect id='after' width='2' height='2' fill='url(#retained-pattern)'/>)svg");
    Fault foreign{PublishStage::Native};
    foreign.action = [&] { doc->getObjectById("red")->emitModified(SP_OBJECT_PARENT_MODIFIED_FLAG); };
    prepared.hooks = {Fault::call, &foreign};
    auto result = publishExplode(*desktop, prepared, 1);
    ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    EXPECT_TRUE(foreign.fired);
    auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
    auto reopened = SPDocument::createNewDocFromMem(after); ASSERT_TRUE(reopened);
    EXPECT_EQ(sp_repr_save_buf(reopened->getReprDoc()).raw(), after);
}
TEST_F(Publish, T13T30RefusalAndFailurePreserveToneMergeKey) {
    static DocumentUndo::AtomicSettlementStage settlement;
    auto last = static_cast<int>(PublishStage::Settlement);
    for (int stage = -1; stage <= last + 2; ++stage) {
        SCOPED_TRACE(stage); open();
        auto baseline = sp_repr_save_buf(doc->getReprDoc()).raw();
        UI::BitmapAdjustmentsController tone(desktop.get());
        Filters::BitmapTonePatch patch; patch.brightness = 12;
        ASSERT_TRUE(tone.commitPatch(patch, true)); doc->ensureUpToDate(); recapture(false);
        auto key = doc->action_key(); ASSERT_FALSE(key.empty());
        Fault f{static_cast<PublishStage>(stage)};
        if (stage < 0) ASSERT_TRUE(prepared.resources.add(Term::prepared, 16 * 1024 * MiB, 0, 0));
        else if (stage <= last) prepared.hooks = {Fault::call, &f};
        else {
            settlement = stage == last + 1 ? DocumentUndo::AtomicSettlementStage::EventConstruction : DocumentUndo::AtomicSettlementStage::HistoryInsertion;
            DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) { return s == settlement; });
        }
        EXPECT_FALSE(publishExplode(*desktop, prepared, 1).ok());
        DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
        if (stage >= 0 && stage <= last) EXPECT_TRUE(f.fired);
        EXPECT_EQ(doc->action_key(), key);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        patch.brightness = 24; ASSERT_TRUE(tone.commitPatch(patch, true));
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), baseline);
    }
}
TEST_F(Publish, T25ApplyUndoRedoRepeatedAndPreservedEffects) {
    open(2, true, true);
    Filters::BitmapToneSettings tone; tone.brightness = 12;
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image(), tone));
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture"), "");
    DocumentUndo::clearUndo(doc.get()); remember(sourceIdentity, Recipe{130, 41}); recapture();
    auto alpha = alphaResult(90); ASSERT_EQ(alpha.image.count(), 1u);
    auto source = image(); auto initial = before;
    auto result = publishAlpha(*desktop, alpha, 1);
    ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
    EXPECT_EQ(image(), source); EXPECT_EQ(image()->getRepr(), original);
    EXPECT_EQ(desktop->getSelection()->singleItem(), source); EXPECT_TRUE(query(sourceIdentity).bypassAlpha);
    auto href = std::string(original->attribute("href"));
    gsize length; auto bytes = g_base64_decode(href.c_str() + 22, &length);
    EXPECT_EQ(length, alpha.image.piece(0).size);
    EXPECT_EQ(std::memcmp(bytes, alpha.image.piece(0).data, length), 0); g_free(bytes);
    auto changedOnlyHref = SPDocument::createNewDocFromMem(initial);
    changedOnlyHref->getObjectById("im")->getRepr()->setAttribute("href", href);
    EXPECT_EQ(sp_repr_save_buf(changedOnlyHref->getReprDoc()).raw(), after);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), initial); EXPECT_FALSE(query(sourceIdentity).bypassAlpha);
    alpha.image.reset(); // Redo cannot use the prepared PNG or rerun its recipe.
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after); EXPECT_TRUE(query(sourceIdentity).bypassAlpha);
    recapture(); auto equal = alphaResult(90);
    EXPECT_EQ(publishAlpha(*desktop, equal, 2).status, Status::unchanged);
    remember(sourceIdentity, Recipe{90, 15}); recapture(); auto second = alphaResult(155);
    ASSERT_EQ(publishAlpha(*desktop, second, 3).status, Status::changed);
    auto twice = sp_repr_save_buf(doc->getReprDoc()).raw(); EXPECT_NE(twice, after);
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 2u);
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), twice);
}
TEST_F(Publish, T25ApplyFailuresAndForeignChangesRestoreExactly) {
    for (auto stage : {PublishStage::Admission, PublishStage::Href, PublishStage::Node, PublishStage::Script, PublishStage::Guard,
                      PublishStage::Binding, PublishStage::Native, PublishStage::Selection, PublishStage::Bake, PublishStage::Settlement, PublishStage::Readiness})
    for (unsigned kind = 0; kind < 3; ++kind) {
        SCOPED_TRACE(static_cast<int>(stage));
        SCOPED_TRACE(kind); open(); redoBranch(); auto alpha = alphaResult(90);
        auto baseline = budget->reserved(); Fault f{stage, 0, kind}; alpha.publication.hooks = {Fault::call, &f};
        EXPECT_EQ(publishAlpha(*desktop, alpha, 1).status, Status::failed); EXPECT_TRUE(f.fired);
        unchanged(1); EXPECT_EQ(budget->reserved(), baseline); EXPECT_FALSE(query(sourceIdentity).bypassAlpha);
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        SCOPED_TRACE(mode); open(); redoBranch(); auto alpha = alphaResult(90);
        Fault foreign{PublishStage::Native}; foreign.action = [&] { original->setAttribute("opacity", "0.3"); };
        if (mode == 0) alpha.publication.hooks = {Fault::call, &foreign};
        if (mode == 1) { original->setAttribute("data-foreign", "preserved"); doc->ensureUpToDate(); before = sp_repr_save_buf(doc->getReprDoc()).raw(); }
        if (mode == 2) alpha.publication.limits = {};
        if (mode == 3) ASSERT_TRUE(alpha.publication.resources.add(Term::prepared, 16 * 1024 * MiB, 0, 0));
        if (mode == 4) { foreign.action = [&] { desktop->getSelection()->clear(); }; alpha.publication.hooks = {Fault::call, &foreign}; }
        if (mode == 5) { foreign.action = [&] { remember(sourceIdentity, Recipe{80, 20}); }; alpha.publication.hooks = {Fault::call, &foreign}; }
        auto revision = doc->getReprDoc()->contentRevision();
        EXPECT_FALSE(publishAlpha(*desktop, alpha, 1).ok());
        if (mode == 1) doc->setModifiedSinceSave(false);
        unchanged(1);
        if (mode >= 1 && mode <= 3) EXPECT_EQ(doc->getReprDoc()->contentRevision(), revision);
    }
    static DocumentUndo::AtomicSettlementStage failure;
    for (auto stage : {DocumentUndo::AtomicSettlementStage::EventConstruction, DocumentUndo::AtomicSettlementStage::HistoryInsertion}) {
        open(); redoBranch(); auto alpha = alphaResult(90); failure = stage;
        DocumentUndo::setAtomicSettlementFaultForTesting([](auto s) { return s == failure; });
        auto result = publishAlpha(*desktop, alpha, 1); DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
        EXPECT_EQ(result.status, Status::failed); unchanged(1);
    }
}
TEST_F(Publish, FaintFloorOnlyEditRejectsStaleApply) {
    open(); auto alpha = alphaResult(90); auto recipe = query(sourceIdentity);
    recipe.faintFloor = 6; remember(sourceIdentity, recipe);
    EXPECT_FALSE(publishAlpha(*desktop, alpha, 1).ok()); unchanged();
}
TEST_F(Publish, T25ApplyHrefPriorityNoopRefineOffAndToneHistory) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode); open(2, false, false, {}, {}, mode == 1);
        auto href = std::string(original->attribute(mode == 1 ? "xlink:href" : "href"));
        if (mode == 0) original->setAttribute("xlink:href", href);
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture"), ""); DocumentUndo::clearUndo(doc.get()); recapture();
        auto alpha = alphaResult(90);
        if (mode == 2) { remember(sourceIdentity, Recipe{128, 40, 0}, false); recapture(); alpha.publication = prepared; }
        auto result = publishAlpha(*desktop, alpha, 1);
        ASSERT_EQ(result.status, mode == 2 ? Status::unchanged : Status::changed) << result.diagnostic;
        if (mode == 2) { unchanged(); EXPECT_NE(std::string(result.diagnostic).find("off"), std::string::npos); continue; }
        if (mode == 0) EXPECT_STREQ(original->attribute("xlink:href"), href.c_str()); // active href wins
        auto applied = sp_repr_save_buf(doc->getReprDoc()).raw();
        remember(sourceIdentity, Recipe{1, 0}); recapture(); auto equal = alphaResult(90);
        auto revision = doc->getReprDoc()->contentRevision();
        EXPECT_EQ(publishAlpha(*desktop, equal, 2).status, Status::unchanged); EXPECT_EQ(doc->getReprDoc()->contentRevision(), revision);
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate(); EXPECT_NE(sp_repr_save_buf(doc->getReprDoc()).raw(), applied);
    }
    for (bool fail : {false, true}) {
        open(); auto baseline = before; UI::BitmapAdjustmentsController tone(desktop.get());
        Filters::BitmapTonePatch patch; patch.brightness = 12;
        ASSERT_TRUE(tone.commitPatch(patch, true)); recapture(false); auto toned = before; auto key = doc->action_key();
        auto alpha = alphaResult(90); Fault fault{PublishStage::Readiness}; if (fail) alpha.publication.hooks = {Fault::call, &fault};
        EXPECT_EQ(publishAlpha(*desktop, alpha, 1).status, fail ? Status::failed : Status::changed);
        if (fail) EXPECT_EQ(doc->action_key(), key);
        patch.brightness = 24; ASSERT_TRUE(tone.commitPatch(patch, true));
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, fail ? 1u : 3u);
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        if (!fail) { ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate(); EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), toned);
            ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate(); }
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), baseline);
    }
}
TEST_F(Publish, T25ApplyRollbackPressureStillRecovers) {
    open(); redoBranch(); auto alpha = alphaResult(90); bool recovery = false;
    alpha.publication.hooks = {[](PublishStage stage, unsigned, void *p) {
        if (stage == PublishStage::Rollback) { *static_cast<bool *>(p) = true; throw std::bad_alloc(); }
        if (stage == PublishStage::Native) throw std::bad_alloc();
    }, &recovery};
    EXPECT_EQ(publishAlpha(*desktop, alpha, 1).status, Status::failed); EXPECT_TRUE(recovery); unchanged(1);
}
TEST_F(Publish, T25WorkerPNGPreservesProfileSamplesAndAdmission) {
    open(); FinalGrid source; source.width = source.height = 2; source.dpiX = source.dpiY = 300;
    ASSERT_TRUE(source.pixels.allocate(*budget, Stage::prepared, 4, 4).ok());
    auto rgba = reinterpret_cast<std::uint8_t *>(source.pixels.data());
    for (unsigned i = 0; i < 16; ++i) rgba[i] = i * 17;
    auto profile = cmsCreate_sRGBProfile(); cmsUInt32Number size = 0; cmsSaveProfileToMem(profile, nullptr, &size);
    ASSERT_TRUE(source.profile.allocate(*budget, Stage::prepared, size, 1).ok());
    cmsSaveProfileToMem(profile, source.profile.data(), &size); cmsCloseProfile(profile);
    EXPECT_FALSE(prepareAlpha(source, *budget).ok()); // never encodes on main
    Result<PreparedAlpha> encoded;
    std::thread worker([&] { encoded = prepareAlpha(source, *budget); }); worker.join();
    ASSERT_TRUE(encoded.ok()) << encoded.outcome.diagnostic;
    auto const &png = encoded.value.image.piece(0);
    ValidatedInput input; input.encoded = {png.data, png.size, "image/png"};
    auto decoded = decode(input, *budget); ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value.width, 2u); EXPECT_EQ(decoded.value.height, 2u);
    EXPECT_EQ(std::memcmp(decoded.value.pixels.data(), rgba, 16), 0);
    ASSERT_EQ(decoded.value.profileBytes, size);
    EXPECT_EQ(std::memcmp(decoded.value.profile.data(), source.profile.data(), size), 0);
    auto baseline = budget->reserved(); AllocationFault fault{1}; EncodeOptions options; options.fault = &fault;
    std::thread failed([&] { auto r = prepareAlpha(source, *budget, {}, options); EXPECT_FALSE(r.ok()); }); failed.join();
    EXPECT_EQ(budget->reserved(), baseline);
    Stop stop(std::make_shared<std::atomic<bool>>(true));
    std::thread canceled([&] { EXPECT_EQ(prepareAlpha(source, *budget, stop).outcome.status, Status::canceled); }); canceled.join();
    EXPECT_EQ(budget->reserved(), baseline);
}

TEST_F(Publish, C2ExplodeGroupsContoursUndoRedoAndReopen) {
    open(3, true); auto contours = fitted(); prepared.contours = &contours;
    Fault native{PublishStage::Native}; native.action = [&] {
        auto path = cast<SPPath>(doc->getObjectById("im-contour")); ASSERT_TRUE(path);
        EXPECT_EQ(path->style->stroke.getColor(), *Colors::Color::parse("#ff00ff"));
        EXPECT_EQ(path->style->fill_rule.computed, SP_WIND_RULE_EVENODD);
        auto m = grid.pixelToDocument;
        EXPECT_NEAR(path->style->stroke_width.computed, 0.1*(96.0/25.4)/std::sqrt(std::abs(m[0]*m[3]-m[1]*m[2])), 1e-12);
        ASSERT_TRUE(path->curve());
        EXPECT_TRUE(*path->curve() == Geom::parse_svg_path(path->getRepr()->attribute("d")));
    };
    prepared.hooks = {Fault::call, &native};
    auto result = publishExplode(*desktop, prepared, 1);
    ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    auto selected = desktop->getSelection()->items_vector(); ASSERT_EQ(selected.size(), 3u);
    for (unsigned i = 0; i < selected.size(); ++i) {
        ASSERT_TRUE(is<SPGroup>(selected[i]));
        auto group = selected[i]->getRepr(); auto bitmap = group->firstChild();
        ASSERT_TRUE(bitmap); auto path = bitmap->next(); ASSERT_TRUE(path); EXPECT_EQ(path->next(), nullptr);
        ASSERT_TRUE(is<SPImage>(doc->getObjectByRepr(bitmap)));
        ASSERT_TRUE(is<SPPath>(doc->getObjectByRepr(path)));
        EXPECT_EQ(bitmap->attribute("transform"), nullptr); EXPECT_EQ(path->attribute("transform"), nullptr);
        for (unsigned j = 0; j < 6; ++j) EXPECT_EQ(selected[i]->transform[j], grid.pixelToParent[j]);
        auto parsed = Geom::parse_svg_path(path->attribute("d"));
        EXPECT_EQ(parsed.size(), contours.pieces()[i].ringEnd - contours.pieces()[i].ringBegin);
        for (auto const &ring : parsed) EXPECT_TRUE(ring.closed());
        auto native = cast<SPPath>(doc->getObjectByRepr(path));
        EXPECT_TRUE(native->style->fill.isNone()); EXPECT_FALSE(native->style->stroke.isNone());
        EXPECT_NE(std::string(path->attribute("style")).find("stroke:#ff00ff"), std::string::npos);
        // Independent document-space oracle: use the published native hierarchy,
        // including ancestors, rather than the fixture's grid metric.
        auto m = native->i2doc_affine();
        EXPECT_NEAR(native->style->stroke_width.computed * std::sqrt(std::abs(m.det())) * 25.4/96, 0.1, 1e-9);
        EXPECT_TRUE(query(logicalImageIdentity(*cast<SPImage>(doc->getObjectByRepr(bitmap)))).bypassAlpha);
    }
    EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
    auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
    auto reopened = SPDocument::createNewDocFromMem(after); ASSERT_TRUE(reopened);
    EXPECT_EQ(sp_repr_save_buf(reopened->getReprDoc()).raw(), after);
    EXPECT_TRUE(is<SPPath>(reopened->getObjectById("im-contour")));
}
TEST_F(Publish, C2OrderMismatchRefusesBeforeMutation) {
    open(3); auto contours = fitted(); prepared.contours = &contours;
    // Deliberately corrupt only the public result IDs; no engine/storage edits.
    auto pieces = const_cast<FittedPiece *>(contours.pieces()); std::swap(pieces[0].piece, pieces[1].piece);
    auto revision = doc->getReprDoc()->contentRevision();
    EXPECT_EQ(publishExplode(*desktop, prepared, 1).status, Status::failed);
    EXPECT_EQ(doc->getReprDoc()->contentRevision(), revision); unchanged();
}
TEST_F(Publish, C2NoContourGroupsAndCount) {
    open(2); auto contours = fitted(true); prepared.contours = &contours;
    ASSERT_EQ(contours.ringCount, 0u);
    auto result = publishExplode(*desktop, prepared, 1);
    ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    EXPECT_NE(std::string(result.diagnostic).find("2 pieces without a contour"), std::string::npos);
    for (auto item : desktop->getSelection()->items_vector()) {
        ASSERT_TRUE(is<SPGroup>(item)); auto child = item->getRepr()->firstChild();
        ASSERT_TRUE(child); EXPECT_EQ(child->next(), nullptr);
        EXPECT_TRUE(is<SPImage>(doc->getObjectByRepr(child)));
    }
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
}
TEST_F(Publish, C2ContoursRollbackEveryStage) {
    for (bool only : {false, true}) for (int i = 0; i <= int(PublishStage::Settlement); ++i) {
        SCOPED_TRACE(only);
        SCOPED_TRACE(i);
        open(2); redoBranch(); auto contours = fitted(); prepared.contours = &contours;
        auto baseline = budget->reserved(); Fault fault{static_cast<PublishStage>(i)};
        prepared.hooks = {Fault::call, &fault};
        auto result = only ? publishContourOnly(*desktop, {prepared}, 1) : publishExplode(*desktop, prepared, 1);
        EXPECT_EQ(result.status, Status::failed) << result.diagnostic; EXPECT_TRUE(fault.fired);
        unchanged(1); EXPECT_EQ(budget->reserved(), baseline);
    }
    for (bool only : {false, true}) {
        open(); auto contours = fitted(); prepared.contours = &contours; bool recovery = false;
        prepared.hooks = {[](PublishStage stage, unsigned, void *p) {
            if (stage == PublishStage::Rollback) { *static_cast<bool *>(p) = true; throw std::bad_alloc(); }
            if (stage == PublishStage::Native) throw std::bad_alloc();
        }, &recovery};
        auto result = only ? publishContourOnly(*desktop, {prepared}, 1) : publishExplode(*desktop, prepared, 1);
        EXPECT_EQ(result.status, Status::failed); EXPECT_TRUE(recovery); unchanged();
    }
}
TEST_F(Publish, C2ContourOnlyExactImageZOrderUndoRedoAndReopen) {
    for (bool clip : {false, true}) {
        open(3, true, clip, {}, " transform='rotate(17)' data-custom='preserve'");
        auto contours = fitted(); prepared.contours = &contours; prepared.pieces = nullptr;
        auto originalBytes = sp_repr_write_buf(original, 0, false, Glib::QueryQuark(0u), 0, 0).raw();
        auto next = original->next();
        auto result = publishContourOnly(*desktop, {prepared}, 1);
        ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
        auto item = desktop->getSelection()->singleItem(); ASSERT_TRUE(is<SPGroup>(item));
        auto group = item->getRepr(); EXPECT_EQ(group->parent(), parent);
        EXPECT_EQ(group->prev(), previous); EXPECT_EQ(group->next(), next); EXPECT_EQ(group->attribute("transform"), nullptr);
        auto bitmap = group->firstChild(); ASSERT_TRUE(bitmap); EXPECT_NE(bitmap, original);
        EXPECT_EQ(sp_repr_write_buf(bitmap, 0, false, Glib::QueryQuark(0u), 0, 0).raw(), originalBytes);
        auto path = bitmap->next(); ASSERT_TRUE(path); EXPECT_EQ(path->next(), nullptr);
        auto parsed = Geom::parse_svg_path(path->attribute("d")); EXPECT_EQ(parsed.size(), contours.ringCount);
        for (auto const &ring : parsed) EXPECT_TRUE(ring.closed());
        auto native = cast<SPPath>(doc->getObjectByRepr(path)); ASSERT_TRUE(native);
        EXPECT_NEAR(native->style->stroke_width.computed * std::sqrt(std::abs(native->i2doc_affine().det())) * 25.4/96, 0.1, 1e-9);
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, 1u);
        auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before); EXPECT_EQ(image()->getRepr(), original);
        ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
        auto reopened = SPDocument::createNewDocFromMem(after); ASSERT_TRUE(reopened);
        EXPECT_EQ(sp_repr_save_buf(reopened->getReprDoc()).raw(), after);
    }
}
TEST_F(Publish, C2ContourOnlyTransfersRecipeAndBakedStateUndoRedoAnalyze) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode);
        open(2, true);
        Recipe recipe{130, 41, 6}; remember(sourceIdentity, recipe, mode != 2); recapture();
        auto initial = before;
        if (mode == 1) {
            auto alpha = alphaResult(90);
            ASSERT_EQ(publishAlpha(*desktop, alpha, 1).status, Status::changed);
            recapture();
        }
        auto sourceSettings = query(sourceIdentity); auto applied = before;
        auto sourceBytes = sp_repr_write_buf(original, 0, false, Glib::QueryQuark(0u), 0, 0).raw();
        std::array<unsigned char, 16> pixels;
        std::memcpy(pixels.data(), image()->pixbuf->pixels(), pixels.size());
        auto contours = fitted(); prepared.contours = &contours; prepared.pieces = nullptr;
        ASSERT_EQ(publishContourOnly(*desktop, {prepared}, 2).status, Status::changed);
        ASSERT_TRUE(image()); auto copyNode = image()->getRepr();
        auto copyIdentity = logicalImageIdentity(*image()); EXPECT_FALSE(copyIdentity == sourceIdentity);
        auto checkCopy = [&] {
            ASSERT_TRUE(image()); EXPECT_EQ(image()->getRepr(), copyNode);
            EXPECT_EQ(doc->getResourceList("image").size(), 1u);
            EXPECT_EQ(logicalImageIdentity(*image()), copyIdentity);
            EXPECT_EQ(sp_repr_write_buf(copyNode, 0, false, Glib::QueryQuark(0u), 0, 0).raw(), sourceBytes);
            EXPECT_EQ(std::memcmp(image()->pixbuf->pixels(), pixels.data(), pixels.size()), 0);
            desktop->getSelection()->set(image()); // explicitly reselect the image child
            auto r = query(logicalImageIdentity(*image()));
            EXPECT_EQ(r.threshold, sourceSettings.threshold); EXPECT_EQ(r.softness, sourceSettings.softness);
            EXPECT_EQ(r.faintFloor, sourceSettings.faintFloor); EXPECT_EQ(r.refine, sourceSettings.refine);
            EXPECT_EQ(r.bypassAlpha, sourceSettings.bypassAlpha);
        };
        auto analyze = [&] {
            auto target = resolve(*desktop, Intent::Explode); ASSERT_TRUE(target.ok());
            auto input = std::make_shared<PanelPreparation::Input>();
            input->target = target.value; input->recipe = query(logicalImageIdentity(*image()));
            auto href = getHrefAttribute(*image()->getRepr()).second; ASSERT_TRUE(href);
            gsize length; auto decoded = g_base64_decode(href + 22, &length); g_free(decoded);
            input->decodedBytes = length + 4;
            JobInput job; job.work = PanelPreparation::calculate; job.pixels = 4;
            job.storage.budget = std::make_shared<Budget>(Budget::FixedLimitForTest{}, 1536*MiB);
            job.storage.bytes.assign(href + 22, href + std::strlen(href)); job.storage.payload = input;
            ASSERT_TRUE(job.storage.budget->acquire(Stage::input, job.storage.bytes.size()+sizeof(*input)+256, job.storage.reservation).ok());
            recordBitmapMainThread(); bool delivered = false; JobResult result;
            BitmapJobs jobs([&](Ticket, JobResult r) { result = std::move(r); delivered = true; }, {}, JobClock::now, false);
            jobs.request(std::move(job)); auto deadline = JobClock::now() + std::chrono::seconds(10);
            while ((!delivered || jobs.active()) && JobClock::now() < deadline) {
                jobs.poll(); Glib::MainContext::get_default()->iteration(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            jobs.close(); ASSERT_TRUE(delivered); ASSERT_TRUE(result.ok()) << result.outcome.diagnostic;
            auto output = std::dynamic_pointer_cast<PanelPreparation::Output const>(result.value.payload); ASSERT_TRUE(output);
            ASSERT_EQ(output->proxyWidth, 2u); ASSERT_EQ(output->proxyHeight, 2u);
            ASSERT_EQ(output->proxy.size(), 16u);
            for (unsigned i = 0; i < 4; ++i) EXPECT_EQ(std::to_integer<unsigned>(output->proxy.data()[4*i+3]), 90u);
            EXPECT_FALSE(output->adjustment); // baked input requires no further Apply payload
            EXPECT_EQ(output->lost, 0u); // Analyze must not reapply T/S to baked alpha 90.
            EXPECT_EQ(std::memcmp(image()->pixbuf->pixels(), pixels.data(), pixels.size()), 0);
        };
        checkCopy(); if (mode == 1) analyze();
        auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
        EXPECT_EQ(preflightUndo(*doc, {false, 0, 1}).usage.undoCount, mode == 1 ? 2u : 1u);
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), applied); ASSERT_TRUE(image());
        EXPECT_EQ(image()->getRepr(), original); EXPECT_EQ(logicalImageIdentity(*image()), sourceIdentity);
        EXPECT_EQ(query(sourceIdentity).bypassAlpha, sourceSettings.bypassAlpha);
        EXPECT_EQ(query(copyIdentity).threshold, 128u); EXPECT_FALSE(query(copyIdentity).bypassAlpha);
        if (mode == 1) {
            ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
            EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), initial); EXPECT_FALSE(query(sourceIdentity).bypassAlpha);
            ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
            EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), applied); EXPECT_TRUE(query(sourceIdentity).bypassAlpha);
        }
        ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
        checkCopy(); if (mode == 1) analyze();
    }
}
TEST_F(Publish, C2ContourOnlyNativeHrefMismatchRollsBack) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode);
        open(2, true, false, {}, mode == 2 ? " xlink:href='" + alphaPng(20) + "'" : "", mode == 1);
        auto contours = fitted(); prepared.contours = &contours;
        auto sourceHref = std::string(image()->href);
        Fault foreign{PublishStage::Native}; foreign.action = [&] {
            ASSERT_TRUE(image()); auto revision = doc->getReprDoc()->contentRevision();
            auto bytes = sp_repr_save_buf(doc->getReprDoc()).raw();
            g_free(image()->href); image()->href = g_strdup(alphaPng(30).c_str());
            EXPECT_EQ(doc->getReprDoc()->contentRevision(), revision);
            EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), bytes);
        };
        prepared.hooks = {Fault::call, &foreign};
        EXPECT_EQ(publishContourOnly(*desktop, {prepared}, 1).status, Status::failed);
        EXPECT_TRUE(foreign.fired); unchanged(); EXPECT_STREQ(image()->href, sourceHref.c_str());
    }
}
TEST_F(Publish, C2NullContoursByteIdenticalLegacyStaging) {
    open(2); ASSERT_EQ(prepared.contours, nullptr);
    auto expected = SPDocument::createNewDocFromMem(before); auto old = expected->getObjectById("im")->getRepr();
    auto container = old->parent(); auto prev = old->prev(); GC::anchor(old); container->removeChild(old); GC::release(old);
    char matrix[256]; auto size = serializeGridTransform(grid.pixelToParent, matrix, sizeof(matrix));
    for (unsigned i = 0; i < pieces.count(); ++i) {
        auto const &piece = pieces.piece(i); auto node = expected->getReprDoc()->createElement("svg:image");
        node->setAttribute("id", i ? "im-piece-2" : "im");
        node->setAttribute("inkscape:label", "Original — piece " + std::to_string(i+1));
        node->setAttribute("x", std::to_string(piece.x)); node->setAttribute("y", std::to_string(piece.y));
        node->setAttribute("width", std::to_string(piece.width)); node->setAttribute("height", std::to_string(piece.height));
        node->setAttribute("preserveAspectRatio", "none"); node->setAttribute("transform", std::string(matrix, size));
        auto base64 = g_base64_encode(piece.data, piece.size);
        node->setAttribute("xlink:href", std::string("data:image/png;base64,")+base64); g_free(base64);
        container->addChild(node, prev); prev = node; GC::release(node);
    }
    auto result = publishExplode(*desktop, prepared, 1); ASSERT_EQ(result.status, Status::changed);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), sp_repr_save_buf(expected->getReprDoc()).raw());
}

TEST_F(Publish, C2CompoundHolesAndCubicRoundTripPrecision) {
    open(); pieces.reset(); grid.pixels.reset(); grid.width = grid.height = 24;
    ASSERT_TRUE(grid.pixels.allocate(*budget, Stage::prepared, 24*24, 4).ok());
    std::memset(grid.pixels.data(), 0, grid.pixels.size());
    auto pixels = reinterpret_cast<std::uint8_t *>(grid.pixels.data());
    for (unsigned y = 2; y < 22; ++y) for (unsigned x = 2; x < 22; ++x)
        if (x < 7 || x >= 17 || y < 7 || y >= 17) pixels[(y*24+x)*4+3] = 255;
    JobWork work(25000000);
    auto regions = label(grid.view(), alphaLut(0, 0), *budget, work); ASSERT_TRUE(regions.ok());
    auto partition = enclose(regions.value, *budget, work); ASSERT_TRUE(partition.ok());
    auto encoded = encode(grid, partition.value, *budget); ASSERT_TRUE(encoded.ok()); pieces = std::move(encoded.value);
    auto contours = fitted(); ASSERT_EQ(contours.pieceCount, 1u); ASSERT_EQ(contours.ringCount, 2u);
    // Exercise every serializer command with binary64 values beyond six decimals.
    auto segment = const_cast<ContourSegment *>(contours.segments());
    auto point = contours.rings()[0].start;
    segment[0].cubic = true;
    segment[0].c1 = {point.x + 0.1234567890123456, point.y - 0.9876543210987654};
    segment[0].c2 = {point.x + 0.2345678901234567, point.y - 0.8765432109876543};
    prepared.contours = &contours;
    auto result = publishExplode(*desktop, prepared, 1); ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    auto path = doc->getObjectById("im-contour")->getRepr(); auto d = std::string(path->attribute("d"));
    EXPECT_NE(d.find("C "), std::string::npos); EXPECT_NE(d.find("L "), std::string::npos);
    auto parsed = Geom::parse_svg_path(d.c_str()); ASSERT_EQ(parsed.size(), 2u);
    auto cubic = dynamic_cast<Geom::CubicBezier const *>(&parsed[0][0]); ASSERT_TRUE(cubic);
    EXPECT_EQ((*cubic)[1][0], segment[0].c1.x); EXPECT_EQ((*cubic)[1][1], segment[0].c1.y);
    EXPECT_EQ((*cubic)[2][0], segment[0].c2.x); EXPECT_EQ((*cubic)[2][1], segment[0].c2.y);
    bool hole = false;
    for (unsigned i = 0; i < parsed.size(); ++i) {
        auto const &ring = parsed[i]; EXPECT_TRUE(ring.closed());
        // Green's theorem with three-point Gauss quadrature: exact for the
        // serialized line/cubic polynomial integrand (degree at most five).
        double area = 0;
        auto q = std::sqrt(3.0 / 5.0);
        double times[] = {(1-q)/2, 0.5, (1+q)/2};
        double weights[] = {5.0/18, 4.0/9, 5.0/18};
        for (auto curve = ring.begin(); curve != ring.end_closed(); ++curve) for (unsigned j = 0; j < 3; ++j) {
            auto p = curve->pointAndDerivatives(times[j], 1);
            area += weights[j] * (p[0][0]*p[1][1] - p[0][1]*p[1][0]) / 2;
        }
        EXPECT_NE(area, 0); EXPECT_NE(contours.rings()[i].area, 0);
        EXPECT_EQ(std::signbit(area), std::signbit(contours.rings()[i].area));
        if (contours.rings()[i].depth % 2) {
            hole = true;
            EXPECT_NE(std::signbit(area), std::signbit(contours.rings()[0].area));
        }
    }
    EXPECT_TRUE(hole);
}
TEST_F(Publish, C2NativeAndXMLTamperingRollBack) {
    for (bool only : {false, true}) for (unsigned mode = 0; mode < 4; ++mode) {
        SCOPED_TRACE(only);
        SCOPED_TRACE(mode);
        open(); auto contours = fitted(); prepared.contours = &contours;
        Fault foreign{PublishStage::Native}; foreign.action = [&] {
            auto path = cast<SPPath>(doc->getObjectById("im-contour")); ASSERT_TRUE(path);
            if (mode == 0) path->style->stroke_width.computed = 999;
            if (mode == 1) path->transform *= Geom::Translate(5, 3);
            if (mode == 2) cast<SPItem>(path->parent)->transform *= Geom::Translate(5, 3);
            if (mode == 3) path->getRepr()->setAttribute("d", "M0,0L1,1Z");
        };
        prepared.hooks = {Fault::call, &foreign};
        auto result = only ? publishContourOnly(*desktop, {prepared}, 1) : publishExplode(*desktop, prepared, 1);
        EXPECT_EQ(result.status, Status::failed); EXPECT_TRUE(foreign.fired); unchanged();
    }
}
TEST_F(Publish, C2ContourOnlyNoContourIsReadOnly) {
    open(); auto contours = fitted(true); prepared.contours = &contours;
    auto revision = doc->getReprDoc()->contentRevision();
    EXPECT_EQ(publishContourOnly(*desktop, {prepared}, 1).status, Status::unchanged);
    EXPECT_EQ(doc->getReprDoc()->contentRevision(), revision); unchanged();
}

TEST_F(Publish, C2FiftyPieceGroupsRespectProductCap) {
    open(50); auto contours = fitted(); prepared.contours = &contours;
    prepared.limits = measuredLimits(diagnosticDependencyEvidence());
    auto result = publishExplode(*desktop, prepared, 1);
    ASSERT_EQ(result.status, Status::changed) << result.diagnostic;
    EXPECT_EQ(desktop->getSelection()->size(), 50u);
    for (auto item : desktop->getSelection()->items_vector()) EXPECT_TRUE(is<SPGroup>(item));
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
}
} // namespace
