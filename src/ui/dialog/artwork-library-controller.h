// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_ARTWORK_LIBRARY_CONTROLLER_H
#define INKSCAPE_ARTWORK_LIBRARY_CONTROLLER_H
#include "artwork-library-view.h"
#include "artwork-library-workspace.h"
#include "artwork-library-host.h"
#include "dialog-base.h"
#include "ui/cache/artwork-library-thumbnail.h"
#include "ui/iconview-item-factory.h"
#include <giomm/liststore.h>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/popovermenu.h>
#include <map>

namespace Inkscape::UI::Dialog {
struct ArtworkLibraryRow;
// Dockable artwork collection panel. Host owns the workspace across panels and must honor
// the native host close guard before unloading, never from a destructor.
class ArtworkLibraryController final : public DialogBase {
public:
    explicit ArtworkLibraryController(std::shared_ptr<ArtworkLibraryWorkspace>);
    ArtworkLibraryController(std::shared_ptr<ArtworkLibraryWorkspace>,
                             std::shared_ptr<Cache::ArtworkLibraryThumbnailPool>);
    ~ArtworkLibraryController() override;
    explicit ArtworkLibraryController(std::string const &retained_slot);
    explicit ArtworkLibraryController(std::unique_ptr<ArtworkLibraryPanelReservation>);
    explicit ArtworkLibraryController(ArtworkLibraryPanelReservation const &);
    std::string const &host_slot() const { return _host_slot; }
    Glib::RefPtr<Gio::SimpleActionGroup> actions() const { return _actions; }
    void status(std::string const &);
    void fonts_changed(); // routes once through application-wide epoch + visible-page refresh
    std::shared_ptr<Cache::ArtworkLibraryThumbnailPool> thumbnail_pool() const { return _thumbnail_pool; }
private:
    ArtworkLibraryController(std::shared_ptr<ArtworkLibraryWorkspace>,
                             std::shared_ptr<Cache::ArtworkLibraryThumbnailPool>,
                             ArtworkLibraryPanelReservation const *);
    void thumbnail_environment_changed();
    void desktopReplaced() override;
    void documentReplaced() override;
    void selectionChanged(Selection *) override;
    void selectionModified(Selection *, guint) override;
    void refresh();
    void enable();
    void invalidate(Cache::ThumbnailInvalidation);
    void schedule_visible();
    void visible();
    void submit_thumbnails();
    void trim_recent_images();
    void bind_picture(Glib::RefPtr<ArtworkLibraryRow> const &);
    void invoke(std::string const &);
    void choose(std::string operation);
    void add_selection();
    void insert();
    void review_import();
    void schedule_import_review();
    void review_recovery();
    void schedule_lock_review();
    void review_lock();
    // Single-line by default (Enter confirms); multiline only for one-per-line lists.
    std::optional<std::string> ask(std::string title, std::string initial, bool multiline = false);
    std::optional<std::string> choose_removed();
    int confirm(std::string title, std::string detail, bool save_choice = false, std::string accept_label = {});
    std::string selected_id() const;
    std::optional<Art::ValidatedSvg> selected_svg() const;
    std::shared_ptr<ArtworkLibraryWorkspace> _workspace;
    std::shared_ptr<Cache::ArtworkLibraryThumbnailPool> _thumbnail_pool;
    std::shared_ptr<ArtworkLibraryHost> _host;
    std::string _host_slot;
    std::uint64_t _host_attachment = 0;
    std::shared_ptr<ArtworkLibraryController *> _alive;
    Glib::RefPtr<Gtk::Builder> _builder;
    Glib::RefPtr<Gio::SimpleActionGroup> _actions;
    ArtworkLibraryView _view;
    Glib::RefPtr<Gio::ListStore<ArtworkLibraryRow>> _store;
    Glib::RefPtr<Gtk::SingleSelection> _selection;
    std::unique_ptr<IconViewItemFactory> _factory;
    std::unique_ptr<Gtk::PopoverMenu> _context;
    Cache::ArtworkLibraryThumbnails _thumbnails;
    std::map<std::string, std::shared_ptr<Cache::Thumbnail const>> _images;
    std::map<std::string, std::uint64_t> _image_used;
    std::uint64_t _image_clock = 0;
    std::map<std::string, std::string> _render_errors;
    std::map<std::string, Glib::RefPtr<ArtworkLibraryRow>> _by_id;
    std::vector<std::string> _visible_ids;
    std::vector<std::pair<std::string, std::string>> _collection_labels;
    std::uint64_t _submitted_generation = 0;
    std::uint64_t _rows_generation = 0;
    std::uint64_t _enable_generation = 0;
    unsigned _size = 80;
    int _last_width = 0, _last_height = 0, _last_scale = 0;
    bool _refreshing = false, _reviewing = false, _page_needed = false, _prompting = false;
    std::string _pending_query;
    GCancellable *_chooser = nullptr;
    // Collection to unload after the Save chosen in the Unload prompt succeeds.
    std::string _unload_after_save;
    sigc::scoped_connection _changed, _visible_idle, _search_idle, _poll_layout, _review_idle, _lock_idle;
    sigc::scoped_connection _thumbnail_environment;
};
}
#endif
