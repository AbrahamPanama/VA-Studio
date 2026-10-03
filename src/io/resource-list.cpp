// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Filesystem-only resource enumeration, shared by the app and font unit tests.
 *
 * Authors:
 *   MenTaLguY <mental@rydia.net>
 *   Martin Owens <doctormo@gmail.com>
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "resource.h"

#include <glibmm/convert.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <glibmm/stringutils.h>

namespace Inkscape::IO::Resource {

/*
 * Get all the files from a specific path and any sub-dirs, populating &files vector
 *
 * &files - Output list to populate, will be populated with full paths
 * path - The directory to parse, will add nothing if directory doesn't exist
 * extensions - Only add files with these extensions, they must be duplicated
 * exclusions - Exclude files that exactly match these names.
 */
void get_filenames_from_path(std::vector<std::string> &files, std::string const &path,
                             std::vector<const char *> const &extensions, std::vector<const char *> const &exclusions)
{
    if(!Glib::file_test(path, Glib::FileTest::IS_DIR)) {
        return;
    }

    Glib::Dir dir(path);
    std::string file = dir.read_name();
    while (!file.empty()){
        // If not extensions are specified, don't reject ANY files.
        bool reject = !extensions.empty();

        // Unreject any file which has one of the extensions.
        for (auto &ext: extensions) {
	    reject ^= Glib::str_has_suffix(file, ext);
        }

        // Reject any file which matches the exclusions.
        for (auto &exc: exclusions) {
	    reject |= Glib::str_has_prefix(file, exc);
        }

        // Reject any filename which isn't a regular file
        auto filename = Glib::build_filename(path, file);

        if(Glib::file_test(filename, Glib::FileTest::IS_DIR)) {
            get_filenames_from_path(files, filename, extensions, exclusions);
        } else if(Glib::file_test(filename, Glib::FileTest::IS_REGULAR) && !reject) {
            files.push_back(Glib::filename_to_utf8(filename));
        }
        file = dir.read_name();
    }
}

} // namespace Inkscape::IO::Resource
