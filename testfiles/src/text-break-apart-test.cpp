// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BRK-1: Break Apart like CorelDRAW (internal note OWNER_REQUESTS).
 * Outcome tests: pieces per level, glyph positions preserved, styles and
 * colours preserved, paths by their own rule, other objects untouched and
 * reported, one Undo step, no-op without a history entry.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <2geom/point.h>

#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "libnrtype/Layout-TNG.h"
#include "object/object-set.h"
#include "object/sp-item.h"
#include "object/sp-path.h"
#include "object/sp-root.h"
#include "object/sp-text.h"
#include "object/sp-tspan.h"
#include "object/sp-use.h"
#include "text-break-apart.h"
#include "text-editing.h"
#include "util-string/context-string.h"

namespace BA = Inkscape::TextBreakApart;

namespace {

constexpr char const *svg_open =
    R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" )"
    R"(xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" width="400" height="300">)";

class TextBreakApartTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    }

    static std::unique_ptr<SPDocument> parse(std::string const &svg)
    {
        auto document = SPDocument::createNewDocFromMem(svg);
        if (document) {
            document->ensureUpToDate();
            // A second, style-level update: tspan em offsets (dy="1.2em", as
            // CorelDRAW writes line spacing) are recomputed from the resolved
            // font size, as the application does after its first redraw.
            document->getRoot()->requestDisplayUpdate(SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG);
            document->ensureUpToDate();
        }
        return document;
    }

    static SPItem *item(SPDocument &document, char const *id) { return cast<SPItem>(document.getObjectById(id)); }

    /// Document-space anchor of every non-space character of every text in the
    /// document, with the character and its computed fill; sorted.
    struct Glyph {
        double x, y;
        gunichar character;
        std::string fill;
        double stroke_width = 0;
        int weight = 0;
        bool operator<(Glyph const &o) const
        {
            if (std::abs(x - o.x) > 1e-3) return x < o.x;
            if (std::abs(y - o.y) > 1e-3) return y < o.y;
            return character < o.character;
        }
    };
    static std::vector<Glyph> glyphs(SPDocument &document)
    {
        document.ensureUpToDate();
        std::vector<Glyph> result;
        std::vector<SPObject *> texts;
        collect_texts(document.getRoot(), texts);
        for (auto *object : texts) {
            auto *text = cast<SPText>(object);
            auto const &layout = text->layout;
            auto const to_doc = text->i2doc_affine();
            for (auto it = layout.begin(); it != layout.end(); it.nextCharacter()) {
                if (!it.hasGlyph() || layout.isWhitespace(it)) continue;
                SPObject *source = nullptr;
                layout.getSourceOfCharacter(it, &source);
                // The glyph's drawn origin (independent of the implementation's
                // pen/baseline arithmetic): includes y/dy offsets and baseline shift.
                auto const point = layout.glyphs()[it.glyphIndex()].transform(layout).translation() * to_doc;
                auto const *run = source && source->parent ? source->parent->style : nullptr;
                result.push_back({point[Geom::X], point[Geom::Y], layout.characterAt(it),
                                  run ? run->fill.get_value() : "", run ? run->stroke_width.computed : 0.0,
                                  run ? static_cast<int>(run->font_weight.computed) : 0});
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }
    static void collect_texts(SPObject *object, std::vector<SPObject *> &out)
    {
        if (is<SPUse>(object)) return; // a clone's shadow copy is not a document text
        if (is<SPText>(object)) out.push_back(object);
        for (auto &child : object->children) collect_texts(&child, out);
    }
    static void expect_same_glyphs(std::vector<Glyph> const &before, std::vector<Glyph> const &after)
    {
        ASSERT_EQ(before.size(), after.size());
        for (std::size_t i = 0; i < before.size(); ++i) {
            EXPECT_NEAR(before[i].x, after[i].x, 1e-3) << "glyph " << i;
            EXPECT_NEAR(before[i].y, after[i].y, 1e-3) << "glyph " << i;
            EXPECT_EQ(before[i].character, after[i].character) << "glyph " << i;
            EXPECT_EQ(before[i].fill, after[i].fill) << "glyph " << i;
            EXPECT_NEAR(before[i].stroke_width, after[i].stroke_width, 1e-6) << "glyph " << i;
            EXPECT_EQ(before[i].weight, after[i].weight) << "glyph " << i;
        }
    }
    static std::size_t text_count(SPDocument &document)
    {
        std::vector<SPObject *> texts;
        collect_texts(document.getRoot(), texts);
        return texts.size();
    }
};

TEST_F(TextBreakApartTest, ParagraphGivesOneTextPerLineThenWordsThenLetters)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="20" y="40" style="font-family:sans-serif;font-size:20px;fill:#1b1918" xml:space="preserve">)svg"
        R"svg(<tspan sodipodi:role="line" x="20" y="40">Mi Primera</tspan>)svg"
        R"svg(<tspan sodipodi:role="line" x="20" y="65">Comunión hoy</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    auto const before = glyphs(*document);
    ASSERT_EQ(before.size(), 20u);
    auto *text = cast<SPText>(item(*document, "t"));
    ASSERT_EQ(BA::level_of(text), BA::Level::Lines);

    Inkscape::ObjectSet set(document.get());
    set.set(text);
    auto outcome = BA::break_apart(set);
    EXPECT_EQ(outcome.texts, 1u);
    EXPECT_EQ(outcome.pieces.size(), 2u);
    EXPECT_EQ(text_count(*document), 2u);
    { SCOPED_TRACE("check 1"); expect_same_glyphs(before, glyphs(*document)); }
    EXPECT_EQ(sp_te_get_string_multiline(outcome.pieces[0]).raw(), "Mi Primera");
    EXPECT_STREQ(outcome.pieces[0]->getRepr()->attribute("id"), "t") << "the first piece keeps the id";
    EXPECT_EQ(sp_te_get_string_multiline(outcome.pieces[1]).raw(), "Comunión hoy");
    EXPECT_EQ(set.items_vector().size(), 2u) << "the pieces are selected";

    // Second Ctrl+K: each line into its words.
    outcome = BA::break_apart(set);
    EXPECT_EQ(outcome.texts, 2u);
    EXPECT_EQ(text_count(*document), 4u);
    { SCOPED_TRACE("check 2"); expect_same_glyphs(before, glyphs(*document)); }

    // Third: each word into letters (no space pieces).
    outcome = BA::break_apart(set);
    EXPECT_EQ(outcome.texts, 4u);
    EXPECT_EQ(text_count(*document), 20u);
    { SCOPED_TRACE("check 3"); expect_same_glyphs(before, glyphs(*document)); }

    // Three Undo steps restore the original text; Redo returns to letters.
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_EQ(text_count(*document), 1u);
    auto *restored = cast<SPText>(item(*document, "t"));
    ASSERT_TRUE(restored) << "#t is the original text again";
    EXPECT_EQ(sp_te_get_string_multiline(restored).raw(), "Mi Primera\nComunión hoy");
    { SCOPED_TRACE("check 4"); expect_same_glyphs(before, glyphs(*document)); }
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(text_count(*document), 20u);
    { SCOPED_TRACE("check redo"); expect_same_glyphs(before, glyphs(*document)); }
    auto *first_letter = cast<SPText>(item(*document, "t"));
    ASSERT_TRUE(first_letter) << "after Redo, #t is the first letter piece";
    EXPECT_EQ(sp_te_get_string_multiline(first_letter).raw(), "M");

    // Fourth: single letters, nothing to break, no history entry.
    Inkscape::DocumentUndo::clearUndo(document.get());
    std::vector<SPItem *> letters;
    for (auto &child : document->getRoot()->children) {
        if (auto *piece = cast<SPText>(&child)) letters.push_back(piece);
    }
    set.setList(letters);
    outcome = BA::break_apart(set);
    EXPECT_EQ(outcome.texts, 0u);
    EXPECT_EQ(outcome.skipped, 20u);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get())) << "no Undo entry for a no-op";
    { SCOPED_TRACE("check 5"); expect_same_glyphs(before, glyphs(*document)); }
}

TEST_F(TextBreakApartTest, CentredCorelTextKeepsLinePositionsAndRunColours)
{
    // CorelDRAW import: lines positioned with x and dy, presentation
    // attributes, text-anchor middle, a coloured run inside a line.
    auto document = parse(std::string{svg_open} +
        R"svg(<text x="100" y="60" xml:space="preserve" id="t">)svg"
        R"svg(<tspan x="200" text-anchor="middle" font-family="sans-serif" font-size="24" fill="#1b1918" id="l1">María <tspan fill="#c00000">Victoria</tspan></tspan>)svg"
        R"svg(<tspan x="200" dy="1.2em" text-anchor="middle" font-family="sans-serif" font-size="24" fill="#1b1918" id="l2">Castillo <tspan font-weight="bold" stroke="#000000" stroke-width="1.5">Barría</tspan></tspan>)svg"
        R"svg(<tspan x="200" dy="1.2em" text-anchor="middle" font-family="sans-serif" font-size="24" fill="#1b1918" id="l3" />)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    auto const before = glyphs(*document);
    ASSERT_FALSE(before.empty());
    ASSERT_TRUE(std::any_of(before.begin(), before.end(), [](Glyph const &g) { return g.fill == "#c00000"; }));
    // The fixture really is laid out on two baselines (dy="1.2em" applied).
    std::vector<double> baselines;
    for (auto const &g : before) {
        if (std::none_of(baselines.begin(), baselines.end(), [&](double y) { return std::abs(y - g.y) < 1e-3; })) baselines.push_back(g.y);
    }
    ASSERT_EQ(baselines.size(), 2u);

    Inkscape::ObjectSet set(document.get());
    set.set(item(*document, "t"));
    auto outcome = BA::break_apart(set);
    EXPECT_EQ(outcome.texts, 1u);
    expect_same_glyphs(before, glyphs(*document));

    outcome = BA::break_apart(set); // words
    expect_same_glyphs(before, glyphs(*document));
    outcome = BA::break_apart(set); // letters
    expect_same_glyphs(before, glyphs(*document));
    EXPECT_EQ(text_count(*document), before.size());
}

TEST_F(TextBreakApartTest, ManualKernsAndTransformsArePreserved)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<g transform="translate(10,5) rotate(15)">)svg"
        R"svg(<text id="t" x="20" y="40" transform="scale(1.5)" style="font-family:sans-serif;font-size:16px;letter-spacing:3px" xml:space="preserve">)svg"
        R"svg(<tspan x="20 44 61 90" y="40 42 40 46">kern</tspan></text></g></svg>)svg");
    ASSERT_TRUE(document);
    auto const before = glyphs(*document);
    ASSERT_EQ(before.size(), 4u);
    Inkscape::ObjectSet set(document.get());
    set.set(item(*document, "t"));
    auto const outcome = BA::break_apart(set); // one word: letters
    EXPECT_EQ(outcome.texts, 1u);
    EXPECT_EQ(text_count(*document), 4u);
    expect_same_glyphs(before, glyphs(*document));
}

TEST_F(TextBreakApartTest, MixedSelectionBreaksEachByKindInOneUndoStep)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="10" y="30" style="font-family:sans-serif;font-size:20px" xml:space="preserve">dos palabras</text>)svg"
        R"svg(<path id="p" d="M 10 100 h 50 v 20 h -50 z M 100 100 h 50 v 20 h -50 z" style="fill:#0000ff"/>)svg"
        R"svg(<path id="single" d="M 200 100 h 50 v 20 h -50 z" style="fill:#00ff00"/>)svg"
        R"svg(<image id="bmp" x="10" y="200" width="20" height="20" preserveAspectRatio="none" )svg"
        R"svg(xlink:href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLx9QAAAABJRU5ErkJggg=="/>)svg"
        R"svg(<g id="grp"><rect id="r" x="0" y="0" width="5" height="5"/></g>)svg"
        R"svg(</svg>)svg");
    ASSERT_TRUE(document);
    auto const before = glyphs(*document);
    Inkscape::ObjectSet set(document.get());
    set.setList(std::vector<SPItem *>{item(*document, "t"), item(*document, "p"), item(*document, "single"),
                                      item(*document, "bmp"), item(*document, "grp")});
    Inkscape::DocumentUndo::clearUndo(document.get());
    auto const outcome = BA::break_apart(set);
    EXPECT_EQ(outcome.texts, 1u);
    EXPECT_EQ(outcome.paths, 1u);
    EXPECT_EQ(outcome.skipped, 2u) << "the image and the group";
    EXPECT_EQ(text_count(*document), 2u);
    expect_same_glyphs(before, glyphs(*document));
    EXPECT_TRUE(item(*document, "bmp") && item(*document, "grp") && item(*document, "single"));
    // The two-subpath path became two paths with the fill kept.
    std::size_t blue_paths = 0;
    for (auto &child : document->getRoot()->children) {
        if (auto *path = cast<SPPath>(&child)) {
            if (path->style->fill.get_value() == "#0000ff") ++blue_paths;
        }
    }
    EXPECT_EQ(blue_paths, 2u);
    EXPECT_EQ(set.items_vector().size(), 2u + 2u + 3u) << "pieces plus the untouched objects stay selected";

    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get())) << "one Undo step";
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get())) << "only one";
    EXPECT_EQ(text_count(*document), 1u);
    EXPECT_TRUE(item(*document, "p"));
    auto *restored = cast<SPText>(item(*document, "t"));
    ASSERT_TRUE(restored) << "#t is the original text again";
    EXPECT_EQ(sp_te_get_string_multiline(restored).raw(), "dos palabras");
    expect_same_glyphs(before, glyphs(*document));

    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(text_count(*document), 2u);
    auto *first_word = cast<SPText>(item(*document, "t"));
    ASSERT_TRUE(first_word) << "after Redo, #t is the first word";
    EXPECT_EQ(sp_te_get_string_multiline(first_word).raw(), "dos");
    blue_paths = 0;
    for (auto &child : document->getRoot()->children) {
        if (auto *path = cast<SPPath>(&child)) {
            if (path->style->fill.get_value() == "#0000ff") ++blue_paths;
        }
    }
    EXPECT_EQ(blue_paths, 2u) << "the path is broken again";
    EXPECT_TRUE(item(*document, "bmp") && item(*document, "grp") && item(*document, "single"));
    expect_same_glyphs(before, glyphs(*document));
}

TEST_F(TextBreakApartTest, MarksStayWithTheirBaseWhenAPieceNeedsPositions)
{
    // "e" + U+0301 (combining acute) is one cluster, as a ligature is. The
    // first line has two chunks on one baseline, so its piece needs positions;
    // the second chunk must stay at x=140 with the mark on its base, and the
    // mark, coloured apart from its base, keeps its colour.
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" style="font-family:sans-serif;font-size:20px" xml:space="preserve">)svg"
        R"svg(<tspan x="20" y="40">ab</tspan><tspan x="140" y="40">ce<tspan style="fill:#ff0000">)svg" "\xcc\x81"
        R"svg(</tspan>f</tspan>)svg"
        R"svg(<tspan x="20" y="80">dos</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    auto const before = glyphs(*document);
    ASSERT_TRUE(std::any_of(before.begin(), before.end(),
                            [](Glyph const &g) { return g.character == 0x301 && g.fill == "#ff0000"; }));
    auto *text = cast<SPText>(item(*document, "t"));
    ASSERT_EQ(BA::level_of(text), BA::Level::Lines);

    Inkscape::ObjectSet set(document.get());
    set.set(text);
    auto outcome = BA::break_apart(set);
    EXPECT_EQ(outcome.texts, 1u);
    ASSERT_EQ(outcome.pieces.size(), 2u);
    EXPECT_EQ(sp_te_get_string_multiline(outcome.pieces[0]).raw(), "abce\xcc\x81" "f");
    { SCOPED_TRACE("lines"); expect_same_glyphs(before, glyphs(*document)); }

    // Down to letters: the mark is never a piece of its own.
    for (int level = 0; level < 2; ++level) {
        outcome = BA::break_apart(set);
        SCOPED_TRACE(level);
        expect_same_glyphs(before, glyphs(*document));
    }
    std::vector<std::string> letters;
    for (auto &child : document->getRoot()->children) {
        if (auto *piece = cast<SPText>(&child)) letters.push_back(sp_te_get_string_multiline(piece).raw());
    }
    EXPECT_NE(std::find(letters.begin(), letters.end(), "e\xcc\x81"), letters.end());
    EXPECT_EQ(std::find(letters.begin(), letters.end(), "\xcc\x81"), letters.end());
}

TEST_F(TextBreakApartTest, LigaturesStayWholeWhenAPieceNeedsPositions)
{
    // Needs a font that forms "ffi"/"fi" ligatures; skipped where none does.
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" style="font-family:'Hoefler Text',Palatino,Calibri,Cambria,'Times New Roman',serif;font-size:24px" xml:space="preserve">)svg"
        R"svg(<tspan x="20" y="40">office</tspan><tspan x="200" y="40">fifty</tspan>)svg"
        R"svg(<tspan x="20" y="80">official</tspan>)svg"
        R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    auto *text = cast<SPText>(item(*document, "t"));
    ASSERT_TRUE(text);
    int shared = 0, previous = -1;
    for (auto it = text->layout.begin(); it != text->layout.end(); it.nextCharacter()) {
        if (!it.hasGlyph()) { previous = -1; continue; }
        if (it.glyphIndex() == previous) ++shared;
        previous = it.glyphIndex();
    }
    if (shared == 0) GTEST_SKIP() << "no ligature forms with the available fonts";

    auto const before = glyphs(*document);
    Inkscape::ObjectSet set(document.get());
    set.set(text);
    for (int level = 0; level < 3; ++level) {
        auto const outcome = BA::break_apart(set);
        SCOPED_TRACE(level);
        EXPECT_EQ(outcome.unmatched, 0u);
        expect_same_glyphs(before, glyphs(*document));
    }
}

TEST_F(TextBreakApartTest, BrokenAndRefusedTextsInOneSelectionMakeOneUndoStep)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="a" x="20" y="30" style="font-family:sans-serif;font-size:20px" xml:space="preserve">dos palabras</text>)svg"
        R"svg(<text id="b" x="20" y="90" dy="0 0 -9" style="font-family:sans-serif;font-size:20px" xml:space="preserve">)svg"
        R"svg(ae)svg" "\xcc\x81" R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    auto const before = glyphs(*document);
    auto *b = item(*document, "b");
    Inkscape::ObjectSet set(document.get());
    set.setList(std::vector<SPItem *>{item(*document, "a"), b}); // a commits before b's trial
    Inkscape::DocumentUndo::clearUndo(document.get());
    auto const outcome = BA::break_apart(set);
    EXPECT_EQ(outcome.texts, 1u);
    EXPECT_EQ(outcome.skipped, 1u);
    EXPECT_EQ(outcome.unmatched, 1u);
    EXPECT_EQ(text_count(*document), 3u);
    EXPECT_EQ(item(*document, "b"), b) << "the refused text is the same object";
    expect_same_glyphs(before, glyphs(*document));

    ASSERT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get())) << "one Undo step";
    EXPECT_EQ(text_count(*document), 2u);
    EXPECT_EQ(sp_te_get_string_multiline(cast<SPText>(item(*document, "a"))).raw(), "dos palabras");
    expect_same_glyphs(before, glyphs(*document));
    ASSERT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_EQ(text_count(*document), 3u);
    EXPECT_EQ(sp_te_get_string_multiline(cast<SPText>(item(*document, "a"))).raw(), "dos");
    expect_same_glyphs(before, glyphs(*document));
}

TEST_F(TextBreakApartTest, TextWhosePiecesCannotMatchIsLeftUnchanged)
{
    // A dy on the mark alone moves it off its base in the original drawing;
    // a piece keeps the cluster together and cannot reproduce that, so the
    // text is left as it is: no pieces, no history entry, reported.
    auto document = parse(std::string{svg_open} +
        R"svg(<text id="t" x="20" y="60" dy="0 0 -9" style="font-family:sans-serif;font-size:20px" xml:space="preserve">)svg"
        R"svg(ae)svg" "\xcc\x81" R"svg(</text></svg>)svg");
    ASSERT_TRUE(document);
    auto const before = glyphs(*document);
    auto *text = cast<SPText>(item(*document, "t"));
    ASSERT_EQ(BA::level_of(text), BA::Level::Letters);
    Inkscape::ObjectSet set(document.get());
    set.set(text);
    Inkscape::DocumentUndo::clearUndo(document.get());
    auto const outcome = BA::break_apart(set);
    EXPECT_EQ(outcome.texts, 0u);
    EXPECT_EQ(outcome.skipped, 1u);
    EXPECT_EQ(outcome.unmatched, 1u);
    EXPECT_EQ(text_count(*document), 1u);
    EXPECT_EQ(item(*document, "t"), text) << "the same object, untouched";
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get())) << "no history entry";
    expect_same_glyphs(before, glyphs(*document));
}

TEST_F(TextBreakApartTest, TwoPathsAndBlendModeKeepEverythingSelectedAndStyled)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<path id="p1" d="M 10 10 h 20 v 20 h -20 z M 40 10 h 20 v 20 h -20 z" style="fill:#ff0000"/>)svg"
        R"svg(<path id="p2" d="M 10 50 h 20 v 20 h -20 z M 40 50 h 20 v 20 h -20 z" style="fill:#00ff00"/>)svg"
        R"svg(<text id="mb" x="10" y="120" style="font-size:20px;mix-blend-mode:multiply" xml:space="preserve">dos palabras</text>)svg"
        R"svg(</svg>)svg");
    ASSERT_TRUE(document);
    Inkscape::ObjectSet set(document.get());
    set.setList(std::vector<SPItem *>{item(*document, "p1"), item(*document, "p2"), item(*document, "mb")});
    auto const outcome = BA::break_apart(set);
    EXPECT_EQ(outcome.paths, 2u);
    EXPECT_EQ(outcome.texts, 1u);
    EXPECT_EQ(outcome.pieces.size(), 2u + 2u + 2u) << "both paths' pieces and the two words";
    EXPECT_EQ(set.items_vector().size(), 6u) << "all pieces stay selected";
    std::size_t multiply = 0;
    for (auto &child : document->getRoot()->children) {
        if (auto *piece = cast<SPText>(&child)) {
            if (piece->style->mix_blend_mode.set && piece->style->mix_blend_mode.value == SP_CSS_BLEND_MULTIPLY) ++multiply;
        }
    }
    EXPECT_EQ(multiply, 2u) << "blend mode kept on each piece";
}

TEST_F(TextBreakApartTest, UnsupportedTextsAreLeftAlone)
{
    auto document = parse(std::string{svg_open} +
        R"svg(<defs><path id="curve" d="M 10 100 C 50 20 150 20 200 100"/></defs>)svg"
        R"svg(<text id="onpath" style="font-size:16px"><textPath xlink:href="#curve">on a path</textPath></text>)svg"
        R"svg(<text id="vertical" x="300" y="20" style="font-size:16px;writing-mode:tb">vertical text</text>)svg"
        R"svg(<text id="one" x="10" y="250" style="font-size:16px">A</text>)svg"
        R"svg(<text id="rotated" x="10" y="200" style="font-size:16px" rotate="0 10 20">abc def</text>)svg"
        R"svg(<text id="cloned" x="10" y="120" style="font-size:16px">cloned text</text><use id="clone" xlink:href="#cloned" x="100"/>)svg"
        R"svg(<clipPath id="clip"><rect x="0" y="0" width="50" height="300"/></clipPath>)svg"
        R"svg(<text id="clipped" x="10" y="150" style="font-size:16px" clip-path="url(#clip)">clipped text</text>)svg"
        R"svg(<rect id="frame" x="200" y="200" width="60" height="30" style="fill:none"/>)svg"
        R"svg(<text id="flowed" style="font-size:16px;shape-inside:url(#frame)">a long text that overflows its frame</text>)svg"
        R"svg(<text id="rtl" x="10" y="280" style="font-size:16px">abc שלום def</text>)svg"
        R"svg(</svg>)svg");
    ASSERT_TRUE(document);
    EXPECT_EQ(BA::level_of(cast<SPText>(item(*document, "onpath"))), BA::Level::None);
    EXPECT_EQ(BA::level_of(cast<SPText>(item(*document, "vertical"))), BA::Level::None);
    EXPECT_EQ(BA::level_of(cast<SPText>(item(*document, "one"))), BA::Level::None);
    EXPECT_EQ(BA::level_of(cast<SPText>(item(*document, "rotated"))), BA::Level::None);
    EXPECT_EQ(BA::level_of(cast<SPText>(item(*document, "cloned"))), BA::Level::None) << "a clone refers to it";
    EXPECT_EQ(BA::level_of(cast<SPText>(item(*document, "clipped"))), BA::Level::None) << "clipped";
    EXPECT_EQ(BA::level_of(cast<SPText>(item(*document, "flowed"))), BA::Level::None) << "flowed into a shape";
    EXPECT_EQ(BA::level_of(cast<SPText>(item(*document, "rtl"))), BA::Level::None) << "a right-to-left run";
    Inkscape::ObjectSet set(document.get());
    set.setList(std::vector<SPItem *>{item(*document, "onpath"), item(*document, "vertical"), item(*document, "one"),
                                      item(*document, "rotated"), item(*document, "cloned"), item(*document, "clipped"),
                                      item(*document, "flowed"), item(*document, "rtl")});
    Inkscape::DocumentUndo::clearUndo(document.get());
    auto const outcome = BA::break_apart(set);
    EXPECT_EQ(outcome.texts, 0u);
    EXPECT_EQ(outcome.skipped, 8u);
    EXPECT_EQ(outcome.unmatched, 0u) << "refused by kind, not by the trial";
    EXPECT_EQ(text_count(*document), 8u);
    EXPECT_FALSE(Inkscape::DocumentUndo::undo(document.get()));
}

} // namespace
