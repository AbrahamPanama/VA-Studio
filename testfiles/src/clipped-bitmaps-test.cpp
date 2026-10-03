// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CDR-1: clips that hold only bitmaps (CorelDRAW PowerClips) become plain
 * images, each on its own; clips with vectors or text stay unchanged; opening
 * converts without an Undo step, the menu command in one Undo step.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <glibmm/i18n.h>
#include "display/cairo-utils.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "util-string/context-string.h"
#include "ui/clipped-bitmaps.h"
#include "xml/attribute-record.h"
#include "xml/node.h"
#include "xml/repr.h"

using namespace Inkscape::UI;
using Inkscape::DocumentUndo;

namespace {

// 4x4 opaque red PNG.
#define RED "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAEklEQVR4nGP4z8DwHxkzkC4AADxAH+HggXe0AAAAAElFTkSuQmCC"
#define IMG(id, x, y) "<image id='" id "' x='" #x "' y='" #y "' width='60' height='60' preserveAspectRatio='none' xlink:href='" RED "'/>"

constexpr std::string_view document_text = R"(<svg xmlns='http://www.w3.org/2000/svg'
     xmlns:xlink='http://www.w3.org/1999/xlink'
     xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape'
     xmlns:sodipodi='http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd'
     width='400' height='400' viewBox='0 0 400 400'>
  <defs>
    <clipPath id='c-photo' clipPathUnits='userSpaceOnUse'><circle cx='30' cy='30' r='20'/></clipPath>
    <clipPath id='c-direct' clipPathUnits='userSpaceOnUse'><rect x='110' y='10' width='40' height='30'/></clipPath>
    <clipPath id='c-mixed' clipPathUnits='userSpaceOnUse'><rect x='190' y='0' width='60' height='60'/></clipPath>
    <clipPath id='c-vector' clipPathUnits='userSpaceOnUse'><rect x='290' y='0' width='40' height='40'/></clipPath>
    <clipPath id='c-outer' clipPathUnits='userSpaceOnUse'><rect x='0' y='100' width='50' height='50'/></clipPath>
    <clipPath id='c-inner' clipPathUnits='userSpaceOnUse'><rect x='10' y='110' width='50' height='50'/></clipPath>
    <clipPath id='c-hidden' clipPathUnits='userSpaceOnUse'><rect x='100' y='100' width='50' height='50'/></clipPath>
    <mask id='m-masked' maskUnits='userSpaceOnUse' x='200' y='100' width='60' height='60'>
      <rect x='210' y='110' width='40' height='40' fill='white'/>
    </mask>
    <clipPath id='c-hidden-rect' clipPathUnits='userSpaceOnUse'><rect x='300' y='110' width='40' height='40'/></clipPath>
    <g id='in-defs' clip-path='url(#c-photo)'>)" IMG("in-defs-img", 0, 0) R"(</g>
  </defs>
  <g id='layer1' inkscape:groupmode='layer'>
    <g id='photo' clip-path='url(#c-photo)'>)" IMG("photo-img", 0, 0) R"(</g>
    )" "<image id='direct' clip-path='url(#c-direct)' x='100' y='0' width='60' height='60' preserveAspectRatio='none' xlink:href='" RED "'/>" R"(
    <g id='mixed' clip-path='url(#c-mixed)'>)" IMG("mixed-img", 190, 0) R"(<rect id='mixed-rect' x='200' y='10' width='10' height='10'/></g>
    <g id='vector' clip-path='url(#c-vector)'><rect id='vector-rect' x='280' y='0' width='60' height='60' fill='blue'/></g>
    <g id='outer' clip-path='url(#c-outer)'><g id='inner' clip-path='url(#c-inner)'>)" IMG("inner-img", 0, 100) R"(</g></g>
    <g id='hidden' style='display:none' clip-path='url(#c-hidden)'>)" IMG("hidden-img", 100, 100) R"(</g>
    <g id='locked' sodipodi:insensitive='true' clip-path='url(#c-hidden)'>)" IMG("locked-img", 100, 100) R"(</g>
    <g id='masked' mask='url(#m-masked)'>)" IMG("masked-img", 200, 100) R"(</g>
    <g id='hidden-rect' clip-path='url(#c-hidden-rect)'>)" IMG("hidden-rect-img", 290, 100) R"(<rect style='display:none' x='300' y='110' width='10' height='10'/></g>
    <g id='plain'>)" IMG("free", 0, 200) R"(</g>
  </g>
  <g id='locked-layer' inkscape:groupmode='layer' sodipodi:insensitive='true'>
    <g id='in-locked-layer' clip-path='url(#c-photo)'>)" IMG("in-locked-layer-img", 0, 0) R"(</g>
  </g>
</svg>)";

std::vector<std::string> const convertible_ids{"photo", "direct", "outer", "masked", "hidden-rect"};

std::vector<std::string> ids_of(std::vector<SPItem *> const &items)
{
    std::vector<std::string> ids;
    for (auto *item : items) {
        ids.emplace_back(item->getId() ? item->getId() : "");
    }
    return ids;
}

std::vector<SPImage *> images_in(SPObject &parent)
{
    std::vector<SPImage *> images;
    for (auto &child : parent.children) {
        if (auto *image = cast<SPImage>(&child)) {
            images.push_back(image);
        }
    }
    return images;
}

std::string xml_of(SPDocument &document, char const *id)
{
    auto *object = document.getObjectById(id);
    if (!object) {
        return "";
    }
    std::string text;
    std::function<void(Inkscape::XML::Node const &)> write = [&](Inkscape::XML::Node const &node) {
        text += node.name() ? node.name() : "";
        for (auto const &attribute : node.attributeList()) {
            text += " " + std::string(g_quark_to_string(attribute.key)) + "=" + attribute.value.pointer();
        }
        text += "{";
        for (auto *child = node.firstChild(); child; child = child->next()) {
            write(*child);
        }
        text += "}";
    };
    write(*object->getRepr());
    return text;
}

unsigned pixel(Inkscape::Pixbuf const &pixbuf, int x, int y)
{
    auto *surface = const_cast<Inkscape::Pixbuf &>(pixbuf).getSurfaceRaw();
    cairo_surface_flush(surface);
    auto const *row = cairo_image_surface_get_data(surface) + y * cairo_image_surface_get_stride(surface);
    return reinterpret_cast<uint32_t const *>(row)[x];
}

class ClippedBitmapsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Inkscape::Application::exists()) {
            Inkscape::Application::create(false);
        }
        document = SPDocument::createNewDocFromMem(document_text);
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        DocumentUndo::setUndoSensitive(document.get(), true);
        DocumentUndo::clearUndo(document.get());
        layer = document->getObjectById("layer1");
        ASSERT_TRUE(layer);
        for (auto const &id : convertible_ids) {
            auto *item = cast<SPItem>(document->getObjectById(id));
            ASSERT_TRUE(item) << id;
            auto bounds = item->documentVisualBounds();
            ASSERT_TRUE(bounds) << id;
            bounds_before[id] = *bounds;
        }
        for (auto const *id : {"mixed", "vector", "hidden", "locked", "plain", "in-defs", "locked-layer"}) {
            unchanged_before[id] = xml_of(*document, id);
            ASSERT_FALSE(unchanged_before[id].empty()) << id;
        }
    }

    Inkscape::BitmapCopyOptions options() const
    {
        Inkscape::BitmapCopyOptions options;
        options.dpi = 96;
        options.antialias = true;
        options.transparent = true;
        options.keep_original = true; // ignored: every clip is replaced
        return options;
    }

    // The images the conversion added to the layer, keyed by the clip each
    // one replaced (matched by its bounds).
    std::map<std::string, SPImage *> new_images() const
    {
        std::map<std::string, SPImage *> found;
        for (auto *image : images_in(*layer)) {
            auto const bounds = image->documentVisualBounds();
            for (auto const &[id, before] : bounds_before) {
                if (bounds && Geom::are_near(bounds->min(), before.min(), 0.5) &&
                    Geom::are_near(bounds->max(), before.max(), 0.5)) {
                    EXPECT_EQ(found.count(id), 0u) << "two images for " << id;
                    found[id] = image;
                }
            }
        }
        return found;
    }

    void expect_converted_except(std::string const &edited)
    {
        auto saved = unchanged_before;
        unchanged_before.erase(edited);
        expect_converted();
        unchanged_before = saved;
    }

    void expect_converted()
    {
        document->ensureUpToDate();
        for (auto const &id : convertible_ids) {
            EXPECT_EQ(document->getObjectById(id), nullptr) << id << " was replaced";
        }
        auto const images = new_images();
        EXPECT_EQ(images.size(), convertible_ids.size()) << "one separate image per clip";
        EXPECT_EQ(images_in(*layer).size(), convertible_ids.size()) << "nothing else became an image";
        for (auto const &[id, before] : unchanged_before) {
            EXPECT_EQ(xml_of(*document, id.c_str()), before) << id << " is untouched";
        }
    }

    std::unique_ptr<SPDocument> document;
    SPObject *layer = nullptr;
    std::map<std::string, Geom::Rect> bounds_before;
    std::map<std::string, std::string> unchanged_before;
};

TEST_F(ClippedBitmapsTest, ScanFindsOutermostVisibleUnlockedBitmapOnlyClips)
{
    auto const found = ClippedBitmaps::scan(*document);
    EXPECT_EQ(ids_of(found.convertible), convertible_ids);
    EXPECT_EQ(found.with_vectors, 1) << "the clip holding a bitmap and a rectangle";
    EXPECT_EQ(found.in_effect_groups, 0);
    EXPECT_EQ(found.ids(), convertible_ids);
}

TEST_F(ClippedBitmapsTest, ScanLeavesClipsInEffectGroupsClippedLayersAndInvisibleImages)
{
    auto *xml = document->getReprDoc();
    auto add = [&](Inkscape::XML::Node *parent, char const *text) {
        auto *fragment = sp_repr_read_mem(text, std::strlen(text), nullptr);
        ASSERT_TRUE(fragment);
        auto *copy = fragment->root()->duplicate(xml);
        parent->appendChild(copy);
        Inkscape::GC::release(copy);
        Inkscape::GC::release(fragment);
    };
    auto *root = document->getRoot()->getRepr();
    // A semi-transparent group: its clips would land above its rectangle.
    add(root, "<svg:g xmlns='http://www.w3.org/2000/svg' xmlns:svg='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' id='faded' "
              "style='opacity:0.5'><svg:g id='fa' clip-path='url(#c-photo)'>" IMG("fa-img", 0, 0) "</svg:g>"
              "<svg:rect id='fb' x='0' y='0' width='50' height='50'/>"
              "<svg:g id='fc' clip-path='url(#c-direct)'>" IMG("fc-img", 100, 0) "</svg:g></svg:g>");
    // A clipped layer is not a target; its clips render with the layer's clip.
    add(root, "<svg:g xmlns='http://www.w3.org/2000/svg' xmlns:svg='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
              "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' id='clipped-layer' "
              "inkscape:groupmode='layer' clip-path='url(#c-outer)'><svg:g id='cl' clip-path='url(#c-inner)'>"
              IMG("cl-img", 0, 100) "</svg:g></svg:g>");
    // An image-only clip whose image is not drawn.
    add(root, "<svg:g xmlns='http://www.w3.org/2000/svg' xmlns:svg='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' "
              "id='ghost' clip-path='url(#c-photo)'><svg:image id='ghost-img' style='visibility:hidden' x='0' y='0' "
              "width='60' height='60' xlink:href='" RED "'/></svg:g>");
    document->ensureUpToDate();

    auto const found = ClippedBitmaps::scan(*document);
    EXPECT_EQ(ids_of(found.convertible), convertible_ids);
    EXPECT_EQ(found.in_effect_groups, 3) << "fa, fc and the clip inside the clipped layer";
    EXPECT_EQ(found.with_vectors, 1) << "the invisible image counts as nothing";
}

TEST_F(ClippedBitmapsTest, ConversionOnOpeningLeavesNoUndoStep)
{
    auto const ids = ClippedBitmaps::scan(*document).ids();
    EXPECT_EQ(ClippedBitmaps::convert(*document, ids, options(), false), 5);
    expect_converted();
    EXPECT_TRUE(DocumentUndo::getUndoSensitive(document.get()));
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "not undoable";
    expect_converted();
    EXPECT_TRUE(ClippedBitmaps::scan(*document).convertible.empty());
    EXPECT_TRUE(document->isModifiedSinceSave());

    // A later edit undoes on its own, leaving the conversion in place.
    auto const converted = xml_of(*document, "layer1");
    document->getObjectById("vector-rect")->getRepr()->setAttribute("fill", "green");
    DocumentUndo::done(document.get(), Inkscape::Util::Internal::ContextString{"Recolor"}, "");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(xml_of(*document, "layer1"), converted);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(ClippedBitmapsTest, UnusedClipPathsAndMasksAreRemovedWithTheirClips)
{
    ClippedBitmaps::convert(*document, ClippedBitmaps::scan(*document).ids(), options(), true);
    for (auto const *id : {"c-direct", "c-outer", "c-inner", "m-masked", "c-hidden-rect"}) {
        EXPECT_EQ(document->getObjectById(id), nullptr) << id;
    }
    // Still used by clips that stay (in <defs>, a locked layer, excluded clips).
    for (auto const *id : {"c-photo", "c-mixed", "c-vector", "c-hidden"}) {
        EXPECT_NE(document->getObjectById(id), nullptr) << id;
    }
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    EXPECT_NE(document->getObjectById("c-direct"), nullptr);
    EXPECT_NE(document->getObjectById("m-masked"), nullptr);
}

TEST_F(ClippedBitmapsTest, OpeningConversionFoldsPendingEditsIntoTheBaseline)
{
    // An edit not yet recorded as a step (as while opening) is kept, and no
    // later Undo can reach back before the conversion.
    document->getObjectById("vector-rect")->getRepr()->setAttribute("fill", "green");
    ASSERT_EQ(ClippedBitmaps::convert(*document, ClippedBitmaps::scan(*document).ids(), options(), false), 5);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_STREQ(document->getObjectById("vector-rect")->getRepr()->attribute("fill"), "green");
    expect_converted_except("vector");
}

TEST_F(ClippedBitmapsTest, NothingIsConvertedWhileAnInteractionOwnsTheHistory)
{
    auto const before = xml_of(*document, "layer1");
    auto interaction = DocumentUndo::beginRollbackableInteraction(document.get());
    ASSERT_TRUE(interaction);
    EXPECT_EQ(ClippedBitmaps::convert(*document, ClippedBitmaps::scan(*document).ids(), options(), true), 0);
    EXPECT_EQ(xml_of(*document, "layer1"), before);
    interaction->rollback();
}

TEST_F(ClippedBitmapsTest, MenuConversionIsOneUndoStep)
{
    auto const ids = ClippedBitmaps::scan(*document).ids();
    ASSERT_EQ(ClippedBitmaps::convert(*document, ids, options(), true), 5);
    expect_converted();

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    for (auto const &id : convertible_ids) {
        EXPECT_NE(document->getObjectById(id), nullptr) << id << " is back";
    }
    EXPECT_TRUE(images_in(*layer).size() == 1 && images_in(*layer).front()->getId() == std::string("direct"));
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "a single step";

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    expect_converted();
}

TEST_F(ClippedBitmapsTest, StepByStepConversionCountsProgressAndIsOneUndoStep)
{
    // What the progress window runs: one image per step, "n / total".
    ClippedBitmaps::Conversion conversion(*document, ClippedBitmaps::scan(*document).ids(), options(), true);
    ASSERT_TRUE(conversion.started());
    EXPECT_EQ(conversion.total(), 5);
    EXPECT_EQ(conversion.processed(), 0);
    EXPECT_TRUE(DocumentUndo::interactionActive(document.get())) << "nothing else can write the history meanwhile";
    std::vector<int> seen;
    while (conversion.step()) {
        seen.push_back(conversion.processed());
    }
    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3, 4, 5}));
    EXPECT_EQ(conversion.finish(), 5);
    EXPECT_EQ(conversion.finish(), 5) << "finish is idempotent";
    EXPECT_FALSE(DocumentUndo::interactionActive(document.get()));
    expect_converted();

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    for (auto const &id : convertible_ids) {
        EXPECT_NE(document->getObjectById(id), nullptr) << id << " is back";
    }
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "a single step";
}

TEST_F(ClippedBitmapsTest, AConversionStoppedHalfwayLeavesTheDocumentAsItWas)
{
    auto const before = xml_of(*document, "layer1");
    {
        ClippedBitmaps::Conversion conversion(*document, ClippedBitmaps::scan(*document).ids(), options(), true);
        ASSERT_TRUE(conversion.step());
        ASSERT_TRUE(conversion.step());
        ASSERT_EQ(conversion.processed(), 2);
    } // stopped before finish(), e.g. its window was closed
    document->ensureUpToDate();
    EXPECT_EQ(xml_of(*document, "layer1"), before);
    EXPECT_FALSE(DocumentUndo::interactionActive(document.get()));
    EXPECT_FALSE(DocumentUndo::undo(document.get())) << "no history entry";
}

TEST_F(ClippedBitmapsTest, AnotherUndoStepBetweenTwoImagesStopsWithTheRealCount)
{
    ClippedBitmaps::Conversion conversion(*document, ClippedBitmaps::scan(*document).ids(), options(), true);
    ASSERT_TRUE(conversion.step());
    ASSERT_TRUE(conversion.step());
    // Another action records a history step between two images (a menu the
    // platform leaves active while the progress window is up).
    document->getObjectById("vector-rect")->getRepr()->setAttribute("fill", "green");
    DocumentUndo::done(document.get(), RC_("Undo", "Other edit"), "");
    EXPECT_FALSE(conversion.step()) << "the conversion stops there";
    EXPECT_EQ(conversion.finish(), 2) << "it reports the images really made";
    EXPECT_FALSE(DocumentUndo::interactionActive(document.get()));
    document->ensureUpToDate();
    int remaining = 0;
    for (auto const &id : convertible_ids) {
        remaining += document->getObjectById(id) ? 1 : 0;
    }
    EXPECT_EQ(remaining, 3) << "three clips were never reached";
    EXPECT_STREQ(document->getObjectById("vector-rect")->getRepr()->attribute("fill"), "green");
}

TEST_F(ClippedBitmapsTest, EachImageLooksLikeItsClip)
{
    ClippedBitmaps::convert(*document, ClippedBitmaps::scan(*document).ids(), options(), true);
    auto const images = new_images();
    ASSERT_EQ(images.count("photo"), 1u);
    auto const *pixbuf = images.at("photo")->pixbuf.get();
    ASSERT_TRUE(pixbuf);
    ASSERT_EQ(pixbuf->width(), 40);
    ASSERT_EQ(pixbuf->height(), 40);
    // Circle of radius 20: red inside, transparent in the corners.
    EXPECT_EQ(pixel(*pixbuf, 20, 20), 0xffff0000u);
    EXPECT_EQ(pixel(*pixbuf, 1, 1) >> 24, 0u);
    // The masked bitmap keeps only the mask's square.
    ASSERT_EQ(images.count("masked"), 1u);
    auto const *masked = images.at("masked")->pixbuf.get();
    ASSERT_TRUE(masked);
    EXPECT_EQ(masked->width(), 40);
    EXPECT_EQ(pixel(*masked, 20, 20) >> 24, 0xffu);
}

TEST_F(ClippedBitmapsTest, ClipsInsideAnEffectGroupKeepTheirStackingOrder)
{
    auto *xml = document->getReprDoc();
    auto *effect = xml->createElement("svg:g");
    effect->setAttribute("id", "effect");
    effect->setAttribute("style", "opacity:0.5");
    for (auto const &[id, clip] : {std::pair{"lower", "c-photo"}, std::pair{"upper", "c-direct"}}) {
        // Two clips at the same place: the upper one must stay on top.
        auto *group = xml->createElement("svg:g");
        group->setAttribute("id", id);
        group->setAttribute("clip-path", std::string("url(#") + clip + ")");
        auto *image = xml->createElement("svg:image");
        image->setAttribute("x", clip == std::string("c-photo") ? "0" : "100");
        image->setAttribute("y", "0");
        image->setAttribute("width", "60");
        image->setAttribute("height", "60");
        image->setAttribute("preserveAspectRatio", "none");
        image->setAttribute("xlink:href", RED);
        group->appendChild(image);
        effect->appendChild(group);
        Inkscape::GC::release(image);
        Inkscape::GC::release(group);
    }
    layer->getRepr()->appendChild(effect);
    Inkscape::GC::release(effect);
    document->ensureUpToDate();
    auto const lower = *cast<SPItem>(document->getObjectById("lower"))->documentVisualBounds();
    auto const upper = *cast<SPItem>(document->getObjectById("upper"))->documentVisualBounds();

    ClippedBitmaps::convert(*document, {"lower", "upper"}, options(), true);
    document->ensureUpToDate();
    int lower_at = -1, upper_at = -1, position = 0;
    for (auto &child : layer->children) {
        if (auto *image = cast<SPImage>(&child)) {
            auto const bounds = *image->documentVisualBounds();
            if (Geom::are_near(bounds.min(), lower.min(), 0.5)) lower_at = position;
            if (Geom::are_near(bounds.min(), upper.min(), 0.5)) upper_at = position;
        }
        ++position;
    }
    ASSERT_GE(lower_at, 0);
    ASSERT_GE(upper_at, 0);
    EXPECT_LT(lower_at, upper_at);
}

TEST(ClippedBitmapsFiles, CorelDrawFormatsAreRecognized)
{
    for (auto const *path : {"a.cdr", "/x/B.CDR", "c.cdt", "d.ccx", "e.cmx", "C:\\cards\\f.Cdr"}) {
        EXPECT_TRUE(ClippedBitmaps::is_coreldraw_file(path)) << path;
    }
    for (auto const *path : {"", "cdr", "a.svg", "a.cdr.svg", "a.pdf", "a.ai"}) {
        EXPECT_FALSE(ClippedBitmaps::is_coreldraw_file(path)) << path;
    }
}

} // namespace
