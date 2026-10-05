// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_EXISTING_FILE_REPLACEMENT_H
#define INKSCAPE_IO_EXISTING_FILE_REPLACEMENT_H

#include <cstdint>
#include <optional>
#include <cstdio>
#include <functional>
#include <span>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>
#include "io/stream/bufferstream.h"

namespace Inkscape::IO {


enum class ExistingFileOutcome {
    Published,
    FailedBeforePublication,
    Conflict,
    Unsupported,
    Uncertain,
    Cancelled,
    Unavailable
};

struct ExistingFileResult {
    ExistingFileOutcome outcome = ExistingFileOutcome::FailedBeforePublication;
    std::string error;
    // On Uncertain, a complete prior version may be retained at this path.
    // On Published with a cleanup error, the old recovery copy remains here.
    std::string recovery_path;
};

struct ExpectedFileVersion { std::string identity, sha256; std::uint64_t bytes = 0; };

struct ExistingFileOptions {
    std::optional<ExpectedFileVersion> expected_version;
    std::function<bool()> cancelled;
    bool timing = false;
    int rename_failure_errno = 0;
    int recovery_delete_failure_errno = 0;
    unsigned long replace_fault_code = 0;
    int replace_fault_kind = 0;
    // Apple buffered-save boundary observer. True injects a failure.
    // 1=create, 2=write, 3=seal, 4=pre-rename, 5=post-rename cleanup.
    // Windows additionally uses 6=post-publication timestamp cleanup.
    std::function<bool(unsigned)> stage_observer;
};

struct FileVersionResult { std::optional<ExpectedFileVersion> version;
    ExistingFileOutcome outcome; std::string error; };
FileVersionResult inspect_existing_file_version(std::string const &absolute_utf8_path);
ExistingFileResult replace_existing_local_file(std::string const &absolute_utf8_path,
    std::function<void(FILE *)> const &writer, ExpectedFileVersion const &expected,
    ExistingFileOptions const &options = {});

// Capture one-shot legacy hooks on the initiating thread, before dispatch.
ExistingFileOptions capture_existing_file_options(bool timing);

// The writer borrows a sibling staging FILE and must finish its own buffered
// serialization, but must not close it. The logical filename is supplied to the
// writer separately; the random stage name must never determine gzip or hrefs.
ExistingFileResult replace_existing_local_file(
    std::string const &absolute_utf8_path,
    std::function<void(FILE *)> const &writer);
ExistingFileResult replace_existing_local_file(
    std::string const &absolute_utf8_path,
    std::function<void(FILE *)> const &writer, ExistingFileOptions const &options);

// Caller retains ownership until this synchronous transaction returns.
inline ExistingFileResult replace_existing_local_file(
    std::string const &absolute_utf8_path, std::span<std::byte const> bytes,
    bool inject_byte_write_failure_for_testing = false)
{
    return replace_existing_local_file(absolute_utf8_path, [bytes, inject_byte_write_failure_for_testing](FILE *stage) {
        if (inject_byte_write_failure_for_testing && file_io_test_hooks_enabled()) {
            throw std::runtime_error("injected staging byte write failure");
        }
        if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), stage) != bytes.size()) {
            throw std::runtime_error("write staging bytes failed");
        }
    });
}

inline ExistingFileResult replace_existing_local_file(
    std::string const &absolute_utf8_path, std::span<std::byte const> bytes,
    bool inject_byte_write_failure_for_testing, ExistingFileOptions const &options)
{
    return replace_existing_local_file(absolute_utf8_path,
        [bytes, inject_byte_write_failure_for_testing](FILE *stage) {
            if (inject_byte_write_failure_for_testing)
                throw std::runtime_error("injected staging byte write failure");
            if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), stage) != bytes.size())
                throw std::runtime_error("write staging bytes failed");
        }, options);
}

// Owns the payload for the duration of a synchronous publication call. A worker
// may move its payload into this overload without retaining a borrowed span.
inline ExistingFileResult replace_existing_local_file(
    std::string const &absolute_utf8_path, std::vector<std::byte> bytes)
{
    return replace_existing_local_file(absolute_utf8_path,
        [bytes = std::move(bytes)](FILE *stage) {
            if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), stage) != bytes.size()) {
                throw std::runtime_error("write staging bytes failed");
            }
        });
}

} // namespace Inkscape::IO

#endif
