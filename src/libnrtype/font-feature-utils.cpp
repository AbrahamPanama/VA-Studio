// SPDX-License-Identifier: GPL-2.0-or-later

#include "font-feature-utils.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace Inkscape {
namespace {

std::string trim(std::string value)
{
    auto const not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string feature_tag(std::string const &entry)
{
    auto text = trim(entry);
    if (text.empty()) return {};
    if (text.front() == '\'' || text.front() == '"') {
        auto const quote = text.front();
        auto const end = text.find(quote, 1);
        return end == std::string::npos ? std::string{} : text.substr(1, end - 1);
    }
    auto const end = text.find_first_of(" =\t");
    return text.substr(0, end);
}

std::optional<bool> feature_value(std::string const &entry)
{
    auto text = trim(entry);
    auto const separator = text.find_first_of("= ");
    if (separator == std::string::npos) return true;
    auto value = trim(text.substr(separator + 1));
    if (!value.empty() && value.front() == '=') value = trim(value.substr(1));
    if (value.empty() || value == "on" || value == "1") return true;
    if (value == "off" || value == "0") return false;
    return std::nullopt;
}

std::vector<std::string> split_features(Glib::ustring const &settings)
{
    std::vector<std::string> result;
    auto raw = settings.raw();
    if (raw.empty() || raw == "normal") return result;
    std::stringstream stream(raw);
    for (std::string entry; std::getline(stream, entry, ',');) {
        entry = trim(std::move(entry));
        if (!entry.empty()) result.emplace_back(std::move(entry));
    }
    return result;
}

} // namespace

Glib::ustring merge_font_feature(Glib::ustring const &settings, std::string_view tag,
                                 std::optional<bool> enabled)
{
    auto entries = split_features(settings);
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](auto const &entry) {
        return feature_tag(entry) == tag;
    }), entries.end());
    if (enabled) entries.emplace_back(std::string(tag) + (*enabled ? " 1" : " 0"));
    if (entries.empty()) return "normal";

    Glib::ustring result;
    for (auto const &entry : entries) {
        if (!result.empty()) result += ", ";
        result += entry;
    }
    return result;
}

std::optional<bool> query_font_feature(Glib::ustring const &settings, std::string_view tag)
{
    for (auto const &entry : split_features(settings)) {
        if (feature_tag(entry) == tag) return feature_value(entry);
    }
    return std::nullopt;
}

} // namespace Inkscape
