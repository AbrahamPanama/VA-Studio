// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Real GUI outcome coverage for desktop/window origin tokens.
 *
 * Two real registered documents A/B are attached as two tabs of one native
 * window. The tests observe:
 *   - a default-constructed InkscapeWindow::LifetimeToken and
 *     SPDesktopWidget::AttachmentToken are invalid, real ones are valid, and an
 *     unknown/null desktop lookup is invalid;
 *   - repeated token reads plus native widget.switchDesktop() leave A's origin
 *     token, its document membership and its raw XML state unchanged, while a
 *     real app.desktopClose() of the unrelated B permanently invalidates B's
 *     attachment token only;
 *   - a native widget.removeDesktop(A) invalidates A's attachment *before* the
 *     native switch/focus/action callbacks fire: inside those callbacks A is
 *     already invalid and native _desktops may still contain A. Re-adding A
 *     installs a fresh token that is not ready during the attachment callbacks
 *     and valid afterwards, while the old token stays permanently invalid.
 *
 * The fixture mirrors testfiles/src/desktop-document-binding-test.cpp (process
 * lifetime registered GUI app, raw const-tree fingerprint, bounded drain).
 */

#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <giomm/actiongroup.h>
#include <giomm/listmodel.h>
#include <glib/gstdio.h>
#include <glibmm/ustring.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <sigc++/scoped_connection.h>

#include "actions/actions-helper-gui.h"
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "object/sp-item.h"
#include "object/sp-object.h"
#include "object/sp-root.h"
#include "selection.h"
#include "io/document-file-operation.h"
#include "ui/widget/canvas.h"
#include "ui/widget/desktop-widget.h"
#include "util/scope_exit.h"
#include "xml/attribute-record.h"
#include "xml/document.h"
#include "xml/node.h"

namespace Fop = Inkscape::IO;

namespace {

class TestApplication : public InkscapeApplication {};

InkscapeApplication &testApplication()
{
    static auto application = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "desktoporiginlifetimetest", true);
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

// Raw, nonmutating XML fingerprint reused from desktop-document-binding-test.cpp.
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

// Everything observable about a document outside its window title: identity,
// whole-tree raw XML, dirty flag, undo sensitivity and transaction state. The
// title is checked separately because a legitimate tab switch changes it.
std::string documentState(SPDocument &document)
{
    std::string out;
    appendField(out, document.getDocumentFilename());
    appendField(out, document.getDocumentBase());
    appendField(out, document.getDocumentName());
    out += "modified=" + std::string(document.isModifiedSinceSave() ? "1" : "0");
    out += ";undo-sensitive=" + std::string(Inkscape::DocumentUndo::getUndoSensitive(&document) ? "1" : "0");
    out += ";transaction=" + std::string(document.getReprDoc()->inTransaction() ? "1" : "0");
    out += ";" + fingerprint(document);
    return out;
}

class DesktopOriginLifetimeTest : public ::testing::Test
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

        // A opens its own window; B natively joins that same window as a second
        // tab (new_window == false), never a duplicate window.
        desktopA = application.createDesktop(docA, false, true);
        ASSERT_TRUE(desktopA);
        desktopA_destroy = desktopA->connectDestroy([this](SPDesktop *) { desktopA = nullptr; });
        window = desktopA->getInkscapeWindow();
        ASSERT_TRUE(window);
        widget = window->get_desktop_widget();
        ASSERT_TRUE(widget);

        desktopB = application.createDesktop(docB, false, false);
        ASSERT_TRUE(desktopB);
        desktopB_destroy = desktopB->connectDestroy([this](SPDesktop *) { desktopB = nullptr; });

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

        // Close any still-attached desktop through the application's normal
        // desktop-close path (removes the tab); registered documents follow.
        if (desktopB) {
            testApplication().desktopClose(desktopB);
        }
        if (desktopA) {
            testApplication().desktopClose(desktopA);
        }
        drainMainContext();

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
    SPDesktop *desktopA = nullptr;
    SPDesktop *desktopB = nullptr;
    InkscapeWindow *window = nullptr;
    SPDesktopWidget *widget = nullptr;
    bool started = false;
    sigc::scoped_connection docA_destroy;
    sigc::scoped_connection docB_destroy;
    sigc::scoped_connection desktopA_destroy;
    sigc::scoped_connection desktopB_destroy;
};

// Stable origin reads and native tab switches. Token validity is an origin
// property, not the active-pointer binding, so A's original token survives
// switchDesktop(B) and switchDesktop(A) and closing the unrelated B.
TEST_F(DesktopOriginLifetimeTest, StableReadsAndTabSwitchPreserveOrigin)
{
    ASSERT_TRUE(docA);
    ASSERT_TRUE(docB);
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);
    ASSERT_TRUE(window);
    ASSERT_TRUE(widget);

    // Default-constructed tokens never name a live origin.
    InkscapeWindow::LifetimeToken const default_window_token{};
    EXPECT_FALSE(default_window_token.valid());
    SPDesktopWidget::AttachmentToken const default_attachment_token{};
    EXPECT_FALSE(default_attachment_token.valid());

    // A real window token and both real attachments are valid.
    InkscapeWindow::LifetimeToken const window_token = window->lifetimeToken();
    EXPECT_TRUE(window_token.valid());
    SPDesktopWidget::AttachmentToken const attachmentA = widget->getAttachmentToken(desktopA);
    SPDesktopWidget::AttachmentToken const attachmentB = widget->getAttachmentToken(desktopB);
    EXPECT_TRUE(attachmentA.valid());
    EXPECT_TRUE(attachmentB.valid());

    // Null and unknown desktop lookups are invalid. The unknown value is only a
    // map key probe and is never dereferenced.
    EXPECT_FALSE(widget->getAttachmentToken(nullptr).valid());
    SPDesktop *const unknown_desktop =
        reinterpret_cast<SPDesktop *>(static_cast<std::uintptr_t>(0x1));
    EXPECT_FALSE(widget->getAttachmentToken(unknown_desktop).valid());

    widget->switchDesktop(desktopA);
    ASSERT_EQ(widget->get_desktop(), desktopA);
    ASSERT_EQ(window->get_document(), docA);

    std::string const state_before = documentState(*docA);
    std::string const title_before = window->get_title().raw();

    // Repeated origin reads preserve the token, the whole raw XML tree and all
    // document identity/undo state; only the read is measured here, so the
    // window title must also be unchanged.
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(window_token.valid());
        EXPECT_TRUE(attachmentA.valid());
        EXPECT_TRUE(attachmentB.valid());
        EXPECT_EQ(documentState(*docA), state_before);
    }
    EXPECT_EQ(window->get_title().raw(), title_before);

    // Native tab switches A -> B -> A. A's original token stays valid and the
    // desktop/document membership stays exact. The title legitimately changes
    // across the switch, so it is not compared here.
    widget->switchDesktop(desktopB);
    EXPECT_TRUE(attachmentA.valid());
    EXPECT_TRUE(attachmentB.valid());
    EXPECT_EQ(widget->get_desktop(), desktopB);
    EXPECT_EQ(desktopA->getDocument(), docA);
    EXPECT_EQ(desktopB->getDocument(), docB);
    EXPECT_EQ(window->get_document(), docB);

    widget->switchDesktop(desktopA);
    EXPECT_TRUE(attachmentA.valid());
    EXPECT_TRUE(attachmentB.valid());
    EXPECT_EQ(widget->get_desktop(), desktopA);
    EXPECT_EQ(desktopA->getDocument(), docA);
    EXPECT_EQ(window->get_document(), docA);
    EXPECT_EQ(documentState(*docA), state_before);

    // Close the unrelated B through the application's normal path: only B's old
    // attachment token is invalidated, A's token and document are untouched.
    testApplication().desktopClose(desktopB);
    drainMainContext();

    EXPECT_TRUE(window_token.valid());
    EXPECT_TRUE(attachmentA.valid());
    EXPECT_FALSE(attachmentB.valid());
    EXPECT_EQ(widget->get_desktop(), desktopA);
    EXPECT_EQ(desktopA->getDocument(), docA);
    EXPECT_EQ(documentState(*docA), state_before);

    // A view-only visibility toggle on the retained window is a legitimate native
    // change but not an origin change: both real tokens stay valid. These
    // view-affecting calls are deliberately kept after the raw-XML no-op
    // comparisons above so they cannot be confused with read-only token behavior.
    window->set_visible(false);
    EXPECT_TRUE(window_token.valid());
    EXPECT_TRUE(attachmentA.valid());
    window->set_visible(true);
    EXPECT_TRUE(window_token.valid());
    EXPECT_TRUE(attachmentA.valid());
    EXPECT_FALSE(attachmentB.valid());
    EXPECT_EQ(window->get_document(), docA);

    // One unrelated ordinary toplevel is retained on the stack. Its captured
    // native identity is compared against the actual GTK toplevel model, and its
    // real native top-level removal is observed positively before that model
    // could invalidate anything else. The callback reads only the captured native
    // pointer, never any token or fixture.
    Gtk::Window unrelatedWindow;
    gpointer const unrelated_native = unrelatedWindow.gobj();
    Glib::RefPtr<Gio::ListModel> const toplevels = Gtk::Window::get_toplevels();
    unsigned unrelated_removals = 0;
    bool unrelated_removed = false;
    sigc::scoped_connection unrelated_conn =
        toplevels->signal_items_changed().connect(
            [&](guint /*position*/, guint removed, guint /*added*/) {
                if (removed == 0) {
                    return;
                }
                bool present = false;
                GListModel *const model = toplevels->gobj();
                guint const n = g_list_model_get_n_items(model);
                for (guint i = 0; i < n; ++i) {
                    gpointer const item = g_list_model_get_item(model, i);
                    if (item == unrelated_native) {
                        present = true;
                    }
                    if (item) {
                        g_object_unref(item);
                    }
                }
                if (!present) {
                    ++unrelated_removals;
                    unrelated_removed = true;
                }
            });
    gtk_window_destroy(GTK_WINDOW(unrelatedWindow.gobj()));
    unrelated_conn.disconnect();

    EXPECT_TRUE(unrelated_removed) << "unrelated toplevel native removal not observed";
    EXPECT_GE(unrelated_removals, 1u);
    EXPECT_TRUE(window_token.valid());
    EXPECT_TRUE(attachmentA.valid());
    EXPECT_FALSE(attachmentB.valid());
}

// A native remove invalidates A's attachment before the native switch/focus/
// action callbacks run, and a re-add installs a fresh, not-yet-ready token.
TEST_F(DesktopOriginLifetimeTest, RemoveAndReattachInvalidatesBeforeNativeCallbacks)
{
    ASSERT_TRUE(docA);
    ASSERT_TRUE(docB);
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);
    ASSERT_TRUE(window);
    ASSERT_TRUE(widget);

    // B keeps the window alive while A is detached; A is made active through the
    // native tab API before any observer is installed.
    widget->switchDesktop(desktopA);
    ASSERT_EQ(widget->get_desktop(), desktopA);

    SPDesktopWidget::AttachmentToken const oldA = widget->getAttachmentToken(desktopA);
    SPDesktopWidget::AttachmentToken const oldB = widget->getAttachmentToken(desktopB);
    ASSERT_TRUE(oldA.valid());
    ASSERT_TRUE(oldB.valid());

    // Observations are staged so setup/restoration emissions cannot be mistaken
    // for remove/add outcomes. All counters and flags outlive the connections.
    enum class Stage { Setup, Remove, Add };
    Stage stage = Stage::Setup;
    unsigned remove_callbacks = 0;
    unsigned add_callbacks = 0;
    // The first callback's exact boundary values are recorded explicitly, and
    // every later callback is ANDed in, so a wrong first callback can never be
    // hidden by a later correct one (the previous OR aggregation could).
    bool remove_first_oldA_invalid = false;
    bool remove_first_B_valid = false;
    bool remove_first_A_still_native_member = false;
    bool remove_all_oldA_invalid = true;
    bool remove_all_B_valid = true;
    bool remove_all_A_still_native_member = true;
    bool add_first_freshA_invalid = false;
    bool add_first_oldA_invalid = false;
    bool add_first_B_valid = false;
    bool add_all_freshA_invalid = true;
    bool add_all_oldA_invalid = true;
    bool add_all_B_valid = true;

    // A single non-throwing callback serves both native signal paths. It reads
    // only the live, retained test-scope widget and tokens.
    auto onSignal = [&](bool /*action_path*/) {
        if (stage == Stage::Remove) {
            bool const oldA_invalid = !oldA.valid();
            bool const B_valid = widget->getAttachmentToken(desktopB).valid();
            auto const &members = widget->get_desktops();
            bool const A_still_native_member =
                std::find(members.begin(), members.end(), desktopA) != members.end();
            if (remove_callbacks == 0) {
                remove_first_oldA_invalid = oldA_invalid;
                remove_first_B_valid = B_valid;
                remove_first_A_still_native_member = A_still_native_member;
            }
            remove_all_oldA_invalid = remove_all_oldA_invalid && oldA_invalid;
            remove_all_B_valid = remove_all_B_valid && B_valid;
            remove_all_A_still_native_member = remove_all_A_still_native_member && A_still_native_member;
            ++remove_callbacks;
        } else if (stage == Stage::Add) {
            bool const freshA_invalid = !widget->getAttachmentToken(desktopA).valid();
            bool const oldA_invalid = !oldA.valid();
            bool const B_valid = widget->getAttachmentToken(desktopB).valid();
            if (add_callbacks == 0) {
                add_first_freshA_invalid = freshA_invalid;
                add_first_oldA_invalid = oldA_invalid;
                add_first_B_valid = B_valid;
            }
            add_all_freshA_invalid = add_all_freshA_invalid && freshA_invalid;
            add_all_oldA_invalid = add_all_oldA_invalid && oldA_invalid;
            add_all_B_valid = add_all_B_valid && B_valid;
            ++add_callbacks;
        }
    };

    // Declared before the scoped observers: on early return the observers
    // disconnect first and only then is a detached A reattached.
    scope_exit restore([&] {
        if (desktopA && widget) {
            auto const &members = widget->get_desktops();
            if (std::find(members.begin(), members.end(), desktopA) == members.end()) {
                widget->addDesktop(desktopA);
            }
        }
    });

    // Two native callback sources: the focus-widget property is the
    // cross-platform path; the "win" action-mirror removal is the macOS path.
    sigc::scoped_connection focus_conn =
        window->property_focus_widget().signal_changed().connect([&] { onSignal(false); });
    sigc::scoped_connection action_conn =
        window->signal_action_removed().connect([&](const Glib::ustring &) { onSignal(true); });

    // The focus path is our only non-macOS callback source. Make the active
    // canvas the actual focus widget and prove it before removal, so the native
    // switch to B during removal is a real focus-widget change.
    auto *const canvasA = desktopA->getCanvas();
    ASSERT_TRUE(canvasA);
    ASSERT_TRUE(canvasA->grab_focus());
    ASSERT_EQ(window->property_focus_widget().get_value(), static_cast<Gtk::Widget *>(canvasA));

    // ---- Remove phase ------------------------------------------------------
    stage = Stage::Remove;
    widget->removeDesktop(desktopA);

    EXPECT_GE(remove_callbacks, 1u) << "no native callback observed during removeDesktop";
    EXPECT_TRUE(remove_first_oldA_invalid);
    EXPECT_TRUE(remove_first_B_valid);
    EXPECT_TRUE(remove_first_A_still_native_member);
    EXPECT_TRUE(remove_all_oldA_invalid);
    EXPECT_TRUE(remove_all_B_valid);
    EXPECT_TRUE(remove_all_A_still_native_member);
    EXPECT_FALSE(oldA.valid());
    EXPECT_EQ(desktopA->getDesktopWidget(), nullptr);
    {
        auto const &members = widget->get_desktops();
        EXPECT_EQ(std::find(members.begin(), members.end(), desktopA), members.end());
    }
    EXPECT_TRUE(widget->getAttachmentToken(desktopB).valid());

    // ---- Add phase ---------------------------------------------------------
    stage = Stage::Add;
    widget->addDesktop(desktopA);

    EXPECT_GE(add_callbacks, 1u) << "no native callback observed during addDesktop";
    EXPECT_TRUE(add_first_freshA_invalid);
    EXPECT_TRUE(add_first_oldA_invalid);
    EXPECT_TRUE(add_first_B_valid);
    EXPECT_TRUE(add_all_freshA_invalid);
    EXPECT_TRUE(add_all_oldA_invalid);
    EXPECT_TRUE(add_all_B_valid);

    SPDesktopWidget::AttachmentToken const newA = widget->getAttachmentToken(desktopA);
    EXPECT_TRUE(newA.valid());
    EXPECT_FALSE(oldA.valid());

    // Same original desktops/documents and exact native membership.
    EXPECT_EQ(desktopA->getDocument(), docA);
    EXPECT_EQ(desktopB->getDocument(), docB);
    EXPECT_EQ(desktopA->getDesktopWidget(), widget);
    EXPECT_EQ(desktopB->getDesktopWidget(), widget);
    auto const &members = widget->get_desktops();
    EXPECT_EQ(members.size(), 2u);
    EXPECT_NE(std::find(members.begin(), members.end(), desktopA), members.end());
    EXPECT_NE(std::find(members.begin(), members.end(), desktopB), members.end());

    // Observers disconnect here, before the scope cleanup can run.
    focus_conn.disconnect();
    action_conn.disconnect();
}

// Detaching the active tab into a fresh native window removes A from the
// original widget (permanently invalidating the old attachment) while B keeps
// the original window alive; the new window installs a fresh, valid attachment
// and owns A's document. The fixture trackers own A and B across the two
// different windows, so TearDown already closes both real desktops.
TEST_F(DesktopOriginLifetimeTest, NativeDetachCreatesFreshDestinationAttachment)
{
    ASSERT_TRUE(docA);
    ASSERT_TRUE(docB);
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);
    ASSERT_TRUE(window);
    ASSERT_TRUE(widget);

    widget->switchDesktop(desktopA);
    ASSERT_EQ(widget->get_desktop(), desktopA);

    SPDesktopWidget::AttachmentToken const oldAttachmentA = widget->getAttachmentToken(desktopA);
    SPDesktopWidget::AttachmentToken const attachmentB = widget->getAttachmentToken(desktopB);
    InkscapeWindow::LifetimeToken const oldWindowToken = window->lifetimeToken();
    ASSERT_TRUE(oldAttachmentA.valid());
    ASSERT_TRUE(attachmentB.valid());
    ASSERT_TRUE(oldWindowToken.valid());

    testApplication().detachDesktopToNewWindow(desktopA);
    drainMainContext();

    // A's original attachment is gone for good; B and the original window stay.
    EXPECT_FALSE(oldAttachmentA.valid());
    EXPECT_FALSE(widget->getAttachmentToken(desktopA).valid());
    EXPECT_TRUE(attachmentB.valid());
    EXPECT_TRUE(widget->getAttachmentToken(desktopB).valid());
    EXPECT_TRUE(oldWindowToken.valid());
    EXPECT_EQ(desktopB->getInkscapeWindow(), window);

    // A now lives in a different window/widget with a fresh valid attachment.
    SPDesktopWidget *const newWidget = desktopA->getDesktopWidget();
    InkscapeWindow *const newWindow = desktopA->getInkscapeWindow();
    ASSERT_TRUE(newWidget);
    ASSERT_TRUE(newWindow);
    EXPECT_NE(newWidget, widget);
    EXPECT_NE(newWindow, window);
    EXPECT_EQ(desktopA->getDocument(), docA);
    EXPECT_EQ(newWindow->get_document(), docA);

    SPDesktopWidget::AttachmentToken const newAttachmentA = newWidget->getAttachmentToken(desktopA);
    InkscapeWindow::LifetimeToken const newWindowToken = newWindow->lifetimeToken();
    EXPECT_TRUE(newAttachmentA.valid());
    EXPECT_TRUE(newWindowToken.valid());

    // Exact native membership: the original widget excludes A, the destination
    // widget includes A.
    auto const &originalMembers = widget->get_desktops();
    EXPECT_EQ(std::find(originalMembers.begin(), originalMembers.end(), desktopA), originalMembers.end());
    EXPECT_NE(std::find(originalMembers.begin(), originalMembers.end(), desktopB), originalMembers.end());

    auto const &destinationMembers = newWidget->get_desktops();
    EXPECT_NE(std::find(destinationMembers.begin(), destinationMembers.end(), desktopA),
              destinationMembers.end());
    EXPECT_EQ(newWidget->get_desktop(), desktopA);
}

// A real pending close on docA stays deferred while an admitted file-operation
// guard owns the document lease. The owned gate authorizes output (Success,
// never Pending) while A's origin is attached, then reports StaleTarget once the
// native desktop close removes that origin. The pending close may destroy docA
// only after the guard/lease is released and the main context is drained.
TEST_F(DesktopOriginLifetimeTest, AdmittedCloseWaitsUntilActualOriginDetach)
{
    ASSERT_TRUE(docA);
    ASSERT_TRUE(docB);
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);
    ASSERT_TRUE(window);
    ASSERT_TRUE(widget);

    // Settle the real GUI document into native, output-ready state.
    docA->ensureUpToDate();
    Inkscape::DocumentUndo::done(docA, Inkscape::Util::Internal::ContextString("origin lifetime fixture"), "");

    InkscapeWindow::LifetimeToken const windowToken = window->lifetimeToken();
    SPDesktopWidget::AttachmentToken const attachmentA = widget->getAttachmentToken(desktopA);
    ASSERT_TRUE(windowToken.valid());
    ASSERT_TRUE(attachmentA.valid());

    // Baseline for the not-mutated-by-close checks taken before any reservation.
    std::string const state_before = documentState(*docA);
    std::string const rawxml_before = fingerprint(*docA);

    // The binding predicate captures ONLY copies of the two origin tokens, never
    // a raw desktop/document pointer, so it stays safe after the origin dies.
    Fop::FileOperationResult refusal;
    auto guard = Fop::DocumentFileOperation::admit(
        *docA,
        Fop::FileOperationRequestInfo{Fop::FileOperationMethod::Save, "", ""},
        [windowToken, attachmentA]() -> bool {
            return windowToken.valid() && attachmentA.valid();
        },
        &refusal);
    ASSERT_TRUE(guard);
    auto const ctx = guard->context();
    ASSERT_TRUE(ctx.valid());

    // A real close intent while the guard owns the lease: deferred, not lost.
    testApplication().document_close(docA);
    EXPECT_TRUE(testApplication().documentClosePending(docA));
    ASSERT_NE(docA, nullptr);
    EXPECT_TRUE(windowToken.valid());
    EXPECT_TRUE(attachmentA.valid());
    EXPECT_TRUE(ctx.valid());
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(*docA));

    // The owned gate authorizes output while the origin still exists and the
    // document is alive under the lease; a pending close is never Pending.
    auto const gate = guard->authorizeOutput(ctx);
    EXPECT_EQ(gate.outcome, Fop::FileOperationOutcome::Success) << gate.detail;

    // A fresh admission cannot enter while our slot and the pending close exist.
    Fop::FileOperationResult busy;
    auto denied = Fop::DocumentFileOperation::admit(
        *docA, Fop::FileOperationRequestInfo{}, [] { return true; }, &busy);
    EXPECT_FALSE(denied);
    EXPECT_EQ(busy.outcome, Fop::FileOperationOutcome::Busy);
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(*docA));

    // The native desktop close destroys only the view/origin; docA survives under
    // the operation lease and the old attachment is permanently invalid.
    testApplication().desktopClose(desktopA);
    drainMainContext();
    EXPECT_EQ(desktopA, nullptr);
    ASSERT_NE(desktopB, nullptr);
    EXPECT_EQ(desktopB->getInkscapeWindow(), window);
    EXPECT_FALSE(attachmentA.valid());
    EXPECT_TRUE(windowToken.valid());
    ASSERT_NE(docA, nullptr);
    EXPECT_TRUE(ctx.valid());
    EXPECT_TRUE(Fop::DocumentFileOperation::hasActiveRequest(*docA));

    // The exact same gate now rejects through the binding predicate because the
    // origin attachment is gone.
    auto const stale = guard->authorizeOutput(ctx);
    EXPECT_EQ(stale.outcome, Fop::FileOperationOutcome::StaleTarget) << stale.detail;

    // Before releasing the guard, the close/gate did not mutate the document
    // where native close has no legitimate effect.
    EXPECT_EQ(fingerprint(*docA), rawxml_before);
    EXPECT_EQ(documentState(*docA), state_before);

    // Explicit terminal StaleTarget once, then release the guard/lease. No
    // document or widget pointer is dereferenced after this point.
    guard->complete(Fop::FileOperationResult{Fop::FileOperationOutcome::StaleTarget, "origin detached"});
    EXPECT_TRUE(guard->terminal());
    guard.reset();
    EXPECT_FALSE(ctx.valid());

    // Bounded drain: only now may the pending close destroy docA (tracker null).
    drainMainContext();
    EXPECT_EQ(docA, nullptr);
    EXPECT_NE(docB, nullptr);
}

// A GTK-native window destroy removes the temporary window's native top-level
// membership and invalidates its lifetime token and fresh attachment token even
// inside an earlier-registered observing listener, while the hidden C++ wrapper is
// still retained and owned. The original window is kept alive by B, and the
// cleanup guard re-adopts A through the actual native widget afterwards.
TEST_F(DesktopOriginLifetimeTest, GtkWindowDestroyInvalidatesBeforeNativeCallback)
{
    ASSERT_TRUE(docA);
    ASSERT_TRUE(docB);
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);
    ASSERT_TRUE(window);
    ASSERT_TRUE(widget);

    widget->switchDesktop(desktopA);
    ASSERT_EQ(widget->get_desktop(), desktopA);

    // Settle the real GUI document so the reentrant admission below reaches its
    // read-only binding predicate instead of refusing as not-fresh-ready.
    docA->ensureUpToDate();
    Inkscape::DocumentUndo::done(docA, Inkscape::Util::Internal::ContextString("origin lifetime fixture"), "");

    InkscapeWindow::LifetimeToken const originalWindowToken = window->lifetimeToken();
    SPDesktopWidget::AttachmentToken const oldAttachmentA = widget->getAttachmentToken(desktopA);
    SPDesktopWidget::AttachmentToken const attachmentB = widget->getAttachmentToken(desktopB);
    ASSERT_TRUE(originalWindowToken.valid());
    ASSERT_TRUE(oldAttachmentA.valid());
    ASSERT_TRUE(attachmentB.valid());

    // The owned temporary is declared before the cleanup guard, and the guard is
    // declared before the detach/construct, so even a failed temporary
    // constructor or fatal assert restores A. It captures only the fixture's
    // actual tracked pointers and copied tokens.
    std::unique_ptr<InkscapeWindow> temporary;
    scope_exit cleanup([&] {
        temporary.reset();
        if (desktopA && desktopB && widget && originalWindowToken.valid()) {
            auto const &members = widget->get_desktops();
            if (std::find(members.begin(), members.end(), desktopA) == members.end()) {
                // A's widget pointer is obsolete (it named the temporary);
                // clear it before the original widget re-adopts A.
                desktopA->setDesktopWidget(nullptr);
                widget->addDesktop(desktopA);
            }
        }
    });

    // Detach A from the original widget; B keeps the original window alive.
    widget->removeDesktop(desktopA);
    ASSERT_EQ(desktopA->getDesktopWidget(), nullptr);

    // Tokens and observation state for the temporary are default-constructed
    // before the wrapper exists; the native identity starts null. The observer is
    // connected now, before make_unique, so its connection precedes the
    // production observer installed by the InkscapeWindow constructor.
    InkscapeWindow::LifetimeToken temporaryWindowToken{};
    SPDesktopWidget::AttachmentToken freshAttachmentA{};
    gpointer temporaryNative = nullptr;
    unsigned removal_callbacks = 0;
    bool first_window_invalid = false;
    bool first_attachment_invalid = false;
    bool first_refused_stale = false;
    bool all_window_invalid = true;
    bool all_attachment_invalid = true;

    Glib::RefPtr<Gio::ListModel> const toplevels = Gtk::Window::get_toplevels();
    sigc::scoped_connection model_conn =
        toplevels->signal_items_changed().connect(
            [&](guint /*position*/, guint removed, guint /*added*/) {
                // Ignore additions and unrelated changes until the target native
                // identity is known and this emission actually removed it.
                if (removed == 0 || temporaryNative == nullptr) {
                    return;
                }
                bool present = false;
                GListModel *const model = toplevels->gobj();
                guint const n = g_list_model_get_n_items(model);
                for (guint i = 0; i < n; ++i) {
                    gpointer const item = g_list_model_get_item(model, i);
                    if (item == temporaryNative) {
                        present = true;
                    }
                    if (item) {
                        g_object_unref(item);
                    }
                }
                if (present) {
                    return;
                }
                // The native membership is gone. Record the first exact state and
                // AND every later removal so a bad first callback cannot hide.
                bool const window_invalid = !temporaryWindowToken.valid();
                bool const attachment_invalid = !freshAttachmentA.valid();
                if (removal_callbacks == 0) {
                    first_window_invalid = window_invalid;
                    first_attachment_invalid = attachment_invalid;
                    Fop::FileOperationResult refusal;
                    auto denied = Fop::DocumentFileOperation::admit(
                        *docA, Fop::FileOperationRequestInfo{},
                        [temporaryWindowToken, freshAttachmentA]() -> bool {
                            return temporaryWindowToken.valid() && freshAttachmentA.valid();
                        },
                        &refusal);
                    first_refused_stale =
                        !denied && refusal.outcome == Fop::FileOperationOutcome::StaleTarget;
                }
                all_window_invalid = all_window_invalid && window_invalid;
                all_attachment_invalid = all_attachment_invalid && attachment_invalid;
                ++removal_callbacks;
            });

    // A hidden owned temporary window: its constructor realizes the widget and
    // installs a fresh origin and attachment for A, but it is never presented and
    // no event-pump runs while it is alive. GTK registers it as a native toplevel;
    // the retained C++ wrapper outlives the native destroy below.
    temporary = std::make_unique<InkscapeWindow>(desktopA);
    ASSERT_TRUE(temporary != nullptr);

    temporaryNative = temporary->gobj();
    temporaryWindowToken = temporary->lifetimeToken();
    SPDesktopWidget *const temporaryWidget = temporary->get_desktop_widget();
    ASSERT_TRUE(temporaryWidget);
    freshAttachmentA = temporaryWidget->getAttachmentToken(desktopA);
    ASSERT_TRUE(temporaryWindowToken.valid());
    ASSERT_TRUE(freshAttachmentA.valid());
    ASSERT_FALSE(oldAttachmentA.valid());
    ASSERT_TRUE(attachmentB.valid());

    // The constructor's addDesktop -> switchDesktop -> setActiveTab transitively
    // made A the app's active desktop/document; restore the retained original
    // window context before the native destroy.
    ASSERT_TRUE(originalWindowToken.valid());
    ASSERT_EQ(widget->get_desktop(), desktopB);
    testApplication().set_active_window(window);
    window->setActiveTab(desktopB);
    ASSERT_EQ(testApplication().get_active_desktop(), desktopB);
    ASSERT_EQ(testApplication().get_active_document(), docB);

    // The constructor's own invalidating listener was connected after this test's
    // observer, so this observer's callback runs first. The actual GTK-native
    // boundary is the native toplevel model removal below, not any wrapper
    // method: gtk_window_destroy drops native membership without disposing the
    // retained C++ wrapper.
    gtk_window_destroy(GTK_WINDOW(temporary->gobj()));

    EXPECT_GE(removal_callbacks, 1u) << "no native model removal observed";
    EXPECT_TRUE(first_window_invalid);
    EXPECT_TRUE(first_attachment_invalid);
    EXPECT_TRUE(first_refused_stale);
    EXPECT_TRUE(all_window_invalid);
    EXPECT_TRUE(all_attachment_invalid);
    // The wrapper is still owned; only captured tokens are queried.
    EXPECT_NE(temporary.get(), nullptr);
    EXPECT_FALSE(temporaryWindowToken.valid());
    EXPECT_FALSE(freshAttachmentA.valid());

    // The observer must be disconnected before the cleanup guard disposes the
    // retained wrapper or re-adopts A.
    model_conn.disconnect();
    // No event pump runs between the C destroy and the scope cleanup.
}

// The C++ wrapper destructor invalidates the temporary window's lifetime before
// it clears document actions, so the first native action-removal callback sees
// both tokens already invalid. Native document action mirrors are created only
// on macOS (InkscapeWindow::add_document_actions), so the whole case compiles
// only there; the other platforms keep the 5 remaining enabled cases.
#ifdef __APPLE__
TEST_F(DesktopOriginLifetimeTest, CppWindowDestroyInvalidatesBeforeActionCallbacks)
{
    ASSERT_TRUE(docA);
    ASSERT_TRUE(docB);
    ASSERT_TRUE(desktopA);
    ASSERT_TRUE(desktopB);
    ASSERT_TRUE(window);
    ASSERT_TRUE(widget);

    widget->switchDesktop(desktopA);
    ASSERT_EQ(widget->get_desktop(), desktopA);

    // Settle the real GUI document so the reentrant admission below reaches its
    // read-only binding predicate instead of refusing as not-fresh-ready.
    docA->ensureUpToDate();
    Inkscape::DocumentUndo::done(docA, Inkscape::Util::Internal::ContextString("origin lifetime fixture"), "");

    InkscapeWindow::LifetimeToken const originalWindowToken = window->lifetimeToken();
    SPDesktopWidget::AttachmentToken const oldAttachmentA = widget->getAttachmentToken(desktopA);
    SPDesktopWidget::AttachmentToken const attachmentB = widget->getAttachmentToken(desktopB);
    ASSERT_TRUE(originalWindowToken.valid());
    ASSERT_TRUE(oldAttachmentA.valid());
    ASSERT_TRUE(attachmentB.valid());

    std::unique_ptr<InkscapeWindow> temporary;
    scope_exit cleanup([&] {
        temporary.reset();
        if (desktopA && desktopB && widget && originalWindowToken.valid()) {
            auto const &members = widget->get_desktops();
            if (std::find(members.begin(), members.end(), desktopA) == members.end()) {
                desktopA->setDesktopWidget(nullptr);
                widget->addDesktop(desktopA);
            }
        }
    });

    widget->removeDesktop(desktopA);
    ASSERT_EQ(desktopA->getDesktopWidget(), nullptr);

    temporary = std::make_unique<InkscapeWindow>(desktopA);
    ASSERT_TRUE(temporary != nullptr);

    InkscapeWindow::LifetimeToken const temporaryWindowToken = temporary->lifetimeToken();
    SPDesktopWidget *const temporaryWidget = temporary->get_desktop_widget();
    ASSERT_TRUE(temporaryWidget);
    SPDesktopWidget::AttachmentToken const freshAttachmentA = temporaryWidget->getAttachmentToken(desktopA);
    ASSERT_TRUE(temporaryWindowToken.valid());
    ASSERT_TRUE(freshAttachmentA.valid());
    ASSERT_FALSE(oldAttachmentA.valid());
    ASSERT_TRUE(attachmentB.valid());

    // The constructor's addDesktop -> switchDesktop -> setActiveTab transitively
    // made A the app's active desktop/document; restore the retained original
    // window context before any observer is installed or the temporary dies.
    ASSERT_TRUE(originalWindowToken.valid());
    ASSERT_EQ(widget->get_desktop(), desktopB);
    testApplication().set_active_window(window);
    window->setActiveTab(desktopB);
    ASSERT_EQ(testApplication().get_active_desktop(), desktopB);
    ASSERT_EQ(testApplication().get_active_document(), docB);

    unsigned action_callbacks = 0;
    bool first_window_invalid = false;
    bool first_attachment_invalid = false;
    bool first_refused_stale = false;
    bool all_window_invalid = true;
    bool all_attachment_invalid = true;

    // A single non-throwing callback. It reads only copied tokens plus the
    // registered document gate, never the wrapper being destroyed.
    sigc::scoped_connection action_conn = temporary->signal_action_removed().connect(
        [&](const Glib::ustring &) {
            bool const window_invalid = !temporaryWindowToken.valid();
            bool const attachment_invalid = !freshAttachmentA.valid();
            if (action_callbacks == 0) {
                first_window_invalid = window_invalid;
                first_attachment_invalid = attachment_invalid;
                Fop::FileOperationResult refusal;
                auto denied = Fop::DocumentFileOperation::admit(
                    *docA, Fop::FileOperationRequestInfo{},
                    [temporaryWindowToken, freshAttachmentA]() -> bool {
                        return temporaryWindowToken.valid() && freshAttachmentA.valid();
                    },
                    &refusal);
                first_refused_stale =
                    !denied && refusal.outcome == Fop::FileOperationOutcome::StaleTarget;
            }
            all_window_invalid = all_window_invalid && window_invalid;
            all_attachment_invalid = all_attachment_invalid && attachment_invalid;
            ++action_callbacks;
        });

    // The C++ destructor invalidates the window lifetime before clearing the
    // document action mirrors, so the first native callback already sees both
    // tokens invalid. The wrapper is empty afterwards and is never dereferenced.
    temporary.reset();

    EXPECT_GE(action_callbacks, 1u) << "no native action-removal callback observed";
    EXPECT_TRUE(first_window_invalid);
    EXPECT_TRUE(first_attachment_invalid);
    EXPECT_TRUE(first_refused_stale);
    EXPECT_TRUE(all_window_invalid);
    EXPECT_TRUE(all_attachment_invalid);
    EXPECT_FALSE(temporaryWindowToken.valid());
    EXPECT_FALSE(freshAttachmentA.valid());
    EXPECT_EQ(temporary.get(), nullptr);
}
#endif

// ST-M D1: closing a BACKGROUND tab (its x, middle click, tab menu) does not move
// focus, so nothing re-establishes the application's active state afterwards. It
// must keep pointing at the still-active tab so selection actions keep working.
bool appActionEnabled(char const *name)
{
    return g_action_group_get_action_enabled(G_ACTION_GROUP(testApplication().gio_app()->gobj()), name);
}

void appActivate(char const *name)
{
    testApplication().gio_app()->activate_action(name);
}

TEST_F(DesktopOriginLifetimeTest, ClosingBackgroundTabKeepsActiveSelectionAndActions)
{
    ASSERT_TRUE(desktopA && desktopB && docA && docB);
    auto &app = testApplication();

    widget->switchDesktop(desktopA); // A active, B in the background
    ASSERT_EQ(app.get_active_desktop(), desktopA);
    ASSERT_EQ(app.get_active_selection(), desktopA->getSelection());

    // Same path as the tab x / middle click / tab menu Close.
    docB->setModifiedSinceSave(false);
    app.desktopClose(desktopB);
    drainMainContext();

    EXPECT_EQ(app.get_active_desktop(), desktopA);
    ASSERT_NE(app.get_active_selection(), nullptr);
    EXPECT_EQ(app.get_active_selection(), desktopA->getSelection());
    EXPECT_EQ(app.get_active_document(), docA);
    EXPECT_EQ(widget->get_desktop(), desktopA);

    // The actions behind Delete, Ctrl+D and Ctrl+G now have a target.
    auto *rect = docA->getObjectById("rectA");
    ASSERT_TRUE(rect);
    auto count_rects = [&] {
        int n = 0;
        for (auto &child : docA->getRoot()->children) {
            if (child.getRepr() && std::string_view{child.getRepr()->name()} == "svg:rect") ++n;
        }
        return n;
    };
    auto *selection = desktopA->getSelection();
    selection->set(rect);

    ASSERT_TRUE(appActionEnabled("duplicate"));
    appActivate("duplicate");
    EXPECT_EQ(count_rects(), 2) << "Ctrl+D did nothing after the background tab closed";

    ASSERT_TRUE(appActionEnabled("selection-group"));
    appActivate("selection-group");
    EXPECT_EQ(count_rects(), 1) << "Ctrl+G did not group the duplicate";
    ASSERT_EQ(selection->size(), 1u);
    EXPECT_EQ(std::string_view{selection->items().front()->getRepr()->name()}, "svg:g");

    ASSERT_TRUE(appActionEnabled("delete"));
    appActivate("delete");
    EXPECT_TRUE(selection->isEmpty());
    EXPECT_EQ(count_rects(), 1) << "Delete left the grouped selection in place";
    EXPECT_TRUE(docA->getObjectById("rectA")) << "Delete removed the unselected original";
}

// ST-M D2/D3: with no document window left the active selection/document are null
// (macOS keeps the global menu bar alive). Every selection/document dependent app
// action must be disabled and activating it must be a no-op, not a null dereference.
TEST_F(DesktopOriginLifetimeTest, NullActiveStateMakesSelectionActionsNoOps)
{
    ASSERT_TRUE(desktopA && desktopB && docA && docB);
    auto &app = testApplication();

    docA->setModifiedSinceSave(false);
    docB->setModifiedSinceSave(false);
    std::string const state_a = documentState(*docA);
    std::string const state_b = documentState(*docB);

    // Close the last two desktops: the window goes away, the application stays.
    app.desktopClose(desktopB);
    app.desktopClose(desktopA);
    drainMainContext();
    ASSERT_EQ(app.get_active_selection(), nullptr);

    for (char const *name : {"delete", "duplicate", "cut", "copy", "paste-style", "selection-group",
                             "selection-ungroup", "selection-top", "path-union", "path-combine",
                             "object-to-path", "transform-remove", "transform-reapply", "unhide-all",
                             "unlock-all", "last-effect", "last-effect-pref", "edit-remove-filter",
                             "fit-canvas-to-selection", "object-flip-horizontal"}) {
        EXPECT_FALSE(appActionEnabled(name)) << name << " must be disabled without an active selection";
        appActivate(name); // must not crash
    }
    testApplication().gio_app()->activate_action("transform-translate", Glib::Variant<Glib::ustring>::create("5,5"));
    testApplication().gio_app()->activate_action("transform-rotate", Glib::Variant<double>::create(10.0));
    EXPECT_FALSE(appActionEnabled("transform-translate"));

    EXPECT_EQ(documentState(*docA), state_a);
    EXPECT_EQ(documentState(*docB), state_b);

    // A new active selection re-enables them (no stuck-disabled state).
    app.set_active_document(docA);
    app.set_active_selection(docA->getSelection());
    EXPECT_TRUE(appActionEnabled("delete"));
    EXPECT_TRUE(appActionEnabled("unhide-all"));
    app.set_active_selection(nullptr);
    EXPECT_FALSE(appActionEnabled("delete"));
}

int countRects(SPDocument *document)
{
    int n = 0;
    for (auto &child : document->getRoot()->children) {
        if (child.getRepr() && std::string_view{child.getRepr()->name()} == "svg:rect") ++n;
    }
    return n;
}

// ST-M review 1: closing the ACTIVE tab with a survivor hands the application's active
// state to the surviving tab (removeDesktop -> switchDesktop -> setActiveTab).
TEST_F(DesktopOriginLifetimeTest, ClosingActiveTabHandsStateToSurvivingTab)
{
    ASSERT_TRUE(desktopA && desktopB && docA && docB);
    auto &app = testApplication();

    widget->switchDesktop(desktopA);
    ASSERT_EQ(app.get_active_desktop(), desktopA);
    docA->setModifiedSinceSave(false);
    app.desktopClose(desktopA);
    drainMainContext();

    EXPECT_EQ(app.get_active_desktop(), desktopB);
    ASSERT_NE(app.get_active_selection(), nullptr);
    EXPECT_EQ(app.get_active_selection(), desktopB->getSelection());
    EXPECT_EQ(app.get_active_document(), docB);
    EXPECT_EQ(widget->get_desktop(), desktopB);

    auto *rect = docB->getObjectById("rectB");
    ASSERT_TRUE(rect);
    desktopB->getSelection()->set(rect);
    ASSERT_TRUE(appActionEnabled("duplicate"));
    appActivate("duplicate");
    EXPECT_EQ(countRects(docB), 2);
    ASSERT_TRUE(appActionEnabled("delete"));
    appActivate("delete"); // removes the selected duplicate only
    EXPECT_EQ(countRects(docB), 1);
    EXPECT_TRUE(docB->getObjectById("rectB"));
}

// ST-M review 2: the command-line path (activate_any_actions, which refuses disabled
// actions) must still run selection-dependent actions on a document that has a selection
// and no window or desktop, as process_document sets it up for headless runs.
TEST_F(DesktopOriginLifetimeTest, CommandLinePathRunsSelectionActionsWithoutDesktop)
{
    auto &app = testApplication();
    auto *docC = app.document_add(SPDocument::createNewDocFromMem(std::string_view{kSvgA}));
    ASSERT_TRUE(docC);
    auto restore = scope_exit([&] {
        app.set_active_selection(nullptr);
        app.set_active_document(nullptr);
        docC->setModifiedSinceSave(false);
        app.document_close(docC);
    });

    app.set_active_document(docC);
    app.set_active_selection(docC->getSelection());
    docC->getSelection()->set(docC->getObjectById("rectA"));
    ASSERT_TRUE(appActionEnabled("duplicate"));

    action_vector_t actions;
    actions.emplace_back("duplicate", Glib::VariantBase());
    actions.emplace_back("transform-translate", Glib::Variant<Glib::ustring>::create("7,0"));
    actions.emplace_back("selection-group", Glib::VariantBase());
    // Non-owning handle: the application outlives the call.
    activate_any_actions(actions, Glib::RefPtr<Gio::Application>(Glib::RefPtr<Gio::Application>{}, app.gio_app()),
                         nullptr, docC);

    EXPECT_EQ(countRects(docC), 1) << "duplicate then group: one rect left at top level";
    ASSERT_EQ(docC->getSelection()->size(), 1u);
    auto *group = docC->getSelection()->items().front();
    EXPECT_EQ(std::string_view{group->getRepr()->name()}, "svg:g");
    ASSERT_NE(group->getRepr()->firstChild(), nullptr);
    EXPECT_EQ(std::string_view{group->getRepr()->firstChild()->name()}, "svg:rect");
}

// ST-M review 3: Welcome -> new document after the last window closed (the same
// createDesktop path New/Open take with no window) re-enables the actions.
TEST_F(DesktopOriginLifetimeTest, NewDocumentAfterLastWindowClosedReenablesActions)
{
    ASSERT_TRUE(desktopA && desktopB && docA && docB);
    auto &app = testApplication();
    docA->setModifiedSinceSave(false);
    docB->setModifiedSinceSave(false);
    app.desktopClose(desktopB);
    app.desktopClose(desktopA);
    drainMainContext();
    ASSERT_EQ(app.get_active_selection(), nullptr);
    ASSERT_FALSE(appActionEnabled("duplicate"));

    auto *docN = app.document_add(SPDocument::createNewDocFromMem(std::string_view{kSvgA}));
    ASSERT_TRUE(docN);
    SPDesktop *desktopN = app.createDesktop(docN, false, true);
    ASSERT_TRUE(desktopN);
    auto cleanup = scope_exit([&] {
        docN->setModifiedSinceSave(false);
        app.desktopClose(desktopN);
        drainMainContext();
        app.document_close(docN);
    });
    drainMainContext();

    EXPECT_EQ(app.get_active_desktop(), desktopN);
    ASSERT_NE(app.get_active_selection(), nullptr);
    EXPECT_EQ(app.get_active_selection(), desktopN->getSelection());
    EXPECT_TRUE(appActionEnabled("duplicate"));
    EXPECT_TRUE(appActionEnabled("delete"));
    EXPECT_TRUE(appActionEnabled("unhide-all"));
    desktopN->getSelection()->set(docN->getObjectById("rectA"));
    appActivate("duplicate");
    EXPECT_EQ(countRects(docN), 2);
}

// ST-M review 4 (D4): file-open-window with no window opens exactly one window for the file.
TEST_F(DesktopOriginLifetimeTest, FileOpenWindowWithoutActiveWindowOpensOneWindow)
{
    ASSERT_TRUE(desktopA && desktopB && docA && docB);
    auto &app = testApplication();
    docA->setModifiedSinceSave(false);
    docB->setModifiedSinceSave(false);
    app.desktopClose(desktopB);
    app.desktopClose(desktopA);
    drainMainContext();
    ASSERT_EQ(app.get_active_window(), nullptr);
    ASSERT_EQ(app.get_number_of_windows(), 0);

    GError *error = nullptr;
    gchar *dir = g_dir_make_tmp("st-m-open-XXXXXX", &error);
    ASSERT_TRUE(dir) << (error ? error->message : "");
    std::string const path = std::string(dir) + "/opened.svg";
    ASSERT_TRUE(g_file_set_contents(path.c_str(), kSvgB, -1, nullptr));
    auto cleanup_files = scope_exit([&] {
        g_remove(path.c_str());
        g_rmdir(dir);
        g_free(dir);
    });

    testApplication().gio_app()->activate_action("file-open-window", Glib::Variant<Glib::ustring>::create(path));
    drainMainContext();

    EXPECT_EQ(app.get_number_of_windows(), 1);
    auto *desktop = app.get_active_desktop();
    ASSERT_NE(desktop, nullptr);
    auto *document = desktop->getDocument();
    ASSERT_TRUE(document);
    EXPECT_TRUE(document->getObjectById("rectB"));
    EXPECT_TRUE(Gio::File::create_for_path(document->getDocumentFilename())->equal(
        Gio::File::create_for_path(path)));
    EXPECT_EQ(app.get_active_selection(), desktop->getSelection());
    EXPECT_TRUE(appActionEnabled("duplicate"));

    document->setModifiedSinceSave(false);
    app.desktopClose(desktop);
    drainMainContext();
    app.document_close(document);
    EXPECT_EQ(app.get_number_of_windows(), 0);
}

} // namespace
