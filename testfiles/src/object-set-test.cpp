// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Multiindex container for selection
 *
 * Authors:
 *   Adrian Boguszewski
 *
 * Copyright (C) 2016 Adrian Boguszewski
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#include <gtest/gtest.h>
#include <glib.h>
#include <doc-per-case-test.h>
#include <src/object/sp-factory.h>
#include <src/object/sp-marker.h>
#include <src/object/sp-rect.h>
#include <src/object/sp-path.h>
#include <src/object/sp-use.h>
#include <src/object/sp-root.h>
#include <src/object/sp-image.h>
#include <src/display/cairo-utils.h>
#include <src/object/object-set.h>
#include "ui/dialog/bitmap-copy-dialog.h"
#include <xml/node.h>
#include <src/xml/text-node.h>
#include <src/xml/simple-document.h>
//#include <unistd.h>
#include <2geom/transforms.h>
#include <2geom/rect.h>
#include <src/document.h>
#include <src/object/sp-item.h>
#include <src/object/sp-item-group.h>
#include <src/object/sp-anchor.h>
#include <src/object/sp-defs.h>
#include <src/object/sp-switch.h>
#include <src/document-undo.h>
#include <src/selection.h>
#include <src/preferences.h>
#include <src/ui/toolbar/boolean-assist.h>
#include <xml/repr.h>
using namespace Inkscape;
using namespace Inkscape::XML;
namespace BAT = Inkscape::UI::Toolbar;

static SPItem *bitmap_test_shape(SPDocument *doc, char const *tag, char const *id,
                                 std::initializer_list<std::pair<char const *, char const *>> attributes)
{
    auto *repr = doc->getReprDoc()->createElement(tag);
    repr->setAttribute("id", id);
    for (auto const &[key, value] : attributes) repr->setAttribute(key, value);
    doc->getRoot()->appendChild(repr);
    return cast<SPItem>(doc->getObjectByRepr(repr));
}

static Inkscape::Pixbuf const *bitmap_test_result(ObjectSet *set)
{
    EXPECT_EQ(set->size(), 1);
    auto *image = dynamic_cast<SPImage *>(set->single());
    EXPECT_NE(image, nullptr);
    return image ? image->pixbuf.get() : nullptr;
}

static unsigned bitmap_test_pixel(Inkscape::Pixbuf const *pixbuf, int x, int y)
{
    auto *surface = pixbuf->getSurfaceRaw();
    cairo_surface_flush(surface);
    auto *data = cairo_image_surface_get_data(surface);
    auto stride = cairo_image_surface_get_stride(surface);
    return reinterpret_cast<uint32_t const *>(data + y * stride)[x];
}

class ObjectSetTest: public DocPerCaseTest {
public:
    ObjectSetTest() {
        auto *const _doc = this->_doc.get();
        N = _doc->getRoot()->children.size();

        A = new SPObject();
        B = new SPObject();
        C = new SPObject();
        D = new SPObject();
        E = new SPObject();
        F = new SPObject();
        G = new SPObject();
        H = new SPObject();
        X = new SPObject();
        set = new ObjectSet(_doc);
        set2 = new ObjectSet(_doc);
        auto sd = _doc->getReprDoc();
        auto xt = new TextNode(Util::share_string("x"), sd);
        auto ht = new TextNode(Util::share_string("h"), sd);
        auto gt = new TextNode(Util::share_string("g"), sd);
        auto ft = new TextNode(Util::share_string("f"), sd);
        auto et = new TextNode(Util::share_string("e"), sd);
        auto dt = new TextNode(Util::share_string("d"), sd);
        auto ct = new TextNode(Util::share_string("c"), sd);
        auto bt = new TextNode(Util::share_string("b"), sd);
        auto at = new TextNode(Util::share_string("a"), sd);
        X->invoke_build(_doc, xt, 0);
        H->invoke_build(_doc, ht, 0);
        G->invoke_build(_doc, gt, 0);
        F->invoke_build(_doc, ft, 0);
        E->invoke_build(_doc, et, 0);
        D->invoke_build(_doc, dt, 0);
        C->invoke_build(_doc, ct, 0);
        B->invoke_build(_doc, bt, 0);
        A->invoke_build(_doc, at, 0);
        
        //create 3 rects at root of document
        Inkscape::XML::Node *repr = _doc->getReprDoc()->createElement("svg:rect");
        _doc->getRoot()->appendChild(repr);
        r1.reset(cast<SPRect>(_doc->getObjectByRepr(repr)));
        repr = _doc->getReprDoc()->createElement("svg:rect");
        _doc->getRoot()->appendChild(repr);
        r2.reset(cast<SPRect>(_doc->getObjectByRepr(repr)));
        repr = _doc->getReprDoc()->createElement("svg:rect");
        _doc->getRoot()->appendChild(repr);
        r3.reset(cast<SPRect>(_doc->getObjectByRepr(repr)));
        EXPECT_EQ(N + 3, _doc->getRoot()->children.size());// defs, namedview, and those three rects.
        r1->x = r1->y = r2->x = r2->y = r3->x = r3->y = 0;
        r1->width = r1->height = r2->width = r2->height = r3->width = r3->height = 10;
        r1->set_shape();
        r2->set_shape();
        r3->set_shape();

    }
    ~ObjectSetTest() override {
        delete set;
        delete set2;
        delete X;
        delete H;
        delete G;
        delete F;
        delete E;
        delete D;
        delete C;
        delete B;
        delete A;
    }
    SPObject* A;
    SPObject* B;
    SPObject* C;
    SPObject* D;
    SPObject* E;
    SPObject* F;
    SPObject* G;
    SPObject* H;
    SPObject* X;
    std::unique_ptr<SPRect> r1;
    std::unique_ptr<SPRect> r2;
    std::unique_ptr<SPRect> r3;
    ObjectSet* set;
    ObjectSet* set2;
    int N; //!< Number of root children in default document
};

TEST_F(ObjectSetTest, BitmapCopySizeReadout) {
    using Inkscape::UI::Dialog::bitmap_copy_size;
    using Inkscape::UI::Dialog::bitmap_copy_size_text;
    auto const size = bitmap_copy_size(Geom::Rect(0, 0, 96, 48), 300);
    EXPECT_EQ(size.width, 300);
    EXPECT_EQ(size.height, 150);
    EXPECT_EQ(size.bytes, 180000u);
    auto *formatted = g_format_size(180000);
    EXPECT_EQ(bitmap_copy_size_text(size), Glib::ustring::compose("%1 × %2 px · %3", 300, 150, formatted));
    g_free(formatted);
    auto const rounded = bitmap_copy_size(Geom::Rect(0.4, 0.4, 10.6, 5.2), 96);
    EXPECT_EQ(rounded.width, 11);
    EXPECT_EQ(rounded.height, 6);
    EXPECT_EQ(bitmap_copy_size(Geom::Rect(0, 0, 96, 96), 9000).width, 600);
}

TEST_F(ObjectSetTest, BitmapCopyUsesRequestedResolution) {
    auto *rect = bitmap_test_shape(_doc.get(), "svg:rect", "resolution-orig",
                                   {{"x", "0"}, {"y", "0"}, {"width", "96"}, {"height", "48"}, {"fill", "black"}});
    set->set(rect);
    BitmapCopyOptions options;
    options.dpi = 300;
    set->createBitmapCopy(options);
    auto *pb = bitmap_test_result(set);
    ASSERT_NE(pb, nullptr);
    EXPECT_EQ(pb->width(), 300);
    EXPECT_EQ(pb->height(), 150);
    EXPECT_NE(_doc->getObjectById("resolution-orig"), nullptr);
}

TEST_F(ObjectSetTest, BitmapCopyReplacesOriginalsWhenNotKept) {
    auto *rect = bitmap_test_shape(_doc.get(), "svg:rect", "orig",
                                   {{"x", "0"}, {"y", "0"}, {"width", "96"}, {"height", "48"}, {"fill", "black"}});
    set->set(rect);
    BitmapCopyOptions options;
    options.keep_original = false;
    set->createBitmapCopy(options);
    EXPECT_EQ(_doc->getObjectById("orig"), nullptr);
    ASSERT_NE(bitmap_test_result(set), nullptr);
}

TEST_F(ObjectSetTest, BitmapCopyReplacesAShapeAndItsCloneTogether) {
    auto *rect = bitmap_test_shape(_doc.get(), "svg:rect", "src",
                                   {{"x", "0"}, {"y", "0"}, {"width", "20"}, {"height", "20"}});
    auto *clone = bitmap_test_shape(_doc.get(), "svg:use", "cl",
                                    {{"xlink:href", "#src"}, {"x", "40"}});
    set->add(rect);
    set->add(clone);
    BitmapCopyOptions options;
    options.keep_original = false;
    set->createBitmapCopy(options);
    EXPECT_EQ(_doc->getObjectById("src"), nullptr);
    EXPECT_EQ(_doc->getObjectById("cl"), nullptr);
    ASSERT_EQ(set->size(), 1);
    EXPECT_TRUE(is<SPImage>(set->single()));
    for (auto *child = _doc->getRoot()->getRepr()->firstChild(); child; child = child->next()) {
        if (std::string(child->name()) == "svg:rect") EXPECT_STRNE(child->attribute("id"), "src");
        if (std::string(child->name()) == "svg:use") EXPECT_STRNE(child->attribute("id"), "cl");
    }
}

TEST_F(ObjectSetTest, BitmapCopyReplacesSelectionsAcrossLayers) {
    auto *xml = _doc->getReprDoc();
    for (auto const *id : {"L1", "L2"}) {
        auto *layer = xml->createElement("svg:g");
        layer->setAttribute("id", id);
        layer->setAttribute("inkscape:groupmode", "layer");
        auto *rect = xml->createElement("svg:rect");
        rect->setAttribute("id", id[1] == '1' ? "r1" : "r2");
        rect->setAttribute("width", "20");
        rect->setAttribute("height", "20");
        layer->appendChild(rect);
        _doc->getRoot()->appendChild(layer);
    }
    set->add(_doc->getObjectById("r1"));
    set->add(_doc->getObjectById("r2"));
    DocumentUndo::setUndoSensitive(_doc.get(), true);
    DocumentUndo::done(_doc.get(), Util::Internal::ContextString{"Initialize layer fixture"}, "document-new");
    DocumentUndo::clearUndo(_doc.get());
    BitmapCopyOptions options;
    options.keep_original = false;
    set->createBitmapCopy(options);
    EXPECT_EQ(_doc->getObjectById("r1"), nullptr);
    EXPECT_EQ(_doc->getObjectById("r2"), nullptr);
    ASSERT_EQ(set->size(), 1);
    ASSERT_TRUE(is<SPImage>(set->single()));
    EXPECT_EQ(set->single()->parent, _doc->getObjectById("L2"));
    set->clear();
    r1.release(); r2.release(); r3.release();
    DocumentUndo::undo(_doc.get());
    EXPECT_NE(_doc->getObjectById("r1"), nullptr);
    EXPECT_NE(_doc->getObjectById("r2"), nullptr);
}

TEST_F(ObjectSetTest, BitmapCopyLooksIdenticalInsideASemiTransparentLayer) {
    auto *xml = _doc->getReprDoc();
    auto *layer = xml->createElement("svg:g");
    layer->setAttribute("id", "L1");
    layer->setAttribute("inkscape:groupmode", "layer");
    layer->setAttribute("style", "opacity:0.5");
    auto *rect = xml->createElement("svg:rect");
    rect->setAttribute("id", "r");
    rect->setAttribute("x", "0");
    rect->setAttribute("y", "0");
    rect->setAttribute("width", "20");
    rect->setAttribute("height", "20");
    rect->setAttribute("fill", "black");
    layer->appendChild(rect);
    _doc->getRoot()->appendChild(layer);
    set->set(_doc->getObjectById("r"));
    BitmapCopyOptions options;
    options.dpi = 96;
    options.antialias = true;
    options.transparent = true;
    options.keep_original = false;
    set->createBitmapCopy(options);
    auto *pixbuf = bitmap_test_result(set);
    ASSERT_NE(pixbuf, nullptr);
    auto *image = cast<SPImage>(set->single());
    EXPECT_EQ(image->parent, _doc->getRoot());
    EXPECT_EQ(image->getRepr()->prev(), layer);
    auto const alpha = (bitmap_test_pixel(pixbuf, pixbuf->width() / 2, pixbuf->height() / 2) >> 24) & 0xffu;
    std::cout << "BMP-1 centre alpha=" << alpha << '\n';
    EXPECT_GE(alpha, 118u) << "centre alpha=" << alpha;
    EXPECT_LE(alpha, 138u) << "centre alpha=" << alpha;
}

TEST_F(ObjectSetTest, BitmapCopyStaysInsideAPlainLayer) {
    auto *xml = _doc->getReprDoc();
    auto *layer = xml->createElement("svg:g");
    layer->setAttribute("id", "L2");
    layer->setAttribute("inkscape:groupmode", "layer");
    auto *rect = xml->createElement("svg:rect");
    rect->setAttribute("id", "p");
    rect->setAttribute("x", "0");
    rect->setAttribute("y", "0");
    rect->setAttribute("width", "20");
    rect->setAttribute("height", "20");
    rect->setAttribute("fill", "black");
    layer->appendChild(rect);
    _doc->getRoot()->appendChild(layer);
    set->set(_doc->getObjectById("p"));
    BitmapCopyOptions options;
    options.keep_original = true;
    set->createBitmapCopy(options);
    ASSERT_NE(bitmap_test_result(set), nullptr);
    auto *image = cast<SPImage>(set->single());
    EXPECT_EQ(image->parent, _doc->getObjectByRepr(layer));
    EXPECT_EQ(image->getRepr()->prev(), rect);
}

TEST_F(ObjectSetTest, BitmapCopyStaysInsideABlendModeLayer) {
    // A blend mode is not in the render (everything unselected is hidden, so
    // the backdrop is transparent): the bitmap must stay inside the layer so
    // the blend still applies.
    auto *xml = _doc->getReprDoc();
    auto *layer = xml->createElement("svg:g");
    layer->setAttribute("id", "LB");
    layer->setAttribute("inkscape:groupmode", "layer");
    layer->setAttribute("style", "mix-blend-mode:multiply");
    auto *rect = xml->createElement("svg:rect");
    rect->setAttribute("id", "blend-rect");
    rect->setAttribute("x", "0");
    rect->setAttribute("y", "0");
    rect->setAttribute("width", "20");
    rect->setAttribute("height", "20");
    rect->setAttribute("fill", "black");
    layer->appendChild(rect);
    _doc->getRoot()->appendChild(layer);
    set->set(_doc->getObjectById("blend-rect"));
    BitmapCopyOptions options;
    options.keep_original = true;
    set->createBitmapCopy(options);
    ASSERT_NE(bitmap_test_result(set), nullptr);
    auto *image = cast<SPImage>(set->single());
    EXPECT_EQ(image->parent, _doc->getObjectByRepr(layer));
    EXPECT_EQ(image->getRepr()->prev(), rect);
}

TEST_F(ObjectSetTest, BitmapCopyLeavesAnEffectGroupButStaysInItsLayer) {
    auto *xml = _doc->getReprDoc();
    auto *layer = xml->createElement("svg:g");
    layer->setAttribute("id", "L3");
    layer->setAttribute("inkscape:groupmode", "layer");
    auto *group = xml->createElement("svg:g");
    group->setAttribute("id", "G");
    group->setAttribute("style", "opacity:0.5");
    auto *rect = xml->createElement("svg:rect");
    rect->setAttribute("id", "q");
    rect->setAttribute("x", "0");
    rect->setAttribute("y", "0");
    rect->setAttribute("width", "20");
    rect->setAttribute("height", "20");
    rect->setAttribute("fill", "black");
    group->appendChild(rect);
    layer->appendChild(group);
    _doc->getRoot()->appendChild(layer);
    set->set(_doc->getObjectById("q"));
    BitmapCopyOptions options;
    options.keep_original = true;
    set->createBitmapCopy(options);
    ASSERT_NE(bitmap_test_result(set), nullptr);
    auto *image = cast<SPImage>(set->single());
    EXPECT_EQ(image->parent, _doc->getObjectByRepr(layer));
    EXPECT_EQ(image->getRepr()->prev(), group);
}

TEST_F(ObjectSetTest, BitmapCopyWhiteBackground) {
    auto *circle = bitmap_test_shape(_doc.get(), "svg:circle", "white-circle",
                                     {{"cx", "50"}, {"cy", "50"}, {"r", "50"}, {"fill", "black"}});
    BitmapCopyOptions options;
    options.transparent = false;
    set->set(circle);
    set->createBitmapCopy(options);
    auto *white = bitmap_test_result(set);
    ASSERT_NE(white, nullptr);
    EXPECT_EQ(bitmap_test_pixel(white, 0, 0), 0xffffffffu);
    auto *circle2 = bitmap_test_shape(_doc.get(), "svg:circle", "transparent-circle",
                                      {{"cx", "50"}, {"cy", "50"}, {"r", "50"}, {"fill", "black"}});
    options.transparent = true;
    set->set(circle2);
    set->createBitmapCopy(options);
    auto *clear = bitmap_test_result(set);
    ASSERT_NE(clear, nullptr);
    EXPECT_EQ((bitmap_test_pixel(clear, 0, 0) >> 24) & 0xffu, 0u);
}

TEST_F(ObjectSetTest, BitmapCopyWithoutAntialiasingHasHardEdges) {
    auto *circle = bitmap_test_shape(_doc.get(), "svg:circle", "hard-circle",
                                     {{"cx", "50"}, {"cy", "50"}, {"r", "40"}, {"fill", "black"}});
    BitmapCopyOptions options;
    options.antialias = false;
    set->set(circle);
    set->createBitmapCopy(options);
    auto *hard = bitmap_test_result(set);
    ASSERT_NE(hard, nullptr);
    for (int y = 0; y < hard->height(); ++y)
        for (int x = 0; x < hard->width(); ++x) {
            auto alpha = (bitmap_test_pixel(hard, x, y) >> 24) & 0xffu;
            EXPECT_TRUE(alpha == 0 || alpha == 255);
        }
    auto *circle2 = bitmap_test_shape(_doc.get(), "svg:circle", "soft-circle",
                                      {{"cx", "50"}, {"cy", "50"}, {"r", "40"}, {"fill", "black"}});
    options.antialias = true;
    set->set(circle2);
    set->createBitmapCopy(options);
    auto *soft = bitmap_test_result(set);
    ASSERT_NE(soft, nullptr);
    bool found_soft_edge = false;
    for (int y = 0; y < soft->height(); ++y)
        for (int x = 0; x < soft->width(); ++x) {
            auto alpha = (bitmap_test_pixel(soft, x, y) >> 24) & 0xffu;
            found_soft_edge |= alpha > 0 && alpha < 255;
        }
    EXPECT_TRUE(found_soft_edge);
}

TEST_F(ObjectSetTest, BitmapCopyIsOneUndoStep) {
    auto *rect = bitmap_test_shape(_doc.get(), "svg:rect", "u",
                                   {{"x", "0"}, {"y", "0"}, {"width", "96"}, {"height", "48"}, {"fill", "black"}});
    set->set(rect);
    DocumentUndo::setUndoSensitive(_doc.get(), true);
    DocumentUndo::done(_doc.get(), Util::Internal::ContextString{"Initialize Bitmap Copy fixture"}, "document-new");
    DocumentUndo::clearUndo(_doc.get());
    BitmapCopyOptions options;
    options.keep_original = false;
    set->createBitmapCopy(options);
    ASSERT_NE(bitmap_test_result(set), nullptr);
    auto *image = set->single();
    image->getRepr()->setAttribute("id", "bitmap-undo-result");
    set->clear();
    r1.release(); r2.release(); r3.release();
    DocumentUndo::undo(_doc.get());
    EXPECT_NE(_doc->getObjectById("u"), nullptr);
    EXPECT_EQ(_doc->getObjectById("bitmap-undo-result"), nullptr);
}

TEST_F(ObjectSetTest, BitmapCopyOptionsRoundTripThroughPreferences) {
    BitmapCopyOptions options;
    options.dpi = 200;
    options.antialias = false;
    options.transparent = false;
    options.keep_original = false;
    options.save_to_preferences();
    auto read = BitmapCopyOptions::from_preferences();
    EXPECT_EQ(read.dpi, options.dpi);
    EXPECT_EQ(read.antialias, options.antialias);
    EXPECT_EQ(read.transparent, options.transparent);
    EXPECT_EQ(read.keep_original, options.keep_original);
    options.dpi = 9000;
    options.save_to_preferences();
    EXPECT_EQ(BitmapCopyOptions::from_preferences().dpi, 600);
    BitmapCopyOptions{}.save_to_preferences();
}

TEST_F(ObjectSetTest, BitmapCopyDialogOptionsUseTheirOwnPreferences) {
    auto *prefs = Inkscape::Preferences::get();
    int const previous = prefs->getInt("/options/createbitmap/resolution", 0);
    prefs->setInt("/options/createbitmap/resolution", 150);
    prefs->remove("/options/createbitmap/dialog/resolution");
    prefs->remove("/options/createbitmap/dialog/antialias");
    prefs->remove("/options/createbitmap/dialog/transparent");
    prefs->remove("/options/createbitmap/dialog/keep_original");
    EXPECT_EQ(BitmapCopyOptions::from_preferences().dpi, 150);
    BitmapCopyOptions options;
    options.dpi = 300;
    options.save_to_preferences();
    EXPECT_EQ(prefs->getInt("/options/createbitmap/resolution", 0), 150);
    EXPECT_EQ(BitmapCopyOptions::from_preferences().dpi, 300);
    prefs->setInt("/options/createbitmap/resolution", previous);
    for (auto const *key : {"/options/createbitmap/dialog/resolution", "/options/createbitmap/dialog/antialias",
                            "/options/createbitmap/dialog/transparent", "/options/createbitmap/dialog/keep_original"}) {
        prefs->remove(key);
    }
}

bool containsClone(ObjectSet* set) {
    for (auto it : set->items()) {
        if (is<SPUse>(it)) {
            return true;
        }
        if (is<SPGroup>(it)) {
            ObjectSet tmp_set(set->document());
            std::vector<SPObject*> c = it->childList(false);
            tmp_set.setList(c);
            if (containsClone(&tmp_set)) {
                return true;
            }
        }
    }
    return false;
}

TEST_F(ObjectSetTest, Basics) {
    EXPECT_EQ(0, set->size());
    set->add(A);
    EXPECT_EQ(1, set->size());
    EXPECT_TRUE(set->includes(A));
    EXPECT_TRUE(set->includes(A->getRepr()));
    set->add(B);
    set->add(C);
    EXPECT_EQ(3, set->size());
    EXPECT_TRUE(set->includes(B));
    EXPECT_TRUE(set->includes(C));
    EXPECT_FALSE(set->includes(D));
    EXPECT_FALSE(set->includes(D->getRepr()));
    EXPECT_FALSE(set->includes(X));
    // These calls deliberately exercise the native null-argument guards.
    // Assert their exact diagnostics without disabling fatal unexpected logs.
    g_test_expect_message(nullptr, G_LOG_LEVEL_CRITICAL,
                          "*ObjectSet::includes*assertion 'object != nullptr' failed*");
    EXPECT_FALSE(set->includes((SPObject*)nullptr));
    g_test_assert_expected_messages();
    EXPECT_FALSE(set->includes((Inkscape::XML::Node*)nullptr));
    set->remove(A);
    EXPECT_EQ(2, set->size());
    EXPECT_FALSE(set->includes(A));
    set->clear();
    EXPECT_EQ(0, set->size());
    g_test_expect_message(nullptr, G_LOG_LEVEL_CRITICAL,
                          "*ObjectSet::add*assertion 'object != nullptr' failed*");
    bool resultNull = set->add((SPObject*)nullptr);
    g_test_assert_expected_messages();
    EXPECT_FALSE(resultNull);
    EXPECT_EQ(0, set->size());
    g_test_expect_message(nullptr, G_LOG_LEVEL_CRITICAL,
                          "*ObjectSet::remove*assertion 'object != nullptr' failed*");
    bool resultNull2 = set->remove(nullptr);
    g_test_assert_expected_messages();
    EXPECT_FALSE(resultNull2);
}

TEST_F(ObjectSetTest, Advanced) {
    set->add(A);
    set->add(B);
    set->add(C);
    EXPECT_TRUE(set->includes(C));
    set->toggle(C);
    EXPECT_EQ(2, set->size());
    EXPECT_FALSE(set->includes(C));
    set->toggle(D);
    EXPECT_EQ(3, set->size());
    EXPECT_TRUE(set->includes(D));
    set->toggle(D);
    EXPECT_EQ(2, set->size());
    EXPECT_FALSE(set->includes(D));
    EXPECT_EQ(nullptr, set->single());
    set->set(X);
    EXPECT_EQ(1, set->size());
    EXPECT_TRUE(set->includes(X));
    EXPECT_EQ(X, set->single());
    EXPECT_FALSE(set->isEmpty());
    set->clear();
    EXPECT_TRUE(set->isEmpty());
    std::vector<SPObject*> list1 {A, B, C, D};
    std::vector<SPObject*> list2 {E, F};
    set->addList(list1);
    EXPECT_EQ(4, set->size());
    set->addList(list2);
    EXPECT_EQ(6, set->size());
    EXPECT_TRUE(set->includes(A));
    EXPECT_TRUE(set->includes(B));
    EXPECT_TRUE(set->includes(C));
    EXPECT_TRUE(set->includes(D));
    EXPECT_TRUE(set->includes(E));
    EXPECT_TRUE(set->includes(F));
    set->setList(list2);
    EXPECT_EQ(2, set->size());
    EXPECT_TRUE(set->includes(E));
    EXPECT_TRUE(set->includes(F));
}

TEST_F(ObjectSetTest, Items) {
    // cannot test smallestItem and largestItem functions due to too many dependencies
    // uncomment if the problem is fixed

    SPRect* rect10x100 = &*r1;
    rect10x100->x = rect10x100->x = 0;
    rect10x100->width = 10;
    rect10x100->height = 100;
    rect10x100->set_shape();

    SPRect* rect20x40 = &*r2;
    rect20x40->x = rect20x40->x = 0;
    rect20x40->width = 20;
    rect20x40->height = 40;
    rect20x40->set_shape();

    SPRect* rect30x30 = &*r3;
    rect30x30->x = rect30x30->x = 0;
    rect30x30->width = 30;
    rect30x30->height = 30;
    rect30x30->set_shape();
    
    
    set->add(rect10x100);
    EXPECT_EQ(rect10x100, set->singleItem());
    EXPECT_EQ(rect10x100->getRepr(), set->singleRepr());
    set->add(rect20x40);
    EXPECT_EQ(nullptr, set->singleItem());
    EXPECT_EQ(nullptr, set->singleRepr());
    set->add(rect30x30);
    EXPECT_EQ(3, set->size());
    EXPECT_EQ(rect10x100, set->smallestItem(ObjectSet::CompareSize::HORIZONTAL));
    EXPECT_EQ(rect30x30, set->smallestItem(ObjectSet::CompareSize::VERTICAL));
    EXPECT_EQ(rect20x40, set->smallestItem(ObjectSet::CompareSize::AREA));
    EXPECT_EQ(rect30x30, set->largestItem(ObjectSet::CompareSize::HORIZONTAL));
    EXPECT_EQ(rect10x100, set->largestItem(ObjectSet::CompareSize::VERTICAL));
    EXPECT_EQ(rect10x100, set->largestItem(ObjectSet::CompareSize::AREA));
}

TEST_F(ObjectSetTest, Ranges) {
    std::vector<SPObject*> objs {A, D, B, E, C, F};
    set->add(objs.begin() + 1, objs.end() - 1);
    EXPECT_EQ(4, set->size());
    auto it = set->objects().begin();
    EXPECT_EQ(D, *it++);
    EXPECT_EQ(B, *it++);
    EXPECT_EQ(E, *it++);
    EXPECT_EQ(C, *it++);
    EXPECT_EQ(set->objects().end(), it);
    SPObject* rect1 = SPFactory::createObject("svg:rect");
    SPObject* rect2 = SPFactory::createObject("svg:rect");
    SPObject* rect3 = SPFactory::createObject("svg:rect");
    set->add(rect1);
    set->add(rect2);
    set->add(rect3);
    EXPECT_EQ(7, set->size());
    auto xmlNodes = set->xmlNodes();
    auto xmlNode = xmlNodes.begin();
    EXPECT_EQ(3, std::ranges::distance(set->xmlNodes()));
    EXPECT_EQ(rect1->getRepr(), *xmlNode++);
    EXPECT_EQ(rect2->getRepr(), *xmlNode++);
    EXPECT_EQ(rect3->getRepr(), *xmlNode++);
    EXPECT_EQ(xmlNodes.end(), xmlNode);
    auto items = set->items();
    auto item = items.begin();
    EXPECT_EQ(3, std::ranges::distance(set->items()));
    EXPECT_EQ(rect1, *item++);
    EXPECT_EQ(rect2, *item++);
    EXPECT_EQ(rect3, *item++);
    EXPECT_EQ(items.end(), item);
}

TEST_F(ObjectSetTest, Autoremoving) {
    set->add(A);
    EXPECT_TRUE(set->includes(A));
    EXPECT_EQ(1, set->size());
    A->releaseReferences();
    EXPECT_EQ(0, set->size());
}

TEST_F(ObjectSetTest, BasicDescendants) {
    A->attach(B, nullptr);
    B->attach(C, nullptr);
    A->attach(D, nullptr);
    bool resultB = set->add(B);
    bool resultB2 = set->add(B);
    EXPECT_TRUE(resultB);
    EXPECT_FALSE(resultB2);
    EXPECT_TRUE(set->includes(B));
    bool resultC = set->add(C);
    EXPECT_FALSE(resultC);
    EXPECT_FALSE(set->includes(C));
    EXPECT_EQ(1, set->size());
    bool resultA = set->add(A);
    EXPECT_TRUE(resultA);
    EXPECT_EQ(1, set->size());
    EXPECT_TRUE(set->includes(A));
    EXPECT_FALSE(set->includes(B));
}

TEST_F(ObjectSetTest, AdvancedDescendants) {
    A->attach(B, nullptr);
    A->attach(C, nullptr);
    A->attach(X, nullptr);
    B->attach(D, nullptr);
    B->attach(E, nullptr);
    C->attach(F, nullptr);
    C->attach(G, nullptr);
    C->attach(H, nullptr);
    set->add(A);
    bool resultF = set->remove(F);
    EXPECT_TRUE(resultF);
    EXPECT_EQ(4, set->size());
    EXPECT_FALSE(set->includes(F));
    EXPECT_TRUE(set->includes(B));
    EXPECT_TRUE(set->includes(G));
    EXPECT_TRUE(set->includes(H));
    EXPECT_TRUE(set->includes(X));
    bool resultF2 = set->add(F);
    EXPECT_TRUE(resultF2);
    EXPECT_EQ(5, set->size());
    EXPECT_TRUE(set->includes(F));
}

TEST_F(ObjectSetTest, Removing) {
    A->attach(B, nullptr);
    A->attach(C, nullptr);
    A->attach(X, nullptr);
    B->attach(D, nullptr);
    B->attach(E, nullptr);
    C->attach(F, nullptr);
    C->attach(G, nullptr);
    C->attach(H, nullptr);
    bool removeH = set->remove(H);
    EXPECT_FALSE(removeH);
    set->add(A);
    bool removeX = set->remove(X);
    EXPECT_TRUE(removeX);
    EXPECT_EQ(2, set->size());
    EXPECT_TRUE(set->includes(B));
    EXPECT_TRUE(set->includes(C));
    EXPECT_FALSE(set->includes(X));
    EXPECT_FALSE(set->includes(A));
    bool removeX2 = set->remove(X);
    EXPECT_FALSE(removeX2);
    EXPECT_EQ(2, set->size());
    bool removeA = set->remove(A);
    EXPECT_FALSE(removeA);
    EXPECT_EQ(2, set->size());
    bool removeC = set->remove(C);
    EXPECT_TRUE(removeC);
    EXPECT_EQ(1, set->size());
    EXPECT_TRUE(set->includes(B));
    EXPECT_FALSE(set->includes(C));
}

TEST_F(ObjectSetTest, TwoSets) {
    A->attach(B, nullptr);
    A->attach(C, nullptr);
    set->add(A);
    set2->add(A);
    EXPECT_EQ(1, set->size());
    EXPECT_EQ(1, set2->size());
    set->remove(B);
    EXPECT_EQ(1, set->size());
    EXPECT_TRUE(set->includes(C));
    EXPECT_EQ(1, set2->size());
    EXPECT_TRUE(set2->includes(A));
    C->releaseReferences();
    EXPECT_EQ(0, set->size());
    EXPECT_EQ(1, set2->size());
    EXPECT_TRUE(set2->includes(A));
}

TEST_F(ObjectSetTest, SetRemoving) {
    ObjectSet *objectSet = new ObjectSet(_doc.get());
    A->attach(B, nullptr);
    objectSet->add(A);
    objectSet->add(C);
    EXPECT_EQ(2, objectSet->size());
    delete objectSet;
    EXPECT_STREQ(nullptr, A->getId());
    EXPECT_STREQ(nullptr, C->getId());
}

TEST_F(ObjectSetTest, Delete) {
    //we cannot use the same item as in other tests since it will be freed at the test destructor

    EXPECT_EQ(_doc->getRoot(), r1->parent);
    set->add(r1.get());
    set->deleteItems();
    r1.release();
    EXPECT_EQ(0, set->size());
    //EXPECT_EQ(nullptr, r1->parent);
}

TEST_F(ObjectSetTest, Ops) {
    set->add(r1.get());
    set->add(r2.get());
    set->add(r3.get());
    set->duplicate();
    EXPECT_EQ(N + 6, _doc->getRoot()->children.size());// defs, namedview, and those 3x2 rects.
    EXPECT_EQ(3, set->size());
    EXPECT_FALSE(set->includes(r1.get()));
    set->deleteItems();
    EXPECT_TRUE(set->isEmpty());
    set->add(r1.get());
    set->add(r2.get());
    set->add(r3.get());
    set->group();//r1-3 are now invalid (grouping makes copies)
    r1.release();
    r2.release();
    r3.release();
    EXPECT_EQ(N + 1, _doc->getRoot()->children.size());
    EXPECT_EQ(1, set->size());
    set->ungroup();
    EXPECT_EQ(N + 3, _doc->getRoot()->children.size());
    EXPECT_EQ(3, set->size());
    /* Uncomment this when toNextLayer is made desktop-independent
    set->group();
    set2->add(set->singleItem()->childList(false)[0]);
    EXPECT_EQ(3, set->singleItem()->children.size());
    EXPECT_EQ(4, _doc->getRoot()->children.size());
    set2->popFromGroup();
    EXPECT_EQ(2, set->singleItem()->children.size());
    EXPECT_EQ(5, _doc->getRoot()->children.size());
    set->ungroup();
    set->add(set2->singleItem());
    */
    set->clone();
    EXPECT_EQ(N + 6, _doc->getRoot()->children.size());
    EXPECT_EQ(3, set->size());
    EXPECT_TRUE(is<SPUse>(set->items().front()));
    EXPECT_FALSE(is<SPRect>(set->items().front()));
    set->unlink();
    EXPECT_EQ(N + 6, _doc->getRoot()->children.size());
    EXPECT_EQ(3, set->size());
    EXPECT_FALSE(is<SPUse>(set->items().front()));
    EXPECT_TRUE(is<SPRect>(set->items().front()));
    set->clone(); //creates 3 clones
    set->clone(); //creates 3 clones of clones
    EXPECT_EQ(N + 12, _doc->getRoot()->children.size());
    EXPECT_EQ(3, set->size());
    EXPECT_TRUE(is<SPUse>(cast_unsafe<SPUse>(set->items().front())->get_original())); // original is a Use
    set->unlink(); //clone of clone of rect -> rect
    EXPECT_FALSE(is<SPUse>(set->items().front()));
    EXPECT_TRUE(is<SPRect>(set->items().front()));
    set->clone();
    set->set(set->items().front());
    set->cloneOriginal();//get clone original
    EXPECT_EQ(N + 15, _doc->getRoot()->children.size());
    EXPECT_EQ(1, set->size());
    EXPECT_TRUE(is<SPRect>(set->items().front()));
    //let's stop here.
    // TODO: write a hundred more tests to check clone (non-)displacement when grouping, ungrouping and unlinking...
    TearDownTestCase();
    SetUpTestCase();
}

TEST_F(ObjectSetTest, unlinkRecursiveBasic) {
    // This is the same as the test (ObjectSetTest, Ops), but with unlinkRecursive instead of unlink.
    set->set(r1.get());
    set->add(r2.get());
    set->add(r3.get());
    EXPECT_FALSE(containsClone(set));
    set->duplicate();
    EXPECT_FALSE(containsClone(set));
    EXPECT_EQ(N + 6, _doc->getRoot()->children.size());// defs, namedview, and those 3x2 rects.
    EXPECT_EQ(3, set->size());
    EXPECT_FALSE(set->includes(r1.get()));
    set->deleteItems();
    EXPECT_FALSE(containsClone(set));
    EXPECT_TRUE(set->isEmpty());
    set->add(r1.get());
    set->add(r2.get());
    set->add(r3.get());
    EXPECT_FALSE(containsClone(set));
    set->group();//r1-3 are now invalid (grouping makes copies)
    r1.release();
    r2.release();
    r3.release();
    EXPECT_FALSE(containsClone(set));
    EXPECT_EQ(N + 1, _doc->getRoot()->children.size());
    EXPECT_EQ(1, set->size());
    set->ungroup();
    EXPECT_FALSE(containsClone(set));
    EXPECT_EQ(N + 3, _doc->getRoot()->children.size());
    EXPECT_EQ(3, set->size());
    /* Uncomment this when toNextLayer is made desktop-independent
    set->group();
    set2->add(set->singleItem()->childList(false)[0]);
    EXPECT_EQ(3, set->singleItem()->children.size());
    EXPECT_EQ(4, _doc->getRoot()->children.size());
    set2->popFromGroup();
    EXPECT_EQ(2, set->singleItem()->children.size());
    EXPECT_EQ(5, _doc->getRoot()->children.size());
    set->ungroup();
    set->add(set2->singleItem());
    */
    set->clone();
    EXPECT_TRUE(containsClone(set));
    EXPECT_EQ(N + 6, _doc->getRoot()->children.size());
    EXPECT_EQ(3, set->size());
    EXPECT_TRUE(is<SPUse>(set->items().front()));
    EXPECT_FALSE(is<SPRect>(set->items().front()));
    set->unlinkRecursive(false, true);
    EXPECT_FALSE(containsClone(set));
    EXPECT_EQ(N + 6, _doc->getRoot()->children.size());
    EXPECT_EQ(3, set->size());
    EXPECT_FALSE(is<SPUse>(set->items().front()));
    EXPECT_TRUE(is<SPRect>(set->items().front()));
    set->clone(); //creates 3 clones
    EXPECT_TRUE(containsClone(set));
    set->clone(); //creates 3 clones of clones
    EXPECT_TRUE(containsClone(set));
    EXPECT_EQ(N + 12, _doc->getRoot()->children.size());
    EXPECT_EQ(3, set->size());
    EXPECT_TRUE(is<SPUse>(cast_unsafe<SPUse>(set->items().front())->get_original())); // original is a Use
    set->unlinkRecursive(false, true); //clone of clone of rect -> rect
    EXPECT_FALSE(containsClone(set));
    EXPECT_FALSE(is<SPUse>(set->items().front()));
    EXPECT_TRUE(is<SPRect>(set->items().front()));
    set->clone();
    EXPECT_TRUE(containsClone(set));
    set->set(set->items().front());
    set->cloneOriginal();//get clone original
    EXPECT_EQ(N + 15, _doc->getRoot()->children.size());
    EXPECT_EQ(1, set->size());
    EXPECT_TRUE(is<SPRect>(set->items().front()));
    TearDownTestCase();
    SetUpTestCase();
}

TEST_F(ObjectSetTest, unlinkRecursiveAdvanced) {
    set->set(r1.get());
    set->add(r2.get());
    set->add(r3.get());
    set->group();//r1-3 are now invalid (grouping makes copies)
    r1.release();
    r2.release();
    r3.release();
    EXPECT_FALSE(containsClone(set));
    EXPECT_EQ(1, set->size());
    SPItem* original = set->singleItem();
    set->clone();
    EXPECT_TRUE(containsClone(set));
    EXPECT_EQ(1, set->size());
    set->add(original);
    EXPECT_TRUE(containsClone(set));
    EXPECT_EQ(2, set->size());
    set->group();
    EXPECT_TRUE(containsClone(set));
    EXPECT_EQ(1, set->size());
    original = set->singleItem();
    set->clone();
    EXPECT_TRUE(containsClone(set));
    EXPECT_EQ(1, set->size());
    set->add(original);
    EXPECT_TRUE(containsClone(set));
    EXPECT_EQ(2, set->size());
    set->group();
    EXPECT_TRUE(containsClone(set));
    EXPECT_EQ(1, set->size());
    original = set->singleItem();
    set->clone();
    EXPECT_TRUE(containsClone(set));
    EXPECT_EQ(1, set->size());
    set->add(original);
    EXPECT_TRUE(containsClone(set));
    EXPECT_EQ(2, set->size());
    set->unlinkRecursive(false, true);
    EXPECT_FALSE(containsClone(set));
    EXPECT_EQ(2, set->size());

    TearDownTestCase();
    SetUpTestCase();
}

TEST_F(ObjectSetTest, ZOrder) {
    //sp_object_compare_position_bool == true iff "r1<r2" iff r1 is "before" r2 in the file, ie r1 is lower than r2
    EXPECT_TRUE(sp_object_compare_position_bool(r1.get(),r2.get()));
    EXPECT_TRUE(sp_object_compare_position_bool(r2.get(),r3.get()));
    EXPECT_TRUE(sp_object_compare_position_bool(r1.get(),r3.get()));
    EXPECT_FALSE(sp_object_compare_position_bool(r2.get(),r1.get()));
    EXPECT_FALSE(sp_object_compare_position_bool(r3.get(),r1.get()));
    EXPECT_FALSE(sp_object_compare_position_bool(r3.get(),r2.get()));
    //1 2 3
    set->set(r2.get());
    set->raise();
    //1 3 2
    EXPECT_TRUE(sp_object_compare_position_bool(r1.get(),r3.get()));
    EXPECT_TRUE(sp_object_compare_position_bool(r3.get(),r2.get()));//!
    set->set(r3.get());
    set->lower();
    //3 1 2
    EXPECT_TRUE(sp_object_compare_position_bool(r3.get(),r1.get()));
    EXPECT_TRUE(sp_object_compare_position_bool(r1.get(),r2.get()));
    set->raiseToTop();
    //1 2 3
    EXPECT_TRUE(sp_object_compare_position_bool(r1.get(),r2.get()));
    EXPECT_TRUE(sp_object_compare_position_bool(r2.get(),r3.get()));
    set->lowerToBottom();
    //3 1 2
    EXPECT_TRUE(sp_object_compare_position_bool(r3.get(),r1.get()));
    EXPECT_TRUE(sp_object_compare_position_bool(r1.get(),r2.get()));
}

TEST_F(ObjectSetTest, Combine) {
    set->add(r1.get());
    set->add(r2.get());
    set->combine();
    r1.release();
    r2.release();
    EXPECT_EQ(1, set->size());
    EXPECT_EQ(N + 2, _doc->getRoot()->children.size());
    set->breakApart();
    EXPECT_EQ(2, set->size());
    EXPECT_EQ(N + 3, _doc->getRoot()->children.size());
    set->deleteItems();
    set->set(r3.get());
    set->toCurves();
    r3.release();
    auto x = set->singleItem();
    EXPECT_TRUE(is<SPPath>(x));
    EXPECT_FALSE(is<SPRect>(x));
    set->deleteItems();
}

TEST_F(ObjectSetTest, Moves) {
    set->add(r1.get());
    set->moveRelative(15,15);
    EXPECT_EQ(15,r1->x.value);
    Geom::Point p(20,20);
    Geom::Scale s(2);
    set->scaleRelative(p,s);
    EXPECT_EQ(10,r1->x.value);
    EXPECT_EQ(20,r1->width.value);
    set->toCurves();
    r1.release();
    auto x = set->singleItem();
    EXPECT_EQ(20,(*(x->documentVisualBounds()))[0].extent());
    set->rotateRelative(*set->center(), 180);
    EXPECT_EQ(20,(*(x->documentVisualBounds()))[0].extent());
    set->deleteItems();
}

TEST_F(ObjectSetTest, toMarker) {
    r1->x = 12;
    r1->y = 34;
    r1->width = 56;
    r1->height = 78;
    r1->set_shape();
    r1->updateRepr();

    r2->x = 6;
    r2->y = 7;
    r2->width = 8;
    r2->height = 9;
    r2->set_shape();
    r2->updateRepr();

    r3->x = 10;
    r3->y = 10;
    r3->width = 10;
    r3->height = 10;
    r3->set_shape();
    r3->updateRepr();

    // add rects to set in different order than they appear in the document,
    // to verify selection order independence.
    set->set(r1.get());
    set->add(r3.get());
    set->add(r2.get());
    set->toMarker();

    // original items got deleted
    r1.release();
    r2.release();
    r3.release();

    auto markers = _doc->getObjectsByElement("marker");
    ASSERT_EQ(markers.size(), 1);

    auto marker = cast<SPMarker>(markers[0]);
    ASSERT_NE(marker, nullptr);

    EXPECT_FLOAT_EQ(marker->refX.computed, 31);
    EXPECT_FLOAT_EQ(marker->refY.computed, 52.5);
    EXPECT_FLOAT_EQ(marker->markerWidth.computed, 62);
    EXPECT_FLOAT_EQ(marker->markerHeight.computed, 105);

    auto markerchildren = marker->childList(false);
    ASSERT_EQ(markerchildren.size(), 3);

    auto *markerrect1 = cast<SPRect>(markerchildren[0]);
    auto *markerrect2 = cast<SPRect>(markerchildren[1]);

    ASSERT_NE(markerrect1, nullptr);
    ASSERT_NE(markerrect2, nullptr);

    EXPECT_FLOAT_EQ(markerrect1->x.value, 6);
    EXPECT_FLOAT_EQ(markerrect1->y.value, 27);
    EXPECT_FLOAT_EQ(markerrect1->width.value, 56);
    EXPECT_FLOAT_EQ(markerrect1->height.value, 78);

    EXPECT_FLOAT_EQ(markerrect2->x.value, 0);
    EXPECT_FLOAT_EQ(markerrect2->y.value, 0);
    EXPECT_FLOAT_EQ(markerrect2->width.value, 8);
    EXPECT_FLOAT_EQ(markerrect2->height.value, 9);
}

// ---------------------------------------------------------------------------
// Boolean Assistant: groups as one combined shape.
//
// These exercise the shared GUI/CLI entry points in ui/toolbar/boolean-assist
// (boolean_assist_leaves / apply_boolean_assist) directly, on their own
// documents so the fixture document state cannot leak in.
// ---------------------------------------------------------------------------

namespace {

std::unique_ptr<SPDocument> make_boolean_doc(std::string const &svg)
{
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(svg.data(), svg.size()));
    if (doc) {
        doc->ensureUpToDate();
    }
    return doc;
}

bool visible_and_unlocked(SPItem *item)
{
    return item->isVisibleAndUnlocked();
}

SPItem *item_by_id(SPDocument *doc, char const *id)
{
    return cast<SPItem>(doc->getObjectById(id));
}

} // namespace

// Union of a rect and a transformed group (with a nested transformed group): the
// group is one operand whose silhouette spans both leaves; the emptied groups are
// removed and an unselected sibling group is left alone.
TEST_F(ObjectSetTest, BooleanAssistUnionTransformedGroup)
{
    constexpr char const *svg = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>
  <rect id='R' x='0' y='0' width='10' height='10'/>
  <g id='G' transform='translate(10,5) scale(2)'>
    <rect id='GA' x='0' y='0' width='10' height='10'/>
    <g id='GIN' transform='translate(3,0)'>
      <rect id='GB' x='0' y='0' width='10' height='10'/>
    </g>
  </g>
  <g id='S'><rect id='SR' x='70' y='70' width='10' height='10'/></g>
</svg>)SVG";
    auto doc = make_boolean_doc(svg);
    ASSERT_TRUE(doc);

    auto *R = item_by_id(doc.get(), "R");
    auto *G = item_by_id(doc.get(), "G");
    ASSERT_NE(R, nullptr);
    ASSERT_NE(G, nullptr);
    ASSERT_TRUE(is<SPGroup>(G));

    ObjectSet set(doc.get());
    set.setList(std::vector<SPItem *>{R, G});
    SPItem *result = BAT::apply_boolean_assist(set, BAT::BooleanAssistOp::Union);
    ASSERT_NE(result, nullptr);
    ASSERT_TRUE(is<SPPath>(result));

    // R = [0,10]x[0,10]; GA = [10,30]x[5,25]; GB = [16,36]x[5,25] -> union [0,36]x[0,25].
    auto bounds = result->documentVisualBounds();
    ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->left(), 0.0, 1e-6);
    EXPECT_NEAR(bounds->top(), 0.0, 1e-6);
    EXPECT_NEAR(bounds->width(), 36.0, 1e-6);
    EXPECT_NEAR(bounds->height(), 25.0, 1e-6);

    // Emptied selected groups (root and nested) are removed ...
    EXPECT_EQ(nullptr, doc->getObjectById("G"));
    EXPECT_EQ(nullptr, doc->getObjectById("GIN"));
    // ... but an unselected sibling group elsewhere is untouched.
    EXPECT_NE(nullptr, doc->getObjectById("S"));
    EXPECT_NE(nullptr, doc->getObjectById("SR"));
}

// A selection with no group root runs no cleanup: the parent group of a selected
// child survives even though the union emptied it.
TEST_F(ObjectSetTest, BooleanAssistNoGroupSelectionKeepsParent)
{
    constexpr char const *svg = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>
  <rect id='R' x='0' y='0' width='10' height='10'/>
  <g id='P'><rect id='C' x='20' y='0' width='10' height='10'/></g>
</svg>)SVG";
    auto doc = make_boolean_doc(svg);
    ASSERT_TRUE(doc);

    auto *R = item_by_id(doc.get(), "R");
    auto *C = item_by_id(doc.get(), "C");
    ASSERT_NE(R, nullptr);
    ASSERT_NE(C, nullptr);

    ObjectSet set(doc.get());
    set.setList(std::vector<SPItem *>{C, R});
    SPItem *result = BAT::apply_boolean_assist(set, BAT::BooleanAssistOp::Union);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(is<SPPath>(result));

    // No selected root is a group, so candidates are empty and nothing is swept.
    EXPECT_NE(nullptr, doc->getObjectById("P"));
}

// A selected group carrying a clip is refused with reason "group-effect"; neither
// the group nor its leaf is changed.
TEST_F(ObjectSetTest, BooleanAssistRefusesGroupWithClip)
{
    constexpr char const *svg = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>
  <defs><clipPath id='cp'><rect x='0' y='0' width='5' height='5'/></clipPath></defs>
  <rect id='R' x='0' y='0' width='10' height='10'/>
  <g id='GC' clip-path='url(#cp)'><rect id='GCA' x='0' y='0' width='10' height='10'/></g>
</svg>)SVG";
    auto doc = make_boolean_doc(svg);
    ASSERT_TRUE(doc);

    auto *R = item_by_id(doc.get(), "R");
    auto *GC = item_by_id(doc.get(), "GC");
    ASSERT_NE(R, nullptr);
    ASSERT_NE(GC, nullptr);
    ASSERT_NE(GC->getClipObject(), nullptr);

    std::vector<SPItem *> leaves;
    SPItem *offending = nullptr;
    std::string reason;
    EXPECT_FALSE(BAT::boolean_assist_leaves(GC, visible_and_unlocked, leaves, &offending, &reason));
    EXPECT_EQ(offending, GC);
    EXPECT_EQ(reason, "group-effect");

    ObjectSet set(doc.get());
    set.setList(std::vector<SPItem *>{R, GC});
    EXPECT_EQ(nullptr, BAT::apply_boolean_assist(set, BAT::BooleanAssistOp::Union));
    EXPECT_NE(nullptr, doc->getObjectById("GC"));
    EXPECT_NE(nullptr, doc->getObjectById("GCA"));
}

// Each operation over one group root and one rect leaves a single path with the
// bounds of its own semantics. R = [0,10]x[0,10]; GPR = [5,15]x[5,15]:
//   BottomMinusRest keeps the bottom rect -> [0,10]x[0,10]
//   TopMinusRest    keeps the top group    -> [5,15]x[5,15]
//   Intersection    keeps the overlap      -> [5,10]x[5,10]
//   Exclusion       spans the two          -> [0,15]x[0,15]
// The distinct bounds fail if the two differences are swapped or an operation
// reports another operation's result.
TEST_F(ObjectSetTest, BooleanAssistGroupOperations)
{
    constexpr char const *svg = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>
  <rect id='R' x='0' y='0' width='10' height='10'/>
  <g id='GP'><rect id='GPR' x='5' y='5' width='10' height='10'/></g>
</svg>)SVG";
    for (auto op : {BAT::BooleanAssistOp::BottomMinusRest, BAT::BooleanAssistOp::TopMinusRest,
                    BAT::BooleanAssistOp::Intersection, BAT::BooleanAssistOp::Exclusion}) {
        SCOPED_TRACE(static_cast<int>(op));
        auto doc = make_boolean_doc(svg);
        ASSERT_TRUE(doc);

        auto *R = item_by_id(doc.get(), "R");
        auto *GP = item_by_id(doc.get(), "GP");
        ASSERT_NE(R, nullptr);
        ASSERT_NE(GP, nullptr);

        ObjectSet set(doc.get());
        set.setList(std::vector<SPItem *>{R, GP});
        SPItem *result = BAT::apply_boolean_assist(set, op);
        ASSERT_NE(result, nullptr);
        ASSERT_TRUE(is<SPPath>(result));

        double left = 0.0;
        double top = 0.0;
        double width = 0.0;
        double height = 0.0;
        if (op == BAT::BooleanAssistOp::BottomMinusRest) {
            left = 0.0;
            top = 0.0;
            width = 10.0;
            height = 10.0;
        } else if (op == BAT::BooleanAssistOp::TopMinusRest) {
            left = 5.0;
            top = 5.0;
            width = 10.0;
            height = 10.0;
        } else if (op == BAT::BooleanAssistOp::Intersection) {
            left = 5.0;
            top = 5.0;
            width = 5.0;
            height = 5.0;
        } else if (op == BAT::BooleanAssistOp::Exclusion) {
            left = 0.0;
            top = 0.0;
            width = 15.0;
            height = 15.0;
        }

        auto bounds = result->documentVisualBounds();
        ASSERT_TRUE(bounds);
        EXPECT_NEAR(bounds->left(), left, 1e-6);
        EXPECT_NEAR(bounds->top(), top, 1e-6);
        EXPECT_NEAR(bounds->width(), width, 1e-6);
        EXPECT_NEAR(bounds->height(), height, 1e-6);
    }
}

// A group-root selection whose operand is a child of an unselected group P: the
// operation empties P, but P is not a cleanup candidate, so it survives, while the
// selected group root G (also emptied) is removed.
TEST_F(ObjectSetTest, BooleanAssistGroupRootSelectionKeepsUnselectedParent)
{
    constexpr char const *svg = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>
  <rect id='R' x='0' y='0' width='10' height='10'/>
  <g id='P'><rect id='C' x='20' y='0' width='10' height='10'/></g>
  <g id='G'><rect id='GA' x='40' y='0' width='10' height='10'/></g>
</svg>)SVG";
    auto doc = make_boolean_doc(svg);
    ASSERT_TRUE(doc);

    auto *R = item_by_id(doc.get(), "R");
    auto *C = item_by_id(doc.get(), "C");
    auto *G = item_by_id(doc.get(), "G");
    ASSERT_NE(R, nullptr);
    ASSERT_NE(C, nullptr);
    ASSERT_NE(G, nullptr);
    ASSERT_TRUE(is<SPGroup>(G));

    ObjectSet set(doc.get());
    set.setList(std::vector<SPItem *>{R, C, G});
    SPItem *result = BAT::apply_boolean_assist(set, BAT::BooleanAssistOp::Union);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(is<SPPath>(result));

    // G was a selected group root and lost its only leaf -> removed with the leaf.
    EXPECT_EQ(nullptr, doc->getObjectById("G"));
    EXPECT_EQ(nullptr, doc->getObjectById("GA"));
    // P was emptied (its child C was an operand) but is not a candidate -> it survives.
    EXPECT_EQ(nullptr, doc->getObjectById("C"));
    EXPECT_NE(nullptr, doc->getObjectById("P"));
}

// AC-6g FIX 1: a boolean result must be a freshly created object, not one of the
// operands. The operand XML nodes are anchored for the operation, so their
// addresses cannot be reused by the new result path.
TEST_F(ObjectSetTest, BooleanResultIsNewRejectsOperand)
{
    constexpr char const *svg = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>
  <rect id='a' x='0' y='0' width='10' height='10'/>
  <rect id='b' x='5' y='5' width='10' height='10'/>
</svg>)SVG";
    auto doc = make_boolean_doc(svg);
    ASSERT_TRUE(doc);

    auto *a = item_by_id(doc.get(), "a");
    auto *b = item_by_id(doc.get(), "b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    // Capture the operand nodes before the operation deletes them.
    auto *const a_repr = a->getRepr();
    auto *const b_repr = b->getRepr();
    ASSERT_NE(a_repr, nullptr);
    ASSERT_NE(b_repr, nullptr);

    // A null result is never new.
    EXPECT_FALSE(BAT::boolean_result_is_new(nullptr, {}));

    ObjectSet set(doc.get());
    set.setList(std::vector<SPItem *>{a, b});
    SPItem *result = BAT::apply_boolean_assist(set, BAT::BooleanAssistOp::Union);
    ASSERT_NE(result, nullptr);
    ASSERT_TRUE(is<SPPath>(result));

    auto *const result_repr = result->getRepr();
    ASSERT_NE(result_repr, nullptr);
    // The new result is none of the operands (anchoring kept their addresses distinct).
    EXPECT_TRUE(BAT::boolean_result_is_new(result, {}));
    EXPECT_TRUE(BAT::boolean_result_is_new(result, {a_repr, b_repr}));
    // The helper rejects the result's own repr.
    EXPECT_FALSE(BAT::boolean_result_is_new(result, {result_repr}));
}

// U2: a Boolean Assist session keeps raw SPItem pointers (operands and group leaves). Undo can delete a member
// of a previewed group without touching the selection, so the session must learn of the release while the member
// is still valid and drop every pointer before the next preview dereferences it.
TEST_F(ObjectSetTest, BooleanAssistSessionDropsLeavesWhenUndoDeletesAMember)
{
    constexpr char const *svg = R"SVG(
<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>
  <rect id='R' x='0' y='0' width='10' height='10'/>
  <g id='G'><rect id='GA' x='0' y='0' width='10' height='10'/></g>
</svg>)SVG";
    auto doc = make_boolean_doc(svg);
    ASSERT_TRUE(doc);
    DocumentUndo::clearUndo(doc.get());
    auto *G = item_by_id(doc.get(), "G");
    ASSERT_TRUE(is<SPGroup>(G));

    // The user adds a member to the group (one Undo step) ...
    auto *member = doc->getReprDoc()->createElement("svg:rect");
    member->setAttribute("id", "GNEW");
    member->setAttribute("width", "10");
    member->setAttribute("height", "10");
    G->getRepr()->appendChild(member);
    Inkscape::GC::release(member);
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"Add member"}, "");
    doc->ensureUpToDate();

    // ... and opens the assistant on the group: operands and leaves are recorded.
    std::vector<SPItem *> leaves;
    SPItem *offending = nullptr;
    ASSERT_TRUE(BAT::boolean_assist_leaves(G, visible_and_unlocked, leaves, &offending));
    ASSERT_EQ(leaves.size(), 2u);
    std::vector<SPItem *> session = leaves;
    session.push_back(G);
    std::vector<std::string> released_ids;
    BAT::ItemReleaseWatch watch([&](SPItem *item) {
        released_ids.emplace_back(item->getId()); // the item must still be valid here
        session.clear();                          // the session forgets every raw pointer
    });
    for (auto *item : session) {
        watch.watch(item);
    }
    EXPECT_EQ(watch.size(), 3u);
    EXPECT_FALSE(watch.released());

    // Undo deletes the new member; the selection is untouched.
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(doc->getObjectById("GNEW"), nullptr);
    ASSERT_EQ(released_ids.size(), 1u);
    EXPECT_EQ(released_ids[0], "GNEW");
    EXPECT_TRUE(watch.released());
    EXPECT_TRUE(session.empty()) << "a stale operand leaf is still reachable";
    EXPECT_EQ(watch.size(), 0u);

    // Redo creates a new object; a fresh session is built from live items only.
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    watch.clear();
    EXPECT_FALSE(watch.released());
    leaves.clear();
    ASSERT_TRUE(BAT::boolean_assist_leaves(item_by_id(doc.get(), "G"), visible_and_unlocked, leaves, &offending));
    EXPECT_EQ(leaves.size(), 2u);
    bool called = false;
    BAT::ItemReleaseWatch quiet([&](SPItem *) { called = true; });
    quiet.watch(leaves[0]);
    quiet.clear(); // a session that ends normally must not fire the callback later
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(called);
}

namespace {
std::string history_selection_xml(SPDocument *document)
{
    return sp_repr_save_buf(document->getReprDoc()).raw();
}

std::unique_ptr<SPDocument> history_selection_doc(std::string const &inner)
{
    auto svg = "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>" + inner + "</svg>";
    return make_boolean_doc(svg);
}
} // namespace

TEST_F(ObjectSetTest, HistoryReplayUngroupRemovesSelectionInOneChange)
{
    std::string children;
    for (int i = 0; i < 200; ++i) {
        children += "<path id='batch-path-" + std::to_string(i) + "' d='M0 0h1v1z'/>";
    }
    auto doc = history_selection_doc("<g id='batch-group'>" + children + "</g>");
    ASSERT_TRUE(doc);
    auto selection = doc->getSelection();
    selection->set(cast<SPItem>(doc->getObjectById("batch-group")));
    DocumentUndo::clearUndo(doc.get());
    auto const before = history_selection_xml(doc.get());
    selection->ungroup();
    auto const after = history_selection_xml(doc.get());
    ASSERT_NE(before, after);

    int changes = 0;
    auto connection = selection->connectChanged([&](auto) { ++changes; });
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    connection.disconnect();
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(selection->isEmpty());
    EXPECT_EQ(history_selection_xml(doc.get()), before);
}

TEST_F(ObjectSetTest, HistoryReplayUndoMoveKeepsSelectionWithoutRemovalChange)
{
    auto doc = history_selection_doc("<rect id='move-target' width='10' height='10'/>");
    ASSERT_TRUE(doc);
    auto selection = doc->getSelection();
    auto target = cast<SPItem>(doc->getObjectById("move-target"));
    selection->set(target);
    DocumentUndo::clearUndo(doc.get());
    auto const before = history_selection_xml(doc.get());
    selection->move(5, 0);
    ASSERT_NE(history_selection_xml(doc.get()), before);

    int changes = 0;
    auto connection = selection->connectChanged([&](auto) { ++changes; });
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    connection.disconnect();
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(selection->size(), 1u);
    EXPECT_EQ(selection->single(), target);
    EXPECT_EQ(history_selection_xml(doc.get()), before);
}

TEST_F(ObjectSetTest, HistoryReplayUndoDuplicateRemovesSelectedDescendant)
{
    auto doc = history_selection_doc("<g id='source'><path id='source-path' d='M0 0h1v1z'/></g>");
    ASSERT_TRUE(doc);
    DocumentUndo::clearUndo(doc.get());
    auto const before = history_selection_xml(doc.get());
    auto duplicate = doc->getReprDoc()->createElement("svg:g");
    duplicate->setAttribute("id", "duplicate");
    auto path = doc->getReprDoc()->createElement("svg:path");
    path->setAttribute("id", "duplicate-path");
    path->setAttribute("d", "M0 0h1v1z");
    duplicate->appendChild(path);
    doc->getRoot()->getRepr()->appendChild(duplicate);
    Inkscape::GC::release(path);
    Inkscape::GC::release(duplicate);
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"Duplicate group"}, "");
    auto selection = doc->getSelection();
    selection->set(cast<SPItem>(doc->getObjectById("duplicate-path")));

    int changes = 0;
    auto connection = selection->connectChanged([&](auto) { ++changes; });
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    connection.disconnect();
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(selection->isEmpty());
    EXPECT_EQ(history_selection_xml(doc.get()), before);
}

TEST_F(ObjectSetTest, HistoryReplayRedoUngroupRemovesRestoredGroupsInOneChange)
{
    auto doc = history_selection_doc(
        "<g id='redo-group-a'><path id='redo-path-a' d='M0 0h1v1z'/></g>"
        "<g id='redo-group-b'><path id='redo-path-b' d='M1 1h1v1z'/></g>");
    ASSERT_TRUE(doc);
    auto selection = doc->getSelection();
    std::vector<SPObject *> groups{
        doc->getObjectById("redo-group-a"), doc->getObjectById("redo-group-b")};
    selection->setList(groups);
    DocumentUndo::clearUndo(doc.get());
    auto const before = history_selection_xml(doc.get());
    selection->ungroup();
    auto const after = history_selection_xml(doc.get());
    ASSERT_NE(before, after);
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_EQ(history_selection_xml(doc.get()), before);
    std::vector<SPObject *> restored_groups{
        doc->getObjectById("redo-group-a"), doc->getObjectById("redo-group-b")};
    selection->setList(restored_groups);

    int changes = 0;
    auto connection = selection->connectChanged([&](auto) { ++changes; });
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    connection.disconnect();
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(selection->isEmpty());
    EXPECT_EQ(history_selection_xml(doc.get()), after);
}

// U3: cancelling a knot drag or a connector reroute undoes only the step that interaction pushed.
TEST_F(ObjectSetTest, CancelUndoesOnlyTheStepPushedSinceTheMark)
{
    auto *root_repr = _doc->getReprRoot();
    root_repr->setAttribute("data-u3", "before");
    DocumentUndo::done(_doc.get(), Util::Internal::ContextString{"Unrelated earlier step"}, "");
    auto const *mark = DocumentUndo::undoStackMark(_doc.get());
    ASSERT_NE(mark, nullptr);

    // A drag that changed nothing, then Escape: the earlier step survives.
    EXPECT_FALSE(DocumentUndo::undoSinceMark(_doc.get(), mark));
    EXPECT_STREQ(root_repr->attribute("data-u3"), "before");
    EXPECT_EQ(DocumentUndo::undoStackMark(_doc.get()), mark);

    // Uncommitted live changes of a drag are rolled back without touching the earlier step.
    root_repr->setAttribute("data-u3", "dragging");
    EXPECT_FALSE(DocumentUndo::undoSinceMark(_doc.get(), mark));
    EXPECT_STREQ(root_repr->attribute("data-u3"), "before");
    EXPECT_EQ(DocumentUndo::undoStackMark(_doc.get()), mark);

    // A drag that pushed a step: Escape undoes exactly that step.
    root_repr->setAttribute("data-u3", "dragged");
    DocumentUndo::done(_doc.get(), Util::Internal::ContextString{"Drag"}, "");
    EXPECT_TRUE(DocumentUndo::undoSinceMark(_doc.get(), mark));
    EXPECT_STREQ(root_repr->attribute("data-u3"), "before");
    EXPECT_EQ(DocumentUndo::undoStackMark(_doc.get()), mark);
}

// U3, keyed case: a drag whose ungrab commit uses maybeDone with the same key as the step on top would merge into
// that step. The mark drops the key, so the drag's step is always new and Escape undoes exactly it.
TEST_F(ObjectSetTest, CancelUndoesKeyedDragStepNotTheEarlierOne)
{
    // Fixture creation must survive the Undo below; its rectangles are still owned by r1/r2/r3.
    DocumentUndo::done(_doc.get(), Util::Internal::ContextString{"Initialize undo fixture"}, "");
    DocumentUndo::clearUndo(_doc.get());

    auto *root_repr = _doc->getReprRoot();
    root_repr->setAttribute("data-u3k", "earlier");
    DocumentUndo::maybeDone(_doc.get(), "u3-key", Util::Internal::ContextString{"Earlier keyed step"}, "");

    // Without a reset, a second same-key commit merges into the earlier step: one Undo removes both.
    root_repr->setAttribute("data-u3k2", "merged");
    DocumentUndo::maybeDone(_doc.get(), "u3-key", Util::Internal::ContextString{"Merged"}, "");
    ASSERT_TRUE(DocumentUndo::undo(_doc.get()));
    EXPECT_EQ(root_repr->attribute("data-u3k"), nullptr);
    EXPECT_EQ(root_repr->attribute("data-u3k2"), nullptr) << "same-key commits without a reset merge";
    ASSERT_TRUE(DocumentUndo::redo(_doc.get()));
    ASSERT_STREQ(root_repr->attribute("data-u3k2"), "merged");

    // With the mark taken at the start of the drag, the same keyed commit is a new step and Escape undoes it.
    root_repr->setAttribute("data-u3k", "earlier2");
    DocumentUndo::maybeDone(_doc.get(), "u3-key", Util::Internal::ContextString{"Earlier keyed step 2"}, "");
    auto const *mark = DocumentUndo::undoStackMark(_doc.get()); // drag starts: drops the key
    root_repr->setAttribute("data-u3k", "dragged");
    DocumentUndo::maybeDone(_doc.get(), "u3-key", Util::Internal::ContextString{"Drag"}, "");
    EXPECT_NE(DocumentUndo::undoStackMark(_doc.get()), mark) << "the drag's keyed commit must be its own step";
    EXPECT_TRUE(DocumentUndo::undoSinceMark(_doc.get(), mark));
    EXPECT_STREQ(root_repr->attribute("data-u3k"), "earlier2");
    EXPECT_STREQ(root_repr->attribute("data-u3k2"), "merged");
}

TEST_F(ObjectSetTest, UngroupAllBreaksEveryNestingLevel)
{
    set->add(r1.get());
    set->add(r2.get());
    set->add(r3.get());
    set->group();
    r1.release();
    r2.release();
    r3.release();

    auto *repr = _doc->getReprDoc()->createElement("svg:rect");
    _doc->getRoot()->appendChild(repr);
    auto *r4 = cast<SPRect>(_doc->getObjectByRepr(repr));
    ASSERT_NE(r4, nullptr);
    set->add(r4);
    set->group();
    set->group();

    set->ungroup_all();
    EXPECT_EQ(4, set->size());
    for (auto *item : set->items()) {
        EXPECT_FALSE(is<SPGroup>(item));
    }
    EXPECT_EQ(N + 4, _doc->getRoot()->children.size());
}

TEST_F(ObjectSetTest, UngroupAllKeepsCompoundPathsWhole)
{
    constexpr char const *d = "M 0,0 H 10 V 10 Z M 20,0 H 30 V 10 Z";
    auto *repr = _doc->getReprDoc()->createElement("svg:path");
    repr->setAttribute("d", d);
    _doc->getRoot()->appendChild(repr);
    auto *path = cast<SPPath>(_doc->getObjectByRepr(repr));
    ASSERT_NE(path, nullptr);
    set->add(r1.get());
    set->add(path);
    set->group();
    r1.release();

    set->ungroup_all();
    EXPECT_EQ(2, set->size());
    int paths = 0;
    for (auto *item : set->items()) {
        if (is<SPPath>(item)) {
            ++paths;
            ASSERT_NE(item->getRepr()->attribute("d"), nullptr);
            EXPECT_STREQ(d, item->getRepr()->attribute("d"));
        }
    }
    EXPECT_EQ(1, paths);
}

TEST_F(ObjectSetTest, UngroupAllWithoutGroupsChangesNothing)
{
    set->add(r1.get());
    set->add(r2.get());
    auto const children = _doc->getRoot()->children.size();
    set->ungroup_all();
    EXPECT_EQ(2, set->size());
    EXPECT_EQ(children, _doc->getRoot()->children.size());
}

TEST_F(ObjectSetTest, UngroupAllIsOneUndoStep)
{
    auto *b = _doc->getReprDoc()->createElement("svg:g");
    b->setAttribute("id", "undo-B");
    auto *a = _doc->getReprDoc()->createElement("svg:g");
    a->setAttribute("id", "undo-A");
    auto *r = _doc->getReprDoc()->createElement("svg:rect");
    r->setAttribute("id", "undo-R");
    a->appendChild(r);
    b->appendChild(a);
    _doc->getRoot()->appendChild(b);
    ASSERT_NE(_doc->getObjectByRepr(b), nullptr);
    DocumentUndo::done(_doc.get(), Util::Internal::ContextString{"Initialize Ungroup All fixture"}, "document-new");
    DocumentUndo::clearUndo(_doc.get());
    set->add(_doc->getObjectByRepr(b));
    set->ungroup_all();
    EXPECT_EQ(_doc->getObjectById("undo-B"), nullptr);
    EXPECT_EQ(_doc->getObjectById("undo-A"), nullptr);
    for (auto *item : set->items()) EXPECT_FALSE(is<SPGroup>(item));
    DocumentUndo::undo(_doc.get());
    auto *restored_b = _doc->getObjectById("undo-B");
    auto *restored_a = _doc->getObjectById("undo-A");
    ASSERT_NE(restored_b, nullptr);
    ASSERT_NE(restored_a, nullptr);
    EXPECT_EQ(restored_b->parent, _doc->getRoot());
    EXPECT_EQ(restored_a->parent, restored_b);
}

TEST_F(ObjectSetTest, UngroupAllKeepsDocumentPositions)
{
    auto *b = _doc->getReprDoc()->createElement("svg:g");
    b->setAttribute("transform", "translate(10,5)");
    auto *a = _doc->getReprDoc()->createElement("svg:g");
    a->setAttribute("transform", "scale(2)");
    auto *r = _doc->getReprDoc()->createElement("svg:rect");
    r->setAttribute("x", "1"); r->setAttribute("y", "1");
    r->setAttribute("width", "2"); r->setAttribute("height", "3");
    a->appendChild(r); b->appendChild(a); _doc->getRoot()->appendChild(b);
    auto *rect = cast<SPRect>(_doc->getObjectByRepr(r));
    ASSERT_NE(rect, nullptr);
    _doc->ensureUpToDate();
    auto before = rect->documentVisualBounds();
    ASSERT_TRUE(before);
    set->add(_doc->getObjectByRepr(b));
    set->ungroup_all();
    SPRect *result = nullptr;
    for (auto *item : set->items()) if (auto *candidate = cast<SPRect>(item)) result = candidate;
    ASSERT_NE(result, nullptr);
    _doc->ensureUpToDate();
    auto after = result->documentVisualBounds();
    ASSERT_TRUE(after);
    EXPECT_NEAR(after->left(), before->left(), 1e-6);
    EXPECT_NEAR(after->top(), before->top(), 1e-6);
    EXPECT_NEAR(after->right(), before->right(), 1e-6);
    EXPECT_NEAR(after->bottom(), before->bottom(), 1e-6);
}

TEST_F(ObjectSetTest, UngroupAllLeavesNonGroupItemsAlone)
{
    auto *b = _doc->getReprDoc()->createElement("svg:g");
    b->appendChild(_doc->getReprDoc()->createElement("svg:rect"));
    _doc->getRoot()->appendChild(b);
    auto *loose = _doc->getReprDoc()->createElement("svg:rect");
    loose->setAttribute("id", "loose");
    loose->setAttribute("x", "17");
    _doc->getRoot()->appendChild(loose);
    set->add(_doc->getObjectByRepr(b));
    set->add(_doc->getObjectByRepr(loose));
    set->ungroup_all();
    auto *remaining = _doc->getObjectById("loose");
    ASSERT_NE(remaining, nullptr);
    EXPECT_TRUE(set->includes(remaining));
    EXPECT_STREQ(remaining->getRepr()->attribute("x"), "17");
}

TEST_F(ObjectSetTest, UngroupAllKeepsNestedGroupsUsedByClones)
{
    auto *b = _doc->getReprDoc()->createElement("svg:g");
    auto *h = _doc->getReprDoc()->createElement("svg:g");
    h->setAttribute("id", "H");
    h->appendChild(_doc->getReprDoc()->createElement("svg:rect"));
    b->appendChild(h); _doc->getRoot()->appendChild(b);
    auto *use = _doc->getReprDoc()->createElement("svg:use");
    use->setAttribute("xlink:href", "#H");
    _doc->getRoot()->appendChild(use);
    ASSERT_NE(_doc->getObjectByRepr(use), nullptr);
    set->add(_doc->getObjectByRepr(b));
    set->ungroup_all();
    auto *kept = _doc->getObjectById("H");
    EXPECT_TRUE(is<SPGroup>(kept));
    auto *remaining_use = cast<SPUse>(_doc->getObjectByRepr(use));
    ASSERT_NE(remaining_use, nullptr);
    EXPECT_EQ(remaining_use->get_original(), kept);
    EXPECT_STREQ(remaining_use->getRepr()->attribute("xlink:href"), "#H");
}

TEST_F(ObjectSetTest, UngroupAllKeepsNestedMaskedGroups)
{
    auto *xml = _doc->getReprDoc();
    auto *mask = xml->createElement("svg:mask");
    mask->setAttribute("id", "m1");
    auto *white = xml->createElement("svg:rect");
    white->setAttribute("width", "100");
    white->setAttribute("height", "100");
    white->setAttribute("fill", "white");
    mask->appendChild(white);
    auto *defs = _doc->getRoot()->getRepr()->firstChild();
    while (defs && std::string(defs->name()) != "svg:defs") defs = defs->next();
    ASSERT_NE(defs, nullptr);
    defs->appendChild(mask);
    auto *outer = xml->createElement("svg:g");
    outer->setAttribute("id", "outer");
    auto *masked = xml->createElement("svg:g");
    masked->setAttribute("id", "masked");
    masked->setAttribute("mask", "url(#m1)");
    auto *rect = xml->createElement("svg:rect");
    rect->setAttribute("width", "20");
    rect->setAttribute("height", "20");
    masked->appendChild(rect);
    outer->appendChild(masked);
    _doc->getRoot()->appendChild(outer);
    set->add(_doc->getObjectById("outer"));
    set->ungroup_all();
    EXPECT_EQ(_doc->getObjectById("outer"), nullptr);
    EXPECT_TRUE(is<SPGroup>(_doc->getObjectById("masked")));
}

TEST_F(ObjectSetTest, UngroupAllKeepsLockedNestedGroups)
{
    auto *b = _doc->getReprDoc()->createElement("svg:g");
    b->setAttribute("id", "locked-outer");
    auto *l = _doc->getReprDoc()->createElement("svg:g");
    l->setAttribute("id", "L");
    l->setAttribute("sodipodi:insensitive", "true");
    l->appendChild(_doc->getReprDoc()->createElement("svg:rect"));
    b->appendChild(l); _doc->getRoot()->appendChild(b);
    set->add(_doc->getObjectByRepr(b));
    set->ungroup_all();
    EXPECT_TRUE(is<SPGroup>(_doc->getObjectById("L")));
    EXPECT_EQ(_doc->getObjectById("locked-outer"), nullptr);
}

TEST_F(ObjectSetTest, UngroupAllLeavesLayersAlone)
{
    auto *layer = _doc->getReprDoc()->createElement("svg:g");
    layer->setAttribute("id", "layer9");
    layer->setAttribute("inkscape:groupmode", "layer");
    auto *rect = _doc->getReprDoc()->createElement("svg:rect");
    rect->setAttribute("id", "layer-rect");
    layer->appendChild(rect); _doc->getRoot()->appendChild(layer);
    set->add(_doc->getObjectByRepr(layer));
    set->ungroup_all();
    auto *remaining = _doc->getObjectById("layer9");
    ASSERT_TRUE(is<SPGroup>(remaining));
    EXPECT_EQ(_doc->getObjectById("layer-rect")->parent, remaining);
}

TEST_F(ObjectSetTest, UngroupAllKeepsNestedGroupsWithEffects)
{
    auto *filter = _doc->getReprDoc()->createElement("svg:filter");
    filter->setAttribute("id", "f1");
    auto *blur = _doc->getReprDoc()->createElement("svg:feGaussianBlur");
    blur->setAttribute("stdDeviation", "1");
    filter->appendChild(blur);
    _doc->getDefs()->getRepr()->appendChild(filter);

    auto *outer = _doc->getReprDoc()->createElement("svg:g");
    outer->setAttribute("id", "outer");
    for (auto const &[id, style] : std::initializer_list<std::pair<char const *, char const *>>{
             {"fx-filter", "filter:url(#f1)"}, {"fx-blend", "mix-blend-mode:multiply"},
             {"fx-opacity", "opacity:0.5"}, {"plain", nullptr}}) {
        auto *group = _doc->getReprDoc()->createElement("svg:g");
        group->setAttribute("id", id);
        if (style) group->setAttribute("style", style);
        group->appendChild(_doc->getReprDoc()->createElement("svg:rect"));
        outer->appendChild(group);
    }
    _doc->getRoot()->appendChild(outer);
    set->add(_doc->getObjectById("outer"));
    set->ungroup_all();
    EXPECT_EQ(_doc->getObjectById("outer"), nullptr);
    EXPECT_EQ(_doc->getObjectById("plain"), nullptr);
    EXPECT_TRUE(is<SPGroup>(_doc->getObjectById("fx-filter")));
    EXPECT_TRUE(is<SPGroup>(_doc->getObjectById("fx-blend")));
    EXPECT_TRUE(is<SPGroup>(_doc->getObjectById("fx-opacity")));
}

TEST_F(ObjectSetTest, UngroupAllHoldsOutSelectedLayers)
{
    auto *layer = _doc->getReprDoc()->createElement("svg:g");
    layer->setAttribute("id", "layerA");
    layer->setAttribute("inkscape:groupmode", "layer");
    auto *rect = _doc->getReprDoc()->createElement("svg:rect");
    rect->setAttribute("id", "layerA-rect");
    layer->appendChild(rect);
    _doc->getRoot()->appendChild(layer);
    auto *group = _doc->getReprDoc()->createElement("svg:g");
    group->setAttribute("id", "g1");
    group->appendChild(_doc->getReprDoc()->createElement("svg:rect"));
    _doc->getRoot()->appendChild(group);
    set->add(_doc->getObjectById("layerA"));
    set->add(_doc->getObjectById("g1"));
    set->ungroup_all();
    auto *remaining = _doc->getObjectById("layerA");
    ASSERT_TRUE(is<SPGroup>(remaining));
    EXPECT_EQ(_doc->getObjectById("layerA-rect")->parent, remaining);
    EXPECT_TRUE(set->includes(remaining));
    EXPECT_EQ(_doc->getObjectById("g1"), nullptr);
}

TEST_F(ObjectSetTest, UngroupHoldsOutSelectedLayers)
{
    auto *layer = _doc->getReprDoc()->createElement("svg:g");
    layer->setAttribute("id", "layerA");
    layer->setAttribute("inkscape:groupmode", "layer");
    auto *rect = _doc->getReprDoc()->createElement("svg:rect");
    rect->setAttribute("id", "layerA-rect");
    layer->appendChild(rect);
    _doc->getRoot()->appendChild(layer);
    auto *group = _doc->getReprDoc()->createElement("svg:g");
    group->setAttribute("id", "g1");
    group->appendChild(_doc->getReprDoc()->createElement("svg:rect"));
    _doc->getRoot()->appendChild(group);
    set->add(_doc->getObjectById("layerA"));
    set->add(_doc->getObjectById("g1"));
    set->ungroup();
    auto *remaining = _doc->getObjectById("layerA");
    ASSERT_TRUE(is<SPGroup>(remaining));
    EXPECT_EQ(_doc->getObjectById("layerA-rect")->parent, remaining);
    EXPECT_TRUE(set->includes(remaining));
    EXPECT_EQ(_doc->getObjectById("g1"), nullptr);
}

TEST_F(ObjectSetTest, UngroupAllKeepsNestedLinksSwitchesAndBoxes)
{
    auto *outer = _doc->getReprDoc()->createElement("svg:g");
    outer->setAttribute("id", "outer");
    auto *link = _doc->getReprDoc()->createElement("svg:a");
    link->setAttribute("id", "link");
    link->setAttribute("xlink:href", "https://example.com");
    link->appendChild(_doc->getReprDoc()->createElement("svg:rect"));
    outer->appendChild(link);
    auto *sw = _doc->getReprDoc()->createElement("svg:switch");
    sw->setAttribute("id", "sw");
    sw->appendChild(_doc->getReprDoc()->createElement("svg:rect"));
    outer->appendChild(sw);
    _doc->getRoot()->appendChild(outer);
    set->add(_doc->getObjectById("outer"));
    set->ungroup_all();
    EXPECT_EQ(_doc->getObjectById("outer"), nullptr);
    EXPECT_TRUE(is<SPAnchor>(_doc->getObjectById("link")));
    EXPECT_TRUE(is<SPSwitch>(_doc->getObjectById("sw")));
}
