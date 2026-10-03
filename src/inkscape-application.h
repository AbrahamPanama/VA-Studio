// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The main Inkscape application.
 *
 * Copyright (C) 2018 Tavmjong Bah
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */
#ifndef INKSCAPE_APPLICATION_H
#define INKSCAPE_APPLICATION_H

#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtkmm/application.h>
#include <sigc++/scoped_connection.h>
#include <sigc++/signal.h>

#include "actions/actions-effect-data.h"
#include "actions/actions-extra-data.h"
#include "actions/actions-hint-data.h"
#include "io/file-export-cmd.h"   // File export (non-verb)
#include "extension/internal/pdfinput/enums.h"
#include "util/smart_ptr_keys.h"

namespace Gio {
class File;
} // namespace Gio

namespace Gtk {
class Window;
} // namespace Gtk

using action_vector_t = std::vector<std::pair<std::string, Glib::VariantBase>>;

class InkscapeWindow;
class SPDocument;
class SPDesktop;

namespace Inkscape {
class Selection;
namespace IO { class PublicationWorkerRegistry; }
namespace UI::Cache { class ArtworkLibraryThumbnailPool; }
namespace UI::Dialog {
class StartScreen;
class ArtworkLibraryHost;
class LibraryCloseGuard;
class PreferencesPresenter;
}
} // namespace Inkscape

class InkscapeApplication
{
public:
    /// Singleton instance.
    static InkscapeApplication *instance();

    /// Exclusively for the creation of the singleton instance inside main().
    InkscapeApplication();
    ~InkscapeApplication();

    /// The Gtk application instance, or NULL if running headless without display
    Gtk::Application *gtk_app() { return dynamic_cast<Gtk::Application *>(_gio_application.get()); }
    /// The Gio application instance, never NULL
    Gio::Application *gio_app() { return _gio_application.get(); }

    SPDesktop *createDesktop(SPDocument *document, bool replace, bool new_window = false);

    /// Request-specific result of create_window(). Callers that only need the side effects
    /// (window creation, error dialog, active-document update) may ignore the returned value.
    /// StartScreen uses it to distinguish success from cancellation or failure without
    /// consulting global active state.
    struct OpenResult {
        enum class Status { Opened, Cancelled, Failed };
        Status status = Status::Failed;
        SPDocument *document = nullptr; ///< The exact document for this request; valid only when opened().
        SPDesktop  *desktop  = nullptr; ///< The desktop created for this request, if any.
        bool opened() const { return status == Status::Opened; }
    };
    OpenResult create_window(Glib::RefPtr<Gio::File> const &file = {});
    bool destroyDesktop(SPDesktop *desktop, bool keep_alive = false);
    void detachDesktopToNewWindow(SPDesktop *desktop);
    bool destroy_all(bool *waiting = nullptr);
    void print_action_list();
    void print_input_type_list() const;

    InkFileExportCmd *file_export() { return &_file_export; }
    bool had_input_failure() const { return _input_failed; }
    int on_handle_local_options(const Glib::RefPtr<Glib::VariantDict> &options);
    void on_new();
    void on_quit(); // Check for data loss.
    void on_quit_immediate(); // Don't check for data loss.

    // Gio::Actions need to know what document, selection, desktop to work on.
    // In headless mode, these are set for each file processed.
    // With GUI, these are set everytime the cursor enters an InkscapeWindow.
    SPDocument*           get_active_document() { return _active_document; };
    void                  set_active_document(SPDocument* document) { _active_document = document; };

    Inkscape::Selection*  get_active_selection() { return _active_selection; }
    void                  set_active_selection(Inkscape::Selection *selection);

    // A desktop should track selection and canvas to document transform matrix. This is partially
    // redundant with the selection functions above.
    // Canvas to document transform matrix should be stored in the canvas, itself.
    SPDesktop*            get_active_desktop() { return _active_desktop; }
    void                  set_active_desktop(SPDesktop *desktop);

    // The currently focused window (nominally corresponding to _active_document).
    // A window must have a document but a document may have zero, one, or more windows.
    // This will replace _active_desktop.
    InkscapeWindow*       get_active_window() { return _active_window; }
    void                  set_active_window(InkscapeWindow* window) { _active_window = window; }

    /// Close Welcome once the active document window is really on screen (see definition).
    void                  closeStartScreenWhenShown() { _closeStartScreen(); }

    /****** Document ******/
    /* These should not require a GUI! */
    SPDocument *document_add(std::unique_ptr<SPDocument> document);

    SPDocument *document_new(std::string const &template_filename = {});
    std::pair<SPDocument *, bool /*cancelled*/> document_open(
        Glib::RefPtr<Gio::File> const &file, std::string *error = nullptr);
    SPDocument *document_open(std::span<char const> buffer);
    bool                  document_swap(SPDesktop *desktop, SPDocument *document);
    bool                  document_revert(SPDocument* document);
    void                  document_close(SPDocument* document, bool no_defer = false);
    void failNextDeferredDocumentCloseForTesting() { _fail_next_deferred_document_close = true; }
    unsigned documentHoldRefreshesForTesting() const { return _document_hold_refreshes; }
    Inkscape::IO::PublicationWorkerRegistry &publications();
    bool publicationInFlight(SPDocument const *document) const;
    bool waitForPublication(SPDocument *document);
    void flushSettledPublication(SPDocument *document);
    bool publicationWaitActive(SPDocument const *document) const noexcept;
    void publicationStarted(SPDocument *document, std::string destination);
    void publicationSettled(SPDocument *document, std::optional<bool> success = false);
    bool deferCloseForPublication(SPDocument *document, SPDesktop *desktop = nullptr);
    bool closePublicationWaitPending(SPDocument *document) const;
    bool hasPublicationRegistry() const noexcept { return static_cast<bool>(_publications); }
    // Synchronous operation lease for registered documents. Private document
    // owners must retain ownership until the operation returns themselves.
    std::function<void()> holdDocumentOperation(SPDocument *document);
    unsigned documentOperationCount() const { return _document_operation_count; }
    bool documentClosePending(SPDocument *document) const { return _pending_document_closes.contains(document); }
    // Retained library host; false is a read-only lookup, never dialog registration.
    std::shared_ptr<Inkscape::UI::Dialog::ArtworkLibraryHost> artworkLibraries(bool create = false);
    // Native application thread only. One pool for all slots/controllers and
    // synchronous callers; never recreated after final shutdown.
    std::shared_ptr<Inkscape::UI::Cache::ArtworkLibraryThumbnailPool> artworkLibraryThumbnailPool();
    sigc::connection connectArtworkLibraryThumbnailEnvironment(sigc::slot<void()> slot);
    void invalidateArtworkLibraryThumbnails(); // fonts/profiles/render environment, not panel close
    bool quitPending() const { return _quit_requested; }
    bool documentHoldActive() const { return _document_hold; }

    /* These require a GUI! */
    void                  document_fix(SPDesktop *desktop);

    std::vector<SPDocument *> get_documents();

    /******* Window *******/
    void startup_close();
    void windowClose(InkscapeWindow *window);

    // Application-owned native Preferences host. Created lazily on the first
    // present (never in headless mode) and reused for the application lifetime.
    void presentPreferences(Gtk::Window *parent = nullptr);
    // Read-only lookup of the retained owned host; never creates one.
    Gtk::Window *ownedPreferencesWindow() const;

    /******* Desktop *******/
    SPDesktop *desktopOpen(SPDocument *document, bool new_window = false);
    void desktopClose(SPDesktop *desktop);
    void desktopCloseActive();

    /****** Actions *******/
    InkActionExtraData&     get_action_extra_data()     { return _action_extra_data;  }
    InkActionEffectData&    get_action_effect_data()    { return _action_effect_data; }
    InkActionHintData&      get_action_hint_data()      { return _action_hint_data;   }
    std::map<std::string, Glib::ustring>& get_menu_label_to_tooltip_map() { return _menu_label_to_tooltip_map; };

    /******* Debug ********/
    void                  dump();

    int get_number_of_windows() const;

protected:
    Glib::RefPtr<Gio::Application> _gio_application;

    bool _with_gui    = true;
    bool _batch_process = false; // Temp
    bool _use_shell   = false;
    bool _use_pipe    = false;
    bool _auto_export = false;
    int _pdf_poppler  = false;
    FontStrategy _pdf_font_strategy = FontStrategy::RENDER_MISSING;
    bool _pdf_convert_colors = false;
    bool _use_command_line_argument = false;
    Glib::ustring _pages;
    std::string _pdf_group_by = "by-xobject";

    // Declared BEFORE documents/windows: owner state and pool survive their
    // natural member teardown. No destructor-driven event drain is required.
    struct LibraryThumbnailEnvironment;
    std::shared_ptr<LibraryThumbnailEnvironment> _libraryThumbnailEnvironment();
    std::thread::id const _library_thumbnail_thread = std::this_thread::get_id();
    std::shared_ptr<LibraryThumbnailEnvironment> _library_thumbnail_environment;
    std::unique_ptr<Inkscape::IO::PublicationWorkerRegistry> _publications;
    SPDocument *_publication_wait = nullptr;
    struct ClosePublicationWait;
    std::unordered_map<SPDocument *, std::unique_ptr<ClosePublicationWait>> _close_publication_waits;
    std::unordered_map<SPDocument *, std::string> _publication_destinations;
    void _endClosePublicationWait(SPDocument *document, int response);
    void _refreshQuitPublicationDialogs();
    void _showClosePublicationWaitDialog(SPDocument *document);
    std::vector<std::pair<SPDocument *, uint64_t>> _laterEditedWaits() const;
    std::vector<SPDocument *> _quitBlockingDocuments() const;
    [[noreturn]] void forceExitAbandoningPublications();

    // Documents are owned by the application which is responsible for opening/saving/exporting.
    std::unordered_map<std::unique_ptr<SPDocument>,
                       std::vector<std::unique_ptr<SPDesktop>>,
                       TransparentPtrHash<SPDocument>,
                       TransparentPtrEqual<SPDocument>> _documents;

    std::unordered_set<SPDocument *> _pending_document_closes;
    std::unordered_set<SPDocument *> _closing_documents;
    std::unordered_set<SPDesktop *> _closing_desktops;
    std::unordered_set<SPDocument *> _quit_waiting_documents;
    std::shared_ptr<InkscapeApplication *> _document_callback_lifetime =
        std::make_shared<InkscapeApplication *>(this);
    unsigned _document_operation_count = 0;
    bool _document_hold = false;
    unsigned _document_hold_refreshes = 0;
    bool _fail_next_deferred_document_close = false;
    bool _quit_requested = false;
    bool _quit_without_prompts = false;
    bool _destroying_all = false;
    bool _finishing_quit = false;
    sigc::scoped_connection _quit_cleanup_idle;
    std::shared_ptr<Inkscape::UI::Dialog::ArtworkLibraryHost> _library_host;
    std::unique_ptr<Inkscape::UI::Dialog::LibraryCloseGuard> _library_quit_guard;
    sigc::scoped_connection _library_changed;
    bool _library_hold = false;
    bool _welcome_shown = false;
    void _updateLibraryHold();
    void _updateDocumentHold();
    void _scheduleQuitContinuation();
    void _finishQuitWhenReady();

    std::vector<std::unique_ptr<InkscapeWindow>> _windows;

    // We keep track of these things so we don't need a window to find them (for headless operation).
    SPDocument*               _active_document   = nullptr;
    Inkscape::Selection*      _active_selection  = nullptr;
    sigc::scoped_connection   _active_selection_changed_connection;
    sigc::scoped_connection   _active_selection_modified_connection;
    SPDesktop*                _active_desktop       = nullptr;
    InkscapeWindow*           _active_window     = nullptr;

    InkFileExportCmd _file_export;

    // Actions from the command line or file.
    // Must read in on_handle_local_options() but parse in on_startup(). This is done as we must
    // have a valid app before initializing extensions which must be done before parsing.
    Glib::ustring _command_line_actions_input;
    action_vector_t _command_line_actions;

    // Extra data associated with actions (Label, Section, Tooltip/Help).
    InkActionExtraData  _action_extra_data;
    InkActionEffectData  _action_effect_data;
    InkActionHintData   _action_hint_data;
    // Needed due to the inabilitiy to get the corresponding Gio::Action from a Gtk::MenuItem.
    // std::string is used as key type because Glib::ustring has slow comparison and equality
    // operators.
    std::map<std::string, Glib::ustring> _menu_label_to_tooltip_map;
    void on_startup();
    void on_activate();
    void on_open(const Gio::Application::type_vec_files &files, const Glib::ustring &hint);
    void process_document(SPDocument* document, std::string output_path, bool new_window = false);
    void parse_actions(const Glib::ustring& input, action_vector_t& action_vector);

    void redirect_output();
    void shell(bool active_window = false);

    void _start_main_option_section(const Glib::ustring& section_name = "");
    
private:
    void init_extension_action_data();
    std::vector<Glib::RefPtr<Gio::SimpleAction>> _effect_actions;
    bool _no_extensions = false;
    bool _input_failed = false;

    // Lazily created by presentPreferences() only. Declared here so the complete
    // owner is shut down explicitly before the singleton/lifetime is cleared.
    std::unique_ptr<Inkscape::UI::Dialog::PreferencesPresenter> _preferences_presenter;

    void _openStartScreen();
    void _closeStartScreen();
    /// The toplevel most recently created by desktopOpen(); Welcome waits for its first show.
    InkscapeWindow *_window_awaiting_first_show = nullptr;
};

#endif // INKSCAPE_APPLICATION_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
