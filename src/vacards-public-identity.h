// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VA Studio public identity: the one place every "report a problem" link uses.
 *
 * The support URL is an owner decision recorded as VA_SUPPORT_URL in
 * packaging/public-release/public-release.env. CMake validates it (https only,
 * no quotes or spaces) and writes the generated vacards-public-identity-config.h.
 *
 * While the URL is unset, callers must not fall back to the upstream Inkscape
 * bug tracker or to a placeholder address. support_destination() and
 * support_uri() then name the SUPPORT.md installed in the documentation folder
 * (share/inkscape/doc), which explains how to obtain support; if even that file
 * is missing, they return its bare name so the message stays readable.
 */
#ifndef VACARDS_PUBLIC_IDENTITY_H
#define VACARDS_PUBLIC_IDENTITY_H

#include <string>

#include <glibmm/convert.h>

#include "io/resource.h"
#include "vacards-public-identity-config.h" // generated: VACARDS_PUBLIC_SUPPORT_URL

namespace Inkscape::VACards {

/// The configured public support URL, or "" while VA_SUPPORT_URL is unset.
inline constexpr char const *public_support_url = VACARDS_PUBLIC_SUPPORT_URL;

/// Installed document that explains how to get support (the neutral fallback).
inline constexpr char const *support_document = "SUPPORT.md";

inline bool has_public_support_url()
{
    return public_support_url[0] != '\0';
}

/// Where to report a problem, for plain text: the URL, else the local SUPPORT.md path.
inline std::string support_destination()
{
    if (has_public_support_url()) {
        return public_support_url;
    }
    auto local = IO::Resource::get_filename(IO::Resource::DOCS, support_document, false, true);
    return local.empty() ? std::string(support_document) : local;
}

/// The same destination as a link target: the URL, else a file:// URI of SUPPORT.md.
/// Returns "" when there is nothing to link to (the caller shows text only).
inline std::string support_uri()
{
    if (has_public_support_url()) {
        return public_support_url;
    }
    auto local = IO::Resource::get_filename(IO::Resource::DOCS, support_document, false, true);
    if (local.empty()) {
        return {};
    }
    try {
        return Glib::filename_to_uri(local).raw();
    } catch (Glib::ConvertError const &) {
        return {};
    }
}

} // namespace Inkscape::VACards

#endif // VACARDS_PUBLIC_IDENTITY_H
