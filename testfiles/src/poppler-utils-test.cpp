// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * Authors:
 *   See git history
 *
 * @copyright
 * Copyright (C) 2024 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 *
 * @file @brief Unit tests for PDF parsing utilities
 */

#include "extension/internal/pdfinput/poppler-utils.h"

#include <GlobalParams.h>
#include <PDFDoc.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtest/gtest.h>

#include <string>

namespace Inkscape {

TEST(PopplerUtilsTest, SanitizeId)
{
    ASSERT_EQ(sanitizeId(""), "_");
    ASSERT_EQ(sanitizeId("hello"), "hello");
    ASSERT_EQ(sanitizeId("a bc"), "a_20bc");
    ASSERT_EQ(sanitizeId("a\xff"
                         "bc"),
              "a_ffbc");
}
} // namespace Inkscape

TEST(PopplerUtilsTest, GetNameWithoutSubsetTag)
{
  ASSERT_EQ(getNameWithoutSubsetTag("AAAAAA+aff65d+OpenSans"), "OpenSans");
  ASSERT_EQ(getNameWithoutSubsetTag("AAAAAA+OpenSans"), "OpenSans");
  ASSERT_EQ(getNameWithoutSubsetTag("OpenSn+Regular"), "Regular");
  ASSERT_EQ(getNameWithoutSubsetTag("AAAAAAAAAAAAAA+OpenSans-Regular"), "AAAAAAAAAAAAAA+OpenSans-Regular");
  ASSERT_EQ(getNameWithoutSubsetTag("AAAAA0+NotoSans-Regular"), "NotoSans-Regular");
}

namespace {

/// A temporary PDF file opened with Poppler; the file is removed after the document is closed.
struct TempPdf
{
    std::string path;
    std::shared_ptr<PDFDoc> doc;
    PDFDoc *operator->() const { return doc.get(); }
    ~TempPdf()
    {
        doc.reset();
        g_remove(path.c_str());
    }
};

/// Write hand-written PDF syntax (no xref table; Poppler reconstructs it) and open it.
TempPdf openPdf(std::string const &name, std::string const &body)
{
    if (!globalParams) {
        globalParams = _POPPLER_NEW_GLOBAL_PARAMS();
    }
    TempPdf pdf;
    pdf.path = std::string(g_get_tmp_dir()) + "/va-poppler-utils-" + name + "-" + std::to_string(g_get_monotonic_time()) + ".pdf";
    EXPECT_TRUE(g_file_set_contents(pdf.path.c_str(), body.data(), body.size(), nullptr));
    pdf.doc = _POPPLER_MAKE_SHARED_PDFDOC(pdf.path.c_str());
    return pdf;
}

std::string pdfBody(std::string const &pages_dict, std::string const &objects)
{
    return "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n" + pages_dict + objects + "trailer\n<< /Root 1 0 R >>\n%%EOF\n";
}

} // namespace

// PDF1: a font that fails to load must not make the font scan dereference a null font.
TEST(PopplerUtilsTest, GetPdfFontsSkipsFontsThatFailToLoad)
{
    auto doc = openPdf("badfont", pdfBody(
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n",
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R /F2 7 /F3 8 0 R >> >> >>\nendobj\n"
        "4 0 obj\n<< /Length 0 >>\nstream\n\nendstream\nendobj\n"
        "5 0 obj\n<< /Type /Font /Subtype /Type0 /BaseFont /Broken /Encoding /NoSuchCMapXYZ /DescendantFonts [6 0 R] >>\nendobj\n"
        "6 0 obj\n<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Broken >>\nendobj\n"
        "8 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n"));
    ASSERT_TRUE(doc->isOk());

    auto fonts = getPdfFonts(doc.doc);
    ASSERT_TRUE(fonts);
    // Only the Helvetica font loads; the broken ones are skipped rather than crashing the scan.
    EXPECT_EQ(fonts->size(), 1u);
    for (auto const &[font, data] : *fonts) {
        EXPECT_TRUE(font);
    }
}

// PDF2: a page tree that claims more pages than it holds must not make the font scan dereference a null page.
TEST(PopplerUtilsTest, GetPdfFontsSurvivesPageCountLargerThanPageTree)
{
    auto doc = openPdf("count", pdfBody(
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 3 >>\nendobj\n",
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n"
        "4 0 obj\n<< /Length 0 >>\nstream\n\nendstream\nendobj\n"
        "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n"));
    ASSERT_TRUE(doc->isOk());

    auto fonts = getPdfFonts(doc.doc);
    ASSERT_TRUE(fonts);
    EXPECT_EQ(fonts->size(), 1u);
}

// PDF13: pages sharing one font dictionary and font list that font once, with every page it is on.
TEST(PopplerUtilsTest, GetPdfFontsListsSharedFontOnce)
{
    std::string objects =
        "5 0 obj\n<< /F1 6 0 R >>\nendobj\n"
        "6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n"
        "4 0 obj\n<< /Length 0 >>\nstream\n\nendstream\nendobj\n";
    std::string kids;
    for (int i = 0; i < 4; ++i) {
        kids += std::to_string(10 + i) + " 0 R ";
        objects += std::to_string(10 + i) + " 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                   "/Contents 4 0 R /Resources << /Font 5 0 R >> >>\nendobj\n";
    }
    auto doc = openPdf("shared", pdfBody("2 0 obj\n<< /Type /Pages /Kids [" + kids + "] /Count 4 >>\nendobj\n", objects));
    ASSERT_TRUE(doc->isOk());
    ASSERT_EQ(doc->getCatalog()->getNumPages(), 4);

    auto fonts = getPdfFonts(doc.doc);
    ASSERT_TRUE(fonts);
    ASSERT_EQ(fonts->size(), 1u);
    auto const &data = fonts->begin()->second;
    EXPECT_EQ(data.pages.size(), 4u);
    for (int page = 1; page <= 4; ++page) {
        EXPECT_TRUE(data.pages.count(page)) << "page " << page;
    }
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
