// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-file-dialog.h"
#include "choose-file.h"
#include <giomm/liststore.h>
#include <giomm/file.h>
#include <glibmm/i18n.h>
#include <gtkmm/filefilter.h>
#include <gtk/gtk.h>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace Inkscape::UI::Dialog {
LibraryFileOperation library_file_operation(std::string const &action) {
    if (action == "open") return LibraryFileOperation::Open;
    if (action == "import") return LibraryFileOperation::Import;
    if (action == "save-as") return LibraryFileOperation::SaveAs;
    if (action == "export") return LibraryFileOperation::Export;
    if (action == "recover") return LibraryFileOperation::Recover;
    if (action == "find-recovery") return LibraryFileOperation::FindRecoveryFolder;
    throw std::invalid_argument("Unknown artwork library file operation");
}
namespace {
std::string filename(std::string const &name, char const *suffix) {
    std::string out;
    if (g_utf8_validate(name.data(), name.size(), nullptr)) {
        for (auto p = name.c_str(); *p;) {
            auto next = g_utf8_next_char(p);
            if (out.size() + std::size_t(next - p) > 180) break;
            if (static_cast<unsigned char>(*p) < 32 || std::strchr("/\\:*?\"<>|", *p)) out += '_';
            else out.append(p, next);
            p = next;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = "Artwork";
    auto lower = g_ascii_strdown(out.c_str(), out.size());
    auto stem = std::string(lower).substr(0, out.find('.'));
    bool reserved = stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9');
    if (reserved) out.insert(0, "_");
    bool has_suffix = std::string_view(lower).ends_with(suffix); g_free(lower);
    if (!has_suffix) out += suffix;
    return out;
}
}
Glib::RefPtr<Gtk::FileDialog> create_library_file_dialog(
    LibraryFileOperation operation, std::string const &name, std::string const &folder)
{
    char const *title = nullptr, *accept = nullptr;
    switch (operation) {
        case LibraryFileOperation::Open: title = _("Open artwork collection"); accept = _("Open"); break;
        case LibraryFileOperation::Import: title = _("Import artwork"); accept = _("Import"); break;
        case LibraryFileOperation::SaveAs: title = _("Save artwork collection as"); accept = _("Save"); break;
        case LibraryFileOperation::Export: title = _("Export artwork as SVG"); accept = _("Export"); break;
        case LibraryFileOperation::Recover: title = _("Recover artwork collection"); accept = _("Open recovery copy"); break;
        case LibraryFileOperation::FindRecoveryFolder: title = _("Find artwork recovery copies"); accept = _("Inspect folder"); break;
    }
    if (!title) throw std::invalid_argument("Unknown artwork library file operation");
    auto dialog = Inkscape::create_file_dialog(title, accept);
    if (operation == LibraryFileOperation::FindRecoveryFolder) {
        if (!folder.empty() && g_path_is_absolute(folder.c_str())) dialog->set_initial_folder(Gio::File::create_for_path(folder));
        return dialog;
    }
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto supported = Gtk::FileFilter::create();
    bool saving = operation == LibraryFileOperation::SaveAs || operation == LibraryFileOperation::Export;
    if (operation == LibraryFileOperation::Export) {
        supported->set_name(_("Editable SVG artwork (*.svg)")); supported->add_suffix("svg");
    } else if (operation == LibraryFileOperation::SaveAs || operation == LibraryFileOperation::Recover) {
        supported->set_name(_("VACards artwork collections (*.valib)")); supported->add_suffix("valib");
    } else {
        supported->set_name(_("Artwork collections and editable SVG (*.valib, *.lbart, *.svg)"));
        for (auto suffix : {"valib", "lbart", "svg"}) supported->add_suffix(suffix);
    }
    filters->append(supported);
    if (!saving) {
        auto all = Gtk::FileFilter::create(); all->set_name(_("All files (validated by contents)")); all->add_pattern("*");
        filters->append(all);
    }
    Inkscape::set_filters(*dialog, filters);
    if (saving) dialog->set_initial_name(filename(name, operation == LibraryFileOperation::Export ? ".svg" : ".valib"));
    if (!folder.empty() && g_path_is_absolute(folder.c_str())) dialog->set_initial_folder(Gio::File::create_for_path(folder));
    return dialog;
}
LibraryFileChoice library_file_choice(GListModel *files, GError const *error, bool multiple) {
    using Result = LibraryFileChoice::Result;
    if (error) {
        if (g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) ||
            g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return {Result::Cancelled, {}, {}};
        return {Result::Failed, {}, error->message ? error->message : _("The file chooser failed")};
    }
    auto count = files ? g_list_model_get_n_items(files) : 0;
    if (!count) return {Result::Failed, {}, _("The file chooser did not return a file")};
    if ((!multiple && count != 1) || count > 128)
        return {Result::Failed, {}, _("Select one file, or up to 128 files when importing artwork")};
    LibraryFileChoice choice{Result::Accepted, {}, {}};
    for (guint i = 0; i < count; ++i) {
        auto item = g_list_model_get_item(files, i);
        std::unique_ptr<void, decltype(&g_object_unref)> owner(item, g_object_unref);
        if (!G_IS_FILE(item)) return {Result::Failed, {}, _("The file chooser returned an invalid item")};
        auto raw = g_file_get_path(G_FILE(item));
        std::unique_ptr<gchar, decltype(&g_free)> path(raw, g_free);
        if (!raw || !g_path_is_absolute(raw))
            return {Result::Failed, {}, _("Select files on a local or mounted drive; nothing was imported")};
        choice.paths.emplace_back(raw);
    }
    return choice;
}
LibraryFileChoice finish_library_file_dialog(Gtk::FileDialog &dialog, LibraryFileOperation operation, GAsyncResult *result) {
    GError *error = nullptr;
    GListModel *files = nullptr;
    if (operation == LibraryFileOperation::Import) files = gtk_file_dialog_open_multiple_finish(dialog.gobj(), result, &error);
    else {
        bool save = operation == LibraryFileOperation::SaveAs || operation == LibraryFileOperation::Export;
        auto file = operation == LibraryFileOperation::FindRecoveryFolder
            ? gtk_file_dialog_select_folder_finish(dialog.gobj(), result, &error)
            : save ? gtk_file_dialog_save_finish(dialog.gobj(), result, &error) : gtk_file_dialog_open_finish(dialog.gobj(), result, &error);
        if (file) {
            auto list = g_list_store_new(G_TYPE_FILE); g_list_store_append(list, file); g_object_unref(file);
            files = G_LIST_MODEL(list);
        }
    }
    std::unique_ptr<GError, decltype(&g_error_free)> owned_error(error, g_error_free);
    std::unique_ptr<GListModel, decltype(&g_object_unref)> owned_files(files, g_object_unref);
    return library_file_choice(files, error, operation == LibraryFileOperation::Import);
}
}
