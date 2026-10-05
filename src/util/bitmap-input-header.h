// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap engine: bounded signature, header and URI validation (EB2-header).
 * Plan: internal note EXPLODE_BITMAP_IMPLEMENTATION_PLAN, internal note EXPLODE_BITMAP_PLAN v3.2.
 *
 * Pure plain-data engine code: no GTK, document, XML or exceptions. Nothing here decodes pixels,
 * preallocates raster storage or copies an href; every offset/length uses checked arithmetic.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UTIL_BITMAP_INPUT_HEADER_H
#define INKSCAPE_UTIL_BITMAP_INPUT_HEADER_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "util/bitmap-island-budget.h"

namespace Inkscape::Bitmap {

enum class Format { Unsupported, PNG, JPEG, WebP, TIFF, GIF, BMP };
enum class ColorModel { Unknown, Gray, GrayAlpha, Palette, RGB, RGBA, CMYK };

struct HeaderLimits {
    std::uint32_t maxAxis = 16384;
    std::uint64_t maxPixels = 100'000'000;
    std::uint64_t scratchBytes = MiB;              // baseline scratch / non-profile metadata allowance
    std::uint64_t maxProfileBytes = 4 * MiB;       // ICC / colour profile
    std::uint64_t maxEncodedBytes = 256 * MiB;     // decoded size of a data URI payload
    std::uint64_t maxUriBytes = 384 * MiB;         // whole href
    bool requireStill = true;                      // refuse animated / multi-frame data
    // The view holds only the LEADING bytes of the file. Parsing stops at the first
    // PNG IDAT / JPEG SOS / WebP image or ANMF chunk / GIF image descriptor; end-of-file checks
    // (IEND, EOI, RIFF end, GIF trailer, GIF frame count, IDAT CRC) become the decoder's job.
    bool prefixOnly = false;
};

struct RasterHeader {
    Format format = Format::Unsupported;
    bool supported = false; // classified as a still format a qualified decoder may accept
    std::uint32_t width = 0, height = 0;
    std::uint32_t bitDepth = 0; // bits per sample (0 = unknown)
    ColorModel color = ColorModel::Unknown;
    bool alpha = false;
    bool hasProfile = false;
    std::uint64_t profileBytes = 0; // stored (possibly compressed) profile size
    bool srgbTagged = false;        // PNG sRGB chunk
    std::uint32_t frames = 1;
    bool animated = false;          // multi-frame/animated
    std::uint8_t orientation = 0;   // EXIF/TIFF 1..8, 0 = absent/unknown
    bool interlaced = false;        // PNG Adam7, progressive JPEG, interlaced GIF
};

// Refuses (never allocates raster storage) on malformed/truncated/oversized/animated/mismatched data.
// TIFF/BMP/unknown data are classified (value.format) and refused as unsupported.
Result<RasterHeader> inspect(EncodedView, HeaderLimits const &, Budget &, Stop = {}) noexcept;

enum class UriKind { Data, Linked };
struct UriInfo {
    UriKind kind = UriKind::Linked;
    Format mimeFormat = Format::Unsupported;
    std::size_t payloadOffset = 0, payloadLength = 0; // base64 text, data URIs only
    std::uint64_t decodedBytes = 0;                    // precomputed, nothing decoded
    std::string_view mime;                             // declared media type (view into the href)
};
// Length bounds, media type and base64 shape/padding of an href; no decode and no copy.
// A Linked href returns ok() with kind == Linked: the caller MUST check kind (linked files are not
// embedded and are refused by embedded-only intake).
Result<UriInfo> inspectUri(std::string_view href, HeaderLimits const &, Stop = {}) noexcept;

// Validates the URI and walks chunk/segment headers without retaining skipped payloads.
// Decodes a budgeted prefix through the first image-data header, at least
// min(scratchBytes, decodedBytes). PNG/JPEG metadata keeps the profile+scratch scan limit;
// all formats keep the hard maxEncodedBytes bound. Smaller payloads get the full checks;
// larger payloads use prefixOnly (see above). The baseline window uses Stage::header;
// larger encoded metadata prefixes use Stage::input in the same ledger. Image-data
// payloads beyond the baseline window are skipped rather than allocated.
// Documented decode-side requirements: IDAT/zlib CRC, inflated ICC size, bytes after IEND/EOI
// (zero padding after JPEG EOI is tolerated, other trailers are refused), GIF frame count.
Result<RasterHeader> inspectHref(std::string_view href, HeaderLimits const &, Budget &, Stop = {}) noexcept;

} // namespace Inkscape::Bitmap

#endif // INKSCAPE_UTIL_BITMAP_INPUT_HEADER_H
