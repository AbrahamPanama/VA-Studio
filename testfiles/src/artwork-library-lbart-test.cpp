// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/artwork-library-lbart.h"
#include <gtest/gtest.h>
#include <glib.h>
#include <zlib.h>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

using namespace Inkscape::IO::ArtworkLibrary;
namespace {
void put32(Bytes &bytes, std::size_t offset, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = (value >> (24 - i * 8)) & 255;
}
std::uint32_t get32(Bytes const &bytes, std::size_t offset)
{
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) result = result * 256 + bytes.at(offset + i);
    return result;
}
void put_double(Bytes &bytes, std::size_t offset, double value)
{
    auto bits = std::bit_cast<std::uint64_t>(value);
    put32(bytes, offset, bits >> 32);
    put32(bytes, offset + 4, bits & 0xffffffffu);
}
Bytes fixture(unsigned count = 1, std::string xml =
              "<LightBurnShapes FormatVersion=\"1\"><Shape Type=\"Rect\"/></LightBurnShapes>")
{
    Bytes result(8), table(count * 136);
    for (unsigned i = 0; i < count; ++i) {
        auto offset = i * 136;
        table[offset + 1] = 'A' + i;
        put32(table, offset + 104, result.size());
        put32(table, offset + 108, 4);
        result.insert(result.end(), {'P', 'N', 'G', '!'}); // deliberately untrusted cache, not decoded
        put32(table, offset + 112, result.size());
        auto start = result.size();
        uLongf size = compressBound(xml.size());
        result.resize(start + 4 + size);
        put32(result, start, xml.size());
        if (compress2(result.data() + start + 4, &size,
                      reinterpret_cast<Bytef const *>(xml.data()), xml.size(), Z_BEST_SPEED) != Z_OK) {
            throw std::runtime_error("Cannot create compressed fixture");
        }
        result.resize(start + 4 + size);
        put32(table, offset + 116, size + 4);
        put_double(table, offset + 120, 25.4 + i);
        put_double(table, offset + 128, 50.8 + i);
    }
    put32(result, 0, result.size());
    put32(result, 4, count);
    result.insert(result.end(), table.begin(), table.end());
    return result;
}

TEST(LbartArchiveTest, ReadsIndexedNamesPayloadsAndRawExtents)
{
    auto archive = LbartArchive::open(fixture(2));
    auto entries = archive.entries();
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].name, "A"); EXPECT_EQ(entries[1].name, "B");
    EXPECT_DOUBLE_EQ(entries[0].extent_x, 25.4);
    EXPECT_DOUBLE_EQ(entries[1].extent_y, 51.8);
    auto xml = archive.read_artwork(1);
    EXPECT_EQ(std::string(xml.begin(), xml.end()),
              "<LightBurnShapes FormatVersion=\"1\"><Shape Type=\"Rect\"/></LightBurnShapes>");
    EXPECT_EQ(archive.read_preview(0), (Bytes{'P', 'N', 'G', '!'}));
    EXPECT_THROW(archive.read_artwork(2), std::out_of_range);
}

TEST(LbartArchiveTest, EmptyDirectoryAndUtf16Names)
{
    EXPECT_TRUE(LbartArchive::open(fixture(0)).entries().empty());
    auto bytes = fixture();
    auto index = get32(bytes, 0);
    bytes[index] = 0xd8; bytes[index + 1] = 0x3d;
    bytes[index + 2] = 0xde; bytes[index + 3] = 0x80;
    EXPECT_EQ(LbartArchive::open(bytes).entries()[0].name, "🚀");
    bytes[index + 2] = 0; bytes[index + 3] = 0;
    EXPECT_THROW(LbartArchive::open(bytes), LbartError);
}

TEST(LbartArchiveTest, EveryTruncationFails)
{
    auto bytes = fixture(2);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        SCOPED_TRACE(size);
        EXPECT_THROW(LbartArchive::open(Bytes(bytes.begin(), bytes.begin() + size)), LbartError);
    }
}

TEST(LbartArchiveTest, RejectsDirectoryAliasesGapsTrailingBytesAndCounts)
{
    auto original = fixture(2);
    auto index = get32(original, 0);
    for (auto [offset, value] : {std::pair<std::size_t, std::uint32_t>{0, 7}, {4, 3},
             {index + 104, 9}, {index + 108, 0xffffffffu}, {index + 112, 8},
             {index + 116, 0xffffffffu}, {index + 136 + 104, 8}}) {
        auto bytes = original; put32(bytes, offset, value);
        EXPECT_THROW(LbartArchive::open(std::move(bytes)), LbartError);
    }
    original.push_back(0);
    EXPECT_THROW(LbartArchive::open(original), LbartError);
}

TEST(LbartArchiveTest, RejectsNamePaddingControlsAndInvalidExtents)
{
    auto original = fixture(); auto index = get32(original, 0);
    auto bytes = original; bytes[index + 5] = 'B';
    EXPECT_THROW(LbartArchive::open(bytes), LbartError);
    bytes = original; bytes[index + 1] = '\n';
    EXPECT_THROW(LbartArchive::open(bytes), LbartError);
    for (double value : {0., -1., std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
        bytes = original; put_double(bytes, index + 120, value);
        EXPECT_THROW(LbartArchive::open(bytes), LbartError);
    }
}

TEST(LbartArchiveTest, EnforcesBudgetsBeforeInflation)
{
    auto bytes = fixture(2);
    LbartLimits limits;
    limits.entries = 1; EXPECT_THROW(LbartArchive::open(bytes, limits), LbartError);
    limits = {}; limits.file_bytes = bytes.size() - 1; EXPECT_THROW(LbartArchive::open(bytes, limits), LbartError);
    limits = {}; limits.preview_bytes = 3; EXPECT_THROW(LbartArchive::open(bytes, limits), LbartError);
    limits = {}; limits.artwork_bytes = 1; EXPECT_THROW(LbartArchive::open(bytes, limits), LbartError);
    limits = {}; limits.total_artwork_bytes = get32(bytes, 12);
    EXPECT_THROW(LbartArchive::open(bytes, limits), LbartError);
}

TEST(LbartArchiveTest, RejectsChecksumAndDeclaredLengthMismatch)
{
    auto original = fixture(); auto index = get32(original, 0);
    auto start = get32(original, index + 112);
    auto bytes = original; bytes[index - 1] ^= 1;
    EXPECT_THROW(LbartArchive::open(bytes).read_artwork(0), LbartError);
    for (auto length : {1u, get32(original, start) + 1}) {
        bytes = original; put32(bytes, start, length);
        EXPECT_THROW(LbartArchive::open(bytes).read_artwork(0), LbartError);
    }
    bytes = original;
    bytes.insert(bytes.begin() + index, 0);
    put32(bytes, 0, index + 1);
    put32(bytes, index + 1 + 116, get32(original, index + 116) + 1);
    EXPECT_THROW(LbartArchive::open(bytes).read_artwork(0), LbartError);
}

TEST(LbartArchiveTest, CancellationAndCallbackOwnerDestruction)
{
    EXPECT_THROW(LbartArchive::open(fixture(), {}, [] { return true; }), LbartError);
    auto archive = LbartArchive::open(fixture());
    EXPECT_THROW(archive.read_artwork(0, [] { return true; }), LbartError);
    EXPECT_THROW(archive.read_preview(0, [] { return true; }), LbartError);
    std::optional<LbartArchive> owner(archive);
    auto xml = owner->read_artwork(0, [&] { owner.reset(); return false; });
    EXPECT_FALSE(owner); EXPECT_FALSE(xml.empty());
    owner = archive;
    EXPECT_EQ(owner->read_preview(0, [&] { owner.reset(); return false; }), (Bytes{'P', 'N', 'G', '!'}));
    EXPECT_FALSE(owner);
}

TEST(LbartArchiveTest, LargePayloadChecksCancellationDuringInflation)
{
    auto archive = LbartArchive::open(fixture(1, std::string(512 * 1024, 'x')));
    unsigned checks = 0;
    EXPECT_THROW(archive.read_artwork(0, [&] { return ++checks == 4; }), LbartError);
    EXPECT_EQ(checks, 4u);
    EXPECT_EQ(archive.read_artwork(0), Bytes(512 * 1024, 'x'));
}

TEST(LbartArchiveTest, TruncatedCompressedStreamsFailAfterDirectoryAdmission)
{
    for (auto const &original : {fixture(), fixture(1, std::string(512 * 1024, 'x'))}) {
        auto index = get32(original, 0);
        auto compressed = get32(original, index + 116);
        // Keep at least the four-byte size plus two-byte zlib header. Rebuild
        // the directory location/length so open() succeeds and the decoder is
        // actually exercised, rather than rejecting a truncated table first.
        for (std::uint32_t cut = 1; cut <= compressed - 6; ++cut) {
            auto bytes = original;
            bytes.erase(bytes.begin() + index - cut, bytes.begin() + index);
            put32(bytes, 0, index - cut);
            put32(bytes, index - cut + 116, compressed - cut);
            auto archive = LbartArchive::open(std::move(bytes));
            EXPECT_THROW(archive.read_artwork(0), LbartError) << "removed " << cut << " compressed bytes";
        }
    }
}

// Explicit standalone/private lane only. The ordinary critical target contains
// no mandatory test that silently skips because customer data is unavailable.
#ifdef VACARDS_PRIVATE_LBART_TEST
TEST(LbartArchivePrivateTest, ExactCustomerSampleExtractsAllIndexedRecords)
{
    auto path = std::getenv("VACARDS_PRIVATE_LBART_SAMPLE");
    if (!path) GTEST_SKIP() << "Private sample is not part of the public fixture corpus";
    ASSERT_EQ(std::filesystem::file_size(path), 4218346u);
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file);
    Bytes bytes(4218346);
    file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()); ASSERT_TRUE(file);
    auto digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes.data(), bytes.size());
    auto hash = std::string(digest); g_free(digest);
    ASSERT_EQ(hash, "f16729fc57d10ae0728191a4dfe180abd171d169a9cdce00b720131238ca2f86");
    auto archive = LbartArchive::open(std::move(bytes));
    auto entries = archive.entries(); ASSERT_EQ(entries.size(), 37);
    std::size_t total = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto xml = archive.read_artwork(i);
        EXPECT_EQ(xml.size(), entries[i].artwork_bytes);
        total += xml.size();
        EXPECT_FALSE(archive.read_preview(i).empty());
    }
    EXPECT_EQ(total, 11256711u);
}
#endif
} // namespace
