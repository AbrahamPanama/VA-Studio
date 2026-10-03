// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Tests for Explode Bitmap (PNG): T04 amplification, T06 arithmetic, T07 faults, T12 stop,
 * T22 gutter/bleed, T23 profile/dpi. Every PNG is decoded back and compared with an independent mask oracle.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <lcms2.h>
#include <memory>
#include <png.h>
#include <tuple>
#include <vector>

#include "util/bitmap-input-decode.h"
#include "util/bitmap-piece-encoder.h"
#include "util/bitmap-island-specks.h"

using namespace Inkscape::Bitmap;
using Bytes = std::vector<std::uint8_t>;

namespace {

struct Fx {
    std::unique_ptr<Budget> budget;
    FinalGrid grid;
    Regions regions;
    Result<Partition> part;
    unsigned w = 0, h = 0;
};

std::unique_ptr<Fx> make(unsigned w, unsigned h, Bytes const &rgba, Bytes const &profile = {}, double dpiX = 300, double dpiY = 300, JobWork *sharedWork = nullptr)
{
    auto f = std::make_unique<Fx>();
    f->budget = std::make_unique<Budget>(Budget::FixedLimitForTest{}, 1536 * MiB);
    f->w = w; f->h = h;
    f->grid.width = w; f->grid.height = h; f->grid.dpiX = dpiX; f->grid.dpiY = dpiY;
    EXPECT_TRUE(f->grid.pixels.allocate(*f->budget, Stage::prepared, std::uint64_t(w) * h, 4).ok());
    std::memcpy(f->grid.pixels.data(), rgba.data(), rgba.size());
    if (!profile.empty()) {
        EXPECT_TRUE(f->grid.profile.allocate(*f->budget, Stage::prepared, profile.size(), 1).ok());
        std::memcpy(f->grid.profile.data(), profile.data(), profile.size());
    }
    AlphaLut lut; for (unsigned i = 0; i < 256; ++i) lut[i] = std::uint8_t(i);
    RgbaView view{reinterpret_cast<std::uint8_t const *>(f->grid.pixels.data()), rgba.size(), std::uint64_t(w) * 4, w, h};
    JobWork localWork(std::uint64_t(w) * h);
    auto &work = sharedWork ? *sharedWork : localWork;
    auto labelled = label(view, lut, *f->budget, work);
    EXPECT_TRUE(labelled.ok());
    f->regions = std::move(labelled.value);
    f->part = enclose(f->regions, *f->budget, work);
    EXPECT_TRUE(f->part.ok());
    return f;
}
std::uint8_t colour(unsigned x, unsigned y, unsigned c) { return std::uint8_t((x * 131 + y * 71 + c * 53 + 17) * 29 % 251 + 1); }
Bytes image(unsigned w, unsigned h, unsigned density, unsigned seed, bool randomColour = false)
{
    Bytes b(std::size_t(w) * h * 4);
    std::uint32_t s = seed * 2654435761u + 1;
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        s = s * 1664525u + 1013904223u;
        bool on = (s >> 16) % 100 < density;
        std::uint8_t a = on ? (((s >> 8) & 7) == 0 ? std::uint8_t(1 + (s >> 3) % 254) : 255) : 0;
        auto *p = &b[(std::size_t(y) * w + x) * 4];
        for (unsigned c = 0; c < 3; ++c) p[c] = randomColour ? std::uint8_t(s >> (c * 5)) : colour(x, y, c);
        p[3] = randomColour ? std::uint8_t(s >> 24) | 1 : a;
    }
    return b;
}
Bytes profileBytes(cmsHPROFILE p)
{
    cmsUInt32Number n = 0; cmsSaveProfileToMem(p, nullptr, &n);
    Bytes b(n); cmsSaveProfileToMem(p, b.data(), &n); cmsCloseProfile(p); return b;
}
Bytes srgb() { return profileBytes(cmsCreate_sRGBProfile()); }
Bytes greyProfile()
{
    cmsCIExyY d65{0.3127, 0.3290, 1.0}; cmsToneCurve *g = cmsBuildGamma(nullptr, 2.2);
    auto p = cmsCreateGrayProfile(&d65, g); cmsFreeToneCurve(g); return profileBytes(p);
}

struct Read {
    unsigned w = 0, h = 0; int colourType = -1;
    Bytes rgba;
    Bytes icc; bool hasIcc = false, gama = false, chrm = false, srgbChunk = false;
    std::uint32_t ppmX = 0, ppmY = 0;
    bool ok = false;
};
struct Src { std::uint8_t const *p; std::size_t n, at; };
void readFn(png_structp png, png_bytep out, png_size_t n)
{
    auto *s = static_cast<Src *>(png_get_io_ptr(png));
    if (n > s->n - s->at) png_error(png, "eof");
    std::memcpy(out, s->p + s->at, n); s->at += n;
}
// Independent framing/order check, before libpng sees any chunks.
std::vector<std::pair<std::size_t, std::size_t>> chunks(std::uint8_t const *data, std::size_t size)
{
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (size < 8 || png_sig_cmp(data, 0, 8)) return out;
    for (std::size_t at = 8; at < size;) {
        if (size - at < 12) return {};
        std::uint64_t n = 0; for (unsigned k = 0; k < 4; ++k) n = (n << 8) | data[at + k];
        if (n > size - at - 12) return {};
        out.emplace_back(at, n + 12); at += n + 12;
    }
    return out;
}
bool orderedPng(std::uint8_t const *data, std::size_t size)
{
    auto list = chunks(data, size);
    if (list.size() < 4) return false;
    auto is = [&](std::size_t i, char const *name) { return !std::memcmp(data + list[i].first + 4, name, 4); };
    if (!is(0, "IHDR") || list[0].second != 25 || !is(list.size() - 1, "IEND") || list.back().second != 12) return false;
    std::size_t i = 1;
    if (is(i, "iCCP")) ++i;
    if (!is(i, "pHYs") || list[i].second != 21) return false;
    ++i; auto first = i;
    while (i < list.size() - 1 && is(i, "IDAT")) ++i;
    return i > first && i == list.size() - 1;
}
void readPng(std::uint8_t const *data, std::size_t size, Read &r)
{
    r = Read{};
    if (!orderedPng(data, size)) return;
    Src src{data, size, 0};
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) { png_destroy_read_struct(&png, &info, nullptr); r.ok = false; return; }
    png_set_crc_action(png, PNG_CRC_ERROR_QUIT, PNG_CRC_ERROR_QUIT);
    png_set_read_fn(png, &src, readFn);
    png_read_info(png, info);
    r.w = png_get_image_width(png, info); r.h = png_get_image_height(png, info);
    r.colourType = png_get_color_type(png, info);
    png_set_gray_to_rgb(png);
    png_read_update_info(png, info);
    r.rgba.assign(std::size_t(r.w) * r.h * 4, 0);
    for (unsigned y = 0; y < r.h; ++y) png_read_row(png, &r.rgba[std::size_t(y) * r.w * 4], nullptr);
    png_read_end(png, info);
    png_charp name; int comp; png_bytep prof; png_uint_32 plen;
    if (png_get_iCCP(png, info, &name, &comp, &prof, &plen)) { r.hasIcc = true; r.icc.assign(prof, prof + plen); }
    r.gama = png_get_valid(png, info, PNG_INFO_gAMA); r.chrm = png_get_valid(png, info, PNG_INFO_cHRM);
    r.srgbChunk = png_get_valid(png, info, PNG_INFO_sRGB);
    png_uint_32 rx, ry; int unit;
    if (png_get_pHYs(png, info, &rx, &ry, &unit) && unit == PNG_RESOLUTION_METER) { r.ppmX = rx; r.ppmY = ry; }
    png_destroy_read_struct(&png, &info, nullptr);
    r.ok = true;
}

// Independent oracle: which piece owns each grid pixel.
std::vector<int> ownerPlane(Fx const &f)
{
    std::vector<int> o(std::size_t(f.w) * f.h, -1);
    auto const &p = f.part.value;
    for (unsigned k = 0; k < p.runCount; ++k) {
        auto r = p.run(k);
        if (!r.foreground) continue;
        for (unsigned x = r.x; x < r.end; ++x) o[std::size_t(r.y) * f.w + x] = int(r.piece);
    }
    return o;
}
// Expected crop of piece i (gutter-inclusive), bleed done by brute force.
Bytes expectedCrop(Fx const &f, std::vector<int> const &owner, unsigned i)
{
    auto const &pc = f.part.value.pieces()[i];
    int cw = pc.endX - pc.x + 2, ch = pc.endY - pc.y + 2;
    auto src = reinterpret_cast<std::uint8_t const *>(f.grid.pixels.data());
    auto visible = [&](int cx, int cy) {
        int gx = cx - 1 + int(pc.x), gy = cy - 1 + int(pc.y);
        return gx >= 0 && gy >= 0 && gx < int(f.w) && gy < int(f.h) && gx >= int(pc.x) && gx < int(pc.endX) &&
               gy >= int(pc.y) && gy < int(pc.endY) && owner[std::size_t(gy) * f.w + gx] == int(i);
    };
    Bytes e(std::size_t(cw) * ch * 4, 0);
    for (int y = 0; y < ch; ++y) for (int x = 0; x < cw; ++x) {
        auto *d = &e[(std::size_t(y) * cw + x) * 4];
        if (visible(x, y)) { std::memcpy(d, src + (std::size_t(y - 1 + pc.y) * f.w + (x - 1 + pc.x)) * 4, 4); continue; }
        std::tuple<int, int, int> best{99, 0, 0}; bool found = false;
        for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
            int d2 = dx * dx + dy * dy;
            if (!d2 || d2 > 4 || x + dx < 0 || y + dy < 0 || x + dx >= cw || y + dy >= ch || !visible(x + dx, y + dy)) continue;
            if (std::make_tuple(d2, dy, dx) < best) { best = {d2, dy, dx}; found = true; }
        }
        if (found) {
            auto *s = src + (std::size_t(y + std::get<1>(best) - 1 + pc.y) * f.w + (x + std::get<2>(best) - 1 + pc.x)) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
    }
    return e;
}
void verifyAll(Fx const &f, EncodedPieces const &enc, bool expectIcc, Bytes const &icc = {})
{
    auto owner = ownerPlane(f);
    ASSERT_EQ(enc.count(), f.part.value.pieceCount);
    for (unsigned i = 0; i < enc.count(); ++i) {
        SCOPED_TRACE(i);
        auto const &e = enc.piece(i); auto const &pc = f.part.value.pieces()[i];
        ASSERT_TRUE(orderedPng(e.data, e.size));
        Read r; readPng(e.data, e.size, r); ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.w, e.width); EXPECT_EQ(r.h, e.height);
        EXPECT_EQ(e.x, int(pc.x) - 1); EXPECT_EQ(e.y, int(pc.y) - 1);
        EXPECT_EQ(r.rgba, expectedCrop(f, owner, i));
        for (unsigned x = 0; x < r.w; ++x) { EXPECT_EQ(r.rgba[x * 4 + 3], 0); EXPECT_EQ(r.rgba[(std::size_t(r.h - 1) * r.w + x) * 4 + 3], 0); }
        for (unsigned y = 0; y < r.h; ++y) { EXPECT_EQ(r.rgba[std::size_t(y) * r.w * 4 + 3], 0); EXPECT_EQ(r.rgba[(std::size_t(y) * r.w + r.w - 1) * 4 + 3], 0); }
        EXPECT_EQ(r.hasIcc, expectIcc); if (expectIcc) EXPECT_EQ(r.icc, icc);
        EXPECT_FALSE(r.gama); EXPECT_FALSE(r.chrm); EXPECT_FALSE(r.srgbChunk);
        EXPECT_LE(e.size, worstPngBytes(e.width, e.height, icc.size()));
    }
}
Bytes lAndDot()
{
    Bytes b(8 * 8 * 4, 0); // 8x8 canvas: crops (64 + 9 px) stay within 2P
    auto put = [&](unsigned x, unsigned y) { for (unsigned c = 0; c < 3; ++c) b[(y * 8 + x) * 4 + c] = colour(x, y, c); b[(y * 8 + x) * 4 + 3] = 255; };
    for (unsigned y = 0; y < 6; ++y) put(0, y);
    for (unsigned x = 0; x < 6; ++x) put(x, 5);
    put(2, 3);
    return b;
}
std::uint64_t ceilDiv3(std::uint64_t v) { return (v + 2) / 3; }

} // namespace

TEST(ExplodeBitmapPng, T22OwnedMaskGutterAndBleedAgainstOracle)
{
    auto f = make(8, 8, lAndDot());
    ASSERT_EQ(f->part.value.pieceCount, 2u);
    auto const &L = f->part.value.pieces()[0];
    EXPECT_TRUE(L.x <= 2 && L.endX > 2 && L.y <= 3 && L.endY > 3); // L's bbox covers the dot's pixel
    auto enc = encode(f->grid, f->part.value, *f->budget);
    ASSERT_TRUE(enc.ok()) << enc.outcome.diagnostic;
    verifyAll(*f, enc.value, false);
    Read r; readPng(enc.value.piece(0).data, enc.value.piece(0).size, r);
    // The dot's pixel (2,3) is crop (3,4) in L: transparent, with L's own colour bled from (0,3), never the dot's.
    auto *p = &r.rgba[(4 * r.w + 3) * 4];
    EXPECT_EQ(p[3], 0);
    EXPECT_EQ(p[0], colour(0, 3, 0)); EXPECT_NE(p[0], colour(2, 3, 0));
    // The crop is bbox + 1 px each side.
    EXPECT_EQ(enc.value.piece(0).width, 8u); EXPECT_EQ(enc.value.piece(1).width, 3u);
    EXPECT_EQ(enc.value.piece(1).x, 1); EXPECT_EQ(enc.value.piece(1).y, 2);
    // Bleed never reaches beyond 2 px: an isolated dot has colour only within d^2 <= 4.
    auto dot = make(7, 7, [] { Bytes b(7 * 7 * 4, 0); auto *p = &b[(3 * 7 + 3) * 4]; p[0] = 9; p[1] = 8; p[2] = 7; p[3] = 200; return b; }());
    auto e2 = encode(dot->grid, dot->part.value, *dot->budget); ASSERT_TRUE(e2.ok());
    Read d; readPng(e2.value.piece(0).data, e2.value.piece(0).size, d); ASSERT_EQ(d.w, 3u);
    for (unsigned i = 0; i < 9; ++i) { // 1 px gutter all around: every neighbour is within d^2 <= 2
        if (i == 4) { EXPECT_EQ(d.rgba[i * 4 + 3], 200); continue; }
        EXPECT_EQ(d.rgba[i * 4 + 3], 0); EXPECT_EQ(d.rgba[i * 4], 9); EXPECT_EQ(d.rgba[i * 4 + 2], 7);
    }
}

TEST(ExplodeBitmapPng, T22RandomImagesMatchOracleAndAreDeterministic)
{
    for (unsigned seed = 1; seed <= 6; ++seed) for (unsigned density : {12u, 30u}) {
        SCOPED_TRACE(seed * 100 + density);
        auto f = make(24 + seed, 18 + seed, image(24 + seed, 18 + seed, density, seed));
        ASSERT_TRUE(f->part.ok());
        auto a = encode(f->grid, f->part.value, *f->budget), b = encode(f->grid, f->part.value, *f->budget);
        ASSERT_TRUE(a.ok() && b.ok());
        verifyAll(*f, a.value, false);
        ASSERT_EQ(a.value.count(), b.value.count());
        for (unsigned i = 0; i < a.value.count(); ++i) {
            ASSERT_EQ(a.value.piece(i).size, b.value.piece(i).size);
            EXPECT_EQ(std::memcmp(a.value.piece(i).data, b.value.piece(i).data, a.value.piece(i).size), 0);
        }
    }
}

TEST(ExplodeBitmapPng, T23ProfileByteIdentityDpiAndNoContradictoryChunks)
{
    Bytes icc = srgb();
    auto f = make(8, 8, lAndDot(), icc, 254, 508);
    auto enc = encode(f->grid, f->part.value, *f->budget); ASSERT_TRUE(enc.ok()) << enc.outcome.diagnostic;
    verifyAll(*f, enc.value, true, icc);
    for (unsigned i = 0; i < enc.value.count(); ++i) {
        Read r; readPng(enc.value.piece(i).data, enc.value.piece(i).size, r);
        EXPECT_EQ(r.ppmX, 10000u); EXPECT_EQ(r.ppmY, 20000u);
        EXPECT_EQ(r.colourType, PNG_COLOR_TYPE_RGB_ALPHA);
    }
    // The existing decoder reads the same profile and the same visible samples.
    auto const &e = enc.value.piece(0);
    ValidatedInput in; in.encoded = {e.data, e.size, "image/png"};
    Budget db(Budget::FixedLimitForTest{}, 64 * MiB);
    auto d = decode(in, db); ASSERT_TRUE(d.ok()) << d.outcome.diagnostic;
    ASSERT_EQ(d.value.profileBytes, icc.size());
    EXPECT_EQ(std::memcmp(d.value.profile.data(), icc.data(), icc.size()), 0);
    Read r; readPng(e.data, e.size, r);
    for (std::size_t i = 0; i < r.rgba.size(); i += 4)
        if (r.rgba[i + 3]) for (int c = 0; c < 4; ++c) EXPECT_EQ(std::to_integer<int>(d.value.pixels.data()[i + c]), r.rgba[i + c]);
    // Untagged stays untagged; dpi 300 -> 11811 px/m.
    auto u = make(8, 8, lAndDot()); auto eu = encode(u->grid, u->part.value, *u->budget); ASSERT_TRUE(eu.ok());
    r = Read{}; readPng(eu.value.piece(0).data, eu.value.piece(0).size, r); EXPECT_FALSE(r.hasIcc); EXPECT_EQ(r.ppmX, 11811u);
    // Grey profile: neutral samples are written as grey+alpha so the profile matches; coloured samples are refused.
    Bytes grey(10 * 10 * 4, 0);
    for (unsigned i = 0; i < 100; ++i) { grey[i * 4] = grey[i * 4 + 1] = grey[i * 4 + 2] = std::uint8_t(i * 7); grey[i * 4 + 3] = i % 5 ? 255 : 0; }
    Bytes gp = greyProfile();
    auto g = make(10, 10, grey, gp);
    auto eg = encode(g->grid, g->part.value, *g->budget); ASSERT_TRUE(eg.ok()) << eg.outcome.diagnostic;
    r = Read{}; readPng(eg.value.piece(0).data, eg.value.piece(0).size, r); EXPECT_EQ(r.colourType, PNG_COLOR_TYPE_GRAY_ALPHA); EXPECT_EQ(r.icc, gp);
    verifyAll(*g, eg.value, true, gp);
    auto colourful = make(8, 8, lAndDot(), gp);
    auto bad = encode(colourful->grid, colourful->part.value, *colourful->budget);
    EXPECT_EQ(bad.outcome.status, Status::incompatible); EXPECT_EQ(colourful->budget->reserved(Stage::png), 0u);
    // Non-RGB/grey and truncated profiles never reach the file.
    auto cmyk = make(8, 8, lAndDot(), profileBytes(cmsCreateLab4Profile(nullptr)));
    EXPECT_EQ(encode(cmyk->grid, cmyk->part.value, *cmyk->budget).outcome.status, Status::incompatible);
}

TEST(ExplodeBitmapPng, T23InvalidIccRefusedBeforePixelWork)
{
    auto rgb = srgb();
    auto put = [](Bytes &p, unsigned at, std::uint32_t n) { png_save_uint_32(p.data() + at, n); };
    auto rejected = [&](Bytes const &icc, char const *reason) {
        auto f = make(8, 8, lAndDot(), icc);
        auto before = f->budget->reserved();
        AllocationFault fault; JobWork work(64); EncodeOptions o;
        o.fault = &fault; o.work = &work;
        auto res = encode(f->grid, f->part.value, *f->budget, {}, o);
        EXPECT_EQ(res.outcome.status, Status::incompatible);
        EXPECT_STREQ(res.outcome.diagnostic, reason);
        EXPECT_EQ(res.value.count(), 0u); EXPECT_EQ(f->budget->reserved(), before);
        EXPECT_EQ(fault.attempts, 0u); EXPECT_EQ(work.visits(), 0u); // no crop/codec/pixel work
    };
    Bytes tiny(20, 0); std::memcpy(tiny.data() + 16, "RGB ", 4);
    rejected(tiny, "ICC profile is too short (minimum 132 bytes).");
    auto bad = rgb; bad.resize(131);
    rejected(bad, "ICC profile is too short (minimum 132 bytes).");
    bad = rgb; bad.pop_back(); // actual truncation, unchanged embedded length
    rejected(bad, "ICC profile embedded length does not match the buffer.");
    bad = rgb; put(bad, 0, rgb.size() + 4);
    rejected(bad, "ICC profile embedded length does not match the buffer.");
    bad = rgb; bad.resize(rgb.size() - 4); put(bad, 0, bad.size()); // truncated tag payload
    rejected(bad, "ICC profile tag lies outside the buffer.");
    bad = rgb; put(bad, 128, UINT32_MAX);
    rejected(bad, "ICC profile tag count exceeds its table.");
    bad = rgb; put(bad, 36, 0);
    rejected(bad, "ICC profile header signature is invalid.");
    bad = rgb; put(bad, 64, 0xffff);
    rejected(bad, "ICC profile rendering intent is invalid.");
    bad = rgb; put(bad, 20, 0);
    rejected(bad, "ICC profile connection space must be XYZ or Lab.");
    for (auto cls : {0x61627374u, 0x6c696e6bu}) {
        bad = rgb; put(bad, 12, cls);
        rejected(bad, "ICC profile class cannot be embedded in a PNG.");
    }
    bad = rgb; put(bad, 16, 0);
    rejected(bad, "ICC profile colour space must be RGB or GRAY for this PNG.");
    bad = rgb; bad.push_back(0); put(bad, 0, bad.size()); bad[8] = 4;
    rejected(bad, "ICC profile length must be a multiple of four.");
    // Valid RGB and GRAY byte identity and matching colour types: T23ProfileDpiAndUntagged.
}

TEST(ExplodeBitmapPng, T04AccountingBoundsAndAmplification)
{
    Bytes icc = srgb();
    auto f = make(40, 30, image(40, 30, 55, 9, true), icc);
    auto b = f->budget.get();
    auto base = b->reserved();
    {
        auto enc = encode(f->grid, f->part.value, *b); ASSERT_TRUE(enc.ok());
        std::uint64_t B = 0, Q = 0, H = 0;
        for (unsigned i = 0; i < enc.value.count(); ++i) {
            auto const &p = enc.value.piece(i);
            B += std::uint64_t(p.width) * p.height; Q += p.size; H += 22 + 4 * ceilDiv3(p.size);
            EXPECT_LE(p.size, worstPngBytes(p.width, p.height, icc.size())); // random RGBA is the codec's worst case
        }
        EXPECT_EQ(enc.value.cropArea, B); EXPECT_EQ(enc.value.encodedBytes, Q); EXPECT_EQ(enc.value.hrefBytes, H);
        EXPECT_EQ(enc.consumed, Q);
        EXPECT_LE(B, 2u * 40 * 30);
        // Ledger holds exactly Q (+index) and H after the worst-case reservations shrank; scratch/crop are gone.
        EXPECT_EQ(b->reserved(Stage::png), Q + enc.value.count() * sizeof(EncodedPiece));
        EXPECT_EQ(b->reserved(Stage::href), H);
        EXPECT_EQ(b->reserved(Stage::crop) + b->reserved(Stage::encoder), 0u);
        EXPECT_EQ(b->reserved(), base + Q + H + enc.value.count() * sizeof(EncodedPiece));
    }
    EXPECT_EQ(b->reserved(), base); // everything released on destruction

    // B >> P: 1 px specks spaced 2 apart need 9 px of crop each (2.25 P > 2P): refused before any allocation.
    Bytes dots(64 * 64 * 4, 0);
    for (unsigned y = 0; y < 64; y += 2) for (unsigned x = 0; x < 64; x += 2) { dots[(y * 64 + x) * 4] = 5; dots[(y * 64 + x) * 4 + 3] = 255; }
    auto d = make(64, 64, dots); ASSERT_EQ(d->part.value.pieceCount, 1024u);
    auto before = d->budget->reserved();
    auto refused = encode(d->grid, d->part.value, *d->budget);
    EXPECT_FALSE(refused.ok()); EXPECT_EQ(d->budget->reserved(), before);
    EncodeOptions low; low.maxCropPixels = 100; // lower-only ceiling refuses a tiny B too
    EXPECT_FALSE(encode(f->grid, f->part.value, *b, {}, low).ok()); EXPECT_EQ(b->reserved(), base);

    // 20,000 tiny pieces (spacing 3: B = P): count, order, exact Q/H and the per-piece chunk overhead.
    Bytes tiny(432 * 432 * 4, 0);
    for (unsigned y = 1; y < 432; y += 3) for (unsigned x = 1; x < 432; x += 3) { auto *p = &tiny[(y * 432 + x) * 4]; p[0] = x; p[1] = y; p[3] = 255; }
    auto t = make(432, 432, tiny, icc);
    ASSERT_EQ(t->part.value.pieceCount, 144u * 144u);
    ASSERT_GE(t->part.value.pieceCount, 20000u);
    auto et = encode(t->grid, t->part.value, *t->budget); ASSERT_TRUE(et.ok()) << et.outcome.diagnostic;
    std::uint64_t Q = 0, H = 0;
    for (unsigned i = 0; i < et.value.count(); ++i) {
        Q += et.value.piece(i).size; H += 22 + 4 * ceilDiv3(et.value.piece(i).size);
        EXPECT_EQ(et.value.piece(i).width, 3u);
        EXPECT_GT(et.value.piece(i).size, 300u); // signature/IHDR/pHYs/iCCP/IDAT/IEND overhead is real and counted
    }
    EXPECT_EQ(et.value.encodedBytes, Q); EXPECT_EQ(et.value.hrefBytes, H);
    Read r; readPng(et.value.piece(5000).data, et.value.piece(5000).size, r); ASSERT_TRUE(r.ok); EXPECT_EQ(r.w, 3u);
    EXPECT_EQ(r.icc, icc);
}

TEST(ExplodeBitmapPng, T06ArithmeticBoundariesQHB)
{
    EXPECT_EQ(worstPngBytes(UINT32_MAX, UINT32_MAX, 0), 0u);
    EXPECT_EQ(worstPngBytes(UINT32_MAX, UINT32_MAX, UINT64_MAX), 0u);
    EXPECT_EQ(worstPngBytes(3, 3, UINT64_MAX), 0u);
    EXPECT_TRUE(pieceAxesFit(32765, 32765)); // incl. gutters: 32767 accepted
    EXPECT_FALSE(pieceAxesFit(32766, 1));   // incl. gutters: 32768 refused on either axis
    EXPECT_FALSE(pieceAxesFit(1, 32766));
    EXPECT_FALSE(pieceAxesFit(0, 1));
    EXPECT_FALSE(pieceAxesFit(UINT32_MAX, 1));
    EXPECT_GT(worstPngBytes(3, 3, 0), 3u * 13 + 100);

    // Axis: the grid axis cap (16,384) keeps crops far below Cairo's 32,767 incl. gutters; the widest grid works.
    Bytes full(16384 * 3 * 4, 0);
    for (std::size_t i = 0; i < full.size(); i += 4) { full[i] = 1; full[i + 3] = 255; }
    auto ok = make(16384, 3, full);
    auto e = encode(ok->grid, ok->part.value, *ok->budget); ASSERT_TRUE(e.ok()) << e.outcome.diagnostic;
    EXPECT_EQ(e.value.piece(0).width, 16386u); EXPECT_EQ(e.value.piece(0).x, -1);

    // H and ledger +-1 on a single piece.
    auto f = make(6, 6, Bytes(6 * 6 * 4, 255)); ASSERT_EQ(f->part.value.pieceCount, 1u);
    std::uint64_t w = worstPngBytes(8, 8, 0), wh = 22 + 4 * ceilDiv3(w);
    EncodeOptions o; o.maxHref = wh;
    EXPECT_TRUE(encode(f->grid, f->part.value, *f->budget, {}, o).ok());
    o.maxHref = wh - 1;
    auto r = encode(f->grid, f->part.value, *f->budget, {}, o); EXPECT_FALSE(r.ok());
    std::uint64_t scratch = encoderScratchBytes(8, 0), peak = sizeof(EncodedPiece) + w + wh + scratch + 8 * 8 * 4;
    Budget exact(Budget::FixedLimitForTest{}, peak + 144);
    { FinalGrid g; g.width = 6; g.height = 6; g.dpiX = g.dpiY = 300;
      ASSERT_TRUE(g.pixels.allocate(exact, Stage::prepared, 36, 4).ok()); std::memset(g.pixels.data(), 255, 144);
      EXPECT_TRUE(encode(g, f->part.value, exact).ok()); }
    Budget under(Budget::FixedLimitForTest{}, peak + 143);
    { FinalGrid g; g.width = 6; g.height = 6; g.dpiX = g.dpiY = 300;
      ASSERT_TRUE(g.pixels.allocate(under, Stage::prepared, 36, 4).ok()); std::memset(g.pixels.data(), 255, 144);
      EXPECT_FALSE(encode(g, f->part.value, under).ok()); EXPECT_EQ(under.reserved(), 144u); }

    // Combined crop + encoder scratch cap is 128 MiB (crop here is 256 bytes).
    o = {}; o.scratchOverride = 128 * MiB - 8 * 8 * 4;
    EXPECT_TRUE(encode(f->grid, f->part.value, *f->budget, {}, o).ok());
    o.scratchOverride += 1;
    EXPECT_FALSE(encode(f->grid, f->part.value, *f->budget, {}, o).ok());
    // The computed codec scratch is sufficient and not vacuous: the smallest working scratch is below it.
    auto big = make(300, 4, image(300, 4, 100, 3, true), srgb());
    std::uint64_t computed = encoderScratchBytes(302, srgb().size()), lo = 1, hi = computed;
    o = {}; o.scratchOverride = computed;
    ASSERT_TRUE(encode(big->grid, big->part.value, *big->budget, {}, o).ok());
    while (lo < hi) {
        std::uint64_t mid = (lo + hi) / 2; o.scratchOverride = mid;
        if (encode(big->grid, big->part.value, *big->budget, {}, o).ok()) hi = mid; else lo = mid + 1;
    }
    EXPECT_LT(lo, computed);
    o.scratchOverride = lo - 1;
    EXPECT_FALSE(encode(big->grid, big->part.value, *big->budget, {}, o).ok());
    EXPECT_EQ(big->budget->reserved(Stage::png) + big->budget->reserved(Stage::href) + big->budget->reserved(Stage::encoder), 0u);
    // Encoder counts 1 and 2: scratch scales, output is identical.
    o = {}; auto e1 = encode(f->grid, f->part.value, *f->budget, {}, o); o.encoders = 2;
    auto e2 = encode(f->grid, f->part.value, *f->budget, {}, o); ASSERT_TRUE(e1.ok() && e2.ok());
    EXPECT_EQ(e1.value.piece(0).size, e2.value.piece(0).size);
    o.encoders = 3; EXPECT_FALSE(encode(f->grid, f->part.value, *f->budget, {}, o).ok());
    o.encoders = 0; EXPECT_FALSE(encode(f->grid, f->part.value, *f->budget, {}, o).ok());
}

TEST(ExplodeBitmapPng, T07FaultSweepAndInconsistentInputs)
{
    auto f = make(10, 8, image(10, 8, 30, 4), srgb());
    auto base = f->budget->reserved();
    auto reference = encode(f->grid, f->part.value, *f->budget); ASSERT_TRUE(reference.ok());
    unsigned failures = 0, successes = 0;
    for (std::uint64_t k = 1; k < 3000; ++k) {
        AllocationFault fault; fault.failAt = k;
        EncodeOptions o; o.fault = &fault;
        auto r = encode(f->grid, f->part.value, *f->budget, {}, o);
        if (r.ok()) {
            ++successes; ASSERT_EQ(r.value.encodedBytes, reference.value.encodedBytes);
            r.value.reset(); EXPECT_EQ(f->budget->reserved(), base + reference.value.encodedBytes + reference.value.hrefBytes +
                                                                  reference.value.count() * sizeof(EncodedPiece));
            if (fault.attempts < k) break; // failAt beyond the last allocation: sweep complete
            continue;
        }
        ++failures;
        EXPECT_EQ(r.outcome.status, Status::failed) << k; EXPECT_EQ(r.value.count(), 0u);
        EXPECT_EQ(f->budget->reserved(), base + reference.value.encodedBytes + reference.value.hrefBytes +
                                             reference.value.count() * sizeof(EncodedPiece)) << k; // balanced: only `reference` remains
    }
    EXPECT_GT(failures, 10u); EXPECT_GE(successes, 1u);
    reference.value.reset(); EXPECT_EQ(f->budget->reserved(), base);

    // Codec failure: scratch far below the codec's need fails cleanly.
    EncodeOptions tiny; tiny.scratchOverride = 100;
    auto r = encode(f->grid, f->part.value, *f->budget, {}, tiny);
    EXPECT_EQ(r.outcome.status, Status::failed); EXPECT_EQ(f->budget->reserved(), base);

    // Inconsistent grid/partition/dpi/profile refuse before allocating.
    auto expectRefused = [&](FinalGrid &g, Partition const &p) {
        auto res = encode(g, p, *f->budget);
        EXPECT_EQ(res.outcome.status, Status::incompatible); EXPECT_EQ(f->budget->reserved(), base);
    };
    auto other = make(11, 8, image(11, 8, 30, 4));
    expectRefused(f->grid, other->part.value);  // partition and grid sizes differ
    auto badDpi = make(10, 8, image(10, 8, 30, 4), {}, 0, 300); expectRefused(badDpi->grid, badDpi->part.value);
    auto nan = make(10, 8, image(10, 8, 30, 4), {}, std::nan(""), 300); expectRefused(nan->grid, nan->part.value);
    auto huge = make(10, 8, image(10, 8, 30, 4), {}, 1e12, 300); expectRefused(huge->grid, huge->part.value);
    auto shortProfile = make(10, 8, image(10, 8, 30, 4), Bytes(10, 1)); expectRefused(shortProfile->grid, shortProfile->part.value);
    FinalGrid empty; Partition none; expectRefused(empty, none);
    // Lower-only maxCropPixels equal to B passes, one less refuses (B boundary).
    auto probe = encode(f->grid, f->part.value, *f->budget); ASSERT_TRUE(probe.ok());
    EncodeOptions c; c.maxCropPixels = probe.value.cropArea; EXPECT_TRUE(encode(f->grid, f->part.value, *f->budget, {}, c).ok());
    c.maxCropPixels -= 1; EXPECT_FALSE(encode(f->grid, f->part.value, *f->budget, {}, c).ok());
}

TEST(ExplodeBitmapPng, T07FailedShrinkRetainsLiveReservation)
{
    auto f = make(128, 128, Bytes(128 * 128 * 4, 255));
    ASSERT_EQ(f->part.value.pieceCount, 1u);
    auto base = f->budget->reserved();
    AllocationFault probe; EncodeOptions o; o.fault = &probe;
    auto reference = encode(f->grid, f->part.value, *f->budget, {}, o); ASSERT_TRUE(reference.ok());
    // The final allocation attempt is the piece's shrinking realloc, after all libpng frees.
    AllocationFault fault; fault.failAt = probe.attempts; o.fault = &fault;
    auto r = encode(f->grid, f->part.value, *f->budget, {}, o); ASSERT_TRUE(r.ok());
    ASSERT_EQ(fault.attempts, fault.failAt);
    auto const &e = r.value.piece(0);
    auto live = worstPngBytes(e.width, e.height, 0);
    EXPECT_GT(live, e.size * 10);
    EXPECT_EQ(r.value.encodedBytes, e.size); EXPECT_EQ(r.consumed, e.size);
    EXPECT_EQ(r.value.hrefBytes, 22 + 4 * ceilDiv3(e.size));
    EXPECT_EQ(f->budget->reserved(Stage::png), live + reference.value.encodedBytes + 2 * sizeof(EncodedPiece));
    EXPECT_EQ(std::memcmp(e.data, reference.value.piece(0).data, e.size), 0);
    verifyAll(*f, r.value, false);
    // Retained worst-case capacity must prevent an allocation beyond the admission limit.
    Budget::Token extra;
    EXPECT_FALSE(f->budget->acquire(Stage::prepared, f->budget->limit() - f->budget->reserved() + 1, extra).ok());
    reference.value.reset(); r.value.reset(); EXPECT_EQ(f->budget->reserved(), base);
}

TEST(ExplodeBitmapPng, T23StrictAncillaryCrcAndIndependentChunkOrder)
{
    auto f = make(8, 8, lAndDot(), srgb());
    auto enc = encode(f->grid, f->part.value, *f->budget); ASSERT_TRUE(enc.ok());
    auto const &e = enc.value.piece(0); ASSERT_TRUE(orderedPng(e.data, e.size));
    auto list = chunks(e.data, e.size);
    for (auto [at, n] : list) {
        if (std::memcmp(e.data + at + 4, "iCCP", 4) && std::memcmp(e.data + at + 4, "pHYs", 4)) continue;
        Bytes corrupt(e.data, e.data + e.size); corrupt[at + n - 1] ^= 1; // ancillary CRC only
        Read r; readPng(corrupt.data(), corrupt.size(), r); EXPECT_FALSE(r.ok);
    }
    // Move intact pHYs (including its valid CRC) after IDAT: framing remains valid, order must fail.
    Bytes reordered(e.data, e.data + 8);
    std::pair<std::size_t, std::size_t> phys{};
    for (auto c : list) if (!std::memcmp(e.data + c.first + 4, "pHYs", 4)) phys = c;
    ASSERT_NE(phys.second, 0u);
    for (auto [at, n] : list) {
        if (at == phys.first) continue;
        if (!std::memcmp(e.data + at + 4, "IEND", 4))
            reordered.insert(reordered.end(), e.data + phys.first, e.data + phys.first + phys.second);
        reordered.insert(reordered.end(), e.data + at, e.data + at + n);
    }
    EXPECT_FALSE(orderedPng(reordered.data(), reordered.size()));
    Read r; readPng(reordered.data(), reordered.size(), r); EXPECT_FALSE(r.ok);
}

TEST(ExplodeBitmapPng, T12DeterministicStopAndSharedWorkDuringBleedAndEncode)
{
    auto f = make(2048, 128, image(2048, 128, 100, 7, true));
    auto flag = std::make_shared<std::atomic<bool>>(true);
    auto base = f->budget->reserved();
    auto r = encode(f->grid, f->part.value, *f->budget, Stop(flag));
    EXPECT_EQ(r.outcome.status, Status::canceled); EXPECT_EQ(f->budget->reserved(), base);
    struct Hook {
        EncodePhase target; std::atomic<bool> &flag; JobWork &work; bool exhaust;
        bool fired = false; std::uint64_t visits = 0;
        std::chrono::steady_clock::time_point raised;
        static void observe(EncodePhase phase, std::uint64_t n, void *data) noexcept {
            auto &h = *static_cast<Hook *>(data);
            if (phase != h.target || n < 4096 || h.fired) return;
            h.fired = true; h.visits = h.work.visits(); h.raised = std::chrono::steady_clock::now();
            if (h.exhaust) h.work.advance(h.work.limit() - h.work.visits());
            else h.flag.store(true, std::memory_order_release);
        }
    };
    // Hollow connected piece exercises the expensive neighbour path in bleed, not just opaque skips.
    Bytes hollow(2048 * 128 * 4, 0);
    for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 2048; ++x)
        if (!x || !y || x == 2047 || y == 127) hollow[(y * 2048 + x) * 4 + 3] = 255;
    auto g = make(2048, 128, hollow);
    for (auto phase : {EncodePhase::bleed, EncodePhase::encode}) for (bool exhaust : {false, true}) {
        SCOPED_TRACE(int(phase));
        SCOPED_TRACE(exhaust);
        auto &fx = phase == EncodePhase::bleed ? *g : *f;
        flag->store(false); JobWork work(std::uint64_t(fx.w) * fx.h);
        Hook hook{phase, *flag, work, exhaust}; EncodeOptions o;
        o.work = &work; o.observe = Hook::observe; o.observerData = &hook;
        auto before = fx.budget->reserved();
        auto res = encode(fx.grid, fx.part.value, *fx.budget, Stop(flag), o);
        auto done = std::chrono::steady_clock::now();
        ASSERT_TRUE(hook.fired); EXPECT_GT(hook.visits, 0u);
        EXPECT_EQ(res.outcome.status, exhaust ? Status::failed : Status::canceled);
        EXPECT_EQ(res.value.count(), 0u); EXPECT_EQ(fx.budget->reserved(), before);
        // Elapsed cleanup time is a measurement, not a scheduler-dependent assertion.
        if (!exhaust) RecordProperty(phase == EncodePhase::bleed ? "bleed_ack_us" : "encode_ack_us",
            std::chrono::duration_cast<std::chrono::microseconds>(done - hook.raised).count());
        if (!exhaust) EXPECT_EQ(work.visits(), hook.visits); // Stop caught at the very same poll
    }
}

TEST(ExplodeBitmapPng, T12PollCountsForWideRowsAndLargeProfile)
{
    auto profile = cmsCreate_sRGBProfile();
    Bytes payload(3 * MiB); std::uint32_t seed = 17;
    for (auto &v : payload) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; v = seed; }
    ASSERT_TRUE(cmsWriteRawTag(profile, static_cast<cmsTagSignature>(0x74657374), payload.data(), payload.size()));
    auto icc = profileBytes(profile);
    auto f = make(16384, 16, image(16384, 16, 100, 7, true), icc);
    struct PollCounts {
        std::array<std::uint64_t, 3> lastVisits{}, charged{}, polls{}, maxStep{};
        EncodePhase phase = EncodePhase::crop;
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration gap{};
        static void observe(EncodePhase phase, std::uint64_t visits, void *p) noexcept {
            auto &t = *static_cast<PollCounts *>(p); auto i = unsigned(phase);
            auto step = visits >= t.lastVisits[i] ? visits - t.lastVisits[i] : visits; // phase re-entry resets count
            t.lastVisits[i] = visits; t.charged[i] += step; ++t.polls[i];
            t.maxStep[i] = std::max(t.maxStep[i], step);
            auto now = std::chrono::steady_clock::now();
            if (t.phase == phase && phase != EncodePhase::crop) t.gap = std::max(t.gap, now - t.last);
            t.phase = phase; t.last = now;
        }
    } timing;
    JobWork work(std::uint64_t(f->w) * f->h); EncodeOptions o;
    o.work = &work; o.observe = PollCounts::observe; o.observerData = &timing;
    auto enc = encode(f->grid, f->part.value, *f->budget, {}, o); ASSERT_TRUE(enc.ok()) << enc.outcome.diagnostic;
    auto crop = unsigned(EncodePhase::crop), bleed = unsigned(EncodePhase::bleed), codec = unsigned(EncodePhase::encode);
    EXPECT_GT(timing.charged[crop], std::uint64_t(f->w) * f->h);
    EXPECT_GT(timing.charged[bleed], std::uint64_t(f->w) * f->h);
    EXPECT_GE(timing.charged[codec], icc.size() + std::uint64_t(f->w + 2) * (f->h + 2));
    EXPECT_LE(timing.maxStep[crop], 16384u + 255); // clear/copy blocks + pending visits
    EXPECT_LE(timing.maxStep[bleed], 256u); // includes neighbour visits and opaque skips
    EXPECT_LE(timing.maxStep[codec], f->w + 2u + 255); // profile blocks or one row
    EXPECT_GE(timing.polls[bleed], timing.charged[bleed] / 256);
    EXPECT_GE(timing.polls[codec], (icc.size() + 16383) / 16384 + f->h + 2);
    EXPECT_EQ(work.visits(), timing.charged[crop] + timing.charged[bleed] + timing.charged[codec]);
    Read r; auto const &e = enc.value.piece(0); readPng(e.data, e.size, r);
    ASSERT_TRUE(r.ok); EXPECT_EQ(r.icc, icc);
    // Diagnostic measurement only: includes scheduler delays and never gates correctness.
    RecordProperty("measurement_max_poll_gap_us", std::chrono::duration_cast<std::chrono::microseconds>(timing.gap).count());
}

TEST(ExplodeBitmapPng, T25WholeGridExactSamplesIncludingInvisibleRGBAndProfile) {
    for (auto icc : {Bytes{}, srgb(), greyProfile()}) {
        auto rgba = image(32, 8, 30, 19); bool grey = !icc.empty() && icc[16] == 'G';
        if (grey) for (unsigned i = 0; i < rgba.size(); i += 4) rgba[i+1] = rgba[i+2] = rgba[i];
        auto f = make(32, 8, rgba, icc); auto baseline = f->budget->reserved();
        auto encoded = encodeWholeGrid(f->grid, *f->budget); ASSERT_TRUE(encoded.ok()) << encoded.outcome.diagnostic;
        ASSERT_EQ(encoded.value.count(), 1u); auto const &piece = encoded.value.piece(0);
        EXPECT_EQ(piece.x, 0); EXPECT_EQ(piece.y, 0); EXPECT_EQ(encoded.value.cropArea, 256u);
        Read read; readPng(piece.data, piece.size, read); ASSERT_TRUE(read.ok);
        EXPECT_EQ(read.w, 32u); EXPECT_EQ(read.h, 8u); EXPECT_EQ(read.rgba, rgba); EXPECT_EQ(read.icc, icc);
        EXPECT_EQ(read.colourType, grey ? PNG_COLOR_TYPE_GRAY_ALPHA : PNG_COLOR_TYPE_RGBA);
        EXPECT_FALSE(read.gama || read.chrm || read.srgbChunk);
        encoded.value.reset(); EXPECT_EQ(f->budget->reserved(), baseline);
    }
}
TEST(ExplodeBitmapPng, T25WholeGridValidationBudgetFaultsAndStop) {
    auto f = make(64, 32, image(64, 32, 40, 7), srgb()); auto baseline = f->budget->reserved();
    for (unsigned mode = 0; mode < 7; ++mode) {
        EncodeOptions opt;
        if (mode == 0) opt.encoders = 3;
        if (mode == 1) opt.maxHref = 1;
        if (mode == 2) opt.maxCropPixels = 2047;
        if (mode == 3) { opt.scratchOverride = UINT64_MAX; opt.encoders = 2; }
        if (mode == 4) opt.scratchOverride = 1;
        if (mode == 5) f->grid.dpiX = 0;
        if (mode == 6) f->grid.width = UINT32_MAX;
        EXPECT_FALSE(encodeWholeGrid(f->grid, *f->budget, {}, opt).ok()); EXPECT_EQ(f->budget->reserved(), baseline);
        f->grid.dpiX = 300; f->grid.width = 64;
    }
    Budget small(Budget::FixedLimitForTest{}, 1); EXPECT_FALSE(encodeWholeGrid(f->grid, small).ok()); EXPECT_EQ(small.reserved(), 0u);
    AllocationFault probe; EncodeOptions opt; opt.fault = &probe;
    { auto r = encodeWholeGrid(f->grid, *f->budget, {}, opt); ASSERT_TRUE(r.ok()); }
    for (unsigned i = 1; i <= probe.attempts; ++i) {
        AllocationFault fault{i}; opt.fault = &fault;
        { auto r = encodeWholeGrid(f->grid, *f->budget, {}, opt);
          if (i == probe.attempts) { ASSERT_TRUE(r.ok()); EXPECT_GE(f->budget->reserved(Stage::png), worstPngBytes(64, 32, f->grid.profile.size())); }
          else EXPECT_FALSE(r.ok()); }
        EXPECT_EQ(f->budget->reserved(), baseline);
    }
    auto flag = std::make_shared<std::atomic<bool>>(true);
    EXPECT_EQ(encodeWholeGrid(f->grid, *f->budget, Stop(flag)).outcome.status, Status::canceled);
    flag->store(false); opt = {}; opt.observerData = flag.get();
    opt.observe = [](EncodePhase phase, std::uint64_t n, void *p) noexcept {
        if (phase == EncodePhase::encode && n >= 64) static_cast<std::atomic<bool> *>(p)->store(true);
    };
    EXPECT_EQ(encodeWholeGrid(f->grid, *f->budget, Stop(flag), opt).outcome.status, Status::canceled);
    JobWork work(2048); work.advance(work.limit()); opt = {}; opt.work = &work;
    EXPECT_FALSE(encodeWholeGrid(f->grid, *f->budget, {}, opt).ok()); EXPECT_EQ(f->budget->reserved(), baseline);
    auto mismatch = make(8, 8, image(8, 8, 50, 2), greyProfile());
    EXPECT_FALSE(encodeWholeGrid(mismatch->grid, *mismatch->budget).ok());
    f->grid.profile.data()[36] = std::byte{0}; EXPECT_FALSE(encodeWholeGrid(f->grid, *f->budget).ok());
    EXPECT_EQ(f->budget->reserved(), baseline);
}

TEST(ExplodeBitmapPng, OneHundredFiftyFullHeightStripsUseOnlyTheirOwnRunsAndMatchOracle)
{
    constexpr unsigned w=5000, h=5000;
    Bytes rgba(std::size_t(w)*h*4,0);
    for (unsigned y=0;y<h;++y) for (unsigned i=0;i<150;++i) {
        unsigned x=16+33*i;
        for (unsigned c=0;c<3;++c) rgba[(std::size_t(y)*w+x)*4+c]=colour(x,y,c);
        rgba[(std::size_t(y)*w+x)*4+3]=255;
    }
    JobWork work(std::uint64_t(w)*h);
    auto f=make(w,h,rgba,{},96,96,&work);
    auto attached=attach(f->part.value,OrthogonalMetric::fromDpi(96,96).value,*f->budget,work);
    ASSERT_TRUE(attached.ok()) << attached.outcome.diagnostic;
    f->part=std::move(attached); ASSERT_EQ(f->part.value.pieceCount,150u);
    EncodeOptions options; options.work=&work;
    auto result=encode(f->grid,f->part.value,*f->budget,{},options);
    ASSERT_TRUE(result.ok()) << result.outcome.diagnostic;
    EXPECT_EQ(work.limit(),300000000u); EXPECT_LT(work.visits(),work.limit());
    std::cout << "EB-REAL1 strips visits=" << work.visits() << " limit=" << work.limit() << std::endl;
    verifyAll(*f,result.value,false);
}
