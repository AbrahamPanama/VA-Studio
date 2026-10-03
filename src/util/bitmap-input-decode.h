// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap engine: qualified straight sRGB RGBA8 decode (EB2-decode).
 * Plan: doc/vacards/EXPLODE_BITMAP_IMPLEMENTATION_PLAN.md, doc/vacards/EXPLODE_BITMAP_PLAN.md v3.2.
 *
 * Runs after the EB2-header gate. Codec route: GdkPixbufLoader (the shipped codec stack: libpng,
 * libjpeg, GIF and, where the platform registers it, a WebP loader), fed in 64 KiB chunks with Stop
 * polled between chunks. Every output and intermediate is reserved through the Budget first.
 * Colour policy (round 2): sample values are kept AS DECODED; nothing is converted to sRGB, because the canvas
 * ignores embedded ICC (sp-image.cpp:274) and pieces must look like the original. Grey/palette/tRNS expand to
 * RGBA through the codec with straight alpha; 16-bit samples use libpng's high-byte reduction (display parity,
 * max error 1/255 versus rounding). An embedded RGB/grey profile is validated (must open, be an input/display
 * class profile and match the image colour model, else refused) and CARRIED in DecodedRaster::profile so the
 * PNG encoder can attach it to every piece. Owner confirmation of this default is pending. CMYK is refused.
 * Metadata bombs: whole-file mode inflates every zTXt/iTXt/iCCP chunk with a bounded inflate (aggregate
 * INFLATED bytes <= HeaderLimits::maxProfileBytes; plain tEXt is bounded by the file size) before GdkPixbuf sees the bytes; JPEGs
 * are capped at 100 scans, verified with libjpeg (damage warnings JWRN_JPEG_EOF/HIT_MARKER/HUFF_BAD_CODE/MUST_RESYNC = corrupt; extraneous bytes and version/transform warnings are tolerated, like the canvas) and written per scan, so Stop is
 * honoured between scans. KNOWN GAP: GIF streams and highly compressible images are only validated by their
 * codec; one gdk write of such data is not interruptible (writes are 64 KiB).
 * Orientation: GdkPixbuf's "orientation" option is the authority (absent = 1), because that is exactly what the
 * canvas applies; it is applied once while writing the output. The header's EXIF value is only a cross-check:
 * if both exist and differ the image is refused; if only one exists, the gdk value wins. The original encoded bytes stay recoverable as a non-owning view plus a
 * 64-bit FNV-1a fingerprint.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UTIL_BITMAP_INPUT_DECODE_H
#define INKSCAPE_UTIL_BITMAP_INPUT_DECODE_H

#include <cstdint>

#include "util/bitmap-input-header.h"
#include "util/bitmap-island-budget.h"

namespace Inkscape::Bitmap {

struct ValidatedInput {
    EncodedView encoded;     // COMPLETE original bytes; caller keeps them alive and unchanged
    HeaderLimits limits;     // the limits the header gate used; decode re-checks the whole file
    // Dimensions promised by an earlier (possibly prefix-only) gate; 0 = none. Mismatch refuses.
    std::uint32_t expectWidth = 0, expectHeight = 0;
    // Optional TEST/TELEMETRY hook, not a UI contract. Called on the thread running decode(), never concurrently,
    // from noexcept code (exceptions are swallowed). phase 0 = verification passes (done/total are positions in
    // the encoded bytes and may restart per pass), phase 1 = after each GdkPixbuf write (done is monotone).
    // Stop is polled right after each call.
    void (*progress)(void *user, std::uint64_t done, std::uint64_t total, unsigned phase) = nullptr;
    void *user = nullptr;
};

struct DecodedRaster {
    PlainBuffer pixels;                  // straight sRGB RGBA8, tight stride (width * 4), after orientation
    std::uint32_t width = 0, height = 0; // output size (axes swapped for orientation 5..8)
    std::uint32_t sourceWidth = 0, sourceHeight = 0;
    Format format = Format::Unsupported;
    std::uint8_t orientation = 1;        // EXIF orientation applied once (1 = none)
    bool hadAlpha = false;               // source carried alpha/tRNS
    bool reduced16 = false;              // 16-bit source reduced to 8 bits
    PlainBuffer profile;                 // embedded ICC profile bytes, carried unchanged (empty = none)
    std::uint64_t profileBytes = 0;
    EncodedView original;                // non-owning: the recoverable original bytes
    std::uint64_t originalBytes = 0;
    std::uint64_t originalHash = 0;      // FNV-1a 64 of `original`
    RgbaView view() const noexcept;
};

// Refuses (no raster, balanced Budget) on corrupt/truncated data, codec errors, output/header
// mismatch, an unavailable codec, Stop or a failed reservation. Never throws.
Result<DecodedRaster> decode(ValidatedInput const &, Budget &, Stop = {}, AllocationFault * = nullptr) noexcept;

std::uint64_t fingerprint(EncodedView) noexcept; // FNV-1a 64, the value stored in originalHash (no Stop)

} // namespace Inkscape::Bitmap

#endif // INKSCAPE_UTIL_BITMAP_INPUT_DECODE_H
