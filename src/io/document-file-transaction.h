// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * New-file staging and no-clobber publication for document saves.
 *
 * This is the bounded F3a "newfile" packet. It is deliberately NOT the full
 * replacement transaction: there is no metadata/ACL/xattr preservation, no
 * symlink or hard-link replacement policy, no document identity, and no
 * ExistingFile mode. The factory creates NewFile targets only and never
 * replaces an existing destination. Existing-file replacement is a later
 * packet.
 *
 * Contract summary:
 *  - The caller chooses the logical final name; the random staging name is
 *    never parsed for format/extension decisions.
 *  - Target validation is exact-spelling and platform-specific: all three
 *    strings must be free of embedded NUL, parent_dir/final_path must be
 *    absolute, final_name must be a basename, and final_path must be
 *    byte-exactly parent_dir + one separator (omitted when parent_dir already
 *    ends in one) + final_name. On POSIX the separator is '/' and a backslash
 *    is an ordinary byte; on Windows either slash is accepted at the boundary
 *    but the parent bytes are preserved. The text is never canonicalized and
 *    symlinks are NOT resolved. This proves the stated parent/name spelling,
 *    not the identity of the parent directory: an already-symlinked parent, or
 *    a directory component replaced between validation and publication, is
 *    outside this guarantee. The caller owns that check-to-publish race.
 *  - Staging is an exclusively created sibling with a fixed short, non-dot
 *    component (secure mkstemp, writable binary FILE).
 *  - The writer borrows the staged FILE. It must serialize completely through
 *    the existing checked completion (FileOutputStream close/flush) and must
 *    NOT fclose() or fsync() it. `seal()` then owns the final flush -> fd
 *    sync -> exactly-once close and records the real file-sync capability.
 *  - Publication is no-clobber. A destination that exists (including one that
 *    appears after staging) is an explicit Conflict and its bytes are
 *    untouched. There is never a plain rename, delete, or copy fallback.
 *  - The outcome is latched before the publication syscall. An ambiguous
 *    publication returns Uncertain and retains the identified staging copy for
 *    recovery; it is never reported as a pre-commit failure.
 *  - Stage cleanup is a single shared decision: the platform adapter only
 *    creates the no-clobber namespace entry and never unlinks; the transaction
 *    performs exactly one cleanup, consuming ownership so a file later recreated
 *    at the old staging path is left alone.
 *  - Parent-directory sync (namespace durability) and file sync are separate
 *    reported capabilities and never turn a published result into a failure.
 *  - Failure and cancellation are terminal and sticky: the first failure reason
 *    survives repeated write/seal/publish with no second flush/close, and
 *    publish is refused after abort or a failed write. Repeated publish returns
 *    the complete originally recorded result and never reissues a syscall.
 *  - Fault injection is compile-time test code by subclassing `SystemCalls`.
 *    Production selection is by `make_platform_system_calls()` only; there is
 *    no environment-variable or runtime backdoor.
 */
#ifndef INKSCAPE_IO_DOCUMENT_FILE_TRANSACTION_H
#define INKSCAPE_IO_DOCUMENT_FILE_TRANSACTION_H

#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <cstddef>
#include <string>
#include <vector>
#include <stdexcept>
#include <utility>

namespace Inkscape::IO::DocumentTransaction {

/**
 * The user-visible destination plus the logical name chosen by the caller.
 *
 * `final_name` is used for extension/format/compression and relative-link
 * decisions. It is NEVER derived from the staging path and the transaction
 * never rewrites it with the random staging suffix.
 *
 * `parent_dir` must be the absolute directory that hosts the sibling staging
 * file, and `final_path` must be the lexical join of `parent_dir` and
 * `final_name` (see the file comment for the exact, symlink-free rule).
 * Relative or pipe (`-`) targets are rejected so that the staging sibling
 * cannot silently land in a different directory.
 */
struct LogicalTarget {
    std::string final_path; ///< full user-visible destination
    std::string final_name; ///< basename exactly as chosen by the caller
    std::string parent_dir; ///< directory that hosts the sibling staging file
};

/** Result of the publication syscall for a NewFile target. */
enum class PublicationStatus {
    NotAttempted, ///< no publication has been started
    Published,    ///< the new namespace entry is confirmed present
    Conflict,     ///< destination already existed; its bytes are untouched
    Failed,       ///< publication definitively did not apply
    Uncertain,    ///< publication was attempted; the outcome cannot be proven
    Unsupported,  ///< the volume cannot provide the publication primitive;
                  ///< no namespace entry was created
};

/**
 * Read-only classification of the recorded staging object at its exact owned
 * path. This is an identity/availability signal only: it never proves the bytes
 * of the staged file. `Unverified` is the default and must remain visible; a
 * caller may not present it as verified recovery.
 */
enum class StagingAvailability {
    Unverified, ///< identity was not (or could not be) re-checked
    Available,  ///< the recorded identity is still present at the exact path
    Missing,    ///< identity was checked; no entry exists at the exact path
    Changed,    ///< identity was checked; a different/reparse object is there
};

/** Coarse failure classification for callers and tests. */
enum class FailureKind {
    None,
    InvalidArgument,      ///< bad target, NUL, pipe target, relative/mismatched path
    Unsupported,          ///< capability gap: admission or publication impossible
    InvalidOperation,     ///< write/seal/publish used in a state that forbids it
    Aborted,              ///< terminal cancellation; publication is forbidden
    DestinationExists,    ///< no-clobber violation (Conflict)
    PermissionDenied,
    ReadOnly,
    NotFound,
    SharingViolation,
    StagingCreateFailed,  ///< exclusive sibling could not be created
    StagingWriteFailed,   ///< writer callback failed/aborted/was empty
    StagingFlushFailed,   ///< fflush failed
    StagingSyncFailed,    ///< fsync failed (not merely unsupported)
    StagingCloseFailed,   ///< final fclose failed
    PublicationFailed,    ///< definite publication failure
    PublicationUncertain, ///< ambiguous publication; staging retained
};

/**
 * Explicit publication result. `status` is the namespace outcome. File-sync and
 * parent-directory durability are separate capabilities and never downgrade
 * `status`.
 */
struct PublicationResult {
    PublicationStatus status = PublicationStatus::NotAttempted;
    FailureKind failure = FailureKind::None;
    std::string error;

    /// True whenever the staging file sync was attempted during seal().
    bool file_sync_attempted = false;
    /// False when the platform reported file fsync unsupported (a capability,
    /// not a successful durability claim).
    bool file_sync_supported = false;
    /// True only when file durability actually succeeded.
    bool file_sync_ok = false;

    /// True whenever a parent-sync call was made after a confirmed publish.
    bool parent_sync_attempted = false;
    /// False when the platform reported directory fsync unsupported.
    bool parent_sync_supported = false;
    /// True only when directory durability actually succeeded.
    bool parent_sync_ok = false;

    /// True when a staging copy is deliberately retained for recovery
    /// (Uncertain, or caller-opted Unsupported, with availability
    /// Available/Unverified). Identity alone does not verify its bytes.
    bool recovery_retained = false;
    /// Identified staging/recovery path. Non-empty only while retained.
    std::string recovery_path;
    /// Availability of the recorded staging identity at the exact staging path.
    /// `Available` states the identified object is present; it does NOT prove
    /// the bytes. `Unverified` deliberately stays visible and is never reported
    /// as verified recovery.
    StagingAvailability staging_availability = StagingAvailability::Unverified;

    /// False when the staged sibling could not be unlinked after a confirmed
    /// publish. This is a cleanup detail, not a data-loss or publication
    /// failure.
    bool staging_cleanup_ok = true;
};

inline char const *to_string(PublicationStatus status) noexcept
{
    switch (status) {
    case PublicationStatus::NotAttempted: return "NotAttempted";
    case PublicationStatus::Published: return "Published";
    case PublicationStatus::Conflict: return "Conflict";
    case PublicationStatus::Failed: return "Failed";
    case PublicationStatus::Uncertain: return "Uncertain";
    case PublicationStatus::Unsupported: return "Unsupported";
    }
    return "Unknown";
}

/**
 * Narrow injectable system-call boundary. Production uses the compile-time
 * platform adapter returned by make_platform_system_calls(); tests subclass
 * this to inject faults. Every subclass method must faithfully implement the
 * documented contract: the injected path is the same code path production
 * runs.
 */
class SystemCalls
{
public:
    virtual ~SystemCalls() = default;

    /**
     * Exclusively create the sibling named by `path_template` (which contains
     * an XXXXXX component). On success `path_template` is updated in place to
     * the actual created path and `out` is a writable binary FILE* owned by
     * the caller. `already_exists` distinguishes a staging name collision
     * (retry) from a real error. `error` is human-readable on failure.
     *
     * On failure the adapter closes its owned handles and makes a best-effort
     * cleanup of the created stage. Windows uses the same owned handle for
     * disposal; the existing POSIX adapter uses pathname cleanup and does not
     * claim protection from hostile concurrent replacement. Removal is not
     * guaranteed if the OS refuses it; a staging artifact may remain.
     */
    virtual bool create_exclusive_file(std::string &path_template, FILE *&out,
                                       bool &already_exists, std::string &error) = 0;

    /// Checked flush of buffered data. Does not close.
    virtual bool flush_file(FILE *stream, std::string &error) = 0;

    /// Flush-to-storage where supported. Returns false only on a real failure;
    /// sets `unsupported` when durability is unavailable (not fabricated).
    virtual bool sync_file(FILE *stream, bool &unsupported, std::string &error) = 0;

    /// Final checked close. Never reports success when the close failed.
    /// Ownership passes to this call whether or not it succeeds.
    virtual bool close_file(FILE *stream, std::string &error) = 0;

    /**
     * No-clobber publication of a completed staging file as a NEW destination.
     * MUST NOT replace an existing destination. The adapter ONLY creates the
     * namespace entry (e.g. link()); it MUST NOT unlink the staged path. The
     * exceptions are the SMB paths: the POSIX fallback's rename(2) over its own
     * empty claim, and the Windows no-clobber MoveFileExW on an SMB 2+ share,
     * both consume the stage path; the layer's later cleanup then finds
     * nothing there, and retained_stage_availability() reports it Missing. On
     * Published the entry is confirmed; on Conflict the destination bytes are
     * untouched. Exactly one staging cleanup is owned by the transaction layer.
     */
    virtual PublicationStatus publish_new_file(std::string const &staged_path,
                                               std::string const &final_path,
                                               std::string &error) = 0;

    /**
     * Pre-create admission. The shared state machine calls this after lexical
     * validation and BEFORE any staging file is created. Returning anything
     * other than FailureKind::None rejects the create with that kind, so a
     * platform that cannot host a hard-link publication never writes a payload.
     * Defaults to None, which preserves POSIX and test adapters.
     */
    virtual FailureKind pre_create_support(std::string const &parent_dir, std::string &error)
    {
        (void)parent_dir;
        (void)error;
        return FailureKind::None;
    }

    /**
     * Read-only availability of the recorded staging identity at the exact
     * owned staging path. The shared layer calls this at most once, only when
     * publication is Uncertain, and latches the answer so repeated publish
     * issues no syscall. It MUST NOT remove, rename, or otherwise consume the
     * staging entry. Defaults to Unverified, which preserves POSIX and test
     * adapters and never claims verified recovery.
     */
    virtual StagingAvailability retained_stage_availability(std::string const &staged_path,
                                                            std::string &error)
    {
        (void)staged_path;
        (void)error;
        return StagingAvailability::Unverified;
    }

    /**
     * Best-effort removal used only for staging paths, never the destination.
     * An adapter that records the created staging identity must verify it
     * through the same handle it deletes with and preserve any foreign object
     * at the path. An unknown (never-created) path must not be deleted.
     */
    virtual bool remove_file(std::string const &path) noexcept = 0;

    /// Best-effort parent directory durability. `unsupported` is set when the
    /// platform cannot express it.
    virtual bool sync_parent_directory(std::string const &parent, bool &unsupported,
                                       std::string &error) = 0;
};

/**
 * Compile-time platform adapter selection. Never chosen by an environment
 * variable or runtime flag. On Windows the selected adapter admits local NTFS
 * only and otherwise reports Unsupported before writing any payload.
 */
std::unique_ptr<SystemCalls> make_platform_system_calls();

/**
 * RAII new-file staging transaction.
 *
 * Lifetime: while alive and not published, the destructor removes the staging
 * sibling (never the destination). Once publication has been attempted with an
 * ambiguous outcome the identified staging copy is retained for recovery. An
 * Unsupported publication retains it only when the caller explicitly opts in.
 * A retained stage is never auto-removed. The destructor is noexcept and never
 * repeats publication,
 * and never deletes any original or destination path. Staging cleanup is
 * consumed by the first attempt: a path that is recreated after a successful
 * cleanup is never deleted by the destructor.
 *
 * Construction is only through `create()`, which rejects pipes, invalid
 * basenames, NUL bytes, and relative/mismatched targets before touching the
 * filesystem. There is no ExistingFile factory; replacement is a later packet.
 */
class NewDocumentFile
{
public:
    /// Writer callback over the borrowed staging stream. It must serialize
    /// completely through the existing checked completion and must not
    /// close/sync the stream. It reports failure by returning after a latched
    /// error or by throwing.
    using StageWriter = std::function<void(FILE *staged_stream)>;

    /**
     * Create a NewFile staging sibling. Returns nullptr on failure with
     * `failure`/`error` set and no staging stream exposed. The adapter closes
     * any handle it owns and attempts cleanup under its platform contract,
     * which the OS may refuse (see create_exclusive_file). Never touches
     * `target.final_path`. `retain_on_unsupported` is intended for interactive
     * document saves; background callers retain the default cleanup behavior.
     */
    static std::unique_ptr<NewDocumentFile> create(LogicalTarget target,
                                                   SystemCalls &calls,
                                                   FailureKind &failure,
                                                   std::string &error,
                                                   bool retain_on_unsupported = false,
                                                   std::function<bool(unsigned)> stage_observer = {});

    ~NewDocumentFile();
    NewDocumentFile(NewDocumentFile const &) = delete;
    NewDocumentFile &operator=(NewDocumentFile const &) = delete;
    NewDocumentFile(NewDocumentFile &&) = delete;
    NewDocumentFile &operator=(NewDocumentFile &&) = delete;

    /// Borrowed staging stream; valid until seal()/abort()/destruction.
    FILE *staged_stream() const noexcept { return stream_; }

    /**
     * Run a writer over the borrowed stream. On failure the staging stream is
     * closed exactly once, the failure is latched terminally, and repeated
     * calls return that first failure without reissuing syscalls. An empty
     * callback is a failure, not a no-op. A successful return leaves the stream
     * open for seal(). Calling this after seal/publish/abort is rejected
     * without altering an already recorded publication.
     */
    bool write(StageWriter const &writer, FailureKind &failure, std::string &error);
    bool write_bytes(std::span<std::byte const> bytes, FailureKind &failure, std::string &error,
                     bool inject_byte_write_failure_for_testing = false);
    bool write_bytes(std::vector<std::byte> bytes, FailureKind &failure, std::string &error)
    {
        return write([bytes = std::move(bytes)](FILE *stage) {
            if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), stage) != bytes.size()) {
                throw std::runtime_error("write staging bytes failed");
            }
        }, failure, error);
    }

    /**
     * Final flush -> sync -> close. Exactly-once close. Returns false on the
     * first failure and never reports success afterwards; later calls return
     * the first failure without a second flush/close. `unsupported` sync is a
     * capability, not a failure. The real file-sync capability is reported in
     * the publish result (file_sync_*).
     */
    bool seal(FailureKind &failure, std::string &error);

    /**
     * Publish as a NEW file, no-clobber. Latches Uncertain before the
     * publication call; a collision (including a raced destination) returns
     * Conflict with the destination bytes untouched. Refuses to publish after
     * abort or a failed write. Never repeats publication: a repeated call
     * returns the complete originally recorded result (including parent/file
     * sync and cleanup details) and does not reissue a syscall.
     */
    PublicationResult publish();

    /// Abandon before publication. Terminally latches cancellation, removes
    /// staging only, and never touches the destination. A retained Uncertain
    /// or caller-opted Unsupported staging copy is left in place.
    void abort() noexcept;

    bool sealed() const noexcept { return sealed_; }
    bool publication_attempted() const noexcept { return publication_attempted_; }
    PublicationStatus publication_status() const noexcept { return status_; }
    FailureKind last_failure() const noexcept { return failure_; }
    std::string const &last_error() const noexcept { return error_; }

    bool file_sync_attempted() const noexcept { return file_sync_attempted_; }
    bool file_sync_supported() const noexcept { return file_sync_supported_; }
    bool file_sync_ok() const noexcept { return file_sync_ok_; }

    /// Last-known staging path. Meaningful as a recovery path only while the
    /// status is Uncertain or caller-opted Unsupported (recovery_retained).
    /// After confirmed cleanup it is a stale name and is never used to delete
    /// anything again.
    std::string const &staged_path() const noexcept { return staged_path_; }
    LogicalTarget const &target() const noexcept { return target_; }

private:
    NewDocumentFile(LogicalTarget target, SystemCalls &calls, std::string staged_path,
                    FILE *stream, bool retain_on_unsupported);

    void close_stream_once() noexcept;
    /// Consume the single staging-cleanup ownership and attempt at most one
    /// removal. Returns false when the attempt failed; never retried.
    bool remove_staging_once() noexcept;
    /// Latch `kind`/`message` as the first failure if none is latched yet, then
    /// copy the latched (possibly earlier) reason to the out-params.
    void latch_failure(FailureKind kind, char const *message, FailureKind &failure, std::string &error);
    void latch(FailureKind &failure, std::string &error) const;
    PublicationResult make_result() const;

    LogicalTarget target_;
    SystemCalls *calls_; ///< borrowed; must outlive this object
    std::string staged_path_;
    FILE *stream_ = nullptr;
    bool retain_on_unsupported_ = false;
    bool sealed_ = false;
    bool closed_ = false;
    bool aborted_ = false;
    bool failed_ = false;
    bool stage_owned_ = true;
    std::function<bool(unsigned)> stage_observer_;
    bool publication_attempted_ = false;
    PublicationStatus status_ = PublicationStatus::NotAttempted;
    FailureKind failure_ = FailureKind::None;
    std::string error_;

    bool file_sync_attempted_ = false;
    bool file_sync_supported_ = false;
    bool file_sync_ok_ = false;
    bool parent_sync_attempted_ = false;
    bool parent_sync_supported_ = false;
    bool parent_sync_ok_ = false;
    bool staging_cleanup_ok_ = true;
    StagingAvailability staging_availability_ = StagingAvailability::Unverified;
};

} // namespace Inkscape::IO::DocumentTransaction

#endif // INKSCAPE_IO_DOCUMENT_FILE_TRANSACTION_H
