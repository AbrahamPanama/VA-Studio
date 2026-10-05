// SPDX-License-Identifier: GPL-2.0-or-later
#include "vacards-cli-dispatch.h"
#include "vacards-cli-production.h"
#include <algorithm>

#include <cmath>
#include <set>
#include <stdexcept>
#include <glib.h>

#include "actions-vacards-cli.h"
#include "document.h"
#include "object/sp-item.h"
#include "selection.h"
namespace Inkscape::VACardsCli {
namespace {
using namespace boost::json;
double factor(std::string_view unit)
{
    if (unit == "px")
        return 1;
    if (unit == "mm")
        return 96 / 25.4;
    if (unit == "cm")
        return 96 / 2.54;
    if (unit == "in")
        return 96;
    if (unit == "pt")
        return 96 / 72.;
    if (unit == "pc")
        return 16;
    throw std::invalid_argument("Unsupported physical unit.");
}
// Run only after the JSON parser accepts the grammar. Decode each key before comparing,
// so escaped and literal spellings of the same key are duplicates too.
class DuplicateScanner
{
    std::string_view text;
    std::size_t pos = 0;
    void space()
    {
        while (pos < text.size() && g_ascii_isspace(text[pos]))
            ++pos;
    }
    std::string string()
    {
        auto begin = pos++;
        while (pos < text.size()) {
            auto c = text[pos++];
            if (c == '\\')
                ++pos;
            else if (c == '"')
                break;
        }
        return std::string(parse(text.substr(begin, pos - begin)).as_string());
    }

public:
    explicit DuplicateScanner(std::string_view input)
        : text(input)
    {}
    bool value()
    {
        space();
        if (text[pos] == '{') {
            ++pos;
            space();
            std::set<std::string> keys;
            if (text[pos] == '}') {
                ++pos;
                return false;
            }
            while (true) {
                space();
                auto key = string();
                if (!keys.insert(key).second)
                    return true;
                space();
                ++pos;
                if (value())
                    return true;
                space();
                if (text[pos++] == '}')
                    break;
            }
        } else if (text[pos] == '[') {
            ++pos;
            space();
            if (text[pos] == ']') {
                ++pos;
                return false;
            }
            while (true) {
                if (value())
                    return true;
                space();
                if (text[pos++] == ']')
                    break;
            }
        } else if (text[pos] == '"')
            string();
        else {
            while (pos < text.size() && text[pos] != ',' && text[pos] != ']' && text[pos] != '}' &&
                   !g_ascii_isspace(text[pos]))
                ++pos;
        }
        return false;
    }
};
object envelope(Request const &r)
{
    object o{{"schema", "va-studio.cli-request/1"},
             {"id", r.id},
             {"command", r.command},
             {"params", r.params},
             {"dry_run", r.dry_run}};
    if (r.document)
        o["document"] = *r.document;
    if (r.if_revision)
        o["if_revision"] = *r.if_revision;
    return o;
}
void reject(Record &r, std::string code, std::string message, std::string key = {})
{
    r.status = Status::Rejected;
    r.reason = code;
    r.message = message;
    r.error = ParseError{std::move(code), std::move(key), std::move(message)};
}
void normalize(Record &r, ParseResult const &p, std::string_view unit)
{
    for (auto const &[key, v] : p.values) {
        switch (v.type) {
            case ParamType::Boolean:
                r.normalized_params[key] = v.boolean;
                break;
            case ParamType::Integer:
                r.normalized_params[key] = v.integer;
                break;
            case ParamType::Number:
            case ParamType::Angle:
            case ParamType::Duration:
                r.normalized_params[key] = v.number;
                break;
            case ParamType::Length:
                r.normalized_params[key] = report_length(v.number, unit);
                break;
            case ParamType::List: {
                array a;
                for (auto const &item : v.items)
                    a.emplace_back(item);
                r.normalized_params[key] = a;
                break;
            }
            default:
                r.normalized_params[key] = v.text;
                break;
        }
    }
}
} // namespace
DispatchContext &action_session_context()
{
    static DispatchContext context;
    return context;
}
boost::json::object report_length(double px, std::string_view unit)
{
    return {{"px", px}, {"preferred", {{"value", px / factor(unit)}, {"unit", unit}}}};
}
static RequestParse parse_request_impl(std::string_view json, bool production)
{
    auto fail = [](std::string code, std::string message) {
        return RequestParse{{}, ParseError{std::move(code), {}, std::move(message)}};
    };
    if (json.size() > 1048576)
        return fail("request-too-large", "Request exceeds 1 MiB.");
    if (!g_utf8_validate(json.data(), json.size(), nullptr))
        return fail("invalid-utf8", "Request must be UTF-8.");
    boost::system::error_code ec;
    boost::json::parse_options options;
    options.max_depth = 128;
    auto value = boost::json::parse(json, ec, {}, options);
    if (ec)
        return fail("malformed-json", ec.message());
    if (DuplicateScanner(json).value())
        return fail("repeated-key", "Duplicate JSON keys are forbidden.");
    if (!value.is_object())
        return fail("wrong-type", "Request must be an object.");
    auto const &o = value.as_object();
    auto command = o.if_contains("command");
    if (!command || !command->is_string())
        return fail("missing-required", "A string command is required.");
    auto spec = production ? find_production_command(command->as_string()) : nullptr;
    production = spec != nullptr; // Overlay mode falls back to unchanged M1/M2 admission.
    if (!spec) spec = find_command(command->as_string());
    if (!spec)
        return fail("unknown-command", "Command is not registered.");
    // Preserve trustworthy identity on schema refusals, without dispatching or
    // echoing invalid params. Framing/duplicate-key failures remain anonymous.
    std::string error_id;
    auto schema = request_schema(*spec);
    if (auto id = o.if_contains("id"); id && id->is_string())
        error_id = std::string(id->as_string());
    auto rejected = [&](ParseError const &error) {
        auto mapped = error;
        if (production) mapped.code = "invalid-argument";
        return RequestParse{{}, mapped, error_id, std::string(spec->canonical_id)};
    };
    if (auto error = validate_schema(value, schema))
        return rejected(*error);
    Request r;
    r.id = std::string(o.at("id").as_string());
    r.command = std::string(command->as_string());
    r.params = o.at("params").as_object();
    if (auto p = o.if_contains("document"))
        r.document = std::string(p->as_string());
    if (auto p = o.if_contains("if_revision")) {
        r.if_revision = p->to_number<std::uint64_t>(ec);
        if (ec)
            return rejected(ParseError{"out-of-range", "/if_revision", "Revision is outside the permitted integer range."});
    }
    if (auto p = o.if_contains("dry_run"))
        r.dry_run = p->as_bool();
    if (production) {
        boost::json::object normalized;
        if (auto error = normalize_schema_params(r.params, spec->input->schema, normalized)) return rejected(*error);
        r.params = std::move(normalized);
    } else {
        auto params = typed_params(*spec, r.params);
        if (params.error) return rejected(*params.error);
    }
    return {r, {}};
}
RequestParse parse_request(std::string_view json) { return parse_request_impl(json, false); }
RequestParse parse_production_request(std::string_view json) { return parse_request_impl(json, true); }
ParseResult typed_params(ActionSpec const &spec, boost::json::object const &params)
{
    ParseResult result;
    if (auto e = validate_schema(params, spec.input->schema)) {
        result.error = e;
        return result;
    }
    for (auto const &p : spec.params) {
        if (p.key == "dry-run") continue;
        auto value = params.if_contains(p.key);
        if (!value) {
            if (p.required) {
                result.error = ParseError{"missing-required", std::string(p.key), "A parameter is required."};
                return result;
            }
            if (!p.default_value.empty()) {
                ActionSpec single{.name = spec.name, .params = std::span<ParamSpec const>(&p, 1)};
                auto defaults = parse_params(single, "");
                if (defaults.error) {
                    result.error = defaults.error;
                    return result;
                }
                result.values.insert(defaults.values.begin(), defaults.values.end());
            }
            continue;
        }
        ParamValue v;
        v.type = p.type;
        // Structured package parameters have already passed their recursive descriptor.
        // The file service consumes Request::params directly, never a legacy string parser.
        if (p.descriptor && p.type != ParamType::Length &&
            (value->is_object() || (value->is_array() && p.type != ParamType::List))) {
            v.text = serialize(*value);
            result.values[std::string(p.key)] = std::move(v);
            continue;
        }
        if (p.descriptor && p.type == ParamType::List && value->is_array() &&
            std::any_of(value->as_array().begin(), value->as_array().end(), [](auto const &x) { return !x.is_string(); })) {
            v.text = serialize(*value);
            result.values[std::string(p.key)] = std::move(v);
            continue;
        }
        switch (p.type) {
            case ParamType::Boolean:
                v.boolean = value->as_bool();
                break;
            case ParamType::Integer: {
                boost::system::error_code ec;
                v.integer = value->to_number<long long>(ec);
                if (ec) {
                    result.error = ParseError{"out-of-range", "/" + std::string(p.key),
                                              "Value is outside the representable integer range."};
                    return result;
                }
                break;
            }
            case ParamType::Length: {
                auto const &o = value->as_object();
                v.number = o.at("value").to_number<double>() * factor(o.at("unit").as_string());
                break;
            }
            case ParamType::Number:
            case ParamType::Angle:
            case ParamType::Duration:
                v.number = value->to_number<double>();
                break;
            case ParamType::List:
                for (auto const &item : value->as_array())
                    v.items.emplace_back(item.as_string());
                break;
            default:
                v.text = std::string(value->as_string());
                break;
        }
        if (p.type == ParamType::Length || p.type == ParamType::Number || p.type == ParamType::Angle ||
            p.type == ParamType::Duration || p.type == ParamType::Integer) {
            auto n = p.type == ParamType::Integer ? static_cast<double>(v.integer) : v.number;
            if (!std::isfinite(n) || (p.min && n < *p.min) || (p.max && n > *p.max)) {
                result.error = ParseError{"out-of-range", std::string(p.key), "Value is outside the permitted range."};
                return result;
            }
        }
        result.values[std::string(p.key)] = std::move(v);
    }
    return result;
}
void dispatch_validated(ActionSpec const &spec, ParseResult const &params, DispatchContext &context, Record &record,
                        std::function<void(ActionContext &)> const &handler, bool needs_document)
{
    record.preferred_unit = context.preferred_unit;
    auto before = document_stamp(context.document);
    record.document_id = before.id;
    record.revision_before = before.revision;
    if (context.document) {
        auto f = context.document->getDocumentFilename();
        record.document_path = f ? f : "";
    }
    if (needs_document && !context.document) {
        reject(record, "no-document", "This action needs an open document.");
        return;
    }
    try {
        if (spec.canonical_id == "system.options" && params.has("preferred-unit")) {
            context.preferred_unit = params.at("preferred-unit").text;
            record.preferred_unit = context.preferred_unit;
        }
        normalize(record, params, context.preferred_unit);
        ActionContext c{context.app, params, context.document, context.selection, record, context.operation_lease};
        if (handler)
            handler(c);
        else if (spec.handler)
            spec.handler(c);
        else
            throw std::logic_error("Missing registered handler.");
    } catch (std::exception const &e) {
        record.status = Status::Failed;
        record.reason = "internal-error";
        record.message = e.what();
    } catch (...) {
        record.status = Status::Failed;
        record.reason = "internal-error";
        record.message = "Unexpected internal error.";
    }
    // Handler-local transactions have finished unwinding here. These arrays
    // describe committed effects, never edits that were rolled back.
    if (record.status == Status::Failed || record.status == Status::Rejected || record.status == Status::Cancelled) {
        record.created.clear();
        record.modified.clear();
        record.deleted.clear();
    }
    if (spec.canonical_id == "system.options")
        record.data["preferred-unit"] = context.preferred_unit;
    record.revision_after = document_stamp(context.document).revision;
    if (context.selection) {
        record.selection_after.clear();
        for (auto *item : context.selection->items())
            if (item->getId())
                record.selection_after.emplace_back(item->getId());
    }
    if (record.one_undo_step)
        record.undo_effect = "one-step";
    if (record.status == Status::Changed && spec.canonical_id == "history.undo")
        record.undo_effect = "undo";
    if (record.status == Status::Changed && spec.canonical_id == "history.redo")
        record.undo_effect = "redo";
}
Record dispatch(Request const &request, DispatchContext &context)
{
    // Only an explicitly supplied native production capability selects the overlay.
    // The agent Engine installs it for development; legacy native dispatch is unchanged.
    if (context.production_handler && find_production_command(request.command))
        return dispatch_production(request, context);
    Record r;
    r.action = request.command;
    auto s = find_command(request.command);
    if (!s) {
        reject(r, "unknown-command", "Command is not registered.");
        return r;
    }
    r.action = std::string(s->canonical_id);
    r.mode = std::string(s->mode);
    r.dry_run = request.dry_run;
    auto stamp = document_stamp(context.document);
    r.document_id = stamp.id;
    r.revision_before = r.revision_after = stamp.revision;
    if (auto e = validate_schema(envelope(request), request_schema(*s))) {
        reject(r, e->code, e->message, e->key);
        return r;
    }
    auto parsed = typed_params(*s, request.params);
    if (parsed.error) {
        auto &e = *parsed.error;
        reject(r, e.code, e.message, e.key);
        return r;
    }
    if (context.session_handler && (s->canonical_id.starts_with("session.") ||
                                    s->canonical_id == "system.options")) {
        if (context.cancelled && context.cancelled()) {
            r.status = Status::Cancelled;
            r.reason = "cancelled";
            r.message = "Cancelled before execution.";
            return r;
        }
        return context.session_handler(request);
    }
    if (s->canonical_id.starts_with("file.") && s->needs_document && !context.document) {
        reject(r, "no-document", "This action needs an open document.");
        return r;
    }
    if (request.document && *request.document != stamp.id) {
        reject(r, "stale-document", "Use the current document identity.");
        return r;
    }
    auto current = s->needs_document ? stamp.revision :
        (context.session_revision_snapshot ? context.session_revision_snapshot() : context.session_revision);
    if (request.if_revision && *request.if_revision != current) {
        reject(r, "stale-revision", "Refresh the revision before retrying.");
        return r;
    }
    if (context.cancelled && context.cancelled()) {
        r.status = Status::Cancelled;
        r.reason = "cancelled";
        r.message = "Cancelled before execution.";
        return r;
    }
    if (s->canonical_id.starts_with("file.")) {
        bool lifecycle = s->canonical_id == "file.new" || s->canonical_id == "file.open" || s->canonical_id == "file.close";
        if (lifecycle && context.document && !request.document) {
            reject(r, "missing-required", "An active document requires its identity for lifecycle changes.", "document");
            return r;
        }
        if (s->needs_document && !context.document) {
            reject(r, "no-document", "This action needs an open document.");
            return r;
        }
    }
    if (context.command_admission) {
        if (auto refused = context.command_admission(request)) {
            refused->action = r.action; refused->mode = r.mode; refused->dry_run = r.dry_run;
            refused->document_id = stamp.id;
            refused->revision_before = refused->revision_after = stamp.revision;
            return *refused;
        }
    }
    if (s->canonical_id.starts_with("file.")) {
        if (!context.file_handler) {
            reject(r, "session-required", "File commands require an agent-owned document lifecycle.");
            return r;
        }
        normalize(r, parsed, context.preferred_unit);
        for (auto const &p : s->params)
            if (p.descriptor && p.type != ParamType::Length)
                if (auto v = request.params.if_contains(p.key)) r.normalized_params[p.key] = *v;
        auto result = context.file_handler(request);
        result.action = r.action; result.mode = r.mode; result.dry_run = r.dry_run;
        result.preferred_unit = context.preferred_unit;
        result.normalized_params = std::move(r.normalized_params);
        if (!result.publication_persisted &&
            (result.status == Status::Rejected || result.status == Status::Failed || result.status == Status::Cancelled)) {
            result.created.clear(); result.modified.clear(); result.deleted.clear();
        }
        if (context.cancelled && context.cancelled() &&
            (result.status == Status::Changed || result.publication_persisted))
            result.warnings.emplace_back("cancel-too-late");
        return result;
    }
    if (request.dry_run && s->effects == "document-edit") {
        if (!context.document) {
            reject(r, "no-document", "This action needs an open document.");
            return r;
        }
        // Disposable document uses the exact same typed service. No history/defaults/selection
        // from the live document can be consumed, even by legacy self-settling engines.
        auto copy = context.document->copy();
        copy->ensureUpToDate();
        auto selection = copy->getSelection();
        if (context.selection)
            for (auto *item : context.selection->items())
                if (item->getId())
                    if (auto *target = cast<SPItem>(copy->getObjectById(item->getId())))
                        selection->add(target);
        DispatchContext disposable{context.app, copy.get(), selection, context.preferred_unit};
        if (parsed.has("dry-run"))
            parsed.values["dry-run"].boolean = false;
        r.dry_run = false; // Execute the commit path on the disposable document.
        dispatch_validated(*s, parsed, disposable, r, {}, s->needs_document);
        r.dry_run = true;
        r.document_id = stamp.id;
        r.revision_before = r.revision_after = stamp.revision;
        if (context.document) {
            auto f = context.document->getDocumentFilename();
            r.document_path = f ? f : "";
        }
        r.one_undo_step = false;
        r.undo_effect = "none";
        if (r.status == Status::Changed)
            r.status = Status::Ok;
        r.data["validation_level"] = "computed";
        return r;
    }
    if (request.dry_run && s->effects == "session-state") {
        normalize(r, parsed, context.preferred_unit);
        r.data["validation_level"] = "preflight";
        return r;
    }
    dispatch_validated(*s, parsed, context, r, {}, s->needs_document);
    if (context.production_handler && s->canonical_id == "system.catalog" && r.status == Status::Ok)
        r.data = production_catalog();
    if (s->canonical_id == "system.options")
        r.data["preferred-unit"] = context.preferred_unit;
    if (s->effects == "session-state" && !s->needs_document && r.status == Status::Ok)
        ++context.session_revision;
    if (context.cancelled && context.cancelled() && r.status == Status::Changed)
        r.warnings.emplace_back("cancel-too-late");
    return r;
}
boost::json::object cli_error(std::string code, std::string message, std::string hint)
{
    // Availability is distinct from conflict, invalid input and unconfirmed publication.
    // ParseError carries a code rather than a retry flag; keep its classification here.
    bool retryable = code == "resource-unavailable" || code == "publication-unavailable" ||
                     code == "document-busy" || code == "transaction-unavailable" || code == "session-busy";
    return {{"code", code}, {"message", message}, {"hint", hint},
            {"retryable", retryable}, {"details", boost::json::object{}}};
}
boost::json::object typed_result(Record const &r, std::string_view id, unsigned seq)
{
    auto out = boost::json::parse(to_json_line(r, seq)).as_object();
    out["id"] = id;
    out["normalized_params"] = r.normalized_params;
    out["document_id"] = r.document_id;
    out["revision_before"] = r.revision_before;
    out["revision_after"] = r.revision_after;
    out["exclusions"] = out.at("targets").as_object().at("excluded");
    boost::json::array warnings;
    for (auto const &w : r.warnings) {
        std::string code = w == "cancel-too-late"                                   ? "cancel-too-late"
                    : w.starts_with("keep-ratio")                            ? "keep-ratio-ignored"
                    : w.find("could not be simplified") != std::string::npos ? "simplify-skipped"
                                                                             : "missing-output-id";
        if (r.action.starts_with("file.")) {
            code = "file-warning";
            if (auto spec = find_command(r.action))
                for (auto declared : spec->warning_codes)
                    if (w == declared || w.starts_with(std::string(declared) + ":")) {
                        code = std::string(declared); break;
                    }
        }
        if (!r.action.starts_with("file.") && !r.action.starts_with("session.") && !r.action.starts_with("system."))
            if (auto spec = find_production_command(r.action))
                for (auto declared : spec->warning_codes)
                    if (w == declared || w.starts_with(std::string(declared) + ":")) { code = std::string(declared); break; }
        warnings.push_back(boost::json::object{{"code", code}, {"message", w}});
    }
    out["coded_warnings"] = warnings;
    out["undo_effect"] = r.undo_effect;
    out["publication"] = boost::json::object{{"state", r.publication}, {"persisted", r.publication_persisted}};
    if (r.error || r.status == Status::Rejected || r.status == Status::Failed || r.status == Status::Uncertain ||
        r.status == Status::Cancelled) {
        auto code = r.error ? r.error->code : r.reason;
        std::string hint = r.error && r.error->message.find("envelope dry_run") != std::string::npos
                               ? "Set dry_run on the request envelope; remove params[\"dry-run\"]."
                           : code == "resource-unavailable" || code == "publication-unavailable"
                               ? "Restore file availability, then submit a fresh request with current guards."
                           : code == "stale-revision" ? "Inspect the current revision and submit a new request."
                           : r.status == Status::Uncertain
                               ? "Inspect destination and recovery paths before another write."
                               : "Inspect the command schema and correct the request or target selection.";
        auto error = cli_error(code, r.message, hint);
        error["key"] = r.error ? r.error->key : "";
        auto details = r.error_details;
        details["action"] = r.action; details["document"] = r.document_id;
        error["details"] = std::move(details);
        if (r.error_retryable) error["retryable"] = *r.error_retryable;
        out["error"] = std::move(error);
    }
    boost::json::object lengths;
    for (auto const &p : r.metrics)
        if (p.value().is_number() &&
            (p.key() == "old_width" || p.key() == "old_height" || p.key() == "new_width" || p.key() == "new_height"))
            lengths[p.key()] = report_length(p.value().to_number<double>(), r.preferred_unit);
    if (!lengths.empty())
        out["lengths"] = lengths;
    return out;
}
int typed_exit_status(Status status)
{
    if (status == Status::Rejected || status == Status::Cancelled)
        return 3;
    if (status == Status::Failed || status == Status::Uncertain)
        return 4;
    return 0;
}
} // namespace Inkscape::VACardsCli
