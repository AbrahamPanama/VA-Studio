// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <future>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "io/existing-file-replacement.h"
#include "actions/actions-vacards-file.h"
#include "actions/vacards-cli-session.h"
#include "actions/vacards-cli-entry.h"
#include "actions/vacards-cli-production.h"
#include "io/vacards-cli-files.h"
#include "inkscape.h"
using namespace Inkscape::VACardsCli;
using namespace boost::json;
namespace {
class VACardsCliM2Integration : public ::testing::Test {
protected:
    void SetUp() override {
        if (!Inkscape::Application::exists())
            Inkscape::Application::create(false, Inkscape::Application::RuntimePolicy::PreviewHelper);
    }
    std::unique_ptr<SPDocument> document() {
        std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"/>)";
        auto d = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(), svg.size()));
        d->ensureUpToDate(); d->setModifiedSinceSave(false); return d;
    }
};
TEST_F(VACardsCliM2Integration, AllSixDescriptorsAreIntegrated) {
    EXPECT_EQ(file_commands().size(), 6u);
    for (auto id : {"file.new", "file.open", "file.close", "file.import", "file.save", "file.export"}) {
        auto spec = find_command(id);
        ASSERT_NE(spec, nullptr) << id;
        auto parsed = parse_request(spec->example);
        ASSERT_TRUE(parsed.request) << id << ": " << (parsed.error ? parsed.error->message : "");
        auto envelope = parse(spec->example).as_object();
        envelope.erase("if_revision");
        EXPECT_TRUE(parse_request(serialize(envelope)).error) << id;
        if (spec->needs_document) {
            envelope = parse(spec->example).as_object(); envelope.erase("document");
            EXPECT_TRUE(parse_request(serialize(envelope)).error) << id;
        }
    }
}
TEST_F(VACardsCliM2Integration, MissingDocumentRefusesBeforeIdentityAndHandler) {
    DispatchContext c;
    unsigned called = 0;
    c.file_handler = [&](Request const &) { ++called; return Record{}; };
    for (auto id : {"file.import", "file.save", "file.export"}) {
        auto spec = find_command(id); ASSERT_NE(spec, nullptr);
        auto parsed = parse_request(spec->example); ASSERT_TRUE(parsed.request);
        auto result = dispatch(*parsed.request, c);
        EXPECT_EQ(result.reason, "no-document"); EXPECT_EQ(called, 0u);
    }
}
TEST_F(VACardsCliM2Integration, TypedLifecycleLengthsRetainRangeErrors) {
    auto spec = find_command("file.new"); ASSERT_NE(spec, nullptr);
    for (double value : {-1., 0., 1e20}) {
        auto r = parse(spec->example).as_object();
        r.at("params").as_object()["width"] = object{{"value", value}, {"unit", "px"}};
        auto parsed = parse_request(serialize(r));
        ASSERT_TRUE(parsed.error); EXPECT_EQ(parsed.error->code, "out-of-range");
    }
}
TEST_F(VACardsCliM2Integration, LifecycleGuardsPrecedeCallbackIncludingDryRun) {
    auto spec = find_command("file.new"); ASSERT_NE(spec, nullptr);
    auto d = document(); auto stamp = document_stamp(d.get());
    DispatchContext c{nullptr, d.get(), d->getSelection()}; c.session_revision = 7;
    unsigned called = 0;
    c.file_handler = [&](Request const &) { ++called; return Record{}; };
    auto parsed = parse_request(spec->example); ASSERT_TRUE(parsed.request);
    auto r = *parsed.request; r.document = stamp.id; r.if_revision = 7; r.dry_run = true;
    r.params["unknown"] = true;
    EXPECT_EQ(dispatch(r, c).reason, "unknown-key"); EXPECT_EQ(called, 0u);
    r.params.erase("unknown"); r.document = "stale";
    EXPECT_EQ(dispatch(r, c).reason, "stale-document"); EXPECT_EQ(called, 0u);
    r.document = stamp.id; r.if_revision = 6;
    EXPECT_EQ(dispatch(r, c).reason, "stale-revision"); EXPECT_EQ(called, 0u);
    r.if_revision = 7; r.document.reset();
    EXPECT_EQ(dispatch(r, c).reason, "missing-required"); EXPECT_EQ(called, 0u);
    r.document = stamp.id; c.cancelled = [] { return true; };
    EXPECT_EQ(dispatch(r, c).status, Status::Cancelled); EXPECT_EQ(called, 0u);
    c.cancelled = {}; auto result = dispatch(r, c);
    EXPECT_EQ(called, 1u); EXPECT_TRUE(result.dry_run);
    EXPECT_EQ(document_stamp(d.get()).revision, stamp.revision);
}
TEST_F(VACardsCliM2Integration, DocumentGuardsAndStructuredParamsReachFileCallback) {
    auto spec = find_command("file.export"); ASSERT_NE(spec, nullptr);
    auto d = document(); auto stamp = document_stamp(d.get());
    DispatchContext c{nullptr, d.get(), d->getSelection()}; c.session_revision = 12;
    auto parsed = parse_request(spec->example); ASSERT_TRUE(parsed.request);
    auto r = *parsed.request; r.document = stamp.id; r.if_revision = stamp.revision; r.dry_run = true;
    unsigned called = 0;
    c.file_handler = [&](Request const &forwarded) {
        ++called; EXPECT_EQ(forwarded.params, r.params); EXPECT_TRUE(forwarded.dry_run); return Record{};
    };
    r.if_revision = 12;
    EXPECT_EQ(dispatch(r, c).reason, "stale-revision"); EXPECT_EQ(called, 0u);
    r.if_revision = stamp.revision;
    auto result = dispatch(r, c); EXPECT_EQ(called, 1u);
    for (auto const &p : spec->params)
        if (p.descriptor && p.type != ParamType::Length)
            if (auto v = r.params.if_contains(p.key)) EXPECT_EQ(result.normalized_params.at(p.key), *v);
}
TEST_F(VACardsCliM2Integration, RefusedFileImportCannotReportCommittedEffects) {
    auto spec = find_command("file.import"); ASSERT_NE(spec, nullptr);
    auto d = document(); auto stamp = document_stamp(d.get());
    DispatchContext c{nullptr, d.get(), d->getSelection()};
    auto parsed = parse_request(spec->example); ASSERT_TRUE(parsed.request);
    auto r = *parsed.request; r.document = stamp.id; r.if_revision = stamp.revision;
    c.file_handler = [](Request const &) {
        Record result; result.status = Status::Cancelled; result.reason = "cancelled";
        result.created = {"temporary"}; result.modified = {"pending"}; result.deleted = {"rolled-back"};
        return result;
    };
    auto result = dispatch(r, c);
    EXPECT_EQ(result.status, Status::Cancelled);
    EXPECT_TRUE(result.created.empty()); EXPECT_TRUE(result.modified.empty()); EXPECT_TRUE(result.deleted.empty());
    EXPECT_EQ(document_stamp(d.get()).revision, stamp.revision);
}
TEST_F(VACardsCliM2Integration, NativeReplacementCancellationSurvivesDispatchAndWire) {
    auto temporary = g_dir_make_tmp("va-p2-r1-XXXXXX", nullptr); ASSERT_NE(temporary, nullptr);
    std::filesystem::path dir = std::filesystem::canonical(temporary); g_free(temporary);
    struct Cleanup { std::filesystem::path dir; ~Cleanup() { std::filesystem::remove_all(dir); } } cleanup{dir};
    auto target = (dir/"out.svg").string(); { std::ofstream f(target); f << "old destination"; }
    auto version = Inkscape::IO::inspect_existing_file_version(target); ASSERT_TRUE(version.version);
    FileState state; state.document = document();
    auto stamp = document_stamp(state.document.get());
    DispatchContext context{nullptr, state.document.get(), state.document->getSelection()};
    Grants grants; grants.read_roots = {dir.string()}; grants.write_roots = {dir.string()};
    unsigned checks = 0;
    // dispatch, file preparation, publication preparation, then R1's native boundary.
    context.cancelled = [&] { return ++checks >= 4; };
    context.file_handler = [&](Request const &r) { return execute_file(r, context, state, grants); };
    auto parsed = parse_request(find_command("file.save")->example); ASSERT_TRUE(parsed.request);
    auto request = *parsed.request; request.document = stamp.id; request.if_revision = stamp.revision;
    request.params["path"] = target; request.params["overwrite"] = true;
    request.params["expected-version"] = object{{"identity", version.version->identity},
        {"sha256", version.version->sha256}, {"bytes", version.version->bytes}};
    auto record = dispatch(request, context);
    EXPECT_EQ(record.status, Status::Cancelled); EXPECT_EQ(record.reason, "cancelled"); EXPECT_GE(checks, 4u);
    EXPECT_NE(record.message.find("replacement"), std::string::npos) << record.message;
    auto result = typed_result(record, "cancelled-save");
    EXPECT_EQ(result.at("status"), "cancelled"); EXPECT_EQ(result.at("error").as_object().at("code"), "cancelled");
    EXPECT_EQ(result.at("error").as_object().at("retryable"), false);
    EXPECT_EQ(result.at("publication"), (object{{"state", "not-published"}, {"persisted", false}}));
    EXPECT_FALSE(validate_schema(result, result_schema_descriptor(*find_command("file.save"))));
    EXPECT_EQ(typed_exit_status(record.status), 3); EXPECT_EQ(document_stamp(state.document.get()).revision, stamp.revision);
    std::ifstream input(target); std::string bytes{std::istreambuf_iterator<char>(input), {}};
    EXPECT_EQ(bytes, "old destination"); EXPECT_FALSE(state.writes_blocked);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}), 1);
}
TEST_F(VACardsCliM2Integration, PublicationTruthAndLegacyBytes) {
    Record r; r.action = "file.save"; r.status = Status::Changed;
    auto legacy = to_json_line(r, 1);
    r.publication = "published"; r.publication_persisted = true;
    EXPECT_EQ(to_json_line(r, 1), legacy);
    EXPECT_EQ(typed_result(r, "saved").at("publication"), (object{{"state", "published"}, {"persisted", true}}));
    r.status = Status::Uncertain; r.reason = "publication-uncertain";
    r.publication = "uncertain"; r.publication_persisted = false;
    r.data = {{"recovery", object{{"destination", "/unconfirmed.svg"}}}};
    auto result = typed_result(r, "uncertain");
    EXPECT_EQ(result.at("status"), "uncertain"); EXPECT_EQ(result.at("data"), r.data);
    EXPECT_EQ(result.at("publication").as_object().at("persisted"), false);
    EXPECT_EQ(typed_exit_status(r.status), 4);
    EXPECT_NE(result.at("error").as_object().at("hint").as_string().find("recovery"), string::npos);
}
TEST_F(VACardsCliM2Integration, ProbeReportsBackendCapabilitiesWithoutSliceAcceptance) {
    auto identity = cli_identity();
    EXPECT_EQ(identity.at("enabled_slices"), array{"M1"});
    EXPECT_EQ(identity.at("capabilities").as_object().at("m2_accepted"), false);
    EXPECT_EQ(identity.at("capabilities").as_object().at("files"), file_capabilities());
    EXPECT_EQ(identity.at("catalog_hash"), production_catalog().at("hash"));
    EXPECT_TRUE(identity.at("capabilities").as_object().at("m3_accepted").as_bool());
    EXPECT_FALSE(identity.at("capabilities").as_object().at("m3_experimental").as_bool());
}
TEST_F(VACardsCliM2Integration, OneShotLifecycleHasOneResultAndNeverAutosaves) {
    auto spec = find_command("file.new"); ASSERT_NE(spec, nullptr);
    std::string bytes;
    auto exit = run_agent_request(spec->example, [&](std::string_view s) { bytes += s; return true; });
    EXPECT_EQ(exit, 0) << bytes;
    EXPECT_EQ(std::count(bytes.begin(), bytes.end(), '\n'), 1);
    auto r = parse(bytes).as_object();
    EXPECT_EQ(r.at("publication").as_object().at("persisted"), false);
    EXPECT_EQ(r.at("revision_after"), 0);
    EXPECT_FALSE(r.at("document_id").as_string().empty());
    EXPECT_FALSE(validate_schema(r, result_schema_descriptor(*spec)));
}
} // namespace

namespace {
TEST_F(VACardsCliM2Integration, EofCancelsRealFilePreparationExactlyOnce) {
 std::mutex mutex;std::condition_variable wake;std::string bytes;bool eof=false,preparing=false;
 std::vector<object> events;SessionOptions options;
 options.before_dispatch=[&](Request const &r,auto const &cancelled) {
  if(r.command!="file.new") return;
  {std::lock_guard lock(mutex);preparing=true;wake.notify_all();}
  auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!cancelled() && std::chrono::steady_clock::now()<until) std::this_thread::yield();
 };
 auto parsed=parse(find_command("file.new")->example).as_object();parsed["id"]="eof-file";
 bytes=serialize(parsed)+"\n";
 auto engine=std::async(std::launch::async,[&]{return run_agent_session([&](char *p,std::size_t n) {
  std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(10),[&]{return !bytes.empty()||eof;});
  if(bytes.empty()) return eof?0:-2;n=std::min(n,bytes.size());std::copy_n(bytes.data(),n,p);bytes.erase(0,n);return int(n);
 },[&](std::string_view line){std::lock_guard lock(mutex);events.push_back(parse(line).as_object());wake.notify_all();return true;},options);});
 {std::unique_lock lock(mutex);EXPECT_TRUE(wake.wait_for(lock,std::chrono::seconds(5),[&]{return preparing;}));eof=true;wake.notify_all();}
 EXPECT_EQ(engine.get(),0);unsigned accepted=0,terminal=0;
 for(auto const &event:events) if(auto id=event.if_contains("id");id && *id=="eof-file") {
  if(event.at("event")=="accepted") ++accepted;
  if(event.at("event")=="result") {++terminal;auto const &r=event.at("result").as_object();
   EXPECT_EQ(r.at("status"),"cancelled");EXPECT_EQ(r.at("publication").as_object().at("persisted"),false);
   EXPECT_TRUE(r.at("document_id").as_string().empty());
  }
 }
 EXPECT_EQ(accepted,1u);EXPECT_EQ(terminal,1u);
}
} // namespace

namespace {
TEST_F(VACardsCliM2Integration, RealFileOpenReportsNativeUninterruptiblePhase) {
 auto temporary=g_dir_make_tmp("va-svc-phase-XXXXXX",nullptr);ASSERT_NE(temporary,nullptr);
 std::filesystem::path dir=std::filesystem::canonical(temporary);g_free(temporary);
 struct Cleanup {std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{dir};
 auto path=(dir/"many.svg").string();{std::ofstream f(path);f<<"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">";
 for(unsigned i=0;i<45000;++i) f<<"<rect x=\"1\" y=\"2\" width=\"3\" height=\"4\"/>";f<<"</svg>";}
 auto request=parse(find_command("file.open")->example).as_object();request["id"]="native-open";
 request.at("params").as_object()["path"]=path;
 std::string bytes=serialize(request)+"\n";std::mutex mutex;std::condition_variable wake;
 bool finished=false;std::vector<object> events;SessionOptions options;options.grants.read_roots={dir.string()};
 auto result=run_agent_session([&](char *p,std::size_t n){
  std::unique_lock lock(mutex);if(bytes.empty()) {wake.wait_for(lock,std::chrono::milliseconds(10));return finished?0:-2;}
  n=std::min(n,bytes.size());std::copy_n(bytes.data(),n,p);bytes.erase(0,n);return int(n);
 },[&](std::string_view line){std::lock_guard lock(mutex);auto event=parse(line).as_object();
  if(event.at("event")=="result") finished=true;events.push_back(event);wake.notify_all();return true;},options);
 EXPECT_EQ(result,0);unsigned native=0,terminals=0;
 for(auto const &event:events) {
  if(event.at("event")=="progress" && event.at("phase")=="native-call") {++native;EXPECT_EQ(event.at("uninterruptible"),true);}
  if(event.at("event")=="result") {++terminals;EXPECT_EQ(event.at("result").as_object().at("status"),"changed");}
 }
 EXPECT_GT(native,0u);EXPECT_EQ(terminals,1u);
}
} // namespace

TEST_F(VACardsCliM2Integration, NativeOwnerRetiresOnlyAtActualDocumentSwap) {
    FileState files; files.document=document();
    DispatchContext context; context.document=files.document.get(); context.selection=context.document->getSelection();
    unsigned retired=0;
    files.before_document_retire=[&] { EXPECT_NE(context.document,nullptr); EXPECT_EQ(context.document,files.document.get()); ++retired; };
    Request r; r.command="file.new"; r.params={{"width",object{{"value",20},{"unit","px"}}},
        {"height",object{{"value",30},{"unit","px"}}},{"discard",true}};
    auto original=files.document.get(); r.dry_run=true;
    EXPECT_EQ(execute_file(r,context,files,{}).status,Status::Ok); EXPECT_EQ(retired,0u); EXPECT_EQ(files.document.get(),original);
    r.dry_run=false; EXPECT_EQ(execute_file(r,context,files,{}).status,Status::Changed); EXPECT_EQ(retired,1u);
    original=files.document.get();
    r.command="file.open"; r.params={{"path","/INT-no-grant.svg"},{"discard",true},{"resource-policy","embed"},{"font-policy","reject"}};
    EXPECT_EQ(execute_file(r,context,files,{}).status,Status::Rejected); EXPECT_EQ(retired,1u); EXPECT_EQ(files.document.get(),original);
    r.command="file.close"; r.params={{"discard",true}}; r.dry_run=true;
    EXPECT_EQ(execute_file(r,context,files,{}).status,Status::Ok); EXPECT_EQ(retired,1u);
    r.dry_run=false; EXPECT_EQ(execute_file(r,context,files,{}).status,Status::Changed); EXPECT_EQ(retired,2u); EXPECT_FALSE(files.document);
}
