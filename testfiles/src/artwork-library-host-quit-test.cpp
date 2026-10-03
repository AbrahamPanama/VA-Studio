// SPDX-License-Identifier: GPL-2.0-or-later
// Separate executable: this test intentionally reaches Gio::Application::quit.
#include "ui/dialog/artwork-library-host.h"
#include "inkscape-application.h"
#include "inkscape.h"
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include <gtkmm/application.h>
#include <gtk/gtk.h>
#include <glibmm/main.h>
#include <gtest/gtest.h>
#include <cstdlib>
using namespace Inkscape;
using namespace Inkscape::UI::Dialog;
namespace {
sigc::connection respond(int response, bool &seen) {
    return Glib::signal_idle().connect([response, &seen] {
        auto list = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(list); ++i) {
            auto object = g_list_model_get_item(list, i);
            bool found = GTK_IS_MESSAGE_DIALOG(object) &&
                g_strcmp0(gtk_window_get_title(GTK_WINDOW(object)), "Artwork Library") == 0;
            if (found) { seen = true; gtk_dialog_response(GTK_DIALOG(object), response); }
            g_object_unref(object); if (found) return false;
        }
        return true;
    });
}
TEST(LibraryNativeQuit, CancelThenDeferredQuitKeepsLibraryReservedUntilDocumentLeaseUnwinds) {
    ASSERT_STREQ(std::getenv("INKSCAPE_TEST_GUI"), "1");
    if (!InkscapeApplication::instance()) {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "libraryhostquittest", true);
        new InkscapeApplication;
    }
    auto app = InkscapeApplication::instance(); app->gio_app()->register_application();
    if (!Application::exists()) Application::create(false);
    auto host = app->artworkLibraries(true); auto w = host->acquire("orphan-quit-test");
    w->new_collection("Original unsaved collection");
    auto doc = app->document_add(SPDocument::createNewDocFromMem(
        "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect width='2' height='3'/></svg>"));
    ASSERT_TRUE(doc); auto desktop = app->createDesktop(doc, false, true); ASSERT_TRUE(desktop);
    doc->ensureUpToDate(); DocumentUndo::done(doc, Util::Internal::ContextString("Fixture"), "");
    DocumentUndo::clearUndo(doc); DocumentUndo::clearRedo(doc); doc->setModifiedSinceSave(false);
    bool document_alive = true, desktop_alive = true;
    sigc::scoped_connection document_destroy = doc->connectDestroy([&] { document_alive = false; });
    sigc::scoped_connection desktop_destroy = desktop->connectDestroy([&](SPDesktop *) { desktop_alive = false; });
    bool cancelled = false;
    auto response = respond(GTK_RESPONSE_CANCEL, cancelled); app->on_quit(); response.disconnect();
    ASSERT_TRUE(cancelled); EXPECT_FALSE(app->quitPending()); EXPECT_TRUE(document_alive); EXPECT_TRUE(desktop_alive);
    EXPECT_FALSE(w->closing()); ASSERT_TRUE(w->active()); EXPECT_TRUE(w->active()->dirty());
    {
        auto operation = DocumentUndo::holdInteractionOperation(doc);
        bool discarded = false;
        response = respond(GTK_RESPONSE_REJECT, discarded); app->on_quit(); response.disconnect();
        ASSERT_TRUE(discarded); EXPECT_TRUE(app->quitPending()); EXPECT_TRUE(document_alive);
        EXPECT_TRUE(w->closing()); EXPECT_TRUE(w->active()); // discard is still provisional
        EXPECT_THROW(w->new_collection("Must not slip in before quit"), std::runtime_error);
        EXPECT_THROW(host->acquire("late-workspace"), std::runtime_error);
    }
    auto end = g_get_monotonic_time() + 10000000;
    while (app->quitPending() && g_get_monotonic_time() < end) { g_main_context_iteration(nullptr, false); g_usleep(1000); }
    EXPECT_FALSE(app->quitPending()); EXPECT_FALSE(document_alive); EXPECT_FALSE(desktop_alive);
    EXPECT_FALSE(app->documentHoldActive()); EXPECT_EQ(app->documentOperationCount(), 0u);
    EXPECT_TRUE(w->collections().empty()); EXPECT_TRUE(w->closing()); // no post-approval callback mutation window
}
}
