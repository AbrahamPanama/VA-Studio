// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Windows local-NTFS adapter for the bounded F3b new-file packet.
 *
 * Publication is CreateHardLinkW only: it creates the destination namespace
 * entry without consuming the staged name and without ever replacing an
 * existing destination. There is no rename, no copy, no delete-then-create and
 * no final-path cleanup. NAS/SMB/UNC, mapped drives, ReFS and FAT/exFAT are
 * explicit Unsupported capability gaps, rejected before any payload.
 *
 * File identities are private to this translation unit. The adapter records
 * the volume serial plus file index of each staging file, keyed by the exact
 * logical path it created, while it still owns the created handle. Cleanup and
 * availability re-check that identity through the handle they act on; a
 * check-then-DeleteFileW-by-name path is never used.
 *
 * Ownership of a created staging object is carried by narrow file-private RAII
 * owners across the CreateFileW -> _open_osfhandle -> _fdopen -> map-insertion
 * bridge. Any error or exception before the FILE* reaches the caller disposes
 * exactly that object through its own handle and closes exactly once; the
 * caller never receives an untracked stream.
 *
 * Admission opens the actual resolved parent (no FILE_FLAG_OPEN_REPARSE_POINT)
 * and proves local NTFS + hard links from that handle and its resolved volume.
 * A resolved volume root that cannot be shown to be a local fixed/removable
 * drive is rejected as Unsupported before any staging file is created; there is
 * no fallback to the public logical drive spelling.
 *
 * Namespace power-loss durability is explicitly unsupported
 * (sync_parent_directory reports unsupported). A successful FlushFileBuffers is
 * file durability only. Parent-component, reparse and hostile-writer races
 * remain documented caller-owned limitations; this adapter makes no
 * hostile-writer immunity claim.
 */

#ifdef _WIN32

#include "document-file-transaction.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winnetwk.h>

#include <fcntl.h>
#include <io.h>
#include <wchar.h>

#include <glib.h>

#include "util-string/string-convert.h"

namespace Inkscape::IO::DocumentTransaction {

namespace {

/// Private identity POD. Never named in the portable header.
struct FileIdentity {
    DWORD volume = 0;
    DWORD index_high = 0;
    DWORD index_low = 0;
};

bool identity_matches(BY_HANDLE_FILE_INFORMATION const &info, FileIdentity const &expected) noexcept
{
    return info.dwVolumeSerialNumber == expected.volume
        && info.nFileIndexHigh == expected.index_high
        && info.nFileIndexLow == expected.index_low;
}

std::string windows_error(char const *operation, DWORD code)
{
    return std::string(operation) + ": Windows error "
        + std::to_string(static_cast<unsigned long>(code));
}

/// Capture the created object's volume serial + file index from its own handle.
/// Reports a specific `error`; the non-disk outcome never pairs a diagnostic
/// with a stale GetLastError() value.
bool read_identity(HANDLE handle, FileIdentity &out, std::string &error)
{
    DWORD const type = GetFileType(handle);
    if (type != FILE_TYPE_DISK) {
        if (type == FILE_TYPE_UNKNOWN) {
            error = windows_error("query created staging file type", GetLastError());
        } else {
            error = "created staging object is not a disk file";
        }
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        error = windows_error("identify created staging file", GetLastError());
        return false;
    }
    out.volume = info.dwVolumeSerialNumber;
    out.index_high = info.nFileIndexHigh;
    out.index_low = info.nFileIndexLow;
    return true;
}

/// Close-only RAII guard for handles this adapter opened but did not create
/// (admission, availability, cleanup). It never marks anything for deletion.
class ScopedHandle
{
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~ScopedHandle()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    ScopedHandle(ScopedHandle const &) = delete;
    ScopedHandle &operator=(ScopedHandle const &) = delete;
    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

/// RAII owner for the handle of an object this adapter just created. Unless it
/// is released (ownership transferred), the destructor marks the object for
/// deletion through that very handle and then closes exactly once. A failed
/// disposition still closes; disposal is best-effort and never by pathname.
class CreatedHandleOwner
{
public:
    explicit CreatedHandleOwner(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~CreatedHandleOwner() { dispose(); }
    CreatedHandleOwner(CreatedHandleOwner const &) = delete;
    CreatedHandleOwner &operator=(CreatedHandleOwner const &) = delete;
    HANDLE release() noexcept
    {
        HANDLE const transferred = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return transferred;
    }

private:
    void dispose() noexcept
    {
        if (handle_ == INVALID_HANDLE_VALUE) {
            return;
        }
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        SetFileInformationByHandle(handle_, FileDispositionInfo, &disposition, sizeof(disposition));
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    HANDLE handle_;
};

/// RAII owner for the CRT descriptor bridged from a created handle. Unless it is
/// released, the destructor marks the object for deletion through the
/// descriptor's native handle and then `_close()`s it exactly once.
class CreatedFdOwner
{
public:
    explicit CreatedFdOwner(int fd = -1) noexcept : fd_(fd) {}
    ~CreatedFdOwner() { dispose(); }
    CreatedFdOwner(CreatedFdOwner const &) = delete;
    CreatedFdOwner &operator=(CreatedFdOwner const &) = delete;
    int release() noexcept
    {
        int const transferred = fd_;
        fd_ = -1;
        return transferred;
    }

private:
    void dispose() noexcept
    {
        if (fd_ < 0) {
            return;
        }
        intptr_t const raw = _get_osfhandle(fd_);
        if (raw != -1) {
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            SetFileInformationByHandle(reinterpret_cast<HANDLE>(raw), FileDispositionInfo, &disposition,
                                       sizeof(disposition));
        }
        _close(fd_);
        fd_ = -1;
    }
    int fd_;
};

/// RAII owner for the stream bridged from a created descriptor. Unless it is
/// released, the destructor marks the object for deletion through the stream's
/// descriptor and `fclose()`s it exactly once.
class CreatedStreamOwner
{
public:
    explicit CreatedStreamOwner(FILE *stream = nullptr) noexcept : stream_(stream) {}
    ~CreatedStreamOwner() { dispose(); }
    CreatedStreamOwner(CreatedStreamOwner const &) = delete;
    CreatedStreamOwner &operator=(CreatedStreamOwner const &) = delete;
    FILE *release() noexcept
    {
        FILE *const transferred = stream_;
        stream_ = nullptr;
        return transferred;
    }

private:
    void dispose() noexcept
    {
        if (!stream_) {
            return;
        }
        int const fd = _fileno(stream_);
        if (fd >= 0) {
            intptr_t const raw = _get_osfhandle(fd);
            if (raw != -1) {
                FILE_DISPOSITION_INFO disposition{};
                disposition.DeleteFile = TRUE;
                SetFileInformationByHandle(reinterpret_cast<HANDLE>(raw), FileDispositionInfo,
                                           &disposition, sizeof(disposition));
            }
        }
        std::fclose(stream_);
        stream_ = nullptr;
    }
    FILE *stream_;
};

/// Plain Win32 long-path UTF-16 form. Converts only the adapter's copy of a
/// validated logical spelling; forward slashes become native backslashes and a
/// drive-absolute path gets the \\?\ prefix. UNC gets \\?\UNC\.
std::wstring to_extended_wide(std::string const &logical)
{
    std::string normalized = logical;
    for (char &c : normalized) {
        if (c == '/') {
            c = '\\';
        }
    }
    std::string extended;
    if (normalized.size() >= 2 && normalized[0] == '\\' && normalized[1] == '\\') {
        extended = "\\\\?\\UNC\\" + normalized.substr(2);
    } else {
        extended = "\\\\?\\" + normalized;
    }
    return Inkscape::utf8_to_wstring(extended);
}

// GetVolumeInformationByHandleW documents MAX_PATH+1 as the maximum size of the
// file system name buffer. This caps a short file system string ("NTFS"); it is
// not a path-length proof, and every path/volume buffer below is sized from the
// resolved handle instead of from MAX_PATH.
constexpr DWORD VOLUME_FS_NAME_MAX = MAX_PATH + 1;

/// Fully resolved path of an open handle using checked two-call sizing. The DOS
/// drive-letter form is tried first; volumes without a drive letter fall back to
/// the volume GUID form. Returns false with `error` set on failure.
bool final_path_by_handle(HANDLE handle, std::wstring &out, std::string &error)
{
    auto query = [handle](DWORD flags, std::wstring &into) -> bool {
        // Probe with the smallest buffer; a too-small buffer reports the
        // required size (including the terminator) rather than failing.
        std::vector<wchar_t> probe(1, L'\0');
        DWORD const required = GetFinalPathNameByHandleW(handle, probe.data(),
                                                         static_cast<DWORD>(probe.size()), flags);
        if (required == 0) {
            return false;
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1, L'\0');
        DWORD const written = GetFinalPathNameByHandleW(handle, buffer.data(),
                                                        static_cast<DWORD>(buffer.size()), flags);
        if (written == 0 || static_cast<std::size_t>(written) >= buffer.size()) {
            return false;
        }
        into.assign(buffer.data(), written);
        return true;
    };

    if (query(FILE_NAME_NORMALIZED | VOLUME_NAME_DOS, out)) {
        return true;
    }
    if (query(FILE_NAME_NORMALIZED | VOLUME_NAME_GUID, out)) {
        return true;
    }
    error = windows_error("GetFinalPathNameByHandleW", GetLastError());
    return false;
}

/// Resolve a `\\?\Volume{GUID}\` root to a DOS drive-letter mount point so
/// GetDriveTypeW can classify it. A volume mounted only into a folder has no
/// drive letter and is deliberately left unproven here (conservative reject).
bool guid_volume_drive_root(std::wstring const &volume_root, std::wstring &drive_root,
                            std::string &error)
{
    std::vector<wchar_t> probe(1, L'\0');
    DWORD needed = 0;
    if (!GetVolumePathNamesForVolumeNameW(volume_root.c_str(), probe.data(),
                                          static_cast<DWORD>(probe.size()), &needed)
        && GetLastError() != ERROR_MORE_DATA) {
        error = windows_error("query volume GUID mount points", GetLastError());
        return false;
    }
    if (needed == 0) {
        error = "volume has no mount point; locality is not provable";
        return false;
    }

    std::vector<wchar_t> names(static_cast<std::size_t>(needed) + 1, L'\0');
    DWORD written = 0;
    if (!GetVolumePathNamesForVolumeNameW(volume_root.c_str(), names.data(),
                                          static_cast<DWORD>(names.size()), &written)) {
        error = windows_error("read volume GUID mount points", GetLastError());
        return false;
    }

    // names is a MULTI_SZ list; accept the first drive-letter mount point.
    for (wchar_t const *entry = names.data(); *entry != L'\0';
         entry += std::wcslen(entry) + 1) {
        std::wstring const mount(entry);
        if (mount.size() >= 3 && mount[1] == L':'
            && (mount[2] == L'\\' || mount[2] == L'/')) {
            drive_root = mount;
            return true;
        }
    }
    error = "volume has no DOS drive-letter mount point; locality is not provable";
    return false;
}

/// Prove that the resolved parent behind `handle` lives on a local
/// fixed/removable drive. The volume root is derived from the handle's own
/// resolved path (never from the public logical drive spelling) and there is no
/// fallback: a root that cannot be positively resolved is rejected.
bool local_volume_drive_root(HANDLE handle, std::wstring &drive_root, std::string &error)
{
    std::wstring resolved;
    if (!final_path_by_handle(handle, resolved, error)) {
        return false;
    }

    // A volume mount point can never be longer than the resolved path that
    // contains it, so the resolved length is a checked dynamic bound.
    std::vector<wchar_t> root(resolved.size() + 2, L'\0');
    if (!GetVolumePathNameW(resolved.c_str(), root.data(), static_cast<DWORD>(root.size()))) {
        error = windows_error("resolve the resolved volume root", GetLastError());
        return false;
    }
    std::wstring const mount_point(root.data());

    if (mount_point.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        // \\?\UNC\server\share\ -> \\server\share\ for GetDriveTypeW.
        drive_root = L"\\\\" + mount_point.substr(8);
        return true;
    }
    if (mount_point.rfind(L"\\\\?\\Volume{", 0) == 0) {
        return guid_volume_drive_root(mount_point, drive_root, error);
    }
    if (mount_point.size() >= 7 && mount_point.rfind(L"\\\\?\\", 0) == 0
        && mount_point[5] == L':') {
        drive_root = mount_point.substr(4);
        return true;
    }
    if (mount_point.size() >= 3 && mount_point[1] == L':'
        && (mount_point[2] == L'\\' || mount_point[2] == L'/')) {
        drive_root = mount_point;
        return true;
    }
    error = "resolved volume root is not a recognized DOS path";
    return false;
}

enum class ShareKind { NotShare, Smb2, OtherProtocol, QueryFailed };

/// Classify a UNC root (\\server\share\) behind `handle`: SMB 2 or later is
/// admitted; WebDAV, RDP drive redirection and SMB1 are refused; a failed
/// protocol query is a momentary failure, not a capability claim.
ShareKind classify_share(HANDLE handle, std::wstring const &drive_root)
{
    if (drive_root.rfind(L"\\\\", 0) != 0) {
        return ShareKind::NotShare;
    }
    FILE_REMOTE_PROTOCOL_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileRemoteProtocolInfo, &info, sizeof info)) {
        // Some redirectors (VM shared folders, cloud drives) never answer this
        // query: that is not SMB, not a momentary failure.
        DWORD const code = GetLastError();
        if (code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION) {
            return ShareKind::OtherProtocol;
        }
        SetLastError(code);
        return ShareKind::QueryFailed;
    }
    return info.Protocol == WNNC_NET_SMB && info.ProtocolMajorVersion >= 2 ? ShareKind::Smb2
                                                                            : ShareKind::OtherProtocol;
}

class WindowsSystemCalls final : public SystemCalls
{
public:
    bool create_exclusive_file(std::string &path_template, FILE *&out,
                               bool &already_exists, std::string &error) override
    {
        already_exists = false;

        std::size_t const placeholder = path_template.rfind("XXXXXX");
        if (placeholder == std::string::npos) {
            error = "staging template is missing its random component";
            return false;
        }

        static char const symbols[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        for (int i = 0; i < 6; ++i) {
            path_template[placeholder + i] = symbols[g_random_int() % 36];
        }
        std::string const created_path = path_template;

        std::wstring wide;
        try {
            wide = to_extended_wide(created_path);
        } catch (...) {
            error = "cannot convert the staging path to UTF-16";
            return false;
        }

        // DELETE is requested so create-failure cleanup can dispose the object
        // through the very handle that created it rather than by name.
        HANDLE const handle = CreateFileW(wide.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                                          nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD const code = GetLastError();
            if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
                already_exists = true;
                error = "staging name collision";
                return false;
            }
            error = windows_error("create staging file", code);
            return false;
        }
        // From here every early return/exception must dispose this exact object
        // through its owning handle. `owner` owns the raw handle until the
        // bridge transfers ownership to the CRT descriptor.
        CreatedHandleOwner owner(handle);

        // Record identity from the actual created handle BEFORE the bridge
        // transfers ownership. If this fails the object is removed through the
        // same handle, so no unverifiable staging file is ever exposed.
        FileIdentity identity;
        if (!read_identity(handle, identity, error)) {
            return false;
        }

        int const fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_BINARY);
        if (fd < 0) {
            error = "could not bridge the staging handle to a descriptor";
            return false;
        }
        // _open_osfhandle succeeded: the descriptor now owns the OS handle, so
        // transfer ownership out of `owner` to avoid a double close.
        (void)owner.release();
        CreatedFdOwner fd_owner(fd);

        FILE *stream = _fdopen(fd, "wb");
        if (!stream) {
            error = "could not bridge the staging descriptor to a stream";
            return false;
        }
        // _fdopen succeeded: the stream now owns the descriptor.
        (void)fd_owner.release();
        CreatedStreamOwner stream_owner(stream);

        // Map insertion can throw (node allocation); `stream_owner` then removes
        // exactly this created object through its own handle and closes once.
        identities_.insert_or_assign(created_path, identity);
        if (admitted_remote_) {
            remote_stages_.insert(created_path);
        }

        // Final transfer point: the caller now owns the only remaining owner.
        out = stream_owner.release();
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
            error = (saved != 0) ? g_strerror(saved) : "fflush failed";
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
        int const fd = _fileno(stream);
        if (fd < 0) {
            error = "staging stream has no descriptor";
            return false;
        }
        intptr_t const raw = _get_osfhandle(fd);
        if (raw == -1) {
            error = "staging descriptor has no native handle";
            return false;
        }
        if (!FlushFileBuffers(reinterpret_cast<HANDLE>(raw))) {
            DWORD const code = GetLastError();
            if (code == ERROR_INVALID_FUNCTION || code == ERROR_NOT_SUPPORTED) {
                unsupported = true;
                return true;
            }
            error = windows_error("FlushFileBuffers", code);
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
        if (std::fclose(stream) != 0) {
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
        std::wstring staged_wide;
        std::wstring final_wide;
        try {
            staged_wide = to_extended_wide(staged_path);
            final_wide = to_extended_wide(final_path);
        } catch (...) {
            error = "cannot convert publication paths to UTF-16";
            return PublicationStatus::Uncertain;
        }

        if (remote_stages_.count(staged_path)) {
            return publish_by_rename(staged_path, staged_wide, final_wide, error);
        }

        // Non-consuming, no-clobber creation of the destination namespace
        // entry. The staged name is untouched on every outcome.
        if (CreateHardLinkW(final_wide.c_str(), staged_wide.c_str(), nullptr)) {
            error.clear();
            return PublicationStatus::Published;
        }

        DWORD const code = GetLastError();
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            error = "destination already exists";
            return PublicationStatus::Conflict;
        }
        if (code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION
            || code == ERROR_NOT_SAME_DEVICE) {
            error = windows_error("hard-link publication is unsupported here", code);
            return PublicationStatus::Unsupported;
        }
        // Every other hard-link failure is treated conservatively: the adapter
        // holds no documented per-error proof of the namespace state, so the
        // staging copy is retained as Uncertain rather than cleaned up.
        error = windows_error("hard-link publication outcome is ambiguous", code);
        return PublicationStatus::Uncertain;
    }

    bool remove_file(std::string const &path) noexcept override
    {
        auto const it = identities_.find(path);
        if (it == identities_.end()) {
            // Unknown/never-created staging path: never delete by name.
            return false;
        }
        FileIdentity const expected = it->second;

        bool removed = false;
        try {
            removed = remove_verified(path, expected);
        } catch (...) {
            removed = false;
        }

        // Consume the single cleanup attempt: never retried, and a path that is
        // later recreated by someone else is never deleted.
        identities_.erase(it);
        remote_stages_.erase(path);
        return removed;
    }

    bool sync_parent_directory(std::string const &, bool &unsupported, std::string &error) override
    {
        // Windows offers no portable directory-flush; namespace durability is a
        // separate, explicitly unsupported capability. File durability is
        // reported by sync_file()/FlushFileBuffers and is never implied here.
        unsupported = true;
        error.clear();
        return true;
    }

    FailureKind pre_create_support(std::string const &parent_dir, std::string &error) override
    {
        admitted_remote_ = false;
        std::wstring extended;
        try {
            extended = to_extended_wide(parent_dir);
        } catch (...) {
            error = "cannot convert the parent path to UTF-16";
            return FailureKind::Unsupported;
        }

        // Open the parent exactly as staging will. FILE_FLAG_OPEN_REPARSE_POINT
        // is deliberately omitted so a junction/symlinked component resolves to
        // the actual directory that will host the stage; capability is never
        // taken from the final reparse object alone. FILE_FLAG_BACKUP_SEMANTICS
        // is required to open a directory.
        ScopedHandle parent(CreateFileW(extended.c_str(), FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                        nullptr));
        if (!parent.valid()) {
            // A validated target whose parent cannot be opened is a resource
            // failure at this moment (missing, access denied, sharing), not a
            // statement that the volume lacks the publication primitive. Report
            // the exact native error as a staging-create failure so callers do
            // not cache it as a permanent filesystem capability gap.
            error = windows_error("open resolved parent for capability admission", GetLastError());
            return FailureKind::StagingCreateFailed;
        }

        // Network shares first: a UNC root served over SMB 2+ (reached through a
        // UNC path or a mapped drive) publishes by a no-clobber server-side
        // rename, so it needs neither hard links nor an NTFS name.
        {
            std::wstring root;
            std::string root_error;
            if (local_volume_drive_root(parent.get(), root, root_error)
                && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) {
                auto const kind = classify_share(parent.get(), root);
                if (kind == ShareKind::QueryFailed) {
                    error = windows_error("query the network share protocol", GetLastError());
                    return FailureKind::StagingCreateFailed;
                }
                if (kind != ShareKind::Smb2) {
                    error = "network location is not an SMB 2 or later share";
                    return FailureKind::Unsupported;
                }
                admitted_remote_ = true;
                error.clear();
                return FailureKind::None;
            }
        }

        // NTFS + hard links are read from the volume behind the resolved parent
        // handle, not from a logical drive query.
        DWORD flags = 0;
        std::vector<wchar_t> filesystem(VOLUME_FS_NAME_MAX, L'\0');
        if (!GetVolumeInformationByHandleW(parent.get(), nullptr, 0, nullptr, nullptr, &flags,
                                           filesystem.data(),
                                           static_cast<DWORD>(filesystem.size()))) {
            error = windows_error("query resolved volume information", GetLastError());
            return FailureKind::Unsupported;
        }
        if ((flags & FILE_SUPPORTS_HARD_LINKS) == 0) {
            error = "volume does not advertise hard-link support";
            return FailureKind::Unsupported;
        }
        if (_wcsicmp(filesystem.data(), L"NTFS") != 0) {
            error = "volume filesystem is not NTFS";
            return FailureKind::Unsupported;
        }

        // Positive locality: resolve the actual volume behind the handle. There
        // is deliberately no fallback to the logical drive spelling; if the
        // resolved root cannot be proven local, admission fails closed before
        // any staging file exists.
        std::wstring drive_root;
        if (!local_volume_drive_root(parent.get(), drive_root, error)) {
            return FailureKind::Unsupported;
        }
        UINT const type = GetDriveTypeW(drive_root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
            error = "resolved volume is not a local fixed or removable drive";
            return FailureKind::Unsupported;
        }

        error.clear();
        return FailureKind::None;
    }

    StagingAvailability retained_stage_availability(std::string const &path,
                                                    std::string &error) override
    {
        auto const it = identities_.find(path);
        if (it == identities_.end()) {
            error = "no recorded staging identity for this path";
            return StagingAvailability::Unverified;
        }
        FileIdentity const expected = it->second;

        std::wstring wide;
        try {
            wide = to_extended_wide(path);
        } catch (...) {
            error = "cannot convert the staging path to UTF-16";
            return StagingAvailability::Unverified;
        }

        HANDLE handle = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD const code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                return StagingAvailability::Missing;
            }
            error = windows_error("query retained staging availability", code);
            return StagingAvailability::Unverified;
        }

        BY_HANDLE_FILE_INFORMATION info{};
        bool const have = GetFileInformationByHandle(handle, &info) != FALSE;
        bool const reparse = have && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        bool const matches = have && !reparse && identity_matches(info, expected);
        CloseHandle(handle);

        if (!have) {
            error = windows_error("identify retained staging file", GetLastError());
            return StagingAvailability::Unverified;
        }
        if (reparse) {
            return StagingAvailability::Changed;
        }
        return matches ? StagingAvailability::Available : StagingAvailability::Changed;
    }

private:
    /// Reopen the exact staging name with DELETE and re-check the recorded
    /// identity through the SAME handle used for deletion. Missing means the
    /// cleanup already completed; a foreign, reparse or unverifiable object is
    /// preserved. Never deletes by pathname after a separate check.
    bool remove_verified(std::string const &path, FileIdentity const &expected) noexcept
    {
        std::wstring wide;
        try {
            wide = to_extended_wide(path);
        } catch (...) {
            return false;
        }

        HANDLE handle = CreateFileW(wide.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD const code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                return true;
            }
            return false;
        }

        BY_HANDLE_FILE_INFORMATION info{};
        bool const have = GetFileInformationByHandle(handle, &info) != FALSE;
        bool const reparse = have && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        bool const matches = have && !reparse && identity_matches(info, expected);
        if (!matches) {
            CloseHandle(handle);
            return false;
        }

        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        bool const marked = SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                                       sizeof(disposition)) != FALSE;
        CloseHandle(handle);
        return marked;
    }

    /// Exact owned staging path -> recorded private identity. One adapter can
    /// serve several transactions; the shared layer always passes the exact
    /// path this adapter returned from create_exclusive_file().
    std::map<std::string, FileIdentity> identities_;
    bool admitted_remote_ = false;              ///< last admission was an SMB 2+ share
    std::set<std::string> remote_stages_;       ///< stages created on such a share

    /// SMB shares: MoveFileExW without MOVEFILE_REPLACE_EXISTING, one server-side
    /// rename that refuses an existing name. On a Windows server this is atomic;
    /// Samba checks the name and then renames, so a client creating the same
    /// name in that instant is not detected (owner-accepted, like the macOS SMB
    /// claim window). The rename consumes the stage; cleanup then finds nothing.
    PublicationStatus publish_by_rename(std::string const &staged_path, std::wstring const &staged,
                                        std::wstring const &final_wide, std::string &error)
    {
        if (MoveFileExW(staged.c_str(), final_wide.c_str(), MOVEFILE_WRITE_THROUGH)) {
            error.clear();
            return PublicationStatus::Published;
        }
        DWORD const code = GetLastError();
        // Network-cache-independent proof that our stage is still ours and in
        // place: open it and compare the recorded identity.
        bool const stage_intact = stage_identity_intact(staged_path, staged);
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            if (stage_intact) {
                error = "destination already exists";
                return PublicationStatus::Conflict;
            }
            error = windows_error("rename reported an existing destination but the stage moved", code);
            return PublicationStatus::Uncertain;
        }
        // Only errors that happen before the server changes anything are
        // definite; network errors may hide an applied rename.
        bool const before_change = code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION
            || code == ERROR_INVALID_NAME || code == ERROR_FILENAME_EXCED_RANGE
            || code == ERROR_DISK_FULL || code == ERROR_WRITE_PROTECT;
        if (before_change && stage_intact) {
            error = windows_error("publish by rename on the network share", code);
            return PublicationStatus::Failed;
        }
        error = windows_error("rename publication outcome is ambiguous", code);
        return PublicationStatus::Uncertain;
    }

    bool stage_identity_intact(std::string const &staged_path, std::wstring const &staged) const noexcept
    {
        auto const it = identities_.find(staged_path);
        if (it == identities_.end()) {
            return false;
        }
        ScopedHandle handle(CreateFileW(staged.c_str(), FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle.valid()) {
            return false;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        return GetFileInformationByHandle(handle.get(), &info) && identity_matches(info, it->second);
    }
};

} // namespace

std::unique_ptr<SystemCalls> make_platform_system_calls()
{
    return std::make_unique<WindowsSystemCalls>();
}

} // namespace Inkscape::IO::DocumentTransaction

#endif // _WIN32
