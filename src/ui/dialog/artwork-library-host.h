// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_ARTWORK_LIBRARY_HOST_H
#define INKSCAPE_ARTWORK_LIBRARY_HOST_H
#include "artwork-library-workspace.h"
#include <set>
namespace Gtk { class Widget; }
namespace Inkscape::UI::Dialog {
enum class LibraryCloseKind { Work, Import, Changes, Uncertain };
enum class LibraryCloseChoice { Cancel, Wait, CancelWork, Save, Discard };
struct LibraryClosePrompt {
    LibraryCloseKind kind;
    std::string slot, label, path, detail;
};
struct LibraryCloseAnswer {
    LibraryCloseChoice choice = LibraryCloseChoice::Cancel;
    std::string destination; // empty uses existing path; new files require Save As
};
class LibraryCloseUI {
public:
    virtual ~LibraryCloseUI() = default;
    virtual LibraryCloseAnswer ask(LibraryClosePrompt const &) = 0;
    // False aborts CLOSE, not necessarily the in-flight operation. No success
    // until the workspace has observed the actual final publication result.
    virtual bool wait(std::shared_ptr<ArtworkLibraryWorkspace> const &,
                      std::function<void()> const &cancel_work) = 0;
    virtual void error(std::string const &) = 0;
};
class ArtworkLibraryClose;
class ArtworkLibraryHost;
class ArtworkLibraryPanelReservation final {
public:
    ~ArtworkLibraryPanelReservation();
    ArtworkLibraryPanelReservation(ArtworkLibraryPanelReservation const &) = delete;
    std::string const &slot() const { return _slot; }
    std::shared_ptr<ArtworkLibraryWorkspace> workspace() const;
    bool belongs_to(ArtworkLibraryHost const &host) const { return _host.get() == &host; }
private:
    friend class ArtworkLibraryHost;
    ArtworkLibraryPanelReservation(std::shared_ptr<ArtworkLibraryHost>, std::string, bool created);
    std::shared_ptr<ArtworkLibraryHost> _host;
    std::string _slot;
    bool _created;
};
class LibraryCloseGuard final {
public:
    ~LibraryCloseGuard();
    LibraryCloseGuard(LibraryCloseGuard const &) = delete;
    void commit(); // only outer owner commits; nested native close guards are borrowers
private:
    friend class ArtworkLibraryHost;
    LibraryCloseGuard(std::shared_ptr<ArtworkLibraryClose>, bool owner);
    std::shared_ptr<ArtworkLibraryClose> _session;
    bool _owner;
};
// Owning-thread registry. A slot is a host identity, NEVER an active document,
// collection name, window address, or a reference to a disposable panel.
class ArtworkLibraryHost final : public std::enable_shared_from_this<ArtworkLibraryHost> {
public:
    // The application supplies its preferences namespace. Empty = transient
    // host, used by isolated workspaces/tests without changing user settings.
    // Remembered-file changes are flushed through the existing preferences API;
    // unsaved collection contents are never serialized into preferences.
    static std::shared_ptr<ArtworkLibraryHost> create(std::string preferences_path = {});
    std::shared_ptr<ArtworkLibraryWorkspace> acquire(std::string const &slot);
    // Prefer this dock's detached slot; otherwise reuse a detached workspace.
    // Reservation spans widget construction, including synchronous callbacks.
    std::unique_ptr<ArtworkLibraryPanelReservation> reserve_panel(std::string const &preferred = {});
    std::string retain(std::shared_ptr<ArtworkLibraryWorkspace> const &);
    std::uint64_t attach(std::string const &slot, std::function<Gtk::Widget *()> live_widget,
                std::function<bool()> ready,
                ArtworkLibraryPanelReservation const *reservation = nullptr); // one live panel per slot
    void detach(std::string const &slot, std::uint64_t attachment = 0); // nonzero releases only its own attachment
    std::vector<std::string> slots() const;
    Gtk::Widget *live_panel(std::string const &slot) const;
    std::vector<std::string> within(Gtk::Widget const &) const; // implemented in native adapter
    bool needs_hold() const;
    std::unique_ptr<LibraryCloseGuard> prepare(std::vector<std::string>, LibraryCloseUI &);
    sigc::signal<void()> changed;
private:
    friend class ArtworkLibraryPanelReservation;
    void release_panel(ArtworkLibraryPanelReservation const &) noexcept;
    struct Entry {
        std::shared_ptr<ArtworkLibraryWorkspace> workspace;
        std::function<Gtk::Widget *()> widget;
        std::function<bool()> ready;
        sigc::scoped_connection changed;
        sigc::scoped_connection session_changed;
        std::optional<LibrarySavedSession> reopen;
        ArtworkLibraryPanelReservation const *opening = nullptr;
        std::uint64_t attachment = 0;
    };
    ArtworkLibraryHost() = default;
    void owner() const;
    void observe(Entry &);
    void save_session();
    std::string _preferences_path;
    std::thread::id _owner = std::this_thread::get_id();
    std::map<std::string, Entry> _entries;
    std::weak_ptr<ArtworkLibraryClose> _session;
};
}
#endif
