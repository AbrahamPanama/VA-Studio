// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "display/drawing.h"
#include "document.h"
#include "inkscape.h"
#include "libnrtype/font-lister.h"
#include "libnrtype/font-factory.h"
#include "libnrtype/font-instance.h"
#include "object/sp-root.h"
#include "object/sp-text.h"
#include "style.h"
#include "style-text.h"
#include "xml/repr.h"
#include "ui/text-target-utils.h"
#include "util/document-fonts.h"

using namespace Inkscape;
using namespace std::literals;

namespace {

class TextFontPreviewTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists()) {
            Application::create(false);
        }

        constexpr auto svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="300" height="140">
  <text id="text" x="10" y="50" style="font-family:sans-serif;font-size:24px">office café Straße í</text>
  <text id="normal-caps" x="10" y="90" style="font-family:sans-serif;font-size:24px">office</text>
  <text id="small-caps" x="10" y="130" style="font-family:sans-serif;font-size:24px;font-variant-caps:small-caps">office</text>
  <text id="forced-caps" x="10" y="170" style="font-family:sans-serif;font-size:24px;font-variant-caps:small-caps;-inkscape-font-variant-caps-mode:synthesized">office</text>
  <text id="normal-position" x="10" y="210" style="font-family:sans-serif;font-size:24px">12345</text>
  <text id="super-position" x="10" y="250" style="font-family:sans-serif;font-size:24px;font-variant-position:super">12345</text>
</svg>)"sv;
        document = SPDocument::createNewDocFromMem(svg);
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        text = cast<SPText>(document->getObjectById("text"));
        ASSERT_TRUE(text);

        first_key = SPItem::display_key_new(1);
        second_key = SPItem::display_key_new(1);
        first_root = document->getRoot()->invoke_show(first_drawing, first_key, SP_ITEM_SHOW_DISPLAY);
        second_root = document->getRoot()->invoke_show(second_drawing, second_key, SP_ITEM_SHOW_DISPLAY);
        first_drawing.setRoot(first_root);
        second_drawing.setRoot(second_root);
        first_drawing.update();
        second_drawing.update();
    }

    void TearDown() override
    {
        document->getRoot()->invoke_hide(first_key);
        document->getRoot()->invoke_hide(second_key);
    }

    std::unique_ptr<SPDocument> document;
    SPText *text = nullptr;
    Drawing first_drawing;
    Drawing second_drawing;
    DrawingItem *first_root = nullptr;
    DrawingItem *second_root = nullptr;
    unsigned first_key = 0;
    unsigned second_key = 0;
};

TEST_F(TextFontPreviewTest, IsViewLocalAndNonpersistent)
{
    auto const *style_before = text->getRepr()->attribute("style");
    auto const canonical_bounds = text->layout.bounds(Geom::Affine());
    auto const character_count = static_cast<unsigned>(text->layout.iteratorToCharIndex(text->layout.end()));

    Text::Layout::FontOverride font_override;
    font_override.first_char = 0;
    font_override.last_char = character_count;
    font_override.family = "serif";
    font_override.fontspec = "serif";

    ASSERT_TRUE(text->setFontPreview(first_key, {font_override}));
    EXPECT_NE(text->displayLayout(first_key), &text->layout);
    EXPECT_EQ(text->displayLayout(second_key), &text->layout);
    EXPECT_EQ(text->layout.bounds(Geom::Affine()), canonical_bounds);
    EXPECT_STREQ(text->getRepr()->attribute("style"), style_before);
    EXPECT_FALSE(document->isModifiedSinceSave());

    text->clearFontPreview(first_key);
    EXPECT_EQ(text->displayLayout(first_key), &text->layout);
    EXPECT_EQ(text->layout.bounds(Geom::Affine()), canonical_bounds);
    EXPECT_STREQ(text->getRepr()->attribute("style"), style_before);
    EXPECT_FALSE(document->isModifiedSinceSave());
}

TEST_F(TextFontPreviewTest, SupportsLogicalSubrangesWithoutChangingCanonicalLayout)
{
    auto const canonical_bounds = text->layout.bounds(Geom::Affine());
    Text::Layout::FontOverride font_override;
    font_override.first_char = 1;
    font_override.last_char = 5;
    font_override.family = "serif";
    font_override.fontspec = "serif";

    ASSERT_TRUE(text->setFontPreview(first_key, {font_override}));
    auto preview = text->displayLayout(first_key);
    ASSERT_NE(preview, &text->layout);
    EXPECT_EQ(preview->iteratorToCharIndex(preview->end()), text->layout.iteratorToCharIndex(text->layout.end()));
    EXPECT_EQ(text->layout.bounds(Geom::Affine()), canonical_bounds);

    text->clearFontPreview(first_key);
}

TEST_F(TextFontPreviewTest, FamilyPreviewUsesRequestedCssInsteadOfLegacyFace)
{
    text->setAttribute("style", "font-family:serif;font-size:24px;font-weight:bold;font-style:oblique;"
                               "font-stretch:condensed;font-variation-settings:'wght' 650;"
                               "-inkscape-font-specification:'sans-serif Regular'");
    document->ensureUpToDate();
    auto const original = sp_repr_save_buf(document->getReprDoc());
    Text::Layout::TypographyOverride override;
    override.first_char = 0;
    override.last_char = text->layout.iteratorToCharIndex(text->layout.end());
    override.family = "monospace";
    ASSERT_TRUE(text->setTextStylePreview(first_key, {override}));
    auto const preview = text->displayLayout(first_key)->bounds(Geom::Affine());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), original);
    EXPECT_EQ(text->displayLayout(second_key), &text->layout);
    text->clearTextStylePreview(first_key);
    auto css = sp_repr_css_attr_new();
    sp_repr_css_set_property(css, "font-family", "monospace");
    sp_repr_css_set_property(css, "-inkscape-font-specification", "");
    text->changeCSS(css, "style");
    sp_repr_css_attr_unref(css);
    document->ensureUpToDate();
    auto const committed = text->layout.bounds(Geom::Affine());
    ASSERT_TRUE(preview && committed);
    for (unsigned axis = 0; axis < 2; ++axis) {
        EXPECT_NEAR(preview->min()[axis], committed->min()[axis], 1e-6);
        EXPECT_NEAR(preview->max()[axis], committed->max()[axis], 1e-6);
    }
    EXPECT_EQ(text->style->font_weight.computed, 700);
    EXPECT_EQ(text->style->font_style.computed, SP_CSS_FONT_STYLE_OBLIQUE);
    EXPECT_EQ(text->style->font_stretch.computed, SP_CSS_FONT_STRETCH_CONDENSED);
}

TEST_F(TextFontPreviewTest, CandidateFaceControlsSuperAndSubscriptFallbackInBothDirections)
{
    // Repository-owned fonts make the presence/absence distinction explicit.
    // Require the fixture pair; a silently substituted system font is not a pass.
    auto &factory = FontFactory::get();
    factory.AddFontFile(INKSCAPE_TESTS_DIR "/rendering_tests/fonts/NotoSans-Regular.ttf");
    factory.AddFontFile(INKSCAPE_TESTS_DIR "/rendering_tests/fonts/FreeSans.ttf");
    factory.AddFontFile(INKSCAPE_TESTS_DIR "/rendering_tests/fonts/Lavi.ttf");
    for (auto tag : {"sups", "subs"}) {
        Glib::ustring with_feature, without_feature;
        for (auto family : {"Noto Sans", "FreeSans", "Lavi"}) {
            auto font = factory.FaceFromDescr(family, "Normal");
            ASSERT_TRUE(font);
            auto actual = pango_font_describe(font->get_font());
            EXPECT_STREQ(pango_font_description_get_family(actual), family);
            pango_font_description_free(actual);
            if (font->get_opentype_tables().contains(tag)) with_feature = family;
            else without_feature = family;
        }
        ASSERT_FALSE(with_feature.empty()) << "Missing controlled font with " << tag;
        ASSERT_FALSE(without_feature.empty()) << "Missing controlled font without " << tag;
        for (bool reverse : {false, true}) {
            auto const from = reverse ? with_feature : without_feature;
            auto const to = reverse ? without_feature : with_feature;
            auto const position = std::string(tag) == "sups" ? "super" : "sub";
            SCOPED_TRACE(from.raw() + " -> " + to.raw() + " " + position);
            text->setAttribute("style", ("font-family:'" + from + "';font-size:24px;font-variant-position:" + position).c_str());
            document->ensureUpToDate();
            auto const canonical = text->layout.bounds(Geom::Affine());
            auto const before = sp_repr_save_buf(document->getReprDoc());
            Text::Layout::TypographyOverride override;
            override.first_char = 0;
            override.last_char = text->layout.iteratorToCharIndex(text->layout.end());
            override.family = to;
            ASSERT_TRUE(text->setTextStylePreview(first_key, {override}));
            auto const preview = text->displayLayout(first_key)->bounds(Geom::Affine());
            EXPECT_EQ(text->layout.bounds(Geom::Affine()), canonical);
            EXPECT_EQ(text->displayLayout(second_key), &text->layout);
            EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()), before);
            text->clearTextStylePreview(first_key);
            auto css = sp_repr_css_attr_new();
            sp_repr_css_set_property(css, "font-family", to.c_str());
            text->changeCSS(css, "style");
            sp_repr_css_attr_unref(css);
            document->ensureUpToDate();
            auto const committed = text->layout.bounds(Geom::Affine());
            ASSERT_TRUE(preview && committed);
            for (unsigned axis = 0; axis < 2; ++axis) {
                EXPECT_NEAR(preview->min()[axis], committed->min()[axis], 1e-6);
                EXPECT_NEAR(preview->max()[axis], committed->max()[axis], 1e-6);
            }
        }
    }
}

TEST_F(TextFontPreviewTest, SupportsTransientCapitalizationWithoutChangingSource)
{
    auto const style_before = std::string{text->getRepr()->attribute("style")};
    auto const canonical_bounds = text->layout.bounds(Geom::Affine());
    auto const character_count = static_cast<unsigned>(text->layout.iteratorToCharIndex(text->layout.end()));

    Text::Layout::TypographyOverride override;
    override.first_char = 0;
    override.last_char = character_count;
    override.text_transform = SP_CSS_TEXT_TRANSFORM_UPPERCASE;

    ASSERT_TRUE(text->setTextStylePreview(first_key, {override}));
    auto preview = text->displayLayout(first_key);
    ASSERT_NE(preview, &text->layout);
    EXPECT_NE(preview->bounds(Geom::Affine()), canonical_bounds);
    EXPECT_EQ(text->layout.bounds(Geom::Affine()), canonical_bounds);
    EXPECT_EQ(text->getRepr()->attribute("style"), style_before);
    EXPECT_FALSE(document->isModifiedSinceSave());

    text->clearTextStylePreview(first_key);
}

TEST_F(TextFontPreviewTest, PreservesLogicalMappingAcrossTypographyOverrides)
{
    auto const source_before = std::string{text->getRepr()->firstChild()->content()};
    auto const character_count = static_cast<unsigned>(text->layout.iteratorToCharIndex(text->layout.end()));

    std::vector<Text::Layout::TypographyOverride> overrides(6);
    for (auto &override : overrides) {
        override.first_char = 0;
        override.last_char = character_count;
    }
    overrides[0].text_transform = SP_CSS_TEXT_TRANSFORM_UPPERCASE;
    overrides[1].font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_TITLING;
    overrides[2].font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_SMALL;
    overrides[3].font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_ALL_SMALL;
    overrides[4].font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_SMALL;
    overrides[4].force_synthesized_caps = true;
    overrides[5].font_variant_position = SP_CSS_FONT_VARIANT_POSITION_SUPER;

    for (auto const &override : overrides) {
        ASSERT_TRUE(text->setTextStylePreview(first_key, {override}));
        auto preview = text->displayLayout(first_key);
        ASSERT_NE(preview, &text->layout);
        EXPECT_EQ(preview->iteratorToCharIndex(preview->end()), character_count);
        EXPECT_EQ(text->displayLayout(second_key), &text->layout);
        EXPECT_EQ(text->getRepr()->firstChild()->content(), source_before);
        EXPECT_FALSE(document->isModifiedSinceSave());
        text->clearTextStylePreview(first_key);
    }
}

TEST_F(TextFontPreviewTest, ReplacesDerivedCapsFeaturesDuringPreview)
{
    auto normal = cast<SPText>(document->getObjectById("normal-caps"));
    auto small = cast<SPText>(document->getObjectById("small-caps"));
    ASSERT_TRUE(normal);
    ASSERT_TRUE(small);
    auto const count = static_cast<unsigned>(small->layout.iteratorToCharIndex(small->layout.end()));

    Text::Layout::TypographyOverride override;
    override.first_char = 0;
    override.last_char = count;
    override.font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_NORMAL;
    override.c2sc = false;

    ASSERT_TRUE(small->setTextStylePreview(first_key, {override}));
    auto const normal_bounds = normal->layout.bounds(Geom::Affine());
    auto const preview_bounds = small->displayLayout(first_key)->bounds(Geom::Affine());
    ASSERT_TRUE(normal_bounds);
    ASSERT_TRUE(preview_bounds);
    EXPECT_NEAR(preview_bounds->width(), normal_bounds->width(), 0.01);
    EXPECT_FALSE(document->isModifiedSinceSave());
    small->clearTextStylePreview(first_key);
}

TEST_F(TextFontPreviewTest, CancelsForcedCapsRenderingInANormalCapsPreview)
{
    auto normal = cast<SPText>(document->getObjectById("normal-caps"));
    auto forced = cast<SPText>(document->getObjectById("forced-caps"));
    ASSERT_TRUE(normal);
    ASSERT_TRUE(forced);
    auto const count = static_cast<unsigned>(forced->layout.iteratorToCharIndex(forced->layout.end()));

    Text::Layout::TypographyOverride override;
    override.first_char = 0;
    override.last_char = count;
    override.text_transform = SP_CSS_TEXT_TRANSFORM_NONE;
    override.font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_NORMAL;
    override.c2sc = false;

    ASSERT_TRUE(forced->setTextStylePreview(first_key, {override}));
    auto const normal_bounds = normal->layout.bounds(Geom::Affine());
    auto const preview_bounds = forced->displayLayout(first_key)->bounds(Geom::Affine());
    ASSERT_TRUE(normal_bounds);
    ASSERT_TRUE(preview_bounds);
    EXPECT_NEAR(preview_bounds->width(), normal_bounds->width(), 0.01);
    EXPECT_NEAR(preview_bounds->height(), normal_bounds->height(), 0.01);
    forced->clearTextStylePreview(first_key);
}

TEST_F(TextFontPreviewTest, CancelsDerivedCapsWithoutExplicitFeatureOverride)
{
    auto normal = cast<SPText>(document->getObjectById("normal-caps"));
    auto small = cast<SPText>(document->getObjectById("small-caps"));
    ASSERT_TRUE(normal);
    ASSERT_TRUE(small);
    auto const style_before = std::string{small->getRepr()->attribute("style")};
    auto const canonical_bounds = small->layout.bounds(Geom::Affine());

    Text::Layout::TypographyOverride override;
    override.first_char = 0;
    override.last_char = static_cast<unsigned>(small->layout.iteratorToCharIndex(small->layout.end()));
    override.font_variant_caps = SP_CSS_FONT_VARIANT_CAPS_NORMAL;
    // No c2sc override: changing caps alone must remove its derived features.
    ASSERT_FALSE(override.c2sc.has_value());

    ASSERT_TRUE(small->setTextStylePreview(first_key, {override}));
    auto const normal_bounds = normal->layout.bounds(Geom::Affine());
    auto const preview_bounds = small->displayLayout(first_key)->bounds(Geom::Affine());
    ASSERT_TRUE(normal_bounds);
    ASSERT_TRUE(preview_bounds);
    EXPECT_NEAR(preview_bounds->width(), normal_bounds->width(), 0.01);
    EXPECT_NEAR(preview_bounds->height(), normal_bounds->height(), 0.01);
    EXPECT_EQ(small->layout.bounds(Geom::Affine()), canonical_bounds);
    EXPECT_EQ(small->displayLayout(second_key), &small->layout);
    EXPECT_EQ(small->getRepr()->attribute("style"), style_before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    small->clearTextStylePreview(first_key);
    EXPECT_EQ(small->displayLayout(first_key), &small->layout);
}

TEST_F(TextFontPreviewTest, CancelsScriptPositionInANormalPositionPreview)
{
    auto normal = cast<SPText>(document->getObjectById("normal-position"));
    auto superscript = cast<SPText>(document->getObjectById("super-position"));
    ASSERT_TRUE(normal);
    ASSERT_TRUE(superscript);
    auto const count = static_cast<unsigned>(
        superscript->layout.iteratorToCharIndex(superscript->layout.end()));

    Text::Layout::TypographyOverride override;
    override.first_char = 0;
    override.last_char = count;
    override.font_variant_position = SP_CSS_FONT_VARIANT_POSITION_NORMAL;

    ASSERT_TRUE(superscript->setTextStylePreview(first_key, {override}));
    auto const normal_bounds = normal->layout.bounds(Geom::Affine());
    auto const preview_bounds = superscript->displayLayout(first_key)->bounds(Geom::Affine());
    ASSERT_TRUE(normal_bounds);
    ASSERT_TRUE(preview_bounds);
    EXPECT_NEAR(preview_bounds->width(), normal_bounds->width(), 0.01);
    EXPECT_NEAR(preview_bounds->height(), normal_bounds->height(), 0.01);
    superscript->clearTextStylePreview(first_key);
}

TEST(TextLayoutTypographyTest, AddsLanguageSpacingOnlyAtScriptBoundaries)
{
    if (!Application::exists()) Application::create(false);
    constexpr auto svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="500" height="200">
  <text id="normal" x="10" y="40" style="font-family:sans-serif;font-size:32px">A中B</text>
  <text id="spaced" x="10" y="80" style="font-family:sans-serif;font-size:32px;-inkscape-language-spacing:100%">A中B</text>
  <text id="explicit" x="10" y="120" style="font-family:sans-serif;font-size:32px">A 中</text>
  <text id="explicit-spaced" x="10" y="160" style="font-family:sans-serif;font-size:32px;-inkscape-language-spacing:100%">A 中</text>
  <text id="runs" x="10" y="200" style="font-family:sans-serif;font-size:32px">A<tspan>中</tspan>B</text>
  <text id="runs-spaced" x="10" y="240" style="font-family:sans-serif;font-size:32px;-inkscape-language-spacing:100%">A<tspan>中</tspan>B</text>
  <text id="punctuation" x="10" y="280" style="font-family:sans-serif;font-size:32px">A。中</text>
  <text id="punctuation-spaced" x="10" y="320" style="font-family:sans-serif;font-size:32px;-inkscape-language-spacing:100%">A。中</text>
  <text id="combining" x="10" y="360" style="font-family:sans-serif;font-size:32px">Á中</text>
  <text id="combining-spaced" x="10" y="400" style="font-family:sans-serif;font-size:32px;-inkscape-language-spacing:100%">Á中</text>
  <text id="number" x="10" y="440" style="font-family:sans-serif;font-size:32px">1中</text>
  <text id="number-spaced" x="10" y="480" style="font-family:sans-serif;font-size:32px;-inkscape-language-spacing:100%">1中</text>
  <text id="bidi" x="10" y="520" style="font-family:sans-serif;font-size:32px;direction:rtl">A中</text>
  <text id="bidi-spaced" x="10" y="560" style="font-family:sans-serif;font-size:32px;direction:rtl;-inkscape-language-spacing:100%">A中</text>
  <text id="vertical" x="10" y="600" style="font-family:sans-serif;font-size:32px;writing-mode:vertical-rl">A中B</text>
  <text id="vertical-spaced" x="60" y="600" style="font-family:sans-serif;font-size:32px;writing-mode:vertical-rl;-inkscape-language-spacing:100%">A中B</text>
</svg>)"sv;
    auto document = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(document);
    document->ensureUpToDate();

    auto width = [&](char const *id) {
        auto item = cast<SPText>(document->getObjectById(id));
        EXPECT_TRUE(item);
        auto bounds = item ? item->layout.bounds(Geom::Affine()) : Geom::OptRect{};
        EXPECT_TRUE(bounds);
        return bounds ? bounds->width() : 0.0;
    };

    EXPECT_GT(width("spaced"), width("normal") + 5.0);
    EXPECT_NEAR(width("explicit-spaced"), width("explicit"), 0.01);
    EXPECT_GT(width("runs-spaced"), width("runs") + 5.0);
    EXPECT_NEAR(width("punctuation-spaced"), width("punctuation"), 0.01);
    EXPECT_GT(width("combining-spaced"), width("combining") + 2.0);
    EXPECT_GT(width("number-spaced"), width("number") + 2.0);
    EXPECT_GT(width("bidi-spaced"), width("bidi") + 2.0);

    auto height = [&](char const *id) {
        auto item = cast<SPText>(document->getObjectById(id));
        auto bounds = item ? item->layout.bounds(Geom::Affine()) : Geom::OptRect{};
        EXPECT_TRUE(bounds);
        return bounds ? bounds->height() : 0.0;
    };
    EXPECT_GT(height("vertical-spaced"), height("vertical") + 5.0);
}

TEST(TextFontUiTest, QueriesFamilyStylesWithoutChangingSharedFontSelection)
{
    if (!Application::exists()) Application::create(false);
    auto lister = FontLister::get_instance();
    auto const current_family = lister->get_font_family();

    Glib::ustring candidate;
    for (auto const &row : lister->get_font_list()->children()) {
        Glib::ustring const family = row[lister->font_list.family];
        if (!family.empty() && family != current_family) {
            candidate = family;
            break;
        }
    }
    ASSERT_FALSE(candidate.empty());

    auto snapshot_styles = [lister] {
        std::vector<std::pair<Glib::ustring, Glib::ustring>> result;
        for (auto const &row : lister->get_style_list()->children()) {
            result.emplace_back(row[lister->font_style_list.cssStyle],
                                row[lister->font_style_list.displayStyle]);
        }
        return result;
    };
    auto const style_store_before = snapshot_styles();

    auto const styles = lister->get_font_styles(candidate);
    ASSERT_TRUE(styles);
    EXPECT_FALSE(styles->empty());
    EXPECT_EQ(lister->get_font_family(), current_family);
    EXPECT_EQ(snapshot_styles(), style_store_before);
}

TEST(TextFontUiTest, IndexedStylesMatchLinearLookupAcrossRefreshes)
{
    if (!Application::exists()) Application::create(false);
    auto lister = FontLister::get_instance();
    auto linear = [lister](Glib::ustring const &family) -> std::shared_ptr<FontLister::Styles const> {
        for (auto iter = lister->get_font_list()->children().begin();
             iter != lister->get_font_list()->children().end(); ++iter) {
            Glib::ustring const name = (*iter)[lister->font_list.family];
            if (family.casefold().compare(name.casefold()) != 0) continue;
            lister->ensureRowStyles(iter);
            return (*iter).get_value(lister->font_list.styles);
        }
        return {};
    };
    auto verify = [&](Glib::ustring const &family) {
        for (auto const &name : {family, family.uppercase()}) {
            auto const expected = linear(name);
            auto const actual = lister->get_font_styles(name);
            ASSERT_TRUE(actual);
            if (expected) {
                EXPECT_EQ(actual, expected) << name;
            } else {
                std::vector<Glib::ustring> css;
                for (auto const &style : *actual) css.push_back(style.css_name);
                std::vector<Glib::ustring> defaults{"Normal", "Italic", "Bold", "Bold Italic"};
                EXPECT_EQ(css, defaults) << name;
            }
        }
    };
    unsigned checked = 0;
    for (auto const &row : lister->get_font_list()->children()) {
        Glib::ustring const family = row[lister->font_list.family];
        if (!family.empty() && family != "#") {
            verify(family);
            if (++checked == 5) break;
        }
    }
    ASSERT_GE(checked, 3u);
    verify("ST-Q missing family");
    lister->insert_font_family("ST-Q missing family");
    verify("ST-Q missing family");
    lister->insert_font_family("Sans"); // document row must precede its system namesake
    verify("Sans");
    constexpr auto svg = R"(<svg xmlns="http://www.w3.org/2000/svg"><text style="font-family:Sans">ST-Q</text><text style="font-family:ST-Q É;font-weight:bold">Unicode</text></svg>)"sv;
    std::unique_ptr<SPDocument> document{SPDocument::createNewDocFromMem(svg)};
    ASSERT_TRUE(document);
    bool notified = false;
    sigc::scoped_connection updated = DocumentFonts::get()->connectUpdate([&] {
        notified = true;
        verify("Sans"); // the document override must be indexed before notifying observers
    });
    lister->update_font_list(document.get());
    EXPECT_TRUE(notified);
    updated.disconnect();
    ASSERT_TRUE(linear("ST-Q É"));
    // Canonical equivalence depends on the locale; match the original lookup for each form.
    verify("ST-Q É");
    verify("ST-Q E\u0301"); // preserve the old locale-aware Unicode comparison
    verify("Sans");
    lister->show_results("Sans"); // deletion/filtering must not retain stale iterators
    verify("Sans");
    verify("ST-Q missing family");
    lister->init_font_families();
    verify("Sans");
}

} // namespace
