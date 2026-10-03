// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork-library-host-native.h"
#include "artwork-library-file-dialog.h"
#include "dialog-window.h"
#include "inkscape-window.h"
#include "ui/dialog-run.h"
#include <glibmm/main.h>
#include <gtkmm/messagedialog.h>
#include <gtk/gtk.h>
#include <algorithm>

namespace Inkscape::UI::Dialog {
std::vector<std::string> ArtworkLibraryHost::within(Gtk::Widget const &root) const {
    owner(); std::vector<std::string> ids;
    for (auto const &[id, e] : _entries) {
        auto widget = e.widget ? e.widget() : nullptr;
        if (!widget) continue;
        auto ancestor = widget;
        while (ancestor && ancestor != &root) ancestor = ancestor->get_parent();
        bool belongs = ancestor == &root;
        // Floating dialogs follow the last active native window. Focus changes
        // move scope, never ownership of the retained workspace.
        if (auto window = dynamic_cast<DialogWindow *>(widget->get_root()))
            belongs = belongs || window->get_inkscape_window() == &root;
        if (belongs) ids.push_back(id);
    }
    return ids;
}
namespace {
class NativeCloseUI final : public LibraryCloseUI {
public:
    explicit NativeCloseUI(Gtk::Widget *root) {
        Gtk::Window *parent = root ? dynamic_cast<Gtk::Window *>(root->get_root()) : nullptr;
        if (!parent) parent = dynamic_cast<Gtk::Window *>(root);
        if (parent) _parent = sigc::track_object([parent] { return parent; }, *parent);
    }
    void parent(Gtk::Window &dialog) {
        if (!_parent.empty()) if (auto p = _parent()) dialog.set_transient_for(*p);
    }
    LibraryCloseAnswer ask(LibraryClosePrompt const &p) override {
        bool const changes = p.kind == LibraryCloseKind::Changes;
        Gtk::MessageDialog dialog(changes ? "Save library changes before closing?" : "Artwork Library — close",
                                  false, Gtk::MessageType::QUESTION, Gtk::ButtonsType::NONE, true);
        dialog.set_title("Artwork Library");
        parent(dialog);
        dialog.set_secondary_text(changes ? p.label + "\nYour changes have not been saved. Closing without saving keeps the original library file unchanged."
                                          : p.label + "\n" + p.detail);
        dialog.add_button("Cancel", int(Gtk::ResponseType::CANCEL));
        dialog.set_default_response(int(Gtk::ResponseType::CANCEL));
        if (p.kind == LibraryCloseKind::Work) {
            dialog.add_button("Wait for final result", int(Gtk::ResponseType::OK));
            dialog.add_button("Cancel operation, then review", int(Gtk::ResponseType::REJECT));
        } else {
            dialog.add_button(changes ? "Close without saving" : "Discard / unload (files retained)", int(Gtk::ResponseType::REJECT));
            if (p.kind == LibraryCloseKind::Changes) dialog.add_button("Save", int(Gtk::ResponseType::APPLY));
        }
        auto response = UI::dialog_run(dialog);
        if (p.kind == LibraryCloseKind::Work) {
            if (response == int(Gtk::ResponseType::OK)) return {LibraryCloseChoice::Wait};
            if (response == int(Gtk::ResponseType::REJECT)) return {LibraryCloseChoice::CancelWork};
        } else {
            if (response == int(Gtk::ResponseType::REJECT)) return {LibraryCloseChoice::Discard};
            if (response == int(Gtk::ResponseType::APPLY) && p.kind == LibraryCloseKind::Changes) {
                if (!p.path.empty()) return {LibraryCloseChoice::Save, p.path};
                auto destination = save_as(p.label);
                if (!destination.empty()) return {LibraryCloseChoice::Save, std::move(destination)};
            }
        }
        return {};
    }
    bool wait(std::shared_ptr<ArtworkLibraryWorkspace> const &w, std::function<void()> const &cancel) override {
        if (!w->busy()) return true;
        Gtk::MessageDialog dialog("Finishing library operation", false, Gtk::MessageType::INFO, Gtk::ButtonsType::NONE, true);
        parent(dialog);
        dialog.set_secondary_text("The final publication result must be observed before closing. Cancellation cannot undo a published save.");
        dialog.add_button("Cancel operation and keep open", int(Gtk::ResponseType::CANCEL));
        auto tick = Glib::signal_timeout().connect([&] {
            if (w->busy()) return true;
            dialog.response(int(Gtk::ResponseType::OK)); return false;
        }, 25);
        auto result = UI::dialog_run(dialog); tick.disconnect();
        if (result != int(Gtk::ResponseType::OK)) { cancel(); return false; }
        return !w->busy();
    }
    void error(std::string const &message) override {
        Gtk::MessageDialog dialog("Library close cancelled", false, Gtk::MessageType::WARNING, Gtk::ButtonsType::OK, true);
        parent(dialog); dialog.set_secondary_text(message); UI::dialog_run(dialog);
    }
private:
    std::string save_as(std::string const &name) {
        // Use the same native suffix filter and result handling as panel Save As.
        // Bracket glob patterns are literal extensions in GTK's macOS bridge.
        auto chooser = create_library_file_dialog(LibraryFileOperation::SaveAs, name);
        struct Result { GMainLoop *loop; Glib::RefPtr<Gtk::FileDialog> dialog; LibraryFileChoice choice; };
        Result result{g_main_loop_new(nullptr, false), chooser, {}};
        auto parent = _parent.empty() ? nullptr : _parent();
        gtk_file_dialog_save(chooser->gobj(), parent ? parent->gobj() : nullptr, nullptr,
            [](GObject *, GAsyncResult *res, void *data) {
                auto &result = *static_cast<Result *>(data);
                try {
                    result.choice = finish_library_file_dialog(*result.dialog, LibraryFileOperation::SaveAs, res);
                } catch (std::exception const &e) {
                    result.choice = {LibraryFileChoice::Result::Failed, {}, e.what()};
                }
                g_main_loop_quit(result.loop);
            }, &result);
        g_main_loop_run(result.loop); g_main_loop_unref(result.loop);
        if (result.choice.result == LibraryFileChoice::Result::Failed) {
            error(result.choice.message); return {};
        }
        if (result.choice.result != LibraryFileChoice::Result::Accepted) return {};
        auto path = result.choice.paths.front();
        if (!path.empty()) {
            auto raw = g_ascii_strdown(path.c_str(), -1); std::string lower(raw); g_free(raw);
            if (!lower.ends_with(".valib")) {
                error("Choose a .valib destination. Library close was cancelled; files and edits are retained.");
                return {};
            }
        }
        return path;
    }
    sigc::slot<Gtk::Window *()> _parent;
};
}
std::unique_ptr<LibraryCloseGuard> prepare_library_close(
    std::shared_ptr<ArtworkLibraryHost> const &host, Gtk::Widget *root, bool all) {
    auto ids = all ? host->slots() : root ? host->within(*root) : std::vector<std::string>{};
    // A modal decision may move a panel or destroy its host. A different
    // post-prompt scope is not authority to discard the moved/new workspace.
    sigc::slot<std::vector<std::string>()> current_scope;
    if (root && !all) current_scope = sigc::track_object([host, root] { return host->within(*root); }, *root);
    NativeCloseUI ui(root);
    auto guard = host->prepare(ids, ui);
    if (!guard) return {};
    if (root && !all && (current_scope.empty() || current_scope() != ids)) return {};
    return guard;
}
}
