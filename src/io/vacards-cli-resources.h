// SPDX-License-Identifier: GPL-2.0-or-later
// Frozen grant interface, shared with the agent session.
#ifndef INKSCAPE_IO_VACARDS_CLI_RESOURCES_H
#define INKSCAPE_IO_VACARDS_CLI_RESOURCES_H
#include <string>
#include <memory>
#include <cstdint>
#include <vector>
namespace Inkscape::VACardsCli {
struct Grants { std::vector<std::string> read_roots, read_files, write_roots, write_files; };
#ifdef _WIN32
bool allowed_windows_reparse_tag(std::uint32_t tag);
bool safe_windows_file_handle(void *handle);
std::shared_ptr<void> retain_windows_path(std::string const &path);
#endif
struct AdmittedFile;
struct ResourceAccess {
    std::string state; // granted, ungranted, remote, missing
    std::string path;  // canonical local path, never a network URI
    std::shared_ptr<AdmittedFile> admitted;
};
/// Classifies references and retains admission handles; never reads payload bytes.
ResourceAccess inspect_resource(std::string const &href, std::string const &base, Grants const &grants);
struct AdmittedBytes { std::string bytes, sha256, identity, error; std::uint64_t found=0; };
AdmittedBytes read_admitted(ResourceAccess const &, std::uint64_t limit);
ResourceAccess inspect_command_path(std::string const &, Grants const &);
ResourceAccess inspect_write_destination(std::string const &absolute_path, Grants const &);
} // namespace Inkscape::VACardsCli
#endif
