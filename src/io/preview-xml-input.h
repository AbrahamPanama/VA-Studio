// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Bounded, non-loading XML input parse for future Welcome SVG admission.
 *
 * This is a prerequisite only. It parses bytes in memory with libxml2's
 * push/SAX2 parser while building a bounded libxml2 DOM, and returns the
 * untouched original bytes plus the tree. It does NOT admit any SVG, does not
 * classify resources, namespaces or reference graphs, and makes no claim that
 * the returned document is safe for native rendering. Those decisions belong
 * to a later consumer.
 *
 * Decoding is UTF-8 only and is validated before the parser sees anything.
 * The original bytes are copied once into an immutable owner and are never
 * normalized, rewritten or stripped (a UTF-8 BOM is preserved). DOCTYPE /
 * internal / external subsets, entity declarations, custom or parameter entity
 * references and every processing instruction are rejected before any loader
 * can run; predefined XML entities and numeric character references are
 * allowed. Node, depth, attributes-per-node, namespace-declaration and
 * qualified-name byte budgets are finite and charged before the native DOM
 * builder runs.
 *
 * No native document loading, no `SPDocument`/repr path, no serialization and
 * no filesystem or network access happen here.
 */
#ifndef SEEN_INKSCAPE_IO_PREVIEW_XML_INPUT_H
#define SEEN_INKSCAPE_IO_PREVIEW_XML_INPUT_H

#include "io/artwork-library-archive.h" // ArtworkLibrary::Cancelled
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <libxml/tree.h>

namespace Inkscape::IO {

/// Reuse the archive cancellation contract verbatim.
using Cancelled = ArtworkLibrary::Cancelled;

enum class PreviewXmlStatus {
    Parsed,   ///< bounded DOM built; original bytes retained untouched
    Rejected, ///< input refused; no tree returned
    Cancelled ///< caller cancellation observed before or during the parse
};

enum class PreviewXmlReason {
    Ok = 0,

    // Byte stage, enforced before any allocation/copy.
    EmptyInput,
    TooManyBytes,
    InvalidLimits,

    // Byte stage, after the single immutable copy.
    EmbeddedNul,
    InvalidUtf8,

    // Native parser metadata.
    /// Declared encoding is neither absent nor UTF-8/UTF8. A declaration the
    /// native parser refuses before this metadata check (e.g. UTF-16) surfaces
    /// as MalformedXml instead; both remain rejected with no document.
    UnsupportedEncoding,

    // Strict XML structure. Anything the parser or validator refuses lands here.
    MalformedXml,
    ForbiddenDoctype,
    ForbiddenEntity,
    ForbiddenParameterEntity,
    ForbiddenProcessingInstruction,

    // Finite budgets, charged before the native DOM builder runs.
    NodeBudget,
    DepthBudget,
    AttributeBudget,
    NamespaceBudget,
    NameBytesBudget,

    Cancelled,

    // Internal failure (allocation/unknown); never a policy decision.
    ParserAllocation,
    InternalFailure,
};

/// Finite budgets. Callers may only tighten these; the hard ceilings equal the
/// defaults, so no caller can raise them.
struct PreviewXmlLimits {
    std::size_t max_bytes = 4u * 1024u * 1024u; ///< raw input bytes, before copy
    std::size_t max_nodes = 20000u;             ///< element + text/CDATA/comment callbacks
    std::size_t max_depth = 64u;                ///< maximum element nesting
    std::size_t max_attributes_per_node = 128u; ///< attributes on one element
    std::size_t max_namespaces_per_element = 64u; ///< xmlns declarations on one element
    /// Composed `prefix:local` byte length. A name at or above this length is
    /// rejected; the default 256 matches the native repr qualified-name buffer.
    std::size_t max_name_bytes = 256u;
};

struct ParseResult;

/// Immutable owning result of a successful bounded parse. The tree is exposed
/// read-only; there are no public mutators and no test backdoors. The contained
/// `xmlDoc` is freed exactly once by this object.
class ParsedPreviewXml final {
public:
    ParsedPreviewXml(ParsedPreviewXml const &) = delete;
    ParsedPreviewXml &operator=(ParsedPreviewXml const &) = delete;
    ParsedPreviewXml(ParsedPreviewXml &&) noexcept;
    ParsedPreviewXml &operator=(ParsedPreviewXml &&) noexcept;
    ~ParsedPreviewXml();

    /// Borrowed read-only tree; valid for the lifetime of this object.
    xmlDoc const *document() const noexcept;
    /// Root element of `document()`, or null when the document has none.
    xmlNode const *root() const noexcept;

    /// The untouched original UTF-8 bytes; never a caller alias.
    std::shared_ptr<std::string const> const &original_bytes() const noexcept;
    PreviewXmlLimits limits() const noexcept;

    std::size_t node_count() const noexcept;
    std::size_t max_depth() const noexcept;
    std::size_t attribute_count() const noexcept;
    std::size_t namespace_count() const noexcept;

private:
    struct State;
    explicit ParsedPreviewXml(std::shared_ptr<State const> state);
    std::shared_ptr<State const> _state;

    friend ParseResult parse_preview_xml(std::span<unsigned char const> bytes,
                                         PreviewXmlLimits limits,
                                         Cancelled cancelled);
};

/// Result of `parse_preview_xml`. `Parsed` carries both the immutable byte
/// snapshot and the RAII document; `Rejected`/`Cancelled` carry an explicit
/// reason and no tree. The snapshot is present whenever input was copied
/// (every outcome except `EmptyInput`/`TooManyBytes`/`InvalidLimits`), but it is
/// also absent when the snapshot allocation itself fails, which returns
/// `Rejected`/`InternalFailure`.
struct ParseResult {
    PreviewXmlStatus status = PreviewXmlStatus::Rejected;
    PreviewXmlReason reason = PreviewXmlReason::EmptyInput;
    std::shared_ptr<std::string const> original_bytes;
    std::shared_ptr<ParsedPreviewXml const> parsed;
    std::size_t node_count = 0;
    std::size_t max_depth = 0;
    std::size_t attribute_count = 0;
    std::size_t namespace_count = 0;

    bool parsed_ok() const noexcept {
        return status == PreviewXmlStatus::Parsed && parsed != nullptr;
    }
};

/// Parse a bounded, non-loading XML input. Never loads a document or a
/// resource, never serializes, and never mutates the caller's bytes.
ParseResult parse_preview_xml(std::span<unsigned char const> bytes,
                              PreviewXmlLimits limits = {},
                              Cancelled cancelled = {});

std::string_view to_string(PreviewXmlStatus status);
std::string_view to_string(PreviewXmlReason reason);

} // namespace Inkscape::IO

#endif // SEEN_INKSCAPE_IO_PREVIEW_XML_INPUT_H
