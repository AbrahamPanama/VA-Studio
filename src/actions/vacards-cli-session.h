// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef VACARDS_CLI_SESSION_H
#define VACARDS_CLI_SESSION_H
#include "vacards-cli-dispatch.h"
#include "vacards-cli-transport.h"
#include "io/vacards-cli-intake.h"
namespace Inkscape::VACardsCli {
struct FileState;
// Shared owner-thread admission; no mutation, callbacks, or test-only bypass.
std::optional<Record> session_command_admission(Request const &, DispatchContext const &, FileState const &);
struct SessionOptions {
    std::string inspection_path;
    Grants grants;
    // In-process test seam only; never selected through the wire or environment.
    std::function<void(Request const &, std::function<bool()> const &)> before_dispatch;
    std::string document_path; // Explicit editable one-shot baseline; inspection stays immutable.
    // Production hook: runs first on the structured session's native thread (for example bitmap
    // main-thread affinity). Never selected by the wire or the environment.
    std::function<void()> on_native_thread_start;
};
// Additive structured-event facade for adapters. Engine owns documents, contexts,
// tokens and worker settlement. Call on the owner thread; cancel/status never
// dereference live document state from the reader. No transport bytes emitted.
enum class EngineEventKind { Accepted, Progress, Result, Status };
struct EngineEvent {
    EngineEventKind kind = EngineEventKind::Result;
    std::string request_id;
    std::uint64_t sequence = 0;
    boost::json::object payload;
};
using EngineEventSink = std::function<bool(EngineEvent const &)>;
struct Submission {
    bool admitted = false;
    std::optional<Record> refusal; // synchronous refusal only; no terminal event
};
class StructuredSession {
public:
    virtual ~StructuredSession() = default;
    // Accepted submit emits exactly one Result event at settlement; return is
    // admission only. A refused submit emits no Accepted/Result event.
    virtual Submission submit(Request const &) = 0;
    virtual Record cancel(std::string const &request_id) = 0;
    virtual boost::json::object status() const = 0;
    // Owner thread: deliver queued native events through the sink. Default no-op for in-process fakes.
    virtual void pump() {}
    // Cancel preparation, settle active native publication exactly once, retire
    // delivery and tokens. No implicit save or crash replay.
    virtual void close() noexcept = 0;
};
struct StructuredSessionResult {
    std::unique_ptr<StructuredSession> engine;
    std::optional<ParseError> error;
};
StructuredSessionResult make_structured_session(SessionOptions, EngineEventSink);
int run_agent_session(ByteReader read, LineWriter write, SessionOptions options = {});
int run_agent_request(std::string_view bytes, LineWriter write, SessionOptions options = {});
}
#endif
