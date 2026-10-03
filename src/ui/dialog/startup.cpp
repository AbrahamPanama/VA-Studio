// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief A dialog for the start screen
 */
/*
 * Copyright (C) Martin Owens 2019 <doctormo@gmail.com>
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "startup.h"

#include "config.h" // only include where actually required!

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtk/gtk.h> // gtk_accessible_update_property (native a11y, no new dependency)

#include <gio/gio.h> // g_content_type_get_symbolic_icon (public C API, all platforms)

#include <gdkmm/surface.h>
#include <giomm/contenttype.h>
#include <giomm/cancellable.h>
#include <giomm/file.h>
#include <giomm/icon.h>
#include <giomm/liststore.h>
#include <glibmm/datetime.h>
#include <glibmm/i18n.h>
#include <gtkmm/aspectframe.h>
#include <gtkmm/boolfilter.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/expression.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/filterlistmodel.h>
#include <gtkmm/gridview.h>
#include <gtkmm/iconpaintable.h>
#include <gtkmm/image.h>
#include <gtkmm/picture.h>
#include <gtkmm/label.h>
#include <gtkmm/listitem.h>
#include <gtkmm/recentmanager.h>
#include <gtkmm/native.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/searchentry2.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/stack.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/togglebutton.h>
#include <glibmm/main.h>

#include "inkscape-application.h"
#include "actions/actions-file-window.h"
#include "inkscape-version-info.h"
#include "inkscape.h"
#include "io/resource.h"
#include "preferences.h"
#include "ui/builder-utils.h"
#include "ui/dialog/about.h"
#include "ui/dialog/choose-file-utils.h"
#include "ui/dialog/choose-file.h"
#include "ui/dialog/welcome-recent-model.h"
#include "ui/cache/welcome-drawing-preview.h"
#include "ui/widget/template-list.h"
#include "util/scope_exit.h"

using namespace Inkscape::IO;

namespace Inkscape::UI::Dialog {

namespace Recent = Inkscape::UI::Dialog::Welcome;
using Inkscape::UI::Widget::TemplateList;

namespace {

Glib::ustring format_last_used(Glib::DateTime const &when)
{
    // Localized display of the recent-manager last-use metadata. Never presented as
    // a document modification time, and formatted once at model build time. W2 records
    // may carry no metadata; guard the invalid Glib::DateTime before calling format()
    // to avoid a GLib critical.
    if (!when) {
        return Glib::ustring(_("Last used")) + Glib::ustring(": ") + Glib::ustring(_("Unknown"));
    }
    return Glib::ustring(_("Last used")) + Glib::ustring(": ")
        + when.to_local().format(Glib::ustring("%x %X"));
}

void set_accessible_label(Gtk::Widget &widget, char const *label)
{
    // Explicit accessible name so labels hidden in the compact rail are still
    // announced to assistive technology. Pinned GTK4 native API; no new dependency.
    gtk_accessible_update_property(GTK_ACCESSIBLE(widget.gobj()),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                   -1);
}

// GTK4 on macOS has no work area; reserve 24 px for the menu bar and 80 px for the Dock.
constexpr int mac_system_ui_allowance = 24 + 80;

void configure_window_size(Gtk::Window &window, Gtk::Widget &content)
{
    auto *display = gtk_widget_get_display(GTK_WIDGET(window.gobj()));
    if (!display) return;
    GdkMonitor *monitor = nullptr;
    if (auto *seat = gdk_display_get_default_seat(display)) {
        if (auto *pointer = gdk_seat_get_pointer(seat)) {
            if (auto *surface = gdk_device_get_surface_at_position(pointer, nullptr, nullptr)) {
                monitor = gdk_display_get_monitor_at_surface(display, surface);
            }
        }
    }
    if (!monitor && GTK_IS_APPLICATION(g_application_get_default())) {
        if (auto *active = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()))) {
            if (auto *surface = gtk_native_get_surface(GTK_NATIVE(active))) {
                monitor = gdk_display_get_monitor_at_surface(display, surface);
            }
        }
    }
    auto *monitors = gdk_display_get_monitors(display);
    bool const owned = !monitor && g_list_model_get_n_items(monitors) > 0;
    if (owned) monitor = GDK_MONITOR(g_list_model_get_item(monitors, 0));
    if (!monitor) return;
    GdkRectangle geometry{};
    gdk_monitor_get_geometry(monitor, &geometry);
    if (owned) g_object_unref(monitor);
    int min_width = 0, min_height = 0;
    gtk_widget_measure(GTK_WIDGET(content.gobj()), GTK_ORIENTATION_HORIZONTAL, -1,
                       &min_width, nullptr, nullptr, nullptr);
    gtk_widget_measure(GTK_WIDGET(content.gobj()), GTK_ORIENTATION_VERTICAL,
                       std::min(geometry.width, std::max(1, min_width)),
                       &min_height, nullptr, nullptr, nullptr);
#ifdef __APPLE__
    geometry.height = std::max(1, geometry.height - mac_system_ui_allowance);
#endif
    auto const size = welcome_default_size({geometry.width, geometry.height}, {min_width, min_height});
    window.set_default_size(size.width, size.height);
}

// Passive slots expire at the UI deadline. Tokens keep late backend completion
// from removing a newer probe for the same URI. Main-context only.
std::set<std::string> &recent_work_in_flight()
{
    static std::set<std::string> work;
    return work;
}
std::map<std::string, unsigned> &recent_passive_in_flight()
{
    static std::map<std::string, unsigned> work;
    return work;
}
unsigned recent_passive_token = 0;

// One UI session can create several StartScreen windows. Keep only URI and
// retry state here; no window or widget is retained.
struct RecentRetry { unsigned used = 0; bool unreachable = false; };
std::map<std::string, RecentRetry> &recent_retry_budget()
{
    static std::map<std::string, RecentRetry> budget;
    return budget;
}

std::string mount_root_for(std::string const &path)
{
#ifdef __APPLE__
    constexpr std::string_view prefix = "/Volumes/";
    if (path.compare(0, prefix.size(), prefix) == 0) {
        auto const slash = path.find('/', prefix.size());
        return path.substr(0, slash);
    }
    constexpr std::string_view autofs = "/net/";
    if (path.compare(0, autofs.size(), autofs) == 0) {
        auto const slash = path.find('/', autofs.size());
        return path.substr(0, slash);
    }
#endif
#ifdef _WIN32
    if (path.size() >= 3 && (path[0] == '\\' || path[0] == '/') &&
        (path[1] == '\\' || path[1] == '/')) {
        auto const server_end = path.find_first_of("\\/", 2);
        if (server_end != std::string::npos) {
            auto const share_end = path.find_first_of("\\/", server_end + 1);
            return path.substr(0, share_end);
        }
    }
    if (path.size() >= 2 && g_ascii_isalpha(path[0]) && path[1] == ':') {
        return path.substr(0, 2) + "\\";
    }
#endif
    return {};
}

struct RecentCheckWork {
    using Result = StartScreen::RecentProbeResult;
    std::string uri;
    std::string root;
    std::string root_filesystem;
    GFile *file = nullptr;
    GCancellable *cancellable = nullptr;
    GInputStream *stream = nullptr;
    char byte = 0;
    std::function<void()> metadata_ready;
    std::function<void(Result)> deliver;
    bool passive = false;
    unsigned passive_token = 0;

    ~RecentCheckWork()
    {
        if (stream) g_object_unref(stream);
        if (file) g_object_unref(file);
        if (cancellable) g_object_unref(cancellable);
    }
    static Result classify(GError const *error)
    {
        if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) return Result::PermissionDenied;
        if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) return Result::Missing;
        return Result::Unreachable;
    }
    void finish(Result result)
    {
        if (passive) {
            auto &work = recent_passive_in_flight();
            if (auto it = work.find(uri); it != work.end() && it->second == passive_token) work.erase(it);
        } else recent_work_in_flight().erase(uri);
        deliver(result);
        delete this;
    }
    static void root_done(GObject *source, GAsyncResult *result, gpointer data)
    {
        auto *work = static_cast<RecentCheckWork *>(data);
        GError *error = nullptr;
        auto *info = g_file_query_info_finish(G_FILE(source), result, &error);
        if (info) {
            auto const *id = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
            if (id) work->root_filesystem = id;
            g_object_unref(info);
        }
        if (error) g_error_free(error);
        if (!info) return work->finish(Result::Unreachable);
        auto *parent = g_file_get_parent(G_FILE(source));
        if (!parent) return work->finish(Result::Missing);
        g_file_query_info_async(parent, G_FILE_ATTRIBUTE_ID_FILESYSTEM,
                                G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, work->cancellable,
                                [](GObject *source, GAsyncResult *result, gpointer data) {
                                    auto *work = static_cast<RecentCheckWork *>(data);
                                    GError *error = nullptr;
                                    auto *info = g_file_query_info_finish(G_FILE(source), result, &error);
                                    auto const *parent_id = info ? g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM) : nullptr;
                                    bool const mounted = parent_id && !work->root_filesystem.empty() && work->root_filesystem != parent_id;
                                    if (info) g_object_unref(info);
                                    if (error) g_error_free(error);
                                    work->finish(mounted ? Result::Missing : Result::Unreachable);
                                }, work);
        g_object_unref(parent);
    }
    void missing_or_root()
    {
        if (root.empty()) return finish(Result::Missing);
        auto *root_file = g_file_new_for_path(root.c_str());
        g_file_query_info_async(root_file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_ID_FILESYSTEM,
                                G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, cancellable,
                                root_done, this);
        g_object_unref(root_file);
    }
    static void close_done(GObject *source, GAsyncResult *result, gpointer data)
    {
        auto *work = static_cast<RecentCheckWork *>(data);
        GError *error = nullptr;
        g_input_stream_close_finish(G_INPUT_STREAM(source), result, &error);
        if (error) g_error_free(error);
        work->finish(Result::Available);
    }
    static void read_done(GObject *source, GAsyncResult *result, gpointer data)
    {
        auto *work = static_cast<RecentCheckWork *>(data);
        GError *error = nullptr;
        auto const bytes = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);
        if (bytes >= 0) {
            if (error) g_error_free(error);
            g_input_stream_close_async(work->stream, G_PRIORITY_DEFAULT, nullptr, close_done, work);
        } else {
            auto const kind = classify(error);
            if (error) g_error_free(error);
            if (kind == Result::Missing) work->missing_or_root();
            else work->finish(kind);
        }
    }
    static void opened(GObject *source, GAsyncResult *result, gpointer data)
    {
        auto *work = static_cast<RecentCheckWork *>(data);
        GError *error = nullptr;
        work->stream = G_INPUT_STREAM(g_file_read_finish(G_FILE(source), result, &error));
        if (work->stream) {
            g_input_stream_read_async(work->stream, &work->byte, 1, G_PRIORITY_DEFAULT,
                                      work->cancellable, read_done, work);
        } else {
            auto const kind = classify(error);
            if (kind == Result::Missing) work->missing_or_root();
            else work->finish(kind);
        }
        if (error) g_error_free(error);
    }
    static void queried(GObject *source, GAsyncResult *result, gpointer data)
    {
        auto *work = static_cast<RecentCheckWork *>(data);
        GError *error = nullptr;
        auto *info = g_file_query_info_finish(G_FILE(source), result, &error);
        if (info) {
            g_object_unref(info);
            if (work->metadata_ready) work->metadata_ready();
            if (work->passive) work->finish(Result::Available);
            else g_file_read_async(work->file, G_PRIORITY_DEFAULT, work->cancellable, opened, work);
        } else {
            auto const kind = classify(error);
            if (kind == Result::Missing) work->missing_or_root();
            else work->finish(kind);
        }
        if (error) g_error_free(error);
    }
};

} // namespace

WelcomeSize welcome_default_size(WelcomeSize area, WelcomeSize minimum)
{
    // Preferred size is 1180x800, shrunk to 85% of the area on smaller screens.
    auto dimension = [](int available, int required, int preferred) {
        if (available <= 0) return 1;
        return std::clamp(std::min(preferred, static_cast<int>(std::floor(available * 0.85))),
                          std::min(std::max(1, required), available), available);
    };
    return {dimension(area.width, minimum.width, 1180), dimension(area.height, minimum.height, 800)};
}

/**
 * Thin immutable wrapper around one W2 recent-history snapshot record. All display
 * strings are precomputed once at model build time: no I/O, locale query or document
 * access ever happens in the factory bind path.
 */
class RecentCardItem final : public Glib::Object
{
public:
    enum class State { Ready, Checking, Missing, Unreachable, PermissionDenied, NonLocal, Busy };
    static Glib::RefPtr<RecentCardItem> create(Recent::RecentFileRecord record)
    {
        return Glib::make_refptr_for_instance<RecentCardItem>(new RecentCardItem(std::move(record)));
    }

    Recent::RecentFileRecord const record;
    Glib::ustring const name;
    Glib::ustring const location;
    Glib::ustring const last_used;
    State state = State::Ready;
    State before_busy = State::Ready;
    State before_checking = State::Ready;
    sigc::signal<void()> changed;

    void set_state(State next)
    {
        state = next;
        changed.emit();
    }

private:
    // The incoming record is `value`; it is moved once into the stored `record`
    // member, and every display string is derived from that stored member (not from
    // the moved-from parameter).
    explicit RecentCardItem(Recent::RecentFileRecord value)
        : record(std::move(value))
        , name(record.display_name.empty() ? Glib::ustring(_("Unnamed document")) : Glib::ustring(record.display_name))
        , location(record.display_location.empty() ? Glib::ustring(record.uri) : Glib::ustring(record.display_location))
        , last_used(format_last_used(record.last_used))
    {
    }
};

struct RecentPreviewDemand {
    std::string uri;
    std::string path;
    unsigned width = 0, height = 0;
    GCancellable *cancellable = nullptr;
    sigc::connection timeout;
    bool requested = false;
    bool activate = false;
    bool passive_pending = true;
    bool timed_out = false;
    bool limits = false;
    bool finished = false;
    ~RecentPreviewDemand()
    {
        timeout.disconnect();
        if (cancellable) {
            g_cancellable_cancel(cancellable);
            g_object_unref(cancellable);
        }
    }
};

StartScreen::StartScreen()
    : build_welcome(create_builder("inkscape-welcome.glade"))
    , _root(get_widget<Gtk::Box>(build_welcome, "welcome-root-box"))
    , _sidebar(get_widget<Gtk::Box>(build_welcome, "welcome-sidebar"))
    , _content(get_widget<Gtk::Box>(build_welcome, "welcome-content"))
    , _content_stack(get_widget<Gtk::Stack>(build_welcome, "welcome-content-stack"))
    , _header(get_widget<Gtk::Box>(build_welcome, "welcome-header"))
    , _search_row(get_widget<Gtk::Box>(build_welcome, "welcome-search-row"))
    , _title_label(get_widget<Gtk::Label>(build_welcome, "welcome-title"))
    , _subtitle_label(get_widget<Gtk::Label>(build_welcome, "welcome-subtitle"))
    , _template_nav_holder(get_widget<Gtk::Box>(build_welcome, "template-nav-holder"))
    , _template_list_holder(get_widget<Gtk::Box>(build_welcome, "template-list-holder"))
    , _new_btn(get_widget<Gtk::Button>(build_welcome, "welcome-new-button"))
    , _open_btn(get_widget<Gtk::Button>(build_welcome, "welcome-open-button"))
    , _recent_nav(get_widget<Gtk::Button>(build_welcome, "welcome-recent-button"))
    , _templates_nav(get_widget<Gtk::Button>(build_welcome, "welcome-templates-button"))
    , _prefs_btn(get_widget<Gtk::Button>(build_welcome, "welcome-preferences-button"))
    , _about_btn(get_widget<Gtk::Button>(build_welcome, "welcome-about-button"))
    , _template_create_btn(get_widget<Gtk::Button>(build_welcome, "welcome-template-create"))
    , _show_toggle(get_widget<Gtk::CheckButton>(build_welcome, "welcome-show-toggle"))
    , _version_label(get_widget<Gtk::Label>(build_welcome, "welcome-version-label"))
    , _brand_label(get_widget<Gtk::Label>(build_welcome, "welcome-brand-label"))
    , _footer(get_widget<Gtk::Box>(build_welcome, "welcome-footer"))
    , _logo(get_widget<Gtk::Image>(build_welcome, "welcome-logo"))
    , _search(get_widget<Gtk::SearchEntry2>(build_welcome, "welcome-search"))
    , _recovery_toggle(get_widget<Gtk::ToggleButton>(build_welcome, "welcome-recovery-toggle"))
    , _recent_state(get_widget<Gtk::Stack>(build_welcome, "welcome-recent-state"))
    , _recent_grid(get_widget<Gtk::GridView>(build_welcome, "welcome-recent-grid"))
{
    _thumbnail_service = std::make_unique<Inkscape::UI::Cache::WelcomeThumbnailService>();
    set_name("start-screen-window");
    set_title(VACARDS_PRODUCT_NAME);
    set_focusable(true);
    set_modal(false); // native non-modal window with platform decorations

    set_child(_root);

    _nav_labels = {
        &get_widget<Gtk::Label>(build_welcome, "welcome-new-label"),
        &get_widget<Gtk::Label>(build_welcome, "welcome-open-label"),
        &get_widget<Gtk::Label>(build_welcome, "welcome-recent-label"),
        &get_widget<Gtk::Label>(build_welcome, "welcome-templates-label"),
        &get_widget<Gtk::Label>(build_welcome, "welcome-preferences-label"),
        &get_widget<Gtk::Label>(build_welcome, "welcome-about-label"),
    };

    // Explicit accessible names for the actions whose visible labels collapse to an
    // icon rail at narrow widths.
    set_accessible_label(_new_btn, _("New Document"));
    set_accessible_label(_open_btn, _("Open File"));
    set_accessible_label(_recent_nav, _("Recent"));
    set_accessible_label(_templates_nav, _("Templates"));
    set_accessible_label(_prefs_btn, _("Preferences"));
    set_accessible_label(_about_btn, _("About"));
    set_accessible_label(_search, _("Search recent documents"));

    // The compact rail hides these labels visually, but keeps their mnemonics.
    auto mnemonic = [](Gtk::Label &label, Gtk::Button &button, char const *title) {
        label.set_use_underline(true);
        label.set_label(title);
        label.set_mnemonic_widget(button);
    };
    mnemonic(*_nav_labels[0], _new_btn, _("_New Document"));
    mnemonic(*_nav_labels[1], _open_btn, _("_Open File"));
    mnemonic(*_nav_labels[2], _recent_nav, _("_Recent"));
    mnemonic(*_nav_labels[3], _templates_nav, _("_Templates"));
    mnemonic(*_nav_labels[4], _prefs_btn, _("_Preferences"));
    mnemonic(*_nav_labels[5], _about_btn, _("_About"));

    // Shared product logo. Root installs the common PNG resource on all platforms now.
    // The native IconPaintable factory renders it at an explicit logical size (96/36) so
    // the asset never drives the layout. If it is absent the picture stays blank; the
    // oversized About artwork is not a substitute (missing-asset runtime gate).
    auto const logo_file = Resource::get_filename(Resource::ICONS, "com.vacards.Inkscape.png", false, true);
    if (!logo_file.empty()) {
        _logo_file = Gio::File::create_for_path(logo_file);
        update_logo();
    }
    // Refresh the paintable when the widget scale factor changes (HiDPI/monitor move).
    // Tracked connection; the native signal does not outlive this window.
    _logo_scale_conn = property_scale_factor().signal_changed().connect(
        sigc::mem_fun(*this, &StartScreen::update_logo));
    _brand_label.set_label(VACARDS_PRODUCT_NAME);
    _version_label.set_label(std::string(VACARDS_PRODUCT_NAME " " VACARDS_PRODUCT_DISPLAY_VERSION));

    // Embed TemplateList whole; pages are never extracted, removed or reparented.
    templates.init(Inkscape::Extension::TEMPLATE_NEW_WELCOME, TemplateList::All, true);
    templates.set_vexpand(true);
    templates.set_hexpand(true);
    _template_list_holder.append(templates);

    // Programmatic category navigation over the native TemplateList API.
    auto const categories = templates.get_categories();
    auto const category_names = Gtk::StringList::create();
    for (auto const &category : categories) {
        category_names->append(templates.get_category_label(category));
    }
    auto *category_dropdown = Gtk::make_managed<Gtk::DropDown>(category_names);
    category_dropdown->set_tooltip_text(_("Choose a template category"));
    set_accessible_label(*category_dropdown, _("Template category"));
    category_dropdown->set_selected(0);
    _template_nav_holder.append(*category_dropdown);
    _category_conn = category_dropdown->property_selected().signal_changed().connect(
        [this, categories, category_dropdown] {
            auto const index = category_dropdown->get_selected();
            if (index < categories.size()) {
                templates.show_page(categories[index]);
                // Category pages have independent selection models; a page switch need
                // not emit selection_changed, so resync Create from the live predicate.
                _template_create_btn.set_sensitive(
                    templates.has_selected_preset() || templates.has_selected_new_template());
            }
        });
    if (!categories.empty()) {
        templates.show_page(categories[0]);
    }

    // Metadata-only recent snapshot from W2. No referenced-document access.
    _recent_store = Gio::ListStore<RecentCardItem>::create();
    build_recent_grid();
    populate_recent_model();

    // Primary actions and navigation.
    _new_btn.signal_clicked().connect(sigc::mem_fun(*this, &StartScreen::new_document));
    _open_btn.signal_clicked().connect(sigc::mem_fun(*this, &StartScreen::open_document));
    _recent_nav.signal_clicked().connect([this] { set_active_nav("recent"); });
    _templates_nav.signal_clicked().connect([this] { set_active_nav("templates"); });
    _prefs_btn.signal_clicked().connect(sigc::mem_fun(*this, &StartScreen::show_preferences));
    _about_btn.signal_clicked().connect([] { Inkscape::UI::Dialog::show_about(); });

    // New/Open stay reachable from the distinct empty and no-match states.
    get_widget<Gtk::Button>(build_welcome, "welcome-empty-new")
        .signal_clicked().connect(sigc::mem_fun(*this, &StartScreen::new_document));
    get_widget<Gtk::Button>(build_welcome, "welcome-empty-open")
        .signal_clicked().connect(sigc::mem_fun(*this, &StartScreen::open_document));
    get_widget<Gtk::Button>(build_welcome, "welcome-no-match-new")
        .signal_clicked().connect(sigc::mem_fun(*this, &StartScreen::new_document));
    get_widget<Gtk::Button>(build_welcome, "welcome-no-match-open")
        .signal_clicked().connect(sigc::mem_fun(*this, &StartScreen::open_document));

    // Templates: selection enables Create only; activation/Create owns document creation.
    _template_create_btn.signal_clicked().connect(sigc::mem_fun(*this, &StartScreen::create_from_template));
    _template_selected_conn = templates.connectItemSelected([this](int) {
        _template_create_btn.set_sensitive(templates.has_selected_preset() || templates.has_selected_new_template());
    });
    _template_activated_conn = templates.connectItemActivated(sigc::mem_fun(*this, &StartScreen::create_from_template));
    _template_create_btn.set_sensitive(templates.has_selected_preset() || templates.has_selected_new_template());

    // Search/recovery filtering reuses the W2 folded Unicode policy.
    _search_conn = _search.signal_search_changed().connect(sigc::mem_fun(*this, &StartScreen::on_search_changed));
    _recovery_conn = _recovery_toggle.signal_toggled().connect(sigc::mem_fun(*this, &StartScreen::on_recovery_toggled));
    _recovery_toggle.set_visible(_has_recovery);

    // Startup checkbox: initialise from the real mode BEFORE connecting the writer.
    _show_toggle.set_active(get_start_mode() != 0);
    _show_toggle_conn = _show_toggle.signal_toggled().connect([this] {
        Inkscape::Preferences::get()->setInt("/options/boot/mode", _show_toggle.get_active() ? 1 : 0);
    });
    _boot_mode_observer = Inkscape::Preferences::get()->createObserver(
        "/options/boot/mode", [this] {
            bool const enabled = get_start_mode() != 0;
            if (_show_toggle.get_active() != enabled) {
                _show_toggle_conn.block();
                auto const unblock = scope_exit([this] { _show_toggle_conn.unblock(); });
                _show_toggle.set_active(enabled);
            }
        });

    // Local key handling runs in the CAPTURE phase so only the explicitly owned
    // accelerators are consumed and everything else reaches the native route.
    auto const key = Gtk::EventControllerKey::create();
    key->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    key->signal_key_pressed().connect(sigc::mem_fun(*this, &StartScreen::on_key_pressed), true);
    add_controller(key);

    _realize_conn = signal_realize().connect(sigc::mem_fun(*this, &StartScreen::on_window_realized));
    configure_window_size(*this, _root);
    set_active_nav("recent");
    show();
}

StartScreen::~StartScreen()
{
    _open_guard->alive = false;
    if (_recent_scroll_handler) g_signal_handler_disconnect(_recent_scroll_adjustment, _recent_scroll_handler);
    if (_open_guard->chooser_pending) {
        _open_guard->chooser_cancelled = true;
        if (_open_guard->chooser_cancellable) _open_guard->chooser_cancellable->cancel();
    }
    cancel_recent_check();
    while (!_recent_previews.empty()) cancel_recent_preview(_recent_previews.begin()->first);
    for (auto &[item, connections] : _recent_bindings) {
        for (auto &connection : connections) connection.disconnect();
    }
    _recent_bindings.clear();
    // GridView emits unbind while releasing its list items. Do this before the
    // binding-connection map is destroyed with the remaining members.
    _recent_grid.set_model({});
    _recent_grid.set_factory({});
}

void StartScreen::build_recent_grid()
{
    _recent_factory = Gtk::SignalListItemFactory::create();

    _recent_factory->signal_setup().connect([](Glib::RefPtr<Gtk::ListItem> const &list_item) {
        auto *box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        box->add_css_class("welcome-card");

        // Real 4:3 preview frame. obey_child=false enforces the ratio; the native
        // vertical placeholder (symbol + visible status label) is the shrinking
        // content. No fixed 180x135 box and no document preview rendering.
        auto *preview = Gtk::make_managed<Gtk::AspectFrame>(
            Gtk::Align::CENTER, Gtk::Align::CENTER, 4.0f / 3.0f, false);
        preview->add_css_class("welcome-card-preview");
        preview->set_size_request(150, -1); // modest minimum, height follows 4:3
        preview->set_hexpand(true);
        auto *placeholder = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        placeholder->add_css_class("welcome-card-placeholder");
        placeholder->set_halign(Gtk::Align::CENTER);
        placeholder->set_valign(Gtk::Align::CENTER);
        placeholder->set_vexpand(true);
        auto *format_icon = Gtk::make_managed<Gtk::Image>();
        format_icon->set_from_icon_name("shape-image-symbolic");
        format_icon->set_icon_size(Gtk::IconSize::LARGE);
        format_icon->set_halign(Gtk::Align::CENTER);
        auto *status = Gtk::make_managed<Gtk::Label>();
        status->add_css_class("welcome-card-preview-status");
        status->set_label(_("Preview unavailable"));
        status->set_wrap(true);
        status->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
        status->set_justify(Gtk::Justification::CENTER);
        status->set_max_width_chars(18); // bounded natural width; never widens the card
        status->set_halign(Gtk::Align::CENTER);
        placeholder->append(*format_icon);
        placeholder->append(*status);
        placeholder->set_tooltip_text(_("Preview unavailable"));
        set_accessible_label(*placeholder, _("Preview unavailable"));
        auto *visual = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        auto *image = Gtk::make_managed<Gtk::Picture>();
        image->set_content_fit(Gtk::ContentFit::CONTAIN);
        image->set_size_request(150, 113);
        image->set_visible(false);
        image->set_hexpand(true);
        image->set_vexpand(true);
        visual->append(*placeholder);
        visual->append(*image);
        preview->set_child(*visual);
        box->append(*preview);

        auto *name = Gtk::make_managed<Gtk::Label>();
        name->add_css_class("welcome-card-name");
        name->set_xalign(0.0f);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        name->set_max_width_chars(28); // cap natural width so long names cannot force a wide window
        box->append(*name);

        auto *last_used = Gtk::make_managed<Gtk::Label>();
        last_used->add_css_class("welcome-card-meta");
        last_used->set_xalign(0.0f);
        last_used->set_ellipsize(Pango::EllipsizeMode::END);
        last_used->set_max_width_chars(28);
        box->append(*last_used);

        auto *badge = Gtk::make_managed<Gtk::Label>();
        badge->add_css_class("welcome-card-badge");
        badge->set_halign(Gtk::Align::START);
        badge->set_visible(false);
        box->append(*badge);

        auto *actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        auto *retry = Gtk::make_managed<Gtk::Button>(_("Retr_y"), true);
        retry->set_visible(false);
        auto *remove = Gtk::make_managed<Gtk::Button>(_("Re_move from Recent"), true);
        actions->append(*retry);
        actions->append(*remove);
        box->append(*actions);

        list_item->set_child(*box);
    });

    _recent_factory->signal_bind().connect([this](Glib::RefPtr<Gtk::ListItem> const &list_item) {
        auto item = std::dynamic_pointer_cast<RecentCardItem>(list_item->get_item());
        auto *box = dynamic_cast<Gtk::Box *>(list_item->get_child());
        if (!item || !box) {
            return;
        }
        ++_recent_bind_count;
        auto *preview = dynamic_cast<Gtk::AspectFrame *>(box->get_first_child());
        auto *visual = preview ? dynamic_cast<Gtk::Box *>(preview->get_child()) : nullptr;
        auto *placeholder = visual ? dynamic_cast<Gtk::Box *>(visual->get_first_child()) : nullptr;
        auto *image = placeholder ? dynamic_cast<Gtk::Picture *>(placeholder->get_next_sibling()) : nullptr;
        if (image) { gtk_picture_set_paintable(GTK_PICTURE(image->gobj()), nullptr); image->set_visible(false); }
        if (placeholder) placeholder->set_visible(true);
        auto *name = preview ? preview->get_next_sibling() : nullptr;
        auto *last_used = name ? name->get_next_sibling() : nullptr;
        auto *badge = last_used ? last_used->get_next_sibling() : nullptr;
        auto *actions = dynamic_cast<Gtk::Box *>(badge ? badge->get_next_sibling() : nullptr);

        // Format icon comes from stored MIME metadata only: no file probe and no
        // content-type guessing. Empty or unusable type keeps the generic symbol.
        if (preview) {
            if (placeholder) {
                if (auto *icon = dynamic_cast<Gtk::Image *>(placeholder->get_first_child())) {
                    bool applied = false;
                    if (!item->record.mime_type.empty()) {
                        // The stored value is MIME, but native content types can
                        // differ (notably on Windows). Translate metadata only; do
                        // not probe or guess from a file.
                        if (auto const content_type = Gio::content_type_from_mime_type(item->record.mime_type);
                            !content_type.empty()) {
                            // giomm's content_type_get_symbolic_icon() is G_OS_UNIX-only,
                            // so call the public C API, which exists on all supported
                            // platforms. It returns a transfer-full GIcon; Glib::wrap
                            // with take_copy = false adopts that single reference.
                            if (auto *raw_icon = g_content_type_get_symbolic_icon(content_type.c_str())) {
                                auto themed = Glib::wrap(raw_icon, false);
                                // Freedesktop MIME icons are not in the bundled themes;
                                // use them only when the theme can draw one.
                                auto *theme = gtk_icon_theme_get_for_display(icon->get_display()->gobj());
                                if (gtk_icon_theme_has_gicon(theme, raw_icon)) {
                                    icon->set(themed);
                                    applied = true;
                                }
                            }
                        }
                    }
                    if (!applied) {
                        icon->set_from_icon_name("shape-image-symbolic");
                    }
                }
            }
        }

        if (auto *label = dynamic_cast<Gtk::Label *>(name)) {
            label->set_label(item->name); // plain text, never markup
        }
        if (auto *label = dynamic_cast<Gtk::Label *>(last_used)) {
            label->set_label(item->last_used);
        }
        if (auto *label = dynamic_cast<Gtk::Label *>(badge)) {
            label->set_label(_("Recovery"));
            label->set_visible(item->record.recovery != Recent::RecoveryKind::None);
        }
        // Location is available without probing the referenced document.
        box->set_tooltip_text(item->location);
        set_accessible_label(*box, (item->name + " — " + item->location).c_str());

        if (actions) {
            auto *retry = dynamic_cast<Gtk::Button *>(actions->get_first_child());
            auto *remove = dynamic_cast<Gtk::Button *>(retry ? retry->get_next_sibling() : nullptr);
            auto update = [this, item, placeholder, image, retry, remove, binding = list_item.get()](bool announce) {
                auto *icon = placeholder ? placeholder->get_first_child() : nullptr;
                auto *label = dynamic_cast<Gtk::Label *>(icon ? icon->get_next_sibling() : nullptr);
                if (!label) return;
                Glib::ustring message;
                auto const shown = item->state == RecentCardItem::State::Busy ? item->before_busy : item->state;
                switch (shown) {
                    case RecentCardItem::State::Ready:
                        message = _recent_previews.count(binding) && _recent_previews.at(binding)->timed_out
                            ? _("Preview timed out") : _("Preview unavailable"); break;
                    case RecentCardItem::State::Checking: message = _("Checking location…"); break;
                    case RecentCardItem::State::Missing:
                        message = Glib::ustring(_("File not found")) + "\n" + item->location; break;
                    case RecentCardItem::State::Unreachable:
                        message = Glib::ustring(_("Location not reachable")) + "\n" + item->location; break;
                    case RecentCardItem::State::PermissionDenied:
                        message = Glib::ustring(_("Permission denied")) + "\n" + item->location; break;
                    case RecentCardItem::State::NonLocal:
                        message = Glib::ustring(_("This location is not a local file")) + "\n" + item->location; break;
                    case RecentCardItem::State::Busy: break;
                }
                if (item->state == RecentCardItem::State::Busy)
                    message = Glib::ustring(recent_work_in_flight().count(item->record.uri)
                        ? _("Still checking this location") : _("Busy – still checking other locations")) + "\n" + message;
                label->set_label(message);
                if (shown != RecentCardItem::State::Ready && image) {
                    gtk_picture_set_paintable(GTK_PICTURE(image->gobj()), nullptr);
                    image->set_visible(false);
                    placeholder->set_visible(true);
                }
                auto tooltip = message;
                if (shown == RecentCardItem::State::Ready && _recent_previews.count(binding) &&
                    _recent_previews.at(binding)->limits)
                    tooltip = _("Preview unavailable: limits exceeded");
                placeholder->set_tooltip_text(tooltip);
                set_accessible_label(*placeholder, message.c_str());
                if (announce) {
                    ++_recent_announcement_count;
                    gtk_accessible_announce(GTK_ACCESSIBLE(placeholder->gobj()),
                                            message.c_str(), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
                }
                if (retry) {
                    auto const it = recent_retry_budget().find(item->record.uri);
                    retry->set_visible(item->state == RecentCardItem::State::Unreachable ||
                        (item->state == RecentCardItem::State::Busy && shown == RecentCardItem::State::Unreachable));
                    bool const exhausted = it != recent_retry_budget().end() && it->second.used >= 2;
                    retry->set_sensitive(!exhausted);
                    retry->set_tooltip_text(exhausted ? _("Retry limit reached for this session") : "");
                }
                auto const action_name = item->name + " — " + item->location;
                if (retry) set_accessible_label(*retry, (Glib::ustring(_("Retry ")) + action_name).c_str());
                if (remove) set_accessible_label(*remove, (Glib::ustring(_("Remove ")) + action_name).c_str());
            };
            auto &connections = _recent_bindings[list_item.get()];
            connections.emplace_back(item->changed.connect([update] { update(true); }));
            if (retry) connections.push_back(retry->signal_clicked().connect([this, item] {
                auto const it = recent_retry_budget().find(item->record.uri);
                if (it == recent_retry_budget().end() || it->second.used < 2) begin_recent_check(item);
                else item->changed.emit();
            }));
            if (remove) connections.push_back(remove->signal_clicked().connect(
                [this, item] { remove_recent(item); }));
            update(false);
        }
        if (image) begin_recent_preview(item, list_item.get(), image);
    });

    _recent_factory->signal_unbind().connect([this](Glib::RefPtr<Gtk::ListItem> const &list_item) {
        cancel_recent_preview(list_item.get());
        if (auto *box = dynamic_cast<Gtk::Box *>(list_item->get_child())) {
            auto *frame = dynamic_cast<Gtk::AspectFrame *>(box->get_first_child());
            auto *visual = frame ? dynamic_cast<Gtk::Box *>(frame->get_child()) : nullptr;
            auto *placeholder = visual ? visual->get_first_child() : nullptr;
            auto *image = placeholder ? dynamic_cast<Gtk::Picture *>(placeholder->get_next_sibling()) : nullptr;
            if (image) { gtk_picture_set_paintable(GTK_PICTURE(image->gobj()), nullptr); image->set_visible(false); }
            if (placeholder) placeholder->set_visible(true);
        }
        _recent_last_unbound_item_for_test = list_item->get_item().get();
        if (auto it = _recent_bindings.find(list_item.get()); it != _recent_bindings.end()) {
            for (auto &connection : it->second) connection.disconnect();
            _recent_last_unbind_live_connections = std::count_if(
                it->second.begin(), it->second.end(),
                [](auto const &connection) { return connection.connected(); });
            ++_recent_unbind_count;
        }
        _recent_bindings.erase(list_item.get());
    });

    auto const expression = Gtk::ClosureExpression<bool>::create(
        [this](Glib::RefPtr<Glib::ObjectBase> const &object) -> bool {
            auto item = std::dynamic_pointer_cast<RecentCardItem>(object);
            return item && recent_item_matches(*item);
        });
    _recent_filter = Gtk::BoolFilter::create(expression);
    _recent_filter_model = Gtk::FilterListModel::create(_recent_store, _recent_filter);
    _recent_selection = Gtk::SingleSelection::create(_recent_filter_model);
    _recent_selection->set_autoselect(false);
    _recent_selection->set_can_unselect(true);

    _recent_grid.set_factory(_recent_factory);
    _recent_grid.set_model(_recent_selection);
    _recent_grid.set_max_columns(3);
    _recent_grid.set_min_columns(1);
    _recent_grid.set_single_click_activate(false); // single click selects only

    _activate_conn = _recent_grid.signal_activate().connect([this](guint position) {
        // Resolve the exact filtered item at the activated position; never rely on a
        // globally unchanged selection.
        auto item = std::dynamic_pointer_cast<RecentCardItem>(_recent_selection->get_object(position));
        if (item) {
            begin_recent_check(item);
        }
    });
}

void StartScreen::cancel_recent_preview(Gtk::ListItem *binding)
{
    auto it = _recent_previews.find(binding);
    if (it == _recent_previews.end()) return;
    auto demand = std::move(it->second);
    _recent_previews.erase(it);
    if (demand->requested) _thumbnail_service->cancel(demand->path, demand->width, demand->height);
    if (_recent_preview_cancel_for_test) _recent_preview_cancel_for_test(demand->uri);
    // The work context owns its own cancellation reference and will release the
    // shared four-probe slot when its async query completes.
}

void StartScreen::begin_recent_preview(Glib::RefPtr<RecentCardItem> const &item,
                                       Gtk::ListItem *binding, Gtk::Picture *image)
{
    if (_suppress_passive_for_test) return;
    if (item->record.format != Recent::RecentFormat::Svg) return;
    auto file = Gio::File::create_for_uri(item->record.uri);
    if (!file) return;
    auto path = file->get_path();
    if (path.empty()) return;
    auto &registry = recent_passive_in_flight();
    if (registry.count(item->record.uri) || registry.size() >= 4) return;
    auto demand = std::make_shared<RecentPreviewDemand>();
    demand->uri = item->record.uri;
    demand->path = path;
    auto const scale = std::max(1, gtk_widget_get_scale_factor(GTK_WIDGET(image->gobj())));
    demand->width = std::min<unsigned>(512, Inkscape::UI::Cache::welcome_card_width * scale);
    demand->height = std::min<unsigned>(512, Inkscape::UI::Cache::welcome_card_height * scale);
    demand->cancellable = g_cancellable_new();
    _recent_previews[binding] = demand;
    auto const token = ++recent_passive_token;
    registry[demand->uri] = token;
    auto weak = std::weak_ptr<RecentPreviewDemand>(demand);
    auto const guard = _open_guard;
    auto complete = [this, weak, guard, item, image, binding](RecentProbeResult result) {
        auto demand = weak.lock();
        if (!guard->alive || !demand) return;
        demand->timeout.disconnect();
        if (!demand->passive_pending) return;
        demand->passive_pending = false;
        if (demand->activate) {
            cancel_recent_preview(binding);
            begin_recent_check(item);
            return;
        }
        if (result != RecentProbeResult::Available) {
            item->set_state(result == RecentProbeResult::Missing ? RecentCardItem::State::Missing
                            : result == RecentProbeResult::PermissionDenied ? RecentCardItem::State::PermissionDenied
                                                                             : RecentCardItem::State::Unreachable);
            return;
        }
        if (item->state != RecentCardItem::State::Ready) item->set_state(RecentCardItem::State::Ready);
        demand->requested = true;
        _thumbnail_service->request(demand->path, demand->width, demand->height,
            [weak, item, image](Inkscape::UI::Cache::WelcomePreviewLaunchResult result) {
                auto demand = weak.lock();
                if (demand) {
                    demand->finished = true;
                    if (result.error == Inkscape::UI::Cache::WelcomePreviewLaunchError::Ok && result.pixbuf) {
                        auto texture = gdk_texture_new_for_pixbuf(result.pixbuf);
                        gtk_picture_set_paintable(GTK_PICTURE(image->gobj()), GDK_PAINTABLE(texture));
                        g_object_unref(texture);
                        if (auto *placeholder = image->get_prev_sibling()) placeholder->set_visible(false);
                        image->set_visible(true);
                    } else {
                        demand->timed_out = result.error == Inkscape::UI::Cache::WelcomePreviewLaunchError::Timeout;
                        demand->limits = result.error == Inkscape::UI::Cache::WelcomePreviewLaunchError::TooLarge;
                        item->changed.emit();
                    }
                }
                if (result.pixbuf) g_object_unref(result.pixbuf);
            });
    };
    demand->timeout = Glib::signal_timeout().connect([weak, item, uri = demand->uri, token] {
        auto &work = recent_passive_in_flight();
        if (auto it = work.find(uri); it != work.end() && it->second == token) work.erase(it);
        if (auto demand = weak.lock()) {
            demand->passive_pending = false;
            g_cancellable_cancel(demand->cancellable);
            item->set_state(RecentCardItem::State::Unreachable);
        }
        return false;
    }, 3000);
    if (_recent_passive_probe) {
        _recent_passive_probe(demand->uri, [this, guard, complete, uri = demand->uri, token](RecentProbeResult result) {
            if (result == RecentProbeResult::MetadataReady) return;
            auto &work = recent_passive_in_flight();
            if (auto it = work.find(uri); it != work.end() && it->second == token) work.erase(it);
            complete(result);
            if (guard->alive) refresh_recent_preview_visibility();
        });
        return;
    }
    auto *work = new RecentCheckWork;
    work->uri = demand->uri;
    work->root = _recent_root_for_test.empty() ? mount_root_for(path) : _recent_root_for_test;
    work->file = G_FILE(g_object_ref(file->gobj()));
    work->cancellable = G_CANCELLABLE(g_object_ref(demand->cancellable));
    work->passive = true;
    work->passive_token = token;
    work->deliver = [this, guard, complete = std::move(complete)](RecentProbeResult result) {
        complete(result);
        if (guard->alive) refresh_recent_preview_visibility();
    };
    g_file_query_info_async(file->gobj(), G_FILE_ATTRIBUTE_STANDARD_TYPE,
                            G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                            demand->cancellable, RecentCheckWork::queried, work);
}

void StartScreen::refresh_recent_preview_visibility()
{
    if (!_recent_scroll_adjustment) return;
    auto const top = gtk_adjustment_get_value(_recent_scroll_adjustment);
    auto const bottom = top + gtk_adjustment_get_page_size(_recent_scroll_adjustment);
    if (bottom <= top) return;
    for (auto const &[binding, connections] : _recent_bindings) {
        auto item = std::dynamic_pointer_cast<RecentCardItem>(binding->get_item());
        auto *box = binding->get_child();
        if (!item || !box) continue;
        graphene_rect_t bounds;
        if (!gtk_widget_compute_bounds(GTK_WIDGET(box->gobj()), GTK_WIDGET(_recent_grid.gobj()), &bounds)) continue;
        bool const visible = bounds.origin.y + bounds.size.height > top && bounds.origin.y < bottom;
        auto *frame = dynamic_cast<Gtk::AspectFrame *>(box->get_first_child());
        auto *visual = frame ? dynamic_cast<Gtk::Box *>(frame->get_child()) : nullptr;
        auto *placeholder = visual ? visual->get_first_child() : nullptr;
        auto *image = placeholder ? dynamic_cast<Gtk::Picture *>(placeholder->get_next_sibling()) : nullptr;
        if (!image) continue;
        if (!visible) {
            cancel_recent_preview(binding);
            gtk_picture_set_paintable(GTK_PICTURE(image->gobj()), nullptr);
            image->set_visible(false);
            if (placeholder) placeholder->set_visible(true);
        } else if (!_recent_previews.count(binding)) {
            begin_recent_preview(item, binding, image);
        }
    }
}

void StartScreen::refilter_recent()
{
    if (!_recent_filter) {
        return;
    }
    // Gtk::Filter::changed() is protected; setting a fresh expression on the
    // BoolFilter notifies its users and the FilterListModel re-evaluates. This is the
    // existing SwatchEditor::refilter() pattern (src/ui/widget/swatch-editor.cpp:513).
    auto expression = Gtk::ClosureExpression<bool>::create(
        [this](Glib::RefPtr<Glib::ObjectBase> const &object) -> bool {
            auto item = std::dynamic_pointer_cast<RecentCardItem>(object);
            return item && recent_item_matches(*item);
        });
    _recent_filter->set_expression(expression);
}

void StartScreen::populate_recent_model()
{
    auto records = Recent::snapshotRecentModel();
    _has_recovery = Recent::recentRecordsHaveRecovery(records);
    for (auto &record : records) {
        _recent_store->append(RecentCardItem::create(std::move(record)));
    }
    update_recent_state();
}

void StartScreen::refresh_recent_model()
{
    cancel_recent_check();
    ++_recent_refresh_count;
    _recent_store->remove_all();
    populate_recent_model();
    if (!_has_recovery && _recovery_only) _recovery_toggle.set_active(false);
    _recovery_toggle.set_visible(_has_recovery);
    refilter_recent();
    update_recent_state();
}

void StartScreen::remove_recent(Glib::RefPtr<RecentCardItem> const &item)
{
    if (!item) return;
    if (auto manager = Gtk::RecentManager::get_default()) {
        auto const key = item->record.display_location.empty() ? item->record.uri : item->record.display_location;
        for (auto const &entry : manager->get_items()) {
            if ((entry->get_uri_display().empty() ? entry->get_uri() : entry->get_uri_display()) != key) continue;
            try {
                manager->remove_item(entry->get_uri());
            } catch (Glib::Error const &error) {
                g_warning("Could not remove recent entry %s: %s", entry->get_uri().c_str(), error.what());
            }
        }
        refresh_recent_model();
    }
}

Gtk::Button *StartScreen::recent_action_for_test(std::string const &uri, bool retry) const
{
    for (auto const &[list_item, connections] : _recent_bindings) {
        auto item = std::dynamic_pointer_cast<RecentCardItem>(list_item->get_item());
        if (!item || item->record.uri != uri) continue;
        auto *box = dynamic_cast<Gtk::Box *>(list_item->get_child());
        auto *preview = box ? box->get_first_child() : nullptr;
        auto *name = preview ? preview->get_next_sibling() : nullptr;
        auto *last = name ? name->get_next_sibling() : nullptr;
        auto *badge = last ? last->get_next_sibling() : nullptr;
        auto *actions = dynamic_cast<Gtk::Box *>(badge ? badge->get_next_sibling() : nullptr);
        auto *first = actions ? actions->get_first_child() : nullptr;
        return dynamic_cast<Gtk::Button *>(retry ? first : first ? first->get_next_sibling() : nullptr);
    }
    return nullptr;
}

guint StartScreen::recent_position_for_test(std::string const &uri) const
{
    for (guint i = 0; i < _recent_selection->get_n_items(); ++i) {
        auto item = std::dynamic_pointer_cast<RecentCardItem>(_recent_selection->get_object(i));
        if (item && item->record.uri == uri) return i;
    }
    return GTK_INVALID_LIST_POSITION;
}

void StartScreen::search_recent_for_test(Glib::ustring const &text)
{
    _search.set_text(text);
}

void StartScreen::set_recovery_only_for_test(bool active)
{
    _recovery_toggle.set_active(active);
}

void StartScreen::replace_recent_for_test(std::string const &old_uri, std::string const &new_uri)
{
    for (guint i = 0; i < _recent_store->get_n_items(); ++i) {
        auto old = _recent_store->get_item(i);
        if (old && old->record.uri == old_uri) {
            GtkListItem *old_binding = nullptr;
            for (auto const &[binding, connections] : _recent_bindings) {
                if (binding->get_item() == old) { old_binding = binding->gobj(); break; }
            }
            if (old_binding) g_object_ref(old_binding);
            _recent_replaced_item_for_test = old;
            auto *old_child = old_binding ? gtk_list_item_get_child(old_binding) : nullptr;
            if (old_child) g_object_ref(old_child);
            auto record = old->record;
            record.uri = new_uri;
            if (auto manager = Gtk::RecentManager::get_default()) {
                try {
                    if (auto entry = manager->lookup_item(new_uri)) record.display_location = entry->get_uri_display();
                } catch (Glib::Error const &) {
                    record.display_location = new_uri;
                }
            }
            _recent_store->splice(i, 1, {RecentCardItem::create(std::move(record))});
            _recent_old_unbind_clean_for_test = _recent_last_unbound_item_for_test == old.get() &&
                                                _recent_last_unbind_live_connections == 0;
            if (old_binding) {
                GtkListItem *new_binding = nullptr;
                for (auto const &[binding, connections] : _recent_bindings) {
                    auto current = std::dynamic_pointer_cast<RecentCardItem>(binding->get_item());
                    if (current && current->record.uri == new_uri) { new_binding = binding->gobj(); break; }
                }
                _recent_reused_binding_for_test = new_binding == old_binding;
                if (new_binding && new_binding != old_binding && old_child) {
                    g_signal_emit_by_name(_recent_factory->gobj(), "unbind", new_binding);
                    gtk_list_item_set_child(old_binding, nullptr);
                    gtk_list_item_set_child(new_binding, old_child);
                    g_signal_emit_by_name(_recent_factory->gobj(), "bind", new_binding);
                }
                if (old_child) g_object_unref(old_child);
                g_object_unref(old_binding);
            }
            return;
        }
    }
}

void StartScreen::emit_replaced_recent_changed_for_test()
{
    if (_recent_replaced_item_for_test) _recent_replaced_item_for_test->set_state(RecentCardItem::State::Missing);
}

void StartScreen::set_recent_probe_for_test(RecentProbe probe)
{
    _recent_probe = std::move(probe);
    _suppress_passive_for_test = true;
    // The deterministic test probe replaces passive work launched while the
    // constructor bound the first visible cards.
    while (!_recent_previews.empty()) {
        auto *binding = _recent_previews.begin()->first;
        auto item = std::dynamic_pointer_cast<RecentCardItem>(binding->get_item());
        auto uri = _recent_previews.begin()->second->uri;
        cancel_recent_preview(binding);
        recent_passive_in_flight().erase(uri);
        if (item && item->state != RecentCardItem::State::Ready) item->set_state(RecentCardItem::State::Ready);
    }
}

void StartScreen::set_recent_passive_probe_for_test(RecentProbe probe)
{
    _recent_passive_probe = std::move(probe);
    std::vector<Gtk::ListItem *> bindings;
    for (auto const &[binding, demand] : _recent_previews) bindings.push_back(binding);
    for (auto *binding : bindings) {
        auto uri = _recent_previews.at(binding)->uri;
        auto item = std::dynamic_pointer_cast<RecentCardItem>(binding->get_item());
        auto *box = binding->get_child();
        auto *frame = box ? dynamic_cast<Gtk::AspectFrame *>(box->get_first_child()) : nullptr;
        auto *visual = frame ? dynamic_cast<Gtk::Box *>(frame->get_child()) : nullptr;
        auto *placeholder = visual ? visual->get_first_child() : nullptr;
        auto *image = placeholder ? dynamic_cast<Gtk::Picture *>(placeholder->get_next_sibling()) : nullptr;
        cancel_recent_preview(binding);
        recent_passive_in_flight().erase(uri);
        if (item && image) begin_recent_preview(item, binding, image);
    }
}

unsigned StartScreen::recent_retry_used_for_test(std::string const &uri) const
{
    auto it = recent_retry_budget().find(uri);
    return it == recent_retry_budget().end() ? 0 : it->second.used;
}

std::size_t StartScreen::recent_preview_renders_for_test() const
{
    return _thumbnail_service->stats().renders;
}

bool StartScreen::recent_preview_finished_for_test(std::string const &uri) const
{
    for (auto const &[binding, demand] : _recent_previews)
        if (demand->uri == uri) return demand->finished;
    return false;
}

void StartScreen::activate_recent_for_test(std::string const &uri)
{
    if (!_test_recent_item || _test_recent_item->record.uri != uri) {
        Recent::RecentFileRecord record;
        record.uri = uri;
        record.display_location = uri;
        _test_recent_item = RecentCardItem::create(std::move(record));
    }
    begin_recent_check(_test_recent_item);
}

StartScreen::RecentProbeResult StartScreen::recent_result_for_test() const
{
    if (!_test_recent_item) return RecentProbeResult::Unreachable;
    switch (_test_recent_item->state) {
        case RecentCardItem::State::Missing: return RecentProbeResult::Missing;
        case RecentCardItem::State::Ready: return RecentProbeResult::Available;
        case RecentCardItem::State::PermissionDenied: return RecentProbeResult::PermissionDenied;
        case RecentCardItem::State::Busy: return RecentProbeResult::Busy;
        default: return RecentProbeResult::Unreachable;
    }
}

void StartScreen::remove_recent_for_test()
{
    remove_recent(_test_recent_item);
}

void StartScreen::cancel_recent_check()
{
    ++_recent_generation;
    _recent_timeout.disconnect();
    if (_recent_cancellable) {
        g_cancellable_cancel(_recent_cancellable);
        g_object_unref(_recent_cancellable);
        _recent_cancellable = nullptr;
    }
    if (_pending_recent_item && _pending_recent_item->state == RecentCardItem::State::Checking) {
        _pending_recent_item->set_state(_pending_recent_item->before_checking);
    }
    _pending_recent_item.reset();
    _pending_recent_uri.clear();
}

void StartScreen::restore_busy_recent_cards()
{
    for (guint i = 0; i < _recent_store->get_n_items(); ++i) {
        auto item = _recent_store->get_item(i);
        if (item->state == RecentCardItem::State::Busy) item->set_state(item->before_busy);
    }
}

void StartScreen::finish_recent_check(std::uint64_t generation,
                                      Glib::RefPtr<RecentCardItem> const &item,
                                      Glib::RefPtr<Gio::File> const &file,
                                      RecentProbeResult result)
{
    if (!_open_guard->alive || generation != _recent_generation) return;
    _recent_timeout.disconnect();
    g_object_unref(_recent_cancellable);
    _recent_cancellable = nullptr;
    _pending_recent_item.reset();
    _pending_recent_uri.clear();
    restore_busy_recent_cards();
    auto &retry = recent_retry_budget()[item->record.uri];
    if (result == RecentProbeResult::Unreachable && retry.unreachable) ++retry.used;
    retry.unreachable = result == RecentProbeResult::Unreachable;
    if (result == RecentProbeResult::Available) {
        retry.used = 0;
        item->set_state(RecentCardItem::State::Ready);
        if (_recent_open_for_test) _recent_open_for_test(file);
        else open_file(file);
    } else {
        item->set_state(result == RecentProbeResult::Missing ? RecentCardItem::State::Missing
                        : result == RecentProbeResult::PermissionDenied ? RecentCardItem::State::PermissionDenied
                                                                         : RecentCardItem::State::Unreachable);
    }
}

void StartScreen::begin_recent_check(Glib::RefPtr<RecentCardItem> const &item)
{
    if (!item || _open_guard->in_flight) return;
    auto const &uri = item->record.uri;
    for (auto const &[binding, demand] : _recent_previews) {
        if (demand->uri == uri && demand->passive_pending) {
            cancel_recent_preview(binding);
            break;
        }
    }
    bool const same_pending = _pending_recent_item == item;
    if (same_pending) { // a second activation cancels rather than queues another probe
        cancel_recent_check();
        return;
    }

    auto &registry = recent_work_in_flight();
    if (registry.count(uri) || registry.size() >= 4) {
        if (item->state != RecentCardItem::State::Busy) item->before_busy = item->state;
        item->set_state(RecentCardItem::State::Busy);
        return;
    }
    cancel_recent_check();
    auto file = Gio::File::create_for_uri(uri);
    if (!file || file->get_path().empty()) {
        item->set_state(RecentCardItem::State::NonLocal);
        return;
    }
    registry.insert(uri);

    item->before_checking = item->state == RecentCardItem::State::Busy ? item->before_busy : item->state;
    item->set_state(RecentCardItem::State::Checking);
    _pending_recent_item = item;
    _pending_recent_uri = uri;
    _recent_cancellable = g_cancellable_new();
    auto const generation = _recent_generation;
    auto const guard = _open_guard;
    _recent_timeout = Glib::signal_timeout().connect([this, guard, generation, item] {
        if (guard->alive && generation == _recent_generation) {
            _recent_timeout.disconnect();
            if (_recent_cancellable) {
                g_cancellable_cancel(_recent_cancellable);
                g_object_unref(_recent_cancellable);
                _recent_cancellable = nullptr;
            }
            ++_recent_generation;
            _pending_recent_item.reset();
            _pending_recent_uri.clear();
            restore_busy_recent_cards();
            auto &retry = recent_retry_budget()[item->record.uri];
            if (retry.unreachable) ++retry.used;
            retry.unreachable = true;
            item->set_state(RecentCardItem::State::Unreachable);
        }
        return false;
    }, 3000);
    if (_recent_probe) {
        _recent_probe(uri, [this, guard, generation, item, file, uri](RecentProbeResult result) {
            if (result == RecentProbeResult::MetadataReady) return;
            recent_work_in_flight().erase(uri);
            if (guard->alive) finish_recent_check(generation, item, file, result);
        });
        return;
    }
    auto *context = new RecentCheckWork;
    context->uri = uri;
    context->root = _recent_root_for_test.empty() ? mount_root_for(file->get_path()) : _recent_root_for_test;
    context->file = G_FILE(g_object_ref(file->gobj()));
    context->cancellable = G_CANCELLABLE(g_object_ref(_recent_cancellable));
    context->metadata_ready = _recent_metadata_for_test;
    context->deliver = [this, guard, generation, item, file](RecentProbeResult result) {
        if (guard->alive) finish_recent_check(generation, item, file, result);
    };
    g_file_query_info_async(file->gobj(), G_FILE_ATTRIBUTE_STANDARD_TYPE,
                            G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                            _recent_cancellable, RecentCheckWork::queried, context);
}

bool StartScreen::recent_item_matches(RecentCardItem const &item) const
{
    if (_recovery_only && item.record.recovery == Recent::RecoveryKind::None) {
        return false;
    }
    if (_search_folded.empty()) {
        return true;
    }
    return Recent::recentRecordMatches(item.record, _search_folded);
}

void StartScreen::update_recent_state()
{
    auto const total = _recent_store->get_n_items();
    auto const visible = _recent_filter_model ? _recent_filter_model->get_n_items() : 0;
    if (total == 0) {
        _recent_state.set_visible_child("empty");
    } else if (visible == 0) {
        _recent_state.set_visible_child("no-match");
    } else {
        _recent_state.set_visible_child("grid");
    }
}

void StartScreen::on_search_changed()
{
    _search_folded = Recent::foldedSearchKey(_search.get_text().raw());
    refilter_recent();
    update_recent_state();
}

void StartScreen::on_recovery_toggled()
{
    _recovery_only = _recovery_toggle.get_active();
    refilter_recent();
    update_recent_state();
}

void StartScreen::set_active_nav(char const *page)
{
    auto const is_recent = std::string_view(page) == "recent";
    auto set_active = [](Gtk::Button &button, bool active) {
        gtk_accessible_update_state(GTK_ACCESSIBLE(button.gobj()),
                                    GTK_ACCESSIBLE_STATE_SELECTED, active, -1);
        if (active) {
            button.add_css_class("welcome-nav-active");
        } else {
            button.remove_css_class("welcome-nav-active");
        }
    };
    set_active(_recent_nav, is_recent);
    set_active(_templates_nav, !is_recent);

    _content_stack.set_visible_child(Glib::ustring(page));
    if (is_recent) {
        _title_label.set_label(_("Recent documents"));
        _subtitle_label.set_label(_("Continue where you left off."));
    } else {
        _title_label.set_label(_("New from template"));
        _subtitle_label.set_label(_("Choose a preset to start a new document."));
    }
    _search_row.set_visible(is_recent);
}

void StartScreen::on_window_realized()
{
    if (auto *scroll = gtk_widget_get_ancestor(GTK_WIDGET(_recent_grid.gobj()), GTK_TYPE_SCROLLED_WINDOW)) {
        _recent_scroll_adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
        _recent_scroll_handler = g_signal_connect(_recent_scroll_adjustment, "value-changed",
            G_CALLBACK(+[](GtkAdjustment *, gpointer data) {
                static_cast<StartScreen *>(data)->refresh_recent_preview_visibility();
            }), this);
        auto guard = _open_guard;
        Glib::signal_idle().connect_once([this, guard] {
            if (guard->alive) refresh_recent_preview_visibility();
        });
    }
    auto *native = dynamic_cast<Gtk::Native *>(this);
    if (!native) {
        return;
    }
    _surface = native->get_surface();
    if (!_surface) {
        return;
    }
    _surface_width_conn = _surface->property_width().signal_changed().connect(
        sigc::mem_fun(*this, &StartScreen::update_compact_nav));
    update_compact_nav();
    if (_recent_selection->get_n_items() > 0) {
        _recent_grid.grab_focus();
        _recent_selection->set_selected(GTK_INVALID_LIST_POSITION);
    } else {
        _new_btn.grab_focus();
    }
}

void StartScreen::update_compact_nav()
{
    if (!_surface) {
        return;
    }
    auto const width = _surface->get_width();

    // Reference responsive column count. Updated on every width notification,
    // before the compact-state early return, so a widened surface can use more
    // columns again. Native GtkGridView still picks fewer when the card minimum
    // does not fit; min-columns stays 1.
    int columns = 5;
    if (width < 620) {
        columns = 1;
    } else if (width < 900) {
        columns = 2;
    } else if (width < 1400) {
        columns = 3;
    } else if (width < 1700) {
        columns = 4;
    }
    _recent_grid.set_max_columns(columns);

    bool const compact = width < 720;
    if (compact == _compact) {
        return;
    }
    _compact = compact;
    if (compact) {
        _sidebar.add_css_class("compact");
        _sidebar.set_size_request(68, -1); // icon rail; still vertical
    } else {
        _sidebar.remove_css_class("compact");
        _sidebar.set_size_request(220, -1);
    }
    // Adapt the existing header/footer in place; the toplevel is never resized here.
    // Compact stacks the search below the title so it cannot force a wide window, and
    // stacks the footer so the checkbox and version remain reachable.
    _header.set_orientation(compact ? Gtk::Orientation::VERTICAL : Gtk::Orientation::HORIZONTAL);
    _footer.set_orientation(compact ? Gtk::Orientation::VERTICAL : Gtk::Orientation::HORIZONTAL);
    update_logo();
    _brand_label.set_visible(!compact);
    // Labels are hidden visually in the rail; their buttons keep explicit accessible
    // names (set in the constructor) in addition to the tooltips.
    for (auto *label : _nav_labels) {
        label->set_visible(!compact);
    }
}

void StartScreen::update_logo()
{
    if (!_logo_file) {
        return; // absent asset keeps the existing blank picture behaviour
    }
    // Native factory: logical size in application pixels plus the widget scale factor.
    // Refreshed only on mode/scale change, never re-reading the resource on every resize.
    auto const size = _compact ? 36 : 96;
    auto paintable = Gtk::IconPaintable::create(_logo_file, size, get_scale_factor());
    if (paintable) {
        _logo.set(paintable);
        _logo.set_pixel_size(size);
    }
}

void StartScreen::begin_closing()
{
    _closing = true;
    if (auto const content = get_child()) {
        content->set_sensitive(false);
    }
}

void StartScreen::new_document()
{
    if (_closing) {
        return;
    }
    // Exact existing default-template operation; the return value is authoritative.
    if (auto *app = InkscapeApplication::instance()) {
        if (auto *document = app->document_new()) {
            _finish(document);
        }
    }
}

void StartScreen::create_from_template()
{
    // Activation and Create must both no-op without a live selection; never fall
    // back to the default document from this entry point.
    if (!templates.has_selected_preset() && !templates.has_selected_new_template()) {
        return;
    }
    if (InkscapeApplication::instance()) {
        auto completion = make_template_completion();
        // nullptr also resolves the native current page inside TemplateList; a null
        // result means the parameters were cancelled and Welcome stays open.
        auto *document = templates.new_document(templates.get_visible_child());
        completion(document);
    }
}

std::function<void(SPDocument *)> StartScreen::make_template_completion()
{
    auto const guard_state = _open_guard;
    return [this, guard_state](SPDocument *document) {
        if (guard_state->alive && document && _closing) {
            // Welcome is already closing for another request; still give this document
            // its one desktop.
            if (auto *app = InkscapeApplication::instance(); app && !app->quitPending()) {
                app->desktopOpen(document);
            }
        } else if (guard_state->alive && document) {
            _finish(document);
        } else if (!guard_state->alive && document) {
            // A settings dialog may have closed Welcome. The document still needs
            // its one desktop, but the destroyed window cannot emit its signal.
            if (auto *app = InkscapeApplication::instance();
                app && !app->quitPending()) {
                app->desktopOpen(document);
            }
        }
    };
}

void StartScreen::open_document()
{
    auto const guard_state = _open_guard;
    if (guard_state->chooser_pending) return;
    std::string folder;
    get_start_directory(folder, "/dialogs/open/path");
    auto const filters = create_open_filters();
    guard_state->chooser_pending = true;
    guard_state->chooser_cancelled = false;
    guard_state->chooser_cancellable = Gio::Cancellable::create();
    choose_file_open_async(_("Open a document"), this, filters, std::move(folder),
        make_open_completion(), guard_state->chooser_cancellable);
}

void StartScreen::cancel_chooser_for_test()
{
    if (_open_guard->chooser_cancellable) _open_guard->chooser_cancellable->cancel();
}

StartScreen::OpenCompletion StartScreen::make_open_completion()
{
    auto const guard_state = _open_guard;
    return [this, guard_state](Glib::RefPtr<Gio::File> file, std::string folder, std::string error) {
            guard_state->chooser_pending = false;
            guard_state->chooser_cancellable.reset();
            if (guard_state->chooser_completed) guard_state->chooser_completed();
            if (guard_state->chooser_cancelled) return;
            auto *app = InkscapeApplication::instance();
            if (!error.empty()) {
                g_warning("Welcome open chooser failed: %s", error.c_str());
                if (app && !app->quitPending()) show_file_chooser_error(error);
                return;
            }
            if (!file) return; // Cancel leaves a live Welcome intact.
            if (!folder.empty()) {
                Inkscape::Preferences::get()->setString("/dialogs/open/path", folder);
            }
            if (guard_state->alive) {
                open_file(file);
            } else if (app && !app->quitPending()) {
                // External Open may have closed Welcome while the chooser was up.
                app->create_window(file);
            }
    };
}

void StartScreen::open_uri(Glib::ustring const &uri)
{
    if (uri.empty()) {
        return;
    }
    // The only Gio::File creation in W1, at an explicit open operation.
    auto file = Gio::File::create_for_uri(uri);
    if (!file) {
        return;
    }
    open_file(file);
}

void StartScreen::open_file(Glib::RefPtr<Gio::File> const &file)
{
    auto *app = InkscapeApplication::instance();
    if (!app || !file) {
        return;
    }
    // Shared entry point for Browse and Recent activation. Repeated activation while a
    // synchronous open is already in flight (nested import/error-dialog event loop) must
    // not start a second open on this screen.
    auto const guard_state = _open_guard;
    if (guard_state->in_flight) {
        return;
    }
    if (_closing) {
        return; // e.g. a late async Recent check while Welcome waits to close
    }

    InkscapeApplication::OpenResult result;
    {
        guard_state->in_flight = true;
        // A nested importer dialog can close Welcome. The guard owns only this
        // independent state and never dereferences the destroyed window.
        auto const clear_in_flight = scope_exit([guard_state] { guard_state->in_flight = false; });
        result = app->create_window(file);
    }

    if (!guard_state->alive) {
        return;
    }

    if (!result.opened() || !result.desktop) {
        // Cancelled, failed, or a document without a view: Welcome stays open and usable.
        // The single failure dialog was shown by create_window(); success is never inferred
        // from the global active document.
        return;
    }

    // Close without _finish: create_window() already built the one desktop for this
    // request, and emitting _signal_open would run process_document() and create a second
    // desktop for the same file. Closing waits until the new window is on screen.
    app->closeStartScreenWhenShown(); // Caution: may delete self.
}

void StartScreen::show_preferences()
{
    // Stable application action only. W7 owns the documentless presenter and the
    // cold-start registration; W1 creates no dummy document and no copied settings UI.
    activate_action("app.preferences");
}

bool StartScreen::on_key_pressed(unsigned keyval, unsigned /*keycode*/, Gdk::ModifierType state)
{
    // Exactly one platform accelerator: Cmd on macOS, Ctrl elsewhere. Lock is ignored;
    // Shift/Alt/Super/Hyper and any other combination are deliberately NOT consumed so
    // the application's existing native accelerators (Ctrl+Shift+N, Ctrl+Alt+O, ...)
    // keep working, and the native quit route stays available.
#if defined(__APPLE__)
    auto const platform_modifier = Gdk::ModifierType::META_MASK;
#else
    auto const platform_modifier = Gdk::ModifierType::CONTROL_MASK;
#endif
    auto const pressed = state & ~Gdk::ModifierType::LOCK_MASK;
    if (_closing) {
        // Welcome waits for the opened window; swallow its own accelerators so no
        // later handler (e.g. a future app.* accelerator) starts a second request.
        bool const own = pressed == platform_modifier &&
            (keyval == GDK_KEY_n || keyval == GDK_KEY_N || keyval == GDK_KEY_o || keyval == GDK_KEY_O ||
             keyval == GDK_KEY_f || keyval == GDK_KEY_F);
        return own;
    }
    if (pressed == platform_modifier) {
        if (keyval == GDK_KEY_n || keyval == GDK_KEY_N) {
            // Consumed locally; the application accelerator route is left unchanged.
            new_document();
            return true;
        }
        if (keyval == GDK_KEY_o || keyval == GDK_KEY_O) {
            open_document();
            return true;
        }
        if (keyval == GDK_KEY_f || keyval == GDK_KEY_F) {
            set_active_nav("recent");
            _search.grab_focus();
            return true;
        }
    }

    if (keyval == GDK_KEY_Escape) {
        if (!_search.get_text().empty()) {
            _search.set_text("");
            return true;
        }
        return false; // empty search: no-op, never creates or closes a document
    }

    auto *focus = gtk_window_get_focus(GTK_WINDOW(gobj()));
    if (pressed == Gdk::ModifierType{} && focus &&
        (focus == GTK_WIDGET(_recent_grid.gobj()) ||
         gtk_widget_get_ancestor(focus, GTK_TYPE_GRID_VIEW) == GTK_WIDGET(_recent_grid.gobj())) &&
        !GTK_IS_BUTTON(focus)) {
        auto const character = gdk_keyval_to_unicode(keyval);
        if (character && g_unichar_isprint(character)) {
            char utf8[7]{};
            g_unichar_to_utf8(character, utf8);
            _search.set_text(utf8);
            _search.grab_focus();
            return true;
        }
    }

    // Return and everything else stay owned by GridView/TemplateList and the native route.
    return false;
}

void StartScreen::_finish(SPDocument *document)
{
    _signal_open.emit(document);
    if (auto *app = InkscapeApplication::instance()) {
        app->closeStartScreenWhenShown(); // Caution: may delete self.
    } else {
        close(); // Caution: typically deletes self.
    }
}

/**
 * Get the preference for the startup mode.
 *
 * @returns
 *    0 - Show nothing
 *    1 = Show the startup screen
 */
int StartScreen::get_start_mode()
{
    auto prefs = Inkscape::Preferences::get();
    auto old_enabled = prefs->getBool("/options/boot/enabled", true);
    return prefs->getInt("/options/boot/mode", old_enabled ? 1 : 0);
}

} // namespace Inkscape::UI::Dialog

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
