// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Auto-save
 *
 * Copyright (C) 2020 Tavmjong Bah
 *
 * Re-write of code formerly in inkscape.cpp and originally written by Jon Cruz and others.
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <giomm/file.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <sigc++/scoped_connection.h>

#include "auto-save.h"
#include "ui/explode-bitmap-publication.h"
#include "document-undo.h"
#include "document.h"
#include "gc-anchored.h"
#include "inkscape-application.h"
#include "io/document-file-transaction.h"
#include "io/recent-files.h"
#include "preferences.h"
#include "xml/document.h"
#include "xml/repr.h"

namespace {

// duplicate() returns a GC-anchored XML::Document with its own anchor
// reference; release that anchor through the GC exactly once.
void release_xml_document(Inkscape::XML::Document *document)
{
    if (document) {
        Inkscape::GC::release(document);
    }
}

char const *failure_kind_name(Inkscape::IO::DocumentTransaction::FailureKind kind)
{
    using Inkscape::IO::DocumentTransaction::FailureKind;
    switch (kind) {
    case FailureKind::None: return "None";
    case FailureKind::InvalidArgument: return "InvalidArgument";
    case FailureKind::Unsupported: return "Unsupported";
    case FailureKind::InvalidOperation: return "InvalidOperation";
    case FailureKind::Aborted: return "Aborted";
    case FailureKind::DestinationExists: return "DestinationExists";
    case FailureKind::PermissionDenied: return "PermissionDenied";
    case FailureKind::ReadOnly: return "ReadOnly";
    case FailureKind::NotFound: return "NotFound";
    case FailureKind::SharingViolation: return "SharingViolation";
    case FailureKind::StagingCreateFailed: return "StagingCreateFailed";
    case FailureKind::StagingWriteFailed: return "StagingWriteFailed";
    case FailureKind::StagingFlushFailed: return "StagingFlushFailed";
    case FailureKind::StagingSyncFailed: return "StagingSyncFailed";
    case FailureKind::StagingCloseFailed: return "StagingCloseFailed";
    case FailureKind::PublicationFailed: return "PublicationFailed";
    case FailureKind::PublicationUncertain: return "PublicationUncertain";
    }
    return "Unknown";
}

char const *staging_availability_name(Inkscape::IO::DocumentTransaction::StagingAvailability availability)
{
    using Inkscape::IO::DocumentTransaction::StagingAvailability;
    switch (availability) {
    case StagingAvailability::Unverified: return "Unverified";
    case StagingAvailability::Available: return "Available";
    case StagingAvailability::Missing: return "Missing";
    case StagingAvailability::Changed: return "Changed";
    }
    return "Unknown";
}

} // namespace

namespace Inkscape {

void
AutoSave::init(InkscapeApplication* app)
{
    _app = app;
    start();
}

void
AutoSave::start()
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    static sigc::scoped_connection autosave_connection;

    // Turn off any previous timeout.
    autosave_connection.disconnect();

    if (prefs->getBool("/options/autosave/enable", true)) {
        // Turn on autosave (timeout is in seconds).
        guint32 timeout = std::max(prefs->getInt("/options/autosave/interval", 10), 1) * 60;
        if (timeout > 60 * 60 * 24) {
            // Sanity check
            std::cerr << "AutoSave::start: auto-save interval set to greater than one day. Not enabling." << std::endl;
            return;
        }
        // Explicit no-argument member binding: the injected overload above would
        // otherwise make &AutoSave::save ambiguous for sigc::mem_fun.
        autosave_connection = Glib::signal_timeout().connect_seconds(
            sigc::mem_fun(*this, static_cast<bool (AutoSave::*)()>(&AutoSave::save)), timeout);
    }
}

bool
AutoSave::save()
{
    // Cheap pre-admission before building the platform call set: a nested tick
    // or an uninitialized app never allocates.
    if (_saving || !_app) {
        return true;
    }
    // The factory is the only step outside the injected-overload guard; any
    // exception it raises must not reach the timer.
    try {
        auto calls = IO::DocumentTransaction::make_platform_system_calls();
        if (!calls) {
            return true;
        }
        return save(*calls);
    } catch (std::exception const &e) {
        g_warning("AutoSave: platform system-call setup failed: %s", e.what() ? e.what() : "unknown");
    } catch (...) {
        g_warning("AutoSave: platform system-call setup failed.");
    }
    return true;
}

bool
AutoSave::save(IO::DocumentTransaction::SystemCalls &calls)
{
    using IO::DocumentTransaction::FailureKind;
    using IO::DocumentTransaction::LogicalTarget;
    using IO::DocumentTransaction::NewDocumentFile;
    using IO::DocumentTransaction::PublicationStatus;

    // Re-entry guard: the whole real tick is single-flight. RAII resets the
    // scope on every return and exception.
    if (_saving) {
        return true;
    }
    struct TickGuard {
        bool &flag;
        explicit TickGuard(bool &f) : flag(f) { flag = true; }
        ~TickGuard() { flag = false; }
        TickGuard(TickGuard const &) = delete;
        TickGuard &operator=(TickGuard const &) = delete;
    } tick_guard(_saving);

    try {
        if (!_app) {
            return true;
        }

        // Gather only live serials at this synchronous boundary. Each iteration
        // re-resolves against a fresh document list before deref so a callback
        // that closes another document cannot leave a stale raw pointer.
        std::vector<unsigned long> serials;
        for (SPDocument *document : _app->get_documents()) {
            if (document) {
                serials.push_back(document->serial());
            }
        }
        if (serials.empty()) {
            return true;
        }

        // One lazily generated session token for every recovery basename in
        // this process, freed through a g_free RAII owner.
        if (_session.empty()) {
            gchar *raw = g_uuid_string_random();
            if (raw) {
                std::unique_ptr<gchar, decltype(&g_free)> owned(raw, &g_free);
                _session.assign(owned.get());
            }
        }
        if (_session.empty()) {
            g_warning("AutoSave: could not create a session token; skipping this tick.");
            return true;
        }

        // The cwd is captured once and reused for every normalization below.
        std::string const cwd = Glib::get_current_dir();

        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        std::string autosave_dir = prefs->getString("/options/autosave/path");
        if (autosave_dir.empty()) {
            autosave_dir = Glib::build_filename(Glib::get_user_cache_dir(), "inkscape");
        }
        if (!Glib::path_is_absolute(autosave_dir)) {
            autosave_dir = Glib::build_filename(cwd, autosave_dir);
        }

        // Each iteration re-resolves and owns its document locally; directory
        // creation happens inside the per-doc try below.
        for (unsigned long const serial : serials) {
            try {
                // Fresh membership resolution at the ready boundary: only the
                // serial crossed the callback-capable gap, never a raw pointer.
                SPDocument *document = nullptr;
                for (SPDocument *candidate : _app->get_documents()) {
                    if (candidate && candidate->serial() == serial) {
                        document = candidate;
                        break;
                    }
                }
                if (!document) {
                    continue;
                }

                // Skip unmodified, non-fresh/busy/closing-running documents and
                // any live rollbackable interaction.
                if (!document->isModifiedSinceAutoSave()) {
                    continue;
                }
                // Re-run a native tick after settlement; never retain injected calls.
                if (Bitmap::deferPublicationBoundary(document,[](SPDocument &) {
                    AutoSave::getInstance().save();
                })) continue;
                if (!Inkscape::DocumentUndo::fileOperationFreshReady(document)) {
                    continue;
                }

                // Exactly one own operation lease, held through snapshot, IO,
                // bookkeeping and snapshot destruction.
                auto lease = Inkscape::DocumentUndo::holdInteractionOperation(document);
                if (!lease) {
                    continue;
                }

                // Immediately re-check readiness under the lease, before any
                // capture or duplicate work.
                if (!Inkscape::DocumentUndo::fileOperationOutputReady(document)) {
                    continue;
                }

                // Consume one generation per attempt. Never wrap on saturation:
                // skip and log, keeping the timer alive.
                if (_generation == std::numeric_limits<std::uint64_t>::max()) {
                    g_warning("AutoSave: generation exhausted; skipping recovery for document serial %lu.", serial);
                    continue;
                }
                std::uint64_t const generation = _generation++;

                // Capture owned metadata and the live XML revision before the
                // duplicate. No borrowed metadata is read after a callback and
                // the snapshot counter is never compared to the live one.
                std::string const original_filename =
                    document->getDocumentFilename() ? document->getDocumentFilename() : std::string();
                std::string const original_base =
                    document->getDocumentBase() ? document->getDocumentBase() : std::string();
                std::string const original_name =
                    document->getDocumentName() ? document->getDocumentName() : std::string();
                // One live XML identity and its revision, captured once under
                // the lease. No new clone/anchor on the live document:
                // SPDocument keeps rdoc stable and rebase mutates it in place.
                Inkscape::XML::Document *live_xml = document->getReprDoc();
                if (!live_xml) {
                    g_warning("AutoSave: document serial %lu has no live XML; skipping.", serial);
                    continue;
                }
                std::optional<std::uint64_t> const live_revision = live_xml->contentRevision();

                // EXACT NATIVE XML ONLY: a private GC-anchored duplicate. No
                // SPDocument copy, SavePreparation, resource reload, clone or
                // parser, so no metadata defaults/original-document pointer.
                Inkscape::XML::Document *snapshot_raw = live_xml->duplicate(nullptr);
                if (!snapshot_raw) {
                    g_warning("AutoSave: could not snapshot document serial %lu.", serial);
                    continue;
                }
                // Declared after `lease`: reverse destruction releases the
                // snapshot first so the lease outlives snapshot destruction.
                std::unique_ptr<Inkscape::XML::Document, void (*)(Inkscape::XML::Document *)> snapshot(
                    snapshot_raw, &release_xml_document);

                // Per-document directory query/create, using the captured
                // normalized autosave_dir. A failure logs and continues with the
                // next serial; nothing is pruned or deleted.
                Glib::RefPtr<Gio::File> dir_file = Gio::File::create_for_path(autosave_dir);
                if (!dir_file->query_exists()) {
                    if (!dir_file->make_directory_with_parents()) {
                        g_warning("AutoSave: failed to create recovery directory: %s", autosave_dir.c_str());
                        continue;
                    }
                }

                // Normalize the old base to absolute using the captured cwd.
                std::string old_abs_base = original_base;
                if (old_abs_base.empty()) {
                    old_abs_base = cwd;
                } else if (!Glib::path_is_absolute(old_abs_base)) {
                    old_abs_base = Glib::build_filename(cwd, old_abs_base);
                }
                // New base is the logical absolute recovery parent, never the
                // random staging basename.
                std::string const &new_abs_base = autosave_dir;

                std::string const final_name = "autosave-" + _session + "-" + std::to_string(serial) + "-" +
                                               std::to_string(generation) + ".svg";
                std::string const final_path = Glib::build_filename(autosave_dir, final_name);

                // Readiness before touching the output path.
                if (!Inkscape::DocumentUndo::fileOperationOutputReady(document)) {
                    continue;
                }

                LogicalTarget target{final_path, final_name, autosave_dir};
                FailureKind create_failure = FailureKind::None;
                std::string create_error;
                auto newfile = NewDocumentFile::create(std::move(target), calls, create_failure, create_error);
                if (!newfile) {
                    g_warning("AutoSave: could not create recovery file %s (%s): %s",
                              final_path.c_str(), failure_kind_name(create_failure), create_error.c_str());
                    continue;
                }

                // create() may have run callbacks; re-check readiness before
                // writing and abandon the staging sibling when not ready.
                if (!Inkscape::DocumentUndo::fileOperationOutputReady(document)) {
                    newfile->abort();
                    continue;
                }

                // The writer only borrows the staged stream: no fclose/fsync.
                FailureKind write_failure = FailureKind::None;
                std::string write_error;
                bool const wrote = newfile->write(
                    [&](FILE *stream) {
                        sp_repr_save_stream(snapshot.get(), stream, SP_SVG_NS_URI, false,
                                            old_abs_base.c_str(), new_abs_base.c_str());
                    },
                    write_failure, write_error);
                if (!wrote) {
                    g_warning("AutoSave: could not write recovery file %s (%s): %s",
                              final_path.c_str(), failure_kind_name(write_failure), write_error.c_str());
                    newfile->abort();
                    continue;
                }

                // Serialization may have invoked callbacks; re-check before seal.
                if (!Inkscape::DocumentUndo::fileOperationOutputReady(document)) {
                    newfile->abort();
                    continue;
                }

                FailureKind seal_failure = FailureKind::None;
                std::string seal_error;
                if (!newfile->seal(seal_failure, seal_error)) {
                    g_warning("AutoSave: could not seal recovery file %s (%s): %s",
                              final_path.c_str(), failure_kind_name(seal_failure), seal_error.c_str());
                    newfile->abort();
                    continue;
                }

                // Re-check readiness before publication.
                if (!Inkscape::DocumentUndo::fileOperationOutputReady(document)) {
                    newfile->abort();
                    continue;
                }

                auto const result = newfile->publish();

                // One bounded diagnostic per attempt, reporting the recorded F3
                // result fields verbatim for healthy and limited results. A sync
                // capability gap is never interpreted as a failed publication,
                // and Published is never presented as full durability.
                g_message("AutoSave: recovery publication %s: status=%s failure=%s error=%s "
                          "file_sync(attempted=%s supported=%s ok=%s) "
                          "parent_sync(attempted=%s supported=%s ok=%s) staging_cleanup_ok=%s",
                          final_path.c_str(), to_string(result.status),
                          failure_kind_name(result.failure), result.error.c_str(),
                          result.file_sync_attempted ? "true" : "false",
                          result.file_sync_supported ? "true" : "false",
                          result.file_sync_ok ? "true" : "false",
                          result.parent_sync_attempted ? "true" : "false",
                          result.parent_sync_supported ? "true" : "false",
                          result.parent_sync_ok ? "true" : "false",
                          result.staging_cleanup_ok ? "true" : "false");

                if (result.status != PublicationStatus::Published) {
                    g_warning("AutoSave: recovery publication for %s not confirmed: status=%s failure=%s error=%s",
                              final_path.c_str(), to_string(result.status),
                              failure_kind_name(result.failure), result.error.c_str());
                    if (result.status == PublicationStatus::Uncertain) {
                        // Availability is an identity signal only, never proof
                        // of the retained bytes.
                        g_warning("AutoSave: recovery publication uncertain; staging %s availability=%s retained=%s (not verified bytes)",
                                  result.recovery_path.c_str(),
                                  staging_availability_name(result.staging_availability),
                                  result.recovery_retained ? "true" : "false");
                    }
                    continue;
                }

                // ONLY a published recovery may add the captured original
                // metadata to Recent, and a Recent failure never downgrades it.
                try {
                    Inkscape::IO::addInkscapeRecentSvg(final_path,
                                                       original_name.empty() ? "unnamed" : original_name,
                                                       {"Auto"}, original_filename);
                } catch (std::exception const &e) {
                    g_warning("AutoSave: recovery published but Recent update failed for %s: %s",
                              final_path.c_str(), e.what() ? e.what() : "unknown");
                } catch (...) {
                    g_warning("AutoSave: recovery published but Recent update failed for %s.", final_path.c_str());
                }

                // Retention (/options/autosave/max): only after this generation
                // is confirmed Published, drop THIS session's older generations
                // of this serial beyond the limit, and their Recent entries. The
                // newest recovery is always kept (limit is at least 1); files
                // that fail to delete stay tracked for the next tick.
                try {
                    auto &published = _published[serial];
                    published.push_back(final_path);
                    int const configured = prefs->getInt("/options/autosave/max", 50);
                    std::size_t const limit = static_cast<std::size_t>(std::max(configured, 1));
                    // Never trade an older recovery for a new one that may not have reached
                    // the disk: prune only when no supported file/directory sync was attempted and
                    // failed. A platform that cannot sync (unsupported) is not a failure.
                    bool const durable =
                        !(result.file_sync_attempted && result.file_sync_supported && !result.file_sync_ok) &&
                        !(result.parent_sync_attempted && result.parent_sync_supported && !result.parent_sync_ok);
                    while (durable && published.size() > limit) {
                        std::string const old_path = published.front();
                        if (old_path == final_path) {
                            break;
                        }
                        if (g_remove(old_path.c_str()) != 0 && g_file_test(old_path.c_str(), G_FILE_TEST_EXISTS)) {
                            g_warning("AutoSave: could not remove old recovery %s; keeping it for a later tick.",
                                      old_path.c_str());
                            break;
                        }
                        Inkscape::IO::removeInkscapeRecent(old_path);
                        published.erase(published.begin());
                    }
                } catch (std::exception const &e) {
                    g_warning("AutoSave: recovery retention failed for document serial %lu: %s", serial,
                              e.what() ? e.what() : "unknown");
                } catch (...) {
                    g_warning("AutoSave: recovery retention failed for document serial %lu.", serial);
                }

                // Clear dirty only for the same live XML that was snapshotted,
                // after callback-capable bookkeeping, under the lease and after
                // a readiness/owner re-check. Empty/saturated or changed stays
                // dirty; modified_since_save/Undo/URI/name/base/viewstate are
                // never touched.
                if (live_revision.has_value() && document->getReprDoc() == live_xml
                    && live_xml->contentRevision() == live_revision
                    && Inkscape::DocumentUndo::fileOperationOutputReady(document)) {
                    document->setModifiedSinceAutoSaveFalse();
                }
            } catch (std::exception const &e) {
                g_warning("AutoSave: recovery attempt for document serial %lu failed: %s",
                          serial, e.what() ? e.what() : "unknown");
            } catch (...) {
                g_warning("AutoSave: recovery attempt for document serial %lu failed.", serial);
            }
        }
    } catch (std::exception const &e) {
        // Bounded unhandled setup failure: never silently swallowed, and the
        // timer tick still never propagates.
        g_warning("AutoSave: unhandled autosave setup failure: %s", e.what() ? e.what() : "unknown");
    } catch (...) {
        g_warning("AutoSave: unhandled autosave setup failure.");
    }

    return true;
}

void
AutoSave::restart()
{
    AutoSave::getInstance().start();
}

} // namespace Inkscape

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
