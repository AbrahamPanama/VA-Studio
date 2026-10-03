// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * File/Print operations.
 */
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Chema Celorio <chema@celorio.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Bruno Dilly <bruno.dilly@gmail.com>
 *   Stephen Silver <sasilver@users.sourceforge.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *   Tavmjong Bah
 *
 * Copyright (C) 2006 Johan Engelen <johan@shouraizou.nl>
 * Copyright (C) 1999-2016 Authors
 * Copyright (C) 2004 David Turner
 * Copyright (C) 2001-2002 Ximian, Inc.
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

/** @file
 * @note This file needs to be cleaned up extensively.
 * What it probably needs is to have one .h file for
 * the API, and two or more .cpp files for the implementations.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"  // only include where actually required!
#endif

#include <libintl.h>

#include <gtkmm.h>

#include "desktop.h"
#include "document-undo.h"
#include "document-update.h"
#include "event-log.h"
#include "extension/db.h"
#include "extension/effect.h"
#include "extension/input.h"
#include "extension/output.h"
#include "file.h"
#include <functional>
#include <exception>
#include <unordered_map>
#include "id-clash.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "inkscape.h"
#include "io/file.h"
#include "io/document-file-operation.h"
#include "io/publication-worker.h"
#include "io/stream/bufferstream.h"
#include "io/fix-broken-links.h"
#include "io/resource.h"
#include "io/save-filename.h"
#include "io/sys.h"
#include "layer-manager.h"
#include "libnrtype/font-lister.h"
#include "message-stack.h"
#include "object/sp-defs.h"
#include "object/sp-namedview.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "object/sp-use.h"
#include "page-manager.h"
#include "path-prefix.h"
#include "rdf.h"
#include "selection.h"
#include "style.h"
#include "svg/svg.h" // for sp_svg_transform_write, used in sp_import_document
#include "ui/cache/welcome-drawing-preview.h"
#include "ui/dialog/choose-file.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/dialog-run.h"
#include "ui/explode-bitmap-publication.h"
#include "ui/icon-names.h"
#include "ui/interface.h"
#include "ui/tools/tool-base.h"
#include "util/recently-used-fonts.h"
#include "xml/rebase-hrefs.h"
#include "xml/sp-css-attr.h"

using Inkscape::DocumentUndo;
using Inkscape::IO::Resource::TEMPLATES;
using Inkscape::IO::Resource::USER;


/*######################
## N E W
######################*/

/**
 * Create a blank document and add it to the desktop
 * Input: empty string or template file name.
 */
SPDesktop *sp_file_new(const std::string &templ)
{
    auto *app = InkscapeApplication::instance();

    auto doc = app->document_new(templ);
    if (!doc) {
        std::cerr << "sp_file_new: failed to open document: " << templ << std::endl;
    }

    return app->desktopOpen(doc);
}

std::string sp_file_default_template_uri()
{
    return Inkscape::IO::Resource::get_filename(TEMPLATES, "default.svg", true);
}

SPDesktop* sp_file_new_default()
{
    SPDesktop* desk = sp_file_new(sp_file_default_template_uri());
    //rdf_add_from_preferences( SP_ACTIVE_DOCUMENT );

    return desk;
}

/**
 *  Handle prompting user for "do you want to revert"?  Revert on "OK"
 */
void sp_file_revert_dialog()
{
    SPDesktop  *desktop = SP_ACTIVE_DESKTOP;
    g_assert(desktop != nullptr);

    SPDocument *doc = desktop->getDocument();
    g_assert(doc != nullptr);

    Inkscape::XML::Node *repr = doc->getReprRoot();
    g_assert(repr != nullptr);

    gchar const *filename = doc->getDocumentFilename();
    if (!filename) {
        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not saved yet.  Cannot revert."));
        return;
    }

    // An explicit Save As/Save Copy bound to this document owns its file-operation
    // slot while the chooser is open. Refuse with a specific reason before the
    // data-loss prompt rather than letting document_revert() load and swap the
    // document out from under the pending save. The app-level check in
    // document_revert() is the defensive backstop; this branch owns the message.
    auto save_operation_active = [&desktop, &doc]() -> bool {
        if (!Inkscape::IO::DocumentFileOperation::hasActiveRequest(*doc)) {
            return false;
        }
        desktop->messageStack()->flash(Inkscape::WARNING_MESSAGE,
            _("Cannot revert the document while a save operation is in progress. Finish the open save operation before reverting."));
        return true;
    };

    if (save_operation_active()) {
        return;
    }

    bool do_revert = true;
    if (doc->isModifiedSinceSave()) {
        Glib::ustring tmpString = Glib::ustring::compose(_("Changes will be lost! Are you sure you want to reload document %1?"), filename);
        bool response = desktop->warnDialog (tmpString);
        if (!response) {
            do_revert = false;
        }
    }

    // The warning prompt above runs its own modal loop, during which a save could
    // be admitted. Re-check immediately before reverting so a Save As/Save Copy
    // chooser that appeared while the prompt was open is not invalidated by a reload.
    if (do_revert && save_operation_active()) {
        return;
    }

    bool reverted = false;
    if (do_revert) {
        auto *app = InkscapeApplication::instance();
        reverted = app->document_revert (doc);
    }

    if (reverted) {
        desktop->messageStack()->flash(Inkscape::NORMAL_MESSAGE, _("Document reverted."));
    } else {
        desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not reverted."));
    }
}


/*######################
## S A V E
######################*/

Glib::ustring saved_older_revision_dialog_text(Glib::ustring const &display_name,
                                               std::string const &save_notice)
{
    Glib::ustring text = Glib::ustring::sprintf(
        _("An earlier version was saved to %s, but newer changes are still unsaved. Save again before closing this document."),
        display_name);
    if (!save_notice.empty()) {
        text += "\n\n";
        text += Inkscape::IO::sanitizeString(save_notice.c_str());
    }
    return text;
}

Glib::ustring saved_stale_document_dialog_text(Glib::ustring const &display_name,
                                                std::string const &reason,
                                                std::string const &save_notice)
{
    auto text = Glib::ustring::sprintf(
        _("A snapshot was saved to %s, but this document was closed, replaced, or changed during the save. Inspect the destination before saving again.\n\n%s"),
        display_name, reason);
    if (!save_notice.empty()) {
        text += "\n\n";
        text += Inkscape::IO::sanitizeString(save_notice.c_str());
    }
    return text;
}

Glib::ustring save_admission_refusal_text(Inkscape::IO::FileOperationResult const &refusal)
{
    return refusal.refusal == Inkscape::IO::FileOperationRefusal::NotReady
        ? _("Finish the current edit before saving.")
        : _("Finish the open save operation before saving again.");
}

// Main-thread UI completion after a confirmed publication.
static bool complete_file_save(Gtk::Window *parentWindow, InkscapeWindow::LifetimeToken window_token,
                               SPDocument *doc, SPDesktop *desktop, bool official,
                               std::string save_notice, Glib::RefPtr<Gio::File> const &file,
                               Inkscape::Extension::FileSaveMethod save_method, bool remember_path,
                               bool bound_desktop = false)
{
    SPDesktop *const flash_to = desktop ? desktop : bound_desktop ? nullptr : SP_ACTIVE_DESKTOP;
    if (flash_to) {
        if (! flash_to->messageStack()) {
            g_message("file_save: ->messageStack() == NULL. please report to bug #967416");
        }
    } else {
        g_message("file_save: SP_ACTIVE_DESKTOP == NULL. please report to bug #967416");
    }

    auto font_lister = Inkscape::FontLister::get_instance();
    auto recently_used = Inkscape::RecentlyUsedFonts::get();
    recently_used->prepend_to_list(font_lister->get_font_family());
    recently_used->set_continuous_streak(false);

    Glib::ustring msg;
    if (doc->getDocumentFilename() == nullptr) {
        msg = Glib::ustring::format(_("Document saved."));
    } else {
        msg = Glib::ustring::format(_("Document saved."), " ", doc->getDocumentFilename());
    }
    if (flash_to && flash_to->messageStack()) {
        flash_to->messageStack()->flash(Inkscape::NORMAL_MESSAGE, msg.c_str());
    }
    // The warning dialog runs a nested main loop; capture this while the
    // caller still owns the document instead of dereferencing it afterwards.
    bool const saved_current_version = !official || !doc->isModifiedSinceSave();
    if (saved_current_version && remember_path) {
        if (doc->getDocumentFilename()) {
            Gtk::RecentManager::get_default()->add_item(file->get_uri());
            if (Inkscape::IO::file_io_test_hooks_enabled())
                g_object_set_data(G_OBJECT(file->gobj()), "vacards-recent-add-called", GINT_TO_POINTER(1));
        }
        Inkscape::Extension::store_save_path_in_prefs(
            Glib::path_get_dirname(file->get_path()), save_method);
    }
    // The file now holds exactly this document: give the Welcome screen its
    // thumbnail, which the preview helper cannot make for large files.
    if (official && !doc->isModifiedSinceSave()) {
        // VIEW-1: on macOS the saved SVG also gets its drawing as Finder icon.
        Inkscape::UI::Cache::store_document_thumbnail(*doc, file->get_path(), 0, {}, {}, true);
    }
    if (!save_notice.empty()) {
        if (flash_to && flash_to->messageStack()) {
            flash_to->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                _("Document saved with a cleanup warning."));
        }
        auto const notice_text = Glib::ustring::sprintf(
            _("The document was saved with a cleanup warning. Inspect any retained copy before deleting it.\n\n%s"),
            Inkscape::IO::sanitizeString(save_notice.c_str()));
        if (parentWindow && window_token.valid()) {
            Gtk::MessageDialog dialog{*parentWindow, notice_text,
                                      false, Gtk::MessageType::WARNING, Gtk::ButtonsType::OK};
            Inkscape::UI::dialog_run(dialog);
        } else {
            if (Inkscape::IO::file_io_test_hooks_enabled()
                && g_strcmp0(g_getenv("VACARDS_SAVE_TEST_CAPTURE_NOTICE_FALLBACK"), "1") == 0) {
                g_object_set_data(G_OBJECT(file->gobj()), "vacards-fallback-notice-seen", GINT_TO_POINTER(1));
            } else {
                sp_ui_error_dialog(notice_text.c_str());
            }
        }
    }
    return saved_current_version;
}

struct SaveFailure {
    Glib::ustring status;
    Glib::ustring dialog;
    Inkscape::MessageType severity = Inkscape::ERROR_MESSAGE;
    bool retry = false;
    bool older = false;
    bool uncertain = false;
    bool unspecified = false;
};

// One typed-outcome mapping for synchronous Save and later main-thread completion.
static SaveFailure classify_file_save_exception(std::exception_ptr exception,
                                                Glib::ustring const &display_name,
                                                std::string &save_notice)
{
    SaveFailure failure;
    failure.status = _("Document not saved.");
    auto append_reason = [&](std::string const &reason) {
        if (!reason.empty()) {
            failure.dialog += "\n\n";
            failure.dialog += Glib::ustring::sprintf(
                _("The output extension reported: %s"), Glib::ustring(reason));
        }
    };
    try {
        std::rethrow_exception(exception);
    } catch (Inkscape::Extension::PublishedStaleDocument const &e) {
        failure.status = _("Saved snapshot does not match the current document.");
        failure.dialog = saved_stale_document_dialog_text(display_name, e.what(), save_notice);
        failure.severity = Inkscape::WARNING_MESSAGE;
    } catch (Inkscape::Extension::PublishedOlderRevision const &) {
        failure.older = true;
        failure.status = _("Newer changes remain unsaved.");
        failure.dialog = saved_older_revision_dialog_text(display_name, save_notice);
        failure.severity = Inkscape::WARNING_MESSAGE;
    } catch (Inkscape::Extension::Output::file_read_only const &) {
        failure.dialog = Glib::ustring::sprintf(
            _("File %s is write protected. Please remove write protection and try again."), display_name);
    } catch (Inkscape::Extension::Output::save_unsupported const &e) {
        failure.dialog = Glib::ustring::sprintf(
            _("File %s cannot safely be saved at that destination. The destination was not changed by this save attempt."), display_name);
        append_reason(e.error);
    } catch (Inkscape::Extension::Output::save_conflict const &e) {
        failure.dialog = Glib::ustring::sprintf(
            _("File %s changed or appeared while it was being saved, so it was not overwritten. Save to a different name or destination."), display_name);
        append_reason(e.error);
    } catch (Inkscape::Extension::Output::save_uncertain const &e) {
        failure.uncertain = true;
        failure.severity = Inkscape::WARNING_MESSAGE;
        failure.status = _("Save outcome uncertain; inspect the destination.");
        failure.dialog = Glib::ustring::sprintf(
            _("The save of %s may or may not have completed. The file may already have been written, but the current document remains marked unsaved. Inspect the destination before closing or retrying."), display_name);
        if (!e.recovery_path.empty()) {
            failure.dialog += "\n\n";
            failure.dialog += e.recovery_path_available
                ? Glib::ustring::sprintf(_("A possible retained copy is at %s; inspect it before deciding whether to retry."), Glib::ustring(e.recovery_path))
                : Glib::ustring::sprintf(_("A possible retained copy may be at %s; inspect it before deciding whether to retry."), Glib::ustring(e.recovery_path));
        }
        if (!e.error.empty()
            && (e.recovery_path.empty() || e.error.find(e.recovery_path) == std::string::npos)) {
            append_reason(e.error);
        }
    } catch (Inkscape::Extension::Output::save_failed const &e) {
        failure.dialog = Glib::ustring::sprintf(_("File %s could not be saved."), display_name);
        append_reason(e.error);
    } catch (Inkscape::Extension::Output::save_cancelled const &) {
    } catch (Inkscape::Extension::Output::export_id_not_found const &e) {
        failure.dialog = Glib::ustring::sprintf(_("File could not be saved:\nNo object with ID '%s' found."), e.id);
    } catch (Inkscape::Extension::Output::no_overwrite const &) {
        failure.status.clear();
        failure.retry = true;
    } catch (std::exception const &e) {
        failure.dialog = Glib::ustring::sprintf(_("File %s could not be saved.\n\n"
            "The following additional information was returned by the output extension:\n"
            "'%s'"), display_name, e.what());
    } catch (...) {
        failure.unspecified = true;
        failure.dialog = Glib::ustring::sprintf(_("File %s could not be saved."), display_name);
    }
    return failure;
}

std::pair<Glib::ustring, Glib::ustring>
classify_file_save_exception_for_testing(std::exception_ptr exception,
                                         Glib::ustring const &display_name,
                                         std::string &save_notice)
{
    auto failure = classify_file_save_exception(exception, display_name, save_notice);
    return {failure.status, failure.dialog};
}

unsigned classify_file_save_flags_for_testing(std::exception_ptr exception)
{
    std::string notice;
    auto const failure = classify_file_save_exception(exception, "drawing.svg", notice);
    return (failure.severity == Inkscape::WARNING_MESSAGE ? 1u : 0u)
        | (failure.retry ? 2u : 0u) | (failure.older ? 4u : 0u)
        | (failure.uncertain ? 8u : 0u) | (failure.unspecified ? 16u : 0u);
}

struct AsyncSaveHandoff {
    Inkscape::IO::FileOperationLease *lease = nullptr;
    bool started = false;
};

struct AsyncSaveState;
static std::unordered_map<SPDocument *, std::weak_ptr<AsyncSaveState>> deferred_async_saves;
static std::unordered_map<SPDocument *, std::weak_ptr<AsyncSaveState>> active_async_saves;

struct AsyncSaveState : std::enable_shared_from_this<AsyncSaveState> {
    SPDocument *doc;
    std::optional<Inkscape::IO::FileOperationLease> lease;
    std::optional<Inkscape::Extension::PendingSavePtr> pending;
    Glib::RefPtr<Gio::File> file;
    Glib::ustring display_name;
    Inkscape::Extension::FileSaveMethod method;
    bool official;
    bool remember_path;
    Gtk::Window *parent;
    InkscapeWindow::LifetimeToken window_token;
    SPDesktop *desktop;
    sigc::connection desktop_destroy;
    Inkscape::MessageId message = 0;
    std::string notice;
    std::optional<Inkscape::Extension::Internal::PublicationResult> deferred_result;
    bool finished = false;

    void report_unapplied()
    {
        if (finished) return;
        finished = true;
        desktop_destroy.disconnect();
        deferred_async_saves.erase(doc);
        active_async_saves.erase(doc);
        lease->complete({Inkscape::IO::FileOperationOutcome::Uncertain,
            "publication completion could not be applied before close"});
        lease.reset();
        if (auto *app = InkscapeApplication::instance()) app->publicationSettled(doc);
    }

    void finish(Inkscape::Extension::Internal::PublicationResult const &result, bool force_older)
    {
        if (finished) return;
        finished = true;
        desktop_destroy.disconnect();
        deferred_async_saves.erase(doc);
        active_async_saves.erase(doc);
        std::exception_ptr exception;
        try { Inkscape::Extension::finish_save_async(**pending, result, &notice, force_older); }
        catch (...) { exception = std::current_exception(); }
        if (force_older && doc->isModifiedSinceSave())
            DocumentUndo::markInteractionBaselineDirty(doc);
        SaveFailure failure;
        if (exception) failure = classify_file_save_exception(exception, display_name, notice);
        auto const outcome = !exception ? (!official || !doc->isModifiedSinceSave()
                ? Inkscape::IO::FileOperationOutcome::Success : Inkscape::IO::FileOperationOutcome::Failed)
            : failure.uncertain ? Inkscape::IO::FileOperationOutcome::Uncertain
            : failure.older ? Inkscape::IO::FileOperationOutcome::SavedOlderRevision
                            : Inkscape::IO::FileOperationOutcome::Failed;
        auto lifecycle = doc->saveLifecycleToken();
        lease->complete({outcome, outcome == Inkscape::IO::FileOperationOutcome::Success
                                          ? "document saved" : "background Save did not complete"});
        lease.reset();
        if (auto *app = InkscapeApplication::instance())
            app->publicationSettled(doc, outcome == Inkscape::IO::FileOperationOutcome::Success);
        if (lifecycle.expired()) return;
        if (exception) {
            if (desktop && desktop->messageStack() && !failure.status.empty())
                desktop->messageStack()->flash(failure.severity, failure.status);
            if (!failure.dialog.empty()) {
                if (Inkscape::IO::file_io_test_hooks_enabled()
                    && g_strcmp0(g_getenv("VACARDS_SAVE_TEST_CAPTURE_ASYNC_DIALOG"), "1") == 0)
                    { g_object_set_data(G_OBJECT(file->gobj()), "vacards-async-dialog-seen", GINT_TO_POINTER(1));
                      if (window_token.valid()) g_object_set_data(G_OBJECT(parent->gobj()), "vacards-async-dialog-seen", GINT_TO_POINTER(1)); }
                else sp_ui_error_dialog(failure.dialog.c_str());
            }
        } else {
            complete_file_save(window_token.valid() ? parent : nullptr, window_token, doc, desktop,
                               official, std::move(notice), file, method, remember_path, true);
        }
    }

    void complete(Inkscape::Extension::Internal::PublicationResult result)
    {
        // The worker is done with the snapshot; free it now even if completion waits.
        if (pending) Inkscape::Extension::release_publication_snapshot(**pending);
        if (desktop && desktop->messageStack() && message) desktop->messageStack()->cancel(message);
        message = 0;
        auto *app = InkscapeApplication::instance();
        if ((official || Inkscape::Extension::pending_save_may_alias_official(**pending))
            && !DocumentUndo::publicationCompletable(doc)
            && !(app && app->publicationWaitActive(doc))) {
            auto self = shared_from_this();
            deferred_result = result;
            if (app) app->publicationSettled(doc, std::nullopt);
            deferred_async_saves[doc] = self;
            if (!DocumentUndo::whenPublicationCompletable(doc,
                [self, result](SPDocument &) { self->finish(result, false); },
                [self](SPDocument &) { self->report_unapplied(); },
                [self] { return self->finished; })) self->report_unapplied();
            return;
        }
        finish(result, official && !DocumentUndo::publicationCompletable(doc));
    }

    void start_failed()
    {
        finished = true;
        desktop_destroy.disconnect();
        active_async_saves.erase(doc);
        if (desktop && desktop->messageStack()) {
            if (message) desktop->messageStack()->cancel(message);
            desktop->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not saved."));
        }
        lease->complete({Inkscape::IO::FileOperationOutcome::Failed,
                         "background Save could not start"});
        lease.reset();
        if (auto *app = InkscapeApplication::instance()) app->publicationSettled(doc);
    }
};

bool sp_file_pending_save_has_later_edits(SPDocument *doc)
{
    if (!doc || !Inkscape::IO::async_save_enabled()) return false;
    auto it = active_async_saves.find(doc);
    if (it == active_async_saves.end()) return false;
    auto state = it->second.lock();
    if (!state || state->finished || !state->pending) return false;
    return doc->getReprDoc()->contentRevision() !=
           Inkscape::Extension::pending_save_snapshot_revision(**state->pending);
}

bool sp_file_pending_save_is_official(SPDocument *doc)
{
    auto it = active_async_saves.find(doc);
    if (it == active_async_saves.end()) return false;
    auto state = it->second.lock();
    return state && !state->finished && state->official;
}

void sp_file_force_deferred_save(SPDocument *doc)
{
    auto it = deferred_async_saves.find(doc);
    if (it == deferred_async_saves.end()) return;
    auto state = it->second.lock();
    if (!state) { deferred_async_saves.erase(it); return; }
    if (state->deferred_result) state->finish(*state->deferred_result, true);
}

bool complete_file_save_without_window_for_testing(SPDocument *doc, Glib::RefPtr<Gio::File> const &file,
                                                   bool remember_path)
{
    if (!Inkscape::IO::file_io_test_hooks_enabled() || !doc || !file) return false;
    g_setenv("VACARDS_SAVE_TEST_CAPTURE_NOTICE_FALLBACK", "1", TRUE);
    complete_file_save(nullptr, {}, doc, nullptr, true, "retained recovery copy", file,
                       Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, remember_path);
    g_unsetenv("VACARDS_SAVE_TEST_CAPTURE_NOTICE_FALLBACK");
    return g_object_get_data(G_OBJECT(file->gobj()), "vacards-fallback-notice-seen") != nullptr;
}

// Native publication callbacks must return without opening a chooser or output.
// The continuation belongs to the document and revalidates its window binding.
static bool defer_bitmap_save(Gtk::Window &parent, SPDocument *doc,
                              std::function<void(InkscapeWindow &,SPDocument &)> action)
{
    if (!Inkscape::Bitmap::publicationBoundaryPending(doc)) return false;
    auto window=dynamic_cast<InkscapeWindow *>(&parent);
    if (!window) return true; // No lifetime-safe parent: refuse this legacy request.
    auto token=window->lifetimeToken();
    Inkscape::Bitmap::deferPublicationBoundary(doc,[window,token,action=std::move(action)](SPDocument &settled) {
        if (token.valid() && window->get_document()==&settled &&
            window->get_desktop() && window->get_desktop()->getDocument()==&settled) action(*window,settled);
    });
    return true;
}

/** This 'save' function is called by the other file-save entry points. */
static bool
file_save(Gtk::Window &parentWindow,
          SPDocument *doc,
          const Glib::RefPtr<Gio::File> file,
          Inkscape::Extension::Extension *key,
          bool checkoverwrite,
          bool official,
          Inkscape::Extension::FileSaveMethod save_method,
          bool native_save_requested = false,
          bool sync_title = false,
          SPDesktop *desktop = nullptr,
          bool *retry_with_different_name = nullptr,
          bool *publication_uncertain = nullptr,
          bool *saved_older_revision = nullptr,
          bool ordinary_save_lease = false,
          bool remember_path = false,
          AsyncSaveHandoff *handoff = nullptr)
{
    if (!doc) { //Safety check
        return false;
    }
    if (defer_bitmap_save(parentWindow,doc,[file,key,checkoverwrite,official,save_method,native_save_requested,
            sync_title,remember_path](InkscapeWindow &w,SPDocument &d) {
        file_save(w,&d,file,key,checkoverwrite,official,save_method,native_save_requested,sync_title,
                  w.get_desktop(),nullptr,nullptr,nullptr,false,remember_path);
    })) return false;
    auto *native_window = dynamic_cast<InkscapeWindow *>(&parentWindow);
    auto window_token = native_window ? native_window->lifetimeToken()
                                      : InkscapeWindow::LifetimeToken{};
    // A bound asynchronous explicit save owns the document's sole file-operation
    // slot while its chooser is open. Legacy synchronous entry points must not
    // publish a second save around that pending request.
    if (!desktop && !ordinary_save_lease && Inkscape::IO::DocumentFileOperation::hasActiveRequest(*doc)) {
        if (SP_ACTIVE_DESKTOP) {
            SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                _("Finish the open save operation before saving again."));
        }
        return false;
    }

    // Bound route: when a desktop is supplied every status flash uses it and the
    // overwrite confirmation is parented on parentWindow. The synchronous
    // callers leave desktop/retry_with_different_name at their defaults, so they
    // keep reading SP_ACTIVE_DESKTOP and keep the no_overwrite chooser recursion.
    auto flash_desktop = [desktop]() -> SPDesktop * {
        return desktop ? desktop : SP_ACTIVE_DESKTOP;
    };
    if (retry_with_different_name) {
        *retry_with_different_name = false;
    }
    if (publication_uncertain) {
        *publication_uncertain = false;
    }
    if (saved_older_revision) {
        *saved_older_revision = false;
    }
    Inkscape::Extension::OverwriteConfirm confirm_overwrite;
    if (desktop) {
        confirm_overwrite = [&parentWindow](std::string const &filename) {
            return sp_ui_overwrite_file(filename, &parentWindow);
        };
    }

    auto path = file->get_path();
    auto display_name = file->get_parse_name();

    std::string save_notice;
    try {
        if (handoff && handoff->lease && Inkscape::IO::async_save_enabled() && native_save_requested) {
            auto pending = Inkscape::Extension::begin_save_async(key, doc, file->get_path().c_str(),
                checkoverwrite, official, save_method, confirm_overwrite, sync_title,
                &save_notice, native_save_requested);
            Inkscape::Extension::Internal::PublicationResult direct_result;
            auto job = Inkscape::Extension::take_publication_job(*pending, &direct_result);
            if (!job) {
                Inkscape::Extension::finish_save_async(*pending, direct_result, &save_notice, false);
            } else {
                auto state = std::make_shared<AsyncSaveState>();
                state->doc = doc;
                state->pending = std::move(pending);
                state->file = file;
                state->display_name = display_name;
                state->method = save_method;
                state->official = official;
                state->remember_path = remember_path;
                state->parent = &parentWindow;
                state->window_token = window_token;
                state->desktop = desktop ? desktop : SP_ACTIVE_DESKTOP;
                state->notice = std::move(save_notice);
                handoff->lease->enterPublication();
                if (state->desktop) {
                    state->desktop_destroy = state->desktop->connectDestroy([weak = std::weak_ptr(state)](SPDesktop *) {
                        if (auto alive = weak.lock()) alive->desktop = nullptr;
                    });
                    if (auto *stack = state->desktop->messageStack())
                        state->message = stack->push(Inkscape::NORMAL_MESSAGE, _("Saving document…"));
                }
                state->lease.emplace(std::move(*handoff->lease));
                active_async_saves[doc] = state;
                handoff->started = true;
                InkscapeApplication::instance()->publicationStarted(doc, file->get_path());
                try {
                    if (InkscapeApplication::instance()->publications().start(doc, std::move(*job),
                            [state](auto result) { state->complete(std::move(result)); })
                        == Inkscape::IO::PublicationWorkerRegistry::StartResult::Busy) {
                        g_warning("Background Save publication slot is busy");
                        state->start_failed();
                    }
                } catch (...) {
                    state->start_failed();
                }
                return false;
            }
        } else {
            Inkscape::Extension::save(key, doc, file->get_path().c_str(),
                                      checkoverwrite, official, save_method,
                                      confirm_overwrite, sync_title, &save_notice,
                                      native_save_requested);
        }
    } catch (...) {
        auto const failure = classify_file_save_exception(
            std::current_exception(), display_name, save_notice);
        if (failure.unspecified)
            g_critical("Extension '%s' threw an unspecified exception.", key ? key->get_id() : nullptr);
        if (failure.older && saved_older_revision) *saved_older_revision = true;
        if (failure.uncertain && publication_uncertain) *publication_uncertain = true;
        if (failure.retry) {
            if (retry_with_different_name) {
                // Declined overwrite: return before complete_file_save, so
                // neither Recent nor the saved-path preference is updated.
                *retry_with_different_name = true;
                return false;
            }
            return sp_file_save_dialog(parentWindow, doc, save_method);
        }
        if (!failure.status.empty()) {
            if (auto *target = flash_desktop(); target && target->messageStack())
                target->messageStack()->flash(failure.severity, failure.status);
        }
        if (!failure.dialog.empty()) sp_ui_error_dialog(failure.dialog.c_str());
        return false;
    }

    return complete_file_save(&parentWindow, window_token, doc, desktop, official,
                              std::move(save_notice), file, save_method, remember_path);
}

/**
 * Returns an output extension suitable for saving (i.e. not a raster extension).
 */
Inkscape::Extension::Output *get_output_extension_for_save(std::string filename)
{
    Inkscape::Extension::DB::OutputList extension_list;
    Inkscape::Extension::db.get_output_list(extension_list);

    for (auto omod : extension_list) {
        if (omod->can_save_filename(filename.c_str())) {
            return omod;
        }
    }

    return nullptr;
}

/**
 * Native editable Inkscape SVG suffix used when the chooser returns a
 * suffixless filename. Shared by the synchronous dialog and the bound helper so
 * the fallback is resolved in exactly one place.
 */
static std::string
file_save_default_extension()
{
    auto *default_save_output = dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get(SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE));
    return default_save_output ? default_save_output->get_extension() : std::string(".svg");
}

/**
 * Complete a Save/Save As/Save Copy after the Save chooser returned a file.
 *
 * This is the tail previously inlined in sp_file_save_dialog(): it normalizes
 * the chosen suffix, resolves the output module, performs the single file_save()
 * call and, on success, updates the Recent list and the remembered save-path
 * preference.  The caller keeps ownership of the chooser, its exceptions and
 * the user-cancellation check.
 *
 * \param parentWindow bound window passed through to file_save()
 * \param doc document being saved
 * \param file chosen Gio::File (non-null)
 * \param save_method Save/Save As/Save Copy; drives the official flag
 * \param default_save_extension native editable SVG suffix for suffixless names
 * \param desktop bound desktop for status flashes; null keeps SP_ACTIVE_DESKTOP
 * \param retry_with_different_name bound-route no_overwrite signal; null keeps
 *        the synchronous sp_file_save_dialog recursion
 * \param publication_uncertain records ambiguous publication for the bound route
 * \return true only when file_save() reported success
 */
static bool
file_save_dialog_apply(Gtk::Window &parentWindow,
                       SPDocument *doc,
                       Glib::RefPtr<Gio::File> file,
                       Inkscape::Extension::FileSaveMethod save_method,
                       std::string const &default_save_extension,
                       SPDesktop *desktop = nullptr,
                       bool *retry_with_different_name = nullptr,
                       bool *publication_uncertain = nullptr,
                       bool *saved_older_revision = nullptr,
                       bool ordinary_save_lease = false,
                       AsyncSaveHandoff *handoff = nullptr)
{
    bool const is_copy = (save_method == Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY);

    // The svg:title -> RDF title normalization for this dialog path is applied
    // to the output copy (and committed to the live document only on a confirmed
    // official save) by the save transaction; it must never mutate the original
    // before the write succeeds, and never for Save Copy.

    // Normalize the chooser-returned path before resolving the output module and
    // before file_save() raises the overwrite confirmation, so the confirmation
    // (sp_ui_overwrite_file) and the recent-file entry name the file that will
    // actually be written. The native chooser may have prompted for the raw
    // suffixless name first; that prompt cannot be controlled from here. An empty
    // path (e.g. a non-local/unmounted URI) is never turned into a ".svg" in the
    // current directory.
    if (!file->get_path().empty()) {
        std::string save_target_path = file->get_path();
        if (Inkscape::IO::append_save_extension_if_missing(save_target_path, default_save_extension)) {
            file = Gio::File::create_for_path(save_target_path);
        }
    }

    // Find output module from file extension.
    auto *extension = get_output_extension_for_save(file->get_path());

    if (!extension) {
        auto display_name = file->get_parse_name();
        auto const text = Glib::ustring::sprintf(_("No VA Studio extension found to save document (%s).  This may have been caused by an unknown or missing filename extension."), display_name);
        (desktop ? desktop : SP_ACTIVE_DESKTOP)->messageStack()->flash(Inkscape::ERROR_MESSAGE, _("Document not saved."));
        sp_ui_error_dialog(text.c_str());
        return false;
    }

    if (file_save(parentWindow, doc, file, extension, true, !is_copy, save_method, /*native_save_requested=*/true, /*sync_title=*/true,
                  desktop, retry_with_different_name, publication_uncertain, saved_older_revision,
                  ordinary_save_lease, true, handoff)) {
        return true;
    }

    return false;
}

/**
 *  Display a SaveAs dialog.  Save the document if OK pressed.
 */
static std::string file_save_chooser_path_for_testing;

void set_file_save_chooser_path_for_testing(std::string path)
{
    file_save_chooser_path_for_testing = std::move(path);
}

std::string take_file_save_chooser_path_for_testing()
{
    if (!Inkscape::IO::file_io_test_hooks_enabled()) return {};
    return std::exchange(file_save_chooser_path_for_testing, {});
}

static bool
sp_file_save_dialog_impl(Gtk::Window &parentWindow, SPDocument *doc,
                         Inkscape::Extension::FileSaveMethod save_method,
                         Inkscape::IO::FileOperationLease *ordinary_lease = nullptr,
                         AsyncSaveHandoff *handoff = nullptr,
                         SPDesktop *desktop = nullptr)
{
    if (!doc) return false;
    if (!ordinary_lease && Inkscape::IO::DocumentFileOperation::hasActiveRequest(*doc)) {
        if (SP_ACTIVE_DESKTOP) {
            SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                _("Finish the open save operation before saving again."));
        }
        return false;
    }
    bool is_copy = (save_method == Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY);

    // Note: default_extension has the format "org.inkscape.output.svg.inkscape",
    //       whereas filename_extension only uses ".svg"
    auto default_extension = Inkscape::Extension::get_file_save_extension(save_method);
    auto extension = dynamic_cast<Inkscape::Extension::Output *>(Inkscape::Extension::db.get(default_extension.c_str()));

    std::string filename_extension = ".svg";
    if (extension) {
        filename_extension = extension->get_extension(); // Glib::ustring -> std::string FIXME
    }

    // Fallback for a native chooser that returns a suffixless filename (its
    // default filter is "All Files", so the panel need not append anything).
    // Such a save must land in the native editable Inkscape SVG, regardless of
    // the format used for a previous save.
    std::string const default_save_extension = file_save_default_extension();

    std::string save_path = Inkscape::Extension::get_file_save_path(doc, save_method); // Glib::ustring -> std::string FIXME

    if (!Inkscape::IO::file_test(save_path.c_str(), (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))) {
        save_path.clear();
    }

    if (save_path.empty()) {
        save_path = Glib::get_home_dir();
    }

    std::string save_loc = save_path;
    save_loc.append(G_DIR_SEPARATOR_S);

    int i = 1;
    if ( !doc->getDocumentFilename() ) {
        // We are saving for the first time; create a unique default filename
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        Glib::ustring default_filename = prefs->getString("/options/defaultfilename/value", _("drawing"));
        save_loc = save_loc + default_filename + filename_extension;

        while (Inkscape::IO::file_test(save_loc.c_str(), G_FILE_TEST_EXISTS)) {
            save_loc = save_path;
            save_loc.append(G_DIR_SEPARATOR_S);
            save_loc = save_loc + default_filename + Glib::ustring::compose("-%1", i++) + filename_extension;
        }
    } else {
        save_loc.append(Glib::path_get_basename(doc->getDocumentFilename()));

        // The current document might be named not after an svg - e.g. if we opened a png, the
        // document is named after the png. But we want to save it as an svg instead.
        // Or even if the filename is something weird like "my-drawing.blarg" - we don't want to
        // try to save as a ".blarg" file.
        // So we'll swap out whatever the current file-ending is and prompt the user to save with
        // the correct target filename extension.
        Inkscape::IO::swap_file_extension(save_loc, filename_extension);
    }

    // Show the SaveAs dialog.
    const Glib::ustring dialog_title = is_copy ?
        _("Select file to save a copy to") :
        _("Select file to save to");

    // Note, there are currently multiple modules per filename extension (.svg, .dxf, .zip).
    // We cannot distinguish between them.
    std::string basename = Glib::path_get_basename(save_loc);
    std::string dirname = Glib::path_get_dirname(save_loc);
    Glib::RefPtr<Gio::File> file;
    try {
        if (Inkscape::IO::file_io_test_hooks_enabled() && !file_save_chooser_path_for_testing.empty()) {
            file = Gio::File::create_for_path(file_save_chooser_path_for_testing);
            file_save_chooser_path_for_testing.clear();
        } else {
            file = Inkscape::choose_file_save(dialog_title, &parentWindow,
                                              Inkscape::UI::Dialog::create_export_filters(true),
                                              basename, dirname);
        }
    } catch (std::exception const &error) {
        auto const text = Glib::ustring::sprintf(
            _("The Save dialog could not complete: %s"), error.what());
        sp_ui_error_dialog(text.c_str());
        return false;
    }

    if (!file) {
        return false; // Cancelled
    }

    if (ordinary_lease && ordinary_lease->authorizeOutput(ordinary_lease->context()).outcome
                              != Inkscape::IO::FileOperationOutcome::Success) return false;
    bool retry_with_different_name = false;
    bool const saved = file_save_dialog_apply(parentWindow, doc, file, save_method,
        default_save_extension, desktop,
        ordinary_lease ? &retry_with_different_name : nullptr,
        nullptr, nullptr, ordinary_lease != nullptr, handoff);
    if (retry_with_different_name)
        return sp_file_save_dialog_impl(parentWindow, doc, save_method, ordinary_lease, handoff, desktop);
    return saved;
}

bool sp_file_save_dialog(Gtk::Window &parentWindow, SPDocument *doc,
                         Inkscape::Extension::FileSaveMethod save_method)
{
    if (defer_bitmap_save(parentWindow,doc,[save_method](InkscapeWindow &w,SPDocument &d) {
        sp_file_save_dialog(w,&d,save_method);
    })) return false;
    return sp_file_save_dialog_impl(parentWindow, doc, save_method);
}

/**
 * Save an already-selected file against a bound document/desktop/window.
 *
 * This is the bound-route counterpart of sp_file_save_dialog(): it reuses
 * file_save_dialog_apply (suffix normalization, output-extension resolution,
 * Recent update and save-path preference) but opens no chooser. Status and
 * overwrite confirmation use the bound context; the legacy error dialog still
 * chooses its transient parent globally. A declined overwrite (Output::
 * no_overwrite) is reported as RetryWithDifferentName so the caller can re-run
 * its own asynchronous chooser.
 */
static FileSaveResult complete_bound_save(bool saved, bool publication_uncertain,
                                          bool saved_older_revision, bool retry_with_different_name)
{
    if (saved) {
        return FileSaveResult::Saved;
    }
    if (publication_uncertain) return FileSaveResult::Uncertain;
    if (saved_older_revision) return FileSaveResult::SavedOlderRevision;
    return retry_with_different_name ? FileSaveResult::RetryWithDifferentName
                                     : FileSaveResult::Failed;
}

FileSaveResult
sp_file_save_bound(Gtk::Window &parentWindow, SPDocument *doc, SPDesktop *desktop,
                   Glib::RefPtr<Gio::File> file,
                   Inkscape::Extension::FileSaveMethod save_method,
                   Inkscape::IO::FileOperationLease *lease)
{
    if (!doc || !file || (!desktop && !(lease && Inkscape::IO::file_io_test_hooks_enabled())) ||
        (desktop && (desktop->getDocument() != doc || desktop->getInkscapeWindow() != &parentWindow))) {
        return FileSaveResult::Failed;
    }

    bool retry_with_different_name = false;
    bool publication_uncertain = false;
    bool saved_older_revision = false;
    AsyncSaveHandoff handoff{lease};
    bool const saved = file_save_dialog_apply(parentWindow, doc, file, save_method,
                                              file_save_default_extension(),
                                              desktop, &retry_with_different_name,
                                              &publication_uncertain, &saved_older_revision,
                                              lease != nullptr, &handoff);
    if (handoff.started) return FileSaveResult::InProgress;
    return complete_bound_save(saved, publication_uncertain, saved_older_revision,
                               retry_with_different_name);
}

/**
 * Save a document, displaying a SaveAs dialog if necessary.
 */
bool
sp_file_save_document(Gtk::Window &parentWindow, SPDocument *doc, bool allow_async, SPDesktop *desktop)
{
    if (!doc) return false;
    if (defer_bitmap_save(parentWindow,doc,[allow_async](InkscapeWindow &w,SPDocument &d) {
        sp_file_save_document(w,&d,allow_async,w.get_desktop());
    })) return false;
    if (auto path = doc->getDocumentFilename()) {
        std::string const saved_path = path;
        // Determine the extension from the filename, which may not lead to a valid extension.
        // In which case, we'll fall back to the save-as dialog below.
        if (auto ext = get_output_extension_for_save(saved_path)) {
            Inkscape::IO::FileOperationResult refusal;
            auto const lifecycle = doc->saveLifecycleToken();
            auto const generation = doc->saveInstanceGeneration();
            auto lease = Inkscape::IO::DocumentFileOperation::admit(*doc,
                {Inkscape::IO::FileOperationMethod::Save, saved_path, "ordinary Save"},
                [doc, lifecycle, generation, saved_path] {
                    return !lifecycle.expired() && doc->saveInstanceGeneration() == generation
                        && g_strcmp0(doc->getDocumentFilename(), saved_path.c_str()) == 0;
                }, &refusal);
            if (!lease) {
                if (SP_ACTIVE_DESKTOP && SP_ACTIVE_DESKTOP->getDocument() == doc)
                    SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                        save_admission_refusal_text(refusal));
                return false;
            }
            auto const context = lease->context();
            if (lease->authorizeOutput(context).outcome != Inkscape::IO::FileOperationOutcome::Success) {
                lease->complete({Inkscape::IO::FileOperationOutcome::StaleTarget, "ordinary Save target changed"});
                return false;
            }
            auto file = Gio::File::create_for_path(saved_path);
            bool publication_uncertain = false;
            bool saved_older_revision = false;
            AsyncSaveHandoff handoff{allow_async ? &*lease : nullptr};
            bool const saved = file_save(parentWindow, doc, file, ext, false, true,
                Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS, /*native_save_requested=*/true,
                false, desktop, nullptr, &publication_uncertain, &saved_older_revision, true,
                false, &handoff);
            if (handoff.started) return false;
            auto const outcome = saved ? Inkscape::IO::FileOperationOutcome::Success
                : publication_uncertain ? Inkscape::IO::FileOperationOutcome::Uncertain
                : saved_older_revision ? Inkscape::IO::FileOperationOutcome::SavedOlderRevision
                                       : Inkscape::IO::FileOperationOutcome::Failed;
            lease->complete({outcome, saved ? "document saved" : "ordinary Save did not complete"});
            return saved;
        }
    }

    // In this case `path == nullptr`, therefore, an argument should be given
    // that indicates that the document is the firsttime saved,
    // so that .svg is selected as the default
    // and not the last one "Save as ..." extension used
    Inkscape::IO::FileOperationResult refusal;
    auto const lifecycle = doc->saveLifecycleToken();
    auto const generation = doc->saveInstanceGeneration();
    std::string const original_path = doc->getDocumentFilename() ? doc->getDocumentFilename() : "";
    auto lease = Inkscape::IO::DocumentFileOperation::admit(*doc,
        {Inkscape::IO::FileOperationMethod::Save, original_path, "ordinary Save chooser"},
        [doc, lifecycle, generation, original_path] {
            return !lifecycle.expired() && doc->saveInstanceGeneration() == generation
                && std::string(doc->getDocumentFilename() ? doc->getDocumentFilename() : "") == original_path;
        }, &refusal);
    if (!lease) {
        if (SP_ACTIVE_DESKTOP && SP_ACTIVE_DESKTOP->getDocument() == doc)
            SP_ACTIVE_DESKTOP->messageStack()->flash(Inkscape::WARNING_MESSAGE,
                save_admission_refusal_text(refusal));
        return false;
    }
    AsyncSaveHandoff handoff{allow_async ? &*lease : nullptr};
    bool const saved = sp_file_save_dialog_impl(parentWindow, doc,
        Inkscape::Extension::FILE_SAVE_METHOD_INKSCAPE_SVG, &*lease, &handoff, desktop);
    if (handoff.started) return false;
    lease->complete({saved ? Inkscape::IO::FileOperationOutcome::Success
                           : Inkscape::IO::FileOperationOutcome::Failed,
                     saved ? "document saved" : "ordinary Save chooser did not complete"});
    return saved;
}

/**
 * Save a document.
 */
bool
sp_file_save(Gtk::Window &parentWindow, gpointer /*object*/, gpointer /*data*/)
{
    auto *desktop = dynamic_cast<InkscapeWindow *>(&parentWindow)
        ? dynamic_cast<InkscapeWindow &>(parentWindow).get_desktop() : SP_ACTIVE_DESKTOP;
    auto *document = desktop ? desktop->getDocument() : SP_ACTIVE_DOCUMENT;
    if (!document || !desktop) {
        return false;
    }

    desktop->messageStack()->flash(Inkscape::IMMEDIATE_MESSAGE, _("Saving document..."));

    if (defer_bitmap_save(parentWindow,document,[](InkscapeWindow &w,SPDocument &) {
        sp_file_save(w,nullptr,nullptr);
    })) return false;

    sp_namedview_document_from_window(desktop);
    return sp_file_save_document(parentWindow, document, true, desktop);
}

/**
 *  Save a document, always displaying the SaveAs dialog.
 */
bool
sp_file_save_as(Gtk::Window &parentWindow, gpointer /*object*/, gpointer /*data*/)
{
    if (!SP_ACTIVE_DOCUMENT) {
        return false;
    }

    if (defer_bitmap_save(parentWindow,SP_ACTIVE_DOCUMENT,[](InkscapeWindow &w,SPDocument &d) {
        sp_namedview_document_from_window(w.get_desktop());
        sp_file_save_dialog(w,&d,Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS);
    })) return false;

    sp_namedview_document_from_window(SP_ACTIVE_DESKTOP);
    return sp_file_save_dialog(parentWindow, SP_ACTIVE_DOCUMENT, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS);
}

/**
 *  Save a copy of a document, always displaying a sort of SaveAs dialog.
 */
bool
sp_file_save_a_copy(Gtk::Window &parentWindow, gpointer /*object*/, gpointer /*data*/)
{
    if (!SP_ACTIVE_DOCUMENT) {
        return false;
    }

    if (defer_bitmap_save(parentWindow,SP_ACTIVE_DOCUMENT,[](InkscapeWindow &w,SPDocument &d) {
        sp_namedview_document_from_window(w.get_desktop());
        sp_file_save_dialog(w,&d,Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY);
    })) return false;

    sp_namedview_document_from_window(SP_ACTIVE_DESKTOP);
    return sp_file_save_dialog(parentWindow, SP_ACTIVE_DOCUMENT, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY);
}

/**
 *  Save a copy of a document as template.
 */
bool
sp_file_save_template(Gtk::Window &parentWindow, Glib::ustring name,
    Glib::ustring author, Glib::ustring description, Glib::ustring keywords,
    bool isDefault)
{
    if (!SP_ACTIVE_DOCUMENT || name.length() == 0)
        return true;

    auto document = SP_ACTIVE_DOCUMENT;
    if (Inkscape::Bitmap::publicationBoundaryPending(document)) return false;

    DocumentUndo::ScopedInsensitive _no_undo(document);

    auto root = document->getReprRoot();
    auto xml_doc = document->getReprDoc();

    auto templateinfo_node = xml_doc->createElement("inkscape:templateinfo");
    Inkscape::GC::release(templateinfo_node);

    auto element_node = xml_doc->createElement("inkscape:name");
    Inkscape::GC::release(element_node);

    element_node->appendChild(xml_doc->createTextNode(name.c_str()));
    templateinfo_node->appendChild(element_node);

    if (author.length() != 0) {

        element_node = xml_doc->createElement("inkscape:author");
        Inkscape::GC::release(element_node);

        element_node->appendChild(xml_doc->createTextNode(author.c_str()));
        templateinfo_node->appendChild(element_node);
    }

    if (description.length() != 0) {

        element_node = xml_doc->createElement("inkscape:shortdesc");
        Inkscape::GC::release(element_node);

        element_node->appendChild(xml_doc->createTextNode(description.c_str()));
        templateinfo_node->appendChild(element_node);

    }

    element_node = xml_doc->createElement("inkscape:date");
    Inkscape::GC::release(element_node);

    element_node->appendChild(xml_doc->createTextNode(
        Glib::DateTime::create_now_local().format("%F").c_str()));
    templateinfo_node->appendChild(element_node);

    if (keywords.length() != 0) {

        element_node = xml_doc->createElement("inkscape:keywords");
        Inkscape::GC::release(element_node);

        element_node->appendChild(xml_doc->createTextNode(keywords.c_str()));
        templateinfo_node->appendChild(element_node);

    }

    root->appendChild(templateinfo_node);

    // Escape filenames for windows users, but filenames are not URIs so
    // Allow UTF-8 and don't escape spaces which are popular chars.
    auto encodedName = Glib::uri_escape_string(name, " ", true);
    encodedName.append(".svg");

    auto path = Inkscape::IO::Resource::get_path_string(USER, TEMPLATES, encodedName.c_str());

    auto operation_confirmed = sp_ui_overwrite_file(path);

    auto file = Gio::File::create_for_path(path);

    if (operation_confirmed) {
        file_save(parentWindow, document, file,
            Inkscape::Extension::db.get(".svg"), false, false,
            Inkscape::Extension::FILE_SAVE_METHOD_INKSCAPE_SVG);

        if (isDefault) {
            // save as "default.svg" by default (so it works independently of UI language), unless
            // a localized template like "default.de.svg" is already present (which overrides "default.svg")
            std::string default_svg_localized = std::string("default.") + _("en") + ".svg";
            path = Inkscape::IO::Resource::get_path_string(USER, TEMPLATES, default_svg_localized.c_str());

            if (!Inkscape::IO::file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
                path = Inkscape::IO::Resource::get_path_string(USER, TEMPLATES, "default.svg");
            }

            file = Gio::File::create_for_path(path);
            file_save(parentWindow, document, file,
                Inkscape::Extension::db.get(".svg"), false, false,
                Inkscape::Extension::FILE_SAVE_METHOD_INKSCAPE_SVG);
        }
    }

    // remove this node from current document after saving it as template
    root->removeChild(templateinfo_node);

    return operation_confirmed;
}

/*######################
## I M P O R T
######################*/

/**
 * Paste the contents of a document into the active desktop.
 * @param clipdoc The document to paste
 * @param in_place Whether to paste the selection where it was when copied
 * @pre @c clipdoc is not empty and items can be added to the current layer
 */
void sp_import_document(SPDesktop *desktop, SPDocument *clipdoc, bool in_place, bool on_page)
{
    SPDocument *target_document = desktop->getDocument();
    Inkscape::XML::Node *root = clipdoc->getReprRoot();
    auto layer = desktop->layerManager().currentLayer();
    Inkscape::XML::Node *target_parent = layer->getRepr();

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    // Get page manager for on_page pasting, this must be done before selection changes
    Inkscape::PageManager &pm = target_document->getPageManager();
    SPPage *to_page = pm.getSelected();
    Geom::OptRect from_page;
    Inkscape::XML::Node *clipboard = sp_repr_lookup_name(root, "inkscape:clipboard", 1);
    if (clipboard && clipboard->attribute("page-min")) {
        from_page = Geom::OptRect(clipboard->getAttributePoint("page-min"), clipboard->getAttributePoint("page-max"));
    }

    auto *node_after = desktop->getSelection()->topRepr();
    if (node_after && prefs->getBool("/options/paste/aboveselected", true) && node_after != target_parent) {
        target_parent = node_after->parent();

        // find parent group
        for (auto p = target_document->getObjectByRepr(node_after->parent()); p; p = p->parent) {
            if (auto parent_group = cast<SPGroup>(p)) {
                layer = parent_group;
                break;
            }
        }
    } else {
        node_after = target_parent->lastChild();
    }

    Geom::Point offset(0, 0);
    Geom::Rect bbox;
    if (clipboard) {
        Geom::Point min, max;
        min = clipboard->getAttributePoint("min", min);
        max = clipboard->getAttributePoint("max", max);
        bbox = Geom::Rect(min, max) * target_document->dt2doc();
        offset = bbox.min();
    }
    if (!in_place) {
        auto &m = desktop->getNamedView()->snap_manager;
        m.setup(desktop);
        desktop->getTool()->discard_delayed_snap_event();

        // Get offset from mouse pointer to bbox center, snap to grid if enabled
        auto cursor_position = desktop->point() * target_document->dt2doc();
        auto snap_shift = m.multipleOfGridPitch(cursor_position - bbox.midpoint(), bbox.midpoint());
        offset += snap_shift;
        m.unSetup();
    }
    if (on_page && from_page && to_page) {
        auto page_offset = to_page->getDocumentRect().min() - (from_page.value() * target_document->dt2doc()).min();
        offset += page_offset;
    }
    Geom::Affine transform = Geom::Translate(offset);

    // copy objects
    std::vector<Inkscape::XML::Node *> pasted_objects;
    target_document->import(*clipdoc, layer->getRepr(), node_after, transform, &pasted_objects);

    target_document->ensureUpToDate();
    Inkscape::Selection *selection = desktop->getSelection();
    // Change the selection to the freshly pasted objects
    selection->setReprList(pasted_objects);
    target_document->emitReconstructionFinish();
}

/**
 *  Import a resource.  Called by document_import() and Drag and Drop.
 *  The only place 'key' is used non-null is in drag-and-drop of a GDK_TYPE_TEXTURE.
 *
 *  `destination_window` is the window the import was issued on when the caller
 *  has one. Both the insertion layer and the selection update require a window
 *  whose document is still `in_doc`: a stale/foreign window would otherwise add
 *  a foreign-document child under another document's layer. With no explicit
 *  window the process active desktop is used, which keeps drag-and-drop, the
 *  import dialog and scripted imports working as before.
 */
SPObject *file_import(SPDocument *in_doc, std::string const &path, Inkscape::Extension::Extension *key,
                      std::optional<Geom::Point> drop_pos, SPDesktop *destination_window)
{
    bool const explicit_destination = destination_window != nullptr;
    SPDesktop *const desktop = explicit_destination ? destination_window : SP_ACTIVE_DESKTOP;
    // A window is a usable destination only while it still shows `in_doc`. This
    // is evaluated now, while `desktop` is known live: it is either an explicit
    // window the caller had just revalidated (clipboard image paste) or the
    // process active desktop read here. It is re-checked at every later use
    // through `window_is_live_destination()`, never assumed.
    bool const destination_shows_doc = desktop && desktop->getDocument() == in_doc;
    bool cancelled = false;
    auto prefs = Inkscape::Preferences::get();

    // Store mouse pointer location before opening any dialogs, so we can drop the item where initially intended.
    // The desktop pointer may be null when the last window was closed while a nested clipboard wait was
    // running (see ClipboardManagerImpl::_pasteImage): never dereference it blindly. value_or() would still
    // evaluate desktop->point(), so the check happens before the fallback is chosen.
    auto pointer_location = drop_pos ? *drop_pos : (desktop ? desktop->point() : Geom::Point());

    // True only while the process still reports the validated pointer as its
    // active desktop. Re-reading SP_ACTIVE_DESKTOP and comparing pointers is what
    // makes later uses safe: a window destroyed by a nested loop is no longer in
    // the active list, so the captured pointer is never dereferenced as a
    // liveness proof of its own.
    auto const window_is_live_destination = [&] {
        SPDesktop *const active = SP_ACTIVE_DESKTOP;
        return destination_shows_doc && active && active == desktop;
    };

    // We need access to the module locally for our import logic
    if (!key) {
        key = Inkscape::Extension::Input::find_by_filename(path.c_str());
    }

    // DEBUG_MESSAGE( fileImport, "file_import( in_doc:%p uri:[%s], key:%p", in_doc, uri, key );
    std::unique_ptr<SPDocument> doc;
    try {
        doc = Inkscape::Extension::open(key, path.c_str(), true);
    } catch (Inkscape::Extension::Input::open_damaged const &damage) {
        auto text = Glib::ustring::compose(
            _("Could not import %1: the SVG is incomplete or damaged. %2"),
            path, damage.what());
        sp_ui_error_dialog(text.c_str());
        return nullptr;
    } catch (Inkscape::Extension::Input::no_extension_found const &) {
    } catch (Inkscape::Extension::Input::open_failed const &) {
    } catch (Inkscape::Extension::Input::open_cancelled const &) {
        cancelled = true;
    } catch (Glib::Error const &e) {
        auto text = Glib::ustring::compose(_("Could not import %1: %2"), path, e.what());
        sp_ui_error_dialog(text.c_str());
        return nullptr;
    } catch (std::exception const &e) {
        // Importers may throw std::bad_alloc, std::out_of_range, Glib::Error (a std::exception)...
        // Callers run inside GTK callbacks, where an escaping exception ends the process.
        auto text = Glib::ustring::compose(_("Could not import %1: %2"), path, e.what());
        sp_ui_error_dialog(text.c_str());
        return nullptr;
    } catch (...) {
        auto text = Glib::ustring::compose(_("Could not import %1: %2"), path, _("unknown error"));
        sp_ui_error_dialog(text.c_str());
        return nullptr;
    }

    bool is_svg = !key || !strcmp(key->get_id(), SP_MODULE_KEY_INPUT_SVG);

    if (!doc) {
        // Open failed or canceled
        if (!cancelled) {
            auto text = Glib::ustring::sprintf(_("Failed to load the requested file %s"), path);
            sp_ui_error_dialog(text.c_str());
        }
        return nullptr;
    }

    if (is_svg && prefs->getString("/dialogs/import/import_mode_svg") == "new") {
        // Special case: "SVG Import mode" is set to "New"
        // (open imported/drag-and-dropped SVGs as new file, do not import them into the current document)
        // --> open and return nothing
        auto *app = InkscapeApplication::instance();
        auto doc_ptr = app->document_add(std::move(doc));
        app->desktopOpen(doc_ptr);
        return nullptr;
    }
    // The extension should set it's pages enabled or disabled when opening
    // in order to indicate if pages are being imported or if objects are.
    if (doc->getPageManager().hasPages()) {
        file_import_pages(in_doc, doc.get());
        DocumentUndo::done(in_doc, RC_("Undo", "Import Pages"), INKSCAPE_ICON("document-import"));
        // This return is only used by dbus in document-interface.cpp (now removed).
        return nullptr;
    }

    // Standard case: Import

    // Determine the place to insert the new object.
    // This will be the current layer, if possible.
    // FIXME: If there's no desktop (command line run?) we need
    //        a document:: method to return the current layer.
    //        For now, we just use the root in this case.
    //
    // The importer above may have opened nested loops (dialogs), so the window
    // pointer captured at entry is not a liveness proof by itself; the lambda
    // re-reads the active list for that. A window that is gone, replaced or
    // showing another document donates no layer, and the choice is additionally
    // required to belong to `in_doc`: the import can never be parented into
    // another document's tree.
    SPObject *place_to_insert = nullptr;
    if (window_is_live_destination()) {
        place_to_insert = desktop->layerManager().currentLayer();
    }
    if (!place_to_insert || place_to_insert->document != in_doc) {
        place_to_insert = in_doc->getRoot();
    }

    std::vector<Inkscape::XML::Node *> result;

    doc->ensureUpToDate();
    auto const bbox = doc->getRoot()->desktopPreferredBounds().value_or(Geom::Rect());
    auto const bbox_doc = bbox * doc->dt2doc();
    Geom::Affine transform = Geom::Translate(pointer_location * in_doc->dt2doc() - bbox_doc.midpoint());

    in_doc->import(*doc, place_to_insert->getRepr(), nullptr, transform, &result,
                   is_svg ? SPDocument::ImportRoot::Single
                          : SPDocument::ImportRoot::UngroupSingle, // remove groups for imported bitmap images
                   SPDocument::ImportLayersMode::ToGroup);

    SPObject *import_root = nullptr;
    if (!result.empty()) {
        g_assert(result.size() == 1);
        import_root = in_doc->getObjectByRepr(result[0]);
    }
    // Selection update: the same live-destination revalidation as the layer
    // choice above, repeated because the import itself ran in between. A window
    // destroyed by a nested importer loop is no longer in the active list and is
    // never dereferenced; when no window is active (command-line run) nothing is
    // selected, as before.
    if (window_is_live_destination()) {
        Inkscape::Selection *selection = desktop->getSelection();
        selection->setReprList(result);
    }
    in_doc->emitReconstructionFinish();
    DocumentUndo::done(in_doc, RC_("Undo", "Import"), INKSCAPE_ICON("document-import"));
    return import_root;
}

/**
 * Import the given document as a set of multiple pages and append to this one.
 *
 * @param this_doc - Our current document, to be changed
 * @param that_doc - The documennt that contains our importable pages
 */
void file_import_pages(SPDocument *this_doc, SPDocument *that_doc)
{
    auto &this_pm = this_doc->getPageManager();
    auto &that_pm = that_doc->getPageManager();

    // Make sure objects have visualBounds created for import
    that_doc->ensureUpToDate();
    this_pm.enablePages();

    Geom::Affine tr = Geom::Translate(this_pm.nextPageLocation() * this_doc->getDocumentScale());
    for (auto &that_page : that_pm.getPages()) {
        auto this_page = this_pm.newDocumentPage(that_page->getDocumentRect() * tr);
        // Set the margin, bleed, etc
        this_page->copyFrom(that_page);
    }

    this_doc->import(*that_doc, nullptr, nullptr, tr, nullptr);
}

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
