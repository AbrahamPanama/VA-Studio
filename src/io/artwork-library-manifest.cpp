// SPDX-License-Identifier: GPL-2.0-or-later

// Compile the JSON implementation from the already-required Boost headers in
// this one TU. No extra shared library, package download or runtime is used.
#define BOOST_JSON_NO_LIB
#include <boost/json/src.hpp>
#include <boost/json/basic_parser_impl.hpp>

#include "artwork-library-manifest.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace Inkscape::IO::ArtworkLibrary {
namespace {
namespace json = boost::json;
using ErrorCode = boost::system::error_code;
constexpr auto format = "org.vacards.artwork-library";

[[noreturn]] void invalid(char const *reason) { throw std::runtime_error(reason); }
void check_cancel(Cancelled const &cancelled)
{
    if (cancelled && cancelled()) invalid("Artwork library operation cancelled");
}

std::string decimal_value(std::string_view token)
{
    // Compare decimal values, not their spelling (0.1 and 1E-1 are equal).
    // Values outside the lossless JSON numeric subset belong in string metadata.
    if (token.size() > 128) invalid("Artwork metadata number is too long");
    auto const negative = !token.empty() && token.front() == '-';
    if (negative) token.remove_prefix(1);
    auto const e = token.find_first_of("eE");
    auto mantissa = token.substr(0, e);
    auto const dot = mantissa.find('.');
    int fraction = dot == std::string_view::npos ? 0 : static_cast<int>(mantissa.size() - dot - 1);
    std::string digits;
    for (auto c : mantissa) if (c != '.') digits += c;
    auto const first = digits.find_first_not_of('0');
    if (first == std::string::npos) return "0";
    digits.erase(0, first);
    int exponent = 0;
    if (e != std::string_view::npos) {
        auto power = token.substr(e + 1);
        if (!power.empty() && power.front() == '+') power.remove_prefix(1);
        auto parsed = std::from_chars(power.data(), power.data() + power.size(), exponent);
        if (parsed.ec != std::errc{} || parsed.ptr != power.data() + power.size() ||
            exponent < -10000 || exponent > 10000) invalid("Artwork metadata exponent is out of range");
    }
    exponent -= fraction;
    while (digits.back() == '0') { digits.pop_back(); ++exponent; }
    return (negative ? "-" : "") + digits + "e" + std::to_string(exponent);
}

// Boost's DOM deliberately accepts duplicate members. Reject them before DOM
// construction so escaped spellings and nested metadata cannot ambiguously
// override a version, asset ID or hash for a different reader.
struct UniqueKeys {
    static constexpr std::size_t max_object_size = 256;
    static constexpr std::size_t max_array_size = 10000;
    static constexpr std::size_t max_key_size = 128;
    static constexpr std::size_t max_string_size = 4096;
    Cancelled cancelled;
    std::vector<std::set<std::string>> keys;
    std::string partial;
    std::string number_partial;
    explicit UniqueKeys(Cancelled value) : cancelled(std::move(value)) {}
    bool tick() { check_cancel(cancelled); return true; }
    bool on_document_begin(ErrorCode &) { return tick(); }
    bool on_document_end(ErrorCode &) { return tick(); }
    bool on_object_begin(ErrorCode &) { keys.emplace_back(); return tick(); }
    bool on_object_end(std::size_t, ErrorCode &) { keys.pop_back(); return tick(); }
    bool on_array_begin(ErrorCode &) { return tick(); }
    bool on_array_end(std::size_t, ErrorCode &) { return tick(); }
    bool on_key_part(json::string_view part, std::size_t, ErrorCode &)
    { partial.append(part.data(), part.size()); return tick(); }
    bool on_key(json::string_view part, std::size_t, ErrorCode &ec)
    {
        partial.append(part.data(), part.size());
        auto const unique = keys.back().insert(std::exchange(partial, {})).second;
        if (!unique) ec = json::error::syntax;
        return unique && tick();
    }
    bool on_string_part(json::string_view, std::size_t, ErrorCode &) { return tick(); }
    bool on_string(json::string_view, std::size_t, ErrorCode &) { return tick(); }
    bool on_number_part(json::string_view part, ErrorCode &)
    {
        if (part.size() > 128 || number_partial.size() > 128 - part.size())
            invalid("Artwork metadata number is too long");
        number_partial.append(part.data(), part.size());
        return tick();
    }
    bool on_int64(std::int64_t, json::string_view, ErrorCode &) { number_partial.clear(); return tick(); }
    bool on_uint64(std::uint64_t, json::string_view, ErrorCode &) { number_partial.clear(); return tick(); }
    bool on_double(double value, json::string_view part, ErrorCode &ec)
    {
        on_number_part(part, ec);
        auto token = std::exchange(number_partial, {});
        if (!std::isfinite(value) || decimal_value(token) != decimal_value(json::serialize(json::value(value))))
            invalid("Artwork metadata number cannot be preserved without loss; use a string");
        return tick();
    }
    bool on_bool(bool, ErrorCode &) { return tick(); }
    bool on_null(ErrorCode &) { return tick(); }
    bool on_comment_part(json::string_view, ErrorCode &) { return tick(); }
    bool on_comment(json::string_view, ErrorCode &) { return tick(); }
};

json::object read_object(std::string_view input, ManifestLimits const &limits, Cancelled const &cancelled)
{
    if (input.size() > limits.bytes) invalid("Artwork library manifest exceeds byte limit");
    check_cancel(cancelled);
    json::parse_options options;
    options.max_depth = 16;
    options.numbers = json::number_precision::precise;
    ErrorCode ec;
    json::basic_parser<UniqueKeys> validator(options, cancelled);
    validator.write_some(false, input.data(), input.size(), ec);
    if (ec) invalid("Invalid or ambiguous artwork library JSON");
    // parse(), unlike basic_parser::write_some(), also rejects trailing data.
    auto value = json::parse(json::string_view(input.data(), input.size()), ec, {}, options);
    check_cancel(cancelled);
    if (ec || !value.is_object()) invalid("Artwork library JSON must be a single object");
    return std::move(value.as_object());
}

json::value take(json::object &object, char const *key)
{
    auto found = object.find(key);
    if (found == object.end()) invalid("Missing required artwork library field");
    auto value = std::move(found->value());
    object.erase(found);
    return value;
}

std::string text(json::value const &value)
{
    if (!value.is_string()) invalid("Artwork library text field has the wrong type");
    return std::string(value.as_string());
}

std::uint64_t integer(json::value const &value)
{
    if (value.is_uint64()) return value.as_uint64();
    if (value.is_int64() && value.as_int64() >= 0) return value.as_int64();
    invalid("Artwork library version or revision must be an unsigned integer");
}

double dimension(json::value const &value)
{
    double result = 0;
    if (value.is_double()) result = value.as_double();
    else if (value.is_int64()) result = value.as_int64();
    else if (value.is_uint64()) result = value.as_uint64();
    else invalid("Artwork library dimensions must be numbers");
    if (!std::isfinite(result) || result <= 0 || result > 1e9) invalid("Invalid artwork library dimensions");
    return result;
}

void label(std::string const &value, std::size_t maximum)
{
    if (value.empty() || value.size() > maximum ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c == ' '; })) {
        invalid("Invalid artwork library name or tag");
    }
}

bool lower_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

void validate(Manifest const &manifest, ManifestLimits const &limits, Cancelled const &cancelled)
{
    if (!valid_library_uuid(manifest.id)) invalid("Invalid artwork library UUID");
    label(manifest.name, 1024);
    if (manifest.assets.size() > limits.assets || manifest.assets.size() > 10000)
        invalid("Too many artwork library assets");
    std::set<std::string> ids;
    for (auto const &asset : manifest.assets) {
        check_cancel(cancelled);
        if (!valid_library_uuid(asset.id) || !ids.insert(asset.id).second) invalid("Invalid or duplicate asset UUID");
        label(asset.name, 1024);
        if (asset.path != "assets/" + asset.id + ".svg") invalid("Invalid artwork asset path");
        if (asset.sha256.size() != 64 || !std::all_of(asset.sha256.begin(), asset.sha256.end(), lower_hex))
            invalid("Invalid artwork asset SHA-256");
        dimension(asset.width_mm);
        dimension(asset.height_mm);
        if (asset.tags.size() > 32) invalid("Too many artwork asset tags");
        std::set<std::string> tags;
        for (auto const &tag : asset.tags) {
            label(tag, 128);
            if (!tags.insert(tag).second) invalid("Duplicate artwork asset tag");
        }
    }
}

void put(json::object &object, char const *key, json::value value)
{
    if (object.contains(key)) invalid("Optional artwork metadata collides with a reserved field");
    object[key] = std::move(value);
}

void put(json::object &object, char const *key, std::string const &value)
{
    put(object, key, json::value(value));
}
} // namespace

bool valid_library_uuid(std::string_view id) noexcept
{
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
        else if (!lower_hex(id[i])) return false;
    }
    return true;
}

Manifest Manifest::parse(std::string_view input, ManifestLimits limits, Cancelled cancelled)
{
    auto object = read_object(input, limits, cancelled);
    if (text(take(object, "format")) != format) invalid("Not a VACards artwork library");
    if (integer(take(object, "formatVersion")) != 1) invalid("Unsupported artwork library format version");
    Manifest result;
    result.id = text(take(object, "id"));
    result.name = text(take(object, "name"));
    result.revision = integer(take(object, "revision"));
    auto assets = take(object, "assets");
    if (!assets.is_array() || assets.as_array().size() > limits.assets) invalid("Invalid artwork library asset list");
    result.assets.reserve(assets.as_array().size());
    for (auto &value : assets.as_array()) {
        check_cancel(cancelled);
        if (!value.is_object()) invalid("Artwork library asset must be an object");
        auto &entry = value.as_object();
        Asset asset;
        asset.id = text(take(entry, "id"));
        asset.name = text(take(entry, "name"));
        asset.path = text(take(entry, "path"));
        asset.sha256 = text(take(entry, "sha256"));
        asset.width_mm = dimension(take(entry, "widthMm"));
        asset.height_mm = dimension(take(entry, "heightMm"));
        auto tags = take(entry, "tags");
        if (!tags.is_array() || tags.as_array().size() > 32) invalid("Invalid artwork asset tags");
        for (auto const &tag : tags.as_array()) asset.tags.push_back(text(tag));
        asset.extra_json = json::serialize(entry);
        result.assets.push_back(std::move(asset));
    }
    result.extra_json = json::serialize(object);
    validate(result, limits, cancelled);
    return result;
}

std::string Manifest::serialize(ManifestLimits limits, Cancelled cancelled) const
{
    validate(*this, limits, cancelled);
    auto object = read_object(extra_json, limits, cancelled);
    put(object, "format", json::value(format));
    put(object, "formatVersion", 1);
    put(object, "id", id);
    put(object, "name", name);
    put(object, "revision", revision);
    // Bound the aggregate before building/serializing a potentially huge DOM
    // from caller-created metadata, not only after allocating its output.
    auto used = json::serialize(object).size();
    auto account = [&](std::size_t bytes) {
        if (bytes > limits.bytes || used > limits.bytes - bytes)
            invalid("Artwork library manifest exceeds byte limit");
        used += bytes;
    };
    account(12); // assets key, delimiters, and conservative punctuation budget
    json::array entries;
    entries.reserve(assets.size());
    for (auto const &asset : assets) {
        check_cancel(cancelled);
        auto entry = read_object(asset.extra_json, limits, cancelled);
        put(entry, "id", asset.id);
        put(entry, "name", asset.name);
        put(entry, "path", asset.path);
        put(entry, "sha256", asset.sha256);
        put(entry, "widthMm", asset.width_mm);
        put(entry, "heightMm", asset.height_mm);
        json::array tags;
        for (auto const &tag : asset.tags) tags.emplace_back(tag);
        put(entry, "tags", std::move(tags));
        account(json::serialize(entry).size());
        if (!entries.empty()) account(1);
        entries.push_back(std::move(entry));
    }
    put(object, "assets", std::move(entries));
    auto result = json::serialize(object);
    // Also validate caller-created UTF-8 and aggregate depth/byte limits.
    (void)read_object(result, limits, cancelled);
    return result;
}

} // namespace Inkscape::IO::ArtworkLibrary
