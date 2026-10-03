// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   Ted Gould <ted@gould.cx>
 *
 * Copyright (C) 2006 Johan Engelen <johan@shouraizou.nl>
 * Copyright (C) 2002-2004 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */



#ifndef INKSCAPE_EXTENSION_OUTPUT_H__
#define INKSCAPE_EXTENSION_OUTPUT_H__

#include "extension.h"

#include <string>
#include <optional>
#include "internal/svg-publication.h"

class SPDocument;

namespace Inkscape {
namespace Extension {

/**
 * Caller-declared preparation for a document save. A value type only; it has no
 * callbacks and all members default to false, so direct Output::save() callers
 * (export dialog, raster path) keep exactly the prior behaviour.
 */
struct SavePreparation {
    bool document_identity = false; //< Rebase filename/hrefs to the destination on the working copy.
    bool update_dataloss  = false;  //< Reflect this Output's lossiness in inkscape:dataloss on the working copy.
    bool sync_title       = false;  //< Synchronize svg:title -> RDF title on the working copy.
    std::string *notice   = nullptr; //< Optional notice for a successful save.
    std::string *route    = nullptr; //< Diagnostic route, separate from user notices.
    std::optional<std::size_t> *direct_size_hint = nullptr; //< Buffered fallback observation for completion.
    bool interactive_native_save = false; //< Route native GUI Save through owned bytes.
};

class Output : public Extension {
    gchar *mimetype;             /**< What is the mime type this inputs? */
    gchar *extension;            /**< The extension of the input files */
    gchar *filetypename;         /**< A userfriendly name for the file type */
    gchar *filetypetooltip;      /**< A more detailed description of the filetype */
    bool   dataloss;             /**< The extension causes data loss on save */
    bool   savecopyonly;         /**< Limit output option to Save a Copy */
    bool   raster = false;       /**< Is the extension expecting a png file */
    bool   exported = false;     /**< Is the extension available in the export dialog */
    std::string *_active_save_notice = nullptr;
    bool _active_interactive_native_save = false;

public:
    std::optional<Internal::PublicationJob> begin_publication(
        SPDocument *doc, gchar const *filename, SavePreparation const &preparation);
    // A nonfatal publication warning; it must not change the save outcome.
    void report_save_notice(std::string const &notice);
    bool is_interactive_native_save() const { return _active_interactive_native_save; }
    /**
     * Generic failure for an undescribed reason. `error` carries the producer's
     * diagnostic when one exists and is empty otherwise; callers must check it
     * before displaying anything.
     */
    class save_failed {
    public:
        std::string error;
        explicit save_failed(std::string error = {}) : error(error) {}
    };
    class save_cancelled {};     /**< Saving was cancelled */
    class no_extension_found {}; /**< Failed because we couldn't find an extension to match the filename */
    class file_read_only {};     /**< The existing file can not be opened for writing */

    /**
     * The destination cannot safely be saved to (e.g. an alias/hard-linked
     * existing file, or a platform capability gap). The destination was not
     * written by this save attempt.
     *
     * Derives from save_failed so callers that only catch save_failed (e.g. the
     * non-GUI export entry points) keep their existing behavior. The GUI catches
     * this more-derived type before the save_failed handler.
     */
    class save_unsupported : public save_failed {
    public:
        explicit save_unsupported(std::string error = {}) : save_failed(error) {}
    };
    /**
     * The destination changed or appeared after the save began. It was NOT
     * overwritten; the user must choose another name or destination.
     *
     * Derives from save_failed (see save_unsupported).
     */
    class save_conflict : public save_failed {
    public:
        explicit save_conflict(std::string error = {}) : save_failed(error) {}
    };
    /**
     * The save may or may not have published. The document must stay dirty and
     * the user must inspect the destination. recovery_path is only meaningful
     * when non-empty. recovery_path_available records only that a producer
     * signalled the retained location's identity/availability; it is NOT proof
     * that the retained bytes are complete, so callers must never report it as a
     * verified copy.
     *
     * Derives from save_failed (see save_unsupported).
     */
    class save_uncertain : public save_failed {
    public:
        std::string recovery_path;
        bool recovery_path_available = false;
        save_uncertain(std::string error = {}, std::string recovery_path = {},
                       bool recovery_path_available = false)
            : save_failed(error), recovery_path(recovery_path),
              recovery_path_available(recovery_path_available)
        {}
    };
    class export_id_not_found {  /**< The object ID requested for export could not be found in the document */
        public:
            const gchar * const id;
            export_id_not_found(const gchar * const id = nullptr) : id{id} {};
    };
    struct lost_document {}; ///< Document was closed during execution of async extension.

    Output(Inkscape::XML::Node *in_repr, ImplementationHolder implementation, std::string *base_directory);
    ~Output () override;

    bool check() override;

    void         save (SPDocument *doc,
                       gchar const *filename,
                       bool detachbase = false);
    void         save (SPDocument *doc,
                       gchar const *filename,
                       bool detachbase,
                       SavePreparation const &preparation);
    void         export_raster (const SPDocument *doc,
                                std::string png_filename,
                                gchar const *filename,
                                bool detachbase);
    gchar const *get_mimetype() const;
    gchar const *get_extension() const;
    const char * get_filetypename(bool translated=false) const;
    const char * get_filetypetooltip(bool translated=false) const;
    bool         causes_dataloss() const { return dataloss; };
    bool         savecopy_only() const { return savecopyonly; };
    bool         is_raster() const { return raster; };
    bool         is_exported() const { return exported; };
    void         add_extension(std::string &filename);
    bool         can_save_filename(gchar const *filename) const;
};

/**
 * Apply one save preparation to a document.
 *
 * This is the single shared implementation used by Output::save() on its working
 * copy and by Extension::save()'s post-success live commit, so the projection
 * and the committed live state cannot diverge. Every change is made under
 * ScopedInsensitive: none of it is a user edit or a new undo transaction.
 *
 * @param doc      Document to prepare (working copy or live document).
 * @param filename Destination filename used for identity rebasing.
 * @param output   Output module whose dataloss applies.
 * @param prep     Which preparations to apply.
 */
void apply_save_preparation(SPDocument *doc, gchar const *filename,
                            Output const &output, SavePreparation const &prep);

} }  /* namespace Inkscape, Extension */
#endif /* INKSCAPE_EXTENSION_OUTPUT_H__ */

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
