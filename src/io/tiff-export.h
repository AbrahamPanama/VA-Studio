// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef INKSCAPE_IO_TIFF_EXPORT_H
#define INKSCAPE_IO_TIFF_EXPORT_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Inkscape::IO {

struct TiffExportInfo
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double x_dpi = 0.0;
    double y_dpi = 0.0;
};

struct TiffExportOptions
{
    /// Print RIPs treat pure white as transparent. When set, every visible
    /// pixel (alpha > 0) whose converted output is exactly 255,255,255 is
    /// written as 254,254,254. Other colours and fully transparent pixels are
    /// unchanged. Applied after the ICC conversion, so the profile cannot
    /// re-create pure white.
    bool prevent_white_clipping = false;
    /// With prevent_white_clipping, also write fully transparent pure white
    /// (alpha 0) as 254,254,254, for RIPs that ignore alpha. Alpha is unchanged.
    bool include_transparent = false;
    /// Clean edges for RIP printing: the semi-transparent outline of opaque
    /// artwork (pixels within about 1/72 inch of full transparency or the
    /// image border: 4 pixels up to 288 dpi, 8 at 600 dpi, 17 at 1200 dpi)
    /// takes the artwork's colour, alpha unchanged; every fully transparent
    /// pixel is written white. Translucent interiors (a 50% panel, a wide
    /// shadow, a gradient) keep their colour.
    /// Images made elsewhere often carry dark or stray colours in these edge
    /// pixels; viewers hide them, but RIPs that print any pixel with some
    /// opacity show them as a dark outline.
    bool clean_edges = false;
    /// Hard edges: alpha >= 128 becomes opaque and the rest fully transparent,
    /// for RIPs that mishandle semi-transparency. Always cleans first.
    bool hard_edges = false;
};

/**
 * The clean_edges / hard_edges pass of TiffExportOptions over straight
 * (unassociated) 8-bit RGBA pixels, row-major, width * height * 4 bytes.
 * \a rings is the outline band width in pixels (1 to 64). Only semi-transparent
 * pixels connected to opaque artwork through the band and within \a rings of a
 * fully transparent pixel or the image border change. Returns how many samples
 * (colour or alpha bytes) changed. May throw std::bad_alloc (one byte per
 * pixel of working memory).
 */
std::size_t clean_rgba_edges_for_rip(std::vector<unsigned char> &rgba, std::uint32_t width, std::uint32_t height,
                                     bool clean_edges, bool hard_edges, unsigned rings = 4);

/// The outline band for an export resolution: about 1/72 inch (the fringe of
/// an image as coarse as 72 dpi), 4 to 64 pixels.
unsigned rip_edge_rings_for_dpi(double dpi);

/**
 * Convert an Inkscape-rendered PNG into an RGB TIFF whose samples have been
 * converted from sRGB to @p output_profile. The output profile is embedded in
 * TIFFTAG_ICCPROFILE and alpha remains straight (unassociated).
 *
 * Paths are UTF-8, including on Windows. The destination is replaced only
 * after the complete TIFF has been written and closed. Replacement failure
 * leaves an existing destination untouched.
 */
bool export_png_to_color_managed_tiff(std::string const &png_path, std::string const &tiff_path,
                                      std::string const &output_profile, std::string &error,
                                      TiffExportInfo *info = nullptr, TiffExportOptions const &options = {});

/**
 * Return the filename template used for the temporary file written beside
 * @p destination before the atomic replacement.
 *
 * The temporary stays in the destination directory (same filesystem, so the
 * final replacement remains a single atomic rename) and uses a short, fixed,
 * non-dot component that does not depend on the destination basename. Appending
 * a marker to the basename would push a legal long destination past NAME_MAX;
 * a fixed component cannot. It deliberately does NOT begin with a dot: on some
 * SMB/Finder shares a dot-prefixed name is marked hidden at creation, and that
 * attribute survives the rename onto the final output. The user's own
 * destination name is never rewritten and no file attribute is ever listed or
 * cleared, so an explicitly dot-prefixed destination is preserved exactly as
 * chosen.
 *
 * Exposed so the regression test can verify the naming contract on a local
 * filesystem where the share-specific hidden attribute cannot be reproduced.
 */
std::string temporary_output_template(std::string const &destination);

/**
 * Write a file by staging it beside @p destination and replacing the
 * destination only after @p render reports success. @p render receives the
 * UTF-8 path of an existing, empty temporary sibling (same directory, so the
 * final replacement is a single rename) and must write the complete file there
 * and close it. It returns false, or throws, on any failure.
 *
 * On failure the temporary is removed, an existing destination is left
 * byte-identical, @p error describes the failure and false is returned. The
 * same replacement and temporary naming as the TIFF export is used.
 */
bool write_file_atomically(std::string const &destination,
                           std::function<bool(std::string const &temporary_path)> const &render,
                           std::string &error);

} // namespace Inkscape::IO

#endif // INKSCAPE_IO_TIFF_EXPORT_H
