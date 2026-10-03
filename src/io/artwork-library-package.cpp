// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-package.h"

#include <algorithm>
#include <glib.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace Inkscape::IO::ArtworkLibrary {

std::size_t Package::declared_bytes() const
{
    std::size_t total = 0;
    // Archive::open already enforces the aggregate bound and overflow checks.
    for (auto const &path : _archive.paths()) total += _archive.size(path);
    return total;
}
namespace {
void check_cancel(Cancelled const &cancelled)
{
    if (cancelled && cancelled()) throw std::runtime_error("Artwork library operation cancelled");
}
std::string preview_path(std::string const &id) { return "previews/" + id + ".png"; }
}

std::string artwork_sha256(Bytes const &bytes, Cancelled cancelled)
{
    check_cancel(cancelled);
    std::unique_ptr<GChecksum, decltype(&g_checksum_free)> hash(g_checksum_new(G_CHECKSUM_SHA256), g_checksum_free);
    if (!hash) throw std::runtime_error("Cannot initialize artwork checksum");
    for (std::size_t at = 0; at < bytes.size();) {
        check_cancel(cancelled);
        auto count = std::min<std::size_t>(65536, bytes.size() - at);
        g_checksum_update(hash.get(), bytes.data() + at, count);
        at += count;
    }
    check_cancel(cancelled);
    return g_checksum_get_string(hash.get());
}

Package::Package(Archive archive, Manifest manifest)
    : _archive(std::move(archive)), _manifest(std::move(manifest))
{
    for (std::size_t i = 0; i < _manifest.assets.size(); ++i) {
        _asset_indices.emplace(_manifest.assets[i].id, i);
    }
}

Package Package::open(Bytes bytes, ArchiveLimits archive_limits,
                      ManifestLimits manifest_limits, Cancelled cancelled)
{
    return open_with_admission(std::move(bytes), {}, archive_limits, manifest_limits, std::move(cancelled));
}

Package Package::open_with_admission(Bytes bytes, std::function<void(std::size_t)> admit,
                      ArchiveLimits archive_limits, ManifestLimits manifest_limits, Cancelled cancelled)
{
    auto archive = Archive::open(std::move(bytes), archive_limits, cancelled);
    if (admit) {
        std::size_t total = 0;
        for (auto const &path : archive.paths()) total += archive.size(path);
        admit(total);
        check_cancel(cancelled);
    }
    if (!archive.contains("library.json")) throw std::runtime_error("Missing artwork library manifest");
    if (!archive.size("library.json") || archive.size("library.json") > manifest_limits.bytes) {
        throw std::runtime_error("Invalid artwork library manifest size");
    }
    auto manifest_bytes = archive.read("library.json", cancelled);
    auto manifest = Manifest::parse(
        std::string_view(reinterpret_cast<char const *>(manifest_bytes.data()), manifest_bytes.size()),
        manifest_limits, cancelled);

    std::set<std::string> permitted{"library.json"};
    for (auto const &asset : manifest.assets) {
        check_cancel(cancelled);
        if (!archive.contains(asset.path)) throw std::runtime_error("Missing artwork: " + asset.id);
        permitted.insert(asset.path);
        permitted.insert(preview_path(asset.id));
    }
    for (auto const &path : archive.paths()) {
        check_cancel(cancelled);
        // Refuse unknown payloads rather than silently dropping them on a
        // future rewrite. Optional metadata belongs in the versioned manifest.
        if (!permitted.count(path)) throw std::runtime_error("Unrecognized artwork library entry: " + path);
    }
    check_cancel(cancelled);
    return Package(std::move(archive), std::move(manifest));
}

Asset const &Package::asset(std::string const &id) const
{
    auto found = _asset_indices.find(id);
    if (found == _asset_indices.end()) throw std::out_of_range("Unknown artwork ID: " + id);
    return _manifest.assets.at(found->second);
}

Bytes Package::read_artwork(std::string const &id, Cancelled cancelled) const
{
    // Own metadata before Archive::read may deliver a callback which replaces
    // or destroys this Package. Archive pins its own payload before callbacks.
    auto entry = asset(id);
    auto bytes = _archive.read(entry.path, cancelled);
    if (artwork_sha256(bytes, cancelled) != entry.sha256) {
        throw std::runtime_error("Artwork content hash mismatch: " + entry.id);
    }
    return bytes;
}

std::size_t Package::artwork_size(std::string const &id) const
{
    return _archive.size(asset(id).path);
}

std::optional<std::size_t> Package::preview_size(std::string const &id) const
{
    auto path = preview_path(asset(id).id);
    if (!_archive.contains(path)) return std::nullopt;
    return _archive.size(path);
}

std::optional<Bytes> Package::read_preview(std::string const &id, Cancelled cancelled) const
{
    auto path = preview_path(asset(id).id);
    if (!_archive.contains(path)) {
        check_cancel(cancelled);
        return std::nullopt;
    }
    return _archive.read(path, cancelled);
}

void Package::verify_artworks(Cancelled cancelled) const
{
    auto snapshot = *this;
    check_cancel(cancelled);
    for (auto const &entry : snapshot._manifest.assets) {
        snapshot.read_artwork(entry.id, cancelled);
    }
    check_cancel(cancelled);
}

} // namespace Inkscape::IO::ArtworkLibrary
