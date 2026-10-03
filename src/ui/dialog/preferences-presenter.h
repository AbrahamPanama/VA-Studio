// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Retained host / router for the native Preferences panel.
 */
/* Released under GNU GPL v2+, read the file 'COPYING' for more information. */

#ifndef INKSCAPE_UI_DIALOG_PREFERENCES_PRESENTER_H
#define INKSCAPE_UI_DIALOG_PREFERENCES_PRESENTER_H

#include <memory>

#include <sigc++/slot.h>
#include <sigc++/trackable.h>

class InkscapeApplication;
class SPDesktop;

namespace Gtk {
class Window;
}

namespace Inkscape::UI::Dialog {

class InkscapePreferences;

/**
 * Routes Preferences presentation to an existing native panel when one is
 * available, and otherwise retains a single owned host for cold zero-document
 * startup. One presenter belongs to exactly one InkscapeApplication.
 *
 * The presenter keeps no cached external widget/desktop pointers.
 */
class PreferencesPresenter final : public sigc::trackable
{
public:
    explicit PreferencesPresenter(InkscapeApplication &app);
    ~PreferencesPresenter();

    PreferencesPresenter(PreferencesPresenter const &) = delete;
    PreferencesPresenter &operator=(PreferencesPresenter const &) = delete;

    void present(Gtk::Window *parent = nullptr);
    void desktopChanged(SPDesktop *desktop);
    void desktopWillClose(SPDesktop *desktop);
    void shutdown();

    Gtk::Window *ownedWindow() const;
    bool focusOwnedIfPresent();

private:
    struct OwnedState;

    void revealOwned();

    InkscapeApplication &_app;

    /// Shared, not unique: every owned operation takes a local strong copy so a
    /// reentrant shutdown()/destructor can release the member without destroying
    /// the native host/panel while their callback frame is still executing.
    std::shared_ptr<OwnedState> _owned;

    /// Tracked, never a raw retained transient parent. Becomes empty when the
    /// parent Gtk::Window is destroyed.
    sigc::slot<Gtk::Window *()> _transient_parent;

    bool _terminated = false;
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_PREFERENCES_PRESENTER_H

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
