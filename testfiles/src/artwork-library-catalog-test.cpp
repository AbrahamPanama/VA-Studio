// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <glib.h>

#include "io/artwork-library-catalog.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace Inkscape::IO::ArtworkLibrary;
namespace {
std::string identifier(unsigned value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic()); // UUID hex digits must never acquire locale grouping.
    out << "b203e8e9-640c-4195-b0ea-" << std::hex << std::setw(12) << std::setfill('0') << value;
    return out.str();
}

Manifest empty_manifest()
{
    Manifest manifest;
    manifest.id = "77bcd302-a034-4b99-9270-139e70273123";
    manifest.name = "Colección 🌸";
    manifest.extra_json = R"({"future":{"source":"fixture","value":0.125}})";
    return manifest;
}

NewArtwork artwork(unsigned number, std::string name = "Marco")
{
    NewArtwork value;
    value.id = identifier(number);
    value.name = std::move(name);
    value.tags = {"boda", "中文"};
    value.width_mm = 12.5;
    value.height_mm = 27.25;
    value.extra_json = R"({"importDiagnostics":["not SVG-validated"],"future":{"kept":true}})";
    return value;
}

Bytes payload() { return {0, 255, 'n', 'o', 't', '-', 's', 'v', 'g'}; }

std::map<std::string, Bytes> package_files(unsigned count = 2, std::uint64_t revision = 7)
{
    auto manifest = empty_manifest();
    manifest.revision = revision;
    std::map<std::string, Bytes> files;
    for (unsigned i = 0; i < count; ++i) {
        Asset asset;
        auto meta = artwork(i, "Marco Straße " + std::to_string(i));
        asset.id = meta.id;
        asset.name = meta.name;
        asset.tags = meta.tags;
        asset.path = "assets/" + asset.id + ".svg";
        asset.sha256 = artwork_sha256(payload());
        asset.width_mm = meta.width_mm;
        asset.height_mm = meta.height_mm;
        asset.extra_json = meta.extra_json;
        files.emplace(asset.path, payload());
        manifest.assets.push_back(std::move(asset));
    }
    auto json = manifest.serialize();
    files.emplace("library.json", Bytes(json.begin(), json.end()));
    return files;
}

Package package(unsigned count = 2, std::uint64_t revision = 7)
{
    return Package::open(Archive::write(package_files(count, revision)));
}

std::string state(Catalog const &catalog) { return catalog.snapshot().manifest().serialize(); }

void expect_unchanged(Catalog const &catalog, CatalogSnapshot const &before)
{
    EXPECT_EQ(state(catalog), before.manifest().serialize());
    EXPECT_EQ(catalog.snapshot().list(), before.list());
    EXPECT_EQ(catalog.snapshot().removed_ids(), before.removed_ids());
    EXPECT_EQ(catalog.snapshot().overlay_bytes(), before.overlay_bytes());
    EXPECT_EQ(catalog.snapshot().prior_package(), before.prior_package());
}
} // namespace

TEST(ArtworkLibraryCatalog, NewCatalogRequiresEmptyRevisionZeroAndPreservesLibraryMetadata)
{
    auto metadata = empty_manifest();
    auto catalog = Catalog::create(metadata);
    EXPECT_TRUE(catalog.snapshot().list().empty());
    EXPECT_TRUE(catalog.snapshot().search(std::string_view{}).empty());
    EXPECT_FALSE(catalog.snapshot().prior_package());
    EXPECT_EQ(catalog.snapshot().manifest().extra_json, metadata.extra_json);
    EXPECT_EQ(catalog.snapshot().manifest().revision, 0);
    EXPECT_FALSE(catalog.rename_library(metadata.name));
    EXPECT_TRUE(catalog.rename_library("Nueva colección"));
    EXPECT_EQ(catalog.snapshot().manifest().revision, 1);
    metadata.revision = 1;
    EXPECT_THROW(Catalog::create(metadata), std::runtime_error);
    EXPECT_THROW(Catalog::create(package().manifest()), std::runtime_error);
    EXPECT_THROW(Catalog::create(empty_manifest(), {}, [] { return true; }), std::runtime_error);
}

TEST(ArtworkLibraryCatalog, BasePackageRemainsAnIndependentRecoverySnapshot)
{
    auto original = package();
    auto original_manifest = original.manifest().serialize();
    auto catalog = Catalog::from_package(original);
    auto before = catalog.snapshot();
    ASSERT_TRUE(before.prior_package());
    EXPECT_EQ(before.list(), (std::vector<std::string>{identifier(0), identifier(1)}));
    EXPECT_TRUE(catalog.rename(identifier(0), "Renamed"));
    EXPECT_TRUE(catalog.set_tags(identifier(0), {"edited"}));
    EXPECT_TRUE(catalog.remove(identifier(1)));
    EXPECT_EQ(catalog.snapshot().manifest().revision, 10);
    EXPECT_EQ(before.manifest().serialize(), original_manifest);
    EXPECT_EQ(original.manifest().serialize(), original_manifest);
    EXPECT_EQ(catalog.snapshot().prior_package()->manifest().serialize(), original_manifest);
    EXPECT_EQ(*before.read_artwork(identifier(1)), payload());
    EXPECT_EQ(catalog.snapshot().asset(identifier(0)).extra_json, original.asset(identifier(0)).extra_json);
    EXPECT_EQ(catalog.snapshot().manifest().extra_json, original.manifest().extra_json);
}

TEST(ArtworkLibraryCatalog, AddedPayloadIsUnvalidatedAndSharedAcrossMetadataEditsAndSnapshots)
{
    auto catalog = Catalog::create(empty_manifest());
    auto bytes = payload();
    EXPECT_EQ(catalog.add(artwork(1), bytes), identifier(1));
    auto before = catalog.snapshot();
    auto handle = before.read_artwork(identifier(1));
    ASSERT_TRUE(handle);
    EXPECT_EQ(*handle, payload()); // Deliberately not XML: integrity is NOT SVG safety.
    bytes[0] = 42;
    EXPECT_EQ(*handle, payload());
    EXPECT_EQ(before.asset(identifier(1)).sha256, artwork_sha256(payload()));
    EXPECT_EQ(before.asset(identifier(1)).path, "assets/" + identifier(1) + ".svg");
    EXPECT_FALSE(before.read_preview(identifier(1)));
    EXPECT_TRUE(catalog.rename(identifier(1), "Nuevo"));
    EXPECT_TRUE(catalog.set_tags(identifier(1), {"nuevo"}));
    EXPECT_EQ(catalog.snapshot().read_artwork(identifier(1)).get(), handle.get());
    EXPECT_EQ(before.asset(identifier(1)).name, "Marco");
    EXPECT_EQ(before.manifest().revision, 1);
    EXPECT_EQ(catalog.snapshot().manifest().revision, 3);
    auto generated = artwork(2);
    generated.id.clear();
    auto id = catalog.add(generated, {});
    EXPECT_TRUE(valid_library_uuid(id));
    EXPECT_TRUE(catalog.snapshot().read_artwork(id)->empty());
}

TEST(ArtworkLibraryCatalog, UnicodeCasefoldCanonicalEquivalenceAndTermSearchRemainOrdered)
{
    auto catalog = Catalog::create(empty_manifest());
    auto first = artwork(1, "Straße Café 🌸");
    first.tags = {"BODA", "中文", "ΑΘΉΝΑ"};
    catalog.add(first, payload());
    catalog.add(artwork(2, "Other"), payload());
    EXPECT_EQ(catalog.snapshot().search("STRASSE cafe\xcc\x81"), (std::vector<std::string>{identifier(1)}));
    EXPECT_EQ(catalog.snapshot().search("🌸\xe3\x80\x80" "boda"), (std::vector<std::string>{identifier(1)}));
    EXPECT_EQ(catalog.snapshot().search("αθήνα"), (std::vector<std::string>{identifier(1)}));
    EXPECT_EQ(catalog.snapshot().search("中文"), catalog.snapshot().list());
    EXPECT_EQ(catalog.snapshot().search(" \t\n"), catalog.snapshot().list());
    EXPECT_TRUE(catalog.snapshot().search("unknown").empty());
    auto old = catalog.snapshot();
    catalog.rename(identifier(1), "Replaced");
    catalog.set_tags(identifier(1), {"newtag"});
    EXPECT_TRUE(catalog.snapshot().search("strasse").empty());
    EXPECT_TRUE(catalog.snapshot().search("αθήνα").empty());
    EXPECT_EQ(old.search("strasse"), (std::vector<std::string>{identifier(1)}));
    EXPECT_THROW(catalog.snapshot().search(std::string(4097, 'a')), std::runtime_error);
    EXPECT_THROW(catalog.snapshot().search(std::string(1, char(0xff))), std::runtime_error);
    EXPECT_THROW(catalog.snapshot().search(std::string("a\0b", 3)), std::runtime_error);
}

TEST(ArtworkLibraryCatalog, DuplicateNamesAreSuffixedWithoutOverwritingStableIds)
{
    auto catalog = Catalog::create(empty_manifest());
    catalog.add(artwork(1, "Straße"), payload());
    catalog.add(artwork(2, "STRASSE"), payload());
    catalog.add(artwork(3, "Straße"), payload());
    EXPECT_EQ(catalog.snapshot().asset(identifier(1)).name, "Straße");
    EXPECT_EQ(catalog.snapshot().asset(identifier(2)).name, "STRASSE (2)");
    EXPECT_EQ(catalog.snapshot().asset(identifier(3)).name, "Straße (3)");
    auto before = catalog.snapshot();
    EXPECT_THROW(catalog.add(artwork(1, "different"), payload()), std::runtime_error);
    expect_unchanged(catalog, before);
    catalog.remove(identifier(1));
    EXPECT_THROW(catalog.add(artwork(1), payload()), std::runtime_error);
    catalog.add(artwork(4, "Straße"), payload());
    EXPECT_TRUE(catalog.restore(identifier(1)));
    EXPECT_EQ(catalog.snapshot().asset(identifier(1)).name, "Straße (4)");
    EXPECT_EQ(catalog.snapshot().list(), (std::vector<std::string>{identifier(1), identifier(2), identifier(3), identifier(4)}));
}

TEST(ArtworkLibraryCatalog, LongDuplicateNamesKeepSuffixAndValidUtf8)
{
    std::string name;
    for (unsigned i = 0; i < 256; ++i) name += "🌸";
    ASSERT_EQ(name.size(), 1024);
    auto catalog = Catalog::create(empty_manifest());
    catalog.add(artwork(1, name), payload());
    catalog.add(artwork(2, name), payload());
    auto suffixed = catalog.snapshot().asset(identifier(2)).name;
    EXPECT_LE(suffixed.size(), 1024);
    EXPECT_TRUE(g_utf8_validate(suffixed.data(), suffixed.size(), nullptr));
    EXPECT_EQ(suffixed.substr(suffixed.size() - 4), " (2)");
}

TEST(ArtworkLibraryCatalog, SessionTrashRestoresOrderMetadataAndPayloadAndNoopsDoNotIncrement)
{
    auto catalog = Catalog::from_package(package(3));
    auto before = catalog.snapshot();
    EXPECT_FALSE(catalog.rename(identifier(0), before.asset(identifier(0)).name));
    EXPECT_FALSE(catalog.set_tags(identifier(0), before.asset(identifier(0)).tags));
    EXPECT_FALSE(catalog.restore(identifier(0)));
    expect_unchanged(catalog, before);
    EXPECT_TRUE(catalog.remove(identifier(1)));
    EXPECT_FALSE(catalog.remove(identifier(1)));
    EXPECT_EQ(catalog.snapshot().manifest().revision, 8);
    EXPECT_EQ(catalog.snapshot().removed_ids(), (std::vector<std::string>{identifier(1)}));
    EXPECT_THROW(catalog.snapshot().asset(identifier(1)), std::out_of_range);
    EXPECT_THROW(catalog.snapshot().read_artwork(identifier(1)), std::out_of_range);
    EXPECT_THROW(catalog.snapshot().read_preview(identifier(1)), std::out_of_range);
    EXPECT_TRUE(catalog.restore(identifier(1)));
    EXPECT_EQ(catalog.snapshot().manifest().revision, 9);
    EXPECT_EQ(catalog.snapshot().list(), before.list());
    EXPECT_EQ(catalog.snapshot().asset(identifier(1)).extra_json, before.asset(identifier(1)).extra_json);
    EXPECT_EQ(*catalog.snapshot().read_artwork(identifier(1)), payload());
    EXPECT_TRUE(catalog.snapshot().removed_ids().empty());
}

TEST(ArtworkLibraryCatalog, OverlayBudgetIncludesTrashAndFailureDoesNotConsumeIt)
{
    CatalogLimits limits;
    limits.artwork_bytes = payload().size();
    limits.overlay_bytes = payload().size();
    auto catalog = Catalog::create(empty_manifest(), limits);
    catalog.add(artwork(1), payload());
    auto handle = catalog.snapshot().read_artwork(identifier(1));
    catalog.remove(identifier(1));
    auto before = catalog.snapshot();
    EXPECT_EQ(before.overlay_bytes(), payload().size());
    EXPECT_THROW(catalog.add(artwork(2), payload()), std::runtime_error);
    expect_unchanged(catalog, before);
    EXPECT_TRUE(catalog.restore(identifier(1)));
    EXPECT_EQ(catalog.snapshot().read_artwork(identifier(1)).get(), handle.get());
    auto too_large = payload();
    too_large.push_back(0);
    EXPECT_THROW(catalog.add(artwork(3), too_large), std::runtime_error);
}

TEST(ArtworkLibraryCatalog, ActiveAndRetainedEntryBudgetsApplyToRestoreAndAdd)
{
    CatalogLimits limits;
    limits.manifest.assets = 1;
    limits.retained_assets = 2;
    auto catalog = Catalog::create(empty_manifest(), limits);
    catalog.add(artwork(1), payload());
    auto before = catalog.snapshot();
    EXPECT_THROW(catalog.add(artwork(2), payload()), std::runtime_error);
    expect_unchanged(catalog, before);
    catalog.remove(identifier(1));
    catalog.add(artwork(2), payload());
    before = catalog.snapshot();
    EXPECT_THROW(catalog.restore(identifier(1)), std::runtime_error);
    expect_unchanged(catalog, before);
    catalog.remove(identifier(2));
    before = catalog.snapshot();
    EXPECT_THROW(catalog.add(artwork(3), payload()), std::runtime_error);
    expect_unchanged(catalog, before);
    EXPECT_TRUE(catalog.restore(identifier(1)));
}

TEST(ArtworkLibraryCatalog, BaseSizeChecksAreMetadataOnlyAndPrecedeLazyArtworkAndPreviewReads)
{
    auto entries = package_files(1);
    auto artwork_path = "assets/" + identifier(0) + ".svg";
    auto preview_path = "previews/" + identifier(0) + ".png";
    entries[artwork_path] = Bytes(100, 42); // Valid ZIP CRC, deliberately incorrect manifest hash.
    entries[preview_path] = Bytes(20, 255);
    auto base = Package::open(Archive::write(entries));
    CatalogLimits limits;
    limits.artwork_bytes = 99;
    EXPECT_THROW(Catalog::from_package(base, limits), std::runtime_error);
    limits.artwork_bytes = 100;
    limits.preview_bytes = 19;
    auto catalog = Catalog::from_package(base, limits); // No artwork hash or preview decode at open.
    EXPECT_EQ(catalog.snapshot().list(), (std::vector<std::string>{identifier(0)}));
    EXPECT_THROW(catalog.snapshot().read_artwork(identifier(0)), std::runtime_error);
    EXPECT_THROW(catalog.snapshot().read_preview(identifier(0)), std::runtime_error);
    limits.preview_bytes = 20;
    auto preview_catalog = Catalog::from_package(base, limits);
    EXPECT_EQ(preview_catalog.snapshot().read_preview(identifier(0)), std::optional<Bytes>(Bytes(20, 255)));
}

TEST(ArtworkLibraryCatalog, InvalidMetadataAndUnknownIdsLeaveAllStateUntouched)
{
    auto catalog = Catalog::from_package(package());
    auto before = catalog.snapshot();
    EXPECT_THROW(catalog.rename(identifier(0), ""), std::runtime_error);
    EXPECT_THROW(catalog.rename_library(""), std::runtime_error);
    EXPECT_THROW(catalog.rename(identifier(0), std::string(1, char(0xff))), std::runtime_error);
    EXPECT_THROW(catalog.set_tags(identifier(0), {"duplicate", "duplicate"}), std::runtime_error);
    EXPECT_THROW(catalog.set_tags(identifier(0), std::vector<std::string>(33, "tag")), std::runtime_error);
    auto bad = artwork(20);
    bad.extra_json = R"({"path":"../outside"})";
    EXPECT_THROW(catalog.add(bad, payload()), std::runtime_error);
    bad = artwork(20);
    bad.width_mm = std::numeric_limits<double>::infinity();
    EXPECT_THROW(catalog.add(bad, payload()), std::runtime_error);
    EXPECT_THROW(catalog.remove(identifier(99)), std::out_of_range);
    EXPECT_THROW(catalog.restore(identifier(99)), std::out_of_range);
    expect_unchanged(catalog, before);
    CatalogLimits small;
    small.metadata_bytes = 1;
    EXPECT_THROW(Catalog::create(empty_manifest(), small), std::runtime_error);
    EXPECT_THROW(Catalog::from_package(package(), small), std::runtime_error);
}

TEST(ArtworkLibraryCatalog, RevisionOverflowRejectsChangesButPermitsTrueNoops)
{
    auto catalog = Catalog::from_package(package(1, std::numeric_limits<std::uint64_t>::max()));
    auto before = catalog.snapshot();
    EXPECT_FALSE(catalog.rename(identifier(0), before.asset(identifier(0)).name));
    EXPECT_FALSE(catalog.set_tags(identifier(0), before.asset(identifier(0)).tags));
    EXPECT_FALSE(catalog.restore(identifier(0)));
    EXPECT_THROW(catalog.rename(identifier(0), "changed"), std::runtime_error);
    EXPECT_THROW(catalog.rename_library("changed"), std::runtime_error);
    EXPECT_THROW(catalog.set_tags(identifier(0), {"changed"}), std::runtime_error);
    EXPECT_THROW(catalog.remove(identifier(0)), std::runtime_error);
    EXPECT_THROW(catalog.add(artwork(2), payload()), std::runtime_error);
    expect_unchanged(catalog, before);
    auto last = Catalog::from_package(package(1, std::numeric_limits<std::uint64_t>::max() - 1));
    EXPECT_TRUE(last.remove(identifier(0)));
    before = last.snapshot();
    EXPECT_THROW(last.restore(identifier(0)), std::runtime_error);
    expect_unchanged(last, before);
}

TEST(ArtworkLibraryCatalog, CancellationAtEarlyMiddleAndFinalEditCheckpointsIsTransactional)
{
    using Edit = std::function<void(Catalog &, Cancelled)>;
    std::vector<Edit> edits = {
        [](Catalog &c, Cancelled cancel) { c.add(artwork(10), payload(), cancel); },
        [](Catalog &c, Cancelled cancel) { c.rename(identifier(0), "changed", cancel); },
        [](Catalog &c, Cancelled cancel) { c.set_tags(identifier(0), {"changed"}, cancel); },
        [](Catalog &c, Cancelled cancel) { c.rename_library("changed", cancel); },
        [](Catalog &c, Cancelled cancel) { c.remove(identifier(0), cancel); },
        [](Catalog &c, Cancelled cancel) { c.restore(identifier(1), cancel); }
    };
    auto initial = Catalog::from_package(package());
    initial.remove(identifier(1));
    for (std::size_t e = 0; e < edits.size(); ++e) {
        SCOPED_TRACE(e);
        auto probe = initial;
        unsigned checkpoints = 0;
        edits[e](probe, [&] { ++checkpoints; return false; });
        ASSERT_GT(checkpoints, 2);
        for (unsigned stop : {1u, 2u, checkpoints / 2, checkpoints - 1, checkpoints}) {
            SCOPED_TRACE(stop);
            auto subject = initial;
            auto before = subject.snapshot();
            unsigned calls = 0;
            EXPECT_THROW(edits[e](subject, [&] { return ++calls == stop; }), std::runtime_error);
            expect_unchanged(subject, before);
        }
    }
}

TEST(ArtworkLibraryCatalog, CancelledReadsAndSearchDoNotPoisonSnapshot)
{
    auto catalog = Catalog::from_package(package());
    auto snapshot = catalog.snapshot();
    auto cancel = [] { return true; };
    EXPECT_THROW(snapshot.list(cancel), std::runtime_error);
    EXPECT_THROW(snapshot.search("marco", cancel), std::runtime_error);
    EXPECT_THROW(snapshot.manifest(cancel), std::runtime_error);
    EXPECT_THROW(snapshot.removed_ids(cancel), std::runtime_error);
    EXPECT_THROW(snapshot.read_artwork(identifier(0), cancel), std::runtime_error);
    EXPECT_THROW(snapshot.read_preview(identifier(0), cancel), std::runtime_error);
    unsigned calls = 0;
    EXPECT_THROW(snapshot.read_artwork(identifier(0), [&] { return ++calls == 4; }), std::runtime_error);
    EXPECT_EQ(*snapshot.read_artwork(identifier(0)), payload());
    EXPECT_EQ(snapshot.search("boda"), snapshot.list());
    expect_unchanged(catalog, snapshot);
}

TEST(ArtworkLibraryCatalog, CancelledLargePayloadStagingAndMetadataBudgetsLeaveNoOverlay)
{
    auto catalog = Catalog::create(empty_manifest());
    auto before = catalog.snapshot();
    Bytes bytes(1024 * 1024, 42);
    unsigned calls = 0;
    // With no existing slots, these checks fall inside the chunked detached copy.
    EXPECT_THROW(catalog.add(artwork(1), bytes, [&] { return ++calls == 5; }), std::runtime_error);
    expect_unchanged(catalog, before);
    EXPECT_EQ(bytes.front(), 42);
    EXPECT_EQ(bytes.back(), 42);
    auto added = catalog.add(artwork(1), bytes);
    EXPECT_EQ(catalog.snapshot().read_artwork(added)->size(), bytes.size());
    EXPECT_EQ(catalog.snapshot().asset(added).sha256, artwork_sha256(bytes));

    CatalogLimits limits;
    limits.metadata_bytes = 1024;
    auto bounded = Catalog::create(empty_manifest(), limits);
    bounded.add(artwork(1), payload());
    auto saved = bounded.snapshot();
    auto large = artwork(2);
    large.extra_json = "{\"value\":\"" + std::string(900, 'x') + "\"}";
    EXPECT_THROW(bounded.add(large, payload()), std::runtime_error);
    expect_unchanged(bounded, saved);
    EXPECT_THROW(bounded.rename(identifier(1), std::string(1024, 'a')), std::runtime_error);
    expect_unchanged(bounded, saved);
}

TEST(ArtworkLibraryCatalog, ReentrantCallbackCannotPublishAnEditOverNewerCatalogState)
{
    auto catalog = Catalog::from_package(package());
    bool once = false;
    EXPECT_THROW(catalog.rename(identifier(0), "outer edit", [&] {
        if (!once) {
            once = true;
            catalog.rename_library("inner edit");
        }
        return false;
    }), std::runtime_error);
    EXPECT_EQ(catalog.snapshot().manifest().name, "inner edit");
    EXPECT_EQ(catalog.snapshot().manifest().revision, 8);
    EXPECT_EQ(catalog.snapshot().asset(identifier(0)).name, "Marco Straße 0");
}

TEST(ArtworkLibraryCatalog, EverySnapshotReaderPinsStateAcrossReassignmentAndOwnerDestruction)
{
    using Reader = std::function<void(CatalogSnapshot const &, Cancelled)>;
    std::vector<Reader> readers = {
        [](CatalogSnapshot const &s, Cancelled cancel) {
            auto m = s.manifest(cancel);
            EXPECT_EQ(m.name, empty_manifest().name);
            EXPECT_EQ(m.revision, 8);
            ASSERT_EQ(m.assets.size(), 2);
            EXPECT_EQ(m.assets.front().id, identifier(0));
        },
        [](CatalogSnapshot const &s, Cancelled cancel) {
            EXPECT_EQ(s.list(cancel), (std::vector<std::string>{identifier(0), identifier(2)}));
        },
        [](CatalogSnapshot const &s, Cancelled cancel) {
            EXPECT_EQ(s.search("STRASSE BODA", cancel), (std::vector<std::string>{identifier(0), identifier(2)}));
        },
        [](CatalogSnapshot const &s, Cancelled cancel) {
            EXPECT_EQ(s.removed_ids(cancel), (std::vector<std::string>{identifier(1)}));
        },
        [](CatalogSnapshot const &s, Cancelled cancel) {
            EXPECT_EQ(*s.read_artwork(identifier(0), cancel), payload());
        },
        [](CatalogSnapshot const &s, Cancelled cancel) {
            EXPECT_EQ(s.read_preview(identifier(0), cancel), std::optional<Bytes>(Bytes{1, 2, 3}));
        }
    };
    auto entries = package_files(3);
    entries["previews/" + identifier(0) + ".png"] = {1, 2, 3};
    auto base = Package::open(Archive::write(entries));
    for (std::size_t r = 0; r < readers.size(); ++r) {
        SCOPED_TRACE(r);
        auto probe = Catalog::from_package(base);
        probe.remove(identifier(1));
        unsigned checkpoints = 0;
        readers[r](probe.snapshot(), [&] { ++checkpoints; return false; });
        ASSERT_GT(checkpoints, 0);
        for (bool destroy : {false, true}) {
            SCOPED_TRACE(destroy);
            for (unsigned stop : {1u, std::max(1u, checkpoints / 2), checkpoints}) {
                SCOPED_TRACE(stop);
                auto catalog = Catalog::from_package(base);
                catalog.remove(identifier(1));
                auto owner = std::make_unique<CatalogSnapshot>(catalog.snapshot());
                // Only owner retains the old CatalogState. The replacement has
                // different membership/name, so reading the new state also fails.
                catalog.remove(identifier(0));
                catalog.rename_library("replacement");
                unsigned calls = 0;
                EXPECT_NO_THROW(readers[r](*owner, [&] {
                    if (++calls == stop) {
                        if (destroy) owner.reset();
                        else *owner = catalog.snapshot();
                    }
                    return false;
                }));
                EXPECT_GE(calls, stop);
                if (destroy) EXPECT_FALSE(owner);
                else EXPECT_EQ(owner->manifest().name, "replacement");
            }
        }
    }
}

TEST(ArtworkLibraryCatalog, AssetValueAndPayloadHandleSurviveAllOwners)
{
    static_assert(std::is_same_v<decltype(std::declval<CatalogSnapshot const &>().asset("id")), Asset>);
    Asset retained;
    std::shared_ptr<Bytes const> bytes;
    {
        auto catalog = Catalog::create(empty_manifest());
        catalog.add(artwork(1), payload());
        retained = catalog.snapshot().asset(identifier(1));
        bytes = catalog.snapshot().read_artwork(identifier(1));
        catalog.rename(identifier(1), "changed");
        catalog.remove(identifier(1));
    }
    EXPECT_EQ(retained.name, "Marco");
    EXPECT_EQ(retained.extra_json, artwork(1).extra_json);
    ASSERT_TRUE(bytes);
    EXPECT_EQ(*bytes, payload());
}

TEST(ArtworkLibraryCatalog, SpareCallerCapacitiesAreNotRetainedByHeadersOrNestedAssetMetadata)
{
    constexpr std::size_t excess = 1024 * 1024;
    CatalogLimits limits;
    limits.metadata_bytes = 2048;
    auto metadata = empty_manifest();
    metadata.id.reserve(excess);
    metadata.name.reserve(excess);
    metadata.extra_json.reserve(excess);
    metadata.assets.reserve(4096); // Empty content must not carry this allocation into the header.
    auto catalog = Catalog::create(std::move(metadata), limits);
    EXPECT_LE(catalog.snapshot().retained_metadata_bytes(), limits.metadata_bytes);

    auto added = artwork(1);
    added.id.reserve(excess);
    added.name.reserve(excess);
    added.extra_json.reserve(excess);
    added.tags.reserve(4096);
    for (auto &tag : added.tags) tag.reserve(excess);
    EXPECT_NO_THROW(catalog.add(std::move(added), payload()));
    auto measured = catalog.snapshot().retained_metadata_bytes(); // Measures actual capacities, not cached string lengths.
    EXPECT_LE(measured, limits.metadata_bytes);
    EXPECT_EQ(catalog.snapshot().asset(identifier(1)).extra_json, artwork(1).extra_json);

    std::string library_name = "renamed library";
    library_name.reserve(excess);
    EXPECT_TRUE(catalog.rename_library(std::move(library_name)));
    std::vector<std::string> tags{"replacement", "中文"};
    tags.reserve(4096);
    for (auto &tag : tags) tag.reserve(excess);
    EXPECT_TRUE(catalog.set_tags(identifier(1), std::move(tags)));
    EXPECT_LE(catalog.snapshot().retained_metadata_bytes(), limits.metadata_bytes);
    auto before_remove = catalog.snapshot().retained_metadata_bytes();
    catalog.remove(identifier(1));
    EXPECT_EQ(catalog.snapshot().retained_metadata_bytes(), before_remove); // Trash remains charged.
    EXPECT_TRUE(catalog.restore(identifier(1)));
    EXPECT_LE(catalog.snapshot().retained_metadata_bytes(), limits.metadata_bytes);
}

TEST(ArtworkLibraryCatalog, StagingSnapshotBindsManifestAndPayloadsAfterLaterEdits)
{
    auto catalog = Catalog::from_package(package(1));
    catalog.add(artwork(3), payload());
    auto staged = catalog.snapshot();
    catalog.rename(identifier(3), "later edit");
    catalog.remove(identifier(0));
    auto manifest = staged.manifest();
    auto serialized = manifest.serialize();
    std::map<std::string, Bytes> files{{"library.json", Bytes(serialized.begin(), serialized.end())}};
    for (auto const &asset : manifest.assets) files.emplace(asset.path, *staged.read_artwork(asset.id));
    auto roundtrip = Package::open(Archive::write(files)); // In-memory integrity check, NOT durable publication.
    EXPECT_NO_THROW(roundtrip.verify_artworks());
    EXPECT_EQ(roundtrip.manifest().serialize(), serialized);
    EXPECT_EQ(roundtrip.asset(identifier(3)).name, "Marco");
    EXPECT_EQ(roundtrip.manifest().assets.size(), 2);
    EXPECT_EQ(catalog.snapshot().list().size(), 1);
}

TEST(ArtworkLibraryCatalog, TenThousandEntryWarmSearchUsesCachedKeysAndRecordsP95)
{
    auto catalog = Catalog::from_package(package(10000));
    auto snapshot = catalog.snapshot();
    ASSERT_EQ(snapshot.search("STRASSE BODA").size(), 10000);
    std::vector<double> elapsed;
    for (unsigned repeat = 0; repeat < 25; ++repeat) {
        auto start = std::chrono::steady_clock::now();
        auto matches = snapshot.search("STRASSE BODA");
        auto duration = std::chrono::steady_clock::now() - start;
        ASSERT_EQ(matches.size(), 10000);
        EXPECT_EQ(matches.front(), identifier(0));
        EXPECT_EQ(matches.back(), identifier(9999));
        elapsed.push_back(std::chrono::duration<double, std::milli>(duration).count());
    }
    std::sort(elapsed.begin(), elapsed.end());
    auto p95 = elapsed[23]; // nearest-rank percentile, ceil(.95 * 25) - 1
    RecordProperty("catalog_10000_warm_search_p95_ms", std::to_string(p95));
    EXPECT_LE(p95, 100.0); // Hardware/build qualification belongs to the main test lane.
}

TEST(ArtworkLibraryCatalog, RemovedItemsSnapshotContainsOnlySessionTrash)
{
    Manifest m; m.id = "77bcd302-a034-4b99-9270-139e70273123"; m.name = "Lib";
    auto catalog = Catalog::create(m);
    auto keep = catalog.add({"", "Keep", {}, 10, 10}, Bytes{'k'});
    auto gone = catalog.add({"", "Gone", {"tag"}, 10, 10}, Bytes{'g'});
    EXPECT_THROW((void)catalog.snapshot().removed_items_snapshot(), std::runtime_error);
    ASSERT_TRUE(catalog.remove(gone));
    auto before = catalog.snapshot().manifest().serialize();
    auto trash = catalog.snapshot().removed_items_snapshot();
    EXPECT_EQ(trash.list(), std::vector<std::string>{gone});
    EXPECT_EQ(trash.asset(gone).name, "Gone");
    EXPECT_EQ(trash.asset(gone).tags, std::vector<std::string>{"tag"});
    EXPECT_EQ(*trash.read_artwork(gone), Bytes{'g'});
    EXPECT_EQ(trash.removed_ids(), std::vector<std::string>{keep});
    EXPECT_EQ(trash.manifest().id, m.id);
    EXPECT_EQ(trash.manifest().revision, catalog.snapshot().manifest().revision);
    EXPECT_EQ(catalog.snapshot().manifest().serialize(), before); // Catalog unchanged.
    EXPECT_EQ(catalog.snapshot().list(), std::vector<std::string>{keep});
}
