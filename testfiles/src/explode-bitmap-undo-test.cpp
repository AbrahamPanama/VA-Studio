// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Tests for Explode Bitmap (Undo): T01 Undo capacity and T07 history faults for the EB5-undo preflight.
 * The history is real (DocumentUndo::done with known payload sizes); the oracle is the document state:
 * Undo/Redo stack rows and serials, attribute values and the Redo ability, all unchanged after every call.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <cstdlib>
#include <new>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "document-undo.h"
#include "desktop.h"
#include "inkscape-application.h"
#include "object/sp-root.h"
#include "selection.h"
#include "xml/repr.h"
#include "undo-stack-observer.h"
#include "document.h"
#include "event-log.h"
#include "inkscape.h"
#include "preferences.h"
#include "ui/explode-bitmap-undo.h"
#include "util/bitmap-memory-admission.h"
#include "xml/attribute-record.h"
#include "xml/document.h"
#include "xml/node.h"

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

using namespace Inkscape::Bitmap;
using ContextString = Inkscape::Util::Internal::ContextString;
using Inkscape::DocumentUndo;

constexpr std::uint64_t MiB = 1024 * 1024;
constexpr char const *kSvg = "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'/>";

class ExplodeBitmapUndo : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        g_setenv("INKSCAPE_APP_ID_TAG", "eb5undofault", TRUE);
        static auto *app = new InkscapeApplication;
        ASSERT_TRUE(app->gtk_app());
        if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    }

    void SetUp() override
    {
        prefs = Inkscape::Preferences::get();
        oldLimit = prefs->getBool("/options/undo/limit", true);
        oldSize = prefs->getInt("/options/undo/size", 200);
        oldMb = prefs->getInt("/options/undo/max-mb", 1024);
        setLimits(true, 200, 1024);
        doc = SPDocument::createNewDocFromMem(std::span<char const>(kSvg, std::strlen(kSvg)));
        ASSERT_NE(doc, nullptr);
        doc->ensureUpToDate();
        DocumentUndo::setUndoSensitive(doc.get(), true);
    }
    void TearDown() override
    {
        DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
        failNextAllocation = false;
        prefs->setBool("/options/undo/limit", oldLimit);
        prefs->setInt("/options/undo/size", oldSize);
        prefs->setInt("/options/undo/max-mb", oldMb);
    }

    void setLimits(bool limit, int size, int mb)
    {
        prefs->setBool("/options/undo/limit", limit);
        prefs->setInt("/options/undo/size", size);
        prefs->setInt("/options/undo/max-mb", mb);
    }
    // One history entry whose payload is exactly `bytes` (a new attribute: old value absent, new value `bytes`).
    void edit(std::size_t bytes)
    {
        doc->getReprRoot()->setAttribute("data-eb" + std::to_string(++edits), std::string(bytes, 'x'));
        DocumentUndo::done(doc.get(), ContextString{"eb edit"}, "");
    }

    struct Snapshot {
        std::size_t rows = 0;
        std::uint64_t serial = 0;
        int attrs = 0;
        bool modified = false;
        bool operator==(Snapshot const &o) const
        {
            return rows == o.rows && serial == o.serial && attrs == o.attrs && modified == o.modified;
        }
    };
    Snapshot snap()
    {
        Snapshot s;
        auto *log = doc->get_event_log();
        for (auto p = log->getEventListStore()->children().begin(); p != log->getEventListStore()->children().end(); ++p) {
            ++s.rows;
            s.rows += p->children().size();
        }
        s.serial = log->getCurrEventSerial();
        for (auto const &a : doc->getReprRoot()->attributeList()) { (void)a; ++s.attrs; }
        s.modified = doc->isModifiedSinceSave();
        return s;
    }
    UndoAdmission run(EventPayloads p)
    {
        auto const before = snap();
        auto a = preflightUndo(*doc, p);
        EXPECT_TRUE(before == snap()) << "preflight changed history";
        return a;
    }
    UndoAdmission direct(std::uint64_t n) { return run({false, 0, n}); }
    UndoAdmission pair(std::uint64_t conv, std::uint64_t ex) { return run({true, conv, ex}); }

    Inkscape::Preferences *prefs = nullptr;
    bool oldLimit = true;
    int oldSize = 200, oldMb = 1024;
    std::unique_ptr<SPDocument> doc;
    int edits = 0;
};

// ---- T01 ----------------------------------------------------------------------------------------------

TEST_F(ExplodeBitmapUndo, ReadsRetainedBytesCountsAndRedoFromRealHistory)
{
    edit(1000);
    edit(2000);
    edit(3000);
    auto a = direct(10);
    EXPECT_EQ(a.usage.undoBytes, 6000u);
    EXPECT_EQ(a.usage.undoCount, 3u);
    EXPECT_EQ(a.usage.redoBytes, 0u);
    EXPECT_EQ(a.usage.redoCount, 0u);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    a = direct(10);
    EXPECT_EQ(a.usage.undoBytes, 3000u);
    EXPECT_EQ(a.usage.undoCount, 2u);
    EXPECT_EQ(a.usage.redoBytes, 3000u);
    EXPECT_EQ(a.usage.redoCount, 1u);
    EXPECT_EQ(a.redoTermBytes, 3000u);
    EXPECT_EQ(a.historyTermBytes, 3010u);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    a = direct(10); // everything undone: all Redo
    EXPECT_EQ(a.usage.undoCount, 0u);
    EXPECT_EQ(a.usage.redoCount, 3u);
    EXPECT_EQ(a.usage.redoBytes, 6000u);
}

TEST_F(ExplodeBitmapUndo, NonEmptyRedoIsReportedUnchangedAndRedoStillWorks)
{
    edit(5000);
    edit(7000);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    auto const before = snap();
    auto a = direct(100);
    EXPECT_TRUE(a.admitted);
    EXPECT_EQ(a.usage.redoCount, 1u);
    EXPECT_EQ(a.usage.redoBytes, 7000u);
    EXPECT_TRUE(before == snap());
    // A refusal must not drop Redo either.
    a = direct(500 * MiB);
    EXPECT_FALSE(a.admitted);
    EXPECT_TRUE(before == snap());
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); // Redo survived both calls
    EXPECT_EQ(doc->getReprRoot()->attribute("data-eb2") != nullptr, true);
}

TEST_F(ExplodeBitmapUndo, FortyPercentBoundaryPlusMinusOneByte)
{
    setLimits(true, 200, 1); // U = 1 MiB, floor(0.4 U) = 419430
    EXPECT_TRUE(direct(419430).admitted);
    auto a = direct(419431);
    EXPECT_FALSE(a.admitted);
    EXPECT_EQ(a.reason, UndoRefusal::payloadTooLarge);
    EXPECT_EQ(a.maxNewBytes, 419430u);
    EXPECT_TRUE(direct(419429).admitted);
    // The limit also binds the 64 MiB setting.
    setLimits(true, 200, 64);
    auto const limit = 64 * MiB * 2 / 5;
    EXPECT_TRUE(direct(limit).admitted);
    EXPECT_EQ(direct(limit + 1).reason, UndoRefusal::payloadTooLarge);
}

TEST_F(ExplodeBitmapUndo, RetainedPlusNewAboveBudgetIsAdmittedAndNativeTrimmingKeepsTheNewPair)
{
    setLimits(true, 200, 1); // U = 1,048,576
    edit(300000);
    edit(300000);
    edit(300000);
    // 900,000 retained + 200,000 new > U, but every new payload is within 40%: admitted, term capped at U.
    auto a = pair(100000, 100000);
    ASSERT_TRUE(a.admitted);
    EXPECT_EQ(a.usage.undoBytes, 900000u);
    EXPECT_EQ(a.historyTermBytes, MiB);
    ResourcePlan plan;
    ASSERT_TRUE(addUndoTerms(plan, a, 0, 0));
    EXPECT_EQ(plan.items[0].term, Term::history);
    EXPECT_EQ(plan.items[0].bytes, MiB);
    // Below U the term is the plain sum.
    EXPECT_EQ(direct(100000).historyTermBytes, 1000000u);
    // Commit the pair through the real history: the oldest entry expires, both new entries stay.
    edit(100000);
    edit(100000);
    auto const after = direct(1);
    EXPECT_EQ(after.usage.undoCount, 4u);     // 5 committed, oldest expired
    EXPECT_EQ(after.usage.undoBytes, 800000u); // 300k + 300k + 100k + 100k
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(doc->getReprRoot()->attribute("data-eb5"), nullptr); // both new entries were recoverable
    EXPECT_EQ(doc->getReprRoot()->attribute("data-eb4"), nullptr);
    EXPECT_NE(doc->getReprRoot()->attribute("data-eb3"), nullptr);
}

TEST_F(ExplodeBitmapUndo, PairFitsOnlyAsAPair)
{
    setLimits(true, 200, 1);
    // Each entry alone is under 0.4 U, together they are not.
    EXPECT_TRUE(direct(300000).admitted);
    EXPECT_TRUE(pair(300000, 0).admitted);
    auto a = pair(300000, 300000);
    EXPECT_FALSE(a.admitted);
    EXPECT_EQ(a.reason, UndoRefusal::payloadTooLarge);
    EXPECT_EQ(a.newBytes, 600000u);
    // Combined payload on the 40% line: exactly fits, +1 does not.
    EXPECT_TRUE(pair(200000, 219430).admitted);
    EXPECT_FALSE(pair(200000, 219431).admitted);
    // Retained history does not change the 40% decision.
    edit(700000);
    EXPECT_TRUE(pair(200000, 219430).admitted);
    EXPECT_EQ(pair(200000, 219431).reason, UndoRefusal::payloadTooLarge);
}

TEST_F(ExplodeBitmapUndo, CountCapPlusMinusOne)
{
    setLimits(true, 2, 1024);
    EXPECT_TRUE(pair(10, 10).admitted);   // cap == entries
    setLimits(true, 1, 1024);
    auto a = pair(10, 10);                // cap == entries - 1
    EXPECT_FALSE(a.admitted);
    EXPECT_EQ(a.reason, UndoRefusal::countCap);
    EXPECT_TRUE(direct(10).admitted);     // a single entry still fits a cap of 1
    setLimits(true, 3, 1024);
    edit(10); edit(10); edit(10);         // history already full: normal oldest-first expiry, new entries retained
    a = pair(10, 10);
    EXPECT_TRUE(a.admitted);
    EXPECT_EQ(a.usage.undoCount, 3u);
    EXPECT_EQ(a.countCap, 3u);
    setLimits(true, 0, 1024);
    EXPECT_EQ(direct(10).reason, UndoRefusal::countCapInvalid);
    setLimits(true, -5, 1024);
    EXPECT_EQ(pair(10, 10).reason, UndoRefusal::countCapInvalid);
}

TEST_F(ExplodeBitmapUndo, DisabledOrNonpositiveLimitsUseTheFeatureCap)
{
    auto const cap = 1024 * MiB;
    auto const forty = cap * 2 / 5;
    // Limiting disabled: size 1 and max-mb 1 are ignored; U = 1,024 MiB, no count cap.
    setLimits(false, 1, 1);
    auto a = pair(10, 10);
    EXPECT_TRUE(a.admitted);
    EXPECT_EQ(a.budgetBytes, cap);
    EXPECT_EQ(a.countCap, 0u);
    EXPECT_TRUE(direct(forty).admitted);
    EXPECT_EQ(direct(forty + 1).reason, UndoRefusal::payloadTooLarge);
    EXPECT_EQ(prefs->getInt("/options/undo/max-mb", -1), 1); // preference untouched
    // Enabled with nonpositive budget: feature cap.
    setLimits(true, 200, 0);
    EXPECT_EQ(direct(10).budgetBytes, cap);
    setLimits(true, 200, -7);
    EXPECT_EQ(direct(10).budgetBytes, cap);
    // Above 1,024 MiB is clamped; 1,024 and exactly 1,023 follow the setting.
    setLimits(true, 200, 100000);
    EXPECT_EQ(direct(10).budgetBytes, cap);
    setLimits(true, 200, 1023);
    EXPECT_EQ(direct(10).budgetBytes, 1023 * MiB);
    setLimits(true, 200, 1024);
    EXPECT_EQ(direct(10).budgetBytes, cap);
}

TEST_F(ExplodeBitmapUndo, PreflightNeverTrimsHistoryEvenWhenRefusing)
{
    setLimits(true, 3, 1);
    edit(300000);
    edit(300000);
    edit(300000);
    auto const before = snap();
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(pair(300000, 300000).admitted); // 600000 > 0.4 U
        EXPECT_TRUE(before == snap());
    }
    EXPECT_EQ(direct(1).usage.undoCount, 3u);
    EXPECT_EQ(direct(1).usage.undoBytes, 900000u);
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(DocumentUndo::undo(doc.get())); // all three still undoable
    EXPECT_EQ(doc->getReprRoot()->attribute("data-eb1"), nullptr);
}

TEST_F(ExplodeBitmapUndo, AddsHistoryAndRedoTermsToAnEb4Plan)
{
    edit(1000);
    edit(2000);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    auto a = pair(40, 60);
    ASSERT_TRUE(a.admitted);
    ResourcePlan plan;
    ASSERT_TRUE(addUndoTerms(plan, a, 0, 0));
    ASSERT_EQ(plan.count, 2u);
    EXPECT_EQ(plan.items[0].term, Term::history);
    EXPECT_EQ(plan.items[0].bytes, 1100u);
    EXPECT_EQ(plan.items[1].term, Term::redo);
    EXPECT_EQ(plan.items[1].bytes, 2000u);
    ResourcePlan refused;
    EXPECT_FALSE(addUndoTerms(refused, direct(500 * MiB), 0, 0));
    EXPECT_EQ(refused.count, 0u);
}

// ---- T01 settlement faults and T07 history faults, injected through the source seam ----------------------

struct FakeHistory final : UndoHistorySource {
    HistoryUsage usage;
    UndoLimits limits;
    bool usageOk = true, limitsOk = true, settleOk = true;
    mutable int settleCalls = 0;
    mutable std::uint64_t settleEntries = 0;
    bool readUsage(HistoryUsage &o) const noexcept override { if (usageOk) o = usage; return usageOk; }
    bool readLimits(UndoLimits &o) const noexcept override { if (limitsOk) o = limits; return limitsOk; }
    bool reserveSettlement(std::uint64_t n) const noexcept override { ++settleCalls; settleEntries = n; return settleOk; }
};

TEST(ExplodeBitmapUndoFaults, SettlementFaultRefusesAfterCapacityPassesAndAsksForThePair)
{
    FakeHistory h;
    auto a = preflightUndo(h, {true, 1, 1});
    EXPECT_TRUE(a.admitted);
    EXPECT_EQ(h.settleCalls, 1);
    EXPECT_EQ(h.settleEntries, 2u); // the pair is reserved together, one probe
    h.settleOk = false;
    a = preflightUndo(h, {true, 1, 1});
    EXPECT_FALSE(a.admitted);
    EXPECT_EQ(a.reason, UndoRefusal::settlementUnavailable);
    h.settleCalls = 0;
    h.settleOk = false;
    a = preflightUndo(h, {false, 0, std::uint64_t{500} * MiB}); // capacity refusal comes first, no probe
    EXPECT_EQ(a.reason, UndoRefusal::payloadTooLarge);
    EXPECT_EQ(h.settleCalls, 0);
}

TEST(ExplodeBitmapUndoFaults, HistoryAndLimitReadFaultsRefuse)
{
    FakeHistory h;
    h.usageOk = false;
    auto a = preflightUndo(h, {false, 0, 1});
    EXPECT_FALSE(a.admitted);
    EXPECT_EQ(a.reason, UndoRefusal::historyUnreadable);
    h.usageOk = true;
    h.limitsOk = false;
    a = preflightUndo(h, {false, 0, 1});
    EXPECT_EQ(a.reason, UndoRefusal::historyUnreadable);
    EXPECT_STRNE(a.diagnostic, "");
}

TEST(ExplodeBitmapUndoFaults, CheckedArithmeticRefusesInsteadOfWrapping)
{
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    FakeHistory h;
    auto a = preflightUndo(h, {true, max, 1});
    EXPECT_EQ(a.reason, UndoRefusal::arithmetic);
    a = preflightUndo(h, {true, max - 1, 1}); // sum fits u64 but is far above 40% U
    EXPECT_EQ(a.reason, UndoRefusal::payloadTooLarge);
    h.usage.undoBytes = max;
    a = preflightUndo(h, {false, 0, 1});
    EXPECT_EQ(a.reason, UndoRefusal::arithmetic);
    h.usage.undoBytes = 1024 * MiB - 100;
    h.usage.redoBytes = max;
    a = preflightUndo(h, {false, 0, 100});
    EXPECT_EQ(a.reason, UndoRefusal::arithmetic);
    h.usage.redoBytes = 0;
    h.limits.maxMb = std::numeric_limits<std::int64_t>::max();
    EXPECT_EQ(preflightUndo(h, {false, 0, 1}).budgetBytes, undoFeatureCapBytes);
    h.limits.maxMb = 1;
    h.usage.undoBytes = 1; // conversion bytes are ignored when there is no conversion step
    EXPECT_TRUE(preflightUndo(h, {false, max, 1}).admitted);
}

TEST_F(ExplodeBitmapUndo, DocumentWithoutUsableHistoryDoesNotThrow)
{
    // A fresh document: empty history is readable and admits.
    auto a = direct(1);
    EXPECT_TRUE(a.admitted);
    EXPECT_EQ(a.usage.undoCount, 0u);
    EXPECT_EQ(a.usage.redoCount, 0u);
    EXPECT_STREQ(undoRefusalName(UndoRefusal::payloadTooLarge), "new payload above 40% of the Undo budget");
}

// Real C++ allocation failures at the two settlement preparation boundaries.
using SettlementStage = DocumentUndo::AtomicSettlementStage;
SettlementStage failingStage;
bool allocationAtStage(SettlementStage stage) {
    if (stage == failingStage) failNextAllocation = true;
    return false;
}
TEST_F(ExplodeBitmapUndo, AtomicSettlementAllocationFailuresKeepRedoAndRollbackExactly)
{
    for (auto stage : {SettlementStage::EventConstruction, SettlementStage::HistoryInsertion}) {
        SCOPED_TRACE(static_cast<int>(stage));
        edit(100); edit(200);
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        doc->setModifiedSinceSave(false);
        SPDesktop desktop(doc->getNamedView());
        desktop.getSelection()->set(doc->getRoot());
        auto const selected = desktop.getSelection()->items_vector();
        auto const baseline = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto const before = snap();
        auto const usage = direct(1).usage;
        auto const top = DocumentUndo::undoStackMark(doc.get());
        auto token = DocumentUndo::beginAtomicInteraction(doc.get()); ASSERT_TRUE(token);
        doc->getReprRoot()->setAttribute("data-atomic-fault", "pending");
        failingStage = stage;
        DocumentUndo::setAtomicSettlementFaultForTesting(allocationAtStage);
        EXPECT_THROW(token->commitAtomically(ContextString("Atomic"), "", [] { return true; }), std::bad_alloc);
        DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
        EXPECT_TRUE(token->active()); EXPECT_EQ(DocumentUndo::undoStackMark(doc.get()), top);
        EXPECT_EQ(direct(1).usage.undoCount, usage.undoCount); EXPECT_EQ(direct(1).usage.redoCount, usage.redoCount);
        token->rollback();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), baseline);
        EXPECT_TRUE(before == snap()); EXPECT_EQ(DocumentUndo::undoStackMark(doc.get()), top);
        EXPECT_EQ(direct(1).usage.undoBytes, usage.undoBytes); EXPECT_EQ(direct(1).usage.redoBytes, usage.redoBytes);
        EXPECT_EQ(desktop.getSelection()->items_vector(), selected);
        ASSERT_TRUE(DocumentUndo::redo(doc.get()));
        EXPECT_NE(doc->getReprRoot()->attribute(("data-eb" + std::to_string(edits)).c_str()), nullptr);
    }
}
// Ordinary done() retains native notification order and EventLog saved-row tracking.
struct RecordingHistoryObserver final : Inkscape::UndoStackObserver {
    std::vector<std::string> notifications;
    void notifyUndoEvent(Inkscape::Event *) override { notifications.emplace_back("undo"); }
    void notifyRedoEvent(Inkscape::Event *) override { notifications.emplace_back("redo"); }
    void notifyUndoCommitEvent(Inkscape::Event *) override { notifications.emplace_back("commit"); }
    void notifyUndoExpired(Inkscape::Event *) override { notifications.emplace_back("expired"); }
    void notifyClearUndoEvent() override { notifications.emplace_back("clear-undo"); }
    void notifyClearRedoEvent() override { notifications.emplace_back("clear-redo"); }
};
TEST_F(ExplodeBitmapUndo, OrdinaryDonePreservesHistoryRedoAndEventLog)
{
    edit(100); edit(200);
    auto *log = doc->get_event_log();
    auto const droppedSerial = log->getCurrEventSerial();
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    auto const savedSerial = log->getCurrEventSerial();
    log->rememberFileSave(savedSerial);
    doc->setModifiedSinceSave(false);
    auto const baseline = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto const before = snap();
    auto const usage = direct(1).usage;
    ASSERT_EQ(usage.redoCount, 1u);
    RecordingHistoryObserver observer;
    doc->addUndoObserver(observer);
    struct Registration {
        SPDocument *document;
        Inkscape::UndoStackObserver &observer;
        ~Registration() { document->removeUndoObserver(observer); }
    } registration{doc.get(), observer};

    doc->getReprRoot()->setAttribute("data-ordinary", "ordinary");
    DocumentUndo::done(doc.get(), ContextString("Ordinary edit"), "ordinary-icon");
    EXPECT_EQ(observer.notifications, (std::vector<std::string>{"clear-redo", "commit"}));
    auto const after = snap();
    EXPECT_EQ(after.rows, before.rows); // new row replaces the discarded Redo row
    EXPECT_GT(after.serial, droppedSerial);
    EXPECT_TRUE(after.modified);
    auto const committed = direct(1).usage;
    EXPECT_EQ(committed.undoCount, usage.undoCount + 1);
    EXPECT_EQ(committed.undoBytes, usage.undoBytes + std::strlen("ordinary"));
    EXPECT_EQ(committed.redoCount, 0u);
    EXPECT_EQ(committed.redoBytes, 0u);
    EXPECT_FALSE(log->findEventBySerial(droppedSerial));
    auto const row = log->getCurrEvent();
    EXPECT_EQ(Glib::ustring((*row)[log->getColumns().description]), "Ordinary edit");
    EXPECT_EQ(Glib::ustring((*row)[log->getColumns().icon_name]), "ordinary-icon");
    EXPECT_EQ(static_cast<Inkscape::Event *>((*row)[log->getColumns().event]), DocumentUndo::undoStackMark(doc.get()));
    EXPECT_TRUE(doc->getReprDoc()->inTransaction());

    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), baseline);
    EXPECT_EQ(log->getCurrEventSerial(), savedSerial);
    EXPECT_FALSE(doc->isModifiedSinceSave());
    auto const undone = snap();
    auto const notifications = observer.notifications;
    DocumentUndo::done(doc.get(), ContextString("No edit"), "");
    EXPECT_TRUE(undone == snap());
    EXPECT_EQ(observer.notifications, notifications);
    EXPECT_EQ(direct(1).usage.redoCount, 1u); // no-op done() preserves Redo
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(doc->getReprRoot()->attribute("data-ordinary"), "ordinary");
    EXPECT_EQ(log->getCurrEventSerial(), after.serial);
    EXPECT_TRUE(doc->isModifiedSinceSave());
    EXPECT_EQ(observer.notifications, (std::vector<std::string>{"clear-redo", "commit", "undo", "redo"}));
}

} // namespace
