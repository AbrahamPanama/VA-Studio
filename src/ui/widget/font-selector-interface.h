// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_FONT_SELECTOR_INTERFACE_H
#define SEEN_FONT_SELECTOR_INTERFACE_H

#include <cstdint>
#include <optional>

#include <glibmm/ustring.h>
#include <sigc++/signal.h>

namespace Gtk { class Widget; }

enum class FontChoicePhase { Preview, Commit, Cancel };
enum class FontChoiceOrigin { Pointer, Keyboard };

struct FontChoice {
    FontChoicePhase phase = FontChoicePhase::Cancel;
    FontChoiceOrigin origin = FontChoiceOrigin::Pointer;
    Glib::ustring family;
    std::optional<Glib::ustring> face;
    Glib::ustring fontspec;
    bool available = false;
    uint64_t interaction_id = 0;
    uint64_t generation = 0;
};

class FontSelectorInterface {
public:
    virtual ~FontSelectorInterface() {};

    // get font selected in this FontList, if any
    virtual Glib::ustring get_fontspec() const = 0;
    virtual double get_fontsize() const = 0;

    // show requested font in a FontList
    virtual void set_current_font(const Glib::ustring& family, const Glib::ustring& face) = 0;
    // 
    virtual void set_current_size(double size) = 0;

    virtual sigc::signal<void ()>& signal_fontspec_changed() = 0;
    virtual sigc::signal<void ()>& signal_fontsize_changed() = 0;
    virtual sigc::signal<void ()>& signal_set_default() = 0;
    virtual sigc::signal<void (const Glib::ustring&)>& signal_insert_text() = 0;
    virtual sigc::signal<void (FontChoice const &)>& signal_font_choice() = 0;

    // get UI element
    virtual Gtk::Widget* box() = 0;

    // legacy font selector
    virtual void set_model() = 0;
    virtual void unset_model() = 0;
};

#endif
