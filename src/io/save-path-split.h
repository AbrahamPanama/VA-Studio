// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_SAVE_PATH_SPLIT_H
#define INKSCAPE_IO_SAVE_PATH_SPLIT_H

#include <optional>
#include <string>
#include <utility>

namespace Inkscape::IO {

struct SavePathParts {
    std::string parent;
    std::string name;
};

// Preserve the caller's path spelling. In particular, a drive or UNC share
// root needs its trailing separator to remain an absolute parent directory.
inline std::optional<SavePathParts> split_save_path(std::string const &path)
{
#ifdef _WIN32
    auto const slash = path.find_last_of("/\\");
#else
    auto const slash = path.find_last_of('/');
#endif
    if (slash == std::string::npos || slash + 1 == path.size()) return std::nullopt;
    std::string parent = path.substr(0, slash);
    if (slash == 0) {
        parent = path.substr(0, slash + 1);
    }
#ifdef _WIN32
    else if ((slash == 2 && path[1] == ':') ||
        (path.size() >= 2 && (path[0] == '\\' || path[0] == '/') && path[1] == path[0] &&
         parent.find_first_of("/\\", 2) != std::string::npos &&
         parent.find_first_of("/\\", parent.find_first_of("/\\", 2) + 1) == std::string::npos)) {
        parent = path.substr(0, slash + 1);
    }
#endif
    return SavePathParts{std::move(parent), path.substr(slash + 1)};
}

} // namespace Inkscape::IO

#endif
