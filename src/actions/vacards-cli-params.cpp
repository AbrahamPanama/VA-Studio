// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: parameter grammar implementation (design 5.2).
 *
 * Locale-independent: doubles go through g_ascii_strtod and integers through
 * std::from_chars; no std::stod/strtod/atof/stringstream is used for numbers.
 */

#include "vacards-cli-params.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include <glib.h>

namespace Inkscape::VACardsCli {

namespace {

bool is_ascii_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

unsigned hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a') + 10u;
    }
    return static_cast<unsigned>(c - 'A') + 10u;
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// key := [a-z][a-z0-9-]*, ASCII only.
bool valid_key(std::string_view key)
{
    if (key.empty()) {
        return false;
    }
    char const first = key.front();
    if (!(first >= 'a' && first <= 'z')) {
        return false;
    }
    for (std::size_t i = 1; i < key.size(); ++i) {
        char const c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

ParseError err_empty_pair(std::string_view action)
{
    return {"empty-pair", {}, "Empty parameter in " + std::string(action) + "."};
}

ParseError err_bad_key(std::string_view action)
{
    return {"bad-key", {}, "Malformed parameter name in " + std::string(action) + "."};
}

ParseError err_unknown_key(std::string_view action, std::string_view key)
{
    return {"unknown-key", std::string(key),
            "Unknown parameter '" + std::string(key) + "' for " + std::string(action) + "."};
}

ParseError err_repeated_key(std::string_view action, std::string_view key)
{
    return {"repeated-key", std::string(key),
            "Parameter '" + std::string(key) + "' is given more than once for " + std::string(action) + "."};
}

ParseError err_missing_value(std::string_view action, std::string_view key)
{
    return {"missing-value", std::string(key),
            "Parameter '" + std::string(key) + "' of " + std::string(action) + " needs a value."};
}

ParseError err_empty_value(std::string_view action, std::string_view key)
{
    return {"empty-value", std::string(key),
            "Parameter '" + std::string(key) + "' of " + std::string(action) + " has an empty value."};
}

ParseError err_bad_percent(std::string_view action, std::string_view key)
{
    return {"bad-percent-encoding", std::string(key),
            "Parameter '" + std::string(key) + "' of " + std::string(action)
                + " has a malformed percent escape."};
}

ParseError err_malformed(std::string_view action, std::string_view key, ParamType type, std::string_view value)
{
    return {"malformed-value", std::string(key),
            "Parameter '" + std::string(key) + "' of " + std::string(action) + " is not a valid "
                + std::string(type_name(type)) + ": '" + std::string(value) + "'."};
}

ParseError err_out_of_range(std::string_view action, std::string_view key, std::string_view value)
{
    return {"out-of-range", std::string(key),
            "Parameter '" + std::string(key) + "' of " + std::string(action)
                + " is out of range: '" + std::string(value) + "'."};
}

ParseError err_not_a_choice(std::string_view action, ParamSpec const &param)
{
    std::string choices;
    for (std::size_t i = 0; i < param.choices.size(); ++i) {
        if (i != 0) {
            choices += ", ";
        }
        choices += std::string(param.choices[i]);
    }
    return {"not-a-choice", std::string(param.key),
            "Parameter '" + std::string(param.key) + "' of " + std::string(action)
                + " must be one of: " + choices + "."};
}

ParseError err_missing_required(std::string_view action, std::string_view key)
{
    return {"missing-required", std::string(key),
            "Parameter '" + std::string(key) + "' is required by " + std::string(action) + "."};
}

ParseError err_bad_default(std::string_view action, std::string_view key)
{
    return {"bad-default", std::string(key),
            "Built-in default of '" + std::string(key) + "' for " + std::string(action) + " is invalid."};
}

// Scanner: [sign] digits* [ '.' digits* ] [ ('e'|'E') [sign] digits+ ]
// At least one digit must appear before or after the '.'. A trailing 'e'
// without exponent digits is left in the suffix.
bool scan_number(std::string_view text, std::string &number_out, std::string_view &suffix_out)
{
    std::size_t const n = text.size();
    std::size_t pos = 0;
    if (pos < n && (text[pos] == '+' || text[pos] == '-')) {
        ++pos;
    }

    std::size_t digits = 0;
    while (pos < n && is_digit(text[pos])) {
        ++pos;
        ++digits;
    }
    if (pos < n && text[pos] == '.') {
        ++pos;
        while (pos < n && is_digit(text[pos])) {
            ++pos;
            ++digits;
        }
    }
    if (digits == 0) {
        return false;
    }

    std::size_t number_end = pos;
    if (pos < n && (text[pos] == 'e' || text[pos] == 'E')) {
        std::size_t p = pos + 1;
        if (p < n && (text[p] == '+' || text[p] == '-')) {
            ++p;
        }
        std::size_t exp_digits = 0;
        while (p < n && is_digit(text[p])) {
            ++p;
            ++exp_digits;
        }
        if (exp_digits > 0) {
            number_end = p;
        }
    }

    number_out.assign(text.substr(0, number_end));
    suffix_out = text.substr(number_end);
    return true;
}

bool parse_number_text(std::string const &number_text, double &out)
{
    char *end = nullptr;
    double const value = g_ascii_strtod(number_text.c_str(), &end);
    if (end != number_text.c_str() + number_text.size()) {
        return false;
    }
    out = value;
    return true;
}

bool length_factor(std::string_view suffix, double &factor)
{
    if (suffix.empty() || suffix == "px") {
        factor = 1.0;
    } else if (suffix == "mm") {
        factor = 96.0 / 25.4;
    } else if (suffix == "cm") {
        factor = 96.0 / 2.54;
    } else if (suffix == "in") {
        factor = 96.0;
    } else if (suffix == "pt") {
        factor = 96.0 / 72.0;
    } else if (suffix == "pc") {
        factor = 16.0;
    } else {
        return false;
    }
    return true;
}

bool duration_factor(std::string_view suffix, double &factor)
{
    if (suffix == "ms") {
        factor = 1.0;
    } else if (suffix == "s") {
        factor = 1000.0;
    } else if (suffix == "m") {
        factor = 60000.0;
    } else {
        return false;
    }
    return true;
}

std::optional<ParseError> convert(ParamSpec const &param, std::string_view value_text,
                                  std::string_view action, ParamValue &out)
{
    out = ParamValue{};
    out.type = param.type;

    if (param.type == ParamType::List) {
        std::vector<std::string> items;
        std::size_t start = 0;
        while (true) {
            std::size_t const bar = value_text.find('|', start);
            std::string_view const item = bar == std::string_view::npos
                                              ? value_text.substr(start)
                                              : value_text.substr(start, bar - start);
            if (item.empty()) {
                return err_empty_value(action, param.key);
            }
            auto decoded = percent_decode(item);
            if (!decoded) {
                return err_bad_percent(action, param.key);
            }
            items.push_back(std::move(*decoded));
            if (bar == std::string_view::npos) {
                break;
            }
            start = bar + 1;
        }
        auto raw = percent_decode(value_text);
        if (!raw) {
            return err_bad_percent(action, param.key);
        }
        out.items = std::move(items);
        out.raw = std::move(*raw);
        return std::nullopt;
    }

    auto decoded_opt = percent_decode(value_text);
    if (!decoded_opt) {
        return err_bad_percent(action, param.key);
    }
    std::string const decoded = std::move(*decoded_opt);
    out.raw = decoded;

    switch (param.type) {
    case ParamType::Boolean:
        if (decoded == "true" || decoded == "1") {
            out.boolean = true;
        } else if (decoded == "false" || decoded == "0") {
            out.boolean = false;
        } else {
            return err_malformed(action, param.key, param.type, value_text);
        }
        return std::nullopt;

    case ParamType::Integer: {
        std::size_t i = 0;
        if (i < decoded.size() && (decoded[i] == '+' || decoded[i] == '-')) {
            ++i;
        }
        std::size_t const digits_start = i;
        while (i < decoded.size() && is_digit(decoded[i])) {
            ++i;
        }
        if (i != decoded.size() || i == digits_start) {
            return err_malformed(action, param.key, param.type, value_text);
        }
        char const *first = decoded.data();
        if (decoded[0] == '+') {
            ++first;
        }
        long long value = 0;
        auto const parsed = std::from_chars(first, decoded.data() + decoded.size(), value);
        if (parsed.ec == std::errc::result_out_of_range) {
            return err_out_of_range(action, param.key, value_text);
        }
        if (parsed.ec != std::errc()) {
            return err_malformed(action, param.key, param.type, value_text);
        }
        if (param.min && static_cast<double>(value) < *param.min) {
            return err_out_of_range(action, param.key, value_text);
        }
        if (param.max && static_cast<double>(value) > *param.max) {
            return err_out_of_range(action, param.key, value_text);
        }
        out.integer = value;
        return std::nullopt;
    }

    case ParamType::Number:
    case ParamType::Length:
    case ParamType::Angle:
    case ParamType::Duration: {
        std::string number_text;
        std::string_view suffix;
        if (!scan_number(decoded, number_text, suffix)) {
            return err_malformed(action, param.key, param.type, value_text);
        }
        double n = 0.0;
        if (!parse_number_text(number_text, n)) {
            return err_malformed(action, param.key, param.type, value_text);
        }
        if (!std::isfinite(n)) {
            return err_out_of_range(action, param.key, value_text);
        }
        double stored = 0.0;
        switch (param.type) {
        case ParamType::Number:
            if (!suffix.empty()) {
                return err_malformed(action, param.key, param.type, value_text);
            }
            stored = n;
            break;
        case ParamType::Length: {
            double factor = 1.0;
            if (!length_factor(suffix, factor)) {
                return err_malformed(action, param.key, param.type, value_text);
            }
            stored = n * factor;
            break;
        }
        case ParamType::Angle:
            if (!(suffix.empty() || suffix == "deg")) {
                return err_malformed(action, param.key, param.type, value_text);
            }
            stored = n;
            break;
        case ParamType::Duration: {
            double factor = 1.0;
            if (!duration_factor(suffix, factor)) {
                return err_malformed(action, param.key, param.type, value_text);
            }
            stored = n * factor;
            break;
        }
        default:
            break;
        }
        if (param.min && stored < *param.min) {
            return err_out_of_range(action, param.key, value_text);
        }
        if (param.max && stored > *param.max) {
            return err_out_of_range(action, param.key, value_text);
        }
        out.number = stored;
        return std::nullopt;
    }

    case ParamType::Choice:
        for (auto const choice : param.choices) {
            if (choice == decoded) {
                out.text = decoded;
                return std::nullopt;
            }
        }
        return err_not_a_choice(action, param);

    case ParamType::Text:
        out.text = decoded;
        return std::nullopt;

    case ParamType::List:
        break;
    }
    return err_malformed(action, param.key, param.type, value_text);
}

} // namespace

ParseResult parse_params(ActionSpec const &spec, std::string_view input)
{
    ParseResult result;
    std::vector<std::string> seen;

    // Steps 1-3: split into pairs and process them left to right; the first
    // structural or value error wins.
    if (!input.empty()) {
        std::size_t start = 0;
        while (true) {
            std::size_t const comma = input.find(',', start);
            std::string_view const pair =
                comma == std::string_view::npos ? input.substr(start) : input.substr(start, comma - start);

            if (pair.empty()) {
                result.error = err_empty_pair(spec.name);
                return result;
            }

            // 3a: first '=' separates key from value; otherwise the pair is bare.
            std::size_t const eq = pair.find('=');
            std::string_view key;
            std::string_view value;
            bool const bare = eq == std::string_view::npos;
            if (bare) {
                key = pair;
            } else {
                key = pair.substr(0, eq);
                value = pair.substr(eq + 1);
            }

            // 3b: syntax.
            if (!valid_key(key)) {
                result.error = err_bad_key(spec.name);
                return result;
            }

            // 3c: known parameter.
            ParamSpec const *param = nullptr;
            for (auto const &candidate : spec.params) {
                if (candidate.key == key) {
                    param = &candidate;
                    break;
                }
            }
            if (!param) {
                result.error = err_unknown_key(spec.name, key);
                return result;
            }

            // 3d: not repeated.
            bool repeated = false;
            for (auto const &done : seen) {
                if (done == key) {
                    repeated = true;
                    break;
                }
            }
            if (repeated) {
                result.error = err_repeated_key(spec.name, key);
                return result;
            }

            // 3e/3f: value text.
            std::string value_text;
            if (bare) {
                if (param->type != ParamType::Boolean) {
                    result.error = err_missing_value(spec.name, key);
                    return result;
                }
                value_text = "true";
            } else {
                if (value.empty()) {
                    result.error = err_empty_value(spec.name, key);
                    return result;
                }
                value_text.assign(value);
            }

            seen.emplace_back(key);

            // 3g: convert.
            ParamValue parsed_value;
            auto error = convert(*param, value_text, spec.name, parsed_value);
            if (error) {
                result.error = std::move(error);
                return result;
            }
            std::string const raw = parsed_value.raw;
            result.values[std::string(key)] = std::move(parsed_value);
            result.given.emplace_back(std::string(key), raw);

            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
    }

    // Step 4: required parameters, then defaults, in spec order.
    for (auto const &param : spec.params) {
        bool given = false;
        for (auto const &done : seen) {
            if (done == param.key) {
                given = true;
                break;
            }
        }
        if (given) {
            continue;
        }
        if (param.required) {
            result.error = err_missing_required(spec.name, param.key);
            return result;
        }
        if (!param.default_value.empty()) {
            ParamValue parsed_value;
            auto error = convert(param, param.default_value, spec.name, parsed_value);
            if (error) {
                result.error = err_bad_default(spec.name, param.key);
                return result;
            }
            parsed_value.from_default = true;
            result.values[std::string(param.key)] = std::move(parsed_value);
        }
    }

    return result;
}

std::optional<std::string> percent_decode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char const c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size()) {
                return std::nullopt;
            }
            char const hi = text[i + 1];
            char const lo = text[i + 2];
            if (!is_ascii_hex(hi) || !is_ascii_hex(lo)) {
                return std::nullopt;
            }
            out.push_back(static_cast<char>(hex_value(hi) * 16u + hex_value(lo)));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string_view type_name(ParamType type)
{
    switch (type) {
    case ParamType::Boolean:
        return "boolean";
    case ParamType::Integer:
        return "integer";
    case ParamType::Number:
        return "number";
    case ParamType::Length:
        return "length";
    case ParamType::Angle:
        return "angle";
    case ParamType::Duration:
        return "duration";
    case ParamType::Choice:
        return "choice";
    case ParamType::Text:
        return "text";
    case ParamType::List:
        return "list";
    }
    return "";
}

std::string_view stored_unit(ParamType type)
{
    switch (type) {
    case ParamType::Length:
        return "px";
    case ParamType::Angle:
        return "deg";
    case ParamType::Duration:
        return "ms";
    default:
        return "";
    }
}

} // namespace Inkscape::VACardsCli

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
