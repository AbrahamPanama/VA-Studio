// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_ARTWORK_LIBRARY_HOST_NATIVE_H
#define INKSCAPE_ARTWORK_LIBRARY_HOST_NATIVE_H
#include "artwork-library-host.h"
namespace Inkscape::UI::Dialog {
// Snapshot scope before any nested GTK loop. Null root is parentless app Quit.
// all includes orphaned retained slots. Does not register or create a panel.
std::unique_ptr<LibraryCloseGuard> prepare_library_close(
    std::shared_ptr<ArtworkLibraryHost> const &, Gtk::Widget *root, bool all = false);
}
#endif
