// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: parameter grammar and specification tables.
 *
 * Every VACards CLI action takes one string parameter:
 *
 *   params := "" | pair ("," pair)*
 *   pair   := key "=" value | key          (a bare key means key=true; Boolean only)
 *   key    := [a-z][a-z0-9-]*
 *   value  := UTF-8 text, percent-encoded where needed (%2C for ',', %3D '=', %7C '|', %3B ';', %25 '%')
 *   list   := item ("|" item)*             (List parameters only)
 *
 * Unknown, repeated, missing-required, malformed and out-of-range values are
 * rejected with a specific code; values are never clamped. One static
 * ActionSpec per action drives the parser, vacards-describe and help text.
 */

#ifndef SEEN_VACARDS_CLI_PARAMS_H
#define SEEN_VACARDS_CLI_PARAMS_H

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Inkscape::VACardsCli {

struct ActionContext;
struct TypeDescriptor;

enum class ParamType
{
    Boolean,  ///< true|false|1|0; a bare key means true
    Integer,  ///< decimal integer with optional sign
    Number,   ///< decimal number without unit
    Length,   ///< number + optional unit px|mm|cm|in|pt|pc; stored in CSS px (bare number = px)
    Angle,    ///< number + optional "deg"; stored in degrees
    Duration, ///< number + required unit ms|s|m; stored in milliseconds
    Choice,   ///< exactly one of ParamSpec::choices (case-sensitive)
    Text,     ///< any non-empty percent-decoded text
    List,     ///< one or more non-empty percent-decoded items separated by '|'
};

/// One parameter of one action. Field order matters: specs use designated initializers.
struct ParamSpec
{
    std::string_view key;
    ParamType type = ParamType::Text;
    bool required = false;
    /// Raw text parsed exactly like user input when the key is absent; empty = no default.
    std::string_view default_value = {};
    /// Inclusive limits in stored units (px, degrees, ms). Integer/Number/Length/Angle/Duration only.
    std::optional<double> min = {};
    std::optional<double> max = {};
    /// Choice only.
    std::span<std::string_view const> choices = {};
    std::string_view help = {};
    TypeDescriptor const *descriptor = nullptr;
};

struct CommandLimits
{
    std::size_t request_bytes = 1048576;
    std::size_t response_bytes = 8388608;
    std::size_t id_bytes = 128;
};

/// Static description of one VACards CLI action.
struct ActionSpec
{
    std::string_view name;    ///< action name without the "app." prefix
    std::string_view mode;    ///< selection-contract mode, e.g. "read-only", "collective-geometry"
    std::string_view summary; ///< one English sentence
    std::span<ParamSpec const> params = {};
    std::string_view canonical_id = {};
    std::span<std::string_view const> aliases = {};
    void (*handler)(ActionContext &) = nullptr;
    unsigned version = 1;
    std::string_view effects = "read-only";
    std::string_view target_policy = "Q";
    std::string_view undo_policy = "none";
    std::string_view dry_run_grade = "computed";
    std::string_view cancellation_boundary = "before-handler";
    std::span<std::string_view const> constraints = {};
    std::span<std::string_view const> error_codes = {};
    std::span<std::string_view const> warning_codes = {};
    TypeDescriptor const *input = nullptr;
    TypeDescriptor const *result = nullptr;
    std::string_view example = {};
    bool needs_document = true;
    CommandLimits limits = {};
};

struct ParamValue
{
    ParamType type = ParamType::Text;
    std::string raw;            ///< decoded value text as given (or the default text)
    bool from_default = false;
    bool boolean = false;       ///< Boolean
    long long integer = 0;      ///< Integer
    double number = 0.0;        ///< Number; Length in px; Angle in degrees; Duration in ms
    std::string text;           ///< Choice, Text
    std::vector<std::string> items; ///< List
};

/// Error codes (stable, lowercase):
///   empty-pair, bad-key, unknown-key, repeated-key, missing-value, empty-value,
///   bad-percent-encoding, malformed-value, out-of-range, not-a-choice,
///   missing-required, bad-default
struct ParseError
{
    std::string code;
    std::string key;     ///< offending key; empty when not known (empty-pair, bad-key)
    std::string message; ///< one English sentence naming the key and action
};

struct ParseResult
{
    std::optional<ParseError> error; ///< set = rejected; values/given are then unspecified
    /// All parameters that have a value: given ones plus defaults.
    std::map<std::string, ParamValue, std::less<>> values;
    /// User-given parameters in input order: (key, decoded raw value; "true" for a bare key).
    std::vector<std::pair<std::string, std::string>> given;

    bool ok() const { return !error.has_value(); }
    bool has(std::string_view key) const { return values.find(key) != values.end(); }
    ParamValue const &at(std::string_view key) const { return values.find(key)->second; }
};

/// Parse one action parameter string against its specification.
ParseResult parse_params(ActionSpec const &spec, std::string_view input);

/// Decode %XX escapes (hex digits in either case). nullopt on a malformed escape.
std::optional<std::string> percent_decode(std::string_view text);

/// "boolean", "integer", "number", "length", "angle", "duration", "choice", "text", "list".
std::string_view type_name(ParamType type);

/// Stored unit of a type for describe output: "px", "deg", "ms", or "" for unitless types.
std::string_view stored_unit(ParamType type);

} // namespace Inkscape::VACardsCli

#endif // SEEN_VACARDS_CLI_PARAMS_H

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
