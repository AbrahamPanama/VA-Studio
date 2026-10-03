// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Bounded, non-loading XML input parse. See io/preview-xml-input.h for the
 * contract. This translation unit deliberately contains one small parser:
 * no resource classification, no allowlist/selector/cascade engine, no graph,
 * no helper and no cache. It never calls SPDocument, repr-io, the filesystem,
 * the network or a serializer.
 */
#include "io/preview-xml-input.h"

#include <glib.h>
#include <libxml/SAX2.h>
#include <libxml/entities.h>
#include <libxml/parser.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <utility>

namespace Inkscape::IO {

namespace {
struct DocDeleter {
    void operator()(xmlDocPtr doc) const {
        if (doc) {
            xmlFreeDoc(doc);
        }
    }
};
using DocPtr = std::unique_ptr<xmlDoc, DocDeleter>;

struct CtxtDeleter {
    // The parser context owns a possibly partial myDoc on every path. Acquire
    // ownership here and null the field so xmlFreeParserCtxt cannot free it
    // twice; a successful frame detaches myDoc before this runs.
    void operator()(xmlParserCtxtPtr ctxt) const {
        if (!ctxt) {
            return;
        }
        if (ctxt->myDoc) {
            xmlFreeDoc(ctxt->myDoc);
            ctxt->myDoc = nullptr;
        }
        xmlFreeParserCtxt(ctxt);
    }
};
using CtxtPtr = std::unique_ptr<xmlParserCtxt, CtxtDeleter>;

using Reason = PreviewXmlReason;
} // namespace

// Immutable state. The document owner is hidden; only const accessors exist.
struct ParsedPreviewXml::State {
    std::shared_ptr<std::string const> bytes;
    PreviewXmlLimits limits;
    DocPtr doc;
    std::size_t node_count = 0;
    std::size_t max_depth = 0;
    std::size_t attribute_count = 0;
    std::size_t namespace_count = 0;
};

namespace {

struct PreviewXmlError {
    Reason reason;
    char const *message;
};

[[noreturn]] void fail(Reason reason, char const *message) {
    throw PreviewXmlError{reason, message};
}

void check_limits(PreviewXmlLimits const &l) {
    PreviewXmlLimits const hard;
    bool ok = l.max_bytes != 0 && l.max_bytes <= hard.max_bytes &&
              l.max_nodes != 0 && l.max_nodes <= hard.max_nodes &&
              l.max_depth != 0 && l.max_depth <= hard.max_depth &&
              l.max_attributes_per_node != 0 &&
              l.max_attributes_per_node <= hard.max_attributes_per_node &&
              l.max_namespaces_per_element != 0 &&
              l.max_namespaces_per_element <= hard.max_namespaces_per_element &&
              l.max_name_bytes != 0 && l.max_name_bytes <= hard.max_name_bytes;
    if (!ok) {
        fail(Reason::InvalidLimits, "limits must be finite and no greater than the hard ceilings");
    }
}

std::size_t part_bytes(xmlChar const *s) {
    return s ? std::strlen(reinterpret_cast<char const *>(s)) : 0u;
}

// The native repr qualified-name buffer is 256 bytes; any composed
// `prefix:local` at or beyond the budget is refused before DOM allocation.
std::size_t qualified_bytes(xmlChar const *prefix, xmlChar const *local) {
    std::size_t n = part_bytes(local);
    if (prefix && *prefix) {
        n += 1u + part_bytes(prefix);
    }
    return n;
}

// Owned audit, reachable from every C callback through ctxt->_private. It
// latches the first failure and stops the parser; no exception ever crosses a
// C frame.
struct Audit {
    PreviewXmlLimits limits;
    Cancelled cancelled;
    xmlParserCtxtPtr parser = nullptr;
    std::exception_ptr error;

    std::size_t node_count = 0;
    std::size_t max_depth = 0;
    std::size_t attribute_count = 0;
    std::size_t namespace_count = 0;
    std::size_t depth = 0;

    // Native SAX2 DOM builders captured before instrumentation.
    startElementNsSAX2Func native_start = nullptr;
    endElementNsSAX2Func native_end = nullptr;
    charactersSAXFunc native_chars = nullptr;
    ignorableWhitespaceSAXFunc native_ignorable = nullptr;
    cdataBlockSAXFunc native_cdata = nullptr;
    commentSAXFunc native_comment = nullptr;

    Audit(PreviewXmlLimits l, Cancelled c) : limits(l), cancelled(std::move(c)) {}

    void poll() const {
        if (cancelled && cancelled()) {
            fail(Reason::Cancelled, "XML parse cancelled");
        }
    }

    template <typename Call> void callback(Call call) noexcept {
        if (error) {
            return;
        }
        try {
            poll();
            call();
        } catch (...) {
            error = std::current_exception();
            if (parser) {
                xmlStopParser(parser);
            }
        }
    }

    static void reject(void *ctx, Reason reason, char const *message) noexcept {
        auto *ctxt = static_cast<xmlParserCtxtPtr>(ctx);
        auto *audit = ctxt ? static_cast<Audit *>(ctxt->_private) : nullptr;
        if (!audit) {
            return;
        }
        audit->callback([reason, message] { fail(reason, message); });
    }

    void check_name(xmlChar const *prefix, xmlChar const *local) const {
        if (qualified_bytes(prefix, local) >= limits.max_name_bytes) {
            fail(Reason::NameBytesBudget, "qualified-name byte budget");
        }
    }

    void check_part(xmlChar const *name) const {
        if (part_bytes(name) >= limits.max_name_bytes) {
            fail(Reason::NameBytesBudget, "name byte budget");
        }
    }

    void charge_node() {
        if (node_count >= limits.max_nodes) {
            fail(Reason::NodeBudget, "node budget");
        }
        ++node_count;
    }

    void start(xmlChar const *local, xmlChar const *prefix, int nb_namespaces,
               int nb_attributes, xmlChar const **namespaces, xmlChar const **attributes) {
        check_name(prefix, local);
        if (static_cast<std::size_t>(std::max(nb_namespaces, 0)) > limits.max_namespaces_per_element) {
            fail(Reason::NamespaceBudget, "namespace-declaration budget");
        }
        if (static_cast<std::size_t>(std::max(nb_attributes, 0)) > limits.max_attributes_per_node) {
            fail(Reason::AttributeBudget, "attribute budget");
        }
        for (int i = 0; namespaces && i < nb_namespaces; ++i) {
            check_part(namespaces[2 * i]); // namespaces[] = prefix, URI
        }
        for (int i = 0; attributes && i < nb_attributes; ++i) {
            // attributes[] = local, prefix, URI, value-end; 5 slots per attribute
            check_name(attributes[5 * i + 1], attributes[5 * i]);
        }
        if (depth >= limits.max_depth) {
            fail(Reason::DepthBudget, "depth budget");
        }
        ++depth;
        if (depth > max_depth) {
            max_depth = depth;
        }
        charge_node();
        namespace_count += static_cast<std::size_t>(std::max(nb_namespaces, 0));
        attribute_count += static_cast<std::size_t>(std::max(nb_attributes, 0));
    }

    void end() {
        if (depth) {
            --depth;
        }
    }
};

void chain_start(void *ctx, xmlChar const *local, xmlChar const *prefix, xmlChar const *uri,
                 int nb_namespaces, xmlChar const **namespaces, int nb_attributes,
                 int nb_defaulted, xmlChar const **attributes) {
    auto *ctxt = static_cast<xmlParserCtxtPtr>(ctx);
    auto *audit = ctxt ? static_cast<Audit *>(ctxt->_private) : nullptr;
    if (!audit) {
        return;
    }
    audit->callback([&] {
        audit->start(local, prefix, nb_namespaces, nb_attributes, namespaces, attributes);
        if (audit->native_start) {
            audit->native_start(ctx, local, prefix, uri, nb_namespaces, namespaces, nb_attributes,
                                nb_defaulted, attributes);
        }
    });
}

void chain_end(void *ctx, xmlChar const *local, xmlChar const *prefix, xmlChar const *uri) {
    auto *ctxt = static_cast<xmlParserCtxtPtr>(ctx);
    auto *audit = ctxt ? static_cast<Audit *>(ctxt->_private) : nullptr;
    if (!audit) {
        return;
    }
    audit->callback([&] {
        audit->end();
        if (audit->native_end) {
            audit->native_end(ctx, local, prefix, uri);
        }
    });
}

void chain_characters(void *ctx, xmlChar const *ch, int len) {
    auto *ctxt = static_cast<xmlParserCtxtPtr>(ctx);
    auto *audit = ctxt ? static_cast<Audit *>(ctxt->_private) : nullptr;
    if (!audit) {
        return;
    }
    audit->callback([&] {
        audit->charge_node();
        if (audit->native_chars) {
            audit->native_chars(ctx, ch, len);
        }
    });
}

void chain_ignorable(void *ctx, xmlChar const *ch, int len) {
    auto *ctxt = static_cast<xmlParserCtxtPtr>(ctx);
    auto *audit = ctxt ? static_cast<Audit *>(ctxt->_private) : nullptr;
    if (!audit) {
        return;
    }
    audit->callback([&] {
        audit->charge_node();
        if (audit->native_ignorable) {
            audit->native_ignorable(ctx, ch, len);
        }
    });
}

void chain_cdata(void *ctx, xmlChar const *ch, int len) {
    auto *ctxt = static_cast<xmlParserCtxtPtr>(ctx);
    auto *audit = ctxt ? static_cast<Audit *>(ctxt->_private) : nullptr;
    if (!audit) {
        return;
    }
    audit->callback([&] {
        audit->charge_node();
        if (audit->native_cdata) {
            audit->native_cdata(ctx, ch, len);
        }
    });
}

void chain_comment(void *ctx, xmlChar const *value) {
    auto *ctxt = static_cast<xmlParserCtxtPtr>(ctx);
    auto *audit = ctxt ? static_cast<Audit *>(ctxt->_private) : nullptr;
    if (!audit) {
        return;
    }
    audit->callback([&] {
        audit->charge_node();
        if (audit->native_comment) {
            audit->native_comment(ctx, value);
        }
    });
}

// Variadic error callbacks cannot be C++ lambdas on every supported compiler,
// so mirror the library preflight's static reporting struct.
struct Errors {
    static void report(void *ctx, char const *, ...) {
        Audit::reject(ctx, Reason::MalformedXml, "strict XML parser rejected the input");
    }
    static void ignore(void *, char const *, ...) {}
};

// Empty when the declared encoding is absent or UTF-8/UTF8; otherwise the
// non-UTF-8 declaration from native parse metadata. This check runs only after
// the native parser accepted the bytes: a declared encoding the UTF-8 push
// parser cannot switch to is refused first and surfaces as MalformedXml. Both
// outcomes are Rejected with no document; no ad hoc XML declaration grammar is
// added here.
std::string_view non_utf8_declared_encoding(xmlParserCtxtPtr ctxt) {
    if (!ctxt->encoding) {
        return {};
    }
    if (xmlStrcasecmp(ctxt->encoding, BAD_CAST "UTF-8") == 0 ||
        xmlStrcasecmp(ctxt->encoding, BAD_CAST "UTF8") == 0) {
        return {};
    }
    return std::string_view(reinterpret_cast<char const *>(ctxt->encoding));
}

} // namespace

ParsedPreviewXml::ParsedPreviewXml(std::shared_ptr<State const> state) : _state(std::move(state)) {}
ParsedPreviewXml::ParsedPreviewXml(ParsedPreviewXml &&) noexcept = default;
ParsedPreviewXml &ParsedPreviewXml::operator=(ParsedPreviewXml &&) noexcept = default;
ParsedPreviewXml::~ParsedPreviewXml() = default;

xmlDoc const *ParsedPreviewXml::document() const noexcept {
    return _state->doc.get();
}

xmlNode const *ParsedPreviewXml::root() const noexcept {
    return xmlDocGetRootElement(_state->doc.get());
}

std::shared_ptr<std::string const> const &ParsedPreviewXml::original_bytes() const noexcept {
    return _state->bytes;
}

PreviewXmlLimits ParsedPreviewXml::limits() const noexcept {
    return _state->limits;
}

std::size_t ParsedPreviewXml::node_count() const noexcept {
    return _state->node_count;
}

std::size_t ParsedPreviewXml::max_depth() const noexcept {
    return _state->max_depth;
}

std::size_t ParsedPreviewXml::attribute_count() const noexcept {
    return _state->attribute_count;
}

std::size_t ParsedPreviewXml::namespace_count() const noexcept {
    return _state->namespace_count;
}

ParseResult parse_preview_xml(std::span<unsigned char const> input, PreviewXmlLimits limits,
                              Cancelled cancelled) {
    ParseResult result;

    // Byte admission happens before any allocation or copy.
    if (input.data() == nullptr || input.empty()) {
        result.reason = Reason::EmptyInput;
        return result;
    }
    try {
        check_limits(limits);
    } catch (PreviewXmlError const &e) {
        result.reason = e.reason;
        return result;
    }
    if (input.size() > limits.max_bytes ||
        input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        result.reason = Reason::TooManyBytes;
        return result;
    }

    // The single owning snapshot. It is never normalized, rewritten or
    // stripped; a UTF-8 BOM is preserved as authored. This copy is outside the
    // main try below, so catch its allocation failure locally and return an
    // internal result with no snapshot instead of letting it escape.
    std::shared_ptr<std::string const> bytes;
    try {
        bytes = std::make_shared<std::string const>(
            reinterpret_cast<char const *>(input.data()), input.size());
    } catch (...) {
        result.status = PreviewXmlStatus::Rejected;
        result.reason = Reason::InternalFailure;
        return result;
    }
    result.original_bytes = bytes;

    if (bytes->find('\0') != std::string::npos) {
        result.reason = Reason::EmbeddedNul;
        return result;
    }
    if (!g_utf8_validate(reinterpret_cast<gchar const *>(bytes->data()),
                         static_cast<gssize>(bytes->size()), nullptr)) {
        result.reason = Reason::InvalidUtf8;
        return result;
    }

    try {
        Audit audit(limits, std::move(cancelled));
        audit.poll(); // cancellation before the parser exists

        xmlSAXHandler sax;
        std::memset(&sax, 0, sizeof(sax));
        xmlSAX2InitDefaultSAXHandler(&sax, 0);
        audit.native_start = sax.startElementNs;
        audit.native_end = sax.endElementNs;
        audit.native_chars = sax.characters;
        audit.native_ignorable = sax.ignorableWhitespace;
        audit.native_cdata = sax.cdataBlock;
        audit.native_comment = sax.comment;
        if (!audit.native_start || !audit.native_end || !audit.native_chars ||
            !audit.native_ignorable || !audit.native_cdata || !audit.native_comment) {
            fail(Reason::InternalFailure, "native SAX2 DOM builders are unavailable");
        }

        // Instrumented callbacks chain to the captured native DOM builders.
        sax.startElementNs = chain_start;
        sax.endElementNs = chain_end;
        sax.characters = chain_characters;
        sax.ignorableWhitespace = chain_ignorable;
        sax.cdataBlock = chain_cdata;
        sax.comment = chain_comment;

        // Deny DTD/entities/PI before any possible loader.
        sax.processingInstruction = [](void *ctx, xmlChar const *, xmlChar const *) {
            Audit::reject(ctx, Reason::ForbiddenProcessingInstruction,
                          "processing instructions are forbidden");
        };
        sax.internalSubset = [](void *ctx, xmlChar const *, xmlChar const *, xmlChar const *) {
            Audit::reject(ctx, Reason::ForbiddenDoctype, "DOCTYPE/internal subset is forbidden");
        };
        sax.externalSubset = [](void *ctx, xmlChar const *, xmlChar const *, xmlChar const *) {
            Audit::reject(ctx, Reason::ForbiddenDoctype, "external subset is forbidden");
        };
        sax.entityDecl = [](void *ctx, xmlChar const *, int, xmlChar const *, xmlChar const *,
                            xmlChar *) {
            Audit::reject(ctx, Reason::ForbiddenEntity, "entity declarations are forbidden");
        };
        sax.unparsedEntityDecl = [](void *ctx, xmlChar const *, xmlChar const *, xmlChar const *,
                                    xmlChar const *) {
            Audit::reject(ctx, Reason::ForbiddenEntity, "unparsed entity declarations are forbidden");
        };
        sax.notationDecl = [](void *ctx, xmlChar const *, xmlChar const *, xmlChar const *) {
            Audit::reject(ctx, Reason::ForbiddenDoctype, "notation declarations are forbidden");
        };
        sax.attributeDecl = [](void *ctx, xmlChar const *, xmlChar const *, int, int,
                               xmlChar const *, xmlEnumerationPtr) {
            Audit::reject(ctx, Reason::ForbiddenDoctype, "attribute declarations are forbidden");
        };
        sax.elementDecl = [](void *ctx, xmlChar const *, int, xmlElementContentPtr) {
            Audit::reject(ctx, Reason::ForbiddenDoctype, "element declarations are forbidden");
        };
        sax.resolveEntity = [](void *ctx, xmlChar const *, xmlChar const *) -> xmlParserInputPtr {
            Audit::reject(ctx, Reason::ForbiddenEntity, "external entity access is forbidden");
            return nullptr;
        };
        sax.getParameterEntity = [](void *ctx, xmlChar const *) -> xmlEntityPtr {
            Audit::reject(ctx, Reason::ForbiddenParameterEntity, "parameter entities are forbidden");
            return nullptr;
        };
        sax.reference = [](void *ctx, xmlChar const *) {
            Audit::reject(ctx, Reason::ForbiddenEntity, "entity references are forbidden");
        };
        sax.getEntity = [](void *ctx, xmlChar const *name) -> xmlEntityPtr {
            auto *ctxt = static_cast<xmlParserCtxtPtr>(ctx);
            auto *audit = ctxt ? static_cast<Audit *>(ctxt->_private) : nullptr;
            if (!audit) {
                return nullptr;
            }
            if (auto *entity = xmlGetPredefinedEntity(name)) {
                return entity;
            }
            audit->callback(
                [] { fail(Reason::ForbiddenEntity, "custom entity references are forbidden"); });
            return nullptr;
        };
        sax.warning = Errors::ignore;
        sax.error = Errors::report;
        sax.fatalError = Errors::report;

        xmlParserCtxtPtr raw =
            xmlCreatePushParserCtxt(&sax, nullptr, nullptr, 0, nullptr);
        if (!raw) {
            fail(Reason::ParserAllocation, "cannot allocate XML parser context");
        }
        raw->userData = raw; // native SAX2 builders cast the context back to xmlParserCtxtPtr
        raw->_private = &audit;
        CtxtPtr ctxt(raw);
        audit.parser = raw;
        // NONET only; no HUGE, RECOVER or XInclude. Entity expansion is not
        // enabled: the DTD/entity surface above is denied, and libxml2 still
        // preserves predefined entities and numeric references without it.
        if (xmlCtxtUseOptions(raw, XML_PARSE_NONET) != 0) {
            fail(Reason::InternalFailure, "cannot apply XML parser options");
        }
        raw->validate = 0;
        raw->loadsubset = 0;
        raw->replaceEntities = 0;

        for (std::size_t at = 0; at < bytes->size(); at += 4096) {
            audit.poll();
            int const n = static_cast<int>(std::min<std::size_t>(4096, bytes->size() - at));
            int const status = xmlParseChunk(raw, bytes->data() + at, n, 0);
            if (audit.error) {
                std::rethrow_exception(audit.error);
            }
            if (status != 0) {
                fail(Reason::MalformedXml, "XML chunk rejected");
            }
        }
        int const status = xmlParseChunk(raw, nullptr, 0, 1);
        if (audit.error) {
            std::rethrow_exception(audit.error);
        }
        if (status != 0 || !raw->wellFormed) {
            fail(Reason::MalformedXml, "incomplete or malformed XML");
        }
        // Declared encoding is read from native parse metadata, never a regex.
        if (!non_utf8_declared_encoding(raw).empty()) {
            fail(Reason::UnsupportedEncoding, "declared encoding is not UTF-8");
        }

        auto state = std::make_shared<ParsedPreviewXml::State>();
        state->bytes = bytes;
        state->limits = limits;
        state->doc = DocPtr(raw->myDoc);
        raw->myDoc = nullptr;
        state->node_count = audit.node_count;
        state->max_depth = audit.max_depth;
        state->attribute_count = audit.attribute_count;
        state->namespace_count = audit.namespace_count;

        result.parsed = std::shared_ptr<ParsedPreviewXml const>(new ParsedPreviewXml(std::move(state)));
        result.status = PreviewXmlStatus::Parsed;
        result.reason = Reason::Ok;
        result.node_count = audit.node_count;
        result.max_depth = audit.max_depth;
        result.attribute_count = audit.attribute_count;
        result.namespace_count = audit.namespace_count;
    } catch (PreviewXmlError const &e) {
        result.status =
            e.reason == Reason::Cancelled ? PreviewXmlStatus::Cancelled : PreviewXmlStatus::Rejected;
        result.reason = e.reason;
    } catch (std::exception const &) {
        result.status = PreviewXmlStatus::Rejected;
        result.reason = Reason::InternalFailure;
    } catch (...) {
        result.status = PreviewXmlStatus::Rejected;
        result.reason = Reason::InternalFailure;
    }
    return result;
}

std::string_view to_string(PreviewXmlStatus status) {
    switch (status) {
    case PreviewXmlStatus::Parsed:
        return "Parsed";
    case PreviewXmlStatus::Rejected:
        return "Rejected";
    case PreviewXmlStatus::Cancelled:
        return "Cancelled";
    }
    return "Rejected";
}

std::string_view to_string(PreviewXmlReason reason) {
    switch (reason) {
    case Reason::Ok:
        return "Ok";
    case Reason::EmptyInput:
        return "EmptyInput";
    case Reason::TooManyBytes:
        return "TooManyBytes";
    case Reason::InvalidLimits:
        return "InvalidLimits";
    case Reason::EmbeddedNul:
        return "EmbeddedNul";
    case Reason::InvalidUtf8:
        return "InvalidUtf8";
    case Reason::UnsupportedEncoding:
        return "UnsupportedEncoding";
    case Reason::MalformedXml:
        return "MalformedXml";
    case Reason::ForbiddenDoctype:
        return "ForbiddenDoctype";
    case Reason::ForbiddenEntity:
        return "ForbiddenEntity";
    case Reason::ForbiddenParameterEntity:
        return "ForbiddenParameterEntity";
    case Reason::ForbiddenProcessingInstruction:
        return "ForbiddenProcessingInstruction";
    case Reason::NodeBudget:
        return "NodeBudget";
    case Reason::DepthBudget:
        return "DepthBudget";
    case Reason::AttributeBudget:
        return "AttributeBudget";
    case Reason::NamespaceBudget:
        return "NamespaceBudget";
    case Reason::NameBytesBudget:
        return "NameBytesBudget";
    case Reason::Cancelled:
        return "Cancelled";
    case Reason::ParserAllocation:
        return "ParserAllocation";
    case Reason::InternalFailure:
        return "InternalFailure";
    }
    return "InternalFailure";
}

} // namespace Inkscape::IO
