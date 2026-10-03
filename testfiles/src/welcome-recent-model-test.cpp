// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests for the Welcome recent-file metadata model.
 *
 * These tests use only caller-supplied metadata. They never touch GTK, a display,
 * the user's real recent history or any referenced document on disk.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/dialog/welcome-recent-model.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Inkscape::UI::Dialog::Welcome::RecentFileRecord;
using Inkscape::UI::Dialog::Welcome::RecentFormat;
using Inkscape::UI::Dialog::Welcome::RecoveryKind;
using Inkscape::UI::Dialog::Welcome::PreviewStatus;

RecentFileRecord makeRecord(std::string display_name, std::string display_location,
                            std::string mime_type = "image/svg+xml", std::string uri = {})
{
    RecentFileRecord record;
    record.display_name = std::move(display_name);
    record.display_location = std::move(display_location);
    record.uri = uri.empty() ? "file://" + record.display_location : std::move(uri);
    record.mime_type = std::move(mime_type);
    record.format = Inkscape::UI::Dialog::Welcome::classifyRecentFormat(record.uri, record.mime_type);
    return record;
}

std::vector<std::size_t> search(std::vector<RecentFileRecord> const &records, std::string const &query)
{
    return Inkscape::UI::Dialog::Welcome::searchRecentRecords(records, query);
}

Glib::DateTime utc(int year, int month, int day)
{
    return Glib::DateTime::create_utc(year, month, day, 0, 0, 0);
}

} // namespace

TEST(WelcomeRecentModelTest, FoldedSearchKeyHandlesInternationalText)
{
    using Inkscape::UI::Dialog::Welcome::foldedSearchKey;

    EXPECT_EQ(foldedSearchKey("Diseño"), foldedSearchKey("DISEÑO"));
    EXPECT_EQ(foldedSearchKey("Canción"), foldedSearchKey("CANCIÓN"));
    // "Canción" precomposed (NFC) and "Cancio\u0301n" decomposed (NFD) must fold equal.
    EXPECT_EQ(foldedSearchKey("Canci\u00F3n"), foldedSearchKey("Cancio\u0301n"));
    // Casefold does not strip diacritics; search is case-insensitive, not accent-insensitive.
    EXPECT_NE(foldedSearchKey("Diseño"), foldedSearchKey("diseno"));
}

TEST(WelcomeRecentModelTest, MatchesNameCaseInsensitive)
{
    std::vector<RecentFileRecord> records = {makeRecord("Diseño.svg", "/home/Usuario/Diseño.svg")};
    EXPECT_EQ(search(records, "DISEÑO"), (std::vector<std::size_t>{0}));
}

TEST(WelcomeRecentModelTest, NormalizesComposedAndDecomposedAccents)
{
    std::vector<RecentFileRecord> records = {makeRecord("Cancio\u0301n.svg", "/home/Usuario/Cancio\u0301n.svg")};
    EXPECT_EQ(search(records, "CANCI\u00D3N"), (std::vector<std::size_t>{0}));
}

TEST(WelcomeRecentModelTest, MatchesLocationCaseInsensitive)
{
    std::vector<RecentFileRecord> records = {makeRecord("logo.svg", "/home/Usuario/Imágenes/logo.svg")};
    EXPECT_EQ(search(records, "usuario"), (std::vector<std::size_t>{0}));
    EXPECT_EQ(search(records, "IMÁGENES"), (std::vector<std::size_t>{0}));
}

TEST(WelcomeRecentModelTest, DuplicateDisplayNamesDistinctLocations)
{
    std::vector<RecentFileRecord> records = {
        makeRecord("logo.svg", "/proyectos/a/logo.svg"),
        makeRecord("logo.svg", "/proyectos/b/logo.svg"),
    };
    EXPECT_EQ(search(records, "a/logo"), (std::vector<std::size_t>{0}));
    EXPECT_EQ(search(records, "b/logo"), (std::vector<std::size_t>{1}));
}

TEST(WelcomeRecentModelTest, PreservesUnfamiliarAndRemoteUris)
{
    auto const remote_uri = std::string("smb://servidor/compartido/dise%C3%B1o%20remoto.svg");
    std::vector<RecentFileRecord> records = {
        makeRecord("diseño remoto.svg", "smb://servidor/compartido/diseño remoto.svg", "image/svg+xml", remote_uri),
    };

    EXPECT_EQ(records[0].uri, remote_uri);
    EXPECT_EQ(records[0].format, RecentFormat::Svg);
    EXPECT_EQ(search(records, "compartido"), (std::vector<std::size_t>{0}));
}

TEST(WelcomeRecentModelTest, UnsupportedFormatStillPresentAndOpenable)
{
    std::vector<RecentFileRecord> records = {makeRecord("foto.png", "/home/Usuario/foto.png", "image/png")};

    EXPECT_EQ(records[0].format, RecentFormat::Unsupported);
    EXPECT_TRUE(records[0].openable);
    EXPECT_EQ(search(records, "foto"), (std::vector<std::size_t>{0}));
    EXPECT_FALSE(Inkscape::UI::Dialog::Welcome::recentRecordsHaveRecovery(records));
}

TEST(WelcomeRecentModelTest, ClassifiesSvgFormatFromMimeOrExtension)
{
    using Inkscape::UI::Dialog::Welcome::classifyRecentFormat;

    EXPECT_EQ(classifyRecentFormat("file:///a/b.svg", ""), RecentFormat::Svg);
    EXPECT_EQ(classifyRecentFormat("file:///a/b.svgz", ""), RecentFormat::Svg);
    EXPECT_EQ(classifyRecentFormat("file:///a/B.SVG", ""), RecentFormat::Svg);
    EXPECT_EQ(classifyRecentFormat("file:///a/b.svg?x=1", ""), RecentFormat::Svg);
    EXPECT_EQ(classifyRecentFormat("file:///a/b.svg#frag", ""), RecentFormat::Svg);
    EXPECT_EQ(classifyRecentFormat("file:///a/b", "image/svg+xml"), RecentFormat::Svg);
    EXPECT_EQ(classifyRecentFormat("smb://host/share/x.svgz", "application/octet-stream"), RecentFormat::Svg);
    EXPECT_EQ(classifyRecentFormat("file:///a/b.png", "image/png"), RecentFormat::Unsupported);
    EXPECT_EQ(classifyRecentFormat("file:///a/b.svg.txt", ""), RecentFormat::Unsupported);
}

TEST(WelcomeRecentModelTest, ClassifiesRecoveryKindFromGroups)
{
    using Inkscape::UI::Dialog::Welcome::classifyRecoveryKind;

    EXPECT_EQ(classifyRecoveryKind(false, false), RecoveryKind::None);
    EXPECT_EQ(classifyRecoveryKind(false, true), RecoveryKind::Crash);
    EXPECT_EQ(classifyRecoveryKind(true, false), RecoveryKind::Auto);
    EXPECT_EQ(classifyRecoveryKind(true, true), RecoveryKind::Auto);
}

TEST(WelcomeRecentModelTest, RecentRecordsHaveRecovery)
{
    auto auto_record = makeRecord("auto.svg", "/tmp/auto.svg");
    auto_record.recovery = RecoveryKind::Auto;
    auto crash_record = makeRecord("crash.svg", "/tmp/crash.svg");
    crash_record.recovery = RecoveryKind::Crash;
    auto plain_record = makeRecord("plain.svg", "/home/Usuario/plain.svg");

    EXPECT_FALSE(Inkscape::UI::Dialog::Welcome::recentRecordsHaveRecovery({plain_record}));
    EXPECT_TRUE(Inkscape::UI::Dialog::Welcome::recentRecordsHaveRecovery({plain_record, crash_record}));
    EXPECT_TRUE(Inkscape::UI::Dialog::Welcome::recentRecordsHaveRecovery({auto_record}));
}

TEST(WelcomeRecentModelTest, EmptyQueryReturnsAllInOrder)
{
    std::vector<RecentFileRecord> records = {
        makeRecord("a.svg", "/a.svg"),
        makeRecord("b.svg", "/b.svg"),
        makeRecord("c.svg", "/c.svg"),
    };
    EXPECT_EQ(search(records, ""), (std::vector<std::size_t>{0, 1, 2}));
}

TEST(WelcomeRecentModelTest, EmptyListReturnsNoMatches)
{
    std::vector<RecentFileRecord> const empty;
    EXPECT_TRUE(search(empty, "").empty());
    EXPECT_TRUE(search(empty, "anything").empty());
}

TEST(WelcomeRecentModelTest, NoMatchReturnsEmpty)
{
    std::vector<RecentFileRecord> records = {makeRecord("a.svg", "/a.svg")};
    EXPECT_TRUE(search(records, "zzz-no-such-name").empty());
}

TEST(WelcomeRecentModelTest, PreviewVsOpenStateIndependent)
{
    std::vector<RecentFileRecord> records = {makeRecord("preview.svg", "/home/Usuario/preview.svg")};

    records[0].preview = PreviewStatus::Failed;
    EXPECT_TRUE(records[0].openable);
    EXPECT_EQ(search(records, "preview"), (std::vector<std::size_t>{0}));

    records[0].preview = PreviewStatus::Unsupported;
    EXPECT_TRUE(records[0].openable);
    EXPECT_EQ(search(records, "preview"), (std::vector<std::size_t>{0}));
}

TEST(WelcomeRecentModelTest, DoNotParseDisplayNamesAsMarkup)
{
    std::vector<RecentFileRecord> records = {makeRecord("<b>bold</b>.svg", "/home/Usuario/<b>bold</b>.svg")};

    EXPECT_EQ(records[0].display_name, "<b>bold</b>.svg");
    EXPECT_EQ(search(records, "<b>"), (std::vector<std::size_t>{0}));
    EXPECT_EQ(search(records, "BOLD"), (std::vector<std::size_t>{0}));
}

TEST(WelcomeRecentModelTest, MergeDedupsDisplayUriAndPrefersRecovery)
{
    using Inkscape::UI::Dialog::Welcome::mergeRecentRecords;

    auto plain = makeRecord("a.svg", "/proyectos/a/a.svg");
    auto auto_entry = makeRecord("a.svg", "/proyectos/a/a.svg");
    auto_entry.recovery = RecoveryKind::Auto;
    std::uint64_t id = 1;

    auto merged = mergeRecentRecords({plain}, {auto_entry}, id);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(merged[0].recovery, RecoveryKind::Auto);

    // Duplicate within one source list is collapsed too.
    std::uint64_t id2 = 100;
    auto merged2 = mergeRecentRecords({makeRecord("x.svg", "/x/x.svg"), makeRecord("x.svg", "/x/x.svg")}, {}, id2);
    EXPECT_EQ(merged2.size(), 1u);

    // Recovery wins regardless of which list it came from.
    auto crash_entry = makeRecord("y.svg", "/y/y.svg");
    crash_entry.recovery = RecoveryKind::Crash;
    std::uint64_t id3 = 200;
    auto merged3 = mergeRecentRecords({makeRecord("y.svg", "/y/y.svg")}, {crash_entry}, id3);
    ASSERT_EQ(merged3.size(), 1u);
    EXPECT_EQ(merged3[0].recovery, RecoveryKind::Crash);
}

TEST(WelcomeRecentModelTest, MergeOrdersByLastUsedDescending)
{
    using Inkscape::UI::Dialog::Welcome::mergeRecentRecords;

    auto older = makeRecord("old.svg", "/old.svg");
    older.last_used = utc(2024, 1, 1);
    auto newer = makeRecord("new.svg", "/new.svg");
    newer.last_used = utc(2024, 2, 1);
    auto middle = makeRecord("mid.svg", "/mid.svg");
    middle.last_used = utc(2024, 1, 15);

    std::uint64_t id = 1;
    auto merged = mergeRecentRecords({older, middle}, {newer}, id);
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_EQ(merged[0].display_name, "new.svg");
    EXPECT_EQ(merged[1].display_name, "mid.svg");
    EXPECT_EQ(merged[2].display_name, "old.svg");
}

TEST(WelcomeRecentModelTest, MergeRestoresLastUsedOrderAfterRecoveryDedup)
{
    using Inkscape::UI::Dialog::Welcome::mergeRecentRecords;

    // Single interaction regression. A recovery duplicate is OLDER than the
    // recovery-free entry it must replace, so deduping after the first sort would leave
    // the older recovery record at the newer entry's position. Dedup must keep the
    // complete recovery record, then the final list must be sorted by last_used again.
    auto normal_a = makeRecord("a.svg", "/proyectos/a/a.svg");
    normal_a.last_used = utc(2024, 3, 1);
    auto normal_b = makeRecord("b.svg", "/proyectos/b/b.svg");
    normal_b.last_used = utc(2024, 2, 1);
    auto normal_c = makeRecord("c.svg", "/proyectos/c/c.svg"); // no timestamp on purpose

    auto recovery_a = makeRecord("a-recovered.svg", "/proyectos/a/a.svg", "image/svg+xml",
                                 "file:///autosave/a-autosave.svg");
    recovery_a.last_used = utc(2024, 1, 1);
    recovery_a.recovery = RecoveryKind::Auto;
    recovery_a.recovery_original_uri = normal_a.uri;

    // Missing-timestamp duplicate: the same clear fix must still select the recovery
    // record exactly once and sort it last, without calling compare() on an invalid
    // Glib::DateTime.
    auto recovery_c = makeRecord("c-recovered.svg", "/proyectos/c/c.svg", "image/svg+xml",
                                 "file:///autosave/c-autosave.svg");
    recovery_c.recovery = RecoveryKind::Crash;
    recovery_c.recovery_original_uri = normal_c.uri;

    std::uint64_t id = 1;
    auto merged = mergeRecentRecords({normal_a, normal_b, normal_c}, {recovery_a, recovery_c}, id);

    ASSERT_EQ(merged.size(), 3u);

    // Independent expectation: B (Feb) first, then recovered A (Jan), then C (missing).
    EXPECT_EQ(merged[0].display_name, "b.svg");
    EXPECT_EQ(merged[1].display_name, "a-recovered.svg");
    EXPECT_EQ(merged[2].display_name, "c-recovered.svg");

    // The complete recovery records win; their stored URI and recovery data are preserved.
    EXPECT_EQ(merged[1].recovery, RecoveryKind::Auto);
    EXPECT_EQ(merged[1].uri, recovery_a.uri);
    EXPECT_EQ(merged[1].recovery_original_uri, normal_a.uri);
    EXPECT_EQ(merged[2].recovery, RecoveryKind::Crash);
    EXPECT_EQ(merged[2].uri, recovery_c.uri);
    EXPECT_EQ(merged[2].recovery_original_uri, normal_c.uri);

    // Last-used descending: B is strictly newer than the retained recovery A, A is valid,
    // and the missing-timestamp duplicate sorts last.
    EXPECT_TRUE(merged[0].last_used.compare(merged[1].last_used) > 0);
    EXPECT_TRUE(static_cast<bool>(merged[1].last_used));
    EXPECT_FALSE(static_cast<bool>(merged[2].last_used));
}

TEST(WelcomeRecentModelTest, MergeHandlesMissingTimestampWithoutGlibCritical)
{
    using Inkscape::UI::Dialog::Welcome::mergeRecentRecords;

    // Default-constructed Glib::DateTime has no instant. merge must not call
    // compare() on it (which would emit a GLib critical under G_DEBUG=fatal-criticals).
    auto missing = makeRecord("missing.svg", "/missing.svg");
    auto present = makeRecord("present.svg", "/present.svg");
    present.last_used = utc(2024, 3, 1);

    std::uint64_t id = 1;
    auto merged = mergeRecentRecords({missing}, {present}, id);
    ASSERT_EQ(merged.size(), 2u);
    EXPECT_EQ(merged[0].display_name, "present.svg");
    EXPECT_EQ(merged[1].display_name, "missing.svg");
}

TEST(WelcomeRecentModelTest, MergeAssignsOpaqueUniqueIds)
{
    using Inkscape::UI::Dialog::Welcome::mergeRecentRecords;

    std::uint64_t id = 7;
    auto merged = mergeRecentRecords(
        {makeRecord("a.svg", "/a.svg"), makeRecord("b.svg", "/b.svg")}, {makeRecord("c.svg", "/c.svg")}, id);

    ASSERT_EQ(merged.size(), 3u);
    std::set<std::string> ids;
    for (auto const &record : merged) {
        EXPECT_FALSE(record.id.empty());
        ids.insert(record.id);
    }
    EXPECT_EQ(ids.size(), 3u);
    EXPECT_GT(id, 7u);
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
