// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_CATALOG_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_CATALOG_H

#include "artwork-library-package.h"

namespace Inkscape::IO::ArtworkLibrary {

struct CatalogLimits {
    ManifestLimits manifest;
    std::size_t artwork_bytes = 32u * 1024 * 1024;
    std::size_t preview_bytes = 8u * 1024 * 1024;
    std::size_t overlay_bytes = 128u * 1024 * 1024;
    // Active entries AND session trash. Container overhead is bounded by this
    // count; metadata_bytes conservatively charges retained string capacities
    // (including search caches) and tag-vector backing storage, not just sizes.
    std::size_t retained_assets = 20000;
    std::size_t metadata_bytes = 32u * 1024 * 1024;
};

struct NewArtwork {
    std::string id; // Empty generates a UUID; supplied IDs must never have been used in this session.
    std::string name;
    std::vector<std::string> tags;
    double width_mm = 0;
    double height_mm = 0;
    std::string extra_json = "{}";
};

namespace detail { struct CatalogState; }

// Stable, immutable staging view for a later atomic writer. No file publication
// or durable recovery is performed here. Holding old snapshots/payload handles
// retains their memory; that additional retention is the caller's responsibility.
// Readers pin their entry state before invoking cancellation callbacks, even if
// a callback reassigns/destroys the snapshot wrapper. Borrowed input buffers (IDs,
// search strings) must remain valid and unchanged until the call returns.
class CatalogSnapshot final {
public:
    Manifest manifest(Cancelled cancelled = {}) const;
    std::vector<std::string> list(Cancelled cancelled = {}) const;
    // NFC + Unicode case folding; whitespace-separated terms are ANDed, with
    // each term matching a substring of the name OR any tag. Order is stable.
    std::vector<std::string> search(std::string_view query, Cancelled cancelled = {}) const;
    // Value metadata is safe to retain after the snapshot/catalog is destroyed.
    Asset asset(std::string const &id) const;
    // Metadata only; does not inflate package artwork. Removed IDs are absent.
    std::size_t artwork_size(std::string const &id) const;
    std::vector<std::string> removed_ids(Cancelled cancelled = {}) const;
    // Recovery view for writing removed artwork durably: entries in session
    // trash become the active entries and currently active entries are
    // excluded. Same library identity and revision; payloads are shared, not
    // copied. Throws when nothing is removed. Never changes the Catalog.
    CatalogSnapshot removed_items_snapshot(Cancelled cancelled = {}) const;
    std::shared_ptr<Bytes const> read_artwork(std::string const &id, Cancelled cancelled = {}) const;
    std::optional<Bytes> read_preview(std::string const &id, Cancelled cancelled = {}) const;
    std::shared_ptr<Package const> prior_package() const;
    std::size_t overlay_bytes() const;
    // Measured retained capacities, excluding the separately owned base Package
    // and count-bounded map/slot nodes. Includes active entries AND session trash.
    std::size_t retained_metadata_bytes() const;

private:
    friend class Catalog;
    explicit CatalogSnapshot(std::shared_ptr<detail::CatalogState const> state);
    std::shared_ptr<detail::CatalogState const> _state;
};

// Pure library state, not document Undo. Mutations require serialized access;
// immutable snapshots can be used independently. Callbacks must not concurrently
// mutate the catalog. Reentrant edits are detected before publishing a stale edit.
// The caller keeps the Catalog and borrowed input buffers alive throughout an edit.
class Catalog final {
public:
    // New libraries start at revision zero; require empty assets and revision 0.
    // Loaded libraries keep their existing revision. Each non-noop edit adds 1.
    static Catalog create(Manifest metadata, CatalogLimits limits = {}, Cancelled cancelled = {});
    static Catalog from_package(Package package, CatalogLimits limits = {}, Cancelled cancelled = {});
    CatalogSnapshot snapshot() const;

    // Bytes are staged as UNVALIDATED artwork, not certified SVG. A later
    // importer/writer must check XML, geometry, units and external resources.
    // Computes hash/path, never trusts caller-supplied integrity metadata.
    // Duplicate active names receive " (2)", " (3)", ... without overwriting.
    std::string add(NewArtwork metadata, Bytes const &unvalidated_artwork, Cancelled cancelled = {});
    bool rename(std::string const &id, std::string name, Cancelled cancelled = {});
    bool set_tags(std::string const &id, std::vector<std::string> tags, Cancelled cancelled = {});
    bool rename_library(std::string name, Cancelled cancelled = {});
    bool remove(std::string const &id, Cancelled cancelled = {});
    // Restores the original ordering slot and payload. If its old name is now
    // occupied, suffix it like add. Trash continues to count against budgets;
    // IDs are not recycled. Recovery lasts only as long as this session/snapshot.
    bool restore(std::string const &id, Cancelled cancelled = {});

private:
    explicit Catalog(std::shared_ptr<detail::CatalogState const> state);
    void publish(std::shared_ptr<detail::CatalogState> next,
                 std::shared_ptr<detail::CatalogState const> const &before, Cancelled const &cancelled);
    std::shared_ptr<detail::CatalogState const> _state;
};

} // namespace Inkscape::IO::ArtworkLibrary
#endif
