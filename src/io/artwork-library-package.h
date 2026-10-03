// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_PACKAGE_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_PACKAGE_H

#include "artwork-library-manifest.h"
#include <optional>

namespace Inkscape::IO::ArtworkLibrary {

// Immutable, UI-independent native collection snapshot. Opening indexes the
// ZIP and parses library.json, but does not inflate artwork or preview images.
// Integrity is not content safety: returned SVG/PNG bytes still require the
// document importer's resource/geometry validation or a bounded image decoder.
class Package final {
public:
    static Package open(Bytes bytes, ArchiveLimits archive_limits = {},
                        ManifestLimits manifest_limits = {}, Cancelled cancelled = {});
    // Batch-reader admission: called once with all declared entry bytes after
    // ZIP metadata validation and BEFORE any entry/manifest decompression.
    // Throw to refuse. A caller's budget debit survives later parse/hash failure.
    static Package open_with_admission(Bytes bytes, std::function<void(std::size_t)> admit,
                        ArchiveLimits archive_limits = {}, ManifestLimits manifest_limits = {},
                        Cancelled cancelled = {});

    Manifest const &manifest() const { return _manifest; }
    Asset const &asset(std::string const &id) const;
    std::size_t artwork_size(std::string const &id) const;
    std::optional<std::size_t> preview_size(std::string const &id) const;
    // Declared uncompressed bytes of all indexed entries, without decoding.
    std::size_t declared_bytes() const;
    Bytes read_artwork(std::string const &id, Cancelled cancelled = {}) const;
    std::optional<Bytes> read_preview(std::string const &id, Cancelled cancelled = {}) const;

    // Verify every authoritative artwork before a later storage service may
    // publish a package. Holds only one decoded entry at a time. Preview caches
    // are not authoritative and are not used to establish artwork integrity.
    void verify_artworks(Cancelled cancelled = {}) const;

private:
    Package(Archive archive, Manifest manifest);
    Archive _archive;
    Manifest _manifest;
    std::map<std::string, std::size_t> _asset_indices;
};

std::string artwork_sha256(Bytes const &bytes, Cancelled cancelled = {});

} // namespace Inkscape::IO::ArtworkLibrary
#endif
