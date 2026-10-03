// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include <string_view>

#include <2geom/affine.h>

#include <string>
#include <vector>

#include "bitmap-adjustment-chemistry.h"
#include "document.h"
#include "document-undo.h"
#include "filter-chemistry.h"
#include "inkscape.h"
#include "object/filters/sp-filter-primitive.h"
#include "object/sp-filter.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "style.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace Inkscape::BitmapAdjustments;
using namespace std::literals;

namespace {

constexpr auto image_data =
    "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJ"
    "AAAADUlEQVQIHWP4z8DwHwAFgAI/ScLx9QAAAABJRU5ErkJggg==";

class BitmapAdjustmentChemistryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists()) Application::create(false);
        auto const svg = Glib::ustring::compose(R"svg(<svg xmlns="http://www.w3.org/2000/svg"
 xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="20" height="10">
 <defs><filter id="shared"><feGaussianBlur stdDeviation="1"/></filter></defs>
 <image id="one" width="10" height="10" href="%1" style="filter:url(#shared)"/>
 <image id="two" x="10" width="10" height="10" href="%1" style="filter:url(#shared)"/>
</svg>)svg", image_data);
        document = SPDocument::createNewDocFromMem(svg.raw());
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        one = cast<SPImage>(document->getObjectById("one"));
        two = cast<SPImage>(document->getObjectById("two"));
        ASSERT_TRUE(one);
        ASSERT_TRUE(two);
    }

    std::unique_ptr<SPDocument> document;
    SPImage *one = nullptr;
    SPImage *two = nullptr;
};

Filters::BitmapToneSettings adjusted_settings()
{
    Filters::BitmapToneSettings settings;
    settings.brightness = 12.5;
    settings.contrast = -8.0;
    settings.intensity = 4.0;
    settings.highlights = -20.0;
    settings.shadows = 15.0;
    settings.midtones = 6.0;
    return settings;
}

} // namespace

TEST_F(BitmapAdjustmentChemistryTest, ForksSharedFiltersAndPreservesUnrelatedPrimitives)
{
    auto original = one->style->getFilter();
    ASSERT_EQ(original, two->style->getFilter());
    ASSERT_EQ(original->primitive_count(), 1);

    ASSERT_TRUE(apply_tone(one, adjusted_settings()));
    document->ensureUpToDate();

    auto edited = one->style->getFilter();
    ASSERT_TRUE(edited);
    EXPECT_NE(edited, original);
    EXPECT_EQ(two->style->getFilter(), original);
    EXPECT_EQ(original->primitive_count(), 1);
    EXPECT_EQ(edited->primitive_count(), 2);
    EXPECT_TRUE(find_managed_tone_primitive(edited));
}

TEST_F(BitmapAdjustmentChemistryTest, PersistsEditableMetadataAndStandardTransferTables)
{
    auto const expected = adjusted_settings();
    ASSERT_TRUE(apply_tone(one, expected));
    document->ensureUpToDate();

    auto queried = query_tone(one);
    ASSERT_TRUE(queried);
    EXPECT_DOUBLE_EQ(queried->brightness, expected.brightness);
    EXPECT_DOUBLE_EQ(queried->contrast, expected.contrast);
    EXPECT_DOUBLE_EQ(queried->intensity, expected.intensity);
    EXPECT_DOUBLE_EQ(queried->highlights, expected.highlights);
    EXPECT_DOUBLE_EQ(queried->shadows, expected.shadows);
    EXPECT_DOUBLE_EQ(queried->midtones, expected.midtones);

    auto primitive = find_managed_tone_primitive(one->style->getFilter());
    ASSERT_TRUE(primitive);
    auto repr = primitive->getRepr();
    EXPECT_STREQ(repr->attribute(TONE_MARKER_ATTRIBUTE), TONE_MARKER_VALUE);
    ASSERT_EQ(repr->childCount(), 4);
    EXPECT_STREQ(repr->firstChild()->name(), "svg:feFuncR");
    EXPECT_STREQ(repr->firstChild()->attribute("type"), "table");
    EXPECT_NE(repr->firstChild()->attribute("tableValues"), nullptr);
    EXPECT_STREQ(repr->lastChild()->name(), "svg:feFuncA");
    EXPECT_STREQ(repr->lastChild()->attribute("type"), "identity");
}

TEST_F(BitmapAdjustmentChemistryTest, NoOpDoesNotRewriteAndResetPreservesExistingFilter)
{
    auto const settings = adjusted_settings();
    ASSERT_TRUE(apply_tone(one, settings));
    document->ensureUpToDate();
    auto filter = one->style->getFilter();
    ASSERT_TRUE(filter);

    EXPECT_FALSE(apply_tone(one, settings));
    EXPECT_TRUE(reset_tone(one));
    document->ensureUpToDate();

    EXPECT_EQ(one->style->getFilter(), filter);
    EXPECT_EQ(filter->primitive_count(), 1);
    EXPECT_FALSE(find_managed_tone_primitive(filter));
    EXPECT_FALSE(reset_tone(one));
}

TEST_F(BitmapAdjustmentChemistryTest, NeutralRemovesOnlyManagedTone)
{
    ASSERT_TRUE(apply_tone(one, adjusted_settings()));
    ASSERT_TRUE(apply_tone(one, {}));
    document->ensureUpToDate();
    ASSERT_TRUE(one->style->getFilter());
    EXPECT_EQ(one->style->getFilter()->primitive_count(), 1);
    EXPECT_FALSE(query_tone(one));
}

TEST_F(BitmapAdjustmentChemistryTest, SavedDocumentReopensWithEditableToneSettings)
{
    auto const expected = adjusted_settings();
    ASSERT_TRUE(apply_tone(one, expected));
    document->ensureUpToDate();

    auto serialized = sp_repr_save_buf(document->getReprDoc());
    auto reopened = SPDocument::createNewDocFromMem(serialized.raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto image = cast<SPImage>(reopened->getObjectById("one"));
    ASSERT_TRUE(image);
    auto actual = query_tone(image);
    ASSERT_TRUE(actual);
    EXPECT_DOUBLE_EQ(actual->brightness, expected.brightness);
    EXPECT_DOUBLE_EQ(actual->contrast, expected.contrast);
    EXPECT_DOUBLE_EQ(actual->intensity, expected.intensity);
    EXPECT_DOUBLE_EQ(actual->highlights, expected.highlights);
    EXPECT_DOUBLE_EQ(actual->shadows, expected.shadows);
    EXPECT_DOUBLE_EQ(actual->midtones, expected.midtones);
}

TEST(BitmapAdjustmentChemistryStandaloneTest, NeutralOnAnUnfilteredImageIsANoOp)
{
    if (!Application::exists()) Application::create(false);
    auto const svg = Glib::ustring::compose(
        R"(<svg xmlns="http://www.w3.org/2000/svg"><image id="image" width="1" height="1" href="%1"/></svg>)",
        image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);
    EXPECT_FALSE(apply_tone(image, {}));
    EXPECT_FALSE(image->style->filter.set);
}

TEST(BitmapAdjustmentChemistryStandaloneTest, PersistsToneOnAnUnfilteredImage)
{
    if (!Application::exists()) Application::create(false);
    auto const svg = Glib::ustring::compose(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"><image id="image" width="1" height="1" href="%1"/></svg>)",
        image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    Filters::BitmapToneSettings settings;
    settings.brightness = 1;
    ASSERT_TRUE(apply_tone(image, settings));
    document->ensureUpToDate();

    auto serialized = sp_repr_save_buf(document->getReprDoc());
    constexpr auto not_found = static_cast<Glib::ustring::size_type>(-1);
    EXPECT_NE(serialized.find("feComponentTransfer"), not_found);
    EXPECT_NE(serialized.find("inkscape:bitmap-adjustment=\"tone-v1\""),
              not_found);

    auto reopened = SPDocument::createNewDocFromMem(serialized.raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto reopened_image = cast<SPImage>(reopened->getObjectById("image"));
    ASSERT_TRUE(reopened_image);
    auto actual = query_tone(reopened_image);
    ASSERT_TRUE(actual);
    EXPECT_DOUBLE_EQ(actual->brightness, 1.0);
}

TEST(BitmapAdjustmentChemistryStandaloneTest, ResetDeletesUnusedManagedFilterDefinition)
{
    if (!Application::exists()) Application::create(false);
    auto const svg = Glib::ustring::compose(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"><image id="image" width="1" height="1" href="%1"/></svg>)",
        image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    Filters::BitmapToneSettings settings;
    settings.brightness = 1;
    ASSERT_TRUE(apply_tone(image, settings));
    document->ensureUpToDate();
    auto filter = image->style->getFilter();
    ASSERT_TRUE(filter);
    ASSERT_NE(filter->getId(), nullptr);
    auto const filter_id = std::string{filter->getId()};

    ASSERT_TRUE(reset_tone(image));
    document->ensureUpToDate();

    EXPECT_FALSE(image->style->filter.set);
    EXPECT_EQ(document->getObjectById(filter_id.c_str()), nullptr);
    auto serialized = sp_repr_save_buf(document->getReprDoc());
    constexpr auto not_found = static_cast<Glib::ustring::size_type>(-1);
    EXPECT_EQ(serialized.find("<filter"), not_found);
    EXPECT_EQ(serialized.find("feComponentTransfer"), not_found);
    EXPECT_EQ(serialized.find("inkscape:bitmap-adjustment=\"tone-v1\""), not_found);
}

TEST(BitmapAdjustmentChemistryStandaloneTest, ResetDoesNotDeleteSharedManagedFilter)
{
    if (!Application::exists()) Application::create(false);
    auto const svg = Glib::ustring::compose(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"><image id="one" width="1" height="1" href="%1"/><image id="two" x="1" width="1" height="1" href="%1"/></svg>)",
        image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto one = cast<SPImage>(document->getObjectById("one"));
    auto two = cast<SPImage>(document->getObjectById("two"));
    ASSERT_TRUE(one);
    ASSERT_TRUE(two);

    Filters::BitmapToneSettings settings;
    settings.brightness = 1;
    ASSERT_TRUE(apply_tone(one, settings));
    auto shared = one->style->getFilter();
    ASSERT_TRUE(shared);
    sp_style_set_property_url(two, "filter", shared, false);
    document->ensureUpToDate();
    ASSERT_EQ(two->style->getFilter(), shared);

    ASSERT_TRUE(reset_tone(one));
    document->ensureUpToDate();

    EXPECT_FALSE(one->style->filter.set);
    EXPECT_EQ(two->style->getFilter(), shared);
    EXPECT_EQ(shared->primitive_count(), 1);
    auto remaining = query_tone(two);
    ASSERT_TRUE(remaining);
    EXPECT_DOUBLE_EQ(remaining->brightness, 1.0);
}

TEST(BitmapAdjustmentChemistryStandaloneTest, UndoCheckpointPreservesNewTonePrimitive)
{
    if (!Application::exists()) Application::create(false);
    auto const svg = Glib::ustring::compose(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"><image id="image" width="1" height="1" href="%1"/></svg>)",
        image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    Filters::BitmapToneSettings settings;
    settings.brightness = 1;
    ASSERT_TRUE(apply_tone(image, settings));
    ASSERT_TRUE(query_tone(image));
    DocumentUndo::done(document.get(),
                       Util::Internal::ContextString{"Adjust bitmap tone"},
                       "shape-image");
    document->ensureUpToDate();

    auto actual = query_tone(image);
    ASSERT_TRUE(actual);
    EXPECT_DOUBLE_EQ(actual->brightness, 1.0);
    auto serialized = sp_repr_save_buf(document->getReprDoc());
    constexpr auto not_found = static_cast<Glib::ustring::size_type>(-1);
    EXPECT_NE(serialized.find("feComponentTransfer"), not_found);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    auto undone_image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(undone_image);
    EXPECT_FALSE(undone_image->style->filter.set);

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    auto redone_image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(redone_image);
    auto redone = query_tone(redone_image);
    ASSERT_TRUE(redone);
    EXPECT_DOUBLE_EQ(redone->brightness, 1.0);
}

TEST(BitmapAdjustmentChemistryStandaloneTest, ResetCleanupRoundTripsThroughUndoRedo)
{
    if (!Application::exists()) Application::create(false);
    auto const svg = Glib::ustring::compose(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"><image id="image" width="1" height="1" href="%1"/></svg>)",
        image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();
    auto image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(image);

    Filters::BitmapToneSettings settings;
    settings.brightness = 1;
    ASSERT_TRUE(apply_tone(image, settings));
    DocumentUndo::done(document.get(),
                       Util::Internal::ContextString{"Adjust bitmap tone"},
                       "shape-image");
    ASSERT_TRUE(reset_tone(image));
    DocumentUndo::done(document.get(),
                       Util::Internal::ContextString{"Reset bitmap tone"},
                       "shape-image");
    document->ensureUpToDate();

    auto serialized = sp_repr_save_buf(document->getReprDoc());
    constexpr auto not_found = static_cast<Glib::ustring::size_type>(-1);
    EXPECT_EQ(serialized.find("<filter"), not_found);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    auto undone_image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(undone_image);
    auto undone = query_tone(undone_image);
    ASSERT_TRUE(undone);
    EXPECT_DOUBLE_EQ(undone->brightness, 1.0);

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    auto redone_image = cast<SPImage>(document->getObjectById("image"));
    ASSERT_TRUE(redone_image);
    EXPECT_FALSE(redone_image->style->filter.set);
    serialized = sp_repr_save_buf(document->getReprDoc());
    EXPECT_EQ(serialized.find("<filter"), not_found);
}

TEST(BitmapAdjustmentChemistryStandaloneTest, ImportCopiesToneWithoutSharingEditableState)
{
    if (!Application::exists()) Application::create(false);
    auto const source_svg = Glib::ustring::compose(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"><image id="source" width="1" height="1" href="%1"/></svg>)",
        image_data);
    auto source = SPDocument::createNewDocFromMem(source_svg.raw());
    auto target = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"/>)");
    ASSERT_TRUE(source);
    ASSERT_TRUE(target);
    source->ensureUpToDate();
    auto source_image = cast<SPImage>(source->getObjectById("source"));
    ASSERT_TRUE(source_image);
    auto const source_settings = adjusted_settings();
    ASSERT_TRUE(apply_tone(source_image, source_settings));
    source->ensureUpToDate();

    std::vector<XML::Node *> pasted;
    target->import(*source, nullptr, nullptr, Geom::Affine(), &pasted,
                   SPDocument::ImportRoot::Single);
    target->ensureUpToDate();
    ASSERT_EQ(pasted.size(), 1u);
    auto pasted_image = cast<SPImage>(target->getObjectByRepr(pasted.front()));
    ASSERT_TRUE(pasted_image);
    auto pasted_settings = query_tone(pasted_image);
    ASSERT_TRUE(pasted_settings);
    EXPECT_DOUBLE_EQ(pasted_settings->brightness, source_settings.brightness);
    EXPECT_DOUBLE_EQ(pasted_settings->contrast, source_settings.contrast);
    ASSERT_TRUE(source_image->style->getFilter());
    ASSERT_TRUE(pasted_image->style->getFilter());
    EXPECT_NE(source_image->style->getFilter(), pasted_image->style->getFilter());

    auto changed_settings = source_settings;
    changed_settings.brightness = -35.0;
    ASSERT_TRUE(apply_tone(pasted_image, changed_settings));
    target->ensureUpToDate();
    auto unchanged_source = query_tone(source_image);
    ASSERT_TRUE(unchanged_source);
    EXPECT_DOUBLE_EQ(unchanged_source->brightness, source_settings.brightness);
    auto changed_paste = query_tone(pasted_image);
    ASSERT_TRUE(changed_paste);
    EXPECT_DOUBLE_EQ(changed_paste->brightness, changed_settings.brightness);

    auto const serialized = sp_repr_save_buf(target->getReprDoc());
    auto reopened = SPDocument::createNewDocFromMem(serialized.raw());
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto reopened_images = reopened->getObjectsBySelector("image");
    ASSERT_EQ(reopened_images.size(), 1u);
    auto reopened_image = cast<SPImage>(reopened_images.front());
    ASSERT_TRUE(reopened_image);
    auto reopened_settings = query_tone(reopened_image);
    ASSERT_TRUE(reopened_settings);
    EXPECT_DOUBLE_EQ(reopened_settings->brightness, changed_settings.brightness);
    EXPECT_DOUBLE_EQ(reopened_settings->contrast, changed_settings.contrast);
}

TEST(BitmapAdjustmentChemistryStandaloneTest, AppliesNonDestructivelyToVectorTextGroupAndClone)
{
    if (!Application::exists()) Application::create(false);
    auto document = SPDocument::createNewDocFromMem(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <path id="path" d="M0,0h10v10z"/>
  <text id="text" x="0" y="20">Editable</text>
  <g id="group"><rect id="child" y="20" width="10" height="10"/></g>
  <use id="clone" href="#path" x="20"/>
</svg>)svg");
    ASSERT_TRUE(document);
    document->ensureUpToDate();

    auto const settings = adjusted_settings();
    for (auto id : {"path", "text", "group", "clone"}) {
        auto item = cast<SPItem>(document->getObjectById(id));
        ASSERT_TRUE(item) << id;
        auto const element_name = std::string{item->getRepr()->name()};
        ASSERT_TRUE(apply_tone(item, settings)) << id;
        document->ensureUpToDate();
        EXPECT_EQ(std::string{item->getRepr()->name()}, element_name) << id;
        auto queried = query_tone(item);
        ASSERT_TRUE(queried) << id;
        EXPECT_TRUE(Filters::bitmap_tone_settings_equal(*queried, settings)) << id;
    }

    auto const serialized = sp_repr_save_buf(document->getReprDoc());
    constexpr auto not_found = static_cast<Glib::ustring::size_type>(-1);
    EXPECT_EQ(serialized.find("<image"), not_found);
    EXPECT_NE(serialized.find("<svg:path"), not_found);
    EXPECT_NE(serialized.find("<svg:text"), not_found);
    EXPECT_NE(serialized.find("<svg:use"), not_found);
}

TEST_F(BitmapAdjustmentChemistryTest, IncompatibleAssignmentPreservesExistingFilterAndDocument)
{
    auto svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg"
      xmlns:xlink="http://www.w3.org/1999/xlink">
      <defs><filter id="old"><feGaussianBlur stdDeviation="1"/></filter>
      <filter id="recursive"><feImage xlink:href="#target"/></filter></defs>
      <rect id="target" width="10" height="10" style="filter:url(#old)"/>
      <rect id="other" x="20" width="10" height="10"/>
    </svg>)svg";
    auto doc = SPDocument::createNewDocFromMem(std::string_view(svg));
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    auto target = cast<SPItem>(doc->getObjectById("target"));
    auto other = cast<SPItem>(doc->getObjectById("other"));
    auto filter = cast<SPFilter>(doc->getObjectById("recursive"));
    ASSERT_TRUE(target); ASSERT_TRUE(other); ASSERT_TRUE(filter);
    ASSERT_FALSE(filter->valid_for(target));
    auto before = sp_repr_save_buf(doc->getReprDoc());
    EXPECT_EQ(assign_filter_preserving_incompatible(target, filter), FilterAssignmentResult::Incompatible);
    EXPECT_EQ(sp_repr_save_buf(doc->getReprDoc()), before);
    EXPECT_EQ(assign_filter_preserving_incompatible(other, filter), FilterAssignmentResult::Changed);
    doc->ensureUpToDate();
    EXPECT_EQ(other->style->getFilter(), filter);
    EXPECT_EQ(target->style->getFilter(), doc->getObjectById("old"));
    EXPECT_EQ(assign_filter_preserving_incompatible(other, filter), FilterAssignmentResult::Unchanged);
    EXPECT_EQ(assign_filter_preserving_incompatible(other, nullptr), FilterAssignmentResult::Changed);
}

TEST(BitmapAdjustmentChemistryStandaloneTest, ResolveToneTargetsHeadless)
{
    if (!Application::exists()) Application::create(false);
    auto const svg = Glib::ustring::compose(R"svg(<svg xmlns="http://www.w3.org/2000/svg"
 xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="10" height="10">
 <image id="a" width="2" height="2" href="%1"/>
 <image id="b" x="2" width="2" height="2" href="%1"/>
 <g id="g"><image id="c" width="2" height="2" href="%1"/></g>
 <image id="missing" x="4" width="2" height="2" href="does-not-exist.png"/>
</svg>)svg", image_data);
    auto document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();

    auto a = cast<SPItem>(document->getObjectById("a"));
    auto b = cast<SPItem>(document->getObjectById("b"));
    auto g = cast<SPItem>(document->getObjectById("g"));
    auto c = cast<SPItem>(document->getObjectById("c"));
    auto missing = cast<SPItem>(document->getObjectById("missing"));
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_TRUE(g);
    ASSERT_TRUE(c);
    ASSERT_TRUE(missing);

    std::vector<SPItem *> const selected = {a, g, c, missing, b};
    auto const resolved = resolve_tone_targets(selected, [](SPItem *) { return true; });
    ASSERT_EQ(resolved.items.size(), 3u);
    EXPECT_EQ(resolved.items[0], a);
    EXPECT_EQ(resolved.items[1], g);
    EXPECT_EQ(resolved.items[2], b);
    EXPECT_EQ(resolved.covered, 1u);
    EXPECT_EQ(resolved.missing_sources, 1u);
    EXPECT_EQ(resolved.unavailable, 0u);

    auto const partial = resolve_tone_targets(selected, [](SPItem *item) {
        return std::string(item->getId()) != "b";
    });
    ASSERT_EQ(partial.items.size(), 2u);
    EXPECT_EQ(partial.items[0], a);
    EXPECT_EQ(partial.items[1], g);
    EXPECT_EQ(partial.unavailable, 1u);

    Filters::BitmapToneSettings settings;
    settings.brightness = 40;
    ASSERT_TRUE(apply_tone(a, settings));
    document->ensureUpToDate();

    auto const tone = aggregate_tone({a, b});
    auto const brightness = static_cast<std::size_t>(Filters::BitmapToneProperty::Brightness);
    auto const contrast = static_cast<std::size_t>(Filters::BitmapToneProperty::Contrast);
    EXPECT_DOUBLE_EQ(tone[brightness].value, 40.0);
    EXPECT_TRUE(tone[brightness].mixed);
    EXPECT_DOUBLE_EQ(tone[contrast].value, 0.0);
    EXPECT_FALSE(tone[contrast].mixed);
}
