// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Bounded W5 CSS admission primitive. See preview-css-admission.h.
 */

#include "preview-css-admission.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <glib.h>

#include "3rdparty/libcroco/src/cr-declaration.h"
#include "3rdparty/libcroco/src/cr-doc-handler.h"
#include "3rdparty/libcroco/src/cr-parser.h"
#include "3rdparty/libcroco/src/cr-selector.h"
#include "3rdparty/libcroco/src/cr-string.h"
#include "3rdparty/libcroco/src/cr-term.h"
#include "3rdparty/libcroco/src/cr-tknzr.h"
#include "3rdparty/libcroco/src/cr-token.h"
#include "3rdparty/libcroco/src/cr-utils.h"

namespace Inkscape {
namespace IO {
namespace {

// ---------------------------------------------------------------------------
// RAII for native libcroco objects. Every early return still releases them.
// ---------------------------------------------------------------------------

struct ParserDeleter {
    void operator()(CRParser *p) const {
        if (p) {
            cr_parser_destroy(p);
        }
    }
};
struct HandlerDeleter {
    void operator()(CRDocHandler *h) const {
        if (h) {
            cr_doc_handler_unref(h);
        }
    }
};
struct DeclarationDeleter {
    void operator()(CRDeclaration *d) const {
        if (d) {
            cr_declaration_destroy(d);
        }
    }
};
struct TokenizerDeleter {
    void operator()(CRTknzr *t) const {
        if (t) {
            cr_tknzr_unref(t);
        }
    }
};
struct TokenDeleter {
    void operator()(CRToken *t) const {
        if (t) {
            cr_token_destroy(t);
        }
    }
};

// ---------------------------------------------------------------------------
// Small helpers. These are not a CSS grammar; they only classify already
// decoded native values and raw bytes.
// ---------------------------------------------------------------------------

std::string ascii_lower(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

/// Copy decoded CRString content. Uses GString length so an escape-decoded
/// embedded NUL cannot cause an over-read; raw NUL is rejected before parse.
std::string str_of(CRString const *s) {
    if (!s || !s->stryng || !s->stryng->str) {
        return {};
    }
    return std::string(s->stryng->str, static_cast<std::size_t>(s->stryng->len));
}

bool is_all_whitespace(std::string_view s) {
    for (unsigned char c : s) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') {
            return false;
        }
    }
    return true;
}

/// Bounded UTF-8 validation delegated to GLib, which is already a linked
/// dependency. GLib's validator rejects overlong forms, UTF-16 surrogates,
/// values above U+10FFFF and truncated sequences, matching the previous local
/// engine. Raw NUL is rejected by the caller before this point because
/// g_utf8_validate treats U+0000 as a valid code point. The byte length is known
/// and must fit gssize.
bool valid_utf8(std::string_view s) {
    if (s.empty()) {
        return true;
    }
    if (s.size() > static_cast<std::size_t>(G_MAXSSIZE)) {
        return false;
    }
    return g_utf8_validate(s.data(), static_cast<gssize>(s.size()), nullptr) != FALSE;
}

/// A single purely local `#fragment`. Anything that could address an external
/// scheme, a relative path, a data payload or an ambiguous further reference
/// is refused. False rejection is the conservative direction.
bool is_local_fragment(std::string const &s) {
    if (s.size() < 2 || s[0] != '#') {
        return false;
    }
    for (std::size_t i = 1; i < s.size(); ++i) {
        unsigned char const c = static_cast<unsigned char>(s[i]);
        if (c <= 0x20u || c == 0x7Fu) {
            return false;
        }
        switch (c) {
        case '"':
        case '\'':
        case '(':
        case ')':
        case '/':
        case '\\':
        case ':':
        case '#':
        case '?':
        case '<':
        case '>':
        case '%':
            return false;
        default:
            break;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Recursive native-term audit.
// ---------------------------------------------------------------------------

struct AuditContext {
    CssLimits limits;
    std::size_t term_count = 0;
    std::size_t reference_count = 0;
    std::size_t max_depth = 0;
    std::vector<std::string> refs;
    std::set<std::string> ref_set;

    bool latched = false;
    CssAdmissionStatus latched_status = CssAdmissionStatus::Rejected;
    CssAdmissionReason latched_reason = CssAdmissionReason::Ok;

    void latch(CssAdmissionStatus status, CssAdmissionReason reason) {
        if (!latched) {
            latched = true;
            latched_status = status;
            latched_reason = reason;
        }
    }
};

void record_reference(AuditContext &ctx, std::string const &fragment) {
    ++ctx.reference_count;
    if (ctx.reference_count > ctx.limits.max_references) {
        ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::TooManyReferences);
        return;
    }
    if (ctx.ref_set.insert(fragment).second) {
        ctx.refs.push_back(fragment);
    }
}

/// Native serialization of a decoded term chain, owned as a std::string and
/// released with g_free. Callers must only invoke this after the chain has
/// passed its term/depth budget, because the native serializer recurses over the
/// chain. Returns empty when the native serializer returns no buffer.
std::string serialize_terms(CRTerm const *term) {
    guchar *raw = cr_term_to_string(term);
    if (!raw) {
        return {};
    }
    // cr_term_to_string returns the buffer from g_string_free(str_buf, FALSE),
    // so g_free is the correct release. Take RAII ownership immediately, before
    // the std::string construction, so the native buffer is freed even if that
    // allocation throws (std::bad_alloc). Both the inline primary loop and
    // cb_property call this path.
    std::unique_ptr<guchar, decltype(&g_free)> owner(raw, &g_free);
    return std::string(reinterpret_cast<char const *>(owner.get()));
}

void audit_uri_string(std::string const &s, AuditContext &ctx) {
    if (s.empty() || s == "#") {
        ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::EmptyReference);
        return;
    }
    if (s[0] != '#' || !is_local_fragment(s)) {
        ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::ExternalReference);
        return;
    }
    record_reference(ctx, s);
}

/// Audit the single argument of an escaped/decoded `url(...)` function. Only a
/// single scalar fragment (`#fragment` hash, string or ident) is allowed. A
/// `TERM_URI` here is the nested `url(url(#x))` shape that reserializes to an
/// ambiguous form, and any function/extra term is refused.
void audit_url_argument(CRTerm const *arg, AuditContext &ctx) {
    switch (arg->type) {
    case TERM_HASH: {
        std::string s = str_of(arg->content.str);
        if (s.empty()) {
            ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::EmptyReference);
            return;
        }
        if (s[0] != '#') {
            s.insert(s.begin(), '#');
        }
        if (!is_local_fragment(s)) {
            ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::ExternalReference);
            return;
        }
        record_reference(ctx, s);
        break;
    }
    case TERM_STRING:
    case TERM_IDENT:
        audit_uri_string(str_of(arg->content.str), ctx);
        break;
    case TERM_URI:
        // Decoded `FUNCTION url` + native `TERM_URI` is `url(url(#x))`; the
        // native reserialization keeps both url wrappers, so treat it as nested.
        ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NestedAmbiguousReference);
        break;
    default:
        ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NestedAmbiguousReference);
        break;
    }
}

/// Visit every term in the native chain/function tree, counting before descent
/// so a term/nesting/reference budget cannot be skipped.
bool audit_terms(CRTerm const *term, AuditContext &ctx, std::size_t depth) {
    for (; term; term = term->next) {
        if (ctx.latched) {
            return false;
        }
        ++ctx.term_count;
        if (ctx.term_count > ctx.limits.max_terms) {
            ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::TooManyTerms);
            return false;
        }
        std::size_t const next_depth = depth + 1;
        if (next_depth > ctx.max_depth) {
            ctx.max_depth = next_depth;
        }
        if (next_depth > ctx.limits.max_depth) {
            ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::TooDeep);
            return false;
        }

        switch (term->type) {
        case TERM_URI:
            audit_uri_string(str_of(term->content.str), ctx);
            break;
        case TERM_FUNCTION: {
            std::string const name = ascii_lower(str_of(term->content.str));
            if (name == "url") {
                CRTerm const *arg = term->ext_content.func_param;
                if (!arg) {
                    ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::EmptyReference);
                    break;
                }
                if (arg->next) {
                    ctx.latch(CssAdmissionStatus::Rejected,
                              CssAdmissionReason::NestedAmbiguousReference);
                    break;
                }
                // Count the argument term before inspecting it.
                ++ctx.term_count;
                if (ctx.term_count > ctx.limits.max_terms) {
                    ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::TooManyTerms);
                    break;
                }
                std::size_t const arg_depth = next_depth + 1;
                if (arg_depth > ctx.max_depth) {
                    ctx.max_depth = arg_depth;
                }
                if (arg_depth > ctx.limits.max_depth) {
                    ctx.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::TooDeep);
                    break;
                }
                audit_url_argument(arg, ctx);
            } else {
                // No native harmless-function proof exists yet beyond `rgb()`.
                ctx.latch(CssAdmissionStatus::Unsupported, CssAdmissionReason::UnknownFunction);
            }
            break;
        }
        case TERM_NUMBER:
        case TERM_STRING:
        case TERM_IDENT:
        case TERM_RGB:
        case TERM_UNICODERANGE:
        case TERM_HASH:
        case TERM_NO_TYPE:
            // Plain non-resource values are retained.
            break;
        }
    }
    return !ctx.latched;
}

// ---------------------------------------------------------------------------
// SAC validation parse. Callbacks latch and never throw across the C frames.
// ---------------------------------------------------------------------------

struct PropertySummary {
    std::string name;
    bool important = false;
    /// Native normalized serialization of the value chain, captured only after
    /// the owning audit context accepted the chain. Used for cross-parse
    /// containment, never to rewrite accepted CSS.
    std::string value_norm;
};

struct SacContext {
    AuditContext *audit = nullptr;
    std::size_t start_document = 0;
    std::size_t end_document = 0;
    std::size_t start_selector = 0;
    std::size_t end_selector = 0;
    std::size_t start_font_face = 0;
    std::size_t end_font_face = 0;
    std::size_t properties = 0;
    std::size_t error = 0;
    std::size_t unrecoverable = 0;
    bool threw = false;
    std::vector<PropertySummary> props;
};

SacContext *ctx_of(CRDocHandler *h) {
    if (!h || !h->app_data) {
        return nullptr;
    }
    return static_cast<SacContext *>(h->app_data);
}

void latch_callback_error(SacContext *c) {
    if (c && c->audit) {
        c->audit->latch(CssAdmissionStatus::Rejected, CssAdmissionReason::CallbackError);
    }
}

void cb_start_document(CRDocHandler *h) {
    SacContext *c = ctx_of(h);
    if (!c) {
        return;
    }
    try {
        ++c->start_document;
    } catch (...) {
        latch_callback_error(c);
    }
}

void cb_end_document(CRDocHandler *h) {
    SacContext *c = ctx_of(h);
    if (!c) {
        return;
    }
    try {
        ++c->end_document;
    } catch (...) {
        latch_callback_error(c);
    }
}

void cb_start_selector(CRDocHandler *h, CRSelector *s) {
    SacContext *c = ctx_of(h);
    if (!c) {
        return;
    }
    try {
        ++c->start_selector;
        // libcroco's SAC layer never reports the empty selector of `{}` (the
        // parser consumes it through cr_parser_parse_ruleset_core without a
        // callback; see scan_for_empty_ruleset). This guard still refuses a
        // null/empty selector if any native path ever does report one.
        if (!s || !s->simple_sel) {
            if (c->audit) {
                c->audit->latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NativeParseError);
            }
        }
    } catch (...) {
        latch_callback_error(c);
    }
}

void cb_end_selector(CRDocHandler *h, CRSelector *s) {
    (void)s;
    SacContext *c = ctx_of(h);
    if (!c) {
        return;
    }
    try {
        ++c->end_selector;
    } catch (...) {
        latch_callback_error(c);
    }
}

void cb_start_font_face(CRDocHandler *h, CRParsingLocation *loc) {
    (void)loc;
    SacContext *c = ctx_of(h);
    if (!c) {
        return;
    }
    try {
        ++c->start_font_face;
    } catch (...) {
        latch_callback_error(c);
    }
}

void cb_end_font_face(CRDocHandler *h) {
    SacContext *c = ctx_of(h);
    if (!c) {
        return;
    }
    try {
        ++c->end_font_face;
    } catch (...) {
        latch_callback_error(c);
    }
}

void cb_property(CRDocHandler *h, CRString *name, CRTerm *value, gboolean important) {
    SacContext *c = ctx_of(h);
    if (!c) {
        return;
    }
    try {
        ++c->properties;
        PropertySummary summary;
        summary.name = str_of(name);
        summary.important = important != FALSE;
        c->props.push_back(std::move(summary));
        if (c->audit) {
            // Serialize only after this context's own term/depth budget has
            // accepted the chain, so the recursive serializer is bounded.
            if (audit_terms(value, *c->audit, 0)) {
                c->props.back().value_norm = serialize_terms(value);
            }
        }
    } catch (...) {
        c->threw = true;
        latch_callback_error(c);
    }
}

void cb_error(CRDocHandler *h) {
    SacContext *c = ctx_of(h);
    if (!c) {
        return;
    }
    try {
        ++c->error;
    } catch (...) {
        latch_callback_error(c);
    }
}

void cb_unrecoverable_error(CRDocHandler *h) {
    SacContext *c = ctx_of(h);
    if (!c) {
        return;
    }
    try {
        ++c->unrecoverable;
    } catch (...) {
        latch_callback_error(c);
    }
}

/// Run the native stylesheet parse used by sp-style-elem.cpp and collect the
/// balance/error counters. Returns false only if parser allocation failed.
bool parse_stylesheet(std::string const &owned, AuditContext &audit, SacContext &sac) {
    std::unique_ptr<CRParser, ParserDeleter> parser(cr_parser_new(nullptr));
    if (!parser) {
        audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NotParsed);
        return false;
    }
    std::unique_ptr<CRDocHandler, HandlerDeleter> handler(cr_doc_handler_new());
    if (!handler) {
        audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NotParsed);
        return false;
    }

    handler->app_data = &sac;
    handler->start_document = cb_start_document;
    handler->end_document = cb_end_document;
    handler->start_selector = cb_start_selector;
    handler->end_selector = cb_end_selector;
    handler->start_font_face = cb_start_font_face;
    handler->end_font_face = cb_end_font_face;
    handler->property = cb_property;
    handler->error = cb_error;
    handler->unrecoverable_error = cb_unrecoverable_error;

    if (cr_parser_set_sac_handler(parser.get(), handler.get()) != CR_OK) {
        audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NotParsed);
        return false;
    }

    enum CRStatus const status = cr_parser_parse_buf(
        parser.get(), reinterpret_cast<const guchar *>(owned.data()),
        static_cast<gulong>(owned.size()), CR_UTF_8);

    if (status != CR_OK) {
        audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NativeParseError);
    }
    if (sac.error > 0 || sac.unrecoverable > 0) {
        audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NativeParseError);
    }
    if (sac.threw) {
        audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::CallbackError);
    }
    // A clean document must open and close exactly once, and every selector and
    // font-face block must balance.
    if (sac.start_document != 1 || sac.end_document != 1) {
        audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::UnbalancedCallbacks);
    }
    if (sac.start_selector != sac.end_selector) {
        audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::UnbalancedCallbacks);
    }
    if (sac.start_font_face != sac.end_font_face) {
        audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::UnbalancedCallbacks);
    }
    return true;
}

/// Scan the raw token stream with the native CRTknzr for any at-rule token,
/// decoded escapes included. No ad-hoc '@' text scan.
bool scan_for_at_rules(std::string const &owned, AuditContext &audit) {
    if (owned.empty()) {
        return true;
    }
    // The tokenizer API is non-const but only reads; the source bytes are
    // owned by the caller and are never mutated.
    auto *mutable_bytes = reinterpret_cast<guchar *>(const_cast<char *>(owned.data()));
    std::unique_ptr<CRTknzr, TokenizerDeleter> tknzr(
        cr_tknzr_new_from_buf(mutable_bytes, static_cast<gulong>(owned.size()), CR_UTF_8, FALSE));
    if (!tknzr) {
        audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NotParsed);
        return false;
    }

    for (;;) {
        CRToken *raw_token = nullptr;
        enum CRStatus const status = cr_tknzr_get_next_token(tknzr.get(), &raw_token);
        std::unique_ptr<CRToken, TokenDeleter> token(raw_token);
        if (status == CR_END_OF_INPUT_ERROR) {
            return true;
        }
        if (status != CR_OK || !raw_token) {
            audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NativeParseError);
            return false;
        }
        switch (raw_token->type) {
        case IMPORT_SYM_TK:
        case PAGE_SYM_TK:
        case MEDIA_SYM_TK:
        case FONT_FACE_SYM_TK:
        case CHARSET_SYM_TK:
        case ATKEYWORD_TK:
            audit.latch(CssAdmissionStatus::Unsupported, CssAdmissionReason::AtRule);
            return false;
        default:
            break;
        }
    }
}

/// Detect a top-level ruleset boundary whose selector is empty, using the native
/// tokenizer's own token stream. libcroco's SAC parser silently consumes `{}`
/// through cr_parser_parse_statement_core -> cr_parser_parse_ruleset_core and
/// never calls start_selector (proved by test/selector-probe.c), so the empty
/// selector is invisible to callbacks. This mirrors the native top-level
/// ruleset-start token set from cr-parser.c and only refuses a `{` reached at
/// brace depth 0 with no such token since the previous boundary. It is a native
/// token-boundary check, not a second CSS grammar and not a raw-brace regex.
bool scan_for_empty_ruleset(std::string const &owned, AuditContext &audit) {
    if (owned.empty()) {
        return true;
    }
    auto *mutable_bytes = reinterpret_cast<guchar *>(const_cast<char *>(owned.data()));
    std::unique_ptr<CRTknzr, TokenizerDeleter> tknzr(
        cr_tknzr_new_from_buf(mutable_bytes, static_cast<gulong>(owned.size()), CR_UTF_8, FALSE));
    if (!tknzr) {
        audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NotParsed);
        return false;
    }

    std::size_t depth = 0; // unmatched '{' at or below the current ruleset
    bool selector_started = false;
    for (;;) {
        CRToken *raw_token = nullptr;
        enum CRStatus const status = cr_tknzr_get_next_token(tknzr.get(), &raw_token);
        std::unique_ptr<CRToken, TokenDeleter> token(raw_token);
        if (status == CR_END_OF_INPUT_ERROR) {
            return true;
        }
        if (status != CR_OK || !raw_token) {
            audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NativeParseError);
            return false;
        }
        if (depth > 0) {
            if (raw_token->type == CBO_TK) {
                ++depth;
            } else if (raw_token->type == CBC_TK) {
                --depth;
                if (depth == 0) {
                    selector_started = false;
                }
            }
            continue;
        }
        switch (raw_token->type) {
        case S_TK:
        case COMMENT_TK:
        case CDO_TK:
        case CDC_TK:
            break;
        case CBO_TK:
            if (!selector_started) {
                audit.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NativeParseError);
                return false;
            }
            depth = 1;
            selector_started = false;
            break;
        case CBC_TK:
            // A stray top-level '}' is left to the native parser's status.
            break;
        case HASH_TK:
        case BO_TK:
        case IDENT_TK:
            selector_started = true;
            break;
        case DELIM_TK:
            if (raw_token->u.unichar == '.' || raw_token->u.unichar == ':'
                || raw_token->u.unichar == '*') {
                selector_started = true;
            }
            break;
        default:
            break;
        }
    }
}

CssAdmissionResult finish(AuditContext &audit) {
    CssAdmissionResult result;
    result.term_count = audit.term_count;
    result.reference_count = audit.reference_count;
    result.max_depth = audit.max_depth;
    result.local_fragment_ids = audit.refs;
    if (audit.latched) {
        result.status = audit.latched_status;
        result.reason = audit.latched_reason;
    } else {
        result.status = CssAdmissionStatus::Accepted;
        result.reason = CssAdmissionReason::Ok;
    }
    return result;
}

/// Largest input byte count representable by every native entry point used
/// below. libcroco's parser/tokenizer APIs take their byte count as `gulong`
/// and GLib's `g_utf8_validate` takes it as `gssize`. `admit_inline_declarations`
/// additionally wraps the input in a synthetic `*{ ... }` SAC selector before
/// the native stylesheet parse, adding exactly three bytes (`*{` and `}`), so
/// the input bound is the smaller native limit minus that wrapper. Checked
/// subtraction keeps the bound non-negative on platforms whose `gulong`/`gssize`
/// are narrower than `size_t`. The default 1 MiB limit is far below this and is
/// unchanged.
constexpr std::size_t max_native_input_bytes() {
    constexpr std::uintmax_t kInlineSacWrapperBytes = 3;
    constexpr std::uintmax_t gulong_max = std::numeric_limits<gulong>::max();
    constexpr std::uintmax_t gssize_max =
        static_cast<std::uintmax_t>(std::numeric_limits<gssize>::max());
    constexpr std::uintmax_t native_max = gulong_max < gssize_max ? gulong_max : gssize_max;
    constexpr std::uintmax_t wrapped_max =
        native_max > kInlineSacWrapperBytes ? native_max - kInlineSacWrapperBytes : 0u;
    return static_cast<std::size_t>(wrapped_max);
}

/// Reject NUL, invalid UTF-8 and oversized input before any native parse.
/// Returns true when already rejected.
bool precheck(std::string_view bytes, CssLimits const &limits, CssAdmissionResult &result) {
    // Refuse input that the caller's own budget or the native length types
    // cannot represent before any copy or native read. The native bound folds
    // in the 3-byte inline SAC wrapper; a stylesheet is bounded conservatively
    // by the same limit. Oversize is the existing TooManyBytes outcome.
    if (bytes.size() > limits.max_bytes || bytes.size() > max_native_input_bytes()) {
        result.status = CssAdmissionStatus::Rejected;
        result.reason = CssAdmissionReason::TooManyBytes;
        return true;
    }
    if (bytes.find('\0') != std::string_view::npos) {
        result.status = CssAdmissionStatus::Rejected;
        result.reason = CssAdmissionReason::EmbeddedNul;
        return true;
    }
    if (!valid_utf8(bytes)) {
        result.status = CssAdmissionStatus::Rejected;
        result.reason = CssAdmissionReason::InvalidUtf8;
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

CssAdmissionResult admit_inline_declarations(std::string_view bytes, CssLimits const &limits) {
    CssAdmissionResult result;
    if (precheck(bytes, limits, result)) {
        return result;
    }
    if (bytes.empty() || is_all_whitespace(bytes)) {
        result.status = CssAdmissionStatus::Accepted;
        result.reason = CssAdmissionReason::Ok;
        return result;
    }

    // NUL-terminated copy keeps every native call bounded and lets the
    // declaration parser operate on the caller's exact bytes unmodified.
    std::string const owned(bytes);

    // Primary context: the recovered declaration list, audited exactly once.
    // Its counters and fragment ids are the counters and ids of the result.
    AuditContext primary;
    primary.limits = limits;

    if (!scan_for_at_rules(owned, primary)) {
        return finish(primary);
    }

    // Exactly the native call repr-css.cpp uses for inline style attributes.
    std::unique_ptr<CRDeclaration, DeclarationDeleter> decls(
        cr_declaration_parse_list_from_buf(reinterpret_cast<const guchar *>(owned.data()),
                                           CR_UTF_8));
    if (!decls) {
        primary.latch(CssAdmissionStatus::Rejected, CssAdmissionReason::NotParsed);
        return finish(primary);
    }

    std::vector<PropertySummary> recovered;
    for (CRDeclaration const *d = decls.get(); d; d = d->next) {
        PropertySummary summary;
        summary.name = str_of(d->property);
        summary.important = d->important != FALSE;
        // Serialize only after this context's term/depth budget accepted the
        // chain, so the recursive native serializer stays bounded.
        if (audit_terms(d->value, primary, 0)) {
            summary.value_norm = serialize_terms(d->value);
        }
        recovered.push_back(std::move(summary));
        if (primary.latched) {
            break;
        }
    }
    if (primary.latched) {
        return finish(primary);
    }

    // Secondary context: a native SAC validation of the declaration context.
    // Wrapping in a synthetic selector makes the native stylesheet parser prove
    // full consumption, since cr_declaration_parse_list_from_buf recovers
    // partial input without a status. The secondary context has its own copy of
    // the limits, so both parses stay independently bounded even if they
    // diverge.
    AuditContext secondary;
    secondary.limits = limits;
    SacContext sac;
    sac.audit = &secondary;
    std::string wrapped;
    wrapped.reserve(bytes.size() + 3);
    wrapped += "*{";
    wrapped.append(bytes);
    wrapped += '}';
    if (!parse_stylesheet(wrapped, secondary, sac) || secondary.latched) {
        // Primary counters, secondary rejection reason.
        result = finish(primary);
        result.status = secondary.latched_status;
        result.reason = secondary.latched_reason;
        return result;
    }

    // Containment: the two native recovered outputs must agree exactly in
    // order, property name, !important and normalized native value, else the
    // declaration list consumed only part of the input or the two native
    // recoveries disagree. Values are compared only as native serializations;
    // no modified CSS is synthesized.
    bool same = sac.properties == recovered.size();
    if (same) {
        for (std::size_t i = 0; i < recovered.size(); ++i) {
            if (sac.props[i].name != recovered[i].name
                || sac.props[i].important != recovered[i].important
                || sac.props[i].value_norm != recovered[i].value_norm) {
                same = false;
                break;
            }
        }
    }
    if (!same) {
        result = finish(primary);
        result.status = CssAdmissionStatus::Rejected;
        result.reason = CssAdmissionReason::PartialConsumption;
        return result;
    }
    return finish(primary);
}

CssAdmissionResult admit_stylesheet(std::string_view bytes, CssLimits const &limits) {
    CssAdmissionResult result;
    if (precheck(bytes, limits, result)) {
        return result;
    }
    if (bytes.empty()) {
        result.status = CssAdmissionStatus::Accepted;
        result.reason = CssAdmissionReason::Ok;
        return result;
    }

    std::string const owned(bytes);

    AuditContext audit;
    audit.limits = limits;

    if (!scan_for_at_rules(owned, audit)) {
        return finish(audit);
    }
    // Native SAC never reports the empty selector of `{}`; refuse the native
    // tokenizer's empty ruleset boundary before the SAC parse can accept it.
    if (!scan_for_empty_ruleset(owned, audit)) {
        return finish(audit);
    }

    SacContext sac;
    sac.audit = &audit;
    parse_stylesheet(owned, audit, sac);
    return finish(audit);
}

std::string_view to_string(CssAdmissionStatus status) {
    switch (status) {
    case CssAdmissionStatus::Accepted:
        return "Accepted";
    case CssAdmissionStatus::Unsupported:
        return "Unsupported";
    case CssAdmissionStatus::Rejected:
        return "Rejected";
    }
    return "Rejected";
}

std::string_view to_string(CssAdmissionReason reason) {
    switch (reason) {
    case CssAdmissionReason::Ok:
        return "Ok";
    case CssAdmissionReason::NullInput:
        return "NullInput";
    case CssAdmissionReason::TooManyBytes:
        return "TooManyBytes";
    case CssAdmissionReason::EmbeddedNul:
        return "EmbeddedNul";
    case CssAdmissionReason::InvalidUtf8:
        return "InvalidUtf8";
    case CssAdmissionReason::NotParsed:
        return "NotParsed";
    case CssAdmissionReason::NativeParseError:
        return "NativeParseError";
    case CssAdmissionReason::CallbackError:
        return "CallbackError";
    case CssAdmissionReason::UnbalancedCallbacks:
        return "UnbalancedCallbacks";
    case CssAdmissionReason::PartialConsumption:
        return "PartialConsumption";
    case CssAdmissionReason::TooManyTerms:
        return "TooManyTerms";
    case CssAdmissionReason::TooDeep:
        return "TooDeep";
    case CssAdmissionReason::TooManyReferences:
        return "TooManyReferences";
    case CssAdmissionReason::ExternalReference:
        return "ExternalReference";
    case CssAdmissionReason::EmptyReference:
        return "EmptyReference";
    case CssAdmissionReason::NestedAmbiguousReference:
        return "NestedAmbiguousReference";
    case CssAdmissionReason::AtRule:
        return "AtRule";
    case CssAdmissionReason::UnknownFunction:
        return "UnknownFunction";
    }
    return "Unknown";
}

} // namespace IO
} // namespace Inkscape
