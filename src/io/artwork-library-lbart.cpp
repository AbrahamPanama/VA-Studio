// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-lbart.h"
#include <glib.h>
#include <zlib.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace Inkscape::IO::ArtworkLibrary {
namespace {
void check_cancel(Cancelled const &cancelled)
{
    if (cancelled && cancelled()) throw LbartError("LightBurn library read cancelled");
}

std::uint32_t u32(Bytes const &bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4) throw LbartError("Truncated LightBurn record");
    return std::uint32_t(bytes[offset]) << 24 | std::uint32_t(bytes[offset + 1]) << 16 |
           std::uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
}

double number(Bytes const &bytes, std::size_t offset)
{
    auto bits = std::uint64_t(u32(bytes, offset)) << 32 | u32(bytes, offset + 4);
    auto value = std::bit_cast<double>(bits);
    if (!std::isfinite(value) || value <= 0) throw LbartError("Invalid LightBurn extent metadata");
    return value;
}

std::string name(Bytes const &bytes, std::size_t offset)
{
    // Fixed 52 UTF-16BE code units, with zero-filled unused units.
    std::vector<gunichar2> units;
    bool padding = false;
    for (unsigned i = 0; i < 52; ++i) {
        auto unit = gunichar2(unsigned(bytes[offset + 2 * i]) << 8 | bytes[offset + 2 * i + 1]);
        if (!unit) { padding = true; continue; }
        if (padding) throw LbartError("Nonzero data after LightBurn name terminator");
        units.push_back(unit);
    }
    if (units.empty()) throw LbartError("Empty LightBurn artwork name");
    GError *error = nullptr;
    auto converted = g_utf16_to_utf8(units.data(), units.size(), nullptr, nullptr, &error);
    if (!converted) {
        g_clear_error(&error);
        throw LbartError("Invalid UTF-16 LightBurn artwork name");
    }
    std::string result(converted);
    g_free(converted);
    for (unsigned char c : result) {
        if (c < 32 || c == 127) throw LbartError("Control character in LightBurn artwork name");
    }
    return result;
}
} // namespace

LbartArchive LbartArchive::open(Bytes bytes, LbartLimits limits, Cancelled cancelled)
{
    if (bytes.size() > limits.file_bytes || bytes.size() < 8) throw LbartError("Invalid LightBurn file size");
    auto storage = std::make_shared<Bytes const>(std::move(bytes));
    auto const &input = *storage;
    check_cancel(cancelled);
    auto directory = std::size_t(u32(input, 0));
    auto count = std::size_t(u32(input, 4));
    if (count > limits.entries || directory < 8 || directory > input.size() ||
        count > (input.size() - directory) / 136 || input.size() - directory != count * 136) {
        throw LbartError("Unsupported or damaged LightBurn directory layout");
    }
    LbartArchive result;
    result._bytes = std::move(storage);
    result._records.reserve(count);
    std::size_t cursor = 8, total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        check_cancel(cancelled);
        auto record = directory + i * 136;
        auto preview_offset = std::size_t(u32(input, record + 104));
        auto preview_size = std::size_t(u32(input, record + 108));
        auto artwork_offset = std::size_t(u32(input, record + 112));
        auto compressed_size = std::size_t(u32(input, record + 116));
        // The observed variant packs every preview followed by its qCompress
        // stream before the table. Reject aliases, gaps and orphan payloads.
        if (preview_offset != cursor || preview_offset > directory ||
            preview_size > directory - preview_offset || preview_size > limits.preview_bytes ||
            artwork_offset != preview_offset + preview_size || artwork_offset > directory ||
            compressed_size < 6 || compressed_size > directory - artwork_offset) {
            throw LbartError("Invalid LightBurn payload boundaries");
        }
        auto declared = std::size_t(u32(input, artwork_offset));
        if (!declared || declared > limits.artwork_bytes || total > limits.total_artwork_bytes ||
            declared > limits.total_artwork_bytes - total) throw LbartError("LightBurn artwork byte budget exceeded");
        total += declared;
        result._records.push_back({{name(input, record), number(input, record + 120), number(input, record + 128),
                                  declared, preview_size}, preview_offset, artwork_offset, compressed_size});
        cursor = artwork_offset + compressed_size;
    }
    if (cursor != directory) throw LbartError("Unindexed LightBurn payload bytes");
    check_cancel(cancelled);
    return result;
}

std::vector<LbartEntry> LbartArchive::entries() const
{
    std::vector<LbartEntry> entries;
    entries.reserve(_records.size());
    for (auto const &record : _records) entries.push_back(record.metadata);
    return entries;
}

Bytes LbartArchive::read_artwork(std::size_t index, Cancelled cancelled) const
{
    // Pin everything before callbacks, including ones that close the archive.
    auto bytes = _bytes;
    auto record = _records.at(index);
    check_cancel(cancelled);
    if (record.compressed_bytes - 4 > std::numeric_limits<uInt>::max() ||
        record.metadata.artwork_bytes == std::numeric_limits<std::size_t>::max()) {
        throw LbartError("LightBurn stream exceeds decoder limits");
    }
    Bytes output(record.metadata.artwork_bytes + 1);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) throw LbartError("Cannot initialize LightBurn decompressor");
    struct Cleanup { z_stream &s; ~Cleanup() { inflateEnd(&s); } } cleanup{stream};
    stream.next_in = const_cast<Bytef *>(bytes->data() + record.artwork_offset + 4);
    stream.avail_in = record.compressed_bytes - 4;
    int status = Z_OK;
    std::size_t produced = 0;
    do {
        check_cancel(cancelled);
        // zlib's total_out is only 32 bits on Windows. Track progress from each
        // bounded chunk instead, including the sentinel byte for overflow.
        auto input_before = stream.avail_in;
        auto chunk = std::min<std::size_t>(65536, output.size() - produced);
        stream.next_out = output.data() + produced;
        stream.avail_out = chunk;
        status = inflate(&stream, Z_NO_FLUSH);
        auto written = chunk - stream.avail_out;
        produced += written;
        if (produced > record.metadata.artwork_bytes ||
            (status != Z_OK && status != Z_STREAM_END) ||
            (status == Z_OK && !written && input_before == stream.avail_in)) {
            throw LbartError("Invalid LightBurn compressed artwork");
        }
    } while (status != Z_STREAM_END);
    if (stream.avail_in || produced != record.metadata.artwork_bytes) {
        throw LbartError("LightBurn artwork length or stream termination mismatch");
    }
    output.resize(record.metadata.artwork_bytes);
    check_cancel(cancelled);
    return output;
}

Bytes LbartArchive::read_preview(std::size_t index, Cancelled cancelled) const
{
    auto bytes = _bytes;
    auto record = _records.at(index);
    check_cancel(cancelled);
    Bytes result(record.metadata.preview_bytes);
    for (std::size_t offset = 0; offset < result.size();) {
        check_cancel(cancelled);
        auto count = std::min<std::size_t>(65536, result.size() - offset);
        std::copy_n(bytes->data() + record.preview_offset + offset, count, result.data() + offset);
        offset += count;
    }
    return result;
}

} // namespace Inkscape::IO::ArtworkLibrary
