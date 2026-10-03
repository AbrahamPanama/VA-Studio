// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Metadata-only recent-file model for the Welcome screen grid.
 *
 * Every function below works on metadata already loaded into Gtk::RecentInfo, or on
 * caller-supplied records. No function probes the referenced documents, mounts a
 * share, resolves a symlink, reads bytes or asks Gio::File about them.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "welcome-recent-model.h"

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

#include <glib.h>

#include "io/recent-files.h"

namespace Inkscape::UI::Dialog::Welcome {
namespace {

// RAII for g_malloc-allocated strings (mirrors src/io/artwork-library-catalog.cpp).
using GlibOwnedString = std::unique_ptr<gchar, decltype(&g_free)>;

bool ascii_ends_with_ci(std::string_view value, std::string_view suffix)
{
    if (value.size() < suffix.size()) {
        return false;
    }
    auto const tail = value.substr(value.size() - suffix.size());
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (g_ascii_tolower(static_cast<guchar>(tail[i])) !=
            g_ascii_tolower(static_cast<guchar>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

bool ascii_equals_ci(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (g_ascii_tolower(static_cast<guchar>(a[i])) != g_ascii_tolower(static_cast<guchar>(b[i]))) {
            return false;
        }
    }
    return true;
}

RecentFileRecord recordFromRecentInfo(Gtk::RecentInfo const &info)
{
    RecentFileRecord record;
    record.uri = std::string(info.get_uri());
    record.display_name = std::string(info.get_display_name());
    record.display_location = std::string(info.get_uri_display());
    record.mime_type = std::string(info.get_mime_type());
    // Recent-manager metadata timestamp (last use), explicitly not a file mtime.
    record.last_used = info.get_modified();

    record.format = classifyRecentFormat(record.uri, record.mime_type);
    record.recovery = classifyRecoveryKind(info.has_group("Auto"), info.has_group("Crash"));
    if (record.recovery != RecoveryKind::None) {
        // addInkscapeRecentSvg() stores the original document URI in the description.
        record.recovery_original_uri = std::string(info.get_description());
    }

    // Preview state and open state are independent. No residency probe is allowed,
    // so every history entry remains openable regardless of preview eligibility.
    record.preview = record.format == RecentFormat::Svg ? PreviewStatus::NotRequested
                                                        : PreviewStatus::Unsupported;
    record.openable = true;
    return record;
}

} // namespace

std::string foldedSearchKey(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    // Invalid UTF-8 cannot be normalized; fall back to exact bytes so search never
    // throws on a UI-supplied string. Stored GTK metadata is valid UTF-8.
    if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
        return std::string(text);
    }

    GlibOwnedString normalized{g_utf8_normalize(text.data(), static_cast<gssize>(text.size()),
                                                G_NORMALIZE_DEFAULT_COMPOSE),
                               &g_free};
    if (!normalized) {
        return std::string(text);
    }
    GlibOwnedString folded{g_utf8_casefold(normalized.get(), -1), &g_free};
    if (!folded) {
        return std::string(normalized.get());
    }
    GlibOwnedString composed{g_utf8_normalize(folded.get(), -1, G_NORMALIZE_DEFAULT_COMPOSE), &g_free};
    if (!composed) {
        return std::string(folded.get());
    }
    return std::string(composed.get());
}

RecentFormat classifyRecentFormat(std::string_view uri, std::string_view mime_type)
{
    if (ascii_equals_ci(mime_type, "image/svg+xml")) {
        return RecentFormat::Svg;
    }

    // Some histories omit the SVG MIME type. A URI-extension fallback of .svg/.svgz
    // is still string-only classification, not a residency or existence check.
    auto const cut = uri.find_first_of("?#");
    auto const path = cut == std::string_view::npos ? uri : uri.substr(0, cut);
    if (ascii_ends_with_ci(path, ".svg") || ascii_ends_with_ci(path, ".svgz")) {
        return RecentFormat::Svg;
    }
    return RecentFormat::Unsupported;
}

RecoveryKind classifyRecoveryKind(bool has_auto_group, bool has_crash_group)
{
    if (has_auto_group) {
        return RecoveryKind::Auto;
    }
    if (has_crash_group) {
        return RecoveryKind::Crash;
    }
    return RecoveryKind::None;
}

bool recentRecordMatches(RecentFileRecord const &record, std::string_view folded_query)
{
    if (folded_query.empty()) {
        return true;
    }
    auto const name_key = foldedSearchKey(record.display_name);
    if (name_key.find(folded_query) != std::string::npos) {
        return true;
    }
    auto const location_key = foldedSearchKey(record.display_location);
    return location_key.find(folded_query) != std::string::npos;
}

std::vector<std::size_t> searchRecentRecords(std::vector<RecentFileRecord> const &records,
                                             Glib::ustring const &query)
{
    std::vector<std::size_t> matches;
    auto const folded_query = foldedSearchKey(std::string_view(query.raw()));

    if (folded_query.empty()) {
        matches.reserve(records.size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            matches.push_back(i);
        }
        return matches;
    }

    for (std::size_t i = 0; i < records.size(); ++i) {
        if (recentRecordMatches(records[i], folded_query)) {
            matches.push_back(i);
        }
    }
    return matches;
}

bool recentRecordsHaveRecovery(std::vector<RecentFileRecord> const &records)
{
    return std::any_of(records.begin(), records.end(), [](RecentFileRecord const &record) {
        return record.recovery != RecoveryKind::None;
    });
}

std::vector<RecentFileRecord> mergeRecentRecords(std::vector<RecentFileRecord> normal,
                                                 std::vector<RecentFileRecord> autos,
                                                 std::uint64_t &next_id)
{
    std::vector<RecentFileRecord> combined;
    combined.reserve(normal.size() + autos.size());
    for (auto &record : normal) {
        combined.push_back(std::move(record));
    }
    for (auto &record : autos) {
        combined.push_back(std::move(record));
    }

    // Stable recent order: most recently used first. std::stable_sort keeps the
    // source (normal-before-auto, then per-list) order for equal timestamps. Guard
    // invalid timestamps so compare() never emits a GLib critical; a missing
    // timestamp sorts after any real one.
    auto more_recent_first = [](RecentFileRecord const &a, RecentFileRecord const &b) {
        bool const a_valid = static_cast<bool>(a.last_used);
        bool const b_valid = static_cast<bool>(b.last_used);
        if (a_valid && b_valid) {
            return a.last_used.compare(b.last_used) > 0;
        }
        return a_valid && !b_valid;
    };
    std::stable_sort(combined.begin(), combined.end(), more_recent_first);

    std::vector<RecentFileRecord> result;
    result.reserve(combined.size());
    std::map<std::string, std::size_t> by_location;
    for (auto &record : combined) {
        auto key = record.display_location.empty() ? record.uri : record.display_location;
        auto const found = by_location.find(key);
        if (found == by_location.end()) {
            by_location.emplace(std::move(key), result.size());
            result.push_back(std::move(record));
        } else if (result[found->second].recovery == RecoveryKind::None &&
                   record.recovery != RecoveryKind::None) {
            // Same display URI already present; keep the recovery-bearing status. The
            // complete recovery record (its stored uri and recovery data) replaces the
            // recovery-free duplicate.
            result[found->second] = std::move(record);
        }
    }

    // The replacement above can place an older recovery record into the slot of the
    // newer recovery-free record it replaced, so re-establish last-used descending over
    // the final deduped list. Stable keeps normal-before-auto order for equal timestamps.
    std::stable_sort(result.begin(), result.end(), more_recent_first);

    for (auto &record : result) {
        record.id = "welcome-recent-" + std::to_string(next_id++);
    }
    return result;
}

std::vector<RecentFileRecord> snapshotRecentModel(unsigned max_files)
{
    auto const normal_infos = Inkscape::IO::getInkscapeRecentFiles(0, false);
    auto const auto_infos = Inkscape::IO::getInkscapeRecentFiles(0, true);

    std::vector<RecentFileRecord> normal;
    normal.reserve(normal_infos.size());
    for (auto const &info : normal_infos) {
        normal.push_back(recordFromRecentInfo(*info));
    }

    std::vector<RecentFileRecord> autos;
    autos.reserve(auto_infos.size());
    for (auto const &info : auto_infos) {
        autos.push_back(recordFromRecentInfo(*info));
    }

    // Opaque id source. snapshotRecentModel is single-owner: call it only on the UI
    // thread that owns the model. This plain counter keeps ids monotonic across calls; it
    // is intentionally not thread-safe and grants no cross-thread unique-id allocation.
    static std::uint64_t next_id = 1;
    auto model = mergeRecentRecords(std::move(normal), std::move(autos), next_id);
    // mergeRecentRecords advances next_id past every id it handed out.

    if (max_files != 0 && model.size() > max_files) {
        model.resize(max_files);
    }
    return model;
}

} // namespace Inkscape::UI::Dialog::Welcome

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
