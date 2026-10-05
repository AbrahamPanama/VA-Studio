// SPDX-License-Identifier: GPL-2.0-or-later
#include "config.h"
#include "inkscape-version.h"
#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#undef IGNORE
#undef near
#else
#include <pthread.h>
#endif
#include "actions/vacards-cli-session.h"
#include "actions/vacards-cli-entry.h"
using namespace Inkscape::VACardsCli;
using namespace boost::json;
using namespace std::chrono_literals;
namespace {
std::string request(std::string id, std::string command = "system.catalog", object params = {}) {
    return serialize(object{{"schema", "va-studio.cli-request/1"}, {"id", id}, {"command", command}, {"params", params}}) + "\n";
}
struct Harness {
    std::mutex mutex;
    std::condition_variable wake;
    std::string bytes;
    bool eof = false;
    std::vector<object> events;
    std::future<int> engine;
    explicit Harness(SessionOptions options = {}) {
        engine = std::async(std::launch::async, [this, options] {
            return run_agent_session([this](char *p, std::size_t n) {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, 10ms, [&] { return !bytes.empty() || eof; });
                if (bytes.empty()) return eof ? 0 : -2;
                n = std::min(n, bytes.size()); std::copy_n(bytes.data(), n, p); bytes.erase(0, n); return int(n);
            }, [this](std::string_view line) {
                std::lock_guard lock(mutex); events.push_back(parse(line).as_object()); wake.notify_all(); return true;
            }, options);
        });
    }
    void send(std::string s) { std::lock_guard lock(mutex); bytes += s; wake.notify_all(); }
    void end() { std::lock_guard lock(mutex); eof = true; wake.notify_all(); }
    object wait(std::string event, std::string id = "") {
        std::unique_lock lock(mutex);
        auto find = [&]() { return std::find_if(events.begin(), events.end(), [&](auto const &v) {
            return v.at("event") == value(event) && (id.empty() || v.at("id") == value(id));
        }); };
        if (!wake.wait_for(lock, 10s, [&] { return find() != events.end(); })) {
            ADD_FAILURE() << "Timed out waiting for " << event << " / " << id; return {};
        }
        return *find();
    }
    int finish() { end(); return engine.get(); }
    ~Harness() { if (engine.valid()) { end(); engine.wait(); } }
};
std::string code(object const &event) {
    return std::string(event.at("result").as_object().at("error").as_object().at("code").as_string());
}
std::vector<object> lines(std::string const &text) {
    std::vector<object> out; std::istringstream input(text); std::string line;
    while (std::getline(input, line)) out.push_back(parse(line).as_object()); return out;
}
std::vector<value> diagnostic_records(std::string const &text) {
    std::vector<value> records; std::istringstream input(text); std::string line;
    while (std::getline(input, line)) {
        boost::system::error_code error;
        auto record = parse(line, error);
        if (!error) records.push_back(std::move(record));
    }
    return records;
}
}
TEST(VacardsCliSession, HelloCatalogSequenceAndDuplicate) {
    Harness h; auto hello = h.wait("hello");
    ASSERT_EQ(hello.at("schema"), "va-studio.cli-session/1");
    EXPECT_TRUE(hello.at("document").is_null()); EXPECT_TRUE(hello.contains("session_id"));
    EXPECT_EQ(hello.at("enabled_slices"), array{"M1"});
    h.send(request("catalog")); auto result = h.wait("result", "catalog");
    ASSERT_EQ(result.at("result").as_object().at("status"), "ok");
    EXPECT_EQ(hello.at("catalog_hash"), result.at("result").as_object().at("data").as_object().at("hash"));
    h.send(request("catalog"));
    h.send(request("status", "session.status")); h.wait("result", "status");
    EXPECT_EQ(h.finish(), 0);
    std::uint64_t seq = 0; unsigned accepted = 0, terminal = 0, duplicate = 0;
    for (auto const &e : h.events) {
        EXPECT_GT(e.at("event_seq").to_number<std::uint64_t>(), seq); seq = e.at("event_seq").to_number<std::uint64_t>();
        if (e.at("event") == "accepted") ++accepted;
        if (e.at("event") == "result") {
            if (auto err = e.at("result").as_object().if_contains("error"); err && !err->is_null() && err->as_object().at("code") == "duplicate-request-id") ++duplicate;
            else ++terminal;
        }
    }
    EXPECT_EQ(accepted, terminal); EXPECT_EQ(duplicate, 1u);
}
TEST(VacardsCliSession, BusyStatusCancelDuringSlowRequest) {
    std::promise<void> started;
    SessionOptions opts;
    opts.before_dispatch = [&](auto const &, auto const &cancelled) {
        started.set_value();
        for (int i = 0; i < 500 && !cancelled(); ++i) std::this_thread::sleep_for(10ms);
    };
    Harness h(opts); h.wait("hello"); h.send(request("slow")); started.get_future().wait();
    h.send(request("busy") + request("status", "session.status") + request("cancel", "session.cancel", {{"id", "slow"}}));
    EXPECT_EQ(code(h.wait("result", "busy")), "session-busy");
    auto status = h.wait("result", "status").at("result").as_object().at("data").as_object();
    EXPECT_EQ(status.at("active_request"), "slow"); EXPECT_EQ(status.at("phase"), "preparation");
    EXPECT_EQ(h.wait("result", "cancel").at("result").as_object().at("data").as_object().at("found"), true);
    EXPECT_EQ(h.wait("result", "slow").at("result").as_object().at("status"), "cancelled");
    EXPECT_EQ(h.finish(), 0);
}
TEST(VacardsCliSession, FramingRecoveryAndFragmentation) {
    Harness h; h.wait("hello");
    h.send(std::string(request_limit + 20, 'x') + "\n" + std::string("\xff\n", 2) + "{broken\n");
    auto r = request("fragment", "session.status"); r.pop_back(); r += "\r\n";
    for (auto c : r) h.send(std::string(1, c));
    EXPECT_EQ(h.wait("result", "fragment").at("result").as_object().at("status"), "ok");
    EXPECT_EQ(h.finish(), 0);
    std::vector<std::string> errors;
    for (auto const &e : h.events) if (e.at("event") == "result") {
        auto const &v = e.at("result").as_object();
        if (auto err = v.if_contains("error"); err && !err->is_null()) {
            errors.emplace_back(err->as_object().at("code").as_string());
            EXPECT_FALSE(err->as_object().at("hint").as_string().empty());
        }
    }
    EXPECT_EQ(errors, (std::vector<std::string>{"request-too-large", "invalid-utf8", "malformed-json"}));
}
TEST(VacardsCliSession, StreamFramingBoundaryAndFatalTransport) {
    std::istringstream s("abc\r\nlast"); auto read = stream_reader(s); JsonLines f; std::string line;
    EXPECT_EQ(f.next(read, line), JsonLines::State::Line); EXPECT_EQ(line, "abc");
    EXPECT_EQ(f.next(read, line), JsonLines::State::Line); EXPECT_EQ(line, "last");
    EXPECT_EQ(f.next(read, line), JsonLines::State::End);
    std::string fragments = "fragmented\r\n"; std::size_t offset = 0;
    ByteReader one_byte = [&](char *p, std::size_t) { if (offset == fragments.size()) return 0; *p = fragments[offset++]; return 1; };
    JsonLines fragmented;
    EXPECT_EQ(fragmented.next(one_byte, line), JsonLines::State::Line); EXPECT_EQ(line, "fragmented");
    EXPECT_EQ(run_agent_session([](char *, std::size_t) { return -1; }, [](auto) { return true; }), 2);
    EXPECT_EQ(run_agent_session([](char *, std::size_t) { return 0; }, [](auto) { return false; }), 2);
}
TEST(VacardsCliSession, EofCancelsPreparationAndProducesTerminal) {
    std::promise<void> started; SessionOptions options;
    options.before_dispatch = [&](auto const &, auto const &cancelled) {
        started.set_value(); for (int i=0; i<500 && !cancelled(); ++i) std::this_thread::sleep_for(10ms);
    };
    Harness h(options); h.wait("hello"); h.send(request("eof")); started.get_future().wait(); h.end();
    EXPECT_EQ(h.wait("result", "eof").at("result").as_object().at("status"), "cancelled");
    EXPECT_EQ(h.engine.get(), 0);
}
TEST(VacardsCliSession, ReleaseCloseAndImmutableBoundary) {
    Harness h; h.wait("hello");
    h.send(request("release", "session.release", {{"tokens", array{"unknown"}}}));
    EXPECT_EQ(h.wait("result", "release").at("result").as_object().at("data").as_object().at("not_found"), array{"unknown"});
    // Keep this Build-30 boundary check on the unchanged legacy route.
    auto edit = parse(request("edit", "vacards-boolean", {{"op", "union"}})).as_object();
    edit["document"] = "d1"; edit["if_revision"] = 0;
    h.send(serialize(edit) + "\n"); EXPECT_EQ(code(h.wait("result", "edit")), "stale-document");
    h.send(request("sink", "system.result-file", {{"target", "-"}}));
    EXPECT_EQ(code(h.wait("result", "sink")), "session-output-fixed");
    h.send(request("close", "session.close")); h.wait("result", "close");
    ASSERT_EQ(h.engine.wait_for(5s), std::future_status::ready); EXPECT_EQ(h.engine.get(), 0);
}
TEST(VacardsCliSession, StartupErrorKeepsSystemCommandsUsable) {
    SessionOptions options; options.inspection_path = "/nonexistent/va-cli-test.svg";
    Harness h(options); auto hello = h.wait("hello");
    EXPECT_TRUE(hello.at("document").is_null()); EXPECT_TRUE(hello.at("startup_error").as_object().contains("hint"));
    h.send(request("catalog")); EXPECT_EQ(h.wait("result", "catalog").at("result").as_object().at("status"), "ok");
    EXPECT_EQ(h.finish(), 0);
}
TEST(VacardsCliSession, OneShotCodesAndPersisted) {
    std::string output; auto write = [&](auto s) { output += s; return true; };
    EXPECT_EQ(run_agent_request(request("one"), write), 0);
    EXPECT_FALSE(parse(output).as_object().contains("persisted"));
    EXPECT_EQ(parse(output).as_object().at("publication").as_object().at("persisted"), false);
    output.clear(); EXPECT_EQ(run_agent_request("{", write), 2);
    output.clear(); EXPECT_EQ(run_agent_request(request("bad", "unknown"), write), 3);
}
namespace {
class VacardsCliProcess : public ::testing::Test {
protected:
    std::filesystem::path dir, executable;
    void SetUp() override {
        if (g_getenv("VACARDS_CLI_DIAGNOSTIC_CHILD")) return;
        auto p = g_dir_make_tmp("va-cli-process-XXXXXX", nullptr); ASSERT_NE(p, nullptr); dir=p; g_free(p);
        std::filesystem::create_directory(dir/"user-profile");
        auto current = std::filesystem::current_path();
        for (int i=0; i<8; ++i) {
            for (auto candidate : {current/"bin/vastudio-cli", current/"build/bin/vastudio-cli"}) {
#ifdef _WIN32
                candidate += ".exe";
#endif
                if (std::filesystem::exists(candidate)) executable=candidate;
            }
            if (!executable.empty()) break; current=current.parent_path();
        }
        ASSERT_FALSE(executable.empty());
    }
    void TearDown() override { std::filesystem::remove_all(dir); }
    std::string write(std::string name, std::string bytes) {
        auto path=dir/name; std::ofstream f(path, std::ios::binary); f << bytes; return path.string();
    }
    struct Run { int exit; std::string out, err; };
    Run run(std::vector<std::string> args, std::string input = {}, bool diagnostic_child = false) {
        auto program = executable;
        if (diagnostic_child) {
            program = executable.parent_path()/"test_vacards-cli-session";
#ifdef _WIN32
            program += ".exe";
#endif
        }
        args.insert(args.begin(), program.string()); std::vector<char *> argv;
        for (auto &s:args) argv.push_back(s.data()); argv.push_back(nullptr);
        auto env=g_get_environ(); env=g_environ_setenv(env,"INKSCAPE_PROFILE_DIR",(dir/"user-profile").string().c_str(),true);
        env=g_environ_setenv(env,"DISPLAY","",true);
        if (diagnostic_child) env=g_environ_setenv(env,"VACARDS_CLI_DIAGNOSTIC_CHILD","1",true);
        else env=g_environ_unsetenv(env,"VACARDS_CLI_DIAGNOSTIC_CHILD");
        GError *error=nullptr;
        auto launcher=g_subprocess_launcher_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDIN_PIPE|G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_PIPE));
        g_subprocess_launcher_set_environ(launcher,env); g_strfreev(env);
        auto process=g_subprocess_launcher_spawnv(launcher,argv.data(),&error); g_object_unref(launcher);
        if (!process) { ADD_FAILURE() << error->message; g_clear_error(&error); return {-1,{},{}}; }
        char *out=nullptr,*err=nullptr;
        bool ok=g_subprocess_communicate_utf8(process,input.c_str(),nullptr,&out,&err,&error);
        EXPECT_TRUE(ok); if(error) { ADD_FAILURE()<<error->message; g_clear_error(&error); }
        Run result{g_subprocess_get_if_exited(process)?g_subprocess_get_exit_status(process):-1,out?out:"",err?err:""};
        g_free(out); g_free(err); g_object_unref(process); return result;
    }
};
}
TEST_F(VacardsCliProcess, VersionMalformedLaunchAndOneShotExits) {
    auto version=run({"--version","--json"}); EXPECT_EQ(version.exit,0);
    EXPECT_EQ(parse(version.out).as_object().at("build"),VACARDS_PRODUCT_VERSION);
    EXPECT_EQ(parse(version.out).as_object().at("catalog_hash"),cli_identity().at("catalog_hash"));
    auto bad=run({"--actions=vacards-describe"}); EXPECT_EQ(bad.exit,2); EXPECT_TRUE(bad.out.empty());
    auto errors=diagnostic_records(bad.err); ASSERT_EQ(errors.size(),1u) << bad.err;
    EXPECT_EQ(diagnostic_records("native diagnostic\n"+bad.err+"another diagnostic\n"),errors);
    ASSERT_TRUE(errors.front().is_object());
    EXPECT_EQ(errors.front().as_object().at("schema"),"va-studio.cli-result/1");
    EXPECT_EQ(errors.front().as_object().at("status"),"rejected");
    EXPECT_EQ(errors.front().as_object().at("error").as_object().at("code"),"invalid-launch");
    for (auto const &logs : {version.err, bad.err}) {
        EXPECT_EQ(logs.find("g_log_writer_default_set_use_stderr"),std::string::npos) << logs;
        EXPECT_EQ(logs.find("g_thread_n_created"),std::string::npos) << logs;
    }
    EXPECT_EQ(run({"--agent-request-file",write("ok.json",request("ok"))}).exit,0);
    EXPECT_EQ(run({"--agent-request-file",write("malformed.json","{")}).exit,2);
    EXPECT_EQ(run({"--agent-request-file",write("rejected.json",request("bad","unknown"))}).exit,3);
    EXPECT_TRUE(std::filesystem::is_empty(dir/"user-profile"));
}
#ifdef _WIN32
TEST_F(VacardsCliProcess, WindowsWideArgumentsReachCliAsUtf8) {
    auto req = (dir/"request ").string() + "café request # 请求.json";
    { std::ofstream file(std::filesystem::u8path(req), std::ios::binary);
      file << request("options", "system.options", {{"preferred-unit", "in"}}); }
    for (auto const *name : {"ascii.svg", "space #.svg", "café.svg", "矢量 😺.svg"}) {
        SCOPED_TRACE(name);
        auto input = dir / std::filesystem::u8path(name);
        auto input_arg = dir.string() + "/" + name;
        { std::ofstream file(input, std::ios::binary);
          file << "<svg xmlns=\"http://www.w3.org/2000/svg\"/>"; }
        auto destination = dir.string() + "/out " + name;
        auto result = run({"--agent-request-file", req,
                           "--grant-read-file", input_arg,
                           "--grant-write-file", destination});
        ASSERT_EQ(result.exit, 0) << result.err;
        auto records = lines(result.out); ASSERT_EQ(records.size(), 1u) << result.out;
        auto const &record = records.front();
        EXPECT_EQ(record.at("schema"), "va-studio.cli-result/1");
        EXPECT_EQ(record.at("id"), "options");
        EXPECT_EQ(record.at("action"), "system.options");
        EXPECT_EQ(record.at("status"), "ok");
        EXPECT_EQ(record.at("data").as_object().at("preferred-unit"), "in");
        EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(destination)));
    }
    EXPECT_TRUE(std::filesystem::is_empty(dir/"user-profile"));
}
#endif

TEST_F(VacardsCliProcess, InspectionSvgSvgzWarningsAndPrivateProfile) {
    auto svg=write("sheet.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><rect id=\"r\" width=\"5\" height=\"5\"/></svg>");
    auto compressed=(dir/"sheet.svgz").string(); auto gz=gzopen(compressed.c_str(),"wb"); ASSERT_NE(gz,nullptr);
    std::ifstream f(svg); std::string bytes((std::istreambuf_iterator<char>(f)),{}); gzwrite(gz,bytes.data(),bytes.size()); gzclose(gz);
    for(auto const &path:{svg,compressed}) {
        auto result=run({"--agent-session","--inspect-file",path},request("status","session.status")+request("close","session.close"));
        EXPECT_EQ(result.exit,0) << result.err;
        auto events=lines(result.out); ASSERT_FALSE(events.empty());
        ASSERT_FALSE(events.front().at("document").is_null()) << result.out << result.err;
        EXPECT_EQ(events.front().at("document").as_object().at("read_only"),true);
        EXPECT_TRUE(std::filesystem::is_empty(dir/"user-profile"));
    }
    auto failed=run({"--agent-session","--inspect-file",(dir/"missing.svg").string()});
    EXPECT_EQ(failed.exit,0); auto hello=lines(failed.out).front();
    EXPECT_TRUE(hello.at("document").is_null()); EXPECT_TRUE(hello.contains("startup_error"));
}

TEST_F(VacardsCliProcess, ProcessWarningsAndStrayStdoutStayOffProtocol) {
    if (g_getenv("VACARDS_CLI_DIAGNOSTIC_CHILD")) {
        // Re-exec with --gtest_brief to keep the test runner off stdout. Exit
        // directly after agent_main so its completion banner is not emitted.
        // Explicitly exercise startup after GLib has created a thread.
        auto thread=g_thread_new("cli-log-precondition",[](void *) -> void * { return nullptr; },nullptr);
        g_thread_join(thread);
        char name[]="vastudio-cli", version[]="--version", json[]="--json";
        char *argv[]={name,version,json,nullptr};
        int result=agent_main(3,argv,[] {
            g_warning("P2 diagnostic warning");
            g_message("P2 diagnostic message");
            g_log_structured("P2",G_LOG_LEVEL_MESSAGE,"MESSAGE","P2 structured diagnostic");
            g_print("P2 stray GLib print\n");
            printf("P2 stray printf\n"); fflush(stdout);
        });
        std::_Exit(result);
    }
    auto result=run({"--gtest_filter=VacardsCliProcess.ProcessWarningsAndStrayStdoutStayOffProtocol",
                     "--gtest_brief=1"},{},true);
    EXPECT_EQ(result.exit,0) << result.err;
    auto const &protocol=result.out, &logs=result.err;
    auto records=lines(protocol); ASSERT_EQ(records.size(),1u) << protocol;
    EXPECT_EQ(records.front().at("build"),VACARDS_PRODUCT_VERSION);
    EXPECT_NE(logs.find("P2 diagnostic warning"),std::string::npos);
    EXPECT_NE(logs.find("P2 diagnostic message"),std::string::npos);
    EXPECT_NE(logs.find("P2 structured diagnostic"),std::string::npos);
    EXPECT_NE(logs.find("P2 stray GLib print"),std::string::npos);
    EXPECT_NE(logs.find("P2 stray printf"),std::string::npos);
    EXPECT_EQ(logs.find("g_log_writer_default_set_use_stderr"),std::string::npos) << logs;
    EXPECT_EQ(logs.find("g_thread_n_created"),std::string::npos) << logs;
}
TEST(VacardsCliSession, VersionIdentityDoesNotInitializeDisplay) {
    EXPECT_FALSE(gtk_is_initialized()); auto identity=cli_identity();
    EXPECT_EQ(identity.at("build"),VACARDS_PRODUCT_VERSION);
    EXPECT_EQ(identity.at("product"),VACARDS_PRODUCT_NAME);
    EXPECT_EQ(identity.at("source_sha"),Inkscape::revision_full_string);
    auto sha=identity.at("source_sha").as_string();
    if (std::string_view(Inkscape::revision_full_string) == "unknown") {
        EXPECT_EQ(sha, "unknown"); // archive build without .git: the identity says so explicitly
    } else {
        EXPECT_EQ(sha.size(),40u);
        EXPECT_TRUE(std::all_of(sha.begin(),sha.end(),[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    }
    EXPECT_FALSE(gtk_is_initialized());
}

TEST(VacardsCliSession, PrivateOptionsDryRunAndGuardedClose) {
    Harness h; h.wait("hello");
    h.send(request("options", "system.options", {{"preferred-unit", "in"}, {"halt-on-error", true}}));
    auto result=h.wait("result", "options").at("result").as_object();
    EXPECT_EQ(result.at("data").as_object().at("preferred-unit"),"in");
    EXPECT_FALSE(validate_schema(result, result_schema_descriptor(*find_command("system.options"))));
    auto dry=parse(request("dry", "session.close")).as_object(); dry["dry_run"]=true;
    h.send(serialize(dry)+"\n"); h.wait("result","dry");
    auto guarded=parse(request("guard", "session.close")).as_object(); guarded["if_revision"]=999;
    h.send(serialize(guarded)+"\n"); EXPECT_EQ(code(h.wait("result","guard")),"stale-revision");
    h.send(request("status","session.status")); auto status=h.wait("result","status").at("result").as_object();
    EXPECT_FALSE(validate_schema(status, result_schema_descriptor(*find_command("session.status"))));
    EXPECT_EQ(h.finish(),0);
    std::string fresh;
    EXPECT_EQ(run_agent_request(request("fresh","system.options"),[&](auto s){fresh+=s;return true;}),0);
    auto data=parse(fresh).as_object().at("data").as_object();
    EXPECT_EQ(data.at("preferred-unit"),"mm"); EXPECT_EQ(data.at("halt-on-error"),false);
}
TEST(VacardsCliSession, InternalFailureHasOneTerminalAndExitFour) {
    SessionOptions options; options.before_dispatch=[](auto const &,auto const &){throw std::runtime_error("test failure");};
    Harness h(options); h.wait("hello"); h.send(request("failure"));
    EXPECT_EQ(h.wait("result","failure").at("result").as_object().at("status"),"failed");
    ASSERT_EQ(h.engine.wait_for(5s),std::future_status::ready); EXPECT_EQ(h.engine.get(),4);
    unsigned accepted=0,terminal=0;
    for(auto const &event:h.events) { if(event.at("event")=="accepted")++accepted; if(event.at("event")=="result")++terminal; }
    EXPECT_EQ(accepted,1u); EXPECT_EQ(terminal,1u);
}

TEST(VacardsCliSession, OptionsAndCatalogUseOneSessionRevision) {
    Harness h; h.wait("hello");
    h.send(request("options", "system.options", {{"preferred-unit", "in"}}));
    auto options = h.wait("result", "options").at("result").as_object();
    ASSERT_EQ(options.at("status"), "ok");
    EXPECT_EQ(options.at("revision_before"), 0);
    EXPECT_EQ(options.at("revision_after"), 0); // document revision, not session revision
    h.send(request("status", "session.status"));
    auto revision = h.wait("result", "status").at("result").as_object().at("data").as_object().at("session_revision");
    ASSERT_EQ(revision, 1);
    auto catalog = parse(request("current")).as_object(); catalog["if_revision"] = revision;
    h.send(serialize(catalog) + "\n");
    EXPECT_EQ(h.wait("result", "current").at("result").as_object().at("status"), "ok");
    catalog["id"] = "old"; catalog["if_revision"] = 0;
    h.send(serialize(catalog) + "\n");
    EXPECT_EQ(code(h.wait("result", "old")), "stale-revision");
    h.send(request("unchanged", "session.status"));
    EXPECT_EQ(h.wait("result", "unchanged").at("result").as_object().at("data").as_object().at("session_revision"), revision);
    EXPECT_EQ(h.finish(), 0);
}

TEST(VacardsCliSession, CancelledDispatchDoesNotCallSessionHandler) {
    for (auto command : {"system.options", "session.release", "session.close"}) {
        SCOPED_TRACE(command);
        auto params = std::string_view(command) == "session.release" ? object{{"tokens", array{"token-1"}}} : object{};
        auto parsed = parse_request(request("cancelled", command, params));
        ASSERT_TRUE(parsed.request);
        bool called = false;
        DispatchContext context;
        context.cancelled = [] { return true; };
        context.session_handler = [&](auto const &) { called = true; return Record{}; };
        auto result = dispatch(*parsed.request, context);
        EXPECT_FALSE(called);
        EXPECT_EQ(result.status, Status::Cancelled);
        EXPECT_EQ(result.reason, "cancelled");
    }
}

TEST(VacardsCliSession, CancelledSessionControlsDoNotMutate) {
    for (auto command : {"system.options", "session.release"}) {
        SCOPED_TRACE(command);
        std::promise<void> started, resume;
        auto gate = resume.get_future().share();
        SessionOptions options;
        options.before_dispatch = [&](auto const &r, auto const &) {
            if (r.id == "target") { started.set_value(); gate.wait(); }
        };
        Harness h(options); h.wait("hello");
        struct ResumeOnExit {
            std::promise<void> &resume;
            bool released = false;
            void release() { if (!released) { resume.set_value(); released = true; } }
            ~ResumeOnExit() { release(); }
        } release{resume};
        object params;
        if (std::string_view(command) == "system.options") params = {{"preferred-unit", "in"}, {"halt-on-error", true}};
        if (std::string_view(command) == "session.release") params = {{"tokens", array{"token-1"}}};
        h.send(request("target", command, params));
        auto ready = started.get_future().wait_for(5s);
        EXPECT_EQ(ready, std::future_status::ready);
        if (ready != std::future_status::ready) continue;
        h.send(request("cancel", "session.cancel", {{"id", "target"}}));
        EXPECT_EQ(h.wait("result", "cancel").at("result").as_object().at("data").as_object().at("found"), true);
        // The cancelled flag must not prevent immediate status from responding.
        h.send(request("during", "session.status"));
        EXPECT_EQ(h.wait("result", "during").at("result").as_object().at("status"), "ok");
        release.release();
        auto result = h.wait("result", "target");
        EXPECT_EQ(result.at("result").as_object().at("status"), "cancelled");
        EXPECT_EQ(code(result), "cancelled");
        h.send(request("after", "session.status"));
        EXPECT_EQ(h.wait("result", "after").at("result").as_object().at("data").as_object().at("session_revision"), 1);
        h.send(request("options", "system.options"));
        auto data = h.wait("result", "options").at("result").as_object().at("data").as_object();
        EXPECT_EQ(data.at("preferred-unit"), "mm"); EXPECT_EQ(data.at("halt-on-error"), false);
        EXPECT_EQ(h.finish(), 0);
    }
}

TEST(VacardsCliSession, DryRunControlResultsPreserveFlagAndState) {
    for (auto command : {"session.status", "session.cancel", "session.release", "session.close", "system.options"}) {
        SCOPED_TRACE(command);
        auto spec = find_command(command); ASSERT_NE(spec, nullptr);
        auto example = parse(spec->example).as_object();
        example["id"] = "dry"; example["dry_run"] = true;
        Harness h; h.wait("hello"); h.send(serialize(example) + "\n");
        auto result = h.wait("result", "dry").at("result").as_object();
        EXPECT_EQ(result.at("status"), "ok"); EXPECT_EQ(result.at("dry_run"), true);
        EXPECT_FALSE(validate_schema(result, result_schema_descriptor(*spec)));
        EXPECT_EQ(result.at("revision_before"), 0); EXPECT_EQ(result.at("revision_after"), 0);
        if (std::string_view(command) != "session.status")
            EXPECT_EQ(result.at("data").as_object().at("validation_level"), "preflight");
        example["id"] = "stale"; example["if_revision"] = 99;
        h.send(serialize(example) + "\n");
        auto stale = h.wait("result", "stale");
        EXPECT_EQ(code(stale), "stale-revision");
        EXPECT_EQ(stale.at("result").as_object().at("dry_run"), true);
        h.send(request("status", "session.status"));
        EXPECT_EQ(h.wait("result", "status").at("result").as_object().at("data").as_object().at("session_revision"), 0);
        h.send(request("options", "system.options"));
        auto data = h.wait("result", "options").at("result").as_object().at("data").as_object();
        EXPECT_EQ(data.at("preferred-unit"), "mm"); EXPECT_EQ(data.at("halt-on-error"), false);
        EXPECT_EQ(h.finish(), 0);
    }
}

TEST(VacardsCliSession, EveryRegisteredSessionExampleMatchesGeneratedResultSchema) {
    unsigned commands = 0, executions = 0;
    for (auto spec : command_specs()) {
        if (!spec->canonical_id.starts_with("session.")) continue;
        ++commands;
        SCOPED_TRACE(spec->canonical_id);
        ASSERT_FALSE(spec->example.empty());
        auto example = parse(spec->example).as_object();
        ASSERT_FALSE(validate_schema(example, request_schema(*spec)));
        ASSERT_TRUE(parse_request(spec->example).request);
        for (bool dry : {false, true}) {
            SCOPED_TRACE(dry);
            example["dry_run"] = dry;
            auto id = std::string(example.at("id").as_string());
            Harness h; h.wait("hello"); h.send(serialize(example) + "\n");
            auto result = h.wait("result", id).at("result").as_object();
            auto schema = result_schema_descriptor(*spec);
            auto expected = !dry && (spec->canonical_id == "session.cancel" || spec->canonical_id == "session.release")
                ? "unchanged" : "ok";
            EXPECT_EQ(result.at("status"), expected) << serialize(result);
            EXPECT_EQ(result.at("dry_run"), dry);
            EXPECT_FALSE(validate_schema(result, schema)) << serialize(result) << "\n" << serialize(schema);
            // Ensure schema validation is active, as in the generic registry loop.
            auto wrong = result; wrong["created"] = 42;
            EXPECT_TRUE(validate_schema(wrong, schema));
            EXPECT_EQ(h.finish(), 0);
            ++executions;
        }
    }
    EXPECT_GT(commands, 0u); EXPECT_EQ(executions, commands * 2);
}

TEST(VacardsCliSession, SchemaRefusalKeepsIdentityWithoutAcceptance) {
    Harness h; auto hello = h.wait("hello");
    EXPECT_EQ(hello.at("protocol"), "va-studio.cli-session/1");
    h.send(request("bad-params", "query.styles", {{"properties", array(11, "fill")}}));
    auto event = h.wait("result", "bad-params");
    auto const &result = event.at("result").as_object();
    EXPECT_EQ(result.at("id"), "bad-params");
    EXPECT_EQ(result.at("action"), "query.styles");
    EXPECT_EQ(code(event), "out-of-range");
    EXPECT_EQ(result.at("normalized_params"), object{});
    EXPECT_FALSE(validate_schema(result, result_schema_descriptor(*find_command("query.styles"))));
    EXPECT_EQ(h.finish(), 0);
    for (auto const &e : h.events) EXPECT_NE(e.at("event"), "accepted");
    std::string output;
    EXPECT_EQ(run_agent_request(request("one-invalid", "query.styles", {{"properties", array(11, "fill")}}),
        [&](auto text) { output += text; return true; }), 3);
    auto one = parse(output).as_object();
    EXPECT_EQ(one.at("id"), "one-invalid"); EXPECT_EQ(one.at("action"), "query.styles");
    EXPECT_FALSE(one.contains("persisted"));
}

TEST(VacardsCliSession, DirtyLifecycleCloseAndStatusSnapshot) {
    auto spec = find_command("file.new"); ASSERT_NE(spec, nullptr);
    Harness h; h.wait("hello");
    auto fresh = parse(spec->example).as_object(); fresh["id"] = "new";
    h.send(serialize(fresh) + "\n");
    auto created = h.wait("result", "new").at("result").as_object();
    ASSERT_EQ(created.at("status"), "changed") << serialize(created);
    auto identity = created.at("document_id");
    h.send(request("status", "session.status"));
    auto state = h.wait("result", "status").at("result").as_object().at("data").as_object();
    EXPECT_EQ(state.at("session_revision"), 1);
    EXPECT_EQ(state.at("document").as_object().at("id"), identity);
    EXPECT_EQ(state.at("document").as_object().at("read_only"), false);
    EXPECT_EQ(state.at("document").as_object().at("dirty"), true);
    h.send(request("dirty-close", "session.close"));
    EXPECT_EQ(code(h.wait("result", "dirty-close")), "dirty-document");
    h.send(request("discard", "session.close", {{"discard", true}}));
    EXPECT_EQ(h.wait("result", "discard").at("result").as_object().at("status"), "ok");
    ASSERT_EQ(h.engine.wait_for(5s), std::future_status::ready); EXPECT_EQ(h.engine.get(), 0);
}
TEST_F(VacardsCliProcess, ExplicitGrantsAreRepeatableAndDoNotImplyStartupEditing) {
    auto input = write("granted.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"/>");
    auto req = write("grants.json", request("grants"));
    auto destination = (dir/"output.svg").string();
    auto result = run({"--agent-request-file", req,
        "--grant-read", dir.string(), "--grant-read", dir.string(),
        "--grant-write", dir.string(), "--grant-write", dir.string(),
        "--grant-read-file", input, "--grant-read-file", input,
        "--grant-write-file", destination, "--grant-write-file", destination});
    EXPECT_EQ(result.exit, 0) << result.err;
    auto records = lines(result.out); ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().at("publication").as_object().at("persisted"), false);
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_TRUE(std::filesystem::is_empty(dir/"user-profile"));
    auto inspected = run({"--agent-request-file", req, "--inspect-file", input});
    EXPECT_EQ(inspected.exit, 0) << inspected.err;
    auto invalid = run({"--agent-request-file", req, "--document", input, "--inspect-file", input});
    EXPECT_EQ(invalid.exit, 2); EXPECT_TRUE(invalid.out.empty());
}

TEST_F(VacardsCliProcess, NativeProfileAvailabilityIsRetryableOnSessionWire) {
    SessionOptions options; options.grants.read_roots = {dir.string()}; options.grants.write_roots = {dir.string()};
    Harness h(options); h.wait("hello");
    auto fresh = parse(find_command("file.new")->example).as_object(); fresh["id"] = "new";
    h.send(serialize(fresh) + "\n");
    auto created = h.wait("result", "new").at("result").as_object();
    ASSERT_EQ(created.at("status"), "changed") << serialize(created);
    auto export_request = parse(find_command("file.export")->example).as_object();
    export_request["id"] = "unavailable"; export_request["document"] = created.at("document_id");
    export_request["if_revision"] = created.at("revision_after"); export_request["dry_run"] = true;
    auto &params = export_request.at("params").as_object();
    params["path"] = (dir/"output.png").string(); params["format"] = "png";
    params.erase("drawing"); params["page"] = 1;
    params["profile"] = object{{"id", "file"}, {"path", (dir/"offline.icc").string()}, {"sha256", std::string(64, '0')}};
    h.send(serialize(export_request) + "\n");
    auto result = h.wait("result", "unavailable").at("result").as_object();
    EXPECT_EQ(result.at("status"), "rejected") << serialize(result);
    EXPECT_EQ(result.at("error").as_object().at("code"), "resource-unavailable");
    EXPECT_EQ(result.at("error").as_object().at("retryable"), true);
    EXPECT_EQ(result.at("publication"), (object{{"state", "not-published"}, {"persisted", false}}));
    EXPECT_FALSE(validate_schema(result, result_schema_descriptor(*find_command("file.export"))));
    EXPECT_FALSE(std::filesystem::exists(dir/"output.png"));
    h.send(request("status", "session.status"));
    EXPECT_EQ(h.wait("result", "status").at("result").as_object().at("status"), "ok");
    EXPECT_EQ(h.finish(), 0);
}
TEST(VacardsCliSession, CancelledFileRequestHasExactlyOneTerminalResult) {
    std::promise<void> started; SessionOptions options;
    options.before_dispatch = [&](auto const &r, auto const &cancelled) {
        if (r.id != "file") return;
        started.set_value();
        for (int i = 0; i < 500 && !cancelled(); ++i) std::this_thread::sleep_for(10ms);
    };
    Harness h(options); h.wait("hello");
    auto fresh = parse(find_command("file.new")->example).as_object(); fresh["id"] = "file";
    h.send(serialize(fresh) + "\n"); started.get_future().wait();
    h.send(request("cancel", "session.cancel", {{"id", "file"}}));
    EXPECT_EQ(h.wait("result", "cancel").at("result").as_object().at("data").as_object().at("found"), true);
    auto result = h.wait("result", "file").at("result").as_object();
    EXPECT_EQ(result.at("status"), "cancelled"); EXPECT_EQ(result.at("error").as_object().at("code"), "cancelled");
    EXPECT_EQ(result.at("error").as_object().at("retryable"), false);
    EXPECT_EQ(result.at("publication"), (object{{"state", "not-published"}, {"persisted", false}}));
    EXPECT_TRUE(result.at("created").as_array().empty()); EXPECT_EQ(typed_exit_status(Status::Cancelled), 3);
    h.send(request("status", "session.status"));
    EXPECT_TRUE(h.wait("result", "status").at("result").as_object().at("data").as_object().at("document").is_null());
    EXPECT_EQ(h.finish(), 0);
    EXPECT_EQ(std::count_if(h.events.begin(), h.events.end(), [](auto const &event) {
        return event.at("event") == "result" && event.at("id") == "file";
    }), 1);
}

TEST_F(VacardsCliProcess, NativeVersionAvailabilityIsRetryableOnSessionWire) {
    auto target = write("unavailable.svg", "old destination");
#ifdef _WIN32
    // Deny the native version read without relying on POSIX mode bits on Windows.
    // Release even if a fatal assertion returns before the save result arrives.
    struct ReadDenial {
        HANDLE handle;
        ~ReadDenial() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } denial{CreateFileW(std::filesystem::u8path(target).wstring().c_str(),
                        GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
    ASSERT_NE(denial.handle, INVALID_HANDLE_VALUE) << GetLastError();
#else
    std::filesystem::permissions(target, std::filesystem::perms::none);
#endif
    SessionOptions options; options.grants.write_roots = {dir.string()};
    Harness h(options); h.wait("hello");
    auto fresh = parse(find_command("file.new")->example).as_object(); fresh["id"] = "new";
    h.send(serialize(fresh) + "\n");
    auto created = h.wait("result", "new").at("result").as_object();
    ASSERT_EQ(created.at("status"), "changed") << serialize(created);
    auto save = parse(find_command("file.save")->example).as_object(); save["id"] = "unavailable";
    save["document"] = created.at("document_id"); save["if_revision"] = created.at("revision_after");
    save.at("params").as_object()["path"] = target;
    h.send(serialize(save) + "\n");
    auto result = h.wait("result", "unavailable").at("result").as_object();
#ifdef _WIN32
    ASSERT_TRUE(CloseHandle(denial.handle)) << GetLastError();
    denial.handle = INVALID_HANDLE_VALUE;
#else
    std::filesystem::permissions(target, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
    EXPECT_EQ(result.at("status"), "rejected") << serialize(result);
    EXPECT_EQ(result.at("error").as_object().at("code"), "publication-unavailable");
    EXPECT_EQ(result.at("error").as_object().at("retryable"), true);
    EXPECT_EQ(result.at("publication"), (object{{"state", "not-published"}, {"persisted", false}}));
    EXPECT_FALSE(validate_schema(result, result_schema_descriptor(*find_command("file.save"))));
    std::ifstream input(target); std::string bytes{std::istreambuf_iterator<char>(input), {}};
    EXPECT_EQ(bytes, "old destination");
    h.send(request("status", "session.status"));
    EXPECT_FALSE(h.wait("result", "status").at("result").as_object().at("data").as_object().at("writes_blocked").as_bool());
    EXPECT_EQ(h.finish(), 0);
}

TEST(VacardsCliSession, DevelopmentOverlayExecutesWithoutAcceptance) {
    Harness h; auto hello = h.wait("hello");
    EXPECT_EQ(hello.at("capabilities").as_object().at("m3_accepted"), false);
    EXPECT_EQ(hello.at("capabilities").as_object().at("m3_experimental"), true);
    auto fresh = parse(find_command("file.new")->example).as_object(); fresh["id"] = "new";
    h.send(serialize(fresh) + "\n");
    auto created = h.wait("result", "new").at("result").as_object();
    ASSERT_EQ(created.at("status"), "changed");
    h.send(request("state", "session.status"));
    auto state = h.wait("result", "state").at("result").as_object().at("data").as_object();
    object selection{{"schema", "va-studio.cli-request/1"}, {"id", "selection"},
        {"command", "selection.set"}, {"params", object{{"ids", array{}}}},
        {"document", created.at("document_id")}, {"if_revision", state.at("session_revision")}};
    h.send(serialize(selection) + "\n");
    auto result = h.wait("result", "selection").at("result").as_object();
    EXPECT_EQ(result.at("status"), "unchanged") << serialize(result);
    EXPECT_EQ(result.at("data").as_object().at("normalized-ids"), array{});
    EXPECT_EQ(result.at("revision_before"), result.at("revision_after"));
    selection["id"] = "stale"; selection["if_revision"] = 999;
    h.send(serialize(selection) + "\n");
    EXPECT_EQ(code(h.wait("result", "stale")), "stale-revision");
    EXPECT_EQ(h.finish(), 0);
}

namespace {
Request structured_request(std::string id, std::string command = "system.catalog") {
    auto parsed = parse_request(request(std::move(id), std::move(command)));
    return std::move(*parsed.request);
}
struct StructuredCapture {
    std::vector<EngineEvent> events;
    std::vector<std::thread::id> sink_threads;
    std::thread::id owner = std::this_thread::get_id();
    std::unique_ptr<StructuredSession> session;
    StructuredCapture(SessionOptions options = {}, EngineEventSink custom = {}) {
        auto sink = custom ? std::move(custom) : EngineEventSink([this](EngineEvent const &event) {
            events.push_back(event); sink_threads.push_back(std::this_thread::get_id()); return true;
        });
        auto created = make_structured_session(std::move(options), std::move(sink));
        if (created.engine) session = std::move(created.engine);
    }
    bool pump_until(std::function<bool()> const &done) {
        auto deadline = std::chrono::steady_clock::now() + 10s;
        do {
            session->pump();
            if (done()) return true;
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        session->pump();
        return done();
    }
    ~StructuredCapture() { if (session) session->close(); }
};
bool has_result(StructuredCapture const &capture, std::string_view id) {
    return std::any_of(capture.events.begin(), capture.events.end(), [&](auto const &event) {
        return event.kind == EngineEventKind::Result && event.request_id == id;
    });
}
std::string structured_code(Record const &record) { return record.reason; }
}

TEST(VacardsCliStructuredSession, FactoryValidatesSinkAndIsSilentUntilReturn) {
    auto invalid = make_structured_session({}, {});
    ASSERT_FALSE(invalid.engine); ASSERT_TRUE(invalid.error);
    EXPECT_EQ(invalid.error->code, "invalid-argument");
    EXPECT_EQ(invalid.error->message, "A structured session needs an event sink.");
    unsigned calls = 0;
    auto created = make_structured_session({}, [&](EngineEvent const &) { ++calls; return true; });
    ASSERT_TRUE(created.engine) << (created.error ? created.error->message : "");
    EXPECT_EQ(calls, 0u);
    created.engine->pump(); EXPECT_EQ(calls, 0u);
    created.engine->close();
    SessionOptions unavailable;
    unavailable.inspection_path = "/nonexistent/va-cli-structured-session.svg";
    auto startup = make_structured_session(std::move(unavailable), [](EngineEvent const &) { return true; });
    ASSERT_TRUE(startup.engine);
    EXPECT_TRUE(startup.engine->status().contains("startup_error"));
    startup.engine->close();
}

TEST(VacardsCliStructuredSession, AcceptedProgressResultOrderSequencesAndOwnerThreadDelivery) {
    StructuredCapture capture;
    ASSERT_TRUE(capture.session);
    auto submitted = capture.session->submit(structured_request("catalog"));
    ASSERT_TRUE(submitted.admitted); EXPECT_FALSE(submitted.refusal);
    ASSERT_EQ(capture.events.size(), 1u);
    EXPECT_EQ(capture.events[0].kind, EngineEventKind::Accepted);
    EXPECT_EQ(capture.events[0].sequence, 1u);
    ASSERT_TRUE(capture.pump_until([&] { return has_result(capture, "catalog"); }));
    ASSERT_EQ(capture.events.size(), 4u);
    EXPECT_EQ(capture.events[0].kind, EngineEventKind::Accepted);
    EXPECT_EQ(capture.events[1].kind, EngineEventKind::Progress);
    EXPECT_EQ(capture.events[1].sequence, 2u);
    EXPECT_EQ(capture.events[1].payload, (object{{"progress", 1}, {"phase", "preparation"}, {"message", "Preparing request"}}));
    EXPECT_EQ(capture.events[2].kind, EngineEventKind::Progress);
    EXPECT_EQ(capture.events[2].sequence, 3u);
    EXPECT_EQ(capture.events[2].payload.at("uninterruptible"), true);
    EXPECT_EQ(capture.events[3].kind, EngineEventKind::Result);
    EXPECT_EQ(capture.events[3].sequence, 4u);
    EXPECT_EQ(capture.events[3].payload.at("schema"), "va-studio.cli-result/1");
    EXPECT_EQ(capture.events[3].payload.at("id"), "catalog");
    EXPECT_EQ(capture.events[3].payload.at("status"), "ok");
    EXPECT_EQ(capture.events[3].payload.at("seq"), 1);
    EXPECT_FALSE(validate_schema(capture.events[3].payload, result_schema_descriptor(*find_command("system.catalog"))));
    EXPECT_EQ(std::count_if(capture.events.begin(), capture.events.end(), [](auto const &event) {
        return event.kind == EngineEventKind::Result;
    }), 1);
    ASSERT_EQ(capture.sink_threads.size(), capture.events.size());
    for (auto thread : capture.sink_threads) EXPECT_EQ(thread, capture.owner);
}

TEST(VacardsCliStructuredSession, RefusalsEmitNoEvents) {
    StructuredCapture duplicate;
    ASSERT_TRUE(duplicate.session);
    ASSERT_TRUE(duplicate.session->submit(structured_request("same")).admitted);
    ASSERT_TRUE(duplicate.pump_until([&] { return has_result(duplicate, "same"); }));
    auto before = duplicate.events.size();
    auto repeated = duplicate.session->submit(structured_request("same"));
    ASSERT_FALSE(repeated.admitted); ASSERT_TRUE(repeated.refusal);
    EXPECT_EQ(structured_code(*repeated.refusal), "duplicate-request-id");
    EXPECT_EQ(duplicate.events.size(), before);

    std::promise<void> started, resume;
    auto gate = resume.get_future().share();
    SessionOptions options;
    options.before_dispatch = [&](auto const &, auto const &cancelled) {
        started.set_value();
        while (!cancelled() && gate.wait_for(1ms) != std::future_status::ready) {}
    };
    StructuredCapture busy(std::move(options));
    ASSERT_TRUE(busy.session);
    ASSERT_TRUE(busy.session->submit(structured_request("held")).admitted);
    ASSERT_EQ(started.get_future().wait_for(10s), std::future_status::ready);
    before = busy.events.size();
    auto refused = busy.session->submit(structured_request("busy"));
    ASSERT_FALSE(refused.admitted); ASSERT_TRUE(refused.refusal);
    EXPECT_EQ(structured_code(*refused.refusal), "session-busy");
    EXPECT_EQ(busy.events.size(), before);
    resume.set_value();
    ASSERT_TRUE(busy.pump_until([&] { return has_result(busy, "held"); }));
}

TEST(VacardsCliStructuredSession, ImmediateStatusAndCancelWhileActive) {
    std::promise<void> started, resume;
    auto gate = resume.get_future().share();
    SessionOptions options;
    options.before_dispatch = [&](auto const &, auto const &cancelled) {
        started.set_value();
        while (!cancelled() && gate.wait_for(1ms) != std::future_status::ready) {}
    };
    StructuredCapture capture(std::move(options)); ASSERT_TRUE(capture.session);
    ASSERT_TRUE(capture.session->submit(structured_request("slow")).admitted);
    ASSERT_EQ(started.get_future().wait_for(10s), std::future_status::ready);
    auto status = capture.session->submit(structured_request("status", "session.status"));
    ASSERT_TRUE(status.admitted);
    ASSERT_EQ(capture.events.back().kind, EngineEventKind::Result);
    auto state = capture.events.back().payload.at("data").as_object();
    EXPECT_EQ(state.at("active_request"), "slow"); EXPECT_EQ(state.at("phase"), "preparation");
    auto cancelled = capture.session->cancel("slow");
    EXPECT_EQ(cancelled.data.at("found"), true);
    resume.set_value();
    ASSERT_TRUE(capture.pump_until([&] { return has_result(capture, "slow"); }));
    unsigned settled = 0;
    for (auto const &event : capture.events) if (event.kind == EngineEventKind::Result && event.request_id == "slow") {
        ++settled; EXPECT_EQ(event.payload.at("status"), "cancelled");
    }
    EXPECT_EQ(settled, 1u);
}

TEST(VacardsCliStructuredSession, CloseIsIdempotentJoinsAndDeliversNothingAfter) {
    std::promise<void> entered;
    std::atomic<bool> observed_cancel{false};
    SessionOptions options;
    options.before_dispatch = [&](auto const &, auto const &cancelled) {
        entered.set_value();
        while (!cancelled()) std::this_thread::sleep_for(1ms);
        observed_cancel = cancelled();
    };
    StructuredCapture capture(std::move(options)); ASSERT_TRUE(capture.session);
    ASSERT_TRUE(capture.session->submit(structured_request("closing")).admitted);
    capture.session->close(); capture.session->close();
    EXPECT_EQ(entered.get_future().wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(observed_cancel);
    auto count = capture.events.size(); capture.session->pump();
    EXPECT_EQ(capture.events.size(), count);
    auto denied = capture.session->submit(structured_request("after"));
    EXPECT_FALSE(denied.admitted); ASSERT_TRUE(denied.refusal);
    EXPECT_EQ(denied.refusal->reason, "session-required");
    EXPECT_EQ(capture.session->cancel("closing").reason, "session-required");
    EXPECT_EQ(capture.session->status(), (object{{"closed", true}}));
}

TEST(VacardsCliStructuredSession, SinkFailureStopsDeliveryAndClosesOnNextPump) {
    unsigned calls = 0;
    std::vector<std::thread::id> sink_threads;
    auto owner = std::this_thread::get_id();
    auto created = make_structured_session({}, [&](EngineEvent const &) {
        ++calls; sink_threads.push_back(std::this_thread::get_id()); return false;
    });
    ASSERT_TRUE(created.engine);
    ASSERT_TRUE(created.engine->submit(structured_request("failed-sink")).admitted);
    EXPECT_EQ(calls, 1u);
    created.engine->pump();
    EXPECT_EQ(created.engine->status(), (object{{"closed", true}}));
    created.engine->close();
    EXPECT_EQ(calls, 1u);
    ASSERT_EQ(sink_threads.size(), calls);
    EXPECT_EQ(sink_threads.front(), owner);
}

TEST(VacardsCliStructuredSession, NativeThreadOwnsDocumentWork) {
    std::mutex mutex;
    std::vector<std::thread::id> native_ids;
#ifdef __APPLE__
    std::vector<std::size_t> stack_sizes;
#endif
    SessionOptions options;
    options.document_path = (std::filesystem::path(__FILE__).parent_path().parent_path() /
                             "cli_tests/vacards-agent/fixtures/m3/explode.svg").string();
    options.grants.read_files = {options.document_path};
    options.before_dispatch = [&](auto const &, auto const &) {
        std::lock_guard lock(mutex); native_ids.push_back(std::this_thread::get_id());
#ifdef __APPLE__
        stack_sizes.push_back(pthread_get_stacksize_np(pthread_self()));
#endif
    };
    StructuredCapture capture(std::move(options)); ASSERT_TRUE(capture.session);
    for (auto id : {"first", "second"}) {
        ASSERT_TRUE(capture.session->submit(structured_request(id)).admitted);
        ASSERT_TRUE(capture.pump_until([&] { return has_result(capture, id); }));
    }
    ASSERT_EQ(native_ids.size(), 2u);
    EXPECT_NE(native_ids.front(), capture.owner);
    EXPECT_EQ(native_ids[0], native_ids[1]);
#ifdef __APPLE__
    ASSERT_EQ(stack_sizes.size(), 2u);
    for (auto size : stack_sizes) EXPECT_GE(size, 64u * 1024u * 1024u);
#endif
}

TEST(VacardsCliStructuredSession, ResultPayloadsMatchJsonlSession) {
    Harness jsonl;
    jsonl.send(request("catalog", "system.catalog"));
    jsonl.wait("result", "catalog");
    jsonl.send(request("status", "session.status") + request("catalog", "system.catalog"));
    EXPECT_EQ(jsonl.finish(), 0);
    // The duplicate uses the same ID; retain its terminal separately.
    object jsonl_duplicate;
    for (auto const &event : jsonl.events)
        if (event.at("event") == "result" && event.at("result").as_object().contains("error") &&
            event.at("result").as_object().at("error").as_object().at("code") == "duplicate-request-id")
            jsonl_duplicate = event.at("result").as_object();

    StructuredCapture structured; ASSERT_TRUE(structured.session);
    ASSERT_TRUE(structured.session->submit(structured_request("catalog")).admitted);
    ASSERT_TRUE(structured.pump_until([&] { return has_result(structured, "catalog"); }));
    ASSERT_TRUE(structured.session->submit(structured_request("status", "session.status")).admitted);
    auto refused = structured.session->submit(structured_request("catalog"));
    ASSERT_FALSE(refused.admitted); ASSERT_TRUE(refused.refusal);
    auto structured_duplicate = typed_result(*refused.refusal, "catalog", 3);
    EXPECT_EQ(structured_duplicate, jsonl_duplicate);
    unsigned compared = 0;
    for (auto const &event : structured.events) if (event.kind == EngineEventKind::Result) {
        auto found = std::find_if(jsonl.events.begin(), jsonl.events.end(), [&](auto const &wire) {
            return wire.at("event") == "result" && wire.at("id").as_string() == event.request_id &&
                   !(wire.at("result").as_object().contains("error") &&
                     wire.at("result").as_object().at("error").as_object().at("code") == "duplicate-request-id");
        });
        ASSERT_NE(found, jsonl.events.end());
        EXPECT_EQ(event.payload, found->at("result").as_object()); ++compared;
    }
    EXPECT_EQ(compared, 2u);
    EXPECT_EQ(structured.events.size(), 6u); // Accepted/Progress/Progress/Result, then Accepted/Result; refusal is silent.
    // No per-instance identifiers need normalization: fixed request IDs and no document are used.
}
