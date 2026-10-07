// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_EXPORT_DESTINATION_H
#define INKSCAPE_IO_EXPORT_DESTINATION_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Inkscape::IO::ExportDestination {

namespace detail {
enum class QueryOutcome { Usable, Unusable, Pending };
using FolderQuery = std::function<void(std::string const &, std::function<void(QueryOutcome)>)>;
// Test seam for bounded asynchronous folder admission. Empty query restores GIO.
void set_folder_query_for_testing(FolderQuery query);
void set_folder_query_timeout_for_testing(unsigned milliseconds);
}

// Arguments come from Extension::Output, never a translated label or typed filename.
std::string format_key(std::string const &mime, std::string extension);
std::string preference_key(std::string const &format);
std::string basename(std::string const &source, std::string const &fallback);
std::vector<std::string> directories(std::string const &remembered,
                                     std::string const &source, std::string const &fallback);
using Remember = std::function<void(std::string const &key, std::string const &native_directory)>;
void record_result(std::string const &format, std::string const &native_path,
                   bool successful, bool cancelled, Remember const &remember);

// Native filesystem encoding throughout; no filesystem access or document mutation.
struct Session {
    std::string folder, name, extension;
    bool user_folder = false, user_name = false;
    void refresh(std::string const &source, std::string const &fallback, std::string const &suffix);
    void edit(std::string const &path, std::string const &suffix, bool chooser = false);
    std::string path() const;
};

// Main-context-only owner. Destruction cancels delivery and drops the UI callback.
// Each candidate has a bounded UI wait, NOT a guarantee that the filesystem's
// worker/system call can be interrupted. No detached threads, mkdir, or sync stat.
class DirectoryRequest {
public:
    using Callback = std::function<void(std::string)>;
    DirectoryRequest(std::vector<std::string> candidates, Callback callback);
    ~DirectoryRequest();
    DirectoryRequest(DirectoryRequest const &) = delete;
    DirectoryRequest &operator=(DirectoryRequest const &) = delete;
private:
    struct State;
    std::shared_ptr<State> _state;
};

} // namespace Inkscape::IO::ExportDestination
#endif
