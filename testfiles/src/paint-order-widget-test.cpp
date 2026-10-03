// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Fill and Stroke "Order" row on a real desktop: choosing a paint order writes
 * the selection's members in one "Set paint order" Undo step, and a
 * programmatic refresh of the row never writes. A GUI suite (INKSCAPE_TEST_GUI);
 * the headless query cases stay in paint-order-query-test.cpp.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/window.h>

#include "desktop.h"
#include "desktop-style.h"
#include "document.h"
#include "document-undo.h"
#include "event-log.h"
#include "extension/init.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "style.h"
#include "selection.h"
#include "ui/widget/canvas.h"
#include "ui/widget/gtk-registry.h"
#include "ui/widget/stroke-style.h"
#include "xml/repr.h"

namespace {

class PaintOrderQueryTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    }

    static std::unique_ptr<SPDocument> parse(std::string const &body)
    {
        auto document = SPDocument::createNewDocFromMem(
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">)" + body + "</svg>");
        if (document) document->ensureUpToDate();
        return document;
    }
};

template <typename T>
T *find_widget(Gtk::Widget &root)
{
    if (auto *widget = dynamic_cast<T *>(&root)) return widget;
    for (auto *child = root.get_first_child(); child; child = child->get_next_sibling()) {
        if (auto *widget = find_widget<T>(*child)) return widget;
    }
    return nullptr;
}

class PaintOrderWriteTest : public PaintOrderQueryTest
{
protected:
    static void SetUpTestSuite()
    {
        PaintOrderQueryTest::SetUpTestSuite();
        static auto *app = [] {
            g_setenv("INKSCAPE_APP_ID_TAG", "paintOrderWriteTest", TRUE);
            auto *app = new InkscapeApplication();
            if (app->gtk_app()) Inkscape::UI::Widget::register_all();
            return app;
        }();
        ASSERT_TRUE(app->gtk_app()) << "a real GTK desktop is required for the write test";
        Inkscape::Extension::init(); // Marker previews open SVGs via the native input extension.
    }

    void SetUp() override
    {
        document = parse(group_fixture());
        ASSERT_TRUE(document);
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        canvas_host = std::make_unique<Gtk::Window>();
        canvas_host->set_child(*desktop->getCanvas());
        desktop->getSelection()->set(cast<SPItem>(document->getObjectById("g")));
        // The constructor must see A as active, so the old lambda's captured
        // desktop is real and remains alive when the panel is rebound to B.
        Inkscape::Application::instance().add_desktop(desktop.get());
        registered_desktop = true;
        ASSERT_EQ(SP_ACTIVE_DESKTOP, desktop.get());
        panel = std::make_unique<Inkscape::UI::Widget::StrokeStyle>();
        // Match updateLine's four-parent dialog lookup without constructing a
        // full dialog. These ordinary boxes keep its visibility gate open.
        inner.append(*panel);
        middle.append(inner);
        outer.append(middle);
        panel_host = std::make_unique<Gtk::Window>();
        panel_host->set_child(outer);
        panel->setDesktop(desktop.get());
        order = find_widget<Inkscape::UI::Widget::PaintOrderWidget>(*panel);
        ASSERT_TRUE(order);
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
    }

    void TearDown() override
    {
        if (panel) panel->setDesktop(nullptr);
        if (panel_host) panel_host->unset_child();
        if (panel && panel->get_parent() == &inner) inner.remove(*panel);
        if (inner.get_parent() == &middle) middle.remove(inner);
        if (middle.get_parent() == &outer) outer.remove(middle);
        panel.reset();
        panel_host.reset();
        if (canvas_host_b) canvas_host_b->unset_child();
        canvas_host_b.reset();
        if (registered_desktop_b) Inkscape::Application::instance().remove_desktop(desktop_b.get());
        desktop_b.reset();
        document_b.reset();
        if (canvas_host) canvas_host->unset_child();
        canvas_host.reset();
        if (registered_desktop) Inkscape::Application::instance().remove_desktop(desktop.get());
        desktop.reset();
        document.reset();
    }

    std::string xml() const { return sp_repr_save_buf(document->getReprDoc()).raw(); }
    static std::string group_fixture()
    {
        return R"(<g id="g"><text id="text" x="0" y="20"><tspan id="span" x="0" y="20" fill="red" stroke="none" font-size="12">Corel text</tspan></text>)"
               R"(<path id="path" d="M0 30h10v10z" style="fill:blue"/></g>)";
    }

    static void apply_stroke(SPDesktop *target)
    {
        // Reuse the desktop's native recursive style path, including the
        // imported tspan's presentation attributes. This is fixture setup;
        // start a clean history afterwards to isolate the Order transaction.
        auto *css = sp_repr_css_attr_new();
        sp_repr_css_set_property(css, "stroke", "#008000");
        sp_desktop_set_style(target, css);
        sp_repr_css_attr_unref(css);
        auto *doc = target->getDocument();
        doc->ensureUpToDate();
        DocumentUndo::done(doc, Inkscape::Util::Internal::ContextString("Prepare stroke colour"), "");
        DocumentUndo::clearUndo(doc);
        DocumentUndo::clearRedo(doc);
    }

    void choose_fill_in_front()
    {
        ASSERT_TRUE(order->get_sensitive()) << "a user must be able to rearrange the Order row";
        SPIPaintOrder value;
        value.read("stroke fill");
        order->setValue(value, false);
        // The drag rearrangement emits this existing signal. Exercise the
        // connected panel writer rather than reproducing its CSS/Undo code.
        order->signal_tab_rearranged().emit(2, 1);
    }

    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<SPDocument> document_b;
    std::unique_ptr<SPDesktop> desktop_b;
    std::unique_ptr<Gtk::Window> canvas_host_b;
    std::unique_ptr<Gtk::Window> canvas_host, panel_host;
    Gtk::Box outer, middle, inner;
    std::unique_ptr<Inkscape::UI::Widget::StrokeStyle> panel;
    Inkscape::UI::Widget::PaintOrderWidget *order = nullptr;
    bool registered_desktop = false;
    bool registered_desktop_b = false;
};

TEST_F(PaintOrderWriteTest, GroupMembersOneUndoAndRefreshDoesNotWrite)
{
    EXPECT_FALSE(order->get_sensitive()) << "the selected group initially has no stroke";
    apply_stroke(desktop.get());
    panel->updateLine();
    ASSERT_TRUE(order->get_sensitive());
    for (auto id : {"g", "text", "span", "path"}) {
        auto *object = document->getObjectById(id);
        ASSERT_TRUE(object && object->style);
        ASSERT_FALSE(object->style->stroke.isNone()) << id;
        EXPECT_FALSE(object->style->paint_order.set) << id;
    }
    auto const before = xml();
    auto *log = document->get_event_log();
    auto const initial_serial = log->getCurrEventSerial();
    unsigned changes = 0;
    auto connection = order->signal_values_changed().connect([&] { ++changes; });
    choose_fill_in_front();
    EXPECT_EQ(changes, 1u);
    auto const after = xml();
    ASSERT_NE(after, before);
    auto const action_serial = log->getCurrEventSerial();
    EXPECT_NE(action_serial, initial_serial);
    auto const &columns = Inkscape::EventLog::getColumns();
    EXPECT_EQ(Glib::ustring((*log->getCurrEvent())[columns.description]), "Set paint order");
    EXPECT_EQ(Glib::ustring((*log->getCurrEvent())[columns.icon_name]), "dialog-fill-and-stroke");
    for (auto id : {"g", "text", "span", "path"}) {
        auto *object = document->getObjectById(id);
        ASSERT_TRUE(object && object->style);
        EXPECT_EQ(object->style->paint_order.get_value(), "stroke fill markers") << id;
        EXPECT_TRUE(object->style->paint_order.set) << id;
    }

    // Disturb only the widget so refresh must visibly restore the queried
    // order. setValue and updateLine must emit no user-change signal.
    SPIPaintOrder normal;
    normal.read("normal");
    order->setValue(normal, false);
    panel->updateLine();
    EXPECT_EQ(order->getValue().get_value(), "stroke fill markers");
    EXPECT_EQ(changes, 1u);
    EXPECT_EQ(xml(), after);
    EXPECT_EQ(log->getCurrEventSerial(), action_serial);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "the order choice owns exactly one step";
    panel->updateLine();
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "refresh must not add an Undo step";
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    EXPECT_EQ(xml(), after);
    connection.disconnect();
}

TEST_F(PaintOrderWriteTest, RebindingDesktopWritesOnlyTheNewSelection)
{
    apply_stroke(desktop.get());
    panel->updateLine();
    ASSERT_TRUE(order->get_sensitive());
    auto const before_a = xml();
    auto const serial_a = document->get_event_log()->getCurrEventSerial();

    document_b = parse(group_fixture());
    ASSERT_TRUE(document_b);
    desktop_b = std::make_unique<SPDesktop>(document_b->getNamedView());
    canvas_host_b = std::make_unique<Gtk::Window>();
    canvas_host_b->set_child(*desktop_b->getCanvas());
    desktop_b->getSelection()->set(cast<SPItem>(document_b->getObjectById("g")));
    Inkscape::Application::instance().add_desktop(desktop_b.get());
    registered_desktop_b = true;
    ASSERT_EQ(SP_ACTIVE_DESKTOP, desktop_b.get());
    apply_stroke(desktop_b.get());
    auto const before_b = sp_repr_save_buf(document_b->getReprDoc()).raw();
    panel->setDesktop(desktop_b.get());
    ASSERT_TRUE(order->get_sensitive());
    choose_fill_in_front();

    EXPECT_EQ(xml(), before_a) << "desktop A must never receive B's Order edit";
    EXPECT_EQ(document->get_event_log()->getCurrEventSerial(), serial_a);
    for (auto id : {"g", "text", "span", "path"}) {
        auto *a = document->getObjectById(id);
        auto *b = document_b->getObjectById(id);
        ASSERT_TRUE(a && a->style && b && b->style);
        EXPECT_FALSE(a->style->paint_order.set) << "desktop A: " << id;
        EXPECT_TRUE(b->style->paint_order.set) << "desktop B: " << id;
        EXPECT_EQ(b->style->paint_order.get_value(), "stroke fill markers") << "desktop B: " << id;
    }
    auto const after_b = sp_repr_save_buf(document_b->getReprDoc()).raw();
    EXPECT_NE(after_b, before_b);
    panel->updateLine();
    EXPECT_EQ(order->getValue().get_value(), "stroke fill markers");
    EXPECT_EQ(sp_repr_save_buf(document_b->getReprDoc()).raw(), after_b);
    ASSERT_TRUE(DocumentUndo::undo(document_b.get()));
    EXPECT_EQ(sp_repr_save_buf(document_b->getReprDoc()).raw(), before_b);
    EXPECT_FALSE(DocumentUndo::undo(document_b.get())) << "B's choice owns exactly one step";
    ASSERT_TRUE(DocumentUndo::redo(document_b.get()));
    EXPECT_EQ(sp_repr_save_buf(document_b->getReprDoc()).raw(), after_b);
    EXPECT_EQ(xml(), before_a);
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "A must have no Order transaction";
}

TEST_F(PaintOrderWriteTest, ReentrantOrderSignalDuringRefreshDoesNotWrite)
{
    // Change both numeric controls' source values, so whichever SpinButton
    // appears first in GTK's child tree emits during the real updateLine.
    document->getObjectById("path")->getRepr()->setAttribute(
        "style", "fill:blue;stroke:black;stroke-width:7;stroke-miterlimit:9");
    desktop->getSelection()->set(cast<SPItem>(document->getObjectById("path")));
    DocumentUndo::done(document.get(), Inkscape::Util::Internal::ContextString("Prepare fixture"), "");
    DocumentUndo::clearUndo(document.get());
    DocumentUndo::clearRedo(document.get());
    auto const before = xml();
    auto *spin = find_widget<Inkscape::UI::Widget::SpinButton>(*panel);
    ASSERT_TRUE(spin);
    unsigned refresh_signals = 0;
    auto connection = spin->get_adjustment()->signal_value_changed().connect([&] {
        ++refresh_signals;
        order->signal_values_changed().emit();
    });
    panel->updateLine();
    EXPECT_GT(refresh_signals, 0u) << "the handler must be exercised while updateLine is running";
    EXPECT_EQ(xml(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    connection.disconnect();
}

} // namespace
