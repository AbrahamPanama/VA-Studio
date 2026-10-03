// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "io/artwork-library-package.h"
#include <stdexcept>

using namespace Inkscape::IO::ArtworkLibrary;
namespace {
constexpr auto id = "b203e8e9-640c-4195-b0ea-f9b790245678";
constexpr auto second_id = "b203e8e9-640c-4195-b0ea-f9b790245679";
Bytes data(std::string const &value) { return {value.begin(), value.end()}; }
Bytes svg() { return data("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10mm\" height=\"20mm\" viewBox=\"0 0 10 20\"><path d=\"M0 0H10V20Z\"/></svg>"); }
Manifest catalog()
{
    Manifest manifest;
    manifest.id = "77bcd302-a034-4b99-9270-139e70273123";
    manifest.name = "Marcos 🌸";
    Asset asset;
    asset.id = id;
    asset.name = "Invitación";
    asset.path = "assets/" + asset.id + ".svg";
    asset.sha256 = artwork_sha256(svg());
    asset.width_mm = 10;
    asset.height_mm = 20;
    manifest.assets.push_back(asset);
    return manifest;
}
std::map<std::string, Bytes> files(Manifest const &manifest = catalog())
{
    std::map<std::string, Bytes> entries{{"library.json", data(manifest.serialize())}};
    for (auto const &asset : manifest.assets) entries[asset.path] = svg();
    return entries;
}
// Corrupt a stored entry without rewriting its CRC, at a position obtained
// from its local header. Open must remain lazy; reading the entry must fail.
void corrupt(Bytes &archive, std::string const &path)
{
    for (std::size_t at = 0; at + 30 < archive.size();) {
        if (archive[at] != 'P' || archive[at + 1] != 'K' || archive[at + 2] != 3) break;
        auto name_size = unsigned(archive[at + 26]) | (unsigned(archive[at + 27]) << 8);
        auto extra_size = unsigned(archive[at + 28]) | (unsigned(archive[at + 29]) << 8);
        unsigned size = 0;
        for (unsigned i = 0; i < 4; ++i) size |= unsigned(archive.at(at + 18 + i)) << (8 * i);
        std::string name(archive.begin() + at + 30, archive.begin() + at + 30 + name_size);
        auto payload = at + 30 + name_size + extra_size;
        if (name == path) {
            if (!size) throw std::runtime_error("Empty corruption fixture");
            archive.at(payload) ^= 1;
            return;
        }
        at = payload + size;
    }
    throw std::runtime_error("Missing corruption fixture entry");
}
}

TEST(ArtworkLibraryPackage, Sha256KnownVectorsAndCancellation)
{
    EXPECT_EQ(artwork_sha256({}), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(artwork_sha256(data("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_THROW(artwork_sha256({}, [] { return true; }), std::runtime_error);
    unsigned calls = 0;
    EXPECT_THROW(artwork_sha256(Bytes(1024 * 1024, 42), [&] { return ++calls == 4; }), std::runtime_error);
    EXPECT_EQ(calls, 4);
}

TEST(ArtworkLibraryPackage, MetadataAndIndependentImmutableCopies)
{
    auto source = Archive::write(files());
    auto original = source;
    auto package = Package::open(source);
    auto copy = package;
    EXPECT_EQ(source, original);
    EXPECT_EQ(package.manifest().name, "Marcos 🌸");
    EXPECT_EQ(package.asset(id).width_mm, 10);
    EXPECT_EQ(package.artwork_size(id), svg().size());
    EXPECT_FALSE(package.preview_size(id));
    auto bytes = package.read_artwork(id);
    EXPECT_EQ(bytes, svg());
    bytes[0] = '!';
    EXPECT_EQ(copy.read_artwork(id), svg());
    EXPECT_FALSE(package.read_preview(id));
    EXPECT_NO_THROW(package.verify_artworks());
    EXPECT_THROW(package.asset(second_id), std::out_of_range);
    EXPECT_THROW(package.read_artwork("../library.json"), std::out_of_range);
    EXPECT_THROW(package.read_preview(second_id), std::out_of_range);
    EXPECT_THROW(package.artwork_size(second_id), std::out_of_range);
    EXPECT_THROW(package.preview_size(second_id), std::out_of_range);
}

TEST(ArtworkLibraryPackage, SupportsAnEmptyCollection)
{
    auto manifest = catalog();
    manifest.assets.clear();
    auto package = Package::open(Archive::write(files(manifest)));
    EXPECT_TRUE(package.manifest().assets.empty());
    EXPECT_NO_THROW(package.verify_artworks());
    EXPECT_THROW(package.verify_artworks([] { return true; }), std::runtime_error);
}
TEST(ArtworkLibraryPackage, ExpansionAdmissionPrecedesManifestParsingAndKeepsFailureCharge) {
    Bytes malformed(8192, 'x');
    auto encoded = Archive::write({{"library.json", malformed}});
    std::size_t charged = 0; unsigned calls = 0;
    EXPECT_THROW(Package::open_with_admission(encoded, [&](std::size_t bytes) { charged += bytes; ++calls; }), std::runtime_error);
    EXPECT_EQ(charged, malformed.size()); EXPECT_EQ(calls, 1u);
    try {
        Package::open_with_admission(encoded, [](std::size_t) { throw std::runtime_error("Admission refused before parsing"); });
        FAIL() << "Denied admission unexpectedly opened a package";
    } catch (std::runtime_error const &e) { EXPECT_STREQ(e.what(), "Admission refused before parsing"); }
    calls = 0;
    EXPECT_THROW(Package::open_with_admission(Bytes{}, [&](std::size_t) { ++calls; }), std::runtime_error);
    EXPECT_EQ(calls, 0u); // Invalid ZIP metadata is rejected without entry inflation.
}

TEST(ArtworkLibraryPackage, RejectsMissingManifestMissingArtworkAndForeignPayloads)
{
    auto entries = files();
    entries.erase("library.json");
    EXPECT_THROW(Package::open(Archive::write(entries)), std::runtime_error);
    entries = files();
    entries.erase(catalog().assets[0].path);
    EXPECT_THROW(Package::open(Archive::write(entries)), std::runtime_error);
    for (auto const &path : {std::string("attachments/extra.svg"),
                           std::string("previews/") + second_id + ".png",
                           std::string("assets/") + second_id + ".svg"}) {
        entries = files();
        entries[path] = svg();
        EXPECT_THROW(Package::open(Archive::write(entries)), std::runtime_error) << path;
    }
}

TEST(ArtworkLibraryPackage, BindsHashRatherThanZipCrcOrPreview)
{
    auto entries = files();
    entries[catalog().assets[0].path] = data("different artwork, valid ZIP CRC");
    entries[std::string("previews/") + id + ".png"] = {1, 2, 3};
    auto package = Package::open(Archive::write(entries));
    EXPECT_THROW(package.read_artwork(id), std::runtime_error);
    EXPECT_THROW(package.verify_artworks(), std::runtime_error);
    EXPECT_EQ(package.read_preview(id), (std::optional<Bytes>{{1, 2, 3}}));
}

TEST(ArtworkLibraryPackage, OpeningDoesNotInflateArtworkOrDisposablePreviews)
{
    auto entries = files();
    auto preview = std::string("previews/") + id + ".png";
    entries[preview] = {1, 2, 3};
    auto bytes = Archive::write(entries);
    corrupt(bytes, catalog().assets[0].path);
    corrupt(bytes, preview);
    auto package = Package::open(std::move(bytes));
    EXPECT_EQ(package.manifest().assets.size(), 1);
    EXPECT_EQ(package.artwork_size(id), svg().size());
    EXPECT_EQ(package.preview_size(id), std::optional<std::size_t>(3));
    EXPECT_THROW(package.read_artwork(id), std::runtime_error);
    EXPECT_THROW(package.read_preview(id), std::runtime_error);
}

TEST(ArtworkLibraryPackage, CorruptPreviewDoesNotInvalidateAuthoritativeArtwork)
{
    auto entries = files();
    auto preview = std::string("previews/") + id + ".png";
    entries[preview] = {1, 2, 3};
    auto bytes = Archive::write(entries);
    corrupt(bytes, preview);
    auto package = Package::open(std::move(bytes));
    EXPECT_NO_THROW(package.verify_artworks());
    EXPECT_THROW(package.read_preview(id), std::runtime_error);
}

TEST(ArtworkLibraryPackage, MalformedManifestCannotMasqueradeAsLibrary)
{
    auto entries = files();
    for (auto const &bad : {"", "{}", "<LightBurnShapes/>", "not JSON"}) {
        entries["library.json"] = data(bad);
        EXPECT_THROW(Package::open(Archive::write(entries)), std::runtime_error);
    }
    auto bytes = Archive::write(files());
    corrupt(bytes, "library.json");
    EXPECT_THROW(Package::open(std::move(bytes)), std::runtime_error);
}

TEST(ArtworkLibraryPackage, HonorsExplicitBudgetsAndCancellation)
{
    auto bytes = Archive::write(files());
    ArchiveLimits archive_limits;
    archive_limits.package_bytes = bytes.size() - 1;
    EXPECT_THROW(Package::open(bytes, archive_limits), std::runtime_error);
    ManifestLimits manifest_limits;
    manifest_limits.assets = 0;
    EXPECT_THROW(Package::open(bytes, {}, manifest_limits), std::runtime_error);
    manifest_limits = {};
    manifest_limits.bytes = catalog().serialize().size() - 1;
    EXPECT_THROW(Package::open(bytes, {}, manifest_limits), std::runtime_error);
    EXPECT_THROW(Package::open(bytes, {}, {}, [] { return true; }), std::runtime_error);
    auto package = Package::open(std::move(bytes));
    EXPECT_THROW(package.read_artwork(id, [] { return true; }), std::runtime_error);
    EXPECT_THROW(package.read_preview(id, [] { return true; }), std::runtime_error);
    EXPECT_THROW(package.verify_artworks([] { return true; }), std::runtime_error);
}

TEST(ArtworkLibraryPackage, VerifiesEveryAssetIncludingLaterFailures)
{
    auto manifest = catalog();
    auto second = manifest.assets.front();
    second.id = second_id;
    second.path = "assets/" + second.id + ".svg";
    manifest.assets.push_back(second);
    auto entries = files(manifest);
    entries[second.path] = data("corrupt but correctly checksummed ZIP entry");
    auto package = Package::open(Archive::write(entries));
    EXPECT_EQ(package.read_artwork(id), svg());
    EXPECT_THROW(package.verify_artworks(), std::runtime_error);
}

TEST(ArtworkLibraryPackage, HashIntegrityDoesNotClaimSvgSafety)
{
    // The later SVG importer must reject unsafe artwork. This layer must not
    // claim that a checksum authenticates content or makes it safe to render.
    for (auto const &payload : {std::string("not SVG"),
        std::string("<svg><script>alert(1)</script></svg>"),
        std::string("<svg><image href=\"https://invalid.example/image.png\"/></svg>")}) {
        auto manifest = catalog();
        manifest.assets[0].sha256 = artwork_sha256(data(payload));
        auto entries = files(manifest);
        entries[manifest.assets[0].path] = data(payload);
        auto package = Package::open(Archive::write(entries));
        EXPECT_EQ(package.read_artwork(id), data(payload));
    }
}

TEST(ArtworkLibraryPackage, CancelledPayloadReadLeavesSnapshotUsable)
{
    auto manifest = catalog();
    Bytes payload(1024 * 1024, 42);
    manifest.assets[0].sha256 = artwork_sha256(payload);
    auto entries = files(manifest);
    entries[manifest.assets[0].path] = payload;
    auto package = Package::open(Archive::write(entries));
    unsigned calls = 0;
    EXPECT_THROW(package.read_artwork(id, [&] { return ++calls == 6; }), std::runtime_error);
    EXPECT_EQ(calls, 6);
    EXPECT_EQ(package.read_artwork(id), payload);
}

TEST(ArtworkLibraryPackage, CallbackCanReplaceOrClosePackageWithoutChangingInflightRead)
{
    auto entries = files();
    auto preview = std::string("previews/") + id + ".png";
    entries[preview] = {1, 2, 3};
    for (bool read_preview : {false, true}) {
        auto package = std::make_unique<Package>(Package::open(Archive::write(entries)));
        unsigned calls = 0;
        auto close = [&] { if (++calls == 1) package.reset(); return false; };
        if (read_preview) {
            EXPECT_EQ(package->read_preview(id, close), (std::optional<Bytes>{{1, 2, 3}}));
        } else {
            EXPECT_EQ(package->read_artwork(id, close), svg());
        }
        EXPECT_FALSE(package);
    }
    auto package = Package::open(Archive::write(entries));
    auto empty = catalog();
    empty.assets.clear();
    bool changed = false;
    EXPECT_EQ(package.read_artwork(id, [&] {
        if (!changed) {
            changed = true;
            package = Package::open(Archive::write(files(empty)));
        }
        return false;
    }), svg());
    EXPECT_TRUE(package.manifest().assets.empty());

    entries[catalog().assets[0].path] = data("wrong hash");
    auto owner = std::make_unique<Package>(Package::open(Archive::write(entries)));
    auto const &borrowed_id = owner->manifest().assets[0].id;
    try {
        owner->read_artwork(borrowed_id, [&] { owner.reset(); return false; });
        FAIL() << "Corrupted artwork must fail even after closing its owner";
    } catch (std::runtime_error const &error) {
        EXPECT_EQ(error.what(), std::string("Artwork content hash mismatch: ") + id);
    }
    EXPECT_FALSE(owner);
}

TEST(ArtworkLibraryPackage, VerificationPinsOriginalInventoryAcrossOwnerReplacement)
{
    auto manifest = catalog();
    auto second = manifest.assets.front();
    second.id = second_id;
    second.path = "assets/" + second.id + ".svg";
    manifest.assets.push_back(second);
    auto entries = files(manifest);
    entries[second.path] = data("valid ZIP CRC but wrong artwork hash");
    auto package = Package::open(Archive::write(entries));
    manifest.assets.clear();
    bool replaced = false;
    EXPECT_THROW(package.verify_artworks([&] {
        if (!replaced) {
            replaced = true;
            package = Package::open(Archive::write(files(manifest)));
        }
        return false;
    }), std::runtime_error);
    EXPECT_TRUE(package.manifest().assets.empty());
}
