// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Metadata-only recent-file model for the Welcome screen grid.
 *
 * This model owns no GTK widgets, no GObject lifecycle and no file handles. It is
 * a plain immutable snapshot of already-loaded Gtk::RecentInfo metadata, plus pure
 * filter/search helpers. Consumers must never probe the referenced documents
 * (stat/exists/symlink/read/residency/Gio::File); only data already present on the
 * recent-history entry is used.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_DIALOG_WELCOME_RECENT_MODEL_H
#define INKSCAPE_UI_DIALOG_WELCOME_RECENT_MODEL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glibmm/datetime.h>
#include <glibmm/ustring.h>

namespace Inkscape::UI::Dialog::Welcome {

/**
 * URI/MIME-based preview eligibility only.
 *
 * Svg means the metadata identifies an SVG/SVGZ document that a preview producer
 * may attempt. Unsupported means no known preview producer for the format. This is
 * deliberately NOT a statement about residency, existence or missing state: a
 * Missing/offline document is never inferred from the URI here.
 */
enum class RecentFormat {
    Svg,
    Unsupported,
};

/** Recovery classification from the recent-history groups. */
enum class RecoveryKind {
    None,
    Crash,
    Auto,
};

/**
 * Independent preview state. It never gates openability and is never derived from
 * filesystem/residency checks. Unsupported is set for non-SVG formats whose bytes
 * this model does not inspect.
 */
enum class PreviewStatus {
    NotRequested,
    Loading,
    Ready,
    Cached,
    Unsupported,
    Unavailable,
    Failed,
};

/**
 * One immutable recent-history entry.
 *
 * `last_used` is the recent-manager metadata timestamp (Gtk::RecentInfo::get_modified,
 * i.e. last use). It is explicitly not a document filesystem mtime and must not be
 * labelled as one.
 */
struct RecentFileRecord {
    std::string id;                    // Opaque runtime identity; unique within a snapshot.
    std::string uri;                   // Original URI exactly as stored; never rewritten.
    std::string display_name;          // Stored display name; never parsed as markup.
    std::string display_location;      // Displayable URI/path (Gtk::RecentInfo::get_uri_display).
    Glib::DateTime last_used;          // Recent-manager last-use metadata timestamp.
    std::string mime_type;             // Stored MIME type.
    RecentFormat format = RecentFormat::Unsupported;
    RecoveryKind recovery = RecoveryKind::None;
    std::string recovery_original_uri; // Gtk::RecentInfo::get_description(); empty unless recovery != None.
    PreviewStatus preview = PreviewStatus::NotRequested;
    // Open state is independent of preview. Every history entry stays openable; this
    // is never derived from URI/MIME/residency and never cleared by a preview failure.
    bool openable = true;
};

/**
 * Unicode search key: validate, NFC-normalize, Unicode-casefold, NFC-normalize again.
 * Invalid UTF-8 falls back to the exact input bytes so search never throws in a UI path.
 */
std::string foldedSearchKey(std::string_view text);

/**
 * Classify preview format from metadata strings only. Never touches the filesystem.
 * Returns Svg for the SVG MIME type or, when the MIME is absent/unknown, for a
 * `.svg`/`.svgz` URI path (case-insensitive; query and fragment are ignored).
 */
RecentFormat classifyRecentFormat(std::string_view uri, std::string_view mime_type);

/** Auto takes precedence over Crash; both true is not expected but is defined. */
RecoveryKind classifyRecoveryKind(bool has_auto_group, bool has_crash_group);

/** True when the record's stored name or location contains the pre-folded query. */
bool recentRecordMatches(RecentFileRecord const &record, std::string_view folded_query);

/**
 * Return indices of matching records in their existing (stable recent) order.
 * An empty query returns every index in order; no match returns an empty vector.
 * Performs no filesystem action and does not reorder records.
 */
std::vector<std::size_t> searchRecentRecords(std::vector<RecentFileRecord> const &records,
                                             Glib::ustring const &query);

/** True when any entry carries Crash or Auto recovery status. */
bool recentRecordsHaveRecovery(std::vector<RecentFileRecord> const &records);

/**
 * Pure combine step for the two source lists (default and Auto group).
 *
 * Deduplicates by display_location (falling back to uri), letting an entry with recovery
 * status win over a recovery-free duplicate, then stable-sorts the final deduped list by
 * last_used descending (a recovery entry may be older than the recovery-free entry it
 * replaces, so the order must be re-established after dedup), then assigns opaque ids
 * from `next_id`. Pure: no GTK, no I/O.
 */
std::vector<RecentFileRecord> mergeRecentRecords(std::vector<RecentFileRecord> normal,
                                                 std::vector<RecentFileRecord> autos,
                                                 std::uint64_t &next_id);

/**
 * Live snapshot adapter. The only function here that reads recent history, through
 * Inkscape::IO::getInkscapeRecentFiles() with its existing application-ID filtering,
 * display-URI dedup and modified-desc ordering. Fetches the Auto group separately and
 * combines via mergeRecentRecords without duplicate display URIs. `max_files == 0`
 * keeps all entries. Never probes the referenced documents.
 *
 * Thread affinity: must be called on the single UI thread that owns the returned model.
 * It is not thread-safe and makes no cross-thread unique-id promise; the id counter is a
 * plain process-local monotonic value, not an atomic allocation.
 */
std::vector<RecentFileRecord> snapshotRecentModel(unsigned max_files = 0);

} // namespace Inkscape::UI::Dialog::Welcome

#endif // INKSCAPE_UI_DIALOG_WELCOME_RECENT_MODEL_H

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
