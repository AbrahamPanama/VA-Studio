// SPDX-License-Identifier: GPL-2.0-or-later
#include "vacards-cli-session.h"
#include "vacards-cli-entry.h"
#include "vacards-cli-production.h"
#include "vacards-cli-fault.h"
#include "vacards-cli-edit-services.h"
#include "actions-vacards-bitmap.h"
#include "nesting/nesting-cli-service.h"
#include "actions-vacards-cli.h"
#include "io/vacards-cli-files.h"
#include "selection.h"
#include "object/sp-item.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <glib.h>
#ifndef _WIN32
#include <pthread.h>
#endif
namespace Inkscape::VACardsCli {
namespace {
using namespace boost::json;
Record refusal(std::string code, std::string message, std::string action = {}) {
    Record r; r.action = std::move(action); r.status = Status::Rejected;
    r.reason = std::move(code); r.message = std::move(message); return r;
}
Record oversized(Record const &original) {
    Record r;
    r.action = original.action; r.mode = original.mode; r.dry_run = original.dry_run;
    r.status = original.status == Status::Uncertain ? Status::Uncertain : Status::Failed;
    r.reason = "response-too-large";
    r.message = "Response exceeds 8 MiB; completed effects and publication were not rolled back.";
    r.publication = original.publication; r.publication_persisted = original.publication_persisted;
    r.document_id = original.document_id; r.document_path = original.document_path;
    r.revision_before = original.revision_before; r.revision_after = original.revision_after;
    // Keep bounded native publication/recovery evidence in the compact error result.
    if (auto publication = original.data.if_contains("publication"); publication && serialize(*publication).size() <= (1u << 20))
        r.data["publication"] = *publication;
    return r;
}
void unavailable(ActionContext &c) {
    c.record = refusal("session-required", "This command needs an agent session.");
}
} // anonymous namespace
std::optional<Record> session_command_admission(Request const &r, DispatchContext const &context, FileState const &files) {
    auto overlay = context.production_handler ? find_production_command(r.command) : nullptr;
    auto s = overlay;
    if (!s) s = find_command(r.command);
    if (!s) return refusal("unknown-command", "Command is not registered.");
    auto id = s->canonical_id;
    bool document_write = s->effects == "document-edit" || id == "history.undo" || id == "history.redo";
    if (files.read_only && document_write)
        return refusal("document-read-only", "Inspection is immutable; explicitly create or open an editable document.");
    if (files.writes_blocked && document_write)
        return refusal("reconciliation-required", "Reconcile uncertain publication before editing the document.");
    if (id == "system.result-file")
        return refusal("session-output-fixed", "Agent results cannot switch output sinks.");
    if (!overlay && ((s->effects == "document-edit" && id != "file.import") || id.starts_with("history.")))
        return refusal(files.read_only ? "document-read-only" : "slice-unavailable",
                       "Legacy edits and history are unavailable on the M2 agent wire.");
    if (files.read_only && (id == "file.import" || id == "file.save"))
        return refusal("document-read-only", "Inspection is immutable; explicitly create or open an editable document.");
    if (files.writes_blocked && (id == "file.new" || id == "file.import" || id == "file.save" || id == "file.export" ||
                                (id == "file.close" && !r.params.contains("reconcile"))))
        return refusal("reconciliation-required", "Verify the uncertain destination before another write or discard.");
    if ((id == "file.new" || id == "file.open" || id == "file.close") && context.document &&
        context.document->isModifiedSinceSave()) {
        auto discard = r.params.if_contains("discard");
        if (!discard || !discard->as_bool())
            return refusal("dirty-document", "Explicit discard is required to replace or close a dirty document.");
    }
    return {};
}
namespace {
struct Engine {
    SessionOptions options;
    bool persistent_session = false;
    LineWriter write;
    IntakeResult intake;
    FileState files;
    TokenStore tokens;
    Bitmap::CliSession bitmap;
    Nesting::CliSession nest;
    std::uint64_t incarnation = 1, target_generation = 1;
    object document_snapshot;
    bool writes_blocked_snapshot = false;
    object reconciliation_snapshot;
    DispatchContext context;
    std::mutex state, output;
    std::condition_variable wake;
    std::optional<Request> pending;
    std::set<std::string> ids;
    std::string active, phase = "idle", session_id;
    bool eof = false, closed = false, halt = false;
    std::atomic<bool> cancelled{false}, stop{false};
    std::atomic<int> fatal{0};
    // Owner-thread settlement survives a later metadata/reporting exception.
    bool settled_persisted = false, settled_uncertain = false;
    std::uint64_t event_seq = 0;
    unsigned terminal_seq = 0;
    std::chrono::steady_clock::time_point last_progress;

    Engine(LineWriter out, SessionOptions opts, bool persistent = false) : options(std::move(opts)), persistent_session(persistent), write(std::move(out)) {
        auto uuid = g_uuid_string_random(); session_id = uuid; g_free(uuid);
        if (!options.inspection_path.empty()) {
            intake = load_inspection_document(options.inspection_path, options.grants);
            files.read_only = true;
        } else if (!options.document_path.empty()) {
            intake = load_editable_document(options.document_path, options.grants,
                                           FileLoadOptions{"auto", "embed", "reject", {}});
        }
        files.document = std::move(intake.document);
        files.provenance = intake.report;
        context.document = files.document.get();
        context.selection = context.document ? context.document->getSelection() : nullptr;
        files.before_document_retire = [this] {
            nest.retire();
            bitmap.retire(); // Jobs/delivery/context retire before SPDocument is destroyed.
            tokens.invalidate_document(document_stamp(context.document).id, incarnation);
        };
        context.cancelled = [this] { return cancelled.load(); };
        context.session_handler = [this](Request const &r) { return control(r); };
        context.file_handler = [this](Request const &r) { return file(r); };
        context.command_admission = [this](Request const &r) { return admission(r); };
        // Development execution is authorized independently of release acceptance.
        context.production_handler = [this](Request const &r) {
            EditServices edits{*context.document, *context.selection, document_stamp(context.document)};
            edits.targets.generation = target_generation;
            ProductionContext production{files, options.grants, edits, tokens, [] {}, persistent_session ? session_id : std::string{},
                std::string(planned_production_catalog().at("hash").as_string()), incarnation, target_generation};
            production.bitmap = &bitmap;
            production.nest = &nest;
            cli_fault_throw("engine.before-service");
            return execute_production(r, context, production);
        };
        snapshot();
        context.session_state_changed = [this] { std::lock_guard lock(state); ++context.session_revision; };
        context.session_revision_snapshot = [this] {
            std::lock_guard lock(state); return context.session_revision;
        };
    }
    ~Engine() { nest.retire(); bitmap.retire(); }
    // Called on the owning thread only. Reader controls use values copied under state.
    void snapshot() {
        object summary;
        if (context.document) {
            auto stamp = document_stamp(context.document);
            summary = {{"id", stamp.id}, {"revision", stamp.revision},
                       {"read_only", files.read_only}, {"dirty", context.document->isModifiedSinceSave()}};
        }
        std::lock_guard lock(state);
        document_snapshot = std::move(summary);
        writes_blocked_snapshot = files.writes_blocked;
        reconciliation_snapshot = files.reconciliation;
    }
    std::optional<Record> admission(Request const &r) {
        return session_command_admission(r, context, files);
    }
    Record file(Request const &request) {
        auto before = document_stamp(context.document);
        auto r = request;
        r.command = std::string(find_command(request.command)->canonical_id);
        Record record;
        try { record = execute_file(r, context, files, options.grants); }
        catch (...) {
            record = refusal("internal-error", "File service did not complete its result; inspect publication evidence before retrying.");
            if (r.command == "file.save" || r.command == "file.export") {
                record.status = Status::Uncertain; record.publication = "uncertain";
                files.writes_blocked = true;
                record.data["publication"] = object{{"outcome", "Uncertain"}, {"destination", r.params.at("path")}};
                files.reconciliation = record.data.at("publication").as_object();
                files.reconciliation["acknowledged"] = false;
            } else record.status = Status::Failed;
        }
        settled_persisted = record.publication_persisted;
        settled_uncertain = record.status == Status::Uncertain;
        // The file service owns atomic swap/intake/publication; the session owns lifetime and metadata visibility.
        context.document = files.document.get();
        context.selection = context.document ? context.document->getSelection() : nullptr;
        auto after = document_stamp(context.document);
        if (record.status == Status::Uncertain) files.writes_blocked = true;
        record.selection_after.clear();
        if (context.selection)
            for (auto item : context.selection->items())
                if (item->getId()) record.selection_after.emplace_back(item->getId());
        record.document_id = after.id;
        record.revision_before = before.revision;
        record.revision_after = after.revision;
        if (context.document) {
            auto path = context.document->getDocumentFilename();
            record.document_path = path ? path : "";
        } else record.document_path.reset();
        if (!request.dry_run && before.id != after.id) {
            tokens.invalidate_document(before.id, incarnation);
            ++incarnation; ++target_generation;
            std::lock_guard lock(state);
            ++context.session_revision;
        }
        // Lifecycle payloads report the resulting session domain; document revisions are independent.
        if (r.command == "file.new" || r.command == "file.open" || r.command == "file.close") {
            std::lock_guard lock(state);
            if (record.status == Status::Ok || record.status == Status::Changed || record.status == Status::Unchanged)
                record.data["session_revision"] = context.session_revision;
        }
        snapshot();
        return record;
    }
    object error(ParseError const &e) {
        return cli_error(e.code, e.message, "Inspect the command schema and send a corrected UTF-8 request.");
    }
    void emit(std::string_view event, std::string_view id, object payload = {}) {
        std::lock_guard lock(output);
        payload["schema"] = "va-studio.cli-session/1";
        payload["event"] = event; payload["event_seq"] = ++event_seq;
        payload["id"] = id.empty() ? value(nullptr) : value(id);
        auto line = serialize(payload) + "\n";
        if (line.size() > response_limit || !write(line)) { fatal = 2; stop = true; wake.notify_all(); }
    }
    object result(Record const &r, std::string_view id, unsigned seq) {
        auto out = typed_result(r, id, seq);
        if (r.reason == "document-read-only")
            out["error"].as_object()["hint"] = "Use explicit file.new or file.open to establish an editable baseline.";
        return out;
    }
    void terminal(Record const &r, std::string_view id) {
        // Serialize terminal seq and event seq together, including reader-thread controls.
        std::lock_guard lock(output);
        auto value = result(r, id, ++terminal_seq);
        object envelope{{"schema", "va-studio.cli-session/1"}, {"event", "result"},
                        {"event_seq", ++event_seq}, {"id", boost::json::value(id)},
                        {"result", value}};
        auto line = serialize(envelope) + "\n";
        if (line.size() > response_limit) {
            envelope["result"] = result(oversized(r), id, terminal_seq);
            line = serialize(envelope) + "\n";
        }
        if (!write(line)) { fatal = 2; stop = true; wake.notify_all(); }
    }
    void hello() {
        auto h = cli_identity(); h["session_id"] = session_id;
        h["limits"] = object{{"request_bytes", request_limit}, {"response_bytes", response_limit},
            {"request_ids", 100000}, {"input_bytes", 64u << 20}, {"objects", 100000}, {"xml_depth", 128},
            {"decoded_raster_pixels", 32000000}, {"operation_scratch_bytes", 512u << 20},
            {"token_roots", 4}, {"retained_bytes", 512u << 20}};
        h["document"] = nullptr;
        if (context.document) {
            auto stamp = document_stamp(context.document);
            object summary;
            for (auto key : {"format", "source_bytes", "source_sha256"})
                if (auto value = intake.report.if_contains(key)) summary[key] = *value;
            if (auto ids = intake.report.if_contains("id_map")) summary["normalized_ids"] = ids->as_array().size();
            h["document"] = object{{"id", stamp.id}, {"revision", stamp.revision}, {"read_only", files.read_only},
                                    {"dirty", context.document->isModifiedSinceSave()}, {"inspection", summary}};
        }
        if (intake.error) h["startup_error"] = cli_error(intake.error->code, intake.error->message, intake.error->hint);
        emit("hello", {}, std::move(h));
    }
    Record control(Request const &original) {
        auto r = original;
        r.command = std::string(find_command(original.command)->canonical_id);
        std::lock_guard lock(state);
        Record out; out.action = r.command; out.normalized_params = r.params; out.dry_run = r.dry_run;
        if (!document_snapshot.empty()) {
            out.document_id = std::string(document_snapshot.at("id").as_string());
            out.revision_before = out.revision_after = document_snapshot.at("revision").to_number<std::uint64_t>();
        }
        auto refuse = [&](std::string code, std::string message) {
            out.status = Status::Rejected; out.reason = std::move(code); out.message = std::move(message); return out;
        };
        // Immediate status/cancel requests describe the active request; they are
        // not themselves cancelled when that request's flag is set.
        if (cancelled && active == r.id) {
            out.status = Status::Cancelled; out.reason = "cancelled";
            out.message = "Cancelled before execution."; return out;
        }
        if (r.document) return refuse("stale-document", "Session commands do not take a document guard.");
        if (r.if_revision && *r.if_revision != context.session_revision)
            return refuse("stale-revision", "Refresh the session revision.");
        if (r.command == "session.close") {
            if (writes_blocked_snapshot) return refuse("reconciliation-required", "Resolve uncertain publication before session close.");
            auto discard = r.params.if_contains("discard");
            if (!document_snapshot.empty() && document_snapshot.at("dirty").as_bool() &&
                (!discard || !discard->as_bool()))
                return refuse("dirty-document", "Explicit discard is required to close a dirty session.");
        }
        if (r.dry_run && r.command != "session.status") {
            out.data["validation_level"] = "preflight"; return out;
        }
        if (r.command == "session.status") {
            auto retained = tokens.snapshot();
            array token_ids; for (auto const &id : retained.active_tokens) token_ids.emplace_back(id);
            out.data = {{"active_request", active.empty() ? value(nullptr) : value(active)},
                {"phase", phase}, {"uninterruptible", phase == "native-call"}, {"session_revision", context.session_revision},
                {"retention", object{{"tokens", token_ids}, {"roots", retained.active_roots}, {"bytes", retained.charged_bytes}}},
                {"document", document_snapshot.empty() ? value(nullptr) : value(document_snapshot)},
                {"writes_blocked", writes_blocked_snapshot}, {"reconciliation", reconciliation_snapshot}};
        } else if (r.command == "session.cancel") {
            auto id = r.params.at("id").as_string();
            bool found = !active.empty() && active == id;
            if (found) cancelled = true;
            else out.status = Status::Unchanged;
            out.data = {{"found", found}, {"id", id}, {"cancellation_requested", found}};
        } else if (r.command == "session.release") {
            array released, not_found;
            for (auto const &id : r.params.at("tokens").as_array()) {
                auto result = tokens.release_subtree(std::string(id.as_string()));
                for (auto const &token : result.released) released.emplace_back(token);
                for (auto const &token : result.not_found) not_found.emplace_back(token);
            }
            auto retained = tokens.snapshot();
            bitmap.prune(retained);
            nest.prune(retained);
            if (released.empty()) out.status = Status::Unchanged;
            out.data = {{"released", released}, {"not_found", not_found}};
        } else if (r.command == "session.close") {
            nest.retire();
            bitmap.retire();
            if (!document_snapshot.empty())
                tokens.invalidate_document(std::string(document_snapshot.at("id").as_string()), incarnation);
            closed = true; out.data = {{"closed", true}, {"persisted", false}};
        } else if (r.command == "system.options") {
            if (auto p = r.params.if_contains("preferred-unit")) context.preferred_unit = std::string(p->as_string());
            if (auto p = r.params.if_contains("halt-on-error")) halt = p->as_bool();
            out.data = {{"preferred-unit", context.preferred_unit}, {"halt-on-error", halt}};
        }
        if (r.command != "session.status" && out.status != Status::Unchanged) ++context.session_revision;
        return out;
    }
    Record internal_failure(Request const &r) {
        auto record = refusal("internal-error", "Unexpected dispatch or result metadata failure.", r.command);
        record.status = Status::Failed;
        if (settled_persisted) {
            record.publication = "published"; record.publication_persisted = true;
            record.message = "Result metadata failed after confirmed publication; completed publication was not rolled back.";
        } else if (settled_uncertain) {
            record.status = Status::Uncertain; record.publication = "uncertain";
            files.writes_blocked = true;
            record.message = "Result metadata failed after uncertain publication; reconcile before another write.";
            if (!files.reconciliation.empty()) record.data["publication"] = files.reconciliation;
        }
        return record;
    }
    Record execute(Request const &r) {
        settled_persisted = settled_uncertain = false;
        // A reader-side cancel advances the session revision. Settle an already
        // accepted file request before that control's revision can mask cancellation.
        if (r.command.starts_with("file.") && cancelled.load()) {
            auto record = refusal("cancelled", "Cancelled before execution.", r.command);
            record.status = Status::Cancelled; record.dry_run = r.dry_run;
            auto stamp = document_stamp(context.document);
            record.document_id = stamp.id;
            record.revision_before = record.revision_after = stamp.revision;
            return record;
        }
        try {
            auto before = document_stamp(context.document);
            auto record = dispatch(r, context);
            if (document_stamp(context.document).revision != before.revision) ++target_generation;
            // Also cover cancellation racing the revision check after the early check.
            if (r.command.starts_with("file.") && cancelled.load() &&
                record.status == Status::Rejected && record.reason == "stale-revision") {
                record.status = Status::Cancelled; record.reason = "cancelled";
                record.message = "Cancelled before execution."; record.error.reset();
            }
            return record;
        }
        catch (...) {
            auto record = internal_failure(r);
            auto stamp = document_stamp(context.document);
            record.document_id = stamp.id;
            record.revision_before = record.revision_after = stamp.revision;
            return record;
        }
    }
    void reader(ByteReader const &read) {
        try {
            JsonLines framing;
            while (!stop) {
                {
                    std::lock_guard lock(this->state);
                    auto now = std::chrono::steady_clock::now();
                    if (!active.empty() && phase != "queued" && now - last_progress >= std::chrono::milliseconds(200)) {
                        last_progress = now;
                        emit("progress", active, {{"phase", phase}, {"phase_label", phase == "native-call" ? "Native call (uninterruptible)" : "Preparing request"},
                            {"phase_percent", 0}, {"completed", 0}, {"total", nullptr}, {"uninterruptible", phase == "native-call"}});
                    }
                }
                std::string line;
                auto state = framing.next(read, line);
                if (state == JsonLines::State::Retry) continue;
                if (state == JsonLines::State::End || state == JsonLines::State::Error) {
                    std::lock_guard lock(this->state);
                    eof = true; cancelled = true;
                    if (state == JsonLines::State::Error) fatal = 2;
                    wake.notify_all(); return;
                }
                if (state == JsonLines::State::Oversize) {
                    terminal(refusal("request-too-large", "Request exceeds 1 MiB."), {}); continue;
                }
                auto parsed = parse_production_request(line);
                if (parsed.error) {
                    terminal(refusal(parsed.error->code, parsed.error->message, parsed.error_command), parsed.error_id); continue;
                }
                auto const &r = *parsed.request;
                bool immediate = r.command == "session.status" || r.command == "session.cancel";
                {
                    std::lock_guard lock(this->state);
                    if (ids.contains(r.id)) { terminal(refusal("duplicate-request-id", "Use a new request id."), r.id); continue; }
                    if (ids.size() == 100000) { terminal(refusal("request-history-full", "Restart the session with new request history."), r.id); continue; }
                    ids.insert(r.id);
                    if (!immediate && !active.empty()) {
                        terminal(refusal("session-busy", "Wait for the active request to finish."), r.id); continue;
                    }
                    if (!immediate) { active = r.id; phase = "queued"; cancelled = false; }
                }
                emit("accepted", r.id);
                if (immediate) {
                    // Snapshot-only control dispatch: no SPDocument, XML or history on this thread.
                    DispatchContext control_context;
                    control_context.session_handler = [this](Request const &r) { return control(r); };
                    terminal(dispatch(r, control_context), r.id);
                } else {
                    { std::lock_guard lock(this->state); pending = r; }
                    wake.notify_one();
                    // The close frame is the last admitted frame; do not block waiting for more input.
                    if (r.command == "session.close" && !r.dry_run) {
                        std::unique_lock lock(this->state);
                        wake.wait(lock, [&] { return closed || active.empty() || stop; });
                        if (closed || stop) return;
                    }
                }
            }
        } catch (...) { fatal = 4; stop = true; wake.notify_all(); }
    }
    int run(ByteReader const &read) {
        hello();
        std::thread input([&] { reader(read); });
        try {
            for (;;) {
                Request request;
                {
                    std::unique_lock lock(state);
                    wake.wait(lock, [&] { return pending || eof || stop; });
                    if (!pending) break;
                    request = std::move(*pending); pending.reset(); phase = "preparation";
                    last_progress = std::chrono::steady_clock::now();
                }
                emit("progress", request.id, {{"phase", "preparation"}, {"phase_label", "Preparing request"},
                     {"phase_percent", 0}, {"completed", 0}, {"total", nullptr}});
                Record record;
                settled_persisted = settled_uncertain = false;
                try {
                    if (options.before_dispatch) options.before_dispatch(request, [this] { return cancelled.load(); });
                    { std::lock_guard lock(state); phase = "native-call"; }
                    record = execute(request);
                    snapshot();
                } catch (...) {
                    record = internal_failure(request);
                    fatal = 4; stop = true;
                }
                {
                    std::lock_guard lock(state);
                    terminal(record, request.id);
                    active.clear(); phase = "idle"; wake.notify_all();
                    if (closed || eof || stop) break;
                }
            }
        } catch (...) { fatal = 4; }
        stop = true; wake.notify_all(); input.join();
        return fatal;
    }
};

class NativeThread {
    std::function<void()> function;
#ifdef _WIN32
    std::thread thread;
#else
    pthread_t thread{};
    bool started = false;
    static void *entry(void *arg) noexcept {
        auto self = static_cast<NativeThread *>(arg);
        try { self->function(); } catch (...) {}
        return nullptr;
    }
#endif
public:
    explicit NativeThread(std::function<void()> fn) : function(std::move(fn)) {}
    bool start() noexcept {
        try {
#ifdef _WIN32
            thread = std::thread([this] { try { function(); } catch (...) {} });
            return true;
#else
            pthread_attr_t attributes;
            if (pthread_attr_init(&attributes) != 0) return false;
            auto const attr_result = pthread_attr_setstacksize(&attributes, 64u * 1024u * 1024u);
            auto const create_result = attr_result == 0 ? pthread_create(&thread, &attributes, &entry, this) : attr_result;
            pthread_attr_destroy(&attributes);
            started = create_result == 0;
            return started;
#endif
        } catch (...) { return false; }
    }
    void join() noexcept {
#ifdef _WIN32
        if (thread.joinable()) { try { thread.join(); } catch (...) {} }
#else
        if (started) { pthread_join(thread, nullptr); started = false; }
#endif
    }
    ~NativeThread() { join(); }
};

class StructuredEngine final : public StructuredSession {
    static constexpr std::size_t max_events = 1024;
    SessionOptions options;
    EngineEventSink sink;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<EngineEvent> events;
    std::optional<Request> pending;
    std::unique_ptr<Engine> engine;
    NativeThread native;
    std::atomic<unsigned> terminal_sequence{0};
    bool started = false, initialized = false, construction_failed = false;
    bool close_requested = false, close_ready = false, delivery_retired = false, delivery_failed = false;
    std::atomic<bool> session_closed{false};
    bool in_sink = false;
    void queue_progress(EngineEvent event) noexcept {
        try {
            std::lock_guard lock(mutex);
            if (delivery_retired || events.size() >= max_events) return;
            events.push_back(std::move(event));
        } catch (...) {}
    }
    bool emit_sync(EngineEvent const &event) {
        if (delivery_retired || delivery_failed) return false;
        in_sink = true;
        bool ok = false;
        try { ok = sink(event); } catch (...) { ok = false; }
        in_sink = false;
        if (!ok) delivery_failed = true;
        return ok;
    }
    boost::json::object result_payload(Record const &record, std::string const &id) {
        auto seq = terminal_sequence.fetch_add(1) + 1;
        auto result = engine->result(record, id, seq);
        boost::json::object envelope{{"schema", "va-studio.cli-session/1"}, {"event", "result"},
                                     {"event_seq", 0}, {"id", id}, {"result", result}};
        if (boost::json::serialize(envelope).size() + 1 > response_limit)
            result = engine->result(oversized(record), id, seq);
        return result;
    }
    void queue_result(Record const &record, std::string const &id) {
        EngineEvent event{EngineEventKind::Result, id, 4, result_payload(record, id)};
        std::unique_lock lock(mutex);
        while (!delivery_retired && events.size() >= max_events) {
            auto progress = std::find_if(events.begin(), events.end(), [](auto const &queued) {
                return queued.kind == EngineEventKind::Progress;
            });
            if (progress != events.end()) { events.erase(progress); break; }
            wake.wait(lock, [&] { return delivery_retired || events.size() < max_events; });
        }
        if (!delivery_retired) events.push_back(std::move(event));
    }
    void run() noexcept {
        try {
            if (options.on_native_thread_start) options.on_native_thread_start();
            engine = std::make_unique<Engine>([](std::string_view) { return false; }, std::move(options), true);
            {
                std::lock_guard lock(mutex); initialized = true;
            }
            wake.notify_all();
            for (;;) {
                Request request;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [&] { return (pending && (!close_requested || close_ready)) ||
                                                   (close_requested && close_ready); });
                    if (!pending && close_requested && close_ready) break;
                    request = std::move(*pending); pending.reset();
                }
                {
                    std::lock_guard lock(engine->state);
                    engine->phase = "preparation";
                }
                queue_progress({EngineEventKind::Progress, request.id, 2,
                    {{"progress", 1}, {"phase", "preparation"}, {"message", "Preparing request"}}});
                Record record;
                bool fatal = false;
                engine->settled_persisted = engine->settled_uncertain = false;
                try {
                    if (engine->options.before_dispatch)
                        engine->options.before_dispatch(request, [this] { return engine->cancelled.load(); });
                    {
                        std::lock_guard lock(engine->state); engine->phase = "native-call";
                    }
                    queue_progress({EngineEventKind::Progress, request.id, 3,
                        {{"progress", 2}, {"phase", "native-call"},
                         {"message", "Native call (uninterruptible)"}, {"uninterruptible", true}}});
                    record = engine->execute(request);
                    engine->snapshot();
                } catch (...) {
                    record = engine->internal_failure(request);
                    engine->stop = true;
                    fatal = true;
                }
                {
                    std::lock_guard lock(engine->state);
                    engine->active.clear(); engine->phase = "idle";
                }
                queue_result(record, request.id);
                {
                    std::unique_lock lock(mutex);
                    if (engine->closed || fatal) session_closed = true;
                    if (close_requested && close_ready) break;
                    if (session_closed) wake.wait(lock, [&] { return close_requested && close_ready; });
                    if (close_requested && close_ready) break;
                }
            }
        } catch (...) {
            std::unique_lock lock(mutex);
            construction_failed = !initialized;
            initialized = true; session_closed = true;
            wake.notify_all();
            if (!construction_failed)
                wake.wait(lock, [&] { return close_requested && close_ready; });
        }
        engine.reset(); // Engine::~Engine retires nesting/bitmap before document members are destroyed.
        wake.notify_all();
    }
    bool start() noexcept {
        started = native.start();
        if (!started) return false;
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return initialized; });
        return !construction_failed;
    }
    static EngineEvent accepted(std::string const &id) {
        return {EngineEventKind::Accepted, id, 1, {}};
    }
    static std::string internal_request_id() {
        auto uuid = g_uuid_string_random();
        std::string id = uuid;
        g_free(uuid);
        return id;
    }
public:
    StructuredEngine(SessionOptions opts, EngineEventSink event_sink)
        : options(std::move(opts)), sink(std::move(event_sink)), native([this] { run(); }) {}
    ~StructuredEngine() override { close(); }
    bool initialize() noexcept { return start(); }
    Submission submit(Request const &request) override {
        if (close_requested || session_closed || delivery_retired || delivery_failed || !engine)
            return {false, refusal("session-required", "The agent session is closed.")};
        bool immediate = request.command == "session.status" || request.command == "session.cancel";
        {
            std::lock_guard lock(engine->state);
            if (engine->ids.contains(request.id)) return {false, refusal("duplicate-request-id", "Use a new request id.")};
            if (engine->ids.size() == 100000) return {false, refusal("request-history-full", "Restart the session with new request history.")};
            engine->ids.insert(request.id);
            if (!immediate && !engine->active.empty())
                return {false, refusal("session-busy", "Wait for the active request to finish.")};
            if (!immediate) { engine->active = request.id; engine->phase = "queued"; engine->cancelled = false; }
        }
        emit_sync(accepted(request.id));
        if (immediate) {
            DispatchContext control_context;
            control_context.session_handler = [this](Request const &r) { return engine->control(r); };
            auto record = dispatch(request, control_context);
            auto payload = result_payload(record, request.id);
            EngineEvent event{EngineEventKind::Result, request.id, 2, std::move(payload)};
            emit_sync(event);
        } else {
            std::lock_guard lock(mutex);
            pending = request;
            wake.notify_one();
        }
        return {true, {}};
    }
    Record cancel(std::string const &id) override {
        if (close_requested || delivery_retired || !engine)
            return refusal("session-required", "The agent session is closed.");
        Request request; request.id = internal_request_id(); request.command = "session.cancel"; request.params = {{"id", id}};
        DispatchContext control_context;
        control_context.session_handler = [this](Request const &r) { return engine->control(r); };
        return dispatch(request, control_context);
    }
    boost::json::object status() const override {
        if (close_requested || delivery_retired || !engine) return {{"closed", true}};
        Request request; request.id = internal_request_id(); request.command = "session.status";
        DispatchContext control_context;
        control_context.session_handler = [this](Request const &r) { return engine->control(r); };
        auto record = dispatch(request, control_context);
        auto data = record.data;
        if (engine->intake.error)
            data["startup_error"] = cli_error(engine->intake.error->code, engine->intake.error->message,
                                               engine->intake.error->hint);
        return data;
    }
    void pump() override {
        std::deque<EngineEvent> delivery;
        {
            std::lock_guard lock(mutex);
            if (delivery_retired || delivery_failed) {
                delivery.clear();
            } else delivery.swap(events);
        }
        wake.notify_all();
        for (auto const &event : delivery) {
            if (delivery_retired || delivery_failed) break;
            emit_sync(event);
        }
        if (delivery_failed) close();
    }
    void close() noexcept override {
        {
            std::lock_guard lock(mutex);
            if (!delivery_retired) { delivery_retired = true; events.clear(); }
            close_requested = true;
        }
        if (engine) { engine->cancelled = true; engine->stop = true; }
        {
            std::lock_guard lock(mutex); close_ready = true;
        }
        wake.notify_all();
        if (!in_sink) native.join();
    }
};
}
std::vector<PackageCommand> session_commands() {
    static ParamSpec const cancel[] = {{.key="id", .type=ParamType::Text, .required=true}};
    static ParamSpec const close[] = {{.key="discard", .type=ParamType::Boolean, .default_value="false"}};
    static ParamSpec const release[] = {{.key="tokens", .type=ParamType::List, .required=true}};
    std::vector<PackageCommand> out;
    auto add = [&](std::string_view id, std::span<ParamSpec const> params, object example, bool readonly) {
        ActionSpec spec{.name=id, .mode="session", .summary="Manage the private agent session.", .params=params};
        spec.handler = unavailable; spec.needs_document = false;
        auto scalar = [](char const *type) { return object{{"type", type}}; };
        auto strings = object{{"type", "array"}, {"items", scalar("string")}};
        object properties{{"validation_level", object{{"enum", array{"preflight", "computed"}}}}};
        if (id == "session.status") {
            properties["active_request"] = object{{"anyOf", array{scalar("null"), scalar("string")}}};
            properties["phase"] = scalar("string"); properties["uninterruptible"] = scalar("boolean");
            properties["session_revision"] = scalar("integer");
            properties["document"] = object{{"anyOf", array{scalar("null"), object{{"type", "object"},
                {"additionalProperties", false}, {"required", array{"id", "revision", "read_only", "dirty"}},
                {"properties", object{{"id", scalar("string")}, {"revision", scalar("integer")},
                                      {"read_only", scalar("boolean")}, {"dirty", scalar("boolean")}}}}}}};
            properties["writes_blocked"] = scalar("boolean");
            properties["reconciliation"] = scalar("object");
            properties["retention"] = object{{"type", "object"}, {"additionalProperties", false},
                {"properties", object{{"tokens", strings}, {"roots", scalar("integer")}, {"bytes", scalar("integer")}}}};
        } else if (id == "session.cancel") {
            properties["found"] = scalar("boolean"); properties["id"] = scalar("string");
            properties["cancellation_requested"] = scalar("boolean");
        } else if (id == "session.release") {
            properties["released"] = strings; properties["not_found"] = strings;
        } else { properties["closed"] = scalar("boolean"); properties["persisted"] = scalar("boolean"); }
        object data{{"type", "object"}, {"properties", properties}, {"additionalProperties", false}};
        out.push_back({spec, id, readonly ? "read-only" : "session-state", "S", example, data,
                       {"session-required", "session-busy", "duplicate-request-id", "request-history-full", "dirty-document", "reconciliation-required"}, {}});
    };
    add("session.status", {}, {}, true);
    add("session.cancel", cancel, {{"id", "request-1"}}, false);
    add("session.release", release, {{"tokens", array{"token-1"}}}, false);
    add("session.close", close, {}, false);
    return out;
}
int run_agent_session(ByteReader read, LineWriter write, SessionOptions options) {
    try { return Engine(std::move(write), std::move(options), true).run(read); }
    catch (...) { return 4; }
}
int run_agent_request(std::string_view bytes, LineWriter write, SessionOptions options) {
    try {
        Engine engine(write, std::move(options));
        auto parsed = parse_production_request(bytes);
        Record record;
        int exit = 0;
        if (parsed.error) {
            record = refusal(parsed.error->code, parsed.error->message, parsed.error_command);
            exit = (parsed.error->code == "malformed-json" || parsed.error->code == "invalid-utf8" ||
                    parsed.error->code == "request-too-large") ? 2 : 3;
        } else if (engine.intake.error) {
            record = refusal(engine.intake.error->code, engine.intake.error->message, parsed.request->command); exit = 3;
        } else { record = engine.execute(*parsed.request); exit = typed_exit_status(record.status); }
        auto result = engine.result(record, parsed.request ? parsed.request->id : parsed.error_id, 1);
        if (engine.intake.error) result["startup_error"] = cli_error(engine.intake.error->code, engine.intake.error->message, engine.intake.error->hint);
        auto line = serialize(result) + "\n";
        if (line.size() > response_limit) {
            result = engine.result(oversized(record), parsed.request ? parsed.request->id : parsed.error_id, 1);
            line = serialize(result) + "\n"; exit = 4;
        }
        return write(line) ? exit : 2;
    } catch (...) { return 4; }
}
}

namespace Inkscape::VACardsCli {
void with_cli_engine_for_testing(SessionOptions options,
    std::function<void(DispatchContext &, FileState &, TokenStore &,
                       std::function<Record(Request const &)> const &)> const &test) {
    Engine engine([](std::string_view) { return true; }, std::move(options), true);
    if (engine.intake.error) throw std::runtime_error(engine.intake.error->message);
    test(engine.context, engine.files, engine.tokens,
         [&engine](Request const &request) { return engine.execute(request); });
}
StructuredSessionResult make_structured_session(SessionOptions options, EngineEventSink sink) {
    if (!sink)
        return {{}, ParseError{"invalid-argument", {}, "A structured session needs an event sink."}};
    try {
        auto engine = std::make_unique<StructuredEngine>(std::move(options), std::move(sink));
        if (!engine->initialize()) {
            engine->close();
            return {{}, ParseError{"internal-error", {}, "Structured session initialization failed."}};
        }
        return {std::move(engine), {}};
    } catch (...) {
        return {{}, ParseError{"internal-error", {}, "Structured session initialization failed."}};
    }
}
}
