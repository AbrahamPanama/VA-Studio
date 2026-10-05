// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <ctime>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "io/existing-file-replacement.h"
#include "io/document-file-transaction.h"

#ifdef __APPLE__
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

// Test-only seam implemented in src/io/existing-file-replacement.cpp. It has no
// public header declaration by design; this forward declaration arms a single
// injected errno for the rename step, consumed by the next save that reaches it.
namespace Inkscape::IO::detail {
void set_rename_failure_errno_for_testing(int errno_value);
void set_recovery_delete_failure_errno_for_testing(int errno_value);
}

namespace {

// Scoped owner of the one-shot rename-failure injection. The injection is
// consumed only when a save reaches the rename step, so an early ASSERT_* return
// would otherwise leave it armed for an unrelated later save on this thread.
// Resetting in the destructor keeps the seam scoped to this test.
struct ScopedRenameFailureInjection {
    explicit ScopedRenameFailureInjection(int errno_value)
    {
        Inkscape::IO::detail::set_rename_failure_errno_for_testing(errno_value);
    }
    ~ScopedRenameFailureInjection()
    {
        Inkscape::IO::detail::set_rename_failure_errno_for_testing(0);
    }
    ScopedRenameFailureInjection(ScopedRenameFailureInjection const &) = delete;
    ScopedRenameFailureInjection &operator=(ScopedRenameFailureInjection const &) = delete;
};

struct ScopedRecoveryDeleteFailureInjection {
    explicit ScopedRecoveryDeleteFailureInjection(int errno_value)
    {
        Inkscape::IO::detail::set_recovery_delete_failure_errno_for_testing(errno_value);
    }
    ~ScopedRecoveryDeleteFailureInjection()
    {
        Inkscape::IO::detail::set_recovery_delete_failure_errno_for_testing(0);
    }
};

struct TempDir {
    gchar *path = g_dir_make_tmp("vacards-existing-save-XXXXXX", nullptr);
    ~TempDir()
    {
        if (!path) return;
        GDir *dir = g_dir_open(path, 0, nullptr);
        if (dir) {
            while (char const *name = g_dir_read_name(dir)) {
                auto const file = std::string(path) + "/" + name;
                // Disposable files may carry UF_IMMUTABLE, or be hard links to
                // an immutable inode. Clear every flag before removal so the
                // scratch directory always tears down. lchflags does not follow
                // symlinks and only ever acts on paths inside this TempDir.
                ::lchflags(file.c_str(), 0);
                g_remove(file.c_str());
            }
            g_dir_close(dir);
        }
        g_rmdir(path);
        g_free(path);
    }
    std::string file(char const *name) const { return std::string(path) + "/" + name; }
};

std::string bytes(std::string const &path)
{
    gchar *contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &length, nullptr)) return {};
    std::string result(contents, length);
    g_free(contents);
    return result;
}

void write(std::string const &path, char const *content)
{
    ASSERT_TRUE(g_file_set_contents(path.c_str(), content, -1, nullptr));
}

// Names of scratch entries in a disposable directory that begin with prefix.
std::vector<std::string> entries_with_prefix(std::string const &dir_path, char const *prefix)
{
    std::vector<std::string> matches;
    GDir *dir = g_dir_open(dir_path.c_str(), 0, nullptr);
    if (!dir) return matches;
    while (char const *name = g_dir_read_name(dir)) {
        if (g_str_has_prefix(name, prefix)) matches.emplace_back(name);
    }
    g_dir_close(dir);
    return matches;
}

// Human-readable, non-mutating evidence for a scratch entry, including the
// BSD flags that explain why removal may have failed.
std::string flags_text(std::string const &path)
{
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return path + " (lstat failed)";
    char buffer[160];
    g_snprintf(buffer, sizeof buffer, "%s flags=0x%08x nlink=%u size=%lld", path.c_str(),
               static_cast<unsigned>(st.st_flags), static_cast<unsigned>(st.st_nlink),
               static_cast<long long>(st.st_size));
    return buffer;
}

TEST(ExistingFileReplacement, PublishesCompleteFileAndPreservesMode)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("renamé.svg");
    write(target, "old SVG");
    ASSERT_EQ(::chmod(target.c_str(), 0640), 0);
    char const metadata[] = "preserve me";
    ASSERT_EQ(::setxattr(target.c_str(), "com.vacards.test", metadata,
                         sizeof(metadata), 0, 0), 0);
    auto const result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        ASSERT_EQ(std::fwrite("new SVG", 1, 7, stream), 7u);
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Published) << result.error;
    EXPECT_EQ(bytes(target), "new SVG");
    struct stat st{};
    ASSERT_EQ(::stat(target.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0640);
    char restored[sizeof(metadata)]{};
    EXPECT_EQ(::getxattr(target.c_str(), "com.vacards.test", restored,
                         sizeof(restored), 0, 0), static_cast<ssize_t>(sizeof(metadata)));
    EXPECT_STREQ(restored, metadata);
}

TEST(ExistingFileReplacement, ByteWriterPreservesMacMetadata)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const callback_path = dir.file("callback.svg");
    auto const bytes_path = dir.file("bytes.svg");
    write(callback_path, "original");
    write(bytes_path, "original");
    for (auto const &path : {callback_path, bytes_path}) {
        ASSERT_EQ(::chmod(path.c_str(), 0640), 0);
        ASSERT_EQ(::setxattr(path.c_str(), "com.vacards.test", "meta", 5, 0, 0), 0);
    }
    std::string const payload("new\0SVG", 7);
    auto const callback = Inkscape::IO::replace_existing_local_file(callback_path, [&](FILE *file) {
        if (std::fwrite(payload.data(), 1, payload.size(), file) != payload.size()) {
            throw std::runtime_error("callback write failed");
        }
    });
    auto const bytes_result = Inkscape::IO::replace_existing_local_file(
        bytes_path, std::as_bytes(std::span(payload.data(), payload.size())));
    EXPECT_EQ(callback.outcome, Inkscape::IO::ExistingFileOutcome::Published) << callback.error;
    EXPECT_EQ(bytes_result.outcome, callback.outcome) << bytes_result.error;
    EXPECT_EQ(bytes(bytes_path), payload);
    EXPECT_EQ(bytes(bytes_path), bytes(callback_path));
    struct stat callback_stat{}, bytes_stat{};
    ASSERT_EQ(::stat(callback_path.c_str(), &callback_stat), 0);
    ASSERT_EQ(::stat(bytes_path.c_str(), &bytes_stat), 0);
    EXPECT_EQ(callback_stat.st_mode & 0777, bytes_stat.st_mode & 0777);
    char callback_xattr[5]{}, bytes_xattr[5]{};
    EXPECT_EQ(::getxattr(callback_path.c_str(), "com.vacards.test", callback_xattr, 5, 0, 0), 5);
    EXPECT_EQ(::getxattr(bytes_path.c_str(), "com.vacards.test", bytes_xattr, 5, 0, 0), 5);
    EXPECT_EQ(std::string(callback_xattr, 5), std::string(bytes_xattr, 5));
}

TEST(ExistingFileReplacement, SavedSvgDoesNotKeepTheReplacedFinderIcon)
{
    // VIEW-1: the Finder icon shows the drawing of the save that set it, so a
    // new version must not inherit it; other Finder information is kept.
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const svg = dir.file("card.svg");
    auto const png = dir.file("card.png");
    unsigned char info[32] = {};
    info[8] = 0x04;  // kHasCustomIcon
    info[9] = 0x0e;  // label colour bits: must survive
    for (auto const &path : {svg, png}) {
        write(path, "old");
        ASSERT_EQ(::setxattr(path.c_str(), XATTR_RESOURCEFORK_NAME, "icon", 4, 0, 0), 0);
        ASSERT_EQ(::setxattr(path.c_str(), XATTR_FINDERINFO_NAME, info, sizeof(info), 0, 0), 0);
        auto const result = Inkscape::IO::replace_existing_local_file(path, [](FILE *stream) {
            ASSERT_EQ(std::fwrite("new", 1, 3, stream), 3u);
        });
        ASSERT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Published) << result.error;
    }
    unsigned char after[32] = {};
    EXPECT_LT(::getxattr(svg.c_str(), XATTR_RESOURCEFORK_NAME, nullptr, 0, 0, 0), 0);
    ASSERT_EQ(::getxattr(svg.c_str(), XATTR_FINDERINFO_NAME, after, sizeof(after), 0, 0), 32);
    EXPECT_EQ(after[8] & 0x04, 0);
    EXPECT_EQ(after[9], 0x0e);
    // Not an SVG: VA Studio does not manage its icon, so it is kept.
    EXPECT_EQ(::getxattr(png.c_str(), XATTR_RESOURCEFORK_NAME, nullptr, 0, 0, 0), 4);
}

TEST(ExistingFileReplacement, WriterFailureLeavesOldDestination)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("old.svg");
    write(target, "complete old bytes");
    auto const result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        std::fwrite("partial", 1, 7, stream);
        throw std::runtime_error("injected writer failure");
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::FailedBeforePublication);
    EXPECT_EQ(bytes(target), "complete old bytes");
}

TEST(ExistingFileReplacement, SuccessfulSaveReportsRetainedOldCopy)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("saved.svg");
    write(target, "complete old file");
    Inkscape::IO::ExistingFileResult result;
    {
        ScopedRecoveryDeleteFailureInjection const failure(EACCES);
        result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
            ASSERT_EQ(std::fwrite("complete new file", 1, 17, stream), 17u);
        });
    }
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Published) << result.error;
    EXPECT_EQ(bytes(target), "complete new file");
    ASSERT_FALSE(result.recovery_path.empty());
    EXPECT_EQ(bytes(result.recovery_path), "complete old file");
    EXPECT_NE(result.error.find("remove old recovery copy"), std::string::npos);
}

// Opt-in real SMB fixture. The parent is supplied explicitly; this test owns
// only its unique child and keeps it on failure for inspection.
TEST(ExistingFileReplacement, SmbScratchPublishesAndKeepsCompleteOldVersionUntilConfirmed)
{
    char const *parent = g_getenv("VACARDS_NAS_SCRATCH");
    if (!parent || !*parent) GTEST_SKIP() << "set VACARDS_NAS_SCRATCH to a writable scratch parent";
    gchar *uuid = g_uuid_string_random();
    std::string const dir = std::string(parent) + "/vacards-existing-test-" + uuid;
    g_free(uuid);
    ASSERT_EQ(g_mkdir(dir.c_str(), 0700), 0);
    std::string const target = dir + "/existing.svg";
    write(target, "complete original SVG");
    auto const result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        ASSERT_EQ(std::fwrite("complete replacement SVG", 1, 24, stream), 24u);
    });
    bool const correct = result.outcome == Inkscape::IO::ExistingFileOutcome::Published
        && result.error.empty() && bytes(target) == "complete replacement SVG"
        && entries_with_prefix(dir, "vacards-save-").empty()
        && entries_with_prefix(dir, "vacards-recovery-").empty()
        && entries_with_prefix(dir, ".smbdelete").empty();
    EXPECT_TRUE(correct) << result.error << " scratch=" << dir;
    if (correct) {
        EXPECT_EQ(g_remove(target.c_str()), 0);
        EXPECT_EQ(g_rmdir(dir.c_str()), 0);
    }
}

// Opt-in real SMB fixture for a NEW filename: the production adapter must
// publish without link(2) through the claim-and-rename fallback, and a second
// save to the same name must refuse rather than replace.
TEST(ExistingFileReplacement, SmbScratchNewFileSaveThroughProductionAdapter)
{
    using namespace Inkscape::IO::DocumentTransaction;
    char const *parent = g_getenv("VACARDS_NAS_SCRATCH");
    if (!parent || !*parent) GTEST_SKIP() << "set VACARDS_NAS_SCRATCH to a writable scratch parent";
    gchar *uuid = g_uuid_string_random();
    std::string const dir = std::string(parent) + "/vacards-new-file-test-" + uuid;
    g_free(uuid);
    ASSERT_EQ(g_mkdir(dir.c_str(), 0700), 0);
    LogicalTarget target;
    target.final_name = "new.svg";
    target.parent_dir = dir;
    target.final_path = dir + "/new.svg";
    std::unique_ptr<SystemCalls> calls = make_platform_system_calls();
    auto save = [&](char const *text) {
        FailureKind failure = FailureKind::None;
        std::string error;
        auto tx = NewDocumentFile::create(target, *calls, failure, error);
        EXPECT_TRUE(tx) << error;
        if (!tx) return PublicationStatus::NotAttempted;
        EXPECT_TRUE(tx->write([text](FILE *stream) { std::fputs(text, stream); }, failure, error)) << error;
        EXPECT_TRUE(tx->seal(failure, error)) << error;
        PublicationResult const result = tx->publish();
        EXPECT_TRUE(result.error.empty() || result.status != PublicationStatus::Published) << result.error;
        return result.status;
    };
    PublicationStatus const first = save("complete new SVG");
    PublicationStatus const second = save("second save");
    bool const correct = first == PublicationStatus::Published && second == PublicationStatus::Conflict
        && bytes(target.final_path) == "complete new SVG"
        && entries_with_prefix(dir, "vacards-new-").empty()
        && entries_with_prefix(dir, ".smbdelete").empty();
    EXPECT_TRUE(correct) << "first=" << to_string(first) << " second=" << to_string(second)
                         << " scratch=" << dir;
    if (correct) {
        EXPECT_EQ(g_remove(target.final_path.c_str()), 0);
        EXPECT_EQ(g_rmdir(dir.c_str()), 0);
    }
}

TEST(ExistingFileReplacement, SmbScratchRenameFailureKeepsOldDestination)
{
    char const *parent = g_getenv("VACARDS_NAS_SCRATCH");
    if (!parent || !*parent) GTEST_SKIP() << "set VACARDS_NAS_SCRATCH to a writable scratch parent";
    gchar *uuid = g_uuid_string_random();
    std::string const dir = std::string(parent) + "/vacards-failed-replace-test-" + uuid;
    g_free(uuid);
    ASSERT_EQ(g_mkdir(dir.c_str(), 0700), 0);
    std::string const target = dir + "/existing.svg";
    write(target, "complete original SVG");
    Inkscape::IO::ExistingFileResult result;
    {
        ScopedRenameFailureInjection const fault(EACCES);
        result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
            ASSERT_EQ(std::fwrite("candidate SVG", 1, 13, stream), 13u);
        });
    }
    bool const correct = result.outcome == Inkscape::IO::ExistingFileOutcome::FailedBeforePublication
        && bytes(target) == "complete original SVG"
        && entries_with_prefix(dir, "vacards-save-").empty()
        && entries_with_prefix(dir, "vacards-recovery-").empty()
        && entries_with_prefix(dir, ".smbdelete").empty();
    EXPECT_TRUE(correct) << result.error << " scratch=" << dir;
    if (correct) {
        EXPECT_EQ(g_remove(target.c_str()), 0);
        EXPECT_EQ(g_rmdir(dir.c_str()), 0);
    }
}

TEST(ExistingFileReplacement, SmbScratchSuccessfulSaveReportsRetainedRecovery)
{
    char const *parent = g_getenv("VACARDS_NAS_SCRATCH");
    if (!parent || !*parent) GTEST_SKIP() << "set VACARDS_NAS_SCRATCH to a writable scratch parent";
    gchar *uuid = g_uuid_string_random();
    std::string const dir = std::string(parent) + "/vacards-recovery-warning-test-" + uuid;
    g_free(uuid);
    ASSERT_EQ(g_mkdir(dir.c_str(), 0700), 0);
    std::string const target = dir + "/existing.svg";
    write(target, "complete old SVG");
    Inkscape::IO::ExistingFileResult result;
    {
        ScopedRecoveryDeleteFailureInjection const fault(EACCES);
        result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
            ASSERT_EQ(std::fwrite("complete new SVG", 1, 16, stream), 16u);
        });
    }
    bool const correct = result.outcome == Inkscape::IO::ExistingFileOutcome::Published
        && bytes(target) == "complete new SVG"
        && !result.recovery_path.empty()
        && bytes(result.recovery_path) == "complete old SVG"
        && result.error.find("remove SMB recovery copy") != std::string::npos;
    EXPECT_TRUE(correct) << result.error << " scratch=" << dir;
    if (correct) {
        EXPECT_EQ(g_remove(result.recovery_path.c_str()), 0);
        EXPECT_EQ(g_remove(target.c_str()), 0);
        EXPECT_EQ(g_rmdir(dir.c_str()), 0);
    }
}

TEST(ExistingFileReplacement, RefusesSymbolicAndHardLinks)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const source = dir.file("source.svg");
    auto const symbolic = dir.file("alias.svg");
    auto const hard = dir.file("hard.svg");
    write(source, "old");
    ASSERT_EQ(::symlink(source.c_str(), symbolic.c_str()), 0);
    auto writer = [](FILE *stream) { std::fwrite("new", 1, 3, stream); };
    auto result = Inkscape::IO::replace_existing_local_file(symbolic, writer);
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported);
    ASSERT_EQ(::link(source.c_str(), hard.c_str()), 0);
    result = Inkscape::IO::replace_existing_local_file(source, writer);
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported);
    EXPECT_EQ(bytes(source), "old");
}

TEST(ExistingFileReplacement, DetectsConcurrentChangeBeforePublication)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("changed.svg");
    write(target, "first");
    auto const result = Inkscape::IO::replace_existing_local_file(target, [&](FILE *stream) {
        std::fwrite("candidate", 1, 9, stream);
        write(target, "external change");
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Conflict);
    EXPECT_EQ(bytes(target), "external change");
}

// Injecting a rename failure must leave the original inode, bytes and nlink=1
// intact, remove the owned backup link so the next Save's nlink preflight
// passes, and leave no owned scratch behind. The seam is one-shot, so a second
// ordinary Save must then publish.
TEST(ExistingFileReplacement, RenameFailureRestoresNlinkAndAllowsNextSave)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("rename-failure.svg");
    write(target, "baseline old bytes");
    struct stat baseline{};
    ASSERT_EQ(::stat(target.c_str(), &baseline), 0);
    ASSERT_EQ(baseline.st_nlink, 1);

    ScopedRenameFailureInjection const injection(EACCES);
    auto const failed = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        ASSERT_EQ(std::fwrite("candidate new bytes", 1, 19, stream), 19u);
    });
    EXPECT_EQ(failed.outcome, Inkscape::IO::ExistingFileOutcome::FailedBeforePublication)
        << failed.error;
    // Publication definitely did not happen, so no prior version is retained and
    // the caller must not be told that one is. The error must still identify the
    // replacement failure that produced this outcome.
    EXPECT_TRUE(failed.recovery_path.empty()) << "unexpected recovery path: " << failed.recovery_path;
    EXPECT_NE(failed.error.find("replace destination"), std::string::npos) << failed.error;
    EXPECT_NE(failed.error.find(std::strerror(EACCES)), std::string::npos) << failed.error;
    EXPECT_EQ(bytes(target), "baseline old bytes");

    struct stat after{};
    ASSERT_EQ(::stat(target.c_str(), &after), 0);
    EXPECT_EQ(after.st_dev, baseline.st_dev);
    EXPECT_EQ(after.st_ino, baseline.st_ino);
    EXPECT_EQ(after.st_nlink, 1);

    auto const stages = entries_with_prefix(dir.path, "vacards-save-");
    auto const recoveries = entries_with_prefix(dir.path, "vacards-recovery-");
    std::string evidence;
    for (auto const &name : stages) evidence += flags_text(dir.file(name.c_str())) + "\n";
    for (auto const &name : recoveries) evidence += flags_text(dir.file(name.c_str())) + "\n";
    EXPECT_TRUE(stages.empty()) << "rename failure left an owned stage:\n" << evidence;
    EXPECT_TRUE(recoveries.empty()) << "rename failure left an owned recovery link:\n" << evidence;

    auto const published = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        ASSERT_EQ(std::fwrite("published new bytes", 1, 19, stream), 19u);
    });
    EXPECT_EQ(published.outcome, Inkscape::IO::ExistingFileOutcome::Published) << published.error;
    EXPECT_EQ(bytes(target), "published new bytes");
    struct stat published_stat{};
    ASSERT_EQ(::stat(target.c_str(), &published_stat), 0);
    EXPECT_EQ(published_stat.st_nlink, 1);
    EXPECT_NE(published_stat.st_ino, baseline.st_ino);
}

TEST(ExistingFileReplacement, PublishedDestinationTimeIsNotOlderThanSaveStart)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("old-time.svg");
    write(target, "old SVG");
    ASSERT_EQ(::chmod(target.c_str(), 0640), 0);
    char const metadata[] = "preserve me";
    ASSERT_EQ(::setxattr(target.c_str(), "com.vacards.test", metadata,
                         sizeof(metadata), 0, 0), 0);

    struct timespec old_time[2];
    old_time[0].tv_sec = 946684800; // 2000-01-01T00:00:00Z
    old_time[0].tv_nsec = 0;
    old_time[1] = old_time[0];
    ASSERT_EQ(::utimensat(AT_FDCWD, target.c_str(), old_time, 0), 0);
    struct stat before{};
    ASSERT_EQ(::stat(target.c_str(), &before), 0);
    ASSERT_EQ(before.st_mtimespec.tv_sec, 946684800);

    auto const start = std::time(nullptr);
    auto const result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        ASSERT_EQ(std::fwrite("new SVG", 1, 7, stream), 7u);
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Published) << result.error;
    EXPECT_EQ(bytes(target), "new SVG");
    struct stat published{};
    ASSERT_EQ(::stat(target.c_str(), &published), 0);
    EXPECT_GE(published.st_mtimespec.tv_sec, start);
    EXPECT_NE(published.st_mtimespec.tv_sec, 946684800);
    EXPECT_EQ(published.st_mode & 0777, 0640);
    char restored[sizeof(metadata)]{};
    EXPECT_EQ(::getxattr(target.c_str(), "com.vacards.test", restored,
                         sizeof(restored), 0, 0), static_cast<ssize_t>(sizeof(metadata)));
    EXPECT_STREQ(restored, metadata);
}

// UF_HIDDEN is a discretionary BSD flag. COPYFILE_METADATA is expected to carry
// it across to the stage, and publication must expose the new bytes with the
// flag intact, a fresh mtime, and no vacards-* scratch files left behind.
TEST(ExistingFileReplacement, PreservesHiddenFlagWhenPublishing)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("hidden.svg");
    write(target, "old hidden");
    ASSERT_EQ(::chflags(target.c_str(), UF_HIDDEN), 0);
    struct stat hidden_before{};
    ASSERT_EQ(::stat(target.c_str(), &hidden_before), 0);
    ASSERT_TRUE((hidden_before.st_flags & UF_HIDDEN) != 0);

    struct timespec old_time[2];
    old_time[0].tv_sec = 946684800; // 2000-01-01T00:00:00Z
    old_time[0].tv_nsec = 0;
    old_time[1] = old_time[0];
    ASSERT_EQ(::utimensat(AT_FDCWD, target.c_str(), old_time, 0), 0);

    auto const start = std::time(nullptr);
    auto const result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        ASSERT_EQ(std::fwrite("new hidden", 1, 10, stream), 10u);
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Published) << result.error;
    EXPECT_EQ(bytes(target), "new hidden");

    struct stat published{};
    ASSERT_EQ(::stat(target.c_str(), &published), 0);
    EXPECT_TRUE((published.st_flags & UF_HIDDEN) != 0)
        << "published destination lost UF_HIDDEN: " << flags_text(target);
    EXPECT_GE(published.st_mtimespec.tv_sec, start);
    EXPECT_NE(published.st_mtimespec.tv_sec, 946684800);

    auto const leftovers = entries_with_prefix(dir.path, "vacards-");
    std::string evidence;
    for (auto const &name : leftovers) evidence += flags_text(dir.file(name.c_str())) + "\n";
    EXPECT_TRUE(leftovers.empty()) << "unexpected owned scratch leftovers:\n" << evidence;
}

// Reject an immutable target before staging. It must never damage the original
// or leave an owned stage behind.
TEST(ExistingFileReplacement, ImmutableTargetOutcomeIsSafeAndLeavesNoLiveStage)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("immutable.svg");
    write(target, "immutable original");
    ASSERT_EQ(::chflags(target.c_str(), UF_IMMUTABLE), 0);
    struct stat immutable_before{};
    ASSERT_EQ(::stat(target.c_str(), &immutable_before), 0);
    ASSERT_TRUE((immutable_before.st_flags & UF_IMMUTABLE) != 0);

    auto const result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        std::fwrite("candidate replacement", 1, 21, stream);
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported) << result.error;
    RecordProperty("outcome", static_cast<int>(result.outcome));
    RecordProperty("error", result.error);
    RecordProperty("recovery_path", result.recovery_path);

    struct stat after{};
    EXPECT_EQ(::stat(target.c_str(), &after), 0);
    if (result.outcome != Inkscape::IO::ExistingFileOutcome::Published) {
        EXPECT_EQ(bytes(target), "immutable original")
            << "unpublished immutable replacement must not damage the original";
    }

    auto const stages = entries_with_prefix(dir.path, "vacards-save-");
    auto const recoveries = entries_with_prefix(dir.path, "vacards-recovery-");
    RecordProperty("stage_leftovers", static_cast<int>(stages.size()));
    RecordProperty("recovery_leftovers", static_cast<int>(recoveries.size()));

    std::string evidence;
    for (auto const &name : stages) evidence += flags_text(dir.file(name.c_str())) + "\n";
    for (auto const &name : recoveries) evidence += flags_text(dir.file(name.c_str())) + "\n";

    EXPECT_TRUE(stages.empty())
        << "immutable replacement left an owned stage file that could not be removed:\n"
        << evidence;
    // Recovery copies are legal and are surfaced via result.recovery_path. They
    // are only reported here; teardown clears their flags and removes them.
    if (!recoveries.empty()) RecordProperty("recovery_evidence", evidence);
}

} // namespace

#elif defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <io.h>

#include "util-string/string-convert.h"

// Test-only seam implemented in src/io/existing-file-replacement-win32.cpp. It
// has no public header declaration by design. kind 0 disarms; kind 1 returns the
// injected code with no namespace mutation; kind 2 reproduces the 1177 mixed
// state (old file moved to the chosen backup, new content left at the stage).
namespace Inkscape::IO::detail {
void set_replace_fault_for_testing(unsigned long code, int kind);
}

namespace {

// Scoped owner of the one-shot publication-fault injection. Resetting in the
// destructor keeps the seam from leaking into an unrelated later save on this
// thread when an ASSERT_* returns early.
struct ScopedReplaceFault {
    ScopedReplaceFault(unsigned long code, int kind)
    {
        Inkscape::IO::detail::set_replace_fault_for_testing(code, kind);
    }
    ~ScopedReplaceFault()
    {
        Inkscape::IO::detail::set_replace_fault_for_testing(0, 0);
    }
    ScopedReplaceFault(ScopedReplaceFault const &) = delete;
    ScopedReplaceFault &operator=(ScopedReplaceFault const &) = delete;
};

std::wstring wide(std::string const &utf8)
{
    return Inkscape::utf8_to_wstring(utf8);
}

struct PathInfo {
    bool ok = false;
    ULONGLONG volume = 0;
    DWORD index_high = 0;
    DWORD index_low = 0;
    DWORD links = 0;
    DWORD attributes = 0;
    ULONGLONG size = 0;
};

PathInfo query_path(std::string const &path)
{
    PathInfo out;
    HANDLE const handle = CreateFileW(wide(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return out;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle, &info)) {
        out.ok = true;
        out.volume = info.dwVolumeSerialNumber;
        out.index_high = info.nFileIndexHigh;
        out.index_low = info.nFileIndexLow;
        out.links = info.nNumberOfLinks;
        out.attributes = info.dwFileAttributes;
        out.size = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    }
    CloseHandle(handle);
    return out;
}

bool same_identity(PathInfo const &a, PathInfo const &b)
{
    return a.ok && b.ok && a.volume == b.volume
        && a.index_high == b.index_high && a.index_low == b.index_low;
}

std::string bytes(std::string const &path)
{
    gchar *contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &length, nullptr)) return {};
    std::string result(contents, length);
    g_free(contents);
    return result;
}

void write(std::string const &path, char const *content)
{
    ASSERT_TRUE(g_file_set_contents(path.c_str(), content, -1, nullptr));
}

std::vector<std::string> entries_with_prefix(std::string const &dir_path, char const *prefix)
{
    std::vector<std::string> matches;
    GDir *dir = g_dir_open(dir_path.c_str(), 0, nullptr);
    if (!dir) return matches;
    while (char const *name = g_dir_read_name(dir)) {
        if (g_str_has_prefix(name, prefix)) matches.emplace_back(name);
    }
    g_dir_close(dir);
    return matches;
}

// A disposable scratch directory that clears read-only/system/hidden before
// removal so teardown always succeeds even when a case set an attribute.
struct TempDir {
    gchar *path = g_dir_make_tmp("vacards-existing-save-XXXXXX", nullptr);
    ~TempDir()
    {
        if (!path) return;
        GDir *dir = g_dir_open(path, 0, nullptr);
        if (dir) {
            while (char const *name = g_dir_read_name(dir)) {
                auto const file = std::string(path) + "/" + name;
                SetFileAttributesW(wide(file).c_str(), FILE_ATTRIBUTE_NORMAL);
                g_remove(file.c_str());
                RemoveDirectoryW(wide(file).c_str());
            }
            g_dir_close(dir);
        }
        g_rmdir(path);
        g_free(path);
    }
    std::string file(char const *name) const { return std::string(path) + "/" + name; }
};

TEST(ExistingFileReplacement, PublishesCompleteFileWithNewIdentityAndNoLeftovers)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("renamé.svg");
    write(target, "old SVG");
    PathInfo const before = query_path(target);
    ASSERT_TRUE(before.ok);
    ASSERT_EQ(before.links, 1u);

    auto const result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        ASSERT_EQ(std::fwrite("new SVG", 1, 7, stream), 7u);
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Published) << result.error;
    EXPECT_EQ(bytes(target), "new SVG");

    PathInfo const after = query_path(target);
    ASSERT_TRUE(after.ok);
    EXPECT_EQ(after.links, 1u);
    EXPECT_FALSE(same_identity(before, after)) << "destination identity did not change";
    EXPECT_TRUE(entries_with_prefix(dir.path, "vacards-").empty());
}

TEST(ExistingFileReplacement, WriterFailureLeavesOldDestination)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("old.svg");
    write(target, "complete old bytes");
    PathInfo const before = query_path(target);
    ASSERT_TRUE(before.ok);

    auto const result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        std::fwrite("partial", 1, 7, stream);
        throw std::runtime_error("injected writer failure");
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::FailedBeforePublication) << result.error;
    EXPECT_EQ(bytes(target), "complete old bytes");

    PathInfo const after = query_path(target);
    ASSERT_TRUE(after.ok);
    EXPECT_TRUE(same_identity(before, after));
    EXPECT_EQ(after.links, 1u);
    EXPECT_TRUE(entries_with_prefix(dir.path, "vacards-").empty());
}

TEST(ExistingFileReplacement, DetectsConcurrentChangeBeforePublication)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("changed.svg");
    write(target, "first");

    auto const result = Inkscape::IO::replace_existing_local_file(target, [&](FILE *stream) {
        std::fwrite("candidate", 1, 9, stream);
        write(target, "external change");
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Conflict) << result.error;
    EXPECT_EQ(bytes(target), "external change");
    EXPECT_TRUE(entries_with_prefix(dir.path, "vacards-").empty());
}

TEST(ExistingFileReplacement, RefusesReadOnlyDestinationAndDoesNotCallWriter)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("readonly.svg");
    write(target, "readonly original");
    ASSERT_TRUE(SetFileAttributesW(wide(target).c_str(), FILE_ATTRIBUTE_READONLY));

    bool called = false;
    auto const result = Inkscape::IO::replace_existing_local_file(target, [&](FILE *) {
        called = true;
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported) << result.error;
    EXPECT_FALSE(called);
    EXPECT_EQ(bytes(target), "readonly original");
    ASSERT_TRUE(SetFileAttributesW(wide(target).c_str(), FILE_ATTRIBUTE_NORMAL));
}

TEST(ExistingFileReplacement, SharingViolationIsRetryableAndPreservesDestination)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("locked.svg");
    write(target, "locked original");
    HANDLE const lock = CreateFileW(wide(target).c_str(), GENERIC_READ, 0,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);

    auto const result = Inkscape::IO::replace_existing_local_file(target, [&](FILE *stream) {
        std::fwrite("candidate", 1, 9, stream);
    });
    CloseHandle(lock);
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::FailedBeforePublication)
        << result.error;
    EXPECT_EQ(bytes(target), "locked original");
    EXPECT_TRUE(entries_with_prefix(dir.path, "vacards-").empty());
}

TEST(ExistingFileReplacement, RefusesDirectoryAndDoesNotCallWriter)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const directory = dir.file("folder");
    ASSERT_TRUE(CreateDirectoryW(wide(directory).c_str(), nullptr) != FALSE);

    bool called = false;
    auto const result = Inkscape::IO::replace_existing_local_file(directory, [&](FILE *) {
        called = true;
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported) << result.error;
    EXPECT_FALSE(called);
    EXPECT_TRUE(entries_with_prefix(dir.path, "vacards-").empty());
}

TEST(ExistingFileReplacement, RefusesSymbolicLinkWhenCreatable)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const source = dir.file("source.svg");
    auto const symbolic = dir.file("alias.svg");
    write(source, "old");
    if (!CreateSymbolicLinkW(wide(symbolic).c_str(), wide(source).c_str(), 0)) {
        GTEST_SKIP() << "CreateSymbolicLinkW unavailable: Windows error "
                     << static_cast<unsigned long>(GetLastError());
    }

    bool called = false;
    auto const result = Inkscape::IO::replace_existing_local_file(symbolic, [&](FILE *) {
        called = true;
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported) << result.error;
    EXPECT_FALSE(called);
    EXPECT_EQ(bytes(source), "old");
}

TEST(ExistingFileReplacement, RefusesMultiHardLinkAndDoesNotCallWriter)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const source = dir.file("source.svg");
    auto const hard = dir.file("hard.svg");
    write(source, "old");
    ASSERT_TRUE(CreateHardLinkW(wide(hard).c_str(), wide(source).c_str(), nullptr) != FALSE);

    bool called = false;
    auto const result = Inkscape::IO::replace_existing_local_file(source, [&](FILE *) {
        called = true;
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported) << result.error;
    EXPECT_FALSE(called);
    EXPECT_EQ(bytes(source), "old");
    PathInfo const info = query_path(source);
    EXPECT_EQ(info.links, 2u);
}

TEST(ExistingFileReplacement, RefusesUncDeviceAndRelativePathsWithoutCallingWriter)
{
    bool called = false;
    auto writer = [&](FILE *) { called = true; };

    // A UNC spelling is admitted for SMB 2+ shares; an unreachable one fails
    // before publication at the parent open, still without calling the writer.
    // Both a file in the share root (parent \\server\share\) and one in a
    // subfolder pass the parser and fail only at the parent open.
    for (char const *unreachable : {"\\\\invalid.invalid\\share\\file.svg",
                                    "\\\\invalid.invalid\\share\\dir\\file.svg"}) {
        auto const r = Inkscape::IO::replace_existing_local_file(unreachable, writer);
        EXPECT_EQ(r.outcome, Inkscape::IO::ExistingFileOutcome::FailedBeforePublication)
            << unreachable << ": " << r.error;
    }
    Inkscape::IO::ExistingFileResult result;

    // Malformed UNC spellings are refused before any network access.
    for (char const *bad : {"\\\\server\\", "\\\\server\\share", "\\\\server\\\\share\\f.svg",
                            "\\\\server\\share\\..\\f.svg", "\\\\server\\share\\dir.\\f.svg",
                            "\\\\server\\share\\CON\\f.svg", "\\\\server\\share\\f.svg "}) {
        result = Inkscape::IO::replace_existing_local_file(bad, writer);
        EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported) << bad << ": " << result.error;
    }

    result = Inkscape::IO::replace_existing_local_file("\\\\?\\C:\\file.svg", writer);
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported) << result.error;

    result = Inkscape::IO::replace_existing_local_file("\\\\.\\C:\\file.svg", writer);
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported) << result.error;

    result = Inkscape::IO::replace_existing_local_file("relative.svg", writer);
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported) << result.error;

    EXPECT_FALSE(called);
}

// Opt-in real SMB fixtures (Windows): VACARDS_NAS_SCRATCH names a writable
// scratch folder on a share (UNC or mapped drive). Each test owns only its
// unique child folder and keeps it on failure for inspection.
std::string windows_nas_child(char const *label)
{
    char const *parent = g_getenv("VACARDS_NAS_SCRATCH");
    if (!parent || !*parent) return {};
    std::string base(parent);
    while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
    gchar *uuid = g_uuid_string_random();
    std::string dir = base + "\\" + label + "-" + uuid;
    g_free(uuid);
    return CreateDirectoryW(wide(dir).c_str(), nullptr) ? dir : std::string();
}

TEST(ExistingFileReplacement, WindowsSmbScratchReplacesExistingFile)
{
    if (!g_getenv("VACARDS_NAS_SCRATCH")) GTEST_SKIP() << "set VACARDS_NAS_SCRATCH to a scratch folder on a share";
    auto const dir = windows_nas_child("vacards-existing-test");
    ASSERT_FALSE(dir.empty());
    auto const target = dir + "\\existing.svg";
    write(target, "complete original SVG");
    auto const result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        ASSERT_EQ(std::fwrite("complete replacement SVG", 1, 24, stream), 24u);
    });
    bool const correct = result.outcome == Inkscape::IO::ExistingFileOutcome::Published
        && bytes(target) == "complete replacement SVG";
    EXPECT_TRUE(correct) << result.error << " scratch=" << dir;
    if (correct) {
        EXPECT_TRUE(DeleteFileW(wide(target).c_str()));
        EXPECT_TRUE(RemoveDirectoryW(wide(dir).c_str())) << "leftover files in " << dir;
    }
}

TEST(ExistingFileReplacement, WindowsSmbScratchPublishesNewFileWithoutReplacing)
{
    using namespace Inkscape::IO::DocumentTransaction;
    if (!g_getenv("VACARDS_NAS_SCRATCH")) GTEST_SKIP() << "set VACARDS_NAS_SCRATCH to a scratch folder on a share";
    auto const dir = windows_nas_child("vacards-new-file-test");
    ASSERT_FALSE(dir.empty());
    LogicalTarget target;
    target.final_name = "new.svg";
    target.parent_dir = dir;
    target.final_path = dir + "\\new.svg";
    auto save = [&](char const *text) {
        std::unique_ptr<SystemCalls> calls = make_platform_system_calls();
        FailureKind failure = FailureKind::None;
        std::string error;
        auto tx = NewDocumentFile::create(target, *calls, failure, error);
        EXPECT_TRUE(tx) << error;
        if (!tx) return PublicationStatus::NotAttempted;
        EXPECT_TRUE(tx->write([text](FILE *stream) { std::fputs(text, stream); }, failure, error)) << error;
        EXPECT_TRUE(tx->seal(failure, error)) << error;
        return tx->publish().status;
    };
    auto const first = save("complete new SVG");
    auto const second = save("second save");
    bool const correct = first == PublicationStatus::Published && second == PublicationStatus::Conflict
        && bytes(target.final_path) == "complete new SVG";
    EXPECT_TRUE(correct) << "first=" << to_string(first) << " second=" << to_string(second)
                         << " scratch=" << dir;
    if (correct) {
        EXPECT_TRUE(DeleteFileW(wide(target.final_path).c_str()));
        EXPECT_TRUE(RemoveDirectoryW(wide(dir).c_str())) << "leftover files in " << dir;
    }
}

TEST(ExistingFileReplacement, PublishesUnicodeDestinationPath)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("保存-é-日本.svg");
    write(target, "old unicode");
    PathInfo const before = query_path(target);
    ASSERT_TRUE(before.ok);

    auto const result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
        ASSERT_EQ(std::fwrite("new unicode", 1, 11, stream), 11u);
    });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Published) << result.error;
    EXPECT_EQ(bytes(target), "new unicode");
    PathInfo const after = query_path(target);
    ASSERT_TRUE(after.ok);
    EXPECT_FALSE(same_identity(before, after));
    EXPECT_TRUE(entries_with_prefix(dir.path, "vacards-").empty());
}

TEST(ExistingFileReplacement, Injected1175NoMutationIsFailedBeforePublication)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("1175.svg");
    write(target, "baseline 1175");
    PathInfo const before = query_path(target);
    ASSERT_TRUE(before.ok);

    Inkscape::IO::ExistingFileResult result;
    {
        ScopedReplaceFault const fault(1175, 1);
        result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
            ASSERT_EQ(std::fwrite("candidate 1175", 1, 14, stream), 14u);
        });
    }
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::FailedBeforePublication) << result.error;
    EXPECT_EQ(bytes(target), "baseline 1175");
    EXPECT_TRUE(same_identity(before, query_path(target)));
    EXPECT_TRUE(entries_with_prefix(dir.path, "vacards-").empty());
}

TEST(ExistingFileReplacement, Injected1176NoMutationIsFailedBeforePublication)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("1176.svg");
    write(target, "baseline 1176");
    PathInfo const before = query_path(target);
    ASSERT_TRUE(before.ok);

    Inkscape::IO::ExistingFileResult result;
    {
        ScopedReplaceFault const fault(1176, 1);
        result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
            ASSERT_EQ(std::fwrite("candidate 1176", 1, 14, stream), 14u);
        });
    }
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::FailedBeforePublication) << result.error;
    EXPECT_EQ(bytes(target), "baseline 1176");
    EXPECT_TRUE(same_identity(before, query_path(target)));
    EXPECT_TRUE(entries_with_prefix(dir.path, "vacards-").empty());
}

TEST(ExistingFileReplacement, Injected1177MixedStateIsUncertainAndRetainsBoth)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("1177.svg");
    write(target, "baseline 1177");
    PathInfo const before = query_path(target);
    ASSERT_TRUE(before.ok);

    Inkscape::IO::ExistingFileResult result;
    {
        ScopedReplaceFault const fault(1177, 2);
        result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
            ASSERT_EQ(std::fwrite("candidate 1177", 1, 14, stream), 14u);
        });
    }
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Uncertain) << result.error;
    ASSERT_FALSE(result.recovery_path.empty()) << result.error;
    EXPECT_NE(result.error.find("vacards-save-"), std::string::npos)
        << "the retained new stage must be reported to the caller";

    // The verified backup must hold the original bytes and identity.
    EXPECT_EQ(bytes(result.recovery_path), "baseline 1177");
    PathInfo const recovery = query_path(result.recovery_path);
    ASSERT_TRUE(recovery.ok);
    EXPECT_TRUE(same_identity(before, recovery));

    // The destination name was moved away and must not be silently restored.
    EXPECT_FALSE(query_path(target).ok);
    // The stage is retained (it may be the only complete new copy).
    EXPECT_FALSE(entries_with_prefix(dir.path, "vacards-save-").empty());
    EXPECT_FALSE(entries_with_prefix(dir.path, "vacards-recovery-").empty());
}

TEST(ExistingFileReplacement, InjectedOtherErrorNoMutationIsFailedBeforePublication)
{
    TempDir dir;
    ASSERT_NE(dir.path, nullptr);
    auto const target = dir.file("denied.svg");
    write(target, "baseline denied");
    PathInfo const before = query_path(target);
    ASSERT_TRUE(before.ok);

    Inkscape::IO::ExistingFileResult result;
    {
        ScopedReplaceFault const fault(5 /* ERROR_ACCESS_DENIED */, 1);
        result = Inkscape::IO::replace_existing_local_file(target, [](FILE *stream) {
            ASSERT_EQ(std::fwrite("candidate denied", 1, 16, stream), 16u);
        });
    }
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::FailedBeforePublication) << result.error;
    EXPECT_EQ(bytes(target), "baseline denied");
    EXPECT_TRUE(same_identity(before, query_path(target)));
    EXPECT_TRUE(entries_with_prefix(dir.path, "vacards-").empty());
}

} // namespace

#else

TEST(ExistingFileReplacement, UnsupportedPlatformDoesNotCallWriter)
{
    bool called = false;
    auto result = Inkscape::IO::replace_existing_local_file("unused", [&](FILE *) { called = true; });
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Unsupported);
    EXPECT_FALSE(called);
}

#endif

TEST(ExistingFileReplacement, ByteInjectionIgnoredWithoutOptIn)
{
    gchar *dir = g_dir_make_tmp("vacards-byte-hook-disabled-XXXXXX", nullptr);
    ASSERT_NE(dir, nullptr);
    auto const target = std::string(dir) + G_DIR_SEPARATOR_S + "existing.svg";
    ASSERT_TRUE(g_file_set_contents(target.c_str(), "old", -1, nullptr));
    std::string const payload = "new";
    auto const result = Inkscape::IO::replace_existing_local_file(
        target, std::as_bytes(std::span(payload.data(), payload.size())), true);
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::Published) << result.error;
    EXPECT_EQ(bytes(target), payload);
    g_remove(target.c_str());
    g_rmdir(dir);
    g_free(dir);
}

TEST(ExistingFileReplacement, ByteAdapterFailureKeepsOriginalOnAllPlatforms)
{
    Inkscape::IO::enable_file_io_test_hooks();
    gchar *dir = g_dir_make_tmp("vacards-byte-adapter-XXXXXX", nullptr);
    ASSERT_NE(dir, nullptr);
    auto const target = std::string(dir) + G_DIR_SEPARATOR_S + "existing.svg";
    ASSERT_TRUE(g_file_set_contents(target.c_str(), "complete old file", -1, nullptr));
    std::string const payload = "new file";
    auto const result = Inkscape::IO::replace_existing_local_file(
        target, std::as_bytes(std::span(payload.data(), payload.size())), true);
    EXPECT_EQ(result.outcome, Inkscape::IO::ExistingFileOutcome::FailedBeforePublication)
        << result.error;
    gchar *contents = nullptr;
    gsize length = 0;
    ASSERT_TRUE(g_file_get_contents(target.c_str(), &contents, &length, nullptr));
    EXPECT_EQ(std::string(contents, length), "complete old file");
    g_free(contents);
    std::vector<std::byte> owned{std::byte{'n'}, std::byte{'e'}, std::byte{'w'}};
    auto const published = Inkscape::IO::replace_existing_local_file(target, std::move(owned));
    EXPECT_EQ(published.outcome, Inkscape::IO::ExistingFileOutcome::Published) << published.error;
    ASSERT_TRUE(g_file_get_contents(target.c_str(), &contents, &length, nullptr));
    EXPECT_EQ(std::string(contents, length), "new");
    g_free(contents);
    g_remove(target.c_str());
    g_rmdir(dir);
    g_free(dir);
}

TEST(ExistingFileReplacement, ByteWriterMatchesCallbackOnAllPlatforms)
{
    gchar *dir = g_dir_make_tmp("vacards-byte-parity-XXXXXX", nullptr);
    ASSERT_NE(dir, nullptr);
    auto const callback_path = std::string(dir) + G_DIR_SEPARATOR_S + "callback.svg";
    auto const bytes_path = std::string(dir) + G_DIR_SEPARATOR_S + "bytes.svg";
    ASSERT_TRUE(g_file_set_contents(callback_path.c_str(), "old", -1, nullptr));
    ASSERT_TRUE(g_file_set_contents(bytes_path.c_str(), "old", -1, nullptr));
    std::string const payload("new\0SVG", 7);
    auto const callback = Inkscape::IO::replace_existing_local_file(callback_path, [&](FILE *file) {
        if (std::fwrite(payload.data(), 1, payload.size(), file) != payload.size()) {
            throw std::runtime_error("callback write failed");
        }
    });
    auto const byte_result = Inkscape::IO::replace_existing_local_file(
        bytes_path, std::as_bytes(std::span(payload.data(), payload.size())));
    EXPECT_EQ(callback.outcome, Inkscape::IO::ExistingFileOutcome::Published) << callback.error;
    EXPECT_EQ(byte_result.outcome, callback.outcome) << byte_result.error;
    gchar *contents = nullptr;
    gsize length = 0;
    ASSERT_TRUE(g_file_get_contents(bytes_path.c_str(), &contents, &length, nullptr));
    EXPECT_EQ(std::string(contents, length), payload);
    g_free(contents);
    g_remove(callback_path.c_str());
    g_remove(bytes_path.c_str());
    g_rmdir(dir);
    g_free(dir);
}

TEST(ExistingFileReplacement, M2ExpectedVersionRefusesBeforeWriterAndAtBoundary)
{
    using namespace Inkscape::IO;
    auto dir=g_dir_make_tmp("vacards-expected-version-XXXXXX",nullptr); ASSERT_NE(dir,nullptr);
    auto path=std::string(dir)+G_DIR_SEPARATOR_S+"prior.svg";
    ASSERT_TRUE(g_file_set_contents(path.c_str(),"AAAA",4,nullptr));
    auto before=inspect_existing_file_version(path); ASSERT_TRUE(before.version)<<before.error;
    ASSERT_TRUE(g_file_set_contents(path.c_str(),"BBBB",4,nullptr));
    bool called=false;
    auto refused=replace_existing_local_file(path,[&](FILE *){called=true;},*before.version);
    EXPECT_EQ(refused.outcome,ExistingFileOutcome::Conflict);EXPECT_FALSE(called);EXPECT_EQ(bytes(path),"BBBB");
    auto current=inspect_existing_file_version(path);ASSERT_TRUE(current.version);
    auto raced=replace_existing_local_file(path,[&](FILE *f){
        ASSERT_EQ(std::fwrite("NEW",1,3,f),3u);
        ASSERT_TRUE(g_file_set_contents(path.c_str(),"CCCC",4,nullptr));
    },*current.version);
    EXPECT_EQ(raced.outcome,ExistingFileOutcome::Conflict)<<raced.error;EXPECT_EQ(bytes(path),"CCCC");
    auto latest=inspect_existing_file_version(path);ASSERT_TRUE(latest.version);
    auto failed=replace_existing_local_file(path,[](FILE *){throw std::runtime_error("real writer failure");},*latest.version);
    EXPECT_EQ(failed.outcome,ExistingFileOutcome::FailedBeforePublication);EXPECT_EQ(bytes(path),"CCCC");
    auto published=replace_existing_local_file(path,[](FILE *f){std::fwrite("DONE",1,4,f);},*latest.version);
    EXPECT_EQ(published.outcome,ExistingFileOutcome::Published)<<published.error;EXPECT_EQ(bytes(path),"DONE");
    auto boundary=inspect_existing_file_version(path);ASSERT_TRUE(boundary.version);
    ExistingFileOptions options;
    options.stage_observer=[&](unsigned stage) {
        if (stage==4) {
            FILE *external=g_fopen(path.c_str(),"wb");
            if (external) { std::fwrite("RACE",1,4,external);std::fclose(external); }
        }
        return false;
    };
    auto late=replace_existing_local_file(path,[](FILE *f){std::fwrite("LATE",1,4,f);},*boundary.version,options);
    EXPECT_EQ(late.outcome,ExistingFileOutcome::Conflict)<<late.error;EXPECT_EQ(bytes(path),"RACE");
    g_remove(path.c_str());g_rmdir(dir);g_free(dir);
}

TEST(ExistingFileReplacement, R1CancellationAtReplacementBoundaryPreservesOldBytes)
{
    using namespace Inkscape::IO;
    auto dir=g_dir_make_tmp("vacards-r1-cancel-XXXXXX",nullptr);ASSERT_NE(dir,nullptr);
    auto path=std::string(dir)+G_DIR_SEPARATOR_S+"old.svg";
    ASSERT_TRUE(g_file_set_contents(path.c_str(),"OLD",3,nullptr));
    auto v=inspect_existing_file_version(path);ASSERT_TRUE(v.version);
    bool staged=false;ExistingFileOptions options;options.cancelled=[&]{return staged;};
    auto result=replace_existing_local_file(path,[&](FILE *f){std::fwrite("NEW",1,3,f);staged=true;},*v.version,options);
    EXPECT_EQ(result.outcome,ExistingFileOutcome::Cancelled)<<result.error;EXPECT_EQ(bytes(path),"OLD");
    g_remove(path.c_str());g_rmdir(dir);g_free(dir);
}

#ifdef _WIN32
TEST(ExistingFileReplacement, R1WindowsVerifiedPublicationCleanupWarning)
{
    using namespace Inkscape::IO;
    auto dir=g_dir_make_tmp("vacards-r1-cleanup-XXXXXX",nullptr);ASSERT_NE(dir,nullptr);
    for(unsigned fail_stage:{5u,6u}) {
    auto path=std::string(dir)+G_DIR_SEPARATOR_S+"old.svg";
    ASSERT_TRUE(g_file_set_contents(path.c_str(),"OLD",3,nullptr));
    ExistingFileOptions options;options.stage_observer=[&](unsigned stage){return stage==fail_stage;};
    auto result=replace_existing_local_file(path,[](FILE *f){std::fwrite("NEW",1,3,f);},options);
    EXPECT_EQ(result.outcome,ExistingFileOutcome::Published)<<result.error;EXPECT_EQ(bytes(path),"NEW");
    EXPECT_FALSE(result.error.empty());if(fail_stage==5) EXPECT_FALSE(result.recovery_path.empty());
    if(!result.recovery_path.empty()) g_remove(result.recovery_path.c_str());g_remove(path.c_str());
    }
    g_rmdir(dir);g_free(dir);
}
#endif
