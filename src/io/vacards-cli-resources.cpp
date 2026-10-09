// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/vacards-cli-resources.h"
#include "actions/vacards-cli-fault.h"
#include <filesystem>
#include <algorithm>
#include <functional>
#include <cerrno>
#include <climits>
#include <glib.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/mount.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
namespace Inkscape::VACardsCli {
#ifndef _WIN32
struct AdmittedFile {
    std::vector<int> dirs;
    std::vector<std::string> names;
    struct stat leaf{};
    ~AdmittedFile() { for (int fd : dirs) ::close(fd); }
};
static ResourceAccess pin(ResourceAccess access)
{
    if (access.state != "granted") return access;
    auto pin = std::make_shared<AdmittedFile>();
    int root = ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return {"unavailable", access.path};
    pin->dirs.push_back(root);
    std::filesystem::path path(access.path);
    for (auto const &part : path.relative_path().parent_path()) {
        int fd = ::openat(pin->dirs.back(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return {"unavailable", access.path};
        pin->names.push_back(part.string()); pin->dirs.push_back(fd);
    }
    pin->names.push_back(path.filename().string());
    if (::fstatat(pin->dirs.back(), pin->names.back().c_str(), &pin->leaf, AT_SYMLINK_NOFOLLOW) || !S_ISREG(pin->leaf.st_mode))
        return {"unavailable", access.path};
    access.admitted = std::move(pin); return access;
}
static AdmittedBytes read_admitted_impl(ResourceAccess const &access, std::uint64_t limit,
                                        std::function<void()> const *after_stat)
{
    AdmittedBytes out;
    auto fail = [&](char const *error) { out.bytes.clear(); out.error=error; return out; };
    if (access.state != "granted" || !access.admitted) return fail(access.state=="missing" ? "missing-file" : access.state=="invalid" ? "invalid-path" : access.state=="unavailable" ? "resource-unavailable" : "read-grant-denied");
    auto const &p=*access.admitted;
    for (std::size_t i=1; i<p.dirs.size(); ++i) {
        struct stat named{}, held{};
        if (::fstatat(p.dirs[i-1],p.names[i-1].c_str(),&named,AT_SYMLINK_NOFOLLOW) ||
            ::fstat(p.dirs[i],&held) || !S_ISDIR(named.st_mode) || named.st_dev!=held.st_dev || named.st_ino!=held.st_ino)
            return fail("stale-dependency");
    }
    int fd=::openat(p.dirs.back(),p.names.back().c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0) return fail(errno==ELOOP ? "stale-dependency" : "resource-unavailable");
    struct Close { int fd; ~Close(){::close(fd);} } close{fd};
    struct stat before{},after{},named{};
    if(cli_fault("resources.read.identity", CliFaultKind::StaleDependency) || ::fstat(fd,&before) || !S_ISREG(before.st_mode) || before.st_dev!=p.leaf.st_dev || before.st_ino!=p.leaf.st_ino)
        return fail("stale-dependency");
    out.found=before.st_size<0 ? 0 : std::uint64_t(before.st_size);
    if(before.st_size<0 || std::uint64_t(before.st_size)>limit) return fail("engine-limit");
    if (after_stat) (*after_stat)(); // Test-only per-call observer; production passes nullptr.
    char buffer[32768];
    // Never read beyond the admitted budget, including when the file grows after fstat.
    // At the limit, go straight to the final size/identity checks; no EOF probe.
    while (out.bytes.size() < limit) {
        auto remaining = std::min<std::uint64_t>(sizeof(buffer), limit - out.bytes.size());
        auto n = ::read(fd, buffer, static_cast<std::size_t>(remaining));
        if (n < 0) return fail("resource-unavailable");
        if (!n) break;
        out.bytes.append(buffer, n);
    }
    if(::fstat(fd,&after) || ::fstatat(p.dirs.back(),p.names.back().c_str(),&named,AT_SYMLINK_NOFOLLOW) ||
       after.st_dev!=named.st_dev || after.st_ino!=named.st_ino || before.st_size!=after.st_size ||
       before.st_mtime!=after.st_mtime || before.st_ctime!=after.st_ctime || out.bytes.size()!=std::uint64_t(after.st_size))
        return fail("stale-dependency");
#ifdef __APPLE__
    if(before.st_mtimespec.tv_nsec!=after.st_mtimespec.tv_nsec || before.st_ctimespec.tv_nsec!=after.st_ctimespec.tv_nsec)
        return fail("stale-dependency");
#endif
    for(std::size_t i=1;i<p.dirs.size();++i) {
        struct stat named_dir{},held{};
        if(::fstatat(p.dirs[i-1],p.names[i-1].c_str(),&named_dir,AT_SYMLINK_NOFOLLOW) || ::fstat(p.dirs[i],&held) ||
           named_dir.st_dev!=held.st_dev || named_dir.st_ino!=held.st_ino || !S_ISDIR(named_dir.st_mode)) return fail("stale-dependency");
    }
    auto sum=g_compute_checksum_for_data(G_CHECKSUM_SHA256,reinterpret_cast<guchar const *>(out.bytes.data()),out.bytes.size());
    out.sha256=sum; g_free(sum); out.identity=std::to_string(after.st_dev)+":"+std::to_string(after.st_ino); return out;
}
#else
bool allowed_windows_reparse_tag(std::uint32_t tag)
{
    // CLOUD and CLOUD_1..CLOUD_F are storage filters, never name surrogates.
    return !(tag & 0x20000000u) && (tag & ~0x0000f000u)==0x9000001au;
}
bool safe_windows_file_handle(void *handle)
{
    FILE_ATTRIBUTE_TAG_INFO info{};
    return GetFileInformationByHandleEx(static_cast<HANDLE>(handle),FileAttributeTagInfo,&info,sizeof(info)) &&
        (!(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || allowed_windows_reparse_tag(info.ReparseTag));
}
static std::wstring wide_path(std::filesystem::path const &path)
{
    auto wide=path; wide.make_preferred();
    auto native=wide.wstring();
    if(!native.starts_with(L"\\\\?\\")) native=L"\\\\?\\"+native;
    return native;
}
static std::filesystem::path final_path(HANDLE handle)
{
    DWORD n=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!n) return {};
    std::wstring out(n,L'\0');
    DWORD written=GetFinalPathNameByHandleW(handle,out.data(),n,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!written || written>=n) return {};
    out.resize(written);
    if(out.starts_with(L"\\\\?\\UNC\\")) return {};
    if(out.starts_with(L"\\\\?\\")) out.erase(0,4);
    return std::filesystem::path(out);
}
static bool same_path(std::filesystem::path const &a,std::filesystem::path const &b)
{
    auto i=a.begin(), j=b.begin();
    std::filesystem::path parent;
    for (; i!=a.end() && j!=b.end(); ++i,++j) {
        bool sensitive=false;
        if (!parent.empty() && parent.has_root_directory()) {
            HANDLE h=CreateFileW(wide_path(parent).c_str(),FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
            if(h==INVALID_HANDLE_VALUE) return false;
            // FileCaseSensitiveInfo (23), available since Windows 10 1803.
            // Use the ABI value so older supported SDK headers still compile.
            struct { ULONG Flags; } info{};
            bool ok=GetFileInformationByHandleEx(h,static_cast<FILE_INFO_BY_HANDLE_CLASS>(23),&info,sizeof(info));
            auto error=ok ? ERROR_SUCCESS : GetLastError();
            CloseHandle(h);
            if(!ok && error!=ERROR_INVALID_PARAMETER && error!=ERROR_NOT_SUPPORTED) return false;
            sensitive=ok && (info.Flags & 1u);
        }
        auto x=i->wstring(), y=j->wstring();
        if(CompareStringOrdinal(x.c_str(),int(x.size()),y.c_str(),int(y.size()),!sensitive)!=CSTR_EQUAL) return false;
        parent/=*i;
    }
    return i==a.end() && j==b.end();
}

struct AdmittedFile {
    std::vector<HANDLE> handles;
    BY_HANDLE_FILE_INFORMATION leaf{};
    bool missing_leaf=false;
    ~AdmittedFile(){ for(auto h:handles) CloseHandle(h); }
};
static std::shared_ptr<AdmittedFile> windows_pin(std::filesystem::path const &path)
{
    auto p=std::make_shared<AdmittedFile>();
    auto fail=[&](DWORD code) {p.reset();SetLastError(code);return std::shared_ptr<AdmittedFile>{};};
    std::filesystem::path walk=path.root_path();
    std::vector<std::filesystem::path> components{walk};
    for(auto const &part:path.relative_path()) {
        // Extended Win32 paths do not interpret dot components. Validate every
        // visited ancestor before returning the final spelling: normalizing the
        // whole path first would hide a junction followed by "..".
        if(part == ".") continue;
        if(part == "..") { if(walk != walk.root_path()) walk=walk.parent_path(); }
        else walk/=part;
        components.push_back(walk);
    }
    for(auto const &component:components) {
        auto wide=wide_path(component);
        HANDLE h=CreateFileW(wide.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        if(h==INVALID_HANDLE_VALUE) return fail(GetLastError());
        p->handles.push_back(h);
        if(!safe_windows_file_handle(h)) return fail(ERROR_REPARSE_TAG_INVALID);
        if(final_path(h).empty()) return fail(ERROR_CANT_ACCESS_FILE);
    }
    if(!GetFileInformationByHandle(p->handles.back(),&p->leaf)) return fail(GetLastError());
    return p;
}
std::shared_ptr<void> retain_windows_path(std::string const &path)
{ return windows_pin(std::filesystem::u8path(path)); }
static ResourceAccess pin(ResourceAccess access)
{
    if(access.state!="granted") return access;
    access.admitted=windows_pin(std::filesystem::u8path(access.path));
    if(!access.admitted && GetLastError()==ERROR_FILE_NOT_FOUND) {
        access.admitted=windows_pin(std::filesystem::u8path(access.path).parent_path());
        if(access.admitted) access.admitted->missing_leaf=true;
    }
    if(!access.admitted)
        return {"unavailable",access.path};
    return access;
}
static AdmittedBytes read_admitted_impl(ResourceAccess const &access, std::uint64_t limit,
                                        std::function<void()> const *after_stat)
{
    AdmittedBytes out;
    auto fail=[&](char const *code){out.bytes.clear();out.error=code;return out;};
    if(access.state!="granted" || !access.admitted) return fail(access.state=="missing" ? "missing-file" : access.state=="invalid" ? "invalid-path" : access.state=="unavailable" ? "resource-unavailable" : "read-grant-denied");
    auto wide=wide_path(std::filesystem::u8path(access.path));
    HANDLE h=CreateFileW(wide.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(h==INVALID_HANDLE_VALUE) return fail("resource-unavailable");
    struct Close { HANDLE h; ~Close(){CloseHandle(h);} } close{h};
    BY_HANDLE_FILE_INFORMATION before{},after{};
    auto const &expected=access.admitted->leaf;
    if(cli_fault("resources.read.identity", CliFaultKind::StaleDependency) || !safe_windows_file_handle(h) || !GetFileInformationByHandle(h,&before) ||
        before.dwVolumeSerialNumber!=expected.dwVolumeSerialNumber || before.nFileIndexHigh!=expected.nFileIndexHigh ||
        before.nFileIndexLow!=expected.nFileIndexLow || !same_path(final_path(h),std::filesystem::u8path(access.path)))
        return fail("stale-dependency");
    std::uint64_t size=(std::uint64_t(before.nFileSizeHigh)<<32)|before.nFileSizeLow;
    out.found=size;
    if(size>limit) return fail("engine-limit");
    if (after_stat) (*after_stat)(); // Test-only per-call observer; production passes nullptr.
    char buffer[32768]; DWORD n=0;
    // Do not probe EOF past the budget; final metadata rejects growth.
    while (out.bytes.size() < limit) {
        auto remaining = std::min<std::uint64_t>(sizeof(buffer), limit - out.bytes.size());
        if (!ReadFile(h, buffer, static_cast<DWORD>(remaining), &n, nullptr)) return fail("resource-unavailable");
        if (!n) break;
        out.bytes.append(buffer, n);
    }
    if(!GetFileInformationByHandle(h,&after) || before.nFileSizeHigh!=after.nFileSizeHigh ||
       before.nFileSizeLow!=after.nFileSizeLow || CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime) || out.bytes.size()!=size)
        return fail("stale-dependency");
    auto sum=g_compute_checksum_for_data(G_CHECKSUM_SHA256,reinterpret_cast<guchar const *>(out.bytes.data()),out.bytes.size());
    out.sha256=sum;g_free(sum);out.identity=std::to_string(after.dwVolumeSerialNumber)+":"+
        std::to_string(after.nFileIndexHigh)+":"+std::to_string(after.nFileIndexLow);return out;
}
#endif
AdmittedBytes read_admitted(ResourceAccess const &access, std::uint64_t limit)
{
    return read_admitted_impl(access, limit, nullptr);
}
// Declaration lives only in the test header; no global hook or request route.
AdmittedBytes read_admitted_after_stat_for_testing(ResourceAccess const &access, std::uint64_t limit,
                                                  std::function<void()> const &after_stat)
{
    return read_admitted_impl(access, limit, &after_stat);
}
static std::filesystem::path resolved(std::filesystem::path const &p, std::error_code &ec)
{
#ifdef _WIN32
    auto held=windows_pin(p);
    if(held) { ec.clear(); return final_path(held->handles.back()); }
    auto failure=GetLastError();
    if(failure==ERROR_REPARSE_TAG_INVALID) { ec=std::make_error_code(std::errc::invalid_argument); return {}; }
    auto parent=windows_pin(p.parent_path());
    if(parent) {
        auto leaf=final_path(parent->handles.back())/p.filename();
        if(GetFileAttributesW(wide_path(leaf).c_str())==INVALID_FILE_ATTRIBUTES && GetLastError()==ERROR_FILE_NOT_FOUND) {
            ec.clear(); return leaf;
        }
    }
    ec=std::make_error_code(std::errc::permission_denied); return {};
#else
    auto result=std::filesystem::weakly_canonical(p,ec);
#ifdef __APPLE__
    if(!ec) {
        int fd=::open(result.c_str(), O_RDONLY|O_NONBLOCK|O_NOFOLLOW);
        char actual[PATH_MAX];
        if(fd>=0) { if(!::fcntl(fd,F_GETPATH,actual)) result=actual; ::close(fd); }
        else if(!result.filename().empty()) {
            int parent=::open(result.parent_path().c_str(),O_RDONLY|O_DIRECTORY);
            if(parent>=0) { if(!::fcntl(parent,F_GETPATH,actual)) result=std::filesystem::path(actual)/result.filename(); ::close(parent); }
        }
    }
#endif
    return result;
#endif
}

static std::string path_utf8(std::filesystem::path const &p)
{ auto u=p.u8string(); return {reinterpret_cast<char const *>(u.data()),u.size()}; }
static bool equal_path(std::filesystem::path const &a,std::filesystem::path const &b)
{
#ifdef _WIN32
    return same_path(a,b);
#else
    return a==b;
#endif
}
static bool contained(std::filesystem::path const &path,std::filesystem::path const &root)
{
#ifdef _WIN32
    auto i=path.begin(); std::filesystem::path prefix;
    for(auto const &part:root) { (void)part; if(i==path.end()) return false; prefix/=*i++; }
    return same_path(prefix,root);
#else
    auto i=path.begin();
    for(auto const &part:root) { if(i==path.end() || !equal_path(*i,part)) return false; ++i; }
    return true;
#endif
}
static bool safe_public_path(std::filesystem::path const &path)
{
#ifdef _WIN32
    for(auto const &part:path.relative_path()) {
        auto leaf=part.wstring();
        if(leaf==L"." || leaf==L"..") continue; // Navigation, not a trailing-dot filename.
        if(leaf.empty() || leaf.find(L':')!=std::wstring::npos || leaf.back()==L'.' || leaf.back()==L' ') return false;
        auto device=leaf.substr(0,leaf.find(L'.'));
        for(auto &c:device) if(c>=L'a' && c<=L'z') c-=L'a'-L'A';
        if(device==L"CON" || device==L"PRN" || device==L"AUX" || device==L"NUL" ||
            (device.size()==4 && (device.starts_with(L"COM") || device.starts_with(L"LPT")) && device[3]>=L'1' && device[3]<=L'9')) return false;
    }
#endif
    return true;
}
static bool directory(std::filesystem::path const &path,std::error_code &ec)
{
#ifdef _WIN32
    auto attrs=GetFileAttributesW(wide_path(path).c_str());ec.clear();
    if(attrs==INVALID_FILE_ATTRIBUTES) {ec=std::error_code(GetLastError(),std::system_category());return false;}
    return attrs & FILE_ATTRIBUTE_DIRECTORY;
#else
    return std::filesystem::is_directory(path,ec);
#endif
}
static bool regular(std::filesystem::path const &path,std::error_code &ec)
{
#ifdef _WIN32
    auto attrs=GetFileAttributesW(wide_path(path).c_str());ec.clear();
    if(attrs==INVALID_FILE_ATTRIBUTES) {ec=std::error_code(GetLastError(),std::system_category());return false;}
    return !(attrs & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_DEVICE));
#else
    return std::filesystem::is_regular_file(path,ec);
#endif
}
// Spelling containment alone is insufficient if an ancestor changes while grants
// are being resolved. Compare the grant entry with the retained request identity.
static bool grant_identity(ResourceAccess const &candidate,std::filesystem::path const &grant,bool root)
{
#ifndef _WIN32
    if(!candidate.admitted) return true; // Missing/type classification follows authorization.
#else
    if(!candidate.admitted) return false;
#endif
    auto relative=grant.relative_path();
    auto depth=std::size_t(std::distance(relative.begin(),relative.end()));
#ifndef _WIN32
    struct stat actual{},held{};
    if(::lstat(grant.c_str(),&actual)) return false;
    if(root) {
        if(depth>=candidate.admitted->dirs.size() || ::fstat(candidate.admitted->dirs[depth],&held)) return false;
    } else held=candidate.admitted->leaf;
    return actual.st_dev==held.st_dev && actual.st_ino==held.st_ino;
#else
    auto retained=windows_pin(!root && candidate.admitted->missing_leaf ? grant.parent_path() : grant); if(!retained) return false;
    BY_HANDLE_FILE_INFORMATION held{};
    if(root) {
        if(depth>=candidate.admitted->handles.size() ||
            !GetFileInformationByHandle(candidate.admitted->handles[depth],&held)) return false;
    } else held=candidate.admitted->leaf;
    auto const &actual=retained->leaf;
    return actual.dwVolumeSerialNumber==held.dwVolumeSerialNumber && actual.nFileIndexHigh==held.nFileIndexHigh && actual.nFileIndexLow==held.nFileIndexLow;
#endif
}
static ResourceAccess inspect_path(std::string const &href, std::string const &base, Grants const &grants)
{
    namespace fs = std::filesystem;
    std::string local = href;
    if(local.empty() || local.find('\0')!=std::string::npos) return {"unsafe",{}};
    if (local.starts_with("//") || local.starts_with("\\\\") || local.starts_with("\\??\\")) return {"remote", {}};
    // A drive letter is native path syntax, not a one-letter URI scheme.
    bool drive_path = local.size() >= 3 && g_ascii_isalpha(local[0]) && local[1] == ':' &&
                      (local[2] == '\\' || local[2] == '/');
#ifndef _WIN32
    // Foreign native paths remain local but cannot be resolved on this host.
    if (drive_path) return {"ungranted", {}};
#endif
    auto scheme = drive_path || fs::u8path(local).is_absolute() ? nullptr : g_uri_parse_scheme(local.c_str());
    if (scheme) {
        bool file = g_ascii_strcasecmp(scheme, "file") == 0;
        g_free(scheme);
        if (!file) return {"remote", {}};
        char *host = nullptr;
        char *name = g_filename_from_uri(local.c_str(), &host, nullptr);
        bool remote = host && *host && g_ascii_strcasecmp(host, "localhost") != 0;
        g_free(host);
        if (!name || remote) { g_free(name); return {remote ? "remote" : "ungranted", {}}; }
        local = name;
        g_free(name);
    }
    if (local.starts_with("//") || local.starts_with("\\\\")) return {"remote", {}};
    std::error_code ec;
    auto path = fs::u8path(local);
    if (!path.is_absolute()) path = fs::u8path(base) / path;
#ifdef _WIN32
    // Without a drive-qualified base, a rooted ("/x") or relative reference names no definite local file.
    // Resolving it would fail as an invalid \\?\ name and report a retryable "unavailable".
    if (!path.is_absolute()) return {"ungranted", {}};
#endif
    if(!safe_public_path(path)) return {"unsafe", {}};
    path = resolved(path, ec);
    if(ec) return {ec==std::errc::permission_denied ? "unavailable" : "ungranted",{}};
    if (!path.is_absolute()) return {"ungranted", {}};
    auto candidate=pin({"granted",path_utf8(path)});
    bool admitted = false;
    for (auto const &root : grants.read_roots) {
        auto r = fs::u8path(root);
        if (!r.is_absolute()) continue;
        r = resolved(r, ec);
        if (ec || !directory(r, ec)) continue;
        if (contained(path,r) && grant_identity(candidate,r,true)) { admitted = true; break; }
    }
    for (auto const &file : grants.read_files) {
        auto f = fs::u8path(file);
        if (!f.is_absolute()) continue;
        f = resolved(f, ec);
        if (!ec && equal_path(f,path) && grant_identity(candidate,f,false)) admitted = true;
    }
    if (!admitted) return {"ungranted", path_utf8(path)};
    if (!regular(path, ec) || ec) return {directory(path,ec) ? "invalid" : "missing", path_utf8(path)};
    return candidate;
}
ResourceAccess inspect_write_destination(std::string const &name, Grants const &grants)
{
    namespace fs = std::filesystem;
    fs::path path=fs::u8path(name); std::error_code ec;
    if (name.empty() || name.find('\0') != std::string::npos || !path.is_absolute() ||
        name.starts_with("//") || name.starts_with("\\") || path != path.lexically_normal() ||
        path.filename().empty()) return {"unsafe", {}};
    if(!safe_public_path(path)) return {"unsafe", {}};
    path = resolved(path, ec);
    if (ec) return {ec==std::errc::permission_denied ? "unavailable" : "unsafe", {}};
    auto parent = path.parent_path();
    if (!directory(parent, ec) || ec) return {"unsafe", {}};
    fs::path walk=path.root_path();
    for (auto const &part : path.relative_path()) {
        walk /= part;
#ifndef _WIN32
        auto st = fs::symlink_status(walk, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) return {"unsafe", {}};
        if (!ec && fs::is_symlink(st)) return {"unsafe", {}};
#endif
#ifdef _WIN32
        auto held=windows_pin(walk);
        if(!held && walk!=path) return {"unsafe", {}};
#endif
    }
#ifdef __APPLE__
    struct statfs volume{};
    if (::statfs(parent.c_str(), &volume) || !(volume.f_flags & MNT_LOCAL)) return {"remote", {}};
#endif
#ifdef _WIN32
    auto drive = path.root_path().wstring();
    auto kind = GetDriveTypeW(drive.c_str());
    if (kind != DRIVE_FIXED && kind != DRIVE_REMOVABLE && kind != DRIVE_RAMDISK) return {"remote", {}};
#endif
    ec.clear();
    if (!regular(path, ec) && !ec) return {"unsafe", {}};
    auto candidate=pin({"granted",path_utf8(path)});
    bool admitted = false;
    for (auto const &root : grants.write_roots) {
        auto r = fs::u8path(root);
        if (!r.is_absolute()) continue;
        r = resolved(r, ec);
        if (ec || !directory(r, ec)) continue;
        if (contained(path,r) && grant_identity(candidate,r,true)) admitted = true;
    }
    for (auto const &file : grants.write_files) {
        auto f=fs::u8path(file);if(!f.is_absolute()) continue;
        f=resolved(f,ec);
        if(!ec && equal_path(f,path) && grant_identity(candidate,f,false)) admitted=true;
    }
#ifdef _WIN32
    // Keep the missing leaf's parent bound, without holding an existing destination
    // against the native replacement operation (its own handle policy applies).
    if(candidate.admitted && candidate.admitted->missing_leaf) {
        candidate.state=admitted ? "granted" : "ungranted";
        return candidate;
    }
#endif
    return {admitted ? "granted" : "ungranted", path_utf8(path)};
}
ResourceAccess inspect_resource(std::string const &href, std::string const &base, Grants const &grants)
{ return inspect_path(href.substr(0,href.find('#')),base,grants); }
ResourceAccess inspect_command_path(std::string const &path, Grants const &grants)
{
    if(!std::filesystem::u8path(path).is_absolute()) return {"ungranted",{}};
    return inspect_path(path,{},grants);
}
} // namespace Inkscape::VACardsCli
