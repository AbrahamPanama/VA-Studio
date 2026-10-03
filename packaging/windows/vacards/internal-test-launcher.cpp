// SPDX-License-Identifier: GPL-2.0-or-later
// Separate, unsigned internal-test launcher. No global environment or registry writes.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include "crash-report-common.h"
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
#ifndef VACARDS_BUILD_VERSION
#define VACARDS_BUILD_VERSION "unknown"
#endif
#ifndef VACARDS_BUILD_NUMBER
#define VACARDS_BUILD_NUMBER "unknown"
#endif
#ifndef VACARDS_SOURCE_COMMIT
#define VACARDS_SOURCE_COMMIT "unknown"
#endif
constexpr DWORD launcher_error = 0x20000001u;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(Handle const &) = delete;
    Handle &operator=(Handle const &) = delete;
};
static FILETIME utc_now() { FILETIME t; GetSystemTimeAsFileTime(&t); return t; }
static ULONGLONG ticks(FILETIME t) {
    return (ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
static std::string utc_text(FILETIME t, bool filename = false) {
    SYSTEMTIME s{};
    if (!FileTimeToSystemTime(&t, &s)) throw std::runtime_error("UTC conversion");
    char text[40];
    if (filename) std::snprintf(text, sizeof(text), "%04u%02u%02uT%02u%02u%02u%03uZ",
        s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond, s.wMilliseconds);
    else std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond, s.wMilliseconds);
    return text;
}
struct Child { DWORD code, pid; FILETIME start, end; };
static std::wstring variable(wchar_t const *name) {
    wchar_t value[32768];
    DWORD n = GetEnvironmentVariableW(name, value, 32768);
    return n && n < 32768 ? std::wstring(value, n) : std::wstring{};
}
static bool visible_station() noexcept {
    USEROBJECTFLAGS flags{}; DWORD needed = 0;
    return GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags,
                                    sizeof(flags), &needed) && (flags.dwFlags & WSF_VISIBLE);
}
static bool spanish() {
    for (auto key : {L"LANGUAGE", L"LANG"}) {
        auto value = variable(key);
        if (!value.empty()) return value.size() >= 2 && (value[0] == L'e' || value[0] == L'E') &&
            (value[1] == L's' || value[1] == L'S') &&
            (value.size() == 2 || value[2] == L'_' || value[2] == L'-' ||
             value[2] == L'.' || value[2] == L':');
    }
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
}
// Reject destination reparse points, including ancestors. No recursive traversal.
static bool plain_directory(fs::path path) {
    for (unsigned i = 0; i < 128; ++i) {
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
            (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        auto parent = path.parent_path();
        if (parent == path || parent.empty()) return true;
        path = std::move(parent);
    }
    return false;
}
static bool regular(BY_HANDLE_FILE_INFORMATION const &info) {
    return !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
static std::uint64_t size_of(BY_HANDLE_FILE_INFORMATION const &info) {
    return (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}
// Enumerate at most 512 entries with a deadline, and remove only owned completed files.
static bool retain(fs::path const &dir, std::vector<vacards_crash::File> pending) {
    auto reserved = pending;
    WIN32_FIND_DATAW item{};
    HANDLE search = FindFirstFileW((dir / L"*").c_str(), &item);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    struct Search { HANDLE h; ~Search() { FindClose(h); } } guard{search};
    auto deadline = GetTickCount64() + 500;
    unsigned count = 0;
    do {
        if (++count > 512 || GetTickCount64() >= deadline) return false;
        if (item.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        std::wstring wide = item.cFileName;
        if (wide.size() > 40) continue;
        std::string name(wide.begin(), wide.end());
        if (!vacards_crash::stem(name).empty()) pending.push_back({name,
            (std::uint64_t(item.nFileSizeHigh) << 32) | item.nFileSizeLow,
            ticks(item.ftLastWriteTime)});
    } while (FindNextFileW(search, &item));
    if (GetLastError() != ERROR_NO_MORE_FILES) return false;
    auto remove = vacards_crash::prune(pending);
    // If future-dated existing records would evict this reservation, skip publication.
    for (auto const &name : remove) {
        if (std::any_of(reserved.begin(), reserved.end(), [&](auto const &f) { return f.name == name; }))
            return false;
    }
    for (auto const &name : remove) {
        if (GetTickCount64() >= deadline) return false;
        auto path = dir / name;
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) continue; // pending entry
            return false;
        }
        if (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (!DeleteFileW(path.c_str())) return false;
    }
    return true;
}
static std::string copy_dump(fs::path const &local, fs::path const &dir,
                             std::string const &id, Child const &child) {
    auto configured = variable(L"LOCALAPPDATA");
    auto dump_local = configured.empty() ? local : fs::path(configured);
    if (!dump_local.is_absolute()) return "rejected";
    auto source = dump_local / L"CrashDumps" / (L"inkscape.exe." + std::to_wstring(child.pid) + L".dmp");
    auto deadline = GetTickCount64() + 3000;
    std::uint64_t previous_size = UINT64_MAX, previous_time = 0;
    std::string outcome = "absent";
    for (;;) {
        // FILE_SHARE_READ prevents a writer changing the dump while this handle is open.
        Handle input(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (input.value != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(input.value, &info) || !regular(info) ||
                GetFileType(input.value) != FILE_TYPE_DISK) return "rejected";
            auto size = size_of(info), modified = ticks(info.ftLastWriteTime);
            // WER may create/finalize shortly after process exit, but never before start.
            if (ticks(info.ftCreationTime) < ticks(child.start) || modified < ticks(child.start) ||
                ticks(info.ftCreationTime) > ticks(child.end) + 30000000ull ||
                modified > ticks(child.end) + 30000000ull) return "stale";
            if (size > vacards_crash::dump_limit) return "oversized";
            if (size && size == previous_size && modified == previous_time) {
                if (!retain(dir, {{id + ".txt", vacards_crash::record_limit, ticks(child.end)},
                                  {id + ".dmp", size, ticks(child.end)}})) return "retention_failed";
                auto target = dir / (id + ".dmp");
                auto temp = dir / (id + ".dmp.tmp");
                Handle output(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (output.value == INVALID_HANDLE_VALUE) return "copy_failed";
                bool ok = true;
                auto copy_deadline = GetTickCount64() + 5000;
                char buffer[65536]; std::uint64_t left = size;
                while (left && ok) {
                    DWORD got = 0, written = 0;
                    DWORD amount = static_cast<DWORD>(std::min<std::uint64_t>(left, sizeof(buffer)));
                    ok = GetTickCount64() < copy_deadline &&
                        ReadFile(input.value, buffer, amount, &got, nullptr) && got == amount &&
                        WriteFile(output.value, buffer, got, &written, nullptr) && written == got;
                    if (ok) left -= got;
                }
                BY_HANDLE_FILE_INFORMATION after{};
                ok = ok && GetFileInformationByHandle(input.value, &after) &&
                    size_of(after) == size && ticks(after.ftLastWriteTime) == modified &&
                    FlushFileBuffers(output.value);
                CloseHandle(output.value); output.value = INVALID_HANDLE_VALUE;
                if (ok) ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
                if (!ok) { DeleteFileW(temp.c_str()); return "copy_failed"; }
                return "copied";
            }
            previous_size = size; previous_time = modified;
            outcome = "unstable";
        } else {
            auto error = GetLastError();
            outcome = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? "absent" : "locked_or_failed";
        }
        auto now = GetTickCount64();
        if (now >= deadline) return outcome;
        Sleep(static_cast<DWORD>(std::min<ULONGLONG>(250, deadline - now)));
    }
}
static bool write_report(fs::path const &profile, fs::path const &local,
                         Child const &child, bool gui) {
    auto dir = profile / L"CrashReports";
    std::error_code ec;
    if (!plain_directory(profile)) return false;
    fs::create_directory(dir, ec);
    if (ec || !plain_directory(dir)) return false;
    // A nonblocking exclusive lock serializes writers/pruning in this folder.
    Handle lock(CreateFileW((dir / L".writer-lock").c_str(), GENERIC_WRITE, 0, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE |
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION lock_info{};
    if (lock.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(lock.value, &lock_info) ||
        !regular(lock_info)) return false;
    auto id = utc_text(child.end, true) + "-" + std::to_string(child.pid);
    if (!retain(dir, {{id + ".txt", vacards_crash::record_limit, ticks(child.end)}})) return false;
    auto dump = copy_dump(local, dir, id, child);
    auto text = vacards_crash::format({utc_text(child.start), utc_text(child.end), child.pid,
        child.code, gui, VACARDS_BUILD_VERSION, VACARDS_BUILD_NUMBER, VACARDS_SOURCE_COMMIT, dump});
    if (text.size() > vacards_crash::record_limit) return false;
    auto target = dir / (id + ".txt"), temp = dir / (id + ".txt.tmp");
    Handle output(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (output.value == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(output.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size() && FlushFileBuffers(output.value);
    CloseHandle(output.value); output.value = INVALID_HANDLE_VALUE;
    if (ok) ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}
static void crash_notice(fs::path const &profile, fs::path const &local,
                         Child const &child, bool gui) noexcept {
    bool recorded = false;
    try { recorded = write_report(profile, local, child, gui); } catch (...) {}
    if (!gui || !visible_station()) return;
    try {
        bool es = spanish();
        std::wstring message = es ?
            L"VA Studio se cerró inesperadamente. Puede volver a abrirlo. Si hay copias de recuperación (auto-save), se ofrecerán al iniciar. Los cambios recientes pueden no haberse guardado." :
            L"VA Studio closed unexpectedly. You can reopen it. Recovery copies (auto-save) will be offered at the next start if they exist. Recent changes may not have been saved.";
        auto code = vacards_crash::hex(child.code);
        message += (es ? L"\n\nCódigo: " : L"\n\nCode: ") + std::wstring(code.begin(), code.end());
        if (std::string(vacards_crash::label(child.code)) == "abnormal termination")
            message += es ? L" (terminación anormal)" : L" (abnormal termination)";
        if (recorded) message += (es ? L"\n\nInforme local: " : L"\n\nLocal report: ") +
            (profile / L"CrashReports").wstring();
        else message += es ? L"\n\nNo se pudo guardar el informe." : L"\n\nThe report could not be saved.";
        MessageBoxW(nullptr, message.c_str(), L"VA Studio", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    } catch (...) {
        wchar_t fallback[256];
        std::swprintf(fallback, 256, L"VA Studio closed unexpectedly (0x%08lX). Recovery copies (auto-save) will be offered at the next start if they exist.", child.code);
        MessageBoxW(nullptr, fallback, L"VA Studio", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    }
}
static std::wstring quote(std::wstring const &s)
{
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
static void env(wchar_t const *key, fs::path const &value)
{
    if (!SetEnvironmentVariableW(key, value.c_str())) throw std::runtime_error("environment");
}
static Child run(fs::path const &exe, std::wstring args, HANDLE output = nullptr)
{
    std::wstring command = quote(exe.wstring()) + L" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    HANDLE standard_output = output ? output : GetStdHandle(STD_OUTPUT_HANDLE);
    // A GUI-subsystem launcher must explicitly forward redirected CLI handles.
    if (standard_output && standard_output != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = standard_output;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    auto start = utc_now();
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        throw std::runtime_error("could not start bundled executable");
    }
    Handle process(pi.hProcess), thread(pi.hThread);
    DWORD wait = WaitForSingleObject(pi.hProcess, INFINITE);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("child wait failed");
    DWORD code;
    if (!GetExitCodeProcess(pi.hProcess, &code)) throw std::runtime_error("child exit unavailable");
    return {code, pi.dwProcessId, start, utc_now()};
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try {
        std::vector<wchar_t> module(32768);
        DWORD n = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (!n || n == module.size()) throw std::runtime_error("launcher path");
        fs::path root = fs::path(std::wstring(module.data(), n)).parent_path();
        PWSTR appdata = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata)))
            throw std::runtime_error("local appdata");
        // VA Studio starts with its own defaults; never copy the older Inkscape
        // or VACards Test preferences. Keep this path stable across reinstalls
        // and builds so subsequent launches retain the user's new settings.
        fs::path local = fs::path(appdata);
        fs::path profile = local / L"VA Studio" / L"Test" / L"Profile";
        CoTaskMemFree(appdata);
        // Explicit test-harness override; ordinary launches use the dedicated profile above.
        std::vector<wchar_t> override_path(32768);
        DWORD override_size = GetEnvironmentVariableW(L"VACARDS_TEST_PROFILE_DIR", override_path.data(),
                                                       static_cast<DWORD>(override_path.size()));
        if (override_size && override_size < override_path.size()) {
            profile = fs::path(std::wstring(override_path.data(), override_size));
            if (!profile.is_absolute()) throw std::runtime_error("test profile must be absolute");
        }
        fs::create_directories(profile);
        fs::create_directories(profile / L"Cache");
        env(L"INKSCAPE_PROFILE_DIR", profile);
        env(L"XDG_CACHE_HOME", profile / L"Cache");
        env(L"INKSCAPE_APP_ID_TAG", L"va_studio_test");
        env(L"INKSCAPE_DATADIR", root / L"share");
        env(L"INKSCAPE_LOCALEDIR", root / L"share" / L"locale");
        env(L"GSETTINGS_SCHEMA_DIR", root / L"share" / L"glib-2.0" / L"schemas");
        // The bundled query tool discovers its relocated directory from its
        // wide executable path. Its narrow argv/environment conversion loses
        // characters outside the user's ANSI code page, so pass no paths.
        SetEnvironmentVariableW(L"GDK_PIXBUF_MODULEDIR", nullptr);
        for (auto key : {L"GTK_PATH", L"GTK_EXE_PREFIX", L"GIO_EXTRA_MODULES"}) {
            SetEnvironmentVariableW(key, nullptr);
        }
        env(L"GIO_MODULE_DIR", root / L"lib" / L"gio" / L"modules");
        env(L"GI_TYPELIB_PATH", root / L"lib" / L"girepository-1.0");
        env(L"FONTCONFIG_FILE", root / L"etc" / L"fonts" / L"fonts.conf");
        env(L"PYTHONHOME", root);
        env(L"PYTHONPATH", root / L"share" / L"inkscape" / L"extensions");
        env(L"PYTHONNOUSERSITE", L"1");
        env(L"PYTHONDONTWRITEBYTECODE", L"1");
        env(L"INKSCAPE_VACARDS_TIFF_ICC_PROFILE", root / L"share" / L"inkscape" / L"color" / L"icc" / L"TheBest.icc");
        wchar_t windir[MAX_PATH];
        if (!GetWindowsDirectoryW(windir, MAX_PATH)) throw std::runtime_error("Windows directory");
        env(L"PATH", root.wstring() + L"\\bin;" + windir + L"\\System32;" + windir);
        // Remove abandoned caches after a day; active launches keep their files.
        auto const stale_before = fs::file_time_type::clock::now() - std::chrono::hours(24);
        std::error_code sweep_error;
        unsigned sweep_count = 0;
        auto sweep_deadline = GetTickCount64() + 500;
        for (fs::directory_iterator it(profile, sweep_error), end;
             !sweep_error && it != end && ++sweep_count <= 512 && GetTickCount64() < sweep_deadline;
             it.increment(sweep_error)) {
            auto const &entry = *it;
            auto const name = entry.path().filename().wstring();
            if (name.size() > 14 && name.compare(0, 8, L"loaders-") == 0 &&
                name.compare(name.size() - 6, 6, L".cache") == 0) {
                std::error_code file_error;
                auto const modified = entry.last_write_time(file_error);
                if (!file_error && modified < stale_before) fs::remove(entry.path(), file_error);
            }
        }
        // Cache contains relocated absolute DLL paths and is private to this process.
        // CREATE_ALWAYS also recovers from an abandoned cache after PID reuse.
        fs::path cache = profile / (L"loaders-" + std::to_wstring(GetCurrentProcessId()) + L".cache");
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE file = CreateFileW(cache.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("loader cache");
        DWORD cache_code = run(root / L"bin/gdk-pixbuf-query-loaders.exe", L"", file).code;
        CloseHandle(file);
        if (cache_code || fs::file_size(cache) == 0) throw std::runtime_error("loader query failed");
        std::ifstream cache_stream(cache);
        std::string cache_text((std::istreambuf_iterator<char>(cache_stream)), {});
        if (cache_text.find("pixbufloader_svg.dll") == std::string::npos ||
            cache_text.find("libpixbufloader-png.dll") == std::string::npos)
            throw std::runtime_error("required image loaders were not discovered");
        cache_stream.close();
        env(L"GDK_PIXBUF_MODULE_FILE", cache);
        int argc = 0;
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw std::runtime_error("command tokens");
        std::wstring args;
        std::vector<std::wstring> tokens;
        for (int i = 1; i < argc; ++i) { tokens.emplace_back(argv[i]); args += quote(argv[i]) + L" "; }
        LocalFree(argv);
        bool gui = vacards_crash::gui_command(tokens);
        Child child = run(root / L"bin/inkscape.exe", args);
        // No cleanup/notice failure may replace an obtained child result.
        try { std::error_code cleanup_error; fs::remove(cache, cleanup_error); } catch (...) {}
        if (vacards_crash::abnormal(child.code)) crash_notice(profile, local, child, gui);
        ExitProcess(child.code);
    } catch (std::exception const &e) {
        if (visible_station()) MessageBoxA(nullptr, e.what(), "VA Studio launcher failed", MB_OK | MB_ICONERROR);
        ExitProcess(launcher_error);
    }
}
