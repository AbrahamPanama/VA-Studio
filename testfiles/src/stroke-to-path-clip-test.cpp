// SPDX-License-Identifier: GPL-2.0-or-later
// BUG-030: native Stroke to Path outcomes, without a renderer or desktop.
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include "doc-per-case-test.h"
#include "document-undo.h"
#include "display/cairo-utils.h"
#include "helper/pixbuf-ops.h"
#include "preferences.h"
#include "live_effects/effect.h"
#include "live_effects/lpe-bspline.h"
#include "live_effects/lpeobject.h"
#include "object/object-set.h"
#include "object/sp-clippath.h"
#include "object/sp-defs.h"
#include "object/sp-mask.h"
#include "object/sp-item.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "path/path-outline.h"
#include "svg/svg.h"
#include "style.h"
#include "xml/attribute-record.h"
#include "xml/repr.h"

namespace {
std::vector<unsigned char> rendered(SPDocument &doc, std::string const &filename = {})
{
    std::unique_ptr<Inkscape::Pixbuf> pixbuf(sp_generate_internal_bitmap(
        &doc, Geom::Rect(Geom::Point(0, 0), Geom::Point(200, 200)), 96.0));
    if (!pixbuf) { ADD_FAILURE() << "Native rendering failed"; return {}; }
    auto surface = pixbuf->getSurfaceRaw();
    cairo_surface_flush(surface);
    if (!filename.empty()) EXPECT_EQ(cairo_surface_write_to_png(surface, filename.c_str()), CAIRO_STATUS_SUCCESS);
    auto bytes = cairo_image_surface_get_data(surface);
    return {bytes, bytes + cairo_image_surface_get_stride(surface) * cairo_image_surface_get_height(surface)};
}
void same_pixels(std::vector<unsigned char> const &actual, std::vector<unsigned char> const &expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    ASSERT_FALSE(actual.empty());
    size_t differences = 0;
    for (size_t i = 0; i < actual.size(); ++i) differences += actual[i] != expected[i];
    EXPECT_EQ(differences, 0u);
}
std::string xml(Inkscape::XML::Node const *node)
{
    // XML attribute order is not significant; retain every value, child and text byte.
    std::string out = std::string(node->name() ? node->name() : "") + '[';
    std::vector<std::string> attrs;
    for (auto const &a : node->attributeList()) {
        attrs.emplace_back(std::string(g_quark_to_string(a.key)) + '=' + a.value.pointer());
    }
    std::sort(attrs.begin(), attrs.end());
    for (auto const &a : attrs) out += std::to_string(a.size()) + ':' + a;
    out += ']';
    if (node->content()) out += node->content();
    for (auto child = node->firstChild(); child; child = child->next()) out += xml(child);
    return out + '/';
}
std::string fixture(std::string const &body)
{
    return R"(<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'
        xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd' width='200' height='200' viewBox='0 0 200 200'>
        <defs id='defs'>
        <clipPath id='clip' clipPathUnits='userSpaceOnUse'><path d='M5,5 H25 V25 H5Z'/></clipPath>
        <mask id='mask' maskUnits='userSpaceOnUse' maskContentUnits='userSpaceOnUse'
              x='0' y='0' width='30' height='30'><path fill='white' d='M5,5 H25 V25 H5Z'/></mask>
        <marker id='marker' markerUnits='userSpaceOnUse' markerWidth='10' markerHeight='10'
                refX='0' refY='0' orient='0' overflow='visible'><path d='M-10,-10 H10 V10 H-10Z' fill='red'/></marker>
        </defs><g id='parent' transform='translate(17,29) rotate(13) scale(1.5,0.7)'>)"
        + body + "</g></svg>";
}
std::string shape(char const *style, std::string const &effects, bool text = false)
{
    auto attrs = std::string(" id='target' transform='translate(3,4) rotate(7)' style='") + style + "' " + effects;
    bool const markers_only = std::string(style).find("fill:none;stroke:none") != std::string::npos;
    // Keep the unpainted centreline within the marker's bounds: SPShape's
    // native visual bbox also includes that centreline even when unpainted.
    auto const d = markers_only ? "M0,0 H6 V6 H0Z" : "M0,0 H30 V30 H0Z";
    return text ? "<text" + attrs + " x='0' y='25' font-size='40' font-family='sans-serif'>HH</text>"
                : "<path" + attrs + " d='" + d + "'/>";
}
int references(Inkscape::XML::Node const *node, char const *key)
{
    int n = node->attribute(key) ? 1 : 0;
    for (auto child = node->firstChild(); child; child = child->next()) n += references(child, key);
    return n;
}
void same_bounds(Geom::OptRect const &a, Geom::OptRect const &b)
{
    ASSERT_EQ(bool(a), bool(b));
    if (!a) return;
    for (unsigned d = 0; d < 2; ++d) {
        EXPECT_NEAR(a->min()[d], b->min()[d], 1e-7);
        EXPECT_NEAR(a->max()[d], b->max()[d], 1e-7);
    }
}
void contained(Geom::OptRect const &visual, Geom::OptRect const &limit)
{
    ASSERT_TRUE(limit);
    if (!visual) return; // An empty clipped result is contained in every clip.
    for (unsigned d = 0; d < 2; ++d) {
        EXPECT_GE(visual->min()[d], limit->min()[d] - 1e-7);
        EXPECT_LE(visual->max()[d], limit->max()[d] + 1e-7);
    }
}
}
class StrokeToPathClip : public DocPerCaseTest {
protected:
    std::unique_ptr<SPDocument> make(std::string const &body) {
        auto doc = SPDocument::createNewDocFromMem(fixture(body));
        doc->ensureUpToDate();
        Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
        Inkscape::DocumentUndo::clearUndo(doc.get());
        return doc;
    }
    SPItem *item(SPDocument &doc, char const *id = "target") { return cast<SPItem>(doc.getObjectById(id)); }
    void outcome(char const *style, bool mask, bool text = false, bool legacy = false) {
        auto doc = make(shape(style, mask ? "clip-path='url(#clip)' mask='url(#mask)'" : "clip-path='url(#clip)'", text));
        auto original = item(*doc);
        ASSERT_TRUE(original);
        auto clip = original->getClipObject();
        auto mask_obj = original->getMaskObject();
        ASSERT_TRUE(clip);
        auto const tr = original->i2doc_affine();
        auto const clip_bounds = clip->geometricBounds(tr);
        auto const before_bounds = original->documentVisualBounds();
        auto const before = xml(doc->getReprRoot());
        auto const defs = xml(doc->getObjectById("defs")->getRepr());
        Inkscape::ObjectSet selection(doc.get()); selection.add(original);
        bool expected_change = text || std::string(style).find("marker-end") != std::string::npos ||
                               std::string(style).find("stroke:none") == std::string::npos;
        EXPECT_EQ(selection.strokesToPaths(legacy), expected_change);
        doc->ensureUpToDate();
        auto result = item(*doc);
        ASSERT_TRUE(result);
        ASSERT_EQ(result->getClipObject(), clip);
        EXPECT_EQ(result->getMaskObject(), mask_obj);
        EXPECT_EQ(references(result->getRepr(), "clip-path"), 1);
        EXPECT_EQ(references(result->getRepr(), "mask"), mask ? 1 : 0);
        EXPECT_EQ(xml(doc->getObjectById("defs")->getRepr()), defs);
        for (unsigned i = 0; i < 6; ++i) EXPECT_NEAR(result->i2doc_affine()[i], tr[i], 1e-10);
        same_bounds(result->getClipObject()->geometricBounds(result->i2doc_affine()), clip_bounds);
        contained(result->documentVisualBounds(), clip_bounds);
        if (!text) same_bounds(result->documentVisualBounds(), before_bounds);
        if (expected_change) {
            ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
            EXPECT_EQ(xml(doc->getReprRoot()), before);
            EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
            ASSERT_TRUE(Inkscape::DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
            EXPECT_EQ(item(*doc)->getClipObject(), clip);
        } else {
            EXPECT_EQ(xml(doc->getReprRoot()), before);
            EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
        }
    }
};
TEST_F(StrokeToPathClip, StrokeOnly) { outcome("fill:none;stroke:blue;stroke-width:10", false); }
TEST_F(StrokeToPathClip, FillOnly) { outcome("fill:blue;stroke:none", false); }
TEST_F(StrokeToPathClip, FillAndStroke) { outcome("fill:blue;stroke:red;stroke-width:10", false); }
TEST_F(StrokeToPathClip, StrokeOnlyMask) { outcome("fill:none;stroke:blue;stroke-width:10", true); }
TEST_F(StrokeToPathClip, FillOnlyMask) { outcome("fill:blue;stroke:none", true); }
TEST_F(StrokeToPathClip, FillAndStrokeMask) { outcome("fill:blue;stroke:red;stroke-width:10", true); }
TEST_F(StrokeToPathClip, StrokeAndMarkers) { outcome("fill:none;stroke:blue;stroke-width:10;marker-end:url(#marker)", true); }
TEST_F(StrokeToPathClip, FillStrokeAndMarkers) { outcome("fill:blue;stroke:red;stroke-width:10;marker-end:url(#marker)", true); }
TEST_F(StrokeToPathClip, MarkersOnly) { outcome("fill:none;stroke:none;marker-end:url(#marker)", true); }
TEST_F(StrokeToPathClip, StrokeOnlyText) { outcome("fill:none;stroke:blue;stroke-width:10", true, true); }
TEST_F(StrokeToPathClip, FillOnlyText) { outcome("fill:blue;stroke:none", true, true); }
TEST_F(StrokeToPathClip, FillStrokeText) { outcome("fill:blue;stroke:red;stroke-width:10", true, true); }
TEST_F(StrokeToPathClip, LegacyStroke) { outcome("fill:none;stroke:blue;stroke-width:10", true, false, true); }
TEST_F(StrokeToPathClip, LegacyFillStroke) { outcome("fill:blue;stroke:red;stroke-width:10", true, false, true); }
TEST_F(StrokeToPathClip, MixedNestedGroup)
{
    auto doc = make(R"(<g id='target' clip-path='url(#clip)' mask='url(#mask)' transform='translate(3,4)'>
        <path id='stroke' d='M0,0 H30 V30 H0Z' style='fill:none;stroke:blue;stroke-width:10' clip-path='url(#clip)'/>
        <g id='nested' transform='translate(2,1)'>
        <path id='both' d='M0,0 H30 V30 H0Z' style='fill:blue;stroke:red;stroke-width:10' clip-path='url(#clip)' mask='url(#mask)'/>
        <path id='fill' d='M0,0 H30 V30 H0Z' style='fill:blue;stroke:none' clip-path='url(#clip)'/>
        <image id='incompatible' width='3' height='4'/></g></g>)");
    auto const before = xml(doc->getReprRoot());
    auto const defs = xml(doc->getObjectById("defs")->getRepr());
    auto const incompatible = xml(doc->getObjectById("incompatible")->getRepr());
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
    ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
    for (auto id : {"target", "stroke", "both", "fill"}) {
        auto result = item(*doc, id); ASSERT_TRUE(result);
        EXPECT_EQ(result->getClipObject(), doc->getObjectById("clip"));
        contained(result->documentVisualBounds(), result->getClipObject()->geometricBounds(result->i2doc_affine()));
    }
    EXPECT_EQ(references(item(*doc)->getRepr(), "clip-path"), 4);
    EXPECT_EQ(references(item(*doc)->getRepr(), "mask"), 2);
    EXPECT_EQ(xml(doc->getObjectById("defs")->getRepr()), defs);
    EXPECT_EQ(xml(doc->getObjectById("incompatible")->getRepr()), incompatible);
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(xml(doc->getReprRoot()), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}
TEST_F(StrokeToPathClip, UnclippedGeometry)
{
    for (auto style : {"fill:none;stroke:blue;stroke-width:10", "fill:blue;stroke:red;stroke-width:10", "fill:blue;stroke:none"}) {
        SCOPED_TRACE(style);
        auto doc = make(shape(style, "")); auto original = item(*doc);
        Geom::PathVector fill, stroke; ASSERT_TRUE(item_find_paths(original, fill, stroke));
        auto const before_bounds = original->documentVisualBounds();
        auto const before = xml(doc->getReprRoot());
        Inkscape::ObjectSet selection(doc.get()); selection.add(original);
        bool changed = selection.strokesToPaths(); doc->ensureUpToDate();
        auto result = item(*doc); ASSERT_TRUE(result);
        EXPECT_EQ(references(result->getRepr(), "clip-path"), 0);
        EXPECT_EQ(references(result->getRepr(), "mask"), 0);
        same_bounds(result->documentVisualBounds(), before_bounds);
        if (changed) {
            auto path = cast<SPPath>(result);
            if (path) EXPECT_EQ(sp_svg_write_path(*path->curve()), sp_svg_write_path(stroke));
            else {
                ASSERT_EQ(result->childList(false).size(), 2u);
                auto fill_result = cast<SPPath>(result->firstChild());
                auto stroke_result = cast<SPPath>(result->lastChild());
                ASSERT_TRUE(fill_result); ASSERT_TRUE(stroke_result);
                EXPECT_EQ(sp_svg_write_path(*fill_result->curve()), sp_svg_write_path(fill));
                EXPECT_EQ(sp_svg_write_path(*stroke_result->curve()), sp_svg_write_path(stroke));
            }
            ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        }
        EXPECT_EQ(xml(doc->getReprRoot()), before);
    }
}
TEST_F(StrokeToPathClip, OwnerTile)
{
    // A caller can supply the preserved owner fixture; otherwise exercise the
    // same 560 stroke-only / 860 filled clipped-dot counts deterministically.
    std::unique_ptr<SPDocument> doc;
    if (auto filename = std::getenv("BUG030_OWNER_SVG")) {
        doc = SPDocument::createNewDoc(filename, true);
    } else {
        std::string dots = "<g id='owner-tile'>";
        for (int i = 0; i < 2924; ++i) {
            dots += "<path id='dot" + std::to_string(i) + "' d='M0,0 H30 V30 H0Z' style='fill:";
            dots += i < 560 ? "none" : "blue";
            dots += ";stroke:red;stroke-width:10'";
            if (i < 1420) dots += " clip-path='url(#clip)'";
            dots += "/>";
        }
        doc = make(dots + "</g>");
    }
    ASSERT_TRUE(doc); doc->ensureUpToDate();
    Inkscape::DocumentUndo::setUndoSensitive(doc.get(), true);
    Inkscape::DocumentUndo::clearUndo(doc.get());
    struct Captured { std::string id; SPClipPath *clip; Geom::Affine tr; std::string outline; };
    std::vector<Captured> captured;
    auto tile = item(*doc, "owner-tile"); ASSERT_TRUE(tile);
    std::function<void(SPObject *)> capture = [&](SPObject *node) {
        if (auto leaf = cast<SPItem>(node); leaf && leaf->getClipObject()) {
            std::string outline;
            if (leaf->style->fill.isNone()) {
                Geom::PathVector fill, stroke;
                ASSERT_TRUE(item_find_paths(leaf, fill, stroke));
                outline = sp_svg_write_path(stroke);
            }
            captured.push_back({leaf->getId(), leaf->getClipObject(), leaf->i2doc_affine(), outline});
        }
        for (auto &child : node->children) capture(&child);
    };
    capture(tile); ASSERT_EQ(captured.size(), 1420u);
    EXPECT_EQ(std::count_if(captured.begin(), captured.end(), [](auto const &c) { return !c.outline.empty(); }), 560);
    auto const original_defs = xml(doc->getDefs()->getRepr());
    auto const original_bytes = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto const before = xml(doc->getReprRoot());
    Inkscape::ObjectSet selection(doc.get()); selection.add(tile);
    ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
    EXPECT_EQ(references(tile->getRepr(), "clip-path"), 1420);
    EXPECT_EQ(xml(doc->getDefs()->getRepr()), original_defs);
    for (auto const &old : captured) {
        SCOPED_TRACE(old.id);
        auto result = item(*doc, old.id.c_str()); ASSERT_TRUE(result);
        EXPECT_EQ(result->getClipObject(), old.clip);
        EXPECT_EQ(references(result->getRepr(), "clip-path"), 1);
        for (unsigned i = 0; i < 6; ++i) EXPECT_NEAR(result->i2doc_affine()[i], old.tr[i], 1e-10);
        contained(result->documentVisualBounds(), old.clip->geometricBounds(old.tr));
        if (!old.outline.empty()) {
            auto path = cast<SPPath>(result); ASSERT_TRUE(path);
            EXPECT_EQ(std::string(path->getRepr()->attribute("d")), old.outline);
        }
    }
    auto const after = xml(doc->getReprRoot());
    if (auto output = std::getenv("BUG030_OWNER_OUTPUT")) {
        std::ofstream file(std::string(output) + "/owner-after.svg"); ASSERT_TRUE(file.good());
        file << sp_repr_save_buf(doc->getReprDoc()); ASSERT_TRUE(file.good());
    }
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(xml(doc->getReprRoot()), before);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), original_bytes);
    if (auto output = std::getenv("BUG030_OWNER_OUTPUT")) {
        std::ofstream file(std::string(output) + "/owner-undo.svg"); ASSERT_TRUE(file.good());
        file << sp_repr_save_buf(doc->getReprDoc()); ASSERT_TRUE(file.good());
    }
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(xml(doc->getReprRoot()), after);
}
TEST_F(StrokeToPathClip, CssClipAndMask)
{
    auto doc = make(shape("fill:none;stroke:blue;stroke-width:10;clip-path:url(#clip);mask:url(#mask)", ""));
    auto const before = xml(doc->getReprRoot());
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
    ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
    auto result = item(*doc); ASSERT_TRUE(result);
    EXPECT_EQ(result->getClipObject(), doc->getObjectById("clip"));
    EXPECT_EQ(result->getMaskObject(), doc->getObjectById("mask"));
    EXPECT_EQ(references(result->getRepr(), "clip-path"), 1);
    EXPECT_EQ(references(result->getRepr(), "mask"), 1);
    contained(result->documentVisualBounds(), result->getClipObject()->geometricBounds(result->i2doc_affine()));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(xml(doc->getReprRoot()), before);
}
TEST_F(StrokeToPathClip, BoundingBoxResourcesAreEquivalentAndShared)
{
    for (bool filled : {false, true}) {
        SCOPED_TRACE(filled);
        auto body = std::string(R"(<defs><clipPath id='bbox-clip' clipPathUnits='objectBoundingBox'>
              <path id='clip-geometry' d='M0.25,0.25 H0.75 V0.75 H0.25Z'/></clipPath>
              <mask id='bbox-mask' maskContentUnits='objectBoundingBox' x='10%' y='20%' width='80%' height='70%'>
              <path id='mask-geometry' fill='white' d='M0.25,0.25 H0.75 V0.75 H0.25Z'/></mask></defs>
              <g id='target'>)");
        for (auto id : {"a", "b"}) {
            body += std::string("<path id='") + id + "' d='M0,0 H30 V30 H0Z' clip-path='url(#bbox-clip)' mask='url(#bbox-mask)'"
                    " transform='translate(3,4) rotate(7)' style='fill:" + (filled ? "blue" : "none") + ";stroke:red;stroke-width:20'/>";
        }
        auto doc = make(body + "</g>");
        auto original = item(*doc, "a"); ASSERT_TRUE(original);
        auto const tr = original->i2doc_affine();
        auto const bbox = original->geometricBounds(); ASSERT_TRUE(bbox);
        auto const units = Geom::Scale(bbox->dimensions()) * Geom::Translate(bbox->min()) * tr;
        auto const old_clip_bounds = original->getClipObject()->geometricBounds(units);
        auto const old_clip_geometry = sp_svg_write_path(original->getClipObject()->getPathVector(units));
        auto const old_mask_bounds = original->getMaskObject()->geometricBounds(units);
        auto const clip_xml = xml(doc->getObjectById("bbox-clip")->getRepr());
        auto const mask_xml = xml(doc->getObjectById("bbox-mask")->getRepr());
        auto const before = xml(doc->getReprRoot());
        Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
        ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
        auto a = item(*doc, "a"), b = item(*doc, "b"); ASSERT_TRUE(a); ASSERT_TRUE(b);
        auto clip = a->getClipObject(); auto mask = a->getMaskObject(); ASSERT_TRUE(clip); ASSERT_TRUE(mask);
        EXPECT_EQ(clip, b->getClipObject()); EXPECT_EQ(mask, b->getMaskObject());
        EXPECT_FALSE(clip->clippath_units()); EXPECT_FALSE(mask->mask_content_units());
        same_bounds(clip->geometricBounds(a->i2doc_affine()), old_clip_bounds);
        EXPECT_EQ(sp_svg_write_path(clip->getPathVector(a->i2doc_affine())), old_clip_geometry);
        same_bounds(mask->geometricBounds(a->i2doc_affine()), old_mask_bounds);
        contained(a->documentVisualBounds(), old_clip_bounds);
        EXPECT_EQ(references(a->getRepr(), "clip-path"), 1);
        EXPECT_EQ(references(a->getRepr(), "mask"), 1);
        EXPECT_EQ(xml(doc->getObjectById("bbox-clip")->getRepr()), clip_xml);
        EXPECT_EQ(xml(doc->getObjectById("bbox-mask")->getRepr()), mask_xml);
        EXPECT_STREQ(mask->getRepr()->attribute("maskUnits"), "userSpaceOnUse");
        EXPECT_STREQ(mask->getRepr()->attribute("x"), "3");
        EXPECT_STREQ(mask->getRepr()->attribute("y"), "6");
        EXPECT_STREQ(mask->getRepr()->attribute("width"), "24");
        EXPECT_STREQ(mask->getRepr()->attribute("height"), "21");
        // Equivalent resources contain references, not duplicated paths.
        EXPECT_EQ(clip->getRepr()->firstChild()->name(), std::string("svg:use"));
        EXPECT_STREQ(clip->getRepr()->firstChild()->attribute("xlink:href"), "#clip-geometry");
        auto const after = xml(doc->getReprRoot());
        auto serialized = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto reopened = SPDocument::createNewDocFromMem(serialized); ASSERT_TRUE(reopened); reopened->ensureUpToDate();
        auto reopened_a = item(*reopened, "a"); ASSERT_TRUE(reopened_a); ASSERT_TRUE(reopened_a->getClipObject());
        same_bounds(reopened_a->getClipObject()->geometricBounds(reopened_a->i2doc_affine()), old_clip_bounds);
        contained(reopened_a->documentVisualBounds(), old_clip_bounds);
        ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(xml(doc->getReprRoot()), before);
        EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
        ASSERT_TRUE(Inkscape::DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(xml(doc->getReprRoot()), after);
    }
}
TEST_F(StrokeToPathClip, UnclippedPaintOrderOpacityAndMarkers)
{
    for (auto style : {"fill:none;stroke:blue;stroke-width:10;opacity:0.4;marker-end:url(#marker)",
                       "fill:blue;stroke:red;stroke-width:10;opacity:0.4;paint-order:stroke fill markers",
                       "fill:none;stroke:none;marker-end:url(#marker);opacity:0.4"}) {
        SCOPED_TRACE(style);
        auto doc = make(shape(style, "")); auto original = item(*doc);
        auto const bounds = original->documentVisualBounds();
        auto const before = xml(doc->getReprRoot());
        Inkscape::ObjectSet selection(doc.get()); selection.add(original);
        ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
        auto result = item(*doc); ASSERT_TRUE(result);
        EXPECT_EQ(references(result->getRepr(), "clip-path"), 0);
        EXPECT_EQ(references(result->getRepr(), "mask"), 0);
        same_bounds(result->documentVisualBounds(), bounds);
        EXPECT_NEAR((static_cast<double>(result->style->opacity.value) / SP_SCALE24_MAX), 0.4, 1e-6);
        if (std::string(style).find("paint-order") != std::string::npos) {
            auto first = cast<SPPath>(result->firstChild()); ASSERT_TRUE(first);
            // Stroke paints first; its former red stroke is now a red fill.
            ASSERT_TRUE(first->style->fill.isColor());
            EXPECT_EQ(first->style->fill.getColor().toRGBA(), 0xff0000ffu);
        }
        ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(xml(doc->getReprRoot()), before);
    }
}

TEST_F(StrokeToPathClip, LegacyFillOnly) { outcome("fill:blue;stroke:none", true, false, true); }
TEST_F(StrokeToPathClip, LegacyMarkersOnly) { outcome("fill:none;stroke:none;marker-end:url(#marker)", true, false, true); }
TEST_F(StrokeToPathClip, LegacyStrokeAndMarkers) { outcome("fill:none;stroke:blue;stroke-width:10;marker-end:url(#marker)", true, false, true); }
TEST_F(StrokeToPathClip, LegacyTextIsUntouched)
{
    auto doc = make(shape("fill:none;stroke:blue;stroke-width:10", "clip-path='url(#clip)' mask='url(#mask)'", true));
    auto const before = xml(doc->getReprRoot());
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
    EXPECT_FALSE(selection.strokesToPaths(true)); doc->ensureUpToDate();
    EXPECT_EQ(xml(doc->getReprRoot()), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}

TEST_F(StrokeToPathClip, ByteExactUndoAcrossMixedReplacements)
{
    auto doc = make(R"(<defs><clipPath id='bbox-clip' clipPathUnits='objectBoundingBox'>
        <path id='clip-geometry' d='M0.25,0.25 H0.75 V0.75 H0.25Z'/></clipPath></defs>
        <g id='target' clip-path='url(#clip)' mask='url(#mask)'>
        <path id='a' d='M0,0 H30 V30 H0Z' style='fill:none;stroke:blue;stroke-width:10' clip-path='url(#clip)'/>
        <path id='b' d='M0,0 H30 V30 H0Z' style='fill:blue;stroke:red;stroke-width:10' clip-path='url(#bbox-clip)'/>
        <text id='txt' x='0' y='25' font-family='sans-serif' font-size='40'
            style='fill:none;stroke:blue;stroke-width:10' clip-path='url(#clip)'>HH</text>
        </g>)");
    auto const before = sp_repr_save_buf(doc->getReprDoc()).raw();
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
    ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}

TEST_F(StrokeToPathClip, BoundingBoxGroupAndText)
{
    for (bool text : {false, true}) {
        SCOPED_TRACE(text);
        std::string body = R"(<defs><clipPath id='bbox-clip' clipPathUnits='objectBoundingBox'>
            <path id='clip-geometry' d='M0.25,0.25 H0.75 V0.75 H0.25Z'/></clipPath></defs>)";
        body += text ? R"(<text id='target' x='0' y='25' font-family='sans-serif' font-size='40'
                transform='translate(3,4) rotate(7)' clip-path='url(#bbox-clip)' style='stroke:blue;stroke-width:10'>
                <tspan style='fill:none'>H</tspan><tspan style='fill:red'>H</tspan></text>)"
                     : R"(<g id='target' transform='translate(3,4) rotate(7)' clip-path='url(#bbox-clip)'>
                <path d='M0,0 H30 V30 H0Z' style='fill:none;stroke:blue;stroke-width:10'/>
                <path d='M0,0 H30 V30 H0Z' style='fill:red;stroke:none'/></g>)";
        auto doc = make(body); auto original = item(*doc); ASSERT_TRUE(original);
        auto const bbox = original->geometricBounds(); ASSERT_TRUE(bbox);
        auto const tr = original->i2doc_affine();
        auto const units = Geom::Scale(bbox->dimensions()) * Geom::Translate(bbox->min()) * tr;
        auto const clip_bounds = original->getClipObject()->geometricBounds(units);
        auto const geometry = sp_svg_write_path(original->getClipObject()->getPathVector(units));
        auto const before = xml(doc->getReprRoot());
        Inkscape::ObjectSet selection(doc.get()); selection.add(original);
        ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
        auto result = item(*doc); ASSERT_TRUE(result); ASSERT_TRUE(result->getClipObject());
        EXPECT_EQ(references(result->getRepr(), "clip-path"), 1);
        same_bounds(result->getClipObject()->geometricBounds(result->i2doc_affine()), clip_bounds);
        EXPECT_EQ(sp_svg_write_path(result->getClipObject()->getPathVector(result->i2doc_affine())), geometry);
        contained(result->documentVisualBounds(), clip_bounds);
        ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(xml(doc->getReprRoot()), before);
        EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
    }
}

TEST_F(StrokeToPathClip, BoundingBoxCssGroupSaveReopen)
{
    auto doc = make(R"(<defs><clipPath id='bbox-clip' clipPathUnits='objectBoundingBox'>
        <path id='clip-geometry' d='M0.25,0.25 H0.75 V0.75 H0.25Z'/></clipPath>
        <mask id='bbox-mask' maskContentUnits='objectBoundingBox'>
        <path id='mask-geometry' fill='white' d='M0.25,0.25 H0.75 V0.75 H0.25Z'/></mask></defs>
        <g id='target' transform='translate(3,4) rotate(7)'
           style='clip-path:url(#bbox-clip);mask:url(#bbox-mask) !important'>
        <path d='M0,0 H30 V30 H0Z' style='fill:none;stroke:blue;stroke-width:10'/></g>)");
    auto original = item(*doc); ASSERT_TRUE(original);
    auto bbox = original->geometricBounds(); ASSERT_TRUE(bbox);
    auto const units = Geom::Scale(bbox->dimensions()) * Geom::Translate(bbox->min()) * original->i2doc_affine();
    auto const clip_bounds = original->getClipObject()->geometricBounds(units);
    auto const mask_bounds = original->getMaskObject()->geometricBounds(units);
    auto const before = sp_repr_save_buf(doc->getReprDoc()).raw();
    Inkscape::ObjectSet selection(doc.get()); selection.add(original);
    ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
    auto serialized = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto reopened = SPDocument::createNewDocFromMem(serialized); ASSERT_TRUE(reopened); reopened->ensureUpToDate();
    for (auto document : {doc.get(), reopened.get()}) {
        auto result = item(*document); ASSERT_TRUE(result);
        auto clip = result->getClipObject(); auto mask = result->getMaskObject(); ASSERT_TRUE(clip); ASSERT_TRUE(mask);
        EXPECT_FALSE(clip->clippath_units()); EXPECT_FALSE(mask->mask_content_units());
        same_bounds(clip->geometricBounds(result->i2doc_affine()), clip_bounds);
        same_bounds(mask->geometricBounds(result->i2doc_affine()), mask_bounds);
        contained(result->documentVisualBounds(), clip_bounds);
        EXPECT_EQ(references(result->getRepr(), "clip-path"), 1);
        EXPECT_EQ(references(result->getRepr(), "mask"), 1);
    }
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}


TEST_F(StrokeToPathClip, ReferencedBoundingBoxClipGeometryAndExport)
{
    for (int mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode);
        std::string defs = "<defs><path id='direct-geometry' d='M0.1,0.1 H0.5 V0.5 H0.1Z'/>";
        defs += "<use id='intermediate' xlink:href='#direct-geometry' x='0.1' y='0.05' transform='scale(0.5)'/>";
        defs += "<clipPath id='use-clip' clipPathUnits='objectBoundingBox'><use id='clip-use' xlink:href='";
        defs += mode == 2 ? "#intermediate'" : "#direct-geometry'";
        if (mode) defs += " x='0.1' y='0.2' transform='translate(0.05,0.025)'";
        defs += "/></clipPath></defs>";
        auto doc = make(defs + "<path id='target' d='M10,20 H50 V60 H10Z' fill='blue' stroke='blue' stroke-width='4' clip-path='url(#use-clip)'/>");
        // Explicit expected rectangle, independent of SPUse/SPClipPath traversal.
        double x0 = mode == 0 ? 14 : mode == 1 ? 20 : 20;
        double y0 = mode == 0 ? 24 : mode == 1 ? 33 : 32;
        double extent = mode == 2 ? 8 : 16;
        auto expected = Geom::Rect(Geom::Point(x0, y0), Geom::Point(x0 + extent, y0 + extent));
        auto const before = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto directory = std::getenv("BUG030_EXPORT_DIR");
        auto prefix = directory ? std::string(directory) + "/use-" + std::to_string(mode) : std::string{};
        auto original_pixels = rendered(*doc, directory ? prefix + "-before-native.png" : "");
        Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
        ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
        same_pixels(rendered(*doc, directory ? prefix + "-after-native.png" : ""), original_pixels);
        auto clip = item(*doc)->getClipObject(); ASSERT_TRUE(clip);
        auto paths = clip->getPathVector(Geom::identity());
        EXPECT_FALSE(paths.empty());
        same_bounds(Geom::bounds_exact(paths), expected);
        EXPECT_STREQ(clip->getRepr()->firstChild()->attribute("xlink:href"), "#direct-geometry");
        if (auto directory = std::getenv("BUG030_EXPORT_DIR")) {
            auto const prefix = std::string(directory) + "/use-" + std::to_string(mode);
            std::ofstream(prefix + "-before.svg") << before;
            std::ofstream(prefix + "-after.svg") << sp_repr_save_buf(doc->getReprDoc()).raw();
            std::ostringstream rect;
            rect << "<defs><clipPath id='oracle'><rect x='" << x0 << "' y='" << y0
                 << "' width='" << extent << "' height='" << extent << "'/></clipPath></defs>"
                 << "<path d='M10,20 H50 V60 H10Z' fill='blue' stroke='blue' stroke-width='4' clip-path='url(#oracle)'/>";
            std::ofstream(prefix + "-oracle.svg") << fixture(rect.str());
        }
        auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
        ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
        ASSERT_TRUE(Inkscape::DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
    }
}

TEST_F(StrokeToPathClip, MappingDoesNotSurviveAnAction)
{
    for (int edit = 0; edit < 4; ++edit) {
        SCOPED_TRACE(edit);
        auto doc = make(R"(<defs><clipPath id='shared-clip' clipPathUnits='objectBoundingBox'>
          <path id='shared-geometry' d='M0.1,0.1 H0.4 V0.4 H0.1Z'/></clipPath>
          <mask id='shared-mask' maskContentUnits='objectBoundingBox' width='80%'>
          <path id='mask-shape' fill='white' d='M0,0 H1 V1 H0Z'/></mask></defs>
          <path id='a' d='M0,0 H30 V30 H0Z' fill='blue' stroke='red' clip-path='url(#shared-clip)' mask='url(#shared-mask)'/>
          <path id='b' d='M0,0 H30 V30 H0Z' fill='blue' stroke='red' clip-path='url(#shared-clip)' mask='url(#shared-mask)'/>)");
        Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc, "a"));
        ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
        auto first_clip = item(*doc, "a")->getClipObject();
        std::string first_id = first_clip->getId();
        if (edit == 0) {
            auto added = doc->getReprDoc()->createElement("svg:path");
            added->setAttribute("id", "added"); added->setAttribute("d", "M0.6,0.6 H0.9 V0.9 H0.6Z");
            doc->getObjectById("shared-clip")->getRepr()->appendChild(added); Inkscape::GC::release(added);
        } else if (edit == 1) {
            doc->getObjectById("shared-mask")->setAttribute("width", "50%");
            doc->getObjectById("shared-mask")->setAttribute("maskContentUnits", "userSpaceOnUse");
        } else if (edit == 2) {
            first_clip->getRepr()->firstChild()->setAttribute("transform", "scale(0.1)");
        } else {
            doc->getObjectById("shared-clip")->deleteObject(false);
            auto replacement = doc->getReprDoc()->createElement("svg:clipPath");
            replacement->setAttribute("id", "shared-clip"); replacement->setAttribute("clipPathUnits", "objectBoundingBox");
            auto geometry = doc->getReprDoc()->createElement("svg:path");
            geometry->setAttribute("id", "replacement-shape"); geometry->setAttribute("d", "M0.6,0.6 H0.9 V0.9 H0.6Z");
            replacement->appendChild(geometry); Inkscape::GC::release(geometry);
            doc->getDefs()->getRepr()->appendChild(replacement); Inkscape::GC::release(replacement);
            item(*doc, "b")->setAttribute("clip-path", "url(#shared-clip)");
        }
        doc->ensureUpToDate();
        Inkscape::DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString{"Edit resource"}, "");
        auto first_xml = xml(doc->getObjectById(first_id)->getRepr());
        auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto source_clip = item(*doc, "b")->getClipObject(); ASSERT_TRUE(source_clip);
        auto expected = sp_svg_write_path(source_clip->getPathVector(Geom::Scale(30,30)));
        selection.clear(); selection.add(item(*doc, "b"));
        ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
        auto result = item(*doc, "b"); ASSERT_TRUE(result->getClipObject());
        EXPECT_NE(result->getClipObject()->getId(), first_id);
        EXPECT_EQ(sp_svg_write_path(result->getClipObject()->getPathVector(Geom::identity())), expected);
        EXPECT_EQ(xml(doc->getObjectById(first_id)->getRepr()), first_xml);
        if (edit == 1) {
            EXPECT_STREQ(result->getMaskObject()->getRepr()->attribute("width"), "15");
            EXPECT_FALSE(result->getMaskObject()->mask_content_units());
            same_bounds(result->getMaskObject()->geometricBounds(Geom::identity()),
                        Geom::Rect(Geom::Point(0, 0), Geom::Point(1, 1)));
        }
        auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
        ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        ASSERT_TRUE(Inkscape::DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
    }
}


namespace {
// Exercise the real native holder-deletion branch, without a desktop or a
// production test hook. Only the fixture LPE's virtual decision is substituted.
class RemovingHolderEffect : public Inkscape::LivePathEffect::LPEBSpline {
public:
    explicit RemovingHolderEffect(LivePathEffectObject *object) : LPEBSpline(object) {
        readallParameters(object->getRepr());
    }
    bool getHolderRemove() override {
        bool result = remove;
        remove = false; // deleteObject re-enters removeAllPathEffects(false).
        return result;
    }
private:
    bool remove = true;
};
}
TEST_F(StrokeToPathClip, HolderRemovalRollsBackWholeSelectedGroup)
{
    auto doc = make(R"(<defs><inkscape:path-effect id='removing' effect='bspline'/></defs>
      <g id='target'><path id='normal' d='M0,0 H30 V30 H0Z' fill='none' stroke='blue'/>
      <path id='holder' d='M0,0 H30 V30 H0Z' fill='none' stroke='blue'
         inkscape:original-d='M0,0 H30 V30 H0Z' inkscape:path-effect='#removing'/></g>)");
    auto lpe = cast<LivePathEffectObject>(doc->getObjectById("removing")); ASSERT_TRUE(lpe);
    delete lpe->lpe; lpe->lpe = new RemovingHolderEffect(lpe);
    doc->ensureUpToDate();
    Inkscape::DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString{"Prepare fixture"}, "");
    Inkscape::DocumentUndo::clearUndo(doc.get());
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
    EXPECT_FALSE(selection.strokesToPaths()); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    ASSERT_EQ(selection.items_vector().size(), 1u);
    EXPECT_EQ(selection.singleItem(), item(*doc));
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
    EXPECT_FALSE(Inkscape::DocumentUndo::redo(doc.get()));
    // An empty later settlement must not pick up leaked pending rollback work.
    Inkscape::DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString{"Empty settlement"}, "");
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}

TEST_F(StrokeToPathClip, ProtectedMembersAreExcludedBeforeBaking)
{
    auto doc = make(R"(<defs><inkscape:path-effect id='effect' effect='bspline'/></defs>
      <g id='target'><path id='normal' d='M0,0 H30 V30 H0Z' fill='none' stroke='blue'/>
      <path id='hidden' d='M0,0 H30 V30 H0Z' style='display:none;fill:none;stroke:red'
         inkscape:original-d='M0,0 H30 V30 H0Z' inkscape:path-effect='#effect'/>
      <path id='invisible' d='M0,0 H30 V30 H0Z' style='visibility:hidden;fill:none;stroke:red'/>
      <g id='locked' sodipodi:insensitive='true'><path d='M0,0 H30 V30 H0Z' fill='none' stroke='red'/></g>
      <use id='clone' xlink:href='#hidden'/></g>)");
    auto prefs = Inkscape::Preferences::get();
    bool unlink = prefs->getBool("/options/pathoperationsunlink/value", true);
    // Clone unlinking belongs to the excluded caller scope; this case checks
    // the allowed conversion routine's original-subtree admission only.
    prefs->setBool("/options/pathoperationsunlink/value", false);
    std::vector<std::pair<std::string, std::string>> protected_xml;
    for (auto id : {"hidden", "invisible", "locked", "clone"}) {
        protected_xml.emplace_back(id, xml(doc->getObjectById(id)->getRepr()));
    }
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
    bool changed = selection.strokesToPaths();
    prefs->setBool("/options/pathoperationsunlink/value", unlink);
    ASSERT_TRUE(changed); doc->ensureUpToDate();
    EXPECT_TRUE(item(*doc, "normal")->style->stroke.isNone());
    for (auto const &[id, snapshot] : protected_xml) {
        ASSERT_TRUE(doc->getObjectById(id));
        EXPECT_EQ(xml(doc->getObjectById(id)->getRepr()), snapshot);
    }
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}


TEST_F(StrokeToPathClip, ManyDistinctMappingsUndoRedo)
{
    int count = std::getenv("BUG030_MAPPING_COUNT") ? std::atoi(std::getenv("BUG030_MAPPING_COUNT")) : 300;
    ASSERT_GT(count, 0); ASSERT_LE(count, 10000);
    std::string body = R"(<defs><clipPath id='many-clip' clipPathUnits='objectBoundingBox'>
      <path id='many-shape' d='M0.1,0.1 H0.9 V0.9 H0.1Z'/></clipPath></defs><g id='target'>)";
    for (int i=0; i<count; ++i) {
        body += "<path id='many-" + std::to_string(i) + "' d='M0,0 H" + std::to_string(i+10)
             + " V30 H0Z' fill='blue' stroke='red' clip-path='url(#many-clip)'/>";
    }
    auto doc = make(body + "</g>");
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto original_defs_count = doc->getDefs()->childList(false).size();
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
    auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::cout << "MAPPING_COUNT=" << count << " CONVERSION_SECONDS=" << elapsed << '\n';
    EXPECT_EQ(doc->getDefs()->childList(false).size(), original_defs_count + count);
    for (int i=0; i<count; ++i) {
        auto result = item(*doc, ("many-" + std::to_string(i)).c_str()); ASSERT_TRUE(result);
        ASSERT_TRUE(result->getClipObject());
        auto paths = result->getClipObject()->getPathVector(Geom::identity()); ASSERT_FALSE(paths.empty());
        same_bounds(Geom::bounds_exact(paths), Geom::Rect(Geom::Point((i+10)*0.1,3), Geom::Point((i+10)*0.9,27)));
    }
    auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
    for (int repeat=0; repeat<3; ++repeat) {
        ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_EQ(doc->getDefs()->childList(false).size(), original_defs_count);
        EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
        ASSERT_TRUE(Inkscape::DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
    }
}


TEST_F(StrokeToPathClip, LegacyUnsupportedLpeGroupIsUnchanged)
{
    auto doc = make(R"(<defs><inkscape:path-effect id='legacy-effect' effect='bspline'/></defs>
      <path id='normal' d='M0,0 H30 V30 H0Z' fill='none' stroke='blue'/>
      <g id='target' inkscape:path-effect='#legacy-effect'>
        <path id='legacy-child' d='M0,0 H30 V30 H0Z' inkscape:original-d='M0,0 H30 V30 H0Z' fill='none' stroke='blue'/>
      </g>)");
    auto protected_xml = xml(item(*doc)->getRepr());
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc, "normal")); selection.add(item(*doc));
    ASSERT_TRUE(selection.strokesToPaths(true)); doc->ensureUpToDate();
    EXPECT_EQ(xml(item(*doc)->getRepr()), protected_xml);
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}


TEST_F(StrokeToPathClip, NonUndoableProjectionStillConverts)
{
    auto doc = make(shape("fill:none;stroke:blue;stroke-width:10", "clip-path='url(#clip)'"));
    Inkscape::DocumentUndo::setUndoSensitive(doc.get(), false);
    auto result = item_to_paths(item(*doc)); ASSERT_TRUE(result);
    doc->ensureUpToDate();
    ASSERT_TRUE(item(*doc)->getClipObject());
    EXPECT_TRUE(item(*doc)->style->stroke.isNone());
    EXPECT_FALSE(Inkscape::DocumentUndo::getUndoSensitive(doc.get()));
}

namespace {
std::vector<std::string> selected_ids(Inkscape::ObjectSet &selection)
{
    std::vector<std::string> ids;
    for (auto item : selection.items_vector()) ids.emplace_back(item->getId());
    return ids;
}
}

TEST_F(StrokeToPathClip, IndependentRootFailureRestoresOrderAndCallerPendingWork)
{
    for (bool pending : {false, true}) for (bool skip_undo : {false, true}) {
        SCOPED_TRACE(pending);
        SCOPED_TRACE(skip_undo);
        auto doc = make(R"(<defs><inkscape:path-effect id='removing' effect='bspline'/></defs>
          <path id='normal' d='M0,0 H30 V30 H0Z' fill='none' stroke='blue'/>
          <path id='holder' d='M40,0 H70 V30 H40Z' fill='none' stroke='blue'
            inkscape:original-d='M40,0 H70 V30 H40Z' inkscape:path-effect='#removing'/>
          <use id='clone' xlink:href='#normal' x='80'/>)");
        auto lpe = cast<LivePathEffectObject>(doc->getObjectById("removing")); ASSERT_TRUE(lpe);
        delete lpe->lpe; lpe->lpe = new RemovingHolderEffect(lpe);
        doc->ensureUpToDate();
        Inkscape::DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString{"Fixture"}, "");
        Inkscape::DocumentUndo::clearUndo(doc.get());
        auto clean = sp_repr_save_buf(doc->getReprDoc()).raw();
        if (pending) item(*doc, "normal")->getRepr()->setAttribute("data-caller", "pending");
        auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
        Inkscape::ObjectSet selection(doc.get());
        for (auto id : {"normal", "holder", "clone"}) selection.add(item(*doc, id));
        auto order = selected_ids(selection);
        auto prefs = Inkscape::Preferences::get();
        bool scale = prefs->getBool("/options/transform/stroke", true);
        prefs->setBool("/options/transform/stroke", false);
        EXPECT_FALSE(selection.strokesToPaths(false, skip_undo)); doc->ensureUpToDate();
        EXPECT_FALSE(prefs->getBool("/options/transform/stroke", true));
        prefs->setBool("/options/transform/stroke", scale);
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_EQ(selected_ids(selection), order);
        // No partial action was committed. Settle only the caller's pending edit.
        Inkscape::DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString{"Caller"}, "");
        EXPECT_EQ(bool(Inkscape::DocumentUndo::undo(doc.get())), pending); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), clean);
        EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
    }
}

TEST_F(StrokeToPathClip, ExclusionOnlyAndNoOpPreserveRedoAndPendingWork)
{
    for (bool protected_root : {false, true}) {
        auto doc = make(protected_root
            ? "<path id='target' d='M0,0H30V30Z' stroke='red' style='display:none'/>"
            : "<path id='target' d='M0,0H30V30Z' fill='red' stroke='none'/>");
        item(*doc)->getRepr()->setAttribute("data-history", "redo");
        Inkscape::DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString{"Earlier"}, "");
        ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get()));
        item(*doc)->getRepr()->setAttribute("data-caller", "pending");
        auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
        Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
        EXPECT_FALSE(selection.strokesToPaths());
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_EQ(selected_ids(selection), std::vector<std::string>{"target"});
        ASSERT_TRUE(Inkscape::DocumentUndo::redo(doc.get()));
        EXPECT_STREQ(item(*doc)->getRepr()->attribute("data-history"), "redo");
        EXPECT_STREQ(item(*doc)->getRepr()->attribute("data-caller"), "pending");
    }
}

TEST_F(StrokeToPathClip, ProtectedRootsChildrenAndEligibleClones)
{
    auto doc = make(R"(<path id='source' d='M0,0 H30 V30 H0Z' fill='none' stroke='blue'/>
      <use id='hidden-root' xlink:href='#source' style='display:none'/>
      <use id='locked-root' xlink:href='#source' sodipodi:insensitive='true'/>
      <g id='target'><use id='eligible' xlink:href='#source' x='40'/>
        <use id='hidden-child' xlink:href='#source' style='visibility:hidden'/>
        <use id='locked-child' xlink:href='#source' sodipodi:insensitive='true'/></g>)");
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    std::vector<std::pair<std::string, std::string>> snapshots;
    for (auto id : {"source", "hidden-root", "locked-root", "hidden-child", "locked-child"})
        snapshots.emplace_back(id, xml(item(*doc, id)->getRepr()));
    Inkscape::ObjectSet selection(doc.get());
    for (auto id : {"hidden-root", "target", "locked-root"}) selection.add(item(*doc, id));
    ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
    EXPECT_TRUE(item(*doc, "eligible")->style->stroke.isNone());
    EXPECT_STREQ(item(*doc, "eligible")->getRepr()->name(), "svg:path");
    for (auto const &[id, snapshot] : snapshots) EXPECT_EQ(xml(item(*doc, id.c_str())->getRepr()), snapshot);
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}

TEST_F(StrokeToPathClip, CloneAndSourceRemainIndependentInEitherOrder)
{
    for (bool reverse : {false, true}) {
        auto doc = make(R"(<path id='source' d='M0,0 H30 V30 H0Z' fill='none' stroke='blue'/>
          <use id='instance' xlink:href='#source' x='40'/>)");
        auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
        auto bounds = item(*doc, "instance")->documentVisualBounds();
        Inkscape::ObjectSet selection(doc.get());
        selection.add(item(*doc, reverse ? "instance" : "source"));
        selection.add(item(*doc, reverse ? "source" : "instance"));
        auto order = selected_ids(selection);
        ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
        EXPECT_EQ(selected_ids(selection), order);
        EXPECT_TRUE(item(*doc, "source")->style->stroke.isNone());
        EXPECT_TRUE(item(*doc, "instance")->style->stroke.isNone());
        EXPECT_STREQ(item(*doc, "instance")->getRepr()->name(), "svg:path");
        same_bounds(item(*doc, "instance")->documentVisualBounds(), bounds);
        ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
        EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
    }
}

TEST_F(StrokeToPathClip, AncestorAndDescendantConvertOnce)
{
    auto doc = make(R"(<g id='target'><path id='child' d='M0,0 H30 V30 H0Z'
      fill='red' stroke='blue'/></g>)");
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    Inkscape::ObjectSet selection(doc.get());
    selection.add(item(*doc, "child")); selection.add(item(*doc)); selection.add(item(*doc, "child"));
    auto order = selected_ids(selection);
    ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
    EXPECT_EQ(selected_ids(selection), order);
    ASSERT_EQ(item(*doc, "child")->childList(false).size(), 2u);
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}

TEST_F(StrokeToPathClip, IndependentRootsShareMappings)
{
    auto doc = make(R"(<defs><clipPath id='bbox' clipPathUnits='objectBoundingBox'>
        <path id='geometry' d='M.1,.1H.9V.9H.1Z'/></clipPath>
        <mask id='bbox-mask' maskContentUnits='objectBoundingBox'><path fill='white' d='M0,0H1V1H0Z'/></mask></defs>
      <path id='a' d='M0,0H30V30H0Z' fill='red' stroke='blue' clip-path='url(#bbox)' mask='url(#bbox-mask)'/>
      <path id='b' d='M0,0H30V30H0Z' fill='none' stroke='blue' clip-path='url(#bbox)' mask='url(#bbox-mask)'/>)");
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    auto count = doc->getDefs()->childList(false).size();
    auto source = xml(doc->getObjectById("bbox")->getRepr());
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc, "a")); selection.add(item(*doc, "b"));
    ASSERT_TRUE(selection.strokesToPaths()); doc->ensureUpToDate();
    EXPECT_EQ(doc->getDefs()->childList(false).size(), count + 2);
    EXPECT_EQ(item(*doc, "a")->getClipObject(), item(*doc, "b")->getClipObject());
    EXPECT_EQ(item(*doc, "a")->getMaskObject(), item(*doc, "b")->getMaskObject());
    EXPECT_EQ(xml(doc->getObjectById("bbox")->getRepr()), source);
    auto after = sp_repr_save_buf(doc->getReprDoc()).raw();
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), after);
}

TEST_F(StrokeToPathClip, UnsafeEffectAncestorsAndCompoundInternalsAreExcluded)
{
    auto doc = make(R"(<defs><inkscape:path-effect id='safe' effect='bspline'/>
        <inkscape:path-effect id='linked' effect='clone_original' linkeditem='#outside'/></defs>
      <path id='outside' d='M0,0H30V30Z'/>
      <g id='target' inkscape:path-effect='#safe'><g><path id='protected' d='M0,0H30V30Z'
        inkscape:original-d='M0,0H30V30Z' style='display:none;stroke:red'/>
        <path d='M0,0H30V30Z' inkscape:original-d='M0,0H30V30Z' stroke='blue'/></g></g>
      <path id='dependency' d='M0,0H30V30Z' inkscape:original-d='M0,0H30V30Z'
        inkscape:path-effect='#linked' stroke='red'/>
      <text id='text' stroke='red'><tspan><tspan sodipodi:insensitive='true'>Protected</tspan></tspan></text>
      <g id='box' sodipodi:type='inkscape:box3d'><path sodipodi:type='inkscape:box3dside'
        id='face' sodipodi:insensitive='true' d='M0,0H30V30Z'/></g>)");
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    Inkscape::ObjectSet selection(doc.get());
    for (auto id : {"target", "dependency", "text", "box"}) selection.add(item(*doc, id));
    auto order = selected_ids(selection);
    EXPECT_FALSE(selection.strokesToPaths());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_EQ(selected_ids(selection), order);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}

TEST_F(StrokeToPathClip, MissingFenceRefusesBeforeUnlink)
{
    auto doc = make(R"(<path id='source' d='M0,0H30V30Z' stroke='red'/><use id='target' xlink:href='#source'/>)");
    Inkscape::DocumentUndo::setUndoSensitive(doc.get(), false);
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
    EXPECT_FALSE(selection.strokesToPaths());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_EQ(selected_ids(selection), std::vector<std::string>{"target"});
}

TEST_F(StrokeToPathClip, SuccessfulSkipUndoReattachesCallerPendingWork)
{
    auto doc = make(shape("fill:none;stroke:blue", ""));
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    item(*doc, "parent")->getRepr()->setAttribute("data-caller", "pending");
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
    ASSERT_TRUE(selection.strokesToPaths(false, true)); doc->ensureUpToDate();
    EXPECT_TRUE(item(*doc)->style->stroke.isNone());
    EXPECT_STREQ(item(*doc, "parent")->getRepr()->attribute("data-caller"), "pending");
    Inkscape::DocumentUndo::done(doc.get(), Inkscape::Util::Internal::ContextString{"Caller"}, "");
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(doc.get())); doc->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}

TEST_F(StrokeToPathClip, DisabledUnlinkPreservesIndependentInstance)
{
    auto doc = make(R"(<path id='source' d='M0,0H30V30Z' fill='none' stroke='red'/>
      <use id='target' xlink:href='#source'/>)");
    auto before = xml(item(*doc)->getRepr());
    auto prefs = Inkscape::Preferences::get();
    auto unlink = prefs->getBool("/options/pathoperationsunlink/value", true);
    prefs->setBool("/options/pathoperationsunlink/value", false);
    Inkscape::ObjectSet selection(doc.get()); selection.add(item(*doc));
    EXPECT_FALSE(selection.strokesToPaths());
    prefs->setBool("/options/pathoperationsunlink/value", unlink);
    EXPECT_EQ(xml(item(*doc)->getRepr()), before);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}

TEST_F(StrokeToPathClip, ProtectedCloneSourcesAndUnsupportedRootsRemainUntouched)
{
    auto doc = make(R"(<g id='source'><path id='locked' d='M0,0H30V30Z' stroke='red'
        sodipodi:insensitive='true'/><path d='M40,0H70V30Z' stroke='blue'/></g>
      <use id='target' xlink:href='#source'/><use id='missing' xlink:href='#absent'/>
      <use id='chain' xlink:href='#target'/><image id='unsupported' width='30' height='30'/>)");
    auto before = sp_repr_save_buf(doc->getReprDoc()).raw();
    Inkscape::ObjectSet selection(doc.get());
    for (auto id : {"target", "missing", "chain", "unsupported"}) selection.add(item(*doc, id));
    auto order = selected_ids(selection);
    EXPECT_FALSE(selection.strokesToPaths());
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()).raw(), before);
    EXPECT_EQ(selected_ids(selection), order);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(doc.get()));
}
