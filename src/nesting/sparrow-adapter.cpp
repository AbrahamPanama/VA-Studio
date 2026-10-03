// SPDX-License-Identifier: GPL-2.0-or-later
// Port of the tested VACards sandbox transport (Sparrow revision 57c45cd).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#define BOOST_JSON_NO_LIB
#include "sparrow-adapter.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <numeric>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <boost/json.hpp>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include "nesting-ffi.h"
#include "path-prefix.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#ifndef VACARDS_SPARROW_SHA256
#define VACARDS_SPARROW_SHA256 ""
#endif

namespace Inkscape::Nesting {
namespace {
namespace json = boost::json;
using Clock = std::chrono::steady_clock;
using Ring = std::vector<std::pair<double, double>>;
void require(bool ok, char const *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
void checkpoint(Clock::time_point deadline, std::stop_token stop)
{
    require(!stop.stop_requested(), "Nesting cancelled");
    require(Clock::now() < deadline, "Sparrow time allowance expired");
}
// GLib and GIO exchange UTF-8 paths on every platform; std::filesystem::path
// converts from the ANSI code page on Windows. Always go through UTF-8 (the
// C++20 char8_t form of u8path, which does not warn) and back with u8string.
std::filesystem::path pathFromUtf8(char const *utf8)
{
    std::string_view const view = utf8 ? utf8 : "";
    return std::filesystem::path(
        std::u8string(reinterpret_cast<char8_t const *>(view.data()), view.size()));
}
std::string utf8String(std::filesystem::path const &path)
{
    auto const bytes = path.u8string();
    return std::string(reinterpret_cast<char const *>(bytes.data()), bytes.size());
}
std::string helperPath()
{
    return utf8String(pathFromUtf8(get_program_dir()) /
#ifdef _WIN32
                      "vacards-sparrow.exe"
#else
                      "vacards-sparrow"
#endif
    );
}
std::string readFile(std::filesystem::path const &path, std::uintmax_t limit)
{
    require(std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) <= limit,
            "Missing or oversized Sparrow file");
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "Cannot read Sparrow file");
    std::string bytes{std::istreambuf_iterator<char>(file), {}};
    require(!file.bad() && bytes.size() <= limit, "Cannot read complete Sparrow file");
    return bytes;
}
struct Bounds
{
    double x = INFINITY, y = INFINITY, right = -INFINITY, bottom = -INFINITY;
    void add(double px, double py)
    {
        x = std::min(x, px);
        y = std::min(y, py);
        right = std::max(right, px);
        bottom = std::max(bottom, py);
    }
    double width() const { return right - x; }
    double height() const { return bottom - y; }
};
std::pair<double, double> rotate(std::pair<double, double> p, double angle)
{
    if (angle == 0)
        return p;
    if (angle == 90)
        return {-p.second, p.first};
    if (angle == 180)
        return {-p.first, -p.second};
    if (angle == 270)
        return {p.second, -p.first};
    auto a = angle * std::numbers::pi / 180;
    return {p.first * std::cos(a) - p.second * std::sin(a), p.first * std::sin(a) + p.second * std::cos(a)};
}
Bounds bounds(Ring const &ring, double angle = 0)
{
    Bounds out;
    for (auto p : ring) {
        auto [x, y] = rotate(p, angle);
        out.add(x, y);
    }
    return out;
}
Ring ring(std::vector<Point> const &points)
{
    Ring out;
    for (auto p : points) {
        require(std::isfinite(p.x) && std::isfinite(p.y), "Nonfinite nesting geometry");
        if (out.empty() || out.back() != std::pair{p.x, p.y})
            out.emplace_back(p.x, p.y);
    }
    if (out.size() > 1 && out.front() == out.back())
        out.pop_back();
    require(out.size() >= 3 && out.size() <= 10000, "Unsupported Sparrow ring size");
    return out;
}
double area(Ring const &p)
{
    long double sum = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        auto a = p[i], b = p[(i + 1) % p.size()];
        sum += (static_cast<long double>(a.first) - p[0].first) * (b.second - p[0].second) -
               (static_cast<long double>(b.first) - p[0].first) * (a.second - p[0].second);
    }
    return std::abs(static_cast<double>(sum / 2));
}
double number(json::value const &value)
{
    require(value.is_number(), "Invalid Sparrow numeric field");
    auto n = value.to_number<double>();
    require(std::isfinite(n), "Nonfinite Sparrow result");
    return n;
}
double minimumWidth(Ring points, Clock::time_point deadline, std::stop_token stop)
{
    std::sort(points.begin(), points.end());
    auto cross = [](auto a, auto b, auto c) {
        return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
    };
    Ring hull;
    for (int half = 0; half < 2; ++half) {
        auto start = hull.size();
        for (auto p : points) {
            while (hull.size() >= start + 2 && cross(hull[hull.size() - 2], hull.back(), p) <= 0)
                hull.pop_back();
            hull.push_back(p);
        }
        hull.pop_back();
        std::reverse(points.begin(), points.end());
    }
    double best = INFINITY;
    for (std::size_t i = 0; i < hull.size(); ++i) {
        checkpoint(deadline, stop);
        auto a = hull[i], b = hull[(i + 1) % hull.size()];
        auto dx = b.first - a.first, dy = b.second - a.second, length = std::hypot(dx, dy);
        if (!length)
            continue;
        double lo = INFINITY, hi = -INFINITY;
        for (auto p : hull) {
            auto v = (-dy * p.first + dx * p.second) / length;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        best = std::min(best, hi - lo);
    }
    return best;
}
struct Group
{
    Ring points;
    std::vector<std::size_t> instances; // part indices, in selection order
    json::object item;                  // Sparrow item without id and demand
    double area = 0;
};
struct Prepared
{
    std::vector<Group> groups;
    std::vector<std::pair<double, double>> origins;
    std::vector<double> angles;
    bool free = false;
    Bounds sheet;
    double tolerance = 0;
    std::string preparation; // diagnostics for backend_detail
};
Prepared prepare(PreparedDocumentNesting const &snapshot, Options const &options, Clock::time_point deadline,
                 std::stop_token stop)
{
    Prepared p;
    p.sheet = bounds(ring(snapshot.container_outline));
    p.sheet.x += options.container_margin;
    p.sheet.y += options.container_margin;
    p.sheet.right -= options.container_margin;
    p.sheet.bottom -= options.container_margin;
    // Sparrow gets one proxy per shared collision envelope: the envelope the
    // validator itself builds (Job::collisionProxies), grown by `proxy_extra`
    // and simplified, verified to contain it. Proxies that do not overlap
    // therefore pass the portfolio's validation, and so does a sheet grown by
    // spacing / 2 per side (the validator's wall is margin - spacing / 2).
    // Envelopes computed any other way can bulge differently (they depend on
    // float rounding at the part's position), so a layout valid by exact
    // distance could still fail validation. Sparrow is not
    // asked for a separation, so it skips its own offset step, whose cost
    // grows super-linearly with the vertex count (measured 9-27 s at 3000
    // vertices). The demand filter, the output check and the final
    // translation below all work in this grown frame.
    // Float32 composition in the validator loses more far from the origin.
    double extent = 0;
    for (auto const &point : snapshot.container_outline)
        extent = std::max({extent, std::abs(point.x), std::abs(point.y)});
    auto const proxy_extra = std::max({snapshot.flatten_tolerance, 0.01, 1e-6 * extent});
    auto const grow = options.part_spacing / 2; // the validator's wall: margin - spacing / 2
    p.sheet.x -= grow;
    p.sheet.y -= grow;
    p.sheet.right += grow;
    p.sheet.bottom += grow;
    p.tolerance = std::max({p.sheet.width(), p.sheet.height(), options.part_spacing, options.container_margin, 1.0}) * 2e-7;
    p.free = options.rotation_mode == RotationMode::Free;
    if (!p.free) {
        double step = options.rotation_mode == RotationMode::None          ? 360
                      : options.rotation_mode == RotationMode::RightAngles ? 90
                                                                           : options.rotation_step_degrees;
        require(step > 0 && std::isfinite(step), "Invalid rotation step");
        for (double a = 0; a < 360 - 1e-9; a += step) {
            require(p.angles.size() < 3600, "Too many Sparrow orientations");
            p.angles.push_back(a);
        }
    }
    // The crate returns the envelopes its own validation will use, grouped
    // and centred exactly as it groups and centres them (one proxy per
    // shared envelope; each part's centre maps its document points into the
    // proxy's frame).
    // The proxies may take at most 40% of the remaining time, or Sparrow
    // could not search; past that the lane gives up and native nests alone.
    auto proxy_options = options;
    auto const remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    proxy_options.time_limit_ms = static_cast<std::uint64_t>(std::max<long long>(500, remaining_ms * 2 / 5));
    Job job(proxy_options);
    require(job.setContainer(snapshot.container_outline) == Status::Ok, "Sparrow proxy container rejected");
    for (auto const &part : snapshot.parts)
        require(job.addPart(part.id, part.components) == Status::Ok, "Sparrow proxy part rejected");
    std::optional<CollisionProxies> proxies;
    auto proxy_status = Status::Ok;
    auto const proxies_started = Clock::now();
    {
        std::stop_callback cancel_proxies(stop, [&job] { job.cancel(); });
        proxies = job.collisionProxies(proxy_extra, &proxy_status, 50000);
    }
    auto const proxy_ms = std::to_string(static_cast<int>(
        std::chrono::duration<double>(Clock::now() - proxies_started).count() * 1000));
    checkpoint(deadline, stop);
    if (!proxies) {
        require(!stop.stop_requested(), "Nesting cancelled");
        throw std::runtime_error(
            proxy_status == Status::Cancelled
                ? "Sparrow outline preparation ran out of its time budget (" + proxy_ms + " ms of " +
                      std::to_string(proxy_options.time_limit_ms) + " ms)"
            : proxy_status == Status::OutOfRange ? std::string{"Sparrow outlines exceed the vertex allowance"}
                                                 : std::string{"No verified Sparrow outline for a part: "} +
                                                       statusMessage(proxy_status));
    }
    require(proxies->part_group.size() == snapshot.parts.size() &&
                proxies->part_center.size() == snapshot.parts.size(),
            "No verified Sparrow outlines");
    for (auto const &center : proxies->part_center)
        p.origins.emplace_back(center.x, center.y);
    std::vector<std::size_t> group_of_type(proxies->rings.size(), SIZE_MAX); // SIZE_MAX: cannot fit alone
    std::size_t total = 0;
    for (std::size_t type = 0; type < proxies->rings.size(); ++type) {
        checkpoint(deadline, stop);
        auto proxy = ring(proxies->rings[type]);
        total += proxy.size();
        require(total <= 50000, "Sparrow vertex allowance exceeded");
        double const w = p.sheet.width(), h = p.sheet.height();
        require(area(proxy) > 0, "Empty Sparrow part");
        bool fits = w > 0 && h > 0 && area(proxy) <= w * h + p.tolerance * p.tolerance;
        if (fits && p.free)
            fits = minimumWidth(proxy, deadline, stop) <= std::min(w, h) + p.tolerance;
        else if (fits)
            fits = std::any_of(p.angles.begin(), p.angles.end(), [&](double a) {
                auto b = bounds(proxy, a);
                return b.width() <= w + p.tolerance && b.height() <= h + p.tolerance;
            });
        if (!fits)
            continue;
        json::array coordinates;
        for (auto [x, y] : proxy) {
            float fx = x, fy = y;
            require(std::isfinite(fx) && std::isfinite(fy) && std::abs(x - fx) <= p.tolerance &&
                        std::abs(y - fy) <= p.tolerance,
                    "Sparrow float32 geometry allowance exceeded");
            coordinates.push_back(json::array{fx, fy});
        }
        json::object item{{"shape", json::object{{"type", "simple_polygon"}, {"data", std::move(coordinates)}}}};
        if (!p.free) {
            json::array angles;
            for (auto a : p.angles)
                angles.push_back(static_cast<float>(a));
            item["allowed_orientations"] = std::move(angles);
        }
        auto const group_area = area(proxy);
        group_of_type[type] = p.groups.size();
        p.groups.push_back({std::move(proxy), {}, std::move(item), group_area});
    }
    p.preparation = "proxies: " + std::to_string(proxies->rings.size()) + " shapes, " + std::to_string(total) +
                    " vertices, " + proxy_ms + " ms";
    for (std::size_t index = 0; index < snapshot.parts.size(); ++index) {
        auto const type = proxies->part_group[index];
        require(type < group_of_type.size(), "Invalid Sparrow proxy group");
        if (group_of_type[type] != SIZE_MAX)
            p.groups[group_of_type[type]].instances.push_back(index);
    }
    return p;
}
struct TempDirectory
{
    std::filesystem::path path;
    TempDirectory()
    {
        auto raw = g_dir_make_tmp("vacards-sparrow-XXXXXX", nullptr);
        require(raw, "Cannot create Sparrow temporary directory");
        path = pathFromUtf8(raw);
        g_free(raw);
    }
    ~TempDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
/// Sparrow separator threads: the cores the native portfolio's two lanes
/// leave free, at most four (DeepnestML's default for the same helper).
unsigned sparrowWorkers()
{
    auto const cores = std::thread::hardware_concurrency();
    return std::clamp(cores > 3 ? cores - 2 : 1u, 1u, 4u);
}
#ifdef _WIN32
// All native resources are owned before any operation that can throw.
struct WindowsHandle
{
    HANDLE value = nullptr;
    explicit WindowsHandle(HANDLE value = nullptr) : value(value) {}
    WindowsHandle(WindowsHandle const &) = delete;
    WindowsHandle &operator=(WindowsHandle const &) = delete;
    ~WindowsHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring windowsUtf16(std::string const &text)
{
    auto const length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    require(length > 0 || text.empty(), "Invalid Sparrow UTF-8 argument");
    std::wstring result(length, L'\0');
    if (length)
        require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                   static_cast<int>(text.size()), result.data(), length) == length,
                "Cannot convert Sparrow argument");
    return result;
}
// Windows CRT quoting: double backslashes before quotes and the closing quote.
std::wstring windowsQuote(std::wstring const &arg)
{
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (auto ch : arg) {
        if (ch == L'\\') {
            ++slashes;
        } else {
            quoted.append(ch == L'"' ? 2 * slashes + 1 : slashes, L'\\');
            quoted += ch;
            slashes = 0;
        }
    }
    quoted.append(2 * slashes, L'\\');
    quoted += L'"';
    return quoted;
}
std::vector<wchar_t> windowsEnvironment(unsigned workers)
{
    auto raw = GetEnvironmentStringsW();
    require(raw, "Cannot read Sparrow environment");
    struct EnvironmentOwner {
        LPWCH value;
        ~EnvironmentOwner() { FreeEnvironmentStringsW(value); }
    } owner{raw};
    std::vector<std::wstring> entries;
    for (auto entry = raw; *entry; entry += wcslen(entry) + 1) {
        // Preserve all entries, including Windows' hidden =C: drive variables.
        std::wstring text(entry);
        auto const equals = text.find(L'=', text.front() == L'=' ? 1 : 0);
        if (_wcsicmp(text.substr(0, equals).c_str(), L"RAYON_NUM_THREADS") != 0)
            entries.push_back(std::move(text));
    }
    entries.push_back(L"RAYON_NUM_THREADS=" + std::to_wstring(workers));
    std::sort(entries.begin(), entries.end(), [](auto const &a, auto const &b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::vector<wchar_t> block;
    for (auto const &entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}
struct WindowsAttributes
{
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    WindowsAttributes()
    {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        require(bytes != 0, "Cannot size Sparrow handle list");
        storage.resize(bytes);
        auto candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        require(InitializeProcThreadAttributeList(candidate, 1, 0, &bytes),
                "Cannot initialize Sparrow handle list");
        list = candidate;
    }
    ~WindowsAttributes() { if (list) DeleteProcThreadAttributeList(list); }
};
void runWindows(std::filesystem::path const &dir, std::vector<std::string> const &args,
                Clock::time_point deadline, std::stop_token stop, unsigned workers)
{
    checkpoint(deadline, stop);
    require(!args.empty(), "Missing Sparrow executable");
    auto const executable = pathFromUtf8(args.front().c_str());
    require(executable.is_absolute(), "Sparrow executable must be an absolute path");
    std::wstring command;
    for (auto const &arg : args) {
        if (!command.empty()) command += L' ';
        command += windowsQuote(windowsUtf16(arg));
    }
    auto environment = windowsEnvironment(workers);
    WindowsHandle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(job.value && SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                                &limits, sizeof(limits)),
            "Cannot contain Sparrow process");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    WindowsHandle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &security, OPEN_EXISTING, 0, nullptr));
    WindowsHandle output(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, 0, nullptr));
    WindowsHandle error(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &security, OPEN_EXISTING, 0, nullptr));
    require(input.value != INVALID_HANDLE_VALUE && output.value != INVALID_HANDLE_VALUE &&
                error.value != INVALID_HANDLE_VALUE, "Cannot open Sparrow NUL streams");
    WindowsAttributes attributes;
    HANDLE streams[]{input.value, output.value, error.value};
    require(UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      streams, sizeof(streams), nullptr, nullptr),
            "Cannot restrict Sparrow inherited handles");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = output.value;
    startup.StartupInfo.hStdError = error.value;
    startup.lpAttributeList = attributes.list;
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                               EXTENDED_STARTUPINFO_PRESENT,
                           environment.data(), dir.c_str(), &startup.StartupInfo, &process),
            "Cannot start Sparrow");
    WindowsHandle child(process.hProcess), thread(process.hThread);
    // Containment is mandatory and precedes the first instruction in the child.
    bool const contained = AssignProcessToJobObject(job.value, child.value);
    bool expired = !contained;
    if (contained && ResumeThread(thread.value) == static_cast<DWORD>(-1))
        expired = true;
    DWORD wait = WAIT_TIMEOUT;
    while (!expired) {
        // Observe completion first, preserving an already-published result at the deadline.
        wait = WaitForSingleObject(child.value, 0);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT || stop.stop_requested() || Clock::now() >= deadline) {
            expired = true;
            break;
        }
        wait = WaitForSingleObject(child.value, 5);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT) expired = true;
    }
    if (expired) {
        TerminateJobObject(job.value, 1);
        // A failed assignment leaves the suspended child outside the job.
        // Also provides a direct-process fallback if job termination fails.
        TerminateProcess(child.value, 1);
        WaitForSingleObject(child.value, INFINITE); // always reap, including rejection
    }
    DWORD code = 1;
    bool const success = GetExitCodeProcess(child.value, &code) && code == 0;
    require(!expired, "Sparrow cancelled, timed out, or could not contain its process");
    require(success, "Sparrow failed");
}
#endif
void run(std::filesystem::path const &dir, std::vector<std::string> const &args, Clock::time_point deadline,
         std::stop_token stop, unsigned workers)
{
#ifdef _WIN32
    runWindows(dir, args, deadline, stop, workers);
#else
    std::vector<char const *> argv;
    for (auto const &arg : args)
        argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    checkpoint(deadline, stop);
    auto launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_set_stdout_file_path(launcher, utf8String(dir / "stdout.log").c_str());
    g_subprocess_launcher_set_stderr_file_path(launcher, utf8String(dir / "stderr.log").c_str());
    g_subprocess_launcher_set_cwd(launcher, utf8String(dir).c_str());
    g_subprocess_launcher_setenv(launcher, "RAYON_NUM_THREADS", std::to_string(workers).c_str(), true);
    g_subprocess_launcher_set_child_setup(
        launcher,
        [](void *) {
            if (setsid() < 0)
                _exit(127);
        },
        nullptr, nullptr);
    GError *error = nullptr;
    auto process = g_subprocess_launcher_spawnv(launcher, argv.data(), &error);
    g_object_unref(launcher);
    if (!process) {
        std::string message = error ? error->message : "Cannot start Sparrow";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }
    // GIO can already have reaped a short-lived helper, leaving no identifier.
    // Like the Windows branch, a missing identifier takes the normal cleanup
    // path (rejected as not contained) instead of crashing in std::stol.
    auto const identifier = g_subprocess_get_identifier(process);
    auto const parsed_pid = identifier ? g_ascii_strtoll(identifier, nullptr, 10) : 0;
    auto pid = static_cast<pid_t>(parsed_pid);
    bool contained = parsed_pid > 0;
    auto context = g_main_context_new();
    g_main_context_push_thread_default(context);
    bool done = false;
    g_subprocess_wait_async(
        process, nullptr,
        [](GObject *source, GAsyncResult *result, void *data) {
            GError *error = nullptr;
            g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error);
            g_clear_error(&error);
            *static_cast<bool *>(data) = true;
        },
        &done);
    // Failed containment is a rejection even if the wait callback wins the
    // race and observes a successful exit before the loop checks cancellation.
    bool expired = !contained;
    while (!done) {
        while (g_main_context_iteration(context, false)) {
        }
        if (!done && (!contained || stop.stop_requested() || Clock::now() >= deadline)) {
            expired = true;
            if (pid > 0) // never signal our own process group (pid 0)
                kill(-pid, SIGKILL);
            g_subprocess_force_exit(process);
            // Reap before returning; never leave a child or callback referring
            // to this stack behind after Cancel or a deadline.
            while (!done)
                g_main_context_iteration(context, true);
        }
        if (!done)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    bool success = g_subprocess_get_successful(process);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    g_object_unref(process);
    require(!expired, "Sparrow cancelled, timed out, or could not contain its process");
    require(success, "Sparrow failed");
#endif
    // A run that completed before the kill is kept, even if reading it back
    // crosses the deadline; only cancellation discards it.
    require(!stop.stop_requested(), "Nesting cancelled");
}
double bucket(double v)
{
    if (v == 0 || !std::isfinite(v))
        return v;
    auto bits = std::bit_cast<std::uint64_t>(v);
    bits = std::min(UINT64_C(0x7fefffffffffffff), (bits + (UINT64_C(1) << 21)) & ~((UINT64_C(1) << 22) - 1));
    return std::bit_cast<double>(bits);
}
auto rank(PreparedDocumentNesting const &snapshot, std::span<Placement const> placements)
{
    require(placements.size() == snapshot.parts.size(), "Incomplete candidate rank vector");
    Bounds occupied;
    std::size_t count = 0;
    std::vector<std::pair<std::uint64_t, double>> areas;
    for (std::size_t i = 0; i < placements.size(); ++i)
        if (placements[i].placed) {
            auto const &p = placements[i];
            ++count;
            require(p.part_id == snapshot.parts[i].id, "Candidate rank identity mismatch");
            double material = 0;
            for (auto const &c : snapshot.parts[i].components) {
                auto component_area = area(ring(c.outer));
                for (auto const &h : c.holes)
                    component_area -= area(ring(h));
                material += std::max(0.0, component_area);
                for (auto pt : c.outer) {
                    auto [x, y] = rotate({pt.x, pt.y}, p.rotation_degrees);
                    occupied.add(x + p.translation_x, y + p.translation_y);
                }
            }
            areas.emplace_back(p.part_id, material);
        }
    std::sort(areas.begin(), areas.end());
    double captured = 0, correction = 0;
    for (auto [id, value] : areas) {
        auto next = captured + value;
        correction += std::abs(captured) >= std::abs(value) ? (captured - next) + value : (value - next) + captured;
        captured = next;
    }
    return std::tuple{bucket(captured + correction), count,
                      count ? -bucket(occupied.width() * occupied.height()) : -INFINITY};
}
/// One Sparrow run over `demand[g]` copies of each group: the first copies of
/// each group in selection order. Only copies that land wholly on the sheet
/// are placed, and they are always a selection-order prefix of their group.
struct RunResult
{
    std::vector<Placement> placements;
    std::vector<std::size_t> placed; // per group
    std::size_t placed_count = 0;
    std::size_t demand_count = 0;
    double strip_width = 0;
};
/// `run_deadline` sets Sparrow's own clock; the process is only killed at
/// `deadline` (the job's), so a run that overruns its slice under load still
/// returns its layout.
RunResult runSparrow(PreparedDocumentNesting const &s, Options const &o, Prepared const &p,
                     std::vector<std::size_t> const &demand, Clock::time_point run_deadline,
                     Clock::time_point deadline, std::stop_token stop)
{
    RunResult r;
    r.placed.assign(p.groups.size(), 0);
    for (auto const &part : s.parts)
        r.placements.push_back({.part_id = part.id});
    json::array items;
    std::vector<std::size_t> item_group;
    for (std::size_t g = 0; g < p.groups.size(); ++g) {
        require(demand[g] <= p.groups[g].instances.size(), "Sparrow demand exceeds the selection");
        if (!demand[g])
            continue;
        auto item = p.groups[g].item;
        item["id"] = item_group.size();
        item["demand"] = demand[g];
        items.push_back(std::move(item));
        item_group.push_back(g);
        r.demand_count += demand[g];
    }
    if (items.empty())
        return r;
    TempDirectory temp;
    auto input = json::serialize(json::object{{"name", "vacards"},
                                              {"strip_height", static_cast<float>(p.sheet.height())},
                                              {"items", std::move(items)}});
    require(g_file_set_contents(utf8String(temp.path / "input.json").c_str(), input.data(), input.size(), nullptr),
            "Cannot write Sparrow input");
    auto remaining = std::chrono::duration<double>(run_deadline - Clock::now()).count();
#ifdef _WIN32
    // Windows process launch plus Sparrow's initial placement happen
    // outside its search clock. Leave room for both lanes to finish
    // and read the final file before our shared wall-clock deadline.
    auto reserve = std::max(1.5, std::min(3.0, remaining * .2));
#else
    // Sparrow can overrun its clock under load, and a run killed at the
    // deadline returns nothing; keep a wider reserve on longer runs.
    auto reserve = std::max(2.0, std::min(4.0, remaining * .15));
#endif
    int seconds = std::max(0, static_cast<int>(std::floor(remaining - reserve)));
    auto const workers = sparrowWorkers();
    std::vector<std::string> args{helperPath(), "-i", utf8String(temp.path / "input.json"), "-t",
                                  std::to_string(seconds), "-s", std::to_string(o.random_seed), "--workers",
                                  std::to_string(workers)};
    run(temp.path, args, deadline, stop, workers);
    auto output = json::parse(readFile(temp.path / "output/final_vacards.json", 64 * 1024 * 1024));
    auto const &solution = output.at("solution");
    r.strip_width = number(solution.at("strip_width"));
    require(r.strip_width > 0, "Invalid Sparrow strip");
    auto const &rows = solution.at("layout").at("placed_items").as_array();
    require(rows.size() <= r.demand_count, "Excess Sparrow placements");
    std::vector<std::size_t> returned(p.groups.size(), 0);
    for (auto const &row : rows) {
        require(!stop.stop_requested(), "Nesting cancelled");
        auto const &id = row.at("item_id");
        require(id.is_int64() || id.is_uint64(), "Invalid Sparrow type ID");
        auto type = id.to_number<std::uint64_t>();
        require(type < item_group.size(), "Unknown Sparrow type ID");
        auto const g = item_group[type];
        auto const &group = p.groups[g];
        require(returned[g]++ < demand[g], "Excess Sparrow copy");
        auto const &transform = row.at("transformation");
        auto angle = std::fmod(number(transform.at("rotation")), 360.0);
        if (angle < 0)
            angle += 360;
        auto const &translation = transform.at("translation").as_array();
        require(translation.size() == 2, "Invalid Sparrow translation");
        auto tx = number(translation[0]), ty = number(translation[1]);
        if (!p.free)
            require(std::any_of(p.angles.begin(), p.angles.end(),
                                [&](double a) { return std::abs(std::remainder(angle - a, 360)) <= 5e-5; }),
                    "Sparrow violated rotation policy");
        auto b = bounds(group.points, angle);
        auto const tol = p.tolerance;
        if (b.x + tx < -tol || b.y + ty < -tol || b.right + tx > p.sheet.width() + tol ||
            b.bottom + ty > p.sheet.height() + tol)
            continue;
        // Copies are interchangeable: give on-sheet rows the earliest copies.
        auto index = group.instances[r.placed[g]++];
        ++r.placed_count;
        auto [ox, oy] = rotate(p.origins[index], angle);
        r.placements[index] = {s.parts[index].id, p.sheet.x + tx - ox, p.sheet.y + ty - oy, angle, true};
    }
    for (std::size_t g = 0; g < p.groups.size(); ++g)
        require(returned[g] == demand[g], "Sparrow omitted demand");
    return r;
}
} // namespace
#ifdef _WIN32
namespace SparrowInternal {
void runWindowsForTest(std::filesystem::path const &dir, std::vector<std::string> const &args,
                       std::chrono::steady_clock::time_point deadline, std::stop_token stop, unsigned workers)
{
    runWindows(dir, args, deadline, stop, workers);
}
} // namespace SparrowInternal
#endif
bool sparrowAvailable()
{
    if (std::string_view(VACARDS_SPARROW_SHA256).empty())
        return false;
    try {
        auto bytes = readFile(pathFromUtf8(helperPath().c_str()), 32 * 1024 * 1024);
        auto hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<guchar const *>(bytes.data()),
                                                bytes.size());
        bool ok = hash && std::string_view(hash) == VACARDS_SPARROW_SHA256;
        g_free(hash);
        return ok;
    } catch (...) {
        return false;
    }
}
// Sparrow's search clock is floor(remaining - reserve) seconds with a reserve of
// at least 1.5 s, so below 3 s it would search for 0 s and only occupy a core.
constexpr std::uint64_t SPARROW_MIN_TIME_LIMIT_MS = 3000;

bool sparrowEligible(PreparedDocumentNesting const &s, Options const &o)
{
    if (!o.time_limit_ms || o.time_limit_ms < SPARROW_MIN_TIME_LIMIT_MS || o.time_limit_ms > 600000 ||
        s.parts.empty() || s.parts.size() > 2048 || !s.container_holes.empty() || !s.obstacles.empty() ||
        !std::isfinite(s.flatten_tolerance) || s.flatten_tolerance < 0)
        return false;
    try {
        auto r = ring(s.container_outline);
        if (r.size() != 4)
            return false;
        auto b = bounds(r);
        std::set<std::pair<double, double>> expected{{b.x, b.y}, {b.right, b.y}, {b.right, b.bottom}, {b.x, b.bottom}};
        if (std::set(r.begin(), r.end()) != expected || b.width() <= 0 || b.height() <= 0)
            return false;
        for (std::size_t i = 0; i < 4; ++i)
            if (r[i].first != r[(i + 1) % 4].first && r[i].second != r[(i + 1) % 4].second)
                return false;
        // Detail and copies are fine: prepare() sends Sparrow one simplified
        // proxy per distinct outline and caps the total proxy vertices. More
        // than the sheet holds is fine too: solveSparrow() fills the sheet.
        for (auto const &p : s.parts) {
            if (p.components.size() != 1 || p.components.front().outer.size() > 10000)
                return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}
SolveResult solveSparrow(PreparedDocumentNesting const &s, Options const &o, Clock::time_point deadline,
                         std::stop_token stop)
{
    auto started = Clock::now();
    try {
        require(sparrowEligible(s, o) && sparrowAvailable(), "Sparrow unavailable or unsupported");
        checkpoint(deadline, stop);
        auto p = prepare(s, o, deadline, stop);
        std::vector<Placement> best;
        for (auto const &part : s.parts)
            best.push_back({.part_id = part.id});
        auto done = [&](std::vector<Placement> placements) {
            // A finished run is kept even if the clock passed the deadline
            // while it was read back; only cancellation discards it.
            require(!stop.stop_requested(), "Nesting cancelled");
            return SolveResult{.status = Status::Ok,
                               .placements = std::move(placements),
                               .error = {},
                               .metrics = {.elapsed_seconds =
                                               std::chrono::duration<double>(Clock::now() - started).count()}};
        };
        if (p.groups.empty())
            return done(std::move(best));

        auto const usable = p.sheet.width() * p.sheet.height();
        auto demand_area = [&](std::vector<std::size_t> const &demand) {
            double total = 0;
            for (std::size_t g = 0; g < p.groups.size(); ++g)
                total += p.groups[g].area * static_cast<double>(demand[g]);
            return total;
        };
        // Copies in selection order.
        std::vector<std::pair<std::size_t, std::size_t>> order; // (part index, group)
        for (std::size_t g = 0; g < p.groups.size(); ++g)
            for (auto index : p.groups[g].instances)
                order.emplace_back(index, g);
        std::sort(order.begin(), order.end());
        // Adds copies in selection order until `count` are chosen, skipping
        // any copy whose area would exceed the sheet (a smaller later part may
        // still fit). Chosen copies stay a selection-order prefix per group.
        auto extend = [&](std::vector<std::size_t> demand, std::size_t count) {
            auto chosen = std::accumulate(demand.begin(), demand.end(), std::size_t{0});
            auto area = demand_area(demand);
            std::vector<std::size_t> seen(p.groups.size(), 0);
            std::vector<bool> closed(p.groups.size(), false); // a skipped copy ends its group's prefix
            for (auto [index, g] : order) {
                if (chosen >= count)
                    break;
                if (seen[g]++ < demand[g] || closed[g])
                    continue;
                if (area + p.groups[g].area > usable) {
                    closed[g] = true;
                    continue;
                }
                ++demand[g];
                ++chosen;
                area += p.groups[g].area;
            }
            return demand;
        };

        // Sparrow packs every copy it is given into the shortest strip. Ask it
        // for the copies whose area fits the sheet and keep those that land on
        // it (as DeepnestML's Sparrow mode does). When some do not, try one
        // more than the best while time remains: every run yields a valid
        // on-sheet layout, and the best one by the portfolio's ranking is kept.
        auto demand = extend(std::vector<std::size_t>(p.groups.size(), 0), order.size());
        auto const everything = std::accumulate(demand.begin(), demand.end(), std::size_t{0}) == order.size();
        std::size_t best_count = 0;
        std::vector<std::size_t> best_placed(p.groups.size(), 0);
        std::size_t failed_count = SIZE_MAX; // smallest count seen not to fit
        std::string trace;
        for (bool first = true;; first = false) {
            auto const remaining = std::chrono::duration<double>(deadline - Clock::now()).count();
            if (!first && remaining < SPARROW_MIN_TIME_LIMIT_MS / 1000.0)
                break;
            // The first run, cut at the sheet edge, is usually the best layout
            // (measured: 110 circles offered, 90 kept; shorter follow-up runs
            // kept fewer), so it gets most of the time; later runs try one more.
            // When everything may fit, it compacts with nearly all of it.
            auto const share = first ? (everything ? .85 : .75) : .5;
            auto const slice = std::min(remaining, std::max(SPARROW_MIN_TIME_LIMIT_MS / 1000.0, remaining * share));
            auto const run_deadline = std::min(deadline, Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                                            std::chrono::duration<double>(slice)));
            RunResult r;
            try {
                r = runSparrow(s, o, p, demand, run_deadline, deadline, stop);
            } catch (...) {
                if (first || stop.stop_requested())
                    throw;
                break; // keep the best layout found so far
            }
            trace += (trace.empty() ? "" : ", ") + std::to_string(r.demand_count) + ">" + std::to_string(r.placed_count);
            // Same ranking as the portfolio: placed material, then count.
            if (first || betterNestingCandidate(s, r.placements, best)) {
                best_count = r.placed_count;
                best_placed = r.placed;
                best = r.placements;
            }
            if (r.placed_count == r.demand_count && everything && first)
                break; // everything fits
            if (r.placed_count < r.demand_count)
                failed_count = std::min(failed_count, r.demand_count);
            // Next count: one more than the best, unless that already failed.
            auto const target = best_count + 1;
            if (failed_count <= target)
                break;
            demand = extend(best_placed, target);
            if (std::accumulate(demand.begin(), demand.end(), std::size_t{0}) != target)
                break; // no further copy fits by area
        }
        auto result = done(std::move(best));
        result.backend_detail = p.preparation + "; Sparrow runs (offered>on sheet): " + trace;
        return result;
    } catch (std::exception const &e) {
        return {.status = stop.stop_requested() ? Status::Cancelled : Status::SolverUnavailable, .error = e.what()};
    }
}
bool betterNestingCandidate(PreparedDocumentNesting const &s, std::span<Placement const> candidate,
                            std::span<Placement const> incumbent)
{
    return rank(s, candidate) > rank(s, incumbent);
}
double capturedNestingArea(PreparedDocumentNesting const &s, std::span<Placement const> placements)
{
    return std::get<0>(rank(s, placements));
}
} // namespace Inkscape::Nesting
