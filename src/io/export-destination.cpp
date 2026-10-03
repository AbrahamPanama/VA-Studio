// SPDX-License-Identifier: GPL-2.0-or-later
#include "export-destination.h"

#include <algorithm>
#include <gio/gio.h>
#include <glibmm/miscutils.h>
#include <utility>

namespace Inkscape::IO::ExportDestination {
namespace {
// Admission is on the owning main context. Cancelled requests retain only pure
// data until GIO delivers completion; rapid format switches cannot queue an
// unlimited number of app-owned pending probes. This does not promise that an
// OS filesystem operation stops when GIO acknowledges cancellation.
unsigned pending_queries = 0;
constexpr unsigned max_pending_queries = 4;
std::string lower(std::string text)
{
    for (auto &c : text) c = g_ascii_tolower(c);
    return text;
}
std::string without_suffix(std::string name, std::string const &suffix)
{
    auto extension = lower(suffix);
    if (extension == ".tiff" || extension == ".tif") {
        for (auto const *alias : {".tiff", ".tif"}) {
            auto length = std::char_traits<char>::length(alias);
            if (name.size() >= length && lower(name.substr(name.size() - length)) == alias) {
                name.resize(name.size() - length);
                return name;
            }
        }
    }
    if (!suffix.empty() && name.size() >= suffix.size() &&
        lower(name.substr(name.size() - suffix.size())) == lower(suffix)) {
        name.resize(name.size() - suffix.size());
    }
    return name;
}
}

std::string format_key(std::string const &mime, std::string extension)
{
    extension = lower(extension);
    if (!extension.empty() && extension.front() == '.') extension.erase(0, 1);
    if (extension == "tif" || extension == "tiff" || lower(mime) == "image/tiff") return "tiff";
    if (extension == "svg" || extension == "svgz" || lower(mime) == "image/svg+xml") return "svg";
    // Keep PS/EPS and other genuinely different registry formats separate even
    // when they share a MIME type. Hex encoding avoids preference path injection.
    auto const identity = extension.empty() ? lower(mime) : extension;
    std::string key = "format-";
    for (unsigned char c : identity) {
        key += "0123456789abcdef"[c >> 4];
        key += "0123456789abcdef"[c & 15];
    }
    return key;
}

std::string preference_key(std::string const &format)
{
    return "/dialogs/export/destinations/" + format + "/directory";
}

std::string basename(std::string const &source, std::string const &fallback)
{
    if (source.empty()) return fallback;
    auto name = Glib::path_get_basename(source);
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) name.resize(dot);
    return name.empty() ? fallback : name;
}

std::vector<std::string> directories(std::string const &remembered,
                                     std::string const &source, std::string const &fallback)
{
    std::vector<std::string> result;
    for (auto const &path : {remembered, source.empty() ? std::string() : Glib::path_get_dirname(source), fallback}) {
        if (!path.empty() && Glib::path_is_absolute(path) &&
            std::find(result.begin(), result.end(), path) == result.end()) result.push_back(path);
    }
    return result;
}

void record_result(std::string const &format, std::string const &native_path,
                   bool successful, bool cancelled, Remember const &remember)
{
    if (successful && !cancelled && !format.empty() && !native_path.empty() &&
        Glib::path_is_absolute(native_path)) {
        remember(preference_key(format), Glib::path_get_dirname(native_path));
    }
}

void Session::refresh(std::string const &source, std::string const &fallback, std::string const &suffix)
{
    if (!user_name) name = basename(source, fallback);
    extension = suffix;
}

void Session::edit(std::string const &path, std::string const &suffix, bool chooser)
{
    auto next_folder = path.empty() ? folder : Glib::path_get_dirname(path);
    auto next_name = path.empty() ? std::string() : without_suffix(Glib::path_get_basename(path), suffix);
    user_folder = user_folder || chooser || next_folder != folder;
    user_name = user_name || chooser || next_name != name;
    folder = std::move(next_folder);
    name = std::move(next_name);
    extension = suffix;
}

std::string Session::path() const
{
    return folder.empty() ? name + extension : Glib::build_filename(folder, name + extension);
}

struct DirectoryRequest::State : std::enable_shared_from_this<State> {
    std::vector<std::string> candidates;
    Callback callback;
    std::size_t index = 0;
    unsigned generation = 0;
    guint timer = 0;
    GCancellable *cancel = nullptr;

    void stop_query()
    {
        if (timer) { g_source_remove(timer); timer = 0; }
        if (cancel) { g_cancellable_cancel(cancel); g_object_unref(cancel); cancel = nullptr; }
    }
    void stop() { callback = {}; ++generation; stop_query(); }
    ~State() { stop_query(); }

    struct Attempt { std::shared_ptr<State> state; unsigned generation; };
    void next()
    {
        stop_query();
        ++generation;
        if (!callback) return;
        if (index == candidates.size() || pending_queries >= max_pending_queries) {
            auto done = std::move(callback);
            done({});
            return;
        }
        auto file = g_file_new_for_path(candidates[index++].c_str());
        cancel = g_cancellable_new();
        timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 750, [](gpointer data) -> gboolean {
            auto attempt = *static_cast<Attempt *>(data);
            if (attempt.generation == attempt.state->generation) {
                attempt.state->timer = 0;
                attempt.state->next();
            }
            return G_SOURCE_REMOVE;
        }, new Attempt{shared_from_this(), generation}, [](gpointer data) { delete static_cast<Attempt *>(data); });
        ++pending_queries;
        g_file_query_info_async(file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
            G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, cancel,
            [](GObject *file, GAsyncResult *result, gpointer data) {
                --pending_queries;
                std::unique_ptr<Attempt> attempt(static_cast<Attempt *>(data));
                GError *error = nullptr;
                auto info = g_file_query_info_finish(G_FILE(file), result, &error);
                bool usable = info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY &&
                    g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) &&
                    g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
                if (info) g_object_unref(info);
                g_clear_error(&error);
                auto state = attempt->state;
                if (attempt->generation != state->generation || !state->callback) return;
                if (!usable) { state->next(); return; }
                state->stop_query();
                auto done = std::move(state->callback);
                done(state->candidates[state->index - 1]);
            }, new Attempt{shared_from_this(), generation});
        g_object_unref(file);
    }
};

DirectoryRequest::DirectoryRequest(std::vector<std::string> candidates, Callback callback)
    : _state(std::make_shared<State>())
{
    _state->candidates = std::move(candidates);
    _state->callback = std::move(callback);
    _state->next();
}
DirectoryRequest::~DirectoryRequest() { _state->stop(); }

} // namespace Inkscape::IO::ExportDestination
