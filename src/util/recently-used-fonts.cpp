// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * This file contains recently used font list related logic. The functions to manage the
 * recently used fonts are defined in this file.
 *
 * Authors:
 *   Vaibhav Malik <vaibhavmalik2018@gmail.com>
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */

#include "recently-used-fonts.h"
#include "font-collections.h"
#include "libnrtype/font-lister.h"
#include "preferences.h"

#include <exception>
#include <sstream>
#include <glib/gstdio.h>
#include <glibmm/error.h>
#include <iostream>

#ifdef _WIN32
#include<direct.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>

using namespace Inkscape::IO::Resource;

namespace Inkscape {

// get_instance method for the singleton class.
RecentlyUsedFonts* RecentlyUsedFonts::get()
{
    static RecentlyUsedFonts* s_instance = new Inkscape::RecentlyUsedFonts();
    return s_instance;
}

RecentlyUsedFonts::RecentlyUsedFonts()
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    _max_size = prefs->getInt("/tools/text/recently_used_fonts_size", 10);

    init();
}

void RecentlyUsedFonts::init()
{
    // Clear the previous collections(we may be re-reading).
    clear();

    // Generate the name of the file.
    std::string file_path = get_path_string(USER, FONTCOLLECTIONS, RECENTFONTS_FILENAME);
    std::string file_dir = get_path_string(USER, FONTCOLLECTIONS, nullptr);

    static bool create_dir = true;

    if(create_dir) {
        g_mkdir_with_parents(file_dir.c_str(), 0775);
        create_dir = false;
    }

    // Read the file.
    read(file_path);
}

void RecentlyUsedFonts::clear()
{
    _recent_list.clear();
}

/*
void RecentlyUsedFonts::print_recently_used_fonts()
{
    std::cout << std::endl << "********************" << std::endl;

    for(auto const& font: _recent_list) {
        std::cout << font << std::endl;
    }

    std::cout << std::endl << "********************" << std::endl;
}
*/

// Read fonts stored in a collection file.
void RecentlyUsedFonts::read(const Glib::ustring& file_path) {
    try {
        _read(file_path);
    }
    catch (std::exception& ex) {
        std::cerr << "Failed to read recently used fonts file: " << ex.what() << std::endl;
    }
}

void RecentlyUsedFonts::_read(const Glib::ustring& file_path)
{
    gchar *contents = nullptr;
    gsize length = 0;
    if (g_file_get_contents(file_path.c_str(), &contents, &length, nullptr)) {
        std::istringstream input_file(std::string(contents, length));
        g_free(contents);
        // Now read all the fonts stored in this file.
        std::string line;
        FontCollections *font_collections = Inkscape::FontCollections::get();

        while (getline(input_file, line)) {
            // Get rid of unwanted characters from the left and right.
            line = font_collections->trim_left_and_right(line);
            Glib::ustring font = line;

            // Now check if the font is installed on the system because it is possible
            // that a previously installed font may not be available now.
            if (Inkscape::FontLister::get_instance()->font_installed_on_system(font)) {
                _recent_list.push_front(font);
            }
        }

    } else {
        // Error: Failed to open the file.
        // std::cout << "Failed to open file: " << file_path << std::endl;
    }
}

// Function to write the recently used fonts to a file.
void RecentlyUsedFonts::write_recently_used_fonts() {
    try {
        _write_recently_used_fonts();
    }
    catch (std::exception& ex) {
        std::cerr << "Failed to write recently used fonts file: " << ex.what() << std::endl;
    }
}

void RecentlyUsedFonts::_write_recently_used_fonts()
{
    // Step 1: Fetch the collections directory from the system directory.

    // Generate the name of the file.
    std::string file_path = get_path_string(USER, FONTCOLLECTIONS, RECENTFONTS_FILENAME);

    std::ostringstream output;
    for (auto it = _recent_list.rbegin(); it != _recent_list.rend(); ++it) {
        output << (*it) << '\n';
    }
    auto const contents = output.str();
    GError *error = nullptr;
    if (g_file_set_contents(file_path.c_str(), contents.data(), contents.size(), &error)) {
        init();
    } else {
        g_warning("Failed to write recently used fonts: %s", error->message);
        g_clear_error(&error);
    }
}

void RecentlyUsedFonts::change_max_list_size(const int& max_size)
{
    // std::cout << "Changing size from: " << _max_size << " to: " << max_size << "\n";

    if(max_size < 0) {
        std::cerr << "Can not set negative size" << std::endl;
        return;
    }

    _max_size = max_size;

    // If the font that are currently stored in the recent list are more than the
    // new max size of the list, then we need to remove the least recent fonts
    // to make sure that the list if not longer than the max size.
    int difference = _recent_list.size() - _max_size;

    if(difference > 0) {
        while(difference--) {
            _recent_list.pop_back();
        }
    }

    update_signal.emit();
}

// This function is called whenever the user clicks the Apply button in
// the text and font dialog. It inserts the selected family into the
// recently used list and it's correct position.
void RecentlyUsedFonts::prepend_to_list(const Glib::ustring& font_name) {
    /*
     * Look-Up step
     */
    auto it = std::find(_recent_list.begin(), _recent_list.end(), font_name);

    /*
     * If the element is already present
     * Delete it, because we'll re-insert it at the top
     */
     if(it != _recent_list.end()) {
         _recent_list.erase(it);
     }

     _recent_list.push_front(font_name);

     /*
      * Check if the current size exceeds the max size
      * If yes, then delete an element from the end
      */
     if(_recent_list.size() > _max_size) {
         _recent_list.pop_back();
     }

     write_recently_used_fonts();
     update_signal.emit();
}

void RecentlyUsedFonts::pop_front()
{
    if (_recent_list.empty()) {
        return;
    }

    _recent_list.pop_front();

    write_recently_used_fonts();
    update_signal.emit();
}

bool RecentlyUsedFonts::get_continuous_streak() const
{
    return continuous_streak;
}

void RecentlyUsedFonts::set_continuous_streak(bool streak)
{
    continuous_streak = streak;
}

/*
bool RecentlyUsedFonts::is_empty()
{
    return _max_size == 0;
}
*/

int RecentlyUsedFonts::get_count() const
{
    return _recent_list.size();
}

// Returns the recently used fonts.
std::list<Glib::ustring> const &RecentlyUsedFonts::get_fonts() const
{
    return _recent_list;
}

} // Namespace

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
