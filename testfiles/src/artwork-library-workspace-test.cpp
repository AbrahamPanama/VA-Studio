// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic local fixtures only. No installed application, private art or GUI.
#include "ui/dialog/artwork-library-workspace.h"
#include "io/artwork-library-lightburn.h"
#include <gtest/gtest.h>
#include <gio/gio.h>
#include <bit>
#include <filesystem>
#include <fstream>
#include <zlib.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace Inkscape::UI::Dialog;
namespace {
std::string svg() { return "<svg xmlns='http://www.w3.org/2000/svg' width='25.4mm' height='25.4mm' viewBox='0 0 96 96'><rect width='10' height='20'/></svg>"; }
Art::Bytes bytes(std::string const &s) { return {s.begin(), s.end()}; }
void wait_for(std::shared_ptr<ArtworkLibraryWorkspace> const &w) {
    auto end = g_get_monotonic_time() + 10000000;
    while (w->busy() && g_get_monotonic_time() < end) {
        while (g_main_context_pending(nullptr) && g_get_monotonic_time() < end) g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    ASSERT_FALSE(w->busy()) << "Worker did not finish within test watchdog: " << w->message();
}
void put32(Art::Bytes &b, std::size_t p, std::uint32_t n) { for (unsigned i = 0; i < 4; ++i) b[p + i] = n >> (24 - i * 8); }
Art::Bytes lbart(bool malformed_tabs = false) {
    std::string good = std::string("<LightBurnShapes FormatVersion='1' MirrorX='False' MirrorY='True'><Shape Type='Rect' W='10' H='6' Cr='0'><XForm>1 0 0 1 0 0</XForm><Tabs>") +
        (malformed_tabs ? "0.1 0.2" : "0.1,0.2") + "</Tabs></Shape></LightBurnShapes>";
    std::string bad = "<LightBurnShapes FormatVersion='1' MirrorX='False' MirrorY='True'><Shape Type='Unsupported'/></LightBurnShapes>";
    Art::Bytes b(8), table(272);
    for (unsigned i = 0; i < 2; ++i) {
        auto xml = i ? bad : good; auto p = i * 136;
        table[p + 1] = 'A' + i; put32(table, p + 104, b.size()); put32(table, p + 108, 4); b.insert(b.end(), {'P','N','G','!'});
        auto offset = b.size(); put32(table, p + 112, offset); uLongf n = compressBound(xml.size());
        b.resize(offset + 4 + n); put32(b, offset, xml.size());
        if (compress2(b.data() + offset + 4, &n, reinterpret_cast<Bytef const *>(xml.data()), xml.size(), Z_BEST_SPEED) != Z_OK) throw std::runtime_error("fixture compression");
        b.resize(offset + 4 + n); put32(table, p + 116, n + 4);
        auto bits = std::bit_cast<std::uint64_t>(12.);
        for (auto axis : {120, 128}) { put32(table, p + axis, bits >> 32); put32(table, p + axis + 4, bits); }
    }
    put32(b, 0, b.size()); put32(b, 4, 2); b.insert(b.end(), table.begin(), table.end()); return b;
}
class WorkspaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto p = g_dir_make_tmp("vacards-library-controller-XXXXXX", nullptr); ASSERT_TRUE(p);
        // macOS's temporary root includes /var -> /private/var. Use the real
        // fixture directory, as the storage tests do; retain strict no-symlink
        // production storage checks.
        auto physical = std::filesystem::canonical(std::filesystem::u8path(p)).u8string();
        directory.assign(physical.begin(), physical.end()); g_free(p);
        RecordProperty("evidence_directory", directory); w = ArtworkLibraryWorkspace::create(); w->new_collection("Synthetic collection");
    }
    void TearDown() override { if (w->busy()) { w->cancel(); wait_for(w); } }
    std::string path(std::string const &name) { return Art::canonical_library_path(directory + "/" + name); }
    void write(std::string const &name, Art::Bytes const &b) { std::ofstream out(std::filesystem::u8path(path(name)), std::ios::binary); out.write(reinterpret_cast<char const *>(b.data()), b.size()); ASSERT_TRUE(out.good()); }
    void add(std::string name = "Frame") { w->add({"", name, {"award"}, 25.4, 25.4}, bytes(svg())); wait_for(w); ASSERT_EQ(w->message(), "Library edit staged; document history unchanged"); }
    std::string directory;
    std::shared_ptr<ArtworkLibraryWorkspace> w;
};
TEST_F(WorkspaceTest, NativeSaveOpenByContentAndSaveAsKeepsIdentity) {
    add(); auto original = w->active()->catalog.snapshot(); w->save(path("LIBRARY.VALIB")); wait_for(w);
    ASSERT_FALSE(w->active()->dirty()) << w->message(); EXPECT_TRUE(w->can_close());
    w->save(path("another-name.bin")); wait_for(w); EXPECT_FALSE(w->active()->dirty());
    EXPECT_TRUE(std::filesystem::exists(path("LIBRARY.VALIB")));
    auto other = ArtworkLibraryWorkspace::create(); other->open(path("another-name.bin")); wait_for(other);
    ASSERT_TRUE(other->active()); EXPECT_EQ(other->active()->catalog.snapshot().manifest().serialize(), original.manifest().serialize());
}
TEST_F(WorkspaceTest, LongUnicodePathsRouteSniffImportExportAndNativeOpen) {
    std::string folder = directory;
    for (unsigned i = 0; i < 4; ++i) folder += "/" + std::to_string(i) + "-" + std::string(70, 'x') + "-圖案";
    folder = Art::canonical_library_path(folder);
    ASSERT_GT(folder.size(), 260u);
    auto native_folder = folder;
#ifdef _WIN32
    // Fixture creation only; callers and saved sessions keep ordinary paths.
    native_folder = folder.starts_with("\\\\") ? "\\\\?\\UNC\\" + folder.substr(2) : "\\\\?\\" + folder;
#endif
    ASSERT_TRUE(std::filesystem::create_directories(std::filesystem::u8path(native_folder)));
    auto input = Art::canonical_library_path(folder + "/Invitación.bin");
    auto output = Art::canonical_library_path(folder + "/Ángel.svg");
    auto collection = Art::canonical_library_path(folder + "/圖案.valib");
    Art::write_library_export(input, bytes(svg()));
    w->open(input); wait_for(w); // Non-ZIP content, despite the unrelated suffix.
    ASSERT_TRUE(w->pending_import()) << w->message();
    w->accept_import(true); ASSERT_EQ(w->rows().size(), 1u);
    auto id = w->rows().front().id;
    w->export_svg(id, output); wait_for(w);
    EXPECT_EQ(w->message(), "SVG exported: " + output);
    EXPECT_EQ(Art::read_library_input(output, 4096), bytes(svg()));
    w->export_svg(id, output); wait_for(w);
    EXPECT_NE(w->message().find("SVG export incomplete"), std::string::npos);
    EXPECT_NE(w->message().find(output), std::string::npos);
    EXPECT_EQ(Art::read_library_input(output, 4096), bytes(svg()));
    w->save(collection); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    auto other = ArtworkLibraryWorkspace::create(); other->open(collection); wait_for(other);
    // A full ZIP is larger than the four-byte sniff; sniffing must use prefix=true.
    ASSERT_TRUE(other->active()) << other->message();
    EXPECT_EQ(other->active_id(), w->active_id());
    EXPECT_EQ(other->rows().size(), 1u);
    EXPECT_EQ(other->saved_session().paths, (std::vector<std::string>{collection}));
}
TEST_F(WorkspaceTest, RestoreCanonicalizesAndDeduplicatesPathsIncludingActiveSelection) {
    add(); w->save(path("first.valib")); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    w->new_collection("Second"); add("Second asset"); auto second = w->active_id();
    w->save(path("second.valib")); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    auto other = ArtworkLibraryWorkspace::create();
    auto nul_path = path("first.valib") + std::string("\0suffix", 7);
    other->restore_session({{directory + "/./first.valib", path("first.valib"), path("second.valib"),
                             "relative.valib", nul_path}, directory + "/./second.valib"});
    wait_for(other);
    EXPECT_EQ(other->collections().size(), 2u);
    EXPECT_EQ(other->active_id(), second);
    EXPECT_EQ(other->saved_session().paths, (std::vector<std::string>{path("first.valib"), path("second.valib")}));
    EXPECT_EQ(other->saved_session().active_path, path("second.valib"));
    // Equivalent Save As spelling must keep the loaded version, not CREATE ONLY.
    ASSERT_EQ(other->rows().size(), 1u);
    other->rename(other->rows().front().id, "Changed second asset"); wait_for(other);
    ASSERT_TRUE(other->active()->dirty());
    other->save(directory + "/./second.valib"); wait_for(other);
    ASSERT_TRUE(other->active()); EXPECT_FALSE(other->active()->dirty()) << other->message();
    EXPECT_EQ(other->active()->path, path("second.valib"));
    EXPECT_EQ(Art::load_library(path("second.valib")).package.manifest().assets.front().name, "Changed second asset");
}
#ifdef _WIN32
TEST_F(WorkspaceTest, NativeShortNameOpenRestoreAndSaveRetainFullPathAndVersion) {
    auto root = std::filesystem::u8path(directory).root_path().native();
    if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) GTEST_SKIP() << "Requires an isolated local NTFS fixture";
    wchar_t filesystem_name[32] = {};
    ASSERT_TRUE(GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, filesystem_name, 32))
        << "GetVolumeInformationW: " << GetLastError();
    if (std::wstring(filesystem_name) != L"NTFS") GTEST_SKIP() << "Requires NTFS short-name aliases";

    std::string folder = directory;
    for (unsigned i = 0; i < 4; ++i) folder += "/" + std::to_string(i) + "-" + std::string(70, 'x');
    folder = Art::canonical_library_path(folder);
    auto native_folder = std::filesystem::u8path("\\\\?\\" + folder);
    ASSERT_TRUE(std::filesystem::create_directories(native_folder));
    auto full = Art::canonical_library_path(folder + "/Windows native short name library.valib");
    ASSERT_GT(full.size(), 260u);
    add(); auto identity = w->active_id();
    w->save(full); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();

    // Query the actual existing file, not a fabricated/mock DOS spelling.
    auto native_full = std::filesystem::u8path("\\\\?\\" + full).native();
    auto required = GetShortPathNameW(native_full.c_str(), nullptr, 0);
    ASSERT_GT(required, 0u) << "GetShortPathNameW: " << GetLastError();
    std::wstring short_name(required, L'\0');
    auto length = GetShortPathNameW(native_full.c_str(), short_name.data(), required);
    ASSERT_GT(length, 0u) << "GetShortPathNameW: " << GetLastError();
    ASSERT_LT(length, required);
    short_name.resize(length);
    if (std::filesystem::path(short_name).filename() == std::filesystem::u8path(full).filename())
        GTEST_SKIP() << "NTFS did not provide a short-name alias for the existing .valib fixture";
    auto utf8 = std::filesystem::path(short_name).u8string();
    auto alias = Art::canonical_library_path(std::string(utf8.begin(), utf8.end()));
    ASSERT_NE(alias, full);
    RecordProperty("short_alias", alias); RecordProperty("full_path", full);

    auto opened = ArtworkLibraryWorkspace::create(); opened->open(alias); wait_for(opened);
    ASSERT_TRUE(opened->active()) << opened->message();
    EXPECT_EQ(opened->active_id(), identity);
    EXPECT_EQ(opened->active()->path, full);
    ASSERT_TRUE(opened->active()->version); EXPECT_EQ(opened->active()->version->path, full);
    EXPECT_EQ(opened->saved_session().paths, (std::vector<std::string>{full}));
    EXPECT_EQ(opened->saved_session().active_path, full);

    // Put another collection first so fallback-to-first cannot mask lost selection.
    auto first = path("first-restored.valib"), unavailable = path("unavailable.valib");
    w->new_collection("First restored"); add("Other asset");
    w->save(first); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    auto restored = ArtworkLibraryWorkspace::create();
    restored->restore_session({{first, alias, unavailable}, alias}); wait_for(restored);
    ASSERT_TRUE(restored->active()) << restored->message();
    EXPECT_EQ(restored->collections().size(), 2u);
    EXPECT_EQ(restored->active_id(), identity);
    EXPECT_EQ(restored->active()->path, full);
    ASSERT_TRUE(restored->active()->version); EXPECT_EQ(restored->active()->version->path, full);
    EXPECT_EQ(restored->saved_session().paths, (std::vector<std::string>{first, full, unavailable}));
    EXPECT_EQ(restored->saved_session().active_path, full);

    auto revision = restored->active()->version->revision;
    ASSERT_EQ(restored->rows().size(), 1u);
    restored->rename(restored->rows().front().id, "Saved through native alias"); wait_for(restored);
    ASSERT_TRUE(restored->active()->dirty());
    restored->save(alias); wait_for(restored);
    ASSERT_FALSE(restored->active()->dirty()) << restored->message();
    EXPECT_FALSE(restored->active()->uncertain);
    EXPECT_EQ(restored->active()->path, full);
    ASSERT_TRUE(restored->active()->version); EXPECT_EQ(restored->active()->version->path, full);
    EXPECT_GT(restored->active()->version->revision, revision);
    EXPECT_EQ(restored->saved_session().paths, (std::vector<std::string>{first, full, unavailable}));
    EXPECT_EQ(restored->saved_session().active_path, full);
    auto disk = Art::load_library(full);
    EXPECT_EQ(disk.version.path, full);
    EXPECT_EQ(disk.version.revision, restored->active()->version->revision);
    ASSERT_EQ(disk.package.manifest().assets.size(), 1u);
    EXPECT_EQ(disk.package.manifest().assets.front().name, "Saved through native alias");
}
#endif
TEST_F(WorkspaceTest, CancelledImportNeverPublishesCandidate) {
    add(); auto before = w->active()->catalog.snapshot().manifest().serialize(); write("input.svg", bytes(svg()));
    w->import_files({path("input.svg")}); w->cancel(); wait_for(w);
    EXPECT_FALSE(w->pending_import()); EXPECT_EQ(w->active()->catalog.snapshot().manifest().serialize(), before);
}
TEST_F(WorkspaceTest, PartialLbartNeedsExplicitDecisionAndRetainsTabDiagnostics) {
    auto input = lbart(); write("source.not-lbart", input); auto digest = Art::artwork_sha256(input);
    w->import_files({path("source.not-lbart")}); wait_for(w); ASSERT_TRUE(w->pending_import()) << w->message();
    EXPECT_TRUE(w->rows().empty()); ASSERT_EQ(w->pending_import()->entries.size(), 2u);
    EXPECT_TRUE(w->pending_import()->entries[0].converted); EXPECT_FALSE(w->pending_import()->entries[1].converted);
    EXPECT_NE(w->pending_import()->entries[0].message.find("Laser tabs not applied"), std::string::npos);
    w->accept_import(true); ASSERT_EQ(w->rows().size(), 1u);
    EXPECT_NE(w->rows()[0].extra_json.find("sourceOrdinal"), std::string::npos);
    EXPECT_NE(w->rows()[0].extra_json.find("0.1,0.2"), std::string::npos);
    std::ifstream in(path("source.not-lbart"), std::ios::binary); Art::Bytes after{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    EXPECT_EQ(Art::artwork_sha256(after), digest);
}
TEST_F(WorkspaceTest, RejectingPartialResultsPreservesPriorCatalog) {
    add(); auto before = w->active()->catalog.snapshot().manifest().serialize(); write("test.lbart", lbart());
    w->import_files({path("test.lbart")}); wait_for(w); ASSERT_TRUE(w->pending_import()); w->accept_import(false);
    EXPECT_EQ(w->active()->catalog.snapshot().manifest().serialize(), before);
}
TEST_F(WorkspaceTest, MalformedLaserTabPairsRemainRejectedWithoutChangingCatalog) {
    add(); auto before = w->active()->catalog.snapshot().manifest().serialize();
    write("malformed-tabs.lbart", lbart(true));
    w->import_files({path("malformed-tabs.lbart")}); wait_for(w);
    ASSERT_TRUE(w->pending_import()); ASSERT_EQ(w->pending_import()->entries.size(), 2u);
    EXPECT_FALSE(w->pending_import()->entries[0].converted);
    EXPECT_NE(w->pending_import()->entries[0].message.find("comma-separated"), std::string::npos);
    EXPECT_FALSE(w->pending_import()->entries[1].converted);
    w->accept_import(false);
    EXPECT_EQ(w->active()->catalog.snapshot().manifest().serialize(), before);
}
TEST_F(WorkspaceTest, MultiFileSvgDuplicateNamesAreIndependentAssets) {
    write("input.svg", bytes(svg())); w->import_files({path("input.svg"), path("input.svg")}); wait_for(w);
    ASSERT_TRUE(w->pending_import()); w->accept_import(true); ASSERT_EQ(w->rows().size(), 2u);
    EXPECT_NE(w->rows()[0].id, w->rows()[1].id); EXPECT_NE(w->rows()[0].name, w->rows()[1].name);
    EXPECT_EQ(w->rows()[0].sha256, w->rows()[1].sha256);
}
TEST_F(WorkspaceTest, UnsafeStandaloneSvgNeverReachesNativeOrCandidate) {
    write("unsafe.svg", bytes("<!DOCTYPE svg [<!ENTITY e SYSTEM 'file:///not-to-be-read'>]><svg xmlns='http://www.w3.org/2000/svg' width='96' height='96' viewBox='0 0 96 96'><text>&e;</text></svg>"));
    w->import_files({path("unsafe.svg")}); wait_for(w); ASSERT_TRUE(w->pending_import());
    ASSERT_EQ(w->pending_import()->entries.size(), 1u); EXPECT_FALSE(w->pending_import()->entries[0].converted);
    EXPECT_THROW(w->accept_import(true), std::runtime_error); EXPECT_TRUE(w->rows().empty());
}
TEST_F(WorkspaceTest, OpenedLightBurnCollectionsUseSourceNamesAndRemainDistinct) {
    auto input = lbart();
    for (auto const &name : {"Hogar.lbart", "Lámparas.LBART"}) {
        write(name, input);
        w->open(path(name)); wait_for(w);
        ASSERT_TRUE(w->pending_import()) << w->message();
        w->accept_import(true);
        EXPECT_EQ(w->rows().size(), 1u); // Only the supported entry is admitted.
    }
    ASSERT_EQ(w->collections().size(), 3u);
    EXPECT_EQ(w->collections()[1].label, "Hogar");
    EXPECT_EQ(w->collections()[2].label, "Lámparas");
    EXPECT_NE(w->collections()[1].identity, w->collections()[2].identity);
    w->open(path("Hogar.lbart")); wait_for(w);
    ASSERT_TRUE(w->pending_import());
    w->accept_import(true);
    EXPECT_EQ(w->active()->label, "Hogar (2)");
    EXPECT_EQ(w->active()->catalog.snapshot().manifest().name, "Hogar (2)");
    w->save(path("renamed-on-disk.valib")); wait_for(w);
    ASSERT_FALSE(w->active()->dirty()) << w->message();
    EXPECT_EQ(Art::load_library(path("renamed-on-disk.valib")).package.manifest().name, "Hogar (2)");
}

TEST_F(WorkspaceTest, ImportStillMergesWithoutRenamingAndCancelledOpenPublishesNothing) {
    write("Hogar.lbart", lbart());
    auto identity = w->active_id();
    w->import_files({path("Hogar.lbart")}); wait_for(w);
    ASSERT_TRUE(w->pending_import());
    w->accept_import(true);
    EXPECT_EQ(w->active_id(), identity);
    EXPECT_EQ(w->active()->label, "Synthetic collection");
    EXPECT_EQ(w->collections().size(), 1u);
    w->open(path("Hogar.lbart")); wait_for(w);
    ASSERT_TRUE(w->pending_import());
    EXPECT_EQ(w->pending_import()->candidate.snapshot().manifest().name, "Hogar");
    w->accept_import(false);
    EXPECT_EQ(w->active_id(), identity);
    EXPECT_EQ(w->collections().size(), 1u);
}

TEST_F(WorkspaceTest, SearchAndPageAdmissionAreBoundedAndInvalidateLateResults) {
    add("Áward frame"); add("Other ornament"); w->search("a\u0301ward"); wait_for(w); ASSERT_EQ(w->rows().size(), 1u);
    auto id = w->rows()[0].id; w->visible({id}); w->invalidate_page(); wait_for(w); EXPECT_TRUE(w->page().empty());
    w->visible({id}); wait_for(w); ASSERT_EQ(w->page().size(), 1u); ASSERT_TRUE(w->page()[0].svg);
    auto token = *w->page()[0].svg; w->invalidate_page(); EXPECT_EQ(token.asset_id(), id);
    EXPECT_THROW(w->visible(std::vector<std::string>(129, id)), std::runtime_error);
}
TEST_F(WorkspaceTest, SaveAfterRemovalMakesDurableTrashEvenBeforeFirstSave) {
    add(); auto id = w->rows()[0].id; w->remove(id); wait_for(w); ASSERT_TRUE(w->rows().empty());
    w->save(path("empty.valib")); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    ASSERT_FALSE(w->active()->recovery_paths.empty()); auto recovery = w->active()->recovery_paths.front();
    auto loaded = Art::load_library(recovery); ASSERT_EQ(loaded.package.manifest().assets.size(), 1u);
    EXPECT_EQ(loaded.package.manifest().assets[0].id, id);
    w->recover(recovery); wait_for(w); ASSERT_EQ(w->rows().size(), 1u); EXPECT_TRUE(w->active()->dirty());
    EXPECT_EQ(w->rows()[0].id, id);
    EXPECT_TRUE(std::filesystem::exists(path("empty.valib")));
}
TEST_F(WorkspaceTest, CatalogTrashRestoreDoesNotReuseIdOrOverwriteName) {
    add(); auto id = w->rows()[0].id; w->remove(id); wait_for(w); add(); w->restore(id); wait_for(w);
    ASSERT_EQ(w->rows().size(), 2u); EXPECT_EQ(w->rows()[0].id, id); EXPECT_NE(w->rows()[0].name, w->rows()[1].name);
}
TEST_F(WorkspaceTest, RecoveryPreservesOptionalCollectionAndAssetMetadataThroughSave) {
    Art::Manifest metadata;
    metadata.id = "f1559ad5-c9cd-47f5-bce2-1f1712c62f26";
    metadata.name = "Premios — colección";
    metadata.extra_json = R"({"customerNote":"Ángel","futureOptional":{"version":7,"opaqueUri":"https://example.invalid/not-a-resource"}})";
    auto catalog = Art::Catalog::create(metadata);
    auto id = catalog.add({"", "Marco", {"corte", "Ángel"}, 25.4, 25.4,
        R"({"sourceFormat":"SVG","nestedOptional":{"enabled":true}})"}, bytes(svg()));
    auto source = catalog.snapshot().manifest();
    auto original = Art::encode_catalog(catalog.snapshot());
    write("source.recovery.valib", original);
    auto prior_id = w->active_id();
    w->recover(path("source.recovery.valib")); wait_for(w);
    ASSERT_NE(w->active_id(), prior_id) << w->message();
    auto recovered = w->active()->catalog.snapshot();
    EXPECT_NE(recovered.manifest().id, source.id);
    EXPECT_EQ(recovered.manifest().extra_json, source.extra_json);
    EXPECT_EQ(recovered.asset(id).extra_json, source.assets.front().extra_json);
    EXPECT_EQ(recovered.asset(id).tags, source.assets.front().tags);
    EXPECT_EQ(*recovered.read_artwork(id), bytes(svg()));
    EXPECT_TRUE(w->active()->path.empty()); EXPECT_FALSE(w->active()->version);
    EXPECT_TRUE(w->active()->dirty());
    w->save(path("recovered.valib")); wait_for(w);
    ASSERT_FALSE(w->active()->dirty()) << w->message();
    auto saved = Art::load_library(path("recovered.valib"));
    EXPECT_EQ(saved.package.manifest().extra_json, source.extra_json);
    EXPECT_EQ(saved.package.manifest().assets.front().extra_json, source.assets.front().extra_json);
    std::ifstream in(path("source.recovery.valib"), std::ios::binary);
    EXPECT_EQ((Art::Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}), original);
}
TEST_F(WorkspaceTest, RecoveryAcceptsMaximumLengthUnicodeCollectionName) {
    Art::Manifest metadata;
    metadata.id = "f1559ad5-c9cd-47f5-bce2-1f1712c62f26";
    for (unsigned i = 0; i < 512; ++i) metadata.name += "á";
    ASSERT_EQ(metadata.name.size(), 1024u);
    auto catalog = Art::Catalog::create(metadata);
    write("long-name.valib", Art::encode_catalog(catalog.snapshot()));
    auto prior_id = w->active_id();
    w->recover(path("long-name.valib")); wait_for(w);
    ASSERT_NE(w->active_id(), prior_id) << w->message();
    EXPECT_EQ(w->active()->label, metadata.name);
    EXPECT_TRUE(w->active()->dirty()); EXPECT_TRUE(w->active()->path.empty());
}
TEST_F(WorkspaceTest, RecoveryRejectsUnsafeLaterAssetWithoutPublishingPartialCollection) {
    add();
    auto prior_id = w->active_id();
    auto prior = w->active()->catalog.snapshot().manifest().serialize();
    auto count = w->collections().size();
    Art::Manifest metadata;
    metadata.id = "f1559ad5-c9cd-47f5-bce2-1f1712c62f26"; metadata.name = "Untrusted recovery";
    auto catalog = Art::Catalog::create(metadata);
    catalog.add({"", "Valid first", {}, 25.4, 25.4}, bytes(svg()));
    catalog.add({"", "Unsafe second", {}, 25.4, 25.4},
        bytes("<svg xmlns='http://www.w3.org/2000/svg' width='25.4mm' height='25.4mm' viewBox='0 0 96 96'><script>never execute</script></svg>"));
    auto original = Art::encode_catalog(catalog.snapshot());
    write("unsafe-recovery.valib", original);
    w->recover(path("unsafe-recovery.valib")); wait_for(w);
    EXPECT_EQ(w->collections().size(), count); EXPECT_EQ(w->active_id(), prior_id);
    EXPECT_EQ(w->active()->catalog.snapshot().manifest().serialize(), prior);
    EXPECT_FALSE(w->message().empty());
    std::ifstream in(path("unsafe-recovery.valib"), std::ios::binary);
    EXPECT_EQ((Art::Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}), original);
}
TEST_F(WorkspaceTest, CancelledRecoveryDoesNotPublishOrChangePriorCollection) {
    add();
    auto prior_id = w->active_id();
    auto prior = w->active()->catalog.snapshot().manifest().serialize();
    auto original = Art::encode_catalog(w->active()->catalog.snapshot());
    write("cancel-recovery.valib", original);
    auto count = w->collections().size();
    w->recover(path("cancel-recovery.valib")); w->cancel(); wait_for(w);
    EXPECT_EQ(w->collections().size(), count); EXPECT_EQ(w->active_id(), prior_id);
    EXPECT_EQ(w->active()->catalog.snapshot().manifest().serialize(), prior);
    EXPECT_TRUE(w->active()->dirty());
    std::ifstream in(path("cancel-recovery.valib"), std::ios::binary);
    EXPECT_EQ((Art::Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}), original);
}
TEST_F(WorkspaceTest, RecoveryDiscoveryIsReadOnlyAndChangedCandidatesRequireAnotherInspection) {
    add();
    auto prior_id = w->active_id(); auto count = w->collections().size();
    auto prior = w->active()->catalog.snapshot().manifest().serialize();
    auto name = ".valib-stage-f1559ad5-c9cd-47f5-bce2-1f1712c62f26";
    write(name, Art::encode_catalog(w->active()->catalog.snapshot()));
    w->scan_recovery(directory); wait_for(w);
    ASSERT_TRUE(w->recovery_scan()) << w->message();
    ASSERT_EQ(w->recovery_scan()->files.size(), 1u);
    auto candidate = w->recovery_scan()->files.front();
    ASSERT_TRUE(candidate.version) << candidate.diagnostic;
    EXPECT_EQ(w->active_id(), prior_id); EXPECT_EQ(w->collections().size(), count);
    EXPECT_EQ(w->active()->catalog.snapshot().manifest().serialize(), prior);
    auto replacement = Art::Catalog::from_package(Art::load_library(path(name)).package);
    replacement.rename_library("Changed after discovery");
    write(name, Art::encode_catalog(replacement.snapshot()));
    w->recover(candidate.path, candidate.version); wait_for(w);
    EXPECT_EQ(w->active_id(), prior_id); EXPECT_EQ(w->collections().size(), count);
    EXPECT_NE(w->message().find("changed since discovery"), std::string::npos);
    w->scan_recovery(directory); wait_for(w);
    ASSERT_TRUE(w->recovery_scan()); candidate = w->recovery_scan()->files.front();
    w->recover(candidate.path, candidate.version); wait_for(w);
    EXPECT_NE(w->active_id(), prior_id); EXPECT_EQ(w->collections().size(), count + 1);
    EXPECT_TRUE(w->active()->dirty()); EXPECT_TRUE(w->active()->path.empty());
}
TEST_F(WorkspaceTest, CancelledRecoveryDiscoveryKeepsCollectionsAndPublishesNoCandidateList) {
    add(); auto prior = w->active()->catalog.snapshot().manifest().serialize();
    w->scan_recovery(directory);
    EXPECT_THROW(w->new_collection("Concurrent mutation"), std::runtime_error);
    w->cancel(); wait_for(w);
    EXPECT_FALSE(w->recovery_scan());
    EXPECT_EQ(w->active()->catalog.snapshot().manifest().serialize(), prior);
}
TEST_F(WorkspaceTest, RepeatedRecoveryCreatesIndependentEditableCollections) {
    add();
    auto original_id = w->active_id();
    auto asset_id = w->rows().front().id;
    auto original = Art::encode_catalog(w->active()->catalog.snapshot());
    write("repeat-recovery.valib", original);
    w->recover(path("repeat-recovery.valib")); wait_for(w);
    auto first_id = w->active_id();
    ASSERT_NE(first_id, original_id) << w->message();
    w->rename(asset_id, "Changed first copy"); wait_for(w);
    auto first = w->active()->catalog.snapshot();
    w->recover(path("repeat-recovery.valib")); wait_for(w);
    EXPECT_NE(w->active_id(), first_id); EXPECT_NE(w->active_id(), original_id);
    EXPECT_EQ(w->active()->catalog.snapshot().asset(asset_id).name, "Frame");
    EXPECT_EQ(first.asset(asset_id).name, "Changed first copy");
    EXPECT_TRUE(w->active()->dirty()); EXPECT_FALSE(w->active()->version);
    ASSERT_EQ(w->collections().size(), 3u);
    w->rename(asset_id, "Changed second copy"); wait_for(w);
    // Inspect the live collections as well as the immutable snapshot.
    w->select_collection(first_id);
    EXPECT_EQ(w->active()->catalog.snapshot().asset(asset_id).name, "Changed first copy");
    w->select_collection(original_id);
    EXPECT_EQ(w->active()->catalog.snapshot().asset(asset_id).name, "Frame");
    std::ifstream in(path("repeat-recovery.valib"), std::ios::binary);
    EXPECT_EQ((Art::Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}), original);
}
TEST_F(WorkspaceTest, ExternalRevisionConflictDoesNotClearDirtyState) {
    add(); w->save(path("source.valib")); wait_for(w); ASSERT_FALSE(w->active()->dirty());
    auto loaded = Art::load_library(path("source.valib")); auto competing = Art::Catalog::from_package(loaded.package);
    competing.rename_library("External edit"); auto result = Art::save_library(path("source.valib"), competing.snapshot(), loaded.version);
    ASSERT_EQ(result.publication, Art::Publication::Published);
    w->rename(w->rows()[0].id, "Local edit"); wait_for(w); w->save(); wait_for(w); EXPECT_TRUE(w->active()->dirty());
    EXPECT_EQ(Art::load_library(path("source.valib")).package.manifest().name, "External edit");
}
TEST_F(WorkspaceTest, SaveAsNeverOverwritesAnUnrelatedExistingFile) {
    add(); write("exists.valib", bytes("not a package")); w->save(path("exists.valib")); wait_for(w);
    EXPECT_TRUE(w->active()->dirty()); std::ifstream in(path("exists.valib")); std::string s; std::getline(in, s); EXPECT_EQ(s, "not a package");
}
TEST_F(WorkspaceTest, SaveAsSucceedsAfterOriginalDirectoryBecomesUnavailable) {
    auto original_directory = path("original-location"), retained_directory = path("retained-original-location");
    auto original = path("original-location/library.valib"), destination = path("new-local.valib");
    ASSERT_TRUE(std::filesystem::create_directory(std::filesystem::u8path(original_directory)));
    add(); auto identity = w->active_id();
    w->save(original); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    auto original_bytes = Art::read_library_input(original, 1024u * 1024);
    w->unload(); w->open(original); wait_for(w);
    ASSERT_TRUE(w->active()) << w->message();
    ASSERT_TRUE(w->active()->version); EXPECT_EQ(w->active()->version->path, original);

    // Preserve the real original while making its saved location unavailable.
    // This is not a simulation of an offline share's resolver/permission error.
    std::filesystem::rename(std::filesystem::u8path(original_directory), std::filesystem::u8path(retained_directory));
    ASSERT_FALSE(std::filesystem::exists(std::filesystem::u8path(original_directory)));
    ASSERT_EQ(w->rows().size(), 1u);
    w->rename(w->rows().front().id, "Saved without original location"); wait_for(w);
    ASSERT_TRUE(w->active()->dirty());
    w->save(destination); wait_for(w);
    ASSERT_FALSE(w->active()->dirty()) << w->message();
    EXPECT_FALSE(w->active()->uncertain);
    EXPECT_EQ(w->active_id(), identity); EXPECT_EQ(w->active()->path, destination);
    ASSERT_TRUE(w->active()->version); EXPECT_EQ(w->active()->version->path, destination);
    EXPECT_EQ(w->saved_session().paths, (std::vector<std::string>{destination}));
    EXPECT_EQ(w->saved_session().active_path, destination);
    auto disk = Art::load_library(destination);
    EXPECT_EQ(disk.package.manifest().id, identity);
    ASSERT_EQ(disk.package.manifest().assets.size(), 1u);
    EXPECT_EQ(disk.package.manifest().assets.front().name, "Saved without original location");
    EXPECT_EQ(Art::read_library_input(path("retained-original-location/library.valib"), 1024u * 1024), original_bytes);
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(original_directory)));
}
TEST_F(WorkspaceTest, WorkerRetainsWorkspaceUntilPublicationIsObserved) {
    add(); auto retained = w; w->save(path("retained.valib")); w.reset(); wait_for(retained);
    ASSERT_TRUE(retained->active()); EXPECT_FALSE(retained->active()->dirty()) << retained->message(); w = retained;
}
TEST_F(WorkspaceTest, UnloadRequiresDiscardAndNeverDeletesFiles) {
    add(); EXPECT_THROW(w->unload(), std::runtime_error); w->save(path("retained.valib")); wait_for(w);
    ASSERT_FALSE(w->active()->dirty()); w->unload(); EXPECT_FALSE(w->active()); EXPECT_TRUE(std::filesystem::exists(path("retained.valib")));
}
TEST_F(WorkspaceTest, BusyWorkerDoesNotAdmitSecondWriterOrMutation) {
    add(); w->save(path("busy.valib")); EXPECT_THROW(w->rename(w->rows()[0].id, "Not admitted"), std::runtime_error);
    EXPECT_FALSE(w->can_close()); wait_for(w); EXPECT_EQ(w->rows()[0].name, "Frame");
}
TEST_F(WorkspaceTest, ExportIsExactSelfContainedSvgAndCreateOnly) {
    add(); auto id = w->rows()[0].id; w->export_svg(id, path("asset.svg")); wait_for(w);
    std::ifstream in(path("asset.svg"), std::ios::binary); std::string s{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}; EXPECT_EQ(s, svg());
    w->export_svg(id, path("asset.svg")); wait_for(w); EXPECT_TRUE(w->active()->dirty());
}
// LIB-1: loading verifies integrity only; an artwork that fails admission must
// still be removable, and the save error must name it.
TEST_F(WorkspaceTest, InadmissibleLoadedArtworkCanBeRemovedAndTheLibrarySaved) {
    Art::Manifest m; m.id = "5d0f4f1e-6c11-4a53-9c43-3b5c0b6a8f10"; m.name = "Legacy";
    auto legacy = Art::Catalog::create(m);
    auto good = legacy.add({"", "Good", {}, 25.4, 25.4}, bytes(svg()));
    auto bad = legacy.add({"", "Scripted", {}, 25.4, 25.4}, bytes(
        "<svg xmlns='http://www.w3.org/2000/svg' width='25.4mm' height='25.4mm' viewBox='0 0 96 96'><script/></svg>"));
    auto created = Art::save_library(path("legacy.valib"), legacy.snapshot());
    ASSERT_EQ(created.publication, Art::Publication::Published) << created.message;
    w->open(path("legacy.valib")); wait_for(w);
    ASSERT_TRUE(w->active()); ASSERT_EQ(w->active()->label, "Legacy");
    w->rename(good, "Good renamed"); wait_for(w);
    w->save(); wait_for(w);
    EXPECT_TRUE(w->active()->dirty());
    EXPECT_NE(w->message().find("\"Scripted\""), std::string::npos) << w->message();
    w->remove(bad); wait_for(w);
    w->save(); wait_for(w);
    ASSERT_FALSE(w->active()->dirty()) << w->message();
    auto disk = Art::load_library(path("legacy.valib"));
    ASSERT_EQ(disk.package.manifest().assets.size(), 1u);
    EXPECT_EQ(disk.package.manifest().assets[0].name, "Good renamed");
    ASSERT_FALSE(w->active()->recovery_paths.empty());
    auto trash = Art::load_library(w->active()->recovery_paths.front());
    ASSERT_EQ(trash.package.manifest().assets.size(), 1u);
    EXPECT_EQ(trash.package.manifest().assets[0].id, bad);
}

// LIB-2: one trash copy per new removal, containing only removed artwork.
TEST_F(WorkspaceTest, SessionTrashIsWrittenOnceAndHoldsOnlyRemovedArtwork) {
    auto trash_files = [&] {
        std::size_t n = 0;
        for (auto const &e : std::filesystem::directory_iterator(std::filesystem::u8path(directory)))
            if (e.path().filename().string().find(".trash-") != std::string::npos) ++n;
        return n;
    };
    auto id_of = [&](std::string const &name) {
        for (auto const &r : w->rows()) if (r.name == name) return r.id;
        return std::string();
    };
    add("Keep"); add("Gone");
    w->save(path("growth.valib")); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    auto gone = id_of("Gone"); ASSERT_FALSE(gone.empty());
    w->remove(gone); wait_for(w);
    w->save(); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    EXPECT_EQ(trash_files(), 1u);
    auto trash = Art::load_library(w->active()->recovery_paths.front());
    ASSERT_EQ(trash.package.manifest().assets.size(), 1u);
    EXPECT_EQ(trash.package.manifest().assets[0].id, gone);
    for (unsigned i = 0; i < 3; ++i) {
        w->rename(w->rows()[0].id, "Keep " + std::to_string(i)); wait_for(w);
        w->save(); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    }
    EXPECT_EQ(trash_files(), 1u); // No new removal, no new trash copy.
    add("Second"); auto second = id_of("Second"); ASSERT_FALSE(second.empty());
    w->remove(second); wait_for(w);
    w->save(); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    EXPECT_EQ(trash_files(), 2u);
}

// LIB-5: the message lists only existing paths retained by this save.
TEST_F(WorkspaceTest, SaveMessageListsOnlyExistingPathsRetainedByThisSave) {
    add(); w->save(path("paths.valib")); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    EXPECT_EQ(w->message().find(".valib-stage-"), std::string::npos) << w->message();
    for (unsigned i = 0; i < 2; ++i) {
        w->rename(w->rows()[0].id, "Frame " + std::to_string(i)); wait_for(w);
        w->save(); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    }
    EXPECT_EQ(w->message().find(".valib-stage-"), std::string::npos) << w->message();
    ASSERT_EQ(w->active()->recovery_paths.size(), 2u);
    std::size_t listed = 0;
    for (auto const &p : w->active()->recovery_paths) {
        EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(p))) << p;
        if (w->message().find(p) != std::string::npos) ++listed;
    }
    EXPECT_EQ(listed, 1u) << w->message();
}

// LIB-7: Save As onto another existing file explains the create-only rule.
TEST_F(WorkspaceTest, SaveAsOntoAnotherExistingFileExplainsCreateOnly) {
    add(); write("occupied.valib", bytes("not a library"));
    w->save(path("occupied.valib")); wait_for(w);
    EXPECT_TRUE(w->active()->dirty());
    EXPECT_NE(w->message().find("Save As never replaces an existing file"), std::string::npos) << w->message();
    EXPECT_EQ(Art::read_library_input(path("occupied.valib"), 64), bytes("not a library"));
}

// LIB-11: saving keeps only the newest 5 prior-version copies.
TEST_F(WorkspaceTest, SavingKeepsOnlyTheNewestFivePriorVersionCopies) {
    add(); w->save(path("retained.valib")); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    for (unsigned i = 0; i < 7; ++i) {
        w->rename(w->rows()[0].id, "Frame " + std::to_string(i)); wait_for(w);
        w->save(); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    }
    std::size_t copies = 0;
    for (auto const &e : std::filesystem::directory_iterator(std::filesystem::u8path(directory)))
        if (e.path().filename().string().starts_with(".valib-recovery-")) ++copies;
    EXPECT_EQ(copies, 5u);
    EXPECT_NE(w->message().find("older recovery cop"), std::string::npos) << w->message();
    EXPECT_EQ(w->active()->recovery_paths.size(), 5u);
    for (auto const &p : w->active()->recovery_paths) EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(p))) << p;
}
TEST_F(WorkspaceTest, ImportedSvgArtworkNameOmitsFileExtension) {
    write("Logo final.svg", bytes(svg())); w->import_files({path("Logo final.svg")}); wait_for(w);
    ASSERT_TRUE(w->pending_import()); w->accept_import(true);
    ASSERT_EQ(w->rows().size(), 1u); EXPECT_EQ(w->rows()[0].name, "Logo final");
}
TEST_F(WorkspaceTest, StaleLockIsInspectedAndRemovedOnlyWhenUnchanged) {
    add(); w->save(path("locked.valib")); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    w->inspect_lock(); wait_for(w); EXPECT_FALSE(w->inspected_lock()) << w->message();
    write("locked.valib.lock", bytes("valib-storage-lock-v1\nnonce\nhost=press-mac\npid=42\nutc=2026-09-25T10:00:00Z\n"));
    w->rename(w->rows()[0].id, "Blocked"); wait_for(w);
    w->save(); wait_for(w); EXPECT_TRUE(w->active()->dirty());
    EXPECT_NE(w->message().find("locked by another save"), std::string::npos) << w->message();
    w->inspect_lock(); wait_for(w);
    ASSERT_TRUE(w->inspected_lock()) << w->message();
    EXPECT_EQ(w->inspected_lock()->holder, "host=press-mac, pid=42, utc=2026-09-25T10:00:00Z");
    auto inspected = *w->inspected_lock();
    write("locked.valib.lock", bytes("valib-storage-lock-v1\nreplaced\n"));
    w->remove_lock(inspected); wait_for(w);
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(path("locked.valib.lock"))));
    EXPECT_NE(w->message().find("changed after it was inspected"), std::string::npos) << w->message();
    w->inspect_lock(); wait_for(w); ASSERT_TRUE(w->inspected_lock());
    w->remove_lock(*w->inspected_lock()); wait_for(w);
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::u8path(path("locked.valib.lock"))));
    w->save(); wait_for(w); EXPECT_FALSE(w->active()->dirty()) << w->message();
}
// Review of LIB-2b: a restored artwork removed again gets a fresh trash copy.
TEST_F(WorkspaceTest, RestoredArtworkRemovedAgainGetsAFreshTrashCopy) {
    auto trash_files = [&] {
        std::size_t n = 0;
        for (auto const &e : std::filesystem::directory_iterator(std::filesystem::u8path(directory)))
            if (e.path().filename().string().find(".trash-") != std::string::npos) ++n;
        return n;
    };
    auto id_of = [&](std::string const &name) {
        for (auto const &r : w->rows()) if (r.name == name) return r.id;
        return std::string();
    };
    add("Keep"); add("Gone");
    w->save(path("restore.valib")); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    auto gone = id_of("Gone"); ASSERT_FALSE(gone.empty());
    w->remove(gone); wait_for(w);
    w->save(); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    ASSERT_EQ(trash_files(), 1u);
    w->restore(gone); wait_for(w);
    w->rename(gone, "Gone edited"); wait_for(w);
    w->save(); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    w->remove(gone); wait_for(w);
    w->save(); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    EXPECT_EQ(trash_files(), 2u);
    std::string names;
    for (auto const &e : std::filesystem::directory_iterator(std::filesystem::u8path(directory)))
        if (e.path().filename().string().find(".trash-") != std::string::npos) {
            auto loaded = Art::load_library(e.path().string());
            names += e.path().filename().string() + ":" + std::to_string(loaded.package.manifest().assets.size()) + " ";
            for (auto const &a : loaded.package.manifest().assets) names += "[" + a.name + "]";
        }
    EXPECT_NE(names.find("[Gone edited]"), std::string::npos) << names;
}
// Review of LIB-2b: Save As to another folder writes a trash copy there too.
TEST_F(WorkspaceTest, SaveAsElsewhereAfterRemovalWritesItsOwnTrashCopy) {
    auto count_trash = [](std::string const &folder) {
        std::size_t n = 0;
        for (auto const &e : std::filesystem::directory_iterator(std::filesystem::u8path(folder)))
            if (e.path().filename().string().find(".trash-") != std::string::npos) ++n;
        return n;
    };
    add("Keep"); add("Gone");
    w->save(path("first.valib")); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    std::string gone;
    for (auto const &r : w->rows()) if (r.name == "Gone") gone = r.id;
    ASSERT_FALSE(gone.empty());
    w->remove(gone); wait_for(w);
    w->save(); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    ASSERT_EQ(count_trash(directory), 1u);
    auto other = directory + "/elsewhere";
    std::filesystem::create_directory(std::filesystem::u8path(other));
    w->save(other + "/second.valib"); wait_for(w); ASSERT_FALSE(w->active()->dirty()) << w->message();
    EXPECT_EQ(count_trash(other), 1u);
}
}
