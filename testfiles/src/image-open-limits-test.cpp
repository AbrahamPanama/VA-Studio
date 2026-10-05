// SPDX-License-Identifier: GPL-2.0-or-later
// BUG-015: real document opens, bounded allocation, and legacy display parity.
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <zlib.h>
#include <tiffio.h>
#include "display/cairo-utils.h"
#include "document.h"
#include "object/sp-image.h"
#include "util/bitmap-memory-admission.h"
#include "xml/node.h"
#include "xml/repr.h"
#ifdef _WIN32
// Last, after the project headers: the <windows.h> macros collide with gtkmm enums.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2 // K32GetProcessMemoryInfo in kernel32: no extra import library
#endif
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
namespace Inkscape {
std::uint64_t image_open_memory_budget(Bitmap::Memory const &);
std::uint64_t image_open_memory_budget(Bitmap::Result<Bitmap::Memory> const &);
bool image_open_dimensions_fit(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
bool image_open_base64_capacity(std::size_t, std::uint64_t, std::size_t &);
guchar *image_open_decode_base64(std::string_view, std::uint64_t, gsize &, bool);
char const *image_open_diagnostic();
void image_open_reset();
}
namespace {
using Bytes = std::vector<guchar>;
void put(Bytes &b, std::size_t at, unsigned n, int count, bool little = true) {
    for (int i = 0; i < count; ++i) b.at(at + i) = n >> (8 * (little ? i : count - i - 1));
}
Bytes fixture(std::string const &format, int w = 4, int h = 4) {
    if (format == "xpm") { std::string text = "/* XPM */\nstatic char *image[] = {\n\"4 4 1 1\",\n\"a c #c8141e\",\n\"aaaa\",\"aaaa\",\"aaaa\",\"aaaa\"};\n"; return Bytes(text.begin(),text.end()); }
    if (format == "gif") return {71,73,70,56,57,97,1,0,1,0,128,0,0,0,0,0,255,255,255,33,249,4,1,0,0,0,0,44,0,0,0,0,1,0,1,0,0,2,2,68,1,0,59};
    if (format == "bmp") {
        Bytes b(54 + 4 * 4 * 3, 0); b[0] = 'B'; b[1] = 'M';
        put(b,2,b.size(),4); put(b,10,54,4); put(b,14,40,4); put(b,18,4,4); put(b,22,4,4); put(b,26,1,2); put(b,28,24,2);
        for (std::size_t p = 54; p < b.size(); p += 3) { b[p] = 30; b[p+1] = 20; b[p+2] = 200; } return b;
    }
    auto pix = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, w, h); gdk_pixbuf_fill(pix, 0xc8141eff);
    gchar *data = nullptr; gsize len = 0; GError *error = nullptr;
    bool ok = gdk_pixbuf_save_to_buffer(pix, &data, &len, format.c_str(), &error, nullptr);
    EXPECT_TRUE(ok) << (error ? error->message : ""); g_clear_error(&error); g_object_unref(pix);
    Bytes b; if (data) b.assign(data, data + len); g_free(data); return b;
}
Bytes bomb(std::string const &format) {
    auto b = fixture(format);
    if (format == "xpm") { std::string text = "/* XPM */\nstatic char *image[] = {\n/* decoy \"1 1 1 1\" */\n\"100000 100000 1 1\",\n\"a c red\",\n\"a\"};\n"; return Bytes(text.begin(),text.end()); }
    if (format == "png") { put(b,16,100000,4,false); put(b,20,100000,4,false); put(b,29,crc32(0,b.data()+12,17),4,false); }
    if (format == "jpeg") for (std::size_t i = 0; i+10 < b.size(); ++i) if (b[i] == 255 && (b[i+1] == 0xc0 || b[i+1] == 0xc2)) { put(b,i+5,65535,2,false); put(b,i+7,65535,2,false); break; }
    if (format == "gif") { put(b,6,65535,2); put(b,8,65535,2); put(b,32,65535,2); put(b,34,65535,2); }
    if (format == "bmp") { put(b,18,100000,4); put(b,22,100000,4); }
    return b;
}
std::string uri(Bytes const &b, std::string const &format) {
    auto encoded = g_base64_encode(b.data(), b.size()); auto s = "data:image/" + format + ";base64," + encoded; g_free(encoded); return s;
}
// Linked hrefs are file URIs: a raw Windows path (C:\...) parses as a URI with scheme "c" and never reaches the
// file loader, which was already true before BUG-015.
std::string linked_href(std::string const &path) {
    gchar *u = g_filename_to_uri(path.c_str(), nullptr, nullptr); std::string s = u ? u : ""; g_free(u); return s;
}
std::unique_ptr<SPDocument> document(std::string const &href) {
    auto s = "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'><image id='bomb-image' width='20' height='20' xlink:href='" + href + "'/><rect id='intact' width='5' height='5'/></svg>";
    auto d = SPDocument::createNewDocFromMem(s); if (d) d->ensureUpToDate(); return d;
}
std::uint64_t peak_rss() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS r{};
    EXPECT_TRUE(GetProcessMemoryInfo(GetCurrentProcess(), &r, sizeof(r)));
    return r.PeakWorkingSetSize;
#else
    rusage r{}; EXPECT_EQ(getrusage(RUSAGE_SELF, &r), 0);
#ifdef __APPLE__
    return r.ru_maxrss;
#else
    return r.ru_maxrss * 1024;
#endif
#endif
}
struct Logs { std::vector<std::string> messages; guint handler;
    Logs() : handler(g_log_set_handler(nullptr, G_LOG_LEVEL_WARNING, +[](gchar const *, GLogLevelFlags, gchar const *m, gpointer p) {
        static_cast<Logs *>(p)->messages.emplace_back(m);
    }, this)) {}
    ~Logs() { g_log_remove_handler(nullptr, handler); }
};
void parity(Bytes const &b, std::string const &format, bool truncated = false) {
    auto loader = gdk_pixbuf_loader_new(); GError *error = nullptr;
    ASSERT_TRUE(gdk_pixbuf_loader_write(loader,b.data(),b.size(),&error)); g_clear_error(&error);
    gdk_pixbuf_loader_close(loader,&error); g_clear_error(&error);
    auto reference = gdk_pixbuf_loader_get_pixbuf(loader); ASSERT_NE(reference,nullptr);
    auto oriented = gdk_pixbuf_apply_embedded_orientation(reference); reference = oriented;
    Inkscape::Pixbuf expected(reference); // includes the legacy alpha expansion
    expected.ensurePixelFormat(Inkscape::Pixbuf::PF_CAIRO); // SPImage stores Cairo pixels
    for (bool linked : {false,true}) {
        auto href = uri(b,format); std::string path;
        if (linked) { gchar *name = nullptr; int fd = g_file_open_tmp("bug015-XXXXXX",&name,nullptr); ASSERT_GE(fd,0); g_close(fd,nullptr); path = name; g_free(name); ASSERT_TRUE(g_file_set_contents(path.c_str(),reinterpret_cast<char const *>(b.data()),b.size(),nullptr)); href = linked_href(path); }
        Logs logs; auto d = document(href); ASSERT_NE(d,nullptr); auto image = dynamic_cast<SPImage *>(d->getObjectById("bomb-image")); ASSERT_NE(image,nullptr); ASSERT_FALSE(image->missing); ASSERT_NE(image->pixbuf,nullptr);
        auto actual = image->pixbuf->getPixbufRaw(); auto ref = expected.getPixbufRaw(false); ASSERT_EQ(gdk_pixbuf_get_width(actual),gdk_pixbuf_get_width(ref)); ASSERT_EQ(gdk_pixbuf_get_height(actual),gdk_pixbuf_get_height(ref));
        for (int y = 0; y < gdk_pixbuf_get_height(ref); ++y) ASSERT_EQ(std::memcmp(gdk_pixbuf_get_pixels(actual)+y*gdk_pixbuf_get_rowstride(actual),gdk_pixbuf_get_pixels(ref)+y*gdk_pixbuf_get_rowstride(ref),gdk_pixbuf_get_width(ref)*4),0);
        EXPECT_STREQ(image->getRepr()->attribute("xlink:href"),href.c_str()); if (truncated) { ASSERT_EQ(logs.messages.size(),1); EXPECT_NE(logs.messages[0].find("truncated"),std::string::npos); }
        if (linked) g_unlink(path.c_str());
    }
    g_object_unref(loader);
}
}
TEST(ImageOpenLimits, HugeHeadersEmbeddedAndLinkedRefuseWithoutLargeAllocationAndPreserveDocument) {
    ASSERT_NE(document(uri(fixture("png"),"png")),nullptr); // warm up the placeholder/document machinery
    for (auto format : {"png","jpeg","gif","bmp","xpm"}) for (bool linked : {false,true}) {
        SCOPED_TRACE(std::string(format)+(linked ? " linked" : " embedded")); auto b = bomb(format); auto href = uri(b,format); std::string path;
        if (linked) { gchar *name = nullptr; int fd = g_file_open_tmp("bug015-bomb-XXXXXX",&name,nullptr); ASSERT_GE(fd,0); g_close(fd,nullptr); path = name; g_free(name); ASSERT_TRUE(g_file_set_contents(path.c_str(),reinterpret_cast<char const *>(b.data()),b.size(),nullptr)); href = linked_href(path); }
        auto before = peak_rss(); auto start = std::chrono::steady_clock::now(); Logs logs; auto d = document(href); ASSERT_NE(d,nullptr); auto image = dynamic_cast<SPImage *>(d->getObjectById("bomb-image")); ASSERT_NE(image,nullptr); EXPECT_TRUE(image->missing); EXPECT_NE(image->pixbuf,nullptr);
        EXPECT_LT(peak_rss()-before,64*Inkscape::Bitmap::MiB); EXPECT_LT(std::chrono::steady_clock::now()-start,std::chrono::seconds(3));
        ASSERT_EQ(logs.messages.size(),1); EXPECT_NE(logs.messages[0].find("bomb-image"),std::string::npos); EXPECT_NE(logs.messages[0].find("limit"),std::string::npos);
        EXPECT_STREQ(image->getRepr()->attribute("xlink:href"),href.c_str()); EXPECT_NE(d->getObjectById("intact"),nullptr);
        // Serializing and reopening retains the refused source rather than dropping it.
        auto serialized = sp_repr_save_buf(d->getReprDoc()); ASSERT_NE(serialized.find(href),std::string::npos); auto reopened = SPDocument::createNewDocFromMem(serialized.raw()); ASSERT_NE(reopened,nullptr);
        if (linked) g_unlink(path.c_str());
    }
}
TEST(ImageOpenLimits, Base64RefusedBeforeDecodeAndArithmeticNeverWraps) {
    auto budget = 18 * Inkscape::Bitmap::MiB; std::string payload(2*Inkscape::Bitmap::MiB,'A'); gsize len = 777;
    auto before = peak_rss(); auto data = Inkscape::image_open_decode_base64(payload,budget,len,true); EXPECT_EQ(data,nullptr); EXPECT_EQ(len,777); EXPECT_LT(peak_rss()-before,Inkscape::Bitmap::MiB);
    std::size_t capacity = 0; EXPECT_FALSE(Inkscape::image_open_base64_capacity(SIZE_MAX,UINT64_MAX,capacity));
    Inkscape::Bitmap::Memory m{32ull*1024*Inkscape::Bitmap::MiB,4ull*1024*Inkscape::Bitmap::MiB,1,true,0}; EXPECT_EQ(Inkscape::image_open_memory_budget(m),m.available-256*Inkscape::Bitmap::MiB); EXPECT_TRUE(Inkscape::image_open_dimensions_fit(14400,21600,100*Inkscape::Bitmap::MiB,Inkscape::image_open_memory_budget(m))); m.measured = false; EXPECT_EQ(Inkscape::image_open_memory_budget(m),0u);
}
TEST(ImageOpenLimits, ValidAndTruncatedPixelsMatchLegacyDecodeAndKeepHref) {
    for (auto format : {"png","jpeg","gif","bmp","tiff","xpm"}) { SCOPED_TRACE(format); parity(fixture(format),format); }
    auto b = fixture("png"); b.resize(b.size()-12); parity(b,"png",true);
    auto cropped = fixture("gif"); cropped[32] = 2; cropped[39] = 4; cropped[40] = 10; parity(cropped,"gif");
    auto animated = fixture("gif"); Bytes second(animated.begin()+27,animated.end()-1); animated.insert(animated.end()-1,second.begin(),second.end()); parity(animated,"gif");
}
TEST(ImageOpenLimits, LegitimateWideImageBeyondExplodeAxisLimitOpens) {
    // Real pixels, not merely an admission calculation: 20000 x 256, >16K axis.
    parity(fixture("png",20000,256),"png");
}
TEST(ImageOpenLimits, TinyLogicalScreenCannotHideHugeGifFrame) {
    auto b = bomb("gif"); put(b,6,1,2); put(b,8,1,2); Logs logs; auto before = peak_rss(); auto d = document(uri(b,"gif"));
    ASSERT_NE(d,nullptr); auto image = dynamic_cast<SPImage *>(d->getObjectById("bomb-image")); ASSERT_NE(image,nullptr); EXPECT_TRUE(image->missing); EXPECT_LT(peak_rss()-before,64*Inkscape::Bitmap::MiB);
    ASSERT_EQ(logs.messages.size(),1); EXPECT_NE(logs.messages[0].find("limit"),std::string::npos);
}

TEST(ImageOpenLimits, R3SharedAdmissionRefusalSurvivesOpeningBufferAndDimensionGates) {
    using namespace Inkscape; using namespace Inkscape::Bitmap;
    image_open_reset();
    Memory low{4096*MiB, 200*MiB, 500*MiB, true};
    auto budget = image_open_memory_budget(low); ASSERT_EQ(budget, 0u);
    std::string expected = "Not enough memory: OS headroom / recovery reserve; estimated need 256.00 MiB, available 200.00 MiB.";
    EXPECT_STREQ(image_open_diagnostic(), expected.c_str());
    auto payload = uri(fixture("png"), "png");
    auto base64 = std::string_view(payload).substr(payload.find(',') + 1);
    gsize len = 777;
    EXPECT_EQ(image_open_decode_base64(base64, budget, len, true), nullptr);
    EXPECT_EQ(len, 777u); EXPECT_STREQ(image_open_diagnostic(), expected.c_str());
    EXPECT_FALSE(image_open_dimensions_fit(4, 4, 100, budget));
    EXPECT_STREQ(image_open_diagnostic(), expected.c_str());
    image_open_reset();
    struct Critical : MemoryProbe {
        bool read(RawMemory &out) const noexcept override {
            VmStats vm{4096*MiB,0,0,0,0,500*MiB,4}; return fromVmStats(vm, out);
        }
    } critical;
    auto sample = sampleMemory(critical); ASSERT_FALSE(sample.ok());
    budget = image_open_memory_budget(sample); ASSERT_EQ(budget, 0u);
    expected = sample.outcome.diagnostic;
    EXPECT_NE(expected.find("critical macOS memory pressure"), std::string::npos);
    EXPECT_EQ(image_open_decode_base64(base64, budget, len, true), nullptr);
    EXPECT_STREQ(image_open_diagnostic(), expected.c_str());
    image_open_reset();
    EXPECT_GT(image_open_memory_budget(Memory{4096*MiB,1024*MiB,500*MiB,true}), 0u);
    EXPECT_TRUE(image_open_dimensions_fit(4,4,100,768*MiB));
    EXPECT_STREQ(image_open_diagnostic(), "");
}

namespace {
// Generate source samples with libtiff, independently of the display decoder.
Bytes tiff_samples(unsigned bits, unsigned photo, bool alpha, bool associated,
                   bool planar, bool tiled, unsigned orientation, unsigned compression,
                   std::vector<std::uint16_t> const &values, unsigned width = 3, unsigned height = 2,
                   unsigned resolution_unit = RESUNIT_CENTIMETER, double x_resolution = 100.0, double y_resolution = 50.0)
{
    gchar *name = nullptr;
    int fd = g_file_open_tmp("bug024-XXXXXX", &name, nullptr);
    EXPECT_GE(fd, 0); if (fd < 0) return {};
    g_close(fd, nullptr);
    auto tif = TIFFOpen(name, "w");
    EXPECT_NE(tif, nullptr);
    if (!tif) { g_unlink(name); g_free(name); return {}; }
    unsigned channels = (photo == PHOTOMETRIC_RGB ? 3 : 1) + unsigned(alpha);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, channels);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, photo);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar ? PLANARCONFIG_SEPARATE : PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, orientation);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, compression);
    if (compression == COMPRESSION_LZW || compression == COMPRESSION_ADOBE_DEFLATE)
        TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
    if (alpha) { std::uint16_t type = associated ? EXTRASAMPLE_ASSOCALPHA : EXTRASAMPLE_UNASSALPHA; TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &type); }
    std::vector<std::uint16_t> r, g, b;
    if (photo == PHOTOMETRIC_PALETTE) {
        r.resize(1u << bits); g.resize(r.size()); b.resize(r.size());
        r[1] = 65535;
        if (r.size() > 2) { g[2] = 65535; b[3] = 65535; }
        TIFFSetField(tif, TIFFTAG_COLORMAP, r.data(), g.data(), b.data());
    }
    std::array<unsigned char, 8> profile{1,2,3,4,5,6,7,8};
    TIFFSetField(tif, TIFFTAG_ICCPROFILE, profile.size(), profile.data());
    TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, resolution_unit);
    TIFFSetField(tif, TIFFTAG_XRESOLUTION, x_resolution);
    TIFFSetField(tif, TIFFTAG_YRESOLUTION, y_resolution);
    unsigned bw = tiled ? 16 : width, bh = tiled ? 16 : 1;
    if (tiled) { TIFFSetField(tif, TIFFTAG_TILEWIDTH, bw); TIFFSetField(tif, TIFFTAG_TILELENGTH, bh); }
    else TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, bh);
    for (unsigned plane = 0; plane < (planar ? channels : 1); ++plane)
        for (unsigned y = 0; y < height; y += bh) for (unsigned x = 0; x < width; x += bw) {
            auto row = (bw * (planar ? 1 : channels) * bits + 7) / 8;
            Bytes block(row * bh, 0);
            for (unsigned yy = 0; yy < bh && y + yy < height; ++yy)
                for (unsigned xx = 0; xx < bw && x + xx < width; ++xx)
                    for (unsigned c = 0; c < (planar ? 1 : channels); ++c) {
                        auto v = values[((y + yy) * width + x + xx) * channels + (planar ? plane : c)];
                        auto index = xx * (planar ? 1 : channels) + c;
                        auto p = block.data() + yy * row;
                        if (bits == 16) std::memcpy(p + index * 2, &v, 2);
                        else if (bits == 8) p[index] = v;
                        else p[index * bits / 8] |= v << (8 - bits - index * bits % 8);
                    }
            auto result = tiled ? TIFFWriteEncodedTile(tif, TIFFComputeTile(tif,x,y,0,plane), block.data(), block.size())
                                : TIFFWriteEncodedStrip(tif, TIFFComputeStrip(tif,y,plane), block.data(), block.size());
            EXPECT_GT(result, 0);
        }
    TIFFClose(tif);
    gchar *data = nullptr; gsize size = 0;
    EXPECT_TRUE(g_file_get_contents(name, &data, &size, nullptr));
    Bytes result; if (data) result.assign(data, data + size);
    g_free(data); g_unlink(name); g_free(name); return result;
}
void expect_tiff(Bytes const &bytes, std::vector<unsigned> const &expected, unsigned width = 3, unsigned height = 2,
                 char const *x_dpi = "254", char const *y_dpi = "127")
{
    auto pix = std::unique_ptr<Inkscape::Pixbuf>(Inkscape::Pixbuf::create_from_buffer(
        std::string(reinterpret_cast<char const *>(bytes.data()), bytes.size())));
    ASSERT_NE(pix, nullptr) << Inkscape::image_open_diagnostic();
    auto raw = pix->getPixbufRaw();
    ASSERT_EQ(gdk_pixbuf_get_width(raw), width); ASSERT_EQ(gdk_pixbuf_get_height(raw), height);
    ASSERT_EQ(expected.size(), width * height * 4);
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width * 4; ++x)
        EXPECT_EQ(gdk_pixbuf_get_pixels(raw)[y * gdk_pixbuf_get_rowstride(raw) + x], expected[y * width * 4 + x]) << y << ':' << x;
    EXPECT_STREQ(gdk_pixbuf_get_option(raw,"icc-profile"), "AQIDBAUGBwg=");
    EXPECT_STREQ(gdk_pixbuf_get_option(raw,"x-dpi"), x_dpi);
    EXPECT_STREQ(gdk_pixbuf_get_option(raw,"y-dpi"), y_dpi);
}
}
TEST(ImageOpenLimits, TiffExactStraightAndAssociatedAcrossStorageAndPrecision)
{
    for (unsigned bits : {8,16}) for (bool alpha : {false,true}) for (bool associated : {false,true})
        for (bool planar : {false,true}) for (bool tiled : {false,true})
            for (unsigned compression : {COMPRESSION_NONE, COMPRESSION_LZW, COMPRESSION_ADOBE_DEFLATE, COMPRESSION_PACKBITS}) {
                if (!alpha && associated) continue;
                SCOPED_TRACE(::testing::Message() << bits << '/' << alpha << '/' << associated << '/' << planar << '/' << tiled << '/' << compression);
                unsigned scale = bits == 16 ? 257 : 1;
                std::vector<std::uint16_t> values;
                std::vector<unsigned> expected;
                for (unsigned i = 0; i < 6; ++i) {
                    // Includes low alpha and hidden RGB: no premultiply/undo can
                    // recover these straight samples from an 8-bit RGBA loader.
                    unsigned a = i == 0 ? 128 : i == 1 ? 1 : i == 2 ? 0 : 255;
                    std::array<unsigned,3> color = associated ? std::array<unsigned,3>{0,a,0} : std::array<unsigned,3>{17,255,93};
                    if (i == 0) color = {0, associated ? a : 255, 0};
                    for (auto c : color) values.push_back(c * scale);
                    if (alpha) values.push_back(a * scale);
                    for (unsigned c = 0; c < 3; ++c) expected.push_back(associated ? (a && c == 1 ? 255 : 0) : color[c]);
                    expected.push_back(alpha ? a : 255);
                }
                expect_tiff(tiff_samples(bits,PHOTOMETRIC_RGB,alpha,associated,planar,tiled,1,compression,values),expected);
            }
}
TEST(ImageOpenLimits, TiffGrayAlphaAndPalette)
{
    for (unsigned bits : {8,16}) for (unsigned photo : {PHOTOMETRIC_MINISBLACK,PHOTOMETRIC_MINISWHITE})
        for (bool alpha : {false,true}) for (bool associated : {false,true}) for (bool planar : {false,true}) for (bool tiled : {false,true}) {
            if (!alpha && associated) continue;
            unsigned scale = bits == 16 ? 257 : 1;
            std::vector<std::uint16_t> values; std::vector<unsigned> expected;
            for (unsigned i = 0; i < 6; ++i) {
                unsigned a = i == 0 ? 0 : 128, v = associated ? a : 73;
                values.push_back(v * scale); if (alpha) values.push_back(a * scale);
                unsigned color = associated ? (a ? 255 : 0) : v;
                if (photo == PHOTOMETRIC_MINISWHITE && (!associated || a)) color = 255 - color;
                expected.insert(expected.end(), {color,color,color,alpha ? a : 255});
            }
            expect_tiff(tiff_samples(bits,photo,alpha,associated,planar,tiled,1,COMPRESSION_LZW,values),expected);
        }
    for (unsigned bits : {2,4,8,16}) for (bool tiled : {false,true}) {
        expect_tiff(tiff_samples(bits,PHOTOMETRIC_PALETTE,false,false,false,tiled,1,COMPRESSION_NONE,{1,2,3,3,2,1}),
                    {255,0,0,255, 0,255,0,255, 0,0,255,255, 0,0,255,255, 0,255,0,255, 255,0,0,255});
    }
}
TEST(ImageOpenLimits, TiffAllEightOrientations)
{
    // TIFF 3x2 source: 1 2 3 / 4 5 6. Expected visual order is explicit.
    std::array<std::array<unsigned,6>,8> order{{{1,2,3,4,5,6},{3,2,1,6,5,4},{6,5,4,3,2,1},{4,5,6,1,2,3},
                                              {1,4,2,5,3,6},{4,1,5,2,6,3},{6,3,5,2,4,1},{3,6,2,5,1,4}}};
    for (unsigned orientation = 1; orientation <= 8; ++orientation) {
        SCOPED_TRACE(orientation);
        std::vector<unsigned> expected;
        for (auto v : order[orientation-1]) expected.insert(expected.end(),{v,v,v,255});
        expect_tiff(tiff_samples(8,PHOTOMETRIC_MINISBLACK,false,false,false,false,orientation,COMPRESSION_NONE,{1,2,3,4,5,6}),
                    expected,orientation < 5 ? 3 : 2,orientation < 5 ? 2 : 3,
                    orientation < 5 ? "254" : "127",orientation < 5 ? "127" : "254");
    }
}
TEST(ImageOpenLimits, TiffOrientationSixSwapsUnequalInchDpi)
{
    // The stored and oriented rasters both describe a 2 x 2 inch image.
    std::vector<std::uint16_t> values(600 * 300, 73);
    std::vector<unsigned> expected;
    expected.reserve(300 * 600 * 4);
    for (unsigned i = 0; i < 300 * 600; ++i) expected.insert(expected.end(), {73,73,73,255});
    expect_tiff(tiff_samples(8,PHOTOMETRIC_MINISBLACK,false,false,false,false,6,COMPRESSION_LZW,
                             values,600,300,RESUNIT_INCH,300.0,150.0),
                expected,300,600,"150","300");
}
TEST(ImageOpenLimits, TiffLegacyBilevelOrientationSwapsDpi)
{
    // Unsupported packed grayscale exercises the legacy TIFF fallback too.
    expect_tiff(tiff_samples(1,PHOTOMETRIC_MINISBLACK,false,false,false,false,6,COMPRESSION_NONE,{1,0,1,0,1,0}),
                {0,0,0,255, 255,255,255,255, 255,255,255,255, 0,0,0,255, 0,0,0,255, 255,255,255,255},
                2,3,"127","254");
}
TEST(ImageOpenLimits, TiffAssociatedUsesSixteenBitPrecisionAndRounds)
{
    expect_tiff(tiff_samples(16,PHOTOMETRIC_RGB,true,true,true,true,1,COMPRESSION_ADOBE_DEFLATE,
        {1,2,3,3, 16384,8192,32768,32768, 1,0,0,0, 1,2,3,3, 16384,8192,32768,32768, 1,0,0,0}),
        {85,170,255,0, 128,64,255,128, 0,0,0,0, 85,170,255,0, 128,64,255,128, 0,0,0,0});
}
TEST(ImageOpenLimits, TiffHugeDimensionsRefuseBeforePixels)
{
    auto bytes = tiff_samples(8,PHOTOMETRIC_RGB,true,false,false,false,1,COMPRESSION_NONE,
                             {0,255,0,128, 0,255,0,128, 0,255,0,128, 0,255,0,128, 0,255,0,128, 0,255,0,128});
    // libtiff's native-endian test output: replace the SHORT/LONG width and
    // height values in the IFD, leaving a valid directory and tiny raster.
    bool little = bytes[0] == 'I';
    auto get = [&](unsigned p, unsigned n) { unsigned v=0; for (unsigned i=0;i<n;++i) v |= unsigned(bytes.at(p+i)) << (8*(little?i:n-1-i)); return v; };
    auto ifd = get(4,4); auto entries = get(ifd,2);
    for (unsigned i=0;i<entries;++i) { auto p=ifd+2+i*12; auto tag=get(p,2); if (tag==256 || tag==257) { put(bytes,p+2,4,2,little); put(bytes,p+8,100000,4,little); } }
    Logs logs;
    auto pix = std::unique_ptr<Inkscape::Pixbuf>(Inkscape::Pixbuf::create_from_buffer(std::string(reinterpret_cast<char const *>(bytes.data()),bytes.size())));
    EXPECT_EQ(pix,nullptr); EXPECT_NE(std::string(Inkscape::image_open_diagnostic()).find("limit"),std::string::npos);
}

TEST(ImageOpenLimits, TiffMultipleTilesAndPackedPaletteAndLegacyBilevel)
{
    // An earlier caller's diagnostic must not short-circuit this attempt's
    // legacy fallback. Only a refusal in this decode may prevent fallback.
    EXPECT_FALSE(Inkscape::image_open_dimensions_fit(100000,100000,0,64*Inkscape::Bitmap::MiB));
    for (bool planar : {false,true}) {
        std::vector<std::uint16_t> values; std::vector<unsigned> expected;
        for (unsigned y = 0; y < 18; ++y) for (unsigned x = 0; x < 19; ++x) {
            values.insert(values.end(), {std::uint16_t(x*11*257),std::uint16_t(y*13*257),65535,32896});
            expected.insert(expected.end(), {x*11,y*13,255,128});
        }
        expect_tiff(tiff_samples(16,PHOTOMETRIC_RGB,true,false,planar,true,1,COMPRESSION_LZW,values,19,18),expected,19,18);
    }
    expect_tiff(tiff_samples(1,PHOTOMETRIC_PALETTE,false,false,false,false,1,COMPRESSION_NONE,{1,0,1,0,1,0}),
                {255,0,0,255, 0,0,0,255, 255,0,0,255, 0,0,0,255, 255,0,0,255, 0,0,0,255});
    // Unsupported 1-bit grayscale still goes through the existing TIFF loader.
    expect_tiff(tiff_samples(1,PHOTOMETRIC_MINISBLACK,false,false,false,false,1,COMPRESSION_NONE,{1,0,1,0,1,0}),
                {255,255,255,255, 0,0,0,255, 255,255,255,255, 0,0,0,255, 255,255,255,255, 0,0,0,255});
    // Separate open operation: callers reset the shared diagnostic explicitly.
    Inkscape::image_open_reset();
    // Corrupt directory must not produce an accepted empty/partial custom raster.
    std::string broken("II\052\000\377\377\377\177",8);
    auto pix = std::unique_ptr<Inkscape::Pixbuf>(Inkscape::Pixbuf::create_from_buffer(broken));
    EXPECT_EQ(pix,nullptr);
    EXPECT_STREQ(Inkscape::image_open_diagnostic(), "image decoder could not read the image");
}
