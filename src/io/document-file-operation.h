// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Owner-thread admission, binding and lifetime for one explicit file operation
 * per document (F6a).
 *
 * The explicit Save As and Save Copy window actions hold this guard across their asynchronous
 * chooser and check the binding before output. Ordinary GUI Save holds it
 * across synchronous publication. Other GUI file actions and save-on-close
 * are not yet migrated. The core/CLI
 * `Inkscape::Extension::save` path never uses it.
 *
 * Owner-thread contract
 * ---------------------
 * Every function here runs on the native application thread. A guard may live
 * across GTK chooser turns, but its admission, output validation and completion
 * calls are synchronous; this component starts no worker, timer, event pump or
 * deferred save queue.
 *
 * Registered-document precondition
 * --------------------------------
 * `admit()` accepts only an SPDocument that is currently registered with
 * `InkscapeApplication::get_documents()` and is not already pending close. The
 * synchronous lease is `DocumentUndo::holdInteractionOperation()`, which defers
 * closing a registered document but does NOT own a private `unique_ptr`. A
 * private/unregistered document must be rejected, because its lifetime is its
 * owner's responsibility, not this lease's.
 *
 * Guard lifetime
 * --------------
 * `FileOperationLease` is the outer RAII guard. It owns the request and the
 * operation lease for the whole admitted lifetime. Destroying the guard makes it
 * logically inactive immediately, so weak contexts and `authorizeOutput()`
 * reject further work. The single internal per-document registry slot and the
 * document operation lease are held past that point until every active callback
 * stack and every copied/moved callable capture has unwound (an internal
 * synchronous dispatch counter). Only then are the stored predicate/notification
 * captures destroyed under the still-held slot and lease, the exact registry
 * slot erased, and finally the lease released. `~FileOperationLease()` performs
 * cleanup only; it never invokes a callback, pumps events or queues work.
 *
 * Result handling
 * ---------------
 * Completion is explicit and first-wins. `complete()` marks the request terminal
 * before invoking the notification, so duplicate or late completion neither
 * overwrites the result nor sends a second callback. `Uncertain` is a distinct
 * outcome from `Failed`; both retain owned detail text exactly. A throwing
 * terminal listener never rewrites the latched outcome; its diagnostic is kept
 * separately and is observable through `notificationError()`.
 *
 * Weak context
 * ------------
 * `FileOperationContext` is a copyable, non-owning token. It never keeps the
 * request or document alive and cannot pin the lease. The request additionally
 * tracks `SPDocument::connectDestroy()` so a revert/swap or application
 * teardown that frees a registered document invalidates the context and every
 * gate without dereferencing the freed document. After the guard is destroyed
 * an escaped context fails `valid()` immediately, even while internal dispatch
 * pins keep the request memory alive. `authorizeOutput()` demands that the
 * exact context request is the active, nonterminal registry slot for the
 * correct, still-living document before it revalidates the caller's binding
 * predicate and strict output readiness.
 */
#ifndef INKSCAPE_IO_DOCUMENT_FILE_OPERATION_H
#define INKSCAPE_IO_DOCUMENT_FILE_OPERATION_H

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

class SPDocument;

namespace Inkscape::IO {

/** Explicit save intent carried by an admitted request. */
enum class FileOperationMethod { Save, SaveAs, SaveCopy };

/** Structured refusal/terminal outcomes. Published older and uncertain output
 * are never folded into a definite failure. */
enum class FileOperationOutcome {
    Success,     ///< request succeeded, or (for authorizeOutput) output may proceed
    SavedOlderRevision, ///< completed older snapshot published; live edits remain unsaved
    Cancelled,   ///< user/host cancelled the request; nothing was published
    Failed,      ///< definite failure, including a throwing binding predicate
    Busy,        ///< another request occupies the slot, or the document is not ready
    StaleTarget, ///< exact context/binding no longer identifies a valid target
    Pending,     ///< request/close intent is waiting; never means saved
    Uncertain,   ///< output was attempted and its outcome cannot be proven
};

enum class FileOperationRefusal { None, NotReady };

/** Owned, stable result description retained exactly by the first completion. */
struct FileOperationResult {
    FileOperationOutcome outcome = FileOperationOutcome::Failed;
    std::string detail;
    FileOperationRefusal refusal = FileOperationRefusal::None;
};

inline char const *to_string(FileOperationOutcome outcome) noexcept
{
    switch (outcome) {
    case FileOperationOutcome::Success:     return "Success";
    case FileOperationOutcome::SavedOlderRevision: return "SavedOlderRevision";
    case FileOperationOutcome::Cancelled:   return "Cancelled";
    case FileOperationOutcome::Failed:      return "Failed";
    case FileOperationOutcome::Busy:        return "Busy";
    case FileOperationOutcome::StaleTarget: return "StaleTarget";
    case FileOperationOutcome::Pending:     return "Pending";
    case FileOperationOutcome::Uncertain:   return "Uncertain";
    }
    return "Unknown";
}

/**
 * Caller-owned request input, copied into the request at admission and never
 * mutated afterwards. Packet B fills `target`/`context` from the originating
 * action instead of retaining dialog stack variables.
 */
struct FileOperationRequestInfo {
    FileOperationMethod method = FileOperationMethod::Save;
    std::string target;
    std::string context;
};

struct FileOperationRequest; // opaque; defined in the .cpp

class FileOperationLease;
class DocumentFileOperation;

/**
 * Copyable weak token identifying one admitted request. It owns nothing and is
 * invalid once the owning guard is destroyed.
 */
class FileOperationContext {
public:
    FileOperationContext() = default;

    /**
     * False once the guard is destroyed or its document is destroyed, even if
     * internal dispatch pins still keep the request memory alive. Never
     * dereferences the document.
     */
    bool valid() const noexcept;

    /** Owned metadata copy while the guard is alive, otherwise nullopt. */
    std::optional<FileOperationRequestInfo> info() const;

private:
    friend class FileOperationLease;
    friend class DocumentFileOperation;
    SPDocument *_document = nullptr;
    std::weak_ptr<FileOperationRequest> _request;
};

/**
 * Outer synchronous guard. Move-only; owns the request and its operation lease
 * for the entire admitted lifetime.
 */
class FileOperationLease {
public:
    FileOperationLease() noexcept = default;
    FileOperationLease(FileOperationLease &&other) noexcept;
    FileOperationLease &operator=(FileOperationLease &&other) noexcept;
    FileOperationLease(FileOperationLease const &) = delete;
    FileOperationLease &operator=(FileOperationLease const &) = delete;
    ~FileOperationLease() noexcept;

    explicit operator bool() const noexcept { return static_cast<bool>(_request); }

    FileOperationContext context() const noexcept;
    void enterPublication();

    /**
     * Before-output gate for this admitted request. Requires the exact context
     * to identify this nonterminal request and this document, invokes the stored
     * read-only binding predicate, then requires strict output readiness.
     * Returns Success only when output may proceed; any other outcome is a
     * refusal to report (StaleTarget/Busy/Failed). A newly pending close intent
     * is permitted to wait; it is never treated as saved.
     */
    FileOperationResult authorizeOutput(FileOperationContext const &context);

    /**
     * Explicit, first-wins terminal completion. Marks the request terminal
     * before invoking `notify`; duplicate/late calls do nothing. Exceptions from
     * `notify` are contained and recorded as a separate notification diagnostic;
     * they never overwrite an already latched `Success`/`Uncertain`/`Failed`
     * result and never escape. The callback capture is destroyed later, under the
     * guard cleanup.
     */
    void complete(FileOperationResult result,
                  std::function<void(FileOperationResult const &)> notify = {});

    bool terminal() const noexcept;

    /** Last terminal result if one was set, otherwise nullopt. */
    std::optional<FileOperationResult> terminalResult() const;

    /**
     * Owned diagnostic describing a throwing terminal notification, empty when
     * the notification completed normally or has not run. It never changes the
     * latched terminal result.
     */
    std::string const &notificationError() const noexcept;

private:
    friend class DocumentFileOperation;
    explicit FileOperationLease(std::shared_ptr<FileOperationRequest> request) noexcept
        : _request(std::move(request))
    {}

    void cleanup() noexcept;
    std::shared_ptr<FileOperationRequest> _request;
};

/**
 * Owner-thread-only admission into the single internal per-document registry.
 * The registry does not own documents and clears the exact raw-key slot before
 * the operation lease is released.
 */
class DocumentFileOperation {
public:
    DocumentFileOperation() = delete;
    static std::optional<FileOperationOutcome> lastTerminalOutcomeForTesting(SPDocument const &document);

    /**
     * Fresh admission. On success returns a guard owning the slot and lease. On
     * refusal returns nullopt and, when `refusal` is non-null, a rich outcome:
     * Failed for a missing predicate or an unregistered document, Busy for an
     * occupied/unready/pending-close document, StaleTarget when the read-only
     * binding predicate rejects the target. Slot+lease reservation is admission;
     * a close that becomes pending inside the initial binding callback is a new
     * close intent and is permitted as long as the exact request survives and
     * strict owned-output readiness holds. A close already pending before
     * reservation is rejected. There is no default binding predicate and no
     * context/depth reuse: every call is a fresh request.
     */
    static std::optional<FileOperationLease>
    admit(SPDocument &document, FileOperationRequestInfo info,
          std::function<bool()> binding, FileOperationResult *refusal = nullptr);

    /** Read-only: true while any request (terminal or not) occupies the slot. */
    static bool hasActiveRequest(SPDocument const &document) noexcept;
};

} // namespace Inkscape::IO

#endif // INKSCAPE_IO_DOCUMENT_FILE_OPERATION_H
