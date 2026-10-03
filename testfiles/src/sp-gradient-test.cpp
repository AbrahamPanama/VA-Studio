// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unit tests migrated from cxxtest
 *
 * Authors:
 *   Adrian Boguszewski
 *
 * Copyright (C) 2018 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>
#include <doc-per-case-test.h>
#include <src/object/sp-gradient.h>
#include <src/attributes.h>
#include <2geom/transforms.h>
#include <src/xml/node.h>
#include <src/xml/simple-document.h>
#include <src/svg/svg.h>
#include <src/desktop.h>
#include <src/document-undo.h>
#include <src/gradient-chemistry.h>
#include <src/helper/stock-items.h>
#include <src/inkscape-application.h>
#include <src/inkscape.h>
#include <src/object/sp-item.h>
#include <src/selection.h>
#include <src/ui/widget/gradient-selector.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/treeview.h>
#include <src/object/sp-defs.h>
#include <src/object/sp-stop.h>
#include <src/colors/color.h>
#include <cstring>
#include <iostream>

using namespace Inkscape;
using namespace Inkscape::XML;

class SPGradientTest: public DocPerCaseTest {
public:
    SPGradientTest() {
        DocPerCaseTest::SetUpTestCase();
        gr = new SPGradient();
    }

    ~SPGradientTest() override {
        delete gr;
        DocPerCaseTest::TearDownTestCase();
    }

    SPGradient *gr;
};

TEST_F(SPGradientTest, Init) {
    ASSERT_TRUE(gr != nullptr);
    EXPECT_TRUE(gr->gradientTransform.isIdentity());
    EXPECT_TRUE(Geom::are_near(Geom::identity(), gr->gradientTransform));
}

TEST_F(SPGradientTest, SetGradientTransform) {
    gr->document = _doc.get();

    gr->setKeyValue(SPAttr::GRADIENTTRANSFORM, "translate(5, 8)");
    EXPECT_TRUE(Geom::are_near(Geom::Affine(Geom::Translate(5.0, 8.0)), gr->gradientTransform));

    gr->setKeyValue(SPAttr::GRADIENTTRANSFORM, "");
    EXPECT_TRUE(Geom::are_near(Geom::identity(), gr->gradientTransform));

    gr->setKeyValue(SPAttr::GRADIENTTRANSFORM, "rotate(90)");
    EXPECT_TRUE(Geom::are_near(Geom::Affine(Geom::Rotate::from_degrees(90.0)), gr->gradientTransform));
}

TEST_F(SPGradientTest, Write) {
    gr->document = _doc.get();

    gr->setKeyValue(SPAttr::GRADIENTTRANSFORM, "matrix(0, 1, -1, 0, 0, 0)");
    Document *xml_doc = _doc->getReprDoc();

    ASSERT_TRUE(xml_doc != nullptr);

    Node *repr = xml_doc->createElement("svg:radialGradient");
    gr->updateRepr(xml_doc, repr, SP_OBJECT_WRITE_ALL);

    gchar const *tr = repr->attribute("gradientTransform");
    Geom::Affine svd;
    bool const valid = sp_svg_transform_read(tr, &svd);

    EXPECT_TRUE(valid);
    EXPECT_TRUE(Geom::are_near(Geom::Affine(Geom::Rotate::from_degrees(90.0)), svd));
}

TEST_F(SPGradientTest, GetG2dGetGs2dSetGs2) {
    gr->document = _doc.get();

    Geom::Affine grXform(2, 1,
                         1, 3,
                         4, 6);
    gr->gradientTransform = grXform;

    Geom::Rect unit_rect(Geom::Point(0, 0), Geom::Point(1, 1));
    {
        Geom::Affine g2d(gr->get_g2d_matrix(Geom::identity(), unit_rect));
        Geom::Affine gs2d(gr->get_gs2d_matrix(Geom::identity(), unit_rect));
        EXPECT_TRUE(Geom::are_near(Geom::identity(), g2d));
        EXPECT_TRUE(Geom::are_near(gs2d, gr->gradientTransform * g2d, 1e-12));

        gr->set_gs2d_matrix(Geom::identity(), unit_rect, gs2d);
        EXPECT_TRUE(Geom::are_near(gr->gradientTransform, grXform, 1e-12));
    }

    gr->gradientTransform = grXform;
    Geom::Affine funny(2, 3,
                       4, 5,
                       6, 7);
    {
        Geom::Affine g2d(gr->get_g2d_matrix(funny, unit_rect));
        Geom::Affine gs2d(gr->get_gs2d_matrix(funny, unit_rect));
        EXPECT_TRUE(Geom::are_near(funny, g2d));
        EXPECT_TRUE(Geom::are_near(gs2d, gr->gradientTransform * g2d, 1e-12));

        gr->set_gs2d_matrix(funny, unit_rect, gs2d);
        EXPECT_TRUE(Geom::are_near(gr->gradientTransform, grXform, 1e-12));
    }

    gr->gradientTransform = grXform;
    Geom::Rect larger_rect(Geom::Point(5, 6), Geom::Point(8, 10));
    {
        Geom::Affine g2d(gr->get_g2d_matrix(funny, larger_rect));
        Geom::Affine gs2d(gr->get_gs2d_matrix(funny, larger_rect));
        EXPECT_TRUE(Geom::are_near(Geom::Affine(3, 0,
                                                0, 4,
                                                5, 6) * funny, g2d ));
        EXPECT_TRUE(Geom::are_near(gs2d, gr->gradientTransform * g2d, 1e-12));

        gr->set_gs2d_matrix(funny, larger_rect, gs2d);
        EXPECT_TRUE(Geom::are_near(gr->gradientTransform, grXform, 1e-12));

        gr->setKeyValue( SPAttr::GRADIENTUNITS, "userSpaceOnUse");
        Geom::Affine user_g2d(gr->get_g2d_matrix(funny, larger_rect));
        Geom::Affine user_gs2d(gr->get_gs2d_matrix(funny, larger_rect));
        EXPECT_TRUE(Geom::are_near(funny, user_g2d));
        EXPECT_TRUE(Geom::are_near(user_gs2d, gr->gradientTransform * user_g2d, 1e-12));
    }
}

namespace {
using Inkscape::UI::Widget::GradientSelector;

class GradientPresetTest : public ::testing::Test {
protected:
    void SetUp() override {
        static auto *app = [] {
            g_setenv("INKSCAPE_APP_ID_TAG", "gradientpresettest", TRUE);
            auto *result = new InkscapeApplication();
            result->gio_app()->register_application();
            return result;
        }();
        application = app;
        if (!Application::exists()) Application::create(false);
        document = SPDocument::createNewDocFromMem(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect id="one" width="20" height="20" fill="#ff0000" stroke="#00ff00"/><rect id="two" x="25" width="20" height="20" fill="#0000ff" stroke="#ffff00"/></svg>)");
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        desktop = std::make_unique<SPDesktop>(document->getNamedView());
        INKSCAPE.add_desktop(desktop.get());
        application->set_active_desktop(desktop.get());
        application->set_active_document(document.get());
        application->set_active_selection(desktop->getSelection());
        selector = std::make_unique<GradientSelector>();
        selector->setVector(document.get(), nullptr);
        auto scroller = dynamic_cast<Gtk::ScrolledWindow *>(selector->get_first_child());
        ASSERT_TRUE(scroller);
        tree = dynamic_cast<Gtk::TreeView *>(scroller->get_child());
        ASSERT_TRUE(tree);
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Initialize gradient fixture"}, "document-new");
        DocumentUndo::clearUndo(document.get());
    }

    void TearDown() override {
        selector.reset();
        tree = nullptr;
        application->set_active_selection(nullptr);
        application->set_active_document(nullptr);
        application->set_active_desktop(nullptr);
        INKSCAPE.remove_desktop(desktop.get());
        desktop.reset();
        document.reset();
    }

    int copies(char const *id) const {
        int count = 0;
        for (auto &child : document->getDefs()->children) {
            auto stockid = child.getRepr()->attribute("inkscape:stockid");
            if (stockid && std::strcmp(stockid, id) == 0) ++count;
        }
        return count;
    }

    void choose(char const *id) {
        auto model = tree->get_model();
        for (auto iter = model->children().begin(); iter != model->children().end(); ++iter) {
            auto label = Glib::ustring((*iter)[tree_columns().preset_id]);
            if (label == id) { tree->get_selection()->select(iter); return; }
        }
        ADD_FAILURE() << "Missing preset " << id;
    }

    GradientSelector::ModelColumns const &tree_columns() const {
        // Column layout is part of the selector model contract.
        static GradientSelector::ModelColumns columns;
        return columns;
    }

    InkscapeApplication *application = nullptr;
    std::unique_ptr<SPDocument> document;
    std::unique_ptr<SPDesktop> desktop;
    std::unique_ptr<GradientSelector> selector;
    Gtk::TreeView *tree = nullptr;
};
}

TEST_F(GradientPresetTest, LibraryOrderAndProtectedRows) {
    auto imported = cast<SPGradient>(get_stock_item("urn:inkscape:gradient:vac-gold", false));
    ASSERT_TRUE(imported);
    selector->setVector(document.get(), imported);
    auto model = tree->get_model();
    std::vector<std::string> rows;
    for (auto iter = model->children().begin(); iter != model->children().end(); ++iter) {
        auto kind = (*iter)[tree_columns().kind];
        auto label = Glib::ustring((*iter)[tree_columns().name]).raw();
        rows.push_back(std::to_string(kind) + ":" + label);
        std::cout << "gradient-row " << rows.back() << '\n';
        if (kind == GradientSelector::ModelColumns::HEADER || kind == GradientSelector::ModelColumns::GROUP) {
            tree->get_selection()->select(iter);
            EXPECT_NE(tree->get_selection()->get_selected(), iter);
        }
    }
    EXPECT_EQ(rows.size(), 30u); // one document gradient, header, four groups, 24 presets
    EXPECT_EQ(rows[0], "0:Gold");
    EXPECT_EQ(rows[1], "1:Presets");
    EXPECT_EQ(rows[2], "2:Metallic");
    EXPECT_EQ(rows[9], "2:Nature");
    EXPECT_EQ(rows[16], "2:Earth tones");
    EXPECT_EQ(rows[23], "2:Basics");
    EXPECT_EQ(rows[3], "3:Gold");
    EXPECT_EQ(rows[29], "3:Neon");
}

TEST_F(GradientPresetTest, EmptySelectionDoesNotImport) {
    ASSERT_EQ(copies("vac-gold"), 0);
    choose("vac-gold");
    EXPECT_EQ(copies("vac-gold"), 0);
    EXPECT_FALSE(tree->get_selection()->get_selected());
}

TEST_F(GradientPresetTest, BlockedHandlerLeavesNoCopyOrUndo) {
    auto one = cast<SPItem>(document->getObjectById("one"));
    ASSERT_TRUE(one);
    one->getRepr()->setAttribute("fill", "#123456");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Redo fixture"}, "color-gradient");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->setModifiedSinceSave(false);
    ASSERT_FALSE(document->isModifiedSinceSave());
    one = cast<SPItem>(document->getObjectById("one"));
    ASSERT_TRUE(one);
    desktop->getSelection()->set(one);
    bool became_modified = false;
    auto modified_connection = document->connectSavedOrModified([&] {
        became_modified |= document->isModifiedSinceSave();
    });
    selector->signal_changed().connect([](SPGradient *) {}); // application declines the edit
    choose("vac-gold");
    modified_connection.disconnect();
    EXPECT_EQ(copies("vac-gold"), 0);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    EXPECT_FALSE(became_modified);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_TRUE(DocumentUndo::redo(document.get()));
}

TEST_F(GradientPresetTest, EditedGoldImportsFreshOriginal) {
    auto one = cast<SPItem>(document->getObjectById("one"));
    ASSERT_TRUE(one);
    desktop->getSelection()->set(one);
    selector->signal_changed().connect([this](SPGradient *gradient) {
        for (auto item : desktop->getSelection()->items_vector())
            sp_item_apply_gradient(item, gradient, desktop.get(), SP_GRADIENT_TYPE_LINEAR, false, FILL);
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Apply preset"}, "color-gradient");
    });
    choose("vac-gold");
    auto original = cast<SPGradient>(document->getObjectById("vac-gold"));
    ASSERT_TRUE(original && original->getFirstStop());
    original->getFirstStop()->getRepr()->setAttribute("style", "stop-color:#000000;stop-opacity:1");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Edit stop"}, "color-gradient");
    tree->get_selection()->unselect_all();
    choose("vac-gold");
    EXPECT_GE(copies("vac-gold"), 1);
    auto applied = sp_item_get_gradient(one, true);
    ASSERT_TRUE(applied && applied->getFirstStop());
    EXPECT_NE(applied, original);
    EXPECT_EQ(applied->getFirstStop()->getColor(), get_stock_gradient_source("vac-gold")->getFirstStop()->getColor());
}

TEST_F(GradientPresetTest, DownSkipsHeaderAndGroup) {
    auto imported = import_stock_gradient("vac-gold", document.get());
    ASSERT_TRUE(imported);
    auto two = cast<SPItem>(document->getObjectById("two"));
    ASSERT_TRUE(two);
    sp_item_apply_gradient(two, imported, desktop.get(), SP_GRADIENT_TYPE_LINEAR, false, FILL);
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Set up gradient row"}, "color-gradient");
    DocumentUndo::clearUndo(document.get());
    imported = cast<SPGradient>(document->getObjectById("vac-gold"));
    ASSERT_TRUE(imported);
    selector->setVector(document.get(), imported);
    auto one = cast<SPItem>(document->getObjectById("one"));
    ASSERT_TRUE(one);
    desktop->getSelection()->set(one);
    std::string chosen;
    selector->signal_changed().connect([this, &chosen](SPGradient *gradient) {
        chosen = gradient->getRepr()->attribute("inkscape:stockid") ?: "";
        for (auto item : desktop->getSelection()->items_vector())
            sp_item_apply_gradient(item, gradient, desktop.get(), SP_GRADIENT_TYPE_LINEAR, false, FILL);
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Apply preset"}, "color-gradient");
    });
    auto model = tree->get_model();
    auto first = model->children().begin();
    ASSERT_TRUE(first);
    tree->get_selection()->select(first);
    auto controllers = gtk_widget_observe_controllers(GTK_WIDGET(tree->gobj()));
    for (guint i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        auto controller = g_list_model_get_item(controllers, i);
        if (GTK_IS_EVENT_CONTROLLER_KEY(controller)) {
            gboolean handled = FALSE;
            g_signal_emit_by_name(controller, "key-pressed", GDK_KEY_Down, 0u, 0u, &handled);
            g_object_unref(controller);
            break;
        }
        g_object_unref(controller);
    }
    g_object_unref(controllers);
    EXPECT_EQ(chosen, "vac-gold");
    auto selected = tree->get_selection()->get_selected();
    ASSERT_TRUE(selected);
    EXPECT_EQ((*selected)[tree_columns().kind], GradientSelector::ModelColumns::PRESET);
    EXPECT_EQ(Glib::ustring((*selected)[tree_columns().preset_id]), "vac-gold");
}

TEST_F(GradientPresetTest, UpFromFirstPresetSkipsPlaceholder) {
    auto model = tree->get_model();
    auto placeholder = model->children().begin();
    ASSERT_TRUE(placeholder);
    EXPECT_EQ((*placeholder)[tree_columns().kind], GradientSelector::ModelColumns::HEADER);
    Gtk::TreeModel::iterator first_preset;
    for (auto iter = model->children().begin(); iter != model->children().end(); ++iter) {
        if (Glib::ustring((*iter)[tree_columns().preset_id]) == "vac-gold") {
            first_preset = iter;
            break;
        }
    }
    ASSERT_TRUE(first_preset);
    auto selection = tree->get_selection();
    auto signal_id = g_signal_lookup("changed", GTK_TYPE_TREE_SELECTION);
    g_signal_handlers_block_matched(selection->gobj(), G_SIGNAL_MATCH_ID, signal_id, 0, nullptr, nullptr, nullptr);
    selection->select(first_preset);
    auto controllers = gtk_widget_observe_controllers(GTK_WIDGET(tree->gobj()));
    for (guint i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        auto controller = g_list_model_get_item(controllers, i);
        if (GTK_IS_EVENT_CONTROLLER_KEY(controller)) {
            gboolean handled = FALSE;
            g_signal_emit_by_name(controller, "key-pressed", GDK_KEY_Up, 0u, 0u, &handled);
        }
        g_object_unref(controller);
    }
    g_object_unref(controllers);
    auto selected = selection->get_selected();
    ASSERT_TRUE(selected);
    EXPECT_EQ(Glib::ustring((*selected)[tree_columns().preset_id]), "vac-gold");
    g_signal_handlers_unblock_matched(selection->gobj(), G_SIGNAL_MATCH_ID, signal_id, 0, nullptr, nullptr, nullptr);
}

TEST_F(GradientPresetTest, ApplyTwoObjectsAndOneUndo) {
    auto one = cast<SPItem>(document->getObjectById("one"));
    auto two = cast<SPItem>(document->getObjectById("two"));
    ASSERT_TRUE(one && two);
    desktop->getSelection()->setList(std::vector<SPItem *>{one, two});
    selector->signal_changed().connect([this](SPGradient *gradient) {
        for (auto item : desktop->getSelection()->items_vector())
            sp_item_apply_gradient(item, gradient, desktop.get(), SP_GRADIENT_TYPE_LINEAR, false, FILL);
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Apply preset"}, "color-gradient");
    });
    choose("vac-gold");
    ASSERT_EQ(copies("vac-gold"), 1);
    auto imported = cast<SPGradient>(document->getObjectById("vac-gold"));
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported->getFirstStop());
    EXPECT_NE(std::string(imported->getFirstStop()->getRepr()->attribute("style") ?: "").find("#8a6e2f"), std::string::npos);
    EXPECT_NE(std::string(one->getRepr()->attribute("style") ?: "").find("url("), std::string::npos);
    EXPECT_NE(std::string(two->getRepr()->attribute("style") ?: "").find("url("), std::string::npos);
    EXPECT_EQ(std::string(one->getRepr()->attribute("stroke") ?: ""), "#00ff00");
    EXPECT_EQ(std::string(two->getRepr()->attribute("stroke") ?: ""), "#ffff00");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_EQ(copies("vac-gold"), 0);
    one = cast<SPItem>(document->getObjectById("one"));
    two = cast<SPItem>(document->getObjectById("two"));
    ASSERT_TRUE(one && two);
    EXPECT_EQ(std::string(one->getRepr()->attribute("fill") ?: ""), "#ff0000");
    EXPECT_EQ(std::string(two->getRepr()->attribute("fill") ?: ""), "#0000ff");
}

TEST_F(GradientPresetTest, RepeatedSelectionReusesImportedCopy) {
    auto one = cast<SPItem>(document->getObjectById("one"));
    ASSERT_TRUE(one);
    desktop->getSelection()->set(one);
    selector->signal_changed().connect([this](SPGradient *gradient) {
        for (auto item : desktop->getSelection()->items_vector())
            sp_item_apply_gradient(item, gradient, desktop.get(), SP_GRADIENT_TYPE_LINEAR, false, FILL);
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Apply preset"}, "color-gradient");
    });
    choose("vac-gold");
    ASSERT_EQ(copies("vac-gold"), 1);
    tree->get_selection()->unselect_all();
    choose("vac-gold");
    EXPECT_EQ(copies("vac-gold"), 1);
}

TEST_F(GradientPresetTest, StrokePreservesFill) {
    auto one = cast<SPItem>(document->getObjectById("one"));
    ASSERT_TRUE(one);
    desktop->getSelection()->set(one);
    selector->signal_changed().connect([this](SPGradient *gradient) {
        for (auto item : desktop->getSelection()->items_vector())
            sp_item_apply_gradient(item, gradient, desktop.get(), SP_GRADIENT_TYPE_LINEAR, false, STROKE);
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Apply stroke preset"}, "color-gradient");
    });
    choose("vac-ocean");
    EXPECT_EQ(copies("vac-ocean"), 1);
    EXPECT_EQ(std::string(one->getRepr()->attribute("fill") ?: ""), "#ff0000");
    EXPECT_NE(std::string(one->getRepr()->attribute("style") ?: "").find("url("), std::string::npos);
}
