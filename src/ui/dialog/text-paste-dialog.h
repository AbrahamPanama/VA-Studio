// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Ask how to format text that is being pasted
 */
/* Authors:
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_DIALOG_TEXT_PASTE_DIALOG_H
#define INKSCAPE_UI_DIALOG_TEXT_PASTE_DIALOG_H

#include <optional>

#include "ui/clipboard.h"

class SPDesktop;

namespace Inkscape::UI::Dialog {

/**
 * What the user chose in a paste-formatting question.
 *
 * The dialog returns this decision and never writes preferences: the caller
 * revalidates the destination and the clipboard, inserts, and only then commits
 * a remembered choice. An accept-then-stale-destination is an abort that changes
 * nothing, including preferences.
 */
struct TextPasteChoice {
    TextPasteMode mode = TextPasteMode::Automatic;
    bool remember = false; ///< "Remember this choice ... and don't ask again" was ticked
};

/**
 * Ask which formatting should be used for a rich text paste (native fragment).
 *
 * Shows the three paste modes with @p initial preselected and an optional
 * "Remember this choice" checkbox. Returns the decision, or std::nullopt when
 * the user cancels or closes the dialog. Widget names are unchanged for the
 * existing GUI automation contract: window `text-paste-dialog`, radios
 * `text-paste-mode-automatic|source|destination`, `text-paste-remember`,
 * buttons `text-paste-accept` / `text-paste-cancel`.
 */
std::optional<TextPasteChoice> choose_text_paste_mode(SPDesktop *desktop, TextPasteMode initial);

/**
 * Ask how external rich text (HTML/RTF) from another application is pasted.
 *
 * Two options only - Keep source formatting / Use destination formatting - with
 * an independent "Remember this choice for external text and don't ask again"
 * checkbox. @p initial must already be resolved by the caller (Automatic is not
 * an option here). Distinct window name `text-paste-external-dialog`, radios
 * `text-paste-external-mode-source|destination`, remember
 * `text-paste-external-remember`; the accept/cancel button names are shared.
 */
std::optional<TextPasteChoice> choose_external_text_paste_mode(SPDesktop *desktop, TextPasteMode initial);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_TEXT_PASTE_DIALOG_H

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
