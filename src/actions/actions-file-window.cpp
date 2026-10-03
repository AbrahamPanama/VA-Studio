// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 *
 *  Actions for opening, saving, etc. files which (mostly) open a dialog or an Inkscape window.
 *  Used by menu items under the "File" submenu.
 *
 * Authors:
 *   Sushant A A <sushant.co19@gmail.com>
 *
 * Copyright (C) 2021 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "ui/explode-bitmap-publication.h"
#include <giomm.h>
#include <glibmm/i18n.h>
#include <glibmm/miscutils.h>
#include <glibmm/main.h>

#include <exception>
#include <memory>
#include <string>
#include <sigc++/connection.h>

#include "actions-file-window.h"
#include "actions-helper.h"

#include "inkscape-application.h"
#include "inkscape-window.h"
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "extension/db.h"
#include "extension/output.h"
#include "file.h"
#include "io/document-file-operation.h"
#include "io/sys.h"
#include "object/sp-namedview.h"
#include "print.h"
#include "preferences.h"
#include "ui/dialog/choose-file.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/dialog/save-template-dialog.h"
#include "ui/dialog/new-from-template.h"
#include "ui/icon-names.h"
#include "ui/interface.h"

namespace {

struct PendingImport {
    SPDocument *document = nullptr;
    SPDesktop *desktop = nullptr;
    sigc::connection document_destroyed;
    sigc::connection desktop_destroyed;
    ~PendingImport()
    {
        document_destroyed.disconnect();
        desktop_destroyed.disconnect();
    }
};

} // namespace

void show_file_chooser_error(std::string const &error)
{
    if (!error.empty()) sp_ui_error_dialog(error.c_str());
}

namespace {

void show_save_as_error(std::string const &error) noexcept
{
    try {
        show_file_chooser_error(error);
    } catch (...) {
        g_warning("Save As error dialog failed");
    }
}

// Explicit File > Save As / Save Copy state. The heap object is owned by the async
// chooser callback (and by the setting-up local), never by itself, so there is
// no strong self-cycle: the binding predicate and the destroy callbacks only
// capture a weak pointer. The move-only lease owns the admitted request and the
// document operation lease for the entire asynchronous lifetime.
struct PendingSaveAs
{
    InkscapeWindow *window = nullptr;
    InkscapeWindow::LifetimeToken window_token;
    SPDocument *document = nullptr;
    SPDesktop *desktop = nullptr;
    // Which explicit save intent this shared state represents. Drives the seed
    // folder/format, the chooser title, the bound save method (official flag)
    // and the admission method/context. Save As keeps its historical default.
    Inkscape::Extension::FileSaveMethod method = Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS;
    sigc::connection document_destroyed;
    sigc::connection desktop_destroyed;
    Inkscape::IO::FileOperationLease lease;
    Inkscape::IO::FileOperationContext context;
    bool terminal = false;

    ~PendingSaveAs()
    {
        document_destroyed.disconnect();
        desktop_destroyed.disconnect();
    }

    // Read-only liveness probe. The raw window pointer is only inspected once
    // its non-owning lifetime token confirms the native window is alive; the
    // document/desktop pointers are nulled by their destroy connections before
    // the objects are freed.
    bool alive() const
    {
        return window_token.valid() && window && document && desktop && window->get_document() == document &&
               window->get_desktop() == desktop && desktop->getDocument() == document;
    }
};

struct SaveAsSeed
{
    std::string basename;
    std::string folder;
};

bool is_save_copy(Inkscape::Extension::FileSaveMethod method)
{
    return method == Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY;
}

// User-visible verb for the shared explicit-save state. Save As wording must not
// leak into Save Copy dialogs and vice versa.
char const *save_dialog_verb(Inkscape::Extension::FileSaveMethod method)
{
    return is_save_copy(method) ? "Save Copy" : "Save As";
}

// Pick the same initial name/folder as the old synchronous Save As dialog, but
// without any synchronous file_test/stat or unique-name loop. A suggestion that
// already exists is handled by the overwrite confirmation inside
// sp_file_save_bound. A suffixless name is still normalized later by the shared
// file_save_dialog_apply helper.
SaveAsSeed compute_save_as_seed(SPDocument *document, Inkscape::Extension::FileSaveMethod method)
{
    SaveAsSeed seed;

    auto const extension_id = Inkscape::Extension::get_file_save_extension(method);

    std::string filename_extension = ".svg";
    if (auto *extension =
            dynamic_cast<Inkscape::Extension::Output *>(Inkscape::Extension::db.get(extension_id.c_str()))) {
        filename_extension = extension->get_extension();
    }

    std::string save_path = Inkscape::Extension::get_file_save_path(document, method);
    if (save_path.empty()) {
        save_path = Glib::get_home_dir();
    }

    std::string save_loc = save_path;
    save_loc.append(G_DIR_SEPARATOR_S);
    if (!document->getDocumentFilename()) {
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        Glib::ustring default_filename = prefs->getString("/options/defaultfilename/value", _("drawing"));
        save_loc = save_loc + default_filename + filename_extension;
    } else {
        save_loc += Glib::path_get_basename(document->getDocumentFilename());
        Inkscape::IO::swap_file_extension(save_loc, filename_extension);
    }

    seed.basename = Glib::path_get_basename(save_loc);
    seed.folder = Glib::path_get_dirname(save_loc);
    return seed;
}

// Exactly-once terminal completion. Marks the state terminal, latches the
// outcome through the lease, then releases the lease. Callers must not use the
// save state after this returns.
void finish_pending_save_as(std::shared_ptr<PendingSaveAs> const &state, Inkscape::IO::FileOperationOutcome outcome,
                            std::string detail = {})
{
    if (!state || state->terminal)
        return;
    state->terminal = true;

    Inkscape::IO::FileOperationResult result;
    result.outcome = outcome;
    result.detail = std::move(detail);
    state->lease.complete(std::move(result));
    state->lease = Inkscape::IO::FileOperationLease{};
}

// Show the async explicit-save chooser for an already-admitted request. On a declined
// overwrite this is called again with the same state and lease; it performs no
// second admission and never starts a nested main-context loop.
void show_pending_save_as_chooser(std::shared_ptr<PendingSaveAs> const &state)
{
    if (!state || state->terminal)
        return;
    if (!state->alive()) {
        finish_pending_save_as(state, Inkscape::IO::FileOperationOutcome::StaleTarget,
                               "window, document or desktop is no longer available");
        return;
    }

    try {
        bool const copy = is_save_copy(state->method);
        SaveAsSeed const seed = compute_save_as_seed(state->document, state->method);
        Glib::ustring const dialog_title = copy
            ? _("Select file to save a copy to")
            : _("Select file to save to");

        auto callback = [state](Glib::RefPtr<Gio::File> file, std::string error) {
                if (!state || state->terminal)
                    return;
                bool output_attempted = false;
                try {
                    if (!state->alive()) {
                        finish_pending_save_as(state, Inkscape::IO::FileOperationOutcome::StaleTarget,
                                               "window, document or desktop is no longer available");
                        return;
                    }
                    if (!error.empty()) {
                        show_save_as_error(error);
                        finish_pending_save_as(state, Inkscape::IO::FileOperationOutcome::Failed, std::move(error));
                        return;
                    }
                    if (!file) {
                        finish_pending_save_as(state, Inkscape::IO::FileOperationOutcome::Cancelled,
                                               std::string(save_dialog_verb(state->method)) + " cancelled");
                        return;
                    }
                    // Gate the exact admitted request before any namedview mutation or
                    // output. A refusal completes its own outcome, never a generic one.
                    auto const gate = state->lease.authorizeOutput(state->context);
                    if (gate.outcome != Inkscape::IO::FileOperationOutcome::Success) {
                        if (gate.outcome == Inkscape::IO::FileOperationOutcome::Failed ||
                            gate.outcome == Inkscape::IO::FileOperationOutcome::Busy) {
                            show_save_as_error(gate.detail);
                        }
                        finish_pending_save_as(state, gate.outcome, gate.detail);
                        return;
                    }

                    sp_namedview_document_from_window(state->desktop);
                    output_attempted = true;
                    auto const result = sp_file_save_bound(*state->window, state->document, state->desktop, file,
                                                           state->method, &state->lease);
                    switch (result) {
                        case FileSaveResult::InProgress:
                            state->terminal = true;
                            break;
                        case FileSaveResult::Saved:
                            finish_pending_save_as(state, Inkscape::IO::FileOperationOutcome::Success,
                                                   "Document saved.");
                            break;
                        case FileSaveResult::SavedOlderRevision:
                            finish_pending_save_as(
                                state, Inkscape::IO::FileOperationOutcome::SavedOlderRevision,
                                "An older document revision was published; newer edits remain unsaved.");
                            break;
                        case FileSaveResult::Failed:
                            finish_pending_save_as(state, Inkscape::IO::FileOperationOutcome::Failed,
                                                   "Document not saved.");
                            break;
                        case FileSaveResult::Uncertain:
                            finish_pending_save_as(state, Inkscape::IO::FileOperationOutcome::Uncertain,
                                                   "Save outcome uncertain; inspect the destination.");
                            break;
                        case FileSaveResult::RetryWithDifferentName:
                            // Same state and same non-terminal lease: re-run the async
                            // chooser without a second admission or a nested loop.
                            show_pending_save_as_chooser(state);
                            break;
                    }
                } catch (std::exception const &e) {
                    show_save_as_error(output_attempted
                        ? "Save outcome could not be confirmed; inspect the destination."
                        : e.what());
                    finish_pending_save_as(state,
                                           output_attempted ? Inkscape::IO::FileOperationOutcome::Uncertain
                                                            : Inkscape::IO::FileOperationOutcome::Failed,
                                           e.what());
                } catch (...) {
                    auto const failed_unexpectedly =
                        std::string(save_dialog_verb(state->method)) + " failed unexpectedly";
                    show_save_as_error(output_attempted
                        ? "Save outcome could not be confirmed; inspect the destination."
                        : failed_unexpectedly);
                    finish_pending_save_as(state,
                                           output_attempted ? Inkscape::IO::FileOperationOutcome::Uncertain
                                                            : Inkscape::IO::FileOperationOutcome::Failed,
                                           failed_unexpectedly);
                }
            };
        if (auto path = take_file_save_chooser_path_for_testing(); !path.empty()) {
            Glib::signal_idle().connect([callback, path] {
                callback(Gio::File::create_for_path(path), {});
                return false;
            });
        } else {
            Inkscape::choose_file_save_async(dialog_title, state->window,
                Inkscape::UI::Dialog::create_export_filters(true), seed.basename, seed.folder,
                std::move(callback));
        }
    } catch (std::exception const &e) {
        show_save_as_error(e.what());
        finish_pending_save_as(state, Inkscape::IO::FileOperationOutcome::Failed, e.what());
    } catch (...) {
        auto const chooser_start_failed =
            std::string(save_dialog_verb(state->method)) + " chooser could not start";
        show_save_as_error(chooser_start_failed);
        finish_pending_save_as(state, Inkscape::IO::FileOperationOutcome::Failed, chooser_start_failed);
    }
}

// Shared admission prologue for the explicit File > Save As and File > Save Copy
// actions. Extracted verbatim from the original document_save_as: validate the
// bound window/document/desktop, invalidate raw pointers on destruction, admit
// exactly one document file operation with a weak read-only binding predicate,
// then hand the shared state and lease to the async chooser. A refusal leaves no
// lease and no request, so there is nothing to complete.
void start_explicit_save_dialog(InkscapeWindow *win, Inkscape::Extension::FileSaveMethod method)
{
    if (!win)
        return;

    auto *document = win->get_document();
    auto *desktop = win->get_desktop();
    if (!document || !desktop || desktop->getDocument() != document)
        return;

    // Refuse before admission's error reporting can enter dialog_run().
    if (Inkscape::Bitmap::publicationBoundaryPending(document)) return;
    bool const copy = is_save_copy(method);

    // The chooser can outlive its originating window, document or desktop. The
    // window lifetime token plus weak destruction callbacks invalidate the raw
    // pointers before any asynchronous completion dereferences them.
    auto state = std::make_shared<PendingSaveAs>();
    state->window = win;
    state->window_token = win->lifetimeToken();
    state->document = document;
    state->desktop = desktop;
    state->method = method;

    std::weak_ptr<PendingSaveAs> weak = state;
    state->document_destroyed = document->connectDestroy([weak] {
        if (auto pending = weak.lock()) pending->document = nullptr;
    });
    state->desktop_destroyed = desktop->connectDestroy([weak](SPDesktop *) {
        if (auto pending = weak.lock()) pending->desktop = nullptr;
    });

    Inkscape::IO::FileOperationRequestInfo info;
    info.method = copy ? Inkscape::IO::FileOperationMethod::SaveCopy
                       : Inkscape::IO::FileOperationMethod::SaveAs;
    info.target = document->getDocumentFilename() ? document->getDocumentFilename() : "";
    info.context = copy ? "win.document-save-copy" : "win.document-save-as";

    // Weak, read-only binding predicate: no strong reference back to the state,
    // and no dereference of a window without a valid lifetime token or a
    // document/desktop that its destroy connection has nulled.
    auto binding = [weak]() -> bool {
        auto pending = weak.lock();
        if (!pending || !pending->window_token.valid() || !pending->window
            || !pending->document || !pending->desktop) {
            return false;
        }
        return pending->window->get_document() == pending->document
            && pending->window->get_desktop() == pending->desktop
            && pending->desktop->getDocument() == pending->document;
    };

    // Exactly one admission, before any chooser is shown. A refusal leaves no
    // lease and no request, so there is nothing to complete.
    Inkscape::IO::FileOperationResult refusal;
    auto lease = Inkscape::IO::DocumentFileOperation::admit(*document, info, std::move(binding), &refusal);
    if (!lease) {
        if (!refusal.detail.empty()) {
            show_save_as_error(refusal.detail);
        }
        return;
    }

    state->lease = std::move(*lease);
    state->context = state->lease.context();
    show_pending_save_as_chooser(state);
}

} // namespace

void
document_new(InkscapeWindow* win)
{
    sp_file_new_default();
}

void
document_dialog_templates(InkscapeWindow* win)
{
    if (win) {
        Inkscape::UI::NewFromTemplate::load_new_from_template(*win);
    }
}

void
document_open(InkscapeWindow* win)
{
    if (!win) return;
    Inkscape::choose_file_open_images_async(_("Select file(s) to open"), win,
        "/dialog/open/path", _("Open"),
        [](std::vector<Glib::RefPtr<Gio::File>> files, std::string error) {
            if (!error.empty()) return show_file_chooser_error(error);
            auto *app = InkscapeApplication::instance();
            if (!app) return;
            for (auto &file : files) {
                if (!file) continue;
                try {
                    app->create_window(file);
                } catch (std::exception const &e) {
                    show_file_chooser_error(e.what());
                } catch (...) {
                    show_file_chooser_error(_("Could not open the selected file"));
                }
            }
        });
}

void
document_revert(InkscapeWindow* win)
{
    sp_file_revert_dialog();
}

void
document_save(InkscapeWindow* win)
{
    // Save File
    sp_file_save(*win, nullptr, nullptr);
}

void
document_save_as(InkscapeWindow* win)
{
    start_explicit_save_dialog(win, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_AS);
}

void
document_save_copy(InkscapeWindow* win)
{
    // Same async owner as Save As; SAVE_COPY drives the copy seed, title and
    // official=false bound save, so filename/dirty state/Undo are preserved.
    start_explicit_save_dialog(win, Inkscape::Extension::FILE_SAVE_METHOD_SAVE_COPY);
}

void
document_save_template(InkscapeWindow* win)
{
    // Save As Template
    Inkscape::UI::Dialog::SaveTemplate::save_document_as_template(*win);
}

void
document_import(InkscapeWindow* win)
{
    if (!win) return;
    auto state = std::make_shared<PendingImport>();
    state->document = win->get_document();
    state->desktop = win->get_desktop();
    if (!state->document || !state->desktop) return;

    // The chooser can outlive its originating window or document. The weak
    // destruction callbacks invalidate the raw pointers before completion.
    std::weak_ptr<PendingImport> weak = state;
    state->document_destroyed = state->document->connectDestroy([weak] {
        if (auto pending = weak.lock()) pending->document = nullptr;
    });
    state->desktop_destroyed = state->desktop->connectDestroy([weak](SPDesktop *) {
        if (auto pending = weak.lock()) pending->desktop = nullptr;
    });

    Inkscape::choose_file_open_images_async(_("Select file(s) to import"), win,
        "/dialog/import/path", _("Import"),
        [state](std::vector<Glib::RefPtr<Gio::File>> files, std::string error) {
            if (!error.empty()) return show_file_chooser_error(error);
            if (!state->document || !state->desktop
                || state->desktop->getDocument() != state->document) return;
            for (auto &file : files) {
                if (!state->document || !state->desktop
                    || state->desktop->getDocument() != state->document) return;
                if (file && !file->get_path().empty()) {
                    try {
                        file_import(state->document, file->get_path(), nullptr,
                                    std::nullopt, state->desktop);
                    } catch (std::exception const &e) {
                        show_file_chooser_error(e.what());
                    } catch (...) {
                        show_file_chooser_error(_("Could not import the selected file"));
                    }
                } else if (file) {
                    show_file_chooser_error(_("Import requires a local file"));
                }
            }
        });
}

void
document_print(InkscapeWindow* win)
{
    // Print File
    if (auto doc = win->get_document()) {
        sp_print_document(*win, doc);
    }
}

void
document_cleanup(InkscapeWindow* win)
{
    // Cleanup Up Document
    auto doc = win->get_document();
    unsigned int diff = doc->vacuumDocument();

    Inkscape::DocumentUndo::done(doc, RC_("Undo", "Clean up document"), INKSCAPE_ICON("document-cleanup"));

    // Show status messages when in GUI mode
    if (diff > 0) {
        win->get_desktop()->messageStack()->flashF(Inkscape::NORMAL_MESSAGE,
                ngettext("Removed <b>%i</b> unused definition in &lt;defs&gt;.",
                        "Removed <b>%i</b> unused definitions in &lt;defs&gt;.",
                        diff),
                diff);
    } else {
        win->get_desktop()->messageStack()->flash(Inkscape::NORMAL_MESSAGE,  _("No unused definitions in &lt;defs&gt;."));
    }
}

// Close tab, checking for data loss. If it's the last tab, keep open with new document.
void document_close(InkscapeWindow *win)
{
    auto app = InkscapeApplication::instance();
    app->destroyDesktop(win->get_desktop(), true); // true == keep alive with new new document
}

const Glib::ustring SECTION = NC_("Action Section", "Window-File");

std::vector<std::vector<Glib::ustring>> raw_data_dialog_window =
{
    // clang-format off
    {"win.document-new",              N_("New"),               SECTION,   N_("Create new document from the default template")},
    {"win.document-dialog-templates", N_("New from Template"), SECTION,   N_("Create new project from template")},
    {"win.document-open",             N_("Open File Dialog"),  SECTION,   N_("Open an existing document")},
    {"win.document-revert",           N_("Revert"),            SECTION,   N_("Revert to the last saved version of document (changes will be lost)")},
    {"win.document-save",             N_("Save"),              SECTION,   N_("Save document")},
    {"win.document-save-as",          N_("Save As"),           SECTION,   N_("Save document under a new name")},
    {"win.document-save-copy",        N_("Save a Copy"),       SECTION,   N_("Save a copy of the document under a new name")},
    {"win.document-save-template",    N_("Save Template"),     SECTION,   N_("Save a copy of the document as template")},
    {"win.document-import",           N_("Import"),            SECTION,   N_("Import a bitmap or SVG image into this document")},
    {"win.document-print",            N_("Print"),             SECTION,   N_("Print document")},
    {"win.document-cleanup",          N_("Clean Up Document"), SECTION,   N_("Remove unused definitions (such as gradients or clipping paths) from the document")},
    {"win.document-close",            N_("Close"),             SECTION,   N_("Close document (unless last document)")},
    // clang-format on
};

void
add_actions_file_window(InkscapeWindow* win)
{
    // clang-format off
    win->add_action( "document-new",                sigc::bind(sigc::ptr_fun(&document_new),               win));
    win->add_action( "document-dialog-templates",   sigc::bind(sigc::ptr_fun(&document_dialog_templates),  win));
    win->add_action( "document-open",               sigc::bind(sigc::ptr_fun(&document_open),              win));
    win->add_action( "document-revert",             sigc::bind(sigc::ptr_fun(&document_revert),            win));
    win->add_action( "document-save",               sigc::bind(sigc::ptr_fun(&document_save),              win));
    win->add_action( "document-save-as",            sigc::bind(sigc::ptr_fun(&document_save_as),           win));
    win->add_action( "document-save-copy",          sigc::bind(sigc::ptr_fun(&document_save_copy),         win));
    win->add_action( "document-save-template",      sigc::bind(sigc::ptr_fun(&document_save_template),     win));
    win->add_action( "document-import",             sigc::bind(sigc::ptr_fun(&document_import),            win));
    win->add_action( "document-print",              sigc::bind(sigc::ptr_fun(&document_print),             win));
    win->add_action( "document-cleanup",            sigc::bind(sigc::ptr_fun(&document_cleanup),           win));
    win->add_action( "document-close",              sigc::bind(sigc::ptr_fun(&document_close),             win));
    // clang-format on

    auto app = InkscapeApplication::instance();
    if (!app) {
        show_output("add_actions_file_window: no app!");
        return;
    }
    app->get_action_extra_data().add_data(raw_data_dialog_window);
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
