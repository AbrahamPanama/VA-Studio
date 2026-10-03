// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-storage.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string_view>
#ifdef _WIN32
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/param.h>
#include <sys/mount.h>
#endif

namespace Inkscape::IO::ArtworkLibrary {
namespace {
template <typename T> struct Unref { void operator()(T *p) const { if (p) g_object_unref(p); } };
template <typename T> using Object = std::unique_ptr<T, Unref<T>>;
using File = Object<GFile>;
using Info = Object<GFileInfo>;
constexpr auto attributes = "standard::type,standard::is-symlink,standard::size,id::file,id::filesystem,etag::value,time::modified,time::modified-usec,unix::nlink";

[[noreturn]] void fail(StorageFailure code, std::string message) { throw StorageError(code, std::move(message)); }
[[noreturn]] void io_error(char const *operation, GError *error, StorageFailure code = StorageFailure::Io)
{
    std::string message = operation;
    if (error) { message += ": "; message += error->message; g_error_free(error); }
    fail(code, std::move(message));
}
void check(Cancelled const &cancelled)
{
    if (cancelled && cancelled()) fail(StorageFailure::Cancelled, "Storage operation cancelled before publication admission");
}
std::string string_attr(GFileInfo *info, char const *key)
{
    if (!g_file_info_has_attribute(info, key)) return {};
    auto value = g_file_info_get_attribute_string(info, key);
    return value ? value : "";
}
File file(std::string const &path) { return File(g_file_new_for_path(path.c_str())); }
std::string path_of(GFile *f)
{
    auto value = g_file_get_path(f);
    if (!value) fail(StorageFailure::Unsupported, "Non-native file backend");
    std::string result(value); g_free(value); return result;
}
std::string canonical(std::string path)
{
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '/', '\\');
    if (g_ascii_strncasecmp(path.c_str(), "\\\\?\\UNC\\", 8) == 0) path = "\\\\" + path.substr(8);
    else if (path.starts_with("\\\\?\\")) path.erase(0, 4);
    bool drive = path.size() >= 3 && g_ascii_isalpha(path[0]) && path[1] == ':' && path[2] == '\\';
    bool unc = path.starts_with("\\\\") && path.size() > 2 && path[2] != '.' && path[2] != '?';
    if (!drive && !unc) fail(StorageFailure::Invalid, "Choose an absolute drive or UNC file path");
#endif
    if (path.empty() || path.find('\0') != path.npos || !g_path_is_absolute(path.c_str())) {
        fail(StorageFailure::Invalid, "Storage requires an absolute native path");
    }
    // Three UTF-8 bytes per UTF-16 unit is the largest possible ratio.
    if (path.size() > 3u * 32767 || !g_utf8_validate(path.data(), path.size(), nullptr)) {
        fail(StorageFailure::Invalid, "Invalid or oversized UTF-8 file path");
    }
    auto value = g_canonicalize_filename(path.c_str(), nullptr);
    std::string result(value); g_free(value);
#ifdef _WIN32
    glong units = 0;
    std::unique_ptr<gunichar2, decltype(&g_free)> wide(
        g_utf8_to_utf16(result.c_str(), -1, nullptr, &units, nullptr), &g_free);
    if (!wide || units + (result.starts_with("\\\\") ? 6 : 4) >= 32767) {
        fail(StorageFailure::Invalid, "File path exceeds the Windows extended-path limit");
    }
#endif
    return result;
}
#ifdef _WIN32
std::wstring native_path(std::string const &path)
{
    auto normalized = canonical(path);
    auto extended = normalized.starts_with("\\\\") ? "\\\\?\\UNC\\" + normalized.substr(2) : "\\\\?\\" + normalized;
    std::unique_ptr<gunichar2, decltype(&g_free)> wide(
        g_utf8_to_utf16(extended.c_str(), -1, nullptr, nullptr, nullptr), &g_free);
    if (!wide) fail(StorageFailure::Invalid, "Invalid UTF-8 native file path");
    return reinterpret_cast<wchar_t const *>(wide.get());
}
[[noreturn]] void windows_error(char const *operation, DWORD error, StorageFailure code = StorageFailure::Io)
{
    fail(code, std::string(operation) + ": Windows error " + std::to_string(error));
}
struct CloseHandleDeleter { void operator()(void *p) const { if (p) CloseHandle(p); } };
using NativeHandle = std::unique_ptr<void, CloseHandleDeleter>;
NativeHandle native_open(std::string const &path, DWORD access, bool allow_missing = false)
{
    auto wide = native_path(path);
    auto handle = CreateFileW(wide.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        auto error = GetLastError();
        if (allow_missing && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) return {};
        fail(StorageFailure::Io, "Open native package information: Windows error " + std::to_string(error));
    }
    return NativeHandle(handle);
}
Info native_info(HANDLE handle)
{
    // Win32 GIO path and stream IDs use different CRT device prefixes (l2/l0).
    // Query the same native identity for both, including the handle we read.
    // Never strip that prefix or substitute a pathname query for an open handle.
    BY_HANDLE_FILE_INFORMATION data {};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &data)) {
        fail(StorageFailure::Unsupported, "Cannot query native file identity: Windows error " + std::to_string(GetLastError()));
    }
    auto join = [](DWORD high, DWORD low) { return (std::uint64_t(high) << 32) | low; };
    auto size = join(data.nFileSizeHigh, data.nFileSizeLow);
    if (size > std::uint64_t(std::numeric_limits<goffset>::max())) fail(StorageFailure::Unsupported, "Native file size out of range");
    auto filesystem = "win32:" + std::to_string(data.dwVolumeSerialNumber);
    auto id = filesystem + ":" + std::to_string(join(data.nFileIndexHigh, data.nFileIndexLow));
    // ReFS identities are 128-bit; the legacy 64-bit index alone is not unique.
    // FAT and older servers may not implement FileIdInfo, so retain their
    // native legacy identity instead of inventing an identity from the pathname.
    FILE_ID_INFO extended {};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &extended, sizeof(extended))) {
        filesystem = "win32-id128:" + std::to_string(extended.VolumeSerialNumber);
        id = filesystem + ":";
        constexpr char hex[] = "0123456789abcdef";
        for (auto byte : extended.FileId.Identifier) { id += hex[byte >> 4]; id += hex[byte & 15]; }
    } else {
        std::array<wchar_t, 32> type {};
        if (GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, nullptr, type.data(), type.size()) &&
            std::wstring_view(type.data()) == L"ReFS") {
            fail(StorageFailure::Unsupported, "Cannot obtain the required ReFS 128-bit file identity");
        }
    }
    auto ticks = join(data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime);
    auto etag = std::to_string(ticks) + ":" + std::to_string(size);
    bool reparse = data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
    Info result(g_file_info_new());
    g_file_info_set_file_type(result.get(), reparse ? G_FILE_TYPE_SYMBOLIC_LINK :
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? G_FILE_TYPE_DIRECTORY : G_FILE_TYPE_REGULAR);
    g_file_info_set_is_symlink(result.get(), reparse);
    g_file_info_set_size(result.get(), size);
    g_file_info_set_attribute_string(result.get(), G_FILE_ATTRIBUTE_ID_FILE, id.c_str());
    g_file_info_set_attribute_string(result.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM, filesystem.c_str());
    g_file_info_set_attribute_string(result.get(), G_FILE_ATTRIBUTE_ETAG_VALUE, etag.c_str());
    g_file_info_set_attribute_uint32(result.get(), G_FILE_ATTRIBUTE_UNIX_NLINK, data.nNumberOfLinks);
    constexpr std::uint64_t epoch = 116444736000000000ULL;
    if (ticks >= epoch) {
        g_file_info_set_attribute_uint64(result.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED, (ticks - epoch) / 10000000);
        g_file_info_set_attribute_uint32(result.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, (ticks - epoch) % 10000000 / 10);
    }
    return result;
}
#endif
Info info(GFile *f, bool allow_missing = false)
{
#ifdef _WIN32
    auto handle = native_open(path_of(f), FILE_READ_ATTRIBUTES, allow_missing);
    return handle ? native_info(handle.get()) : Info{};
#else
    GError *error = nullptr;
    Info result(g_file_query_info(f, attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &error));
    if (!result) {
        if (allow_missing && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
            g_clear_error(&error); return {};
        }
        io_error("Query file", error);
    }
    return result;
#endif
}
void require_type(GFileInfo *i, GFileType type, bool path_information = true)
{
    // NOFOLLOW path queries still reject SYMBOLIC_LINK by type even when the
    // optional boolean is absent. Opened handles have no path symlink property.
    if (!g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_STANDARD_TYPE)) {
        fail(StorageFailure::Unsupported, "Backend does not supply file type");
    }
    bool symlink = path_information &&
        g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK) &&
        g_file_info_get_is_symlink(i);
    if (symlink || g_file_info_get_file_type(i) != type) {
        fail(StorageFailure::Unsupported, "Symlink or unsupported file type");
    }
}
void require_identity(GFileInfo *i)
{
    if (string_attr(i, G_FILE_ATTRIBUTE_ID_FILE).empty() || string_attr(i, G_FILE_ATTRIBUTE_ID_FILESYSTEM).empty()) {
        fail(StorageFailure::Unsupported, "Backend does not supply stable file identity");
    }
}
void parents(GFile *target, bool allow_root = false)
{
    File parent(g_file_get_parent(target));
    if (!parent && !allow_root) fail(StorageFailure::Invalid, "A filesystem root is not a library filename");
    while (parent) {
        auto i = info(parent.get());
        try {
            require_type(i.get(), G_FILE_TYPE_DIRECTORY);
        } catch (StorageError const &e) {
            fail(e.failure(), "The folder \"" + path_of(parent.get()) + "\" is a symbolic link or not a "
                 "folder. Open and save libraries through their real folder path (for example "
                 "/private/tmp instead of /tmp on macOS).");
        }
        parent.reset(g_file_get_parent(parent.get()));
    }
}
FileVersion fingerprint(GFileInfo *i, std::string const &path)
{
    require_type(i, G_FILE_TYPE_REGULAR);
    require_identity(i);
    if (!g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
        fail(StorageFailure::Unsupported, "Backend does not supply file size");
    }
    auto size = g_file_info_get_size(i);
    if (size < 0) fail(StorageFailure::Unsupported, "Negative file size");
    FileVersion result;
    result.path = path;
    result.file_id = string_attr(i, G_FILE_ATTRIBUTE_ID_FILE);
    result.filesystem_id = string_attr(i, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
    result.etag = string_attr(i, G_FILE_ATTRIBUTE_ETAG_VALUE);
    result.size = static_cast<std::uint64_t>(size);
    if (g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
        result.modified_seconds = g_file_info_get_attribute_uint64(i, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    }
    if (g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC)) {
        result.modified_microseconds = g_file_info_get_attribute_uint32(i, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
    }
    return result;
}
void require_opened_matches(GFileInfo *i, FileVersion const &path_version)
{
    require_type(i, G_FILE_TYPE_REGULAR, false);
    require_identity(i);
    if (!g_file_info_has_attribute(i, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
        fail(StorageFailure::Unsupported, "Opened handle does not supply file size");
    }
    // Opened streams omit the path-only is-symlink flag; optional timestamps
    // depend on the requested attributes and backend. Do not invent zero-valued
    // times and compare those with the path's real mtime. Path fingerprints and
    // available handle etags still verify changes before/after the read.
    auto size = g_file_info_get_size(i);
    auto etag = string_attr(i, G_FILE_ATTRIBUTE_ETAG_VALUE);
    if (size < 0 || static_cast<std::uint64_t>(size) != path_version.size ||
        string_attr(i, G_FILE_ATTRIBUTE_ID_FILE) != path_version.file_id ||
        string_attr(i, G_FILE_ATTRIBUTE_ID_FILESYSTEM) != path_version.filesystem_id ||
        (!etag.empty() && !path_version.etag.empty() && etag != path_version.etag)) {
        fail(StorageFailure::Conflict, "Opened package differs from path identity/version");
    }
}
struct Read { Bytes bytes; FileVersion version; };
Read read_file(std::string const &path, std::size_t limit, Cancelled const &cancelled, bool prefix = false)
{
    check(cancelled);
    auto f = file(path);
    parents(f.get());
    auto before_info = info(f.get());
    auto before = fingerprint(before_info.get(), path);
    if (!prefix && before.size > limit) fail(StorageFailure::Integrity, "Package file exceeds byte budget");
#ifdef _WIN32
    auto stream = native_open(path, GENERIC_READ);
    auto opened = native_info(stream.get());
#else
    GError *error = nullptr;
    Object<GFileInputStream> stream(g_file_read(f.get(), nullptr, &error));
    if (!stream) io_error("Open package", error);
    Info opened(g_file_input_stream_query_info(stream.get(), attributes, nullptr, &error));
    if (!opened) io_error("Query opened package", error);
#endif
    require_opened_matches(opened.get(), before);
    Bytes bytes;
    bytes.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(before.size, limit)));
    std::array<unsigned char, 65536> buffer;
    for (;;) {
        check(cancelled);
        auto amount = prefix ? std::min(buffer.size(), limit - bytes.size()) : buffer.size();
        if (!amount) break;
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(stream.get(), buffer.data(), amount, &n, nullptr)) {
            fail(StorageFailure::Io, "Read native package: Windows error " + std::to_string(GetLastError()));
        }
#else
        auto n = g_input_stream_read(G_INPUT_STREAM(stream.get()), buffer.data(), amount, nullptr, &error);
        if (n < 0) io_error("Read package", error);
#endif
        if (!n) break;
        if (static_cast<std::size_t>(n) > limit - bytes.size()) fail(StorageFailure::Integrity, "Growing package exceeds byte budget");
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + n);
    }
#ifdef _WIN32
    auto finished = native_info(stream.get());
#else
    Info finished(g_file_input_stream_query_info(stream.get(), attributes, nullptr, &error));
    if (!finished) io_error("Query completed read", error);
#endif
    require_opened_matches(finished.get(), before);
    auto opened_etag = string_attr(opened.get(), G_FILE_ATTRIBUTE_ETAG_VALUE);
    auto finished_etag = string_attr(finished.get(), G_FILE_ATTRIBUTE_ETAG_VALUE);
    if (!opened_etag.empty() && !finished_etag.empty() && opened_etag != finished_etag) {
        fail(StorageFailure::Conflict, "Opened package changed during read");
    }
#ifdef _WIN32
    if (!CloseHandle(stream.release())) fail(StorageFailure::Io, "Close native package read failed");
#else
    if (!g_input_stream_close(G_INPUT_STREAM(stream.get()), nullptr, &error)) io_error("Close package read", error);
#endif
    auto after = info(f.get());
    if (bytes.size() != (prefix ? std::min<std::uint64_t>(before.size, limit) : before.size) || fingerprint(after.get(), path) != before) {
        fail(StorageFailure::Conflict, "Package changed during read");
    }
    before.sha256 = artwork_sha256(bytes, [&] { check(cancelled); return false; });
    auto after_hash = info(f.get());
    auto unhashed = before; unhashed.sha256.clear();
    if (fingerprint(after_hash.get(), path) != unhashed) fail(StorageFailure::Conflict, "Package changed while hashing");
    return {std::move(bytes), std::move(before)};
}
LoadedLibrary decode(Read read, StorageLimits const &limits, Cancelled const &cancelled,
                     std::size_t *expanded_budget = nullptr, bool *budget_exceeded = nullptr)
{
    // Package APIs use runtime_error for integrity/cancellation; keep the storage
    // cancellation latch explicit rather than guessing from exception messages.
    bool was_cancelled = false;
    auto cancel = [&] { was_cancelled = cancelled && cancelled(); return was_cancelled; };
    try {
        std::function<void(std::size_t)> admission;
        if (expanded_budget) admission = [&](std::size_t bytes) {
            if (bytes > *expanded_budget) {
                if (budget_exceeded) *budget_exceeded = true;
                fail(StorageFailure::Integrity, "Recovery scan expanded-byte budget exceeded");
            }
            *expanded_budget -= bytes; // Charge even if manifest parsing fails.
        };
        auto p = Package::open_with_admission(std::move(read.bytes), std::move(admission),
                                             limits.archive, limits.manifest, cancel);
        p.verify_artworks(cancel);
        read.version.library_id = p.manifest().id;
        read.version.revision = p.manifest().revision;
        return {std::move(p), std::move(read.version)};
    } catch (StorageError const &) { throw; }
    catch (std::exception const &e) {
        fail(was_cancelled ? StorageFailure::Cancelled : StorageFailure::Integrity, e.what());
    }
}
struct ParentIdentity { std::string id, filesystem; };
ParentIdentity writable_parent(GFile *target)
{
    parents(target);
    File parent(g_file_get_parent(target));
    auto pi = info(parent.get());
    require_identity(pi.get());
#ifdef __APPLE__
    // Direct-path statfs resolves the actual Data mount behind firmlinks. The
    // observed GIO filesystem query reported read-only even for writable
    // /Users and /private/var paths; neither GIO boolean is a
    // substitute for this path-specific positive native evidence.
    struct statfs mount {};
    auto path = path_of(parent.get());
    if (statfs(path.c_str(), &mount) != 0) {
        fail(StorageFailure::Unsupported, "Cannot identify native mount: " + std::string(g_strerror(errno)));
    }
    if (mount.f_flags & MNT_RDONLY) {
        fail(StorageFailure::Unsupported, "Destination volume is read-only");
    }
#elif defined(_WIN32)
    // Native identity/type queries above also work on UNC, FAT/exFAT and ReFS.
    // Do not infer capabilities from a filesystem name or GIO's missing Win32
    // locality attributes. CREATE_NEW, FlushFileBuffers and same-directory
    // rename must actually succeed; access/read-only failures remain errors.
#else
    GError *error = nullptr;
    Info fs(g_file_query_filesystem_info(parent.get(), "filesystem::type,filesystem::remote,filesystem::readonly", nullptr, &error));
    if (!fs) io_error("Query filesystem", error, StorageFailure::Unsupported);
    if (g_file_info_get_attribute_boolean(fs.get(), G_FILE_ATTRIBUTE_FILESYSTEM_READONLY)) {
        fail(StorageFailure::Unsupported, "Destination volume is read-only");
    }
#endif
    return {string_attr(pi.get(), G_FILE_ATTRIBUTE_ID_FILE), string_attr(pi.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM)};
}
void check_parent(GFile *target, ParentIdentity const &expected)
{
    auto actual = writable_parent(target);
    if (actual.id != expected.id || actual.filesystem != expected.filesystem) {
        fail(StorageFailure::Conflict, "Destination directory replaced");
    }
}
void single_link(GFile *f)
{
    auto i = info(f, true);
    if (!i) return;
    require_type(i.get(), G_FILE_TYPE_REGULAR);
    if (g_file_info_has_attribute(i.get(), G_FILE_ATTRIBUTE_UNIX_NLINK) &&
        g_file_info_get_attribute_uint32(i.get(), G_FILE_ATTRIBUTE_UNIX_NLINK) > 1) {
        fail(StorageFailure::Unsupported, "Hard-linked package is not writable through this service");
    }
}
void flush_file(std::string const &path)
{
#ifdef _WIN32
    auto handle = native_open(path, GENERIC_WRITE);
    if (!FlushFileBuffers(handle.get())) windows_error("Flush package", GetLastError());
#else
    int fd = g_open(path.c_str(), O_RDONLY | O_NOFOLLOW, 0);
    if (fd < 0) fail(StorageFailure::Io, "Open file for durability flush: " + std::string(g_strerror(errno)));
    int result = fsync(fd); int saved = errno; close(fd);
    if (result) fail(StorageFailure::Io, "File durability flush: " + std::string(g_strerror(saved)));
#endif
}
bool flush_parent(GFile *target)
{
#ifdef _WIN32
    (void)target;
    return false; // Explicitly no Windows directory-durability claim.
#else
    File parent(g_file_get_parent(target));
    auto path = path_of(parent.get());
    int fd = g_open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    if (fd < 0) fail(StorageFailure::Io, "Open directory for durability flush");
    int result = fsync(fd); int saved = errno; close(fd);
    // Some removable/network filesystems support file flush but not directory
    // fsync. Report that narrower durability level; real I/O failures still fail.
    if (result && (saved == EINVAL || saved == ENOTSUP || saved == EOPNOTSUPP)) return false;
    if (result) fail(StorageFailure::Io, "Directory durability flush: " + std::string(g_strerror(saved)));
    return true;
#endif
}
std::string nonce()
{
    auto value = g_uuid_string_random(); std::string result(value); g_free(value); return result;
}
void event(StorageOptions const &o, StorageResult const &r, std::string const &path, StoragePhase phase, std::size_t bytes = 0)
{
    if (o.checkpoint) o.checkpoint({phase, path, r.staged_path, r.recovery_path, bytes});
}
std::string write_new(std::string const &path, Bytes const &bytes, Cancelled const &cancelled,
               std::function<void(bool, std::size_t)> const &progress,
               StorageFailure collision = StorageFailure::Conflict)
{
    check(cancelled);
#ifdef _WIN32
    auto wide = native_path(path);
    auto raw = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        auto error = GetLastError();
        windows_error("Exclusively create file", error,
                      error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? collision : StorageFailure::Io);
    }
    NativeHandle stream(raw);
#else
    auto f = file(path);
    GError *error = nullptr;
    Object<GFileOutputStream> stream(g_file_create(f.get(), G_FILE_CREATE_PRIVATE, nullptr, &error));
    if (!stream) io_error("Exclusively create staging/recovery file", error,
                          g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS) ? collision : StorageFailure::Io);
#endif
    if (progress) progress(true, 0);
    for (std::size_t at = 0; at < bytes.size();) {
        check(cancelled);
        auto n = std::min<std::size_t>(65536, bytes.size() - at);
#ifdef _WIN32
        DWORD written = 0;
        if (!WriteFile(stream.get(), bytes.data() + at, n, &written, nullptr)) windows_error("Write file", GetLastError());
#else
        gsize written = 0;
        if (!g_output_stream_write_all(G_OUTPUT_STREAM(stream.get()), bytes.data() + at, n, &written, nullptr, &error)) {
            io_error("Write package", error);
        }
#endif
        if (!written) fail(StorageFailure::Io, "File write made no progress");
        at += written;
        if (progress) progress(false, at);
    }
#ifdef _WIN32
    if (!FlushFileBuffers(stream.get())) windows_error("Flush written file", GetLastError());
    auto finished = native_info(stream.get());
    if (!CloseHandle(stream.release())) windows_error("Close written file", GetLastError());
#else
    if (!g_output_stream_flush(G_OUTPUT_STREAM(stream.get()), nullptr, &error)) io_error("Flush output stream", error);
    // exFAT can replace a provisional inode at the first fsync. Flush BEFORE
    // obtaining the final identity, while the original writer is still open.
    flush_file(path);
    Info finished(g_file_output_stream_query_info(stream.get(), attributes, nullptr, &error));
    if (!finished) io_error("Inspect written file handle", error);
    if (!g_output_stream_close(G_OUTPUT_STREAM(stream.get()), nullptr, &error)) io_error("Close written package", error);
    // SMB close can update the server's final timestamps. Retain the existing
    // post-close flush/revalidation boundary as well as exFAT's pre-ID flush.
    flush_file(path);
#endif
    check(cancelled);
    require_identity(finished.get());
    return string_attr(finished.get(), G_FILE_ATTRIBUTE_ID_FILE);
}
// Human diagnostics only. Ownership is still proven by the nonce and file
// identity, never by these lines. Kept short: validate() reads at most 1 KiB.
std::string lock_holder_lines()
{
    std::string host;
    if (auto raw = g_get_host_name()) {
        for (auto p = raw; *p && host.size() < 64; ++p) {
            auto c = static_cast<unsigned char>(*p);
            host += (g_ascii_isalnum(c) || c == '-' || c == '.' || c == '_') ? char(c) : '_';
        }
    }
#ifdef _WIN32
    auto process = std::to_string(GetCurrentProcessId());
#else
    auto process = std::to_string(getpid());
#endif
    std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> now(g_date_time_new_now_utc(), &g_date_time_unref);
    std::unique_ptr<gchar, decltype(&g_free)> when(now ? g_date_time_format_iso8601(now.get()) : nullptr, &g_free);
    return "host=" + host + "\npid=" + process + "\nutc=" + (when ? when.get() : "") + "\n";
}
// "host=…, pid=…, utc=…" from a lock's diagnostic lines; printable ASCII only.
std::string lock_holder_summary(Bytes const &bytes)
{
    std::string text(bytes.begin(), bytes.end()), holder;
    for (auto key : {"host=", "pid=", "utc="}) {
        auto at = text.find(std::string("\n") + key);
        if (at == std::string::npos) continue;
        at += 1 + std::strlen(key);
        std::string value;
        for (; at < text.size() && text[at] != '\n' && value.size() < 64; ++at) {
            auto c = static_cast<unsigned char>(text[at]);
            if (c >= 32 && c < 127) value += char(c);
        }
        if (!value.empty()) holder += std::string(holder.empty() ? "" : ", ") + key + value;
    }
    return holder;
}
// Message for a lock we did not create. Any read failure just omits the holder.
std::string locked_message(std::string const &lock_path)
{
    std::string holder;
    try { holder = lock_holder_summary(read_file(lock_path, 1024, {}, true).bytes); } catch (...) {}
    return "This library is locked by another save (lock file: " + lock_path + ")" +
           (holder.empty() ? "" : " [" + holder + "]") +
           ". The library file was not changed. If no VA Studio on any computer is saving this "
           "library (for example after a crash), use Recovery > Remove stale lock, or close VA Studio "
           "everywhere, delete that lock file and save again.";
}
// One byte-based protocol on every OS: Mac and Windows can edit the same SMB
// file and MUST choose the same lock name for Unicode names. Extracted
// unchanged from Lock::acquire so lock inspection uses the identical name.
std::string lock_path_for(std::string const &destination)
{
    auto base = std::unique_ptr<gchar, decltype(&g_free)>(g_path_get_basename(destination.c_str()), &g_free);
    auto length = std::strlen(base.get());
    auto path = destination + ".lock";
    if (length > 250) {
        // Appending .lock must not make a valid 255-unit filename unusable.
        // Preserve existing lock names wherever old versions can save.
        std::string key = base.get();
        auto folded = g_utf8_casefold(key.c_str(), -1); key = folded; g_free(folded);
        auto name = ".valib-lock-" + artwork_sha256(Bytes(key.begin(), key.end())) + ".lock";
        File target = file(destination), parent(g_file_get_parent(target.get()));
        File child(g_file_get_child(parent.get(), name.c_str())); path = path_of(child.get());
    }
    return path;
}

class Lock final {
public:
    void acquire(std::string const &destination)
    {
        path = lock_path_for(destination);
        token = "valib-storage-lock-v1\n" + nonce() + "\n" + lock_holder_lines();
        try {
            identity = write_new(path, Bytes(token.begin(), token.end()), {}, [&](bool initial, std::size_t) {
                if (!initial) return;
                // Mark ownership before fallible inspection; unproven locks remain.
                created = true;
                auto f = file(path); auto i = info(f.get());
                identity = fingerprint(i.get(), path).file_id;
            }, StorageFailure::Locked);
        } catch (StorageError const &e) {
            if (e.failure() != StorageFailure::Locked || created) throw;
            fail(StorageFailure::Locked, locked_message(path));
        }
        // FAT/exFAT may assign a new identity when the first data cluster is
        // allocated. Use the completed writer handle's identity, not the empty
        // file's ID or an unrelated pathname re-query. validate() still compares
        // both that identity and our unique ownership token before any removal.
    }
    void validate() const
    {
        if (!created || identity.empty()) fail(StorageFailure::Conflict, "Writer lock ownership is unproven");
        auto current = read_file(path, 1024, {});
        if (current.version.file_id != identity || std::string(current.bytes.begin(), current.bytes.end()) != token) {
            fail(StorageFailure::Conflict, "Writer lock identity changed; not removing/replacing it");
        }
    }
    void release(StorageResult &result) noexcept
    {
        if (!created) return;
        try {
            validate();
#ifdef _WIN32
            if (!DeleteFileW(native_path(path).c_str())) windows_error("Release writer lock", GetLastError());
#else
            auto f = file(path);
            GError *error = nullptr;
            if (!g_file_delete(f.get(), nullptr, &error)) io_error("Release writer lock", error);
#endif
            created = false;
        } catch (std::exception const &e) {
            result.retained_lock_path = path;
            result.message += std::string("; lock retained: ") + e.what();
        }
    }
private:
    std::string path, identity, token;
    bool created = false;
};

// GIO's Windows directory enumerator can still use MAX_PATH-limited calls.
// Keep the same bounded recovery scan, but use extended-path Win32 enumeration.
class DirectoryReader final {
public:
    explicit DirectoryReader(GFile *folder)
    {
#ifdef _WIN32
        auto pattern = native_path(path_of(folder) + "\\*");
        handle = FindFirstFileW(pattern.c_str(), &data);
        if (handle == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND)
            windows_error("Inspect recovery directory", GetLastError());
#else
        GError *error = nullptr;
        entries.reset(g_file_enumerate_children(folder,
            "standard::name,standard::type,standard::is-symlink,standard::size",
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &error));
        if (!entries) io_error("Inspect recovery directory", error);
#endif
    }
    ~DirectoryReader()
    {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) FindClose(handle);
#endif
    }
    Info next()
    {
#ifdef _WIN32
        if (handle == INVALID_HANDLE_VALUE) return {};
        for (;;) {
            if (!first && !FindNextFileW(handle, &data)) {
                if (GetLastError() != ERROR_NO_MORE_FILES) windows_error("Enumerate recovery directory", GetLastError());
                return {};
            }
            first = false;
            std::wstring_view name(data.cFileName);
            if (name == L"." || name == L"..") continue;
            std::unique_ptr<gchar, decltype(&g_free)> utf8(g_utf16_to_utf8(
                reinterpret_cast<gunichar2 const *>(data.cFileName), -1, nullptr, nullptr, nullptr), &g_free);
            if (!utf8) fail(StorageFailure::Invalid, "Invalid UTF-16 recovery filename");
            Info result(g_file_info_new());
            g_file_info_set_name(result.get(), utf8.get());
            bool link = data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
            g_file_info_set_is_symlink(result.get(), link);
            g_file_info_set_file_type(result.get(), link ? G_FILE_TYPE_SYMBOLIC_LINK :
                data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? G_FILE_TYPE_DIRECTORY : G_FILE_TYPE_REGULAR);
            g_file_info_set_size(result.get(), (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
            return result;
        }
#else
        GError *error = nullptr;
        Info result(g_file_enumerator_next_file(entries.get(), nullptr, &error));
        if (error) io_error("Enumerate recovery directory", error);
        return result;
#endif
    }
    void close()
    {
#ifdef _WIN32
        auto previous = handle; handle = INVALID_HANDLE_VALUE;
        if (previous != INVALID_HANDLE_VALUE && !FindClose(previous)) windows_error("Close recovery directory", GetLastError());
#else
        GError *error = nullptr;
        if (!g_file_enumerator_close(entries.get(), nullptr, &error)) io_error("Close recovery directory", error);
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW data {};
    bool first = true;
#else
    Object<GFileEnumerator> entries;
#endif
};

} // namespace

StorageError::StorageError(StorageFailure failure, std::string message)
    : std::runtime_error(std::move(message)), _failure(failure) {}

std::string canonical_library_path(std::string const &path) { return canonical(path); }

std::string resolved_library_path(std::string const &path)
{
    auto result = canonical(path);
#ifdef _WIN32
    // The native picker may return DOS 8.3 aliases even for a long-path-aware
    // app. Expand names, not links: all normal NOFOLLOW/identity checks still
    // happen when the service opens the file. Missing/inaccessible paths stay
    // intact so their actual storage operation reports the appropriate error.
    auto expand = [](std::string const &input) -> std::optional<std::string> {
        auto wide = native_path(input);
        auto size = GetLongPathNameW(wide.c_str(), nullptr, 0);
        if (!size) {
            auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
            windows_error("Expand native file name", error);
        }
        if (size > 32767) fail(StorageFailure::Invalid, "Expanded file path exceeds the Windows limit");
        std::vector<wchar_t> buffer(size);
        auto written = GetLongPathNameW(wide.c_str(), buffer.data(), size);
        if (!written) windows_error("Read expanded native file name", GetLastError());
        if (written >= size) fail(StorageFailure::Conflict, "File name changed while expanding its path");
        std::unique_ptr<gchar, decltype(&g_free)> utf8(g_utf16_to_utf8(
            reinterpret_cast<gunichar2 const *>(buffer.data()), written, nullptr, nullptr, nullptr), &g_free);
        if (!utf8) fail(StorageFailure::Invalid, "Invalid UTF-16 file name");
        return canonical(utf8.get());
    };
    if (auto expanded = expand(result)) return *expanded;
    // Save As has a new basename, but its existing parent may be an 8.3 alias.
    auto target = file(result);
    File parent(g_file_get_parent(target.get()));
    if (parent) {
        if (auto expanded = expand(path_of(parent.get()))) {
            std::unique_ptr<gchar, decltype(&g_free)> name(g_file_get_basename(target.get()), &g_free);
            result = canonical(*expanded + "\\" + name.get());
        }
    }
#endif
    return result;
}

Bytes read_library_input(std::string path, std::size_t limit, Cancelled cancelled, bool prefix)
{
    return read_file(canonical(path), limit, cancelled, prefix).bytes;
}

void write_library_export(std::string path, Bytes const &bytes, Cancelled cancelled)
{
    path = canonical(path);
    auto target = file(path);
    writable_parent(target.get());
    write_new(path, bytes, cancelled, {});
}

LoadedLibrary load_library(std::string path, StorageLimits limits, Cancelled cancelled)
{
    check(cancelled);
    path = resolved_library_path(path);
    return decode(read_file(path, limits.archive.package_bytes, cancelled), limits, cancelled);
}

RecoveryScan scan_library_recovery(std::string directory, RecoveryScanLimits limits, Cancelled cancelled)
{
    RecoveryScanLimits const ceiling;
    if (!limits.directory_entries || limits.directory_entries > ceiling.directory_entries ||
        !limits.candidate_files || limits.candidate_files > ceiling.candidate_files ||
        !limits.package_bytes || limits.package_bytes > ceiling.package_bytes ||
        !limits.expanded_bytes || limits.expanded_bytes > ceiling.expanded_bytes)
        fail(StorageFailure::Invalid, "Invalid recovery discovery limits");
    check(cancelled);
    directory = resolved_library_path(directory);
    auto folder = file(directory);
    parents(folder.get(), true); // Drive/share roots are valid recovery folders.
    auto before = info(folder.get());
    require_type(before.get(), G_FILE_TYPE_DIRECTORY); require_identity(before.get());
    auto kind = [](std::string_view name) -> std::optional<RecoveryFileKind> {
        constexpr std::string_view stage = ".valib-stage-", previous = ".valib-recovery-", trash = ".trash-";
        if (name.starts_with(stage) && valid_library_uuid(name.substr(stage.size()))) return RecoveryFileKind::Staged;
        if (name.starts_with(previous) && name.ends_with(".valib")) return RecoveryFileKind::PreviousVersion;
        auto at = name.rfind(trash);
        if (at != name.npos && at > 0 && name.ends_with(".valib")) {
            auto id = name.substr(at + trash.size(), name.size() - at - trash.size() - 6);
            if (valid_library_uuid(id)) return RecoveryFileKind::Trash;
        }
        return {};
    };
    DirectoryReader entries(folder.get());
    RecoveryScan result; result.directory = directory;
    auto remaining = limits.expanded_bytes;
    for (;;) {
        check(cancelled);
        auto entry = entries.next();
        if (!entry) break;
        if (result.entries_examined == limits.directory_entries) { result.limited = true; break; }
        ++result.entries_examined;
        auto name = g_file_info_get_name(entry.get());
        auto type = name ? kind(name) : std::nullopt;
        if (!type) continue;
        if (result.files.size() == limits.candidate_files) { result.limited = true; break; }
        File child(g_file_get_child(folder.get(), name));
        RecoveryFile candidate; candidate.path = path_of(child.get()); candidate.kind = *type;
        try {
            require_type(entry.get(), G_FILE_TYPE_REGULAR);
            auto size = g_file_info_get_size(entry.get());
            auto available = limits.package_bytes - result.package_bytes_examined;
            if (size < 0 || std::uint64_t(size) > available || !remaining) {
                result.limited = true;
                fail(StorageFailure::Integrity, "Recovery discovery byte budget exceeded; candidate not verified");
            }
            // Reserve the observed size even if a corrupt candidate cannot load.
            result.package_bytes_examined += size;
            auto storage = limits.storage;
            storage.archive.package_bytes = std::min(storage.archive.package_bytes, std::size_t(size));
            auto loaded = decode(read_file(candidate.path, storage.archive.package_bytes, cancelled),
                                 storage, cancelled, &remaining, &result.limited);
            candidate.label = loaded.package.manifest().name;
            candidate.version = std::move(loaded.version);
            candidate.diagnostic = "Package integrity verified; artwork still requires admission when recovered";
        } catch (std::exception const &e) {
            check(cancelled); // Never disguise cancellation as a damaged candidate.
            candidate.diagnostic = e.what();
        }
        result.files.push_back(std::move(candidate));
    }
    entries.close();
    auto after = info(folder.get());
    require_type(after.get(), G_FILE_TYPE_DIRECTORY); require_identity(after.get());
    if (string_attr(before.get(), G_FILE_ATTRIBUTE_ID_FILE) != string_attr(after.get(), G_FILE_ATTRIBUTE_ID_FILE) ||
        string_attr(before.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM) != string_attr(after.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM))
        fail(StorageFailure::Conflict, "Recovery directory changed during discovery");
    result.expanded_bytes_examined = limits.expanded_bytes - remaining;
    std::sort(result.files.begin(), result.files.end(), [](auto const &a, auto const &b) {
        if (bool(a.version) != bool(b.version)) return bool(a.version);
        if (a.version && a.version->modified_seconds != b.version->modified_seconds)
            return a.version->modified_seconds > b.version->modified_seconds;
        return a.path < b.path;
    });
    check(cancelled);
    return result;
}

RecoveryPruneResult prune_library_recovery(std::string destination, std::string const &library_id,
                                           std::size_t keep, std::string const &current,
                                           std::size_t max_deletions)
{
    RecoveryPruneResult out;
    if (!valid_library_uuid(library_id) || !keep) fail(StorageFailure::Invalid, "Invalid recovery retention request");
    destination = resolved_library_path(destination);
    auto target = file(destination);
    parents(target.get());
    File folder(g_file_get_parent(target.get()));
    auto prefix = ".valib-recovery-" + library_id + "-r";
    struct Candidate { std::uint64_t revision; std::string sha256, path; };
    std::vector<Candidate> found;
    DirectoryReader entries(folder.get());
    for (std::size_t examined = 0;; ++examined) {
        if (examined == 65536) { out.warnings.push_back("Stopped after 65536 folder entries"); break; }
        auto entry = entries.next();
        if (!entry) break;
        auto raw = g_file_info_get_name(entry.get());
        if (!raw || g_file_info_get_file_type(entry.get()) != G_FILE_TYPE_REGULAR) continue;
        std::string_view name(raw);
        if (!name.starts_with(prefix) || !name.ends_with(".valib")) continue;
        auto rest = name.substr(prefix.size(), name.size() - prefix.size() - 6); // <rev>-<sha>-<uuid>
        auto dash = rest.find('-');
        if (dash == 0 || dash == std::string_view::npos) continue;
        std::uint64_t revision = 0;
        auto parsed = std::from_chars(rest.data(), rest.data() + dash, revision);
        if (parsed.ec != std::errc{} || parsed.ptr != rest.data() + dash) continue;
        auto tail = rest.substr(dash + 1);
        if (tail.size() != 64 + 1 + 36 || tail[64] != '-' || !valid_library_uuid(tail.substr(65)) ||
            tail.substr(0, 64).find_first_not_of("0123456789abcdef") != std::string_view::npos) continue;
        File child(g_file_get_child(folder.get(), raw));
        found.push_back({revision, std::string(tail.substr(0, 64)), path_of(child.get())});
    }
    entries.close();
    if (!current.empty()) {
        // The copy this save wrote is never a candidate and takes one slot. Copies
        // above its revision belong to a same-ID sibling file; leave them alone.
        auto const mine = std::find_if(found.begin(), found.end(), [&](auto const &c) {
            return canonical_library_path(c.path) == canonical_library_path(current);
        });
        if (mine == found.end()) {
            out.warnings.push_back("The new recovery copy was not found; nothing was removed");
            return out;
        }
        auto const ceiling = mine->revision;
        found.erase(mine);
        std::erase_if(found, [&](auto const &c) { return c.revision > ceiling; });
        --keep;
    }
    if (found.size() <= keep) return out;
    std::sort(found.begin(), found.end(), [](auto const &a, auto const &b) {
        return a.revision != b.revision ? a.revision > b.revision : a.path > b.path;
    });
    // Bounded work per save: at most `max_deletions` removals and three times as
    // many verifications, so copies that keep failing verification cannot starve
    // the older copies behind them.
    std::size_t attempts = 0;
    for (std::size_t i = keep; i < found.size(); ++i) {
        if (out.removed.size() == max_deletions || attempts == 3 * max_deletions) {
            out.warnings.push_back(std::to_string(found.size() - i) +
                                   " older recovery copies remain; later saves remove them");
            break;
        }
        ++attempts;
        auto const &c = found[i];
        try {
            auto f = file(c.path);
            single_link(f.get());
            auto loaded = load_library(c.path);
            if (loaded.version.library_id != library_id || loaded.version.revision != c.revision ||
                loaded.version.sha256 != c.sha256) {
                out.warnings.push_back("Kept a recovery copy that does not match its name: " + c.path);
                continue;
            }
            auto seen = loaded.version; seen.sha256.clear(); seen.library_id.clear(); seen.revision = 0;
            auto now = info(f.get());
            if (fingerprint(now.get(), loaded.version.path) != seen) {
                out.warnings.push_back("Kept a recovery copy that changed during verification: " + c.path);
                continue;
            }
#ifdef _WIN32
            if (!DeleteFileW(native_path(c.path).c_str())) windows_error("Remove old recovery copy", GetLastError());
#else
            GError *error = nullptr;
            if (!g_file_delete(f.get(), nullptr, &error)) io_error("Remove old recovery copy", error);
#endif
            out.removed.push_back(c.path);
        } catch (std::exception const &e) {
            out.warnings.push_back("Kept " + c.path + ": " + e.what());
        }
    }
    return out;
}

std::optional<LibraryLock> inspect_library_lock(std::string destination)
{
    destination = resolved_library_path(destination);
    auto path = lock_path_for(destination);
    auto f = file(path);
    if (!info(f.get(), true)) return std::nullopt;
    auto read = read_file(path, 1024, {});
    return LibraryLock{path, lock_holder_summary(read.bytes), std::move(read.version)};
}

void remove_stale_library_lock(LibraryLock const &inspected)
{
    if (inspected.path.empty()) fail(StorageFailure::Invalid, "No lock was inspected");
    auto current = read_file(inspected.path, 1024, {});
    if (current.version != inspected.version) {
        fail(StorageFailure::Conflict, "The lock changed after it was inspected; it was not removed");
    }
    auto f = file(inspected.path);
    single_link(f.get());
#ifdef _WIN32
    if (!DeleteFileW(native_path(inspected.path).c_str())) windows_error("Remove stale writer lock", GetLastError());
#else
    GError *error = nullptr;
    if (!g_file_delete(f.get(), nullptr, &error)) io_error("Remove stale writer lock", error);
#endif
}

StorageResult save_library(std::string path, CatalogSnapshot snapshot,
                           std::optional<FileVersion> expected, StorageOptions options)
{
    StorageResult result;
    Lock lock;
    bool move_started = false;
    try {
        check(options.cancelled);
        path = resolved_library_path(path);
        if (expected) expected->path = resolved_library_path(expected->path);
        auto target = file(path);
        auto parent = writable_parent(target.get());
        single_link(target.get());
        lock.acquire(path);
        event(options, result, path, StoragePhase::Locked);
        check_parent(target.get(), parent);

        std::optional<Read> previous;
        std::optional<LoadedLibrary> old;
        if (expected) {
            if (expected->path != path) fail(StorageFailure::Conflict, "Expected version belongs to another path");
            if (!info(target.get(), true)) fail(StorageFailure::Conflict, "Previously committed package is missing");
            previous = read_file(path, options.limits.archive.package_bytes, options.cancelled);
            old = decode(*previous, options.limits, options.cancelled);
            if (old->version != *expected) fail(StorageFailure::Conflict, "Committed package changed since load");
        } else if (info(target.get(), true)) {
            fail(StorageFailure::Conflict, "Create destination already exists");
        }

        bool cancelled_in_encode = false;
        Bytes encoded;
        try {
            encoded = encode_catalog(snapshot, options.limits.archive, options.limits.manifest, [&] {
                cancelled_in_encode = options.cancelled && options.cancelled(); return cancelled_in_encode;
            });
        } catch (std::exception const &e) {
            fail(cancelled_in_encode ? StorageFailure::Cancelled : StorageFailure::Integrity, e.what());
        }
        auto integrity_checkpoint = [&] { check(options.cancelled); return false; };
        auto digest = artwork_sha256(encoded, integrity_checkpoint);
        auto desired = snapshot.manifest(integrity_checkpoint);
        if (old) {
            if (desired.id != old->version.library_id || desired.revision < old->version.revision) {
                fail(StorageFailure::Conflict, "Library UUID or revision would replace another history");
            }
            if (desired.revision == old->version.revision) {
                if (desired.serialize(options.limits.manifest) != old->package.manifest().serialize(options.limits.manifest)) {
                    fail(StorageFailure::Conflict, "Different catalog at the same revision");
                }
                check(options.cancelled);
                lock.validate();
                check_parent(target.get(), parent);
                if (load_library(path, options.limits).version != *expected) {
                    fail(StorageFailure::Conflict, "External change during no-op save");
                }
                result.publication = Publication::Unchanged;
                result.version = old->version;
                lock.release(result);
                return result;
            }
        }

        File directory(g_file_get_parent(target.get()));
        auto companion = [&](std::string const &name) {
            File child(g_file_get_child(directory.get(), name.c_str())); return path_of(child.get());
        };
        result.staged_path = companion(".valib-stage-" + nonce());
        event(options, result, path, StoragePhase::BeforeStageCreate);
        write_new(result.staged_path, encoded, options.cancelled, [&](bool created, std::size_t bytes) {
            event(options, result, path, created ? StoragePhase::StageCreated : StoragePhase::StageChunkWritten, bytes);
        });
        auto staged_version = load_library(result.staged_path, options.limits, options.cancelled).version;
        if (staged_version.sha256 != digest || staged_version.library_id != desired.id || staged_version.revision != desired.revision) {
            fail(StorageFailure::Integrity, "Disk staging does not match encoded snapshot");
        }
        event(options, result, path, StoragePhase::StageVerified);

        if (old) {
            result.recovery_path = companion(".valib-recovery-" + old->version.library_id + "-r" +
                std::to_string(old->version.revision) + "-" + old->version.sha256 + "-" + nonce() + ".valib");
            event(options, result, path, StoragePhase::BeforeRecoveryCreate);
            write_new(result.recovery_path, previous->bytes, options.cancelled, [&](bool created, std::size_t bytes) {
                event(options, result, path, created ? StoragePhase::RecoveryCreated : StoragePhase::RecoveryChunkWritten, bytes);
            });
            auto recovery = load_library(result.recovery_path, options.limits, options.cancelled);
            if (recovery.version.sha256 != old->version.sha256) fail(StorageFailure::Integrity, "Recovery differs from prior committed package");
            flush_parent(target.get());
            event(options, result, path, StoragePhase::RecoveryVerified);
        }

        event(options, result, path, StoragePhase::BeforeAdmission);
        check(options.cancelled); // Last cancellable admission; no caller code until AFTER move.
        lock.validate();
        check_parent(target.get(), parent);
        single_link(target.get());
        // Revalidate BOTH staged package and prior file after all callbacks. An
        // uncooperative writer may still race the check-to-rename interval.
        auto final_stage = load_library(result.staged_path, options.limits).version;
        if (final_stage != staged_version) fail(StorageFailure::Conflict, "Staged file changed before publication");
        if (expected) {
            if (!info(target.get(), true)) fail(StorageFailure::Conflict, "Committed package disappeared before publication");
            auto current = load_library(path, options.limits).version;
            if (current != *expected) fail(StorageFailure::Conflict, "External change before publication");
            auto recovery = load_library(result.recovery_path, options.limits).version;
            if (recovery.sha256 != expected->sha256) fail(StorageFailure::Conflict, "Recovery changed before publication");
        } else if (info(target.get(), true)) {
            fail(StorageFailure::Conflict, "Create destination appeared before publication");
        }
        move_started = true;
        result.publication = Publication::Uncertain;
#ifdef _WIN32
        // Same directory, no COPY_ALLOWED fallback. The wide extended paths
        // apply to both sides, including UNC and recovery-sized deep paths.
        auto source = native_path(result.staged_path), destination = native_path(path);
        if (!MoveFileExW(source.c_str(), destination.c_str(),
                         MOVEFILE_WRITE_THROUGH | (expected ? MOVEFILE_REPLACE_EXISTING : 0))) {
            windows_error("Native publication move failed; inspect destination before retry", GetLastError());
        }
#else
        auto staged_file = file(result.staged_path);
        GError *error = nullptr;
        auto flags = GFileCopyFlags(G_FILE_COPY_NO_FALLBACK_FOR_MOVE | G_FILE_COPY_NOFOLLOW_SYMLINKS |
                                   (expected ? G_FILE_COPY_OVERWRITE : GFileCopyFlags(0)));
        if (!g_file_move(staged_file.get(), target.get(), flags, nullptr, nullptr, nullptr, &error)) {
            io_error("Native publication move failed; inspect destination before retry", error);
        }
#endif
        result.publication = Publication::Published;
        // The staged package IS the destination now; there is no retained stage.
        result.staged_path.clear();
        event(options, result, path, StoragePhase::Moved);
        result.directory_flush_completed = flush_parent(target.get());
        auto committed = load_library(path, options.limits);
        if (committed.version.sha256 != digest || committed.version.library_id != desired.id || committed.version.revision != desired.revision) {
            result.publication = Publication::Uncertain;
            fail(StorageFailure::Conflict, "Post-publication file differs; recovery retained, no rollback attempted");
        }
        result.version = committed.version;
        event(options, result, path, StoragePhase::Published);
        result.cancellation_too_late = options.cancelled && options.cancelled();
        result.message = result.cancellation_too_late ? "Published; cancellation arrived after admission" : "Published";
    } catch (StorageError const &e) {
        result.failure = e.failure(); result.message = e.what();
    } catch (std::exception const &e) {
        result.failure = StorageFailure::Io; result.message = e.what();
    } catch (...) {
        result.failure = StorageFailure::Io; result.message = "Unknown storage callback/IO failure";
    }
    if (move_started && !result.version && result.publication == Publication::Published) {
        result.publication = Publication::Uncertain;
    }
    if (move_started && result.failure == StorageFailure::Cancelled) result.cancellation_too_late = true;
    lock.release(result);
    return result;
}

} // namespace Inkscape::IO::ArtworkLibrary
