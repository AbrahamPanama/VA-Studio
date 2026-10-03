// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_DIALOG_ARTWORK_LIBRARY_VIEW_H
#define INKSCAPE_UI_DIALOG_ARTWORK_LIBRARY_VIEW_H

#include <giomm/actiongroup.h>
#include <gtkmm/box.h>
#include <gtkmm/builder.h>

namespace Inkscape::UI::Dialog {

// Native presentation only. The controller owns action enablement and all
// library/document operations. Construction never reads or renders artwork.
class ArtworkLibraryView final : public Gtk::Box {
public:
    ArtworkLibraryView(Glib::RefPtr<Gtk::Builder> builder,
                       Glib::RefPtr<Gio::ActionGroup> actions);
    ~ArtworkLibraryView() override;

private:
    Glib::RefPtr<Gtk::Builder> _builder;
    Gtk::Box &_content;
};

} // namespace Inkscape::UI::Dialog
#endif
