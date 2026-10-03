// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Bounded W5 resource policy over a `ParsedPreviewXml` DOM. See the header for
 * the contract. This translation unit reads only the const libxml2 tree and
 * calls the accepted CSS admission primitive; it contains no loader, no
 * selector/cascade engine, no geometry, no serializer, no native object
 * construction and no global-namespace mutation.
 */
#include "io/preview-svg-resource-policy.h"

#include <libxml/tree.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Inkscape::IO {

namespace {

using Status = PreviewResourcePolicyStatus;
using Reason = PreviewResourcePolicyReason;

constexpr char const *XML_NS_URI = "http://www.w3.org/XML/1998/namespace";

/// Internal control-flow exception; never escapes `admit_preview_resources`.
struct Abort {
    Status status;
    Reason reason;
};

[[noreturn]] void reject(Reason reason) {
    throw Abort{Status::Rejected, reason};
}
[[noreturn]] void unsupported(Reason reason) {
    throw Abort{Status::Unsupported, reason};
}
[[noreturn]] void cancelled_abort() {
    throw Abort{Status::Cancelled, Reason::Cancelled};
}

// ---------------------------------------------------------------------------
// Fixed namespace map: exactly `repr-util.cpp:72-131` + `repr.h:24-32`.
// The array capacity in native is 11; entries 0-6, 8, 9, 10 are populated
// (index 7 is commented out), so ten URIs are reproduced. Unknown URIs are
// never invented and the module never mutates this table or the native one.
// ---------------------------------------------------------------------------
struct NsEntry {
    char const *uri;
    char const *prefix;
};

NsEntry const NS_TABLE[] = {
    // repr-util.cpp:77-81
    {"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd", "sodipodi"},
    // repr-util.cpp:83-86
    {"http://www.w3.org/1999/xlink", "xlink"},
    // repr-util.cpp:88-91
    {"http://www.w3.org/2000/svg", "svg"},
    // repr-util.cpp:93-96
    {"http://www.inkscape.org/namespaces/inkscape", "inkscape"},
    // repr-util.cpp:98-101
    {"http://www.w3.org/1999/02/22-rdf-syntax-ns#", "rdf"},
    // repr-util.cpp:103-106
    {"http://creativecommons.org/ns#", "cc"},
    // repr-util.cpp:108-111
    {"http://purl.org/dc/elements/1.1/", "dc"},
    // repr-util.cpp:117-120 (broken sodipodi)
    {"http://inkscape.sourceforge.net/DTD/sodipodi-0.dtd", "sodipodi"},
    // repr-util.cpp:121 ("duck prion", literal really contains a space)
    {"http://inkscape.sourceforge.net/DTD/s odipodi-0.dtd", "sodipodi"},
    // repr-util.cpp:127-131 (old cc)
    {"http://web.resource.org/cc/", "cc"},
};

char const *prefix_for_uri(char const *uri) {
    if (!uri) {
        return nullptr;
    }
    for (auto const &e : NS_TABLE) {
        if (std::strcmp(e.uri, uri) == 0) {
            return e.prefix;
        }
    }
    // E1: the XML-reserved URI is proven by the bounded parser to be reachable
    // only through the standard `xml` prefix (see the report); admit only that
    // canonical spelling here without touching the process-global table.
    if (std::strcmp(uri, XML_NS_URI) == 0) {
        return "xml";
    }
    return nullptr;
}

std::string local_name(xmlChar const *name) {
    return name ? std::string(reinterpret_cast<char const *>(name)) : std::string{};
}

/// Canonical qualified name. `known == false` means an unknown namespace URI.
struct QName {
    bool known = false;
    std::string text;
};

QName canonical_name(xmlChar const *local, xmlNs const *ns) {
    if (!ns || !ns->href) {
        return {true, local_name(local)}; // unqualified
    }
    char const *prefix = prefix_for_uri(reinterpret_cast<char const *>(ns->href));
    if (!prefix) {
        return {false, {}};
    }
    return {true, std::string(prefix) + ":" + local_name(local)};
}

// ---------------------------------------------------------------------------
// Element sets derived from `sp-factory.cpp:139-282` and `:297-310`.
// ---------------------------------------------------------------------------
bool is_admitted_element(std::string const &n) {
    static std::unordered_set<std::string> const set = {
        // sp-factory.cpp:146-245
        "svg:svg",          "svg:g",            "svg:defs",       "svg:path",
        "svg:rect",         "svg:circle",       "svg:ellipse",    "svg:line",
        "svg:polyline",     "svg:polygon",      "svg:text",       "svg:tspan",
        "svg:textPath",     "svg:use",          "svg:symbol",     "svg:switch",
        "svg:a",            "svg:title",        "svg:desc",       "svg:metadata",
        "svg:style",        "svg:view",         "svg:linearGradient",
        "svg:radialGradient", "svg:stop",       "svg:pattern",    "svg:marker",
        "svg:clipPath",     "svg:mask",         "svg:filter",
        // filter primitives, sp-factory.cpp:249-273 (feImage excluded)
        "svg:feBlend",      "svg:feColorMatrix", "svg:feComponentTransfer",
        "svg:feFuncR",      "svg:feFuncG",      "svg:feFuncB",    "svg:feFuncA",
        "svg:feComposite",  "svg:feConvolveMatrix", "svg:feDiffuseLighting",
        "svg:feDisplacementMap", "svg:feDistantLight", "svg:feDropShadow",
        "svg:feFlood",      "svg:feGaussianBlur", "svg:feMerge",  "svg:feMergeNode",
        "svg:feMorphology", "svg:feOffset",     "svg:fePointLight",
        "svg:feSpecularLighting", "svg:feSpotLight", "svg:feTile",
        "svg:feTurbulence",
        // sp-factory.cpp:176,211,274
        "sodipodi:namedview", "sodipodi:guide", "inkscape:grid",
    };
    return set.count(n) != 0;
}

/// Whole-document Unsupported placeholders; the specific reason is returned.
/// Returns Reason::Ok when the element is not a known placeholder.
Reason placeholder_reason(std::string const &n) {
    if (n == "svg:image" || n == "svg:feImage") {
        return Reason::BitmapOrImageFeature;
    }
    if (n == "svg:color-profile") {
        return Reason::IccColorProfile;
    }
    if (n == "inkscape:path-effect") {
        return Reason::ActivePathEffect;
    }
    // Every other known factory row outside the admitted subset.
    static std::unordered_set<std::string> const set = {
        "svg:script",     "svg:font",         "svg:font-face",  "svg:glyph",
        "svg:hkern",      "svg:vkern",        "svg:missing-glyph",
        "svg:mesh",       "svg:meshGradient", "svg:meshgradient",
        "svg:meshPatch",  "svg:meshpatch",    "svg:meshRow",    "svg:meshrow",
        "svg:solidColor", "svg:solidcolor",   "svg:flowDiv",    "svg:flowSpan",
        "svg:flowPara",   "svg:flowLine",     "svg:flowRegion", "svg:flowRegionBreak",
        "svg:flowRegionExclude", "svg:flowRoot", "svg:hatch",   "svg:hatchpath",
        "svg:hatchPath",  "svg:tref",         "inkscape:tag",   "inkscape:tagref",
        "inkscape:offset", "inkscape:box3d",  "inkscape:box3dside",
        "inkscape:persp3d", "inkscape:clipboard", "inkscape:templateinfo",
        "inkscape:_templateinfo",
    };
    return set.count(n) ? Reason::UnsupportedElement : Reason::Ok;
}

/// Finite RDF/DC/CC metadata set; anything else in metadata is ForeignMetadata.
bool is_metadata_element(std::string const &n) {
    static std::unordered_set<std::string> const set = {
        "rdf:RDF",
        "dc:title",    "dc:creator",     "dc:subject",  "dc:description",
        "dc:publisher", "dc:contributor", "dc:date",    "dc:type",
        "dc:format",   "dc:identifier",  "dc:source",   "dc:language",
        "dc:relation", "dc:coverage",    "dc:rights",
        "cc:Work",     "cc:License",     "cc:license",  "cc:attributionName",
        "cc:attributionURL", "cc:morePermissions", "cc:useGuidelines",
        "cc:permits",  "cc:prohibits",   "cc:requires", "cc:jurisdiction",
        "cc:legalcode", "cc:deprecatedOn",
    };
    return set.count(n) != 0;
}

bool is_metadata_attr(std::string const &n) {
    static std::unordered_set<std::string> const set = {
        "id", "rdf:about", "rdf:resource", "rdf:datatype", "rdf:parseType",
        "rdf:ID", "rdf:nodeID",
    };
    return set.count(n) != 0;
}

/// Audited finite attribute set: the literal reads and `readAttr` keys that the
/// admitted classes can observe (`src/attributes.cpp`, `sp-namedview.cpp:68-132`,
/// `sp-grid.cpp:84-108`, `sp-guide.cpp:65-74`, `sp-page.cpp:39-87`). Anything
/// not here is Unsupported/UnknownAttribute; there is no blanket allow.
bool is_allowed_attr(std::string const &n) {
    static std::unordered_set<std::string> const set = {
        // structural / core
        "id", "style", "class", "type", "media", "title",
        // xml reserved (E1)
        "xml:space", "xml:lang",
        // href literal reads: href-attribute-helper.cpp:20-31, attributes.cpp:628
        "href", "xlink:href",
        // geometry / structure
        "d", "x", "y", "x1", "y1", "x2", "y2", "dx", "dy", "width", "height",
        "rx", "ry", "cx", "cy", "r", "points", "transform", "viewBox",
        "preserveAspectRatio", "version",
        // presentation / CSS
        "fill", "fill-opacity", "fill-rule", "stroke", "stroke-width",
        "stroke-linecap", "stroke-linejoin", "stroke-miterlimit",
        "stroke-dasharray", "stroke-dashoffset", "stroke-opacity", "opacity",
        "color", "display", "visibility", "overflow", "marker", "marker-start",
        "marker-mid", "marker-end", "filter", "clip-path", "clip-rule", "mask",
        "stop-color", "stop-opacity",
        "font-family", "font-size", "font-style", "font-weight", "font-variant",
        "text-anchor", "text-align", "text-decoration", "letter-spacing",
        "word-spacing", "dominant-baseline", "paint-order", "vector-effect",
        "mix-blend-mode", "isolation", "shape-inside", "shape-subtract",
        "enable-background", "color-interpolation", "color-interpolation-filters",
        "color-rendering", "shape-rendering", "text-rendering", "image-rendering",
        // text
        "rotate", "textLength", "lengthAdjust", "startOffset", "method",
        "spacing", "side",
        // gradients
        "gradientUnits", "gradientTransform", "spreadMethod", "fx", "fy", "fr",
        "offset",
        // patterns / markers
        "patternUnits", "patternContentUnits", "patternTransform", "markerUnits",
        "markerWidth", "markerHeight", "refX", "refY", "orient",
        // clips / masks / filters
        "clipPathUnits", "maskUnits", "maskContentUnits", "filterUnits",
        "primitiveUnits", "filterRes",
        "in", "in2", "result", "stdDeviation", "mode", "operator", "k1", "k2",
        "k3", "k4", "values", "tableValues", "slope", "intercept", "amplitude",
        "exponent", "scale", "xChannelSelector", "yChannelSelector", "targetX",
        "targetY", "order", "kernelMatrix", "divisor", "bias", "edgeMode",
        "radius", "baseFrequency", "numOctaves", "seed", "stitchTiles",
        "surfaceScale", "diffuseConstant", "specularConstant", "specularExponent",
        "azimuth", "elevation", "pointsAtX", "pointsAtY", "pointsAtZ",
        "limitingConeAngle", "flood-color", "flood-opacity", "lighting-color",
        "preserveAlpha",
        // misc SVG
        "baseProfile", "contentScriptType", "contentStyleType", "zoomAndPan",
        "externalResourcesRequired", "requiredFeatures", "requiredExtensions",
        "systemLanguage", "viewTarget",
        // inkscape / sodipodi common
        "inkscape:label", "inkscape:groupmode", "inkscape:collect",
        "inkscape:connector-curvature", "inkscape:connector-avoid",
        "inkscape:connection-points", "inkscape:highlight-color",
        "inkscape:spray-origin", "inkscape:transform-center-x",
        "inkscape:transform-center-y", "inkscape:version",
        "sodipodi:role", "sodipodi:nodetypes", "sodipodi:insensitive",
        "sodipodi:docname", "sodipodi:docbase",
        // namedview, sp-namedview.cpp:68-132 / xml-native-policy-proof:71-137
        "pagecolor", "bordercolor", "borderopacity", "showborder", "borderlayer",
        "labelstyle", "showgrid", "showguides", "lockguides", "viewonly",
        "clip-to-page-rendering", "antialias-rendering", "origin-correction",
        "y-axis-down", "gridtolerance", "guidetolerance", "objecttolerance",
        "alignmenttolerance", "distributiontolerance",
        "inkscape:pageopacity", "inkscape:pageshadow", "inkscape:showpageshadow",
        "inkscape:pagecheckerboard", "inkscape:deskcolor", "inkscape:document-units",
        "inkscape:zoom", "inkscape:rotation", "inkscape:cx", "inkscape:cy",
        "inkscape:window-width", "inkscape:window-height", "inkscape:window-x",
        "inkscape:window-y", "inkscape:window-maximized", "inkscape:current-layer",
        "inkscape:connector-spacing", "inkscape:lockguides",
        "inkscape:clip-to-page-rendering", "inkscape:antialias-rendering",
        "inkscape:origin-correction", "inkscape:y-axis-down",
        "guidecolor", "guideopacity", "guidehicolor", "guidehiopacity",
        // grid, sp-grid.cpp:84-108,147-260
        "units", "originx", "originy", "spacingx", "spacingy", "gridanglex",
        "gridanglez", "angleyvertical", "gapx", "gapy", "marginx", "marginy",
        "empcolor", "empopacity", "visible", "enabled", "empspacing", "dotted",
        "snapvisiblegridlinesonly",
        // guide, sp-guide.cpp:65-74,91-200
        "position", "orientation", "inkscape:color", "inkscape:locked",
        // page / view, sp-page.cpp:39-87
        "inkscape:margin", "inkscape:bleed", "inkscape:page-size",
    };
    return set.count(n) != 0;
}

/// Attributes whose value is a resource-capable presentation property and must
/// be routed through the accepted CSS primitive (attributes.cpp:454-491,543-544,
/// style-internal.cpp:1419-1436).
bool is_resource_capable_attr(std::string const &n) {
    return n == "fill" || n == "stroke" || n == "marker" || n == "marker-start" ||
           n == "marker-mid" || n == "marker-end" || n == "filter" ||
           n == "clip-path" || n == "mask" || n == "shape-inside" ||
           n == "shape-subtract";
}

/// Elements that are SPItem subclasses in the admitted subset; inherited
/// presentation refs conservatively propagate to these descendants.
bool is_supported_item(std::string const &n) {
    static std::unordered_set<std::string> const set = {
        "svg:g",    "svg:path", "svg:rect",  "svg:circle", "svg:ellipse",
        "svg:line", "svg:polyline", "svg:polygon", "svg:text", "svg:tspan",
        "svg:textPath", "svg:use", "svg:switch", "svg:symbol", "svg:a",
    };
    return set.count(n) != 0;
}

bool is_href_consumer(std::string const &n) {
    return n == "svg:use" || n == "svg:linearGradient" || n == "svg:radialGradient" ||
           n == "svg:pattern" || n == "svg:marker" || n == "svg:clipPath" ||
           n == "svg:mask" || n == "svg:filter";
}

/// Exact simple local `#id`; no URI-unescape guesses. Encoded, external,
/// relative, query, nested or otherwise ambiguous fragments are refused.
bool is_simple_local_fragment(std::string const &v) {
    if (v.size() < 2 || v[0] != '#') {
        return false;
    }
    for (std::size_t i = 1; i < v.size(); ++i) {
        unsigned char const c = static_cast<unsigned char>(v[i]);
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
        case '%':
        case '<':
        case '>':
            return false;
        default:
            break;
        }
    }
    return true;
}

struct Budget {
    PreviewResourcePolicyLimits limits;
    Cancelled const &cancelled;
    std::size_t nodes = 0;
    std::size_t attributes = 0;
    std::size_t references = 0;
    std::size_t work = 0;            ///< conservative expanded pre-charge
    std::size_t validation_work = 0; ///< unique construction/traversal work

    void poll() const {
        if (cancelled && cancelled()) {
            cancelled_abort();
        }
    }
    void node() {
        if (nodes >= limits.max_nodes) {
            reject(Reason::NodeBudget);
        }
        ++nodes;
    }
    void attr() {
        if (attributes >= limits.max_attributes) {
            reject(Reason::AttributeBudget);
        }
        ++attributes;
    }
    void ref() {
        if (references >= limits.max_references) {
            reject(Reason::ReferenceBudget);
        }
        ++references;
    }
    /// Conservative pre-charge before an edge append: for a root-reachable DAG
    /// `expanded[root]` is at least the reachable edge count, so a graph whose
    /// unique edges alone exceed the expanded budget is stopped before its
    /// adjacency is built. The comparison is exact size_t, no overflow.
    void charge() {
        if (work >= limits.max_expanded_work) {
            reject(Reason::ExpandedWorkBudget);
        }
        ++work;
    }
    /// Unique validation accounting, charged before each unit of graph work.
    /// Rejects with `ValidationWorkBudget` rather than saturating, so inherited
    /// non-`SPItem` traversal cannot outrun the limit before any `add_edge`.
    void touch() {
        if (validation_work >= limits.max_validation_work) {
            reject(Reason::ValidationWorkBudget);
        }
        ++validation_work;
    }
};

std::size_t sat_add(std::size_t a, std::size_t b) {
    std::size_t const max = std::numeric_limits<std::size_t>::max();
    if (b > max - a) {
        return max;
    }
    return a + b;
}

/// Saturating add that independently reports whether the true sum overflowed
/// `size_t`. Used only for the expanded-work DP, where a saturated accumulator
/// must never be mistaken for a value within the configured ceiling.
bool sat_add_overflow(std::size_t a, std::size_t b, std::size_t &out) {
    std::size_t const max = std::numeric_limits<std::size_t>::max();
    if (b > max - a) {
        out = max;
        return true;
    }
    out = a + b;
    return false;
}

/// Aggregate CSS budget: byte/term/reference totals persist across every style
/// value in the document; nothing resets per attribute.
struct CssAggregate {
    PreviewResourcePolicyLimits const *limits;
    std::size_t bytes = 0;
    std::size_t terms = 0;
    std::size_t refs = 0;

    void check_bytes(std::size_t n) const {
        if (bytes >= limits->css.max_bytes || n > limits->css.max_bytes - bytes) {
            reject(Reason::CssRejected);
        }
    }
    CssLimits remaining() const {
        CssLimits l = limits->css;
        l.max_bytes = limits->css.max_bytes - bytes;
        l.max_terms = terms >= limits->css.max_terms ? 0 : limits->css.max_terms - terms;
        l.max_references =
            refs >= limits->css.max_references ? 0 : limits->css.max_references - refs;
        return l;
    }
    void account(std::size_t n, CssAdmissionResult const &r) {
        bytes += n;
        terms += r.term_count;
        refs += r.reference_count;
    }
};

struct Node {
    int parent = -1;
    std::string name;
    std::string id;
    bool is_item = false;
    std::vector<int> children;
    std::vector<std::string> inline_refs;
    std::vector<std::string> href_refs;
};

// ---------------------------------------------------------------------------
// DOM walk (pass A): classification, budgets, id collection, ref collection.
// ---------------------------------------------------------------------------
struct Walk {
    Budget &budget;
    CssAggregate &css;
    std::vector<Node> nodes;
    std::unordered_map<std::string, int> ids;
    bool use_present = false;
    bool local_inline_resource = false;

    void route_inline(Node &node, std::string const &decl) {
        css.check_bytes(decl.size());
        CssLimits const rem = css.remaining();
        CssAdmissionResult const r = admit_inline_declarations(decl, rem);
        css.account(decl.size(), r);
        switch (r.status) {
        case CssAdmissionStatus::Accepted:
            for (auto const &f : r.local_fragment_ids) {
                budget.ref();
                node.inline_refs.push_back(f);
            }
            if (!r.local_fragment_ids.empty()) {
                local_inline_resource = true;
            }
            return;
        case CssAdmissionStatus::Unsupported:
            unsupported(Reason::CssUnsupported);
        case CssAdmissionStatus::Rejected:
            reject(Reason::CssRejected);
        }
    }

    void route_stylesheet(std::string const &body) {
        // No stylesheet local references in this first subset.
        css.check_bytes(body.size());
        CssLimits const rem = css.remaining();
        CssAdmissionResult const r = admit_stylesheet(body, rem);
        css.account(body.size(), r);
        switch (r.status) {
        case CssAdmissionStatus::Accepted:
            if (!r.local_fragment_ids.empty()) {
                unsupported(Reason::UnsupportedStylesheetReferences);
            }
            return;
        case CssAdmissionStatus::Unsupported:
            unsupported(Reason::CssUnsupported);
        case CssAdmissionStatus::Rejected:
            reject(Reason::CssRejected);
        }
    }

    void handle_href(Node &node, std::string const &value) {
        if (node.name == "svg:a") {
            unsupported(Reason::UnsupportedElement);
        }
        if (!is_href_consumer(node.name)) {
            reject(Reason::AmbiguousReference);
        }
        if (!is_simple_local_fragment(value)) {
            reject(Reason::AmbiguousReference);
        }
        budget.ref();
        node.href_refs.push_back(value);
    }

    /// Gather a `<style>` body from text/CDATA children with a hard byte cap.
    std::string style_body(xmlNode const *n) {
        std::string body;
        std::size_t const cap = css.limits->css.max_bytes;
        for (xmlNode const *c = n->children; c; c = c->next) {
            if (c->type != XML_TEXT_NODE && c->type != XML_CDATA_SECTION_NODE) {
                continue;
            }
            char const *content = reinterpret_cast<char const *>(c->content);
            std::size_t const len = content ? std::strlen(content) : 0u;
            if (body.size() > cap || len > cap - std::min(body.size(), cap)) {
                reject(Reason::CssRejected);
            }
            body.append(content, len);
        }
        return body;
    }

    void classify_attrs(Node &node, int idx, xmlNode const *n, bool in_metadata) {
        for (xmlAttr const *a = n->properties; a; a = a->next) {
            budget.attr();
            QName const q = canonical_name(a->name, a->ns);
            if (!q.known) {
                unsupported(Reason::UnknownNamespace);
            }
            if (q.text.size() >= budget.limits.max_name_bytes) {
                reject(Reason::NameBytesBudget);
            }
            if (q.text == "sodipodi:type") {
                unsupported(Reason::UnknownDispatch);
            }
            if (q.text == "inkscape:path-effect") {
                unsupported(Reason::ActivePathEffect);
            }
            if (q.text == "xml:base") {
                unsupported(Reason::XmlNamespaceFeatureUnsupported);
            }
            if (q.text.rfind("xml:", 0) == 0 && q.text != "xml:space" && q.text != "xml:lang") {
                unsupported(Reason::XmlNamespaceFeatureUnsupported);
            }
            if (q.text == "id") {
                xmlChar *v = xmlNodeListGetString(n->doc, a->children, 1);
                std::string value = v ? reinterpret_cast<char const *>(v) : std::string{};
                if (v) {
                    xmlFree(v);
                }
                auto it = ids.find(value);
                if (it != ids.end()) {
                    reject(Reason::DuplicateId);
                }
                ids.emplace(value, idx);
                node.id = std::move(value);
                continue;
            }
            if (in_metadata) {
                if (!is_metadata_attr(q.text)) {
                    unsupported(Reason::ForeignMetadata);
                }
                continue;
            }
            if (!is_allowed_attr(q.text)) {
                unsupported(Reason::UnknownAttribute);
            }
            // `sodipodi:type` cannot reach here; it was refused above.
            if (q.text == "href" || q.text == "xlink:href") {
                xmlChar *v = xmlNodeListGetString(n->doc, a->children, 1);
                std::string value = v ? reinterpret_cast<char const *>(v) : std::string{};
                if (v) {
                    xmlFree(v);
                }
                handle_href(node, value);
                continue;
            }
            if (q.text == "style") {
                xmlChar *v = xmlNodeListGetString(n->doc, a->children, 1);
                std::string value = v ? reinterpret_cast<char const *>(v) : std::string{};
                if (v) {
                    xmlFree(v);
                }
                route_inline(node, value);
                continue;
            }
            if (is_resource_capable_attr(q.text)) {
                xmlChar *v = xmlNodeListGetString(n->doc, a->children, 1);
                std::string value = v ? reinterpret_cast<char const *>(v) : std::string{};
                if (v) {
                    xmlFree(v);
                }
                route_inline(node, q.text + ":" + value);
                continue;
            }
        }
    }

    void run(xmlNode const *root) {
        struct Frame {
            xmlNode const *node;
            int parent;
            bool in_metadata;
        };
        std::vector<Frame> stack;
        stack.push_back({root, -1, false});

        while (!stack.empty()) {
            budget.poll();
            Frame const frame = stack.back();
            stack.pop_back();

            xmlNode const *n = frame.node;
            if (n->type != XML_ELEMENT_NODE) {
                continue;
            }
            budget.node();

            QName const eq = canonical_name(n->name, n->ns);
            if (!eq.known) {
                unsupported(Reason::UnknownNamespace);
            }
            if (!n->ns || !n->ns->href) {
                unsupported(Reason::NamespaceRepair);
            }
            if (eq.text.size() >= budget.limits.max_name_bytes) {
                reject(Reason::NameBytesBudget);
            }

            bool const in_metadata = frame.in_metadata || eq.text == "svg:metadata";
            if (in_metadata) {
                if (!is_metadata_element(eq.text) && eq.text != "svg:metadata") {
                    unsupported(Reason::ForeignMetadata);
                }
            } else {
                Reason const ph = placeholder_reason(eq.text);
                if (ph != Reason::Ok) {
                    unsupported(ph);
                } else if (!is_admitted_element(eq.text)) {
                    unsupported(Reason::UnknownElement);
                }
            }

            int const idx = static_cast<int>(nodes.size());
            nodes.push_back(Node{});
            Node &node = nodes.back();
            node.parent = frame.parent;
            node.name = eq.text;
            node.is_item = is_supported_item(eq.text);
            if (frame.parent >= 0) {
                nodes[frame.parent].children.push_back(idx);
            }
            if (eq.text == "svg:use") {
                use_present = true;
            }

            classify_attrs(node, idx, n, in_metadata);

            if (eq.text == "svg:style" && !in_metadata) {
                route_stylesheet(style_body(n));
            }

            // Push children in document order (reverse for the LIFO stack).
            std::vector<xmlNode const *> kids;
            for (xmlNode const *c = n->children; c; c = c->next) {
                if (c->type == XML_ELEMENT_NODE) {
                    // Enforce the already-checked finite node bound before the
                    // append so the per-node child queue cannot grow unbounded.
                    if (kids.size() >= budget.limits.max_nodes) {
                        reject(Reason::NodeBudget);
                    }
                    kids.push_back(c);
                }
            }
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                stack.push_back({*it, idx, in_metadata});
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Graph (pass B/C): resolution, bounded adjacency construction, iterative
// cycle detection + topological postorder, and saturating dynamic programming
// for the longest reference depth and the expanded work estimate.
// ---------------------------------------------------------------------------
struct Edge {
    int target;
    bool ref;
};

void resolve_and_walk(Budget &budget, std::vector<Node> const &nodes,
                      std::unordered_map<std::string, int> const &ids,
                      PreviewResourcePolicyLimits const &limits,
                      PreviewResourcePolicyResult &result, bool use_present) {
    // Resolve every collected fragment; unresolved ids are refused. Both lists
    // are sized by the already-bounded node count, and the total number of
    // resolved targets is bounded by `max_references` (charged at collection).
    std::vector<std::vector<int>> inline_targets(nodes.size());
    std::vector<std::vector<int>> href_targets(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        for (auto const &f : nodes[i].inline_refs) {
            // Charge the validation step before resolving/looking up.
            budget.touch();
            auto const it = ids.find(f.substr(1));
            if (it == ids.end()) {
                reject(Reason::UnresolvedReference);
            }
            // Enforce the already-checked finite fanout bound before the append
            // (the global reference bound was enforced at collection).
            if (inline_targets[i].size() >= limits.max_fanout) {
                reject(Reason::ReferenceFanoutBudget);
            }
            inline_targets[i].push_back(it->second);
        }
        for (auto const &f : nodes[i].href_refs) {
            budget.touch();
            auto const it = ids.find(f.substr(1));
            if (it == ids.end()) {
                reject(Reason::UnresolvedReference);
            }
            if (href_targets[i].size() >= limits.max_fanout) {
                reject(Reason::ReferenceFanoutBudget);
            }
            href_targets[i].push_back(it->second);
        }
    }

    // Adjacency: containment + direct local refs + conservative inherited refs.
    // Sized by the bounded node count.
    std::vector<std::vector<Edge>> adj(nodes.size());
    auto add_edge = [&](int u, int v, bool is_ref) {
        // The conservative expanded pre-charge and the fanout bound are both
        // enforced before the append, so no adjacency growth is unaccounted.
        budget.charge();
        if (adj[u].size() >= limits.max_fanout) {
            reject(Reason::ReferenceFanoutBudget);
        }
        // Charge the unique validation step before the append.
        budget.touch();
        adj[u].push_back(Edge{v, is_ref});
    };

    // Containment edges.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        for (int child : nodes[i].children) {
            add_edge(static_cast<int>(i), child, false);
        }
    }
    // Direct reference edges.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        for (int t : href_targets[i]) {
            add_edge(static_cast<int>(i), t, true);
        }
        for (int t : inline_targets[i]) {
            add_edge(static_cast<int>(i), t, true);
        }
    }
    // Conservative inherited propagation for ordinary inline/presentation refs
    // (only when no `use` is present; a use+local-resource document is refused
    // earlier). A single reusable frontier is walked; every visited descendant
    // is charged before use and every append is bounded by the node count, so
    // no inherited-ref set is copied per descendant.
    if (!use_present) {
        std::vector<int> frontier;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (inline_targets[i].empty()) {
                continue;
            }
            // The node count is the already-checked finite bound on the
            // frontier of a DOM subtree, so this copy cannot grow unbounded.
            if (nodes[i].children.size() > nodes.size()) {
                reject(Reason::ReferenceFanoutBudget);
            }
            frontier.assign(nodes[i].children.begin(), nodes[i].children.end());
            while (!frontier.empty()) {
                budget.poll();
                // Charge the descendant visit before it is processed, so the
                // inherited non-`SPItem` walk is bounded by `max_validation_work`
                // and cannot run to completion before any `add_edge`.
                budget.touch();
                int const d = frontier.back();
                frontier.pop_back();
                if (nodes[d].is_item) {
                    for (int t : inline_targets[i]) {
                        add_edge(d, t, true);
                    }
                }
                for (int c : nodes[d].children) {
                    // Enforce the already-checked finite node bound before the
                    // descendant append. A DOM tree cannot overflow it.
                    if (frontier.size() >= nodes.size()) {
                        reject(Reason::ReferenceFanoutBudget);
                    }
                    frontier.push_back(c);
                }
            }
        }
    }

    // Iterative colour DFS: a gray target is a cycle; finish order is a
    // topological postorder. No recursion and no per-node path copy.
    std::vector<unsigned char> color(nodes.size(), 0);
    std::vector<std::size_t> next(nodes.size(), 0);
    std::vector<int> order;
    order.reserve(nodes.size());
    std::vector<int> stack;
    stack.reserve(nodes.size());
    for (std::size_t s = 0; s < nodes.size(); ++s) {
        if (color[s] != 0) {
            continue;
        }
        budget.poll();
        color[s] = 1;
        stack.push_back(static_cast<int>(s));
        while (!stack.empty()) {
            budget.poll();
            int const v = stack.back();
            if (next[v] < adj[v].size()) {
                // Charge the DFS step before consuming the edge.
                budget.touch();
                Edge const e = adj[v][next[v]++];
                if (color[e.target] == 1) {
                    reject(Reason::ReferenceCycle);
                }
                if (color[e.target] == 0) {
                    color[e.target] = 1;
                    stack.push_back(e.target);
                }
            } else {
                color[v] = 2;
                order.push_back(v);
                stack.pop_back();
            }
        }
    }

    // Longest reference depth over the whole graph. Postorder forward: every
    // target is already computed. Duplicate edges do not change a maximum; the
    // result is independent of DOM/declaration/attribute order. Depth is bounded
    // by the parser's finite node count (<= 20000, one per `ref` edge on a simple
    // path), so this `sat_add` cannot overflow `size_t`; no overflow flag is
    // needed here (unlike the clone-count expansion below).
    std::vector<std::size_t> depth(nodes.size(), 0);
    for (int const v : order) {
        budget.touch();
        std::size_t d = 0;
        for (Edge const &e : adj[v]) {
            budget.touch();
            std::size_t const cd = sat_add(depth[e.target], e.ref ? 1u : 0u);
            if (cd > d) {
                d = cd;
            }
        }
        depth[v] = d;
        if (d > result.max_reference_depth) {
            result.max_reference_depth = d;
        }
        if (d > limits.max_reference_depth) {
            reject(Reason::ReferenceDepth);
        }
    }

    // Expanded work: each node contributes itself plus every outgoing edge's
    // target estimate; duplicate edges count separately. The root reaches every
    // node by containment, so `expanded[0]` is the whole-document upper bound and
    // includes defs/resources (a conservative bound, not exact native work).
    // `expanded_overflow` records that some true sum exceeds `size_t`: the stored
    // `expanded_work` is then only a saturated lower bound and the decision must
    // be a rejection even when `max_expanded_work == SIZE_MAX`, because the true
    // expansion is larger than every representable limit.
    std::vector<std::size_t> expanded(nodes.size(), 0);
    bool expanded_overflow = false;
    for (int const v : order) {
        budget.touch();
        std::size_t w = 1;
        for (Edge const &e : adj[v]) {
            budget.touch();
            std::size_t next_w = 0;
            if (sat_add_overflow(w, expanded[e.target], next_w)) {
                expanded_overflow = true;
            }
            w = next_w;
        }
        expanded[v] = w;
    }
    result.expanded_work = expanded.empty() ? 0 : expanded[0];
    if (expanded_overflow) {
        // Disclose the saturated lower bound in `result.expanded_work` (already
        // assigned) and refuse, independent of the configured ceiling.
        reject(Reason::ExpandedWorkBudget);
    }
    if (result.expanded_work > limits.max_expanded_work) {
        reject(Reason::ExpandedWorkBudget);
    }
}

} // namespace

PreviewResourcePolicyResult admit_preview_resources(ParsedPreviewXml const &parsed,
                                                    PreviewResourcePolicyLimits const &limits,
                                                    Cancelled cancelled) {
    PreviewResourcePolicyResult result;
    result.original_bytes = parsed.original_bytes();

    if (!parsed.document() || !parsed.root()) {
        result.status = Status::Rejected;
        result.reason = Reason::NotParsed;
        return result;
    }

    Budget budget{limits, cancelled};
    CssAggregate css{&limits};
    Walk walk{budget, css, {}, {}, false, false};
    result.max_reference_depth = 0;

    try {
        // Cancellation is checked before any budget, walk or terminal decision.
        budget.poll();

        // Enforce the policy's tighter node/attribute bounds before any sized
        // vector or traversal queue is allocated, using the parser's already
        // finite counts. Conservatively this counts all parser nodes (elements
        // plus text/CDATA/comment callbacks), not only elements.
        if (parsed.node_count() > limits.max_nodes) {
            reject(Reason::NodeBudget);
        }
        if (parsed.attribute_count() > limits.max_attributes) {
            reject(Reason::AttributeBudget);
        }
        walk.nodes.reserve(parsed.node_count());

        walk.run(parsed.root());

        // Root resolves E2: a document that mixes any `use` with any local
        // inline/presentation resource URL is deferred whole-document rather
        // than running an unproven context-expansion algorithm.
        if (walk.use_present && walk.local_inline_resource) {
            unsupported(Reason::UnsupportedUseResourceInheritance);
        }

        budget.poll();
        resolve_and_walk(budget, walk.nodes, walk.ids, limits, result, walk.use_present);

        // Cancellation is re-checked before the terminal Candidate decision.
        budget.poll();
        result.status = Status::Candidate;
        result.reason = Reason::Ok;
    } catch (Abort const &a) {
        result.status = a.status;
        result.reason = a.reason;
    } catch (...) {
        result.status = Status::Rejected;
        result.reason = Reason::InternalFailure;
    }

    result.nodes = budget.nodes;
    result.attributes = budget.attributes;
    result.references = budget.references;
    result.validation_work = budget.validation_work;
    return result;
}

std::string_view to_string(PreviewResourcePolicyStatus status) {
    switch (status) {
    case PreviewResourcePolicyStatus::Candidate:
        return "Candidate";
    case PreviewResourcePolicyStatus::Unsupported:
        return "Unsupported";
    case PreviewResourcePolicyStatus::Rejected:
        return "Rejected";
    case PreviewResourcePolicyStatus::Cancelled:
        return "Cancelled";
    }
    return "Rejected";
}

std::string_view to_string(PreviewResourcePolicyReason reason) {
    switch (reason) {
    case Reason::Ok:
        return "Ok";
    case Reason::NotParsed:
        return "NotParsed";
    case Reason::NodeBudget:
        return "NodeBudget";
    case Reason::AttributeBudget:
        return "AttributeBudget";
    case Reason::NameBytesBudget:
        return "NameBytesBudget";
    case Reason::DuplicateId:
        return "DuplicateId";
    case Reason::UnknownNamespace:
        return "UnknownNamespace";
    case Reason::NamespaceRepair:
        return "NamespaceRepair";
    case Reason::UnknownElement:
        return "UnknownElement";
    case Reason::UnknownAttribute:
        return "UnknownAttribute";
    case Reason::UnknownDispatch:
        return "UnknownDispatch";
    case Reason::ForeignMetadata:
        return "ForeignMetadata";
    case Reason::XmlNamespaceFeatureUnsupported:
        return "XmlNamespaceFeatureUnsupported";
    case Reason::CssRejected:
        return "CssRejected";
    case Reason::CssUnsupported:
        return "CssUnsupported";
    case Reason::AmbiguousReference:
        return "AmbiguousReference";
    case Reason::UnresolvedReference:
        return "UnresolvedReference";
    case Reason::ReferenceBudget:
        return "ReferenceBudget";
    case Reason::ReferenceDepth:
        return "ReferenceDepth";
    case Reason::ReferenceCycle:
        return "ReferenceCycle";
    case Reason::ReferenceFanoutBudget:
        return "ReferenceFanoutBudget";
    case Reason::ExpandedWorkBudget:
        return "ExpandedWorkBudget";
    case Reason::ValidationWorkBudget:
        return "ValidationWorkBudget";
    case Reason::UnsupportedUseResourceInheritance:
        return "UnsupportedUseResourceInheritance";
    case Reason::UnsupportedStylesheetReferences:
        return "UnsupportedStylesheetReferences";
    case Reason::BitmapOrImageFeature:
        return "BitmapOrImageFeature";
    case Reason::IccColorProfile:
        return "IccColorProfile";
    case Reason::ActivePathEffect:
        return "ActivePathEffect";
    case Reason::UnsupportedElement:
        return "UnsupportedElement";
    case Reason::Cancelled:
        return "Cancelled";
    case Reason::InternalFailure:
        return "InternalFailure";
    }
    return "InternalFailure";
}

} // namespace Inkscape::IO
