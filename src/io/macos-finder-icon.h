// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * VIEW-1: Finder custom icon for SVG files saved by VA Studio (macOS only).
 *
 * Finder shows only an SVG's page, so cards whose artwork lies off the page
 * appear blank. A custom icon (stored in the file's com.apple.ResourceFork and
 * com.apple.FinderInfo attributes) replaces that page preview in Finder,
 * including on SMB shares that keep Mac attributes. Other systems ignore it.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_IO_MACOS_FINDER_ICON_H
#define INKSCAPE_IO_MACOS_FINDER_ICON_H

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Inkscape::IO {

/// The file a queued icon is meant for, as it was when the icon was made.
struct FinderIconTarget {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::int64_t size = -1;
    std::int64_t modified_seconds = 0;
    std::uint32_t modified_microseconds = 0;
};

/**
 * Set the PNG image \a png as the Finder custom icon of the file \a path and
 * return whether it was set. With \a target, nothing is done unless the file
 * at \a path is still that file (same device, inode, size and modification
 * time). NSWorkspace moves the modification time to now; the previous access
 * and modification times are restored afterwards on the same open file, and
 * only if the path still names it, so the file keeps its save time (its bytes
 * are not changed). Volumes without native attributes (FAT, exFAT) are
 * skipped, so no "._" files appear there. May block on network volumes.
 */
bool set_finder_icon(std::string const &path, std::span<unsigned char const> png,
                     FinderIconTarget const *target = nullptr);

/**
 * set_finder_icon() on one background thread, in request order, so a slow or
 * unreachable network volume never blocks the window. \a done runs on the
 * main context with the result.
 */
void set_finder_icon_async(std::string path, std::vector<unsigned char> png, FinderIconTarget target,
                           std::function<void(bool)> done);

/**
 * Remove a Finder custom icon from the open file \a fd (the resource fork and
 * the "has custom icon" flag). A save copies the replaced file's attributes,
 * which would otherwise keep showing the previous drawing.
 */
void clear_finder_icon(int fd);

} // namespace Inkscape::IO

#endif // INKSCAPE_IO_MACOS_FINDER_ICON_H
