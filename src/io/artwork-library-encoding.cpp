// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-encoding.h"

#include <stdexcept>

namespace Inkscape::IO::ArtworkLibrary {

Bytes encode_catalog(CatalogSnapshot snapshot, ArchiveLimits archive_limits,
                     ManifestLimits manifest_limits, Cancelled cancelled)
{
    auto manifest = snapshot.manifest(cancelled);
    auto json = manifest.serialize(manifest_limits, cancelled);
    auto header = std::make_shared<Bytes const>(json.begin(), json.end());
    std::vector<ArchiveWriteEntry> entries;
    entries.reserve(manifest.assets.size() + 1);
    entries.push_back({"library.json", header->size(), [header] { return header; }});
    for (auto const &asset : manifest.assets) {
        if (cancelled && cancelled()) throw std::runtime_error("Artwork library operation cancelled");
        entries.push_back({asset.path, snapshot.artwork_size(asset.id),
            [snapshot, id = asset.id, hash = asset.sha256, cancelled] {
                auto bytes = snapshot.read_artwork(id, cancelled);
                if (artwork_sha256(*bytes, cancelled) != hash) {
                    throw std::runtime_error("Artwork library hash mismatch while encoding " + id);
                }
                return bytes;
            }});
    }
    return Archive::write_streamed(std::move(entries), archive_limits, std::move(cancelled));
}

} // namespace Inkscape::IO::ArtworkLibrary
