// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_BITMAP_PREVIEW_COMPOSER_H
#define INKSCAPE_UI_BITMAP_PREVIEW_COMPOSER_H
#include <cstdint>
#include <memory>
#include <optional>
#include "display/bitmap-tone.h"
#include "util/bitmap-island-budget.h"
class SPItem;
namespace Inkscape { class Pixbuf; }
namespace Inkscape::Bitmap {
struct ViewIdentity { SPItem *item; unsigned display_key; };
enum class PreviewLayer { Tone, Alpha };
// Alpha pixels are source-only: never baked tone, clip or opacity. Their backing
// owner must retain its EB4 reservation until the last shared reference dies.
// An independently reserved display allocation. Only the worker fills pixels;
// published owners are const, and never retain a preparation Output or PNG/grid.
struct AlphaDisplayBacking {
    std::shared_ptr<Budget> budget, ledger;
    Budget::Token reservation;
    PlainBuffer pixels;
    unsigned width = 0, height = 0, stride = 0;
};
Result<std::shared_ptr<AlphaDisplayBacking>> reserveAlphaDisplay(
    unsigned width, unsigned height, std::shared_ptr<Budget> const &) noexcept;
Outcome prepareAlphaDisplay(RgbaView, AlphaDisplayBacking &, Stop = {}, AllocationFault * = nullptr) noexcept;
std::shared_ptr<Pixbuf const> wrapAlphaDisplay(std::shared_ptr<AlphaDisplayBacking const> const &);
struct Contribution {
    PreviewLayer layer = PreviewLayer::Tone;
    std::optional<Filters::BitmapToneSettings> tone;
    bool clipping_warning = false;
    std::shared_ptr<Pixbuf const> pixels;
    std::shared_ptr<AlphaDisplayBacking const> backing;
    std::weak_ptr<Pixbuf const> source; // source incarnation used by the job
};
using Generation = std::uint64_t;
struct PreviewClient;
class ClientLease final {
public:
    ClientLease() = default;
    ~ClientLease();
    ClientLease(ClientLease &&) noexcept;
    ClientLease &operator=(ClientLease &&) noexcept;
    ClientLease(ClientLease const &) = delete;
    ClientLease &operator=(ClientLease const &) = delete;
    explicit operator bool() const { return bool(_client); }
    // Prepare all tone targets before installing any, preserving atomic preview.
    // Only one replaceable prepared generation is retained per lease.
    // After invalidation, prepare a newer generation; retired generations reject.
    Outcome prepare(Contribution, Generation) noexcept;
    void reset() noexcept;
private:
    friend ClientLease contribute(ViewIdentity, Contribution);
    friend Outcome update(ClientLease &, Generation);
    std::shared_ptr<PreviewClient> _client;
};
// No pixel copying or worker launch happens here. Off-thread clients submit
// plain bytes through BitmapJobs, then prepare a source-only Pixbuf on main.
// Their session checks its own weak lifetime before accessing the lease.
// Replacing or resetting a lease drops prepared buffers immediately; the view
// and the surviving contribution retain only the currently installed pixels.
// Main-thread only. contribute registers a weak client; it does not install a
// view. update publishes the prepared generation (or the initial contribution
// only while the lease has never been invalidated).
ClientLease contribute(ViewIdentity, Contribution);
Outcome update(ClientLease &, Generation);
} // namespace Inkscape::Bitmap
#endif
