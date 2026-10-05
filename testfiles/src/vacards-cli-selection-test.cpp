// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "actions/vacards-cli-production.h"
#include "actions/vacards-cli-edit-services.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "selection.h"
#include "object/sp-item.h"
#include "xml/node.h"
#include <glibmm/i18n.h>
#include <boost/json.hpp>
#include "xml/attribute-record.h"
#include <algorithm>
using namespace Inkscape;
using namespace Inkscape::VACardsCli;
namespace {
std::string canonical(XML::Node const *node)
{
    auto field = [](char const *v) { std::string s = v ? v : ""; return std::to_string(s.size()) + ':' + s; };
    std::string result = std::to_string(static_cast<int>(node->type())) + field(node->name()) + field(node->content());
    std::vector<std::pair<std::string, std::string>> attrs;
    for (auto const &attr : node->attributeList()) attrs.emplace_back(g_quark_to_string(attr.key), attr.value.pointer());
    std::sort(attrs.begin(), attrs.end());
    for (auto const &[key, value] : attrs) result += field(key.c_str()) + field(value.c_str());
    for (auto *child = node->firstChild(); child; child = child->next()) result += canonical(child);
    return result + '/';
}
class M3HistorySnapshot : public testing::Test {
protected:
    void SetUp() override {
        if (!Application::exists()) Application::create(false);
        doc = SPDocument::createNewDocFromMem(
            "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
            "<defs id='defs'/><g id='group'><rect id='a' width='20' height='20'/></g>"
            "<rect id='b' x='40' width='10' height='10'/></svg>");
        doc->ensureUpToDate();
        DocumentUndo::setUndoSensitive(doc.get(), true);
        DocumentUndo::clearUndo(doc.get());
        DocumentUndo::clearRedo(doc.get());
    }
    HistorySnapshotResult read(std::shared_ptr<void> lease = {}) {
        EditServices services{*doc, *doc->getSelection(), document_stamp(doc.get()), {}, lease};
        return history_snapshot(services);
    }
    void edit(char const *value, bool coalesce = false) {
        doc->getObjectById("a")->getRepr()->setAttribute("x", value);
        if (coalesce) DocumentUndo::maybeDone(doc.get(), "P5-test-key", Util::Internal::ContextString("Move fixture"), "object-move");
        else DocumentUndo::done(doc.get(), Util::Internal::ContextString("Move fixture"), "object-move");
    }
    Record call(std::string command, boost::json::object params = {}, bool dry = false) {
        Request request; request.command = command; request.params = std::move(params); request.dry_run = dry;
        EditServices services{*doc, *doc->getSelection(), document_stamp(doc.get()), {}};
        context.document = doc.get(); context.selection = doc->getSelection();
        return command.starts_with("selection.") ? execute_selection(request, context, services)
                                                 : execute_history(request, context, services);
    }
    DispatchContext context;
    std::unique_ptr<SPDocument> doc;
};
TEST_F(M3HistorySnapshot, EmptyIsAReadOnlyValue) {
    auto stamp = document_stamp(doc.get());
    auto result = read();
    ASSERT_TRUE(result.value);
    EXPECT_FALSE(result.value->can_undo);
    EXPECT_FALSE(result.value->can_redo);
    EXPECT_FALSE(result.value->next_undo_label);
    EXPECT_FALSE(result.value->next_redo_label);
    EXPECT_EQ(document_stamp(doc.get()).revision, stamp.revision);
}
TEST_F(M3HistorySnapshot, GroupedRowsRemainIndividualUndoSteps) {
    edit("10"); edit("20");
    auto snapshot = read(); ASSERT_TRUE(snapshot.value);
    EXPECT_EQ(snapshot.value->next_undo_label, "Move fixture");
    EXPECT_FALSE(snapshot.value->can_redo);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    snapshot = read(); ASSERT_TRUE(snapshot.value);
    EXPECT_TRUE(snapshot.value->can_undo);
    EXPECT_TRUE(snapshot.value->can_redo);
    EXPECT_EQ(snapshot.value->next_redo_label, "Move fixture");
    EXPECT_STREQ(doc->getObjectById("a")->getRepr()->attribute("x"), "10");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    snapshot = read(); ASSERT_TRUE(snapshot.value);
    EXPECT_FALSE(snapshot.value->can_undo);
    EXPECT_TRUE(snapshot.value->can_redo);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    snapshot = read(); ASSERT_TRUE(snapshot.value);
    EXPECT_TRUE(snapshot.value->can_undo);
    EXPECT_TRUE(snapshot.value->can_redo);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    snapshot = read(); ASSERT_TRUE(snapshot.value);
    EXPECT_FALSE(snapshot.value->can_redo);
}
TEST_F(M3HistorySnapshot, KeyedEditsCoalesceIntoOneNativeStep) {
    edit("10", true); edit("20", true);
    auto snapshot = read(); ASSERT_TRUE(snapshot.value);
    EXPECT_EQ(snapshot.value->next_undo_label, "Move fixture");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(doc->getObjectById("a")->getRepr()->attribute("x"), nullptr);
    snapshot = read(); ASSERT_TRUE(snapshot.value);
    EXPECT_FALSE(snapshot.value->can_undo);
    EXPECT_TRUE(snapshot.value->can_redo);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(doc->getObjectById("a")->getRepr()->attribute("x"), "20");
}
TEST_F(M3HistorySnapshot, BusyRefusalPreservesRedoAndOwnedLeaseIsReadable) {
    edit("10"); ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    auto revision = document_stamp(doc.get()).revision;
    {
        auto lease = DocumentUndo::holdInteractionOperation(doc.get());
        auto busy = read();
        ASSERT_TRUE(busy.error);
        EXPECT_EQ(busy.error->code, "document-busy");
        auto owned = read(lease); ASSERT_TRUE(owned.value);
        EXPECT_TRUE(owned.value->can_redo);
        auto interaction = DocumentUndo::beginAtomicCommandInteraction(doc.get(), lease);
        ASSERT_TRUE(interaction);
        busy = read(lease); ASSERT_TRUE(busy.error);
        EXPECT_EQ(busy.error->code, "document-busy");
    }
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    auto snapshot = read(); ASSERT_TRUE(snapshot.value);
    EXPECT_TRUE(snapshot.value->can_redo);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(doc->getObjectById("a")->getRepr()->attribute("x"), "10");
}

TEST_F(M3HistorySnapshot, SelectionSetNormalizesWithoutDocumentHistory) {
    edit("10"); ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    auto revision = document_stamp(doc.get()).revision;
    auto params = boost::json::object{{"ids", boost::json::array{"a", "b", "group"}}};
    auto preview = call("selection.set", params, true);
    ASSERT_EQ(preview.status, Status::Ok);
    EXPECT_TRUE(doc->getSelection()->isEmpty());
    EXPECT_TRUE(preview.selection_after.empty());
    EXPECT_EQ(preview.data.at("normalized-ids"), (boost::json::array{"b", "group"}));
    EXPECT_EQ(preview.data.at("covered-ids"), (boost::json::array{"a"}));
    EXPECT_EQ(preview.data.at("session-revision"), 1);
    auto changed = call("selection.set", params);
    EXPECT_EQ(changed.status, Status::Changed);
    EXPECT_EQ(changed.selection_after, (std::vector<std::string>{"b", "group"}));
    EXPECT_EQ(context.session_revision, 0u); // Integration, not this adapter, owns increment.
    EXPECT_FALSE(changed.one_undo_step);
    auto same = call("selection.set", params);
    EXPECT_EQ(same.status, Status::Unchanged);
    auto bad = call("selection.set", {{"ids", boost::json::array{"missing"}}});
    EXPECT_EQ(bad.status, Status::Rejected);
    EXPECT_EQ(bad.reason, "unknown-id");
    EXPECT_EQ(doc->getSelection()->size(), 2u);
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    auto snapshot = read(); ASSERT_TRUE(snapshot.value);
    EXPECT_TRUE(snapshot.value->can_redo);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(doc->getObjectById("a")->getRepr()->attribute("x"), "10");
}
TEST_F(M3HistorySnapshot, SelectionClearAndEmptySetHaveSameNoUndoSemantics) {
    doc->getSelection()->set(doc->getObjectById("b"));
    edit("10"); ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    auto revision = document_stamp(doc.get()).revision;
    auto preview = call("selection.clear", {}, true);
    EXPECT_EQ(preview.status, Status::Ok);
    EXPECT_EQ(preview.data.at("cleared-count"), 1);
    EXPECT_EQ(doc->getSelection()->size(), 1u);
    context.cancelled = [] { return true; };
    EXPECT_EQ(call("selection.clear").status, Status::Cancelled);
    EXPECT_EQ(doc->getSelection()->size(), 1u);
    context.cancelled = {};
    auto changed = call("selection.clear");
    EXPECT_EQ(changed.status, Status::Changed);
    EXPECT_TRUE(doc->getSelection()->isEmpty());
    EXPECT_EQ(call("selection.clear").status, Status::Unchanged);
    EXPECT_EQ(call("selection.set", {{"ids", boost::json::array{}}}).status, Status::Unchanged);
    doc->getSelection()->set(doc->getObjectById("b"));
    EXPECT_EQ(call("selection.set", {{"ids", boost::json::array{}}}).status, Status::Changed);
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(doc->getObjectById("a")->getRepr()->attribute("x"), "10");
}
TEST_F(M3HistorySnapshot, HistoryQueryReportsNativeLabelsWithoutMutation) {
    auto empty = call("history.query");
    EXPECT_EQ(empty.status, Status::Ok);
    EXPECT_EQ(empty.data.at("variant"), "success"); // queries never unchanged
    edit("10");
    auto revision = document_stamp(doc.get()).revision;
    auto query = call("history.query", {}, true);
    EXPECT_EQ(query.data.at("variant"), "computed-dry-run");
    EXPECT_EQ(query.data.at("next-undo-label"), "Move fixture");
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    {
        auto lease = DocumentUndo::holdInteractionOperation(doc.get());
        auto refusal = call("history.query");
        EXPECT_EQ(refusal.status, Status::Rejected);
        EXPECT_EQ(refusal.reason, "document-busy");
    }
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    query = call("history.query");
    EXPECT_EQ(query.data.at("next-redo-label"), "Move fixture");
    EXPECT_EQ(query.data.at("next-undo-label"), nullptr);
}
TEST_F(M3HistorySnapshot, HistoryUndoHasSuccessDryRunEmptyAndBusyOutcomes) {
    EXPECT_EQ(call("history.undo").reason, "history-empty");
    edit("10");
    auto revision = document_stamp(doc.get()).revision;
    auto preview = call("history.undo", {}, true);
    EXPECT_EQ(preview.status, Status::Ok);
    EXPECT_EQ(preview.data.at("would-undo"), true);
    EXPECT_STREQ(doc->getObjectById("a")->getRepr()->attribute("x"), "10");
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    {
        auto lease = DocumentUndo::holdInteractionOperation(doc.get());
        EXPECT_EQ(call("history.undo").reason, "document-busy");
    }
    auto undo = call("history.undo");
    EXPECT_EQ(undo.status, Status::Changed);
    EXPECT_EQ(undo.undo_effect, "undo");
    EXPECT_EQ(undo.data.at("consumed-label"), "Move fixture");
    EXPECT_EQ(undo.revision_after, revision + 1);
    EXPECT_EQ(doc->getObjectById("a")->getRepr()->attribute("x"), nullptr);
    EXPECT_EQ(call("history.undo").reason, "history-empty");
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(doc->getObjectById("a")->getRepr()->attribute("x"), "10");
}
TEST_F(M3HistorySnapshot, HistoryRedoHasSuccessDryRunEmptyAndBusyOutcomes) {
    EXPECT_EQ(call("history.redo").reason, "history-empty");
    edit("10"); ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    auto revision = document_stamp(doc.get()).revision;
    auto preview = call("history.redo", {}, true);
    EXPECT_EQ(preview.status, Status::Ok);
    EXPECT_EQ(preview.data.at("would-redo"), true);
    EXPECT_EQ(doc->getObjectById("a")->getRepr()->attribute("x"), nullptr);
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    {
        auto lease = DocumentUndo::holdInteractionOperation(doc.get());
        EXPECT_EQ(call("history.redo").reason, "document-busy");
    }
    auto redo = call("history.redo");
    EXPECT_EQ(redo.status, Status::Changed);
    EXPECT_EQ(redo.undo_effect, "redo");
    EXPECT_EQ(redo.data.at("consumed-label"), "Move fixture");
    EXPECT_EQ(redo.revision_after, revision + 1);
    EXPECT_STREQ(doc->getObjectById("a")->getRepr()->attribute("x"), "10");
    EXPECT_EQ(call("history.redo").reason, "history-empty");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(doc->getObjectById("a")->getRepr()->attribute("x"), nullptr);
}

TEST_F(M3HistorySnapshot, SelectionProtectedAndNonRenderingRefusalsPreserveExactState) {
    doc->getObjectById("b")->getRepr()->setAttribute("style", "display:none");
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Hide fixture"), "object-hide");
    edit("10"); ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->getSelection()->set(doc->getObjectById("a"));
    auto before = canonical(doc->getReprRoot()); auto revision = document_stamp(doc.get()).revision;
    for (bool dry : {false, true}) {
        auto refused = call("selection.set", {{"ids", boost::json::array{"a", "b"}}}, dry);
        EXPECT_EQ(refused.reason, "unavailable");
        EXPECT_EQ(refused.status, Status::Rejected);
        EXPECT_EQ(canonical(doc->getReprRoot()), before);
        EXPECT_EQ(doc->getSelection()->items_vector()[0]->getId(), std::string("a"));
    }
    auto nonrendering = call("selection.set", {{"ids", boost::json::array{"defs"}}});
    EXPECT_EQ(nonrendering.reason, "unknown-id");
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
    auto preview = call("selection.set", {{"ids", boost::json::array{"group"}}}, true);
    EXPECT_EQ(preview.status, Status::Ok);
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    EXPECT_EQ(context.session_revision, 0u);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(doc->getObjectById("a")->getRepr()->attribute("x"), "10");
}
TEST_F(M3HistorySnapshot, ReadOnlyQueryAndBothHistoryPreviewsPreserveExactXml) {
    edit("10");
    auto before = canonical(doc->getReprRoot()); auto revision = document_stamp(doc.get()).revision;
    EXPECT_EQ(call("history.query").status, Status::Ok);
    EXPECT_EQ(call("history.undo", {}, true).status, Status::Ok);
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    before = canonical(doc->getReprRoot()); revision = document_stamp(doc.get()).revision;
    EXPECT_EQ(call("history.redo", {}, true).status, Status::Ok);
    EXPECT_EQ(canonical(doc->getReprRoot()), before);
    EXPECT_EQ(document_stamp(doc.get()).revision, revision);
    EXPECT_EQ(context.session_revision, 0u);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
}
} // namespace

// Literal independent oracles, never computed from service output.
#include <filesystem>
#include <fstream>
#include <iostream>
namespace {
class P9Independent : public testing::Test {
protected:
    struct Payload : TokenPayload { TokenKind kind() const noexcept override { return TokenKind::NestAnalysis; } };
    void SetUp() override {
        if (!Application::exists()) Application::create(false);
        auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "cli_tests/vacards-agent/fixtures/m3/geometry.svg";
        std::ifstream stream(path); std::string svg((std::istreambuf_iterator<char>(stream)), {});
        ASSERT_FALSE(svg.empty());
        doc = SPDocument::createNewDocFromMem(svg); ASSERT_TRUE(doc);
        doc->ensureUpToDate(); DocumentUndo::setUndoSensitive(doc.get(), true);
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
        context.document = doc.get(); context.selection = doc->getSelection(); context.session_revision = 17;
        doc->getSelection()->set(doc->getObjectById("untouched"));
        TokenBinding binding; binding.session="P9"; binding.document=document_stamp(doc.get()).id;
        binding.engine="native"; binding.catalog_hash="P9-independent"; binding.incarnation=1;
        auto retained=tokens.retain(TokenKind::NestAnalysis,binding,std::make_shared<Payload>(),
            {{"P9-held-buffer",37,std::make_shared<std::vector<unsigned char>>(37,9)}});
        ASSERT_TRUE(retained.value); token_id=retained.value->id;
    }
    void TearDown() override { DocumentUndo::setAtomicSettlementFaultForTesting(nullptr); }
    boost::json::array selection() {
        boost::json::array a; for(auto *item:doc->getSelection()->items()) a.emplace_back(item->getId()); return a;
    }
    boost::json::object state() {
        EditServices e{*doc,*doc->getSelection(),document_stamp(doc.get()),{},lease};
        auto h=history_snapshot(e); EXPECT_TRUE(h.value);
        auto t=tokens.snapshot(); boost::json::array ids; for(auto const &id:t.active_tokens) ids.emplace_back(id);
        boost::json::object s{{"xml",canonical(doc->getReprRoot())},{"selection",selection()},
            {"document_revision",document_stamp(doc.get()).revision},{"session_revision",context.session_revision},
            {"tokens",boost::json::object{{"ids",ids},{"roots",t.active_roots},{"held_bytes",t.charged_bytes}}}};
        if(h.value) {
            s["undo"]=boost::json::object{{"available",h.value->can_undo},{"label",h.value->next_undo_label?boost::json::value(*h.value->next_undo_label):boost::json::value(nullptr)}};
            s["redo"]=boost::json::object{{"available",h.value->can_redo},{"label",h.value->next_redo_label?boost::json::value(*h.value->next_redo_label):boost::json::value(nullptr)}};
        }
        return s;
    }
    Request request(std::string const &command, boost::json::object params) {
        auto parsed=parse_production_request(boost::json::serialize(boost::json::object{
            {"schema","va-studio.cli-request/1"},{"id","P9"},{"command",command},{"params",params},
            {"document",document_stamp(doc.get()).id},{"if_revision",command.starts_with("selection.")?context.session_revision:document_stamp(doc.get()).revision}}));
        EXPECT_TRUE(parsed.request) << (parsed.error?parsed.error->message:"");
        return parsed.request ? *parsed.request : Request{};
    }
    Record call(Request r, bool dry=false) {
        r.dry_run=dry;
        EditServices e{*doc,*doc->getSelection(),document_stamp(doc.get()),{}};
        return r.command.starts_with("selection.")?execute_selection(r,context,e):
            r.command.starts_with("history.")?execute_history(r,context,e):execute_geometry(r,context,e);
    }
    void seed_redo() {
        // Fixture mutations must survive the unrelated edit's Undo.
        DocumentUndo::done(doc.get(),Util::Internal::ContextString("P9 fixture setup"),"object-move");
        DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
        doc->getObjectById("untouched")->getRepr()->setAttribute("x","151");
        DocumentUndo::done(doc.get(),Util::Internal::ContextString("P9 prior edit"),"object-move");
        ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    }
    void subset(boost::json::value const &actual, boost::json::value const &expected) {
        if(expected.is_object()) { ASSERT_TRUE(actual.is_object()); for(auto const &v:expected.as_object()) {
            ASSERT_TRUE(actual.as_object().contains(v.key())) << v.key(); subset(actual.as_object().at(v.key()),v.value()); } }
        else if(expected.is_array()) { ASSERT_TRUE(actual.is_array()); ASSERT_EQ(actual.as_array().size(),expected.as_array().size());
            for(size_t i=0;i<expected.as_array().size();++i) subset(actual.as_array()[i],expected.as_array()[i]); }
        else if(expected.is_number()) { ASSERT_TRUE(actual.is_number()); EXPECT_NEAR(actual.to_number<double>(),expected.to_number<double>(),1e-6); }
        else EXPECT_EQ(actual,expected);
    }
    void independent(std::string const &command, boost::json::object p, boost::json::object expected) {
        auto r=request(command,p); ASSERT_FALSE(r.command.empty());
        doc->getSelection()->set(doc->getObjectById("untouched"));
        if(command=="selection.clear") doc->getSelection()->set(doc->getObjectById("shape1"));
        auto untouched=canonical(doc->getObjectById("untouched")->getRepr());
        if(command=="history.undo" || command=="history.redo") {
            auto move=request("geometry.move",boost::json::parse(R"({"ids":["shape1"],"dx":{"value":2,"unit":"px"},"dy":{"value":3,"unit":"px"}})").as_object());
            ASSERT_EQ(call(move).status,Status::Changed);
            if(command=="history.redo") ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        } else if(command!="history.query") seed_redo();
        auto before=state();
        auto preview=call(r,true); ASSERT_EQ(preview.status,Status::Ok) << preview.reason;
        auto preview_after=state(); EXPECT_EQ(preview_after,before);
        context.cancelled=[] {return true;}; auto cancel=call(r); context.cancelled={};
        ASSERT_EQ(cancel.status,Status::Cancelled); auto cancel_after=state(); EXPECT_EQ(cancel_after,before);
        auto result=call(r); ASSERT_EQ(result.status,command=="history.query"?Status::Ok:Status::Changed) << result.reason;
        subset(result.data,expected);
        auto after=state();
        EXPECT_EQ(after.at("tokens"),before.at("tokens")); EXPECT_EQ(context.session_revision,17u);
        EXPECT_EQ(canonical(doc->getObjectById("untouched")->getRepr()),untouched);
        EXPECT_TRUE(DocumentUndo::fileOperationFreshReady(doc.get()));
        if(command.starts_with("geometry.")) {
            auto actual_bounds=[&](std::string const &id, boost::json::value const &bounds) {
                auto *item=cast<SPItem>(doc->getObjectById(id)); ASSERT_TRUE(item);
                auto b=item->documentGeometricBounds(); ASSERT_TRUE(b);
                subset(boost::json::array{b->left(),b->top(),b->right(),b->bottom()},bounds);
            };
            if(expected.contains("bounds-after")) actual_bounds("shape1",expected.at("bounds-after"));
            if(expected.contains("paths")) {
                auto const &paths=result.data.at("paths").as_array();
                for(size_t i=0;i<expected.at("paths").as_array().size();++i)
                    actual_bounds(std::string(paths[i].as_object().at("id").as_string()),expected.at("paths").as_array()[i].as_object().at("bounds"));
            }
            EXPECT_TRUE(result.one_undo_step); EXPECT_EQ(result.undo_effect,"one-step");
            EXPECT_EQ(document_stamp(doc.get()).revision,before.at("document_revision").to_number<uint64_t>()+1);
            ASSERT_TRUE(DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
            EXPECT_EQ(canonical(doc->getReprRoot()),before.at("xml").as_string()); EXPECT_EQ(selection(),before.at("selection"));
            ASSERT_TRUE(DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
            EXPECT_EQ(canonical(doc->getReprRoot()),after.at("xml").as_string()); EXPECT_EQ(selection(),after.at("selection"));
        } else if(command=="history.undo" || command=="history.redo") {
            EXPECT_EQ(result.data.at("consumed-label"),"Transform objects");
            EXPECT_EQ(document_stamp(doc.get()).revision,before.at("document_revision").to_number<uint64_t>()+1);
            ASSERT_TRUE(command=="history.undo"?DocumentUndo::redo(doc.get()):DocumentUndo::undo(doc.get()));
            doc->ensureUpToDate(); EXPECT_EQ(canonical(doc->getReprRoot()),before.at("xml").as_string());
            EXPECT_EQ(selection(),before.at("selection"));
        } else {
            EXPECT_EQ(after.at("xml"),before.at("xml")); EXPECT_EQ(after.at("document_revision"),before.at("document_revision"));
            EXPECT_EQ(after.at("undo"),before.at("undo")); EXPECT_EQ(after.at("redo"),before.at("redo"));
        }
        std::cout << "P9-NATIVE-OBSERVATION " << boost::json::serialize(boost::json::object{
            {"command",command},{"before",before},{"after",after},{"preview_after",preview_after},
            {"cancel_after",cancel_after},{"settled",DocumentUndo::fileOperationFreshReady(doc.get())},
            {"status",static_cast<int>(result.status)}}) << std::endl;
    }
    void receipt(Request r, std::string const &code, std::string const &branch, bool detail=true) {
        auto before=state(); auto result=call(r); auto after=state();
        ASSERT_TRUE(result.error); ASSERT_EQ(result.error->code,code);
        if(detail) { ASSERT_TRUE(result.error_details.contains("reason")); EXPECT_EQ(result.error_details.at("reason"),boost::json::value(branch)); }
        EXPECT_EQ(before,after); EXPECT_FALSE(result.one_undo_step);
        ASSERT_TRUE(result.error_details.contains("mutation_state"));
        EXPECT_EQ(result.error_details.at("mutation_state"),code=="internal-error"?"rolled-back":"none");
        bool settled=lease?DocumentUndo::fileOperationOutputReady(doc.get()):DocumentUndo::fileOperationFreshReady(doc.get());
        EXPECT_TRUE(settled);
        if(HasFailure()) return;
        boost::json::object o{{"obligation","P9:"+r.command+":command-service:"+code+":"+branch},
            {"command",result.action},{"code",result.error->code},{"branch",branch},{"before",before},{"after",after},{"settled",settled},
            {"native_reason",result.error_details.at("reason")},{"mutation_state",result.error_details.at("mutation_state")}};
        std::cout << "P9-OBSERVATION " << boost::json::serialize(o) << std::endl;
    }
    std::unique_ptr<SPDocument> doc; DispatchContext context; TokenStore tokens; std::string token_id; std::shared_ptr<void> lease;
};
TEST_F(P9Independent, selection_set) { independent("selection.set",boost::json::parse(R"p9({"ids": ["group1", "child1", "shape1"]})p9").as_object(),boost::json::parse(R"p9({"normalized-ids": ["group1", "shape1"], "covered-ids": ["child1"], "selection-after": ["group1", "shape1"]})p9").as_object()); }
TEST_F(P9Independent, selection_clear) { independent("selection.clear",boost::json::parse(R"p9({})p9").as_object(),boost::json::parse(R"p9({"selection-after": [], "cleared-count": 1})p9").as_object()); }
TEST_F(P9Independent, history_query) { independent("history.query",boost::json::parse(R"p9({})p9").as_object(),boost::json::parse(R"p9({"can-undo": false, "can-redo": false, "next-undo-label": null, "next-redo-label": null})p9").as_object()); }
TEST_F(P9Independent, history_undo) { independent("history.undo",boost::json::parse(R"p9({})p9").as_object(),boost::json::parse(R"p9({"can-undo": false, "can-redo": true})p9").as_object()); }
TEST_F(P9Independent, history_redo) { independent("history.redo",boost::json::parse(R"p9({})p9").as_object(),boost::json::parse(R"p9({"can-undo": true, "can-redo": false})p9").as_object()); }
TEST_F(P9Independent, D12_selection_set_resolve_composite_targets_unknown_root) {
seed_redo();
receipt(request("selection.set",boost::json::parse(R"p9({"ids": ["missing"]})p9").as_object()),"unknown-id","resolve_composite_targets: unknown root");
}
TEST_F(P9Independent, D12_selection_set_resolve_composite_targets_protected_root) {
doc->getObjectById("shape1")->getRepr()->setAttribute("style","display:none"); doc->ensureUpToDate(); seed_redo();
receipt(request("selection.set",boost::json::parse(R"p9({"ids": ["shape1"]})p9").as_object()),"unavailable","resolve_composite_targets: protected root");
}
TEST_F(P9Independent, D12_history_query_EventLog_snapshot_closing_or_interaction_busy) {
seed_redo(); lease=DocumentUndo::holdInteractionOperation(doc.get()); ASSERT_TRUE(lease);
receipt(request("history.query",boost::json::parse(R"p9({})p9").as_object()),"document-busy","EventLog snapshot: closing or interaction busy");
}
TEST_F(P9Independent, D12_history_undo_DocumentUndo_admission_busy) {
seed_redo(); lease=DocumentUndo::holdInteractionOperation(doc.get()); ASSERT_TRUE(lease);
receipt(request("history.undo",boost::json::parse(R"p9({})p9").as_object()),"document-busy","DocumentUndo admission busy");
}
TEST_F(P9Independent, D12_history_redo_DocumentUndo_admission_busy) {
seed_redo(); lease=DocumentUndo::holdInteractionOperation(doc.get()); ASSERT_TRUE(lease);
receipt(request("history.redo",boost::json::parse(R"p9({})p9").as_object()),"document-busy","DocumentUndo admission busy");
}
TEST_F(P9Independent, D12_history_undo_EventLog_no_undo_row) {

receipt(request("history.undo",boost::json::parse(R"p9({})p9").as_object()),"history-empty","EventLog: no undo row");
}
TEST_F(P9Independent, D12_history_redo_EventLog_no_redo_row) {

receipt(request("history.redo",boost::json::parse(R"p9({})p9").as_object()),"history-empty","EventLog: no redo row");
}
} // namespace
