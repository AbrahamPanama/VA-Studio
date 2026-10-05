// SPDX-License-Identifier: GPL-2.0-or-later
#include "config.h"
#include "vacards-cli-entry.h"
#include "vacards-cli-production.h"
#include "vacards-cli-mcp.h"
#include "async/bitmap-job-reaper.h"
#include "vacards-cli-session.h"
#include "io/vacards-cli-files.h"
#include "inkscape-version.h"
#include "inkscape.h"
#include "inkgc/gc-core.h"
#include "util/statics.h"
#ifdef __APPLE__
#include "io/macos-bundle-bootstrap.h"
#endif
#include <giomm/init.h>
#include <glibmm/init.h>
#include <glib/gstdio.h>
#include <gsl/gsl_errno.h>
#include <filesystem>
#include <fstream>
#include <csignal>
#include <cstdio>
#include <cerrno>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define write _write
#else
#include <unistd.h>
#endif
namespace Inkscape::VACardsCli {
using namespace boost::json;
object cli_identity() {
    array formats;
    for (auto const &f : inspection_formats()) formats.emplace_back(f);
    auto files = file_capabilities();
    // Slice acceptance is a release qualification decision, never inferred from registration.
    // This integration remains M1-advertised until the full Mac/Windows matrix is accepted.
    bool editable = false;
    if (auto v = files.if_contains("editable_documents"); v && v->is_bool()) editable = v->as_bool();
    else if (auto formats = files.if_contains("open"); formats && formats->is_array())
        editable = !formats->as_array().empty() && find_command("file.open") != nullptr;
    return {{"product", VACARDS_PRODUCT_NAME}, {"build", VACARDS_PRODUCT_VERSION}, {"source_sha", Inkscape::revision_full_string},
        {"protocol", "va-studio.cli-session/1"},
        {"protocol_version", "va-studio.cli-session/1"},
        {"protocol_versions", array{"va-studio.cli-session/1", "va-studio.cli-request/1", "va-studio.cli-result/1"}},
        {"catalog_version", catalog_version}, {"catalog_hash", production_catalog().at("hash")},
        {"enabled_slices", array{"M1"}}, {"inspection_formats", formats},
        {"capabilities", object{{"enabled_slices", array{"M1"}}, {"inspection", true}, {"editable_documents", editable},
            {"files", files}, {"m2_accepted", false},
            {"m3_accepted", m3_accepted}, {"m3_experimental", true}, {"tokens", true}, {"mcp", true},
            {"mcp_experimental", true}, {"mcp_protocol_version", "2025-06-18"},
            {"cancellation", "before-handler"}, {"native_calls_interruptible", false}}}};
}
namespace {
bool absolute(char const *path) {
    return g_utf8_validate(path, -1, nullptr) && g_path_is_absolute(path);
}
// A regular file, deliberately not a directory: Preferences::_load cannot create
// preferences.xml or any user subdirectory here. Keep it alive through all native
// destructors. No caller profile path is consulted or written by this process.
struct PrivateProfile {
    char *directory = nullptr;
    std::string sentinel;
    PrivateProfile() {
        directory = g_dir_make_tmp("vastudio-cli-runtime-XXXXXX", nullptr);
        if (!directory) throw std::runtime_error("Cannot establish private runtime directory.");
        sentinel = std::string(directory) + "/profile-disabled";
        if (!g_file_set_contents(sentinel.c_str(), "", 0, nullptr)) {
            g_rmdir(directory); g_free(directory); directory = nullptr;
            throw std::runtime_error("Cannot establish private profile.");
        }
        g_setenv("INKSCAPE_PROFILE_DIR", sentinel.c_str(), true);
    }
    void bootstrap_resources() {
#ifdef __APPLE__
        // Reuse the bundle's resource setup, with its loader cache in our private
        // temporary directory. Switch back before any native preferences lookup.
        g_setenv("INKSCAPE_PROFILE_DIR", directory, true);
        bool ok = Inkscape::IO::init_macos_bundle_resources();
        g_setenv("INKSCAPE_PROFILE_DIR", sentinel.c_str(), true);
        if (!ok) throw std::runtime_error("Cannot initialize bundled CLI resources.");
#endif
    }
    ~PrivateProfile() {
        if (directory) {
            g_unlink(sentinel.c_str());
            g_unlink((std::string(directory) + "/gdk-pixbuf-loaders.cache").c_str());
            g_rmdir(directory); g_free(directory);
        }
    }
};
}
int agent_main(int argc, char **argv, std::function<void()> diagnostic_probe) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    // Only the saved descriptor carries the protocol. printf, iostream, GLib,
    // GTK and native-library diagnostics all inherit stderr as fd 1.
    fflush(stdout);
    int protocol = dup(1);
    if (protocol < 0 || dup2(2, 1) < 0) { if (protocol >= 0) close(protocol); return 2; }
    struct Close { int fd; ~Close() { close(fd); } } closer{protocol};
    auto output = [protocol](std::string_view bytes) {
        while (!bytes.empty()) {
            auto n = ::write(protocol, bytes.data(), static_cast<unsigned>(bytes.size()));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            bytes.remove_prefix(n);
        }
        return true;
    };
    // Install once, including when native startup has already created threads.
    // The default writer's set_use_stderr switch has a thread precondition.
    static bool const logging_installed = [] {
        g_log_set_writer_func([](GLogLevelFlags level, GLogField const *fields,
                                 gsize count, void *) -> GLogWriterOutput {
            auto formatted = g_log_writer_format_fields(level, fields, count, false);
            fprintf(stderr, "%s\n", formatted ? formatted : "GLib diagnostic");
            g_free(formatted);
            return G_LOG_WRITER_HANDLED;
        }, nullptr, nullptr);
        g_log_set_default_handler([](char const *domain, GLogLevelFlags, char const *message, void *) {
            fprintf(stderr, "%s: %s\n", domain ? domain : "GLib", message ? message : "");
        }, nullptr);
        return true;
    }();
    (void)logging_installed;
    auto launch_error = [](std::string message) {
        object out{{"schema", "va-studio.cli-result/1"}, {"status", "rejected"},
            {"error", cli_error("invalid-launch", message,
                "Use --agent-session, --mcp-stdio, --agent-request-file ABS_PATH, or --version --json.")}};
        auto line = serialize(out) + "\n"; fputs(line.c_str(), stderr); return 2;
    };
    try {
        if (diagnostic_probe) diagnostic_probe();
        if (argc == 3 && std::string_view(argv[1]) == "--version" && std::string_view(argv[2]) == "--json")
            return output(serialize(cli_identity()) + "\n") ? 0 : 2;
        if (argc < 2) return launch_error("An agent mode is required.");
        std::string mode(argv[1]), file;
        SessionOptions options;
        int i = 2;
        if (mode == "--agent-request-file") {
            if (i == argc || !absolute(argv[i])) return launch_error("Request file must be an absolute path.");
            file = argv[i++];
        } else if (mode != "--agent-session" && mode != "--mcp-stdio") return launch_error("Unsupported CLI mode.");
        while (i < argc) {
            std::string flag(argv[i++]);
            if (i == argc || !absolute(argv[i])) return launch_error("Options need absolute paths.");
            if (flag == "--grant-read" || flag == "--grant-write") {
                std::error_code ec;
                if (!std::filesystem::is_directory(std::filesystem::u8path(argv[i]), ec))
                    return launch_error("Root grants must name existing absolute directories.");
                auto &roots = flag == "--grant-read" ? options.grants.read_roots : options.grants.write_roots;
                roots.emplace_back(argv[i++]);
            } else if (flag == "--grant-read-file" || flag == "--grant-write-file") {
                auto path = std::filesystem::u8path(argv[i]);
                std::error_code ec;
                if (flag == "--grant-read-file" && !std::filesystem::is_regular_file(path, ec))
                    return launch_error("Read file grants must name existing absolute regular files.");
                if (flag == "--grant-write-file" && !std::filesystem::is_directory(path.parent_path(), ec))
                    return launch_error("Write file grants require an existing parent directory.");
                auto &files = flag == "--grant-read-file" ? options.grants.read_files : options.grants.write_files;
                files.emplace_back(argv[i++]);
            } else if (flag == "--inspect-file" || (flag == "--document" && mode == "--agent-request-file")) {
                if (!options.inspection_path.empty() || !options.document_path.empty())
                    return launch_error("Only one startup document option is allowed.");
                auto &path = flag == "--inspect-file" ? options.inspection_path : options.document_path;
                path = argv[i++];
            } else return launch_error("Unsupported agent option.");
        }
        std::string bytes;
        if (!file.empty()) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(std::filesystem::u8path(file), ec))
                return launch_error("Request file must be a readable regular file.");
            std::ifstream input(std::filesystem::u8path(file), std::ios::binary);
            if (!input) return launch_error("Cannot read the request file.");
            bytes.resize(request_limit + 1);
            input.read(bytes.data(), bytes.size()); bytes.resize(input.gcount());
            if (input.bad()) return launch_error("Request file read failed.");
        }
        PrivateProfile profile;
        profile.bootstrap_resources();
        Inkscape::Util::Statics statics;
        Glib::init(); Gio::init(); Inkscape::GC::init();
        gsl_set_error_handler_off();
        // No Gio/Gtk application is constructed or registered. PreviewHelper
        // disables GUI and emergency/autosave recovery in the native core.
        // An initially empty session can create/open/import later. Initialize the
        // headless native core once, under the private profile; never register a GUI app.
        if (mode != "--mcp-stdio") Inkscape::Bitmap::recordBitmapMainThread();
        Inkscape::Application::create(false, Inkscape::Application::RuntimePolicy::PreviewHelper);
        if (mode == "--mcp-stdio") {
            options.on_native_thread_start = [] { Inkscape::Bitmap::recordBitmapMainThread(); };
            BoundedFdWriter writer(protocol);
            StructuredSession *session = nullptr;
            auto factory = [&](SessionOptions o, EngineEventSink s) {
                auto r = make_structured_session(std::move(o), std::move(s));
                session = r.engine.get();
                return r;
            };
            auto pump = [&] { if (session) session->pump(); };
            auto result = run_mcp_stdio_with_factory(descriptor_reader(0),
                [&](std::string_view line) { return writer.push_line(line); }, std::move(options), factory, pump);
            writer.close_and_flush(std::chrono::seconds(10));
            return result;
        }
        return mode == "--agent-session" ? run_agent_session(descriptor_reader(0), output, std::move(options))
                                        : run_agent_request(bytes, output, std::move(options));
    } catch (std::exception const &e) {
        object out{{"schema", "va-studio.cli-result/1"}, {"status", "failed"},
            {"error", cli_error("internal-error", e.what(), "Restart the CLI session.")}};
        auto line = serialize(out) + "\n"; fputs(line.c_str(), stderr); return 4;
    }
}
}
