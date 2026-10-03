// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap engine: qualified straight sRGB RGBA8 decode (EB2-decode).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "util/bitmap-input-decode.h"

#include <algorithm>
#include <cstring>
#include <array>
#include <cstdio>
#include <csetjmp>
#include <exception>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <lcms2.h>
#include <memory>
#include <new>
#include <utility>
#include <zlib.h>

#ifdef HAVE_JPEG
extern "C" {
#include <jpeglib.h>
#include <jerror.h>
}
#endif

namespace Inkscape::Bitmap {
namespace {

constexpr std::size_t kChunk = 64 * 1024;
constexpr char const *kCmyk = "Convert this CMYK image to RGB and embed it before adjusting or exploding it.";
constexpr char const *kProfile = "This image's colour profile cannot be used safely.";

Outcome refuse(char const *why) noexcept { return {Status::incompatible, why}; }
Outcome broken(char const *why) noexcept { return {Status::failed, why}; }

struct LoaderGuard {
    GdkPixbufLoader *l = nullptr;
    bool closed = false;
    void close(GError **e) { closed = true; gdk_pixbuf_loader_close(l, e); }
    ~LoaderGuard()
    {
        if (!l) return;
        if (!closed) gdk_pixbuf_loader_close(l, nullptr); // finalizing an open loader is a GLib warning
        g_object_unref(l);
    }
};
struct ErrGuard {
    GError *e = nullptr;
    ~ErrGuard() { if (e) g_error_free(e); }
};
struct Profile {
    cmsHPROFILE h = nullptr;
    ~Profile() { if (h) cmsCloseProfile(h); }
};

struct Report {
    void (*fn)(void *, std::uint64_t, std::uint64_t, unsigned) = nullptr;
    void *user = nullptr;
    std::uint64_t total = 0, base = 0;
    void at(std::uint64_t done, unsigned phase) const noexcept
    {
        if (!fn) return;
        try { fn(user, done, total, phase); } catch (...) {} // the hook is telemetry; it must not break the decoder
    }
};

struct SizeProbe {
    std::uint32_t w = 0, h = 0;
    bool seen = false, bad = false;
};
void onSize(GdkPixbufLoader *loader, gint w, gint h, gpointer data)
{
    auto *p = static_cast<SizeProbe *>(data);
    p->seen = true;
    if (w < 0 || h < 0 || std::uint32_t(w) != p->w || std::uint32_t(h) != p->h) {
        p->bad = true;
        gdk_pixbuf_loader_set_size(loader, 0, 0); // codecs refuse a zero size before allocating pixels
    }
}

char const *mimeOf(Format f) noexcept
{
    switch (f) {
        case Format::PNG: return "image/png";
        case Format::JPEG: return "image/jpeg";
        case Format::WebP: return "image/webp";
        case Format::GIF: return "image/gif";
        default: return nullptr;
    }
}

// EXIF orientation 1..8: where source pixel (x, y) of a W x H image lands (dw = output width).
void place(unsigned o, std::uint32_t W, std::uint32_t H, std::uint32_t x, std::uint32_t y, std::uint32_t &dx, std::uint32_t &dy) noexcept
{
    switch (o) {
        case 2: dx = W - 1 - x; dy = y; break;
        case 3: dx = W - 1 - x; dy = H - 1 - y; break;
        case 4: dx = x; dy = H - 1 - y; break;
        case 5: dx = y; dy = x; break;
        case 6: dx = H - 1 - y; dy = x; break;
        case 7: dx = H - 1 - y; dy = W - 1 - x; break;
        case 8: dx = y; dy = W - 1 - x; break;
        default: dx = x; dy = y; break;
    }
}

constexpr unsigned kMaxScans = 100;       // normal progressive JPEG uses <= ~20 scans; libjpeg-turbo's default cap is 1000
constexpr std::uint64_t kInflateState = 64 * 1024;

std::uint64_t fnv(EncodedView v, Stop stop, bool &stopped) noexcept
{
    std::uint64_t h = 1469598103934665603ull;
    stopped = false;
    for (std::size_t i = 0; i < v.size; ++i) {
        if ((i & 0xFFFFF) == 0 && stop.requested()) { stopped = true; return 0; }
        h = (h ^ v.data[i]) * 1099511628211ull;
    }
    return h;
}

// Feeds `len` bytes into z and discards the output. Z_BUF_ERROR only means "needs more input" (zero-length
// IDAT, buffer filled exactly at a chunk end). Stops as soon as the output passes `limit`.
Outcome feed(z_stream &z, std::uint8_t const *in, std::uint64_t len, PlainBuffer &sink, std::uint64_t &total, std::uint64_t limit, int &zr, Stop stop, Report const &rep) noexcept
{
    if (len == 0) return {Status::changed, ""};
    z.next_in = const_cast<Bytef *>(in);
    z.avail_in = uInt(len);
    do {
        if (stop.requested()) return {Status::canceled, "Decode canceled."};
        z.next_out = reinterpret_cast<Bytef *>(sink.data());
        z.avail_out = kChunk;
        zr = inflate(&z, Z_NO_FLUSH);
        total += kChunk - z.avail_out;
        rep.at(rep.base + (len - z.avail_in), 0);
        if (total > limit) return refuse("The image carries more compressed data than allowed.");
        if (zr == Z_BUF_ERROR) { zr = Z_OK; break; }
    } while (zr == Z_OK && (z.avail_in || z.avail_out == 0));
    return {Status::changed, ""};
}

// libpng (inside GdkPixbuf) tolerates a corrupt IDAT zlib stream and keeps up to ~8 MB per text chunk, so the
// decoder walks every chunk first: IDAT must inflate to exactly the Adam7-aware scanline size (Adler-32 checked,
// Z_STREAM_END, nothing trailing); every zTXt/iTXt/iCCP is inflated with a bound and all text/profile bytes
// together inflate to at most maxProfileBytes (plain tEXt is bounded by the file size, so it is not counted).
Outcome verifyPng(EncodedView v, RasterHeader const &h, HeaderLimits const &lim, Budget &budget, Stop stop, Report rep) noexcept
{
    static char const *const bad = "The PNG image data stream is corrupt.";
    unsigned samples = 0;
    switch (h.color) {
        case ColorModel::Gray: case ColorModel::Palette: samples = 1; break;
        case ColorModel::GrayAlpha: samples = 2; break;
        case ColorModel::RGB: samples = 3; break;
        case ColorModel::RGBA: samples = 4; break;
        default: return refuse("This bitmap format or variant is not supported.");
    }
    std::uint64_t want = 0, bits = std::uint64_t(samples) * h.bitDepth;
    static unsigned const x0[7] = {0, 4, 0, 2, 0, 1, 0}, dx[7] = {8, 8, 4, 4, 2, 2, 1}, y0[7] = {0, 0, 4, 0, 2, 0, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
    for (unsigned pass = 0; pass < (h.interlaced ? 7u : 1u); ++pass) {
        std::uint64_t pw = h.width, ph = h.height;
        if (h.interlaced) {
            pw = h.width > x0[pass] ? (h.width - x0[pass] + dx[pass] - 1) / dx[pass] : 0;
            ph = h.height > y0[pass] ? (h.height - y0[pass] + dy[pass] - 1) / dy[pass] : 0;
        }
        std::uint64_t rowBytes = 0, add = 0;
        if (pw && ph && (!checkedCeilDiv(pw * bits, 8, rowBytes) || !checkedMul(ph, rowBytes + 1, add) || !checkedAdd(want, add, want)))
            return refuse("Image has too many pixels.");
    }
    PlainBuffer sink;
    if (Outcome o = sink.allocate(budget, Stage::decode, kChunk, 1); !o.ok()) return o;
    Budget::Token state; // two inflate states (image + text chunk), each under 64 KiB
    if (Outcome o = budget.acquire(Stage::decode, 2 * kInflateState, state); !o.ok()) return o;
    z_stream img{};
    if (inflateInit(&img) != Z_OK) return broken("Not enough memory to verify the image data.");
    int zr = Z_OK;
    std::uint64_t total = 0, textTotal = 0;
    bool sawIdat = false, sawEnd = false;
    Outcome out{Status::changed, ""};
    std::size_t pos = 8;
    while (out.ok() && pos + 12 <= v.size && !sawEnd) {
        std::uint8_t const *c = v.data + pos;
        std::uint64_t len = (std::uint64_t(c[0]) << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
        if (pos + 12 + len > v.size) { out = broken(bad); break; }
        std::uint8_t const *d = c + 8;
        auto is = [&](char const *t) { return std::memcmp(c + 4, t, 4) == 0; };
        if (is("IDAT")) {
            sawIdat = true;
            if (zr == Z_STREAM_END && len) { out = broken(bad); break; } // data after the zlib stream ended
            rep.base = std::uint64_t(d - v.data);
            out = feed(img, d, len, sink, total, want, zr, stop, rep);
            if (out.ok() && zr != Z_OK && zr != Z_STREAM_END) out = broken(bad);
            if (out.ok() && zr == Z_STREAM_END && img.avail_in) out = broken(bad);
        } else if (is("IEND")) {
            sawEnd = true;
        } else if (is("zTXt") || is("iTXt") || is("iCCP")) { // plain tEXt is bounded by the file size
            std::uint64_t k = 0, off = len; // off = start of a zlib stream inside the chunk (len = none)
            {
                while (k < len && k < 80 && d[k]) ++k;
                if (k >= len || d[k]) { out = broken(bad); break; } // keyword is not NUL terminated
                off = k + 2; // iCCP/zTXt: NUL, method byte, zlib stream
                if (is("iTXt")) {
                    off = len; // uncompressed unless the flag says otherwise
                    if (k + 3 <= len && d[k + 1] == 1) {
                        std::uint64_t q = k + 3;
                        for (int i = 0; i < 2 && out.ok(); ++i) { // language tag, translated keyword
                            while (q < len && d[q]) ++q;
                            if (q >= len) out = broken(bad);
                            ++q;
                        }
                        if (!out.ok()) break;
                        off = q;
                    }
                }
            }
            if (off < len) {
                z_stream t{};
                if (inflateInit(&t) != Z_OK) { out = broken("Not enough memory to verify the image data."); break; }
                int tr = Z_OK;
                std::uint64_t got = 0, room = lim.maxProfileBytes - textTotal;
                rep.base = std::uint64_t(d + off - v.data);
                out = feed(t, d + off, len - off, sink, got, room, tr, stop, rep);
                inflateEnd(&t);
                if (out.ok() && !checkedAdd(textTotal, got, textTotal)) out = refuse("The image carries more text or profile data than allowed.");
            }
        }
        pos += 12 + len;
    }
    inflateEnd(&img);
    if (!out.ok()) return out;
    return zr == Z_STREAM_END && sawIdat && sawEnd && total == want ? Outcome{Status::changed, ""} : broken(bad);
}

#ifdef HAVE_JPEG
struct JpegErr {
    jpeg_error_mgr pub;
    std::jmp_buf jb;
    unsigned damage = 0;
    bool stopped = false;
    Stop const *stop = nullptr;
    Report const *rep = nullptr;
};
extern "C" {
static void jpegFatal(j_common_ptr c) { std::longjmp(reinterpret_cast<JpegErr *>(c->err)->jb, 1); }
// Only data damage is refused. Extraneous bytes, JFIF version and Adobe transform warnings do not hurt the
// pixels and the canvas shows such files, so they are tolerated.
static void jpegMessage(j_common_ptr c, int level)
{
    if (level >= 0) return;
    switch (reinterpret_cast<JpegErr *>(c->err)->pub.msg_code) {
        case JWRN_JPEG_EOF: case JWRN_HIT_MARKER: case JWRN_HUFF_BAD_CODE: case JWRN_MUST_RESYNC:
            ++reinterpret_cast<JpegErr *>(c->err)->damage;
            break;
        default: break;
    }
}
static void jpegSilent(j_common_ptr) {}
static void jpegProgress(j_common_ptr c)
{
    auto *e = reinterpret_cast<JpegErr *>(c->err);
    auto *p = c->progress;
    if (e->rep && p->pass_limit > 0) e->rep->at(e->rep->total * std::uint64_t(p->pass_counter) / std::uint64_t(p->pass_limit), 0);
    if (e->stop->requested()) { e->stopped = true; std::longjmp(e->jb, 1); }
}
}

// 0 = clean, 1 = fatal error, 2 = stopped, 3 = damage warning (premature EOI, bad Huffman data, resync).
// Progressive files use jpeg_read_coefficients (entropy decode only, no IDCT or colour conversion; its
// coefficient arrays are covered by the 6 B/px reservation for progressive JPEG); baseline files stream rows.
// Plain locals only: this function longjmps.
int jpegCheck(std::uint8_t const *data, std::size_t n, Stop const &stop, Report const &rep) noexcept
{
    jpeg_decompress_struct c;
    jpeg_progress_mgr prog;
    JpegErr e;
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = jpegFatal;
    e.pub.emit_message = jpegMessage;
    e.pub.output_message = jpegSilent;
    e.stop = &stop;
    e.rep = &rep;
    prog.progress_monitor = jpegProgress;
    int result = 0;
    if (setjmp(e.jb)) { jpeg_destroy_decompress(&c); return e.stopped ? 2 : 1; }
    jpeg_create_decompress(&c);
    c.progress = &prog;
    jpeg_mem_src(&c, const_cast<std::uint8_t *>(data), n);
    jpeg_read_header(&c, TRUE);
    if (c.progressive_mode) {
        jpeg_read_coefficients(&c);
    } else {
        jpeg_start_decompress(&c);
        JSAMPARRAY row = (*c.mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(&c), JPOOL_IMAGE, c.output_width * c.output_components, 1);
        while (c.output_scanline < c.output_height) {
            if ((c.output_scanline & 63) == 0) {
                if (stop.requested()) { result = 2; break; }
                rep.at(n * std::uint64_t(c.output_scanline) / c.output_height, 0);
            }
            jpeg_read_scanlines(&c, row, 1);
        }
    }
    if (!result) jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    return result ? result : (e.damage ? 3 : 0);
}
#else
int jpegCheck(std::uint8_t const *, std::size_t, Stop const &, Report const &) noexcept { return 4; } // no libjpeg: cannot verify
#endif

struct Cuts { std::array<std::size_t, kMaxScans + 1> at{}; unsigned count = 0; };

// Counts SOS scans (cap kMaxScans) and records where each starts, so writes can be split per scan.
Outcome walkJpeg(EncodedView v, Cuts &cuts, Stop stop) noexcept
{
    std::uint8_t const *d = v.data;
    std::size_t n = v.size, pos = 2, polled = 0;
    while (pos + 2 <= n) {
        if (pos - polled > MiB) { if (stop.requested()) return {Status::canceled, "Decode canceled."}; polled = pos; }
        while (pos < n && d[pos] != 0xFF) ++pos; // skip stray bytes like libjpeg's next_marker
        if (pos + 2 > n) break;
        std::uint8_t m = d[pos + 1];
        if (m == 0xFF) { ++pos; continue; }
        if (m == 0) { pos += 2; continue; } // FF 00 outside entropy data is discarded by libjpeg too
        if (m == 0xD9) break;
        std::size_t start = pos;
        pos += 2;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) continue;
        if (pos + 2 > n) return broken("The JPEG data is corrupt.");
        std::size_t len = (std::size_t(d[pos]) << 8) | d[pos + 1];
        if (len < 2 || pos + len > n) return broken("The JPEG data is corrupt.");
        pos += len;
        if (m != 0xDA) continue;
        if (cuts.count >= kMaxScans) return refuse("The JPEG has too many scans.");
        cuts.at[cuts.count++] = start;
        for (;;) { // entropy-coded data up to the next real marker
            auto const *q = static_cast<std::uint8_t const *>(std::memchr(d + pos, 0xFF, n - pos));
            if (!q || q + 1 >= d + n) { pos = n; break; }
            pos = std::size_t(q - d);
            std::uint8_t b = d[pos + 1];
            if (b == 0 || (b >= 0xD0 && b <= 0xD7)) { pos += 2; continue; }
            if (b == 0xFF) { ++pos; continue; }
            break;
        }
    }
    return {Status::changed, ""};
}

} // namespace

std::uint64_t fingerprint(EncodedView v) noexcept
{
    bool stopped = false;
    return fnv(v, {}, stopped);
}

RgbaView DecodedRaster::view() const noexcept
{
    RgbaView v;
    v.data = reinterpret_cast<std::uint8_t const *>(pixels.data());
    v.bytes = pixels.size();
    v.stride = std::uint64_t(width) * 4;
    v.width = width;
    v.height = height;
    return v;
}

namespace {

Result<DecodedRaster> run(ValidatedInput const &in, Budget &budget, Stop stop, AllocationFault *fault)
{
    Result<DecodedRaster> r;
    auto fail = [&](Outcome o) { r.value = DecodedRaster{}; r.outcome = o; return std::move(r); };
    auto stopped = [&]() { return fail({Status::canceled, "Decode canceled."}); };
    if (stop.requested()) return stopped();
    EncodedView const enc = in.encoded;
    if (!enc.data || enc.size == 0) return fail(broken("Image data is empty."));

    // 1. Whole-file validation: CRC, IEND/EOI/RIFF end/GIF trailer, frame count (the prefix gate left these to us).
    HeaderLimits lim = in.limits;
    lim.prefixOnly = false;
    Result<RasterHeader> hr = inspect(enc, lim, budget, stop);
    RasterHeader const &h = hr.value;
    if (!hr.ok()) {
        r.value.format = h.format;
        r.outcome = h.color == ColorModel::CMYK && hr.outcome.status == Status::incompatible ? refuse(kCmyk) : hr.outcome;
        return r;
    }
    if (h.color == ColorModel::CMYK) return fail(refuse(kCmyk));
    if (!h.supported || h.animated || h.frames != 1) return fail(refuse("This bitmap format or variant is not supported."));
    if (h.width == 0 || h.height == 0 || h.width > lim.maxAxis || h.height > lim.maxAxis) return fail(refuse("Image dimensions are outside the supported range."));
    std::uint64_t px = 0, outBytes = 0, total = 0, extra = 0;
    if (!checkedMul(h.width, h.height, px) || px > lim.maxPixels || !checkedMul(px, 4, outBytes)) return fail(refuse("Image has too many pixels."));
    if ((in.expectWidth && in.expectWidth != h.width) || (in.expectHeight && in.expectHeight != h.height))
        return fail(broken("Image size differs from the size read by the header gate."));
    // codec pixbuf (<= 4 B/px) + codec-side buffers bounded by the input; progressive JPEG also holds coefficients
    if (h.interlaced && h.format == Format::JPEG && !checkedMul(px, 6, extra)) return fail(refuse("Image has too many pixels."));
    if (!checkedAdd(outBytes, std::min<std::uint64_t>(enc.size, lim.maxEncodedBytes) + MiB, total) || !checkedAdd(total, extra, total))
        return fail(refuse("Image has too many pixels."));

    // 2. Reserve everything before the first allocation: output raster, then codec pixbuf and scratch.
    PlainBuffer out;
    if (Outcome o = out.allocate(budget, Stage::canonical, px, 4, fault, stop); !o.ok()) return fail(o);
    Budget::Token codecToken;
    if (Outcome o = budget.acquire(Stage::decode, total, codecToken); !o.ok()) return fail(o);
    if (fault && fault->fail()) return fail(broken("Not enough memory for the image decoder."));

    // 3. Metadata/stream verification before GdkPixbuf sees a byte.
    Report rep{in.progress, in.user, enc.size, 0};
    Cuts cuts;
    if (h.format == Format::PNG) {
        if (Outcome o = verifyPng(enc, h, lim, budget, stop, rep); !o.ok()) return fail(o);
    } else if (h.format == Format::JPEG) {
        if (Outcome o = walkJpeg(enc, cuts, stop); !o.ok()) return fail(o);
        int jc = jpegCheck(enc.data, enc.size, stop, rep);
        if (jc == 2) return stopped();
        if (jc == 4) return fail({Status::unavailable, "JPEG verification is not available in this build."});
        if (jc == 3) return fail(broken("The JPEG image data is damaged (premature end or corrupt entropy data)."));
        if (jc) return fail(broken("The JPEG image data is corrupt."));
    }
    if (stop.requested()) return stopped();

    // 4. Incremental decode, Stop polled between chunks (JPEG: writes never span a scan start).
    ErrGuard err;
    SizeProbe probe{h.width, h.height}; // outlives the loader it is connected to
    LoaderGuard lg;
    lg.l = gdk_pixbuf_loader_new_with_mime_type(mimeOf(h.format), &err.e);
    if (!lg.l) return fail({Status::unavailable, "No decoder for this image format is installed."});
    g_signal_connect(lg.l, "size-prepared", G_CALLBACK(onSize), &probe);
    unsigned nextCut = 0;
    for (std::size_t pos = 0; pos < enc.size;) {
        if (stop.requested()) return stopped();
        while (nextCut < cuts.count && cuts.at[nextCut] <= pos) ++nextCut;
        std::size_t n = std::min(kChunk, enc.size - pos);
        if (nextCut < cuts.count) n = std::min(n, cuts.at[nextCut] - pos);
        if (!gdk_pixbuf_loader_write(lg.l, enc.data + pos, n, &err.e)) {
            lg.closed = true; // a failed write closes the loader itself
            return fail(probe.bad ? broken("Decoded size differs from the image header.") : broken("The image data is corrupt or truncated."));
        }
        pos += n;
        if (in.progress) try { in.progress(in.user, pos, enc.size, 1); } catch (...) {}
    }
    lg.close(&err.e);
    if (err.e) return fail(broken("The image data is corrupt or incomplete."));
    GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(lg.l);
    if (!pb) return fail(broken("The decoder produced no image."));
    if (probe.bad) return fail(broken("Decoded size differs from the image header."));
    int const ch = gdk_pixbuf_get_n_channels(pb);
    std::uint64_t const stride = gdk_pixbuf_get_rowstride(pb);
    guchar const *src = gdk_pixbuf_read_pixels(pb);
    if (std::uint32_t(gdk_pixbuf_get_width(pb)) != h.width || std::uint32_t(gdk_pixbuf_get_height(pb)) != h.height)
        return fail(broken("Decoded size differs from the image header."));
    if (!src || gdk_pixbuf_get_bits_per_sample(pb) != 8 || gdk_pixbuf_get_colorspace(pb) != GDK_COLORSPACE_RGB || (ch != 3 && ch != 4) ||
        stride < std::uint64_t(h.width) * ch || gdk_pixbuf_get_has_alpha(pb) != (ch == 4))
        return fail(broken("The decoder returned an unexpected pixel layout."));
    // GdkPixbuf's option is the authority (absent = 1): it is exactly what the canvas applies. The header's EXIF
    // value is only a cross-check when both exist.
    unsigned orient = 1;
    if (char const *o = gdk_pixbuf_get_option(pb, "orientation")) {
        int v = std::atoi(o);
        if (v >= 1 && v <= 8) orient = unsigned(v);
        if (h.orientation && unsigned(v) != h.orientation) return fail(broken("The image orientation metadata is inconsistent."));
    }
    bool const swap = orient >= 5;
    std::uint32_t const ow = swap ? h.height : h.width, oh = swap ? h.width : h.height;

    // 5. Embedded profile: validated and carried, never applied (the canvas ignores it too).
    PlainBuffer profile;
    std::uint64_t profileBytes = 0;
    if (h.hasProfile && !h.srgbTagged) {
        char const *b64 = gdk_pixbuf_get_option(pb, "icc-profile");
        if (!b64 || !*b64) return fail(refuse(kProfile));
        std::uint64_t textLen = std::strlen(b64), maxText = 0;
        if (!checkedMul(lim.maxProfileBytes, 2, maxText) || textLen > maxText) return fail(refuse("Colour profile is too large."));
        if (Outcome o = profile.allocate(budget, Stage::canonical, textLen / 4 * 3 + 3, 1, fault, stop); !o.ok()) return fail(o);
        gint state = 0; guint save = 0;
        profileBytes = g_base64_decode_step(b64, textLen, reinterpret_cast<guchar *>(profile.data()), &state, &save);
        if (profileBytes == 0 || profileBytes > lim.maxProfileBytes) return fail(refuse(profileBytes ? "Colour profile is too large." : kProfile));
        Profile prof;
        prof.h = cmsOpenProfileFromMem(profile.data(), cmsUInt32Number(profileBytes));
        if (!prof.h) return fail(refuse(kProfile));
        auto space = cmsGetColorSpace(prof.h);
        auto cls = cmsGetDeviceClass(prof.h);
        if (space == cmsSigCmykData) return fail(refuse(kCmyk));
        bool const grayImage = h.color == ColorModel::Gray || h.color == ColorModel::GrayAlpha;
        if (space != (grayImage ? cmsSigGrayData : cmsSigRgbData) || cls == cmsSigLinkClass || cls == cmsSigAbstractClass || cls == cmsSigNamedColorClass)
            return fail(refuse(kProfile));
    }

    // 6. One pass: expand to RGBA, apply orientation straight into the output (values unchanged).
    auto *dst = reinterpret_cast<std::uint8_t *>(out.data());
    for (std::uint32_t y = 0; y < h.height; ++y) {
        if ((y & 63) == 0 && stop.requested()) return stopped();
        guchar const *s = src + y * stride;
        for (std::uint32_t x = 0; x < h.width; ++x, s += ch) {
            std::uint32_t dx, dy;
            place(orient, h.width, h.height, x, y, dx, dy);
            std::uint8_t *o = dst + (std::uint64_t(dy) * ow + dx) * 4;
            o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = ch == 4 ? s[3] : 255;
        }
    }
    bool hashStopped = false;
    std::uint64_t const hash = fnv(enc, stop, hashStopped);
    if (hashStopped) return stopped();

    DecodedRaster &d = r.value;
    d.pixels = std::move(out);
    d.profile = std::move(profile);
    d.profileBytes = profileBytes;
    d.width = ow; d.height = oh;
    d.sourceWidth = h.width; d.sourceHeight = h.height;
    d.format = h.format;
    d.orientation = std::uint8_t(orient);
    d.hadAlpha = h.alpha || ch == 4;
    d.reduced16 = h.bitDepth == 16;
    d.original = enc;
    d.originalBytes = enc.size;
    d.originalHash = hash;
    r.consumed = enc.size;
    r.outcome = {Status::changed, ""};
    return r;
}

} // namespace

Result<DecodedRaster> decode(ValidatedInput const &in, Budget &budget, Stop stop, AllocationFault *fault) noexcept
{
    try {
        return run(in, budget, stop, fault);
    } catch (std::exception const &e) {
        Result<DecodedRaster> r;
        r.outcome = broken(dynamic_cast<std::bad_alloc const *>(&e) ? "Not enough memory to decode the image." : "The image decoder failed unexpectedly.");
        return r;
    } catch (...) {
        Result<DecodedRaster> r;
        r.outcome = broken("The image decoder failed unexpectedly.");
        return r;
    }
}

} // namespace Inkscape::Bitmap
