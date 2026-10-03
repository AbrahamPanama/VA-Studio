// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief A dialog for the start screen
 */
/*
 * Copyright (C) Martin Owens 2020 <doctormo@gmail.com>
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef STARTSCREEN_H
#define STARTSCREEN_H

#include <memory>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <map>

#include <gdkmm/enums.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtkmm/box.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include "preferences.h"
#include "ui/widget/template-list.h"

namespace Gdk { class Surface; }
namespace Gio { class File; class Cancellable; template <typename T> class ListStore; }
typedef struct _GCancellable GCancellable;
namespace Gtk {
class BoolFilter;
class Builder;
class Button;
class CheckButton;
class DropDown;
class FilterListModel;
class GridView;
class Label;
class ListItem;
class Image;
class SearchEntry2;
class SignalListItemFactory;
class Picture;
class SingleSelection;
class Stack;
class StringList;
class ToggleButton;
} // namespace Gtk

class SPDocument;

namespace Inkscape::UI::Dialog {

class RecentCardItem;
struct RecentPreviewDemand;
}
namespace Inkscape::UI::Cache { class WelcomeThumbnailService; }
namespace Inkscape::UI::Dialog {

struct WelcomeSize { int width; int height; };
WelcomeSize welcome_default_size(WelcomeSize area, WelcomeSize minimum);


class StartScreen : public Gtk::Window
{
public:
    StartScreen();
    ~StartScreen() override;

    static int get_start_mode();

    /// Completion shared by the GTK chooser and lifecycle tests. It is safe
    /// to invoke after this window has been destroyed.
    using OpenCompletion = std::function<void(Glib::RefPtr<Gio::File>, std::string, std::string)>;
    OpenCompletion make_open_completion();
    std::function<void(SPDocument *)> make_template_completion();

    // Deterministic probe injection for the existing GUI test target. Completion
    // must run on the GTK main context, like GFile's async completion.
    enum class RecentProbeResult { Available, Missing, Unreachable, PermissionDenied, MetadataReady, Busy };
    using RecentProbe = std::function<void(std::string const &, std::function<void(RecentProbeResult)>)>;
    void set_recent_probe_for_test(RecentProbe probe);
    void set_recent_passive_probe_for_test(RecentProbe probe);
    unsigned recent_retry_used_for_test(std::string const &uri) const;
    std::size_t recent_preview_renders_for_test() const;
    std::size_t recent_preview_demands_for_test() const { return _recent_previews.size(); }
    bool recent_preview_finished_for_test(std::string const &uri) const;
    bool recent_scroll_connected_for_test() const { return _recent_scroll_handler != 0; }
    void set_recent_preview_cancel_for_test(std::function<void(std::string const &)> callback)
    { _recent_preview_cancel_for_test = std::move(callback); }
    void set_recent_metadata_for_test(std::function<void()> callback) { _recent_metadata_for_test = std::move(callback); }
    void set_recent_root_for_test(std::string root) { _recent_root_for_test = std::move(root); }
    void set_recent_open_for_test(std::function<void(Glib::RefPtr<Gio::File>)> observer) { _recent_open_for_test = std::move(observer); }
    void activate_recent_for_test(std::string const &uri);
    RecentProbeResult recent_result_for_test() const;
    bool recent_pending_for_test() const { return _recent_cancellable != nullptr; }
    void remove_recent_for_test();
    std::size_t recent_bind_count_for_test() const { return _recent_bind_count; }
    std::size_t recent_unbind_count_for_test() const { return _recent_unbind_count; }
    std::size_t recent_last_unbind_live_connections_for_test() const { return _recent_last_unbind_live_connections; }
    bool recent_old_unbind_clean_for_test() const { return _recent_old_unbind_clean_for_test; }
    bool recent_reused_binding_for_test() const { return _recent_reused_binding_for_test; }
    void emit_replaced_recent_changed_for_test();
    std::size_t recent_refresh_count_for_test() const { return _recent_refresh_count; }
    std::size_t recent_announcement_count_for_test() const { return _recent_announcement_count; }
    void refresh_recent_for_test() { refresh_recent_model(); }
    Gtk::Button *recent_action_for_test(std::string const &uri, bool retry) const;
    guint recent_position_for_test(std::string const &uri) const;
    bool press_key_for_test(unsigned keyval, Gdk::ModifierType state)
    { return on_key_pressed(keyval, 0, state); }
    void search_recent_for_test(Glib::ustring const &text);
    void set_recovery_only_for_test(bool active);
    bool recovery_only_for_test() const { return _recovery_only; }
    void replace_recent_for_test(std::string const &old_uri, std::string const &new_uri);
    bool chooser_pending_for_test() const { return _open_guard->chooser_pending; }
    void cancel_chooser_for_test();
    void set_chooser_completed_for_test(std::function<void()> observer) { _open_guard->chooser_completed = std::move(observer); }

    /// The open signal is emitted when the user opens a document.
    /// If the document is null, a default new document should be opened.
    sigc::connection connectOpen(sigc::slot<void (SPDocument *)> &&slot) { return _signal_open.connect(std::move(slot)); }

    /// Welcome waits to close until the opened document window is on screen. From now on
    /// it ignores every entry point (keys, Recent, Browse, New, templates).
    void begin_closing();
    bool closing() const { return _closing; }

private:
    bool _closing = false;
    void build_recent_grid();
    void begin_recent_preview(Glib::RefPtr<RecentCardItem> const &item,
                              Gtk::ListItem *binding, Gtk::Picture *image);
    void cancel_recent_preview(Gtk::ListItem *binding);
    void refresh_recent_preview_visibility();
    void populate_recent_model();
    void update_recent_state();
    void refilter_recent();
    bool recent_item_matches(RecentCardItem const &item) const;
    void set_active_nav(char const *page);
    void update_compact_nav();
    void on_window_realized();
    void on_search_changed();
    void on_recovery_toggled();

    void new_document();
    void create_from_template();
    void open_document();
    void open_uri(Glib::ustring const &uri);
    void begin_recent_check(Glib::RefPtr<RecentCardItem> const &item);
    void finish_recent_check(std::uint64_t generation, Glib::RefPtr<RecentCardItem> const &item,
                             Glib::RefPtr<Gio::File> const &file, RecentProbeResult result);
    void cancel_recent_check();
    void restore_busy_recent_cards();
    void remove_recent(Glib::RefPtr<RecentCardItem> const &item);
    void refresh_recent_model();
    void open_file(Glib::RefPtr<Gio::File> const &file);
    void show_preferences();
    bool on_key_pressed(unsigned keyval, unsigned keycode, Gdk::ModifierType state);

    void _finish(SPDocument *document);
    void update_logo();

    Glib::RefPtr<Gio::File> _logo_file;
    sigc::scoped_connection _logo_scale_conn;

    Glib::RefPtr<Gtk::Builder> build_welcome;
    Gtk::Box &_root;
    Gtk::Box &_sidebar;
    Gtk::Box &_content;
    Gtk::Stack &_content_stack;
    Gtk::Box &_header;
    Gtk::Box &_search_row;
    Gtk::Label &_title_label;
    Gtk::Label &_subtitle_label;
    Gtk::Box &_template_nav_holder;
    Gtk::Box &_template_list_holder;
    Gtk::Button &_new_btn;
    Gtk::Button &_open_btn;
    Gtk::Button &_recent_nav;
    Gtk::Button &_templates_nav;
    Gtk::Button &_prefs_btn;
    Gtk::Button &_about_btn;
    Gtk::Button &_template_create_btn;
    Gtk::CheckButton &_show_toggle;
    Gtk::Label &_version_label;
    Gtk::Label &_brand_label;
    Gtk::Box &_footer;
    Gtk::Image &_logo;
    Gtk::SearchEntry2 &_search;
    Gtk::ToggleButton &_recovery_toggle;
    Gtk::Stack &_recent_state;
    Gtk::GridView &_recent_grid;

    Inkscape::UI::Widget::TemplateList templates;
    std::vector<Gtk::Label *> _nav_labels;

    Glib::RefPtr<Gio::ListStore<RecentCardItem>> _recent_store;
    Glib::RefPtr<Gtk::BoolFilter> _recent_filter;
    Glib::RefPtr<Gtk::FilterListModel> _recent_filter_model;
    Glib::RefPtr<Gtk::SingleSelection> _recent_selection;
    Glib::RefPtr<Gtk::SignalListItemFactory> _recent_factory;
    std::map<Gtk::ListItem *, std::vector<sigc::connection>> _recent_bindings;
    std::map<Gtk::ListItem *, std::shared_ptr<RecentPreviewDemand>> _recent_previews;
    std::unique_ptr<Inkscape::UI::Cache::WelcomeThumbnailService> _thumbnail_service;
    GtkAdjustment *_recent_scroll_adjustment = nullptr;
    gulong _recent_scroll_handler = 0;
    std::size_t _recent_bind_count = 0;
    std::size_t _recent_unbind_count = 0;
    std::size_t _recent_last_unbind_live_connections = 0;
    Glib::ObjectBase *_recent_last_unbound_item_for_test = nullptr;
    Glib::RefPtr<RecentCardItem> _recent_replaced_item_for_test;
    bool _recent_old_unbind_clean_for_test = false;
    bool _recent_reused_binding_for_test = false;
    std::size_t _recent_refresh_count = 0;
    std::size_t _recent_announcement_count = 0;
    GCancellable *_recent_cancellable = nullptr;
    sigc::connection _recent_timeout;
    std::uint64_t _recent_generation = 0;
    std::string _pending_recent_uri;
    Glib::RefPtr<RecentCardItem> _pending_recent_item;
    Glib::RefPtr<RecentCardItem> _test_recent_item;
    RecentProbe _recent_probe;
    RecentProbe _recent_passive_probe;
    bool _suppress_passive_for_test = false;
    std::function<void(std::string const &)> _recent_preview_cancel_for_test;
    std::function<void()> _recent_metadata_for_test;
    std::string _recent_root_for_test;
    std::function<void(Glib::RefPtr<Gio::File>)> _recent_open_for_test;

    std::string _search_folded;
    bool _recovery_only = false;
    bool _has_recovery = false;
    bool _compact = false;
    // Lives through async chooser and nested importer/template dialogs.
    struct OpenGuardState {
        bool alive = true;
        bool in_flight = false;
        bool chooser_pending = false;
        bool chooser_cancelled = false;
        Glib::RefPtr<Gio::Cancellable> chooser_cancellable;
        std::function<void()> chooser_completed;
    };
    std::shared_ptr<OpenGuardState> _open_guard = std::make_shared<OpenGuardState>();
    std::unique_ptr<Inkscape::Preferences::PreferencesObserver> _boot_mode_observer;
    Glib::RefPtr<Gdk::Surface> _surface;

    sigc::scoped_connection _surface_width_conn;
    sigc::scoped_connection _show_toggle_conn;
    sigc::scoped_connection _search_conn;
    sigc::scoped_connection _recovery_conn;
    sigc::scoped_connection _activate_conn;
    sigc::scoped_connection _template_selected_conn;
    sigc::scoped_connection _template_activated_conn;
    sigc::scoped_connection _category_conn;
    sigc::scoped_connection _realize_conn;

    sigc::signal<void (SPDocument *)> _signal_open;
};

} // namespace Inkscape::UI::Dialog

#endif // STARTSCREEN_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
