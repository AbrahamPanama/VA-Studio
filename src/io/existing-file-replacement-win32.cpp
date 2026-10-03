// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Windows local-NTFS adapter for conservative existing-file replacement.
 *
 * Publication is ReplaceFileW only, through a sibling stage and a sibling
 * backup. There is no CopyFileW, no delete-then-rename, no direct open/truncate
 * of the destination, no REPLACEFILE_WRITE_THROUGH and no
 * REPLACEFILE_IGNORE_MERGE_ERRORS. NAS/SMB/UNC, mapped and remote drives,
 * non-NTFS volumes, reparse targets, multi-hard-link targets and read-only
 * destinations are explicit Unsupported capability gaps, rejected before any
 * payload is written.
 *
 * ReplaceFileW's return code and GetLastError() select only which post-states to
 * expect. The outcome is computed exclusively from post-call identity probes of
 * the destination, the stage and the backup. A probe that cannot complete is
 * Uncertain; it never becomes Published or FailedBeforePublication. Deletion
 * only ever happens through RemoveVerified(), which reopens the exact path,
 * re-checks the recorded identity on the same handle and disposes through that
 * handle. A path whose identity is unproven is never advertised as a recovery
 * version and never deleted.
 *
 * File identities are private to this translation unit. Ownership of the staged
 * object is carried across the CreateFileW -> _open_osfhandle -> _fdopen bridge
 * by narrow RAII owners; every early return disposes exactly that object through
 * its own handle/descriptor/stream. The writer borrows the FILE* and must not
 * close it.
 *
 * Windows namespace power-loss durability is not claimed: FlushFileBuffers is
 * file durability only and there is no portable parent-directory flush. Races
 * between the last probe and ReplaceFileW are caller-owned limitations; this
 * adapter makes no hostile-writer immunity claim.
 */

#ifdef _WIN32

#include "existing-file-replacement.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <string>
#include <utility>
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
#include "save-path-split.h"

#include "util-string/string-convert.h"

namespace Inkscape::IO {

namespace {

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

std::string windows_error(char const *operation, DWORD code)
{
    return std::string(operation) + ": Windows error "
        + std::to_string(static_cast<unsigned long>(code));
}

std::string errno_error(char const *operation, int code)
{
    return std::string(operation) + ": " + g_strerror(code);
}

ExistingFileResult before_failure(std::string error)
{
    return {ExistingFileOutcome::FailedBeforePublication, std::move(error), {}};
}

// ---------------------------------------------------------------------------
// Path normalization / admission
// ---------------------------------------------------------------------------

bool is_reserved_device_leaf(std::string const &leaf)
{
    // The Win32 device namespace treats these base names as devices even with an
    // extension. Reject them rather than let the extended prefix disable that
    // parsing; device spellings must never fall through to the file API.
    std::string base = leaf;
    auto const dot = base.find('.');
    if (dot != std::string::npos) {
        base = base.substr(0, dot);
    }
    for (char &c : base) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") {
        return true;
    }
    if (base.size() == 4) {
        bool const com = base.compare(0, 3, "COM") == 0;
        bool const lpt = base.compare(0, 3, "LPT") == 0;
        if ((com || lpt) && base[3] >= '1' && base[3] <= '9') {
            return true;
        }
    }
    return false;
}

std::string normalize_logical(std::string const &logical)
{
    std::string normalized = logical;
    for (char &c : normalized) {
        if (c == '/') {
            c = '\\';
        }
    }
    return normalized;
}

/// Convert a drive-absolute or \\server\share UNC logical UTF-8 path to its
/// extended-length UTF-16 form. Device (\\.\), already-extended (\\?\) and
/// relative spellings are rejected before any filesystem access. A UNC path is
/// only a spelling: admission still requires an SMB 2+ share.
bool utf8_to_extended(std::string const &logical, std::wstring &out, std::string &error)
{
    if (logical.empty()) {
        error = "path is empty";
        return false;
    }
    if (logical.find('\0') != std::string::npos) {
        error = "path contains an embedded null";
        return false;
    }

    std::string const normalized = normalize_logical(logical);
    if (normalized.size() >= 2 && normalized[0] == '\\' && normalized[1] == '\\') {
        // \\server\share\component... only: no device or already-extended
        // prefix, and every component a plain, non-empty, non-reserved name
        // (the extended \\?\UNC\ form reaches the server unnormalized).
        if (normalized.size() > 2 && (normalized[2] == '?' || normalized[2] == '.')) {
            error = "device and extended paths are not supported for existing-file replacement";
            return false;
        }
        std::vector<std::string> components;
        for (std::size_t start = 2;;) {
            auto const end = normalized.find('\\', start);
            components.push_back(normalized.substr(start, end == std::string::npos ? std::string::npos
                                                                                   : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        // A share-root parent keeps its trailing separator (\\server\share\).
        if (components.size() >= 3 && components.back().empty()) {
            components.pop_back();
        }
        // At least server and share (a file's parent may be the share root; a
        // bare share as the destination is refused when its own parent,
        // \\server, fails this check).
        if (components.size() < 2) {
            error = "a network path must name a share";
            return false;
        }
        for (auto const &c : components) {
            if (c.empty() || c == "." || c == ".." || c.back() == '.' || c.back() == ' '
                || is_reserved_device_leaf(c)) {
                error = "the network path has an empty, dot, trailing-dot/space or reserved component";
                return false;
            }
        }
        try {
            out = Inkscape::utf8_to_wstring("\\\\?\\UNC\\" + normalized.substr(2));
        } catch (...) {
            out.clear();
        }
        if (out.empty()) {
            error = "cannot convert the path to UTF-16";
            return false;
        }
        return true;
    }
    bool const drive_absolute = normalized.size() >= 3
        && ((normalized[0] >= 'A' && normalized[0] <= 'Z')
            || (normalized[0] >= 'a' && normalized[0] <= 'z'))
        && normalized[1] == ':' && normalized[2] == '\\';
    if (!drive_absolute) {
        error = "existing-file replacement requires an absolute drive path";
        return false;
    }
    auto const slash = normalized.find_last_of('\\');
    if (slash != std::string::npos && is_reserved_device_leaf(normalized.substr(slash + 1))) {
        error = "reserved device names are not supported for existing-file replacement";
        return false;
    }

    std::string const extended = "\\\\?\\" + normalized;
    try {
        out = Inkscape::utf8_to_wstring(extended);
    } catch (...) {
        error = "cannot convert the path to UTF-16";
        return false;
    }
    if (out.empty()) {
        error = "cannot convert the path to UTF-16";
        return false;
    }
    return true;
}

std::string join_logical(std::string const &parent, std::string const &name)
{
    if (!parent.empty() && parent.back() == '\\') {
        return parent + name;
    }
    return parent + "\\" + name;
}

// ---------------------------------------------------------------------------
// Identity / object metadata
// ---------------------------------------------------------------------------

struct FileIdentity {
    bool valid = false;
    ULONGLONG volume = 0;
    DWORD index_high = 0;
    DWORD index_low = 0;
    bool has_file_id = false;
    unsigned char file_id[16] = {};

    bool same(FileIdentity const &other) const
    {
        if (!valid || !other.valid || volume != other.volume) {
            return false;
        }
        if (has_file_id && other.has_file_id) {
            return std::memcmp(file_id, other.file_id, sizeof(file_id)) == 0;
        }
        if (has_file_id != other.has_file_id) {
            return false;
        }
        return index_high == other.index_high && index_low == other.index_low;
    }
};

struct ObjectInfo {
    FileIdentity id;
    DWORD attributes = 0;
    DWORD nlinks = 0;
    ULONGLONG size = 0;
    ULONGLONG creation = 0;
    ULONGLONG last_write = 0;
    ULONGLONG change = 0;
    bool has_change = false;
};

ULONGLONG filetime_u64(FILETIME const &ft)
{
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool query_object(HANDLE handle, ObjectInfo &out, std::string &error)
{
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        error = "object is not a disk file";
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        error = windows_error("identify file", GetLastError());
        return false;
    }
    out.attributes = info.dwFileAttributes;
    out.nlinks = info.nNumberOfLinks;
    out.size = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    out.creation = filetime_u64(info.ftCreationTime);
    out.last_write = filetime_u64(info.ftLastWriteTime);
    out.change = 0;
    out.has_change = false;

    out.id = FileIdentity{};
    out.id.volume = info.dwVolumeSerialNumber;
    out.id.index_high = info.nFileIndexHigh;
    out.id.index_low = info.nFileIndexLow;
    out.id.valid = true;

    // Prefer the 128-bit identity; the legacy index is the fallback only.
    FILE_ID_INFO extended{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &extended, sizeof(extended))) {
        std::memcpy(out.id.file_id, extended.FileId.Identifier, sizeof(out.id.file_id));
        out.id.has_file_id = true;
    }
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) {
        out.change = static_cast<ULONGLONG>(basic.ChangeTime.QuadPart);
        out.has_change = true;
    }
    return true;
}

/// Content-safety comparison used by the pre-replace destination re-check. The
/// identity must match and the size/LastWrite (and ChangeTime when both probes
/// expose it) must be unchanged; otherwise a writer modified the object in place.
bool content_unchanged(ObjectInfo const &baseline, ObjectInfo const &now)
{
    if (!baseline.id.same(now.id)) {
        return false;
    }
    if (baseline.size != now.size) {
        return false;
    }
    if (baseline.last_write != now.last_write) {
        return false;
    }
    if (baseline.has_change && now.has_change && baseline.change != now.change) {
        return false;
    }
    return true;
}

enum class ProbeState { Absent, Baseline, Staged, Other, Unreadable };

struct Probe {
    ProbeState state = ProbeState::Unreadable;
    ObjectInfo info;
    std::string error;
};

Probe probe_path(std::string const &logical, FileIdentity const &baseline, FileIdentity const &staged)
{
    Probe result;
    std::wstring wide;
    if (!utf8_to_extended(logical, wide, result.error)) {
        return result;
    }
    HANDLE const handle = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD const code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            result.state = ProbeState::Absent;
        } else {
            result.error = windows_error("probe path", code);
        }
        return result;
    }
    ObjectInfo info;
    std::string query_error;
    bool const ok = query_object(handle, info, query_error);
    CloseHandle(handle);
    if (!ok) {
        result.error = query_error;
        return result;
    }
    result.info = info;
    if ((info.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        // A reparse object is never ours and is never deleted.
        result.state = ProbeState::Other;
        return result;
    }
    if (info.id.same(baseline)) {
        result.state = ProbeState::Baseline;
    } else if (info.id.same(staged)) {
        result.state = ProbeState::Staged;
    } else {
        result.state = ProbeState::Other;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Handle / FILE ownership
// ---------------------------------------------------------------------------

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
            SetFileInformationByHandle(reinterpret_cast<HANDLE>(raw), FileDispositionInfo,
                                       &disposition, sizeof(disposition));
        }
        _close(fd_);
        fd_ = -1;
    }
    int fd_;
};

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

/// The only deletion primitive. Reopens the exact extended path, requires a
/// non-reparse object whose identity matches `expected`, and disposes through
/// that same handle. A missing target is success (already gone); an unverifiable
/// or foreign object is preserved.
bool remove_verified(std::wstring const &wide, FileIdentity const &expected, std::string &error)
{
    HANDLE const handle = CreateFileW(wide.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD const code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        error = windows_error("reopen owned object for verified removal", code);
        return false;
    }
    ObjectInfo info;
    std::string query_error;
    bool const ok = query_object(handle, info, query_error);
    bool const reparse = ok && (info.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    bool const matches = ok && !reparse && info.id.same(expected);
    if (!matches) {
        CloseHandle(handle);
        error = query_error.empty() ? "owned object identity could not be verified" : query_error;
        return false;
    }
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    bool const marked = SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                                   sizeof(disposition)) != FALSE;
    if (!marked) {
        error = windows_error("dispose verified owned object", GetLastError());
    }
    CloseHandle(handle);
    return marked;
}

// ---------------------------------------------------------------------------
// Sibling name creation
// ---------------------------------------------------------------------------

std::string random_suffix(int length)
{
    static char const symbols[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string suffix;
    suffix.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
        suffix.push_back(symbols[g_random_int() % 36]);
    }
    return suffix;
}

bool create_stage_file(std::string const &parent_logical, std::string &stage_logical,
                       std::wstring &stage_wide, HANDLE &out_handle, FileIdentity &out_identity,
                       std::string &error)
{
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::string const logical = join_logical(parent_logical, "vacards-save-" + random_suffix(12));
        std::wstring wide;
        if (!utf8_to_extended(logical, wide, error)) {
            return false;
        }
        // DELETE is requested so any later cleanup can dispose this exact object
        // through the handle that created it, never by pathname.
        HANDLE const handle = CreateFileW(wide.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                                          nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD const code = GetLastError();
            if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
                continue;
            }
            error = windows_error("create sibling stage file", code);
            return false;
        }
        ObjectInfo info;
        std::string query_error;
        if (!query_object(handle, info, query_error)) {
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
            CloseHandle(handle);
            error = query_error;
            return false;
        }
        stage_logical = logical;
        stage_wide = wide;
        out_handle = handle;
        out_identity = info.id;
        return true;
    }
    error = "could not choose a unique staging filename";
    return false;
}

bool choose_absent_name(std::string const &parent_logical, char const *prefix,
                        std::string &out_logical, std::wstring &out_wide, std::string &error)
{
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::string const logical = join_logical(parent_logical, std::string(prefix) + random_suffix(12));
        std::wstring wide;
        if (!utf8_to_extended(logical, wide, error)) {
            return false;
        }
        DWORD const attributes = GetFileAttributesW(wide.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD const code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                out_logical = logical;
                out_wide = wide;
                return true;
            }
        }
    }
    error = "could not choose an unused recovery filename";
    return false;
}

// ---------------------------------------------------------------------------
// Times
// ---------------------------------------------------------------------------

ULONGLONG read_save_start()
{
    FILETIME ft{};
#if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0602)
    GetSystemTimePreciseAsFileTime(&ft);
#else
    GetSystemTimeAsFileTime(&ft);
#endif
    return filetime_u64(ft);
}

/// After a verified publish, ReplaceFileW may have carried the old destination's
/// LastWrite onto the new content. Restore it to a value no earlier than the save
/// start; creation/access/change are left as the API merged them. A failure here
/// leaves the overall transaction Uncertain so the caller can surface the
/// incomplete metadata repair instead of silently marking the document saved.
bool reset_last_write(std::wstring const &dest_wide, ULONGLONG save_start, std::string &error)
{
    HANDLE const handle = CreateFileW(dest_wide.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = windows_error("open published destination for time repair", GetLastError());
        return false;
    }
    FILE_BASIC_INFO basic{};
    bool ok = GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) != FALSE;
    if (!ok) {
        error = windows_error("read published destination times", GetLastError());
        CloseHandle(handle);
        return false;
    }
    if (static_cast<ULONGLONG>(basic.LastWriteTime.QuadPart) >= save_start) {
        CloseHandle(handle);
        return true;
    }
    basic.LastWriteTime.QuadPart = static_cast<LONGLONG>(save_start);
    ok = SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic)) != FALSE;
    if (!ok) {
        error = windows_error("set published destination modification time", GetLastError());
    }
    CloseHandle(handle);
    return ok;
}

// ---------------------------------------------------------------------------
// Resolved-volume locality (patterns mirrored from the new-file adapter; kept
// private to this TU rather than refactoring another translation unit)
// ---------------------------------------------------------------------------

// GetVolumeInformationByHandleW documents MAX_PATH+1 as the maximum file system
// name buffer; it caps a short name such as "NTFS", not a path.
constexpr DWORD VOLUME_FS_NAME_MAX = MAX_PATH + 1;

bool final_path_by_handle(HANDLE handle, std::wstring &out, std::string &error)
{
    auto query = [handle](DWORD flags, std::wstring &into) -> bool {
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

bool local_volume_drive_root(HANDLE handle, std::wstring &drive_root, std::string &error)
{
    std::wstring resolved;
    if (!final_path_by_handle(handle, resolved, error)) {
        return false;
    }

    std::vector<wchar_t> root(resolved.size() + 2, L'\0');
    if (!GetVolumePathNameW(resolved.c_str(), root.data(), static_cast<DWORD>(root.size()))) {
        error = windows_error("resolve the resolved volume root", GetLastError());
        return false;
    }
    std::wstring const mount_point(root.data());

    if (mount_point.rfind(L"\\\\?\\UNC\\", 0) == 0) {
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

} // namespace

// ---------------------------------------------------------------------------
// Test-only, thread-local, one-shot publication fault seam. It is omitted
// entirely from non-test builds. There is deliberately no declaration in the
// public header; the unit test forward-declares the setter.
// ---------------------------------------------------------------------------

#ifdef VACARDS_FILE_IO_TEST_HOOKS
namespace detail {

namespace {
thread_local bool g_replace_fault_armed = false;
thread_local unsigned long g_replace_fault_code = 0;
thread_local int g_replace_fault_kind = 0;
}

void set_replace_fault_for_testing(unsigned long code, int kind)
{
    g_replace_fault_armed = (kind != 0);
    g_replace_fault_code = code;
    g_replace_fault_kind = kind;
}

bool consume_replace_fault_for_testing(unsigned long &code, int &kind)
{
    if (!g_replace_fault_armed) {
        return false;
    }
    g_replace_fault_armed = false;
    code = g_replace_fault_code;
    kind = g_replace_fault_kind;
    return true;
}

} // namespace detail
#endif

ExistingFileOptions capture_existing_file_options(bool timing)
{
    ExistingFileOptions options;
    options.timing = timing;
#ifdef VACARDS_FILE_IO_TEST_HOOKS
    detail::consume_replace_fault_for_testing(options.replace_fault_code, options.replace_fault_kind);
#endif
    return options;
}

ExistingFileResult replace_existing_local_file(
    std::string const &path, std::function<void(FILE *)> const &writer)
{
    return replace_existing_local_file(path, writer, capture_existing_file_options(false));
}

ExistingFileResult replace_existing_local_file(
    std::string const &path, std::function<void(FILE *)> const &writer,
    [[maybe_unused]] ExistingFileOptions const &options)
{
    // P1 — input validation.
    if (path.empty() || path.find('\0') != std::string::npos || !writer) {
        return {ExistingFileOutcome::Unsupported,
                "existing-file replacement requires an absolute path and a writer", {}};
    }

    // P2 — drive-absolute logical path only; no UNC/device spelling.
    std::wstring dest_wide;
    std::string error;
    if (!utf8_to_extended(path, dest_wide, error)) {
        return {ExistingFileOutcome::Unsupported, error, {}};
    }

    std::string const normalized = normalize_logical(path);
    auto const parts = split_save_path(normalized);
    if (!parts) {
        return {ExistingFileOutcome::Unsupported, "destination must name a file", {}};
    }
    std::string const &parent_logical = parts->parent;
    std::wstring parent_wide;
    if (!utf8_to_extended(parent_logical, parent_wide, error)) {
        return {ExistingFileOutcome::Unsupported, error, {}};
    }

    // P3–P5 — resolved parent, NTFS, positive locality.
    {
        ScopedHandle parent(CreateFileW(parent_wide.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!parent.valid()) {
            // A validated target whose parent cannot be opened right now is a
            // momentary resource failure, not a permanent capability claim.
            return before_failure(windows_error("open destination directory", GetLastError()));
        }

        std::wstring drive_root;
        if (!local_volume_drive_root(parent.get(), drive_root, error)) {
            return {ExistingFileOutcome::Unsupported, error, {}};
        }
        UINT const type = GetDriveTypeW(drive_root.c_str());
        if (type == DRIVE_REMOTE) {
            // UNC path or mapped drive: only an SMB 2+ share. ReplaceFileW and the
            // probes below are the same client operations used there; the
            // server's filesystem name is not meaningful over SMB.
            auto const kind = classify_share(parent.get(), drive_root);
            if (kind == ShareKind::QueryFailed) {
                return before_failure(windows_error("query the network share protocol", GetLastError()));
            }
            if (kind != ShareKind::Smb2) {
                return {ExistingFileOutcome::Unsupported,
                        "network location is not an SMB 2 or later share", {}};
            }
        } else {
            DWORD flags = 0;
            std::vector<wchar_t> filesystem(VOLUME_FS_NAME_MAX, L'\0');
            if (!GetVolumeInformationByHandleW(parent.get(), nullptr, 0, nullptr, nullptr, &flags,
                                               filesystem.data(), static_cast<DWORD>(filesystem.size()))) {
                return {ExistingFileOutcome::Unsupported,
                        windows_error("query resolved volume information", GetLastError()), {}};
            }
            if (_wcsicmp(filesystem.data(), L"NTFS") != 0) {
                return {ExistingFileOutcome::Unsupported, "volume filesystem is not NTFS", {}};
            }
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
                return {ExistingFileOutcome::Unsupported,
                        "resolved volume is not a local fixed or removable drive", {}};
            }
        }
    }

    ULONGLONG const save_start = read_save_start();

    // P6–P9 — existing regular destination, one link, no reparse, not read-only.
    ObjectInfo baseline;
    {
        ScopedHandle destination(CreateFileW(dest_wide.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!destination.valid()) {
            DWORD const code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                return {ExistingFileOutcome::Unsupported,
                        "destination must be an existing regular file", {}};
            }
            if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
                return before_failure(windows_error("open existing destination", code));
            }
            return {ExistingFileOutcome::Unsupported,
                    windows_error("open existing destination", code), {}};
        }
        if (!query_object(destination.get(), baseline, error)) {
            return {ExistingFileOutcome::Unsupported, error, {}};
        }
        if ((baseline.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return {ExistingFileOutcome::Unsupported, "destination is a directory", {}};
        }
        if ((baseline.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return {ExistingFileOutcome::Unsupported, "destination is a reparse point", {}};
        }
        if (baseline.nlinks != 1) {
            return {ExistingFileOutcome::Unsupported,
                    "destination must have exactly one hard link", {}};
        }
        if ((baseline.attributes & FILE_ATTRIBUTE_READONLY) != 0) {
            return {ExistingFileOutcome::Unsupported,
                    "destination is read-only; use Save Copy", {}};
        }
    }

    // Stage a sibling and bridge it to the writer's FILE*.
    std::string stage_logical;
    std::wstring stage_wide;
    HANDLE stage_handle = INVALID_HANDLE_VALUE;
    FileIdentity staged_id;
    if (!create_stage_file(parent_logical, stage_logical, stage_wide, stage_handle, staged_id, error)) {
        return before_failure(error);
    }
    CreatedHandleOwner handle_owner(stage_handle);

    int const fd = _open_osfhandle(reinterpret_cast<intptr_t>(stage_handle), _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        return before_failure("could not bridge the staging handle to a descriptor");
    }
    (void)handle_owner.release();
    CreatedFdOwner fd_owner(fd);

    FILE *const stream = _fdopen(fd, "wb");
    if (!stream) {
        return before_failure("could not bridge the staging descriptor to a stream");
    }
    (void)fd_owner.release();
    CreatedStreamOwner stream_owner(stream);

    try {
        writer(stream);
    } catch (std::exception const &e) {
        return before_failure(std::string("serialize staging file: ") + e.what());
    } catch (...) {
        return before_failure("serialize staging file failed");
    }

    if (std::fflush(stream) != 0 || std::ferror(stream) != 0) {
        int const saved = errno;
        return before_failure(saved != 0 ? errno_error("flush staging file", saved)
                                         : "flush staging file failed");
    }
    {
        int const native_fd = _fileno(stream);
        intptr_t const raw = native_fd >= 0 ? _get_osfhandle(native_fd) : -1;
        if (raw == -1) {
            return before_failure("staging stream has no native handle");
        }
        if (!FlushFileBuffers(reinterpret_cast<HANDLE>(raw))) {
            // Local NTFS and SMB 2+ both implement flush, so a failed flush is a
            // real durability gap rather than an optional capability.
            return before_failure(windows_error("FlushFileBuffers", GetLastError()));
        }
    }
    FILE *const raw_stream = stream_owner.release();
    errno = 0;
    if (std::fclose(raw_stream) != 0) {
        int const saved = errno;
        std::string err = saved != 0 ? errno_error("close staging file", saved)
                                     : "close staging file failed";
        std::string remove_error;
        if (!remove_verified(stage_wide, staged_id, remove_error)) {
            err += "; an owned staging file remains at " + stage_logical;
        }
        return before_failure(err);
    }

    // Post-close stage probe: never publish a name whose identity changed.
    ULONGLONG staged_size = 0;
    {
        Probe const stage_probe = probe_path(stage_logical, baseline.id, staged_id);
        if (stage_probe.state == ProbeState::Absent) {
            return before_failure("staging file disappeared before publication");
        }
        if (stage_probe.state != ProbeState::Staged) {
            return {ExistingFileOutcome::Uncertain,
                    "staging file identity could not be confirmed before publication: "
                        + (stage_probe.error.empty()
                               ? std::string("unexpected object at the staging path")
                               : stage_probe.error),
                    {}};
        }
        staged_size = stage_probe.info.size;
    }

    // Pre-replace destination re-check: identity, size and times must be intact.
    {
        Probe const destination_now = probe_path(path, baseline.id, staged_id);
        if (destination_now.state != ProbeState::Baseline
            || !content_unchanged(baseline, destination_now.info)) {
            std::string err = (destination_now.state == ProbeState::Unreadable)
                ? "destination could not be confirmed before replacement: " + destination_now.error
                : "destination changed before replacement";
            std::string remove_error;
            if (!remove_verified(stage_wide, staged_id, remove_error)) {
                err += "; an owned staging file remains at " + stage_logical;
            }
            return {ExistingFileOutcome::Conflict, err, {}};
        }
    }

    std::string backup_logical;
    std::wstring backup_wide;
    if (!choose_absent_name(parent_logical, "vacards-recovery-", backup_logical, backup_wide, error)) {
        std::string remove_error;
        std::string err = error;
        if (!remove_verified(stage_wide, staged_id, remove_error)) {
            err += "; an owned staging file remains at " + stage_logical;
        }
        return before_failure(err);
    }

    // Publication. ReplaceFileW merge flags are deliberately 0: no
    // WRITE_THROUGH, no IGNORE_MERGE_ERRORS. The fault seam exists in test
    // builds only.
    bool replaced = false;
    DWORD code = ERROR_SUCCESS;
#ifdef VACARDS_FILE_IO_TEST_HOOKS
    if (options.replace_fault_kind != 0) {
        if (options.replace_fault_kind == 2) {
            // Reproduce the 1177 mixed state: old file moved to the backup name,
            // new content left at the stage name, destination absent.
            (void)MoveFileExW(dest_wide.c_str(), backup_wide.c_str(), 0);
        }
        replaced = false;
        code = static_cast<DWORD>(options.replace_fault_code);
    } else {
#endif
        replaced = ReplaceFileW(dest_wide.c_str(), stage_wide.c_str(), backup_wide.c_str(),
                                0, nullptr, nullptr) != FALSE;
        if (!replaced) {
            code = GetLastError();
        }
#ifdef VACARDS_FILE_IO_TEST_HOOKS
    }
#endif

    // Probes are the authority; the code above only selected what to expect.
    Probe const dest_probe = probe_path(path, baseline.id, staged_id);
    Probe const stage_probe = probe_path(stage_logical, baseline.id, staged_id);
    Probe const backup_probe = probe_path(backup_logical, baseline.id, staged_id);
    std::string const verified_backup =
        (backup_probe.state == ProbeState::Baseline) ? backup_logical : std::string();

    if (replaced) {
        bool const destination_ok = dest_probe.state == ProbeState::Staged
            && dest_probe.info.size == staged_size;
        bool const stage_gone = stage_probe.state == ProbeState::Absent;
        bool const backup_ok = backup_probe.state == ProbeState::Baseline;
        if (destination_ok && stage_gone && backup_ok) {
            std::string warning;
            std::string time_error;
            if (!reset_last_write(dest_wide, save_start, time_error)) {
                warning = "; last-write time could not be reset: " + time_error;
            }
            std::string remove_error;
            if (!remove_verified(backup_wide, baseline.id, remove_error)) {
                std::string err = "new content was published, but old recovery copy cleanup failed at "
                    + backup_logical;
                if (!remove_error.empty()) {
                    err += " (" + remove_error + ")";
                }
                if (!warning.empty()) {
                    err += warning;
                }
                return {ExistingFileOutcome::Uncertain, err, backup_logical};
            }
            if (!warning.empty()) {
                return {ExistingFileOutcome::Uncertain,
                        "new content was published, but its modification time could not be repaired"
                            + warning,
                        {}};
            }
            return {ExistingFileOutcome::Published, {}, {}};
        }
        return {ExistingFileOutcome::Uncertain,
                "replacement outcome could not be verified by post-publication probes",
                verified_backup};
    }

    if (code == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
        // The original may already have moved to the backup and the replacement
        // may remain at the stage with merged streams/attributes. Never claim
        // FailedBeforePublication here, and retain both objects.
        return {ExistingFileOutcome::Uncertain,
                windows_error("replace destination", code)
                    + "; the destination may no longer hold the original; inspect the retained new staging file at "
                    + stage_logical + " and any verified old recovery file before retrying",
                verified_backup};
    }

    bool const namespace_unchanged = dest_probe.state == ProbeState::Baseline
        && stage_probe.state == ProbeState::Staged
        && (backup_probe.state == ProbeState::Absent
            || backup_probe.state == ProbeState::Baseline);
    if (namespace_unchanged) {
        std::string err = windows_error("replace destination", code);
        std::string remove_error;
        if (!remove_verified(stage_wide, staged_id, remove_error)) {
            return {ExistingFileOutcome::Uncertain,
                    err + "; an owned staging file could not be removed and remains at "
                        + stage_logical,
                    verified_backup};
        }
        if (backup_probe.state == ProbeState::Baseline) {
            // Our verified backup is a redundant hard link to the unchanged
            // destination; remove it only after identity verification.
            if (!remove_verified(backup_wide, baseline.id, remove_error)) {
                return {ExistingFileOutcome::Uncertain,
                        err + "; a redundant recovery copy remains at " + backup_logical,
                        backup_logical};
            }
        }
        return before_failure(err);
    }

    return {ExistingFileOutcome::Uncertain,
            windows_error("replace destination", code)
                + "; outcome is ambiguous; inspect the retained staging file at " + stage_logical,
            verified_backup};
}

} // namespace Inkscape::IO

#endif // _WIN32
