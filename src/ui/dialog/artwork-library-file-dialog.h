// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_ARTWORK_LIBRARY_FILE_DIALOG_H
#define INKSCAPE_ARTWORK_LIBRARY_FILE_DIALOG_H
#include <gtkmm/filedialog.h>
#include <gio/gio.h>
#include <string>
#include <vector>

namespace Inkscape::UI::Dialog {
enum class LibraryFileOperation { Open, Import, SaveAs, Export, Recover, FindRecoveryFolder };
LibraryFileOperation library_file_operation(std::string const &action);
Glib::RefPtr<Gtk::FileDialog> create_library_file_dialog(
    LibraryFileOperation, std::string const &suggested_name = {}, std::string const &folder = {});

struct LibraryFileChoice {
    enum class Result { Accepted, Cancelled, Failed } result = Result::Failed;
    std::vector<std::string> paths;
    std::string message;
};
// All-or-nothing path admission: never silently omit a remote/invalid item from
// a multi-selection. No IO or content admission; the workspace validates bytes.
LibraryFileChoice library_file_choice(GListModel *files, GError const *error, bool multiple);
LibraryFileChoice finish_library_file_dialog(Gtk::FileDialog &, LibraryFileOperation, GAsyncResult *);
}
#endif
