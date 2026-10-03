// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_EXTENSION_INTERNAL_SVG_PUBLICATION_H
#define INKSCAPE_EXTENSION_INTERNAL_SVG_PUBLICATION_H

#include <cstddef>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "io/existing-file-replacement.h"
#include "xml/repr-save-output-stream.h"

namespace Inkscape::XML {
class Document;
} // namespace Inkscape::XML

namespace Inkscape::Extension::Internal {

/**
 * Keeps a standalone XML snapshot (not attached to any SPDocument) GC-anchored
 * until destroyed. XML nodes live in the Boehm GC heap and their anchor counts
 * are not thread-safe, so create and destroy it on the initiating thread only.
 * A worker may read the snapshot through DeferredSerialization while this owner
 * is alive, but never allocates GC memory, anchors or releases.
 */
class SerializationSnapshot
{
public:
    explicit SerializationSnapshot(Inkscape::XML::Document *document) noexcept;
    ~SerializationSnapshot();
    SerializationSnapshot(SerializationSnapshot const &) = delete;
    SerializationSnapshot &operator=(SerializationSnapshot const &) = delete;
    Inkscape::XML::Document const *document() const noexcept { return _document; }

private:
    Inkscape::XML::Document *_document;
    std::thread::id _owner;
};

// Serialization that publish() performs before destination admission (Save
// slice 2). Everything here is plain data captured on the initiating thread;
// `snapshot` is borrowed from the SerializationSnapshot that the initiating
// thread retains until completion.
struct DeferredSerialization {
    Inkscape::XML::Document const *snapshot = nullptr;
    SerializerOptions options;
    SerializerPlan plan;
    bool compress = false;
    std::string old_href_base;
    std::string new_href_base;
    std::size_t cap = 0;
    std::optional<std::size_t> reserve;
    bool inject_reserve_failure = false;
    bool inject_buffer_failure = false;
};

enum class PublicationOutcome { Published, Failed, Conflict, Unsupported, Uncertain, ReadOnly };

// All state needed after serialization is owned by this value. In particular,
// publication never consults the live document or process configuration.
struct PublicationJob {
    std::vector<std::byte> bytes;
    std::string absolute_path;
    std::string route = "owned_bytes";
    bool timing = false;
    bool test_hooks_enabled = false;
    bool inject_stage_write_failure = false;
    // Captured on the initiating thread. A test hook is inert by default.
    struct StageHook {
        unsigned stall_ms = 0;
        std::optional<PublicationOutcome> failure;
    };
    std::array<StageHook, 6> stage_hooks;
    std::optional<std::size_t> direct_size_hint;
    Inkscape::IO::ExistingFileOptions existing_file_options;
    // Set when serialization is deferred to publish(). The owner must be moved
    // out (take_publication_job) before the job leaves the initiating thread.
    std::optional<DeferredSerialization> deferred;
    std::shared_ptr<SerializationSnapshot> snapshot_owner;
};

struct PublicationResult {
    PublicationOutcome outcome = PublicationOutcome::Failed;
    std::string route;
    std::string error;
    std::string notice;
    std::string recovery_path;
    bool recovery_path_available = false;
    std::optional<std::size_t> serialized_size;
};

// Storage-only operation. The caller may run this on any thread after begin.
PublicationResult publish(PublicationJob job);

// Test observation of the number of native serialization attempts.
void reset_native_serialization_count_for_testing() noexcept;
unsigned native_serialization_count_for_testing() noexcept;

} // namespace Inkscape::Extension::Internal

#endif
