// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <glib.h>
#include <zlib.h>
#include <tiffio.h>
#include <libxml/parser.h>
#include "object/sp-image.h"
#include "io/export-color-profiles.h"
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
// Win32 macros collide with gtkmm enum and member names below.
#undef IGNORE
#undef near
#else
#include <sys/stat.h>
#endif
#include "actions/actions-vacards-file.h"
#include "actions/vacards-cli-edit-services.h"
#include "actions/vacards-cli-fault-testing.h"
#include "io/vacards-cli-files.h"
#include "io/vacards-cli-import.h"
#include "io/vacards-cli-intake.h"
#include "io/vacards-cli-intake-testing.h"
#include "io/existing-file-replacement.h"
#include "document-undo.h"
#include "event-log.h"
#include "inkscape.h"
#include "selection.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "xml/repr.h"
#include "page-manager.h"
#include "object/sp-page.h"
#include "util/units.h"
#include "extension/system.h"
#include <sstream>
#include <iomanip>
#include <cstring>
#include <map>
using namespace Inkscape;
using namespace Inkscape::VACardsCli;
using namespace boost::json;
namespace {
#ifdef _WIN32
// MinGW remove_all can spin in _wstat for trees beyond MAX_PATH, even when
// passed an extended root. Use Win32 enumeration and never follow reparse points.
bool remove_windows_tree(std::wstring const &path) {
 WIN32_FIND_DATAW entry{};
 HANDLE search=FindFirstFileW((path+L"\\*").c_str(),&entry);
 if(search==INVALID_HANDLE_VALUE) return false;
 bool ok=true;
 do {
  std::wstring name=entry.cFileName;
  if(name==L"." || name==L"..") continue;
  auto child=path+L"\\"+name;
  if(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
   if(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ok=RemoveDirectoryW(child.c_str()) && ok;
   else ok=remove_windows_tree(child) && ok;
  } else ok=DeleteFileW(child.c_str()) && ok;
 } while(FindNextFileW(search,&entry));
 ok=(GetLastError()==ERROR_NO_MORE_FILES) && ok;
 FindClose(search);
 return RemoveDirectoryW(path.c_str()) && ok;
}
#endif
class Files : public ::testing::Test {
protected:
 std::string dir; FileState state; DispatchContext context; Grants grants;
 void SetUp() override {
  if (!Application::exists()) Application::create(false);
  auto p=g_dir_make_tmp("va-m2-native-XXXXXX",nullptr); ASSERT_NE(p,nullptr); dir=std::filesystem::canonical(p).string();g_free(p);
  grants.read_roots={dir};grants.write_roots={dir};
 }
 void TearDown() override {
  state.document.reset();
#ifdef _WIN32
  // The suite creates paths beyond MAX_PATH; keep cleanup extended as well.
  auto path=std::filesystem::u8path(dir);path.make_preferred();
  EXPECT_TRUE(remove_windows_tree(L"\\\\?\\"+path.wstring())) << GetLastError();
#else
  std::filesystem::remove_all(dir);
#endif
 }
 std::string write(std::string name,std::string bytes) { auto path=dir+"/"+name;std::ofstream f(path,std::ios::binary);f<<bytes;return path; }
 std::string read(std::string path) {std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
 Record run(std::string command,object params={},bool dry=false) {
  Request r; r.command=command;r.params=std::move(params);r.dry_run=dry;
  return execute_file(r,context,state,grants);
 }
 object length(double v) {return {{"value",v},{"unit","mm"}};}
 object dims() {return {{"width",length(100)},{"height",length(50)}};}
 object load(std::string path) {return {{"path",path},{"format","auto"},{"resource-policy","embed"},{"font-policy","reject"}};}
 std::string sheet() {return write("sheet.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"50\"><defs/><g id=\"outer\" transform=\"translate(3,4)\"><g id=\"inner\"><rect id=\"red\" width=\"20\" height=\"10\" fill=\"red\"/></g></g></svg>");}
 std::string xml() {return sp_repr_save_buf(context.document->getReprDoc()).raw();}
 // Live-document state that a P9-OBSERVATION before/after pair must carry.
 object p9_state() {
  auto &doc=*context.document; EditServices edits{doc,*doc.getSelection(),document_stamp(&doc),{}};
  auto history=history_snapshot(edits); if(!history.value) throw std::runtime_error("metadata replay history snapshot unavailable");
  auto label=[](auto const &maybe_label)->boost::json::value {return maybe_label?boost::json::value(*maybe_label):boost::json::value(nullptr);};
  array selected; for(auto item:context.document->getSelection()->items()) selected.emplace_back(item->getId());
  return object{{"xml",xml()},{"selection",selected},{"document_revision",document_stamp(context.document).revision},
   {"session_revision",context.session_revision},{"undo",object{{"available",history.value->can_undo},{"label",label(history.value->next_undo_label)}}},
   {"redo",object{{"available",history.value->can_redo},{"label",label(history.value->next_redo_label)}}},
   {"tokens",object{{"ids",array{}},{"roots",0},{"unique-held-bytes",0}}}};
 }
};
// Byte-heavy, object-light PNG: a valid 1x1 image plus ignored ancillary chunks after IDAT.
// Native decoding stays tiny, while the real base64 payload crosses both old 64 MiB ceilings.
std::string heavy_png() {
 std::string png("\x89PNG\r\n\x1a\n",8);
 auto be=[&](std::uint32_t n){for(int shift=24;shift>=0;shift-=8) png+=char(n>>shift);};
 auto chunk=[&](char const *type,std::string const &data) {
  be(data.size());auto start=png.size();png.append(type,4);png+=data;
  be(crc32(0,reinterpret_cast<Bytef const *>(png.data()+start),4+data.size()));
 };
 std::string ihdr("\0\0\0\1\0\0\0\1\x08\x06\0\0\0",13);chunk("IHDR",ihdr);
 char pixel[]={0,char(255),0,0,char(255)};char compressed[64];uLongf n=sizeof compressed;
 EXPECT_EQ(compress2(reinterpret_cast<Bytef *>(compressed),&n,reinterpret_cast<Bytef *>(pixel),sizeof pixel,Z_BEST_SPEED),Z_OK);
 chunk("IDAT",std::string(compressed,n));
 std::string padding(1<<20,'x');
 for(unsigned i=0;i<60;++i) chunk("vaCd",padding);
 chunk("IEND",{});return png;
}
TEST_F(Files, Bug025HeavyEmbeddedImageOpensAndSaves) {
 // Fixed test headroom isolates byte/snapshot boundaries from other apps on this 8 GiB Mac.
 // The one-pixel decode deliberately does not reproduce the owner's multi-GB object workload.
 ScopedIntakeMemoryForTesting headroom(8ull<<30);
 auto path=dir+"/heavy.svg";std::string expected_hash;std::size_t href_size=0;
 {
  auto png=heavy_png();auto encoded=g_base64_encode(reinterpret_cast<guchar const *>(png.data()),png.size());
  std::string href="data:image/png;base64,";href+=encoded;g_free(encoded);href_size=href.size();
  auto hash=g_compute_checksum_for_string(G_CHECKSUM_SHA256,href.c_str(),href.size());expected_hash=hash;g_free(hash);
  std::ofstream out(path,std::ios::binary);
  out<<"<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"100\" height=\"50\"><image id=\"heavy\" width=\"1\" height=\"1\" xlink:href=\""<<href<<"\"/><rect id=\"box\" x=\"2\" y=\"3\" width=\"7\" height=\"11\" fill=\"#123456\"/><circle id=\"dot\" cx=\"20\" cy=\"21\" r=\"4\"/></svg>";
 }
 auto input_bytes=std::filesystem::file_size(path);
 ASSERT_GT(input_bytes,80ull<<20);ASSERT_LT(input_bytes,81ull<<20);
 auto check=[&] {
  auto image=cast<SPImage>(context.document->getObjectById("heavy"));ASSERT_NE(image,nullptr);
  ASSERT_FALSE(image->missing);ASSERT_TRUE(image->pixbuf);
  auto href=image->getRepr()->attribute("xlink:href");ASSERT_NE(href,nullptr);EXPECT_EQ(std::strlen(href),href_size);
  auto hash=g_compute_checksum_for_string(G_CHECKSUM_SHA256,href,-1);EXPECT_EQ(std::string(hash),expected_hash);g_free(hash);
  auto box=context.document->getObjectById("box");ASSERT_NE(box,nullptr);
  for(auto [name,value]:{std::pair{"x","2"},{"y","3"},{"width","7"},{"height","11"},{"fill","#123456"}})
   EXPECT_STREQ(box->getRepr()->attribute(name),value);
  auto dot=context.document->getObjectById("dot");ASSERT_NE(dot,nullptr);
  EXPECT_STREQ(dot->getRepr()->attribute("cx"),"20");EXPECT_STREQ(dot->getRepr()->attribute("cy"),"21");EXPECT_STREQ(dot->getRepr()->attribute("r"),"4");
 };
 auto opened=run("file.open",load(path));ASSERT_EQ(opened.status,Status::Changed)<<opened.reason<<": "<<opened.message;check();
 auto saved=run("file.save",{{"path",dir+"/heavy-saved.svg"},{"embedding-policy","embed"}});
 ASSERT_EQ(saved.status,Status::Changed)<<saved.reason<<": "<<saved.message;
 EXPECT_TRUE(saved.publication_persisted);EXPECT_FALSE(context.document->isModifiedSinceSave());check();
 auto saved_bytes=std::filesystem::file_size(dir+"/heavy-saved.svg");EXPECT_GT(saved_bytes,80ull<<20);
 {
  auto parsed=xmlReadFile((dir+"/heavy-saved.svg").c_str(),nullptr,XML_PARSE_NONET|XML_PARSE_HUGE);
  ASSERT_NE(parsed,nullptr);xmlFreeDoc(parsed);
 }
 ASSERT_EQ(run("file.close").status,Status::Changed);
 auto reopened=run("file.open",load(dir+"/heavy-saved.svg"));ASSERT_EQ(reopened.status,Status::Changed)<<reopened.message;check();
 RecordProperty("input_bytes",std::to_string(input_bytes));RecordProperty("saved_bytes",std::to_string(saved_bytes));
}
TEST_F(Files, Bug025MemoryRefusalBeforeLoadAndSnapshotPublishesNothing) {
 auto input=sheet();
 {
  ScopedIntakeMemoryForTesting no_memory(0);
  auto intake=load_editable_document(input,grants,{"svg","embed","reject",{}});
  ASSERT_TRUE(intake.error);EXPECT_EQ(intake.error->code,"engine-limit");
  EXPECT_EQ(intake.error->details.at("reason"),"insufficient-memory");EXPECT_EQ(intake.error->details.at("phase"),"load");
  EXPECT_EQ(intake.error->details.at("limit"),0);EXPECT_GT(intake.error->details.at("found").to_number<std::uint64_t>(),0u);
  EXPECT_FALSE(intake.document);EXPECT_TRUE(intake.document_id.empty());
  auto opened=run("file.open",load(input));EXPECT_EQ(opened.status,Status::Rejected);EXPECT_EQ(opened.reason,"engine-limit");
  EXPECT_FALSE(context.document);EXPECT_FALSE(state.document);EXPECT_FALSE(opened.publication_persisted);
 }
 ASSERT_EQ(run("file.open",load(input)).status,Status::Changed);
 context.document->getReprRoot()->setAttribute("data-memory-test","retained");
 DocumentUndo::done(context.document,Util::Internal::ContextString("memory test"),"");
 auto before=xml();auto stamp=document_stamp(context.document);auto anchor=context.document->get_event_log()->getLastSavedSerial();
 {
  ScopedIntakeMemoryForTesting no_memory(0);
  auto snapshot=prepare_file_snapshot(*context.document,grants,"embed");ASSERT_TRUE(snapshot.error);
  EXPECT_EQ(snapshot.error->code,"engine-limit");EXPECT_EQ(snapshot.error->details.at("reason"),"insufficient-memory");
  EXPECT_EQ(snapshot.error->details.at("phase"),"save-snapshot");EXPECT_EQ(snapshot.error->details.at("limit"),0);
  EXPECT_EQ(snapshot.error->details.at("factor"),42);EXPECT_EQ(snapshot.error->details.at("reserve_bytes"),256ull<<20);
  EXPECT_FALSE(snapshot.document);
  auto saved=run("file.save",{{"path",dir+"/memory-refused.svg"},{"embedding-policy","embed"}});
  EXPECT_EQ(saved.status,Status::Rejected);EXPECT_EQ(saved.reason,"engine-limit");EXPECT_FALSE(saved.publication_persisted);
  EXPECT_FALSE(std::filesystem::exists(dir+"/memory-refused.svg"));EXPECT_EQ(xml(),before);
  EXPECT_TRUE(context.document->isModifiedSinceSave());EXPECT_FALSE(state.writes_blocked);
  EXPECT_EQ(document_stamp(context.document).id,stamp.id);EXPECT_EQ(document_stamp(context.document).revision,stamp.revision);
  EXPECT_EQ(context.document->get_event_log()->getLastSavedSerial(),anchor);
 }
 auto saved=run("file.save",{{"path",dir+"/memory-admitted.svg"},{"embedding-policy","embed"}});
 EXPECT_EQ(saved.status,Status::Changed)<<saved.message;EXPECT_TRUE(saved.publication_persisted);
}
TEST_F(Files, R3DryRunCancellationAfterPreparation) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);auto before=xml();
 for(auto command:{"file.save","file.export"}) {
  unsigned checks=0;context.cancelled=[&]{return ++checks>=2;};
  object p{{"path",dir+"/dry.svg"}};
  if(std::string(command)=="file.save")p["embedding-policy"]="embed";
  else {p["format"]="png";p["page"]=1;p["profile"]=object{{"id","srgb"}};}
  auto r=run(command,p,true);
  EXPECT_EQ(r.status,Status::Cancelled)<<r.reason;EXPECT_EQ(r.reason,"cancelled");
  EXPECT_GE(checks,2u);EXPECT_EQ(xml(),before);EXPECT_FALSE(r.publication_persisted);
  EXPECT_FALSE(std::filesystem::exists(dir+"/dry.svg"));
 }
}
TEST_F(Files, R3OrientedTiffOpenAndImportGeometry) {
 for(unsigned orientation=1;orientation<=8;++orientation) {
  // Uncompressed 3x2 RGB TIFF; the oracle is the oriented page and image bounds.
  std::string bytes="II";
  auto word=[&](unsigned n){bytes+=char(n);bytes+=char(n>>8);};
  auto dword=[&](unsigned n){word(n);word(n>>16);};
  word(42);dword(8);word(10);
  auto entry=[&](unsigned tag,unsigned type,unsigned count,unsigned value){word(tag);word(type);dword(count);dword(value);};
  entry(256,3,1,3);entry(257,3,1,2);entry(258,3,3,134);entry(259,3,1,1);
  entry(262,3,1,2);entry(273,4,1,140);entry(274,3,1,orientation);entry(277,3,1,3);
  entry(278,4,1,2);entry(279,4,1,18);dword(0);word(8);word(8);word(8);
  bytes.append(18,char(255));auto path=write("oriented.tiff",bytes);
  auto p=load(path);p["discard"]=true;
  auto opened=run("file.open",p);ASSERT_EQ(opened.status,Status::Changed)<<opened.reason;
  double w=orientation>=5?2:3,h=orientation>=5?3:2;
  EXPECT_DOUBLE_EQ(context.document->getWidth().value("px"),w);
  EXPECT_DOUBLE_EQ(context.document->getHeight().value("px"),h);
  auto bounds=context.document->getRoot()->documentVisualBounds();ASSERT_TRUE(bounds);
  EXPECT_NEAR(bounds->width(),w,1e-6);EXPECT_NEAR(bounds->height(),h,1e-6);
  auto fresh=dims();fresh["discard"]=true;ASSERT_EQ(run("file.new",fresh).status,Status::Changed);
  p.erase("discard");p["position"]=object{{"x",length(0)},{"y",length(0)}};
  auto imported=run("file.import",p);ASSERT_EQ(imported.status,Status::Changed)<<imported.reason;
  bounds=context.document->getRoot()->documentVisualBounds();ASSERT_TRUE(bounds);
  EXPECT_NEAR(bounds->width(),w,1e-6);EXPECT_NEAR(bounds->height(),h,1e-6);
 }
}
#ifndef _WIN32
TEST_F(Files, R3LinkedAdmissionAvailabilityIsRetryable) {
 if(geteuid()==0)GTEST_SKIP()<<"Permission test requires non-root user";
 auto private_dir=dir+"/private";std::filesystem::create_directory(private_dir);
 write("private/image.png","placeholder");
 auto path=write("admission.svg","<svg xmlns=\"http://www.w3.org/2000/svg\"><image href=\"private/image.png\" width=\"3\" height=\"2\"/></svg>");
 chmod(private_dir.c_str(),0);
 auto r=run("file.open",load(path));
 chmod(private_dir.c_str(),0700);
 EXPECT_EQ(r.reason,"resource-unavailable");EXPECT_FALSE(state.document);
 auto wire=typed_result(r,"linked-admission");
 EXPECT_TRUE(wire.at("error").as_object().at("retryable").as_bool());
 EXPECT_EQ(wire.at("error").as_object().at("code"),"resource-unavailable");
}
#endif
TEST_F(Files, R3LinkedReadFailuresRetainClassification) {
 auto linked=write("linked.png","placeholder");
 auto path=write("linked.svg","<svg xmlns=\"http://www.w3.org/2000/svg\"><image href=\"linked.png\" width=\"3\" height=\"2\"/></svg>");
 FileLoadOptions options{"svg","embed","reject",{}};
 options.before_linked_read_for_testing=[&](ResourceAccess const &){
#ifndef _WIN32
  std::filesystem::rename(linked,dir+"/old.png");write("linked.png","replacement");
#else
  // Held handles prevent rename; an exclusive reader instead forces availability.
#endif
 };
#ifndef _WIN32
 auto stale=load_editable_document(path,grants,options);ASSERT_TRUE(stale.error);
 EXPECT_EQ(stale.error->code,"stale-dependency");EXPECT_FALSE(stale.error->retryable);EXPECT_FALSE(stale.document);
 if(geteuid()==0) return; // Permission refusal cannot be reproduced as root.
 options.before_linked_read_for_testing=[&](ResourceAccess const &){chmod(linked.c_str(),0);};
 auto unavailable=load_editable_document(path,grants,options);chmod(linked.c_str(),0600);
#else
 HANDLE held=INVALID_HANDLE_VALUE;
 options.before_linked_read_for_testing=[&](ResourceAccess const &){held=CreateFileW(std::filesystem::path(linked).c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);};
 auto unavailable=load_editable_document(path,grants,options);
 if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
#endif
 ASSERT_TRUE(unavailable.error);EXPECT_EQ(unavailable.error->code,"resource-unavailable");
 EXPECT_TRUE(unavailable.error->retryable);EXPECT_FALSE(unavailable.document);
}
#ifdef _WIN32
TEST_F(Files, WindowsReadNavigationRetainsGrantAndTypeChecks) {
 auto source=sheet();std::filesystem::create_directory(dir+"/child");
 Grants g;g.read_files={source};
 for(auto const &name:{dir+"/./sheet.svg",dir+"/child/../sheet.svg"}) {
  auto access=inspect_command_path(name,g);ASSERT_EQ(access.state,"granted")<<name;
  EXPECT_EQ(read_admitted(access,1<<20).bytes,read(source));
 }
 g={};g.read_roots={dir+"/child"};
 EXPECT_EQ(inspect_command_path(dir+"/child/../sheet.svg",g).state,"ungranted");
 g={};g.read_files={dir};
 EXPECT_EQ(inspect_command_path(dir,g).state,"invalid");
 EXPECT_EQ(inspect_command_path(source+".",grants).state,"unsafe");
 EXPECT_EQ(inspect_write_destination(dir+"/child/../new.svg",grants).state,"unsafe");
}
TEST_F(Files, R3WindowsForwardSeparators) {
 auto source=sheet();auto slash=std::filesystem::path(source).generic_string();
 Grants g;g.read_files={slash};g.write_roots={std::filesystem::path(dir).generic_string()};
 auto access=inspect_command_path(slash,g);ASSERT_EQ(access.state,"granted");
 EXPECT_EQ(read_admitted(access,1<<20).bytes,read(source));
 EXPECT_EQ(inspect_write_destination(g.write_roots[0]+"/new.svg",g).state,"granted");
}
TEST_F(Files, R3WindowsMissingLeavesRespectDirectoryCasePolicy) {
 auto set_case=[](std::filesystem::path const &p,bool enabled) {
  HANDLE h=CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES,
   FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
  if(h==INVALID_HANDLE_VALUE)return false;
  struct {ULONG Flags;} info{enabled?1u:0u};
  bool ok=SetFileInformationByHandle(h,static_cast<FILE_INFO_BY_HANDLE_CLASS>(23),&info,sizeof(info));CloseHandle(h);return ok;
 };
 auto parent=std::filesystem::path(dir)/"case";std::filesystem::create_directory(parent);
 if(!set_case(parent,true))GTEST_SKIP()<<"Per-directory case sensitivity unavailable (NTFS/privilege required)";
 auto safe=parent/"safe",other=parent/"SAFE";
 std::filesystem::create_directory(safe);std::filesystem::create_directory(other);
 ASSERT_TRUE(set_case(safe,true));
 Grants g;g.write_roots={safe.string()};g.read_roots=g.write_roots;
 EXPECT_EQ(inspect_write_destination((safe/"new.svg").string(),g).state,"granted");
 EXPECT_EQ(inspect_write_destination((other/"new.svg").string(),g).state,"ungranted");
 EXPECT_EQ(inspect_command_path((other/"new.svg").string(),g).state,"ungranted");
 g={};g.write_files={(safe/"new.svg").string()};g.read_files=g.write_files;
 EXPECT_EQ(inspect_write_destination((safe/"new.svg").string(),g).state,"granted");
 EXPECT_EQ(inspect_write_destination((safe/"NEW.svg").string(),g).state,"ungranted");
 EXPECT_EQ(inspect_command_path((safe/"NEW.svg").string(),g).state,"ungranted");
 EXPECT_EQ(inspect_command_path((safe/"new.svg").string(),g).state,"missing");
 ASSERT_TRUE(set_case(safe,false));
 EXPECT_EQ(inspect_write_destination((safe/"NEW.svg").string(),g).state,"granted");
}
#endif
TEST_F(Files, LifecycleDryRunAndDirtyRefusal) {
 auto dry=run("file.new",dims(),true);EXPECT_EQ(dry.status,Status::Ok);EXPECT_FALSE(state.document);
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);auto before=xml();auto id=document_stamp(context.document).id;
 EXPECT_TRUE(context.document->isModifiedSinceSave());
 EXPECT_EQ(run("file.new",dims()).reason,"dirty-document");EXPECT_EQ(xml(),before);
 EXPECT_EQ(run("file.close").reason,"dirty-document");
 auto p=load(dir+"/missing.svg");p["discard"]=true;EXPECT_EQ(run("file.open",p).status,Status::Rejected);EXPECT_EQ(document_stamp(context.document).id,id);
 EXPECT_EQ(run("file.close",{{"discard",true}},true).status,Status::Ok);EXPECT_EQ(xml(),before);
 EXPECT_EQ(run("file.close",{{"discard",true}}).status,Status::Changed);EXPECT_FALSE(context.document);
 EXPECT_EQ(run("file.close").status,Status::Unchanged);
}
TEST_F(Files, GrantsAreIndependentAndExact) {
 auto source=sheet(); Grants g;g.read_files={source};
 EXPECT_EQ(inspect_resource(source,{},g).state,"granted");
 EXPECT_EQ(inspect_write_destination(source,g).state,"ungranted");
 g={};g.write_files={source};EXPECT_EQ(inspect_resource(source,{},g).state,"ungranted");
 EXPECT_EQ(inspect_write_destination(source,g).state,"granted");
 EXPECT_EQ(inspect_write_destination(dir+"/../escape.svg",grants).state,"unsafe");
 EXPECT_NE(inspect_write_destination(dir+"-sibling/output.svg",grants).state,"granted");
}
TEST_F(Files, SymlinkAliasPolicyWhenCreatable) {
 auto source=sheet();
#ifdef _WIN32
 // MinGW std::filesystem::create_symlink is unimplemented; use Win32 directly.
 if(!CreateSymbolicLinkW(std::filesystem::u8path(dir+"/alias.svg").c_str(),
     std::filesystem::u8path(source).c_str(),0)) {
  auto error=GetLastError();
  if(error==ERROR_PRIVILEGE_NOT_HELD)
   GTEST_SKIP()<<"CreateSymbolicLinkW unavailable: Windows error "<<static_cast<unsigned long>(error);
  FAIL()<<"CreateSymbolicLinkW failed: Windows error "<<static_cast<unsigned long>(error);
 }
 EXPECT_NE(inspect_write_destination(dir+"/alias.svg",grants).state,"granted");
#else
 std::filesystem::create_symlink(source,dir+"/alias.svg");
 EXPECT_EQ(inspect_write_destination(dir+"/alias.svg",grants).state,"granted");
#endif
}
TEST_F(Files, EditableRefusesActiveAndUngrantedLinkedContent) {
 auto active=write("active.svg","<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>");
 EXPECT_EQ(run("file.open",load(active)).reason,"resource-denied");EXPECT_FALSE(state.document);
 auto linked=write("linked.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\"><image width=\"10\" height=\"10\" xlink:href=\"https://example.invalid/a.png\"/></svg>");
 EXPECT_EQ(run("file.open",load(linked)).status,Status::Rejected);EXPECT_FALSE(state.document);
 auto inspect=load_inspection_document(active,{});EXPECT_TRUE(inspect.document); // M1 stays nonexecuting inspection.
}
TEST_F(Files, ImportOneUndoAndDryRunPreserveExactSource) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);auto before=xml();
 auto p=load(sheet());p["position"]=object{{"x",length(1)},{"y",length(2)}};
 auto dry=run("file.import",p,true);ASSERT_EQ(dry.status,Status::Ok)<<dry.message;EXPECT_EQ(xml(),before);
 auto imported=run("file.import",p);ASSERT_EQ(imported.status,Status::Changed)<<imported.message;
 EXPECT_TRUE(imported.one_undo_step);auto after=xml();EXPECT_NE(after,before);
 ASSERT_TRUE(DocumentUndo::undo(context.document));EXPECT_EQ(xml(),before);
 ASSERT_TRUE(DocumentUndo::redo(context.document));EXPECT_EQ(xml(),after);
}
TEST_F(Files, SaveSnapshotConflictAndGuardedOverwrite) {
 ASSERT_EQ(run("file.open",load(sheet())).status,Status::Changed);
 auto before=xml();auto path=dir+"/saved.svg"; object p{{"path",path},{"embedding-policy","embed"}};
 EXPECT_EQ(run("file.save",p,true).status,Status::Ok);EXPECT_FALSE(std::filesystem::exists(path));EXPECT_EQ(xml(),before);
 auto saved=run("file.save",p);ASSERT_EQ(saved.status,Status::Changed)<<saved.message;EXPECT_TRUE(saved.publication_persisted);
 EXPECT_FALSE(context.document->isModifiedSinceSave());auto bytes=read(path);
 EXPECT_EQ(run("file.save",p).reason,"publication-conflict");EXPECT_EQ(read(path),bytes);
 p["overwrite"]=true;EXPECT_EQ(run("file.save",p).reason,"expected-version-required");
 p["expected-version"]=saved.data.at("destination_version");
 ASSERT_EQ(run("file.save",p).status,Status::Changed);
 auto reopened=load_editable_document(path,grants,{"svg","embed","reject",{}});ASSERT_TRUE(reopened.document);
 EXPECT_NE(reopened.document->getObjectById("red"),nullptr);
}
TEST_F(Files, ExportNativeFormatsPreserveLiveXmlAndDirty) {
 ASSERT_EQ(run("file.open",load(sheet())).status,Status::Changed);context.document->setModifiedSinceSave(true);auto before=xml();
 for(auto format:{"svg","png","pdf","tiff"}) {
  auto path=dir+"/out."+format;object p{{"path",path},{"format",format},{"page",1}};
  if(std::string(format)=="png" || std::string(format)=="tiff") p["profile"]=object{{"id","srgb"}};
  auto dry=run("file.export",p,true);ASSERT_EQ(dry.status,Status::Ok)<<format<<dry.message;EXPECT_FALSE(std::filesystem::exists(path));
  auto result=run("file.export",p);ASSERT_EQ(result.status,Status::Changed)<<format<<result.message;
  EXPECT_TRUE(result.publication_persisted);EXPECT_GT(std::filesystem::file_size(path),0u);
  EXPECT_EQ(xml(),before);EXPECT_TRUE(context.document->isModifiedSinceSave());
 }
}
TEST_F(Files, CliGap34TiffRipOptionsAndRasterMemoryGuard) {
 auto source=write("rip.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"1\"><rect x=\"0\" y=\"0\" width=\"1\" height=\"1\" fill=\"white\"/><rect x=\"1\" y=\"0\" width=\"1\" height=\"1\" fill=\"white\" opacity=\"0\"/><rect x=\"2\" y=\"0\" width=\"1\" height=\"1\" fill=\"black\" opacity=\"0.4\"/><rect x=\"3\" y=\"0\" width=\"1\" height=\"1\" fill=\"black\"/></svg>");
 ASSERT_EQ(run("file.open",load(source)).status,Status::Changed);
 auto export_tiff=[&](std::string name,object extra={}) {
  object p{{"path",dir+"/"+name},{"format","tiff"},{"page",1},{"profile",object{{"id","srgb"}}}};
  for(auto const &[key,value]:extra)p[key]=value;
  return run("file.export",p);
 };
 auto pixels=[&](std::string const &name) {
  auto *tif=TIFFOpen((dir+"/"+name).c_str(),"r");EXPECT_NE(tif,nullptr);
  std::vector<unsigned char> row(16);if(tif){EXPECT_GE(TIFFReadScanline(tif,row.data(),0,0),0);TIFFClose(tif);}return row;
 };
 auto off=export_tiff("rip-off.tiff");ASSERT_EQ(off.status,Status::Changed)<<off.message;
 auto off_row=pixels("rip-off.tiff");EXPECT_EQ(off_row[0],255);EXPECT_EQ(off_row[1],255);EXPECT_EQ(off_row[2],255);
 auto on=export_tiff("rip-on.tiff",{{"prevent-white-clipping",true}});ASSERT_EQ(on.status,Status::Changed)<<on.message;
 auto on_row=pixels("rip-on.tiff");EXPECT_EQ(on_row[0],254);EXPECT_EQ(on_row[1],254);EXPECT_EQ(on_row[2],254);
 EXPECT_EQ(on_row[3],off_row[3]);
 // The fully transparent white pixel is untouched until the dependent option is enabled.
 EXPECT_EQ(on_row[4],off_row[4]);EXPECT_EQ(on_row[5],off_row[5]);EXPECT_EQ(on_row[6],off_row[6]);EXPECT_EQ(on_row[7],0);
 auto both=export_tiff("rip-transparent.tiff",{{"prevent-white-clipping",true},{"white-clipping-transparent",true}});
 ASSERT_EQ(both.status,Status::Changed)<<both.message;
 auto both_row=pixels("rip-transparent.tiff");EXPECT_EQ(both_row[4],on_row[4]);EXPECT_EQ(both_row[5],on_row[5]);EXPECT_EQ(both_row[6],on_row[6]);EXPECT_EQ(both_row[7],0);
 EXPECT_TRUE(both.data.at("rip_options").as_object().at("white-clipping-transparent").as_bool());
 // Cleaning writes transparent pixels white; the dependent flag then changes only RGB.
 auto clean_white=export_tiff("clean-white.tiff",{{"clean-edges",true},{"prevent-white-clipping",true}});
 ASSERT_EQ(clean_white.status,Status::Changed)<<clean_white.message;
 auto clean_white_row=pixels("clean-white.tiff");
 EXPECT_EQ(clean_white_row[4],255);EXPECT_EQ(clean_white_row[5],255);EXPECT_EQ(clean_white_row[6],255);EXPECT_EQ(clean_white_row[7],0);
 auto clean_clipped=export_tiff("clean-clipped.tiff",{{"clean-edges",true},{"prevent-white-clipping",true},{"white-clipping-transparent",true}});
 ASSERT_EQ(clean_clipped.status,Status::Changed)<<clean_clipped.message;
 auto clean_clipped_row=pixels("clean-clipped.tiff");
 EXPECT_EQ(clean_clipped_row[4],254);EXPECT_EQ(clean_clipped_row[5],254);EXPECT_EQ(clean_clipped_row[6],254);EXPECT_EQ(clean_clipped_row[7],0);
 for(auto const &[key,value]:std::vector<std::pair<std::string,bool>>{{"prevent-white-clipping",true},{"white-clipping-transparent",true},{"clean-edges",true},{"hard-edges",true}}) {
  auto rejected=run("file.export",{{"path",dir+"/bad.png"},{"format","png"},{"page",1},{key,value}});
  EXPECT_EQ(rejected.reason,"invalid-argument")<<key;EXPECT_EQ(rejected.message,"RIP options apply to TIFF export only.");
  EXPECT_FALSE(std::filesystem::exists(dir+"/bad.png"));
 }
 auto dependent=run("file.export",{{"path",dir+"/dependent.tiff"},{"format","tiff"},{"page",1},{"white-clipping-transparent",true}});
 EXPECT_EQ(dependent.reason,"invalid-argument");EXPECT_FALSE(std::filesystem::exists(dir+"/dependent.tiff"));
 auto hard=export_tiff("hard.tiff",{{"hard-edges",true}});ASSERT_EQ(hard.status,Status::Changed)<<hard.message;
 auto hard_row=pixels("hard.tiff");for(std::size_t i=3;i<hard_row.size();i+=4)EXPECT_TRUE(hard_row[i]==0||hard_row[i]==255)<<i;
 EXPECT_TRUE(hard.data.at("rip_options").as_object().at("clean-edges").as_bool());

 auto large=write("large.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"864\" height=\"1824\"><rect width=\"864\" height=\"1824\" fill=\"white\"/></svg>");
 ASSERT_EQ(run("file.open",load(large)).status,Status::Changed);
 auto large_export=[&](std::string const &name) {return run("file.export",{{"path",dir+"/"+name},{"format","png"},{"page",1},{"dpi",600}});};
 auto sixty_one=large_export("sixty-one.png");ASSERT_EQ(sixty_one.status,Status::Changed)<<sixty_one.reason<<": "<<sixty_one.message;
 EXPECT_EQ(sixty_one.data.at("output_width"),5400);EXPECT_EQ(sixty_one.data.at("output_height"),11400);
 EXPECT_EQ(sixty_one.data.at("output_width").to_number<std::uint64_t>()*sixty_one.data.at("output_height").to_number<std::uint64_t>(),61560000u);
 EXPECT_TRUE(std::filesystem::exists(dir+"/sixty-one.png"));
 {
  ScopedIntakeMemoryForTesting no_memory(0);
  auto refused=large_export("no-memory.png");EXPECT_EQ(refused.reason,"engine-limit");
  EXPECT_EQ(refused.error_details.at("phase"),"raster-export");EXPECT_EQ(refused.error_details.at("pixels"),61560000u);
  EXPECT_TRUE(refused.error_details.contains("bytes_needed"));EXPECT_TRUE(refused.error_details.contains("available_bytes"));
  EXPECT_TRUE(refused.error_details.contains("bytes_per_pixel"));EXPECT_FALSE(std::filesystem::exists(dir+"/no-memory.png"));
 }

 auto boundary=write("boundary.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1152\" height=\"2304\"><rect width=\"1152\" height=\"2304\" fill=\"white\"/></svg>");
 ASSERT_EQ(run("file.open",load(boundary)).status,Status::Changed);
 auto max_export=[&](std::string const &name,double height) {return run("file.export",{{"path",dir+"/"+name},{"format","png"},{"dpi",600},{"area",object{{"x",object{{"value",0},{"unit","px"}}},{"y",object{{"value",0},{"unit","px"}}},{"width",object{{"value",1152},{"unit","px"}}},{"height",object{{"value",height},{"unit","px"}}}}}});};
 auto maximum=max_export("maximum.png",2304);
 if(maximum.status==Status::Changed) {
  EXPECT_EQ(maximum.data.at("output_width"),7200);EXPECT_EQ(maximum.data.at("output_height"),14400);
  EXPECT_TRUE(std::filesystem::exists(dir+"/maximum.png"));
 } else {
  EXPECT_EQ(maximum.reason,"engine-limit")<<maximum.message;
  EXPECT_EQ(maximum.error_details.at("phase"),"raster-export");
  EXPECT_FALSE(std::filesystem::exists(dir+"/maximum.png"));
 }
 auto too_tall=max_export("too-tall.png",2304.16); // rounds to one output row beyond the owner maximum.
 EXPECT_EQ(too_tall.reason,"out-of-range");EXPECT_EQ(too_tall.message,"Raster export is limited to 103.68 MP (12 x 24 in at 600 dpi).");
 EXPECT_FALSE(std::filesystem::exists(dir+"/too-tall.png"));
}
TEST_F(Files, CliGap34Raster61MPPNG) {
 auto source=write("raster-61.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"864\" height=\"1824\"><rect width=\"864\" height=\"1824\" fill=\"white\"/></svg>");
 ASSERT_EQ(run("file.open",load(source)).status,Status::Changed);
 auto result=run("file.export",{{"path",dir+"/raster-61.png"},{"format","png"},{"page",1},{"dpi",600}});
 ASSERT_EQ(result.status,Status::Changed)<<result.reason<<": "<<result.message;
 EXPECT_EQ(result.data.at("output_width"),5400);EXPECT_EQ(result.data.at("output_height"),11400);
 auto png=read(dir+"/raster-61.png");ASSERT_GE(png.size(),24u);
 EXPECT_EQ(png.substr(0,8),std::string("\x89PNG\r\n\x1a\n",8));
 auto dimension=[&](std::size_t offset) {std::uint32_t value=0;for(unsigned i=0;i<4;++i)value=(value<<8)|static_cast<unsigned char>(png[offset+i]);return value;};
 EXPECT_EQ(dimension(16),5400u);EXPECT_EQ(dimension(20),11400u);
}
TEST_F(Files, CliGap34Raster61MPTIFF) {
 auto source=write("raster-61.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"864\" height=\"1824\"><rect width=\"864\" height=\"1824\" fill=\"white\"/></svg>");
 ASSERT_EQ(run("file.open",load(source)).status,Status::Changed);
 auto result=run("file.export",{{"path",dir+"/raster-61.tiff"},{"format","tiff"},{"page",1},{"dpi",600},{"profile",object{{"id","srgb"}}}});
 ASSERT_EQ(result.status,Status::Changed)<<result.reason<<": "<<result.message;
 EXPECT_EQ(result.data.at("output_width"),5400);EXPECT_EQ(result.data.at("output_height"),11400);
 auto *tif=TIFFOpen((dir+"/raster-61.tiff").c_str(),"r");ASSERT_NE(tif,nullptr);
 std::uint32_t width=0,height=0;EXPECT_TRUE(TIFFGetField(tif,TIFFTAG_IMAGEWIDTH,&width));EXPECT_TRUE(TIFFGetField(tif,TIFFTAG_IMAGELENGTH,&height));
 EXPECT_EQ(width,5400u);EXPECT_EQ(height,11400u);TIFFClose(tif);
}
TEST_F(Files, CliGap34RasterMaximumTIFF) {
 auto source=write("raster-max.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1152\" height=\"2304\"><rect width=\"1152\" height=\"2304\" fill=\"white\"/></svg>");
 ASSERT_EQ(run("file.open",load(source)).status,Status::Changed);
 auto result=run("file.export",{{"path",dir+"/raster-max.tiff"},{"format","tiff"},{"page",1},{"dpi",600},{"profile",object{{"id","srgb"}}}});
 if(result.status==Status::Changed) {
  auto *tif=TIFFOpen((dir+"/raster-max.tiff").c_str(),"r");ASSERT_NE(tif,nullptr);
  std::uint32_t width=0,height=0;EXPECT_TRUE(TIFFGetField(tif,TIFFTAG_IMAGEWIDTH,&width));EXPECT_TRUE(TIFFGetField(tif,TIFFTAG_IMAGELENGTH,&height));
  EXPECT_EQ(width,7200u);EXPECT_EQ(height,14400u);TIFFClose(tif);
  RecordProperty("maximum_export","7200x14400 TIFF exported");
 } else {
  EXPECT_EQ(result.reason,"engine-limit")<<result.message;
  EXPECT_EQ(result.error_details.at("phase"),"raster-export");
  EXPECT_FALSE(std::filesystem::exists(dir+"/raster-max.tiff"));
  RecordProperty("maximum_export",serialize(result.error_details));
 }
 // Landscape uses the same pixel budget and reaches memory admission.
 {
  ScopedIntakeMemoryForTesting no_memory(0);
  auto rotated=run("file.export",{{"path",dir+"/rotated.png"},{"format","png"},{"dpi",600},{"area",object{{"x",object{{"value",0},{"unit","px"}}},{"y",object{{"value",0},{"unit","px"}}},{"width",object{{"value",2304},{"unit","px"}}},{"height",object{{"value",1152},{"unit","px"}}}}}});
  EXPECT_EQ(rotated.reason,"engine-limit");EXPECT_EQ(rotated.error_details.at("phase"),"raster-export");
  EXPECT_EQ(rotated.error_details.at("pixels"),103680000u);EXPECT_FALSE(std::filesystem::exists(dir+"/rotated.png"));
 }
}
TEST_F(Files, ExpectedVersionRejectsContentAndIdentityRaces) {
 auto path=write("prior.svg","AAAA");auto expected=IO::inspect_existing_file_version(path);ASSERT_TRUE(expected.version);
 write("prior.svg","BBBB");bool called=false;
 auto failed=IO::replace_existing_local_file(path,[&](FILE*){called=true;},*expected.version);
 EXPECT_EQ(failed.outcome,IO::ExistingFileOutcome::Conflict);EXPECT_FALSE(called);EXPECT_EQ(read(path),"BBBB");
 auto current=IO::inspect_existing_file_version(path);ASSERT_TRUE(current.version);
 auto raced=IO::replace_existing_local_file(path,[&](FILE *f){std::fwrite("NEW",1,3,f);write("prior.svg","CCCC");},*current.version);
 EXPECT_EQ(raced.outcome,IO::ExistingFileOutcome::Conflict);EXPECT_EQ(read(path),"CCCC");
 auto old=IO::inspect_existing_file_version(path);ASSERT_TRUE(old.version);
 std::filesystem::rename(path,dir+"/retained.svg");write("prior.svg","CCCC");
 auto replaced=IO::inspect_existing_file_version(path);ASSERT_TRUE(replaced.version);
 EXPECT_NE(old.version->identity,replaced.version->identity);EXPECT_EQ(old.version->sha256,replaced.version->sha256);
}
TEST_F(Files, UncertaintyLatchSurvivesNewAndRequiresVerifiedReopen) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);
 auto dest=write("uncertain.svg","observed");state.writes_blocked=true;
 state.reconciliation={{"destination",dest},{"acknowledged",false}};
 EXPECT_EQ(run("file.save",{{"path",dir+"/out.svg"},{"embedding-policy","embed"}}).reason,"writes-blocked");
 EXPECT_EQ(run("file.close",{{"discard",true}}).reason,"reconciliation-required");
 auto v=IO::inspect_existing_file_version(dest);ASSERT_TRUE(v.version);
 object observed{{"identity",v.version->identity},{"sha256",v.version->sha256},{"bytes",v.version->bytes}};
 object p{{"discard",true},{"reconcile",object{{"destination",dest},{"observed-version",observed},{"acknowledge",true}}}};
 ASSERT_EQ(run("file.close",p).status,Status::Changed);EXPECT_TRUE(state.writes_blocked);
 ASSERT_EQ(run("file.open",load(sheet())).status,Status::Changed);EXPECT_FALSE(state.writes_blocked);
}
TEST_F(Files, NativeRequiredIntakeFormatsAndCustomProfiles) {
 std::string fixtures=std::string(INKSCAPE_TESTS_DIR)+"/cli_tests/vacards-agent/fixtures/m2";
 for (auto name:{"sheet.svg","sheet.svgz","sheet.cdr","rgba.png","photo.jpg","rgba.tiff"}) {
  auto path=dir+"/"+name;
  ASSERT_TRUE(std::filesystem::exists(fixtures+"/"+name))<<name;
  std::filesystem::copy_file(fixtures+"/"+name,path);
  auto p=load(path); p["discard"]=true;p["font-policy"]="substitute";
  auto loaded=run("file.open",p);ASSERT_EQ(loaded.status,Status::Changed)<<name<<": "<<loaded.reason<<" "<<loaded.message;
  EXPECT_TRUE(loaded.data.contains("source_version"));
 }
 auto profile=dir+"/custom-rgb.icc";
 std::filesystem::copy_file(fixtures+"/custom-rgb.icc",profile);
 auto v=IO::inspect_existing_file_version(profile);ASSERT_TRUE(v.version);
 for (auto format:{"png","tiff"}) {
  object p{{"path",dir+"/profile."+format},{"format",format},{"page",1},
    {"profile",object{{"id","file"},{"path",profile},{"sha256",v.version->sha256}}}};
  auto exported=run("file.export",p);ASSERT_EQ(exported.status,Status::Changed)<<exported.message;
  EXPECT_EQ(exported.data.at("profile").as_object().at("sha256").as_string(),v.version->sha256);
 }
}

TEST_F(Files, LiteralHashCommandPathDoesNotBorrowPrefixGrant) {
 auto ordinary=sheet(); auto literal=write("sheet.svg#private.svg",read(ordinary));
 grants.read_roots.clear();grants.read_files={ordinary};
 EXPECT_EQ(run("file.open",load(literal)).reason,"read-grant-denied");
 grants.read_files={literal};
 auto r=run("file.open",load(literal)); ASSERT_EQ(r.status,Status::Changed)<<r.message;
 for(auto const &spec:file_commands()) if(spec.id=="file.open") EXPECT_FALSE(validate_schema(r.data,spec.result_data))<<serialize(r.data);
 auto expected=IO::inspect_existing_file_version(literal);ASSERT_TRUE(expected.version);
 EXPECT_EQ(r.data.at("source_version").as_object().at("sha256").as_string(),expected.version->sha256);
}
#ifndef _WIN32
TEST_F(Files, AdmittedLeafAndParentSwapsReadNoBytes) {
 auto source=sheet();auto secret=write("secret.svg","not admitted");
 Grants g;g.read_files={source};auto admitted=inspect_command_path(source,g);
 ASSERT_EQ(admitted.state,"granted");
 std::filesystem::rename(source,source+".old");std::filesystem::create_symlink(secret,source);
 auto read=read_admitted(admitted,4096);EXPECT_FALSE(read.error.empty());EXPECT_TRUE(read.bytes.empty());
 std::filesystem::create_directory(dir+"/parent");auto nested=write("parent/input.svg","safe");
 g.read_files={nested};admitted=inspect_command_path(nested,g);ASSERT_EQ(admitted.state,"granted");
 std::filesystem::rename(dir+"/parent",dir+"/old-parent");std::filesystem::create_directory(dir+"/parent");
 write("parent/input.svg","not admitted");read=read_admitted(admitted,4096);
 EXPECT_EQ(read.error,"stale-dependency");EXPECT_TRUE(read.bytes.empty());
}
TEST_F(Files, ExactGrantDoesNotAdmitOtherHardlinkAndReplacementFails)
{
 auto source=sheet();auto other=dir+"/other.svg";std::filesystem::create_hard_link(source,other);
 Grants g;g.read_files={source};EXPECT_EQ(inspect_command_path(other,g).state,"ungranted");
 auto admitted=inspect_command_path(source,g);ASSERT_EQ(admitted.state,"granted");
 std::filesystem::rename(source,source+".old");write("sheet.svg",read(other));
 auto input=read_admitted(admitted,4096);EXPECT_EQ(input.error,"stale-dependency");EXPECT_TRUE(input.bytes.empty());
}
TEST_F(Files, UserSymlinkRequiresResolvedTargetGrant) {
 auto source=sheet();std::filesystem::create_symlink(source,dir+"/alias.svg");
 Grants g;g.read_roots={dir+"/elsewhere"};EXPECT_EQ(inspect_command_path(dir+"/alias.svg",g).state,"ungranted");
 g.read_files={source};auto access=inspect_command_path(dir+"/alias.svg",g);ASSERT_EQ(access.state,"granted");
 EXPECT_EQ(read_admitted(access,4096).bytes,read(source));
}
TEST_F(Files, UnavailableVersionIsNotConflict) {
 auto source=sheet(); ASSERT_EQ(::chmod(source.c_str(),0000),0);
 auto observed=IO::inspect_existing_file_version(source);
 ASSERT_EQ(::chmod(source.c_str(),0600),0);
 EXPECT_EQ(observed.outcome,IO::ExistingFileOutcome::Unavailable);EXPECT_FALSE(observed.version);
}
#endif
#ifdef __APPLE__
TEST_F(Files, MacGrantedSymlinkPublishesResolvedDestination) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);auto dest=write("target.svg","old");
 std::filesystem::create_symlink(dest,dir+"/alias.svg");
 grants.write_roots.clear();grants.write_files={dest};
 auto v=IO::inspect_existing_file_version(dest);ASSERT_TRUE(v.version);
 auto r=run("file.save",{{"path",dir+"/alias.svg"},{"overwrite",true},{"embedding-policy","embed"},
   {"expected-version",object{{"identity",v.version->identity},{"sha256",v.version->sha256},{"bytes",v.version->bytes}}}});
 EXPECT_EQ(r.status,Status::Changed)<<r.message;EXPECT_TRUE(std::filesystem::is_symlink(dir+"/alias.svg"));
 EXPECT_NE(read(dest),"old");
}
TEST_F(Files, MacSystemCaseAndNormalizationAliases) {
 auto source=write("Art-é.svg","safe"); Grants g;g.read_files={source};g.write_files={source};
 auto alias=dir+"/art-é.svg";
 if(std::filesystem::exists(alias)) {
  auto admitted=inspect_command_path(alias,g);ASSERT_EQ(admitted.state,"granted");
  EXPECT_EQ(read_admitted(admitted,100).bytes,"safe");EXPECT_EQ(inspect_write_destination(alias,g).state,"granted");
 }
 auto physical=std::filesystem::canonical(dir).string();
 if(physical.starts_with("/private/var/")) {
  auto var=physical.substr(8)+"/Art-é.svg";
  EXPECT_EQ(inspect_command_path(var,g).state,"granted");EXPECT_EQ(inspect_write_destination(var,g).state,"granted");
 }
 auto name=std::string("/tmp/va-r1-")+std::filesystem::path(dir).filename().string();
 std::filesystem::create_directory(name);
 struct Cleanup{std::string p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{name};
 auto tmp=name+"/file.svg";{std::ofstream out(tmp);out<<"tmp";}
 g.read_files={std::filesystem::canonical(tmp).string()};g.write_files=g.read_files;
 EXPECT_EQ(inspect_command_path(tmp,g).state,"granted");EXPECT_EQ(inspect_write_destination(tmp,g).state,"granted");
}
#endif
TEST_F(Files, FontListsRejectOrReportFirstFamilyFallback) {
 auto path=write("fonts.svg","<svg xmlns=\"http://www.w3.org/2000/svg\"><text style=\"font-family:'Definitely Missing R1', serif\">abc</text></svg>");
 auto p=load(path);EXPECT_EQ(run("file.open",p).reason,"font-policy-required");
 p["font-policy"]="substitute";auto r=run("file.open",p);ASSERT_EQ(r.status,Status::Changed)<<r.message;
 EXPECT_FALSE(r.data.at("intake").as_object().at("fonts").as_array().empty());
 for(auto const &spec:file_commands()) if(spec.id=="file.open") EXPECT_FALSE(validate_schema(r.data,spec.result_data))<<serialize(r.data);
 auto generic=write("generic.svg","<svg xmlns=\"http://www.w3.org/2000/svg\"><text style=\"font-family:serif, 'Definitely Missing R1'\">abc</text></svg>");
 p=load(generic);p["discard"]=true;r=run("file.open",p);EXPECT_EQ(r.status,Status::Changed)<<r.message;
}
TEST_F(Files, StaleCustomICCIsStructuredDependencyError) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);
 auto profile=IO::ExportColorProfiles::srgb();auto path=write("custom#profile.icc",{profile.bytes.begin(),profile.bytes.end()});
 object p{{"path",dir+"/out.png"},{"format","png"},{"page",1},
   {"profile",object{{"id","file"},{"path",path},{"sha256",std::string(64,'0')}}}};
 auto r=run("file.export",p,true);EXPECT_EQ(r.reason,"stale-dependency");ASSERT_TRUE(r.error);EXPECT_EQ(r.error->code,"stale-dependency");
 EXPECT_FALSE(std::filesystem::exists(dir+"/out.png"));
 p["profile"].as_object()["path"]=dir+"/missing.icc";
 EXPECT_EQ(run("file.export",p,true).reason,"resource-unavailable");
 p["profile"].as_object()["path"]=path;grants.read_roots.clear();
 EXPECT_EQ(run("file.export",p,true).reason,"read-grant-denied");
 grants.read_files={path};write("custom#profile.icc","bad");auto bad=IO::inspect_existing_file_version(path);ASSERT_TRUE(bad.version);
 p["profile"].as_object()["sha256"]=bad.version->sha256;
 EXPECT_EQ(run("file.export",p,true).reason,"profile-invalid");
}
TEST_F(Files, CancellationBeforePublicationPreservesDestination) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);auto target=write("out.svg","old");
 auto v=IO::inspect_existing_file_version(target);ASSERT_TRUE(v.version);
 unsigned checks=0;context.cancelled=[&] {return ++checks>=3;};
 auto r=run("file.save",{{"path",target},{"overwrite",true},{"embedding-policy","embed"},
   {"expected-version",object{{"identity",v.version->identity},{"sha256",v.version->sha256},{"bytes",v.version->bytes}}}});
 EXPECT_EQ(r.status,Status::Cancelled);EXPECT_EQ(r.reason,"cancelled");EXPECT_EQ(read(target),"old");EXPECT_GE(checks,3u);
}
#ifdef _WIN32
TEST_F(Files, WindowsCloudTagPolicyDeniesNameSurrogates) {
 for(unsigned i=0;i<16;++i) EXPECT_TRUE(allowed_windows_reparse_tag(0x9000001au|(i<<12)));
 EXPECT_FALSE(allowed_windows_reparse_tag(IO_REPARSE_TAG_SYMLINK));
 EXPECT_FALSE(allowed_windows_reparse_tag(IO_REPARSE_TAG_MOUNT_POINT));
 EXPECT_FALSE(allowed_windows_reparse_tag(0x80000042u));
}
TEST_F(Files, WindowsFinalPathCaseAndLongPaths) {
 auto source=sheet();Grants g;g.read_files={source};g.write_files={source};
 auto upper=source;for(auto &c:upper) c=g_ascii_toupper(c);
 EXPECT_EQ(inspect_command_path(upper,g).state,"granted");EXPECT_EQ(inspect_write_destination(upper,g).state,"granted");
 std::filesystem::path longdir=std::filesystem::u8path(dir);
 while(longdir.wstring().size()<280) longdir/=L"long-component-0123456789";
 auto extended=std::filesystem::path(L"\\\\?\\"+longdir.wstring());std::filesystem::create_directories(extended);
 auto file=extended/L"input.svg";{std::ofstream out(file);out<<"safe";}
 auto u=(longdir/L"input.svg").u8string();std::string logical(reinterpret_cast<char const *>(u.data()),u.size());
 g.read_files={logical};g.write_files={logical};auto access=inspect_command_path(logical,g);
 ASSERT_EQ(access.state,"granted");EXPECT_EQ(read_admitted(access,100).bytes,"safe");
 EXPECT_EQ(inspect_write_destination(logical,g).state,"granted");
 access.admitted.reset();
 auto version=IO::inspect_existing_file_version(logical);ASSERT_TRUE(version.version);
 auto result=IO::replace_existing_local_file(logical,[](FILE *f){std::fwrite("changed",1,7,f);},*version.version);
 EXPECT_EQ(result.outcome,IO::ExistingFileOutcome::Published)<<result.error;

}
TEST_F(Files, WindowsJunctionAncestorIsRefused) {
 auto source=sheet();std::filesystem::create_directory(dir+"/junction");
 auto link=std::filesystem::u8path(dir+"/junction").wstring();
 HANDLE handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
    FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
 ASSERT_NE(handle,INVALID_HANDLE_VALUE);
 auto target=std::wstring(L"\\??\\")+std::filesystem::u8path(dir).wstring();
 struct Junction { DWORD tag;WORD length,reserved,sub_offset,sub_length,print_offset,print_length;wchar_t names[2048]; } data{};
 data.tag=IO_REPARSE_TAG_MOUNT_POINT;data.sub_length=WORD(target.size()*sizeof(wchar_t));
 data.print_offset=data.sub_length+sizeof(wchar_t);data.print_length=0;
 std::copy(target.begin(),target.end(),data.names);data.length=WORD(8+data.print_offset+sizeof(wchar_t));DWORD returned=0;
 BOOL set=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,data.length+8,nullptr,0,&returned,nullptr);
 auto error=GetLastError();CloseHandle(handle);
 if(!set) GTEST_SKIP()<<"Cannot create NTFS junction fixture: "<<error;
 EXPECT_NE(inspect_command_path(dir+"/junction/sheet.svg",grants).state,"granted");
 EXPECT_NE(inspect_command_path(dir+"/junction/../sheet.svg",grants).state,"granted");
 EXPECT_NE(inspect_write_destination(dir+"/junction/new.svg",grants).state,"granted");
 EXPECT_FALSE(IO::inspect_existing_file_version(dir+"/junction/sheet.svg").version);
 ASSERT_TRUE(RemoveDirectoryW(link.c_str()));
}
TEST_F(Files, WindowsCloudProviderReadFixture) {
 auto fixture=g_getenv("VACARDS_R1_CLOUD_FILE");
 if(!fixture || !*fixture) GTEST_SKIP()<<"Set VACARDS_R1_CLOUD_FILE to a hydrated CLOUD-tag file under a CLOUD-tag ancestor";
 Grants g;g.read_files={fixture};g.write_files={fixture};
 auto access=inspect_command_path(fixture,g);ASSERT_EQ(access.state,"granted");
 auto bytes=read_admitted(access,64ull<<20);EXPECT_TRUE(bytes.error.empty())<<bytes.error;
 EXPECT_EQ(inspect_write_destination(fixture,g).state,"granted");
 EXPECT_TRUE(IO::inspect_existing_file_version(fixture).version);
}
TEST_F(Files, WindowsShortNamesResolveToGrantedLongEntry) {
 auto source=write("long descriptive input filename.svg","short name test");
 auto wide=std::filesystem::u8path(source).wstring();DWORD size=GetShortPathNameW(wide.c_str(),nullptr,0);
 ASSERT_GT(size,0u);std::wstring short_name(size,L'\0');
 auto count=GetShortPathNameW(wide.c_str(),short_name.data(),size);ASSERT_GT(count,0u);short_name.resize(count);
 if(short_name==wide) GTEST_SKIP()<<"Volume does not create 8.3 names";
 auto u=std::filesystem::path(short_name).u8string();std::string logical(reinterpret_cast<char const *>(u.data()),u.size());
 Grants g;g.read_files={source};g.write_files={source};
 auto access=inspect_command_path(logical,g);ASSERT_EQ(access.state,"granted");
 EXPECT_EQ(read_admitted(access,100).bytes,"short name test");EXPECT_EQ(inspect_write_destination(logical,g).state,"granted");
}
TEST_F(Files, WindowsCloudReplacementDisposableFixture) {
 auto fixture=g_getenv("VACARDS_R1_CLOUD_REPLACE_FILE");
 if(!fixture || !*fixture) GTEST_SKIP()<<"Set VACARDS_R1_CLOUD_REPLACE_FILE to a disposable hydrated CLOUD-tag file";
 auto path=std::filesystem::u8path(fixture).wstring();
 HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
    nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 ASSERT_NE(h,INVALID_HANDLE_VALUE);FILE_ATTRIBUTE_TAG_INFO tag{};
 BOOL queried=GetFileInformationByHandleEx(h,FileAttributeTagInfo,&tag,sizeof(tag));CloseHandle(h);
 ASSERT_TRUE(queried);ASSERT_TRUE(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
 ASSERT_TRUE(allowed_windows_reparse_tag(tag.ReparseTag));
 Grants g;g.read_files={fixture};auto access=inspect_command_path(fixture,g);ASSERT_EQ(access.state,"granted");
 auto original=read_admitted(access,64ull<<20);ASSERT_TRUE(original.error.empty());access.admitted.reset();
 auto before=IO::inspect_existing_file_version(fixture);ASSERT_TRUE(before.version);
 auto result=IO::replace_existing_local_file(fixture,[&](FILE *f){
    ASSERT_EQ(std::fwrite(original.bytes.data(),1,original.bytes.size(),f),original.bytes.size());
 },*before.version);
 EXPECT_EQ(result.outcome,IO::ExistingFileOutcome::Published)<<result.error;
 auto after=IO::inspect_existing_file_version(fixture);ASSERT_TRUE(after.version);EXPECT_EQ(after.version->sha256,original.sha256);
}
TEST_F(Files, WindowsSharingFailureIsUnavailable) {
 auto source=sheet();auto path=std::filesystem::u8path(source).wstring();
 HANDLE held=CreateFileW(path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);ASSERT_NE(held,INVALID_HANDLE_VALUE);
 auto version=IO::inspect_existing_file_version(source);CloseHandle(held);
 EXPECT_EQ(version.outcome,IO::ExistingFileOutcome::Unavailable);EXPECT_FALSE(version.version);
}
#endif
}

namespace {
std::string request_pdf(bool missing_font = false)
{
    auto stream = [](std::string const &s) {
        return "<< /Length " + std::to_string(s.size()) + " >>\nstream\n" + s + "endstream";
    };
    std::vector<std::string> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents 5 0 R >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources << /Font << /F1 7 0 R >> >> /Contents 6 0 R >>",
        stream("1 0 0 rg 10 10 20 20 re f\n"),
        stream(missing_font ? "BT /F1 12 Tf 10 30 Td (Missing font text) Tj ET\n" : "0 0 1 rg 10 10 40 20 re f\n"),
        "<< /Type /Font /Subtype /Type1 /BaseFont /VACardsRequestMissingFont73917 /Encoding /WinAnsiEncoding >>"
    };
    if (!missing_font) objects[3] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources << >> /Contents 6 0 R >>";
    std::ostringstream pdf;
    pdf << "%PDF-1.4\n";
    std::vector<long> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<long>(pdf.tellp()));
        pdf << i + 1 << " 0 obj\n" << objects[i] << "\nendobj\n";
    }
    auto xref = static_cast<long>(pdf.tellp());
    pdf << "xref\n0 " << objects.size() + 1 << "\n0000000000 65535 f \n";
    for (auto offset : offsets) pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
    pdf << "trailer\n<< /Size " << objects.size() + 1 << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
    return pdf.str();
}

std::string cdr_integer(unsigned n) {
 std::string s;for (unsigned i=0;i<4;++i) s.push_back(static_cast<char>(n>>(8*i)));return s;
}
std::string cdr_chunk(std::string tag,std::string data) {
 auto result=tag+cdr_integer(data.size())+data;if(data.size()%2) result+='\0';return result;
}
// Minimal uncompressed CDR7: two real page records, unequal widths, empty groups.
std::string two_page_cdr() {
 auto page=[](unsigned width) {return cdr_chunk("LIST","page"+
   cdr_chunk("mcfg",cdr_integer(width)+cdr_integer(254000))+
   cdr_chunk("LIST","grp "));};
 return cdr_chunk("RIFF","CDR7"+page(254000)+page(508000));
}
}
TEST_F(Files, PdfOpenImportPagesAndOneUndo) {
 auto path=write("two.pdf",request_pdf());
 for (auto pages : {array{1,2},array{2}}) {
  auto p=load(path);p["pages"]=pages;p["discard"]=true;
  auto opened=run("file.open",p);ASSERT_EQ(opened.status,Status::Changed)<<opened.reason<<" "<<opened.message;
  EXPECT_TRUE(context.document->isModifiedSinceSave());
  EXPECT_NE(xml().find("#0000ff"),std::string::npos);
  if(pages.size()==1) EXPECT_EQ(xml().find("#ff0000"),std::string::npos);
  else EXPECT_NE(xml().find("#ff0000"),std::string::npos);
  EXPECT_EQ(context.document->getPageManager().getPages().size(),pages.size());
  EXPECT_NEAR(context.document->getWidth().value("px"),(pages.size()==2?100:200)*96./72,0.01);
  if(pages.size()==2) {
   auto const &list=context.document->getPageManager().getPages();
   EXPECT_NEAR(list[1]->getDocumentRect().left(),100*96./72+20,0.01);
  }
  auto before=xml();auto bad=p;bad["pages"]=array{3};
  EXPECT_EQ(run("file.open",bad).reason,"pages-invalid");EXPECT_EQ(xml(),before);
  p.erase("discard");p["position"]=object{{"x",length(0)},{"y",length(0)}};
  auto imported=run("file.import",p);ASSERT_EQ(imported.status,Status::Changed)<<imported.reason<<" "<<imported.message;
  EXPECT_TRUE(imported.one_undo_step);auto after=xml();EXPECT_NE(before,after);
  ASSERT_TRUE(DocumentUndo::undo(context.document));EXPECT_EQ(xml(),before);
  ASSERT_TRUE(DocumentUndo::redo(context.document));EXPECT_EQ(xml(),after);
  bad=p;bad["pages"]=array{3};EXPECT_EQ(run("file.import",bad).reason,"pages-invalid");EXPECT_EQ(xml(),after);
 }
}
TEST_F(Files, PdfFontPoliciesMalformedAndSourceVersion) {
 auto path=write("font.pdf",request_pdf(true));
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);
 for(auto command:{"file.open","file.import"}) {
  auto p=load(path);p["pages"]=array{2};
  if(std::string(command)=="file.open")p["discard"]=true;
  else p["position"]=object{{"x",length(0)},{"y",length(0)}};
  auto before=xml();EXPECT_EQ(run(command,p).reason,"font-policy-required");EXPECT_EQ(xml(),before);
  p["font-policy"]="substitute";
  auto accepted=run(command,p);ASSERT_EQ(accepted.status,Status::Changed)<<accepted.reason<<" "<<accepted.message;
  auto loaded=load_editable_document(path,grants,{"pdf","embed","substitute",{2}});
  ASSERT_TRUE(loaded.document);ASSERT_FALSE(loaded.report.at("fonts").as_array().empty());
  EXPECT_EQ(loaded.report.at("fonts").as_array()[0].as_object().at("family"),"VACardsRequestMissingFont73917");
  auto version=IO::inspect_existing_file_version(path);ASSERT_TRUE(version.version);
  EXPECT_EQ(loaded.source_version.at("sha256").as_string(),version.version->sha256);
  p["path"]=write("bad.pdf","%PDF-1.4\nnot a PDF");before=xml();
  EXPECT_EQ(run(command,p).reason,"invalid-file");EXPECT_EQ(xml(),before);
 }
}
TEST_F(Files, CdrPagesOrderMissingAndConversionBudgets) {
 auto bytes=two_page_cdr();auto path=write("two.cdr",bytes);
 for(auto pages:{std::vector<unsigned>{1,2},std::vector<unsigned>{2,1},std::vector<unsigned>{2}}) {
  auto loaded=load_editable_document(path,grants,{"cdr","embed","substitute",pages});
  ASSERT_TRUE(loaded.document)<<(loaded.error?loaded.error->message:"");
  auto const &list=loaded.document->getPageManager().getPages();ASSERT_EQ(list.size(),pages.size());
  EXPECT_NEAR(list[0]->getDocumentRect().width(),pages[0]*96,0.01);
  if(pages.size()==2) EXPECT_NEAR(list[1]->getDocumentRect().left(),pages[0]*96+20,0.01);
 }
 auto open_params=load(path);open_params["pages"]=array{1,2};
 ASSERT_EQ(run("file.open",open_params).status,Status::Changed);
 auto import_params=open_params;import_params["position"]=object{{"x",length(0)},{"y",length(0)}};
 auto baseline=xml();auto both=run("file.import",import_params);
 ASSERT_EQ(both.status,Status::Changed)<<both.reason<<" "<<both.message;
 ASSERT_TRUE(DocumentUndo::undo(context.document));EXPECT_EQ(xml(),baseline);
 import_params["pages"]=array{3};EXPECT_EQ(run("file.import",import_params).reason,"pages-invalid");
 ASSERT_EQ(run("file.close",{{"discard",true}}).status,Status::Changed);
 auto bad=load_editable_document(path,grants,{"cdr","embed","substitute",{3}});
 ASSERT_TRUE(bad.error);EXPECT_EQ(bad.error->code,"pages-invalid");
 IntakeLimits limits;limits.max_conversion_pages=1;
 auto bounded=load_editable_document(path,grants,{"cdr","embed","substitute",{1}},limits);
 ASSERT_TRUE(bounded.error);EXPECT_EQ(bounded.error->code,"input-too-large");
 limits.max_conversion_pages=1000;limits.max_conversion_bytes=32;
 bounded=load_editable_document(path,grants,{"cdr","embed","substitute",{1}},limits);
 ASSERT_TRUE(bounded.error);EXPECT_EQ(bounded.error->code,"input-too-large");
 // Exercise real artwork and colliding IDs through the same composition/import path.
 auto fixture=std::string(INKSCAPE_TESTS_DIR)+"/cli_tests/vacards-agent/fixtures/m2/sheet.cdr";
 path=write("art.cdr",read(fixture));
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);auto before=xml();
 auto p=load(path);p["pages"]=array{1,1};p["font-policy"]="substitute";
 p["position"]=object{{"x",length(0)},{"y",length(0)}};
 auto imported=run("file.import",p);ASSERT_EQ(imported.status,Status::Changed)<<imported.reason<<" "<<imported.message;
 ASSERT_TRUE(DocumentUndo::undo(context.document));EXPECT_EQ(xml(),before);
}

// SVC: faults delegate every unaffected operation to the real native filesystem.
namespace {
namespace DT = IO::DocumentTransaction;
class ServiceCalls final : public DT::SystemCalls {
public:
 std::unique_ptr<DT::SystemCalls> native=DT::make_platform_system_calls();
 std::string fault; unsigned publications=0; std::function<void()> at_publish;
 bool create_exclusive_file(std::string &p,FILE *&f,bool &exists,std::string &e) override {
  if(fault=="create") { exists=false;e="injected create failure";return false; }
  return native->create_exclusive_file(p,f,exists,e);
 }
 bool flush_file(FILE *f,std::string &e) override {
  if(fault=="flush" || fault=="full-disk") {errno=fault=="full-disk"?ENOSPC:EIO;e=std::strerror(errno);return false;}
  return native->flush_file(f,e);
 }
 bool sync_file(FILE *f,bool &u,std::string &e) override {
  if(fault=="sync") {u=false;e="injected sync failure";return false;}return native->sync_file(f,u,e);
 }
 bool close_file(FILE *f,std::string &e) override {
  auto ok=native->close_file(f,e);if(fault=="close") {e="injected close failure";return false;}return ok;
 }
 DT::PublicationStatus publish_new_file(std::string const &s,std::string const &p,std::string &e) override {
  ++publications;if(at_publish) at_publish();
  if(fault=="unsupported") {e="injected unsupported primitive";return DT::PublicationStatus::Unsupported;}
  auto result=native->publish_new_file(s,p,e);
  if(fault=="uncertain" && result==DT::PublicationStatus::Published) {e="injected lost confirmation";return DT::PublicationStatus::Uncertain;}
  return result;
 }
 bool remove_file(std::string const &p) noexcept override {return native->remove_file(p);}
 bool sync_parent_directory(std::string const &p,bool &u,std::string &e) override {return native->sync_parent_directory(p,u,e);}
};
class ServiceFiles : public Files {
protected:
 Record save(FileServiceTestHooks const &hooks,object extra={}) {
  Request r;r.command="file.save";r.params={{"path",dir+"/out.svg"},{"embedding-policy","embed"}};
  for(auto const &v:extra) r.params[v.key()]=v.value();
  return execute_file_for_testing(r,context,state,grants,hooks);
 }
 void failure(std::string fault,unsigned stage=0) {
  ASSERT_EQ(run("file.new",dims()).status,Status::Changed);
  auto before=xml();auto anchor=context.document->get_event_log()->getLastSavedSerial();
  ServiceCalls calls;calls.fault=fault;FileServiceTestHooks hooks;hooks.calls=&calls;
  hooks.new_stage=[stage](unsigned s){return stage && s==stage;};
  auto result=save(hooks);EXPECT_EQ(result.reason,"publication-failed")<<result.message;
  EXPECT_FALSE(result.publication_persisted);EXPECT_FALSE(std::filesystem::exists(dir+"/out.svg"));
  EXPECT_EQ(xml(),before);EXPECT_TRUE(context.document->isModifiedSinceSave());
  EXPECT_EQ(context.document->get_event_log()->getLastSavedSerial(),anchor);
  EXPECT_FALSE(state.writes_blocked);EXPECT_EQ(calls.publications,0u);
  EXPECT_TRUE(std::filesystem::is_empty(dir));
 }
};
TEST_F(ServiceFiles, PublicationCreateFailure) {failure("create");}
TEST_F(ServiceFiles, PublicationWriteFailure) {failure("",2);}
TEST_F(ServiceFiles, PublicationFlushFailure) {failure("flush");}
TEST_F(ServiceFiles, PublicationSyncFailure) {failure("sync");}
TEST_F(ServiceFiles, PublicationCloseFailure) {failure("close");}
TEST_F(ServiceFiles, PublicationSealFailure) {failure("",3);}
TEST_F(ServiceFiles, PublicationFullDisk) {failure("full-disk");}
TEST_F(ServiceFiles, PublicationUnsupported) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);ServiceCalls calls;calls.fault="unsupported";
 FileServiceTestHooks hooks;hooks.calls=&calls;auto r=save(hooks);
 EXPECT_EQ(r.reason,"publication-unsupported");EXPECT_FALSE(r.publication_persisted);
 EXPECT_TRUE(context.document->isModifiedSinceSave());EXPECT_FALSE(state.writes_blocked);
 EXPECT_EQ(calls.publications,1u);EXPECT_TRUE(std::filesystem::is_empty(dir));
}
TEST_F(ServiceFiles, UncertainLatchAndReconciliationCloseOpen) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);
 ASSERT_EQ(run("file.save",{{"path",dir+"/anchor.svg"},{"embedding-policy","embed"}}).status,Status::Changed);
 context.document->getReprRoot()->setAttribute("data-dirty-edit","retained");
 DocumentUndo::done(context.document,Util::Internal::ContextString("dirty after save"),"");
 ASSERT_TRUE(context.document->isModifiedSinceSave());auto before=xml();
 auto anchor=context.document->get_event_log()->getLastSavedSerial();ASSERT_NE(anchor,0u);
 auto filename=std::string(context.document->getDocumentFilename());
 ServiceCalls calls;calls.fault="uncertain";FileServiceTestHooks hooks;hooks.calls=&calls;
 auto r=save(hooks);ASSERT_EQ(r.status,Status::Uncertain)<<r.message;
 EXPECT_EQ(r.reason,"publication-uncertain");EXPECT_FALSE(r.publication_persisted);
 EXPECT_TRUE(state.writes_blocked);EXPECT_TRUE(context.document->isModifiedSinceSave());
 EXPECT_EQ(context.document->get_event_log()->getLastSavedSerial(),anchor);EXPECT_EQ(xml(),before);
 EXPECT_EQ(std::string(context.document->getDocumentFilename()),filename);
 EXPECT_EQ(save({}).reason,"writes-blocked");
 EXPECT_EQ(run("file.export",{{"path",dir+"/blocked.svg"},{"format","svg"},{"page",1}}).reason,"writes-blocked");
 EXPECT_EQ(run("file.close",{{"discard",true}}).reason,"reconciliation-required");
 auto recovery=std::string(r.data.at("publication").as_object().at("recovery_path").as_string());
 ASSERT_FALSE(recovery.empty());EXPECT_EQ(read(recovery),read(dir+"/out.svg"));
 auto v=IO::inspect_existing_file_version(dir+"/out.svg");ASSERT_TRUE(v.version);
 object observed{{"identity",v.version->identity},{"sha256",v.version->sha256},{"bytes",v.version->bytes}};
#ifdef _WIN32
 auto alias=dir+"/different-entry.svg";
 ASSERT_TRUE(CreateHardLinkW(std::filesystem::u8path(alias).c_str(),std::filesystem::u8path(dir+"/out.svg").c_str(),nullptr));
 auto wrong=run("file.close",{{"discard",true},{"reconcile",object{{"destination",alias},{"observed-version",observed},{"acknowledge",true}}}});
 EXPECT_EQ(wrong.reason,"reconciliation-required");EXPECT_TRUE(context.document);EXPECT_TRUE(state.writes_blocked);
#endif
 auto closed=run("file.close",{{"discard",true},{"reconcile",object{{"destination",dir+"/out.svg"},{"observed-version",observed},{"acknowledge",true}}}});
 ASSERT_EQ(closed.status,Status::Changed)<<closed.reason<<": "<<closed.message;
 EXPECT_TRUE(state.writes_blocked);
 ASSERT_EQ(run("file.open",load(dir+"/out.svg")).status,Status::Changed);EXPECT_FALSE(state.writes_blocked);
 EXPECT_EQ(run("file.save",{{"path",dir+"/reconciled.svg"},{"embedding-policy","embed"}}).status,Status::Changed);
 EXPECT_EQ(calls.publications,1u);
}
TEST_F(ServiceFiles, PublishedCleanupWarning) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);FileServiceTestHooks hooks;
 hooks.new_stage=[](unsigned s){return s==5;};auto r=save(hooks);
 ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_TRUE(r.publication_persisted);
 EXPECT_NE(std::find(r.warnings.begin(),r.warnings.end(),"publication-cleanup"),r.warnings.end());
 EXPECT_FALSE(context.document->isModifiedSinceSave());EXPECT_FALSE(state.writes_blocked);
 EXPECT_FALSE(read(dir+"/out.svg").empty());
}
TEST_F(ServiceFiles, RacedNewFileConflict) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);ServiceCalls calls;
 calls.at_publish=[&]{write("out.svg","concurrent owner");};FileServiceTestHooks hooks;hooks.calls=&calls;
 auto r=save(hooks);EXPECT_EQ(r.reason,"publication-conflict");EXPECT_EQ(read(dir+"/out.svg"),"concurrent owner");
 EXPECT_TRUE(context.document->isModifiedSinceSave());EXPECT_FALSE(r.publication_persisted);EXPECT_FALSE(state.writes_blocked);
}
TEST_F(ServiceFiles, ExpectedVersionAtReplacementBoundary) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);auto target=write("out.svg","old");
 auto v=IO::inspect_existing_file_version(target);ASSERT_TRUE(v.version);FileServiceTestHooks hooks;bool boundary=false;
 hooks.replacement_stage=[&](unsigned s){if(s==4) {boundary=true;write("out.svg","raced");}return false;};
 auto r=save(hooks,{{"overwrite",true},{"expected-version",object{{"identity",v.version->identity},{"sha256",v.version->sha256},{"bytes",v.version->bytes}}}});
 EXPECT_TRUE(boundary);EXPECT_EQ(r.reason,"publication-conflict");EXPECT_EQ(read(target),"raced");EXPECT_FALSE(r.publication_persisted);
}
TEST_F(ServiceFiles, PrepareCancellation) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);auto before=xml();context.cancelled=[] {return true;};
 auto r=save({});EXPECT_EQ(r.status,Status::Cancelled);EXPECT_EQ(xml(),before);EXPECT_TRUE(std::filesystem::is_empty(dir));
}
TEST_F(ServiceFiles, BeforePublishCancellation) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);bool cancel=false;FileServiceTestHooks hooks;
 hooks.new_stage=[&](unsigned s){if(s==3)cancel=true;return false;};context.cancelled=[&]{return cancel;};
 auto r=save(hooks);EXPECT_TRUE(cancel);EXPECT_EQ(r.status,Status::Cancelled);EXPECT_TRUE(std::filesystem::is_empty(dir));
 EXPECT_TRUE(context.document->isModifiedSinceSave());
}
TEST_F(ServiceFiles, AfterBoundaryCancellationTooLate) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);bool cancel=false;ServiceCalls calls;
 calls.at_publish=[&]{cancel=true;};FileServiceTestHooks hooks;hooks.calls=&calls;context.cancelled=[&]{return cancel;};
 auto r=save(hooks);EXPECT_TRUE(cancel);EXPECT_EQ(r.status,Status::Changed);EXPECT_TRUE(r.publication_persisted);
 EXPECT_FALSE(context.document->isModifiedSinceSave());EXPECT_FALSE(read(dir+"/out.svg").empty());
}
} // namespace

namespace {
TEST_F(ServiceFiles, ImportPreservesGroupsCloneLockHiddenAndSelectionUndoRedo) {
 auto source=write("baseline.svg",R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" width="100" height="50"><defs/><g id="keep" transform="translate(2,3)"><rect id="member" width="4" height="5"/></g><use id="clone" xlink:href="#keep" x="20"/><rect id="locked" sodipodi:insensitive="true" width="2" height="3"/><rect id="hidden" style="display:none" width="8" height="9"/></svg>)SVG");
 ASSERT_EQ(run("file.open",load(source)).status,Status::Changed);
 auto selection=context.document->getSelection();selection->set(cast<SPItem>(context.document->getObjectById("keep")));
 auto before=xml();auto p=load(sheet());p["position"]=object{{"x",length(0)},{"y",length(0)}};
 auto subtree=[&](char const *id) {return sp_repr_write_buf(context.document->getObjectById(id)->getRepr(),0,false,Glib::QueryQuark(GQuark(0)),0,0).raw();};
 std::map<std::string,std::string> preserved;
 for(auto id:{"keep","member","clone","locked","hidden"}) preserved[id]=subtree(id);
 ASSERT_EQ(run("file.import",p,true).status,Status::Ok);EXPECT_EQ(xml(),before);
 EXPECT_EQ(selection->singleItem(),context.document->getObjectById("keep"));
 auto r=run("file.import",p);ASSERT_EQ(r.status,Status::Changed)<<r.message;
 auto after=xml();std::vector<std::string> selected;
 for(auto item:selection->items()) selected.emplace_back(item->getId());
 for(auto id:{"keep","member","clone","locked","hidden"}) {
  ASSERT_NE(context.document->getObjectById(id),nullptr);EXPECT_EQ(subtree(id),preserved.at(id));
 }
 ASSERT_TRUE(DocumentUndo::undo(context.document));EXPECT_EQ(xml(),before);
 EXPECT_EQ(selection->singleItem(),context.document->getObjectById("keep"));
 ASSERT_TRUE(DocumentUndo::redo(context.document));EXPECT_EQ(xml(),after);
 std::vector<std::string> redone;for(auto item:selection->items()) redone.emplace_back(item->getId());EXPECT_EQ(redone,selected);
}
TEST_F(ServiceFiles, SelectedExportPreservesLiveSelectionAndPayload) {
 ASSERT_EQ(run("file.open",load(sheet())).status,Status::Changed);
 auto selection=context.document->getSelection();selection->set(cast<SPItem>(context.document->getObjectById("outer")));
 auto before=xml();auto r=run("file.export",{{"path",dir+"/selected.svg"},{"format","svg"},{"ids",array{"outer"}}});
 ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_EQ(xml(),before);
 EXPECT_EQ(selection->singleItem(),context.document->getObjectById("outer"));
 auto reopened=load_editable_document(dir+"/selected.svg",grants,{"svg","embed","reject",{}});
 ASSERT_TRUE(reopened.document);EXPECT_NE(reopened.document->getObjectById("red"),nullptr);
 EXPECT_FALSE(context.document->isModifiedSinceSave());
}
TEST_F(ServiceFiles, SaveReopenAnchorAndExportHints) {
 ASSERT_EQ(run("file.open",load(sheet())).status,Status::Changed);
 context.document->getReprRoot()->setAttribute("inkscape:export-filename","retained.png");
 context.document->getReprRoot()->setAttribute("inkscape:export-xdpi","144");
 context.document->getReprRoot()->setAttribute("inkscape:export-ydpi","144");
 DocumentUndo::done(context.document,Util::Internal::ContextString("set export hints"),"");
 auto r=save({});ASSERT_EQ(r.status,Status::Changed)<<r.message;
 EXPECT_TRUE(context.document->get_event_log()->hasFileSaveAnchor());
 EXPECT_EQ(context.document->get_event_log()->getLastSavedSerial(),context.document->get_event_log()->getCurrEventSerial());
 EXPECT_FALSE(context.document->isModifiedSinceSave());
 ASSERT_EQ(run("file.close").status,Status::Changed);
 ASSERT_EQ(run("file.open",load(dir+"/out.svg")).status,Status::Changed);
 EXPECT_STREQ(context.document->getReprRoot()->attribute("inkscape:export-filename"),"retained.png");
 EXPECT_STREQ(context.document->getReprRoot()->attribute("inkscape:export-xdpi"),"144");
 EXPECT_STREQ(context.document->getReprRoot()->attribute("inkscape:export-ydpi"),"144");
 EXPECT_FALSE(context.document->isModifiedSinceSave());
}
TEST_F(ServiceFiles, DefaultFallbackNotice) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);
 std::filesystem::create_directories(dir+"/inkscape/color/icc");
 write("inkscape/color/icc/TheBest.icc","invalid bundled fixture");
 IO::ExportColorProfiles profiles(dir,dir,{},"");FileServiceTestHooks hooks;hooks.default_profiles=&profiles;
 Request request;request.command="file.export";request.params={{"path",dir+"/default.png"},{"format","png"},{"page",1},{"profile",object{{"id","default"}}}};
 auto r=execute_file_for_testing(request,context,state,grants,hooks);
 ASSERT_EQ(r.status,Status::Changed)<<r.message;
 EXPECT_FALSE(r.data.at("profile").as_object().at("notice").as_string().empty());
 EXPECT_NE(std::find(r.warnings.begin(),r.warnings.end(),"profile-fallback"),r.warnings.end());
 EXPECT_FALSE(read(dir+"/default.png").empty());
}
TEST_F(ServiceFiles, ExecutableExtensionRefusal) {
 auto path=write("extension.svg",R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape/extension"><inkscape:script>never execute</inkscape:script></svg>)");
 auto r=run("file.open",load(path));EXPECT_EQ(r.reason,"resource-denied");EXPECT_FALSE(state.document);
}
#ifdef __APPLE__
TEST_F(ServiceFiles, MountedNetworkIntakeAndPublicationRefusal) {
 auto root=std::filesystem::path("/Volumes/va-studio-network-test");
 if(!std::filesystem::exists(root)) GTEST_SKIP()<<"No mounted va-studio-network-test network volume";
 auto scratch=root/std::filesystem::path(dir).filename();
 ASSERT_TRUE(std::filesystem::create_directory(scratch));
 struct Cleanup {std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{scratch};
 auto path=(scratch/"input.svg").string();{std::ofstream f(path);f<<read(sheet());}
 grants.read_roots.push_back(scratch.string());grants.write_roots.push_back(scratch.string());
 auto r=run("file.open",load(path));EXPECT_EQ(r.reason,"invalid-file");EXPECT_FALSE(state.document);
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);
 auto output=(scratch/"output.svg").string();r=run("file.save",{{"path",output},{"embedding-policy","embed"}});
 EXPECT_EQ(r.status,Status::Rejected);EXPECT_EQ(r.reason,"unsafe-destination");
 EXPECT_FALSE(std::filesystem::exists(output));EXPECT_FALSE(r.publication_persisted);
}
#endif
} // namespace

namespace {
TEST_F(ServiceFiles, SourceChangeAfterAdmissionDoesNotReopenOrAlterSnapshot) {
 auto input=sheet();ASSERT_EQ(run("file.open",load(input)).status,Status::Changed);
 auto before=xml();write("sheet.svg","invalid changed source bytes");
 auto saved=save({});ASSERT_EQ(saved.status,Status::Changed)<<saved.message;
 EXPECT_NE(context.document->getObjectById("red"),nullptr);
 auto reopened=load_editable_document(dir+"/out.svg",grants,{"svg","embed","reject",{}});
 ASSERT_TRUE(reopened.document);EXPECT_NE(reopened.document->getObjectById("red"),nullptr);
 EXPECT_EQ(read(input),"invalid changed source bytes");
}
TEST_F(ServiceFiles, ProfileBytesChangedSincePinnedVersion) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);
 auto profile=IO::ExportColorProfiles::srgb();auto path=write("changed.icc",{profile.bytes.begin(),profile.bytes.end()});
 auto v=IO::inspect_existing_file_version(path);ASSERT_TRUE(v.version);write("changed.icc","changed after pinning");
 auto r=run("file.export",{{"path",dir+"/changed.png"},{"format","png"},{"page",1},
  {"profile",object{{"id","file"},{"path",path},{"sha256",v.version->sha256}}}});
 EXPECT_EQ(r.reason,"stale-dependency");EXPECT_FALSE(std::filesystem::exists(dir+"/changed.png"));
 EXPECT_TRUE(context.document->isModifiedSinceSave());EXPECT_FALSE(state.writes_blocked);
}
#ifndef _WIN32
TEST_F(ServiceFiles, StableDestinationParentIsExplicitTrustedPrecondition) {
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);struct stat before{},at{},after{};
 ASSERT_EQ(::stat(dir.c_str(),&before),0);ServiceCalls calls;bool checked=false;
 calls.at_publish=[&]{checked=true;EXPECT_EQ(::stat(dir.c_str(),&at),0);EXPECT_EQ(at.st_dev,before.st_dev);EXPECT_EQ(at.st_ino,before.st_ino);};
 FileServiceTestHooks hooks;hooks.calls=&calls;auto r=save(hooks);
 ASSERT_EQ(r.status,Status::Changed)<<r.message;EXPECT_TRUE(checked);ASSERT_EQ(::stat(dir.c_str(),&after),0);
 EXPECT_EQ(after.st_dev,before.st_dev);EXPECT_EQ(after.st_ino,before.st_ino);EXPECT_TRUE(r.publication_persisted);
 // This oracle qualifies the stated stable-parent precondition only. No atomic
 // compare-and-swap or hostile parent-rename protection is asserted.
 EXPECT_FALSE(read(dir+"/out.svg").empty());
}
#endif
} // namespace

namespace {
TEST_F(ServiceFiles, PerCommandSchemaAndPersistenceSnapshotMatrix) {
 for(auto command:{"file.new","file.open","file.close","file.import","file.save","file.export"}) {
  SCOPED_TRACE(command);
  if(context.document) ASSERT_EQ(run("file.close",{{"discard",true}}).status,Status::Changed);
  ASSERT_EQ(run("file.open",load(sheet())).status,Status::Changed);
  auto doc=context.document;auto selection=doc->getSelection();selection->set(cast<SPItem>(doc->getObjectById("outer")));
  doc->getReprRoot()->setAttribute("data-svc-history","one");
  DocumentUndo::done(doc,Util::Internal::ContextString("matrix first edit"),"");
  doc->getReprRoot()->setAttribute("data-svc-history","two");
  DocumentUndo::done(doc,Util::Internal::ContextString("matrix redo edit"),"");
  auto future_xml=xml();ASSERT_TRUE(DocumentUndo::undo(doc));auto before=xml();
  auto stamp=document_stamp(doc);auto dirty=doc->isModifiedSinceSave();
  auto anchor=doc->get_event_log()->getLastSavedSerial();auto mark=DocumentUndo::undoStackMark(doc);
  std::string filename=doc->getDocumentFilename()?doc->getDocumentFilename():"";
  auto unchanged=[&] {
   ASSERT_EQ(context.document,doc);EXPECT_EQ(xml(),before);EXPECT_EQ(selection->singleItem(),doc->getObjectById("outer"));
   EXPECT_EQ(document_stamp(doc).id,stamp.id);EXPECT_EQ(document_stamp(doc).revision,stamp.revision);
   EXPECT_EQ(doc->isModifiedSinceSave(),dirty);EXPECT_EQ(doc->get_event_log()->getLastSavedSerial(),anchor);
   EXPECT_EQ(DocumentUndo::undoStackMark(doc),mark);EXPECT_EQ(std::string(doc->getDocumentFilename()?doc->getDocumentFilename():""),filename);
  };
  auto output=dir+"/matrix-"+command+".svg";object p;
  if(std::string(command)=="file.new") {p=dims();p["discard"]=true;}
  if(std::string(command)=="file.open") {p=load(sheet());p["discard"]=true;}
  if(std::string(command)=="file.close") p={{"discard",true}};
  if(std::string(command)=="file.import") {p=load(sheet());p["position"]=object{{"x",length(0)},{"y",length(0)}};}
  if(std::string(command)=="file.save") p={{"path",output},{"embedding-policy","embed"}};
  if(std::string(command)=="file.export") p={{"path",output},{"format","svg"},{"ids",array{"outer"}}};
  auto check_schema=[&](Record const &r){auto result=typed_result(r,"matrix");EXPECT_FALSE(validate_schema(result,result_schema_descriptor(*find_command(command))))<<serialize(result);};
  auto dry=run(command,p,true);ASSERT_EQ(dry.status,Status::Ok)<<dry.message;check_schema(dry);unchanged();EXPECT_FALSE(std::filesystem::exists(output));
  context.file_handler=[&](Request const &r){return execute_file(r,context,state,grants);};
  Request stale;stale.id="matrix-stale";stale.command=command;stale.params=p;stale.document="wrong-incarnation";
  stale.if_revision=std::string(command)=="file.new"||std::string(command)=="file.open"||std::string(command)=="file.close"?context.session_revision:stamp.revision;
  auto refused=dispatch(stale,context);EXPECT_EQ(refused.reason,"stale-document");check_schema(refused);unchanged();
  context.cancelled=[] {return true;};auto cancelled=run(command,p);EXPECT_EQ(cancelled.status,Status::Cancelled);EXPECT_EQ(cancelled.reason,"cancelled");EXPECT_EQ(typed_result(cancelled,"matrix").at("error").as_object().at("code"),"cancelled");check_schema(cancelled);context.cancelled={};unchanged();
  ASSERT_TRUE(DocumentUndo::redo(doc));EXPECT_EQ(xml(),future_xml);ASSERT_TRUE(DocumentUndo::undo(doc));EXPECT_EQ(xml(),before);
  auto success=run(command,p);ASSERT_EQ(success.status,Status::Changed)<<success.message;check_schema(success);
  if(std::string(command)=="file.save") {EXPECT_TRUE(success.publication_persisted);EXPECT_FALSE(doc->isModifiedSinceSave());EXPECT_TRUE(doc->get_event_log()->hasFileSaveAnchor());EXPECT_FALSE(read(output).empty());}
  else if(std::string(command)=="file.export") {EXPECT_TRUE(success.publication_persisted);EXPECT_EQ(xml(),before);EXPECT_EQ(selection->singleItem(),doc->getObjectById("outer"));EXPECT_EQ(doc->isModifiedSinceSave(),dirty);EXPECT_EQ(doc->get_event_log()->getLastSavedSerial(),anchor);EXPECT_FALSE(read(output).empty());}
  else {EXPECT_FALSE(success.publication_persisted);EXPECT_FALSE(std::filesystem::exists(output));}
  if(std::string(command)=="file.import") {auto imported=xml();ASSERT_TRUE(DocumentUndo::undo(doc));EXPECT_EQ(xml(),before);ASSERT_TRUE(DocumentUndo::redo(doc));EXPECT_EQ(xml(),imported);}
  if(std::string(command)=="file.close") EXPECT_EQ(context.document,nullptr);
  if(std::string(command)=="file.open" || std::string(command)=="file.new") EXPECT_NE(document_stamp(context.document).id,stamp.id);
 }
}
} // namespace

TEST_F(Files, IntakeObserverPositiveControlAndRequestIsolation) {
 auto path=sheet(); std::vector<IntakeObservation> events, nested;
 FileLoadOptions opts{"svg","embed","substitute",{}};
 opts.observe_for_testing=[&](IntakeObservation const &event) {
  events.push_back(event);
  if(event.phase=="source" && event.access=="read-end") {
   auto inner=load_inspection_document_for_testing(path,grants,{},[&](auto const &e){nested.push_back(e);});
   EXPECT_TRUE(inner.document); EXPECT_FALSE(inner.error);
  }
 };
 auto loaded=load_editable_document(path,grants,opts); ASSERT_TRUE(loaded.document); ASSERT_FALSE(loaded.error);
 auto count=[](auto const &list, std::string const &phase, std::string const &access) {
  return std::count_if(list.begin(),list.end(),[&](auto const &e){return e.phase==phase && e.access==access;});
 };
 EXPECT_EQ(count(events,"source","read-end"),1); EXPECT_EQ(count(nested,"source","read-end"),1);
 EXPECT_EQ(count(events,"native-document","parser-begin"),1); EXPECT_EQ(count(events,"native-document","parser-end"),1);
 auto read=std::find_if(events.begin(),events.end(),[](auto const &e){return e.access=="read-end";});
 ASSERT_NE(read,events.end()); EXPECT_EQ(std::filesystem::path(read->path).lexically_normal().make_preferred(), std::filesystem::path(path).lexically_normal().make_preferred()); EXPECT_TRUE(read->admitted); EXPECT_FALSE(read->identity.empty());
 auto n=events.size(); auto m=nested.size();
 auto ordinary=load_editable_document(path,grants,FileLoadOptions{"svg","embed","substitute",{}});
 ASSERT_TRUE(ordinary.document); EXPECT_EQ(events.size(),n); EXPECT_EQ(nested.size(),m);
}
TEST_F(Files, IntakeObserverDeniedSourceDoesNotEnterParser) {
 auto path=sheet(); std::vector<IntakeObservation> events;
 FileLoadOptions opts{"svg","embed","substitute",{}};
 opts.observe_for_testing=[&](auto const &e){events.push_back(e);};
 auto loaded=load_editable_document(path,{},opts); ASSERT_TRUE(loaded.error); EXPECT_EQ(loaded.error->code,"read-grant-denied");
 ASSERT_FALSE(events.empty()); EXPECT_FALSE(events.front().admitted);
 for(auto const &e:events) EXPECT_NE(e.access,"parser-begin");
}
TEST_F(Files, FileOpenMissingFileRaceUsesSecondAdmission) {
 // The refused open must leave the live document untouched.
 ASSERT_EQ(run("file.open",load(sheet())).status,Status::Changed);
 auto path=write("race-source.svg",read(dir+"/sheet.svg")); unsigned deletions=0; std::vector<IntakeObservation> events;
 auto before=p9_state(); auto stamp=document_stamp(context.document);
 FileServiceTestHooks hooks;
 hooks.intake_observer=[&](IntakeObservation const &event) {
  events.push_back(event);
  if (!deletions && event.phase=="source" && event.access=="granted") {
   ++deletions; std::filesystem::remove(path);
  }
 };
 Request request; request.command="file.open"; request.params=load(path);
 auto result=execute_file_for_testing(request,context,state,grants,hooks);
 ASSERT_EQ(deletions,1u); ASSERT_FALSE(std::filesystem::exists(path));
 EXPECT_EQ(result.status,Status::Rejected) << result.reason << ": " << result.message;
 EXPECT_EQ(result.reason,"missing-file");
 ASSERT_NE(context.document,nullptr); EXPECT_EQ(document_stamp(context.document).id,stamp.id);
 auto after=p9_state(); EXPECT_EQ(after,before);
 std::cout<<"P9-OBSERVATION "<<serialize(object{{"obligation","error:file.open/missing-file"},
  {"command","file.open"},{"branch","admitted source read reports missing file"},
  {"code",result.reason},{"retryable",false},{"mutation_state","none"},{"before",before},{"after",after},{"settled",true},
  {"actual",object{{"race","source deleted after the first source/granted admission"},{"observations",events.size()}}}})<<std::endl;
}
TEST_F(Files, FileImportMissingFileRaceUsesSecondAdmission) {
 auto path=sheet(); unsigned deletions=0; std::vector<IntakeObservation> events;
 ASSERT_EQ(run("file.new",dims()).status,Status::Changed);
 auto before=p9_state();
 FileServiceTestHooks hooks;
 hooks.intake_observer=[&](IntakeObservation const &event) {
  events.push_back(event);
  if (!deletions && event.phase=="source" && event.access=="granted") {
   ++deletions; std::filesystem::remove(path);
  }
 };
 Request request; request.command="file.import"; request.params=load(path);
 request.params["position"]=object{{"x",length(0)},{"y",length(0)}};
 auto result=execute_file_for_testing(request,context,state,grants,hooks);
 ASSERT_EQ(deletions,1u); ASSERT_FALSE(std::filesystem::exists(path));
 EXPECT_EQ(result.status,Status::Rejected) << result.reason << ": " << result.message;
 EXPECT_EQ(result.reason,"missing-file");
 ASSERT_NE(context.document,nullptr);
 auto after=p9_state(); EXPECT_EQ(after,before);
 std::cout<<"P9-OBSERVATION "<<serialize(object{{"obligation","error:file.import/missing-file"},
  {"command","file.import"},{"branch","admitted source read reports missing file"},
  {"code",result.reason},{"retryable",false},{"mutation_state","none"},{"before",before},{"after",after},{"settled",true},
  {"actual",object{{"race","source deleted after the first source/granted admission"},{"observations",events.size()}}}})<<std::endl;
}
TEST_F(Files, P9R4SnapshotMetadataReplayFaultRows) {
 ASSERT_EQ(run("file.open",load(sheet())).status,Status::Changed);
 auto snapshot_state=[&] {return p9_state();};
 for (auto command:{"file.save","file.export"}) {
  SCOPED_TRACE(command);
  auto spec=find_command(command); ASSERT_NE(spec,nullptr);
  auto parsed=parse_request(spec->example); ASSERT_TRUE(parsed.request);
  auto base=*parsed.request; base.params={{"path",dir+"/metadata-"+std::string(command)+".svg"}};
  if (std::string(command)=="file.save") base.params["embedding-policy"]="embed";
  else { base.params["format"]="svg"; base.params["ids"]=array{"outer"}; }
  auto before=snapshot_state(); auto stamp=document_stamp(context.document);
  {
   ScopedCliFaultPlanForTesting scope({{},{{"file.payload.bytes",0}}});
   auto r=base; r.dry_run=false; auto result=execute_file_for_testing(r,context,state,grants,{});
   EXPECT_EQ(result.reason,"input-too-large")<<result.message;
   auto after=snapshot_state(); EXPECT_EQ(after,before); EXPECT_EQ(document_stamp(context.document).revision,stamp.revision);
   EXPECT_FALSE(std::filesystem::exists(std::string(base.params.at("path").as_string())));
   std::cout<<"P9-OBSERVATION "<<serialize(object{{"obligation","error:"+std::string(command)+"/input-too-large"},
    {"command",command},{"branch","snapshot serialization exceeds bounded intake size"},
    {"code",result.reason},{"retryable",false},{"mutation_state","none"},{"before",before},{"after",after},{"settled",true},
    {"actual",object{{"fault_point","cli_counted_limit:file.payload.bytes"}}}})<<std::endl;
  }
  {
   ScopedCliFaultPlanForTesting scope({{{"intake.snapshot.after-copy",CliFaultKind::ServiceException}}, {}});
   auto r=base; r.dry_run=false; auto result=execute_file_for_testing(r,context,state,grants,{});
   EXPECT_EQ(result.reason,"intake-failed")<<result.message;
   auto after=snapshot_state(); EXPECT_EQ(after,before); EXPECT_EQ(document_stamp(context.document).revision,stamp.revision);
   EXPECT_FALSE(std::filesystem::exists(std::string(base.params.at("path").as_string())));
   std::cout<<"P9-OBSERVATION "<<serialize(object{{"obligation","error:"+std::string(command)+"/intake-failed"},
    {"command",command},{"branch","snapshot normalization or native construction throws and is caught as intake-failed"},
    {"code",result.reason},{"retryable",false},{"mutation_state","none"},{"before",before},{"after",after},{"settled",true},
    {"actual",object{{"fault_point","intake.snapshot.after-copy"}}}})<<std::endl;
  }
  std::cout<<"P9-METADATA-FAULT-ONLY "<<serialize(object{{"id","error:"+std::string(command)+"/invalid-file"},
   {"reason","No real admitted in-memory document fixture was found that makes snapshot native construction fail; no fault was used."}})<<std::endl;
 }
}
TEST_F(Files, IntakeObserverRemoteReferenceNeverRead) {
 auto path=write("remote-observer.svg","<svg xmlns=\"http://www.w3.org/2000/svg\"><image href=\"https://example.invalid/marker.png\" width=\"1\" height=\"1\"/></svg>");
 std::vector<IntakeObservation> events; FileLoadOptions opts{"svg","embed","substitute",{}};
 opts.observe_for_testing=[&](auto const &e){events.push_back(e);};
 auto loaded=load_editable_document(path,grants,opts); ASSERT_TRUE(loaded.error); EXPECT_EQ(loaded.error->code,"remote-resource");
 EXPECT_TRUE(std::any_of(events.begin(),events.end(),[](auto const &e){return e.phase=="image" && e.access=="remote" && !e.admitted;}));
 for(auto const &e:events) { EXPECT_NE(e.phase,"linked"); EXPECT_NE(e.phase,"native-document"); }
}
TEST_F(Files, IntakeObserverSnapshotPositiveControl) {
 auto loaded=load_editable_document(sheet(),grants,FileLoadOptions{"svg","embed","substitute",{}}); ASSERT_TRUE(loaded.document);
 std::vector<IntakeObservation> events;
 auto snapshot=prepare_file_snapshot_for_testing(*loaded.document,grants,"embed",[&](auto const &e){events.push_back(e);});
 ASSERT_TRUE(snapshot.document); ASSERT_FALSE(snapshot.error);
 for(auto phase:{"snapshot-copy","snapshot-svg","snapshot-native"}) {
  EXPECT_EQ(std::count_if(events.begin(),events.end(),[&](auto const &e){return e.phase==phase && e.access=="parser-begin";}),1);
  EXPECT_EQ(std::count_if(events.begin(),events.end(),[&](auto const &e){return e.phase==phase && e.access=="parser-end";}),1);
 }
}
