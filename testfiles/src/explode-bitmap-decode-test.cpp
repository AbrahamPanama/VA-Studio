// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Tests for the Explode Bitmap engine (Decode): T07 codec/allocation faults, T12 stop/refusal,
 * T17 truncated/corrupt/mismatched input, T23 colour and source variants. Encoded inputs are built here.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <lcms2.h>
#include <zlib.h>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#ifdef HAVE_JPEG
extern "C" {
#include <jpeglib.h>
}
#endif
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "util/bitmap-input-decode.h"

using namespace Inkscape::Bitmap;
using Bytes = std::vector<std::uint8_t>;
using Px = std::array<int, 4>;

namespace {

void put(Bytes &b, std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }
void be32(Bytes &b, std::uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(std::uint8_t(v >> s)); }
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
void chunk(Bytes &b, char const *type, Bytes const &data)
{
    be32(b, std::uint32_t(data.size()));
    std::size_t s = b.size();
    put(b, std::string_view(type, 4));
    b.insert(b.end(), data.begin(), data.end());
    be32(b, crc(b, s, b.size()));
}
// zlib stream made of stored deflate blocks (no compressor needed)
Bytes zlibStored(Bytes const &raw)
{
    Bytes z{0x78, 0x01};
    std::size_t pos = 0;
    do {
        std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    std::uint32_t a = 1, b2 = 0;
    for (auto v : raw) { a = (a + v) % 65521; b2 = (b2 + a) % 65521; }
    be32(z, (b2 << 16) | a);
    return z;
}
struct PngSpec {
    std::uint32_t w = 1, h = 1;
    int depth = 8, ct = 6;
    Bytes rows;      // unfiltered scanlines, filter byte added here
    Bytes plte, trns, iccp;
    bool corruptZlib = false;
    int interlace = 0;
    Bytes raw;       // complete filtered stream instead of `rows` (Adam7 tests)
    std::vector<std::size_t> split; // IDAT boundaries inside the zlib stream
    bool emptyIdat = false;         // zero-length IDAT after the first piece
    std::vector<std::pair<std::string, Bytes>> after; // extra chunks between IDAT and IEND
};
Bytes png(PngSpec const &s)
{
    Bytes b{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Bytes ih;
    be32(ih, s.w); be32(ih, s.h);
    ih.insert(ih.end(), {std::uint8_t(s.depth), std::uint8_t(s.ct), 0, 0, std::uint8_t(s.interlace)});
    chunk(b, "IHDR", ih);
    if (!s.iccp.empty()) {
        Bytes p{'i', 'c', 'c', 0, 0};
        Bytes z = zlibStored(s.iccp);
        p.insert(p.end(), z.begin(), z.end());
        chunk(b, "iCCP", p);
    }
    if (!s.plte.empty()) chunk(b, "PLTE", s.plte);
    if (!s.trns.empty()) chunk(b, "tRNS", s.trns);
    std::size_t rowLen = s.rows.size() / s.h;
    Bytes raw = s.raw;
    for (std::uint32_t y = 0; y < s.h && s.raw.empty(); ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), s.rows.begin() + y * rowLen, s.rows.begin() + (y + 1) * rowLen);
    }
    Bytes z = zlibStored(raw);
    if (s.corruptZlib) { z[2] = 0x07; for (std::size_t i = 3; i + 4 < z.size(); ++i) z[i] ^= 0xA5; } // invalid block type, CRC still valid
    std::size_t from = 0;
    for (std::size_t cut : s.split) {
        chunk(b, "IDAT", Bytes(z.begin() + from, z.begin() + cut));
        if (s.emptyIdat && from == 0) chunk(b, "IDAT", {});
        from = cut;
    }
    chunk(b, "IDAT", Bytes(z.begin() + from, z.end()));
    if (s.emptyIdat && s.split.empty()) chunk(b, "IDAT", {});
    for (auto const &[t, d] : s.after) chunk(b, t.c_str(), d);
    chunk(b, "IEND", {});
    return b;
}
Bytes rgba(std::uint32_t w, std::uint32_t h, std::vector<Px> const &px)
{
    PngSpec s; s.w = w; s.h = h;
    for (auto &p : px) for (int c : p) s.rows.push_back(std::uint8_t(c));
    return png(s);
}
Bytes profileBytes(cmsHPROFILE p)
{
    cmsUInt32Number n = 0;
    cmsSaveProfileToMem(p, nullptr, &n);
    Bytes b(n);
    cmsSaveProfileToMem(p, b.data(), &n);
    cmsCloseProfile(p);
    return b;
}
Bytes linearRgbProfile()
{
    cmsCIExyY d65{0.3127, 0.3290, 1.0};
    cmsCIExyYTRIPLE prim{{0.64, 0.33, 1.0}, {0.30, 0.60, 1.0}, {0.15, 0.06, 1.0}};
    cmsToneCurve *lin = cmsBuildGamma(nullptr, 1.0);
    cmsToneCurve *curves[3] = {lin, lin, lin};
    cmsHPROFILE p = cmsCreateRGBProfile(&d65, &prim, curves);
    cmsFreeToneCurve(lin);
    return profileBytes(p);
}
Bytes linearGrayProfile()
{
    cmsCIExyY d65{0.3127, 0.3290, 1.0};
    cmsToneCurve *lin = cmsBuildGamma(nullptr, 1.0);
    cmsHPROFILE p = cmsCreateGrayProfile(&d65, lin);
    cmsFreeToneCurve(lin);
    return profileBytes(p);
}
Bytes gdkSave(GdkPixbuf *pb, char const *type)
{
    gchar *buf = nullptr; gsize n = 0;
    EXPECT_TRUE(gdk_pixbuf_save_to_buffer(pb, &buf, &n, type, nullptr, nullptr));
    Bytes b(buf, buf + n);
    g_free(buf);
    return b;
}
// JPEG of four flat quadrants A B / C D (32 x 16), with an EXIF orientation segment right after SOI.
Bytes quadJpeg(int orientation)
{
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 32, 16);
    guchar const col[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
    guchar *p = gdk_pixbuf_get_pixels(pb);
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 32; ++x) std::memcpy(p + y * gdk_pixbuf_get_rowstride(pb) + x * 3, col[(y / 8) * 2 + x / 16], 3);
    Bytes j = gdkSave(pb, "jpeg");
    g_object_unref(pb);
    if (orientation > 1) {
        Bytes app{'E', 'x', 'i', 'f', 0, 0, 'M', 'M', 0, '*', 0, 0, 0, 8, 0, 1, 0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, std::uint8_t(orientation), 0, 0, 0, 0, 0, 0};
        Bytes seg{0xFF, 0xE1};
        be16(seg, std::uint32_t(app.size() + 2));
        seg.insert(seg.end(), app.begin(), app.end());
        j.insert(j.begin() + 2, seg.begin(), seg.end());
    }
    return j;
}

constexpr std::uint64_t kBig = 8192 * MiB;
struct Run { std::unique_ptr<Budget> budget; Result<DecodedRaster> r; }; // budget outlives every token
Run run(Bytes const &b, char const *mime = "", Stop s = {}, AllocationFault *f = nullptr, std::uint64_t limit = kBig, std::uint32_t ew = 0, std::uint32_t eh = 0,
        void (*progress)(void *, std::uint64_t, std::uint64_t, unsigned) = nullptr, void *user = nullptr)
{
    Run o;
    o.budget = std::make_unique<Budget>(Budget::FixedLimitForTest{}, limit);
    ValidatedInput in;
    in.encoded = {b.data(), b.size(), mime};
    in.expectWidth = ew; in.expectHeight = eh;
    in.progress = progress; in.user = user;
    o.r = decode(in, *o.budget, s, f);
    return o;
}
Px at(DecodedRaster const &d, std::uint32_t x, std::uint32_t y)
{
    auto const *p = reinterpret_cast<std::uint8_t const *>(d.pixels.data()) + (std::uint64_t(y) * d.width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}
void expectRefused(Run const &o)
{
    EXPECT_FALSE(o.r.ok());
    EXPECT_EQ(o.r.value.pixels.size(), 0u);
    EXPECT_EQ(o.r.value.width, 0u);
    EXPECT_EQ(o.budget->reserved(), 0u);
}
void expectNear(Px a, Px b, int tol)
{
    for (int i = 0; i < 4; ++i) EXPECT_LE(std::abs(a[i] - b[i]), tol) << "channel " << i;
}
// The platform loader's own decode, to check that Explode adds no error on top of it.
GdkPixbuf *loaderDecode(Bytes const &b, char const *mime)
{
    GdkPixbufLoader *l = gdk_pixbuf_loader_new_with_mime_type(mime, nullptr);
    if (!l) return nullptr;
    gdk_pixbuf_loader_write(l, b.data(), b.size(), nullptr);
    gdk_pixbuf_loader_close(l, nullptr);
    GdkPixbuf *p = gdk_pixbuf_loader_get_pixbuf(l);
    if (p) g_object_ref(p);
    g_object_unref(l);
    return p;
}
Px pixbufPx(GdkPixbuf *p, int x, int y)
{
    int const n = gdk_pixbuf_get_n_channels(p);
    auto const *q = gdk_pixbuf_read_pixels(p) + y * gdk_pixbuf_get_rowstride(p) + x * n;
    return {q[0], q[1], q[2], n == 4 ? q[3] : 255};
}
bool webpLoader()
{
    GError *e = nullptr;
    GdkPixbufLoader *l = gdk_pixbuf_loader_new_with_mime_type("image/webp", &e);
    if (e) g_error_free(e);
    if (l) { gdk_pixbuf_loader_close(l, nullptr); g_object_unref(l); }
    return l != nullptr;
}
// 4x4 WebP fixtures (cwebp): lossless -exact and lossy q90 alpha_q100; pixel (x,y)=(60x,60y,100+10x,alpha[(x+y)%4])
std::uint8_t const kWebpLossless[] = {0x52, 0x49, 0x46, 0x46, 0x4a, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4c, 0x3d, 0x00, 0x00, 0x00, 0x2f, 0x03, 0xc0, 0x00, 0x10, 0x7f, 0x20, 0x10, 0x48, 0x06, 0x0b, 0x7f, 0xc8, 0xe1, 0x11, 0x0b, 0xa6, 0x78, 0x77, 0x9c, 0x19, 0x48, 0xdb, 0xe6, 0x13, 0x19, 0x95, 0x15, 0xf9, 0xc8, 0x07, 0x00, 0x6d, 0xf3, 0xd3, 0xd4, 0x00, 0x06, 0xcd, 0x34, 0xcd, 0x83, 0x20, 0x00, 0x22, 0xce, 0x81, 0x03, 0x07, 0x3e, 0x1d, 0x38, 0x70, 0x20, 0x25, 0xa2, 0xff, 0xe1, 0x6b, 0x2c, 0x02, 0x00};
std::uint8_t const kWebpLossy[] = {0x52, 0x49, 0x46, 0x46, 0x9c, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x58, 0x0a, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x41, 0x4c, 0x50, 0x48, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0xaa, 0xff, 0x55, 0xaa, 0xff, 0x00, 0xaa, 0xff, 0x00, 0x55, 0xff, 0x00, 0x55, 0xaa, 0x00, 0x56, 0x50, 0x38, 0x20, 0x64, 0x00, 0x00, 0x00, 0x10, 0x03, 0x00, 0x9d, 0x01, 0x2a, 0x04, 0x00, 0x04, 0x00, 0x00, 0xc0, 0x12, 0x25, 0x90, 0x02, 0x74, 0xba, 0x00, 0xd4, 0x00, 0xfe, 0x01, 0x7e, 0x01, 0xe0, 0x00, 0x0d, 0xcf, 0xd3, 0x8b, 0xbe, 0xc0, 0x00, 0xfe, 0xf9, 0x67, 0xb2, 0xfc, 0xb7, 0x11, 0x94, 0x7d, 0xfa, 0x12, 0xff, 0x6d, 0x1f, 0x90, 0x2c, 0x29, 0x8e, 0xf9, 0x6c, 0x99, 0xdd, 0xff, 0xa3, 0xe8, 0x76, 0xfe, 0xe3, 0x72, 0xd6, 0x1d, 0x77, 0xeb, 0xf1, 0x0d, 0xea, 0x81, 0xff, 0xfd, 0xaf, 0xf6, 0x9d, 0x4f, 0xf5, 0x73, 0xba, 0xed, 0x2c, 0xeb, 0x4f, 0xf7, 0x1b, 0x96, 0xb0, 0xeb, 0xbf, 0xff, 0xd8, 0xcb, 0x7e, 0xae, 0x77, 0x2b, 0x80, 0x00, 0x00};

} // namespace

TEST(ExplodeBitmapDecode, StraightAlphaSurvivesAndOriginalIsRecoverable) // T23, EB2 recovery
{
    Bytes b = rgba(2, 1, {{200, 100, 50, 3}, {10, 20, 30, 255}});
    auto o = run(b, "image/png");
    ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic;
    auto const &d = o.r.value;
    EXPECT_EQ(at(d, 0, 0), (Px{200, 100, 50, 3})); // no premultiply round trip at low alpha
    EXPECT_EQ(at(d, 1, 0), (Px{10, 20, 30, 255}));
    EXPECT_EQ(d.original.data, b.data());
    EXPECT_EQ(d.originalBytes, b.size());
    EXPECT_EQ(d.originalHash, fingerprint({b.data(), b.size(), ""}));
    EXPECT_EQ(d.view().validate().status, Status::unchanged);
    EXPECT_GT(o.budget->reserved(Stage::canonical), 0u);
    EXPECT_EQ(o.budget->reserved(Stage::decode), 0u); // codec/scratch reservations are returned
}

TEST(ExplodeBitmapDecode, GreyGreyAlphaPaletteTrns16Bit) // T23
{
    { PngSpec s; s.w = 2; s.ct = 0; s.rows = {0, 128};
      auto o = run(png(s)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic;
      EXPECT_EQ(at(o.r.value, 0, 0), (Px{0, 0, 0, 255})); EXPECT_EQ(at(o.r.value, 1, 0), (Px{128, 128, 128, 255})); }
    { PngSpec s; s.ct = 4; s.rows = {100, 40};
      auto o = run(png(s)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic;
      EXPECT_EQ(at(o.r.value, 0, 0), (Px{100, 100, 100, 40})); EXPECT_TRUE(o.r.value.hadAlpha); }
    { PngSpec s; s.w = 3; s.ct = 3; s.depth = 8; s.plte = {255, 0, 0, 0, 255, 0, 0, 0, 255}; s.trns = {0, 128}; s.rows = {0, 1, 2};
      auto o = run(png(s)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic;
      EXPECT_EQ(at(o.r.value, 0, 0), (Px{255, 0, 0, 0})); EXPECT_EQ(at(o.r.value, 1, 0), (Px{0, 255, 0, 128}));
      EXPECT_EQ(at(o.r.value, 2, 0), (Px{0, 0, 255, 255})); EXPECT_TRUE(o.r.value.hadAlpha); }
    { PngSpec s; s.depth = 16; s.ct = 6; s.rows = {0x12, 0x34, 0xFE, 0xDC, 0x00, 0xFF, 0xFF, 0xFF};
      auto o = run(png(s)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic;
      EXPECT_EQ(at(o.r.value, 0, 0), (Px{0x12, 0xFE, 0x00, 0xFF})); // libpng high-byte reduction, as the display path
      EXPECT_TRUE(o.r.value.reduced16); }
}

TEST(ExplodeBitmapDecode, IccProfileCarriedNotApplied) // T23, M4/m3
{
    Bytes lin = linearRgbProfile();
    auto same = [](DecodedRaster const &d, Bytes const &prof) {
        return d.profileBytes == prof.size() && std::memcmp(d.profile.data(), prof.data(), prof.size()) == 0;
    };
    { PngSpec s; s.ct = 6; s.rows = {128, 128, 128, 77}; s.iccp = lin;
      auto o = run(png(s)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic;
      EXPECT_EQ(at(o.r.value, 0, 0), (Px{128, 128, 128, 77})); // raw values, as the canvas shows them
      EXPECT_TRUE(same(o.r.value, lin)); }
    { Bytes g = linearGrayProfile(); PngSpec s; s.ct = 0; s.rows = {128}; s.iccp = g;
      auto o = run(png(s)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic;
      EXPECT_EQ(at(o.r.value, 0, 0), (Px{128, 128, 128, 255})); EXPECT_TRUE(same(o.r.value, g)); }
    { PngSpec s; s.ct = 2; s.rows = {128, 128, 128};
      auto o = run(png(s)); ASSERT_TRUE(o.r.ok());
      EXPECT_EQ(o.r.value.profileBytes, 0u); EXPECT_EQ(o.r.value.profile.size(), 0u); }
    { PngSpec s; s.ct = 2; s.rows = {128, 128, 128}; s.iccp = linearGrayProfile(); // grey profile on RGB samples
      expectRefused(run(png(s))); }
    { PngSpec s; s.ct = 0; s.rows = {128}; s.iccp = lin; // RGB profile on grey samples
      expectRefused(run(png(s))); }
    { PngSpec s; s.ct = 2; s.rows = {128, 128, 128}; s.iccp = Bytes(200, 0x5A); expectRefused(run(png(s))); }
    { PngSpec s; s.ct = 2; s.rows = {128, 128, 128}; s.iccp = profileBytes(cmsCreateLab4Profile(nullptr));
      auto o = run(png(s)); expectRefused(o); EXPECT_EQ(o.r.outcome.status, Status::incompatible); }
}

TEST(ExplodeBitmapDecode, CmykJpegRefusedWithPlanMessage) // T23, m5
{
    Bytes j{0xFF, 0xD8, 0xFF, 0xC0, 0, 20, 8, 0, 8, 0, 8, 4, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0, 4, 0x11, 0,
            0xFF, 0xDA, 0, 8, 1, 1, 0, 0, 0x3F, 0, 0, 0xFF, 0xD9};
    auto o = run(j, "image/jpeg");
    expectRefused(o);
    EXPECT_STREQ(o.r.outcome.diagnostic, "Convert this CMYK image to RGB and embed it before adjusting or exploding it.");
    Bytes ycck{0xFF, 0xD8, 0xFF, 0xEE, 0, 14, 'A', 'd', 'o', 'b', 'e', 0, 100, 0, 0, 0, 0, 2,
               0xFF, 0xC0, 0, 17, 8, 0, 8, 0, 8, 3, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0,
               0xFF, 0xDA, 0, 8, 1, 1, 0, 0, 0x3F, 0, 0, 0xFF, 0xD9};
    auto a = run(ycck, "image/jpeg"); // Adobe YCCK is CMYK too
    expectRefused(a);
    EXPECT_STREQ(a.r.outcome.diagnostic, o.r.outcome.diagnostic);
}

TEST(ExplodeBitmapDecode, GifStill) // T23
{
    Bytes transparent{0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 1, 0, 1, 0, 0x80, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x21, 0xF9, 4, 1, 0, 0, 0, 0,
                      0x2C, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0, 0x3B};
    Bytes white = transparent;
    white.erase(white.begin() + 19, white.begin() + 27); // drop the graphic control block
    white[white.size() - 4] = 0x4C; // LZW: pixel index 1
    auto a = run(transparent, "image/gif"); ASSERT_TRUE(a.r.ok()) << a.r.outcome.diagnostic;
    EXPECT_EQ(at(a.r.value, 0, 0)[3], 0);
    auto b = run(white, "image/gif"); ASSERT_TRUE(b.r.ok()) << b.r.outcome.diagnostic;
    EXPECT_EQ(at(b.r.value, 0, 0), (Px{255, 255, 255, 255}));
}

TEST(ExplodeBitmapDecode, WebpLossyAndLossless) // T23: needs a registered WebP loader (platform limit otherwise)
{
    Bytes ll(kWebpLossless, kWebpLossless + sizeof kWebpLossless), ly(kWebpLossy, kWebpLossy + sizeof kWebpLossy);
    if (!webpLoader()) {
        auto o = run(ll, "image/webp");
        expectRefused(o);
        EXPECT_EQ(o.r.outcome.status, Status::unavailable);
        GTEST_SKIP() << "No GdkPixbuf WebP loader on this platform; refusal verified, pixels NOT VERIFIED";
    }
    auto a = run(ll, "image/webp"); ASSERT_TRUE(a.r.ok()) << a.r.outcome.diagnostic;
    auto b = run(ly, "image/webp"); ASSERT_TRUE(b.r.ok()) << b.r.outcome.diagnostic;
    // Explode adds no error to the platform loader's decode. The loader itself need not be exact: the MSYS2 WebP
    // loader premultiplies, so colour under alpha 0 is lost and partially transparent colour moves by the 8-bit
    // round-trip error (LIMITATIONS_LEDGER L-EB-2). Alpha, and colour under full opacity, stay exact.
    std::unique_ptr<GdkPixbuf, decltype(&g_object_unref)> ref(loaderDecode(ll, "image/webp"), &g_object_unref);
    ASSERT_NE(ref.get(), nullptr);
    int const alpha[4] = {0, 85, 170, 255};
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        int const al = alpha[(x + y) % 4];
        Px const source{x * 60, y * 60, 100 + x * 10, al}, got = at(a.r.value, x, y);
        EXPECT_EQ(got, pixbufPx(ref.get(), x, y));
        EXPECT_EQ(got[3], al);
        int const tol = al == 255 ? 0 : 255 / std::max(al, 1) + 1;
        if (al) for (int c = 0; c < 3; ++c) EXPECT_LE(std::abs(got[c] - source[c]), tol) << "channel " << c;
        EXPECT_EQ(at(b.r.value, x, y)[3], al);
        if (alpha[(x + y) % 4] == 255) expectNear(at(b.r.value, x, y), {x * 60, y * 60, 100 + x * 10, 255}, 40);
    }
}

TEST(ExplodeBitmapDecode, ExifOrientationAppliedExactlyOnce) // T23
{
    // original corners A B / C D = red green / blue yellow; displayed corners per EXIF 1..8 (TL TR BL BR)
    Px const A{255, 0, 0, 255}, B{0, 255, 0, 255}, C{0, 0, 255, 255}, D{255, 255, 0, 255};
    std::array<Px, 4> const want[8] = {{A, B, C, D}, {B, A, D, C}, {D, C, B, A}, {C, D, A, B}, {A, C, B, D}, {C, A, D, B}, {D, B, C, A}, {B, D, A, C}};
    for (int o = 1; o <= 8; ++o) {
        Bytes j = quadJpeg(o);
        auto r = run(j, "image/jpeg"); ASSERT_TRUE(r.r.ok()) << o << ": " << r.r.outcome.diagnostic;
        auto const &d = r.r.value;
        bool swap = o >= 5;
        EXPECT_EQ(d.orientation, o);
        ASSERT_EQ(d.width, swap ? 16u : 32u) << o;
        ASSERT_EQ(d.height, swap ? 32u : 16u) << o;
        EXPECT_EQ(d.sourceWidth, 32u);
        std::uint32_t const qx = d.width / 4, qy = d.height / 4;
        expectNear(at(d, qx, qy), want[o - 1][0], 40);
        expectNear(at(d, d.width - 1 - qx, qy), want[o - 1][1], 40);
        expectNear(at(d, qx, d.height - 1 - qy), want[o - 1][2], 40);
        expectNear(at(d, d.width - 1 - qx, d.height - 1 - qy), want[o - 1][3], 40);
        EXPECT_EQ(d.originalBytes, j.size()); // the encoded bytes (EXIF included) stay recoverable
    }
}

// What the canvas would apply: GdkPixbuf's own "orientation" option, absent = 1.
unsigned canvasOrientation(Bytes const &b, char const *mime)
{
    unsigned o = 1;
    GdkPixbufLoader *l = gdk_pixbuf_loader_new_with_mime_type(mime, nullptr);
    if (!l) return 0;
    gdk_pixbuf_loader_write(l, b.data(), b.size(), nullptr);
    gdk_pixbuf_loader_close(l, nullptr);
    if (GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(l)) if (char const *v = gdk_pixbuf_get_option(pb, "orientation")) o = unsigned(std::atoi(v));
    g_object_unref(l);
    return o;
}

TEST(ExplodeBitmapDecode, OrientationAuthorityIsTheCanvasValue) // N1: one-sided cases must follow GdkPixbuf
{
    // header sees EXIF (LONG typed orientation 6), GdkPixbuf may not read it
    Bytes j = quadJpeg(1);
    Bytes app{'E', 'x', 'i', 'f', 0, 0, 'M', 'M', 0, '*', 0, 0, 0, 8, 0, 1, 0x01, 0x12, 0, 4, 0, 0, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0};
    Bytes seg{0xFF, 0xE1}; be16(seg, std::uint32_t(app.size() + 2)); seg.insert(seg.end(), app.begin(), app.end());
    j.insert(j.begin() + 2, seg.begin(), seg.end());
    // GdkPixbuf sees an orientation the header does not parse: PNG eXIf chunk (orientation 6)
    PngSpec s; s.w = 4; s.h = 2; s.ct = 6; s.rows = Bytes(4 * 2 * 4, 90);
    s.after.push_back({"eXIf", Bytes{'M', 'M', 0, '*', 0, 0, 0, 8, 0, 1, 0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 6, 0, 0, 0, 0, 0, 0}});
    struct Case { Bytes data; char const *mime; std::uint32_t w, h; } cases[] = {{j, "image/jpeg", 32, 16}, {png(s), "image/png", 4, 2}};
    for (auto &c : cases) {
        unsigned const want = canvasOrientation(c.data, c.mime);
        fprintf(stderr, "[N1] %s: canvas orientation %u\n", c.mime, want);
        auto o = run(c.data, c.mime);
        ASSERT_TRUE(o.r.ok()) << c.mime << ": " << o.r.outcome.diagnostic; // never refused for a one-sided orientation
        EXPECT_EQ(o.r.value.orientation, want) << c.mime;
        EXPECT_EQ(o.r.value.width, want >= 5 ? c.h : c.w) << c.mime;
        EXPECT_EQ(o.r.value.height, want >= 5 ? c.w : c.h) << c.mime;
    }
}

TEST(ExplodeBitmapDecode, InterlacedAndMultiChunkPng) // T17 stream accounting, T23
{
    auto v = [](int x, int y) { return std::uint8_t(10 * y + x + 1); };
    PngSpec s; s.w = 3; s.h = 3; s.ct = 0; s.interlace = 1;
    s.raw = {0, v(0, 0), 0, v(2, 0), 0, v(0, 2), v(2, 2), 0, v(1, 0), 0, v(1, 2), 0, v(0, 1), v(1, 1), v(2, 1)};
    auto o = run(png(s)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic;
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 3; ++x) EXPECT_EQ(at(o.r.value, x, y)[0], v(x, y)) << x << "," << y;
    s.raw.pop_back(); // one sample short of the Adam7 size
    expectRefused(run(png(s)));
    std::vector<Px> px;
    for (int y = 0; y < 300; ++y) for (int x = 0; x < 300; ++x) px.push_back({x & 255, y & 255, (x ^ y) & 255, 255}); // > one 64 KiB chunk
    Bytes b = rgba(300, 300, px);
    ASSERT_GT(b.size(), 3 * 64 * 1024u);
    auto big = run(b); ASSERT_TRUE(big.r.ok()) << big.r.outcome.diagnostic;
    EXPECT_EQ(at(big.r.value, 299, 7), (Px{299 & 255, 7, (299 ^ 7) & 255, 255}));
}

TEST(ExplodeBitmapDecode, TruncatedCorruptAndMismatchedInputsRefuse) // T17
{
    Bytes p = rgba(8, 8, std::vector<Px>(64, Px{1, 2, 3, 255}));
    for (int pct : {10, 50, 99}) {
        Bytes cut(p.begin(), p.begin() + p.size() * pct / 100);
        expectRefused(run(cut, "image/png"));
    }
    Bytes j = quadJpeg(1);
    for (int pct : {10, 50, 99}) expectRefused(run(Bytes(j.begin(), j.begin() + j.size() * pct / 100), "image/jpeg"));
    { Bytes d = j; for (std::size_t i = d.size() / 3; i < d.size() / 2; ++i) d[i] = 0x55; // damaged entropy data, EOI intact
      auto o = run(d, "image/jpeg"); expectRefused(o); EXPECT_EQ(o.r.outcome.status, Status::failed); } // codec write failure
    { PngSpec s; s.w = 8; s.h = 8; s.rows = Bytes(8 * 8 * 4, 9); s.corruptZlib = true; // passes the header gate, fails in the codec
      auto o = run(png(s)); expectRefused(o); EXPECT_EQ(o.r.outcome.status, Status::failed); }
    { PngSpec s; s.w = 8; s.h = 8; s.rows = Bytes(8 * 8 * 4, 9);
      Bytes b = png(s);
      expectRefused(run(b, "image/png", {}, nullptr, kBig, 9, 8)); // earlier gate promised another size
      expectRefused(run(b, "image/jpeg")); // media type disagrees with the signature
      auto ok = run(b, "image/png", {}, nullptr, kBig, 8, 8); EXPECT_TRUE(ok.r.ok()); }
    { Bytes huge = png(PngSpec{}); // IHDR rewritten to 20000 x 20000, tiny data: refused before any allocation
      std::uint8_t const v[8] = {0, 0, 0x4E, 0x20, 0, 0, 0x4E, 0x20};
      std::memcpy(&huge[16], v, 8);
      std::uint32_t c = crc(huge, 12, 29);
      for (int i = 0; i < 4; ++i) huge[29 + i] = std::uint8_t(c >> (24 - 8 * i));
      auto o = run(huge, "image/png"); expectRefused(o);
      EXPECT_EQ(o.budget->reserved(Stage::canonical), 0u); }
}

struct Tick {
    std::shared_ptr<std::atomic<bool>> flag = std::make_shared<std::atomic<bool>>(false);
    unsigned calls = 0;
    std::uint64_t last = 0;
    bool stopOnFirst = false;
    unsigned stopPhase = 1;    // which phase raises the stop
    unsigned verifyCalls = 0;  // phase 0 (verification) calls
};
// calls/last count the codec writes (phase 1) only; verification passes are phase 0
void tick(void *u, std::uint64_t done, std::uint64_t, unsigned phase)
{
    auto *t = static_cast<Tick *>(u);
    if (phase == 1) { ++t->calls; t->last = done; } else ++t->verifyCalls;
    if (t->stopOnFirst && phase == t->stopPhase) t->flag->store(true);
}

#ifdef HAVE_JPEG
Bytes jpegEncode(int w, int h, bool progressive)
{
    jpeg_compress_struct c;
    jpeg_error_mgr e;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    unsigned char *buf = nullptr; unsigned long sz = 0;
    jpeg_mem_dest(&c, &buf, &sz);
    c.image_width = w; c.image_height = h; c.input_components = 3; c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 95, TRUE);
    if (progressive) jpeg_simple_progression(&c);
    jpeg_start_compress(&c, TRUE);
    std::vector<unsigned char> row(w * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) { row[3 * x] = x < w / 2 ? 255 : 0; row[3 * x + 1] = y < h / 2 ? 255 : 0; row[3 * x + 2] = 40; }
        unsigned char *rp = row.data();
        jpeg_write_scanlines(&c, &rp, 1);
    }
    jpeg_finish_compress(&c);
    Bytes out(buf, buf + sz);
    free(buf);
    jpeg_destroy_compress(&c);
    return out;
}
#endif
Bytes zbomb(std::size_t inflated)
{
    Bytes raw(inflated, 0), z(compressBound(uLong(inflated)));
    uLongf n = uLongf(z.size());
    EXPECT_EQ(compress2(z.data(), &n, raw.data(), uLong(raw.size()), 9), Z_OK);
    z.resize(n);
    return z;
}

TEST(ExplodeBitmapDecode, StopCancelsMidDecode) // T12
{
    Bytes b = rgba(300, 300, std::vector<Px>(300 * 300, Px{9, 9, 9, 255}));
    auto flag = std::make_shared<std::atomic<bool>>(true);
    auto pre = run(b, "image/png", Stop(flag));
    expectRefused(pre);
    EXPECT_EQ(pre.r.outcome.status, Status::canceled);
    Tick t; t.stopOnFirst = true;
    auto o = run(b, "image/png", Stop(t.flag), nullptr, kBig, 0, 0, tick, &t);
    expectRefused(o); // the stop arrived after the first codec write, in the middle of the stream
    EXPECT_EQ(o.r.outcome.status, Status::canceled);
    EXPECT_EQ(t.calls, 1u);
    EXPECT_LT(t.last, b.size());
    Tick full; // control: without a stop every write happens and the decode completes
    auto ok = run(b, "image/png", Stop(full.flag), nullptr, kBig, 0, 0, tick, &full);
    ASSERT_TRUE(ok.r.ok());
    EXPECT_GE(full.calls, 3u);
    EXPECT_EQ(full.last, b.size());
}

#ifdef HAVE_JPEG
TEST(ExplodeBitmapDecode, JpegWritesFollowScansAndStopBetweenScans) // T12, M2
{
    Bytes j = jpegEncode(64, 64, true);
    Tick t;
    auto o = run(j, "image/jpeg", Stop(t.flag), nullptr, kBig, 0, 0, tick, &t);
    ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic;
    EXPECT_GE(t.calls, 10u); // one write per scan of the 10-scan progressive file, not one 64 KiB write
    expectNear(at(o.r.value, 4, 4), {255, 255, 40, 255}, 24);
    Tick s; s.stopOnFirst = true;
    auto c = run(j, "image/jpeg", Stop(s.flag), nullptr, kBig, 0, 0, tick, &s);
    expectRefused(c);
    EXPECT_EQ(c.r.outcome.status, Status::canceled);
    EXPECT_EQ(s.calls, 1u);
    // scan bomb: far more scans than any real progressive file
    Bytes bomb = j;
    Bytes scan{0xFF, 0xDA, 0, 8, 1, 1, 0, 0, 0x3F, 0, 0x12, 0x34};
    for (int i = 0; i < 120; ++i) bomb.insert(bomb.end() - 2, scan.begin(), scan.end());
    Tick bt;
    auto b = run(bomb, "image/jpeg", Stop(bt.flag), nullptr, kBig, 0, 0, tick, &bt);
    expectRefused(b);
    EXPECT_NE(std::string(b.r.outcome.diagnostic).find("scans"), std::string::npos) << b.r.outcome.diagnostic;
    EXPECT_EQ(bt.calls, 0u); // refused before GdkPixbuf saw a byte
    // N3: a stop raised inside the verification pass of a progressive file cancels before any codec write
    Tick v; v.stopOnFirst = true; v.stopPhase = 0;
    auto vc = run(j, "image/jpeg", Stop(v.flag), nullptr, kBig, 0, 0, tick, &v);
    expectRefused(vc);
    EXPECT_EQ(vc.r.outcome.status, Status::canceled);
    EXPECT_GE(v.verifyCalls, 1u);
    EXPECT_EQ(v.calls, 0u);
}

TEST(ExplodeBitmapDecode, DamagedJpegWithIntactEoiRefused) // T17, M3
{
    for (bool progressive : {false, true}) {
        Bytes j = jpegEncode(64, 64, progressive);
        std::size_t sos = 0;
        for (std::size_t i = 0; i + 1 < j.size(); ++i) if (j[i] == 0xFF && j[i + 1] == 0xDA) { sos = i; break; }
        ASSERT_GT(sos, 0u);
        Bytes cut(j.begin(), j.begin() + sos + 60); // cut mid-scan, then the end marker
        cut.push_back(0xFF); cut.push_back(0xD9);
        auto o = run(cut, "image/jpeg");
        expectRefused(o);
        EXPECT_EQ(o.r.outcome.status, Status::failed) << progressive;
        // baseline: libjpeg raises the damage warning (premature end); progressive: libjpeg fails fatally (the scan script is incomplete)
        EXPECT_NE(std::string(o.r.outcome.diagnostic).find(progressive ? "corrupt" : "damaged"), std::string::npos) << o.r.outcome.diagnostic;
    }
    Bytes ok = jpegEncode(64, 64, false);
    EXPECT_TRUE(run(ok, "image/jpeg").r.ok()); // control
    // N2: extraneous bytes before the EOI (JWRN_EXTRANEOUS_DATA) do not damage pixels and the canvas shows the file
    for (bool progressive : {false, true}) {
        Bytes e = jpegEncode(64, 64, progressive);
        e.insert(e.end() - 2, {0x12, 0x34, 0x56});
        auto o = run(e, "image/jpeg");
        ASSERT_TRUE(o.r.ok()) << progressive << ": " << o.r.outcome.diagnostic;
        expectNear(at(o.r.value, 4, 4), {255, 255, 40, 255}, 24);
    }
}

#endif

TEST(ExplodeBitmapDecode, PngMetadataBombRefusedBeforeCodec) // T17, B1
{
    PngSpec base; base.w = 2; base.h = 2; base.rows = Bytes(2 * 2 * 4, 7);
    { PngSpec s = base; s.after.push_back({"zTXt", [] { Bytes d{'k', 0, 0}; Bytes z = zbomb(8 * MiB); d.insert(d.end(), z.begin(), z.end()); return d; }()});
      Tick t; auto o = run(png(s), "image/png", {}, nullptr, kBig, 0, 0, tick, &t);
      expectRefused(o); EXPECT_EQ(t.calls, 0u); } // 8 MiB inflated from ~8 KiB, after IDAT
    { PngSpec s = base; Bytes z = zbomb(2 * MiB);
      for (int i = 0; i < 3; ++i) { Bytes d{'k', 0, 0}; d.insert(d.end(), z.begin(), z.end()); s.after.push_back({"zTXt", d}); } // 6 MiB together
      expectRefused(run(png(s))); }
    { PngSpec s = base; Bytes z = zbomb(2 * MiB);
      Bytes d{'k', 0, 1, 0, 'e', 'n', 0, 'x', 0}; d.insert(d.end(), z.begin(), z.end());
      s.after.push_back({"iTXt", d}); s.after.push_back({"iTXt", d}); s.after.push_back({"iTXt", d});
      expectRefused(run(png(s))); } // compressed iTXt
    { PngSpec s = base; s.iccp = Bytes(5 * MiB, 0); expectRefused(run(png(s))); } // inflated profile over the cap
    { PngSpec s = base; Bytes t{'k', 0}; t.insert(t.end(), 20 * 1024, 'v'); // 300 chunks, ~6 MiB of plain text: bounded by the file size
      for (int i = 0; i < 300; ++i) s.after.push_back({"tEXt", t});
      auto o = run(png(s)); EXPECT_TRUE(o.r.ok()) << o.r.outcome.diagnostic; }
    { PngSpec s = base; s.after.push_back({"zTXt", [] { Bytes d{'k', 0, 0}; Bytes z = zbomb(1000); d.insert(d.end(), z.begin(), z.end()); return d; }()});
      s.after.push_back({"tEXt", Bytes{'k', 0, 'v'}});
      EXPECT_TRUE(run(png(s)).r.ok()); } // ordinary metadata is fine
}

TEST(ExplodeBitmapDecode, ValidPngIdatBoundaries) // M1
{
    PngSpec s; s.w = 255; s.h = 256; s.ct = 0; s.rows = Bytes(255 * 256, 77); // inflated size 256 * 256 = 64 KiB exactly
    { auto o = run(png(s)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic; EXPECT_EQ(at(o.r.value, 254, 255)[0], 77); }
    s.split = {2 + 5 + 65535 + 5 + 1}; // first IDAT ends exactly as the 64 KiB output buffer fills
    { auto o = run(png(s)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic; }
    s.emptyIdat = true;                 // legal zero-length IDAT between the pieces
    { auto o = run(png(s)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic; }
    PngSpec e; e.w = 2; e.h = 2; e.rows = Bytes(16, 5); e.emptyIdat = true;
    { auto o = run(png(e)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic; }
    PngSpec many; many.w = 2; many.h = 2; many.rows = Bytes(16, 5); many.split = {3, 4, 9, 10};
    { auto o = run(png(many)); ASSERT_TRUE(o.r.ok()) << o.r.outcome.diagnostic; }
}

TEST(ExplodeBitmapDecode, AllocationAndLedgerFaultSweep) // T07
{
    PngSpec s; s.w = 16; s.h = 16; s.ct = 2; s.iccp = linearRgbProfile(); s.rows = Bytes(16 * 16 * 3, 128);
    Bytes b = png(s);
    unsigned failures = 0, total = 0;
    for (std::uint64_t k = 1; k < 64; ++k) {
        AllocationFault f; f.failAt = k;
        auto o = run(b, "image/png", {}, &f);
        if (o.r.ok()) { total = unsigned(k - 1); EXPECT_EQ(at(o.r.value, 0, 0), (Px{128, 128, 128, 255})); break; }
        ++failures;
        expectRefused(o);
        EXPECT_EQ(o.r.outcome.status, Status::failed);
    }
    EXPECT_GE(failures, 3u); // output, codec, profile bytes
    EXPECT_EQ(failures, total);
    // ledger limits: every refusal balances; the limit that fits succeeds
    bool sawOk = false, sawRefusal = false;
    for (std::uint64_t limit = 1; limit < 16 * MiB; limit = limit * 2 + 1) {
        auto o = run(b, "image/png", {}, nullptr, limit);
        if (o.r.ok()) sawOk = true; else { sawRefusal = true; expectRefused(o); }
    }
    EXPECT_TRUE(sawOk && sawRefusal);
}
