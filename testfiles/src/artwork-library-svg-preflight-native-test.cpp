// SPDX-License-Identifier: GPL-2.0-or-later
// Native writer -> preflight -> native reopen. Source fixtures are trusted test
// literals; the staged bytes are not given to a native parser before admission.
#include "io/artwork-library-svg-preflight.h"
#include "io/artwork-library-document.h"
#include "io/artwork-library-package.h"
#include "io/artwork-library-lbart.h"
#include "io/artwork-library-lightburn.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "object/sp-gradient.h"
#include "object/sp-pattern.h"
#include "style.h"
#include "bitmap-adjustment-chemistry.h"
#include "object/sp-image.h"
#include "object/sp-text.h"
#include "object/sp-flowtext.h"
#include "object/sp-flowregion.h"
#include "object/sp-string.h"
#include "text-editing.h"
#include "ui/text-paragraph-tools.h"
#include "ui/text-frame-tools.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "xml/attribute-record.h"
#include "xml/repr.h"
#include "display/drawing.h"
#include "display/drawing-item.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "display/cairo-utils.h"
#include <gtest/gtest.h>
#include <2geom/transforms.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <bit>
#include <zlib.h>

using namespace Inkscape;
namespace Library = Inkscape::IO::ArtworkLibrary;
namespace {
std::unique_ptr<SPDocument> source(std::string const &body, std::string const &size = "width='120' height='100'")
{
    auto xml = "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
               "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' "
               "xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' " + size + ">" + body + "</svg>";
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
    if (doc) doc->ensureUpToDate();
    return doc;
}
void fingerprint(XML::Node const *node, std::string &out)
{
    auto field = [&](char const *p) { std::string_view s = p ? p : ""; out += std::to_string(s.size()) + ":"; out += s; };
    out += '['; field(node->name()); field(node->content());
    for (auto const &a : node->attributeList()) { field(g_quark_to_string(a.key)); field(static_cast<char const *>(a.value)); }
    out += ';';
    for (auto child = node->firstChild(); child; child = child->next()) fingerprint(child, out);
    out += ']';
}
std::string fingerprint(SPDocument &doc)
{
    std::string out; fingerprint(doc.getReprDoc(), out); return out;
}
Library::ValidatedSvg validate(Library::StagedSelection const &s)
{
    return Library::preflight_svg(s.svg, {"b203e8e9-640c-4195-b0ea-f9b790245678", Library::artwork_sha256(s.svg), s.width_mm, s.height_mm});
}
std::unique_ptr<SPDocument> reopen(Library::ValidatedSvg const &a)
{
    auto bytes = a.svg_bytes();
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(bytes->data(), bytes->size()));
    if (doc) doc->ensureUpToDate();
    return doc;
}
unsigned count(XML::Node const *n, char const *name)
{
    unsigned total = g_strcmp0(n->name(), name) == 0;
    for (auto c = n->firstChild(); c; c = c->next()) total += count(c, name);
    return total;
}
Cairo::RefPtr<Cairo::ImageSurface> render(SPDocument &doc, Geom::Point correction)
{
    // Fixed synthetic-fixture surface, NOT a general filter allocator fence.
    Drawing drawing;
    auto key = SPItem::display_key_new(1);
    auto root = doc.getRoot();
    auto shown = root->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY);
    drawing.setRoot(shown);
    shown->setTransform(Geom::Translate(correction) * Geom::Translate(9.25, 7.375));
    drawing.update();
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 160, 120);
    DrawingSurface target(surface->cobj(), Geom::IntPoint(0, 0));
    DrawingContext context(target);
    drawing.render(context, Geom::IntRect::from_xywh(0, 0, 160, 120));
    surface->flush(); root->invoke_hide(key); return surface;
}
std::size_t difference(Cairo::RefPtr<Cairo::ImageSurface> const &a, Cairo::RefPtr<Cairo::ImageSurface> const &b)
{
    std::size_t changed = 0;
    for (int y = 0; y < a->get_height(); ++y) for (int x = 0; x < a->get_width(); ++x)
        changed += std::memcmp(a->get_data() + y * a->get_stride() + x * 4, b->get_data() + y * b->get_stride() + x * 4, 4) != 0;
    return changed;
}
class ArtworkLibraryPreflightNative : public ::testing::Test {
    void SetUp() override { if (!Application::exists()) Application::create(false); }
};

// Observed fixed-record container with deliberately unrelated raw extents.
// The disposable preview is not an image; the editable pipeline must not use it.
Library::Bytes lightburn_archive(std::string const &xml)
{
    Library::Bytes data(9, 0);
    data[8] = '?';
    auto put32 = [&](std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) data.at(at + i) = (value >> (24 - i * 8)) & 255;
    };
    uLongf compressed = compressBound(xml.size());
    data.resize(13 + compressed);
    put32(9, xml.size());
    if (compress2(data.data() + 13, &compressed,
                  reinterpret_cast<Bytef const *>(xml.data()), xml.size(), Z_BEST_SPEED) != Z_OK)
        throw std::runtime_error("Synthetic library compression failed");
    data.resize(13 + compressed);
    auto directory = data.size();
    data.resize(directory + 136);
    put32(0, directory); put32(4, 1);
    data[directory + 1] = 'A';
    put32(directory + 104, 8); put32(directory + 108, 1);
    put32(directory + 112, 9); put32(directory + 116, compressed + 4);
    auto raw = std::bit_cast<std::uint64_t>(777.);
    for (auto at : {directory + 120, directory + 128}) {
        put32(at, raw >> 32); put32(at + 4, raw & 0xffffffffu);
    }
    return data;
}
XML::Node *first_element(XML::Node *node, char const *name)
{
    if (g_strcmp0(node->name(), name) == 0) return node;
    for (auto c = node->firstChild(); c; c = c->next())
        if (auto found = first_element(c, name)) return found;
    return nullptr;
}
} // namespace

TEST_F(ArtworkLibraryPreflightNative, ActualRectStageWithInnerSvgAndNamedviewPassesWithoutMutation)
{
    auto doc = source("<rect id='art' x='10' y='8' width='30' height='20' fill='red'/>"); ASSERT_TRUE(doc);
    ObjectSet selection(doc.get()); selection.add(doc->getObjectById("art"));
    auto before = fingerprint(*doc); auto dirty = doc->isModifiedSinceSave();
    auto undo = DocumentUndo::getUndoSensitive(doc.get()); auto selected = selection.items_vector();
    auto staged = Library::stage_selection(selection);
    auto a = validate(staged); auto parsed = reopen(a); ASSERT_TRUE(parsed);
    EXPECT_GE(count(parsed->getReprRoot(), "svg:svg"), 2u);
    EXPECT_EQ(count(parsed->getReprRoot(), "sodipodi:namedview"), 1u);
    ASSERT_TRUE(parsed->getObjectById("art"));
    EXPECT_EQ(*a.svg_bytes(), std::string(staged.svg.begin(), staged.svg.end()));
    EXPECT_NEAR(a.width_mm(), 30 * 25.4 / 96, 1e-10);
    EXPECT_NEAR(a.height_mm(), 20 * 25.4 / 96, 1e-10);
    EXPECT_EQ(fingerprint(*doc), before); EXPECT_EQ(doc->isModifiedSinceSave(), dirty);
    EXPECT_EQ(DocumentUndo::getUndoSensitive(doc.get()), undo); EXPECT_EQ(selection.items_vector(), selected);
    EXPECT_EQ(difference(render(*doc, {0, 0}), render(*parsed, {10, 8})), 0u);
}

namespace {
std::string tone_png_uri()
{
    auto raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 4, 3);
    if (!raw) throw std::runtime_error("Test pixels");
    Pixbuf pixels(raw);
    gdk_pixbuf_fill(raw, 0x33669980);
    gdk_pixbuf_set_option(raw, "x-dpi", "300");
    gdk_pixbuf_set_option(raw, "y-dpi", "301");
    auto result = sp_image_encode_png_data_uri(pixels);
    if (!result) throw std::runtime_error("Test PNG");
    return *result;
}
}

TEST_F(ArtworkLibraryPreflightNative, NativeLegacyFlowWriterRemainsEditableWithoutTextConversion)
{
    auto doc = source(""); ASSERT_TRUE(doc);
    auto item = create_flowtext_with_internal_frame(doc->getRoot(), nullptr, Geom::Rect::from_xywh(5, 5, 100, 80));
    ASSERT_TRUE(item);
    item->getRepr()->setAttribute("id", "art");
    item->getRepr()->setAttribute("style", "font-family:sans-serif;font-size:12px;line-height:150%");
    sp_te_set_repr_text_multiline(item, "Legacy flowed invitation text stays editable after library reopen.");
    doc->ensureUpToDate();
    ObjectSet selected(doc.get()); selected.add(item);
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto before = fingerprint(*doc);
    auto copy = reopen(validate(Library::stage_selection(selected))); ASSERT_TRUE(copy);
    auto text = cast<SPFlowtext>(copy->getObjectById("art")); ASSERT_TRUE(text);
    EXPECT_EQ(count(copy->getReprRoot(), "svg:flowRoot"), 1u);
    EXPECT_GT(count(copy->getReprRoot(), "svg:flowRegion"), 0u);
    EXPECT_EQ(difference(render(*doc, {0, 0}), render(*copy, bounds->min())), 0u);
    sp_te_set_repr_text_multiline(text, "Changed legacy paragraph"); copy->ensureUpToDate();
    ObjectSet again(copy.get()); again.add(text);
    auto twice = reopen(validate(Library::stage_selection(again))); ASSERT_TRUE(twice);
    EXPECT_TRUE(cast<SPFlowtext>(twice->getObjectById("art")));
    EXPECT_EQ(fingerprint(*doc), before);
}

namespace {
Library::ValidatedSvg admit_flow_fixture(std::string const &body, std::string const &viewbox = "0 0 96 96")
{
    auto xml = "<svg xmlns='http://www.w3.org/2000/svg' width='25.4mm' height='25.4mm' viewBox='" +
               viewbox + "'>" + body + "</svg>";
    Library::Bytes bytes(xml.begin(), xml.end());
    return Library::preflight_svg(bytes, {"b203e8e9-640c-4195-b0ea-f9b790245678",
                                          Library::artwork_sha256(bytes), 25.4, 25.4});
}
std::string child_ids(SPObject &object)
{
    std::string ids;
    for (auto &child : object.children) {
        if (auto id = child.getRepr()->attribute("id")) ids += std::string(id) + ";";
    }
    return ids;
}
}

TEST_F(ArtworkLibraryPreflightNative, FlowExclusionOwnerSurvivesLaterRegionsAndNestedRoots)
{
    // Preflight BEFORE native parse. These orders formerly moved the sole
    // exclusion owner into an earlier paragraph, leaving later regions null.
    std::string const region = "<flowRegion id='region'><rect width='80' height='80'/></flowRegion>";
    std::string const para = "<flowPara id='para'>First paragraph</flowPara>";
    std::string const later = "<flowRegion id='later'><rect x='1' y='1' width='70' height='70'/></flowRegion>";
    for (bool excluded : {false, true}) {
        for (int order = 0; order < 3; ++order) {
            SCOPED_TRACE(::testing::Message() << "exclusion=" << excluded << " order=" << order);
            auto exclusion = excluded ?
                "<flowRegionExclude id='exclude'><rect x='25' y='25' width='5' height='5'/></flowRegionExclude>" : "";
            auto content = order == 0 ? para + region : order == 1 ? region + para + later :
                region + para + "<flowRoot id='nested'>" + later + "<flowPara>Nested</flowPara></flowRoot>";
            auto accepted = admit_flow_fixture("<flowRoot id='art' style='font-size:12px'>" + content + exclusion + "</flowRoot>");
            auto bytes_before = *accepted.svg_bytes();
            auto doc = reopen(accepted); ASSERT_TRUE(doc);
            auto flow = cast<SPFlowtext>(doc->getObjectById("art")); ASSERT_TRUE(flow);
            std::string expected = order == 0 ? "para;region;" : order == 1 ? "region;para;later;" : "region;para;nested;";
            if (excluded) {
                expected += "exclude;";
                auto exclude = cast<SPFlowregionExclude>(doc->getObjectById("exclude")); ASSERT_TRUE(exclude);
                ASSERT_TRUE(exclude->getComputed()); EXPECT_TRUE(exclude->getComputed()->hasEdges());
            }
            EXPECT_EQ(child_ids(*flow), expected);
            auto before = fingerprint(*doc);
            for (int i = 0; i < 3; ++i) {
                flow->rebuildLayout();
                if (auto nested = cast<SPFlowtext>(doc->getObjectById("nested"))) nested->rebuildLayout();
                doc->ensureUpToDate();
                EXPECT_TRUE(flow->layout.inputExists());
                EXPECT_EQ(fingerprint(*doc), before);
                EXPECT_EQ(child_ids(*flow), expected);
            }
            EXPECT_EQ(*accepted.svg_bytes(), bytes_before);
        }
    }
}

TEST_F(ArtworkLibraryPreflightNative, DirectPreservedFlowRootSpacesReachNativeLayout)
{
    std::string spaces(32, ' ');
    auto accepted = admit_flow_fixture("<flowRoot id='art' xml:space='preserve' style='font-size:12px'>"
        "<flowRegion><rect width='80' height='80'/></flowRegion>" + spaces + "<flowPara>X</flowPara></flowRoot>");
    EXPECT_GE(accepted.stats().geometry_commands, spaces.size() + 1);
    auto doc = reopen(accepted); ASSERT_TRUE(doc);
    auto flow = cast<SPFlowtext>(doc->getObjectById("art")); ASSERT_TRUE(flow);
    bool found = false;
    for (auto &child : flow->children) {
        if (auto text = cast<SPString>(&child)) {
            EXPECT_EQ(text->string.raw(), spaces); found = true;
        }
    }
    EXPECT_TRUE(found); EXPECT_TRUE(flow->layout.inputExists());
}

TEST_F(ArtworkLibraryPreflightNative, InlinePercentNativeComputedUsesRootAndNestedViewportAxes)
{
    for (bool vertical : {false, true}) for (bool nested : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "vertical=" << vertical << " nested=" << nested);
        auto mode = vertical ? "tb-rl" : "lr-tb";
        auto text = std::string("<text id='art' x='0' y='20' style='font-size:12px;inline-size:inherit;writing-mode:") + mode + "'>Editable</text>";
        if (nested) text = "<svg width='40' height='40' viewBox='0 0 120 80'>" + text + "</svg>";
        auto accepted = admit_flow_fixture("<g style='inline-size:50%'>" + text + "</g>", "0 0 400 200");
        auto doc = reopen(accepted); ASSERT_TRUE(doc);
        auto item = cast<SPText>(doc->getObjectById("art")); ASSERT_TRUE(item); ASSERT_TRUE(item->style);
        EXPECT_EQ(item->style->inline_size.unit, SP_CSS_UNIT_PERCENT);
        EXPECT_DOUBLE_EQ(item->style->inline_size.computed, nested ? (vertical ? 40 : 60) : (vertical ? 100 : 200));
        EXPECT_STREQ(item->getRepr()->attribute("style"),
                     (std::string("font-size:12px;inline-size:inherit;writing-mode:") + mode).c_str());
    }
}

namespace {
std::string native_descendant_strings(SPObject &object)
{
    if (auto text = cast<SPString>(&object)) return text->string.raw();
    std::string result;
    for (auto &child : object.children) result += native_descendant_strings(child);
    return result;
}
}

TEST_F(ArtworkLibraryPreflightNative, OrdinaryChildTransformDoesNotContractNativeTextLayout)
{
    auto make = [](char const *child_transform, char const *root_transform, char const *shift = "1000px") {
        auto body = std::string("<text id='art' style='font-family:sans-serif;font-size:12px' "
            "baseline-shift='baseline' transform='") + root_transform + "'>"
            "<g id='run' transform='" + child_transform + "' baseline-shift='" + shift + "'>M</g></text>";
        return reopen(admit_flow_fixture(body)); // Default cap; no huge raster surface.
    };
    auto reference = make("scale(1)", "scale(100)");
    auto contracted_child = make("scale(.01)", "scale(100)");
    auto unit_root = make("scale(.01)", "scale(1)");
    auto unshifted = make("scale(.01)", "scale(1)", "baseline");
    ASSERT_TRUE(reference); ASSERT_TRUE(contracted_child); ASSERT_TRUE(unit_root);
    ASSERT_TRUE(unshifted);
    auto plain = cast<SPText>(reference->getObjectById("art")); ASSERT_TRUE(plain);
    auto child = cast<SPText>(contracted_child->getObjectById("art")); ASSERT_TRUE(child);
    auto unit = cast<SPText>(unit_root->getObjectById("art")); ASSERT_TRUE(unit);
    auto zero = cast<SPText>(unshifted->getObjectById("art")); ASSERT_TRUE(zero);
    ASSERT_TRUE(plain->layout.outputExists()); ASSERT_TRUE(child->layout.outputExists()); ASSERT_TRUE(unit->layout.outputExists());
    ASSERT_TRUE(zero->layout.outputExists());
    auto a = plain->layout.characterAnchorPoint(plain->layout.begin());
    auto b = child->layout.characterAnchorPoint(child->layout.begin());
    auto c = unit->layout.characterAnchorPoint(unit->layout.begin());
    auto plain_bounds = plain->layout.bounds(Geom::Affine());
    auto child_bounds = child->layout.bounds(Geom::Affine());
    ASSERT_TRUE(plain_bounds); ASSERT_TRUE(child_bounds);
    for (auto axis : {Geom::X, Geom::Y}) {
        EXPECT_NEAR(a[axis], b[axis], 1e-9); EXPECT_NEAR(b[axis], c[axis], 1e-9);
        EXPECT_NEAR(plain_bounds->min()[axis], child_bounds->min()[axis], 1e-9);
        EXPECT_NEAR(plain_bounds->max()[axis], child_bounds->max()[axis], 1e-9);
    }
    // Output Span::baseline_shift is zero: compute folds the shift into each
    // glyph's y instead. characterAnchorPoint is therefore NOT the glyph
    // displacement oracle. bounds() uses Glyph::transform, as rendering does.
    auto unit_bounds = unit->layout.bounds(unit->i2doc_affine());
    auto document_bounds = child->layout.bounds(child->i2doc_affine());
    auto zero_bounds = zero->layout.bounds(zero->i2doc_affine());
    ASSERT_TRUE(unit_bounds); ASSERT_TRUE(document_bounds); ASSERT_TRUE(zero_bounds);
    auto run = unit_root->getObjectById("run"); ASSERT_TRUE(run); ASSERT_TRUE(run->style);
    SPObject *first_source = nullptr;
    unit->layout.getSourceOfCharacter(unit->layout.begin(), &first_source);
    ASSERT_TRUE(first_source); ASSERT_TRUE(first_source->parent);
    EXPECT_EQ(unit->layout.characterAt(unit->layout.begin()), gunichar('M'));
    EXPECT_EQ(first_source->parent, run);
    ASSERT_TRUE(cast<SPString>(first_source));
    EXPECT_EQ(cast<SPString>(first_source)->string.raw(), "M");
    auto document_anchor = b * child->i2doc_affine();
    auto unit_anchor = c * unit->i2doc_affine();
    RecordProperty("transform_first_character", std::to_string(unit->layout.characterAt(unit->layout.begin())));
    RecordProperty("transform_source_parent", first_source->parent->getId());
    RecordProperty("transform_computed_parent_baseline", std::to_string(run->style->baseline_shift.computed));
    RecordProperty("transform_unit_anchor_y", std::to_string(unit_anchor[Geom::Y]));
    RecordProperty("transform_document_anchor_y", std::to_string(document_anchor[Geom::Y]));
    RecordProperty("transform_unit_glyph_top", std::to_string(unit_bounds->min()[Geom::Y]));
    RecordProperty("transform_document_glyph_top", std::to_string(document_bounds->min()[Geom::Y]));
    RecordProperty("transform_zero_shift_glyph_top", std::to_string(zero_bounds->min()[Geom::Y]));
    EXPECT_DOUBLE_EQ(run->style->baseline_shift.computed, 1000.);
    // Native diagnostics confirm zero cursor anchors alongside shifted glyphs.
    // The glyph-bound displacement assertions below are the geometry oracle.
    EXPECT_NEAR(unit_anchor[Geom::Y], 0., 1e-9);
    EXPECT_NEAR(document_anchor[Geom::Y], 0., 1e-9);
    EXPECT_NEAR(document_anchor[Geom::Y], unit_anchor[Geom::Y] * 100, 1e-6);
    EXPECT_GT(std::abs(unit_bounds->min()[Geom::Y]), 990.);
    EXPECT_GT(std::abs(document_bounds->min()[Geom::Y]), 99000.);
    EXPECT_NEAR(std::abs(unit_bounds->min()[Geom::Y] - zero_bounds->min()[Geom::Y]), 1000., 1e-6);
    EXPECT_NEAR(std::abs(unit_bounds->max()[Geom::Y] - zero_bounds->max()[Geom::Y]), 1000., 1e-6);
    for (auto axis : {Geom::X, Geom::Y}) {
        EXPECT_NEAR(document_bounds->min()[axis], unit_bounds->min()[axis] * 100, 1e-6);
        EXPECT_NEAR(document_bounds->max()[axis], unit_bounds->max()[axis] * 100, 1e-6);
    }
}

TEST_F(ArtworkLibraryPreflightNative, UseAndNestedTextTransformsDoNotAlterEnclosingLayout)
{
    auto control = reopen(admit_flow_fixture("<text id='art' baseline-shift='baseline' style='font-size:12px'>"
                                            "<g baseline-shift='1000px'>M</g></text>"));
    ASSERT_TRUE(control);
    auto plain = cast<SPText>(control->getObjectById("art")); ASSERT_TRUE(plain);
    ASSERT_TRUE(plain->layout.outputExists());
    auto expected = plain->layout.characterAnchorPoint(plain->layout.begin());
    for (bool clone : {false, true}) {
        SCOPED_TRACE(clone);
        auto definition = clone ? "<defs><g id='words' transform='scale(.01)' baseline-shift='1000px'>M</g></defs>" : "";
        auto content = clone ? "<use href='#words' transform='scale(.01)' baseline-shift='baseline'/>" :
            "<text transform='scale(.01)' baseline-shift='baseline'><g baseline-shift='1000px'>M</g></text>";
        auto doc = reopen(admit_flow_fixture(std::string(definition) +
            "<text id='art' baseline-shift='baseline' style='font-size:12px'>" + content + "</text>"));
        ASSERT_TRUE(doc);
        auto text = cast<SPText>(doc->getObjectById("art")); ASSERT_TRUE(text);
        ASSERT_TRUE(text->layout.outputExists());
        auto actual = text->layout.characterAnchorPoint(text->layout.begin());
        for (auto axis : {Geom::X, Geom::Y}) EXPECT_NEAR(actual[axis], expected[axis], 1e-9);
    }
}

TEST_F(ArtworkLibraryPreflightNative, DeferredWhitespaceIsConsumedByNativeXmlAndUseTextTraversal)
{
    for (bool flow : {false, true}) for (bool clone : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "flow=" << flow << " clone=" << clone);
        auto root = flow ? "flowRoot" : "text";
        auto spaces = std::string(33, ' ');
        auto group = "<g id='words' xml:space='preserve'>" + spaces + "</g>";
        auto body = clone ? "<defs>" + group + "</defs>" : std::string();
        body += std::string("<") + root + " id='art' xml:space='preserve' style='font-size:12px'>";
        if (flow) body += "<flowRegion><rect width='80' height='80'/></flowRegion>";
        body += (clone ? "<use href='#words'/>" : group) + std::string("</") + root + ">";
        auto accepted = admit_flow_fixture(body); // Always before native parsing.
        auto doc = reopen(accepted); ASSERT_TRUE(doc);
        auto art = doc->getObjectById("art"); ASSERT_TRUE(art);
        EXPECT_EQ(native_descendant_strings(*art), spaces);
        if (flow) {
            auto text = cast<SPFlowtext>(art); ASSERT_TRUE(text); EXPECT_TRUE(text->layout.inputExists());
        } else {
            auto text = cast<SPText>(art); ASSERT_TRUE(text); EXPECT_TRUE(text->layout.inputExists());
        }
        EXPECT_EQ(accepted.stats().geometry_commands, (clone ? 66u : 33u) + (flow ? 8u : 0u));
    }
}

TEST_F(ArtworkLibraryPreflightNative, ExclusionChangesActualOutputInRegionAfterParagraph)
{
    auto make = [](bool exclude) {
        // The only wrap region is AFTER the recursive paragraph which used to
        // consume the exclusion owner. Control differs only in exclusion shape.
        auto content = std::string("<flowRoot id='art' style='font-family:sans-serif;font-size:12px'>"
            "<flowPara>MM</flowPara><flowRegion id='later'><rect width='80' height='80'/></flowRegion>"
            "<flowRegionExclude id='exclude'>");
        if (exclude) content += "<rect width='40' height='80'/>";
        content += "</flowRegionExclude></flowRoot>";
        return reopen(admit_flow_fixture(content));
    };
    auto control = make(false), excluded = make(true);
    ASSERT_TRUE(control); ASSERT_TRUE(excluded);
    auto plain = cast<SPFlowtext>(control->getObjectById("art")); ASSERT_TRUE(plain);
    auto cut = cast<SPFlowtext>(excluded->getObjectById("art")); ASSERT_TRUE(cut);
    auto shape = cast<SPFlowregionExclude>(excluded->getObjectById("exclude")); ASSERT_TRUE(shape);
    ASSERT_TRUE(shape->getComputed()); ASSERT_TRUE(shape->getComputed()->hasEdges());
    auto before_plain = fingerprint(*control), before_cut = fingerprint(*excluded);
    for (int i = 0; i < 3; ++i) {
        plain->rebuildLayout(); cut->rebuildLayout();
        control->ensureUpToDate(); excluded->ensureUpToDate();
        ASSERT_TRUE(plain->layout.outputExists()); ASSERT_TRUE(cut->layout.outputExists());
        auto plain_bounds = plain->layout.bounds(Geom::Affine());
        auto cut_bounds = cut->layout.bounds(Geom::Affine());
        ASSERT_TRUE(plain_bounds);
        // The unbreakable word must fit in the 40-unit remaining wrap region;
        // otherwise lack of glyph output is correct native overflow behavior.
        ASSERT_LT(plain_bounds->width(), 40.);
        ASSERT_TRUE(cut_bounds);
        auto plain_anchor = plain->layout.characterAnchorPoint(plain->layout.begin());
        auto cut_anchor = cut->layout.characterAnchorPoint(cut->layout.begin());
        // Nonempty output must move into the remaining right half, not merely
        // retain text input or survive a Boolean operation whose result is lost.
        EXPECT_NEAR(cut_anchor[Geom::X] - plain_anchor[Geom::X], 40., 1e-6);
        EXPECT_GT(cut_bounds->min()[Geom::X] - plain_bounds->min()[Geom::X], 30.);
        EXPECT_EQ(fingerprint(*control), before_plain); EXPECT_EQ(fingerprint(*excluded), before_cut);
    }
}

TEST_F(ArtworkLibraryPreflightNative, NativeToneWriterStagesReopensEditsWithoutBakingPixels)
{
    auto doc = source("<image id='art' x='10' y='8' width='32' height='24' xlink:href='" + tone_png_uri() + "'/>");
    ASSERT_TRUE(doc);
    auto art = cast<SPImage>(doc->getObjectById("art")); ASSERT_TRUE(art);
    ASSERT_TRUE(art->pixbuf);
    auto original_payload = std::string(art->href);
    Pixbuf original_pixels(*art->pixbuf);
    auto original_raw = original_pixels.getPixbufRaw();
    ASSERT_EQ(gdk_pixbuf_get_n_channels(original_raw), 4);
    ObjectSet unadjusted(doc.get()); unadjusted.add(art);
    auto baseline_copy = reopen(validate(Library::stage_selection(unadjusted)));
    ASSERT_TRUE(baseline_copy);
    auto baseline_image = cast<SPImage>(baseline_copy->getObjectById("art"));
    ASSERT_TRUE(baseline_image);
    auto unadjusted_staged_payload = std::string(baseline_image->href);
    Filters::BitmapToneSettings tone{20, -10, 15, 7, -4, 11};
    ASSERT_TRUE(BitmapAdjustments::apply_tone(art, tone)); doc->ensureUpToDate();
    ObjectSet selected(doc.get()); selected.add(art);
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto before = fingerprint(*doc);
    auto dirty = doc->isModifiedSinceSave();
    auto staged = Library::stage_selection(selected);
    auto accepted = validate(staged); auto copy = reopen(accepted); ASSERT_TRUE(copy);
    auto image = cast<SPImage>(copy->getObjectById("art")); ASSERT_TRUE(image);
    auto settings = BitmapAdjustments::query_tone(image); ASSERT_TRUE(settings);
    EXPECT_TRUE(Filters::bitmap_tone_settings_equal(*settings, tone));
    EXPECT_EQ(count(copy->getReprRoot(), "svg:feComponentTransfer"), 1u);
    EXPECT_EQ(count(copy->getReprRoot(), "svg:feFuncA"), 1u);
    ASSERT_TRUE(image->pixbuf);
    Pixbuf pixels(*image->pixbuf); auto raw = pixels.getPixbufRaw();
    ASSERT_EQ(gdk_pixbuf_get_n_channels(raw), 4);
    // Compare against the unadjusted native image, not ideal straight-alpha
    // channel values: Pixbuf's Cairo round-trip quantizes 0x33 at alpha 0x80.
    // Staging intentionally encodes the native pixels, including metadata.
    // Compare the adjusted and unadjusted staging payloads to detect filter
    // baking, and every original native pixel to detect content loss.
    EXPECT_EQ(std::string(image->href), unadjusted_staged_payload);
    EXPECT_EQ(std::string(art->href), original_payload);
    ASSERT_EQ(gdk_pixbuf_get_width(raw), gdk_pixbuf_get_width(original_raw));
    ASSERT_EQ(gdk_pixbuf_get_height(raw), gdk_pixbuf_get_height(original_raw));
    for (int y = 0; y < gdk_pixbuf_get_height(raw); ++y) {
        auto row = gdk_pixbuf_get_pixels(raw) + y * gdk_pixbuf_get_rowstride(raw);
        auto original_row = gdk_pixbuf_get_pixels(original_raw) + y * gdk_pixbuf_get_rowstride(original_raw);
        EXPECT_EQ(std::memcmp(row, original_row, gdk_pixbuf_get_width(raw) * 4), 0);
        for (int x = 0; x < gdk_pixbuf_get_width(raw); ++x) EXPECT_EQ(row[x * 4 + 3], 0x80);
    }
    EXPECT_STREQ(gdk_pixbuf_get_option(raw, "x-dpi"), "300");
    EXPECT_STREQ(gdk_pixbuf_get_option(raw, "y-dpi"), "301");
    auto rendered = render(*copy, bounds->min());
    EXPECT_EQ(difference(render(*doc, {0, 0}), rendered), 0u);
    auto saved_payload = std::string(image->href);
    tone.brightness = -60;
    ASSERT_TRUE(BitmapAdjustments::apply_tone(image, tone)); copy->ensureUpToDate();
    EXPECT_EQ(std::string(image->href), saved_payload);
    EXPECT_GT(difference(rendered, render(*copy, bounds->min())), 0u);
    ObjectSet again(copy.get()); again.add(image);
    auto twice = reopen(validate(Library::stage_selection(again))); ASSERT_TRUE(twice);
    auto revised = BitmapAdjustments::query_tone(cast<SPItem>(twice->getObjectById("art"))); ASSERT_TRUE(revised);
    EXPECT_TRUE(Filters::bitmap_tone_settings_equal(*revised, tone));
    EXPECT_EQ(fingerprint(*doc), before); EXPECT_EQ(doc->isModifiedSinceSave(), dirty);
}

TEST_F(ArtworkLibraryPreflightNative, NativeMultilineWriterPreservesEditableTextAndPercentLeading)
{
    auto doc = source("<text id='art' x='10' y='20' style='font-size:12px;font-family:sans-serif;line-height:150%'>seed</text>");
    ASSERT_TRUE(doc);
    auto art = cast<SPText>(doc->getObjectById("art")); ASSERT_TRUE(art);
    sp_te_set_repr_text_multiline(art, "Café invitation\nEditable second line"); doc->ensureUpToDate();
    ObjectSet selected(doc.get()); selected.add(art);
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto before = fingerprint(*doc);
    auto staged = Library::stage_selection(selected);
    auto accepted = validate(staged); auto copy = reopen(accepted); ASSERT_TRUE(copy);
    auto text = cast<SPText>(copy->getObjectById("art")); ASSERT_TRUE(text);
    EXPECT_GT(count(copy->getReprRoot(), "svg:tspan"), 0u);
    EXPECT_FALSE(accepted.requested_font_families().empty());
    EXPECT_EQ(difference(render(*doc, {0, 0}), render(*copy, bounds->min())), 0u);
    sp_te_set_repr_text_multiline(text, "Changed after library reopen"); copy->ensureUpToDate();
    ObjectSet again(copy.get()); again.add(text);
    auto twice = reopen(validate(Library::stage_selection(again))); ASSERT_TRUE(twice);
    EXPECT_TRUE(cast<SPText>(twice->getObjectById("art")));
    EXPECT_EQ(fingerprint(*doc), before);
}

TEST_F(ArtworkLibraryPreflightNative, NativeParagraphAndDropCapWritersRemainEditable)
{
    auto doc = source("<text id='art' x='5' y='25' style='font-family:sans-serif;font-size:12px;line-height:150%;inline-size:100px;white-space:pre-wrap;"
                      "-inkscape-paragraph-spacing-before:2px;-inkscape-paragraph-spacing-after:3px;-inkscape-language-spacing:110%'>"
                      "<tspan id='p1' sodipodi:role='paragraph'>Invitation text wraps here and stays editable.</tspan>"
                      "<tspan id='p2' sodipodi:role='paragraph'>Another paragraph.</tspan></text>");
    ASSERT_TRUE(doc);
    auto art = cast<SPText>(doc->getObjectById("art")); ASSERT_TRUE(art);
    ASSERT_TRUE(UI::setParagraphDropCapLines(*doc, {doc->getObjectById("p1")}, 2));
    ASSERT_TRUE(UI::setParagraphListMode(*doc, {doc->getObjectById("p2")}, UI::TextListMode::Numbered, 3));
    doc->ensureUpToDate();
    ObjectSet selected(doc.get()); selected.add(art);
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto before = fingerprint(*doc);
    auto copy = reopen(validate(Library::stage_selection(selected))); ASSERT_TRUE(copy);
    auto p1 = copy->getObjectById("p1"); auto p2 = copy->getObjectById("p2"); ASSERT_TRUE(p1); ASSERT_TRUE(p2);
    EXPECT_EQ(UI::paragraphDropCapLines(*p1), 2u);
    EXPECT_EQ(UI::paragraphListMode(*p2), UI::TextListMode::Numbered);
    EXPECT_EQ(UI::paragraphListStart(*p2), 3u);
    EXPECT_EQ(difference(render(*doc, {0, 0}), render(*copy, bounds->min())), 0u);
    ASSERT_TRUE(UI::setParagraphDropCapLines(*copy, {p1}, 3));
    ASSERT_TRUE(UI::setParagraphListMode(*copy, {p2}, UI::TextListMode::Bulleted)); copy->ensureUpToDate();
    ObjectSet again(copy.get()); again.add(copy->getObjectById("art"));
    auto twice = reopen(validate(Library::stage_selection(again))); ASSERT_TRUE(twice);
    EXPECT_EQ(UI::paragraphDropCapLines(*twice->getObjectById("p1")), 3u);
    EXPECT_EQ(UI::paragraphListMode(*twice->getObjectById("p2")), UI::TextListMode::Bulleted);
    EXPECT_EQ(fingerprint(*doc), before);
}

namespace {
void native_column_roundtrip(bool overset)
{
    // Retain the original 100/70/8 geometry as the overset counterexample.
    // The positive uses 196 total width: 94-unit then 60-unit columns.
    auto word_doc = source("<text id='word' style='font-family:sans-serif;font-size:10px'>Invitation</text>");
    ASSERT_TRUE(word_doc);
    auto word = cast<SPText>(word_doc->getObjectById("word")); ASSERT_TRUE(word);
    ASSERT_TRUE(word->layout.outputExists());
    auto word_bounds = word->layout.bounds(Geom::Affine()); ASSERT_TRUE(word_bounds);
    auto word_advance = word->layout.characterAnchorPoint(word->layout.end())[Geom::X] -
                        word->layout.characterAnchorPoint(word->layout.begin())[Geom::X];
    ::testing::Test::RecordProperty("frame_first_word_advance", std::to_string(word_advance));
    ::testing::Test::RecordProperty("frame_first_word_ink_width", std::to_string(word_bounds->width()));
    auto frame_nodes = [](SPText &text) {
        std::vector<std::shared_ptr<XML::Node>> result;
        for (auto *href : text.style->shape_inside.hrefs) {
            auto object = href->getObject();
            EXPECT_NE(object, nullptr);
            if (!object) continue;
            auto node = object->getRepr();
            GC::anchor(node); result.emplace_back(node, [](auto *n) { GC::release(n); });
        }
        return result;
    };
    auto record_bounds = [](char const *phase, Geom::OptRect const &bounds) {
        ::testing::Test::RecordProperty(phase, bounds ?
            std::to_string(bounds->left()) + "," + std::to_string(bounds->top()) + "," +
            std::to_string(bounds->width()) + "," + std::to_string(bounds->height()) : "absent");
    };
    auto doc = source("<text id='art' x='5' y='20' style='font-family:sans-serif;font-size:10px'>"
                      "Invitation text in multiple columns stays editable across library staging and reopening.</text>");
    ASSERT_TRUE(doc);
    auto art = cast<SPText>(doc->getObjectById("art")); ASSERT_TRUE(art);
    UI::TextFrameSettings frame; frame.width = overset ? 100 : 196;
    frame.height = 70; frame.columns = 2; frame.gap = 8;
    auto const two_width = (frame.width - frame.gap) / 2;
    auto const three_width = (frame.width - 2 * frame.gap) / 3;
    ASSERT_GT(word_advance, 0.);
    ASSERT_LT(word_advance, two_width);
    ASSERT_LT(word_bounds->width(), two_width);
    if (overset) {
        ASSERT_GT(word_advance, three_width);
        ASSERT_GT(word_bounds->width(), three_width);
    } else {
        ASSERT_LT(word_advance, three_width);
        ASSERT_LT(word_bounds->width(), three_width);
    }
    ASSERT_TRUE(UI::setTextFrameSettings(*doc, {art}, frame)); doc->ensureUpToDate();
    ObjectSet selected(doc.get()); selected.add(art);
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto before = fingerprint(*doc);
    std::optional<Library::StagedSelection> first_stage;
    {
        SCOPED_TRACE("first stage: original two-column writer");
        record_bounds("frame_two_column_glyph_bounds", art->layout.bounds(art->i2doc_affine()));
        record_bounds("frame_two_column_selection_bounds", bounds);
        auto actual_frames = frame_nodes(*art); ASSERT_EQ(actual_frames.size(), 2u);
        for (auto const &node : actual_frames) {
            ASSERT_TRUE(node->parent()); EXPECT_STREQ(node->parent()->name(), "svg:defs");
            EXPECT_STREQ(node->attribute("inkscape:text-frame-owner"), "art");
            EXPECT_DOUBLE_EQ(node->getAttributeDouble("width", -1), two_width);
        }
        ::testing::Test::RecordProperty("frame_first_word_fits_two", word_advance <= two_width ? "yes" : "no");
        EXPECT_GT(bounds->width(), 0.); EXPECT_GT(bounds->height(), 0.);
        ASSERT_NO_THROW(first_stage.emplace(Library::stage_selection(selected)));
    }
    auto copy = reopen(validate(*first_stage)); ASSERT_TRUE(copy);
    auto text = cast<SPText>(copy->getObjectById("art")); ASSERT_TRUE(text);
    EXPECT_EQ(UI::textFrameSettings(*text).columns, 2u);
    ASSERT_TRUE(copy->getObjectById("art-text-frame-1"));
    ASSERT_TRUE(copy->getObjectById("art-text-frame-2"));
    EXPECT_EQ(difference(render(*doc, {0, 0}), render(*copy, bounds->min())), 0u);
    auto old_frames = frame_nodes(*text); ASSERT_EQ(old_frames.size(), 2u);
    frame.columns = 3;
    ASSERT_TRUE(UI::setTextFrameSettings(*copy, {text}, frame)); copy->ensureUpToDate();
    ObjectSet again(copy.get()); again.add(text);
    auto copy_before_stage = fingerprint(*copy);
    auto again_bounds = again.documentBounds(SPItem::VISUAL_BBOX);
    std::optional<Library::StagedSelection> second_stage;
    {
        SCOPED_TRACE("second stage: three fresh columns after reopen");
        auto actual_frames = frame_nodes(*text); ASSERT_EQ(actual_frames.size(), 3u);
        for (auto const &old : old_frames) EXPECT_EQ(old->parent(), nullptr);
        for (auto const &node : actual_frames) {
            ASSERT_TRUE(node->parent()); EXPECT_STREQ(node->parent()->name(), "svg:defs");
            EXPECT_STREQ(node->attribute("inkscape:text-frame-owner"), "art");
            auto object = copy->getObjectByRepr(node.get()); ASSERT_TRUE(object);
            EXPECT_EQ(object->document, copy.get());
            EXPECT_DOUBLE_EQ(node->getAttributeDouble("width", -1), three_width);
            EXPECT_DOUBLE_EQ(node->getAttributeDouble("height", -1), 70.);
            for (auto const &old : old_frames) EXPECT_NE(node.get(), old.get());
        }
        record_bounds("frame_three_column_glyph_bounds", text->layout.bounds(text->i2doc_affine()));
        record_bounds("frame_three_column_selection_bounds", again_bounds);
        ::testing::Test::RecordProperty("frame_first_word_fits_three", word_advance <= three_width ? "yes" : "no");
        if (overset) {
            EXPECT_FALSE(text->layout.bounds(text->i2doc_affine()));
            EXPECT_FALSE(again_bounds);
            try {
                (void)Library::stage_selection(again);
                ADD_FAILURE() << "Fully overset text must not stage as visible artwork";
            } catch (std::runtime_error const &error) {
                EXPECT_STREQ(error.what(), "Selection has no finite positive physical extent");
            }
            EXPECT_EQ(fingerprint(*copy), copy_before_stage);
            EXPECT_EQ(fingerprint(*doc), before);
            return;
        }
        ASSERT_TRUE(again_bounds && again_bounds->width() > 0 && again_bounds->height() > 0);
        ASSERT_NO_THROW(second_stage.emplace(Library::stage_selection(again)));
    }
    auto twice = reopen(validate(*second_stage)); ASSERT_TRUE(twice);
    auto reopened_text = cast<SPText>(twice->getObjectById("art")); ASSERT_TRUE(reopened_text);
    EXPECT_EQ(UI::textFrameSettings(*reopened_text).columns, 3u);
    auto reopened_frames = frame_nodes(*reopened_text); ASSERT_EQ(reopened_frames.size(), 3u);
    for (auto const &node : reopened_frames) {
        ASSERT_TRUE(node->parent());
        EXPECT_STREQ(node->parent()->name(), "svg:defs");
        EXPECT_STREQ(node->attribute("inkscape:text-frame-owner"), "art");
        EXPECT_DOUBLE_EQ(node->getAttributeDouble("width", -1), three_width);
        EXPECT_DOUBLE_EQ(node->getAttributeDouble("height", -1), 70.);
        ASSERT_TRUE(twice->getObjectByRepr(node.get()));
    }
    EXPECT_EQ(difference(render(*copy, {0, 0}), render(*twice, again_bounds->min())), 0u);
    EXPECT_EQ(fingerprint(*copy), copy_before_stage);
    EXPECT_EQ(fingerprint(*doc), before);
}
}

TEST_F(ArtworkLibraryPreflightNative, NativeMultiColumnFrameWriterPreservesAllRegions)
{
    native_column_roundtrip(false);
}

TEST_F(ArtworkLibraryPreflightNative, NativeNarrowColumnsKeepFreshFramesButRefuseOversetStaging)
{
    native_column_roundtrip(true);
}

TEST_F(ArtworkLibraryPreflightNative, RichNativeTextStylePreservesAxesDecorationsAndOrientation)
{
    auto doc = source("<text id='art' x='5' y='30' style='font-family:sans-serif;font-size:12px;line-height:150%;text-orientation:mixed'>"
                      "Café <tspan id='span'>rich</tspan></text>");
    ASSERT_TRUE(doc);
    auto span = doc->getObjectById("span"); ASSERT_TRUE(span);
    auto css = sp_repr_css_attr(span->getRepr(), "style");
    sp_repr_css_set_property(css, "font-variation-settings", "'wght' 650");
    sp_repr_css_set_property(css, "font-feature-settings", "'liga' 0");
    sp_repr_css_set_property(css, "font-size", "150%");
    sp_repr_css_set_property(css, "text-decoration-line", "underline");
    sp_repr_css_set_property(css, "text-decoration-color", "#123456");
    sp_repr_css_set_property(css, "-inkscape-font-variant-caps-mode", "synthesized");
    sp_repr_css_set(span->getRepr(), css, "style"); sp_repr_css_attr_unref(css); doc->ensureUpToDate();
    ObjectSet selected(doc.get()); selected.add(doc->getObjectById("art"));
    auto bounds = selected.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto before = fingerprint(*doc);
    auto copy = reopen(validate(Library::stage_selection(selected))); ASSERT_TRUE(copy);
    ASSERT_TRUE(copy->getObjectById("span"));
    auto persisted = std::string(copy->getObjectById("span")->getRepr()->attribute("style"));
    EXPECT_NE(persisted.find("font-variation-settings"), std::string::npos);
    EXPECT_NE(persisted.find("synthesized"), std::string::npos);
    EXPECT_EQ(difference(render(*doc, {0, 0}), render(*copy, bounds->min())), 0u);
    EXPECT_EQ(fingerprint(*doc), before);
}

TEST_F(ArtworkLibraryPreflightNative, ActualNestedViewportStagePassesAndPreservesNativeGeometry)
{
    auto doc = source("<svg id='viewport' x='10' y='5' width='80' height='40' viewBox='0 0 40 20' preserveAspectRatio='xMidYMid meet'>"
                      "<rect id='art' x='2' y='3' width='12' height='8' fill='#123456'/></svg>"); ASSERT_TRUE(doc);
    ObjectSet selection(doc.get()); selection.add(doc->getObjectById("art"));
    auto bounds = selection.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto staged = Library::stage_selection(selection); auto a = validate(staged); auto parsed = reopen(a); ASSERT_TRUE(parsed);
    ASSERT_TRUE(parsed->getObjectById("viewport")); ASSERT_TRUE(parsed->getObjectById("art"));
    EXPECT_GE(count(parsed->getReprRoot(), "svg:svg"), 3u);
    EXPECT_STREQ(parsed->getObjectById("viewport")->getRepr()->attribute("viewBox"), "0 0 40 20");
    EXPECT_EQ(difference(render(*doc, {0, 0}), render(*parsed, bounds->min())), 0u);
}

TEST_F(ArtworkLibraryPreflightNative, FilteredGradientArtworkStagesAdmitsReopensAndRemainsEditable)
{
    auto doc = source("<defs><linearGradient id='base' gradientUnits='userSpaceOnUse' x1='0' y1='0' x2='70' y2='20'>"
                      "<stop offset='0' stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient>"
                      "<linearGradient id='paint' xlink:href='#base' x1='0' y1='0' x2='70' y2='20'/>"
                      "<filter id='f' x='-0.2' y='-0.2' width='1.4' height='1.4'><feGaussianBlur id='blur' stdDeviation='1'/></filter></defs>"
                      "<rect id='art' x='10' y='10' width='40' height='30' fill='url(#paint)' filter='url(#f)'/>"); ASSERT_TRUE(doc);
    ObjectSet selection(doc.get()); selection.add(doc->getObjectById("art"));
    auto bounds = selection.documentBounds(SPItem::VISUAL_BBOX); ASSERT_TRUE(bounds);
    auto staged = Library::stage_selection(selection); auto a = validate(staged); auto parsed = reopen(a); ASSERT_TRUE(parsed);
    EXPECT_EQ(a.stats().filter_primitives, 1u);
    ASSERT_TRUE(parsed->getObjectById("blur")); ASSERT_TRUE(parsed->getObjectById("paint"));
    auto paint = cast<SPGradient>(parsed->getObjectById("paint")); ASSERT_TRUE(paint);
    EXPECT_EQ(paint->fetchUnits(), SP_GRADIENT_UNITS_USERSPACEONUSE);
    auto expected = render(*doc, {0, 0}); auto actual = render(*parsed, bounds->min());
    EXPECT_EQ(difference(expected, actual), 0u);
    // Mutation positive control: retained filter is live native editing data.
    parsed->getObjectById("blur")->getRepr()->setAttribute("stdDeviation", "3"); parsed->ensureUpToDate();
    EXPECT_GT(difference(actual, render(*parsed, bounds->min())), 0u);
    // Low-level native writer -> preflight -> reopen a second time, no disk.
    ObjectSet again(parsed.get()); again.add(parsed->getObjectById("art"));
    auto second = Library::stage_selection(again); auto twice = reopen(validate(second)); ASSERT_TRUE(twice);
    ASSERT_TRUE(twice->getObjectById("blur"));
    EXPECT_STREQ(twice->getObjectById("blur")->getRepr()->attribute("stdDeviation"), "3");
}

TEST_F(ArtworkLibraryPreflightNative, InvalidPaintActuallyInheritsNativeServerButIsRejectedByPreflight)
{
    auto doc = source("<defs><linearGradient id='p'><stop offset='0' stop-color='red'/></linearGradient></defs>"
                      "<g fill='url(#p)'><g fill='bogus'><rect id='art' width='30' height='20'/></g></g>"); ASSERT_TRUE(doc);
    auto art = cast<SPItem>(doc->getObjectById("art")); ASSERT_TRUE(art); ASSERT_TRUE(art->style);
    ASSERT_TRUE(art->style->fill.isPaintserver()); // Native oracle for Banach's counterexample.
    ObjectSet selection(doc.get()); selection.add(art);
    auto staged = Library::stage_selection(selection);
    EXPECT_THROW((void)validate(staged), Library::SvgPreflightError);
}

TEST_F(ArtworkLibraryPreflightNative, LightburnReaderConverterAndAdmissionPreserveMillimetersForEveryMirror)
{
    for (bool mx : {false, true}) for (bool my : {false, true}) {
        auto xml = std::string("<LightBurnShapes FormatVersion='1' MirrorX='") + (mx ? "True" : "False") +
            "' MirrorY='" + (my ? "True" : "False") + "'><Shape Type='Rect' W='10' H='6' Cr='0'>"
            "<XForm>1 0 0 1 0 0</XForm></Shape></LightBurnShapes>";
        auto original = lightburn_archive(xml);
        auto hash = Library::artwork_sha256(original);
        auto archive = Library::LbartArchive::open(original);
        ASSERT_EQ(archive.entries().size(), 1u);
        EXPECT_DOUBLE_EQ(archive.entries()[0].extent_x, 777.);
        auto payload = archive.read_artwork(0);
        auto converted = Library::convert_lightburn_shapes_v1_draft(std::string(payload.begin(), payload.end()));
        EXPECT_DOUBLE_EQ(converted.width_mm, 12);
        EXPECT_DOUBLE_EQ(converted.height_mm, 8);
        Library::Bytes bytes(converted.svg.begin(), converted.svg.end());
        auto admitted = Library::preflight_svg(bytes, {"b203e8e9-640c-4195-b0ea-f9b790245678",
            Library::artwork_sha256(bytes), converted.width_mm, converted.height_mm});
        auto doc = reopen(admitted); ASSERT_TRUE(doc);
        auto repr = first_element(doc->getReprRoot(), "svg:rect"); ASSERT_TRUE(repr);
        auto item = cast<SPItem>(doc->getObjectByRepr(repr)); ASSERT_TRUE(item);
        auto bounds = item->documentGeometricBounds(); ASSERT_TRUE(bounds);
        EXPECT_NEAR(bounds->width() * 25.4 / 96, 10, 1e-10);
        EXPECT_NEAR(bounds->height() * 25.4 / 96, 6, 1e-10);
        EXPECT_EQ(count(doc->getReprRoot(), "svg:image"), 0u);
        EXPECT_EQ(Library::artwork_sha256(original), hash);
        // Editing remains native rectangle data, without changing archive bytes.
        repr->setAttribute("width", "5"); doc->ensureUpToDate();
        auto edited = item->documentGeometricBounds(); ASSERT_TRUE(edited);
        EXPECT_NEAR(edited->width() * 25.4 / 96, 5, 1e-10);
        EXPECT_EQ(archive.read_artwork(0), payload);
    }
}

TEST_F(ArtworkLibraryPreflightNative, LightburnNonuniformRotatedGroupRemainsEditableAtPhysicalSize)
{
    auto converted = Library::convert_lightburn_shapes_v1_draft(
        "<LightBurnShapes FormatVersion='1' MirrorX='True' MirrorY='True'>"
        "<Shape Type='Group'><XForm>0 2 -3 0 20 30</XForm><Children>"
        "<Shape Type='Rect' W='10' H='6' Cr='0'><XForm>1 0 0 1 0 0</XForm><Tabs>1,0.5</Tabs></Shape>"
        "</Children></Shape></LightBurnShapes>");
    ASSERT_EQ(converted.unapplied_laser_tabs.size(), 1u);
    EXPECT_EQ(converted.unapplied_laser_tabs[0].source_text, "1,0.5");
    Library::Bytes bytes(converted.svg.begin(), converted.svg.end());
    auto admitted = Library::preflight_svg(bytes, {"b203e8e9-640c-4195-b0ea-f9b790245678",
        Library::artwork_sha256(bytes), converted.width_mm, converted.height_mm});
    auto doc = reopen(admitted); ASSERT_TRUE(doc);
    auto repr = first_element(doc->getReprRoot(), "svg:rect"); ASSERT_TRUE(repr);
    auto item = cast<SPItem>(doc->getObjectByRepr(repr)); ASSERT_TRUE(item);
    auto bounds = item->documentGeometricBounds(); ASSERT_TRUE(bounds);
    EXPECT_NEAR(bounds->width() * 25.4 / 96, 18, 1e-10);
    EXPECT_NEAR(bounds->height() * 25.4 / 96, 20, 1e-10);
    EXPECT_EQ(count(doc->getReprRoot(), "svg:g"), 2u);
    EXPECT_EQ(count(doc->getReprRoot(), "svg:image"), 0u);
}


TEST_F(ArtworkLibraryPreflightNative, PatternAliasUsesOneEffectiveTransformAndKeepsLocalContent)
{
    for (bool own_content : {false, true}) for (bool local_transform : {false, true}) {
        SCOPED_TRACE(own_content);
        SCOPED_TRACE(local_transform);
        auto xml = std::string(
            "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
            "width='10' height='10' viewBox='0 0 10 10'>"
            "<defs><pattern id='base' patternUnits='userSpaceOnUse' width='1' height='1' "
            "patternTransform='scale(16)'><rect id='base-child' width='1' height='1'/></pattern>"
            "<pattern id='p' xlink:href='#base' patternUnits='userSpaceOnUse' width='1' height='1'") +
            (local_transform ? " patternTransform='scale(2)'" : "") +
            (own_content ? "><rect id='own-child' width='2' height='1'/></pattern>" : "/>") +
            "</defs><rect width='10' height='10' fill='url(#p)'/></svg>";
        Library::Bytes input(xml.begin(), xml.end());
        auto admitted = Library::preflight_svg(input, {"b203e8e9-640c-4195-b0ea-f9b790245678",
            Library::artwork_sha256(input), 10 * 25.4 / 96, 10 * 25.4 / 96});
        EXPECT_EQ(*admitted.svg_bytes(), xml);
        auto doc = reopen(admitted); ASSERT_TRUE(doc); // No rendering needed.
        auto base = cast<SPPattern>(doc->getObjectById("base")); ASSERT_TRUE(base);
        auto alias = cast<SPPattern>(doc->getObjectById("p")); ASSERT_TRUE(alias);
        auto scale = local_transform ? 2. : 16.;
        auto const &effective = alias->getTransform();
        EXPECT_DOUBLE_EQ(effective[0], scale);
        EXPECT_DOUBLE_EQ(effective[3], scale);
        for (unsigned i : {1u, 2u, 4u, 5u}) EXPECT_DOUBLE_EQ(effective[i], 0.);
        EXPECT_DOUBLE_EQ(alias->width(), 1.);
        EXPECT_DOUBLE_EQ(alias->height(), 1.);
        EXPECT_DOUBLE_EQ(alias->width() * effective[0], scale);
        EXPECT_EQ(alias->rootPattern(), own_content ? alias : base);
        EXPECT_DOUBLE_EQ(base->getTransform()[0], 16.);
        EXPECT_EQ(*admitted.svg_bytes(), xml);
    }
}
