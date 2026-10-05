// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Test Inkscape::Extensions::Internal::PdfOutput
 */
/*
 * Authors:
 *   Charlotte Curtis
 *
 * Copyright (C) 2025 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "config.h"
#include <gtest/gtest.h>
#include <2geom/svg-path-writer.h>
#include <2geom/rect.h>
#include <GfxState.h>
#define Operator Operator_Gfx // Same Poppler/libcroco isolation as svg-builder.h.
#include <Gfx.h>
#undef Operator
#ifdef _WIN32
// Win32 macros collide with gtkmm enum and member names below.
#undef IGNORE
#undef near
#endif
#include "extension/internal/pdfinput/pdf-utils.h"
#include "extension/internal/pdfinput/poppler-transition-api.h"
#include "svg/svg.h"

class PdfUtilsTest : public ::testing::Test
{
public:
    // Test vectors and comparePaths method copied from path-boolop-test.cpp
    Geom::PathVector const rectangle_bigger = sp_svg_read_pathv("M 0,0 L 0,2 L 2,2 L 2,0 z");
    Geom::PathVector const rectangle_smaller = sp_svg_read_pathv("M 0.5,0.5 L 0.5,1.5 L 1.5,1.5 L 1.5,0.5 z");
    Geom::PathVector const rectangle_outside = sp_svg_read_pathv("M 0,1.5 L 0.5,1.5 L 0.5,2.5 L 0,2.5 z");
    Geom::PathVector const empty = sp_svg_read_pathv("");
    
    // shapes to test fill rules
    Geom::PathVector const star = sp_svg_read_pathv("M 0,10 20,0 15,25 5,0 25,15 z");
    Geom::PathVector const star_odd_even =
        sp_svg_read_pathv("M 5 0 L 7.5 6.25 L 11 4.5 z M 11 4.5 L 18.04296875 9.783203125 L 20 0 z M 18.04296875 "
                          "9.783203125 L 17.30859375 13.4609375 L 25 15 z M 17.30859375 13.4609375 L 9.783203125 "
                          "11.95703125 L 15 25 z M 9.783203125 11.95703125 L 7.5 6.25 L 0 10 z");
    Geom::PathVector const star_non_zero =
        sp_svg_read_pathv("M 5 0 L 7.5 6.25 L 0 10 L 9.783203125 11.95703125 L 15 25 L 17.30859375 13.4609375 L 25 15 "
                          "L 18.04296875 9.783203125 L 20 0 L 11 4.5 z");
    Geom::PathVector const rectangle_star = sp_svg_read_pathv("M 0,0 L 0,25 L 25,25 L 25,0 z");

    PDFRectangle *page_bbox;
    GfxState *state;

    // Dynamic Poppler stuff
    void SetUp() override {
        // A sufficiently large fake page bounding box for Poppler state object use
        page_bbox = new PDFRectangle(0, 0, 30, 30);
        state = new _POPPLER_GFX_STATE(72, 72, *page_bbox, 0, false);
    }

    // Clean up
    void TearDown() override {
        delete page_bbox;
        delete state;
    }
    
    static void comparePaths(Geom::PathVector const &result, Geom::PathVector const &reference)
    {
        Geom::SVGPathWriter wr;
        wr.feed(result);
        auto const resultD = wr.str();
        wr.clear();
        wr.feed(reference);
        auto const referenceD = wr.str();
        EXPECT_EQ(resultD, referenceD);
        EXPECT_EQ(result, reference);
    }

    static void writeGfxState(GfxState *state, Geom::PathVector const &path) {
        for (const auto &path : path) {
            if (path.empty()) continue;

            // Start a new subpath with the first point
            auto startPoint = path.initialPoint();
            state->moveTo(startPoint.x(), startPoint.y());

            // Process each curve in the path
            for (const auto &curve : path) {
                if (curve.isLineSegment()) {
                    auto endPoint = curve.finalPoint();
                    state->lineTo(endPoint.x(), endPoint.y());
                } else {
                    // Lazy, but I'm only using straight line segments in this test program
                    std::cout << "Unsupported curve type" << std::endl;
                }
            }
            if (path.closed()) {
                state->closePath();
            }
        }
    }
};

// Tests for ClipHistoryEntry class
TEST_F(PdfUtilsTest, ClipHistoryEntryConstructor) {
    // Test default constructor (empty path)
    ClipHistoryEntry clip_history;
    EXPECT_FALSE(clip_history.hasClipPath());
    EXPECT_FALSE(clip_history.hasSaves());
    EXPECT_EQ(clip_history.getFillRule(), FillRule::fill_nonZero);
}

TEST_F(PdfUtilsTest, ClipHistoryEntryWithPath) {
    // Test constructor with path
    ClipHistoryEntry clip_history(rectangle_bigger, clipNormal);
    EXPECT_TRUE(clip_history.hasClipPath());
    EXPECT_FALSE(clip_history.hasSaves());
    EXPECT_EQ(clip_history.getFillRule(), FillRule::fill_nonZero);
    comparePaths(clip_history.getClipPath(), rectangle_bigger);
}

TEST_F(PdfUtilsTest, ClipHistoryEntrySaveRestore) {
    ClipHistoryEntry *clip_history = new ClipHistoryEntry(rectangle_bigger, clipNormal);
    
    // Save the current state
    ClipHistoryEntry *saved = clip_history->save();
    EXPECT_TRUE(saved->hasSaves());
    EXPECT_TRUE(saved->hasClipPath());
    EXPECT_TRUE(saved->isCopied());
    
    // Restore should return the original clip_history
    ClipHistoryEntry *restored = saved->restore();
    EXPECT_EQ(restored, clip_history);
    EXPECT_FALSE(restored->hasSaves());
    
    delete restored;
}

TEST_F(PdfUtilsTest, ClipHistoryEntrySetClipPathV) {
    ClipHistoryEntry *clip_history = new ClipHistoryEntry();
    // push another instance to the stack to call setClip
    clip_history = clip_history->save();
    clip_history->setClip(rectangle_bigger, FillRule::fill_oddEven);
    
    EXPECT_TRUE(clip_history->hasClipPath());
    EXPECT_EQ(clip_history->getFillRule(), FillRule::fill_oddEven);
    comparePaths(clip_history->getClipPath(), rectangle_bigger);
    
    delete clip_history;
}

TEST_F(PdfUtilsTest, ClipHistoryEntrySetClipGfxState) {
    ClipHistoryEntry *clip_history = new ClipHistoryEntry();
    clip_history = clip_history->save();    
    writeGfxState(state, rectangle_bigger);
    clip_history->setClip(state);
    EXPECT_TRUE(clip_history->hasClipPath());
    EXPECT_EQ(clip_history->getFillRule(), FillRule::fill_nonZero);
    comparePaths(clip_history->getClipPath(), rectangle_bigger);
    
    state->clearPath();
    delete clip_history;
}

// Tests for helper functions
TEST_F(PdfUtilsTest, GetRectFromPDFRectangle) {
    PDFRectangle pdf_rect = PDFRectangle(10, 20, 30, 40);
    Geom::Rect result = getRect(&pdf_rect);
    Geom::Rect expected(10.0, 20.0, 30.0, 40.0);
    
    EXPECT_EQ(result, expected);
}

TEST_F(PdfUtilsTest, MaybeIntersectBothEmpty) {
    auto result = maybeIntersect(empty, empty);
    comparePaths(result, empty);
}

TEST_F(PdfUtilsTest, MaybeIntersectOneEmpty) {
    // If first is empty, return second
    auto result1 = maybeIntersect(empty, rectangle_bigger);
    comparePaths(result1, rectangle_bigger);
    
    // If second is empty, return first
    auto result2 = maybeIntersect(rectangle_bigger, empty);
    comparePaths(result2, rectangle_bigger);
}

TEST_F(PdfUtilsTest, MaybeIntersectBothFilled) {
    // Test intersection of two overlapping rectangles
    auto result = maybeIntersect(rectangle_bigger, rectangle_smaller);
    comparePaths(result, rectangle_smaller);
}

TEST_F(PdfUtilsTest, MaybeIntersectSimpleDifferentFills) {
    // for these basic rectangles the fill rule shouldn't matter
    auto result = maybeIntersect(rectangle_bigger, rectangle_smaller, 
                                FillRule::fill_nonZero, FillRule::fill_oddEven);
    comparePaths(result, rectangle_smaller);
}

TEST_F(PdfUtilsTest, MaybeIntersectNoOverlap) {
    Geom::PathVector not_overlapping = sp_svg_read_pathv("M 2,2 L 2,3 L 3,3 L 3,2 z");
    auto result = maybeIntersect(rectangle_smaller, not_overlapping);
    // Non-overlapping rectangles should result in empty intersection
    EXPECT_TRUE(result.empty());
}


TEST_F(PdfUtilsTest, ClipHistoryEntryFlattenedClipPath) {
    ClipHistoryEntry *clip_history = new ClipHistoryEntry(rectangle_bigger, clipNormal);
    clip_history = clip_history->save();
    
    // The flattened path should be the same as the original when there's only one level
    comparePaths(clip_history->getFlattenedClipPath(), rectangle_bigger);
    
    delete clip_history;
}

TEST_F(PdfUtilsTest, ClipHistoryEntryFlattenedPathSimple) {
    // Test multiple levels of clipping
    ClipHistoryEntry *clip_history = new ClipHistoryEntry(rectangle_bigger, clipNormal);
    clip_history = clip_history->save();
    clip_history->setClip(rectangle_smaller, FillRule::fill_nonZero);
    
    // The flattened clip should be the same as the smaller rectangle
    auto flattened = clip_history->getFlattenedClipPath();
    comparePaths(flattened, rectangle_smaller);
    
    delete clip_history;
}

TEST_F(PdfUtilsTest, ClipHistoryEntryFlattenedPathSkipLevel) {
    // Test multiple levels of clipping
    ClipHistoryEntry *clip_history = new ClipHistoryEntry(rectangle_bigger, clipNormal);
    clip_history = clip_history->save();
    clip_history->clear();
    clip_history = clip_history->save();
    clip_history->setClip(rectangle_smaller, FillRule::fill_nonZero);

    // Still should be the same as the smaller rectangle
    auto flattened = clip_history->getFlattenedClipPath();
    comparePaths(flattened, rectangle_smaller);
        
    delete clip_history;
}

TEST_F(PdfUtilsTest, ClipHistoryEntryFlattenedOddEven) {
    ClipHistoryEntry *clip_history = new ClipHistoryEntry(star, clipEO);
    clip_history = clip_history->save();
    clip_history->setClip(rectangle_star, FillRule::fill_nonZero);
    auto result = clip_history->getFlattenedClipPath();
    comparePaths(result, star_odd_even);
    
    delete clip_history;
}

TEST_F(PdfUtilsTest, ClipHistoryEntryFlattenedOddEvenSkipLevel) {
    ClipHistoryEntry *clip_history = new ClipHistoryEntry(star, clipEO);
    clip_history = clip_history->save();
    clip_history->clear();
    clip_history = clip_history->save();
    clip_history->setClip(rectangle_star, FillRule::fill_nonZero);
    auto result = clip_history->getFlattenedClipPath();
    comparePaths(result, star_odd_even);
    
    delete clip_history;
}


TEST_F(PdfUtilsTest, ClipHistoryEntryStarIntersectionNonZero) {
    ClipHistoryEntry *clip_history = new ClipHistoryEntry(star, clipNormal);
    clip_history = clip_history->save();
    clip_history->setClip(rectangle_star, FillRule::fill_nonZero);
    auto result = clip_history->getFlattenedClipPath();
    comparePaths(result, star_non_zero);
    
    delete clip_history;
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :

// Request-local intake tests. Generate complete PDFs in memory, including xref,
// so missing-font coverage does not depend on the host's PDF export fonts.
#include <iomanip>
#include <sstream>
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "preferences.h"
#include "page-manager.h"
#include "util/units.h"
#include "xml/repr.h"
#include "xml/node.h"
#include "style.h"
#include "extension/internal/pdfinput/pdf-input.h"

namespace {
using namespace Inkscape;
using namespace Inkscape::Extension::Internal;

std::string request_pdf(bool missing_font = false)
{
    auto stream = [](std::string const &s) {
        return "<< /Length " + std::to_string(s.size()) + " >>\nstream\n" + s + "endstream";
    };
    std::vector<std::string> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents 5 0 R >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources << /Font << /F1 7 0 R >> >> /Contents 6 0 R >>",
        stream("1 0 0 rg 10 10 20 20 re f\n"),
        stream(missing_font ? "BT /F1 12 Tf 10 30 Td (Missing font text) Tj ET\n" : "0 0 1 rg 10 10 40 20 re f\n"),
        "<< /Type /Font /Subtype /Type1 /BaseFont /VACardsRequestMissingFont73917 /Encoding /WinAnsiEncoding >>"
    };
    if (!missing_font) objects[3] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources << >> /Contents 6 0 R >>";
    std::ostringstream pdf;
    pdf << "%PDF-1.4\n";
    std::vector<long> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<long>(pdf.tellp()));
        pdf << i + 1 << " 0 obj\n" << objects[i] << "\nendobj\n";
    }
    auto xref = static_cast<long>(pdf.tellp());
    pdf << "xref\n0 " << objects.size() + 1 << "\n0000000000 65535 f \n";
    for (auto offset : offsets) pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
    pdf << "trailer\n<< /Size " << objects.size() + 1 << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
    return pdf.str();
}

// Both pages reach the same indirect Form and resource dictionary. Its font
// must be admitted for every requested page, even after another page scanned it.
std::string shared_form_request_pdf()
{
    auto stream = [](std::string const &s, std::string const &entries = "") {
        return "<< /Length " + std::to_string(s.size()) + entries + " >>\nstream\n" + s + "endstream";
    };
    std::vector<std::string> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /XObject << /Shared 9 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /XObject << /Shared 9 0 R >> >> /Contents 6 0 R >>",
        stream("/Shared Do\n"),
        stream("/Shared Do\n"),
        "<< /Type /Font /Subtype /Type1 /BaseFont /VACardsSharedFormMissingFont73917 /Encoding /WinAnsiEncoding >>",
        "<< /Font << /F1 7 0 R >> >>",
        stream("BT /F1 12 Tf 10 30 Td (Shared form text) Tj ET\n",
               " /Type /XObject /Subtype /Form /BBox [0 0 100 100] /Resources 8 0 R")
    };
    std::ostringstream pdf;
    pdf << "%PDF-1.4\n";
    std::vector<long> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<long>(pdf.tellp()));
        pdf << i + 1 << " 0 obj\n" << objects[i] << "\nendobj\n";
    }
    auto xref = static_cast<long>(pdf.tellp());
    pdf << "xref\n0 " << objects.size() + 1 << "\n0000000000 65535 f \n";
    for (auto offset : offsets) pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
    pdf << "trailer\n<< /Size " << objects.size() + 1 << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
    return pdf.str();
}

class PdfRequestTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!Application::exists()) Application::create(false);
    }
    PdfInput input;
};

TEST_F(PdfRequestTest, AllPagesSubsetOrderAndInvalid)
{
    PdfIntakeOptions options;
    options.pages = {1, 2};
    auto both = input.open_request(request_pdf(), "two.pdf", options);
    ASSERT_TRUE(both.document);
    EXPECT_FALSE(both.error);
    EXPECT_EQ(both.document->getPageManager().getPages().size(), 2);
    EXPECT_EQ(both.report.imported_pages, options.pages);
    EXPECT_EQ(both.report.catalog_pages, 2);
    EXPECT_FALSE(DocumentUndo::getUndoSensitive(both.document.get()));
    EXPECT_NE(both.document->getReprRoot()->attribute("viewBox"), nullptr);
    EXPECT_NE(sp_repr_save_buf(both.document->getReprDoc()).raw().find("#ff0000"), std::string::npos);
    EXPECT_NE(sp_repr_save_buf(both.document->getReprDoc()).raw().find("#0000ff"), std::string::npos);
    options.pages = {2};
    auto second = input.open_request(request_pdf(), "two.pdf", options);
    ASSERT_TRUE(second.document);
    EXPECT_NEAR(second.document->getWidth().value("px"), 200 * 96.0 / 72, 0.01);
    EXPECT_EQ(second.report.imported_pages, options.pages);
    options.pages = {2, 1};
    auto reversed = input.open_request(request_pdf(), "two.pdf", options);
    ASSERT_TRUE(reversed.document);
    EXPECT_EQ(reversed.document->getPageManager().getPages().size(), 2);
    EXPECT_NEAR(reversed.document->getWidth().value("px"), 200 * 96.0 / 72, 0.01);
    options.pages = {3};
    auto bad = input.open_request(request_pdf(), "two.pdf", options);
    EXPECT_FALSE(bad.document);
    ASSERT_TRUE(bad.error);
    EXPECT_EQ(bad.error->code, "pages-invalid");
    EXPECT_EQ(bad.error->invalid_pages, options.pages);
    options.pages = {1, 0, 3};
    bad = input.open_request(request_pdf(), "two.pdf", options);
    ASSERT_TRUE(bad.error);
    EXPECT_EQ(bad.error->invalid_pages, (std::vector<int>{0, 3}));
    EXPECT_TRUE(bad.report.imported_pages.empty());
}

TEST_F(PdfRequestTest, MissingFontRejectAndSubstitute)
{
    PdfIntakeOptions options;
    options.pages = {2};
    auto rejected = input.open_request(request_pdf(true), "font.pdf", options);
    EXPECT_FALSE(rejected.document);
    ASSERT_TRUE(rejected.error);
    EXPECT_EQ(rejected.error->code, "fonts-missing");
    ASSERT_EQ(rejected.report.fonts.size(), 1);
    EXPECT_EQ(rejected.report.fonts[0].name, "VACardsRequestMissingFont73917");
    EXPECT_TRUE(rejected.report.fonts[0].substitute.empty());
    options.font_policy = PdfFontPolicy::SubstituteMissing;
    auto substituted = input.open_request(request_pdf(true), "font.pdf", options);
    ASSERT_TRUE(substituted.document);
    ASSERT_EQ(substituted.report.fonts.size(), 1);
    EXPECT_TRUE(substituted.report.fonts[0].missing);
    EXPECT_EQ(substituted.report.fonts[0].name, "VACardsRequestMissingFont73917");
    EXPECT_FALSE(substituted.report.fonts[0].substitute.empty());
    EXPECT_NE(sp_repr_save_buf(substituted.document->getReprDoc()).raw().find("Missing font text"), std::string::npos);
    options.pages = {1};
    options.font_policy = PdfFontPolicy::RejectMissing;
    auto unused = input.open_request(request_pdf(true), "font.pdf", options);
    ASSERT_TRUE(unused.document);
    EXPECT_TRUE(unused.report.fonts.empty());
}

TEST_F(PdfRequestTest, SharedFormMissingFontReject)
{
    for (auto const &pages : {std::vector<int>{2}, std::vector<int>{1, 2}}) {
        SCOPED_TRACE(::testing::PrintToString(pages));
        PdfIntakeOptions options;
        options.pages = pages;
        options.font_policy = PdfFontPolicy::RejectMissing;
        auto result = input.open_request(shared_form_request_pdf(), "shared-form.pdf", options);
        EXPECT_FALSE(result.document);
        ASSERT_TRUE(result.error);
        EXPECT_EQ(result.error->code, "fonts-missing");
        EXPECT_TRUE(result.report.imported_pages.empty());
        ASSERT_EQ(result.report.fonts.size(), 1);
        auto const &font = result.report.fonts.front();
        EXPECT_EQ(font.name, "VACardsSharedFormMissingFont73917");
        EXPECT_TRUE(font.missing);
        EXPECT_EQ(font.pages, pages);
        EXPECT_TRUE(font.substitute.empty());
    }
}

TEST_F(PdfRequestTest, SharedFormMissingFontSubstitute)
{
    for (auto const &pages : {std::vector<int>{2}, std::vector<int>{1, 2}}) {
        SCOPED_TRACE(::testing::PrintToString(pages));
        PdfIntakeOptions options;
        options.pages = pages;
        options.font_policy = PdfFontPolicy::SubstituteMissing;
        auto result = input.open_request(shared_form_request_pdf(), "shared-form.pdf", options);
        ASSERT_TRUE(result.document);
        EXPECT_FALSE(result.error);
        EXPECT_EQ(result.report.imported_pages, pages);
        EXPECT_EQ(result.document->getPageManager().getPages().size(), pages.size());
        ASSERT_EQ(result.report.fonts.size(), 1);
        auto const &font = result.report.fonts.front();
        EXPECT_EQ(font.name, "VACardsSharedFormMissingFont73917");
        EXPECT_TRUE(font.missing);
        EXPECT_EQ(font.pages, pages);
        ASSERT_FALSE(font.substitute.empty());
        auto const xml = sp_repr_save_buf(result.document->getReprDoc()).raw();
        // Verify every text span uses the reported strategy. SVG serialization
        // quotes family names containing spaces; compare parsed CSS values.
        size_t span_count = 0;
        auto check_spans = [&](auto const &self, XML::Node const *node) -> void {
            if (g_strcmp0(node->name(), "svg:tspan") == 0) {
                ++span_count;
                auto css = sp_repr_css_attr(node, "style");
                Glib::ustring family = sp_repr_css_property(css, "font-family", "");
                sp_repr_css_attr_unref(css);
                css_font_family_unquote(family);
                EXPECT_EQ(family.raw(), font.substitute);
                EXPECT_NE(family.raw(), font.name);
            }
            for (auto child = node->firstChild(); child; child = child->next()) {
                self(self, child);
            }
        };
        check_spans(check_spans, result.document->getReprRoot());
        EXPECT_EQ(span_count, pages.size());
        size_t text_count = 0;
        for (size_t pos = 0; (pos = xml.find("Shared form text", pos)) != std::string::npos; ++pos) {
            ++text_count;
        }
        EXPECT_EQ(text_count, pages.size());
    }
}

void snapshot_preferences(std::string const &path, std::map<std::string, std::string> &snapshot)
{
    auto prefs = Preferences::get();
    for (auto const &entry : prefs->getAllEntries(path)) snapshot[entry.getPath()] = entry.getString();
    for (auto const &dir : prefs->getAllDirs(path)) snapshot_preferences(dir, snapshot);
}

TEST_F(PdfRequestTest, PreferencesUnchanged)
{
    std::map<std::string, std::string> before, after;
    snapshot_preferences("/", before);
    PdfIntakeOptions options;
    options.pages = {1, 2};
    options.convert_colors = false;
    options.group_by = "by-layer";
    options.crop_box = "media-box";
    options.approximation_precision = 7;
    {
        auto result = input.open_request(request_pdf(), "prefs.pdf", options);
        ASSERT_TRUE(result.document);
        EXPECT_FALSE(result.report.conversion.convert_colors);
    }
    snapshot_preferences("/", after);
    EXPECT_EQ(before, after);
}

TEST_F(PdfRequestTest, MalformedTruncatedAndUnsupportedOptions)
{
    PdfIntakeOptions options;
    options.pages = {1};
    for (auto const &bytes : {std::string{}, std::string("not a PDF"), request_pdf().substr(0, 45)}) {
        auto bad = input.open_request(bytes, "bad.pdf", options);
        EXPECT_FALSE(bad.document);
        ASSERT_TRUE(bad.error);
        EXPECT_EQ(bad.error->code, "pdf-invalid");
    }
    options.import_type = PdfImportType::PDF_IMPORT_CAIRO;
    options.pages = {1, 2};
    auto bad = input.open_request(request_pdf(), "two.pdf", options);
    ASSERT_TRUE(bad.error);
    EXPECT_EQ(bad.error->code, "mode-unsupported");
    options.import_type = PdfImportType::PDF_IMPORT_INTERNAL;
    options.pages.clear();
    bad = input.open_request(request_pdf(), "two.pdf", options);
    ASSERT_TRUE(bad.error);
    EXPECT_EQ(bad.error->code, "pages-invalid");
    options.pages = {1};
    options.crop_box = "invalid";
    bad = input.open_request(request_pdf(), "two.pdf", options);
    ASSERT_TRUE(bad.error);
    EXPECT_EQ(bad.error->code, "options-invalid");
}
} // namespace
