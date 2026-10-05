// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "vacards-cli-session.h"
namespace Inkscape::VACardsCli {
inline constexpr std::string_view mcp_protocol_version = "2025-06-18";
// All calls and structured events are serialized on the session owner thread.
// The writer accepts one complete UTF-8 line including its newline, or returns
// false without retry. It must not block the owner thread or reenter this adapter.
class McpProtocol {
public:
    explicit McpProtocol(LineWriter);
    ~McpProtocol();
    McpProtocol(McpProtocol const &) = delete;
    McpProtocol &operator=(McpProtocol const &) = delete;
    void bind(StructuredSession &); // Session outlives adapter; bind once, before receive.
    bool receive(std::string_view line);
    bool event(EngineEvent const &);
    bool oversized_input(); // Framer drained an over-limit line without retaining it.
    void close() noexcept; // EOF/disconnect, idempotent; retires delivery before engine close.
    bool failed() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
using StructuredSessionFactory = std::function<StructuredSessionResult(SessionOptions, EngineEventSink)>;
// Pump and ByteReader must cooperate: read returns -2 instead of blocking the
// owner loop while native work needs pumping. No pump is declared by integration yet.
using McpOwnerPump = std::function<void()>;
int run_mcp_stdio(ByteReader read, LineWriter write, SessionOptions options = {});
int run_mcp_stdio_with_factory(ByteReader, LineWriter, SessionOptions,
                               StructuredSessionFactory, McpOwnerPump = {});
}
