// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "io/artwork-library-encoding.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace Inkscape::IO::ArtworkLibrary;

namespace {
constexpr auto first = "b203e8e9-640c-4195-b0ea-f9b790245678";
constexpr auto second = "b203e8e9-640c-4195-b0ea-f9b790245679";
Bytes bytes(std::string const &s) { return {s.begin(), s.end()}; }
Manifest metadata()
{
    Manifest m;
    m.id = "77bcd302-a034-4b99-9270-139e70273123";
    m.name = "Marcos 🌸";
    m.extra_json = R"({"future":{"value":9007199254740993}})";
    return m;
}
NewArtwork art(std::string id = first)
{
    NewArtwork a;
    a.id = std::move(id);
    a.name = "Invitación";
    a.tags = {"Premio", "Árbol"};
    a.width_mm = 25.4;
    a.height_mm = 50.8;
    a.extra_json = R"({"source":{"format":"svg"}})";
    return a;
}
Bytes svg()
{
    return bytes("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1in\" height=\"2in\" viewBox=\"0 0 10 20\"><text x=\"2\" y=\"5\">Hola</text></svg>");
}
Catalog populated()
{
    auto c = Catalog::create(metadata());
    c.add(art(), svg());
    c.add(art(second), bytes("second opaque payload"));
    return c;
}
}

TEST(ArtworkLibraryEncoding, StreamedWriterMatchesExistingZipAndReleasesEachPayload)
{
    std::map<std::string, Bytes> files{{"assets/a.svg", Bytes(150000, 42)},
                                       {"assets/b.svg", Bytes(100000, 19)}, {"empty", {}}};
    std::vector<ArchiveWriteEntry> entries;
    std::weak_ptr<Bytes const> previous;
    unsigned reads = 0;
    for (auto const &[name, data] : files) {
        entries.push_back({name, data.size(), [&data, &previous, &reads] {
            EXPECT_TRUE(previous.expired());
            auto payload = std::make_shared<Bytes const>(data);
            previous = payload;
            ++reads;
            return payload;
        }});
    }
    auto encoded = Archive::write_streamed(entries);
    EXPECT_EQ(reads, files.size());
    EXPECT_TRUE(previous.expired());
    EXPECT_EQ(encoded, Archive::write(files));
    auto reopened = Archive::open(encoded);
    for (auto const &[name, data] : files) EXPECT_EQ(reopened.read(name), data);
}

TEST(ArtworkLibraryEncoding, StreamedWriterPreflightsAllMetadataBeforeReading)
{
    unsigned reads = 0;
    auto read = [&] { ++reads; return std::make_shared<Bytes const>(8, 1); };
    auto rejects = [&](std::vector<ArchiveWriteEntry> entries, ArchiveLimits limits = {}) {
        EXPECT_THROW(Archive::write_streamed(std::move(entries), limits), std::runtime_error);
        EXPECT_EQ(reads, 0);
    };
    rejects({{"a", 8, read}, {"../b", 8, read}});
    rejects({{"a", 8, read}, {"A", 8, read}});
    rejects({{"a", 8, read}, {"b", 8, {}}});
    auto limits = ArchiveLimits{};
    limits.entry_bytes = 7;
    rejects({{"a", 8, read}}, limits);
    limits = {};
    limits.total_bytes = 15;
    rejects({{"a", 8, read}, {"b", 8, read}}, limits);
    limits = {};
    limits.entries = 1;
    rejects({{"a", 8, read}, {"b", 8, read}}, limits);
    limits = {};
    limits.package_bytes = 100;
    rejects({{"a", 8, read}}, limits);
    rejects({{"a", std::numeric_limits<std::size_t>::max(), read}});
}

TEST(ArtworkLibraryEncoding, StoredZipMatchesIndependentGoldenBytes)
{
    // Generated in memory with Python's zipfile, with zero DOS timestamp,
    // UTF-8 flags and no external attributes to match the native v1 contract.
    // Python ZipFile.testzip() returned None and read("a") returned b"*".
    // Neither expected bytes nor CRC/offsets depend on our writer/reader.
    std::string const hex =
        "504b0304140000080000000000005b26b909010000000100000001000000612a"
        "504b01021400140000080000000000005b26b909010000000100000001000000"
        "000000000000000000000000000061504b050600000000010001002f000000200000000000";
    Bytes expected;
    for (std::size_t at = 0; at < hex.size(); at += 2) {
        expected.push_back(static_cast<unsigned char>(std::stoul(hex.substr(at, 2), nullptr, 16)));
    }
    EXPECT_EQ(Archive::write({{"a", {'*'}}}), expected);
    EXPECT_EQ(Archive::write_streamed({{"a", 1, [] {
        return std::make_shared<Bytes const>(1, '*');
    }}}), expected);
    EXPECT_EQ(Archive::open(expected).read("a"), Bytes{'*'});
}

TEST(ArtworkLibraryEncoding, StreamedWriterChecksReturnedSizeNullAndReadFailure)
{
    for (std::size_t actual : {0, 7, 9}) {
        EXPECT_THROW(Archive::write_streamed({{"a", 8, [=] {
            return std::make_shared<Bytes const>(actual, 1);
        }}}), std::runtime_error);
    }
    EXPECT_THROW(Archive::write_streamed({{"a", 0, [] {
        return std::shared_ptr<Bytes const>{};
    }}}), std::runtime_error);
    EXPECT_THROW(Archive::write_streamed({{"a", 8, []() -> std::shared_ptr<Bytes const> {
        throw std::logic_error("read failed");
    }}}), std::logic_error);
}

TEST(ArtworkLibraryEncoding, StreamedWriterOwnsDescriptorsAcrossCallbacks)
{
    std::vector<ArchiveWriteEntry> entries{{"a", 1, [] { return std::make_shared<Bytes const>(1, 42); }}};
    auto result = Archive::write_streamed(entries, {}, [&] { entries.clear(); return false; });
    EXPECT_EQ(Archive::open(std::move(result)).read("a"), Bytes{42});
}

TEST(ArtworkLibraryEncoding, StreamedWriterCancelsInsideLargePayloadAndReleasesIt)
{
    bool loaded = false;
    unsigned checks_after_load = 0;
    std::weak_ptr<Bytes const> payload;
    EXPECT_THROW(Archive::write_streamed({{"a", 1024 * 1024, [&] {
        loaded = true;
        auto p = std::make_shared<Bytes const>(1024 * 1024, 42);
        payload = p;
        return p;
    }}}, {}, [&] { return loaded && ++checks_after_load == 4; }), std::runtime_error);
    EXPECT_EQ(checks_after_load, 4);
    EXPECT_TRUE(payload.expired());
}

TEST(ArtworkLibraryEncoding, StreamedWriterExactPackageAndAggregateBoundaries)
{
    std::map<std::string, Bytes> input{{"a", {1}}, {"b", {2, 3}}};
    auto encoded = Archive::write(input);
    ArchiveLimits limits;
    limits.package_bytes = encoded.size();
    limits.total_bytes = 3;
    limits.entry_bytes = 2;
    limits.entries = 2;
    EXPECT_EQ(Archive::write(input, limits), encoded);
    --limits.package_bytes;
    EXPECT_THROW(Archive::write(input, limits), std::runtime_error);
    EXPECT_EQ(Archive::write_streamed({}).size(), 22);
}

TEST(ArtworkLibraryEncoding, NativeRoundTripPreservesMetadataRevisionOrderAndExactPayload)
{
    auto catalog = populated();
    catalog.rename(first, "Placa");
    auto snapshot = catalog.snapshot();
    auto encoded = encode_catalog(snapshot);
    EXPECT_EQ(encoded, encode_catalog(snapshot));
    auto reopened = Package::open(encoded);
    EXPECT_EQ(reopened.manifest().serialize(), snapshot.manifest().serialize());
    EXPECT_EQ(reopened.read_artwork(first), svg());
    EXPECT_EQ(reopened.read_artwork(second), bytes("second opaque payload"));
    EXPECT_EQ(snapshot.artwork_size(first), svg().size());
    EXPECT_EQ(reopened.manifest().assets.at(0).id, first);
    EXPECT_EQ(reopened.manifest().assets.at(1).id, second);
    auto loaded = Catalog::from_package(reopened);
    EXPECT_EQ(loaded.snapshot().artwork_size(first), svg().size());
    EXPECT_EQ(encode_catalog(loaded.snapshot()), encoded);
    EXPECT_NO_THROW(reopened.verify_artworks());
}

TEST(ArtworkLibraryEncoding, EmptyAndRemovedEntriesDoNotPretendToBeDurableTrash)
{
    auto c = Catalog::create(metadata());
    EXPECT_TRUE(Package::open(encode_catalog(c.snapshot())).manifest().assets.empty());
    c.add(art(), svg());
    auto original = c.snapshot();
    c.remove(first);
    auto removed = c.snapshot();
    EXPECT_THROW(removed.artwork_size(first), std::out_of_range);
    EXPECT_TRUE(Package::open(encode_catalog(removed)).manifest().assets.empty());
    EXPECT_EQ(Package::open(encode_catalog(original)).read_artwork(first), svg());
    EXPECT_TRUE(c.restore(first));
    EXPECT_EQ(Package::open(encode_catalog(c.snapshot())).read_artwork(first), svg());
}

TEST(ArtworkLibraryEncoding, IgnoresDisposablePreviewsWithoutInflatingOrTrustingThem)
{
    auto snapshot = populated().snapshot();
    auto m = snapshot.manifest();
    std::map<std::string, Bytes> files{{"library.json", bytes(m.serialize())}};
    for (auto const &a : m.assets) files[a.path] = *snapshot.read_artwork(a.id);
    auto const preview_path = std::string("previews/") + first + ".png";
    auto const bad_preview = bytes("not a PNG");
    files[preview_path] = bad_preview;
    auto wire = Archive::write(files);
    auto location = std::search(wire.begin(), wire.end(), bad_preview.begin(), bad_preview.end());
    ASSERT_NE(location, wire.end());
    *location ^= 1; // Leave both CRC fields unchanged: even a raw read must fail.
    auto package = Package::open(wire);
    EXPECT_THROW(package.read_preview(first), std::runtime_error);
    auto c = Catalog::from_package(std::move(package));
    auto result = Package::open(encode_catalog(c.snapshot()));
    EXPECT_FALSE(result.read_preview(first));
    EXPECT_EQ(result.read_artwork(first), svg());
    EXPECT_EQ(result.manifest().serialize(), m.serialize());
}

TEST(ArtworkLibraryEncoding, RejectsCorruptAuthoritativeArtworkWithoutChangingCatalog)
{
    auto snapshot = populated().snapshot();
    auto m = snapshot.manifest();
    std::map<std::string, Bytes> files{{"library.json", bytes(m.serialize())}};
    for (auto const &a : m.assets) files[a.path] = *snapshot.read_artwork(a.id);
    files[m.assets.back().path] = bytes("wrong hash, valid CRC");
    auto c = Catalog::from_package(Package::open(Archive::write(files)));
    auto before = c.snapshot().manifest().serialize();
    EXPECT_THROW(encode_catalog(c.snapshot()), std::runtime_error);
    EXPECT_EQ(c.snapshot().manifest().serialize(), before);
    EXPECT_EQ(*c.snapshot().read_artwork(first), svg());
}

TEST(ArtworkLibraryEncoding, PinsOriginalRevisionWhenCallerClosesOrEditsDuringCallbacks)
{
    auto c = populated();
    auto owner = std::make_unique<CatalogSnapshot>(c.snapshot());
    auto expected = owner->manifest().serialize();
    bool called = false;
    auto output = encode_catalog(*owner, {}, {}, [&] {
        if (!called) {
            called = true;
            owner.reset();
            c.rename(first, "newer revision");
            c.remove(second);
        }
        return false;
    });
    EXPECT_TRUE(called);
    auto result = Package::open(std::move(output));
    EXPECT_EQ(result.manifest().serialize(), expected);
    EXPECT_EQ(result.read_artwork(second), bytes("second opaque payload"));
    EXPECT_NE(c.snapshot().manifest().serialize(), expected);
}

TEST(ArtworkLibraryEncoding, CancellationAtEveryCheckpointLeavesSnapshotUnchanged)
{
    auto c = populated();
    auto snapshot = c.snapshot();
    auto original = snapshot.manifest().serialize();
    unsigned checkpoints = 0;
    auto expected = encode_catalog(snapshot, {}, {}, [&] { ++checkpoints; return false; });
    ASSERT_GT(checkpoints, 10);
    for (unsigned stop = 1; stop <= checkpoints; ++stop) {
        unsigned seen = 0;
        EXPECT_THROW(encode_catalog(snapshot, {}, {}, [&] { return ++seen == stop; }), std::runtime_error) << stop;
        EXPECT_EQ(snapshot.manifest().serialize(), original);
    }
    EXPECT_EQ(encode_catalog(snapshot), expected);
}

TEST(ArtworkLibraryEncoding, EncodingHonorsManifestAndArchiveLimits)
{
    auto snapshot = populated().snapshot();
    ManifestLimits manifest;
    manifest.assets = 1;
    EXPECT_THROW(encode_catalog(snapshot, {}, manifest), std::runtime_error);
    manifest = {};
    manifest.bytes = 10;
    EXPECT_THROW(encode_catalog(snapshot, {}, manifest), std::runtime_error);
    ArchiveLimits archive;
    archive.entries = 2;
    EXPECT_THROW(encode_catalog(snapshot, archive), std::runtime_error);
    archive = {};
    archive.package_bytes = 100;
    EXPECT_THROW(encode_catalog(snapshot, archive), std::runtime_error);
}
