// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/bitmap-preview-composer.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
#include <sigc++/scoped_connection.h>
#include "bitmap-adjustment-chemistry.h"
#include "display/cairo-utils.h"
#include "display/drawing-item.h"
#include "display/drawing-image.h"
#include "display/nr-filter.h"
#include "document.h"
#include "object/sp-filter.h"
#include "object/sp-image.h"
#include "object/sp-root.h"
#include "object/weakptr.h"
#include "style.h"
#include "async/bitmap-job-reaper.h"
namespace Inkscape::Bitmap {
Result<std::shared_ptr<AlphaDisplayBacking>> reserveAlphaDisplay(
    unsigned width, unsigned height, std::shared_ptr<Budget> const &budget) noexcept
try {
    if (!budget || !width || !height || width > 5000 || height > 5000)
        return {{Status::failed, "Invalid display dimensions."}, {}};
    auto owner = std::make_shared<AlphaDisplayBacking>();
    owner->budget = budget; owner->width = width; owner->height = height;
    owner->stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    auto bytes = std::uint64_t(owner->stride) * height;
    // Full-source display cache; Stage::preview remains the bounded panel proxy.
    auto check = budget->acquire(Stage::composition, bytes, owner->reservation);
    if (!check.ok()) return {check, {}};
    owner->ledger = std::make_shared<Budget>(Budget::FixedLimitForTest{}, bytes);
    return {{}, std::move(owner)};
} catch (...) { return {{Status::failed, "Display reservation failed."}, {}}; }
Outcome prepareAlphaDisplay(RgbaView source, AlphaDisplayBacking &owner, Stop stop, AllocationFault *fault) noexcept
{
    auto check = source.validate(); if (!check.ok()) return check;
    if (!owner.width || !owner.height || owner.width > 5000 || owner.height > 5000 ||
        source.width != owner.width || source.height != owner.height || !owner.ledger ||
        owner.stride != source.width * 4 || owner.reservation.bytes() != std::uint64_t(owner.stride)*owner.height)
        return {Status::failed, "Invalid display reservation."};
    check = owner.pixels.allocate(*owner.ledger, Stage::composition, owner.reservation.bytes(), 1, fault, stop);
    if (!check.ok()) return check;
    for (unsigned y = 0; y < owner.height; ++y) {
        if (stop.requested()) { owner.pixels.reset(); return {Status::canceled, "Canceled"}; }
        auto dst = reinterpret_cast<unsigned char *>(owner.pixels.data()) + std::uint64_t(y)*owner.stride;
        std::memcpy(dst, source.data + std::uint64_t(y)*source.stride, owner.stride);
        convert_pixels_pixbuf_to_argb32(dst, owner.width, 1, owner.stride);
    }
    return {};
}
std::shared_ptr<Pixbuf const> wrapAlphaDisplay(std::shared_ptr<AlphaDisplayBacking const> const &backing)
{
    assertBitmapMainThread();
    if (!backing || !backing->budget || !backing->width || !backing->height ||
        backing->width > 5000 || backing->height > 5000 || backing->stride != backing->width*4 ||
        backing->reservation.bytes() != std::uint64_t(backing->stride)*backing->height ||
        backing->pixels.size() != backing->reservation.bytes()) return {};
    // Cairo does not write source surfaces. The const owner lasts through frozen readers.
    auto surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char *>(const_cast<std::byte *>(backing->pixels.data())),
        CAIRO_FORMAT_ARGB32, backing->width, backing->height, backing->stride);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(surface); return {}; }
    static cairo_user_data_key_t key;
    std::shared_ptr<AlphaDisplayBacking const> *owner;
    try { owner = new std::shared_ptr<AlphaDisplayBacking const>(backing); }
    catch (...) { cairo_surface_destroy(surface); throw; }
    if (cairo_surface_set_user_data(surface, &key, owner, [](void *p) {
            delete static_cast<std::shared_ptr<AlphaDisplayBacking const> *>(p);
        }) != CAIRO_STATUS_SUCCESS) { delete owner; cairo_surface_destroy(surface); return {}; }
    try { return std::make_shared<Pixbuf>(surface); }
    catch (...) { cairo_surface_destroy(surface); throw; }
}
struct PreviewOwner;
// Distinct from Bitmap::Prepared, the Explode publication payload: their
// destructors must never share a linker symbol across translation units.
struct PreparedPreview {
    Contribution contribution;
    Generation generation = 0;
    std::uint64_t revision = 0;
    unsigned view_key = 0;
    DrawingItem *view = nullptr; // checked again; never dereferenced after hide
    std::unique_ptr<Filters::Filter> renderer;
    std::shared_ptr<Pixbuf const> pixels;
    bool alpha_pixels = false;
    Generation view_generation = 0;
    std::weak_ptr<Pixbuf const> source;
};
struct PreviewClient {
    std::shared_ptr<PreviewOwner> owner;
    Contribution contribution;
    Generation generation = 0; // highest published or invalidated generation
    bool published = false;
    bool requires_preparation = false;
    std::optional<PreparedPreview> prepared;
    void invalidatePrepared() noexcept
    {
        requires_preparation = true;
        if (prepared) generation = std::max(generation, prepared->generation);
        prepared.reset();
    }
};
namespace { std::vector<std::weak_ptr<PreviewOwner>> owners; }
struct PreviewOwner {
    SPWeakPtr<SPItem> item;
    SPWeakPtr<SPRoot> root; // document lifetime, never an id lookup
    unsigned key;
    bool owns_pixels = false;
    std::uint64_t revision = 0;
    std::vector<std::weak_ptr<PreviewClient>> clients;
    sigc::scoped_connection modified, released, document_released;
    PreviewOwner(ViewIdentity id) : item(id.item), root(id.item->document->getRoot()), key(id.display_key) {}
    DrawingItem *view() const
    {
        if (!root || !item) return nullptr;
        if (auto image = cast<SPImage>(item.get()); image && (image->isHidden() || image->isHidden(key))) return nullptr;
        return item->get_arenaitem(key);
    }
    PreparedPreview compose(PreviewClient *replacing = nullptr, Contribution value = {})
    {
        PreparedPreview result;
        result.view = view();
        result.revision = revision;
        result.contribution = std::move(value);
        if (!result.view) return result;
        result.view_key = SPItem::ensure_key(result.view);
        Contribution const *tone = nullptr;
        auto image = cast<SPImage>(item.get());
        if (image) {
            result.pixels = image->pixbuf; result.source = image->pixbuf;
            result.view_generation = image->composedViewGeneration(key);
        }
        auto visit = [&](Contribution const &c) {
            if (c.layer == PreviewLayer::Tone) tone = &c;
            else if (image && c.pixels && c.source.lock() == image->pixbuf) { result.pixels = c.pixels; result.alpha_pixels = true; }
        };
        for (auto const &weak : clients) {
            if (auto client = weak.lock(); client && client.get() != replacing && client->published) visit(client->contribution);
        }
        if (replacing) visit(result.contribution);
        if (tone) {
            result.renderer = BitmapAdjustments::build_tone_preview_renderer(
                item.get(), result.view, tone->tone, tone->clipping_warning);
        } else if (item->style && item->style->filter.set) {
            if (auto filter = item->style->getFilter()) result.renderer = filter->build_renderer(result.view);
        }
        return result;
    }
    bool install(PreparedPreview &p)
    {
        if (!p.view || view() != p.view || p.view->key() != p.view_key) return false;
        // Tone alone retains legacy pixels, but cannot reuse pixels with baked tone.
        if (auto image = cast<SPImage>(item.get())) {
            ViewPixels pixels{p.alpha_pixels ? p.pixels : nullptr, p.source, p.view_key};
            auto drawing_image = cast<DrawingImage>(p.view);
            bool const restore_source = drawing_image && drawing_image->suppressesOwnEffect(SuppressedOwnEffects::Tone);
            // Existing contributions are source-only; tone remains in the
            // renderer. Restore canonical pixels and clear all baked effects
            // together when the previous view would suppress the new renderer.
            if (!image->setComposedView(key, pixels, SuppressedOwnEffects::None, p.view_generation,
                                        &p.renderer, owns_pixels || p.alpha_pixels || restore_source)) return false;
        } else {
            p.view->setFilterRenderer(std::move(p.renderer));
        }
        owns_pixels = p.alpha_pixels;
        return true;
    }
    void invalidate() noexcept
    {
        ++revision;
        owns_pixels = false;
        for (auto const &weak : clients) if (auto c = weak.lock()) {
            c->invalidatePrepared();
            c->contribution.pixels.reset(); c->contribution.backing.reset();
            c->published = false;
        }
    }
    void refresh() noexcept
    {
        assertBitmapMainThread();
        ++revision;
        auto image = cast<SPImage>(item.get());
        for (auto const &weak : clients) if (auto c = weak.lock()) {
            c->invalidatePrepared();
            if (image && c->contribution.pixels && c->contribution.source.lock() != image->pixbuf) {
                c->contribution.pixels.reset(); c->contribution.backing.reset(); // never pin obsolete source or job output
            }
        }
        try { auto p = compose(); install(p); } catch (...) { std::fputs("Bitmap preview: recomposition failed\n", stderr); }
    }
};
ClientLease::~ClientLease() { reset(); }
ClientLease::ClientLease(ClientLease &&) noexcept = default;
ClientLease &ClientLease::operator=(ClientLease &&other) noexcept
{
    if (this != &other) { reset(); _client = std::move(other._client); }
    return *this;
}
void ClientLease::reset() noexcept
{
    if (!_client) return;
    assertBitmapMainThread();
    auto owner = _client->owner;
    auto published = _client->published;
    _client.reset(); // remove this client before composing survivors
    std::erase_if(owner->clients, [](auto const &w) { return w.expired(); });
    if (published) owner->refresh(); // last departure rebuilds CURRENT committed filter/pixbuf
    std::erase_if(owners, [](auto const &w) { return w.expired(); });
}
ClientLease contribute(ViewIdentity id, Contribution value)
try {
    assertBitmapMainThread();
    ClientLease lease;
    if (!id.item || !id.item->document) return lease;
    std::erase_if(owners, [](auto const &w) { return w.expired(); });
    std::shared_ptr<PreviewOwner> owner;
    for (auto const &weak : owners) if (auto candidate = weak.lock()) {
        if (candidate->item.get() == id.item && candidate->root && candidate->key == id.display_key) owner = candidate;
    }
    if (!owner) {
        owner = std::make_shared<PreviewOwner>(id);
        owner->modified = id.item->connectModified([weak = std::weak_ptr(owner)](auto *, auto) {
            if (auto current = weak.lock()) current->refresh();
        });
        auto invalidate = [weak = std::weak_ptr(owner)](auto *) {
            if (auto current = weak.lock()) current->invalidate();
        };
        owner->released = id.item->connectRelease(invalidate);
        owner->document_released = id.item->document->getRoot()->connectRelease(invalidate);
        owners.emplace_back(owner);
    }
    for (auto const &weak : owner->clients) if (auto c = weak.lock()) {
        if (c->contribution.layer == value.layer) return lease; // one client per layer
    }
    lease._client = std::make_shared<PreviewClient>();
    lease._client->owner = owner;
    lease._client->contribution = std::move(value);
    owner->clients.emplace_back(lease._client);
    return lease;
} catch (...) { return {}; }
Outcome ClientLease::prepare(Contribution value, Generation generation) noexcept
{
    assertBitmapMainThread();
    if (!_client) return {Status::unavailable, "preview view unavailable"};
    auto &c = *_client;
    if (generation <= c.generation || (c.prepared && generation < c.prepared->generation)) return {Status::canceled, "stale preview generation"};
    c.prepared.reset();
    if (!c.owner->view()) return {Status::unavailable, "preview view unavailable"};
    if (value.layer != c.contribution.layer) return {Status::incompatible, "conflicting preview layer"};
    auto image = cast<SPImage>(c.owner->item.get());
    if (value.layer == PreviewLayer::Alpha && value.pixels) {
        if (!image || !image->pixbuf || value.source.lock() != image->pixbuf) return {Status::canceled, "stale preview generation"};
        if (value.pixels->width() != image->pixbuf->width() || value.pixels->height() != image->pixbuf->height() ||
            !value.backing || !value.backing->budget ||
            value.pixels->pixelFormat() != Pixbuf::PF_CAIRO ||
            value.pixels->width() <= 0 || value.pixels->height() <= 0 ||
            value.pixels->rowstride() != cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, value.pixels->width()) ||
            value.backing->width != unsigned(value.pixels->width()) ||
            value.backing->height != unsigned(value.pixels->height()) ||
            value.backing->stride != unsigned(value.pixels->rowstride()) ||
            value.backing->reservation.bytes() != std::uint64_t(value.pixels->rowstride()) * value.pixels->height() ||
            value.backing->pixels.size() != value.backing->reservation.bytes() ||
            value.pixels->pixels() != reinterpret_cast<unsigned char const *>(value.backing->pixels.data())) return {Status::failed, "preview preparation failed"};
    }
    try {
        auto p = c.owner->compose(&c, std::move(value));
        if (!p.view) return {Status::unavailable, "preview view unavailable"};
        p.generation = generation;
        c.prepared = std::move(p);
        return {Status::unchanged, "prepared"};
    } catch (...) { return {Status::failed, "preview preparation failed"}; }
}
Outcome update(ClientLease &lease, Generation generation)
{
    assertBitmapMainThread();
    if (!lease._client) return {Status::unavailable, "preview view unavailable"};
    auto &c = *lease._client;
    if (!c.owner->view()) { c.invalidatePrepared(); return {Status::unavailable, "preview view unavailable"}; }
    if (generation <= c.generation) return {Status::canceled, "stale preview generation"};
    if (!c.prepared) {
        if (c.published || c.requires_preparation) return {Status::canceled, "stale preview generation"};
        auto result = lease.prepare(c.contribution, generation);
        if (result.status != Status::unchanged) return result;
    }
    auto &p = *c.prepared;
    if (generation != p.generation) return {Status::canceled, "stale preview generation"};
    if (p.revision != c.owner->revision) { c.invalidatePrepared(); return {Status::canceled, "stale preview generation"}; }
    if (auto image = cast<SPImage>(c.owner->item.get()); image && p.source.lock() != image->pixbuf) {
        c.invalidatePrepared(); return {Status::canceled, "stale source incarnation"};
    }
    if (!c.owner->install(p)) { c.invalidatePrepared(); return {Status::unavailable, "preview view unavailable"}; }
    c.contribution = std::move(p.contribution);
    c.generation = generation;
    c.published = true;
    c.prepared.reset();
    ++c.owner->revision;
    return {Status::changed, "installed"};
}
} // namespace Inkscape::Bitmap
