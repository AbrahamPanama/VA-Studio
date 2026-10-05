// SPDX-License-Identifier: GPL-2.0-or-later
#include "actions-vacards-query.h"
#include "actions-vacards-cli.h"
#include "vacards-cli-dispatch.h"
#include "io/vacards-cli-intake.h"
#include "bitmap-adjustment-chemistry.h"
#include "display/cairo-utils.h"
#include "libnrtype/font-factory.h"
#include "object/sp-image.h"
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "object/sp-text.h"
#include "object/sp-flowtext.h"
#include "page-manager.h"
#include "selection.h"
#include "style.h"
#include "svg/svg.h"
#include "util/units.h"
#include "3rdparty/libcroco/src/cr-tknzr.h"
#include "3rdparty/libcroco/src/cr-token.h"
#include "xml/repr.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <utility>
#include <set>
#include <glib.h>

namespace Inkscape::VACardsCli {
namespace {
using namespace boost::json;
object scalar(char const *type) { return {{"type", type}}; }
object closed(object properties, array required = {})
{
    return {{"type", "object"}, {"properties", std::move(properties)},
            {"required", std::move(required)}, {"additionalProperties", false}};
}
object nullable(object rule) { return {{"anyOf", array{rule, scalar("null")}}}; }
object length_schema()
{
    return closed({{"px", scalar("number")}, {"preferred", closed({{"value", scalar("number")},
        {"unit", {{"enum", {"px", "mm", "cm", "in", "pt", "pc"}}}}}, {"value", "unit"})}}, {"px", "preferred"});
}
object affine_schema()
{
    return {{"type", "array"}, {"items", scalar("number")}, {"minItems", 6}, {"maxItems", 6}};
}
object rect_schema()
{
    return closed({{"kind", {{"enum", {"geometric", "visual"}}}}, {"x", length_schema()},
        {"y", length_schema()}, {"width", length_schema()}, {"height", length_schema()}},
        {"kind", "x", "y", "width", "height"});
}
object page_schema(object row)
{
    return closed({{"items", {{"type", "array"}, {"items", row}, {"maxItems", 1000}}},
        {"total", {{"type", "integer"}, {"minimum", 0}}}, {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
        {"next_cursor", nullable(scalar("string"))}, {"document_id", scalar("string")},
        {"revision", {{"type", "integer"}, {"minimum", 0}}}},
        {"items", "total", "limit", "next_cursor", "document_id", "revision"});
}
array affine(Geom::Affine const &a)
{
    array out; for (unsigned i = 0; i < 6; ++i) out.push_back(a[i]); return out;
}
value rectangle(Geom::OptRect const &r, std::string_view unit, char const *kind)
{
    if (!r) return nullptr;
    if (!std::isfinite(r->left()) || !std::isfinite(r->top()) ||
        !std::isfinite(r->width()) || !std::isfinite(r->height())) return nullptr;
    return object{{"kind", kind}, {"x", report_length(r->left(), unit)}, {"y", report_length(r->top(), unit)},
        {"width", report_length(r->width(), unit)}, {"height", report_length(r->height(), unit)}};
}
std::string id(SPObject const *o) { return o && o->getId() ? o->getId() : ""; }
std::string type(SPObject const *o)
{
    std::string s = o->getRepr()->name(); auto colon = s.find(':'); return colon == s.npos ? s : s.substr(colon + 1);
}
std::string label(SPObject const *o)
{
    auto s = o->getRepr()->attribute("inkscape:label"); return s ? s : "";
}
// Walk repr-backed objects only: clone shadow children must never masquerade as independent IDs.
std::vector<SPObject *> objects(SPDocument *doc)
{
    std::vector<SPObject *> out;
    std::function<void(XML::Node *)> walk = [&](XML::Node *n) {
        if (auto o = doc->getObjectByRepr(n)) out.push_back(o);
        for (auto c = n->firstChild(); c; c = c->next()) walk(c);
    };
    walk(doc->getReprRoot()); return out;
}
bool visible(SPObject const *o)
{
    // Computed visibility already includes inheritance and descendant overrides.
    if (o && o->style && o->style->visibility.computed != SP_CSS_VISIBILITY_VISIBLE) return false;
    for (; o; o = o->parent) if (auto item = cast<SPItem>(o))
        if (item->isHidden()) return false;
    return true;
}
bool locked(SPObject const *o)
{
    for (; o; o = o->parent) if (auto item = cast<SPItem>(o); item && item->isLocked()) return true;
    return false;
}
SPObject const *layer(SPObject const *o)
{
    for (; o; o = o->parent) if (SP_IS_LAYER(o)) return o;
    return nullptr;
}
unsigned depth(SPObject const *o) { unsigned n = 0; while ((o = o->parent)) ++n; return n; }
void reject(ActionContext &c, std::string code, std::string message)
{
    c.record.status = Status::Rejected; c.record.reason = code; c.record.message = message;
    c.record.error = ParseError{std::move(code), {}, std::move(message)};
}
void check_response(ActionContext &c)
{
    // Reserve 64 KiB for the result envelope, normalized parameters and request ID.
    if (serialize(c.record.data).size() + serialize(c.record.normalized_params).size() > 8388608 - 65536) {
        c.record.data.clear();
        reject(c, "engine-limit", "Response exceeds the 8 MiB ceiling; reduce limit or request a narrower projection.");
    }
}
bool targets(ActionContext &c, std::vector<SPObject *> &out, bool items_only = false)
{
    if (c.params.has("ids")) {
        std::set<std::string> seen;
        for (auto const &name : c.params.at("ids").items) {
            if (!seen.insert(name).second) { reject(c, "duplicate-id", "IDs must be unique; remove duplicates and retry."); return false; }
            auto o = c.document->getObjectById(name.c_str());
            if (!o || !o->getRepr()) { reject(c, "unknown-id", "Unknown object ID '" + name + "'; refresh query.objects and retry."); return false; }
            out.push_back(o);
        }
    } else {
        for (auto o : objects(c.document))
            if (!items_only || (cast<SPItem>(o) && o != c.document->getRoot())) out.push_back(o);
    }
    return true;
}
std::string cursor_prefix(ActionContext &c)
{
    auto stamp = document_stamp(c.document);
    // Bind command, projections, filters and unit policy as well as incarnation/revision.
    auto parameters = c.record.normalized_params;
    parameters.erase("cursor"); parameters.erase("limit");
    object signature{{"command", c.record.action}, {"params", parameters}, {"unit", c.record.preferred_unit}};
    return stamp.id + ":" + std::to_string(stamp.revision) + ":" + catalog_hash(signature) + ":";
}
void paginate(ActionContext &c, array const &rows)
{
    auto limit = static_cast<std::size_t>(c.params.at("limit").integer);
    std::size_t start = 0;
    auto prefix = cursor_prefix(c);
    if (c.params.has("cursor")) {
        auto const &cursor = c.params.at("cursor").text;
        if (!cursor.starts_with(prefix)) {
            reject(c, "stale-cursor", "Cursor belongs to another document, revision or query; restart this query without a cursor."); return;
        }
        auto suffix = std::string_view(cursor).substr(prefix.size());
        auto [end, ec] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), start);
        if (ec != std::errc{} || end != suffix.data() + suffix.size() || !start || start >= rows.size()) {
            reject(c, "invalid-cursor", "Cursor offset is invalid; restart this query without a cursor."); return;
        }
    }
    auto stop = std::min(rows.size(), start + limit);
    array page; for (auto i = start; i < stop; ++i) page.push_back(rows[i]);
    auto stamp = document_stamp(c.document);
    c.record.data = {{"items", std::move(page)}, {"total", rows.size()}, {"limit", limit},
        {"next_cursor", stop < rows.size() ? value(prefix + std::to_string(stop)) : value(nullptr)},
        {"document_id", stamp.id}, {"revision", stamp.revision}};
    check_response(c);
}
void document_query(ActionContext &c)
{
    auto root = c.document->getRoot(); auto stamp = document_stamp(c.document);
    object counts;
    for (auto o : objects(c.document)) {
        auto key = type(o); auto p = counts.if_contains(key);
        counts[key] = p ? p->to_number<std::uint64_t>() + 1 : 1;
    }
    auto const &a = root->c2p;
    object units{{"preferred", c.record.preferred_unit}};
    units["root_user_unit"] = object{{"name", "user"},
        {"x", report_length(std::hypot(a[0], a[1]), c.record.preferred_unit)},
        {"y", report_length(std::hypot(a[2], a[3]), c.record.preferred_unit)}};
    units["viewbox_to_css_px"] = affine(a);
    c.record.data = {{"document_id", stamp.id}, {"revision", stamp.revision},
        {"dirty", c.document->isModifiedSinceSave()}, {"units", units},
        {"size", object{{"width", report_length(c.document->getWidth().value("px"), c.record.preferred_unit)},
                        {"height", report_length(c.document->getHeight().value("px"), c.record.preferred_unit)}}},
        {"object_count", objects(c.document).size()}, {"object_counts", counts}};
    if (auto report = inspection_report(c.document)) for (auto key : {"title", "metadata"})
        if (auto v = report->if_contains(key)) c.record.data[key] = *v;
    check_response(c);
}
void pages_query(ActionContext &c)
{
    array rows; std::size_t order = 0;
    for (auto page : c.document->getPageManager().getPages())
        rows.emplace_back(object{{"id", id(page)}, {"label", page->getLabel()}, {"order", order++},
            {"rect", rectangle(page->getDocumentRect(), c.record.preferred_unit, "geometric")}});
    if (rows.empty()) rows.emplace_back(object{{"id", id(c.document->getRoot())}, {"label", ""}, {"order", 0},
        {"rect", rectangle(Geom::Rect(Geom::Point(0, 0), Geom::Point(c.document->getWidth().value("px"),
                             c.document->getHeight().value("px"))), c.record.preferred_unit, "geometric")}});
    paginate(c, rows);
}
void layers_query(ActionContext &c)
{
    array rows;
    for (auto o : objects(c.document)) if (SP_IS_LAYER(o)) {
        auto parent = layer(o->parent); unsigned d = 0;
        for (auto p = parent; p; p = layer(p->parent)) ++d;
        rows.emplace_back(object{{"id", id(o)}, {"label", label(o)}, {"parent_id", parent ? value(id(parent)) : value(nullptr)},
            {"depth", d}, {"visible", visible(o)}, {"locked", locked(o)}});
    }
    paginate(c, rows);
}
void objects_query(ActionContext &c)
{
    std::vector<SPObject *> list; if (!targets(c, list)) return;
    SPObject *requested_layer = nullptr;
    if (c.params.has("layer")) {
        requested_layer = c.document->getObjectById(c.params.at("layer").text.c_str());
        if (!requested_layer || !SP_IS_LAYER(requested_layer)) {
            reject(c, "unknown-id", "Layer ID is unknown or is not a layer; refresh query.layers."); return;
        }
    }
    array rows;
    for (auto o : list) {
        if (c.params.has("type") && type(o) != c.params.at("type").text) continue;
        if (requested_layer) {
            auto p = o; while (p && p != requested_layer) p = p->parent;
            if (!p) continue;
        }
        if (c.params.has("visibility") && visible(o) != (c.params.at("visibility").text == "visible")) continue;
        if (c.params.has("lock") && locked(o) != (c.params.at("lock").text == "locked")) continue;
        auto item = cast<SPItem>(o);
        bool visual = c.params.at("bounds").text == "visual";
        auto box = item ? (visual ? item->documentVisualBounds() : item->documentGeometricBounds()) : Geom::OptRect{};
        rows.emplace_back(object{{"id", id(o)}, {"parent_id", o->parent ? value(id(o->parent)) : value(nullptr)},
            {"layer_id", layer(o) ? value(id(layer(o))) : value(nullptr)}, {"depth", depth(o)},
            {"type", type(o)}, {"label", label(o)}, {"visible", visible(o)}, {"locked", locked(o)},
            {"bounds", rectangle(box, c.record.preferred_unit, visual ? "visual" : "geometric")}});
    }
    paginate(c, rows);
}
void geometry_query(ActionContext &c)
{
    std::vector<SPObject *> list; if (!targets(c, list, true)) return;
    array rows;
    for (auto o : list) {
        auto item = cast<SPItem>(o);
        auto a = item ? item->i2doc_affine() : Geom::identity();
        value path = nullptr;
        if (type(o) == "path") if (auto shape = cast<SPShape>(o); shape && shape->curve()) {
            auto paths = *shape->curve(); paths *= a; path = sp_svg_write_path(paths);
        }
        rows.emplace_back(object{{"id", id(o)}, {"shape_kind", type(o)}, {"transform", affine(a)},
            {"path_data", path}, {"geometric_bounds", rectangle(item ? item->documentGeometricBounds() : Geom::OptRect{}, c.record.preferred_unit, "geometric")},
            {"visual_bounds", rectangle(item ? item->documentVisualBounds() : Geom::OptRect{}, c.record.preferred_unit, "visual")}});
    }
    paginate(c, rows);
}
constexpr std::string_view style_properties[] = {"fill", "stroke", "stroke-width", "opacity", "paint-order",
    "font-family", "font-style", "font-weight", "font-stretch", "font-size"};
std::string computed_text(SPObject *o, std::string const &key)
{
    if (!o || !o->style) return {};
    auto style = o->style;
    if (key == "font-family") return style->font_family.value() ? style->font_family.value() : "";
    if (key == "font-weight") return std::to_string(static_cast<int>(style->font_weight.computed));
    if (key == "font-style") {
        auto p = style->font_style; p.inherit = false; p.value = p.computed; return p.get_value().raw();
    }
    if (key == "font-stretch") {
        auto p = style->font_stretch; p.inherit = false; p.value = p.computed; return p.get_value().raw();
    }
    for (auto prop : style->properties()) if (prop->name() == key) {
        auto text = prop->get_value().raw();
        if (text == "inherit") return computed_text(o->parent, key);
        if (key == "paint-order" && text.empty()) return "normal";
        if (key == "stroke" && text.empty() && style->stroke.isNone()) return "none";
        if (text == "currentColor") return computed_text(o, "color");
        return text;
    }
    return {};
}
void styles_query(ActionContext &c)
{
    std::vector<SPObject *> list; if (!targets(c, list, true)) return;
    std::vector<std::string> properties;
    if (c.params.has("properties")) properties = c.params.at("properties").items;
    else for (auto p : style_properties) properties.emplace_back(p);
    array rows;
    for (auto o : list) {
        object projection;
        for (auto const &key : properties) {
            value result = nullptr;
            if (auto style = o->style) {
                double scale = c.document->getRoot()->c2p.descrim();
                if (key == "stroke-width") result = report_length(style->stroke_width.computed * scale, c.record.preferred_unit);
                else if (key == "font-size") result = report_length(style->font_size.computed * scale, c.record.preferred_unit);
                else if (key == "opacity") result = style->opacity.as_double();
                else result = computed_text(o, key);
            }
            projection[key] = result;
        }
        rows.emplace_back(object{{"id", id(o)}, {"properties", projection}});
    }
    paginate(c, rows);
}
void images_query(ActionContext &c)
{
    std::vector<SPObject *> list; if (!targets(c, list, true)) return;
    array rows;
    for (auto o : list) if (auto image = cast<SPImage>(o)) {
        std::string href = image->href ? image->href : "", state = href.starts_with("data:") ? "embedded" : "ungranted";
        bool embedded = href.starts_with("data:"); std::string mime;
        if (auto report = inspection_report(c.document)) for (auto const &v : report->at("resources").as_array()) {
            auto const &resource = v.as_object();
            if (resource.at("id").as_string() == id(o) && resource.at("role") == "image") {
                state = std::string(resource.at("state").as_string());
                embedded = state == "embedded";
                href = std::string(resource.at("href").as_string());
                if (auto m = resource.if_contains("mime")) mime = std::string(m->as_string());
                break;
            }
        }
        value pixels = nullptr, density = nullptr;
        if (!image->missing && image->pixbuf) {
            pixels = object{{"width", image->pixbuf->width()}, {"height", image->pixbuf->height()}};
            if (mime.empty()) { gsize size; image->pixbuf->getMimeData(size, mime); }
            if (auto a = image->pixelToDocumentAffine()) {
                double x = std::hypot((*a)[0], (*a)[1]), y = std::hypot((*a)[2], (*a)[3]);
                if (x > 0 && y > 0) density = object{{"x", 96 / x}, {"y", 96 / y}, {"unit", "dpi"}};
            }
        }
        auto tone = BitmapAdjustments::canonical_tone(image);
        object adjustment;
        for (auto key : {"inkscape:brightness", "inkscape:contrast", "inkscape:intensity", "inkscape:highlights", "inkscape:shadows", "inkscape:midtones", "inkscape:bitmap-adjustment"})
            if (auto s = o->getRepr()->attribute(key)) adjustment[key] = s;
        rows.emplace_back(object{{"id", id(o)}, {"source_kind", image->missing ? "missing" : embedded ? "embedded" : "linked"},
            {"href", href}, {"link_grant", state}, {"pixel_size", pixels}, {"density", density},
            {"mime", mime.empty() ? value(nullptr) : value(mime)},
            {"tone", object{{"managed", BitmapAdjustments::query_tone(image).has_value()},
                {"brightness", tone.brightness}, {"contrast", tone.contrast}, {"intensity", tone.intensity},
                {"highlights", tone.highlights}, {"shadows", tone.shadows}, {"midtones", tone.midtones}}},
            {"adjustment_attributes", adjustment}});
    }
    paginate(c, rows);
}
std::vector<std::string> font_families(std::string const &css)
{
    std::vector<std::string> out;
    std::string name;
    auto flush = [&] {
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (!name.empty()) out.push_back(std::exchange(name, {}));
    };
    auto t = cr_tknzr_new_from_buf(reinterpret_cast<guchar *>(const_cast<char *>(css.data())), css.size(), CR_UTF_8, FALSE);
    if (!t) return out;
    CRToken *token = nullptr;
    while (cr_tknzr_get_next_token(t, &token) == CR_OK && token) {
        if (token->type == STRING_TK || token->type == IDENT_TK) {
            if (token->u.str && token->u.str->stryng) name.append(token->u.str->stryng->str, token->u.str->stryng->len);
        } else if (token->type == DELIM_TK && token->u.unichar == ',') flush();
        else if (token->type == S_TK && !name.empty() && name.back() != ' ') name += ' ';
        cr_token_destroy(token); token = nullptr;
    }
    if (token) cr_token_destroy(token);
    flush(); cr_tknzr_unref(t); return out;
}
void fonts_query(ActionContext &c)
{
    std::vector<SPObject *> list; if (!targets(c, list)) return;
    array rows;
    // A row is one family/style/object occurrence. This keeps object IDs within the same pagination
    // contract instead of hiding an unbounded IDs array inside each font-family row.
    for (auto o : list) {
        if (!o->style || !(cast<SPText>(o) || cast<SPFlowtext>(o) || type(o) == "tspan" || type(o) == "flowPara" || type(o) == "flowSpan")) continue;
        auto families = computed_text(o, "font-family");
        for (auto const &family : font_families(families)) {
            bool generic = family == "sans-serif" || family == "serif" || family == "monospace" || family == "cursive" || family == "fantasy" || family == "system-ui";
            bool available = generic || FontFactory::get().hasFontFamily(family);
            rows.emplace_back(object{{"family", family}, {"style", computed_text(o, "font-style")},
                {"weight", computed_text(o, "font-weight")}, {"stretch", computed_text(o, "font-stretch")},
                {"object_id", id(o)}, {"available", available}, {"missing", !available}});
        }
    }
    paginate(c, rows);
}
void selection_query(ActionContext &c)
{
    array rows;
    if (c.selection) for (auto item : c.selection->items()) rows.emplace_back(object{{"id", id(item)}});
    paginate(c, rows);
}
constexpr std::string_view bound_choices[] = {"geometric", "visual"};
constexpr std::string_view visible_choices[] = {"visible", "hidden"};
constexpr std::string_view lock_choices[] = {"locked", "unlocked"};
TypeDescriptor const property_list{{{"type", "array"}, {"minItems", 1}, {"maxItems", 10},
    {"items", {{"type", "string"}, {"enum", {"fill", "stroke", "stroke-width", "opacity", "paint-order", "font-family", "font-style", "font-weight", "font-stretch", "font-size"}}}}}};
constexpr ParamSpec pagination[] = {
    {.key = "limit", .type = ParamType::Integer, .default_value = "100", .min = 1, .max = 1000, .help = "Page size, 1 to 1000."},
    {.key = "cursor", .type = ParamType::Text, .help = "Opaque continuation bound to document, revision and query."}};
constexpr ParamSpec with_ids[] = {
    pagination[0], pagination[1], {.key = "ids", .type = ParamType::List, .help = "Explicit unique object IDs; absent means all applicable objects."}};
constexpr ParamSpec object_params[] = {
    pagination[0], pagination[1], with_ids[2],
    {.key = "type", .type = ParamType::Text, .help = "Exact SVG element local name, such as rect or use."},
    {.key = "layer", .type = ParamType::Text, .help = "Layer ID; includes descendants of nested layers."},
    {.key = "visibility", .type = ParamType::Choice, .choices = visible_choices, .help = "Effective ancestor-aware visibility."},
    {.key = "lock", .type = ParamType::Choice, .choices = lock_choices, .help = "Effective ancestor-aware lock state."},
    {.key = "bounds", .type = ParamType::Choice, .default_value = "geometric", .choices = bound_choices, .help = "Geometric or visual bounds."}};
ParamSpec const style_params[] = {pagination[0], pagination[1], with_ids[2],
    {.key = "properties", .type = ParamType::List, .help = "Computed style projection; absent means the supported ten properties.", .descriptor = &property_list}};
object document_schema()
{
    auto number_map = object{{"type", "object"}, {"additionalProperties", scalar("integer")}};
    return closed({{"document_id", scalar("string")}, {"revision", scalar("integer")}, {"dirty", scalar("boolean")},
        {"title", scalar("string")}, {"metadata", scalar("string")},
        {"units", closed({{"preferred", scalar("string")}, {"root_user_unit", closed({{"name", {{"const", "user"}}},
            {"x", length_schema()}, {"y", length_schema()}}, {"name", "x", "y"})}, {"viewbox_to_css_px", affine_schema()}},
            {"preferred", "root_user_unit", "viewbox_to_css_px"})},
        {"size", closed({{"width", length_schema()}, {"height", length_schema()}}, {"width", "height"})},
        {"object_count", scalar("integer")}, {"object_counts", number_map}},
        {"document_id", "revision", "dirty", "units", "size", "object_count", "object_counts"});
}
} // namespace
std::vector<PackageCommand> query_commands()
{
    std::vector<PackageCommand> out;
    auto add = [&](std::string_view name, std::string_view summary, std::span<ParamSpec const> params,
                   void (*handler)(ActionContext &), object data, object example = {}) {
        ActionSpec spec{.name = name, .mode = "read-only", .summary = summary, .params = params, .canonical_id = name, .handler = handler};
        // The registry binds the documented payload to successful envelope statuses.
        out.push_back({spec, name, "read-only", "Q", std::move(example), std::move(data),
                       {"unknown-id", "duplicate-id", "stale-cursor", "invalid-cursor"}, {}});
    };
    add("query.document", "Inspect document units, dimensions, revision, metadata and object counts.", {}, document_query, document_schema());
    add("query.pages", "Inspect page order, labels and rectangles in document-root CSS px.", pagination, pages_query,
        page_schema(closed({{"id", scalar("string")}, {"label", scalar("string")}, {"order", scalar("integer")}, {"rect", nullable(rect_schema())}}, {"id", "label", "order", "rect"})), {{"limit", 100}});
    auto layer_row = closed({{"id", scalar("string")}, {"parent_id", nullable(scalar("string"))}, {"depth", scalar("integer")},
        {"label", scalar("string")}, {"visible", scalar("boolean")}, {"locked", scalar("boolean")}},
        {"id", "parent_id", "depth", "label", "visible", "locked"});
    add("query.layers", "Inspect the layer hierarchy, effective visibility and locking.", pagination, layers_query, page_schema(layer_row));
    auto object_row = layer_row;
    auto &props = object_row.at("properties").as_object();
    props["type"] = scalar("string"); props["layer_id"] = nullable(scalar("string")); props["bounds"] = nullable(rect_schema());
    auto &required = object_row.at("required").as_array(); required.emplace_back("type"); required.emplace_back("layer_id"); required.emplace_back("bounds");
    add("query.objects", "Inspect object hierarchy, IDs, types, labels and filtered bounds.", object_params, objects_query,
        page_schema(object_row), {{"type", "path"}, {"bounds", "geometric"}});
    add("query.geometry", "Inspect per-object root transforms, path geometry and both bound kinds.", with_ids, geometry_query,
        page_schema(closed({{"id", scalar("string")}, {"shape_kind", scalar("string")}, {"transform", affine_schema()},
            {"path_data", nullable(scalar("string"))}, {"geometric_bounds", nullable(rect_schema())}, {"visual_bounds", nullable(rect_schema())}},
            {"id", "shape_kind", "transform", "path_data", "geometric_bounds", "visual_bounds"})));
    object projection;
    for (auto key : style_properties) projection[key] = nullable(key == "stroke-width" || key == "font-size" ? length_schema() : scalar(key == "opacity" ? "number" : "string"));
    add("query.styles", "Inspect a computed projection of paint, stroke, opacity and font style.", style_params, styles_query,
        page_schema(closed({{"id", scalar("string")}, {"properties", closed(projection)}}, {"id", "properties"})), {{"properties", {"fill", "stroke-width"}}});
    auto tone_schema = closed({{"managed", scalar("boolean")}, {"brightness", scalar("number")}, {"contrast", scalar("number")},
        {"intensity", scalar("number")}, {"highlights", scalar("number")}, {"shadows", scalar("number")}, {"midtones", scalar("number")}},
        {"managed", "brightness", "contrast", "intensity", "highlights", "shadows", "midtones"});
    object attributes; for (auto key : {"inkscape:brightness", "inkscape:contrast", "inkscape:intensity", "inkscape:highlights", "inkscape:shadows", "inkscape:midtones", "inkscape:bitmap-adjustment"}) attributes[key] = scalar("string");
    add("query.images", "Inspect image sources, grant states, pixel metadata and VA adjustments.", with_ids, images_query,
        page_schema(closed({{"id", scalar("string")}, {"source_kind", {{"enum", {"embedded", "linked", "missing"}}}},
            {"href", scalar("string")}, {"link_grant", {{"enum", {"embedded", "granted", "ungranted", "remote", "missing"}}}},
            {"pixel_size", nullable(closed({{"width", scalar("integer")}, {"height", scalar("integer")}}, {"width", "height"}))},
            {"density", nullable(closed({{"x", scalar("number")}, {"y", scalar("number")}, {"unit", {{"const", "dpi"}}}}, {"x", "y", "unit"}))},
            {"mime", nullable(scalar("string"))}, {"tone", tone_schema}, {"adjustment_attributes", closed(attributes)}},
            {"id", "source_kind", "href", "link_grant", "pixel_size", "density", "mime", "tone", "adjustment_attributes"})));
    add("query.fonts", "Inspect used font family/style occurrences and native font availability.", with_ids, fonts_query,
        page_schema(closed({{"family", scalar("string")}, {"style", scalar("string")}, {"weight", scalar("string")}, {"stretch", scalar("string")},
            {"object_id", scalar("string")}, {"available", scalar("boolean")}, {"missing", scalar("boolean")}},
            {"family", "style", "weight", "stretch", "object_id", "available", "missing"})));
    add("query.selection", "Inspect the session selection; inspection documents start empty.", pagination, selection_query,
        page_schema(closed({{"id", scalar("string")}}, {"id"})));
    return out;
}
} // namespace Inkscape::VACardsCli
