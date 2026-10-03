// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_SP_FILE_H
#define SEEN_SP_FILE_H

/*
 * File/Print operations
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Chema Celorio <chema@celorio.com>
 *
 * Copyright (C) 2006 Johan Engelen <johan@shouraizou.nl>
 * Copyright (C) 2001-2002 Ximian, Inc.
 * Copyright (C) 1999-2002 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <optional>
#include <string>
#include <2geom/point.h>
#include "extension/system.h"

class SPDesktop;
class SPDocument;
class SPObject;
class SPRoot;

void sp_file_flush_async_save(SPDocument *);
void sp_file_force_deferred_save(SPDocument *);
bool sp_file_pending_save_has_later_edits(SPDocument *);
// True while a background save of the document itself (not a copy) is running.
bool sp_file_pending_save_is_official(SPDocument *);

namespace Inkscape {
    namespace IO { class FileOperationLease; }
    namespace Extension {
        class Extension;
    }
}

namespace Gio {
class File;
}

namespace Gtk {
class Window;
}

// Get the name of the default template uri
std::string sp_file_default_template_uri();

/*######################
## N E W
######################*/

/**
 * Creates a new Inkscape document and window.
 * Return value is a pointer to the newly created desktop.
 */
SPDesktop* sp_file_new (const std::string &templ);
SPDesktop* sp_file_new_default ();

/*######################
## D E L E T E
######################*/

/**
 * Close the document/view
 */
void sp_file_exit ();

/*######################
## O P E N
######################*/

// See src/actions/actions-file-window.h

/**
 * Reverts file to disk-copy on "YES"
 */
void sp_file_revert_dialog ();

/*######################
## S A V E
######################*/

/**
 *
 */
bool sp_file_save (Gtk::Window &parentWindow, void* object, void* data);

/**
 *  Saves the given document.  Displays a file select dialog
 *  to choose the new name.
 */
bool sp_file_save_as (Gtk::Window &parentWindow, void* object, void* data);

/**
 *  Saves a copy of the given document.  Displays a file select dialog
 *  to choose a name for the copy.
 */
bool sp_file_save_a_copy (Gtk::Window &parentWindow, void* object, void* data);

/**
 *  Save a copy of a document as template.
 */
bool
sp_file_save_template(Gtk::Window &parentWindow, Glib::ustring name,
    Glib::ustring author, Glib::ustring description, Glib::ustring keywords,
    bool isDefault);

/**
 *  Saves the given document.  Displays a file select dialog
 *  if needed.
 */
bool sp_file_save_document (Gtk::Window &parentWindow, SPDocument *document, bool allow_async = false,
                            SPDesktop *desktop = nullptr);
// One-shot chooser replacement for local file-I/O integration tests only.
void set_file_save_chooser_path_for_testing(std::string path);
std::string take_file_save_chooser_path_for_testing();

/* Do the saveas dialog with a document as the parameter */
bool sp_file_save_dialog (Gtk::Window &parentWindow, SPDocument *doc, Inkscape::Extension::FileSaveMethod save_method);

/**
 * Outcome of saving an already-selected file against a bound document.
 *
 * RetryWithDifferentName means the save was refused because the destination
 * existed and overwrite was declined (Output::no_overwrite); the caller should
 * re-run its async chooser rather than retry the same file.
 */
enum class FileSaveResult {
    Saved,
    SavedOlderRevision,
    Failed,
    Uncertain,
    InProgress,
    RetryWithDifferentName,
};

/**
 * Save an already-selected file against an explicitly bound document, desktop
 * and window. Status and overwrite confirmation use the bound context;
 * error dialogs still use the legacy process-global transient parent.
 *
 * This is the completion primitive for a future asynchronous Save As action:
 * it reuses the same suffix normalization, output-extension resolution,
 * Recent-list update and save-path preference as sp_file_save_dialog, but it
 * never opens a synchronous file chooser. Every status flash uses @a desktop;
 * overwrite confirmation uses @a parentWindow.
 * The caller must keep the bound window/document/desktop alive and authorize
 * its file-operation lease immediately before this call.
 *
 * @param parentWindow window owning the overwrite confirmation
 * @param doc          bound document being saved
 * @param desktop      bound desktop; must be non-null for the bound route
 * @param file         selected destination file
 * @param save_method  Save As / Save Copy / first-save method
 * @return Saved on success, SavedOlderRevision when a completed older snapshot
 *         was published, RetryWithDifferentName when overwrite was declined,
 *         Uncertain when publication cannot be proved, Failed otherwise
 */
FileSaveResult sp_file_save_bound (Gtk::Window &parentWindow, SPDocument *doc, SPDesktop *desktop,
                                   Glib::RefPtr<Gio::File> file,
                                   Inkscape::Extension::FileSaveMethod save_method,
                                   Inkscape::IO::FileOperationLease *lease = nullptr);


/*######################
## I M P O R T
######################*/

void sp_import_document(SPDesktop *desktop, SPDocument *clipdoc, bool in_place, bool on_page = false);

// See src/actions/actions-file-window.h

/**
 * Imports pages into the document.
 */
void file_import_pages(SPDocument *this_doc, SPDocument *that_doc);

/**
 * Imports a resource
 *
 * @param in_doc    destination document; insertion and Undo are recorded here.
 * @param path      file to import.
 * @param key       input extension, or nullptr to resolve from @a path.
 * @param drop_pos  explicit drop position in desktop coordinates; when it is
 *                  not given, the position of @a destination_window is used.
 * @param destination_window
 *                  the window the import was issued on, when the caller has
 *                  one. When given, the insertion layer and the selection
 *                  update come from this window only if its document is still
 *                  @a in_doc; otherwise the import uses @a in_doc's root and
 *                  changes no selection. Omitted/null means "use the process
 *                  active desktop", which is the drag-and-drop and
 *                  command-line behaviour.
 */
SPObject* file_import(SPDocument *in_doc, const std::string &path,
                 Inkscape::Extension::Extension *key,
                 std::optional<Geom::Point> drop_pos = std::nullopt,
                 SPDesktop *destination_window = nullptr);

#endif // SEEN_SP_FILE_H


/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vi: set autoindent shiftwidth=4 tabstop=8 filetype=cpp expandtab softtabstop=4 fileencoding=utf-8 textwidth=99 :
