// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <glibmm/ustring.h>
#include <sigc++/signal.h>

namespace Inkscape {

struct FontTag {
    std::string tag;
    Glib::ustring display_name;

    bool operator == (const FontTag& ft) const { return tag == ft.tag && display_name == ft.display_name; }
};

class FontTags {
public:
    static FontTags& get();

    std::vector<FontTag> get_tags() const;
    void add_tag(const FontTag& tag);

    std::set<std::string> get_font_tags(Glib::ustring const &fontspec) const;
    void tag_font(Glib::ustring const &fontspec, std::string tag);
  
    const std::vector<FontTag>& get_selected_tags() const;
    bool select_tag(const std::string& tag_id, bool selected);
    bool is_tag_selected(const std::string& tag_id) const;
    bool deselect_all();

    const FontTag* find_tag(const std::string& tag_id) const;

    sigc::signal<void (const FontTag*, bool)>& get_signal_tag_changed();

private:
    FontTags();
    std::unordered_map<std::string, std::set<std::string>> _map;
    std::vector<FontTag> _tags;
    std::vector<FontTag> _selected;
    sigc::signal<void (const FontTag*, bool)> _signal_tag_changed;
};

} // namespace
