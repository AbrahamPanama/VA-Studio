// SPDX-License-Identifier: GPL-2.0-or-later
#include "vacards-cli-registry.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <set>
#include <regex>
#include <glib.h>

#include "actions-vacards-cli.h"
#include "actions-vacards-file.h"
#include "vacards-cli-dispatch.h"
namespace Inkscape::VACardsCli {
namespace {
using namespace boost::json;
int compare_numbers(value const &a, value const &b) {
    if (!a.is_double() && !b.is_double()) {
        bool negative_a = a.is_int64() && a.as_int64() < 0;
        bool negative_b = b.is_int64() && b.as_int64() < 0;
        if (negative_a != negative_b) return negative_a ? -1 : 1;
        if (negative_a) return a.as_int64() < b.as_int64() ? -1 : a.as_int64() > b.as_int64() ? 1 : 0;
        auto x = a.to_number<std::uint64_t>(), y = b.to_number<std::uint64_t>();
        return x < y ? -1 : x > y ? 1 : 0;
    }
    auto x = a.to_number<long double>(), y = b.to_number<long double>();
    return x < y ? -1 : x > y ? 1 : 0;
}
object scalar(char const *type)
{
    return {{"type", type}};
}
object strings()
{
    return {{"type", "array"}, {"items", {{"type", "string"}}}};
}
object closed(object properties, array required = {})
{
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}
object physical()
{
    return closed({{"value", scalar("number")}, {"unit", {{"enum", {"px", "mm", "cm", "in", "pt", "pc"}}}}},
                  {"value", "unit"});
}
object parameter(ParamSpec const &p)
{
    if (p.descriptor)
        return p.descriptor->schema;
    object s;
    switch (p.type) {
        case ParamType::Boolean:
            s = scalar("boolean");
            break;
        case ParamType::Integer:
            s = scalar("integer");
            break;
        case ParamType::Number:
        case ParamType::Angle:
        case ParamType::Duration:
            s = scalar("number");
            break;
        case ParamType::Length:
            s = physical();
            break;
        case ParamType::List:
            s = strings();
            s["minItems"] = 1;
            s["items"].as_object()["minLength"] = 1;
            break;
        default:
            s = scalar("string");
            s["minLength"] = 1;
            break;
    }
    if (p.type != ParamType::Length) {
        if (p.min)
            s["minimum"] = *p.min;
        if (p.max)
            s["maximum"] = *p.max;
    } else {
        array alternatives;
        for (auto const &[unit, factor] : std::vector<std::pair<std::string_view, double>>{{"px", 1},
                                                                                           {"mm", 96 / 25.4},
                                                                                           {"cm", 96 / 2.54},
                                                                                           {"in", 96},
                                                                                           {"pt", 96 / 72.},
                                                                                           {"pc", 16}}) {
            auto n = scalar("number");
            if (p.min)
                n["minimum"] = *p.min / factor;
            if (p.max)
                n["maximum"] = *p.max / factor;
            alternatives.push_back(object{{"properties", {{"value", n}, {"unit", {{"const", unit}}}}}});
        }
        s["anyOf"] = alternatives;
    }
    if (!p.choices.empty()) {
        array a;
        for (auto v : p.choices)
            a.emplace_back(v);
        s["enum"] = a;
    }
    if (!stored_unit(p.type).empty())
        s["x-unit"] = stored_unit(p.type);
    if (!p.default_value.empty()) {
        ActionSpec single{.name = "default", .params = std::span<ParamSpec const>(&p, 1)};
        auto parsed = parse_params(single, "");
        if (parsed.ok() && parsed.has(p.key)) {
            auto const &v = parsed.at(p.key);
            switch (p.type) {
                case ParamType::Boolean:
                    s["default"] = v.boolean;
                    break;
                case ParamType::Integer:
                    s["default"] = v.integer;
                    break;
                case ParamType::Length:
                    s["default"] = object{{"value", v.number}, {"unit", "px"}};
                    break;
                case ParamType::Number:
                case ParamType::Angle:
                case ParamType::Duration:
                    s["default"] = v.number;
                    break;
                case ParamType::List: {
                    array a;
                    for (auto const &i : v.items)
                        a.emplace_back(i);
                    s["default"] = a;
                    break;
                }
                default:
                    s["default"] = v.text;
                    break;
            }
        }
    }
    s["description"] = p.help;
    return s;
}
object common_result()
{
    auto arbitrary = scalar("object");
    auto length = closed({{"px", scalar("number")}, {"preferred", physical()}}, {"px", "preferred"});
    auto error = closed({{"code", scalar("string")},
                         {"key", scalar("string")},
                         {"message", scalar("string")},
                         {"hint", scalar("string")},
                         {"retryable", scalar("boolean")},
                         {"details", arbitrary}},
                        {"code", "message", "hint", "retryable", "details"});
    object props{
        {"schema", {{"const", "va-studio.cli-result/1"}}},
        {"id", scalar("string")},
        {"seq", scalar("integer")},
        {"action", scalar("string")},
        {"params_text", scalar("string")},
        {"params", arbitrary},
        {"status", {{"enum", {"ok", "changed", "unchanged", "rejected", "cancelled", "failed", "uncertain"}}}},
        {"reason", scalar("string")},
        {"message", scalar("string")},
        {"mode", scalar("string")},
        {"dry_run", scalar("boolean")},
        {"document", {{"anyOf", {arbitrary, scalar("null")}}}},
        {"targets",
         closed({{"selected", scalar("integer")},
                 {"eligible", scalar("integer")},
                 {"covered", scalar("integer")},
                 {"excluded",
                  {{"type", "array"},
                   {"items", closed({{"id", scalar("string")}, {"reason", scalar("string")}}, {"id", "reason"})}}}},
                {"selected", "eligible", "covered", "excluded"})},
        {"created", strings()},
        {"modified", strings()},
        {"deleted", strings()},
        {"selection_after", strings()},
        {"undo", scalar("string")},
        {"metrics", arbitrary},
        {"warnings", strings()},
        {"error", error},
        {"data", arbitrary},
        {"normalized_params", arbitrary},
        {"document_id", scalar("string")},
        {"revision_before", scalar("integer")},
        {"revision_after", scalar("integer")},
        {"exclusions",
         {{"type", "array"},
          {"items", closed({{"id", scalar("string")}, {"reason", scalar("string")}}, {"id", "reason"})}}},
        {"coded_warnings",
         {{"type", "array"},
          {"items", closed({{"code", scalar("string")}, {"message", scalar("string")}}, {"code", "message"})}}},
        {"undo_effect", {{"enum", {"none", "one-step", "undo", "redo"}}}},
        {"publication",
         closed({{"state", scalar("string")}, {"persisted", scalar("boolean")}}, {"state", "persisted"})},
        {"lengths", {{"type", "object"}, {"additionalProperties", length}}}};
    array required;
    for (auto const &kv : props)
        if (kv.key() != "error" && kv.key() != "data" && kv.key() != "lengths")
            required.emplace_back(kv.key());
    return closed(std::move(props), std::move(required));
}
// Payload schemas are command-specific; open maps are reserved for generated
// schemas/catalog metadata, whose vocabulary belongs to JSON Schema itself.
object result_payload(std::string_view id)
{
    object properties{{"validation_level", {{"enum", {"preflight", "computed"}}}}};
    if (id == "system.options") {
        properties["halt-on-error"] = scalar("boolean");
        properties["preferred-unit"] = scalar("string");
    } else if (id == "system.result-file") {
        properties["target"] = scalar("string");
    } else if (id == "system.describe") {
        for (auto key :
             {"product", "display_version", "version", "inkscape_version", "schema", "catalog_version", "catalog_hash"})
            properties[key] = scalar("string");
        properties["features"] = object{{"type", "object"}, {"additionalProperties", scalar("boolean")}};
        properties["exit_codes"] = object{{"type", "object"}, {"additionalProperties", scalar("string")}};
        properties["actions"] = object{{"type", "array"}, {"items", scalar("object")}};
    } else if (id == "system.catalog") {
        properties["schema"] = object{{"const", catalog_version}};
        properties["hash"] = scalar("string");
        properties["commands"] = object{{"type", "array"}, {"items", scalar("object")}};
    } else if (id == "geometry.resize") {
        properties["transform_stroke"] = scalar("boolean");
        properties["preserve_transform"] = scalar("boolean");
    } else if (id == "geometry.corners") {
        properties["status"] = scalar("string");
    } else if (id == "bitmap.histogram" || id == "bitmap.tone-query") {
        object tone;
        for (auto key : {"brightness", "contrast", "intensity", "highlights", "shadows", "midtones"})
            tone[key] = scalar("number");
        if (id == "bitmap.histogram") {
            properties["image"] = scalar("string");
            properties["embedded"] = scalar("boolean");
            properties["remapped"] = scalar("boolean");
            for (auto key : {"pixel_width", "pixel_height", "sampled_pixels", "transparent_pixels", "shadow_clipped",
                             "highlight_clipped"})
                properties[key] = object{{"type", "integer"}, {"minimum", 0}};
            properties["settings"] = closed(tone);
            properties["luminance"] = object{{"type", "array"}, {"items", object{{"type", "integer"}, {"minimum", 0}}}};
        } else {
            auto target = tone;
            target["id"] = scalar("string");
            target["type"] = scalar("string");
            target["managed"] = scalar("boolean");
            properties["targets"] = object{{"type", "array"}, {"items", closed(target)}};
            object aggregate;
            for (auto const &p : tone)
                aggregate[p.key()] =
                    closed({{"value", scalar("number")}, {"mixed", scalar("boolean")}}, {"value", "mixed"});
            properties["aggregate"] = closed(aggregate);
        }
    }
    return closed(properties);
}
void catalog_body(ActionContext &c)
{
    c.record.data = command_catalog();
}
struct Entry
{
    ActionSpec spec;
    TypeDescriptor input, result;
    std::vector<std::string_view> aliases, constraints, errors, warnings;
    std::string example;
};
void file_session_required(ActionContext &context)
{
    context.record.status = Status::Rejected;
    context.record.reason = "session-required";
    context.record.message = "File commands require an agent-owned document lifecycle.";
}
std::list<Entry> &entries()
{
    static std::list<Entry> list = [] {
        std::list<Entry> out;
        auto add = [&](ActionSpec spec, std::string_view id, std::string_view effects, std::string_view policy,
                       object example = {}, object result_data = {},
                       std::vector<std::string_view> const &extra_errors = {},
                       std::vector<std::string_view> const &extra_warnings = {}) {
            out.emplace_back();
            auto &e = out.back();
            e.spec = spec;
            e.spec.canonical_id = id;
            if (id.starts_with("file.") && !e.spec.handler) e.spec.handler = file_session_required;
            e.spec.effects = effects;
            e.spec.target_policy = policy;
            e.spec.dry_run_grade = id == "file.new" || id == "file.open" || id == "file.import" ? "computed"
                                  : id == "file.save" || id == "file.export" || id == "file.close" ? "preflight"
                                  : effects == "session-state" ? "preflight" : "computed";
            e.spec.undo_policy = effects == "document-edit"   ? "one-step"
                                 : id.starts_with("history.") ? "consume-history"
                                                              : "none";
            e.aliases.push_back(spec.name);
            e.spec.aliases = e.aliases;
            e.errors = {"invalid-argument",
                        "unknown-key",
                        "repeated-key",
                        "invalid-utf8",
                        "malformed-json",
                        "nonfinite-number",
                        "wrong-type",
                        "out-of-range",
                        "missing-required",
                        "not-a-choice",
                        "unknown-command",
                        "no-document",
                        "stale-document",
                        "stale-revision",
                        "internal-error",
                        "slice-unavailable",
                        "cancelled",
                        "transaction-unavailable",
                        "request-too-large",
                        "malformed-value",
                        "cannot-open-result-file",
                        "offset-failed",
                        "no-such-node",
                        "no-corners",
                        "invalid-input",
                        "engine-limit",
                        "corner-edit-failed",
                        "empty-selection",
                        "unavailable",
                        "no-eligible-targets",
                        "requires-single-bitmap",
                        "missing-source",
                        "zero-dimension",
                        "no-closed-shapes",
                        "document-busy"};
            if (effects == "document-edit")
                e.errors.insert(e.errors.end(),
                                {"empty-selection", "unavailable", "no-bounds", "incompatible-operands",
                                 "boolean-failed", "needs-two-operands", "requires-single-shape", "unsupported-shape",
                                 "path-not-supported", "non-similarity-transform"});
            for (auto code : extra_errors)
                if (std::find(e.errors.begin(), e.errors.end(), code) == e.errors.end())
                    e.errors.push_back(code);
            // Availability codes extend the frozen file inventory at the session's
            // registration boundary, shared by descriptors and system.catalog.
            if (id.starts_with("file."))
                for (auto code : {"resource-unavailable", "publication-unavailable"})
                    if (std::find(e.errors.begin(), e.errors.end(), code) == e.errors.end())
                        e.errors.push_back(code);
            // Typed file callbacks never invoke legacy flat parsing or geometry services.
            // Preserve historical debt IDs in the before/after reconciliation ledger.
            if (id.starts_with("file.")) std::erase_if(e.errors, [](std::string_view code) {
                return code == "malformed-value" || code == "no-closed-shapes" || code == "no-bounds" ||
                       code == "incompatible-operands" || code == "boolean-failed" || code == "needs-two-operands" ||
                       code == "requires-single-shape" || code == "unsupported-shape" || code == "path-not-supported" ||
                       code == "non-similarity-transform";
            });
            // Remove only these known-command legacy pairs.
            static constexpr std::pair<std::string_view, std::string_view> d18_legacy_removed[] = {
                {"file.new", "unknown-command"}, {"file.open", "unknown-command"},
                {"file.close", "unknown-command"}, {"file.save", "unknown-command"},
                {"file.export", "unknown-command"}, {"file.import", "unknown-command"},
                {"file.close", "nonfinite-number"}, {"file.open", "nonfinite-number"},
                {"file.save", "nonfinite-number"}
            };
            std::erase_if(e.errors, [&](std::string_view code) {
                return std::any_of(std::begin(d18_legacy_removed), std::end(d18_legacy_removed),
                    [&](auto const &pair) { return pair.first == id && pair.second == code; });
            });
            // Remove only these known-command legacy pairs accepted as counterexamples.
            static constexpr std::pair<std::string_view, std::string_view> d21_legacy_removed[] = {
                {"file.close", "not-a-choice"}, {"file.close", "publication-unavailable"},
                {"file.close", "resource-unavailable"},
                {"file.export", "duplicate-id"}, {"file.export", "font-policy-required"},
                {"file.export", "format-unsupported"}, {"file.export", "invalid-path"},
                {"file.export", "invalid-svg"}, {"file.export", "invalid-svgz"},
                {"file.export", "missing-file"}, {"file.export", "pages-invalid"},
                {"file.export", "read-failed"}, {"file.export", "unsafe-xml"},
                {"file.import", "publication-unavailable"}, {"file.import", "read-failed"},
                {"file.import", "transaction-unavailable"},
                {"file.new", "publication-unavailable"}, {"file.new", "resource-unavailable"},
                {"file.open", "publication-unavailable"}, {"file.open", "read-failed"},
                {"file.save", "duplicate-id"}, {"file.save", "font-policy-required"},
                {"file.save", "format-unsupported"}, {"file.save", "invalid-path"},
                {"file.save", "invalid-svg"}, {"file.save", "invalid-svgz"},
                {"file.save", "missing-file"}, {"file.save", "pages-invalid"},
                {"file.save", "read-failed"}, {"file.save", "read-grant-denied"},
                {"file.save", "save-failed"}, {"file.save", "unsafe-xml"}
            };
            std::erase_if(e.errors, [&](std::string_view code) {
                return std::any_of(std::begin(d21_legacy_removed), std::end(d21_legacy_removed),
                    [&](auto const &pair) { return pair.first == id && pair.second == code; });
            });
            // Snapshot parse/native-construction refusals only guard the app's own serialization.
            static constexpr std::pair<std::string_view, std::string_view> d22_legacy_removed[] = {
                {"file.export", "invalid-file"}, {"file.save", "invalid-file"}
            };
            std::erase_if(e.errors, [&](std::string_view code) {
                return std::any_of(std::begin(d22_legacy_removed), std::end(d22_legacy_removed),
                    [&](auto const &pair) { return pair.first == id && pair.second == code; });
            });
            e.spec.error_codes = e.errors;
            e.warnings = {"cancel-too-late", "keep-ratio-ignored", "simplify-skipped", "missing-output-id"};
            for (auto code : extra_warnings)
                if (std::find(e.warnings.begin(), e.warnings.end(), code) == e.warnings.end())
                    e.warnings.push_back(code);
            if (id.starts_with("file.")) e.warnings.push_back("file-warning");
            e.spec.warning_codes = e.warnings;
            if (id == "geometry.resize")
                e.constraints = {"width-or-height"};
            if (id == "geometry.corners")
                e.constraints = {"nodes-for-node-scope", "node-scope-for-nodes"};
            e.spec.constraints = e.constraints;
            object props;
            array required;
            for (auto const &p : spec.params) {
                if (p.key == "dry-run") continue;
                auto node = parameter(p);
                if (p.required)
                    required.emplace_back(p.key);
                props[p.key] = node;
            }
            e.input.schema = closed(props, required);
            e.result.schema = common_result();
            e.result.schema["properties"].as_object()["data"] =
                result_data.empty() ? result_payload(id) : result_data;
            object normalized;
            for (auto const &p : spec.params) {
                if (p.key == "dry-run") continue;
                normalized[p.key] =
                    p.type == ParamType::Length
                        ? closed({{"px", scalar("number")}, {"preferred", physical()}}, {"px", "preferred"})
                        : parameter(p);
            }
            e.result.schema["properties"].as_object()["normalized_params"] = closed(normalized);
            if (!result_data.empty()) {
                // Package payloads are mandatory on success; errors retain the generic envelope.
                // The registry's own commands use the unchanged default descriptor above.
                auto success = e.result.schema;
                success["properties"].as_object()["status"] = object{{"enum", {"ok", "changed", "unchanged"}}};
                success["required"].as_array().emplace_back("data");
                auto error = e.result.schema;
                error["properties"].as_object()["status"] = object{{"enum", {"rejected", "cancelled", "failed", "uncertain"}}};
                error["properties"].as_object()["data"] = scalar("object");
                e.result.schema = object{{"anyOf", array{std::move(success), std::move(error)}}};
            }
            e.spec.input = &e.input;
            e.spec.result = &e.result;
            object request{{"schema", "va-studio.cli-request/1"},
                           {"id", "example"},
                           {"command", id},
                           {"params", example},
                           {"dry_run", false}};
            if (spec.needs_document) {
                request["document"] = "d1";
                request["if_revision"] = 0;
            }
            if (id == "file.new" || id == "file.open" || id == "file.close") request["if_revision"] = 0;
            e.example = serialize(request);
            e.spec.example = e.example;
        };
        add(describe_command(), "system.describe", "read-only", "Q");
        add(options_command(), "system.options", "session-state", "S");
        add(undo_command(), "history.undo", "session-state", "S");
        add(redo_command(), "history.redo", "session-state", "S");
        add(result_file_command(), "system.result-file", "session-state", "S", {{"target", "-"}});
        add(boolean_command(), "geometry.boolean", "document-edit", "G", {{"op", "union"}});
        add(corners_command(), "geometry.corners", "document-edit", "G", {{"radius", {{"value", 1}, {"unit", "mm"}}}});
        add(offset_command(), "geometry.offset", "document-edit", "G", {{"distance", {{"value", 1}, {"unit", "mm"}}}});
        add(transform_resize_command(), "geometry.resize", "document-edit", "G",
            {{"width", {{"value", 10}, {"unit", "mm"}}}});
        add(histogram_command(), "bitmap.histogram", "read-only", "Q");
        add(tone_query_command(), "bitmap.tone-query", "read-only", "Q");
        ActionSpec catalog{.name = "system.catalog",
                           .mode = "read-only",
                           .summary = "Return the generated typed command catalog."};
        catalog.handler = catalog_body;
        catalog.needs_document = false;
        add(catalog, "system.catalog", "read-only", "Q");
        // Package-owned tables (session.*, query.*), appended after the registry's own commands.
        for (auto const &c : session_commands())
            add(c.spec, c.id, c.effects, c.policy, c.example, c.result_data, c.errors, c.warnings);
        for (auto const &c : query_commands())
            add(c.spec, c.id, c.effects, c.policy, c.example, c.result_data, c.errors, c.warnings);
        for (auto const &c : file_commands())
            add(c.spec, c.id, c.effects, c.policy, c.example, c.result_data, c.errors, c.warnings);
        return out;
    }();
    return list;
}
value canonical(value const &v)
{
    if (v.is_object()) {
        std::vector<std::string> keys;
        for (auto const &p : v.as_object())
            keys.emplace_back(p.key());
        std::sort(keys.begin(), keys.end());
        object o;
        for (auto const &k : keys)
            o[k] = canonical(v.as_object().at(k));
        return o;
    }
    if (v.is_array()) {
        array a;
        for (auto const &x : v.as_array())
            a.push_back(canonical(x));
        return a;
    }
    return v;
}
} // namespace
std::vector<ActionSpec const *> command_specs()
{
    std::vector<ActionSpec const *> out;
    for (auto &e : entries())
        out.push_back(&e.spec);
    std::sort(out.begin(), out.end(), [](auto a, auto b) { return a->canonical_id < b->canonical_id; });
    return out;
}
ActionSpec const *find_command(std::string_view name)
{
    for (auto s : command_specs()) {
        if (s->canonical_id == name)
            return s;
        for (auto a : s->aliases)
            if (a == name)
                return s;
    }
    return nullptr;
}
boost::json::object request_schema(ActionSpec const &s)
{
    auto params = s.input->schema;
    for (auto name : s.constraints) {
        if (name == "width-or-height")
            params["anyOf"] = array{object{{"required", {"width"}}}, object{{"required", {"height"}}}};
        if (name == "node-scope-for-nodes") {
            object rule;
            rule["if"] = object{{"required", {"nodes"}}};
            rule["then"] = object{{"properties", {{"scope", {{"const", "nodes"}}}}}, {"required", {"scope"}}};
            params["allOf"] = array{rule};
        }
        if (name == "nodes-for-node-scope") {
            params["if"] = object{{"properties", {{"scope", {{"const", "nodes"}}}}}, {"required", {"scope"}}};
            params["then"] = object{{"required", {"nodes"}}};
        }
    }
    array commands{s.canonical_id};
    for (auto a : s.aliases)
        if (a != s.canonical_id)
            commands.emplace_back(a);
    auto result = closed({{"schema", {{"const", "va-studio.cli-request/1"}}},
                          {"id", {{"type", "string"}, {"minLength", 1}, {"maxBytes", 128}}},
                          {"command", {{"enum", commands}}},
                          {"document", {{"type", "string"}, {"minLength", 1}}},
                          {"if_revision", {{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}},
                          {"params", params},
                          {"dry_run", {{"type", "boolean"}, {"default", false}}}},
                         {"schema", "id", "command", "params"});
    if (auto meta = s.input->schema.if_contains("x-m3-contract")) {
        result["required"].as_array().emplace_back("document");
        if (meta->as_object().at("guard_required").as_bool())
            result["required"].as_array().emplace_back("if_revision");
    } else if (s.effects == "document-edit" || s.canonical_id.starts_with("history.") ||
        s.canonical_id == "file.save" || s.canonical_id == "file.export") {
        result["required"].as_array().emplace_back("document");
        result["required"].as_array().emplace_back("if_revision");
    }
    if (s.canonical_id == "file.new" || s.canonical_id == "file.open" || s.canonical_id == "file.close")
        result["required"].as_array().emplace_back("if_revision");
    result["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    return result;
}
boost::json::object result_schema_descriptor(ActionSpec const &s)
{
    auto out = s.result->schema;
    out["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    return out;
}
boost::json::object production_result_schema(boost::json::object const &data)
{
    auto success = common_result();
    success["properties"].as_object()["status"] = object{{"enum", {"ok", "changed", "unchanged"}}};
    success["properties"].as_object()["data"] = data;
    success["required"].as_array().emplace_back("data");
    auto error = common_result();
    error["properties"].as_object()["status"] = object{{"enum", {"rejected", "cancelled", "failed", "uncertain"}}};
    return object{{"$schema", "https://json-schema.org/draft/2020-12/schema"},
                  {"anyOf", array{std::move(success), std::move(error)}}};
}
std::string catalog_hash(boost::json::value const &descriptor)
{
    auto bytes = serialize(canonical(descriptor));
    auto *hash =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<guchar const *>(bytes.data()), bytes.size());
    std::string out(hash);
    g_free(hash);
    return out;
}
boost::json::object command_catalog()
{
    array commands;
    for (auto s : command_specs()) {
        auto desc = describe_action(*s);
        desc["id"] = s->canonical_id;
        desc["version"] = s->version;
        array aliases;
        for (auto a : s->aliases)
            aliases.emplace_back(a);
        desc["aliases"] = aliases;
        desc["effects"] = s->effects;
        desc["target_policy"] = s->target_policy;
        desc["undo_policy"] = s->undo_policy;
        desc["dry_run_grade"] = s->dry_run_grade;
        desc["cancellation_boundary"] = s->cancellation_boundary;
        array constraints;
        for (auto a : s->constraints)
            constraints.emplace_back(a);
        desc["constraints"] = constraints;
        array errors;
        for (auto a : s->error_codes)
            errors.emplace_back(a);
        desc["error_codes"] = errors;
        array warnings;
        for (auto a : s->warning_codes)
            warnings.emplace_back(a);
        desc["warning_codes"] = warnings;
        desc["limits"] = object{{"request_bytes", s->limits.request_bytes},
                                {"response_bytes", s->limits.response_bytes},
                                {"id_bytes", s->limits.id_bytes}};
        desc["request_schema"] = request_schema(*s);
        desc["result_schema"] = result_schema_descriptor(*s);
        desc["example"] = parse(s->example);
        commands.push_back(desc);
    }
    object out{{"schema", catalog_version}, {"commands", commands}};
    out["hash"] = catalog_hash(out);
    return out;
}
boost::json::array mcp_descriptors()
{
    array out;
    for (auto s : command_specs()) {
        std::string name = "va_" + std::string(s->canonical_id);
        std::replace(name.begin(), name.end(), '.', '_');
        std::replace(name.begin(), name.end(), '-', '_');
        out.push_back(object{{"name", name},
                             {"description", s->summary},
                             {"inputSchema", request_schema(*s)},
                             {"outputSchema", result_schema_descriptor(*s)}});
    }
    return out;
}
std::optional<ParseError> validate_schema(value const &v, object const &s, std::string path)
{
    auto fail = [&](std::string code, std::string message) -> std::optional<ParseError> {
        return ParseError{std::move(code), path, std::move(message)};
    };
    if (auto t = s.if_contains("type")) {
        auto matches = [&](value const &type) {
            return type == "object" ? v.is_object() : type == "array" ? v.is_array() :
                   type == "string" ? v.is_string() : type == "boolean" ? v.is_bool() :
                   type == "null" ? v.is_null() : type == "integer" ?
                   (v.is_int64() || v.is_uint64() || (v.is_double() && std::isfinite(v.as_double()) &&
                    std::trunc(v.as_double()) == v.as_double())) : type == "number" && v.is_number();
        };
        bool good = t->is_array() ? std::any_of(t->as_array().begin(), t->as_array().end(), matches) : matches(*t);
        if (!good) return fail("wrong-type", "Unexpected JSON type at " + path);
    }
    if (v.is_number()) {
        long double n = v.is_uint64() ? static_cast<long double>(v.as_uint64()) :
                        v.is_int64() ? static_cast<long double>(v.as_int64()) : v.as_double();
        if (!std::isfinite(n))
            return fail("nonfinite-number", "Numbers must be finite.");
        if (auto x = s.if_contains("minimum"); x && compare_numbers(v, *x) < 0)
            return fail("out-of-range", "Below minimum.");
        if (auto x = s.if_contains("maximum"); x && compare_numbers(v, *x) > 0)
            return fail("out-of-range", "Above maximum.");
    }
    if (auto x = s.if_contains("x-exact-uint64"); x && x->as_bool()) {
        if (!(v.is_uint64() || (v.is_int64() && v.as_int64() >= 0)))
            return fail("out-of-range", "Use an exact unsigned 64-bit integer JSON token.");
    }
    if (v.is_number()) {
        if (auto x = s.if_contains("exclusiveMinimum"); x && compare_numbers(v, *x) <= 0)
            return fail("out-of-range", "Must exceed exclusive minimum.");
        if (auto x = s.if_contains("exclusiveMaximum"); x && compare_numbers(v, *x) >= 0)
            return fail("out-of-range", "Must be below exclusive maximum.");
    }
    if (auto x = s.if_contains("const"); x && v != *x)
        return fail("invalid-argument", "Unexpected constant.");
    if (auto x = s.if_contains("enum")) {
        bool found = false;
        for (auto const &e : x->as_array())
            if (v == e)
                found = true;
        if (!found)
            return fail("not-a-choice", "Value is not an allowed choice.");
    }
    if (v.is_string()) {
        auto const &str = v.as_string();
        if (auto pattern = s.if_contains("pattern"); pattern && pattern->is_string()) {
            std::regex expression(std::string(pattern->as_string()));
            if (!std::regex_search(str.begin(), str.end(), expression))
                return fail("invalid-argument", "String does not match the required pattern.");
        }
        if (!g_utf8_validate(str.data(), str.size(), nullptr))
            return fail("invalid-utf8", "Text must be UTF-8.");
        if (auto x = s.if_contains("minLength"); x && static_cast<std::size_t>(g_utf8_strlen(str.data(), str.size())) < x->to_number<std::size_t>())
            return fail("out-of-range", "String is too short.");
        if (auto x = s.if_contains("maxLength"); x && static_cast<std::size_t>(g_utf8_strlen(str.data(), str.size())) > x->to_number<std::size_t>())
            return fail("out-of-range", "String is too long.");
        if (auto x = s.if_contains("maxBytes"); x && str.size() > x->to_number<std::size_t>())
            return fail("out-of-range", "String exceeds byte limit.");
    }
    if (v.is_object()) {
        auto const &o = v.as_object();
        if (auto x = s.if_contains("maxProperties"); x && o.size() > x->to_number<std::size_t>())
            return fail("out-of-range", "Too many object properties.");
        if (auto x = s.if_contains("minProperties"); x && o.size() < x->to_number<std::size_t>())
            return fail("out-of-range", "Too few object properties.");
        if (auto x = s.if_contains("propertyNames"))
            for (auto const &p : o)
                if (auto error = validate_schema(value(p.key()), x->as_object(), path)) return error;
        if (auto x = s.if_contains("required"))
            for (auto const &k : x->as_array())
                if (!o.contains(k.as_string()))
                    return fail("missing-required", "Missing required field: " + std::string(k.as_string()));
        auto props = s.if_contains("properties");
        auto additional = s.if_contains("additionalProperties");
        for (auto const &p : o) {
            auto node = props ? props->as_object().if_contains(p.key()) : nullptr;
            if (!node && additional && additional->is_bool() && !additional->as_bool())
                return fail("unknown-key", p.key() == "dry-run"
                    ? "Use envelope dry_run, not params[\"dry-run\"]."
                    : "Unknown key: " + std::string(p.key()));
            if (!node && additional && additional->is_object())
                node = additional;
            if (node)
                if (auto error = validate_schema(p.value(), node->as_object(), path + "/" + std::string(p.key())))
                    return error;
        }
    }
    if (v.is_array()) {
        if (auto x = s.if_contains("uniqueItems"); x && x->is_bool() && x->as_bool()) {
            auto const &items = v.as_array();
            bool strings_only = std::all_of(items.begin(), items.end(), [](auto const &item) { return item.is_string(); });
            if (strings_only) {
                std::set<std::string_view> seen;
                for (auto const &item : items)
                    if (!seen.insert(item.as_string()).second)
                        return fail("invalid-argument", "Array items must be unique.");
            }
            for (std::size_t i = 0; !strings_only && i < items.size(); ++i)
                for (std::size_t j = i + 1; j < items.size(); ++j)
                    if (items[i] == items[j] ||
                        ((items[i].is_double() || items[j].is_double()) && items[i].is_number() && items[j].is_number() &&
                         items[i].to_number<double>() == items[j].to_number<double>()))
                        return fail("invalid-argument", "Array items must be unique.");
        }
        if (auto x = s.if_contains("minItems"); x && v.as_array().size() < x->to_number<std::size_t>())
            return fail("out-of-range", "Array is too short.");
        if (auto x = s.if_contains("maxItems"); x && v.as_array().size() > x->to_number<std::size_t>())
            return fail("out-of-range", "Array is too long.");
        if (auto x = s.if_contains("items"))
            for (auto const &item : v.as_array())
                if (auto error = validate_schema(item, x->as_object(), path + "/[]"))
                    return error;
    }
    if (auto x = s.if_contains("allOf"))
        for (auto const &option : x->as_array())
            if (auto error = validate_schema(v, option.as_object(), path))
                return error;
    if (auto x = s.if_contains("anyOf")) {
        bool matched = false;
        std::optional<ParseError> discriminated_error;
        for (auto const &option : x->as_array()) {
            auto const &branch = option.as_object();
            auto error = validate_schema(v, branch, path);
            if (!error) matched = true;
            else if (v.is_object()) {
                // Physical bounds have one alternative per explicit unit. Retain
                // the matching unit's range error instead of claiming a missing field.
                bool has_discriminator = false, matches_discriminators = true;
                if (auto props = branch.if_contains("properties"))
                    for (auto const &p : props->as_object())
                        if (auto expected = p.value().as_object().if_contains("const")) {
                            has_discriminator = true;
                            auto actual = v.as_object().if_contains(p.key());
                            if (!actual || *actual != *expected) matches_discriminators = false;
                        }
                if (has_discriminator && matches_discriminators) discriminated_error = error;
            }
        }
        if (!matched) {
            if (discriminated_error) return discriminated_error;
            return fail("missing-required", "No permitted alternative matches.");
        }
    }
    if (auto x = s.if_contains("not"); x && !validate_schema(v, x->as_object(), path))
        return fail("invalid-argument", "Forbidden parameter combination.");
    if (auto x = s.if_contains("oneOf")) {
        unsigned matches = 0;
        for (auto const &branch : x->as_array())
            if (!validate_schema(v, branch.as_object(), path)) ++matches;
        if (matches != 1) return fail("invalid-argument", "Exactly one route must match.");
    }
    if (auto x = s.if_contains("x-css-px-absolute-maximum"); x && v.is_object()) {
        auto const &length = v.as_object();
        auto unit = length.at("unit").as_string();
        double factor = unit == "px" ? 1 : unit == "mm" ? 96 / 25.4 : unit == "cm" ? 96 / 2.54 :
                        unit == "in" ? 96 : unit == "pt" ? 96 / 72. : 16;
        auto px = length.at("value").to_number<double>() * factor;
        if (!std::isfinite(px) || std::abs(px) > x->to_number<double>())
            return fail("out-of-range", "Converted CSS-px length exceeds the permitted magnitude.");
    }
    if (auto x = s.if_contains("if"); x && !validate_schema(v, x->as_object(), path)) {
        if (auto then = s.if_contains("then"))
            if (auto error = validate_schema(v, then->as_object(), path))
                return error;
    } else if (x) {
        if (auto otherwise = s.if_contains("else"))
            if (auto error = validate_schema(v, otherwise->as_object(), path)) return error;
    }
    return {};
}
namespace {
void defaults(value &v, object const &schema) {
    // Match branches on the original node, before any defaults can select a route.
    auto raw = v;
    if (v.is_object()) {
        auto &o = v.as_object();
        if (auto props = schema.if_contains("properties")) {
            for (auto const &p : props->as_object()) {
                auto const &child = p.value().as_object();
                if (!o.contains(p.key())) {
                    if ((p.key() == "anchor" && o.contains("pivot")) ||
                        (p.key() == "dpi" && o.contains("size"))) continue;
                    if (auto d = child.if_contains("default")) o[p.key()] = *d;
                }
                if (auto current = o.if_contains(p.key())) defaults(*current, child);
            }
        }
    } else if (v.is_array()) {
        if (auto items = schema.if_contains("items"))
            for (auto &item : v.as_array()) defaults(item, items->as_object());
    }
    for (auto key : {"allOf", "oneOf", "anyOf"})
        if (auto branches = schema.if_contains(key))
            for (auto const &branch : branches->as_array())
                if (!validate_schema(raw, branch.as_object())) defaults(v, branch.as_object());
    if (auto condition = schema.if_contains("if")) {
        auto branch = schema.if_contains(validate_schema(raw, condition->as_object()) ? "else" : "then");
        if (branch) defaults(v, branch->as_object());
    }
}
void css_lengths(value &v, object const &schema) {
    if (schema.contains("x-css-px-absolute-maximum") && v.is_object()) {
        auto &o = v.as_object(); auto unit = o.at("unit").as_string();
        double factor = unit == "px" ? 1 : unit == "mm" ? 96 / 25.4 : unit == "cm" ? 96 / 2.54 :
                        unit == "in" ? 96 : unit == "pt" ? 96 / 72. : 16;
        o["value"] = o.at("value").to_number<double>() * factor; o["unit"] = "px";
        return;
    }
    if (v.is_object()) {
        if (auto props = schema.if_contains("properties"))
            for (auto const &p : props->as_object())
                if (auto child = v.as_object().if_contains(p.key())) css_lengths(*child, p.value().as_object());
    } else if (v.is_array()) {
        if (auto items = schema.if_contains("items"))
            for (auto &child : v.as_array()) css_lengths(child, items->as_object());
    }
}
}
std::optional<ParseError> normalize_schema_params(object const &raw, object const &schema, object &out) {
    if (auto error = validate_schema(raw, schema)) return error;
    value normalized = raw;
    defaults(normalized, schema);
    if (auto error = validate_schema(normalized, schema)) return error;
    css_lengths(normalized, schema);
    out = canonical(normalized).as_object();
    return {};
}
} // namespace Inkscape::VACardsCli
