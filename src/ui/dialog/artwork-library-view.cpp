// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-view.h"
#include "ui/builder-utils.h"
#include <giomm/menu.h>
#include <gtkmm/button.h>
#include <gtkmm/menubutton.h>
#include <utility>

namespace Inkscape::UI::Dialog {

ArtworkLibraryView::ArtworkLibraryView(Glib::RefPtr<Gtk::Builder> builder,
                                      Glib::RefPtr<Gio::ActionGroup> actions)
    : Gtk::Box(Gtk::Orientation::VERTICAL)
    , _builder(std::move(builder))
    , _content(get_widget<Gtk::Box>(_builder, "artwork-library"))
{
    // Install the controller group before creating GtkActionable observers.
    // Builder-created observers can otherwise retain an incomplete ancestor
    // action chain when an intermediate box has no action muxer of its own.
    insert_action_group("library", actions);
    append(_content);
    for (auto const &[id, action] : {
             std::pair{"save-collection", "library.save"},
             {"new-collection", "library.new"},
             {"open-collection", "library.open"},
             {"add-selection", "library.add-selection"},
             {"import-artwork", "library.import"},
             {"insert-artwork", "library.insert"}}) {
        get_widget<Gtk::Button>(_builder, id).set_action_name(action);
    }
    get_widget<Gtk::MenuButton>(_builder, "collection-picker")
        .set_menu_model(get_object<Gio::Menu>(_builder, "collection-menu"));
    set_vexpand(true);
}

ArtworkLibraryView::~ArtworkLibraryView()
{
    remove(_content);
}

} // namespace Inkscape::UI::Dialog
