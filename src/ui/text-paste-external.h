// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Origin / representation / capability model and rich-representation
 * dispatch for external clipboard text paste.
 *
 * The legacy reader carried one `bool native` that conflated three different
 * facts: where the payload came from (origin), which representation produced it,
 * and whether the decoded fragment can carry rich formatting. This header makes
 * them explicit and keeps the shared platform aliases in common code, so the
 * macOS `text/html` / `text/rtf` spellings and the Windows
 * `application/x.windows.HTML Format` / `application/x.windows.Rich Text Format`
 * spellings are handled by one implementation. The Windows spellings are derived
 * from the pinned GTK 4.22.4 source (gdkclipdrop-win32.c) and are **not**
 * host-verified here: absence of a Windows host is reported, not hidden.
 */

#ifndef INKSCAPE_UI_TEXT_PASTE_EXTERNAL_H
#define INKSCAPE_UI_TEXT_PASTE_EXTERNAL_H

#include <string>
#include <string_view>
#include <vector>

#include "ui/text-paste.h"

namespace Inkscape::UI::TextPaste {

enum class Origin {
    Native,   ///< this application published the payload
    External, ///< any other clipboard owner
};

enum class Representation {
    Native, ///< validated native VA fragment
    Html,   ///< external HTML (bare document or CF_HTML envelope)
    Rtf,    ///< external RTF
    Plain,  ///< interoperable plain text
};

/** Deterministic representation preference: native > HTML > RTF > plain. */
inline constexpr Representation REPRESENTATION_PREFERENCE[] = {
    Representation::Native, Representation::Html, Representation::Rtf, Representation::Plain,
};

/** MIME spellings for the HTML representation, in request preference order. */
std::vector<std::string> const &html_mime_aliases();
/** MIME spellings for the RTF representation, in request preference order. */
std::vector<std::string> const &rtf_mime_aliases();

/** Outcome of decoding one external rich representation. */
struct ExternalDecode {
    bool usable = false;         ///< a non-empty validated fragment was produced
    bool limit_exceeded = false; ///< a declared resource cap was hit: abort the whole paste
    bool malformed = false;      ///< under every cap but unusable: use the complete plain alternative
    Fragment fragment;
    bool has_meaningful_styles = false; ///< formatting worth asking the user about
    std::vector<std::string> losses;    ///< bounded reason codes, never user-facing
    std::string error;
};

/**
 * Decode @a bytes as @a representation through the shared validated
 * construction path. Native and Plain are not handled here (the clipboard
 * reader keeps its existing strict parsers for those).
 */
ExternalDecode decode_external(std::string_view bytes, Representation representation);

} // namespace Inkscape::UI::TextPaste

#endif // INKSCAPE_UI_TEXT_PASTE_EXTERNAL_H
