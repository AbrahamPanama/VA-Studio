// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Outcome-based tests for the F3a new-file staging/publication packet.
 *
 * The oracle is independent of the implementation: real temporary directories,
 * expected byte strings written/read with GLib, and existence checks on the
 * actual filesystem. Fault injection subclasses SystemCalls; there is no
 * production environment hook. Exact create/close/publish/remove counts are
 * asserted for every fault case so the state machine's "no second close" and
 * "no publication after failure" contracts are runtime evidence, not syntax.
 *
 * These tests compile and run only after the root registers the module and test
 * in CMake (see the packet handoff). Nothing here performs a build.
 */

#include <gtest/gtest.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

#include "io/document-file-transaction.h"

#ifndef _WIN32

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace Inkscape::IO::DocumentTransaction {

// POSIX-internal classifier defined in
// src/io/document-file-transaction-posix.cpp. Intentionally not declared in the
// public header; forward-declared here so the focused classifier tests exercise
// the exact production errno mapping.
PublicationStatus classify_link_errno(int error) noexcept;
// SMB fallback defined in the same file; `after_claim` is a test seam.
PublicationStatus publish_by_claim_and_rename(std::string const &staged_path,
                                              std::string const &final_path, std::string &error,
                                              void (*after_claim)(std::string const &final_path));

} // namespace Inkscape::IO::DocumentTransaction

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
                std::string const entry = std::string(path) + "/" + name;
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
    target.final_path = dir + "/out.svg";
    return target;
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

void write_file(std::string const &path, std::string const &bytes)
{
    ASSERT_TRUE(g_file_set_contents(path.c_str(), bytes.data(), static_cast<gssize>(bytes.size()), nullptr));
}

/// Independent staging-sibling check: any entry using the module's fixed prefix.
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

void expect_result_equal(PublicationResult const &a, PublicationResult const &b)
{
    EXPECT_EQ(a.status, b.status);
    EXPECT_EQ(a.failure, b.failure);
    EXPECT_EQ(a.error, b.error);
    EXPECT_EQ(a.file_sync_attempted, b.file_sync_attempted);
    EXPECT_EQ(a.file_sync_supported, b.file_sync_supported);
    EXPECT_EQ(a.file_sync_ok, b.file_sync_ok);
    EXPECT_EQ(a.parent_sync_attempted, b.parent_sync_attempted);
    EXPECT_EQ(a.parent_sync_supported, b.parent_sync_supported);
    EXPECT_EQ(a.parent_sync_ok, b.parent_sync_ok);
    EXPECT_EQ(a.recovery_retained, b.recovery_retained);
    EXPECT_EQ(a.recovery_path, b.recovery_path);
    EXPECT_EQ(a.staging_availability, b.staging_availability);
    EXPECT_EQ(a.staging_cleanup_ok, b.staging_cleanup_ok);
}

class InjectableCalls final : public SystemCalls
{
public:
    enum class Fault {
        None,
        FlushFail,
        SyncFail,
        SyncUnsupported,
        CloseFail,
        PublishUncertain,
        PublishCleanupFail,
        PublishUnsupported,
        PublishByClaimAndRename, ///< the production SMB fallback on a local directory
        ParentSyncFail,
    };

    Fault fault = Fault::None;
    int collision_budget = 0; ///< collisions injected before delegating
    /// Injected pre-create admission result; None delegates to the real adapter.
    FailureKind support_result = FailureKind::None;
    /// Injected read-only staging availability; Unverified delegates by default.
    StagingAvailability availability = StagingAvailability::Unverified;
    std::unique_ptr<SystemCalls> real = make_platform_system_calls();

    int create_calls = 0;
    int flush_calls = 0;
    int sync_calls = 0;
    int close_calls = 0;
    int publish_calls = 0;
    int remove_calls = 0;
    int parent_sync_calls = 0;
    int support_calls = 0;
    int availability_calls = 0;

    FailureKind pre_create_support(std::string const &parent_dir, std::string &error) override
    {
        ++support_calls;
        if (support_result != FailureKind::None) {
            (void)parent_dir;
            error = "injected unsupported capability";
            return support_result;
        }
        return real->pre_create_support(parent_dir, error);
    }

    StagingAvailability retained_stage_availability(std::string const &staged_path,
                                                    std::string &error) override
    {
        ++availability_calls;
        if (availability != StagingAvailability::Unverified) {
            return availability;
        }
        return real->retained_stage_availability(staged_path, error);
    }

    bool create_exclusive_file(std::string &path_template, FILE *&out,
                               bool &already_exists, std::string &error) override
    {
        ++create_calls;
        if (collision_budget > 0) {
            --collision_budget;
            already_exists = true;
            error = "injected staging collision";
            return false;
        }
        return real->create_exclusive_file(path_template, out, already_exists, error);
    }

    bool flush_file(FILE *stream, std::string &error) override
    {
        ++flush_calls;
        if (fault == Fault::FlushFail) {
            error = "injected flush failure";
            return false;
        }
        return real->flush_file(stream, error);
    }

    bool sync_file(FILE *stream, bool &unsupported, std::string &error) override
    {
        ++sync_calls;
        if (fault == Fault::SyncFail) {
            error = "injected sync failure";
            return false;
        }
        if (fault == Fault::SyncUnsupported) {
            unsupported = true;
            return true;
        }
        return real->sync_file(stream, unsupported, error);
    }

    bool close_file(FILE *stream, std::string &error) override
    {
        ++close_calls;
        bool const closed = real->close_file(stream, error);
        if (fault == Fault::CloseFail) {
            error = "injected close failure";
            return false;
        }
        return closed;
    }

    PublicationStatus publish_new_file(std::string const &staged_path,
                                       std::string const &final_path,
                                       std::string &error) override
    {
        ++publish_calls;
        if (fault == Fault::PublishUnsupported) {
            // Capability rejection: the namespace entry was definitely not
            // created and the staged name must remain untouched for cleanup.
            error = "injected unsupported publication";
            return PublicationStatus::Unsupported;
        }
        if (fault == Fault::PublishByClaimAndRename) {
            return publish_by_claim_and_rename(staged_path, final_path, error, nullptr);
        }
        if (fault == Fault::PublishUncertain) {
            // Ambiguous publication that may have applied: create the
            // destination link but keep the complete staging copy.
            if (::link(staged_path.c_str(), final_path.c_str()) != 0) {
                error = "injected ambiguous link failure";
                return PublicationStatus::Failed;
            }
            error = "injected ambiguous publication";
            return PublicationStatus::Uncertain;
        }
        if (fault == Fault::PublishCleanupFail) {
            // Confirmed namespace entry; the shared layer's single cleanup will
            // fail because remove_file is faulted below.
            if (::link(staged_path.c_str(), final_path.c_str()) != 0) {
                error = "injected cleanup link failure";
                return PublicationStatus::Failed;
            }
            error.clear();
            return PublicationStatus::Published;
        }
        return real->publish_new_file(staged_path, final_path, error);
    }

    bool remove_file(std::string const &path) noexcept override
    {
        ++remove_calls;
        if (fault == Fault::PublishCleanupFail) {
            return false;
        }
        return real->remove_file(path);
    }

    bool sync_parent_directory(std::string const &parent, bool &unsupported,
                               std::string &error) override
    {
        ++parent_sync_calls;
        if (fault == Fault::ParentSyncFail) {
            error = "injected parent sync failure";
            return false;
        }
        return real->sync_parent_directory(parent, unsupported, error);
    }
};

} // namespace

TEST(NewDocumentFileTest, HappyNewFile)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("hello-new", stream); }, failure, error)) << error;
    ASSERT_TRUE(tx->seal(failure, error)) << error;
    EXPECT_TRUE(tx->file_sync_attempted());
    EXPECT_EQ(calls.flush_calls, 1);
    EXPECT_EQ(calls.sync_calls, 1);

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Published);
    EXPECT_EQ(result.failure, FailureKind::None);
    EXPECT_TRUE(result.file_sync_attempted);
    EXPECT_TRUE(result.parent_sync_attempted);
    EXPECT_EQ(read_file(dir.str() + "/out.svg"), "hello-new");
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.close_calls, 1);
    EXPECT_EQ(calls.remove_calls, 1);
    EXPECT_EQ(calls.parent_sync_calls, 1);

    std::string const staged = tx->staged_path();
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    // The logical final name is never rewritten with the random staging suffix.
    EXPECT_EQ(tx->target().final_name, "out.svg");
    EXPECT_EQ(tx->target().final_path, dir.str() + "/out.svg");

    // Repeated publish is the complete original result and no new syscall.
    PublicationResult const again = tx->publish();
    expect_result_equal(result, again);
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.parent_sync_calls, 1);
}

TEST(NewDocumentFileTest, ByteWriterMatchesCallbackPublication)
{
    TempDir const callback_dir;
    TempDir const bytes_dir;
    ASSERT_TRUE(callback_dir.ok());
    ASSERT_TRUE(bytes_dir.ok());
    std::string const payload("ab\0cd", 5);
    InjectableCalls callback_calls, bytes_calls;
    FailureKind callback_failure = FailureKind::None, bytes_failure = FailureKind::None;
    std::string callback_error, bytes_error;
    auto callback = NewDocumentFile::create(target_in(callback_dir.str()), callback_calls,
                                            callback_failure, callback_error);
    auto owned = NewDocumentFile::create(target_in(bytes_dir.str()), bytes_calls,
                                         bytes_failure, bytes_error);
    ASSERT_TRUE(callback);
    ASSERT_TRUE(owned);
    ASSERT_TRUE(callback->write([&](FILE *file) {
        ASSERT_EQ(std::fwrite(payload.data(), 1, payload.size(), file), payload.size());
    }, callback_failure, callback_error));
    ASSERT_TRUE(owned->write_bytes(std::as_bytes(std::span(payload.data(), payload.size())),
                                   bytes_failure, bytes_error));
    ASSERT_TRUE(callback->seal(callback_failure, callback_error));
    ASSERT_TRUE(owned->seal(bytes_failure, bytes_error));
    auto const callback_result = callback->publish();
    auto const bytes_result = owned->publish();
    EXPECT_EQ(callback_result.status, PublicationStatus::Published);
    EXPECT_EQ(bytes_result.status, callback_result.status);
    EXPECT_EQ(bytes_result.failure, callback_result.failure);
    EXPECT_EQ(bytes_result.file_sync_attempted, callback_result.file_sync_attempted);
    EXPECT_EQ(bytes_result.parent_sync_attempted, callback_result.parent_sync_attempted);
    EXPECT_EQ(bytes_calls.flush_calls, callback_calls.flush_calls);
    EXPECT_EQ(bytes_calls.sync_calls, callback_calls.sync_calls);
    EXPECT_EQ(bytes_calls.close_calls, callback_calls.close_calls);
    EXPECT_EQ(read_file(bytes_dir.str() + "/out.svg"), payload);
    EXPECT_EQ(read_file(bytes_dir.str() + "/out.svg"), read_file(callback_dir.str() + "/out.svg"));
}

TEST(NewDocumentFileTest, ByteWriterStageWriteFailureKeepsOriginal)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    auto const target = target_in(dir.str());
    write_file(target.final_path, "original");
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;
    auto tx = NewDocumentFile::create(target, calls, failure, error);
    ASSERT_TRUE(tx) << error;
    // Test-only injected stage failure: replace the owned stream's descriptor
    // with a read-only descriptor, while leaving the staged path under the
    // transaction's cleanup ownership. Unbuffered fwrite must fail at write().
    ASSERT_EQ(std::setvbuf(tx->staged_stream(), nullptr, _IONBF, 0), 0);
    int const read_only = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(read_only, 0);
    ASSERT_EQ(::dup2(read_only, ::fileno(tx->staged_stream())), ::fileno(tx->staged_stream()));
    ::close(read_only);
    std::string const payload = "replacement";
    EXPECT_FALSE(tx->write_bytes(std::as_bytes(std::span(payload.data(), payload.size())),
                                 failure, error));
    EXPECT_EQ(failure, FailureKind::StagingWriteFailed);
    auto const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Failed);
    EXPECT_EQ(result.failure, FailureKind::StagingWriteFailed);
    EXPECT_EQ(read_file(target.final_path), "original");
    tx.reset(); // The transaction owner performs the one-time stage cleanup.
    EXPECT_FALSE(has_staging_sibling(dir.str()));
}

TEST(NewDocumentFileTest, PriorDestinationCollisionPreservesBytes)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const dest = dir.str() + "/out.svg";
    write_file(dest, "ORIGINAL");

    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;
    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("REPLACEMENT", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Conflict);
    EXPECT_EQ(result.failure, FailureKind::DestinationExists);
    EXPECT_EQ(read_file(dest), "ORIGINAL");
    EXPECT_FALSE(g_file_test(tx->staged_path().c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.close_calls, 1);
    EXPECT_EQ(calls.remove_calls, 1);

    PublicationResult const again = tx->publish();
    expect_result_equal(result, again);
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.remove_calls, 1);
}

TEST(NewDocumentFileTest, DestinationAppearsAfterStage)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const dest = dir.str() + "/out.svg";

    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;
    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("NEW", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    // Raced destination appears after the stage was sealed.
    write_file(dest, "RACED");

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Conflict);
    EXPECT_EQ(result.failure, FailureKind::DestinationExists);
    EXPECT_EQ(read_file(dest), "RACED");
    EXPECT_FALSE(g_file_test(tx->staged_path().c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.remove_calls, 1);
}

TEST(NewDocumentFileTest, WriterCallbackThrows)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    std::string const staged = tx->staged_path();

    bool const ok = tx->write(
        [](FILE *) { throw std::runtime_error("injected writer failure"); }, failure, error);
    EXPECT_FALSE(ok);
    EXPECT_EQ(failure, FailureKind::StagingWriteFailed);
    // Removal is not write()'s job: the failed stage is still present, but the
    // stream was closed exactly once and no publish was attempted.
    EXPECT_TRUE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(calls.close_calls, 1);
    EXPECT_EQ(calls.publish_calls, 0);
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));

    // Explicit terminal abort per contract, then independent checks.
    tx->abort();
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    tx.reset();
    EXPECT_EQ(calls.close_calls, 1); // no second close on destruction
    EXPECT_EQ(calls.remove_calls, 1); // exactly one cleanup
}

TEST(NewDocumentFileTest, EmptyWriterForbidsSealAndPublish)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    std::string const staged = tx->staged_path();

    EXPECT_FALSE(tx->write(NewDocumentFile::StageWriter{}, failure, error));
    EXPECT_EQ(failure, FailureKind::StagingWriteFailed);
    EXPECT_EQ(calls.close_calls, 1);

    // A failed write is sticky: seal must not flush/close again, publish must
    // not be attempted, and the first reason survives.
    EXPECT_FALSE(tx->seal(failure, error));
    EXPECT_EQ(failure, FailureKind::StagingWriteFailed);
    EXPECT_EQ(calls.flush_calls, 0);
    EXPECT_EQ(calls.sync_calls, 0);
    EXPECT_EQ(calls.close_calls, 1);

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Failed);
    EXPECT_EQ(result.failure, FailureKind::StagingWriteFailed);
    EXPECT_EQ(calls.publish_calls, 0);

    tx.reset();
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(calls.close_calls, 1);
}

TEST(NewDocumentFileTest, LateFlushFailureIsPrecommit)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::FlushFail;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("partial", stream); }, failure, error));
    EXPECT_FALSE(tx->seal(failure, error));
    EXPECT_EQ(failure, FailureKind::StagingFlushFailed);
    EXPECT_EQ(calls.flush_calls, 1);
    EXPECT_EQ(calls.sync_calls, 0);
    EXPECT_EQ(calls.close_calls, 1);

    // Sticky: a second seal/write/publish must not flush/close again.
    EXPECT_FALSE(tx->seal(failure, error));
    EXPECT_EQ(failure, FailureKind::StagingFlushFailed);
    EXPECT_FALSE(tx->write([](FILE *) {}, failure, error));
    EXPECT_EQ(failure, FailureKind::StagingFlushFailed);
    EXPECT_EQ(calls.flush_calls, 1);
    EXPECT_EQ(calls.close_calls, 1);
    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Failed);
    EXPECT_EQ(result.failure, FailureKind::StagingFlushFailed);
    EXPECT_EQ(result.file_sync_attempted, false);
    EXPECT_EQ(calls.publish_calls, 0);

    std::string const staged = tx->staged_path();
    tx.reset();
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(calls.close_calls, 1);
}

TEST(NewDocumentFileTest, CloseFailureIsPrecommit)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::CloseFail;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("partial", stream); }, failure, error));
    EXPECT_FALSE(tx->seal(failure, error));
    EXPECT_EQ(failure, FailureKind::StagingCloseFailed);
    EXPECT_EQ(calls.flush_calls, 1);
    EXPECT_EQ(calls.sync_calls, 1);
    EXPECT_EQ(calls.close_calls, 1);

    EXPECT_FALSE(tx->seal(failure, error));
    EXPECT_EQ(failure, FailureKind::StagingCloseFailed);
    EXPECT_EQ(calls.close_calls, 1); // no second close

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Failed);
    EXPECT_EQ(result.failure, FailureKind::StagingCloseFailed);
    EXPECT_EQ(calls.publish_calls, 0);

    std::string const staged = tx->staged_path();
    tx.reset();
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(calls.close_calls, 1);
}

TEST(NewDocumentFileTest, PrePublicationAbortAndDestructionRemoveStaging)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const dest = dir.str() + "/out.svg";
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    std::string staged;
    {
        auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
        ASSERT_TRUE(tx) << error;
        ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("cancel-me", stream); }, failure, error));
        staged = tx->staged_path();
        ASSERT_TRUE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
        tx->abort();
        EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    }
    EXPECT_FALSE(g_file_test(dest.c_str(), G_FILE_TEST_EXISTS));

    // Destruction without abort also removes the staging sibling.
    {
        auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
        ASSERT_TRUE(tx) << error;
        staged = tx->staged_path();
        ASSERT_TRUE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    }
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test(dest.c_str(), G_FILE_TEST_EXISTS));
}

TEST(NewDocumentFileTest, AbortAfterSealForbidsPublish)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const dest = dir.str() + "/out.svg";
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("abort-after-seal", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));
    std::string const staged = tx->staged_path();

    tx->abort();
    EXPECT_FALSE(tx->sealed());
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Failed);
    EXPECT_EQ(result.failure, FailureKind::Aborted);
    EXPECT_EQ(calls.publish_calls, 0);
    EXPECT_FALSE(g_file_test(dest.c_str(), G_FILE_TEST_EXISTS));
}

TEST(NewDocumentFileTest, StagingCollisionRetriesThenSucceeds)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.collision_budget = 2;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    EXPECT_EQ(calls.create_calls, 3); // 2 injected collisions + 1 success
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("retry-ok", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));
    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Published);
    EXPECT_EQ(read_file(dir.str() + "/out.svg"), "retry-ok");
    EXPECT_EQ(calls.publish_calls, 1);
}

TEST(NewDocumentFileTest, StagingCollisionExhaustionFailsCleanly)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.collision_budget = 100; // more than the bounded retry budget
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    EXPECT_FALSE(tx);
    EXPECT_EQ(failure, FailureKind::StagingCreateFailed);
    EXPECT_EQ(calls.create_calls, 8); // exact bounded budget
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(has_staging_sibling(dir.str()));
}

TEST(NewDocumentFileTest, UncertainPublicationRetainsCompleteCopy)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const dest = dir.str() + "/out.svg";
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::PublishUncertain;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("recover-me", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Uncertain);
    EXPECT_EQ(result.failure, FailureKind::PublicationUncertain);
    EXPECT_TRUE(result.recovery_retained);
    std::string const staged = tx->staged_path();
    EXPECT_EQ(result.recovery_path, staged);
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.remove_calls, 0); // recovery copy deliberately retained

    // Repeated publish returns the stable complete result and no new syscall.
    PublicationResult const again = tx->publish();
    expect_result_equal(result, again);
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.remove_calls, 0);

    // Ambiguous publication may have applied: destination may exist, and the
    // complete staging copy must still be readable.
    ASSERT_TRUE(g_file_test(dest.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(read_file(dest), "recover-me");
    ASSERT_TRUE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(read_file(staged), "recover-me");

    tx.reset();
    // The destructor never auto-removes an ambiguous publication's staging copy.
    EXPECT_TRUE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(read_file(staged), "recover-me");
}

TEST(NewDocumentFileTest, CleanupFailureAfterPublishIsNotDataLoss)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const dest = dir.str() + "/out.svg";
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::PublishCleanupFail;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("keep-me", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    // Namespace outcome stays Published; the failure is cleanup only.
    EXPECT_EQ(result.status, PublicationStatus::Published);
    EXPECT_FALSE(result.staging_cleanup_ok);
    EXPECT_EQ(read_file(dest), "keep-me");
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.remove_calls, 1);

    // Repeat is stable and does not retry cleanup.
    PublicationResult const again = tx->publish();
    expect_result_equal(result, again);
    EXPECT_EQ(calls.remove_calls, 1);

    std::string const staged = tx->staged_path();
    tx.reset();
    EXPECT_TRUE(g_file_test(dest.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(read_file(dest), "keep-me");
    // A failed cleanup is reported, not retried on destruction.
    EXPECT_EQ(calls.remove_calls, 1);
    EXPECT_TRUE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
}

TEST(NewDocumentFileTest, RecreatedStagingPathSurvivesPublishDestruction)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("pub", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));
    ASSERT_EQ(tx->publish().status, PublicationStatus::Published);
    EXPECT_EQ(calls.remove_calls, 1);

    std::string const old_stage = tx->staged_path();
    write_file(old_stage, "REAPPEARED");
    tx.reset(); // destructor must not delete the recreated file

    EXPECT_TRUE(g_file_test(old_stage.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(read_file(old_stage), "REAPPEARED");
    EXPECT_EQ(read_file(dir.str() + "/out.svg"), "pub");
    EXPECT_EQ(calls.remove_calls, 1);
}

TEST(NewDocumentFileTest, RecreatedStagingPathSurvivesAbortDestruction)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    std::string const old_stage = tx->staged_path();
    tx->abort();
    EXPECT_EQ(calls.remove_calls, 1);

    write_file(old_stage, "REAPPEARED");
    tx.reset(); // destructor must not delete the recreated file

    EXPECT_TRUE(g_file_test(old_stage.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(read_file(old_stage), "REAPPEARED");
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(calls.remove_calls, 1);
}

TEST(NewDocumentFileTest, InvalidOperationsAfterPublishDoNotAlterResult)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("first", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));
    PublicationResult const result = tx->publish();
    ASSERT_EQ(result.status, PublicationStatus::Published);

    EXPECT_FALSE(tx->write([](FILE *) {}, failure, error));
    EXPECT_EQ(failure, FailureKind::InvalidOperation);
    EXPECT_FALSE(tx->seal(failure, error));
    EXPECT_EQ(failure, FailureKind::InvalidOperation);
    EXPECT_EQ(tx->publication_status(), PublicationStatus::Published);
    EXPECT_EQ(tx->last_failure(), FailureKind::None);

    PublicationResult const again = tx->publish();
    expect_result_equal(result, again);
    EXPECT_EQ(calls.publish_calls, 1);
}

TEST(NewDocumentFileTest, StickyFirstFailureSurvivesRepeatedOperations)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::SyncFail;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("sticky", stream); }, failure, error));
    EXPECT_FALSE(tx->seal(failure, error));
    EXPECT_EQ(failure, FailureKind::StagingSyncFailed);
    EXPECT_TRUE(tx->file_sync_attempted());
    EXPECT_FALSE(tx->file_sync_supported());
    EXPECT_FALSE(tx->file_sync_ok());
    EXPECT_EQ(calls.sync_calls, 1);
    EXPECT_EQ(calls.close_calls, 1);

    // Repeated operations keep the first reason and issue no second flush/close.
    EXPECT_FALSE(tx->seal(failure, error));
    EXPECT_EQ(failure, FailureKind::StagingSyncFailed);
    EXPECT_FALSE(tx->write([](FILE *) {}, failure, error));
    EXPECT_EQ(failure, FailureKind::StagingSyncFailed);
    EXPECT_EQ(calls.flush_calls, 1);
    EXPECT_EQ(calls.sync_calls, 1);
    EXPECT_EQ(calls.close_calls, 1);

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Failed);
    EXPECT_EQ(result.failure, FailureKind::StagingSyncFailed);
    EXPECT_TRUE(result.file_sync_attempted);
    EXPECT_FALSE(result.file_sync_ok);
    EXPECT_EQ(calls.publish_calls, 0);

    std::string const staged = tx->staged_path();
    tx.reset();
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(calls.close_calls, 1);
}

TEST(NewDocumentFileTest, SyncUnsupportedIsCapabilityNotDurability)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::SyncUnsupported;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("no-fsync", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error)) << error;

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Published);
    EXPECT_TRUE(result.file_sync_attempted);
    EXPECT_FALSE(result.file_sync_supported); // capability, not fabricated
    EXPECT_FALSE(result.file_sync_ok);
    EXPECT_EQ(read_file(dir.str() + "/out.svg"), "no-fsync");
    EXPECT_EQ(calls.publish_calls, 1);
}

TEST(NewDocumentFileTest, FileSyncFailureBlocksPublication)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::SyncFail;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("no-publish", stream); }, failure, error));
    EXPECT_FALSE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Failed);
    EXPECT_EQ(result.failure, FailureKind::StagingSyncFailed);
    EXPECT_TRUE(result.file_sync_attempted);
    EXPECT_FALSE(result.file_sync_ok);
    // No publication was attempted on a real file-sync failure.
    EXPECT_EQ(calls.publish_calls, 0);
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
}

TEST(NewDocumentFileTest, ParentSyncFailureKeepsPublished)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::ParentSyncFail;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("durable?", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Published); // not downgraded
    EXPECT_TRUE(result.parent_sync_attempted);
    EXPECT_TRUE(result.parent_sync_supported);
    EXPECT_FALSE(result.parent_sync_ok);
    EXPECT_EQ(read_file(dir.str() + "/out.svg"), "durable?");
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.parent_sync_calls, 1);
}

TEST(NewDocumentFileTest, RejectsPipeAndRelativeTargets)
{
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    LogicalTarget pipe;
    pipe.final_name = "-";
    pipe.parent_dir = "/tmp";
    pipe.final_path = "-";
    auto pipe_tx = NewDocumentFile::create(pipe, calls, failure, error);
    EXPECT_FALSE(pipe_tx);
    EXPECT_EQ(failure, FailureKind::InvalidArgument);

    LogicalTarget relative;
    relative.final_name = "out.svg";
    relative.parent_dir = ".";
    relative.final_path = "out.svg";
    auto relative_tx = NewDocumentFile::create(relative, calls, failure, error);
    EXPECT_FALSE(relative_tx);
    EXPECT_EQ(failure, FailureKind::InvalidArgument);
    EXPECT_EQ(calls.create_calls, 0); // validation happens before any filesystem touch
}

TEST(NewDocumentFileTest, RejectsMismatchedParentName)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    LogicalTarget mismatch = target_in(dir.str());
    mismatch.final_path = dir.str() + "/other.svg"; // not parent + final_name
    auto tx = NewDocumentFile::create(mismatch, calls, failure, error);
    EXPECT_FALSE(tx);
    EXPECT_EQ(failure, FailureKind::InvalidArgument);
    EXPECT_EQ(calls.create_calls, 0);
    EXPECT_FALSE(has_staging_sibling(dir.str()));
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test((dir.str() + "/other.svg").c_str(), G_FILE_TEST_EXISTS));
}

TEST(NewDocumentFileTest, RejectsLexicallyEquivalentNonByteExactPath)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    LogicalTarget mismatch = target_in(dir.str());
    // Canonicalizes to dir/out.svg but is not the exact parent + name spelling.
    mismatch.final_path = dir.str() + "/./out.svg";
    auto tx = NewDocumentFile::create(mismatch, calls, failure, error);
    EXPECT_FALSE(tx);
    EXPECT_EQ(failure, FailureKind::InvalidArgument);
    EXPECT_EQ(calls.create_calls, 0);
    EXPECT_FALSE(has_staging_sibling(dir.str()));
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
}

TEST(NewDocumentFileTest, RejectsSymlinkedDotDotDivergentPath)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const base = dir.str() + "/base";
    std::string const other = dir.str() + "/other";
    ASSERT_EQ(g_mkdir(base.c_str(), 0777), 0);
    ASSERT_EQ(g_mkdir(other.c_str(), 0777), 0);
    std::string const link = base + "/link";
    ASSERT_EQ(symlink(other.c_str(), link.c_str()), 0);
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    LogicalTarget divergent;
    divergent.final_name = "out.svg";
    divergent.parent_dir = base;
    divergent.final_path = base + "/link/../out.svg";
    auto tx = NewDocumentFile::create(divergent, calls, failure, error);
    EXPECT_FALSE(tx);
    EXPECT_EQ(failure, FailureKind::InvalidArgument);
    EXPECT_EQ(calls.create_calls, 0);
    EXPECT_FALSE(has_staging_sibling(base));
    // Neither the stated location nor the symlink-resolved location may exist.
    EXPECT_FALSE(g_file_test((base + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test((other + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));

    g_remove(link.c_str());
    g_rmdir(base.c_str());
    g_rmdir(other.c_str());
}

TEST(NewDocumentFileTest, AcceptsOrdinaryAndTrailingSlashParentSpelling)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());

    // Ordinary spelling.
    {
        InjectableCalls calls;
        FailureKind failure = FailureKind::None;
        std::string error;
        auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
        ASSERT_TRUE(tx) << error;
        ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("ordinary", stream); }, failure, error));
        ASSERT_TRUE(tx->seal(failure, error));
        EXPECT_EQ(tx->publish().status, PublicationStatus::Published);
        EXPECT_EQ(read_file(dir.str() + "/out.svg"), "ordinary");
    }

    // Parent spelling that already ends in '/': the exact join omits the extra slash.
    {
        InjectableCalls calls;
        FailureKind failure = FailureKind::None;
        std::string error;
        LogicalTarget trailing;
        trailing.final_name = "out2.svg";
        trailing.parent_dir = dir.str() + "/";
        trailing.final_path = dir.str() + "/out2.svg";
        auto tx = NewDocumentFile::create(trailing, calls, failure, error);
        ASSERT_TRUE(tx) << error;
        ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("trailing", stream); }, failure, error));
        ASSERT_TRUE(tx->seal(failure, error));
        EXPECT_EQ(tx->publish().status, PublicationStatus::Published);
        EXPECT_EQ(read_file(dir.str() + "/out2.svg"), "trailing");
    }
}

TEST(NewDocumentFileTest, AcceptsBackslashAsOrdinaryBasenameByte)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    LogicalTarget target;
    target.final_name = "out\\back.svg";
    target.parent_dir = dir.str();
    target.final_path = dir.str() + "/out\\back.svg";
    auto tx = NewDocumentFile::create(target, calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("backslash", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));
    EXPECT_EQ(tx->publish().status, PublicationStatus::Published);
    EXPECT_EQ(read_file(dir.str() + "/out\\back.svg"), "backslash");
}

TEST(NewDocumentFileTest, RejectsEmbeddedNul)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;

    LogicalTarget name_nul = target_in(dir.str());
    name_nul.final_name = std::string("out\0.svg", 8);
    LogicalTarget path_nul = target_in(dir.str());
    path_nul.final_path = dir.str() + std::string("/out\0.svg", 8);
    LogicalTarget parent_nul = target_in(dir.str());
    parent_nul.parent_dir = dir.str() + std::string("\0x", 2);

    LogicalTarget const cases[] = {name_nul, path_nul, parent_nul};
    for (LogicalTarget const &bad : cases) {
        FailureKind failure = FailureKind::None;
        std::string error;
        auto tx = NewDocumentFile::create(bad, calls, failure, error);
        EXPECT_FALSE(tx);
        EXPECT_EQ(failure, FailureKind::InvalidArgument);
    }
    EXPECT_EQ(calls.create_calls, 0);
    EXPECT_FALSE(has_staging_sibling(dir.str()));
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
}

TEST(NewDocumentFileTest, AcceptsBackslashInPosixBasename)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    FailureKind failure = FailureKind::None;
    std::string error;

    LogicalTarget target;
    target.final_name = std::string("back\\slash.svg");
    target.parent_dir = dir.str();
    target.final_path = dir.str() + "/" + target.final_name;

    auto tx = NewDocumentFile::create(target, calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("backslash", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));
    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Published);
    EXPECT_EQ(read_file(target.final_path), "backslash");
    EXPECT_EQ(calls.publish_calls, 1);
}

TEST(NewDocumentFileTest, PreCreateSupportRejectionCreatesNothing)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.support_result = FailureKind::Unsupported;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    EXPECT_FALSE(tx);
    EXPECT_EQ(failure, FailureKind::Unsupported);
    EXPECT_EQ(calls.support_calls, 1);
    EXPECT_EQ(calls.create_calls, 0); // admission precedes any payload
    EXPECT_FALSE(has_staging_sibling(dir.str()));
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
}

TEST(NewDocumentFileTest, ClassifyLinkErrnoUnsupportedIsCapability)
{
#ifdef ENOTSUP
    EXPECT_EQ(classify_link_errno(ENOTSUP), PublicationStatus::Unsupported);
#endif
#ifdef EOPNOTSUPP
    EXPECT_EQ(classify_link_errno(EOPNOTSUPP), PublicationStatus::Unsupported);
#endif
}

TEST(NewDocumentFileTest, ClassifyLinkErrnoExistingDestinationIsConflict)
{
    EXPECT_EQ(classify_link_errno(EEXIST), PublicationStatus::Conflict);
}

TEST(NewDocumentFileTest, ClassifyLinkErrnoIoOrInterruptIsUncertain)
{
    EXPECT_EQ(classify_link_errno(EIO), PublicationStatus::Uncertain);
    EXPECT_EQ(classify_link_errno(EINTR), PublicationStatus::Uncertain);
}

TEST(NewDocumentFileTest, ClassifyLinkErrnoKnownDefiniteIsFailed)
{
    EXPECT_EQ(classify_link_errno(EACCES), PublicationStatus::Failed);
}

TEST(NewDocumentFileTest, UnsupportedPublicationIsExplicitCapabilityFailure)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::PublishUnsupported;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    std::string const staged = tx->staged_path();
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("unsupported", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Unsupported);
    EXPECT_EQ(result.failure, FailureKind::Unsupported);
    EXPECT_FALSE(result.recovery_retained);
    EXPECT_TRUE(result.recovery_path.empty());
    EXPECT_EQ(result.staging_availability, StagingAvailability::Unverified);
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.remove_calls, 1); // background callers clean up
    EXPECT_EQ(calls.availability_calls, 0);
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    // Sticky repeat: complete recorded result, no new syscall.
    PublicationResult const again = tx->publish();
    expect_result_equal(result, again);
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.remove_calls, 1);
    EXPECT_EQ(calls.availability_calls, 0);
    tx.reset();
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
}

TEST(NewDocumentFileTest, InteractiveUnsupportedPublicationRetainsStage)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::PublishUnsupported;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure,
                                      error, /*retain_on_unsupported=*/true);
    ASSERT_TRUE(tx) << error;
    std::string const staged = tx->staged_path();
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("unsupported", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Unsupported);
    EXPECT_TRUE(result.recovery_retained);
    EXPECT_EQ(result.recovery_path, staged);
    EXPECT_EQ(result.staging_availability, StagingAvailability::Unverified);
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.remove_calls, 0);
    EXPECT_EQ(calls.availability_calls, 1);
    EXPECT_TRUE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test((dir.str() + "/out.svg").c_str(), G_FILE_TEST_EXISTS));
    expect_result_equal(result, tx->publish());
    EXPECT_EQ(calls.publish_calls, 1);
    EXPECT_EQ(calls.availability_calls, 1);
    tx.reset();
    EXPECT_TRUE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(g_remove(staged.c_str()), 0);
}

TEST(NewDocumentFileTest, UncertainPublicationWithAvailableIdentityRetains)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::PublishUncertain;
    calls.availability = StagingAvailability::Available;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("avail", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Uncertain);
    EXPECT_EQ(result.failure, FailureKind::PublicationUncertain);
    EXPECT_EQ(result.staging_availability, StagingAvailability::Available);
    EXPECT_TRUE(result.recovery_retained);
    EXPECT_EQ(result.recovery_path, tx->staged_path());
    EXPECT_EQ(calls.availability_calls, 1);
    EXPECT_EQ(calls.remove_calls, 0);

    PublicationResult const again = tx->publish();
    expect_result_equal(result, again);
    EXPECT_EQ(calls.availability_calls, 1); // latched once, no new syscall
    EXPECT_EQ(calls.remove_calls, 0);
}

TEST(NewDocumentFileTest, UncertainPublicationWithUnverifiedIdentityRetainsUnverified)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::PublishUncertain;
    // Default availability delegate reports Unverified on POSIX/mocks.
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("unverified", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Uncertain);
    EXPECT_EQ(result.staging_availability, StagingAvailability::Unverified);
    EXPECT_TRUE(result.recovery_retained); // deliberate retention, not a byte claim
    EXPECT_EQ(result.recovery_path, tx->staged_path());
    EXPECT_EQ(calls.availability_calls, 1);

    PublicationResult const again = tx->publish();
    expect_result_equal(result, again);
    EXPECT_EQ(calls.availability_calls, 1);
}

TEST(NewDocumentFileTest, UncertainPublicationWithMissingIdentityIsNotRetained)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::PublishUncertain;
    calls.availability = StagingAvailability::Missing;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("missing", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Uncertain); // uncertainty is never collapsed
    EXPECT_EQ(result.failure, FailureKind::PublicationUncertain);
    EXPECT_EQ(result.staging_availability, StagingAvailability::Missing);
    EXPECT_FALSE(result.recovery_retained);
    EXPECT_TRUE(result.recovery_path.empty());
    EXPECT_EQ(calls.availability_calls, 1);

    PublicationResult const again = tx->publish();
    expect_result_equal(result, again);
    EXPECT_EQ(calls.availability_calls, 1); // latched, no new syscall
}

TEST(NewDocumentFileTest, UncertainPublicationWithChangedIdentityIsNotRetained)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::PublishUncertain;
    calls.availability = StagingAvailability::Changed;
    FailureKind failure = FailureKind::None;
    std::string error;

    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("changed", stream); }, failure, error));
    ASSERT_TRUE(tx->seal(failure, error));

    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Uncertain);
    EXPECT_EQ(result.staging_availability, StagingAvailability::Changed);
    EXPECT_FALSE(result.recovery_retained);
    EXPECT_TRUE(result.recovery_path.empty());
    EXPECT_EQ(calls.availability_calls, 1);

    PublicationResult const again = tx->publish();
    expect_result_equal(result, again);
    EXPECT_EQ(calls.availability_calls, 1);
}

// Claim-and-rename fallback for SMB shares without link(2)/RENAME_EXCL. rename(2)
// has the same semantics locally, so these run on a temporary directory.

TEST(ClaimAndRenamePublication, PublishesNewFileAndConsumesStage)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const staged = dir.str() + "/vacards-new-stage";
    std::string const final_path = dir.str() + "/out.svg";
    write_file(staged, "complete new SVG");
    std::string error;
    EXPECT_EQ(publish_by_claim_and_rename(staged, final_path, error, nullptr), PublicationStatus::Published)
        << error;
    EXPECT_EQ(read_file(final_path), "complete new SVG");
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
}

TEST(ClaimAndRenamePublication, ExistingDestinationIsNeverReplaced)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const staged = dir.str() + "/vacards-new-stage";
    std::string const final_path = dir.str() + "/out.svg";
    write_file(staged, "candidate");
    write_file(final_path, "existing user file");
    std::string error;
    EXPECT_EQ(publish_by_claim_and_rename(staged, final_path, error, nullptr), PublicationStatus::Conflict);
    EXPECT_EQ(read_file(final_path), "existing user file");
    EXPECT_EQ(read_file(staged), "candidate");
}

TEST(ClaimAndRenamePublication, ClaimWrittenByAnotherWriterIsLeftAlone)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const staged = dir.str() + "/vacards-new-stage";
    std::string const final_path = dir.str() + "/out.svg";
    write_file(staged, "candidate");
    std::string error;
    auto const other_writer = [](std::string const &path) {
        int const fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(::write(fd, "other writer", 12), 12);
        ::close(fd);
    };
    EXPECT_EQ(publish_by_claim_and_rename(staged, final_path, error, other_writer),
              PublicationStatus::Conflict);
    EXPECT_EQ(read_file(final_path), "other writer");
    EXPECT_EQ(read_file(staged), "candidate");
}

TEST(ClaimAndRenamePublication, ReplacedClaimIsLeftAlone)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const staged = dir.str() + "/vacards-new-stage";
    std::string const final_path = dir.str() + "/out.svg";
    write_file(staged, "candidate");
    std::string error;
    // A different (empty) file now holds the name: identity, not size, detects it.
    auto const replace_claim = [](std::string const &path) {
        std::string const other = path + ".other";
        write_file(other, "");
        ASSERT_EQ(::rename(other.c_str(), path.c_str()), 0);
    };
    EXPECT_EQ(publish_by_claim_and_rename(staged, final_path, error, replace_claim),
              PublicationStatus::Conflict);
    EXPECT_TRUE(g_file_test(final_path.c_str(), G_FILE_TEST_IS_REGULAR));
    EXPECT_EQ(read_file(final_path), "");
    EXPECT_EQ(read_file(staged), "candidate");
}

TEST(ClaimAndRenamePublication, DefiniteRenameFailureReleasesOwnClaim)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::string const staged = dir.str() + "/vacards-new-stage";
    std::string const final_path = dir.str() + "/out.svg";
    write_file(staged, "candidate");
    std::string error;
    // The stage vanishes after the claim, so rename(2) fails with ENOENT.
    auto const remove_stage = [](std::string const &path) {
        std::string const stage = path.substr(0, path.rfind('/')) + "/vacards-new-stage";
        ASSERT_EQ(g_remove(stage.c_str()), 0);
    };
    EXPECT_EQ(publish_by_claim_and_rename(staged, final_path, error, remove_stage),
              PublicationStatus::Failed);
    EXPECT_NE(error.find("publish by rename failed"), std::string::npos) << error;
    EXPECT_FALSE(g_file_test(final_path.c_str(), G_FILE_TEST_EXISTS));
}

TEST(ClaimAndRenamePublication, TransactionPublishesAndFindsNoStageToRemove)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    InjectableCalls calls;
    calls.fault = InjectableCalls::Fault::PublishByClaimAndRename;
    FailureKind failure = FailureKind::None;
    std::string error;
    auto tx = NewDocumentFile::create(target_in(dir.str()), calls, failure, error);
    ASSERT_TRUE(tx) << error;
    std::string const staged = tx->staged_path();
    ASSERT_TRUE(tx->write([](FILE *stream) { std::fputs("smb-new", stream); }, failure, error)) << error;
    ASSERT_TRUE(tx->seal(failure, error)) << error;
    PublicationResult const result = tx->publish();
    EXPECT_EQ(result.status, PublicationStatus::Published);
    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_FALSE(result.recovery_retained);
    EXPECT_TRUE(result.staging_cleanup_ok);
    EXPECT_EQ(calls.remove_calls, 1);
    EXPECT_EQ(read_file(dir.str() + "/out.svg"), "smb-new");
    EXPECT_FALSE(g_file_test(staged.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(has_staging_sibling(dir.str()));
}

TEST(ClaimAndRenamePublication, PosixAvailabilityReportsOnlyProvenAbsence)
{
    TempDir const dir;
    ASSERT_TRUE(dir.ok());
    std::unique_ptr<SystemCalls> real = make_platform_system_calls();
    std::string const staged = dir.str() + "/vacards-new-stage";
    std::string error;
    EXPECT_EQ(real->retained_stage_availability(staged, error), StagingAvailability::Missing);
    write_file(staged, "kept");
    EXPECT_EQ(real->retained_stage_availability(staged, error), StagingAvailability::Unverified);
}

#endif // !_WIN32
