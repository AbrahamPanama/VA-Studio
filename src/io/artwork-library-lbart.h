// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_LBART_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_LBART_H

#include "artwork-library-archive.h"
#include <stdexcept>

namespace Inkscape::IO::ArtworkLibrary {

struct LbartLimits {
    std::size_t file_bytes = 512u * 1024 * 1024;
    std::size_t entries = 10000;
    std::size_t artwork_bytes = 32u * 1024 * 1024;
    std::size_t total_artwork_bytes = 1024u * 1024 * 1024;
    std::size_t preview_bytes = 8u * 1024 * 1024;
};

class LbartError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct LbartEntry {
    std::string name;
    // Raw container metadata; not yet qualified as SVG millimeters or bounds.
    double extent_x, extent_y;
    std::size_t artwork_bytes, preview_bytes;
};

// Read-only adapter for the observed big-endian fixed-136-byte-directory
// variant. It does not claim support for every LightBurn container version.
// Uses indexed boundaries, never PNG/zlib signature scanning. No disk access,
// GTK, native document parsing, rendering or active-document mutation.
class LbartArchive final {
public:
    static LbartArchive open(Bytes bytes, LbartLimits limits = {}, Cancelled cancelled = {});
    std::vector<LbartEntry> entries() const;
    // Returns untrusted LightBurnShapes XML bytes, NOT validated/editable SVG.
    // Decompression success is not conversion or safe-rendering certification.
    Bytes read_artwork(std::size_t index, Cancelled cancelled = {}) const;
    // Disposable encoded bytes only; a bounded image decoder must validate them
    // before presentation. They must never substitute for editable artwork.
    Bytes read_preview(std::size_t index, Cancelled cancelled = {}) const;

private:
    struct Record {
        LbartEntry metadata;
        std::size_t preview_offset, artwork_offset, compressed_bytes;
    };
    std::shared_ptr<Bytes const> _bytes;
    std::vector<Record> _records;
};

} // namespace Inkscape::IO::ArtworkLibrary
#endif
