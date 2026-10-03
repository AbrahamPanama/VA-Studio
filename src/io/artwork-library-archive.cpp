// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-archive.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <zlib.h>

namespace Inkscape::IO::ArtworkLibrary {
namespace {

[[noreturn]] void invalid(char const *reason) { throw std::runtime_error(reason); }
void check_cancel(Cancelled const &cancelled)
{
    if (cancelled && cancelled()) invalid("Artwork library operation cancelled");
}
void range(Bytes const &bytes, std::size_t at, std::size_t count)
{
    if (at > bytes.size() || count > bytes.size() - at) invalid("Truncated artwork library ZIP");
}
std::uint32_t number(Bytes const &bytes, std::size_t at, unsigned size)
{
    range(bytes, at, size);
    std::uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= std::uint32_t(bytes[at + i]) << (8 * i);
    return value;
}
void append(Bytes &bytes, std::uint32_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i) bytes.push_back((value >> (8 * i)) & 255);
}
std::string normalized_path(std::string const &path)
{
    // Native archive entry names are identifiers, not display names. Restrict
    // them to portable ASCII; reject Windows drive names, separators and aliases.
    if (path.empty() || path.size() > 240 || path.front() == '/' || path.back() == '/') {
        invalid("Invalid artwork library entry path");
    }
    std::string lower;
    std::string component;
    auto finish = [&] {
        if (component.empty() || component == "." || component == ".." || component.back() == '.') {
            invalid("Unsafe artwork library entry path");
        }
        auto stem = component.substr(0, component.find('.'));
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
            (stem.size() == 4 && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt") &&
             stem[3] >= '1' && stem[3] <= '9')) invalid("Reserved artwork library entry path");
        component.clear();
    };
    for (unsigned char c : path) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '/') finish();
        else {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
                invalid("Nonportable artwork library entry path");
            }
            component += c;
        }
        lower += c;
    }
    finish();
    return lower;
}
std::uint32_t checksum(Bytes const &bytes)
{
    return crc32(0, bytes.data(), static_cast<uInt>(bytes.size()));
}
void extra(Bytes const &bytes, std::size_t at, std::size_t count)
{
    range(bytes, at, count);
    auto end = at + count;
    while (at < end) {
        if (end - at < 4) invalid("Malformed ZIP extra field");
        auto id = number(bytes, at, 2);
        auto length = number(bytes, at + 2, 2);
        at += 4;
        if (length > end - at) invalid("Malformed ZIP extra field");
        if (id == 1 || id == 0x7075) invalid("ZIP64 and alternate ZIP paths are unsupported");
        at += length;
    }
}
void limits_valid(ArchiveLimits const &limits)
{
    if (limits.package_bytes >= std::numeric_limits<std::uint32_t>::max() ||
        limits.entry_bytes >= std::numeric_limits<uInt>::max() || limits.entries >= 65535) {
        invalid("Artwork library limits exceed ZIP32 capacity");
    }
}
} // namespace

Archive Archive::open(Bytes bytes, ArchiveLimits limits, Cancelled cancelled)
{
    limits_valid(limits);
    check_cancel(cancelled);
    if (bytes.size() > limits.package_bytes || bytes.size() < 22) invalid("Invalid artwork library package size");
    // Locate an EOCD whose declared comment ends exactly at EOF. Never scan
    // for local file signatures or accept trailing/concatenated packages.
    auto end = bytes.size() - 22;
    auto minimum = end > 65535 ? end - 65535 : 0;
    while (number(bytes, end, 4) != 0x06054b50 || number(bytes, end + 20, 2) != bytes.size() - end - 22) {
        if (end == minimum) invalid("Missing ZIP central directory");
        --end;
    }
    auto count = number(bytes, end + 10, 2);
    auto directory = number(bytes, end + 16, 4);
    auto directory_size = number(bytes, end + 12, 4);
    if (number(bytes, end + 4, 2) || number(bytes, end + 6, 2) ||
        number(bytes, end + 8, 2) != count || count >= 65535 || count > limits.entries ||
        directory > end || directory_size != end - directory) invalid("Unsupported ZIP directory");

    Archive result;
    std::set<std::string> names;
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    std::size_t cursor = directory, total = 0;
    for (unsigned i = 0; i < count; ++i) {
        check_cancel(cancelled);
        range(bytes, cursor, 46);
        if (number(bytes, cursor, 4) != 0x02014b50) invalid("Invalid ZIP entry");
        auto flags = number(bytes, cursor + 8, 2);
        auto method = number(bytes, cursor + 10, 2);
        auto crc = number(bytes, cursor + 16, 4);
        auto compressed = number(bytes, cursor + 20, 4);
        auto uncompressed = number(bytes, cursor + 24, 4);
        auto name_size = number(bytes, cursor + 28, 2);
        auto extra_size = number(bytes, cursor + 30, 2);
        auto comment_size = number(bytes, cursor + 32, 2);
        std::size_t offset = number(bytes, cursor + 42, 4);
        auto mode = number(bytes, cursor + 38, 4) >> 16;
        auto allowed_flags = method == 8 ? 0x80eu : 0x808u;
        if (number(bytes, cursor + 6, 2) > 20 || (flags & ~allowed_flags) || (method != 0 && method != 8) ||
            number(bytes, cursor + 34, 2) || (mode & 0170000u && (mode & 0170000u) != 0100000u)) {
            invalid("Encrypted, linked or unsupported ZIP entry");
        }
        range(bytes, cursor + 46, name_size + extra_size + comment_size);
        if (cursor + 46 + name_size + extra_size + comment_size > end) invalid("ZIP directory overrun");
        std::string name(bytes.begin() + cursor + 46, bytes.begin() + cursor + 46 + name_size);
        if (!names.insert(normalized_path(name)).second) invalid("Duplicate artwork library entry");
        extra(bytes, cursor + 46 + name_size, extra_size);
        if (uncompressed > limits.entry_bytes || total > limits.total_bytes ||
            uncompressed > limits.total_bytes - total || (method == 0 && compressed != uncompressed)) {
            invalid("Artwork library decompression budget exceeded");
        }
        total += uncompressed;
        range(bytes, offset, 30);
        auto local_name = number(bytes, offset + 26, 2);
        auto local_extra = number(bytes, offset + 28, 2);
        std::size_t data = std::size_t(offset) + 30 + local_name + local_extra;
        range(bytes, offset + 30, local_name + local_extra);
        if (number(bytes, offset, 4) != 0x04034b50 || number(bytes, offset + 4, 2) > 20 || number(bytes, offset + 6, 2) != flags ||
            number(bytes, offset + 8, 2) != method || local_name != name_size ||
            !std::equal(name.begin(), name.end(), bytes.begin() + offset + 30)) invalid("Conflicting ZIP headers");
        extra(bytes, offset + 30 + local_name, local_extra);
        if (!(flags & 8) && (number(bytes, offset + 14, 4) != crc ||
            number(bytes, offset + 18, 4) != compressed || number(bytes, offset + 22, 4) != uncompressed)) {
            invalid("Conflicting ZIP sizes or checksum");
        }
        if (data > directory || compressed > directory - data) invalid("ZIP payload overlaps directory");
        auto payload_end = data + compressed;
        if (flags & 8) {
            auto descriptor = payload_end;
            auto matches = [&](std::size_t at) {
                return at <= directory && directory - at >= 12 && number(bytes, at, 4) == crc &&
                    number(bytes, at + 4, 4) == compressed && number(bytes, at + 8, 4) == uncompressed;
            };
            // The optional signature may also be a valid unsigned descriptor's
            // CRC. Prefer a complete matching tuple, not a signature-only guess.
            if (!matches(descriptor)) {
                if (descriptor > directory || directory - descriptor < 16 ||
                    number(bytes, descriptor, 4) != 0x08074b50 || !matches(descriptor + 4)) {
                    invalid("Invalid ZIP data descriptor");
                }
                descriptor += 4;
            }
            payload_end = descriptor + 12;
        }
        spans.emplace_back(offset, payload_end);
        result._entries.emplace(name, Entry{data, compressed, uncompressed, crc, static_cast<std::uint16_t>(method)});
        cursor += 46 + name_size + extra_size + comment_size;
    }
    if (cursor != end) invalid("Unexpected ZIP directory data");
    std::sort(spans.begin(), spans.end());
    std::size_t previous = 0;
    for (auto const &[start, finish] : spans) {
        if (start != previous) invalid("Overlapping or unindexed ZIP data");
        previous = finish;
    }
    if (previous != directory) invalid("Unindexed ZIP payload");
    result._bytes = std::make_shared<Bytes const>(std::move(bytes));
    return result;
}

bool Archive::contains(std::string const &path) const { return _entries.count(path) != 0; }
std::size_t Archive::size(std::string const &path) const { return _entries.at(path).uncompressed; }
std::vector<std::string> Archive::paths() const
{
    std::vector<std::string> paths;
    for (auto const &[path, entry] : _entries) paths.push_back(path);
    return paths;
}
Bytes Archive::read(std::string const &path, Cancelled cancelled) const
{
    // A cancellation callback can close or replace the owning collection.
    // Keep this read's immutable storage and metadata alive independently.
    auto bytes = _bytes;
    auto entry = _entries.at(path);
    check_cancel(cancelled);
    Bytes output(entry.uncompressed);
    auto input = bytes->data() + entry.offset;
    if (!entry.method) {
        for (std::size_t at = 0; at < output.size();) {
            check_cancel(cancelled);
            auto count = std::min<std::size_t>(32768, output.size() - at);
            std::copy_n(input + at, count, output.data() + at);
            at += count;
        }
    } else {
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) invalid("Cannot initialize ZIP decompressor");
        struct End { z_stream *stream; ~End() { inflateEnd(stream); } } cleanup{&stream};
        unsigned char excess;
        int status;
        do {
            check_cancel(cancelled);
            if (!stream.avail_in) {
                // Bound input as well as output: empty DEFLATE blocks can use
                // substantial CPU without producing any output bytes.
                stream.next_in = const_cast<Bytef *>(input + stream.total_in);
                stream.avail_in = std::min<std::size_t>(32768, entry.compressed - stream.total_in);
            }
            auto remaining = output.size() - std::size_t(stream.total_out);
            stream.next_out = remaining ? output.data() + stream.total_out : &excess;
            stream.avail_out = remaining ? std::min<std::size_t>(remaining, 32768) : 1;
            status = inflate(&stream, Z_NO_FLUSH);
            if (stream.total_out > output.size() || (status != Z_OK && status != Z_STREAM_END)) {
                invalid("Invalid or oversized ZIP deflate stream");
            }
        } while (status != Z_STREAM_END);
        if (stream.total_out != output.size() || stream.total_in != entry.compressed) invalid("Conflicting ZIP stream length");
    }
    check_cancel(cancelled);
    if (checksum(output) != entry.crc) invalid("Artwork library checksum mismatch");
    return output;
}

Bytes Archive::write(std::map<std::string, Bytes> const &files, ArchiveLimits limits, Cancelled cancelled)
{
    limits_valid(limits);
    if (files.size() > limits.entries) invalid("Artwork library package limit exceeded");
    std::vector<ArchiveWriteEntry> entries;
    entries.reserve(files.size());
    // This compatibility API borrows its map for the synchronous call, as
    // before. No payload copy is needed; callbacks must not mutate that map.
    for (auto const &[name, data] : files) {
        entries.push_back({name, data.size(), [&data] {
            return std::shared_ptr<Bytes const>(&data, [](Bytes const *) {});
        }});
    }
    return write_streamed(std::move(entries), limits, std::move(cancelled));
}

Bytes Archive::write_streamed(std::vector<ArchiveWriteEntry> entries, ArchiveLimits limits, Cancelled cancelled)
{
    limits_valid(limits);
    check_cancel(cancelled);
    if (entries.size() > limits.entries || limits.package_bytes < 22) invalid("Artwork library package limit exceeded");
    std::size_t total = 0, package = 22;
    std::set<std::string> names;
    for (auto const &entry : entries) {
        check_cancel(cancelled);
        if (!entry.read) invalid("Missing artwork library entry reader");
        if (!names.insert(normalized_path(entry.path)).second) invalid("Duplicate artwork library entry");
        if (entry.size > limits.entry_bytes || total > limits.total_bytes || entry.size > limits.total_bytes - total) {
            invalid("Artwork library size limit exceeded");
        }
        total += entry.size;
        auto size = std::uint64_t(76) + 2 * entry.path.size() + entry.size;
        if (package > limits.package_bytes || size > limits.package_bytes - package) invalid("Artwork library package limit exceeded");
        package += size;
    }
    Bytes output;
    output.reserve(package);
    std::vector<std::uint32_t> offsets, crcs;
    for (auto const &entry : entries) {
        check_cancel(cancelled);
        auto data = entry.read();
        check_cancel(cancelled);
        if (!data || data->size() != entry.size) invalid("Artwork library entry changed during encoding");
        auto const &name = entry.path;
        offsets.push_back(output.size());
        std::uint32_t crc = crc32(0, nullptr, 0);
        for (std::size_t at = 0; at < data->size();) {
            check_cancel(cancelled);
            auto count = std::min<std::size_t>(65536, data->size() - at);
            crc = crc32(crc, data->data() + at, static_cast<uInt>(count));
            at += count;
        }
        crcs.push_back(crc);
        append(output, 0x04034b50, 4); append(output, 20, 2);
        append(output, 0x800, 2); append(output, 0, 2); append(output, 0, 4);
        append(output, crcs.back(), 4); append(output, entry.size, 4); append(output, entry.size, 4);
        append(output, name.size(), 2); append(output, 0, 2);
        output.insert(output.end(), name.begin(), name.end());
        for (std::size_t at = 0; at < data->size();) {
            check_cancel(cancelled);
            auto count = std::min<std::size_t>(65536, data->size() - at);
            output.insert(output.end(), data->begin() + at, data->begin() + at + count);
            at += count;
        }
    }
    auto directory = output.size();
    std::size_t index = 0;
    for (auto const &entry : entries) {
        check_cancel(cancelled);
        auto const &name = entry.path;
        append(output, 0x02014b50, 4); append(output, 20, 2); append(output, 20, 2);
        append(output, 0x800, 2); append(output, 0, 2); append(output, 0, 4);
        append(output, crcs[index], 4); append(output, entry.size, 4); append(output, entry.size, 4);
        append(output, name.size(), 2); append(output, 0, 2); append(output, 0, 2);
        append(output, 0, 2); append(output, 0, 2); append(output, 0, 4); append(output, offsets[index++], 4);
        output.insert(output.end(), name.begin(), name.end());
    }
    auto size = output.size() - directory;
    append(output, 0x06054b50, 4); append(output, 0, 4);
    append(output, entries.size(), 2); append(output, entries.size(), 2);
    append(output, size, 4); append(output, directory, 4); append(output, 0, 2);
    check_cancel(cancelled);
    return output;
}

} // namespace Inkscape::IO::ArtworkLibrary
