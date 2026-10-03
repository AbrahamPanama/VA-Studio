// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-host.h"
#include "preferences.h"
#include <glib.h>
#include <algorithm>
#include <stdexcept>
#include <limits>

namespace Inkscape::UI::Dialog {
namespace {
void require(bool b, char const *s) { if (!b) throw std::runtime_error(s); }
}
class ArtworkLibraryClose {
public:
    struct Replacement { std::vector<LibraryCollection> collections; std::string active; std::vector<Art::Asset> rows; };
    struct Held {
        std::string slot;
        std::shared_ptr<ArtworkLibraryWorkspace> w;
        std::optional<LibraryPendingImport> pending;
        std::set<std::string> discarded;
        std::optional<Replacement> replacement;
        bool locked = false;
    };
    std::vector<Held> held;
    bool prepared = false, committed = false;
    // Rollback is library-only: already-published saves are never rolled back.
    ~ArtworkLibraryClose() {
        for (auto &h : held) {
            if (!h.locked) continue;
            if (!committed && h.pending) h.w->_pending = std::move(h.pending);
            h.w->_closing = false;
        }
        for (auto &h : held) {
            try { if (h.locked) h.w->notify(); } catch (...) {} // no throwing out of native close unwind
        }
    }
    void lock() {
        for (auto &h : held) {
            require(h.w->_owner == std::this_thread::get_id(), "Workspace owning-thread violation");
            require(!h.w->_closing, "Library is already reserved for close");
        }
        for (auto &h : held) { h.w->_closing = true; h.locked = true; }
        for (auto &h : held) h.w->notify();
    }
    bool decide(LibraryCloseUI &ui) {
        for (auto &h : held) {
            auto cancel = [w = h.w] { w->cancel_impl(); };
            if (h.w->busy()) {
                auto a = ui.ask({LibraryCloseKind::Work, h.slot, {}, {}, h.w->message()});
                if (a.choice == LibraryCloseChoice::CancelWork) cancel();
                else if (a.choice != LibraryCloseChoice::Wait) return false;
                if (!ui.wait(h.w, cancel) || h.w->busy()) return false;
            }
            if (h.w->_pending) {
                auto a = ui.ask({LibraryCloseKind::Import, h.slot, {}, {},
                                "Discard the unpublished import candidate? Existing collections and files are unchanged."});
                if (a.choice != LibraryCloseChoice::Discard) return false;
                h.pending = std::move(h.w->_pending); h.w->_pending.reset();
            }
            // No public mutation can invalidate this list while reserved.
            for (auto const &c : h.w->_collections) {
                if (!c.dirty() && !c.uncertain) continue;
                std::string detail = h.w->message();
                for (auto const &path : c.recovery_paths) detail += "\n" + path;
                auto id = c.identity;
                auto a = ui.ask({c.uncertain ? LibraryCloseKind::Uncertain : LibraryCloseKind::Changes,
                                h.slot, c.label, c.path, std::move(detail)});
                if (a.choice == LibraryCloseChoice::Discard) { h.discarded.insert(id); continue; }
                if (a.choice != LibraryCloseChoice::Save || c.uncertain) return false;
                h.w->save_impl(id, std::move(a.destination), true);
                if (!ui.wait(h.w, cancel) || h.w->busy()) return false;
                auto saved = h.w->find(id);
                if (!saved || saved->dirty() || saved->uncertain) {
                    ui.error(h.w->message()); return false; // no retry loop / blind overwrite
                }
            }
        }
        // Allocate the post-discard UI/catalog state before native destruction.
        for (auto &h : held) if (!h.discarded.empty()) {
            Replacement r;
            for (auto const &c : h.w->_collections) if (!h.discarded.contains(c.identity)) r.collections.push_back(c);
            r.active = h.w->_active;
            auto active = std::find_if(r.collections.begin(), r.collections.end(), [&](auto const &c) { return c.identity == r.active; });
            if (active == r.collections.end()) { active = r.collections.begin(); r.active = active == r.collections.end() ? "" : active->identity; }
            if (active != r.collections.end()) { auto snapshot = active->catalog.snapshot(); for (auto const &id : snapshot.search(h.w->_query)) r.rows.push_back(snapshot.asset(id)); }
            h.replacement = std::move(r);
        }
        prepared = true;
        return true;
    }
    void commit() {
        if (committed) return;
        require(prepared, "Close decisions have not completed");
        // No file deletions. Do not emit GTK callbacks during the irreversible
        // native close tail, and remain reserved until the outer guard releases.
        for (auto &h : held) {
            if (h.replacement) {
                auto &w = *h.w; auto &r = *h.replacement;
                w._collections.swap(r.collections); w._active.swap(r.active); w._rows.swap(r.rows);
                ++w._rows_generation; w.invalidate_page();
            }
            h.pending.reset();
        }
        committed = true;
    }
    bool contains(std::vector<std::string> const &ids) const {
        return std::all_of(ids.begin(), ids.end(), [&](auto const &id) {
            return std::any_of(held.begin(), held.end(), [&](auto const &h) { return h.slot == id; });
        });
    }
};
LibraryCloseGuard::LibraryCloseGuard(std::shared_ptr<ArtworkLibraryClose> s, bool owner)
    : _session(std::move(s)), _owner(owner) {}
LibraryCloseGuard::~LibraryCloseGuard() = default;
void LibraryCloseGuard::commit() { if (_owner) _session->commit(); }
std::shared_ptr<ArtworkLibraryHost> ArtworkLibraryHost::create(std::string preferences_path) {
    auto host = std::shared_ptr<ArtworkLibraryHost>(new ArtworkLibraryHost);
    host->_preferences_path = std::move(preferences_path);
    if (host->_preferences_path.empty()) return host;
    auto prefs = Preferences::get();
    auto count = std::clamp(prefs->getInt(host->_preferences_path + "/count", 0), 0, 64);
    for (int i = 0; i < count; ++i) {
        auto base = host->_preferences_path + "/" + std::to_string(i);
        auto slot = prefs->getString(base + "/slot").raw();
        if (slot.empty() || slot.size() > 256 || host->_entries.contains(slot)) continue;
        LibrarySavedSession session;
        auto files = std::clamp(prefs->getInt(base + "/count", 0), 0, 16);
        for (int j = 0; j < files; ++j) {
            auto path = prefs->getString(base + "/file" + std::to_string(j)).raw();
            try { session.paths.push_back(Art::canonical_library_path(path)); }
            catch (Art::StorageError const &) {} // Reject invalid paths, not long UTF-8 spellings.
        }
        if (session.paths.empty()) continue;
        auto active = prefs->getString(base + "/active").raw();
        if (!active.empty()) {
            try { session.active_path = Art::canonical_library_path(active); }
            catch (Art::StorageError const &) {}
        }
        auto &entry = host->_entries[slot];
        entry.workspace = ArtworkLibraryWorkspace::create();
        entry.reopen = std::move(session);
        host->observe(entry);
    }
    return host;
}
void ArtworkLibraryHost::observe(Entry &entry) {
    entry.changed = entry.workspace->changed.connect([weak = weak_from_this()] {
        if (auto host = weak.lock()) host->changed.emit();
    });
    entry.session_changed = entry.workspace->session_changed.connect([weak = weak_from_this()] {
        if (auto host = weak.lock()) host->save_session();
    });
}
void ArtworkLibraryHost::save_session() {
    owner();
    if (_preferences_path.empty()) return;
    auto prefs = Preferences::get();
    int index = 0;
    for (auto const &[slot, entry] : _entries) {
        auto const &session = entry.reopen ? *entry.reopen : entry.workspace->saved_session();
        if (session.paths.empty()) continue;
        auto base = _preferences_path + "/" + std::to_string(index++);
        prefs->setString(base + "/slot", slot);
        prefs->setString(base + "/active", session.active_path);
        prefs->setInt(base + "/count", session.paths.size());
        for (std::size_t j = 0; j < session.paths.size(); ++j)
            prefs->setString(base + "/file" + std::to_string(j), session.paths[j]);
    }
    prefs->setInt(_preferences_path + "/count", index);
    // Remember a successful Open/Save (and explicit Unload) before returning to
    // the user, not only after the application's main loop eventually exits.
    // Keep using saved_session/reopen above: close/discard may empty the live
    // collections without forgetting their last saved files. Only semantic
    // session changes reach here; thumbnail/workspace notifications do not.
    prefs->save();
}
void ArtworkLibraryHost::owner() const { require(_owner == std::this_thread::get_id(), "Library host owning-thread violation"); }
ArtworkLibraryPanelReservation::ArtworkLibraryPanelReservation(std::shared_ptr<ArtworkLibraryHost> host, std::string slot, bool created)
    : _host(std::move(host)), _slot(std::move(slot)), _created(created) {}
ArtworkLibraryPanelReservation::~ArtworkLibraryPanelReservation() { _host->release_panel(*this); }
std::shared_ptr<ArtworkLibraryWorkspace> ArtworkLibraryPanelReservation::workspace() const {
    return _host->acquire(_slot);
}
std::unique_ptr<ArtworkLibraryPanelReservation> ArtworkLibraryHost::reserve_panel(std::string const &preferred) {
    owner(); require(_session.expired(), "Cannot open a library panel during close");
    auto free = [](Entry const &e) { return !e.opening && (!e.widget || !e.widget()); };
    auto found = _entries.find(preferred);
    if (found != _entries.end()) require(free(found->second), "The requested artwork library already has an open or opening panel");
    if (found == _entries.end())
        found = std::find_if(_entries.begin(), _entries.end(), [&](auto const &pair) { return free(pair.second); });
    bool const created = found == _entries.end();
    std::string slot;
    if (created) {
        auto raw = g_uuid_string_random(); slot = raw; g_free(raw);
    } else slot = found->first;
    auto reservation = std::unique_ptr<ArtworkLibraryPanelReservation>(
        new ArtworkLibraryPanelReservation(shared_from_this(), slot, created));
    if (created) (void)acquire(slot);
    _entries.at(slot).opening = reservation.get();
    changed.emit(); // Reentrant opens see the reservation; close refuses it.
    return reservation;
}
void ArtworkLibraryHost::release_panel(ArtworkLibraryPanelReservation const &reservation) noexcept {
    g_assert(_owner == std::this_thread::get_id());
    auto found = _entries.find(reservation.slot());
    if (found != _entries.end() && found->second.opening == &reservation) {
        found->second.opening = nullptr;
        auto const &entry = found->second;
        if (reservation._created && (!entry.widget || !entry.widget()) &&
            entry.workspace->can_close() && entry.workspace->collections().empty()) {
            _entries.erase(found);
        }
        try { changed.emit(); } catch (...) {} // constructor failure must still release the slot
    }
}
std::shared_ptr<ArtworkLibraryWorkspace> ArtworkLibraryHost::acquire(std::string const &slot) {
    owner(); require(!slot.empty() && slot.size() <= 256, "Invalid library host slot");
    auto found = _entries.find(slot);
    if (found != _entries.end()) {
        auto &entry = found->second;
        if (entry.reopen) {
            auto session = std::move(*entry.reopen); entry.reopen.reset();
            entry.workspace->restore_session(std::move(session));
        }
        return entry.workspace;
    }
    require(_session.expired(), "Cannot add a library workspace during close");
    require(_entries.size() < 64, "Retained library workspace limit (64) reached");
    auto w = ArtworkLibraryWorkspace::create();
    auto &e = _entries[slot]; e.workspace = w;
    observe(e);
    return w;
}
std::string ArtworkLibraryHost::retain(std::shared_ptr<ArtworkLibraryWorkspace> const &w) {
    owner(); require(bool(w), "Missing library workspace");
    for (auto const &[id, e] : _entries) if (e.workspace == w) return id;
    auto raw = g_uuid_string_random(); std::string id(raw); g_free(raw);
    (void)acquire(id); auto &e = _entries.at(id);
    e.changed.disconnect(); e.session_changed.disconnect(); e.workspace = w;
    observe(e); save_session();
    changed.emit(); return id;
}
std::uint64_t ArtworkLibraryHost::attach(std::string const &slot, std::function<Gtk::Widget *()> widget,
                               std::function<bool()> ready, ArtworkLibraryPanelReservation const *reservation) {
    owner(); auto &e = _entries.at(slot);
    require(_session.expired(), "Cannot attach a library panel during close");
    require(e.opening == reservation, "Library workspace is reserved by another panel construction");
    require(!e.widget || !e.widget(), "This retained workspace already has a live panel");
    require(e.attachment != std::numeric_limits<std::uint64_t>::max(), "Library attachment identity exhausted");
    e.widget = std::move(widget); e.ready = std::move(ready);
    return ++e.attachment;
}
void ArtworkLibraryHost::detach(std::string const &slot, std::uint64_t attachment) {
    owner(); auto &e = _entries.at(slot);
    if (attachment && attachment != e.attachment) return;
    e.widget = {}; e.ready = {};
}
std::vector<std::string> ArtworkLibraryHost::slots() const {
    owner(); std::vector<std::string> out;
    for (auto const &[id, e] : _entries) out.push_back(id);
    return out;
}
Gtk::Widget *ArtworkLibraryHost::live_panel(std::string const &slot) const {
    owner(); auto found = _entries.find(slot);
    return found != _entries.end() && found->second.widget ? found->second.widget() : nullptr;
}
bool ArtworkLibraryHost::needs_hold() const {
    owner();
    return !_session.expired() || std::any_of(_entries.begin(), _entries.end(),
        [](auto const &e) { return e.second.opening || !e.second.workspace->can_close(); });
}
std::unique_ptr<LibraryCloseGuard> ArtworkLibraryHost::prepare(std::vector<std::string> ids, LibraryCloseUI &ui) {
    owner();
    std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (auto current = _session.lock()) {
        if (!current->prepared || !current->contains(ids)) return {};
        return std::unique_ptr<LibraryCloseGuard>(new LibraryCloseGuard(current, false));
    }
    auto session = std::make_shared<ArtworkLibraryClose>();
    try {
        _session = session; // Barrier before readiness/UI callbacks can reenter.
        if (std::any_of(_entries.begin(), _entries.end(), [](auto const &e) { return bool(e.second.opening); })) {
            ui.error("Finish opening the artwork library before closing."); return {};
        }
        for (auto const &id : ids) {
            auto const &e = _entries.at(id);
            if (e.opening) { ui.error("Finish opening the artwork library before closing."); return {}; }
            if (e.widget && e.widget() && e.ready && !e.ready()) {
                ui.error("Finish or cancel the library file chooser / dialog before closing."); return {};
            }
            session->held.push_back({id, e.workspace, {}, {}});
        }
        session->lock();
        if (!session->decide(ui)) return {};
        return std::unique_ptr<LibraryCloseGuard>(new LibraryCloseGuard(std::move(session), true));
    } catch (std::exception const &e) { ui.error(e.what()); return {}; }
}
}
