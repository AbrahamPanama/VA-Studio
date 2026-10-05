// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Spell checking: UI gate.
 *
 * VA Studio build 31 hides the libspelling checker (owner decision, 2026-10-04): the Check Spelling dialog,
 * its menu, context-menu, search and shortcut entries, its Preferences page and the Font Browser's text
 * underlining. libspelling stays linked and packaged (WITH_LIBSPELLING is a required build feature), so
 * the gate is a compile-time constant here rather than a CMake flag.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_SPELLCHECK_FEATURE_H
#define INKSCAPE_UI_SPELLCHECK_FEATURE_H

namespace Inkscape::UI {

inline constexpr bool spellcheckUiEnabled = false;

} // namespace Inkscape::UI

#endif // INKSCAPE_UI_SPELLCHECK_FEATURE_H
