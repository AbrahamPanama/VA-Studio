// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Tests for the Explode Bitmap engine (Header): T06 header arithmetic, T17 hostile inputs,
 * T23 source/colour classification. Synthetic bytes only.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "util/bitmap-input-header.h"

using namespace Inkscape::Bitmap;
using Bytes = std::vector<std::uint8_t>;

namespace {

void put(Bytes &b, std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }
void be32(Bytes &b, std::uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(std::uint8_t(v >> s)); }
void le32(Bytes &b, std::uint32_t v) { for (int s = 0; s < 32; s += 8) b.push_back(std::uint8_t(v >> s)); }
void le16(Bytes &b, std::uint32_t v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); }
void be16(Bytes &b, std::uint32_t v) { b.push_back((v >> 8) & 255); b.push_back(v & 255); }

std::uint32_t crc(Bytes const &d, std::size_t from, std::size_t to)
{
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = from; i < to; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

void chunk(Bytes &b, char const *type, Bytes const &data, bool badCrc = false)
{
    be32(b, std::uint32_t(data.size()));
    std::size_t s = b.size();
    put(b, std::string_view(type, 4));
    b.insert(b.end(), data.begin(), data.end());
    be32(b, crc(b, s, b.size()) ^ (badCrc ? 1 : 0));
}

Bytes ihdr(std::uint32_t w, std::uint32_t h, int depth = 8, int ct = 6, int interlace = 0)
{
    Bytes d;
    be32(d, w); be32(d, h);
    d.insert(d.end(), {std::uint8_t(depth), std::uint8_t(ct), 0, 0, std::uint8_t(interlace)});
    return d;
}

struct PngOpt { int depth = 8, ct = 6, interlace = 0; bool badCrc = false, iccp = false, srgb = false, trns = false, actl = false; std::uint32_t frames = 2, iccBytes = 16, len = 0; };
Bytes png(std::uint32_t w, std::uint32_t h, PngOpt o = {})
{
    Bytes b{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    chunk(b, "IHDR", ihdr(w, h, o.depth, o.ct, o.interlace), o.badCrc);
    if (o.srgb) chunk(b, "sRGB", {0});
    if (o.iccp) chunk(b, "iCCP", Bytes(o.iccBytes, 1));
    if (o.actl) { Bytes a; be32(a, o.frames); be32(a, 0); chunk(b, "acTL", a); }
    if (o.ct == 3) chunk(b, "PLTE", {0, 0, 0});
    if (o.trns) chunk(b, "tRNS", {0});
    chunk(b, "IDAT", Bytes{1, 2, 3, 4, 5});
    chunk(b, "IEND", {});
    return b;
}

struct JpgOpt { int sof = 0xC0, comps = 1, precision = 8; std::uint32_t icc = 0, orientation = 0; int ffRun = 0; };
Bytes jpeg(std::uint32_t w, std::uint32_t h, JpgOpt o = {})
{
    Bytes b{0xFF, 0xD8};
    if (o.orientation) { // APP1 Exif big-endian TIFF with one orientation entry
        Bytes t; put(t, "MM"); be16(t, 42); be32(t, 8); be16(t, 1);
        be16(t, 0x0112); be16(t, 3); be32(t, 1); be16(t, o.orientation); be16(t, 0); be32(t, 0);
        b.push_back(0xFF); b.push_back(0xE1); be16(b, std::uint32_t(2 + 6 + t.size())); put(b, std::string_view("Exif\0\0", 6)); b.insert(b.end(), t.begin(), t.end());
    }
    if (o.icc) {
        b.push_back(0xFF); b.push_back(0xE2); be16(b, 2 + 14 + o.icc); put(b, std::string_view("ICC_PROFILE\0", 12)); b.push_back(1); b.push_back(1);
        b.insert(b.end(), o.icc, 7);
    }
    for (int i = 0; i < o.ffRun; ++i) b.push_back(0xFF);
    b.push_back(0xFF); b.push_back(std::uint8_t(o.sof)); be16(b, 8 + 3 * o.comps);
    b.push_back(std::uint8_t(o.precision)); be16(b, h); be16(b, w); b.push_back(std::uint8_t(o.comps));
    for (int i = 0; i < o.comps; ++i) b.insert(b.end(), {std::uint8_t(i + 1), 0x11, 0});
    b.insert(b.end(), {0xFF, 0xDA, 0, 8, 1, 1, 0, 0, 0x3F, 0, 0x12, 0x34, 0x56, 0xFF, 0x00, 0x78, 0xFF, 0xD9});
    return b;
}

void wchunk(Bytes &b, char const *t, Bytes const &d)
{
    put(b, std::string_view(t, 4)); le32(b, std::uint32_t(d.size())); b.insert(b.end(), d.begin(), d.end());
    if (d.size() & 1) b.push_back(0);
}
Bytes riff(Bytes const &chunks) { Bytes b; put(b, "RIFF"); le32(b, std::uint32_t(chunks.size() + 4)); put(b, "WEBP"); b.insert(b.end(), chunks.begin(), chunks.end()); return b; }
Bytes vp8l(std::uint32_t w, std::uint32_t h, bool alpha)
{
    Bytes d{0x2F}; le32(d, (w - 1) | ((h - 1) << 14) | (alpha ? 1u << 28 : 0)); d.push_back(0); return d;
}
Bytes vp8(std::uint32_t w, std::uint32_t h)
{
    Bytes d{0x10, 0, 0, 0x9D, 0x01, 0x2A}; le16(d, w); le16(d, h); d.push_back(0); d.push_back(0); return d;
}
Bytes vp8x(std::uint8_t flags, std::uint32_t w, std::uint32_t h)
{
    Bytes d{flags, 0, 0, 0}; for (std::uint32_t v : {w - 1, h - 1}) { d.push_back(v & 255); d.push_back((v >> 8) & 255); d.push_back((v >> 16) & 255); } return d;
}
Bytes webpL(std::uint32_t w, std::uint32_t h, bool alpha = false) { Bytes c; wchunk(c, "VP8L", vp8l(w, h, alpha)); return riff(c); }
Bytes webpLossy(std::uint32_t w, std::uint32_t h) { Bytes c; wchunk(c, "VP8 ", vp8(w, h)); return riff(c); }

Bytes gif(std::uint32_t w, std::uint32_t h, int frames = 1, bool transparent = false, bool interlaced = false)
{
    Bytes b; put(b, "GIF89a"); le16(b, w); le16(b, h); b.insert(b.end(), {0x80, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF});
    if (transparent) b.insert(b.end(), {0x21, 0xF9, 4, 1, 0, 0, 0, 0});
    for (int i = 0; i < frames; ++i) {
        b.insert(b.end(), {0x2C, 0, 0, 0, 0, 1, 0, 1, 0, std::uint8_t(interlaced ? 0x40 : 0), 2, 2, 0x44, 0x01, 0});
    }
    b.push_back(0x3B);
    return b;
}

Bytes bmp(std::int32_t w, std::int32_t h, int bpp = 24)
{
    Bytes b; put(b, "BM"); le32(b, 60); le32(b, 0); le32(b, 54); le32(b, 40); le32(b, std::uint32_t(w)); le32(b, std::uint32_t(h));
    le16(b, 1); le16(b, bpp); b.resize(60, 0); return b;
}

Bytes tiff(std::uint32_t w, std::uint32_t h, std::uint32_t orientation = 1, bool second = false)
{
    Bytes t; put(t, "II"); le16(t, 42); le32(t, 8); le16(t, 4);
    auto ent = [&](std::uint32_t id, std::uint32_t v) { le16(t, id); le16(t, 4); le32(t, 1); le32(t, v); };
    ent(256, w); ent(257, h); ent(258, 8); ent(274, orientation); le32(t, second ? 200 : 0);
    return t;
}

struct Fixture {
    Budget budget{Budget::FixedLimitForTest{}, 64 * MiB};
    HeaderLimits lim;
    Result<RasterHeader> run(Bytes const &b, std::string_view mime = {}) { return inspect({b.data(), b.size(), mime}, lim, budget); }
};

} // namespace

#define EXPECT_REFUSED(r, st, text) \
    do { EXPECT_FALSE((r).ok()); EXPECT_EQ((r).outcome.status, st); EXPECT_NE(std::string((r).outcome.diagnostic).find(text), std::string::npos) << (r).outcome.diagnostic; } while (0)

// ---- T06: arithmetic and header boundaries ---------------------------------------------------
TEST(ExplodeBitmapHeader, T06AxisAndPixelBoundaries)
{
    Fixture f;
    auto ok = f.run(png(16384, 6103)); // 99,991,552 px
    EXPECT_TRUE(ok.ok()) << ok.outcome.diagnostic;
    EXPECT_EQ(ok.value.width, 16384u);
    EXPECT_REFUSED(f.run(png(16385, 1)), Status::incompatible, "axis");
    EXPECT_REFUSED(f.run(png(1, 16385)), Status::incompatible, "axis");
    f.lim.maxAxis = 20000;
    EXPECT_TRUE(f.run(png(10000, 10000)).ok()); // exactly 100 MP
    EXPECT_REFUSED(f.run(png(10000, 10001)), Status::incompatible, "pixel count");
    EXPECT_REFUSED(f.run(png(0, 5)), Status::incompatible, "zero-size");
    EXPECT_REFUSED(f.run(png(5, 0)), Status::incompatible, "zero-size");
    EXPECT_REFUSED(f.run(png(0xFFFFFFFFu, 0xFFFFFFFFu)), Status::incompatible, "axis");
    EXPECT_REFUSED(f.run(png(0x7FFFFFFFu, 0x7FFFFFFFu)), Status::incompatible, "axis");
    EXPECT_EQ(f.budget.reserved(), 0u);
}

TEST(ExplodeBitmapHeader, T06ScratchThroughBudget)
{
    Bytes b = png(4, 4);
    Budget tiny(Budget::FixedLimitForTest{}, 1024); // below the 1 MiB scratch reservation for large inputs, but this input is small
    HeaderLimits lim;
    EXPECT_TRUE(inspect({b.data(), b.size(), {}}, lim, tiny).ok());
    EXPECT_EQ(tiny.reserved(), 0u);
    Bytes big = png(4, 4);
    big.resize(2 * MiB, 0);
    auto r = inspect({big.data(), big.size(), {}}, lim, tiny);
    EXPECT_EQ(r.outcome.status, Status::failed); // scratch cannot be reserved: refused, nothing allocated
    EXPECT_EQ(tiny.reserved(), 0u);
}

TEST(ExplodeBitmapHeader, T06UriArithmetic)
{
    HeaderLimits lim;
    auto r = inspectUri("data:image/png;base64,QUJD", lim);
    ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    EXPECT_EQ(r.value.decodedBytes, 3u);
    EXPECT_EQ(r.value.payloadOffset, 22u);
    EXPECT_EQ(inspectUri("data:image/png;base64,QUI=", lim).value.decodedBytes, 2u);
    EXPECT_EQ(inspectUri("data:image/png;base64,QQ==", lim).value.decodedBytes, 1u);
    EXPECT_EQ(inspectUri("data:image/png;base64,QU\nJD\r\n", lim).value.decodedBytes, 3u);
    EXPECT_REFUSED(inspectUri("data:image/png;base64,QUJ", lim), Status::failed, "length");
    EXPECT_REFUSED(inspectUri("data:image/png;base64,Q===", lim), Status::failed, "padding");
    EXPECT_REFUSED(inspectUri("data:image/png;base64,QQ==QUJD", lim), Status::failed, "padding");
    EXPECT_REFUSED(inspectUri("data:image/png;base64,QU*D", lim), Status::failed, "invalid character");
    EXPECT_REFUSED(inspectUri("data:image/png;base64,", lim), Status::failed, "length");
    EXPECT_REFUSED(inspectUri("data:image/png;base64", lim), Status::failed, "separator");
    EXPECT_REFUSED(inspectUri("data:image/png,QUJD", lim), Status::incompatible, "base64");
    EXPECT_REFUSED(inspectUri("data:image/svg+xml;base64,QUJD", lim), Status::incompatible, "media type");
    EXPECT_REFUSED(inspectUri("data:;base64,QUJD", lim), Status::incompatible, "media type");
    lim.maxEncodedBytes = 2;
    EXPECT_REFUSED(inspectUri("data:image/png;base64,QUJD", lim), Status::incompatible, "size limit");
    lim.maxUriBytes = 10;
    EXPECT_REFUSED(inspectUri("data:image/png;base64,QUJD", lim), Status::incompatible, "size limit");
    EXPECT_TRUE(inspectUri("a.png", lim).ok());
    EXPECT_EQ(inspectUri("a.png", lim).value.kind, UriKind::Linked);
    EXPECT_FALSE(inspectUri("", lim).ok());
    auto flag = std::make_shared<std::atomic<bool>>(true);
    EXPECT_EQ(inspectUri("data:image/png;base64,QUJD", HeaderLimits{}, Stop(flag)).outcome.status, Status::canceled);
}

// ---- T17: hostile inputs ----------------------------------------------------------------------
TEST(ExplodeBitmapHeader, T17TruncationAtEveryOffset)
{
    Fixture f;
    std::vector<std::pair<char const *, Bytes>> files = {
        {"png", png(3, 2)}, {"jpeg", jpeg(3, 2)}, {"webp", webpL(3, 2)}, {"webp-x", [] { Bytes c; wchunk(c, "VP8X", vp8x(0x10, 3, 2)); wchunk(c, "VP8L", vp8l(3, 2, true)); return riff(c); }()},
        {"gif", gif(3, 2)}, {"bmp", bmp(3, 2)},
    };
    for (auto &[name, full] : files) {
        EXPECT_TRUE(f.run(full).value.width == 3 || std::string(name) == "bmp") << name;
        for (std::size_t n = 0; n < full.size(); ++n) {
            Bytes cut(full.begin(), full.begin() + n);
            auto r = inspect({cut.data(), cut.size(), {}}, f.lim, f.budget);
            EXPECT_FALSE(r.ok()) << name << " truncated at " << n;
            EXPECT_EQ(f.budget.reserved(), 0u) << name << " " << n;
        }
    }
}

TEST(ExplodeBitmapHeader, T17HugeDimensionsAndBombs)
{
    Fixture f;
    EXPECT_REFUSED(f.run(png(65535, 65535)), Status::incompatible, "axis");
    EXPECT_REFUSED(f.run(png(50000, 50000)), Status::incompatible, "axis");
    EXPECT_REFUSED(f.run(jpeg(65535, 65535)), Status::incompatible, "axis");
    EXPECT_REFUSED(f.run(jpeg(16384, 16384)), Status::incompatible, "pixel count");
    EXPECT_REFUSED(f.run(webpL(16384, 16384)), Status::incompatible, "pixel count");
    EXPECT_REFUSED(f.run(webpLossy(16383, 16383)), Status::incompatible, "pixel count");
    Bytes c; wchunk(c, "VP8X", vp8x(0, 0x1000000, 0x1000000)); wchunk(c, "VP8L", vp8l(1, 1, false));
    EXPECT_REFUSED(f.run(riff(c)), Status::incompatible, "axis");
    EXPECT_REFUSED(f.run(gif(0, 5)), Status::incompatible, "zero-size");
    EXPECT_REFUSED(f.run(jpeg(0, 5)), Status::incompatible, "zero-size");
    EXPECT_REFUSED(f.run(bmp(0x7FFFFFFF, 0x7FFFFFFF)), Status::incompatible, "axis");
    EXPECT_REFUSED(f.run(bmp(-5, 5)), Status::failed, "width");
    EXPECT_REFUSED(f.run(tiff(60000, 60000)), Status::incompatible, "axis");
    EXPECT_EQ(f.budget.reserved(), 0u);
}

TEST(ExplodeBitmapHeader, T17WrappingLengthsAndCrc)
{
    Fixture f;
    Bytes b = png(4, 4);
    // IDAT chunk length 0xFFFFFFF0 (wraps in 32-bit pos+12+len) and 0x7FFFFFFF (over the data).
    std::size_t idat = 8 + 25; // after IHDR chunk
    for (std::uint32_t len : {0xFFFFFFF0u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFF0u}) {
        Bytes c = b;
        for (int i = 0; i < 4; ++i) c[idat + i] = std::uint8_t(len >> (24 - 8 * i));
        EXPECT_FALSE(f.run(c).ok()) << std::hex << len;
    }
    EXPECT_REFUSED(f.run(png(4, 4, {.badCrc = true})), Status::failed, "CRC");
    Bytes d = b; d[8 + 8 + 4] ^= 1; // flip a width bit; CRC no longer matches
    EXPECT_REFUSED(f.run(d), Status::failed, "CRC");
    Bytes noEnd(b.begin(), b.end() - 12);
    EXPECT_REFUSED(f.run(noEnd), Status::failed, "IEND");
    Bytes e = b; e[8 + 3] = 12; // IHDR length 12
    EXPECT_FALSE(f.run(e).ok());
    // Bad depth/colour type, interlace byte.
    EXPECT_FALSE(f.run(png(4, 4, {.depth = 16, .ct = 3})).ok());
    EXPECT_FALSE(f.run(png(4, 4, {.depth = 8, .ct = 1})).ok());
    EXPECT_FALSE(f.run(png(4, 4, {.interlace = 2})).ok());
    // WebP RIFF size wraps / lies.
    Bytes w = webpL(4, 4);
    for (std::uint32_t sz : {0xFFFFFFFFu, 0xFFFFFFF8u, 0x7FFFFFFFu, 4u, 1u}) {
        Bytes x = w;
        for (int i = 0; i < 4; ++i) x[4 + i] = std::uint8_t(sz >> (8 * i));
        EXPECT_FALSE(f.run(x).ok()) << std::hex << sz;
    }
    Bytes x = w; x[16] = 0xFF; x[17] = 0xFF; x[18] = 0xFF; x[19] = 0xFF; // chunk size wraps
    EXPECT_FALSE(f.run(x).ok());
    // JPEG segment length lies.
    Bytes j = jpeg(4, 4);
    j[4] = 0xFF; j[5] = 0xFF;
    EXPECT_FALSE(f.run(j).ok());
    j = jpeg(4, 4); j[4] = 0; j[5] = 1;
    EXPECT_FALSE(f.run(j).ok());
    EXPECT_EQ(f.budget.reserved(), 0u);
}

TEST(ExplodeBitmapHeader, T17EndlessMarkerRunAndMetadataBombs)
{
    Fixture f;
    Bytes j{0xFF, 0xD8};
    j.insert(j.end(), 5 * MiB, 0xFF);
    EXPECT_REFUSED(f.run(j), Status::failed, "marker run");
    Bytes k = jpeg(4, 4, {.ffRun = 300});
    EXPECT_REFUSED(f.run(k), Status::failed, "marker run");
    Bytes m{0xFF, 0xD8};
    for (int i = 0; i < 70000; ++i) { m.insert(m.end(), {0xFF, 0xFE, 0, 2}); }
    EXPECT_REFUSED(f.run(m), Status::failed, "marker run");
    EXPECT_EQ(f.budget.reserved(), 0u);
}

TEST(ExplodeBitmapHeader, T17OversizedProfiles)
{
    Fixture f;
    f.lim.maxProfileBytes = 1000;
    EXPECT_TRUE(f.run(png(4, 4, {.iccp = true, .iccBytes = 1000})).ok());
    EXPECT_REFUSED(f.run(png(4, 4, {.iccp = true, .iccBytes = 1001})), Status::incompatible, "profile");
    EXPECT_TRUE(f.run(jpeg(4, 4, {.icc = 1000})).ok());
    EXPECT_REFUSED(f.run(jpeg(4, 4, {.icc = 1001})), Status::incompatible, "profile");
    Bytes c; wchunk(c, "VP8X", vp8x(0x20, 4, 4)); wchunk(c, "ICCP", Bytes(1001, 1)); wchunk(c, "VP8L", vp8l(4, 4, false));
    EXPECT_REFUSED(f.run(riff(c)), Status::incompatible, "profile");
    // A multi-megabyte iCCP is refused by length BEFORE its CRC pass: with a deliberately bad CRC
    // the diagnostic proves the checksum never ran (a CRC failure would be Status::failed).
    Fixture g;
    Bytes b{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    chunk(b, "IHDR", ihdr(4, 4));
    chunk(b, "iCCP", Bytes(4 * MiB + 1, 1), true);
    EXPECT_REFUSED(g.run(b), Status::incompatible, "profile");
    // Header metadata beyond profile+scratch before any image data is refused.
    Bytes t{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    chunk(t, "IHDR", ihdr(4, 4));
    for (int i = 0; i < 6; ++i) chunk(t, "tEXt", Bytes(MiB, 'a'));
    chunk(t, "IDAT", {1}); chunk(t, "IEND", {});
    EXPECT_REFUSED(g.run(t), Status::incompatible, "scan limit");
}

TEST(ExplodeBitmapHeader, T17MimeSignatureAndGarbage)
{
    Fixture f;
    Bytes p = png(4, 4), j = jpeg(4, 4);
    EXPECT_TRUE(f.run(p, "image/png").ok());
    EXPECT_TRUE(f.run(p, "IMAGE/PNG; charset=x").ok());
    EXPECT_TRUE(f.run(p, "application/octet-stream").ok());
    EXPECT_TRUE(f.run(j, "image/jpg").ok());
    EXPECT_REFUSED(f.run(p, "image/jpeg"), Status::incompatible, "disagree");
    EXPECT_REFUSED(f.run(j, "image/png"), Status::incompatible, "disagree");
    EXPECT_REFUSED(f.run(p, "image/svg+xml"), Status::incompatible, "media type");
    EXPECT_REFUSED(f.run(p, "text/html"), Status::incompatible, "media type");
    Bytes svg; put(svg, "<svg xmlns='http://www.w3.org/2000/svg'/>");
    EXPECT_REFUSED(f.run(svg), Status::incompatible, "Unrecognised");
    EXPECT_FALSE(inspect({nullptr, 0, {}}, f.lim, f.budget).ok());
    Bytes zeros(100, 0);
    EXPECT_FALSE(f.run(zeros).ok());
    auto flag = std::make_shared<std::atomic<bool>>(true);
    EXPECT_EQ(inspect({p.data(), p.size(), {}}, f.lim, f.budget, Stop(flag)).outcome.status, Status::canceled);
    EXPECT_EQ(f.budget.reserved(), 0u);
}

TEST(ExplodeBitmapHeader, T17ExhaustedBudgetRefusesBeforeParsing)
{
    Budget none(Budget::FixedLimitForTest{}, 0);
    Bytes p = png(4, 4);
    auto r = inspect({p.data(), p.size(), {}}, HeaderLimits{}, none);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.value.width, 0u); // nothing parsed
}

// ---- T23: classification of variants ---------------------------------------------------------
TEST(ExplodeBitmapHeader, T23PngVariants)
{
    Fixture f;
    struct Case { int depth, ct; ColorModel color; bool alpha; };
    for (Case c : {Case{16, 0, ColorModel::Gray, false}, Case{8, 2, ColorModel::RGB, false}, Case{16, 2, ColorModel::RGB, false},
                   Case{8, 3, ColorModel::Palette, false}, Case{4, 3, ColorModel::Palette, false}, Case{8, 4, ColorModel::GrayAlpha, true},
                   Case{16, 4, ColorModel::GrayAlpha, true}, Case{8, 6, ColorModel::RGBA, true}, Case{16, 6, ColorModel::RGBA, true}}) {
        auto r = f.run(png(7, 5, {.depth = c.depth, .ct = c.ct}));
        ASSERT_TRUE(r.ok()) << c.depth << "/" << c.ct << r.outcome.diagnostic;
        EXPECT_EQ(r.value.format, Format::PNG);
        EXPECT_EQ(r.value.color, c.color);
        EXPECT_EQ(r.value.alpha, c.alpha);
        EXPECT_EQ(r.value.bitDepth, std::uint32_t(c.depth));
        EXPECT_EQ(r.value.width, 7u);
        EXPECT_EQ(r.value.height, 5u);
    }
    EXPECT_TRUE(f.run(png(4, 4, {.ct = 3, .trns = true})).value.alpha);
    EXPECT_TRUE(f.run(png(4, 4, {.ct = 0, .trns = true})).value.alpha);
    EXPECT_TRUE(f.run(png(4, 4, {.interlace = 1})).value.interlaced);
    EXPECT_FALSE(f.run(png(4, 4)).value.interlaced);
    auto s = f.run(png(4, 4, {.srgb = true}));
    EXPECT_TRUE(s.value.srgbTagged);
    EXPECT_FALSE(s.value.hasProfile);
    auto i = f.run(png(4, 4, {.iccp = true, .iccBytes = 123}));
    EXPECT_TRUE(i.value.hasProfile);
    EXPECT_EQ(i.value.profileBytes, 123u);
    EXPECT_FALSE(i.value.animated);
    EXPECT_EQ(i.value.frames, 1u);
}

TEST(ExplodeBitmapHeader, T23AnimatedVersusStill)
{
    Fixture f;
    EXPECT_REFUSED(f.run(png(4, 4, {.actl = true, .frames = 3})), Status::incompatible, "multi-frame");
    EXPECT_FALSE(f.run(png(4, 4, {.actl = true, .frames = 0})).ok());
    EXPECT_TRUE(f.run(png(4, 4, {.actl = true, .frames = 1})).ok()); // single-frame APNG is a still
    EXPECT_REFUSED(f.run(gif(4, 4, 2)), Status::incompatible, "multi-frame");
    auto g = f.run(gif(4, 4, 1, true, true));
    ASSERT_TRUE(g.ok());
    EXPECT_TRUE(g.value.alpha);
    EXPECT_TRUE(g.value.interlaced);
    EXPECT_EQ(g.value.frames, 1u);
    EXPECT_EQ(g.value.color, ColorModel::Palette);
    Bytes c; wchunk(c, "VP8X", vp8x(0x02, 4, 4)); wchunk(c, "ANIM", Bytes(6, 0));
    Bytes fr(16, 0); fr.insert(fr.end(), 0, 0); wchunk(fr, "VP8L", vp8l(4, 4, false)); wchunk(c, "ANMF", fr); wchunk(c, "ANMF", fr);
    auto a = f.run(riff(c));
    EXPECT_REFUSED(a, Status::incompatible, "multi-frame");
    EXPECT_TRUE(a.value.animated);
    EXPECT_GE(a.value.frames, 2u);
    f.lim.requireStill = false; // caller that can accept animation still gets a classified header
    auto ok = f.run(png(4, 4, {.actl = true, .frames = 3}));
    EXPECT_TRUE(ok.ok());
    EXPECT_TRUE(ok.value.animated);
    EXPECT_EQ(ok.value.frames, 3u);
}

TEST(ExplodeBitmapHeader, T23JpegVariants)
{
    Fixture f;
    auto g = f.run(jpeg(9, 7));
    ASSERT_TRUE(g.ok());
    EXPECT_EQ(g.value.format, Format::JPEG);
    EXPECT_EQ(g.value.color, ColorModel::Gray);
    EXPECT_EQ(g.value.bitDepth, 8u);
    EXPECT_EQ(g.value.width, 9u);
    EXPECT_EQ(g.value.height, 7u);
    EXPECT_FALSE(g.value.interlaced);
    EXPECT_EQ(g.value.orientation, 0);
    EXPECT_EQ(f.run(jpeg(9, 7, {.comps = 3})).value.color, ColorModel::RGB);
    auto cmyk = f.run(jpeg(9, 7, {.comps = 4}));
    EXPECT_REFUSED(cmyk, Status::incompatible, "CMYK");
    EXPECT_TRUE(f.run(jpeg(9, 7, {.sof = 0xC2})).value.interlaced);
    for (std::uint32_t o = 1; o <= 8; ++o) EXPECT_EQ(f.run(jpeg(9, 7, {.orientation = o})).value.orientation, o);
    EXPECT_EQ(f.run(jpeg(9, 7, {.orientation = 9})).value.orientation, 0);
    auto p = f.run(jpeg(9, 7, {.icc = 500}));
    EXPECT_TRUE(p.value.hasProfile);
    EXPECT_EQ(p.value.profileBytes, 500u);
    auto lossless = f.run(jpeg(9, 7, {.sof = 0xC3}));
    EXPECT_FALSE(lossless.ok());
    EXPECT_EQ(lossless.value.format, Format::JPEG);
    EXPECT_FALSE(f.run(jpeg(9, 7, {.comps = 2})).ok());
}

TEST(ExplodeBitmapHeader, T23WebpVariants)
{
    Fixture f;
    auto l = f.run(webpL(11, 6, true));
    ASSERT_TRUE(l.ok());
    EXPECT_EQ(l.value.format, Format::WebP);
    EXPECT_TRUE(l.value.alpha);
    EXPECT_EQ(l.value.width, 11u);
    EXPECT_EQ(l.value.height, 6u);
    EXPECT_FALSE(f.run(webpL(11, 6, false)).value.alpha);
    auto y = f.run(webpLossy(12, 8));
    ASSERT_TRUE(y.ok());
    EXPECT_EQ(y.value.width, 12u);
    EXPECT_EQ(y.value.height, 8u);
    Bytes c; wchunk(c, "VP8X", vp8x(0x30, 11, 6)); wchunk(c, "ICCP", Bytes(40, 1)); wchunk(c, "ALPH", Bytes(4, 0)); wchunk(c, "VP8 ", vp8(11, 6));
    auto x = f.run(riff(c));
    ASSERT_TRUE(x.ok()) << x.outcome.diagnostic;
    EXPECT_TRUE(x.value.alpha);
    EXPECT_TRUE(x.value.hasProfile);
    EXPECT_EQ(x.value.profileBytes, 40u);
    Bytes m; wchunk(m, "VP8X", vp8x(0, 11, 6)); wchunk(m, "VP8L", vp8l(12, 6, false));
    EXPECT_REFUSED(f.run(riff(m)), Status::failed, "disagree");
    Bytes bad; wchunk(bad, "JUNK", Bytes(4, 0));
    EXPECT_FALSE(f.run(riff(bad)).ok());
}

TEST(ExplodeBitmapHeader, T23ClassifiedUnsupportedFormats)
{
    Fixture f;
    auto t = f.run(tiff(30, 20, 6));
    EXPECT_REFUSED(t, Status::incompatible, "not supported");
    EXPECT_EQ(t.value.format, Format::TIFF);
    EXPECT_EQ(t.value.width, 30u);
    EXPECT_EQ(t.value.height, 20u);
    EXPECT_EQ(t.value.orientation, 6);
    EXPECT_FALSE(t.value.supported);
    EXPECT_REFUSED(f.run(tiff(30, 20, 1, true)), Status::incompatible, "multi-frame");
    Bytes big; put(big, "II+"); big.push_back(0); big.resize(32, 0);
    auto bt = f.run(big);
    EXPECT_FALSE(bt.ok());
    EXPECT_EQ(bt.value.format, Format::TIFF);
    auto b = f.run(bmp(5, -4, 32));
    EXPECT_REFUSED(b, Status::incompatible, "not supported");
    EXPECT_EQ(b.value.format, Format::BMP);
    EXPECT_EQ(b.value.width, 5u);
    EXPECT_EQ(b.value.height, 4u);
    EXPECT_TRUE(b.value.alpha);
    EXPECT_FALSE(f.run(tiff(30, 20), "image/png").ok());
    EXPECT_TRUE(f.run(png(4, 4)).value.supported);
    EXPECT_TRUE(f.run(webpL(4, 4)).value.supported);
    EXPECT_TRUE(f.run(gif(4, 4)).value.supported);
    EXPECT_TRUE(f.run(jpeg(4, 4)).value.supported);
    EXPECT_EQ(f.budget.reserved(), 0u);
}

// ======================= round 2 =======================
namespace {
Bytes pngHead() { return Bytes{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'}; }
Bytes segment(std::uint8_t marker, Bytes const &payload)
{
    Bytes b{0xFF, marker};
    be16(b, std::uint32_t(payload.size() + 2));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}
Bytes withSegments(Bytes jpg, Bytes const &segs) { jpg.insert(jpg.begin() + 2, segs.begin(), segs.end()); return jpg; }
Bytes iccSeg(int seq, int count, std::size_t bytes)
{
    Bytes p; put(p, std::string_view("ICC_PROFILE\0", 12)); p.push_back(std::uint8_t(seq)); p.push_back(std::uint8_t(count)); p.resize(p.size() + bytes, 9);
    return segment(0xE2, p);
}
Bytes exifSeg(Bytes const &tiffBlob) { Bytes p; put(p, std::string_view("Exif\0\0", 6)); p.insert(p.end(), tiffBlob.begin(), tiffBlob.end()); return segment(0xE1, p); }
Bytes exifLE(std::uint32_t orientation)
{
    Bytes t; put(t, "II"); le16(t, 42); le32(t, 8); le16(t, 1); le16(t, 0x0112); le16(t, 3); le32(t, 1); le16(t, orientation); le16(t, 0); le32(t, 0);
    return t;
}
Bytes adobeSeg(int transform) { Bytes p; put(p, "Adobe"); p.insert(p.end(), {0, 100, 0, 0, 0, 0, std::uint8_t(transform)}); return segment(0xEE, p); }
Bytes animWebp()
{
    Bytes c; wchunk(c, "VP8X", vp8x(0x32, 4, 4)); wchunk(c, "ICCP", Bytes(8, 1)); wchunk(c, "ANIM", Bytes(6, 0));
    Bytes fr(16, 0); wchunk(fr, "VP8L", vp8l(4, 4, false)); wchunk(c, "ANMF", fr); wchunk(c, "ANMF", fr);
    return riff(c);
}
std::string b64(Bytes const &b)
{
    static char const *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (std::size_t i = 0; i < b.size(); i += 3) {
        std::uint32_t v = b[i] << 16 | (i + 1 < b.size() ? b[i + 1] << 8 : 0) | (i + 2 < b.size() ? b[i + 2] : 0);
        o += a[v >> 18]; o += a[(v >> 12) & 63]; o += i + 1 < b.size() ? a[(v >> 6) & 63] : '='; o += i + 2 < b.size() ? a[v & 63] : '=';
    }
    return o;
}
Bytes bigPng(std::size_t idatBytes)
{
    Bytes b = pngHead();
    chunk(b, "IHDR", ihdr(40, 30)); chunk(b, "IDAT", Bytes(idatBytes, 7)); chunk(b, "IEND", {});
    return b;
}
} // namespace

TEST(ExplodeBitmapHeader, R2PngChecksumWorkIsBoundedAndOrdered)
{
    Fixture f;
    // Order: scan-limit and profile-length refusals come before any CRC; a bad CRC would be Status::failed.
    Bytes t = pngHead(); chunk(t, "IHDR", ihdr(4, 4));
    chunk(t, "tEXt", Bytes(6 * MiB, 'a'), true);
    chunk(t, "IDAT", {1}); chunk(t, "IEND", {});
    EXPECT_REFUSED(f.run(t), Status::incompatible, "scan limit");
    // After IDAT, trailing chunks are not checksummed at all (bad CRC tolerated; decode owns it).
    Bytes a = png(4, 4); a.resize(a.size() - 12);
    chunk(a, "tEXt", Bytes(20, 'x'), true); chunk(a, "IEND", {});
    EXPECT_TRUE(f.run(a).ok());
    // Stop flipped while a large chunk is being checksummed is honoured (profile limit raised so the CRC runs).
    Fixture g;
    g.lim.maxProfileBytes = 40 * MiB;
    Bytes big = pngHead(); chunk(big, "IHDR", ihdr(4, 4)); chunk(big, "iCCP", Bytes(32 * MiB, 1));
    chunk(big, "IDAT", {1}); chunk(big, "IEND", {});
    EXPECT_TRUE(g.run(big).ok());
    auto flag = std::make_shared<std::atomic<bool>>(false);
    std::thread flipper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); flag->store(true); });
    auto r = inspect({big.data(), big.size(), {}}, g.lim, g.budget, Stop(flag));
    flipper.join();
    EXPECT_EQ(r.outcome.status, Status::canceled);
    EXPECT_EQ(g.budget.reserved(), 0u);
}

TEST(ExplodeBitmapHeader, R2CompressedSizeCeilingBeforeScratch)
{
    Budget none(Budget::FixedLimitForTest{}, 0); // any reservation attempt would fail with Status::failed, not incompatible
    HeaderLimits lim;
    Bytes p = png(4, 4);
    lim.maxEncodedBytes = p.size() - 1;
    EXPECT_REFUSED(inspect({p.data(), p.size(), {}}, lim, none), Status::incompatible, "compressed size");
    lim.maxEncodedBytes = p.size();
    EXPECT_EQ(inspect({p.data(), p.size(), {}}, lim, none).outcome.status, Status::failed); // now reaches the budget
}

TEST(ExplodeBitmapHeader, R2PrefixModeStopsAtFirstImageData)
{
    Fixture f;
    f.lim.prefixOnly = true;
    Bytes big = bigPng(3 * MiB);
    auto r = inspect({big.data(), 8 + 25 + 8, {}}, f.lim, f.budget); // IHDR + IDAT chunk header only
    ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    EXPECT_EQ(r.value.width, 40u);
    f.lim.prefixOnly = false;
    EXPECT_FALSE(inspect({big.data(), 8 + 25 + 8, {}}, f.lim, f.budget).ok()); // full mode needs the whole file
    f.lim.prefixOnly = true;
    EXPECT_FALSE(inspect({big.data(), 20, {}}, f.lim, f.budget).ok()); // header itself truncated
    Bytes j = jpeg(40, 30);
    EXPECT_TRUE(inspect({j.data(), j.size() - 5, {}}, f.lim, f.budget).ok()); // SOS found, EOI not required
    Bytes w = webpLossy(40, 30); w.resize(w.size() + 5000, 0); // riff size lies beyond the prefix
    for (int i = 0; i < 4; ++i) w[4 + i] = std::uint8_t((w.size() - 8 + 100000) >> (8 * i)) & 0xFF;
    EXPECT_EQ(inspect({w.data(), 40, {}}, f.lim, f.budget).value.height, 30u);
    Bytes g = gif(40, 30);
    EXPECT_TRUE(inspect({g.data(), 13 + 6 + 11, {}}, f.lim, f.budget).ok());
    EXPECT_EQ(f.budget.reserved(), 0u);
}

TEST(ExplodeBitmapHeader, R2InspectHrefDecodesOnlyTheScratchWindow)
{
    HeaderLimits lim;
    Bytes big = bigPng(3 * MiB);
    std::string uri = "data:image/png;base64," + b64(big);
    Budget budget(Budget::FixedLimitForTest{}, MiB + 4096); // a full 3 MiB decode could not be reserved; the 1 MiB window can
    auto r = inspectHref(uri, lim, budget);
    ASSERT_TRUE(r.ok()) << r.outcome.diagnostic;
    EXPECT_EQ(r.value.width, 40u);
    EXPECT_EQ(budget.reserved(), 0u);
    // MIME in the URI is checked against the decoded signature.
    EXPECT_REFUSED(inspectHref("data:image/jpeg;base64," + b64(big), lim, budget), Status::incompatible, "disagree");
    // A small payload gets the full checks (truncated PNG refused), a hostile prefix is refused.
    Bytes small = png(4, 4);
    EXPECT_TRUE(inspectHref("data:image/png;base64," + b64(small), lim, budget).ok());
    Bytes cut(small.begin(), small.end() - 5);
    EXPECT_FALSE(inspectHref("data:image/png;base64," + b64(cut), lim, budget).ok());
    Bytes huge = pngHead(); chunk(huge, "IHDR", ihdr(65535, 65535)); huge.resize(2 * MiB, 0);
    EXPECT_REFUSED(inspectHref("data:image/png;base64," + b64(huge), lim, budget), Status::incompatible, "axis");
    EXPECT_FALSE(inspectHref("a.png", lim, budget).ok());
    Budget tiny(Budget::FixedLimitForTest{}, 1000);
    EXPECT_FALSE(inspectHref(uri, lim, tiny).ok());
    EXPECT_EQ(tiny.reserved(), 0u);
    // Case-insensitive base64 parameter; distinct limit diagnostics.
    EXPECT_TRUE(inspectUri("data:image/png;BASE64,QUJD", lim).ok());
    lim.maxEncodedBytes = 2;
    EXPECT_REFUSED(inspectUri("data:image/png;base64,QUJD", lim), Status::incompatible, "encoded size");
    lim.maxUriBytes = 10;
    EXPECT_REFUSED(inspectUri("data:image/png;base64,QUJD", lim), Status::incompatible, "reference exceeds");
}

TEST(ExplodeBitmapHeader, R2JpegRefusals)
{
    Fixture f;
    EXPECT_REFUSED(f.run(jpeg(4, 4, {.comps = 4})), Status::incompatible, "CMYK");
    EXPECT_REFUSED(f.run(jpeg(4, 4, {.sof = 0xC2, .comps = 4})), Status::incompatible, "CMYK");
    EXPECT_REFUSED(f.run(withSegments(jpeg(4, 4, {.comps = 3}), adobeSeg(2))), Status::incompatible, "CMYK");
    EXPECT_TRUE(f.run(withSegments(jpeg(4, 4, {.comps = 3}), adobeSeg(1))).ok());
    EXPECT_TRUE(f.run(withSegments(jpeg(4, 4, {.comps = 3}), adobeSeg(0))).ok());
    for (int prec : {0, 12, 16}) {
        for (int sof : {0xC0, 0xC1, 0xC2}) EXPECT_REFUSED(f.run(jpeg(4, 4, {.sof = sof, .precision = prec})), Status::incompatible, "8-bit");
    }
}

TEST(ExplodeBitmapHeader, R2JpegIccSequences)
{
    Fixture f;
    auto two = f.run(withSegments(jpeg(4, 4), iccSeg(1, 2, 100)));
    EXPECT_FALSE(two.ok()); // chunk 2 of 2 missing
    Bytes both = iccSeg(1, 2, 100), second = iccSeg(2, 2, 50);
    both.insert(both.end(), second.begin(), second.end());
    auto ok = f.run(withSegments(jpeg(4, 4), both));
    ASSERT_TRUE(ok.ok()) << ok.outcome.diagnostic;
    EXPECT_EQ(ok.value.profileBytes, 150u);
    Bytes rev = iccSeg(2, 2, 50), first = iccSeg(1, 2, 100); rev.insert(rev.end(), first.begin(), first.end());
    EXPECT_EQ(f.run(withSegments(jpeg(4, 4), rev)).value.profileBytes, 150u); // order does not matter
    Bytes dup = iccSeg(1, 2, 10), dup2 = iccSeg(1, 2, 10), d3 = iccSeg(2, 2, 10);
    dup.insert(dup.end(), dup2.begin(), dup2.end()); dup.insert(dup.end(), d3.begin(), d3.end());
    EXPECT_REFUSED(f.run(withSegments(jpeg(4, 4), dup)), Status::failed, "inconsistent");
    EXPECT_REFUSED(f.run(withSegments(jpeg(4, 4), iccSeg(3, 2, 10))), Status::failed, "inconsistent");
    EXPECT_REFUSED(f.run(withSegments(jpeg(4, 4), iccSeg(0, 1, 10))), Status::failed, "inconsistent");
    EXPECT_REFUSED(f.run(withSegments(jpeg(4, 4), iccSeg(1, 0, 10))), Status::failed, "inconsistent");
    Bytes mism = iccSeg(1, 2, 10), m2 = iccSeg(2, 3, 10); mism.insert(mism.end(), m2.begin(), m2.end());
    EXPECT_REFUSED(f.run(withSegments(jpeg(4, 4), mism)), Status::failed, "inconsistent");
    f.lim.maxProfileBytes = 120; // sum across chunks is bounded
    EXPECT_REFUSED(f.run(withSegments(jpeg(4, 4), both)), Status::incompatible, "profile");
}

TEST(ExplodeBitmapHeader, R2JpegExif)
{
    Fixture f;
    for (std::uint32_t o = 1; o <= 8; ++o) EXPECT_EQ(f.run(withSegments(jpeg(4, 4), exifSeg(exifLE(o)))).value.orientation, o);
    Bytes two = exifSeg(exifLE(3)), other = exifSeg(exifLE(6));
    two.insert(two.end(), other.begin(), other.end());
    EXPECT_EQ(f.run(withSegments(jpeg(4, 4), two)).value.orientation, 3); // first Exif wins
    // Hostile blobs: still parse the image, never read out of range, orientation unknown.
    Bytes hostile[5];
    { Bytes t; put(t, "II"); le16(t, 42); le32(t, 0xFFFFFFF0u); hostile[0] = t; }
    { Bytes t; put(t, "II"); le16(t, 42); le32(t, 8); le16(t, 0xFFFF); hostile[1] = t; }
    { Bytes t; put(t, "MM"); be16(t, 42); be32(t, 8); be16(t, 2); hostile[2] = t; }
    { Bytes t; put(t, "II"); le16(t, 43); le32(t, 8); hostile[3] = t; }
    { Bytes t; put(t, "II"); le16(t, 42); hostile[4] = t; }
    for (auto const &h : hostile) {
        auto r = f.run(withSegments(jpeg(4, 4), exifSeg(h)));
        EXPECT_TRUE(r.ok());
        EXPECT_EQ(r.value.orientation, 0);
    }
}

TEST(ExplodeBitmapHeader, R2PngAndGifAndWebpStrictness)
{
    Fixture f;
    Bytes d = pngHead(); chunk(d, "IHDR", ihdr(4, 4));
    Bytes a1 = d, a2;
    { Bytes ac; be32(ac, 5); be32(ac, 0); chunk(a1, "acTL", ac); Bytes ac1; be32(ac1, 1); be32(ac1, 0); chunk(a1, "acTL", ac1); chunk(a1, "IDAT", {1}); chunk(a1, "IEND", {}); }
    EXPECT_REFUSED(f.run(a1), Status::failed, "duplicate animation");
    Bytes pl = d; chunk(pl, "PLTE", {1, 2}); chunk(pl, "IDAT", {1}); chunk(pl, "IEND", {});
    EXPECT_REFUSED(f.run(pl), Status::failed, "palette"); // not a multiple of 3
    Bytes pg = pngHead(); chunk(pg, "IHDR", ihdr(4, 4, 8, 4)); chunk(pg, "PLTE", {1, 2, 3}); chunk(pg, "IDAT", {1}); chunk(pg, "IEND", {});
    EXPECT_REFUSED(f.run(pg), Status::failed, "palette"); // PLTE forbidden for grey+alpha
    Bytes uc = d; chunk(uc, "ABCD", {1}); chunk(uc, "IDAT", {1}); chunk(uc, "IEND", {});
    EXPECT_REFUSED(f.run(uc), Status::incompatible, "critical");
    Bytes anc = d; chunk(anc, "abCD", {1}); chunk(anc, "IDAT", {1}); chunk(anc, "IEND", {});
    EXPECT_TRUE(f.run(anc).ok());
    // GIF frame larger than the logical screen / limits / zero.
    Bytes g = gif(1, 1); // frame is 1x1; patch its width to 65535
    std::size_t desc = 6 + 4 + 3 + 6 + 3; // after header, LSD, 2-entry colour table... find 0x2C
    for (std::size_t i = 0; i < g.size(); ++i) if (g[i] == 0x2C) { desc = i; break; }
    Bytes big = g; big[desc + 5] = 0xFF; big[desc + 6] = 0xFF;
    EXPECT_REFUSED(f.run(big), Status::incompatible, "axis");
    Bytes out = g; out[desc + 5] = 2; out[desc + 6] = 0; // 2 > screen 1
    EXPECT_REFUSED(f.run(out), Status::incompatible, "outside");
    Bytes shifted = g; shifted[desc + 1] = 1; // left offset 1 + width 1 > 1
    EXPECT_REFUSED(f.run(shifted), Status::incompatible, "outside");
    Bytes zero = g; zero[desc + 5] = 0;
    EXPECT_REFUSED(f.run(zero), Status::incompatible, "zero-size");
    // WebP: VP8L version bits, VP8X ICC flag without ICCP.
    Bytes v; { Bytes d2 = vp8l(4, 4, false); d2[4] |= 0x20; wchunk(v, "VP8L", d2); }
    EXPECT_REFUSED(f.run(riff(v)), Status::failed, "version");
    Bytes x; wchunk(x, "VP8X", vp8x(0x20, 4, 4)); wchunk(x, "VP8L", vp8l(4, 4, false));
    EXPECT_REFUSED(f.run(riff(x)), Status::failed, "declares");
}

TEST(ExplodeBitmapHeader, R2TruncationAtEveryOffsetRichFixtures)
{
    Fixture f;
    f.lim.requireStill = false; // so that refusal can only come from the truncation itself
    Bytes pngRich = pngHead();
    chunk(pngRich, "IHDR", ihdr(3, 2, 8, 6)); chunk(pngRich, "sRGB", {0}); chunk(pngRich, "iCCP", Bytes(40, 1));
    { Bytes ac; be32(ac, 2); be32(ac, 0); chunk(pngRich, "acTL", ac); }
    chunk(pngRich, "IDAT", Bytes{1, 2, 3}); chunk(pngRich, "IEND", {});
    Bytes icc2 = iccSeg(1, 2, 20), icc2b = iccSeg(2, 2, 20); icc2.insert(icc2.end(), icc2b.begin(), icc2b.end());
    Bytes jpgRich = withSegments(jpeg(3, 2, {.comps = 3}), exifSeg(exifLE(6)));
    jpgRich = withSegments(jpgRich, icc2);
    Bytes webpIcc; wchunk(webpIcc, "VP8X", vp8x(0x30, 3, 2)); wchunk(webpIcc, "ICCP", Bytes(9, 1)); wchunk(webpIcc, "ALPH", Bytes(3, 0)); wchunk(webpIcc, "VP8 ", vp8(3, 2));
    std::vector<std::pair<char const *, Bytes>> files = {{"png-rich", pngRich}, {"jpeg-exif-icc", jpgRich}, {"webp-vp8x-iccp", riff(webpIcc)}, {"webp-anim", animWebp()}, {"gif-gce", gif(3, 2, 2, true, true)}};
    for (auto &[name, full] : files) {
        auto ok = f.run(full);
        ASSERT_TRUE(ok.ok()) << name << ": " << ok.outcome.diagnostic;
        for (std::size_t n = 0; n < full.size(); ++n) {
            Bytes cut(full.begin(), full.begin() + n);
            EXPECT_FALSE(inspect({cut.data(), cut.size(), {}}, f.lim, f.budget).ok()) << name << " at " << n;
        }
        EXPECT_EQ(f.budget.reserved(), 0u) << name;
    }
}
