// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Test Inkscape::Extensions::Internal::PdfOutput
 */
/*
 * Authors:
 *   Martin Owens
 *
 * Copyright (C) 2025 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "attributes.h"
#include "style.h"

#include "extension/internal/pdfoutput/remember-styles.h"

#include <config.h>

#include <cairo.h>
#include <giomm/file.h>
#include <glib/gstdio.h>
#include <glibmm/error.h>
#include <glibmm/fileutils.h>
#include <cstring>
#include <span>
#include <gtest/gtest.h>
#include <png.h>
#include <sys/stat.h>
#ifndef G_OS_WIN32
#include <sys/resource.h>
#include <csignal>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "colors/color.h"
#include "document.h"
#include "extension/db.h"
#include "extension/init.h"
#include "extension/internal/cairo-render-context.h"
#include "extension/internal/cairo-renderer-pdf-out.h"
#include "extension/internal/cairo-renderer.h"
#include "extension/output.h"
#include "extension/implementation/implementation.h"
#include "extension/input.h"
#include "extension/system.h"
#include "helper/png-write.h"
#include "inkscape.h"
#include "preferences.h"
#include "util/scope_exit.h"
#include "io/file.h"
#include "io/tiff-export.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "ui/dialog/print.h"
#include "util/units.h"

#if defined(WITH_LIBCDR) || defined(WITH_LIBVISIO)
#include <libcdr/libcdr.h>
#include "extension/internal/rvng-import-dialog.h"
#define VACARDS_TEST_RVNG 1
#endif

using Inkscape::Extension::Internal::StyleMemory;

TEST(StyleMemeoryTest, MapFiltersStyle)
{
    SPStyle *style = new SPStyle(); // No document style
    style->mergeString("opacity:1.0;fill:black;stroke:red");

    auto memory = StyleMemory({SPAttr::OPACITY, SPAttr::FILL});
    auto map = memory.get_changes(style);
    EXPECT_TRUE(map.contains(SPAttr::OPACITY));
    EXPECT_TRUE(map.contains(SPAttr::FILL));
    EXPECT_FALSE(map.contains(SPAttr::STROKE));

    EXPECT_EQ(map[SPAttr::OPACITY], "1");
    EXPECT_EQ(map[SPAttr::FILL], "black");
}

TEST(StyleMemoryTest, MapContainsUnsetStyle)
{
    SPStyle *style = new SPStyle(); // No document style
    style->mergeString("fill:black;stroke:red");

    auto memory = StyleMemory({SPAttr::OPACITY, SPAttr::FILL});
    auto map = memory.get_changes(style);
    EXPECT_TRUE(map.contains(SPAttr::OPACITY));
    EXPECT_TRUE(map.contains(SPAttr::FILL));

    EXPECT_EQ(map[SPAttr::OPACITY], "1");
    EXPECT_EQ(map[SPAttr::FILL], "black");
}

TEST(StyleMemoryTest, MemoryState)
{
    SPStyle *style = new SPStyle(); // No document style
    style->mergeString("fill:black;");

    auto memory = StyleMemory({SPAttr::OPACITY, SPAttr::FILL});
    ASSERT_EQ(memory.get_state().size(), 0);

    auto map = memory.get_changes(style);
    ASSERT_EQ(map.size(), 2);

    {
        auto scope = memory.remember(map);
        ASSERT_EQ(memory.get_state().size(), 2);
        ASSERT_EQ(memory.get_state().find(SPAttr::FILL)->second, "black");
        ASSERT_EQ(memory.get_state().find(SPAttr::OPACITY)->second, "1");

        // Nothing has changed, so nothing should change
        ASSERT_EQ(memory.get_changes(style).size(), 0);

        style->clear(SPAttr::FILL);
        style->mergeString("fill:red");
        auto map2 = memory.get_changes(style);
        ASSERT_EQ(map2.size(), 1);
        ASSERT_EQ(map2[SPAttr::FILL], "red");

        {
            auto scope2 = memory.remember(map2);
            ASSERT_EQ(memory.get_state().find(SPAttr::FILL)->second, "red");
            ASSERT_EQ(memory.get_state().find(SPAttr::OPACITY)->second, "1");
            ASSERT_EQ(memory.get_changes(style).size(), 0);
        }

        ASSERT_EQ(memory.get_state().find(SPAttr::FILL)->second, "black");
        ASSERT_EQ(memory.get_state().find(SPAttr::OPACITY)->second, "1");
        ASSERT_EQ(memory.get_changes(style).size(), 1);
    }
}

// ---------------------------------------------------------------------------
// File output robustness (ST-F IO1/IO2): wide raster exports and PDF replacement.
// ---------------------------------------------------------------------------
namespace {

std::span<char const> mem(char const *text) { return {text, std::strlen(text)}; }

std::string read_file(std::string const &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct TempDir
{
    std::string path;
    TempDir()
    {
        if (auto *d = g_dir_make_tmp("vacards-file-output-XXXXXX", nullptr)) {
            path = d;
            g_free(d);
        }
    }
    ~TempDir()
    {
        if (path.empty()) return;
        g_chmod(path.c_str(), 0700);
        if (auto *dir = g_dir_open(path.c_str(), 0, nullptr)) {
            while (auto const *name = g_dir_read_name(dir)) {
                auto *full = g_build_filename(path.c_str(), name, nullptr);
                g_remove(full);
                g_free(full);
            }
            g_dir_close(dir);
        }
        g_rmdir(path.c_str());
    }
    std::string file(char const *name) const { return path + G_DIR_SEPARATOR_S + name; }
    std::vector<std::string> entries() const
    {
        std::vector<std::string> names;
        if (auto *dir = g_dir_open(path.c_str(), 0, nullptr)) {
            while (auto const *name = g_dir_read_name(dir)) names.emplace_back(name);
            g_dir_close(dir);
        }
        return names;
    }
};

class FileOutputTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Inkscape::Application::create(false);
        static bool initialised = false;
        if (!initialised) {
            Inkscape::Extension::init();
            initialised = true;
        }
    }
};

} // namespace

// IO1: Cairo refuses image surfaces wider than 32767 px. A 3 m banner at 300 dpi is about
// 35,400 px wide; the export must tile and produce the correct pixels, not blank or garbage.
TEST_F(FileOutputTest, PngExportWiderThanCairoSurfaceLimitIsTiledCorrectly)
{
    constexpr int W = 40000;
    constexpr int H = 4;
    auto doc = SPDocument::createNewDocFromMem(mem(
        "<svg xmlns='http://www.w3.org/2000/svg' width='40000' height='4' viewBox='0 0 40000 4'>"
        "<rect x='0' y='0' width='40000' height='4' style='fill:#ff0000;stroke:none'/>"
        "<rect x='32760' y='0' width='20' height='4' style='fill:#0000ff;stroke:none'/>"
        "</svg>"));
    ASSERT_TRUE(doc);
    TempDir dir;
    ASSERT_FALSE(dir.path.empty());
    auto const out = dir.file("wide.png");

    auto const result = sp_export_png_file(doc.get(), out.c_str(), 0, 0, W, H, W, H, 96.0, 96.0,
                                           Inkscape::Colors::Color(0xffffffff), nullptr, nullptr, true,
                                           {}, false, 6, 8, 1, 0);
    ASSERT_EQ(result, EXPORT_OK);

    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    ASSERT_TRUE(png_image_begin_read_from_file(&image, out.c_str())) << image.message;
    ASSERT_EQ(image.width, png_uint_32(W));
    ASSERT_EQ(image.height, png_uint_32(H));
    image.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> px(PNG_IMAGE_SIZE(image));
    ASSERT_TRUE(png_image_finish_read(&image, nullptr, px.data(), 0, nullptr)) << image.message;

    auto at = [&](int x, int y) {
        auto const *p = &px[(static_cast<size_t>(y) * W + x) * 4];
        return std::vector<int>{p[0], p[1], p[2], p[3]};
    };
    std::vector<int> const red{255, 0, 0, 255};
    std::vector<int> const blue{0, 0, 255, 255};
    for (int y : {0, H - 1}) {
        EXPECT_EQ(at(0, y), red);
        EXPECT_EQ(at(100, y), red);
        EXPECT_EQ(at(32759, y), red);
        EXPECT_EQ(at(32760, y), blue);
        EXPECT_EQ(at(32766, y), blue) << "last column of the first tile";
        EXPECT_EQ(at(32767, y), blue) << "first column of the second tile";
        EXPECT_EQ(at(32779, y), blue);
        EXPECT_EQ(at(32780, y), red);
        EXPECT_EQ(at(W - 1, y), red);
    }
}

// IO1 (filters): a blurred object straddling the tile seam must render without a blank gap or step.
TEST_F(FileOutputTest, PngExportBlurAcrossTileSeamIsContinuous)
{
    constexpr int W = 40000;
    constexpr int H = 40;
    auto doc = SPDocument::createNewDocFromMem(mem(
        "<svg xmlns='http://www.w3.org/2000/svg' width='40000' height='40' viewBox='0 0 40000 40'>"
        "<defs><filter id='b' x='-0.01' y='-0.5' width='1.02' height='2'>"
        "<feGaussianBlur stdDeviation='4'/></filter></defs>"
        "<rect x='31000' y='5' width='1767' height='30' style='fill:#000000;filter:url(#b)'/>"
        "</svg>"));
    ASSERT_TRUE(doc);
    TempDir dir;
    auto const out = dir.file("blur.png");
    ASSERT_EQ(sp_export_png_file(doc.get(), out.c_str(), 0, 0, W, H, W, H, 96.0, 96.0,
                                 Inkscape::Colors::Color(0xffffffff), nullptr, nullptr, true, {}, false, 6, 8, 1, 2),
              EXPORT_OK);
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    ASSERT_TRUE(png_image_begin_read_from_file(&image, out.c_str())) << image.message;
    ASSERT_EQ(image.width, png_uint_32(W));
    image.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> px(PNG_IMAGE_SIZE(image));
    ASSERT_TRUE(png_image_finish_read(&image, nullptr, px.data(), 0, nullptr)) << image.message;
    auto red_at = [&](int x, int y) { return int(px[(size_t(y) * W + x) * 4]); };
    // The object's right edge (and so the blur gradient) sits exactly on the tile seam at x=32767.
    int const y = H / 2;
    EXPECT_LT(red_at(32700, y), 10) << "inside the object";
    EXPECT_GT(red_at(32840, y), 245) << "outside the object";
    EXPECT_GT(red_at(32766, y), 90);
    EXPECT_LT(red_at(32766, y), 170);
    EXPECT_GT(red_at(32767, y), 90);
    EXPECT_LT(red_at(32767, y), 170);
    int maximum_step = 0;
    for (int x = 32700; x < 32840; ++x) {
        EXPECT_GE(red_at(x + 1, y), red_at(x, y)) << "gradient is monotonic across the seam at x=" << x;
        maximum_step = std::max(maximum_step, red_at(x + 1, y) - red_at(x, y));
    }
    EXPECT_LE(maximum_step, 40) << "no step across the tile boundary";
}

// ---------------------------------------------------------------------------
// Export and filter render safety (ST-S)
// ---------------------------------------------------------------------------
namespace {

struct Rgba {
    unsigned width = 0, height = 0;
    std::vector<unsigned char> px;
    bool ok = false;
    std::vector<int> at(unsigned x, unsigned y) const
    {
        auto const *p = &px[(size_t(y) * width + x) * 4];
        return {p[0], p[1], p[2], p[3]};
    }
};

Rgba read_png(std::string const &path)
{
    Rgba out;
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str())) return out;
    image.format = PNG_FORMAT_RGBA;
    out.width = image.width;
    out.height = image.height;
    out.px.resize(PNG_IMAGE_SIZE(image));
    out.ok = png_image_finish_read(&image, nullptr, out.px.data(), 0, nullptr);
    png_image_free(&image);
    return out;
}

ExportResult export_png(SPDocument *doc, std::string const &out, unsigned w, unsigned h, double x1, double y1)
{
    return sp_export_png_file(doc, out.c_str(), 0, 0, x1, y1, w, h, 96.0, 96.0,
                              Inkscape::Colors::Color(0xffffffff), nullptr, nullptr, true, {}, false, 6, 8, 1, 2);
}

// A filtered item wider than Cairo's 32767 px surface limit: its intermediate surface cannot be
// allocated, which is what an out-of-memory looks like to the filter code.
constexpr char const *UNALLOCATABLE_FILTER_SVG =
    "<svg xmlns='http://www.w3.org/2000/svg' width='40000' height='40' viewBox='0 0 40000 40'>"
    "<defs><filter id='b' x='0' y='0' width='1' height='1'><feGaussianBlur stdDeviation='4'/></filter></defs>"
    "<rect id='r' x='0' y='0' width='40000' height='40' style='fill:#000000;filter:url(#b)'/>"
    "</svg>";

} // namespace

// R1/R3: an export whose rendering runs out of memory fails with EXPORT_ERROR, leaves no partial
// file, and hides the document again, so editing and closing it afterwards works.
TEST_F(FileOutputTest, PngExportOutOfMemoryFailsCleanlyAndLeavesDocumentUsable)
{
    auto doc = SPDocument::createNewDocFromMem(mem(UNALLOCATABLE_FILTER_SVG));
    ASSERT_TRUE(doc);
    TempDir dir;
    auto const out = dir.file("huge.png");
    auto *rect = cast<SPItem>(doc->getObjectById("r"));
    ASSERT_TRUE(rect);

    EXPECT_EQ(export_png(doc.get(), out, 40000, 40, 40000, 40), EXPORT_ERROR);
    EXPECT_FALSE(g_file_test(out.c_str(), G_FILE_TEST_EXISTS)) << "no partial file may be left behind";
    EXPECT_TRUE(dir.entries().empty());

    // The export's display items must be gone: a stale view would be updated on the next edit.
    EXPECT_TRUE(rect->views.empty()) << rect->views.size() << " stale view(s) on the filtered item";
    EXPECT_TRUE(doc->getRoot()->views.empty()) << "stale view on the root";

    // Editing afterwards works, and so does a normal export of the same document area.
    rect->setAttribute("style", "fill:#ff0000");
    doc->ensureUpToDate();
    rect->setAttribute("width", "50");
    doc->ensureUpToDate();
    EXPECT_EQ(export_png(doc.get(), out, 50, 40, 50, 40), EXPORT_OK);
    auto const img = read_png(out);
    ASSERT_TRUE(img.ok);
    EXPECT_EQ(img.at(10, 10), (std::vector<int>{255, 0, 0, 255}));
}

// Preserve exact PNG bytes with the original whole-page filter cache (R2 withdrawn).
TEST_F(FileOutputTest, PngExportBlurPreservesWholePageCacheBytes)
{
    auto *prefs = Inkscape::Preferences::get();
    bool const old_dithering = prefs->getBool("/options/dithering/value", true);
    auto restore_dithering = scope_exit([&] { prefs->setBool("/options/dithering/value", old_dithering); });
    prefs->setBool("/options/dithering/value", false); // compare deterministic gradient pixels
    constexpr int W = 240;
    constexpr int H = 1100;
    auto doc = SPDocument::createNewDocFromMem(mem(
        "<svg xmlns='http://www.w3.org/2000/svg' width='240' height='1100' viewBox='0 0 240 1100'>"
        "<defs><filter id='b' x='-0.2' y='-0.2' width='1.4' height='1.4' color-interpolation-filters='sRGB'>"
        "<feGaussianBlur stdDeviation='9'/></filter>"
        "<filter id='s' x='-0.5' y='-0.5' width='2' height='2'><feGaussianBlur stdDeviation='2'/></filter>"
        "<linearGradient id='g' x1='0' y1='0' x2='1' y2='1'><stop offset='0' stop-color='#d00'/>"
        "<stop offset='1' stop-color='#05f'/></linearGradient></defs>"
        // A page-size blurred rectangle: its blur crosses every stripe and window boundary.
        "<rect x='20' y='20' width='200' height='1060' style='fill:url(#g);filter:url(#b)'/>"
        // Small blurred shapes sitting on stripe boundaries (rows 64, 128 ...) and window edges.
        "<circle cx='60' cy='64' r='20' style='fill:#000;filter:url(#s)'/>"
        "<circle cx='150' cy='256' r='25' style='fill:#080;filter:url(#s)'/>"
        "<circle cx='100' cy='640' r='30' style='fill:#fa0;filter:url(#s)'/>"
        "</svg>"));
    ASSERT_TRUE(doc);
    TempDir dir;

    auto const output = dir.file("whole.png");
    ASSERT_EQ(export_png(doc.get(), output, W, H, W, H), EXPORT_OK);
    auto const whole = read_png(output);
    ASSERT_TRUE(whole.ok);
    ASSERT_EQ(whole.width, unsigned(W));
    ASSERT_EQ(whole.height, unsigned(H));
    EXPECT_NE(whole.at(120, 500), (std::vector<int>{255, 255, 255, 255}));
    auto const bytes = read_file(output);
    ASSERT_EQ(export_png(doc.get(), dir.file("repeat.png"), W, H, W, H), EXPORT_OK);
    EXPECT_EQ(read_file(dir.file("repeat.png")), bytes);
    // External references are created by a separate run of the R2-free reference build.
    if (auto const *reference_dir = std::getenv("ST_S_REFERENCE_DIR")) {
        auto const reference = std::string(reference_dir) + "/gradient.png";
        ASSERT_TRUE(g_file_test(reference.c_str(), G_FILE_TEST_EXISTS));
        EXPECT_EQ(bytes, read_file(reference)) << "PNG must remain byte-identical to reference";
    }
    if (auto const *save_dir = std::getenv("ST_S_SAVE_REFERENCE_DIR")) {
        auto const reference = std::string(save_dir) + "/gradient.png";
        ASSERT_TRUE(g_file_set_contents(reference.c_str(), bytes.data(), bytes.size(), nullptr));
    }
}

// The solid fixture also covers blurred shapes crossing export stripes.
TEST_F(FileOutputTest, PngExportSolidBlurPreservesWholePageCacheBytes)
{
    auto *prefs = Inkscape::Preferences::get();
    bool const old_dithering = prefs->getBool("/options/dithering/value", true);
    auto restore_dithering = scope_exit([&] { prefs->setBool("/options/dithering/value", old_dithering); });
    prefs->setBool("/options/dithering/value", false); // compare deterministic gradient pixels
    constexpr int W = 240;
    constexpr int H = 1100;
    auto doc = SPDocument::createNewDocFromMem(mem(
        "<svg xmlns='http://www.w3.org/2000/svg' width='240' height='1100' viewBox='0 0 240 1100'>"
        "<defs><filter id='b' x='-0.2' y='-0.2' width='1.4' height='1.4' color-interpolation-filters='sRGB'>"
        "<feGaussianBlur stdDeviation='9'/></filter>"
        "<filter id='s' x='-0.5' y='-0.5' width='2' height='2'><feGaussianBlur stdDeviation='2'/></filter>"
        "<linearGradient id='g' x1='0' y1='0' x2='1' y2='1'><stop offset='0' stop-color='#d00'/>"
        "<stop offset='1' stop-color='#05f'/></linearGradient></defs>"
        // A page-size blurred rectangle: its blur crosses every stripe and window boundary.
        "<rect x='20' y='20' width='200' height='1060' style='fill:#dd0000;filter:url(#b)'/>"
        // Small blurred shapes sitting on stripe boundaries (rows 64, 128 ...) and window edges.
        "<circle cx='60' cy='64' r='20' style='fill:#000;filter:url(#s)'/>"
        "<circle cx='150' cy='256' r='25' style='fill:#080;filter:url(#s)'/>"
        "<circle cx='100' cy='640' r='30' style='fill:#fa0;filter:url(#s)'/>"
        "</svg>"));
    ASSERT_TRUE(doc);
    TempDir dir;

    auto const output = dir.file("whole.png");
    ASSERT_EQ(export_png(doc.get(), output, W, H, W, H), EXPORT_OK);
    auto const whole = read_png(output);
    ASSERT_TRUE(whole.ok);
    ASSERT_EQ(whole.width, unsigned(W));
    ASSERT_EQ(whole.height, unsigned(H));
    EXPECT_NE(whole.at(120, 500), (std::vector<int>{255, 255, 255, 255}));
    auto const bytes = read_file(output);
    ASSERT_EQ(export_png(doc.get(), dir.file("repeat.png"), W, H, W, H), EXPORT_OK);
    EXPECT_EQ(read_file(dir.file("repeat.png")), bytes);
    // External references are created by a separate run of the R2-free reference build.
    if (auto const *reference_dir = std::getenv("ST_S_REFERENCE_DIR")) {
        auto const reference = std::string(reference_dir) + "/solid.png";
        ASSERT_TRUE(g_file_test(reference.c_str(), G_FILE_TEST_EXISTS));
        EXPECT_EQ(bytes, read_file(reference)) << "PNG must remain byte-identical to reference";
    }
    if (auto const *save_dir = std::getenv("ST_S_SAVE_REFERENCE_DIR")) {
        auto const reference = std::string(save_dir) + "/solid.png";
        ASSERT_TRUE(g_file_set_contents(reference.c_str(), bytes.data(), bytes.size(), nullptr));
    }
}

// R8/R9: extreme filter parameters render promptly and sensibly instead of hanging or invoking
// undefined behaviour.
TEST_F(FileOutputTest, PngExportExtremeFilterParametersRenderPromptly)
{
    auto render = [&](char const *filter_body, std::string &png) {
        std::string svg =
            "<svg xmlns='http://www.w3.org/2000/svg' width='120' height='100' viewBox='0 0 120 100'>"
            "<defs><filter id='f' x='0' y='0' width='120' height='100' filterUnits='userSpaceOnUse' color-interpolation-filters='sRGB'>";
        svg += filter_body;
        svg += "</filter></defs><rect x='50' y='40' width='20' height='20' style='fill:#ff0000;filter:url(#f)'/></svg>";
        auto doc = SPDocument::createNewDocFromMem(mem(svg.c_str()));
        EXPECT_TRUE(doc);
        TempDir dir;
        png = dir.file("f.png");
        auto const start = std::chrono::steady_clock::now();
        auto const result = export_png(doc.get(), png, 120, 100, 120, 100);
        auto const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        EXPECT_EQ(result, EXPORT_OK) << filter_body;
        EXPECT_LT(seconds, 20.0) << filter_body << " took " << seconds << " s";
        return read_png(png);
    };
    std::string png;

    // feOffset far beyond the canvas moves the object out of the picture: only background remains.
    auto img = render("<feOffset dx='1e308' dy='-1e308'/>", png);
    ASSERT_TRUE(img.ok);
    EXPECT_EQ(img.at(60, 50), (std::vector<int>{255, 255, 255, 255}));
    EXPECT_EQ(img.at(10, 10), (std::vector<int>{255, 255, 255, 255}));

    // A morphology radius larger than the image: dilation floods the filter region with the
    // object's colour; erosion removes the object.
    img = render("<feMorphology operator='dilate' radius='1e308'/>", png);
    ASSERT_TRUE(img.ok);
    EXPECT_EQ(img.at(5, 5), (std::vector<int>{255, 0, 0, 255}));
    EXPECT_EQ(img.at(115, 95), (std::vector<int>{255, 0, 0, 255}));
    img = render("<feMorphology operator='erode' radius='1e308'/>", png);
    ASSERT_TRUE(img.ok);
    EXPECT_EQ(img.at(60, 50), (std::vector<int>{255, 255, 255, 255}));

    // A sub-pixel feTile tile in a large primitive area has billions of copies: bounded work.
    img = render("<feFlood x='10' y='10' width='0.002' height='0.002' flood-color='#0000ff'/>"
                 "<feOffset dx='0' dy='0' x='10' y='10' width='0.002' height='0.002'/><feTile/>", png);
    ASSERT_TRUE(img.ok);
    EXPECT_EQ(img.width, 120u);

    // A tile that is large relative to the slot still tiles exactly as before: the 40x40 blue tile
    // repeats from the origin of the region.
    img = render("<feFlood x='0' y='0' width='40' height='40' flood-color='#0000ff'/>"
                 "<feOffset dx='0' dy='0' x='0' y='0' width='40' height='40'/><feTile/>", png);
    ASSERT_TRUE(img.ok);
    EXPECT_EQ(img.at(5, 5), (std::vector<int>{0, 0, 255, 255}));
    EXPECT_EQ(img.at(100, 90), (std::vector<int>{0, 0, 255, 255}));
}

// IO2: the atomic writer replaces the destination only after the render succeeds.
TEST_F(FileOutputTest, AtomicWriterKeepsOldFileWhenRenderFailsAndCleansUp)
{
    TempDir dir;
    auto const dest = dir.file("out.pdf");
    ASSERT_TRUE(g_file_set_contents(dest.c_str(), "OLD-GOOD-FILE", -1, nullptr));

    std::string error;
    EXPECT_FALSE(Inkscape::IO::write_file_atomically(
        dest,
        [&](std::string const &tmp) {
            EXPECT_TRUE(g_file_set_contents(tmp.c_str(), "PARTIAL", -1, nullptr));
            return false; // render failed after writing part of the file
        },
        error));
    EXPECT_EQ(read_file(dest), "OLD-GOOD-FILE");
    EXPECT_EQ(dir.entries(), std::vector<std::string>{"out.pdf"}) << "temporary must be removed";
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(Inkscape::IO::write_file_atomically(
        dest, [&](std::string const &) -> bool { throw std::runtime_error("boom"); }, error));
    EXPECT_EQ(read_file(dest), "OLD-GOOD-FILE");
    EXPECT_EQ(dir.entries().size(), 1u);

    error.clear();
    EXPECT_TRUE(Inkscape::IO::write_file_atomically(
        dest,
        [&](std::string const &tmp) { return g_file_set_contents(tmp.c_str(), "NEW", -1, nullptr); }, error));
    EXPECT_EQ(read_file(dest), "NEW");
    EXPECT_EQ(dir.entries().size(), 1u);
}


// K2: "print as bitmap" must not ask for an image Cairo cannot load back (over 32767 px a side).
TEST_F(FileOutputTest, PrintBitmapDpiIsLimitedToTheCairoSurfaceSize)
{
    using Inkscape::UI::Dialog::limit_print_bitmap_dpi;
    double const px_per_m = Inkscape::Util::Quantity::convert(3.0, "m", "px");
    double const banner_height = Inkscape::Util::Quantity::convert(1.0, "m", "px");
    // A 3 m x 1 m banner at 300 dpi would be 35,433 px wide.
    double const dpi = limit_print_bitmap_dpi(px_per_m, banner_height, 300.0);
    EXPECT_LT(dpi, 300.0);
    EXPECT_LE(Inkscape::Util::Quantity::convert(px_per_m, "px", "in") * dpi, 32767.0);
    EXPECT_GT(dpi, 250.0) << "only reduced as far as needed";
    // A1-ish page at 300 dpi fits: unchanged. Tall pages are limited by height too.
    EXPECT_DOUBLE_EQ(limit_print_bitmap_dpi(793.7, 1122.5, 300.0), 300.0);
    EXPECT_LE(Inkscape::Util::Quantity::convert(px_per_m, "px", "in") * limit_print_bitmap_dpi(banner_height, px_per_m, 300.0),
              32767.0);
    EXPECT_DOUBLE_EQ(limit_print_bitmap_dpi(100.0, 100.0, 0.0), 0.0);
}

#ifdef VACARDS_TEST_RVNG
// IO3: a CDR/VSD/WPG path is opened through one helper that never hands NULL to fopen()
// (Windows: g_win32_locale_filename_from_utf8 returns NULL for characters outside the ANSI page).
TEST_F(FileOutputTest, RvngStreamOpensNonAsciiPathAndRejectsMissingFile)
{
    std::string const source = INKSCAPE_TESTS_DIR "/cli_tests/testcases/librevenge_formats/corel_draw.cdr";
    std::string const bytes = read_file(source);
    ASSERT_FALSE(bytes.empty());

    TempDir dir;
    auto const odd = dir.file("Logo \xE2\x98\x85 V\xC3\xA9ro \xE6\xB5\x8B\xE8\xAF\x95.cdr");
    ASSERT_TRUE(g_file_set_contents(odd.c_str(), bytes.data(), bytes.size(), nullptr));

    auto stream = Inkscape::Extension::Internal::open_rvng_input_stream(odd.c_str());
    ASSERT_TRUE(stream);
    unsigned long got = 0;
    auto const *data = stream->read(bytes.size(), got);
    ASSERT_EQ(got, bytes.size());
    EXPECT_EQ(std::string(reinterpret_cast<char const *>(data), got), bytes);

    EXPECT_FALSE(Inkscape::Extension::Internal::open_rvng_input_stream(dir.file("missing \xE2\x98\x85.cdr").c_str()));
    EXPECT_FALSE(Inkscape::Extension::Internal::open_rvng_input_stream(dir.path.c_str())) << "a directory";
    EXPECT_FALSE(Inkscape::Extension::Internal::open_rvng_input_stream(nullptr));
    EXPECT_FALSE(Inkscape::Extension::Internal::open_rvng_input_stream(""));
}

#ifdef WITH_LIBCDR
// IO3: the whole CDR import works from a non-ASCII path, and a missing file is an open failure.
TEST_F(FileOutputTest, CdrImportFromNonAsciiPathAndMissingFile)
{
    std::string const source = INKSCAPE_TESTS_DIR "/cli_tests/testcases/librevenge_formats/corel_draw.cdr";
    auto const bytes = read_file(source);
    ASSERT_FALSE(bytes.empty());
    TempDir dir;
    auto const odd = dir.file("Logo \xE2\x98\x85.cdr");
    ASSERT_TRUE(g_file_set_contents(odd.c_str(), bytes.data(), bytes.size(), nullptr));

    auto doc = Inkscape::Extension::Internal::rvng_open(odd.c_str(), libcdr::CDRDocument::isSupported,
                                                        libcdr::CDRDocument::parse);
    EXPECT_TRUE(doc);
    EXPECT_FALSE(Inkscape::Extension::Internal::rvng_open(dir.file("gone \xE2\x98\x85.cdr").c_str(),
                                                          libcdr::CDRDocument::isSupported,
                                                          libcdr::CDRDocument::parse));
}
#endif
#endif

namespace {
// An importer that fails the way real ones can: allocation, range and GLib errors.
class ThrowingInput : public Inkscape::Extension::Implementation::Implementation
{
public:
    std::unique_ptr<SPDocument> open(Inkscape::Extension::Input *, char const *filename, bool) override
    {
        auto const what = read_file(filename);
        if (what == "bad_alloc") throw std::bad_alloc();
        if (what == "out_of_range") throw std::out_of_range("importer index");
        if (what == "glib") throw Glib::FileError(Glib::FileError::FAILED, "importer glib error");
        throw 42; // not even an exception object
    }
};
} // namespace

// IO4: importer exceptions other than the Extension ones must become an open failure, not
// std::terminate through the GTK callback.
TEST_F(FileOutputTest, FileOpenTurnsImporterExceptionsIntoFailure)
{
    static bool registered = false;
    if (!registered) {
        Inkscape::Extension::build_from_mem(
            "<inkscape-extension xmlns=\"" INKSCAPE_EXTENSION_URI "\">\n"
            "<name>VA test throwing input</name>\n<id>org.vacards.test.input.throwing</id>\n"
            "<input><extension>.vathrow</extension><mimetype>application/x-vathrow</mimetype>"
            "<filetypename>Throwing test</filetypename></input>\n</inkscape-extension>",
            std::make_unique<ThrowingInput>());
        registered = true;
    }
    TempDir dir;
    for (char const *what : {"bad_alloc", "out_of_range", "glib", "other"}) {
        auto const path = dir.file((std::string("f-") + what + ".vathrow").c_str());
        ASSERT_TRUE(g_file_set_contents(path.c_str(), what, -1, nullptr));
        std::string error;
        std::pair<std::unique_ptr<SPDocument>, bool> result;
        EXPECT_NO_THROW(result = ink_file_open(Gio::File::create_for_path(path), &error)) << what;
        EXPECT_FALSE(result.first) << what;
        EXPECT_FALSE(result.second) << what << " is a failure, not a user cancel";
        EXPECT_FALSE(error.empty()) << what;
    }
}

#ifdef CAIRO_HAS_PDF_SURFACE
namespace {
Inkscape::Extension::Output *pdf_output()
{
    return dynamic_cast<Inkscape::Extension::Output *>(
        Inkscape::Extension::db.get("org.inkscape.output.pdf.cairorenderer"));
}
constexpr char const *SMALL_SVG =
    "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='50' viewBox='0 0 100 50'>"
    "<rect x='10' y='10' width='40' height='20' style='fill:#ff0000'/></svg>";
} // namespace

// IO2: a normal PDF export still produces a valid PDF and leaves no temporary file behind.
TEST_F(FileOutputTest, PdfExportWritesValidPdfAndReplacesOldFile)
{
    auto doc = SPDocument::createNewDocFromMem(mem(SMALL_SVG));
    ASSERT_TRUE(doc);
    ASSERT_TRUE(pdf_output());
    TempDir dir;
    auto const dest = dir.file("out.pdf");
    ASSERT_TRUE(g_file_set_contents(dest.c_str(), "OLD", -1, nullptr));

    Inkscape::Extension::Internal::CairoRendererPdfOutput imp;
    EXPECT_NO_THROW(imp.save(pdf_output(), doc.get(), dest.c_str()));
    auto const bytes = read_file(dest);
    EXPECT_EQ(bytes.substr(0, 5), "%PDF-");
    EXPECT_NE(bytes.find("%%EOF"), std::string::npos);
    EXPECT_EQ(dir.entries(), std::vector<std::string>{"out.pdf"});
}

// F2: an existing writable file in a folder that does not allow creating files still exports.
TEST_F(FileOutputTest, PdfExportOverwritesWritableFileInNonWritableDirectory)
{
    auto doc = SPDocument::createNewDocFromMem(mem(SMALL_SVG));
    ASSERT_TRUE(doc);
    ASSERT_TRUE(pdf_output());
    TempDir dir;
    auto const dest = dir.file("out.pdf");
    ASSERT_TRUE(g_file_set_contents(dest.c_str(), "OLD", -1, nullptr));
    ASSERT_EQ(g_chmod(dir.path.c_str(), 0555), 0);
    if (auto *probe = g_fopen(dir.file("probe").c_str(), "wb")) {
        fclose(probe);
        g_remove(dir.file("probe").c_str());
        g_chmod(dir.path.c_str(), 0700);
        GTEST_SKIP() << "directory permissions are not enforced for this user";
    }

    Inkscape::Extension::Internal::CairoRendererPdfOutput imp;
    EXPECT_NO_THROW(imp.save(pdf_output(), doc.get(), dest.c_str()));
    g_chmod(dir.path.c_str(), 0700);
    EXPECT_EQ(read_file(dest).substr(0, 5), "%PDF-");
    EXPECT_EQ(dir.entries(), std::vector<std::string>{"out.pdf"});
}

// F2: the replacement keeps the permissions of the file it replaces. POSIX
// mode bits only: Windows has no group/other permission bits to keep.
#ifndef _WIN32
TEST_F(FileOutputTest, AtomicWriterKeepsDestinationPermissions)
{
    TempDir dir;
    auto const dest = dir.file("out.pdf");
    ASSERT_TRUE(g_file_set_contents(dest.c_str(), "OLD", -1, nullptr));
    ASSERT_EQ(g_chmod(dest.c_str(), 0640), 0);
    std::string error;
    ASSERT_TRUE(Inkscape::IO::write_file_atomically(
        dest, [&](std::string const &tmp) { return g_file_set_contents(tmp.c_str(), "NEW", -1, nullptr); }, error));
    GStatBuf st;
    ASSERT_EQ(g_stat(dest.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, mode_t(0640));
    EXPECT_EQ(read_file(dest), "NEW");
}
#endif

// F1: "-" is standard output: rendered directly, never staged and renamed to a file called "-".
TEST_F(FileOutputTest, AtomicWriterPassesStdoutThroughUnstaged)
{
    TempDir dir;
    auto *const cwd = g_get_current_dir();
    ASSERT_EQ(g_chdir(dir.path.c_str()), 0);
    std::string seen;
    std::string error;
    bool const ok = Inkscape::IO::write_file_atomically(
        "-", [&](std::string const &target) { seen = target; return true; }, error);
    auto const listing = dir.entries();
    g_chdir(cwd);
    g_free(cwd);
    EXPECT_TRUE(ok);
    EXPECT_EQ(seen, "-");
    EXPECT_TRUE(listing.empty()) << "nothing may be staged or created for stdout";
    error.clear();
    EXPECT_FALSE(Inkscape::IO::write_file_atomically("-", [](std::string const &) { return false; }, error));
    EXPECT_FALSE(error.empty());
}

// F7: a missing parent directory is created, as opening the destination directly did.
TEST_F(FileOutputTest, AtomicWriterCreatesMissingParentDirectory)
{
    TempDir dir;
    auto const dest = dir.path + G_DIR_SEPARATOR_S + "made" + G_DIR_SEPARATOR_S + "deeper" + G_DIR_SEPARATOR_S + "o.pdf";
    std::string error;
    EXPECT_TRUE(Inkscape::IO::write_file_atomically(
        dest, [&](std::string const &tmp) { return g_file_set_contents(tmp.c_str(), "X", -1, nullptr); }, error))
        << error;
    EXPECT_EQ(read_file(dest), "X");
    g_remove(dest.c_str());
    g_rmdir((dir.path + G_DIR_SEPARATOR_S + "made" + G_DIR_SEPARATOR_S + "deeper").c_str());
    g_rmdir((dir.path + G_DIR_SEPARATOR_S + "made").c_str());
}

#ifndef G_OS_WIN32
// IO2: a write error while rendering (full disk, lost share) must report failure and leave the
// previous good file byte-identical. Seam: RLIMIT_FSIZE makes the write fail part-way, as a full disk does.
TEST_F(FileOutputTest, FailedPdfWriteLeavesExistingDestinationIntact)
{
    auto doc = SPDocument::createNewDocFromMem(mem(SMALL_SVG));
    ASSERT_TRUE(doc);
    ASSERT_TRUE(pdf_output());
    TempDir dir;
    auto const dest = dir.file("out.pdf");
    std::string const good = "%PDF-1.5 previous good file\n";
    ASSERT_TRUE(g_file_set_contents(dest.c_str(), good.c_str(), good.size(), nullptr));

    auto const old_handler = std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit before{};
    ASSERT_EQ(getrlimit(RLIMIT_FSIZE, &before), 0);
    struct rlimit tiny = before;
    tiny.rlim_cur = 100;
    ASSERT_EQ(setrlimit(RLIMIT_FSIZE, &tiny), 0);

    Inkscape::Extension::Internal::CairoRendererPdfOutput imp;
    bool threw = false;
    try {
        imp.save(pdf_output(), doc.get(), dest.c_str());
    } catch (Inkscape::Extension::Output::save_failed const &) {
        threw = true;
    }
    setrlimit(RLIMIT_FSIZE, &before);
    std::signal(SIGXFSZ, old_handler);

    EXPECT_TRUE(threw) << "a failed write must be reported";
    EXPECT_EQ(read_file(dest), good);
    EXPECT_EQ(dir.entries(), std::vector<std::string>{"out.pdf"}) << "temporary must be removed";
}

// R6: a filtered object that cannot be rasterized for a PDF must fail the export with a message,
// not silently disappear from the print file.
TEST_F(FileOutputTest, PdfExportFailsWhenFilteredObjectCannotBeRasterized)
{
    // 400 px at 10000 dpi would be a 41,667 px wide bitmap, past Cairo's 32767 px surface limit.
    char const *svg =
        "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='100' viewBox='0 0 400 100'>"
        "<defs><filter id='b'><feGaussianBlur stdDeviation='2'/></filter></defs>"
        "<rect id='blurred' x='0' y='0' width='400' height='100' style='fill:#ff0000;filter:url(#b)'/></svg>";
    auto doc = SPDocument::createNewDocFromMem(mem(svg));
    ASSERT_TRUE(doc);
    TempDir dir;

    doc->ensureUpToDate();
    // The renderer reports the failure by name, and finish() fails.
    {
        using namespace Inkscape::Extension::Internal;
        CairoRenderer renderer;
        CairoRenderContext ctx = renderer.createContext();
        ctx.setPDFLevel(0);
        ctx.setFilterToBitmap(true);
        ctx.setBitmapResolution(10000);
        auto const pdf = dir.file("direct.pdf");
        ASSERT_TRUE(ctx.setPdfTarget(("> " + pdf).c_str()));
        ASSERT_TRUE(renderer.setupDocument(&ctx, doc.get(), doc->getRoot()));
        renderer.renderPages(&ctx, doc.get(), false);
        EXPECT_FALSE(ctx.finish()) << "an incomplete document must not be reported as a success";
        EXPECT_NE(renderer.failure().find("blurred"), std::string::npos) << renderer.failure();
        EXPECT_NE(renderer.failure().find("rasterized"), std::string::npos) << renderer.failure();
    }

    // Through the real output module: the save fails and the existing file is untouched.
    ASSERT_TRUE(pdf_output());
    auto *mod = pdf_output();
    mod->set_param_bool("blurToBitmap", true);
    mod->set_param_int("resolution", 10000);
    auto const dest = dir.file("out.pdf");
    ASSERT_TRUE(g_file_set_contents(dest.c_str(), "OLD-GOOD-PDF", -1, nullptr));
    Inkscape::Extension::Internal::CairoRendererPdfOutput imp;
    EXPECT_THROW(imp.save(mod, doc.get(), dest.c_str()), Inkscape::Extension::Output::save_failed);
    EXPECT_EQ(read_file(dest), "OLD-GOOD-PDF");

    // Control: at a normal resolution the same document exports, with the blurred object rasterized.
    mod->set_param_int("resolution", 96);
    EXPECT_NO_THROW(imp.save(mod, doc.get(), dest.c_str()));
    EXPECT_EQ(read_file(dest).substr(0, 5), "%PDF-");
}
#endif
#endif

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
