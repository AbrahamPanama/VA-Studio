// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_ARTWORK_LIBRARY_WORKSPACE_H
#define INKSCAPE_ARTWORK_LIBRARY_WORKSPACE_H
#include "io/artwork-library-storage.h"
#include "io/artwork-library-svg-preflight.h"
#include <sigc++/sigc++.h>
#include <atomic>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <optional>
#include <thread>

namespace Inkscape::UI::Dialog {
namespace Art = IO::ArtworkLibrary;

struct LibraryCollection {
    LibraryCollection(Art::Catalog, std::string path = {}, std::optional<Art::FileVersion> = {},
                      std::optional<std::uint64_t> saved = {});
    Art::Catalog catalog;
    std::string identity, label;
    std::uint64_t revision = 0;
    std::string path;
    std::optional<Art::FileVersion> version;
    std::optional<std::uint64_t> saved_revision;
    bool uncertain = false;
    std::vector<std::string> recovery_paths;
    // Removed IDs already written to a published trash copy in this session.
    // A later save writes a new trash copy only for IDs outside this set.
    std::set<std::string> trash_covered;
    std::map<std::string, std::string> removed_names; // UI labels; authority remains Catalog session trash
    bool dirty() const;
    void sync_header(); // once per edit, not on every native action/thumbnail notification
};
struct LibraryImportEntry {
    std::string source, name, message;
    std::size_t ordinal = 0; // one-based for lbart; never exposes private geometry
    bool converted = false;
};
struct LibraryPendingImport {
    Art::Catalog candidate;
    std::string target_id; // empty means new collection
    std::vector<LibraryImportEntry> entries;
};
struct LibraryPageEntry {
    Art::Asset metadata;
    std::optional<Art::ValidatedSvg> svg;
    std::string diagnostic;
};
// Reopen saved files, not unpublished imports or temporary recovery data.
struct LibrarySavedSession {
    std::vector<std::string> paths;
    std::string active_path;
};

// Application-owned, main-context service. Retain across panel destruction.
// One worker at a time; all workers own immutable snapshots/copies, never GTK
// or SPDocument. Completion (including late/uncertain saves) is always observed.
// Host MUST consult can_close()/dirty collections before application shutdown.
class ArtworkLibraryWorkspace final : public std::enable_shared_from_this<ArtworkLibraryWorkspace> {
public:
    static std::shared_ptr<ArtworkLibraryWorkspace> create();
    ~ArtworkLibraryWorkspace();
    std::vector<LibraryCollection> const &collections() const { return _collections; }
    LibraryCollection const *active() const;
    std::string const &active_id() const { return _active; }
    bool busy() const { return _busy; }
    bool closing() const { return _closing; }
    bool can_close() const;
    std::string const &message() const { return _message; }
    std::optional<LibraryPendingImport> const &pending_import() const { return _pending; }
    std::optional<Art::RecoveryScan> const &recovery_scan() const { return _recovery_scan; }
    std::optional<Art::LibraryLock> const &inspected_lock() const { return _inspected_lock; }
    std::string const &inspected_lock_collection() const { return _inspected_lock_collection; }
    std::vector<Art::Asset> const &rows() const { return _rows; }
    std::uint64_t rows_generation() const { return _rows_generation; }
    std::vector<LibraryPageEntry> const &page() const { return _page; }
    std::uint64_t page_generation() const { return _page_generation; }
    sigc::signal<void()> changed;
    sigc::signal<void()> session_changed;
    LibrarySavedSession const &saved_session() const { return _saved_session; }
    void restore_session(LibrarySavedSession);

    void cancel(); // Request only: never declares a save rolled back.
    void new_collection(std::string name);
    void select_collection(std::string id);
    void unload(bool discard_confirmed = false); // files never deleted
    void open(std::string path);
    void import_files(std::vector<std::string> paths, bool into_new = false);
    void accept_import(bool accept); // explicit all-success/partial results decision
    void add(Art::NewArtwork metadata, Art::Bytes bytes);
    void rename(std::string id, std::string name);
    void tags(std::string id, std::vector<std::string> tags);
    void remove(std::string id);
    void restore(std::string id);
    void save(std::string destination = {}); // Save As is create-only, never overwrite by inference
    void scan_recovery(std::string directory); // explicit, read-only worker discovery
    void recover(std::string recovery_path, std::optional<Art::FileVersion> expected = {});
    // Explicit stale-lock workflow (worker I/O). Nothing is removed without remove_lock().
    void inspect_lock();                // Active collection's saved file.
    void remove_lock(Art::LibraryLock); // Only if still exactly as inspected.
    void dismiss_lock(std::string message = "Lock kept; nothing was removed"); // Removes nothing.
    // Recovery opens a separate candidate; never auto-replaces source. A version
    // from discovery is checked again, so confirmation cannot accept changed bytes.
    void export_svg(std::string id, std::string destination);
    void search(std::string query);
    void visible(std::vector<std::string> ids);
    void invalidate_page();
    static Art::ValidatedSvg admit(Art::CatalogSnapshot snapshot, std::string id, Art::Cancelled = {});
private:
    friend class ArtworkLibraryClose; // unforgeable close reservation; never a public mutation bypass
    ArtworkLibraryWorkspace();
    void cancel_impl();
    void save_impl(std::string const &id, std::string destination, bool closing = false);
    void import_files_impl(std::vector<std::string>, bool into_new, bool closing = false);
    using Complete = std::function<void(ArtworkLibraryWorkspace &)>;
    void start(std::function<Complete(Art::Cancelled)> work, bool retain_publication_result = false, bool closing = false);
    void check_idle(bool closing = false) const;
    LibraryCollection &current();
    LibraryCollection *find(std::string const &id);
    void edit(std::function<void(Art::Catalog &, Art::Cancelled)>);
    void refresh_rows();
    void notify();
    void remember_saved(LibraryCollection const &, std::string const &previous_path = {});
    void remember_selection();
    std::thread::id _owner;
    std::vector<LibraryCollection> _collections;
    std::string _active, _query, _message;
    LibrarySavedSession _saved_session;
    std::optional<LibraryPendingImport> _pending;
    std::optional<Art::RecoveryScan> _recovery_scan;
    std::optional<Art::LibraryLock> _inspected_lock;
    std::string _inspected_lock_collection; // Collection active when the lock was inspected.
    std::vector<Art::Asset> _rows;
    std::vector<LibraryPageEntry> _page;
    std::uint64_t _page_generation = 0;
    std::uint64_t _rows_generation = 0;
    bool _busy = false;
    bool _closing = false;
    std::shared_ptr<std::atomic<bool>> _cancel;
};
}
#endif
