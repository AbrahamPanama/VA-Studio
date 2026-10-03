// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise pointer crossing during the nested CDR page-selector event loop.
#include "config.h"
#include <gtest/gtest.h>
#include <csignal>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <glibmm/main.h>
#ifdef WITH_LIBCDR
#include <libcdr/libcdr.h>
#endif
#include "inkscape-application.h"
#include "extension/internal/rvng-import-dialog.h"
#include "ui/widget/canvas.h"
#include "ui/widget/canvas/pixelstreamer.h"
#include "util/crash-handler-thread.h"


namespace {
using Inkscape::UI::Widget::Canvas;
using Inkscape::Extension::Internal::RvngImportDialog;

void initialize_app()
{
    static auto app = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "canvaspreviewtest", true);
        auto result = new InkscapeApplication();
        result->gio_app()->register_application();
        for (auto signal : {SIGSEGV, SIGABRT, SIGILL, SIGFPE}) std::signal(signal, SIG_DFL);
        return result; // Native test application's process lifetime.
    }();
    (void)app;
}

template <typename T> T *find_widget(GtkWidget *widget)
{
    if (auto result = dynamic_cast<T *>(Glib::wrap(widget))) return result;
    for (auto child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        if (auto result = find_widget<T>(child)) return result;
    }
    return nullptr;
}

class CanvasPreview : public testing::TestWithParam<bool> {};

TEST_P(CanvasPreview, PointerCrossingsWhileChangingImportPages)
{
    auto const accept = GetParam();
    initialize_app();
    std::vector<librevenge::RVNGString> pages;
    // Optional private regression artwork stays outside the source repository.
    // When specified it must parse successfully; there is no synthetic fallback.
    if (auto file = g_getenv("VACARDS_TEST_CDR")) {
#ifdef WITH_LIBCDR
        librevenge::RVNGFileStream input(file);
        ASSERT_TRUE(libcdr::CDRDocument::isSupported(&input)) << file;
        librevenge::RVNGStringVector output;
        librevenge::RVNGSVGDrawingGenerator generator(output, "svg");
        ASSERT_TRUE(libcdr::CDRDocument::parse(&input, &generator)) << file;
        for (unsigned i = 0; i < output.size(); ++i) pages.push_back(output[i]);
#else
        FAIL() << "VACARDS_TEST_CDR requires the libcdr-enabled application";
#endif
    } else {
        pages.emplace_back("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'><rect width='90' height='90' fill='red'/></svg>");
        pages.emplace_back("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'><circle cx='50' cy='50' r='45' fill='blue'/></svg>");
    }
    ASSERT_GE(pages.size(), 2u);
    RvngImportDialog dialog(pages);
    auto canvas = find_widget<Canvas>(GTK_WIDGET(dialog.gobj()));
    auto spinner = find_widget<Gtk::SpinButton>(GTK_WIDGET(dialog.gobj()));
    ASSERT_TRUE(canvas);
    ASSERT_TRUE(spinner);
    ASSERT_EQ(canvas->get_desktop(), nullptr); // Preview deliberately has no editor.
    auto controllers = gtk_widget_observe_controllers(GTK_WIDGET(canvas->gobj()));
    GtkEventControllerMotion *motion = nullptr;
    for (unsigned i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        auto controller = G_OBJECT(g_list_model_get_item(controllers, i));
        if (GTK_IS_EVENT_CONTROLLER_MOTION(controller) && !motion) {
            motion = GTK_EVENT_CONTROLLER_MOTION(controller);
        } else {
            g_object_unref(controller);
        }
    }
    g_object_unref(controllers);
    ASSERT_TRUE(motion);
    unsigned crossings = 0;
    unsigned readiness_checks = 0;
    auto timer = Glib::signal_timeout().connect([&] {
        if (!canvas->get_mapped() || canvas->get_width() <= 0) {
            if (++readiness_checks < 100) return true;
            ADD_FAILURE() << "Preview must be mapped to exercise canvas picking";
            dialog.response(Gtk::ResponseType::CANCEL);
            return false;
        }
        g_signal_emit_by_name(motion, "enter", 25.0, 25.0);
        g_signal_emit_by_name(motion, "leave");
        ++crossings;
        spinner->set_value(crossings % pages.size() + 1);
        if (crossings == 32) {
            dialog.response(accept ? Gtk::ResponseType::OK : Gtk::ResponseType::CANCEL);
            return false;
        }
        return true;
    }, 100);
    EXPECT_EQ(dialog.showDialog(), accept);
    timer.disconnect();
    g_object_unref(motion);
    EXPECT_EQ(crossings, 32u);
    EXPECT_EQ(dialog.getSelectedPage(), 32 % pages.size() + 1);
}
TEST(CrashHandlerThread, OnlyTheRegisteredMainThreadMayUseGtk)
{
    auto const main_thread = std::this_thread::get_id();
    EXPECT_TRUE(Inkscape::Util::crash_handler_on_main_thread(main_thread));
    bool worker_may_use_gtk = true;
    std::thread worker([&] {
        worker_may_use_gtk = Inkscape::Util::crash_handler_on_main_thread(main_thread);
    });
    worker.join();
    EXPECT_FALSE(worker_may_use_gtk);
}

TEST(PixelStreamer, PersistentTileLargerThanSixteenMiBHasRoom)
{
    using Inkscape::UI::Widget::PixelStreamer;
    constexpr int limit = 0x1000000;
    EXPECT_EQ(PixelStreamer::persistent_capacity_for(limit), limit);
    EXPECT_EQ(PixelStreamer::persistent_capacity_for(limit + 64), limit + 64);
    // 1500 px tiles at scale 2 use 3000 * 3000 * 4 bytes.
    EXPECT_EQ(PixelStreamer::persistent_capacity_for(3000 * 3000 * 4), 3000 * 3000 * 4);
}

class CanvasXray : public testing::TestWithParam<bool> {};

TEST_P(CanvasXray, SplitAndXrayRedraws)
{
    initialize_app();
    std::vector<librevenge::RVNGString> pages;
    std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='400'>";
    for (unsigned i = 0; i < 1600; ++i) {
        svg += "<circle cx='" + std::to_string(i % 40 * 10) + "' cy='" + std::to_string(i / 40 * 10)
             + "' r='8' fill='red' stroke='blue'/>";
    }
    svg += "</svg>";
    pages.emplace_back(svg.c_str());
    RvngImportDialog dialog(pages);
    auto canvas = find_widget<Canvas>(GTK_WIDGET(dialog.gobj()));
    ASSERT_TRUE(canvas);
    canvas->set_opengl_enabled(GetParam());
    auto controllers = gtk_widget_observe_controllers(GTK_WIDGET(canvas->gobj()));
    GtkEventControllerMotion *motion = nullptr;
    for (unsigned i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        auto controller = G_OBJECT(g_list_model_get_item(controllers, i));
        if (GTK_IS_EVENT_CONTROLLER_MOTION(controller) && !motion) {
            motion = GTK_EVENT_CONTROLLER_MOTION(controller);
        } else {
            g_object_unref(controller);
        }
    }
    g_object_unref(controllers);
    ASSERT_TRUE(motion);
    unsigned redraws = 0, checks = 0;
    auto timer = Glib::signal_timeout().connect([&] {
        if (!canvas->get_mapped() || canvas->get_width() <= 0) {
            if (++checks < 100) return true;
            ADD_FAILURE() << "Canvas must be mapped";
            dialog.response(Gtk::ResponseType::CANCEL);
            return false;
        }
        if (redraws == 0) {
            EXPECT_EQ(canvas->get_opengl_enabled(), GetParam());
            std::cerr << "Xray test: GL=" << canvas->get_opengl_enabled()
                      << " scale=" << canvas->get_scale_factor() << std::endl;
        }
        canvas->set_split_mode(redraws % 2 ? Inkscape::SplitMode::XRAY : Inkscape::SplitMode::SPLIT);
        // Enter sets the pointer without requiring a native GdkMotionEvent.
        // Keep the X-ray circle active while tile workers redraw and the split
        // controller leases/returns mappings on the GTK thread.
        g_signal_emit_by_name(motion, "enter", 20.0 + redraws % 350, 20.0 + (redraws * 3) % 350);
        canvas->queue_draw();
        EXPECT_TRUE(canvas->get_last_mouse().has_value());
        canvas->set_affine(Geom::Scale(1.0 + (redraws % 7) * 0.2));
        canvas->redraw_all();
        if (++redraws == 600) {
            dialog.response(Gtk::ResponseType::CANCEL);
            return false;
        }
        return true;
    }, 40);
    EXPECT_FALSE(dialog.showDialog());
    timer.disconnect();
    g_object_unref(motion);
    EXPECT_EQ(redraws, 600u);
}
INSTANTIATE_TEST_SUITE_P(CairoAndOpenGL, CanvasXray, testing::Bool());
INSTANTIATE_TEST_SUITE_P(AcceptAndCancel, CanvasPreview, testing::Bool());
} // namespace
