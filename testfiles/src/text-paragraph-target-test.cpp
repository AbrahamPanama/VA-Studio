// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include <span>
#include <string>

#include <glib.h>
#include <glib/gstdio.h>

#include "actions/actions-svg-processing.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-text.h"
#include "object/sp-tspan.h"
#include "object/sp-string.h"
#include "style.h"
#include "text-editing.h"
#include "ui/text-target-utils.h"
#include "ui/text-style-units.h"
#include "ui/text-paragraph-tools.h"
#include "ui/text-hyphenation.h"
#include "ui/text-frame-tools.h"
#include "xml/repr.h"

using namespace Inkscape;
using namespace std::literals;

namespace {

class TextParagraphTargetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists()) {
            Application::create(false);
        }

        constexpr auto svg = R"(<svg xmlns="http://www.w3.org/2000/svg"
                                      xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd">
  <text id="text" style="font-family:sans-serif;font-size:20px">
    <tspan sodipodi:role="paragraph">one</tspan>
    <tspan sodipodi:role="paragraph">two</tspan>
    <tspan sodipodi:role="paragraph">three</tspan>
  </text>
  <text id="empty"/>
  <text id="hyphen" xml:lang="en_TEST"><tspan sodipodi:role="paragraph">hyphen</tspan></text>
  <text id="dropcap" x="10" y="260"
        style="font-family:sans-serif;font-size:20px;line-height:24px;inline-size:150px;white-space:pre-wrap"><tspan sodipodi:role="paragraph">Drop caps reserve room beside several wrapped lines of paragraph text.</tspan></text>
  <text id="frame" x="300" y="300" style="font-family:sans-serif;font-size:14px"><tspan sodipodi:role="paragraph">Text frames reuse shape inside and flow this sufficiently long paragraph through generated columns without changing its source characters.</tspan></text>
  <text id="plain-indent" x="10" y="100" style="font-family:sans-serif;font-size:20px">indent</text>
  <text id="editable" x="10" y="60" style="font-family:sans-serif;font-size:20px">alpha beta gamma</text>
  <text id="editable-preserve" x="10" y="80" xml:space="preserve" style="font-family:sans-serif;font-size:20px">alpha beta gamma</text>
  <text id="with-indent" x="10" y="130" style="font-family:sans-serif;font-size:20px;text-indent:24px">indent</text>
  <text id="wrapped-indent" x="10" y="160"
        style="font-family:sans-serif;font-size:20px;inline-size:120px;white-space:pre-wrap;text-indent:24px">one two three four five six seven</text>
  <text id="plain-paragraph-spacing" x="300" y="100"
        style="font-family:sans-serif;font-size:20px;line-height:20px"><tspan sodipodi:role="paragraph">one</tspan><tspan sodipodi:role="paragraph">two</tspan></text>
  <text id="with-paragraph-spacing" x="300" y="200"
        style="font-family:sans-serif;font-size:20px;line-height:20px;-inkscape-paragraph-spacing-before:8px;-inkscape-paragraph-spacing-after:12px"><tspan sodipodi:role="paragraph">one</tspan><tspan sodipodi:role="paragraph">two</tspan></text>
</svg>)"sv;
        document = SPDocument::createNewDocFromMem(svg);
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        text = cast<SPText>(document->getObjectById("text"));
        ASSERT_TRUE(text);
    }

    std::unique_ptr<SPDocument> document;
    SPText *text = nullptr;
};

TEST_F(TextParagraphTargetTest, PreservesParagraphRoleInObjectModel)
{
    auto children = text->childList(false);
    ASSERT_EQ(children.size(), 3);
    for (auto child : children) {
        auto tspan = cast<SPTSpan>(child);
        ASSERT_TRUE(tspan);
        EXPECT_EQ(tspan->role, SP_TSPAN_ROLE_PARAGRAPH);
    }
}

TEST_F(TextParagraphTargetTest, PreservesParagraphRoleForInlineSizeText)
{
    auto item = cast<SPText>(document->getObjectById("dropcap"));
    ASSERT_TRUE(item);
    auto paragraph = cast<SPTSpan>(item->firstChild());
    ASSERT_TRUE(paragraph);
    EXPECT_EQ(paragraph->role, SP_TSPAN_ROLE_PARAGRAPH);
}

TEST_F(TextParagraphTargetTest, ReturnsEachParagraphAsAStableLogicalRange)
{
    auto const character_count =
        static_cast<unsigned>(text->layout.iteratorToCharIndex(text->layout.end()));
    auto const ranges = UI::paragraphRanges(text->layout, 0, character_count);

    ASSERT_EQ(ranges.size(), 3);
    EXPECT_EQ(ranges.front().first, 0);
    EXPECT_EQ(ranges.back().last, character_count);
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        EXPECT_LT(ranges[i].first, ranges[i].last);
        if (i > 0) {
            EXPECT_EQ(ranges[i - 1].last, ranges[i].first);
        }
    }
}

TEST_F(TextParagraphTargetTest, CollapsedCaretTargetsOnlyItsParagraph)
{
    auto const all = UI::paragraphRanges(
        text->layout, 0,
        static_cast<unsigned>(text->layout.iteratorToCharIndex(text->layout.end())));
    ASSERT_EQ(all.size(), 3);

    EXPECT_EQ(UI::paragraphRanges(text->layout, all[1].first, all[1].first),
              std::vector<UI::TextLogicalRange>{all[1]});
    EXPECT_EQ(UI::paragraphRanges(text->layout, all[2].last, all[2].last),
              std::vector<UI::TextLogicalRange>{all[2]});
}

TEST_F(TextParagraphTargetTest, HalfOpenSelectionDoesNotIncludeNextParagraph)
{
    auto const all = UI::paragraphRanges(
        text->layout, 0,
        static_cast<unsigned>(text->layout.iteratorToCharIndex(text->layout.end())));
    ASSERT_EQ(all.size(), 3);

    EXPECT_EQ(UI::paragraphRanges(text->layout, all[0].first, all[1].first),
              std::vector<UI::TextLogicalRange>{all[0]});
    EXPECT_EQ(UI::paragraphRanges(text->layout, all[0].last - 1, all[1].first + 1),
              (std::vector<UI::TextLogicalRange>{all[0], all[1]}));
}

TEST_F(TextParagraphTargetTest, EmptyTextHasNoParagraphTarget)
{
    auto empty = cast<SPText>(document->getObjectById("empty"));
    ASSERT_TRUE(empty);
    EXPECT_TRUE(UI::paragraphRanges(empty->layout, 0, 0).empty());
}

TEST_F(TextParagraphTargetTest, AppliesStyleOnlyToRequestedParagraph)
{
    auto const character_count =
        static_cast<unsigned>(text->layout.iteratorToCharIndex(text->layout.end()));
    auto const ranges = UI::paragraphRanges(text->layout, 0, character_count);
    ASSERT_EQ(ranges.size(), 3);

    auto css = sp_repr_css_attr_new();
    sp_repr_css_set_property(css, "text-align", "center");
    sp_repr_css_set_property(css, "text-anchor", "middle");
    sp_repr_css_set_property(css, "line-height", "200%");
    ASSERT_TRUE(UI::applyStyleToLogicalRanges(text, {ranges[1]}, css));
    sp_repr_css_attr_unref(css);
    document->ensureUpToDate();

    auto children = text->childList(false);
    ASSERT_EQ(children.size(), 3);
    auto first = cast<SPTSpan>(children[0]);
    auto second = cast<SPTSpan>(children[1]);
    auto third = cast<SPTSpan>(children[2]);
    ASSERT_TRUE(first && second && third);
    EXPECT_EQ(first->style->text_align.computed, SP_CSS_TEXT_ALIGN_START);
    EXPECT_EQ(second->style->text_align.computed, SP_CSS_TEXT_ALIGN_CENTER);
    EXPECT_EQ(third->style->text_align.computed, SP_CSS_TEXT_ALIGN_START);
    EXPECT_NEAR(second->style->line_height.computed,
                second->style->font_size.computed * 2.0, 0.01);

    EXPECT_EQ(cast<SPString>(first->firstChild())->string, "one");
    EXPECT_EQ(cast<SPString>(second->firstChild())->string, "two");
    EXPECT_EQ(cast<SPString>(third->firstChild())->string, "three");
}

TEST_F(TextParagraphTargetTest, PreservesAuthoredLineHeightUnits)
{
    SPStyle style(document.get());
    style.font_size.read("20px");

    style.line_height.read("150%");
    auto percent = UI::lineHeightFromStyle(style);
    EXPECT_EQ(percent.unit, UI::TextLineHeightUnit::Percent);
    EXPECT_NEAR(percent.value, 150.0, 0.001);
    EXPECT_EQ(UI::serializeLineHeight(percent), "150%");

    style.line_height.read("1.4");
    auto lines = UI::lineHeightFromStyle(style);
    EXPECT_EQ(lines.unit, UI::TextLineHeightUnit::Lines);
    EXPECT_NEAR(lines.value, 1.4, 0.001);
    EXPECT_EQ(UI::serializeLineHeight(lines), "1.4");
}

TEST_F(TextParagraphTargetTest, ConvertsLineHeightWithoutChangingItsRenderedSize)
{
    UI::TextLineHeightValue percent{150.0, UI::TextLineHeightUnit::Percent};
    auto points = UI::convertLineHeight(percent, UI::TextLineHeightUnit::Pt, 20.0);
    EXPECT_NEAR(points.value, 22.5, 0.001);

    auto lines = UI::convertLineHeight(points, UI::TextLineHeightUnit::Lines, 20.0);
    EXPECT_NEAR(lines.value, 1.5, 0.001);
    EXPECT_EQ(lines.unit, UI::TextLineHeightUnit::Lines);
    EXPECT_EQ(UI::serializeLineHeight(points), "30px");
}

// A replaced range must reach the XML repr. The layout and SPString::string are
// updated in memory during the edit; if the repr text node is not reconciled, a
// later copy (which duplicates the repr) or save resurrects the pre-edit
// characters. This is the reported "old characters came back" regression and it
// had no direct coverage before.
TEST_F(TextParagraphTargetTest, ReplaceReachesReprWithoutResurrectingOldCharacters)
{
    auto editable = cast<SPText>(document->getObjectById("editable"));
    ASSERT_TRUE(editable);
    auto const &layout = editable->layout;

    // "alpha beta gamma": replace "beta" (chars 6..10) with "BETA".
    sp_te_replace(editable, layout.charIndexToIterator(6), layout.charIndexToIterator(10), "BETA");
    document->ensureUpToDate();

    auto const saved = sp_repr_save_buf(document->getReprDoc()).raw();
    // Scope the check to the edited element: the fixture holds other "beta" text.
    auto const subtree = sp_repr_write_buf(editable->getRepr(), 0, false, GQuark(0), 0, 0).raw();
    EXPECT_NE(subtree.find("BETA"), std::string::npos) << subtree;
    EXPECT_EQ(subtree.find("beta"), std::string::npos) << subtree;

    // The serialized document is what copy/paste and save/reopen observe.
    auto reopened = SPDocument::createNewDocFromMem(saved);
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto reopened_text = cast<SPText>(reopened->getObjectById("editable"));
    ASSERT_TRUE(reopened_text);
    EXPECT_EQ(sp_te_get_string_multiline(reopened_text), "alpha BETA gamma");
}

// Control for the test above: the same edit on text that already carries
// xml:space="preserve" (which Inkscape sets on text it creates) must be exact.
TEST_F(TextParagraphTargetTest, ReplaceIsExactWhenTextPreservesWhitespace)
{
    auto editable = cast<SPText>(document->getObjectById("editable-preserve"));
    ASSERT_TRUE(editable);
    auto const &layout = editable->layout;

    sp_te_replace(editable, layout.charIndexToIterator(6), layout.charIndexToIterator(10), "BETA");
    document->ensureUpToDate();

    auto const saved = sp_repr_save_buf(document->getReprDoc()).raw();
    auto reopened = SPDocument::createNewDocFromMem(saved);
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();
    auto reopened_text = cast<SPText>(reopened->getObjectById("editable-preserve"));
    ASSERT_TRUE(reopened_text);
    EXPECT_EQ(sp_te_get_string_multiline(reopened_text), "alpha BETA gamma");
}

TEST_F(TextParagraphTargetTest, TextIndentMovesOnlyTheParagraphsFirstLine)
{
    auto plain = cast<SPText>(document->getObjectById("plain-indent"));
    auto indented = cast<SPText>(document->getObjectById("with-indent"));
    ASSERT_TRUE(plain && indented);
    EXPECT_NEAR(indented->style->text_indent.computed, 24.0, 0.01);

    auto const plain_x = plain->layout.characterAnchorPoint(plain->layout.begin())[Geom::X];
    auto const indented_x = indented->layout.characterAnchorPoint(indented->layout.begin())[Geom::X];
    EXPECT_NEAR(indented_x - plain_x, 24.0, 0.01);

    auto wrapped = cast<SPText>(document->getObjectById("wrapped-indent"));
    ASSERT_TRUE(wrapped);
    auto first = wrapped->layout.begin();
    auto second_line = first;
    while (second_line != wrapped->layout.end() && wrapped->layout.lineIndex(second_line) == 0) {
        second_line.nextCharacter();
    }
    ASSERT_NE(second_line, wrapped->layout.end());
    EXPECT_NEAR(wrapped->layout.characterAnchorPoint(first)[Geom::X], 34.0, 0.01);
    EXPECT_NEAR(wrapped->layout.characterAnchorPoint(second_line)[Geom::X], 10.0, 0.01);
}

TEST_F(TextParagraphTargetTest, ParagraphSpacingAddsBlockAxisSpaceWithoutChangingText)
{
    auto plain = cast<SPText>(document->getObjectById("plain-paragraph-spacing"));
    auto spaced = cast<SPText>(document->getObjectById("with-paragraph-spacing"));
    ASSERT_TRUE(plain && spaced);
    EXPECT_NEAR(spaced->style->paragraph_spacing_before.computed, 8.0, 0.01);
    EXPECT_NEAR(spaced->style->paragraph_spacing_after.computed, 12.0, 0.01);

    auto paragraph_advance = [](SPText const &item) {
        auto const count = static_cast<unsigned>(
            item.layout.iteratorToCharIndex(item.layout.end()));
        auto const ranges = UI::paragraphRanges(item.layout, 0, count);
        EXPECT_EQ(ranges.size(), 2);
        auto first = item.layout.begin();
        auto second = item.layout.begin();
        for (unsigned i = 0; i < ranges[1].first; ++i) second.nextCharacter();
        return item.layout.characterAnchorPoint(second)[Geom::Y] -
               item.layout.characterAnchorPoint(first)[Geom::Y];
    };

    EXPECT_NEAR(paragraph_advance(*spaced) - paragraph_advance(*plain), 20.0, 0.01);
}

TEST_F(TextParagraphTargetTest, ListMarkersAreStructuredAndReversible)
{
    auto paragraphs = text->childList(false);
    ASSERT_EQ(paragraphs.size(), 3);

    EXPECT_TRUE(UI::setParagraphListMode(*document, paragraphs,
                                        UI::TextListMode::Numbered, 3));
    document->ensureUpToDate();
    for (unsigned i = 0; i < paragraphs.size(); ++i) {
        EXPECT_EQ(UI::paragraphListMode(*paragraphs[i]), UI::TextListMode::Numbered);
        EXPECT_EQ(UI::paragraphListStart(*paragraphs[i]), i + 3);
        auto marker = paragraphs[i]->getRepr()->firstChild();
        ASSERT_TRUE(marker);
        EXPECT_STREQ(marker->attribute("inkscape:list-marker"), "true");
        ASSERT_TRUE(marker->firstChild() && marker->firstChild()->content());
        EXPECT_EQ(marker->firstChild()->content(), std::to_string(i + 3) + ".\u00a0");
    }
    EXPECT_FALSE(UI::setParagraphListMode(*document, paragraphs,
                                         UI::TextListMode::Numbered, 3));

    EXPECT_TRUE(UI::setParagraphListMode(*document, paragraphs, UI::TextListMode::None));
    document->ensureUpToDate();
    for (auto paragraph : paragraphs) {
        EXPECT_EQ(UI::paragraphListMode(*paragraph), UI::TextListMode::None);
        auto first = paragraph->getRepr()->firstChild();
        ASSERT_TRUE(first && first->content());
        EXPECT_FALSE(first->attribute("inkscape:list-marker"));
    }
}

TEST_F(TextParagraphTargetTest, HyphenationPatternsReturnLogicalCharacterOffsets)
{
    auto hyphenator = UI::TextHyphenator::fromDictionary("UTF-8\nhy3phen\n");
    ASSERT_TRUE(hyphenator);
    EXPECT_EQ(hyphenator->hyphenate("hyphen", 1, 1), std::vector<unsigned>{2});
    EXPECT_TRUE(hyphenator->hyphenate("hyphen", 3, 1).empty());
}

TEST_F(TextParagraphTargetTest, HyphenationDictionaryMayOmitEncodingHeader)
{
    auto hyphenator = UI::TextHyphenator::fromDictionary("ca1fé\n");
    ASSERT_TRUE(hyphenator);
    EXPECT_EQ(hyphenator->hyphenate("café", 1, 1), std::vector<unsigned>{2});
}

TEST_F(TextParagraphTargetTest, HyphenationOverrideSupportsUtf8Directory)
{
    struct DictionaryDirectory {
        std::string parent;
        std::string directory;
        std::string dictionary;
        std::optional<std::string> previous;
        ~DictionaryDirectory()
        {
            if (previous) g_setenv("INKSCAPE_HYPHENATION_PATH", previous->c_str(), true);
            else g_unsetenv("INKSCAPE_HYPHENATION_PATH");
            if (!dictionary.empty()) g_remove(dictionary.c_str());
            if (!directory.empty()) g_rmdir(directory.c_str());
            if (!parent.empty()) g_rmdir(parent.c_str());
        }
    } temporary;
    if (auto previous = g_getenv("INKSCAPE_HYPHENATION_PATH")) temporary.previous = previous;
    auto created = g_dir_make_tmp("inkscape-hyphenation-unicode-XXXXXX", nullptr);
    ASSERT_NE(created, nullptr);
    temporary.parent = created;
    g_free(created);
    temporary.directory = temporary.parent + G_DIR_SEPARATOR_S + "Véronica测试 diccionarios";
    ASSERT_EQ(g_mkdir(temporary.directory.c_str(), 0700), 0);
    temporary.dictionary = temporary.directory + G_DIR_SEPARATOR_S + "hyph_xx_TEST.dic";
    ASSERT_TRUE(g_file_set_contents(temporary.dictionary.c_str(), "UTF-8\nca1fé\n", -1, nullptr));
    ASSERT_TRUE(g_setenv("INKSCAPE_HYPHENATION_PATH", temporary.directory.c_str(), true));
    auto hyphenator = UI::TextHyphenator::forLanguage("xx-TEST");
    ASSERT_TRUE(hyphenator);
    EXPECT_EQ(hyphenator->hyphenate("café", 1, 1), std::vector<unsigned>{2});
}

TEST_F(TextParagraphTargetTest, AutomaticHyphensAreTaggedAndReversible)
{
    GError *error = nullptr;
    auto directory = g_dir_make_tmp("inkscape-hyphenation-XXXXXX", &error);
    ASSERT_NE(directory, nullptr) << (error ? error->message : "unable to create temp directory");
    if (error) g_error_free(error);
    auto dictionary = std::string{directory} + G_DIR_SEPARATOR_S + "hyph_en_TEST.dic";
    ASSERT_TRUE(g_file_set_contents(dictionary.c_str(), "UTF-8\nhy3phen\n", -1, nullptr));
    auto previous = g_getenv("INKSCAPE_HYPHENATION_PATH");
    std::string previous_value = previous ? previous : "";
    g_setenv("INKSCAPE_HYPHENATION_PATH", directory, true);

    auto item = document->getObjectById("hyphen");
    ASSERT_TRUE(item);
    auto paragraph = item->firstChild();
    ASSERT_TRUE(paragraph);
    EXPECT_TRUE(UI::setParagraphHyphenation(*document, {paragraph}, true));
    document->ensureUpToDate();
    EXPECT_TRUE(UI::paragraphHyphenation(*paragraph));
    auto first = paragraph->getRepr()->firstChild();
    ASSERT_TRUE(first && first->content());
    EXPECT_STREQ(first->content(), "hy");
    auto marker = first->next();
    ASSERT_TRUE(marker);
    EXPECT_STREQ(marker->attribute("inkscape:auto-hyphen"), "true");
    ASSERT_TRUE(marker->firstChild() && marker->firstChild()->content());
    EXPECT_STREQ(marker->firstChild()->content(), "\xc2\xad");

    EXPECT_TRUE(UI::setParagraphHyphenation(*document, {paragraph}, false));
    document->ensureUpToDate();
    EXPECT_FALSE(UI::paragraphHyphenation(*paragraph));
    EXPECT_STREQ(paragraph->getRepr()->firstChild()->content(), "hy");
    EXPECT_STREQ(paragraph->getRepr()->lastChild()->content(), "phen");

    if (previous) g_setenv("INKSCAPE_HYPHENATION_PATH", previous_value.c_str(), true);
    else g_unsetenv("INKSCAPE_HYPHENATION_PATH");
    g_remove(dictionary.c_str());
    g_rmdir(directory);
    g_free(directory);
}

TEST_F(TextParagraphTargetTest, DropCapPreservesTextAndIndentsFollowingLines)
{
    auto item = cast<SPText>(document->getObjectById("dropcap"));
    ASSERT_TRUE(item && item->firstChild());
    auto paragraph = item->firstChild();
    auto const original_count = item->layout.iteratorToCharIndex(item->layout.end());

    EXPECT_TRUE(UI::setParagraphDropCapLines(*document, {paragraph}, 3));
    document->ensureUpToDate();
    EXPECT_EQ(UI::paragraphDropCapLines(*paragraph), 3);
    EXPECT_EQ(item->layout.iteratorToCharIndex(item->layout.end()), original_count);
    auto wrapper = paragraph->getRepr()->firstChild();
    ASSERT_TRUE(wrapper);
    EXPECT_STREQ(wrapper->attribute("inkscape:drop-cap"), "true");

    auto first = item->layout.begin();
    auto second_line = first;
    while (second_line != item->layout.end() && item->layout.lineIndex(second_line) == 0) {
        second_line.nextCharacter();
    }
    ASSERT_NE(second_line, item->layout.end());
    EXPECT_GT(item->layout.characterAnchorPoint(second_line)[Geom::X], 10.0);

    EXPECT_TRUE(UI::setParagraphDropCapLines(*document, {paragraph}, 0));
    document->ensureUpToDate();
    EXPECT_EQ(UI::paragraphDropCapLines(*paragraph), 0);
    EXPECT_EQ(item->layout.iteratorToCharIndex(item->layout.end()), original_count);
}

TEST_F(TextParagraphTargetTest, ChangingDropCapLinesPreservesTheBaseFontSize)
{
    auto item = cast<SPText>(document->getObjectById("dropcap"));
    ASSERT_TRUE(item && item->firstChild());
    auto paragraph = item->firstChild();
    auto const original_count = item->layout.iteratorToCharIndex(item->layout.end());

    ASSERT_TRUE(UI::setParagraphDropCapLines(*document, {paragraph}, 3));
    document->ensureUpToDate();
    ASSERT_TRUE(UI::setParagraphDropCapLines(*document, {paragraph}, 4));
    document->ensureUpToDate();

    EXPECT_NEAR(item->style->font_size.computed, 20.0, 0.01);
    ASSERT_TRUE(paragraph->style);
    EXPECT_NEAR(paragraph->style->font_size.computed, 20.0, 0.01);
    EXPECT_EQ(UI::paragraphDropCapLines(*paragraph), 4);
    EXPECT_EQ(item->layout.iteratorToCharIndex(item->layout.end()), original_count);
}

TEST_F(TextParagraphTargetTest, TextFrameColumnsReuseShapeInsideAndPreserveCharacters)
{
    auto item = cast<SPText>(document->getObjectById("frame"));
    ASSERT_TRUE(item);
    auto const original_count = item->layout.iteratorToCharIndex(item->layout.end());
    UI::TextFrameSettings settings;
    settings.width = 240;
    settings.height = 80;
    settings.columns = 2;
    settings.gap = 16;
    EXPECT_TRUE(UI::setTextFrameSettings(*document, {item}, settings));
    document->ensureUpToDate();

    auto actual = UI::textFrameSettings(*item);
    EXPECT_TRUE(actual.generated);
    EXPECT_NEAR(actual.width, 240, 0.01);
    EXPECT_NEAR(actual.height, 80, 0.01);
    EXPECT_EQ(actual.columns, 2);
    EXPECT_NEAR(actual.gap, 16, 0.01);
    EXPECT_TRUE(item->has_shape_inside());
    EXPECT_EQ(item->style->shape_inside.hrefs.size(), 2);
    EXPECT_EQ(item->layout.iteratorToCharIndex(item->layout.end()), original_count);
}

TEST_F(TextParagraphTargetTest, GeneratedFrameRemovesAuthoredInlineSize)
{
    auto item = cast<SPText>(document->getObjectById("dropcap"));
    ASSERT_TRUE(item);
    auto const original_count = item->layout.iteratorToCharIndex(item->layout.end());
    ASSERT_TRUE(item->has_inline_size());
    item->getRepr()->setAttribute("inline-size", "150");
    document->ensureUpToDate();
    ASSERT_TRUE(item->getRepr()->attribute("inline-size"));

    UI::TextFrameSettings settings;
    settings.width = 240;
    settings.height = 100;
    settings.columns = 2;
    settings.gap = 12;
    ASSERT_TRUE(UI::setTextFrameSettings(*document, {item}, settings));
    document->ensureUpToDate();

    EXPECT_FALSE(item->getRepr()->attribute("inline-size"));
    EXPECT_TRUE(item->has_shape_inside());
    EXPECT_FALSE(item->has_inline_size());
    EXPECT_EQ(item->layout.iteratorToCharIndex(item->layout.end()), original_count);
}

TEST_F(TextParagraphTargetTest, SingleFrameSupportsVerticalAlignment)
{
    auto item = cast<SPText>(document->getObjectById("frame"));
    ASSERT_TRUE(item);
    UI::TextFrameSettings settings;
    settings.width = 240;
    settings.height = 160;
    settings.columns = 1;
    settings.vertical_alignment = UI::TextFrameVerticalAlignment::Top;
    ASSERT_TRUE(UI::setTextFrameSettings(*document, {item}, settings));
    document->ensureUpToDate();
    auto top_y = item->layout.characterAnchorPoint(item->layout.begin())[Geom::Y];

    settings.vertical_alignment = UI::TextFrameVerticalAlignment::Middle;
    ASSERT_TRUE(UI::setTextFrameSettings(*document, {item}, settings));
    document->ensureUpToDate();
    auto middle_y = item->layout.characterAnchorPoint(item->layout.begin())[Geom::Y];
    EXPECT_GT(middle_y, top_y);
}

TEST_F(TextParagraphTargetTest, StructuredParagraphFeaturesSurviveSaveAndReopen)
{
    auto paragraphs = text->childList(false);
    ASSERT_EQ(paragraphs.size(), 3);
    auto frame = cast<SPText>(document->getObjectById("frame"));
    ASSERT_TRUE(frame);

    auto const frame_count = frame->layout.iteratorToCharIndex(frame->layout.end());

    ASSERT_TRUE(UI::setParagraphListMode(*document, {paragraphs[0]},
                                        UI::TextListMode::Numbered, 4));
    ASSERT_TRUE(UI::setParagraphDropCapLines(*document, {paragraphs[1]}, 3));

    UI::TextFrameSettings settings;
    settings.width = 260;
    settings.height = 120;
    settings.columns = 2;
    settings.gap = 18;
    ASSERT_TRUE(UI::setTextFrameSettings(*document, {frame}, settings));
    document->ensureUpToDate();
    auto const structured_text_count = text->layout.iteratorToCharIndex(text->layout.end());

    std::string serialized = sp_repr_save_buf(document->getReprDoc());
    auto reopened = SPDocument::createNewDocFromMem(serialized);
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();

    auto reopened_text = cast<SPText>(reopened->getObjectById("text"));
    auto reopened_frame = cast<SPText>(reopened->getObjectById("frame"));
    ASSERT_TRUE(reopened_text && reopened_frame);
    auto reopened_paragraphs = reopened_text->childList(false);
    ASSERT_EQ(reopened_paragraphs.size(), 3);

    EXPECT_EQ(UI::paragraphListMode(*reopened_paragraphs[0]), UI::TextListMode::Numbered);
    EXPECT_EQ(UI::paragraphListStart(*reopened_paragraphs[0]), 4);
    EXPECT_EQ(UI::paragraphDropCapLines(*reopened_paragraphs[1]), 3);
    EXPECT_EQ(reopened_text->layout.iteratorToCharIndex(reopened_text->layout.end()),
              structured_text_count);

    auto actual = UI::textFrameSettings(*reopened_frame);
    EXPECT_TRUE(actual.generated);
    EXPECT_NEAR(actual.width, 260, 0.01);
    EXPECT_NEAR(actual.height, 120, 0.01);
    EXPECT_EQ(actual.columns, 2);
    EXPECT_NEAR(actual.gap, 18, 0.01);
    EXPECT_EQ(reopened_frame->style->shape_inside.hrefs.size(), 2);
    EXPECT_EQ(reopened_frame->layout.iteratorToCharIndex(reopened_frame->layout.end()), frame_count);
}

TEST_F(TextParagraphTargetTest, Svg11FallbackPreservesSemanticParagraphFeatures)
{
    constexpr auto svg = R"(<svg xmlns="http://www.w3.org/2000/svg"
                                   xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd">
  <text id="fallback" x="10" y="30"
        style="font-family:sans-serif;font-size:20px;line-height:24px;inline-size:160px;white-space:pre-wrap">
    <tspan sodipodi:role="paragraph">First paragraph wraps across lines.</tspan>
    <tspan sodipodi:role="paragraph">Second paragraph also wraps.</tspan>
  </text>
</svg>)"sv;
    auto original = SPDocument::createNewDocFromMem(svg);
    ASSERT_TRUE(original);
    original->ensureUpToDate();
    auto original_text = cast<SPText>(original->getObjectById("fallback"));
    ASSERT_TRUE(original_text);
    auto paragraphs = original_text->childList(false);
    ASSERT_EQ(paragraphs.size(), 2);
    ASSERT_TRUE(UI::setParagraphListMode(*original, paragraphs,
                                        UI::TextListMode::Numbered, 4));
    ASSERT_TRUE(UI::setParagraphDropCapLines(*original, {paragraphs.front()}, 3));
    original->ensureUpToDate();
    auto const character_count = original_text->layout.iteratorToCharIndex(original_text->layout.end());
    auto const original_bounds = original_text->geometricBounds();
    ASSERT_TRUE(original_bounds);

    std::string serialized = sp_repr_save_buf(original->getReprDoc());
    auto export_doc = SPDocument::createNewDocFromMem(serialized);
    ASSERT_TRUE(export_doc);
    insert_text_fallback(export_doc->getReprRoot(), original.get());
    std::string fallback = sp_repr_save_buf(export_doc->getReprDoc());
    auto reopened = SPDocument::createNewDocFromMem(fallback);
    ASSERT_TRUE(reopened);
    reopened->ensureUpToDate();

    auto reopened_text = cast<SPText>(reopened->getObjectById("fallback"));
    ASSERT_TRUE(reopened_text);
    auto reopened_paragraphs = reopened_text->childList(false);
    ASSERT_EQ(reopened_paragraphs.size(), 2);
    for (auto paragraph : reopened_paragraphs) {
        auto tspan = cast<SPTSpan>(paragraph);
        ASSERT_TRUE(tspan);
        EXPECT_EQ(tspan->role, SP_TSPAN_ROLE_PARAGRAPH);
        EXPECT_EQ(UI::paragraphListMode(*paragraph), UI::TextListMode::Numbered);
    }
    EXPECT_EQ(UI::paragraphListStart(*reopened_paragraphs[0]), 4);
    EXPECT_EQ(UI::paragraphListStart(*reopened_paragraphs[1]), 5);
    EXPECT_EQ(UI::paragraphDropCapLines(*reopened_paragraphs[0]), 3);
    EXPECT_EQ(reopened_text->layout.iteratorToCharIndex(reopened_text->layout.end()), character_count);
    auto const reopened_bounds = reopened_text->geometricBounds();
    ASSERT_TRUE(reopened_bounds);
    EXPECT_NEAR(reopened_bounds->width(), original_bounds->width(), 0.5);
    EXPECT_NEAR(reopened_bounds->height(), original_bounds->height(), 0.5);
}

} // namespace


namespace {
class TextShapeInsideNoneTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!Application::exists()) Application::create(false);
    }

    static std::unique_ptr<SPDocument> make_document(std::string const &probe_style,
                                                    std::string const &common_style = {})
    {
        std::string const content =
            R"(<tspan dx="2" dy="1" rotate="7">Editable</tspan>)";
        std::string const xml =
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="240">
            <defs><rect id="frame" x="100" y="100" width="360" height="120"/></defs>
            <style>.shield {shape-inside:url(#frame)!important}</style>
            <g style="font-family:sans-serif;font-size:16px;line-height:normal;)" + common_style + R"(">
            <text id="control" x="11" y="60" dx="3" dy="2">)" + content + R"(</text>
            <text id="probe" class="shield" x="11" y="60" dx="3" dy="2" style=")" +
            probe_style + R"(">)" + content + "</text></g></svg>";
        auto document = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
        if (document) document->ensureUpToDate();
        return document;
    }

    static void same_layout(SPText &expected, SPText &actual)
    {
        ASSERT_TRUE(expected.layout.outputExists()); // Not an empty/empty oracle.
        ASSERT_TRUE(actual.layout.outputExists());
        auto lhs = expected.geometricBounds();
        auto rhs = actual.geometricBounds();
        ASSERT_TRUE(lhs);
        ASSERT_TRUE(rhs);
        EXPECT_EQ(lhs->left(), rhs->left());
        EXPECT_EQ(lhs->top(), rhs->top());
        EXPECT_EQ(lhs->right(), rhs->right());
        EXPECT_EQ(lhs->bottom(), rhs->bottom());
        EXPECT_EQ(expected.layout.iteratorToCharIndex(expected.layout.end()),
                  actual.layout.iteratorToCharIndex(actual.layout.end()));
        auto baseline = expected.layout.baselineAnchorPoint();
        auto actual_baseline = actual.layout.baselineAnchorPoint();
        ASSERT_TRUE(baseline);
        ASSERT_TRUE(actual_baseline);
        EXPECT_EQ((*baseline)[0], (*actual_baseline)[0]);
        EXPECT_EQ((*baseline)[1], (*actual_baseline)[1]);
    }
};

TEST_F(TextShapeInsideNoneTest, ImportantNonePreservesOrdinaryPositioningAndCssShield)
{
    for (auto const *keyword : {"none", "NONE"}) {
        SCOPED_TRACE(keyword);
        auto doc = make_document(std::string("shape-inside:") + keyword + " !important");
        ASSERT_TRUE(doc);
        auto control = cast<SPText>(doc->getObjectById("control"));
        auto probe = cast<SPText>(doc->getObjectById("probe"));
        ASSERT_TRUE(control);
        ASSERT_TRUE(probe);
        EXPECT_FALSE(control->has_shape_inside());
        EXPECT_FALSE(probe->has_shape_inside());
        EXPECT_EQ(probe->layout.wrap_mode, Text::Layout::WRAP_NONE);
        EXPECT_TRUE(probe->style->shape_inside.set);
        EXPECT_TRUE(probe->style->shape_inside.important);
        EXPECT_FALSE(probe->style->shape_inside.inherit);
        EXPECT_TRUE(probe->style->shape_inside.hrefs.empty());
        EXPECT_EQ(probe->style->shape_inside.write(SP_STYLE_FLAG_IFSET),
                  std::string("shape-inside:") + keyword + " !important;");
        ASSERT_NO_FATAL_FAILURE(same_layout(*control, *probe));

        // Removing the shield must expose the real stylesheet URL, not drop
        // the property or silently convert all empty/unknown values to none.
        probe->getRepr()->setAttribute("style", nullptr);
        doc->ensureUpToDate();
        EXPECT_TRUE(probe->has_shape_inside());
        EXPECT_EQ(probe->layout.wrap_mode, Text::Layout::WRAP_SHAPE_INSIDE);
        EXPECT_EQ(probe->style->shape_inside.hrefs.size(), 1u);
    }
}

TEST_F(TextShapeInsideNoneTest, ExplicitNoneAllowsInlineSizeAndWhitespacePrecedence)
{
    for (auto const *common : {"inline-size:180px;white-space:pre-wrap",
                              "white-space:pre"}) {
        SCOPED_TRACE(common);
        auto doc = make_document("shape-inside:none !important", common);
        ASSERT_TRUE(doc);
        auto control = cast<SPText>(doc->getObjectById("control"));
        auto probe = cast<SPText>(doc->getObjectById("probe"));
        ASSERT_TRUE(control);
        ASSERT_TRUE(probe);
        control->getRepr()->setAttribute("style", common);
        auto explicit_style = std::string(common) + ";shape-inside:none !important";
        probe->getRepr()->setAttribute("style", explicit_style.c_str());
        doc->ensureUpToDate();
        EXPECT_FALSE(probe->has_shape_inside());
        EXPECT_EQ(control->layout.wrap_mode, std::string(common).starts_with("inline-size:")
                  ? Text::Layout::WRAP_INLINE_SIZE : Text::Layout::WRAP_WHITE_SPACE);
        EXPECT_EQ(probe->layout.wrap_mode, control->layout.wrap_mode);
        ASSERT_NO_FATAL_FAILURE(same_layout(*control, *probe));
    }
}

TEST_F(TextShapeInsideNoneTest, MissingAndResolvedUrlsAreNotClassifiedAsNone)
{
    auto doc = make_document("shape-inside:none !important");
    ASSERT_TRUE(doc);
    auto probe = cast<SPText>(doc->getObjectById("probe"));
    ASSERT_TRUE(probe);
    for (auto const *value : {"url(#missing)", "url(#frame)", "url(#missing) url(#frame)"}) {
        SCOPED_TRACE(value);
        auto declaration = std::string("shape-inside:") + value + " !important";
        probe->getRepr()->setAttribute("style", declaration.c_str());
        doc->ensureUpToDate();
        EXPECT_TRUE(probe->has_shape_inside());
        EXPECT_EQ(probe->layout.wrap_mode, Text::Layout::WRAP_SHAPE_INSIDE);
        EXPECT_TRUE(probe->style->shape_inside.important);
        EXPECT_STREQ(probe->style->shape_inside.value(), value);
    }
    probe->getRepr()->setAttribute("style", "shape-inside:none !important");
    doc->ensureUpToDate();
    EXPECT_FALSE(probe->has_shape_inside());
    EXPECT_EQ(probe->layout.wrap_mode, Text::Layout::WRAP_NONE);
    EXPECT_TRUE(probe->style->shape_inside.hrefs.empty());
    auto control = cast<SPText>(doc->getObjectById("control"));
    ASSERT_TRUE(control);
    ASSERT_NO_FATAL_FAILURE(same_layout(*control, *probe));
}

TEST_F(TextShapeInsideNoneTest, ParentShapeDoesNotImplicitlyFlowUnspecifiedOrNeutralChild)
{
    auto doc = make_document("shape-inside:none !important", "shape-inside:url(#frame)");
    ASSERT_TRUE(doc);
    auto control = cast<SPText>(doc->getObjectById("control"));
    auto probe = cast<SPText>(doc->getObjectById("probe"));
    ASSERT_TRUE(control);
    ASSERT_TRUE(probe);
    EXPECT_FALSE(control->style->shape_inside.set); // Non-inherited property.
    EXPECT_FALSE(control->has_shape_inside());
    EXPECT_FALSE(probe->has_shape_inside());
    EXPECT_EQ(control->layout.wrap_mode, Text::Layout::WRAP_NONE);
    EXPECT_EQ(probe->layout.wrap_mode, Text::Layout::WRAP_NONE);
    ASSERT_NO_FATAL_FAILURE(same_layout(*control, *probe));
}
} // namespace
