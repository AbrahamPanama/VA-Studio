// SPDX-License-Identifier: GPL-2.0-or-later

#include "tiff-export.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <lcms2.h>
#include <png.h>
#include <tiffio.h>

#ifdef G_OS_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Inkscape::IO {
namespace {

constexpr double FALLBACK_DPI = 96.0;
constexpr double METRES_PER_INCH = 0.0254;
constexpr double PNG_DPI_QUANTIZATION = METRES_PER_INCH / 2.0;

// Short, fixed, non-dot temporary component. g_mkstemp supplies the unique
// XXXXXX suffix. It is independent of the destination basename so a legal long
// basename can never be pushed past NAME_MAX by the temporary name, and it does
// not begin with a dot because some SMB/Finder shares mark dot names hidden at
// creation and that attribute survives the atomic rename onto the destination.
constexpr char const *TEMPORARY_OUTPUT_NAME = "vacards-tmp-XXXXXX";

struct PngImageGuard
{
    png_image image{};

    PngImageGuard() { image.version = PNG_IMAGE_VERSION; }
    ~PngImageGuard() { png_image_free(&image); }
};

struct CmsProfileGuard
{
    cmsHPROFILE handle = nullptr;

    explicit CmsProfileGuard(cmsHPROFILE profile = nullptr)
        : handle(profile)
    {}
    ~CmsProfileGuard()
    {
        if (handle) {
            cmsCloseProfile(handle);
        }
    }
    CmsProfileGuard(CmsProfileGuard const &) = delete;
    CmsProfileGuard &operator=(CmsProfileGuard const &) = delete;
};

struct CmsTransformGuard
{
    cmsHTRANSFORM handle = nullptr;

    explicit CmsTransformGuard(cmsHTRANSFORM transform = nullptr)
        : handle(transform)
    {}
    ~CmsTransformGuard()
    {
        if (handle) {
            cmsDeleteTransform(handle);
        }
    }
    CmsTransformGuard(CmsTransformGuard const &) = delete;
    CmsTransformGuard &operator=(CmsTransformGuard const &) = delete;
};

struct TiffGuard
{
    TIFF *handle = nullptr;

    explicit TiffGuard(TIFF *tiff = nullptr)
        : handle(tiff)
    {}
    ~TiffGuard()
    {
        if (handle) {
            TIFFClose(handle);
        }
    }
    TiffGuard(TiffGuard const &) = delete;
    TiffGuard &operator=(TiffGuard const &) = delete;
};

struct TemporaryFile
{
    std::string path;
    bool keep = false;

    ~TemporaryFile()
    {
        if (!keep && !path.empty()) {
            g_remove(path.c_str());
        }
    }
};

TIFF *open_tiff(std::string const &path, char const *mode)
{
#ifdef G_OS_WIN32
    // Application paths are UTF-8. TIFFOpen uses the active Windows code page.
    std::unique_ptr<gunichar2, decltype(&g_free)> wide(
        g_utf8_to_utf16(path.c_str(), -1, nullptr, nullptr, nullptr), &g_free);
    return wide ? TIFFOpenW(reinterpret_cast<wchar_t const *>(wide.get()), mode) : nullptr;
#else
    return TIFFOpen(path.c_str(), mode);
#endif
}

bool replace_output(std::string const &source, std::string const &destination, std::string &error)
{
#ifdef G_OS_WIN32
    std::unique_ptr<gunichar2, decltype(&g_free)> wide_source(
        g_utf8_to_utf16(source.c_str(), -1, nullptr, nullptr, nullptr), &g_free);
    std::unique_ptr<gunichar2, decltype(&g_free)> wide_destination(
        g_utf8_to_utf16(destination.c_str(), -1, nullptr, nullptr, nullptr), &g_free);
    if (!wide_source || !wide_destination) {
        error = "Invalid UTF-8 destination path '" + destination + "'";
        return false;
    }
    // Both files are on the same filesystem. Never delete the previous output
    // first, and do not fall back to copying a partial file over it.
    if (!MoveFileExW(reinterpret_cast<wchar_t const *>(wide_source.get()),
                     reinterpret_cast<wchar_t const *>(wide_destination.get()),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not replace destination '" + destination + "': Windows error " +
                std::to_string(GetLastError());
        return false;
    }
#else
    if (g_rename(source.c_str(), destination.c_str()) != 0) {
        error = "Could not replace destination '" + destination + "': " + std::strerror(errno);
        return false;
    }
#endif
    return true;
}

std::uint32_t read_be32(unsigned char const *bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

bool read_exact(FILE *file, void *buffer, std::size_t size)
{
    return std::fread(buffer, 1, size, file) == size;
}

std::pair<double, double> read_png_dpi(std::string const &path)
{
    std::unique_ptr<FILE, decltype(&std::fclose)> file(g_fopen(path.c_str(), "rb"), &std::fclose);
    if (!file) {
        return {FALLBACK_DPI, FALLBACK_DPI};
    }

    std::array<unsigned char, 8> signature{};
    constexpr std::array<unsigned char, 8> png_signature{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    if (!read_exact(file.get(), signature.data(), signature.size()) || signature != png_signature) {
        return {FALLBACK_DPI, FALLBACK_DPI};
    }

    while (true) {
        std::array<unsigned char, 8> header{};
        if (!read_exact(file.get(), header.data(), header.size())) {
            break;
        }

        auto const length = read_be32(header.data());
        char const type[] = {static_cast<char>(header[4]), static_cast<char>(header[5]), static_cast<char>(header[6]),
                             static_cast<char>(header[7]), '\0'};

        if (std::strcmp(type, "pHYs") == 0 && length == 9) {
            std::array<unsigned char, 9> data{};
            if (!read_exact(file.get(), data.data(), data.size())) {
                break;
            }
            if (data[8] == PNG_RESOLUTION_METER) {
                auto const x_dpi = static_cast<double>(read_be32(data.data())) * METRES_PER_INCH;
                auto const y_dpi = static_cast<double>(read_be32(data.data() + 4)) * METRES_PER_INCH;
                if (x_dpi > 0.0 && y_dpi > 0.0) {
                    return {x_dpi, y_dpi};
                }
            }
            break;
        }

        // pHYs must precede IDAT. Do not seek through compressed image data.
        if (std::strcmp(type, "IDAT") == 0 || std::strcmp(type, "IEND") == 0) {
            break;
        }

        auto const bytes_to_skip = static_cast<std::uint64_t>(length) + 4; // data + CRC
        if (bytes_to_skip > static_cast<std::uint64_t>(std::numeric_limits<long>::max()) ||
            std::fseek(file.get(), static_cast<long>(bytes_to_skip), SEEK_CUR) != 0) {
            break;
        }
    }

    return {FALLBACK_DPI, FALLBACK_DPI};
}

double normalize_png_dpi(double dpi)
{
    // PNG stores pixels per metre as an integer, so values such as 300 dpi
    // round-trip as 299.9994. Restore intended whole-number DPI when the
    // difference is no larger than one half of a pHYs unit.
    auto const nearest_integer = std::round(dpi);
    return std::abs(dpi - nearest_integer) <= PNG_DPI_QUANTIZATION ? nearest_integer : dpi;
}

bool read_file(std::string const &path, std::vector<unsigned char> &contents, std::string &error)
{
    gchar *raw = nullptr;
    gsize size = 0;
    GError *gerror = nullptr;
    if (!g_file_get_contents(path.c_str(), &raw, &size, &gerror)) {
        error = "Could not read ICC output profile '" + path + "': " + (gerror ? gerror->message : "unknown error");
        g_clear_error(&gerror);
        return false;
    }

    auto const *first = reinterpret_cast<unsigned char const *>(raw);
    contents.assign(first, first + size);
    g_free(raw);
    return true;
}

bool decode_png(std::string const &path, std::vector<unsigned char> &pixels, std::uint32_t &width,
                std::uint32_t &height, std::string &error)
{
    std::unique_ptr<FILE, decltype(&std::fclose)> file(g_fopen(path.c_str(), "rb"), &std::fclose);
    if (!file) {
        error = "Could not open rendered PNG '" + path + "': " + std::strerror(errno);
        return false;
    }

    PngImageGuard png;
    if (!png_image_begin_read_from_stdio(&png.image, file.get())) {
        error = "Could not read rendered PNG header: " + std::string(png.image.message);
        return false;
    }

    png.image.format = PNG_FORMAT_RGBA;
    auto const byte_count = PNG_IMAGE_SIZE(png.image);
    if (png.image.width == 0 || png.image.height == 0 || byte_count == 0 || byte_count > pixels.max_size()) {
        error = "Rendered PNG dimensions are invalid or too large";
        return false;
    }

    try {
        pixels.resize(byte_count);
    } catch (std::bad_alloc const &) {
        error = "Not enough memory to decode the rendered PNG";
        return false;
    }

    if (!png_image_finish_read(&png.image, nullptr, pixels.data(), 0, nullptr)) {
        error = "Could not decode rendered PNG: " + std::string(png.image.message);
        return false;
    }

    width = png.image.width;
    height = png.image.height;
    return true;
}

bool create_temporary_output(std::string const &destination, TemporaryFile &temporary, std::string &error,
                             int *failure_errno = nullptr)
{
    temporary.path = temporary_output_template(destination);
    if (temporary.path.empty()) {
        error = "Could not build a temporary file name beside '" + destination + "'";
        return false;
    }

    int const fd = g_mkstemp_full(temporary.path.data(), O_RDWR, 0666);
    if (fd < 0) {
        int const code = errno;
        if (failure_errno) {
            *failure_errno = code;
        }
        error = "Could not create a temporary file beside '" + destination + "': " + std::strerror(code);
        temporary.path.clear();
        return false;
    }

    g_close(fd, nullptr);
    return true;
}

bool set_required_tiff_fields(TIFF *tiff, std::uint32_t width, std::uint32_t height, double x_dpi, double y_dpi,
                              std::vector<unsigned char> const &profile, std::string &error)
{
    std::uint16_t const extra_sample = EXTRASAMPLE_UNASSALPHA;
    bool const ok = TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width) && TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height) &&
                    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8) && TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 4) &&
                    TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT) &&
                    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) &&
                    TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) &&
                    TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE) &&
                    TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) &&
                    TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, 1, &extra_sample) &&
                    TIFFSetField(tiff, TIFFTAG_XRESOLUTION, static_cast<float>(x_dpi)) &&
                    TIFFSetField(tiff, TIFFTAG_YRESOLUTION, static_cast<float>(y_dpi)) &&
                    TIFFSetField(tiff, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH) &&
                    TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tiff, 0)) &&
                    TIFFSetField(tiff, TIFFTAG_SOFTWARE, "VA Studio") &&
                    TIFFSetField(tiff, TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(profile.size()), profile.data());

    if (!ok) {
        error = "Could not configure required RGB TIFF metadata";
    }
    return ok;
}

} // namespace

std::string temporary_output_template(std::string const &destination)
{
    std::unique_ptr<gchar, decltype(&g_free)> directory(g_path_get_dirname(destination.c_str()), &g_free);
    // Keep the temporary beside the destination so replace_output() stays an
    // intra-filesystem atomic rename. The component is short, fixed and
    // independent of the destination basename: appending a marker to the
    // basename would push a legal long destination past NAME_MAX. It also does
    // not begin with a dot, because a leading dot makes some SMB/Finder shares
    // set a hidden attribute at creation that the rename would carry onto the
    // final file. The user's destination name is never rewritten, so an
    // explicitly dot-prefixed destination stays exactly as chosen; no attribute
    // is listed or cleared here. g_mkstemp_full() adds the unique O_EXCL suffix.
    std::unique_ptr<gchar, decltype(&g_free)> candidate(
        g_build_filename(directory.get(), TEMPORARY_OUTPUT_NAME, nullptr), &g_free);
    return candidate ? std::string(candidate.get()) : std::string();
}

bool write_file_atomically(std::string const &destination,
                           std::function<bool(std::string const &temporary_path)> const &render,
                           std::string &error)
{
    if (destination.empty()) {
        error = "Empty output destination";
        return false;
    }

    auto const run = [&](std::string const &path) {
        try {
            return render(path);
        } catch (std::exception const &e) {
            error = std::string("Output failed: ") + e.what();
        } catch (...) {
            error = "Output failed";
        }
        return false;
    };

    // "-" is standard output, not a file to stage and rename.
    if (destination == "-") {
        bool const ok = run(destination);
        if (!ok && error.empty()) {
            error = "Could not write to standard output";
        }
        return ok;
    }

    // A missing parent directory is created, as opening the destination directly did.
    {
        std::unique_ptr<gchar, decltype(&g_free)> directory(g_path_get_dirname(destination.c_str()), &g_free);
        if (directory && !g_file_test(directory.get(), G_FILE_TEST_IS_DIR)) {
            g_mkdir_with_parents(directory.get(), 0777);
        }
    }

    TemporaryFile temporary;
    int create_errno = 0;
    if (!create_temporary_output(destination, temporary, error, &create_errno)) {
        if (create_errno == EACCES || create_errno == EPERM || create_errno == EROFS) {
            // The folder does not allow creating files but may allow overwriting the existing one
            // (common on shared folders): write the destination directly, as before staging existed.
            error.clear();
            bool const ok = run(destination);
            if (!ok && error.empty()) {
                error = "Could not write '" + destination + "'";
            }
            return ok;
        }
        return false;
    }
    if (!run(temporary.path)) {
        if (error.empty()) {
            error = "Could not write '" + destination + "'";
        }
        return false;
    }
#ifndef G_OS_WIN32
    // The replacement keeps the permissions of the file it replaces.
    GStatBuf existing;
    if (g_stat(destination.c_str(), &existing) == 0 && S_ISREG(existing.st_mode)) {
        g_chmod(temporary.path.c_str(), existing.st_mode & 07777);
    }
#endif
    if (!replace_output(temporary.path, destination, error)) {
        return false;
    }
    temporary.keep = true; // renamed onto the destination; nothing left to remove
    return true;
}

unsigned rip_edge_rings_for_dpi(double dpi)
{
    if (!std::isfinite(dpi)) {
        return 4;
    }
    return static_cast<unsigned>(std::clamp(std::lround(dpi / 72.0), 4L, 64L));
}

std::size_t clean_rgba_edges_for_rip(std::vector<unsigned char> &rgba, std::uint32_t width, std::uint32_t height,
                                     bool clean_edges, bool hard_edges, unsigned rings)
{
    auto const count = static_cast<std::size_t>(width) * height;
    if (count == 0 || rgba.size() < count * 4u) {
        return 0;
    }
    rings = std::clamp(rings, 1u, 64u);
    std::size_t changed = 0;
    if (clean_edges || hard_edges) {
        // One byte per pixel. First: chessboard distance to the nearest fully
        // transparent pixel or the image border (a tight crop ends there),
        // capped at rings + 1, in two passes. Only outline pixels (distance <=
        // rings) change: the interior of translucent artwork (a 50% panel, a
        // wide shadow, the middle of a gradient) is farther away and keeps its
        // colour.
        std::vector<unsigned char> state(count);
        auto const beyond = static_cast<unsigned char>(rings + 1);
        for (std::size_t p = 0; p < count; ++p) {
            state[p] = rgba[p * 4 + 3] == 0 ? 0 : beyond;
        }
        auto const relax = [&](std::size_t p, std::int64_t x, std::int64_t y) {
            unsigned char through = 1; // outside the image
            if (x >= 0 && x < width && y >= 0 && y < height) {
                through = state[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] + 1;
            }
            state[p] = std::min(state[p], through);
        };
        for (std::int64_t y = 0; y < height; ++y) {
            for (std::int64_t x = 0; x < width; ++x) {
                auto const p = static_cast<std::size_t>(y) * width + x;
                relax(p, x - 1, y - 1), relax(p, x, y - 1), relax(p, x + 1, y - 1), relax(p, x - 1, y);
            }
        }
        for (std::int64_t y = height - 1; y >= 0; --y) {
            for (std::int64_t x = width - 1; x >= 0; --x) {
                auto const p = static_cast<std::size_t>(y) * width + x;
                relax(p, x + 1, y + 1), relax(p, x, y + 1), relax(p, x - 1, y + 1), relax(p, x + 1, y);
            }
        }
        // Then, in place: 1 for opaque pixels, 0 for outline pixels still to
        // colour, `excluded` for the rest.
        constexpr unsigned char excluded = 255;
        for (std::size_t p = 0; p < count; ++p) {
            auto const alpha = rgba[p * 4 + 3];
            state[p] = alpha == 255 ? 1 : alpha != 0 && state[p] <= rings ? 0 : excluded;
        }
        // Carry the artwork colour outwards through the connected outline, one
        // ring at a time. A pixel coloured in ring k gets k + 1, so a ring only
        // reads pixels settled before it and can write in place.
        for (unsigned k = 1; k <= rings; ++k) {
            bool any = false;
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    auto const p = static_cast<std::size_t>(y) * width + x;
                    if (state[p] != 0) {
                        continue;
                    }
                    unsigned sum[3] = {0, 0, 0};
                    unsigned found = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        auto const ny = static_cast<std::int64_t>(y) + dy;
                        if (ny < 0 || ny >= height) continue;
                        for (int dx = -1; dx <= 1; ++dx) {
                            auto const nx = static_cast<std::int64_t>(x) + dx;
                            if ((dx == 0 && dy == 0) || nx < 0 || nx >= width) continue;
                            auto const q = static_cast<std::size_t>(ny) * width + static_cast<std::size_t>(nx);
                            if (state[q] != 0 && state[q] <= k) {
                                sum[0] += rgba[q * 4];
                                sum[1] += rgba[q * 4 + 1];
                                sum[2] += rgba[q * 4 + 2];
                                ++found;
                            }
                        }
                    }
                    if (found == 0) {
                        continue;
                    }
                    auto *pixel = rgba.data() + p * 4;
                    for (int c = 0; c < 3; ++c) {
                        auto const value = static_cast<unsigned char>((sum[c] + found / 2) / found);
                        if (pixel[c] != value) {
                            pixel[c] = value;
                            ++changed;
                        }
                    }
                    state[p] = static_cast<unsigned char>(k + 1);
                    any = true;
                }
            }
            if (!any) {
                break;
            }
        }
    }
    for (std::size_t i = 0; i < count * 4u; i += 4) {
        if (hard_edges && rgba[i + 3] != 0 && rgba[i + 3] != 255) {
            rgba[i + 3] = rgba[i + 3] >= 128 ? 255 : 0;
            ++changed;
        }
        if ((clean_edges || hard_edges) && rgba[i + 3] == 0) {
            // No hidden colour under full transparency.
            for (int c = 0; c < 3; ++c) {
                if (rgba[i + c] != 255) {
                    rgba[i + c] = 255;
                    ++changed;
                }
            }
        }
    }
    return changed;
}

bool export_png_to_color_managed_tiff(std::string const &png_path, std::string const &tiff_path,
                                      std::string const &output_profile, std::string &error, TiffExportInfo *info, TiffExportOptions const &options)
{
    error.clear();
    if (png_path.empty() || tiff_path.empty() || output_profile.empty()) {
        error = "PNG, TIFF and ICC profile paths are required";
        return false;
    }

    std::vector<unsigned char> profile_data;
    if (!read_file(output_profile, profile_data, error)) {
        return false;
    }
    if (profile_data.empty() || profile_data.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "ICC output profile is empty or too large";
        return false;
    }

    CmsProfileGuard destination(
        cmsOpenProfileFromMem(profile_data.data(), static_cast<cmsUInt32Number>(profile_data.size())));
    if (!destination.handle) {
        error = "The configured TIFF output profile is not a valid ICC profile";
        return false;
    }
    if (cmsGetColorSpace(destination.handle) != cmsSigRgbData) {
        error = "The configured TIFF output profile is not an RGB profile";
        return false;
    }
    if (cmsGetDeviceClass(destination.handle) != cmsSigOutputClass) {
        error = "The configured TIFF output profile is not an RGB printer/output profile";
        return false;
    }
    if (!cmsIsIntentSupported(destination.handle, INTENT_RELATIVE_COLORIMETRIC, LCMS_USED_AS_OUTPUT)) {
        error = "The configured TIFF output profile does not support relative-colorimetric output";
        return false;
    }

    std::vector<unsigned char> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!decode_png(png_path, pixels, width, height, error)) {
        return false;
    }
    auto const [raw_x_dpi, raw_y_dpi] = read_png_dpi(png_path);
    auto const x_dpi = normalize_png_dpi(raw_x_dpi);
    auto const y_dpi = normalize_png_dpi(raw_y_dpi);
    // Before the ICC conversion: the edge colours come from the sRGB render.
    // The outline band is about 1/72 inch (the fringe of an image as coarse as
    // 72 dpi): 4 pixels up to 288 dpi, 8 at 600 dpi, 17 at 1200 dpi.
    if (options.clean_edges || options.hard_edges) {
        auto const rings = rip_edge_rings_for_dpi(std::max(x_dpi, y_dpi));
        try {
            clean_rgba_edges_for_rip(pixels, width, height, options.clean_edges, options.hard_edges, rings);
        } catch (std::bad_alloc const &) {
            error = "Not enough memory to clean the image edges for printing";
            return false;
        }
    }

    CmsProfileGuard source(cmsCreate_sRGBProfile());
    if (!source.handle) {
        error = "Could not create the sRGB source profile";
        return false;
    }

    constexpr cmsUInt32Number flags = cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_COPY_ALPHA;
    CmsTransformGuard transform(cmsCreateTransform(source.handle, TYPE_RGBA_8, destination.handle, TYPE_RGBA_8,
                                                   INTENT_RELATIVE_COLORIMETRIC, flags));
    if (!transform.handle) {
        error = "Could not create the sRGB-to-output ICC transform";
        return false;
    }

    TemporaryFile temporary;
    if (!create_temporary_output(tiff_path, temporary, error)) {
        return false;
    }

    auto const estimated_size = static_cast<std::uint64_t>(width) * height * 4u + profile_data.size();
    // Classic TIFF uses 32-bit offsets. Leave ample room for directory metadata.
    char const *mode = estimated_size >= 0xe0000000ULL ? "w8" : "w";
    {
        TiffGuard tiff(open_tiff(temporary.path, mode));
        if (!tiff.handle) {
            error = "Could not create TIFF output beside '" + tiff_path + "'";
            return false;
        }
        if (!set_required_tiff_fields(tiff.handle, width, height, x_dpi, y_dpi, profile_data, error)) {
            return false;
        }

        auto const row_size = static_cast<std::size_t>(width) * 4u;
        std::vector<unsigned char> converted;
        try {
            converted.resize(row_size);
        } catch (std::bad_alloc const &) {
            error = "Not enough memory for TIFF color conversion";
            return false;
        }

        for (std::uint32_t row = 0; row < height; ++row) {
            auto const *input = pixels.data() + static_cast<std::size_t>(row) * row_size;
            cmsDoTransform(transform.handle, input, converted.data(), width);
            if (options.prevent_white_clipping) {
                for (std::size_t px = 0; px < row_size; px += 4) {
                    auto *rgba = converted.data() + px;
                    if ((rgba[3] != 0 || options.include_transparent) &&
                        rgba[0] == 255 && rgba[1] == 255 && rgba[2] == 255) {
                        rgba[0] = rgba[1] = rgba[2] = 254;
                    }
                }
            }
            if (TIFFWriteScanline(tiff.handle, converted.data(), row, 0) < 0) {
                error = "Could not write all TIFF image rows";
                return false;
            }
        }

        if (!TIFFFlush(tiff.handle)) {
            error = "Could not finalize TIFF output";
            return false;
        }
    }

    if (!replace_output(temporary.path, tiff_path, error)) {
        return false;
    }
    temporary.keep = true;

    if (info) {
        info->width = width;
        info->height = height;
        info->x_dpi = x_dpi;
        info->y_dpi = y_dpi;
    }
    return true;
}

} // namespace Inkscape::IO
