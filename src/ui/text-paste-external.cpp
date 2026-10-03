// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * @brief Origin / representation dispatch for external clipboard rich text.
 */

#include "ui/text-paste-external.h"

#include "ui/text-paste-html.h"
#include "ui/text-paste-rtf.h"

namespace Inkscape::UI::TextPaste {

std::vector<std::string> const &html_mime_aliases()
{
    // macOS/GTK maps public.html to "text/html"; Linux passes "text/html"
    // through. Windows/GTK exposes the registered "HTML Format" clipboard
    // format as "application/x.windows.HTML Format" (the name contains a space,
    // so GDK mangles it) and does NOT map it to text/html; the payload keeps its
    // CF_HTML byte-offset header, which the decoder sniffs and validates. Both
    // spellings live here, in shared code, so the platform difference is handled
    // by one implementation.
    // Windows is not host-verified on this machine: see the harness/report.
    static std::vector<std::string> const aliases{
        "text/html",
        "application/x.windows.HTML Format",
    };
    return aliases;
}

std::vector<std::string> const &rtf_mime_aliases()
{
    // macOS: public.rtf -> "text/rtf" (measured; application/rtf maps to a
    // dynamic UTI and is not an equivalent alias). Linux: "text/rtf".
    // Windows/GTK: the registered "Rich Text Format" name contains spaces and
    // surfaces as "application/x.windows.Rich Text Format"; a literal
    // "text/rtf" clipboard format is used as-is when a source publishes it.
    static std::vector<std::string> const aliases{
        "text/rtf",
        "application/x.windows.Rich Text Format",
        "application/rtf",
    };
    return aliases;
}

ExternalDecode decode_external(std::string_view bytes, Representation representation)
{
    ExternalDecode out;

    switch (representation) {
        case Representation::Html: {
            auto decoded = HtmlImport::decode(bytes);
            out.losses = std::move(decoded.losses);
            out.error = std::move(decoded.error);
            out.has_meaningful_styles = decoded.has_meaningful_styles;
            switch (decoded.status) {
                case HtmlImport::Status::ok:
                    out.fragment = std::move(decoded.fragment);
                    out.usable = !out.fragment.empty();
                    out.malformed = !out.usable;
                    break;
                case HtmlImport::Status::malformed:
                    out.malformed = true;
                    break;
                case HtmlImport::Status::limit_exceeded:
                    out.limit_exceeded = true;
                    break;
            }
            break;
        }
        case Representation::Rtf: {
            auto decoded = Rtf::decode(bytes);
            out.error = std::move(decoded.error);
            out.has_meaningful_styles = decoded.has_meaningful_styles;
            switch (decoded.status) {
                case Rtf::Status::ok:
                    out.fragment = std::move(decoded.fragment);
                    out.usable = !out.fragment.empty();
                    out.malformed = !out.usable;
                    break;
                case Rtf::Status::malformed:
                    out.malformed = true;
                    break;
                case Rtf::Status::over_limit:
                    out.limit_exceeded = true;
                    break;
            }
            break;
        }
        case Representation::Native:
        case Representation::Plain:
        default:
            out.malformed = true;
            out.error = "not an external rich representation";
            break;
    }

    if (out.usable && !out.has_meaningful_styles) {
        // A decoder may under-report; the shared policy is the final authority.
        out.has_meaningful_styles = has_meaningful_styles(out.fragment);
    }
    return out;
}

} // namespace Inkscape::UI::TextPaste
