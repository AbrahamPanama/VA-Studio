// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/artwork-library-storage.h"
#include <gtest/gtest.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <cstring>
#include <memory>
#include <string>
#include <zlib.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __APPLE__
#include <sys/param.h>
#include <sys/mount.h>
#endif

using namespace Inkscape::IO::ArtworkLibrary;

namespace {
template <typename T> struct TestUnref { void operator()(T *p) const { if (p) g_object_unref(p); } };
template <typename T> using TestObject = std::unique_ptr<T, TestUnref<T>>;
Bytes bytes(std::string const &s) { return {s.begin(), s.end()}; }
std::string canonical_path(std::string const &path)
{
    std::unique_ptr<gchar, decltype(&g_free)> value(g_canonicalize_filename(path.c_str(), nullptr), &g_free);
    return value.get();
}
// Use native extended paths to construct fixtures independently of the storage
// adapter. No registry changes, shell commands or administrator token needed.
void make_test_directory(std::string const &path)
{
#ifdef _WIN32
    auto normalized = canonical_library_path(path);
    auto extended = normalized.starts_with("\\\\") ? "\\\\?\\UNC\\" + normalized.substr(2) : "\\\\?\\" + normalized;
    auto wide = g_utf8_to_utf16(extended.c_str(), -1, nullptr, nullptr, nullptr);
    ASSERT_NE(wide, nullptr);
    bool ok = CreateDirectoryW(reinterpret_cast<wchar_t const *>(wide), nullptr);
    auto error = GetLastError(); g_free(wide);
    ASSERT_TRUE(ok) << "Create test directory: Windows error " << error;
#else
    ASSERT_EQ(g_mkdir(path.c_str(), 0700), 0) << g_strerror(errno);
#endif
}
// Same raw-deflate ZIP construction used by the archive regression fixtures;
// store-only production encoding cannot reproduce compressed manifest failures.
Bytes compressed_manifest(std::string const &text) {
    auto zip = Archive::write({{"library.json", bytes(text)}});
    auto set = [](Bytes &b, std::size_t at, std::size_t value, unsigned n = 4) {
        for (unsigned i = 0; i < n; ++i) b.at(at + i) = value >> (8 * i);
    };
    std::size_t directory_offset = 0;
    for (unsigned i = 0; i < 4; ++i) directory_offset |= std::size_t(zip.at(zip.size() - 6 + i)) << (8 * i);
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("Cannot initialize fixture compression");
    Bytes compressed(compressBound(text.size()));
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(text.data())); stream.avail_in = text.size();
    stream.next_out = compressed.data(); stream.avail_out = compressed.size();
    auto status = deflate(&stream, Z_FINISH); auto size = stream.total_out; deflateEnd(&stream);
    if (status != Z_STREAM_END) throw std::runtime_error("Fixture compression failed");
    compressed.resize(size);
    Bytes tail(zip.begin() + directory_offset, zip.end());
    zip.resize(42); set(zip, 8, 8, 2); set(zip, 18, size);
    zip.insert(zip.end(), compressed.begin(), compressed.end()); auto new_directory = zip.size();
    set(tail, 10, 8, 2); set(tail, 20, size);
    zip.insert(zip.end(), tail.begin(), tail.end()); set(zip, zip.size() - 6, new_directory);
    return zip;
}
#ifdef _WIN32
// Independent fixture I/O: do not use the storage adapter being tested. GLib's
// unprefixed helpers cannot read deep recovery paths, and set_contents appends
// a temporary suffix which can exceed MAX_PATH even when the target does not.
std::wstring native_fixture_path(std::string const &path)
{
    auto normalized = canonical_path(path);
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    if (!normalized.starts_with("\\\\?\\")) {
        normalized = normalized.starts_with("\\\\") ? "\\\\?\\UNC\\" + normalized.substr(2) : "\\\\?\\" + normalized;
    }
    std::unique_ptr<gunichar2, decltype(&g_free)> wide(
        g_utf8_to_utf16(normalized.c_str(), -1, nullptr, nullptr, nullptr), &g_free);
    if (!wide) throw std::runtime_error("Invalid UTF-8 fixture path");
    return reinterpret_cast<wchar_t const *>(wide.get());
}
struct FixtureCloseHandle {
    void operator()(void *handle) const { CloseHandle(handle); }
};
using FixtureHandle = std::unique_ptr<void, FixtureCloseHandle>;
[[noreturn]] void fixture_io_error(char const *operation, std::string const &path, DWORD error)
{
    throw std::runtime_error(std::string(operation) + ": Windows error " + std::to_string(error) + " at " + path);
}
#endif
void make_fixture_symlink(std::string const &path, std::string const &target, bool directory_link = false)
{
#ifdef _WIN32
    // GLocalFile's Windows symlink operation is not implemented. Use the
    // native operation, without changing privileges or machine configuration.
    auto link = native_fixture_path(path);
    auto destination = native_fixture_path(target);
    auto flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE |
                 (directory_link ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    if (!CreateSymbolicLinkW(link.c_str(), destination.c_str(), flags)) {
        fixture_io_error("Create native fixture symlink (requires existing symlink capability)", path, GetLastError());
    }
    auto raw = CreateFileW(link.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (raw == INVALID_HANDLE_VALUE) fixture_io_error("Inspect fixture symlink", path, GetLastError());
    FixtureHandle handle(raw);
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &info, sizeof(info))) {
        fixture_io_error("Query fixture symlink tag", path, GetLastError());
    }
    if (!(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || info.ReparseTag != IO_REPARSE_TAG_SYMLINK) {
        throw std::runtime_error("Fixture is not a native symbolic link: " + path);
    }
#else
    (void)directory_link;
    TestObject<GFile> link(g_file_new_for_path(path.c_str()));
    GError *error = nullptr;
    if (!g_file_make_symbolic_link(link.get(), target.c_str(), nullptr, &error)) {
        std::string message = error ? error->message : "Cannot create fixture symbolic link";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }
#endif
}
void put(std::string const &path, Bytes const &data)
{
#ifdef _WIN32
    auto target = native_fixture_path(path);
    std::unique_ptr<gchar, decltype(&g_free)> id(g_uuid_string_random(), &g_free);
    // Preserve replacement semantics without lengthening the destination's
    // basename. A failed write/rename retains this task-owned file as evidence.
    auto temporary = target.substr(0, target.find_last_of(L'\\') + 1) + L".fixture-" +
        std::wstring(id.get(), id.get() + std::strlen(id.get())) + L".tmp";
    auto raw = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) fixture_io_error("Create fixture temporary", path, GetLastError());
    FixtureHandle handle(raw);
    for (std::size_t offset = 0; offset < data.size();) {
        DWORD written = 0;
        auto count = static_cast<DWORD>(std::min<std::size_t>(65536, data.size() - offset));
        if (!WriteFile(handle.get(), data.data() + offset, count, &written, nullptr))
            fixture_io_error("Write fixture", path, GetLastError());
        if (!written) fixture_io_error("Write fixture made no progress", path, ERROR_WRITE_FAULT);
        offset += written;
    }
    if (!FlushFileBuffers(handle.get())) fixture_io_error("Flush fixture", path, GetLastError());
    if (!CloseHandle(handle.release())) fixture_io_error("Close fixture", path, GetLastError());
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fixture_io_error("Replace fixture", path, GetLastError());
#else
    GError *error = nullptr;
    if (!g_file_set_contents(path.c_str(), reinterpret_cast<char const *>(data.data()), data.size(), &error)) {
        std::string message(error->message); g_error_free(error); throw std::runtime_error(message);
    }
#endif
}
Bytes get(std::string const &path)
{
#ifdef _WIN32
    auto wide = native_fixture_path(path);
    auto raw = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) fixture_io_error("Open fixture", path, GetLastError());
    FixtureHandle handle(raw);
    Bytes result;
    std::array<unsigned char, 65536> buffer;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(handle.get(), buffer.data(), buffer.size(), &count, nullptr))
            fixture_io_error("Read fixture", path, GetLastError());
        if (!count) break;
        result.insert(result.end(), buffer.begin(), buffer.begin() + count);
    }
    return result;
#else
    gchar *data = nullptr; gsize size = 0; GError *error = nullptr;
    if (!g_file_get_contents(path.c_str(), &data, &size, &error)) {
        std::string message(error->message); g_error_free(error); throw std::runtime_error(message);
    }
    Bytes result(reinterpret_cast<unsigned char *>(data), reinterpret_cast<unsigned char *>(data) + size);
    g_free(data); return result;
#endif
}
Catalog populated(std::string id = "77bcd302-a034-4b99-9270-139e70273123")
{
    Manifest metadata; metadata.id = std::move(id); metadata.name = "Tarjetas 🌸";
    auto catalog = Catalog::create(metadata);
    NewArtwork art; art.id = "b203e8e9-640c-4195-b0ea-f9b790245678";
    art.name = "Asset"; art.width_mm = 25.4; art.height_mm = 25.4;
    // Deliberately opaque: storage integrity does not make these bytes safe SVG.
    catalog.add(art, Bytes(140000, 'X'));
    return catalog;
}
Bytes repack(Package const &package, std::string name, std::uint64_t revision)
{
    auto manifest = package.manifest(); manifest.name = std::move(name); manifest.revision = revision;
    std::map<std::string, Bytes> files{{"library.json", bytes(manifest.serialize())}};
    for (auto const &asset : manifest.assets) files.emplace(asset.path, package.read_artwork(asset.id));
    return Archive::write(files);
}
class ArtworkLibraryStorageTest : public ::testing::Test {
protected:
    std::string directory, destination;
    void SetUp() override
    {
        GError *error = nullptr;
        auto path = g_dir_make_tmp("valib-storage-XXXXXX", &error);
        ASSERT_NE(path, nullptr) << (error ? error->message : "No temporary directory");
        // macOS /var is a symlink. Resolve the TEST-owned directory, rather than
        // weakening production's symlink refusal. No shell/build/installed app.
        auto resolved = std::filesystem::canonical(std::filesystem::path(std::u8string(path, path + std::strlen(path)))).u8string();
        directory.assign(reinterpret_cast<char const *>(resolved.data()), resolved.size()); g_free(path);
        destination = directory + "/Tarjetas 🌸.VALIB";
        RecordProperty("evidence_directory", directory);
    }
    // Preserve the uniquely owned directory, failures and every recovery. Main
    // may clean an exact recorded directory later; no recursive test deletion.
    FileVersion create(Catalog const &catalog)
    {
        auto result = save_library(destination, catalog.snapshot());
        if (result.publication != Publication::Published || result.failure != StorageFailure::None || !result.version) {
            throw std::runtime_error("Real-filesystem setup publication failed: " + result.message);
        }
        return *result.version;
    }
};

// Reusable real-volume qualification. With no override this is an ordinary
// local regression; an explicit root runs the identical cases on USB/SMB.
// Every run creates its own directory and retains the evidence/recovery files.
class ArtworkLibraryNativeVolumeTest : public ArtworkLibraryStorageTest {
protected:
    void SetUp() override
    {
        auto root = g_getenv("VACARDS_LIBRARY_TEST_ROOT");
        if (!root || !*root) { ArtworkLibraryStorageTest::SetUp(); return; }
        auto id = g_uuid_string_random();
        directory = canonical_library_path(std::string(root) + "/valib-qa-" + id); g_free(id);
        ASSERT_NO_FATAL_FAILURE(make_test_directory(directory));
        destination = directory + "/Tarjetas 🌸.valib";
        RecordProperty("evidence_directory", directory);
    }
    void lifecycle()
    {
        auto catalog = populated(); auto original = create(catalog);
        auto reopened = load_library(destination);
        ASSERT_EQ(reopened.version, original);
        catalog.rename_library("Saved again — segunda revisión");
        auto updated = save_library(destination, catalog.snapshot(), original);
        ASSERT_EQ(updated.failure, StorageFailure::None) << updated.message;
        ASSERT_TRUE(updated.version); EXPECT_EQ(updated.publication, Publication::Published);
        EXPECT_EQ(load_library(destination).package.manifest().name, "Saved again — segunda revisión");
        EXPECT_EQ(load_library(updated.recovery_path).version.sha256, original.sha256);
        auto scan = scan_library_recovery(directory);
        ASSERT_EQ(scan.files.size(), 1u);
        ASSERT_TRUE(scan.files.front().version) << scan.files.front().diagnostic;
        EXPECT_EQ(scan.files.front().version->sha256, original.sha256);
        EXPECT_EQ(save_library(destination, catalog.snapshot()).failure, StorageFailure::Conflict);
        EXPECT_EQ(save_library(destination, catalog.snapshot(), original).failure, StorageFailure::Conflict);
        catalog.rename_library("Must not be published");
        bool cancel = false;
        StorageOptions options;
        options.cancelled = [&] { return cancel; };
        options.checkpoint = [&](auto const &p) { if (p.phase == StoragePhase::StageVerified) cancel = true; };
        auto stopped = save_library(destination, catalog.snapshot(), updated.version, options);
        EXPECT_EQ(stopped.failure, StorageFailure::Cancelled);
        EXPECT_EQ(stopped.publication, Publication::NotPublished);
        EXPECT_EQ(load_library(destination).version, *updated.version);
        // Lock release is proven by another real save, not a path-limited stat.
        auto final = save_library(destination, catalog.snapshot(), updated.version);
        ASSERT_EQ(final.failure, StorageFailure::None) << final.message;
        ASSERT_TRUE(final.version);
        EXPECT_EQ(load_library(destination).version, *final.version);

        auto exported = directory + "/Exportación 🌸.svg";
        auto svg = bytes("<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
        ASSERT_NO_THROW(write_library_export(exported, svg));
        EXPECT_EQ(read_library_input(exported, svg.size()), svg);
        EXPECT_EQ(read_library_input(exported, 4, {}, true), bytes("<svg"));
        EXPECT_THROW(read_library_input(exported, 4), StorageError);
        EXPECT_THROW(write_library_export(exported, bytes("not a replacement")), StorageError);
        EXPECT_EQ(read_library_input(exported, svg.size()), svg);
        RecordProperty("destination", canonical_library_path(destination));
        RecordProperty("directory_flush", updated.directory_flush_completed ? "supported" : "not-supported");
    }
};
}

TEST_F(ArtworkLibraryNativeVolumeTest, SaveUpdateReopenConflictCancellationAndRecovery)
{
    lifecycle();
}

TEST_F(ArtworkLibraryNativeVolumeTest, DeepUnicodePathsSupportTheWholeLifecycle)
{
    for (int i = 0; i < 7; ++i) {
        directory += "/" + std::to_string(i) + "-proyectos-tarjetas-premios-invitaciones-álbum-🌸";
        ASSERT_NO_FATAL_FAILURE(make_test_directory(directory));
    }
    destination = directory + "/Biblioteca de diseños 🌸.valib";
    ASSERT_GT(destination.size(), 400u);
    lifecycle();
}

TEST_F(ArtworkLibraryNativeVolumeTest, MaximumLengthFilenameDoesNotOverflowItsWriterLock)
{
    destination = directory + "/" + std::string(249, 'a') + ".valib";
    lifecycle();
}

TEST_F(ArtworkLibraryNativeVolumeTest, MacAndWindowsUseIdenticalLongFilenameLocks)
{
    std::string unicode;
    for (int i = 0; i < 82; ++i) unicode += "字";
    for (auto const &[name, digest] : std::array<std::pair<std::string, std::string>, 2>{{
        {std::string(245, 'A') + ".valib", "b815f8105b80be464ba545951df3e4647d7a0d40d3d7c901efa4f6ead680d2cc"},
        {unicode + ".valib", "9c701efbb13e10299c69fa6298c05c6981f58f6810f8ecf5e5b3194d05dabbc0"}
    }}) {
        auto lock = directory + "/.valib-lock-" + digest + ".lock";
        auto marker = bytes("Another Mac or Windows client is saving this library");
        write_library_export(lock, marker);
        auto result = save_library(directory + "/" + name, populated().snapshot());
        EXPECT_EQ(result.failure, StorageFailure::Locked) << result.message;
        EXPECT_EQ(result.publication, Publication::NotPublished);
        EXPECT_EQ(read_library_input(lock, 256), marker);
        EXPECT_TRUE(result.staged_path.empty());
    }
}

#ifdef _WIN32
TEST(ArtworkLibraryWindowsPathTest, NormalizesExtendedDriveAndUncPathsWithoutByteTruncation)
{
    EXPECT_EQ(canonical_library_path("\\\\?\\C:\\folder\\x.valib"), "C:\\folder\\x.valib");
    EXPECT_EQ(canonical_library_path("\\\\?\\UNC\\server\\share\\x.valib"), "\\\\server\\share\\x.valib");
    std::string long_unicode = "C:\\";
    for (int i = 0; i < 12000; ++i) long_unicode += "字";
    // Length validation is UTF-16; the filesystem separately enforces its
    // per-component limits when actually opening a path.
    EXPECT_GT(long_unicode.size(), 32768u);
    EXPECT_EQ(canonical_library_path(long_unicode), long_unicode);
    EXPECT_THROW(canonical_library_path("C:\\" + std::string(32767, 'a')), StorageError);
    for (auto path : {"C:relative.valib", "\\relative.valib", "\\\\.\\PhysicalDrive0", "\\\\?\\GLOBALROOT\\Device\\Harddisk0"})
        EXPECT_THROW(canonical_library_path(path), StorageError) << path;
}

TEST_F(ArtworkLibraryNativeVolumeTest, WindowsDriveAndShareRootsCanBeRecoveryFolders)
{
    TestObject<GFile> root(g_file_new_for_path(directory.c_str()));
    while (auto parent = g_file_get_parent(root.get())) root.reset(parent);
    auto path = g_file_get_path(root.get());
    RecoveryScanLimits limits;
    // Read-only, tightly bounded inspection; never load unrelated packages.
    limits.directory_entries = 1; limits.package_bytes = 1; limits.expanded_bytes = 1;
    EXPECT_NO_THROW(scan_library_recovery(path, limits));
    g_free(path);
}

TEST_F(ArtworkLibraryStorageTest, WindowsShortAliasesKeepTheRealNameAndOverwriteAuthority)
{
    for (int i = 0; i < 7; ++i) {
        directory += "/" + std::to_string(i) + "-biblioteca-premios-invitaciones-álbum";
        ASSERT_NO_FATAL_FAILURE(make_test_directory(directory));
    }
    destination = canonical_library_path(directory + "/Nombre original de la biblioteca.valib");
    ASSERT_GT(destination.size(), 260u);
    auto catalog = populated();
    auto first = create(catalog);
    auto short_name = [](std::string const &path) {
        auto wide = native_fixture_path(path);
        auto size = GetShortPathNameW(wide.c_str(), nullptr, 0);
        if (!size) throw std::runtime_error("Cannot query fixture short name: " + std::to_string(GetLastError()));
        std::vector<wchar_t> buffer(size);
        auto written = GetShortPathNameW(wide.c_str(), buffer.data(), size);
        if (!written || written >= size) throw std::runtime_error("Cannot read fixture short name");
        std::unique_ptr<gchar, decltype(&g_free)> utf8(g_utf16_to_utf8(
            reinterpret_cast<gunichar2 const *>(buffer.data()), written, nullptr, nullptr, nullptr), &g_free);
        if (!utf8) throw std::runtime_error("Invalid fixture short name");
        return canonical_library_path(utf8.get());
    };
    auto alias = short_name(destination);
    if (alias == destination) GTEST_SKIP() << "This filesystem does not supply DOS short-name aliases";
    EXPECT_EQ(resolved_library_path(alias), destination);
    EXPECT_EQ(load_library(alias).version, first);
    auto new_name = short_name(directory) + "\\New filename must remain.valib";
    EXPECT_EQ(resolved_library_path(new_name),
              canonical_library_path(directory + "/New filename must remain.valib"));
    catalog.rename_library("Saved via alias");
    auto changed = save_library(alias, catalog.snapshot(), first);
    ASSERT_EQ(changed.failure, StorageFailure::None) << changed.message;
    ASSERT_TRUE(changed.version);
    EXPECT_EQ(changed.version->path, destination);
    EXPECT_EQ(load_library(destination).version, *changed.version);
    EXPECT_EQ(save_library(alias, catalog.snapshot(), first).failure, StorageFailure::Conflict);
    EXPECT_EQ(save_library(alias, catalog.snapshot()).failure, StorageFailure::Conflict);
}
#endif

TEST_F(ArtworkLibraryStorageTest, CreateReadBackStrongIdentityAndIntegrityNotSvgSafety)
{
    auto catalog = populated(); auto version = create(catalog);
    auto loaded = load_library(destination);
    EXPECT_EQ(version, loaded.version);
    EXPECT_FALSE(version.file_id.empty()); EXPECT_FALSE(version.filesystem_id.empty());
    EXPECT_EQ(version.sha256, artwork_sha256(get(destination)));
    EXPECT_EQ(version.library_id, catalog.snapshot().manifest().id);
    EXPECT_EQ(version.revision, 1u);
    EXPECT_EQ(loaded.package.read_artwork("b203e8e9-640c-4195-b0ea-f9b790245678"), Bytes(140000, 'X'));
    EXPECT_FALSE(g_file_test((destination + ".lock").c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(ArtworkLibraryStorageTest, RecoveryDiscoveryFindsActualInterruptedWriterFilesWithoutRestoring) {
    auto catalog = populated(); auto first = create(catalog); auto original = get(destination);
    catalog.rename_library("Unpublished revision");
    StorageOptions options;
    options.checkpoint = [](auto const &event) {
        if (event.phase == StoragePhase::BeforeAdmission) throw std::runtime_error("Synthetic interrupted save");
    };
    auto interrupted = save_library(destination, catalog.snapshot(), first, options);
    ASSERT_EQ(interrupted.publication, Publication::NotPublished);
    ASSERT_FALSE(interrupted.staged_path.empty()); ASSERT_FALSE(interrupted.recovery_path.empty());
    auto scan = scan_library_recovery(directory);
    ASSERT_EQ(scan.files.size(), 2u); EXPECT_FALSE(scan.limited);
    for (auto const &candidate : scan.files) {
        ASSERT_TRUE(candidate.version) << candidate.diagnostic;
        EXPECT_EQ(*candidate.version, load_library(candidate.path).version);
        if (candidate.path == interrupted.staged_path) {
            EXPECT_EQ(candidate.kind, RecoveryFileKind::Staged);
            EXPECT_EQ(candidate.label, "Unpublished revision");
        } else {
            EXPECT_EQ(candidate.path, interrupted.recovery_path);
            EXPECT_EQ(candidate.kind, RecoveryFileKind::PreviousVersion);
            EXPECT_EQ(candidate.version->sha256, first.sha256);
        }
    }
    EXPECT_EQ(get(destination), original);
    EXPECT_FALSE(g_file_test((destination + ".lock").c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(ArtworkLibraryStorageTest, RecoveryDiscoveryReportsDamagedCopiesAndIgnoresOrdinaryFilesAndLocks) {
    auto encoded = encode_catalog(populated().snapshot());
    auto trash = destination + ".trash-f1559ad5-c9cd-47f5-bce2-1f1712c62f26.valib";
    auto damaged = directory + "/.valib-stage-f1559ad5-c9cd-47f5-bce2-1f1712c62f26";
    put(trash, encoded); put(damaged, bytes("truncated"));
    put(destination, encoded); put(destination + ".lock", bytes("not our lock"));
    auto scan = scan_library_recovery(directory);
    ASSERT_EQ(scan.files.size(), 2u);
    ASSERT_TRUE(scan.files[0].version); EXPECT_EQ(scan.files[0].path, canonical_path(trash));
    EXPECT_EQ(scan.files[0].kind, RecoveryFileKind::Trash);
    EXPECT_FALSE(scan.files[1].version); EXPECT_EQ(scan.files[1].path, canonical_path(damaged));
    EXPECT_FALSE(scan.files[1].diagnostic.empty());
    EXPECT_EQ(get(destination + ".lock"), bytes("not our lock"));
    EXPECT_EQ(get(damaged), bytes("truncated")); EXPECT_EQ(get(trash), encoded);
    // Opaque 'X' artwork is integrity-valid but NOT admitted SVG. Discovery
    // intentionally does not instantiate a document or confer render authority.
}

TEST_F(ArtworkLibraryStorageTest, RecoveryDiscoveryDoesNotRecurseOrFollowCandidateSymlinks) {
    auto encoded = encode_catalog(populated().snapshot());
    auto nested = directory + "/nested";
    ASSERT_EQ(g_mkdir(nested.c_str(), 0700), 0);
    auto name = "/.valib-stage-f1559ad5-c9cd-47f5-bce2-1f1712c62f26";
    put(nested + name, encoded);
    make_fixture_symlink(directory + name, nested + name);
    auto scan = scan_library_recovery(directory);
    ASSERT_EQ(scan.files.size(), 1u);
    EXPECT_FALSE(scan.files.front().version); EXPECT_FALSE(scan.files.front().diagnostic.empty());
    EXPECT_EQ(get(nested + name), encoded);
}

TEST_F(ArtworkLibraryStorageTest, RecoveryDiscoveryFlagsDirectoryAndCandidateCountLimits) {
    auto encoded = encode_catalog(populated().snapshot());
    for (auto id : {"f1559ad5-c9cd-47f5-bce2-1f1712c62f26", "b203e8e9-640c-4195-b0ea-f9b790245678"})
        put(directory + "/.valib-stage-" + id, encoded);
    RecoveryScanLimits limits; limits.candidate_files = 1;
    auto scan = scan_library_recovery(directory, limits);
    EXPECT_TRUE(scan.limited); EXPECT_EQ(scan.files.size(), 1u);
    limits = {}; limits.directory_entries = 1;
    scan = scan_library_recovery(directory, limits);
    EXPECT_TRUE(scan.limited); EXPECT_EQ(scan.entries_examined, 1u);
}

TEST_F(ArtworkLibraryStorageTest, RecoveryDiscoveryFlagsEncodedAndExpandedByteLimits) {
    auto encoded = encode_catalog(populated().snapshot());
    auto path = directory + "/.valib-stage-f1559ad5-c9cd-47f5-bce2-1f1712c62f26"; put(path, encoded);
    for (bool expanded : {false, true}) {
        RecoveryScanLimits limits;
        if (expanded) limits.expanded_bytes = 1; else limits.package_bytes = 1;
        auto scan = scan_library_recovery(directory, limits);
        EXPECT_TRUE(scan.limited); ASSERT_EQ(scan.files.size(), 1u);
        EXPECT_FALSE(scan.files.front().version); EXPECT_FALSE(scan.files.front().diagnostic.empty());
        EXPECT_LE(scan.package_bytes_examined, limits.package_bytes);
        EXPECT_LE(scan.expanded_bytes_examined, limits.expanded_bytes);
    }
    EXPECT_EQ(get(path), encoded);
}

TEST_F(ArtworkLibraryStorageTest, RecoveryDiscoveryCancellationAndInvalidRootsDoNotReturnSuccess) {
    EXPECT_THROW(scan_library_recovery(directory, {}, [] { return true; }), StorageError);
    EXPECT_THROW(scan_library_recovery("relative"), StorageError);
    EXPECT_THROW(scan_library_recovery(destination), StorageError);
    RecoveryScanLimits limits; limits.candidate_files = 0;
    EXPECT_THROW(scan_library_recovery(directory, limits), StorageError);
    limits = {}; limits.directory_entries = 4097;
    EXPECT_THROW(scan_library_recovery(directory, limits), StorageError);
}
TEST_F(ArtworkLibraryStorageTest, MalformedCompressedManifestsCannotBypassAggregateExpansionBudget) {
    constexpr std::size_t expanded = 1024 * 1024;
    auto encoded = compressed_manifest(std::string(expanded, 'x')); // Invalid JSON, valid ZIP/CRC.
    ASSERT_LT(encoded.size(), expanded / 100);
    EXPECT_EQ(Archive::open(encoded).size("library.json"), expanded);
    for (auto id : {"f1559ad5-c9cd-47f5-bce2-1f1712c62f26", "b203e8e9-640c-4195-b0ea-f9b790245678",
                    "77bcd302-a034-4b99-9270-139e70273123"})
        put(directory + "/.valib-stage-" + id, encoded);
    RecoveryScanLimits limits; limits.expanded_bytes = 2 * expanded;
    auto scan = scan_library_recovery(directory, limits);
    ASSERT_EQ(scan.files.size(), 3u);
    EXPECT_TRUE(scan.limited);
    EXPECT_EQ(scan.expanded_bytes_examined, 2 * expanded);
    EXPECT_LE(scan.package_bytes_examined, 3 * encoded.size());
    for (auto const &candidate : scan.files) { EXPECT_FALSE(candidate.version); EXPECT_FALSE(candidate.diagnostic.empty()); }
}

TEST_F(ArtworkLibraryStorageTest, RealOpenedHandleIdentityDoesNotRequirePathOnlyAttributes)
{
    auto catalog = populated();
    auto encoded = encode_catalog(catalog.snapshot());
    put(destination, encoded); // Exercise loading independently of writer admission.
    constexpr auto attrs = "standard::type,standard::is-symlink,standard::size,id::file,id::filesystem,etag::value,time::modified,time::modified-usec,unix::nlink";
    TestObject<GFile> file(g_file_new_for_path(destination.c_str()));
    GError *error = nullptr;
    TestObject<GFileInfo> path(g_file_query_info(file.get(), attrs, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                nullptr, &error));
    ASSERT_TRUE(path) << (error ? error->message : "No path info");
    TestObject<GFileInputStream> stream(g_file_read(file.get(), nullptr, &error));
    ASSERT_TRUE(stream) << (error ? error->message : "No opened stream");
    TestObject<GFileInfo> opened(g_file_input_stream_query_info(stream.get(), attrs, nullptr, &error));
    ASSERT_TRUE(opened) << (error ? error->message : "No opened info");
    for (auto key : {G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                     G_FILE_ATTRIBUTE_ID_FILE, G_FILE_ATTRIBUTE_ID_FILESYSTEM}) {
        ASSERT_TRUE(g_file_info_has_attribute(path.get(), key)) << key;
        ASSERT_TRUE(g_file_info_has_attribute(opened.get(), key)) << key;
    }
    EXPECT_EQ(g_file_info_get_file_type(opened.get()), G_FILE_TYPE_REGULAR);
    EXPECT_EQ(g_file_info_get_size(opened.get()), static_cast<goffset>(encoded.size()));
    for (auto key : {G_FILE_ATTRIBUTE_ID_FILE, G_FILE_ATTRIBUTE_ID_FILESYSTEM}) {
        ASSERT_NE(g_file_info_get_attribute_string(opened.get(), key), nullptr);
        EXPECT_STRNE(g_file_info_get_attribute_string(opened.get(), key), "");
#ifndef _WIN32
        EXPECT_STREQ(g_file_info_get_attribute_string(opened.get(), key),
                     g_file_info_get_attribute_string(path.get(), key));
#else
        // The Windows backend uses incompatible path/CRT-stream device IDs.
        // Preserve the observed values; storage must use native handle IDs.
        RecordProperty(std::string("gio_path_") + key, g_file_info_get_attribute_string(path.get(), key));
        RecordProperty(std::string("gio_opened_") + key, g_file_info_get_attribute_string(opened.get(), key));
#endif
    }
    // Record the real backend's inventory, not a fabricated GFileInfo. These
    // availability depends on backend and requested attributes. The observed
    // macOS stream omits is-symlink but supplies explicitly requested mtimes.
    // Never call an absent convenience getter.
    RecordProperty("opened_has_symlink", g_file_info_has_attribute(opened.get(), G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK));
    RecordProperty("opened_has_mtime", g_file_info_has_attribute(opened.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED));
    RecordProperty("opened_has_mtime_usec", g_file_info_has_attribute(opened.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC));
    RecordProperty("opened_has_etag", g_file_info_has_attribute(opened.get(), G_FILE_ATTRIBUTE_ETAG_VALUE));
    if (g_file_info_has_attribute(path.get(), G_FILE_ATTRIBUTE_ETAG_VALUE) &&
        g_file_info_has_attribute(opened.get(), G_FILE_ATTRIBUTE_ETAG_VALUE)) {
        EXPECT_STREQ(g_file_info_get_attribute_string(opened.get(), G_FILE_ATTRIBUTE_ETAG_VALUE),
                     g_file_info_get_attribute_string(path.get(), G_FILE_ATTRIBUTE_ETAG_VALUE));
    }
    ASSERT_TRUE(g_input_stream_close(G_INPUT_STREAM(stream.get()), nullptr, &error))
        << (error ? error->message : "Close failed");
    auto loaded = load_library(destination);
    EXPECT_EQ(loaded.version.size, encoded.size());
    EXPECT_EQ(loaded.version.sha256, artwork_sha256(encoded));
#ifdef _WIN32
    auto wide = std::filesystem::u8path(destination).wstring();
    auto handle = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION native {};
    auto queried = GetFileInformationByHandle(handle, &native);
    FILE_ID_INFO extended {};
    auto has_extended = GetFileInformationByHandleEx(handle, FileIdInfo, &extended, sizeof(extended));
    CloseHandle(handle);
    ASSERT_TRUE(queried);
    auto filesystem = "win32:" + std::to_string(native.dwVolumeSerialNumber);
    auto id = filesystem + ":" + std::to_string((std::uint64_t(native.nFileIndexHigh) << 32) | native.nFileIndexLow);
    if (has_extended) {
        filesystem = "win32-id128:" + std::to_string(extended.VolumeSerialNumber);
        id = filesystem + ":";
        constexpr char hex[] = "0123456789abcdef";
        for (auto byte : extended.FileId.Identifier) { id += hex[byte >> 4]; id += hex[byte & 15]; }
    }
    EXPECT_EQ(loaded.version.file_id, id);
    EXPECT_EQ(loaded.version.filesystem_id, filesystem);
#else
    EXPECT_EQ(loaded.version.file_id, g_file_info_get_attribute_string(path.get(), G_FILE_ATTRIBUTE_ID_FILE));
    EXPECT_EQ(loaded.version.filesystem_id, g_file_info_get_attribute_string(path.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM));
#endif
    if (g_file_info_has_attribute(path.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
        EXPECT_EQ(loaded.version.modified_seconds, g_file_info_get_attribute_uint64(path.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED));
    }
    if (g_file_info_has_attribute(path.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC)) {
        EXPECT_EQ(loaded.version.modified_microseconds, g_file_info_get_attribute_uint32(path.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC));
    }
    EXPECT_EQ(loaded.package.manifest().serialize(), catalog.snapshot().manifest().serialize());
    EXPECT_EQ(loaded.package.read_artwork("b203e8e9-640c-4195-b0ea-f9b790245678"), Bytes(140000, 'X'));
    EXPECT_EQ(load_library(destination).version, loaded.version);
}

TEST_F(ArtworkLibraryStorageTest, SameSizePathReplacementDuringReadStillConflicts)
{
    auto encoded = encode_catalog(populated().snapshot());
    put(destination, encoded);
    auto replacement = directory + "/replacement.valib";
    put(replacement, encoded); // Same bytes/size: only the file identity changes.
    unsigned polls = 0; bool replaced = false;
    auto callback = [&] {
        // load_library polls before path resolution; read_file polls again
        // before opening, then before the first chunk (the third poll).
        // Replace the pathname while its original handle remains open.
        if (++polls == 3) {
#ifdef _WIN32
            // Windows permits renaming this shared-delete open file, but
            // MoveFileEx cannot overwrite an open destination. Rebind the
            // pathname while the reader still holds the original handle;
            // both files contain identical bytes, so identity must catch it.
            auto from = native_fixture_path(replacement);
            auto to = native_fixture_path(destination);
            auto retained = native_fixture_path(directory + "/original-open.valib");
            if (!MoveFileExW(to.c_str(), retained.c_str(), 0)) {
                fixture_io_error("Retain original open fixture", destination, GetLastError());
            }
            if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
                fixture_io_error("Rebind fixture path during read", destination, GetLastError());
            }
#else
            TestObject<GFile> from(g_file_new_for_path(replacement.c_str()));
            TestObject<GFile> to(g_file_new_for_path(destination.c_str()));
            GError *error = nullptr;
            if (!g_file_move(from.get(), to.get(),
                    static_cast<GFileCopyFlags>(G_FILE_COPY_OVERWRITE | G_FILE_COPY_NO_FALLBACK_FOR_MOVE |
                                               G_FILE_COPY_NOFOLLOW_SYMLINKS), nullptr, nullptr, nullptr, &error)) {
                std::string message = error ? error->message : "Test replacement failed";
                g_clear_error(&error);
                throw std::runtime_error(message);
            }
#endif
            replaced = true;
        }
        return false;
    };
    try {
        (void)load_library(destination, {}, callback);
        FAIL() << "Path replacement was not detected";
    } catch (StorageError const &error) {
        EXPECT_EQ(error.failure(), StorageFailure::Conflict) << error.what();
    }
    EXPECT_TRUE(replaced);
    EXPECT_EQ(get(destination), encoded); // No automatic restoration/deletion.
    EXPECT_EQ(load_library(destination).version.sha256, artwork_sha256(encoded));
#ifdef _WIN32
    EXPECT_EQ(get(directory + "/original-open.valib"), encoded);
    EXPECT_NE(load_library(directory + "/original-open.valib").version.file_id,
              load_library(destination).version.file_id);
#endif
}

#ifdef _WIN32
TEST_F(ArtworkLibraryStorageTest, WindowsNativeVolumeAllowsUnicodeLocalPublicationWithoutGioFilesystemAttributes)
{
    directory += "/Biblioteca 🌸";
    ASSERT_EQ(g_mkdir(directory.c_str(), 0700), 0);
    destination = directory + "/Tarjetas 🌸.VALIB";
    RecordProperty("native_directory", directory);
    std::unique_ptr<gunichar2, decltype(&g_free)> wide(
        g_utf8_to_utf16(directory.c_str(), -1, nullptr, nullptr, nullptr), &g_free);
    ASSERT_NE(wide, nullptr);
    std::array<wchar_t, 32768> volume {};
    ASSERT_TRUE(GetVolumePathNameW(reinterpret_cast<wchar_t const *>(wide.get()), volume.data(), volume.size()))
        << GetLastError();
    auto drive = GetDriveTypeW(volume.data());
    ASSERT_TRUE(drive == DRIVE_FIXED || drive == DRIVE_REMOVABLE || drive == DRIVE_RAMDISK);
    DWORD flags = 0;
    std::array<wchar_t, MAX_PATH + 1> filesystem {};
    ASSERT_TRUE(GetVolumeInformationW(volume.data(), nullptr, 0, nullptr, nullptr, &flags,
                                     filesystem.data(), filesystem.size())) << GetLastError();
    ASSERT_EQ(flags & FILE_READ_ONLY_VOLUME, 0u);
    ASSERT_STREQ(filesystem.data(), L"NTFS");
    RecordProperty("native_type", "NTFS");
    RecordProperty("native_drive_type", std::to_string(drive));
    RecordProperty("native_volume_flags", std::to_string(flags));
    RecordProperty("glib_version", std::to_string(glib_major_version) + "." +
                   std::to_string(glib_minor_version) + "." + std::to_string(glib_micro_version));
    TestObject<GFile> parent(g_file_new_for_path(directory.c_str()));
    GError *error = nullptr;
    TestObject<GFileInfo> fs(g_file_query_filesystem_info(parent.get(), "filesystem::*", nullptr, &error));
    // Diagnostic only: future GIO versions may supply these attributes. The
    // positive native evidence above, not absent GIO booleans, admits the save.
    if (fs) {
        for (auto key : {G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
                         G_FILE_ATTRIBUTE_FILESYSTEM_READONLY}) {
            auto value = g_file_info_get_attribute_as_string(fs.get(), key);
            RecordProperty("gio_" + std::string(key).substr(std::strlen("filesystem::")), value ? value : "absent");
            g_free(value);
        }
    } else {
        RecordProperty("gio_filesystem_error", error ? error->message : "No filesystem info");
    }
    g_clear_error(&error);
    auto catalog = populated();
    auto first = create(catalog); auto prior = get(destination);
    catalog.rename_library("Native Windows volume save");
    auto saved = save_library(destination, catalog.snapshot(), first);
    ASSERT_EQ(saved.publication, Publication::Published) << saved.message;
    ASSERT_EQ(saved.failure, StorageFailure::None) << saved.message;
    ASSERT_TRUE(saved.version);
    EXPECT_EQ(load_library(destination).version, *saved.version);
    EXPECT_EQ(get(saved.recovery_path), prior);
    EXPECT_EQ(load_library(saved.recovery_path).version.sha256, first.sha256);
    EXPECT_FALSE(saved.directory_flush_completed); // Do not invent Windows directory durability.
    EXPECT_FALSE(g_file_test((destination + ".lock").c_str(), G_FILE_TEST_EXISTS));
}
#endif

#ifdef __APPLE__
TEST_F(ArtworkLibraryStorageTest, MacDirectMountEvidenceAllowsWritableLocalPublication)
{
    struct statfs mount {};
    ASSERT_EQ(statfs(directory.c_str(), &mount), 0);
    ASSERT_NE(mount.f_flags & MNT_LOCAL, 0u);
    ASSERT_EQ(mount.f_flags & MNT_RDONLY, 0u);
    RecordProperty("native_mount", mount.f_mntonname);
    RecordProperty("native_type", mount.f_fstypename);
    RecordProperty("native_flags", std::to_string(mount.f_flags));
    TestObject<GFile> parent(g_file_new_for_path(directory.c_str()));
    GError *error = nullptr;
    TestObject<GFileInfo> fs(g_file_query_filesystem_info(parent.get(),
        "filesystem::type,filesystem::remote,filesystem::readonly", nullptr, &error));
    // GIO evidence is diagnostic only: the exact-path native mount decides.
    if (fs) {
        for (auto key : {G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY}) {
            bool present = g_file_info_has_attribute(fs.get(), key);
            std::string property = std::strcmp(key, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE) == 0 ? "gio_remote" : "gio_readonly";
            RecordProperty(property, present ? std::to_string(g_file_info_get_attribute_boolean(fs.get(), key)) : "absent");
        }
    } else {
        RecordProperty("gio_filesystem_error", error ? error->message : "No filesystem info");
    }
    g_clear_error(&error);
    auto catalog = populated();
    auto first = create(catalog); auto prior = get(destination);
    catalog.rename_library("Native mount save");
    auto saved = save_library(destination, catalog.snapshot(), first);
    ASSERT_EQ(saved.publication, Publication::Published) << saved.message;
    ASSERT_EQ(saved.failure, StorageFailure::None) << saved.message;
    ASSERT_TRUE(saved.version);
    EXPECT_EQ(load_library(destination).version, *saved.version);
    EXPECT_EQ(get(saved.recovery_path), prior);
    EXPECT_EQ(load_library(saved.recovery_path).version.sha256, first.sha256);
}
#endif

TEST_F(ArtworkLibraryStorageTest, SavesRetainDistinctExactPriorPackagesAndRemovedArtwork)
{
    auto catalog = populated(); auto first = create(catalog); auto first_bytes = get(destination);
    catalog.rename_library("Second");
    auto second = save_library(destination, catalog.snapshot(), first);
    ASSERT_EQ(second.publication, Publication::Published) << second.message; ASSERT_TRUE(second.version);
    EXPECT_EQ(get(second.recovery_path), first_bytes);
    EXPECT_EQ(load_library(second.recovery_path).version.revision, first.revision);
    auto second_bytes = get(destination);
    catalog.remove("b203e8e9-640c-4195-b0ea-f9b790245678");
    auto third = save_library(destination, catalog.snapshot(), second.version);
    ASSERT_EQ(third.publication, Publication::Published) << third.message;
    EXPECT_NE(second.recovery_path, third.recovery_path);
    EXPECT_EQ(get(second.recovery_path), first_bytes);
    EXPECT_EQ(get(third.recovery_path), second_bytes);
    EXPECT_EQ(load_library(third.recovery_path).package.manifest().assets.size(), 1u);
    EXPECT_TRUE(load_library(destination).package.manifest().assets.empty());
}

TEST_F(ArtworkLibraryStorageTest, SameRevisionNoOpRetainsBytesAndCreatesNoRecovery)
{
    auto catalog = populated(); auto version = create(catalog); auto baseline = get(destination);
    unsigned writes = 0;
    StorageOptions o; o.checkpoint = [&](auto const &p) { if (p.phase != StoragePhase::Locked) ++writes; };
    auto result = save_library(destination, catalog.snapshot(), version, o);
    EXPECT_EQ(result.publication, Publication::Unchanged) << result.message;
    EXPECT_EQ(result.version, version); EXPECT_TRUE(result.recovery_path.empty());
    EXPECT_EQ(writes, 0u); EXPECT_EQ(get(destination), baseline);
}

TEST_F(ArtworkLibraryStorageTest, NoOpPreservesDisposablePreviews)
{
    auto c = populated(); auto m = c.snapshot().manifest();
    std::map<std::string, Bytes> files{{"library.json", bytes(m.serialize())},
        {m.assets[0].path, *c.snapshot().read_artwork(m.assets[0].id)},
        {"previews/" + m.assets[0].id + ".png", bytes("disposable invalid PNG")}};
    put(destination, Archive::write(files));
    auto loaded = load_library(destination); auto baseline = get(destination);
    auto catalog = Catalog::from_package(loaded.package);
    auto result = save_library(destination, catalog.snapshot(), loaded.version);
    EXPECT_EQ(result.publication, Publication::Unchanged) << result.message;
    EXPECT_EQ(get(destination), baseline);
}

TEST_F(ArtworkLibraryStorageTest, CreateCollisionNeverOverwritesExistingFile)
{
    put(destination, bytes("not a library, must survive")); auto baseline = get(destination);
    auto result = save_library(destination, populated().snapshot());
    EXPECT_EQ(result.publication, Publication::NotPublished); EXPECT_EQ(result.failure, StorageFailure::Conflict);
    EXPECT_EQ(get(destination), baseline);
}

TEST_F(ArtworkLibraryStorageTest, LockCollisionHasNoImplicitStaleTakeover)
{
    auto catalog = populated(); auto version = create(catalog); auto baseline = get(destination);
    auto lock = destination + ".lock"; put(lock, bytes("apparently stale, owned elsewhere"));
    catalog.rename_library("New"); auto result = save_library(destination, catalog.snapshot(), version);
    EXPECT_EQ(result.failure, StorageFailure::Locked); EXPECT_EQ(result.publication, Publication::NotPublished);
    EXPECT_EQ(get(lock), bytes("apparently stale, owned elsewhere")); EXPECT_EQ(get(destination), baseline);
}

TEST_F(ArtworkLibraryStorageTest, NestedCooperatingWriterIsRejectedWhileLockHeld)
{
    auto catalog = populated(); auto version = create(catalog); catalog.rename_library("Next");
    unsigned attempts = 0; StorageOptions o;
    o.checkpoint = [&](auto const &p) {
        if (p.phase == StoragePhase::StageVerified) {
            ++attempts;
            auto nested = save_library(destination, catalog.snapshot(), version);
            EXPECT_EQ(nested.failure, StorageFailure::Locked);
        }
    };
    auto result = save_library(destination, catalog.snapshot(), version, o);
    EXPECT_EQ(result.publication, Publication::Published) << result.message; EXPECT_EQ(attempts, 1u);
}

TEST_F(ArtworkLibraryStorageTest, SameRevisionExternalEditDetectedByStrongVersion)
{
    auto catalog = populated(); auto version = create(catalog);
    auto loaded = load_library(destination);
    put(destination, repack(loaded.package, "Externally changed", version.revision));
    auto external = get(destination); catalog.rename_library("Our change");
    auto result = save_library(destination, catalog.snapshot(), version);
    EXPECT_EQ(result.failure, StorageFailure::Conflict); EXPECT_EQ(result.publication, Publication::NotPublished);
    EXPECT_EQ(get(destination), external);
}

TEST_F(ArtworkLibraryStorageTest, ExternalRevisionAfterRecoveryRetainsExternalAndPriorRecovery)
{
    auto catalog = populated(); auto version = create(catalog); auto baseline = get(destination);
    auto old = load_library(destination); catalog.rename_library("Ours"); Bytes external;
    StorageOptions o; o.checkpoint = [&](auto const &p) {
        if (p.phase == StoragePhase::BeforeAdmission) {
            external = repack(old.package, "External winner", version.revision + 4); put(destination, external);
        }
    };
    auto result = save_library(destination, catalog.snapshot(), version, o);
    EXPECT_EQ(result.failure, StorageFailure::Conflict); EXPECT_EQ(result.publication, Publication::NotPublished);
    EXPECT_EQ(get(destination), external); EXPECT_EQ(get(result.recovery_path), baseline);
    EXPECT_NO_THROW(load_library(result.staged_path));
}

TEST_F(ArtworkLibraryStorageTest, ForeignUuidAndRegressedRevisionCannotReplaceHistory)
{
    auto catalog = populated(); auto first = create(catalog); auto old_snapshot = catalog.snapshot();
    catalog.rename_library("Next"); auto second = save_library(destination, catalog.snapshot(), first);
    ASSERT_TRUE(second.version); auto baseline = get(destination);
    auto foreign = populated("88bcd302-a034-4b99-9270-139e70273123");
    EXPECT_EQ(save_library(destination, foreign.snapshot(), second.version).failure, StorageFailure::Conflict);
    EXPECT_EQ(save_library(destination, old_snapshot, second.version).failure, StorageFailure::Conflict);
    EXPECT_EQ(get(destination), baseline);
}

TEST_F(ArtworkLibraryStorageTest, DiskStageAndRecoveryAreRevalidatedAfterCallbacks)
{
    for (bool corrupt_recovery : {false, true}) {
        SCOPED_TRACE(corrupt_recovery);
        destination = directory + (corrupt_recovery ? "/recovery.valib" : "/stage.valib");
        auto c = populated(); auto version = create(c); auto baseline = get(destination); c.rename_library("New");
        StorageOptions o; o.checkpoint = [&](auto const &p) {
            if (p.phase == StoragePhase::BeforeAdmission) put(corrupt_recovery ? p.recovery : p.staged, bytes("truncated"));
        };
        auto result = save_library(destination, c.snapshot(), version, o);
        EXPECT_EQ(result.publication, Publication::NotPublished); EXPECT_NE(result.failure, StorageFailure::None);
        EXPECT_EQ(get(destination), baseline);
    }
}

TEST_F(ArtworkLibraryStorageTest, CompanionNameCollisionsAreExclusiveAndNeverOverwrite)
{
    for (auto collision : {StoragePhase::BeforeStageCreate, StoragePhase::BeforeRecoveryCreate}) {
        destination = directory + "/collision-" + std::to_string(static_cast<int>(collision)) + ".valib";
        auto c = populated(); auto version = create(c); auto baseline = get(destination); c.rename_library("New");
        std::string occupied; StorageOptions o; o.checkpoint = [&](auto const &p) {
            if (p.phase == collision) {
                occupied = collision == StoragePhase::BeforeStageCreate ? p.staged : p.recovery;
                put(occupied, bytes("prior recovery collision"));
            }
        };
        auto result = save_library(destination, c.snapshot(), version, o);
        EXPECT_EQ(result.failure, StorageFailure::Conflict) << result.message;
        EXPECT_EQ(result.publication, Publication::NotPublished);
        EXPECT_EQ(get(occupied), bytes("prior recovery collision")); EXPECT_EQ(get(destination), baseline);
    }
}

TEST_F(ArtworkLibraryStorageTest, FailuresAtEveryPrepublicationPhaseRetainCommittedAndEarlierRecovery)
{
    for (auto phase : {StoragePhase::Locked, StoragePhase::BeforeStageCreate, StoragePhase::StageCreated,
                      StoragePhase::StageChunkWritten, StoragePhase::StageVerified, StoragePhase::BeforeRecoveryCreate,
                      StoragePhase::RecoveryCreated, StoragePhase::RecoveryChunkWritten,
                      StoragePhase::RecoveryVerified, StoragePhase::BeforeAdmission}) {
        SCOPED_TRACE(static_cast<int>(phase));
        destination = directory + "/fault-" + std::to_string(static_cast<int>(phase)) + ".valib";
        auto c = populated(); auto first = create(c); auto original = get(destination);
        c.rename_library("Second"); auto second = save_library(destination, c.snapshot(), first); ASSERT_TRUE(second.version);
        auto baseline = get(destination); c.rename_library("Third"); bool hit = false;
        StorageOptions o; o.checkpoint = [&](auto const &p) {
            if (p.phase == phase) { hit = true; throw StorageError(StorageFailure::Io, "Injected IO failure"); }
        };
        auto result = save_library(destination, c.snapshot(), second.version, o);
        EXPECT_TRUE(hit); EXPECT_EQ(result.publication, Publication::NotPublished); EXPECT_EQ(result.failure, StorageFailure::Io);
        EXPECT_EQ(get(destination), baseline); EXPECT_EQ(get(second.recovery_path), original);
        EXPECT_FALSE(g_file_test((destination + ".lock").c_str(), G_FILE_TEST_EXISTS));
    }
}

TEST_F(ArtworkLibraryStorageTest, CancellationBeforeDuringWriteAndBeforeAdmissionNeverPublishes)
{
    for (auto phase : {StoragePhase::Locked, StoragePhase::StageChunkWritten, StoragePhase::RecoveryVerified, StoragePhase::BeforeAdmission}) {
        destination = directory + "/cancel-" + std::to_string(static_cast<int>(phase)) + ".valib";
        auto c = populated(); auto version = create(c); auto baseline = get(destination); c.rename_library("New");
        bool cancelled = false; StorageOptions o; o.cancelled = [&] { return cancelled; };
        o.checkpoint = [&](auto const &p) { if (p.phase == phase) cancelled = true; };
        auto result = save_library(destination, c.snapshot(), version, o);
        EXPECT_EQ(result.failure, StorageFailure::Cancelled); EXPECT_EQ(result.publication, Publication::NotPublished);
        EXPECT_EQ(get(destination), baseline);
    }
    StorageOptions early; early.cancelled = [] { return true; };
    EXPECT_EQ(save_library(directory + "/never-created.valib", populated().snapshot(), {}, early).failure, StorageFailure::Cancelled);
}

TEST_F(ArtworkLibraryStorageTest, CancellationAfterPublicationIsExplicitlyTooLate)
{
    auto c = populated(); auto version = create(c); auto baseline = get(destination); c.rename_library("Committed");
    bool cancelled = false; StorageOptions o; o.cancelled = [&] { return cancelled; };
    o.checkpoint = [&](auto const &p) { if (p.phase == StoragePhase::Published) cancelled = true; };
    auto result = save_library(destination, c.snapshot(), version, o);
    EXPECT_EQ(result.publication, Publication::Published) << result.message;
    EXPECT_EQ(result.failure, StorageFailure::None); EXPECT_TRUE(result.cancellation_too_late);
    EXPECT_EQ(load_library(destination).package.manifest().name, "Committed"); EXPECT_EQ(get(result.recovery_path), baseline);
}

TEST_F(ArtworkLibraryStorageTest, PostpublicationCallbackFailureDoesNotPretendRollback)
{
    auto c = populated(); auto version = create(c); auto baseline = get(destination); c.rename_library("Committed");
    StorageOptions o; o.checkpoint = [](auto const &p) {
        if (p.phase == StoragePhase::Published) throw std::runtime_error("Consumer failed after publication");
    };
    auto result = save_library(destination, c.snapshot(), version, o);
    EXPECT_EQ(result.publication, Publication::Published); EXPECT_EQ(result.failure, StorageFailure::Io);
    ASSERT_TRUE(result.version); EXPECT_EQ(load_library(destination).version, *result.version);
    EXPECT_EQ(get(result.recovery_path), baseline);
}

TEST_F(ArtworkLibraryStorageTest, BoundedReadsRejectDirectoriesCorruptionAndMissingAuthoritativeArt)
{
    auto c = populated(); auto version = create(c);
    StorageLimits limit; limit.archive.package_bytes = version.size - 1;
    EXPECT_THROW(load_library(destination, limit), StorageError);
    EXPECT_THROW(load_library(directory), StorageError);
    EXPECT_THROW(load_library("relative.valib"), StorageError);
    auto data = get(destination); data.resize(data.size() / 2); put(directory + "/truncated.valib", data);
    EXPECT_THROW(load_library(directory + "/truncated.valib"), StorageError);
    auto m = c.snapshot().manifest();
    put(directory + "/missing-art.valib", Archive::write({{"library.json", bytes(m.serialize())}}));
    EXPECT_THROW(load_library(directory + "/missing-art.valib"), StorageError);
    auto wrong = Bytes(140000, 'Y');
    put(directory + "/hash-invalid.valib", Archive::write({{"library.json", bytes(m.serialize())}, {m.assets[0].path, wrong}}));
    EXPECT_THROW(load_library(directory + "/hash-invalid.valib"), StorageError);
    EXPECT_THROW(load_library(destination, {}, [] { return true; }), StorageError);
}

TEST_F(ArtworkLibraryStorageTest, SymlinkTargetAndAncestorRefusePublication)
{
    auto c = populated(); create(c); auto baseline = get(destination);
    auto alias = directory + "/alias.valib";
    make_fixture_symlink(alias, destination);
    EXPECT_EQ(save_library(alias, c.snapshot()).failure, StorageFailure::Unsupported);
    EXPECT_THROW(load_library(alias), StorageError);
    auto parent_alias = directory + "/parent-alias";
    make_fixture_symlink(parent_alias, directory, true);
    EXPECT_EQ(save_library(parent_alias + "/new.valib", c.snapshot()).failure, StorageFailure::Unsupported);
    EXPECT_EQ(get(destination), baseline);
}

TEST_F(ArtworkLibraryStorageTest, MissingParentFailsWithoutCreatingDirectories)
{
    auto missing = directory + "/missing/asset.valib";
    auto result = save_library(missing, populated().snapshot());
    EXPECT_EQ(result.publication, Publication::NotPublished); EXPECT_NE(result.failure, StorageFailure::None);
    EXPECT_FALSE(g_file_test((directory + "/missing").c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(ArtworkLibraryStorageTest, FailureAfterRealRenameReportsUncertainAndNeverRestoresOldBytes)
{
    auto c = populated(); auto version = create(c); auto baseline = get(destination); c.rename_library("Moved");
    StorageOptions o; o.checkpoint = [](auto const &p) {
        if (p.phase == StoragePhase::Moved) throw StorageError(StorageFailure::Io, "Injected post-rename durability failure");
    };
    auto result = save_library(destination, c.snapshot(), version, o);
    EXPECT_EQ(result.publication, Publication::Uncertain); EXPECT_EQ(result.failure, StorageFailure::Io);
    EXPECT_FALSE(result.version);
    EXPECT_EQ(load_library(destination).package.manifest().name, "Moved");
    EXPECT_EQ(get(result.recovery_path), baseline);
}

TEST_F(ArtworkLibraryStorageTest, ReplacedLockIsNeitherConsumedNorSilentlyTakenOver)
{
    auto c = populated(); auto version = create(c); auto baseline = get(destination); c.rename_library("New");
    StorageOptions o; o.checkpoint = [&](auto const &p) {
        if (p.phase == StoragePhase::BeforeAdmission) put(destination + ".lock", bytes("other owner's lock"));
    };
    auto result = save_library(destination, c.snapshot(), version, o);
    EXPECT_EQ(result.publication, Publication::NotPublished); EXPECT_EQ(result.failure, StorageFailure::Conflict);
    EXPECT_EQ(result.retained_lock_path, canonical_path(destination + ".lock"));
    EXPECT_EQ(get(destination + ".lock"), bytes("other owner's lock")); EXPECT_EQ(get(destination), baseline);
}

TEST_F(ArtworkLibraryStorageTest, SnapshotIsPinnedBeforeCallbacksEditCatalog)
{
    auto c = populated(); auto version = create(c); c.rename_library("Captured"); auto revision = c.snapshot().manifest().revision;
    StorageOptions o; o.checkpoint = [&](auto const &p) {
        if (p.phase == StoragePhase::Locked) c.rename_library("Later unsaved edit");
    };
    auto result = save_library(destination, c.snapshot(), version, o);
    ASSERT_EQ(result.publication, Publication::Published) << result.message; ASSERT_TRUE(result.version);
    EXPECT_EQ(result.version->revision, revision);
    EXPECT_EQ(load_library(destination).package.manifest().name, "Captured");
    EXPECT_EQ(c.snapshot().manifest().name, "Later unsaved edit");
}

TEST_F(ArtworkLibraryStorageTest, SuccessfulPublicationReportsNoStagedPath)
{
    auto catalog = populated();
    auto created = save_library(destination, catalog.snapshot());
    ASSERT_EQ(created.publication, Publication::Published) << created.message;
    EXPECT_TRUE(created.staged_path.empty()) << created.staged_path;
    catalog.rename_library("Next");
    auto updated = save_library(destination, catalog.snapshot(), created.version);
    ASSERT_EQ(updated.publication, Publication::Published) << updated.message;
    EXPECT_TRUE(updated.staged_path.empty()) << updated.staged_path;
    ASSERT_FALSE(updated.recovery_path.empty());
    EXPECT_EQ(load_library(updated.recovery_path).version.revision, created.version->revision);
}

TEST_F(ArtworkLibraryStorageTest, LockCollisionNamesTheLockFileAndHolder)
{
    auto catalog = populated(); auto version = create(catalog); auto baseline = get(destination);
    auto lock = destination + ".lock";
    auto marker = bytes("valib-storage-lock-v1\nnonce\nhost=press-mac\npid=42\nutc=2026-09-25T10:00:00Z\n");
    put(lock, marker);
    catalog.rename_library("New");
    auto result = save_library(destination, catalog.snapshot(), version);
    EXPECT_EQ(result.failure, StorageFailure::Locked);
    EXPECT_EQ(result.publication, Publication::NotPublished);
    EXPECT_NE(result.message.find(".VALIB.lock"), std::string::npos) << result.message;
    EXPECT_NE(result.message.find("host=press-mac"), std::string::npos) << result.message;
    EXPECT_EQ(result.message.find("staging/recovery"), std::string::npos) << result.message;
    EXPECT_EQ(get(lock), marker);
    EXPECT_EQ(get(destination), baseline);
}

TEST_F(ArtworkLibraryStorageTest, WriterLockRecordsDiagnosticHolderAndIsReleased)
{
    std::string content;
    StorageOptions o;
    o.checkpoint = [&](auto const &p) {
        if (p.phase != StoragePhase::Locked) return;
        auto b = read_library_input(p.destination + ".lock", 1024);
        content.assign(b.begin(), b.end());
    };
    auto result = save_library(destination, populated().snapshot(), {}, o);
    ASSERT_EQ(result.publication, Publication::Published) << result.message;
    EXPECT_TRUE(content.starts_with("valib-storage-lock-v1\n")) << content;
    EXPECT_NE(content.find("\nhost="), std::string::npos) << content;
    EXPECT_NE(content.find("\npid="), std::string::npos) << content;
    EXPECT_LE(content.size(), 512u);
    EXPECT_FALSE(g_file_test((destination + ".lock").c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(ArtworkLibraryStorageTest, SymlinkedParentFolderIsRefusedWithActionableMessage)
{
    create(populated());
    auto link = directory + "/linked-folder";
    make_fixture_symlink(link, directory, true);
    try {
        (void)load_library(link + "/Tarjetas 🌸.VALIB");
        FAIL() << "A symlinked parent folder was accepted";
    } catch (StorageError const &e) {
        EXPECT_EQ(e.failure(), StorageFailure::Unsupported);
        EXPECT_NE(std::string(e.what()).find("real folder path"), std::string::npos) << e.what();
    }
}

TEST_F(ArtworkLibraryStorageTest, RecoveryRetentionKeepsNewestVerifiedCopiesOfOneLibrary)
{
    auto catalog = populated(); auto version = create(catalog);
    std::vector<std::string> copies;
    for (unsigned i = 0; i < 7; ++i) {
        catalog.rename_library("Revision " + std::to_string(i));
        auto r = save_library(destination, catalog.snapshot(), version);
        ASSERT_EQ(r.publication, Publication::Published) << r.message;
        copies.push_back(r.recovery_path); version = *r.version;
    }
    // Another library in the same folder keeps its own copies.
    auto other = populated("0d3f7a2e-5b1c-4e8f-9a6d-2c4b8e1f7a90");
    auto other_destination = directory + "/Other.valib";
    auto other_first = save_library(other_destination, other.snapshot());
    ASSERT_EQ(other_first.publication, Publication::Published) << other_first.message;
    other.rename_library("Other next");
    auto other_second = save_library(other_destination, other.snapshot(), other_first.version);
    ASSERT_FALSE(other_second.recovery_path.empty());
    // A damaged file that matches the name pattern is never deleted.
    auto fake = directory + "/.valib-recovery-77bcd302-a034-4b99-9270-139e70273123-r0-" +
                std::string(64, 'a') + "-5d0f4f1e-6c11-4a53-9c43-3b5c0b6a8f10.valib";
    put(fake, bytes("damaged"));
    auto result = prune_library_recovery(destination, "77bcd302-a034-4b99-9270-139e70273123", 5);
    EXPECT_EQ(result.removed.size(), 2u);
    for (std::size_t i = 0; i < copies.size(); ++i)
        EXPECT_EQ(g_file_test(copies[i].c_str(), G_FILE_TEST_EXISTS), i >= 2) << copies[i];
    EXPECT_TRUE(g_file_test(fake.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_TRUE(g_file_test(other_second.recovery_path.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(result.warnings.empty());
    EXPECT_EQ(load_library(destination).version, version);
    // Idempotent once within the window (the damaged file stays with a warning).
    EXPECT_TRUE(prune_library_recovery(destination, "77bcd302-a034-4b99-9270-139e70273123", 5).removed.empty());
}

TEST_F(ArtworkLibraryStorageTest, StaleLockInspectionAndGuardedRemoval)
{
    auto catalog = populated(); auto version = create(catalog);
    EXPECT_FALSE(inspect_library_lock(destination));
    auto lock = destination + ".lock";
    put(lock, bytes("valib-storage-lock-v1\nnonce\nhost=press-mac\npid=42\nutc=2026-09-25T10:00:00Z\n"));
    auto inspected = inspect_library_lock(destination);
    ASSERT_TRUE(inspected);
    EXPECT_EQ(inspected->path, canonical_library_path(lock));
    EXPECT_EQ(inspected->holder, "host=press-mac, pid=42, utc=2026-09-25T10:00:00Z");
    // A lock replaced after inspection (for example by a live writer) is never removed.
    put(lock, bytes("valib-storage-lock-v1\nreplaced by a live writer\n"));
    try {
        remove_stale_library_lock(*inspected);
        FAIL() << "A changed lock was removed";
    } catch (StorageError const &e) {
        EXPECT_EQ(e.failure(), StorageFailure::Conflict);
    }
    EXPECT_EQ(get(lock), bytes("valib-storage-lock-v1\nreplaced by a live writer\n"));
    auto again = inspect_library_lock(destination);
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->holder.empty());
    remove_stale_library_lock(*again);
    EXPECT_FALSE(g_file_test(lock.c_str(), G_FILE_TEST_EXISTS));
    catalog.rename_library("After unlock");
    auto result = save_library(destination, catalog.snapshot(), version);
    EXPECT_EQ(result.publication, Publication::Published) << result.message;
}

TEST_F(ArtworkLibraryStorageTest, RecoveryRetentionNeverRemovesTheNewCopyOrASameIdSiblingsNewerCopies)
{
    auto const id = std::string("77bcd302-a034-4b99-9270-139e70273123");
    auto catalog = populated(); auto a = create(catalog);
    std::vector<std::string> own; // A's copies, oldest first
    for (unsigned i = 0; i < 6; ++i) {
        catalog.rename_library("A " + std::to_string(i));
        auto r = save_library(destination, catalog.snapshot(), a);
        ASSERT_EQ(r.publication, Publication::Published) << r.message; a = *r.version;
        own.push_back(r.recovery_path);
    }
    // A Save As copy in the same folder keeps the library ID and moves ahead.
    auto sibling = directory + "/Sibling.valib";
    auto b = save_library(sibling, catalog.snapshot());
    ASSERT_EQ(b.publication, Publication::Published) << b.message;
    auto bv = *b.version;
    std::vector<std::string> sibling_copies;
    for (unsigned i = 0; i < 7; ++i) {
        catalog.rename_library("B " + std::to_string(i));
        auto r = save_library(sibling, catalog.snapshot(), bv);
        ASSERT_EQ(r.publication, Publication::Published) << r.message;
        sibling_copies.push_back(r.recovery_path); bv = *r.version;
    }
    catalog.rename_library("A again");
    auto latest = save_library(destination, catalog.snapshot(), a);
    ASSERT_EQ(latest.publication, Publication::Published) << latest.message;
    ASSERT_FALSE(latest.recovery_path.empty());
    auto result = prune_library_recovery(destination, id, 5, latest.recovery_path);
    EXPECT_TRUE(result.warnings.empty()) << result.warnings.front();
    // The window below the new copy's revision holds A's six older copies and
    // B's first copy (same revision as A's new copy; names do not identify the
    // source file). Four slots remain, so A's three oldest copies go.
    std::vector<std::string> expected{own[0], own[1], own[2]};
    auto removed = result.removed; std::sort(removed.begin(), removed.end()); std::sort(expected.begin(), expected.end());
    EXPECT_EQ(removed, expected);
    EXPECT_TRUE(g_file_test(latest.recovery_path.c_str(), G_FILE_TEST_EXISTS));
    for (auto const &p : sibling_copies) EXPECT_TRUE(g_file_test(p.c_str(), G_FILE_TEST_EXISTS)) << p;
    for (auto const &p : result.removed)
        EXPECT_EQ(std::find(sibling_copies.begin(), sibling_copies.end(), p), sibling_copies.end()) << p;
}

TEST_F(ArtworkLibraryStorageTest, RecoveryRetentionVerifiesAtMostTheDeletionCapPerCall)
{
    auto catalog = populated(); auto version = create(catalog);
    for (unsigned i = 0; i < 6; ++i) {
        catalog.rename_library("Revision " + std::to_string(i));
        auto r = save_library(destination, catalog.snapshot(), version);
        ASSERT_EQ(r.publication, Publication::Published) << r.message; version = *r.version;
    }
    auto first = prune_library_recovery(destination, "77bcd302-a034-4b99-9270-139e70273123", 1, {}, 2);
    EXPECT_EQ(first.removed.size(), 2u);
    ASSERT_FALSE(first.warnings.empty());
    EXPECT_NE(first.warnings.front().find("remain"), std::string::npos) << first.warnings.front();
    auto rest = prune_library_recovery(destination, "77bcd302-a034-4b99-9270-139e70273123", 1, {}, 10);
    EXPECT_EQ(rest.removed.size(), 3u);
}

