// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <stdexcept>
#include <limits>
#include <zlib.h>
#include "io/artwork-library-archive.h"

using namespace Inkscape::IO::ArtworkLibrary;
namespace {
Bytes data(std::string const &value) { return Bytes(value.begin(), value.end()); }
void set(Bytes &bytes, std::size_t at, std::uint32_t value, unsigned size = 4)
{
    for (unsigned i = 0; i < size; ++i) bytes.at(at + i) = (value >> (8 * i)) & 255;
}
std::size_t directory(Bytes const &bytes)
{
    std::size_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= std::size_t(bytes.at(bytes.size() - 6 + i)) << (8 * i);
    return value;
}
Bytes one() { return Archive::write({{"library.json", data("{\"formatVersion\":1}")}}); }

// Independent deflate fixture (writer intentionally emits stored entries).
Bytes deflated(std::string const &text, bool descriptor = false)
{
    auto bytes = Archive::write({{"library.json", data(text)}});
    auto original_directory = directory(bytes);
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("fixture deflate init failed");
    }
    Bytes compressed(compressBound(text.size()));
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(text.data()));
    stream.avail_in = text.size();
    stream.next_out = compressed.data();
    stream.avail_out = compressed.size();
    auto status = deflate(&stream, Z_FINISH);
    auto size = stream.total_out;
    deflateEnd(&stream);
    if (status != Z_STREAM_END) throw std::runtime_error("fixture deflate failed");
    compressed.resize(size);
    Bytes tail(bytes.begin() + original_directory, bytes.end());
    bytes.resize(42); // fixed header + library.json
    set(bytes, 8, 8, 2);
    set(bytes, 18, size);
    bytes.insert(bytes.end(), compressed.begin(), compressed.end());
    if (descriptor) {
        set(bytes, 6, 0x808, 2);
        auto at = bytes.size();
        bytes.resize(at + 16);
        set(bytes, at, 0x08074b50);
        set(bytes, at + 4, crc32(0, reinterpret_cast<Bytef const *>(text.data()), text.size()));
        set(bytes, at + 8, size);
        set(bytes, at + 12, text.size());
        set(bytes, 14, 0); set(bytes, 18, 0); set(bytes, 22, 0);
        set(tail, 8, 0x808, 2);
    }
    auto new_directory = bytes.size();
    set(tail, 10, 8, 2);
    set(tail, 20, size);
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    set(bytes, bytes.size() - 6, new_directory);
    return bytes;
}
}

TEST(ArtworkLibraryArchive, RoundTripIndependentPayloadsAndEmptyArchive)
{
    std::map<std::string, Bytes> files{{"library.json", data("{}")},
        {"assets/0123.svg", data("<svg/>")}, {"previews/0123.png", {0, 1, 127, 255}}, {"empty", {}}};
    auto archive = Archive::open(Archive::write(files));
    ASSERT_EQ(archive.paths().size(), files.size());
    for (auto const &[name, value] : files) {
        EXPECT_EQ(archive.size(name), value.size());
        EXPECT_EQ(archive.read(name), value);
    }
    EXPECT_FALSE(archive.contains("missing"));
    EXPECT_THROW(archive.read("missing"), std::out_of_range);
    EXPECT_THROW(archive.size("missing"), std::out_of_range);
    EXPECT_TRUE(Archive::open(Archive::write({})).paths().empty());
}

TEST(ArtworkLibraryArchive, InflightReadSurvivesOwnerDestructionAndReplacement)
{
    Bytes expected(100000, 42);
    auto bytes = Archive::write({{"payload", expected}});
    auto owner = std::make_unique<Archive>(Archive::open(bytes));
    unsigned calls = 0;
    EXPECT_EQ(owner->read("payload", [&] {
        if (++calls == 2) owner.reset();
        return false;
    }), expected);
    EXPECT_FALSE(owner);
    auto archive = Archive::open(std::move(bytes));
    calls = 0;
    EXPECT_EQ(archive.read("payload", [&] {
        if (++calls == 2) archive = Archive::open(Archive::write({{"payload", {0}}}));
        return false;
    }), expected);
    EXPECT_EQ(archive.read("payload"), Bytes{0});
}

TEST(ArtworkLibraryArchive, RejectsEveryTruncation)
{
    auto bytes = one();
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        EXPECT_THROW(Archive::open(Bytes(bytes.begin(), bytes.begin() + size)), std::runtime_error) << size;
    }
}

TEST(ArtworkLibraryArchive, RejectsUnsafeAndCaseAliasedPaths)
{
    for (auto const *name : {"", "../a", "/a", "a/../b", "a//b", "a/", "a\\b", "C:a", "./a",
                             "a.", "a ", "a/nul.svg", "COM1", "a/LPT9.png"}) {
        EXPECT_THROW(Archive::write({{name, {}}}), std::runtime_error) << name;
    }
    EXPECT_THROW(Archive::write({{"A.svg", {}}, {"a.svg", {}}}), std::runtime_error);
    auto bytes = one();
    bytes[30] = '/'; bytes[directory(bytes) + 46] = '/';
    EXPECT_THROW(Archive::open(bytes), std::runtime_error);
}

TEST(ArtworkLibraryArchive, RejectsChecksumsHeadersLinksAndOffsets)
{
    auto original = one();
    auto bytes = original;
    bytes[42] ^= 1;
    auto archive = Archive::open(bytes); // CRC is lazy; opening does not inflate content
    EXPECT_THROW(archive.read("library.json"), std::runtime_error);
    for (auto at : {std::size_t(14), std::size_t(18), std::size_t(22), std::size_t(30)}) {
        bytes = original; bytes[at] ^= 1;
        EXPECT_THROW(Archive::open(bytes), std::runtime_error) << at;
    }
    bytes = original; set(bytes, directory(bytes) + 42, 0xfffffff0);
    EXPECT_THROW(Archive::open(bytes), std::runtime_error);
    bytes = original; set(bytes, directory(bytes) + 38, 0120777u << 16);
    EXPECT_THROW(Archive::open(bytes), std::runtime_error);
    bytes = original; set(bytes, directory(bytes) + 8, 1, 2);
    EXPECT_THROW(Archive::open(bytes), std::runtime_error);
    bytes = original; set(bytes, bytes.size() - 18, 1, 2);
    EXPECT_THROW(Archive::open(bytes), std::runtime_error);
    bytes = original; bytes.push_back(0);
    EXPECT_THROW(Archive::open(bytes), std::runtime_error);
}

TEST(ArtworkLibraryArchive, EnforcesBudgetsBeforeInflationAndWriting)
{
    auto bytes = one();
    ArchiveLimits limits;
    limits.package_bytes = bytes.size() - 1;
    EXPECT_THROW(Archive::open(bytes, limits), std::runtime_error);
    EXPECT_THROW(Archive::write({{"library.json", data("{\"formatVersion\":1}")}}, limits), std::runtime_error);
    limits = {}; limits.entry_bytes = 1;
    EXPECT_THROW(Archive::open(bytes, limits), std::runtime_error);
    EXPECT_THROW(Archive::write({{"a", {1, 2}}}, limits), std::runtime_error);
    limits = {}; limits.total_bytes = 3;
    EXPECT_THROW(Archive::write({{"a", {1, 2}}, {"b", {3, 4}}}, limits), std::runtime_error);
    limits = {}; limits.entries = 0;
    EXPECT_THROW(Archive::open(bytes, limits), std::runtime_error);
    limits = {}; limits.package_bytes = 0;
    EXPECT_THROW(Archive::write({}, limits), std::runtime_error);
    limits = {}; limits.entry_bytes = std::numeric_limits<std::uint32_t>::max();
    EXPECT_THROW(Archive::open(bytes, limits), std::runtime_error);
}

TEST(ArtworkLibraryArchive, DeflateAndDescriptorRoundTrip)
{
    for (auto const &text : {std::string(), std::string("<svg/>"), std::string(100000, 'a')}) {
        for (bool descriptor : {false, true}) {
            EXPECT_EQ(Archive::open(deflated(text, descriptor)).read("library.json"), data(text));
        }
    }
}

TEST(ArtworkLibraryArchive, RejectsDeflateSizeLies)
{
    auto bytes = deflated(std::string(100000, 'a'));
    set(bytes, 22, 1); set(bytes, directory(bytes) + 24, 1);
    auto archive = Archive::open(bytes);
    EXPECT_THROW(archive.read("library.json"), std::runtime_error);
    bytes = deflated("abc");
    set(bytes, 22, 100); set(bytes, directory(bytes) + 24, 100);
    EXPECT_THROW(Archive::open(bytes).read("library.json"), std::runtime_error);
}

TEST(ArtworkLibraryArchive, CancellationAtOpenWriteAndDuringRead)
{
    auto cancel = [] { return true; };
    EXPECT_THROW(Archive::open(one(), {}, cancel), std::runtime_error);
    EXPECT_THROW(Archive::write({{"a", {}}}, {}, cancel), std::runtime_error);
    EXPECT_THROW(Archive::write({}, {}, cancel), std::runtime_error);
    auto archive = Archive::open(deflated(std::string(100000, 'a')));
    unsigned calls = 0;
    EXPECT_THROW(archive.read("library.json", [&] { return ++calls == 3; }), std::runtime_error);
    EXPECT_EQ(calls, 3u);
    EXPECT_EQ(archive.read("library.json").size(), 100000u); // cancellation cannot poison snapshot
}

TEST(ArtworkLibraryArchive, AllowsDeflateCompressionOptionFlagsAndUnsignedDescriptors)
{
    for (unsigned option : {0u, 2u, 4u, 6u}) {
        auto bytes = deflated("abc");
        set(bytes, 6, 0x800 | option, 2);
        set(bytes, directory(bytes) + 8, 0x800 | option, 2);
        EXPECT_EQ(Archive::open(bytes).read("library.json"), data("abc"));
    }
    auto bytes = deflated("abc", true);
    auto at = directory(bytes);
    bytes.erase(bytes.begin() + at - 16, bytes.begin() + at - 12);
    set(bytes, bytes.size() - 6, at - 4);
    EXPECT_EQ(Archive::open(bytes).read("library.json"), data("abc"));
}

TEST(ArtworkLibraryArchive, EmptyDeflateBlocksObserveBoundedInputCancellation)
{
    auto bytes = deflated("");
    Bytes tail(bytes.begin() + directory(bytes), bytes.end());
    bytes.resize(42);
    for (unsigned block = 0; block < 100000; ++block) {
        bytes.insert(bytes.end(), {0, 0, 0, 255, 255}); // empty non-final stored block
    }
    bytes.insert(bytes.end(), {1, 0, 0, 255, 255}); // final empty block
    auto compressed = bytes.size() - 42;
    set(bytes, 18, compressed);
    set(tail, 20, compressed);
    auto at = bytes.size();
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    set(bytes, bytes.size() - 6, at);
    auto archive = Archive::open(bytes);
    unsigned calls = 0;
    EXPECT_THROW(archive.read("library.json", [&] { return ++calls == 4; }), std::runtime_error);
    EXPECT_EQ(calls, 4u);
    EXPECT_TRUE(archive.read("library.json").empty());
}

TEST(ArtworkLibraryArchive, DeterministicMalformedInputCampaign)
{
    auto seed = Archive::write({{"library.json", data("{}")}, {"assets/test.svg", data("<svg/>")}});
    std::uint32_t state = 0x4c494231;
    auto next = [&] { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
    ArchiveLimits limits;
    limits.package_bytes = 65536;
    limits.entry_bytes = 65536;
    limits.total_bytes = 131072;
    for (unsigned attempt = 0; attempt < 10000; ++attempt) {
        auto bytes = seed;
        for (unsigned change = 0, count = 1 + next() % 8; change < count; ++change) {
            bytes[next() % bytes.size()] ^= 1u << (next() % 8);
        }
        try {
            auto archive = Archive::open(std::move(bytes), limits);
            for (auto const &path : archive.paths()) archive.read(path);
        } catch (std::runtime_error const &) {
            // Malformed input rejection is expected. Run this under sanitizers
            // too: neither unchecked offsets nor decompression may escape caps.
        }
    }
}
