// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_REPR_SAVE_OUTPUT_STREAM_H
#define SEEN_REPR_SAVE_OUTPUT_STREAM_H

#include <glib.h>
#include <string>
#include <utility>
#include <vector>

namespace Inkscape::XML { class Document; }
namespace Inkscape::IO { class OutputStream; }

// Checked serializer entry for caller-owned output streams, including buffers.
// A successful call closes the output stream; a failure throws StreamException.
void sp_repr_save_output_stream(Inkscape::XML::Document *doc, Inkscape::IO::OutputStream &output,
                                char const *default_ns = nullptr, bool compress = false,
                                char const *old_href_base = nullptr,
                                char const *new_href_base = nullptr);

/// Serializer preferences, captured on the UI thread.
struct SerializerOptions
{
    bool inlineattrs = false; ///< /options/svgoutput/inlineattrs
    int indent = 2;           ///< /options/svgoutput/indent
};

/// Result of the UI-thread preparation of a document for writing.
struct SerializerPlan
{
    GQuark elide_prefix = 0;                                  ///< namespace prefix written as the default xmlns
    std::vector<std::pair<GQuark, std::string>> root_extras;  ///< xmlns attributes appended to the root element
};

/// Reads the serializer preferences. UI thread only.
SerializerOptions sp_repr_capture_serializer_options();

/// UI thread only. The plan describes a single-root document (every parsed SVG); the tree must not change
/// between preparation and sp_repr_write_prepared(). Performs the root-element preparation the writer used to do while writing: attribute cleaning
/// and sorting (per /options/svgoutput preferences; this MUTATES the tree exactly as before), namespace collection,
/// default-namespace elision and the xmlns attribute list. Applies to the first element child of @a doc.
SerializerPlan sp_repr_prepare_serializer_plan(Inkscape::XML::Document *doc, char const *default_ns);

/// Writes a prepared document. Reads no preferences, allocates no GC memory and does not modify the tree.
/// Same checked-close contract as sp_repr_save_output_stream (closes @a output on success; on failure marks it
/// failed, abandons gzip and rethrows).
void sp_repr_write_prepared(Inkscape::XML::Document const *doc, Inkscape::IO::OutputStream &output, bool compress,
                            SerializerOptions const &options, SerializerPlan const &plan,
                            char const *old_href_base = nullptr, char const *new_href_base = nullptr);

#endif
