// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Authors:
 *   See Git history
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "font-utils.h"

#include <iostream> // TEMP TEMP TEMP
#include <glibmm/regex.h>
#include <pango/pango-font.h>
#include "io/sys.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace Inkscape {

bool font_directory_allowed_at_startup(char const *path)
{
    if (!path || !*path) return false;
    bool remote = g_str_has_prefix(path, "\\\\") || g_str_has_prefix(path, "//");
#ifdef _WIN32
    if (!remote) {
        // GetDriveTypeW reads the drive type, without probing files on the share.
        // Resolve relative/root-relative paths so mapped drives are covered too.
        auto wide = g_utf8_to_utf16(path, -1, nullptr, nullptr, nullptr);
        if (!wide) return false;
        auto const length = GetFullPathNameW(reinterpret_cast<wchar_t const *>(wide), 0, nullptr, nullptr);
        std::vector<wchar_t> absolute(length + 1);
        auto const written = GetFullPathNameW(reinterpret_cast<wchar_t const *>(wide), absolute.size(), absolute.data(), nullptr);
        g_free(wide);
        if (!written || written >= absolute.size()) return false;
        if (absolute[0] == L'\\' && absolute[1] == L'\\') {
            remote = true;
        } else if (written >= 3 && absolute[1] == L':') {
            wchar_t const drive[] = {absolute[0], L':', L'\\', L'\0'};
            remote = GetDriveTypeW(drive) == DRIVE_REMOTE;
        }
    }
#endif
    if (remote) {
        g_warning("Remote fonts dir '%s' skipped at startup. Copy the fonts to a local folder "
                  "and add that folder in Preferences > Text > Additional font directories.", path);
        return false;
    }
    if (!Inkscape::IO::file_test(path, G_FILE_TEST_IS_DIR)) {
        g_info("Fonts dir '%s' does not exist and will be ignored.", path);
        return false;
    }
    return true;
}

// Pass fontspec to and back from Pango to get a the fontspec in canonical form. TEST
Glib::ustring canonize_fontspec(Glib::ustring const &fontspec)
{
    PangoFontDescription *descr = pango_font_description_from_string(fontspec.c_str());

    // CSS Font Module Level 4 dictates that "font-weight", "font-width' ("font-stretch"),
    // "font-style", and "font-optical-sizing" shoud be used rather than the
    // "font-variation-settings" axes 'wght', 'wdth', 'slnt'/'ital', or 'opsz'.
    // At the moment (May 2026), Pango only provides adequate support for doing so for
    // font weight.
    //
    // Pango font description does not order the axes. This can cause problems when
    // trying to match named instances. Reorder alphabetically here.
    std::string variations;

    const char* str = pango_font_description_get_variations(descr);
    if (str) {
        auto variations_map = parse_variations(str);
        for (auto [tag, value] : variations_map) {
            if (tag == "wght") {
                auto weight = std::stoi(value);
                pango_font_description_set_weight(descr, (PangoWeight)weight);
            } else if (tag == "ital" && value == "1") {
                pango_font_description_set_style(descr, PANGO_STYLE_ITALIC);
            } else {
                variations += tag;
                variations += "=";
                variations += value;
                variations += ",";
            }
        }

        if (variations.length() >= 1) { // Remove last comma and save
            variations.pop_back();
        }

        pango_font_description_set_variations(descr, variations.c_str());
    }

    gchar *canonized = pango_font_description_to_string(descr);
    Glib::ustring Canonized = canonized;
    g_free(canonized);
    pango_font_description_free(descr);

    // Pango canonized strings remove space after comma between family names. Put it back.
    // But don't add a space inside a 'font-variation-settings' declaration (this breaks Pango).
    size_t i = 0;
    while ((i = Canonized.find_first_of(",@", i)) != std::string::npos ) {
        if (Canonized[i] == '@') // Found start of 'font-variation-settings'.
            break;
        Canonized.replace(i, 1, ", ");
        i += 2;
    }

    return Canonized;
}


// Returns a map of 'tag' => 'value' from a variations string. TEST
std::map<std::string, std::string> parse_variations(const char* variations)
{
    std::map<std::string, std::string> variations_map;

    auto regex = Glib::Regex::create("(\\w{4})=([-+]?\\d*\\.?\\d+([eE][-+]?\\d+)?)");
    Glib::MatchInfo matchInfo;

    std::vector<Glib::ustring> tokens = Glib::Regex::split_simple(",", variations);
    for (auto const &token : tokens) {
        regex->match(token, matchInfo);
        if (matchInfo.matches()) {
            auto tag   = matchInfo.fetch(1).raw();
            auto value = matchInfo.fetch(2).raw();
            variations_map[tag] = value; // This will alphabetize axes based on tag.
        }
    }

    return variations_map;
}

} // namespace Inkscape

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
