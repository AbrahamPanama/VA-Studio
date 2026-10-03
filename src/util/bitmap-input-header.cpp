// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap engine: bounded signature, header and URI validation (EB2-header).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "util/bitmap-input-header.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Inkscape::Bitmap {
namespace {

using u8 = std::uint8_t;
Outcome refuse(char const *why) noexcept { return {Status::incompatible, why}; }
Outcome broken(char const *why) noexcept { return {Status::failed, why}; }
constexpr Outcome good{};

bool has(std::size_t size, std::uint64_t pos, std::uint64_t n) noexcept { return pos <= size && n <= size - pos; }
std::uint32_t be16(u8 const *p) noexcept { return (p[0] << 8) | p[1]; }
std::uint32_t le16(u8 const *p) noexcept { return p[0] | (p[1] << 8); }
std::uint32_t be32(u8 const *p) noexcept { return (std::uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
std::uint32_t le32(u8 const *p) noexcept { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint32_t le24(u8 const *p) noexcept { return p[0] | (p[1] << 8) | (p[2] << 16); }
bool tag(u8 const *p, char const *s) noexcept { return std::memcmp(p, s, 4) == 0; }

struct Ctx {
    HeaderLimits const &lim;
    Stop stop;
    std::uint64_t scanLimit; // bytes of metadata allowed before the image data/frame header
};

Outcome checkDims(RasterHeader const &h, HeaderLimits const &lim) noexcept
{
    if (h.width == 0 || h.height == 0) return refuse("Image has a zero-size axis.");
    if (h.width > lim.maxAxis || h.height > lim.maxAxis) return refuse("Image axis exceeds the limit.");
    std::uint64_t px = 0;
    if (!checkedMul(h.width, h.height, px) || px > lim.maxPixels) return refuse("Image pixel count exceeds the limit.");
    return good;
}

// CRC with Stop polled every 64 KiB; false = canceled. Result in `ok`.
bool crcMatches(u8 const *p, std::uint64_t n, std::uint32_t expected, Stop stop, bool &ok) noexcept
{
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::uint64_t i = 0; i < n;) {
        if (stop.requested()) return false;
        std::uint64_t e = std::min<std::uint64_t>(n, i + 65536);
        for (; i < e; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    ok = ~c == expected;
    return true;
}

// ---- PNG -------------------------------------------------------------------------------------
Outcome parsePng(u8 const *p, std::size_t n, Ctx const &c, RasterHeader &h) noexcept
{
    std::uint64_t pos = 8;
    bool ihdr = false, plte = false, idat = false, iend = false;
    std::uint32_t colorType = 0, iter = 0;
    bool sawActl = false;
    while (pos < n && !iend) {
        if ((++iter & 255) == 0 && c.stop.requested()) return {Status::canceled, "Inspection canceled."};
        if (!has(n, pos, 12)) {
            // A prefix may end inside the first IDAT chunk header's CRC field; only the 8 header bytes matter.
            if (!(c.lim.prefixOnly && has(n, pos, 8) && tag(p + pos + 4, "IDAT"))) return broken("PNG chunk header is truncated.");
        }
        std::uint32_t len = be32(p + pos);
        if (len > 0x7FFFFFFFu) return broken("PNG chunk length is invalid.");
        u8 const *type = p + pos + 4, *data = p + pos + 8;
        bool isIdat = tag(type, "IDAT");
        bool prefixIdat = c.lim.prefixOnly && isIdat; // prefix mode stops here; the rest is decode's job
        std::uint64_t total = 0, end = 0;
        if (!checkedAdd(12, len, total) || !checkedAdd(pos, total, end)) return broken("PNG chunk length exceeds the data.");
        if (end > n && !prefixIdat) {
            return c.lim.prefixOnly ? refuse("Image header does not fit the inspection window.") : broken("PNG chunk length exceeds the data.");
        }
        // Metadata before image data is bounded by where it ENDS, before any checksum work.
        if (!idat && (isIdat ? pos : end) > c.scanLimit) return refuse("Image header metadata exceeds the scan limit.");
        if (ihdr && tag(type, "iCCP") && len > c.lim.maxProfileBytes) return refuse("Colour profile is too large.");
        if (ihdr && !(type[0] & 0x20) && !isIdat && !tag(type, "PLTE") && !tag(type, "IEND")) {
            return refuse("PNG has an unknown critical chunk.");
        }
        // Image data CRC is the decoder's job. Chunks after IDAT are not checksummed (only IEND, which is empty).
        if (!isIdat && (!idat || tag(type, "IEND"))) {
            bool good32 = false;
            if (!crcMatches(type, std::uint64_t(len) + 4, be32(data + len), c.stop, good32)) return {Status::canceled, "Inspection canceled."};
            if (!good32) return broken("PNG chunk CRC mismatch.");
        }
        if (!ihdr) {
            if (!tag(type, "IHDR") || len != 13) return broken("PNG IHDR is missing or malformed.");
            ihdr = true;
            h.width = be32(data);
            h.height = be32(data + 4);
            h.bitDepth = data[8];
            colorType = data[9];
            bool depthOk = false;
            switch (colorType) {
                case 0: depthOk = h.bitDepth == 1 || h.bitDepth == 2 || h.bitDepth == 4 || h.bitDepth == 8 || h.bitDepth == 16; break;
                case 3: depthOk = h.bitDepth == 1 || h.bitDepth == 2 || h.bitDepth == 4 || h.bitDepth == 8; break;
                case 2: case 4: case 6: depthOk = h.bitDepth == 8 || h.bitDepth == 16; break;
                default: break;
            }
            if (!depthOk) return broken("PNG colour type or bit depth is invalid.");
            if (data[10] != 0 || data[11] != 0 || data[12] > 1) return broken("PNG compression/filter/interlace is invalid.");
            h.interlaced = data[12] == 1;
            h.color = colorType == 0 ? ColorModel::Gray : colorType == 2 ? ColorModel::RGB
                    : colorType == 3 ? ColorModel::Palette : colorType == 4 ? ColorModel::GrayAlpha : ColorModel::RGBA;
            h.alpha = colorType == 4 || colorType == 6;
            if (Outcome d = checkDims(h, c.lim); !d.ok()) return d;
        } else if (tag(type, "IHDR")) {
            return broken("PNG has a duplicate IHDR.");
        } else if (tag(type, "PLTE")) {
            if (len % 3 != 0 || len == 0 || colorType == 0 || colorType == 4) return broken("PNG palette is invalid.");
            plte = true;
        } else if (tag(type, "tRNS")) {
            h.alpha = true;
        } else if (tag(type, "sRGB")) {
            h.srgbTagged = true;
        } else if (tag(type, "iCCP")) {
            h.hasProfile = true;
            h.profileBytes = len; // compressed size: decode must cap the inflated profile
        } else if (tag(type, "acTL")) {
            if (len != 8 || idat) return broken("PNG animation control chunk is invalid.");
            if (h.frames != 1 || h.animated || sawActl) return broken("PNG has a duplicate animation control chunk.");
            sawActl = true;
            h.frames = be32(data);
            if (h.frames == 0) return broken("PNG animation has no frames.");
            h.animated = h.frames > 1;
        } else if (isIdat) {
            if (colorType == 3 && !plte) return broken("PNG palette is missing.");
            idat = true;
            if (c.lim.prefixOnly) return good;
        } else if (tag(type, "IEND")) {
            if (len != 0) return broken("PNG IEND is malformed.");
            iend = true;
        }
        pos = end;
    }
    if (!ihdr || !idat) return broken("PNG has no image data.");
    if (!iend) return broken("PNG is not terminated by IEND."); // bytes after IEND are tolerated
    return good;
}

// ---- TIFF IFD0 (standalone TIFF and JPEG EXIF) -----------------------------------------------
struct Tiff {
    std::uint32_t width = 0, height = 0, bits = 0, orientation = 0, samples = 0;
    std::uint64_t icc = 0;
    bool extra = false, more = false;
};

// Returns 0 = malformed, 1 = ok, 2 = BigTIFF (classified only).
int parseTiff(u8 const *p, std::size_t n, Tiff &t) noexcept
{
    if (n < 8) return 0;
    bool le = p[0] == 'I' && p[1] == 'I';
    if (!le && !(p[0] == 'M' && p[1] == 'M')) return 0;
    auto r16 = [&](u8 const *q) noexcept { return le ? le16(q) : be16(q); };
    auto r32 = [&](u8 const *q) noexcept { return le ? le32(q) : be32(q); };
    if (r16(p + 2) == 43) return 2;
    if (r16(p + 2) != 42) return 0;
    std::uint64_t ifd = r32(p + 4);
    if (!has(n, ifd, 2)) return 0;
    std::uint32_t count = r16(p + ifd);
    std::uint64_t bytes = std::uint64_t(count) * 12 + 4;
    if (!has(n, ifd + 2, bytes)) return 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        u8 const *e = p + ifd + 2 + std::uint64_t(i) * 12;
        std::uint32_t id = r16(e), type = r16(e + 2), cnt = r32(e + 4);
        std::uint32_t v = type == 3 ? r16(e + 8) : r32(e + 8);
        bool scalar = cnt == 1 && (type == 3 || type == 4);
        if (id == 256 && scalar) t.width = v;
        else if (id == 257 && scalar) t.height = v;
        else if (id == 258 && scalar) t.bits = v;
        else if (id == 277 && scalar) t.samples = v;
        else if (id == 274 && scalar) t.orientation = v;
        else if (id == 338) t.extra = true;
        else if (id == 34675) t.icc = cnt;
    }
    t.more = r32(p + ifd + 2 + std::uint64_t(count) * 12) != 0;
    return 1;
}

Outcome parseTiffFile(u8 const *p, std::size_t n, Ctx const &c, RasterHeader &h) noexcept
{
    h.format = Format::TIFF;
    Tiff t;
    int r = parseTiff(p, n, t);
    if (r == 0) return broken("TIFF header is malformed.");
    if (r == 2) return good; // BigTIFF: classified, dimensions not read
    h.width = t.width;
    h.height = t.height;
    h.bitDepth = t.bits;
    h.alpha = t.extra;
    h.color = t.samples == 1 ? ColorModel::Gray : t.samples == 3 ? ColorModel::RGB : t.samples == 4 ? ColorModel::RGBA : ColorModel::Unknown;
    h.orientation = t.orientation >= 1 && t.orientation <= 8 ? u8(t.orientation) : 0;
    h.hasProfile = t.icc != 0;
    h.profileBytes = t.icc;
    if (t.more) { h.frames = 2; h.animated = true; }
    if (t.icc > c.lim.maxProfileBytes) return refuse("Colour profile is too large.");
    return checkDims(h, c.lim);
}

// ---- JPEG ------------------------------------------------------------------------------------
Outcome parseJpeg(u8 const *p, std::size_t n, Ctx const &c, RasterHeader &h) noexcept
{
    std::uint64_t pos = 2;
    bool sof = false, sos = false, supportedSof = false;
    std::uint32_t segments = 0, iccCount = 0, iccSeen = 0;
    bool exifSeen = false;
    std::uint64_t iccMask[4] = {0, 0, 0, 0};
    while (!sos) {
        if (++segments > 65536) return broken("JPEG marker run is too long.");
        if ((segments & 255) == 0 && c.stop.requested()) return {Status::canceled, "Inspection canceled."};
        if (pos > c.scanLimit) return refuse("Image header metadata exceeds the scan limit.");
        if (!has(n, pos, 2) || p[pos] != 0xFF) return broken(has(n, pos, 2) ? "JPEG marker expected." : "JPEG is truncated.");
        std::uint32_t fill = 0;
        while (has(n, pos, 1) && p[pos] == 0xFF) {
            if (++fill > 256) return broken("JPEG marker run is too long.");
            ++pos;
        }
        if (!has(n, pos, 1)) return broken("JPEG is truncated.");
        u8 m = p[pos++];
        if (m == 0 || m == 0xD8) return broken("JPEG marker is invalid.");
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) return broken("JPEG ended before image data.");
        if (!has(n, pos, 2)) return broken("JPEG is truncated.");
        std::uint32_t len = be16(p + pos);
        if (len < 2) return broken("JPEG segment length is invalid.");
        if (!has(n, pos, len)) return broken("JPEG segment exceeds the data.");
        u8 const *seg = p + pos + 2;
        std::uint32_t slen = len - 2;
        bool isSof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (isSof) {
            if (sof) return broken("JPEG has two frame headers.");
            if (slen < 6) return broken("JPEG frame header is truncated.");
            sof = true;
            supportedSof = m <= 0xC2;
            h.bitDepth = seg[0];
            if (seg[0] != 8) return refuse("Only 8-bit JPEG images are supported.");
            h.height = be16(seg + 1);
            h.width = be16(seg + 3);
            std::uint32_t nc = seg[5];
            if (nc != 1 && nc != 3 && nc != 4) return broken("JPEG component count is invalid.");
            if (slen < 6 + 3 * nc) return broken("JPEG frame header is truncated.");
            if (nc == 4) { h.color = ColorModel::CMYK; return refuse("CMYK JPEG images are not supported."); }
            h.color = nc == 1 ? ColorModel::Gray : ColorModel::RGB;
            h.interlaced = m == 0xC2 || m == 0xCA;
            if (Outcome d = checkDims(h, c.lim); !d.ok()) return d;
        } else if (m == 0xE1 && slen > 6 && std::memcmp(seg, "Exif\0\0", 6) == 0) {
            if (!exifSeen) { // the first Exif segment wins, as in libjpeg/gdk-pixbuf
                exifSeen = true;
                Tiff t;
                if (parseTiff(seg + 6, slen - 6, t) == 1 && t.orientation >= 1 && t.orientation <= 8) h.orientation = u8(t.orientation);
            }
        } else if (m == 0xEE && slen >= 12 && std::memcmp(seg, "Adobe", 5) == 0) {
            if (seg[11] == 2) { h.color = ColorModel::CMYK; return refuse("CMYK JPEG images are not supported."); } // Adobe YCCK
        } else if (m == 0xE2 && slen >= 14 && std::memcmp(seg, "ICC_PROFILE\0", 12) == 0) {
            std::uint32_t seq = seg[12], cnt = seg[13];
            if (cnt == 0 || seq == 0 || seq > cnt || (iccCount && cnt != iccCount) || (iccMask[seq >> 6] >> (seq & 63) & 1)) {
                return broken("JPEG ICC profile chunks are inconsistent.");
            }
            iccCount = cnt;
            iccMask[seq >> 6] |= 1ull << (seq & 63);
            ++iccSeen;
            if (!checkedAdd(h.profileBytes, slen - 14, h.profileBytes) || h.profileBytes > c.lim.maxProfileBytes) {
                return refuse("Colour profile is too large.");
            }
            h.hasProfile = true;
        } else if (m == 0xDA) {
            sos = true;
        }
        pos += len;
    }
    if (!sof) return broken("JPEG has no frame header.");
    if (iccSeen != iccCount) return broken("JPEG ICC profile chunks are missing.");
    if (!c.lim.prefixOnly) { // prefix mode leaves end-of-file checks to decode; only zero padding may follow EOI
        std::size_t end = n;
        while (end > 0 && p[end - 1] == 0) --end;
        if (end < 2 || p[end - 2] != 0xFF || p[end - 1] != 0xD9) return broken("JPEG is not terminated by EOI.");
    }
    if (!supportedSof) return good; // classified below as an unsupported JPEG variant
    h.supported = true;
    return good;
}

// ---- WebP ------------------------------------------------------------------------------------
Outcome parseWebp(u8 const *p, std::size_t n, Ctx const &c, RasterHeader &h) noexcept
{
    std::uint64_t riffEnd = 0;
    if (n < 20 || !checkedAdd(le32(p + 4), 8, riffEnd) || riffEnd < 20) return broken("WebP RIFF size is invalid.");
    if (riffEnd > n && !c.lim.prefixOnly) return broken("WebP is truncated.");
    if (riffEnd & 1) return broken("WebP RIFF size is odd.");
    std::uint64_t pos = 12;
    bool vp8x = false, image = false, first = true, anim = false;
    std::uint32_t iter = 0, cw = 0, ch = 0;
    h.bitDepth = 8;
    h.color = ColorModel::RGB;
    std::uint64_t walkEnd = std::min<std::uint64_t>(riffEnd, n);
    bool vp8xIcc = false, sawIccp = false;
    while (pos < walkEnd) {
        if ((++iter & 255) == 0 && c.stop.requested()) return {Status::canceled, "Inspection canceled."};
        if (!has(walkEnd, pos, 8)) return broken("WebP chunk header is truncated.");
        std::uint64_t csz = le32(p + pos + 4), padded = 0, next = 0;
        if (!checkedAdd(csz, csz & 1, padded) || !checkedAdd(pos + 8, padded, next) || next > riffEnd) return broken("WebP chunk exceeds the RIFF data.");
        u8 const *type = p + pos, *d = p + pos + 8;
        bool imageish = tag(type, "VP8 ") || tag(type, "VP8L") || tag(type, "ANMF");
        if (next > n && !(c.lim.prefixOnly && imageish)) return c.lim.prefixOnly ? refuse("Image header does not fit the inspection window.") : broken("WebP chunk exceeds the RIFF data.");
        if (!image && h.frames == 1 && pos > c.scanLimit) return refuse("Image header metadata exceeds the scan limit.");
        if (first && !tag(type, "VP8X") && !tag(type, "VP8 ") && !tag(type, "VP8L")) return broken("WebP has no image chunk.");
        if (tag(type, "VP8X")) {
            if (!first || csz < 10) return broken("WebP VP8X chunk is malformed.");
            vp8x = true;
            cw = le24(d + 4) + 1;
            ch = le24(d + 7) + 1;
            h.width = cw; h.height = ch;
            h.alpha = d[0] & 0x10;
            anim = d[0] & 0x02;
            h.animated = anim;
            if (d[0] & 0x20) { h.hasProfile = true; vp8xIcc = true; }
            if (Outcome r = checkDims(h, c.lim); !r.ok()) return r;
        } else if (tag(type, "ICCP")) {
            if (csz > c.lim.maxProfileBytes) return refuse("Colour profile is too large.");
            h.hasProfile = true;
            sawIccp = true;
            h.profileBytes = csz;
        } else if (tag(type, "ANMF")) {
            ++h.frames;
            h.animated = true;
        } else if (tag(type, "ALPH")) {
            h.alpha = true;
        } else if ((tag(type, "VP8 ") || tag(type, "VP8L")) && !image) {
            std::uint32_t w = 0, hh = 0;
            if (!has(n, pos + 8, std::min<std::uint64_t>(csz, 10))) return refuse("Image header does not fit the inspection window.");
            if (type[3] == ' ') {
                if (csz < 10 || (d[0] & 1) || d[3] != 0x9D || d[4] != 0x01 || d[5] != 0x2A) return broken("WebP VP8 frame header is invalid.");
                w = le16(d + 6) & 0x3FFF;
                hh = le16(d + 8) & 0x3FFF;
            } else {
                if (csz < 5 || d[0] != 0x2F) return broken("WebP VP8L header is invalid.");
                std::uint32_t bits = le32(d + 1);
                if (bits >> 29) return broken("WebP VP8L version is unsupported.");
                w = (bits & 0x3FFF) + 1;
                hh = ((bits >> 14) & 0x3FFF) + 1;
                if ((bits >> 28) & 1) h.alpha = true;
            }
            if (vp8x && (w != cw || hh != ch)) return broken("WebP canvas and image size disagree.");
            h.width = w; h.height = hh;
            if (Outcome r = checkDims(h, c.lim); !r.ok()) return r;
            image = true;
        }
        first = false;
        pos = next;
        if (c.lim.prefixOnly && (image || h.frames > 1 || (anim && tag(type, "ANMF")))) break;
    }
    if (!c.lim.prefixOnly && vp8xIcc && !sawIccp) return broken("WebP declares a colour profile but has none.");
    if (anim && h.frames == 1) h.frames = 2; // flagged animated without counted frames: treat as multi-frame
    if (!image && !anim) return broken("WebP has no image data.");
    h.supported = true;
    return good;
}

// ---- GIF -------------------------------------------------------------------------------------
Outcome parseGif(u8 const *p, std::size_t n, Ctx const &c, RasterHeader &h) noexcept
{
    if (n < 13) return broken("GIF is truncated.");
    h.width = le16(p + 6);
    h.height = le16(p + 8);
    h.bitDepth = ((p[10] >> 4) & 7) + 1;
    h.color = ColorModel::Palette;
    if (Outcome d = checkDims(h, c.lim); !d.ok()) return d;
    std::uint64_t pos = 13;
    auto skipTable = [&](u8 flags) noexcept {
        if (!(flags & 0x80)) return true;
        std::uint64_t t = 3ull << ((flags & 7) + 1);
        if (!has(n, pos, t)) return false;
        pos += t;
        return true;
    };
    auto skipBlocks = [&]() noexcept {
        for (;;) {
            if (!has(n, pos, 1)) return false;
            u8 s = p[pos++];
            if (s == 0) return true;
            if (!has(n, pos, s)) return false;
            pos += s;
        }
    };
    if (!skipTable(p[10])) return broken("GIF is truncated.");
    h.frames = 0;
    for (std::uint32_t iter = 1;; ++iter) {
        if ((iter & 255) == 0 && c.stop.requested()) return {Status::canceled, "Inspection canceled."};
        if (!has(n, pos, 1)) return broken("GIF is not terminated.");
        u8 b = p[pos++];
        if (b == 0x3B) break;
        if (b == 0x21) {
            if (!has(n, pos, 1)) return broken("GIF is truncated.");
            u8 label = p[pos++];
            if (label == 0xF9 && has(n, pos, 6) && p[pos] == 4 && (p[pos + 1] & 1)) h.alpha = true;
            if (!skipBlocks()) return broken("GIF extension is truncated.");
        } else if (b == 0x2C) {
            if (!has(n, pos, 10)) return broken("GIF image descriptor is truncated.");
            u8 packed = p[pos + 8];
            RasterHeader fr;
            fr.width = le16(p + pos + 4);
            fr.height = le16(p + pos + 6);
            if (Outcome d = checkDims(fr, c.lim); !d.ok()) return d;
            if (std::uint64_t(le16(p + pos)) + fr.width > h.width || std::uint64_t(le16(p + pos + 2)) + fr.height > h.height) {
                return refuse("GIF frame lies outside the logical screen.");
            }
            if (++h.frames == 1) h.interlaced = packed & 0x40;
            if (c.lim.prefixOnly) { // frame count is unknown without the whole file: decode must count frames
                h.supported = true;
                return good;
            }
            pos += 9;
            if (!skipTable(packed) || !has(n, pos, 1)) return broken("GIF is truncated.");
            ++pos; // LZW minimum code size
            if (!skipBlocks()) return broken("GIF image data is truncated.");
        } else {
            return broken("GIF block type is invalid.");
        }
    }
    if (h.frames == 0) return broken("GIF has no image.");
    h.animated = h.frames > 1;
    h.supported = true;
    return good;
}

// ---- BMP -------------------------------------------------------------------------------------
Outcome parseBmp(u8 const *p, std::size_t n, Ctx const &c, RasterHeader &h) noexcept
{
    if (n < 26 || le32(p + 2) > n) return broken("BMP is truncated.");
    std::uint32_t dib = le32(p + 14);
    std::int64_t w = 0, hh = 0;
    if (dib == 12) {
        w = le16(p + 18); hh = le16(p + 20); h.bitDepth = le16(p + 24);
    } else if (dib >= 40 && n >= 54) {
        w = std::int32_t(le32(p + 18)); hh = std::int32_t(le32(p + 22)); h.bitDepth = le16(p + 28);
        if (hh < 0) hh = -hh;
    } else {
        return broken("BMP header is malformed.");
    }
    if (w < 0) return broken("BMP width is invalid.");
    h.width = std::uint32_t(w);
    h.height = std::uint32_t(hh);
    h.alpha = h.bitDepth == 32;
    return checkDims(h, c.lim);
}

// ---- MIME / dispatch ---------------------------------------------------------------------------
// Returns false for a media type we refuse to accept at all; Format::Unsupported = no claim.
bool mimeFormat(std::string_view mime, Format &out) noexcept
{
    out = Format::Unsupported;
    auto semi = mime.find(';');
    if (semi != std::string_view::npos) mime = mime.substr(0, semi);
    while (!mime.empty() && mime.back() == ' ') mime.remove_suffix(1);
    while (!mime.empty() && mime.front() == ' ') mime.remove_prefix(1);
    if (mime.empty() || mime.size() > 40) return mime.empty();
    char lower[48] = {};
    for (std::size_t i = 0; i < mime.size(); ++i) lower[i] = char((mime[i] >= 'A' && mime[i] <= 'Z') ? mime[i] + 32 : mime[i]);
    std::string_view m(lower, mime.size());
    if (m == "application/octet-stream") return true;
    if (m == "image/png") out = Format::PNG;
    else if (m == "image/jpeg" || m == "image/jpg" || m == "image/pjpeg") out = Format::JPEG;
    else if (m == "image/webp") out = Format::WebP;
    else if (m == "image/gif") out = Format::GIF;
    else if (m == "image/tiff") out = Format::TIFF;
    else if (m == "image/bmp" || m == "image/x-ms-bmp") out = Format::BMP;
    else return false;
    return true;
}

Format sniff(u8 const *p, std::size_t n) noexcept
{
    if (n >= 8 && std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return Format::PNG;
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return Format::JPEG;
    if (n >= 12 && tag(p, "RIFF") && tag(p + 8, "WEBP")) return Format::WebP;
    if (n >= 6 && (std::memcmp(p, "GIF87a", 6) == 0 || std::memcmp(p, "GIF89a", 6) == 0)) return Format::GIF;
    if (n >= 4 && (std::memcmp(p, "II*\0", 4) == 0 || std::memcmp(p, "MM\0*", 4) == 0 || std::memcmp(p, "II+\0", 4) == 0 || std::memcmp(p, "MM\0+", 4) == 0)) return Format::TIFF;
    if (n >= 2 && p[0] == 'B' && p[1] == 'M') return Format::BMP;
    return Format::Unsupported;
}

// Parses an already-accounted buffer: caller owns the scratch reservation.
Result<RasterHeader> inspectBytes(EncodedView v, HeaderLimits const &lim, Stop stop) noexcept
{
    Result<RasterHeader> r;
    if (stop.requested()) { r.outcome = {Status::canceled, "Inspection canceled."}; return r; }
    if (!v.data || v.size == 0) { r.outcome = broken("Image data is empty."); return r; }
    Format declared = Format::Unsupported;
    if (!mimeFormat(v.mime, declared)) { r.outcome = refuse("Declared media type is not a supported bitmap type."); return r; }
    Format f = sniff(v.data, v.size);
    r.value.format = f;
    if (f == Format::Unsupported) { r.outcome = refuse("Unrecognised or unsupported image data."); return r; }
    if (declared != Format::Unsupported && declared != f) { r.outcome = refuse("Declared media type and image signature disagree."); return r; }
    std::uint64_t scan = 0;
    if (!checkedAdd(lim.maxProfileBytes, lim.scratchBytes, scan)) scan = UINT64_MAX;
    Ctx c{lim, stop, scan};
    switch (f) {
        case Format::PNG: r.value.supported = true; r.outcome = parsePng(v.data, v.size, c, r.value); break;
        case Format::JPEG: r.outcome = parseJpeg(v.data, v.size, c, r.value); break;
        case Format::WebP: r.outcome = parseWebp(v.data, v.size, c, r.value); break;
        case Format::GIF: r.outcome = parseGif(v.data, v.size, c, r.value); break;
        case Format::TIFF: r.outcome = parseTiffFile(v.data, v.size, c, r.value); break;
        case Format::BMP: r.outcome = parseBmp(v.data, v.size, c, r.value); break;
        default: break;
    }
    RasterHeader &h = r.value;
    if (f == Format::PNG && !r.outcome.ok()) h.supported = false;
    if (!r.outcome.ok()) return r;
    if (lim.requireStill && (h.animated || h.frames > 1)) { r.outcome = refuse("Animated or multi-frame images are not supported."); return r; }
    h.hasProfile = h.hasProfile || h.profileBytes != 0;
    if (!h.supported) r.outcome = refuse("This bitmap format or variant is not supported.");
    return r;
}

int b64Value(char ch) noexcept
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    return ch == '+' ? 62 : ch == '/' ? 63 : -1;
}

} // namespace

Result<RasterHeader> inspect(EncodedView v, HeaderLimits const &lim, Budget &budget, Stop stop) noexcept
{
    Result<RasterHeader> r;
    if (stop.requested()) { r.outcome = {Status::canceled, "Inspection canceled."}; return r; }
    if (!v.data || v.size == 0) { r.outcome = broken("Image data is empty."); return r; }
    // The compressed-size ceiling is enforced before any reservation or parsing.
    if (v.size > lim.maxEncodedBytes) { r.outcome = refuse("Image data exceeds the compressed size limit."); return r; }
    Budget::Token scratch; // accounts the bounded header window; released on every return
    if (Outcome o = budget.acquire(Stage::header, std::min<std::uint64_t>(v.size, lim.scratchBytes), scratch); !o.ok()) {
        r.outcome = o;
        return r;
    }
    return inspectBytes(v, lim, stop);
}

Result<RasterHeader> inspectHref(std::string_view href, HeaderLimits const &lim, Budget &budget, Stop stop) noexcept
{
    Result<RasterHeader> r;
    Result<UriInfo> u = inspectUri(href, lim, stop);
    if (!u.ok()) { r.outcome = u.outcome; return r; }
    if (u.value.kind != UriKind::Data) { r.outcome = refuse("Only embedded images can be inspected."); return r; }
    std::uint64_t want = std::min<std::uint64_t>(u.value.decodedBytes, lim.scratchBytes);
    HeaderLimits l = lim;
    l.prefixOnly = u.value.decodedBytes > lim.scratchBytes;
    PlainBuffer buf;
    if (Outcome o = buf.allocate(budget, Stage::header, want, 1, nullptr, stop); !o.ok()) { r.outcome = o; return r; }
    auto *out = reinterpret_cast<u8 *>(buf.data());
    std::uint64_t got = 0;
    std::uint32_t acc = 0;
    int bits = 0;
    for (std::size_t i = u.value.payloadOffset; i < href.size() && got < want; ++i) {
        if (((i - u.value.payloadOffset) & 0xFFFF) == 0 && stop.requested()) { r.outcome = {Status::canceled, "Inspection canceled."}; return r; }
        int v = b64Value(href[i]);
        if (v < 0) continue; // whitespace/padding were validated by inspectUri
        acc = (acc << 6) | std::uint32_t(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out[got++] = u8(acc >> bits); acc &= (1u << bits) - 1; }
    }
    return inspectBytes({out, std::size_t(got), u.value.mime}, l, stop);
}

Result<UriInfo> inspectUri(std::string_view href, HeaderLimits const &lim, Stop stop) noexcept
{
    Result<UriInfo> r;
    if (href.empty()) { r.outcome = broken("Image reference is empty."); return r; }
    if (href.size() > lim.maxUriBytes) { r.outcome = refuse("Image reference exceeds the size limit."); return r; }
    bool isData = href.size() >= 5;
    for (std::size_t i = 0; isData && i < 5; ++i) isData = ((href[i] | 0x20) == "data:"[i]);
    if (!isData) { r.value.kind = UriKind::Linked; return r; }
    r.value.kind = UriKind::Data;
    std::size_t comma = href.substr(0, 512).find(',');
    if (comma == std::string_view::npos) { r.outcome = broken("Data URI has no payload separator."); return r; }
    std::string_view meta = href.substr(5, comma - 5);
    auto semi = meta.find(';');
    std::string_view mime = meta.substr(0, semi);
    bool b64 = false;
    for (std::string_view rest = semi == std::string_view::npos ? std::string_view{} : meta.substr(semi + 1); !rest.empty();) {
        auto next = rest.find(';');
        std::string_view param = rest.substr(0, next);
        for (char const *w = "base64"; param.size() == 6 && !b64;) { // case-insensitive, any position
            b64 = true;
            for (int k = 0; k < 6; ++k) b64 = b64 && (param[k] | 0x20) == w[k];
            break;
        }
        rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next + 1);
    }
    Format f = Format::Unsupported;
    if (mime.empty() || !mimeFormat(mime, f) || f == Format::Unsupported) { r.outcome = refuse("Data URI media type is not a supported bitmap type."); return r; }
    r.value.mimeFormat = f;
    r.value.mime = mime;
    if (!b64) { r.outcome = refuse("Data URI must be base64 encoded."); return r; }
    std::uint64_t quads = 0, pad = 0;
    std::size_t start = comma + 1;
    for (std::size_t i = start; i < href.size(); ++i) {
        if (((i - start) & 0xFFFFF) == 0 && stop.requested()) { r.outcome = {Status::canceled, "Inspection canceled."}; return r; }
        char ch = href[i];
        if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
        if (ch == '=') { if (++pad > 2) { r.outcome = broken("Base64 padding is invalid."); return r; } ++quads; continue; }
        bool alpha = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
        if (!alpha || pad) { r.outcome = broken(pad ? "Base64 data follows padding." : "Base64 contains an invalid character."); return r; }
        ++quads;
    }
    if (quads == 0 || quads % 4 != 0) { r.outcome = broken("Base64 length is invalid."); return r; }
    std::uint64_t dec = 0;
    if (!checkedMul(quads / 4, 3, dec) || dec < pad) { r.outcome = broken("Base64 length is invalid."); return r; }
    dec -= pad;
    if (dec > lim.maxEncodedBytes) { r.outcome = refuse("Embedded image exceeds the encoded size limit."); return r; }
    r.value.payloadOffset = start;
    r.value.payloadLength = href.size() - start;
    r.value.decodedBytes = dec;
    return r;
}

} // namespace Inkscape::Bitmap
