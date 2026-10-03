// SPDX-License-Identifier: GPL-2.0-or-later
// Native editing oracles for explicit shape-inside:none, not a UI event test.
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <2geom/transforms.h>
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "preferences.h"
#include "style.h"
#include "svg/svg.h"
#include "text-editing.h"
#include "object/sp-root.h"
#include "object/sp-text.h"
#include "object/sp-tspan.h"
#include "object/sp-textpath.h"
#include "object/sp-flowtext.h"
#include "xml/repr.h"
#include "xml/attribute-record.h"
#include "display/drawing.h"
#include "display/drawing-item.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "display/cairo-utils.h"

using namespace Inkscape;
namespace {
struct BoolPreference {
    Preferences *prefs = Preferences::get();
    char const *key;
    Preferences::Entry old;
    BoolPreference(char const *key, bool value) : key(key), old(prefs->getEntry(key)) {
        prefs->setBool(key, value);
    }
    ~BoolPreference() {
        if (old.isSet()) prefs->setString(key, old.getString());
        else prefs->remove(key);
    }
};

std::unique_ptr<SPDocument> parse(std::string const &xml)
{
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
    if (!doc) throw std::runtime_error("Native text fixture failed");
    doc->ensureUpToDate();
    DocumentUndo::done(doc.get(), Util::Internal::ContextString("Fixture"), "");
    DocumentUndo::clearUndo(doc.get()); DocumentUndo::clearRedo(doc.get());
    doc->setModifiedSinceSave(false);
    return doc;
}

std::unique_ptr<SPDocument> fixture(bool path, bool neutral, bool important)
{
    std::string p = important ? "!important" : "";
    auto rules = ".shield{font-size:31px" + p + ";fill:lime" + p + ";" +
        (neutral ? "shape-inside:url(#frame)" + p + ";" : "") + "}";
    auto style = "font-family:sans-serif;font-size:12px" + p + ";fill:#24579b" + p +
        ";line-height:18px" + p + ";letter-spacing:1px" + p + ";word-spacing:2px" + p +
        (neutral ? ";shape-inside:none" + p : "");
    std::string content = "<tspan id='run' dx='2' dy='1'>Native</tspan>";
    if (path) content = "<textPath id='pathrun' xlink:href='#baseline'>" + content + "</textPath>";
    return parse("<svg xmlns='http://www.w3.org/2000/svg' "
        "xmlns:xlink='http://www.w3.org/1999/xlink' width='320' height='200'>"
        "<defs><rect id='frame' x='170' y='110' width='120' height='70'/></defs>"
        "<style>" + rules + "</style>"
        "<path id='baseline' d='M12 45H250' style='fill:none;stroke:none'/>"
        "<text id='art' class='shield' x='12' y='45' transform='rotate(7)' style='" +
        style + "'>" + content + "</text></svg>");
}

SPText &text(SPDocument &doc) {
    auto t = cast<SPText>(doc.getObjectById("art"));
    if (!t) throw std::runtime_error("Missing native text");
    return *t;
}
SPTSpan &run(SPDocument &doc) {
    auto t = cast<SPTSpan>(doc.getObjectById("run"));
    if (!t) throw std::runtime_error("Missing inherited run");
    return *t;
}

void snapshot(XML::Node const *node, std::string &out, bool canonical_attributes)
{
    auto field = [&](char const *v) {
        if (!v) { out += "-;"; return; }
        out += std::to_string(std::strlen(v)) + ":" + v;
    };
    out += "["; field(node->name()); field(node->content());
    // XML Undo records attribute names/values, not attribute-list positions.
    // Preserve all value bytes (including CSS priority), content and child order.
    std::vector<std::pair<std::string, char const *>> attributes;
    for (auto const &a : node->attributeList())
        attributes.emplace_back(g_quark_to_string(a.key), static_cast<char const *>(a.value));
    if (canonical_attributes)
        std::sort(attributes.begin(), attributes.end(), [](auto const &a, auto const &b) {
            return a.first < b.first;
        });
    for (auto const &[key, value] : attributes) { field(key.c_str()); field(value); }
    out += ";";
    for (auto c = node->firstChild(); c; c = c->next()) snapshot(c, out, canonical_attributes);
    out += "]";
}
std::string snapshot(SPDocument &doc, bool canonical_attributes = true) {
    std::string out; snapshot(doc.getReprDoc(), out, canonical_attributes); return out;
}

Cairo::RefPtr<Cairo::ImageSurface> render(SPDocument &doc, double zoom = 1.)
{
    Drawing drawing;
    auto root = doc.getRoot();
    auto key = SPItem::display_key_new(1);
    auto shown = root->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY);
    drawing.setRoot(shown);
    shown->setTransform(Geom::Scale(zoom) * Geom::Translate(8.25, 9.375));
    drawing.update();
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 640, 400);
    DrawingSurface target(surface->cobj(), Geom::IntPoint(0, 0));
    DrawingContext context(target);
    drawing.render(context, Geom::IntRect::from_xywh(0, 0, 640, 400));
    surface->flush(); root->invoke_hide(key);
    return surface;
}
std::size_t differences(Cairo::RefPtr<Cairo::ImageSurface> const &a,
                        Cairo::RefPtr<Cairo::ImageSurface> const &b)
{
    std::size_t count = 0;
    for (int y = 0; y < a->get_height(); ++y) for (int x = 0; x < a->get_width(); ++x)
        count += std::memcmp(a->get_data() + y*a->get_stride() + 4*x,
                             b->get_data() + y*b->get_stride() + 4*x, 4) != 0;
    return count;
}
void same_native(SPDocument &a, SPDocument &b)
{
    auto lhs = text(a).layout.bounds(text(a).i2doc_affine());
    auto rhs = text(b).layout.bounds(text(b).i2doc_affine());
    ASSERT_TRUE(lhs); ASSERT_TRUE(rhs);
    ASSERT_GT(lhs->width(), 0.); ASSERT_GT(lhs->height(), 0.);
    for (auto axis : {Geom::X, Geom::Y}) {
        EXPECT_NEAR(lhs->min()[axis], rhs->min()[axis], 1e-9);
        EXPECT_NEAR(lhs->max()[axis], rhs->max()[axis], 1e-9);
    }
    for (double zoom : {.75, 1., 1.375}) {
        SCOPED_TRACE(zoom);
        auto expected = render(a, zoom), actual = render(b, zoom);
        auto ink = [](auto const &surface) {
            std::size_t pixels = 0;
            for (int y = 0; y < surface->get_height(); ++y)
                for (int x = 0; x < surface->get_width(); ++x) {
                    std::uint32_t pixel;
                    std::memcpy(&pixel, surface->get_data() + y*surface->get_stride() + 4*x, 4);
                    pixels += (pixel >> 24) != 0;
                }
            return pixels;
        };
        ASSERT_GT(ink(expected), 0u); ASSERT_GT(ink(actual), 0u);
        EXPECT_EQ(differences(expected, actual), 0u);
    }
}
void shield(SPDocument &doc, bool important)
{
    auto &t = text(doc);
    EXPECT_TRUE(t.style->shape_inside.set);
    EXPECT_FALSE(t.has_shape_inside());
    EXPECT_STREQ(t.style->shape_inside.value(), "none");
    EXPECT_EQ(t.style->shape_inside.important, important);
    EXPECT_EQ(t.style->font_size.important, important);
    EXPECT_EQ(t.style->fill.important, important);
}
class TextShapeNoneEditing : public ::testing::Test {
    void SetUp() override { if (!Application::exists()) Application::create(false); }
};
}

TEST_F(TextShapeNoneEditing, ContentScaleBakesNormalAndNeutralTextIdentically)
{
    BoolPreference svg2("/tools/text/use_svg2", true);
    BoolPreference preserve("/options/preservetransform/value", false);
    for (bool path : {false, true}) for (bool important : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "path=" << path << " important=" << important);
        auto normal = fixture(path, false, important);
        auto neutral = fixture(path, true, important);
        same_native(*normal, *neutral); shield(*neutral, important);
        auto before_normal = snapshot(*normal), before_neutral = snapshot(*neutral);
        auto raw_normal = snapshot(*normal, false), raw_neutral = snapshot(*neutral, false);
        auto before_pixels = render(*neutral);
        auto initial_font = text(*neutral).style->font_size.computed;
        ASSERT_DOUBLE_EQ(initial_font, 12.);
        auto inherited_font = run(*neutral).style->font_size.computed;
        auto initial_dx = run(*neutral).attributes.getDx(0);
        auto initial_dy = run(*neutral).attributes.getDy(0);
        EXPECT_FALSE(run(*neutral).style->font_size.set); // Actual inherited metrics.
        unsigned commits = 0;
        auto connection = neutral->connectCommit([&] { ++commits; });
        normal->scaleContentBy(Geom::Scale(2));
        neutral->scaleContentBy(Geom::Scale(2));
        normal->ensureUpToDate(); neutral->ensureUpToDate();
        EXPECT_DOUBLE_EQ(text(*neutral).style->font_size.computed, initial_font * 2);
        EXPECT_DOUBLE_EQ(run(*neutral).style->font_size.computed, inherited_font * 2);
        EXPECT_DOUBLE_EQ(run(*neutral).attributes.getDx(0), initial_dx * 2);
        EXPECT_DOUBLE_EQ(run(*neutral).attributes.getDy(0), initial_dy * 2);
        if (path) EXPECT_FALSE(text(*neutral)._optimizeTextpathText); // One-shot request consumed.
        same_native(*normal, *neutral); shield(*neutral, important);
        EXPECT_GT(differences(before_pixels, render(*neutral)), 0u);
        DocumentUndo::done(normal.get(), Util::Internal::ContextString("Scale content"), "");
        DocumentUndo::done(neutral.get(), Util::Internal::ContextString("Scale content"), "");
        EXPECT_EQ(commits, 1u); connection.disconnect();
        auto after_normal = snapshot(*normal), after_neutral = snapshot(*neutral);
        auto reopened = parse(sp_repr_save_buf(neutral->getReprDoc()).raw());
        shield(*reopened, important); same_native(*neutral, *reopened);
        EXPECT_TRUE(DocumentUndo::undo(normal.get())); EXPECT_TRUE(DocumentUndo::undo(neutral.get()));
        EXPECT_EQ(snapshot(*normal), before_normal); EXPECT_EQ(snapshot(*neutral), before_neutral);
        if (path) {
            // Preserve the original order-only observation as evidence, not as
            // a required nonsemantic attribute-order behavior for future Undo.
            auto suffix = important ? "_important" : "_normal";
            RecordProperty(std::string("textpath_undo_attribute_order_control") + suffix,
                snapshot(*normal, false) == raw_normal ? "unchanged" : "changed");
            RecordProperty(std::string("textpath_undo_attribute_order_neutral") + suffix,
                snapshot(*neutral, false) == raw_neutral ? "unchanged" : "changed");
        }
        EXPECT_EQ(differences(before_pixels, render(*neutral)), 0u);
        EXPECT_TRUE(DocumentUndo::redo(normal.get())); EXPECT_TRUE(DocumentUndo::redo(neutral.get()));
        EXPECT_EQ(snapshot(*normal), after_normal); EXPECT_EQ(snapshot(*neutral), after_neutral);
        same_native(*normal, *neutral); shield(*neutral, important);
    }
}

TEST_F(TextShapeNoneEditing, OrdinaryTextpathTransformDoesNotBakeWithoutExplicitRequest)
{
    BoolPreference svg2("/tools/text/use_svg2", true);
    BoolPreference preserve("/options/preservetransform/value", false);
    for (bool important : {false, true}) {
        SCOPED_TRACE(important);
        auto normal = fixture(true, false, important), neutral = fixture(true, true, important);
        auto initial_font = text(*neutral).style->font_size.computed;
        for (auto doc : {normal.get(), neutral.get()}) {
            auto &t = text(*doc);
            ASSERT_FALSE(t._optimizeTextpathText);
            t.doWriteTransform(t.transform * Geom::Scale(1.5), nullptr, true);
            doc->ensureUpToDate();
            EXPECT_DOUBLE_EQ(t.style->font_size.computed, initial_font);
            EXPECT_FALSE(t._optimizeTextpathText);
        }
        same_native(*normal, *neutral); shield(*neutral, important);
    }
}

TEST_F(TextShapeNoneEditing, NeutralKerningEligibilityAndNativeOffsetsRotationUndo)
{
    for (bool path : {false, true}) for (bool important : {false, true})
    for (unsigned operation = 0; operation != 3; ++operation) {
        SCOPED_TRACE(::testing::Message() << "path=" << path << " important=" << important
                                         << " operation=" << operation);
        auto normal = fixture(path, false, important), neutral = fixture(path, true, important);
        ASSERT_TRUE(is_kerning_supported(&text(*normal)));
        ASSERT_TRUE(is_kerning_supported(&text(*neutral)));
        auto before_normal = snapshot(*normal), before_neutral = snapshot(*neutral);
        auto before_pixels = render(*neutral);
        unsigned commits = 0;
        auto connection = neutral->connectCommit([&] { ++commits; });
        for (auto doc : {normal.get(), neutral.get()}) {
            auto &t = text(*doc);
            auto pos = t.layout.begin(); pos.nextCharacter();
            if (operation == 0) sp_te_adjust_dx(&t, pos, pos, nullptr, 4);
            if (operation == 1) sp_te_adjust_dy(&t, pos, pos, nullptr, 3);
            if (operation == 2) sp_te_adjust_rotation(&t, pos, pos, nullptr, 15);
            doc->ensureUpToDate();
            if (operation == 0) EXPECT_DOUBLE_EQ(run(*doc).attributes.getDx(1), 4);
            if (operation == 1) EXPECT_DOUBLE_EQ(run(*doc).attributes.getDy(1), 3);
            if (operation == 2) EXPECT_DOUBLE_EQ(run(*doc).attributes.getRotate(1), 15);
            DocumentUndo::done(doc, Util::Internal::ContextString("Edit spacing"), "");
        }
        EXPECT_EQ(commits, 1u); connection.disconnect();
        same_native(*normal, *neutral); shield(*neutral, important);
        EXPECT_GT(differences(before_pixels, render(*neutral)), 0u);
        auto after_normal = snapshot(*normal), after_neutral = snapshot(*neutral);
        auto reopened = parse(sp_repr_save_buf(neutral->getReprDoc()).raw());
        same_native(*neutral, *reopened); shield(*reopened, important);
        EXPECT_TRUE(DocumentUndo::undo(normal.get())); EXPECT_TRUE(DocumentUndo::undo(neutral.get()));
        EXPECT_EQ(snapshot(*normal), before_normal); EXPECT_EQ(snapshot(*neutral), before_neutral);
        EXPECT_EQ(differences(before_pixels, render(*neutral)), 0u);
        EXPECT_TRUE(DocumentUndo::redo(normal.get())); EXPECT_TRUE(DocumentUndo::redo(neutral.get()));
        EXPECT_EQ(snapshot(*normal), after_normal); EXPECT_EQ(snapshot(*neutral), after_neutral);
        same_native(*normal, *neutral); shield(*neutral, important);
    }
}

TEST_F(TextShapeNoneEditing, RealMissingAndInheritedShapeDeclarationsKeepKerningPolicy)
{
    EXPECT_FALSE(is_kerning_supported(nullptr));
    for (auto value : {"url(#frame)", "url(#missing)", "inherit"}) {
        SCOPED_TRACE(value);
        auto doc = fixture(false, true, true);
        auto &t = text(*doc);
        auto css = std::string("font-size:12px;shape-inside:") + value + "!important";
        t.getRepr()->setAttribute("style", css.c_str()); doc->ensureUpToDate();
        EXPECT_TRUE(t.has_shape_inside());
        EXPECT_FALSE(is_kerning_supported(&t));
    }
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg'>"
        "<flowRoot id='legacy'><flowRegion><rect width='120' height='60'/></flowRegion>"
        "<flowPara>Legacy</flowPara></flowRoot></svg>");
    auto legacy = cast<SPFlowtext>(doc->getObjectById("legacy")); ASSERT_TRUE(legacy);
    // Preserve the existing non-SPText behavior, not a new legacy-flow policy.
    EXPECT_EQ(is_kerning_supported(legacy), !(legacy->style && legacy->style->shape_inside.set));
}

TEST_F(TextShapeNoneEditing, EnterAtStyledRunEndKeepsCharacterStyleAndUndoXml)
{
    for (bool flowed : {false, true}) {
        SCOPED_TRACE(flowed ? "flowed text" : "point text");
        auto doc = parse(flowed
            ? "<svg xmlns='http://www.w3.org/2000/svg' width='320' height='200'>"
              "<flowRoot id='art' style='font-family:serif;font-size:12px;fill:#000000'>"
              "<flowRegion><rect width='280' height='180'/></flowRegion>"
              "<flowPara id='line'>Base <flowSpan id='hot' style='font-size:28px;fill:#d02040;font-weight:bold;letter-spacing:2px'>HOT</flowSpan></flowPara>"
              "</flowRoot></svg>"
            : "<svg xmlns='http://www.w3.org/2000/svg' width='320' height='200'>"
              "<text id='art' x='10' y='50' style='font-family:serif;font-size:12px;fill:#000000'>"
              "<tspan id='line' sodipodi:role='line' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'>"
              "Base <tspan id='hot' style='font-size:28px;fill:#d02040;font-weight:bold;letter-spacing:2px'>HOT</tspan>"
              "</tspan></text></svg>");
        auto *art = cast<SPItem>(doc->getObjectById("art"));
        ASSERT_TRUE(art);
        auto const before = snapshot(*doc);
        auto const *layout = te_get_layout(art);
        auto previous = layout->end();
        ASSERT_TRUE(previous.prevCharacter());
        auto const *expected = sp_te_style_at_position(art, previous);
        ASSERT_TRUE(expected);
        auto const expected_size = expected->font_size.computed;
        auto const expected_fill = expected->fill.getColor().toRGBA();
        auto const expected_weight = expected->font_weight.computed;
        auto const expected_spacing = expected->letter_spacing.computed;
        ASSERT_DOUBLE_EQ(expected_size, 28.);

        auto caret = layout->end();
        caret = sp_te_insert_line(art, caret, true);
        caret = sp_te_insert(art, caret, "Z");
        doc->ensureUpToDate();
        auto *line = doc->getObjectById("line");
        ASSERT_TRUE(line);
        auto *new_line = line->getNext();
        ASSERT_TRUE(new_line);
        ASSERT_TRUE(new_line->firstChild()) << "new paragraph must retain the styled run";
        ASSERT_TRUE(new_line->firstChild()->firstChild());
        EXPECT_STREQ(new_line->firstChild()->firstChild()->getRepr()->content(), "Z");
        auto const *style = new_line->firstChild()->getRepr()->attribute("style");
        ASSERT_TRUE(style);
        EXPECT_NE(std::string(style).find("font-size:28px"), std::string::npos);
        EXPECT_NE(std::string(style).find("fill:#d02040"), std::string::npos);
        auto const *actual = new_line->firstChild()->style;
        ASSERT_TRUE(actual);
        EXPECT_DOUBLE_EQ(actual->font_size.computed, expected_size);
        EXPECT_EQ(actual->fill.getColor().toRGBA(), expected_fill);
        EXPECT_EQ(actual->font_weight.computed, expected_weight);
        EXPECT_DOUBLE_EQ(actual->letter_spacing.computed, expected_spacing);
        DocumentUndo::done(doc.get(), Util::Internal::ContextString("Enter and type"), "draw-text");
        ASSERT_TRUE(DocumentUndo::undo(doc.get()));
        EXPECT_EQ(snapshot(*doc), before);
        EXPECT_FALSE(DocumentUndo::undo(doc.get()));
    }
}

TEST_F(TextShapeNoneEditing, EnterInMiddleOfStyledRunRetainsBothStyles)
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='320' height='200'>"
        "<text id='art' x='10' y='50' style='font-size:12px;fill:#000000'>"
        "<tspan id='line' sodipodi:role='line' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'>"
        "<tspan id='hot' style='font-size:28px;fill:#d02040'>HOT</tspan></tspan></text></svg>");
    auto *art = cast<SPText>(doc->getObjectById("art"));
    ASSERT_TRUE(art);
    auto caret = art->layout.charIndexToIterator(1);
    caret = sp_te_insert_line(art, caret);
    caret = sp_te_insert(art, caret, "Z");
    doc->ensureUpToDate();
    auto *line = doc->getObjectById("line");
    ASSERT_TRUE(line);
    auto *new_line = line->getNext();
    ASSERT_TRUE(new_line);
    ASSERT_TRUE(new_line->firstChild());
    ASSERT_TRUE(new_line->firstChild()->getRepr()->attribute("style"));
    EXPECT_STREQ(new_line->firstChild()->getRepr()->attribute("style"),
                 line->firstChild()->getRepr()->attribute("style"));
    auto *inserted_style = new_line->firstChild()->style;
    ASSERT_TRUE(inserted_style);
    EXPECT_DOUBLE_EQ(inserted_style->font_size.computed, 28.);
    EXPECT_EQ(inserted_style->fill.getColor().toRGBA(), 0xd02040ffu);
}

TEST_F(TextShapeNoneEditing, RepeatedEnterKeepsEmptyRunStyle)
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='320' height='200'>"
        "<text id='art' x='10' y='50' style='font-size:12px;fill:#000000'>"
        "<tspan id='line' sodipodi:role='line' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'>"
        "<tspan style='font-size:28px;fill:#d02040'>HOT</tspan></tspan></text></svg>");
    auto *art = cast<SPText>(doc->getObjectById("art"));
    ASSERT_TRUE(art);
    auto caret = art->layout.end();
    caret = sp_te_insert_line(art, caret, true);
    caret = sp_te_insert_line(art, caret, true);
    caret = sp_te_insert(art, caret, "Z");
    doc->ensureUpToDate();
    auto *third = doc->getObjectById("line")->getNext()->getNext();
    ASSERT_TRUE(third);
    ASSERT_TRUE(third->firstChild());
    EXPECT_STREQ(third->firstChild()->getRepr()->attribute("style"), "font-size:28px;fill:#d02040");
    EXPECT_STREQ(third->firstChild()->firstChild()->getRepr()->content(), "Z");
}

TEST_F(TextShapeNoneEditing, PasteParagraphBreakDoesNotCarryPreviousRunStyle)
{
    // The HTML fragment <p>Title <span style="font-size:28px;color:red">HOT</span></p><p>body</p>
    // reaches this shared newline operation between the two paragraphs.
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='320' height='200'>"
        "<text id='art' x='10' y='50' style='font-size:12px;fill:#000000'>"
        "<tspan id='line' x='10' y='50' sodipodi:role='line' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'>"
        "Title <tspan style='font-size:28px;fill:#ff0000'>HOT</tspan></tspan></text></svg>");
    auto *art = cast<SPText>(doc->getObjectById("art"));
    ASSERT_TRUE(art);
    auto caret = art->layout.end();
    caret = sp_te_insert_line(art, caret); // Paste keeps the default, unlike Enter.
    caret = sp_te_insert(art, caret, "body");
    doc->ensureUpToDate();
    auto *new_line = doc->getObjectById("line")->getNext();
    ASSERT_TRUE(new_line);
    ASSERT_TRUE(new_line->firstChild());
    EXPECT_STREQ(new_line->firstChild()->getRepr()->content(), "body");
    EXPECT_FALSE(new_line->firstChild()->getRepr()->attribute("style"));
    EXPECT_DOUBLE_EQ(new_line->style->font_size.computed, 12.);
    EXPECT_EQ(new_line->style->fill.getColor().toRGBA(), 0x000000ffu);
}

TEST_F(TextShapeNoneEditing, EnterAtTrefEndCreatesLineWithoutDesktop)
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' width='320' height='200'>"
        "<defs><text id='source' x='0' y='20' style='font-family:sans-serif;font-size:20px'>REF</text></defs>"
        "<text id='art' x='10' y='50' style='font-family:sans-serif;font-size:20px'>"
        "<tspan id='line' sodipodi:role='line' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'>"
        "<tref xlink:href='#source'/></tspan></text></svg>");
    auto *art = cast<SPText>(doc->getObjectById("art"));
    ASSERT_TRUE(art);
    ASSERT_EQ(sp_te_get_string_multiline(art, art->layout.begin(), art->layout.end()), "REF");
    auto caret = art->layout.end();
    caret = sp_te_insert_line(art, caret, true);
    doc->ensureUpToDate();
    EXPECT_TRUE(doc->getObjectById("line")->getNext());
}

TEST_F(TextShapeNoneEditing, EnterUsesLastVisibleRunAndDoesNotCopyPositions)
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='320' height='200'>"
        "<text id='art' x='10' y='50' style='font-size:12px;fill:#000000'>"
        "<tspan id='line' x='10' y='50' sodipodi:role='line' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'>"
        "<tspan id='visible' style='font-size:28px;fill:#d02040' dx='3' rotate='12'>HOT</tspan>"
        "<tspan id='space' style='font-size:9px;fill:#008000'> </tspan></tspan></text></svg>");
    auto *art = cast<SPText>(doc->getObjectById("art"));
    ASSERT_TRUE(art);
    auto *line = doc->getObjectById("line");
    ASSERT_TRUE(line);
    std::string line_before; snapshot(line->getRepr(), line_before, true);
    auto const before_anchor = art->layout.characterAnchorPoint(art->layout.begin());
    auto caret = art->layout.end();
    caret = sp_te_insert_line(art, caret, true);
    doc->ensureUpToDate();
    auto const after_anchor = art->layout.characterAnchorPoint(art->layout.begin());
    EXPECT_NEAR(after_anchor[Geom::X], before_anchor[Geom::X], 1e-9);
    EXPECT_NEAR(after_anchor[Geom::Y], before_anchor[Geom::Y], 1e-9);
    std::string line_after; snapshot(line->getRepr(), line_after, true);
    EXPECT_EQ(line_after, line_before);
    auto *new_line = line->getNext();
    ASSERT_TRUE(new_line);
    auto *new_span = new_line->firstChild();
    ASSERT_TRUE(new_span);
    EXPECT_STREQ(new_span->getRepr()->attribute("style"), "font-size:28px;fill:#d02040");
    for (auto key : {"x", "y", "dx", "dy", "rotate"}) EXPECT_FALSE(new_span->getRepr()->attribute(key));
    caret = sp_te_insert(art, caret, "Z");
    doc->ensureUpToDate();
    auto saved = sp_repr_save_buf(doc->getReprDoc());
    auto reopened = parse(saved.raw());
    auto *reopened_art = cast<SPText>(reopened->getObjectById("art"));
    ASSERT_TRUE(reopened_art);
    auto *reopened_span = reopened->getObjectById("line")->getNext()->firstChild();
    ASSERT_TRUE(reopened_span);
    for (auto key : {"x", "y", "dx", "dy", "rotate"}) EXPECT_FALSE(reopened_span->getRepr()->attribute(key));
    EXPECT_STREQ(reopened_span->firstChild()->getRepr()->content(), "Z");
    auto *reopened_line = reopened->getObjectById("line");
    std::string reopened_line_xml; snapshot(reopened_line->getRepr(), reopened_line_xml, true);
    EXPECT_EQ(reopened_line_xml, line_before);
    auto const reopened_anchor = reopened_art->layout.characterAnchorPoint(reopened_art->layout.begin());
    EXPECT_NEAR(reopened_anchor[Geom::X], before_anchor[Geom::X], 1e-9);
    EXPECT_NEAR(reopened_anchor[Geom::Y], before_anchor[Geom::Y], 1e-9);
}

TEST_F(TextShapeNoneEditing, EnterStyleSourceFollowsLayoutBeforeHiddenXmlRun)
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='320' height='200'>"
        "<text id='art' x='10' y='50' style='font-size:12px;fill:#000000'>"
        "<tspan id='line' x='10' y='50' sodipodi:role='line' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'>"
        "<tspan id='visible' style='font-size:28px;fill:#d02040'>HOT</tspan>"
        "<tspan id='hidden' style='display:none;font-size:41px;fill:#008000'>HIDDEN</tspan>"
        "</tspan></text></svg>");
    auto *art = cast<SPText>(doc->getObjectById("art"));
    ASSERT_TRUE(art);
    auto *line = doc->getObjectById("line");
    ASSERT_TRUE(line);
    ASSERT_EQ(line->lastChild(), doc->getObjectById("hidden"));
    ASSERT_EQ(sp_te_get_string_multiline(art, art->layout.begin(), art->layout.end()), "HOT");
    auto caret = art->layout.end();
    caret = sp_te_insert_line(art, caret, true);
    doc->ensureUpToDate();
    auto *new_line = line->getNext();
    ASSERT_TRUE(new_line);
    ASSERT_TRUE(new_line->firstChild());
    EXPECT_STREQ(new_line->firstChild()->getRepr()->attribute("style"),
                 "font-size:28px;fill:#d02040");
}

TEST_F(TextShapeNoneEditing, EnterCopiedSpanDropsTextLengthAttributes)
{
    auto doc = parse("<svg xmlns='http://www.w3.org/2000/svg' width='320' height='200'>"
        "<text id='art' x='10' y='50'>"
        "<tspan id='line' sodipodi:role='line' xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'>"
        "<tspan id='run' style='fill:#d02040' textLength='42' lengthAdjust='spacing'>HOT</tspan>"
        "</tspan></text></svg>");
    auto *art = cast<SPText>(doc->getObjectById("art"));
    ASSERT_TRUE(art);
    auto *line = doc->getObjectById("line");
    ASSERT_TRUE(line);
    ASSERT_STREQ(doc->getObjectById("run")->getRepr()->attribute("textLength"), "42");
    auto caret = art->layout.end();
    caret = sp_te_insert_line(art, caret, true);
    doc->ensureUpToDate();
    auto *new_line = line->getNext();
    ASSERT_TRUE(new_line);
    auto *new_span = new_line->firstChild();
    ASSERT_TRUE(new_span);
    EXPECT_STREQ(new_span->getRepr()->attribute("style"), "fill:#d02040");
    EXPECT_FALSE(new_span->getRepr()->attribute("textLength"));
    EXPECT_FALSE(new_span->getRepr()->attribute("lengthAdjust"));
}

TEST_F(TextShapeNoneEditing, GenuineFlowTextLengthAndInlineSizeStillKeepTransform)
{
    BoolPreference svg2("/tools/text/use_svg2", true);
    BoolPreference preserve("/options/preservetransform/value", false);
    auto check = [](SPDocument &doc) {
        auto &t = text(doc);
        auto font = t.style->font_size.computed;
        auto requested = t.transform * Geom::Scale(2);
        // Predict the ordinary native writer's representation BEFORE the edit;
        // do not derive the expectation from the actual output attribute.
        auto expected_xml = sp_svg_transform_write(requested);
        Geom::Affine expected_matrix;
        ASSERT_FALSE(expected_xml.empty());
        ASSERT_TRUE(sp_svg_transform_read(expected_xml.c_str(), &expected_matrix));
        t.doWriteTransform(requested, nullptr, true); doc.ensureUpToDate();
        EXPECT_DOUBLE_EQ(t.style->font_size.computed, font);
        EXPECT_STREQ(t.getRepr()->attribute("transform"), expected_xml.c_str());
        for (unsigned i = 0; i != 6; ++i) EXPECT_DOUBLE_EQ(t.transform[i], expected_matrix[i]);
    };
    for (auto style : {"shape-inside:url(#frame)", "shape-inside:url(#missing)",
                       "shape-inside:inherit", "shape-inside:none;inline-size:100px"}) {
        SCOPED_TRACE(style);
        auto doc = fixture(false, false, false);
        auto &t = text(*doc);
        auto css = std::string("font-size:12px;") + style;
        t.getRepr()->setAttribute("style", css.c_str()); doc->ensureUpToDate();
        check(*doc);
    }
    auto doc = fixture(false, true, true);
    auto &t = text(*doc);
    t.getRepr()->setAttribute("textLength", "90"); doc->ensureUpToDate();
    check(*doc);
    shield(*doc, true);
}

TEST_F(TextShapeNoneEditing, SemanticSnapshotIgnoresOnlyAttributeOrder)
{
    auto doc = fixture(false, true, true);
    auto node = text(*doc).getRepr();
    auto before = snapshot(*doc), raw = snapshot(*doc, false);
    auto transform = std::string(node->attribute("transform"));
    node->setAttribute("transform", nullptr);
    node->setAttribute("transform", transform.c_str());
    EXPECT_NE(snapshot(*doc, false), raw); // Known append-on-readd native behavior.
    EXPECT_EQ(snapshot(*doc), before);

    auto style = std::string(node->attribute("style"));
    auto without_priority = style;
    auto at = without_priority.find("!important"); ASSERT_NE(at, std::string::npos);
    without_priority.erase(at, std::strlen("!important"));
    node->setAttribute("style", without_priority.c_str());
    EXPECT_NE(snapshot(*doc), before); // Never normalize CSS or remove importance.
    node->setAttribute("style", style.c_str()); EXPECT_EQ(snapshot(*doc), before);

    node->setAttribute("data-probe", "");
    EXPECT_NE(snapshot(*doc), before); // Empty is not absent.
    node->setAttribute("data-probe", nullptr); EXPECT_EQ(snapshot(*doc), before);
    node->setAttribute("x", "12.0001");
    EXPECT_NE(snapshot(*doc), before); // No numeric tolerance/canonicalization.
    node->setAttribute("x", "12"); EXPECT_EQ(snapshot(*doc), before);

    auto content = run(*doc).getRepr()->firstChild(); ASSERT_TRUE(content);
    auto original = std::string(content->content());
    content->setContent("Native ");
    EXPECT_NE(snapshot(*doc), before); // Whitespace is significant.
    content->setContent(original.c_str()); EXPECT_EQ(snapshot(*doc), before);
    auto parent = node->parent(); ASSERT_TRUE(parent);
    auto previous = doc->getObjectById("baseline")->getRepr();
    parent->changeOrder(node, nullptr);
    EXPECT_NE(snapshot(*doc), before); // Child order is never sorted.
    parent->changeOrder(node, previous); EXPECT_EQ(snapshot(*doc), before);
}
