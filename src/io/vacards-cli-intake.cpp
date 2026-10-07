// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/vacards-cli-intake.h"
#include "actions/vacards-cli-fault.h"
#include "actions/vacards-cli-transaction.h"
#include "io/preview-css-admission.h"
#include "util/bitmap-input-header.h"
#include "util/bitmap-memory-admission.h"
#include "io/stream/inkscapestream.h"
#include "xml/repr-save-output-stream.h"
#include "object/sp-root.h"
#include "object/sp-image.h"
#include "xml/repr.h"
#include "extension/system.h"
#include "extension/internal/pdfinput/pdf-input.h"
#include "page-manager.h"
#include "libnrtype/font-factory.h"
#include "style.h"
#include "util/units.h"
#include "3rdparty/libcroco/src/cr-tknzr.h"
#include "3rdparty/libcroco/src/cr-token.h"
#include <boost/json.hpp>
#include <libxml/parser.h>
#include <exception>
#include <zlib.h>
#include <glib/gstdio.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef IGNORE
#undef near
#endif

namespace Inkscape::VACardsCli {
namespace {
using namespace boost::json;
constexpr std::uint64_t byte_ceiling = 512ull << 20;
thread_local std::optional<std::uint64_t> available_memory_for_testing;
std::map<SPDocument const *, object> reports;
struct Refusal { IntakeError error; };
[[noreturn]] void refuse(std::string code, std::string message, std::string hint, object details = {})
{
    bool retryable=code=="resource-unavailable" || code=="publication-unavailable";
    throw Refusal{{std::move(code), std::move(message), std::move(hint), retryable, std::move(details)}};
}
void ceiling(std::string const &name, std::uint64_t found, std::uint64_t limit)
{
    if (found > limit)
        refuse("engine-limit", "Inspection exceeds the " + name + " ceiling.",
               "Reduce the input complexity or size before inspecting it.",
               {{"limit_name", name}, {"limit", limit}, {"found", found}});
}
// Owner measurement: a 49 MB sheet uses about 1.9 GB. 42x covers even GiB/decimal-MB
// (1.9 * 2^30 / 49,000,000 = 41.63); retain another 256 MiB for recovery/headroom.
constexpr std::uint64_t memory_factor = 42, memory_reserve = 256ull << 20;
struct AvailableMemory { std::uint64_t bytes = 0; bool measured = false; };
AvailableMemory available_memory()
{
    AvailableMemory result;
    if (available_memory_for_testing) return {*available_memory_for_testing, true};
    auto sample = Bitmap::sampleMemory(); // Shared native platform/pressure/footprint query.
    if (!sample.ok()) return result;
    result.bytes = std::min(sample.value.available, sample.value.physical > sample.value.baseline ?
        sample.value.physical - sample.value.baseline : 0);
    result.measured = true;
    // The shared query reports process/commit headroom, not system free physical RAM.
    // Bound it by reclaimable physical pages on macOS and ullAvailPhys on Windows.
#ifdef __APPLE__
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    auto host = mach_host_self();
    result.measured = host_page_size(host, &page) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    if (result.measured) result.bytes = std::min(result.bytes,
        (std::uint64_t(vm.free_count) + vm.inactive_count) * page);
#elif defined(_WIN32)
    MEMORYSTATUSEX status{}; status.dwLength = sizeof status;
    result.measured = GlobalMemoryStatusEx(&status);
    if (result.measured) result.bytes = std::min(result.bytes, std::uint64_t(status.ullAvailPhys));
#endif
    return result;
}
void admit_memory(std::uint64_t bytes, char const *phase)
{
    auto memory = available_memory();
    auto available = memory.bytes;
    auto measured = memory.measured;
    std::uint64_t required;
    if (!Bitmap::checkedMul(bytes, memory_factor, required) ||
        !Bitmap::checkedAdd(required, memory_reserve, required)) required = UINT64_MAX;
    if (!measured || required > available)
        // Existing bitmap CLI memory-budget refusal code; typed intake reason identifies RAM.
        refuse("engine-limit", "Insufficient available physical memory for document " + std::string(phase) + ".",
               "Free memory or use a smaller document on this machine.",
               {{"reason", "insufficient-memory"}, {"phase", phase}, {"estimated_bytes", bytes},
                {"factor", memory_factor}, {"reserve_bytes", memory_reserve},
                {"limit", available}, {"found", required}, {"measured", measured}});
}
std::uint64_t admitted_size(ResourceAccess const &access, std::uint64_t limit, char const *phase)
{
    if (access.state != "granted") return 0; // read_admitted supplies the existing path/grant error.
    std::error_code ec;
    auto size = std::filesystem::file_size(std::filesystem::u8path(access.path), ec);
    if (ec) refuse("resource-unavailable", "Cannot measure admitted source.", "Retry with a stable local file.");
    ceiling("input-bytes", size, limit); // Byte refusal wins over memory refusal.
    admit_memory(size, phase);
    return size;
}
// Count a read-only repr serialization before source.copy(). This does not call serializer-plan
// preparation (which can clean/sort live XML). Qualified prefixes and 1 KiB overhead make the
// estimate conservative for generated namespace declarations and the XML prolog.
std::uint64_t snapshot_estimate(SPDocument const &source)
{
    class Counter final : public IO::OutputStream {
    public:
        std::uint64_t bytes = 1024;
        void close() override {}
        void flush() override {}
        int put(char) override { ++bytes; return 1; }
    } counter;
    IO::OutputStreamWriter writer(counter);
    auto options = sp_repr_capture_serializer_options();
    for (auto node = sp_repr_document_first_child(source.getReprDoc()); node; node = node->next())
        sp_repr_write_stream(node, writer, 0, true, Glib::QueryQuark(GQuark(0)), options.inlineattrs, options.indent);
    writer.close();
    return counter.bytes;
}
std::string read_local(ResourceAccess const &access, std::uint64_t limit)
{
    auto size = admitted_size(access, limit, "linked-load");
    auto read=read_admitted(access,std::min(size,limit));
    if (read.error=="engine-limit" && read.found<=limit)
        refuse("stale-dependency", "Source grew after memory admission.", "Retry with a stable source.");
    if(read.error=="engine-limit") ceiling("input-bytes",read.found,limit);
    if(!read.error.empty()) refuse(read.error,"Admitted input could not be read.","Retry with a stable granted local file.");
    return std::move(read.bytes);
}

struct CssFamily { std::string name; bool quoted=false; };
std::vector<CssFamily> font_families(std::string const &input)
{
    std::vector<CssFamily> result; CssFamily family; char quote=0;
    auto finish=[&] {
        auto begin=family.name.find_first_not_of(" \t\r\n"),end=family.name.find_last_not_of(" \t\r\n");
        if(begin!=std::string::npos) {family.name=family.name.substr(begin,end-begin+1);result.push_back(family);}
        family={};
    };
    for(std::size_t i=0;i<input.size();++i) {
        char c=input[i];
        if(c=='\\' && i+1<input.size()) {
            gunichar code=0;unsigned digits=0;
            while(i+1<input.size() && digits<6 && g_ascii_isxdigit(input[i+1])) {
                code=code*16+g_ascii_xdigit_value(input[++i]);++digits;
            }
            if(digits) {char utf8[6];if(!g_unichar_validate(code) || !code) code=0xfffd;
                family.name.append(utf8,g_unichar_to_utf8(code,utf8));
                if(i+1<input.size() && g_ascii_isspace(input[i+1])) ++i;
            } else family.name+=input[++i];
            continue;
        }
        if(quote) {if(c==quote) quote=0;else family.name+=c;continue;}
        if(c=='\'' || c=='"') {quote=c;family.quoted=true;continue;}
        if(c==',') finish();else family.name+=c;
    }
    finish();return result;
}
bool generic_family(CssFamily const &family)
{
    if(family.quoted) return false;
    auto raw=g_ascii_strdown(family.name.c_str(),-1);std::string name(raw);g_free(raw);
    return name=="serif" || name=="sans-serif" || name=="monospace" || name=="cursive" ||
           name=="fantasy" || name=="system-ui" || name=="ui-serif" || name=="ui-sans-serif" ||
           name=="ui-monospace" || name=="ui-rounded" || name=="math" || name=="emoji" || name=="fangsong";
}
std::string available_family(CssFamily const &family)
{
    if(generic_family(family)) return getSubstituteFontName(family.name);
    auto wanted=g_utf8_casefold(family.name.c_str(),-1);
    std::string result;
    for(auto const &installed:FontFactory::get().GetAllFontNames()) {
        auto folded=g_utf8_casefold(installed.c_str(),-1);
        bool same=std::strcmp(wanted,folded)==0;g_free(folded);
        if(same) {result=installed;break;}
    }
    g_free(wanted);return result;
}
std::string gunzip(std::string const &input, std::uint64_t limit)
{
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK)
        refuse("invalid-svgz", "Cannot initialize SVGZ decompression.", "Use an uncompressed SVG.");
    struct End { z_stream &s; ~End() { inflateEnd(&s); } } end{stream};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    stream.avail_in = input.size();
    std::string output;
    char buf[32768];
    int status;
    do {
        stream.next_out = reinterpret_cast<Bytef *>(buf);
        stream.avail_out = sizeof(buf);
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END)
            refuse("invalid-svgz", "SVGZ is truncated or malformed.", "Provide a complete gzip-compressed SVG.");
        auto n = sizeof(buf) - stream.avail_out;
        ceiling("expanded-input-bytes", output.size() + n, limit);
        if (output.size() / Bitmap::MiB != (output.size() + n) / Bitmap::MiB)
            admit_memory(output.size() + n, "expanded-load");
        output.append(buf, n);
    } while (status != Z_STREAM_END);
    if (stream.avail_in)
        refuse("invalid-svgz", "SVGZ has trailing or concatenated input.", "Provide one gzip member containing SVG.");
    return output;
}
std::string text(xmlChar const *s) { return s ? reinterpret_cast<char const *>(s) : ""; }
std::string attr(xmlNode *node, char const *key)
{
    auto s = xmlGetProp(node, BAD_CAST key);
    auto result = text(s); xmlFree(s); return result;
}
std::string contents(xmlNode *node)
{
    auto s = xmlNodeGetContent(node); auto result = text(s); xmlFree(s); return result;
}
// SAX preflight precedes both the libxml DOM and SPDocument. Feed the admitted buffer once:
// xmlTextReaderRead's tiny push chunks repeatedly scan a huge unfinished base64 attribute.
// No default SAX tree builder, DTD/entity loader or network resolver is installed.
void preflight(std::string const &bytes, IntakeLimits const &limits)
{
    struct Audit {
        IntakeLimits const &limits;
        xmlParserCtxtPtr parser = nullptr;
        std::uint64_t objects = 0;
        unsigned depth = 0;
        std::exception_ptr failure;
        void stop() noexcept { failure = std::current_exception(); xmlStopParser(parser); }
    } audit{limits};
    xmlSAXHandler sax{};
    sax.initialized = XML_SAX2_MAGIC;
    sax.startElementNs = [](void *context, xmlChar const *local, xmlChar const *prefix, xmlChar const *uri,
                            int namespaces, xmlChar const **ns, int attributes, int, xmlChar const **attrs) {
        auto &a = *static_cast<Audit *>(context);
        try {
            ceiling("xml-depth", ++a.depth, a.limits.max_xml_depth);
            ceiling("objects", ++a.objects, a.limits.max_objects);
            if (a.objects == 1 && (text(local) != "svg" || text(uri) != "http://www.w3.org/2000/svg"))
                refuse("unsupported-format", "The input is not an SVG document.", "Format import arrives in M2; use SVG or SVGZ for M1 inspection.");
            auto name = [](xmlChar const *local, xmlChar const *prefix) {
                return xmlStrlen(local) + (prefix ? xmlStrlen(prefix) + 1 : 0);
            };
            ceiling("qualified-name-bytes", name(local, prefix), 255);
            for (int i = 0; i < attributes; ++i)
                ceiling("qualified-name-bytes", name(attrs[5*i], attrs[5*i+1]), 255);
            for (int i = 0; i < namespaces; ++i)
                ceiling("qualified-name-bytes", ns[2*i] ? xmlStrlen(ns[2*i]) + 6 : 5, 255);
        } catch (...) { a.stop(); } // Never unwind through libxml's C frames.
    };
    sax.endElementNs = [](void *context, xmlChar const *, xmlChar const *, xmlChar const *) {
        --static_cast<Audit *>(context)->depth;
    };
    sax.internalSubset = [](void *context, xmlChar const *, xmlChar const *, xmlChar const *) {
        auto &a = *static_cast<Audit *>(context);
        try { refuse("unsafe-xml", "DTDs and custom entities are disabled.", "Remove the DTD/entity declaration."); }
        catch (...) { a.stop(); }
    };
    sax.processingInstruction = [](void *context, xmlChar const *, xmlChar const *) {
        auto &a = *static_cast<Audit *>(context);
        try { refuse("unsafe-xml", "Processing instructions are disabled.", "Remove the processing instruction."); }
        catch (...) { a.stop(); }
    };
    sax.reference = [](void *context, xmlChar const *) {
        auto &a = *static_cast<Audit *>(context);
        try { refuse("unsafe-xml", "Custom entities are disabled.", "Use ordinary SVG content."); }
        catch (...) { a.stop(); }
    };
    auto parser = cli_fault("intake.xml.preflight", CliFaultKind::Analysis) ? nullptr :
        xmlCreatePushParserCtxt(&sax, &audit, nullptr, 0, nullptr);
    if (!parser) refuse("invalid-xml", "Cannot parse XML.", "Provide well-formed UTF-8 SVG.");
    std::unique_ptr<xmlParserCtxt, decltype(&xmlFreeParserCtxt)> owner(parser, xmlFreeParserCtxt);
    audit.parser = parser;
    xmlCtxtUseOptions(parser, XML_PARSE_NONET | XML_PARSE_HUGE | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    parser->replaceEntities = 0; parser->loadsubset = 0; parser->validate = 0;
    auto status = xmlParseChunk(parser, bytes.data(), bytes.size(), 1);
    if (audit.failure) std::rethrow_exception(audit.failure);
    if (status || !parser->wellFormed || !parser->nsWellFormed || !audit.objects)
        refuse("invalid-xml", "The input XML is malformed.", "Provide well-formed UTF-8 SVG without external entities.");
}
std::vector<xmlNode *> elements(xmlNode *root)
{
    std::vector<xmlNode *> out;
    std::function<void(xmlNode *)> walk = [&](xmlNode *n) {
        for (; n; n = n->next) {
            if (n->type == XML_ELEMENT_NODE) out.push_back(n);
            walk(n->children);
        }
    };
    walk(root); return out;
}
// Audit local clone expansion before the native object builder can follow any clone references.
void references(std::vector<xmlNode *> const &nodes, IntakeLimits const &limits)
{
    std::map<std::string, xmlNode *> ids;
    for (auto n : nodes) {
        auto id = attr(n, "id");
        if (!id.empty() && !ids.emplace(id, n).second)
            refuse("duplicate-id", "SVG has duplicate object IDs.", "Make source IDs unique before inspection.");
    }
    std::map<xmlNode *, unsigned> state;
    std::map<xmlNode *, std::uint64_t> expanded;
    std::map<xmlNode *, unsigned> height;
    std::function<std::uint64_t(xmlNode *, unsigned)> visit = [&](xmlNode *n, unsigned depth) {
        ceiling("reference-depth", depth, limits.max_xml_depth);
        if (state[n] == 1) refuse("unsafe-reference", "SVG has a cyclic clone reference.", "Remove cyclic use references.");
        if (state[n] == 2) return expanded[n];
        state[n] = 1;
        std::uint64_t count = 1;
        height[n] = 1;
        for (auto c = n->children; c; c = c->next) if (c->type == XML_ELEMENT_NODE) {
            count += visit(c, depth + 1); ceiling("expanded-objects", count, limits.max_objects);
            height[n] = std::max(height[n], height[c] + 1);
            ceiling("reference-depth", height[n], limits.max_xml_depth);
        }
        if (text(n->name) == "use") {
            auto href = attr(n, "href");
            if (href.starts_with('#')) {
                auto it = ids.find(href.substr(1));
                if (it != ids.end()) {
                    count += visit(it->second, depth + 1);
                    height[n] = std::max(height[n], height[it->second] + 1);
                    ceiling("reference-depth", height[n], limits.max_xml_depth);
                }
                ceiling("expanded-objects", count, limits.max_objects);
            }
        }
        state[n] = 2; return expanded[n] = count;
    };
    if (!nodes.empty()) visit(nodes.front(), 1);
}
std::string raster_mime(std::string const &bytes)
{
    Bitmap::HeaderLimits limits;
    limits.maxPixels = 32000000;
    limits.maxEncodedBytes = byte_ceiling;
    Bitmap::Budget budget(Bitmap::Budget::FixedLimitForTest{}, 2 * Bitmap::MiB);
    auto header = Bitmap::inspect({reinterpret_cast<std::uint8_t const *>(bytes.data()), bytes.size(), {}}, limits, budget);
    ceiling("decoded-raster-pixels", std::uint64_t(header.value.width) * header.value.height, limits.maxPixels);
    // The shared header service parses TIFF IFD0 but marks TIFF unsupported
    // specifically for Explode's decoder. Editable intake uses the application's
    // existing GdkPixbuf TIFF decoder; retain every parsed header limit here.
    auto const &h=header.value;
    bool native_tiff=h.format==Bitmap::Format::TIFF && h.width && h.height &&
        h.width<=limits.maxAxis && h.height<=limits.maxAxis &&
        std::uint64_t(h.width)*h.height<=limits.maxPixels && h.profileBytes<=limits.maxProfileBytes &&
        !h.animated && h.frames==1 && bytes.size()<=limits.maxEncodedBytes;
    if (!header.ok() && !native_tiff) return {};
    // Sniffing and bounded structural inspection reuse the existing header machinery; no decoder.
    switch (header.value.format) {
        case Bitmap::Format::PNG: return "image/png";
        case Bitmap::Format::JPEG: return "image/jpeg";
        case Bitmap::Format::WebP: return "image/webp";
        case Bitmap::Format::GIF: return "image/gif";
        case Bitmap::Format::TIFF: return "image/tiff";
        default: return {};
    }
}
// Native tokenizer recovers decoded URLs, including CSS escapes and quoted @import strings.
std::vector<std::string> css_links(std::string const &css)
{
    ceiling("css-bytes", css.size(), 1048576);
    std::vector<std::string> links;
    auto t = cr_tknzr_new_from_buf(reinterpret_cast<guchar *>(const_cast<char *>(css.data())), css.size(), CR_UTF_8, FALSE);
    if (!t) return links;
    struct End { CRTknzr *t; ~End() { cr_tknzr_unref(t); } } end{t};
    auto record = [&](CRString const *str) {
        if (!str || !str->stryng) return;
        std::string link(str->stryng->str, str->stryng->len);
        if (!link.empty() && !link.starts_with('#')) links.push_back(std::move(link));
        ceiling("css-references", links.size(), 4096);
    };
    bool import = false;
    CRToken *token = nullptr;
    unsigned count = 0;
    while (cr_tknzr_get_next_token(t, &token) == CR_OK && token) {
        std::unique_ptr<CRToken, decltype(&cr_token_destroy)> owned(token, cr_token_destroy);
        ceiling("css-tokens", ++count, 100000);
        if (token->type == IMPORT_SYM_TK) import = true;
        else if (token->type == URI_TK || (import && token->type == STRING_TK)) {
            record(token->u.str);
            import = false;
        } else if (token->type == FUNCTION_TK && token->u.str && token->u.str->stryng &&
                   g_ascii_strcasecmp(token->u.str->stryng->str, "url") == 0) {
            // The native lexer emits escaped url names as FUNCTION rather than URI. Normalize
            // just that decoded name for a second native URI lex, retaining the exact argument.
            CRInputPos pos{};
            cr_tknzr_get_cur_pos(t, &pos);
            if (pos.next_byte_index >= 0 && static_cast<std::size_t>(pos.next_byte_index) <= css.size()) {
                auto canonical = "url(" + css.substr(pos.next_byte_index);
                auto nested = cr_tknzr_new_from_buf(reinterpret_cast<guchar *>(canonical.data()), canonical.size(), CR_UTF_8, FALSE);
                if (nested) {
                    End nested_end{nested}; CRToken *uri = nullptr;
                    auto status = cr_tknzr_get_next_token(nested, &uri);
                    std::unique_ptr<CRToken, decltype(&cr_token_destroy)> uri_owned(uri, cr_token_destroy);
                    if (status == CR_OK && uri && uri->type == URI_TK) record(uri->u.str);
                }
            }
            import = false;
        } else if (token->type != S_TK && token->type != COMMENT_TK) import = false;
        token = nullptr;
    }
    if (token) cr_token_destroy(token);
    return links;
}
void observe(IntakeObserver const &observer, std::string phase, std::string path,
             std::string access, bool admitted, std::string identity = {})
{
    if (observer) observer({std::move(phase), std::move(path), std::move(identity),
                            std::move(access), admitted});
}
void normalize(xmlDoc *dom, std::string const &base, Grants const &grants,
               IntakeLimits const &limits, object &report, FileLoadOptions const *editable = nullptr, IntakeObserver const &observer = {})
{
    auto nodes = elements(xmlDocGetRootElement(dom));
    references(nodes, limits);
    std::set<std::string> used;
    for (auto n : nodes) { auto id = attr(n, "id"); if (!id.empty()) used.insert(id); }
    array id_map, resources, changes;
    std::uint64_t next_id = 0, retained_bytes = 0;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        auto n = nodes[index];
        if (attr(n, "id").empty()) {
            std::string id;
            do { id = "inspect" + std::to_string(++next_id); } while (used.contains(id));
            used.insert(id); xmlSetProp(n, BAD_CAST "id", BAD_CAST id.c_str());
            id_map.emplace_back(object{{"id", id}, {"source_index", index}, {"original_id", nullptr}});
        }
        std::string id = attr(n, "id"), kind = text(n->name);
        auto ns = n->ns ? text(n->ns->href) : "";
        bool document_child = n->parent == xmlDocGetRootElement(dom);
        if (document_child && kind == "title" && ns == "http://www.w3.org/2000/svg") report["title"] = contents(n);
        if (document_child && kind == "metadata" && ns == "http://www.w3.org/2000/svg") {
            xmlBufferPtr b = xmlBufferCreate();
            xmlNodeDump(b, dom, n, 0, 0);
            report["metadata"] = text(xmlBufferContent(b)); xmlBufferFree(b);
        }
        auto record = [&](std::string const &href, std::string const &role) -> ResourceAccess {
            auto access = inspect_resource(href, base, grants);
            observe(observer, role, access.path, access.state, access.state == "granted");
            if (editable) {
                if (editable->resource_policy=="reject-external")
                    refuse("resource-denied","External references are refused by policy.","Embed the resource before loading.");
                if (access.state!="granted")
                    refuse(access.state=="unavailable" ? "resource-unavailable" : access.state=="remote" ? "remote-resource" : "resource-denied","External resource is not granted and readable.","Grant the linked local file or embed it.");
                Grants locality; locality.write_files={access.path};
                auto local_access=inspect_write_destination(access.path,locality);
                if(local_access.state=="unavailable")
                    refuse("resource-unavailable","Linked resource is temporarily unavailable.","Retry when access recovers.");
                if (local_access.state!="granted")
                    refuse("remote-resource","External resource must be local and symlink-free.","Copy the resource into a stable local directory.");
            }
            auto report_state = access.state;
#ifdef _WIN32
            if (!editable) {
                if (report_state == "unsafe") report_state = "ungranted";
                if (report_state == "invalid" || report_state == "unavailable") report_state = "missing";
            }
#endif
            resources.emplace_back(object{{"id", id}, {"role", role}, {"href", href},
                {"state", report_state}, {"loaded", false}});
            return access;
        };
        // Disable scripts and executable extensions; preserve the source identity in the report.
        if (kind == "script" || ns == "http://www.w3.org/2001/XInclude" ||
            ns == "http://www.inkscape.org/namespaces/inkscape/extension") {
            changes.emplace_back(object{{"id", id}, {"reason", "inactive-content"}});
            xmlNodeSetName(n, BAD_CAST "inspection-inactive");
        }
        for (auto a = n->properties; a;) {
            auto next = a->next;
            std::string key = text(a->name), ans = a->ns ? text(a->ns->href) : "";
            auto raw = xmlNodeListGetString(dom, a->children, 1);
            std::string value = text(raw); xmlFree(raw);
            bool remove = false;
            if (key.starts_with("on") || key == "absref" || key == "base" || key == "color-profile") remove = true;
            if (key == "absref" && !value.empty()) record(value, "image-fallback");
            if (key == "href" || key == "src") {
                if (value.starts_with('#') && kind != "image" && kind != "feImage") {
                    // Only use/paint/filter references can stay local; no active link fetching.
                } else if (kind == "image" && value.starts_with("data:")) {
                    auto comma = value.find(',');
                    std::string bytes;
                    if (comma != std::string::npos && value.substr(0, comma).ends_with(";base64")) {
                        gsize length = 0;
                        auto data = g_base64_decode(value.c_str() + comma + 1, &length);
                        bytes.assign(reinterpret_cast<char *>(data), length); g_free(data);
                    }
                    auto mime = raster_mime(bytes);
                    remove = mime.empty();
                    resources.emplace_back(object{{"id", id}, {"role", "image"}, {"href", value},
                        {"state", "embedded"}, {"mime", mime}, {"loaded", !remove}});
                } else {
                    auto access = record(value, kind == "image" ? "image" : kind);
                    remove = true;
                    if (kind == "image" && access.state == "granted") {
                        std::string bytes;
                        try {
                            if(editable && editable->before_linked_read_for_testing) editable->before_linked_read_for_testing(access);
                            observe(observer, "linked", access.path, "read-begin", true);
                            bytes = read_local(access, limits.max_input_bytes);
                            observe(observer, "linked", access.path, "read-end", true);
                        } catch (Refusal const &e) {
                            if (e.error.code == "engine-limit" || (editable && (e.error.code=="resource-unavailable" || e.error.code=="stale-dependency"))) throw;
                            resources.back().as_object()["state"] = "missing";
                            resources.back().as_object()["reason"] = e.error.code;
                        }
                        ceiling("linked-input-bytes", retained_bytes + bytes.size(), limits.max_input_bytes);
                        retained_bytes += bytes.size();
                        auto mime = raster_mime(bytes);
                        if (!mime.empty()) {
                            auto encoded = g_base64_encode(reinterpret_cast<guchar const *>(bytes.data()), bytes.size());
                            auto embedded = "data:" + mime + ";base64," + encoded; g_free(encoded);
                            xmlSetNsProp(n, a->ns, a->name, BAD_CAST embedded.c_str());
                            remove = false;
                            auto &r = resources.back().as_object(); r["loaded"] = true; r["mime"] = mime;
                        }
                    }
                }
            } else if (key == "style" || (ans.empty() && (key == "fill" || key == "stroke" || key == "filter" || key == "clip-path" || key == "mask" || key.starts_with("marker") || key == "cursor" || key == "shape-inside" || key == "shape-subtract"))) {
                auto css = key == "style" ? value : key + ":" + value;
                for (auto const &link : css_links(css)) record(link, "css");
                auto admitted = IO::admit_inline_declarations(css);
                remove = !admitted.accepted();
            }
            if (remove) {
                changes.emplace_back(object{{"id", id}, {"attribute", key}, {"reason", "resource-or-active-content-disabled"}});
                xmlRemoveProp(a);
            }
            a = next;
        }
        if (kind == "style") {
            auto css = contents(n);
            for (auto const &link : css_links(css)) record(link, "css");
            if (!IO::admit_stylesheet(css).accepted()) {
                xmlNodeSetContent(n, BAD_CAST "");
                changes.emplace_back(object{{"id", id}, {"reason", "stylesheet-disabled"}});
            }
        }
    }
    report["id_map"] = std::move(id_map);
    report["resources"] = std::move(resources);
    report["normalization"] = std::move(changes);
}
} // namespace
static IntakeResult load_document(std::string const &path, Grants const &grants, IntakeLimits const &requested, FileLoadOptions const *editable, IntakeObserver const &observer = {})
{
    IntakeResult result;
    try {
        // Refuse network/device namespaces before any filesystem access, on every host.
        if (path.starts_with("//") || path.starts_with("\\\\") || path.starts_with("\\??\\"))
            refuse("invalid-path", "Inspection refuses network and device-namespace paths.",
                   "Use an absolute local SVG/SVGZ filename outside network and device namespaces.");
        if (!std::filesystem::path(path).is_absolute())
            refuse("invalid-path", "Inspection requires an absolute local path.", "Pass an absolute local SVG/SVGZ filename, not a relative path or URL.");
        IntakeLimits limits{std::min(requested.max_input_bytes, byte_ceiling),
                            std::min<std::uint64_t>(requested.max_objects, 1000000),
                            std::min(requested.max_xml_depth, 128u),
                            cli_counted_limit("intake.conversion.bytes", std::min(requested.max_conversion_bytes, byte_ceiling)),
                            std::min(requested.max_conversion_pages, 1000u)};
        if (editable) {
            auto access = inspect_command_path(path, grants);
            observe(observer, "source", access.path, access.state, access.state == "granted");
            if (access.state != "granted") refuse(access.state=="unavailable" ? "resource-unavailable" : "read-grant-denied", "Source is not a granted regular local file.", "Grant the source file or its root.");
            // Apply the same locality/symlink admission without deriving any actual write authority.
            Grants locality; locality.write_files.push_back(path);
            if (inspect_write_destination(path, locality).state != "granted")
                refuse("invalid-file", "Source must be a local regular file without symlink components.", "Use a stable local source.");
        }
        Grants source_grants=grants;
        if(!editable) source_grants.read_files.push_back(path); // M1 explicit inspection input authority
        auto access = inspect_command_path(path,source_grants);
        observe(observer, "source", access.path, access.state, access.state == "granted");
        auto size = admitted_size(access, limits.max_input_bytes, "load");
        observe(observer, "source", access.path, "read-begin", access.state == "granted");
        auto source=read_admitted(access,std::min(size,limits.max_input_bytes));
        observe(observer, "source", access.path, source.error.empty() ? "read-end" : "read-error", access.state == "granted", source.identity);
        if (source.error=="engine-limit" && source.found<=limits.max_input_bytes)
            refuse("stale-dependency", "Source grew after memory admission.", "Retry with a stable source.");
        if(source.error=="engine-limit") ceiling("input-bytes",source.found,limits.max_input_bytes);
        if(!source.error.empty()) refuse(source.error,"Admitted source could not be read.","Retry with a stable granted local file.");
        cli_fault_throw("intake.source.after-read");
        auto source_size=source.bytes.size();
        auto bytes = std::move(source.bytes);
        if (editable && editable->pages.size() > 1000)
            refuse("input-too-large", "Too many requested pages.", "Select at most 1000 pages.");
        std::string raster_format;
        array pdf_fonts;
        bool pdf = editable && (editable->format == "pdf" || bytes.starts_with("%PDF-"));
        if (pdf) {
            Extension::Internal::PdfIntakeOptions options;
            auto selected = editable->pages.empty() ? std::vector<unsigned>{1} : editable->pages;
            for (auto page : selected) {
                if (!page || page > INT_MAX) refuse("pages-invalid", "Invalid PDF page.", "Use existing 1-based pages.");
                options.pages.push_back(static_cast<int>(page));
            }
            options.font_policy = editable->font_policy == "reject" ?
                Extension::Internal::PdfFontPolicy::RejectMissing : Extension::Internal::PdfFontPolicy::SubstituteMissing;
            // Embedded images are mandatory: the API must never emit sidecars.
            options.embed_images = true;
            Extension::Internal::PdfInput input;
            observe(observer, "pdf", access.path, "parser-begin", true, source.identity);
            auto converted = input.open_request(std::move(bytes), std::filesystem::path(path).filename().string(), options);
            observe(observer, "pdf", access.path, "parser-end", true, source.identity);
            if (converted.error) {
                auto const &error = *converted.error;
                refuse(error.code == "pages-invalid" ? "pages-invalid" :
                       error.code == "fonts-missing" ? "font-policy-required" : "invalid-file",
                       error.message, "Choose existing pages and an explicit font policy for a valid PDF.");
            }
            for (auto const &font : converted.report.fonts)
                if (font.missing) pdf_fonts.emplace_back(object{{"family",font.name},{"substituted",true}});
            bytes = sp_repr_save_buf(converted.document->getReprDoc()).raw();
            ceiling("conversion-bytes", bytes.size(), limits.max_conversion_bytes);
        }
        bool cdr = editable && (editable->format == "cdr" ||
            (bytes.size() >= 12 && bytes.substr(0,4) == "RIFF" &&
             (bytes.substr(8,3) == "CDR" || bytes.substr(8,3) == "cdr")) ||
            (editable->format == "auto" && bytes.starts_with("PK\003\004")));
        if (cdr) {
            std::string error;
            observe(observer, "cdr", access.path, "parser-begin", true, source.identity);
            auto pages = Extension::cdr_svg_pages(bytes,error, {limits.max_conversion_bytes, limits.max_conversion_pages});
            observe(observer, "cdr", access.path, "parser-end", true, source.identity);
            if (pages.empty()) refuse(error == "input-too-large" ? error : "invalid-file",error,"Provide a valid supported CDR file.");
            auto requested_pages = editable->pages.empty() ? std::vector<unsigned>{1} : editable->pages;
            for (auto page : requested_pages)
                if (!page || page > pages.size())
                    refuse("pages-invalid", "Requested CDR page does not exist.", "Use existing 1-based pages.");
            std::unique_ptr<SPDocument> combined;
            double left = 0;
            std::size_t selected_bytes = 0;
            for (auto page : requested_pages) {
                auto const &svg = pages[page - 1];
                ceiling("conversion-bytes", selected_bytes + svg.size(), limits.max_conversion_bytes);
                ceiling("input-bytes", selected_bytes + svg.size(), limits.max_input_bytes);
                admit_memory(selected_bytes + svg.size(), "conversion");
                selected_bytes += svg.size();
                preflight(svg, limits);
                auto part = SPDocument::createNewDocFromMem(std::span<char const>{svg.data(),svg.size()});
                if (!part) refuse("invalid-file", "Cannot construct converted CDR page.", "Provide a valid CDR.");
                DocumentUndo::setUndoSensitive(part.get(),false);
                // RVNG emits point-valued SVG coordinates without a viewBox.
                if (!part->getRoot()->viewBox_set) {
                    part->setWidth(Util::Quantity(part->getWidth().quantity,"pt"),false);
                    part->setHeight(Util::Quantity(part->getHeight().quantity,"pt"),false);
                    part->setViewBox(Geom::Rect::from_xywh(0,0,part->getWidth().value("pt"),part->getHeight().value("pt")));
                }
                auto width=part->getWidth().value("px"), height=part->getHeight().value("px");
                if (!combined) {
                    combined=SPDocument::createNewDoc(nullptr,false);
                    DocumentUndo::setUndoSensitive(combined.get(),false);
                    combined->setWidth(Util::Quantity(width,"px"),false);
                    combined->setHeight(Util::Quantity(height,"px"),false);
                    combined->setViewBox(Geom::Rect::from_xywh(0,0,width,height));
                }
                combined->import(*part,nullptr,nullptr,Geom::Translate(left,0),nullptr,
                    SPDocument::ImportRoot::AlwaysGroup,SPDocument::ImportLayersMode::None,
                    SPDocument::ImportResources::Independent);
                combined->getPageManager().newPage(Geom::Rect::from_xywh(left,0,width,height),left==0);
                left += width + 20; // Same 20 CSS-pixel horizontal gap as PdfInput/SvgBuilder.
            }
            bytes = sp_repr_save_buf(combined->getReprDoc()).raw();
        }
        if (editable) {
            auto mime = raster_mime(bytes);
            if (mime == "image/png" || mime == "image/jpeg" || mime == "image/tiff") {
                raster_format = mime.substr(6);
                Bitmap::HeaderLimits hl; hl.maxPixels = 32000000; hl.maxEncodedBytes = limits.max_input_bytes;
                Bitmap::Budget budget(Bitmap::Budget::FixedLimitForTest{}, 2 * Bitmap::MiB);
                observe(observer, "raster", access.path, "parser-begin", true, source.identity);
                auto h = Bitmap::inspect({reinterpret_cast<std::uint8_t const *>(bytes.data()), bytes.size(), {}}, hl, budget);
                observe(observer, "raster", access.path, "parser-end", true, source.identity);
                if(mime=="image/tiff" && h.value.orientation>=5 && h.value.orientation<=8)
                    std::swap(h.value.width,h.value.height);
                auto encoded = g_base64_encode(reinterpret_cast<guchar const *>(bytes.data()), bytes.size());
                bytes = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"" +
                    std::to_string(h.value.width) + "\" height=\"" + std::to_string(h.value.height) + "\"><image width=\"" +
                    std::to_string(h.value.width) + "\" height=\"" + std::to_string(h.value.height) +
                    "\" xlink:href=\"data:" + mime + ";base64," + encoded + "\"/></svg>";
                g_free(encoded);
            }
        }
        bool compressed = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0x1f &&
                          static_cast<unsigned char>(bytes[1]) == 0x8b;
        if (compressed) {
            observe(observer, "svgz", access.path, "parser-begin", true, source.identity);
            bytes = gunzip(bytes, limits.max_input_bytes);
            observe(observer, "svgz", access.path, "parser-end", true, source.identity);
        }
        auto sniff = std::string_view(bytes);
        if (sniff.starts_with("\xef\xbb\xbf")) sniff.remove_prefix(3);
        auto first = sniff.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || sniff[first] != '<')
            refuse("unsupported-format", "Inspection accepts SVG and SVGZ content only.", "Format import arrives in M2; use SVG or SVGZ for M1 inspection.");
        ceiling("input-bytes", bytes.size(), limits.max_input_bytes);
        admit_memory(bytes.size(), "expanded-load");
        observe(observer, "svg", access.path, "parser-begin", true, source.identity);
        preflight(bytes, limits);
        auto dom = xmlReadMemory(bytes.data(), bytes.size(), nullptr, "UTF-8", XML_PARSE_NONET | XML_PARSE_HUGE);
        if (!dom) refuse("invalid-xml", "Cannot build the admitted XML tree.", "Provide well-formed UTF-8 SVG.");
        observe(observer, "svg", access.path, "parser-end", true, source.identity);
        std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> owner(dom, xmlFreeDoc);
        std::string format = pdf ? "pdf" : cdr ? "cdr" : raster_format.empty() ? (compressed ? "svgz" : "svg") : raster_format;
        if (editable && !editable->format.empty() && editable->format != "auto" && editable->format != format)
            refuse("format-unsupported", "Requested format differs from source content.", "Choose auto or the actual source format.");
        if (editable && !cdr && !pdf && !editable->pages.empty() && editable->pages != std::vector<unsigned>{1})
            refuse("pages-invalid", "This intake accepts the first source page only.", "Use pages [1].");
        result.report["format"] = format;
        result.report["source_bytes"] = bytes.size();
        auto hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<guchar const *>(bytes.data()), bytes.size());
        result.report["source_sha256"] = hash; g_free(hash);
        normalize(dom, std::filesystem::path(path).parent_path().string(), grants, limits, result.report, editable, observer);
        if (editable) {
            for (auto const &v : result.report.at("normalization").as_array())
                if (v.as_object().at("reason") != "")
                    refuse("resource-denied", "Editable intake refuses content that inspection would disable.", "Remove active content or unsupported references.");
            for (auto const &v : result.report.at("resources").as_array()) {
                auto const &r = v.as_object();
                if (r.at("state") == "embedded") continue;
                if (editable->resource_policy == "reject-external" || !r.at("loaded").as_bool())
                    refuse(r.at("state") == "remote" ? "remote-resource" : "resource-denied",
                        "An external resource cannot be admitted under the requested policy.", "Embed resources or grant every linked image with resource-policy embed.");
            }
            result.report["conversion"] = (!raster_format.empty() || cdr || pdf);
            result.report["dirty"] = (!raster_format.empty() || cdr || pdf);
            result.report["fonts"] = std::move(pdf_fonts);
            for (auto &v : result.report.at("resources").as_array())
                if (v.as_object().at("state")=="embedded") v.as_object()["href"]="[embedded]";
        }
        xmlChar *serialized = nullptr; int length = 0;
        xmlDocDumpMemoryEnc(dom, &serialized, &length, "UTF-8");
        std::unique_ptr<xmlChar, decltype(xmlFree)> snapshot(serialized, xmlFree);
        observe(observer, "native-document", access.path, "parser-begin", true, source.identity);
        if (!cli_fault("intake.native-document", CliFaultKind::Analysis))
            result.document = SPDocument::createNewDocFromMem(std::span<char const>{reinterpret_cast<char const *>(serialized), static_cast<std::size_t>(length)});
        observe(observer, "native-document", access.path, "parser-end", true, source.identity);
        if (!result.document) refuse("invalid-svg", "Native SVG construction failed.", "Simplify the SVG and inspect again.");
        if (editable) DocumentUndo::setUndoSensitive(result.document.get(),false);
        if (cdr && !result.document->getRoot()->viewBox_set) {
            result.document->setWidth(Util::Quantity(result.document->getWidth().quantity,"pt"),false);
            result.document->setHeight(Util::Quantity(result.document->getHeight().quantity,"pt"),false);
            result.document->setViewBox(Geom::Rect::from_xywh(0,0,result.document->getWidth().value("pt"),result.document->getHeight().value("pt")));
        }
        if(editable) result.source_version=object{{"identity",source.identity},{"sha256",source.sha256},{"bytes",source_size}};
        result.document->ensureUpToDate(); // All normalization/layout occurs before query baseline.
        if (editable) {
            std::function<void(XML::Node *)> fonts = [&](XML::Node *n) {
                auto o=result.document->getObjectByRepr(n);
                if (auto image=cast<SPImage>(o); image && (image->missing || !image->pixbuf))
                    refuse("invalid-file","Native image decoding failed.","Provide a complete supported raster image.");
                bool text_node=std::string_view(n->name())=="svg:text" || std::string_view(n->name())=="svg:tspan" ||
                    std::string_view(n->name())=="svg:flowRoot" || std::string_view(n->name())=="svg:flowPara" || std::string_view(n->name())=="svg:flowSpan";
                if (text_node && o && o->style && o->style->font_family.value()) {
                    auto families=font_families(o->style->font_family.value());
                    if(!families.empty()) {
                        std::string resolved;std::size_t index=0;
                        for(;index<families.size();++index) {resolved=available_family(families[index]);if(!resolved.empty()) break;}
                        if(index!=0) {
                            if(resolved.empty()) resolved=getSubstituteFontName("sans-serif");
                            auto const &first=families.front().name;
                            result.report.at("fonts").as_array().emplace_back(object{{"family",first},
                                {"substituted",editable->font_policy=="substitute"}});
                            if(editable->font_policy=="reject") refuse("font-policy-required","Source uses an unavailable first font family: "+first,
                                "Install the first listed font or explicitly choose substitute.");
                        }
                    }
                }
                for (auto c=n->firstChild();c;c=c->next()) fonts(c);
            };
            fonts(result.document->getReprRoot());
        }
        if (editable && (pdf || cdr)) {
            // Flush generated transforms/page defaults before the editable baseline;
            // otherwise the first import/Undo canonicalizes pre-existing XML.
            result.document->getRoot()->updateRepr();
            result.document->ensureUpToDate();
        }
        if (editable) DocumentUndo::setUndoSensitive(result.document.get(),true);
        result.document->setModifiedSinceSave(editable && (!raster_format.empty() || cdr || pdf));
        if (editable) result.document->setDocumentFilename(path.c_str());
        result.document_id = document_stamp(result.document.get()).id;
        // Native construction may introduce namedview/defs/metadata/perspective objects.
        std::set<std::string> mapped, input_ids;
        for (auto const &v : result.report.at("id_map").as_array()) mapped.insert(std::string(v.as_object().at("id").as_string()));
        for (auto n : elements(xmlDocGetRootElement(dom))) input_ids.insert(attr(n, "id"));
        std::function<void(XML::Node *)> collect = [&](XML::Node *n) {
            if (auto o = result.document->getObjectByRepr(n); o && o->getId()) {
                std::string id = o->getId();
                if (!input_ids.contains(id) && mapped.insert(id).second)
                    result.report.at("id_map").as_array().emplace_back(object{{"id", id}, {"source_index", nullptr}, {"original_id", nullptr}});
            }
            for (auto c = n->firstChild(); c; c = c->next()) collect(c);
        };
        collect(result.document->getReprRoot());
        auto doc = result.document.get(); reports.emplace(doc, result.report);
        doc->connectDestroy([doc] { reports.erase(doc); });
    } catch (Refusal const &e) {
        result.document.reset(); result.document_id.clear(); result.error = e.error;
    } catch (std::exception const &e) {
        result.document.reset(); result.document_id.clear();
        result.error = IntakeError{"intake-failed", e.what(), "Check the local SVG input and engine resources.", false, {}};
    }
    return result;
}
// Production never includes the test-only header. Its RAII wrapper exchanges this
// thread-local optional value, restoring both nested values and the unsampled state.
namespace detail {
std::optional<std::uint64_t> exchange_intake_memory_for_testing(std::optional<std::uint64_t> bytes) noexcept
{
    auto previous = available_memory_for_testing;
    available_memory_for_testing = bytes;
    return previous;
}
} // namespace detail
std::optional<IntakeError> admit_raster_export_memory(std::uint64_t pixels)
{
    // CLI-GAP-3/4 (build 31.1), Mac /usr/bin/time -l, baseline 59,195,392 B:
    // 61.56 MP TIFF peak 550,453,248 B (delta 491,257,856; 7.980 B/pixel),
    // PNG peak 65,863,680 B (delta 6,668,288; 0.108 B/pixel).
    // ceil(max measured delta/pixel * 1.5) = 12; retain 256 MiB.
    // 103.68 MP TIFF also exported: peak 853,704,704 B (7.663 B/pixel delta).
    constexpr std::uint64_t bytes_per_pixel = 12;
    constexpr std::uint64_t reserve_bytes = 256ull << 20;
    std::uint64_t bytes_needed;
    if (!Bitmap::checkedMul(pixels, bytes_per_pixel, bytes_needed) ||
        !Bitmap::checkedAdd(bytes_needed, reserve_bytes, bytes_needed)) bytes_needed = UINT64_MAX;
    auto const memory = available_memory();
    if (!memory.measured || bytes_needed > memory.bytes) {
        return IntakeError{"engine-limit", "Insufficient available physical memory for raster-export.",
            "Free memory or use a smaller raster export on this machine.", false,
            {{"reason", "insufficient-memory"}, {"phase", "raster-export"}, {"pixels", pixels},
             {"bytes_needed", bytes_needed}, {"available_bytes", memory.bytes},
             {"bytes_per_pixel", bytes_per_pixel}, {"reserve_bytes", reserve_bytes},
             {"measured", memory.measured}}};
    }
    return std::nullopt;
}
IntakeResult load_inspection_document(std::string const &path, Grants const &grants, IntakeLimits const &limits)
{ return load_document(path, grants, limits, nullptr); }
IntakeResult load_inspection_document_for_testing(std::string const &path, Grants const &grants,
                                                 IntakeLimits const &limits, IntakeObserver const &observer)
{ return load_document(path, grants, limits, nullptr, observer); }
std::vector<std::string> inspection_formats() { return {"svg", "svgz"}; }
boost::json::object const *inspection_report(SPDocument const *doc)
{
    auto it = reports.find(doc); return it == reports.end() ? nullptr : &it->second;
}
} // namespace Inkscape::VACardsCli

namespace Inkscape::VACardsCli {
IntakeResult load_editable_document(std::string const &path, Grants const &grants,
                                   FileLoadOptions const &options, IntakeLimits const &limits)
{
    if (options.resource_policy != "embed" && options.resource_policy != "reject-external") {
        IntakeResult r; r.error = IntakeError{"resource-denied", "An explicit resource policy is required.", "Choose embed or reject-external.", false, {}}; return r;
    }
    if (options.font_policy != "reject" && options.font_policy != "substitute") {
        IntakeResult r; r.error = IntakeError{"font-policy-required", "An explicit font policy is required.", "Choose reject or substitute.", false, {}}; return r;
    }
    auto result = load_document(path, grants, limits, &options, options.observe_for_testing);
    if (result.error && result.error->code == "unsupported-format") {
        result.error->code = "format-unsupported";
        result.error->message = "No admitted editable backend is available for this input format.";
        result.error->hint = "Use SVG, SVGZ, PDF, CDR, PNG, JPEG or TIFF.";
    }
    return result;
}
IntakeResult prepare_file_snapshot(SPDocument const &source, Grants const &grants, std::string const &policy)
{ return prepare_file_snapshot_for_testing(source, grants, policy, {}); }
IntakeResult prepare_file_snapshot_for_testing(SPDocument const &source, Grants const &grants,
                                              std::string const &policy, IntakeObserver const &observer)
{
    IntakeResult result;
    try {
        admit_memory(snapshot_estimate(source), "save-snapshot");
        observe(observer, "snapshot-copy", {}, "parser-begin", false);
        auto copy=source.copy();
        cli_fault_throw("intake.snapshot.after-copy");
        observe(observer, "snapshot-copy", {}, "parser-end", false);
        auto bytes=sp_repr_save_buf(copy->getReprDoc()).raw();
        IntakeLimits limits;
        ceiling("snapshot-bytes",bytes.size(),limits.max_input_bytes);
        observe(observer, "snapshot-svg", {}, "parser-begin", false);
        preflight(bytes,limits);
        auto raw=xmlReadMemory(bytes.data(),bytes.size(),nullptr,"UTF-8",XML_PARSE_NONET | XML_PARSE_HUGE);
        if (!raw) refuse("invalid-file","Cannot parse snapshot.","Inspect document content.");
        std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)> dom(raw,xmlFreeDoc);
        observe(observer, "snapshot-svg", {}, "parser-end", false);
        FileLoadOptions opts{"svg",policy,"substitute",{}};
        normalize(raw,source.getDocumentBase() ? source.getDocumentBase() : "",grants,limits,result.report,&opts,observer);
        if (!result.report.at("normalization").as_array().empty())
            refuse("resource-denied","Snapshot contains unsafe or unsupported content.","Remove active or unsupported references.");
        for (auto const &v:result.report.at("resources").as_array()) {
            auto const &r=v.as_object();
            if (r.at("state")=="embedded") continue;
            if (policy=="reject-external" || !r.at("loaded").as_bool())
                refuse("resource-denied","Snapshot resource cannot be admitted.","Grant linked resources and use embed.");
        }
        xmlChar *out=nullptr; int n=0; xmlDocDumpMemoryEnc(raw,&out,&n,"UTF-8");
        std::unique_ptr<xmlChar,decltype(xmlFree)> owned(out,xmlFree);
        ceiling("snapshot-bytes", n, limits.max_input_bytes);
        admit_memory(n, "normalized-snapshot");
        observe(observer, "snapshot-native", {}, "parser-begin", false);
        result.document=SPDocument::createNewDocFromMem(std::span<char const>{reinterpret_cast<char const *>(out),static_cast<std::size_t>(n)});
        observe(observer, "snapshot-native", {}, "parser-end", false);
        if (!result.document) refuse("invalid-file","Native snapshot construction failed.","Inspect document content.");
        result.document->ensureUpToDate();
    } catch (Refusal const &e) { result.error=e.error; }
    catch (std::exception const &e) { result.error=IntakeError{"intake-failed",e.what(),"Inspect document resources.",false,{}}; }
    if (result.error) result.document.reset();
    return result;
}
} // namespace Inkscape::VACardsCli
