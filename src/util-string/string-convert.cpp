// SPDX-License-Identifier: GPL-2.0-or-later

#include "string-convert.h"

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glib.h>

namespace Inkscape {

namespace {

static_assert(sizeof(wchar_t) == sizeof(gunichar) || sizeof(wchar_t) == sizeof(gunichar2));

[[noreturn]] void throw_conversion_error(GError *error)
{
    std::string message = error ? error->message : "Unicode conversion failed";
    if (error) {
        g_error_free(error);
    }
    throw std::range_error(message);
}

glong checked_length(std::size_t length)
{
    if (length > static_cast<std::size_t>(std::numeric_limits<glong>::max())) {
        throw std::length_error("String is too large for GLib Unicode conversion");
    }
    return static_cast<glong>(length);
}

template <typename T>
using GFreePtr = std::unique_ptr<T, decltype(&g_free)>;

} // namespace

std::wstring utf8_to_wstring(std::string const &str)
{
    GError *error = nullptr;
    glong written = 0;

    if constexpr (sizeof(wchar_t) == sizeof(gunichar)) {
        GFreePtr<gunichar> converted(
            g_utf8_to_ucs4(str.data(), checked_length(str.size()), nullptr, &written, &error), g_free);
        if (!converted) {
            throw_conversion_error(error);
        }

        std::wstring result;
        result.reserve(static_cast<std::size_t>(written));
        for (glong i = 0; i < written; ++i) {
            result.push_back(static_cast<wchar_t>(converted.get()[i]));
        }
        return result;
    } else {
        GFreePtr<gunichar2> converted(
            g_utf8_to_utf16(str.data(), checked_length(str.size()), nullptr, &written, &error), g_free);
        if (!converted) {
            throw_conversion_error(error);
        }

        std::wstring result;
        result.reserve(static_cast<std::size_t>(written));
        for (glong i = 0; i < written; ++i) {
            result.push_back(static_cast<wchar_t>(converted.get()[i]));
        }
        return result;
    }
}

std::string wstring_to_utf8(wchar_t const *wstr)
{
    if (!wstr) {
        throw std::invalid_argument("Cannot convert a null wide string");
    }

    auto const length = std::char_traits<wchar_t>::length(wstr);
    if (length == 0) {
        return {};
    }
    GError *error = nullptr;
    glong written = 0;
    gchar *raw = nullptr;

    if constexpr (sizeof(wchar_t) == sizeof(gunichar)) {
        std::vector<gunichar> input;
        input.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            auto const codepoint = static_cast<gunichar>(wstr[i]);
            if (!g_unichar_validate(codepoint)) {
                throw std::range_error("Wide string contains an invalid Unicode code point");
            }
            input.push_back(codepoint);
        }
        raw = g_ucs4_to_utf8(input.data(), checked_length(input.size()), nullptr, &written, &error);
    } else {
        std::vector<gunichar2> input;
        input.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            auto const code_unit = static_cast<gunichar2>(wstr[i]);
            if (code_unit >= 0xD800 && code_unit <= 0xDBFF) {
                if (i + 1 >= length) {
                    throw std::range_error("Wide string ends with an unpaired high surrogate");
                }
                auto const low = static_cast<gunichar2>(wstr[i + 1]);
                if (low < 0xDC00 || low > 0xDFFF) {
                    throw std::range_error("Wide string contains an unpaired high surrogate");
                }
            } else if (code_unit >= 0xDC00 && code_unit <= 0xDFFF &&
                       (i == 0 || static_cast<gunichar2>(wstr[i - 1]) < 0xD800 ||
                        static_cast<gunichar2>(wstr[i - 1]) > 0xDBFF)) {
                throw std::range_error("Wide string contains an unpaired low surrogate");
            }
            input.push_back(code_unit);
        }
        raw = g_utf16_to_utf8(input.data(), checked_length(input.size()), nullptr, &written, &error);
    }

    GFreePtr<gchar> converted(raw, g_free);
    if (!converted) {
        throw_conversion_error(error);
    }
    return {converted.get(), static_cast<std::size_t>(written)};
}

std::string unicode_char_to_utf8(uint32_t unicode)
{
    std::string result;
    result.resize(6);
    auto size = g_unichar_to_utf8(unicode, result.data());
    result.resize(size);
    return result;
}

} // namespace Inkscape
