// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Helper functions to use cairo with inkscape
 *
 * Copyright (C) 2007 bulia byak
 * Copyright (C) 2008 Johan Engelen
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 */

#include "display/cairo-utils.h"

#include <2geom/affine.h>
#include <2geom/curves.h>
#include <2geom/path-sink.h>
#include <2geom/path.h>
#include <2geom/pathvector.h>
#include <2geom/point.h>
#include <2geom/sbasis-to-bezier.h>
#include <2geom/transforms.h>
#include <boost/algorithm/string.hpp>
#include <boost/operators.hpp>
#include <boost/optional/optional.hpp>
#include <array>
#include <cairomm/context.h>
#include <cairomm/matrix.h>
#include <cairomm/pattern.h>
#include <cairomm/refptr.h>
#include <cairomm/surface.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <cstdint>
#include <new>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <zlib.h>
#include <tiffio.h>
#include <glibmm/fileutils.h>

#include "cairo-templates.h"
#include "colors/manager.h"
#include "colors/spaces/rgb.h"
#include "colors/utils.h"
#include "document.h"
#include "helper/pixbuf-ops.h"
#include "preferences.h"
#include "ui/util.h"
#include "util/scope_exit.h"
#include "util/bitmap-input-header.h"
#include "util/bitmap-memory-admission.h"
#include "util/units.h"
#include "util/uri.h"

#if CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 17, 6)
#define CAIRO_HAS_HAIRLINE
#endif

/**
 * Key for cairo_surface_t to keep track of current color interpolation value
 * Only the address of the structure is used, it is never initialized. See:
 * http://www.cairographics.org/manual/cairo-Types.html#cairo-user-data-key-t
 */
static cairo_user_data_key_t ink_color_interpolation_key;

namespace Inkscape {

void copy_supported_pixbuf_metadata(GdkPixbuf *source, GdkPixbuf *destination)
{
    // EXIF orientation is intentionally excluded. SPImage applies it while
    // decoding; propagating it to an edited PNG would rotate the pixels again
    // when the replacement is reopened. These options describe the edited
    // pixel payload without changing its orientation or interpretation.
    static constexpr std::array metadata_keys = {"icc-profile", "x-dpi", "y-dpi"};
    for (auto const *key : metadata_keys) {
        if (auto const *value = gdk_pixbuf_get_option(source, key)) {
            gdk_pixbuf_set_option(destination, key, value);
        }
    }
}

/* The class below implement the following hack:
 * 
 * The pixels formats of Cairo and GdkPixbuf are different.
 * GdkPixbuf accesses pixels as bytes, alpha is not premultiplied,
 * and successive bytes of a single pixel contain R, G, B and A components.
 * Cairo accesses pixels as 32-bit ints, alpha is premultiplied,
 * and each int contains as 0xAARRGGBB, accessed with bitwise operations.
 *
 * In other words, on a little endian system, a GdkPixbuf will contain:
 *   char *data = "rgbargbargba...."
 *   int *data = { 0xAABBGGRR, 0xAABBGGRR, 0xAABBGGRR, ... }
 * while a Cairo image surface will contain:
 *   char *data = "bgrabgrabgra...."
 *   int *data = { 0xAARRGGBB, 0xAARRGGBB, 0xAARRGGBB, ... }
 *
 * It is possible to convert between these two formats (almost) losslessly.
 * Some color information from partially transparent regions of the image
 * is lost, but the result when displaying this image will remain the same.
 *
 * The class allows interoperation between GdkPixbuf
 * and Cairo surfaces without creating a copy of the image.
 * This is implemented by creating a GdkPixbuf and a Cairo image surface
 * which share their data. Depending on what is needed at a given time,
 * the pixels are converted in place to the Cairo or the GdkPixbuf format.
 */

/** Create a pixbuf from a Cairo surface.
 * The constructor takes ownership of the passed surface,
 * so it should not be destroyed. */
Pixbuf::Pixbuf(cairo_surface_t *s)
    : _pixbuf(gdk_pixbuf_new_from_data(
        cairo_image_surface_get_data(s), GDK_COLORSPACE_RGB, TRUE, 8,
        cairo_image_surface_get_width(s), cairo_image_surface_get_height(s),
        cairo_image_surface_get_stride(s),
        ink_cairo_pixbuf_cleanup, s))
    , _surface(s)
    , _mod_time(0)
    , _pixel_format(PF_CAIRO)
    , _cairo_store(true)
{}

/** Create a pixbuf from a GdkPixbuf.
 * The constructor takes ownership of the passed GdkPixbuf reference,
 * so it should not be unrefed. */
Pixbuf::Pixbuf(GdkPixbuf *pb)
    : _pixbuf(pb)
    , _surface(nullptr)
    , _mod_time(0)
    , _pixel_format(PF_GDK)
    , _cairo_store(false)
{
    _forceAlpha();
    _surface = cairo_image_surface_create_for_data(
        gdk_pixbuf_get_pixels(_pixbuf), CAIRO_FORMAT_ARGB32,
        gdk_pixbuf_get_width(_pixbuf), gdk_pixbuf_get_height(_pixbuf), gdk_pixbuf_get_rowstride(_pixbuf));
}

Pixbuf::Pixbuf(Inkscape::Pixbuf const &other)
    : _pixbuf(gdk_pixbuf_copy(other._pixbuf))
    , _surface(cairo_image_surface_create_for_data(
        gdk_pixbuf_get_pixels(_pixbuf), CAIRO_FORMAT_ARGB32,
        gdk_pixbuf_get_width(_pixbuf), gdk_pixbuf_get_height(_pixbuf), gdk_pixbuf_get_rowstride(_pixbuf)))
    , _mod_time(other._mod_time)
    , _path(other._path)
    , _pixel_format(other._pixel_format)
    , _cairo_store(false)
{
    // gdk_pixbuf_copy() deliberately copies only pixels. Preserve only
    // metadata that remains valid for a newly encoded, already-oriented PNG.
    copy_supported_pixbuf_metadata(other._pixbuf, _pixbuf);
}

Pixbuf::~Pixbuf()
{
    if (!_cairo_store) {
        cairo_surface_destroy(_surface);
    }
    g_object_unref(_pixbuf);
}

#if !GDK_PIXBUF_CHECK_VERSION(2, 41, 0)
/**
 * Incremental file read introduced to workaround
 * https://gitlab.gnome.org/GNOME/gdk-pixbuf/issues/70
 */
static bool _workaround_issue_70__gdk_pixbuf_loader_write( //
    GdkPixbufLoader *loader, guchar *decoded, gsize decoded_len, GError **error)
{
    bool success = true;
    gsize bytes_left = decoded_len;
    gsize secret_limit = 0xffff;
    guchar *decoded_head = decoded;
    while (bytes_left && success) {
        gsize bytes = (bytes_left > secret_limit) ? secret_limit : bytes_left;
        success = gdk_pixbuf_loader_write(loader, decoded_head, bytes, error);
        decoded_head += bytes;
        bytes_left -= bytes;
    }

    return success;
}
#define gdk_pixbuf_loader_write _workaround_issue_70__gdk_pixbuf_loader_write
#endif

/**
 * Create a new Pixbuf with the image cropped to the given area.
 */
Pixbuf *Pixbuf::cropTo(const Geom::IntRect &area) const
{
    GdkPixbuf *copy = nullptr;
    auto source = _pixbuf;
    if (_pixel_format == PF_CAIRO) {
        // This copies twice, but can be run on const, which is useful.
        copy = gdk_pixbuf_copy(_pixbuf);
        ensure_pixbuf(copy);
        source = copy;
    }
    auto cropped = gdk_pixbuf_new_subpixbuf(source,
        area.left(), area.top(), area.width(), area.height());
    if (copy) {
        // Clean up our pixbuf copy
        g_object_unref(copy);
    }
    return new Pixbuf(cropped);
}

// BUG-015: private translation-unit interface (also exercised by the open tests).
// No widely included header changes. Diagnostics use static storage even on OOM.
static thread_local char const *open_diagnostic = "";
static thread_local bool open_refused = false;
static thread_local Bitmap::Outcome open_admission_outcome;
static thread_local bool open_admission_refused = false;
char const *image_open_diagnostic() { return open_diagnostic; }
bool image_open_refused() { return open_refused; }
void image_open_reset() { open_diagnostic = ""; open_refused = false; open_admission_refused = false; }
static bool refuse_open(char const *why)
{
    if (!open_admission_refused) open_diagnostic = why;
    open_refused = true;
    return false;
}

std::uint64_t image_open_memory_budget(Bitmap::Memory const &m)
{
    // No upward guess when the native probe is unavailable.
    std::uint64_t limit = 0;
    auto outcome = Bitmap::admissionLimit(m, limit);
    if (outcome.ok()) return limit;
    open_admission_outcome = outcome; // owns the diagnostic after this call returns
    open_admission_refused = true;
    refuse_open(open_admission_outcome.diagnostic);
    open_diagnostic = open_admission_outcome.diagnostic;
    return 0;
}
std::uint64_t image_open_memory_budget(Bitmap::Result<Bitmap::Memory> const &sample)
{
    if (sample.ok()) return image_open_memory_budget(sample.value);
    open_admission_outcome = sample.outcome;
    open_admission_refused = true;
    open_refused = true;
    open_diagnostic = open_admission_outcome.diagnostic;
    return 0;
}
static constexpr std::uint64_t open_buffer_ceiling = std::uint64_t{4} * 1024 * Bitmap::MiB;
static constexpr std::uint64_t open_scratch = 16 * Bitmap::MiB;
static std::uint64_t open_budget()
{
    return image_open_memory_budget(Bitmap::sampleMemory());
}
static bool encoded_fits(std::uint64_t n, std::uint64_t budget)
{
    std::uint64_t peak;
    return (n <= open_buffer_ceiling && n <= budget / 2 && n <= SIZE_MAX &&
            Bitmap::checkedMul(n, 2, peak) && Bitmap::checkedAdd(peak, open_scratch, peak) && peak <= budget)
        || refuse_open("encoded image buffer exceeds the available-memory limit");
}
// GLib accepts whitespace and other non-alphabet bytes. Use the encoded length's
// checked upper bound, preserving that permissive behaviour without g_base64_decode's
// aborting allocation (and without a second decoded-byte copy).
bool image_open_base64_capacity(std::size_t length, std::uint64_t budget, std::size_t &capacity)
{
    std::uint64_t groups, bytes;
    return (Bitmap::checkedCeilDiv(length, 4, groups) && Bitmap::checkedMul(groups, 3, bytes) &&
            encoded_fits(bytes, budget) && Bitmap::checkedSize(bytes, capacity))
        || refuse_open("base64 decoded buffer exceeds the available-memory limit");
}
bool image_open_dimensions_fit(std::uint64_t w, std::uint64_t h, std::uint64_t encoded, std::uint64_t budget)
{
    std::uint64_t rgba, input, peak;
    // 3 RGBA equivalents cover codec pixels + alpha/orientation overlap and
    // progressive JPEG coefficients (6 B/px). 2 encoded buffers cover TIFF's copy.
    return (w && h && w <= INT_MAX / 4 && h <= INT_MAX &&
            Bitmap::checkedMul(w, h, rgba) && Bitmap::checkedMul(rgba, 4, rgba) &&
            rgba <= open_buffer_ceiling && rgba <= budget / 2 && rgba <= SIZE_MAX &&
            Bitmap::checkedMul(rgba, 3, peak) && Bitmap::checkedMul(encoded, 2, input) &&
            Bitmap::checkedAdd(peak, input, peak) && Bitmap::checkedAdd(peak, open_scratch, peak) && peak <= budget)
        || refuse_open("decoded image dimensions exceed the available-memory limit");
}
static bool header_fits(guchar const *data, std::size_t len, std::uint64_t encoded, std::uint64_t budget)
{
    Bitmap::HeaderLimits limits;
    limits.prefixOnly = true;
    limits.requireStill = false;
    limits.maxAxis = UINT32_MAX;
    limits.maxPixels = UINT64_MAX;
    limits.maxEncodedBytes = open_buffer_ceiling;
    limits.maxProfileBytes = open_buffer_ceiling;
    Bitmap::Budget scratch(Bitmap::Budget::FixedLimitForTest{}, Bitmap::MiB);
    auto header = Bitmap::inspect({data, std::min<std::size_t>(len, Bitmap::MiB), {}}, limits, scratch);
    auto const &h = header.value;
    // Explode's CMYK, animation, metadata and format refusals are NOT display
    // policy. If its parser lacks dimensions, the loader's signal is authoritative.
    if (h.format == Bitmap::Format::PNG || h.format == Bitmap::Format::JPEG ||
        h.format == Bitmap::Format::WebP || h.format == Bitmap::Format::GIF) {
        if (h.width && h.height) return image_open_dimensions_fit(h.width, h.height, encoded, budget);
    }
    return true;
}
static bool png_metadata(guchar const *data, std::size_t len, std::uint64_t budget, std::uint64_t &extra)
{
    if (len < 8 || std::memcmp(data, "\211PNG\r\n\032\n", 8)) return true;
    auto be32 = [](guchar const *p) { return (std::uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; };
    bool complete = false;
    for (std::size_t pos = 8; len - pos >= 12;) {
        auto n = be32(data + pos);
        if (n > len - pos - 12) break; // leave truncation/corruption handling to the legacy codec
        auto tag = data + pos + 4, p = data + pos + 8, end = p + n;
        if (!std::memcmp(tag, "IEND", 4)) complete = true;
        if (!std::memcmp(tag, "zTXt", 4) || !std::memcmp(tag, "iCCP", 4) || !std::memcmp(tag, "iTXt", 4)) {
            while (p < end && *p) ++p;
            if (end - p >= (!std::memcmp(tag, "iTXt", 4) ? 3 : 2)) {
                bool compressed = std::memcmp(tag, "iTXt", 4) || p[1];
                p += std::memcmp(tag, "iTXt", 4) ? 2 : 3;
                if (!std::memcmp(tag, "iTXt", 4)) {
                    for (int k = 0; k < 2 && p < end; ++k) { while (p < end && *p) ++p; if (p < end) ++p; }
                }
                if (compressed && p < end) {
                    z_stream z{};
                    if (inflateInit(&z) == Z_OK) {
                        auto cleanup = scope_exit([&] { inflateEnd(&z); });
                        z.next_in = const_cast<guchar *>(p); z.avail_in = end - p;
                        std::array<guchar, 65536> sink;
                        int status;
                        do {
                            z.next_out = sink.data(); z.avail_out = sink.size();
                            status = inflate(&z, Z_NO_FLUSH);
                            // Extra /2 is folded into the encoded term below, reserving
                            // four copies for libpng text/profile storage + ICC base64.
                            if (!Bitmap::checkedAdd(extra, sink.size() - z.avail_out, extra) || extra > budget / 4)
                                return refuse_open("inflated PNG metadata exceeds the available-memory limit");
                        } while (status == Z_OK && (z.avail_in || !z.avail_out));
                    }
                }
            }
        }
        pos += std::size_t(n) + 12;
    }
    if (!complete) open_diagnostic = "truncated PNG: missing complete IEND; displaying partial pixels";
    return true;
}

static bool gif_frames_fit(guchar const *data, std::size_t len, std::uint64_t budget)
{
    if (len < 13 || std::memcmp(data, "GIF", 3)) return true;
    auto le16 = [](guchar const *p) { return std::uint64_t(p[0]) | (std::uint64_t(p[1]) << 8); };
    std::size_t pos = 13;
    auto skip = [&](std::size_t n) { if (n > len - pos) return false; pos += n; return true; };
    auto table = [&](guchar flags) { return !(flags & 128) || skip(3u << ((flags & 7) + 1)); };
    auto blocks = [&] { while (pos < len) { auto n = data[pos++]; if (!n) return true; if (!skip(n)) return false; } return false; };
    std::uint64_t canvas, total = 0;
    if (!Bitmap::checkedMul(le16(data + 6), le16(data + 8), canvas) || !table(data[10])) return true;
    while (pos < len) {
        auto tag = data[pos++];
        if (tag == 0x21) { if (!skip(1) || !blocks()) break; continue; }
        if (tag != 0x2C || len - pos < 9) break;
        auto w = le16(data + pos + 4), h = le16(data + pos + 6);
        auto flags = data[pos + 8]; pos += 9;
        std::uint64_t pixels;
        // Do not apply Explode's logical-screen containment rule: GdkPixbuf
        // displays cropped/outlying frames too. Admit their actual dimensions.
        if (!image_open_dimensions_fit(w, h, len, budget) || !Bitmap::checkedMul(w, h, pixels) ||
            !Bitmap::checkedAdd(total, std::max(canvas, pixels), total) ||
            !Bitmap::checkedAdd(total, 256, total) || !image_open_dimensions_fit(1, total, len, budget))
            return refuse_open("GIF frame storage exceeds the available-memory limit");
        // 256 extra pixels/frame reserve 3 KiB for frame bookkeeping/colour tables.
        if (!table(flags) || !skip(1) || !blocks()) break;
    }
    return true; // codec still owns damage/truncation behaviour
}

// GdkPixbuf's legacy XPM module emits size-prepared only AFTER allocation.
// Supplement the signal gate with its header/palette preflight (no format ban).
static bool xpm_fits(guchar const *data, std::size_t len, std::uint64_t budget)
{
    if (len < 9 || std::memcmp(data, "/* XPM */", 9)) return true;
    std::size_t pos = 6; // immediately after the XPM token consumed by the module
    auto seek = [&](char target) {
        while (pos < len) {
            auto c = data[pos++];
            if (c == target) return true;
            if (c == '/' && pos < len) {
                if (data[pos++] == '*') {
                    while (pos + 1 < len && !(data[pos] == '*' && data[pos + 1] == '/')) ++pos;
                    pos = std::min(len, pos + 2);
                }
            }
        }
        return false;
    };
    if (!seek('{') || !seek('"')) return true; // codec owns malformed inputs
    std::array<std::uint64_t, 4> values{};
    for (auto &v : values) {
        while (pos < len && g_ascii_isspace(data[pos])) ++pos;
        if (pos < len && data[pos] == '+') ++pos;
        if (pos == len || !g_ascii_isdigit(data[pos])) return true;
        while (pos < len && g_ascii_isdigit(data[pos])) {
            if (!Bitmap::checkedMul(v, 10, v) || !Bitmap::checkedAdd(v, data[pos++] - '0', v))
                return refuse_open("XPM header exceeds the available-memory limit");
        }
    }
    auto [w, h, colors, cpp] = values;
    if (!cpp || cpp >= 32 || !colors) return true; // rejected by the existing codec
    std::uint64_t palette, encoded;
    // Colour/name/hash storage and the module's doubling string read buffer.
    if (!Bitmap::checkedMul(colors, cpp + 129, palette) || !Bitmap::checkedMul(len, 2, encoded) ||
        !Bitmap::checkedAdd(encoded, palette, encoded) || !encoded_fits(encoded, budget) ||
        !image_open_dimensions_fit(w, h, encoded, budget))
        return refuse_open("XPM pixel/palette storage exceeds the available-memory limit");
    return true;
}

// BUG-024: TIFFReadRGBAImage premultiplies even UNASSALPHA samples. Decode
// ordinary unsigned RGB/gray/palette strips or tiles ourselves instead. This
// route never consumes GdkPixbuf TIFF pixels, so a future loader fix cannot
// double-unassociate them. Exotic sample formats/photometrics retain the loader.
static GdkPixbuf *decode_tiff_samples(guchar const *data, std::size_t len, std::uint64_t budget, bool &refused)
{
    struct Input { guchar const *data; std::size_t size, pos = 0; } input{data, len};
    auto read = +[](thandle_t h, void *dst, tmsize_t size) -> tmsize_t {
        auto &in = *static_cast<Input *>(h);
        if (size < 0) return -1;
        auto n = std::min<std::size_t>(size, in.size - in.pos);
        std::memcpy(dst, in.data + in.pos, n); in.pos += n;
        return n;
    };
    auto seek = +[](thandle_t h, toff_t offset, int whence) -> toff_t {
        auto &in = *static_cast<Input *>(h);
        // libtiff represents negative relative seeks in unsigned toff_t.
        auto base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? in.pos : in.size;
        if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return toff_t(-1);
        auto delta = static_cast<std::int64_t>(offset);
        if (delta < 0) {
            auto magnitude = std::uint64_t(-(delta + 1)) + 1;
            if (magnitude > base) return toff_t(-1);
            in.pos = base - magnitude;
        } else {
            if (std::uint64_t(delta) > in.size - base) return toff_t(-1);
            in.pos = base + delta;
        }
        return in.pos;
    };
    auto tif = TIFFClientOpen("VA TIFF input", "rm", &input, read,
        +[](thandle_t, void *, tmsize_t) -> tmsize_t { return -1; }, seek,
        +[](thandle_t) { return 0; },
        +[](thandle_t h) -> toff_t { return static_cast<Input *>(h)->size; },
        +[](thandle_t, void **, toff_t *) { return 0; },
        +[](thandle_t, void *, toff_t) {});
    if (!tif) return nullptr; // legacy codec owns malformed-header diagnostics
    auto close = scope_exit([&] { TIFFClose(tif); });
    std::uint32_t w = 0, h = 0;
    std::uint16_t bits = 1, samples = 1, photo = 0, planar = 1, kind = 1, orientation = 1;
    std::uint16_t extras = 0, *extra_types = nullptr;
    if (!TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w) || !TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h)) return nullptr;
    if (!image_open_dimensions_fit(w, h, len, budget)) { refused = true; return nullptr; }
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &kind);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orientation);
    if (!TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photo)) return nullptr;
    TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &extras, &extra_types);
    bool rgb = photo == PHOTOMETRIC_RGB;
    bool gray = photo == PHOTOMETRIC_MINISBLACK || photo == PHOTOMETRIC_MINISWHITE;
    bool palette = photo == PHOTOMETRIC_PALETTE;
    unsigned colors = rgb ? 3 : 1;
    bool alpha = extras == 1 && extra_types &&
        (extra_types[0] == EXTRASAMPLE_ASSOCALPHA || extra_types[0] == EXTRASAMPLE_UNASSALPHA);
    if ((!rgb && !gray && !palette) || kind != SAMPLEFORMAT_UINT ||
        (planar != PLANARCONFIG_CONTIG && planar != PLANARCONFIG_SEPARATE) ||
        orientation < 1 || orientation > 8 ||
        samples != colors + unsigned(alpha) || extras != unsigned(alpha) ||
        (palette ? (alpha || (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16))
                 : (bits != 8 && bits != 16))) return nullptr;
    std::uint16_t *red = nullptr, *green = nullptr, *blue = nullptr;
    if (palette && !TIFFGetField(tif, TIFFTAG_COLORMAP, &red, &green, &blue)) return nullptr;
    bool associated = alpha && extra_types[0] == EXTRASAMPLE_ASSOCALPHA;
    bool tiled = TIFFIsTiled(tif);
    std::uint32_t bw = w, bh = 0;
    if (tiled) {
        if (!TIFFGetField(tif, TIFFTAG_TILEWIDTH, &bw) || !TIFFGetField(tif, TIFFTAG_TILELENGTH, &bh)) return nullptr;
    } else {
        TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &bh);
        bh = std::min(bh, h);
    }
    auto block = tiled ? TIFFTileSize64(tif) : TIFFStripSize64(tif);
    auto row = tiled ? TIFFTileRowSize64(tif) : TIFFScanlineSize64(tif);
    auto planes = planar == PLANARCONFIG_SEPARATE ? samples : 1;
    std::uint64_t storage, scratch, pixels, encoded, peak, row_bits, expected_row;
    std::uint32_t profile_size = 0; void *profile = nullptr;
    TIFFGetField(tif, TIFFTAG_ICCPROFILE, &profile_size, &profile);
    // Reserve the codec block as well as our plane buffers, orientation overlap,
    // encoded bytes and ICC copies. Reject huge padded tiles before allocating.
    if (!bw || !bh || !block || !row ||
        !Bitmap::checkedMul(bw, planar == PLANARCONFIG_SEPARATE ? 1 : samples, row_bits) ||
        !Bitmap::checkedMul(row_bits, bits, row_bits) ||
        !Bitmap::checkedCeilDiv(row_bits, 8, expected_row) || row != expected_row ||
        !Bitmap::checkedMul(row, bh, storage) || block < storage ||
        !Bitmap::checkedMul(block, planes, storage) || storage > SIZE_MAX || block > PTRDIFF_MAX ||
        !Bitmap::checkedMul(storage, 2, scratch) ||
        !Bitmap::checkedMul(w, h, pixels) || !Bitmap::checkedMul(pixels, 12, pixels) ||
        !Bitmap::checkedMul(len, 2, encoded) || !Bitmap::checkedAdd(pixels, encoded, peak) ||
        !Bitmap::checkedAdd(peak, scratch, peak) || !Bitmap::checkedAdd(peak, open_scratch, peak) ||
        !Bitmap::checkedAdd(peak, std::uint64_t(profile_size) * 4, peak) || peak > budget) {
        refused = true;
        refuse_open("TIFF block/metadata storage exceeds the available-memory limit");
        return nullptr;
    }
    auto buffer = static_cast<guchar *>(g_try_malloc(storage));
    auto pixels_out = static_cast<guchar *>(g_try_malloc(std::size_t(w) * h * 4));
    if (!buffer || !pixels_out) {
        g_free(buffer); g_free(pixels_out);
        refused = true;
        refuse_open("not enough memory for TIFF pixels"); return nullptr;
    }
    auto free_buffer = scope_exit([&] { g_free(buffer); });
    auto free_pixels = scope_exit([&] { g_free(pixels_out); });
    auto maximum = bits == 16 ? 65535u : 255u;
    auto to_byte = [](std::uint32_t value, std::uint32_t max) -> guchar {
        return (std::uint64_t(value) * 255 + max / 2) / max;
    };
    for (std::uint32_t y = 0; y < h;) {
        auto rows = std::min(bh, h - y);
        for (std::uint32_t x = 0; x < w;) {
            auto cols = std::min(bw, w - x);
            for (unsigned plane = 0; plane < planes; ++plane) {
                auto dst = buffer + plane * block;
                auto got = tiled ? TIFFReadEncodedTile(tif, TIFFComputeTile(tif, x, y, 0, plane), dst, block)
                                 : TIFFReadEncodedStrip(tif, TIFFComputeStrip(tif, y, plane), dst, block);
                if (got < 0 || std::uint64_t(got) < row * rows) return nullptr;
            }
            for (std::uint32_t yy = 0; yy < rows; ++yy) for (std::uint32_t xx = 0; xx < cols; ++xx) {
                auto sample = [&](unsigned channel) -> std::uint32_t {
                    auto p = buffer + (planes > 1 ? channel * block : 0) + yy * row;
                    auto index = std::size_t(xx) * (planes > 1 ? 1 : samples) + (planes > 1 ? 0 : channel);
                    if (bits == 16) { std::uint16_t value; std::memcpy(&value, p + index * 2, 2); return value; }
                    if (bits == 8) return p[index];
                    return (p[index * bits / 8] >> (8 - bits - index * bits % 8)) & ((1u << bits) - 1);
                };
                auto out = pixels_out + (std::size_t(y + yy) * w + x + xx) * 4;
                auto a = alpha ? sample(colors) : maximum;
                for (unsigned c = 0; c < 3; ++c) {
                    auto value = sample(rgb ? c : 0);
                    if (palette) { out[c] = to_byte((c == 0 ? red : c == 1 ? green : blue)[value], 65535); continue; }
                    // Unassociate at source precision, before 16 -> 8 reduction.
                    // Straight alpha=0 retains hidden RGB; associated alpha=0
                    // has no recoverable color, and is canonically black.
                    if (associated) value = a ? std::min<std::uint64_t>(maximum, (std::uint64_t(value) * maximum + a / 2) / a) : 0;
                    if (photo == PHOTOMETRIC_MINISWHITE && (!associated || a)) value = maximum - value;
                    out[c] = to_byte(value, maximum);
                }
                out[3] = to_byte(a, maximum);
            }
            x += cols;
        }
        y += rows;
    }
    auto pixbuf = gdk_pixbuf_new_from_data(pixels_out, GDK_COLORSPACE_RGB, true, 8, w, h, w * 4,
        +[](guchar *p, gpointer) { g_free(p); }, nullptr);
    if (!pixbuf) return nullptr;
    pixels_out = nullptr;
    auto option = [&](char const *name, unsigned value) {
        auto text = std::to_string(value); gdk_pixbuf_set_option(pixbuf, name, text.c_str());
    };
    // Raw samples have not been flipped by libtiff; the existing shared caller
    // applies the complete TIFF orientation exactly once, including axis swaps.
    if (orientation != ORIENTATION_TOPLEFT) option("orientation", orientation);
    option("bits-per-sample", bits);
    std::uint16_t compression = 1, unit = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression); option("compression", compression);
    if (profile && profile_size) {
        auto encoded_profile = g_base64_encode(static_cast<guchar const *>(profile), profile_size);
        gdk_pixbuf_set_option(pixbuf, "icc-profile", encoded_profile); g_free(encoded_profile);
    }
    if (TIFFGetField(tif, TIFFTAG_RESOLUTIONUNIT, &unit) && (unit == RESUNIT_INCH || unit == RESUNIT_CENTIMETER)) {
        float xd = 0, yd = 0;
        TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xd); TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yd);
        auto dpi = [&](char const *name, double value) {
            value = std::round(value * (unit == RESUNIT_CENTIMETER ? 2.54 : 1));
            if (std::isfinite(value) && value >= 0 && value <= INT_MAX) option(name, value);
        };
        dpi("x-dpi", xd); dpi("y-dpi", yd);
    }
    if (!TIFFLastDirectory(tif)) gdk_pixbuf_set_option(pixbuf, "multipage", "yes");
    return pixbuf;
}

static GdkPixbuf *decode_open(guchar const *data, std::size_t len, std::uint64_t budget, std::string &format)
{
    if (!encoded_fits(len, budget) || !header_fits(data, len, len, budget) || !gif_frames_fit(data, len, budget) ||
        !xpm_fits(data, len, budget)) return nullptr;
    if (len >= 4 && (!std::memcmp(data, "II\052\000", 4) || !std::memcmp(data, "MM\000\052", 4) ||
                     !std::memcmp(data, "II\053\000", 4) || !std::memcmp(data, "MM\000\053", 4))) {
        bool refused = false;
        if (auto pixbuf = decode_tiff_samples(data, len, budget, refused)) { format = "tiff"; return pixbuf; }
        if (refused) return nullptr;
        // Unsupported/exotic or damaged TIFFs retain the legacy codec's
        // validation, compression errors and partial-image semantics.
    }
    Bitmap::HeaderLimits limits;
    limits.prefixOnly = false; limits.requireStill = false;
    limits.maxAxis = UINT32_MAX; limits.maxPixels = UINT64_MAX;
    limits.maxEncodedBytes = open_buffer_ceiling; limits.maxProfileBytes = open_buffer_ceiling;
    Bitmap::Budget scratch(Bitmap::Budget::FixedLimitForTest{}, Bitmap::MiB);
    // Supplemental WebP frame count; other formats keep the prefix route.
    if (len >= 12 && !std::memcmp(data, "RIFF", 4)) {
        auto all = Bitmap::inspect({data, len, {}}, limits, scratch);
        auto const &h = all.value;
        std::uint64_t rows;
        if (h.width && h.height && (!Bitmap::checkedMul(h.height, std::max(1u, h.frames), rows) ||
            !image_open_dimensions_fit(h.width, rows, len, budget))) return nullptr;
    }
    std::uint64_t extra = 0, accounted = len;
    if (!png_metadata(data, len, budget, extra) || !Bitmap::checkedMul(extra, 2, extra) ||
        !Bitmap::checkedAdd(accounted, extra, accounted) || !encoded_fits(accounted, budget)) return nullptr;
    struct Probe { std::uint64_t encoded, budget; bool refused = false; } probe{accounted, budget};
    auto loader = gdk_pixbuf_loader_new();
    auto signal = g_signal_connect(loader, "size-prepared", G_CALLBACK(+[](GdkPixbufLoader *loader, int w, int h, gpointer p) {
        auto &probe = *static_cast<Probe *>(p);
        if (w <= 0 || h <= 0 || !image_open_dimensions_fit(w, h, probe.encoded, probe.budget)) {
            refuse_open("decoded image dimensions exceed the available-memory limit");
            probe.refused = true;
            // Zero size is the GdkPixbuf module API's get-file-info abort route;
            // 1x1 would still allocate the original raster in TIFF and other loaders.
            gdk_pixbuf_loader_set_size(loader, 0, 0);
        }
    }), &probe);
    GError *error = nullptr;
    bool written = true;
    for (std::size_t pos = 0; pos < len && written && !probe.refused;) {
        auto n = std::min<std::size_t>(len - pos, 65535);
        written = gdk_pixbuf_loader_write(loader, data + pos, n, &error);
        pos += n;
    }
    g_clear_error(&error);
    // write failure closes the loader internally. close failure may still have
    // partial pixels, which the embedded-image path has historically displayed.
    bool closed = written && gdk_pixbuf_loader_close(loader, &error);
    auto buf = written && !probe.refused ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
    if (buf && !image_open_dimensions_fit(gdk_pixbuf_get_width(buf), gdk_pixbuf_get_height(buf), accounted, budget)) buf = nullptr;
    if (buf) {
        g_object_ref(buf);
        auto name = gdk_pixbuf_format_get_name(gdk_pixbuf_loader_get_format(loader));
        format = name;
        g_free(name);
        if (!closed) open_diagnostic = "truncated image: decoder close failed; displaying partial pixels";
    } else if (!open_refused) {
        open_diagnostic = "image decoder could not read the image";
    }
    g_clear_error(&error);
    g_signal_handler_disconnect(loader, signal);
    g_object_unref(loader);
    return buf;
}

guchar *image_open_decode_base64(std::string_view data, std::uint64_t budget, gsize &len, bool raster)
{
    std::size_t capacity;
    if (!image_open_base64_capacity(data.size(), budget, capacity)) return nullptr;
    if (raster) {
        auto prefix = static_cast<guchar *>(g_try_malloc(std::min<std::size_t>(capacity, Bitmap::MiB + 3)));
        if (!prefix) { refuse_open("not enough memory for image header"); return nullptr; }
        auto prefix_guard = scope_exit([&] { g_free(prefix); });
        int state = 0;
        guint save = 0;
        auto n = g_base64_decode_step(data.data(), std::min<std::size_t>(data.size(), Bitmap::MiB), prefix, &state, &save);
        if (!header_fits(prefix, n, capacity, budget)) return nullptr;
    }
    auto decoded = static_cast<guchar *>(g_try_malloc(capacity ? capacity : 1));
    if (!decoded) { refuse_open("not enough memory for encoded image buffer"); return nullptr; }
    int state = 0;
    guint save = 0;
    len = g_base64_decode_step(data.data(), data.size(), decoded, &state, &save);
    return decoded;
}

Pixbuf *Pixbuf::create_from_data_uri(gchar const *uri_data, double svgdpi)
{
    Pixbuf *pixbuf = nullptr;
    auto [data, type] = extract_uri_data(uri_data);
    auto meta = std::string_view(uri_data, data - uri_data);
    if (type == Base64Data::NONE && meta.find("image/") != meta.npos && meta.find("base64") != meta.npos) {
        type = Base64Data::RASTER;
    }
    auto budget = open_budget();
    if ((*data) && type == Base64Data::RASTER) {
        gsize len = 0;
        auto decoded = image_open_decode_base64(data, budget, len, true);
        if (!decoded) return nullptr;
        auto owned = reinterpret_cast<gchar *>(decoded);
        return create_from_buffer(std::move(owned), len, svgdpi);
    }

    if ((*data) && type == Base64Data::SVG) {
        gsize decoded_len = 0;
        auto decoded = image_open_decode_base64(data, budget, decoded_len, false);
        if (!decoded) return nullptr;
        auto svgDoc = SPDocument::createNewDocFromMem({reinterpret_cast<char const *>(decoded), decoded_len});
        // Check the document loaded properly
        if (!svgDoc || !svgDoc->getRoot()) {
            g_free(decoded);
            return nullptr;
        }
        Inkscape::Preferences *prefs = Inkscape::Preferences::get();
        double dpi = prefs->getDouble("/dialogs/import/defaultxdpi/value", 96.0);
        if (svgdpi && svgdpi > 0) {
            dpi = svgdpi;
        }

        // Get the size of the document
        Inkscape::Util::Quantity svgWidth = svgDoc->getWidth();
        Inkscape::Util::Quantity svgHeight = svgDoc->getHeight();
        // Limit the size of the document to 100 inches square, mirroring create_from_buffer
        double const svgWidth_px = std::min(svgWidth.value("px"), dpi * 100);
        double const svgHeight_px = std::min(svgHeight.value("px"), dpi * 100);
        if (svgWidth_px < 0 || svgHeight_px < 0) {
            g_warning("create_from_data_uri: malformed document: svgWidth_px=%f, svgHeight_px=%f", svgWidth_px,
                      svgHeight_px);
            g_free(decoded);
            return nullptr;
        }

        auto rw = std::ceil(svgWidth_px * dpi / 96), rh = std::ceil(svgHeight_px * dpi / 96);
        if (!std::isfinite(rw) || !std::isfinite(rh) || rw <= 0 || rh <= 0 || rw > INT_MAX || rh > INT_MAX ||
            !image_open_dimensions_fit(rw, rh, decoded_len, budget)) {
            g_free(decoded);
            return nullptr;
        }
        assert(!pixbuf);
        Geom::Rect area(0, 0, svgWidth_px, svgHeight_px);
        pixbuf = sp_generate_internal_bitmap(svgDoc.get(), area, dpi);
        if (!pixbuf) {
            std::cerr << "Pixbuf::create_from_data_uri: failed to rasterize embedded SVG" << std::endl;
            g_free(decoded);
            return nullptr;
        }
        GdkPixbuf const *buf = pixbuf->getPixbufRaw();

        // Tidy up
        if (buf == nullptr) {
            std::cerr << "Pixbuf::create_from_data: failed to load contents: " << std::endl;
            delete pixbuf;
            g_free(decoded);
            return nullptr;
        } else {
            pixbuf->_setMimeData(decoded, decoded_len, "svg+xml");
        }
    }

    return pixbuf;
}

Pixbuf *Pixbuf::create_from_file(std::string const &fn, double svgdpi)
{
    Pixbuf *pb = nullptr;
    // test correctness of filename
    if (!g_file_test(fn.c_str(), G_FILE_TEST_EXISTS)) { 
        return nullptr;
    }
    GStatBuf stdir;
    int val = g_stat(fn.c_str(), &stdir);
    if (val == 0 && stdir.st_mode & S_IFDIR){
        return nullptr;
    }
    // Bound the read on the opened descriptor too: a growing/replaced file must
    // never bypass the stat check and trigger g_file_get_contents' unbounded growth.
    auto budget = open_budget();
    if (val != 0 || stdir.st_size < 0 || !encoded_fits(stdir.st_size, budget)) return nullptr;
    auto file = g_fopen(fn.c_str(), "rb");
    if (!file) return nullptr;
    auto guard = scope_exit([&] { std::fclose(file); });
    auto len = static_cast<gsize>(stdir.st_size);
    auto data = static_cast<gchar *>(g_try_malloc(len ? len : 1));
    if (!data) { refuse_open("not enough memory for linked image buffer"); return nullptr; }
    if (std::fread(data, 1, len, file) != len || std::fgetc(file) != EOF || std::ferror(file)) {
        g_free(data);
        refuse_open("linked image changed size or failed during bounded read");
        return nullptr;
    }
    pb = Pixbuf::create_from_buffer(std::move(data), len, svgdpi, fn);
    if (pb) pb->_mod_time = stdir.st_mtime;

    return pb;
}

Pixbuf *image_open_from_uri(char const *uri, double dpi)
{
    auto file = g_file_new_for_uri(uri);
    GError *error = nullptr;
    auto stream = g_file_read(file, nullptr, &error);
    g_object_unref(file);
    g_clear_error(&error);
    if (!stream) return nullptr;
    gchar *data = nullptr;
    gsize len = 0;
    auto guard = scope_exit([&] { g_free(data); g_object_unref(stream); });
    auto budget = open_budget();
    std::array<gchar, 65536> chunk;
    while (true) {
        auto n = g_input_stream_read(G_INPUT_STREAM(stream), chunk.data(), chunk.size(), nullptr, &error);
        if (n <= 0) { g_clear_error(&error); if (n < 0) return nullptr; break; }
        std::uint64_t next;
        if (!Bitmap::checkedAdd(len, n, next) || !encoded_fits(next, budget)) return nullptr;
        auto grown = static_cast<gchar *>(g_try_realloc(data, next));
        if (!grown) { refuse_open("not enough memory for linked image buffer"); return nullptr; }
        data = grown;
        std::memcpy(data + len, chunk.data(), n);
        len = next;
    }
    try {
        std::string contents(data ? data : "", len);
        g_free(data); data = nullptr;
        return Pixbuf::create_from_buffer(contents, dpi);
    } catch (std::bad_alloc const &) {
        refuse_open("not enough memory for linked image buffer");
        return nullptr;
    }
}

GdkPixbuf *Pixbuf::apply_embedded_orientation(GdkPixbuf *buf)
{
    GdkPixbuf *old = buf;
    buf = gdk_pixbuf_apply_embedded_orientation(buf);
    g_object_unref(old);
    return buf;
}

/**
 * Gets any available orientation data and returns it as an affine.
 */
Geom::Affine Pixbuf::get_embedded_orientation(GdkPixbuf *buf)
{
    // See gdk_pixbuf_apply_embedded_orientation in gdk-pixbuf
    if (auto opt_str = gdk_pixbuf_get_option(buf, "orientation")) {
        switch ((int)g_ascii_strtoll(opt_str, NULL, 10)) {
            case 2: // Flip Horz
                return Geom::Scale(-1, 1);
            case 3: // +180 Rotate
                return Geom::Scale(-1, -1);
            case 4: // Flip Vert
                return Geom::Scale(1, -1);
            case 5: // +90 Rotate & Flip Horz
                return Geom::Rotate(90) * Geom::Scale(-1, 1);
            case 6: // +90 Rotate
                return Geom::Rotate(90);
            case 7: // +90 Rotate * Flip Vert
                return Geom::Rotate(90) * Geom::Scale(1, -1);
            case 8: // -90 Rotate
                return Geom::Rotate(-90);
            default:
                break;

        }
    }
    return Geom::identity();
}

Pixbuf *Pixbuf::create_from_buffer(std::string const &buffer, double svgdpi, std::string const &fn)
{
    if (!encoded_fits(buffer.size(), open_budget())) return nullptr;
    auto datacopy = static_cast<gchar *>(g_try_malloc(buffer.size() ? buffer.size() : 1));
    if (!datacopy) { refuse_open("not enough memory for encoded image buffer"); return nullptr; }
    std::memcpy(datacopy, buffer.data(), buffer.size());
    return Pixbuf::create_from_buffer(std::move(datacopy), buffer.size(), svgdpi, fn);
}

Pixbuf *Pixbuf::create_from_buffer(gchar *&&data, gsize len, double svgdpi, std::string const &fn)
{
    auto data_guard = scope_exit([&] { g_free(data); });
    auto budget = open_budget();
    if (!encoded_fits(len, budget)) return nullptr;
    bool has_ori = false;
    Pixbuf *pb = nullptr;
    {
        GdkPixbuf *buf = nullptr;
        std::string format;
        std::string::size_type idx;
        idx = fn.rfind('.');
        bool is_svg = false;    
        if(idx != std::string::npos)
        {
            if (boost::iequals(fn.substr(idx+1).c_str(), "svg")) {
                auto svgDoc = SPDocument::createNewDocFromMem({data, len}, fn.c_str());

                // Check the document loaded properly
                if (!svgDoc || !svgDoc->getRoot()) {
                    return nullptr;
                }

                Inkscape::Preferences *prefs = Inkscape::Preferences::get();
                double dpi = prefs->getDouble("/dialogs/import/defaultxdpi/value", 96.0);
                if (svgdpi && svgdpi > 0) {
                    dpi = svgdpi;
                }

                // Get the size of the document
                Inkscape::Util::Quantity svgWidth = svgDoc->getWidth();
                Inkscape::Util::Quantity svgHeight = svgDoc->getHeight();
                // Limit the size of the document to 100 inches square
                const double svgWidth_px = std::min(svgWidth.value("px"), dpi * 100);
                const double svgHeight_px = std::min(svgHeight.value("px"), dpi * 100);
                if (svgWidth_px < 0 || svgHeight_px < 0) {
                    g_warning("create_from_buffer: malformed document: svgWidth_px=%f, svgHeight_px=%f", svgWidth_px,
                              svgHeight_px);
                    return nullptr;
                }

                Geom::Rect area(0, 0, svgWidth_px, svgHeight_px);
                auto rw = std::ceil(svgWidth_px * dpi / 96), rh = std::ceil(svgHeight_px * dpi / 96);
                if (!std::isfinite(rw) || !std::isfinite(rh) || rw <= 0 || rh <= 0 || rw > INT_MAX || rh > INT_MAX ||
                    !image_open_dimensions_fit(rw, rh, len, budget)) return nullptr;
                pb = sp_generate_internal_bitmap(svgDoc.get(), area, dpi);
                if (!pb)
                    return nullptr;

                buf = pb->getPixbufRaw();

                // Tidy up
                if (buf == nullptr) {
                    delete pb;
                    return nullptr;
                }
                buf = Pixbuf::apply_embedded_orientation(buf);
                is_svg = true;
            }
        }
        if (!is_svg) {
            buf = decode_open(reinterpret_cast<guchar *>(data), len, budget, format);
            if (buf) {
                has_ori = Pixbuf::get_embedded_orientation(buf) != Geom::identity();
                if (format == "tiff") {
                    // GdkPixbuf's orientation copy drops options. Preserve TIFF
                    // color/resolution metadata across the sample transform.
                    auto original = buf;
                    buf = gdk_pixbuf_apply_embedded_orientation(original);
                    if (buf) {
                        copy_supported_pixbuf_metadata(original, buf);
                        auto const *option = gdk_pixbuf_get_option(original, "orientation");
                        auto orientation = option ? g_ascii_strtoll(option, nullptr, 10) : 1;
                        if (orientation >= 5 && orientation <= 8) {
                            // Density follows the oriented pixel axes; ICC is unchanged.
                            gdk_pixbuf_remove_option(buf, "x-dpi");
                            gdk_pixbuf_remove_option(buf, "y-dpi");
                            if (auto const *dpi = gdk_pixbuf_get_option(original, "y-dpi"))
                                gdk_pixbuf_set_option(buf, "x-dpi", dpi);
                            if (auto const *dpi = gdk_pixbuf_get_option(original, "x-dpi"))
                                gdk_pixbuf_set_option(buf, "y-dpi", dpi);
                        }
                    }
                    g_object_unref(original);
                } else {
                    buf = Pixbuf::apply_embedded_orientation(buf);
                }
                if (buf && !gdk_pixbuf_get_has_alpha(buf)) {
                    auto alpha = gdk_pixbuf_add_alpha(buf, FALSE, 0, 0, 0);
                    g_object_unref(buf); buf = alpha;
                }
                if (!buf) { refuse_open("not enough memory for oriented RGBA pixels"); return nullptr; }
                try { pb = new Pixbuf(buf); }
                catch (std::bad_alloc const &) { g_object_unref(buf); refuse_open("not enough memory for image wrapper"); return nullptr; }
            }
        }

        if (pb) {
            pb->_path = fn;
            if (is_svg) {
                pb->_setMimeData((guchar *) data, len, "svg");
                data = nullptr;
            } else if(!has_ori) {
                // We DO NOT want to store the original data if it contains orientation
                // data since many exports that will use the surface do not handle it.
                pb->_setMimeData(reinterpret_cast<guchar *>(data), len, format);
                data = nullptr; // Cairo owns it (or _setMimeData freed it).
            }
        }

        // TODO: we could also read DPI, ICC profile, gamma correction, and other information
        // from the file. This can be done by using format-specific libraries e.g. libpng.
    }

    return pb;
}

/**
 * Converts the pixbuf to GdkPixbuf pixel format.
 * The returned pixbuf can be used e.g. in calls to gdk_pixbuf_save().
 */
GdkPixbuf *Pixbuf::getPixbufRaw(bool convert_format)
{
    if (convert_format) {
        ensurePixelFormat(PF_GDK);
    }
    return _pixbuf;
}

GdkPixbuf *Pixbuf::getPixbufRaw() const
{
    assert(_pixel_format == PF_GDK);
    return _pixbuf;
}

/**
 * Converts the pixbuf to Cairo pixel format and returns an image surface
 * which can be used as a source.
 *
 * The returned surface is owned by the GdkPixbuf and should not be freed.
 * Calling this function causes the pixbuf to be unsuitable for use
 * with GTK drawing functions until ensurePixelFormat(Pixbuf::PIXEL_FORMAT_PIXBUF) is called.
 */
cairo_surface_t *Pixbuf::getSurfaceRaw()
{
    ensurePixelFormat(PF_CAIRO);
    return _surface;
}

cairo_surface_t *Pixbuf::getSurfaceRaw() const
{
    assert(_pixel_format == PF_CAIRO);
    return _surface;
}

/* Declaring this function in the header requires including <gdkmm/pixbuf.h>,
 * which stupidly includes <glibmm.h> which in turn pulls in <glibmm/threads.h>.
 * However, since glib 2.32, <glibmm/threads.h> has to be included before <glib.h>
 * when compiling with G_DISABLE_DEPRECATED, as we do in non-release builds.
 * This necessitates spamming a lot of files with #include <glibmm/threads.h>
 * at the top.
 *
 * Since we don't really use gdkmm, do not define this function for now. */

/*
Glib::RefPtr<Gdk::Pixbuf> Pixbuf::getPixbuf(bool convert_format = true)
{
    g_object_ref(_pixbuf);
    Glib::RefPtr<Gdk::Pixbuf> p(getPixbuf(convert_format));
    return p;
}
*/

Cairo::RefPtr<Cairo::Surface> Pixbuf::getSurface()
{
    return Cairo::RefPtr<Cairo::Surface>(new Cairo::Surface(getSurfaceRaw(), false));
}

/** Retrieves the original compressed data for the surface, if any.
 * The returned data belongs to the object and should not be freed. */
guchar const *Pixbuf::getMimeData(gsize &len, std::string &mimetype) const
{
    static gchar const *mimetypes[] = {
        CAIRO_MIME_TYPE_JPEG, CAIRO_MIME_TYPE_JP2, CAIRO_MIME_TYPE_PNG, nullptr };
    static guint mimetypes_len = g_strv_length(const_cast<gchar**>(mimetypes));

    guchar const *data = nullptr;

    for (guint i = 0; i < mimetypes_len; ++i) {
        unsigned long len_long = 0;
        cairo_surface_get_mime_data(const_cast<cairo_surface_t*>(_surface), mimetypes[i], &data, &len_long);
        if (data != nullptr) {
			len = len_long;
            mimetype = mimetypes[i];
            break;
        }
    }

    return data;
}

int Pixbuf::width() const {
    return gdk_pixbuf_get_width(const_cast<GdkPixbuf*>(_pixbuf));
}
int Pixbuf::height() const {
    return gdk_pixbuf_get_height(const_cast<GdkPixbuf*>(_pixbuf));
}
int Pixbuf::rowstride() const {
    return gdk_pixbuf_get_rowstride(const_cast<GdkPixbuf*>(_pixbuf));
}
guchar const *Pixbuf::pixels() const {
    return gdk_pixbuf_get_pixels(const_cast<GdkPixbuf*>(_pixbuf));
}
guchar *Pixbuf::pixels() {
    return gdk_pixbuf_get_pixels(_pixbuf);
}
void Pixbuf::markDirty() {
    cairo_surface_mark_dirty(_surface);
}

void Pixbuf::_forceAlpha()
{
    if (gdk_pixbuf_get_has_alpha(_pixbuf)) return;

    GdkPixbuf *old = _pixbuf;
    _pixbuf = gdk_pixbuf_add_alpha(old, FALSE, 0, 0, 0);
    g_object_unref(old);
}

void Pixbuf::_setMimeData(guchar *data, gsize len, Glib::ustring const &format)
{
    gchar const *mimetype = nullptr;

    if (format == "jpeg") {
        mimetype = CAIRO_MIME_TYPE_JPEG;
    } else if (format == "jpeg2000") {
        mimetype = CAIRO_MIME_TYPE_JP2;
    } else if (format == "png") {
        mimetype = CAIRO_MIME_TYPE_PNG;
    }

    if (mimetype != nullptr) {
        cairo_surface_set_mime_data(_surface, mimetype, data, len, g_free, data);
        //g_message("Setting Cairo MIME data: %s", mimetype);
    } else {
        g_free(data);
        //g_message("Not setting Cairo MIME data: unknown format %s", name.c_str());
    }
}

/**
 * Convert the internal pixel format between CAIRO and GDK formats.
 */
void Pixbuf::ensurePixelFormat(PixelFormat fmt)
{
    if (fmt == PF_CAIRO && _pixel_format == PF_GDK) {
        ensure_argb32(_pixbuf);
        _pixel_format = fmt;
    } else if (fmt == PF_GDK && _pixel_format == PF_CAIRO) {
        ensure_pixbuf(_pixbuf);
        _pixel_format = fmt;
    } else if (fmt != _pixel_format) {
        g_assert_not_reached();
    }
}

/**
 * Converts GdkPixbuf's data to premultiplied ARGB.
 * This function will convert a GdkPixbuf in place into Cairo's native pixel format.
 * Note that this is a hack intended to save memory. When the pixbuf is in Cairo's format,
 * using it with GTK will result in corrupted drawings.
 */
void Pixbuf::ensure_argb32(GdkPixbuf *pb)
{
    convert_pixels_pixbuf_to_argb32(
        gdk_pixbuf_get_pixels(pb),
        gdk_pixbuf_get_width(pb),
        gdk_pixbuf_get_height(pb),
        gdk_pixbuf_get_rowstride(pb));
}

/**
 * Converts GdkPixbuf's data back to its native format.
 * Once this is done, the pixbuf can be used with GTK again.
 */
void Pixbuf::ensure_pixbuf(GdkPixbuf *pb)
{
    convert_pixels_argb32_to_pixbuf(
        gdk_pixbuf_get_pixels(pb),
        gdk_pixbuf_get_width(pb),
        gdk_pixbuf_get_height(pb),
        gdk_pixbuf_get_rowstride(pb));
}

} // namespace Inkscape

/*
 * Can be called recursively.
 * If optimize_stroke == false, the view Rect is not used.
 */
static void
feed_curve_to_cairo(cairo_t *cr, Geom::Curve const &c, Geom::Affine const &trans, Geom::Rect const &view, bool optimize_stroke)
{
    using Geom::X;
    using Geom::Y;

    unsigned order = 0;
    if (auto b = dynamic_cast<Geom::BezierCurve const*>(&c)) {
        order = b->order();
    }

    // handle the three typical curve cases
    switch (order) {
    case 1:
    {
        Geom::Point end_tr = c.finalPoint() * trans;
        if (!optimize_stroke) {
            cairo_line_to(cr, end_tr[0], end_tr[1]);
        } else {
            Geom::Rect swept(c.initialPoint()*trans, end_tr);
            if (swept.intersects(view)) {
                cairo_line_to(cr, end_tr[0], end_tr[1]);
            } else {
                cairo_move_to(cr, end_tr[0], end_tr[1]);
            }
        }
    }
    break;
    case 2:
    {
        auto quadratic_bezier = static_cast<Geom::QuadraticBezier const*>(&c);
        std::array<Geom::Point, 3> points;
        for (int i = 0; i < 3; i++) {
            points[i] = quadratic_bezier->controlPoint(i) * trans;
        }
        // degree-elevate to cubic Bezier, since Cairo doesn't do quadratic Beziers
        Geom::Point b1 = points[0] + (2./3) * (points[1] - points[0]);
        Geom::Point b2 = b1 + (1./3) * (points[2] - points[0]);
        if (!optimize_stroke) {
            cairo_curve_to(cr, b1[X], b1[Y], b2[X], b2[Y], points[2][X], points[2][Y]);
        } else {
            Geom::Rect swept(points[0], points[2]);
            swept.expandTo(points[1]);
            if (swept.intersects(view)) {
                cairo_curve_to(cr, b1[X], b1[Y], b2[X], b2[Y], points[2][X], points[2][Y]);
            } else {
                cairo_move_to(cr, points[2][X], points[2][Y]);
            }
        }
    }
    break;
    case 3:
    {
        auto cubic_bezier = static_cast<Geom::CubicBezier const*>(&c);
        std::array<Geom::Point, 4> points;
        for (int i = 0; i < 4; i++) {
            points[i] = cubic_bezier->controlPoint(i);
        }
        //points[0] *= trans; // don't do this one here for fun: it is only needed for optimized strokes
        points[1] *= trans;
        points[2] *= trans;
        points[3] *= trans;
        if (!optimize_stroke) {
            cairo_curve_to(cr, points[1][X], points[1][Y], points[2][X], points[2][Y], points[3][X], points[3][Y]);
        } else {
            points[0] *= trans;  // didn't transform this point yet
            Geom::Rect swept(points[0], points[3]);
            swept.expandTo(points[1]);
            swept.expandTo(points[2]);
            if (swept.intersects(view)) {
                cairo_curve_to(cr, points[1][X], points[1][Y], points[2][X], points[2][Y], points[3][X], points[3][Y]);
            } else {
                cairo_move_to(cr, points[3][X], points[3][Y]);
            }
        }
    }
    break;
    default:
    {
        if (Geom::EllipticalArc const *arc = dynamic_cast<Geom::EllipticalArc const*>(&c)) {
            if (arc->isChord()) {
                Geom::Point endPoint(arc->finalPoint());
                cairo_line_to(cr, endPoint[0], endPoint[1]);
            } else {
                Geom::Affine xform = arc->unitCircleTransform() * trans;
                // Don't draw anything if the angle is borked
                if(std::isnan(arc->initialAngle()) || std::isnan(arc->finalAngle())) {
                    g_warning("Bad angle while drawing EllipticalArc");
                    break;
                }

                // Apply the transformation to the current context
                auto cm = geom_to_cairo(xform);

                cairo_save(cr);
                cairo_transform(cr, &cm);

                // Draw the circle
                if (arc->sweep()) {
                    cairo_arc(cr, 0, 0, 1, arc->initialAngle(), arc->finalAngle());
                } else {
                    cairo_arc_negative(cr, 0, 0, 1, arc->initialAngle(), arc->finalAngle());
                }
                // Revert the current context
                cairo_restore(cr);
            }
        } else {
            // handles sbasis as well as all other curve types
            // this is very slow
            Geom::Path sbasis_path = Geom::cubicbezierpath_from_sbasis(c.toSBasis(), 0.1);

            // recurse to convert the new path resulting from the sbasis to svgd
            for (const auto & iter : sbasis_path) {
                feed_curve_to_cairo(cr, iter, trans, view, optimize_stroke);
            }
        }
    }
    break;
    }
}


/** Feeds path-creating calls to the cairo context translating them from the Path */
static void
feed_path_to_cairo (cairo_t *ct, Geom::Path const &path)
{
    if (path.empty())
        return;

    cairo_move_to(ct, path.initialPoint()[0], path.initialPoint()[1] );

    for (Geom::Path::const_iterator cit = path.begin(); cit != path.end_open(); ++cit) {
        feed_curve_to_cairo(ct, *cit, Geom::identity(), Geom::Rect(), false); // optimize_stroke is false, so the view rect is not used
    }

    if (path.closed()) {
        cairo_close_path(ct);
    }
}

/** Feeds path-creating calls to the cairo context translating them from the Path, with the given transform and shift */
static void
feed_path_to_cairo (cairo_t *ct, Geom::Path const &path, Geom::Affine trans, Geom::OptRect area, bool optimize_stroke, double stroke_width)
{
    if (!area)
        return;
    if (path.empty())
        return;

    // Transform all coordinates to coords within "area"
    Geom::Point shift = area->min();
    Geom::Rect view = *area;
    view.expandBy (stroke_width);
    view = view * (Geom::Affine)Geom::Translate(-shift);
    //  Pass transformation to feed_curve, so that we don't need to create a whole new path.
    Geom::Affine transshift(trans * Geom::Translate(-shift));

    Geom::Point initial = path.initialPoint() * transshift;
    cairo_move_to(ct, initial[0], initial[1] );

    for(Geom::Path::const_iterator cit = path.begin(); cit != path.end_open(); ++cit) {
        feed_curve_to_cairo(ct, *cit, transshift, view, optimize_stroke);
    }

    if (path.closed()) {
        if (!optimize_stroke) {
            cairo_close_path(ct);
        } else {
            cairo_line_to(ct, initial[0], initial[1]);
            /* We cannot use cairo_close_path(ct) here because some parts of the path may have been
               clipped and not drawn (maybe the before last segment was outside view area), which 
               would result in closing the "subpath" after the last interruption, not the entire path.

               However, according to cairo documentation:
               The behavior of cairo_close_path() is distinct from simply calling cairo_line_to() with the equivalent coordinate
               in the case of stroking. When a closed sub-path is stroked, there are no caps on the ends of the sub-path. Instead,
               there is a line join connecting the final and initial segments of the sub-path. 

               The correct fix will be possible when cairo introduces methods for moving without
               ending/starting subpaths, which we will use for skipping invisible segments; then we
               will be able to use cairo_close_path here. This issue also affects ps/eps/pdf export,
               see bug 168129
            */
        }
    }
}

/** Feeds path-creating calls to the cairo context translating them from the PathVector, with the given transform and shift
 *  One must have done cairo_new_path(ct); before calling this function. */
void
feed_pathvector_to_cairo (cairo_t *ct, Geom::PathVector const &pathv, Geom::Affine trans, Geom::OptRect area, bool optimize_stroke, double stroke_width)
{
    if (!area)
        return;
    if (pathv.empty())
        return;

    for(const auto & it : pathv) {
        feed_path_to_cairo(ct, it, trans, area, optimize_stroke, stroke_width);
    }
}

/** Feeds path-creating calls to the cairo context translating them from the PathVector
 *  One must have done cairo_new_path(ct); before calling this function. */
void
feed_pathvector_to_cairo (cairo_t *ct, Geom::PathVector const &pathv)
{
    if (pathv.empty())
        return;

    for(const auto & it : pathv) {
        feed_path_to_cairo(ct, it);
    }
}

/*
 * Pulls out the last cairo path context and reconstitutes it
 * into a local geom path vector for inkscape use.
 *
 * @param ct - The cairo context
 *
 * @returns an optioal Geom::PathVector object
 */
std::optional<Geom::PathVector> extract_pathvector_from_cairo(cairo_t *ct)
{
    cairo_path_t *path = cairo_copy_path(ct);
    if (!path)
        return std::nullopt;

    auto path_freer = scope_exit([&] { cairo_path_destroy(path); });

    Geom::PathBuilder res;
    auto end = &path->data[path->num_data];
    for (auto p = &path->data[0]; p < end; p += p->header.length) {
        switch (p->header.type) {
            case CAIRO_PATH_MOVE_TO:
                if (p->header.length != 2)
                    return std::nullopt;
                res.moveTo(Geom::Point(p[1].point.x, p[1].point.y));
                break;

            case CAIRO_PATH_LINE_TO:
                if (p->header.length != 2)
                    return std::nullopt;
                res.lineTo(Geom::Point(p[1].point.x, p[1].point.y));
                break;

            case CAIRO_PATH_CURVE_TO:
                if (p->header.length != 4)
                    return std::nullopt;
                res.curveTo(Geom::Point(p[1].point.x, p[1].point.y), Geom::Point(p[2].point.x, p[2].point.y),
                            Geom::Point(p[3].point.x, p[3].point.y));
                break;

            case CAIRO_PATH_CLOSE_PATH:
                if (p->header.length != 1)
                    return std::nullopt;
                res.closePath();
                break;
            default:
                return std::nullopt;
        }
    }

    res.flush();
    return res.peek();
}

SPColorInterpolation
get_cairo_surface_ci(cairo_surface_t *surface) {
    void* data = cairo_surface_get_user_data( surface, &ink_color_interpolation_key );
    if( data != nullptr ) {
        return (SPColorInterpolation)GPOINTER_TO_INT( data );
    } else {
        return SP_CSS_COLOR_INTERPOLATION_AUTO;
    }
}

/** Set the color_interpolation_value for a Cairo surface.
 *  Transform the surface between sRGB and linearRGB if necessary. */
void
set_cairo_surface_ci(cairo_surface_t *surface, SPColorInterpolation ci) {

    if( cairo_surface_get_content( surface ) != CAIRO_CONTENT_ALPHA ) {

        SPColorInterpolation ci_in = get_cairo_surface_ci( surface );

        if( ci_in == SP_CSS_COLOR_INTERPOLATION_SRGB &&
            ci    == SP_CSS_COLOR_INTERPOLATION_LINEARRGB ) {
            ink_cairo_surface_srgb_to_linear( surface );
        }
        if( ci_in == SP_CSS_COLOR_INTERPOLATION_LINEARRGB &&
            ci    == SP_CSS_COLOR_INTERPOLATION_SRGB ) {
            ink_cairo_surface_linear_to_srgb( surface );
        }

        cairo_surface_set_user_data(surface, &ink_color_interpolation_key, GINT_TO_POINTER (ci), nullptr);
    }
}

void
copy_cairo_surface_ci(cairo_surface_t *in, cairo_surface_t *out) {
    cairo_surface_set_user_data(out, &ink_color_interpolation_key, cairo_surface_get_user_data(in, &ink_color_interpolation_key), nullptr);
}

namespace Colors = Inkscape::Colors;

/**
 * Set the source color of the Cairo context.
 *
 * @arg ctx - The cairo context C pointer, must exist.
 * @arg color - The Color object containing floating point channel values.
 * @arg to_srgb - Default true, does a conversion to sRGB if needed, force no conversion
 *                to hack the data of the Cairo surface to non sRGB spaces.
 */
void ink_cairo_set_source_color(Cairo::RefPtr<Cairo::Context> &ctx, Colors::Color const &color, bool to_srgb)
{
    ink_cairo_set_source_color(ctx->cobj(), color, to_srgb);
}
void ink_cairo_set_source_color(cairo_t *ctx, Colors::Color const &color, bool to_srgb)
{
    if (!to_srgb || *color.getSpace() == Colors::Space::Type::RGB) {
        // Do not copy the color if we don't need to.
        // This sets floats so if the Surface is RGB96F or RGBA128F your surface will have more color data.
        cairo_set_source_rgba(ctx, color[0], color[1], color[2], color.getOpacity());
    } else if (auto copy = color.converted(Colors::Space::Type::RGB)) {
        ink_cairo_set_source_color(ctx, *copy, false); // Callback
    }
}
void ink_cairo_pattern_add_color_stop(cairo_pattern_t *ptn, double offset, Colors::Color const &color, bool to_srgb)
{
    if (!to_srgb || *color.getSpace() == Colors::Space::Type::RGB) {
        cairo_pattern_add_color_stop_rgba(ptn, offset, color[0], color[1], color[2], color.getOpacity());
    } else if (auto copy = color.converted(Colors::Space::Type::RGB)) {
        ink_cairo_pattern_add_color_stop(ptn, offset, *copy, false); // Callback
    }
}
cairo_pattern_t *ink_cairo_pattern_create(Colors::Color const &color, bool to_srgb)
{
    if (!to_srgb || *color.getSpace() == Colors::Space::Type::RGB) {
        return cairo_pattern_create_rgba(color[0], color[1], color[2], color.getOpacity());
    } else if (auto copy = color.converted(Colors::Space::Type::RGB)) {
        return ink_cairo_pattern_create(*copy, false);
    }
    return nullptr;
}

void ink_cairo_mesh_pattern_set_corner_color(cairo_pattern_t *pattern, unsigned corner_num, Colors::Color color)
{
    color.convert(Colors::Space::Type::RGB);
    cairo_mesh_pattern_set_corner_color_rgba(pattern, corner_num, color[0], color[1], color[2], color.getOpacity());
}


void ink_matrix_to_2geom(Geom::Affine &m, cairo_matrix_t const &cm)
{
    m[0] = cm.xx;
    m[2] = cm.xy;
    m[4] = cm.x0;
    m[1] = cm.yx;
    m[3] = cm.yy;
    m[5] = cm.y0;
}

void ink_matrix_to_cairo(cairo_matrix_t &cm, Geom::Affine const &m)
{
    cm.xx = m[0];
    cm.xy = m[2];
    cm.x0 = m[4];
    cm.yx = m[1];
    cm.yy = m[3];
    cm.y0 = m[5];
}

Geom::Affine ink_matrix_to_2geom(cairo_matrix_t const &cairo_matrix)
{
    Geom::Affine result;
    ink_matrix_to_2geom(result, cairo_matrix);
    return result;
}

void
ink_cairo_transform(cairo_t *ct, Geom::Affine const &m)
{
    cairo_matrix_t cm;
    ink_matrix_to_cairo(cm, m);
    cairo_transform(ct, &cm);
}

void
ink_cairo_pattern_set_matrix(cairo_pattern_t *cp, Geom::Affine const &m)
{
    cairo_matrix_t cm;
    ink_matrix_to_cairo(cm, m);
    cairo_pattern_set_matrix(cp, &cm);
}

void
ink_cairo_set_hairline(cairo_t *ct)
{
#ifdef CAIRO_HAS_HAIRLINE
    cairo_set_hairline(ct, true);
#else
    // As a backup, use a device unit of 1
    double x = 1.0, y = 0.0;
    cairo_device_to_user_distance(ct, &x, &y);
    cairo_set_line_width(ct, std::hypot(x, y));
#endif
}

void ink_cairo_pattern_set_dither(cairo_pattern_t *pattern, bool enabled)
{
#if CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 18, 0)
    cairo_pattern_set_dither(pattern, enabled ? CAIRO_DITHER_BEST : CAIRO_DITHER_NONE);
#endif
}

size_t ink_cairo_surface_byte_size(int stride, int height)
{
    if (stride <= 0 || height <= 0) return 0;
    return static_cast<size_t>(stride) * static_cast<size_t>(height);
}

void ink_cairo_surface_require_pixels(cairo_surface_t *s)
{
    if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        throw std::bad_alloc();
    }
    if (cairo_surface_get_type(s) == CAIRO_SURFACE_TYPE_IMAGE && !cairo_image_surface_get_data(s) &&
        cairo_image_surface_get_width(s) > 0 && cairo_image_surface_get_height(s) > 0) {
        throw std::bad_alloc();
    }
}

/**
 * Create an exact copy of a surface.
 * Creates a surface that has the same type, content type, dimensions and contents
 * as the specified surface.
 *
 * @throws std::bad_alloc if the copy cannot be allocated.
 */
cairo_surface_t *
ink_cairo_surface_copy(cairo_surface_t *s)
{
    cairo_surface_t *ns = ink_cairo_surface_create_identical(s);

    if (cairo_surface_get_type(s) == CAIRO_SURFACE_TYPE_IMAGE) {
        // use memory copy instead of using a Cairo context
        cairo_surface_flush(s);
        try {
            ink_cairo_surface_require_pixels(s);
            ink_cairo_surface_require_pixels(ns);
        } catch (...) {
            cairo_surface_destroy(ns);
            throw;
        }
        int stride = cairo_image_surface_get_stride(s);
        int h = cairo_image_surface_get_height(s);
        memcpy(cairo_image_surface_get_data(ns), cairo_image_surface_get_data(s), ink_cairo_surface_byte_size(stride, h));
        cairo_surface_mark_dirty(ns);
    } else {
        // generic implementation
        cairo_t *ct = cairo_create(ns);
        cairo_set_source_surface(ct, s, 0, 0);
        cairo_set_operator(ct, CAIRO_OPERATOR_SOURCE);
        cairo_paint(ct);
        cairo_destroy(ct);
    }

    return ns;
}

/**
 * Create an exact copy of an image surface.
 */
Cairo::RefPtr<Cairo::ImageSurface>
ink_cairo_surface_copy(Cairo::RefPtr<Cairo::ImageSurface> surface )
{
    int width  = surface->get_width();
    int height = surface->get_height();
    int stride = surface->get_stride();
    auto new_surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, width, height); // device scale?

    surface->flush();
    memcpy(new_surface->get_data(), surface->get_data(), ink_cairo_surface_byte_size(stride, height));
    new_surface->mark_dirty(); // Clear caches. Mandatory after messing directly with contents.

    return new_surface;
}

/**
 * Create a surface that differs only in pixel content.
 * Creates a surface that has the same type, content type and dimensions
 * as the specified surface. Pixel contents are not copied.
 */
cairo_surface_t *
ink_cairo_surface_create_identical(cairo_surface_t *s)
{
    cairo_surface_t *ns = ink_cairo_surface_create_same_size(s, cairo_surface_get_content(s));
    cairo_surface_set_user_data(ns, &ink_color_interpolation_key, cairo_surface_get_user_data(s, &ink_color_interpolation_key), nullptr);
    return ns;
}

cairo_surface_t *
ink_cairo_surface_create_same_size(cairo_surface_t *s, cairo_content_t c)
{
    // ink_cairo_surface_get_width()/height() returns value in pixels
    // cairo_surface_create_similar() uses device units
    double x_scale = 0;
    double y_scale = 0;
    cairo_surface_get_device_scale( s, &x_scale, &y_scale );

    assert (x_scale > 0);
    assert (y_scale > 0);

    cairo_surface_t *ns =
        cairo_surface_create_similar(s, c,
                                     ink_cairo_surface_get_width(s)/x_scale,
                                     ink_cairo_surface_get_height(s)/y_scale);
    if (cairo_surface_status(ns) != CAIRO_STATUS_SUCCESS) {
        // Filter primitives write straight into the pixel buffer; a failed surface has none.
        // Fail the render the way Cairomm does instead of letting them write through NULL.
        cairo_surface_destroy(ns);
        throw std::bad_alloc();
    }
    return ns;
}

/**
 * Extract the alpha channel into a new surface.
 * Creates a surface with a content type of CAIRO_CONTENT_ALPHA that contains
 * the alpha values of pixels from @a s.
 */
cairo_surface_t *
ink_cairo_extract_alpha(cairo_surface_t *s)
{
    cairo_surface_t *alpha = ink_cairo_surface_create_same_size(s, CAIRO_CONTENT_ALPHA);

    cairo_t *ct = cairo_create(alpha);
    cairo_set_source_surface(ct, s, 0, 0);
    cairo_set_operator(ct, CAIRO_OPERATOR_SOURCE);
    cairo_paint(ct);
    cairo_destroy(ct);

    return alpha;
}

cairo_surface_t *
ink_cairo_surface_create_output(cairo_surface_t *image, cairo_surface_t *bg)
{
    cairo_content_t imgt = cairo_surface_get_content(image);
    cairo_content_t bgt = cairo_surface_get_content(bg);
    cairo_surface_t *out = nullptr;

    if (bgt == CAIRO_CONTENT_ALPHA && imgt == CAIRO_CONTENT_ALPHA) {
        out = ink_cairo_surface_create_identical(bg);
    } else {
        out = ink_cairo_surface_create_same_size(bg, CAIRO_CONTENT_COLOR_ALPHA);
    }

    return out;
}

void
ink_cairo_surface_blit(cairo_surface_t *src, cairo_surface_t *dest)
{
    if (cairo_surface_get_type(src) == CAIRO_SURFACE_TYPE_IMAGE &&
        cairo_surface_get_type(dest) == CAIRO_SURFACE_TYPE_IMAGE &&
        cairo_image_surface_get_format(src) == cairo_image_surface_get_format(dest) &&
        cairo_image_surface_get_height(src) == cairo_image_surface_get_height(dest) &&
        cairo_image_surface_get_width(src) == cairo_image_surface_get_width(dest) &&
        cairo_image_surface_get_stride(src) == cairo_image_surface_get_stride(dest))
    {
        // use memory copy instead of using a Cairo context
        cairo_surface_flush(src);
        ink_cairo_surface_require_pixels(src);
        ink_cairo_surface_require_pixels(dest);
        int stride = cairo_image_surface_get_stride(src);
        int h = cairo_image_surface_get_height(src);
        memcpy(cairo_image_surface_get_data(dest), cairo_image_surface_get_data(src), ink_cairo_surface_byte_size(stride, h));
        cairo_surface_mark_dirty(dest);
    } else {
        // generic implementation
        cairo_t *ct = cairo_create(dest);
        cairo_set_source_surface(ct, src, 0, 0);
        cairo_set_operator(ct, CAIRO_OPERATOR_SOURCE);
        cairo_paint(ct);
        cairo_destroy(ct);
    }
}

/**
 * Return width in pixels.
 */
int
ink_cairo_surface_get_width(cairo_surface_t *surface)
{
    // For now only image surface is handled.
    // Later add others, e.g. cairo-gl
    assert(cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE);
    return cairo_image_surface_get_width(surface);
}

/**
 * Return height in pixels.
 */
int
ink_cairo_surface_get_height(cairo_surface_t *surface)
{
    assert(cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE);
    return cairo_image_surface_get_height(surface);
}

static double ink_cairo_surface_average_color_internal(cairo_surface_t *surface, cairo_surface_t *mask, double &rf, double &gf, double &bf, double &af)
{
    rf = gf = bf = af = 0.0;

    cairo_surface_flush(surface);
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    double count = 0;

    if (width * 4 != cairo_image_surface_get_stride(surface)) {
        g_warning("Stride and width don't match, expect zero buffer for average color.");
    }

    unsigned char *mask_data = nullptr;
    if (mask) {
        cairo_surface_flush(mask);
        if (width != cairo_image_surface_get_width(mask)
         || height != cairo_image_surface_get_height(mask)) {
            g_warning("Mask size must be exactly the same size as the image surface.");
            mask = nullptr;
        } else {
            mask_data = cairo_image_surface_get_data(mask);
        }
    }

    for (int p = 0; p < (width * height); ++p, data += 4) {
        EXTRACT_ARGB32(*reinterpret_cast<guint32*>(data), a, r, g, b)

        double amount = mask_data ? mask_data[p] / 255.0 : 1.0;
        rf += r / 255.0 * amount;
        gf += g / 255.0 * amount;
        bf += b / 255.0 * amount;
        af += a / 255.0 * amount;
        count += amount;
    }
    return count;
}

// We extract colors from pattern background, if we need to extract sometimes from a gradient we can add
// a extra parameter with the spot number and use cairo_pattern_get_color_stop_rgba
// also if the pattern is a image we can pass a boolean like solid = false to get the color by image average ink_cairo_surface_average_color
guint32 ink_cairo_pattern_get_argb32(cairo_pattern_t *pattern)
{
    double red = 0;
    double green = 0;
    double blue = 0;
    double alpha = 0;
    auto status = cairo_pattern_get_rgba(pattern, &red, &green, &blue, &alpha);
    if (status != CAIRO_STATUS_PATTERN_TYPE_MISMATCH) {
        // in ARGB32 format
        return SP_RGBA32_F_COMPOSE(alpha, red, green, blue);
    }
        
    cairo_surface_t *surface;
    status = cairo_pattern_get_surface (pattern, &surface);
    if (status != CAIRO_STATUS_PATTERN_TYPE_MISMATCH) {
        // first pixel only
        auto *pxbsurface =  cairo_image_surface_get_data(surface);
        return *reinterpret_cast<guint32 const *>(pxbsurface);
    }
    return 0;
}

/**
 * Get the average color from the given surface.
 *
 * @arg surface - The cairo surface to get data from.
 * @arg masked - If true, ignores any truely invisible pixels.
 */
Colors::Color ink_cairo_surface_average_color(cairo_surface_t *surface, cairo_surface_t * mask)
{
    double r, g, b, a = 0.0;
    double count = ink_cairo_surface_average_color_internal(surface, mask, r, g, b, a);
    if (a == 0.0) {
        return Colors::Color(Colors::Space::Type::RGB, {0, 0, 0, 0});
    }
    auto color = Colors::Color(Colors::Space::Type::RGB, {r / a, g / a, b / a, a / count});
    color.normalize();
    return color;
}

static guint32 srgb_to_linear( const guint32 c, const guint32 a ) {

    const guint32 c1 = unpremul_alpha( c, a );

    double cc = c1/255.0;

    if( cc < 0.04045 ) {
        cc /= 12.92;
    } else {
        cc = pow( (cc+0.055)/1.055, 2.4 );
    }
    cc *= 255.0;

    const guint32 c2 = (int)cc;

    return premul_alpha( c2, a );
}

static guint32 linear_to_srgb( const guint32 c, const guint32 a ) {

    const guint32 c1 = unpremul_alpha( c, a );

    double cc = c1/255.0;

    if( cc < 0.0031308 ) {
        cc *= 12.92;
    } else {
        cc = pow( cc, 1.0/2.4 )*1.055-0.055;
    }
    cc *= 255.0;

    const guint32 c2 = (int)cc;

    return premul_alpha( c2, a );
}

static uint32_t srgb_to_linear_argb32(uint32_t in)
{
    EXTRACT_ARGB32(in, a, r, g, b);
    if (a != 0) {
        r = srgb_to_linear(r, a);
        g = srgb_to_linear(g, a);
        b = srgb_to_linear(b, a);
    }
    ASSEMBLE_ARGB32(out, a, r, g, b);
    return out;
}

int ink_cairo_surface_srgb_to_linear(cairo_surface_t *surface)
{
    cairo_surface_flush(surface);
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);

    ink_cairo_surface_filter(surface, surface, srgb_to_linear_argb32);

    return width * height;
}

static uint32_t linear_to_srgb_argb32(uint32_t in)
{
    EXTRACT_ARGB32(in, a, r, g, b);
    if (a != 0) {
        r = linear_to_srgb(r, a);
        g = linear_to_srgb(g, a);
        b = linear_to_srgb(b, a);
    }
    ASSEMBLE_ARGB32(out, a, r, g, b);
    return out;
}

SPBlendMode ink_cairo_operator_to_css_blend(cairo_operator_t cairo_operator)
{
    // All of the blend modes are implemented in Cairo as of 1.10.
    // For a detailed description, see:
    // http://cairographics.org/operators/

    switch (cairo_operator) {
        case CAIRO_OPERATOR_MULTIPLY:
            return SP_CSS_BLEND_MULTIPLY;
        case CAIRO_OPERATOR_SCREEN:
            return SP_CSS_BLEND_SCREEN;
        case CAIRO_OPERATOR_DARKEN:
            return SP_CSS_BLEND_DARKEN;
        case CAIRO_OPERATOR_LIGHTEN:
            return SP_CSS_BLEND_LIGHTEN;
        case CAIRO_OPERATOR_OVERLAY:
            return SP_CSS_BLEND_OVERLAY;
        case CAIRO_OPERATOR_COLOR_DODGE:
            return SP_CSS_BLEND_COLORDODGE;
        case CAIRO_OPERATOR_COLOR_BURN:
            return SP_CSS_BLEND_COLORBURN;
        case CAIRO_OPERATOR_HARD_LIGHT:
            return SP_CSS_BLEND_HARDLIGHT;
        case CAIRO_OPERATOR_SOFT_LIGHT:
            return SP_CSS_BLEND_SOFTLIGHT;
        case CAIRO_OPERATOR_DIFFERENCE:
            return SP_CSS_BLEND_DIFFERENCE;
        case CAIRO_OPERATOR_EXCLUSION:
            return SP_CSS_BLEND_EXCLUSION;
        case CAIRO_OPERATOR_HSL_HUE:
            return SP_CSS_BLEND_HUE;
        case CAIRO_OPERATOR_HSL_SATURATION:
            return SP_CSS_BLEND_SATURATION;
        case CAIRO_OPERATOR_HSL_COLOR:
            return SP_CSS_BLEND_COLOR;
        case CAIRO_OPERATOR_HSL_LUMINOSITY:
            return SP_CSS_BLEND_LUMINOSITY;
        case CAIRO_OPERATOR_OVER:
            return SP_CSS_BLEND_NORMAL;
        default:
            return SP_CSS_BLEND_NORMAL;
    }
}

cairo_operator_t ink_css_blend_to_cairo_operator(SPBlendMode css_blend)
{
    // All of the blend modes are implemented in Cairo as of 1.10.
    // For a detailed description, see:
    // http://cairographics.org/operators/

    switch (css_blend) {
        case SP_CSS_BLEND_MULTIPLY:
            return CAIRO_OPERATOR_MULTIPLY;
        case SP_CSS_BLEND_SCREEN:
            return CAIRO_OPERATOR_SCREEN;
        case SP_CSS_BLEND_DARKEN:
            return CAIRO_OPERATOR_DARKEN;
        case SP_CSS_BLEND_LIGHTEN:
            return CAIRO_OPERATOR_LIGHTEN;
        case SP_CSS_BLEND_OVERLAY:
            return CAIRO_OPERATOR_OVERLAY;
        case SP_CSS_BLEND_COLORDODGE:
            return CAIRO_OPERATOR_COLOR_DODGE;
        case SP_CSS_BLEND_COLORBURN:
            return CAIRO_OPERATOR_COLOR_BURN;
        case SP_CSS_BLEND_HARDLIGHT:
            return CAIRO_OPERATOR_HARD_LIGHT;
        case SP_CSS_BLEND_SOFTLIGHT:
            return CAIRO_OPERATOR_SOFT_LIGHT;
        case SP_CSS_BLEND_DIFFERENCE:
            return CAIRO_OPERATOR_DIFFERENCE;
        case SP_CSS_BLEND_EXCLUSION:
            return CAIRO_OPERATOR_EXCLUSION;
        case SP_CSS_BLEND_HUE:
            return CAIRO_OPERATOR_HSL_HUE;
        case SP_CSS_BLEND_SATURATION:
            return CAIRO_OPERATOR_HSL_SATURATION;
        case SP_CSS_BLEND_COLOR:
            return CAIRO_OPERATOR_HSL_COLOR;
        case SP_CSS_BLEND_LUMINOSITY:
            return CAIRO_OPERATOR_HSL_LUMINOSITY;
        case SP_CSS_BLEND_NORMAL:
            return CAIRO_OPERATOR_OVER;
        default:
            g_error("Invalid SPBlendMode %d", css_blend);
            return CAIRO_OPERATOR_OVER;
    }
}

int ink_cairo_surface_linear_to_srgb(cairo_surface_t *surface)
{
    cairo_surface_flush(surface);
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);

    ink_cairo_surface_filter(surface, surface, linear_to_srgb_argb32);

    return width * height;
}

Cairo::RefPtr<Cairo::Pattern> ink_cairo_pattern_create_slanting_stripes(uint32_t color)
{
    constexpr int width = 10;
    constexpr int line_width = 10;

    auto surface = Cairo::ImageSurface::create(Cairo::ImageSurface::Format::ARGB32, width, 1);
    auto context = Cairo::Context::create(surface);
    context->rectangle(0, 0, line_width / 2.0, 1);
    ink_cairo_set_source_color(context, Colors::Color(color));
    context->fill();
    context->paint();

    auto pattern = Cairo::SurfacePattern::create(surface);
    pattern->set_extend(Cairo::SurfacePattern::Extend::REPEAT);
    pattern->set_filter(Cairo::SurfacePattern::Filter::NEAREST);
    pattern->set_matrix(Cairo::rotation_matrix(3 * M_PI / 4));
    return pattern;
}

Cairo::RefPtr<Cairo::Pattern> create_checkerboard_pattern(uint32_t dark, uint32_t light, int size) {
    auto img = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 2 * size, 2 * size);
    auto ctx = Cairo::Context::create(img);
    ctx->set_source_rgb(SP_RGBA32_R_F(dark), SP_RGBA32_G_F(dark), SP_RGBA32_B_F(dark));
    ctx->paint();
    ctx->set_source_rgb(SP_RGBA32_R_F(light), SP_RGBA32_G_F(light), SP_RGBA32_B_F(light));
    ctx->rectangle(0, 0, size, size);
    ctx->fill();
    ctx->rectangle(size, size, size, size);
    ctx->fill();
    auto pattern = Cairo::SurfacePattern::create(img);
    pattern->set_extend(Cairo::Pattern::Extend::REPEAT);
    pattern->set_filter(Cairo::SurfacePattern::Filter::NEAREST);
    return pattern;
}

Cairo::RefPtr<Cairo::Pattern> ink_cairo_pattern_create_checkerboard(guint32 rgba, bool use_alpha, int size) {
    auto color_a = Colors::Color(rgba, use_alpha);
    auto color_b = Inkscape::Colors::make_contrasted_color(color_a, 1.0);
    // Once the second color is generated, the original doesn't need alpha
    color_a.enableOpacity(false);
    return create_checkerboard_pattern(color_a.toRGBA(), color_b.toRGBA(), size);
}

/** 
 * Draw drop shadow around the 'rect' with given 'size' and 'color'; shadow extends to the right and bottom of rect.
 */
void ink_cairo_draw_drop_shadow(const Cairo::RefPtr<Cairo::Context> &ctx, const Geom::Rect& rect, double size, guint32 color, double color_alpha) {
    // draw fake drop shadow built from gradients
    const auto r = SP_RGBA32_R_F(color);
    const auto g = SP_RGBA32_G_F(color);
    const auto b = SP_RGBA32_B_F(color);
    const auto a = color_alpha;
    const Geom::Point corners[] = { rect.corner(0), rect.corner(1), rect.corner(2), rect.corner(3) };
    // space for gradient shadow
    double sw = size;
    double half = sw / 2;
    using Geom::X;
    using Geom::Y;
    // 8 gradients total: 4 sides + 4 corners
    auto grad_top    = Cairo::LinearGradient::create(0, corners[0][Y] + half, 0, corners[0][Y] - half);
    auto grad_right  = Cairo::LinearGradient::create(corners[1][X], 0, corners[1][X] + sw, 0);
    auto grad_bottom = Cairo::LinearGradient::create(0, corners[2][Y], 0, corners[2][Y] + sw);
    auto grad_left   = Cairo::LinearGradient::create(corners[0][X] + half, 0, corners[0][X] - half, 0);
    auto grad_btm_right = Cairo::RadialGradient::create(corners[2][X], corners[2][Y], 0, corners[2][X], corners[2][Y], sw);
    auto grad_top_right = Cairo::RadialGradient::create(corners[1][X], corners[1][Y] + half, 0, corners[1][X], corners[1][Y] + half, sw);
    auto grad_btm_left  = Cairo::RadialGradient::create(corners[3][X] + half, corners[3][Y], 0, corners[3][X] + half, corners[3][Y], sw);
    auto grad_top_left  = Cairo::RadialGradient::create(corners[0][X], corners[0][Y], 0, corners[0][X], corners[0][Y], half);
    const int N = 15; // number of gradient stops; stops used to make it non-linear
    // using easing function here: (exp(a*(1-t)) - 1) / (exp(a) - 1);
    // it has a nice property of growing from 0 to 1 for t in [0..1]
    const auto A = 4.0; // this coefficient changes how steep the curve is and controls shadow drop-off
    const auto denominator = exp(A) - 1;
    for (int i = 0; i <= N; ++i) {
        auto pos = static_cast<double>(i) / N;
        // exponential decay for drop shadow - long tail, with values from 100% down to 0% opacity
        auto t = 1 - pos; // reverse 't' so alpha drops from 1 to 0
        auto alpha = (exp(A * t) - 1) / denominator;
        grad_top->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_bottom->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_right->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_left->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_btm_right->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_top_right->add_color_stop_rgba(pos, r, g, b, alpha * a);
        grad_btm_left->add_color_stop_rgba(pos, r, g, b, alpha * a);
        // this left/top corner is just a silver of the shadow: half of it is "hidden" beneath the page
        if (pos >= 0.5) {
            grad_top_left->add_color_stop_rgba(2 * (pos - 0.5), r, g, b, alpha * a);
        }
    }

    // shadow at the top (faint)
    ctx->rectangle(corners[0][X], corners[0][Y] - half, std::max(corners[1][X] - corners[0][X], 0.0), half);
    ctx->set_source(grad_top);
    ctx->fill();

    // right side
    ctx->rectangle(corners[1][X], corners[1][Y] + half, sw, std::max(corners[2][Y] - corners[1][Y] - half, 0.0));
    ctx->set_source(grad_right);
    ctx->fill();

    // bottom side
    ctx->rectangle(corners[0][X] + half, corners[2][Y], std::max(corners[1][X] - corners[0][X] - half, 0.0), sw);
    ctx->set_source(grad_bottom);
    ctx->fill();

    // left side (faint)
    ctx->rectangle(corners[0][X] - half, corners[0][Y], half, std::max(corners[2][Y] - corners[1][Y], 0.0));
    ctx->set_source(grad_left);
    ctx->fill();

    // bottom corners
    ctx->rectangle(corners[2][X], corners[2][Y], sw, sw);
    ctx->set_source(grad_btm_right);
    ctx->fill();

    ctx->rectangle(corners[3][X] - half, corners[3][Y], std::min(sw, rect.width() + half), sw);
    ctx->set_source(grad_btm_left);
    ctx->fill();

    // top corners
    ctx->rectangle(corners[1][X], corners[1][Y] - half, sw, std::min(sw, rect.height() + half));
    ctx->set_source(grad_top_right);
    ctx->fill();

    ctx->rectangle(corners[0][X] - half, corners[0][Y] - half, half, half);
    ctx->set_source(grad_top_left);
    ctx->fill();
}

/**
 * Converts the Cairo surface to a GdkPixbuf pixel format,
 * without allocating extra memory.
 *
 * This function is intended mainly for creating previews displayed by GTK.
 * For loading images for display on the canvas, use the Inkscape::Pixbuf object.
 *
 * The returned GdkPixbuf takes ownership of the passed surface reference,
 * so it should NOT be freed after calling this function.
 */
GdkPixbuf *ink_pixbuf_create_from_cairo_surface(cairo_surface_t *s)
{
    guchar *pixels = cairo_image_surface_get_data(s);
    int w = cairo_image_surface_get_width(s);
    int h = cairo_image_surface_get_height(s);
    int rs = cairo_image_surface_get_stride(s);

    convert_pixels_argb32_to_pixbuf(pixels, w, h, rs);

    GdkPixbuf *pb = gdk_pixbuf_new_from_data(
        pixels, GDK_COLORSPACE_RGB, TRUE, 8,
        w, h, rs, ink_cairo_pixbuf_cleanup, s);

    return pb;
}

/**
 * Cleanup function for GdkPixbuf.
 * This function should be passed as the GdkPixbufDestroyNotify parameter
 * to gdk_pixbuf_new_from_data when creating a GdkPixbuf backed by
 * a Cairo surface.
 */
void ink_cairo_pixbuf_cleanup(guchar * /*pixels*/, void *data)
{
    cairo_surface_t *surface = static_cast<cairo_surface_t*>(data);
    cairo_surface_destroy(surface);
}

/* The following two functions use "from" instead of "to", because when you write:
   val1 = argb32_from_pixbuf(val1);
   the name of the format is closer to the value in that format. */

guint32 argb32_from_pixbuf(guint32 c)
{
    uint32_t a;
    if constexpr (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
        a = (c & 0xff000000) >> 24;
    } else {
        a = (c & 0x000000ff);
    }

    if (a == 0) {
        return 0;
    }

    // extract color components
    uint32_t r, g, b;
    if constexpr (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
        r = (c & 0x000000ff);
        g = (c & 0x0000ff00) >> 8;
        b = (c & 0x00ff0000) >> 16;
    } else {
        r = (c & 0xff000000) >> 24;
        g = (c & 0x00ff0000) >> 16;
        b = (c & 0x0000ff00) >> 8;
    }

    // premultiply
    r = premul_alpha(r, a);
    b = premul_alpha(b, a);
    g = premul_alpha(g, a);

    // combine into output
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/**
 * Convert one pixel from ARGB to GdkPixbuf format.
 *
 * @param c ARGB color
 * @param bgcolor Color to use if c.alpha is zero (bgcolor.alpha is ignored)
 */
guint32 pixbuf_from_argb32(guint32 c, guint32 bgcolor)
{
    guint32 a = (c & 0xff000000) >> 24;
    if (a == 0) {
        assert(c == 0);
        c = bgcolor;
    }

    // extract color components
    guint32 r = (c & 0x00ff0000) >> 16;
    guint32 g = (c & 0x0000ff00) >> 8;
    guint32 b = (c & 0x000000ff);

    if (a != 0) {
        r = unpremul_alpha(r, a);
        g = unpremul_alpha(g, a);
        b = unpremul_alpha(b, a);
    }

    // combine into output
    if constexpr (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
        return r | (g << 8) | (b << 16) | (a << 24);
    } else {
        return (r << 24) | (g << 16) | (b << 8) | a;
    }
}

/**
 * Convert pixel data from GdkPixbuf format to ARGB.
 * This will convert pixel data from GdkPixbuf format to Cairo's native pixel format.
 * This involves premultiplying alpha and shuffling around the channels.
 * Pixbuf data must have an alpha channel, otherwise the results are undefined
 * (usually a segfault).
 */
void
convert_pixels_pixbuf_to_argb32(guchar *data, int w, int h, int stride)
{
    if (!data || w < 1 || h < 1 || stride < 1) {
        return;
    }

    for (size_t i = 0; i < h; ++i) {
        guint32 *px = reinterpret_cast<guint32*>(data + i*stride);
        for (size_t j = 0; j < w; ++j) {
            *px = argb32_from_pixbuf(*px);
            ++px;
        }
    }
}

/**
 * Convert pixel data from ARGB to GdkPixbuf format.
 * This will convert pixel data from GdkPixbuf format to Cairo's native pixel format.
 * This involves premultiplying alpha and shuffling around the channels.
 */
void
convert_pixels_argb32_to_pixbuf(guchar *data, int w, int h, int stride, guint32 bgcolor)
{
    if (!data || w < 1 || h < 1 || stride < 1) {
        return;
    }
    for (size_t i = 0; i < h; ++i) {
        guint32 *px = reinterpret_cast<guint32*>(data + i*stride);
        for (size_t j = 0; j < w; ++j) {
            *px = pixbuf_from_argb32(*px, bgcolor);
            ++px;
        }
    }
}

guint32 argb32_from_rgba(guint32 in)
{
    guint32 r, g, b, a;
    a = (in & 0x000000ff);
    r = premul_alpha((in & 0xff000000) >> 24, a);
    g = premul_alpha((in & 0x00ff0000) >> 16, a);
    b = premul_alpha((in & 0x0000ff00) >> 8,  a);
    ASSEMBLE_ARGB32(px, a, r, g, b)
    return px;
}


/**
 * Convert one pixel from ARGB to GdkPixbuf format.
 *
 * @param c RGBA color
 */
guint32 rgba_from_argb32(guint32 c)
{
    guint32 a = (c & 0xff000000) >> 24;
    guint32 r = (c & 0x00ff0000) >> 16;
    guint32 g = (c & 0x0000ff00) >> 8;
    guint32 b = (c & 0x000000ff);

    if (a != 0) {
        r = unpremul_alpha(r, a);
        g = unpremul_alpha(g, a);
        b = unpremul_alpha(b, a);
    }

    // combine into output
    guint32 o = (r << 24) | (g << 16) | (b << 8) | (a);

    return o;
}

static constexpr uint16_t get_luminance(uint32_t r, uint32_t g, uint32_t b)
{
    return ((1063 * r + 3576 * g + 361 * b) * 257 + 2500) / 5000;
}

static_assert(
    [] {
        for (int x = 0; x < 256; x++) {
            const uint16_t hex = 0x101 * x;
            assert(get_luminance(x, x, x) == hex);
        }
        return true;
    }(),
    "get_luminance function doesn't produce expected luminance values");

/**
 * Converts a pixbuf to a PNG data structure.
 * For 8-but RGBA png, this is like copying.
 *
 */
const guchar* pixbuf_to_png(guchar const**rows, guchar* px, int num_rows, int num_cols, int stride, int color_type, int bit_depth)
{
    int n_fields = 1 + (color_type&2) + (color_type&4)/4;
    size_t const row_bytes = (static_cast<size_t>(n_fields) * bit_depth * num_cols + 7) / 8;
    const guchar* new_data = (const guchar*)malloc(row_bytes * num_rows);
    if (!new_data) {
        return nullptr; // Out of memory; the caller must check.
    }
    char* ptr = (char*) new_data;
    // Used when we write image data smaller than one byte (for instance in
    // black and white images where 1px = 1bit). Only possible with greyscale.
    int pad = 0;
    for (int row = 0; row < num_rows; ++row) {
        rows[row] = (const guchar*)ptr;
        for (int col = 0; col < num_cols; ++col) {
            guint32 *pixel = reinterpret_cast<guint32*>(px + static_cast<size_t>(row) * stride) + col;

            guint64 pix3 = (*pixel & 0xff000000) >> 24;
            guint64 pix2 = (*pixel & 0x00ff0000) >> 16;
            guint64 pix1 = (*pixel & 0x0000ff00) >> 8;
            guint64 pix0 = (*pixel & 0x000000ff);

            uint64_t a, r, g, b;
            if constexpr (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
                a = pix3;
                b = pix2;
                g = pix1;
                r = pix0;
            } else {
                r = pix3;
                g = pix2;
                b = pix1;
                a = pix0;
            }

            // One of possible rgb to greyscale formulas. This one is called "luminance", "luminosity" or "luma"
            uint16_t const gray = get_luminance(r, g, b);

            if (color_type & 2) { // RGB or RGBA
                // for 8bit->16bit transition, I take the FF -> FFFF convention (multiplication by 0x101). 
                // If you prefer FF -> FF00 (multiplication by 0x100), remove the <<8, <<24, <<40 and <<56
                // for little-endian, and remove the <<0, <<16, <<32 and <<48 for big-endian.
                if (color_type & 4) { // RGBA
                    if (bit_depth == 8)
                        *((guint32*)ptr) = *pixel; 
                    else 
                        // This uses the samples in the order they appear in pixel rather than
                        // normalised to abgr or rgba in order to make it endian agnostic,
                        // exploiting the symmetry of the expression (0x101 is the same in both
                        // endiannesses and each sample is multiplied by that).
                        *((guint64*)ptr) = (guint64)((pix3<<56)+(pix3<<48)+(pix2<<40)+(pix2<<32)+(pix1<<24)+(pix1<<16)+(pix0<<8)+(pix0));
                } else { // RGB
                    if (bit_depth == 8) {
                        *ptr = r;
                        *(ptr+1) = g;
                        *(ptr+2) = b;
                    } else {
                        *((guint16*)ptr) = (r<<8)+r;
                        *((guint16*)(ptr+2)) = (g<<8)+g;
                        *((guint16*)(ptr+4)) = (b<<8)+b;
                    }
                }
            } else { // Grayscale
                if (bit_depth == 16) {
                    if constexpr (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
                        *(guint16*)ptr = ((gray & 0xff00)>>8) + ((gray & 0x00ff)<<8);
                    } else {
                        *(guint16*)ptr = gray;
                    }
                    // For 8bit->16bit this mirrors RGB(A), multiplying by
                    // 0x101; if you prefer multiplying by 0x100, remove the
                    // <<8 for little-endian, and remove the unshifted value
                    // for big-endian.
                    if (color_type & 4) // Alpha channel
                        *((guint16*)(ptr+2)) = a + (a<<8);
                } else if (bit_depth == 8) {
                    *ptr = guint8(gray >> 8);
                    if (color_type & 4) // Alpha channel
                        *((guint8*)(ptr+1)) = a;
                } else {
                    if (!pad) *ptr=0;
                    // In PNG numbers are stored left to right, but in most significant bits first, so the first one processed is the ``big'' mask, etc.
                    int realpad = 8 - bit_depth - pad;
                    *ptr += guint8((gray >> (16-bit_depth))<<realpad); // Note the "+="
                    if (color_type & 4) // Alpha channel
                        *(ptr+1) += guint8((a >> (8-bit_depth))<<(bit_depth + realpad));
                }
            }

            pad += bit_depth*n_fields;
            ptr += pad/8;
            pad %= 8;
        }
        // Align bytes on rows
        if (pad) {
            pad = 0;
            ptr++;
        }
    }
    return new_data; 
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
