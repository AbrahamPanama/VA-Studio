// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * This is file is kind of the junk file.  Basically everything that
 * didn't fit in one of the other well defined areas, well, it's now
 * here.  Which is good in someways, but this file really needs some
 * definition.  Hopefully that will come ASAP.
 *
 * Authors:
 *   Ted Gould <ted@gould.cx>
 *
 * Copyright (C) 2002-2004 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_EXTENSION_SYSTEM_H__
#define INKSCAPE_EXTENSION_SYSTEM_H__

#include <vector>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <optional>

#include "extension/internal/svg-publication.h"

#include <glibmm/ustring.h>

class SPDocument;

namespace Inkscape::Extension {

class Extension;
class Print;
namespace Implementation { class Implementation; }

/**
 * Used to distinguish between the various invocations of the save dialogs (and thus to determine
 * the file type and save path offered in the dialog)
 */
enum FileSaveMethod {
    FILE_SAVE_METHOD_SAVE_AS,
    FILE_SAVE_METHOD_SAVE_COPY,
    FILE_SAVE_METHOD_EXPORT,
    // Fallback for special cases (e.g., when saving a document for the first time or after saving
    // it in a lossy format)
    FILE_SAVE_METHOD_INKSCAPE_SVG,
    // For saving temporary files; we return the same data as for FILE_SAVE_METHOD_SAVE_AS
    FILE_SAVE_METHOD_TEMPORARY,
};

std::unique_ptr<SPDocument> open(Extension *key, char const *filename, bool is_importing = false);

/**
 * Injectable overwrite confirmation for deterministic (headless) tests.
 *
 * Empty means "use the real predicate" (sp_ui_overwrite_file). A callable
 * replaces it and must return true for the save to proceed. It is consulted
 * only when @a check_overwrite is true, so it cannot bypass the normal
 * check_overwrite semantics.
 */
using OverwriteConfirm = std::function<bool(std::string const &)>;

struct PendingSave;
void dispose_pending_save(PendingSave *pending) noexcept;
using PendingSavePtr = std::unique_ptr<PendingSave, decltype(&dispose_pending_save)>;
PendingSavePtr begin_save_async(
    Extension *key, SPDocument *doc, char const *filename, bool check_overwrite,
    bool official, FileSaveMethod save_method, OverwriteConfirm const &confirm_overwrite,
    bool sync_title, std::string *notice, bool interactive_file_save);
bool pending_save_may_alias_official(PendingSave const &pending) noexcept;
std::optional<uint64_t> pending_save_snapshot_revision(PendingSave const &pending) noexcept;
/// Release a deferred-serialization snapshot once its publication has finished (initiating thread only).
void release_publication_snapshot(PendingSave &pending) noexcept;
void set_save_completion_interleaving_for_testing(std::function<void(SPDocument *)> callback);
std::optional<Internal::PublicationJob> take_publication_job(
    PendingSave &pending, Internal::PublicationResult *direct_result = nullptr);
void finish_save_async(PendingSave &pending, Internal::PublicationResult const &result,
                       std::string *notice, bool force_older = false);

/** The output file contains a completed snapshot, but the live document changed
 * while it was being written. Keep the document dirty and its prior save anchor.
 * The destination may contain useful work, so callers must not describe this as
 * a failed write or close the document as though its current state were saved.
 */
class PublishedOlderRevision : public std::runtime_error {
public:
    PublishedOlderRevision() : std::runtime_error("The file contains an earlier snapshot; newer document changes remain unsaved.") {}
};

class PublishedStaleDocument : public std::runtime_error {
public:
    explicit PublishedStaleDocument(std::string reason) : std::runtime_error(std::move(reason)) {}
};

// Existing entry point: unchanged signature/symbol for all application callers.
void save(Extension *key, SPDocument *doc, char const *filename,
          bool check_overwrite, bool official,
          Inkscape::Extension::FileSaveMethod save_method);

// Seam overload used by tests; an empty callback keeps production behaviour.
void save(Extension *key, SPDocument *doc, char const *filename,
          bool check_overwrite, bool official,
          Inkscape::Extension::FileSaveMethod save_method,
          OverwriteConfirm const &confirm_overwrite);

/**
 * Full save entry point. @a sync_title requests the dialog-path svg:title ->
 * RDF title normalization for this save only; the six- and seven-argument
 * overloads above forward with false so ordinary Save and CLI/export keep their
 * prior title behaviour.
 */
void save(Extension *key, SPDocument *doc, char const *filename,
          bool check_overwrite, bool official,
          Inkscape::Extension::FileSaveMethod save_method,
          OverwriteConfirm const &confirm_overwrite,
          bool sync_title);

// Optional one-save warning channel. A successful publication can leave a
// recovery file without making the save itself fail.
void save(Extension *key, SPDocument *doc, char const *filename,
          bool check_overwrite, bool official,
          Inkscape::Extension::FileSaveMethod save_method,
          OverwriteConfirm const &confirm_overwrite,
          bool sync_title, std::string *notice, bool interactive_file_save = false);

Print *get_print(char const *key);
void build_from_file(char const *filename);
void build_from_mem(char const *buffer, std::unique_ptr<Implementation::Implementation> in_imp);

/**
 * Determine the desired default file extension depending on the given file save method.
 * The returned string is guaranteed to be non-empty.
 *
 * @param method the file save method of the dialog
 * @return the corresponding default file extension
 */
Glib::ustring get_file_save_extension (FileSaveMethod method);

/**
 * Determine the desired default save path depending on the given FileSaveMethod.
 * The returned string is guaranteed to be non-empty.
 *
 * @param method the file save method of the dialog
 * @param doc the file's document
 * @return the corresponding default save path
 */
Glib::ustring get_file_save_path (SPDocument *doc, FileSaveMethod method);

/**
 * Write the given file extension back to prefs so that it can be used later on.
 *
 * @param extension the file extension which should be written to prefs
 * @param method the file save method of the dialog
 */
void store_file_extension_in_prefs (Glib::ustring extension, FileSaveMethod method);

/**
 * Write the given path back to prefs so that it can be used later on.
 *
 * @param path the path which should be written to prefs
 * @param method the file save method of the dialog
 */
void store_save_path_in_prefs (Glib::ustring path, FileSaveMethod method);

} // namespace Inkscape::Extension

// Request-local headless adapter. Uses the same libcdr/RVNG engine as CdrInput,
// returning generated SVG before native document construction/resource admission.
namespace Inkscape::Extension {
struct CdrConversionLimits { std::size_t bytes = 64u << 20; unsigned pages = 1000; };
std::vector<std::string> cdr_svg_pages(std::string const &pinned_bytes, std::string &error);
std::vector<std::string> cdr_svg_pages(std::string const &pinned_bytes, std::string &error, CdrConversionLimits limits);
}

#endif /* INKSCAPE_EXTENSION_SYSTEM_H__ */

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
