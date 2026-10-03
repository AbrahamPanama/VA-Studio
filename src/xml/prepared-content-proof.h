// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Read-only exact proof of prepared XML content
 *
 * Retains a private anchored native XML::Document duplicate of the source tree,
 * released through GC::release RAII, together with a nullable filename/base
 * identity. The duplicate shares the native immutable content/attribute buffers
 * created by the ordinary SimpleNode copy; no second SPDocument, parser,
 * serializer, normalization, or mutable view is involved.
 *
 * capture() validates the native duplicate against the source immediately and
 * returns nullptr (unavailable) on any exact mismatch, including the DOCUMENT
 * node fields that SimpleDocument::duplicate does not copy. matches() performs
 * the same exact const comparison plus filename/base nullness.
 *
 * This is an ordinary C++ owner; it is not GC-managed and exposes no mutable
 * Node/Document. The caller owns source lifetime/quiescence and the
 * contentRevision gate around the actual copy.
 */

#ifndef SEEN_INKSCAPE_XML_PREPARED_CONTENT_PROOF_H
#define SEEN_INKSCAPE_XML_PREPARED_CONTENT_PROOF_H

#include <memory>
#include <optional>
#include <string>

namespace Inkscape {
namespace XML {

class Document;

class PreparedContentProof
{
public:
    /**
     * Duplicate @a source into an owned snapshot and prove the copy is exact.
     *
     * @return the proof, or nullptr if the native duplicate did not exactly
     *         reproduce the source (including noncanonical DOCUMENT fields).
     */
    static std::unique_ptr<PreparedContentProof> capture(Document const &source,
                                                         char const *filename,
                                                         char const *base);

    /// Exact const comparison against @a source plus filename/base identity.
    bool matches(Document const &source, char const *filename, char const *base) const;

    /// Read-only access to the owned snapshot for native ownership inspection.
    /// Never exposes a mutable view.
    Document const &preparedDocument() const;

    ~PreparedContentProof();

    PreparedContentProof(PreparedContentProof const &) = delete;
    PreparedContentProof &operator=(PreparedContentProof const &) = delete;
    PreparedContentProof(PreparedContentProof &&) = delete;
    PreparedContentProof &operator=(PreparedContentProof &&) = delete;

private:
    struct DocumentRelease {
        void operator()(Document *document) const;
    };

    PreparedContentProof(std::unique_ptr<Document, DocumentRelease> snapshot,
                         std::optional<std::string> filename,
                         std::optional<std::string> base);

    std::unique_ptr<Document, DocumentRelease> _snapshot;
    std::optional<std::string> _filename;
    std::optional<std::string> _base;
};

} // namespace XML
} // namespace Inkscape

#endif
