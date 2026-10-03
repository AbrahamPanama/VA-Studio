// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/artwork-library-document.h"
#include "io/artwork-library-package.h"
#include "io/artwork-library-svg-preflight.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <locale>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "display/cairo-utils.h"
#include "display/drawing.h"
#include "display/drawing-item.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "object/object-set.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-paint-server.h"
#include "object/sp-root.h"
#include "object/sp-text.h"
#include "object/sp-use.h"
#include "preferences.h"
#include "style.h"
#include "util/units.h"
#include "xml/attribute-record.h"
#include "xml/repr.h"

using namespace Inkscape;
namespace Library = Inkscape::IO::ArtworkLibrary;
using namespace std::literals;

namespace {

// Raw, nonmutating snapshot: includes node types, all attributes in their actual
// order, all text/comments and child order. Does NOT use save_buf, clean or sort.
void fingerprint(XML::Node const *node, std::string &out)
{
    auto field = [&](char const *text) {
        std::string_view value = text ? text : "";
        out += std::to_string(value.size()) + ":";
        out.append(value);
    };
    out += "[" + std::to_string(static_cast<int>(node->type()));
    field(node->name()); field(node->content());
    for (auto const &attr : node->attributeList()) {
        field(g_quark_to_string(attr.key)); field(static_cast<char const *>(attr.value));
    }
    out += ";";
    for (auto child = node->firstChild(); child; child = child->next()) fingerprint(child, out);
    out += "]";
}

std::string fingerprint(SPDocument &document)
{
    std::string result;
    fingerprint(document.getReprDoc(), result);
    return result;
}

std::unique_ptr<SPDocument> document(std::string const &body, std::string const &size = "width=\"120\" height=\"100\"",
                                      int *update_result = nullptr)
{
    auto xml = "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
               "xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" "
               "xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd\" " + size + ">" + body + "</svg>";
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
    if (doc) {
        auto result = doc->ensureUpToDate();
        if (update_result) *update_result = result;
    }
    return doc;
}

std::unique_ptr<SPDocument> reopen(Library::StagedSelection const &staged)
{
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(
        reinterpret_cast<char const *>(staged.svg.data()), staged.svg.size()));
    if (doc) doc->ensureUpToDate();
    return doc;
}

SPItem *item(SPDocument &doc, char const *id)
{
    return cast<SPItem>(doc.getObjectById(id));
}

std::size_t id_count(XML::Node *node, char const *id)
{
    std::size_t count = g_strcmp0(node->attribute("id"), id) == 0;
    for (auto child = node->firstChild(); child; child = child->next()) count += id_count(child, id);
    return count;
}

void drawing_trace(SPDocument &doc, XML::Node *node, unsigned key, std::ostream &out)
{
    if (auto object = cast<SPItem>(doc.getObjectByRepr(node))) {
        out << node->name() << " id=" << (node->attribute("id") ? node->attribute("id") : "<none>");
        for (auto name : {"x", "y", "width", "height", "viewBox", "transform", "style", "clip-path", "mask", "filter"}) {
            if (auto value = node->attribute(name)) out << " " << name << "=\"" << value << "\"";
        }
        if (auto root = cast<SPRoot>(object)) {
            out << " c2p=[";
            for (unsigned i = 0; i < 6; ++i) out << (i ? "," : "") << root->c2p[i];
            out << "] overflow=" << root->style->overflow.get_value();
        }
        if (auto view = object->get_arenaitem(key)) {
            out << " CTM=[";
            for (unsigned i = 0; i < 6; ++i) out << (i ? "," : "") << view->ctm()[i];
            out << "]";
            if (auto bounds = view->itemBounds()) {
                out << " itemBounds=" << bounds->left() << "," << bounds->top()
                    << "," << bounds->right() << "," << bounds->bottom();
            }
            if (auto bounds = view->drawbox()) {
                out << " drawbox=" << bounds->left() << "," << bounds->top()
                    << "," << bounds->right() << "," << bounds->bottom();
            }
        }
        out << '\n';
    }
    for (auto child = node->firstChild(); child; child = child->next()) drawing_trace(doc, child, key, out);
}

Cairo::RefPtr<Cairo::ImageSurface> render(SPDocument &doc, Geom::Point correction,
                                        double zoom, std::vector<char const *> const &hide = {},
                                        std::string *trace = nullptr)
{
    Drawing drawing;
    auto key = SPItem::display_key_new(1);
    auto root = doc.getRoot();
    auto shown = root->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY);
    drawing.setRoot(shown);
    shown->setTransform(Geom::Translate(correction) * Geom::Scale(zoom) * Geom::Translate(9.25, 7.375));
    for (auto id : hide) if (auto object = item(doc, id)) {
        if (auto view = object->get_arenaitem(key)) view->setVisible(false);
    }
    drawing.update();
    if (trace) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(17) << "correction=" << correction[0] << "," << correction[1]
            << " zoom=" << zoom << '\n';
        drawing_trace(doc, doc.getReprRoot(), key, out);
        *trace = out.str();
    }
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 256, 192);
    DrawingSurface target(surface->cobj(), Geom::IntPoint(0, 0));
    DrawingContext context(target);
    drawing.render(context, Geom::IntRect::from_xywh(0, 0, 256, 192));
    surface->flush();
    root->invoke_hide(key);
    return surface;
}

std::size_t different_pixels(Cairo::RefPtr<Cairo::ImageSurface> const &a,
                             Cairo::RefPtr<Cairo::ImageSurface> const &b)
{
    std::size_t different = 0;
    for (int y = 0; y < a->get_height(); ++y) for (int x = 0; x < a->get_width(); ++x) {
        different += std::memcmp(a->get_data() + y * a->get_stride() + 4 * x,
                                 b->get_data() + y * b->get_stride() + 4 * x, 4) != 0;
    }
    return different;
}

std::string pixel_difference(Cairo::RefPtr<Cairo::ImageSurface> const &a,
                             Cairo::RefPtr<Cairo::ImageSurface> const &b)
{
    std::ostringstream out;
    int min_x = a->get_width(), min_y = a->get_height(), max_x = -1, max_y = -1, max_delta = 0;
    std::size_t samples = 0;
    for (int y = 0; y < a->get_height(); ++y) for (int x = 0; x < a->get_width(); ++x) {
        auto p = a->get_data() + y * a->get_stride() + 4 * x;
        auto q = b->get_data() + y * b->get_stride() + 4 * x;
        if (!std::memcmp(p, q, 4)) continue;
        min_x = std::min(min_x, x); min_y = std::min(min_y, y);
        max_x = std::max(max_x, x); max_y = std::max(max_y, y);
        for (unsigned c = 0; c < 4; ++c) max_delta = std::max(max_delta, std::abs(int(p[c]) - int(q[c])));
        if (samples++ < 8) {
            // Native-endian premultiplied ARGB32 bytes, not unpremultiplied color.
            out << "pixel(" << x << "," << y << ") source=[";
            for (unsigned c = 0; c < 4; ++c) out << (c ? "," : "") << unsigned(p[c]);
            out << "] staged=[";
            for (unsigned c = 0; c < 4; ++c) out << (c ? "," : "") << unsigned(q[c]);
            out << "]\n";
        }
    }
    out << "difference bounds=" << min_x << "," << min_y << ".." << max_x << "," << max_y
        << " maximum byte delta=" << max_delta << '\n';
    return out.str();
}

void expect_native_render(SPDocument &source, SPDocument &staged, Geom::Point offset,
                          std::vector<char const *> const &hide = {})
{
    for (double zoom : {0.75, 1.0, 1.375}) {
        SCOPED_TRACE(zoom);
        std::string source_trace, staged_trace;
        auto expected = render(source, {0, 0}, zoom, hide, &source_trace);
        auto actual = render(staged, offset, zoom, {}, &staged_trace);
        EXPECT_EQ(different_pixels(expected, actual), 0u)
            << pixel_difference(expected, actual) << "SOURCE\n" << source_trace << "STAGED\n" << staged_trace;
    }
}

class ArtworkLibraryDocumentTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        // No InkscapeApplication, desktop or ClipboardManager/GDK clipboard.
        if (!Application::exists()) Application::create(false);
    }
};

TEST_F(ArtworkLibraryDocumentTest, HeadlessStageLeavesSourceXmlDirtyFlagAndObjectSetUntouched)
{
    auto doc = document(R"svg(<rect id="art" x="10" y="8" width="30" height="20" fill="red"/>
                           <circle id="unselected" cx="80" cy="60" r="5"/>)svg");
    ASSERT_TRUE(doc);
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto before = fingerprint(*doc);
    auto dirty = doc->isModifiedSinceSave();
    auto sensitive = DocumentUndo::getUndoSensitive(doc.get());
    auto pointers = selected.items_vector();
    auto staged = Library::stage_selection(selected);
    EXPECT_EQ(fingerprint(*doc), before);
    EXPECT_EQ(doc->isModifiedSinceSave(), dirty);
    EXPECT_EQ(DocumentUndo::getUndoSensitive(doc.get()), sensitive);
    EXPECT_EQ(selected.items_vector(), pointers);
    auto copy = reopen(staged);
    ASSERT_TRUE(copy);
    EXPECT_TRUE(copy->getObjectById("art"));
    EXPECT_FALSE(copy->getObjectById("unselected"));
    EXPECT_NEAR(staged.width_mm, 30 * 25.4 / 96, 0.01);
    EXPECT_NEAR(staged.height_mm, 20 * 25.4 / 96, 0.01);
    expect_native_render(*doc, *copy, {10, 8}, {"unselected"});
    auto expected = render(*copy, {10, 8}, 1);
    item(*copy, "art")->getRepr()->setAttribute("x", "11");
    copy->ensureUpToDate();
    EXPECT_GT(different_pixels(expected, render(*copy, {10, 8}, 1)), 0u); // Oracle positive control.
}

TEST_F(ArtworkLibraryDocumentTest, StagingDoesNotLeakXmlStandaloneMetadataIntoSvg)
{
    auto doc = document(R"svg(<rect id="art" x="10" y="8" width="30" height="20" fill="red"/>)svg");
    ASSERT_TRUE(doc);
    // Older/native document paths can expose the XML declaration flag on the
    // root representation. It must never become an SVG/CSS attribute in a
    // library asset.
    doc->getReprRoot()->setAttribute("standalone", "no");
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto staged = Library::stage_selection(selected);
    auto admitted = Library::preflight_svg(
        std::span<unsigned char const>(staged.svg.data(), staged.svg.size()),
        {"b203e8e9-640c-4195-b0ea-f9b790245678", Library::artwork_sha256(staged.svg), staged.width_mm, staged.height_mm});
    EXPECT_FALSE(admitted.svg_bytes()->find("standalone") != std::string::npos);
}

// Owner report: Add selection failed with "Unaudited Inkscape metadata
// attribute: export-filename" on a shape that had been exported before.
TEST_F(ArtworkLibraryDocumentTest, StagingDropsTheRememberedExportPathAndIsAdmitted)
{
    auto doc = document(R"svg(<rect id="art" x="10" y="8" width="30" height="20" fill="red"
        inkscape:export-filename="/Users/someone/Desktop/art.png" inkscape:export-xdpi="300"
        inkscape:export-ydpi="300"/>)svg");
    ASSERT_TRUE(doc);
    doc->getReprRoot()->setAttribute("inkscape:export-filename", "C:\\Exports\\page.png");
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto before = fingerprint(*doc);
    auto staged = Library::stage_selection(selected);
    EXPECT_EQ(fingerprint(*doc), before); // the source keeps its own export settings
    std::string const text(staged.svg.begin(), staged.svg.end());
    EXPECT_EQ(text.find("export-filename"), std::string::npos) << text;
    EXPECT_NE(text.find("export-xdpi"), std::string::npos) << text;
    auto admitted = Library::preflight_svg(
        std::span<unsigned char const>(staged.svg.data(), staged.svg.size()),
        {"b203e8e9-640c-4195-b0ea-f9b790245678", Library::artwork_sha256(staged.svg), staged.width_mm, staged.height_mm});
    EXPECT_TRUE(admitted.svg_bytes());
}

TEST_F(ArtworkLibraryDocumentTest, StagingDoesNotConsumeOrClearExistingUndoRedo)
{
    auto doc = document(R"svg(<rect id="art" x="10" y="8" width="30" height="20"/>)svg");
    ASSERT_TRUE(doc);
    DocumentUndo::setUndoSensitive(doc.get(), true);
    auto repr = doc->getObjectById("art")->getRepr();
    repr->setAttribute("x", "12");
    DocumentUndo::done(doc.get(), Util::Internal::ContextString{"staging history fixture"}, "");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    doc->ensureUpToDate();
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto before = fingerprint(*doc);
    auto staged = Library::stage_selection(selected);
    EXPECT_EQ(fingerprint(*doc), before);
    ASSERT_TRUE(DocumentUndo::redo(doc.get()));
    EXPECT_STREQ(repr->attribute("x"), "12");
    ASSERT_TRUE(DocumentUndo::undo(doc.get()));
    EXPECT_STREQ(repr->attribute("x"), "10");
    EXPECT_FALSE(staged.svg.empty());
}

TEST_F(ArtworkLibraryDocumentTest, PrunedSiblingDoesNotDonateItsStyleAndAncestorOpacitySurvives)
{
    auto doc = document(R"svg(<style>.red{fill:red}.blue{fill:blue}</style>
      <g id="parent" transform="translate(3,4)" opacity="0.6" stroke="green" stroke-width="2">
       <rect id="skip" class="red" width="20" height="15"/>
       <rect id="art" class="blue" x="25" width="20" height="15"/>
      </g>)svg");
    ASSERT_TRUE(doc);
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    EXPECT_FALSE(copy->getObjectById("skip"));
    EXPECT_STREQ(copy->getObjectById("parent")->getRepr()->attribute("opacity"), "0.6");
    EXPECT_EQ(item(*copy, "art")->style->fill.get_value(), item(*doc, "art")->style->fill.get_value());
    expect_native_render(*doc, *copy, bounds->min(), {"skip"});
}

TEST_F(ArtworkLibraryDocumentTest, CssTextDescendantsKeepContentKerningAndInheritedFontSizes)
{
    auto doc = document(R"svg(<style>.word{fill:#124578;font-size:150%}</style>
      <g font-family="sans-serif" font-size="12"><text id="art" x="4" y="35" xml:space="preserve">
      <tspan id="span" class="word" dx="1 2 3">a  b</tspan></text></g>)svg");
    ASSERT_TRUE(doc);
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    auto old_span = item(*doc, "span"); auto new_span = item(*copy, "span"); ASSERT_TRUE(new_span);
    EXPECT_DOUBLE_EQ(new_span->style->font_size.computed, old_span->style->font_size.computed);
    EXPECT_STREQ(new_span->getRepr()->attribute("dx"), "1 2 3");
    EXPECT_STREQ(new_span->getRepr()->firstChild()->content(), "a  b");
    EXPECT_FALSE(staged.warnings.empty()); // Text is editable; fonts are not embedded.
    expect_native_render(*doc, *copy, bounds->min());
}

TEST_F(ArtworkLibraryDocumentTest, RootViewBoxPercentLengthsAndPhysicalUnitsSurvive)
{
    for (auto size : {"width=\"120mm\" height=\"80mm\" viewBox=\"10 20 240 160\"",
                      "width=\"160\" height=\"90\" viewBox=\"-20 -10 120 80\" preserveAspectRatio=\"none\""}) {
        SCOPED_TRACE(size);
        auto doc = document(R"svg(<g transform="translate(2.5,3.25)"><rect id="art" x="25%" y="30%" width="20%" height="10%" fill="red"/></g>)svg", size);
        ASSERT_TRUE(doc);
        ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
        auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
        auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
        EXPECT_NEAR(staged.width_mm, bounds->width() * 25.4 / 96, 0.01);
        EXPECT_NEAR(staged.height_mm, bounds->height() * 25.4 / 96, 0.01);
        EXPECT_STREQ(item(*copy, "art")->getRepr()->attribute("width"), "20%");
        auto actual = item(*copy, "art")->documentGeometricBounds(); ASSERT_TRUE(actual);
        EXPECT_NEAR(actual->width(), bounds->width(), 1e-8);
        EXPECT_NEAR(actual->height(), bounds->height(), 1e-8);
    }
}

TEST_F(ArtworkLibraryDocumentTest, NestedClonesKeepIdsTargetsAndDoNotRenderUnselectedOriginal)
{
    auto doc = document(R"svg(<g id="ancestor" transform="translate(70,0)" fill="blue">
      <rect id="original" width="15" height="12"/></g>
      <use id="middle" xlink:href="#original" x="5" fill="green"/>
      <use id="art" xlink:href="#middle" x="20" y="25" fill="red"/>)svg");
    ASSERT_TRUE(doc);
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    for (auto id : {"original", "middle", "art", "ancestor"}) {
        EXPECT_EQ(id_count(copy->getReprRoot(), id), 1u);
    }
    auto use = cast<SPUse>(copy->getObjectById("art")); ASSERT_TRUE(use);
    ASSERT_TRUE(use->trueOriginal()); EXPECT_STREQ(use->trueOriginal()->getId(), "original");
    expect_native_render(*doc, *copy, bounds->min(), {"ancestor", "middle"});
}

TEST_F(ArtworkLibraryDocumentTest, SelectedOriginalAndCloneAreNotDuplicated)
{
    auto doc = document(R"svg(<rect id="original" width="10" height="10"/>
                           <use id="art" xlink:href="#original" x="25"/>)svg");
    ASSERT_TRUE(doc); ObjectSet selected(doc.get());
    selected.add(doc->getObjectById("original")); selected.add(doc->getObjectById("art"));
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    EXPECT_EQ(id_count(copy->getReprRoot(), "original"), 1u);
    expect_native_render(*doc, *copy, {0, 0});
}

TEST_F(ArtworkLibraryDocumentTest, TextPathTargetKeepsNativeTransformWithoutExtraVisiblePath)
{
    auto doc = document(R"svg(<g transform="translate(0,45)"><path id="path" transform="translate(4,2)" d="M 0,25 L 95,25" stroke="red"/></g>
      <text id="art" style="font-family:sans-serif;font-size:12px"><textPath xlink:href="#path">Editable text</textPath></text>)svg");
    ASSERT_TRUE(doc); ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    EXPECT_EQ(id_count(copy->getReprRoot(), "path"), 1u);
    EXPECT_STREQ(item(*copy, "path")->getRepr()->attribute("transform"), "translate(4,2)");
    expect_native_render(*doc, *copy, bounds->min(), {"path"});
}

TEST_F(ArtworkLibraryDocumentTest, ShapeInsideAndSubtractKeepTheirDifferentNativeCoordinateRules)
{
    auto doc = document(R"svg(<g transform="translate(2,3)"><rect id="inside" x="4" y="4" width="90" height="60"/></g>
      <g transform="translate(8,3)"><rect id="subtract" x="20" y="10" width="12" height="15"/></g>
      <text id="art" style="font-family:sans-serif;font-size:10px;shape-inside:url(#inside);shape-subtract:url(#subtract);white-space:pre-wrap">Several words wrapping around the excluded shape to test editable flow layout.</text>)svg");
    ASSERT_TRUE(doc); ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    ASSERT_TRUE(copy->getObjectById("inside")); ASSERT_TRUE(copy->getObjectById("subtract"));
    expect_native_render(*doc, *copy, bounds->min(), {"inside", "subtract"});
}

TEST_F(ArtworkLibraryDocumentTest, MixedSelectedBranchKeepsOutsideShapeSubtractAffineAndRender)
{
    // T alone did not exercise the old placement bug: its whole G branch went
    // below defs. Selecting KEEP too retained G visibly and put generated defs
    // INSIDE G, where native i2anc_affine stopped before seeing G's transform.
    for (auto inner_transform : {"translate(0,0)", "scale(1.1,0.9) skewX(12)"}) {
        SCOPED_TRACE(inner_transform);
        auto body = std::string(R"svg(<rect id="INSIDE" x="4" y="4" width="90" height="60"/>
          <g id="G" transform="translate(25,12)" fill="blue">
           <g id="H" transform=")svg") + inner_transform + R"svg(">
            <rect id="KEEP" x="65" y="45" width="5" height="5"/>
            <rect id="CUT" x="0" y="0" width="22" height="30"/>
           </g>
          </g>
          <text id="T" style="font-family:sans-serif;font-size:10px;shape-inside:url(#INSIDE);shape-subtract:url(#CUT);white-space:pre-wrap">Several words wrapping around the excluded shape to test editable flow layout.</text>)svg";
        auto doc = document(body); ASSERT_TRUE(doc);
        ObjectSet selected(doc.get());
        selected.add(doc->getObjectById("T")); selected.add(doc->getObjectById("KEEP"));
        auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
        auto original = fingerprint(*doc);
        auto selection_before = selected.items_vector();
        auto expected_affine = item(*doc, "CUT")->getRelativeTransform(item(*doc, "T"));
        auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
        EXPECT_EQ(fingerprint(*doc), original);
        EXPECT_EQ(selected.items_vector(), selection_before);
        for (auto id : {"G", "H", "KEEP", "CUT", "INSIDE", "T"}) {
            EXPECT_EQ(id_count(copy->getReprRoot(), id), 1u);
            ASSERT_TRUE(copy->getObjectById(id));
            EXPECT_STREQ(copy->getObjectById(id)->getRepr()->attribute("transform"),
                         doc->getObjectById(id)->getRepr()->attribute("transform"));
        }
        auto cut = item(*copy, "CUT"); auto text = item(*copy, "T");
        ASSERT_TRUE(cut); ASSERT_TRUE(text);
        auto actual_affine = cut->getRelativeTransform(text);
        for (unsigned i = 0; i < 6; ++i) EXPECT_NEAR(actual_affine[i], expected_affine[i], 1e-10);

        // The visible tree/IDs stay intact; only resource contexts are private.
        EXPECT_EQ(item(*copy, "KEEP")->parent, copy->getObjectById("H"));
        EXPECT_EQ(copy->getObjectById("H")->parent, copy->getObjectById("G"));
        auto context_h = cut->parent; ASSERT_TRUE(context_h);
        auto context_g = context_h->parent; ASSERT_TRUE(context_g);
        ASSERT_NE(context_h, copy->getObjectById("H"));
        ASSERT_NE(context_g, copy->getObjectById("G"));
        EXPECT_STREQ(context_h->getRepr()->attribute("transform"), inner_transform);
        EXPECT_STREQ(context_g->getRepr()->attribute("transform"), "translate(25,12)");
        auto defs = context_g->parent; ASSERT_TRUE(defs);
        EXPECT_STREQ(defs->getRepr()->name(), "svg:defs");
        EXPECT_EQ(defs->parent, copy->getObjectById("G")->parent);
        expect_native_render(*doc, *copy, bounds->min(), {"INSIDE", "CUT"});

        // Positive control reproduces the lost G contribution without touching
        // the selected visible G/KEEP or weakening the premultiplied oracle.
        auto expected_render = render(*copy, bounds->min(), 1.375);
        context_g->getRepr()->setAttribute("transform", nullptr);
        copy->ensureUpToDate();
        // This control tests geometry/layout, not ancestor-change notification:
        // recompute native text explicitly after deliberately breaking context.
        cast<SPText>(text)->rebuildLayout();
        auto broken_affine = cut->getRelativeTransform(text);
        EXPECT_GT(std::abs(broken_affine[4] - expected_affine[4]), 1.0);
        EXPECT_GT(different_pixels(expected_render, render(*copy, bounds->min(), 1.375)), 0u);
    }
}

TEST_F(ArtworkLibraryDocumentTest, MixedResourceContextsReuseChainsAndReserveIdsWithoutChangingSourceDefs)
{
    auto doc = document(R"svg(<g id="G" transform="translate(15,9)">
       <rect id="KEEP" x="65" y="45" width="5" height="5"/>
       <rect id="CUT" x="0" y="0" width="15" height="20"/>
       <rect id="CUT2" x="35" y="20" width="15" height="20"/>
       <defs id="original-defs"><rect id="EXISTING" x="4" y="4" width="90" height="60"/></defs>
      </g>
      <rect id="vacards-library-context-1" x="90" y="70" width="3" height="3"/>
      <text id="T" style="font-family:sans-serif;font-size:10px;shape-inside:url(#EXISTING);shape-subtract:url(#CUT) url(#CUT2);white-space:pre-wrap">Several words wrapping around the excluded shapes to test editable flow layout.</text>)svg");
    ASSERT_TRUE(doc); ObjectSet selected(doc.get());
    for (auto id : {"KEEP", "T", "vacards-library-context-1"}) selected.add(doc->getObjectById(id));
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    for (auto id : {"G", "KEEP", "CUT", "CUT2", "original-defs", "EXISTING", "T", "vacards-library-context-1"}) {
        ASSERT_TRUE(copy->getObjectById(id));
        EXPECT_EQ(id_count(copy->getReprRoot(), id), 1u);
    }
    EXPECT_EQ(copy->getObjectById("CUT")->parent, copy->getObjectById("CUT2")->parent);
    EXPECT_NE(copy->getObjectById("CUT")->parent, copy->getObjectById("G"));
    EXPECT_EQ(copy->getObjectById("EXISTING")->parent, copy->getObjectById("original-defs"));
    EXPECT_EQ(copy->getObjectById("original-defs")->parent, copy->getObjectById("G"));
    for (auto id : {"CUT", "CUT2", "EXISTING"}) {
        auto expected = item(*doc, id)->getRelativeTransform(item(*doc, "T"));
        auto actual = item(*copy, id)->getRelativeTransform(item(*copy, "T"));
        for (unsigned i = 0; i < 6; ++i) EXPECT_NEAR(actual[i], expected[i], 1e-10);
    }
    expect_native_render(*doc, *copy, bounds->min(), {"CUT", "CUT2"});
}

TEST_F(ArtworkLibraryDocumentTest, SharedGradientsPatternsClipsMasksAndFiltersRemainEditable)
{
    auto doc = document(R"svg(<defs>
      <linearGradient id="base"><stop stop-color="red"/><stop offset="1" stop-color="blue"/></linearGradient>
      <linearGradient id="paint" xlink:href="#base"/>
      <pattern id="pattern" width="10" height="10" patternUnits="userSpaceOnUse"><rect width="10" height="10" fill="url(#paint)"/></pattern>
      <clipPath id="clip"><rect x="2" y="2" width="25" height="20"/></clipPath>
      <mask id="mask" maskUnits="userSpaceOnUse" x="0" y="0" width="80" height="40"><rect width="80" height="40" fill="white"/></mask>
      <filter id="filter" x="-0.1" y="-0.1" width="1.2" height="1.2"><feGaussianBlur stdDeviation="0.5"/></filter>
      <linearGradient id="unused"><stop stop-color="green"/></linearGradient>
      </defs><g id="art"><rect id="patterned" width="30" height="25" fill="url(#pattern)" clip-path="url(#clip)"/>
       <rect id="filtered" x="40" width="20" height="20" fill="url(#paint)" filter="url(#filter)" mask="url(#mask)"/>
      </g>)svg");
    ASSERT_TRUE(doc); ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    for (auto id : {"base", "paint", "pattern", "clip", "mask", "filter"}) {
        EXPECT_EQ(id_count(copy->getReprRoot(), id), 1u);
    }
    EXPECT_FALSE(copy->getObjectById("unused"));
    expect_native_render(*doc, *copy, bounds->min());
    // Keep the complete three-zoom oracle above. These independent one-branch
    // renders localize a failure without removing any resource from either XML.
    for (auto hidden : {"patterned", "filtered"}) {
        SCOPED_TRACE(std::string("hidden branch: ") + hidden);
        auto expected = render(*doc, {0, 0}, 1, {hidden});
        auto actual = render(*copy, bounds->min(), 1, {hidden});
        EXPECT_EQ(different_pixels(expected, actual), 0u) << pixel_difference(expected, actual);
    }
}

TEST_F(ArtworkLibraryDocumentTest, ResourceContextBridgesRootPresentationWithoutCompoundingRelativeFontSize)
{
    // A top-level percentage font has no native parent cascade. Keep the root
    // finite and put the percentage on a real child context, which is moved
    // below generated defs by staging. Keep the paint server outside the branch
    // that inherits its fill, avoiding a source paint-modification feedback loop.
    int updated = 0;
    auto doc = document(R"svg(<defs><linearGradient id="paint"><stop stop-color="red"/>
      <stop offset="1" stop-color="blue"/></linearGradient></defs>
      <g id="hidden" transform="translate(3,2)" opacity="inherit" font-size="150%" fill="url(#paint)">
       <text id="original" x="4" y="25" style="font-size:80%">Editable</text>
      </g><use id="art" xlink:href="#original" x="35" y="10" font-size="150%" fill="url(#paint)"/>)svg",
      R"svg(width="200" height="100" font-family="sans-serif" font-size="12px" opacity="0.6" fill="#345678")svg", &updated);
    ASSERT_TRUE(doc);
    ASSERT_NE(updated, 0) << "Positive source fixture must converge before staging";
    ASSERT_EQ(doc->getRoot()->style->font_size.computed, 12.0);
    auto source_context = doc->getObjectById("hidden"); ASSERT_TRUE(source_context);
    ASSERT_EQ(source_context->style->font_size.computed, 18.0);
    auto source_original = item(*doc, "original"); ASSERT_TRUE(source_original);
    ASSERT_NEAR(source_original->style->font_size.computed, 14.4, 1e-12);
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    ASSERT_TRUE(std::isfinite(bounds->width())); ASSERT_GT(bounds->width(), 0);
    ASSERT_TRUE(std::isfinite(bounds->height())); ASSERT_GT(bounds->height(), 0);
    auto before = fingerprint(*doc);
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    EXPECT_EQ(fingerprint(*doc), before);
    auto original = item(*copy, "original"); ASSERT_TRUE(original);
    auto context = original->parent; ASSERT_TRUE(context);
    auto defs = context->parent; ASSERT_TRUE(defs);
    ASSERT_STREQ(defs->getRepr()->name(), "svg:defs");
    ASSERT_TRUE(defs->style); ASSERT_TRUE(context->style);
    EXPECT_DOUBLE_EQ(defs->style->font_size.computed, doc->getRoot()->style->font_size.computed);
    EXPECT_DOUBLE_EQ(context->style->font_size.computed, source_context->style->font_size.computed);
    EXPECT_DOUBLE_EQ(original->style->font_size.computed, item(*doc, "original")->style->font_size.computed);
    EXPECT_EQ(unsigned(context->style->opacity.value), unsigned(source_context->style->opacity.value));
    EXPECT_EQ(unsigned(defs->style->opacity.value), unsigned(doc->getRoot()->style->opacity.value));
    auto paint = copy->getObjectById("paint"); ASSERT_TRUE(paint);
    auto art = item(*copy, "art"); ASSERT_TRUE(art);
    EXPECT_EQ(static_cast<SPObject *>(original->style->getFillPaintServer()), paint);
    EXPECT_EQ(static_cast<SPObject *>(context->style->getFillPaintServer()), paint);
    EXPECT_EQ(static_cast<SPObject *>(art->style->getFillPaintServer()), paint);
    EXPECT_EQ(original->style->fill.get_value(), item(*doc, "original")->style->fill.get_value());
    auto style = defs->getRepr()->attribute("style"); ASSERT_TRUE(style);
    std::string_view css(style);
    EXPECT_EQ(css.find("font-size:"), css.npos);
    EXPECT_EQ(css.find("filter:inherit"), css.npos);
    EXPECT_EQ(css.find("shape-inside:inherit"), css.npos);
    EXPECT_EQ(css.find("shape-subtract:inherit"), css.npos);
    expect_native_render(*doc, *copy, bounds->min(), {"hidden"});
}

TEST_F(ArtworkLibraryDocumentTest, NativeTopLevelPercentageFontSourceRefusesWithoutMutation)
{
    // Retain the originally failing source geometry/style, not a new production rejection
    // category. The solid-fill variant isolates the unresolved percentage size
    // from root/descendant paint-server modification feedback. No warnings are
    // suppressed; the original paint variant's update-limit warning is expected.
    for (bool root_paint : {false, true}) {
        SCOPED_TRACE(root_paint ? "original source: percentage + root paint" : "source control: percentage only");
        int updated = 0;
        auto size = std::string(R"svg(width="200" height="100" font-family="sans-serif" font-size="150%" opacity="0.6" fill=")svg") +
                    (root_paint ? "url(#paint)" : "#345678") + "\"";
        auto doc = document(R"svg(<defs><linearGradient id="paint"><stop stop-color="red"/>
          <stop offset="1" stop-color="blue"/></linearGradient></defs>
          <g id="hidden" transform="translate(3,2)" opacity="inherit">
           <text id="original" x="4" y="25" style="font-size:80%">Editable</text>
          </g><use id="art" xlink:href="#original" x="35" y="10"/>)svg", size, &updated);
        ASSERT_TRUE(doc);
        EXPECT_EQ(updated != 0, !root_paint);
        ASSERT_GT(doc->getRoot()->style->font_size.computed, 0);
        ASSERT_LE(doc->getRoot()->style->font_size.computed, 1e-30);
        ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
        auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX);
        ASSERT_TRUE(!bounds || bounds->width() <= 0 || bounds->height() <= 0)
            << "If native root percentage sizing is repaired, requalify this source positively";
        auto before = fingerprint(*doc);
        auto pointers = selected.items_vector();
        auto dirty = doc->isModifiedSinceSave();
        auto sensitive = DocumentUndo::getUndoSensitive(doc.get());
        try {
            Library::stage_selection(selected);
            ADD_FAILURE() << "A source with no usable text extent must not be published";
        } catch (std::runtime_error const &error) {
            EXPECT_STREQ(error.what(), "Selection has no finite positive physical extent");
        }
        EXPECT_EQ(fingerprint(*doc), before);
        EXPECT_EQ(selected.items_vector(), pointers);
        EXPECT_EQ(doc->isModifiedSinceSave(), dirty);
        EXPECT_EQ(DocumentUndo::getUndoSensitive(doc.get()), sensitive);
    }
}

TEST_F(ArtworkLibraryDocumentTest, NestedSourceViewportDoesNotClipNegativeBlurBounds)
{
    // Unlike the shared-resource fixture, no mask intersects away y < 0 here.
    auto defs = R"svg(<defs><filter id="f" x="-1" y="-1" width="3" height="3">
      <feGaussianBlur stdDeviation="2"/></filter>
      <clipPath id="viewport"><rect width="24" height="12"/></clipPath></defs>)svg";
    auto art = R"svg(<rect id="art" x="8" y="0" width="8" height="8" fill="red" filter="url(#f)"/>)svg";
    auto doc = document(std::string(defs) + art, "width=\"24\" height=\"12\""); ASSERT_TRUE(doc);
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    ASSERT_LT(bounds->top(), 0);
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    expect_native_render(*doc, *copy, bounds->min());
    auto expected = render(*doc, {0, 0}, 1);
    std::size_t above_viewport = 0;
    // Original viewport y=0 maps to 7.375 device pixels in this harness.
    for (int y = 0; y < 7; ++y) for (int x = 0; x < expected->get_width(); ++x) {
        std::uint32_t pixel;
        std::memcpy(&pixel, expected->get_data() + y * expected->get_stride() + 4 * x, sizeof(pixel));
        above_viewport += (pixel >> 24) != 0;
    }
    EXPECT_GT(above_viewport, 0u);
    // A real native viewport-sized clip must be detected, not tolerated as AA.
    auto clipped = document(std::string(defs) + "<g clip-path=\"url(#viewport)\">" + art + "</g>",
                            "width=\"24\" height=\"12\"");
    ASSERT_TRUE(clipped);
    EXPECT_GT(different_pixels(expected, render(*clipped, {0, 0}, 1)), 0u);
}

TEST_F(ArtworkLibraryDocumentTest, ExplicitUnsupportedAndMissingReferencesLeaveSourceUntouched)
{
    for (auto body : {
        R"svg(<rect id="art" width="20" height="20" fill="url(#missing)"/>)svg",
        R"svg(<use id="art" xlink:href="https://example.invalid/external.svg#shape"/>)svg",
        R"svg(<g id="art"><rect width="20" height="20"/><script>void 0</script></g>)svg",
        R"svg(<g filter="url(#f)"><rect id="art" width="20" height="20"/><rect x="40" width="20" height="20"/></g><defs><filter id="f"><feGaussianBlur stdDeviation="1"/></filter></defs>)svg",
        R"svg(<path id="art" d="M0,0L20,0L20,20Z" inkscape:path-effect="#lpe"/>)svg"}) {
        SCOPED_TRACE(body); auto doc = document(body); ASSERT_TRUE(doc);
        ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
        auto before = fingerprint(*doc);
        EXPECT_THROW(Library::stage_selection(selected), std::runtime_error);
        EXPECT_EQ(fingerprint(*doc), before);
        EXPECT_EQ(selected.items_vector().size(), 1u);
    }
}

TEST_F(ArtworkLibraryDocumentTest, RejectsReferenceCycleAndBoundedAcyclicCloneExpansion)
{
    auto cyclic = document(R"svg(<defs><linearGradient id="a" xlink:href="#b"/><linearGradient id="b" xlink:href="#a"/></defs>
                               <rect id="art" width="20" height="20" fill="url(#a)"/>)svg");
    ASSERT_TRUE(cyclic); ObjectSet selected(cyclic.get()); selected.add(cyclic->getObjectById("art"));
    auto before = fingerprint(*cyclic);
    EXPECT_THROW(Library::stage_selection(selected), std::runtime_error);
    EXPECT_EQ(fingerprint(*cyclic), before);

    auto dag = document(R"svg(<defs><g id="a"><rect width="4" height="4"/></g>
      <g id="b"><use xlink:href="#a"/><use xlink:href="#a" x="5"/></g>
      <g id="c"><use xlink:href="#b"/><use xlink:href="#b" y="5"/></g></defs><use id="art" xlink:href="#c"/>)svg");
    ASSERT_TRUE(dag); ObjectSet clones(dag.get()); clones.add(dag->getObjectById("art"));
    UI::SelectionCopyLimits limits; limits.nodes = 20;
    EXPECT_THROW(Library::stage_selection(clones, limits), std::runtime_error);
}

TEST_F(ArtworkLibraryDocumentTest, CachedDagHeightsEnforceDepthIndependentOfReferenceOrder)
{
    // XML depth is only four. Every ni expands to g/use/n(i-1), so n11's
    // expanded height is 24 and root/art/use/n11/.../n0/rect has height 27.
    // Short-to-long references prime each cache shallowly before its deeper use.
    for (bool short_first : {true, false}) {
        SCOPED_TRACE(short_first);
        std::string body = R"svg(<defs><g id="n0"><rect width="2" height="2"/></g>)svg";
        for (unsigned i = 1; i <= 11; ++i) {
            body += "<g id=\"n" + std::to_string(i) + "\"><use xlink:href=\"#n" +
                    std::to_string(i - 1) + "\"/></g>";
        }
        body += "</defs><g id=\"art\">";
        for (unsigned step = 0; step <= 11; ++step) {
            auto i = short_first ? step : 11 - step;
            body += "<use id=\"u" + std::to_string(i) + "\" x=\"" + std::to_string(4 * i) +
                    "\" xlink:href=\"#n" + std::to_string(i) + "\"/>";
        }
        body += "</g>";
        auto doc = document(body); ASSERT_TRUE(doc);
        ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
        auto before = fingerprint(*doc);
        auto pointers = selected.items_vector();
        auto dirty = doc->isModifiedSinceSave();
        auto sensitive = DocumentUndo::getUndoSensitive(doc.get());
        auto expect_depth_rejection = [&](std::size_t depth, char const *reason) {
            UI::SelectionCopyLimits limits; limits.depth = depth;
            try {
                (void)Library::stage_selection(selected, limits);
                ADD_FAILURE() << "Expanded DAG unexpectedly accepted at depth " << depth;
            } catch (std::runtime_error const &error) {
                EXPECT_STREQ(error.what(), reason); // Not an unrelated byte/node/geometry failure.
            }
            EXPECT_EQ(fingerprint(*doc), before);
            EXPECT_EQ(selected.items_vector(), pointers);
            EXPECT_EQ(doc->isModifiedSinceSave(), dirty);
            EXPECT_EQ(DocumentUndo::getUndoSensitive(doc.get()), sensitive);
        };
        expect_depth_rejection(8, "Resource depth limit exceeded");
        // visit(art, 1) fits at 26. The final root-inclusive expansion must
        // independently reject it, including reuse of a cached subtree height.
        expect_depth_rejection(26, "Expanded reference depth limit exceeded");
        UI::SelectionCopyLimits exact; exact.depth = 27;
        auto staged = Library::stage_selection(selected, exact);
        auto copy = reopen(staged); ASSERT_TRUE(copy);
        for (unsigned i = 0; i <= 11; ++i) {
            auto id = "n" + std::to_string(i);
            EXPECT_EQ(id_count(copy->getReprRoot(), id.c_str()), 1u);
        }
        EXPECT_EQ(fingerprint(*doc), before);
        EXPECT_EQ(selected.items_vector(), pointers);
        EXPECT_EQ(doc->isModifiedSinceSave(), dirty);
        EXPECT_EQ(DocumentUndo::getUndoSensitive(doc.get()), sensitive);
    }
}

TEST_F(ArtworkLibraryDocumentTest, CancellationAtEarlyMiddleAndFinalCheckpointsLeavesSourceAndSelectionExact)
{
    auto doc = document(R"svg(<defs><linearGradient id="g"><stop stop-color="red"/></linearGradient></defs>
      <g id="art"><rect width="25" height="15" fill="url(#g)"/><text y="30" font-size="10">text</text></g>)svg");
    ASSERT_TRUE(doc); ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto before = fingerprint(*doc); auto pointers = selected.items_vector(); auto dirty = doc->isModifiedSinceSave();
    std::size_t calls = 0;
    ASSERT_NO_THROW(Library::stage_selection(selected, {}, [&] { ++calls; return false; }));
    ASSERT_GT(calls, 3u);
    for (auto stop : {std::size_t{1}, calls / 2, calls - 1, calls}) {
        SCOPED_TRACE(stop); std::size_t current = 0;
        EXPECT_THROW(Library::stage_selection(selected, {}, [&] { return ++current == stop; }), UI::SelectionCopyCancelled);
        EXPECT_EQ(fingerprint(*doc), before);
        EXPECT_EQ(doc->isModifiedSinceSave(), dirty);
        EXPECT_EQ(selected.items_vector(), pointers);
    }
}

TEST_F(ArtworkLibraryDocumentTest, NodeReferenceDepthAndByteBudgetsRejectWithoutPublication)
{
    auto doc = document(R"svg(<defs><linearGradient id="g"><stop stop-color="red"/></linearGradient></defs>
      <g><g><rect id="art" width="25" height="15" fill="url(#g)"/></g></g>)svg");
    ASSERT_TRUE(doc); ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto before = fingerprint(*doc);
    for (unsigned which = 0; which < 4; ++which) {
        UI::SelectionCopyLimits limits;
        if (which == 0) limits.nodes = 1;
        if (which == 1) limits.references = 0;
        if (which == 2) limits.depth = 1;
        if (which == 3) limits.svg_bytes = 64;
        EXPECT_THROW(Library::stage_selection(selected, limits), std::runtime_error);
        EXPECT_EQ(fingerprint(*doc), before);
    }
}

TEST_F(ArtworkLibraryDocumentTest, PreservesNamespacesAndMetadataRegardlessOfExportCleanupPreferences)
{
    auto prefs = Preferences::get(); auto temporary = prefs->temporaryPreferences();
    auto doc = document(R"svg(<g id="art" inkscape:label="Library asset" inkscape:nesting-contour-version="1" data-vacards="retain &amp; escape">
      <!--retain comment--><title>Editable title</title><desc>Long &lt;description&gt;</desc>
      <path id="contour" inkscape:label="Nesting contour" inkscape:nesting-contour="true" d="M0,0H20V20H0Z"/></g>)svg");
    ASSERT_TRUE(doc); ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto before = fingerprint(*doc);
    prefs->setBool("/options/svgoutput/check_on_writing", true);
    prefs->setBool("/options/svgoutput/sort_attributes", true);
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    EXPECT_EQ(fingerprint(*doc), before);
    auto repr = copy->getObjectById("art")->getRepr();
    EXPECT_STREQ(repr->attribute("data-vacards"), "retain & escape");
    EXPECT_STREQ(repr->attribute("inkscape:label"), "Library asset");
    EXPECT_STREQ(repr->attribute("inkscape:nesting-contour-version"), "1");
    EXPECT_STREQ(copy->getObjectById("contour")->getRepr()->attribute("inkscape:nesting-contour"), "true");
    EXPECT_STREQ(copy->getObjectById("contour")->getRepr()->attribute("d"), "M0,0H20V20H0Z");
    std::string xml(staged.svg.begin(), staged.svg.end());
    EXPECT_NE(xml.find("retain comment"), xml.npos);
    EXPECT_NE(xml.find("xmlns:inkscape="), xml.npos);
}

TEST_F(ArtworkLibraryDocumentTest, DetachedDocumentAndStagedBytesOutliveSource)
{
    auto doc = document(R"svg(<rect id="art" width="25" height="15"/>)svg"); ASSERT_TRUE(doc);
    Library::StagedSelection staged;
    UI::DetachedSelection detached;
    {
        ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
        detached = UI::copy_selection_detached(selected);
        staged = Library::stage_selection(selected);
    }
    doc.reset();
    ASSERT_TRUE(detached.document->getObjectById("art"));
    auto copy = reopen(staged); ASSERT_TRUE(copy);
    EXPECT_TRUE(copy->getObjectById("art"));
}

TEST_F(ArtworkLibraryDocumentTest, EmptyAndZeroExtentSelectionFailExplicitly)
{
    auto doc = document(R"svg(<rect id="zero" width="0" height="10"/>)svg"); ASSERT_TRUE(doc);
    ObjectSet selected(doc.get());
    EXPECT_THROW(Library::stage_selection(selected), std::runtime_error);
    selected.add(doc->getObjectById("zero"));
    EXPECT_THROW(Library::stage_selection(selected), std::runtime_error);
}

TEST_F(ArtworkLibraryDocumentTest, SerializedEscapingBudgetAcceptsExactBoundaryAndRejectsOneByteLess)
{
    std::string text;
    for (unsigned i = 0; i < 2000; ++i) text += "&amp;";
    auto doc = document("<g id=\"art\"><desc>" + text + "</desc><rect width=\"20\" height=\"20\"/></g>");
    ASSERT_TRUE(doc); ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto original = Library::stage_selection(selected);
    UI::SelectionCopyLimits limits; limits.svg_bytes = original.svg.size();
    auto exact = Library::stage_selection(selected, limits);
    EXPECT_EQ(exact.svg, original.svg);
    --limits.svg_bytes;
    auto before = fingerprint(*doc);
    EXPECT_THROW(Library::stage_selection(selected, limits), std::runtime_error);
    EXPECT_EQ(fingerprint(*doc), before);
}

std::string png_uri(int width = 4, int height = 3)
{
    auto raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, width, height);
    if (!raw) throw std::runtime_error("Cannot allocate test bitmap");
    Pixbuf pixels(raw);
    gdk_pixbuf_fill(raw, 0x33669980);
    gdk_pixbuf_set_option(raw, "x-dpi", "300");
    gdk_pixbuf_set_option(raw, "y-dpi", "301");
    auto uri = sp_image_encode_png_data_uri(pixels);
    if (!uri) throw std::runtime_error("Cannot encode test bitmap");
    return *uri;
}

struct TempBitmap {
    std::string directory, path;
    explicit TempBitmap(std::string const &uri)
    {
        auto dir = g_dir_make_tmp("valib-stage-XXXXXX", nullptr);
        if (!dir) throw std::runtime_error("Cannot create test directory");
        directory = dir; g_free(dir);
        path = directory + "/linked.png";
        gsize size = 0;
        auto bytes = g_base64_decode(uri.c_str() + uri.find(',') + 1, &size);
        auto ok = g_file_set_contents(path.c_str(), reinterpret_cast<char const *>(bytes), size, nullptr);
        g_free(bytes);
        if (!ok) { g_rmdir(directory.c_str()); throw std::runtime_error("Cannot write test bitmap"); }
    }
    ~TempBitmap() { g_unlink(path.c_str()); g_rmdir(directory.c_str()); }
};

TEST_F(ArtworkLibraryDocumentTest, LinkedBitmapBecomesSelfContainedWithoutReloadingOrChangingSourceHref)
{
    TempBitmap file(png_uri());
    auto uri = g_filename_to_uri(file.path.c_str(), nullptr, nullptr); ASSERT_TRUE(uri);
    auto doc = document("<image id=\"art\" width=\"32\" height=\"24\" xlink:href=\"" + std::string(uri) + "\"/>");
    g_free(uri); ASSERT_TRUE(doc);
    auto source = cast<SPImage>(doc->getObjectById("art")); ASSERT_TRUE(source); ASSERT_FALSE(source->missing);
    ObjectSet selected(doc.get()); selected.add(source);
    auto before = fingerprint(*doc);
    auto staged = Library::stage_selection(selected);
    EXPECT_EQ(fingerprint(*doc), before);
    ASSERT_EQ(g_unlink(file.path.c_str()), 0); // Reopen must not require the linked file.
    auto copy = reopen(staged); ASSERT_TRUE(copy);
    auto image = cast<SPImage>(copy->getObjectById("art")); ASSERT_TRUE(image); ASSERT_FALSE(image->missing);
    ASSERT_TRUE(image->pixbuf); EXPECT_EQ(image->pixbuf->width(), 4); EXPECT_EQ(image->pixbuf->height(), 3);
    EXPECT_TRUE(std::string_view(image->href).starts_with("data:image/png;base64,"));
    EXPECT_EQ(image->getRepr()->attribute("sodipodi:absref"), nullptr);
    Pixbuf result_pixels(*image->pixbuf);
    auto raw = result_pixels.getPixbufRaw();
    EXPECT_STREQ(gdk_pixbuf_get_option(raw, "x-dpi"), "300");
    EXPECT_STREQ(gdk_pixbuf_get_option(raw, "y-dpi"), "301");
    EXPECT_EQ(gdk_pixbuf_get_pixels(raw)[3], 128);
}

TEST_F(ArtworkLibraryDocumentTest, BitmapIccDensityAndAlphaUseMetadataAwareEncoder)
{
    auto uri = png_uri();
    auto doc = document("<image id=\"art\" width=\"32\" height=\"24\" xlink:href=\"" + uri + "\"/>");
    ASSERT_TRUE(doc); auto source = cast<SPImage>(doc->getObjectById("art")); ASSERT_TRUE(source);
    auto pixels = std::make_shared<Pixbuf>(*source->pixbuf);
    auto raw = pixels->getPixbufRaw();
    gchar *profile = nullptr; gsize size = 0;
    auto path = std::string(INKSCAPE_TESTS_DIR) + "/data/colors/display.icc";
    ASSERT_TRUE(g_file_get_contents(path.c_str(), &profile, &size, nullptr));
    auto encoded = g_base64_encode(reinterpret_cast<guchar const *>(profile), size); g_free(profile);
    ASSERT_TRUE(encoded);
    std::string expected_profile(encoded); g_free(encoded);
    gdk_pixbuf_set_option(raw, "icc-profile", expected_profile.c_str());
    pixels->ensurePixelFormat(Pixbuf::PF_CAIRO);
    auto pixel_size = static_cast<std::size_t>(pixels->rowstride()) * pixels->height();
    std::vector<unsigned char> original_pixels(pixels->pixels(), pixels->pixels() + pixel_size);
    // A private fixture pixbuf models the existing metadata-bearing native image.
    source->pixbuf = pixels;
    ObjectSet selected(doc.get()); selected.add(source);
    auto before = fingerprint(*doc);
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    EXPECT_EQ(fingerprint(*doc), before);
    EXPECT_EQ(source->pixbuf->pixelFormat(), Pixbuf::PF_CAIRO);
    EXPECT_EQ(std::memcmp(source->pixbuf->pixels(), original_pixels.data(), pixel_size), 0);
    auto image = cast<SPImage>(copy->getObjectById("art")); ASSERT_TRUE(image); ASSERT_FALSE(image->missing);
    Pixbuf result_pixels(*image->pixbuf);
    auto result = result_pixels.getPixbufRaw();
    ASSERT_NE(gdk_pixbuf_get_option(result, "icc-profile"), nullptr);
    EXPECT_STREQ(gdk_pixbuf_get_option(result, "icc-profile"), expected_profile.c_str());
    EXPECT_STREQ(gdk_pixbuf_get_option(result, "x-dpi"), "300");
    EXPECT_STREQ(gdk_pixbuf_get_option(result, "y-dpi"), "301");
    EXPECT_EQ(gdk_pixbuf_get_pixels(result)[3], 128);
}

TEST_F(ArtworkLibraryDocumentTest, PixelBudgetsAndMissingBitmapRejectBeforePublication)
{
    auto uri = png_uri();
    auto doc = document("<g id=\"art\"><image id=\"a\" width=\"32\" height=\"24\" xlink:href=\"" + uri +
                        "\"/><image id=\"b\" x=\"40\" width=\"32\" height=\"24\" xlink:href=\"" + uri + "\"/></g>");
    ASSERT_TRUE(doc); ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto before = fingerprint(*doc);
    UI::SelectionCopyLimits limits; limits.image_pixels = 11;
    EXPECT_THROW(Library::stage_selection(selected, limits), std::runtime_error);
    limits.image_pixels = 12; limits.total_image_pixels = 23;
    EXPECT_THROW(Library::stage_selection(selected, limits), std::runtime_error);
    limits.total_image_pixels = 24;
    EXPECT_NO_THROW(Library::stage_selection(selected, limits));
    auto image = cast<SPImage>(doc->getObjectById("a")); ASSERT_TRUE(image);
    image->missing = true; // Pixel presence must not make a placeholder eligible.
    EXPECT_THROW(Library::stage_selection(selected), std::runtime_error);
    EXPECT_EQ(fingerprint(*doc), before);
}

TEST_F(ArtworkLibraryDocumentTest, SvgImageIsNotSilentlyRasterized)
{
    auto svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\"><rect width=\"20\" height=\"20\"/></svg>";
    auto encoded = g_base64_encode(reinterpret_cast<guchar const *>(svg), std::strlen(svg));
    auto body = "<image id=\"art\" width=\"20\" height=\"20\" xlink:href=\"data:image/svg+xml;base64," + std::string(encoded) + "\"/>";
    g_free(encoded);
    auto doc = document(body); ASSERT_TRUE(doc);
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto before = fingerprint(*doc);
    EXPECT_THROW(Library::stage_selection(selected), std::runtime_error);
    EXPECT_EQ(fingerprint(*doc), before);
}

TEST_F(ArtworkLibraryDocumentTest, MissingFontIsReportedWithoutOutliningEditableText)
{
    auto doc = document(R"svg(<text id="art" x="2" y="25" font-family="VACards-Definitely-Missing-Family-7e61" font-size="15">Editable</text>)svg");
    ASSERT_TRUE(doc); ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    EXPECT_TRUE(cast<SPText>(copy->getObjectById("art")));
    EXPECT_TRUE(std::any_of(staged.warnings.begin(), staged.warnings.end(), [](auto const &warning) {
        return warning.find("VACards-Definitely-Missing-Family-7e61") != warning.npos;
    }));
}

TEST_F(ArtworkLibraryDocumentTest, GroupAndSelectedDescendantDoNotDuplicateIds)
{
    auto doc = document(R"svg(<g id="art"><rect id="child" width="20" height="15"/></g>)svg"); ASSERT_TRUE(doc);
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art")); selected.add(doc->getObjectById("child"));
    auto staged = Library::stage_selection(selected); auto copy = reopen(staged); ASSERT_TRUE(copy);
    EXPECT_EQ(id_count(copy->getReprRoot(), "art"), 1u);
    EXPECT_EQ(id_count(copy->getReprRoot(), "child"), 1u);
}

} // namespace
