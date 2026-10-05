// SPDX-License-Identifier: GPL-2.0-or-later
#include "vacards-cli-mcp.h"
#include "vacards-cli-production.h"
#include <boost/json.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace Inkscape::VACardsCli {
namespace {
using namespace boost::json;
constexpr std::size_t page_size = 32, active_limit = 128, identity_limit = 65536;
bool valid_id(value const &v) {
    if (v.is_string() || v.is_int64() || v.is_uint64()) return true;
    // JSON integer syntax can also use a decimal/exponent (e.g. 1.0).
    // Accept only the exact-integer double range; never correlate rounded IDs.
    return v.is_double() && std::isfinite(v.as_double()) &&
           std::trunc(v.as_double()) == v.as_double() && std::abs(v.as_double()) <= 9007199254740991.0;
}
std::string id_key(value const &v) {
    return v.is_double() ? serialize(value(static_cast<std::int64_t>(v.as_double()))) : serialize(v);
} // Equivalent numeric integers share identity; strings remain distinct.
bool domain_error(object const &o) {
    auto s = o.if_contains("status");
    return !s || (*s != "ok" && *s != "changed" && *s != "unchanged");
}
object tool_result(object const &payload) {
    return {{"content", array{object{{"type", "text"}, {"text", serialize(payload)}}}},
            {"structuredContent", payload}, {"isError", domain_error(payload)}};
}
struct Tool { std::string command; object descriptor; };
}
struct McpProtocol::Impl {
    LineWriter writer;
    StructuredSession *session = nullptr;
    enum class Phase { New, Initializing, Ready, Closed } phase = Phase::New;
    bool broken = false;
    std::uint64_t next_id = 0;
    std::size_t identity_bytes = 0;
    std::map<std::string, Tool> tools;
    std::string catalog_identity;
    struct Pending { value rpc_id, progress_token; std::uint64_t sequence = 0; bool seen_event = false; std::optional<double> last_progress = std::nullopt; };
    std::map<std::string, Pending> pending;
    std::map<std::string, std::string> rpc_to_engine;
    std::set<std::string> used_ids;

    explicit Impl(LineWriter w) : writer(std::move(w)) {
        auto catalog = production_catalog();
        catalog_identity = std::string(catalog.at("hash").as_string());
        for (auto const &v : catalog.at("commands").as_array()) {
            auto const &row = v.as_object();
            if (auto available = row.if_contains("available"); available && !available->as_bool()) continue;
            auto command = std::string(row.at("id").as_string());
            // Legacy action/sink endpoints are absent from the typed agent catalog.
            auto name = "va_" + command;
            std::replace(name.begin(), name.end(), '.', '_');
            std::replace(name.begin(), name.end(), '-', '_');
            auto input = row.at("request_schema").as_object();
            auto &props = input.at("properties").as_object();
            for (auto field : {"schema", "id", "command"}) props.erase(field);
            auto &required = input.at("required").as_array();
            required.erase(std::remove_if(required.begin(), required.end(), [](value const &x) {
                return x == "schema" || x == "id" || x == "command";
            }), required.end());
            auto effects = row.at("effects").as_string();
            bool readonly = effects == "read-only";
            object descriptor{{"name", name}, {"description", row.at("summary")},
                {"inputSchema", input}, {"outputSchema", row.at("result_schema")},
                {"annotations", object{{"readOnlyHint", readonly}, {"destructiveHint", !readonly}}}};
            if (!tools.emplace(name, Tool{command, std::move(descriptor)}).second)
                throw std::runtime_error("MCP tool name collision");
        }
    }
    bool send(object message) {
        if (broken || phase == Phase::Closed) return false;
        auto bytes = serialize(message);
        if (bytes.size() + 1 > response_limit) {
            value id = nullptr;
            if (auto p = message.if_contains("id")) id = *p;
            if (!message.contains("id")) return true; // Drop oversized progress only.
            bytes = serialize(object{{"jsonrpc", "2.0"}, {"id", id},
                {"error", object{{"code", -32002}, {"message", "MCP response exceeds output limit; inspect session state before retrying."}}}});
        }
        bytes += '\n';
        try { if (writer(bytes)) return true; } catch (...) {}
        broken = true;
        return false;
    }
    bool error(value id, int code, std::string_view message) {
        return send({{"jsonrpc", "2.0"}, {"id", id}, {"error", object{{"code", code}, {"message", message}}}});
    }
    bool result(value id, object payload) {
        return send({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(payload)}});
    }
    bool settle(std::string const &id, object const &payload) {
        auto i = pending.find(id);
        if (i == pending.end()) return !broken;
        auto rpc = i->second.rpc_id;
        rpc_to_engine.erase(id_key(rpc));
        pending.erase(i); // Retire before invoking external sink; no second terminal.
        return result(rpc, tool_result(payload));
    }
    bool receive(std::string_view line) {
        if (broken || phase == Phase::Closed || !session) return false;
        if (line.size() > request_limit) return error(nullptr, -32600, "MCP request exceeds input limit");
        boost::system::error_code ec;
        auto v = parse(line, ec);
        if (ec) return error(nullptr, -32700, "Parse error");
        if (!v.is_object()) return error(nullptr, -32600, "Invalid Request; batches are unsupported");
        auto const &o = v.as_object();
        auto id = o.if_contains("id");
        auto method = o.if_contains("method");
        auto version = o.if_contains("jsonrpc");
        auto pv = o.if_contains("params");
        if (!version || *version != "2.0" || !method || !method->is_string() ||
            (id && !valid_id(*id)) || (pv && !pv->is_object()))
            return error(id && valid_id(*id) ? *id : value(nullptr), -32600, "Invalid Request");
        object empty;
        auto const &p = pv ? pv->as_object() : empty;
        auto m = method->as_string();
        if (!id) {
            if (m == "notifications/initialized" && phase == Phase::Initializing) phase = Phase::Ready;
            if (m == "notifications/cancelled" && phase == Phase::Ready) {
                auto target = p.if_contains("requestId");
                if (target && valid_id(*target)) {
                    auto i = rpc_to_engine.find(id_key(*target));
                    if (i != rpc_to_engine.end()) {
                        auto engine_id = i->second; // cancel may synchronously settle and erase map.
                        session->cancel(engine_id); // Acknowledgement is not a terminal outcome.
                    }
                }
            }
            return !broken; // Never answer notifications, including unknown methods.
        }
        if (used_ids.contains(id_key(*id))) {
            // An active duplicate cannot receive a second response with the same ID.
            if (rpc_to_engine.contains(id_key(*id))) { broken = true; return false; }
            return error(*id, -32600, "Request ID already used in this session");
        }
        auto key = id_key(*id);
        if (used_ids.size() >= identity_limit || key.size() > response_limit - identity_bytes) { broken = true; return false; }
        identity_bytes += key.size();
        used_ids.insert(std::move(key));
        if (m == "ping") return result(*id, {});
        if (m == "initialize") {
            if (phase != Phase::New) return error(*id, -32600, "Already initialized");
            auto protocol = p.if_contains("protocolVersion"), capabilities = p.if_contains("capabilities"), info = p.if_contains("clientInfo");
            if (!protocol || !protocol->is_string() || !capabilities || !capabilities->is_object() ||
                !info || !info->is_object() || !info->as_object().if_contains("name") ||
                !info->as_object().at("name").is_string() || !info->as_object().if_contains("version") ||
                !info->as_object().at("version").is_string()) return error(*id, -32602, "Invalid initialize parameters");
            phase = Phase::Initializing;
            return result(*id, {{"protocolVersion", mcp_protocol_version},
                {"capabilities", object{{"tools", object{{"listChanged", false}}}}},
                {"serverInfo", object{{"name", "va-studio-cli"}, {"version", "31-experimental"}}}});
        }
        if (phase != Phase::Ready) return error(*id, -32000, "Initialization is incomplete");
        if (m == "tools/list") {
            std::size_t offset = 0;
            if (auto cursor = p.if_contains("cursor")) {
                if (!cursor->is_string()) return error(*id, -32602, "Invalid cursor");
                bool found = false;
                for (std::size_t n = page_size; n < tools.size(); n += page_size) {
                    if (std::string(cursor->as_string()) == catalog_identity + ":" + std::to_string(n)) { offset = n; found = true; break; }
                }
                if (!found) return error(*id, -32602, "Invalid cursor");
            }
            array page;
            auto i = tools.begin(); std::advance(i, offset);
            for (std::size_t n = 0; i != tools.end() && n < page_size; ++i, ++n) page.emplace_back(i->second.descriptor);
            object out{{"tools", page}};
            if (offset + page.size() < tools.size()) out["nextCursor"] = catalog_identity + ":" + std::to_string(offset + page.size());
            return result(*id, std::move(out));
        }
        if (m != "tools/call") return error(*id, -32601, "Method not found");
        auto name = p.if_contains("name"), args = p.if_contains("arguments");
        if (!name || !name->is_string() || (args && !args->is_object())) return error(*id, -32602, "Invalid tools/call parameters");
        auto tool = tools.find(std::string(name->as_string()));
        if (tool == tools.end()) return error(*id, -32602, "Unknown tool");
        value progress = nullptr;
        if (auto meta = p.if_contains("_meta")) {
            if (!meta->is_object()) return error(*id, -32602, "Invalid request metadata");
            if (auto token = meta->as_object().if_contains("progressToken")) {
                if (!valid_id(*token)) return error(*id, -32602, "Invalid progress token");
                progress = *token;
            }
        }
        if (!progress.is_null() && std::any_of(pending.begin(), pending.end(), [&](auto const &entry) {
                return !entry.second.progress_token.is_null() && id_key(entry.second.progress_token) == id_key(progress);
            })) return error(*id, -32602, "Progress token is already active");
        if (pending.size() >= active_limit) return error(*id, -32001, "Too many active tool calls");
        auto engine_id = "mcp-" + std::to_string(++next_id);
        object request = args ? args->as_object() : object{};
        // Clients may not override adapter identity or command routing.
        for (auto field : {"schema", "id", "command"})
            if (request.contains(field)) return error(*id, -32602, "Reserved tool argument");
        request["schema"] = "va-studio.cli-request/1"; request["id"] = engine_id;
        request["command"] = tool->second.command;
        auto parsed = parse_production_request(serialize(request));
        if (!parsed.request) {
            Record r; r.action = tool->second.command; r.status = Status::Rejected;
            r.reason = parsed.error ? parsed.error->code : "invalid-argument"; r.error = parsed.error;
            r.message = parsed.error ? parsed.error->message : "Invalid tool arguments";
            return result(*id, tool_result(typed_result(r, engine_id)));
        }
        pending.emplace(engine_id, Pending{*id, progress, 0, false});
        rpc_to_engine.emplace(id_key(*id), engine_id);
        auto admission = session->submit(*parsed.request);
        if (!admission.admitted) {
            if (!admission.refusal) {
                pending.erase(engine_id); rpc_to_engine.erase(id_key(*id));
                return error(*id, -32603, "Session refused without a result");
            }
            return settle(engine_id, typed_result(*admission.refusal, engine_id));
        }
        return !broken;
    }
};
McpProtocol::McpProtocol(LineWriter w) : _impl(std::make_unique<Impl>(std::move(w))) {}
McpProtocol::~McpProtocol() { close(); }
void McpProtocol::bind(StructuredSession &s) {
    if (_impl->session || _impl->phase != Impl::Phase::New) throw std::logic_error("MCP bind once before input");
    _impl->session = &s;
}
bool McpProtocol::receive(std::string_view line) {
    try { return _impl->receive(line); }
    catch (...) { _impl->broken = true; return false; } // Engine/transport contract failed; never replay.
}
bool McpProtocol::oversized_input() {
    return _impl->error(nullptr, -32600, "MCP request exceeds input limit");
}
bool McpProtocol::event(EngineEvent const &e) {
    auto &s = *_impl;
    if (s.broken || s.phase == Impl::Phase::Closed) return false;
    auto i = s.pending.find(e.request_id);
    if (i == s.pending.end()) return true; // Late/duplicate/foreign event has no delivery.
    if (i->second.seen_event && e.sequence <= i->second.sequence) return true;
    i->second.seen_event = true; i->second.sequence = e.sequence;
    try {
        if (e.kind == EngineEventKind::Result) return s.settle(e.request_id, e.payload);
        if (e.kind == EngineEventKind::Progress && !i->second.progress_token.is_null()) {
            auto progress = e.payload.if_contains("progress");
            if (!progress || !progress->is_number() || !std::isfinite(progress->to_number<double>())) return true;
            auto current = progress->to_number<double>();
            if (i->second.last_progress && current <= *i->second.last_progress) return true;
            i->second.last_progress = current;
            object params{{"progressToken", i->second.progress_token}, {"progress", *progress}};
            if (auto total = e.payload.if_contains("total"); total && total->is_number() && std::isfinite(total->to_number<double>())) params["total"] = *total;
            if (auto message = e.payload.if_contains("message"); message && message->is_string() && message->as_string().size() <= 1024)
                params["message"] = *message;
            return s.send({{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", params}});
        }
        return true; // Accepted and Status are internal, never raw JSONL output.
    } catch (...) { s.broken = true; return false; }
}
void McpProtocol::close() noexcept {
    auto &s = *_impl;
    if (s.phase == Impl::Phase::Closed) return;
    s.phase = Impl::Phase::Closed; s.pending.clear(); s.rpc_to_engine.clear();
    if (s.session) s.session->close();
}
bool McpProtocol::failed() const { return _impl->broken; }
int run_mcp_stdio_with_factory(ByteReader read, LineWriter write, SessionOptions options,
                               StructuredSessionFactory factory, McpOwnerPump pump) {
    try {
        McpProtocol protocol(std::move(write));
        auto created = factory(std::move(options), [&](EngineEvent const &event) { return protocol.event(event); });
        if (!created.engine || created.error) return 2;
        // Explicit close before session destruction (protocol is constructed first).
        struct Close { McpProtocol &p; ~Close() { p.close(); } } close{protocol};
        protocol.bind(*created.engine);
        JsonLines lines;
        std::string line;
        for (;;) {
            if (pump) pump();
            if (protocol.failed()) return 1;
            // Yield even if an oversized producer continuously fills stdin. This
            // bounds framing work between owner pumps without changing JsonLines.
            unsigned read_budget = 16;
            ByteReader fair_read = [&](char *bytes, std::size_t capacity) {
                if (!read_budget) return -2;
                --read_budget;
                return read(bytes, capacity);
            };
            switch (lines.next(fair_read, line)) {
                case JsonLines::State::Line: if (!protocol.receive(line)) return 1; break;
                case JsonLines::State::Oversize:
                    // Do not allocate the oversized input just to diagnose it.
                    if (!protocol.oversized_input()) return 1;
                    break;
                case JsonLines::State::Retry: break;
                case JsonLines::State::End: return 0;
                case JsonLines::State::Error: return 1;
            }
        }
    } catch (...) { return 1; }
}
int run_mcp_stdio(ByteReader read, LineWriter write, SessionOptions options) {
    return run_mcp_stdio_with_factory(std::move(read), std::move(write), std::move(options), make_structured_session);
}
}
