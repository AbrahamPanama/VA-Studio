// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "actions/actions-vacards-clip.h"
#include "actions/vacards-cli-edit-services.h"
#include "actions/vacards-cli-production.h"
#include "io/vacards-cli-files.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "selection.h"
#include "object/sp-item.h"
#include "xml/repr.h"
using namespace Inkscape;
using namespace Inkscape::VACardsCli;
namespace {
struct ClipFixture {
    FileState files; Grants grants; TokenStore tokens;
    ClipFixture(bool bitmap=false, std::string extra={}) {
        if(!Application::exists())Application::create(false);
        std::string art="<rect id='target' width='40' height='40' fill='red'/>";
        if(bitmap) {
            auto *pixels=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,40,40);gdk_pixbuf_fill(pixels,0xff0000ff);
            gchar *buffer=nullptr;gsize size=0;gdk_pixbuf_save_to_buffer(pixels,&buffer,&size,"png",nullptr,nullptr);
            gchar *encoded=g_base64_encode(reinterpret_cast<guchar const *>(buffer),size);
            art="<image id='target' width='40' height='40' xlink:href='data:image/png;base64,"+std::string(encoded)+"'/>";
            g_free(encoded);g_free(buffer);g_object_unref(pixels);
        }
        auto xml="<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' width='100' height='100'>"+art+"<path id='cutter' d='M10,10 H30 V30 H10Z'/>"+extra+"</svg>";
        files.document=SPDocument::createNewDocFromMem(std::span<char const>(xml.data(),xml.size()));
        files.document->ensureUpToDate();DocumentUndo::done(files.document.get(),Util::Internal::ContextString("Fixture"),"document-new");
        DocumentUndo::clearUndo(files.document.get());DocumentUndo::clearRedo(files.document.get());
    }
    Record call(std::string command, boost::json::object params={},bool dry=false,bool cancel_after_work=false) {
        if(params.empty())params={{"target-id","target"},{"cutter-id","cutter"},{"keep-cutter",true}};
        auto &doc=*files.document;EditServices edits{doc,*doc.getSelection(),document_stamp(&doc),{}};
        ProductionContext context{files,grants,edits,tokens,{}};DispatchContext dispatch;dispatch.document=&doc;dispatch.selection=doc.getSelection();
        unsigned polls=0;dispatch.cancelled=[&]{return cancel_after_work && ++polls>1;};
        Request r;r.command=command;r.params=std::move(params);r.dry_run=dry;return execute_clip(r,dispatch,context);
    }
    std::string xml(){return sp_repr_save_buf(files.document->getReprDoc()).raw();}
};
}
TEST(M3Clip, FrozenNativeRefusalsAreDistinctAndReadOnly) {
    ClipFixture p(false,"<use id='missing' xlink:href='#absent'/><use id='follower' xlink:href='#cutter'/>");
    auto before=p.xml();
    for(auto const &entry:std::vector<std::pair<std::string,std::string>>{{"missing","MissingSource"},{"cutter","ReferencedCutter"}}) {
        auto r=p.call("clip.set",{{"target-id","target"},{"cutter-id",entry.first},{"keep-cutter",false}});
        ASSERT_TRUE(r.error);EXPECT_EQ(r.error->code,"unsupported-target");
        EXPECT_EQ(std::string(r.error_details.at("reason").as_string()),"ClipDocumentService::Reason::"+entry.second);
        EXPECT_EQ(r.error_details.at("mutation_state"),"none");EXPECT_EQ(r.error_retryable,false);EXPECT_EQ(before,p.xml());
    }
    EXPECT_FALSE(DocumentUndo::undo(p.files.document.get()));
}
TEST(M3RequiredCommands, clip_set) {
    ClipFixture p;auto before=p.xml();auto preview=p.call("clip.set",{},true);
    ASSERT_EQ(preview.status,Status::Ok)<<preview.message;EXPECT_EQ(preview.data.at("variant"),"computed-dry-run");EXPECT_EQ(before,p.xml());
    EXPECT_TRUE(preview.data.at("relations").as_array()[0].as_object().at("resource-id").is_null());
    auto bad=p.call("clip.set",{{"target-id","target"},{"cutter-id","target"}});ASSERT_TRUE(bad.error);EXPECT_EQ(bad.error->code,"duplicate-id");EXPECT_EQ(before,p.xml());
    auto r=p.call("clip.set");ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_TRUE(r.one_undo_step);
    auto *target=cast<SPItem>(p.files.document->getObjectById("target"));ASSERT_TRUE(target->getClipObject());EXPECT_TRUE(p.files.document->getObjectById("cutter"));
    auto bounds=target->documentVisualBounds();ASSERT_TRUE(bounds);EXPECT_NEAR(bounds->left(),10,1e-6);EXPECT_NEAR(bounds->right(),30,1e-6);
    auto after=p.xml();ASSERT_TRUE(DocumentUndo::undo(p.files.document.get()));EXPECT_EQ(before,p.xml());ASSERT_TRUE(DocumentUndo::redo(p.files.document.get()));EXPECT_EQ(after,p.xml());
    auto reopened=SPDocument::createNewDocFromMem(std::span<char const>(after.data(),after.size()));ASSERT_TRUE(reopened);EXPECT_TRUE(cast<SPItem>(reopened->getObjectById("target"))->getClipObject());
    ClipFixture consumed;consumed.files.document->getSelection()->add(consumed.files.document->getObjectById("cutter"));
    auto removed=consumed.call("clip.set",{{"target-id","target"},{"cutter-id","cutter"},{"keep-cutter",false}});
    ASSERT_EQ(removed.status,Status::Changed);EXPECT_EQ(removed.data.at("relations").as_array().size(),1u);EXPECT_TRUE(removed.selection_after.empty());EXPECT_FALSE(consumed.files.document->getObjectById("cutter"));
}
TEST(M3RequiredCommands, clip_release) {
    ClipFixture p;ASSERT_EQ(p.call("clip.set").status,Status::Changed);auto before=p.xml();
    boost::json::object params{{"ids",boost::json::array{"target","cutter"}},{"keep-cutter",false}};
    ASSERT_EQ(p.call("clip.release",params,true).status,Status::Ok);EXPECT_EQ(before,p.xml());
    auto r=p.call("clip.release",params);ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_EQ(r.excluded.size(),1u);EXPECT_TRUE(r.one_undo_step);
    EXPECT_FALSE(cast<SPItem>(p.files.document->getObjectById("target"))->getClipObject());auto after=p.xml();
    auto no=p.call("clip.release",params);ASSERT_TRUE(no.error);EXPECT_EQ(no.error->code,"no-eligible-targets");EXPECT_EQ(after,p.xml());
    ASSERT_TRUE(DocumentUndo::undo(p.files.document.get()));EXPECT_EQ(before,p.xml());ASSERT_TRUE(DocumentUndo::redo(p.files.document.get()));EXPECT_EQ(after,p.xml());
}
TEST(M3RequiredCommands, clip_destructive) {
    ClipFixture p(true);auto before=p.xml();auto *cutter=p.files.document->getObjectById("cutter");auto cutter_path=std::string(cutter->getRepr()->attribute("d"));
    auto preview=p.call("clip.destructive",{},true);ASSERT_EQ(preview.status,Status::Ok)<<preview.message;EXPECT_EQ(before,p.xml());
    auto r=p.call("clip.destructive");ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_TRUE(r.one_undo_step);EXPECT_TRUE(r.data.at("cutter-retained").as_bool());
    EXPECT_EQ(r.data.at("pixel-sha256"),preview.data.at("pixel-sha256"));EXPECT_EQ(r.data.at("pixel-sha256").as_string().size(),64u);
    EXPECT_STREQ(cutter->getRepr()->attribute("d"),cutter_path.c_str());auto bounds=cast<SPItem>(p.files.document->getObjectById("target"))->documentGeometricBounds();ASSERT_TRUE(bounds);EXPECT_NEAR(bounds->left(),10,1e-6);EXPECT_NEAR(bounds->right(),30,1e-6);
    auto after=p.xml();auto no=p.call("clip.destructive");EXPECT_EQ(no.status,Status::Unchanged)<<no.message;EXPECT_EQ(after,p.xml());
    ASSERT_TRUE(DocumentUndo::undo(p.files.document.get()));EXPECT_EQ(before,p.xml());ASSERT_TRUE(DocumentUndo::redo(p.files.document.get()));EXPECT_EQ(after,p.xml());
    ClipFixture vector;auto refused=vector.call("clip.destructive");ASSERT_TRUE(refused.error);EXPECT_EQ(refused.error->code,"unsupported-target");
}

TEST(M3Clip, CancellationAndHistoryFailureRollBackWithoutSuccessData) {
    for(auto command:{"clip.set","clip.destructive"}) {
        ClipFixture p(std::string(command)=="clip.destructive");auto before=p.xml();
        auto cancelled=p.call(command,{},false,true);EXPECT_EQ(cancelled.status,Status::Cancelled);EXPECT_TRUE(cancelled.data.empty());EXPECT_TRUE(cancelled.modified.empty());EXPECT_EQ(before,p.xml());
        DocumentUndo::setAtomicSettlementFaultForTesting([](DocumentUndo::AtomicSettlementStage stage){return stage==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});
        auto failed=p.call(command);
        DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);
        EXPECT_EQ(failed.status,Status::Failed);EXPECT_TRUE(failed.data.empty());EXPECT_TRUE(failed.modified.empty());EXPECT_EQ(failed.error_details.at("mutation_state"),"rolled-back");
        EXPECT_EQ(before,p.xml());EXPECT_FALSE(DocumentUndo::undo(p.files.document.get()));
    }
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
    Record call(std::string command,boost::json::object params,bool dry=false,bool cancel=false) {
        auto &doc=*files.document;EditServices edits{doc,*doc.getSelection(),document_stamp(&doc),{}};
        ProductionContext context{files,grants,edits,tokens,{}};context.session_id="P9-native";context.catalog_identity="P9-oracle";
        context.incarnation=1;context.target_generation=1;context.nest=&owner;
        dispatch.cancelled=[cancel]{return cancel;};
        Request request;request.command=std::move(command);request.params=std::move(params);request.dry_run=dry;
        return execute_clip(request,dispatch,context);
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
    void error(std::string const &cmd,boost::json::object const &params,std::string const &code,std::string const &branch) {
        auto before=state();auto r=call(cmd,params);auto after=state();
        ASSERT_TRUE(r.error)<<r.message;ASSERT_EQ(r.error->code,code)<<r.message;
        ASSERT_EQ(r.error_details.at("reason"),boost::json::value(branch));EXPECT_EQ(before,after);
        EXPECT_FALSE(r.one_undo_step);EXPECT_TRUE(r.data.empty());
        ASSERT_FALSE(DocumentUndo::interactionActive(files.document.get()));
        // Branch comes from the actual native response; expected ID remains independently authored.
        boost::json::object record{{"obligation","P9:"+cmd+":command-service:"+code+":"+branch},
            {"command",r.action},{"code",r.error->code},{"branch",r.error_details.at("reason")},
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

TEST(P9Independent, clip_set) {
    P9Native p("clip");boost::json::object args{{"target-id","shape1"},{"cutter-id","cutter1"},{"keep-cutter",true}};
    p.preview_cancel("clip.set",args);auto before=p.state();auto untouched=p.untouched();
    auto r=p.call("clip.set",args);ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_TRUE(r.one_undo_step);
    auto const &relations=r.data.at("relations").as_array();ASSERT_EQ(relations.size(),1u);
    auto const &row=relations[0].as_object();EXPECT_EQ(row.at("target-id"),"shape1");EXPECT_EQ(row.at("cutter-id"),"cutter1");
    EXPECT_EQ(row.at("target-retained"),true);EXPECT_EQ(row.at("cutter-retained"),true);
    EXPECT_EQ(p.untouched(),untouched);auto after=p.state();p.undo_redo(before,after);
}
TEST(P9Independent, clip_release) {
    P9Native p("clip");ASSERT_EQ(p.call("clip.set",{{"target-id","shape1"},{"cutter-id","cutter1"},{"keep-cutter",true}}).status,Status::Changed);
    boost::json::object args{{"ids",boost::json::array{"shape1"}},{"keep-cutter",true}};
    p.preview_cancel("clip.release",args);auto before=p.state();auto untouched=p.untouched();
    auto r=p.call("clip.release",args);ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_TRUE(r.one_undo_step);
    auto const &relations=r.data.at("relations").as_array();ASSERT_EQ(relations.size(),1u);
    auto const &row=relations[0].as_object();EXPECT_EQ(row.at("target-id"),"shape1");
    EXPECT_EQ(row.at("target-retained"),true);EXPECT_EQ(row.at("cutter-retained"),true);
    EXPECT_EQ(r.created.size(),1u);EXPECT_EQ(p.untouched(),untouched);p.undo_redo(before,p.state());
}
TEST(P9Independent, clip_destructive) {
    P9Native p("clip");boost::json::object args{{"target-id","image1"},{"cutter-id","cutter1"},{"keep-cutter",true}};
    p.preview_cancel("clip.destructive",args);auto before=p.state();auto untouched=p.untouched();
    auto cutter=p9_node(p.files.document->getObjectById("cutter1")->getRepr());
    auto r=p.call("clip.destructive",args);ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_TRUE(r.one_undo_step);
    EXPECT_EQ(r.data.at("cutter-retained"),true);EXPECT_EQ(r.data.at("straightened"),false);
    // Published bounds: opaque islands [10,30), [50,70),
    // [90,110) at y=[20,40), intersect cutter x=[0,60); trim => [10,20,60,40].
    EXPECT_EQ(r.data.at("bounds"),boost::json::value(boost::json::array{10.,20.,60.,40.}));
    EXPECT_EQ(p9_node(p.files.document->getObjectById("cutter1")->getRepr()),cutter);
    EXPECT_EQ(p.untouched(),untouched);p.undo_redo(before,p.state());
}

#include "object/sp-image.h"
#include "display/cairo-utils.h"
#include "xml/node-observer.h"
namespace {
boost::json::object p9_pair(char const *target="shape1",char const *cutter="cutter1",bool keep=true,bool inverse=false) {
    return {{"target-id",target},{"cutter-id",cutter},{"keep-cutter",keep},{"inverse",inverse}};
}
struct P9SettlementFault {
    P9SettlementFault(){DocumentUndo::setAtomicSettlementFaultForTesting([](auto stage){return stage==DocumentUndo::AtomicSettlementStage::HistoryInsertion;});}
    ~P9SettlementFault(){DocumentUndo::setAtomicSettlementFaultForTesting(nullptr);}
};
}
TEST(P9D12, clip_set_invalid_item) {
    P9Native p("clip");p.files.document->getReprRoot()->setAttribute("id","p9root");p.checkpoint();
    p.error("clip.set",p9_pair("p9root"),"unknown-id","ClipDocumentService::Reason::InvalidItem");
}
TEST(P9D12, clip_set_protected) {
    P9Native p("clip");p.files.document->getObjectById("shape1")->setAttribute("style","display:none");p.checkpoint();
    p.error("clip.set",p9_pair(),"unavailable","ClipDocumentService::Reason::ProtectedItem");
}
TEST(P9D12, clip_set_singular) {
    P9Native p("clip");p.files.document->getObjectById("shape1")->setAttribute("transform","scale(0,1)");p.checkpoint();
    p.error("clip.set",p9_pair(),"invalid-transform","ClipDocumentService::Reason::SingularTransform");
}
TEST(P9D12, clip_set_overlap) {
    P9Native p("clip");p.error("clip.set",p9_pair("shape1","shape1"),"duplicate-id","ClipDocumentService::Reason::OverlappingRoles");
}
TEST(P9D12, clip_set_missing_source) {
    P9Native p("clip");p.error("clip.set",p9_pair("shape1","broken-clone"),"unsupported-target","ClipDocumentService::Reason::MissingSource");
}
TEST(P9D12, clip_set_referenced_cutter) {
    P9Native p("clip");p.error("clip.set",p9_pair("shape1","cutter1",false),"unsupported-target","ClipDocumentService::Reason::ReferencedCutter");
}
TEST(P9D12, clip_set_clone_source) {
    P9Native p("clip");p.error("clip.set",p9_pair("cutter-reference"),"unsupported-target","ClipDocumentService::Reason::CloneWithSource");
}
TEST(P9D12, clip_set_inverse_target) {
    P9Native p("clip");p.error("clip.set",p9_pair("image1","cutter1",true,true),"unsupported-target","ClipDocumentService::Reason::UnsupportedInverseTarget");
}
TEST(P9D12, clip_set_inverse_cutter) {
    P9Native p("clip");p.error("clip.set",p9_pair("shape1","image1",true,true),"unsupported-target","ClipDocumentService::Reason::UnsupportedInverseCutter");
}
TEST(P9D12, clip_set_existing_powerclip) {
    P9Native p("clip");ASSERT_EQ(p.call("clip.set",p9_pair("shape1","cutter1",true,true)).status,Status::Changed);
    p.error("clip.set",p9_pair(),"unsupported-target","ClipDocumentService::Reason::ExistingPowerClip");
}
TEST(P9D12, clip_set_history_fault) {
    P9Native p("clip");P9SettlementFault fault;
    p.error("clip.set",p9_pair(),"internal-error","unexpected service exception; rollback before return");
}
TEST(P9D12, clip_release_invalid_item) {
    P9Native p("clip");p.files.document->getReprRoot()->setAttribute("id","p9root");p.checkpoint();
    p.error("clip.release",{{"ids",boost::json::array{"p9root"}}},"unknown-id","ClipDocumentService::Reason::InvalidItem");
}
TEST(P9D12, clip_release_protected) {
    P9Native p("clip");ASSERT_EQ(p.call("clip.set",p9_pair()).status,Status::Changed);
    p.files.document->getObjectById("shape1")->setAttribute("style","display:none");p.checkpoint();
    p.error("clip.release",{{"ids",boost::json::array{"shape1"}}},"unavailable","ClipDocumentService::Reason::ProtectedItem");
}
TEST(P9D12, clip_release_not_clipped) {
    P9Native p("clip");p.error("clip.release",{{"ids",boost::json::array{"shape1"}}},"no-eligible-targets","ClipDocumentService::Reason::NotClipped");
}
TEST(P9D12, clip_release_history_fault) {
    P9Native p("clip");ASSERT_EQ(p.call("clip.set",p9_pair()).status,Status::Changed);P9SettlementFault fault;
    p.error("clip.release",{{"ids",boost::json::array{"shape1"}},{"keep-cutter",true}},"internal-error","unexpected service exception; rollback before return");
}
TEST(P9D12, clip_destructive_invalid_selection) {
    P9Native p("clip");p.error("clip.destructive",p9_pair(),"unsupported-target","InvalidSelection");
}
TEST(P9D12, clip_destructive_invalid_geometry) {
    P9Native p("clip");p.files.document->getObjectById("image1")->setAttribute("transform","scale(0,1)");p.checkpoint();
    p.error("clip.destructive",p9_pair("image1"),"invalid-input","InvalidGeometry");
}
TEST(P9D12, clip_destructive_empty_straightening) {
    P9Native p("clip");p.files.document->getObjectById("image1")->setAttribute("transform","translate(1000,1000) rotate(30)");p.checkpoint();
    p.error("clip.destructive",p9_pair("image1"),"straightening-empty-region","StraighteningEmptyRegion");
}
TEST(P9D12, clip_destructive_large_straightening) {
    P9Native p("clip");auto image=p.files.document->getObjectById("image1");image->setAttribute("transform","rotate(30)");
    image->setAttribute("width","10000");image->setAttribute("height","0.01");image->setAttribute("preserveAspectRatio","none");p.checkpoint();
    p.error("clip.destructive",p9_pair("image1","cutter1",true,true),"engine-limit","StraighteningTooLarge");
}
TEST(P9D12, clip_destructive_encoding) {
    P9Native p("clip");auto *image=cast<SPImage>(p.files.document->getObjectById("image1"));
    auto poisoned=std::make_shared<Pixbuf>(*image->pixbuf);ASSERT_TRUE(gdk_pixbuf_set_option(poisoned->getPixbufRaw(false),"icc-profile","AAAA"));image->pixbuf=poisoned;
    p.error("clip.destructive",p9_pair("image1"),"encoding-failed","EncodingFailed");
}
TEST(P9D12, clip_destructive_history_fault) {
    P9Native p("clip");P9SettlementFault fault;
    p.error("clip.destructive",p9_pair("image1"),"internal-error","unexpected service exception; rollback before return");
}
TEST(P9D12, clip_destructive_rasterization_fault) {
    P9Native p("clip");struct Observer final:XML::NodeObserver {
        bool armed=true;
        void notifyAttributeChanged(XML::Node &,GQuark,Util::ptr_shared,Util::ptr_shared) override {
            if(armed){armed=false;throw std::runtime_error("P9 publication observer fault");}
        }
    } observer;
    auto repr=p.files.document->getObjectById("image1")->getRepr();repr->addObserver(observer);
    p.error("clip.destructive",p9_pair("image1"),"rasterization-failed","RasterizationFailed");repr->removeObserver(observer);
}

TEST(P9D12, clip_destructive_unsupported_trim) {
    P9Native p("clip");
    auto *pixels=gdk_pixbuf_new(GDK_COLORSPACE_RGB,true,8,8,8);gdk_pixbuf_fill(pixels,0x336699ff);
    gchar *buffer=nullptr;gsize size=0;ASSERT_TRUE(gdk_pixbuf_save_to_buffer(pixels,&buffer,&size,"png",nullptr,nullptr));
    gchar *encoded=g_base64_encode(reinterpret_cast<guchar const *>(buffer),size);
    auto image=p.files.document->getObjectById("image1");
    image->setAttribute("xlink:href",("data:image/png;base64,"+std::string(encoded)).c_str());
    g_free(encoded);g_free(buffer);g_object_unref(pixels);
    image->setAttribute("x","1");image->setAttribute("y","1");image->setAttribute("width","8");image->setAttribute("height","4");
    image->setAttribute("preserveAspectRatio","xMidYMid slice");
    auto cutter=p.files.document->getObjectById("cutter1");cutter->setAttribute("x","0");cutter->setAttribute("y","6");
    cutter->setAttribute("width","10");cutter->setAttribute("height","2");p.checkpoint();
    // Removing hidden source row 7 would expose rows outside the slice viewport.
    p.error("clip.destructive",p9_pair("image1","cutter1",true,true),"unsupported-target","UnsupportedTrim");
}
