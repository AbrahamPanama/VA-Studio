// SPDX-License-Identifier: GPL-2.0-or-later
#include "actions-vacards-file.h"
#include "actions-vacards-cli.h"
#include <boost/json.hpp>
void add_actions_vacards_file(InkscapeApplication *) {}
namespace Inkscape::VACardsCli {
namespace {
using namespace boost::json;
object closed(object p, array r = {}) { return {{"type","object"},{"properties",p},{"required",r},{"additionalProperties",false}}; }
object scalar(char const *s) { return {{"type",s}}; }
object length() { return closed({{"value",scalar("number")},{"unit",{{"enum",{"px","mm","cm","in","pt","pc"}}}}},{"value","unit"}); }
TypeDescriptor const version{closed({{"identity",scalar("string")},{"sha256",{{"type","string"},{"minLength",64},{"maxLength",64}}},{"bytes",{{"type","integer"},{"minimum",0}}}},{"identity","sha256","bytes"})};
TypeDescriptor const pages{{{"type","array"},{"items",{{"type","integer"},{"minimum",1},{"maximum",10000}}},{"minItems",1},{"maxItems",1000},{"uniqueItems",true}}};
TypeDescriptor const ids{{{"type","array"},{"items",{{"type","string"},{"minLength",1}}},{"minItems",1},{"maxItems",1000},{"uniqueItems",true}}};
TypeDescriptor const position{closed({{"x",length()},{"y",length()}},{"x","y"})};
TypeDescriptor const area{closed({{"x",length()},{"y",length()},{"width",length()},{"height",length()}},{"x","y","width","height"})};
TypeDescriptor const profile{closed({{"id",{{"enum",{"default","srgb","file"}}}},{"path",scalar("string")},{"sha256",{{"type","string"},{"minLength",64},{"maxLength",64}}}},{"id"})};
TypeDescriptor const rgba{{{"type","array"},{"items",{{"type","number"},{"minimum",0},{"maximum",1}}},{"minItems",4},{"maxItems",4}}};
TypeDescriptor const reconcile{closed({{"destination",scalar("string")},{"observed-version",version.schema},{"acknowledge",{{"const",true}}}},{"destination","observed-version","acknowledge"})};
constexpr std::string_view formats[]{"auto","svg","svgz","cdr","pdf","png","jpeg","tiff"};
constexpr std::string_view outputs[]{"svg","png","pdf","tiff"};
constexpr std::string_view resources[]{"embed","reject-external"};
constexpr std::string_view fonts[]{"reject","substitute"};
constexpr std::string_view text[]{"preserve","paths"};
#define DISCARD {.key="discard",.type=ParamType::Boolean,.default_value="false"}
#define LOAD {.key="path",.type=ParamType::Text,.required=true}, \
 {.key="format",.type=ParamType::Choice,.default_value="auto",.choices=formats}, \
 {.key="pages",.descriptor=&pages}, \
 {.key="resource-policy",.type=ParamType::Choice,.required=true,.choices=resources}, \
 {.key="font-policy",.type=ParamType::Choice,.required=true,.choices=fonts}
#define WRITE {.key="path",.type=ParamType::Text,.required=true}, \
 {.key="overwrite",.type=ParamType::Boolean,.default_value="false"}, \
 {.key="expected-version",.descriptor=&version}
ParamSpec const new_params[]{
 {.key="width",.type=ParamType::Length,.required=true,.min=0.000001,.max=1000000},
 {.key="height",.type=ParamType::Length,.required=true,.min=0.000001,.max=1000000},DISCARD};
ParamSpec const open_params[]{LOAD,DISCARD};
ParamSpec const close_params[]{DISCARD,{.key="reconcile",.descriptor=&reconcile}};
ParamSpec const import_params[]{LOAD,{.key="position",.required=true,.descriptor=&position}};
ParamSpec const save_params[]{WRITE,{.key="embedding-policy",.type=ParamType::Choice,.required=true,.choices=resources}};
ParamSpec const export_params[]{WRITE,
 {.key="format",.type=ParamType::Choice,.required=true,.choices=outputs},
 {.key="page",.type=ParamType::Integer,.min=1,.max=10000},
 {.key="ids",.type=ParamType::List,.descriptor=&ids}, {.key="area",.descriptor=&area}, {.key="drawing",.type=ParamType::Boolean},
 {.key="dpi",.type=ParamType::Number,.default_value="96",.min=1,.max=9600},
 {.key="background",.descriptor=&rgba},{.key="profile",.descriptor=&profile},
 {.key="text-policy",.type=ParamType::Choice,.default_value="preserve",.choices=text}};
#undef LOAD
#undef WRITE
#undef DISCARD
void typed_only(ActionContext &c) {
    c.record.status=Status::Rejected; c.record.reason="session-required";
    c.record.message="File operations require the validated agent-owned file service.";
    c.record.error=ParseError{"session-required",{},c.record.message};
}
object file_result_schema(std::string_view name)
{
    auto reported_length=closed({{"px",scalar("number")},{"preferred",length()}},{"px","preferred"});
    object props{{"validation_level",{{"enum",{"preflight","computed"}}}}};
    if (name=="file.new" || name=="file.open") {
        props["document_id"]=scalar("string"); props["revision"]=scalar("integer");
        props["session_revision"]=scalar("integer"); props["dirty"]=scalar("boolean");
    }
    if (name=="file.new") {
        props["width"]=reported_length;props["height"]=reported_length;props["units"]=scalar("string");
    }
    if (name=="file.close") {
        props["closed"]=scalar("boolean");props["discarded"]=scalar("boolean");props["session_revision"]=scalar("integer");
    }
    if (name=="file.open" || name=="file.import") {
        // The intake report shares M1's resource and normalization vocabulary.
        // BUG-025 admits up to one million objects; the response-byte cap remains independent.
        auto rows=[](object row) {return object{{"type","array"},{"items",row},{"maxItems",1000000}};};
        props["intake"]=closed({{"format",scalar("string")},{"source_bytes",scalar("integer")},
          {"source_sha256",scalar("string")},{"title",scalar("string")},{"metadata",scalar("string")},
          {"conversion",scalar("boolean")},{"dirty",scalar("boolean")},
          {"id_map",rows(closed({{"id",scalar("string")},{"source_index",{{"anyOf",array{scalar("integer"),scalar("null")}}}},
            {"original_id",{{"anyOf",array{scalar("string"),scalar("null")}}}}}))},
          {"resources",rows(closed({{"id",scalar("string")},{"role",scalar("string")},{"href",scalar("string")},
            {"state",scalar("string")},{"loaded",scalar("boolean")},{"mime",scalar("string")},{"reason",scalar("string")}}))},
          {"normalization",rows(closed({{"id",scalar("string")},{"reason",scalar("string")},{"attribute",scalar("string")}}))},
          {"fonts",rows(closed({{"family",scalar("string")},{"substituted",scalar("boolean")}}))}});
    }
    if (name=="file.open") props["source_version"]=version.schema;
    if (name=="file.import") {
        props["inserted_ids"]=ids.schema;
        props["root_affines"]={{"type","array"},{"items",closed({{"id",scalar("string")},
          {"affine",{{"type","array"},{"items",scalar("number")},{"minItems",6},{"maxItems",6}}}})}};
        props["exclusions"]={{"type","array"},{"items",closed({{"id",scalar("string")},{"reason",scalar("string")}})}};
    }
    if (name=="file.save" || name=="file.export") {
        props["destination_version"]=version.schema;props["bytes"]=scalar("integer");props["sha256"]=scalar("string");
        props["format"]=scalar("string"); props["dirty"]=scalar("boolean");
        props["publication"]=closed({{"outcome",{{"enum",{"Published","Conflict","Unsupported","Uncertain","Failed"}}}},
          {"destination",scalar("string")},{"error",scalar("string")},{"cleanup_ok",scalar("boolean")},
          {"recovery_path",scalar("string")},{"recovery_availability",scalar("string")},
          {"durability",closed({{"file_sync",scalar("string")},{"parent_sync",scalar("string")},
            {"file_sync_attempted",scalar("boolean")},{"file_sync_supported",scalar("boolean")},{"file_sync_ok",scalar("boolean")},
            {"parent_sync_attempted",scalar("boolean")},{"parent_sync_supported",scalar("boolean")},{"parent_sync_ok",scalar("boolean")}})}});
    }
    if (name=="file.save") props["saved_revision"]=scalar("integer");
    if (name=="file.export") {
        props["output_width"]=scalar("integer");props["output_height"]=scalar("integer");
        props["profile"]=closed({{"id",scalar("string")},{"name",scalar("string")},{"sha256",scalar("string")},{"notice",scalar("string")}});
    }
    return closed(props);
}

}
std::vector<PackageCommand> file_commands()
{
    std::vector<PackageCommand> out;
    auto add = [&](std::string_view name, std::span<ParamSpec const> params, std::string_view effect,
                   std::string_view policy, bool document, object example) {
        ActionSpec s{.name=name,.mode=policy == "G" ? "collective-geometry" : "document-root",
            .summary=name=="file.new" ? "Create a new editable document with typed page dimensions." :
                name=="file.open" ? "Open a granted source as an editable document." :
                name=="file.close" ? "Close the active document with explicit dirty-state and reconciliation policy." :
                name=="file.import" ? "Import granted content collectively with one native Undo step." :
                name=="file.save" ? "Publish an editable SVG snapshot through guarded native file services." :
                "Export one explicit scope through native rendering and guarded publication.",.params=params,.canonical_id=name,.handler=typed_only,
            .effects=effect,.target_policy=policy,.needs_document=document};
        out.push_back({s,name,effect,policy,example,file_result_schema(name),
          {"dirty-document","read-grant-denied","resource-denied","remote-resource","invalid-file","input-too-large",
           "format-unsupported","pages-invalid","font-policy-required","stale-dependency","write-grant-denied",
           "unsafe-destination","expected-version-required","publication-conflict","publication-unsupported",
           "publication-uncertain","publication-failed","writes-blocked","reconciliation-required",
           "document-read-only","import-failed","save-failed","invalid-target","profile-invalid","profile-unsupported",
           "export-failed","invalid-path","missing-file","read-failed","invalid-xml","unsafe-xml","invalid-svgz",
           "duplicate-id","unsafe-reference","invalid-svg","intake-failed","session-required"},
          {"profile-fallback","font-substitution","export-loss","publication-cleanup"}});
    };
    object l{{"value",100},{"unit","mm"}};
    object load{{"path","/tmp/sheet.svg"},{"resource-policy","embed"},{"font-policy","reject"}};
    add("file.new",new_params,"session-state","S",false,{{"width",l},{"height",l}});
    add("file.open",open_params,"session-state","S",false,load);
    add("file.close",close_params,"session-state","S",false,{});
    load["position"] = object{{"x",l},{"y",l}};
    add("file.import",import_params,"document-edit","G",true,load);
    add("file.save",save_params,"file-publication","I",true,{{"path","/tmp/sheet.svg"},{"embedding-policy","embed"}});
    add("file.export",export_params,"file-publication","I",true,{{"path","/tmp/sheet.png"},{"format","png"},{"drawing",true}});
    return out;
}
}
