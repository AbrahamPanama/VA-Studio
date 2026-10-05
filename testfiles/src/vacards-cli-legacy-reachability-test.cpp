// SPDX-License-Identifier: GPL-2.0-or-later
// Native reachability receipts for legacy file.* rows.
#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <iostream>
#include <functional>
#include <glib.h>
#include "actions/actions-vacards-cli.h"
#include "actions/vacards-cli-dispatch.h"
#include "actions/vacards-cli-production.h"
#include "actions/vacards-cli-session.h"
#include "actions/vacards-cli-edit-services.h"
#include "actions/actions-vacards-bitmap.h"
#include "nesting/nesting-cli-service.h"
#include "async/bitmap-job-reaper.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "selection.h"
#include "xml/repr.h"
#include "xml/attribute-record.h"
#include "xml/node.h"
#include "io/vacards-cli-files.h"
using namespace Inkscape;
using namespace Inkscape::VACardsCli;
namespace {
using namespace boost::json;
value canonical(XML::Node const *n) {
    std::map<std::string,std::string> attrs;
    for (auto const &a:n->attributeList()) attrs[g_quark_to_string(a.key)]=a.value.pointer();
    object a; for (auto const &[k,v]:attrs) a[k]=v;
    array children; for(auto c=n->firstChild();c;c=c->next())children.push_back(canonical(c));
    return array{n->name()?n->name():"",a,n->content()?n->content():"",children};
}
struct Fixture {
    FileState files; Grants grants; TokenStore tokens; Nesting::CliSession nest; Bitmap::CliSession bitmap;
    DispatchContext context; std::filesystem::path dir; std::string session="INT-FINAL-2";
    Fixture() : tokens(TokenBudget{}) {
        if(!Application::exists())Application::create(false,Application::RuntimePolicy::PreviewHelper);
        Bitmap::recordBitmapMainThread();
        auto tmp=g_dir_make_tmp("int-final2-XXXXXX",nullptr); if(!tmp)throw std::runtime_error("temp fixture creation failed");
        dir=std::filesystem::canonical(tmp);g_free(tmp);grants.read_roots={dir.string()};grants.write_roots={dir.string()};
        write("source.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"><rect id=\"a\" width=\"10\" height=\"10\"/><circle id=\"b\" cx=\"20\" cy=\"20\" r=\"5\"/></svg>");
        auto bytes=read("source.svg");files.document=SPDocument::createNewDocFromMem(std::span<char const>(bytes.data(),bytes.size()));
        if(!files.document)throw std::runtime_error("temp SVG did not create a document");
        auto &doc=*files.document;doc.ensureUpToDate();DocumentUndo::done(&doc,Util::Internal::ContextString("INT-FINAL-2 fixture"),"document-new");
        DocumentUndo::clearUndo(&doc);DocumentUndo::clearRedo(&doc);
        doc.getReprRoot()->setAttribute("data-final2-undo","seed");DocumentUndo::done(&doc,Util::Internal::ContextString("undo seed"),"document-new");
        doc.getReprRoot()->setAttribute("data-final2-redo","seed");DocumentUndo::done(&doc,Util::Internal::ContextString("redo seed"),"document-new");if(!DocumentUndo::undo(&doc))throw std::runtime_error("redo seed could not be undone");
        doc.getSelection()->add(cast<SPItem>(doc.getObjectById("a")));
        context.document=&doc;context.selection=doc.getSelection();context.session_revision=41;
        context.command_admission=[this](Request const &r){return session_command_admission(r,context,files);};
        context.production_handler=[this](Request const &r){
            auto &d=*files.document;EditServices edits{d,*d.getSelection(),document_stamp(&d),{}};
            ProductionContext p{files,grants,edits,tokens,[]{},session,"INT-FINAL-2",1,1};p.nest=&nest;p.bitmap=&bitmap;
            return execute_production(r,context,p);
        };
        context.file_handler=[this](Request const &r){return execute_file(r,context,files,grants);};
    }
    ~Fixture(){bitmap.retire();nest.retire();std::error_code ec;std::filesystem::remove_all(dir,ec);}
    std::string write(std::string const &name,std::string const &bytes){auto p=dir/name;std::ofstream o(p,std::ios::binary);o<<bytes;o.close();return p.string();}
    std::string read(std::string const &name){std::ifstream i(dir/name,std::ios::binary);return {std::istreambuf_iterator<char>(i),{}};}
    object state(){auto &d=*files.document;d.ensureUpToDate();EditServices e{d,*d.getSelection(),document_stamp(&d),{}};auto h=history_snapshot(e);if(!h.value)throw std::runtime_error("history snapshot unavailable");
        array sel,ids;for(auto i:d.getSelection()->items())sel.emplace_back(i->getId());auto t=tokens.snapshot();for(auto const &id:t.active_tokens)ids.emplace_back(id);
        auto label=[](auto const &x)->value{return x?value(*x):value(nullptr);};
        auto doctype=static_cast<XML::Node *>(d.getReprDoc())->attribute("doctype");
        return {{"xml",canonical(d.getReprRoot())},{"doctype",doctype?value(doctype):value(nullptr)},{"selection",sel},{"document_revision",document_stamp(&d).revision},{"session_revision",context.session_revision},
          {"undo",object{{"available",h.value->can_undo},{"label",label(h.value->next_undo_label)}}},
          {"redo",object{{"available",h.value->can_redo},{"label",label(h.value->next_redo_label)}}},
          {"tokens",object{{"ids",ids},{"roots",t.active_roots},{"unique-held-bytes",t.charged_bytes}}}};
    }
};
object wire(Request const &r){object p=r.params;return {{"schema","va-studio.cli-request/1"},{"id",r.id},{"command",r.command},{"document",*r.document},{"if_revision",*r.if_revision},{"params",p},{"dry_run",r.dry_run}};}
std::string row_citations(std::string const &id){
    auto source=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    auto path=source/"work/cli-b31/evidence/P9/r4-proposal-decisions.json";
    std::ifstream in(path);if(!in)return "source/src/io/vacards-cli-files.cpp:189";
    auto data=parse(std::string(std::istreambuf_iterator<char>(in),{}));
    for(auto const &item:data.as_object().at("decisions").as_array())if(std::string(item.as_object().at("id").as_string())==id){
        std::string out;for(auto const &c:item.as_object().at("citations").as_array()){if(!out.empty())out+=", ";auto cite=std::string(c.as_string());if(cite=="source/src/object/sp-object.cpp:1027")cite="source/src/object/sp-object.cpp:1035";out+=cite;}return out;
    }
    return "source/src/io/vacards-cli-files.cpp:189";
}
void run_case(std::string const &id,std::string const &input_class,std::string const &field,std::string const &value,
              std::string const &expected_status,std::string const &expected_code,bool active_transaction=false,
              bool doctype=false,bool duplicate_ids=false) {
    Fixture f;auto colon=id.find(':');auto slash=id.find('/',colon+1);auto command=id.substr(colon+1,slash-colon-1);auto code=id.substr(slash+1);
    auto spec=find_command(command);ASSERT_NE(spec,nullptr)<<command;auto parsed=parse_production_request(spec->example);ASSERT_TRUE(parsed.request);
    Request r=*parsed.request;r.id="INT-FINAL-2";r.command=command;r.document=document_stamp(f.context.document).id;
    r.if_revision=spec->needs_document?document_stamp(f.context.document).revision:f.context.session_revision;r.dry_run=!active_transaction;
    if(r.params.contains("path"))r.params["path"]=(f.dir/"output.svg").string();
    if(field=="page-invalid"){r.params.erase("drawing");r.params["page"]=2;}
    else if(field=="path-relative")r.params["path"]="relative.svg";
    else if(field=="path-directory")r.params["path"]=f.dir.string();
    else if(field=="path-missing-source")r.params["path"]=(f.dir/"absent.svg").string();
    else if(field=="path-svgz")r.params["path"]=(f.dir/"output.svgz").string();
    else if(field=="path-invalid-svg")r.params["path"]=f.write("invalid.svg","<not-svg/>");
    else if(field=="path-invalid-svgz")r.params["path"]=f.write("invalid.svgz","not a gzip stream");
    else if(!field.empty()) r.params[field]=(field=="pages"||field=="profile")?parse(value):boost::json::value(value);
    if(command=="file.open"||command=="file.new"||command=="file.close")r.params["discard"]=true;
    if(command=="file.open"||command=="file.import") {
        if(field!="path-missing-source"&&field!="path-directory")r.params["path"]=(f.dir/"source.svg").string();
    }
    if(doctype){static_cast<XML::Node *>(f.context.document->getReprDoc())->setAttribute("doctype","<!DOCTYPE svg [<!ENTITY final2 'x'>]>\n");DocumentUndo::done(f.context.document,Util::Internal::ContextString("DTD input"),"");r.if_revision=document_stamp(f.context.document).revision;}
    if(duplicate_ids){f.context.document->getObjectById("a")->getRepr()->setAttribute("id","same");f.context.document->getObjectById("b")->getRepr()->setAttribute("id","same");DocumentUndo::done(f.context.document,Util::Internal::ContextString("duplicate ID input"),"");r.if_revision=document_stamp(f.context.document).revision;}
    auto before=f.state();
    if(doctype)ASSERT_FALSE(before.at("doctype").is_null());
    if(duplicate_ids){unsigned count=0;std::function<void(boost::json::value const &)> visit=[&](boost::json::value const &node){auto const &a=node.as_array();if(a[1].as_object().if_contains("id")&&std::string(a[1].as_object().at("id").as_string())=="same")++count;for(auto const &child:a[3].as_array())visit(child);};visit(before.at("xml"));ASSERT_EQ(count,1u);}
    auto request_wire=wire(r);Record result;
    if(active_transaction){EditTransaction outer(f.context.document,f.context.selection);ASSERT_TRUE(outer.active());result=dispatch(r,f.context);outer.rollback();}
    else result=dispatch(r,f.context);
    auto typed=typed_result(result,r.id,1);auto status=std::string(typed.at("status").as_string());auto actual_code=typed.if_contains("error")?std::string(typed.at("error").as_object().at("code").as_string()):"success";
    auto mutation=result.error_details.if_contains("mutation_state")?std::string(result.error_details.at("mutation_state").as_string()):"none";
    auto after=f.state();bool preserved=before==after;ASSERT_TRUE(preserved)<<"native file route changed guarded state";
    object observation{{"id",id},{"command",command},{"request",request_wire},{"input_class",input_class},
      {"observed",object{{"status",status},{"code",actual_code},{"mutation_state",mutation},{"success_variant",typed.if_contains("data")?*typed.if_contains("data"):boost::json::value(nullptr)}}},{"typed_result",typed},{"preserved",preserved},
      {"argument", "The real file command route observed this typed outcome and preserved XML, selection, revisions, Undo/Redo, and tokens; proposal branch citations at HEAD: "+row_citations(id)}};
    if(actual_code==code)std::cout<<"LEGACY-REACHABLE "<<serialize(observation)<<std::endl;
    else std::cout<<"LEGACY-COUNTEREXAMPLE "<<serialize(observation)<<std::endl;
    ASSERT_EQ(status,expected_status)<<serialize(typed);ASSERT_EQ(actual_code,expected_code)<<serialize(typed);
    ASSERT_EQ(result.action,command);ASSERT_FALSE(DocumentUndo::interactionActive(f.context.document));
    if(expected_status=="rejected"||expected_status=="failed")ASSERT_FALSE(result.one_undo_step);
}
}
#define LEGACY(name,id,ic,field,val,status,code) TEST(Legacy,name){run_case(id,ic,field,val,status,code);}
#define LEGACY_TX(name,id,ic,field,val,status,code) TEST(Legacy,name){run_case(id,ic,field,val,status,code,true);}
#define LEGACY_XML(name,id,ic,field,val,status,code) TEST(Legacy,name){run_case(id,ic,field,val,status,code,false,true,false);}
#define LEGACY_IDS(name,id,ic,field,val,status,code) TEST(Legacy,name){run_case(id,ic,field,val,status,code,false,false,true);}
LEGACY(file_close_not_a_choice,"error:file.close/not-a-choice","out-of-domain choice on unrelated file command","format","bogus","rejected","unknown-key")
LEGACY(file_close_resource_unavailable,"error:file.close/resource-unavailable","source resource path supplied to lifecycle command","path","/outside/source.svg","rejected","unknown-key")
LEGACY(file_close_publication_unavailable,"error:file.close/publication-unavailable","destination path supplied to lifecycle command","path","/outside/output.svg","rejected","unknown-key")
LEGACY(file_export_format_unsupported,"error:file.export/format-unsupported","unsupported source format token on snapshot export","format","cdr","rejected","not-a-choice")
LEGACY(file_export_pages_invalid,"error:file.export/pages-invalid","requested page absent from source document","page-invalid","","rejected","invalid-target")
LEGACY(file_export_font_policy_required,"error:file.export/font-policy-required","font rejection policy on snapshot export","font-policy","reject","rejected","unknown-key")
LEGACY(file_export_invalid_path,"error:file.export/invalid-path","relative output destination","path-relative","","rejected","unsafe-destination")
LEGACY(file_export_missing_file,"error:file.export/missing-file","nonexistent output destination is not a source read","path-missing-source","","ok","success")
LEGACY(file_export_read_failed,"error:file.export/read-failed","directory used as output destination","path-directory","","rejected","unsafe-destination")
LEGACY_XML(file_export_unsafe_xml,"error:file.export/unsafe-xml","live document DTD passed to copied snapshot","","","ok","success")
LEGACY(file_export_invalid_svgz,"error:file.export/invalid-svgz","invalid gzip bytes supplied at SVGZ destination","path-invalid-svgz","","rejected","publication-conflict")
LEGACY_IDS(file_export_duplicate_id,"error:file.export/duplicate-id","duplicate ID attempt normalized by native object binding before snapshot","","","ok","success")
LEGACY(file_export_invalid_svg,"error:file.export/invalid-svg","invalid SVG bytes supplied as export destination","path-invalid-svg","","rejected","publication-conflict")
LEGACY_TX(file_import_transaction_unavailable,"error:file.import/transaction-unavailable","active native edit transaction during import","","","rejected","document-busy")
LEGACY(file_import_read_failed,"error:file.import/read-failed","missing granted source path","path-missing-source","","rejected","read-grant-denied")
LEGACY(file_import_publication_unavailable,"error:file.import/publication-unavailable","output destination class submitted as import source","path-directory","","rejected","read-grant-denied")
LEGACY(file_new_resource_unavailable,"error:file.new/resource-unavailable","source path supplied to document creation","path","/outside/source.svg","rejected","unknown-key")
LEGACY(file_new_publication_unavailable,"error:file.new/publication-unavailable","destination path supplied to document creation","path","/outside/output.svg","rejected","unknown-key")
LEGACY(file_open_read_failed,"error:file.open/read-failed","missing granted source path","path-missing-source","","rejected","read-grant-denied")
LEGACY(file_open_publication_unavailable,"error:file.open/publication-unavailable","valid granted source opens without publication","","","ok","success")
LEGACY(file_save_read_grant_denied,"error:file.save/read-grant-denied","source grant request on snapshot save","profile","{\"id\":\"file\",\"path\":\"/outside/profile.icc\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}","rejected","unknown-key")
LEGACY(file_save_format_unsupported,"error:file.save/format-unsupported","format token supplied to SVG-only snapshot save","format","cdr","rejected","unknown-key")
LEGACY(file_save_pages_invalid,"error:file.save/pages-invalid","page list supplied to SVG-only snapshot save","pages","[0]","rejected","unknown-key")
LEGACY(file_save_font_policy_required,"error:file.save/font-policy-required","font policy supplied to SVG-only snapshot save","font-policy","reject","rejected","unknown-key")
LEGACY(file_save_save_failed,"error:file.save/save-failed","directory supplied as snapshot destination","path-directory","","rejected","unsafe-destination")
LEGACY(file_save_invalid_path,"error:file.save/invalid-path","relative output destination","path-relative","","rejected","unsafe-destination")
LEGACY(file_save_missing_file,"error:file.save/missing-file","nonexistent output destination is not a source read","path-missing-source","","ok","success")
LEGACY(file_save_read_failed,"error:file.save/read-failed","directory supplied as snapshot destination","path-directory","","rejected","unsafe-destination")
LEGACY_XML(file_save_unsafe_xml,"error:file.save/unsafe-xml","live document DTD passed to copied snapshot","","","ok","success")
LEGACY(file_save_invalid_svgz,"error:file.save/invalid-svgz","invalid gzip bytes supplied at SVGZ destination","path-invalid-svgz","","rejected","publication-conflict")
LEGACY_IDS(file_save_duplicate_id,"error:file.save/duplicate-id","duplicate ID attempt normalized by native object binding before snapshot","","","ok","success")
LEGACY(file_save_invalid_svg,"error:file.save/invalid-svg","invalid SVG bytes supplied as save destination","path-invalid-svg","","rejected","publication-conflict")
class RealRunCreateFailure final : public IO::DocumentTransaction::SystemCalls {
public:
    std::unique_ptr<IO::DocumentTransaction::SystemCalls> native=IO::DocumentTransaction::make_platform_system_calls();
    bool create_exclusive_file(std::string &,FILE *&,bool &exists,std::string &error) override {
        exists=false;error="INT real non-dry-run destination create failure";return false;
    }
    bool flush_file(FILE *f,std::string &e) override{return native->flush_file(f,e);}
    bool sync_file(FILE *f,bool &u,std::string &e) override{return native->sync_file(f,u,e);}
    bool close_file(FILE *f,std::string &e) override{return native->close_file(f,e);}
    IO::DocumentTransaction::PublicationStatus publish_new_file(std::string const &s,std::string const &p,std::string &e) override{return native->publish_new_file(s,p,e);}
    bool remove_file(std::string const &p) noexcept override{return native->remove_file(p);}
    bool sync_parent_directory(std::string const &p,bool &u,std::string &e) override{return native->sync_parent_directory(p,u,e);}
};
TEST(Legacy, PublicationPhaseLegacyRowsAreRecheckedWithRealExecution) {
    struct Case { char const *id; char const *command; char const *kind; };
    for (auto const &test : {Case{"error:file.save/save-failed","file.save","path-directory"},
                             Case{"error:file.save/read-grant-denied","file.save","profile-denied"},
                             Case{"error:file.save/invalid-path","file.save","path-relative"},
                             Case{"error:file.export/invalid-path","file.export","path-relative"}}) {
        Fixture f; auto parsed=parse_production_request(find_command(test.command)->example);
        ASSERT_TRUE(parsed.request); auto r=*parsed.request; r.id="INT-FINAL-1-real";
        r.document=document_stamp(f.context.document).id; r.if_revision=document_stamp(f.context.document).revision;
        r.dry_run=false;
        std::string destination=(f.dir/"real-output.svg").string();
        RealRunCreateFailure fail_calls; FileServiceTestHooks hooks; bool forced_create_failure=false;
        if(std::string(test.kind)=="path-directory") {
            r.params["path"]=destination; hooks.calls=&fail_calls; forced_create_failure=true;
            f.context.file_handler=[&](Request const &request){return execute_file_for_testing(request,f.context,f.files,f.grants,hooks);};
        }
        else if(std::string(test.kind)=="path-relative") r.params["path"]="relative.svg";
        else { r.params["path"]=destination; r.params["profile"]=object{{"id","file"},{"path","/outside/profile.icc"},{"sha256",std::string(64,'0')}}; }
        auto result=dispatch(r,f.context); auto typed=typed_result(result,r.id,1);
        auto actual=typed.if_contains("error")?std::string(typed.at("error").as_object().at("code").as_string()):"success";
        std::cout<<"INT-FINAL-1-REAL "<<serialize(object{{"id",test.id},{"command",test.command},
            {"dry_run",false},{"actual_code",actual},{"status",typed.at("status")},
            {"controlled_native_create_failure",forced_create_failure},
            {"destination",r.params.if_contains("path")?*r.params.if_contains("path"):boost::json::value(nullptr)},
            {"destination_exists",std::filesystem::exists(destination)},{"result",typed}})<<std::endl;
        ASSERT_FALSE(DocumentUndo::interactionActive(f.context.document));
        ASSERT_FALSE(result.one_undo_step);
    }
}
#undef LEGACY
#undef LEGACY_TX
#undef LEGACY_XML
#undef LEGACY_IDS
