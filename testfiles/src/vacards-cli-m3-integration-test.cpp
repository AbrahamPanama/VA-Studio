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
#include <cstdlib>
#include "ui/explode-bitmap-context.h"
#include "ui/bitmap-tone-member-targets.h"
#include "nesting/nesting-cli-service.h"
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
    EXPECT_FALSE(m3_accepted); EXPECT_FALSE(m4_accepted);
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
    EXPECT_TRUE(tokens.snapshot().active_tokens.empty()); EXPECT_FALSE(m3_accepted);
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
    if (!Inkscape::VACardsCli::m3_accepted) GTEST_SKIP() << "M3 ships experimental (m3_accepted=false); this acceptance obligation is pending.";
    FAIL() << "INT/P9 must prove all version-2 schema branches and guarded admission before enabling M3.";
}
TEST(M3Result, RequiredNativeErrorAndResultMappings) {
    if (!Inkscape::VACardsCli::m3_accepted) GTEST_SKIP() << "M3 ships experimental (m3_accepted=false); this acceptance obligation is pending.";
    FAIL() << "INT/P5/P6/P7/P9 must prove native code mappings and every command result before acceptance.";
}

TEST(M3Contract, RequiredResultFieldsNativeEvidence) {
    if (!Inkscape::VACardsCli::m3_accepted) GTEST_SKIP() << "M3 ships experimental (m3_accepted=false); this acceptance obligation is pending.";
    FAIL() << "P5/P6/P7/P9: every required success/unchanged/computed-preview field from real native outcomes.";
}
TEST(M3Contract, RequiredErrorManifestNativeCoverage) {
    if (!Inkscape::VACardsCli::m3_accepted) GTEST_SKIP() << "M3 ships experimental (m3_accepted=false); this acceptance obligation is pending.";
    FAIL() << "INT/P9: every manifest row requires independent native branch evidence; removals are not passes.";
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
    EXPECT_TRUE(tokens.snapshot().active_tokens.empty()); EXPECT_FALSE(m3_accepted);
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
        EXPECT_TRUE(row.at("experimental").as_bool());
        EXPECT_FALSE(row.at("accepted").as_bool());
        EXPECT_EQ(row.at("request_schema"), request_schema(*find_production_command(id)));
        EXPECT_TRUE(row.at("result_schema").as_object().contains("anyOf"));
    }
    EXPECT_EQ(overlay, 31u); EXPECT_FALSE(m3_accepted); EXPECT_FALSE(m4_accepted);
    EXPECT_EQ(find_command("nest.solve"), nullptr);
    EXPECT_TRUE(find_command("system.catalog"));
}
