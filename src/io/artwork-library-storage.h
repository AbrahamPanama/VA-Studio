// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_STORAGE_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_STORAGE_H

#include "artwork-library-encoding.h"
#include <optional>
#include <stdexcept>

namespace Inkscape::IO::ArtworkLibrary {

struct StorageLimits {
    ArchiveLimits archive;
    ManifestLimits manifest;
};

enum class StorageFailure { None, Cancelled, Conflict, Locked, Unsupported, Io, Integrity, Invalid };

class StorageError final : public std::runtime_error {
public:
    StorageError(StorageFailure failure, std::string message);
    StorageFailure failure() const noexcept { return _failure; }
private:
    StorageFailure _failure;
};

// Optimistic version of the exact file read, not an authenticity credential.
// Copy it unchanged into save_library; no caller-owned GFile/SPDocument/UI state.
struct FileVersion {
    std::string path, file_id, filesystem_id, etag, sha256, library_id;
    std::uint64_t size = 0, modified_seconds = 0, revision = 0;
    std::uint32_t modified_microseconds = 0;
    bool operator==(FileVersion const &) const = default;
};

struct LoadedLibrary {
    Package package;
    FileVersion version;
};

// Native absolute UTF-8 paths. Windows accepts drive/UNC paths, with or without
// the extended-length prefix; persisted/displayed paths omit that prefix. The
// OS limit is measured in UTF-16 units, not UTF-8 bytes. No registry change is
// required by this service. Device namespaces and relative paths are rejected.
std::string canonical_library_path(std::string const &path);

// Worker-thread variant: expand existing Windows short-name aliases (including
// a new destination's parent) without resolving links or granting write access.
// Can query a mounted share; keep it out of UI/session input validation.
std::string resolved_library_path(std::string const &path);

// Shared native I/O for the library's SVG/LightBurn importer and SVG exporter.
// Prefix reads are for format sniffing only; full reads enforce the byte cap.
// Export is CREATE ONLY; a failed write can leave a partial file at this path.
Bytes read_library_input(std::string path, std::size_t limit, Cancelled cancelled = {}, bool prefix = false);
void write_library_export(std::string path, Bytes const &bytes, Cancelled cancelled = {});

// Reads a native absolute path, including read-only mounted shares. Rejects
// symlink components/non-regular files, bounds reads, detects read-time changes,
// and verifies every authoritative artwork. No extraction/rendering/SVG parsing.
// Synchronous: use a service worker, NOT the UI thread (NAS calls can block).
LoadedLibrary load_library(std::string path, StorageLimits limits = {}, Cancelled cancelled = {});

enum class RecoveryFileKind { Staged, PreviousVersion, Trash };
struct RecoveryFile {
    std::string path, label, diagnostic;
    RecoveryFileKind kind = RecoveryFileKind::Staged;
    // Present only after full package integrity verification. Not SVG admission
    // or permission to replace any original; recheck this exact version on use.
    std::optional<FileVersion> version;
};
struct RecoveryScanLimits {
    StorageLimits storage;
    std::size_t directory_entries = 4096, candidate_files = 128;
    std::size_t package_bytes = 512u * 1024 * 1024;
    std::size_t expanded_bytes = 1024u * 1024 * 1024;
};
struct RecoveryScan {
    std::string directory;
    std::vector<RecoveryFile> files;
    std::size_t entries_examined = 0, package_bytes_examined = 0, expanded_bytes_examined = 0;
    bool limited = false; // Results are incomplete; never silently report none/all.
};
// Read-only, nonrecursive scan of ONE explicitly chosen native directory.
// Recognizes our stage/recovery/trash filename families, then validates content.
// Does not read locks, follow symlinks, render SVG, delete or auto-restore files.
// Bounded work, not a wall-time guarantee on a disconnected mounted filesystem.
// Run on a worker. Directory errors/cancellation throw; individual damaged
// candidates remain visible with a diagnostic and no version/restore authority.
RecoveryScan scan_library_recovery(std::string directory, RecoveryScanLimits = {}, Cancelled = {});

// Retention for prior-version copies (".valib-recovery-<library>-r<revision>-
// <sha256>-<uuid>.valib") of ONE library in the folder of `destination`.
// Keeps the `keep` newest by revision and deletes older copies only after each
// one verifies as exactly that library/revision/hash and is unchanged. Call only
// after a conclusively Published save; save_library never calls it. Never
// touches the library file, trash or staged copies, locks, other libraries'
// copies, symlinks or hard links. Failures to verify/delete are warnings.
// `current` is the copy the calling save just wrote: it is always kept and fills
// one slot, and copies with a higher revision (a same-ID sibling file, such as a
// Save As copy in the same folder) are never deleted by this call. Recovery
// names do not identify the source file, so the file saved at the higher
// revision can still prune a same-ID sibling's older copies. At most
// `max_deletions` copies are removed (and 3x that verified) per call; any rest
// waits for later saves.
struct RecoveryPruneResult {
    std::vector<std::string> removed, warnings;
};
RecoveryPruneResult prune_library_recovery(std::string destination, std::string const &library_id,
                                           std::size_t keep = 5, std::string const &current = {},
                                           std::size_t max_deletions = 10);

// Writer-lock inspection for an explicit, user-confirmed stale-lock removal.
// Never used automatically. Run both functions on a worker thread.
struct LibraryLock {
    std::string path;    // Native lock file path for this library file.
    std::string holder;  // "host=…, pid=…, utc=…" when recorded; empty for older locks.
    FileVersion version; // Exact identity and SHA-256 of the lock as inspected.
};
// Empty result: there is no lock for `destination`.
std::optional<LibraryLock> inspect_library_lock(std::string destination);
// Deletes the lock only if it still has exactly the inspected identity and
// bytes; otherwise throws StorageError(Conflict) and leaves it in place.
void remove_stale_library_lock(LibraryLock const &inspected);

// Explicit deterministic fault-injection/diagnostic seams, all real IO remains
// enabled. Throwing aborts before publication, or reports a post-publication
// diagnostic afterward. Callbacks must not recursively save the same collection.
enum class StoragePhase {
    Locked, BeforeStageCreate, StageCreated, StageChunkWritten, StageVerified,
    BeforeRecoveryCreate, RecoveryCreated, RecoveryChunkWritten, RecoveryVerified, BeforeAdmission, Moved, Published
};
struct StorageProgress {
    StoragePhase phase;
    std::string const &destination, &staged, &recovery;
    std::size_t bytes_written = 0;
};
struct StorageOptions {
    StorageLimits limits;
    Cancelled cancelled;
    std::function<void(StorageProgress const &)> checkpoint;
};

enum class Publication { NotPublished, Unchanged, Published, Uncertain };
struct StorageResult {
    Publication publication = Publication::NotPublished;
    StorageFailure failure = StorageFailure::None;
    std::optional<FileVersion> version;
    std::string recovery_path, staged_path, retained_lock_path, message;
    bool cancellation_too_late = false;
    // False on Windows and on filesystems without a directory-flush operation.
    // A true value records successful OS flush calls, not hardware certification.
    bool directory_flush_completed = false;
};

// No expected version means CREATE ONLY. Saving an existing collection requires
// an exact version from load_library, same UUID and increasing revision. A same
// revision/identical manifest is a no-op (retains existing disposable previews).
//
// Exclusive <destination>.lock (a bounded hashed sibling for maximum-length
// filenames), no stale-lock takeover. Native mounted network/removable volumes
// are supported when they provide file identities, exclusive creation, file
// flush and same-directory rename. Trusted, stable parent directories and
// cooperating writers are REQUIRED: native rename provides no
// portable atomic compare-and-swap against noncooperating writers/namespace swaps.
// Symlink/known hardlink targets are refused. Server disconnects/permissions or
// unsupported operations produce an explicit error, never a copy/delete fallback.
//
// Temp and uniquely versioned recovery packages are flushed, read back and fully
// integrity-verified before same-directory native move (no copy fallback). Last
// cancellation admission precedes final conflict checks/move; that bounded section
// is intentionally non-cancellable. Later cancellation never implies rollback.
// Failures retain staged/recovery files, named in the result. Never auto-delete
// recovery, auto-restore it, steal a lock, or consume catalog/session Undo state.
// Published/Uncertain results must NOT be blindly retried with the old version.
//
// INTEGRITY ONLY: callers still need independently validated SVG/resource/geometry
// policy before exposing artwork for rendering/insertion. This API certifies none.
StorageResult save_library(std::string path, CatalogSnapshot snapshot,
                           std::optional<FileVersion> expected = {}, StorageOptions options = {});

} // namespace Inkscape::IO::ArtworkLibrary
#endif
