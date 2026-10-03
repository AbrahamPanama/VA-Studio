// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_ARCHIVE_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_ARCHIVE_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Inkscape::IO::ArtworkLibrary {

using Bytes = std::vector<unsigned char>;
using Cancelled = std::function<bool()>;

// Limits are checked before allocation/inflation. No filesystem extraction,
// GTK, SPDocument, or process-global parser state is involved.
struct ArchiveLimits {
    std::size_t package_bytes = 512u * 1024 * 1024;
    std::size_t entry_bytes = 32u * 1024 * 1024;
    std::size_t total_bytes = 1024u * 1024 * 1024;
    std::size_t entries = 20001; // manifest + 10,000 assets and thumbnails
};

// Metadata is checked for the whole archive before any payload reader runs.
// Readers must return immutable bytes of exactly the declared size. The writer
// releases each handle before asking for the next entry; a reader may share an
// existing immutable buffer instead of making another copy.
struct ArchiveWriteEntry {
    std::string path;
    std::size_t size;
    std::function<std::shared_ptr<Bytes const>()> read;
};

class Archive final {
public:
    // The caller transfers ownership: metadata is indexed without inflating
    // assets/previews. Copies share an immutable package snapshot.
    static Archive open(Bytes bytes, ArchiveLimits limits = {}, Cancelled cancelled = {});
    bool contains(std::string const &path) const;
    std::size_t size(std::string const &path) const;
    std::vector<std::string> paths() const;
    Bytes read(std::string const &path, Cancelled cancelled = {}) const;

    // Store-only ZIP writer, deliberately independent of file publication.
    // The catalog service must validate and atomically publish the result.
    static Bytes write(std::map<std::string, Bytes> const &files,
                       ArchiveLimits limits = {}, Cancelled cancelled = {});
    static Bytes write_streamed(std::vector<ArchiveWriteEntry> entries,
                                ArchiveLimits limits = {}, Cancelled cancelled = {});

private:
    struct Entry {
        std::size_t offset;
        std::uint32_t compressed, uncompressed, crc;
        std::uint16_t method;
    };
    std::shared_ptr<Bytes const> _bytes;
    std::map<std::string, Entry> _entries;
};

} // namespace Inkscape::IO::ArtworkLibrary
#endif
