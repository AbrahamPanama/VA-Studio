// SPDX-License-Identifier: GPL-2.0-or-later
#include "vacards-cli-production.h"
#include "vacards-cli-fault.h"
#include <stdexcept>
#include <utility>
#include "actions-vacards-cli.h"
#include "actions-vacards-selection.h"
#include "actions-vacards-geometry.h"
#include "actions-vacards-bitmap.h"
#include "actions-vacards-clip.h"
#include "actions-vacards-nest.h"
#include "nesting/nesting-cli-service.h"
#include "document.h"
#include "vacards-cli-edit-services.h"
#include "selection.h"
#include "object/sp-item.h"
#include <algorithm>
namespace Inkscape::VACardsCli {
namespace {
thread_local detail::CliFaultPlan *cli_fault_plan = nullptr;
}
namespace detail {
CliFaultPlan *exchange_cli_fault_plan_for_testing(CliFaultPlan *plan) noexcept {
    return std::exchange(cli_fault_plan, plan);
}
}
bool cli_fault(std::string_view point, CliFaultKind kind) noexcept {
    if (!cli_fault_plan) return false;
    for (auto &entry : cli_fault_plan->faults) {
        if (entry.point == point && entry.kind == kind) {
            if (entry.visits != UINT64_MAX) ++entry.visits;
            return entry.occurrence != 0 && entry.visits == entry.occurrence;
        }
    }
    return false;
}
void cli_fault_throw(std::string_view point) {
    if (cli_fault(point, CliFaultKind::ServiceException))
        throw std::runtime_error("Scoped CLI service exception at " + std::string(point));
}
std::uint64_t cli_counted_limit(std::string_view name, std::uint64_t production_limit) noexcept {
    if (cli_fault_plan) for (auto const &entry : cli_fault_plan->limits)
        if (entry.name == name) return std::min(entry.ceiling, production_limit);
    return production_limit;
}
Record production_unavailable(Request const &request) {
    Record r;
    r.action = request.command; r.dry_run = request.dry_run;
    r.status = Status::Rejected; r.reason = "slice-unavailable";
    r.message = "Complete the owning M3 packet and acceptance gates before enabling this command.";
    r.error = ParseError{r.reason, {}, r.message};
    return r;
}
void production_unavailable_action(ActionContext &c) {
    c.record.status = Status::Rejected; c.record.reason = "slice-unavailable";
    c.record.message = "Complete the owning M3 packet and acceptance gates before enabling this command.";
    c.record.error = ParseError{c.record.reason, {}, c.record.message};
}
ActionSpec const *find_production_command(std::string_view id) {
    static auto commands = [] {
        auto out = production_commands();
        for (auto &c : out) {
            c.spec.canonical_id = c.id; c.spec.effects = c.effects;
            c.spec.error_codes = c.errors; c.spec.warning_codes = c.warnings;
        }
        return out;
    }();
    for (auto const &c : commands) if (c.id == id) return &c.spec;
    return nullptr;
}
Record dispatch_production(Request const &request, DispatchContext &context) {
    auto commands = production_commands();
    auto command = std::find_if(commands.begin(), commands.end(), [&](auto const &c) { return c.id == request.command; });
    Record r; r.action = request.command; r.dry_run = request.dry_run;
    auto reject = [&](std::string code, std::string message) {
        r.status = Status::Rejected; r.reason = std::move(code); r.message = std::move(message);
        r.error = ParseError{r.reason, {}, r.message}; return r;
    };
    if (command == commands.end()) return reject("unknown-command", "Command is not in the M3 overlay.");
    auto spec = command->spec; spec.canonical_id = command->id; spec.effects = command->effects;
    r.mode = std::string(spec.mode);
    auto stamp = document_stamp(context.document);
    r.document_id = stamp.id; r.revision_before = r.revision_after = stamp.revision;
    boost::json::object envelope{{"schema","va-studio.cli-request/1"},{"id",request.id},
        {"command",request.command},{"params",request.params},{"dry_run",request.dry_run}};
    if (request.document) envelope["document"] = *request.document;
    if (request.if_revision) envelope["if_revision"] = *request.if_revision;
    if (auto error = validate_schema(envelope, request_schema(spec))) return reject("invalid-argument", error->message);
    Request normalized = request;
    if (auto error = normalize_schema_params(request.params, spec.input->schema, normalized.params))
        return reject("invalid-argument", error->message);
    r.normalized_params = normalized.params;
    if (!context.document) return reject("no-document", "This command requires an open document.");
    if (!request.document || *request.document != stamp.id) return reject("stale-document", "Use the current document identity.");
    auto const &meta = spec.input->schema.at("x-m3-contract").as_object();
    auto revision = meta.at("guard_domain").as_string() == "session" ?
        (context.session_revision_snapshot ? context.session_revision_snapshot() : context.session_revision) : stamp.revision;
    if (request.if_revision && *request.if_revision != revision) return reject("stale-revision", "Refresh the guard revision.");
    if (context.command_admission)
        if (auto refused = context.command_admission(normalized)) {
            refused->action = r.action; refused->dry_run = r.dry_run; refused->mode = r.mode;
            refused->document_id = stamp.id; refused->revision_before = refused->revision_after = stamp.revision;
            return *refused;
        }
    if (context.cancelled && context.cancelled()) {
        r.status = Status::Cancelled; r.reason = "cancelled"; r.message = "Cancelled before execution."; return r;
    }
    if (!context.production_handler) return production_unavailable(request);
    // Owner adapters compute previews and settle exactly once; no generic disposable edit path.
    auto result = context.production_handler(normalized);
    result.action = r.action; result.mode = r.mode; result.dry_run = r.dry_run;
    result.normalized_params = normalized.params; result.document_id = stamp.id;
    result.revision_before = stamp.revision; result.revision_after = document_stamp(context.document).revision;
    result.preferred_unit = context.preferred_unit;
    return result;
}
Record execute_production(Request const &r, DispatchContext &context, ProductionContext &production) {
    auto selection_ids = [&] {
        std::vector<std::string> ids;
        if (context.selection) for (auto item : context.selection->items())
            if (item->getId()) ids.emplace_back(item->getId());
        return ids;
    };
    auto before_selection = selection_ids();
    auto before_tokens = production.tokens.snapshot().active_tokens;
    Record result;
    if (r.command.starts_with("selection.")) result = execute_selection(r, context, production.edits);
    else if (r.command.starts_with("history.")) result = execute_history(r, context, production.edits);
    else if (r.command.starts_with("geometry.")) result = execute_geometry(r, context, production.edits);
    else if (r.command.starts_with("bitmap.")) result = execute_bitmap(r, context, production);
    else if (r.command.starts_with("clip.")) result = execute_clip(r, context, production);
    else if (r.command.starts_with("nest.")) result = execute_nest(r, context, production);
    else return production_unavailable(r);
    // Selection adapters install directly; publishers report IDs only after native commit.
    if (!r.dry_run && result.status == Status::Changed && !r.command.starts_with("selection.") &&
        !r.command.starts_with("history.") && context.selection) {
        std::vector<SPItem *> items;
        for (auto const &id : result.selection_after)
            if (auto item = cast<SPItem>(context.document->getObjectById(id))) items.push_back(item);
        std::vector<SPItem *> selected;
        for (auto item : context.selection->items()) selected.push_back(item);
        bool same = items.size() == selected.size() && std::equal(items.begin(), items.end(), selected.begin());
        if (items.size() == result.selection_after.size() && !same) context.selection->setList(items);
    }
    auto retained = production.tokens.snapshot();
    if (production.bitmap && before_tokens != retained.active_tokens) production.bitmap->prune(retained);
    if (production.nest && before_tokens != retained.active_tokens) production.nest->prune(retained);
    if (!r.dry_run && (before_selection != selection_ids() || before_tokens != retained.active_tokens)) {
        if (context.session_state_changed) context.session_state_changed(); else ++context.session_revision;
    }
    if (result.status == Status::Ok || result.status == Status::Changed || result.status == Status::Unchanged) {
        for (auto const &command : production_commands()) if (command.id == r.command) {
            if (auto error = validate_schema(result.data, command.result_data)) {
                // Native settlement has already happened. Preserve its IDs/history/revision;
                // incomplete metadata must never masquerade as a successful empty result.
                auto mutation = document_stamp(context.document).revision != production.edits.stamp.revision ? "committed" :
                    before_selection != selection_ids() || before_tokens != retained.active_tokens ? "session-only" : "none";
                result.status = Status::Failed; result.reason = "internal-error";
                result.message = "Native result does not satisfy the command data contract: " + error->message;
                result.error = ParseError{result.reason, error->key, result.message};
                result.error_details = {{"reason", "result-contract"}, {"mutation_state", mutation}};
                result.error_retryable = false;
            }
            break;
        }
    }
    return result;
}
std::vector<PackageCommand> production_commands() {
    std::vector<PackageCommand> result;
    for (auto commands : {selection_commands(), geometry_commands(), bitmap_commands(), clip_commands(), nest_commands()})
        for (auto &command : commands) result.push_back(std::move(command));
    return result;
}
boost::json::object planned_production_catalog() {
    boost::json::array rows;
    for (auto const &c : production_commands()) {
        boost::json::array errors; for (auto code : c.errors) errors.emplace_back(code);
        auto const &contract = c.spec.input->schema.at("x-m3-contract").as_object();
        rows.push_back(boost::json::object{{"id",c.id},{"version",c.spec.version},
            {"available",false},{"accepted",false},{"effects",c.effects},{"target_policy",c.policy},
            {"contract",contract},{"guard_domain",contract.at("guard_domain")},{"guard_required",contract.at("guard_required")},
            {"warnings",contract.at("warnings")},{"undo",c.spec.undo_policy},{"dry_run",c.spec.dry_run_grade},
            {"params",c.spec.input->schema},{"result_data",c.result_data},
            {"errors",errors},{"stub_errors",boost::json::array{"slice-unavailable"}},{"example",c.example},
            {"schema_status","S0-R1 contract amended; implementation and independent acceptance pending"}});
    }
    boost::json::object result{{"schema","va-studio.cli-m3-planned/1"},{"accepted",false},{"commands",rows}};
    result["hash"] = catalog_hash(result);
    return result;
}
boost::json::object production_catalog() {
    using namespace boost::json;
    auto result = command_catalog();
    auto &rows = result.at("commands").as_array();
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](value const &row) {
        return find_production_command(row.as_object().at("id").as_string()) != nullptr;
    }), rows.end());
    for (auto const &c : production_commands()) {
        auto spec = find_production_command(c.id);
        auto row = describe_action(*spec);
        row["id"] = c.id; row["version"] = spec->version;
        row["available"] = true; row["accepted"] = true; row["experimental"] = false;
        row["schema_status"] = "accepted (VA Studio 1.1, M3 and M4)";
        row["aliases"] = array{}; row["effects"] = c.effects; row["target_policy"] = c.policy;
        row["params"] = spec->input->schema; row["request_schema"] = request_schema(*spec);
        row["result_schema"] = production_result_schema(c.result_data); row["result_data"] = c.result_data;
        row["contract"] = spec->input->schema.at("x-m3-contract");
        row["guard_domain"] = row.at("contract").as_object().at("guard_domain");
        row["guard_required"] = row.at("contract").as_object().at("guard_required");
        array errors; for (auto e : c.errors) errors.emplace_back(e); row["error_codes"] = errors;
        array warnings; for (auto w : c.warnings) warnings.emplace_back(w); row["warning_codes"] = warnings;
        row["undo_policy"] = spec->undo_policy; row["dry_run_grade"] = spec->dry_run_grade;
        object example{{"schema", "va-studio.cli-request/1"}, {"id", "example"},
                       {"command", c.id}, {"params", c.example}, {"document", "document-id"}};
        if (row.at("guard_required").as_bool()) example["if_revision"] = 0;
        row["example"] = std::move(example);
        rows.push_back(std::move(row));
    }
    result.erase("hash"); result["hash"] = catalog_hash(result);
    return result;
}

}
