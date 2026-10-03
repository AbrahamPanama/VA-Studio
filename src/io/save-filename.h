// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_INKSCAPE_IO_SAVE_FILENAME_H
#define SEEN_INKSCAPE_IO_SAVE_FILENAME_H

/*
 * Filename normalization for the Save / Save As / Save Copy chooser.
 *
 * Copyright (C) 2026 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <string>

#include <glib.h>
#include <glibmm/miscutils.h>

namespace Inkscape::IO {

/**
 * Appends @a default_extension to @a path when the basename carries no
 * filename extension.
 *
 * The native save chooser on macOS/Windows can return a suffixless filename
 * because its default filter is "All Files"; without normalization such a path
 * matches no output module and the save is rejected. Detection is
 * basename-aware, so a dot in a parent directory (e.g. "/tmp/x.y/name") is not
 * mistaken for a filename suffix. Any explicit suffix, supported or unknown, is
 * preserved, so a known format is never rewritten and ".svg.svg" is never
 * produced.
 *
 * A leading dot marks a hidden file (".hidden"), not an extension, so such a
 * basename receives the default suffix.
 *
 * This helper never invents a target: it leaves an empty path, an empty default
 * extension, a directory path (trailing separator, including the filesystem
 * root), or a bare "."/".." basename unchanged, so a non-local/unmounted URI
 * (whose get_path() is empty) can never become an unintended ".svg" in the
 * current directory, and a chooser return of "/tmp/dir/" can never become the
 * hidden "/tmp/dir/.svg".
 *
 * @param path               filename or path, modified in place
 * @param default_extension  suffix to append, including the dot (e.g. ".svg")
 * @return true if the extension was appended, false if the path was unchanged
 *         (explicit suffix present, directory/root/"."/".." basename, or
 *         path/default_extension empty).
 */
inline bool append_save_extension_if_missing(std::string &path, std::string const &default_extension)
{
    if (path.empty() || default_extension.empty()) {
        return false;
    }

    // A trailing separator means the chooser returned a directory, not a file
    // (this also covers the filesystem root). Extending it would invent a hidden
    // file such as "/tmp/dir/.svg" or "/.svg"; refuse instead.
    if (G_IS_DIR_SEPARATOR(path.back())) {
        return false;
    }

    auto const basename = Glib::path_get_basename(path);

    // "." and ".." are directory references, not suffixless filenames.
    if (basename.empty() || basename == "." || basename == "..") {
        return false;
    }

    auto const dot = basename.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        return false;
    }

    path += default_extension;
    return true;
}

} // namespace Inkscape::IO

#endif // SEEN_INKSCAPE_IO_SAVE_FILENAME_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
