// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EXP-1: which formats the Export dialog offers, and in what order (per user).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "export-formats.h"

#include <algorithm>
#include <glibmm/i18n.h>
#include <initializer_list>

#include "extension/db.h"
#include "extension/output.h"
#include "preferences.h"

namespace Inkscape::UI::ExportFormats {

namespace {

std::vector<std::string> split(Glib::ustring const &text)
{
    std::vector<std::string> ids;
    std::string current;
    for (char c : text.raw()) {
        if (c == ',') {
            if (!current.empty()) {
                ids.push_back(current);
            }
            current.clear();
        } else if (c != ' ') {
            current += c;
        }
    }
    if (!current.empty()) {
        ids.push_back(current);
    }
    return ids;
}

std::string join(std::vector<std::string> const &ids)
{
    std::string text;
    for (auto const &id : ids) {
        if (!text.empty()) {
            text += ',';
        }
        text += id;
    }
    return text;
}

bool one_of(std::string const &value, std::initializer_list<char const *> candidates)
{
    return std::any_of(candidates.begin(), candidates.end(), [&](char const *candidate) { return value == candidate; });
}

std::ptrdiff_t index_of(std::vector<std::string> const &ids, std::string const &id)
{
    auto const found = std::find(ids.begin(), ids.end(), id);
    return found == ids.end() ? -1 : found - ids.begin();
}

} // namespace

std::vector<std::string> const &default_shown()
{
    static std::vector<std::string> const ids{
        "org.inkscape.output.png.inkscape",       // PNG
        "org.inkscape.raster.tiff_output",        // TIFF
        "org.inkscape.raster.jpg_output",         // JPEG
        "org.inkscape.output.pdf.cairorenderer",  // PDF
        "org.inkscape.output.svg.inkscape",       // VA Studio SVG
        "org.inkscape.output.svg.plain",          // Plain SVG
    };
    return ids;
}

Group group_of(Inkscape::Extension::Output const &output)
{
    std::string const id = output.get_id() ? output.get_id() : "";
    std::string extension = Glib::ustring(output.get_extension() ? output.get_extension() : "").lowercase();
    if (id.find("jessyink") != std::string::npos || id.find("synfig") != std::string::npos ||
        id == "org.inkscape.output.gif.animated") {
        return Group::Animation;
    }
    if (one_of(extension, {".png", ".jpg", ".jpeg", ".tif", ".tiff", ".webp", ".gif", ".bmp", ".ico", ".xcf"})) {
        return Group::Raster;
    }
    if (one_of(extension, {".tex", ".pov", ".html", ".htm", ".txt"})) {
        return Group::Text;
    }
    if (one_of(extension, {".svg", ".svgz", ".pdf", ".ps", ".eps", ".emf", ".wmf", ".dxf", ".hpgl", ".plt", ".fxg",
                           ".odg", ".ai", ".xaml", ".zip", ".tar", ".sla"})) {
        return Group::Vector;
    }
    return Group::Other;
}

Glib::ustring group_label(Group group)
{
    switch (group) {
    case Group::Raster: return _("Raster");
    case Group::Vector: return _("Vector");
    case Group::Text: return _("Text");
    case Group::Animation: return _("Animation");
    case Group::Other: break;
    }
    return _("Other");
}

std::vector<Format> formats()
{
    auto *prefs = Inkscape::Preferences::get();
    auto const order = split(prefs->getString(order_pref));
    bool const configured = prefs->getEntry(shown_pref).isSet();
    auto const shown = configured ? split(prefs->getString(shown_pref)) : default_shown();

    Inkscape::Extension::DB::OutputList outputs;
    Inkscape::Extension::db.get_output_list(outputs);
    std::vector<Format> result;
    std::vector<std::ptrdiff_t> rank;
    for (auto *output : outputs) {
        if (!output || output->deactivated() || !output->get_id()) {
            continue;
        }
        Format format;
        format.id = output->get_id();
        format.name = output->get_filetypename() ? output->get_filetypename() : format.id;
        format.extension = output->get_extension() ? output->get_extension() : "";
        format.group = group_of(*output);
        format.shown = index_of(shown, format.id) >= 0;
        // The user's order first, then the short list's order, then the
        // extension database order for formats never ordered before.
        auto position = index_of(order, format.id);
        if (position < 0) {
            auto const preferred = index_of(default_shown(), format.id);
            position = preferred >= 0 ? static_cast<std::ptrdiff_t>(order.size()) + preferred
                                      : static_cast<std::ptrdiff_t>(order.size() + default_shown().size() + result.size());
        }
        rank.push_back(position);
        result.push_back(std::move(format));
    }
    std::vector<std::size_t> indices(result.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
        indices[i] = i;
    }
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
        if (result[a].group != result[b].group) {
            return result[a].group < result[b].group;
        }
        return rank[a] < rank[b];
    });
    std::vector<Format> sorted;
    sorted.reserve(result.size());
    for (auto index : indices) {
        sorted.push_back(result[index]);
    }
    return sorted;
}

void save(std::vector<Format> const &formats)
{
    std::vector<std::string> order, shown;
    for (auto const &format : formats) {
        order.push_back(format.id);
        if (format.shown) {
            shown.push_back(format.id);
        }
    }
    auto *prefs = Inkscape::Preferences::get();
    // A format not available right now (for example an extension missing a
    // dependency) keeps its check box, and its place at the end of the order.
    if (prefs->getEntry(shown_pref).isSet()) {
        for (auto const &id : split(prefs->getString(order_pref))) {
            if (index_of(order, id) < 0) {
                order.push_back(id);
            }
        }
        for (auto const &id : split(prefs->getString(shown_pref))) {
            if (index_of(shown, id) < 0 && std::none_of(formats.begin(), formats.end(),
                                                        [&id](Format const &format) { return format.id == id; })) {
                shown.push_back(id);
            }
        }
    }
    prefs->setString(order_pref, join(order));
    prefs->setString(shown_pref, join(shown));
}

void restore_short_list()
{
    auto *prefs = Inkscape::Preferences::get();
    prefs->setString(order_pref, join(default_shown()));
    prefs->setString(shown_pref, join(default_shown()));
}

void reset()
{
    auto *prefs = Inkscape::Preferences::get();
    for (auto const *path : {order_pref, shown_pref}) {
        if (prefs->getEntry(path).isSet()) {
            prefs->remove(path);
        }
    }
}

} // namespace Inkscape::UI::ExportFormats
