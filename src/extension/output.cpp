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

#include "output.h"
#include "internal/svg.h"

#include <memory>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <glib/gstdio.h>

#include "document.h"
#include "document-undo.h"
#include "rdf.h"

#include "io/sys.h"
#include "implementation/implementation.h"

#include "object/sp-root.h"
#include "xml/attribute-record.h"
#include "xml/node.h"

/* Inkscape::Extension::Output */

namespace Inkscape {
namespace Extension {

/**
    \brief    Builds an Output object from a XML description
    \param    in_repr        The XML description in a Inkscape::XML::Node tree
    \param    implementation The implementation of the extension

    Okay, so you want to build an Output object.

    This function looks for the <output> section of the
    XML description.  Under there should be several fields which
    describe the output module to excruciating detail.  Those are parsed,
    copied, and put into the structure that is passed in as module.
    Overall, there are many levels of indentation, just to handle the
    levels of indentation in the XML file.
*/
Output::Output(Inkscape::XML::Node *in_repr, ImplementationHolder implementation, std::string *base_directory)
    : Extension(in_repr, std::move(implementation), base_directory)
{
    mimetype = nullptr;
    extension = nullptr;
    filetypename = nullptr;
    filetypetooltip = nullptr;
    dataloss = true;
    savecopyonly = false;

    if (repr != nullptr) {
        Inkscape::XML::Node * child_repr;

        child_repr = repr->firstChild();

        while (child_repr != nullptr) {
            if (!strcmp(child_repr->name(), INKSCAPE_EXTENSION_NS "output")) {

                for (const auto &iter : child_repr->attributeList()) {
                    std::string name = g_quark_to_string(iter.key);
                    std::string value = std::string(iter.value);
                    if (name == "raster")
                        raster = value == "true";
                    else if (name == "is_exported")
                        exported = value == "true";
                    else if (name == "priority")
                        set_sort_priority(strtol(value.c_str(), nullptr, 0));
                }

                child_repr = child_repr->firstChild();
                while (child_repr != nullptr) {
                    char const * chname = child_repr->name();
					if (!strncmp(chname, INKSCAPE_EXTENSION_NS_NC, strlen(INKSCAPE_EXTENSION_NS_NC))) {
						chname += strlen(INKSCAPE_EXTENSION_NS);
					}
                    if (chname[0] == '_') /* Allow _ for translation of tags */
                        chname++;
                    if (!strcmp(chname, "extension")) {
                        g_free (extension);
                        extension = g_strdup(child_repr->firstChild()->content());
                    }
                    if (!strcmp(chname, "mimetype")) {
                        g_free (mimetype);
                        mimetype = g_strdup(child_repr->firstChild()->content());
                    }
                    if (!strcmp(chname, "filetypename")) {
                        g_free (filetypename);
                        filetypename = g_strdup(child_repr->firstChild()->content());
                    }
                    if (!strcmp(chname, "filetypetooltip")) {
                        g_free (filetypetooltip);
                        filetypetooltip = g_strdup(child_repr->firstChild()->content());
                    }
                    if (!strcmp(chname, "dataloss")) {
                        dataloss = strcmp(child_repr->firstChild()->content(), "false");
                    }
                    if (!strcmp(chname, "savecopyonly")) {
                        savecopyonly = !strcmp(child_repr->firstChild()->content(), "true");
                    }

                    child_repr = child_repr->next();
                }

                break;
            }

            child_repr = child_repr->next();
        }

    }
}

/**
    \brief  Destroy an output extension
*/
Output::~Output ()
{
    g_free(mimetype);
    g_free(extension);
    g_free(filetypename);
    g_free(filetypetooltip);
    return;
}

/**
    \return  Whether this extension checks out
	\brief   Validate this extension

	This function checks to make sure that the output extension has
	a filename extension and a MIME type.  Then it calls the parent
	class' check function which also checks out the implementation.
*/
bool
Output::check ()
{
	if (extension == nullptr)
		return FALSE;
	if (mimetype == nullptr)
		return FALSE;

	return Extension::check();
}

/**
    \return  IETF mime-type for the extension
	\brief   Get the mime-type that describes this extension
*/
gchar const *
Output::get_mimetype() const
{
    return mimetype;
}

/**
    \return  Filename extension for the extension
	\brief   Get the filename extension for this extension
*/
gchar const *
Output::get_extension() const
{
    return extension;
}

/**
    \return  The name of the filetype supported
	\brief   Get the name of the filetype supported
*/
const char *
Output::get_filetypename(bool translated) const
{
    const char *name;

    if (filetypename)
        name = filetypename;
    else
        name = get_name();

    if (name && translated && filetypename) {
        return get_translation(name);
    } else {
        return name;
    }
}

/**
    \return  Tooltip giving more information on the filetype
	\brief   Get the tooltip for more information on the filetype
*/
const char *
Output::get_filetypetooltip(bool translated) const
{
    if (filetypetooltip && translated) {
        return get_translation(filetypetooltip);
    } else {
        return filetypetooltip;
    }
}

/**
    \return  None
	\brief   Save a document as a file
	\param   doc  Document to save
	\param   filename  File to save the document as

	This function does a little of the dirty work involved in saving
	a document so that the implementation only has to worry about getting
	bits on the disk.

	The big thing that it does is remove and read the fields that are
	only used at runtime and shouldn't be saved.  One that may surprise
	people is the output extension.  This is not saved so that the IDs
	could be changed, and old files will still work properly.
*/
void
apply_save_preparation(SPDocument *doc, gchar const *filename,
                       Output const &output, SavePreparation const &prep)
{
    // None of this is a user edit: do not create undo entries on the working
    // copy, and do not touch the live history when committing.
    DocumentUndo::ScopedInsensitive _no_undo(doc);

    if (prep.document_identity) {
        // Rebase the working copy / live document to the final logical target.
        doc->changeFilenameAndHrefs(filename);
    }

    if (prep.update_dataloss) {
        Inkscape::XML::Node *repr = doc->getReprRoot();
        repr->removeAttribute("inkscape:dataloss");
        if (output.causes_dataloss()) {
            repr->setAttribute("inkscape:dataloss", "true");
        }
    }

    if (prep.sync_title) {
        // Same normalization that used to run on the live document in
        // sp_file_save_dialog(): read svg:title and store it as the RDF title.
        // Own the returned string so it is freed even if rdf_set_work_entity
        // throws.
        std::unique_ptr<gchar, decltype(&g_free)> title(doc->getRoot()->title(), &g_free);
        if (title) {
            rdf_set_work_entity(doc, rdf_find_entity("title"), title.get());
        }
    }
}

std::optional<Internal::PublicationJob>
Output::begin_publication(SPDocument *doc, gchar const *filename,
                          SavePreparation const &preparation)
{
#ifdef __APPLE__
    auto *svg = dynamic_cast<Internal::Svg *>(imp.get());
    if (!preparation.interactive_native_save || !svg) return std::nullopt;
    using Clock = std::chrono::steady_clock;
    bool const timing = g_strcmp0(g_getenv("VACARDS_SAVE_TIMING"), "1") == 0;
    auto const prepare_begin = Clock::now();
    imp->setDetachBase(false);
    auto copy = doc->copy();
    apply_save_preparation(copy.get(), filename, *this, preparation);
    copy->ensureUpToDate();
    run_processing_actions(copy.get());
    if (timing) {
        auto const ms = std::chrono::duration<double, std::milli>(Clock::now() - prepare_begin).count();
        std::fprintf(stderr, "VACARDS_SAVE_TIMING span=prepare_copy ms=%.3f\n", ms);
    }
    auto const *prior_path = doc->getDocumentFilename();
    bool const same_compression = prior_path
        && (g_str_has_suffix(prior_path, ".svg") || g_str_has_suffix(prior_path, ".svgz"))
        && (g_str_has_suffix(filename, ".svg") || g_str_has_suffix(filename, ".svgz"))
        && (g_str_has_suffix(prior_path, ".svgz") == g_str_has_suffix(filename, ".svgz"));
    auto job = svg->begin_publication(copy.get(), filename,
                                     same_compression ? doc->lastKnownSerializedSize() : std::nullopt);
    if (job.route == "owned_bytes") return job;
    if (preparation.route) *preparation.route = job.route;
    // The size route retains the direct native writer and its existing semantics.
    if (Inkscape::IO::file_test(filename, G_FILE_TEST_EXISTS)
        && !Inkscape::IO::file_is_writable(filename)) throw Output::file_read_only();
    auto *previous_notice = _active_save_notice;
    bool const previous_interactive_native_save = _active_interactive_native_save;
    _active_save_notice = preparation.notice;
    _active_interactive_native_save = false;
    try {
        imp->save(this, copy.get(), filename);
    } catch (...) {
        _active_save_notice = previous_notice;
        _active_interactive_native_save = previous_interactive_native_save;
        throw;
    }
    _active_save_notice = previous_notice;
    _active_interactive_native_save = previous_interactive_native_save;
    if (preparation.direct_size_hint) {
        GStatBuf published_stat{};
        if (g_stat(filename, &published_stat) == 0 && published_stat.st_size >= 0)
            *preparation.direct_size_hint = static_cast<std::size_t>(published_stat.st_size);
        else
            *preparation.direct_size_hint = job.direct_size_hint;
    }
    return std::nullopt;
#else
    (void)doc; (void)filename; (void)preparation;
    return std::nullopt;
#endif
}

void
Output::save(SPDocument *doc, gchar const *filename, bool detachbase)
{
    // Preserve the existing three-argument symbol/default signature; direct
    // callers keep today's defaults (no identity rebase, dataloss or title sync).
    save(doc, filename, detachbase, SavePreparation{});
}

void
Output::save(SPDocument *doc, gchar const *filename, bool detachbase,
             SavePreparation const &preparation)
{
    if (!loaded())
        set_state(Extension::STATE_LOADED);

    if (loaded()) {
        using Clock = std::chrono::steady_clock;
        bool const timing = preparation.interactive_native_save
            && g_getenv("VACARDS_SAVE_TIMING")
            && std::strcmp(g_getenv("VACARDS_SAVE_TIMING"), "1") == 0;
        auto const prepare_begin = Clock::now();
        imp->setDetachBase(detachbase);
        auto new_doc = doc->copy();
        // Preparation runs on the single working copy, before the object tree
        // and processing actions, exactly where the old pre-I/O live mutation
        // used to be observed. The original document is never modified here.
        apply_save_preparation(new_doc.get(), filename, *this, preparation);
        new_doc->ensureUpToDate();
        run_processing_actions(new_doc.get());
        if (timing) {
            auto const ms = std::chrono::duration<double, std::milli>(Clock::now() - prepare_begin).count();
            std::fprintf(stderr, "VACARDS_SAVE_TIMING span=prepare_copy ms=%.3f\n", ms);
        }
        auto *previous_notice = _active_save_notice;
        bool const previous_interactive_native_save = _active_interactive_native_save;
        _active_save_notice = preparation.notice;
        _active_interactive_native_save = preparation.interactive_native_save;
        try {
            imp->save(this, new_doc.get(), filename);
        } catch (...) {
            _active_save_notice = previous_notice;
            _active_interactive_native_save = previous_interactive_native_save;
            throw;
        }
        _active_save_notice = previous_notice;
        _active_interactive_native_save = previous_interactive_native_save;
    } else {
        throw save_failed();
    }
}

void Output::report_save_notice(std::string const &notice)
{
    if (notice.empty()) return;
    if (_active_save_notice) {
        if (!_active_save_notice->empty()) *_active_save_notice += "\n";
        *_active_save_notice += notice;
    } else {
        g_message("%s", notice.c_str());
    }
}

/**
    \return  None
    \brief   Save a rendered png as a raster output
    \param   png_filename source png file.
    \param   filename  File to save the raster as

*/
void
Output::export_raster(const SPDocument *doc, std::string png_filename, gchar const *filename, bool detachbase)
{
    if (!loaded())
        set_state(Extension::STATE_LOADED);

    if (loaded()) {
        imp->setDetachBase(detachbase);
        imp->export_raster(this, doc, png_filename, filename);
    } else {
        throw save_failed();
    }
}

/**
 * Adds a valid extension to the filename if it's missing. Only used by export-single.cpp.
 */
void
Output::add_extension(std::string &filename)
{
    auto current_ext = Inkscape::IO::get_file_extension(filename);
    if (extension && current_ext != extension) {
        filename = filename + extension;
    }
}

/**
    \return  True if the filename matches
    \brief   Match filename to extension that can open it.
*/
bool
Output::can_save_filename(gchar const *filename) const
{
    // Ignore raster Outputs, because those are only used as part of the "export-as" pipeline,
    // not for the "save/save-as" pipeline. Such output extensions don't override save().
    if (is_raster()) {
        return false;
    }

    gchar *filenamelower = g_utf8_strdown(filename, -1);
    gchar *extensionlower = g_utf8_strdown(extension, -1);
    bool result = g_str_has_suffix(filenamelower, extensionlower);
    g_free(filenamelower);
    g_free(extensionlower);
    return result;
}

} }  /* namespace Inkscape, Extension */

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
