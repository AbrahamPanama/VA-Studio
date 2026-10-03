// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Bounded W5 resource policy over an already-built `ParsedPreviewXml` DOM.
 *
 * This is a conservative first subset. It reads the const libxml2 tree produced
 * by `parse_preview_xml` and decides whether the document may be handed to the
 * native loader. It performs only string, id and integer accounting: no
 * geometry, no bbox, no native `SPDocument`/`SPFactory`, no native loader, no
 * filesystem, no network, no serializer and no global-namespace mutation.
 *
 * `Candidate` means "no resource-policy escape was found in this bounded
 * input"; it is explicitly NOT a renderer-safety, fidelity, sandbox or
 * whole-Welcome guarantee. `Unsupported` is a known-but-deferred or unknown
 * feature kept whole as a placeholder; it is never stripped or rewritten.
 * `Rejected` is a definite policy violation. `Cancelled` is caller
 * cancellation, returned whenever cancellation is observed. The status is the
 * deterministic first terminal reason reached in walk/graph order; there is no
 * global status-precedence scan over the whole input, and both `Rejected` and
 * `Unsupported` forbid a native load.
 *
 * The result borrows no DOM pointer and retains no tree handle: it carries only
 * the exact original-bytes owner (the same `std::shared_ptr` as the parse
 * result) and scalar accounting. That owner keeps the bytes alive after the
 * parse, so the result may outlive the `ParsedPreviewXml`; it still cannot
 * mutate the tree and exposes no `xmlDoc`/`xmlNode`.
 *
 * Namespace canonicalization reproduces only the fixed URI->prefix map from
 * `src/xml/repr-util.cpp:72-131` + `src/xml/repr.h:24-32`; it never calls
 * `sp_xml_ns_uri_prefix`/`sp_xml_ns_register_defaults`. The XML-reserved URI is
 * handled by a narrow, separately proven rule: the bounded parser already
 * refuses a non-`xml` prefix bound to the XML namespace URI, a wrong URI on the
 * `xml` prefix, and any rebinding, so only the standard `xml` prefix can reach
 * the module; only `xml:space` and `xml:lang` are admitted, and every other
 * XML-namespace item (including `xml:base`) is `Unsupported`.
 *
 * First-subset dispatch is derived from `sp-factory.cpp:139-310` and the
 * attribute reads in `src/attributes.cpp`; any `sodipodi:type` value is
 * `Unsupported` (`UnknownDispatch`) with no arc/star/spiral promotion.
 */
#ifndef SEEN_INKSCAPE_IO_PREVIEW_SVG_RESOURCE_POLICY_H
#define SEEN_INKSCAPE_IO_PREVIEW_SVG_RESOURCE_POLICY_H

#include "io/preview-css-admission.h"
#include "io/preview-xml-input.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace Inkscape::IO {

enum class PreviewResourcePolicyStatus {
    Candidate,   ///< no policy escape found; native load may be attempted
    Unsupported, ///< known-but-deferred or unknown feature, kept whole
    Rejected,    ///< definite policy violation
    Cancelled,   ///< caller cancellation; terminal
};

enum class PreviewResourcePolicyReason {
    Ok = 0,

    // Input / structural accounting.
    NotParsed,
    NodeBudget,
    AttributeBudget,
    NameBytesBudget,
    DuplicateId,

    // Namespace / element / attribute / dispatch.
    UnknownNamespace,
    NamespaceRepair,
    UnknownElement,
    UnknownAttribute,
    UnknownDispatch,
    ForeignMetadata,
    XmlNamespaceFeatureUnsupported,

    // CSS (mapped from the accepted primitive).
    CssRejected,
    CssUnsupported,

    // Reference graph.
    AmbiguousReference,
    UnresolvedReference,
    ReferenceBudget,
    ReferenceDepth,
    ReferenceCycle,
    ReferenceFanoutBudget,
    ExpandedWorkBudget,
    ValidationWorkBudget,

    // Whole-document deferrals.
    UnsupportedUseResourceInheritance,
    UnsupportedStylesheetReferences,
    BitmapOrImageFeature,
    IccColorProfile,
    ActivePathEffect,
    UnsupportedElement,

    Cancelled,
    InternalFailure,
};

struct PreviewResourcePolicyLimits {
    /// Policy node ceiling, pre-enforced against the parser's total node count
    /// (elements + text/CDATA/comment callbacks), so it is conservative and
    /// counts all parser nodes, not only elements. The parser hard ceiling is
    /// 20000, which keeps element `int` indices safe.
    std::size_t max_nodes = 20000;
    /// Attribute ceiling, pre-enforced against the parser's attribute total.
    std::size_t max_attributes = 65536;
    std::size_t max_name_bytes = 256;      ///< canonical qname, rejected at >=
    std::size_t max_references = 4096;     ///< local #fragment occurrences
    std::size_t max_reference_depth = 32;  ///< longest reference-path depth
    /// Conservative expanded render/loader estimate ceiling. The estimate is
    /// `expanded[root] = 1 + sum(expanded[target])` over every outgoing edge
    /// with duplicate edges counted separately; it includes defs/resources and
    /// is an upper bound, never an exact native-workload claim. A true sum that
    /// exceeds `size_t` is detected independently of the comparison, so it is
    /// always `Rejected`/`ExpandedWorkBudget`, never an admissible `Candidate`,
    /// even when this limit is `SIZE_MAX`.
    std::size_t max_expanded_work = 20000;
    /// Unique validation/traversal work ceiling: resolved references, built
    /// adjacency edges, inherited-propagation visits, cycle DFS steps and both
    /// DP steps. Charged before each unit of work, so a graph that would exceed
    /// it is rejected rather than explored. Distinct from the conservative
    /// `max_expanded_work` (clone expansion, not unique work).
    std::size_t max_validation_work = 100000;
    std::size_t max_fanout = 1024;         ///< outgoing edges per node, saturating
    CssLimits css = {};                    ///< aggregate across the whole document
};

/// Owning, DOM-free decision summary. Carries the exact original-bytes owner
/// and scalar accounting only; never a borrowed `xmlDoc`/`xmlNode`.
struct PreviewResourcePolicyResult {
    PreviewResourcePolicyStatus status = PreviewResourcePolicyStatus::Rejected;
    PreviewResourcePolicyReason reason = PreviewResourcePolicyReason::NotParsed;

    /// Same owner as `ParseResult::original_bytes`; never copied or rewritten.
    std::shared_ptr<std::string const> original_bytes;

    std::size_t nodes = 0;             ///< element nodes classified
    std::size_t attributes = 0;        ///< attributes visited
    std::size_t references = 0;        ///< local #fragment occurrences charged
    /// Longest reference path over the whole graph (only edges flagged as
    /// references add one); independent of DOM/declaration/attribute order.
    std::size_t max_reference_depth = 0;
    /// Conservative whole-document expanded estimate: the DP count at the
    /// document root (`1 + sum` of target counts, duplicate edges counted).
    /// Distinct from `validation_work`; includes defs/resources. If the true
    /// sum overflows `size_t`, this is the saturated lower bound `SIZE_MAX` and
    /// the status is `Rejected`/`ExpandedWorkBudget` regardless of the limit.
    std::size_t expanded_work = 0;
    /// Unique construction/traversal work spent by the policy (resolved refs,
    /// built edges, frontier visits, DFS and DP steps), charged before each
    /// step and bounded by `max_validation_work`. Distinct from
    /// `expanded_work`.
    std::size_t validation_work = 0;

    bool candidate() const { return status == PreviewResourcePolicyStatus::Candidate; }
};

/// Audit the bounded, read-only DOM of an accepted parse. Never mutates the
/// parse, never loads a native document and never rewrites source bytes.
PreviewResourcePolicyResult admit_preview_resources(ParsedPreviewXml const &parsed,
                                                    PreviewResourcePolicyLimits const &limits = {},
                                                    Cancelled cancelled = {});

std::string_view to_string(PreviewResourcePolicyStatus status);
std::string_view to_string(PreviewResourcePolicyReason reason);

} // namespace Inkscape::IO

#endif // SEEN_INKSCAPE_IO_PREVIEW_SVG_RESOURCE_POLICY_H
