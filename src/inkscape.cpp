// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Interface to main application.
 */
/* Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Liam P. White <inkscapebrony@gmail.com>
 *
 * Copyright (C) 1999-2014 authors
 * c++ port Copyright (C) 2003 Nathan Hurst
 * c++ification Copyright (C) 2014 Liam P. White
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <unistd.h>

#include <boost/stacktrace.hpp>
#ifdef _WIN32
#undef near
#undef IGNORE
#undef DOUBLE_CLICK
#endif
#include <glibmm/regex.h>
#include <glibmm/i18n.h>
#include <glibmm/miscutils.h>
#include <glibmm/convert.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/label.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/settings.h>
#include <gtkmm/textbuffer.h>

#include "desktop.h"
#include "document.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "inkscape-version-info.h"
#include "inkscape-window.h"
#include "selection.h"

#include "debug/simple-event.h"
#include "debug/event-tracker.h"
#include "io/resource.h"
#include "io/stream/inkscapestream.h"
#include "io/sys.h"
#include "libnrtype/font-factory.h"
#include "libnrtype/font-utils.h"
#include "object/sp-root.h"
#include "io/resource.h"
#include "io/recent-files.h"
#include "ui/builder-utils.h"
#include "ui/themes.h"
#include "ui/dialog-events.h"
#include "ui/dialog/dialog-manager.h"
#include "ui/error-reporter.h"
#include "ui/themes.h"
#include "ui/tools/tool-base.h"
#include "ui/util.h"
#include "ui/widget/gtk-registry.h"
#include "util/delete-with.h"
#include "util/crash-handler-thread.h"
#include "util/font-discovery.h"
#include "vacards-public-identity.h"

static bool desktop_is_active(SPDesktop const *d)
{
    return !INKSCAPE.get_desktops().empty() && d == INKSCAPE.get_desktops().front();
}

static void (* segv_handler) (int) = SIG_DFL;
static void (* abrt_handler) (int) = SIG_DFL;
static void (* fpe_handler)  (int) = SIG_DFL;
static void (* ill_handler)  (int) = SIG_DFL;
#ifndef _WIN32
static void (* bus_handler)  (int) = SIG_DFL;
#endif

static constexpr int SP_INDENT = 8;
static std::thread::id crash_handler_main_thread;

/// Crash dialog request to report the problem. The destination is VA Studio's
/// public support identity (vacards-public-identity.h), never the upstream
/// Inkscape bug tracker; without a configured URL it is the local SUPPORT.md.
static Glib::ustring crash_report_markup()
{
    auto const uri = Inkscape::VACards::support_uri();
    if (uri.empty()) {
        return Glib::Markup::escape_text(Glib::ustring::compose(
            _("If you can reproduce this crash, please report it to VA Studio as described in %1 "
              "(in the VA Studio documentation folder) and include the backtrace from the area below."),
            Inkscape::VACards::support_document));
    }
    return Glib::ustring::compose(
        _("If you can reproduce this crash, please <a href=\"%1\">report it to VA Studio</a> "
          "and include the backtrace from the area below."),
        Glib::Markup::escape_text(uri));
}

namespace Inkscape {

// Expose constructor to allow use with std::optional.
struct Application::ConstructibleApplication : Application
{
    ConstructibleApplication(bool use_gui, RuntimePolicy policy)
        : Application(use_gui, policy)
    {}
};

/**
 *  Creates a new Inkscape::Application global object with the default
 *  Editor runtime policy.
 */
void Application::create(bool use_gui)
{
    create(use_gui, RuntimePolicy::Editor);
}

/**
 *  Creates a new Inkscape::Application global object with an explicit,
 *  immutable runtime policy.
 *
 *  A repeated request for the same existing policy is a no-op, preserving the
 *  historical behavior of create(bool). A request for a different policy is
 *  rejected before any global state is changed. PreviewHelper is non-GUI only.
 */
void Application::create(bool use_gui, RuntimePolicy policy)
{
    if (_get()) { // block multiple calls by test suite
        auto &application = *_get();
        if (application._runtime_policy != policy) {
            throw std::logic_error("Inkscape::Application already exists with a different runtime policy");
        }
        if (policy == RuntimePolicy::PreviewHelper && use_gui) {
            throw std::invalid_argument("Inkscape::Application preview-helper policy cannot use the GUI");
        }
        return;
    }

    if (policy == RuntimePolicy::PreviewHelper && use_gui) {
        throw std::invalid_argument("Inkscape::Application preview-helper policy cannot use the GUI");
    }
    _get().emplace(use_gui, policy);
}

/**
 *  Checks whether the current Inkscape::Application global object exists.
 */
bool Application::exists()
{
    return _get().has_value();
}

/**
 *  Returns the current Inkscape::Application global object.
 *  \pre Application::exists()
 */
Application &Application::instance()
{
    return *_get();
}

Application::Application(bool use_gui, RuntimePolicy policy) :
    _pages("all")
    , _runtime_policy(policy)
    , _use_gui(use_gui)
{
    using namespace Inkscape::IO::Resource;
    /* fixme: load application defaults */

    // we need a app runing to know shared path
    auto extensiondir_shared = get_path_string(SHARED, EXTENSIONS);
    if (!extensiondir_shared.empty()) {
        std::string pythonpath = extensiondir_shared;
        auto pythonpath_old = Glib::getenv("PYTHONPATH");
        if (!pythonpath_old.empty()) {
            pythonpath += G_SEARCHPATH_SEPARATOR + pythonpath_old;
        }
        Glib::setenv("PYTHONPATH", pythonpath);
    }

    // Historical Editor initialization order: the emergency-save signal
    // handlers are installed before Preferences::get(). A preview helper skips
    // only these recovery installs; it still performs the shared preferences,
    // font and language setup below.
    if (policy == RuntimePolicy::Editor) {
        crash_handler_main_thread = std::this_thread::get_id();
        segv_handler = signal (SIGSEGV, Application::crash_handler);
        abrt_handler = signal (SIGABRT, Application::crash_handler);
        fpe_handler  = signal (SIGFPE,  Application::crash_handler);
        ill_handler  = signal (SIGILL,  Application::crash_handler);
#ifndef _WIN32
        bus_handler  = signal (SIGBUS,  Application::crash_handler);
#endif
    }

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    // The preferences ErrorReporter is Editor-only recovery behavior too. It is
    // intentionally after Preferences::get(), matching the historical order.
    if (policy == RuntimePolicy::Editor) {
        ErrorReporter* handler = new ErrorReporter(use_gui);
        prefs->setErrorHandler(handler);
        {
            Glib::ustring msg;
            Glib::ustring secondary;
            if (prefs->getLastError( msg, secondary )) {
                handler->handleError(msg, secondary);
            }
        }
    }

    if (use_gui) {
        using namespace Inkscape::IO::Resource;

        auto display = Gdk::Display::get_default();
        auto icon_theme = Gtk::IconTheme::get_for_display(display);
        auto search_paths = icon_theme->get_search_path();
        // prepend search paths or else hicolor icons fallback will fail
        for (auto type : {USER, SHARED, SYSTEM}) {
            auto path = get_path_string(type, ICONS);
            if (!path.empty()) {
                search_paths.insert(search_paths.begin(), path);
            }
        }
        icon_theme->set_search_path(search_paths);

        themecontext = new Inkscape::UI::ThemeContext();
        themecontext->add_gtk_css(false);
        auto scale = prefs->getDoubleLimited(UI::ThemeContext::get_font_scale_pref_path(), 100, 50, 200);
        themecontext->adjustGlobalFontScale(scale / 100.0);
        themecontext->applyMonospacedFont(themecontext->getMonospacedFont());
        Inkscape::UI::ThemeContext::initialize_source_syntax_styles();

        // register custom widget types
        Inkscape::UI::Widget::register_all();
    }

    /* set language for user interface according setting in preferences */
    Glib::ustring ui_language = prefs->getString("/ui/language");
    if(!ui_language.empty())
    {
        Glib::setenv("LANGUAGE", ui_language.raw(), true);
#ifdef _WIN32
        // locale may be set to C with some Windows Region Formats (like English(Europe)).
        // forcing the LANGUAGE variable to be ignored
        // see :guess_category_value:gettext-runtime/intl/dcigettext.c,
        // and :gl_locale_name_from_win32_LANGID:gettext-runtime/gnulib-lib/localename.c
        Glib::setenv("LANG", ui_language.raw(), true);
#endif
    }

    if (use_gui)
    {
        Inkscape::UI::Tools::init_latin_keys_group();

        /* update highlight colors when theme changes */
        themecontext->getChangeThemeSignal().connect([this](){
            themecontext->themechangecallback();
        });

        /* Set animations to user-defined value */
        if (prefs->hasPref("/theme/enableAnimations")) {
            // If the animation setting is overridden, set the enableAnimations
            // value and set it (Gtk::Settings is not persistent)
            bool enabled = prefs->getBool("/theme/enableAnimations", false);
            Gtk::Settings::get_default()->property_gtk_enable_animations().set_value(enabled);
        }

        /* set PangoCairo rendering backend to FontConfig. See issue #6117 */
        Glib::setenv("PANGOCAIRO_BACKEND", "fontconfig", true);

    }

    /* Initialize font factory */
    auto &factory = FontFactory::get();
    auto add_startup_fonts = [&factory](char const *path) {
        if (font_directory_allowed_at_startup(path)) factory.AddFontsDir(path);
    };
    if (prefs->getBool("/options/font/use_fontsdir_system", true)) {
        char const *fontsdir = get_path(SYSTEM, FONTS);
        add_startup_fonts(fontsdir);
    }
    // we keep user font dir for simplicity
    if (prefs->getBool("/options/font/use_fontsdir_user", true)) {
        char const *fontsdirshared = get_path(SHARED, FONTS);
        if (fontsdirshared) {
            add_startup_fonts(fontsdirshared);
        }
        char const *fontsdir = get_path(USER, FONTS);
        add_startup_fonts(fontsdir);
    }
    Glib::ustring fontdirs_pref = prefs->getString("/options/font/custom_fontdirs");
    std::vector<Glib::ustring> fontdirs = Glib::Regex::split_simple("\\|", fontdirs_pref);
    for (auto &fontdir : fontdirs) {
        add_startup_fonts(fontdir.c_str());
    }
}

Application::~Application()
{
    if (!_desktops.empty()) {
        g_critical("desktops still in list on application destruction!");
    }

    Inkscape::Preferences::unload();
}

void
Application::crash_handler (int /*signum*/)
{
    using Inkscape::Debug::SimpleEvent;
    using Inkscape::Debug::EventTracker;
    using Inkscape::Debug::Logger;

    auto const on_main_thread = Util::crash_handler_on_main_thread(crash_handler_main_thread);
    static bool recursion = false;

    /*
     * reset all signal handlers: any further crashes should just be allowed
     * to crash normally.
     * */
    signal (SIGSEGV, segv_handler );
    signal (SIGABRT, abrt_handler );
    signal (SIGFPE,  fpe_handler  );
    signal (SIGILL,  ill_handler  );
#ifndef _WIN32
    signal (SIGBUS,  bus_handler  );
#endif

    /* Stop bizarre loops */
    if (recursion) {
        abort ();
    }
    recursion = true;

    EventTracker<SimpleEvent<Inkscape::Debug::Event::CORE> > tracker("crash");
    tracker.set<SimpleEvent<> >("emergency-save");

    fprintf(stderr, "\nEmergency save activated!\n");

    time_t sptime = time (nullptr);
    struct tm *sptm = localtime (&sptime);
    gchar sptstr[256];
    strftime(sptstr, 256, "%Y_%m_%d_%H_%M_%S", sptm);

    gint count = 0;
    gchar *curdir = g_get_current_dir(); // This one needs to be freed explicitly
    std::vector<gchar *> savednames;
    std::vector<gchar *> failednames;
    for (auto doc : INKSCAPE._document_set) {
        Inkscape::XML::Node *repr;
        repr = doc->getReprRoot();
        if (doc->isModifiedSinceSave()) {
            const gchar *docname;
            char n[64];

            /* originally, the document name was retrieved from
             * the sodipod:docname attribute */
            docname = doc->getDocumentName();
            if (docname) {
                /* Removes an emergency save suffix if present: /(.*)\.[0-9_]*\.[0-9_]*\.[~\.]*$/\1/ */
                const char* d0 = strrchr ((char*)docname, '.');
                if (d0 && (d0 > docname)) {
                    const char* d = d0;
                    unsigned int dots = 0;
                    while ((isdigit (*d) || *d=='_' || *d=='.') && d>docname && dots<2) {
                        d -= 1;
                        if (*d=='.') dots++;
                    }
                    if (*d=='.' && d>docname && dots==2) {
                        size_t len = MIN (d - docname, 63);
                        memcpy (n, docname, len);
                        n[len] = '\0';
                        docname = n;
                    }
                }
            }
            if (!docname || !*docname) docname = "emergency";

            // Emergency filename
            char c[1024];
            g_snprintf (c, 1024, "%.256s.%s.%d.svg", docname, sptstr, count);

            const char* document_filename = doc->getDocumentFilename();
            char* document_base = nullptr;
            if (document_filename) {
                document_base = g_path_get_dirname(document_filename);
            }

            // Find a location
            const char* locations[] = {
                // Don't use getDocumentBase as that also can be unsaved template locations.
                document_base,
                g_get_home_dir(),
                g_get_tmp_dir(),
                curdir,
            };
            FILE *file = nullptr;
            for(auto & location : locations) {
                if (!location) continue; // It seems to be okay, but just in case
                gchar * filename = g_build_filename(location, c, nullptr);
                Inkscape::IO::dump_fopen_call(filename, "E");
                file = Inkscape::IO::fopen_utf8name(filename, "w");
                if (file) {
                    g_snprintf (c, 1024, "%s", filename); // we want the complete path to be stored in c (for reporting purposes)
                    break;
                }
            }
            if (document_base) {
                g_free(document_base);
            }

            // Save
            if (file) {
                bool saved = false;
                // Own the FILE for the whole block so a non-std exception or an
                // exception thrown while reporting still closes the stream.
                auto file_owner = Inkscape::Util::delete_with<fclose>(file);
                try {
                    sp_repr_save_stream (repr->document(), file, SP_SVG_NS_URI);
                    saved = true;
                } catch (Inkscape::IO::StreamException &e) {
                    saved = false;
                } catch (std::exception &e) {
                    saved = false;
                } catch (...) {
                    saved = false;
                }

                // Close exactly once on every path and only publish the backup
                // if the final close also succeeded.
                if (fclose (file_owner.release()) != 0) {
                    saved = false;
                }

                if (saved) {
                    savednames.push_back(g_strdup (c));

                    // Attempt to add the emergency save to the recent files, so users can find it on restart
                    try {
                        if (on_main_thread) {
                            Inkscape::IO::addInkscapeRecentSvg(c, docname, {"Crash"}, document_filename ? document_filename : "");
                        }
                    } catch (std::exception &e) {
                        // Recent-file bookkeeping must not abort remaining documents.
                    }
                } else {
                    failednames.push_back((doc->getDocumentName()) ? g_strdup(doc->getDocumentName()) : g_strdup (_("Untitled document")));
                }
            } else {
                failednames.push_back((doc->getDocumentName()) ? g_strdup(doc->getDocumentName()) : g_strdup (_("Untitled document")));
            }
            count++;
        }
    }
    g_free(curdir);

    if (!savednames.empty()) {
        fprintf (stderr, "\nEmergency save document locations:\n");
        for (auto i:savednames) {
            fprintf (stderr, "  %s\n", i);
        }
    }
    if (!failednames.empty()) {
        fprintf (stderr, "\nFailed to do emergency save for documents:\n");
        for (auto i:failednames) {
            fprintf (stderr, "  %s\n", i);
        }
    }

    fprintf (stderr, "Emergency save completed. VA Studio will close now.\n");
    // Reports go to VA Studio, not to the upstream Inkscape tracker (vacards-public-identity.h).
    fprintf (stderr, "If you can reproduce this crash, please report it to VA Studio with a detailed\n"
                     "description of the steps leading to the crash, so we can fix it:\n  %s\n",
             Inkscape::VACards::support_destination().c_str());

    // Signal handlers execute on the faulting thread. GTK (including the
    // recent manager above) must never be entered from a canvas render worker.
    // Return just as the normal handler does, letting the restored handler take
    // over when the faulting instruction is retried.
    if (!on_main_thread) {
        for (auto name : savednames) g_free(name);
        for (auto name : failednames) g_free(name);
        tracker.clear();
        Logger::shutdown();
        fflush(stderr);
        return;
    }

    /* Show nice dialog box */

    char const *istr = "";
    char const *sstr = _("Automatic backups of unsaved documents were done to the following locations:\n");
    char const *fstr = _("Automatic backup of the following documents failed:\n");
    gint nllen = strlen ("\n");
    gint len = strlen (istr) + strlen (sstr) + strlen (fstr);
    for (auto i:savednames) {
        len = len + SP_INDENT + strlen (i) + nllen;
    }
    for (auto i:failednames) {
        len = len + SP_INDENT + strlen (i) + nllen;
    }
    len += 1;
    gchar *b = g_new (gchar, len);
    gint pos = 0;
    len = strlen (istr);
    memcpy (b + pos, istr, len);
    pos += len;
    if (!savednames.empty()) {
        len = strlen (sstr);
        memcpy (b + pos, sstr, len);
        pos += len;
        for (auto i:savednames) {
            memset (b + pos, ' ', SP_INDENT);
            pos += SP_INDENT;
            len = strlen(i);
            memcpy (b + pos, i, len);
            pos += len;
            memcpy (b + pos, "\n", nllen);
            pos += nllen;
        }
    }
    if (!failednames.empty()) {
        len = strlen (fstr);
        memcpy (b + pos, fstr, len);
        pos += len;
        for (auto i:failednames) {
            memset (b + pos, ' ', SP_INDENT);
            pos += SP_INDENT;
            len = strlen(i);
            memcpy (b + pos, i, len);
            pos += len;
            memcpy (b + pos, "\n", nllen);
            pos += nllen;
        }
    }
    *(b + pos) = '\0';

    // Only offer the interactive handler when a live document window exists to
    // attach the dialog to. After shutdown has begun the desktop widget is
    // already gone; building and transientizing the dialog then dereferences null
    // and turns the original signal into a second, harder fault that hides the
    // real error.
    auto const *crash_desktop = SP_ACTIVE_DESKTOP;
    if ( exists() && instance().use_gui() && crash_desktop && crash_desktop->getDesktopWidget() ) {
        try {
            auto mainloop = Glib::MainLoop::create();
            auto builder = UI::create_builder("dialog-crash.glade");
            // Optional lookup: a missing label must not abort the crash dialog.
            if (auto const report = builder->get_widget<Gtk::Label>("report_link")) {
                report->set_markup(crash_report_markup());
            }
            auto &autosaves = UI::get_widget<Gtk::Label>(builder, "autosaves");
            if (std::strlen(b) == 0) {
                autosaves.set_visible(false);
            } else {
                autosaves.set_label(b);
            }
            UI::get_object<Gtk::TextBuffer>(builder, "stacktrace")->set_text("<pre>\n" + boost::stacktrace::to_string(boost::stacktrace::stacktrace()) + "</pre>\n<details><summary>System info</summary>\n" + debug_info() + "\n</details>");
            auto &window = UI::get_widget<Gtk::Window>(builder, "crash_dialog");
            auto &button_ok = UI::get_widget<Gtk::Button>(builder, "button_ok");
            button_ok.signal_clicked().connect([&] { window.close(); });
            button_ok.grab_focus();
            window.signal_close_request().connect([&] { mainloop->quit(); return false; }, true);
            // Make this dialog transient to help it not get lost (but not modal - if other
            // dialogs are open, having a mix of modal and not modal windows can have confusing
            // interactions on some window managers).
            sp_transientize(window);
            window.present();
            mainloop->run();
        } catch (const Glib::Error &ex) {
            g_message("Glade file loading failed for crash handler... Anyway, error was: %s", b);
            std::cerr << boost::stacktrace::stacktrace();
        }
    } else {
        g_message( "Error: %s", b );
        std::cerr << boost::stacktrace::stacktrace();
    }
    g_free (b);

    tracker.clear();
    Logger::shutdown();

    fflush(stderr); // make sure buffers are empty before crashing (otherwise output might be suppressed)

    /* on exit, allow restored signal handler to take over and crash us */
}

std::optional<Application::ConstructibleApplication> &Application::_get()
{
    static std::optional<Application::ConstructibleApplication> instance;
    return instance;
}

void Application::add_desktop(SPDesktop *desktop)
{
    g_return_if_fail(desktop);

    if (std::find(_desktops.begin(), _desktops.end(), desktop) != _desktops.end()) {
        g_error("Attempted to add desktop already in list.");
    }

    _desktops.insert(_desktops.begin(), desktop);
}

void Application::remove_desktop(SPDesktop *desktop)
{
    g_return_if_fail(desktop);

    if (std::find (_desktops.begin(), _desktops.end(), desktop) == _desktops.end() ) {
        g_error("Attempted to remove desktop not in list.");
    }

    if (desktop_is_active(desktop)) {
        if (_desktops.size() > 1) {
            SPDesktop * new_desktop = *(++_desktops.begin());
            _desktops.erase(std::find(_desktops.begin(), _desktops.end(), new_desktop));
            _desktops.insert(_desktops.begin(), new_desktop);
        } else {
            if (desktop->getSelection())
                desktop->getSelection()->clear();
        }
    }

    _desktops.erase(std::find(_desktops.begin(), _desktops.end(), desktop));
}

void Application::activate_desktop(SPDesktop *desktop)
{
    g_return_if_fail(desktop);

    if (desktop_is_active(desktop)) {
        return;
    }

    auto it = std::find(_desktops.begin(), _desktops.end(), desktop);
    if (it == _desktops.end()) {
        g_error("Tried to activate desktop not added to list.");
    }

    _desktops.erase(it);
    _desktops.insert(_desktops.begin(), desktop);
}

SPDesktop *
Application::find_desktop_by_dkey (unsigned int dkey)
{
    for (auto desktop : _desktops) {
        if (desktop->dkey == dkey) {
            return desktop;
        }
    }
    return nullptr;
}

unsigned int
Application::maximum_dkey()
{
    unsigned int dkey = 0;

    for (auto desktop : _desktops) {
        if (desktop->dkey > dkey) {
            dkey = desktop->dkey;
        }
    }
    return dkey;
}

SPDesktop *
Application::next_desktop ()
{
    SPDesktop *d = nullptr;
    unsigned int dkey_current = _desktops.front()->dkey;

    if (dkey_current < maximum_dkey()) {
        // find next existing
        for (unsigned int i = dkey_current + 1; i <= maximum_dkey(); ++i) {
            d = find_desktop_by_dkey (i);
            if (d) {
                break;
            }
        }
    } else {
        // find first existing
        for (unsigned int i = 0; i <= maximum_dkey(); ++i) {
            d = find_desktop_by_dkey (i);
            if (d) {
                break;
            }
        }
    }

    g_assert (d);
    return d;
}

SPDesktop *
Application::prev_desktop ()
{
    SPDesktop *d = nullptr;
    unsigned int dkey_current = (_desktops.front())->dkey;

    if (dkey_current > 0) {
        // find prev existing
        for (signed int i = dkey_current - 1; i >= 0; --i) {
            d = find_desktop_by_dkey (i);
            if (d) {
                break;
            }
        }
    }
    if (!d) {
        // find last existing
        d = find_desktop_by_dkey (maximum_dkey());
    }

    g_assert (d);
    return d;
}

void
Application::switch_desktops_next ()
{
    next_desktop()->presentWindow();
}

void
Application::switch_desktops_prev()
{
    prev_desktop()->presentWindow();
}

void Application::add_document(SPDocument *document)
{
    _document_set.emplace(document);
}

void Application::remove_document(SPDocument *document)
{
    _document_set.erase(document);
}

SPDesktop *
Application::active_desktop()
{
    if (_desktops.empty()) {
        return nullptr;
    }

    return _desktops.front();
}

SPDocument *
Application::active_document()
{
    if (SP_ACTIVE_DESKTOP) {
        return SP_ACTIVE_DESKTOP->getDocument();
    } else if (!_document_set.empty()) {
        // If called from the command line there will be no desktop
        // So 'fall back' to take the first listed document in the Inkscape instance
        return *_document_set.begin();
    }

    return nullptr;
}

} // namespace Inkscape

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
