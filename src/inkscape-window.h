// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Inkscape - An SVG editor.
 */
/*
 * Authors:
 *   Tavmjong Bah
 *
 * Copyright (C) 2018 Authors
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 * Read the file 'COPYING' for more information.
 *
 */

#ifndef INKSCAPE_WINDOW_H
#define INKSCAPE_WINDOW_H

#include <glibmm/refptr.h>
#include <gdkmm/toplevel.h>
#include <gtkmm/applicationwindow.h>

#include <sigc++/scoped_connection.h>
#include <giomm/action.h>
#include <memory>
#include <utility>
#include <vector>

namespace Gtk { class Box; }

class InkscapeApplication;
class SPDocument;
class SPDesktop;
class SPDesktopWidget;

class InkscapeWindow : public Gtk::ApplicationWindow
{
private:
    class LifetimeState;

public:
    /**
     * Non-owning, owner-thread-only handle to the lifetime of the native window
     * that produced it. The opaque token owns no widget; it only weakly observes
     * the window's private lifetime state and compares native identity. Explicit
     * app/C++ teardown invalidates it before the existing destruction callbacks
     * run. The raw gtk_window_destroy boundary is native top-level-model removal,
     * not the earlier unmap, so ordinary hide/show stays valid. A
     * default-constructed token is invalid. Document/tab membership is tracked
     * separately and is not implied here.
     */
    class LifetimeToken
    {
    public:
        bool valid() const noexcept;

    private:
        friend class InkscapeWindow;

        std::weak_ptr<LifetimeState> _state;
    };

    InkscapeWindow(SPDesktop *desktop);
    ~InkscapeWindow() override;

    /** Return a non-owning token observing this window's lifetime. */
    LifetimeToken lifetimeToken() const noexcept;

    /**
     * Mark this window's lifetime as ended. Idempotent, allocation-free and
     * non-throwing; safe to call before the window's actions are cleared.
     */
    void invalidateLifetime() noexcept;

    SPDocument*      get_document()       { return _document; }
    SPDesktop*       get_desktop()        { return _desktop; }
    SPDesktopWidget* get_desktop_widget() { return _desktop_widget; }
    // Owning application, captured at construction. Exposes the existing _app
    // field so dialog routing can address the receiving window's owner directly
    // instead of consulting a process singleton.
    InkscapeApplication* get_application_owner() const noexcept { return _app; }
    void change_document(SPDocument* document);

    Gdk::Toplevel::State get_toplevel_state() const;

    bool isFullscreen() const;
    bool isMaximised() const;
    bool isMinimised() const;

    void toggleFullscreen();

    void setActiveTab(SPDesktop *desktop);

private:
    InkscapeApplication *_app = nullptr;
    SPDocument*          _document = nullptr;
    SPDesktop*           _desktop = nullptr;
    std::shared_ptr<LifetimeState> _lifetime_state;
    SPDesktopWidget*     _desktop_widget = nullptr;
    Glib::RefPtr<Gtk::ShortcutController> _shortcut_controller;

    void add_document_actions();
    void clear_document_actions();
    std::vector<std::pair<Glib::ustring, Glib::RefPtr<Gio::Action>>> _document_action_mirrors;
    bool _document_actions_attached = false;
    bool _window_destroyed = false;
    sigc::scoped_connection _destroy_connection;
    sigc::scoped_connection _shortcuts_changed_connection;

    sigc::scoped_connection _toplevel_state_connection;
    Gdk::Toplevel::State _old_toplevel_state{};

    void on_realize() override;
    Glib::RefPtr<Gdk::Toplevel const> get_toplevel() const;
    void on_toplevel_state_changed();
    void on_is_active_changed();
    bool on_close_request() override;
    void on_size_changed();

    void update_dialogs();
};

#endif // INKSCAPE_WINDOW_H

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
