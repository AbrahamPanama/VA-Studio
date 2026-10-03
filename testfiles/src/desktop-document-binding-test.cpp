// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Readiness probe for the now-present SPDesktop::documentBindingGeneration()
 * accessor: no stamp throughout a document change, a fresh stamp once it
 * settles, stable under same-document reads.
 * Fixture startup/teardown follows namedview-window-state-test.cpp.
 */

#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <gtkmm/window.h>
#include <2geom/point.h>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sigc++/scoped_connection.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "object/sp-item.h"
#include "object/sp-object.h"
#include "object/sp-text.h"
#include "selection.h"
#include "ui/tools/nesting-tool.h"
#include "ui/text-font-preview.h"
#include "util/scope_exit.h"
#include "xml/attribute-record.h"
#include "xml/document.h"
#include "xml/node.h"

namespace {
class TestApplication : public InkscapeApplication {};

InkscapeApplication &testApplication()
{
    static auto application = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "desktopdocumentbindingtest", true);
        auto result = new TestApplication();
        result->gio_app()->register_application();
        // Preserve fatal failures as test failures rather than the application's
        // interactive emergency dialog.
        for (auto signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) {
            std::signal(signal, SIG_DFL);
        }
#ifndef _WIN32
        std::signal(SIGBUS, SIG_DFL);
#endif
        return result;
    }();
    return *application;
}
void drainMainContext()
{
    for (unsigned i = 0; i < 10000 && g_main_context_pending(nullptr); ++i) {
        g_main_context_iteration(nullptr, false);
    }
}
// Synthetic documents: a titled, non-empty svg with one known rect each.
constexpr char kSvgA[] =
    "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
    "<title>Document A</title>"
    "<rect id='rectA' x='10' y='10' width='5' height='5'/>"
    "</svg>";
constexpr char kSvgB[] =
    "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
    "<title>Document B</title>"
    "<rect id='rectB' x='20' y='20' width='5' height='5'/>"
    "</svg>";
// Raw, nonmutating XML fingerprint reused from namedview-window-state-test.cpp.
// Serialization (sp_repr_save_buf) mutates the live document, so it is never used.
void appendField(std::string &out, char const *text)
{
    if (!text) {
        out += "-;";
        return;
    }
    out += std::to_string(std::strlen(text)) + ":" + text + ";";
}
void fingerprintNode(Inkscape::XML::Node const *node, std::string &out)
{
    out += "[" + std::to_string(static_cast<int>(node->type()));
    appendField(out, node->name());
    appendField(out, node->content());
    for (auto const &attr : node->attributeList()) {
        appendField(out, g_quark_to_string(attr.key));
        appendField(out, static_cast<char const *>(attr.value));
    }
    out += ";";
    for (auto child = node->firstChild(); child; child = child->next()) {
        fingerprintNode(child, out);
    }
    out += "]";
}
std::string fingerprint(SPDocument &document)
{
    std::string out;
    fingerprintNode(document.getReprDoc(), out);
    return out;
}

class DesktopDocumentBindingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "GUI testing not enabled";
        }
        // Only a test that passed the gate may touch the application or prefs.
        started = true;

        auto &application = testApplication();
        ASSERT_TRUE(application.gtk_app());

        docA = application.document_add(
            SPDocument::createNewDocFromMem(std::string_view{kSvgA}));
        ASSERT_TRUE(docA);
        docA_destroy = docA->connectDestroy([this] { docA = nullptr; });

        docB = application.document_add(
            SPDocument::createNewDocFromMem(std::string_view{kSvgB}));
        ASSERT_TRUE(docB);
        docB_destroy = docB->connectDestroy([this] { docB = nullptr; });

        desktop = application.createDesktop(docA, false, true);
        ASSERT_TRUE(desktop);
        desktop_destroy = desktop->connectDestroy([this](SPDesktop *) {
            desktop = nullptr;
            window = nullptr;
        });
        window = desktop->getInkscapeWindow();
        ASSERT_TRUE(window);
        drainMainContext();
    }

    void TearDown() override
    {
        // A skipped SetUp must not initialize the application or pump events.
        if (!started) {
            return;
        }

        // Mark live documents clean so teardown cannot raise a Save prompt.
        if (docA) {
            docA->setModifiedSinceSave(false);
        }
        if (docB) {
            docB->setModifiedSinceSave(false);
        }

        // Restore the document the application's desktop map expects before
        // destruction, in case a test stopped mid-swap.
        if (desktop && docA && desktop->getDocument() != docA) {
            testApplication().document_swap(desktop, docA);
        }
        if (desktop) {
            testApplication().destroyDesktop(desktop);
        }

        // The connectDestroy handlers null each tracked pointer as it dies, so
        // a partial setup can neither double-close nor touch a freed document.
        if (docA) {
            testApplication().document_close(docA);
        }
        if (docB) {
            testApplication().document_close(docB);
        }
        drainMainContext();
    }

    SPDocument *docA = nullptr;
    SPDocument *docB = nullptr;
    SPDesktop *desktop = nullptr;
    InkscapeWindow *window = nullptr;
    bool started = false;
    sigc::scoped_connection docA_destroy;
    sigc::scoped_connection docB_destroy;
    sigc::scoped_connection desktop_destroy;
};

TEST_F(DesktopDocumentBindingTest, StableReadsAndSameDocumentActivityPreserveBinding)
{
    ASSERT_TRUE(docA);
    ASSERT_TRUE(desktop);
    ASSERT_TRUE(window);
    auto *selection = desktop->getSelection();
    ASSERT_TRUE(selection);
    auto *rectA = cast<SPItem>(docA->getObjectById("rectA"));
    ASSERT_TRUE(rectA);

    auto const stamp = desktop->documentBindingGeneration();
    ASSERT_TRUE(stamp.has_value()) << "a settled desktop must expose a binding stamp";
    uint64_t const base = *stamp;

    std::string const xml_before = fingerprint(*docA);
    bool const dirty_before = docA->isModifiedSinceSave();
    bool const undo_sensitive_before = Inkscape::DocumentUndo::getUndoSensitive(docA);
    bool const transaction_before = docA->getReprDoc()->inTransaction();
    std::string const name_before = docA->getDocumentName() ? docA->getDocumentName() : "";

    // Repeated reads preserve stamp, raw XML, dirty flag, undo sensitivity,
    // transaction state and identity.
    for (int i = 0; i < 5; ++i) {
        auto const repeated = desktop->documentBindingGeneration();
        ASSERT_TRUE(repeated.has_value());
        EXPECT_EQ(*repeated, base);
    }
    EXPECT_EQ(fingerprint(*docA), xml_before);
    EXPECT_EQ(docA->isModifiedSinceSave(), dirty_before);
    EXPECT_EQ(Inkscape::DocumentUndo::getUndoSensitive(docA), undo_sensitive_before);
    // Native documents may legitimately carry an active Undo transaction; the
    // contract is that same-document reads leave the transaction state unchanged.
    EXPECT_EQ(docA->getReprDoc()->inTransaction(), transaction_before);
    std::string const name_after = docA->getDocumentName() ? docA->getDocumentName() : "";
    EXPECT_EQ(name_after, name_before);
    EXPECT_EQ(desktop->getDocument(), docA);

    // Native same-document activity must not advance the binding stamp: it is
    // document identity, not a content or selection revision.
    selection->set(rectA);
    selection->clear();
    desktop->zoom_absolute(Geom::Point(20, 30), 2.0);

    auto const after = desktop->documentBindingGeneration();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*after, base);
    EXPECT_EQ(desktop->getDocument(), docA);
    EXPECT_EQ(docA->getReprDoc()->inTransaction(), transaction_before);
}

TEST_F(DesktopDocumentBindingTest, ChangeDocumentIsUnreadyFromFirstSelectionCallbackThroughReplacement)
{
    ASSERT_TRUE(docA);
    ASSERT_TRUE(docB);
    ASSERT_TRUE(desktop);
    ASSERT_TRUE(window);
    auto *selection = desktop->getSelection();
    ASSERT_TRUE(selection);
    auto *rectA = cast<SPItem>(docA->getObjectById("rectA"));
    ASSERT_TRUE(rectA);

    // A live selection makes change_document's clear notify our callbacks.
    selection->set(rectA);

    auto const old = desktop->documentBindingGeneration();
    ASSERT_TRUE(old.has_value());
    uint64_t const old_stamp = *old;

    // Trial: an early selection callback that throws must unwind without a
    // half-switched desktop. The throw invalidates the old binding, so the
    // generation must surface a fresh stamp rather than rewind.
    bool throwing_saw_empty = false;
    sigc::scoped_connection throwing = selection->connectChangedFirst([&](Inkscape::Selection *) {
        throwing_saw_empty = !desktop->documentBindingGeneration().has_value();
        throw std::runtime_error("desktop-document-binding fixture early callback");
    });
    EXPECT_THROW(testApplication().document_swap(desktop, docB), std::runtime_error);
    throwing.disconnect();

    EXPECT_TRUE(throwing_saw_empty);
    // A fatal assertion here stops the test before rectA is used again, so a
    // bad runtime path cannot continue with a cross-document selection.
    ASSERT_EQ(desktop->getDocument(), docA);
    auto const after_throw = desktop->documentBindingGeneration();
    ASSERT_TRUE(after_throw.has_value());
    EXPECT_NE(*after_throw, old_stamp);

    // The throwing trial may have cleared the selection; re-establish it.
    selection->set(rectA);

    // Actual replacement: the unready window spans the first selection
    // notification through the document-replaced notification.
    unsigned selection_calls = 0;
    bool selection_saw_doc_a = false;
    bool selection_saw_empty = true;
    sigc::scoped_connection selection_conn = selection->connectChangedFirst([&](Inkscape::Selection *) {
        if (selection_calls++ == 0) {
            selection_saw_doc_a = (desktop->getDocument() == docA);
        }
        selection_saw_empty = selection_saw_empty && !desktop->documentBindingGeneration().has_value();
    });

    unsigned replaced_calls = 0;
    bool replaced_saw_doc_b = false;
    bool replaced_saw_empty = true;
    sigc::scoped_connection replaced_conn = desktop->connectDocumentReplaced([&](SPDesktop *, SPDocument *document) {
        if (replaced_calls++ == 0) {
            replaced_saw_doc_b = (document == docB);
        }
        replaced_saw_empty = replaced_saw_empty && !desktop->documentBindingGeneration().has_value();
    });

    ASSERT_TRUE(testApplication().document_swap(desktop, docB));

    // Both native callbacks ran; the first selection notification latched docA
    // and every invocation observed an unready desktop.
    EXPECT_GT(selection_calls, 0u);
    EXPECT_TRUE(selection_saw_doc_a);
    EXPECT_TRUE(selection_saw_empty);
    EXPECT_GT(replaced_calls, 0u);
    EXPECT_TRUE(replaced_saw_doc_b);
    EXPECT_TRUE(replaced_saw_empty);

    // Disconnect before restoring to docA so no handler outlives its locals.
    selection_conn.disconnect();
    replaced_conn.disconnect();

    // After the swap settles, a fresh, different stamp is present.
    auto const fresh = desktop->documentBindingGeneration();
    ASSERT_TRUE(fresh.has_value());
    EXPECT_NE(*fresh, old_stamp);
    EXPECT_EQ(desktop->getDocument(), docB);
    EXPECT_EQ(window->get_document(), docB);
    EXPECT_TRUE(docB->getObjectById("rectB"));

    // Restore the original map-consistent state through the same public path.
    ASSERT_TRUE(testApplication().document_swap(desktop, docA));
    auto const restored = desktop->documentBindingGeneration();
    ASSERT_TRUE(restored.has_value());
    EXPECT_NE(*restored, *fresh);
    EXPECT_EQ(desktop->getDocument(), docA);
    EXPECT_EQ(window->get_document(), docA);
}

// A direct, public SPDesktop::setDocument bypasses the application's desktop
// map and the window's active document, so each switch can only be undone by
// another direct setDocument. Both native emissions must be observed while the
// binding is unready, and each settled switch must publish a fresh stamp.
TEST_F(DesktopDocumentBindingTest, DirectSetDocumentAndReturnToOriginalStayStale)
{
    ASSERT_TRUE(docA);
    ASSERT_TRUE(docB);
    ASSERT_TRUE(desktop);
    ASSERT_TRUE(window);

    auto const original = desktop->documentBindingGeneration();
    ASSERT_TRUE(original.has_value());
    uint64_t const original_stamp = *original;

    // Declared before the observer so, on any early return, the observer
    // disconnects before the desktop is returned to the map-owned docA.
    scope_exit restore([&] {
        if (desktop && docA && desktop->getDocument() != docA) {
            desktop->setDocument(docA);
        }
    });

    bool saw_doc_b = false;
    bool saw_doc_a = false;
    bool every_callback_unready = true;
    sigc::scoped_connection replaced = desktop->connectDocumentReplaced(
        [&](SPDesktop *, SPDocument *document) {
            if (document == docB) {
                saw_doc_b = true;
            } else if (document == docA) {
                saw_doc_a = true;
            }
            every_callback_unready =
                every_callback_unready && !desktop->documentBindingGeneration().has_value();
        });

    desktop->setDocument(docB);
    EXPECT_EQ(desktop->getDocument(), docB);
    auto const after_b = desktop->documentBindingGeneration();
    ASSERT_TRUE(after_b.has_value());
    uint64_t const stamp_b = *after_b;
    EXPECT_NE(stamp_b, original_stamp);

    desktop->setDocument(docA);
    EXPECT_EQ(desktop->getDocument(), docA);
    auto const after_a = desktop->documentBindingGeneration();
    ASSERT_TRUE(after_a.has_value());
    EXPECT_NE(*after_a, original_stamp);
    EXPECT_NE(*after_a, stamp_b);

    replaced.disconnect();

    EXPECT_TRUE(saw_doc_b);
    EXPECT_TRUE(saw_doc_a);
    EXPECT_TRUE(every_callback_unready);
}

// BUG-009: SPDesktop's picking cache (get_flat_item_list) kept raw SPItem
// pointers of the outgoing document across a document replacement. File >
// Revert destroys the old document right after the swap, and the next pointer
// move over the canvas read the freed items (SelectTool::_updateHoverOutline ->
// SPDesktop::find_items_at_point). Documents without layers never cleared it.
TEST_F(DesktopDocumentBindingTest, DocumentReplacementDropsThePickingCache)
{
    ASSERT_TRUE(docA);
    ASSERT_TRUE(docB);
    ASSERT_TRUE(desktop);
    scope_exit restore([&] {
        if (desktop && docA && desktop->getDocument() != docA) {
            desktop->setDocument(docA);
        }
    });
    auto const expect_only = [&](SPDocument *document) {
        std::size_t seen = 0;
        for (bool into_groups : {false, true}) {
            for (bool active_only : {false, true}) {
                for (auto *item : desktop->get_flat_item_list_for_testing(into_groups, active_only)) {
                    ++seen;
                    EXPECT_EQ(item->document, document)
                        << "stale picking-cache entry " << (item->getId() ? item->getId() : "(no id)")
                        << " into_groups=" << into_groups << " active_only=" << active_only;
                }
            }
        }
        EXPECT_GT(seen, 0u);
    };
    // Fill every cache key with docA's items, as a hover over the canvas does.
    expect_only(docA);
    // The swap File > Revert and an Open into this window perform.
    desktop->setDocument(docB);
    expect_only(docB);
    desktop->setDocument(docA);
    expect_only(docA);
}

// A nested direct setDocument from inside the outer replacement callback must
// not restore readiness when the inner call returns: readiness is a per-switch
// previous-state restore, so the outer switch owns the final settle. The guard
// declared before the observer returns the desktop to docA before docC dies.
TEST_F(DesktopDocumentBindingTest, NestedSetDocumentRemainsUnreadyUntilOuterReturn)
{
    ASSERT_TRUE(docA);
    ASSERT_TRUE(docB);
    ASSERT_TRUE(desktop);
    ASSERT_TRUE(window);

    auto const original = desktop->documentBindingGeneration();
    ASSERT_TRUE(original.has_value());
    uint64_t const original_stamp = *original;

    SPDocument *docC = testApplication().document_add(SPDocument::createNewDocFromMem(std::string_view{
        "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
        "<title>Document C</title>"
        "<rect id='rectC' x='30' y='30' width='5' height='5'/>"
        "</svg>"}));
    ASSERT_TRUE(docC);

    // The destruction tracker outlives the cleanup guard so closing docC below
    // can still null this pointer.
    sigc::scoped_connection docC_destroy = docC->connectDestroy([&] { docC = nullptr; });

    scope_exit cleanup([&] {
        if (desktop && docA && desktop->getDocument() != docA) {
            desktop->setDocument(docA);
        }
        if (docC) {
            docC->setModifiedSinceSave(false);
            testApplication().document_close(docC);
        }
    });

    EXPECT_TRUE(docC->getObjectById("rectC"));

    bool nested_entered = false;
    bool outer_saw_empty = true;
    bool saw_doc_c_signal = false;
    bool c_callback_saw_empty = true;
    bool c_still_unready = false;
    sigc::scoped_connection replaced = desktop->connectDocumentReplaced(
        [&](SPDesktop *, SPDocument *document) {
            bool const unready = !desktop->documentBindingGeneration().has_value();
            if (document == docB) {
                outer_saw_empty = outer_saw_empty && unready;
                if (!nested_entered) {
                    nested_entered = true;
                    desktop->setDocument(docC);
                    EXPECT_EQ(desktop->getDocument(), docC);
                    c_still_unready = !desktop->documentBindingGeneration().has_value();
                }
            } else if (document == docC) {
                saw_doc_c_signal = true;
                c_callback_saw_empty = c_callback_saw_empty && unready;
            }
        });

    // Outer switch to docB; its callback performs the nested switch to docC.
    desktop->setDocument(docB);

    // Readiness stays empty through the nested unwind and only the outer return
    // publishes the fresh stamp, with the nested document still selected.
    EXPECT_TRUE(nested_entered);
    EXPECT_TRUE(outer_saw_empty);
    EXPECT_TRUE(saw_doc_c_signal);
    EXPECT_TRUE(c_callback_saw_empty);
    EXPECT_TRUE(c_still_unready);
    EXPECT_EQ(desktop->getDocument(), docC);
    auto const ready = desktop->documentBindingGeneration();
    ASSERT_TRUE(ready.has_value());
    EXPECT_NE(*ready, original_stamp);

    replaced.disconnect();
}

} // namespace

TEST_F(DesktopDocumentBindingTest, NestingToolObservesModifiedReplacementDocument)
{
    desktop->setTool("/tools/nesting");
    ASSERT_TRUE(testApplication().document_swap(desktop, docB));
    drainMainContext();
    auto tool = dynamic_cast<Inkscape::UI::Tools::NestingTool *>(desktop->getTool());
    ASSERT_TRUE(tool);
    unsigned updates = 0;
    sigc::scoped_connection changed = tool->connect_state_changed([&] { ++updates; });
    auto rect = docB->getObjectById("rectB");
    ASSERT_TRUE(rect);
    rect->getRepr()->setAttribute("x", "21");
    docB->ensureUpToDate();
    drainMainContext();
    EXPECT_GT(updates, 0u);
    updates = 0;
    docA->getObjectById("rectA")->getRepr()->setAttribute("x", "11");
    docA->ensureUpToDate();
    drainMainContext();
    EXPECT_EQ(updates, 0u) << "outgoing document must no longer notify the tool";
}

TEST_F(DesktopDocumentBindingTest, FontPreviewDropsDestroyedDesktopBeforeCancelOrRebind)
{
    auto preview_document = testApplication().document_add(SPDocument::createNewDocFromMem(
        std::string_view{"<svg xmlns='http://www.w3.org/2000/svg'><text id='textA' x='10' y='40'>Preview</text></svg>"}));
    ASSERT_TRUE(preview_document);
    ASSERT_TRUE(testApplication().document_swap(desktop, preview_document));
    auto owner = std::make_unique<Inkscape::UI::TextFontPreviewController>();
    auto &preview = *owner;
    preview.setDesktop(desktop);
    auto text = cast<SPText>(preview_document->getObjectById("textA"));
    ASSERT_TRUE(text);
    preview.begin({{Inkscape::SPWeakPtr<SPItem>{text}, 0, 7, true}});
    FontChoice choice;
    choice.phase = FontChoicePhase::Preview;
    choice.origin = FontChoiceOrigin::Keyboard;
    choice.family = "serif";
    choice.available = true;
    preview.request(choice);
    drainMainContext();
    ASSERT_TRUE(preview.active());
    ASSERT_NE(text->displayLayout(desktop->dkey), &text->layout);
    preview_document->setModifiedSinceSave(false);
    ASSERT_TRUE(testApplication().destroyDesktop(desktop));
    ASSERT_EQ(desktop, nullptr);
    EXPECT_EQ(preview.desktopForTesting(), nullptr);
    // Avoid a UAF on the old implementation after the pointer assertion fails.
    // Nulling via the live destroy signal is the regression oracle.
    if (preview.desktopForTesting()) {
        (void)owner.release(); // Only the failing old implementation leaks instead of dereferencing freed storage.
        return;
    }
    preview.cancel();
    preview.request(choice);
    EXPECT_FALSE(preview.active());
    EXPECT_FALSE(preview.commit(choice));
    auto replacement = testApplication().createDesktop(docB, false, true);
    ASSERT_TRUE(replacement);
    sigc::scoped_connection replacement_destroy = replacement->connectDestroy([&](SPDesktop *) { replacement = nullptr; });
    scope_exit close([&] {
        if (docB) docB->setModifiedSinceSave(false);
        if (replacement) testApplication().destroyDesktop(replacement);
    });
    preview.setDesktop(replacement);
    EXPECT_EQ(preview.desktopForTesting(), replacement);
    preview.cancel();
}
