// SPDX-License-Identifier: GPL-2.0-or-later

#include "choose-file.h"

#include <exception>
#include <memory>
#include <utility>

#include <giomm/liststore.h>
#include <glib/gi18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/error.h>
#include <gtkmm/filedialog.h>

#include "choose-file-utils.h"
#include "preferences.h"

namespace Inkscape {

Glib::RefPtr<Gtk::FileDialog> create_file_dialog(Glib::ustring const &title,
                                                 Glib::ustring const &accept_label)
{
    auto const file_dialog = Gtk::FileDialog::create();
    file_dialog->set_title(title);
    file_dialog->set_accept_label(accept_label);
    return file_dialog;
}

void set_filters(Gtk::FileDialog &file_dialog,
                 Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> const &filters)
{
    file_dialog.set_filters(filters);
    if (filters->get_n_items() > 0) {
        file_dialog.set_default_filter(filters->get_item(0));
    }
}

void set_filter(Gtk::FileDialog &file_dialog, Glib::RefPtr<Gtk::FileFilter> const &filter)
{
    auto const filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(filter);
    set_filters(file_dialog, filters);
}

using StartMethod = void (Gtk::FileDialog::*)
                    (Gtk::Window &, Gio::SlotAsyncReady const &,
                     Glib::RefPtr<Gio::Cancellable> const &);

using FinishMethod = Glib::RefPtr<Gio::File> (Gtk::FileDialog::*)
                     (Glib::RefPtr<Gio::AsyncResult> const &);

[[nodiscard]] static auto run(Gtk::FileDialog &file_dialog, Gtk::Window &parent,
                              std::string &current_folder,
                              StartMethod const start, FinishMethod const finish)
{
    file_dialog.set_initial_folder(Gio::File::create_for_path(current_folder));

    bool responded = false;
    Glib::RefPtr<Gio::File> file;
    std::exception_ptr completion_error;

    (file_dialog.*start)(parent, [&](Glib::RefPtr<Gio::AsyncResult> const &result)
    {
        try {
            responded = true;

            file = (file_dialog.*finish)(result);
            if (!file) {
                return;
            }

            if (auto parent_folder = file->get_parent()) {
                current_folder = parent_folder->get_path();
            }
        } catch (Gtk::DialogError const &error) {
            if (error.code() == Gtk::DialogError::Code::DISMISSED) {
                responded = true;
            } else {
                completion_error = std::current_exception();
            }
        } catch (...) {
            completion_error = std::current_exception();
        }
    }, Glib::RefPtr<Gio::Cancellable>{});

    auto const main_context = Glib::MainContext::get_default();
    while (!responded) {
         main_context->iteration(true);
    }

    if (completion_error) std::rethrow_exception(completion_error);

    return file;
}

Glib::RefPtr<Gio::File> choose_file_save(Glib::ustring const &title, Gtk::Window *parent,
                                         Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> const &filters_model,
                                         std::string const &file_name,
                                         std::string &current_folder)
{
    if (!parent) return {};

    if (current_folder.empty()) {
        current_folder = Glib::get_home_dir();
    }

    auto const file_dialog = create_file_dialog(title, _("Save"));

    if (filters_model) {
        set_filters(*file_dialog, filters_model);
        // for (int i = 0; i < filters_model->get_n_items(); ++i) {
        //     std::cout << filters_model->get_item(i)->get_name() << std::endl;
        // }
    }

    file_dialog->set_initial_name(file_name);

    return run(*file_dialog, *parent, current_folder,
               &Gtk::FileDialog::save, &Gtk::FileDialog::save_finish);
}

Glib::RefPtr<Gio::File> choose_file_save(Glib::ustring const &title, Gtk::Window *parent,
                                         Glib::ustring const &mime_type,
                                         std::string const &file_name,
                                         std::string &current_folder)
{
    if (!parent) return {};

    auto filters_model = Gio::ListStore<Gtk::FileFilter>::create();
    auto filter = Gtk::FileFilter::create();
    if (!mime_type.empty()) {
        auto filter = Gtk::FileFilter::create();
        filter->add_mime_type(mime_type);
    }

    return choose_file_save(title, parent, filters_model, file_name, current_folder);
}

void choose_file_save_async(Glib::ustring const &title, Gtk::Window *parent,
                            Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> const &filters_model,
                            std::string const &file_name, std::string current_folder, SaveFileCallback callback)
{
    if (!callback)
        return;
    auto completed = std::make_shared<bool>(false);
    auto callback_holder = std::make_shared<SaveFileCallback>(std::move(callback));
    auto complete = [completed, callback_holder](Glib::RefPtr<Gio::File> file, std::string error) {
        if (*completed)
            return;
        *completed = true;
        try {
            (*callback_holder)(std::move(file), std::move(error));
        } catch (std::exception const &e) {
            g_warning("Save chooser callback failed: %s", e.what());
        } catch (...) {
            g_warning("Save chooser callback failed");
        }
    };
    if (!parent) {
        complete({}, "no parent window for file chooser");
        return;
    }

    try {
        auto const file_dialog = create_file_dialog(title, _("Save"));

        if (filters_model) {
            set_filters(*file_dialog, filters_model);
        }

        file_dialog->set_initial_name(file_name);

        // Avoid a synchronous stat of a remembered NAS/cloud folder. GTK resolves
        // the initial folder as part of its asynchronous dialog operation.
        if (current_folder.empty()) {
            current_folder = Glib::get_home_dir();
        }
        file_dialog->set_initial_folder(Gio::File::create_for_path(current_folder));

        // The dialog is captured by value to retain it until completion. The shared
        // flag guarantees the callback is invoked at most once even if the async
        // result is delivered more than once.
        file_dialog->save(
            *parent,
            [file_dialog, complete](Glib::RefPtr<Gio::AsyncResult> const &result) mutable {
                Glib::RefPtr<Gio::File> file;
                std::string error;
                try {
                    file = file_dialog->save_finish(result);
                } catch (Gtk::DialogError const &e) {
                    if (e.code() != Gtk::DialogError::Code::DISMISSED)
                        error = e.what();
                } catch (std::exception const &e) {
                    error = e.what();
                } catch (...) {
                    error = "file chooser failed";
                }

                complete(std::move(file), std::move(error));
            },
            Glib::RefPtr<Gio::Cancellable>{});
    } catch (std::exception const &e) {
        complete({}, e.what());
    } catch (...) {
        complete({}, "file chooser failed");
    }
}

Glib::RefPtr<Gio::File> choose_file_open(Glib::ustring const &title, Gtk::Window *parent,
                                         Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> const &filters_model,
                                         std::string &current_folder,
                                         Glib::ustring const &accept)
{
    if (!parent) return Glib::RefPtr<Gio::File>();

    if (current_folder.empty()) {
        current_folder = Glib::get_home_dir();
    }

    auto const file_dialog = create_file_dialog(title, accept.empty() ? _("Open") : accept);

    if (filters_model) {
        set_filters(*file_dialog, filters_model);
    }

    return run(*file_dialog, *parent, current_folder,
               &Gtk::FileDialog::open, &Gtk::FileDialog::open_finish);
}

void choose_file_open_async(Glib::ustring const &title, Gtk::Window *parent,
                            Glib::RefPtr<Gio::ListStore<Gtk::FileFilter>> const &filters_model,
                            std::string current_folder, OpenFileCallback callback,
                            Glib::RefPtr<Gio::Cancellable> cancellable)
{
    if (!callback) return;
    auto callback_holder = std::make_shared<OpenFileCallback>(std::move(callback));
    auto completed = std::make_shared<bool>(false);
    auto complete = [callback_holder, completed](Glib::RefPtr<Gio::File> file,
                                                  std::string folder, std::string error) {
        if (*completed) return;
        *completed = true;
        try {
            (*callback_holder)(std::move(file), std::move(folder), std::move(error));
        } catch (std::exception const &e) {
            g_warning("Open chooser callback failed: %s", e.what());
        }
    };
    if (!parent) {
        complete({}, {}, "no parent window for file chooser");
        return;
    }
    try {
        auto dialog = create_file_dialog(title, _("Open"));
        if (filters_model) set_filters(*dialog, filters_model);
        if (current_folder.empty()) current_folder = Glib::get_home_dir();
        dialog->set_initial_folder(Gio::File::create_for_path(current_folder));
        dialog->open(*parent, [dialog, complete]
                     (Glib::RefPtr<Gio::AsyncResult> const &result) {
            Glib::RefPtr<Gio::File> file;
            std::string folder;
            std::string error;
            try {
                file = dialog->open_finish(result);
                if (file) {
                    if (auto parent_folder = file->get_parent()) folder = parent_folder->get_path();
                }
            } catch (Gtk::DialogError const &e) {
                if (e.code() != Gtk::DialogError::Code::DISMISSED &&
                    e.code() != Gtk::DialogError::Code::CANCELLED) error = e.what();
            } catch (std::exception const &e) {
                error = e.what();
            }
            complete(std::move(file), std::move(folder), std::move(error));
        }, cancellable);
    } catch (std::exception const &e) {
        complete({}, {}, e.what());
    }
}

Glib::RefPtr<Gio::File> choose_file_open(Glib::ustring const &title, Gtk::Window *parent,
                                         std::vector<Glib::ustring> const &mime_types,
                                         std::string &current_folder,
                                         Glib::ustring const &accept)
{
    auto filters_model = Gio::ListStore<Gtk::FileFilter>::create();
    auto filter = Gtk::FileFilter::create();
    for (auto const &t : mime_types) {
        filter->add_mime_type(t);
    }
    filters_model->append(filter);

    return choose_file_open(title, parent, filters_model, current_folder, accept);
}

Glib::RefPtr<Gio::File> choose_file_open(Glib::ustring const &title, Gtk::Window *parent,
                                         std::vector<std::pair<Glib::ustring, Glib::ustring>> const &filters,
                                         std::string &current_folder,
                                         Glib::ustring const &accept)
{
    auto filters_model = Gio::ListStore<Gtk::FileFilter>::create();

    auto all_supported = Gtk::FileFilter::create();
    if (filters.size() > 1) {
        all_supported->set_name(_("All Supported Formats"));
        filters_model->append(all_supported);
    }

    for (auto const &f : filters) {
        auto filter = Gtk::FileFilter::create();
        filter->set_name(f.first);
        filter->add_pattern(f.second);
        filters_model->append(filter);
        all_supported->add_pattern(f.second);
    }

    return choose_file_open(title, parent, filters_model, current_folder, accept);
}

// Open one or more image files.
std::vector<Glib::RefPtr<Gio::File>> choose_file_open_images(Glib::ustring const &title,
                                                             Gtk::Window* parent,
                                                             std::string const &pref_path,
                                                             Glib::ustring const &accept)
{
    auto const file_dialog = create_file_dialog(title, accept);
    auto filter_model = Inkscape::UI::Dialog::create_open_filters();
    set_filters(*file_dialog, filter_model);

    std::string current_folder;
    Inkscape::UI::Dialog::get_start_directory(current_folder, pref_path, true);
    if (current_folder.empty()) {
        current_folder = Glib::get_home_dir();
    }
    file_dialog->set_initial_folder(Gio::File::create_for_path(current_folder));

    bool responded = false;
    std::vector<Glib::RefPtr<Gio::File>> files;
    std::exception_ptr completion_error;

    file_dialog->open_multiple(*parent, [&](Glib::RefPtr<Gio::AsyncResult> const &result)
    {
        try {
            responded = true;

            files = file_dialog->open_multiple_finish(result);
            if (files.size() == 0) {
                return;
            }
            if (files.size() == 1) {
                // Save current_folder.
                current_folder = files[0]->get_parent()->get_path();
                Inkscape::Preferences *prefs = Inkscape::Preferences::get();
                prefs->setString(pref_path, current_folder);
            }
        } catch (Gtk::DialogError const &error) {
            if (error.code() == Gtk::DialogError::Code::DISMISSED) {
                responded = true;
            } else {
                completion_error = std::current_exception();
            }
        } catch (...) {
            completion_error = std::current_exception();
        }
    }, Glib::RefPtr<Gio::Cancellable>{});

    auto const main_context = Glib::MainContext::get_default();
    while (!responded) {
        main_context->iteration(true);
    }

    if (completion_error) std::rethrow_exception(completion_error);

    return files;
}

void choose_file_open_images_async(Glib::ustring const &title,
                                   Gtk::Window *parent,
                                   std::string const &pref_path,
                                   Glib::ustring const &accept,
                                   OpenImagesCallback callback)
{
    if (!callback) return;
    if (!parent) {
        callback({}, "no parent window for file chooser");
        return;
    }

    auto dialog = create_file_dialog(title, accept);
    set_filters(*dialog, Inkscape::UI::Dialog::create_open_filters());

    // Avoid a synchronous stat of a remembered NAS/cloud folder. GTK resolves
    // the initial folder as part of its asynchronous dialog operation.
    auto *prefs = Inkscape::Preferences::get();
    std::string folder = prefs->getString(pref_path);
    if (folder.empty()) folder = Glib::get_user_special_dir(Glib::UserDirectory::DOCUMENTS);
    if (folder.empty()) folder = Glib::get_home_dir();
    dialog->set_initial_folder(Gio::File::create_for_path(folder));

    dialog->open_multiple(*parent,
        [dialog, callback = std::move(callback), pref_path]
        (Glib::RefPtr<Gio::AsyncResult> const &result) mutable {
            std::vector<Glib::RefPtr<Gio::File>> files;
            std::string error;
            try {
                files = dialog->open_multiple_finish(result);
                if (files.size() == 1 && files.front()) {
                    if (auto parent_file = files.front()->get_parent()) {
                        auto const current_folder = parent_file->get_path();
                        if (!current_folder.empty()) {
                            Inkscape::Preferences::get()->setString(pref_path, current_folder);
                        }
                    }
                }
            } catch (Gtk::DialogError const &e) {
                if (e.code() != Gtk::DialogError::Code::DISMISSED) error = e.what();
            } catch (std::exception const &e) {
                error = e.what();
            } catch (...) {
                error = "file chooser failed";
            }
            callback(std::move(files), std::move(error));
        }, Glib::RefPtr<Gio::Cancellable>{});
}

} // namespace Inkscape

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim:filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99:
