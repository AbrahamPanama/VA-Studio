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
    Inkscape::Bitmap::Memory m{32ull*1024*Inkscape::Bitmap::MiB,4ull*1024*Inkscape::Bitmap::MiB,1,true,0}; EXPECT_EQ(Inkscape::image_open_memory_budget(m),m.available); EXPECT_TRUE(Inkscape::image_open_dimensions_fit(14400,21600,100*Inkscape::Bitmap::MiB,Inkscape::image_open_memory_budget(m))); m.measured = false; EXPECT_EQ(Inkscape::image_open_memory_budget(m),256*Inkscape::Bitmap::MiB);
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
