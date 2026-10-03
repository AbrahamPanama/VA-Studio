// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Admission, binding, output gate and terminal lifetime for one explicit file
 * operation per document (F6a Packet A). See document-file-operation.h for the
 * full contract. Not wired into any caller and not part of the CLI save path.
 */

#include "io/document-file-operation.h"

#include <algorithm>
#include <exception>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <sigc++/connection.h>

#include "document-undo.h"
#include "document.h"
#include "inkscape-application.h"
#include "io/stream/bufferstream.h"
#include "xml/document.h"

namespace Inkscape::IO {

/**
 * One admitted request. Owns its immutable input metadata, the caller's
 * read-only binding predicate, the terminal result and the synchronous
 * operation lease. It never owns the document.
 */
struct FileOperationRequest {
    FileOperationRequest(SPDocument *document, FileOperationRequestInfo input)
        : document(document), info(std::move(input))
    {}

    SPDocument *document;               ///< registered, non-owning; protected by the lease
    FileOperationRequestInfo const info; ///< immutable owned request input
    std::function<bool()> binding;       ///< caller-owned read-only binding predicate
    bool terminal = false;
    // The lease defers ordinary close, but a revert/swap or application teardown
    // can still destroy a registered document. connectDestroy() flips this flag
    // so no gate ever dereferences a freed SPDocument.
    bool document_alive = true;
    // Logical guard ownership. Cleared as soon as the outer guard starts
    // cleanup, so contexts and output authorization reject immediately. The
    // registry slot and lease are not released until dispatch_depth reaches zero.
    bool guard_alive = true;
    // Active synchronous callback stacks plus every copied/moved callable capture
    // they created. Keeps the slot+lease alive across a guard destroyed from
    // inside its own binding/notification callback.
    int dispatch_depth = 0;
    // Single-shot teardown latch: a reentrant cleanup from a capture destructor
    // must not clear the same callable/lease twice.
    bool teardown_done = false;
    sigc::connection document_destroy;
    FileOperationResult result;
    std::function<void(FileOperationResult const &)> notify;
    std::string notification_error;      ///< separate diagnostic for a throwing listener
    std::shared_ptr<void> lease;         ///< DocumentUndo interaction operation scope
};

} // namespace Inkscape::IO

namespace {

using RequestPtr = std::shared_ptr<Inkscape::IO::FileOperationRequest>;
using Slot = std::weak_ptr<Inkscape::IO::FileOperationRequest>;

// The single owner-thread registry shared by every admission. It deliberately
// does not own documents; the slot is a weak reference so only the guard keeps
// the request alive. Leaked at exit so static teardown can never outlive a
// document or reenter application signals.
std::unordered_map<SPDocument *, Slot> &registry()
{
    static auto *map = new std::unordered_map<SPDocument *, Slot>();
    return *map;
}

std::unordered_map<SPDocument const *, Inkscape::IO::FileOperationOutcome> &terminalOutcomesForTesting()
{
    static auto *outcomes = new std::unordered_map<SPDocument const *, Inkscape::IO::FileOperationOutcome>();
    return *outcomes;
}

RequestPtr activeRequest(SPDocument *document)
{
    if (!document) return {};
    auto const it = registry().find(document);
    return it == registry().end() ? RequestPtr{} : it->second.lock();
}

bool isActiveSlot(RequestPtr const &request)
{
    return request && activeRequest(request->document).get() == request.get();
}

void eraseSlot(RequestPtr const &request)
{
    if (!request) return;
    auto const it = registry().find(request->document);
    if (it != registry().end() && it->second.lock().get() == request.get()) {
        registry().erase(it);
    }
}

// Single-shot teardown, run only once dispatch_depth is zero and the guard has
// relinquished logical ownership. Order is fixed and enforced by this function:
// while the registry slot is still occupied and the document operation lease is
// still held, destroy the stored binding/notification captures (their
// destructors may reenter admission/close and must observe Busy), then erase the
// exact raw-key slot, then release the lease. teardown_done stops a reentrant
// cleanup from clearing the same callable or lease twice.
void teardownRequest(RequestPtr const &request) noexcept
{
    if (!request || request->teardown_done) return;
    request->teardown_done = true;
    request->document_destroy.disconnect();
    request->binding = nullptr;
    request->notify = nullptr;
    eraseSlot(request);
    request->lease.reset();
}

void maybeTeardown(RequestPtr const &request) noexcept
{
    if (!request || request->guard_alive || request->dispatch_depth != 0) return;
    teardownRequest(request);
}

// RAII synchronous dispatch pin. Incrementing dispatch_depth keeps the slot and
// lease alive past a guard destroyed inside the callback. Declared before any
// local copy of the callable so the copy is destroyed while the pin is still
// active.
class DispatchScope {
public:
    explicit DispatchScope(RequestPtr request) noexcept : _request(std::move(request))
    {
        if (_request) ++_request->dispatch_depth;
    }
    DispatchScope(DispatchScope const &) = delete;
    DispatchScope &operator=(DispatchScope const &) = delete;
    ~DispatchScope() noexcept
    {
        if (_request) {
            --_request->dispatch_depth;
            maybeTeardown(_request);
        }
    }

private:
    RequestPtr _request;
};

// RAII admission reservation. Owns cleanup from the moment the slot/lease are
// reserved until the guard takes over via release(). Any exception or early
// refusal unwinds through here, so no occupied/expired registry entry or
// out-of-order lease release can survive a failed admission.
class AdmissionReservation {
public:
    explicit AdmissionReservation(RequestPtr request) noexcept : _request(std::move(request)) {}
    AdmissionReservation(AdmissionReservation const &) = delete;
    AdmissionReservation &operator=(AdmissionReservation const &) = delete;
    ~AdmissionReservation() noexcept
    {
        if (_request) {
            _request->guard_alive = false;
            maybeTeardown(_request);
        }
    }
    RequestPtr release() noexcept { return std::move(_request); }

private:
    RequestPtr _request;
};

bool isRegisteredDocument(SPDocument *document)
{
    auto *app = InkscapeApplication::instance();
    if (!app || !document) return false;
    auto const documents = app->get_documents();
    return std::find(documents.begin(), documents.end(), document) != documents.end();
}

// Outcome-preserving result construction that never lets a diagnostic allocation
// failure escape. Recovery from global OOM is not promised; the outcome and the
// ownership ordering are.
Inkscape::IO::FileOperationResult makeResult(Inkscape::IO::FileOperationOutcome outcome,
                                             char const *detail) noexcept
{
    Inkscape::IO::FileOperationResult result;
    result.outcome = outcome;
    if (detail) {
        try {
            result.detail = detail;
        } catch (...) {
        }
    }
    return result;
}

Inkscape::IO::FileOperationResult makeExceptionResult(Inkscape::IO::FileOperationOutcome outcome,
                                                      char const *prefix, char const *what) noexcept
{
    Inkscape::IO::FileOperationResult result;
    result.outcome = outcome;
    try {
        result.detail = prefix ? prefix : "";
        if (what) result.detail += what;
    } catch (...) {
    }
    return result;
}

// Best-effort owned notification diagnostic. It never escapes and never touches
// the latched terminal result.
void recordNotificationError(RequestPtr const &request, char const *prefix, char const *what) noexcept
{
    if (!request) return;
    try {
        request->notification_error = prefix ? prefix : "";
        if (what) request->notification_error += what;
    } catch (...) {
    }
}

// Copy the predicate before invoking it: a reentrant guard teardown cannot
// destroy the executing callable, and an exception becomes a Failed refusal.
// DispatchScope outlives the copied predicate so a guard destroyed by the
// predicate keeps the slot+lease until the copy is also gone.
Inkscape::IO::FileOperationResult invokeBinding(RequestPtr const &request)
{
    using Inkscape::IO::FileOperationOutcome;
    if (!request) {
        return makeResult(FileOperationOutcome::StaleTarget, "request no longer exists");
    }
    DispatchScope dispatch(request);
    try {
        auto predicate = request->binding;
        if (!predicate) {
            return makeResult(FileOperationOutcome::StaleTarget, "request has no binding predicate");
        }
        if (!predicate()) {
            return makeResult(FileOperationOutcome::StaleTarget, "binding predicate rejected the target");
        }
    } catch (std::exception const &e) {
        return makeExceptionResult(FileOperationOutcome::Failed, "binding predicate threw: ", e.what());
    } catch (...) {
        return makeResult(FileOperationOutcome::Failed, "binding predicate threw an unknown exception");
    }
    return makeResult(FileOperationOutcome::Success, "");
}

} // namespace

namespace Inkscape::IO {

bool FileOperationContext::valid() const noexcept
{
    // Only the weak request is inspected; the raw document pointer is never
    // dereferenced here. guard_alive is checked separately so an escaped
    // context is invalid as soon as the guard dies, even while dispatch pins
    // still keep the request memory alive.
    if (!_document) return false;
    auto alive = _request.lock();
    return alive && alive->guard_alive && alive->document_alive;
}

std::optional<FileOperationRequestInfo> FileOperationContext::info() const
{
    auto alive = _request.lock();
    if (!alive || !alive->guard_alive) return std::nullopt;
    return alive->info;
}

FileOperationLease::FileOperationLease(FileOperationLease &&other) noexcept
    : _request(std::move(other._request))
{}

FileOperationLease &FileOperationLease::operator=(FileOperationLease &&other) noexcept
{
    if (this != &other) {
        cleanup();
        _request = std::move(other._request);
    }
    return *this;
}

FileOperationLease::~FileOperationLease() noexcept
{
    cleanup();
}

void FileOperationLease::cleanup() noexcept
{
    // Become logically inactive first so escaped contexts and authorizeOutput()
    // reject immediately, then defer the slot/lease teardown until every active
    // dispatch scope has unwound.
    auto request = std::move(_request);
    if (!request) return;
    request->guard_alive = false;
    maybeTeardown(request);
}

FileOperationContext FileOperationLease::context() const noexcept
{
    FileOperationContext token;
    if (_request) {
        token._document = _request->document;
        token._request = _request;
    }
    return token;
}

void FileOperationLease::enterPublication()
{
    g_assert(_request && _request->guard_alive && _request->document_alive && !_request->terminal);
    auto publication = DocumentUndo::holdPublication(_request->document);
    g_assert(publication);
    _request->lease.swap(publication); // Release operation scope after publication is held.
}

FileOperationResult FileOperationLease::authorizeOutput(FileOperationContext const &context)
{
    // Pin the request so a callback that tears down this guard cannot free it
    // under us. Never touch `this` after this point: the guard may be dead.
    auto request = _request;
    if (!request) {
        return makeResult(FileOperationOutcome::StaleTarget, "file operation is no longer active");
    }
    if (!request->guard_alive) {
        return makeResult(FileOperationOutcome::StaleTarget, "file operation guard is no longer active");
    }
    if (!request->document_alive) {
        return makeResult(FileOperationOutcome::StaleTarget, "document was destroyed before output");
    }
    auto const alive = context._request.lock();
    if (!alive || alive.get() != request.get()) {
        return makeResult(FileOperationOutcome::StaleTarget, "context does not identify the active request");
    }
    if (context._document != request->document) {
        return makeResult(FileOperationOutcome::StaleTarget, "context belongs to a different document");
    }
    if (request->terminal) {
        return makeResult(FileOperationOutcome::StaleTarget, "request is already terminal");
    }
    if (!isActiveSlot(request)) {
        return makeResult(FileOperationOutcome::StaleTarget, "request no longer owns the document slot");
    }

    auto const binding = invokeBinding(request);
    if (binding.outcome != FileOperationOutcome::Success) return binding;

    // The read-only predicate may have run nested callbacks and may even have
    // destroyed the guard. Recheck the pinned token before dereferencing the
    // document; a dead guard or released slot rejects without touching it.
    if (!request->guard_alive || request->terminal || !isActiveSlot(request)) {
        return makeResult(FileOperationOutcome::StaleTarget, "request changed during binding validation");
    }
    if (!request->document_alive) {
        return makeResult(FileOperationOutcome::StaleTarget, "document was destroyed during binding validation");
    }
    if (!DocumentUndo::fileOperationOutputReady(request->document)) {
        return makeResult(FileOperationOutcome::Busy, "document is not ready for output");
    }
    return makeResult(FileOperationOutcome::Success, "");
}

void FileOperationLease::complete(FileOperationResult result,
                                  std::function<void(FileOperationResult const &)> notify)
{
    // Pin local state; a notification may destroy this guard, so `this` must
    // never be dereferenced after the callback starts.
    auto request = _request;
    if (!request) return;
    if (request->terminal) {
        // Duplicate completion: preserve the latched result/detail and never
        // invoke the incoming listener, but pin the slot/lease while the
        // incoming by-value callable's captures unwind. If a capture's deleter
        // resets the outer guard, teardown must wait until the capture is gone.
        DispatchScope dispatch(request);
        // Swap into an empty local rather than move: libc++'s std::function
        // move constructor clones a small (SBO) callable and leaves the source
        // pointing at its own buffer (MacOSX.sdk
        // usr/include/c++/v1/__functional/function.h:390-398), so a move would
        // leave the incoming parameter's captures to unwind after the pin.
        // Swapping with an empty function empties the source exactly.
        std::function<void(FileOperationResult const &)> callback;
        callback.swap(notify); // declared after the pin; notify is left empty
        notify = nullptr;      // normally empty; disposes under the active pin
        (void)callback;        // destructs before dispatch releases
        return;                // no latch/invoke/diagnostic
    }
    request->terminal = true;                  // marked before any callback
    request->result = std::move(result);
    if (file_io_test_hooks_enabled() && request->document_alive)
        terminalOutcomesForTesting()[request->document] = request->result.outcome;
    // Pin before transferring ownership so every capture disposes under it.
    DispatchScope dispatch(request);
    // Swap rather than move: a small callable would be cloned and left behind
    // in the source (function.h:390-398), so its captures could outlive the pin.
    request->notify.swap(notify);
    std::function<void(FileOperationResult const &)> callback;
    callback.swap(request->notify);            // stored ownership now empty
    notify = nullptr;                          // dispose any replaced capture
    if (!callback) return;

    try {
        callback(request->result);
    } catch (std::exception const &e) {
        // Preserve the latched outcome/detail exactly; record a separate
        // diagnostic for the throwing listener instead of rewriting the result.
        recordNotificationError(request, "completion notification threw: ", e.what());
    } catch (...) {
        recordNotificationError(request, "completion notification threw an unknown exception", nullptr);
    }
}

bool FileOperationLease::terminal() const noexcept
{
    auto const request = _request;
    return request && request->terminal;
}

std::optional<FileOperationResult> FileOperationLease::terminalResult() const
{
    auto const request = _request;
    // A request that exists but has not completed has no terminal result.
    if (!request || !request->terminal) return std::nullopt;
    return request->result;
}

std::string const &FileOperationLease::notificationError() const noexcept
{
    static std::string const empty;
    auto const request = _request;
    return request ? request->notification_error : empty;
}

std::optional<FileOperationLease>
DocumentFileOperation::admit(SPDocument &document, FileOperationRequestInfo info,
                             std::function<bool()> binding, FileOperationResult *refusal)
{
    // Refusal helper is noexcept: a diagnostic allocation failure must not
    // escape admission or leave the reservation behind. The outcome is kept.
    auto refuse = [refusal](FileOperationOutcome outcome, std::string_view detail,
                            FileOperationRefusal code = FileOperationRefusal::None) noexcept {
        if (refusal) {
            refusal->outcome = outcome;
            refusal->refusal = code;
            try {
                refusal->detail.assign(detail);
            } catch (...) {
                try {
                    refusal->detail.clear();
                } catch (...) {
                }
            }
        }
        return std::optional<FileOperationLease>{};
    };

    if (!binding) {
        return refuse(FileOperationOutcome::Failed, "a read-only binding predicate is required");
    }

    auto *app = InkscapeApplication::instance();
    if (!app || !isRegisteredDocument(&document)) {
        return refuse(FileOperationOutcome::Failed, "document is not registered with the application");
    }
    if (app->documentClosePending(&document)) {
        return refuse(FileOperationOutcome::Busy, "document close is already pending");
    }
    if (activeRequest(&document)) {
        return refuse(FileOperationOutcome::Busy, "another file operation is active for this document");
    }
    if (!DocumentUndo::fileOperationFreshReady(&document)) {
        return refuse(FileOperationOutcome::Busy, "document is not ready for a fresh file operation",
                      FileOperationRefusal::NotReady);
    }

    // Reserve the registry slot and the synchronous lease before running the
    // binding predicate, so nested callbacks observe a Busy slot. From here on
    // the reservation owns cleanup until it is released to the guard, so any
    // exception or early refusal tears down slot+lease in the right order.
    auto request = std::make_shared<FileOperationRequest>(&document, std::move(info));
    if (file_io_test_hooks_enabled()) terminalOutcomesForTesting().erase(&document);
    request->binding = std::move(binding);
    request->lease = DocumentUndo::holdInteractionOperation(&document);
    if (!request->lease) {
        return refuse(FileOperationOutcome::Busy, "could not acquire a document operation lease");
    }
    request->guard_alive = true;
    AdmissionReservation reservation(request);
    registry()[&document] = request;
    request->document_destroy = document.connectDestroy(
        [weak = std::weak_ptr<FileOperationRequest>(request)] {
            if (auto alive = weak.lock()) {
                alive->document_alive = false;
                // Drop the raw-key slot before the address can be reused. The
                // guard still owns the request/lease for terminal bookkeeping.
                eraseSlot(alive);
                if (file_io_test_hooks_enabled()) terminalOutcomesForTesting().erase(alive->document);
            }
        });

    auto const binding_result = invokeBinding(request);
    if (binding_result.outcome != FileOperationOutcome::Success) {
        return refuse(binding_result.outcome, binding_result.detail);
    }

    // Recheck liveness, exact slot and strict owned-output readiness. A close
    // that became pending inside the binding predicate is a new close intent
    // and is permitted to wait; fileOperationOutputReady rejects any foreign or
    // missing lease. Never touch the document after destruction.
    if (!request->document_alive) {
        return refuse(FileOperationOutcome::StaleTarget, "document was destroyed while binding");
    }
    if (!isActiveSlot(request)) {
        return refuse(FileOperationOutcome::Busy, "document request changed while binding");
    }
    if (!DocumentUndo::fileOperationOutputReady(&document)) {
        return refuse(FileOperationOutcome::Busy, "document became unready while binding",
                      FileOperationRefusal::NotReady);
    }

    return std::optional<FileOperationLease>{FileOperationLease(reservation.release())};
}

bool DocumentFileOperation::hasActiveRequest(SPDocument const &document) noexcept
{
    return static_cast<bool>(activeRequest(const_cast<SPDocument *>(&document)));
}

std::optional<FileOperationOutcome> DocumentFileOperation::lastTerminalOutcomeForTesting(
    SPDocument const &document)
{
    if (!file_io_test_hooks_enabled()) return std::nullopt;
    auto const it = terminalOutcomesForTesting().find(&document);
    if (it == terminalOutcomesForTesting().end()) return std::nullopt;
    return it->second;
}

} // namespace Inkscape::IO
