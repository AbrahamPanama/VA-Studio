// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief FontList ownership/lifetime: the process-wide FontTags subscription
 *
 * Independent regression for the F1 lifetime defect: FontList connected a
 * lambda capturing `this` to the process-wide FontTags signal and discarded the
 * returned connection, so the destroyed widget stayed subscribed. These cases
 * use real FontList consumers (no mocked widget lifetime) under the project's
 * Gtk/Inkscape test application and assert the global signal slot count returns
 * to its baseline after destruction.
 *
 * FontTags::tag_font() only mutates the font->tag mapping and does NOT emit;
 * deselect_all() emits only while a tag is actually selected. Notifications are
 * therefore driven exclusively through the public select_tag()/deselect_all()
 * API so a callback really runs against the state left after destruction.
 */

#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>

#include <csignal>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <glibmm/main.h>
#include <gtkmm/listview.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/treelistrow.h>

#include "inkscape.h"
#include "inkscape-application.h"
#include "ui/widget/font-list.h"
#include "util/font-discovery.h"
#include "util/font-collections.h"
#include "util/recently-used-fonts.h"
#include "libnrtype/font-lister.h"
#include "io/resource.h"
#include <glib/gstdio.h>

// InkscapeApplication is declared at global scope (inkscape-application.h closes
// namespace Inkscape before the class), so it must not be qualified as
// Inkscape::InkscapeApplication.
using Inkscape::Application;
using Inkscape::FontTags;
using Inkscape::UI::Widget::FontList;
using Inkscape::UI::Widget::FontListMode;

namespace {

InkscapeApplication &testApplication()
{
    static auto application = [] {
        auto const original_profile = std::getenv("INKSCAPE_PROFILE_DIR");
        auto const profile = std::string(original_profile ? original_profile : g_get_tmp_dir()) + "/perfil-é-字体";
        g_mkdir_with_parents(profile.c_str(), 0700);
        g_setenv("INKSCAPE_PROFILE_DIR", profile.c_str(), true);
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "fontlistlifecycletest", true);
        auto result = new InkscapeApplication();
        result->gio_app()->register_application();
        // Preserve fatal failures (including shutdown) as test failures rather
        // than waiting in the application's interactive emergency dialog.
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
    // Bound diagnostics even if a broken idle keeps rescheduling itself.
    for (unsigned i = 0; i < 10000 && g_main_context_pending(nullptr); ++i) {
        g_main_context_iteration(nullptr, false);
    }
}

std::unique_ptr<FontList> makeConsumer(std::string const &name)
{
    // Distinct preference paths keep the two consumers independent; the compact
    // mode exercises the same FontTags subscription path as the browser.
    return std::make_unique<FontList>("/tests/font-list-lifecycle-" + name,
                                      FontListMode::CompactPopover);
}

// Discovery is asynchronous; every wait below is bounded by a ceiling and a
// timeout is a test failure, never a skip. The predicate is always a real
// observation of the product state.
constexpr unsigned readiness_ceiling_ms = 30000;

bool pumpUntil(std::function<bool ()> const &done, unsigned ceiling_ms)
{
    if (done()) return true;
    auto loop = Glib::MainLoop::create();
    auto poll = Glib::signal_timeout().connect([&] {
        if (done()) { loop->quit(); return false; }
        return true;
    }, 10);
    auto ceiling = Glib::signal_timeout().connect([&] { loop->quit(); return false; }, ceiling_ms);
    loop->run();
    poll.disconnect();
    ceiling.disconnect();
    return done();
}

// Capture the native discovery payload that feeds both consumers' stores, so
// expected family sets are derived from the isolated fixture's real entries
// rather than a fabricated global count. A cached replay arrives synchronously.
Inkscape::FontDiscovery::FontsPayload discoverPayload(std::string &why)
{
    Inkscape::FontDiscovery::FontsPayload payload;
    bool finished = false;
    auto connection = Inkscape::FontDiscovery::get().connect_to_fonts(
        [&](Inkscape::FontDiscovery::MessageType const &msg) {
            if (auto result = Inkscape::Async::Msg::get_result(msg)) payload = *result;
            if (Inkscape::Async::Msg::is_finished(msg)) finished = true;
        });
    if (!finished && !pumpUntil([&] { return finished; }, readiness_ceiling_ms)) {
        why = "font discovery did not finish within the readiness ceiling";
        connection.disconnect();
        return {};
    }
    connection.disconnect();
    return payload;
}

// The main font list is the ListView whose selection model holds TreeListRows;
// the compact popover's recent list is backed by plain font objects instead.
Gtk::ListView *findMainListView(Gtk::Widget &root)
{
    if (auto *view = dynamic_cast<Gtk::ListView *>(&root)) {
        if (auto selection = std::dynamic_pointer_cast<Gtk::SingleSelection>(view->get_model())) {
            if (selection->get_n_items() > 0 &&
                std::dynamic_pointer_cast<Gtk::TreeListRow>(selection->get_object(0))) {
                return view;
            }
        }
    }
    for (auto *child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto *found = findMainListView(*child)) return found;
    }
    return nullptr;
}

Glib::RefPtr<Gtk::SingleSelection> mainModel(Gtk::ListView *view)
{
    if (!view) return Glib::RefPtr<Gtk::SingleSelection>{};
    return std::dynamic_pointer_cast<Gtk::SingleSelection>(view->get_model());
}

bool modelsAtUniverse(FontList &first, FontList &second, std::size_t count)
{
    auto first_selection = mainModel(findMainListView(first));
    auto second_selection = mainModel(findMainListView(second));
    return first_selection && second_selection &&
           static_cast<std::size_t>(first_selection->get_n_items()) == count &&
           static_cast<std::size_t>(second_selection->get_n_items()) == count;
}

// Enumerate the families actually present in this consumer's native filtered
// model. Family identity comes from selecting each top-level row and reading the
// public get_fontspec(); the pre-existing selection is restored afterwards.
std::set<std::string> visibleFamilies(FontList &list, Gtk::ListView *view)
{
    std::set<std::string> families;
    auto selection = mainModel(view);
    if (!selection) return families;
    guint const saved = selection->get_selected();
    guint const total = selection->get_n_items();
    for (guint i = 0; i < total; ++i) {
        selection->set_selected(i);
        if (selection->get_selected() != i) continue;
        std::string spec = list.get_fontspec().raw();
        auto const comma = spec.find(", ");
        families.insert(comma == std::string::npos ? spec : spec.substr(0, comma));
    }
    selection->set_selected(saved);
    return families;
}

// Select the native model row for a family that is currently visible, leaving
// the selection on it, and report whether it was found.
bool selectFamily(FontList &list, Gtk::ListView *view, std::string const &family)
{
    auto selection = mainModel(view);
    if (!selection) return false;
    guint const total = selection->get_n_items();
    for (guint i = 0; i < total; ++i) {
        selection->set_selected(i);
        if (selection->get_selected() != i) continue;
        std::string spec = list.get_fontspec().raw();
        auto const comma = spec.find(", ");
        if ((comma == std::string::npos ? spec : spec.substr(0, comma)) == family) return true;
    }
    return false;
}

struct FixtureSets
{
    std::set<std::string> families;
    std::map<std::string, std::set<std::string>> by_tag;
    std::map<std::string, std::string> face_by_family;
    // Native canonical key (font_specification) per family, retained alongside
    // the public face name for the selection-preservation diagnostic.
    std::map<std::string, std::string> native_spec_by_family;
    std::set<std::string> variable_families;
};

// Universe and per-category family sets derived from the real native entries and
// the process-wide tag map that production prepare_tags() populated for them.
FixtureSets deriveFixture(Inkscape::FontDiscovery::FontsPayload const &payload, FontTags &tags)
{
    FixtureSets sets;
    for (auto const &family : *payload) {
        if (family.empty()) continue;
        auto const &regular = Inkscape::get_family_font(family);
        std::string const name = Inkscape::font_family_name(regular).raw();
        if (name.empty()) continue;
#ifdef __APPLE__
        if (name.front() == '.') continue;
#endif
        sets.families.insert(name);
        sets.face_by_family[name] = Inkscape::font_face_name(regular).raw();
        sets.native_spec_by_family[name] = Inkscape::font_specification(regular).raw();
        if (regular.variable_font) sets.variable_families.insert(name);
        for (auto const &tag : tags.get_font_tags(Inkscape::font_specification(regular))) {
            sets.by_tag[tag].insert(name);
        }
    }
    return sets;
}

// Pick one category that really selects a proper non-empty subset, plus one
// category that selects nothing (all-hidden). Both are derived, not hard-coded.
bool chooseTags(FixtureSets const &sets, FontTags &tags, std::string &subset_tag, std::string &empty_tag)
{
    std::size_t best = 0;
    bool have_subset = false;
    for (auto const &tag : tags.get_tags()) {
        auto const it = sets.by_tag.find(tag.tag);
        std::size_t const count = it == sets.by_tag.end() ? 0u : it->second.size();
        if (count == 0) {
            if (empty_tag.empty()) empty_tag = tag.tag;
        } else if (count < sets.families.size() && count > best) {
            best = count;
            subset_tag = tag.tag;
            have_subset = true;
        }
    }
    return have_subset && !empty_tag.empty();
}

bool prepareConsumers(std::string const &prefix, FontTags &tags, std::unique_ptr<FontList> &first,
                      std::unique_ptr<FontList> &second, Gtk::ListView *&first_view,
                      Gtk::ListView *&second_view, FixtureSets &sets, std::string &why)
{
    first = makeConsumer(prefix + "-first");
    second = makeConsumer(prefix + "-second");
    drainMainContext();

    auto payload = discoverPayload(why);
    if (!payload || payload->empty()) {
        if (why.empty()) why = "font discovery produced no payload for the isolated fixture";
        return false;
    }
    sets = deriveFixture(payload, tags);
    if (sets.families.empty()) {
        why = "isolated fixture exposed no font families";
        return false;
    }
    if (!pumpUntil([&] { return modelsAtUniverse(*first, *second, sets.families.size()); },
                   readiness_ceiling_ms)) {
        why = "font models did not reach the discovered family count within the readiness ceiling";
        return false;
    }
    first_view = findMainListView(*first);
    second_view = findMainListView(*second);
    if (!first_view || !second_view) {
        why = "could not locate a live main font ListView";
        return false;
    }
    return true;
}

std::vector<std::string> selectedTagIds(FontTags &tags)
{
    std::vector<std::string> ids;
    for (auto const &tag : tags.get_selected_tags()) ids.push_back(tag.tag);
    return ids;
}

void restoreTagSelection(FontTags &tags, std::vector<std::string> const &ids)
{
    tags.deselect_all();
    for (auto const &id : ids) tags.select_tag(id, true);
}

// Clears the process-wide FontTags selection for the length of a case so every
// consumer starts from the full family universe, then restores the entry
// selection even when a fatal ASSERT returns early. Declared before the
// consumer unique_ptrs so the consumers are torn down before the restore.
class CategoryStateGuard
{
public:
    explicit CategoryStateGuard(FontTags &tags)
        : _tags(tags)
        , _original(selectedTagIds(tags))
    {
        _tags.deselect_all();
    }

    ~CategoryStateGuard() { restoreTagSelection(_tags, _original); }

    CategoryStateGuard(CategoryStateGuard const &) = delete;
    CategoryStateGuard &operator=(CategoryStateGuard const &) = delete;

private:
    FontTags &_tags;
    std::vector<std::string> _original;
};

struct ChoiceCounters
{
    int preview = 0;
    int commit = 0;
    int cancel = 0;
    int fontspec_changed = 0;
};

void watchChoices(FontList &list, ChoiceCounters &counters, sigc::scoped_connection &choice_connection,
                  sigc::scoped_connection &fontspec_connection)
{
    choice_connection = list.signal_font_choice().connect([&counters](FontChoice const &choice) {
        switch (choice.phase) {
            case FontChoicePhase::Preview: ++counters.preview; break;
            case FontChoicePhase::Commit: ++counters.commit; break;
            case FontChoicePhase::Cancel: ++counters.cancel; break;
        }
    });
    fontspec_connection = list.signal_fontspec_changed().connect([&counters] { ++counters.fontspec_changed; });
}

} // namespace

class FontListLifecycleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto const gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "Skipping GUI integration test: GUI testing not enabled";
        }
        auto const fontconfig = std::getenv("INKSCAPE_FONTCONFIG");
        ASSERT_TRUE(fontconfig && *fontconfig != '\0')
            << "font-list category-sync tests require the isolated INKSCAPE_FONTCONFIG fixture; "
               "a missing fixture must fail, not fall back to the user's fonts";
        ASSERT_TRUE(testApplication().gtk_app());
        ASSERT_TRUE(Application::exists());
    }
};

TEST_F(FontListLifecycleTest, FontPersistenceRoundTripsThroughUnicodeProfile)
{
    EXPECT_NE(Inkscape::IO::Resource::profile_path().find("perfil-é-字体"), std::string::npos);
    auto *lister = Inkscape::FontLister::get_instance();
    ASSERT_FALSE(lister->pango_family_map.empty());
    Glib::ustring const font = lister->pango_family_map.begin()->first;
    ASSERT_TRUE(lister->font_installed_on_system(font));
    auto *collections = Inkscape::FontCollections::get();
    Glib::ustring const name = "colección-字体";
    collections->write_collection(name, {font});
    collections->clear();
    collections->init();
    EXPECT_TRUE(collections->find_collection(name));
    EXPECT_TRUE(collections->is_font_in_collection(name, font));
    auto *recent = Inkscape::RecentlyUsedFonts::get();
    recent->prepend_to_list(font);
    recent->clear();
    recent->init();
    ASSERT_FALSE(recent->get_fonts().empty());
    EXPECT_EQ(recent->get_fonts().front(), font);
    recent->clear();
    recent->write_recently_used_fonts();
    collections->remove_collection(name);
}

TEST_F(FontListLifecycleTest, DestroyedConsumerReleasesGlobalFontTagsSlot)
{
    auto &tags = FontTags::get();
    auto &tag_signal = tags.get_signal_tag_changed();
    auto const baseline = tag_signal.size();

    auto survivor = makeConsumer("survivor");
    auto doomed = makeConsumer("doomed");
    drainMainContext();
    ASSERT_EQ(tag_signal.size(), baseline + 2u);

    // Destroy the second consumer for real: hide/unmap is not destruction.
    doomed.reset();
    drainMainContext();
    ASSERT_EQ(tag_signal.size(), baseline + 1u) << "destroyed consumer kept its global slot";

    // Normalize the known tag, then raise a real notification after the doomed
    // subscriber was destroyed. tag_font() would not emit; select_tag() does.
    tags.select_tag("sans", false);
    ASSERT_TRUE(tags.select_tag("sans", true));
    ASSERT_TRUE(tags.is_tag_selected("sans"));
    drainMainContext();
    ASSERT_EQ(tag_signal.size(), baseline + 1u);

    // deselect_all() returns true only while a tag is actually selected and
    // emits for it; the destroyed consumer's detached callback must not run.
    ASSERT_TRUE(tags.deselect_all());
    ASSERT_FALSE(tags.is_tag_selected("sans"));
    drainMainContext();
    ASSERT_EQ(tag_signal.size(), baseline + 1u);

    survivor.reset();
    drainMainContext();
    EXPECT_EQ(tag_signal.size(), baseline);
}

TEST_F(FontListLifecycleTest, RepeatedConsumersDoNotAccumulateGlobalSlots)
{
    auto &tags = FontTags::get();
    auto &tag_signal = tags.get_signal_tag_changed();
    auto const baseline = tag_signal.size();

    auto survivor = makeConsumer("repeated-survivor");
    drainMainContext();
    ASSERT_EQ(tag_signal.size(), baseline + 1u);

    // Isolate from any selection state left by earlier cases.
    tags.select_tag("serif", false);

    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        auto temporary = makeConsumer("cycle-" + std::to_string(cycle));
        drainMainContext();
        ASSERT_EQ(tag_signal.size(), baseline + 2u) << "cycle " << cycle;

        temporary.reset();
        drainMainContext();
        ASSERT_EQ(tag_signal.size(), baseline + 1u)
            << "cycle " << cycle << " leaked a subscription";

        // After each destruction, drive a real notification through the public
        // selection API (tag_font() alone does not emit) and re-assert the slot
        // count, then restore the deselected state.
        ASSERT_TRUE(tags.select_tag("serif", true)) << "cycle " << cycle;
        ASSERT_TRUE(tags.is_tag_selected("serif")) << "cycle " << cycle;
        drainMainContext();
        ASSERT_EQ(tag_signal.size(), baseline + 1u) << "cycle " << cycle;

        ASSERT_TRUE(tags.deselect_all()) << "cycle " << cycle;
        ASSERT_FALSE(tags.is_tag_selected("serif")) << "cycle " << cycle;
        drainMainContext();
        ASSERT_EQ(tag_signal.size(), baseline + 1u) << "cycle " << cycle;
    }

    survivor.reset();
    drainMainContext();
    EXPECT_EQ(tag_signal.size(), baseline);
}

TEST_F(FontListLifecycleTest, SurvivingConsumerProcessesNotificationsWithoutCrashing)
{
    auto &tags = FontTags::get();
    auto &tag_signal = tags.get_signal_tag_changed();
    auto const baseline = tag_signal.size();

    auto survivor = makeConsumer("responsive-survivor");
    drainMainContext();
    ASSERT_EQ(tag_signal.size(), baseline + 1u);

    // Notification-safety only: clear and toggle each known tag through the
    // production FontTags API so the surviving consumer processes the resulting
    // signal emissions without crashing. This does NOT assert that the font
    // list contents stay in sync with the tag selection (that is F2, tracked
    // separately and not exercised here).
    for (auto const &tag : tags.get_tags()) {
        tags.select_tag(tag.tag, false);
        tags.select_tag(tag.tag, true);
    }
    ASSERT_TRUE(tags.deselect_all());
    drainMainContext();
    EXPECT_EQ(tag_signal.size(), baseline + 1u);

    survivor.reset();
    drainMainContext();
    EXPECT_EQ(tag_signal.size(), baseline);
}

// F2: a single shared category change made outside any widget must update the
// native filtered model of every live consumer, not just the originating one,
// and must not emit Preview/Commit or a fontspec change into a document.
TEST_F(FontListLifecycleTest, ExternalCategoryChangeRefreshesEveryConsumerModel)
{
    auto &tags = FontTags::get();
    CategoryStateGuard category_guard(tags);

    std::unique_ptr<FontList> first;
    std::unique_ptr<FontList> second;
    Gtk::ListView *first_view = nullptr;
    Gtk::ListView *second_view = nullptr;
    FixtureSets sets;
    std::string why;
    ASSERT_TRUE(prepareConsumers("f2-live", tags, first, second, first_view, second_view, sets, why)) << why;

    std::string subset_tag;
    std::string empty_tag;
    ASSERT_TRUE(chooseTags(sets, tags, subset_tag, empty_tag))
        << "isolated fixture exposes no proper-subset category plus an empty category";

    auto const &expected = sets.by_tag[subset_tag];
    ASSERT_FALSE(expected.empty());
    // Prefer a static family so selection preservation is exercised without
    // variable-axis state, which would append variations to the saved spec.
    std::string keep_family;
    for (auto const &candidate : expected) {
        if (!sets.variable_families.contains(candidate)) {
            keep_family = candidate;
            break;
        }
    }
    if (keep_family.empty()) keep_family = *expected.begin();
    ASSERT_TRUE(selectFamily(*first, first_view, keep_family)) << "chosen family was not in the native model";
    std::string const kept_spec = first->get_fontspec().raw();
    {
        auto const comma = kept_spec.find(", ");
        EXPECT_EQ(comma == std::string::npos ? kept_spec : kept_spec.substr(0, comma), keep_family);
    }

    // Diagnostic: report the public display spec and the native canonical key
    // that find_font/select_font actually compare. Keys may coincide or differ
    // by platform, so this asserts only that the real payload supplied one.
    auto const native_key = sets.native_spec_by_family.find(keep_family);
    ASSERT_NE(native_key, sets.native_spec_by_family.end())
        << "fixture retained no native canonical key for the chosen family";
    EXPECT_FALSE(native_key->second.empty()) << "native canonical key for " << keep_family << " is empty";
    SCOPED_TRACE("selection public spec: " + kept_spec + "; native canonical key: " + native_key->second);

    ChoiceCounters first_choices;
    ChoiceCounters second_choices;
    sigc::scoped_connection first_choice;
    sigc::scoped_connection first_fontspec;
    sigc::scoped_connection second_choice;
    sigc::scoped_connection second_fontspec;
    watchChoices(*first, first_choices, first_choice, first_fontspec);
    watchChoices(*second, second_choices, second_choice, second_fontspec);

    ASSERT_TRUE(tags.select_tag(subset_tag, true));
    drainMainContext();

    EXPECT_EQ(first->get_fontspec().raw(), kept_spec) << "chosen visible font was not preserved";
    EXPECT_EQ(first_choices.preview, 0) << "external category change emitted Preview";
    EXPECT_EQ(first_choices.commit, 0) << "external category change emitted Commit";
    EXPECT_EQ(first_choices.fontspec_changed, 0) << "external category change emitted fontspec changed";
    EXPECT_EQ(second_choices.preview, 0);
    EXPECT_EQ(second_choices.commit, 0);
    EXPECT_EQ(second_choices.fontspec_changed, 0);

    EXPECT_EQ(visibleFamilies(*first, first_view), expected)
        << "first consumer's native model does not match the fixture-derived category set";
    EXPECT_EQ(visibleFamilies(*second, second_view), expected)
        << "second consumer's native model does not match the fixture-derived category set";

    ASSERT_TRUE(tags.select_tag(subset_tag, false));
    drainMainContext();
    EXPECT_EQ(visibleFamilies(*first, first_view), sets.families);
    EXPECT_EQ(visibleFamilies(*second, second_view), sets.families);

    first.reset();
    second.reset();
    drainMainContext();
}

// F2: an all-hidden category empties both native models; deselect_all() is the
// shared bulk path that must restore every consumer's rows.
TEST_F(FontListLifecycleTest, EmptyCategoryHidesAllRowsAndBulkClearRestoresEveryConsumer)
{
    auto &tags = FontTags::get();
    CategoryStateGuard category_guard(tags);

    std::unique_ptr<FontList> first;
    std::unique_ptr<FontList> second;
    Gtk::ListView *first_view = nullptr;
    Gtk::ListView *second_view = nullptr;
    FixtureSets sets;
    std::string why;
    ASSERT_TRUE(prepareConsumers("f2-hidden", tags, first, second, first_view, second_view, sets, why)) << why;

    std::string subset_tag;
    std::string empty_tag;
    ASSERT_TRUE(chooseTags(sets, tags, subset_tag, empty_tag))
        << "isolated fixture exposes no proper-subset category plus an empty category";

    ASSERT_TRUE(tags.select_tag(empty_tag, true));
    drainMainContext();

    auto first_selection = mainModel(first_view);
    auto second_selection = mainModel(second_view);
    ASSERT_TRUE(first_selection);
    ASSERT_TRUE(second_selection);
    EXPECT_EQ(first_selection->get_n_items(), 0u) << "empty category left rows in the first consumer";
    EXPECT_EQ(second_selection->get_n_items(), 0u) << "empty category left rows in the second consumer";
    EXPECT_TRUE(visibleFamilies(*first, first_view).empty());
    EXPECT_TRUE(visibleFamilies(*second, second_view).empty());

    ASSERT_TRUE(tags.deselect_all()) << "bulk clear should report a real change";
    drainMainContext();
    EXPECT_EQ(visibleFamilies(*first, first_view), sets.families)
        << "bulk clear did not restore the first consumer's rows";
    EXPECT_EQ(visibleFamilies(*second, second_view), sets.families)
        << "bulk clear did not restore the second consumer's rows";

    first.reset();
    second.reset();
    drainMainContext();
}

// F2/F1: destroying one consumer must not leave the survivor stale or attached.
TEST_F(FontListLifecycleTest, DestroyedConsumerLeavesSurvivorModelResponsive)
{
    auto &tags = FontTags::get();
    CategoryStateGuard category_guard(tags);
    auto &tag_signal = tags.get_signal_tag_changed();
    auto const baseline = tag_signal.size();

    std::unique_ptr<FontList> first;
    std::unique_ptr<FontList> second;
    Gtk::ListView *first_view = nullptr;
    Gtk::ListView *second_view = nullptr;
    FixtureSets sets;
    std::string why;
    ASSERT_TRUE(prepareConsumers("f2-destroyed", tags, first, second, first_view, second_view, sets, why)) << why;

    std::string subset_tag;
    std::string empty_tag;
    ASSERT_TRUE(chooseTags(sets, tags, subset_tag, empty_tag))
        << "isolated fixture exposes no proper-subset category plus an empty category";
    auto const &expected = sets.by_tag[subset_tag];

    second.reset();
    second_view = nullptr;
    drainMainContext();
    ASSERT_EQ(tag_signal.size(), baseline + 1u) << "destroyed consumer kept a global slot";

    ASSERT_TRUE(tags.select_tag(subset_tag, true));
    drainMainContext();
    EXPECT_EQ(visibleFamilies(*first, first_view), expected)
        << "survivor did not refresh its native model after the peer was destroyed";

    ASSERT_TRUE(tags.deselect_all());
    drainMainContext();
    EXPECT_EQ(visibleFamilies(*first, first_view), sets.families);

    first.reset();
    drainMainContext();
}
