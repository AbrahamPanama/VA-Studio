// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/explode-bitmap-publication.h"
#include "artwork-library-controller.h"
#include "artwork-library-file-dialog.h"
#include "artwork-library-placement.h"
#include "io/artwork-library-document.h"
#include "desktop.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape-application.h"
#include "selection.h"
#include "preferences.h"
#include "ui/builder-utils.h"
#include "ui/dialog-run.h"
#include "util/value-utils.h"
#include "util/scope_exit.h"
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/main.h>
#include <glibmm/i18n.h>
#include <glibmm/markup.h>
#include <gtkmm/application.h>
#include <gtkmm/centerbox.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/dialog.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gesturesingle.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gridview.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/scale.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/searchentry2.h>
#include <gtkmm/textview.h>
#include <gtkmm/window.h>
#include <gtk/gtk.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace Inkscape::UI::Dialog {
struct ArtworkLibraryRow final : Glib::Object {
    Art::Asset asset;
    static Glib::RefPtr<ArtworkLibraryRow> make(Art::Asset a) {
        auto row = Glib::make_refptr_for_instance(new ArtworkLibraryRow); row->asset = std::move(a); return row;
    }
};
namespace {
constexpr char const *pref = "/dialogs/artwork-library/thumbnail-size";
auto application_thumbnail_pool() {
    auto app = InkscapeApplication::instance();
    if (!app) throw std::logic_error("Artwork Library requires an application");
    return app->artworkLibraryThumbnailPool();
}
auto require_application_pool(std::shared_ptr<Cache::ArtworkLibraryThumbnailPool> pool) {
    if (!pool || pool != application_thumbnail_pool())
        throw std::invalid_argument("Artwork Library requires the single application thumbnail pool");
    return pool;
}
auto reserved_workspace(ArtworkLibraryPanelReservation const *reservation) {
    auto app = InkscapeApplication::instance();
    if (!reservation || !app || !reservation->belongs_to(*app->artworkLibraries(true)))
        throw std::invalid_argument("Artwork Library requires a reservation from the application host");
    return reservation->workspace();
}
template <typename F> void safe(std::shared_ptr<ArtworkLibraryController *> life, F operation) {
    if (!*life) return;
    try { operation(**life); }
    catch (std::exception const &e) { if (*life) (**life).status(e.what()); }
}
std::string trim_line(std::string text)
{
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}
}
ArtworkLibraryController::ArtworkLibraryController(std::string const &slot)
    : ArtworkLibraryController(InkscapeApplication::instance()->artworkLibraries(true)->acquire(slot)) {}
ArtworkLibraryController::ArtworkLibraryController(std::unique_ptr<ArtworkLibraryPanelReservation> reservation)
    : ArtworkLibraryController(reserved_workspace(reservation.get()),
                               application_thumbnail_pool(), reservation.get()) {}
ArtworkLibraryController::ArtworkLibraryController(ArtworkLibraryPanelReservation const &reservation)
    : ArtworkLibraryController(reserved_workspace(&reservation), application_thumbnail_pool(), &reservation) {}
ArtworkLibraryController::ArtworkLibraryController(std::shared_ptr<ArtworkLibraryWorkspace> workspace)
    : ArtworkLibraryController(std::move(workspace), application_thumbnail_pool()) {}
ArtworkLibraryController::ArtworkLibraryController(
    std::shared_ptr<ArtworkLibraryWorkspace> workspace,
    std::shared_ptr<Cache::ArtworkLibraryThumbnailPool> pool)
    : ArtworkLibraryController(std::move(workspace), std::move(pool), nullptr) {}
ArtworkLibraryController::ArtworkLibraryController(
    std::shared_ptr<ArtworkLibraryWorkspace> workspace,
    std::shared_ptr<Cache::ArtworkLibraryThumbnailPool> pool,
    ArtworkLibraryPanelReservation const *reservation)
    : DialogBase("/dialogs/artwork-library", "ArtworkLibrary")
    , _workspace(std::move(workspace)), _thumbnail_pool(require_application_pool(std::move(pool)))
    , _alive(std::make_shared<ArtworkLibraryController *>(this))
    , _builder(create_builder("dialog-artwork-library.glade")), _actions(Gio::SimpleActionGroup::create())
    , _view(_builder, _actions), _store(Gio::ListStore<ArtworkLibraryRow>::create())
    , _selection(Gtk::SingleSelection::create(_store))
    , _thumbnails(_thumbnail_pool)
{
    if (!_workspace) throw std::invalid_argument("Artwork Library requires an application-retained workspace");
    bool constructed = false;
    auto rollback = scope_exit{[&] {
        if (constructed) return;
        *_alive = nullptr; // The controller destructor does not run on constructor failure.
        if (_host_attachment) _host->detach(_host_slot, _host_attachment);
        if (_context && _context->get_parent()) _context->unparent();
        if (auto parent = dynamic_cast<Gtk::Box *>(_view.get_parent())) parent->remove(_view);
    }};
    append(_view); auto life = _alive;
    for (auto name : {"new", "open", "save", "save-as", "unload", "add-selection", "import", "insert",
                      "rename", "tags", "export", "remove", "restore", "recover", "find-recovery", "review-recovery", "cancel", "review-import",
                      "remove-lock"}) {
        _actions->add_action(name, [life, name] { safe(life, [name](auto &s) { s.invoke(name); }); });
    }
    auto select = Gio::SimpleAction::create("collection", Glib::VariantType("s"));
    select->signal_activate().connect([life](Glib::VariantBase const &v) {
        safe(life, [life, &v](auto &s) { s.invalidate(Cache::ThumbnailInvalidation::CollectionChange);
            if (!*life) return;
            s._workspace->select_collection(Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(v).get().raw()); });
    });
    _actions->add_action(select);
    get_widget<Gtk::ListBox>(_builder, "collection-list").signal_row_selected().connect([life](Gtk::ListBoxRow *row) {
        safe(life, [row](auto &s) {
            if (!row || s._refreshing) return;
            auto id = row->get_name();
            if (id != s._workspace->active_id())
                s._actions->activate_action("collection", Glib::Variant<Glib::ustring>::create(id));
        });
    });
    _size = std::clamp(Preferences::get()->getInt(pref, 80), 48, 160);
    get_widget<Gtk::Scale>(_builder, "thumbnail-size").set_value(_size);
    get_widget<Gtk::Label>(_builder, "thumbnail-size-value").set_text(std::to_string(_size) + " px");
    auto &grid = get_widget<Gtk::GridView>(_builder, "artwork-grid");
    _factory = IconViewItemFactory::create([life](auto &ptr) -> IconViewItemFactory::ItemData {
        auto row = std::dynamic_pointer_cast<ArtworkLibraryRow>(ptr); if (!*life || !row) return {};
        auto &s = **life; auto found = s._images.find(row->asset.id);
        s.schedule_visible();
        return {Glib::Markup::escape_text(row->asset.name), found == s._images.end() ? nullptr : found->second->texture, row->asset.name, int(s._size)};
    });
    _factory->set_track_bindings(true); grid.set_factory(_factory->get_factory()); grid.set_model(_selection);
    _selection->set_autoselect(false);
    _selection->property_selected().signal_changed().connect([life] { safe(life, [](auto &s) { s.enable(); }); });
    grid.signal_activate().connect([life](guint) { safe(life, [](auto &s) { s.insert(); }); });
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect([life](unsigned key, unsigned, Gdk::ModifierType) {
        if (!*life) return false;
        if (key == GDK_KEY_Insert) { safe(life, [](auto &s) { s.insert(); }); return true; }
        if (key == GDK_KEY_Escape) {
            safe(life, [life](auto &s) {
                auto workspace = s._workspace;
                workspace->cancel(); // changed/enable observers may replace this panel
                if (!*life) return;
                s.invalidate(Cache::ThumbnailInvalidation::Search);
            });
            return true;
        }
        return false;
    }, false); add_controller(keys);
    _context = std::make_unique<Gtk::PopoverMenu>(get_object<Gio::Menu>(_builder, "artwork-menu")); _context->set_parent(grid);
    auto right = Gtk::GestureClick::create(); right->set_button(3);
    right->signal_pressed().connect([life](int, double x, double y) {
        safe(life, [life, x, y](auto &s) {
            auto &grid = get_widget<Gtk::GridView>(s._builder, "artwork-grid");
            for (auto child = grid.get_last_child(); child; child = child->get_prev_sibling()) {
                if (!child->get_visible() || !child->get_child_visible()) continue;
                int bx = 0, by = 0, w = 0, h = 0;
                if (!child->get_bounds(bx, by, w, h) || w <= 0 || h <= 0) continue;
                if (x < bx || x >= double(bx) + w || y < by || y >= double(by) + h) continue;
                auto row = std::dynamic_pointer_cast<ArtworkLibraryRow>(s._factory->find_item(*child));
                if (!row) continue; // popover/decorative children must not mask an artwork row
                for (guint i = 0; i < s._store->get_n_items(); ++i) if (s._store->get_item(i) == row) {
                    auto selection = s._selection;
                    selection->set_selected(i);
                    if (!*life) return;
                    break;
                }
                break;
            }
            s._context->set_pointing_to(Gdk::Rectangle(int(x), int(y), 1, 1));
            if (!*life) return;
            s._context->popup();
        });
    }); grid.add_controller(right);
    auto drag = Gtk::DragSource::create(); drag->set_actions(Gdk::DragAction::COPY);
    drag->signal_prepare().connect([life](double x, double y) -> Glib::RefPtr<Gdk::ContentProvider> {
        if (!*life) return {}; auto &s = **life; auto &grid = get_widget<Gtk::GridView>(s._builder, "artwork-grid");
        for (auto child = grid.get_last_child(); child; child = child->get_prev_sibling()) {
            if (!child->get_visible() || !child->get_child_visible()) continue;
            int bx = 0, by = 0, w = 0, h = 0;
            if (!child->get_bounds(bx, by, w, h) || w <= 0 || h <= 0) continue;
            if (x < bx || x >= double(bx) + w || y < by || y >= double(by) + h) continue;
            auto row = std::dynamic_pointer_cast<ArtworkLibraryRow>(s._factory->find_item(*child)); if (!row) continue;
            for (auto const &entry : s._workspace->page()) if (entry.metadata.id == row->asset.id && entry.svg)
                return Gdk::ContentProvider::create(Util::GlibValue::create<ArtworkDrop>(ArtworkDrop{*entry.svg}));
        }
        return {};
    }, false); grid.add_controller(drag);
    get_widget<Gtk::SearchEntry2>(_builder, "artwork-search").signal_search_changed().connect([life] {
        safe(life, [life](auto &s) {
            s._pending_query = get_widget<Gtk::SearchEntry2>(s._builder, "artwork-search").get_text();
            s.invalidate(Cache::ThumbnailInvalidation::Search); if (!*life) return;
            s._search_idle.disconnect();
            s._search_idle = Glib::signal_timeout().connect([life] {
                if (!*life) return false; auto &s = **life;
                if (s._workspace->closing()) return false;
                if (s._workspace->busy() || s._workspace->pending_import()) return true;
                safe(life, [](auto &v) { if (v._workspace->active()) v._workspace->search(v._pending_query); }); return false;
            }, 100);
        });
    });
    get_widget<Gtk::Scale>(_builder, "thumbnail-size").signal_value_changed().connect([life] {
        safe(life, [life](auto &s) {
            s._size = get_widget<Gtk::Scale>(s._builder, "thumbnail-size").get_value();
            get_widget<Gtk::Label>(s._builder, "thumbnail-size-value").set_text(std::to_string(s._size) + " px");
            if (!*life) return;
            Preferences::get()->setInt(pref, s._size); if (!*life) return;
            s.invalidate(Cache::ThumbnailInvalidation::Resize); if (!*life) return;
            s.schedule_visible();
        });
    });
    auto &scroller = get_widget<Gtk::ScrolledWindow>(_builder, "artwork-scroller");
    scroller.get_vadjustment()->signal_value_changed().connect([life] { safe(life, [](auto &s) { s.schedule_visible(); }); });
    _poll_layout = Glib::signal_timeout().connect([life] { if (!*life) return false; safe(life, [](auto &s) { if (s.get_mapped()) s.visible(); }); return true; }, 100);
    _changed = _workspace->changed.connect([life] { safe(life, [](auto &s) { s.refresh(); }); });
    signal_map().connect([life] { safe(life, [](auto &s) { s.schedule_import_review(); }); });
    refresh();
    _host = getApp()->artworkLibraries(true);
    _host_slot = reservation ? reservation->slot() : _host->retain(_workspace);
    _host_attachment = _host->attach(_host_slot, [life]() -> Gtk::Widget * { return *life; },
        [life] { return !*life || (!(*life)->_chooser && !(*life)->_prompting && !(*life)->_reviewing); }, reservation);
    _thumbnail_environment = getApp()->connectArtworkLibraryThumbnailEnvironment([life] {
        safe(life, [](auto &s) { s.thumbnail_environment_changed(); });
    });
    constructed = true;
}
ArtworkLibraryController::~ArtworkLibraryController() {
    *_alive = nullptr;
    _thumbnail_environment.disconnect();
    if (_host && _host_attachment) _host->detach(_host_slot, _host_attachment);
    if (_chooser) { g_cancellable_cancel(_chooser); g_object_unref(_chooser); _chooser = nullptr; }
    _changed.disconnect(); _visible_idle.disconnect(); _search_idle.disconnect(); _poll_layout.disconnect(); _review_idle.disconnect();
    _lock_idle.disconnect();
    _thumbnails.invalidate(Cache::ThumbnailInvalidation::Close); _images.clear();
    if (_context) _context->unparent();
    auto &grid = get_widget<Gtk::GridView>(_builder, "artwork-grid"); grid.set_model({}); grid.set_factory({});
    // DialogNotebook::add_page reparents our view into its scrolling wrapper.
    if (auto parent = dynamic_cast<Gtk::Box *>(_view.get_parent())) parent->remove(_view);
}
void ArtworkLibraryController::status(std::string const &message) {
    auto life = _alive; auto &label = get_widget<Gtk::Label>(_builder, "library-status"); label.set_text(message); if (*life) label.set_tooltip_text(message);
}
std::string ArtworkLibraryController::selected_id() const {
    auto row = std::dynamic_pointer_cast<ArtworkLibraryRow>(_selection->get_selected_item()); return row ? row->asset.id : "";
}
std::optional<Art::ValidatedSvg> ArtworkLibraryController::selected_svg() const {
    auto id = selected_id(); for (auto const &e : _workspace->page()) if (e.metadata.id == id) return e.svg; return {};
}
void ArtworkLibraryController::enable() {
    auto life = _alive; auto generation = ++_enable_generation;
    bool idle = !_workspace->closing() && !_workspace->busy() && !_workspace->pending_import() && !_chooser && !_prompting;
    bool collection = _workspace->active(); bool asset = !selected_id().empty();
    bool doc = getDesktop() && getDocument() && !DocumentUndo::interactionCloseRequested(getDocument());
    std::vector<std::pair<Glib::RefPtr<Gio::SimpleAction>, bool>> updates;
    auto set = [&](char const *name, bool enabled) {
        updates.emplace_back(std::dynamic_pointer_cast<Gio::SimpleAction>(_actions->lookup_action(name)), enabled);
    };
    for (auto name : {"new", "open", "recover", "find-recovery"}) set(name, idle);
    set("review-recovery", idle && bool(_workspace->recovery_scan()));
    set("remove-lock", idle && collection && !_workspace->active()->path.empty());
    for (auto name : {"unload", "import"}) set(name, idle && collection);
    for (auto name : {"save", "save-as"}) set(name, idle && collection && !_workspace->active()->uncertain);
    for (auto name : {"rename", "tags", "export", "remove"}) set(name, idle && collection && asset);
    set("collection", idle);
    set("restore", idle && collection && !_workspace->active()->catalog.snapshot().removed_ids().empty());
    set("add-selection", idle && collection && doc && getSelection() && !getSelection()->isEmpty());
    bool eligible = false;
    if (idle && doc && selected_svg()) {
        try { auto parent = getSelection()->activeContext(); if (parent) { (void)Art::capture_insertion_target(*getDocument(), *parent); eligible = true; } } catch (...) {}
    }
    set("insert", eligible);
    set("cancel", !_workspace->closing() && _workspace->busy());
    set("review-import", !_workspace->closing() && bool(_workspace->pending_import()));
    get_widget<Gtk::ListBox>(_builder, "collection-list").set_sensitive(idle);
    // Notifications may destroy this panel or synchronously open another chooser.
    // A nested pass owns the new state; never resume an obsolete update batch.
    for (auto const &[action, enabled] : updates) {
        if (!*life || (*life)->_enable_generation != generation) return;
        action->set_enabled(enabled);
    }
}
void ArtworkLibraryController::refresh() {
    auto life = _alive;
    if (_refreshing) return; _refreshing = true;
    auto cleanup = scope_exit([life] { if (*life) (*life)->_refreshing = false; });
    auto workspace = _workspace; auto store = _store; auto selection = _selection;
    bool same = _rows_generation == workspace->rows_generation();
    _rows_generation = workspace->rows_generation();
    bool empty = workspace->rows().empty();
    // Snapshot values before any native notification; callbacks may edit the
    // workspace or replace this entire panel synchronously.
    auto rows = same ? std::vector<Art::Asset>{} : workspace->rows();
    std::vector<std::pair<std::string, std::string>> collections;
    for (auto const &c : workspace->collections()) collections.emplace_back(c.identity, c.label + (c.dirty() ? " *" : ""));
    auto active = workspace->active();
    auto active_id = workspace->active_id();
    std::string label = active ? active->label + (active->dirty() ? " *" : "") : "Artwork Library";
    if (!same) {
        // Metadata edits invalidate admission, even with identical visible IDs.
        // Admission completion changes page_generation only, NOT rows_generation.
        invalidate(Cache::ThumbnailInvalidation::CollectionChange); if (!*life) return;
        auto old = selected_id(); _by_id.clear(); store->remove_all(); if (!*life) return;
        for (auto const &asset : rows) {
            auto row = ArtworkLibraryRow::make(asset); _by_id.emplace(asset.id, row);
            store->append(row); if (!*life) return;
        }
        for (guint i = 0; i < store->get_n_items(); ++i) if (store->get_item(i)->asset.id == old) {
            selection->set_selected(i); if (!*life) return;
        }
    }
    auto &list = get_widget<Gtk::ListBox>(_builder, "collection-list");
    if (collections != _collection_labels) {
        _collection_labels = collections;
        list.remove_all(); if (!*life) return;
        auto menu = get_object<Gio::Menu>(_builder, "open-collections"); menu->remove_all(); if (!*life) return;
        for (auto const &[id, title] : collections) {
            auto row = Gtk::make_managed<Gtk::ListBoxRow>();
            row->set_name(id);
            auto name = Gtk::make_managed<Gtk::Label>(title);
            name->set_xalign(0); name->set_ellipsize(Pango::EllipsizeMode::END);
            name->set_width_chars(12); name->set_max_width_chars(12);
            name->set_margin_start(8); name->set_margin_end(8);
            name->set_margin_top(8); name->set_margin_bottom(8);
            name->set_tooltip_text(title); row->set_child(*name);
            list.append(*row); if (!*life) return;
            auto item = Gio::MenuItem::create(title, "");
            item->set_action_and_target("library.collection", Glib::Variant<Glib::ustring>::create(id));
            menu->append_item(item); if (!*life) return;
        }
    }
    for (auto child = list.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto row = dynamic_cast<Gtk::ListBoxRow *>(child); row && row->get_name() == active_id) {
            list.select_row(*row); if (!*life) return;
            break;
        }
    }
    get_widget<Gtk::Label>(_builder, "collection-name").set_tooltip_text(label); if (!*life) return;
    get_widget<Gtk::Label>(_builder, "empty-message").set_visible(empty); if (!*life) return;
    status(workspace->message()); if (!*life) return;
    enable(); if (!*life) return;
    if (!workspace->busy() && !workspace->closing()) {
        submit_thumbnails(); if (!*life) return;
        schedule_visible(); if (!*life) return;
    }
    if (workspace->pending_import() && !workspace->closing()) {
        status("Checking imported artwork…"); if (!*life) return;
        schedule_import_review();
    }
    if (!_unload_after_save.empty() && !workspace->busy() && !_chooser && !_prompting && !workspace->closing()) {
        auto collection = std::exchange(_unload_after_save, {});
        // Defer: refresh() is not reentrant, and unload() notifies synchronously.
        Glib::signal_idle().connect([life, collection] {
            safe(life, [&](auto &s) {
                auto c = s._workspace->active();
                if (c && c->identity == collection && !c->dirty() && !c->uncertain &&
                    !s._workspace->busy() && !s._workspace->closing()) s._workspace->unload();
            });
            return false;
        });
    }
    if (workspace->inspected_lock() && !workspace->busy() && !workspace->closing()) schedule_lock_review();
}
void ArtworkLibraryController::invalidate(Cache::ThumbnailInvalidation reason) {
    auto life = _alive;
    _thumbnails.invalidate(reason); _workspace->invalidate_page(); _images.clear(); _image_used.clear(); _render_errors.clear(); _visible_ids.clear(); _submitted_generation = 0; _page_needed = true;
    auto &grid = get_widget<Gtk::GridView>(_builder, "artwork-grid");
    for (auto child = grid.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto row = std::dynamic_pointer_cast<ArtworkLibraryRow>(_factory->find_item(*child))) bind_picture(row);
        if (!*life) return; // do not advance a child iterator from a destroyed grid
    }
}
void ArtworkLibraryController::fonts_changed() {
    // No member access after fanout: a callback can destroy this controller.
    getApp()->invalidateArtworkLibraryThumbnails();
}
void ArtworkLibraryController::thumbnail_environment_changed() {
    auto life = _alive;
    // The application already advanced the pool epoch. FontsChanged here would
    // invalidate the shared pool once AGAIN for every panel in the fanout.
    invalidate(Cache::ThumbnailInvalidation::DocumentChange); if (!*life) return;
    schedule_visible();
}
void ArtworkLibraryController::schedule_visible() {
    if (_visible_idle.connected()) return; auto life = _alive;
    _visible_idle = Glib::signal_idle().connect([life] { if (*life) safe(life, [](auto &s) { s.visible(); }); return false; });
}
void ArtworkLibraryController::bind_picture(Glib::RefPtr<ArtworkLibraryRow> const &row) {
    auto life = _alive;
    auto item = row; // caller may pass a map value invalidated by a notification
    auto &grid = get_widget<Gtk::GridView>(_builder, "artwork-grid");
    auto box = dynamic_cast<Gtk::CenterBox *>(_factory->find_child_item(grid, item)); if (!box) return;
    std::string detail = item->asset.name;
    for (auto const &entry : _workspace->page()) if (entry.metadata.id == item->asset.id) {
        if (!entry.diagnostic.empty()) detail += "\n" + entry.diagnostic;
        if (entry.svg) for (auto const &warning : entry.svg->warnings()) detail += "\n" + warning;
    }
    if (auto error = _render_errors.find(item->asset.id); error != _render_errors.end()) detail += "\n" + error->second;
    if (item->asset.extra_json.find("LightBurnShapes-v1") != std::string::npos)
        detail += "\nLightBurn import: original cut/print styling is not reconstructed; laser tabs, if present, remain unapplied. Full diagnostics are retained in the collection metadata.";
    box->set_tooltip_text(detail); if (!*life) return;
    if (auto picture = dynamic_cast<Gtk::Picture *>(box->get_start_widget())) {
        auto image = _images.find(item->asset.id); auto texture = image == _images.end() ? Glib::RefPtr<Gdk::Texture>{} : image->second->texture;
        picture->set_paintable(texture); if (!*life) return;
        picture->set_size_request(_size, _size); if (!*life) return;
    }
    if (auto label = dynamic_cast<Gtk::Label *>(box->get_end_widget())) { label->set_width_chars(10); if (!*life) return;
        label->set_max_width_chars(10); if (!*life) return;
        label->set_ellipsize(Pango::EllipsizeMode::END); }
}
void ArtworkLibraryController::visible() {
    auto life = _alive;
    if (_workspace->closing() || !get_mapped() || !_workspace->active() || _workspace->pending_import() || _search_idle.connected()) return;
    auto &grid = get_widget<Gtk::GridView>(_builder, "artwork-grid"); auto &scroll = get_widget<Gtk::ScrolledWindow>(_builder, "artwork-scroller");
    if (scroll.get_width() != _last_width || scroll.get_height() != _last_height || get_scale_factor() != _last_scale) {
        _last_width = scroll.get_width(); _last_height = scroll.get_height(); _last_scale = get_scale_factor();
        invalidate(Cache::ThumbnailInvalidation::Resize); if (!*life) return;
    }
    std::vector<std::string> ids;
    for (auto child = grid.get_first_child(); child; child = child->get_next_sibling()) {
        auto row = std::dynamic_pointer_cast<ArtworkLibraryRow>(_factory->find_item(*child)); if (!row) continue;
        bind_picture(row); if (!*life) return;
        graphene_rect_t bounds;
        if (child->get_child_visible() && gtk_widget_compute_bounds(child->gobj(), GTK_WIDGET(scroll.gobj()), &bounds) &&
            bounds.origin.y + bounds.size.height > 0 && bounds.origin.y < scroll.get_height() &&
            bounds.origin.x + bounds.size.width > 0 && bounds.origin.x < scroll.get_width() && ids.size() < 128) ids.push_back(row->asset.id);
    }
    if (ids != _visible_ids) {
        // A viewport change does not invalidate artwork. Keep recent textures
        // and let the admitted page / thumbnail work finish, rather than
        // cancelling and blanking the same rows on every scroll event.
        auto offscreen = [&ids](auto const &entry) { return std::find(ids.begin(), ids.end(), entry.first) == ids.end(); };
        std::erase_if(_render_errors, offscreen);
        _visible_ids = ids; _page_needed = true;
        for (auto const &id : ids) if (_images.contains(id)) _image_used[id] = ++_image_clock;
        trim_recent_images();
        for (auto child = grid.get_first_child(); child; child = child->get_next_sibling()) {
            if (auto row = std::dynamic_pointer_cast<ArtworkLibraryRow>(_factory->find_item(*child))) bind_picture(row);
            if (!*life) return;
        }
    }
    submit_thumbnails(); if (!*life) return;
    if (_workspace->busy() || _thumbnails.pending_sources() || !_page_needed) return;
    _page_needed = false;
    if (!ids.empty()) _workspace->visible(std::move(ids));
}
void ArtworkLibraryController::trim_recent_images() {
    // Retain at most 128 offscreen images / 16 MiB for instant return scrolling.
    // These are existing charged textures, NOT a second pixel cache. The shared
    // pool's 128 MiB hard cap still covers visible and retained images together.
    auto visible = [&](std::string const &id) {
        return std::find(_visible_ids.begin(), _visible_ids.end(), id) != _visible_ids.end();
    };
    auto cost = [](Cache::Thumbnail const &image) {
        auto bytes = image.bytes + std::size_t(4096);
        for (auto warning : image.warnings) bytes += warning.size() + sizeof(std::string_view);
        return bytes;
    };
    std::size_t count = 0, bytes = 0;
    for (auto const &[id, image] : _images) if (!visible(id)) { ++count; bytes += cost(*image); }
    while (count > 128 || bytes > 16u * 1024 * 1024) {
        auto oldest = _images.end();
        for (auto it = _images.begin(); it != _images.end(); ++it) {
            if (visible(it->first)) continue;
            if (oldest == _images.end() || _image_used.at(it->first) < _image_used.at(oldest->first)) oldest = it;
        }
        if (oldest == _images.end()) break;
        bytes -= cost(*oldest->second); --count;
        _image_used.erase(oldest->first); _images.erase(oldest);
    }
}
void ArtworkLibraryController::submit_thumbnails() {
    auto life = _alive;
    if (_workspace->closing() || _thumbnails.pending_sources()) return;
    if (_submitted_generation == _workspace->page_generation() || _workspace->page().empty()) return;
    _submitted_generation = _workspace->page_generation(); std::vector<Cache::ThumbnailDemand> demand; std::vector<std::string> ids;
    auto page = _workspace->page();
    for (auto const &e : page) {
        if (_images.contains(e.metadata.id) ||
            std::find(_visible_ids.begin(), _visible_ids.end(), e.metadata.id) == _visible_ids.end()) continue;
        if (e.svg) { demand.push_back({*e.svg, {_size, _size, unsigned(std::clamp(get_scale_factor(), 1, 4))}}); ids.push_back(e.metadata.id); }
        else { status(e.metadata.name + ": " + e.diagnostic); if (!*life) return; }
    }
    if (demand.empty()) return;
    _thumbnails.set_visible(std::move(demand), [life, ids](std::uint64_t generation, std::size_t index, Cache::ThumbnailResult result) {
        safe(life, [&](auto &s) {
            if (generation != s._thumbnails.generation() || index >= ids.size()) return;
            if (result.image) {
                s._images[ids[index]] = result.image; s._image_used[ids[index]] = ++s._image_clock;
                s.trim_recent_images();
            } else { s._render_errors[ids[index]] = result.diagnostic; s.status(result.diagnostic); if (!*life) return; }
            if (auto row = s._by_id.find(ids[index]); row != s._by_id.end()) s.bind_picture(row->second);
            if (*life) s.schedule_visible();
        });
    });
}
void ArtworkLibraryController::desktopReplaced() {
    auto life = _alive;
    invalidate(Cache::ThumbnailInvalidation::DocumentChange); if (!*life) return;
    enable();
}
void ArtworkLibraryController::documentReplaced() {
    auto life = _alive;
    invalidate(Cache::ThumbnailInvalidation::DocumentChange); if (!*life) return;
    enable();
}
void ArtworkLibraryController::selectionChanged(Selection *) { enable(); }
void ArtworkLibraryController::selectionModified(Selection *, guint) { enable(); }
std::optional<std::string> ArtworkLibraryController::ask(std::string title, std::string initial, bool multiline) {
    if (getDesktop() && Bitmap::publicationBoundaryPending(getDesktop()->getDocument())) return {};
    auto life = _alive; auto workspace = _workspace; auto collection = workspace->active_id(); _prompting = true; enable(); if (!*life) return {};
    Gtk::Dialog dialog(title, true); if (auto parent = dynamic_cast<Gtk::Window *>(get_root())) dialog.set_transient_for(*parent);
    Gtk::Entry line; Gtk::TextView block;
    Gtk::Widget &input = multiline ? static_cast<Gtk::Widget &>(block) : static_cast<Gtk::Widget &>(line);
    if (multiline) { block.get_buffer()->set_text(initial); block.set_size_request(320, 96); }
    else { line.set_text(initial); line.set_activates_default(true); line.set_size_request(320, -1); }
    dialog.get_content_area()->append(input);
    dialog.add_button("Cancel", int(Gtk::ResponseType::CANCEL)); dialog.add_button("Apply", int(Gtk::ResponseType::OK));
    dialog.set_default_response(int(Gtk::ResponseType::OK));
    auto response = UI::dialog_run(dialog);
    auto text = multiline ? block.get_buffer()->get_text().raw() : line.get_text().raw();
    dialog.get_content_area()->remove(input);
    if (!*life) return {}; _prompting = false; enable(); if (!*life) return {};
    if (collection != workspace->active_id() || response != int(Gtk::ResponseType::OK)) return {};
    if (text.size() > 4096) throw std::runtime_error("Text entry exceeds 4096 UTF-8 bytes");
    if (!multiline) {
        // Pasted text can still carry line breaks; a name is one line.
        for (auto &ch : text) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        text = trim_line(std::move(text));
        if (text.empty()) { status("A name is required; nothing was changed"); return {}; }
    }
    return text;
}
int ArtworkLibraryController::confirm(std::string title, std::string detail, bool save_choice, std::string accept_label) {
    if (getDesktop() && Bitmap::publicationBoundaryPending(getDesktop()->getDocument())) return int(Gtk::ResponseType::CANCEL);
    auto life = _alive; auto workspace = _workspace; auto collection = workspace->active_id(); _prompting = true; enable(); if (!*life) return {};
    Gtk::MessageDialog dialog(title, false, Gtk::MessageType::QUESTION, Gtk::ButtonsType::NONE, true);
    if (auto parent = dynamic_cast<Gtk::Window *>(get_root())) dialog.set_transient_for(*parent);
    dialog.set_secondary_text(detail);
    dialog.add_button("Cancel", int(Gtk::ResponseType::CANCEL));
    dialog.add_button(save_choice ? "Discard / Unload" : accept_label.empty() ? "Accept" : accept_label,
                      int(Gtk::ResponseType::ACCEPT));
    if (save_choice) dialog.add_button("Save", int(Gtk::ResponseType::APPLY));
    auto response = UI::dialog_run(dialog);
    if (!*life) return int(Gtk::ResponseType::CANCEL); _prompting = false; enable(); if (!*life) return {};
    return collection == workspace->active_id() ? response : int(Gtk::ResponseType::CANCEL);
}
void ArtworkLibraryController::schedule_import_review() {
    if (_review_idle.connected() || _reviewing || !_workspace->pending_import() || _workspace->closing()) return;
    auto life = _alive;
    // Finish the workspace notification and native chooser callback before
    // publishing the import or opening its error report.
    _review_idle = Glib::signal_idle().connect([life] {
        if (!*life) return false;
        (*life)->_review_idle.disconnect();
        safe(life, [](auto &s) {
            if (s.get_mapped() && !s._chooser && !s._prompting) s.review_import();
        });
        return false;
    });
}
void ArtworkLibraryController::review_import() {
    if (getDesktop() && Bitmap::publicationBoundaryPending(getDesktop()->getDocument())) return;
    if (_workspace->closing() || _reviewing || !_workspace->pending_import()) return;
    auto const &entries = _workspace->pending_import()->entries;
    if (!entries.empty() && std::all_of(entries.begin(), entries.end(), [](auto const &e) { return e.converted; })) {
        // Validation already ran on the worker. Opening/importing is the user's
        // authorization; successful files do not need a second confirmation.
        _workspace->accept_import(true);
        return;
    }
    bool const any_ready = std::any_of(entries.begin(), entries.end(), [](auto const &e) { return e.converted; });
    auto life = _alive; _reviewing = true; std::string detail;
    for (auto const &e : _workspace->pending_import()->entries)
        detail += e.source + " #" + std::to_string(e.ordinal) + " " + e.name + ": " + (e.converted ? "Ready — " : "Not imported — ") + e.message + "\n";
    auto collection = _workspace->active_id(); _prompting = true; enable(); if (!*life) return;
    Gtk::Dialog dialog("Review import results — only Ready entries will be added", true);
    if (auto parent = dynamic_cast<Gtk::Window *>(get_root())) dialog.set_transient_for(*parent);
    Gtk::ScrolledWindow scroll; Gtk::TextView text;
    text.set_editable(false); text.get_buffer()->set_text(detail); text.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    scroll.set_child(text); scroll.set_min_content_width(480); scroll.set_min_content_height(320);
    dialog.get_content_area()->append(scroll);
    dialog.add_button("Cancel import", int(Gtk::ResponseType::CANCEL)); dialog.add_button("Add Ready entries", int(Gtk::ResponseType::ACCEPT));
    dialog.set_response_sensitive(int(Gtk::ResponseType::ACCEPT), any_ready);
    auto response = UI::dialog_run(dialog); dialog.get_content_area()->remove(scroll); scroll.unset_child();
    if (!*life) return; _prompting = false; enable(); if (!*life) return;
    if (collection != _workspace->active_id()) response = int(Gtk::ResponseType::CANCEL);
    if (!*life) return; _reviewing = false; if (_workspace->pending_import()) _workspace->accept_import(response == int(Gtk::ResponseType::ACCEPT));
}
void ArtworkLibraryController::review_recovery() {
    if (getDesktop() && Bitmap::publicationBoundaryPending(getDesktop()->getDocument())) return;
    if (_workspace->closing() || _workspace->busy() || _prompting || _chooser || !_workspace->recovery_scan()) return;
    auto life = _alive; auto workspace = _workspace;
    auto scan = *workspace->recovery_scan(); // Immutable choices across the native nested loop.
    _prompting = true;
    auto release = scope_exit([life] { if (*life) { (*life)->_prompting = false; (*life)->enable(); } });
    enable(); if (!*life) return;
    Gtk::Dialog dialog(_("Recover artwork collection"), true);
    if (auto parent = dynamic_cast<Gtk::Window *>(get_root())) dialog.set_transient_for(*parent);
    dialog.set_default_size(480, 420);
    Gtk::Box content(Gtk::Orientation::VERTICAL, 8);
    Gtk::Label summary, detail; Gtk::ScrolledWindow scroll; Gtk::ListBox list;
    summary.set_text(scan.limited
        ? _("Discovery limits reached; this list is incomplete. Nothing has been restored.")
        : scan.files.empty() ? _("No retained recovery copies were found in this folder.")
        : _("Choose a copy to open as a new, unsaved collection. Original files are retained."));
    summary.set_wrap(true); summary.set_max_width_chars(60); summary.set_xalign(0);
    detail.set_wrap(true); detail.set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    detail.set_max_width_chars(60); detail.set_lines(5); detail.set_ellipsize(Pango::EllipsizeMode::END);
    detail.set_xalign(0); detail.set_selectable(true);
    list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    auto display_path = [](std::string const &path) {
        auto raw = g_filename_display_name(path.c_str()); std::string text(raw); g_free(raw); return text;
    };
    for (auto const &file : scan.files) {
        auto title = file.version ? file.label + " — r" + std::to_string(file.version->revision)
                                  : std::string(_("Not verified"));
        auto label = Gtk::make_managed<Gtk::Label>(title);
        label->set_xalign(0); label->set_margin(6);
        label->set_ellipsize(Pango::EllipsizeMode::END); label->set_max_width_chars(45);
        label->set_tooltip_text(title + "\n" + display_path(file.path)); list.append(*label);
    }
    scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll.set_min_content_height(140); scroll.set_vexpand(true); scroll.set_child(list);
    content.set_margin(12); content.append(summary); content.append(scroll); content.append(detail);
    dialog.get_content_area()->append(content);
    auto unparent = scope_exit([&] {
        dialog.get_content_area()->remove(content);
        content.remove(summary); content.remove(scroll); content.remove(detail); scroll.unset_child();
    });
    dialog.add_button(_("Cancel"), int(Gtk::ResponseType::CANCEL));
    dialog.add_button(_("Open recovery copy"), int(Gtk::ResponseType::ACCEPT));
    dialog.set_default_response(int(Gtk::ResponseType::CANCEL));
    dialog.set_response_sensitive(int(Gtk::ResponseType::ACCEPT), false);
    sigc::scoped_connection selected = list.signal_row_selected().connect([&](Gtk::ListBoxRow *row) {
        auto index = row ? row->get_index() : -1;
        bool valid = index >= 0 && std::size_t(index) < scan.files.size();
        dialog.set_response_sensitive(int(Gtk::ResponseType::ACCEPT), valid && bool(scan.files[index].version));
        auto text = valid ? display_path(scan.files[index].path) + "\n" + scan.files[index].diagnostic
                          : display_path(scan.directory);
        detail.set_text(text); detail.set_tooltip_text(text);
    });
    if (auto row = list.get_row_at_index(0)) list.select_row(*row);
    else detail.set_text(display_path(scan.directory));
    auto response = UI::dialog_run(dialog);
    auto row = list.get_selected_row(); auto index = row ? row->get_index() : -1;
    if (!*life || response != int(Gtk::ResponseType::ACCEPT) || index < 0 ||
        std::size_t(index) >= scan.files.size() || !scan.files[index].version) return;
    // Keep the prompt lease through dispatch. The worker checks the exact
    // discovered FileVersion again before admitting any SVG or publishing state.
    workspace->recover(scan.files[index].path, scan.files[index].version);
}
void ArtworkLibraryController::schedule_lock_review() {
    if (_lock_idle.connected() || _prompting || _chooser) return;
    auto life = _alive;
    _lock_idle = Glib::signal_idle().connect([life] {
        if (!*life) return false;
        (*life)->_lock_idle.disconnect();
        safe(life, [](auto &s) { if (s.get_mapped() && !s._prompting && !s._chooser) s.review_lock(); });
        return false;
    });
}
void ArtworkLibraryController::review_lock() {
    auto const &inspected = _workspace->inspected_lock();
    if (!inspected || _workspace->busy() || _workspace->closing()) return;
    // An inspection for another collection is stale; never offer it here.
    if (_workspace->inspected_lock_collection() != _workspace->active_id()) {
        _workspace->dismiss_lock("The lock check was for another collection; nothing was removed");
        return;
    }
    auto lock = *inspected; auto life = _alive; auto workspace = _workspace;
    std::string age = "an unknown time";
    if (lock.version.modified_seconds) {
        auto now = g_get_real_time() / G_USEC_PER_SEC;
        auto then = static_cast<std::int64_t>(lock.version.modified_seconds);
        auto seconds = now > then ? now - then : 0;
        age = seconds < 120 ? std::to_string(seconds) + " seconds"
            : seconds < 7200 ? std::to_string(seconds / 60) + " minutes"
            : std::to_string(seconds / 3600) + " hours";
    }
    auto detail = "Lock file: " + lock.path +
        "\nCreated by: " + (lock.holder.empty() ? std::string("unknown (written by an older VA Studio)") : lock.holder) +
        "\nLast changed: " + age + " ago"
        "\n\nRemove it only if no VA Studio on any computer is saving this library. Removing a lock "
        "that is in use can let two saves overwrite each other.";
    auto response = confirm("Remove stale library lock?", detail, false, "Remove lock");
    if (!*life) return;
    if (response == int(Gtk::ResponseType::ACCEPT)) workspace->remove_lock(lock);
    else workspace->dismiss_lock();
}
void ArtworkLibraryController::choose(std::string operation) {
    if (_chooser) return;
    auto life = _alive; auto kind = library_file_operation(operation);
    auto folder_pref = "/dialogs/artwork-library/paths/" + operation;
    auto folder = Preferences::get()->getString(folder_pref);
    std::string name;
    if (auto collection = _workspace->active()) {
        name = collection->label;
        if (operation == "save-as" && !collection->path.empty()) {
            auto base = g_path_get_basename(collection->path.c_str()); name = base; g_free(base);
            if (folder.empty()) { auto dir = g_path_get_dirname(collection->path.c_str()); folder = dir; g_free(dir); }
        }
        if (operation == "export") {
            auto id = selected_id(); if (!id.empty()) name = collection->catalog.snapshot().asset(id).name;
        }
    }
    auto chooser = create_library_file_dialog(kind, name, folder);
    auto parent = dynamic_cast<Gtk::Window *>(get_root());
    struct Request {
        std::shared_ptr<ArtworkLibraryController *> life;
        LibraryFileOperation operation;
        std::string asset, collection, folder_pref;
        Glib::RefPtr<Gtk::FileDialog> dialog;
        GCancellable *cancellable; // Identity only; the live controller owns it.
    };
    auto cancellable = std::unique_ptr<GCancellable, decltype(&g_object_unref)>(g_cancellable_new(), g_object_unref);
    auto request = std::make_unique<Request>(Request{life, kind, selected_id(), _workspace->active_id(), folder_pref, chooser, cancellable.get()});
    auto ready = +[](GObject *, GAsyncResult *result, void *data) {
        std::unique_ptr<Request> r(static_cast<Request *>(data));
        auto finished = scope_exit{[&] {
            safe(r->life, [&](auto &s) {
                if (s._chooser != r->cancellable) return;
                g_clear_object(&s._chooser);
                // Cancelled/failed Save As from the Unload prompt: never unload later.
                if (r->operation == LibraryFileOperation::SaveAs && !s._workspace->busy()) s._unload_after_save.clear();
                s.enable(); // Admit reentrant actions only after handling the old result.
            });
        }};
        try {
            auto choice = finish_library_file_dialog(*r->dialog, r->operation, result);
            safe(r->life, [&](auto &s) {
                if (s._chooser != r->cancellable || choice.result == LibraryFileChoice::Result::Cancelled) return;
                if (choice.result == LibraryFileChoice::Result::Failed) { s.status(choice.message); return; }
                if (r->collection != s._workspace->active_id()) { s.status("Collection changed while choosing a file; no operation performed"); return; }
                auto const &paths = choice.paths;
                if (r->operation == LibraryFileOperation::SaveAs) {
                    auto lower = g_ascii_strdown(paths.front().c_str(), -1); std::string suffix(lower); g_free(lower);
                    if (!suffix.ends_with(".valib")) { s.status("Choose a .valib destination; native libraries are not renamed .lbart files"); return; }
                }
                auto dir = g_path_get_dirname(paths.front().c_str());
                std::string folder = r->operation == LibraryFileOperation::FindRecoveryFolder ? paths.front() : std::string(dir);
                g_free(dir);
                Preferences::get()->setString(r->folder_pref, folder);
                if (!*r->life || s._chooser != r->cancellable) return;
                if (r->collection != s._workspace->active_id()) { s.status("Collection changed while choosing a file; no operation performed"); return; }
                switch (r->operation) {
                    case LibraryFileOperation::Import: s._workspace->import_files(paths); break;
                    case LibraryFileOperation::Open: s._workspace->open(paths.front()); break;
                    case LibraryFileOperation::Recover: s._workspace->recover(paths.front()); break;
                    case LibraryFileOperation::Export: s._workspace->export_svg(r->asset, paths.front()); break;
                    case LibraryFileOperation::SaveAs: s._workspace->save(paths.front()); break;
                    case LibraryFileOperation::FindRecoveryFolder: s._workspace->scan_recovery(paths.front()); break;
                }
            });
        } catch (std::exception const &e) { safe(r->life, [&](auto &s) { s.status(e.what()); }); }
    };
    _chooser = cancellable.release();
    if (kind == LibraryFileOperation::FindRecoveryFolder)
        gtk_file_dialog_select_folder(chooser->gobj(), parent ? parent->gobj() : nullptr, _chooser, ready, request.release());
    else if (kind == LibraryFileOperation::Import) gtk_file_dialog_open_multiple(chooser->gobj(), parent ? parent->gobj() : nullptr, _chooser, ready, request.release());
    else if (kind == LibraryFileOperation::SaveAs || kind == LibraryFileOperation::Export)
        gtk_file_dialog_save(chooser->gobj(), parent ? parent->gobj() : nullptr, _chooser, ready, request.release());
    else gtk_file_dialog_open(chooser->gobj(), parent ? parent->gobj() : nullptr, _chooser, ready, request.release());
    if (*life) enable();
}
void ArtworkLibraryController::add_selection() {
    if (getDesktop() && Bitmap::publicationBoundaryPending(getDesktop()->getDocument())) return;
    if (_workspace->closing() || _workspace->busy() || !getDesktop() || !getDocument() || !getSelection() || getSelection()->isEmpty()) return;
    auto life = _alive; auto document = getDocument(); auto desktop = getDesktop(); auto operation = DocumentUndo::holdInteractionOperation(document);
    ObjectSet captured(document); captured.setList(getSelection()->items_vector()); auto count = captured.size();
    auto title = ask("Artwork name", "Artwork");
    if (!*life || !title || getDocument() != document || getDesktop() != desktop || DocumentUndo::interactionCloseRequested(document)) return;
    document->ensureUpToDate(); if (!*life || getDocument() != document || getDesktop() != desktop) return;
    if (captured.size() != count) { status("Selected objects were removed while naming the artwork"); return; }
    auto staged = Art::stage_selection(captured, {}, [&] { return DocumentUndo::interactionCloseRequested(document); });
    if (!*life) return;
    std::string warnings; for (auto const &w : staged.warnings) warnings += w + "\n";
    _workspace->add({"", *title, {}, staged.width_mm, staged.height_mm}, std::move(staged.svg));
    if (*life && !warnings.empty()) status(warnings);
}
void ArtworkLibraryController::insert() {
    if (getDesktop() && Bitmap::publicationBoundaryPending(getDesktop()->getDocument())) return;
    if (_workspace->closing() || _workspace->busy() || _prompting || _chooser || !getDesktop()) return;
    auto token = selected_svg(); if (!token) { status("Wait for this artwork's admission, or inspect its diagnostic"); return; }
    auto life = _alive; auto desktop = getDesktop();
    auto center = desktop->dt2doc(desktop->current_center());
    auto origin = center - Geom::Point(token->width_mm(), token->height_mm()) * (96. / 25.4 / 2.);
    auto result = place_library_artwork(*desktop, *token, origin);
    if (!*life) return; status("Independent editable copy inserted (one Undo)"); if (!*life) return;
    for (auto const &w : result.warnings) { status(w); if (!*life) return; }
}
void ArtworkLibraryController::invoke(std::string const &action) {
    if (getDesktop() && Bitmap::publicationBoundaryPending(getDesktop()->getDocument())) return;
    auto life = _alive;
    if (_workspace->closing()) return;
    try {
        if (action == "new") { auto name = ask("New collection", "Artwork Library"); if (*life && name) _workspace->new_collection(*name); }
        else if (action == "open" || action == "import" || action == "save-as" || action == "export" || action == "recover" || action == "find-recovery") choose(action);
        else if (action == "save") { if (_workspace->active() && _workspace->active()->path.empty()) choose("save-as"); else _workspace->save(); }
        else if (action == "insert") insert();
        else if (action == "add-selection") add_selection();
        else if (action == "cancel") _workspace->cancel();
        else if (action == "review-import") review_import();
        else if (action == "review-recovery") review_recovery();
        else if (action == "remove-lock") _workspace->inspect_lock();
        else if (action == "remove") _workspace->remove(selected_id());
        else if (action == "restore") { auto id = choose_removed(); if (*life && id) _workspace->restore(*id); }
        else if (action == "rename" || action == "tags") {
            auto id = selected_id(); if (id.empty()) return; auto a = _workspace->active()->catalog.snapshot().asset(id);
            std::string initial = a.name;
            if (action == "tags") { initial.clear(); for (auto const &t : a.tags) { if (!initial.empty()) initial += '\n'; initial += t; } }
            auto value = ask(action == "tags" ? "Tags (one per line)" : "Artwork name", initial, action == "tags");
            if (!*life || !value) return;
            if (action == "rename") _workspace->rename(id, *value);
            else {
                std::vector<std::string> tags; std::istringstream in(*value); std::string tag;
                while (std::getline(in, tag)) {
                    tag = trim_line(std::move(tag));
                    if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
                }
                _workspace->tags(id, tags);
            }
        } else if (action == "unload") {
            auto c = _workspace->active(); if (!c) return;
            if (!c->dirty() && !c->uncertain) { _workspace->unload(); return; }
            // Copy everything before the modal prompt; the collection may change.
            auto collection = c->identity;
            std::string detail = "Unload does not delete files.";
            if (!c->recovery_paths.empty()) {
                detail += "\nRetained recovery paths:";
                for (auto const &p : c->recovery_paths) detail += "\n" + p;
            }
            auto response = confirm("Unsaved library changes", detail, true);
            if (!*life) return;
            if (response == int(Gtk::ResponseType::APPLY)) {
                _unload_after_save = collection;
                invoke("save");
                if (!*life) return;
                // Nothing started (error already shown): do not unload later.
                if (!_workspace->busy() && !_chooser) _unload_after_save.clear();
            } else if (response == int(Gtk::ResponseType::ACCEPT)) _workspace->unload(true);
        }
    } catch (std::exception const &e) { if (*life) status(e.what()); }
}
std::optional<std::string> ArtworkLibraryController::choose_removed() {
    if (getDesktop() && Bitmap::publicationBoundaryPending(getDesktop()->getDocument())) return {};
    if (!_workspace->active()) return {};
    auto life = _alive; auto collection = _workspace->active_id(); auto c = *_workspace->active();
    auto ids = c.catalog.snapshot().removed_ids(); if (ids.empty()) return {};
    _prompting = true; enable(); if (!*life) return {}; Gtk::Dialog dialog("Restore removed artwork", true);
    if (auto parent = dynamic_cast<Gtk::Window *>(get_root())) dialog.set_transient_for(*parent);
    Gtk::ComboBoxText choice;
    for (auto const &id : ids) { auto it = c.removed_names.find(id); choice.append(id, it == c.removed_names.end() ? id : it->second); }
    choice.set_active(0); dialog.get_content_area()->append(choice);
    dialog.add_button("Cancel", int(Gtk::ResponseType::CANCEL)); dialog.add_button("Restore", int(Gtk::ResponseType::OK));
    auto response = UI::dialog_run(dialog); auto id = choice.get_active_id().raw(); dialog.get_content_area()->remove(choice);
    if (!*life) return {}; _prompting = false; enable(); if (!*life) return {};
    if (response != int(Gtk::ResponseType::OK) || collection != _workspace->active_id()) return {}; return id;
}
}
