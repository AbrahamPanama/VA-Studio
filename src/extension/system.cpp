// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * This is file is kind of the junk file.  Basically everything that
 * didn't fit in one of the other well defined areas, well, it's now
 * here.  Which is good in someways, but this file really needs some
 * definition.  Hopefully that will come ASAP.
 *
 * Authors:
 *   Ted Gould <ted@gould.cx>
 *   Johan Engelen <johan@shouraizou.nl>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2006-2007 Johan Engelen
 * Copyright (C) 2002-2004 Ted Gould
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "system.h"

#include <cerrno>

#include <glib/gstdio.h>
#include <glibmm/miscutils.h>

#include "db.h"
#include "document.h"
#include "document-undo.h"
#include "effect.h"
#include "event-log.h"
#include "extension.h"
#include "implementation/script.h"
#include "implementation/xslt.h"
#include "inkscape.h"
#include "input.h"
#include "io/sys.h"
#include "io/stream/bufferstream.h"
#include "loader.h"
#include "output.h"
#include "internal/svg-publication.h"
#include "patheffect.h"
#include "preferences.h"
#include "print.h"
#include "template.h"
#include "ui/interface.h"
#include "xml/rebase-hrefs.h"
#include "xml/document.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h defines legacy macros that collide with gtkmm declarations.
#undef near
#undef IGNORE
#endif

namespace Inkscape::Extension {

/**
 * \return   A new document created from the filename passed in
 * \brief    This is a generic function to use the open function of
 *           a module (including Autodetect)
 * \param    key       Identifier of which module to use
 * \param    filename  The file that should be opened
 * \param    is_importing Is the request an import request, for example drag & drop
 *
 * First things first, are we looking at an autodetection?  Well if that's the case then the module
 * needs to be found, and that is done with a database lookup through the module DB.  The foreach
 * function is called, with the parameter being a gpointer array.  It contains both the filename
 * (to find its extension) and where to write the module when it is found.
 *
 * If there is no autodetection, then the module database is queried with the key given.
 *
 * If everything is cool at this point, the module is loaded, and there is possibility for
 * preferences.  If there is a function, then it is executed to get the dialog to be displayed.
 * After it is finished the function continues.
 *
 * Lastly, the open function is called in the module itself.
 */
std::unique_ptr<SPDocument> open(Extension *key, char const *filename, bool is_importing)
{
    Input *imod = dynamic_cast<Input *>(key ? key : Input::find_by_filename(filename));

    bool last_chance_svg = false;
    if (!key && !imod) {
        last_chance_svg = true;
        imod = dynamic_cast<Input *>(db.get(SP_MODULE_KEY_INPUT_SVG));
    }

    if (!imod) {
        throw Input::no_extension_found();
    }

    // Hide pixbuf extensions depending on user preferences.
    //g_warning("Extension: %s", imod->get_id());

    bool show = true;
    if (std::strlen(imod->get_id()) > 21) {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        bool ask = prefs->getBool("/dialogs/import/ask");
        bool ask_svg = prefs->getBool("/dialogs/import/ask_svg");
        Glib::ustring id = Glib::ustring(imod->get_id(), 22);
        if (id.compare("org.inkscape.input.svg") == 0) {
            if (ask_svg && is_importing) {
                show = true;
                imod->set_gui(true);
            } else {
                show = false;
                imod->set_gui(false);
            }
        } else if(strlen(imod->get_id()) > 27) {
            id = Glib::ustring(imod->get_id(), 28);
            if (!ask && id.compare( "org.inkscape.input.gdkpixbuf") == 0) {
                show = false;
                imod->set_gui(false);
            }
        }
    }
    imod->set_state(Extension::STATE_LOADED);

    if (!imod->loaded()) {
        throw Input::open_failed();
    }

    if (!imod->prefs()) {
        throw Input::open_cancelled();
    }

    auto doc = imod->open(filename, is_importing);

    if (!doc) {
        if (last_chance_svg) {
            if ( INKSCAPE.use_gui() ) {
                sp_ui_error_dialog(_("Could not detect file format. Tried to open it as an SVG anyway but this also failed."));
            } else {
                g_warning("%s", _("Could not detect file format. Tried to open it as an SVG anyway but this also failed."));
            }
        }
        throw Input::open_failed();
    }
    // If last_chance_svg is true here, it means we successfully opened a file as an svg
    // and there's no need to warn the user about it, just do it.

    doc->setDocumentFilename(filename);
    if (!show) {
        imod->set_gui(true);
    }

    return doc;
}

/**
 * Prove, before an official publication runs, that its target is definitely a
 * different physical file from the document's current official saved path.
 *
 * Returns true only when distinctness is proven; every uncertainty (missing or
 * unidentifiable saved path, unknown target identity, zero device/inode values
 * on platforms where the identity is not reliable) returns false so the caller
 * conservatively treats the target as a possible alias of the official file.
 *
 * g_stat follows symlinks, so a symlink or hard link onto the official file is
 * correctly seen as the same file rather than a distinct lexical path. This is
 * a read-only probe; it does not mutate the filesystem.
 */
static bool
publicationTargetDefinitelyDistinctFromSavedPath(char const *target, std::string const &saved_path)
{
    if (!target || target[0] == '\0' || saved_path.empty()) {
        return false;
    }

    GStatBuf saved_stat{};
    if (g_stat(saved_path.c_str(), &saved_stat) != 0) {
        // The official saved path must exist and be identifiable for its Undo
        // anchor to be meaningful; without it we cannot prove distinctness.
        return false;
    }

    GStatBuf target_stat{};
    if (g_stat(target, &target_stat) != 0) {
        // An absent target cannot be the same physical file as the existing
        // saved file. Any other stat error leaves the identity unknown.
        return errno == ENOENT;
    }

    // Both paths resolve to something that exists. Windows GLib stat does not
    // reliably supply an inode, so ask the OS for both file identities.
#ifdef _WIN32
    auto identity = [](char const *path, BY_HANDLE_FILE_INFORMATION &info) {
        auto *wide = g_utf8_to_utf16(path, -1, nullptr, nullptr, nullptr);
        if (!wide) return false;
        HANDLE const handle = CreateFileW(reinterpret_cast<wchar_t const *>(wide),
                                           FILE_READ_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        g_free(wide);
        if (handle == INVALID_HANDLE_VALUE) return false;
        bool const ok = GetFileInformationByHandle(handle, &info) != FALSE;
        CloseHandle(handle);
        return ok;
    };
    BY_HANDLE_FILE_INFORMATION saved_info{}, target_info{};
    if (!identity(saved_path.c_str(), saved_info) || !identity(target, target_info)) return false;
    if (saved_info.dwVolumeSerialNumber == 0 || target_info.dwVolumeSerialNumber == 0
        || (saved_info.nFileIndexHigh == 0 && saved_info.nFileIndexLow == 0)
        || (target_info.nFileIndexHigh == 0 && target_info.nFileIndexLow == 0)) return false;
    return saved_info.dwVolumeSerialNumber != target_info.dwVolumeSerialNumber
        || saved_info.nFileIndexHigh != target_info.nFileIndexHigh
        || saved_info.nFileIndexLow != target_info.nFileIndexLow;
#else
    // Only a reliable nonzero
    // device/inode pair can establish difference; zero means the platform did
    // not provide a usable identity (notably some Windows configurations).
    if (saved_stat.st_dev == 0 || saved_stat.st_ino == 0
        || target_stat.st_dev == 0 || target_stat.st_ino == 0) {
        return false;
    }

    return saved_stat.st_dev != target_stat.st_dev || saved_stat.st_ino != target_stat.st_ino;
#endif
}

struct SaveCompletionContext {
    SPDocument *doc;
    std::weak_ptr<void> lifecycle;
    std::uint64_t instance_generation;
    std::uint64_t attempt_generation;
    std::uint64_t undo_serial;
    Output *output;
    std::string filename;
    SavePreparation preparation;
    FileSaveMethod save_method;
    bool official;
    std::optional<uint64_t> saved_revision;
    std::optional<uint64_t> save_invalidation;
    std::string prior_saved_path;
    bool target_definitely_distinct;
    bool may_alias_official;
    std::string notice;
    bool handled = false;
};

static void complete(Internal::PublicationResult const &result, SaveCompletionContext &context)
{
    if (!result.route.empty()) g_debug("VACARDS_SAVE_ROUTE=%s", result.route.c_str());
    context.notice += result.notice;
    // These outcomes prove that no bytes were published. Preserve their typed
    // refusal even if a nested callback superseded the save attempt.
    if (result.outcome == Internal::PublicationOutcome::ReadOnly
        || result.outcome == Internal::PublicationOutcome::Conflict
        || result.outcome == Internal::PublicationOutcome::Unsupported) {
        context.handled = true;
        if (result.outcome == Internal::PublicationOutcome::ReadOnly) throw Output::file_read_only();
        if (result.outcome == Internal::PublicationOutcome::Conflict) throw Output::save_conflict(result.error);
        throw Output::save_unsupported(result.error);
    }
    // The token expires before destruction can expose a reused address. Rebase
    // retains the object but changes its instance generation.
    auto stale = [&](char const *reason) {
        bool const may_have_published = result.outcome == Internal::PublicationOutcome::Published
            || result.outcome == Internal::PublicationOutcome::Uncertain
            || result.outcome == Internal::PublicationOutcome::Failed;
        if (may_have_published && !context.lifecycle.expired()
            && context.doc->saveInstanceGeneration() == context.instance_generation
            && (context.official || context.may_alias_official)) {
            context.doc->setModifiedSinceSave(true);
            DocumentUndo::markInteractionBaselineDirty(context.doc);
            if (!context.target_definitely_distinct ||
                std::string(context.doc->getDocumentFilename() ? context.doc->getDocumentFilename() : "")
                    != context.prior_saved_path)
                context.doc->get_event_log()->invalidateFileSave();
        }
        context.handled = true;
        if (result.outcome == Internal::PublicationOutcome::Published)
            throw PublishedStaleDocument(reason);
        if (result.outcome == Internal::PublicationOutcome::Uncertain)
            throw Output::save_uncertain(reason, result.recovery_path, result.recovery_path_available);
        throw Output::save_failed(reason);
    };
    if (context.lifecycle.expired() || context.doc->saveInstanceGeneration() != context.instance_generation
        || context.doc->saveAttemptGeneration() != context.attempt_generation)
        stale("The snapshot was published after its document was closed, replaced, or superseded. Inspect the destination.");
    std::string const current_saved_path =
        context.doc->getDocumentFilename() ? context.doc->getDocumentFilename() : "";
    if (current_saved_path != context.prior_saved_path)
        stale("The snapshot was published after the document's official path changed. Inspect the destination.");
    if (result.outcome == Internal::PublicationOutcome::Failed
        || result.outcome == Internal::PublicationOutcome::Uncertain) {
        if (context.official || context.may_alias_official) context.doc->setModifiedSinceSave(true);
        if (context.official || context.may_alias_official)
            DocumentUndo::markInteractionBaselineDirty(context.doc);
        if (context.may_alias_official || (context.official && current_saved_path != context.prior_saved_path))
            context.doc->get_event_log()->invalidateFileSave();
    }
    context.handled = result.outcome != Internal::PublicationOutcome::Published;
    switch (result.outcome) {
    case Internal::PublicationOutcome::Unsupported: throw Output::save_unsupported(result.error);
    case Internal::PublicationOutcome::Conflict: throw Output::save_conflict(result.error);
    case Internal::PublicationOutcome::Uncertain:
        throw Output::save_uncertain(result.error, result.recovery_path, result.recovery_path_available);
    case Internal::PublicationOutcome::Failed: throw Output::save_failed(result.error);
    case Internal::PublicationOutcome::ReadOnly: throw Output::file_read_only();
    case Internal::PublicationOutcome::Published: break;
    }
    auto const *output_id = context.output->get_id();
    bool const native_svg = output_id
        && (std::strcmp(output_id, SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE) == 0
            || std::strcmp(output_id, SP_MODULE_KEY_OUTPUT_SVGZ_INKSCAPE) == 0);
    bool const compressed = output_id
        && std::strcmp(output_id, SP_MODULE_KEY_OUTPUT_SVGZ_INKSCAPE) == 0;
    bool const target_matches_format = g_str_has_suffix(context.filename.c_str(),
                                                        compressed ? ".svgz" : ".svg");
    if (context.official && native_svg && target_matches_format
        && result.serialized_size)
        context.doc->setLastKnownSerializedSize(*result.serialized_size);
    if (!context.official && context.may_alias_official) {
        // Save Copy usually leaves the original document untouched. If its
        // destination aliases the official file, however, that file may now
        // contain different bytes and no prior Undo position is safely clean.
        context.doc->setModifiedSinceSave(true);
        DocumentUndo::markInteractionBaselineDirty(context.doc);
        context.doc->get_event_log()->invalidateFileSave();
    }

    // Only a confirmed successful official save commits to the live document.
    // This is in-memory state only; the file has already been published, so
    // there is no rollback here (outcome-uncertain failures are F3's concern).
    if (context.official) {
        // The bytes are published, but they do not include intervening edits.
        // Do not rebase a live Save As document to a stale snapshot, move the
        // Undo save anchor, or return success to save-on-close callers.
        auto const *current_xml = context.doc->getReprDoc();
        if (!context.saved_revision || !context.save_invalidation || !current_xml
            || current_xml->contentRevision() != context.saved_revision
            || context.doc->saveInvalidationGeneration() != context.save_invalidation) {
            context.doc->setModifiedSinceSave(true);
            DocumentUndo::markInteractionBaselineDirty(context.doc);
            // Disk may hold a snapshot no Undo position represents, so the old
            // anchor must not become a false-clean state. Keep it only when the
            // publication target was proven to be a different physical file and
            // the live official identity did not move during the callback.
            if (!context.target_definitely_distinct || current_saved_path != context.prior_saved_path) {
                context.doc->get_event_log()->invalidateFileSave();
            }
            context.handled = true;
            throw PublishedOlderRevision{};
        }
        apply_save_preparation(context.doc, context.filename.c_str(), *context.output, context.preparation);
        store_file_extension_in_prefs(context.output->get_id(), context.save_method);
        context.doc->setModifiedSinceSave(false);
        // The saved Undo anchor must move only for a real save, never Save Copy.
        auto *event_log = context.doc->get_event_log();
        if (auto const position = event_log->findEventBySerial(context.undo_serial)) {
            event_log->rememberFileSave((*position)[Inkscape::EventLog::getColumns().serial]);
        } else {
            event_log->invalidateFileSave();
        }
    }
    context.handled = true;
}

/**
 * \return   None
 * \brief    This is a generic function to use the save function of
 *           a module (including Autodetect)
 * \param    key       Identifier of which module to use
 * \param    doc       The document to be saved
 * \param    filename  The file that the document should be saved to
 * \param    official  (optional) whether to set :output_module and :modified in the
 *                     document; is true for normal save, false for temporary saves
 *
 * The module database is queried with the key given.
 *
 * If everything is cool at this point, the module is loaded, and there is possibility for
 * preferences.  If there is a function, then it is executed to get the dialog to be displayed.
 * After it is finished the function continues.
 *
 * Lastly, the save function is called in the module itself.
 */
void
save(Extension *key, SPDocument *doc, gchar const *filename, bool check_overwrite, bool official,
    Inkscape::Extension::FileSaveMethod save_method)
{
    // Preserve the existing six-argument symbol; share the real implementation.
    save(key, doc, filename, check_overwrite, official, save_method, OverwriteConfirm{}, false);
}

void
save(Extension *key, SPDocument *doc, gchar const *filename, bool check_overwrite, bool official,
    Inkscape::Extension::FileSaveMethod save_method,
    Inkscape::Extension::OverwriteConfirm const &confirm_overwrite)
{
    // Preserve the existing seven-argument (test seam) symbol.
    save(key, doc, filename, check_overwrite, official, save_method, confirm_overwrite, false);
}

void
save(Extension *key, SPDocument *doc, gchar const *filename, bool check_overwrite, bool official,
    Inkscape::Extension::FileSaveMethod save_method,
    Inkscape::Extension::OverwriteConfirm const &confirm_overwrite,
    bool sync_title)
{
    save(key, doc, filename, check_overwrite, official, save_method,
         confirm_overwrite, sync_title, nullptr);
}

struct PendingSave {
    SaveCompletionContext context;
    std::optional<Internal::PublicationJob> job;
    Internal::PublicationResult direct_result;
    // Keeps a deferred-serialization snapshot anchored until this pending save
    // is disposed, which always happens on the initiating thread.
    std::shared_ptr<Internal::SerializationSnapshot> snapshot;
};

PendingSavePtr
begin_save_async(Extension *key, SPDocument *doc, gchar const *filename, bool check_overwrite, bool official,
           Inkscape::Extension::FileSaveMethod save_method,
           Inkscape::Extension::OverwriteConfirm const &confirm_overwrite,
           bool sync_title, std::string *notice, bool interactive_file_save)
{
    assert(key);

    Output *omod = dynamic_cast<Output *>(key);
    if (!omod) {
        g_warning("No valid output module provided to handle file: %s\n", filename);
        throw Output::no_extension_found();
    }

    omod->set_state(Extension::STATE_LOADED);
    if (!omod->loaded()) {
        throw Output::save_failed();
    }

    if (!omod->prefs()) {
        throw Output::save_cancelled();
    }

    if (check_overwrite && !(confirm_overwrite ? confirm_overwrite(filename)
                                               : sp_ui_overwrite_file(filename))) {
        throw Output::no_overwrite();
    }

    // Declare the preparation that Output::save() applies to its single working
    // copy. Nothing on the live document is touched before the write succeeds.
    std::string save_route;
    std::optional<std::size_t> direct_size_hint;
    SavePreparation prep;
    prep.document_identity = official;
    prep.update_dataloss = true;
    prep.sync_title = sync_title;
    prep.notice = notice;
    prep.route = &save_route;
    prep.direct_size_hint = &direct_size_hint;
    // Only file.cpp explicitly requests this route. Module gui state is mutable
    // during export-do and cannot identify the origin of a later Save.
    auto const *output_id = omod->get_id();
    bool const explicit_save = save_method == FILE_SAVE_METHOD_SAVE_AS
        || save_method == FILE_SAVE_METHOD_SAVE_COPY
        || save_method == FILE_SAVE_METHOD_INKSCAPE_SVG;
    prep.interactive_native_save = interactive_file_save && explicit_save
        && output_id && (std::strcmp(output_id, SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE) == 0
                         || std::strcmp(output_id, SP_MODULE_KEY_OUTPUT_SVGZ_INKSCAPE) == 0);
    // Direct routes retain the early check. Buffered publication admits the
    // destination at publish time, after serialization.
    bool buffered_admission = false;
#ifdef __APPLE__
    buffered_admission = prep.interactive_native_save;
#endif
    if (!buffered_admission && Inkscape::IO::file_test(filename, G_FILE_TEST_EXISTS)
        && !Inkscape::IO::file_is_writable(filename)) throw Output::file_read_only();

    // A synchronous output module can reenter the main loop (and future saves
    // may write asynchronously). Capture the live XML generation before taking
    // the output copy so a later edit cannot be marked clean by this save.
    auto const *live_xml = doc->getReprDoc();
    auto const saved_revision = live_xml ? live_xml->contentRevision() : std::nullopt;
    auto const save_invalidation = doc->saveInvalidationGeneration();

    // A stale official publish must drop the saved Undo anchor only when it may
    // have overwritten the document's current official file. Decide that now,
    // before the module runs; the live identity is captured too so a callback
    // that moves it is treated conservatively below.
    std::string const prior_saved_path =
        doc->getDocumentFilename() ? doc->getDocumentFilename() : "";
    // Ordinary same-path Save and Save Copy need no extra filesystem probe.
    // A different target may still alias the official file, even for Save Copy.
    // Prove distinctness before output so an uncertain result cannot leave a
    // stale Undo save anchor pointing at bytes that may have been replaced.
    // Temporary extension output is a freshly created scratch file, not a
    // user-directed Save Copy. It must never change the live save anchor.
    bool const user_publication = save_method != FILE_SAVE_METHOD_TEMPORARY;
    bool const target_definitely_distinct = user_publication && filename && !prior_saved_path.empty()
        && prior_saved_path != filename
        && publicationTargetDefinitelyDistinctFromSavedPath(filename, prior_saved_path);
    bool const may_alias_official = user_publication && !prior_saved_path.empty() && !target_definitely_distinct;

    // A generic output module may have written before throwing save_failed;
    // save_uncertain explicitly says publication may have happened. Preserve
    // the typed exception, but never leave the old Undo anchor able to mark
    // an earlier state clean when the official file may now contain new bytes.
    bool const user_save_attempt = save_method != FILE_SAVE_METHOD_TEMPORARY;
    SaveCompletionContext context{doc, doc->saveLifecycleToken(), doc->saveInstanceGeneration(),
        user_save_attempt ? doc->beginSaveAttempt() : doc->saveAttemptGeneration(),
        doc->get_event_log()->getCurrEventSerial(),
        omod, filename, prep, save_method, official, saved_revision, save_invalidation,
        prior_saved_path, target_definitely_distinct, may_alias_official};
    context.preparation.notice = nullptr;
    context.preparation.route = nullptr;
    context.preparation.direct_size_hint = nullptr;
    PendingSavePtr pending(new PendingSave(std::move(context)), &dispose_pending_save);
    auto &begun = *pending;
    begun.direct_result.outcome = Internal::PublicationOutcome::Published;
    try {
        if (prep.interactive_native_save) {
#ifdef __APPLE__
            begun.job = omod->begin_publication(doc, filename, prep);
            if (!begun.job) {
                begun.direct_result.route = save_route;
                begun.direct_result.serialized_size = direct_size_hint;
            }
#else
            omod->save(doc, filename, /*detachbase=*/false, prep);
            begun.direct_result.route = "direct";
#endif
        } else {
            omod->save(doc, filename, /*detachbase=*/false, prep);
        }
        begun.context.notice += notice ? *notice : "";
    } catch (Output::save_unsupported const &) {
        throw; // Contract: destination was not written.
    } catch (Output::save_conflict const &) {
        throw; // Contract: destination was not overwritten.
    } catch (Output::save_cancelled const &) {
        throw;
    } catch (Output::no_overwrite const &) {
        throw;
    } catch (Output::file_read_only const &) {
        throw; // Direct-route admission found a read-only target before writing.
    } catch (PublishedOlderRevision const &) {
        if (notice) *notice = begun.context.notice;
        throw;
    } catch (PublishedStaleDocument const &) {
        if (notice) *notice = begun.context.notice;
        throw;
    } catch (...) {
        Internal::PublicationResult failure;
        failure.outcome = Internal::PublicationOutcome::Failed;
        if (!begun.context.handled) {
            try { complete(failure, begun.context); } catch (...) {}
        }
        if (notice && !begun.context.notice.empty()) *notice = begun.context.notice;
        throw;
    }
    return pending;
}

void dispose_pending_save(PendingSave *pending) noexcept { delete pending; }
bool pending_save_may_alias_official(PendingSave const &pending) noexcept { return pending.context.may_alias_official; }
std::optional<uint64_t> pending_save_snapshot_revision(PendingSave const &pending) noexcept { return pending.context.saved_revision; }
void release_publication_snapshot(PendingSave &pending) noexcept { pending.snapshot.reset(); }
static std::function<void(SPDocument *)> save_completion_interleaving;
void set_save_completion_interleaving_for_testing(std::function<void(SPDocument *)> callback)
{
    save_completion_interleaving = std::move(callback);
}

std::optional<Internal::PublicationJob> take_publication_job(
    PendingSave &pending, Internal::PublicationResult *direct_result)
{
    if (direct_result) *direct_result = pending.direct_result;
    auto job = std::exchange(pending.job, {});
    // The snapshot's GC anchor must be released on this thread, never where
    // the job ends up (the publication worker destroys its job).
    if (job) pending.snapshot = std::move(job->snapshot_owner);
    return job;
}

void finish_save_async(PendingSave &begun, Internal::PublicationResult const &result,
                       std::string *notice, bool force_older)
{
    auto &context = begun.context;
    if (Inkscape::IO::file_io_test_hooks_enabled() && save_completion_interleaving)
        std::exchange(save_completion_interleaving, {})(context.doc);
    // Test-only interleaving after real publication and before completion.
#ifdef __APPLE__
    if (context.preparation.interactive_native_save
        && Inkscape::IO::file_io_test_hooks_enabled()
        && g_strcmp0(g_getenv("VACARDS_SAVE_TEST_STALE_AFTER_PUBLISH"), "1") == 0)
        context.doc->beginSaveAttempt();
#endif
    try {
        if (force_older) context.saved_revision.reset();
        complete(result, context);
        if (notice) *notice = context.notice;
    } catch (PublishedOlderRevision const &) {
        if (notice && !context.notice.empty()) *notice = context.notice;
        throw;
    } catch (PublishedStaleDocument const &) {
        if (notice && !context.notice.empty()) *notice = context.notice;
        throw;
    } catch (...) {
        if (!context.handled) {
            Internal::PublicationResult failure;
            failure.outcome = Internal::PublicationOutcome::Failed;
            try { complete(failure, context); } catch (...) {}
        }
        if (notice && !context.notice.empty()) *notice = context.notice;
        throw;
    }
}

void
save(Extension *key, SPDocument *doc, gchar const *filename, bool check_overwrite, bool official,
    Inkscape::Extension::FileSaveMethod save_method,
    Inkscape::Extension::OverwriteConfirm const &confirm_overwrite,
    bool sync_title, std::string *notice, bool interactive_file_save)
{
    auto begun = begin_save_async(key, doc, filename, check_overwrite, official,
                            save_method, confirm_overwrite, sync_title, notice, interactive_file_save);
    try {
        Internal::PublicationResult result;
        auto job = take_publication_job(*begun, &result);
#if defined(__APPLE__) || defined(_WIN32)
        if (job) result = Internal::publish(std::move(*job));
#endif
        finish_save_async(*begun, result, notice, false);
    } catch (PublishedOlderRevision const &) {
        throw;
    } catch (PublishedStaleDocument const &) {
        throw;
    } catch (...) {
        if (!begun->context.handled) {
            Internal::PublicationResult failure;
            failure.outcome = Internal::PublicationOutcome::Failed;
            try { complete(failure, begun->context); } catch (...) {}
        }
        if (notice && !begun->context.notice.empty()) *notice = begun->context.notice;
        throw;
    }
}

Print *
get_print(gchar const *key)
{
    return dynamic_cast<Print *>(db.get(key));
}

/**
 * \return   true if extension successfully parsed, false otherwise
 *           A true return value does not guarantee an extension was actually registered,
 *           but indicates no errors occurred while parsing the extension.
 * \brief    Creates a module from a Inkscape::XML::Document describing the module
 * \param    doc  The XML description of the module
 *
 * This function basically has two segments.  The first is that it goes through the Repr tree
 * provided, and determines what kind of module this is, and what kind of implementation to use.
 * All of these are then stored in two enums that are defined in this function.  This makes it
 * easier to add additional types (which will happen in the future, I'm sure).
 *
 * Second, there is case statements for these enums.  The first one is the type of module.  This is
 * the one where the module is actually created.  After that, then the implementation is applied to
 * get the load and unload functions.  If there is no implementation then these are not set.  This
 * case could apply to modules that are built in (like the SVG load/save functions).
 */
bool
build_from_reprdoc(Inkscape::XML::Document *doc, std::unique_ptr<Implementation::Implementation> in_imp, std::string* baseDir, std::string* file_name)
{
    ModuleImpType module_implementation_type = MODULE_UNKNOWN_IMP;
    ModuleFuncType module_functional_type = MODULE_UNKNOWN_FUNC;

    g_return_val_if_fail(doc != nullptr, false);

    Inkscape::XML::Node *repr = doc->root();

    if (strcmp(repr->name(), INKSCAPE_EXTENSION_NS "inkscape-extension")) {
        g_warning("Extension definition started with <%s> instead of <" INKSCAPE_EXTENSION_NS "inkscape-extension>.  Extension will not be created. See http://wiki.inkscape.org/wiki/index.php/Extensions for reference.\n", repr->name());
        return false;
    }

    Inkscape::XML::Node *child_repr = repr->firstChild();
    while (child_repr != nullptr) {
        char const *element_name = child_repr->name();
        /* printf("Child: %s\n", child_repr->name()); */
        if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "input")) {
            module_functional_type = MODULE_INPUT;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "template")) {
            module_functional_type = MODULE_TEMPLATE;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "output")) {
            module_functional_type = MODULE_OUTPUT;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "effect")) {
            module_functional_type = MODULE_FILTER;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "print")) {
            module_functional_type = MODULE_PRINT;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "path-effect")) {
            module_functional_type = MODULE_PATH_EFFECT;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "script")) {
            module_implementation_type = MODULE_EXTENSION;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "xslt")) {
            module_implementation_type = MODULE_XSLT;
        } else if (!strcmp(element_name, INKSCAPE_EXTENSION_NS "plugin")) {
            module_implementation_type = MODULE_PLUGIN;
        }

        //Inkscape::XML::Node *old_repr = child_repr;
        child_repr = child_repr->next();
        //Inkscape::GC::release(old_repr);
    }

    using ImplementationHolder = Util::HybridPointer<Implementation::Implementation>;
    ImplementationHolder imp;
    if (in_imp) {
        imp = std::move(in_imp);
    } else {
        switch (module_implementation_type) {
            case MODULE_EXTENSION: {
                imp = ImplementationHolder::make_owning<Implementation::Script>();
                break;
            }
            case MODULE_XSLT: {
                imp = ImplementationHolder::make_owning<Implementation::XSLT>();
                break;
            }
            case MODULE_PLUGIN: {
                auto loader = Inkscape::Extension::Loader();
                if (baseDir) {
                    loader.set_base_directory(*baseDir);
                }
                imp = ImplementationHolder::make_nonowning(loader.load_implementation(doc));
                break;
            }
        }
    }

    std::unique_ptr<Extension> module;
    try {
        switch (module_functional_type) {
            case MODULE_INPUT: {
                module = std::make_unique<Input>(repr, std::move(imp), baseDir);
                break;
            }
            case MODULE_TEMPLATE: {
                module = std::make_unique<Template>(repr, std::move(imp), baseDir);
                break;
            }
            case MODULE_OUTPUT: {
                module = std::make_unique<Output>(repr, std::move(imp), baseDir);
                break;
            }
            case MODULE_FILTER: {
                module = std::make_unique<Effect>(repr, std::move(imp), baseDir, file_name);
                break;
            }
            case MODULE_PRINT: {
                module = std::make_unique<Print>(repr, std::move(imp), baseDir);
                break;
            }
            case MODULE_PATH_EFFECT: {
                module = std::make_unique<PathEffect>(repr, std::move(imp), baseDir);
                break;
            }
            default: {
                g_warning("Extension of unknown type!"); // TODO: Should not happen! Is this even useful?
                module = std::make_unique<Extension>(repr, std::move(imp), baseDir);
                break;
            }
        }
    } catch (const Extension::extension_no_id &) {
        g_warning("Building extension failed. Extension does not have a valid ID");
        return false;
    } catch (const Extension::extension_no_name &) {
        g_warning("Building extension failed. Extension does not have a valid name");
        return false;
    } catch (const Extension::extension_not_compatible &) {
        return true; // This is not an actual error; just silently ignore the extension
    } catch (Extension::no_implementation_for_extension &) {
        g_warning("Building extension failed: no implementation was found");
        return false;
    }

    assert(module);
    db.take_ownership(std::move(module));
    return true;
}

/**
 * \brief    This function creates a module from a filename of an
 *           XML description.
 * \param    filename  The file holding the XML description of the module.
 *
 * This function calls build_from_reprdoc with using sp_repr_read_file to create the reprdoc.
 */
void
build_from_file(gchar const *filename)
{
    std::string dir = Glib::path_get_dirname(filename);
    auto file_name = Glib::path_get_basename(filename);

    Inkscape::XML::Document *doc = sp_repr_read_file(filename, INKSCAPE_EXTENSION_URI);
    if (!doc) {
        g_critical("Inkscape::Extension::build_from_file() - XML description loaded from '%s' not valid.", filename);
        return;
    }

    if (!build_from_reprdoc(doc, {}, &dir, &file_name)) {
        g_warning("Inkscape::Extension::build_from_file() - Could not parse extension from '%s'.", filename);
    }

    Inkscape::GC::release(doc);
}

/**
 * \brief Create a module from a buffer holding an XML description.
 * \param buffer The buffer holding the XML description of the module.
 * \param in_imp An owning pointer to a freshly created implementation.
 *
 * This function calls build_from_reprdoc with using sp_repr_read_mem to create the reprdoc.  It
 * finds the length of the buffer using strlen.
 */
void
build_from_mem(gchar const *buffer, std::unique_ptr<Implementation::Implementation> in_imp)
{
    Inkscape::XML::Document *doc = sp_repr_read_mem(buffer, strlen(buffer), INKSCAPE_EXTENSION_URI);
    if (!doc) {
        g_critical("Inkscape::Extension::build_from_mem() - XML description loaded from memory buffer not valid.");
        return;
    }

    if (!build_from_reprdoc(doc, std::move(in_imp), nullptr, nullptr)) {
        g_critical("Inkscape::Extension::build_from_mem() - Could not parse extension from memory buffer.");
    }

    Inkscape::GC::release(doc);
}

/*
 * TODO: Is it guaranteed that the returned extension is valid? If so, we can remove the check for
 * filename_extension in sp_file_save_dialog().
 */
Glib::ustring
get_file_save_extension (Inkscape::Extension::FileSaveMethod method) {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    Glib::ustring extension;
    switch (method) {
        case FILE_SAVE_METHOD_SAVE_AS:
        case FILE_SAVE_METHOD_TEMPORARY:
            extension = prefs->getString("/dialogs/save_as/default");
            break;
        case FILE_SAVE_METHOD_SAVE_COPY:
            extension = prefs->getString("/dialogs/save_copy/default");
            break;
        case FILE_SAVE_METHOD_INKSCAPE_SVG:
            extension = SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE;
            break;
        case FILE_SAVE_METHOD_EXPORT:
            /// \todo no default extension set for Export? defaults to SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE is ok?
            break;
    }

    if(extension.empty()) {
        extension = SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE;
    }

    return extension;
}

Glib::ustring
get_file_save_path (SPDocument *doc, FileSaveMethod method) {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    Glib::ustring path;
    bool use_current_dir = true;
    switch (method) {
        case FILE_SAVE_METHOD_SAVE_AS:
        {
            use_current_dir = prefs->getBool("/dialogs/save_as/use_current_dir", true);
            if (doc->getDocumentFilename() && use_current_dir) {
                path = Glib::path_get_dirname(doc->getDocumentFilename());
            } else {
                path = prefs->getString("/dialogs/save_as/path");
            }
            break;
        }
        case FILE_SAVE_METHOD_TEMPORARY:
            path = prefs->getString("/dialogs/save_as/path");
            break;
        case FILE_SAVE_METHOD_SAVE_COPY:
            use_current_dir = prefs->getBool("/dialogs/save_copy/use_current_dir", prefs->getBool("/dialogs/save_as/use_current_dir", true));
            if (doc->getDocumentFilename() && use_current_dir) {
                path = Glib::path_get_dirname(doc->getDocumentFilename());
            } else {
                path = prefs->getString("/dialogs/save_copy/path");
            }
            break;
        case FILE_SAVE_METHOD_INKSCAPE_SVG:
            if (doc->getDocumentFilename()) {
                path = Glib::path_get_dirname(doc->getDocumentFilename());
            } else {
                // FIXME: should we use the save_as path here or something else? Maybe we should
                // leave this as a choice to the user.
                path = prefs->getString("/dialogs/save_as/path");
            }
            break;
        case FILE_SAVE_METHOD_EXPORT:
            /// \todo no default path set for Export?
            // defaults to g_get_home_dir()
            break;
    }

    if(path.empty()) {
        path = g_get_home_dir(); // Is this the most sensible solution? Note that we should avoid
                                 // g_get_current_dir because this leads to problems on OS X where
                                 // Inkscape opens the dialog inside application bundle when it is
                                 // invoked for the first teim.
    }

    return path;
}

void
store_file_extension_in_prefs (Glib::ustring extension, FileSaveMethod method) {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    switch (method) {
        case FILE_SAVE_METHOD_SAVE_AS:
        case FILE_SAVE_METHOD_TEMPORARY:
            prefs->setString("/dialogs/save_as/default", extension);
            break;
        case FILE_SAVE_METHOD_SAVE_COPY:
            prefs->setString("/dialogs/save_copy/default", extension);
            break;
        case FILE_SAVE_METHOD_INKSCAPE_SVG:
        case FILE_SAVE_METHOD_EXPORT:
            // do nothing
            break;
    }
}

void
store_save_path_in_prefs (Glib::ustring path, FileSaveMethod method) {
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    switch (method) {
        case FILE_SAVE_METHOD_SAVE_AS:
        case FILE_SAVE_METHOD_TEMPORARY:
            prefs->setString("/dialogs/save_as/path", path);
            break;
        case FILE_SAVE_METHOD_SAVE_COPY:
            prefs->setString("/dialogs/save_copy/path", path);
            break;
        case FILE_SAVE_METHOD_INKSCAPE_SVG:
        case FILE_SAVE_METHOD_EXPORT:
            // do nothing
            break;
    }
}

} // namespace Inkscape::Extension

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

#ifdef WITH_LIBCDR
#include <libcdr/libcdr.h>
#include <librevenge/librevenge.h>
#include <librevenge-stream/librevenge-stream.h>
#endif
namespace Inkscape::Extension {
#ifdef WITH_LIBCDR
namespace {
// RVNG has a private ostringstream, not a streaming output interface. Charge
// callbacks before forwarding and check real completed output after every page.
// This bounds admitted generator work, not libcdr's earlier parser allocations.
// Never throw for budget refusal: libcdr also emits callbacks from destructors.
// Once exceeded, every callback is a no-op and parse can clean up normally.
class BudgetedCdrGenerator final : public librevenge::RVNGSVGDrawingGenerator {
    librevenge::RVNGStringVector &output;
    CdrConversionLimits limits;
    std::size_t charged = 0, completed = 0, style = 0;
    unsigned pages = 0;
    bool charge(std::size_t amount) {
        if (exceeded || amount > limits.bytes - charged) {
            exceeded = true;
            return false;
        }
        charged += amount;
        return true;
    }
    bool properties(librevenge::RVNGPropertyList const &props, unsigned depth = 0) {
        if (!charge(4096)) return false;
        if (depth > 128) { exceeded=true; return false; }
        librevenge::RVNGPropertyList::Iter it(props);
        for (it.rewind(); it.next();) {
            if (auto children=it.child()) {
                for (unsigned i=0;i<children->count();++i)
                    if (!properties((*children)[i],depth+1)) return false;
            } else if (it()) {
                auto value=it()->getStr();
                if (value.size() > limits.bytes / 8) { exceeded=true; return false; }
                if (!charge(8 * value.size() + 256)) return false;
            }
        }
        return true;
    }
public:
    bool exceeded = false;
    BudgetedCdrGenerator(librevenge::RVNGStringVector &out, CdrConversionLimits lim)
        : RVNGSVGDrawingGenerator(out,"svg"), output(out), limits(lim) {}
    void startPage(librevenge::RVNGPropertyList const &p) override {
        if (exceeded || pages >= limits.pages) { exceeded=true; return; }
        ++pages; if (properties(p)) RVNGSVGDrawingGenerator::startPage(p);
    }
    void endPage() override {
        if (!charge(4096)) return;
        RVNGSVGDrawingGenerator::endPage();
        auto size=output[output.size()-1].size();
        if (size > limits.bytes - completed) { exceeded=true; return; }
        completed += size;
    }
    void setStyle(librevenge::RVNGPropertyList const &p) override {
        auto before=charged; if (!properties(p)) return; style=charged-before;
        RVNGSVGDrawingGenerator::setStyle(p);
    }
    void insertText(librevenge::RVNGString const &s) override {
        if (s.size() > limits.bytes / 8) { exceeded=true; return; }
        if (charge(4096 + 8*s.size())) RVNGSVGDrawingGenerator::insertText(s);
    }
#define CDR_PROPERTIES(method) \
    void method(librevenge::RVNGPropertyList const &p) override { \
        if (charge(style) && properties(p)) RVNGSVGDrawingGenerator::method(p); }
#define CDR_EVENT(method) \
    void method() override { if (charge(4096)) RVNGSVGDrawingGenerator::method(); }
    CDR_PROPERTIES(startDocument)
    CDR_PROPERTIES(setDocumentMetaData)
    CDR_PROPERTIES(defineEmbeddedFont)
    CDR_PROPERTIES(startMasterPage)
    CDR_PROPERTIES(startLayer)
    CDR_PROPERTIES(startEmbeddedGraphics)
    CDR_PROPERTIES(openGroup)
    CDR_PROPERTIES(drawRectangle)
    CDR_PROPERTIES(drawEllipse)
    CDR_PROPERTIES(drawPolyline)
    CDR_PROPERTIES(drawPolygon)
    CDR_PROPERTIES(drawPath)
    CDR_PROPERTIES(drawGraphicObject)
    CDR_PROPERTIES(drawConnector)
    CDR_PROPERTIES(startTextObject)
    CDR_PROPERTIES(startTableObject)
    CDR_PROPERTIES(openTableRow)
    CDR_PROPERTIES(openTableCell)
    CDR_PROPERTIES(insertCoveredTableCell)
    CDR_PROPERTIES(openOrderedListLevel)
    CDR_PROPERTIES(openUnorderedListLevel)
    CDR_PROPERTIES(openListElement)
    CDR_PROPERTIES(defineParagraphStyle)
    CDR_PROPERTIES(openParagraph)
    CDR_PROPERTIES(defineCharacterStyle)
    CDR_PROPERTIES(openSpan)
    CDR_PROPERTIES(openLink)
    CDR_PROPERTIES(insertField)
    CDR_EVENT(endDocument)
    CDR_EVENT(endMasterPage)
    CDR_EVENT(endLayer)
    CDR_EVENT(endEmbeddedGraphics)
    CDR_EVENT(closeGroup)
    CDR_EVENT(endTextObject)
    CDR_EVENT(closeTableRow)
    CDR_EVENT(closeTableCell)
    CDR_EVENT(endTableObject)
    CDR_EVENT(closeOrderedListLevel)
    CDR_EVENT(closeUnorderedListLevel)
    CDR_EVENT(closeListElement)
    CDR_EVENT(closeParagraph)
    CDR_EVENT(closeSpan)
    CDR_EVENT(closeLink)
    CDR_EVENT(insertTab)
    CDR_EVENT(insertSpace)
    CDR_EVENT(insertLineBreak)
#undef CDR_PROPERTIES
#undef CDR_EVENT
};
}
#endif
std::vector<std::string> cdr_svg_pages(std::string const &bytes, std::string &error)
{ return cdr_svg_pages(bytes,error,{}); }
std::vector<std::string> cdr_svg_pages(std::string const &bytes, std::string &error, CdrConversionLimits limits)
{
    std::vector<std::string> pages;
#ifdef WITH_LIBCDR
    if (bytes.empty() || bytes.size() > (64u << 20)) { error = "CDR input exceeds 64 MiB"; return pages; }
    librevenge::RVNGStringStream input(reinterpret_cast<unsigned char const *>(bytes.data()), bytes.size());
    if (!libcdr::CDRDocument::isSupported(&input)) { error = "Unsupported CDR content"; return pages; }
    input.seek(0, librevenge::RVNG_SEEK_SET);
    librevenge::RVNGStringVector output;
    limits.bytes=std::min<std::size_t>(limits.bytes,64u << 20);
    limits.pages=std::min(limits.pages,1000u);
    BudgetedCdrGenerator generator(output, limits);
    bool parsed=libcdr::CDRDocument::parse(&input, &generator);
    if (generator.exceeded) { error="input-too-large"; return {}; }
    if (!parsed) { error = "Native CDR conversion failed"; return pages; }
    std::size_t total = 0;
    for (unsigned i=0; i<output.size(); ++i) {
        total += output[i].size();
        if (total > limits.bytes || i >= limits.pages) { error = "input-too-large"; return {}; }
        pages.emplace_back(output[i].cstr());
    }
    if (pages.empty()) error = "CDR contains no pages";
#else
    error = "Native CDR backend is unavailable in this build";
#endif
    return pages;
}
}
