// SPDX-License-Identifier: GPL-2.0-or-later
// Native view contract only. This does not certify the pending library controller,
// SVG import, insertion, data recovery or application close workflow.
#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/builder.h>
#include <gtkmm/button.h>
#include <gtkmm/gridview.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/scale.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "ui/iconview-item-factory.h"
#include "ui/dialog/artwork-library-view.h"
#include "ui/dialog/artwork-library-file-dialog.h"
#include "util/scope_exit.h"
#include <glibmm/i18n.h>
#include <gtkmm/filefilter.h>
#include <giomm/file.h>
#include <giomm/liststore.h>

namespace {

auto application()
{
    static auto app = [] {
        auto result = Gtk::Application::create("org.inkscape.vacards.libraryviewtest",
                                                Gio::Application::Flags::NON_UNIQUE);
        result->register_application();
        return result;
    }();
    return app;
}

void frames()
{
    auto loop = g_main_loop_new(nullptr, false);
    g_timeout_add(120, [](gpointer ptr) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop *>(ptr));
        return G_SOURCE_REMOVE;
    }, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

std::pair<int, int> width(Gtk::Widget &widget)
{
    int minimum, natural;
    gtk_widget_measure(widget.gobj(), GTK_ORIENTATION_HORIZONTAL, -1,
                       &minimum, &natural, nullptr, nullptr);
    return {minimum, natural};
}

class ArtworkLibraryViewTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto enabled = std::getenv("INKSCAPE_TEST_GUI");
        if (!enabled || std::string(enabled) != "1") GTEST_SKIP() << "GUI testing not enabled";
        application();
        builder = Gtk::Builder::create_from_file(std::string(INKSCAPE_SHARE_DIR) + "/ui/dialog-artwork-library.glade");
        root = builder->get_widget<Gtk::Box>("artwork-library");
        ASSERT_TRUE(root);
        group = Gio::SimpleActionGroup::create();
        view = std::make_unique<Inkscape::UI::Dialog::ArtworkLibraryView>(builder, group);
    }
    Glib::RefPtr<Gtk::Builder> builder;
    Gtk::Box *root = nullptr;
    Glib::RefPtr<Gio::SimpleActionGroup> group;
    std::unique_ptr<Inkscape::UI::Dialog::ArtworkLibraryView> view;
};

TEST_F(ArtworkLibraryViewTest, FileDialogsHaveOperationSpecificFiltersAndTitles) {
    using namespace Inkscape::UI::Dialog;
    auto matches = [](Gtk::FileDialog &dialog, char const *name) {
        auto info = g_file_info_new(); g_file_info_set_name(info, name); g_file_info_set_display_name(info, name);
        auto accepted = gtk_filter_match(GTK_FILTER(dialog.get_default_filter()->gobj()), info);
        g_object_unref(info); return bool(accepted);
    };
    auto open = create_library_file_dialog(LibraryFileOperation::Open);
    EXPECT_EQ(open->get_title(), _("Open artwork collection"));
    EXPECT_EQ(open->get_accept_label(), _("Open"));
    for (auto name : {"a.valib", "a.VALIB", "b.lbart", "b.LBART", "c.svg", "c.SVG"}) EXPECT_TRUE(matches(*open, name));
    EXPECT_FALSE(matches(*open, "unsupported.png")); // No false bitmap-import support claim.
    EXPECT_EQ(open->get_filters()->get_n_items(), 2u);
    auto save = create_library_file_dialog(LibraryFileOperation::SaveAs);
    EXPECT_EQ(save->get_title(), _("Save artwork collection as"));
    EXPECT_EQ(save->get_filters()->get_n_items(), 1u);
    EXPECT_TRUE(matches(*save, "a.VALIB")); EXPECT_FALSE(matches(*save, "b.lbart")); EXPECT_FALSE(matches(*save, "c.svg"));
    auto export_svg = create_library_file_dialog(LibraryFileOperation::Export);
    EXPECT_EQ(export_svg->get_title(), _("Export artwork as SVG"));
    EXPECT_TRUE(matches(*export_svg, "c.SVG")); EXPECT_FALSE(matches(*export_svg, "a.valib"));
    auto recover = create_library_file_dialog(LibraryFileOperation::Recover);
    EXPECT_TRUE(matches(*recover, "collection.recovery-uuid.VALIB"));
    EXPECT_FALSE(matches(*recover, "preview.svg"));
    auto folder = create_library_file_dialog(LibraryFileOperation::FindRecoveryFolder);
    EXPECT_EQ(folder->get_title(), _("Find artwork recovery copies"));
    EXPECT_EQ(folder->get_accept_label(), _("Inspect folder"));
    EXPECT_FALSE(folder->get_filters());
    EXPECT_EQ(library_file_operation("find-recovery"), LibraryFileOperation::FindRecoveryFolder);
    EXPECT_THROW(library_file_operation("delete"), std::invalid_argument);
    EXPECT_THROW(create_library_file_dialog(static_cast<LibraryFileOperation>(99)), std::invalid_argument);
}
TEST_F(ArtworkLibraryViewTest, FileNameSuggestionsPreserveUnicodeAndCannotBecomePaths) {
    using namespace Inkscape::UI::Dialog;
    EXPECT_EQ(create_library_file_dialog(LibraryFileOperation::SaveAs)->get_initial_name(), "Artwork.valib");
    EXPECT_EQ(create_library_file_dialog(LibraryFileOperation::SaveAs, "Invitación.VALIB")->get_initial_name(), "Invitación.VALIB");
    EXPECT_EQ(create_library_file_dialog(LibraryFileOperation::Export, "Premios/Ángel:2026")->get_initial_name(), "Premios_Ángel_2026.svg");
    EXPECT_EQ(create_library_file_dialog(LibraryFileOperation::Export, "CON")->get_initial_name(), "_CON.svg");
    EXPECT_EQ(create_library_file_dialog(LibraryFileOperation::Export, "LPT9.svg")->get_initial_name(), "_LPT9.svg");
    EXPECT_EQ(create_library_file_dialog(LibraryFileOperation::Export, ".. ")->get_initial_name(), "Artwork.svg");
    auto name = create_library_file_dialog(LibraryFileOperation::Export, std::string(400, 'a') + "á")->get_initial_name();
    EXPECT_LE(name.size(), 184u); EXPECT_TRUE(g_utf8_validate(name.data(), name.size(), nullptr));
    auto folder = std::string(g_get_tmp_dir());
    auto save = create_library_file_dialog(LibraryFileOperation::SaveAs, "Named", folder);
    ASSERT_TRUE(save->get_initial_folder());
    EXPECT_TRUE(g_file_equal(save->get_initial_folder()->gobj(), Gio::File::create_for_path(folder)->gobj()));
    EXPECT_FALSE(create_library_file_dialog(LibraryFileOperation::Open, {}, "relative/path")->get_initial_folder());
}
TEST_F(ArtworkLibraryViewTest, FileChoicePreservesOrderedLocalSelectionAndEnforcesCount) {
    using namespace Inkscape::UI::Dialog;
    auto files = Gio::ListStore<Gio::File>::create();
    auto a = Gio::File::create_for_path(std::string(g_get_tmp_dir()) + "/Invitación.svg");
    auto b = Gio::File::create_for_path(std::string(g_get_tmp_dir()) + "/Premios.VALIB");
    files->append(a); files->append(b);
    auto choice = library_file_choice(G_LIST_MODEL(files->gobj()), nullptr, true);
    EXPECT_EQ(choice.result, LibraryFileChoice::Result::Accepted);
    EXPECT_EQ(choice.paths, (std::vector<std::string>{a->get_path(), b->get_path()}));
    auto single = library_file_choice(G_LIST_MODEL(files->gobj()), nullptr, false);
    EXPECT_EQ(single.result, LibraryFileChoice::Result::Failed); EXPECT_TRUE(single.paths.empty());
    for (unsigned i = 2; i < 128; ++i) files->append(a);
    EXPECT_EQ(library_file_choice(G_LIST_MODEL(files->gobj()), nullptr, true).paths.size(), 128u);
    files->append(a);
    auto over = library_file_choice(G_LIST_MODEL(files->gobj()), nullptr, true);
    EXPECT_EQ(over.result, LibraryFileChoice::Result::Failed); EXPECT_TRUE(over.paths.empty());
}
TEST_F(ArtworkLibraryViewTest, RemoteOrInvalidSelectedItemCannotCauseSilentPartialImport) {
    using namespace Inkscape::UI::Dialog;
    auto files = Gio::ListStore<Gio::File>::create();
    files->append(Gio::File::create_for_path(std::string(g_get_tmp_dir()) + "/valid.svg"));
    files->append(Gio::File::create_for_uri("https://example.invalid/remote.svg"));
    auto choice = library_file_choice(G_LIST_MODEL(files->gobj()), nullptr, true);
    EXPECT_EQ(choice.result, LibraryFileChoice::Result::Failed); EXPECT_TRUE(choice.paths.empty()); EXPECT_FALSE(choice.message.empty());
    auto invalid = g_list_store_new(G_TYPE_OBJECT); auto item = g_object_new(G_TYPE_OBJECT, nullptr);
    g_list_store_append(invalid, item); g_object_unref(item);
    auto wrong = library_file_choice(G_LIST_MODEL(invalid), nullptr, true); g_object_unref(invalid);
    EXPECT_EQ(wrong.result, LibraryFileChoice::Result::Failed); EXPECT_TRUE(wrong.paths.empty());
    EXPECT_EQ(library_file_choice(nullptr, nullptr, true).result, LibraryFileChoice::Result::Failed);
}
TEST_F(ArtworkLibraryViewTest, FileChoiceKeepsLongUnicodeNativePathsWithoutUiRewriting) {
    using namespace Inkscape::UI::Dialog;
    std::string folder = g_get_tmp_dir();
    for (unsigned i = 0; i < 5; ++i) folder += "/" + std::string(60, 'x') + "-圖案";
    auto file = Gio::File::create_for_path(folder + "/Invitación.valib");
    auto files = Gio::ListStore<Gio::File>::create(); files->append(file);
    auto choice = library_file_choice(G_LIST_MODEL(files->gobj()), nullptr, false);
    ASSERT_EQ(choice.result, LibraryFileChoice::Result::Accepted);
    ASSERT_EQ(choice.paths.size(), 1u); EXPECT_GT(choice.paths.front().size(), 260u);
    EXPECT_EQ(choice.paths.front(), file->get_path());
    auto dialog = create_library_file_dialog(LibraryFileOperation::SaveAs, "Cards", folder);
    ASSERT_TRUE(dialog->get_initial_folder());
    EXPECT_EQ(dialog->get_initial_folder()->get_path(), Gio::File::create_for_path(folder)->get_path());
}
#ifdef _WIN32
TEST_F(ArtworkLibraryViewTest, FileChoiceKeepsLongUnicodeUncSpelling) {
    using namespace Inkscape::UI::Dialog;
    std::string component;
    for (unsigned i = 0; i < 64; ++i) component += "圖";
    std::string path = "\\\\server.invalid\\share";
    for (unsigned i = 0; i < 220; ++i) path += "\\" + component;
    path += "\\Invitación.valib";
    auto files = Gio::ListStore<Gio::File>::create(); files->append(Gio::File::create_for_path(path));
    // GFile construction/choice extraction is lexical: no share is contacted.
    auto choice = library_file_choice(G_LIST_MODEL(files->gobj()), nullptr, false);
    ASSERT_EQ(choice.result, LibraryFileChoice::Result::Accepted);
    ASSERT_EQ(choice.paths.size(), 1u); EXPECT_GT(choice.paths.front().size(), 32768u);
    EXPECT_EQ(choice.paths.front(), path); // no \\?\ prefix added to the UI result
}
#endif
TEST_F(ArtworkLibraryViewTest, CancelledAndFailedFileChoicesAreDistinguished) {
    using namespace Inkscape::UI::Dialog;
    for (auto code : {GTK_DIALOG_ERROR_CANCELLED, GTK_DIALOG_ERROR_DISMISSED}) {
        auto error = g_error_new_literal(GTK_DIALOG_ERROR, code, "cancelled");
        auto result = library_file_choice(nullptr, error, false); g_error_free(error);
        EXPECT_EQ(result.result, LibraryFileChoice::Result::Cancelled); EXPECT_TRUE(result.message.empty());
    }
    auto cancelled = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
    EXPECT_EQ(library_file_choice(nullptr, cancelled, false).result, LibraryFileChoice::Result::Cancelled); g_error_free(cancelled);
    auto error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Access denied — synthetic diagnostic");
    auto result = library_file_choice(nullptr, error, false); g_error_free(error);
    EXPECT_EQ(result.result, LibraryFileChoice::Result::Failed);
    EXPECT_EQ(result.message, "Access denied — synthetic diagnostic"); EXPECT_TRUE(result.paths.empty());
}
TEST_F(ArtworkLibraryViewTest, NativeAsyncFinishHandlesCancellationAfterOpeningForEveryFileOperation) {
    using namespace Inkscape::UI::Dialog;
    for (auto operation : {LibraryFileOperation::Open, LibraryFileOperation::Import, LibraryFileOperation::SaveAs,
                           LibraryFileOperation::Export, LibraryFileOperation::Recover, LibraryFileOperation::FindRecoveryFolder}) {
        struct State { Glib::RefPtr<Gtk::FileDialog> dialog; LibraryFileOperation operation; LibraryFileChoice result; bool finished = false; };
        auto state = std::make_shared<State>(); state->dialog = create_library_file_dialog(operation); state->operation = operation;
        auto ready = +[](GObject *, GAsyncResult *result, void *data) {
            std::unique_ptr<std::shared_ptr<State>> owned(static_cast<std::shared_ptr<State> *>(data));
            auto &s = **owned;
            try { s.result = finish_library_file_dialog(*s.dialog, s.operation, result); }
            catch (std::exception const &e) { s.result.message = e.what(); }
            s.finished = true;
        };
        auto cancelled = g_cancellable_new();
        auto data = new std::shared_ptr<State>(state);
        if (operation == LibraryFileOperation::FindRecoveryFolder) gtk_file_dialog_select_folder(state->dialog->gobj(), nullptr, cancelled, ready, data);
        else if (operation == LibraryFileOperation::Import) gtk_file_dialog_open_multiple(state->dialog->gobj(), nullptr, cancelled, ready, data);
        else if (operation == LibraryFileOperation::SaveAs || operation == LibraryFileOperation::Export)
            gtk_file_dialog_save(state->dialog->gobj(), nullptr, cancelled, ready, data);
        else gtk_file_dialog_open(state->dialog->gobj(), nullptr, cancelled, ready, data);
        // Matches the controller lifetime: an uncancelled request is launched,
        // then disposal cancels it. Pre-cancelled GTK 4.22.4 requests timed out
        // in the separately retained library-file-dialog-2 failure evidence.
        g_cancellable_cancel(cancelled); g_object_unref(cancelled);
        auto end = g_get_monotonic_time() + 10000000;
        while (!state->finished && g_get_monotonic_time() < end) { g_main_context_iteration(nullptr, false); g_usleep(1000); }
        ASSERT_TRUE(state->finished) << int(operation);
        EXPECT_EQ(state->result.result, LibraryFileChoice::Result::Cancelled) << state->result.message;
        EXPECT_TRUE(state->result.paths.empty());
    }
}

TEST_F(ArtworkLibraryViewTest, NativeControlsAndBoundedGridArePresent)
{
    auto grid = builder->get_widget<Gtk::GridView>("artwork-grid");
    auto scroller = builder->get_widget<Gtk::ScrolledWindow>("artwork-scroller");
    ASSERT_TRUE(grid); ASSERT_TRUE(scroller);
    EXPECT_EQ(grid->get_min_columns(), 1u);
    EXPECT_EQ(grid->get_max_columns(), 5u);
    EXPECT_FALSE(grid->get_single_click_activate());
    EXPECT_FALSE(scroller->get_propagate_natural_width());
    Gtk::PolicyType horizontal, vertical;
    scroller->get_policy(horizontal, vertical);
    EXPECT_EQ(horizontal, Gtk::PolicyType::NEVER);
    EXPECT_EQ(vertical, Gtk::PolicyType::AUTOMATIC);
    for (auto id : {"collection-picker", "artwork-search", "empty-message", "library-status",
                    "thumbnail-size", "save-collection", "add-selection", "import-artwork", "insert-artwork",
                    "collection-list", "library-split", "new-collection", "open-collection", "thumbnail-size-value"}) {
        EXPECT_TRUE(builder->get_object(id)) << id;
    }
    EXPECT_TRUE(builder->get_object("collection-menu"));
    EXPECT_TRUE(builder->get_object("open-collections"));
    EXPECT_TRUE(builder->get_object("artwork-menu"));
}

TEST_F(ArtworkLibraryViewTest, LongCollectionNamesDoNotGrowThePanelRequest)
{
    auto label = builder->get_widget<Gtk::Label>("collection-name"); ASSERT_TRUE(label);
    label->set_text("Cards");
    auto before = width(*root);
    EXPECT_LE(before.first, 500); // persistent library sidebar beside the existing controls
    for (auto const &name : {std::string(1024, 'W'), std::string("A very long collection — tarjetas e invitaciones")}) {
        label->set_text(name);
        EXPECT_EQ(width(*root), before);
        EXPECT_EQ(label->get_ellipsize(), Pango::EllipsizeMode::END);
    }
}

TEST_F(ArtworkLibraryViewTest, ActionRoutingUsesControllerOwnedEnablement)
{
    // Exercise the real mounted-panel contract, not an unattached builder tree.
    Gtk::Window window;
    application()->add_window(window);
    window.set_child(*view);
    std::array<char const *, 6> names = {"save", "add-selection", "import", "insert", "new", "open"};
    std::array<char const *, 6> ids = {"save-collection", "add-selection", "import-artwork", "insert-artwork", "new-collection", "open-collection"};
    std::array<unsigned, 6> calls{};
    std::array<Glib::RefPtr<Gio::SimpleAction>, 6> actions;
    for (unsigned i = 0; i < names.size(); ++i) {
        actions[i] = Gio::SimpleAction::create(names[i]);
        actions[i]->signal_activate().connect([&, i](Glib::VariantBase const &) { ++calls[i]; });
        actions[i]->set_enabled(false);
        group->add_action(actions[i]);
    }
    window.present();
    frames();
    for (unsigned i = 0; i < names.size(); ++i) {
        SCOPED_TRACE(names[i]);
        ASSERT_TRUE(group->has_action(names[i]));
        auto button = builder->get_widget<Gtk::Button>(ids[i]); ASSERT_TRUE(button);
        auto action = "library." + std::string(names[i]);
        EXPECT_EQ(button->get_action_name(), action);
        gtk_widget_activate_action(GTK_WIDGET(button->gobj()), action.c_str(), nullptr);
        EXPECT_EQ(calls[i], 0u);
        actions[i]->set_enabled(true);
        EXPECT_TRUE(button->get_sensitive());
        EXPECT_TRUE(root->activate_action(action));
        EXPECT_EQ(calls[i], 1u);
        EXPECT_TRUE(gtk_widget_activate_action(GTK_WIDGET(button->gobj()), action.c_str(), nullptr));
        EXPECT_EQ(calls[i], 2u);
        EXPECT_TRUE(gtk_widget_activate(GTK_WIDGET(button->gobj())));
        // GtkButton activation includes a short pressed-state animation.
        for (unsigned frame = 0; frame < 3; ++frame) frames();
        EXPECT_EQ(calls[i], 3u);
        actions[i]->set_enabled(false);
        EXPECT_FALSE(button->get_sensitive());
    }
    window.unset_child();
    window.set_visible(false);
    application()->remove_window(window);
    frames();
}

TEST_F(ArtworkLibraryViewTest, ThumbnailControlHasKeyboardFriendlyFiniteBounds)
{
    auto scale = builder->get_widget<Gtk::Scale>("thumbnail-size"); ASSERT_TRUE(scale);
    auto adjustment = scale->get_adjustment(); ASSERT_TRUE(adjustment);
    EXPECT_EQ(adjustment->get_lower(), 48);
    EXPECT_EQ(adjustment->get_upper(), 160);
    EXPECT_EQ(adjustment->get_value(), 80);
    EXPECT_GT(adjustment->get_step_increment(), 0);
    EXPECT_FALSE(scale->get_draw_value());
    scale->set_value(100000);
    EXPECT_EQ(scale->get_value(), 160);
    scale->set_value(-100000);
    EXPECT_EQ(scale->get_value(), 48);
}

TEST_F(ArtworkLibraryViewTest, ThumbnailSurfaceStaysWhiteInLightAndDarkWithoutChangingPixelsOrSelection)
{
    auto grid = builder->get_widget<Gtk::GridView>("artwork-grid"); ASSERT_TRUE(grid);
    ASSERT_TRUE(grid->has_css_class("artwork-library-grid"));
    auto provider = std::unique_ptr<GtkCssProvider, decltype(&g_object_unref)>(gtk_css_provider_new(), g_object_unref);
    gtk_css_provider_load_from_path(provider.get(), (std::string(INKSCAPE_SHARE_DIR) + "/ui/style.css").c_str());
    auto display = gtk_widget_get_display(GTK_WIDGET(grid->gobj()));
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider.get()), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    auto settings = gtk_settings_get_default();
    gboolean old_dark = false; gchar *old_theme = nullptr;
    g_object_get(settings, "gtk-application-prefer-dark-theme", &old_dark, "gtk-theme-name", &old_theme, nullptr);
    auto restore_style = scope_exit{[&] {
        gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(provider.get()));
        g_object_set(settings, "gtk-application-prefer-dark-theme", old_dark, "gtk-theme-name", old_theme, nullptr);
        g_free(old_theme);
    }};

    // Real transparent texture with opaque black and white blocks over the
    // scoped pure-white surface; no SVG rewrite.
    std::array<guint8, 16 * 16 * 4> pixels{};
    for (unsigned y = 4; y < 12; ++y) for (unsigned x : {2, 3, 4, 5, 10, 11, 12, 13}) {
        auto p = (y * 16 + x) * 4;
        pixels[p] = pixels[p + 1] = pixels[p + 2] = x < 8 ? 0 : 255; pixels[p + 3] = 255;
    }
    auto data = g_bytes_new(pixels.data(), pixels.size());
    auto texture = Glib::wrap(GDK_TEXTURE(gdk_memory_texture_new(16, 16, GDK_MEMORY_R8G8B8A8, data, 16 * 4)));
    g_bytes_unref(data);
    auto model = Gtk::StringList::create({"Black and white"});
    auto selection = Gtk::SingleSelection::create(model); selection->set_autoselect(false); selection->set_can_unselect(true);
    auto factory = Inkscape::UI::IconViewItemFactory::create([&](auto &) -> Inkscape::UI::IconViewItemFactory::ItemData {
        return {"Black and white", texture, "Synthetic contrast fixture", 80};
    });
    grid->set_factory(factory->get_factory()); grid->set_model(selection);
    Gtk::Window window; application()->add_window(window); window.set_default_size(500, 500); window.set_child(*view);
    auto cleanup = scope_exit{[&] {
        grid->set_model({}); grid->set_factory({}); window.unset_child(); window.set_visible(false);
        application()->remove_window(window);
    }};
    window.present(); frames();
    Gtk::Picture *picture = nullptr;
    for (auto child = grid->get_first_child(); child; child = child->get_next_sibling()) {
        auto box = dynamic_cast<Gtk::CenterBox *>(child->get_first_child());
        if (box && (picture = dynamic_cast<Gtk::Picture *>(box->get_start_widget()))) break;
    }
    ASSERT_TRUE(picture);
    auto render = [](Gtk::Widget &widget) {
        auto paintable = std::unique_ptr<GdkPaintable, decltype(&g_object_unref)>(gtk_widget_paintable_new(widget.gobj()), g_object_unref);
        auto snapshot = gtk_snapshot_new();
        int w = widget.get_width(), h = widget.get_height();
        gdk_paintable_snapshot(paintable.get(), GDK_SNAPSHOT(snapshot), w, h);
        auto node = std::unique_ptr<GskRenderNode, decltype(&gsk_render_node_unref)>(gtk_snapshot_free_to_node(snapshot), gsk_render_node_unref);
        std::vector<guint32> samples;
        if (!node || w <= 0 || h <= 0) return samples;
        auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        auto cr = cairo_create(surface); gsk_render_node_draw(node.get(), cr); cairo_destroy(cr); cairo_surface_flush(surface);
        auto row = reinterpret_cast<guint32 *>(cairo_image_surface_get_data(surface) + (h / 2) * cairo_image_surface_get_stride(surface));
        for (auto column : {3, 8, 11}) samples.push_back(row[w * column / 16]);
        cairo_surface_destroy(surface); return samples;
    };
    for (bool dark : {false, true}) {
        SCOPED_TRACE(dark ? "dark" : "light");
        g_object_set(settings, "gtk-theme-name", "Adwaita", "gtk-application-prefer-dark-theme", gboolean(dark), nullptr);
        frames();
        for (guint selected : {GTK_INVALID_LIST_POSITION, 0u}) {
            selection->set_selected(selected); frames();
            auto samples = render(*picture); ASSERT_EQ(samples.size(), 3u);
            // samples[1] is the transparent center gap over the scoped pure-white
            // surface; samples[0]/[2] are the opaque black/white artwork blocks.
            EXPECT_EQ(samples[0], 0xff000000u); EXPECT_EQ(samples[1], 0xffffffffu); EXPECT_EQ(samples[2], 0xffffffffu);
            EXPECT_EQ(selection->get_selected(), selected);
            EXPECT_EQ(picture->get_paintable().get(), texture.get());
        }
    }
    // Removing only the scope class restores transparency, not recolored pixels.
    grid->remove_css_class("artwork-library-grid"); frames();
    auto unscoped = render(*picture); ASSERT_EQ(unscoped.size(), 3u); EXPECT_EQ(unscoped[1], 0u);
    grid->add_css_class("artwork-library-grid");
    std::array<guint8, 16 * 16 * 4> unchanged{}; gdk_texture_download(texture->gobj(), unchanged.data(), 16 * 4);
    EXPECT_EQ(unchanged, pixels);
}

TEST_F(ArtworkLibraryViewTest, RemovedAndReconnectedActionsRefreshButtons)
{
    Gtk::Window window;
    application()->add_window(window);
    window.set_child(*view);
    window.present();
    frames();
    auto button = builder->get_widget<Gtk::Button>("insert-artwork"); ASSERT_TRUE(button);
    unsigned calls = 0;
    auto action = Gio::SimpleAction::create("insert");
    auto connection = action->signal_activate().connect([&](Glib::VariantBase const &) { ++calls; });
    group->add_action(action);
    EXPECT_TRUE(button->get_sensitive());
    EXPECT_TRUE(button->activate_action("library.insert"));
    EXPECT_EQ(calls, 1u);
    group->remove_action("insert");
    EXPECT_FALSE(button->get_sensitive());
    EXPECT_FALSE(button->activate_action("library.insert"));
    EXPECT_EQ(calls, 1u);
    group->add_action(action);
    EXPECT_TRUE(button->get_sensitive());
    EXPECT_TRUE(button->activate_action("library.insert"));
    EXPECT_EQ(calls, 2u);
    connection.disconnect();
    window.unset_child();
    window.set_visible(false);
    application()->remove_window(window);
    frames();
}

TEST_F(ArtworkLibraryViewTest, CollectionPopoverInheritsControllerActions)
{
    Gtk::Window window;
    application()->add_window(window);
    window.set_child(*view);
    auto picker = builder->get_widget<Gtk::MenuButton>("collection-picker"); ASSERT_TRUE(picker);
    unsigned calls = 0;
    auto action = Gio::SimpleAction::create("new");
    auto connection = action->signal_activate().connect([&](Glib::VariantBase const &) { ++calls; });
    group->add_action(action);
    window.present();
    picker->popup();
    frames();
    auto popover = picker->get_popover(); ASSERT_TRUE(popover);
    EXPECT_TRUE(popover->get_visible());
    EXPECT_TRUE(popover->activate_action("library.new"));
    EXPECT_EQ(calls, 1u);
    action->set_enabled(false);
    popover->activate_action("library.new");
    EXPECT_EQ(calls, 1u);
    picker->popdown();
    connection.disconnect();
    window.unset_child();
    window.set_visible(false);
    application()->remove_window(window);
    frames();
}

TEST_F(ArtworkLibraryViewTest, NativeNarrowLayoutCanBeResizedAndSnapshotted)
{
    Gtk::Window window;
    application()->add_window(window);
    auto cleanup = scope_exit{[&] {
        window.unset_child();
        window.set_visible(false);
        application()->remove_window(window);
        frames();
    }};
    auto wait_for_allocation = [](auto ready) {
        auto deadline = g_get_monotonic_time() + 5000000;
        while (!ready() && g_get_monotonic_time() < deadline) frames();
        return ready();
    };
    window.set_decorated(false);
    window.set_default_size(500, 500);
    window.set_child(*view);
    window.present();
    auto scroller = builder->get_widget<Gtk::ScrolledWindow>("artwork-scroller"); ASSERT_TRUE(scroller);
    ASSERT_TRUE(wait_for_allocation([&] {
        return window.get_mapped() && root->get_width() > 0 && scroller->get_height() >= 160;
    })) << "Initial native window allocation did not complete";
    auto initial_width = window.get_width(), initial_window_height = window.get_height();
    auto initial_height = scroller->get_height();
    RecordProperty("initial_window_width", initial_width);
    RecordProperty("initial_window_height", initial_window_height);
    RecordProperty("initial_scroller_height", initial_height);
    EXPECT_GE(initial_height, 160);
    EXPECT_LE(root->get_width(), 500);
    EXPECT_GT(root->get_width(), 0);
    window.set_default_size(620, 800);
    // Default-size only queues layout. Win32's layout request can prefer the
    // existing HWND size; explicitly present its new layout to exercise a real
    // native resize. Do not force-allocate the library or change its size hints.
    auto surface = gtk_native_get_surface(GTK_NATIVE(window.gobj())); ASSERT_TRUE(surface);
    ASSERT_TRUE(GDK_IS_TOPLEVEL(surface));
    auto layout = gdk_toplevel_layout_new();
    gdk_toplevel_layout_set_resizable(layout, window.get_resizable());
    gdk_toplevel_present(GDK_TOPLEVEL(surface), layout);
    gdk_toplevel_layout_unref(layout);
    auto resized = wait_for_allocation([&] {
        return window.get_width() > initial_width && window.get_height() > initial_window_height &&
               scroller->get_height() > initial_height;
    });
    RecordProperty("resized_window_width", window.get_width());
    RecordProperty("resized_window_height", window.get_height());
    RecordProperty("resized_scroller_height", scroller->get_height());
    EXPECT_TRUE(resized) << "Requested native size 620x800; observed window " << window.get_width()
                         << "x" << window.get_height() << ", scroller " << scroller->get_height();
    EXPECT_GT(window.get_width(), initial_width);
    EXPECT_GT(window.get_height(), initial_window_height);
    EXPECT_GT(scroller->get_height(), initial_height);
    // Optional evidence, never an alternate/fallback test oracle. Capture our
    // own GTK widget, not the user's desktop or another application's content.
    if (auto path = std::getenv("VACARDS_LIBRARY_VIEW_SNAPSHOT")) {
        auto paintable = std::unique_ptr<GdkPaintable, decltype(&g_object_unref)>(
            gtk_widget_paintable_new(GTK_WIDGET(window.gobj())), g_object_unref);
        auto snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable.get(), GDK_SNAPSHOT(snapshot), window.get_width(), window.get_height());
        auto node = std::unique_ptr<GskRenderNode, decltype(&gsk_render_node_unref)>(
            gtk_snapshot_free_to_node(snapshot), gsk_render_node_unref);
        ASSERT_TRUE(node);
        auto renderer = gtk_native_get_renderer(GTK_NATIVE(window.gobj()));
        ASSERT_TRUE(renderer);
        graphene_rect_t rect = GRAPHENE_RECT_INIT(0, 0, float(window.get_width()), float(window.get_height()));
        auto texture = std::unique_ptr<GdkTexture, decltype(&g_object_unref)>(
            gsk_renderer_render_texture(renderer, node.get(), &rect), g_object_unref);
        ASSERT_TRUE(texture);
        EXPECT_TRUE(gdk_texture_save_to_png(texture.get(), path));
    }
}

} // namespace
