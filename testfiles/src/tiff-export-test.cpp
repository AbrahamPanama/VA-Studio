// SPDX-License-Identifier: GPL-2.0-or-later

#include "io/tiff-export.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtest/gtest.h>
#include <lcms2.h>
#include <png.h>
#include <tiffio.h>

#ifndef G_OS_WIN32
#include <sys/stat.h>
#ifndef UF_HIDDEN
#define UF_HIDDEN 0x8000
#endif
#endif

#ifdef G_OS_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        GError *error = nullptr;
        auto *directory = g_dir_make_tmp("inkscape-tiff-export-test-XXXXXX", &error);
        if (directory) {
            _path = directory;
            g_free(directory);
        }
        if (error) {
            _error = error->message;
            g_error_free(error);
        }
    }

    ~TemporaryDirectory()
    {
        for (auto const &file : _files) {
            g_remove(file.c_str());
        }
        for (auto it = _directories.rbegin(); it != _directories.rend(); ++it) {
            g_rmdir(it->c_str());
        }
        if (!_path.empty()) {
            g_rmdir(_path.c_str());
        }
    }

    std::string file(std::string const &name)
    {
        auto const path = _path + G_DIR_SEPARATOR_S + name;
        _files.push_back(path);
        return path;
    }

    bool valid() const { return !_path.empty(); }
    std::string const &error() const { return _error; }
    std::string const &path() const { return _path; }

    bool subdirectory(std::string const &name)
    {
        auto path = _path + G_DIR_SEPARATOR_S + name;
        if (g_mkdir(path.c_str(), 0700) != 0) return false;
        _directories.push_back(std::move(path));
        return true;
    }

private:
    std::string _path;
    std::string _error;
    std::vector<std::string> _files;
    std::vector<std::string> _directories;
};

struct PngWriteGuard
{
    png_structp png = nullptr;
    png_infop info = nullptr;
    FILE *file = nullptr;

    ~PngWriteGuard()
    {
        if (png || info) {
            png_destroy_write_struct(png ? &png : nullptr, info ? &info : nullptr);
        }
        if (file) {
            std::fclose(file);
        }
    }
};

bool write_png(std::string const &path, std::vector<unsigned char> const &pixels, std::uint32_t width,
               std::uint32_t height, double x_dpi, double y_dpi, bool include_resolution = true)
{
    PngWriteGuard guard;
    guard.file = g_fopen(path.c_str(), "wb");
    if (!guard.file) {
        return false;
    }
    guard.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!guard.png) {
        return false;
    }
    guard.info = png_create_info_struct(guard.png);
    if (!guard.info) {
        return false;
    }
    if (setjmp(png_jmpbuf(guard.png))) {
        return false;
    }

    png_init_io(guard.png, guard.file);
    png_set_IHDR(guard.png, guard.info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    if (include_resolution) {
        png_set_pHYs(guard.png, guard.info, static_cast<png_uint_32>(std::lround(x_dpi / 0.0254)),
                     static_cast<png_uint_32>(std::lround(y_dpi / 0.0254)), PNG_RESOLUTION_METER);
    }
    png_write_info(guard.png, guard.info);
    for (std::uint32_t row = 0; row < height; ++row) {
        auto *data = const_cast<png_bytep>(pixels.data() + static_cast<std::size_t>(row) * width * 4u);
        png_write_row(guard.png, data);
    }
    png_write_end(guard.png, guard.info);
    return true;
}

bool write_rgb_output_profile(std::string const &path)
{
    cmsCIExyY white{};
    if (!cmsWhitePointFromTemp(&white, 6504)) {
        return false;
    }
    cmsCIExyYTRIPLE const primaries = {
        {0.64, 0.33, 1.0},
        {0.30, 0.60, 1.0},
        {0.15, 0.06, 1.0},
    };
    cmsToneCurve *curves[] = {
        cmsBuildGamma(nullptr, 1.8),
        cmsBuildGamma(nullptr, 1.8),
        cmsBuildGamma(nullptr, 1.8),
    };
    if (!curves[0] || !curves[1] || !curves[2]) {
        for (auto *curve : curves) {
            if (curve)
                cmsFreeToneCurve(curve);
        }
        return false;
    }

    auto profile = cmsCreateRGBProfile(&white, &primaries, curves);
    for (auto *curve : curves) {
        cmsFreeToneCurve(curve);
    }
    if (!profile) {
        return false;
    }
    cmsSetDeviceClass(profile, cmsSigOutputClass);
    cmsSetHeaderRenderingIntent(profile, INTENT_RELATIVE_COLORIMETRIC);
    cmsUInt32Number size = 0;
    auto result = cmsSaveProfileToMem(profile, nullptr, &size) != 0;
    std::vector<unsigned char> contents(size);
    result = result && cmsSaveProfileToMem(profile, contents.data(), &size) &&
             g_file_set_contents(path.c_str(), reinterpret_cast<char const *>(contents.data()), size, nullptr);
    cmsCloseProfile(profile);
    return result;
}

std::vector<unsigned char> read_file(std::string const &path)
{
    gchar *contents = nullptr;
    gsize size = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &size, nullptr)) return {};
    std::vector<unsigned char> result(contents, contents + size);
    g_free(contents);
    return result;
}

bool has_tiff_magic(std::vector<unsigned char> const &bytes)
{
    return bytes.size() >= 4 &&
           ((bytes[0] == 'I' && bytes[1] == 'I' && bytes[2] == 0x2a && bytes[3] == 0x00) ||
            (bytes[0] == 'M' && bytes[1] == 'M' && bytes[2] == 0x00 && bytes[3] == 0x2a));
}

std::vector<std::string> directory_entries(std::string const &directory)
{
    std::vector<std::string> names;
    GError *error = nullptr;
    auto *handle = g_dir_open(directory.c_str(), 0, &error);
    if (!handle) {
        if (error) {
            g_error_free(error);
        }
        return names;
    }
    while (auto const *name = g_dir_read_name(handle)) {
        names.emplace_back(name);
    }
    g_dir_close(handle);
    return names;
}

std::array<unsigned char, 4> transform_pixel(std::string const &profile_path, std::array<unsigned char, 4> input)
{
    std::array<unsigned char, 4> output{};
    auto source = cmsCreate_sRGBProfile();
    auto destination = cmsOpenProfileFromFile(profile_path.c_str(), "r");
    auto transform = cmsCreateTransform(source, TYPE_RGBA_8, destination, TYPE_RGBA_8, INTENT_RELATIVE_COLORIMETRIC,
                                        cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_COPY_ALPHA);
    if (transform) {
        cmsDoTransform(transform, input.data(), output.data(), 1);
        cmsDeleteTransform(transform);
    }
    if (destination)
        cmsCloseProfile(destination);
    if (source)
        cmsCloseProfile(source);
    return output;
}

class TiffExportTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(directory.valid()) << directory.error();
        png_path = directory.file("input.png");
        profile_path = directory.file("output.icc");
        tiff_path = directory.file("output.tiff");
        ASSERT_TRUE(write_rgb_output_profile(profile_path));
    }

    TemporaryDirectory directory;
    std::string png_path;
    std::string profile_path;
    std::string tiff_path;
};

TEST_F(TiffExportTest, ConvertsRgbPreservesStraightAlphaAndEmbedsProfile)
{
    std::vector<unsigned char> const input = {
        0xcc, 0x99, 0x33, 0xff, 0x20, 0x80, 0xe0, 0x80, 0xfa, 0x10, 0x60, 0x00, 0xff, 0xff, 0xff, 0x40,
    };
    ASSERT_TRUE(write_png(png_path, input, 2, 2, 300.0, 600.0));

    std::string error;
    Inkscape::IO::TiffExportInfo info;
    ASSERT_TRUE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error, &info))
        << error;
    EXPECT_EQ(info.width, 2u);
    EXPECT_EQ(info.height, 2u);
    EXPECT_DOUBLE_EQ(info.x_dpi, 300.0);
    EXPECT_DOUBLE_EQ(info.y_dpi, 600.0);

    auto *tiff = TIFFOpen(tiff_path.c_str(), "r");
    ASSERT_NE(tiff, nullptr);

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t bits = 0;
    std::uint16_t samples = 0;
    std::uint16_t compression = 0;
    std::uint16_t photometric = 0;
    std::uint16_t planar = 0;
    std::uint16_t resolution_unit = 0;
    float x_dpi = 0.0f;
    float y_dpi = 0.0f;
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width));
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height));
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bits));
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples));
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression));
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_PHOTOMETRIC, &photometric));
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_PLANARCONFIG, &planar));
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_RESOLUTIONUNIT, &resolution_unit));
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_XRESOLUTION, &x_dpi));
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_YRESOLUTION, &y_dpi));
    EXPECT_EQ(width, 2u);
    EXPECT_EQ(height, 2u);
    EXPECT_EQ(bits, 8);
    EXPECT_EQ(samples, 4);
    EXPECT_EQ(compression, COMPRESSION_NONE);
    EXPECT_EQ(photometric, PHOTOMETRIC_RGB);
    EXPECT_EQ(planar, PLANARCONFIG_CONTIG);
    EXPECT_EQ(resolution_unit, RESUNIT_INCH);
    EXPECT_FLOAT_EQ(x_dpi, 300.0f);
    EXPECT_FLOAT_EQ(y_dpi, 600.0f);

    std::uint16_t extra_count = 0;
    std::uint16_t *extra_types = nullptr;
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_EXTRASAMPLES, &extra_count, &extra_types));
    ASSERT_EQ(extra_count, 1);
    ASSERT_NE(extra_types, nullptr);
    EXPECT_EQ(extra_types[0], EXTRASAMPLE_UNASSALPHA);

    std::uint32_t embedded_size = 0;
    void *embedded_data = nullptr;
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &embedded_size, &embedded_data));
    auto const expected_profile = read_file(profile_path);
    ASSERT_EQ(embedded_size, expected_profile.size());
    ASSERT_NE(embedded_data, nullptr);
    EXPECT_EQ(std::memcmp(embedded_data, expected_profile.data(), embedded_size), 0);

    // The exported Software tag is a product label and follows the product name.
    char *software = nullptr;
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_SOFTWARE, &software));
    ASSERT_NE(software, nullptr);
    EXPECT_STREQ(software, "VA Studio");

    std::vector<unsigned char> actual(input.size());
    ASSERT_GE(TIFFReadScanline(tiff, actual.data(), 0, 0), 0);
    ASSERT_GE(TIFFReadScanline(tiff, actual.data() + 8, 1, 0), 0);
    TIFFClose(tiff);

    for (std::size_t pixel = 0; pixel < 4; ++pixel) {
        std::array<unsigned char, 4> source{};
        std::copy_n(input.data() + pixel * 4, 4, source.data());
        auto const expected = transform_pixel(profile_path, source);
        EXPECT_EQ(actual[pixel * 4 + 0], expected[0]);
        EXPECT_EQ(actual[pixel * 4 + 1], expected[1]);
        EXPECT_EQ(actual[pixel * 4 + 2], expected[2]);
        EXPECT_EQ(actual[pixel * 4 + 3], input[pixel * 4 + 3]);
    }
    std::array<unsigned char, 3> const actual_first{actual[0], actual[1], actual[2]};
    std::array<unsigned char, 3> const input_first{input[0], input[1], input[2]};
    EXPECT_NE(actual_first, input_first);
}

// Owner option: print RIPs treat pure white as transparent. Only visible pixels whose
// converted output is exactly 255,255,255 change (to 254,254,254); near-white, colours
// and fully transparent pixels are written exactly as without the option.
TEST_F(TiffExportTest, PreventWhiteClippingChangesOnlyVisiblePureWhite)
{
    std::vector<unsigned char> const input = {
        0xff, 0xff, 0xff, 0xff, // opaque white
        0xff, 0xff, 0xff, 0x80, // half-transparent white
        0xff, 0xff, 0xff, 0x00, // fully transparent white: unchanged
        0xfe, 0xff, 0xff, 0xff, // near white
        0xcc, 0x99, 0x33, 0xff, // colour
        0x00, 0x00, 0x00, 0xff, // black
    };
    ASSERT_TRUE(write_png(png_path, input, 6, 1, 300.0, 300.0));
    // The test profile must really map sRGB white to device pure white, or the
    // option would have nothing to change.
    auto const white = transform_pixel(profile_path, {0xff, 0xff, 0xff, 0xff});
    ASSERT_EQ((std::array<unsigned char, 3>{white[0], white[1], white[2]}),
              (std::array<unsigned char, 3>{255, 255, 255}));

    auto read_row = [&](bool prevent) {
        std::string error;
        Inkscape::IO::TiffExportOptions options;
        options.prevent_white_clipping = prevent;
        EXPECT_TRUE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error,
                                                                   nullptr, options)) << error;
        std::vector<unsigned char> row(input.size());
        auto *tiff = TIFFOpen(tiff_path.c_str(), "r");
        EXPECT_NE(tiff, nullptr);
        if (tiff) {
            EXPECT_GE(TIFFReadScanline(tiff, row.data(), 0, 0), 0);
            TIFFClose(tiff);
        }
        return row;
    };
    auto const off = read_row(false);
    auto const on = read_row(true);
    for (std::size_t pixel = 0; pixel < 6; ++pixel) {
        std::array<unsigned char, 4> source{};
        std::copy_n(input.data() + pixel * 4, 4, source.data());
        auto const expected = transform_pixel(profile_path, source);
        bool const pure_white = expected[0] == 255 && expected[1] == 255 && expected[2] == 255;
        for (std::size_t c = 0; c < 3; ++c) {
            EXPECT_EQ(off[pixel * 4 + c], expected[c]) << "pixel " << pixel;
            unsigned char const want = pure_white && source[3] != 0 ? 254 : expected[c];
            EXPECT_EQ(on[pixel * 4 + c], want) << "pixel " << pixel;
        }
        EXPECT_EQ(on[pixel * 4 + 3], source[3]) << "alpha of pixel " << pixel;
    }
    // Opaque and half-transparent white changed; transparent white did not.
    EXPECT_EQ(on[0], 254); EXPECT_EQ(on[4], 254); EXPECT_EQ(on[8], 255);

    // The experimental second option also covers fully transparent white; alpha stays 0.
    std::string error;
    Inkscape::IO::TiffExportOptions both;
    both.prevent_white_clipping = true;
    both.include_transparent = true;
    ASSERT_TRUE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error,
                                                               nullptr, both)) << error;
    std::vector<unsigned char> all(input.size());
    auto *tiff = TIFFOpen(tiff_path.c_str(), "r");
    ASSERT_NE(tiff, nullptr);
    ASSERT_GE(TIFFReadScanline(tiff, all.data(), 0, 0), 0);
    TIFFClose(tiff);
    EXPECT_EQ(all[8], 254); EXPECT_EQ(all[9], 254); EXPECT_EQ(all[10], 254); EXPECT_EQ(all[11], 0);
    for (std::size_t i = 12; i < all.size(); ++i) EXPECT_EQ(all[i], on[i]) << "byte " << i;
    // Without the main option the second one does nothing.
    Inkscape::IO::TiffExportOptions only_transparent;
    only_transparent.include_transparent = true;
    ASSERT_TRUE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error,
                                                               nullptr, only_transparent)) << error;
    tiff = TIFFOpen(tiff_path.c_str(), "r");
    ASSERT_NE(tiff, nullptr);
    ASSERT_GE(TIFFReadScanline(tiff, all.data(), 0, 0), 0);
    TIFFClose(tiff);
    EXPECT_EQ(all, off);
}

TEST_F(TiffExportTest, UsesNinetySixDpiWhenPngHasNoPhysicalResolution)
{
    std::vector<unsigned char> const input = {0x20, 0x40, 0x80, 0xff};
    ASSERT_TRUE(write_png(png_path, input, 1, 1, 0.0, 0.0, false));

    std::string error;
    Inkscape::IO::TiffExportInfo info;
    ASSERT_TRUE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error, &info))
        << error;
    EXPECT_DOUBLE_EQ(info.x_dpi, 96.0);
    EXPECT_DOUBLE_EQ(info.y_dpi, 96.0);
}

TEST_F(TiffExportTest, SupportsUtf8DirectoryAndFilenamesWithSpaces)
{
    std::string const subdirectory = "Véronica测试 images";
    ASSERT_TRUE(directory.subdirectory(subdirectory));
    png_path = directory.file(subdirectory + G_DIR_SEPARATOR_S + "entrada ñ.png");
    profile_path = directory.file(subdirectory + G_DIR_SEPARATOR_S + "perfil é.icc");
    tiff_path = directory.file(subdirectory + G_DIR_SEPARATOR_S + "salida 测试.tiff");
    ASSERT_TRUE(write_rgb_output_profile(profile_path));
    ASSERT_TRUE(write_png(png_path, {0x20, 0x40, 0x80, 0x80}, 1, 1, 300.0, 300.0));
    std::string error;
    ASSERT_TRUE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error)) << error;
    auto first = read_file(tiff_path);
    ASSERT_FALSE(first.empty());
    // Exercise successful replacement, not just creation of a new file.
    ASSERT_TRUE(write_png(png_path, {0xe0, 0x40, 0x20, 0xff}, 1, 1, 300.0, 300.0));
    ASSERT_TRUE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error)) << error;
    EXPECT_NE(read_file(tiff_path), first);
    auto replacement = read_file(tiff_path);
    ASSERT_TRUE(g_file_set_contents(png_path.c_str(), "invalid PNG", -1, nullptr));
    EXPECT_FALSE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error));
    EXPECT_EQ(read_file(tiff_path), replacement);
}

#ifdef G_OS_WIN32
TEST_F(TiffExportTest, LockedDestinationFailsWithoutDamagingPreviousOutput)
{
    ASSERT_TRUE(write_png(png_path, {0x20, 0x40, 0x80, 0x80}, 1, 1, 300.0, 300.0));
    ASSERT_TRUE(g_file_set_contents(tiff_path.c_str(), "previous TIFF", -1, nullptr));
    auto before = read_file(tiff_path);
    auto wide = g_utf8_to_utf16(tiff_path.c_str(), -1, nullptr, nullptr, nullptr);
    ASSERT_NE(wide, nullptr);
    auto handle = CreateFileW(reinterpret_cast<wchar_t const *>(wide), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_free(wide);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    std::string error;
    auto success = Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error);
    CloseHandle(handle);
    EXPECT_FALSE(success);
    EXPECT_NE(error.find("Windows error"), std::string::npos);
    EXPECT_NE(error.find(tiff_path), std::string::npos);
    EXPECT_EQ(read_file(tiff_path), before);
}
#endif

TEST_F(TiffExportTest, FailureLeavesExistingDestinationUntouched)
{
    std::ofstream(tiff_path, std::ios::binary) << "existing-output";
    auto const before = read_file(tiff_path);
    std::string error;

    EXPECT_FALSE(
        Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, directory.file("missing.icc"), error));
    EXPECT_NE(error.find("Could not read ICC output profile"), std::string::npos);
    EXPECT_EQ(read_file(tiff_path), before);
}

TEST_F(TiffExportTest, RejectsRgbInputProfiles)
{
    auto const display_profile_path = directory.file("display.icc");
    auto display_profile = cmsCreate_sRGBProfile();
    ASSERT_NE(display_profile, nullptr);
    cmsSetDeviceClass(display_profile, cmsSigInputClass);
    ASSERT_TRUE(cmsSaveProfileToFile(display_profile, display_profile_path.c_str()));
    cmsCloseProfile(display_profile);

    std::vector<unsigned char> const input = {0x20, 0x40, 0x80, 0xff};
    ASSERT_TRUE(write_png(png_path, input, 1, 1, 300.0, 300.0));
    std::string error;
    EXPECT_FALSE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, display_profile_path, error));
    EXPECT_NE(error.find("RGB display or output"), std::string::npos);
    EXPECT_FALSE(g_file_test(tiff_path.c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(TiffExportTest, RejectsMalformedPngWithoutPublishingPartialTiff)
{
    std::ofstream(png_path, std::ios::binary) << "not a png";
    std::string error;
    EXPECT_FALSE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error));
    EXPECT_NE(error.find("rendered PNG"), std::string::npos);
    EXPECT_FALSE(g_file_test(tiff_path.c_str(), G_FILE_TEST_EXISTS));
}

// BUG-004: a leading dot in the temporary name makes some SMB/Finder shares
// mark it hidden at creation; the attribute then survives the rename onto the
// final output. The temporary must stay beside the destination (atomic
// same-filesystem rename) and use a short, fixed, non-dot component that is
// independent of the destination basename, so it can never push a legal long
// destination past NAME_MAX.
TEST(TiffExportTemporaryTemplateTest, KeepsDestinationDirectoryWithShortFixedNonDotName)
{
    std::string const destination = "/tmp/vacards dir/salida 测试.tiff";
    auto const temporary = Inkscape::IO::temporary_output_template(destination);

    auto const separator = temporary.find_last_of('/');
    ASSERT_NE(separator, std::string::npos);
    EXPECT_EQ(temporary.substr(0, separator + 1), std::string("/tmp/vacards dir/"));
    auto const basename = temporary.substr(separator + 1);
    ASSERT_FALSE(basename.empty());
    EXPECT_NE(basename.front(), '.');
    EXPECT_EQ(basename, std::string("vacards-tmp-XXXXXX"));
    EXPECT_EQ(basename.substr(basename.size() - 6), std::string("XXXXXX"));

    // The generated component must not grow with the destination basename.
    auto const longer =
        Inkscape::IO::temporary_output_template("/tmp/vacards dir/" + std::string(200, 'a') + ".tiff");
    EXPECT_EQ(longer, temporary);
}

// An explicit dot in the user's destination is a property of the final path,
// which is never rewritten. The generated temporary is intentionally non-dot
// regardless of the destination, so the marker cannot carry a hidden attribute
// onto the output; the real export preserves the dot path (see
// ExplicitDotDestinationIsNotSilentlyUnhiddenOrRenamed).
TEST(TiffExportTemporaryTemplateTest, ExplicitDotDestinationDoesNotLeakIntoGeneratedTempName)
{
    auto const temporary = Inkscape::IO::temporary_output_template("/tmp/.hidden output.tiff");
    EXPECT_EQ(temporary, std::string("/tmp/vacards-tmp-XXXXXX"));
    auto const basename = temporary.substr(temporary.find_last_of('/') + 1);
    ASSERT_FALSE(basename.empty());
    EXPECT_NE(basename.front(), '.');
    EXPECT_EQ(basename.find("hidden"), std::string::npos);
}

TEST_F(TiffExportTest, ExplicitDotDestinationIsNotSilentlyUnhiddenOrRenamed)
{
    auto const dot_path = directory.file(".hidden output.tiff");
    auto const plain_twin = directory.file("hidden output.tiff");
    std::vector<unsigned char> const input = {0x10, 0x20, 0x30, 0xff};
    ASSERT_TRUE(write_png(png_path, input, 1, 1, 300.0, 300.0));

    std::string error;
    ASSERT_TRUE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, dot_path, profile_path, error)) << error;

    EXPECT_TRUE(g_file_test(dot_path.c_str(), G_FILE_TEST_IS_REGULAR));
    EXPECT_FALSE(g_file_test(plain_twin.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_TRUE(has_tiff_magic(read_file(dot_path)));
}

#ifndef G_OS_WIN32
TEST_F(TiffExportTest, ExportOutcomeIsVisibleAndLeavesNoTemporaryFile)
{
    std::vector<unsigned char> const input = {
        0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x40,
        0x00, 0x00, 0xff, 0x80, 0xff, 0xff, 0xff, 0xff,
    };
    ASSERT_TRUE(write_png(png_path, input, 2, 2, 300.0, 300.0));

    std::string error;
    ASSERT_TRUE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error)) << error;
    EXPECT_TRUE(has_tiff_magic(read_file(tiff_path)));

    // Straight (unassociated) alpha must survive the real export path.
    auto *tiff = TIFFOpen(tiff_path.c_str(), "r");
    ASSERT_NE(tiff, nullptr);
    std::vector<unsigned char> actual(input.size());
    ASSERT_GE(TIFFReadScanline(tiff, actual.data(), 0, 0), 0);
    ASSERT_GE(TIFFReadScanline(tiff, actual.data() + 8, 1, 0), 0);
    TIFFClose(tiff);
    EXPECT_EQ(actual[3], input[3]);
    EXPECT_EQ(actual[7], input[7]);
    EXPECT_EQ(actual[11], input[11]);
    EXPECT_EQ(actual[15], input[15]);

#ifdef __APPLE__
    struct stat st{};
    ASSERT_EQ(stat(tiff_path.c_str(), &st), 0);
    EXPECT_EQ(st.st_flags & UF_HIDDEN, 0u);
#endif

    for (auto const &name : directory_entries(directory.path())) {
        EXPECT_EQ(name.find("vacards-tmp-"), std::string::npos) << "temporary file left behind: " << name;
        EXPECT_NE(name.front(), '.') << "dot-prefixed sibling left behind: " << name;
    }
}

// Behavioral guard that actually observes the temporary name produced by the
// real export: it must be the short, fixed, non-dot component and must not
// embed the destination basename. The destination is large enough that the
// temporary exists long enough to be enumerated from a second thread.
TEST_F(TiffExportTest, ActualExportTemporaryNameIsNotDotPrefixed)
{
    constexpr std::uint32_t side = 1024;
    std::vector<unsigned char> input(static_cast<std::size_t>(side) * side * 4u, 0x40);
    for (std::size_t alpha = 3; alpha < input.size(); alpha += 4) {
        input[alpha] = 0xff;
    }
    ASSERT_TRUE(write_png(png_path, input, side, side, 300.0, 300.0));

    std::set<std::string> observed;
    for (int attempt = 0; attempt < 3 && observed.empty(); ++attempt) {
        std::string error;
        bool ok = false;
        std::atomic<bool> finished{false};
        std::thread exporter([&] {
            ok = Inkscape::IO::export_png_to_color_managed_tiff(png_path, tiff_path, profile_path, error);
            finished.store(true, std::memory_order_release);
        });

        while (!finished.load(std::memory_order_acquire)) {
            for (auto const &name : directory_entries(directory.path())) {
                if (name != "input.png" && name != "output.icc" && name != "output.tiff") {
                    observed.insert(name);
                }
            }
        }
        exporter.join();
        ASSERT_TRUE(ok) << error;
    }

    ASSERT_FALSE(observed.empty()) << "temporary file was never observed";
    for (auto const &name : observed) {
        EXPECT_NE(name.front(), '.') << "dot-prefixed temporary observed: " << name;
        EXPECT_EQ(name.compare(0, 12, "vacards-tmp-"), 0)
            << "temporary is not the fixed short component: " << name;
        EXPECT_EQ(name.find("output.tiff"), std::string::npos)
            << "temporary still embeds the destination basename: " << name;
    }
}

// Regression for the BUG-004 follow-up: a legal ~240-byte destination basename
// (under NAME_MAX=255) used to make "<basename>.vacards-tmp-XXXXXX" exceed
// NAME_MAX and fail ENAMETOOLONG. The fixed short component keeps the export
// working, leaves no temporary behind and publishes a reopenable TIFF.
TEST_F(TiffExportTest, LongDestinationBasenameStillExportsAndLeavesNoTemporaryFile)
{
    std::string const long_name = std::string(235, 'a') + ".tiff"; // 240 bytes
    auto const long_path = directory.file(long_name);
    ASSERT_EQ(long_name.size(), 240u);
    ASSERT_LT(long_name.size(), 255u);

    ASSERT_TRUE(write_png(png_path, {0x20, 0x40, 0x80, 0xff}, 1, 1, 300.0, 300.0));
    std::string error;
    ASSERT_TRUE(Inkscape::IO::export_png_to_color_managed_tiff(png_path, long_path, profile_path, error)) << error;

    EXPECT_TRUE(has_tiff_magic(read_file(long_path)));
    auto *tiff = TIFFOpen(long_path.c_str(), "r");
    ASSERT_NE(tiff, nullptr);
    std::uint32_t width = 0;
    ASSERT_TRUE(TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width));
    EXPECT_EQ(width, 1u);
    TIFFClose(tiff);

    for (auto const &name : directory_entries(directory.path())) {
        EXPECT_EQ(name.find("vacards-tmp-"), std::string::npos) << "temporary file left behind: " << name;
        if (name != long_name) {
            EXPECT_NE(name.front(), '.') << "dot-prefixed sibling left behind: " << name;
        }
    }
}
#endif

} // namespace
