// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-catalog.h"

#include <algorithm>
#include <glib.h>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace Inkscape::IO::ArtworkLibrary {
namespace detail {
struct CatalogRecord {
    Asset asset;
    std::string name_key;
    std::vector<std::string> tag_keys;
    std::shared_ptr<Bytes const> payload; // Null means lazily read the immutable base package.
    std::size_t metadata_bytes = 0;
};
struct CatalogSlot {
    std::shared_ptr<CatalogRecord const> record;
    bool removed = false;
};
struct CatalogState {
    Manifest header; // Library metadata only; assets are assembled in slot order.
    CatalogLimits limits;
    std::shared_ptr<Package const> base;
    std::vector<CatalogSlot> slots;
    std::map<std::string, std::size_t> indices;
    std::size_t overlay_bytes = 0;
    std::size_t metadata_bytes = 0;
};
} // namespace detail
namespace {
using detail::CatalogRecord;
using detail::CatalogState;
using GString = std::unique_ptr<gchar, decltype(&g_free)>;

void check_cancel(Cancelled const &cancelled)
{
    if (cancelled && cancelled()) throw std::runtime_error("Artwork catalog operation cancelled");
}

void account(std::size_t &total, std::size_t bytes, std::size_t maximum)
{
    if (bytes > maximum || total > maximum - bytes) throw std::runtime_error("Artwork catalog budget exceeded");
    total += bytes;
}

void account_string(std::size_t &total, std::string const &text, std::size_t maximum)
{
    account(total, text.capacity(), maximum);
    account(total, 1, maximum); // Terminator; conservative for small-string storage.
}

void account_array(std::size_t &total, std::size_t count, std::size_t element, std::size_t maximum)
{
    if (count > maximum / element) throw std::runtime_error("Artwork catalog budget exceeded");
    account(total, count * element, maximum);
}

Manifest canonical_header(Manifest const &source, CatalogLimits const &limits)
{
    if (source.id.size() > 36 || source.name.size() > 1024 || source.extra_json.size() > limits.manifest.bytes) {
        throw std::runtime_error("Artwork catalog metadata exceeds limits");
    }
    // Never move caller buffers: a short string or empty vector can own an
    // arbitrarily large reserve. Assets are deliberately not copied here.
    Manifest result;
    result.id.assign(source.id.data(), source.id.size());
    result.name.assign(source.name.data(), source.name.size());
    result.revision = source.revision;
    result.extra_json.assign(source.extra_json.data(), source.extra_json.size());
    return result;
}

std::size_t record_capacity(CatalogRecord const &record, std::size_t maximum)
{
    std::size_t total = 0;
    for (auto const *text : {&record.asset.id, &record.asset.name, &record.asset.path,
                            &record.asset.sha256, &record.asset.extra_json, &record.name_key}) {
        account_string(total, *text, maximum);
    }
    account_array(total, record.asset.tags.capacity(), sizeof(std::string), maximum);
    account_array(total, record.tag_keys.capacity(), sizeof(std::string), maximum);
    for (auto const &tag : record.asset.tags) account_string(total, tag, maximum);
    for (auto const &tag : record.tag_keys) account_string(total, tag, maximum);
    return total;
}

std::size_t metadata_capacity(CatalogState const &state, Cancelled const &cancelled = {})
{
    auto const maximum = state.limits.metadata_bytes;
    std::size_t total = 0;
    for (auto const *text : {&state.header.id, &state.header.name, &state.header.extra_json}) {
        account_string(total, *text, maximum);
    }
    account_array(total, state.header.assets.capacity(), sizeof(Asset), maximum);
    for (auto const &slot : state.slots) {
        check_cancel(cancelled);
        account(total, record_capacity(*slot.record, maximum), maximum);
    }
    for (auto const &[id, at] : state.indices) {
        check_cancel(cancelled);
        account_string(total, id, maximum);
    }
    return total;
}

std::string fold(std::string_view text)
{
    if (text.empty()) return {};
    if (text.find('\0') != std::string_view::npos ||
        !g_utf8_validate(text.data(), text.size(), nullptr)) {
        throw std::runtime_error("Invalid artwork catalog UTF-8");
    }
    GString normalized(g_utf8_normalize(text.data(), text.size(), G_NORMALIZE_DEFAULT_COMPOSE), g_free);
    if (!normalized) throw std::runtime_error("Cannot normalize artwork catalog text");
    GString folded(g_utf8_casefold(normalized.get(), -1), g_free);
    if (!folded) throw std::runtime_error("Cannot fold artwork catalog text");
    GString composed(g_utf8_normalize(folded.get(), -1, G_NORMALIZE_DEFAULT_COMPOSE), g_free);
    if (!composed) throw std::runtime_error("Cannot normalize artwork catalog search key");
    return composed.get();
}

std::shared_ptr<CatalogRecord const> make_record(Asset const &asset, std::shared_ptr<Bytes const> payload,
                                                CatalogLimits const &limits)
{
    // Bound normalization work even for caller-created, not-yet-validated data.
    if (asset.id.size() > 36 || asset.path.size() > 47 || asset.sha256.size() > 64 ||
        asset.name.size() > 1024 || asset.tags.size() > 32 || asset.extra_json.size() > limits.manifest.bytes) {
        throw std::runtime_error("Artwork catalog metadata exceeds limits");
    }
    auto record = std::make_shared<CatalogRecord>();
    // Fresh bounded containers, including every nested tag string. A move of
    // caller-owned metadata would preserve potentially enormous spare capacity.
    record->asset.id.assign(asset.id.data(), asset.id.size());
    record->asset.name.assign(asset.name.data(), asset.name.size());
    record->asset.path.assign(asset.path.data(), asset.path.size());
    record->asset.sha256.assign(asset.sha256.data(), asset.sha256.size());
    record->asset.extra_json.assign(asset.extra_json.data(), asset.extra_json.size());
    record->asset.width_mm = asset.width_mm;
    record->asset.height_mm = asset.height_mm;
    record->asset.tags.reserve(asset.tags.size());
    record->tag_keys.reserve(asset.tags.size());
    record->name_key = fold(asset.name);
    for (auto const &tag : asset.tags) {
        if (tag.size() > 128) throw std::runtime_error("Artwork catalog tag exceeds limits");
        record->asset.tags.emplace_back(tag.data(), tag.size());
        record->tag_keys.push_back(fold(tag));
    }
    record->payload = std::move(payload);
    record->metadata_bytes = record_capacity(*record, limits.metadata_bytes);
    return record;
}

std::size_t index(CatalogState const &state, std::string const &id, bool require_active = true)
{
    auto found = state.indices.find(id);
    if (found == state.indices.end() || (require_active && state.slots[found->second].removed)) {
        throw std::out_of_range("Unknown active artwork ID: " + id);
    }
    return found->second;
}

Manifest manifest_for(CatalogState const &state, Cancelled const &cancelled)
{
    check_cancel(cancelled);
    auto result = state.header;
    result.assets.reserve(state.slots.size());
    for (auto const &slot : state.slots) {
        check_cancel(cancelled);
        if (!slot.removed) result.assets.push_back(slot.record->asset);
    }
    check_cancel(cancelled);
    return result;
}

void validate_state(CatalogState const &state, Cancelled const &cancelled)
{
    if (state.slots.size() > state.limits.retained_assets) throw std::runtime_error("Artwork catalog session trash is full");
    (void)metadata_capacity(state, cancelled);
    (void)manifest_for(state, cancelled).serialize(state.limits.manifest, cancelled);
    check_cancel(cancelled);
}

void replace_record(CatalogState &state, std::size_t at, std::shared_ptr<CatalogRecord const> record)
{
    state.metadata_bytes -= state.slots[at].record->metadata_bytes;
    account(state.metadata_bytes, record->metadata_bytes, state.limits.metadata_bytes);
    state.slots[at].record = std::move(record);
}

std::string unique_name(CatalogState const &state, std::string const &requested,
                        std::string const &except, Cancelled const &cancelled)
{
    if (requested.size() > 1024) throw std::runtime_error("Artwork catalog name exceeds limits");
    auto key = fold(requested);
    std::set<std::string> names;
    for (auto const &slot : state.slots) {
        check_cancel(cancelled);
        if (!slot.removed && slot.record->asset.id != except) names.insert(slot.record->name_key);
    }
    if (!names.count(key)) return requested;
    // At most names.size() occupied candidates. Reserve suffix space without
    // cutting a UTF-8 code point when the original label is at the byte limit.
    for (std::size_t number = 2; number <= names.size() + 2; ++number) {
        check_cancel(cancelled);
        auto suffix = " (" + std::to_string(number) + ")";
        auto length = std::min(requested.size(), 1024 - suffix.size());
        while (length && length < requested.size() && (static_cast<unsigned char>(requested[length]) & 0xc0) == 0x80) --length;
        auto candidate = requested.substr(0, length) + suffix;
        if (!names.count(fold(candidate))) return candidate;
    }
    throw std::runtime_error("Cannot resolve artwork catalog name collision");
}

std::vector<std::string> terms(std::string_view query)
{
    if (query.size() > 4096) throw std::runtime_error("Artwork catalog search exceeds limits");
    auto normalized = fold(query);
    std::vector<std::string> result;
    auto start = normalized.c_str();
    auto cursor = start;
    while (*cursor) {
        auto next = g_utf8_next_char(cursor);
        if (g_unichar_isspace(g_utf8_get_char(cursor))) {
            if (cursor != start) result.emplace_back(start, cursor);
            start = next;
        }
        cursor = next;
    }
    if (cursor != start) result.emplace_back(start, cursor);
    return result;
}
std::vector<std::string> search_state(CatalogState const &state, std::string_view query, Cancelled const &cancelled)
{
    check_cancel(cancelled);
    auto words = terms(query);
    std::vector<std::string> result;
    for (auto const &slot : state.slots) {
        check_cancel(cancelled);
        if (slot.removed) continue;
        auto const &record = *slot.record;
        if (std::all_of(words.begin(), words.end(), [&](auto const &word) {
            return record.name_key.find(word) != std::string::npos ||
                std::any_of(record.tag_keys.begin(), record.tag_keys.end(), [&](auto const &tag) {
                    return tag.find(word) != std::string::npos;
                });
        })) result.push_back(record.asset.id);
    }
    check_cancel(cancelled);
    return result;
}
} // namespace

CatalogSnapshot::CatalogSnapshot(std::shared_ptr<CatalogState const> state) : _state(std::move(state)) {}
Manifest CatalogSnapshot::manifest(Cancelled cancelled) const
{
    auto state = _state;
    return manifest_for(*state, cancelled);
}
std::vector<std::string> CatalogSnapshot::list(Cancelled cancelled) const
{
    auto state = _state;
    return search_state(*state, "", cancelled);
}
Asset CatalogSnapshot::asset(std::string const &id) const
{
    auto state = _state;
    return state->slots[index(*state, id)].record->asset;
}
std::size_t CatalogSnapshot::artwork_size(std::string const &id) const
{
    auto state = _state;
    auto const &record = *state->slots[index(*state, id)].record;
    return record.payload ? record.payload->size() : state->base->artwork_size(id);
}
std::size_t CatalogSnapshot::overlay_bytes() const { return _state->overlay_bytes; }
std::shared_ptr<Package const> CatalogSnapshot::prior_package() const { return _state->base; }
std::size_t CatalogSnapshot::retained_metadata_bytes() const { return metadata_capacity(*_state); }

std::vector<std::string> CatalogSnapshot::search(std::string_view query, Cancelled cancelled) const
{
    auto state = _state;
    return search_state(*state, query, cancelled);
}

std::vector<std::string> CatalogSnapshot::removed_ids(Cancelled cancelled) const
{
    auto state = _state;
    check_cancel(cancelled);
    std::vector<std::string> result;
    for (auto const &slot : state->slots) {
        check_cancel(cancelled);
        if (slot.removed) result.push_back(slot.record->asset.id);
    }
    check_cancel(cancelled);
    return result;
}

CatalogSnapshot CatalogSnapshot::removed_items_snapshot(Cancelled cancelled) const
{
    auto state = _state;
    check_cancel(cancelled);
    auto next = std::make_shared<CatalogState>(*state);
    bool any = false;
    for (auto &slot : next->slots) {
        check_cancel(cancelled);
        any = any || slot.removed;
        slot.removed = !slot.removed;
    }
    if (!any) throw std::runtime_error("Artwork catalog has no removed entries");
    validate_state(*next, cancelled);
    return CatalogSnapshot(std::move(next));
}

std::shared_ptr<Bytes const> CatalogSnapshot::read_artwork(std::string const &id, Cancelled cancelled) const
{
    auto state = _state;
    check_cancel(cancelled);
    auto const &record = *state->slots[index(*state, id)].record;
    if (record.payload) return record.payload;
    if (state->base->artwork_size(id) > state->limits.artwork_bytes) throw std::runtime_error("Artwork catalog payload exceeds limits");
    auto result = std::make_shared<Bytes const>(state->base->read_artwork(id, cancelled));
    check_cancel(cancelled);
    return result;
}

std::optional<Bytes> CatalogSnapshot::read_preview(std::string const &id, Cancelled cancelled) const
{
    auto state = _state;
    check_cancel(cancelled);
    auto const &record = *state->slots[index(*state, id)].record;
    if (record.payload) return std::nullopt; // New artwork has no trusted/generated preview yet.
    auto size = state->base->preview_size(id);
    if (size && *size > state->limits.preview_bytes) throw std::runtime_error("Artwork catalog preview exceeds limits");
    return state->base->read_preview(id, cancelled);
}

Catalog::Catalog(std::shared_ptr<CatalogState const> state) : _state(std::move(state)) {}
CatalogSnapshot Catalog::snapshot() const { return CatalogSnapshot(_state); }

Catalog Catalog::create(Manifest metadata, CatalogLimits limits, Cancelled cancelled)
{
    check_cancel(cancelled);
    if (!metadata.assets.empty() || metadata.revision != 0) throw std::runtime_error("New artwork catalog requires an empty revision-zero manifest");
    auto state = std::make_shared<CatalogState>();
    state->header = canonical_header(metadata, limits);
    state->limits = limits;
    validate_state(*state, cancelled);
    return Catalog(std::move(state));
}

Catalog Catalog::from_package(Package package, CatalogLimits limits, Cancelled cancelled)
{
    check_cancel(cancelled);
    auto state = std::make_shared<CatalogState>();
    state->limits = limits;
    state->base = std::make_shared<Package const>(std::move(package));
    auto const &manifest = state->base->manifest();
    if (manifest.assets.size() > limits.retained_assets || manifest.assets.size() > limits.manifest.assets) {
        throw std::runtime_error("Too many artwork catalog entries");
    }
    state->header = canonical_header(manifest, limits);
    for (auto const &asset : manifest.assets) {
        check_cancel(cancelled);
        if (state->base->artwork_size(asset.id) > limits.artwork_bytes) throw std::runtime_error("Artwork catalog payload exceeds limits");
        auto record = make_record(asset, {}, limits);
        account(state->metadata_bytes, record->metadata_bytes, limits.metadata_bytes);
        state->indices.emplace(std::string(asset.id.data(), asset.id.size()), state->slots.size());
        state->slots.push_back({std::move(record), false});
    }
    validate_state(*state, cancelled);
    return Catalog(std::move(state));
}

void Catalog::publish(std::shared_ptr<CatalogState> next, std::shared_ptr<CatalogState const> const &before,
                      Cancelled const &cancelled)
{
    if (before->header.revision == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Artwork library revision overflow");
    next->header.revision = before->header.revision + 1;
    validate_state(*next, cancelled);
    check_cancel(cancelled);
    if (_state != before) throw std::runtime_error("Artwork catalog changed during edit");
    _state = std::move(next); // Only publication; every throwing operation is above.
}

std::string Catalog::add(NewArtwork metadata, Bytes const &unvalidated_artwork, Cancelled cancelled)
{
    auto before = _state;
    check_cancel(cancelled);
    if (before->slots.size() >= before->limits.retained_assets) throw std::runtime_error("Artwork catalog session trash is full");
    if (unvalidated_artwork.size() > before->limits.artwork_bytes) throw std::runtime_error("Artwork catalog payload exceeds limits");
    auto overlay_bytes = before->overlay_bytes;
    account(overlay_bytes, unvalidated_artwork.size(), before->limits.overlay_bytes);
    if (metadata.id.empty()) {
        GString uuid(g_uuid_string_random(), g_free);
        if (!uuid) throw std::runtime_error("Cannot create artwork UUID");
        metadata.id = uuid.get();
    }
    if (!valid_library_uuid(metadata.id) || before->indices.count(metadata.id)) throw std::runtime_error("Duplicate or invalid artwork catalog UUID");
    Asset asset;
    asset.id = metadata.id;
    asset.name = unique_name(*before, metadata.name, "", cancelled);
    asset.tags = std::move(metadata.tags);
    asset.path = "assets/" + asset.id + ".svg";
    asset.width_mm = metadata.width_mm;
    asset.height_mm = metadata.height_mm;
    asset.extra_json = std::move(metadata.extra_json);
    auto payload = std::make_shared<Bytes>(unvalidated_artwork.size());
    for (std::size_t at = 0; at < payload->size();) {
        check_cancel(cancelled);
        auto count = std::min<std::size_t>(65536, payload->size() - at);
        std::copy_n(unvalidated_artwork.data() + at, count, payload->data() + at);
        at += count;
    }
    asset.sha256 = artwork_sha256(*payload, cancelled);
    auto record = make_record(asset, std::move(payload), before->limits);
    auto next = std::make_shared<CatalogState>(*before);
    account(next->metadata_bytes, record->metadata_bytes, next->limits.metadata_bytes);
    next->overlay_bytes = overlay_bytes;
    next->indices.emplace(std::string(metadata.id.data(), metadata.id.size()), next->slots.size());
    next->slots.push_back({std::move(record), false});
    publish(std::move(next), before, cancelled);
    return std::move(metadata.id); // No allocating string copy after publication.
}

bool Catalog::rename(std::string const &id, std::string name, Cancelled cancelled)
{
    auto before = _state;
    check_cancel(cancelled);
    auto at = index(*before, id);
    auto const &record = *before->slots[at].record;
    if (record.asset.name == name) return false;
    auto asset = record.asset;
    asset.name = unique_name(*before, name, id, cancelled);
    if (asset.name == record.asset.name) return false;
    auto next = std::make_shared<CatalogState>(*before);
    replace_record(*next, at, make_record(asset, record.payload, next->limits));
    publish(std::move(next), before, cancelled);
    return true;
}

bool Catalog::set_tags(std::string const &id, std::vector<std::string> tags, Cancelled cancelled)
{
    auto before = _state;
    check_cancel(cancelled);
    auto at = index(*before, id);
    auto const &record = *before->slots[at].record;
    if (record.asset.tags == tags) return false;
    auto asset = record.asset;
    asset.tags = std::move(tags);
    auto next = std::make_shared<CatalogState>(*before);
    replace_record(*next, at, make_record(asset, record.payload, next->limits));
    publish(std::move(next), before, cancelled);
    return true;
}

bool Catalog::rename_library(std::string name, Cancelled cancelled)
{
    auto before = _state;
    check_cancel(cancelled);
    if (before->header.name == name) return false;
    auto next = std::make_shared<CatalogState>(*before);
    if (name.size() > 1024) throw std::runtime_error("Artwork catalog name exceeds limits");
    next->header.name = std::string(name.data(), name.size());
    publish(std::move(next), before, cancelled);
    return true;
}

bool Catalog::remove(std::string const &id, Cancelled cancelled)
{
    auto before = _state;
    check_cancel(cancelled);
    auto at = index(*before, id, false);
    if (before->slots[at].removed) return false;
    auto next = std::make_shared<CatalogState>(*before);
    next->slots[at].removed = true;
    publish(std::move(next), before, cancelled);
    return true;
}

bool Catalog::restore(std::string const &id, Cancelled cancelled)
{
    auto before = _state;
    check_cancel(cancelled);
    auto at = index(*before, id, false);
    if (!before->slots[at].removed) return false;
    auto const &record = *before->slots[at].record;
    auto asset = record.asset;
    asset.name = unique_name(*before, asset.name, id, cancelled);
    auto next = std::make_shared<CatalogState>(*before);
    replace_record(*next, at, make_record(asset, record.payload, next->limits));
    next->slots[at].removed = false;
    publish(std::move(next), before, cancelled);
    return true;
}

} // namespace Inkscape::IO::ArtworkLibrary
