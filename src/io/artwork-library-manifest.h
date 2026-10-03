// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_MANIFEST_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_MANIFEST_H

#include "artwork-library-archive.h"
#include <string_view>

namespace Inkscape::IO::ArtworkLibrary {

struct ManifestLimits {
    std::size_t bytes = 16u * 1024 * 1024;
    std::size_t assets = 10000;
};

struct Asset {
    std::string id;
    std::string name;
    std::vector<std::string> tags;
    std::string path;
    std::string sha256;
    double width_mm = 0;
    double height_mm = 0;
    // Unknown optional metadata is round-tripped, never interpreted as code,
    // paths, dependencies or permission to fetch external resources.
    std::string extra_json = "{}";
};

struct Manifest {
    std::string id;
    std::string name;
    std::uint64_t revision = 0;
    std::vector<Asset> assets;
    std::string extra_json = "{}";

    static Manifest parse(std::string_view json, ManifestLimits limits = {}, Cancelled cancelled = {});
    std::string serialize(ManifestLimits limits = {}, Cancelled cancelled = {}) const;
};

bool valid_library_uuid(std::string_view id) noexcept;

} // namespace Inkscape::IO::ArtworkLibrary
#endif
