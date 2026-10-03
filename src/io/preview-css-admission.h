// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Bounded W5 CSS admission primitive.
 *
 * This is a CSS-only resource audit for a conservative first subset: ordinary
 * selectors, ordinary declarations, inline declaration lists and purely local
 * `#fragment` URL references. It reuses the native libcroco tokenizer, SAC
 * parser and declaration parser; it does not implement a second CSS grammar.
 *
 * `Accepted` means "no CSS-level resource escape was found in this bounded
 * input"; it is explicitly NOT a claim that a whole SVG document is safe and
 * NOT a claim of strict CSS validity. At-rules, unknown functions,
 * external/data/relative references and the malformed inputs the native paths
 * can observe are refused. It reuses libcroco exactly, so the native parser's
 * proven limitations remain and are reported as conservative refusals rather
 * than rewritten: a comment that abuts `}` without whitespace, a comment inside
 * `url()`, an astral CSS escape, a control character inside a string, and
 * unknown-but-valid functions such as `rgba()` are refused. Source bytes are
 * never normalized, rewritten or rendered.
 *
 * XML admission, helper entry and caching remain separate and unimplemented.
 */
#ifndef SEEN_INKSCAPE_IO_PREVIEW_CSS_ADMISSION_H
#define SEEN_INKSCAPE_IO_PREVIEW_CSS_ADMISSION_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Inkscape {
namespace IO {

enum class CssAdmissionStatus {
    Accepted,
    Unsupported,
    Rejected,
};

enum class CssAdmissionReason {
    Ok = 0,

    // Rejected before any native parse.
    NullInput,
    TooManyBytes,
    EmbeddedNul,
    InvalidUtf8,

    // Rejected by the native parse / recovery / budget audit.
    NotParsed,
    NativeParseError,
    CallbackError,
    UnbalancedCallbacks,
    PartialConsumption,
    TooManyTerms,
    TooDeep,
    TooManyReferences,
    ExternalReference,
    EmptyReference,
    NestedAmbiguousReference,

    // Refused but not malformed: deferred or unproven CSS features.
    AtRule,
    UnknownFunction,
};

struct CssLimits {
    std::size_t max_bytes = 1024u * 1024u; ///< raw input bytes
    std::size_t max_terms = 100000u;       ///< decoded CRTerm nodes visited
    std::size_t max_depth = 64u;           ///< term/function nesting
    std::size_t max_references = 4096u;    ///< resource reference occurrences
};

/// Owning summary of an admission decision. Contains no raw libcroco AST
/// pointers; lifetime is independent of any parser or declaration list.
struct CssAdmissionResult {
    CssAdmissionStatus status = CssAdmissionStatus::Rejected;
    CssAdmissionReason reason = CssAdmissionReason::NullInput;

    /// Local `#fragment` identifiers recovered from accepted input, e.g. "#g".
    std::vector<std::string> local_fragment_ids;

    std::size_t term_count = 0;
    std::size_t reference_count = 0;
    std::size_t max_depth = 0;

    bool accepted() const { return status == CssAdmissionStatus::Accepted; }
};

/// Admit an inline declaration list (`style="..."`), matching the native
/// `cr_declaration_parse_list_from_buf` call used by `src/xml/repr-css.cpp`.
CssAdmissionResult admit_inline_declarations(std::string_view bytes,
                                             CssLimits const &limits = {});

/// Admit a stylesheet (`<style>...</style>`), matching the native
/// `cr_parser_parse_buf` + SAC call used by `src/object/sp-style-elem.cpp`.
CssAdmissionResult admit_stylesheet(std::string_view bytes,
                                    CssLimits const &limits = {});

std::string_view to_string(CssAdmissionStatus status);
std::string_view to_string(CssAdmissionReason reason);

} // namespace IO
} // namespace Inkscape

#endif // SEEN_INKSCAPE_IO_PREVIEW_CSS_ADMISSION_H
