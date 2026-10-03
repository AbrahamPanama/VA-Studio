// SPDX-License-Identifier: GPL-2.0-or-later
// Original synthetic files only. No GUI, private sample, installed application,
// build, or execution is performed by the artifact author.
#include "ui/dialog/artwork-library-host.h"
#include "preferences.h"
#include "io/resource.h"
#include <gtest/gtest.h>
#include <glib.h>
#include <libxml/parser.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string_view>
using namespace Inkscape::UI::Dialog;
namespace {
bool settle(std::shared_ptr<ArtworkLibraryWorkspace> const &w) {
    auto end = g_get_monotonic_time() + 10000000;
    while (w->busy() && g_get_monotonic_time() < end) {
        g_main_context_iteration(nullptr, false); g_usleep(1000);
    }
    return !w->busy();
}
std::string disk_preference(std::string const &key) {
    // Read independently of the Preferences singleton, which would otherwise
    // hide a missing disk flush when a test recreates only the library host.
    auto filename = Inkscape::IO::Resource::profile_path("preferences.xml");
    gchar *raw = nullptr; gsize size = 0; GError *error = nullptr;
    if (!g_file_get_contents(filename.c_str(), &raw, &size, &error)) {
        std::string message = error ? error->message : "Cannot read test preferences";
        g_clear_error(&error); throw std::runtime_error(message);
    }
    std::unique_ptr<gchar, decltype(&g_free)> bytes(raw, g_free);
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
        xmlReadMemory(raw, static_cast<int>(size), nullptr, "UTF-8", XML_PARSE_NONET), xmlFreeDoc);
    if (!doc) throw std::runtime_error("Invalid test preferences XML");
    auto property = [](xmlNode *node, char const *name) {
        auto value = xmlGetProp(node, BAD_CAST name);
        std::string text = value ? reinterpret_cast<char const *>(value) : "";
        xmlFree(value); return text;
    };
    auto node = xmlDocGetRootElement(doc.get());
    std::string_view remaining(key);
    if (remaining.starts_with('/')) remaining.remove_prefix(1);
    for (auto slash = remaining.find('/'); slash != remaining.npos; slash = remaining.find('/')) {
        auto id = remaining.substr(0, slash);
        auto child = node ? node->children : nullptr;
        for (; child; child = child->next)
            if (child->type == XML_ELEMENT_NODE && property(child, "id") == id) break;
        if (!child) return {};
        node = child; remaining.remove_prefix(slash + 1);
    }
    return node ? property(node, std::string(remaining).c_str()) : "";
}
struct Decisions : LibraryCloseUI {
    std::deque<LibraryCloseAnswer> answers;
    std::vector<LibraryCloseKind> prompts;
    std::vector<std::string> errors;
    std::function<void()> during_prompt;
    bool abandon_wait = false;
    LibraryCloseAnswer ask(LibraryClosePrompt const &p) override {
        prompts.push_back(p.kind); if (during_prompt) during_prompt();
        if (answers.empty()) { ADD_FAILURE() << "Unexpected close prompt"; return {}; }
        auto a = answers.front(); answers.pop_front(); return a;
    }
    bool wait(std::shared_ptr<ArtworkLibraryWorkspace> const &w, std::function<void()> const &) override {
        return !abandon_wait && settle(w);
    }
    void error(std::string const &s) override { errors.push_back(s); }
};
class LibraryHostTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto raw = g_dir_make_tmp("vacards-library-host-XXXXXX", nullptr); ASSERT_TRUE(raw);
        // Resolve only our own fixture directory, matching storage/workspace
        // tests; production no-symlink storage policy remains unchanged.
        auto physical = std::filesystem::canonical(std::filesystem::u8path(raw)).u8string();
        directory.assign(physical.begin(), physical.end()); g_free(raw);
        RecordProperty("evidence_directory", directory);
        host = ArtworkLibraryHost::create(); w = host->acquire("stable-dock-slot"); w->new_collection("Synthetic");
    }
    void TearDown() override { if (w->busy()) { w->cancel(); EXPECT_TRUE(settle(w)); } }
    std::string path(std::string const &s) { return Art::canonical_library_path(directory + "/" + s); }
    std::string persistent_host() {
        auto key = "/test/artwork-library-session/" + std::filesystem::path(directory).filename().string();
        host = ArtworkLibraryHost::create(key); w = host->acquire("stable-dock-slot");
        return key;
    }
    void save_named(std::string name, std::string file) {
        w->new_collection(std::move(name)); w->save(path(file));
        ASSERT_TRUE(settle(w)); ASSERT_FALSE(w->active()->dirty()) << w->message();
    }
    std::string directory;
    std::shared_ptr<ArtworkLibraryHost> host;
    std::shared_ptr<ArtworkLibraryWorkspace> w;
};
TEST_F(LibraryHostTest, SavedSessionSurvivesProcessRestart) {
    // CTest runs this suite with a fresh profile, then invokes this case again
    // with VERIFY=1. Neither invocation explicitly saves preferences or uses
    // the application's clean-shutdown path.
    std::string key = "/test/artwork-library-process-session";
    if (!g_getenv("VACARDS_LIBRARY_SESSION_VERIFY")) {
        Inkscape::Preferences::get()->setInt(key + "/count", 0);
        host = ArtworkLibraryHost::create(key); w = host->acquire("stable-dock-slot");
        save_named("Tarjetas test", "Tarjetas test.valib");
        auto first = w->active_id();
        save_named("Lámparas test", "Lámparas & designs.valib");
        auto source = ArtworkLibraryWorkspace::create();
        source->new_collection("Opened collection"); source->save(path("opened.valib"));
        ASSERT_TRUE(settle(source)); ASSERT_FALSE(source->active()->dirty()) << source->message();
        w->open(path("opened.valib")); ASSERT_TRUE(settle(w));
        w->select_collection(first);
        ASSERT_TRUE(Inkscape::Preferences::get()->isWritable());
        EXPECT_EQ(disk_preference(key + "/count"), "1");
        EXPECT_EQ(disk_preference(key + "/0/count"), "3");
        EXPECT_EQ(disk_preference(key + "/0/file2"), path("opened.valib"));
        EXPECT_EQ(disk_preference(key + "/0/active"), path("Tarjetas test.valib"));
    }
    w.reset(); host.reset();
    host = ArtworkLibraryHost::create(key);
    auto panel = host->reserve_panel("stable-dock-slot");
    EXPECT_EQ(panel->slot(), "stable-dock-slot");
    w = panel->workspace(); ASSERT_TRUE(settle(w));
    ASSERT_EQ(w->collections().size(), 3u) << w->message();
    EXPECT_EQ(w->collections()[0].label, "Tarjetas test");
    EXPECT_EQ(w->collections()[1].label, "Lámparas test");
    EXPECT_EQ(w->collections()[2].label, "Opened collection");
    ASSERT_TRUE(w->active()); EXPECT_EQ(w->active()->label, "Tarjetas test");
    EXPECT_TRUE(w->can_close()); EXPECT_FALSE(w->pending_import());
    RecordProperty("restored_first_file", w->collections()[0].path);
}
TEST_F(LibraryHostTest, NativeOpenPersistsWithoutAnotherLibrarySaveAndPreservesOtherPreferences) {
    // Manufacture a native file using a transient host, then only Open it in
    // the persistent host. Open of an unchanged library must also be durable.
    auto source = path("open-only.valib");
    w->save(source); ASSERT_TRUE(settle(w)); ASSERT_FALSE(w->active()->dirty()) << w->message();
    auto key = persistent_host();
    auto other_key = key + "-unrelated/value";
    Inkscape::Preferences::get()->setString(other_key, "Keep & preserve <preferences>");
    w->open(source); ASSERT_TRUE(settle(w));
    ASSERT_TRUE(w->active()); EXPECT_FALSE(w->active()->dirty());
    EXPECT_EQ(disk_preference(key + "/0/file0"), source);
    EXPECT_EQ(disk_preference(key + "/0/active"), source);
    EXPECT_EQ(disk_preference(other_key), "Keep & preserve <preferences>");
}
TEST_F(LibraryHostTest, SaveAsReplacesRememberedPathAndUnloadForgetsIt) {
    auto key = persistent_host();
    save_named("Keep", "keep.valib");
    save_named("Rename", "before.valib");
    w->save(path("after.valib")); ASSERT_TRUE(settle(w));
    ASSERT_EQ(w->saved_session().paths, (std::vector<std::string>{path("keep.valib"), path("after.valib")}));
    EXPECT_EQ(disk_preference(key + "/0/file1"), path("after.valib"));
    w->unload();
    EXPECT_EQ(disk_preference(key + "/0/count"), "1");
    EXPECT_EQ(disk_preference(key + "/0/file0"), path("keep.valib"));
    EXPECT_TRUE(std::filesystem::exists(path("before.valib")));
    EXPECT_TRUE(std::filesystem::exists(path("after.valib")));
    w.reset(); host.reset(); host = ArtworkLibraryHost::create(key);
    w = host->acquire("stable-dock-slot"); ASSERT_TRUE(settle(w));
    ASSERT_EQ(w->collections().size(), 1u); EXPECT_EQ(w->active()->label, "Keep");
}
TEST_F(LibraryHostTest, UnsavedCollectionsAndFailedSavesAreNotRemembered) {
    auto key = persistent_host();
    w->new_collection("Unsaved import");
    EXPECT_TRUE(w->saved_session().paths.empty());
    // Destination already exists: no overwrite and no remembered bogus path.
    { std::ofstream out(path("occupied.valib")); out << "unchanged"; }
    w->save(path("occupied.valib")); ASSERT_TRUE(settle(w));
    EXPECT_TRUE(w->active()->dirty()); EXPECT_TRUE(w->saved_session().paths.empty());
    auto other = ArtworkLibraryHost::create(key);
    EXPECT_TRUE(other->slots().empty());
    EXPECT_TRUE(disk_preference(key + "/count").empty());
}
TEST_F(LibraryHostTest, WorkspaceNotificationsDoNotFlushPreferencesOrForgetSavedFiles) {
    auto key = persistent_host(); save_named("Saved", "saved.valib");
    auto marker = key + "-notification-marker/value";
    Inkscape::Preferences::get()->setString(marker, "Not a session change");
    w->search("No matching artwork");
    // Search completes asynchronously; wait for its completion notification
    // before creating another collection through the idle-only editing API.
    ASSERT_TRUE(settle(w));
    EXPECT_TRUE(disk_preference(marker).empty());
    w->new_collection("Unsaved");
    EXPECT_TRUE(disk_preference(marker).empty());
    EXPECT_EQ(disk_preference(key + "/0/file0"), path("saved.valib"));
    Decisions ui; ui.answers = {{LibraryCloseChoice::Cancel}};
    EXPECT_FALSE(host->prepare(host->slots(), ui));
    EXPECT_TRUE(disk_preference(marker).empty());
    EXPECT_EQ(disk_preference(key + "/0/file0"), path("saved.valib"));
}
TEST_F(LibraryHostTest, CloseWithoutSavingRetainsLastSavedLibraryForRestart) {
    auto key = persistent_host(); save_named("Saved original", "original.valib");
    auto text = "<svg xmlns='http://www.w3.org/2000/svg' width='10mm' height='10mm' viewBox='0 0 10 10'><rect width='3' height='3'/></svg>";
    std::string svg(text);
    w->add({"", "Unsaved item", {}, 10, 10}, {svg.begin(), svg.end()});
    ASSERT_TRUE(settle(w)); ASSERT_TRUE(w->active()->dirty()) << w->message();
    w->new_collection("Never saved");
    Decisions ui; ui.answers = {{LibraryCloseChoice::Discard}, {LibraryCloseChoice::Discard}};
    auto close = host->prepare(host->slots(), ui); ASSERT_TRUE(close);
    EXPECT_EQ(disk_preference(key + "/0/file0"), path("original.valib"));
    close->commit(); close.reset(); EXPECT_TRUE(w->collections().empty());
    EXPECT_EQ(disk_preference(key + "/count"), "1");
    EXPECT_EQ(disk_preference(key + "/0/file0"), path("original.valib"));
    // A later session notification must still use remembered durable paths,
    // not serialize the live collection list emptied by close/discard.
    save_named("Later collection", "later.valib");
    EXPECT_EQ(disk_preference(key + "/0/count"), "2");
    EXPECT_EQ(disk_preference(key + "/0/file1"), path("original.valib"));
    w.reset(); host.reset(); host = ArtworkLibraryHost::create(key);
    w = host->acquire("stable-dock-slot"); ASSERT_TRUE(settle(w));
    ASSERT_EQ(w->collections().size(), 2u);
    w->select_collection(w->collections()[1].identity);
    EXPECT_EQ(w->active()->label, "Saved original"); EXPECT_TRUE(w->rows().empty());
    EXPECT_FALSE(w->active()->dirty());
}
TEST_F(LibraryHostTest, UnavailableFileDoesNotBlockOtherLibrariesOrLoseItsRememberedPath) {
    auto key = persistent_host(); save_named("Temporarily offline", "offline.valib");
    save_named("Available", "available.valib");
    std::filesystem::rename(path("offline.valib"), path("disconnected.valib"));
    w.reset(); host.reset(); host = ArtworkLibraryHost::create(key);
    w = host->acquire("stable-dock-slot"); ASSERT_TRUE(settle(w));
    ASSERT_EQ(w->collections().size(), 1u); EXPECT_EQ(w->active()->label, "Available");
    EXPECT_NE(w->message().find("offline.valib"), std::string::npos);
    EXPECT_EQ(w->saved_session().paths.size(), 2u);
    std::filesystem::rename(path("disconnected.valib"), path("offline.valib"));
    w.reset(); host.reset(); host = ArtworkLibraryHost::create(key);
    w = host->acquire("stable-dock-slot"); ASSERT_TRUE(settle(w));
    EXPECT_EQ(w->collections().size(), 2u);
}
TEST_F(LibraryHostTest, SavedPathValidationPreservesCanonicalActiveSelection) {
    auto key = persistent_host(); save_named("First", "first.valib");
    save_named("Second", "second.valib"); auto second = w->active_id();
    auto prefs = Inkscape::Preferences::get();
    prefs->setString(key + "/0/file0", directory + "/./first.valib");
    prefs->setString(key + "/0/file1", directory + "/./second.valib");
    prefs->setString(key + "/0/file2", "relative.valib");
    prefs->setInt(key + "/0/count", 3);
    prefs->setString(key + "/0/active", directory + "/./second.valib");
    w.reset(); host.reset(); host = ArtworkLibraryHost::create(key);
    w = host->acquire("stable-dock-slot"); ASSERT_TRUE(settle(w));
    EXPECT_EQ(w->active_id(), second);
    EXPECT_EQ(w->saved_session().paths, (std::vector<std::string>{path("first.valib"), path("second.valib")}));
    EXPECT_EQ(w->saved_session().active_path, path("second.valib"));
}
#ifdef _WIN32
TEST_F(LibraryHostTest, SavedUtf16PathCanExceed32768Utf8Bytes) {
    auto key = persistent_host();
    // Absent descendants under our local fixture directory: no network access
    // or giant file creation. Each component remains below the filename limit.
    std::string component;
    for (unsigned i = 0; i < 64; ++i) component += "圖";
    std::string long_path = directory;
    for (unsigned i = 0; i < 220; ++i) long_path += "/" + component;
    long_path = Art::canonical_library_path(long_path + "/collection.valib");
    ASSERT_GT(long_path.size(), 32768u);
    auto prefs = Inkscape::Preferences::get();
    prefs->setInt(key + "/count", 1); prefs->setString(key + "/0/slot", "stable-dock-slot");
    prefs->setInt(key + "/0/count", 2); prefs->setString(key + "/0/file0", long_path);
    prefs->setString(key + "/0/file1", directory + "/" + std::string(33000, 'a')); // invalid UTF-16 length
    auto extended = long_path.starts_with("\\\\") ? "\\\\?\\UNC\\" + long_path.substr(2) : "\\\\?\\" + long_path;
    prefs->setString(key + "/0/active", extended);
    w.reset(); host.reset(); host = ArtworkLibraryHost::create(key);
    w = host->acquire("stable-dock-slot"); ASSERT_TRUE(settle(w));
    EXPECT_FALSE(w->active()); // unavailable does not mean invalid or forgotten
    EXPECT_EQ(w->saved_session().paths, (std::vector<std::string>{long_path}));
    EXPECT_EQ(w->saved_session().active_path, long_path);
}
#endif
TEST_F(LibraryHostTest, DifferentPanelsKeepSeparateListsAndRestoreOnlyWhenOpened) {
    auto key = persistent_host(); save_named("First panel", "first.valib");
    auto first = w; w = host->acquire("second-slot");
    save_named("Second panel", "second.valib");
    first.reset(); w.reset(); host.reset(); host = ArtworkLibraryHost::create(key);
    EXPECT_FALSE(host->needs_hold()); // no background reads for unopened panels
    w = host->acquire("stable-dock-slot"); ASSERT_TRUE(settle(w));
    save_named("Added after restart", "third.valib"); // must retain second slot
    // std::map orders second-slot before stable-dock-slot. It is still unopened
    // here, so its pending reopen list must survive another slot's disk flush.
    EXPECT_EQ(disk_preference(key + "/count"), "2");
    EXPECT_EQ(disk_preference(key + "/0/slot"), "second-slot");
    EXPECT_EQ(disk_preference(key + "/0/file0"), path("second.valib"));
    w.reset(); host.reset(); host = ArtworkLibraryHost::create(key);
    w = host->acquire("second-slot"); ASSERT_TRUE(settle(w));
    ASSERT_EQ(w->collections().size(), 1u); EXPECT_EQ(w->active()->label, "Second panel");
}
TEST_F(LibraryHostTest, SavingInReverseOrderPreservesSidebarOrder) {
    auto key = persistent_host();
    w->new_collection("First"); auto first = w->active_id();
    save_named("Second", "second.valib");
    w->select_collection(first); w->save(path("first.valib")); ASSERT_TRUE(settle(w));
    ASSERT_FALSE(w->active()->dirty()) << w->message();
    w.reset(); host.reset(); host = ArtworkLibraryHost::create(key);
    w = host->acquire("stable-dock-slot"); ASSERT_TRUE(settle(w));
    ASSERT_EQ(w->collections().size(), 2u);
    EXPECT_EQ(w->collections()[0].label, "First"); EXPECT_EQ(w->collections()[1].label, "Second");
}
TEST_F(LibraryHostTest, SlotSurvivesAllPanelReferencesAndDetachment) {
    auto id = w->active_id(); std::weak_ptr<ArtworkLibraryWorkspace> weak = w;
    host->attach("stable-dock-slot", []() -> Gtk::Widget * { return nullptr; }, [] { return true; });
    host->detach("stable-dock-slot"); w.reset(); ASSERT_FALSE(weak.expired());
    w = host->acquire("stable-dock-slot"); EXPECT_EQ(w->active_id(), id); EXPECT_TRUE(w->active()->dirty());
    EXPECT_EQ(host->retain(w), "stable-dock-slot"); EXPECT_EQ(host->slots().size(), 1u); EXPECT_TRUE(host->needs_hold());
}
// Core host tests use an opaque non-null identity, never a dereferenced widget.
Gtk::Widget *live_panel_identity() {
    static int identity;
    return reinterpret_cast<Gtk::Widget *>(&identity);
}
TEST_F(LibraryHostTest, ReservationKeepsPreferredCollectionsAndExcludesCompetingOpens) {
    auto active = w->active_id();
    auto first = host->reserve_panel("stable-dock-slot");
    ASSERT_EQ(first->workspace(), w); EXPECT_EQ(first->workspace()->active_id(), active);
    EXPECT_THROW(host->reserve_panel(first->slot()), std::runtime_error);
    EXPECT_THROW(host->attach(first->slot(), live_panel_identity, [] { return true; }), std::runtime_error);
    auto second = host->reserve_panel();
    EXPECT_NE(first->slot(), second->slot()); EXPECT_NE(first->workspace(), second->workspace());
    EXPECT_THROW(host->attach(first->slot(), live_panel_identity, [] { return true; }, second.get()), std::runtime_error);
    host->attach(first->slot(), live_panel_identity, [] { return true; }, first.get());
    first.reset();
    EXPECT_THROW(host->reserve_panel("stable-dock-slot"), std::runtime_error);
    host->detach("stable-dock-slot");
    first = host->reserve_panel("stable-dock-slot");
    EXPECT_EQ(first->workspace()->active_id(), active);
}
TEST_F(LibraryHostTest, AbandonedNewReservationRollsBackOnlyPristineEntry) {
    host->attach("stable-dock-slot", live_panel_identity, [] { return true; });
    auto baseline = host->slots().size();
    { auto reservation = host->reserve_panel(); EXPECT_EQ(host->slots().size(), baseline + 1); }
    EXPECT_EQ(host->slots().size(), baseline);
    std::string retained;
    {
        auto reservation = host->reserve_panel(); retained = reservation->slot();
        reservation->workspace()->new_collection("Do not lose work during failed construction");
    }
    ASSERT_EQ(host->slots().size(), baseline + 1);
    auto restored = host->reserve_panel(retained);
    ASSERT_TRUE(restored->workspace()->active());
    EXPECT_TRUE(restored->workspace()->active()->dirty());
}
TEST_F(LibraryHostTest, ReentrantOpenCannotStealReservationAndThrownObserverUnwindsIt) {
    bool entered = false;
    sigc::scoped_connection changed = host->changed.connect([&] {
        if (entered) return;
        entered = true;
        EXPECT_THROW(host->reserve_panel("stable-dock-slot"), std::runtime_error);
        throw std::runtime_error("Injected construction observer failure");
    });
    EXPECT_THROW(host->reserve_panel("stable-dock-slot"), std::runtime_error);
    EXPECT_TRUE(entered); changed.disconnect();
    auto retry = host->reserve_panel("stable-dock-slot");
    EXPECT_EQ(retry->workspace(), w); EXPECT_TRUE(w->active()->dirty());
}
TEST_F(LibraryHostTest, SequentialReopensDoNotExhaustRetainedSlotCapacity) {
    auto active = w->active_id();
    for (int i = 0; i < 128; ++i) {
        auto reservation = host->reserve_panel("stable-dock-slot");
        EXPECT_EQ(reservation->workspace()->active_id(), active);
        host->attach(reservation->slot(), live_panel_identity, [] { return true; }, reservation.get());
        reservation.reset(); host->detach("stable-dock-slot");
    }
    EXPECT_EQ(host->slots().size(), 1u);
}
TEST_F(LibraryHostTest, StaleAttachmentReleaseCannotDetachSuccessor) {
    auto first = host->attach("stable-dock-slot", live_panel_identity, [] { return true; });
    host->detach("stable-dock-slot", first);
    auto second = host->attach("stable-dock-slot", live_panel_identity, [] { return true; });
    ASSERT_NE(first, second);
    host->detach("stable-dock-slot", first);
    EXPECT_THROW(host->reserve_panel("stable-dock-slot"), std::runtime_error);
    host->detach("stable-dock-slot", second);
    EXPECT_NO_THROW(host->reserve_panel("stable-dock-slot"));
}
TEST_F(LibraryHostTest, CapacityCountsBothReservationsAndAttachedPanels) {
    std::vector<std::unique_ptr<ArtworkLibraryPanelReservation>> claims;
    for (unsigned i = 0; i < 64; ++i) claims.push_back(host->reserve_panel());
    EXPECT_EQ(host->slots().size(), 64u);
    EXPECT_THROW(host->reserve_panel(), std::runtime_error);
    for (auto &claim : claims) host->attach(claim->slot(), live_panel_identity, [] { return true; }, claim.get());
    claims.clear();
    EXPECT_THROW(host->reserve_panel(), std::runtime_error);
    EXPECT_EQ(host->slots().size(), 64u);
}
TEST_F(LibraryHostTest, CloseRefusesConstructionAndConstructionRefusesClose) {
    auto reservation = host->reserve_panel("stable-dock-slot");
    Decisions ui;
    EXPECT_FALSE(host->prepare(host->slots(), ui));
    EXPECT_FALSE(ui.errors.empty()); EXPECT_TRUE(ui.prompts.empty()); EXPECT_FALSE(w->closing());
    reservation.reset();
    ui.answers = {{LibraryCloseChoice::Discard}};
    auto close = host->prepare(host->slots(), ui); ASSERT_TRUE(close);
    EXPECT_THROW(host->reserve_panel("stable-dock-slot"), std::runtime_error);
    close.reset(); EXPECT_NO_THROW(host->reserve_panel("stable-dock-slot"));
}
TEST_F(LibraryHostTest, OpeningReservationBlocksEmptyAndUnrelatedNativeCloseScopes) {
    (void)host->acquire("other-window");
    auto reservation = host->reserve_panel("stable-dock-slot");
    Decisions ui;
    EXPECT_FALSE(host->prepare({}, ui));
    EXPECT_FALSE(host->prepare({"other-window"}, ui));
    EXPECT_EQ(ui.errors.size(), 2u); EXPECT_TRUE(ui.prompts.empty());
    EXPECT_FALSE(w->closing()); EXPECT_TRUE(w->active()->dirty());
}
TEST_F(LibraryHostTest, ReadinessCallbackCannotReserveBeforeCloseLocking) {
    bool attempted = false;
    host->attach("stable-dock-slot", live_panel_identity, [&] {
        attempted = true;
        EXPECT_THROW(host->reserve_panel(), std::runtime_error);
        return true;
    });
    Decisions ui; ui.answers = {{LibraryCloseChoice::Discard}};
    auto close = host->prepare(host->slots(), ui);
    ASSERT_TRUE(close); EXPECT_TRUE(attempted); EXPECT_EQ(host->slots().size(), 1u);
    close.reset(); EXPECT_TRUE(w->active()->dirty());
}
TEST_F(LibraryHostTest, CancelLeavesCollectionsAndNoReservation) {
    Decisions ui; ui.answers = {{LibraryCloseChoice::Cancel}};
    EXPECT_FALSE(host->prepare(host->slots(), ui)); EXPECT_FALSE(w->closing()); EXPECT_TRUE(w->active()->dirty());
}
TEST_F(LibraryHostTest, DiscardIsProvisionalUntilNativeCloseCommits) {
    Decisions ui; ui.answers = {{LibraryCloseChoice::Discard}};
    auto guard = host->prepare(host->slots(), ui); ASSERT_TRUE(guard);
    EXPECT_TRUE(w->active()->dirty()); EXPECT_TRUE(w->closing());
    guard.reset(); // later DOCUMENT cancel / vanished close target
    ASSERT_TRUE(w->active()); EXPECT_TRUE(w->active()->dirty()); EXPECT_FALSE(w->closing());
}
TEST_F(LibraryHostTest, CommitDiscardsOnlyRequestedEditsAndNeverFiles) {
    auto destination = path("saved.valib"); w->save(destination); ASSERT_TRUE(settle(w)); ASSERT_FALSE(w->active()->dirty());
    w->new_collection("Unsaved");
    Decisions ui; ui.answers = {{LibraryCloseChoice::Discard}};
    auto guard = host->prepare(host->slots(), ui); ASSERT_TRUE(guard); guard->commit(); guard.reset();
    ASSERT_EQ(w->collections().size(), 1u); EXPECT_FALSE(w->active()->dirty());
    EXPECT_TRUE(std::filesystem::exists(destination)); EXPECT_FALSE(host->needs_hold());
}
TEST_F(LibraryHostTest, LaterCollectionCancelRestoresEarlierProvisionalDiscard) {
    w->new_collection("Second");
    Decisions ui; ui.answers = {{LibraryCloseChoice::Discard}, {LibraryCloseChoice::Cancel}};
    EXPECT_FALSE(host->prepare(host->slots(), ui)); EXPECT_EQ(w->collections().size(), 2u);
    for (auto const &c : w->collections()) EXPECT_TRUE(c.dirty());
}
TEST_F(LibraryHostTest, PublishedSaveSurvivesLaterCollectionCancel) {
    auto key = persistent_host(); w->new_collection("First");
    w->new_collection("Second"); auto destination = path("first.valib");
    Decisions ui; ui.answers = {{LibraryCloseChoice::Save, destination}, {LibraryCloseChoice::Cancel}};
    EXPECT_FALSE(host->prepare(host->slots(), ui)); ASSERT_EQ(w->collections().size(), 2u);
    EXPECT_FALSE(w->collections()[0].dirty()); EXPECT_TRUE(w->collections()[1].dirty());
    EXPECT_TRUE(std::filesystem::exists(destination)); EXPECT_FALSE(w->closing());
    EXPECT_EQ(disk_preference(key + "/count"), "1");
    EXPECT_EQ(disk_preference(key + "/0/count"), "1");
    EXPECT_EQ(disk_preference(key + "/0/file0"), destination);
}
TEST_F(LibraryHostTest, SaveFailureNeverAuthorizesCloseOrBlindRetry) {
    Decisions ui; ui.answers = {{LibraryCloseChoice::Save, path("missing/collection.valib")}};
    EXPECT_FALSE(host->prepare(host->slots(), ui)); EXPECT_TRUE(w->active()->dirty());
    EXPECT_EQ(ui.prompts.size(), 1u); EXPECT_FALSE(ui.errors.empty()); EXPECT_FALSE(w->closing());
}
TEST_F(LibraryHostTest, WaitObservesSaveCompletionBeforeClose) {
    w->save(path("wait.valib")); ASSERT_TRUE(w->busy());
    Decisions ui; ui.answers = {{LibraryCloseChoice::Wait}};
    auto guard = host->prepare(host->slots(), ui); ASSERT_TRUE(guard) << w->message();
    EXPECT_FALSE(w->busy()); EXPECT_FALSE(w->active()->dirty()); guard->commit(); guard.reset();
    EXPECT_FALSE(host->needs_hold()); ASSERT_EQ(ui.prompts.size(), 1u);
    EXPECT_EQ(ui.prompts[0], LibraryCloseKind::Work);
}
TEST_F(LibraryHostTest, AbandonWaitRetainsWorkerResultAndApplicationHold) {
    w->save(path("late.valib"));
    Decisions ui; ui.answers = {{LibraryCloseChoice::Wait}}; ui.abandon_wait = true;
    EXPECT_FALSE(host->prepare(host->slots(), ui)); EXPECT_TRUE(w->busy()); EXPECT_FALSE(w->closing()); EXPECT_TRUE(host->needs_hold());
    ASSERT_TRUE(settle(w)); EXPECT_FALSE(w->active()->dirty()); EXPECT_FALSE(host->needs_hold());
}
TEST_F(LibraryHostTest, CancellationAfterFilePublicationDoesNotLoseSavedState) {
    auto key = persistent_host(); w->new_collection("Published before cancellation");
    auto destination = path("already-published.valib"); w->save(destination);
    // Let the worker publish without dispatching its main-context completion.
    // The file boundary is evidence, not an arbitrary sleep or timeout extension.
    auto end = g_get_monotonic_time() + 10000000;
    while (!std::filesystem::exists(destination) && g_get_monotonic_time() < end) g_usleep(1000);
    ASSERT_TRUE(std::filesystem::exists(destination)); ASSERT_TRUE(w->busy());
    Decisions ui; ui.answers = {{LibraryCloseChoice::CancelWork}};
    auto guard = host->prepare(host->slots(), ui); ASSERT_TRUE(guard) << w->message();
    EXPECT_FALSE(w->active()->dirty()); EXPECT_EQ(w->active()->path, destination);
    guard->commit(); guard.reset(); EXPECT_TRUE(std::filesystem::exists(destination));
    EXPECT_EQ(disk_preference(key + "/0/file0"), destination);
}
TEST_F(LibraryHostTest, PreparedReservationBlocksNewEditsAndNestedGuardCannotCommit) {
    Decisions ui; ui.answers = {{LibraryCloseChoice::Discard}};
    auto guard = host->prepare(host->slots(), ui); ASSERT_TRUE(guard);
    EXPECT_THROW(w->new_collection("Not admitted"), std::runtime_error);
    EXPECT_THROW(w->save(path("not-admitted.valib")), std::runtime_error);
    EXPECT_THROW(w->accept_import(false), std::runtime_error);
    EXPECT_THROW(w->cancel(), std::runtime_error);
    auto nested = host->prepare(host->slots(), ui); ASSERT_TRUE(nested); nested->commit(); nested.reset();
    EXPECT_TRUE(w->active()); EXPECT_TRUE(w->closing()); guard.reset(); EXPECT_TRUE(w->active()->dirty());
}
TEST_F(LibraryHostTest, NestedCloseDuringPromptIsRejectedWithoutRecursion) {
    Decisions ui; ui.answers = {{LibraryCloseChoice::Cancel}};
    unsigned calls = 0;
    ui.during_prompt = [&] { ++calls; EXPECT_FALSE(host->prepare(host->slots(), ui)); EXPECT_THROW(w->unload(true), std::runtime_error); };
    EXPECT_FALSE(host->prepare(host->slots(), ui)); EXPECT_EQ(calls, 1u); EXPECT_TRUE(w->active()->dirty());
}
TEST_F(LibraryHostTest, ImportDiscardRestoresCandidateWhenLaterCloseCancelled) {
    auto input = path("original.svg");
    { std::ofstream out(input); out << "<svg xmlns='http://www.w3.org/2000/svg' width='10mm' height='10mm' viewBox='0 0 10 10'><rect width='5' height='6'/></svg>"; ASSERT_TRUE(out.good()); }
    w->import_files({input}); ASSERT_TRUE(settle(w)); ASSERT_TRUE(w->pending_import());
    Decisions ui; ui.answers = {{LibraryCloseChoice::Discard}, {LibraryCloseChoice::Cancel}};
    EXPECT_FALSE(host->prepare(host->slots(), ui)); ASSERT_TRUE(w->pending_import());
    EXPECT_EQ(w->pending_import()->entries.size(), 1u); EXPECT_TRUE(w->rows().empty());
    w->cancel();
}
TEST_F(LibraryHostTest, NonZipOpenCanFinishItsAdmittedImportChainUnderReservation) {
    auto input = path("original.svg");
    { std::ofstream out(input); out << "<svg xmlns='http://www.w3.org/2000/svg' width='10mm' height='10mm' viewBox='0 0 10 10'><rect width='5' height='6'/></svg>"; }
    w->open(input);
    Decisions ui; ui.answers = {{LibraryCloseChoice::Wait}, {LibraryCloseChoice::Discard}, {LibraryCloseChoice::Cancel}};
    EXPECT_FALSE(host->prepare(host->slots(), ui));
    ASSERT_TRUE(w->pending_import()) << w->message(); ASSERT_EQ(w->pending_import()->entries.size(), 1u);
    EXPECT_TRUE(w->pending_import()->entries[0].converted); w->cancel();
}
TEST_F(LibraryHostTest, QuitReservationAlsoIncludesUnboundWorkspace) {
    auto orphan = host->acquire("orphan"); orphan->new_collection("Retained without panel");
    Decisions ui; ui.answers = {{LibraryCloseChoice::Discard}, {LibraryCloseChoice::Discard}};
    auto guard = host->prepare(host->slots(), ui); ASSERT_TRUE(guard);
    EXPECT_TRUE(orphan->closing()); EXPECT_TRUE(w->closing()); EXPECT_TRUE(host->needs_hold());
    guard->commit(); guard.reset(); EXPECT_TRUE(orphan->collections().empty()); EXPECT_FALSE(host->needs_hold());
}
TEST_F(LibraryHostTest, WaitingQuitReservationRejectsNewWorkspaceUntilCancelled) {
    Decisions ui; ui.answers = {{LibraryCloseChoice::Discard}};
    auto guard = host->prepare(host->slots(), ui); ASSERT_TRUE(guard);
    EXPECT_THROW(host->acquire("callback-created-slot"), std::runtime_error);
    guard.reset(); EXPECT_NO_THROW(host->acquire("callback-created-slot"));
}
}
