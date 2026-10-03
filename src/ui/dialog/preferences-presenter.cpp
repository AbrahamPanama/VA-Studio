// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Retained host / router for the native Preferences panel.
 */
/* Released under GNU GPL v2+, read the file 'COPYING' for more information. */

#include "preferences-presenter.h"

#include "config.h" // only include where actually required!

#include <string>

#include <glibmm/i18n.h>
#include <gtkmm/window.h>
#include <sigc++/adaptors/track_obj.h>

#include "desktop.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "ui/dialog/dialog-base.h"
#include "ui/dialog/dialog-container.h"
#include "ui/dialog/dialog-manager.h"
#include "ui/dialog/dialog-window.h"
#include "ui/dialog/inkscape-preferences.h"

namespace Inkscape::UI::Dialog {

namespace {

constexpr char const *kPreferencesDialog = "Preferences";

} // namespace

struct PreferencesPresenter::OwnedState final
{
    // Declared before window: members are destroyed in reverse declaration
    // order, so the host unroots the panel before the panel is destroyed.
    InkscapePreferences panel;
    Gtk::Window window;

    OwnedState()
    {
        window.set_name("VACardsPreferencesWindow");

        Glib::ustring title = VACARDS_PRODUCT_NAME;
        title += " - ";
        title += _("Preferences");
        window.set_title(title);

        // Native GTK close semantics: a close hides the host (so the same host
        // and panel are retained for a later reveal) and a transient-parent
        // cascade never destroys it. No custom close signal is needed.
        window.set_hide_on_close(true);
        window.set_destroy_with_parent(false);

        window.set_child(panel);
    }
};

PreferencesPresenter::PreferencesPresenter(InkscapeApplication &app)
    : _app(app)
{
}

PreferencesPresenter::~PreferencesPresenter()
{
    shutdown();
}

void PreferencesPresenter::present(Gtk::Window *parent)
{
    if (_terminated) {
        return;
    }

    // A null parent clears any previously tracked transient parent; the owned
    // host must never become its own transient parent.
    if (!parent) {
        _transient_parent = {};
    } else if (!_owned || parent != &_owned->window) {
        // Tracked, so no raw transient parent is retained.
        _transient_parent = sigc::track_object([parent] { return parent; }, *parent);
    }

    // 1. Retained owned host first.
    if (_owned) {
        revealOwned();
        return;
    }

    // The global DialogManager below unconditionally dereferences the current
    // Gtk application, so bail out before that when there is no GUI application.
    auto *gapp = _app.gtk_app();
    if (!gapp) {
        return;
    }

    // 2. Existing floating Preferences, including F12-hidden windows. The
    //    global DialogManager list is filtered to this exact creating
    //    application.
    DialogBase *borrowed = nullptr;
    for (auto *wnd : DialogManager::singleton().get_all_floating_dialog_windows()) {
        if (!wnd || wnd->get_application_owner() != &_app) {
            continue;
        }
        if (auto *container = wnd->get_container()) {
            if (auto *dlg = container->get_dialog(kPreferencesDialog)) {
                borrowed = dlg;
                break;
            }
        }
    }

    // 3. Existing docked Preferences across this application's native windows.
    if (!borrowed && gapp) {
        for (auto *w : gapp->get_windows()) {
            auto *win = dynamic_cast<InkscapeWindow *>(w);
            if (!win || !win->get_desktop()) {
                continue;
            }
            if (auto *container = win->get_desktop()->getContainer()) {
                if (auto *dlg = container->get_dialog(kPreferencesDialog)) {
                    borrowed = dlg;
                    break;
                }
            }
        }
    }

    if (borrowed) {
        // The app-window snapshot ends here. Focus the borrowed native panel
        // and make no further use of borrowed state.
        borrowed->focus_dialog();
        return;
    }

    // 4. Active desktop's native container creates/owns the panel.
    if (auto *desktop = _app.get_active_desktop()) {
        if (auto *container = desktop->getContainer()) {
            container->new_dialog(kPreferencesDialog);
            return;
        }
    }

    // 5. Cold zero-document startup owns a host. If real documents/desktops
    //    exist but none is active, do not fabricate a second panel.
    bool has_desktop = false;
    if (gapp) {
        for (auto *w : gapp->get_windows()) {
            auto *win = dynamic_cast<InkscapeWindow *>(w);
            if (win && win->get_desktop()) {
                has_desktop = true;
                break;
            }
        }
    }
    if (has_desktop || !_app.get_documents().empty()) {
        return;
    }

    // Publish a reentrancy-safe first state. The tracked guard is captured
    // before construction; if constructing the native host reenters and
    // destroys the presenter, we must not touch `this` again.
    sigc::slot<bool()> guard = sigc::track_object([] { return true; }, *this);
    auto state = std::make_shared<OwnedState>();
    if (guard.empty()) {
        return;
    }
    if (_terminated || _owned) {
        // A reentrant path already published its own host; retain the first.
        return;
    }
    _owned = state;
    revealOwned();
}

void PreferencesPresenter::revealOwned()
{
    if (_terminated || !_owned) {
        return;
    }

    // Strong local ownership: keeps the native host and panel alive across
    // every callback below even if a reentrant shutdown()/destructor releases
    // the _owned member.
    auto state = _owned;

    // Local liveness slot tracks presenter, host and panel. `valid` checks the
    // slot FIRST so a destroyed presenter is detected before any `this` member
    // is read; then the terminal flag; then owned identity, so a
    // replaced/reentrant state is never touched.
    sigc::slot<bool()> alive = sigc::track_object([] { return true; }, *this, state->window, state->panel);
    auto valid = [&]() -> bool {
        return !alive.empty() && !_terminated && _owned == state;
    };
    if (!valid()) {
        return;
    }

    auto *gapp = _app.gtk_app();
    if (!gapp) {
        return;
    }

    // Hiding removes the window from the application; re-add before each reveal
    // and check actual membership. Never retarget or reparent an external panel.
    bool registered = false;
    for (auto *w : gapp->get_windows()) {
        if (w == &state->window) {
            registered = true;
            break;
        }
    }
    if (!valid()) {
        return;
    }

    if (!registered) {
        gapp->add_window(state->window);
    }
    if (!valid()) {
        return;
    }

    if (!_transient_parent.empty()) {
        if (auto *tracked_parent = _transient_parent(); tracked_parent && tracked_parent != &state->window) {
            state->window.set_transient_for(*tracked_parent);
        }
    } else {
        // No live tracked parent: drop any previous transient relationship.
        state->window.unset_transient_for();
    }
    if (!valid()) {
        return;
    }

    state->window.present();
    if (!valid()) {
        return;
    }

    // DialogBase::on_map binds the legacy active desktop; reapply the receiving
    // application's active desktop if still live.
    state->panel.setDesktop(_app.get_active_desktop());
    if (!valid()) {
        return;
    }

    // An already-visible owned host receives no map event, so the native
    // constructor's signal_map->showPage connection never fires; refresh the
    // panel to the page saved under "/dialogs/preferences/page" explicitly.
    state->panel.showPage();
    // showPage() may emit callbacks that terminate/replace the presenter; the
    // liveness check is mandatory before the final access below.
    if (!valid()) {
        return;
    }

    // Final reveal: nothing may touch `this` after this call.
    state->panel.focus_dialog();
}

void PreferencesPresenter::desktopChanged(SPDesktop *desktop)
{
    if (_terminated || !_owned) {
        return;
    }

    // Local strong state plus liveness slot retained across the final
    // setDesktop() callback (no owner use follows it).
    auto state = _owned;
    sigc::slot<bool()> alive = sigc::track_object([] { return true; }, *this, state->window, state->panel);
    if (alive.empty() || _terminated || _owned != state) {
        return;
    }

    // Only the owned panel is rebound; external panels remain owned by their
    // native containers.
    state->panel.setDesktop(desktop);
}

void PreferencesPresenter::desktopWillClose(SPDesktop *desktop)
{
    if (_terminated || !_owned) {
        return;
    }

    auto state = _owned;
    sigc::slot<bool()> alive = sigc::track_object([] { return true; }, *this, state->window, state->panel);
    if (alive.empty() || _terminated || _owned != state) {
        return;
    }

    if (state->panel.getDesktop() != desktop) {
        return;
    }

    // Local strong state retained across the final setDesktop() callback.
    state->panel.setDesktop(nullptr);
}

void PreferencesPresenter::shutdown()
{
    if (_terminated) {
        return;
    }
    _terminated = true;

    // Clear the tracked parent and move the owned state out BEFORE any external
    // callback, so reentrant paths observe no owned host.
    _transient_parent = {};

    auto state = std::move(_owned);
    _owned.reset();
    if (!state) {
        return;
    }

    // Grab the application pointer before any callback; from here on `this`
    // must not be accessed again.
    auto *gapp = _app.gtk_app();

    // Local shared state keeps host and panel alive to the end of this scope.
    state->window.hide();
    state->window.unset_child();

    if (gapp) {
        // Remove only this host's actual application membership.
        bool registered = false;
        for (auto *w : gapp->get_windows()) {
            if (w == &state->window) {
                registered = true;
                break;
            }
        }
        if (registered) {
            gapp->remove_window(state->window);
        }
    }

    // `state` destruction runs host window then panel (reverse declaration).
}

Gtk::Window *PreferencesPresenter::ownedWindow() const
{
    return _owned ? &_owned->window : nullptr;
}

bool PreferencesPresenter::focusOwnedIfPresent()
{
    if (_terminated || !_owned) {
        return false;
    }

    revealOwned();

    // Report the attempt even if a callback shut the presenter down, so callers
    // do not create a duplicate host.
    return true;
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
