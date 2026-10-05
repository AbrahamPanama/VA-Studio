// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "actions/vacards-cli-production.h"
#include "actions/actions-vacards-nest.h"
#include "nesting/nesting-cli-service.h"
#include "io/vacards-cli-files.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "selection.h"
#include "object/sp-item.h"
#include "xml/repr.h"
#include <boost/json.hpp>
using namespace Inkscape;
using namespace Inkscape::VACardsCli;
namespace {
std::unique_ptr<SPDocument> contour_document() {
    if (!Application::exists()) Application::create(false);
    auto doc = SPDocument::createNewDocFromMem(
        "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
        "<rect id='art' x='20' y='30' width='10' height='12' fill='red'/>"
        "<path id='cut' d='M20,30 H30 V42 H20Z'/></svg>");
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Initialize fixture"), "document-new");
    DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    return doc;
}
Record contour_call(SPDocument &doc, std::string command, boost::json::object params, bool dry=false, bool cancel=false) {
    Request request; request.command=std::move(command); request.params=std::move(params); request.dry_run=dry;
    EditServices edits{doc, *doc.getSelection(), document_stamp(&doc), {}};
    TokenStore tokens;
    NestCliContext context{edits, tokens, [cancel] { return cancel; }};
    DispatchContext dispatch; dispatch.document=&doc; dispatch.selection=doc.getSelection();
    return execute_nest(request, dispatch, context);
}
}

namespace {
struct NestPipeline {
    FileState files; Grants grants; TokenStore tokens; Nesting::CliSession owner;
    NestPipeline() {
        if(!Application::exists())Application::create(false);
        files.document=SPDocument::createNewDocFromMem("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'><rect id='part' x='150' y='10' width='10' height='12' fill='red'/></svg>");
        files.document->ensureUpToDate();DocumentUndo::done(files.document.get(),Util::Internal::ContextString("Initialize fixture"),"document-new");
        DocumentUndo::clearUndo(files.document.get());DocumentUndo::clearRedo(files.document.get());
    }
    Record call(std::string command,boost::json::object params,bool dry=false,bool cancelled=false) {
        auto &doc=*files.document;EditServices edits{doc,*doc.getSelection(),document_stamp(&doc),{}};
        ProductionContext context{files,grants,edits,tokens,{}};context.session_id="P7-test";context.catalog_identity="P7-test-catalog";context.incarnation=1;context.target_generation=1;context.nest=&owner;
        DispatchContext dispatch;dispatch.document=&doc;dispatch.selection=doc.getSelection();dispatch.cancelled=[cancelled]{return cancelled;};
        Request request;request.command=std::move(command);request.params=std::move(params);request.dry_run=dry;
        return execute_nest(request,dispatch,context);
    }
    boost::json::object params(unsigned copies=2) {
        auto px=[](double n){return boost::json::object{{"value",n},{"unit","px"}};};
        return {{"ids",boost::json::array{"part"}},{"sheet",boost::json::object{{"x",px(0)},{"y",px(0)},{"width",px(100)},{"height",px(100)}}},
            {"gap",px(1)},{"margin",px(1)},{"rotations",boost::json::object{{"mode","none"}}},{"copies",boost::json::object{{"part",copies}}},{"fallback","reject"},{"retain",true}};
    }
    TokenValidationContext validation() {
        auto stamp=document_stamp(files.document.get());return {"P7-test",stamp.id,"native","P7-test-catalog",1,stamp.revision,1};
    }
    std::string xml() {return sp_repr_save_buf(files.document->getReprDoc()).raw();}
};
}

TEST(M3Nest, RetiredCaptureAndHeldLeaseAreSafe) {
    NestPipeline p;auto analyzed=p.call("nest.analyze",p.params());ASSERT_EQ(analyzed.status,Status::Ok)<<analyzed.message;
    auto id=std::string(analyzed.data.at("analysis-token").as_string());
    auto held=p.tokens.lookup(id,TokenKind::NestAnalysis,p.validation());ASSERT_TRUE(held.value);
    auto capture=p.owner.lookup(id);ASSERT_TRUE(capture);
    p.tokens.release_subtree(id);p.owner.prune(p.tokens.snapshot());
    EXPECT_EQ(capture->snapshot.document,nullptr);EXPECT_FALSE(capture->snapshot.revision);
    EXPECT_GT(p.tokens.snapshot().charged_bytes,0u);
    p.owner.retire();p.files.document.reset();capture.reset();held.value.reset();
    EXPECT_EQ(p.tokens.snapshot().charged_bytes,0u);
}
TEST(M3RequiredCommands, nest_contour_set) {
    auto doc=contour_document(); ASSERT_TRUE(doc);
    boost::json::object params{{"payload-id","art"},{"contour-id","cut"}};
    auto before=sp_repr_save_buf(doc->getReprDoc()).raw();
    auto stamp=document_stamp(doc.get());
    auto preview=contour_call(*doc,"nest.contour-set",params,true);
    EXPECT_EQ(preview.status,Status::Ok);
    EXPECT_EQ(preview.data.at("variant"),"computed-dry-run");
    EXPECT_EQ(before,sp_repr_save_buf(doc->getReprDoc()).raw());
    EXPECT_EQ(document_stamp(doc.get()).revision,stamp.revision);
    EXPECT_EQ(contour_call(*doc,"nest.contour-set",params,false,true).status,Status::Cancelled);
    EXPECT_EQ(before,sp_repr_save_buf(doc->getReprDoc()).raw());
    auto changed=contour_call(*doc,"nest.contour-set",params);
    ASSERT_EQ(changed.status,Status::Changed);
    EXPECT_TRUE(changed.one_undo_step);
    EXPECT_EQ(changed.created.size(),1u);
    EXPECT_TRUE(doc->getSelection()->isEmpty()); // integration installs after native commit
    auto after=sp_repr_save_buf(doc->getReprDoc()).raw();
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("fill"),"red");
    EXPECT_STREQ(doc->getObjectById("art")->getRepr()->attribute("x"),"20");
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(before,sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_EQ(after,sp_repr_save_buf(doc->getReprDoc()).raw());
    params["contour-id"]="art";
    auto refused=contour_call(*doc,"nest.contour-set",params);
    ASSERT_TRUE(refused.error); EXPECT_EQ(refused.error->code,"duplicate-id");
    EXPECT_EQ(after,sp_repr_save_buf(doc->getReprDoc()).raw());
}
TEST(M3RequiredCommands, nest_contour_release) {
    auto doc=contour_document(); ASSERT_TRUE(doc);
    auto bound=contour_call(*doc,"nest.contour-set",{{"payload-id","art"},{"contour-id","cut"}});
    ASSERT_EQ(bound.status,Status::Changed); ASSERT_EQ(bound.created.size(),1u);
    boost::json::object params{{"ids",boost::json::array{bound.created.front()}}};
    auto before=sp_repr_save_buf(doc->getReprDoc()).raw();
    EXPECT_EQ(contour_call(*doc,"nest.contour-release",params,true).status,Status::Ok);
    EXPECT_EQ(before,sp_repr_save_buf(doc->getReprDoc()).raw());
    auto released=contour_call(*doc,"nest.contour-release",params);
    ASSERT_EQ(released.status,Status::Changed); EXPECT_TRUE(released.one_undo_step);
    EXPECT_EQ(doc->getObjectById("art")->parent,doc->getObjectById("cut")->parent);
    EXPECT_FALSE(doc->getObjectById("cut")->getRepr()->attribute("inkscape:nesting-contour"));
    auto after=sp_repr_save_buf(doc->getReprDoc()).raw();
    auto refusal=contour_call(*doc,"nest.contour-release",params);
    ASSERT_TRUE(refusal.error); EXPECT_EQ(refusal.error->code,"no-eligible-targets");
    EXPECT_EQ(after,sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_TRUE(DocumentUndo::undo(doc.get())); EXPECT_EQ(before,sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_TRUE(DocumentUndo::redo(doc.get())); EXPECT_EQ(after,sp_repr_save_buf(doc->getReprDoc()).raw());
}
TEST(M3RequiredCommands, nest_analyze) {
    NestPipeline p;auto before=p.xml();auto stamp=document_stamp(p.files.document.get());
    auto preview=p.call("nest.analyze",p.params(),true);ASSERT_EQ(preview.status,Status::Ok)<<preview.message;
    EXPECT_TRUE(preview.data.at("analysis-token").is_null());EXPECT_TRUE(p.tokens.snapshot().active_tokens.empty());
    auto analyzed=p.call("nest.analyze",p.params());ASSERT_EQ(analyzed.status,Status::Ok)<<analyzed.message;
    EXPECT_TRUE(analyzed.data.at("analysis-token").is_string());EXPECT_EQ(p.tokens.snapshot().active_roots,1u);
    EXPECT_EQ(before,p.xml());EXPECT_EQ(document_stamp(p.files.document.get()).revision,stamp.revision);
    auto const &recovery=analyzed.data.at("recovery").as_array();ASSERT_EQ(recovery.size(),1u);
    EXPECT_EQ(recovery[0].as_object().at("bounds"),boost::json::value(boost::json::array{150.,10.,160.,22.}));
    auto excessive=p.params(2049);auto refusal=p.call("nest.analyze",excessive);ASSERT_TRUE(refusal.error);EXPECT_EQ(refusal.error->code,"engine-limit");
    EXPECT_EQ(before,p.xml());
}
TEST(M3RequiredCommands, nest_solve) {
    NestPipeline p;auto analyzed=p.call("nest.analyze",p.params());ASSERT_EQ(analyzed.status,Status::Ok)<<analyzed.message;
    boost::json::object params{{"analysis-token",analyzed.data.at("analysis-token")},{"seed",std::uint64_t{0xfedcba9876543210ULL}},{"iterations",1027},{"retain",false}};
    auto before=p.xml();auto a=p.call("nest.solve",params),b=p.call("nest.solve",params);
    ASSERT_EQ(a.status,Status::Ok)<<a.message;ASSERT_EQ(b.status,Status::Ok)<<b.message;
    EXPECT_EQ(a.data.at("iterations").to_number<std::uint64_t>(),1027u);EXPECT_EQ(a.data.at("stop-reason"),"work-limit");
    EXPECT_EQ(a.data.at("engine"),"native");EXPECT_EQ(a.data.at("deterministic"),true);
    EXPECT_EQ(a.data.at("placements"),b.data.at("placements"));EXPECT_EQ(before,p.xml());
    auto cancelled=p.call("nest.solve",params,false,true);EXPECT_EQ(cancelled.status,Status::Cancelled);EXPECT_TRUE(cancelled.data.empty());
    EXPECT_EQ(p.tokens.snapshot().active_roots,1u);EXPECT_EQ(before,p.xml());
}
TEST(M3RequiredCommands, nest_apply) {
    NestPipeline p;auto analyzed=p.call("nest.analyze",p.params());ASSERT_EQ(analyzed.status,Status::Ok)<<analyzed.message;
    auto solved=p.call("nest.solve",{{"analysis-token",analyzed.data.at("analysis-token")},{"seed",7},{"iterations",4096},{"retain",true}});
    ASSERT_EQ(solved.status,Status::Ok)<<solved.message;ASSERT_EQ(solved.data.at("placements").as_array().size(),2u);
    boost::json::object params{{"solution-token",solved.data.at("solution-token")},{"partial","reject"}};
    auto before=p.xml();auto preview=p.call("nest.apply",params,true);
    ASSERT_EQ(preview.status,Status::Ok)<<preview.message;EXPECT_EQ(before,p.xml());EXPECT_EQ(p.tokens.snapshot().active_tokens.size(),2u);
    EXPECT_EQ(preview.data.at("source-copy").as_array()[0].as_object().at("output-id"),"part");
    EXPECT_TRUE(preview.data.at("source-copy").as_array()[1].as_object().at("output-id").is_null());
    auto applied=p.call("nest.apply",params);ASSERT_EQ(applied.status,Status::Changed)<<applied.message;
    EXPECT_TRUE(applied.one_undo_step);EXPECT_EQ(applied.created.size(),1u);EXPECT_TRUE(p.tokens.snapshot().active_tokens.empty());
    auto const &outputs=applied.data.at("source-copy").as_array();ASSERT_EQ(outputs.size(),2u);
    EXPECT_EQ(outputs[0].as_object().at("copy").to_number<unsigned>(),0u);EXPECT_EQ(outputs[0].as_object().at("output-id"),"part");
    for(auto const &output:outputs) {
        auto *item=cast<SPItem>(p.files.document->getObjectById(std::string(output.as_object().at("output-id").as_string())));ASSERT_TRUE(item);
        auto bounds=item->documentGeometricBounds();ASSERT_TRUE(bounds);EXPECT_GE(bounds->left(),1);EXPECT_GE(bounds->top(),1);EXPECT_LE(bounds->right(),99);EXPECT_LE(bounds->bottom(),99);
        EXPECT_STREQ(item->getRepr()->attribute("fill"),"red");
    }
    auto after=p.xml();ASSERT_TRUE(DocumentUndo::undo(p.files.document.get()));EXPECT_EQ(before,p.xml());
    ASSERT_TRUE(DocumentUndo::redo(p.files.document.get()));EXPECT_EQ(after,p.xml());
    auto reopened=SPDocument::createNewDocFromMem(std::span<char const>(after.data(),after.size()));ASSERT_TRUE(reopened);EXPECT_TRUE(reopened->getObjectById(applied.created.front()));
}

TEST(M3Nest, ContourPublicationRollbackPreservesDocumentAndSelection)
{
    auto doc=contour_document(); ASSERT_TRUE(doc);
    doc->getSelection()->add(doc->getObjectById("art"));
    auto before=sp_repr_save_buf(doc->getReprDoc()).raw();
    DocumentUndo::setAtomicSettlementFaultForTesting([](DocumentUndo::AtomicSettlementStage stage) {
        return stage == DocumentUndo::AtomicSettlementStage::HistoryInsertion;
    });
    auto result=contour_call(*doc,"nest.contour-set",{{"payload-id","art"},{"contour-id","cut"}});
    DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
    EXPECT_EQ(result.status,Status::Failed);
    EXPECT_EQ(result.error_details.at("mutation_state"),"rolled-back");
    EXPECT_EQ(before,sp_repr_save_buf(doc->getReprDoc()).raw());
    ASSERT_EQ(doc->getSelection()->size(),1);
    EXPECT_STREQ(doc->getSelection()->singleItem()->getId(),"art");
    EXPECT_FALSE(DocumentUndo::undo(doc.get()));
}

TEST(M3Nest, ApplyRollbackRetainsTokenFamilyAndStaleRevisionRefuses)
{
    NestPipeline p;auto analyzed=p.call("nest.analyze",p.params());ASSERT_EQ(analyzed.status,Status::Ok)<<analyzed.message;
    auto solved=p.call("nest.solve",{{"analysis-token",analyzed.data.at("analysis-token")},{"seed",7},{"iterations",4096},{"retain",true}});
    ASSERT_EQ(solved.status,Status::Ok)<<solved.message;
    boost::json::object params{{"solution-token",solved.data.at("solution-token")},{"partial","reject"}};
    auto before=p.xml();
    DocumentUndo::setAtomicSettlementFaultForTesting([](DocumentUndo::AtomicSettlementStage stage){return stage==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});
    auto failed=p.call("nest.apply",params);
    DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
    EXPECT_EQ(failed.status,Status::Failed)<<failed.message;EXPECT_EQ(failed.error_details.at("mutation_state"),"rolled-back");
    EXPECT_EQ(before,p.xml());EXPECT_EQ(p.tokens.snapshot().active_tokens.size(),2u);EXPECT_FALSE(DocumentUndo::undo(p.files.document.get()));
    p.files.document->getObjectById("part")->setAttribute("fill","blue");
    DocumentUndo::done(p.files.document.get(),Util::Internal::ContextString("Change source"),"object-set");
    auto changed=p.xml();auto stale=p.call("nest.apply",params);ASSERT_TRUE(stale.error);EXPECT_EQ(stale.error->code,"stale-plan");EXPECT_EQ(changed,p.xml());
}
TEST(M3Nest, InlineApplyHasNoRetainedTokens)
{
    NestPipeline p;auto analysis=p.params(1);analysis["retain"]=false;
    boost::json::object params{{"analyze",analysis},{"solve",boost::json::object{{"seed",9},{"iterations",1027},{"retain",false}}},{"partial","reject"}};
    auto before=p.xml();auto applied=p.call("nest.apply",params);ASSERT_EQ(applied.status,Status::Changed)<<applied.message;
    EXPECT_TRUE(p.tokens.snapshot().active_tokens.empty());EXPECT_TRUE(applied.created.empty());EXPECT_EQ(applied.modified.size(),1u);
    ASSERT_TRUE(DocumentUndo::undo(p.files.document.get()));EXPECT_EQ(before,p.xml());
}

// Independently authored fixtures/oracles; native owner boundary with real dispatch state.
#include "actions/vacards-cli-edit-services.h"
#include "nesting/nesting-cli-service.h"
#include "xml/attribute-record.h"
#include "xml/node.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
namespace {
boost::json::value p9_node(XML::Node const *node) {
    std::map<std::string,std::string> sorted;
    for(auto const &a:node->attributeList())sorted[g_quark_to_string(a.key)]=a.value.pointer();
    boost::json::object attrs;for(auto const &[k,v]:sorted)attrs[k]=v;
    boost::json::array children;for(auto *c=node->firstChild();c;c=c->next())children.push_back(p9_node(c));
    return boost::json::array{node->name()?node->name():"",std::move(attrs),node->content()?node->content():"",std::move(children)};
}
struct P9Native {
    FileState files; Grants grants; TokenStore tokens; Nesting::CliSession owner; DispatchContext dispatch;
    explicit P9Native(char const *fixture,std::string extra={}) {
        if(!Application::exists())Application::create(false);
        auto path=std::filesystem::path(__FILE__).parent_path().parent_path()/"cli_tests/vacards-agent/fixtures/m3"/(std::string(fixture)+".svg");
        std::ifstream input(path);std::string xml((std::istreambuf_iterator<char>(input)),{});
        if(!extra.empty())xml.insert(xml.rfind("</svg>"),extra);
        files.document=SPDocument::createNewDocFromMem(std::span<char const>(xml.data(),xml.size()));
        if(!files.document)throw std::runtime_error("P9 fixture unavailable: "+path.string());
        checkpoint();
        dispatch.document=files.document.get();dispatch.selection=files.document->getSelection();dispatch.session_revision=37;
        dispatch.selection->add(files.document->getObjectById("untouched"));
    }
    void checkpoint() {
        files.document->ensureUpToDate();DocumentUndo::done(files.document.get(),Util::Internal::ContextString("P9 fixture"),"document-new");
        DocumentUndo::clearUndo(files.document.get());DocumentUndo::clearRedo(files.document.get());
        files.document->getReprRoot()->setAttribute("data-p9-history","kept");
        DocumentUndo::done(files.document.get(),Util::Internal::ContextString("P9 kept Undo"),"document-new");
        files.document->getReprRoot()->setAttribute("data-p9-history","redo");
        DocumentUndo::done(files.document.get(),Util::Internal::ContextString("P9 pending Redo"),"document-new");
        if(!DocumentUndo::undo(files.document.get()))throw std::runtime_error("P9 Redo seed failed");
    }
    std::function<bool()> cancellation_probe;
    Record call(std::string command,boost::json::object params,bool dry=false,bool cancel=false) {
        auto &doc=*files.document;EditServices edits{doc,*doc.getSelection(),document_stamp(&doc),{}};
        ProductionContext context{files,grants,edits,tokens,{}};context.session_id="P9-native";context.catalog_identity="P9-oracle";
        context.incarnation=1;context.target_generation=1;context.nest=&owner;
        dispatch.cancelled=cancellation_probe ? cancellation_probe : std::function<bool()>([cancel]{return cancel;});
        Request request;request.command=std::move(command);request.params=std::move(params);request.dry_run=dry;
        return execute_nest(request,dispatch,context);
    }
    boost::json::object state() {
        auto &doc=*files.document;doc.ensureUpToDate();
        EditServices edits{doc,*doc.getSelection(),document_stamp(&doc),{}};
        auto history=history_snapshot(edits);if(!history.value)throw std::runtime_error("P9 history snapshot unavailable");
        auto label=[](auto const &s)->boost::json::value{return s?boost::json::value(*s):boost::json::value(nullptr);};
        boost::json::array selection,ids;for(auto *i:doc.getSelection()->items())selection.emplace_back(i->getId());
        auto snapshot=tokens.snapshot();for(auto const &id:snapshot.active_tokens)ids.emplace_back(id);
        return {{"xml",p9_node(doc.getReprRoot())},{"selection",std::move(selection)},
          {"document_revision",document_stamp(&doc).revision},{"session_revision",dispatch.session_revision},
          {"undo",boost::json::object{{"available",history.value->can_undo},{"label",label(history.value->next_undo_label)}}},
          {"redo",boost::json::object{{"available",history.value->can_redo},{"label",label(history.value->next_redo_label)}}},
          {"tokens",boost::json::object{{"ids",std::move(ids)},{"roots",snapshot.active_roots},{"unique-held-bytes",snapshot.charged_bytes}}}};
    }
    void preview_cancel(std::string const &cmd,boost::json::object const &params) {
        auto before=state();auto preview=call(cmd,params,true);EXPECT_EQ(preview.status,Status::Ok)<<preview.message;
        auto preview_state=state();EXPECT_EQ(preview_state,before);auto cancelled=call(cmd,params,false,true);EXPECT_EQ(cancelled.status,Status::Cancelled);
        auto cancel_state=state();EXPECT_EQ(cancel_state,before);
        EXPECT_FALSE(DocumentUndo::interactionActive(files.document.get()));
        std::cout<<"P9-NATIVE-OBSERVATION "<<boost::json::serialize(boost::json::object{{"command",cmd},
            {"before",before},{"preview",preview_state},{"cancel",cancel_state},{"settled",!DocumentUndo::interactionActive(files.document.get())}})<<'\n';
    }
    void undo_redo(boost::json::object const &before,boost::json::object const &after) {
        ASSERT_TRUE(DocumentUndo::undo(files.document.get()));auto undone=state();
        EXPECT_EQ(undone.at("xml"),before.at("xml"));EXPECT_EQ(undone.at("selection"),before.at("selection"));
        ASSERT_TRUE(DocumentUndo::redo(files.document.get()));auto redone=state();
        EXPECT_EQ(redone.at("xml"),after.at("xml"));EXPECT_EQ(redone.at("selection"),after.at("selection"));
        EXPECT_EQ(redone.at("undo"),after.at("undo"));EXPECT_EQ(redone.at("redo"),after.at("redo"));
        EXPECT_FALSE(DocumentUndo::interactionActive(files.document.get()));
        std::cout<<"P9-NATIVE-OBSERVATION "<<boost::json::serialize(boost::json::object{{"before",before},{"committed",after},
            {"undone",undone},{"redone",redone},{"settled",!DocumentUndo::interactionActive(files.document.get())}})<<'\n';
    }
    boost::json::value untouched() {return p9_node(files.document->getObjectById("untouched")->getRepr());}
    void error(std::string const &cmd,boost::json::object const &params,std::string const &code,std::string const &branch,std::string const &native_reason={}) {
        auto before=state();auto r=call(cmd,params);auto after=state();
        ASSERT_TRUE(r.error)<<r.message;ASSERT_EQ(r.error->code,code)<<r.message;
        ASSERT_EQ(r.error_details.at("reason"),boost::json::value(native_reason.empty()?branch:native_reason));EXPECT_EQ(before,after);
        EXPECT_FALSE(r.one_undo_step);EXPECT_TRUE(r.data.empty());
        if(branch=="capture: unusable part") {
            ASSERT_FALSE(r.excluded.empty());
            for(auto const &exclusion:r.excluded)EXPECT_FALSE(exclusion.reason.empty());
        }
        ASSERT_FALSE(DocumentUndo::interactionActive(files.document.get()));
        // Exact fixture precondition plus asserted native reason establishes the frozen branch.
        // Preserve the native reason separately when the descriptor uses a route-specific label.
        boost::json::array exclusions;
        for(auto const &entry:r.excluded)exclusions.emplace_back(boost::json::object{{"id",entry.id},{"reason",entry.reason}});
        boost::json::object record{{"exclusions",std::move(exclusions)},{"obligation","P9:"+cmd+":command-service:"+code+":"+branch},
            {"command",r.action},{"code",r.error->code},{"branch",branch},{"native_reason",r.error_details.at("reason")},
            {"before",std::move(before)},{"after",std::move(after)},{"settled",!DocumentUndo::interactionActive(files.document.get())}};
        std::cout<<"P9-OBSERVATION "<<boost::json::serialize(record)<<'\n';
    }
    static boost::json::object analysis() {
        auto px=[](double n){return boost::json::object{{"value",n},{"unit","px"}};};
        return {{"ids",boost::json::array{"shape1"}},{"page",1},{"gap",px(0)},{"margin",px(0)},
            {"rotations",boost::json::object{{"mode","none"}}},{"fallback","reject"},{"retain",true}};
    }
};
}

TEST(P9Independent, nest_contour_set) {
    P9Native p("nest");boost::json::object args{{"payload-id","shape1"},{"contour-id","contour1"}};
    p.preview_cancel("nest.contour-set",args);auto before=p.state();auto untouched=p.untouched();
    auto r=p.call("nest.contour-set",args);ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_TRUE(r.one_undo_step);
    EXPECT_EQ(r.data.at("payload-ids"),boost::json::value(boost::json::array{"shape1"}));
    EXPECT_EQ(r.data.at("contour-ids"),boost::json::value(boost::json::array{"contour1"}));
    EXPECT_EQ(p.untouched(),untouched);p.undo_redo(before,p.state());
}
TEST(P9Independent, nest_contour_release) {
    P9Native p("nest");auto bound=p.call("nest.contour-set",{{"payload-id","shape1"},{"contour-id","contour1"}});
    ASSERT_EQ(bound.status,Status::Changed);boost::json::object args{{"ids",bound.data.at("binding-ids")}};
    p.preview_cancel("nest.contour-release",args);auto before=p.state();auto untouched=p.untouched();
    auto r=p.call("nest.contour-release",args);ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_TRUE(r.one_undo_step);
    EXPECT_EQ(r.data.at("payload-ids"),boost::json::value(boost::json::array{"shape1"}));
    EXPECT_EQ(r.data.at("contour-ids"),boost::json::value(boost::json::array{"contour1"}));
    EXPECT_EQ(p.untouched(),untouched);p.undo_redo(before,p.state());
}
TEST(P9Independent, nest_analyze) {
    P9Native p("nest");auto args=p.analysis();p.preview_cancel("nest.analyze",args);auto before=p.state();auto untouched=p.untouched();
    auto r=p.call("nest.analyze",args);ASSERT_EQ(r.status,Status::Ok)<<r.message;
    EXPECT_EQ(r.data.at("sheet-bounds"),boost::json::value(boost::json::array{0.,0.,200.,200.}));
    auto const &copies=r.data.at("requested-copies").as_array();ASSERT_EQ(copies.size(),1u);
    EXPECT_EQ(copies[0].as_object().at("id"),"shape1");EXPECT_EQ(copies[0].as_object().at("count").to_number<unsigned>(),1u);
    auto after=p.state();EXPECT_EQ(after.at("xml"),before.at("xml"));EXPECT_EQ(after.at("undo"),before.at("undo"));
    EXPECT_EQ(after.at("redo"),before.at("redo"));EXPECT_EQ(after.at("document_revision"),before.at("document_revision"));
    EXPECT_EQ(p.tokens.snapshot().active_tokens.size(),1u);EXPECT_GT(p.tokens.snapshot().charged_bytes,0u);
    EXPECT_EQ(p.untouched(),untouched);
}
TEST(P9Independent, nest_solve) {
    P9Native p("nest");auto a=p.call("nest.analyze",p.analysis());ASSERT_EQ(a.status,Status::Ok);
    boost::json::object args{{"analysis-token",a.data.at("analysis-token")},{"iterations",17},{"seed",UINT64_MAX},{"engine","native"},{"retain",true}};
    p.preview_cancel("nest.solve",args);auto before=p.state();auto untouched=p.untouched();
    auto r=p.call("nest.solve",args);ASSERT_EQ(r.status,Status::Ok)<<r.message;
    EXPECT_EQ(r.data.at("engine"),"native");EXPECT_EQ(r.data.at("iterations").to_number<unsigned>(),17u);
    EXPECT_EQ(r.data.at("stop-reason"),"work-limit");EXPECT_EQ(r.data.at("deterministic"),true);
    EXPECT_TRUE(r.data.at("unplaced").as_array().empty());EXPECT_DOUBLE_EQ(r.data.at("utilization").to_number<double>(),0.25);
    auto const &poses=r.data.at("placements").as_array();ASSERT_EQ(poses.size(),1u);
    EXPECT_EQ(poses[0].as_object().at("id"),"shape1");EXPECT_EQ(poses[0].as_object().at("copy").to_number<unsigned>(),0u);
    auto after=p.state();for(auto key:{"xml","selection","document_revision","session_revision","undo","redo"})EXPECT_EQ(after.at(key),before.at(key));
    EXPECT_EQ(p.tokens.snapshot().active_tokens.size(),2u);EXPECT_EQ(p.untouched(),untouched);
}
TEST(P9Independent, nest_apply) {
    P9Native p("nest");boost::json::object args{{"analyze",p.analysis()},{"solve",boost::json::object{{"iterations",17},{"seed",0},{"retain",false}}},{"partial","reject"}};
    p.preview_cancel("nest.apply",args);auto before=p.state();auto untouched=p.untouched();
    auto r=p.call("nest.apply",args);ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_TRUE(r.one_undo_step);
    EXPECT_TRUE(r.data.at("unplaced").as_array().empty());auto const &outputs=r.data.at("source-copy").as_array();ASSERT_EQ(outputs.size(),1u);
    auto const &row=outputs[0].as_object();EXPECT_EQ(row.at("source"),"shape1");EXPECT_EQ(row.at("copy").to_number<unsigned>(),0u);EXPECT_EQ(row.at("output-id"),"shape1");
    EXPECT_TRUE(p.tokens.snapshot().active_tokens.empty());EXPECT_EQ(p.untouched(),untouched);p.undo_redo(before,p.state());
}

namespace {
struct P9NestSettlementFault {
    P9NestSettlementFault(){DocumentUndo::setAtomicSettlementFaultForTesting([](auto stage){return stage==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});}
    ~P9NestSettlementFault(){DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);}
};
}
TEST(P9D12, nest_contour_set_equal) {
    P9Native p("nest");p.error("nest.contour-set",{{"payload-id","shape1"},{"contour-id","shape1"}},"duplicate-id","payload and contour roles equal");
}
TEST(P9D12, nest_contour_set_unsupported) {
    P9Native p("nest");p.error("nest.contour-set",{{"payload-id","shape1"},{"contour-id","image1"}},"unsupported-target","binding geometry unsupported");
}
TEST(P9D12, nest_contour_set_history_fault) {
    P9Native p("nest");P9NestSettlementFault fault;
    p.error("nest.contour-set",{{"payload-id","shape1"},{"contour-id","contour1"}},"internal-error","unexpected service exception; rollback before return");
}
TEST(P9D12, nest_contour_release_unbound) {
    P9Native p("nest");p.error("nest.contour-release",{{"ids",boost::json::array{"shape1"}}},"no-eligible-targets","no bound root");
}
TEST(P9D12, nest_contour_release_history_fault) {
    P9Native p("nest");auto bound=p.call("nest.contour-set",{{"payload-id","shape1"},{"contour-id","contour1"}});ASSERT_EQ(bound.status,Status::Changed);
    P9NestSettlementFault fault;p.error("nest.contour-release",{{"ids",bound.data.at("binding-ids")}},"internal-error","unexpected service exception; rollback before return");
}
TEST(P9D12, nest_analyze_resource_limit) {
    P9Native p("nest");auto args=p.analysis();args["copies"]=boost::json::object{{"shape1",2049}};
    p.error("nest.analyze",args,"engine-limit","expanded copies >100000 or resource budget");
}
TEST(P9D12, nest_apply_invalid_copy_keys) {
    P9Native p("nest");auto args=p.analysis();args["copies"]=boost::json::object{{"shape2",1}};
    p.error("nest.apply",{{"analyze",args},{"solve",boost::json::object{{"seed",0},{"iterations",17}}}},"invalid-input","capture/assemble: invalid page or copy keys");
}
TEST(P9D12, nest_apply_publication_fault) {
    P9Native p("nest");auto a=p.call("nest.analyze",p.analysis());ASSERT_EQ(a.status,Status::Ok);
    auto s=p.call("nest.solve",{{"analysis-token",a.data.at("analysis-token")},{"seed",0},{"iterations",17},{"retain",true}});ASSERT_EQ(s.status,Status::Ok);
    P9NestSettlementFault fault;p.error("nest.apply",{{"solution-token",s.data.at("solution-token")},{"partial","reject"}},"publication-failed","copy insertion/apply failed; native rollback");
}
TEST(P9D12, nest_apply_stale_native_capture) {
    P9Native p("nest");auto a=p.call("nest.analyze",p.analysis());ASSERT_EQ(a.status,Status::Ok);
    auto s=p.call("nest.solve",{{"analysis-token",a.data.at("analysis-token")},{"seed",0},{"iterations",17},{"retain",true}});ASSERT_EQ(s.status,Status::Ok);
    auto capture=p.owner.lookup(std::string(a.data.at("analysis-token").as_string()));ASSERT_TRUE(capture);
    // A retired native weak capture, with still-live token identities, takes the exact freshness branch.
    p.owner.retire();p.error("nest.apply",{{"solution-token",s.data.at("solution-token")},{"partial","reject"}},"stale-dependency","native freshness check");
}

TEST(M3Nest, ReleasedGroupedPayloadKeepsRootIdentityAndHierarchy) {
    P9Native p("nest");auto payload=p9_node(p.files.document->getObjectById("group1")->getRepr());
    auto bound=p.call("nest.contour-set",{{"payload-id","group1"},{"contour-id","contour1"}});ASSERT_EQ(bound.status,Status::Changed);
    boost::json::object args{{"ids",bound.data.at("binding-ids")}};
    auto preview=p.call("nest.contour-release",args,true);ASSERT_EQ(preview.status,Status::Ok);
    EXPECT_EQ(preview.data.at("payload-ids"),boost::json::value(boost::json::array{"group1"}));
    auto r=p.call("nest.contour-release",args);ASSERT_EQ(r.status,Status::Changed);
    EXPECT_EQ(r.data.at("payload-ids"),boost::json::value(boost::json::array{"group1"}));
    EXPECT_EQ(p9_node(p.files.document->getObjectById("group1")->getRepr()),payload);
    EXPECT_EQ(p.files.document->getObjectById("child1")->parent,p.files.document->getObjectById("group1"));
}

TEST(P9D12, nest_analyze_conservative) {
    P9Native p("nest","<path id='islands' d='M20,20 h10 v10 h-10z M50,40 h10 v10 h-10z'/>");
    auto args=p.analysis();args["ids"]=boost::json::array{"islands"};
    p.error("nest.analyze",args,"unsupported-target","capture: conservative and fallback reject");
}
TEST(P9D12, nest_analyze_failed_geometry) {
    // Empty parts are exclusions. An unmeasurable explicit obstacle still
    // makes native geometry preparation unsafe even with a usable part.
    P9Native p("nest");p.files.document->getObjectById("shape2")->setAttribute("d","");p.checkpoint();
    auto args=p.analysis();args["obstacles"]=boost::json::array{"shape2"};
    p.error("nest.analyze",args,"analysis-failed","prepare/assemble geometry failed");
}
TEST(P9D12, nest_apply_partial_reject) {
    P9Native p("nest");auto args=p.analysis();args.erase("page");
    auto px=[](double n){return boost::json::object{{"value",n},{"unit","px"}};};
    args["sheet"]=boost::json::object{{"x",px(0)},{"y",px(0)},{"width",px(1)},{"height",px(1)}};
    p.error("nest.apply",{{"analyze",args},{"solve",boost::json::object{{"seed",0},{"iterations",17}}},{"partial","reject"}},"partial-result","unplaced copies and partial reject");
}

TEST(P9D12, nest_analyze_missing_page) {
    P9Native p("nest");auto args=p.analysis();args["page"]=999;
    // Valid copies/parts; prepareRequestNesting's page-not-found exit is the only invalid input here.
    p.error("nest.analyze",args,"invalid-input","capture: page not found","capture/assemble: invalid page or copy keys");
}
TEST(P9D12, nest_apply_conservative) {
    P9Native p("nest","<path id='islands' d='M20,20 h10 v10 h-10z M50,40 h10 v10 h-10z'/>");
    auto args=p.analysis();args["ids"]=boost::json::array{"islands"};
    p.error("nest.apply",{{"analyze",args},{"solve",boost::json::object{{"seed",0},{"iterations",17}}}},
        "unsupported-target","capture: conservative geometry forbidden","capture: conservative and fallback reject");
}
TEST(P9D12, nest_apply_resource_limit) {
    P9Native p("nest");auto args=p.analysis();args["copies"]=boost::json::object{{"shape1",2049}};
    p.error("nest.apply",{{"analyze",args},{"solve",boost::json::object{{"seed",0},{"iterations",17}}}},
        "engine-limit","expanded copies or resource budget exceeded","expanded copies >100000 or resource budget");
}
TEST(P9D12, nest_apply_failed_geometry) {
    P9Native p("nest");p.files.document->getObjectById("shape1")->setAttribute("d","");p.checkpoint();
    p.error("nest.apply",{{"analyze",p.analysis()},{"solve",boost::json::object{{"seed",0},{"iterations",17}}}},
        "analysis-failed","prepare: failed geometry","prepare/assemble geometry failed");
}

TEST(P9D12, nest_apply_native_validation_fault) {
    P9Native p("nest");auto a=p.call("nest.analyze",p.analysis());ASSERT_EQ(a.status,Status::Ok);
    auto s=p.call("nest.solve",{{"analysis-token",a.data.at("analysis-token")},{"seed",0},{"iterations",17},{"retain",true}});ASSERT_EQ(s.status,Status::Ok);
    auto capture=p.owner.lookup(std::string(a.data.at("analysis-token").as_string()));ASSERT_TRUE(capture);
    // Native fault injection: the result no longer fits its prepared validation geometry.
    // No XML/token/dispatch state is edited and Job::validate itself must reject it.
    capture->snapshot.container_outline={{0,0},{1,0},{1,1},{0,1}};
    p.error("nest.apply",{{"solution-token",s.data.at("solution-token")},{"partial","reject"}},
        "invalid-placement","Job::validate: overlap/outside/invalid pose");
}
TEST(P9D12, nest_solve_native_worker_fault) {
    P9Native p("nest");auto a=p.call("nest.analyze",p.analysis());ASSERT_EQ(a.status,Status::Ok);
    auto capture=p.owner.lookup(std::string(a.data.at("analysis-token").as_string()));ASSERT_TRUE(capture);
    // Native prepared-input fault; the actual native worker's container admission fails.
    capture->snapshot.container_outline.clear();
    p.error("nest.solve",{{"analysis-token",a.data.at("analysis-token")},{"seed",0},{"iterations",17},{"retain",true}},
        "solver-failed","native worker failed");
}

// Real inline worker callback failure, without changing production code or a prepared capture.
// execute_nest_geometry polls twice before Job::run; its third poll is the native
// progress callback. The FFI trampoline catches, cancels and rethrows that exception;
// solveNativeNesting converts it to InternalError, then the adapter emits solver-failed.
TEST(P9D12, nest_apply_native_worker_callback_fault) {
    P9Native p("nest");unsigned polls=0, faults=0;
    p.cancellation_probe=[&] {
        if (++polls==3) { ++faults;throw std::runtime_error("P7 native worker progress callback fault"); }
        return false;
    };
    p.error("nest.apply",{{"analyze",p.analysis()},
        {"solve",boost::json::object{{"seed",0},{"iterations",17}}},{"partial","reject"}},
        "solver-failed","native worker failed");
    EXPECT_EQ(faults,1u);EXPECT_GE(polls,4u);
}

#include "actions/vacards-cli-fault-testing.h"

TEST(P9D12, nest_analyze_internal_exception_retention_rollback) {
    P9Native p("nest");
    ScopedCliFaultPlanForTesting fault({{{"nest.analyze.after-token-retain",CliFaultKind::ServiceException}}, {}});
    p.error("nest.analyze",p.analysis(),"internal-error","unexpected service exception; rollback before return");
    EXPECT_TRUE(p.tokens.snapshot().active_tokens.empty());
}
TEST(P9D12, nest_solve_internal_exception_retention_rollback) {
    P9Native p("nest");auto a=p.call("nest.analyze",p.analysis());ASSERT_EQ(a.status,Status::Ok);
    auto root=std::string(a.data.at("analysis-token").as_string());auto capture=p.owner.lookup(root);
    ScopedCliFaultPlanForTesting fault({{{"nest.solve.after-token-retain",CliFaultKind::ServiceException}}, {}});
    p.error("nest.solve",{{"analysis-token",root},{"seed",0},{"iterations",17},{"retain",true}},
        "internal-error","unexpected service exception; rollback before return");
    EXPECT_EQ(p.owner.lookup(root),capture);EXPECT_EQ(p.tokens.snapshot().active_tokens.size(),1u);
}
TEST(P9D12, nest_apply_internal_exception_before_native_apply) {
    P9Native p("nest");auto a=p.call("nest.analyze",p.analysis());ASSERT_EQ(a.status,Status::Ok);
    auto s=p.call("nest.solve",{{"analysis-token",a.data.at("analysis-token")},{"seed",0},{"iterations",17},{"retain",true}});ASSERT_EQ(s.status,Status::Ok);
    ScopedCliFaultPlanForTesting fault({{{"nest.apply.before-native-apply",CliFaultKind::ServiceException}}, {}});
    p.error("nest.apply",{{"solution-token",s.data.at("solution-token")},{"partial","reject"}},
        "internal-error","unexpected service exception; rollback before return");
    EXPECT_EQ(p.tokens.snapshot().active_tokens.size(),2u);
}
TEST(P9D12, nest_analyze_all_unusable) {
    P9Native p("nest","<path id='empty' d=''/>");auto args=p.analysis();args["ids"]=boost::json::array{"empty"};
    ScopedCliFaultPlanForTesting fault({{}, {}});
    p.error("nest.analyze",args,"unsupported-target","capture: unusable part");
}
TEST(P7Fault, MixedUnusableAnalysisReportsExclusion) {
    P9Native p("nest","<path id='empty' d=''/>");auto args=p.analysis();args["ids"]=boost::json::array{"shape1","empty"};args["retain"]=false;
    ScopedCliFaultPlanForTesting fault({{}, {}});
    auto before=p.state();auto r=p.call("nest.analyze",args);auto after=p.state();
    ASSERT_EQ(r.status,Status::Ok)<<r.message;ASSERT_EQ(r.excluded.size(),1u);
    EXPECT_EQ(r.excluded[0].id,"empty");EXPECT_FALSE(r.excluded[0].reason.empty());
    EXPECT_EQ(r.eligible,1u);EXPECT_EQ(r.selected,2u);EXPECT_EQ(before,after);
    EXPECT_FALSE(DocumentUndo::interactionActive(p.files.document.get()));
    std::cout<<"P9-OBSERVATION "<<boost::json::serialize(boost::json::object{
        {"obligation","P9:nest.analyze:command-service:success:D16 mixed usable/unusable succeeds with exclusions"},
        {"command",r.action},{"code",r.data.at("variant")},
        {"branch","D16 mixed usable/unusable succeeds with exclusions"},{"before",before},{"after",after},
        {"excluded-id",r.excluded[0].id},{"exclusion-reason",r.excluded[0].reason},
        {"settled",!DocumentUndo::interactionActive(p.files.document.get())}})<<'\n';
}
TEST(P7Fault, AfterPrepareExceptionsPreserveState) {
    for (auto command : {"nest.analyze", "nest.solve", "nest.apply"}) {
        P9Native p("nest");boost::json::object params=p.analysis();
        if(std::string(command)=="nest.solve") {
            auto a=p.call("nest.analyze",params);ASSERT_EQ(a.status,Status::Ok);
            params={{"analysis-token",a.data.at("analysis-token")},{"seed",0},{"iterations",17}};
        } else if(std::string(command)=="nest.apply") {
            params={{"analyze",params},{"solve",boost::json::object{{"seed",0},{"iterations",17}}}};
        }
        auto before=p.state();
        ScopedCliFaultPlanForTesting fault({{{std::string(command)+".after-prepare",CliFaultKind::ServiceException}}, {}});
        auto r=p.call(command,params);ASSERT_TRUE(r.error);EXPECT_EQ(r.error->code,"internal-error");
        EXPECT_EQ(before,p.state());EXPECT_FALSE(DocumentUndo::interactionActive(p.files.document.get()));
    }
}
