// SPDX-License-Identifier: GPL-2.0-or-later
// Conservative existing-file publication for local macOS SVG saves and a
// bounded SMB replacement path. Other network filesystems remain unsupported.

#include "existing-file-replacement.h"
#include "save-path-split.h"

#include <cerrno>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <time.h>

#ifdef __APPLE__
#include "macos-finder-icon.h"
#include <copyfile.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>

// Test-only, thread-local, one-shot errno injection for the rename step. It
// is omitted entirely from non-test builds. There is deliberately no
// declaration in the public header; the unit test forward-declares the setter.
#ifdef VACARDS_FILE_IO_TEST_HOOKS
namespace Inkscape::IO::detail {

namespace {
thread_local int g_injected_rename_errno = 0;
thread_local int g_injected_recovery_delete_errno = 0;
}

void set_rename_failure_errno_for_testing(int errno_value)
{
    g_injected_rename_errno = errno_value;
}

void set_recovery_delete_failure_errno_for_testing(int errno_value)
{
    g_injected_recovery_delete_errno = errno_value;
}

static int consume_injected_rename_errno()
{
    int const value = g_injected_rename_errno;
    g_injected_rename_errno = 0;
    return value;
}

static int consume_injected_recovery_delete_errno()
{
    int const value = g_injected_recovery_delete_errno;
    g_injected_recovery_delete_errno = 0;
    return value;
}

} // namespace Inkscape::IO::detail
#endif

namespace Inkscape::IO {
namespace {

using SaveClock = std::chrono::steady_clock;
bool save_timing_enabled()
{
    auto const *value = std::getenv("VACARDS_SAVE_TIMING");
    return value && std::strcmp(value, "1") == 0;
}
void save_timing_span(char const *name, SaveClock::time_point begin, bool timing)
{
    if (timing) {
        auto const ms = std::chrono::duration<double, std::milli>(SaveClock::now() - begin).count();
        std::fprintf(stderr, "VACARDS_SAVE_TIMING span=%s ms=%.3f\n", name, ms);
    }
}

std::string error_text(char const *operation, int code)
{
    return std::string(operation) + ": " + g_strerror(code);
}

int remove_confirmed_recovery(std::string const &path, ExistingFileOptions const &options)
{
    if (options.recovery_delete_failure_errno != 0) { errno = options.recovery_delete_failure_errno; return -1; }
    return ::unlink(path.c_str());
}

bool same_file_version(struct stat const &a, struct stat const &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_nlink == b.st_nlink
        && a.st_size == b.st_size
        && a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec
        && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec
        && a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec
        && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
}

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : _fd(fd) {}
    ~ScopedFd() { if (_fd >= 0) ::close(_fd); }
    ScopedFd(ScopedFd const &) = delete;
    ScopedFd &operator=(ScopedFd const &) = delete;
    int get() const { return _fd; }
    int release() { int const fd = _fd; _fd = -1; return fd; }
    void reset() { if (_fd >= 0) ::close(_fd); _fd = -1; }
private:
    int _fd;
};

// Best-effort removal of the BSD flags that make a scratch file undeletable.
// Only ever called for the descriptor of an owned stage we are about to unlink,
// never for the destination or a recovery copy. Failure is ignored because
// cleanup is best-effort and unlink follows regardless.
void clear_protective_flags(int fd)
{
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        return;
    }
    unsigned int const protective = UF_IMMUTABLE | SF_IMMUTABLE | UF_APPEND | SF_APPEND;
    if ((st.st_flags & protective) != 0) {
        (void)::fchflags(fd, st.st_flags & ~protective);
    }
}

class ScopedStage {
public:
    explicit ScopedStage(std::string path) : path(std::move(path)) {}
    ~ScopedStage()
    {
        auto const cleanup_begin = SaveClock::now();
        // A metadata copy (or a race) can stamp UF_IMMUTABLE/UF_APPEND onto the
        // scratch stage, which would make unlink fail and leak the stage. Clear
        // those flags through the open descriptor before closing it.
        if (owned) {
            if (stream) {
                clear_protective_flags(::fileno(stream));
            } else if (fd >= 0) {
                clear_protective_flags(fd);
            } else {
                // The normal path already closed the descriptor before a later
                // failure (conflict, backup, rename). Reopen only our own,
                // uniquely named stage without following links so the flags can
                // still be cleared. A missing/replaced path just fails here.
                int const reopened = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
                if (reopened >= 0) {
                    clear_protective_flags(reopened);
                    ::close(reopened);
                }
            }
        }
        if (stream) {
            std::fclose(stream);
        } else if (fd >= 0) {
            ::close(fd);
        }
        if (owned) {
            ::unlink(path.c_str());
        }
        save_timing_span("cleanup", cleanup_begin, timing);
    }
    ScopedStage(ScopedStage const &) = delete;
    ScopedStage &operator=(ScopedStage const &) = delete;
    std::string path;
    int fd = -1;
    FILE *stream = nullptr;
    bool owned = true;
    bool timing = false;
};

ExistingFileResult before_failure(std::string error)
{
    return {ExistingFileOutcome::FailedBeforePublication, std::move(error), {}};
}

bool current_matches(std::string const &path, struct stat const &baseline)
{
    struct stat now{};
    return ::lstat(path.c_str(), &now) == 0 && S_ISREG(now.st_mode)
        && same_file_version(now, baseline);
}

// Read through a fresh descriptor with the local cache disabled where
// supported. This verifies client-visible bytes, not server crash durability.
bool sha256_file(std::string const &path, std::string &digest)
{
    ScopedFd fd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW));
    if (fd.get() < 0) return false;
    (void)::fcntl(fd.get(), F_NOCACHE, 1);
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    if (!checksum) return false;
    std::array<unsigned char, 65536> buffer{};
    bool ok = true;
    while (true) {
        ssize_t const n = ::read(fd.get(), buffer.data(), buffer.size());
        if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
        if (n == 0) break;
        g_checksum_update(checksum, buffer.data(), static_cast<gsize>(n));
    }
    if (ok) digest = g_checksum_get_string(checksum);
    g_checksum_free(checksum);
    return ok;
}

ExistingFileResult publish_smb_existing(std::string const &path, std::string const &parent,
                                        int directory_fd, ScopedFd &original,
                                        struct stat const &baseline, ScopedStage &stage, ExistingFileOptions const &options)
{
    std::string old_digest, new_digest;
    if (!current_matches(path, baseline))
        return {ExistingFileOutcome::Conflict, "SMB destination changed before recovery", {}};
    if (!sha256_file(path, old_digest) || !sha256_file(stage.path, new_digest))
        return before_failure("read back SMB destination or staged file");

    // SMB lacks hard links on this supported share. Retain a complete,
    // independently readable old version before replacing the destination.
    std::string recovery;
    int recovery_fd_raw = -1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        gchar *uuid = g_uuid_string_random();
        recovery = parent + "/vacards-recovery-" + uuid;
        g_free(uuid);
        int const fd = ::open(recovery.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd >= 0) { recovery_fd_raw = fd; break; }
        if (errno != EEXIST) return before_failure(error_text("create SMB recovery copy", errno));
    }
    ScopedFd recovery_fd(recovery_fd_raw);
    if (recovery_fd.get() < 0) return before_failure("could not choose SMB recovery filename");
    if (::fcopyfile(original.get(), recovery_fd.get(), nullptr, COPYFILE_ALL) != 0
        || ::fsync(recovery_fd.get()) != 0) {
        int const saved = errno;
        recovery_fd.reset();
        if (::unlink(recovery.c_str()) != 0) {
            return before_failure(error_text("retain SMB recovery copy", saved)
                + "; incomplete recovery may remain at " + recovery);
        }
        return before_failure(error_text("retain SMB recovery copy", saved));
    }
    recovery_fd.reset();
    original.reset(); // smbfs must not rename an open destination aside.
    std::string recovery_digest, current_digest;
    bool const backup_readable = sha256_file(recovery, recovery_digest);
    bool const current_readable = sha256_file(path, current_digest);
    bool const changed = !current_matches(path, baseline)
        || (current_readable && current_digest != old_digest);
    if (!backup_readable || recovery_digest != old_digest || !current_readable || changed) {
        std::string const error = changed ? "SMB destination changed before publication"
                                          : "SMB recovery or destination read-back failed";
        if (::unlink(recovery.c_str()) != 0)
            return changed ? ExistingFileResult{ExistingFileOutcome::Conflict,
                    error + "; recovery may remain at " + recovery, {}}
                : before_failure(error + "; recovery may remain at " + recovery);
        return changed ? ExistingFileResult{ExistingFileOutcome::Conflict, error, {}}
                       : before_failure(error);
    }
    if (::fsync(directory_fd) != 0) {
        int const saved = errno;
        if (::unlink(recovery.c_str()) != 0)
            return before_failure(error_text("sync SMB recovery directory", saved)
                + "; recovery may remain at " + recovery);
        return before_failure(error_text("sync SMB recovery directory", saved));
    }

    bool const injected_stage_rename_failure = options.stage_observer && options.stage_observer(4);
    if (options.cancelled && options.cancelled()) {
        bool removed=::unlink(recovery.c_str())==0;
        return {ExistingFileOutcome::Cancelled,"Cancelled before replacement",removed ? std::string() : recovery};
    }
#ifdef VACARDS_FILE_IO_TEST_HOOKS
    int const injected_rename_errno = injected_stage_rename_failure ? EIO : options.rename_failure_errno;
    int const renamed = injected_rename_errno ? -1 : ::rename(stage.path.c_str(), path.c_str());
    int const rename_error = injected_rename_errno ? injected_rename_errno : (renamed == 0 ? 0 : errno);
#else
    int const renamed = injected_stage_rename_failure ? -1 : ::rename(stage.path.c_str(), path.c_str());
    int const rename_error = injected_stage_rename_failure ? EIO : (renamed == 0 ? 0 : errno);
#endif
    std::string after_digest;
    bool const dest_is_new = sha256_file(path, after_digest) && after_digest == new_digest;
    struct stat stage_after{};
    int const stage_probe = ::lstat(stage.path.c_str(), &stage_after);
    bool const stage_still_exists = stage_probe == 0;
    bool const stage_gone = stage_probe != 0 && errno == ENOENT;
    if (!dest_is_new || !stage_gone) {
        if (renamed != 0 && stage_still_exists && after_digest == old_digest
            && current_matches(path, baseline)) {
            int const removed = options.recovery_delete_failure_errno
                ? (errno = options.recovery_delete_failure_errno, -1) : ::unlink(recovery.c_str());
            if (removed == 0) {
                return before_failure(error_text("replace SMB destination", rename_error));
            }
            return before_failure(error_text("replace SMB destination", rename_error)
                + "; redundant recovery may remain at " + recovery);
        }
        stage.owned = false; // ambiguous: retain the complete staged candidate.
        return {ExistingFileOutcome::Uncertain,
                "SMB replacement outcome could not be verified; inspect recovery " + recovery
                    + (!stage_gone ? " and possible staged candidate " + stage.path : ""),
                recovery};
    }
    stage.owned = false; // stage name was consumed by the rename.
    if (::fsync(directory_fd) != 0) {
        return {ExistingFileOutcome::Uncertain,
                "SMB destination has new bytes but directory sync failed; old version retained",
                recovery};
    }
    if (options.stage_observer && options.stage_observer(5)) {
        return {ExistingFileOutcome::Published,
                "injected recovery cleanup failure", recovery};
    }
    if (remove_confirmed_recovery(recovery, options) != 0) {
        return {ExistingFileOutcome::Published,
                error_text("remove SMB recovery copy", errno), recovery};
    }
    return {ExistingFileOutcome::Published, {}, {}};
}

} // namespace

ExistingFileOptions capture_existing_file_options(bool timing)
{
    ExistingFileOptions options;
    options.timing = timing;
#ifdef VACARDS_FILE_IO_TEST_HOOKS
    options.rename_failure_errno = detail::consume_injected_rename_errno();
    options.recovery_delete_failure_errno = detail::consume_injected_recovery_delete_errno();
#endif
    return options;
}

ExistingFileResult replace_existing_local_file(
    std::string const &path, std::function<void(FILE *)> const &writer)
{
    return replace_existing_local_file(path, writer,
        capture_existing_file_options(save_timing_enabled()));
}

ExistingFileResult replace_existing_local_file(
    std::string const &path, std::function<void(FILE *)> const &writer,
    ExistingFileOptions const &options)
{
    ExistingFileOutcome expected_failure=ExistingFileOutcome::Conflict;
    auto matches_expected = [&] {
        if (!options.expected_version) return true;
        auto found = inspect_existing_file_version(path);
        expected_failure=found.outcome==ExistingFileOutcome::Unavailable ? ExistingFileOutcome::Unavailable : ExistingFileOutcome::Conflict;
        auto const &e = *options.expected_version;
        return found.version && found.version->identity == e.identity &&
               found.version->bytes == e.bytes && found.version->sha256 == e.sha256;
    };
    if (!matches_expected()) return {expected_failure, "expected version mismatch before staging", {}};
    auto const admission_begin = SaveClock::now();
    if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos || !writer) {
        return {ExistingFileOutcome::Unsupported, "existing-file replacement requires an absolute path and a writer", {}};
    }

    // Capture the save start before any staging work. The stage inherits the
    // old destination times through COPYFILE_METADATA, so the published mtime
    // must be restored to a value no earlier than this instant.
    struct timespec save_start{};
    if (::clock_gettime(CLOCK_REALTIME, &save_start) != 0) {
        return before_failure(error_text("read save start time", errno));
    }

    auto const parts = split_save_path(path);
    if (!parts) {
        return {ExistingFileOutcome::Unsupported, "destination must name a file", {}};
    }
    std::string const &parent = parts->parent;
    ScopedFd directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY));
    if (directory.get() < 0) {
        return {ExistingFileOutcome::Unsupported, error_text("open destination directory", errno), {}};
    }
    struct statfs fs{};
    if (::fstatfs(directory.get(), &fs) != 0) {
        return {ExistingFileOutcome::Unsupported, "existing-file replacement requires an identifiable filesystem", {}};
    }
    // This path is exercised on one NAS appliance's SMB 3.1.1 share. Other SMB server
    // types still require independent qualification; a protocol name alone
    // does not prove identical rename or durability semantics.
    bool const smb = !(fs.f_flags & MNT_LOCAL) && std::strcmp(fs.f_fstypename, "smbfs") == 0;
    if (!(fs.f_flags & MNT_LOCAL) && !smb) {
        return {ExistingFileOutcome::Unsupported, "existing-file replacement requires a local or SMB filesystem", {}};
    }

    ScopedFd original(::open(path.c_str(), O_RDONLY | O_NOFOLLOW));
    if (original.get() < 0) {
        return {ExistingFileOutcome::Unsupported, error_text("open existing destination", errno), {}};
    }
    struct stat baseline{};
    if (::fstat(original.get(), &baseline) != 0 || !S_ISREG(baseline.st_mode)
        || baseline.st_nlink != 1) {
        return {ExistingFileOutcome::Unsupported,
                "destination must be a regular file with one hard link", {}};
    }
    // Reject before any staging work: COPYFILE_METADATA would propagate these
    // system-enforced flags onto the owned stage, and the replacement cannot be
    // published over an immutable or append-only destination anyway.
    if ((baseline.st_flags & (UF_IMMUTABLE | SF_IMMUTABLE | UF_APPEND | SF_APPEND)) != 0) {
        return {ExistingFileOutcome::Unsupported,
                "destination is immutable or append-only", {}};
    }
    if (!current_matches(path, baseline)) {
        return {ExistingFileOutcome::Conflict, "destination changed before staging", {}};
    }

    ScopedStage stage(parent + "/vacards-save-XXXXXX");
    stage.timing = options.timing;
    stage.fd = ::mkstemp(stage.path.data());
    if (stage.fd < 0) {
        return before_failure(error_text("create sibling stage", errno));
    }
    stage.stream = ::fdopen(stage.fd, "wb");
    if (!stage.stream) {
        return before_failure(error_text("open sibling stage stream", errno));
    }
    stage.fd = -1; // FILE now owns the descriptor.
    save_timing_span("admission", admission_begin, options.timing);
    if (options.stage_observer && options.stage_observer(1))
        return before_failure("injected stage-create failure");

    auto const write_begin = SaveClock::now();
    try {
        writer(stage.stream);
    } catch (std::exception const &e) {
        return before_failure(std::string("serialize staging file: ") + e.what());
    } catch (...) {
        return before_failure("serialize staging file failed");
    }
    save_timing_span("stage_write", write_begin, options.timing);
    if (options.stage_observer && options.stage_observer(2))
        return before_failure("injected stage-write failure");
    auto const seal_begin = SaveClock::now();
    if (std::fflush(stage.stream) != 0 || std::ferror(stage.stream) != 0) {
        return before_failure(error_text("flush staging file", errno));
    }
    auto const metadata_begin = SaveClock::now();
    if (::fcopyfile(original.get(), ::fileno(stage.stream), nullptr, COPYFILE_METADATA) != 0) {
        return before_failure(error_text("copy destination metadata", errno));
    }
    // VIEW-1: an SVG's Finder icon shows the drawing of the save that made
    // it; the new version must not keep the replaced file's icon (a document
    // save sets a new one afterwards).
    {
        std::string lower = path;
        for (auto &c : lower) c = g_ascii_tolower(c);
        if (g_str_has_suffix(lower.c_str(), ".svg") || g_str_has_suffix(lower.c_str(), ".svgz")) {
            clear_finder_icon(::fileno(stage.stream));
        }
    }
    struct stat staged_metadata{};
    if (::fstat(::fileno(stage.stream), &staged_metadata) != 0
        || (staged_metadata.st_mode & 07777) != (baseline.st_mode & 07777)
        || staged_metadata.st_uid != baseline.st_uid
        || staged_metadata.st_gid != baseline.st_gid
        || staged_metadata.st_flags != baseline.st_flags) {
        return before_failure("staged destination metadata could not be verified");
    }
    // COPYFILE_METADATA restored the old destination's times onto the stage.
    // Re-stamp the modification time to the save start; keep the copied atime
    // untouched where UTIME_OMIT is available. This runs before fsync so the
    // corrected time is what the published namespace exposes.
    struct timespec stage_times[2];
#ifdef UTIME_OMIT
    stage_times[0].tv_sec = 0;
    stage_times[0].tv_nsec = UTIME_OMIT;
    stage_times[1] = save_start;
#else
    stage_times[0] = save_start;
    stage_times[1] = save_start;
#endif
    if (::futimens(::fileno(stage.stream), stage_times) != 0) {
        return before_failure(error_text("set staging file modification time", errno));
    }
    save_timing_span("metadata", metadata_begin, options.timing);
    if (::fsync(::fileno(stage.stream)) != 0) {
        return before_failure(error_text("sync staging file", errno));
    }
    FILE *const stream = stage.stream;
    stage.stream = nullptr;
    if (std::fclose(stream) != 0) {
        return before_failure(error_text("close staging file", errno));
    }
    save_timing_span("flush_sync_close", seal_begin, options.timing);
    if (options.stage_observer && options.stage_observer(3))
        return before_failure("injected stage-seal failure");

    auto const publish_begin = SaveClock::now();

    if (!current_matches(path, baseline)) {
        return {ExistingFileOutcome::Conflict, "destination changed before replacement", {}};
    }

    if (smb) {
        return publish_smb_existing(path, parent, directory.get(), original, baseline, stage, options);
    }

    // A hard-link backup keeps the complete original reachable if rename or
    // directory sync has an ambiguous result. It is created only after the
    // stage is complete and the destination still matches the baseline.
    std::string backup;
    bool backup_created = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        gchar *uuid = g_uuid_string_random();
        backup = parent + "/vacards-recovery-" + uuid;
        g_free(uuid);
        if (::link(path.c_str(), backup.c_str()) == 0) {
            backup_created = true;
            break;
        }
        if (errno != EEXIST) {
            return before_failure(error_text("retain old destination", errno));
        }
    }
    if (!backup_created) {
        return before_failure("could not choose a recovery filename");
    }

    struct stat linked{};
    if (::lstat(backup.c_str(), &linked) != 0 || linked.st_dev != baseline.st_dev
        || linked.st_ino != baseline.st_ino || !current_matches(path, linked)) {
        ::unlink(backup.c_str());
        return {ExistingFileOutcome::Conflict, "destination changed while retaining old version", {}};
    }

    bool const injected_stage_rename_failure = options.stage_observer && options.stage_observer(4);


    if (!matches_expected()) {
        ::unlink(backup.c_str());
        return {expected_failure, "expected version mismatch before replacement", {}};
    }

    if (options.cancelled && options.cancelled()) {
        bool removed=::unlink(backup.c_str())==0;
        return {ExistingFileOutcome::Cancelled,"Cancelled before replacement",removed ? std::string() : backup};
    }

    // The backup itself increments the original's link count. Test builds may
    // inject one rename failure; production builds call rename directly.
#ifdef VACARDS_FILE_IO_TEST_HOOKS
    int const injected_rename_errno = injected_stage_rename_failure ? EIO : options.rename_failure_errno;
    if (injected_rename_errno != 0 || ::rename(stage.path.c_str(), path.c_str()) != 0) {
        int const saved = injected_rename_errno != 0 ? injected_rename_errno : errno;
#else
    if (injected_stage_rename_failure || ::rename(stage.path.c_str(), path.c_str()) != 0) {
        int const saved = injected_stage_rename_failure ? EIO : errno;
#endif
        // Probe identity only (not full version): the backup we just created
        // has already changed nlink, so a version compare would be misleading.
        struct stat path_now{};
        struct stat backup_now{};
        bool const path_is_original = ::lstat(path.c_str(), &path_now) == 0
            && path_now.st_dev == baseline.st_dev && path_now.st_ino == baseline.st_ino;
        bool const backup_is_original = ::lstat(backup.c_str(), &backup_now) == 0
            && backup_now.st_dev == baseline.st_dev && backup_now.st_ino == baseline.st_ino;
        // recovery_path means "a complete prior version is retained here", so it
        // may only name a location whose baseline device/inode this very probe
        // observed. A path whose identity is unproven is never reported.
        std::string const verified_recovery = backup_is_original ? backup : std::string();
        if (path_is_original && backup_is_original) {
            // The namespace still names the original inode and our verified
            // backup links to it, so publication definitely did not happen.
            // Drop the redundant link only after its identity was checked; an
            // unverified backup is never removed.
            int const removed = options.recovery_delete_failure_errno
                ? (errno = options.recovery_delete_failure_errno, -1) : ::unlink(backup.c_str());
            if (removed != 0) {
                int const unlink_errno = errno;
                return {ExistingFileOutcome::Uncertain,
                        error_text("replace destination", saved)
                            + "; the redundant recovery link could not be removed and may remain at "
                            + backup + ": " + g_strerror(unlink_errno),
                        backup};
            }
            // Removal succeeded, but do not claim a clean retry state until a
            // fresh probe proves the destination still names the baseline inode
            // with link count restored to one. A concurrent writer, or an extra
            // hard link, could otherwise leave an ambiguous state behind.
            struct stat path_after{};
            if (::lstat(path.c_str(), &path_after) == 0
                && path_after.st_dev == baseline.st_dev
                && path_after.st_ino == baseline.st_ino
                && path_after.st_nlink == 1) {
                (void)::fsync(directory.get());
                return before_failure(error_text("replace destination", saved));
            }
            return {ExistingFileOutcome::Uncertain,
                    error_text("replace destination", saved)
                        + "; destination identity or link count could not be confirmed after removing the recovery link",
                    {}};
        }
        // The destination or the owned backup no longer has its expected
        // identity. Publication may or may not have happened; retain any
        // verified backup, never remove one whose identity is unproven, and do
        // not advertise an unverified location as a prior version.
        return {ExistingFileOutcome::Uncertain,
                error_text("replace destination; inspect retained files", saved),
                verified_recovery};
    }
    stage.owned = false;
    if (::fsync(directory.get()) != 0) {
        return {ExistingFileOutcome::Uncertain,
                error_text("sync destination directory; replacement may be published", errno), backup};
    }
    // The old version is no longer needed after the replacement namespace is
    // durable. Removal failure is surfaced but does not negate publication.
    if (options.stage_observer && options.stage_observer(5)) {
        return {ExistingFileOutcome::Published,
                "injected recovery cleanup failure", backup};
    }
    if (remove_confirmed_recovery(backup, options) != 0) {
        return {ExistingFileOutcome::Published,
                error_text("remove old recovery copy", errno), backup};
    }
    (void)::fsync(directory.get()); // backup deletion durability is not save durability.
    save_timing_span("publish", publish_begin, options.timing);
    return {ExistingFileOutcome::Published, {}, {}};
}

} // namespace Inkscape::IO

#elif !defined(_WIN32)

namespace Inkscape::IO {
ExistingFileOptions capture_existing_file_options(bool timing) { ExistingFileOptions options; options.timing = timing; return options; }
ExistingFileResult replace_existing_local_file(
    std::string const &, std::function<void(FILE *)> const &, ExistingFileOptions const &)
{
    return {ExistingFileOutcome::Unsupported,
            "existing-file replacement is not yet implemented on this platform", {}};
}
ExistingFileResult replace_existing_local_file(
    std::string const &, std::function<void(FILE *)> const &)
{
    return {ExistingFileOutcome::Unsupported,
            "existing-file replacement is not yet implemented on this platform", {}};
}
} // namespace Inkscape::IO

#endif

// M2 guarded overload preserves legacy callers and routes checks into native boundaries.
namespace Inkscape::IO {
ExistingFileResult replace_existing_local_file(std::string const &path,
    std::function<void(FILE *)> const &writer, ExpectedFileVersion const &expected,
    ExistingFileOptions const &options)
{
    auto guarded = options;
    guarded.expected_version = expected;
    return replace_existing_local_file(path, writer, guarded);
}
} // namespace Inkscape::IO
#ifndef _WIN32
#include <glib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace Inkscape::IO {
FileVersionResult inspect_existing_file_version(std::string const &path)
{
    auto conflict = [](char const *s) { return FileVersionResult{std::nullopt, ExistingFileOutcome::Conflict, s}; };
    if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos)
        return conflict("Version inspection requires an absolute local path");
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return {std::nullopt, errno==ENOENT ? ExistingFileOutcome::Conflict : ExistingFileOutcome::Unavailable,"Cannot open destination version"};
    struct Owner { int fd; ~Owner() { ::close(fd); } } owner{fd};
    struct stat a{}, b{}, named{};
    if(::fstat(fd,&a)) return {std::nullopt,ExistingFileOutcome::Unavailable,"Version metadata unavailable"};
    if (!S_ISREG(a.st_mode)) return conflict("Not a regular file");
    auto sum = g_checksum_new(G_CHECKSUM_SHA256);
    struct Sum { GChecksum *p; ~Sum() { g_checksum_free(p); } } sum_owner{sum};
    unsigned char buffer[65536]; std::uint64_t size = 0;
    for (;;) {
        auto n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) return {std::nullopt,ExistingFileOutcome::Unavailable,"Version read failed"};
        if (!n) break;
        size += n; g_checksum_update(sum, buffer, n);
    }
    if (::fstat(fd, &b) || ::lstat(path.c_str(), &named) ||
        a.st_dev != b.st_dev || a.st_ino != b.st_ino || a.st_size != b.st_size ||
        a.st_mtime != b.st_mtime || a.st_ctime != b.st_ctime ||
        b.st_dev != named.st_dev || b.st_ino != named.st_ino || size != std::uint64_t(b.st_size))
        return conflict("Destination changed while hashing");
#ifdef __APPLE__
    if (a.st_mtimespec.tv_nsec != b.st_mtimespec.tv_nsec || a.st_ctimespec.tv_nsec != b.st_ctimespec.tv_nsec)
        return conflict("Destination changed while hashing");
#endif
    return {ExpectedFileVersion{std::to_string(b.st_dev) + ":" + std::to_string(b.st_ino),
            g_checksum_get_string(sum), size}, ExistingFileOutcome::Published, {}};
}
} // namespace Inkscape::IO
#endif
