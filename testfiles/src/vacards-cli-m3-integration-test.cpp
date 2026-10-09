// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "actions/vacards-cli-production.h"
#include "actions/actions-vacards-cli.h"
#include "actions/vacards-cli-session.h"
#include "actions/vacards-cli-edit-services.h"
#include "io/vacards-cli-files.h"
#include "inkscape.h"
#include "selection.h"
#include "document-undo.h"
#include "object/sp-item.h"
#include "xml/repr.h"
#include <set>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdlib>
#include "ui/explode-bitmap-context.h"
#include "ui/bitmap-tone-member-targets.h"
#include "nesting/nesting-cli-service.h"

namespace {
using namespace Inkscape::VACardsCli;
using namespace boost::json;

std::filesystem::path m3_source_root()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

value m3_manifest()
{
    auto path = m3_source_root() / "doc/vacards/cli" / "m3-outcome-manifest.json";
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read M3 outcome manifest: " + path.string());
    return parse(std::string(std::istreambuf_iterator<char>(input), {}));
}

std::string join_mismatches(std::vector<std::string> const &rows)
{
    std::ostringstream out;
    for (auto const &row : rows) out << "\n  " << row;
    return out.str();
}

// A minimal schema witness used to exercise every compiled result_data variant.
// Values come from schema constraints (const/enum/minimum/required), not examples
// copied from an implementation result.
value schema_witness(object const &schema)
{
    if (auto it = schema.find("const"); it != schema.end()) return it->value();
    if (auto it = schema.find("enum"); it != schema.end()) return it->value().as_array().front();
    if (auto it = schema.find("oneOf"); it != schema.end()) {
        auto const &variants = it->value().as_array();
        return schema_witness(variants.front().as_object());
    }
    auto type_it = schema.find("type");
    if (type_it == schema.end()) return object{};
    std::string type;
    if (type_it->value().is_string()) type = std::string(type_it->value().as_string());
    else {
        for (auto const &candidate : type_it->value().as_array()) {
            if (candidate.as_string() != "null") { type = std::string(candidate.as_string()); break; }
        }
        if (type.empty()) return nullptr;
    }
    if (type == "object") {
        object result;
        auto props = schema.if_contains("properties");
        auto required = schema.if_contains("required");
        if (required) for (auto const &key : required->as_array()) {
            auto name = std::string(key.as_string());
            if (props && props->as_object().if_contains(name))
                result[name] = schema_witness(props->as_object().at(name).as_object());
        }
        return result;
    }
    if (type == "array") {
        array result;
        auto min = schema.if_contains("minItems");
        auto count = min ? min->to_number<unsigned>() : 0u;
        if (auto items = schema.if_contains("items"))
            for (unsigned i = 0; i < count; ++i) result.push_back(schema_witness(items->as_object()));
        return result;
    }
    if (type == "string") {
        if (auto pattern = schema.if_contains("pattern"); pattern && pattern->is_string() && pattern->as_string() == "^[0-9a-f]{64}$")
            return value(std::string(64, '0'));
        auto length = schema.if_contains("minLength") ? schema.at("minLength").to_number<unsigned>() : 0u;
        return value(std::string(length, 'x'));
    }
    if (type == "boolean") return true;
    if (type == "integer") {
        if (auto minimum = schema.if_contains("minimum")) return *minimum;
        if (auto minimum = schema.if_contains("exclusiveMinimum")) return value(minimum->to_number<std::int64_t>() + 1);
        if (auto maximum = schema.if_contains("maximum")) return *maximum;
        return 0;
    }
    if (type == "number") {
        if (auto minimum = schema.if_contains("minimum")) return *minimum;
        if (auto minimum = schema.if_contains("exclusiveMinimum")) return value(minimum->to_number<double>() + 1.0);
        if (auto maximum = schema.if_contains("maximum")) return *maximum;
        return 0.0;
    }
    return nullptr;
}

} // namespace

TEST(M3Scaffold, DescriptorInventoryAndRefusal) {
    using namespace Inkscape::VACardsCli;
    auto commands = production_commands();
    ASSERT_EQ(commands.size(), 31u);
    std::set<std::string_view> ids;
    for (auto const &c : commands) {
        EXPECT_TRUE(ids.insert(c.id).second) << c.id;
        EXPECT_NE(c.spec.input, nullptr);
        ParseResult params; Record direct;
        ActionContext action{.params=params, .record=direct};
        ASSERT_NE(c.spec.handler, nullptr);
        c.spec.handler(action);
        EXPECT_EQ(direct.status, Status::Rejected);
        EXPECT_EQ(direct.reason, "slice-unavailable");
        EXPECT_FALSE(direct.one_undo_step);
        EXPECT_TRUE(direct.modified.empty());
        EXPECT_FALSE(validate_schema(c.example, c.spec.input->schema));
        Request r; r.command = c.id; r.dry_run = true;
        auto refusal = production_unavailable(r);
        EXPECT_EQ(refusal.status, Status::Rejected);
        EXPECT_EQ(refusal.reason, "slice-unavailable");
        EXPECT_FALSE(refusal.one_undo_step);
        EXPECT_TRUE(refusal.created.empty());
        EXPECT_FALSE(refusal.publication_persisted);
    }
    EXPECT_TRUE(m3_accepted); EXPECT_TRUE(m4_accepted);
    if (auto path = std::getenv("VACARDS_S0_CATALOG_SNAPSHOT")) {
        std::ofstream out(path); out << boost::json::serialize(planned_production_catalog());
        ASSERT_TRUE(out.good());
        boost::json::object inputs, results;
        for (auto const &c : commands) {
            inputs[c.id] = c.spec.input->schema;
            results[c.id] = c.result_data;
        }
        std::string base(path); base = base.substr(0, base.find_last_of("/\\") + 1);
        std::ofstream in(base + "schemas-draft.json"); in << boost::json::serialize(inputs);
        std::ofstream res(base + "result-schemas-draft.json"); res << boost::json::serialize(results);
        ASSERT_TRUE(in.good()); ASSERT_TRUE(res.good());
    }
}
TEST(M3M3Integration, AdmittedSelectionUsesSessionGuardAndSettlesOnce) {
    using namespace Inkscape::VACardsCli;
    using namespace boost::json;
    if (!Inkscape::Application::exists()) Inkscape::Application::create(false, Inkscape::Application::RuntimePolicy::PreviewHelper);
    std::string svg=R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="group"><rect id="child" width="10" height="10"/></g><rect id="other" x="20" width="10" height="10"/></svg>)";
    FileState files; files.document=SPDocument::createNewDocFromMem(std::span<char const>(svg.data(),svg.size()));
    ASSERT_TRUE(files.document); files.document->ensureUpToDate();
    auto &doc=*files.document; auto selection=doc.getSelection();
    auto xml=sp_repr_save_buf(doc.getReprDoc()).raw(); auto stamp=document_stamp(&doc);
    DispatchContext context; context.document=&doc; context.selection=selection; context.session_revision=9;
    EditServices edits{doc,*selection,stamp}; Grants grants; TokenStore tokens;
    ProductionContext production{files,grants,edits,tokens,[]{}};
    context.production_handler=[&](Request const &r){return execute_production(r,context,production);};
    auto history=history_snapshot(edits); ASSERT_TRUE(history.value);
    Request request; request.id="selection"; request.command="selection.set"; request.document=stamp.id;
    request.if_revision=9; request.params={{"ids",array{"child","group"}}}; request.dry_run=true;
    auto preview=dispatch(request,context);
    EXPECT_EQ(preview.status,Status::Ok); EXPECT_TRUE(selection->isEmpty()); EXPECT_EQ(context.session_revision,9u);
    EXPECT_EQ(preview.data.at("normalized-ids"),value(array{"group"}));
    request.dry_run=false;
    auto changed=dispatch(request,context);
    ASSERT_EQ(changed.status,Status::Changed) << changed.message; EXPECT_EQ(context.session_revision,10u);
    EXPECT_EQ(changed.selection_after,std::vector<std::string>{"group"}); EXPECT_FALSE(changed.one_undo_step);
    EXPECT_EQ(changed.data.at("session-revision").to_number<unsigned>(),10u);
    EXPECT_EQ(dispatch(request,context).reason,"stale-revision");
    request.if_revision=10;
    EXPECT_EQ(dispatch(request,context).status,Status::Unchanged); EXPECT_EQ(context.session_revision,10u);
    request.params={{"ids",array{"unknown"}}};
    EXPECT_EQ(dispatch(request,context).reason,"unknown-id"); EXPECT_EQ(context.session_revision,10u);
    request.params={{"ids",array{}}};
    EXPECT_EQ(dispatch(request,context).status,Status::Changed); EXPECT_TRUE(selection->isEmpty()); EXPECT_EQ(context.session_revision,11u);
    EXPECT_EQ(sp_repr_save_buf(doc.getReprDoc()).raw(),xml); EXPECT_EQ(document_stamp(&doc).revision,stamp.revision);
    auto after=history_snapshot(edits); ASSERT_TRUE(after.value);
    EXPECT_EQ(after.value->can_undo,history.value->can_undo); EXPECT_EQ(after.value->can_redo,history.value->can_redo);
    EXPECT_EQ(after.value->next_undo_label,history.value->next_undo_label); EXPECT_EQ(after.value->next_redo_label,history.value->next_redo_label);
    EXPECT_TRUE(tokens.snapshot().active_tokens.empty()); EXPECT_TRUE(m3_accepted);
}

// The factory is implemented: it returns an engine that stays silent until a request.
TEST(M3Scaffold, StructuredFactoryStartsSilent) {
    bool emitted = false;
    auto result = Inkscape::VACardsCli::make_structured_session({}, [&](auto const &) { emitted=true; return true; });
    ASSERT_TRUE(result.engine) << (result.error ? result.error->message : "");
    EXPECT_FALSE(result.error);
    result.engine->pump(); EXPECT_FALSE(emitted);
    result.engine->close();
}
TEST(M3Registry, RequiredVersionedAdmissionAndSchemaOutcomes) {
    using namespace Inkscape::VACardsCli;
    auto commands = production_commands();
    auto catalog = production_catalog();
    auto const &rows = catalog.at("commands").as_array();
    std::map<std::string, object const *> overlay;
    std::vector<std::string> mismatches;
    for (auto const &row_value : rows) {
        auto const &row = row_value.as_object();
        auto id = std::string(row.at("id").as_string());
        if (find_production_command(id)) {
            if (!overlay.emplace(id, &row).second) mismatches.push_back(id + ": duplicate overlay row");
        }
    }
    if (overlay.size() != commands.size()) mismatches.push_back("overlay executable descriptor count differs: " + std::to_string(overlay.size()) + " vs " + std::to_string(commands.size()));
    for (auto const &command : commands) {
        auto found = overlay.find(std::string(command.id));
        if (found == overlay.end()) { mismatches.push_back(std::string(command.id) + ": missing overlay row"); continue; }
        auto const &row = *found->second;
        auto const &contract = command.spec.input->schema;
        auto const &metadata = contract.at("x-m3-contract");
        if (command.spec.version != 2) mismatches.push_back(std::string(command.id) + ": descriptor version is not 2");
        if (row.at("version") != value(command.spec.version)) mismatches.push_back(std::string(command.id) + ": overlay version differs");
        if (row.at("params") != value(contract)) mismatches.push_back(std::string(command.id) + ": overlay params differ from compiled input descriptor");
        if (row.at("contract") != metadata) mismatches.push_back(std::string(command.id) + ": overlay contract differs from compiled x-m3-contract");
        if (row.at("request_schema") != value(request_schema(command.spec))) mismatches.push_back(std::string(command.id) + ": overlay request schema differs from compiled descriptor");
        if (row.at("result_data") != value(command.result_data)) mismatches.push_back(std::string(command.id) + ": overlay result_data differs from compiled descriptor");
        if (row.at("available") != true || row.at("experimental") != false || row.at("accepted") != true)
            mismatches.push_back(std::string(command.id) + ": overlay admission flags are inconsistent with the experimental M3 catalog");
        if (auto error = validate_schema(command.example, contract))
            mismatches.push_back(std::string(command.id) + ": example params rejected at " + error->key + " (" + error->message + ")");
    }
    EXPECT_TRUE(mismatches.empty()) << "M3 registry/overlay mismatch rows:" << join_mismatches(mismatches);
}
TEST(M3Result, RequiredNativeErrorAndResultMappings) {
    using namespace Inkscape::VACardsCli;
    auto manifest = m3_manifest().as_object();
    std::map<std::string, object const *> contracts;
    for (auto const &entry : manifest.at("commands").as_array()) {
        auto const &row = entry.as_object(); contracts.emplace(std::string(row.at("id").as_string()), &row);
    }
    std::vector<std::string> mismatches;
    for (auto const &command : production_commands()) {
        auto id = std::string(command.id); auto found = contracts.find(id);
        if (found == contracts.end()) { mismatches.push_back(id + ": no manifest command row"); continue; }
        auto const &contract_errors = command.spec.input->schema.at("x-m3-contract").as_object().at("error_rows").as_array();
        auto const &manifest_errors = found->second->at("errors").as_array();
        if (manifest_errors.size() != contract_errors.size()) mismatches.push_back(id + ": error row count " + std::to_string(contract_errors.size()) + " compiled vs " + std::to_string(manifest_errors.size()) + " manifest");
        std::map<std::string, object const *> native_rows;
        for (auto const &error_value : contract_errors) {
            auto const &error = error_value.as_object();
            auto key = std::string(error.at("oracle").as_string());
            native_rows.emplace(key, &error);
        }
        for (auto const &error_value : manifest_errors) {
            auto const &error = error_value.as_object(); auto key = std::string(error.at("oracle").as_string());
            auto native = native_rows.find(key);
            if (native == native_rows.end()) { mismatches.push_back(id + ": manifest-only error row " + key); continue; }
            auto const &compiled = *native->second;
            for (auto field : {"native_branch", "code", "retryable", "mutation_state"})
                if (error.at(field) != compiled.at(field)) mismatches.push_back(id + ": " + key + " field " + field + " manifest=" + serialize(error.at(field)) + " compiled=" + serialize(compiled.at(field)));
        }
        std::set<std::string> codes_from_rows, codes_from_descriptor;
        for (auto const &row : contract_errors) codes_from_rows.emplace(row.as_object().at("code").as_string());
        for (auto code : command.errors) codes_from_descriptor.emplace(code);
        for (auto const &code : codes_from_rows) if (!codes_from_descriptor.contains(code)) mismatches.push_back(id + ": error row code absent from compiled wire error_codes: " + code);
    }
    EXPECT_TRUE(mismatches.empty()) << "M3 native reason to wire/retryability/mutation mismatch rows:" << join_mismatches(mismatches);
}

TEST(M3Contract, RequiredResultFieldsNativeEvidence) {
    using namespace Inkscape::VACardsCli;
    std::vector<std::string> mismatches;
    for (auto const &command : production_commands()) {
        auto id = std::string(command.id);
        auto const &metadata = command.spec.input->schema.at("x-m3-contract").as_object();
        object example{{"schema", "va-studio.cli-request/1"}, {"id", "example"},
                       {"command", id}, {"params", command.example}, {"document", "document-id"}};
        if (metadata.at("guard_required").as_bool()) example["if_revision"] = 0;
        if (auto error = validate_schema(example, request_schema(command.spec)))
            mismatches.push_back(id + ": versioned example request rejected at " + error->key + " (" + error->message + ")");
        auto const &variants = command.result_data.at("oneOf").as_array();
        if (variants.size() != 3) mismatches.push_back(id + ": result_data does not define success, unchanged, and computed-dry-run variants");
        for (std::size_t index = 0; index < variants.size(); ++index) {
            auto const &variant = variants[index].as_object();
            auto sample = schema_witness(variant);
            if (auto error = validate_schema(sample, variant))
                mismatches.push_back(id + ": result_data variant " + std::to_string(index) + " has no valid witness at " + error->key + " (" + error->message + ")");
        }
    }
    EXPECT_TRUE(mismatches.empty()) << "M3 example/result_data mismatch rows:" << join_mismatches(mismatches);
}
TEST(M3Contract, RequiredErrorManifestNativeCoverage) {
    using namespace Inkscape::VACardsCli;
    auto manifest = m3_manifest().as_object();
    std::map<std::string, object const *> manifest_commands;
    for (auto const &entry : manifest.at("commands").as_array()) {
        auto const &row = entry.as_object();
        if (!manifest_commands.emplace(std::string(row.at("id").as_string()), &row).second)
            ADD_FAILURE() << "M3 manifest duplicate command row: " << row.at("id").as_string();
    }
    std::set<std::string> compiled_ids;
    std::vector<std::string> mismatches;
    for (auto const &command : production_commands()) {
        auto id = std::string(command.id); compiled_ids.insert(id);
        auto found = manifest_commands.find(id);
        if (found == manifest_commands.end()) { mismatches.push_back(id + ": missing manifest row"); continue; }
        auto const &manifest_row = *found->second;
        auto const &metadata = command.spec.input->schema.at("x-m3-contract").as_object();
        auto compare = [&](char const *field, value const &compiled) {
            if (!manifest_row.contains(field) || manifest_row.at(field) != compiled)
                mismatches.push_back(id + ": manifest/compiled descriptor field differs: " + field);
        };
        compare("data_variants", command.result_data);
        compare("guard_domain", metadata.at("guard_domain"));
        compare("guard_required", metadata.at("guard_required"));
        compare("native_service", metadata.at("native_service"));
        compare("warnings", metadata.at("warnings"));
        compare("errors", metadata.at("error_rows"));
        compare("success_rows", metadata.at("success_rows"));
        object target;
        for (auto field : {"selection_mode", "target_cardinality", "normalization", "partial_policy", "role_order"})
            if (auto value = metadata.if_contains(field)) target[field] = *value;
        compare("target", target);
    }
    for (auto const &[id, _] : manifest_commands) if (!compiled_ids.contains(id)) mismatches.push_back(id + ": manifest-only command row");
    EXPECT_TRUE(mismatches.empty()) << "M3 manifest/compiled descriptor mismatch rows:" << join_mismatches(mismatches);
}
TEST(M3Contract, StructuralVariantAndMetadataCoverage) {
    using namespace Inkscape::VACardsCli;
    for (auto const &c : production_commands()) {
        auto const &meta = c.spec.input->schema.at("x-m3-contract").as_object();
        EXPECT_TRUE(meta.contains("guard_domain")); EXPECT_TRUE(meta.contains("guard_required"));
        EXPECT_TRUE(meta.contains("selection_mode")); EXPECT_FALSE(meta.at("error_rows").as_array().empty());
        auto const &variants = c.result_data.at("oneOf").as_array(); ASSERT_EQ(variants.size(), 3u);
        for (auto const &v : variants) {
            auto const &shape = v.as_object(); EXPECT_GT(shape.at("required").as_array().size(), 1u);
            EXPECT_FALSE(shape.at("additionalProperties").as_bool());
        }
    }
}

TEST(M3Integration, NativeClipSettlementAndNestRefusalThroughSharedDispatch) {
    using namespace Inkscape; using namespace VACardsCli; using namespace boost::json;
    if (!Application::exists()) Application::create(false, Application::RuntimePolicy::PreviewHelper);
    std::string svg=R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect id="target" width="40" height="40"/><path id="cutter" d="M10,10 H30 V30 H10 Z"/></svg>)";
    FileState files; files.document=SPDocument::createNewDocFromMem(std::span<char const>(svg.data(),svg.size()));
    ASSERT_TRUE(files.document); auto &doc=*files.document; doc.ensureUpToDate();
    doc.getSelection()->set(cast<SPItem>(doc.getObjectById("target")));
    DocumentUndo::clearUndo(&doc); DocumentUndo::clearRedo(&doc);
    auto xml=sp_repr_save_buf(doc.getReprDoc()).raw(); auto stamp=document_stamp(&doc);
    DispatchContext context; context.document=&doc; context.selection=doc.getSelection(); context.session_revision=7;
    unsigned selection_events=0;
    auto selection_connection=doc.getSelection()->connectChanged([&](Selection *) { ++selection_events; });
    Grants grants; TokenStore tokens; Nesting::CliSession nest;
    context.production_handler=[&](Request const &r) {
        EditServices edits{doc,*context.selection,document_stamp(&doc)};
        ProductionContext production{files,grants,edits,tokens,[]{},"integration-session","integration-catalog",1,1};
        production.nest=&nest; return execute_production(r,context,production);
    };
    Request request; request.id="clip"; request.command="clip.set"; request.document=stamp.id;
    request.if_revision=stamp.revision; request.params={{"target-id","target"},{"cutter-id","cutter"}}; request.dry_run=true;
    auto preview=dispatch(request,context); ASSERT_EQ(preview.status,Status::Ok)<<preview.message;
    EXPECT_EQ(sp_repr_save_buf(doc.getReprDoc()).raw(),xml); EXPECT_EQ(context.selection->size(),1u);
    EXPECT_EQ(context.session_revision,7u); EXPECT_EQ(document_stamp(&doc).revision,stamp.revision);
    request.dry_run=false; auto changed=dispatch(request,context);
    ASSERT_EQ(changed.status,Status::Changed)<<changed.message; EXPECT_TRUE(changed.one_undo_step);
    ASSERT_EQ(context.selection->size(),1u); EXPECT_STREQ(context.selection->singleItem()->getId(),"target");
    EXPECT_EQ(context.session_revision,7u); EXPECT_TRUE(cast<SPItem>(doc.getObjectById("target"))->getClipObject());
    EXPECT_EQ(selection_events,0u);
    selection_connection.disconnect();
    auto committed=sp_repr_save_buf(doc.getReprDoc()).raw();
    ASSERT_TRUE(DocumentUndo::undo(&doc)); EXPECT_EQ(sp_repr_save_buf(doc.getReprDoc()).raw(),xml);
    ASSERT_TRUE(DocumentUndo::redo(&doc)); EXPECT_EQ(sp_repr_save_buf(doc.getReprDoc()).raw(),committed);
    for (auto const &c: production_commands()) if (c.id=="nest.solve") request.params=c.example;
    request.command="nest.solve"; request.if_revision=document_stamp(&doc).revision;
    auto refused=dispatch(request,context); EXPECT_EQ(refused.reason,"invalid-token")<<refused.message;
    EXPECT_EQ(sp_repr_save_buf(doc.getReprDoc()).raw(),committed); EXPECT_EQ(context.session_revision,7u);
    EXPECT_TRUE(tokens.snapshot().active_tokens.empty()); EXPECT_TRUE(m3_accepted);
}

TEST(M3DevelopmentOverlay, CatalogIsUniqueExecutableAndUnaccepted) {
    using namespace Inkscape::VACardsCli;
    auto catalog = production_catalog();
    std::set<std::string> ids;
    unsigned overlay = 0;
    for (auto const &v : catalog.at("commands").as_array()) {
        auto const &row = v.as_object();
        auto id = std::string(row.at("id").as_string());
        EXPECT_TRUE(ids.insert(id).second);
        if (!find_production_command(id)) continue;
        ++overlay;
        EXPECT_TRUE(row.at("available").as_bool());
        EXPECT_FALSE(row.at("experimental").as_bool());
        EXPECT_TRUE(row.at("accepted").as_bool());
        EXPECT_EQ(row.at("request_schema"), request_schema(*find_production_command(id)));
        EXPECT_TRUE(row.at("result_schema").as_object().contains("anyOf"));
    }
    EXPECT_EQ(overlay, 31u); EXPECT_TRUE(m3_accepted); EXPECT_TRUE(m4_accepted);
    EXPECT_EQ(find_command("nest.solve"), nullptr);
    EXPECT_TRUE(find_command("system.catalog"));
}
