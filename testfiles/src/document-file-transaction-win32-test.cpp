// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Native Windows outcome tests for the bounded F3b local-NTFS new-file
 * adapter.
 *
 * STATUS: OS-only. These tests compile and run on a Windows host only and are
 * intentionally unexecuted/unqualified on macOS. They use the real Win32
 * adapter and real files in a real local directory; the oracles read bytes
 * back independently of the result flags. There are deliberately NO fake
 * volume tests: an unsupported-volume case cannot be counted as qualified here
 * and is covered only by the shared injectable state-machine tests.
 *
 * These tests require a local NTFS working directory. The admission test fails
 * with a clear message (never a silent skip) if the host temp directory is not
 * local NTFS.
 */

#ifdef _WIN32

#include <gtest/gtest.h>

#include <glib.h>
#include <glib/gstdio.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "io/document-file-transaction.h"

namespace {

using namespace Inkscape::IO::DocumentTransaction;

struct TempDir {
    char *path = nullptr;

    TempDir() { path = g_dir_make_tmp("vacards-dftest-XXXXXX", nullptr); }
    ~TempDir()
    {
        if (!path) {
            return;
        }
        GError *err = nullptr;
        GDir *dir = g_dir_open(path, 0, &err);
        if (dir) {
            while (char const *name = g_dir_read_name(dir)) {
                std::string const entry = std::string(path) + "\\" + name;
                g_remove(entry.c_str());
            }
            g_dir_close(dir);
        }
        if (err) {
            g_error_free(err);
        }
        g_rmdir(path);
        g_free(path);
    }

    bool ok() const { return path != nullptr; }
    std::string str() const { return path ? path : std::string(); }
};

LogicalTarget target_in(std::string const &dir)
{
    LogicalTarget target;
    target.final_name = "out.svg";
    target.parent_dir = dir;
    target.final_path = dir + "\\out.svg";
    return target;
}

std::wstring to_wide(std::string const &utf8, bool extended)
{
    std::string normalized = utf8;
    for (char &c : normalized) {
        if (c == '/') {
            c = '\\';
        }
    }
    std::string prefixed;
    if (extended) {
        if (normalized.size() >= 2 && normalized[0] == '\\' && normalized[1] == '\\') {
            prefixed = "\\\\?\\UNC\\" + normalized.substr(2);
        } else {
            prefixed = "\\\\?\\" + normalized;
        }
    } else {
        prefixed = normalized;
    }
    glong units = 0;
    gunichar2 *raw = g_utf8_to_utf16(prefixed.c_str(), -1, nullptr, &units, nullptr);
    if (!raw) {
        return {};
    }
    std::wstring result(reinterpret_cast<wchar_t const *>(raw));
    g_free(raw);
    return result;
}

std::string read_file(std::string const &path)
{
    gchar *contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &length, nullptr)) {
        return std::string();
    }
    std::string const result(contents, length);
    g_free(contents);
    return result;
}

/// Independent read-back for paths beyond MAX_PATH, using the extended prefix
/// rather than the implementation's conversion.
std::string read_file_extended(std::string const &path)
{
    HANDLE handle = CreateFileW(to_wide(path, true).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    std::string result;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        result.append(buffer, read);
    }
    CloseHandle(handle);
    return result;
}

bool has_staging_sibling(std::string const &dir)
{
    GDir *d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) {
        return false;
    }
    bool found = false;
    while (char const *name = g_dir_read_name(d)) {
        if (g_str_has_prefix(name, "vacards-new-")) {
            found = true;
            break;
        }
    }
    g_dir_close(d);
    return found;
}

/// Thin counting delegate over the real Windows adapter, so invalid-target
/// cases can assert that no staging create was attempted.
class CountingCalls final : public SystemCalls
{
public:
    std::unique_ptr<SystemCalls> real = make_platform_system_calls();
    int create_calls = 0;
    int support_calls = 0;

    FailureKind pre_create_support(std::string const &parent_dir, std::string &error) override
    {
        ++support_calls;
        return real->pre_create_support(parent_dir, error);
    }

    bool create_exclusive_file(std::string &path_template, FILE *&out,
                               bool &already_exists, std::string &error) override
    {
        ++create_calls;
        return real->create_exclusive_file(path_template, out, already_exists, error);
    }

    bool flush_file(FILE *stream, std::string &error) override
    {
        return real->flush_file(stream, error);
    }

    bool sync_file(FILE *stream, bool &unsupported, std::string &error) override
    {
        return real->sync_file(stream, unsupported, error);
    }

    bool close_file(FILE *stream, std::string &error) override
    {
        return real->close_file(stream, error);
    }

    PublicationStatus publish_new_file(std::string const &staged_path,
                                       std::string const &final_path,
                                       std::string &error) override
    {
        return real->publish_new_file(staged_path, final_path, error);
    }

    bool remove_file(std::string const &path) noexcept override { return real->remove_file(path); }

    bool sync_parent_directory(std::string const &parent, bool &unsupported,
                               std::string &error) override
    {
        return real->sync_parent_directory(parent, unsupported, error);
    }
};

} // namespace

TEST(WindowsNewDocumentFileTest, LocalNtfsIsAdmittedAndBytesRoundTrip)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    CountingCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << "host must provide a local NTFS temp directory: " << error;
    EXPECT_EQ(calls.support_calls, 1); // admission precedes staging
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("ntfs-round-trip", stream); }, failure, error))
        << error;
    ASSERT_TRUE(tx->seal(failure, error)) << error;

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Published);
    EXPECT_EQ(result.failure, FailureKind::None);
    EXPECT_EQ(read_file(dir.str() + "\\out.svg"), "ntfs-round-trip");
    EXPECT_FALSE(has_staging_sibling(dir.str()));
    EXPECT_FALSE(g_file_test(tx->staged_path().c_str(), G_FILE_TEST_EXISTS));
}

TEST(WindowsNewDocumentFileTest, ExistingDestinationIsConflictWithBytesUnchanged)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const dest = dir.str() + "\\out.svg";
    ASSERT_TRUE(g_file_set_contents(dest.c_str(), "ORIGINAL", 8, nullptr));

    CountingCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;
    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("REPLACEMENT", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Conflict);
    EXPECT_EQ(result.failure, FailureKind::DestinationExists);
    EXPECT_EQ(read_file(dest), "ORIGINAL"); // never replaced
    EXPECT_FALSE(has_staging_sibling(dir.str()));
}

TEST(WindowsNewDocumentFileTest, InvalidTargetsCreateNothing)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    CountingCalls calls;

    auto const make = [&](std::string const &name, std::string const &parent,
                          std::string const &path) {
        LogicalTarget target;
        target.final_name = name;
        target.parent_dir = parent;
        target.final_path = path;
        return target;
    };

    LogicalTarget const cases[] = {
        make("out.svg", "C:", "C:out.svg"),                                   // drive-relative
        make("out.svg", dir.str(), "\\\\?\\" + dir.str() + "\\out.svg"),      // extended prefix
        make("out:ads.svg", dir.str(), dir.str() + "\\out:ads.svg"),          // ADS colon
        make("CON", dir.str(), dir.str() + "\\CON"),                          // reserved DOS name
        make("out.", dir.str(), dir.str() + "\\out."),                        // trailing dot
        make("out ", dir.str(), dir.str() + "\\out "),                        // trailing space
        make("out.svg", dir.str() + "\\..", dir.str() + "\\..\\out.svg"),     // dotdot component
        make("sub/out.svg", dir.str(), dir.str() + "\\sub/out.svg"),          // separator in name
        make("out.svg", dir.str(), dir.str() + "\\other.svg"),                // mismatch
    };

    for (LogicalTarget const &target : cases) {
        FailureKind failure = FailureKind::None;
        std::string error;
        auto tx = NewDocumentFile::create(target, calls, failure, error);
        EXPECT_FALSE(tx) << "target must be rejected: " << target.final_path;
        EXPECT_EQ(failure, FailureKind::InvalidArgument) << target.final_path;
        EXPECT_EQ(calls.support_calls, 0); // invalid targets never reach admission
    }
    EXPECT_EQ(calls.create_calls, 0);
    EXPECT_FALSE(has_staging_sibling(dir.str()));
}

TEST(WindowsNewDocumentFileTest, WindowsInvalidCharactersCreateNothing)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    CountingCalls calls;

    auto const make = [&](std::string const &name, std::string const &parent) {
        LogicalTarget target;
        target.final_name = name;
        target.parent_dir = parent;
        target.final_path = parent + "\\" + name;
        return target;
    };

    std::string const base = dir.str();
    LogicalTarget const cases[] = {
        make("bad*name.svg", base),                 // wildcard in final_name
        make("bad?name.svg", base),
        make("bad<name>.svg", base),
        make("bad>name.svg", base),
        make("bad\"name.svg", base),
        make("bad|name.svg", base),
        make(std::string("bad") + '\x01' + "name.svg", base), // control in final_name
        make("out.svg", base + "\\bad*dir"),        // wildcard in a parent component
        make("out.svg", base + "\\bad?dir"),
        make("out.svg", base + "\\bad<dir"),
        make("out.svg", base + "\\bad>dir"),
        make("out.svg", base + "\\bad|dir"),
        make("out.svg", base + "\\NUL"),            // reserved name parent component
        make("out.svg", base + "\\trailing."),      // trailing dot parent component
        make("out.svg", base + "\\trailing "),      // trailing space parent component
    };

    for (LogicalTarget const &target : cases) {
        FailureKind failure = FailureKind::None;
        std::string error;
        auto tx = NewDocumentFile::create(target, calls, failure, error);
        // create() returning nullptr means no staging stream exists, so no
        // writer/payload call is possible in addition to no staging create.
        EXPECT_FALSE(tx) << "target must be rejected: " << target.final_path;
        EXPECT_EQ(failure, FailureKind::InvalidArgument) << target.final_path;
        EXPECT_EQ(calls.create_calls, 0) << target.final_path;
        EXPECT_EQ(calls.support_calls, 0) << target.final_path;
    }
    EXPECT_FALSE(has_staging_sibling(dir.str()));
}

TEST(WindowsNewDocumentFileTest, RepeatedSeparatorsCreateNothing)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    CountingCalls calls;

    auto const make = [](std::string const &parent, std::string const &name) {
        LogicalTarget target;
        target.final_name = name;
        target.parent_dir = parent;
        target.final_path = parent + "\\" + name;
        return target;
    };

    std::string const base = dir.str();
    LogicalTarget const cases[] = {
        make(base + "\\\\sub", "out.svg"),      // interior doubled separator
        make("\\\\\\server\\share", "out.svg"), // three leading separators
        make(base + "\\/sub", "out.svg"),       // mixed-separator run
    };

    for (LogicalTarget const &target : cases) {
        FailureKind failure = FailureKind::None;
        std::string error;
        auto tx = NewDocumentFile::create(target, calls, failure, error);
        EXPECT_FALSE(tx) << "target must be rejected: " << target.parent_dir;
        EXPECT_EQ(failure, FailureKind::InvalidArgument) << target.parent_dir;
        EXPECT_EQ(calls.create_calls, 0) << target.parent_dir;
        EXPECT_EQ(calls.support_calls, 0) << target.parent_dir;
    }
    EXPECT_FALSE(has_staging_sibling(base));
}

TEST(WindowsNewDocumentFileTest, UnavailableParentFailsBeforeStaging)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    CountingCalls calls;

    // Syntactically valid target whose parent directory does not exist. The
    // admission open fails; that is a resource failure at this moment, not a
    // permanent filesystem-capability gap, and no staging file is attempted.
    LogicalTarget const target = target_in(dir.str() + "\\does-not-exist");
    FailureKind failure = FailureKind::None;
    std::string error;
    auto tx = NewDocumentFile::create(target, calls, failure, error);
    EXPECT_FALSE(tx);
    EXPECT_EQ(failure, FailureKind::StagingCreateFailed);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(calls.support_calls, 1); // admission was reached
    EXPECT_EQ(calls.create_calls, 0);  // but no staging create
    EXPECT_FALSE(has_staging_sibling(dir.str()));
}

TEST(WindowsNewDocumentFileTest, UnicodeFinalNameRoundTripsBytes)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());

    LogicalTarget target;
    target.final_name = "caf\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.svg";
    target.parent_dir = dir.str();
    target.final_path = dir.str() + "\\" + target.final_name;
    std::string const payload = "unicode-\xE2\x9C\x93-bytes";

    CountingCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;
    auto tx = NewDocumentFile::create(target, calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([&payload](FILE *stream) {
        std::fwrite(payload.data(), 1, payload.size(), stream);
    }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    EXPECT_EQ(tx->publish().status, PublicationStatus::Published);
    EXPECT_EQ(read_file(target.final_path), payload);
}

TEST(WindowsNewDocumentFileTest, LongUnicodePathRoundTripsBytes)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());

    // Build a parent beyond MAX_PATH using extended-prefix directories.
    std::string const component_a(120, 'a');
    std::string const component_b(120, 'b');
    std::string const dir_a = dir.str() + "\\" + component_a;
    std::string const parent = dir_a + "\\" + component_b;
    ASSERT_TRUE(CreateDirectoryW(to_wide(dir_a, true).c_str(), nullptr) != FALSE);
    ASSERT_TRUE(CreateDirectoryW(to_wide(parent, true).c_str(), nullptr) != FALSE);

    LogicalTarget target;
    target.final_name = "long-\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.svg";
    target.parent_dir = parent;
    target.final_path = parent + "\\" + target.final_name;
    ASSERT_GT(target.final_path.size(), static_cast<std::size_t>(260));
    std::string const payload = "long-path-\xE2\x9C\x93";

    CountingCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;
    auto tx = NewDocumentFile::create(target, calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([&payload](FILE *stream) {
        std::fwrite(payload.data(), 1, payload.size(), stream);
    }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    EXPECT_EQ(tx->publish().status, PublicationStatus::Published);
    EXPECT_EQ(read_file_extended(target.final_path), payload);

    RemoveDirectoryW(to_wide(parent, true).c_str());
    RemoveDirectoryW(to_wide(dir_a, true).c_str());
}

TEST(WindowsNewDocumentFileTest, ForeignStageAtOwnedPathIsPreserved)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    CountingCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("owned", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error)); // closes the stream; the recorded identity remains

    std::string const staged = tx->staged_path();
    ASSERT_EQ(g_remove(staged.c_str()), 0);
    ASSERT_TRUE(g_file_set_contents(staged.c_str(), "FOREIGN", 7, nullptr));

    tx->abort(); // single cleanup attempt must preserve the foreign object

    EXPECT_TRUE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(read_file(staged), "FOREIGN");
    EXPECT_FALSE(g_file_test((dir.str() + "\\out.svg").c_str(), G_FILE_TEST_EXISTS));

    tx.reset();
    // Consumed cleanup: destruction never deletes the recreated foreign file.
    EXPECT_TRUE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(read_file(staged), "FOREIGN");
}

TEST(WindowsNewDocumentFileTest, UnknownStagingPathIsNeverRemoved)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const stranger = dir.str() + "\\vacards-new-stranger";
    ASSERT_TRUE(g_file_set_contents(stranger.c_str(), "STRANGER", 8, nullptr));

    CountingCalls calls;
    // A path this adapter never created carries no identity and must be left.
    EXPECT_FALSE(calls.remove_file(stranger));
    EXPECT_TRUE(g_file_test(stranger.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(read_file(stranger), "STRANGER");
}

#endif // _WIN32
