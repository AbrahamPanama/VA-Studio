// SPDX-License-Identifier: GPL-2.0-or-later
// File-backed XML damage detection leaves the legacy recovery reader intact.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <glib.h>
#include <glib/gstdio.h>
#include <giomm/file.h>

#include "gc-anchored.h"
#include "document.h"
#include "document-undo.h"
#include "extension/init.h"
#include "inkscape.h"
#include "io/file.h"
#include "xml/node.h"
#include "xml/repr.h"

namespace {

struct TempFile {
    std::string path;

    explicit TempFile(char const *suffix, std::string const &bytes)
    {
        gchar *name = nullptr;
        GError *error = nullptr;
        int const fd = g_file_open_tmp("vacards-xml-report-XXXXXX", &name, &error);
        EXPECT_GE(fd, 0);
        if (error) g_error_free(error);
        if (fd >= 0) {
            g_close(fd, nullptr);
            path = std::string(name) + suffix;
            EXPECT_EQ(g_rename(name, path.c_str()), 0);
            EXPECT_TRUE(g_file_set_contents(path.c_str(), bytes.data(), bytes.size(), nullptr));
        }
        g_free(name);
    }

    ~TempFile() { if (!path.empty()) g_remove(path.c_str()); }
};

XmlReadReport read_report(TempFile const &file, bool *document_returned = nullptr)
{
    XmlReadReport report;
    auto *document = sp_repr_read_file(file.path.c_str(), SP_SVG_NS_URI, false, &report);
    if (document_returned) *document_returned = document != nullptr;
    if (document) Inkscape::GC::release(document);
    return report;
}

constexpr char kGzipSvg[] =
    "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xb3\x29\x2e\x4b\x57\xa8"
    "\xc8\xcd\xc9\x2b\xb6\x55\xcf\x28\x29\x29\xb0\xd2\xd7\x2f\x2f\x2f"
    "\xd7\x2b\x37\xd6\xcb\x2f\x4a\xd7\x37\x32\x30\x30\xd0\x07\xaa"
    "\x50\xb7\xb3\x01\x51\x76\x00\xf7\xa3\x84\x65\x2e\x00\x00\x00";

TEST(XmlReadReportTest, CompleteSvgIsWellFormed)
{
    TempFile file(".svg", "<svg xmlns='http://www.w3.org/2000/svg'><rect/></svg>");
    bool returned = false;
    auto report = read_report(file, &returned);
    EXPECT_TRUE(returned);
    EXPECT_TRUE(report.parsed);
    EXPECT_TRUE(report.well_formed);
    EXPECT_FALSE(report.input_error);
}

TEST(XmlReadReportTest, RecoveryReportsTruncatedAndMalformedXml)
{
    for (auto const &xml : {
             "<svg xmlns='http://www.w3.org/2000/svg'><rect>",
             "<svg xmlns='http://www.w3.org/2000/svg'><rect width='",
             "<svg xmlns='http://www.w3.org/2000/svg'></svg><extra/>"}) {
        TempFile file(".svg", xml);
        bool returned = false;
        auto report = read_report(file, &returned);
        EXPECT_TRUE(returned);
        EXPECT_TRUE(report.parsed);
        EXPECT_FALSE(report.well_formed);
        EXPECT_FALSE(report.message.empty());
        EXPECT_GT(report.line, 0);
        EXPECT_FALSE(report.input_error);
        // The old caller still recovers; diagnostics alone change no outcome.
        auto *legacy = sp_repr_read_file(file.path.c_str(), SP_SVG_NS_URI);
        ASSERT_NE(legacy, nullptr);
        Inkscape::GC::release(legacy);
    }
}

TEST(XmlReadReportTest, OrdinaryDoctypeAndStylesheetRemainAccepted)
{
    TempFile file(".svg",
        "<?xml version='1.0'?><?xml-stylesheet type='text/css' href='x.css'?>"
        "<!DOCTYPE svg [<!ENTITY label 'ok'>]>"
        "<svg xmlns='http://www.w3.org/2000/svg'><text>&label;</text></svg>");
    auto report = read_report(file);
    EXPECT_TRUE(report.parsed);
    EXPECT_TRUE(report.well_formed);
    EXPECT_FALSE(report.input_error);
    auto opened = SPDocument::createNewDoc(file.path.c_str(), false, nullptr, &report);
    EXPECT_NE(opened, nullptr);
}

TEST(XmlReadReportTest, LargeValidSvgIsNotRejected)
{
    std::string xml = "<svg xmlns='http://www.w3.org/2000/svg'><desc>";
    xml.append(5 * 1024 * 1024, 'a');
    xml += "</desc></svg>";
    TempFile file(".svg", xml);
    auto report = read_report(file);
    EXPECT_TRUE(report.parsed);
    EXPECT_TRUE(report.well_formed);
    EXPECT_FALSE(report.input_error);
    auto opened = SPDocument::createNewDoc(file.path.c_str(), false, nullptr, &report);
    EXPECT_NE(opened, nullptr);
}

TEST(XmlReadReportTest, GzipTrailerDamageIsInputErrorEvenAfterCompleteXml)
{
    std::string const valid(kGzipSvg, sizeof(kGzipSvg) - 1);
    TempFile whole(".svgz", valid);
    auto good = read_report(whole);
    EXPECT_TRUE(good.parsed);
    EXPECT_TRUE(good.well_formed);
    EXPECT_FALSE(good.input_error);

    auto bad_crc = valid;
    bad_crc[bad_crc.size() - 8] ^= 1;
    TempFile corrupt(".svgz", bad_crc);
    auto crc_report = read_report(corrupt);
    EXPECT_TRUE(crc_report.input_error);

    TempFile truncated(".svgz", valid.substr(0, valid.size() - 3));
    auto truncated_report = read_report(truncated);
    EXPECT_TRUE(truncated_report.input_error);
}

TEST(XmlReadReportTest, FileBackedDocumentRefusesRecoveredPartialSvg)
{
    TempFile good(".svg", "<svg xmlns='http://www.w3.org/2000/svg'><rect/></svg>");
    XmlReadReport report;
    auto document = SPDocument::createNewDoc(good.path.c_str(), false, nullptr, &report);
    ASSERT_NE(document, nullptr);
    EXPECT_TRUE(report.well_formed);

    TempFile damaged(".svg", "<svg xmlns='http://www.w3.org/2000/svg'><rect>");
    report = {};
    auto rejected = SPDocument::createNewDoc(damaged.path.c_str(), false, nullptr, &report);
    EXPECT_EQ(rejected, nullptr);
    EXPECT_TRUE(report.parsed); // libxml recovery did produce a partial tree.
    EXPECT_FALSE(report.well_formed);
}

TEST(XmlReadReportTest, DamagedRebaseLeavesLiveDocumentAndDirtyStateIntact)
{
    TempFile source(".svg", "<svg xmlns='http://www.w3.org/2000/svg' id='original'><rect/></svg>");
    auto document = SPDocument::createNewDoc(source.path.c_str());
    ASSERT_NE(document, nullptr);
    document->setModifiedSinceSave(true);
    auto *root = document->getReprRoot();
    ASSERT_NE(root, nullptr);
    ASSERT_STREQ(root->attribute("id"), "original");
    root->setAttribute("id", "edited");
    Inkscape::DocumentUndo::done(document.get(),
        Inkscape::Util::Internal::ContextString("SVG open test edit"), "");

    TempFile damaged(".svg", "<svg xmlns='http://www.w3.org/2000/svg' id='partial'><rect>");
    std::string error;
    EXPECT_FALSE(document->rebase(damaged.path.c_str(), true, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(document->getReprRoot(), root);
    ASSERT_NE(root->attribute("id"), nullptr);
    EXPECT_STREQ(root->attribute("id"), "edited");
    EXPECT_TRUE(document->isModifiedSinceSave());

    TempFile not_svg(".svg", "<notes><entry/></notes>");
    error.clear();
    EXPECT_FALSE(document->rebase(not_svg.path.c_str(), true, &error));
    EXPECT_NE(error.find("not an SVG"), std::string::npos);
    EXPECT_EQ(document->getReprRoot(), root);
    EXPECT_TRUE(document->isModifiedSinceSave());
    EXPECT_TRUE(Inkscape::DocumentUndo::undo(document.get()));
    EXPECT_STREQ(document->getReprRoot()->attribute("id"), "original");
    EXPECT_TRUE(Inkscape::DocumentUndo::redo(document.get()));
    EXPECT_STREQ(document->getReprRoot()->attribute("id"), "edited");
}

TEST(XmlReadReportTest, FileOpenDoesNotRetryDamagedSvgAsRecoveredPartialTree)
{
    // Native extension registration expects the headless application singleton.
    // Mac happened to tolerate a missing singleton; Windows does not.
    if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    Inkscape::Extension::init();
    TempFile good(".svg", "<svg xmlns='http://www.w3.org/2000/svg'><rect/></svg>");
    std::string error;
    auto [opened, good_cancelled] = ink_file_open(Gio::File::create_for_path(good.path), &error);
    EXPECT_NE(opened, nullptr);
    EXPECT_FALSE(good_cancelled);
    EXPECT_TRUE(error.empty());

    for (auto const *suffix : {".svg", ".dat"}) {
        TempFile damaged(suffix, "<svg xmlns='http://www.w3.org/2000/svg'><rect>");
        error.clear();
        auto [document, cancelled] = ink_file_open(Gio::File::create_for_path(damaged.path), &error);
        EXPECT_EQ(document, nullptr);
        EXPECT_FALSE(cancelled);
        EXPECT_FALSE(error.empty());
    }

    std::string bad_gzip(kGzipSvg, sizeof(kGzipSvg) - 1);
    bad_gzip[bad_gzip.size() - 8] ^= 1;
    TempFile damaged_svgz(".svgz", bad_gzip);
    error.clear();
    auto [compressed, compressed_cancelled] = ink_file_open(
        Gio::File::create_for_path(damaged_svgz.path), &error);
    EXPECT_EQ(compressed, nullptr);
    EXPECT_FALSE(compressed_cancelled);
    EXPECT_FALSE(error.empty());
}

} // namespace
