// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstring>
#include <set>
#include <map>
#include <stdexcept>
#include "actions/vacards-cli-production.h"
#include "actions/vacards-cli-mcp.h"
using namespace Inkscape::VACardsCli;
using namespace boost::json;
namespace {
struct FakeSession : StructuredSession {
    EngineEventSink sink;
    std::vector<Request> requests;
    std::vector<std::string> cancellations;
    bool deferred = false, refuse = false, cancel_settles = false;
    int closes = 0;
    Status outcome = Status::Ok;
    Record record(Request const &r, Status status) {
        Record result; result.action = r.command; result.status = status;
        result.reason = status == Status::Rejected ? "document-busy" : "success";
        result.document_id = r.document.value_or("");
        return result;
    }
    Submission submit(Request const &r) override {
        requests.push_back(r);
        if (refuse) return {false, record(r, Status::Rejected)};
        sink({EngineEventKind::Accepted, r.id, 1, {}});
        if (!deferred) complete(requests.size() - 1, outcome);
        return {true, {}};
    }
    bool complete(std::size_t index, Status status, std::uint64_t seq = 3) {
        auto const &r = requests.at(index);
        auto result = record(r, status);
        if (status == Status::Changed && !cancellations.empty()) result.warnings.push_back("cancel-too-late");
        return sink({EngineEventKind::Result, r.id, seq, typed_result(result, r.id)});
    }
    Record cancel(std::string const &id) override {
        cancellations.push_back(id);
        if (cancel_settles) {
            auto i = std::find_if(requests.begin(), requests.end(), [&](auto const &r) { return r.id == id; });
            complete(i - requests.begin(), Status::Cancelled);
        }
        Record ack; ack.status = Status::Ok; return ack;
    }
    object status() const override { return {{"active", requests.size()}}; }
    void close() noexcept override { ++closes; }
};
class McpTest : public ::testing::Test {
public:
    // Destruction order: adapter closes while fake is alive.
    FakeSession fake;
    std::vector<object> output;
    bool writable = true;
    McpProtocol protocol{[&](std::string_view bytes) {
        EXPECT_FALSE(bytes.empty()); EXPECT_EQ(bytes.back(), '\n');
        EXPECT_EQ(std::count(bytes.begin(), bytes.end(), '\n'), 1);
        if (!writable) return false;
        output.push_back(parse(bytes).as_object()); return true;
    }};
    void SetUp() override {
        fake.sink = [&](auto const &e) { return protocol.event(e); };
        protocol.bind(fake);
    }
    bool send(value id, std::string_view method, object params = {}) {
        return protocol.receive(serialize(object{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}));
    }
    bool note(std::string_view method, object params = {}) {
        return protocol.receive(serialize(object{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}));
    }
    void initialize(std::string_view offered = mcp_protocol_version) {
        ASSERT_TRUE(send("init", "initialize", {{"protocolVersion", offered}, {"capabilities", object{}},
            {"clientInfo", object{{"name", "fake-client"}, {"version", "1"}}}}));
        ASSERT_TRUE(note("notifications/initialized"));
        output.clear();
    }
    object args(std::string_view command = "system.catalog") {
        auto catalog = production_catalog();
        for (auto const &row : catalog.at("commands").as_array()) {
            auto const &o = row.as_object();
            if (std::string(o.at("id").as_string()) != command) continue;
            auto a = o.at("example").as_object();
            for (auto f : {"schema", "id", "command"}) a.erase(f);
            return a;
        }
        throw std::runtime_error("Missing catalog example");
    }
    bool call(value id, std::string_view name = "va_system_catalog", object arguments = {{"params", object{}}}, value token = nullptr) {
        object p{{"name", name}, {"arguments", arguments}};
        if (!token.is_null()) p["_meta"] = object{{"progressToken", token}};
        return send(id, "tools/call", p);
    }
    int code() { return output.back().at("error").as_object().at("code").to_number<int>(); }
};
TEST_F(McpTest, NegotiatesOnlyPinnedVersion) {
    ASSERT_TRUE(send(1, "initialize", {{"protocolVersion", "2099-01-01"}, {"capabilities", object{}},
        {"clientInfo", object{{"name", "client"}, {"version", "1"}}}}));
    auto const &r = output.back().at("result").as_object();
    EXPECT_EQ(r.at("protocolVersion"), "2025-06-18");
    EXPECT_EQ(r.at("capabilities").as_object().size(), 1);
    EXPECT_FALSE(r.at("capabilities").as_object().at("tools").as_object().at("listChanged").as_bool());
}
TEST_F(McpTest, RequiresInitializedNotification) {
    ASSERT_TRUE(send(1, "tools/list")); EXPECT_EQ(code(), -32000);
    ASSERT_TRUE(send(2, "initialize", {{"protocolVersion", mcp_protocol_version}, {"capabilities", object{}},
        {"clientInfo", object{{"name", "x"}, {"version", "1"}}}}));
    ASSERT_TRUE(send(3, "tools/list")); EXPECT_EQ(code(), -32000);
    ASSERT_TRUE(note("notifications/initialized"));
    ASSERT_TRUE(send(4, "tools/list")); EXPECT_TRUE(output.back().at("result").as_object().contains("tools"));
}
TEST_F(McpTest, RejectsInvalidAndRepeatedInitialize) {
    ASSERT_TRUE(send(1, "initialize")); EXPECT_EQ(code(), -32602);
    initialize(); ASSERT_TRUE(send(2, "initialize")); EXPECT_EQ(code(), -32600);
}
TEST_F(McpTest, PingAndUnknownNotifications) {
    ASSERT_TRUE(send(1, "ping")); EXPECT_TRUE(output.back().at("result").as_object().empty());
    auto n = output.size(); EXPECT_TRUE(note("anything")); EXPECT_EQ(output.size(), n);
}
TEST_F(McpTest, PaginatedCatalogMatchesProductionSchemas) {
    initialize(); std::map<std::string, object> listed;
    object params; int id = 0;
    do {
        ASSERT_TRUE(send(++id, "tools/list", params));
        auto const &r = output.back().at("result").as_object();
        ASSERT_LE(r.at("tools").as_array().size(), 32);
        for (auto const &v : r.at("tools").as_array()) {
            auto const &t = v.as_object(); EXPECT_TRUE(listed.emplace(std::string(t.at("name").as_string()), t).second);
        }
        if (!r.contains("nextCursor")) break;
        params = {{"cursor", r.at("nextCursor")}};
    } while (id < 100);
    auto catalog = production_catalog();
    EXPECT_EQ(listed.size(), catalog.at("commands").as_array().size());
    std::size_t overlay = 0;
    for (auto const &v : catalog.at("commands").as_array()) {
        auto const &row = v.as_object(); auto command = std::string(row.at("id").as_string());
        if (find_production_command(command)) ++overlay;
        auto name = "va_" + command; std::replace(name.begin(), name.end(), '.', '_'); std::replace(name.begin(), name.end(), '-', '_');
        ASSERT_TRUE(listed.contains(name)); auto const &t = listed.at(name);
        EXPECT_EQ(t.at("outputSchema"), row.at("result_schema"));
        auto const &input = t.at("inputSchema").as_object();
        EXPECT_EQ(input.at("properties").as_object().at("params"), row.at("request_schema").as_object().at("properties").as_object().at("params"));
        EXPECT_FALSE(input.at("properties").as_object().contains("id"));
        EXPECT_EQ(t.at("annotations").as_object().at("readOnlyHint"), row.at("effects") == "read-only");
    }
    EXPECT_EQ(overlay, 31);
}
TEST_F(McpTest, RejectsInvalidCursorAndUnknownTool) {
    initialize(); ASSERT_TRUE(send(1, "tools/list", {{"cursor", "stale:32"}})); EXPECT_EQ(code(), -32602);
    ASSERT_TRUE(call(2, "no_tool")); EXPECT_EQ(code(), -32602); EXPECT_TRUE(fake.requests.empty());
}
TEST_F(McpTest, SynchronousCallHasMatchingStructuredAndTextContent) {
    initialize(); ASSERT_TRUE(call(1)); ASSERT_EQ(fake.requests.size(), 1); ASSERT_EQ(output.size(), 1);
    auto const &r = output.back().at("result").as_object();
    EXPECT_FALSE(r.at("isError").as_bool());
    EXPECT_EQ(parse(r.at("content").as_array()[0].as_object().at("text").as_string()), r.at("structuredContent"));
    EXPECT_EQ(fake.requests[0].command, "system.catalog");
    EXPECT_EQ(output.back().at("id"), 1);
}
TEST_F(McpTest, DomainStatusesMapToIsErrorWithoutChangingStatus) {
    initialize(); int id = 0;
    for (auto status : {Status::Ok, Status::Changed, Status::Unchanged, Status::Rejected,
                        Status::Cancelled, Status::Failed, Status::Uncertain, Status::Skipped}) {
        fake.outcome = status; ASSERT_TRUE(call(++id));
        auto const &r = output.back().at("result").as_object();
        EXPECT_EQ(r.at("isError").as_bool(), status != Status::Ok && status != Status::Changed && status != Status::Unchanged);
        EXPECT_EQ(std::string(r.at("structuredContent").as_object().at("status").as_string()), std::string(status_name(status)));
    }
}
TEST_F(McpTest, MapsM3GuardsAndDryRunWithoutInventingSessionSemantics) {
    initialize(); auto a = args("selection.set"); a["document"] = "d-test"; a["if_revision"] = 42; a["dry_run"] = true;
    ASSERT_TRUE(call("edit", "va_selection_set", a)); ASSERT_EQ(fake.requests.size(), 1);
    EXPECT_EQ(fake.requests[0].document, "d-test"); EXPECT_EQ(fake.requests[0].if_revision, 42);
    EXPECT_TRUE(fake.requests[0].dry_run); EXPECT_EQ(fake.requests[0].params, a.at("params"));
}
TEST_F(McpTest, EveryProductionExampleReachesFakeSession) {
    initialize(); auto catalog = production_catalog(); int id = 0;
    for (auto const &v : catalog.at("commands").as_array()) {
        auto const &row = v.as_object(); auto command = std::string(row.at("id").as_string());
        auto name = "va_" + command; std::replace(name.begin(), name.end(), '.', '_'); std::replace(name.begin(), name.end(), '-', '_');
        ASSERT_TRUE(call(++id, name, args(command))) << command;
        ASSERT_EQ(fake.requests.size(), static_cast<std::size_t>(id)) << command;
        EXPECT_EQ(fake.requests.back().command, command);
    }
}
TEST_F(McpTest, SchemaFailuresAreDomainResults) {
    initialize(); ASSERT_TRUE(call(1, "va_selection_set", {{"params", object{}}}));
    EXPECT_TRUE(fake.requests.empty()); auto const &r = output.back().at("result").as_object();
    EXPECT_TRUE(r.at("isError").as_bool()); EXPECT_TRUE(r.at("structuredContent").as_object().contains("error"));
}
TEST_F(McpTest, ReservedRoutingArgumentsAreProtocolErrors) {
    initialize(); ASSERT_TRUE(call(1, "va_system_catalog", {{"command", "file.save"}, {"params", object{}}}));
    EXPECT_EQ(code(), -32602); EXPECT_TRUE(fake.requests.empty());
}
TEST_F(McpTest, AdmissionRefusalIsOneDomainTerminal) {
    initialize(); fake.refuse = true; ASSERT_TRUE(call(1)); ASSERT_EQ(output.size(), 1);
    EXPECT_TRUE(output.back().at("result").as_object().at("isError").as_bool());
    EXPECT_EQ(output.back().at("result").as_object().at("structuredContent").as_object().at("status"), "rejected");
}
TEST_F(McpTest, DeferredCallsPreserveNumericAndStringIdentity) {
    initialize(); fake.deferred = true; ASSERT_TRUE(call(7)); ASSERT_TRUE(call("7"));
    EXPECT_NE(fake.requests[0].id, fake.requests[1].id); EXPECT_TRUE(output.empty());
    ASSERT_TRUE(fake.complete(1, Status::Ok)); ASSERT_TRUE(fake.complete(0, Status::Ok));
    ASSERT_EQ(output.size(), 2); EXPECT_EQ(output[0].at("id"), "7"); EXPECT_EQ(output[1].at("id"), 7);
}
TEST_F(McpTest, Uint64AndUnicodeIdsRemainLossless) {
    initialize(); ASSERT_TRUE(call(18446744073709551615ULL));
    EXPECT_EQ(output.back().at("id").as_uint64(), 18446744073709551615ULL);
    ASSERT_TRUE(call("árbol\\雪\"")); EXPECT_EQ(output.back().at("id"), "árbol\\雪\"");
}
TEST_F(McpTest, ProgressUsesCallerTokenAndHidesInternalEvents) {
    initialize(); fake.deferred = true; ASSERT_TRUE(call(1, "va_system_catalog", {{"params", object{}}}, "phase-token"));
    EXPECT_TRUE(output.empty()); auto const &id = fake.requests[0].id;
    EXPECT_TRUE(protocol.event({EngineEventKind::Status, id, 2, {{"secret", "internal"}}}));
    EXPECT_TRUE(protocol.event({EngineEventKind::Progress, id, 3, {{"progress", 10}, {"total", 20}, {"message", "native phase"}}}));
    ASSERT_EQ(output.size(), 1); EXPECT_EQ(output[0].at("method"), "notifications/progress");
    EXPECT_EQ(output[0].at("params").as_object().at("progressToken"), "phase-token");
    ASSERT_TRUE(fake.complete(0, Status::Ok, 4)); EXPECT_EQ(output.size(), 2);
}
TEST_F(McpTest, IntegralDecimalIdsShareNumericCancellationIdentity) {
    initialize(); fake.deferred = true; ASSERT_TRUE(call(1.0));
    ASSERT_TRUE(note("notifications/cancelled", {{"requestId", 1}})); EXPECT_EQ(fake.cancellations.size(), 1);
    ASSERT_TRUE(fake.complete(0, Status::Ok)); EXPECT_EQ(output.back().at("id").as_double(), 1.0);
    ASSERT_TRUE(call(2.5)); EXPECT_EQ(code(), -32600); EXPECT_EQ(fake.requests.size(), 1);
}
TEST_F(McpTest, ProgressTokensAreIntegralAndUniqueWhileActive) {
    initialize(); fake.deferred = true;
    ASSERT_TRUE(call(1, "va_system_catalog", {{"params", object{}}}, 7));
    ASSERT_TRUE(call(2, "va_system_catalog", {{"params", object{}}}, 7.0)); EXPECT_EQ(code(), -32602);
    ASSERT_TRUE(call(3, "va_system_catalog", {{"params", object{}}}, 2.5)); EXPECT_EQ(code(), -32602);
    ASSERT_EQ(fake.requests.size(), 1);
    ASSERT_TRUE(fake.complete(0, Status::Ok));
    ASSERT_TRUE(call(4, "va_system_catalog", {{"params", object{}}}, 7)); EXPECT_EQ(fake.requests.size(), 2);
}
TEST_F(McpTest, RegressingPhaseProgressIsOmittedWithoutFabrication) {
    initialize(); fake.deferred = true; ASSERT_TRUE(call(1, "va_system_catalog", {{"params", object{}}}, "token"));
    auto const &id = fake.requests[0].id;
    for (auto [seq, progress] : {std::pair{2, 10.0}, {3, 10.0}, {4, 2.0}, {5, 10.5}})
        ASSERT_TRUE(protocol.event({EngineEventKind::Progress, id, static_cast<std::uint64_t>(seq), {{"progress", progress}}}));
    ASSERT_EQ(output.size(), 2); EXPECT_EQ(output[0].at("params").as_object().at("progress"), 10.0);
    EXPECT_EQ(output[1].at("params").as_object().at("progress"), 10.5);
    ASSERT_TRUE(fake.complete(0, Status::Ok, 6)); EXPECT_EQ(output.size(), 3);
}
TEST_F(McpTest, ProgressWithoutTokenIsSilent) {
    initialize(); fake.deferred = true; ASSERT_TRUE(call(1));
    EXPECT_TRUE(protocol.event({EngineEventKind::Progress, fake.requests[0].id, 2, {{"progress", 2}}})); EXPECT_TRUE(output.empty());
}
TEST_F(McpTest, CancelUnknownCompletedAndInitializeAreSilent) {
    initialize(); ASSERT_TRUE(call(1)); auto n = output.size();
    ASSERT_TRUE(note("notifications/cancelled", {{"requestId", 1}}));
    ASSERT_TRUE(note("notifications/cancelled", {{"requestId", "init"}}));
    ASSERT_TRUE(note("notifications/cancelled", {{"requestId", "missing"}}));
    EXPECT_TRUE(fake.cancellations.empty()); EXPECT_EQ(output.size(), n);
}
TEST_F(McpTest, CancellationWaitsForRealTerminalAndPreservesTooLateCommit) {
    initialize(); fake.deferred = true; ASSERT_TRUE(call(1));
    ASSERT_TRUE(note("notifications/cancelled", {{"requestId", 1}}));
    ASSERT_EQ(fake.cancellations.size(), 1); EXPECT_EQ(fake.cancellations[0], fake.requests[0].id); EXPECT_TRUE(output.empty());
    ASSERT_TRUE(fake.complete(0, Status::Changed));
    auto const &r = output.back().at("result").as_object(); EXPECT_FALSE(r.at("isError").as_bool());
    EXPECT_EQ(r.at("structuredContent").as_object().at("status"), "changed");
    EXPECT_EQ(r.at("structuredContent").as_object().at("coded_warnings").as_array()[0].as_object().at("code"), "cancel-too-late");
}
TEST_F(McpTest, CancelCanSynchronouslySettle) {
    initialize(); fake.deferred = true; fake.cancel_settles = true; ASSERT_TRUE(call(1));
    ASSERT_TRUE(note("notifications/cancelled", {{"requestId", 1}})); ASSERT_EQ(output.size(), 1);
    EXPECT_EQ(output[0].at("result").as_object().at("structuredContent").as_object().at("status"), "cancelled");
}
TEST_F(McpTest, DuplicateTerminalAndStaleProgressNeverRepublish) {
    initialize(); fake.deferred = true; ASSERT_TRUE(call(1)); ASSERT_TRUE(fake.complete(0, Status::Ok));
    ASSERT_TRUE(fake.complete(0, Status::Changed, 4));
    EXPECT_TRUE(protocol.event({EngineEventKind::Progress, fake.requests[0].id, 5, {{"progress", 1}}})); EXPECT_EQ(output.size(), 1);
}
TEST_F(McpTest, ActiveDuplicateDisconnectsWithoutSecondTerminal) {
    initialize(); fake.deferred = true; ASSERT_TRUE(call(1)); EXPECT_FALSE(call(1)); EXPECT_TRUE(protocol.failed());
    EXPECT_EQ(fake.requests.size(), 1); EXPECT_TRUE(output.empty());
    protocol.close(); EXPECT_EQ(fake.closes, 1); EXPECT_FALSE(fake.complete(0, Status::Changed));
}
TEST_F(McpTest, CompletedDuplicateIsRejected) {
    initialize(); ASSERT_TRUE(call(1)); ASSERT_TRUE(call(1)); EXPECT_EQ(code(), -32600); EXPECT_EQ(fake.requests.size(), 1);
}
TEST_F(McpTest, UnknownMethodAndMalformedProtocol) {
    initialize(); ASSERT_TRUE(send(1, "shutdown")); EXPECT_EQ(code(), -32601);
    for (auto line : {"{", "[]", "null", "{\"jsonrpc\":\"1.0\",\"id\":2,\"method\":\"ping\"}",
                      "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}",
                      "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\",\"params\":[]}"}) {
        ASSERT_TRUE(protocol.receive(line)); EXPECT_TRUE(output.back().contains("error"));
    }
    EXPECT_EQ(fake.requests.size(), 0);
}
TEST_F(McpTest, InvalidUtf8DoesNotReachSession) {
    initialize(); std::string line = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\""; line += char(0xff); line += "\"}";
    ASSERT_TRUE(protocol.receive(line)); EXPECT_EQ(code(), -32700); EXPECT_TRUE(fake.requests.empty());
}
TEST_F(McpTest, OversizedInputHasBoundedProtocolError) {
    initialize(); ASSERT_TRUE(protocol.receive(std::string(request_limit + 1, 'x'))); EXPECT_EQ(code(), -32600); EXPECT_TRUE(fake.requests.empty());
}
TEST_F(McpTest, OversizedTerminalDoesNotTruncateOrReplayMutation) {
    initialize(); fake.deferred = true; ASSERT_TRUE(call(1));
    auto payload = typed_result(fake.record(fake.requests[0], Status::Changed), fake.requests[0].id);
    payload["data"] = object{{"large", std::string(response_limit, 'x')}};
    ASSERT_TRUE(protocol.event({EngineEventKind::Result, fake.requests[0].id, 3, payload}));
    ASSERT_EQ(output.size(), 1); EXPECT_EQ(code(), -32002); EXPECT_LT(serialize(output[0]).size(), 512);
    ASSERT_TRUE(fake.complete(0, Status::Changed, 4)); EXPECT_EQ(output.size(), 1);
}
TEST_F(McpTest, OutputFailureRetiresDeliveryAndCloseIsIdempotent) {
    initialize(); fake.deferred = true; ASSERT_TRUE(call(1)); writable = false;
    EXPECT_FALSE(fake.complete(0, Status::Changed)); EXPECT_TRUE(protocol.failed());
    protocol.close(); protocol.close(); EXPECT_EQ(fake.closes, 1);
    EXPECT_FALSE(fake.complete(0, Status::Changed, 4)); EXPECT_FALSE(send(2, "ping"));
}
TEST_F(McpTest, ActiveAdmissionIsBounded) {
    initialize(); fake.deferred = true;
    for (int id = 0; id < 128; ++id) ASSERT_TRUE(call(id));
    ASSERT_TRUE(call(128)); EXPECT_EQ(code(), -32001); EXPECT_EQ(fake.requests.size(), 128);
}
TEST_F(McpTest, CloseBeforePublicationSuppressesLaterResults) {
    initialize(); fake.deferred = true; ASSERT_TRUE(call(1)); protocol.close();
    EXPECT_EQ(fake.closes, 1); EXPECT_FALSE(fake.complete(0, Status::Changed)); EXPECT_TRUE(output.empty());
}
TEST(McpStdio, ChunkedCrLfEofAndRetryPump) {
    std::string input = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\r\n";
    std::size_t offset = 0; int reads = 0, pumps = 0, closes = 0; std::vector<object> output;
    struct ClosingFake : FakeSession { int &count; explicit ClosingFake(int &c) : count(c) {} void close() noexcept override { ++count; } };
    auto factory = [&](SessionOptions, EngineEventSink sink) -> StructuredSessionResult {
        auto f = std::make_unique<ClosingFake>(closes); f->sink = sink; return {std::move(f), {}};
    };
    int exit = run_mcp_stdio_with_factory([&](char *p, std::size_t) {
        if (++reads == 1) return -2;
        if (offset == input.size()) return 0;
        *p = input[offset++]; return 1;
    }, [&](std::string_view line) { output.push_back(parse(line).as_object()); return true; }, {}, factory, [&] { ++pumps; });
    EXPECT_EQ(exit, 0); EXPECT_EQ(closes, 1); EXPECT_GE(pumps, 3); ASSERT_EQ(output.size(), 1); EXPECT_EQ(output[0].at("id"), 1);
}
TEST(McpStdio, InputOversizeDrainsAndRecoversAtNextLine) {
    std::string input(request_limit + 2, 'x'); input += "\n{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n";
    std::size_t offset = 0; std::vector<object> output;
    auto factory = [](SessionOptions, EngineEventSink sink) -> StructuredSessionResult {
        auto f = std::make_unique<FakeSession>(); f->sink = sink; return {std::move(f), {}};
    };
    EXPECT_EQ(run_mcp_stdio_with_factory([&](char *p, std::size_t n) { auto size = std::min(n, input.size() - offset); std::memcpy(p, input.data() + offset, size); offset += size; return static_cast<int>(size); },
        [&](std::string_view line) { output.push_back(parse(line).as_object()); return true; }, {}, factory), 0);
    ASSERT_EQ(output.size(), 2); EXPECT_EQ(output[0].at("error").as_object().at("code"), -32600); EXPECT_EQ(output[1].at("id"), 1);
}
TEST(McpStdio, FactoryUnavailableAndReadFailureHaveDistinctExits) {
    bool touched = false;
    auto unavailable = [](SessionOptions, EngineEventSink) -> StructuredSessionResult { return {{}, ParseError{"slice-unavailable", {}, "fake unavailable"}}; };
    EXPECT_EQ(run_mcp_stdio_with_factory([&](char *, std::size_t) { touched = true; return 0; }, [&](std::string_view) { touched = true; return true; }, {}, unavailable), 2);
    EXPECT_FALSE(touched);
    auto factory = [](SessionOptions, EngineEventSink sink) -> StructuredSessionResult { auto f = std::make_unique<FakeSession>(); f->sink = sink; return {std::move(f), {}}; };
    EXPECT_EQ(run_mcp_stdio_with_factory([](char *, std::size_t) { return -1; }, [](std::string_view) { return true; }, {}, factory), 1);
}
TEST(McpStdio, OwnerPumpDeliversDeferredResultDuringIdleInput) {
    std::string input = R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"fake","version":"1"}}})";
    input += "\n";
    input += R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    input += "\n";
    input += R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"va_system_catalog","arguments":{"params":{}}}})";
    input += "\n";
    std::size_t offset = 0; FakeSession *fake = nullptr; bool idle = false, completed = false;
    std::vector<object> output;
    auto factory = [&](SessionOptions, EngineEventSink sink) -> StructuredSessionResult {
        auto f = std::make_unique<FakeSession>(); f->deferred = true; f->sink = sink; fake = f.get(); return {std::move(f), {}};
    };
    EXPECT_EQ(run_mcp_stdio_with_factory([&](char *p, std::size_t n) {
        if (offset < input.size()) { auto count = std::min(n, input.size() - offset); std::memcpy(p, input.data() + offset, count); offset += count; return static_cast<int>(count); }
        if (!idle) { idle = true; return -2; }
        return 0;
    }, [&](std::string_view line) { output.push_back(parse(line).as_object()); return true; }, {}, factory, [&] {
        if (idle && !completed && !fake->requests.empty()) { EXPECT_TRUE(fake->complete(0, Status::Ok)); completed = true; }
    }), 0);
    EXPECT_TRUE(completed); ASSERT_EQ(output.size(), 2); EXPECT_EQ(output.back().at("id"), 9);
}
TEST(McpStdio, ContinuouslyAvailableOversizedInputYieldsToOwnerPump) {
    std::size_t remaining = request_limit + 4096; int pumps = 0, replies = 0;
    auto factory = [](SessionOptions, EngineEventSink sink) -> StructuredSessionResult {
        auto f = std::make_unique<FakeSession>(); f->sink = sink; return {std::move(f), {}};
    };
    EXPECT_EQ(run_mcp_stdio_with_factory([&](char *p, std::size_t n) {
        auto count = std::min(n, remaining); std::memset(p, 'x', count); remaining -= count; return static_cast<int>(count);
    }, [&](std::string_view line) { EXPECT_EQ(parse(line).as_object().at("error").as_object().at("code"), -32600); ++replies; return true; }, {}, factory, [&] { ++pumps; }), 0);
    EXPECT_GE(pumps, 17); EXPECT_EQ(replies, 1);
}
TEST(McpStdio, BrokenOutputClosesWithoutRetry) {
    std::string input = R"({"jsonrpc":"2.0","id":1,"method":"ping"})"; input += "\n";
    bool read = false; int writes = 0, closes = 0;
    struct ClosingFake : FakeSession { int &count; explicit ClosingFake(int &c) : count(c) {} void close() noexcept override { ++count; } };
    auto factory = [&](SessionOptions, EngineEventSink sink) -> StructuredSessionResult { auto f = std::make_unique<ClosingFake>(closes); f->sink = sink; return {std::move(f), {}}; };
    EXPECT_EQ(run_mcp_stdio_with_factory([&](char *p, std::size_t) { if (read) return 0; read = true; std::memcpy(p, input.data(), input.size()); return static_cast<int>(input.size()); },
        [&](std::string_view) { ++writes; return false; }, {}, factory), 1);
    EXPECT_EQ(writes, 1); EXPECT_EQ(closes, 1);
}
}
