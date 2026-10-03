// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-svg-preflight.h"
#include "artwork-library-png-preflight.h"
#include "artwork-library-manifest.h"
#include "artwork-library-package.h"
#include "svg/svg.h"
#include "colors/parser.h"
#include <2geom/svg-path-parser.h>
#include <2geom/transforms.h>
#include <libxml/parser.h>
#include <libxml/entities.h>
#include <glib.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <string_view>

namespace Inkscape::IO::ArtworkLibrary {
struct ValidatedSvg::State {
    std::shared_ptr<std::string const> bytes;
    SvgPreflightExpectation expected;
    double width_mm, height_mm;
    SvgViewBox viewbox;
    SvgPreflightStats stats;
    std::vector<std::string> warnings, fonts;
};
ValidatedSvg::ValidatedSvg(std::shared_ptr<State const> s) : _state(std::move(s)) {}
std::shared_ptr<std::string const> ValidatedSvg::svg_bytes() const { return _state->bytes; }
std::string ValidatedSvg::asset_id() const { return _state->expected.asset_id; }
std::string ValidatedSvg::sha256() const { return _state->expected.sha256; }
double ValidatedSvg::width_mm() const { return _state->width_mm; }
double ValidatedSvg::height_mm() const { return _state->height_mm; }
SvgViewBox ValidatedSvg::view_box() const { return _state->viewbox; }
SvgPreflightStats ValidatedSvg::stats() const { return _state->stats; }
std::vector<std::string> ValidatedSvg::warnings() const { return _state->warnings; }
std::vector<std::string> ValidatedSvg::requested_font_families() const { return _state->fonts; }
unsigned ValidatedSvg::policy_version() const { return 3; }
SvgPreflightError::SvgPreflightError(SvgPreflightFailure f, std::string m)
    : std::runtime_error(std::move(m)), _failure(f) {}

namespace {
using F = SvgPreflightFailure;
constexpr std::string_view SVG = "http://www.w3.org/2000/svg";
constexpr std::string_view XLINK = "http://www.w3.org/1999/xlink";
constexpr std::string_view XML = "http://www.w3.org/XML/1998/namespace";
constexpr std::string_view INK = "http://www.inkscape.org/namespaces/inkscape";
constexpr std::string_view SOD = "http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd";
constexpr std::string_view RDF = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
constexpr std::string_view DC = "http://purl.org/dc/elements/1.1/";
constexpr std::size_t attribute_cap = 1024u * 1024;
constexpr std::size_t image_uri_cap = 8u * 1024 * 1024;
constexpr std::size_t style_cap = 65536;
[[noreturn]] void fail(F f, std::string m) { throw SvgPreflightError(f, std::move(m)); }
void require(bool b, F f, std::string const &m) { if (!b) fail(f, m); }
void charge(std::size_t &n, std::size_t add, std::size_t cap, char const *label)
{
    if (n > cap || add > cap - n) fail(F::LimitExceeded, label);
    n += add;
}
std::string_view trim(std::string_view s)
{
    while (!s.empty() && g_ascii_isspace(s.front())) s.remove_prefix(1);
    while (!s.empty() && g_ascii_isspace(s.back())) s.remove_suffix(1);
    return s;
}
bool member(std::string_view value, std::string_view choices)
{
    std::size_t at = 0;
    while (at < choices.size()) {
        auto end = choices.find(' ', at);
        if (end == choices.npos) end = choices.size();
        if (choices.substr(at, end - at) == value) return true;
        at = end + 1;
    }
    return false;
}
bool id_ok(std::string_view s)
{
    if (s.empty() || s.size() > 256 || !(g_ascii_isalpha(s[0]) || s[0] == '_')) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return g_ascii_isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':';
    });
}
struct Numbers {
    std::string_view value;
    std::size_t at = 0;
    void spaces() { while (at < value.size() && g_ascii_isspace(value[at])) ++at; }
    double one()
    {
        spaces(); auto start = at;
        if (at < value.size() && (value[at] == '+' || value[at] == '-')) ++at;
        unsigned digits = 0;
        while (at < value.size() && g_ascii_isdigit(value[at])) { ++at; ++digits; }
        if (at < value.size() && value[at] == '.') {
            ++at; while (at < value.size() && g_ascii_isdigit(value[at])) { ++at; ++digits; }
        }
        require(digits, F::Unsupported, "Expected finite decimal SVG number");
        // Lowercase em/ex are length units, not the start of an exponent.
        // Their legality is checked by the consuming metric parser. Keep
        // malformed scientific notation (1e, 1e+, etc.) a hard failure.
        bool font_unit = value.substr(at, 2) == "em" || value.substr(at, 2) == "ex";
        if (!font_unit && at < value.size() && (value[at] == 'e' || value[at] == 'E')) {
            ++at; if (at < value.size() && (value[at] == '+' || value[at] == '-')) ++at;
            auto exponent = at;
            while (at < value.size() && g_ascii_isdigit(value[at])) ++at;
            require(at != exponent, F::Unsupported, "Malformed exponent");
        }
        require(at - start <= 64, F::LimitExceeded, "Numeric token exceeds 64 bytes");
        auto text = std::string(value.substr(start, at - start));
        char *end = nullptr; auto result = g_ascii_strtod(text.c_str(), &end);
        require(end == text.c_str() + text.size() && std::isfinite(result), F::Unsupported, "Nonfinite SVG number");
        // Reject underflow-to-zero for a nonzero mantissa.
        auto mantissa = text.substr(0, text.find_first_of("eE"));
        require(result != 0 || mantissa.find_first_of("123456789") == mantissa.npos,
                F::Unsupported, "Underflowing SVG number");
        return result;
    }
};
struct Cost {
    std::size_t nodes = 1, height = 1, commands = 0, pixels = 0;
    double radius = 0;
    // Native text input stays in the enclosing layout's coordinates: ordinary
    // descendant item transforms are not applied by _buildLayoutInput.
    double text_layout_radius = 0;
    double font_gain = 1;
    // Text reachable by native style inheritance (XML children or use clones),
    // not merely by a paint/resource dependency. All dependencies still cost work.
    bool has_text = false;
    // Pattern content before this server's effective transform; local tile
    // attributes and the transformed prototype audit envelope are not inherited.
    double pattern_content_radius = 0;
};
struct Ref { std::string id, kind; };
struct Node {
    std::string name, uri;
    std::map<std::string, std::string> attrs;
    std::vector<std::size_t> children, edges;
    std::vector<Ref> refs;
    std::map<std::string, std::vector<Ref>> paint_values, effective_paints;
    std::map<std::string, std::string> gradient_effective;
    std::map<std::string, std::string> text_metrics;
    double font_bound = 16; // Conservative parent-aware envelope; never native layout.
    double font_gain = 1;
    // Native SPIBaselineShift::cascade copies then adds the parent for unset
    // or inherit (gain 2). Explicit values add one local shift (gain 1).
    double baseline_gain = 2, baseline_absolute = 0, baseline_relative = 0;
    std::size_t parent = 0, commands = 0, pixels = 0;
    std::size_t tile_factor = 1;
    double radius = 0, gain = 1, offset = 0, pattern_min = 0;
    double text_run_radius = 0; // Contextual text/position bound, before item transforms.
    double pattern_origin = 0; // Effective max(|x|, |y|), before patternTransform.
    double viewport_w = 0, viewport_h = 0;
    unsigned gradient_visit = 0;
    bool metadata = false;
    std::size_t layout_bytes = 0; // Text retained for post-reference consumption accounting.
    bool nonspace_text = false;
    bool layout_text = false; // Consumed in at least one native XML/use text context.
    unsigned visit = 0;
    Cost cost;
};

class Audit;
class Sink final : public Geom::PathSink {
public:
    explicit Sink(Audit &a, Node &n) : audit(a), node(n) {}
    void moveTo(Geom::Point const &p) override;
    void lineTo(Geom::Point const &p) override { moveTo(p); }
    void curveTo(Geom::Point const &a, Geom::Point const &b, Geom::Point const &p) override;
    void quadTo(Geom::Point const &a, Geom::Point const &p) override;
    void arcTo(double rx, double ry, double angle, bool, bool, Geom::Point const &p) override;
    void closePath() override;
    void flush() override {}
    void rethrow() const { if (error) std::rethrow_exception(error); }
private:
    Audit &audit; Node &node;
    std::exception_ptr error;
    // SVGPathParser::_pushCurve owns its incoming curve only after feed returns.
    // Never unwind through a sink callback: surface failure after the bounded feed.
    template <typename Call> void guard(Call call) noexcept
    {
        if (error) return;
        try { call(); } catch (...) { error = std::current_exception(); }
    }
};

class Audit {
public:
    SvgPreflightLimits limits;
    Cancelled cancelled;
    SvgPreflightStats stats;
    SvgViewBox viewbox {};
    double width_mm = 0, height_mm = 0;
    std::set<std::string> fonts;
    std::vector<Node> nodes;
    std::vector<std::size_t> stack;
    std::map<std::string, std::size_t> ids;
    xmlParserCtxtPtr parser = nullptr;
    std::exception_ptr error;
    std::size_t retained = 0, text_bytes = 0;
    double text_metric = 16, text_factor = 1.25;
    bool seen_root = false;
    explicit Audit(SvgPreflightLimits l, Cancelled c) : limits(l), cancelled(std::move(c)) {}
    void poll() const { if (cancelled && cancelled()) fail(F::Cancelled, "SVG preflight cancelled"); }
    template <typename Call> void callback(Call call) noexcept
    {
        if (error) return;
        try { poll(); call(); } catch (...) {
            error = std::current_exception();
            if (parser) xmlStopParser(parser);
        }
    }
    void coordinate(double v, Node *node = nullptr)
    {
        require(std::isfinite(v) && std::abs(v) <= limits.coordinate_magnitude, F::LimitExceeded, "Coordinate budget");
        if (node) node->radius = std::max(node->radius, std::abs(v));
    }
    void command(Node &n)
    {
        poll(); charge(stats.geometry_commands, 1, limits.geometry_commands, "Geometry command budget");
        ++n.commands;
    }
    // Deliberately avoid SVGLength's file-backed UnitTable initialization.
    double length(std::string_view s, bool percent = false, Node *node = nullptr)
    {
        s = trim(s); Numbers p{s}; double v = p.one();
        auto unit = trim(s.substr(p.at));
        require(unit.empty() || !g_ascii_isspace(s[p.at]), F::Unsupported, "Whitespace before a length unit");
        double scale = 1;
        if (unit == "mm") scale = 96 / 25.4;
        else if (unit == "cm") scale = 96 / 2.54;
        else if (unit == "in") scale = 96;
        else if (unit == "pt") scale = 96 / 72.;
        else if (unit == "pc") scale = 16;
        else if (unit == "%" && percent) scale = 0.01;
        else require(unit.empty() || unit == "px", F::Unsupported, "Relative/unknown length unit is unsupported");
        v *= scale; coordinate(v, node); return v;
    }
    std::vector<double> numbers(std::string_view s, std::size_t cap, Node *node = nullptr)
    {
        Numbers p{trim(s)}; std::vector<double> result;
        while (p.at < p.value.size()) {
            require(result.size() < cap, F::LimitExceeded, "Numeric-list budget");
            auto v = p.one(); coordinate(v, node); result.push_back(v);
            auto before = p.at; p.spaces(); bool spaced = p.at != before;
            if (p.at == p.value.size()) break;
            if (p.value[p.at] == ',') {
                ++p.at; p.spaces(); require(p.at < p.value.size(), F::Unsupported, "Trailing numeric separator");
            } else {
                require(spaced || p.value[p.at] == '+' || p.value[p.at] == '-', F::Unsupported, "Malformed numeric separator");
            }
        }
        return result;
    }
    void path(Node &n, std::string const &value)
    {
        // Bound number tokens before 2Geom's parser retains a partial token.
        std::size_t count = 0;
        for (std::size_t at = 0; at < value.size();) {
            poll(); unsigned char c = value[at];
            if (g_ascii_isspace(c) || c == ',' || member(std::string_view(value).substr(at, 1), "M m Z z L l H h V v C c S s Q q T t A a")) { ++at; continue; }
            Numbers p{std::string_view(value).substr(at)};
            coordinate(p.one());
            charge(count, 1, limits.geometry_commands * 7, "Path numeric-token budget");
            at += p.at;
        }
        Sink sink(*this, n); Geom::SVGPathParser p(sink);
        try {
            for (std::size_t at = 0; at < value.size(); at += 4096) {
                poll(); p.feed(value.data() + at, static_cast<int>(std::min<std::size_t>(4096, value.size() - at)));
                sink.rethrow();
            }
            p.finish(); sink.rethrow();
        } catch (SvgPreflightError const &) { throw; }
        catch (std::bad_alloc const &) { throw; }
        catch (std::exception const &) {
            sink.rethrow(); // Preserve an already-recorded cancellation/error.
            fail(F::Unsupported, "Native SVG path parser rejected path data");
        }
    }
    void transform(Node &n, std::string const &value)
    {
        require(value.size() <= 4096, F::LimitExceeded, "Transform byte budget");
        // Bound every numeric input before handing it to the existing reader.
        for (std::size_t at = 0; at < value.size();) {
            unsigned char c = value[at];
            if (g_ascii_isalpha(c) || g_ascii_isspace(c) || c == '(' || c == ')' || c == ',') { ++at; continue; }
            Numbers p{std::string_view(value).substr(at)}; coordinate(p.one()); at += p.at;
        }
        Geom::Affine a;
        require(sp_svg_transform_read(value.c_str(), &a), F::Unsupported, "Malformed native SVG transform");
        for (unsigned i = 0; i < 6; ++i) coordinate(a[i]);
        auto gain = std::max(std::abs(a[0]) + std::abs(a[2]), std::abs(a[1]) + std::abs(a[3]));
        double determinant = a[0] * a[3] - a[1] * a[2];
        require(std::isfinite(determinant) && std::abs(determinant) >= 1e-12 && gain <= 1e6,
                F::Unsupported, "Singular/extreme transform is unsupported");
        n.gain *= gain; n.offset = n.offset * gain + std::max(std::abs(a[4]), std::abs(a[5]));
        coordinate(n.gain); coordinate(n.offset);
    }
    void reference(Node &n, std::string_view value, std::string_view kind)
    {
        value = trim(value);
        require(value.size() > 1 && value.front() == '#', F::ForbiddenContent, "External/empty resource reference");
        require(id_ok(value.substr(1)), F::Unsupported, "Escaped/unsupported local reference spelling");
        charge(stats.references, 1, limits.references, "Reference budget");
        n.refs.push_back({std::string(value.substr(1)), std::string(kind)});
    }
    void paint(Node &n, std::string_view value, std::string_view kind)
    {
        value = trim(value);
        auto keyword = std::string(value);
        std::transform(keyword.begin(), keyword.end(), keyword.begin(), [](unsigned char c) { return g_ascii_tolower(c); });
        require(!member(keyword, "inherit unset initial revert revert-layer context-fill context-stroke") || value == keyword,
                F::Unsupported, "Noncanonical CSS control keyword is unsupported");
        require(!member(keyword, "revert revert-layer initial unset"), F::Unsupported,
                "Paint control keyword is not recognized by this native SPIPaint parser");
        require(value != "context-fill" && value != "context-stroke", F::Unsupported,
                "Context-dependent paint is not yet budgeted");
        if (value.starts_with("url(")) {
            auto end = value.find(')');
            require(end != value.npos, F::Unsupported, "Malformed local URL");
            auto ref = trim(value.substr(4, end - 4));
            if (ref.size() >= 2 && (ref[0] == '\'' || ref[0] == '"') && ref.back() == ref.front()) {
                ref.remove_prefix(1); ref.remove_suffix(1);
            }
            reference(n, ref, kind);
            value = trim(value.substr(end + 1));
            if (value.empty()) return;
            require(kind == "paint", F::Unsupported, "Resource fallback is unsupported");
        }
        if (kind != "paint") {
            require(value == "none", F::Unsupported, "Expected local resource URL or none"); return;
        }
        if (member(value, "inherit none currentColor")) return;
        // Strict color subset, not a general CSS parser: no escaped identifiers,
        // functions except numeric rgb/rgba/hsl/hsla, or arbitrary CSS tokens.
        if (value.starts_with('#')) {
            auto digits = value.substr(1);
            require((digits.size() == 3 || digits.size() == 4 || digits.size() == 6 || digits.size() == 8) &&
                    std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return g_ascii_isxdigit(c); }),
                    F::Unsupported, "Unsupported hexadecimal color");
            semantic_color(value); return;
        }
        auto open = value.find('(');
        if (open != value.npos) {
            require(value.back() == ')' && member(value.substr(0, open), "rgb rgba hsl hsla"), F::Unsupported, "Unsupported color function");
            auto args = value.substr(open + 1, value.size() - open - 2);
            require(!args.empty(), F::Unsupported, "Empty color function");
            for (unsigned char c : args) require(g_ascii_isdigit(c) || g_ascii_isspace(c) ||
                c == ',' || c == '.' || c == '%' || c == '+' || c == '-', F::Unsupported, "Unsupported color token");
            // Percent signs are separators for magnitude validation, NOT URL tokens.
            auto normalized = std::string(args); std::replace(normalized.begin(), normalized.end(), '%', ' ');
            auto values = numbers(normalized, 4);
            require(values.size() == 3 || values.size() == 4, F::Unsupported, "Color arity");
            semantic_color(value); return;
        }
        require(value != "context-fill" && value != "context-stroke", F::Unsupported,
                "Context-dependent paint fallback is not yet budgeted");
        require(!value.empty() && value.size() <= 64 &&
                std::all_of(value.begin(), value.end(), [](unsigned char c) { return g_ascii_isalpha(c) || c == '-'; }),
                F::Unsupported, "Unsupported color keyword");
        semantic_color(value);
    }
    void semantic_color(std::string_view value)
    {
        // Same color parser used by native Color::parse/SPIPaint, without a
        // document CMS or Manager initialization. Lexical admission above rules
        // out trailing garbage, ICC lookup, recursion and excessive token sizes.
        Colors::Space::Type type;
        std::string cms; std::vector<double> values, fallback;
        require(value.size() <= 1024 && Colors::Parsers::get().parse(std::string(value), type, cms, values, fallback) &&
                cms.empty() && fallback.empty() &&
                (type == Colors::Space::Type::RGB || type == Colors::Space::Type::CSSNAME || type == Colors::Space::Type::HSL) &&
                (values.size() == 3 || values.size() == 4) &&
                std::all_of(values.begin(), values.end(), [](double x) { return std::isfinite(x); }),
                F::Unsupported, "Paint is not a native-supported semantic color");
    }
    void property(Node &n, std::string_view key, std::string_view value)
    {
        if (member(key, "fill stroke text-decoration-fill text-decoration-stroke")) {
            auto control = trim(value);
            // SPIPaint strips leading whitespace, but tests exact control words
            // before Color::parse; a padded presentation value such as "none "
            // is unset/inherited natively, not an explicit no-paint override.
            if (member(control, "none currentColor inherit"))
                require(value == control, F::Unsupported, "Noncanonical paint control whitespace");
        }
        value = trim(value);
        // Native text-style controller writes an empty SPIString to return
        // caps synthesis to automatic; it has no resource or layout payload.
        if (key == "-inkscape-font-variant-caps-mode" && value.empty()) return;
        require(!value.empty() && value.find_first_of("\\{}@;") == value.npos &&
                value.find("/*") == value.npos && value.find("*/") == value.npos,
                F::Unsupported, "Escaped/commented/active CSS syntax is unsupported");
        if (key == "enable-background") {
            require(value == "accumulate", F::Unsupported, "Backdrop-producing enable-background is not admitted"); return;
        }
        if (member(key, "fill stroke color stop-color flood-color lighting-color text-decoration-color text-decoration-fill text-decoration-stroke")) {
            auto before = n.refs.size(); paint(n, value, "paint");
            if (member(key, "fill stroke text-decoration-fill text-decoration-stroke")) {
                if (value == "inherit" || value == "unset") n.paint_values.erase(std::string(key));
                else n.paint_values[std::string(key)] = std::vector<Ref>(n.refs.begin() + before, n.refs.end());
            }
            return;
        }
        if (member(key, "shape-inside shape-subtract")) {
            // Native multi-column frame writer emits a whitespace-separated URL
            // list. Resolve and charge EVERY target, not only the first column.
            if (value == "none") return;
            std::size_t count = 0;
            while (!value.empty()) {
                require(++count <= 20 && value.starts_with("url("), F::LimitExceeded, "Text region list budget/syntax");
                auto end = value.find(')');
                require(end != value.npos, F::Unsupported, "Unterminated text region URL");
                paint(n, value.substr(0, end + 1), "shape");
                value.remove_prefix(end + 1);
                require(value.empty() || g_ascii_isspace(value.front()), F::Unsupported, "Text region separator");
                value = trim(value);
            }
            return;
        }
        if (member(key, "clip-path mask filter marker-start marker-mid marker-end")) {
            paint(n, value, key == "clip-path" ? "clipPath" : key == "mask" ? "mask" : key == "filter" ? "filter" : key.starts_with("marker") ? "marker" : "shape"); return;
        }
        if (member(key, "filter cursor color-profile")) {
            require(value == "none" || (key == "color-profile" && member(value, "auto sRGB")) ||
                    (key == "cursor" && value == "auto"), F::Unsupported, "Filter/cursor/profile resource is unsupported");
            return;
        }
        if (member(key, "font-size letter-spacing word-spacing line-height baseline-shift text-indent "
                        "shape-padding shape-margin inline-size -inkscape-language-spacing "
                        "-inkscape-paragraph-spacing-before -inkscape-paragraph-spacing-after")) {
            // Resolve after all declarations, independent of CSS serialization
            // order. Inline style replaces presentation attributes in this map.
            n.text_metrics[std::string(key)] = std::string(value); return;
        }
        if (member(key, "stroke-width stroke-dashoffset textLength")) {
            if (member(value, "normal inherit initial unset")) return;
            auto metric = length(value, false, &n);
            if (key == "textLength") text_metric = std::max(text_metric, std::abs(metric));
            return;
        }
        if (member(key, "opacity fill-opacity stroke-opacity stop-opacity flood-opacity stroke-miterlimit")) {
            auto v = length(value, true);
            require(v >= 0 && v <= (key == "stroke-miterlimit" ? 1000 : 1), F::Unsupported, "Style scalar range"); return;
        }
        if (key == "stroke-dasharray") {
            if (value == "none") return;
            auto vals = numbers(value, 256, &n);
            require(!vals.empty() && std::all_of(vals.begin(), vals.end(), [](double v) { return v >= 0; }),
                    F::Unsupported, "Dash array"); return;
        }
        if (key == "font-family" || key == "-inkscape-font-specification") {
            require(value.size() <= 1024 && value.find_first_of("()<>:") == value.npos, F::Unsupported, "Font descriptor syntax");
            // Preserve descriptions, don't resolve/load fonts in this service.
            fonts.insert(std::string(value));
            require(fonts.size() <= 128, F::LimitExceeded, "Font descriptor budget"); return;
        }
        require(member(key, "fill-rule clip-rule display visibility overflow stroke-linecap stroke-linejoin "
                            "vector-effect paint-order image-rendering shape-rendering text-rendering color-rendering "
                            "color-interpolation color-interpolation-filters font-style font-weight font-stretch "
                            "font-variant font-variant-ligatures font-variant-caps font-variant-numeric font-variant-position "
                            "font-variant-east-asian font-variant-alternates font-feature-settings font-variation-settings font-kerning text-anchor text-align "
                            "text-decoration text-decoration-line text-decoration-style text-transform "
                            "writing-mode direction unicode-bidi white-space isolation mix-blend-mode dominant-baseline "
                            "alignment-baseline text-orientation -inkscape-font-variant-caps-mode"),
                F::Unsupported, "Unsupported CSS property: " + std::string(key));
        require(value.size() <= 1024 && value.find_first_of("():/\\") == value.npos,
                F::Unsupported, "Unsupported CSS value syntax");
        for (unsigned char c : value) {
            require(g_ascii_isalnum(c) || g_ascii_isspace(c) || c == '-' || c == '_' || c == ',' ||
                    c == '\'' || c == '"' || c == '.', F::Unsupported, "Unsupported CSS value token");
        }
        for (std::size_t at = 0; at < value.size();) {
            if (!g_ascii_isdigit(value[at]) && value[at] != '.') { ++at; continue; }
            Numbers p{value.substr(at)}; coordinate(p.one()); at += p.at;
        }
    }
    void style(Node &n, std::string_view text)
    {
        require(text.size() <= style_cap, F::LimitExceeded, "Inline style budget");
        require(text.find_first_of("\\{}@") == text.npos && text.find("/*") == text.npos &&
                text.find("*/") == text.npos, F::Unsupported, "Escaped/commented stylesheet syntax is unsupported");
        std::size_t at = 0, declarations = 0;
        std::set<std::string> seen_properties;
        while (at < text.size()) {
            auto end = text.find(';', at); if (end == text.npos) end = text.size();
            auto decl = trim(text.substr(at, end - at)); at = end + 1;
            if (decl.empty()) continue;
            require(++declarations <= 128, F::LimitExceeded, "CSS declaration budget");
            auto colon = decl.find(':');
            require(colon != decl.npos, F::Unsupported, "Malformed CSS declaration");
            auto key = trim(decl.substr(0, colon)), value = trim(decl.substr(colon + 1));
            require(seen_properties.insert(std::string(key)).second, F::Unsupported,
                    "Repeated inline declaration/importance cascade is not yet supported");
            if (value.ends_with("!important")) value = trim(value.substr(0, value.size() - 10));
            property(n, key, value);
        }
    }
    void image(Node &n, std::string const &uri)
    {
        constexpr std::string_view prefix = "data:image/png;base64,";
        require(uri.starts_with(prefix), F::Unsupported, "Only embedded base64 PNG is admitted; no auto-decoder");
        std::string encoded; encoded.reserve(uri.size() - prefix.size());
        for (unsigned char c : std::string_view(uri).substr(prefix.size())) {
            if (g_ascii_isspace(c)) continue;
            require(g_ascii_isalnum(c) || c == '+' || c == '/' || c == '=', F::Unsupported, "Malformed base64");
            encoded.push_back(c);
        }
        require(!encoded.empty() && encoded.size() % 4 == 0, F::Unsupported, "Base64 quartet");
        auto pad = encoded.find('=');
        require(pad == encoded.npos || (pad >= encoded.size() - 2 &&
                encoded.find_first_not_of('=', pad) == encoded.npos), F::Unsupported, "Base64 padding");
        gsize size = 0;
        std::unique_ptr<guchar, decltype(&g_free)> decoded(g_base64_decode(encoded.c_str(), &size), g_free);
        require(decoded != nullptr, F::Unsupported, "Base64 decode failed");
        std::unique_ptr<gchar, decltype(&g_free)> canonical(g_base64_encode(decoded.get(), size), g_free);
        require(canonical && encoded == canonical.get(), F::Unsupported, "Noncanonical base64 padding bits");
        Bytes bytes(decoded.get(), decoded.get() + size);
        n.pixels = detail::preflight_png(bytes, limits.image_pixels, cancelled);
        charge(stats.image_pixels, n.pixels, limits.total_image_pixels, "Aggregate decoded pixel budget");
    }
    void start(xmlChar const *local, xmlChar const *uri, int ns_count, xmlChar const **namespaces,
               int attr_count, xmlChar const **attrs);
    void text(xmlChar const *content, int length);
    void analyze();
    Cost cost(std::size_t index, std::size_t depth);
    void viewport(Node &n, std::size_t index);
    void scaffolding(Node const &n);
    void gradient(std::size_t index, std::size_t depth);
    void pattern(std::size_t index);
    void text_metrics(Node &n, std::size_t index);
    void text_consumption(std::size_t index, std::size_t depth, bool in_text,
                          bool in_flow, std::size_t &instances);
    void inline_extents(std::size_t index, std::size_t depth, double inherited_percent,
                        double viewport_bound, std::size_t &instances);
    void text_extents(std::size_t index, std::size_t depth, double parent_baseline,
                      double parent_font, double font_gain, std::size_t &instances);
    void filters(Node &n);
};
void Sink::moveTo(Geom::Point const &p)
{
    guard([&] { audit.command(node); audit.coordinate(p[0], &node); audit.coordinate(p[1], &node); });
}
void Sink::curveTo(Geom::Point const &a, Geom::Point const &b, Geom::Point const &p)
{
    guard([&] {
        audit.command(node); audit.coordinate(p[0], &node); audit.coordinate(p[1], &node);
        audit.coordinate(a[0], &node); audit.coordinate(a[1], &node);
        audit.coordinate(b[0], &node); audit.coordinate(b[1], &node);
    });
}
void Sink::quadTo(Geom::Point const &a, Geom::Point const &p)
{
    guard([&] {
        audit.command(node); audit.coordinate(p[0], &node); audit.coordinate(p[1], &node);
        audit.coordinate(a[0], &node); audit.coordinate(a[1], &node);
    });
}
void Sink::arcTo(double rx, double ry, double angle, bool, bool, Geom::Point const &p)
{
    guard([&] {
        audit.command(node); audit.coordinate(p[0], &node); audit.coordinate(p[1], &node);
        audit.coordinate(rx, &node); audit.coordinate(ry, &node); audit.coordinate(angle);
    });
}
void Sink::closePath() { guard([&] { audit.command(node); }); }

void Audit::start(xmlChar const *local, xmlChar const *namespace_uri, int ns_count,
                  xmlChar const **namespaces, int attr_count, xmlChar const **attrs)
{
    auto small = [](xmlChar const *s, std::size_t cap) {
        if (!s) return std::string{};
        std::size_t size = 0; while (size <= cap && s[size]) ++size;
        require(size <= cap, F::LimitExceeded, "XML name/namespace budget");
        return std::string(reinterpret_cast<char const *>(s), size);
    };
    charge(stats.nodes, 1, limits.nodes, "XML node budget");
    auto depth = stack.size() + 1;
    require(depth <= limits.depth, F::LimitExceeded, "XML nesting budget");
    stats.depth = std::max(stats.depth, depth);
    require(attr_count >= 0 && ns_count >= 0 &&
            std::size_t(attr_count) <= limits.attributes_per_node &&
            std::size_t(ns_count) <= limits.attributes_per_node, F::LimitExceeded, "XML attribute/namespace count");
    for (int i = 0; i < ns_count * 2; ++i) (void)small(namespaces[i], 256);
    Node n; n.name = small(local, 128); n.uri = small(namespace_uri, 256);
    require(!(n.uri == SVG && member(n.name, "style script foreignObject animate animateMotion animateTransform set discard")),
            F::ForbiddenContent, "Active content or stylesheet is forbidden at every depth");
    if (stack.empty()) {
        require(!seen_root && n.uri == SVG && n.name == "svg", F::MalformedXml, "Expected single namespaced SVG root");
        seen_root = true;
    } else {
        n.parent = stack.back(); n.metadata = nodes[n.parent].metadata || nodes[n.parent].name == "metadata";
        require(!(nodes[n.parent].uri == SOD && nodes[n.parent].name == "namedview"),
                F::Unsupported, "Only inert child-free namedview scaffolding is admitted");
    }
    if (n.uri == SVG) {
        require(member(n.name, "svg g defs path rect circle ellipse line polyline polygon use text tspan textPath "
                               "linearGradient radialGradient stop pattern clipPath mask image title desc metadata filter "
                               "feGaussianBlur feOffset feColorMatrix feFlood feBlend feComposite feMerge feMergeNode "
                               "feComponentTransfer feFuncR feFuncG feFuncB feFuncA feMorphology "
                               "flowRoot flowRegion flowRegionExclude flowDiv flowPara flowSpan flowLine"),
                F::Unsupported, "Unsupported SVG element: " + n.name);
        require(!n.metadata, F::Unsupported, "SVG elements inside metadata are not admitted");
    } else {
        require((n.uri == SOD && n.name == "namedview" && n.parent == 0 && !stack.empty()) ||
                (n.metadata && ((n.uri == RDF && member(n.name, "RDF Description")) ||
                (n.uri == DC && member(n.name, "title creator description subject date format language type")))),
                F::Unsupported, "Unsupported foreign XML/metadata element");
    }
    for (int i = 0; i < attr_count; ++i) {
        auto a = attrs + i * 5;
        auto key = small(a[0], 128), uri = small(a[2], 256);
        auto size = a[4] - a[3];
        require(size >= 0 && std::size_t(size) <= (n.name == "image" && key == "href" ? image_uri_cap : attribute_cap),
                F::LimitExceeded, "XML attribute byte budget");
        charge(retained, std::size_t(size) + key.size() + uri.size(), limits.xml_bytes, "Retained XML attribute budget");
        std::string value(reinterpret_cast<char const *>(a[3]), std::size_t(size));
        std::string lower = key;
        for (auto &c : lower) c = g_ascii_tolower(c);
        require(!lower.starts_with("on") && !(uri == XML && key == "base"),
                F::ForbiddenContent, "Event handlers and xml:base are forbidden");
        if (n.uri == SOD && n.name == "namedview") {
            require(uri.empty() || uri == INK || uri == XML, F::Unsupported, "Namedview attribute namespace");
            if (uri == INK) key = "inkscape:" + key;
            if (uri == XML) key = "xml:" + key;
        } else if (uri == XLINK) {
            require(key == "href", F::Unsupported, "Unsupported xlink attribute"); key = "xlink:href";
        } else if (uri == XML) {
            require(key == "space" || key == "lang", F::Unsupported, "Unsupported XML attribute");
            if (key == "space") require(member(value, "default preserve"), F::Unsupported, "xml:space value");
            key = "xml:" + key;
        } else if (uri == INK) {
            require(member(key, "label groupmode nesting-contour nesting-contour-version export-xdpi export-ydpi version collect isstock auto-region "
                                "bitmap-adjustment brightness contrast intensity highlights shadows midtones "
                                "list-marker list-style auto-hyphen hyphenation drop-cap drop-cap-lines "
                                "text-frame-owner text-frame-generated text-frame-width text-frame-height text-frame-columns text-frame-gap text-frame-align"),
                    F::Unsupported, "Unaudited Inkscape metadata attribute: " + key);
            auto integer = [&](double low, double high) {
                Numbers p{value}; auto v = p.one();
                require(p.at == value.size() && v == std::floor(v) && v >= low && v <= high,
                        F::Unsupported, "Native text metadata integer range");
            };
            if (member(key, "bitmap-adjustment brightness contrast intensity highlights shadows midtones")) {
                require(n.name == "feComponentTransfer", F::Unsupported, "Tone metadata on wrong element");
                if (key == "bitmap-adjustment") require(value == "tone-v1", F::Unsupported, "Unknown tone metadata version");
                else { Numbers p{value}; auto v = p.one();
                    require(p.at == value.size() && v >= -100 && v <= 100, F::Unsupported, "Tone parameter range"); }
            }
            if (member(key, "list-marker list-style auto-hyphen hyphenation drop-cap drop-cap-lines")) {
                require(member(n.name, "text tspan flowRoot flowPara flowSpan flowDiv"), F::Unsupported, "Paragraph metadata on wrong element");
                if (member(key, "list-marker auto-hyphen drop-cap")) require(value == "true", F::Unsupported, "Paragraph marker flag");
                if (key == "list-style") require(member(value, "bullet number"), F::Unsupported, "List style");
                if (key == "hyphenation") require(value == "auto", F::Unsupported, "Hyphenation marker");
                if (key == "drop-cap-lines") integer(2, 20);
            }
            if (key.starts_with("text-frame-")) {
                require(n.name == (key == "text-frame-owner" ? "rect" : "text"), F::Unsupported, "Text frame metadata on wrong element");
                if (key == "text-frame-owner") require(id_ok(value), F::Unsupported, "Text frame owner spelling");
                if (key == "text-frame-generated") require(value == "true", F::Unsupported, "Text frame generated flag");
                if (key == "text-frame-align") require(member(value, "top middle bottom"), F::Unsupported, "Text frame alignment");
                if (key == "text-frame-columns") integer(1, 20);
                if (member(key, "text-frame-width text-frame-height text-frame-gap")) {
                    Numbers p{value}; auto v = p.one();
                    require(p.at == value.size() && v >= 0 && (key == "text-frame-gap" || v > 0), F::Unsupported, "Text frame metric");
                    coordinate(v);
                }
            }
            if (key == "groupmode") require(member(value, "layer group"), F::Unsupported, "Group mode");
            if (key == "nesting-contour") require(member(value, "true false"), F::Unsupported, "Contour flag");
            if (key == "nesting-contour-version") require(value == "1", F::Unsupported, "Contour metadata version");
            if (key == "collect") require(value == "always", F::Unsupported, "Resource collect metadata");
            if (key == "isstock" || key == "auto-region") require(member(value, "true false"), F::Unsupported, "Resource boolean metadata");
            if (key == "export-xdpi" || key == "export-ydpi") {
                Numbers p{trim(value)}; auto v = p.one(); p.spaces();
                require(p.at == p.value.size() && v > 0 && v <= 100000, F::Unsupported, "Export DPI metadata");
            }
            key = "inkscape:" + key;
        } else if (uri == SOD) {
            require((key == "nodetypes" && value.find_first_not_of("csaz") == value.npos) ||
                    (key == "role" && n.name == "tspan" && member(value, "line paragraph")) ||
                    (key == "docname" && n.name == "svg"),
                    F::Unsupported, "Procedural Sodipodi geometry/metadata is not yet audited");
            key = "sodipodi:" + key;
        } else if (!uri.empty()) {
            require(n.metadata && uri == RDF && member(key, "about resource"), F::Unsupported, "Unsupported attribute namespace");
            require(value.empty() || (value.front() == '#' && id_ok(std::string_view(value).substr(1))),
                    F::ForbiddenContent, "External metadata URI is outside this admission subset");
            key = "rdf:" + key;
        }
        if (n.metadata || n.name == "metadata") {
            require(member(key, "id xml:lang xml:space rdf:about rdf:resource"), F::Unsupported,
                    "Unaudited attribute on inert metadata");
        }
        require(n.attrs.emplace(key, std::move(value)).second, F::MalformedXml, "Duplicate normalized attribute");
    }
    auto index = nodes.size();
    if (!stack.empty()) nodes[stack.back()].children.push_back(index);
    nodes.push_back(std::move(n)); stack.push_back(index);
}
void Audit::text(xmlChar const *content, int size)
{
    require(size >= 0, F::MalformedXml, "Invalid text callback length");
    charge(stats.nodes, 1, limits.nodes, "Text/CDATA event node budget");
    charge(retained, std::size_t(size), limits.xml_bytes, "Retained text budget");
    if (stack.empty()) return; // libxml enforces whitespace outside the root.
    auto &n = nodes[stack.back()];
    std::string_view s(reinterpret_cast<char const *>(content), std::size_t(size));
    if (n.metadata || member(n.name, "metadata title desc")) return;
    // A g outside a text root can later be cloned under one by use. Do not
    // discard its whitespace or reject its non-whitespace before resolving
    // those contexts. XML bytes/events remain bounded during SAX parsing.
    charge(n.layout_bytes, std::size_t(size), limits.xml_bytes, "Deferred text byte budget");
    n.nonspace_text = n.nonspace_text || !trim(s).empty();
}
void Audit::scaffolding(Node const &n)
{
    require(n.children.empty(), F::Unsupported, "Namedview must be inert and child-free");
    for (auto const &[key, value] : n.attrs) {
        if (key == "id") continue; // Already validated/registered by analyze().
        if (member(key, "pagecolor bordercolor inkscape:deskcolor")) {
            if (!value.empty()) { Node scratch; paint(scratch, value, "paint");
                require(scratch.refs.empty(), F::ForbiddenContent, "Namedview cannot load paint resources"); }
        } else if (key == "inkscape:document-units") {
            require(member(value, "px mm cm in pt pc"), F::Unsupported, "Namedview unit");
        } else if (member(key, "borderopacity inkscape:pageopacity")) {
            if (value.empty()) continue; // Native factory uses unset empty defaults.
            Numbers p{trim(value)}; auto v = p.one(); p.spaces();
            require(p.at == p.value.size() && v >= 0 && v <= 1, F::Unsupported, "Namedview opacity");
        } else if (member(key, "inkscape:showpageshadow inkscape:pagecheckerboard showborder showgrid showguides")) {
            require(member(value, "0 1 2 true false"), F::Unsupported, "Namedview display flag");
        } else fail(F::Unsupported, "Unaudited namedview setting: " + key);
    }
}

void Audit::viewport(Node &n, std::size_t index)
{
    if (index) {
        n.viewport_w = nodes[n.parent].viewport_w;
        n.viewport_h = nodes[n.parent].viewport_h;
    }
    if (n.uri != SVG || n.name != "svg") return;
    auto dimension = [&](char const *key, double basis, double fallback) {
        auto it = n.attrs.find(key);
        if (it == n.attrs.end()) return fallback;
        auto s = trim(it->second); auto v = length(s, index != 0);
        if (s.ends_with('%')) v *= basis;
        coordinate(v); return v;
    };
    auto w = dimension("width", n.viewport_w, n.viewport_w);
    auto h = dimension("height", n.viewport_h, n.viewport_h);
    auto x = index ? dimension("x", n.viewport_w, 0) : 0;
    auto y = index ? dimension("y", n.viewport_h, 0) : 0;
    require(w > 0 && h > 0, F::Unsupported, "Finite positive SVG viewport required");
    std::vector<double> vb{0, 0, w, h};
    if (auto it = n.attrs.find("viewBox"); it != n.attrs.end()) vb = numbers(it->second, 4);
    require(vb.size() == 4 && vb[2] >= 1e-9 && vb[3] >= 1e-9, F::Unsupported, "Nested viewBox extent");
    auto sx = w / vb[2], sy = h / vb[3];
    std::string aspect = "xMidYMid meet";
    if (auto it = n.attrs.find("preserveAspectRatio"); it != n.attrs.end()) aspect = std::string(trim(it->second));
    auto split = aspect.find(' ');
    auto align = aspect.substr(0, split);
    auto mode = split == aspect.npos ? "meet" : std::string(trim(std::string_view(aspect).substr(split + 1)));
    require(member(align, "none xMinYMin xMidYMin xMaxYMin xMinYMid xMidYMid xMaxYMid xMinYMax xMidYMax xMaxYMax") &&
            member(mode, "meet slice"), F::Unsupported, "Viewport preserveAspectRatio");
    if (align != "none") sx = sy = mode == "meet" ? std::min(sx, sy) : std::max(sx, sy);
    require(std::isfinite(sx) && std::isfinite(sy) && sx >= 1e-9 && sy >= 1e-9 && sx <= 1e6 && sy <= 1e6,
            F::LimitExceeded, "Viewport scale budget");
    // Admission envelope only: do not rewrite native viewport/overflow mapping.
    // Include the full meet/slice alignment slack even when clipping is visible.
    n.gain = std::max(sx, sy);
    n.offset = std::max(std::abs(x) + std::abs(vb[0] * sx) + std::abs(w - vb[2] * sx),
                        std::abs(y) + std::abs(vb[1] * sy) + std::abs(h - vb[3] * sy));
    coordinate(n.offset);
    n.viewport_w = vb[2]; n.viewport_h = vb[3];
}

void Audit::text_metrics(Node &n, std::size_t index)
{
    auto parent_font = index ? nodes[n.parent].font_bound : 16.;
    n.font_bound = parent_font;
    auto measure = [&](std::string_view value, double basis, bool unitless_factor) {
        Numbers p{value}; auto number = p.one(); auto unit = value.substr(p.at);
        double v;
        if (unit == "%" || unit == "em" || unit == "ex" || (unit.empty() && unitless_factor)) {
            auto factor = number * (unit == "%" ? 0.01 : unit == "ex" ? 0.5 : 1.);
            require(std::abs(factor) <= 16, F::LimitExceeded, "Relative text metric factor budget");
            v = factor * basis;
        } else v = length(value);
        coordinate(v);
        require(std::abs(v) <= 65536, F::LimitExceeded, "Computed text metric budget");
        return v;
    };
    if (auto it = n.text_metrics.find("font-size"); it != n.text_metrics.end()) {
        auto const &v = it->second;
        if (v == "inherit") {} // Native default is 12; 16 is an upper bound.
        else if (member(v, "xx-small x-small small medium large x-large xx-large")) n.font_bound = 24;
        else if (v == "smaller") n.font_bound = parent_font;
        else if (v == "larger") { n.font_bound = parent_font * 1.2; n.font_gain = 1.2; }
        else {
            auto computed = measure(v, parent_font, false);
            require(computed >= 1e-6, F::LimitExceeded, "Near-zero/negative font size");
            n.font_bound = std::max(parent_font, computed);
            Numbers p{v}; auto number = p.one(); auto unit = std::string_view(v).substr(p.at);
            if (member(unit, "% em ex")) n.font_gain = std::max(1., number * (unit == "%" ? .01 : unit == "ex" ? .5 : 1.));
        }
    }
    require(n.font_bound <= 65536, F::LimitExceeded, "Inherited font-size budget");
    coordinate(n.font_bound);
    text_metric = std::max(text_metric, n.font_bound);
    for (auto const &[key, value] : n.text_metrics) {
        if (key == "inline-size" && (value == "inherit" || value.ends_with('%'))) {
            if (value != "inherit") {
                auto factor = measure(value, 1, false);
                require(factor >= 0, F::Unsupported, "Negative inline-size percentage");
            }
            // Resolve against each consuming text's viewport after reference
            // validation, not this declaration's font size or source context.
            continue;
        }
        if (key == "baseline-shift") {
            if (value == "inherit") continue;
            n.baseline_gain = 1;
            if (value == "baseline") continue;
            if (value == "sub" || value == "super") {
                n.baseline_relative = value == "sub" ? .2 : .4; continue;
            }
            // Keep the existing numeric/metric admission limits. Retain the
            // formula too: a reference may supply a different parent font.
            auto metric = measure(value, parent_font, false);
            Numbers p{value}; auto number = p.one(); auto unit = std::string_view(value).substr(p.at);
            if (member(unit, "% em ex")) n.baseline_relative = std::abs(number) * (unit == "%" ? .01 : unit == "ex" ? .5 : 1.);
            else n.baseline_absolute = std::abs(metric);
            continue; // Baseline is a translation, not every glyph's advance.
        }
        if (key == "font-size" || value == "inherit") continue;
        if (value == "normal" && member(key, "line-height letter-spacing word-spacing -inkscape-language-spacing")) continue;
        // Language spacing uses U+0020 advance, not font size. This remains a
        // font-dependent extent estimate (no font lookup here).
        auto metric = measure(value, n.font_bound, key == "line-height");
        if (member(key, "line-height inline-size shape-padding shape-margin"))
            require(metric >= 0, F::Unsupported, "Negative text layout size");
        text_metric = std::max(text_metric, std::abs(metric));
        n.radius = std::max(n.radius, std::abs(metric));
    }
}

void Audit::text_consumption(std::size_t index, std::size_t depth, bool in_text,
                             bool in_flow, std::size_t &instances)
{
    poll(); require(depth <= limits.depth, F::LimitExceeded, "Text consumption context depth");
    charge(instances, 1, limits.expanded_nodes, "Expanded text consumption work");
    auto &n = nodes[index];
    if (n.metadata || member(n.name, "metadata title desc")) return;
    // SPText traverses ordinary children; SPFlowtext handles region shapes
    // separately. Keep both contexts when a nested text root also has its own
    // layout. Geometry/resource dependency edges are not text children.
    if (member(n.name, "flowRegion flowRegionExclude")) in_flow = false;
    if (n.name == "text") in_text = true;
    if (n.name == "flowRoot") in_flow = true;
    if (n.layout_bytes && (in_text || in_flow ||
        member(n.name, "text tspan textPath flowPara flowSpan flowDiv"))) n.layout_text = true;
    for (auto child : n.children) text_consumption(child, depth + 1, in_text, in_flow, instances);
    for (std::size_t i = 0; i < n.edges.size(); ++i)
        if (n.refs[i].kind == "use")
            text_consumption(n.edges[i], depth + 1, in_text, in_flow, instances);
}

void Audit::text_extents(std::size_t index, std::size_t depth, double parent_baseline,
                         double parent_font, double font_gain, std::size_t &instances)
{
    poll();
    require(depth <= limits.depth, F::LimitExceeded, "Text ancestry depth");
    charge(instances, 1, limits.expanded_nodes, "Expanded text ancestry work");
    auto &n = nodes[index];
    if (!n.cost.has_text) return;
    font_gain *= n.font_gain;
    require(std::isfinite(font_gain) && font_gain <= 4096, F::LimitExceeded, "Expanded relative font cascade budget");
    auto font = std::max(n.font_bound, parent_font * n.font_gain);
    require(std::isfinite(font) && font <= 65536, F::LimitExceeded, "Expanded inherited font-size budget");
    coordinate(font);
    auto local_baseline = n.baseline_absolute + parent_font * n.baseline_relative;
    require(std::isfinite(local_baseline) && local_baseline <= 65536,
            F::LimitExceeded, "Contextual baseline metric budget");
    auto baseline = parent_baseline * n.baseline_gain + local_baseline;
    coordinate(baseline);
    if (n.layout_text || member(n.name, "text tspan textPath flowRoot flowPara flowSpan flowDiv")) {
        // Count actual characters and actual ancestry. A baseline translates
        // the run once; neither the configured maximum depth nor an unrelated
        // sibling's relative-font chain multiplies this run's glyph estimate.
        auto glyphs = double(n.commands) * text_metric * text_factor * 2 * font_gain;
        n.radius = std::max(n.radius, glyphs + baseline);
        coordinate(n.radius);
        // Keep a separate untransformed channel. The normal geometry radius
        // still audits each item's transform/resource cost, but cannot stand
        // in for text assembled directly in an ancestor's native layout.
        n.text_run_radius = std::max(n.text_run_radius, n.radius);
    }
    // cost() still checks EVERY resource edge for cycles, depth and work. Only
    // XML children and use clones inherit this style context. Paint resources
    // keep their own XML-parent styles and are visited through the root XML walk;
    // a consumer is not their native parent. Do not memoize use contexts by ID.
    for (auto child : n.children) text_extents(child, depth + 1, baseline, font, font_gain, instances);
    for (std::size_t i = 0; i < n.edges.size(); ++i)
        if (n.refs[i].kind == "use")
            text_extents(n.edges[i], depth + 1, baseline, font, font_gain, instances);
}

void Audit::inline_extents(std::size_t index, std::size_t depth, double inherited_percent,
                           double viewport_bound, std::size_t &instances)
{
    poll(); require(depth <= limits.depth, F::LimitExceeded, "Inline-size context depth");
    charge(instances, 1, limits.expanded_nodes, "Expanded inline-size context work");
    auto &n = nodes[index];
    if (n.uri == SVG && n.name == "svg") {
        if (n.attrs.contains("viewBox")) {
            // Native SPViewBox::get_rctx sets the child viewport to viewBox.
            viewport_bound = std::max(n.viewport_w, n.viewport_h);
        } else {
            // No viewBox: native child viewport is the resolved width/height.
            // Use the parent maximum for either percentage axis conservatively,
            // including a nested SVG reached through a referenced group.
            auto dimension = [&](char const *key) {
                auto it = n.attrs.find(key);
                if (it == n.attrs.end()) return viewport_bound;
                auto value = trim(it->second); auto v = length(value, true);
                if (value.ends_with('%')) v *= viewport_bound;
                coordinate(v); return v;
            };
            viewport_bound = std::max(dimension("width"), dimension("height"));
        }
        coordinate(viewport_bound);
    }
    if (auto it = n.text_metrics.find("inline-size"); it != n.text_metrics.end() && it->second != "inherit") {
        inherited_percent = it->second.ends_with('%') ? length(it->second, true) : -1;
    }
    if (n.name == "text" && inherited_percent >= 0) {
        auto extent = inherited_percent * viewport_bound;
        coordinate(extent);
        require(std::isfinite(extent) && extent <= 65536, F::LimitExceeded, "Viewport inline-size metric budget");
        // This is the flow-frame size, not one glyph's advance. Keep it out of
        // the document-wide font metric so it is not multiplied by text length.
        n.radius = std::max(n.radius, extent);
    }
    for (auto child : n.children) inline_extents(child, depth + 1, inherited_percent, viewport_bound, instances);
    for (auto target : n.edges) inline_extents(target, depth + 1, inherited_percent, viewport_bound, instances);
}

void Audit::gradient(std::size_t index, std::size_t depth)
{
    poll(); require(depth <= limits.depth, F::LimitExceeded, "Gradient inheritance depth");
    auto &n = nodes[index];
    require(n.gradient_visit != 1, F::ForbiddenContent, "Gradient href cycle");
    if (n.gradient_visit == 2) return; // General graph cost also checks cached height.
    n.gradient_visit = 1;
    for (auto const &ref : n.refs) {
        if (!member(ref.kind, "linearGradient radialGradient")) continue;
        auto target = ids.at(ref.id);
        gradient(target, depth + 1);
        for (auto const &[key, value] : nodes[target].gradient_effective) {
            if (n.name == nodes[target].name || member(key, "gradientUnits gradientTransform spreadMethod"))
                n.gradient_effective[key] = value;
        }
    }
    for (auto const &[key, value] : n.attrs) {
        if (member(key, "gradientUnits gradientTransform spreadMethod x1 y1 x2 y2 cx cy r fx fy fr"))
            n.gradient_effective[key] = value;
    }
    n.gradient_visit = 2;
}

void Audit::filters(Node &n)
{
    auto is_primitive = n.name.starts_with("fe");
    if (is_primitive) {
        auto const &parent = nodes[n.parent];
        require(parent.uri == SVG && (parent.name == "filter" ||
                (n.name == "feMergeNode" && parent.name == "feMerge") ||
                (member(n.name, "feFuncR feFuncG feFuncB feFuncA") && parent.name == "feComponentTransfer")),
                F::Unsupported, "Filter primitive outside its typed parent");
        require(!member(n.name, "feMergeNode feFuncR feFuncG feFuncB feFuncA") || parent.name != "filter",
                F::Unsupported, "Filter child directly in filter");
        charge(stats.filter_primitives, 1, limits.filter_primitives, "Aggregate filter primitive budget");
        command(n);
    }
    auto scalar = [&](std::string const &s, double low, double high) {
        auto v = numbers(s, 1); require(v.size() == 1 && v[0] >= low && v[0] <= high,
                                      F::Unsupported, "Filter scalar range"); return v[0];
    };
    auto sequence = [&](std::string const &s, std::size_t cap, double low, double high) {
        auto v = numbers(s, cap);
        require(!v.empty() && std::all_of(v.begin(), v.end(), [&](double x) { return x >= low && x <= high; }),
                F::Unsupported, "Filter numeric list range"); return v;
    };
    for (auto const &[key, value] : n.attrs) {
        if (key == "id" || key == "class" || key.starts_with("inkscape:")) continue;
        if (key == "style") { style(n, value); continue; }
        if (member(key, "color-interpolation-filters flood-color flood-opacity")) { property(n, key, value); continue; }
        if (member(key, "x y width height")) {
            auto v = length(value, true);
            if (trim(value).ends_with('%')) require(std::abs(v) <= 4, F::LimitExceeded, "Filter percentage region range");
            if (key == "width" || key == "height") require(v > 0, F::Unsupported, "Empty filter region");
            // Interpretation of bbox versus userspace is checked on the parent
            // filter below. Numbers here never reach an unbounded native scanner.
            continue;
        }
        if (member(key, "filterUnits primitiveUnits")) {
            require(n.name == "filter" && member(value, "userSpaceOnUse objectBoundingBox"), F::Unsupported, "Filter unit"); continue;
        }
        if (key == "filterRes") {
            require(n.name == "filter", F::Unsupported, "filterRes on primitive");
            auto v = sequence(value, 2, 1, 4096);
            for (auto x : v) require(x == std::floor(x), F::Unsupported, "Integral filterRes required");
            continue;
        }
        if (member(key, "in in2 result")) {
            require(is_primitive && (key != "in2" || member(n.name, "feBlend feComposite")) && id_ok(value),
                    F::Unsupported, "Filter input/result spelling"); continue;
        }
        if (key == "stdDeviation" && n.name == "feGaussianBlur") { sequence(value, 2, 0, 1024); continue; }
        if (member(key, "dx dy") && n.name == "feOffset") { scalar(value, -1024, 1024); continue; }
        if (key == "radius" && n.name == "feMorphology") { sequence(value, 2, 0, 64); continue; }
        if (key == "mode" && n.name == "feBlend") {
            require(member(value, "normal multiply screen darken lighten"), F::Unsupported, "Blend mode"); continue;
        }
        if (key == "operator" && member(n.name, "feComposite feMorphology")) {
            require(member(value, n.name == "feComposite" ? "over in out atop xor arithmetic" : "erode dilate"),
                    F::Unsupported, "Filter operator"); continue;
        }
        if (member(key, "k1 k2 k3 k4") && n.name == "feComposite") { scalar(value, -16, 16); continue; }
        if (key == "type" && n.name == "feColorMatrix") {
            require(member(value, "matrix saturate hueRotate luminanceToAlpha"), F::Unsupported, "Color matrix type"); continue;
        }
        if (key == "values" && n.name == "feColorMatrix") {
            auto type = n.attrs.contains("type") ? n.attrs.at("type") : "matrix";
            // hueRotate is a periodic angle in degrees; matrix/saturate keep their ranges.
            auto v = type == "hueRotate" ? sequence(value, 1, -1e6, 1e6) : sequence(value, 20, -360, 360);
            require((type == "matrix" && v.size() == 20) ||
                    (type == "saturate" && v.size() == 1 && v[0] >= 0 && v[0] <= 1) ||
                    (type == "hueRotate" && v.size() == 1), F::Unsupported, "Color matrix arity/range"); continue;
        }
        if (member(n.name, "feFuncR feFuncG feFuncB feFuncA")) {
            if (key == "type") { require(member(value, "identity table discrete linear gamma"), F::Unsupported, "Transfer type"); continue; }
            if (key == "tableValues") { sequence(value, 256, 0, 1); continue; }
            if (member(key, "slope intercept amplitude offset")) { scalar(value, -16, 16); continue; }
            if (key == "exponent") { scalar(value, 0, 16); continue; }
        }
        fail(F::Unsupported, "Unaudited filter attribute: " + key);
    }
    if (n.name == "filter") {
        require(!n.children.empty() && n.children.size() <= 64, F::LimitExceeded, "Filter chain length");
        std::set<std::string> slots{"SourceGraphic", "SourceAlpha"};
        for (auto child : n.children) {
            auto const &p = nodes[child];
            require(p.uri == SVG && p.name.starts_with("fe") && !member(p.name, "feMergeNode feFuncR feFuncG feFuncB feFuncA"),
                    F::Unsupported, "Filter child type");
            auto input = [&](Node const &part, char const *key) {
                if (auto it = part.attrs.find(key); it != part.attrs.end())
                    require(slots.contains(it->second), F::ForbiddenContent, "Missing/forward/context-dependent filter input");
            };
            input(p, "in"); input(p, "in2");
            if (p.name == "feMerge") for (auto c : p.children) input(nodes[c], "in");
            if (auto it = p.attrs.find("result"); it != p.attrs.end()) {
                // SlotResolver::read recognizes built-ins before its result map.
                // A result with one of these names cannot shadow that built-in.
                require(!member(it->second, "SourceGraphic SourceAlpha BackgroundImage BackgroundAlpha FillPaint StrokePaint"),
                        F::Unsupported, "Reserved native filter result name");
                require(slots.insert(it->second).second, F::Unsupported, "Duplicate/reserved filter result name");
            }
        }
        bool bbox = !n.attrs.contains("filterUnits") || n.attrs.at("filterUnits") == "objectBoundingBox";
        for (auto key : {"x", "y", "width", "height"}) if (n.attrs.contains(key))
            require(std::abs(length(n.attrs.at(key), true)) <= (bbox ? 4 : 65536), F::LimitExceeded, "Filter region range");
        bool primitive_bbox = n.attrs.contains("primitiveUnits") && n.attrs.at("primitiveUnits") == "objectBoundingBox";
        for (auto child : n.children) {
            auto const &p = nodes[child];
            for (auto key : {"x", "y", "width", "height", "stdDeviation", "radius", "dx", "dy"}) {
                if (!p.attrs.contains(key)) continue;
                if (member(key, "x y width height")) {
                    auto v = length(p.attrs.at(key), true);
                    require(std::abs(v) <= (primitive_bbox ? 4 : 65536), F::LimitExceeded, "Filter primitive region range");
                } else if (primitive_bbox) {
                    auto v = numbers(p.attrs.at(key), 2);
                    for (auto x : v) require(std::abs(x) <= 4, F::LimitExceeded, "Bounding-box filter kernel/offset range");
                }
            }
        }
    } else if (n.name == "feMerge") {
        require(!n.children.empty() && n.children.size() <= 64, F::LimitExceeded, "Filter merge input count");
        for (auto c : n.children) require(nodes[c].name == "feMergeNode", F::Unsupported, "Merge child type");
    } else if (n.name == "feComponentTransfer") {
        std::set<std::string> channels;
        for (auto c : n.children) require(member(nodes[c].name, "feFuncR feFuncG feFuncB feFuncA") && channels.insert(nodes[c].name).second,
                                        F::Unsupported, "Duplicate/invalid transfer channel");
        bool marked = n.attrs.contains("inkscape:bitmap-adjustment");
        for (auto key : {"inkscape:brightness", "inkscape:contrast", "inkscape:intensity", "inkscape:highlights", "inkscape:shadows", "inkscape:midtones"})
            require(n.attrs.contains(key) == marked, F::Unsupported, "Incomplete/versionless tone parameter tuple");
    } else {
        require(n.children.empty(), F::Unsupported, "Unexpected filter primitive children");
        if (member(n.name, "feFuncR feFuncG feFuncB feFuncA"))
            require(n.attrs.contains("type"), F::Unsupported, "Transfer function requires explicit type");
    }
}

void Audit::pattern(std::size_t index)
{
    auto &n = nodes[index];
    // SPPattern::getTransform follows href until the first explicitly set
    // patternTransform. Local width/height do not cancel that inheritance.
    // References have already been type checked; bound traversal independently
    // because aggregate graph cycle/depth validation happens immediately after.
    std::string const *effective = nullptr;
    auto current = index;
    for (std::size_t depth = 0;; ++depth) {
        poll();
        require(depth < limits.depth, F::LimitExceeded, "Pattern inheritance depth");
        auto const &part = nodes[current];
        if (auto t = part.attrs.find("patternTransform"); t != part.attrs.end()) {
            effective = &t->second; break;
        }
        auto ref = std::find_if(part.refs.begin(), part.refs.end(), [](Ref const &r) { return r.kind == "pattern"; });
        if (ref == part.refs.end()) break;
        current = ids.at(ref->id);
    }
    Geom::Affine a;
    if (effective) {
        require(sp_svg_transform_read(effective->c_str(), &a), F::Unsupported, "Effective pattern transform");
        // Explicit local transforms were charged during the attribute pass.
        if (!n.attrs.contains("patternTransform")) transform(n, *effective);
    }
    auto inverse = a.inverse();
    auto norm = std::max(std::abs(inverse[0]) + std::abs(inverse[2]), std::abs(inverse[1]) + std::abs(inverse[3]));
    require(std::isfinite(norm) && norm > 0, F::Unsupported, "Effective pattern inverse");
    n.pattern_min /= norm;
    // Origins inherit independently of content and transform. Resolve the first
    // explicit value before charging the alias-local envelope; prototype tile
    // width/height are deliberately not inherited (each is required locally).
    for (auto key : {"x", "y"}) {
        current = index;
        for (std::size_t depth = 0;; ++depth) {
            poll();
            require(depth < limits.depth, F::LimitExceeded, "Pattern origin inheritance depth");
            auto const &part = nodes[current];
            if (auto origin = part.attrs.find(key); origin != part.attrs.end()) {
                n.pattern_origin = std::max(n.pattern_origin, std::abs(length(origin->second))); break;
            }
            auto ref = std::find_if(part.refs.begin(), part.refs.end(), [](Ref const &r) { return r.kind == "pattern"; });
            if (ref == part.refs.end()) break;
            current = ids.at(ref->id);
        }
    }
}

Cost Audit::cost(std::size_t index, std::size_t depth)
{
    poll(); require(depth <= limits.depth, F::LimitExceeded, "Expanded reference depth");
    auto &n = nodes[index];
    require(n.visit != 1, F::ForbiddenContent, "Cyclic resource graph");
    if (n.visit == 2) {
        require(n.cost.height - 1 <= limits.depth - depth, F::LimitExceeded, "Cached subtree at deeper reference depth");
        return n.cost;
    }
    n.visit = 1; Cost c; c.commands = n.commands; c.pixels = n.pixels; c.radius = n.radius;
    c.text_layout_radius = n.text_run_radius;
    c.has_text = n.layout_text || member(n.name, "text tspan textPath flowRoot flowPara flowSpan flowDiv");
    double prototype_radius = 0;
    auto include = [&](std::size_t child, bool prototype = false, bool inherits_text = true) {
        auto part = cost(child, depth + 1);
        charge(c.nodes, part.nodes, limits.expanded_nodes, "Expanded node budget");
        charge(c.commands, part.commands, limits.geometry_commands, "Expanded geometry budget");
        charge(c.pixels, part.pixels, limits.total_image_pixels, "Expanded image pixel budget");
        c.height = std::max(c.height, part.height + 1);
        if (prototype) {
            // href inherits content, not a nested transformed pattern instance.
            // Retain the already-checked prototype envelope without transforming
            // it again; all work, height and font-gain accounting stays intact.
            c.pattern_content_radius = std::max(c.pattern_content_radius, part.pattern_content_radius);
            prototype_radius = std::max(prototype_radius, part.radius);
        } else {
            c.radius = std::max(c.radius, part.radius);
            if (n.name == "pattern")
                c.pattern_content_radius = std::max(c.pattern_content_radius, part.radius);
        }
        c.font_gain = std::max(c.font_gain, part.font_gain);
        c.has_text = c.has_text || (inherits_text && part.has_text);
        if (inherits_text) c.text_layout_radius = std::max(c.text_layout_radius, part.text_layout_radius);
    };
    for (auto child : n.children) include(child);
    // analyze() appends one edge per ref in the same order. A paint dependency
    // targeting a pattern is not prototype inheritance, even with the same ID.
    for (std::size_t i = 0; i < n.edges.size(); ++i)
        include(n.edges[i], n.name == "pattern" && n.refs[i].kind == "pattern", n.refs[i].kind == "use");
    if (n.name == "pattern")
        c.radius = std::max(c.radius, c.pattern_content_radius);
    auto multiply = [&](std::size_t &value, std::size_t cap) {
        require(!value || n.tile_factor <= cap / value, F::LimitExceeded, "Expanded pattern work budget");
        value *= n.tile_factor;
    };
    multiply(c.nodes, limits.expanded_nodes);
    multiply(c.commands, limits.geometry_commands);
    multiply(c.pixels, limits.total_image_pixels);
    // SPText/SPFlowtext render their assembled layout under the root item
    // transform. Descendant g/use/text item transforms are not text-layout
    // transforms. Protect that channel here, without changing generic costs.
    if (member(n.name, "text flowRoot")) c.radius = std::max(c.radius, c.text_layout_radius);
    // Leave text_layout_radius untransformed for a possible enclosing layout,
    // including a nested text root or use clone. Non-inheriting resource edges
    // never propagate this channel (but remain fully charged above).
    // Translate pattern space by its effective origin BEFORE the transform.
    // pattern_content_radius excludes this origin; prototype_radius is already
    // checked in its own space and is merged below without another translation.
    c.radius = (c.radius + n.pattern_origin) * n.gain + n.offset;
    c.font_gain *= n.font_gain;
    require(std::isfinite(c.font_gain) && c.font_gain <= 4096, F::LimitExceeded, "Expanded relative font cascade budget");
    coordinate(c.radius);
    c.radius = std::max(c.radius, prototype_radius);
    require(c.height - 1 <= limits.depth - depth, F::LimitExceeded, "Expanded subtree depth");
    n.cost = c; n.visit = 2; return c;
}
void Audit::analyze()
{
    require(seen_root && stack.empty() && !nodes.empty(), F::MalformedXml, "Incomplete SVG document");
    auto &root = nodes.front();
    auto required = [](Node const &n, char const *key) -> std::string const & {
        auto it = n.attrs.find(key);
        require(it != n.attrs.end(), F::Unsupported, std::string("Required explicit attribute: ") + key);
        return it->second;
    };
    auto width = length(required(root, "width")), height = length(required(root, "height"));
    require(width > 0 && height > 0, F::DimensionMismatch, "Root width/height must be positive absolute lengths");
    width_mm = width * 25.4 / 96; height_mm = height * 25.4 / 96;
    require(std::isfinite(width_mm) && std::isfinite(height_mm) && width_mm > 0 && height_mm > 0,
            F::DimensionMismatch, "Root physical dimension conversion underflow/overflow");
    auto vb = numbers(required(root, "viewBox"), 4);
    require(vb.size() == 4 && vb[2] >= 1e-9 && vb[3] >= 1e-9, F::DimensionMismatch, "Finite positive viewBox required");
    viewbox = {vb[0], vb[1], vb[2], vb[3]};
    require(width / vb[2] <= 1e6 && height / vb[3] <= 1e6, F::LimitExceeded, "Extreme root viewport scale");
    std::size_t namedviews = 0;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        poll(); auto &n = nodes[index];
        if (auto id = n.attrs.find("id"); id != n.attrs.end()) {
            require(id_ok(id->second), F::Unsupported, "Unsupported ID spelling");
            require(ids.emplace(id->second, index).second, F::MalformedXml, "Duplicate SVG ID");
        }
        viewport(n, index);
        if (n.uri == SOD && n.name == "namedview") {
            require(++namedviews == 1, F::Unsupported, "Duplicate namedview scaffolding");
            scaffolding(n); continue;
        }
        if (n.metadata || n.name == "metadata") continue;
        if (n.name.starts_with("flow") && n.name != "flowRoot") {
            auto const &parent = nodes[n.parent];
            bool region = member(n.name, "flowRegion flowRegionExclude");
            require(parent.uri == SVG && (region ? parent.name == "flowRoot" :
                    member(parent.name, "flowRoot flowDiv flowPara flowSpan")), F::Unsupported, "Legacy text structural parent");
            if (region) {
                require(n.children.size() <= 20, F::LimitExceeded, "Legacy flow region shape budget");
                for (auto c : n.children) require(member(nodes[c].name, "path rect circle ellipse line polyline polygon use"),
                                                F::Unsupported, "Legacy flow region shape type");
            }
            if (n.name == "flowLine") require(n.children.empty(), F::Unsupported, "Legacy line break must be empty");
        }
        if (n.name == "filter" || n.name.starts_with("fe")) { filters(n); continue; }
        for (auto const &[key, value] : n.attrs) {
            poll();
            if (key.find(':') != key.npos && key != "xlink:href") continue; // Audited namespace attributes from SAX stage.
            if (key == "id" || key == "class") continue;
            if (key == "href" || key == "xlink:href") continue; // Resolve conflicting spellings once below.
            if (key == "style") continue; // Inline declarations override presentation attributes below.
            if (member(key, "transform gradientTransform patternTransform")) { transform(n, value); continue; }
            if (key == "d") {
                require(n.name == "path", F::Unsupported, "d on non-path"); path(n, value); continue;
            }
            if (key == "points") {
                require(member(n.name, "polyline polygon"), F::Unsupported, "points on unsupported element");
                auto points = numbers(value, limits.geometry_commands * 2, &n);
                require(points.size() >= 4 && points.size() % 2 == 0, F::Unsupported, "Invalid points");
                for (std::size_t i = 0; i < points.size(); i += 2) command(n);
                continue;
            }
            if (key == "viewBox") { require(n.name == "svg", F::Unsupported, "Resource viewBox mapping not yet supported"); continue; }
            if (key == "version") { require(value == "1.1" || value == "1.0" || value == "1.2" || value == "2.0", F::Unsupported, "SVG version"); continue; }
            if (key == "preserveAspectRatio") {
                if (n.name == "svg") continue; // Validated by viewport(), including nested align/meet/slice.
                require(member(value, "none xMinYMin xMidYMin xMaxYMin xMinYMid xMidYMid xMaxYMid xMinYMax xMidYMax xMaxYMax") ||
                    value == "xMidYMid meet" || value == "xMidYMid slice", F::Unsupported, "Unsupported preserveAspectRatio spelling"); continue;
            }
            if (member(key, "gradientUnits patternUnits patternContentUnits clipPathUnits maskUnits maskContentUnits")) {
                require(member(value, "userSpaceOnUse objectBoundingBox"), F::Unsupported, "Resource units"); continue;
            }
            if (key == "spreadMethod") { require(member(value, "pad reflect repeat"), F::Unsupported, "Gradient spread"); continue; }
            if (member(key, "x y x1 y1 x2 y2 cx cy r rx ry fx fy fr width height dx dy rotate startOffset textLength offset")) {
                if (n.name == "svg" && member(key, "x y width height")) continue; // Resolved viewport dimensions.
                if (n.name == "pattern" && member(key, "x y")) {
                    (void)length(value); continue; // Validate now; keep the effective origin separate in pattern().
                }
                // textPath startOffset is commonly a path-length percentage ("50%").
                bool percent = member(n.name, "linearGradient radialGradient stop mask") ||
                               (n.name == "textPath" && key == "startOffset");
                // Text positioning lists use the same strict numeric scanner, not
                // an SPObject/text layout engine. Relative font units are refused.
                if (member(n.name, "text tspan") && member(key, "x y dx dy rotate")) {
                    auto list = numbers(value, 4096, &n);
                    require(!list.empty(), F::Unsupported, "Empty text positioning list");
                } else {
                    auto v = length(value, percent, &n);
                    if (member(key, "r rx ry fr width height textLength")) require(v >= 0, F::Unsupported, "Negative geometry size");
                    if (key == "offset") require(v >= 0 && v <= 1, F::Unsupported, "Gradient offset range");
                }
                continue;
            }
            if (key == "lengthAdjust") { require(member(value, "spacing spacingAndGlyphs"), F::Unsupported, "lengthAdjust"); continue; }
            property(n, key, value); // Only explicitly admitted style properties.
        }
        if (auto inline_style = n.attrs.find("style"); inline_style != n.attrs.end()) style(n, inline_style->second);
        text_metrics(n, index);
        auto href = n.attrs.find("href"), xlink = n.attrs.find("xlink:href");
        require(href == n.attrs.end() || xlink == n.attrs.end() || href->second == xlink->second,
                F::ForbiddenContent, "Conflicting href/xlink:href");
        if (href == n.attrs.end()) href = xlink;
        if (n.name == "image") {
            require(href != n.attrs.end(), F::Unsupported, "Image missing embedded payload");
            image(n, href->second);
            require(length(required(n, "width")) > 0 && length(required(n, "height")) > 0, F::Unsupported, "Image viewport");
        } else if (href != n.attrs.end()) {
            require(member(n.name, "use textPath linearGradient radialGradient pattern"), F::Unsupported, "Unsupported href consumer");
            reference(n, href->second, n.name);
        } else {
            require(n.name != "use" && n.name != "textPath", F::Unsupported, "Missing required reference");
        }
        if (n.name == "pattern") {
            require(required(n, "patternUnits") == "userSpaceOnUse", F::Unsupported, "Object-bounding-box pattern tiling not yet bounded");
            if (auto u = n.attrs.find("patternContentUnits"); u != n.attrs.end()) {
                require(u->second == "userSpaceOnUse", F::Unsupported, "Object-bounding-box pattern content is unsupported");
            }
            auto w = length(required(n, "width")), h = length(required(n, "height"));
            require(w >= 1e-6 && h >= 1e-6, F::Unsupported, "Tiny/empty pattern tile");
            n.pattern_min = std::min(w, h); // Resolve effective href transform after reference validation.
        }
        if (member(n.name, "rect circle ellipse line")) {
            for (unsigned i = 0; i < 8; ++i) command(n);
        }
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        auto &n = nodes[i];
        if (i) n.effective_paints = nodes[n.parent].effective_paints;
        for (auto const &[property, refs] : n.paint_values) n.effective_paints[property] = refs;
        if (n.name == "use") {
            require(std::none_of(n.effective_paints.begin(), n.effective_paints.end(),
                        [](auto const &entry) { return !entry.second.empty(); }), F::Unsupported,
                    "Resource-bearing inherited use style context is not yet budgeted");
        }
        if (member(n.name, "path rect circle ellipse line polyline polygon text tspan textPath use flowRoot flowPara flowSpan flowDiv")) {
            for (auto const &[property, refs] : n.effective_paints) {
                for (auto const &ref : refs) {
                    if (std::any_of(n.refs.begin(), n.refs.end(), [&](Ref const &own) { return own.id == ref.id && own.kind == ref.kind; })) continue;
                    charge(stats.references, 1, limits.references, "Inherited paint reference budget");
                    n.refs.push_back(ref);
                }
            }
        }
        for (auto const &ref : n.refs) {
            auto it = ids.find(ref.id);
            require(it != ids.end(), F::ForbiddenContent, "Missing local reference target: " + ref.id);
            auto const &target = nodes[it->second];
            bool type = false;
            if (ref.kind == "paint") type = member(target.name, "linearGradient radialGradient pattern");
            else if (ref.kind == "use") type = member(target.name, "g path rect circle ellipse line polyline polygon text image use flowRoot");
            else if (ref.kind == "textPath") type = target.name == "path";
            else if (ref.kind == "shape") type = member(target.name, "path rect circle ellipse");
            else type = target.name == ref.kind ||
                (member(ref.kind, "linearGradient radialGradient") && member(target.name, "linearGradient radialGradient"));
            require(target.uri == SVG && !target.metadata && type, F::Unsupported, "Reference target type mismatch");
            n.edges.push_back(it->second);
        }
    }
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].name == "pattern") pattern(i);
    auto total = cost(0, 1);
    // First validate graph depth/cycles/work, then discover text reached through
    // XML and use clones. Charge each definition once; cost() charges every
    // expanded instance (and conservatively its original XML definition).
    std::size_t consumption_instances = 0;
    text_consumption(0, 1, false, false, consumption_instances);
    for (auto &n : nodes) {
        poll();
        require(!n.nonspace_text || n.layout_text, F::Unsupported,
                "Text outside supported text/metadata nodes");
        if (n.layout_text) {
            charge(text_bytes, n.layout_bytes, limits.geometry_commands, "Text/glyph input budget");
            charge(stats.geometry_commands, n.layout_bytes, limits.geometry_commands, "Text/geometry budget");
            charge(n.commands, n.layout_bytes, limits.geometry_commands, "Node text/geometry budget");
            n.radius = std::max(n.radius, double(n.layout_bytes) * 16);
        }
        n.visit = 0;
    }
    total = cost(0, 1); // Refresh has_text/commands before either envelope pass.
    std::size_t inline_instances = 0;
    inline_extents(0, 1, -1, 0, inline_instances);
    std::size_t text_instances = 0;
    text_extents(0, 1, 0, 16, 1, text_instances);
    for (auto &n : nodes) n.visit = 0;
    total = cost(0, 1);
    stats.expanded_nodes = total.nodes;
    stats.expanded_image_pixels = total.pixels;
    double extent = std::max({1., total.radius * 2, viewbox.width, viewbox.height});
    for (auto &n : nodes) {
        auto units = [&](char const *key, char const *fallback) {
            auto it = n.attrs.find(key); return it == n.attrs.end() ? std::string(fallback) : it->second;
        };
        if (member(n.name, "linearGradient radialGradient")) {
            gradient(static_cast<std::size_t>(&n - nodes.data()), 1);
            auto const &effective = n.gradient_effective;
            auto unit = effective.contains("gradientUnits") ? effective.at("gradientUnits") : "objectBoundingBox";
            bool bbox = unit == "objectBoundingBox";
            double range = bbox ? 1 : std::max(n.viewport_w, n.viewport_h);
            for (auto const &[key, value] : effective) {
                if (!member(key, "x1 y1 x2 y2 cx cy r fx fy fr")) continue;
                auto v = length(value, true);
                if (bbox) require(std::abs(v) <= 4, F::Unsupported, "Effective bbox gradient coordinate range");
                else if (trim(value).ends_with('%')) v *= std::max(n.viewport_w, n.viewport_h);
                coordinate(v); range = std::max(range, std::abs(v));
            }
            if (effective.contains("gradientTransform")) {
                Node t; transform(t, effective.at("gradientTransform"));
                coordinate(range * t.gain + t.offset);
                if (bbox) require(range * t.gain + t.offset <= 16, F::Unsupported, "Effective bbox gradient transform range");
            }
        }
        if ((n.name == "mask" && units("maskContentUnits", "userSpaceOnUse") == "objectBoundingBox") ||
            (n.name == "clipPath" && units("clipPathUnits", "userSpaceOnUse") == "objectBoundingBox")) {
            require(n.cost.radius <= 4, F::Unsupported, "Bounding-box resource content range");
        }
        if (n.name == "mask" && units("maskUnits", "objectBoundingBox") == "objectBoundingBox") {
            for (auto key : {"x", "y", "width", "height"}) {
                if (auto it = n.attrs.find(key); it != n.attrs.end()) {
                    require(std::abs(length(it->second, true)) <= 4, F::Unsupported, "Bounding-box mask extent");
                }
            }
        }
        if (n.pattern_min) {
            require(n.pattern_min > 0 && std::isfinite(n.pattern_min) &&
                    extent / n.pattern_min <= 512, F::LimitExceeded, "Pattern tile repetition budget (512 per axis)");
            auto across = static_cast<std::size_t>(std::ceil(extent / n.pattern_min)) + 2;
            n.tile_factor = across * across; // Includes boundary-overlap tiles.
        }
    }
    for (auto &n : nodes) n.visit = 0;
    total = cost(0, 1); // Recheck memoized expansion with actual tiling work charged.
    stats.expanded_nodes = total.nodes;
    stats.expanded_image_pixels = total.pixels;
    stats.geometry_commands = total.commands;
}
void parse(Audit &audit, std::string const &input)
{
    xmlSAXHandler sax {};
    sax.initialized = XML_SAX2_MAGIC;
    sax.startElementNs = [](void *p, xmlChar const *local, xmlChar const *, xmlChar const *uri,
                            int ns, xmlChar const **namespaces, int count, int, xmlChar const **attrs) {
        auto &a = *static_cast<Audit *>(p);
        a.callback([&] { a.start(local, uri, ns, namespaces, count, attrs); });
    };
    sax.endElementNs = [](void *p, xmlChar const *, xmlChar const *, xmlChar const *) {
        auto &a = *static_cast<Audit *>(p); a.callback([&] {
            require(!a.stack.empty(), F::MalformedXml, "Unexpected closing element"); a.stack.pop_back();
        });
    };
    sax.characters = [](void *p, xmlChar const *s, int len) {
        auto &a = *static_cast<Audit *>(p); a.callback([&] { a.text(s, len); });
    };
    sax.ignorableWhitespace = sax.characters;
    sax.cdataBlock = sax.characters;
    sax.comment = [](void *p, xmlChar const *) {
        auto &a = *static_cast<Audit *>(p); a.callback([&] { charge(a.stats.nodes, 1, a.limits.nodes, "Comment node budget"); });
    };
    sax.processingInstruction = [](void *p, xmlChar const *, xmlChar const *) {
        auto &a = *static_cast<Audit *>(p); a.callback([] { fail(F::ForbiddenContent, "Processing instructions are forbidden"); });
    };
    sax.internalSubset = [](void *p, xmlChar const *, xmlChar const *, xmlChar const *) {
        auto &a = *static_cast<Audit *>(p); a.callback([] { fail(F::ForbiddenContent, "DOCTYPE is forbidden"); });
    };
    sax.externalSubset = sax.internalSubset;
    sax.entityDecl = [](void *p, xmlChar const *, int, xmlChar const *, xmlChar const *, xmlChar *) {
        auto &a = *static_cast<Audit *>(p); a.callback([] { fail(F::ForbiddenContent, "Entity declarations are forbidden"); });
    };
    sax.resolveEntity = [](void *p, xmlChar const *, xmlChar const *) -> xmlParserInputPtr {
        auto &a = *static_cast<Audit *>(p); a.callback([] { fail(F::ForbiddenContent, "External entity access is forbidden"); }); return nullptr;
    };
    sax.getEntity = [](void *p, xmlChar const *name) -> xmlEntityPtr {
        if (auto builtin = xmlGetPredefinedEntity(name)) return builtin;
        auto &a = *static_cast<Audit *>(p); a.callback([] { fail(F::ForbiddenContent, "Only predefined XML entities are admitted"); }); return nullptr;
    };
    sax.getParameterEntity = [](void *p, xmlChar const *) -> xmlEntityPtr {
        auto &a = *static_cast<Audit *>(p); a.callback([] { fail(F::ForbiddenContent, "Parameter entities are forbidden"); }); return nullptr;
    };
    // No global error/entity loader changes, no recovery or HUGE parser option.
    // Variadic callbacks cannot use a C++ lambda on all supported compilers.
    struct Errors {
        static void report(void *p, char const *, ...) {
            auto &a = *static_cast<Audit *>(p); a.callback([] { fail(F::MalformedXml, "Strict XML parser rejected input"); });
        }
    };
    sax.error = Errors::report; sax.fatalError = Errors::report;
    std::unique_ptr<xmlParserCtxt, decltype(&xmlFreeParserCtxt)> context(
        xmlCreatePushParserCtxt(&sax, &audit, nullptr, 0, nullptr), xmlFreeParserCtxt);
    require(context != nullptr, F::LimitExceeded, "Cannot allocate XML parser");
    audit.parser = context.get();
    xmlCtxtUseOptions(context.get(), XML_PARSE_NONET);
    context->validate = 0; context->loadsubset = 0;
    // Expansion is restricted by getEntity above to the five predefined values;
    // declarations/subsets are rejected before any general entity can exist.
    context->replaceEntities = 1;
    for (std::size_t at = 0; at < input.size(); at += 4096) {
        audit.poll();
        auto n = static_cast<int>(std::min<std::size_t>(4096, input.size() - at));
        auto status = xmlParseChunk(context.get(), input.data() + at, n, 0);
        if (audit.error) std::rethrow_exception(audit.error);
        require(status == 0, F::MalformedXml, "XML chunk rejected");
    }
    auto status = xmlParseChunk(context.get(), nullptr, 0, 1);
    if (audit.error) std::rethrow_exception(audit.error);
    require(status == 0 && context->wellFormed, F::MalformedXml, "Incomplete/malformed XML");
    if (context->encoding) require(!xmlStrcasecmp(context->encoding, BAD_CAST "UTF-8"), F::Unsupported, "Only UTF-8 XML encoding is admitted");
    audit.parser = nullptr;
}
void check_limits(SvgPreflightLimits const &l)
{
    SvgPreflightLimits hard;
    require(l.xml_bytes && l.xml_bytes <= hard.xml_bytes && l.nodes && l.nodes <= hard.nodes &&
        l.depth && l.depth <= hard.depth && l.attributes_per_node && l.attributes_per_node <= hard.attributes_per_node &&
        l.references <= hard.references && l.expanded_nodes && l.expanded_nodes <= hard.expanded_nodes &&
        l.geometry_commands && l.geometry_commands <= hard.geometry_commands &&
        l.image_pixels <= hard.image_pixels && l.total_image_pixels <= hard.total_image_pixels &&
        l.filter_primitives <= hard.filter_primitives &&
        std::isfinite(l.coordinate_magnitude) && l.coordinate_magnitude > 0 && l.coordinate_magnitude <= hard.coordinate_magnitude &&
        std::isfinite(l.dimension_tolerance_mm) && l.dimension_tolerance_mm >= 0 &&
        l.dimension_tolerance_mm <= hard.dimension_tolerance_mm, F::LimitExceeded, "Limits must not raise hard policy ceilings");
}
} // namespace

ValidatedSvg preflight_svg(std::span<unsigned char const> input, SvgPreflightExpectation expected,
                          SvgPreflightLimits limits, Cancelled cancelled)
{
    check_limits(limits);
    require(!input.empty() && input.size() <= limits.xml_bytes, F::LimitExceeded, "SVG byte budget");
    require(valid_library_uuid(expected.asset_id) && expected.sha256.size() == 64 &&
        expected.sha256.find_first_not_of("0123456789abcdef") == expected.sha256.npos, F::Integrity, "Invalid expected identity/hash");
    require(std::isfinite(expected.width_mm) && expected.width_mm > 0 && expected.width_mm <= 1e9 &&
            std::isfinite(expected.height_mm) && expected.height_mm > 0 && expected.height_mm <= 1e9,
            F::DimensionMismatch, "Invalid expected physical dimensions");
    auto bytes = std::make_shared<std::string const>(reinterpret_cast<char const *>(input.data()), input.size());
    Audit audit(limits, std::move(cancelled)); audit.poll();
    require(bytes->find('\0') == bytes->npos && g_utf8_validate(bytes->data(), bytes->size(), nullptr),
            F::MalformedXml, "Input must be NUL-free UTF-8");
    // Existing checksum helper pins this private copy; never hash mutable input.
    Bytes checksum_input(bytes->begin(), bytes->end());
    auto hash = artwork_sha256(checksum_input, [&] { audit.poll(); return false; });
    require(hash == expected.sha256, F::Integrity, "SVG bytes differ from manifest hash");
    parse(audit, *bytes); audit.analyze(); audit.poll();
    require(std::abs(audit.width_mm - expected.width_mm) <= limits.dimension_tolerance_mm &&
            std::abs(audit.height_mm - expected.height_mm) <= limits.dimension_tolerance_mm,
            F::DimensionMismatch, "Manifest dimensions disagree with SVG root viewport");
    std::vector<std::string> warnings;
    if (audit.text_bytes || !audit.fonts.empty()) warnings.emplace_back("Editable text/font descriptions retained; font availability and layout fidelity are not verified here.");
    if (audit.stats.filter_primitives) warnings.emplace_back("Native filter XML retained. Admission bounds filter inputs, not device-space surfaces; render callers must separately cap scale, surfaces and cache/work queues.");
    // No retained alias to a mutable string or State, even inside this factory.
    return ValidatedSvg(std::make_shared<ValidatedSvg::State const>(ValidatedSvg::State{
        std::move(bytes), std::move(expected), audit.width_mm, audit.height_mm, audit.viewbox, audit.stats, std::move(warnings),
        std::vector<std::string>(audit.fonts.begin(), audit.fonts.end())}));
}
} // namespace Inkscape::IO::ArtworkLibrary
