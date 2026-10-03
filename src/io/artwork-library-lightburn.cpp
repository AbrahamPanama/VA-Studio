// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-lightburn.h"

#include <2geom/affine.h>
#include <2geom/bezier-curve.h>
#include <glib.h>
#include <libxml/parser.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace Inkscape::IO::ArtworkLibrary {

LightBurnDraftError::LightBurnDraftError(LightBurnDraftErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), _code(code) {}

namespace {
using Code = LightBurnDraftErrorCode;
[[noreturn]] void fail(Code code, std::string const &message) { throw LightBurnDraftError(code, message); }
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
std::string_view trim(std::string_view s)
{
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}
void charge(std::size_t &used, std::size_t amount, std::size_t limit)
{
    if (used > limit || amount > limit - used) fail(Code::Limit, "LightBurn draft budget exceeded");
    used += amount;
}
void check_cancel(std::function<bool()> const &cancelled)
{
    if (cancelled && cancelled()) fail(Code::Cancelled, "LightBurn draft conversion cancelled");
}

struct Node {
    std::string tag, text;
    std::map<std::string, std::string> attrs;
    std::vector<std::unique_ptr<Node>> children;
};

// Bounded SAX tree: depth, nodes, attributes and text are checked before storage.
// Exceptions never cross libxml2's C callback boundary. There is no global loader
// replacement, global parser cleanup, entity substitution or external resource API.
struct Parse {
    explicit Parse(LightBurnDraftLimits value) : limits(value) {}
    LightBurnDraftLimits limits;
    xmlParserCtxtPtr ctxt = nullptr;
    std::unique_ptr<Node> root;
    std::vector<Node *> stack;
    std::size_t nodes = 0, text = 0;
    std::exception_ptr error;

    template <typename F> void guarded(F &&f) noexcept
    {
        if (error) return;
        try { f(); }
        catch (...) { error = std::current_exception(); if (ctxt) xmlStopParser(ctxt); }
    }
    static void start(void *ctx, xmlChar const *local, xmlChar const *prefix, xmlChar const *uri,
                      int namespaces, xmlChar const **, int attrs, int defaults, xmlChar const **values)
    {
        auto &p = *static_cast<Parse *>(ctx);
        p.guarded([&] {
            if (prefix || uri || namespaces || defaults || attrs < 0 || attrs > 8)
                fail(Code::Unsupported, "Namespaced/defaulted/excessive LightBurn attributes are unsupported");
            if (p.stack.size() >= std::min<std::size_t>(p.limits.xml_depth, 64))
                fail(Code::Limit, "LightBurn XML nesting limit exceeded");
            charge(p.nodes, 1, p.limits.xml_nodes);
            if (xmlStrlen(local) > 32) fail(Code::Unsupported, "Unsupported LightBurn element name");
            auto node = std::make_unique<Node>();
            node->tag = reinterpret_cast<char const *>(local);
            for (int i = 0; i < attrs; ++i) {
                auto a = values + 5 * i;
                if (a[1] || a[2] || xmlStrlen(a[0]) > 32 || a[4] - a[3] > 256)
                    fail(Code::Unsupported, "Unsupported LightBurn attribute");
                std::string key(reinterpret_cast<char const *>(a[0]));
                std::string value(reinterpret_cast<char const *>(a[3]), a[4] - a[3]);
                if (!node->attrs.emplace(std::move(key), std::move(value)).second)
                    fail(Code::Xml, "Duplicate LightBurn attribute");
            }
            auto raw = node.get();
            if (p.stack.empty()) {
                if (p.root) fail(Code::Xml, "Multiple LightBurn XML roots");
                p.root = std::move(node);
            } else {
                p.stack.back()->children.push_back(std::move(node));
            }
            p.stack.push_back(raw);
        });
    }
    static void end(void *ctx, xmlChar const *, xmlChar const *, xmlChar const *)
    {
        auto &p = *static_cast<Parse *>(ctx);
        p.guarded([&] {
            if (p.stack.empty()) fail(Code::Xml, "Unbalanced LightBurn XML");
            p.stack.pop_back();
        });
    }
    static void chars(void *ctx, xmlChar const *chars, int length)
    {
        auto &p = *static_cast<Parse *>(ctx);
        p.guarded([&] {
            if (length < 0) fail(Code::Xml, "Invalid LightBurn XML text");
            charge(p.text, static_cast<std::size_t>(length), p.limits.text_bytes);
            std::string_view text(reinterpret_cast<char const *>(chars), length);
            if (p.stack.empty()) {
                if (!trim(text).empty()) fail(Code::Xml, "Text outside LightBurn root");
            } else {
                p.stack.back()->text.append(text);
            }
        });
    }
    static void forbidden(void *ctx)
    {
        auto &p = *static_cast<Parse *>(ctx);
        p.guarded([] { fail(Code::Xml, "DTD, entity or processing instruction is forbidden"); });
    }
    static void subset(void *ctx, xmlChar const *, xmlChar const *, xmlChar const *) { forbidden(ctx); }
    static xmlParserInputPtr resolve(void *ctx, xmlChar const *, xmlChar const *) { forbidden(ctx); return nullptr; }
    static xmlEntityPtr entity(void *ctx, xmlChar const *) { forbidden(ctx); return nullptr; }
    static void pi(void *ctx, xmlChar const *, xmlChar const *) { forbidden(ctx); }
    static void silent(void *, char const *, ...) {}
};

void declaration(std::string_view input)
{
    if (input.starts_with("\xef\xbb\xbf")) input.remove_prefix(3);
    if (!input.starts_with("<?xml") || input.size() <= 5 || !space(input[5])) return;
    auto end = input.find("?>");
    if (end == std::string_view::npos || end > 512) fail(Code::Xml, "Invalid XML declaration");
    auto fields = trim(input.substr(5, end - 5));
    bool version = false, encoding = false, standalone = false;
    while (!fields.empty()) {
        auto split = fields.find('=');
        if (split == std::string_view::npos) fail(Code::Xml, "Malformed XML declaration");
        auto key = trim(fields.substr(0, split));
        fields = trim(fields.substr(split + 1));
        if (fields.empty() || (fields.front() != '\'' && fields.front() != '"'))
            fail(Code::Xml, "Malformed XML declaration value");
        auto quote = fields.front(); fields.remove_prefix(1);
        auto close = fields.find(quote);
        if (close == std::string_view::npos) fail(Code::Xml, "Unterminated XML declaration value");
        auto value = fields.substr(0, close);
        fields.remove_prefix(close + 1);
        if (!fields.empty() && !space(fields.front())) fail(Code::Xml, "Missing declaration separator");
        fields = trim(fields);
        if (key == "version" && !version) {
            if (value != "1.0") fail(Code::Unsupported, "Only XML version 1.0 is supported");
            version = true;
        } else if (key == "encoding" && !encoding) {
            if (g_ascii_strcasecmp(std::string(value).c_str(), "UTF-8"))
                fail(Code::Unsupported, "Only declared UTF-8 encoding is supported");
            encoding = true;
        } else if (key == "standalone" && !standalone) {
            if (value != "yes" && value != "no") fail(Code::Xml, "Invalid standalone declaration");
            standalone = true;
        } else fail(Code::Xml, "Unsupported or repeated XML declaration attribute");
    }
    if (!version) fail(Code::Xml, "XML declaration lacks version");
}

std::unique_ptr<Node> parse(std::string const &xml, LightBurnDraftLimits limits,
                            std::function<bool()> const &cancelled)
{
    if (xml.empty() || xml.size() > limits.xml_bytes || xml.size() > G_MAXSSIZE)
        fail(Code::Limit, "LightBurn XML byte limit exceeded");
    // This numeric-only schema needs no references, DTD, comments or CDATA.
    // Reject before parsing: NONET alone would not block local external entities.
    // Validate the declaration BEFORE libxml2 can switch decoders: validating
    // raw UTF-8/no-NUL alone would not exclude an ASCII-compatible encoding.
    if (xml.find('\0') != std::string::npos || xml.find("<!") != std::string::npos ||
        xml.find('&') != std::string::npos || !g_utf8_validate(xml.data(), xml.size(), nullptr))
        fail(Code::Xml, "Only reference-free UTF-8 LightBurn XML is accepted");
    declaration(xml);
    Parse state{limits};
    xmlSAXHandler sax{};
    sax.initialized = XML_SAX2_MAGIC;
    sax.startElementNs = Parse::start;
    sax.endElementNs = Parse::end;
    sax.characters = Parse::chars;
    sax.ignorableWhitespace = Parse::chars;
    sax.internalSubset = Parse::subset;
    sax.externalSubset = Parse::subset;
    sax.resolveEntity = Parse::resolve;
    sax.getEntity = Parse::entity;
    sax.getParameterEntity = Parse::entity;
    sax.processingInstruction = Parse::pi;
    sax.warning = Parse::silent;
    sax.error = Parse::silent;
    sax.fatalError = Parse::silent;
    std::unique_ptr<xmlParserCtxt, decltype(&xmlFreeParserCtxt)> ctxt(
        xmlCreatePushParserCtxt(&sax, &state, nullptr, 0, nullptr), xmlFreeParserCtxt);
    if (!ctxt) fail(Code::Xml, "Cannot create LightBurn XML parser");
    state.ctxt = ctxt.get();
    xmlCtxtUseOptions(ctxt.get(), XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    ctxt->replaceEntities = 0;
    ctxt->loadsubset = 0;
    for (std::size_t pos = 0; pos < xml.size();) {
        check_cancel(cancelled);
        auto chunk = std::min<std::size_t>(4096, xml.size() - pos);
        int status = xmlParseChunk(ctxt.get(), xml.data() + pos, static_cast<int>(chunk), 0);
        if (state.error) std::rethrow_exception(state.error);
        if (status) fail(Code::Xml, "Malformed LightBurn XML");
        pos += chunk;
    }
    int status = xmlParseChunk(ctxt.get(), nullptr, 0, 1);
    if (state.error) std::rethrow_exception(state.error);
    if (status || !ctxt->wellFormed || !state.root || !state.stack.empty())
        fail(Code::Xml, "Incomplete LightBurn XML");
    if (ctxt->encoding && xmlStrcasecmp(ctxt->encoding, BAD_CAST "UTF-8") != 0)
        fail(Code::Unsupported, "Only declared UTF-8 encoding is supported");
    check_cancel(cancelled);
    return std::move(state.root);
}

class Scan {
public:
    explicit Scan(std::string_view s, double bound) : s(s), bound(bound) {}
    void ws() { while (pos < s.size() && space(s[pos])) ++pos; }
    void separator()
    {
        if (pos == s.size() || !space(s[pos])) fail(Code::Geometry, "Missing numeric separator");
        ws();
    }
    bool done() { ws(); return pos == s.size(); }
    bool at_end() const { return pos == s.size(); }
    bool take(std::string_view token)
    {
        ws();
        if (s.substr(pos, token.size()) != token) return false;
        pos += token.size(); return true;
    }
    double real()
    {
        ws(); auto start = pos;
        if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
        std::size_t digits = 0;
        while (pos < s.size() && digit(s[pos])) { ++pos; ++digits; }
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            while (pos < s.size() && digit(s[pos])) { ++pos; ++digits; }
        }
        if (!digits) fail(Code::Geometry, "Invalid LightBurn number");
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
            auto exponent = pos;
            while (pos < s.size() && digit(s[pos])) ++pos;
            if (exponent == pos) fail(Code::Geometry, "Invalid LightBurn exponent");
        }
        if (pos - start > 64) fail(Code::Limit, "LightBurn numeric token too long");
        auto token = std::string(s.substr(start, pos - start));
        char *end = nullptr; errno = 0;
        auto value = g_ascii_strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || errno == ERANGE ||
            !std::isfinite(value) || std::abs(value) > bound)
            fail(Code::Geometry, "Nonfinite or out-of-range LightBurn number");
        return value;
    }
    std::size_t index()
    {
        ws(); auto start = pos; std::size_t value = 0;
        while (pos < s.size() && digit(s[pos])) {
            unsigned n = s[pos++] - '0';
            if (value > (std::numeric_limits<std::size_t>::max() - n) / 10)
                fail(Code::Geometry, "LightBurn index overflow");
            value = value * 10 + n;
        }
        if (start == pos) fail(Code::Geometry, "Invalid LightBurn index");
        return value;
    }
private:
    std::string_view s;
    std::size_t pos = 0;
    double bound;
};

void attrs(Node const &node, std::initializer_list<std::string_view> allowed)
{
    for (auto const &[key, value] : node.attrs) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            fail(Code::Unsupported, "Unsupported attribute " + key + " on " + node.tag);
    }
}
std::string const &attr(Node const &node, std::string const &key)
{
    auto it = node.attrs.find(key);
    if (it == node.attrs.end()) fail(Code::Unsupported, "Missing " + key + " on " + node.tag);
    return it->second;
}
void container(Node const &node)
{
    if (!trim(node.text).empty()) fail(Code::Unsupported, "Unexpected text in " + node.tag);
}
Node const *child(Node const &node, std::string_view name, bool required = true)
{
    Node const *found = nullptr;
    for (auto const &c : node.children) if (c->tag == name) {
        if (found) fail(Code::Unsupported, "Duplicate " + std::string(name));
        found = c.get();
    }
    if (required && !found) fail(Code::Unsupported, "Missing " + std::string(name));
    return found;
}
void children(Node const &node, std::initializer_list<std::string_view> allowed)
{
    for (auto const &c : node.children) {
        if (std::find(allowed.begin(), allowed.end(), c->tag) == allowed.end())
            fail(Code::Unsupported, "Unsupported element " + c->tag + " inside " + node.tag);
    }
}
std::string_view leaf(Node const &node)
{
    attrs(node, {});
    if (!node.children.empty()) fail(Code::Unsupported, "Nested content inside " + node.tag);
    return trim(node.text);
}
bool flag(Node const &root, std::string const &key)
{
    auto const &v = attr(root, key);
    if (v != "True" && v != "False") fail(Code::Unsupported, "Invalid or unsupported " + key);
    return v == "True";
}
std::string number(double value)
{
    char b[G_ASCII_DTOSTR_BUF_SIZE];
    return g_ascii_dtostr(b, sizeof(b), value == 0 ? 0 : value);
}

struct Svg {
    std::string value;
    std::size_t limit;
    void add(std::string_view s)
    {
        if (value.size() > limit || s.size() > limit - value.size()) fail(Code::Limit, "SVG output budget exceeded");
        value.append(s);
    }
    void num(double n) { add(number(n)); }
    void point(Geom::Point const &p) { num(p[0]); add(" "); num(p[1]); add(" "); }
    void matrix(Geom::Affine const &a)
    {
        add("matrix(");
        for (unsigned i = 0; i < 6; ++i) { if (i) add(" "); num(a[i]); }
        add(")");
    }
};
struct Vertex { Geom::Point p; std::optional<Geom::Point> c0, c1; };
struct Edge { char type; std::size_t from, to; };

struct Convert {
    Convert(LightBurnDraftLimits value, std::function<bool()> cancel)
        : limits(value), cancelled(std::move(cancel)), body{{}, value.svg_bytes} {}
    LightBurnDraftLimits limits;
    std::function<bool()> cancelled;
    Svg body;
    LightBurnSvgDraft result;
    std::optional<Geom::Rect> bounds;
    bool compact = false, smooth = false, marker = false, radius = false, cut_index = false;
    std::size_t tab_pairs = 0, tab_metadata_bytes = 0;

    double checked(double v) const
    {
        if (!std::isfinite(v) || std::abs(v) > limits.coordinate_abs)
            fail(Code::Geometry, "Transformed LightBurn coordinate exceeds bounds");
        return v;
    }
    Geom::Point world(Geom::Point const &p, Geom::Affine const &a) const
    {
        auto q = p * a;
        checked(q[0]); checked(q[1]); return q;
    }
    void include(Geom::Rect const &r)
    {
        for (unsigned i = 0; i < 2; ++i) { checked(r.min()[i]); checked(r.max()[i]); }
        if (bounds) bounds->unionWith(r); else bounds = r;
    }
    double scalar(Node const &n, std::string const &key)
    {
        Scan s(attr(n, key), limits.coordinate_abs); auto v = s.real();
        if (!s.done()) fail(Code::Geometry, "Trailing data in LightBurn numeric attribute");
        return v;
    }
    Geom::Affine transform(Node const &n)
    {
        Scan s(leaf(*child(n, "XForm")), limits.coordinate_abs);
        std::array<double, 6> a{};
        for (unsigned i = 0; i < 6; ++i) { if (i) s.separator(); a[i] = s.real(); }
        if (!s.done()) fail(Code::Geometry, "XForm must contain exactly six numbers");
        return {a[0], a[1], a[2], a[3], a[4], a[5]};
    }
    std::vector<Vertex> vertices(Node const &n)
    {
        Scan s(leaf(n), limits.coordinate_abs); std::vector<Vertex> out;
        while (!s.done()) {
            check_cancel(cancelled);
            if (!s.take("V")) fail(Code::Unsupported, "Unknown VertList token");
            charge(result.vertices, 1, limits.vertices);
            auto x = s.real(); s.separator(); auto y = s.real();
            Vertex v{{x, y}, {}, {}};
            for (unsigned h = 0; h < 2; ++h) {
                if (!s.take(h ? "c1x" : "c0x")) continue;
                auto cx = s.real();
                if (s.take(h ? "c1y" : "c0y")) {
                    auto cy = s.real();
                    (h ? v.c1 : v.c0) = Geom::Point(cx, cy);
                } else {
                    // x-only 1 is an observed absent-handle marker, NOT x=1,y=0.
                    // It is allowed only if no cubic needs this handle (below).
                    if (cx != 1) fail(Code::Unsupported, "Ambiguous compact control has X but no Y; no missing ordinate is guessed");
                    marker = true;
                }
            }
            if (s.take("S")) smooth = true;
            out.push_back(std::move(v));
        }
        if (out.size() < 2) fail(Code::Geometry, "Path requires at least two vertices");
        return out;
    }
    std::vector<Edge> edges(Node const &n, std::size_t size)
    {
        auto text = leaf(n); std::vector<Edge> out;
        auto add = [&](char type, std::size_t from, std::size_t to) {
            charge(result.primitives, 1, limits.primitives);
            if (from >= size || to >= size || from == to) fail(Code::Geometry, "Invalid primitive vertex index");
            out.push_back({type, from, to});
        };
        if (text == "LineClosed" || text == "LineOpen") {
            compact = true;
            for (std::size_t i = 1; i < size; ++i) { check_cancel(cancelled); add('L', i - 1, i); }
            if (text == "LineClosed") add('L', size - 1, 0);
        } else {
            Scan s(text, limits.coordinate_abs);
            while (!s.done()) {
                check_cancel(cancelled);
                char type;
                if (s.take("L")) type = 'L';
                else if (s.take("B")) type = 'B';
                else fail(Code::Unsupported, "Unknown PrimList token");
                auto from = s.index(); s.separator(); auto to = s.index();
                add(type, from, to);
            }
        }
        if (out.empty()) fail(Code::Geometry, "Path has no primitives");
        return out;
    }
    void path(Node const &n, Geom::Affine const &local, Geom::Affine const &cumulative)
    {
        auto v = vertices(*child(n, "VertList"));
        auto e = edges(*child(n, "PrimList"), v.size());
        // Support ordered directed chains and disjoint chains/cycles. Never
        // reorder, reverse, join by coordinate proximity, or ignore orphan nodes.
        std::vector<unsigned char> incoming(v.size()), outgoing(v.size()), used(v.size());
        for (auto const &p : e) {
            check_cancel(cancelled);
            if (++outgoing[p.from] > 1 || ++incoming[p.to] > 1)
                fail(Code::Unsupported, "Branched or repeated primitive topology");
        }
        body.add("<path vector-effect=\"non-scaling-stroke\" transform=\"");
        body.matrix(local); body.add("\" d=\"");
        std::optional<std::size_t> current, start;
        for (auto const &p : e) {
            check_cancel(cancelled);
            if (!current || *current != p.from) {
                if (used[p.from]) fail(Code::Unsupported, "Unordered primitive chain");
                start = p.from;
                body.add("M "); body.point(v[p.from].p);
            }
            if (used[p.to] && p.to != *start) fail(Code::Unsupported, "Unordered or rejoined primitive chain");
            used[p.from] = used[p.to] = 1;
            auto a = world(v[p.from].p, cumulative), b = world(v[p.to].p, cumulative);
            if (p.type == 'L') {
                body.add("L "); body.point(v[p.to].p);
                include(Geom::Rect(a, b));
            } else {
                if (!v[p.from].c0 || !v[p.to].c1)
                    fail(Code::Unsupported, "Cubic requires explicit start c0 and end c1 coordinate pairs");
                auto c0 = world(*v[p.from].c0, cumulative), c1 = world(*v[p.to].c1, cumulative);
                include(Geom::CubicBezier(a, c0, c1, b).boundsExact());
                body.add("C "); body.point(*v[p.from].c0); body.point(*v[p.to].c1); body.point(v[p.to].p);
            }
            current = p.to;
            if (p.to == *start) { body.add("Z "); current.reset(); start.reset(); }
        }
        if (std::find(used.begin(), used.end(), 0) != used.end()) fail(Code::Unsupported, "Unreferenced path vertex");
        body.add("\"/>");
    }
    void rect(Node const &n, Geom::Affine const &local, Geom::Affine const &cumulative)
    {
        auto w = scalar(n, "W"), h = scalar(n, "H"), cr = scalar(n, "Cr");
        if (w <= 0 || h <= 0) fail(Code::Geometry, "Rectangle dimensions must be positive");
        if (cr < 0 || cr > std::min(w, h) / 2)
            fail(Code::Unsupported, "Reversed or oversized rectangle corner radius is unqualified");
        radius |= cr != 0;
        // Conservative transformed box for rounded rectangles; no flattening.
        for (double x : {-w / 2, w / 2}) for (double y : {-h / 2, h / 2}) {
            auto p = world({x, y}, cumulative); include(Geom::Rect(p, p));
        }
        body.add("<rect vector-effect=\"non-scaling-stroke\" transform=\""); body.matrix(local);
        body.add("\" x=\""); body.num(-w / 2); body.add("\" y=\""); body.num(-h / 2);
        body.add("\" width=\""); body.num(w); body.add("\" height=\""); body.num(h);
        body.add("\" rx=\""); body.num(cr); body.add("\" ry=\""); body.num(cr); body.add("\"/>");
    }
    void tabs(Node const &n)
    {
        auto text = leaf(n); // No attributes, nested nodes, or unknown XML content.
        if (text.empty()) return;
        // Recognize only observed finite numeric-pair syntax. These are opaque
        // laser metadata: no coordinate/index/unit/range meaning is invented.
        // Bounds constrain storage/parse work, not an asserted field meaning.
        charge(tab_metadata_bytes, n.text.size(), limits.tab_metadata_bytes);
        Scan s(text, 1e9);
        auto before = tab_pairs;
        while (!s.done()) {
            check_cancel(cancelled);
            charge(tab_pairs, 1, limits.tab_pairs);
            s.real();
            if (!s.take(",")) fail(Code::Unsupported, "Tabs requires comma-separated numeric pairs");
            s.real();
            if (!s.at_end()) s.separator(); // Pairs must be whitespace-separated.
        }
        result.unapplied_laser_tabs.push_back({result.shapes, tab_pairs - before, n.text});
    }
    void shape(Node const &n, Geom::Affine const &parent)
    {
        check_cancel(cancelled);
        charge(result.shapes, 1, limits.shapes);
        if (n.tag != "Shape") fail(Code::Unsupported, "Expected Shape");
        container(n);
        auto const &type = attr(n, "Type");
        if (type == "Group") { attrs(n, {"Type", "CutIndex"}); children(n, {"XForm", "Children"}); }
        else if (type == "Path") { attrs(n, {"Type", "CutIndex"}); children(n, {"XForm", "VertList", "PrimList", "Tabs"}); }
        else if (type == "Rect") { attrs(n, {"Type", "CutIndex", "W", "H", "Cr"}); children(n, {"XForm", "Tabs"}); }
        else fail(Code::Unsupported, "Unsupported LightBurn shape type: " + type);
        if (auto it = n.attrs.find("CutIndex"); it != n.attrs.end()) {
            Scan s(it->second, limits.coordinate_abs); auto index = s.index();
            if (!s.done() || index > std::numeric_limits<std::uint32_t>::max()) fail(Code::Geometry, "Invalid CutIndex");
            cut_index = true; // Layer identity is not original fill/stroke/color.
        }
        if (auto node = child(n, "Tabs", false)) tabs(*node);
        auto local = transform(n);
        auto cumulative = local * parent; // 2geom applies left operand first.
        for (unsigned i = 0; i < 6; ++i) checked(cumulative[i]);
        if (type == "Group") {
            auto const &list = *child(n, "Children"); attrs(list, {}); container(list); children(list, {"Shape"});
            if (list.children.empty()) fail(Code::Geometry, "Empty LightBurn group");
            body.add("<g transform=\""); body.matrix(local); body.add("\">");
            for (auto const &c : list.children) shape(*c, cumulative);
            body.add("</g>");
        } else if (type == "Path") path(n, local, cumulative);
        else rect(n, local, cumulative);
    }
};
} // namespace

LightBurnSvgDraft convert_lightburn_shapes_v1_draft(std::string xml, LightBurnDraftLimits limits,
                                                   std::function<bool()> cancelled)
{
    check_cancel(cancelled);
    if (!std::isfinite(limits.coordinate_abs) || limits.coordinate_abs <= 0 || limits.coordinate_abs > 1e9)
        fail(Code::Limit, "Invalid LightBurn coordinate limit (hard ceiling 1e9)");
    auto root = parse(xml, limits, cancelled);
    if (root->tag != "LightBurnShapes") fail(Code::Unsupported, "Expected LightBurnShapes root");
    attrs(*root, {"FormatVersion", "MirrorX", "MirrorY"}); container(*root); children(*root, {"Shape"});
    if (attr(*root, "FormatVersion") != "1") fail(Code::Unsupported, "Unsupported LightBurnShapes version");
    bool mx = flag(*root, "MirrorX"), my = flag(*root, "MirrorY");
    // Include the base Y-up -> SVG Y-down conversion ONCE outside the hierarchy.
    // True/True is X-only, confirmed against 36 private stored previews; other
    // combinations have original writer evidence and synthetic math oracles.
    // Preserve signed bounds; do not invent a machine-bed size or raw-extent scale.
    // Primary evidence and remaining qualification are recorded in SEMANTICS.md.
    Geom::Affine mirror(mx ? -1 : 1, 0, 0, my ? 1 : -1, 0, 0);
    Convert c{limits, std::move(cancelled)};
    c.body.add("<g fill=\"none\" stroke=\"#000000\" stroke-width=\"0.1\" stroke-linejoin=\"round\" stroke-linecap=\"round\" transform=\"");
    c.body.matrix(mirror); c.body.add("\">");
    for (auto const &n : root->children) c.shape(*n, mirror);
    c.body.add("</g>");
    if (!c.bounds) fail(Code::Geometry, "LightBurn entry contains no drawable geometry");
    // One millimeter of margin, including degenerate line-only bounds.
    auto x = c.bounds->min()[0] - 1, y = c.bounds->min()[1] - 1;
    auto w = c.bounds->max()[0] - c.bounds->min()[0] + 2;
    auto h = c.bounds->max()[1] - c.bounds->min()[1] + 2;
    Svg out{{}, limits.svg_bytes};
    out.add("<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\""); out.num(w);
    out.add("mm\" height=\""); out.num(h); out.add("mm\" viewBox=\"");
    out.num(x); out.add(" "); out.num(y); out.add(" "); out.num(w); out.add(" "); out.num(h);
    out.add("\"><desc>LightBurnShapes v1 geometry draft in millimeters; physical fidelity and source styling are not certified. Requires independent SVG preflight.");
    if (!c.result.unapplied_laser_tabs.empty())
        out.add(" Laser tabs not applied: full editable geometry retained; source tab metadata is returned separately.");
    out.add("</desc>");
    out.add(c.body.value); out.add("</svg>");
    c.result.svg = std::move(out.value);
    c.result.width_mm = w;
    c.result.height_mm = h;
    c.result.qualifications = {
        "Draft only: no LightBurn-generated known-dimension or visual-fidelity qualification.",
        "One SVG user unit is one millimeter, backed by vendor unit documentation; raw container extents are not interpreted. Physical fidelity still requires validation.",
        "Root signs are (MirrorX ? -1 : 1, MirrorY ? 1 : -1), applied once; True/True has private preview evidence, other combinations lack native paired qualification. Device-bed placement is not preserved.",
        "Neutral unfilled outlines are a display policy, not recovered source print or laser styling.",
        "Independent SVG preflight remains mandatory before SPDocument use."
    };
    if (!c.result.unapplied_laser_tabs.empty())
        c.result.qualifications.emplace_back("Laser tabs not applied on " +
            std::to_string(c.result.unapplied_laser_tabs.size()) +
            " shape(s): full editable geometry retained. Preserve returned unapplied_laser_tabs metadata with source provenance; pair semantics are not decoded.");
    if (c.compact) c.result.qualifications.emplace_back("LineOpen/LineClosed sequential expansion is an unqualified compact-format inference.");
    if (c.marker) c.result.qualifications.emplace_back("x-only control flag 1 is treated as absent; cubics needing it are refused.");
    if (c.smooth) c.result.qualifications.emplace_back("S flags preserve explicit curve geometry but not linked-handle editing constraints.");
    if (c.radius) c.result.qualifications.emplace_back("Positive Cr is mapped to SVG rx/ry in local user units; this mapping lacks paired LightBurn qualification.");
    if (c.cut_index) c.result.qualifications.emplace_back("CutIndex was recognized and validated but not mapped to source colors, fills or cut settings.");
    check_cancel(c.cancelled);
    return std::move(c.result);
}

} // namespace Inkscape::IO::ArtworkLibrary
