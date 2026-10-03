// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * POSIX platform adapter for the F3a new-file storage packet.
 *
 * New-file publication uses link(stage, final): link(2) is an atomic entry
 * creation that fails with EEXIST rather than replacing an existing
 * destination, and the stage is a sibling on the same filesystem. The adapter
 * ONLY creates that namespace entry; it never unlinks the staging path. Stage
 * cleanup is owned exactly once by the transaction layer, which also reports
 * cleanup success/failure.
 *
 * macOS SMB shares (smbfs) may support neither link(2) nor RENAME_EXCL. There,
 * and only there, publication claims the name with an exclusive create
 * (O_CREAT|O_EXCL, an atomic server-side create), re-checks that the claimed
 * entry is still the same empty file, and renames the sealed stage over it,
 * which consumes the stage path. A destination that existed before the claim
 * is never replaced. The re-check sees this client's view only: smbfs caches
 * attributes, so another SMB client writing the claimed name in the short
 * window before the rename is not detected (owner-accepted), nor is the empty
 * file a crash between claim and rename can leave under the new name.
 * There is no delete-then-rename and no copy fallback. Errors that cannot be
 * proven to have left the namespace unchanged are reported as Uncertain so the
 * caller retains the staging copy.
 */

#ifndef _WIN32

#include "document-file-transaction.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/param.h>
#endif

#include <glib.h>
#include <glib/gstdio.h>

namespace Inkscape::IO::DocumentTransaction {

namespace {

// link(2) errors that prove the destination entry was not created and the
// namespace is unchanged. Anything else (EIO/EINTR/ENOMEM/... on remote or
// interrupted storage) is treated as ambiguous so the staging copy is kept.
bool link_error_is_definite(int error) noexcept
{
    switch (error) {
    case EACCES:
    case EPERM:
    case ENOENT:
    case ENOTDIR:
    case EROFS:
    case ENOSPC:
    case EMLINK:
    case ENAMETOOLONG:
    case EXDEV:
#ifdef EDQUOT
    case EDQUOT:
#endif
#ifdef ELOOP
    case ELOOP:
#endif
        return true;
    default:
        return false;
    }
}

} // namespace

/**
 * Classify a link(2) failure errno into the publication outcome.
 *
 * Externally linkable so the focused classifier tests can exercise the exact
 * mapping without a public header declaration; this stays an adapter-internal
 * detail of the POSIX backend. Definite errors prove no namespace entry was
 * created. ENOTSUP/EOPNOTSUPP are Unsupported capability gaps; EEXIST is a
 * no-clobber Conflict. EIO/EINTR and anything not known to be definite are
 * Uncertain so the caller keeps the staging copy.
 */
PublicationStatus classify_link_errno(int error) noexcept
{
    if (error == EEXIST) {
        return PublicationStatus::Conflict;
    }
    // ENOTSUP and EOPNOTSUPP alias on several platforms; check both without
    // relying on them being distinct case labels.
#ifdef EOPNOTSUPP
    if (error == EOPNOTSUPP) {
        return PublicationStatus::Unsupported;
    }
#endif
#ifdef ENOTSUP
    if (error == ENOTSUP) {
        return PublicationStatus::Unsupported;
    }
#endif
    if (error == EIO || error == EINTR) {
        return PublicationStatus::Uncertain;
    }
    if (link_error_is_definite(error)) {
        return PublicationStatus::Failed;
    }
    return PublicationStatus::Uncertain;
}

namespace {

bool same_empty_regular_file(struct stat const &current, struct stat const &claim) noexcept
{
    return S_ISREG(current.st_mode) && current.st_dev == claim.st_dev
        && current.st_ino == claim.st_ino && current.st_size == 0;
}

// Remove our own claim only while it is still the same untouched empty file.
void release_claim(std::string const &final_path, struct stat const &claim) noexcept
{
    struct stat current{};
    if (::lstat(final_path.c_str(), &current) == 0 && same_empty_regular_file(current, claim)) {
        ::unlink(final_path.c_str());
    }
}

// True for a macOS SMB mount, the only filesystem qualified for the claim fallback.
bool parent_is_smb(std::string const &final_path)
{
#ifdef __APPLE__
    gchar *parent = g_path_get_dirname(final_path.c_str());
    struct statfs fs{};
    bool const ok = ::statfs(parent, &fs) == 0;
    g_free(parent);
    return ok && !(fs.f_flags & MNT_LOCAL) && std::strcmp(fs.f_fstypename, "smbfs") == 0;
#else
    (void)final_path;
    return false;
#endif
}

} // namespace

/**
 * No-clobber publication without link(2): claim the name with an exclusive
 * create, verify the claim is still our empty file, then rename the sealed
 * stage over it. The stage path is consumed by the rename; the transaction
 * layer's later stage cleanup then finds nothing to remove.
 *
 * Externally linkable (no header declaration) so the focused tests can run it
 * on a local directory, where rename(2) has the same semantics. `after_claim`
 * is a test seam invoked between the claim and the re-check; production passes
 * nullptr.
 */
PublicationStatus publish_by_claim_and_rename(std::string const &staged_path,
                                              std::string const &final_path, std::string &error,
                                              void (*after_claim)(std::string const &final_path))
{
    struct stat stage{};
    if (::lstat(staged_path.c_str(), &stage) != 0 || !S_ISREG(stage.st_mode)) {
        error = "staged file is missing before publication";
        return PublicationStatus::Failed;
    }

    int const fd = ::open(final_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (fd < 0) {
        int const saved = errno;
        PublicationStatus const status = classify_link_errno(saved);
        switch (status) {
        case PublicationStatus::Conflict:
            error = "destination already exists";
            break;
        case PublicationStatus::Unsupported:
            error = std::string("publication primitive unsupported: ") + g_strerror(saved);
            break;
        case PublicationStatus::Failed:
            error = g_strerror(saved);
            break;
        default:
            error = std::string("ambiguous exclusive create: ") + g_strerror(saved);
            break;
        }
        return status;
    }
    struct stat claim{};
    if (::fstat(fd, &claim) != 0) {
        int const saved = errno;
        ::close(fd);
        // Created exclusively a moment ago and never written; still ours.
        struct stat current{};
        if (::lstat(final_path.c_str(), &current) == 0 && S_ISREG(current.st_mode) && current.st_size == 0) {
            ::unlink(final_path.c_str());
        }
        error = std::string("could not identify the claimed destination: ") + g_strerror(saved);
        return PublicationStatus::Failed;
    }
    ::close(fd); // smbfs must not rename over an open file

    if (after_claim) {
        after_claim(final_path);
    }

    struct stat current{};
    if (::lstat(final_path.c_str(), &current) != 0 || !same_empty_regular_file(current, claim)) {
        // Someone else changed or replaced the claimed name; leave it alone.
        error = "destination changed after it was claimed";
        return PublicationStatus::Conflict;
    }

    if (::rename(staged_path.c_str(), final_path.c_str()) != 0) {
        int const saved = errno;
        // A lost SMB reply can report failure for a rename the server applied:
        // the stage is gone and the name holds a file of the stage's size.
        struct stat left{};
        struct stat published{};
        if (::lstat(staged_path.c_str(), &left) != 0 && errno == ENOENT
            && ::lstat(final_path.c_str(), &published) == 0 && S_ISREG(published.st_mode)
            && published.st_size == stage.st_size) {
            error.clear();
            return PublicationStatus::Published;
        }
        // EBUSY: a viewer holds the fresh claim; nothing was renamed.
        if (saved == EBUSY || link_error_is_definite(saved)) {
            release_claim(final_path, claim);
            error = std::string("publish by rename failed: ") + g_strerror(saved);
            return PublicationStatus::Failed;
        }
        error = std::string("ambiguous rename failure: ") + g_strerror(saved)
            + "; inspect " + final_path;
        return PublicationStatus::Uncertain;
    }
    error.clear();
    return PublicationStatus::Published;
}

namespace {

class PosixSystemCalls final : public SystemCalls
{
public:
    bool create_exclusive_file(std::string &path_template, FILE *&out,
                               bool &already_exists, std::string &error) override
    {
        already_exists = false;
        int const fd = g_mkstemp_full(path_template.data(), O_RDWR, 0666);
        if (fd < 0) {
            int const saved = errno; // capture before any cleanup/formatting
            if (saved == EEXIST) {
                already_exists = true;
            }
            error = g_strerror(saved);
            return false;
        }

        FILE *stream = ::fdopen(fd, "wb");
        if (!stream) {
            // The exclusive stage now exists on disk but has no stream. Unlink
            // it ourselves (this is adapter-internal create failure, before any
            // transaction object exists) and preserve the original errno.
            int const saved = errno;
            ::close(fd);
            g_remove(path_template.c_str());
            errno = saved;
            error = g_strerror(saved);
            return false;
        }
        out = stream;
        return true;
    }

    bool flush_file(FILE *stream, std::string &error) override
    {
        if (!stream) {
            error = "null staging stream";
            return false;
        }
        if (std::fflush(stream) != 0) {
            int const saved = errno;
            error = g_strerror(saved);
            return false;
        }
        if (std::ferror(stream) != 0) {
            error = "staging stream error flag set";
            return false;
        }
        return true;
    }

    bool sync_file(FILE *stream, bool &unsupported, std::string &error) override
    {
        unsupported = false;
        if (!stream) {
            error = "null staging stream";
            return false;
        }
        int const fd = ::fileno(stream);
        if (fd < 0) {
            int const saved = errno;
            error = g_strerror(saved);
            return false;
        }
        if (::fsync(fd) != 0) {
            int const saved = errno;
            if (saved == EINVAL) {
                // Some filesystems/providers do not implement fsync on this
                // descriptor; record the capability instead of inventing it.
                unsupported = true;
                return true;
            }
#ifdef EOPNOTSUPP
            if (saved == EOPNOTSUPP) {
                unsupported = true;
                return true;
            }
#endif
#ifdef ENOTSUP
            if (saved == ENOTSUP) {
                unsupported = true;
                return true;
            }
#endif
            error = g_strerror(saved);
            return false;
        }
        return true;
    }

    bool close_file(FILE *stream, std::string &error) override
    {
        if (!stream) {
            error = "null staging stream";
            return false;
        }
        errno = 0;
        int const rc = std::fclose(stream);
        if (rc != 0) {
            int const saved = errno;
            error = (saved != 0) ? g_strerror(saved) : "fclose failed";
            return false;
        }
        return true;
    }

    PublicationStatus publish_new_file(std::string const &staged_path,
                                       std::string const &final_path,
                                       std::string &error) override
    {
        if (::link(staged_path.c_str(), final_path.c_str()) != 0) {
            int const saved = errno;
            PublicationStatus const status = classify_link_errno(saved);
            if (status == PublicationStatus::Unsupported && parent_is_smb(final_path)) {
                return publish_by_claim_and_rename(staged_path, final_path, error, nullptr);
            }
            switch (status) {
            case PublicationStatus::Conflict:
                error = "destination already exists";
                break;
            case PublicationStatus::Unsupported:
                error = std::string("publication primitive unsupported: ") + g_strerror(saved);
                break;
            case PublicationStatus::Failed:
                error = g_strerror(saved);
                break;
            default:
                // Remote/interrupted I/O: the entry may or may not exist now.
                error = std::string("ambiguous link failure: ") + g_strerror(saved);
                break;
            }
            return status;
        }

        // Namespace entry confirmed. The staging path is still linked; the
        // transaction layer owns its single cleanup (and reports failure).
        error.clear();
        return PublicationStatus::Published;
    }

    // Only a proven absence is reported; presence stays Unverified because a
    // pathname check does not prove the recorded identity or its bytes.
    StagingAvailability retained_stage_availability(std::string const &staged_path,
                                                    std::string &error) override
    {
        struct stat current{};
        if (::lstat(staged_path.c_str(), &current) != 0 && errno == ENOENT) {
            error = "staging file is no longer present";
            return StagingAvailability::Missing;
        }
        return StagingAvailability::Unverified;
    }

    bool remove_file(std::string const &path) noexcept override
    {
        return g_remove(path.c_str()) == 0 || errno == ENOENT;
    }

    bool sync_parent_directory(std::string const &parent, bool &unsupported,
                               std::string &error) override
    {
        unsupported = false;
        if (parent.empty()) {
            error = "empty parent directory";
            return false;
        }
        int const fd = g_open(parent.c_str(), O_RDONLY, 0);
        if (fd < 0) {
            int const saved = errno;
            error = g_strerror(saved);
            return false;
        }

        bool ok = true;
        if (::fsync(fd) != 0) {
            int const saved = errno;
            if (saved == EINVAL) {
                unsupported = true;
            }
#ifdef EOPNOTSUPP
            else if (saved == EOPNOTSUPP) {
                unsupported = true;
            }
#endif
#ifdef ENOTSUP
            else if (saved == ENOTSUP) {
                unsupported = true;
            }
#endif
            else {
                error = g_strerror(saved);
                ok = false;
            }
        }
        ::close(fd);
        return ok;
    }
};

} // namespace

std::unique_ptr<SystemCalls> make_platform_system_calls()
{
    return std::make_unique<PosixSystemCalls>();
}

} // namespace Inkscape::IO::DocumentTransaction

#endif // !_WIN32
