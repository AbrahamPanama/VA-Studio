// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * This is the code that moves all of the SVG loading and saving into
 * the module format.  Really Inkscape is built to handle these formats
 * internally, so this is just calling those internal functions.
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Ted Gould <ted@gould.cx>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2002-2003 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <giomm/file.h>
#include <glib/gstdio.h>
#include <glibmm/miscutils.h>

#include <cerrno>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <sys/stat.h>

#include "document.h"
#include "inkscape.h"
#include "preferences.h"
#include "extension/output.h"
#include "extension/input.h"
#include "extension/system.h"
#include "svg.h"
#include "display/cairo-utils.h"
#include "extension/system.h"
#include "extension/output.h"

#include "object/sp-image.h"
#include "object/sp-root.h"

#include "util/units.h"
#include "util-string/ustring-format.h"

#include "selection-chemistry.h"
#include "io/existing-file-replacement.h"
#include "io/save-path-split.h"
#include "io/document-file-transaction.h"
#include "io/sys.h"
#include "io/stream/bufferstream.h"
#include "io/stream/uristream.h"
#include "xml/repr.h"
#include "xml/repr-save-output-stream.h"
#include "xml/document.h"
#include "gc-anchored.h"
#include "extension/internal/svg-publication.h"

// TODO due to internal breakage in glibmm headers, this must be last:
#include <glibmm/i18n.h>

namespace Inkscape::Extension::Internal {

namespace { std::atomic<unsigned> native_serializations{0}; }

SerializationSnapshot::SerializationSnapshot(Inkscape::XML::Document *document) noexcept
    : _document(document)
    , _owner(std::this_thread::get_id())
{}

SerializationSnapshot::~SerializationSnapshot()
{
    if (!_document) return;
    if (std::this_thread::get_id() != _owner) {
        // Releasing a GC anchor off the initiating thread could corrupt the
        // collector. Leak rather than crash; this is a programming error.
        g_critical("Save snapshot released off its initiating thread; leaking it");
        return;
    }
    Inkscape::GC::release(_document);
}
void reset_native_serialization_count_for_testing() noexcept { native_serializations.store(0); }
unsigned native_serialization_count_for_testing() noexcept { return native_serializations.load(); }

#include "clear-n_.h"

/**
    \return   None
    \brief    What would an SVG editor be without loading/saving SVG
              files.  This function sets that up.

    For each module there is a call to Inkscape::Extension::build_from_mem
    with a rather large XML file passed in.  This is a constant string
    that describes the module.  At the end of this call a module is
    returned that is basically filled out.  The one thing that it doesn't
    have is the key function for the operation.  And that is linked at
    the end of each call.
*/
void
Svg::init()
{
    // clang-format off
    /* SVG in */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("SVG Input") "</name>\n"
            "<id>" SP_MODULE_KEY_INPUT_SVG "</id>\n"
            SVG_COMMON_INPUT_PARAMS
            "<input priority='1'>\n"
                "<extension>.svg</extension>\n"
                "<mimetype>image/svg+xml</mimetype>\n"
                "<filetypename>" N_("Scalable Vector Graphic (*.svg)") "</filetypename>\n"
                "<filetypetooltip>" N_("VA Studio native file format and W3C standard") "</filetypetooltip>\n"
            "</input>\n"
        "</inkscape-extension>", std::make_unique<Svg>());

    /* SVG out Inkscape */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("SVG Output VA Studio") "</name>\n"
            "<id>" SP_MODULE_KEY_OUTPUT_SVG_INKSCAPE "</id>\n"
            "<output is_exported='true' priority='1'>\n"
                "<extension>.svg</extension>\n"
                "<mimetype>image/x-inkscape-svg</mimetype>\n"
                "<filetypename>" N_("Inkscape SVG (*.svg) — VA Studio native format") "</filetypename>\n"
                "<filetypetooltip>" N_("VA Studio native SVG format with Inkscape extensions") "</filetypetooltip>\n"
                "<dataloss>false</dataloss>\n"
            "</output>\n"
            "<action>prune-proprietary-namespaces</action>\n"
            "<action>set-svg-version-2</action>\n"
            "<action>set-inkscape-version</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>reverse-auto-start-markers</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>remove-marker-context-paint</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>set-svg-version-1</action>\n"
            "<action pref='/options/svgexport/text_insertfallback'>insert-text-fallback</action>\n"
            "<action pref='/options/svgexport/mesh_insertpolyfill'>insert-mesh-polyfill</action>\n"
            "<action pref='/options/svgexport/hatch_insertpolyfill'>insert-hatch-polyfill</action>\n"
        "</inkscape-extension>", std::make_unique<Svg>());

    /* SVG out */
    Inkscape::Extension::build_from_mem(
        "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>" N_("SVG Output") "</name>\n"
            "<id>" SP_MODULE_KEY_OUTPUT_SVG "</id>\n"
            "<output is_exported='true' priority='2'>\n"
                "<extension>.svg</extension>\n"
                "<mimetype>image/svg+xml</mimetype>\n"
                "<filetypename>" N_("Plain SVG (*.svg)") "</filetypename>\n"
                "<filetypetooltip>" N_("Scalable Vector Graphics format as defined by the W3C") "</filetypetooltip>\n"
            "</output>\n"
            "<action>set-svg-version-2</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>reverse-auto-start-markers</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>remove-marker-context-paint</action>\n"
            "<action pref='!/dialogs/save_as/enable_svgexport'>set-svg-version-1</action>\n"
            "<action pref='/options/svgexport/text_insertfallback'>insert-text-fallback</action>\n"
            "<action>prune-proprietary-namespaces</action>\n"
            "<action>prune-inkscape-namespaces</action>\n"
        "</inkscape-extension>", std::make_unique<Svg>());
    // clang-format on

    return;
}


/**
    \return    A new document just for you!
    \brief     This function takes in a filename of a SVG document and
               turns it into a SPDocument.
    \param     mod   Module to use
    \param     uri   The path or URI to the file. 
                     FIXME: Path in UTF8 or in platform-native encoding?
                     It seems like the variable is used as both.
                     Please change to Gio::File here and in the called functions to avoid the confusion.

    This function is really simple, it just calls sp_document_new...
    That's BS, it does all kinds of things for importing documents
    that probably should be in a separate function.

    Most of the import code was copied from gdkpixpuf-input.cpp.
*/
std::unique_ptr<SPDocument> Svg::open(Inkscape::Extension::Input *mod, char const *uri, bool is_importing)
{
    g_assert(mod);

    // File-backed SVG opens require a complete, well-formed input. Legacy
    // recovery remains available to other XML readers and in-memory callers.
    auto open_file_checked = [](char const *filename) {
        XmlReadReport report;
        auto result = SPDocument::createNewDoc(filename, false, nullptr, &report);
        if (!result && report.parsed && report.well_formed && !report.input_error) {
            // Well-formed XML with a non-SVG root is a format mismatch.
            return result;
        }
        if (!result && (report.parsed || report.input_error)) {
            auto detail = report.message.empty() ? std::string("The SVG file is incomplete or malformed.")
                                                 : report.message;
            if (report.line > 0) detail = "Line " + std::to_string(report.line) + ": " + detail;
            throw Inkscape::Extension::Input::open_damaged(std::move(detail));
        }
        return result;
    };

    // This is only used at the end... but it should go here once uri stuff is fixed.
    auto file = Gio::File::create_for_commandline_arg(uri);
    const auto path = file->get_path();

    // Fixing this means fixing a whole string of things.
    // if (path.empty()) {
    //     // We lied, the uri wasn't a uri, try as path.
    //     file = Gio::File::create_for_path(uri);
    // }

    // std::cout << "Svg::open: uri in: " << uri << std::endl;
    // std::cout << "         : uri:    " << file->get_uri() << std::endl;
    // std::cout << "         : scheme: " << file->get_uri_scheme() << std::endl;
    // std::cout << "         : path:   " << file->get_path() << std::endl;
    // std::cout << "         : parse:  " << file->get_parse_name() << std::endl;
    // std::cout << "         : base:   " << file->get_basename() << std::endl;

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    // Get import preferences.
    bool ask_svg                   = prefs->getBool(  "/dialogs/import/ask_svg");
    Glib::ustring import_mode_svg  = prefs->getString("/dialogs/import/import_mode_svg");
    Glib::ustring scale            = prefs->getString("/dialogs/import/scale");

    // Selecting some of the pages (via command line) in some future update
    // we could add an option which would allow user page selection.
    auto page_nums = INKSCAPE.get_pages();

    // If we popped up a window asking about import preferences, get values from
    // there and update preferences.
    if(mod->get_gui() && ask_svg) {
        ask_svg         = !mod->get_param_bool("do_not_ask");
        import_mode_svg =  mod->get_param_optiongroup("import_mode_svg");
        scale           =  mod->get_param_optiongroup("scale");

        prefs->setBool(  "/dialogs/import/ask_svg",         ask_svg);
        prefs->setString("/dialogs/import/import_mode_svg", import_mode_svg );
        prefs->setString("/dialogs/import/scale",           scale );
    }

    bool const import_pages = import_mode_svg == "pages";
    bool const import_new = import_mode_svg == "new";

    // Do we "import" as <image>?
    if (is_importing && import_mode_svg != "include" && !import_pages && !import_new) {
        // We import!

        // New wrapper document.
        auto doc = SPDocument::createNewDoc(nullptr, true);

        // Imported document
        auto ret = open_file_checked(uri);

        if (!ret) {
            return nullptr;
        }

        // What is display unit doing here?
        Glib::ustring display_unit = doc->getDisplayUnit()->abbr;
        double width = ret->getWidth().value(display_unit);
        double height = ret->getHeight().value(display_unit);
        if (width < 0 || height < 0) {
            return nullptr;
        }

        // Create image node
        Inkscape::XML::Document *xml_doc = doc->getReprDoc();
        Inkscape::XML::Node *image_node = xml_doc->createElement("svg:image");

        // Set default value as we honor "preserveAspectRatio".
        image_node->setAttribute("preserveAspectRatio", "none");

        double svgdpi = mod->get_param_float("svgdpi");
        image_node->setAttribute("inkscape:svg-dpi", Inkscape::ustring::format_classic(svgdpi));

        image_node->setAttribute("width", Inkscape::ustring::format_classic(width));
        image_node->setAttribute("height", Inkscape::ustring::format_classic(height));

        // This is actually "image-rendering"
        Glib::ustring scale = prefs->getString("/dialogs/import/scale");
        if( scale != "auto") {
            SPCSSAttr *css = sp_repr_css_attr_new();
            sp_repr_css_set_property(css, "image-rendering", scale.c_str());
            sp_repr_css_set(image_node, css, "style");
            sp_repr_css_attr_unref( css );
        }

        // Do we embed or link?
        if (import_mode_svg == "embed") {
            std::unique_ptr<Inkscape::Pixbuf> pb(Inkscape::Pixbuf::create_from_file(uri, svgdpi));
            if(pb) {
                sp_embed_svg(image_node, uri);
            }
        } else {
            // Convert filename to uri (why do we need to do this, we claimed it was already a uri).
            gchar* _uri = g_filename_to_uri(uri, nullptr, nullptr);
            if(_uri) {
                // if (strcmp(_uri, uri) != 0) {
                //     std::cout << "Svg::open: _uri != uri! " << _uri << ":" << uri << std::endl;
                // }
                image_node->setAttribute("xlink:href", _uri);
                g_free(_uri);
            } else {
                image_node->setAttribute("xlink:href", uri);
            }
        }

        // Add the image to a layer.
        Inkscape::XML::Node *layer_node = xml_doc->createElement("svg:g");
        layer_node->setAttribute("inkscape:groupmode", "layer");
        layer_node->setAttribute("inkscape:label", "Image");
        doc->getRoot()->appendChildRepr(layer_node);
        layer_node->appendChild(image_node);
        Inkscape::GC::release(image_node);
        Inkscape::GC::release(layer_node);
        fit_canvas_to_drawing(doc.get());

        // Set viewBox if it doesn't exist. What is display unit doing here?
        if (!doc->getRoot()->viewBox_set) {
            doc->setViewBox(Geom::Rect::from_xywh(0, 0, doc->getWidth().value(doc->getDisplayUnit()), doc->getHeight().value(doc->getDisplayUnit())));
        }
        return doc;
    }

    // We are not importing as <image>. Open as new document.

    // Try to open non-local file (when does this occur?).
    if (!file->get_uri_scheme().empty()) {
        if (path.empty()) {
            try {
                char *contents;
                gsize length;
                file->load_contents(contents, length);
                return SPDocument::createNewDocFromMem({contents, length});
            } catch (Gio::Error &e) {
                g_warning("Could not load contents of non-local URI %s\n", uri);
                return nullptr;
            }
        } else {
            // Do we ever get here and does this actually work?
            uri = path.c_str();
        }
    }

    auto doc = open_file_checked(uri);

    // Page selection is achieved by removing any page not in the found list, the exports
    // Can later figure out how they'd like to process the remaining pages.
    if (doc && !page_nums.empty()) {
        doc->prunePages(page_nums, true);
    }

    // Convert single page docs into multi page mode, and visa-versa if
    // we are importing. We never change the mode for opening.
    if (doc && is_importing && !import_new) {
        doc->setPages(import_pages);
    }

    return doc;
}

/**
    \return    None
    \brief     This is the function that does all of the SVG saves in
               Inkscape.  It detects whether it should do a Inkscape
               namespace save internally.
    \param     mod   Extension to use.
    \param     doc   Document to save.
    \param     uri   The filename to save the file to.
*/
#if defined(__APPLE__) || defined(_WIN32)
PublicationJob Svg::begin_publication(SPDocument *doc, char const *filename,
                                      std::optional<std::size_t> last_known_size)
{
    PublicationJob job;
    job.absolute_path = Glib::path_is_absolute(filename)
        ? filename : Glib::build_filename(Glib::get_current_dir(), filename);
    job.timing = g_strcmp0(g_getenv("VACARDS_SAVE_TIMING"), "1") == 0;
    job.test_hooks_enabled = Inkscape::IO::file_io_test_hooks_enabled();
    job.existing_file_options = Inkscape::IO::capture_existing_file_options(job.timing);
    job.inject_stage_write_failure = Inkscape::IO::file_io_test_hooks_enabled()
        && g_strcmp0(g_getenv("VACARDS_SAVE_TEST_STAGE_FAILURE"), "1") == 0;
    if (job.test_hooks_enabled) {
        char const *names[] = {"ADMISSION", "CREATE", "STAGE_WRITE", "SEAL", "PUBLISH", "CLEANUP"};
        for (unsigned i = 0; i < job.stage_hooks.size(); ++i) {
            auto const prefix = std::string("VACARDS_SAVE_TEST_") + names[i];
            if (auto const *value = g_getenv((prefix + "_STALL_MS").c_str())) {
                char *end = nullptr;
                auto const parsed = std::strtoul(value, &end, 10);
                if (end != value && *end == '\0' && parsed <= 60000)
                    job.stage_hooks[i].stall_ms = static_cast<unsigned>(parsed);
            }
            if (auto const *value = g_getenv((prefix + "_OUTCOME").c_str())) {
                using O = PublicationOutcome;
                if (i != 0 && i != 4 && std::strcmp(value, "failed") != 0) continue;
                if (!std::strcmp(value, "failed")) job.stage_hooks[i].failure = O::Failed;
                else if (!std::strcmp(value, "conflict")) job.stage_hooks[i].failure = O::Conflict;
                else if (!std::strcmp(value, "unsupported")) job.stage_hooks[i].failure = O::Unsupported;
                else if (!std::strcmp(value, "uncertain")) job.stage_hooks[i].failure = O::Uncertain;
                else if (!std::strcmp(value, "read_only")) job.stage_hooks[i].failure = O::ReadOnly;
            }
        }
    }
    auto const options = sp_repr_rebased_save_options(filename,
        doc->getDocumentBase(), m_detachbase ? nullptr : filename);
    std::size_t cap = 1024ULL * 1024 * 1024;
    if (Inkscape::IO::file_io_test_hooks_enabled()) {
        if (auto const *value = g_getenv("VACARDS_SAVE_TEST_BUFFER_CAP")) {
            char *end = nullptr;
            auto const parsed = std::strtoull(value, &end, 10);
            if (end && *end == '\0' && parsed <= cap) cap = parsed;
        }
    }
    // Supervisor decision 2026-09-24: size pre-decision uses the document's last known serialized size, not a destination stat.
    if (last_known_size && *last_known_size > cap) {
        job.route = "direct_size";
        return job;
    }
    // Save slice 2: capture everything the writer needs here, on the
    // initiating thread, and defer the serialization itself to publish().
    // Preparation (attribute clean/sort per preferences, namespace plan) runs
    // on this already-copied document; the snapshot is then a standalone XML
    // copy that outlives the SPDocument copy and that nothing else references.
    using Clock = std::chrono::steady_clock;
    auto const snapshot_begin = Clock::now();
    DeferredSerialization deferred;
    deferred.options = sp_repr_capture_serializer_options();
    deferred.plan = sp_repr_prepare_serializer_plan(doc->getReprDoc(), SP_SVG_NS_URI);
    auto *snapshot = doc->getReprDoc()->duplicate(nullptr);
    job.snapshot_owner = std::make_shared<SerializationSnapshot>(snapshot);
    deferred.snapshot = snapshot;
    deferred.compress = options.compress;
    deferred.old_href_base = options.old_href_abs_base;
    deferred.new_href_base = options.new_href_abs_base;
    deferred.cap = cap;
    if (last_known_size && *last_known_size > 0) deferred.reserve = *last_known_size;
    deferred.inject_reserve_failure = Inkscape::IO::file_io_test_hooks_enabled()
        && g_strcmp0(g_getenv("VACARDS_SAVE_TEST_RESERVE_FAILURE"), "1") == 0;
    deferred.inject_buffer_failure = Inkscape::IO::file_io_test_hooks_enabled()
        && g_strcmp0(g_getenv("VACARDS_SAVE_TEST_BUFFER_FAILURE"), "1") == 0;
    job.deferred = std::move(deferred);
    if (job.timing) {
        auto const ms = std::chrono::duration<double, std::milli>(Clock::now() - snapshot_begin).count();
        std::fprintf(stderr, "VACARDS_SAVE_TIMING span=snapshot ms=%.3f\n", ms);
    }
    return job;
}

PublicationResult publish(PublicationJob job)
{
    PublicationResult result;
    result.route = job.route;
    if (job.route != "owned_bytes") {
        result.error = "direct route must be handled by the caller";
        return result;
    }
    using Clock = std::chrono::steady_clock;
    // Save slice 2: serialize the borrowed snapshot here, possibly on the worker,
    // before any destination access, so a serializer failure is a definite
    // pre-publication failure. Over the cap (or when the buffer cannot be
    // allocated) the same writer streams into the checked stage file instead,
    // which keeps "never refuse above the cap" without returning to the UI.
    // Nothing below allocates GC memory or touches the live document.
    std::function<void(FILE *)> stream_writer;
    std::optional<std::size_t> streamed_size;
    if (job.deferred) {
        auto const &deferred = *job.deferred;
        auto const serialize_begin = Clock::now();
        bool stream = false;
        Inkscape::IO::BufferOutputStream buffer(deferred.cap);
        if (deferred.reserve
            && (deferred.inject_reserve_failure || !buffer.reserve(*deferred.reserve))) {
            stream = true;
            result.route = "direct_allocation";
        }
        if (!stream) {
            if (deferred.inject_buffer_failure) buffer.mark_failed();
            try {
                if (job.test_hooks_enabled) ++native_serializations;
                sp_repr_write_prepared(deferred.snapshot, buffer, deferred.compress, deferred.options,
                                       deferred.plan, deferred.old_href_base.c_str(),
                                       deferred.new_href_base.c_str());
            } catch (std::exception const &e) {
                if (buffer.failure_reason() == Inkscape::IO::BufferOutputStream::Failure::CapExceeded
                    || buffer.failure_reason() == Inkscape::IO::BufferOutputStream::Failure::Allocation) {
                    buffer.discard();
                    stream = true;
                    result.route = "direct_cap_or_allocation";
                } else {
                    if (job.timing) {
                        auto const ms = std::chrono::duration<double, std::milli>(Clock::now() - serialize_begin).count();
                        std::fprintf(stderr, "VACARDS_SAVE_TIMING span=serialize_compress ms=%.3f\n", ms);
                    }
                    result.outcome = PublicationOutcome::Failed;
                    result.error = e.what();
                    return result;
                }
            } catch (...) {
                // Nothing reached the destination yet: a definite failure.
                result.outcome = PublicationOutcome::Failed;
                result.error = "snapshot serialization failed";
                return result;
            }
        }
        if (job.timing) {
            auto const ms = std::chrono::duration<double, std::milli>(Clock::now() - serialize_begin).count();
            std::fprintf(stderr, "VACARDS_SAVE_TIMING span=serialize_compress ms=%.3f\n", ms);
            if (stream) std::fprintf(stderr, "VACARDS_SAVE_TIMING route=%s\n", result.route.c_str());
        }
        if (!stream) {
            job.bytes = buffer.release_bytes();
        } else {
            stream_writer = [&job, &streamed_size](FILE *stage) {
                if (job.inject_stage_write_failure && job.test_hooks_enabled)
                    throw std::runtime_error("injected staging write failure");
                auto const &deferred = *job.deferred;
                auto const start = std::ftell(stage);
                if (job.test_hooks_enabled) ++native_serializations;
                Inkscape::IO::FileOutputStream out(stage);
                sp_repr_write_prepared(deferred.snapshot, out, deferred.compress, deferred.options,
                                       deferred.plan, deferred.old_href_base.c_str(),
                                       deferred.new_href_base.c_str());
                auto const end = std::ftell(stage);
                if (start >= 0 && end >= start) streamed_size = static_cast<std::size_t>(end - start);
            };
        }
    }
    if (!stream_writer) result.serialized_size = job.bytes.size();
    std::optional<PublicationOutcome> injected_publish_outcome;
    auto hook = [&](unsigned stage) {
        if (!job.test_hooks_enabled) return false;
        auto const &settings = job.stage_hooks[stage];
        if (settings.stall_ms) std::this_thread::sleep_for(std::chrono::milliseconds(settings.stall_ms));
        if (!settings.failure) return false;
        result.outcome = *settings.failure;
        if (stage == 4) injected_publish_outcome = *settings.failure;
        result.error = "injected publication stage failure";
        return true;
    };
    // Match file_is_writable's mode-bit policy without a GLib warning path.
    auto quiet_writable = [](std::string const &path) {
        auto *native = g_filename_from_utf8(path.c_str(), -1, nullptr, nullptr, nullptr);
        if (!native) return true;
        GStatBuf stat{};
        bool const writable = g_lstat(native, &stat) == 0 && (stat.st_mode & S_IWRITE) != 0;
        g_free(native);
        return writable;
    };
    auto report_span = [timing = job.timing](char const *name, Clock::time_point begin) {
        if (timing) {
            auto const ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            std::fprintf(stderr, "VACARDS_SAVE_TIMING span=%s ms=%.3f\n", name, ms);
        }
    };
    GStatBuf destination_stat{};
    auto const admission_begin = Clock::now();
    if (hook(0)) return result;
    if (g_lstat(job.absolute_path.c_str(), &destination_stat) == 0) {
        report_span("admission_probe", admission_begin);
        if (!quiet_writable(job.absolute_path)) {
            result.outcome = PublicationOutcome::ReadOnly;
            result.error = "destination is write protected";
            return result;
        }
        if (job.test_hooks_enabled) job.existing_file_options.stage_observer = hook;
        auto const stored = stream_writer
            ? Inkscape::IO::replace_existing_local_file(job.absolute_path, stream_writer,
                                                         job.existing_file_options)
            : Inkscape::IO::replace_existing_local_file(
                  job.absolute_path, std::span<std::byte const>(job.bytes),
                  job.inject_stage_write_failure && job.test_hooks_enabled,
                  job.existing_file_options);
        if (stream_writer) result.serialized_size = streamed_size;
        switch (stored.outcome) {
        case Inkscape::IO::ExistingFileOutcome::Published:
            result.outcome = PublicationOutcome::Published;
            if (!stored.error.empty()) {
                if (!stored.recovery_path.empty()) {
                    result.notice += "\nA possible old recovery copy remains at "
                        + stored.recovery_path + "; inspect it before deleting it. ";
                } else result.notice += "\n";
                result.notice += stored.error;
            }
            break;
        case Inkscape::IO::ExistingFileOutcome::Unsupported:
            result.outcome = PublicationOutcome::Unsupported; break;
        case Inkscape::IO::ExistingFileOutcome::Conflict:
            result.outcome = PublicationOutcome::Conflict; break;
        case Inkscape::IO::ExistingFileOutcome::Uncertain:
            result.outcome = PublicationOutcome::Uncertain; break;
        case Inkscape::IO::ExistingFileOutcome::FailedBeforePublication:
            result.outcome = PublicationOutcome::Failed; break;
        }
        result.error = stored.error;
        result.recovery_path = stored.recovery_path;
        if (injected_publish_outcome) result.outcome = *injected_publish_outcome;
        return result;
    }
    if (errno != ENOENT) return result;
    report_span("admission_probe", admission_begin);
    auto const parts = Inkscape::IO::split_save_path(job.absolute_path);
    if (!parts) return result;
    std::string const &parent = parts->parent;
    namespace Tx = Inkscape::IO::DocumentTransaction;
    Tx::LogicalTarget target{job.absolute_path, parts->name, parent};
    if (g_mkdir_with_parents(parent.c_str(), 0777) != 0) return result;
    auto calls = Tx::make_platform_system_calls();
    Tx::FailureKind failure = Tx::FailureKind::None;
    std::string error;
    std::function<bool(unsigned)> observer;
    if (job.test_hooks_enabled) observer = hook;
    auto transaction = Tx::NewDocumentFile::create(std::move(target), *calls, failure,
                                                   error, /*retain_on_unsupported=*/true, observer);
    report_span("admission_stage_create", admission_begin);
    if (!transaction) {
        result.error = error;
        result.outcome = failure == Tx::FailureKind::Unsupported
            ? PublicationOutcome::Unsupported : PublicationOutcome::Failed;
        return result;
    }
    auto const stage_begin = Clock::now();
    bool const written = stream_writer
        ? transaction->write(stream_writer, failure, error)
        : transaction->write_bytes(std::span<std::byte const>(job.bytes), failure, error,
                                   job.inject_stage_write_failure && job.test_hooks_enabled);
    if (stream_writer) result.serialized_size = streamed_size;
    report_span("stage_write", stage_begin);
    auto const seal_begin = Clock::now();
    bool const sealed = written && transaction->seal(failure, error);
    if (written) report_span("flush_sync_close", seal_begin);
    if (!transaction || !written || !sealed) {
        result.outcome = failure == Tx::FailureKind::Unsupported
            ? PublicationOutcome::Unsupported : PublicationOutcome::Failed;
        result.error = error;
        return result;
    }
    auto const publish_begin = Clock::now();
    auto const stored = transaction->publish();
    report_span("publish", publish_begin);
    if (job.timing) std::fprintf(stderr, "VACARDS_SAVE_TIMING span=metadata ms=0.000\n");
    auto const cleanup_begin = Clock::now();
    // Cleanup follows the publication syscall. A test failure here must never
    // claim definite nonpublication or allow a false-clean document.
    transaction.reset();
    report_span("cleanup", cleanup_begin);
    result.error = stored.error;
    result.recovery_path = stored.recovery_path;
    result.recovery_path_available = stored.staging_availability == Tx::StagingAvailability::Available;
    switch (stored.status) {
    case Tx::PublicationStatus::Published: result.outcome = PublicationOutcome::Published; break;
    case Tx::PublicationStatus::Conflict: result.outcome = PublicationOutcome::Conflict; break;
    case Tx::PublicationStatus::Uncertain: result.outcome = PublicationOutcome::Uncertain; break;
    case Tx::PublicationStatus::Unsupported:
        result.outcome = PublicationOutcome::Unsupported;
        if (stored.recovery_retained && !stored.recovery_path.empty()) {
            if (!result.error.empty()) result.error += "; ";
            result.error += "a possible staged SVG remains at " + stored.recovery_path
                          + "; inspect it before deleting it";
        }
        break;
    default: result.outcome = PublicationOutcome::Failed; break;
    }
    if (injected_publish_outcome) result.outcome = *injected_publish_outcome;
    if (stored.status == Tx::PublicationStatus::Published && !stored.staging_cleanup_ok) {
        result.notice = "A possible staged SVG remains at " + stored.recovery_path
            + "; inspect it before deleting it.";
    }
    return result;
}
#endif // __APPLE__ || _WIN32

void
Svg::save(Inkscape::Extension::Output *mod, SPDocument *doc, gchar const *filename)
{
    g_return_if_fail(doc != nullptr);
    g_return_if_fail(filename != nullptr);

#if defined(__APPLE__) || defined(_WIN32)
    if (std::strcmp(filename, "-") == 0) {
        if (!sp_repr_save_rebased_file(doc->getReprDoc(), filename, SP_SVG_NS_URI,
                                      doc->getDocumentBase(),
                                      m_detachbase ? nullptr : filename)) {
            throw Inkscape::Extension::Output::save_failed();
        }
        return;
    }
    std::string const absolute = Glib::path_is_absolute(filename)
        ? filename : Glib::build_filename(Glib::get_current_dir(), filename);
    auto const options = sp_repr_rebased_save_options(filename,
        doc->getDocumentBase(), m_detachbase ? nullptr : filename);
    // Windows retains the reviewed staged direct writer for every native save.
#ifdef _WIN32
    bool owned_bytes = false;
#else
    bool owned_bytes = mod->is_interactive_native_save();
#endif
    char const *route = owned_bytes ? "owned_bytes" : "direct";
    bool const timing = owned_bytes && std::strcmp(g_getenv("VACARDS_SAVE_TIMING")
                                                   ? g_getenv("VACARDS_SAVE_TIMING") : "", "1") == 0;
    using Clock = std::chrono::steady_clock;
    auto report_span = [timing](char const *name, Clock::time_point begin) {
        if (timing) {
            auto const ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            std::fprintf(stderr, "VACARDS_SAVE_TIMING span=%s ms=%.3f\n", name, ms);
        }
    };
    Inkscape::IO::BufferOutputStream buffer([&] {
        // The production cap bounds retained serialized bytes. Tests may lower it.
        auto const *value = g_getenv("VACARDS_SAVE_TEST_BUFFER_CAP");
        constexpr std::size_t cap = 1024ULL * 1024 * 1024;
        if (!owned_bytes || !Inkscape::IO::file_io_test_hooks_enabled() || !value || !*value) return cap;
        char *end = nullptr;
        auto const parsed = std::strtoull(value, &end, 10);
        return end && *end == '\0' && parsed <= cap
            ? static_cast<std::size_t>(parsed) : cap;
    }());
    // A previous target size is a cheap estimate for ordinary Save. Do not
    // serialize known over-cap files twice, and reuse that size for capacity.
    GStatBuf prior_stat{};
    if (owned_bytes && g_stat(filename, &prior_stat) == 0 && prior_stat.st_size > 0) {
        auto const prior_size = static_cast<std::uintmax_t>(prior_stat.st_size);
        if (prior_size > buffer.maximum_size()) {
            owned_bytes = false;
            route = "direct_size";
        } else if (!buffer.reserve(static_cast<std::size_t>(prior_size))) {
            owned_bytes = false;
            route = "direct_allocation";
        }
    }
    // Test-only non-cap rejection exercises the definite-failure path.
    if (owned_bytes && Inkscape::IO::file_io_test_hooks_enabled()
        && g_strcmp0(g_getenv("VACARDS_SAVE_TEST_BUFFER_FAILURE"), "1") == 0) {
        buffer.mark_failed();
    }
    if (owned_bytes) {
        auto const begin = Clock::now();
        try {
            if (Inkscape::IO::file_io_test_hooks_enabled()) ++native_serializations;
            sp_repr_save_output_stream(doc->getReprDoc(), buffer, SP_SVG_NS_URI,
                options.compress, options.old_href_abs_base.c_str(),
                options.new_href_abs_base.c_str());
        } catch (std::exception const &e) {
            report_span("serialize_compress", begin);
            if (buffer.failure_reason() == Inkscape::IO::BufferOutputStream::Failure::CapExceeded
                || buffer.failure_reason() == Inkscape::IO::BufferOutputStream::Failure::Allocation) {
                buffer.discard();
                owned_bytes = false;
                route = "direct_cap_or_allocation";
            } else {
                throw Inkscape::Extension::Output::save_failed(e.what());
            }
        }
        report_span("serialize_compress", begin);
    }
    if (timing) std::fprintf(stderr, "VACARDS_SAVE_TIMING route=%s\n", route);
    auto bytes = owned_bytes ? buffer.release_bytes() : std::vector<std::byte>{};
    auto const write_staged_svg = [&](FILE *stage) {
        if (Inkscape::IO::file_io_test_hooks_enabled()
            && g_strcmp0(g_getenv("VACARDS_SAVE_TEST_STAGE_FAILURE"), "1") == 0)
            throw std::runtime_error("injected staging write failure");
        if (Inkscape::IO::file_io_test_hooks_enabled()) ++native_serializations;
        sp_repr_save_stream(doc->getReprDoc(), stage, SP_SVG_NS_URI,
            options.compress, options.old_href_abs_base.c_str(),
            options.new_href_abs_base.c_str());
    };

    // Existing local SVG files are written to a checked sibling before their
    // directory entry is replaced. lstat (rather than a following existence
    // check) ensures even a dangling symlink takes the conservative path.
    // GStatBuf is struct stat on Apple and the type g_lstat() takes on Windows.
    GStatBuf destination_stat{};
    auto const admission_begin = Clock::now();
    if (g_lstat(filename, &destination_stat) == 0) {
        report_span("admission_probe", admission_begin);
        auto const result = owned_bytes
            ? Inkscape::IO::replace_existing_local_file(absolute, std::move(bytes))
            : Inkscape::IO::replace_existing_local_file(absolute, write_staged_svg);
        switch (result.outcome) {
        case Inkscape::IO::ExistingFileOutcome::Published:
            if (!result.error.empty()) {
                std::string notice;
                if (!result.recovery_path.empty()) {
                    notice = "A possible old recovery copy remains at "
                        + result.recovery_path + "; inspect it before deleting it. ";
                }
                notice += result.error;
                mod->report_save_notice(notice);
            }
            return;
        case Inkscape::IO::ExistingFileOutcome::Unsupported:
            throw Inkscape::Extension::Output::save_unsupported(result.error);
        case Inkscape::IO::ExistingFileOutcome::Conflict:
            throw Inkscape::Extension::Output::save_conflict(result.error);
        case Inkscape::IO::ExistingFileOutcome::Uncertain:
            // A non-empty existing-file recovery_path means a prior complete
            // version MAY be retained there; it never proves the bytes. Do not
            // claim verified recovery from the path alone. The error carries
            // only the diagnostic reason; file.cpp owns the user-facing text.
            throw Inkscape::Extension::Output::save_uncertain(
                result.error, result.recovery_path, /*recovery_path_available=*/false);
        case Inkscape::IO::ExistingFileOutcome::FailedBeforePublication:
            break;
        }
        throw Inkscape::Extension::Output::save_failed(result.error);
    }
    if (errno != ENOENT) {
        throw Inkscape::Extension::Output::save_failed();
    }
    report_span("admission_probe", admission_begin);

    // A new destination uses the established no-clobber transaction. If a
    // different process creates it after lstat, publication reports Conflict
    // instead of opening and truncating that process's file.
    namespace Tx = Inkscape::IO::DocumentTransaction;
    // Split the logical destination without rewriting the caller's bytes.
    auto const parts = Inkscape::IO::split_save_path(absolute);
    if (!parts) {
        throw Inkscape::Extension::Output::save_failed();
    }
    Tx::LogicalTarget target{absolute, parts->name, parts->parent};
    // fopen_utf8name() historically creates missing parent directories for
    // write mode. Keep that user-visible Save As behavior before staging.
    if (g_mkdir_with_parents(target.parent_dir.c_str(), 0777) != 0) {
        throw Inkscape::Extension::Output::save_failed();
    }
    auto calls = Tx::make_platform_system_calls();
    Tx::FailureKind failure = Tx::FailureKind::None;
    std::string error;
    auto transaction = Tx::NewDocumentFile::create(std::move(target), *calls, failure,
                                                   error, /*retain_on_unsupported=*/true);
    report_span("admission_stage_create", admission_begin);
    auto const stage_begin = Clock::now();
    bool const written = transaction && (owned_bytes
        ? transaction->write_bytes(std::move(bytes), failure, error)
        : transaction->write(write_staged_svg, failure, error));
    report_span("stage_write", stage_begin);
    auto const seal_begin = Clock::now();
    bool const sealed = written && transaction->seal(failure, error);
    if (written) report_span("flush_sync_close", seal_begin);
    if (!transaction || !written || !sealed) {
        if (failure == Tx::FailureKind::Unsupported) {
            throw Inkscape::Extension::Output::save_unsupported(error);
        }
        throw Inkscape::Extension::Output::save_failed(error);
    }
    auto const publish_begin = Clock::now();
    auto const result = transaction->publish();
    report_span("publish", publish_begin);
    // New-file publication has no inherited destination metadata to copy.
    if (timing) std::fprintf(stderr, "VACARDS_SAVE_TIMING span=metadata ms=0.000\n");
    auto const cleanup_begin = Clock::now();
    transaction.reset();
    report_span("cleanup", cleanup_begin);
    if (result.status == Tx::PublicationStatus::Published) {
        return;
    }
    if (result.status == Tx::PublicationStatus::Conflict) {
        throw Inkscape::Extension::Output::save_conflict(result.error);
    }
    if (result.status == Tx::PublicationStatus::Uncertain) {
        // The retained stage location is only signalled available when the
        // transaction reported its identity Available; Unverified is never
        // upgraded here. Available does not prove the retained bytes.
        throw Inkscape::Extension::Output::save_uncertain(
            result.error, result.recovery_path,
            result.staging_availability == Tx::StagingAvailability::Available);
    }
    if (result.status == Tx::PublicationStatus::Unsupported) {
        std::string error = result.error;
        if (result.recovery_retained && !result.recovery_path.empty()) {
            if (!error.empty()) error += "; ";
            error += "a possible staged SVG remains at " + result.recovery_path
                   + "; inspect it before deleting it";
        }
        throw Inkscape::Extension::Output::save_unsupported(error);
    }
    throw Inkscape::Extension::Output::save_failed();
#endif

    if (!sp_repr_save_rebased_file(doc->getReprDoc(), filename, SP_SVG_NS_URI,
                                   doc->getDocumentBase(),
                                   m_detachbase ? nullptr : filename)) {
        throw Inkscape::Extension::Output::save_failed();
    }
}

} // namespace Inkscape::Extension::Internal

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
