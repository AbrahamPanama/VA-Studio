// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Shared NewDocumentFile logic for the F3a new-file storage packet.
 *
 * This translation unit contains no platform headers. POSIX system calls live
 * in document-file-transaction-posix.cpp and the Windows local-NTFS adapter in
 * document-file-transaction-win32.cpp. All platform-specific spelling and the
 * pre-create admission are reached through the SystemCalls boundary, so POSIX
 * and test adapters inherit the permissive defaults unchanged.
 */

#include "document-file-transaction.h"

#include <cerrno>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <glib.h>

namespace Inkscape::IO::DocumentTransaction {

namespace {

// Short, fixed, non-dot sibling component. The random uniqueness is supplied by
// the platform's secure exclusive create handling (mkstemp on POSIX, CREATE_NEW
// on Windows). It is independent of the destination basename so a legal long
// basename can never be pushed past NAME_MAX, and it does not begin with a dot
// because some SMB/Finder shares mark dot names hidden at creation.
constexpr char const *STAGING_NAME_TEMPLATE = "vacards-new-XXXXXX";
constexpr int MAX_STAGING_NAME_ATTEMPTS = 8;

bool contains_nul(std::string const &value)
{
    return value.find('\0') != std::string::npos;
}

#ifdef _WIN32

// Windows accepts both '/' and '\\' as path separators. The stored logical
// strings are never rewritten; this predicate only compares/joins spellings.
bool is_path_separator(char c)
{
    return c == '/' || c == '\\';
}

bool is_ascii_alpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool has_drive_prefix(std::string const &path)
{
    return path.size() >= 2 && is_ascii_alpha(path[0]) && path[1] == ':';
}

bool is_drive_absolute(std::string const &path)
{
    return path.size() >= 3 && is_ascii_alpha(path[0]) && path[1] == ':' && is_path_separator(path[2]);
}

bool is_unc_absolute(std::string const &path)
{
    return path.size() >= 2 && is_path_separator(path[0]) && is_path_separator(path[1]);
}

// Raw device (`\\.\`) and extended (`\\?\`) namespaces are rejected here; the
// adapter alone adds the extended prefix for the Win32 calls.
bool is_device_prefix(std::string const &path)
{
    return path.size() >= 3 && is_path_separator(path[0]) && is_path_separator(path[1])
        && (path[2] == '?' || path[2] == '.');
}

bool has_colon_outside_drive(std::string const &path)
{
    std::size_t const start = has_drive_prefix(path) ? 2 : 0;
    return path.find(':', start) != std::string::npos;
}

bool has_dot_component(std::string const &path)
{
    std::size_t i = has_drive_prefix(path) ? 2 : 0;
    while (i < path.size()) {
        while (i < path.size() && is_path_separator(path[i])) {
            ++i;
        }
        std::size_t const start = i;
        while (i < path.size() && !is_path_separator(path[i])) {
            ++i;
        }
        if (i > start) {
            std::string const component = path.substr(start, i - start);
            if (component == "." || component == "..") {
                return true;
            }
        }
    }
    return false;
}

// Windows accepts either separator, but a canonical absolute spelling uses at
// most one separator between components and, at the very start, exactly two
// separators for a UNC root (or one for a rooted spelling). Runs beyond that are
// rejected here rather than normalized into extended `\\?\` namespace spelling,
// which CreateFileW can refuse for reasons unrelated to the target's authority.
bool has_noncanonical_separator_run(std::string const &path)
{
    std::size_t i = 0;
    if (!has_drive_prefix(path)) {
        std::size_t leading = 0;
        while (leading < path.size() && is_path_separator(path[leading])) {
            ++leading;
        }
        if (leading > 2) {
            return true;
        }
        i = leading;
    }
    while (i < path.size()) {
        if (!is_path_separator(path[i])) {
            ++i;
            continue;
        }
        std::size_t run = 0;
        while (i + run < path.size() && is_path_separator(path[i + run])) {
            ++run;
        }
        if (run > 1) {
            return true;
        }
        i += run;
    }
    return false;
}

bool is_reserved_dos_name(std::string const &name)
{
    std::size_t const dot = name.find('.');
    std::string stem = name.substr(0, dot == std::string::npos ? name.size() : dot);
    for (char &c : stem) {
        c = static_cast<char>(g_ascii_toupper(c));
    }
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") {
        return true;
    }
    if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0)
        && stem[3] >= '1' && stem[3] <= '9') {
        return true;
    }
    return false;
}

// Characters Windows forbids in a filename component, including double quote,
// wildcards and the redirection/pipe set. UTF-8 bytes >= 0x80 are ordinary and
// must stay accepted.
bool has_invalid_windows_char(std::string const &component)
{
    return component.find_first_of("\"*?<>|") != std::string::npos;
}

bool has_ascii_control(std::string const &component)
{
    for (unsigned char const c : component) {
        if (c < 0x20) {
            return true;
        }
    }
    return false;
}

// Component-wise Windows legality for an absolute drive or UNC path. The drive
// prefix (when present) and the leading separators are skipped; each remaining
// component must avoid invalid characters/controls, trailing dot or space, and
// reserved DOS device names. Length and Unicode are never restricted here.
bool validate_windows_components(std::string const &path)
{
    std::size_t i = has_drive_prefix(path) ? 2 : 0;
    while (i < path.size()) {
        while (i < path.size() && is_path_separator(path[i])) {
            ++i;
        }
        std::size_t const start = i;
        while (i < path.size() && !is_path_separator(path[i])) {
            ++i;
        }
        if (i == start) {
            continue;
        }
        std::string const component = path.substr(start, i - start);
        if (has_invalid_windows_char(component) || has_ascii_control(component)) {
            return false;
        }
        if (component.back() == '.' || component.back() == ' ') {
            return false;
        }
        if (is_reserved_dos_name(component)) {
            return false;
        }
    }
    return true;
}

// Exact parent/name spelling: the parent bytes are preserved verbatim and the
// boundary is exactly one separator of either kind, omitted only when the
// parent already ends in one. This accepts both Windows spellings while
// rejecting normalization, doubled separators and dot components.
bool exact_parent_name_relation(std::string const &parent, std::string const &name,
                                std::string const &path)
{
    if (path.size() < parent.size() || path.compare(0, parent.size(), parent) != 0) {
        return false;
    }
    std::size_t pos = parent.size();
    if (parent.empty() || !is_path_separator(parent.back())) {
        if (pos >= path.size() || !is_path_separator(path[pos])) {
            return false;
        }
        ++pos;
    }
    return path.size() - pos == name.size() && path.compare(pos, name.size(), name) == 0;
}

// Only used to build the staging sibling from the same parent bytes. Windows
// accepts either separator; a backslash is the native default.
std::string join_parent_and_name(std::string const &parent_dir, std::string const &name)
{
    std::string joined = parent_dir;
    if (!joined.empty() && !is_path_separator(joined.back())) {
        joined.push_back('\\');
    }
    joined += name;
    return joined;
}

bool validate_target(LogicalTarget const &target, std::string &error)
{
    if (contains_nul(target.final_path) || contains_nul(target.final_name)
        || contains_nul(target.parent_dir)) {
        error = "target strings must not contain embedded NUL bytes";
        return false;
    }
    if (!g_utf8_validate(target.final_path.data(), target.final_path.size(), nullptr)
        || !g_utf8_validate(target.final_name.data(), target.final_name.size(), nullptr)
        || !g_utf8_validate(target.parent_dir.data(), target.parent_dir.size(), nullptr)) {
        error = "target strings must be valid UTF-8";
        return false;
    }
    if (target.final_path.empty()) {
        error = "final_path is empty";
        return false;
    }
    if (target.final_path == "-" || target.final_name == "-") {
        error = "pipe/stdio destination '-' bypasses the new-file transaction";
        return false;
    }
    if (target.final_name.empty() || target.final_name == "." || target.final_name == "..") {
        error = "final_name is invalid";
        return false;
    }
    if (target.final_name.find_first_of("/\\") != std::string::npos) {
        error = "final_name must be a basename without path separators";
        return false;
    }
    if (target.final_name.find(':') != std::string::npos) {
        error = "final_name must not contain ':' (alternate data stream)";
        return false;
    }
    if (has_invalid_windows_char(target.final_name)) {
        error = "final_name contains a character Windows forbids in a name";
        return false;
    }
    for (char const c : target.final_name) {
        if (static_cast<unsigned char>(c) < 0x20) {
            error = "final_name must not contain control characters";
            return false;
        }
    }
    if (target.final_name.back() == '.' || target.final_name.back() == ' ') {
        error = "final_name must not end with a dot or space";
        return false;
    }
    if (is_reserved_dos_name(target.final_name)) {
        error = "final_name is a reserved DOS device name";
        return false;
    }
    if (target.parent_dir.empty()) {
        error = "parent_dir is empty";
        return false;
    }
    if (is_device_prefix(target.parent_dir) || is_device_prefix(target.final_path)) {
        error = "raw device and extended-length prefixes are not accepted";
        return false;
    }
    if (!is_drive_absolute(target.parent_dir) && !is_unc_absolute(target.parent_dir)) {
        error = "parent_dir must be drive-absolute (or UNC)";
        return false;
    }
    if (!is_drive_absolute(target.final_path) && !is_unc_absolute(target.final_path)) {
        error = "final_path must be drive-absolute (or UNC)";
        return false;
    }
    if (has_noncanonical_separator_run(target.parent_dir)
        || has_noncanonical_separator_run(target.final_path)) {
        error = "path uses a repeated separator; use one separator between components and exactly two leading separators for a UNC root";
        return false;
    }
    if (has_colon_outside_drive(target.parent_dir) || has_colon_outside_drive(target.final_path)) {
        error = "':' is only allowed in the drive prefix";
        return false;
    }
    if (has_dot_component(target.parent_dir) || has_dot_component(target.final_path)) {
        error = "'.' and '..' path components are not accepted";
        return false;
    }
    if (!validate_windows_components(target.parent_dir)
        || !validate_windows_components(target.final_path)) {
        error = "a path component uses a character, trailing dot/space or reserved name Windows forbids";
        return false;
    }
    if (!exact_parent_name_relation(target.parent_dir, target.final_name, target.final_path)) {
        error = "final_path must be exactly parent_dir + one separator + final_name (no normalization, no symlink resolution)";
        return false;
    }
    return true;
}

#else // POSIX

// POSIX-only: '/' is the only path separator. A backslash is a legitimate
// basename byte and must never be treated as a separator here.
bool is_path_separator(char c)
{
    return c == '/';
}

// Single exact-spelling join used by both target validation and the staging
// template: parent_dir, then exactly one '/' unless parent_dir already ends in
// one, then name. No canonicalization and no symlink resolution.
std::string join_parent_and_name(std::string const &parent_dir, std::string const &name)
{
    std::string joined = parent_dir;
    if (!joined.empty() && !is_path_separator(joined.back())) {
        joined.push_back('/');
    }
    joined += name;
    return joined;
}

bool validate_target(LogicalTarget const &target, std::string &error)
{
    if (contains_nul(target.final_path) || contains_nul(target.final_name)
        || contains_nul(target.parent_dir)) {
        error = "target strings must not contain embedded NUL bytes";
        return false;
    }
    if (target.final_path.empty()) {
        error = "final_path is empty";
        return false;
    }
    if (target.final_path == "-" || target.final_name == "-") {
        error = "pipe/stdio destination '-' bypasses the new-file transaction";
        return false;
    }
    if (target.final_name.empty() || target.final_name == "." || target.final_name == "..") {
        error = "final_name is invalid";
        return false;
    }
    if (target.final_name.find('/') != std::string::npos) {
        error = "final_name must be a basename without path separators";
        return false;
    }
    if (target.parent_dir.empty()) {
        error = "parent_dir is empty";
        return false;
    }
    if (!g_path_is_absolute(target.parent_dir.c_str())) {
        error = "relative parent_dir is ambiguous; supply an absolute directory";
        return false;
    }
    if (!g_path_is_absolute(target.final_path.c_str())) {
        error = "relative final_path is ambiguous; supply an absolute destination";
        return false;
    }

    // Exact byte-spelling relationship, never canonicalized and with no symlink
    // resolution: final_path must be exactly parent_dir + one '/' (omitted when
    // parent_dir already ends in '/') + final_name. The same helper builds the
    // staging template, so the destination the adapter links to is constructed
    // from the identical parent spelling whose sibling was staged. This proves
    // only the stated spelling, not the identity of the parent directory: an
    // already-symlinked parent, or a component replaced between this check and
    // publication, remains the caller's documented check-to-publish race.
    if (target.final_path != join_parent_and_name(target.parent_dir, target.final_name)) {
        error = "final_path must be exactly parent_dir + '/' + final_name (no normalization, no symlink resolution)";
        return false;
    }
    return true;
}

#endif // _WIN32

std::string staging_template_for(LogicalTarget const &target)
{
    // Deliberately the same exact-spelling helper as validate_target so the
    // staged sibling and the stated final_path share one parent construction.
    return join_parent_and_name(target.parent_dir, STAGING_NAME_TEMPLATE);
}

// The stage-capture transfer below relies on this being non-throwing: once the
// platform reports a created stage, no allocation may fail before the owning
// NewDocumentFile holds the path and stream.
static_assert(std::is_nothrow_move_assignable<std::string>::value,
              "staging path capture must be non-throwing after the stage exists");

bool create_exclusive_sibling(LogicalTarget const &target, SystemCalls &calls,
                              std::string &staged_path, FILE *&staged, std::string &error)
{
    std::string const template_path = staging_template_for(target);
    std::string last_error;

    for (int attempt = 0; attempt < MAX_STAGING_NAME_ATTEMPTS; ++attempt) {
        std::string attempt_path = template_path;
        FILE *out = nullptr;
        bool already_exists = false;
        std::string err;
        bool ok = false;
        try {
            ok = calls.create_exclusive_file(attempt_path, out, already_exists, err);
        } catch (...) {
            error = "create_exclusive_file threw an exception";
            return false;
        }

        if (ok) {
            // The platform reports the actual created name in place. std::string
            // move assignment is noexcept, so nothing can fail between the
            // created path/stream becoming available and the owning
            // NewDocumentFile capturing them; there is deliberately no copy.
            staged_path = std::move(attempt_path);
            staged = out;
            return true;
        }
        if (already_exists) {
            last_error = err.empty() ? "staging name collision" : err;
            continue;
        }
        error = err.empty() ? "could not create staging file" : err;
        return false;
    }

    error = "exhausted staging name retries: " + last_error;
    return false;
}

} // namespace

NewDocumentFile::NewDocumentFile(LogicalTarget target, SystemCalls &calls,
                                 std::string staged_path, FILE *stream,
                                 bool retain_on_unsupported)
    : target_(std::move(target))
    , calls_(&calls)
    , staged_path_(std::move(staged_path))
    , stream_(stream)
    , retain_on_unsupported_(retain_on_unsupported)
{
}

NewDocumentFile::~NewDocumentFile()
{
    if (stream_) {
        close_stream_once();
    }
    // Retain an ambiguous stage, and an unsupported one only for callers that
    // explicitly need a user-visible recovery candidate.
    if (status_ == PublicationStatus::Uncertain
        || (status_ == PublicationStatus::Unsupported && retain_on_unsupported_)) {
        return;
    }
    // Consumed exactly once; a file recreated at the old path is never deleted.
    remove_staging_once();
}

std::unique_ptr<NewDocumentFile> NewDocumentFile::create(LogicalTarget target,
                                                         SystemCalls &calls,
                                                         FailureKind &failure,
                                                         std::string &error,
                                                         bool retain_on_unsupported,
                                                         std::function<bool(unsigned)> stage_observer)
{
    if (!validate_target(target, error)) {
        failure = FailureKind::InvalidArgument;
        return nullptr;
    }

    // Admission is part of the shared state machine: POSIX and test adapters
    // inherit a None default, so this preserves their behavior while a platform
    // that cannot host the publication primitive rejects before any payload.
    FailureKind support = FailureKind::None;
    try {
        support = calls.pre_create_support(target.parent_dir, error);
    } catch (...) {
        support = FailureKind::Unsupported;
        error = "pre-create support check threw an exception";
    }
    if (support != FailureKind::None) {
        failure = support;
        return nullptr;
    }

    // Allocate the owner BEFORE any staging file exists. The owner's path and
    // stream members are filled in place by the helper, so no allocation can
    // fail after the stage exists and leave it ownerless.
    auto self = std::unique_ptr<NewDocumentFile>(
        new (std::nothrow) NewDocumentFile(std::move(target), calls, std::string(),
                                          nullptr, retain_on_unsupported));
    if (!self) {
        // No stage exists yet: allocation failure is entirely side-effect free.
        failure = FailureKind::StagingCreateFailed;
        error = "could not allocate NewDocumentFile";
        return nullptr;
    }

    if (!create_exclusive_sibling(self->target_, calls, self->staged_path_, self->stream_, error)) {
        // The helper only assigns the actual created path/stream, and only via
        // non-throwing operations, so on failure the owner holds no stage and
        // its destructor is side-effect free.
        failure = FailureKind::StagingCreateFailed;
        return nullptr;
    }

    failure = FailureKind::None;
    error.clear();
    self->stage_observer_ = std::move(stage_observer);
    if (self->stage_observer_ && self->stage_observer_(1)) {
        failure = FailureKind::StagingCreateFailed;
        error = "injected stage-create failure";
        return nullptr;
    }
    return self;
}

void NewDocumentFile::close_stream_once() noexcept
{
    if (!stream_) {
        return;
    }
    FILE *stream = stream_;
    stream_ = nullptr; // exactly-once ownership even if close fails
    try {
        std::string ignored;
        calls_->close_file(stream, ignored);
    } catch (...) {
    }
    closed_ = true;
}

bool NewDocumentFile::remove_staging_once() noexcept
{
    if (!stage_owned_) {
        return true;
    }
    // Consume ownership before the attempt: a failed cleanup is reported and
    // never retried, so a file later recreated at this path is left alone and
    // there is no second deletion.
    stage_owned_ = false;
    if (staged_path_.empty()) {
        return true;
    }
    try {
        return calls_->remove_file(staged_path_);
    } catch (...) {
        return false;
    }
}

void NewDocumentFile::latch_failure(FailureKind kind, char const *message,
                                    FailureKind &failure, std::string &error)
{
    if (!failed_) {
        failure_ = kind;
        failed_ = true;
        error_ = message;
    }
    // Report the latched first reason, not the current attempt's.
    failure = failure_;
    error = error_;
}

void NewDocumentFile::latch(FailureKind &failure, std::string &error) const
{
    failure = failure_;
    error = error_;
}

PublicationResult NewDocumentFile::make_result() const
{
    PublicationResult result;
    result.status = status_;
    result.failure = failure_;
    result.error = error_;
    result.file_sync_attempted = file_sync_attempted_;
    result.file_sync_supported = file_sync_supported_;
    result.file_sync_ok = file_sync_ok_;
    result.parent_sync_attempted = parent_sync_attempted_;
    result.parent_sync_supported = parent_sync_supported_;
    result.parent_sync_ok = parent_sync_ok_;
    // Deliberate retention requires an ambiguous publication or caller opt-in,
    // and availability that is not known to be Missing/Changed. Identity proves the
    // object, never the bytes; an Unverified answer stays visible as such.
    bool const availability_allows_retention =
        staging_availability_ == StagingAvailability::Available
        || staging_availability_ == StagingAvailability::Unverified;
    result.recovery_retained =
        (status_ == PublicationStatus::Uncertain
         || (status_ == PublicationStatus::Unsupported && retain_on_unsupported_))
        && availability_allows_retention;
    result.recovery_path = result.recovery_retained || (status_ == PublicationStatus::Published && !staging_cleanup_ok_)
        ? staged_path_ : std::string();
    result.staging_availability = staging_availability_;
    result.staging_cleanup_ok = staging_cleanup_ok_;
    return result;
}

bool NewDocumentFile::write(StageWriter const &writer, FailureKind &failure, std::string &error)
{
    if (publication_attempted_) {
        // Invalid after publication; do not alter the recorded outcome.
        failure = FailureKind::InvalidOperation;
        error = "write is not allowed after publication";
        return false;
    }
    if (failed_) {
        latch(failure, error);
        return false;
    }
    if (aborted_) {
        failure = FailureKind::Aborted;
        error = "write is not allowed after abort";
        return false;
    }
    if (sealed_) {
        failure = FailureKind::InvalidOperation;
        error = "write is not allowed after seal";
        return false;
    }
    if (closed_ || !stream_) {
        latch_failure(FailureKind::StagingWriteFailed, "staging stream is not open", failure, error);
        return false;
    }
    if (!writer) {
        close_stream_once();
        latch_failure(FailureKind::StagingWriteFailed, "writer callback is empty", failure, error);
        return false;
    }

    try {
        writer(stream_);
    } catch (std::exception const &e) {
        close_stream_once();
        latch_failure(FailureKind::StagingWriteFailed,
                      e.what() ? e.what() : "writer callback threw", failure, error);
        return false;
    } catch (...) {
        close_stream_once();
        latch_failure(FailureKind::StagingWriteFailed, "writer callback threw", failure, error);
        return false;
    }

    failure_ = FailureKind::None;
    error_.clear();
    if (stage_observer_ && stage_observer_(2)) {
        latch_failure(FailureKind::StagingWriteFailed, "injected stage-write failure", failure, error);
        close_stream_once();
        return false;
    }
    latch(failure, error);
    return true;
}

bool NewDocumentFile::write_bytes(std::span<std::byte const> bytes, FailureKind &failure,
                                  std::string &error, bool inject_byte_write_failure_for_testing)
{
    return write([bytes, inject_byte_write_failure_for_testing](FILE *stage) {
        if (inject_byte_write_failure_for_testing)
            throw std::runtime_error("injected staging byte write failure");
        if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), stage) != bytes.size()) {
            throw std::runtime_error("write staging bytes failed");
        }
    }, failure, error);
}

bool NewDocumentFile::seal(FailureKind &failure, std::string &error)
{
    if (publication_attempted_) {
        // Invalid after publication; do not alter the recorded outcome.
        failure = FailureKind::InvalidOperation;
        error = "seal is not allowed after publication";
        return false;
    }
    if (failed_) {
        latch(failure, error);
        return false;
    }
    if (aborted_) {
        failure = FailureKind::Aborted;
        error = "seal is not allowed after abort";
        return false;
    }
    if (sealed_) {
        // Idempotent success; no second flush/close.
        failure = FailureKind::None;
        error.clear();
        return true;
    }
    if (closed_ || !stream_) {
        latch_failure(FailureKind::StagingWriteFailed, "staging stream is not open", failure, error);
        return false;
    }

    bool pre_ok = false;
    bool sync_unsupported = false;
    std::string err;
    try {
        if (!calls_->flush_file(stream_, err)) {
            failure_ = FailureKind::StagingFlushFailed;
            error_ = err.empty() ? "staging flush failed" : err;
        } else {
            file_sync_attempted_ = true;
            if (!calls_->sync_file(stream_, sync_unsupported, err)) {
                failure_ = FailureKind::StagingSyncFailed;
                error_ = err.empty() ? "staging sync failed" : err;
            } else {
                // A reported-unsupported sync is a capability, not durability.
                file_sync_supported_ = !sync_unsupported;
                file_sync_ok_ = !sync_unsupported;
                pre_ok = true;
            }
        }
    } catch (...) {
        failure_ = FailureKind::StagingFlushFailed;
        error_ = "staging flush/sync threw an exception";
    }

    if (!pre_ok) {
        failed_ = true;
        close_stream_once();
        latch(failure, error);
        return false;
    }

    bool close_ok = false;
    err.clear();
    FILE *stream = stream_;
    stream_ = nullptr; // exactly-once close
    try {
        close_ok = calls_->close_file(stream, err);
    } catch (...) {
        close_ok = false;
        err = "staging close threw an exception";
    }
    closed_ = true;

    if (!close_ok) {
        failed_ = true;
        failure_ = FailureKind::StagingCloseFailed;
        error_ = err.empty() ? "staging close failed" : err;
        latch(failure, error);
        return false;
    }

    sealed_ = true;
    failure_ = FailureKind::None;
    error_.clear();
    if (stage_observer_ && stage_observer_(3)) {
        latch_failure(FailureKind::StagingCloseFailed, "injected stage-seal failure", failure, error);
        return false;
    }
    latch(failure, error);
    return true;
}

PublicationResult NewDocumentFile::publish()
{
    if (publication_attempted_) {
        // Stable repeat: complete originally recorded details, no syscall.
        return make_result();
    }
    if (failed_) {
        status_ = PublicationStatus::Failed;
        return make_result();
    }
    if (aborted_) {
        status_ = PublicationStatus::Failed;
        failure_ = FailureKind::Aborted;
        error_ = "transaction aborted; publication is forbidden";
        return make_result();
    }
    if (!sealed_ || stream_) {
        failed_ = true;
        status_ = PublicationStatus::Failed;
        if (failure_ == FailureKind::None) {
            failure_ = FailureKind::StagingWriteFailed;
        }
        error_ = "publish requires a sealed staging file";
        return make_result();
    }

    // Latch the ambiguous outcome BEFORE the platform call. The members carry
    // this state, so even an exception while formatting the result cannot turn
    // a possible publication into a pre-commit failure and the destructor keeps
    // the recovery copy.
    if (stage_observer_ && stage_observer_(4)) {
        failed_ = true;
        status_ = PublicationStatus::Failed;
        failure_ = FailureKind::PublicationFailed;
        error_ = "injected pre-publication failure";
        return make_result();
    }
    publication_attempted_ = true;
    status_ = PublicationStatus::Uncertain;
    failure_ = FailureKind::PublicationUncertain;
    error_.clear();

    PublicationStatus published = PublicationStatus::Uncertain;
    std::string err;
    try {
        published = calls_->publish_new_file(staged_path_, target_.final_path, err);
    } catch (std::exception const &e) {
        published = PublicationStatus::Uncertain;
        err = e.what() ? e.what() : "publish call threw an exception";
    } catch (...) {
        published = PublicationStatus::Uncertain;
        err = "publish call threw an exception";
    }

    switch (published) {
    case PublicationStatus::Published:
        status_ = PublicationStatus::Published;
        failure_ = FailureKind::None;
        error_ = err;
        if (stage_observer_ && stage_observer_(5)) {
            error_ = "published, injected staging cleanup failure";
            staging_cleanup_ok_ = false;
            stage_owned_ = false; // retain this stage as a possible recovery artifact
        } else {
            staging_cleanup_ok_ = remove_staging_once();
            if (!staging_cleanup_ok_) error_ = "published, staging cleanup failed";
        }

        // Directory durability is a separate result and never downgrades the
        // confirmed namespace outcome.
        parent_sync_attempted_ = true;
        {
            bool sync_unsupported = false;
            std::string sync_error;
            bool sync_ok = false;
            try {
                sync_ok = calls_->sync_parent_directory(target_.parent_dir, sync_unsupported, sync_error);
            } catch (...) {
                sync_ok = false;
                sync_unsupported = false;
            }
            parent_sync_supported_ = !sync_unsupported;
            parent_sync_ok_ = sync_ok && !sync_unsupported;
        }
        break;
    case PublicationStatus::Conflict:
        status_ = PublicationStatus::Conflict;
        failure_ = FailureKind::DestinationExists;
        error_ = err;
        staging_cleanup_ok_ = remove_staging_once();
        break;
    case PublicationStatus::Uncertain:
        status_ = PublicationStatus::Uncertain;
        failure_ = FailureKind::PublicationUncertain;
        error_ = err;
        // Retention depends on whether the adapter can positively confirm the
        // recorded staging identity at the exact path. The answer is latched
        // here so a repeated publish issues no new syscall, and an Unverified
        // answer stays visible rather than being presented as verified.
        staging_availability_ = StagingAvailability::Unverified;
        {
            std::string availability_error;
            try {
                staging_availability_ = calls_->retained_stage_availability(staged_path_, availability_error);
            } catch (...) {
                staging_availability_ = StagingAvailability::Unverified;
            }
        }
        break;
    case PublicationStatus::Unsupported:
        status_ = PublicationStatus::Unsupported;
        failure_ = FailureKind::Unsupported;
        error_ = err;
        // No final entry was created. Interactive document saves keep a possible
        // recovery candidate; background saves clean their stage as before.
        if (retain_on_unsupported_) {
            staging_availability_ = StagingAvailability::Unverified;
            std::string availability_error;
            try {
                staging_availability_ = calls_->retained_stage_availability(staged_path_, availability_error);
            } catch (...) {
                staging_availability_ = StagingAvailability::Unverified;
            }
        } else {
            staging_cleanup_ok_ = remove_staging_once();
        }
        break;
    case PublicationStatus::NotAttempted:
    case PublicationStatus::Failed:
    default:
        status_ = PublicationStatus::Failed;
        failure_ = FailureKind::PublicationFailed;
        error_ = err;
        staging_cleanup_ok_ = remove_staging_once();
        break;
    }

    return make_result();
}

void NewDocumentFile::abort() noexcept
{
    if (publication_attempted_) {
        // Publication outcomes have already resolved their staging policy.
        return;
    }
    // Terminal cancellation: no later seal or publish is possible.
    aborted_ = true;
    sealed_ = false;
    if (stream_) {
        close_stream_once();
    }
    remove_staging_once();
}

} // namespace Inkscape::IO::DocumentTransaction
