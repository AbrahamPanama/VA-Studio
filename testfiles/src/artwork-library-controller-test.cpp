// SPDX-License-Identifier: GPL-2.0-or-later
// Native isolated host; main must use INKSCAPE_TEST_GUI=1 and fatal criticals.
// No customer fixtures. No skip substitution for missing native prerequisites.
#include "ui/dialog/artwork-library-controller.h"
#include "ui/dialog/artwork-library-placement.h"
#include "ui/dialog/artwork-library-host.h"
#include "ui/dialog/dialog-container.h"
#include "ui/dialog/dialog-notebook.h"
#include "ui/dialog/dialog-manager.h"
#include "ui/dialog/dialog-data.h"
#include "ui/dialog/dialog-window.h"
#include "ui/builder-utils.h"
#include "inkscape-window.h"
#include "ui/widget/desktop-widget.h"
#include "ui/widget/canvas.h"
#include <glibmm/i18n.h>
#include "util/scope_exit.h"
#include "util/value-utils.h"
#include "preferences.h"
#include <gtkmm/centerbox.h>
#include <gtkmm/listbox.h>
#include <gtkmm/picture.h>
#include <gtkmm/scale.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/searchentry2.h>
#include <glibmm/main.h>
#include <giomm/simpleaction.h>
#include <gtkmm/gridview.h>
#include <gtkmm/window.h>
#include <gtkmm/dialog.h>
#include <gtkmm/settings.h>
#include "desktop.h"
#include "enums.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-root.h"
#include "selection.h"
#include "xml/repr.h"
#include "undo-stack-observer.h"
#include <gtkmm/application.h>
#include <gtk/gtk.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <utility>
#include <vector>

using namespace Inkscape;
using namespace Inkscape::UI::Dialog;
namespace {
struct DiscardForTest : LibraryCloseUI {
    LibraryCloseAnswer ask(LibraryClosePrompt const &p) override {
        return {p.kind == LibraryCloseKind::Work ? LibraryCloseChoice::Wait : LibraryCloseChoice::Discard};
    }
    bool wait(std::shared_ptr<ArtworkLibraryWorkspace> const &w, std::function<void()> const &) override {
        auto end = g_get_monotonic_time() + 10000000;
        while (w->busy() && g_get_monotonic_time() < end) { g_main_context_iteration(nullptr, false); g_usleep(1000); }
        return !w->busy();
    }
    void error(std::string const &s) override { ADD_FAILURE() << s; }
};
bool until(std::function<bool()> predicate) {
    auto end = g_get_monotonic_time() + 10000000;
    while (!predicate() && g_get_monotonic_time() < end) { g_main_context_iteration(nullptr, false); g_usleep(1000); }
    return predicate();
}
template <class T> T *descendant(Gtk::Widget &root) {
    if (auto found = dynamic_cast<T *>(&root)) return found;
    for (auto child = root.get_first_child(); child; child = child->get_next_sibling())
        if (auto found = descendant<T>(*child)) return found;
    return nullptr;
}
sigc::connection answer_library_close(int response, bool &answered) {
    return Glib::signal_idle().connect([response, &answered] {
        auto windows = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
            auto item = g_list_model_get_item(windows, i);
            bool match = GTK_IS_MESSAGE_DIALOG(item) &&
                g_strcmp0(gtk_window_get_title(GTK_WINDOW(item)), "Artwork Library") == 0;
            if (match) { answered = true; gtk_dialog_response(GTK_DIALOG(item), response); }
            g_object_unref(item); if (match) return false;
        }
        return true;
    });
}
sigc::connection answer_recovery_review(int response, bool &answered,
                                       std::function<void(GtkDialog *)> inspect = {}) {
    return Glib::signal_idle().connect([response, &answered, inspect = std::move(inspect)] {
        auto windows = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
            auto item = g_list_model_get_item(windows, i);
            bool match = GTK_IS_DIALOG(item) &&
                g_strcmp0(gtk_window_get_title(GTK_WINDOW(item)), _("Recover artwork collection")) == 0;
            if (match) {
                answered = true;
                if (inspect) inspect(GTK_DIALOG(item));
                gtk_dialog_response(GTK_DIALOG(item), response);
            }
            g_object_unref(item); if (match) return false;
        }
        return true;
    });
}
std::string xml() { return "<svg xmlns='http://www.w3.org/2000/svg' width='25.4mm' height='25.4mm' viewBox='0 0 96 96'><rect id='original' width='10' height='20'/></svg>"; }
Art::ValidatedSvg token() { auto s = xml(); Art::Bytes b(s.begin(), s.end()); return Art::preflight_svg(b, {"b203e8e9-640c-4195-b0ea-f9b790245678", Art::artwork_sha256(b), 25.4, 25.4}); }
struct History : UndoStackObserver {
    unsigned commits = 0;
    void notifyUndoEvent(Event *) override {} void notifyRedoEvent(Event *) override {}
    void notifyUndoCommitEvent(Event *) override { ++commits; } void notifyUndoExpired(Event *) override {}
    void notifyClearUndoEvent() override {} void notifyClearRedoEvent() override {}
};
class ControllerTest : public ::testing::Test {
protected:
    virtual bool settle_fixture_history() const { return true; }
    void SetUp() override {
        ASSERT_STREQ(std::getenv("INKSCAPE_TEST_GUI"), "1") << "Native host required, not a skipped test";
        if (!InkscapeApplication::instance()) { Gtk::Application::wrap_in_search_entry2(); g_setenv("INKSCAPE_APP_ID_TAG", "librarycontrollertest", true); new InkscapeApplication; }
        app = InkscapeApplication::instance(); app->gio_app()->register_application();
        if (!Application::exists()) Application::create(false);
        doc = app->document_add(SPDocument::createNewDocFromMem(xml())); ASSERT_TRUE(doc);
        desktop = app->createDesktop(doc, false, true); ASSERT_TRUE(desktop); doc->ensureUpToDate();
        if (settle_fixture_history()) {
            DocumentUndo::done(doc, Util::Internal::ContextString("Fixture"), ""); DocumentUndo::clearUndo(doc); DocumentUndo::clearRedo(doc); doc->setModifiedSinceSave(false);
        }
        doc->addUndoObserver(history); workspace = ArtworkLibraryWorkspace::create(); workspace->new_collection("Synthetic");
    }
    void TearDown() override {
        panel.reset();
        // Retained orphan slots are intentional in production; test teardown
        // makes an explicit library-only discard without driving a native prompt.
        if (auto host = app->artworkLibraries()) {
            DiscardForTest ui; auto guard = host->prepare(host->slots(), ui);
            ASSERT_TRUE(guard); guard->commit(); guard.reset();
        }
        doc->removeUndoObserver(history); doc->setModifiedSinceSave(false); app->destroyDesktop(desktop);
        for (unsigned i = 0; i < 64 && g_main_context_pending(nullptr); ++i) g_main_context_iteration(nullptr, false);
    }
    InkscapeApplication *app = nullptr; SPDocument *doc = nullptr; SPDesktop *desktop = nullptr; History history;
    std::shared_ptr<ArtworkLibraryWorkspace> workspace; std::unique_ptr<ArtworkLibraryController> panel;
    std::string recovery_candidate() {
        auto raw = g_dir_make_tmp("vacards-library-recovery-ui-XXXXXX", nullptr);
        if (!raw) throw std::runtime_error("No recovery test directory");
        auto physical = std::filesystem::canonical(raw).u8string(); g_free(raw);
        std::string directory(physical.begin(), physical.end()); RecordProperty("recovery_directory", directory);
        auto path = directory + "/.valib-stage-f1559ad5-c9cd-47f5-bce2-1f1712c62f26";
        auto asset = token();
        workspace->add({"", "Recovery fixture", {}, 25.4, 25.4},
                       Art::Bytes(asset.svg_bytes()->begin(), asset.svg_bytes()->end()));
        if (!until([&] { return !workspace->busy(); })) throw std::runtime_error("Recovery fixture add timed out");
        auto saved = Art::save_library(path, workspace->active()->catalog.snapshot());
        if (!saved.version) throw std::runtime_error(saved.message);
        workspace->scan_recovery(directory);
        if (!until([&] { return !workspace->busy(); }) || !workspace->recovery_scan() ||
            workspace->recovery_scan()->files.size() != 1 || !workspace->recovery_scan()->files.front().version)
            throw std::runtime_error("Recovery fixture scan failed: " + workspace->message());
        return path;
    }
};
TEST_F(ControllerTest, SidebarSwitchesNamedLibrariesWithoutReplacingOrEditingThem) {
    auto first = workspace->active_id();
    workspace->new_collection("Tarjetas"); auto second = workspace->active_id();
    workspace->new_collection("Lámparas"); auto third = workspace->active_id();
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    auto list = descendant<Gtk::ListBox>(*panel); ASSERT_TRUE(list);
    ASSERT_TRUE(list->get_row_at_index(2)); EXPECT_FALSE(list->get_row_at_index(3));
    EXPECT_EQ(list->get_selected_row()->get_name(), third);
    auto label = descendant<Gtk::Label>(*list->get_row_at_index(1)); ASSERT_TRUE(label);
    // A newly created, unsaved library carries the existing dirty marker.
    EXPECT_EQ(label->get_text(), "Tarjetas *");
    list->select_row(*list->get_row_at_index(1));
    EXPECT_EQ(workspace->active_id(), second);
    list->select_row(*list->get_row_at_index(0));
    EXPECT_EQ(workspace->active_id(), first);
    EXPECT_EQ(workspace->collections().size(), 3u);
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(ControllerTest, RemoveLockActionRequiresASavedCollection) {
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    EXPECT_FALSE(panel->actions()->get_action_enabled("remove-lock")); // "Synthetic" was never saved.
    std::unique_ptr<gchar, decltype(&g_free)> folder(g_dir_make_tmp("vacards-lock-action-XXXXXX", nullptr), &g_free);
    ASSERT_TRUE(folder);
    // Library storage refuses symlinked parents (macOS /var -> /private/var).
    auto const real = std::filesystem::canonical(std::filesystem::u8path(folder.get())).u8string();
    auto const file = std::string(real.begin(), real.end()) + "/Lock check.valib";
    workspace->save(file);
    ASSERT_TRUE(until([&] { return !workspace->busy(); })) << workspace->message();
    ASSERT_FALSE(workspace->active()->path.empty()) << workspace->message();
    EXPECT_TRUE(panel->actions()->get_action_enabled("remove-lock"));
    std::error_code ignored; std::filesystem::remove_all(std::filesystem::u8path(folder.get()), ignored);
}
TEST_F(ControllerTest, ReopenedLibrariesPopulateTheSidebarWithoutManualLoading) {
    auto raw = g_dir_make_tmp("vacards-library-sidebar-restart-XXXXXX", nullptr); ASSERT_TRUE(raw);
    auto directory = std::filesystem::canonical(raw).string(); g_free(raw);
    RecordProperty("evidence_directory", directory);
    auto key = "/test/sidebar-session/" + std::filesystem::path(directory).filename().string();
    auto host = ArtworkLibraryHost::create(key);
    auto saved = host->acquire("sidebar");
    saved->new_collection("Tarjetas");
    auto first = saved->active_id();
    saved->save(directory + "/Tarjetas.valib");
    ASSERT_TRUE(until([&] { return !saved->busy(); }));
    ASSERT_FALSE(saved->active()->dirty()) << saved->message();
    saved->new_collection("Lámparas");
    saved->save(directory + "/Lámparas.valib");
    ASSERT_TRUE(until([&] { return !saved->busy(); }));
    ASSERT_FALSE(saved->active()->dirty()) << saved->message();
    saved->select_collection(first);
    saved.reset(); host.reset();
    host = ArtworkLibraryHost::create(key);
    saved = host->acquire("sidebar");
    panel = std::make_unique<ArtworkLibraryController>(saved); panel->setDesktop(desktop);
    Gtk::Window window;
    window.set_default_size(620, 520); window.set_child(*panel);
    app->gtk_app()->add_window(window);
    auto cleanup = scope_exit{[&] { window.unset_child(); app->gtk_app()->remove_window(window); }};
    window.present();
    ASSERT_TRUE(until([&] { return panel->get_mapped() && !saved->busy(); }));
    auto list = descendant<Gtk::ListBox>(*panel); ASSERT_TRUE(list);
    ASSERT_TRUE(until([&] { return bool(list->get_row_at_index(1)); }));
    EXPECT_FALSE(list->get_row_at_index(2));
    ASSERT_TRUE(list->get_selected_row());
    EXPECT_EQ(list->get_selected_row()->get_name(), first);
    auto label = descendant<Gtk::Label>(*list->get_row_at_index(0)); ASSERT_TRUE(label);
    EXPECT_EQ(label->get_text(), "Tarjetas");
    list->select_row(*list->get_row_at_index(1));
    EXPECT_EQ(saved->active()->label, "Lámparas");
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}

class ControllerImportTest : public ControllerTest {
protected:
    void SetUp() override {
        ControllerTest::SetUp();
        auto raw = g_dir_make_tmp("vacards-import-ui-XXXXXX", nullptr); ASSERT_TRUE(raw);
        directory = std::filesystem::canonical(raw).string(); g_free(raw);
        RecordProperty("evidence_directory", directory);
        panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
        window = std::make_unique<Gtk::Window>();
        window->set_title("Artwork import and scroll check");
        window->set_default_size(620, 520); window->set_child(*panel);
        app->gtk_app()->add_window(*window); window->present();
        ASSERT_TRUE(until([&] { return panel->get_mapped(); }));
    }
    void TearDown() override {
        response.disconnect();
        if (window) { window->unset_child(); app->gtk_app()->remove_window(*window); window.reset(); }
        ControllerTest::TearDown();
    }
    std::string input(std::string const &name, std::string const &contents) {
        auto path = directory + "/" + name;
        if (!g_file_set_contents(path.c_str(), contents.data(), contents.size(), nullptr))
            throw std::runtime_error("Cannot write import fixture");
        return path;
    }
    void answer_report(int value) {
        response = Glib::signal_idle().connect([this, value] {
            auto windows = gtk_window_get_toplevels();
            for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
                auto item = g_list_model_get_item(windows, i);
                bool match = GTK_IS_DIALOG(item) && g_strcmp0(gtk_window_get_title(GTK_WINDOW(item)),
                    "Review import results — only Ready entries will be added") == 0;
                if (match) { reported = true; gtk_dialog_response(GTK_DIALOG(item), value); }
                g_object_unref(item); if (match) return false;
            }
            return true;
        });
    }
    void frames(unsigned ms) {
        auto end = g_get_monotonic_time() + ms * 1000;
        while (g_get_monotonic_time() < end) { g_main_context_iteration(nullptr, false); g_usleep(1000); }
    }
    std::unique_ptr<Gtk::Window> window;
    sigc::scoped_connection response;
    std::string directory;
    bool reported = false;
};

TEST_F(ControllerImportTest, SuccessfulImportLoadsWithoutManualReview) {
    answer_report(GTK_RESPONSE_CANCEL); // An unexpected prompt must fail, not hang.
    workspace->open(input("Award.svg", xml()));
    ASSERT_TRUE(until([&] { return !workspace->busy() && !workspace->pending_import(); }));
    EXPECT_FALSE(reported);
    EXPECT_EQ(workspace->collections().size(), 2u); EXPECT_EQ(workspace->rows().size(), 1u);
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(ControllerImportTest, PartialImportOpensItsReportAutomatically) {
    answer_report(GTK_RESPONSE_ACCEPT);
    workspace->import_files({input("Good.svg", xml()), input("Broken.svg", "<svg broken")});
    ASSERT_TRUE(until([&] { return !workspace->busy() && !workspace->pending_import(); }));
    EXPECT_TRUE(reported); EXPECT_EQ(workspace->rows().size(), 1u);
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(ControllerImportTest, CancellingPartialImportPreservesTheLibrary) {
    auto original = workspace->active()->catalog.snapshot().manifest().serialize();
    answer_report(GTK_RESPONSE_CANCEL);
    workspace->import_files({input("Good.svg", xml()), input("Broken.svg", "<svg broken")});
    ASSERT_TRUE(until([&] { return !workspace->busy() && !workspace->pending_import(); }));
    EXPECT_TRUE(reported);
    EXPECT_EQ(workspace->active()->catalog.snapshot().manifest().serialize(), original);
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(ControllerImportTest, LoadingCellsKeepTheConfiguredThumbnailSize) {
    auto grid = descendant<Gtk::GridView>(*panel); ASSERT_TRUE(grid);
    auto scale = descendant<Gtk::Scale>(*panel); ASSERT_TRUE(scale);
    struct BindSizes { int expected; unsigned bound = 0, unstable = 0; } sizes{int(scale->get_value())};
    auto factory = grid->get_factory();
    auto connection = g_signal_connect_after(factory->gobj(), "bind",
        G_CALLBACK((+[](GtkSignalListItemFactory *, GtkListItem *item, gpointer data) {
            auto &sizes = *static_cast<BindSizes *>(data);
            auto box = gtk_list_item_get_child(item);
            if (!GTK_IS_CENTER_BOX(box)) return;
            auto picture = gtk_center_box_get_start_widget(GTK_CENTER_BOX(box));
            int width = 0, height = 0; gtk_widget_get_size_request(picture, &width, &height);
            ++sizes.bound;
            if (width != sizes.expected || height != sizes.expected) ++sizes.unstable;
        })), &sizes);
    auto disconnect = scope_exit{[&] { g_signal_handler_disconnect(factory->gobj(), connection); }};
    workspace->open(input("New-artwork.svg", xml()));
    ASSERT_TRUE(until([&] { return sizes.bound && !workspace->busy() && !workspace->pending_import(); }));
    EXPECT_EQ(sizes.unstable, 0u) << "An unrendered thumbnail was bound with a collapsing cell size";
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(ControllerImportTest, PreviouslyViewedThumbnailsStayVisibleDuringRapidScrolling) {
    // Optional local customer-file exercise; no private path/artwork is stored
    // in the repository. Normal regression execution uses generated SVG fixtures.
    auto private_input = std::getenv("VACARDS_LIBRARY_SCROLL_INPUT");
    if (private_input) {
        answer_report(GTK_RESPONSE_ACCEPT);
        workspace->open(private_input);
    } else {
        std::vector<std::string> files;
        for (unsigned i = 0; i < 48; ++i) files.push_back(input("Artwork-" + std::to_string(i) + ".svg", xml()));
        workspace->import_files(std::move(files));
    }
    ASSERT_TRUE(until([&] { return !workspace->busy() && !workspace->pending_import(); }));
    if (!private_input) ASSERT_EQ(workspace->rows().size(), 48u);
    RecordProperty("artwork_count", workspace->rows().size());
    auto grid = descendant<Gtk::GridView>(*panel); ASSERT_TRUE(grid);
    Gtk::ScrolledWindow *scroll = nullptr;
    for (auto parent = grid->get_parent(); parent && !scroll; parent = parent->get_parent())
        scroll = dynamic_cast<Gtk::ScrolledWindow *>(parent);
    ASSERT_TRUE(scroll);
    auto counts = [&] {
        std::pair<unsigned, unsigned> result{};
        for (auto child = grid->get_first_child(); child; child = child->get_next_sibling()) {
            auto box = dynamic_cast<Gtk::CenterBox *>(child->get_first_child());
            auto picture = box ? dynamic_cast<Gtk::Picture *>(box->get_start_widget()) : nullptr;
            graphene_rect_t bounds;
            if (!picture || !child->get_child_visible() ||
                !gtk_widget_compute_bounds(child->gobj(), GTK_WIDGET(scroll->gobj()), &bounds) ||
                bounds.origin.y + bounds.size.height <= 0 || bounds.origin.y >= scroll->get_height()) continue;
            ++result.first; if (!picture->get_paintable()) ++result.second;
        }
        return result;
    };
    ASSERT_TRUE(until([&] { auto [visible, blank] = counts(); return !workspace->busy() && visible && !blank; }));
    auto adjustment = scroll->get_vadjustment();
    ASSERT_GT(adjustment->get_upper(), adjustment->get_page_size());
    auto pool = app->artworkLibraryThumbnailPool();
    // Visit every row first. First-time loading is allowed; returning to already
    // displayed artwork must not blank it while repeating validation.
    for (unsigned step = 0; step <= 10; ++step) {
        adjustment->set_value((adjustment->get_upper() - adjustment->get_page_size()) * step / 10.);
        frames(120);
        ASSERT_TRUE(until([&] { auto [visible, blank] = counts(); return !workspace->busy() && visible && !blank; }));
    }
    adjustment->set_value(0); frames(120);
    ASSERT_TRUE(until([&] { auto [visible, blank] = counts(); return !workspace->busy() && visible && !blank; }));
    auto baseline = pool->stats(); auto generation = workspace->page_generation();
    unsigned blank_frames = 0, peak_blanks = 0;
    for (unsigned step = 1; step <= 20; ++step) {
        auto bottom = adjustment->get_upper() - adjustment->get_page_size();
        adjustment->set_value(bottom * (step <= 10 ? step : 20 - step) / 10.);
        frames(20);
        auto [visible, blank] = counts();
        if (blank) ++blank_frames;
        peak_blanks = std::max(peak_blanks, blank);
    }
    auto stopped = g_get_monotonic_time();
    bool settled = until([&] { auto [visible, blank] = counts(); return !workspace->busy() && visible && !blank; });
    auto stats = pool->stats();
    RecordProperty("blank_frames_of_20", blank_frames);
    RecordProperty("peak_visible_blank_cells", peak_blanks);
    RecordProperty("page_generation_changes", workspace->page_generation() - generation);
    RecordProperty("settle_after_scroll_ms", (g_get_monotonic_time() - stopped) / 1000);
    RecordProperty("cache_bytes", stats.charged);
    RecordProperty("new_backing_allocations", stats.backing_allocations - baseline.backing_allocations);
    EXPECT_TRUE(settled) << "Visible thumbnails did not recover after scrolling stopped";
    EXPECT_EQ(blank_frames, 0u) << "Already viewed thumbnails disappeared while scrolling";
    EXPECT_EQ(stats.backing_allocations, baseline.backing_allocations);
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}

TEST_F(ControllerTest, RecoveryReviewCancellationPreservesCollectionAndDocument) {
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    EXPECT_TRUE(panel->actions()->get_action_enabled("find-recovery"));
    EXPECT_FALSE(panel->actions()->get_action_enabled("review-recovery"));
    auto path = recovery_candidate();
    EXPECT_TRUE(panel->actions()->get_action_enabled("review-recovery"));
    auto before = sp_repr_save_buf(doc->getReprDoc()); auto id = workspace->active_id();
    auto source = Art::load_library(path).version; bool answered = false;
    sigc::scoped_connection answer = answer_recovery_review(int(Gtk::ResponseType::CANCEL), answered, [&](auto dialog) {
        EXPECT_FALSE(panel->actions()->get_action_enabled("find-recovery"));
        EXPECT_FALSE(panel->actions()->get_action_enabled("review-recovery"));
        EXPECT_TRUE(gtk_widget_get_sensitive(gtk_dialog_get_widget_for_response(dialog, int(Gtk::ResponseType::ACCEPT))));
    });
    panel->actions()->activate_action("review-recovery");
    EXPECT_TRUE(answered); EXPECT_EQ(workspace->active_id(), id);
    EXPECT_EQ(workspace->collections().size(), 1u); EXPECT_FALSE(workspace->busy());
    EXPECT_EQ(Art::load_library(path).version, source);
    EXPECT_TRUE(panel->actions()->get_action_enabled("review-recovery"));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before); EXPECT_EQ(history.commits, 0u);
}
TEST_F(ControllerTest, RecoveryReviewAcceptCreatesIndependentCollectionWithoutEditingCanvas) {
    auto path = recovery_candidate(); auto original = Art::load_library(path).version;
    auto prior = workspace->active_id(); auto before = sp_repr_save_buf(doc->getReprDoc());
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    bool answered = false;
    sigc::scoped_connection answer = answer_recovery_review(int(Gtk::ResponseType::ACCEPT), answered);
    panel->actions()->activate_action("review-recovery");
    ASSERT_TRUE(answered); ASSERT_TRUE(until([&] { return !workspace->busy(); }));
    EXPECT_NE(workspace->active_id(), prior) << workspace->message();
    EXPECT_EQ(workspace->collections().size(), 2u);
    EXPECT_TRUE(workspace->active()->dirty()); EXPECT_TRUE(workspace->active()->path.empty());
    EXPECT_FALSE(workspace->active()->version);
    EXPECT_EQ(Art::load_library(path).version, original);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before); EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(ControllerTest, RecoveryReviewRejectsCandidateChangedDuringNativePrompt) {
    auto path = recovery_candidate(); auto prior = workspace->active_id();
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    bool answered = false;
    sigc::scoped_connection answer = answer_recovery_review(int(Gtk::ResponseType::ACCEPT), answered, [&](auto) {
        auto loaded = Art::load_library(path); auto changed = Art::Catalog::from_package(loaded.package);
        changed.rename_library("Changed while the prompt was open");
        EXPECT_EQ(Art::save_library(path, changed.snapshot(), loaded.version).publication, Art::Publication::Published);
    });
    panel->actions()->activate_action("review-recovery");
    ASSERT_TRUE(answered); ASSERT_TRUE(until([&] { return !workspace->busy(); }));
    EXPECT_EQ(workspace->active_id(), prior); EXPECT_EQ(workspace->collections().size(), 1u);
    EXPECT_NE(workspace->message().find("changed since discovery"), std::string::npos);
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(ControllerTest, DisposingPanelDuringRecoveryPromptCannotPublishItsOldChoice) {
    recovery_candidate(); auto prior = workspace->active_id();
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    auto actions = panel->actions(); bool answered = false;
    sigc::scoped_connection answer = answer_recovery_review(int(Gtk::ResponseType::ACCEPT), answered, [&](auto) { panel.reset(); });
    actions->activate_action("review-recovery");
    EXPECT_TRUE(answered); EXPECT_FALSE(panel); EXPECT_FALSE(workspace->busy());
    EXPECT_EQ(workspace->active_id(), prior); EXPECT_EQ(workspace->collections().size(), 1u);
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(ControllerTest, DamagedRecoveryCandidateDisablesAcceptAndCannotBeForcedThroughAction) {
    auto path = recovery_candidate();
    ASSERT_TRUE(g_file_set_contents(path.c_str(), "damaged", 7, nullptr));
    workspace->scan_recovery(std::filesystem::path(path).parent_path().string());
    ASSERT_TRUE(until([&] { return !workspace->busy(); }));
    ASSERT_TRUE(workspace->recovery_scan()); ASSERT_EQ(workspace->recovery_scan()->files.size(), 1u);
    ASSERT_FALSE(workspace->recovery_scan()->files.front().version);
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    auto prior = workspace->active_id(); bool answered = false;
    sigc::scoped_connection answer = answer_recovery_review(int(Gtk::ResponseType::ACCEPT), answered, [&](auto dialog) {
        EXPECT_FALSE(gtk_widget_get_sensitive(gtk_dialog_get_widget_for_response(dialog, int(Gtk::ResponseType::ACCEPT))));
    });
    panel->actions()->activate_action("review-recovery");
    EXPECT_TRUE(answered); EXPECT_EQ(workspace->active_id(), prior); EXPECT_FALSE(workspace->busy());
    EXPECT_EQ(workspace->collections().size(), 1u); EXPECT_EQ(history.commits, 0u);
}
TEST_F(ControllerTest, ConstructionAndActionEnablementDoNotTouchDocument) {
    auto before = sp_repr_save_buf(doc->getReprDoc()); panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    EXPECT_TRUE(panel->actions()->get_action_enabled("open")); EXPECT_FALSE(panel->actions()->get_action_enabled("insert"));
    EXPECT_FALSE(panel->actions()->get_action_enabled("add-selection"));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before); EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(ControllerTest, ReservedConstructionFailureRetainsWorkspaceAndReleasesEscapedCallbacks) {
    auto host = app->artworkLibraries(true);
    auto slot = host->retain(workspace);
    auto reservation = host->reserve_panel(slot);
    auto before = sp_repr_save_buf(doc->getReprDoc());
    // This deliberately lacks the reservation token: failure happens late at
    // host attachment, after GTK/actions/changed callbacks were constructed.
    EXPECT_THROW(std::make_unique<ArtworkLibraryController>(workspace), std::runtime_error);
    EXPECT_THROW(host->reserve_panel(slot), std::runtime_error);
    workspace->new_collection("Changed after failed construction");
    panel = std::make_unique<ArtworkLibraryController>(std::move(reservation));
    EXPECT_EQ(panel->host_slot(), slot);
    EXPECT_TRUE(panel->actions()->get_action_enabled("save"));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
    panel.reset();
    auto reopened = host->reserve_panel(slot);
    EXPECT_EQ(reopened->workspace(), workspace);
    EXPECT_EQ(reopened->workspace()->collections().size(), 2u);
}
TEST_F(ControllerTest, StaleControllerDestructionCannotDetachReplacementPanel) {
    auto host = app->artworkLibraries(true);
    auto slot = host->retain(workspace);
    panel = std::make_unique<ArtworkLibraryController>(host->reserve_panel(slot));
    auto old = std::move(panel);
    host->detach(slot); // Deliberate test-only retirement before old wrapper disposal.
    panel = std::make_unique<ArtworkLibraryController>(host->reserve_panel(slot));
    old.reset();
    EXPECT_THROW(host->reserve_panel(slot), std::runtime_error);
    EXPECT_TRUE(panel->actions()->get_action_enabled("save"));
    panel.reset();
    EXPECT_NO_THROW(host->reserve_panel(slot));
}
TEST_F(ControllerTest, ForeignReservationCannotLeakApplicationWorkspaceSlots) {
    auto host = app->artworkLibraries(true);
    auto before = host->slots();
    auto foreign = ArtworkLibraryHost::create();
    for (unsigned i = 0; i < 65; ++i) {
        EXPECT_THROW(std::make_unique<ArtworkLibraryController>(foreign->reserve_panel()), std::invalid_argument);
        EXPECT_EQ(host->slots(), before);
        EXPECT_TRUE(foreign->slots().empty());
    }
}
TEST_F(ControllerTest, PublicMenuAndWindowActionOpenExactlyOneArtworkPanel) {
    auto const &data = get_dialog_data();
    ASSERT_TRUE(data.contains("ArtworkLibrary"));
    EXPECT_EQ(data.at("ArtworkLibrary").category, DialogData::Assets);
    EXPECT_EQ(data.at("ArtworkLibrary").provide_scroll, ScrollProvider::PROVIDE);
    ASSERT_TRUE(data.contains("Symbols"));
    auto builder = UI::create_builder("menus.ui");
    auto menu = builder->get_object("object-menu"); ASSERT_TRUE(menu);
    std::function<unsigned(GMenuModel *, char const *)> count_target = [&](GMenuModel *model, char const *expected) {
        unsigned count = 0;
        for (int i = 0; i < g_menu_model_get_n_items(model); ++i) {
            gchar *action = nullptr, *target = nullptr;
            g_menu_model_get_item_attribute(model, i, "action", "s", &action);
            g_menu_model_get_item_attribute(model, i, "target", "s", &target);
            if (g_strcmp0(action, "win.dialog-open") == 0 && g_strcmp0(target, expected) == 0) ++count;
            g_free(action); g_free(target);
            for (auto link : {G_MENU_LINK_SECTION, G_MENU_LINK_SUBMENU})
                if (auto child = g_menu_model_get_item_link(model, i, link)) {
                    count += count_target(child, expected); g_object_unref(child);
                }
        }
        return count;
    };
    EXPECT_EQ(count_target(G_MENU_MODEL(menu->gobj()), "ArtworkLibrary"), 1u);
    EXPECT_EQ(count_target(G_MENU_MODEL(menu->gobj()), "Symbols"), 1u);
    auto prefs = Preferences::get(); auto old = prefs->getInt("/options/dialogtype/value", PREFS_DIALOGS_BEHAVIOR_DOCKABLE);
    auto restore = scope_exit{[&] { prefs->setInt("/options/dialogtype/value", old); }};
    prefs->setInt("/options/dialogtype/value", PREFS_DIALOGS_BEHAVIOR_DOCKABLE);
    DialogManager::singleton().remove_dialog_floating_state("ArtworkLibrary");
    auto win = desktop->getInkscapeWindow(); auto container = desktop->getContainer();
    auto before = sp_repr_save_buf(doc->getReprDoc());
    auto action = win->lookup_action("dialog-open"); ASSERT_TRUE(action);
    auto value = Glib::Variant<Glib::ustring>::create("ArtworkLibrary");
    action->activate(value);
    auto library = dynamic_cast<ArtworkLibraryController *>(container->get_dialog("ArtworkLibrary"));
    ASSERT_TRUE(library); auto slot = library->host_slot();
    auto close_owned_tab = scope_exit{[&] {
        // Real native cleanup before desktop teardown saves its layout. Do not
        // leave a newly registered panel to be restored by unrelated fixtures.
        auto widget = container->get_dialog("ArtworkLibrary");
        for (auto parent = widget ? widget->get_parent() : nullptr; parent; parent = parent->get_parent()) {
            if (auto notebook = dynamic_cast<DialogNotebook *>(parent)) { notebook->close_tab(widget); break; }
        }
    }};
    action->activate(value);
    EXPECT_EQ(container->get_dialog("ArtworkLibrary"), library);
    EXPECT_EQ(app->artworkLibraries()->live_panel(slot), library);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before); EXPECT_EQ(history.commits, 0u);
}
TEST_F(ControllerTest, ReentrantFactoryOpenCannotCreateDuplicatePanel) {
    DialogContainer container(desktop->getInkscapeWindow()); DialogNotebook notebook(&container);
    auto host = app->artworkLibraries(true); bool reentered = false;
    sigc::scoped_connection changed = host->changed.connect([&] {
        if (reentered) return; reentered = true;
        container.new_dialog("ArtworkLibrary", &notebook, false);
    });
    container.new_dialog("ArtworkLibrary", &notebook, false);
    ASSERT_TRUE(reentered); ASSERT_TRUE(container.get_dialog("ArtworkLibrary"));
    EXPECT_EQ(notebook.get_notebook()->get_n_pages(), 1);
    EXPECT_EQ(host->within(notebook).size(), 1u);
}
TEST_F(ControllerTest, FactoryReservationSpansNativePageInsertion) {
    struct Refusal : DiscardForTest { unsigned errors = 0; void error(std::string const &) override { ++errors; } } ui;
    DialogContainer container(desktop->getInkscapeWindow()); DialogNotebook notebook(&container);
    auto host = app->artworkLibraries(true); bool inserted = false;
    sigc::scoped_connection added = notebook.get_notebook()->signal_page_added().connect([&](Gtk::Widget *, guint) {
        inserted = true;
        EXPECT_FALSE(host->prepare({}, ui));
    });
    container.new_dialog("ArtworkLibrary", &notebook, false);
    EXPECT_TRUE(inserted); EXPECT_EQ(ui.errors, 1u);
    EXPECT_TRUE(host->prepare({}, ui));
}
TEST_F(ControllerTest, ReservationCallbackCanDestroyDestinationWithoutPublishingPanel) {
    auto host = app->artworkLibraries(true); auto before = host->slots();
    auto container = std::make_unique<DialogContainer>(desktop->getInkscapeWindow());
    auto raw = container.get();
    sigc::scoped_connection changed = host->changed.connect([&] { container.reset(); });
    raw->new_dialog("ArtworkLibrary", nullptr, false);
    EXPECT_FALSE(container); EXPECT_EQ(host->slots(), before);
}
TEST_F(ControllerTest, IndependentDocksAndMovedPanelUseWorkspaceIdentity) {
    DialogContainer first(desktop->getInkscapeWindow()), second(desktop->getInkscapeWindow()), third(desktop->getInkscapeWindow());
    DialogNotebook a(&first), b(&second), c(&third);
    first.new_dialog("ArtworkLibrary", &a, false);
    second.new_dialog("ArtworkLibrary", &b, false);
    auto one = dynamic_cast<ArtworkLibraryController *>(first.get_dialog("ArtworkLibrary"));
    auto two = dynamic_cast<ArtworkLibraryController *>(second.get_dialog("ArtworkLibrary"));
    ASSERT_TRUE(one && two); EXPECT_NE(one->host_slot(), two->host_slot());
    auto host = app->artworkLibraries();
    EXPECT_NE(host->acquire(one->host_slot()), host->acquire(two->host_slot()));
    c.move_page(*one);
    first.new_dialog("ArtworkLibrary", &a, false);
    third.new_dialog("ArtworkLibrary", &c, false);
    EXPECT_EQ(a.get_notebook()->get_n_pages(), 0);
    EXPECT_EQ(c.get_notebook()->get_n_pages(), 1);
    EXPECT_EQ(third.get_dialog("ArtworkLibrary"), one);
    EXPECT_EQ(second.get_dialog("ArtworkLibrary"), two);
}
TEST_F(ControllerTest, SixtyFiveNativeTabReopensRetainSavedCollectionWithoutNewSlots) {
    DialogContainer container(desktop->getInkscapeWindow()); DialogNotebook notebook(&container);
    container.new_dialog("ArtworkLibrary", &notebook, false);
    auto library = dynamic_cast<ArtworkLibraryController *>(container.get_dialog("ArtworkLibrary")); ASSERT_TRUE(library);
    auto host = app->artworkLibraries(); auto slot = library->host_slot(); auto retained = host->acquire(slot);
    auto raw = g_dir_make_tmp("vacards-library-reopen-XXXXXX", nullptr); ASSERT_TRUE(raw);
    auto directory = std::filesystem::canonical(std::filesystem::u8path(raw)); g_free(raw);
    auto path = (directory / "synthetic.valib").string(); RecordProperty("evidence_file", path);
    retained->new_collection("Retained through native tab close");
    auto id = retained->active_id(); retained->save(path);
    ASSERT_TRUE(until([&] { return !retained->busy(); })); ASSERT_FALSE(retained->active()->dirty());
    auto slots = host->slots();
    for (unsigned i = 0; i < 65; ++i) {
        notebook.close_tab(container.get_dialog("ArtworkLibrary"));
        ASSERT_FALSE(container.get_dialog("ArtworkLibrary"));
        container.new_dialog("ArtworkLibrary", &notebook, false);
        auto reopened = dynamic_cast<ArtworkLibraryController *>(container.get_dialog("ArtworkLibrary")); ASSERT_TRUE(reopened);
        EXPECT_EQ(reopened->host_slot(), slot); EXPECT_EQ(host->slots(), slots);
        EXPECT_EQ(host->acquire(slot), retained); EXPECT_EQ(retained->active_id(), id);
        EXPECT_EQ(retained->active()->path, path);
    }
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}
// Public floating-state regressions: no private container/host access, no
// synthetic dialog state, and no relaxation of native fatal-critical handling.
struct LibraryEntryFloatingScope {
    struct SavedPref {
        std::string path;
        bool was_set;
        Glib::ustring value;
        explicit SavedPref(char const *p)
            : path(p), was_set(Preferences::get()->getEntry(p).isSet()),
              value(Preferences::get()->getString(p)) {}
        void restore() const {
            if (was_set) Preferences::get()->setString(path, value);
            else Preferences::get()->remove(path);
        }
    };
    struct Window {
        DialogWindow *pointer;
        sigc::slot<bool()> alive;
    };
    SavedPref docking{"/options/dialogtype/value"};
    SavedPref saving{"/options/savedialogposition/value"};
    std::vector<Window> windows;
    sigc::scoped_connection added;

    explicit LibraryEntryFloatingScope(InkscapeApplication &app) {
        auto prefs = Preferences::get();
        prefs->setInt("/options/dialogtype/value", PREFS_DIALOGS_BEHAVIOR_DOCKABLE);
        prefs->setInt("/options/savedialogposition/value", PREFS_DIALOGS_STATE_SAVE);
        auto &manager = DialogManager::singleton();
        manager.remove_dialog_floating_state("ArtworkLibrary");
        manager.remove_dialog_floating_state("Symbols");
        // add_window is called inside DialogWindow's constructor: only record
        // lifetime here; do not inspect its not-yet-constructed container.
        added = app.gtk_app()->signal_window_added().connect([this](Gtk::Window *root) {
            if (auto window = dynamic_cast<DialogWindow *>(root))
                windows.push_back({window, sigc::track_object([] { return true; }, *window)});
        });
    }
    ~LibraryEntryFloatingScope() {
        added.disconnect();
        // Also owns newly reconstructed windows on failed ASSERT paths.
        for (auto i = windows.rbegin(); i != windows.rend(); ++i)
            if (!i->alive.empty()) delete i->pointer;
        auto &manager = DialogManager::singleton();
        manager.remove_dialog_floating_state("ArtworkLibrary");
        manager.remove_dialog_floating_state("Symbols");
        saving.restore();
        docking.restore();
    }
    std::size_t live_count() const {
        return std::count_if(windows.begin(), windows.end(),
                             [](auto const &w) { return !w.alive.empty(); });
    }
    DialogWindow *only_live() const {
        if (live_count() != 1) return nullptr;
        for (auto const &w : windows) if (!w.alive.empty()) return w.pointer;
        return nullptr;
    }
};
bool map_and_store_library_window(DialogWindow &window) {
    window.set_default_size(700, 500);
    window.set_visible(true);
    if (!until([&] {
        return window.get_mapped() && window.get_width() > 1 && window.get_height() > 1;
    })) return false;
    auto position = dm_get_window_position(window);
    if (!position || position->width <= 0 || position->height <= 0) return false;
    DialogManager::singleton().store_state(window);
    return true;
}
TEST_F(ControllerTest, SymbolsRecreatesMixedFloatingGroupWithItsRetainedLibrarySlot) {
    LibraryEntryFloatingScope scope(*app);
    auto &manager = DialogManager::singleton();
    ASSERT_FALSE(manager.find_floating_dialog("Symbols"));
    auto host = app->artworkLibraries(true);
    auto source = desktop->getInkscapeWindow();
    auto before = sp_repr_save_buf(doc->getReprDoc());
    auto first = new DialogWindow(source, nullptr);
    auto second = new DialogWindow(source, nullptr);
    first->get_container()->new_dialog("ArtworkLibrary", nullptr, false);
    second->get_container()->new_dialog("ArtworkLibrary", nullptr, false);
    auto one = dynamic_cast<ArtworkLibraryController *>(first->get_container()->get_dialog("ArtworkLibrary"));
    auto two = dynamic_cast<ArtworkLibraryController *>(second->get_container()->get_dialog("ArtworkLibrary"));
    ASSERT_TRUE(one && two);
    ASSERT_NE(one->host_slot(), two->host_slot());

    // The alternate slot must sort BEFORE the expected slot: dropping the
    // saved identity must deterministically select the wrong retained workspace.
    auto target = one->host_slot() > two->host_slot() ? first : second;
    auto other = target == first ? second : first;
    auto library = target == first ? one : two;
    auto decoy = target == first ? two : one;
    auto slot = library->host_slot();
    auto decoy_slot = decoy->host_slot();
    ASSERT_LT(decoy_slot, slot);
    auto retained = host->acquire(slot);
    retained->new_collection("Mixed floating group retained collection");
    auto raw = g_dir_make_tmp("vacards-library-floating-state-XXXXXX", nullptr);
    ASSERT_TRUE(raw);
    auto directory = std::filesystem::canonical(std::filesystem::u8path(raw));
    g_free(raw);
    auto path = (directory / "synthetic.valib").string();
    RecordProperty("evidence_file", path);
    auto collection = retained->active_id();
    retained->save(path);
    ASSERT_TRUE(until([&] { return !retained->busy(); }));
    ASSERT_TRUE(retained->active());
    ASSERT_FALSE(retained->active()->dirty());
    ASSERT_EQ(retained->active()->path, path);

    auto notebook = descendant<DialogNotebook>(*target->get_container());
    ASSERT_TRUE(notebook);
    target->get_container()->new_dialog("Symbols", notebook, false);
    ASSERT_TRUE(target->get_container()->get_dialog("Symbols"));

    // Close the decoy first so the final stored group is Symbols + Library.
    ASSERT_TRUE(map_and_store_library_window(*other));
    sigc::slot<bool()> other_alive = sigc::track_object([] { return true; }, *other);
    other->close();
    ASSERT_TRUE(until([&] { return other_alive.empty(); }));
    ASSERT_TRUE(map_and_store_library_window(*target));
    ASSERT_TRUE(manager.find_dialog_state("Symbols"));
    sigc::slot<bool()> target_alive = sigc::track_object([] { return true; }, *target);
    target->close();
    ASSERT_TRUE(until([&] { return target_alive.empty(); }));
    ASSERT_EQ(scope.live_count(), 0u);
    ASSERT_FALSE(host->live_panel(slot));
    ASSERT_FALSE(host->live_panel(decoy_slot));
    auto slots = host->slots();

    // Deliberately request the sibling, not ArtworkLibrary. No slot is injected.
    desktop->getContainer()->new_dialog("Symbols");
    auto restored = scope.only_live();
    ASSERT_TRUE(restored);
    ASSERT_TRUE(restored->get_container()->get_dialog("Symbols"));
    auto reopened = dynamic_cast<ArtworkLibraryController *>(
        restored->get_container()->get_dialog("ArtworkLibrary"));
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened->host_slot(), slot);
    EXPECT_EQ(host->live_panel(slot), reopened);
    EXPECT_EQ(host->acquire(reopened->host_slot()), retained);
    EXPECT_FALSE(host->live_panel(decoy_slot));
    EXPECT_EQ(host->slots(), slots);
    EXPECT_EQ(retained->active_id(), collection);
    ASSERT_TRUE(retained->active());
    EXPECT_EQ(retained->active()->path, path);
    EXPECT_FALSE(retained->active()->dirty());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_EQ(history.commits, 0u);
    EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(ControllerTest, FloatingRestoreStopsWhenReservationReleaseClosesItsWindow) {
    LibraryEntryFloatingScope scope(*app);
    auto host = app->artworkLibraries(true);
    auto before = sp_repr_save_buf(doc->getReprDoc());
    auto seed = new DialogWindow(desktop->getInkscapeWindow(), nullptr);
    seed->get_container()->new_dialog("ArtworkLibrary", nullptr, false);
    auto library = dynamic_cast<ArtworkLibraryController *>(seed->get_container()->get_dialog("ArtworkLibrary"));
    ASSERT_TRUE(library);
    auto slot = library->host_slot();
    ASSERT_TRUE(host->acquire(slot)->can_close());
    ASSERT_TRUE(map_and_store_library_window(*seed));
    ASSERT_TRUE(DialogManager::singleton().find_dialog_state("ArtworkLibrary"));
    sigc::slot<bool()> seed_alive = sigc::track_object([] { return true; }, *seed);
    seed->close();
    ASSERT_TRUE(until([&] { return seed_alive.empty(); }));
    ASSERT_FALSE(host->live_panel(slot));
    auto slots = host->slots();
    auto created = scope.windows.size();
    auto native_windows = app->gtk_app()->get_windows().size();
    unsigned closures = 0;
    bool destroyed = false;
    sigc::scoped_connection changed = host->changed.connect([&] {
        if (closures) return;
        auto live = host->live_panel(slot);
        auto window = live ? dynamic_cast<DialogWindow *>(live->get_root()) : nullptr;
        if (!window) return; // reserve notification precedes attach and parenting
        ++closures; // Guard nested notifications from normal close.
        sigc::slot<bool()> alive = sigc::track_object([] { return true; }, *window);
        window->close(); // Real close must be permitted only after release.
        destroyed = alive.empty();
    });

    desktop->getContainer()->new_dialog("ArtworkLibrary");
    changed.disconnect();
    EXPECT_EQ(closures, 1u);
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(scope.windows.size(), created + 1); // No fallback recreation.
    EXPECT_EQ(scope.live_count(), 0u);
    EXPECT_EQ(app->gtk_app()->get_windows().size(), native_windows);
    EXPECT_FALSE(host->live_panel(slot));
    EXPECT_EQ(host->slots(), slots);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_EQ(history.commits, 0u);
    EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(ControllerTest, RefusedFloatingRestoreAtFullReservationCapacityPublishesNoWindow) {
    LibraryEntryFloatingScope scope(*app);
    auto host = app->artworkLibraries(true);
    auto before = sp_repr_save_buf(doc->getReprDoc());
    auto seed = new DialogWindow(desktop->getInkscapeWindow(), nullptr);
    seed->get_container()->new_dialog("ArtworkLibrary", nullptr, false);
    auto library = dynamic_cast<ArtworkLibraryController *>(seed->get_container()->get_dialog("ArtworkLibrary"));
    ASSERT_TRUE(library);
    auto slot = library->host_slot();
    ASSERT_TRUE(host->acquire(slot)->can_close());
    ASSERT_TRUE(map_and_store_library_window(*seed));
    ASSERT_TRUE(DialogManager::singleton().find_dialog_state("ArtworkLibrary"));
    sigc::slot<bool()> seed_alive = sigc::track_object([] { return true; }, *seed);
    seed->close();
    ASSERT_TRUE(until([&] { return seed_alive.empty(); }));
    auto initial_slots = host->slots();
    ASSERT_LE(initial_slots.size(), 64u);
    std::vector<std::unique_ptr<ArtworkLibraryPanelReservation>> reservations;
    // Public reservations occupy all entries, including the saved preferred
    // slot. Do not fake attach(), private quota counters or saved state.
    for (auto const &id : initial_slots) {
        ASSERT_FALSE(host->live_panel(id));
        reservations.push_back(host->reserve_panel(id));
    }
    while (host->slots().size() < 64) reservations.push_back(host->reserve_panel());
    ASSERT_EQ(reservations.size(), 64u);
    auto full_slots = host->slots();
    auto created = scope.windows.size();
    auto native_windows = app->gtk_app()->get_windows().size();

    desktop->getContainer()->new_dialog("ArtworkLibrary");
    EXPECT_EQ(scope.windows.size(), created + 1); // Refused attempt, no fallback.
    EXPECT_EQ(scope.live_count(), 0u); // No blank native window is published.
    EXPECT_EQ(app->gtk_app()->get_windows().size(), native_windows);
    EXPECT_EQ(host->slots(), full_slots);
    for (auto const &id : full_slots) EXPECT_FALSE(host->live_panel(id));
    EXPECT_FALSE(desktop->getContainer()->get_dialog("ArtworkLibrary"));
    reservations.clear();
    EXPECT_EQ(host->slots(), initial_slots); // Pristine abandoned slots reclaimed.
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_EQ(history.commits, 0u);
    EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(ControllerTest, SelectionAndDocumentDetachRecomputeNativeActions) {
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    desktop->getSelection()->set(doc->getObjectById("original")); EXPECT_TRUE(panel->actions()->get_action_enabled("add-selection"));
    panel->setDesktop(nullptr); EXPECT_FALSE(panel->actions()->get_action_enabled("add-selection")); EXPECT_FALSE(panel->actions()->get_action_enabled("insert"));
    EXPECT_EQ(history.commits, 0u);
}
TEST_F(ControllerTest, PlacementSelectsIndependentGroupWithExactlyOneUndo) {
    auto before = sp_repr_save_buf(doc->getReprDoc()); auto selection = desktop->getSelection();
    auto layer = selection->getChangeLayer(), page = selection->getChangePage();
    auto result = place_library_artwork(*desktop, token(), {50, 60}); ASSERT_EQ(result.inserted_ids.size(), 1u);
    EXPECT_EQ(history.commits, 1u); EXPECT_EQ(selection->firstItem(), doc->getObjectById(result.inserted_ids[0]));
    EXPECT_EQ(selection->getChangeLayer(), layer); EXPECT_EQ(selection->getChangePage(), page);
    EXPECT_TRUE(DocumentUndo::undo(doc)); EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before); EXPECT_FALSE(DocumentUndo::undo(doc));
    EXPECT_TRUE(DocumentUndo::redo(doc)); EXPECT_TRUE(doc->getObjectById(result.inserted_ids[0]));
}
TEST_F(ControllerTest, TwoIndependentCopiesOutlivePayloadAndDoNotShareIds) {
    auto asset = token(); auto original = *asset.svg_bytes();
    auto a = place_library_artwork(*desktop, asset, {0, 0}); auto b = place_library_artwork(*desktop, asset, {100, 100});
    ASSERT_NE(a.inserted_ids, b.inserted_ids); EXPECT_EQ(history.commits, 2u);
    auto second = sp_repr_save_buf(doc->getReprDoc()); EXPECT_FALSE(second.empty());
    auto first = doc->getObjectById(a.inserted_ids[0]); ASSERT_TRUE(first); first->getRepr()->setAttribute("data-local-test", "changed");
    EXPECT_FALSE(doc->getObjectById(b.inserted_ids[0])->getRepr()->attribute("data-local-test")); EXPECT_EQ(*asset.svg_bytes(), original);
}
TEST_F(ControllerTest, CallerOwnedOperationStillRefusesPlacementWithoutMutation) {
    auto before = sp_repr_save_buf(doc->getReprDoc());
    auto foreign_operation = DocumentUndo::holdInteractionOperation(doc);
    ASSERT_TRUE(foreign_operation);
    EXPECT_THROW(place_library_artwork(*desktop, token(), {10, 20}), Art::InsertionError);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
    foreign_operation.reset();
    auto inserted = place_library_artwork(*desktop, token(), {10, 20});
    ASSERT_EQ(inserted.inserted_ids.size(), 1u); EXPECT_EQ(history.commits, 1u);
}
TEST_F(ControllerTest, UnsettledXmlIsNeitherCommittedNorDiscardedByPlacement) {
    auto before = sp_repr_save_buf(doc->getReprDoc());
    doc->getObjectById("original")->getRepr()->setAttribute("data-pending-owner", "kept");
    auto pending = sp_repr_save_buf(doc->getReprDoc());
    EXPECT_THROW(place_library_artwork(*desktop, token(), {10, 20}), Art::InsertionError);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), pending); EXPECT_EQ(history.commits, 0u);
    DocumentUndo::done(doc, Util::Internal::ContextString("Caller settles pending edit"), "");
    EXPECT_EQ(history.commits, 1u);
    ASSERT_TRUE(DocumentUndo::undo(doc));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_FALSE(DocumentUndo::undo(doc));
}
TEST_F(ControllerTest, ClosingPanelRetainsUnsavedWorkspaceAndDoesNotTouchHistory) {
    auto before = sp_repr_save_buf(doc->getReprDoc()); panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop); panel.reset();
    ASSERT_TRUE(workspace->active()); EXPECT_TRUE(workspace->active()->dirty()); EXPECT_FALSE(workspace->can_close());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before); EXPECT_EQ(history.commits, 0u);
    panel = std::make_unique<ArtworkLibraryController>(workspace); EXPECT_TRUE(panel->actions()->get_action_enabled("save-as"));
}
TEST_F(ControllerTest, DragPayloadOwnsTokenWithoutClipboardOrDocumentDependency) {
    auto before = sp_repr_save_buf(doc->getReprDoc()); std::optional<ArtworkDrop> drop;
    { auto asset = token(); drop.emplace(ArtworkDrop{asset}); }
    EXPECT_EQ(drop->svg.asset_id(), "b203e8e9-640c-4195-b0ea-f9b790245678"); drop.reset();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before); EXPECT_EQ(history.commits, 0u);
}

TEST_F(ControllerTest, EnabledNotificationMayDestroyPanelDuringRefresh) {
    auto before = sp_repr_save_buf(doc->getReprDoc());
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    auto action = std::dynamic_pointer_cast<Gio::SimpleAction>(panel->actions()->lookup_action("open"));
    ASSERT_TRUE(action); bool destroyed = false;
    sigc::scoped_connection changed = action->property_enabled().signal_changed().connect([&] {
        if (!destroyed && !action->get_enabled()) { destroyed = true; panel.reset(); }
    });
    auto asset = token();
    workspace->add({"", "Original rectangle", {}, 25.4, 25.4},
                   Art::Bytes(asset.svg_bytes()->begin(), asset.svg_bytes()->end()));
    EXPECT_TRUE(destroyed); EXPECT_FALSE(panel);
    ASSERT_TRUE(until([&] { return !workspace->busy(); }));
    ASSERT_EQ(workspace->rows().size(), 1u); EXPECT_TRUE(workspace->active()->dirty());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before); EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(ControllerTest, ReentrantChooserKeepsMutatingActionsDisabled) {
    auto before = sp_repr_save_buf(doc->getReprDoc());
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    auto actions = panel->actions();
    auto open = std::dynamic_pointer_cast<Gio::SimpleAction>(actions->lookup_action("open"));
    ASSERT_TRUE(open); bool saw_busy = false, opened = false;
    sigc::scoped_connection changed = open->property_enabled().signal_changed().connect([&] {
        if (!open->get_enabled()) { saw_busy = true; return; }
        if (saw_busy && !opened) {
            opened = true;
            actions->activate_action("open");
        }
    });
    auto asset = token();
    workspace->add({"", "Original rectangle", {}, 25.4, 25.4},
                   Art::Bytes(asset.svg_bytes()->begin(), asset.svg_bytes()->end()));
    ASSERT_TRUE(until([&] { return !workspace->busy() && opened; }));
    EXPECT_TRUE(saw_busy);
    // The outer idle-enablement pass must not overwrite the nested chooser's
    // disabled state after the open-action notification returns.
    for (auto name : {"new", "open", "recover", "unload", "import", "save", "save-as",
                      "collection", "add-selection", "insert", "export", "remove", "rename", "tags", "restore"}) {
        EXPECT_FALSE(actions->get_action_enabled(name)) << name;
    }
    EXPECT_EQ(workspace->rows().size(), 1u);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
    // Cancels the real native chooser; its eventual completion owns only the
    // invalidated lifetime token, never the disposed controller.
    panel.reset();
    auto end = g_get_monotonic_time() + 200000;
    while (g_get_monotonic_time() < end) { g_main_context_iteration(nullptr, false); g_usleep(1000); }
}
TEST_F(ControllerTest, MetadataEditsRenewSameVisiblePayloadWithoutScrollOrResize) {
    auto before = sp_repr_save_buf(doc->getReprDoc()); auto asset = token();
    workspace->add({"", "Frame", {}, 25.4, 25.4},
                   Art::Bytes(asset.svg_bytes()->begin(), asset.svg_bytes()->end()));
    ASSERT_TRUE(until([&] { return !workspace->busy(); })); ASSERT_EQ(workspace->rows().size(), 1u);
    auto id = workspace->rows()[0].id;
    panel = std::make_unique<ArtworkLibraryController>(workspace);
    Gtk::Window window; window.set_default_size(340, 420); window.set_child(*panel);
    auto unparent = scope_exit([&] { window.unset_child(); }); window.present();
    ASSERT_TRUE(until([&] { return panel->get_mapped(); })); panel->setDesktop(desktop);
    auto grid = descendant<Gtk::GridView>(*panel); ASSERT_TRUE(grid);
    auto selection = std::dynamic_pointer_cast<Gtk::SingleSelection>(grid->get_model()); ASSERT_TRUE(selection);
    selection->set_selected(0);
    ASSERT_TRUE(until([&] { return !workspace->busy() && panel->actions()->get_action_enabled("insert"); }));
    for (bool rename : {false, true}) {
        auto generation = workspace->page_generation();
        if (rename) workspace->rename(id, "Border"); else workspace->tags(id, {"original"});
        ASSERT_TRUE(until([&] { return !workspace->busy() && workspace->page_generation() > generation &&
            !workspace->page().empty() && panel->actions()->get_action_enabled("insert"); })) << workspace->message();
        ASSERT_EQ(workspace->page().size(), 1u); ASSERT_TRUE(workspace->page()[0].svg);
        EXPECT_EQ(workspace->page()[0].metadata.id, id);
        EXPECT_EQ(workspace->page()[0].metadata.name, rename ? "Border" : "Frame");
        // Stable fulfillment does not cause an admission/reload loop.
        auto stable = workspace->page_generation(); auto end = g_get_monotonic_time() + 300000;
        while (g_get_monotonic_time() < end) { g_main_context_iteration(nullptr, false); g_usleep(1000); }
        EXPECT_EQ(workspace->page_generation(), stable); EXPECT_FALSE(workspace->busy());
    }
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before); EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
}
TEST_F(ControllerTest, RebindingLiveDesktopRestoresCapturedSelectionPolicy) {
    auto replacement = SPDocument::createNewDocFromMem(xml()); ASSERT_TRUE(replacement);
    replacement->ensureUpToDate(); auto untouched = sp_repr_save_buf(replacement->getReprDoc());
    auto selection = desktop->getSelection(); selection->setChangeLayer(true); selection->setChangePage(true);
    auto restore_document = scope_exit([&] { desktop->setDocument(doc); });
    bool rebound = false;
    sigc::scoped_connection change = selection->connectChanged([&](Selection *) {
        if (!rebound) { rebound = true; desktop->setDocument(replacement.get()); }
    });
    auto result = place_library_artwork(*desktop, token(), {40, 50});
    ASSERT_TRUE(rebound); EXPECT_EQ(desktop->getSelection(), selection);
    EXPECT_TRUE(selection->getChangeLayer()); EXPECT_TRUE(selection->getChangePage()); EXPECT_TRUE(selection->isEmpty());
    ASSERT_EQ(result.inserted_ids.size(), 1u); EXPECT_TRUE(doc->getObjectById(result.inserted_ids[0]));
    EXPECT_EQ(history.commits, 1u); EXPECT_EQ(sp_repr_save_buf(replacement->getReprDoc()), untouched);
    EXPECT_FALSE(DocumentUndo::undo(replacement.get()));
}
TEST_F(ControllerTest, ExplicitDifferentSelectionPolicyIsNotOverwritten) {
    auto selection = desktop->getSelection(); selection->setChangeLayer(true); selection->setChangePage(true);
    sigc::scoped_connection change = selection->connectChanged([&](Selection *) {
        selection->setChangeLayer(true); selection->setChangePage(false);
    });
    (void)place_library_artwork(*desktop, token(), {0, 0});
    EXPECT_TRUE(selection->getChangeLayer()); EXPECT_FALSE(selection->getChangePage()); EXPECT_EQ(history.commits, 1u);
}
TEST_F(ControllerTest, ExplicitSameValueSelectionPolicyWriteIsPreserved) {
    auto selection = desktop->getSelection(); selection->setChangeLayer(true); selection->setChangePage(true);
    sigc::scoped_connection change = selection->connectChanged([&](Selection *) {
        selection->setChangeLayer(false); selection->setChangePage(false);
    });
    (void)place_library_artwork(*desktop, token(), {0, 0});
    EXPECT_FALSE(selection->getChangeLayer()); EXPECT_FALSE(selection->getChangePage()); EXPECT_EQ(history.commits, 1u);
}
TEST_F(ControllerTest, DestructionOfPrivateDesktopDuringSelectionDoesNotRestoreFreedSelection) {
    auto private_doc = SPDocument::createNewDocFromMem(xml()); ASSERT_TRUE(private_doc); private_doc->ensureUpToDate();
    auto private_desktop = std::make_unique<SPDesktop>(private_doc->getNamedView());
    DocumentUndo::done(private_doc.get(), Util::Internal::ContextString("Fixture"), "");
    DocumentUndo::clearUndo(private_doc.get()); DocumentUndo::clearRedo(private_doc.get());
    History own_history; private_doc->addUndoObserver(own_history);
    auto remove_observer = scope_exit([&] { private_doc->removeUndoObserver(own_history); });
    auto selection = private_desktop->getSelection(); selection->setChangeLayer(true); selection->setChangePage(true);
    bool destroyed = false;
    sigc::scoped_connection change = selection->connectChanged([&](Selection *) {
        if (!destroyed) { destroyed = true; private_desktop.reset(); }
    });
    auto result = place_library_artwork(*private_desktop, token(), {0, 0});
    EXPECT_TRUE(destroyed); EXPECT_FALSE(private_desktop); EXPECT_EQ(own_history.commits, 1u);
    ASSERT_EQ(result.inserted_ids.size(), 1u); EXPECT_TRUE(private_doc->getObjectById(result.inserted_ids[0]));
}

namespace {
TEST_F(ControllerTest, DockMoveRetainsSlotAndDirtyTabCloseCanCancel) {
    panel = std::make_unique<ArtworkLibraryController>(workspace);
    DialogContainer container(desktop->getInkscapeWindow());
    DialogNotebook first(&container), second(&container);
    first.add_page(*panel); auto host = app->artworkLibraries(); ASSERT_TRUE(host);
    auto ids = host->within(first); ASSERT_EQ(ids.size(), 1u);
    second.move_page(*panel);
    auto detach = scope_exit([&] { if (panel && second.get_notebook()->page_num(*panel) >= 0) second.get_notebook()->detach_tab(*panel); });
    EXPECT_TRUE(host->within(first).empty()); EXPECT_EQ(host->within(second), ids);
    EXPECT_EQ(host->acquire(ids[0]), workspace);
    bool answered = false; auto response = answer_library_close(GTK_RESPONSE_CANCEL, answered);
    second.close_tab(panel.get()); response.disconnect();
    EXPECT_TRUE(answered); EXPECT_EQ(second.get_page(0), panel.get()); EXPECT_TRUE(workspace->active()->dirty());
    EXPECT_EQ(history.commits, 0u);
}
TEST_F(ControllerTest, NativeLastWindowCloseHonorsRetainedOrphanCancel) {
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel.reset();
    bool answered = false; auto response = answer_library_close(GTK_RESPONSE_CANCEL, answered);
    auto closed = app->destroyDesktop(desktop); response.disconnect();
    EXPECT_TRUE(answered); EXPECT_FALSE(closed); EXPECT_EQ(desktop->getDocument(), doc);
    EXPECT_TRUE(workspace->active()->dirty()); EXPECT_FALSE(workspace->closing()); EXPECT_EQ(history.commits, 0u);
}
using CallbackObject = std::unique_ptr<GObject, decltype(&g_object_unref)>;
std::vector<CallbackObject> callback_controllers(Gtk::Widget &widget, GType type) {
    auto model = gtk_widget_observe_controllers(widget.gobj());
    auto release = scope_exit([&] { g_object_unref(model); });
    std::vector<CallbackObject> out;
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
        CallbackObject object(G_OBJECT(g_list_model_get_item(model, i)), g_object_unref);
        if (g_type_is_a(G_OBJECT_TYPE(object.get()), type)) out.push_back(std::move(object));
    }
    return out; // retain signal emitters, NOT the controller/panel owner
}
class ControllerCallbackTest : public ControllerTest {
protected:
    void SetUp() override {
        ControllerTest::SetUp();
        old_size = Preferences::get()->getInt("/dialogs/artwork-library/thumbnail-size", 80);
        before = sp_repr_save_buf(doc->getReprDoc());
    }
    void TearDown() override {
        kill_signal.disconnect();
        if (window) window->unset_child();
        panel.reset(); window.reset();
        if (workspace->busy()) { workspace->cancel(); EXPECT_TRUE(until([&] { return !workspace->busy(); })); }
        Preferences::get()->setInt("/dialogs/artwork-library/thumbnail-size", old_size);
        ControllerTest::TearDown();
    }
    Gtk::CenterBox *row_box() {
        if (!panel) return nullptr;
        auto grid = descendant<Gtk::GridView>(*panel); if (!grid) return nullptr;
        for (auto child = grid->get_first_child(); child; child = child->get_next_sibling())
            if (auto box = dynamic_cast<Gtk::CenterBox *>(child->get_first_child());
                box && box->has_css_class("item-box")) return box;
        return nullptr;
    }
    bool row_click_ready() {
        if (!panel) return false;
        auto grid = descendant<Gtk::GridView>(*panel);
        // Menu-button popovers also contain scrollers. The viewport must own
        // this grid, not merely be the first scroller anywhere in the panel.
        Gtk::ScrolledWindow *viewport = nullptr;
        for (auto parent = grid ? grid->get_parent() : nullptr; parent; parent = parent->get_parent()) {
            if ((viewport = dynamic_cast<Gtk::ScrolledWindow *>(parent))) break;
        }
        auto box = row_box(); auto cell = box ? box->get_parent() : nullptr;
        if (!grid || !viewport || !cell || cell->get_parent() != grid ||
            !grid->get_mapped() || !cell->get_mapped() || !cell->get_visible() || !cell->get_child_visible() ||
            grid->get_width() <= 0 || grid->get_height() <= 0 ||
            viewport->get_width() <= 0 || viewport->get_height() <= 0 ||
            cell->get_width() <= 0 || cell->get_height() <= 0) return false;
        int x = 0, y = 0, width = 0, height = 0;
        if (!cell->get_bounds(x, y, width, height) || width <= 0 || height <= 0) return false;
        // Keep the original integer-half cell center. CSS outlines may extend
        // outside allocation; the actual click must be inside the visible area.
        double cx = double(x) + width / 2, cy = double(y) + height / 2;
        if (cx < 0 || cy < 0 || cx >= grid->get_width() || cy >= grid->get_height()) return false;
        graphene_point_t point{float(cx), float(cy)}, in_viewport{};
        if (!gtk_widget_compute_point(GTK_WIDGET(grid->gobj()), GTK_WIDGET(viewport->gobj()),
                                      &point, &in_viewport)) return false;
        return in_viewport.x >= 0 && in_viewport.y >= 0 &&
               in_viewport.x < viewport->get_width() && in_viewport.y < viewport->get_height();
    }
    void map_one() {
        auto asset = token(); auto data = asset.svg_bytes();
        workspace->add({"", "Callback rectangle", {}, 25.4, 25.4}, Art::Bytes(data->begin(), data->end()));
        ASSERT_TRUE(until([&] { return !workspace->busy(); }));
        ASSERT_EQ(workspace->rows().size(), 1u);
        panel = std::make_unique<ArtworkLibraryController>(workspace);
        window = std::make_unique<Gtk::Window>(); window->set_default_size(340, 420);
        window->set_child(*panel); window->present();
        ASSERT_TRUE(until([&] { return panel->get_mapped(); })); panel->setDesktop(desktop);
        // Normal visible-page admission only; no insertion or document/history settlement.
        ASSERT_TRUE(until([&] { return !workspace->busy() && !workspace->page().empty() && row_click_ready(); }));
        mapped = true;
    }
    void destroy_panel() {
        if (destroyed) return;
        destroyed = true;
        if (window) window->unset_child();
        panel.reset();
    }
    void arm_row_notification() {
        auto box = row_box(); ASSERT_TRUE(box);
        // Ensure the next bind_picture writes a DIFFERENT tooltip synchronously.
        box->set_tooltip_text("Original synthetic notification sentinel");
        kill_signal = box->property_tooltip_text().signal_changed().connect([this] { destroy_panel(); });
    }
    void check_unchanged() {
        EXPECT_TRUE(destroyed); EXPECT_FALSE(panel);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
        EXPECT_EQ(history.commits, 0u); EXPECT_FALSE(doc->isModifiedSinceSave());
        EXPECT_TRUE(workspace->active()); EXPECT_EQ(workspace->rows().size(), 1u);
    }
    std::unique_ptr<Gtk::Window> window;
    sigc::scoped_connection kill_signal;
    std::string before;
    int old_size = 80;
    bool mapped = false, destroyed = false;
};
class ControllerColdOpenTest : public ControllerCallbackTest {
    // Production opening does not settle a fake edit or clear history before
    // the first drop. Keep the load-time log intact so admission really tests it.
    bool settle_fixture_history() const override { return false; }
};
TEST_F(ControllerColdOpenTest, DragCallbacksInsertBeforeAnyUserZoomOrHistorySettlement) {
    map_one(); ASSERT_TRUE(mapped);
    auto grid = descendant<Gtk::GridView>(*panel); ASSERT_TRUE(grid);
    auto selection = std::dynamic_pointer_cast<Gtk::SingleSelection>(grid->get_model()); ASSERT_TRUE(selection);
    selection->set_selected(GTK_INVALID_LIST_POSITION);
    auto box = row_box(); ASSERT_TRUE(box);
    auto picture = dynamic_cast<Gtk::Picture *>(box->get_start_widget()); ASSERT_TRUE(picture);
    graphene_point_t center{float(picture->get_width()) / 2, float(picture->get_height()) / 2}, in_grid{};
    ASSERT_TRUE(gtk_widget_compute_point(GTK_WIDGET(picture->gobj()), GTK_WIDGET(grid->gobj()), &center, &in_grid));
    auto sources = callback_controllers(*grid, GTK_TYPE_DRAG_SOURCE);
    ASSERT_EQ(sources.size(), 1u);
    auto zoom = desktop->current_zoom();
    auto before_drop = sp_repr_save_buf(doc->getReprDoc());
    // Exercise the installed prepare/drop callbacks, not a hand-built ArtworkDrop.
    // This does not synthesize native mouse events or qualify the macOS DnD session.
    GdkContentProvider *provider = nullptr;
    g_signal_emit_by_name(sources.front().get(), "prepare", double(in_grid.x), double(in_grid.y), &provider);
    ASSERT_TRUE(provider);
    auto release = scope_exit([&] { g_object_unref(provider); });
    Glib::ValueBase value; value.init(Util::GlibValue::type<ArtworkDrop>());
    ASSERT_TRUE(gdk_content_provider_get_value(provider, value.gobj(), nullptr));
    auto artwork = Util::GlibValue::get<ArtworkDrop>(value); ASSERT_TRUE(artwork);
    ASSERT_EQ(artwork->svg.asset_id(), workspace->rows().front().id);
    EXPECT_EQ(history.commits, 0u);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before_drop);

    auto canvas = desktop->getCanvas(); ASSERT_TRUE(canvas);
    auto stack = canvas->get_parent(); ASSERT_TRUE(stack);
    auto targets = callback_controllers(*stack, GTK_TYPE_DROP_TARGET);
    ASSERT_EQ(targets.size(), 1u);
    gboolean accepted = false;
    g_signal_emit_by_name(targets.front().get(), "drop", value.gobj(),
                         double(canvas->get_width()) / 2, double(canvas->get_height()) / 2, &accepted);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(history.commits, 1u);
    EXPECT_DOUBLE_EQ(desktop->current_zoom(), zoom);
    EXPECT_TRUE(desktop->getSelection()->singleItem());
    ASSERT_TRUE(DocumentUndo::undo(doc));
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before_drop);
    EXPECT_FALSE(DocumentUndo::undo(doc));
    EXPECT_TRUE(DocumentUndo::redo(doc));
}
TEST_F(ControllerCallbackTest, RightClickSelectionNotificationStopsBeforePopover) {
    map_one(); ASSERT_TRUE(mapped);
    auto grid = descendant<Gtk::GridView>(*panel); ASSERT_TRUE(grid);
    auto selection = std::dynamic_pointer_cast<Gtk::SingleSelection>(grid->get_model()); ASSERT_TRUE(selection);
    selection->set_selected(GTK_INVALID_LIST_POSITION);
    auto action = std::dynamic_pointer_cast<Gio::SimpleAction>(panel->actions()->lookup_action("rename"));
    ASSERT_TRUE(action); ASSERT_FALSE(action->get_enabled());
    auto emitters = callback_controllers(*grid, GTK_TYPE_GESTURE_CLICK);
    GObject *right = nullptr;
    for (auto const &e : emitters)
        if (gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(e.get())) == 3) right = e.get();
    ASSERT_TRUE(right);
    auto box = row_box(); ASSERT_TRUE(box); auto cell = box->get_parent(); ASSERT_TRUE(cell);
    ASSERT_TRUE(row_click_ready());
    int x = 0, y = 0, width = 0, height = 0;
    ASSERT_TRUE(cell->get_bounds(x, y, width, height));
    ASSERT_GT(width, 0); ASSERT_GT(height, 0);
    kill_signal = action->property_enabled().signal_changed().connect([&] {
        if (action->get_enabled()) destroy_panel();
    });
    g_signal_emit_by_name(right, "pressed", 1, double(x + width / 2), double(y + height / 2));
    check_unchanged();
}
TEST_F(ControllerCallbackTest, EscapeCancelNotificationStopsBeforeInvalidation) {
    map_one(); ASSERT_TRUE(mapped);
    auto generation = workspace->page_generation();
    auto emitters = callback_controllers(*panel, GTK_TYPE_EVENT_CONTROLLER_KEY); ASSERT_FALSE(emitters.empty());
    unsigned cancelled = 0;
    kill_signal = workspace->changed.connect([&] { ++cancelled; destroy_panel(); });
    for (auto const &e : emitters) {
        gboolean handled = false;
        g_signal_emit_by_name(e.get(), "key-pressed", GDK_KEY_Escape, 0u, GdkModifierType(0), &handled);
        if (destroyed) { EXPECT_TRUE(handled); break; }
    }
    EXPECT_EQ(cancelled, 1u); EXPECT_EQ(workspace->page_generation(), generation);
    check_unchanged();
}
TEST_F(ControllerCallbackTest, SearchInvalidationStopsAfterRowNotificationDestroysPanel) {
    map_one(); ASSERT_TRUE(mapped);
    auto search = descendant<Gtk::SearchEntry2>(*panel); ASSERT_TRUE(search);
    CallbackObject emitter(G_OBJECT(g_object_ref(search->gobj())), g_object_unref);
    arm_row_notification();
    g_signal_emit_by_name(emitter.get(), "search-changed");
    EXPECT_FALSE(workspace->busy()); // no follow-up search worker was admitted
    check_unchanged();
}
TEST_F(ControllerCallbackTest, ScaleInvalidationStopsAfterRowNotificationDestroysPanel) {
    map_one(); ASSERT_TRUE(mapped);
    auto scale = descendant<Gtk::Scale>(*panel); ASSERT_TRUE(scale);
    CallbackObject emitter(G_OBJECT(g_object_ref(scale->gobj())), g_object_unref);
    auto value = scale->get_value(); arm_row_notification();
    gtk_range_set_value(GTK_RANGE(emitter.get()), value < 120 ? value + 8 : value - 8);
    check_unchanged();
}
TEST_F(ControllerCallbackTest, ScalePreferenceObserverStopsBeforeInvalidation) {
    map_one(); ASSERT_TRUE(mapped);
    auto scale = descendant<Gtk::Scale>(*panel); ASSERT_TRUE(scale);
    CallbackObject emitter(G_OBJECT(g_object_ref(scale->gobj())), g_object_unref);
    auto value = scale->get_value(); auto generation = workspace->page_generation();
    struct PreferenceHook final : Preferences::Observer {
        explicit PreferenceHook(std::function<void()> hit)
            : Observer("/dialogs/artwork-library/thumbnail-size"), hit(std::move(hit)) {}
        void notify(Preferences::Entry const &) override { hit(); }
        std::function<void()> hit;
    } hook([&] { destroy_panel(); });
    Preferences::get()->addObserver(hook);
    gtk_range_set_value(GTK_RANGE(emitter.get()), value < 120 ? value + 8 : value - 8);
    EXPECT_EQ(workspace->page_generation(), generation);
    check_unchanged();
}
TEST_F(ControllerCallbackTest, CollectionInvalidationStopsBeforeSwitchingWorkspace) {
    auto first = workspace->active_id(); workspace->new_collection("Other synthetic collection");
    map_one(); ASSERT_TRUE(mapped); auto current = workspace->active_id(); ASSERT_NE(current, first);
    auto actions = panel->actions(); arm_row_notification();
    actions->activate_action("collection", Glib::Variant<Glib::ustring>::create(first));
    EXPECT_EQ(workspace->active_id(), current); check_unchanged();
}
TEST_F(ControllerCallbackTest, FontInvalidationStopsBeforeSchedulingDestroyedPanel) {
    map_one(); ASSERT_TRUE(mapped); arm_row_notification();
    panel->fonts_changed();
    check_unchanged();
}
TEST_F(ControllerCallbackTest, VisibleUpdatesStopAfterRowNotificationDestroysPanel) {
    map_one(); ASSERT_TRUE(mapped); arm_row_notification();
    // Existing polling/thumbnail callbacks drive this integration case; it does
    // not distinguish the visible loop from a thumbnail completion binding.
    ASSERT_TRUE(until([&] { return destroyed; }));
    check_unchanged();
}
TEST_F(ControllerCallbackTest, RightClickSkipsOverlappingNonRowBeforeDestroyNotification) {
    map_one(); ASSERT_TRUE(mapped);
    auto grid = descendant<Gtk::GridView>(*panel); ASSERT_TRUE(grid);
    auto selection = std::dynamic_pointer_cast<Gtk::SingleSelection>(grid->get_model()); ASSERT_TRUE(selection);
    selection->set_selected(GTK_INVALID_LIST_POSITION);
    auto action = std::dynamic_pointer_cast<Gio::SimpleAction>(panel->actions()->lookup_action("rename"));
    ASSERT_TRUE(action); ASSERT_FALSE(action->get_enabled());
    auto emitters = callback_controllers(*grid, GTK_TYPE_GESTURE_CLICK);
    GObject *right = nullptr;
    for (auto const &e : emitters)
        if (gtk_gesture_single_get_button(GTK_GESTURE_SINGLE(e.get())) == 3) right = e.get();
    ASSERT_TRUE(right);
    auto box = row_box(); ASSERT_TRUE(box); auto cell = box->get_parent(); ASSERT_TRUE(cell);
    ASSERT_TRUE(row_click_ready());
    int x = 0, y = 0, width = 0, height = 0;
    ASSERT_TRUE(cell->get_bounds(x, y, width, height));
    ASSERT_GT(width, 0); ASSERT_GT(height, 0);
    double click_x = x + width / 2, click_y = y + height / 2;

    // A visible, non-targetable decoration intersects the exact existing click.
    // Pin its GObject independently; never retain the controller/grid owner.
    CallbackObject decoration(G_OBJECT(g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0))), g_object_unref);
    auto decoration_widget = GTK_WIDGET(decoration.get());
    auto detach = [&] {
        if (gtk_widget_get_parent(decoration_widget)) gtk_widget_unparent(decoration_widget);
    };
    auto cleanup = scope_exit([&] { detach(); });
    gtk_widget_set_can_target(decoration_widget, false);
    gtk_widget_set_visible(decoration_widget, true);
    gtk_widget_set_parent(decoration_widget, GTK_WIDGET(grid->gobj()));
    GtkAllocation allocation{x, y, width, height};
    gtk_widget_size_allocate(decoration_widget, &allocation, -1);
    // No event-loop pump between this synthetic allocation and the real signal.
    auto last = grid->get_last_child(); ASSERT_TRUE(last);
    ASSERT_EQ(last->gobj(), decoration_widget);
    ASSERT_TRUE(last->get_visible()); ASSERT_TRUE(last->get_child_visible());
    ASSERT_FALSE(last->get_first_child()); // cannot be a factory-bound artwork row
    int bx = 0, by = 0, bw = 0, bh = 0;
    ASSERT_TRUE(last->get_bounds(bx, by, bw, bh));
    ASSERT_GE(click_x, bx); ASSERT_LT(click_x, bx + bw);
    ASSERT_GE(click_y, by); ASSERT_LT(click_y, by + bh);

    kill_signal = action->property_enabled().signal_changed().connect([&] {
        if (action->get_enabled()) {
            detach(); // remove the foreign test child before native grid disposal
            destroy_panel();
        }
    });
    g_signal_emit_by_name(right, "pressed", 1, click_x, click_y);
    check_unchanged(); // actual selection notification must still destroy the panel
}
} // callback-only fixtures

namespace {
// Drives the real native modal dialogs. An unexpected visible message is a
// failure and is cancelled, rather than leaving the test in an endless modal.
struct LocalCloseResponses {
    unsigned library = 0, document = 0;
    std::function<void()> at_library, at_document;
    int document_response = GTK_RESPONSE_NO;
    sigc::scoped_connection poll;
    LocalCloseResponses() {
        poll = Glib::signal_idle().connect([this] {
            auto windows = gtk_window_get_toplevels();
            for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
                auto item = g_list_model_get_item(windows, i);
                auto release = scope_exit([&] { g_object_unref(item); });
                if (!GTK_IS_MESSAGE_DIALOG(item) || !gtk_widget_get_visible(GTK_WIDGET(item))) continue;
                auto title = gtk_window_get_title(GTK_WINDOW(item));
                int answer = GTK_RESPONSE_CANCEL;
                try {
                    if (g_strcmp0(title, "Artwork Library") == 0) {
                        ++library; if (at_library) at_library(); answer = GTK_RESPONSE_REJECT;
                    } else if (g_strcmp0(title, _("Save Document")) == 0) {
                        ++document; if (at_document) at_document(); answer = document_response;
                    } else {
                        ADD_FAILURE() << "Unexpected native close dialog: " << (title ? title : "(no title)");
                    }
                } catch (std::exception const &e) {
                    ADD_FAILURE() << e.what(); answer = GTK_RESPONSE_CANCEL;
                }
                gtk_dialog_response(GTK_DIALOG(item), answer);
                break; // callbacks may have changed the native window list
            }
            return true;
        });
    }
};
class ControllerCloseScopeTest : public ControllerTest {
protected:
    void SetUp() override {
        ControllerTest::SetUp(); if (HasFatalFailure()) return;
        source_document_alive = source_desktop_alive = true;
        source_document_destroy = doc->connectDestroy([this] { source_document_alive = false; });
        source_desktop_destroy = desktop->connectDestroy([this](SPDesktop *) { source_desktop_alive = false; });
    }
    void unparent_panel() {
        if (panel) if (auto parent = dynamic_cast<Gtk::Box *>(panel->get_parent())) parent->remove(*panel);
    }
    void close_auxiliary() {
        if (auxiliary_desktop_alive) app->desktopClose(auxiliary_desktop);
        if (auxiliary_document_alive) app->document_close(auxiliary_document);
        auxiliary_desktop_destroy.disconnect(); auxiliary_document_destroy.disconnect();
        auxiliary_desktop = nullptr; auxiliary_document = nullptr;
        auxiliary_desktop_alive = auxiliary_document_alive = false;
    }
    void open_auxiliary(bool separate_window) {
        ASSERT_FALSE(auxiliary_document_alive);
        auxiliary_document = app->document_add(SPDocument::createNewDocFromMem(xml()));
        ASSERT_TRUE(auxiliary_document); auxiliary_document_alive = true;
        auxiliary_document_destroy = auxiliary_document->connectDestroy([this] { auxiliary_document_alive = false; });
        auxiliary_document->ensureUpToDate(); auxiliary_document->setModifiedSinceSave(false);
        auxiliary_desktop = app->createDesktop(auxiliary_document, false, separate_window);
        ASSERT_TRUE(auxiliary_desktop); auxiliary_desktop_alive = true;
        auxiliary_desktop_destroy = auxiliary_desktop->connectDestroy([this](SPDesktop *) { auxiliary_desktop_alive = false; });
    }
    void TearDown() override {
        // A regression may already have destroyed the source desktop. Cleanup
        // must not turn the primary assertion into a stale-pointer dereference.
        unparent_panel(); panel.reset();
        if (app) if (auto host = app->artworkLibraries()) {
            DiscardForTest ui; auto guard = host->prepare(host->slots(), ui);
            EXPECT_TRUE(guard); if (guard) { guard->commit(); guard.reset(); }
        }
        close_auxiliary();
        if (source_document_alive) { doc->removeUndoObserver(history); doc->setModifiedSinceSave(false); }
        if (source_desktop_alive) app->desktopClose(desktop);
        if (source_document_alive) app->document_close(doc);
        for (unsigned i = 0; i < 64 && g_main_context_pending(nullptr); ++i) g_main_context_iteration(nullptr, false);
    }
    SPDocument *auxiliary_document = nullptr;
    SPDesktop *auxiliary_desktop = nullptr;
    bool source_document_alive = false, source_desktop_alive = false;
    bool auxiliary_document_alive = false, auxiliary_desktop_alive = false;
    sigc::scoped_connection source_document_destroy, source_desktop_destroy;
    sigc::scoped_connection auxiliary_document_destroy, auxiliary_desktop_destroy;
};
TEST_F(ControllerCloseScopeTest, ModalTopologyChangesDoNotDiscardSurvivingLibrary) {
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    auto source_window = desktop->getInkscapeWindow();
    auto source_container = source_window->get_desktop_widget()->getDialogContainer();
    ASSERT_TRUE(source_container);
    // Mount the controller directly in the real native dock root so this case
    // isolates host scope/reparenting from the separately tested public factory.
    source_container->append(*panel);
    auto host = app->artworkLibraries(); ASSERT_TRUE(host);
    auto ids = host->within(*source_window); ASSERT_EQ(ids.size(), 1u);
    auto collection = workspace->active_id(); auto before = sp_repr_save_buf(doc->getReprDoc());
    enum class Change { TabDuringLibrary, TabDuringDocument, MoveDuringDocument };
    for (auto change : {Change::TabDuringLibrary, Change::TabDuringDocument, Change::MoveDuringDocument}) {
        SCOPED_TRACE(int(change));
        if (change == Change::MoveDuringDocument) open_auxiliary(true);
        app->set_active_window(source_window); app->set_active_desktop(desktop);
        doc->setModifiedSinceSave(true); // trigger the native document decision, without XML/history edits
        auto operation = DocumentUndo::holdInteractionOperation(doc); ASSERT_TRUE(operation);
        bool changed = false;
        LocalCloseResponses response;
        auto alter_topology = [&] {
            if (changed) return;
            changed = true;
            if (change == Change::MoveDuringDocument) {
                ASSERT_TRUE(auxiliary_desktop_alive);
                auto destination = auxiliary_desktop->getInkscapeWindow()->get_desktop_widget()->getDialogContainer();
                ASSERT_TRUE(destination);
                source_container->remove(*panel); destination->append(*panel);
                EXPECT_TRUE(host->within(*source_window).empty());
                EXPECT_EQ(host->within(*auxiliary_desktop->getInkscapeWindow()), ids);
            } else {
                app->set_active_window(source_window); open_auxiliary(false);
                ASSERT_TRUE(auxiliary_desktop_alive);
                EXPECT_EQ(auxiliary_desktop->getInkscapeWindow(), source_window);
                EXPECT_EQ(source_window->get_desktop_widget()->get_desktops().size(), 2u);
            }
        };
        if (change == Change::TabDuringLibrary) response.at_library = alter_topology;
        else response.at_document = alter_topology;
        bool closed = app->destroyDesktop(desktop); response.poll.disconnect();
        EXPECT_FALSE(closed); EXPECT_TRUE(changed); EXPECT_EQ(response.library, 1u);
        EXPECT_EQ(response.document, change == Change::TabDuringLibrary ? 0u : 1u);
        ASSERT_TRUE(source_desktop_alive); ASSERT_TRUE(source_document_alive);
        ASSERT_TRUE(workspace->active()); EXPECT_EQ(workspace->active_id(), collection);
        EXPECT_TRUE(workspace->active()->dirty()); EXPECT_FALSE(workspace->closing());
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before); EXPECT_EQ(history.commits, 0u);
        EXPECT_TRUE(doc->isModifiedSinceSave()); EXPECT_FALSE(app->quitPending());
        EXPECT_FALSE(app->documentClosePending(doc));
        operation.reset();
        unparent_panel(); source_container->append(*panel); close_auxiliary();
    }
}
TEST_F(ControllerCloseScopeTest, LibraryDiscardThenDocumentCancelCannotQuitAfterLeaseUnwinds) {
    panel = std::make_unique<ArtworkLibraryController>(workspace); panel->setDesktop(desktop);
    auto collection = workspace->active_id(); auto before = sp_repr_save_buf(doc->getReprDoc());
    doc->setModifiedSinceSave(true);
    auto operation = DocumentUndo::holdInteractionOperation(doc); ASSERT_TRUE(operation);
    LocalCloseResponses response; response.document_response = GTK_RESPONSE_CANCEL;
    app->on_quit(); response.poll.disconnect();
    EXPECT_EQ(response.library, 1u); EXPECT_EQ(response.document, 1u);
    ASSERT_TRUE(source_desktop_alive); ASSERT_TRUE(source_document_alive);
    EXPECT_FALSE(app->quitPending()); EXPECT_FALSE(app->documentClosePending(doc));
    EXPECT_FALSE(DocumentUndo::interactionCloseRequested(doc));
    ASSERT_TRUE(workspace->active()); EXPECT_EQ(workspace->active_id(), collection);
    EXPECT_TRUE(workspace->active()->dirty()); EXPECT_FALSE(workspace->closing());
    // Real owner-quiescence continuation: no test-only XML/history settlement.
    auto unwound = std::make_shared<bool>(false);
    ASSERT_TRUE(DocumentUndo::deferUntilInteractionQuiescent(doc, [unwound](SPDocument &) { *unwound = true; }));
    operation.reset();
    ASSERT_TRUE(until([&] { return *unwound; }));
    ASSERT_TRUE(source_desktop_alive); ASSERT_TRUE(source_document_alive);
    EXPECT_FALSE(app->quitPending()); EXPECT_FALSE(app->documentClosePending(doc));
    EXPECT_FALSE(app->documentHoldActive()); EXPECT_EQ(app->documentOperationCount(), 0u);
    ASSERT_TRUE(workspace->active()); EXPECT_EQ(workspace->active_id(), collection);
    EXPECT_TRUE(workspace->active()->dirty()); EXPECT_FALSE(workspace->closing());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before); EXPECT_EQ(history.commits, 0u);
    EXPECT_TRUE(doc->isModifiedSinceSave());
}
} // host modal scope regressions
}

// Real application + controllers + mapped GTK viewports. Small first host
// packet: full 1k/10k viewport/widget retention and aggregate quota saturation
// remain required, not replaced by these one-row native rendering tests.
namespace {
class ControllerPoolTest : public ControllerTest {
protected:
    void SetUp() override {
        ControllerTest::SetUp();
        old_size = Preferences::get()->getInt("/dialogs/artwork-library/thumbnail-size", 80);
        Preferences::get()->setInt("/dialogs/artwork-library/thumbnail-size", 80);
        pool = app->artworkLibraryThumbnailPool();
        ASSERT_TRUE(pool);
        app->invalidateArtworkLibraryThumbnails();
        before = sp_repr_save_buf(doc->getReprDoc());
        second_workspace = ArtworkLibraryWorkspace::create();
        second_workspace->new_collection("Second synthetic collection");
    }
    void TearDown() override {
        death.disconnect();
        close_first();
        if (second_window) second_window->unset_child();
        second.reset();
        remove_window(second_window);
        if (second_workspace && second_workspace->busy()) {
            second_workspace->cancel();
            EXPECT_TRUE(until([&] { return !second_workspace->busy(); }));
        }
        Preferences::get()->setInt("/dialogs/artwork-library/thumbnail-size", old_size);
        ControllerTest::TearDown(); // existing library close/discard + document lease harness
    }
    void remove_window(std::unique_ptr<Gtk::Window> &window) {
        if (!window) return;
        // close() can already remove the registered native window.
        if (gtk_window_get_application(window->gobj()) == app->gtk_app()->gobj())
            app->gtk_app()->remove_window(*window);
        window.reset();
    }
    void close_first() {
        if (first_closed) return;
        first_closed = true; // native unparent/close notifications can reenter
        if (first_window) first_window->unset_child();
        panel.reset(); // service Close only; workspace retained by existing host
        if (first_window) first_window->close();
        remove_window(first_window);
    }
    static Gtk::CenterBox *box(ArtworkLibraryController *controller) {
        if (!controller) return nullptr;
        auto grid = descendant<Gtk::GridView>(*controller);
        if (!grid) return nullptr;
        for (auto child = grid->get_first_child(); child; child = child->get_next_sibling())
            if (auto b = dynamic_cast<Gtk::CenterBox *>(child->get_first_child());
                b && b->has_css_class("item-box")) return b;
        return nullptr;
    }
    static Glib::RefPtr<Gdk::Texture> image(ArtworkLibraryController *controller) {
        auto b = box(controller);
        if (!b) return {};
        auto picture = dynamic_cast<Gtk::Picture *>(b->get_start_widget());
        return picture ? std::dynamic_pointer_cast<Gdk::Texture>(picture->get_paintable()) : Glib::RefPtr<Gdk::Texture>{};
    }
    void settle() {
        auto end = g_get_monotonic_time() + 350000;
        while (g_get_monotonic_time() < end) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
    }
    void map_pair() {
        auto svg = token();
        auto bytes = svg.svg_bytes();
        for (auto w : {workspace, second_workspace}) {
            w->add({"", "Shared native rectangle", {}, 25.4, 25.4}, Art::Bytes(bytes->begin(), bytes->end()));
            ASSERT_TRUE(until([&] { return !w->busy(); })) << w->message();
            ASSERT_EQ(w->rows().size(), 1u);
        }
        // Exercise both retained-slot and explicit injection construction.
        auto host = app->artworkLibraries(true);
        auto slot = host->retain(workspace);
        panel = std::make_unique<ArtworkLibraryController>(slot);
        second = std::make_unique<ArtworkLibraryController>(second_workspace, pool);
        panel->setDesktop(desktop);
        second->setDesktop(desktop);
        first_window = std::make_unique<Gtk::Window>();
        second_window = std::make_unique<Gtk::Window>();
        first_window->set_title("Library pool first");
        second_window->set_title("Library pool second");
        first_window->set_default_size(340, 420);
        second_window->set_default_size(340, 420);
        first_window->set_child(*panel);
        second_window->set_child(*second);
        app->gtk_app()->add_window(*first_window);
        app->gtk_app()->add_window(*second_window);
        first_window->present();
        second_window->present();
        ASSERT_TRUE(until([&] {
            return !workspace->busy() && !second_workspace->busy() &&
                   image(panel.get()) && image(second.get());
        })) << workspace->message() << " / " << second_workspace->message();
        settle();
        ASSERT_TRUE(image(panel.get()));
        ASSERT_TRUE(image(second.get()));
        ASSERT_EQ(panel->get_scale_factor(), second->get_scale_factor());
        paired = true;
    }
    void unchanged_document() {
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
        EXPECT_EQ(history.commits, 0u);
        EXPECT_FALSE(doc->isModifiedSinceSave());
    }
    std::shared_ptr<UI::Cache::ArtworkLibraryThumbnailPool> pool;
    std::shared_ptr<ArtworkLibraryWorkspace> second_workspace;
    std::unique_ptr<ArtworkLibraryController> second;
    std::unique_ptr<Gtk::Window> first_window, second_window;
    sigc::scoped_connection death;
    std::string before;
    int old_size = 80;
    bool paired = false, first_closed = false;
};

TEST_F(ControllerPoolTest, SameApplicationPoolAndNativeBackingAcrossTwoControllers) {
    map_pair(); ASSERT_TRUE(paired);
    EXPECT_NE(workspace, second_workspace);
    EXPECT_EQ(panel->thumbnail_pool(), pool);
    EXPECT_EQ(second->thumbnail_pool(), pool);
    EXPECT_EQ(app->artworkLibraryThumbnailPool(), pool);
    auto first = image(panel.get()), other = image(second.get());
    ASSERT_TRUE(first); ASSERT_TRUE(other);
    EXPECT_EQ(first->gobj(), other->gobj()); // actual native cache dedup, not a mock renderer
    auto stats = pool->stats();
    EXPECT_EQ(stats.capacity, UI::Cache::ArtworkLibraryThumbnailPool::hard_limit);
    EXPECT_EQ(stats.capacity, 128u * 1024 * 1024);
    EXPECT_EQ(stats.charged, stats.reserved + stats.resident);
    EXPECT_LE(stats.charged, stats.capacity);
    EXPECT_GE(stats.resident, stats.pixel_bytes);
    EXPECT_EQ(stats.cache_entries, 1u);
    EXPECT_GT(stats.backing_allocations, 0u);
    EXPECT_GT(first->get_width(), 0);
    EXPECT_GT(first->get_height(), 0);
    unchanged_document();
}

TEST_F(ControllerPoolTest, ClosingOneControllerDoesNotInvalidateOtherOrSharedCache) {
    map_pair(); ASSERT_TRUE(paired);
    auto survivor = image(second.get());
    auto generation = second_workspace->page_generation();
    auto stats = pool->stats();
    close_first();
    settle();
    ASSERT_FALSE(panel);
    ASSERT_TRUE(second);
    EXPECT_TRUE(workspace->active()); // existing retained-slot close contract remains intact
    EXPECT_EQ(app->artworkLibraryThumbnailPool(), pool);
    EXPECT_EQ(second_workspace->page_generation(), generation);
    ASSERT_TRUE(image(second.get()));
    EXPECT_EQ(image(second.get())->gobj(), survivor->gobj());
    EXPECT_EQ(pool->stats().cache_entries, stats.cache_entries);
    EXPECT_EQ(pool->stats().backing_allocations, stats.backing_allocations);
    EXPECT_FALSE(second_workspace->busy());
    unchanged_document();
}

TEST_F(ControllerPoolTest, NativeFontconfigNotificationRefreshesBothVisiblePages) {
    map_pair(); ASSERT_TRUE(paired);
    auto first = image(panel.get()), other = image(second.get());
    auto generation = workspace->page_generation(), other_generation = second_workspace->page_generation();
    unsigned broadcasts = 0;
    sigc::scoped_connection broadcast = app->connectArtworkLibraryThumbnailEnvironment([&] { ++broadcasts; });
    auto settings = Gtk::Settings::get_default(); ASSERT_TRUE(settings);
    // Public native notification drives FontLister::refreshConfig/NewFonts,
    // then the application's installed listener. No private signal or mock pool.
    g_object_notify(G_OBJECT(settings->gobj()), "gtk-fontconfig-timestamp");
    EXPECT_EQ(broadcasts, 1u);
    ASSERT_TRUE(until([&] {
        auto a = image(panel.get()), b = image(second.get());
        return !workspace->busy() && !second_workspace->busy() &&
               workspace->page_generation() > generation &&
               second_workspace->page_generation() > other_generation &&
               a && b && a->gobj() != first->gobj() && b->gobj() != other->gobj();
    }));
    EXPECT_EQ(image(panel.get())->gobj(), image(second.get())->gobj());
    // Old GTK consumers are deliberately kept alive above; invalidation must
    // not release their charge as if LRU removal destroyed their backing.
    auto stats = pool->stats();
    EXPECT_GE(stats.backings, 2u);
    EXPECT_EQ(stats.charged, stats.reserved + stats.resident);
    EXPECT_LE(stats.charged, stats.capacity);
    unchanged_document();
}

TEST_F(ControllerPoolTest, FontFanoutCanDestroyFirstPanelAndStillRefreshSecond) {
    map_pair(); ASSERT_TRUE(paired);
    auto other = image(second.get());
    auto generation = second_workspace->page_generation();
    auto row = box(panel.get()); ASSERT_TRUE(row);
    row->set_tooltip_text("One-shot native invalidation sentinel");
    death = row->property_tooltip_text().signal_changed().connect([&] { close_first(); });
    app->invalidateArtworkLibraryThumbnails();
    EXPECT_FALSE(panel);
    ASSERT_TRUE(second);
    ASSERT_TRUE(until([&] {
        auto next = image(second.get());
        return !second_workspace->busy() && second_workspace->page_generation() > generation &&
               next && next->gobj() != other->gobj();
    }));
    unchanged_document();
}

TEST_F(ControllerPoolTest, NullInjectionCannotConstructAnIndependentService) {
    EXPECT_THROW((std::make_unique<ArtworkLibraryController>(workspace,
        std::shared_ptr<UI::Cache::ArtworkLibraryThumbnailPool>{})), std::invalid_argument);
    EXPECT_EQ(app->artworkLibraryThumbnailPool(), pool);
    unchanged_document();
}
} // native application-pool fixtures
