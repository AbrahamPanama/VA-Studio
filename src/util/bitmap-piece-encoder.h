// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap: piece crops, gutters, bleed and PNG encoding (EB2-png).
 * Plan: internal note EXPLODE_BITMAP_IMPLEMENTATION_PLAN, internal note EXPLODE_BITMAP_PLAN v3.2.
 *
 * Every piece is cut from the final grid by RUN OWNERSHIP (never by bounding box): only the foreground
 * runs the piece owns are copied; every other pixel of the crop, including samples owned by other pieces
 * that lie inside the bbox, is transparent. The crop is the half-open bbox plus a 1 px transparent gutter
 * on every side (it may start at -1 and end past the grid; nothing is read outside the grid). The RGB of
 * transparent pixels within 2 px (Euclidean, d^2 <= 4) of an owned visible sample of the SAME piece is copied
 * from the nearest such sample (ties: smaller d^2, then smaller dy, then smaller dx); alpha is never changed.
 * PNG: straight RGBA8, non-interlaced, zlib level 6, deterministic. Colour: pixel values unchanged; the
 * source ICC profile bytes (FinalGrid::profile) are attached byte-identical as iCCP, untagged stays untagged;
 * ICC header, embedded length and tag bounds are checked before pixel work; RGB uses colour type 6,
 * GRAY uses type 4 and requires neutral samples (owner option C).
 * pHYs carries the grid dpi; no gAMA/cHRM/sRGB chunk is ever written.
 *
 * Accounting: B (sum of gutter-inclusive crop areas) <= min(64 MP, 2P); a worst-case codec bound per piece is
 * reserved FIRST (crop buffer, encoder scratch, sum of worst PNG sizes, worst href size), then everything is
 * shrunk to live PNG allocation sizes and exact H = sum(22 + 4*ceil(Qi/3)) (< 2 GiB). Encoder scratch is enforced for real by
 * libpng's user allocator. Execution is serial: `encoders` only scales the scratch reservation (1 or 2).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UTIL_BITMAP_PIECE_ENCODER_H
#define INKSCAPE_UTIL_BITMAP_PIECE_ENCODER_H

#include <cstdint>

#include "ui/explode-bitmap-grid.h"
#include "util/bitmap-island-enclosure.h"

namespace Inkscape::Bitmap {

constexpr std::uint32_t maxPieceAxis = 32767;      // Cairo limit, gutter included
constexpr std::uint64_t maxCropPixels = 64'000'000; // B ceiling (also <= 2P)
constexpr std::uint64_t maxHrefBytes = 2048 * MiB - 1; // signed XML/document text-size bound, not RAM

struct EncodedPiece {
    std::int32_t x = 0, y = 0;            // crop origin on the grid, gutter included (may be -1)
    std::uint32_t width = 0, height = 0;  // gutter included
    std::uint64_t size = 0;               // PNG bytes
    std::uint8_t *data = nullptr;         // owned by EncodedPieces
};

// Move-only. Budget must outlive it. Pieces follow the Partition's sorted order.
class EncodedPieces {
public:
    EncodedPieces() noexcept = default;
    ~EncodedPieces() { reset(); }
    EncodedPieces(EncodedPieces const &) = delete;
    EncodedPieces &operator=(EncodedPieces const &) = delete;
    EncodedPieces(EncodedPieces &&o) noexcept { *this = std::move(o); }
    EncodedPieces &operator=(EncodedPieces &&) noexcept;
    void reset() noexcept;
    std::uint32_t count() const noexcept { return _count; }
    EncodedPiece const &piece(std::uint32_t i) const noexcept { return _pieces[i]; }
    std::uint64_t cropArea = 0;      // B
    std::uint64_t encodedBytes = 0;  // Q, exact
    std::uint64_t hrefBytes = 0;     // H, exact; reserved in `hrefReservation` until the publisher releases it
    Budget::Token hrefReservation;
private:
    friend Result<EncodedPieces> encode(FinalGrid const &, Partition const &, Budget &, Stop, struct EncodeOptions) noexcept;
    friend Result<EncodedPieces> encodeWholeGrid(FinalGrid const &, Budget &, Stop, struct EncodeOptions) noexcept;
    EncodedPiece *_pieces = nullptr;
    std::uint32_t _count = 0;
    Budget::Token _indexToken, _dataToken;
};

enum class EncodePhase { crop, bleed, encode };

// Lower-only ceilings and test hooks.
struct EncodeOptions {
    std::uint32_t encoders = 1;              // 1 or 2 (scratch reservation only; execution is serial)
    std::uint64_t maxCropPixels = Inkscape::Bitmap::maxCropPixels;
    std::uint64_t maxHref = maxHrefBytes;
    std::uint64_t scratchOverride = 0;       // TEST: replace the computed encoder scratch (0 = computed)
    JobWork *work = nullptr;                 // shared job meter; absent = standalone meter
    void (*observe)(EncodePhase, std::uint64_t, void *) noexcept = nullptr; // TEST: after charged work
    void *observerData = nullptr;
    AllocationFault *fault = nullptr;        // fails crop/index/piece/libpng allocations and shrinking realloc
};

// Half-open piece dimensions, before adding the two gutter pixels.
bool pieceAxesFit(std::uint32_t width, std::uint32_t height) noexcept;

// Worst-case encoded size of one piece (codec bound, chunks, profile). 0 on overflow.
std::uint64_t worstPngBytes(std::uint32_t width, std::uint32_t height, std::uint64_t profileBytes) noexcept;
// Encoder scratch reserved per encoder for crops up to `maxWidth` wide.
std::uint64_t encoderScratchBytes(std::uint32_t maxWidth, std::uint64_t profileBytes) noexcept;

Result<EncodedPieces> encode(FinalGrid const &, Partition const &, Budget &, Stop = {}, EncodeOptions = {}) noexcept;

// Full source grid: no crop, gutter or bleed; identical PNG/profile/Stop policy.
Result<EncodedPieces> encodeWholeGrid(FinalGrid const &, Budget &, Stop = {}, EncodeOptions = {}) noexcept;

} // namespace Inkscape::Bitmap

#endif // INKSCAPE_UTIL_BITMAP_PIECE_ENCODER_H
