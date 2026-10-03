// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * VIEW-2: guide the user to turn off PowerToys' SVG add-ons on Windows, so
 * File Explorer uses VA Studio's SVG thumbnails and preview (VIEW-1).
 *
 * PowerToys registers its SVG handlers for the user, which takes precedence
 * over VA Studio's machine-wide ones. VA Studio never edits PowerToys'
 * settings: it detects which handler is in effect, shows numbered steps with
 * a button that opens PowerToys at its File Explorer add-ons page, and marks
 * each step done when the change is in effect (owner decisions 2026-09-28).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_SVG_VIEWER_GUIDE_H
#define INKSCAPE_UI_SVG_VIEWER_GUIDE_H

#include <functional>
#include <string>

class SPDesktop;

namespace Gtk {
class Window;
}

namespace Inkscape::UI::SvgViewerGuide {

/// The handler File Explorer uses for .svg files (one kind: thumbnail or preview).
struct Handler {
    std::string clsid;  ///< e.g. {3A05ACA6-...}; empty when none is registered
    std::string server; ///< its InprocServer32 path; may be empty
};

struct State {
    Handler thumbnail;
    Handler preview;
    bool vastudio_registered = false; ///< VA Studio's shell extension is installed
    bool icons_only = false;          ///< File Explorer: "Always show icons, never thumbnails"
    std::string powertoys;            ///< PowerToys.exe, when found
};

enum class Owner { None, VaStudio, PowerToys, Other };

Owner owner_of(Handler const &handler);

/// True when VA Studio's viewer is installed but PowerToys handles SVG files,
/// or File Explorer is set to show icons only.
bool needs_guide(State const &state);

/// Where the user is in the guide.
struct Progress {
    bool preview_off = false;   ///< PowerToys no longer handles the preview pane
    bool thumbnail_off = false; ///< PowerToys no longer handles thumbnails
    bool thumbnails_shown = true; ///< File Explorer shows thumbnails (not icons only)
    enum class Result { Pending, Done, NotActive } result = Result::Pending;
};

/// Done: VA Studio handles both and File Explorer shows thumbnails.
/// NotActive: PowerToys is off for both but VA Studio's handlers are not the
/// ones in effect.
Progress progress(State const &state);

/// The handlers in effect for this user (Windows); an empty state elsewhere.
State read_state();

/// Show the guide, reading the state with \a read (read_state by default).
/// Returns the guide window (owned by GTK; it deletes itself when closed).
Gtk::Window *show(Gtk::Window *parent, std::function<State()> read = {});

/// Windows, once per session, a moment after the first window appears: show
/// the guide when needs_guide() and the user has not asked not to see it.
void offer_at_startup(SPDesktop *desktop);

/// Preference set by "Don't show this again".
inline constexpr char const *dismissed_pref = "/options/vacards/svg-viewer-guide/dismissed";

} // namespace Inkscape::UI::SvgViewerGuide

#endif // INKSCAPE_UI_SVG_VIEWER_GUIDE_H
