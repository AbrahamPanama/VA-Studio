// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EXP-1: the formats the Export dialog offers, and their order, per user.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>
#include <gtkmm/application.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <string>
#include <vector>

#include "extension/extension.h"
#include "extension/output.h"
#include "inkscape-application.h"
#include "preferences.h"
#include "ui/export-formats.h"
#include "ui/widget/export-formats-editor.h"
#include "ui/widget/export-lists.h"

using namespace Inkscape::UI;

namespace {

constexpr char const *PNG = "org.inkscape.output.png.inkscape";
constexpr char const *TIFF = "org.inkscape.raster.tiff_output";
constexpr char const *JPEG = "org.inkscape.raster.jpg_output";
constexpr char const *PDF = "org.inkscape.output.pdf.cairorenderer";
constexpr char const *VA_SVG = "org.inkscape.output.svg.inkscape";
constexpr char const *PLAIN_SVG = "org.inkscape.output.svg.plain";
constexpr char const *EPS = SP_MODULE_KEY_PRINT_CAIRO_EPS;

std::vector<std::string> shown_ids(std::vector<ExportFormats::Format> const &formats)
{
    std::vector<std::string> ids;
    for (auto const &format : formats) {
        if (format.shown) {
            ids.push_back(format.id);
        }
    }
    return ids;
}

ExportFormats::Format const *find(std::vector<ExportFormats::Format> const &formats, std::string const &id)
{
    auto const found = std::find_if(formats.begin(), formats.end(), [&](auto const &f) { return f.id == id; });
    return found == formats.end() ? nullptr : &*found;
}

class ExportFormatsTest : public ::testing::Test
{
protected:
    static InkscapeApplication &application()
    {
        static auto *instance = [] {
            Gtk::Application::wrap_in_search_entry2();
            g_setenv("INKSCAPE_APP_ID_TAG", "exportformatstest", true);
            auto *result = new InkscapeApplication();
            result->gio_app()->register_application(); // loads the extensions
            for (auto signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) {
                std::signal(signal, SIG_DFL);
            }
            return result;
        }();
        return *instance;
    }

    void SetUp() override
    {
        auto const *gui = std::getenv("INKSCAPE_TEST_GUI");
        if (!gui || std::string(gui) != "1") {
            GTEST_SKIP() << "GUI testing not enabled";
        }
        application();
        ExportFormats::reset();
    }

    void TearDown() override { ExportFormats::reset(); }
};

TEST_F(ExportFormatsTest, NewInstallShowsTheShortListGrouped)
{
    auto const formats = ExportFormats::formats();
    ASSERT_GT(formats.size(), 6u);
    // Grouped: Raster (PNG, TIFF, JPEG) then Vector (PDF, VA Studio SVG, plain SVG).
    EXPECT_EQ(shown_ids(formats), (std::vector<std::string>{PNG, TIFF, JPEG, PDF, VA_SVG, PLAIN_SVG}));
    for (auto const *id : {PNG, TIFF, JPEG}) {
        ASSERT_TRUE(find(formats, id)) << id;
        EXPECT_EQ(find(formats, id)->group, ExportFormats::Group::Raster) << id;
    }
    for (auto const *id : {PDF, VA_SVG, PLAIN_SVG, EPS}) {
        ASSERT_TRUE(find(formats, id)) << id;
        EXPECT_EQ(find(formats, id)->group, ExportFormats::Group::Vector) << id;
    }
    EXPECT_FALSE(find(formats, EPS)->shown);
    for (std::size_t i = 1; i < formats.size(); ++i) {
        EXPECT_LE(formats[i - 1].group, formats[i].group) << "groups stay together";
    }
}

TEST_F(ExportFormatsTest, SavedOrderAndShownFormatsPersistAndNewFormatsStayHidden)
{
    auto formats = ExportFormats::formats();
    // Hide PNG, show EPS, and move JPEG first in the Raster group.
    for (auto &format : formats) {
        if (format.id == PNG) format.shown = false;
        if (format.id == EPS) format.shown = true;
    }
    auto const jpeg = std::find_if(formats.begin(), formats.end(), [](auto const &f) { return f.id == JPEG; });
    std::rotate(formats.begin(), jpeg, jpeg + 1);
    // A format the user never saw: drop it from the stored lists.
    auto const unseen = std::find_if(formats.begin(), formats.end(), [](auto const &f) {
        return f.group == ExportFormats::Group::Text;
    });
    ASSERT_NE(unseen, formats.end());
    auto const unseen_id = unseen->id;
    unseen->shown = true;
    ExportFormats::save(formats);
    auto *prefs = Inkscape::Preferences::get();
    auto order = prefs->getString(ExportFormats::order_pref).raw();
    auto shown = prefs->getString(ExportFormats::shown_pref).raw();
    auto const erase = [&unseen_id](std::string &list) {
        auto const at = list.find(unseen_id);
        ASSERT_NE(at, std::string::npos);
        list.erase(at, unseen_id.size());
    };
    erase(order);
    erase(shown);
    prefs->setString(ExportFormats::order_pref, order);
    prefs->setString(ExportFormats::shown_pref, shown);

    auto const reloaded = ExportFormats::formats();
    EXPECT_EQ(reloaded.front().id, JPEG);
    EXPECT_FALSE(find(reloaded, PNG)->shown);
    EXPECT_TRUE(find(reloaded, EPS)->shown);
    EXPECT_FALSE(find(reloaded, unseen_id)->shown) << "a format added later starts hidden";
}

TEST_F(ExportFormatsTest, ExportComboListsShownFormatsAndMoreFormatsListsEverything)
{
    Dialog::ExtensionList combo;
    combo.setup();
    EXPECT_EQ(combo.listedIds(), (std::vector<std::string>{PNG, TIFF, JPEG, PDF, VA_SVG, PLAIN_SVG}));
    EXPECT_EQ(combo.get_active_id(), PNG);
    ASSERT_TRUE(combo.getExtension());

    // "More formats..." lists every available format and keeps the selection.
    combo.set_active_id(Dialog::ExtensionList::more_formats_id);
    EXPECT_EQ(combo.listedIds().size(), ExportFormats::formats().size());
    EXPECT_EQ(combo.get_active_id(), PNG);
    combo.popdown();
}

TEST_F(ExportFormatsTest, HiddenFormatStillExportsWhenTheFilenameAsksForIt)
{
    Dialog::ExtensionList combo;
    combo.setup();
    ASSERT_EQ(std::find(combo.listedIds().begin(), combo.listedIds().end(), EPS), combo.listedIds().end());
    combo.setExtensionFromFilename("card.eps");
    ASSERT_TRUE(combo.getExtension());
    EXPECT_STREQ(combo.getExtension()->get_id(), EPS);
    EXPECT_NE(std::find(combo.listedIds().begin(), combo.listedIds().end(), EPS), combo.listedIds().end());
}

TEST_F(ExportFormatsTest, EveryFormatUncheckedStillExportsPng)
{
    auto formats = ExportFormats::formats();
    for (auto &format : formats) format.shown = false;
    ExportFormats::save(formats);
    Dialog::ExtensionList combo;
    combo.setup();
    EXPECT_EQ(combo.get_active_id(), PNG);
    ASSERT_TRUE(combo.getExtension());
}

TEST_F(ExportFormatsTest, OpenComboFollowsPreferenceChangesOnAFreshProfile)
{
    // No stored lists yet (fresh profile): the combo must still see changes.
    ASSERT_FALSE(Inkscape::Preferences::get()->getEntry(ExportFormats::shown_pref).isSet());
    Dialog::ExtensionList combo;
    combo.setup();
    ASSERT_EQ(std::find(combo.listedIds().begin(), combo.listedIds().end(), EPS), combo.listedIds().end());
    auto formats = ExportFormats::formats();
    for (auto &format : formats) {
        if (format.id == EPS) format.shown = true;
    }
    ExportFormats::save(formats);
    EXPECT_NE(std::find(combo.listedIds().begin(), combo.listedIds().end(), EPS), combo.listedIds().end());
}

TEST_F(ExportFormatsTest, MoreFormatsKeepsTheSelectedFormatWithoutPassingThroughPng)
{
    Dialog::ExtensionList combo;
    combo.setup();
    combo.set_active_id(PDF);
    std::vector<std::string> seen;
    auto connection = combo.signal_changed().connect([&] { seen.emplace_back(combo.get_active_id()); });
    combo.set_active_id(Dialog::ExtensionList::more_formats_id);
    combo.popdown();
    connection.disconnect();
    EXPECT_EQ(combo.get_active_id(), PDF);
    EXPECT_EQ(std::count(seen.begin(), seen.end(), std::string(PNG)), 0);
}

// Owner report 2026-09-28: after "More formats..." the short list was lost for
// the rest of the dialog, and Restore Short List did not bring it back.
TEST_F(ExportFormatsTest, ChoosingFromMoreFormatsReturnsToTheShortListWithTheChoice)
{
    Dialog::ExtensionList combo;
    combo.setup();
    combo.set_active_id(Dialog::ExtensionList::more_formats_id);
    combo.popdown();
    ASSERT_EQ(combo.listedIds().size(), ExportFormats::formats().size());

    combo.set_active_id(EPS);
    EXPECT_EQ(combo.get_active_id(), EPS);
    EXPECT_EQ(combo.listedIds(), (std::vector<std::string>{PNG, TIFF, JPEG, PDF, VA_SVG, PLAIN_SVG, EPS}))
        << "short list again, plus the chosen hidden format";
    ASSERT_TRUE(combo.getExtension());
    EXPECT_STREQ(combo.getExtension()->get_id(), EPS);

    combo.set_active_id(PNG);
    EXPECT_EQ(combo.listedIds(), (std::vector<std::string>{PNG, TIFF, JPEG, PDF, VA_SVG, PLAIN_SVG}));
}

TEST_F(ExportFormatsTest, HiddenFormatFromTheFilenameIsDroppedWhenAnotherIsChosen)
{
    Dialog::ExtensionList combo;
    combo.setup();
    combo.setExtensionFromFilename("card.eps");
    ASSERT_EQ(combo.get_active_id(), EPS);
    EXPECT_NE(std::find(combo.listedIds().begin(), combo.listedIds().end(), EPS), combo.listedIds().end());
    combo.set_active_id(PNG);
    EXPECT_EQ(combo.listedIds(), (std::vector<std::string>{PNG, TIFF, JPEG, PDF, VA_SVG, PLAIN_SVG}));
}

TEST_F(ExportFormatsTest, RestoreShortListStoresTheDefaultsThemselves)
{
    auto *prefs = Inkscape::Preferences::get();
    // A default format missing from the current lists (as when its output is
    // deactivated) is still in the restored short list.
    prefs->setString(ExportFormats::order_pref, PNG);
    prefs->setString(ExportFormats::shown_pref, PNG);
    Widget::ExportFormatsEditor editor;
    editor.restoreShortList();
    EXPECT_EQ(shown_ids(ExportFormats::formats()), (std::vector<std::string>{PNG, TIFF, JPEG, PDF, VA_SVG, PLAIN_SVG}));
    EXPECT_NE(prefs->getString(ExportFormats::shown_pref).raw().find(TIFF), std::string::npos);
}

TEST_F(ExportFormatsTest, RestoreShortListEndsAMoreFormatsExpansion)
{
    ASSERT_FALSE(Inkscape::Preferences::get()->getEntry(ExportFormats::shown_pref).isSet()) << "fresh profile";
    Dialog::ExtensionList combo;
    combo.setup();
    combo.set_active_id(Dialog::ExtensionList::more_formats_id);
    combo.popdown();
    ASSERT_EQ(combo.listedIds().size(), ExportFormats::formats().size());

    Widget::ExportFormatsEditor editor;
    editor.restoreShortList();
    EXPECT_EQ(combo.listedIds(), (std::vector<std::string>{PNG, TIFF, JPEG, PDF, VA_SVG, PLAIN_SVG}));
    EXPECT_EQ(combo.get_active_id(), PNG);
}

TEST_F(ExportFormatsTest, SavingKeepsFormatsThatAreNotAvailableNow)
{
    auto *prefs = Inkscape::Preferences::get();
    prefs->setString(ExportFormats::order_pref, std::string("org.example.missing,") + PNG);
    prefs->setString(ExportFormats::shown_pref, std::string("org.example.missing,") + PNG);
    ExportFormats::save(ExportFormats::formats());
    EXPECT_NE(prefs->getString(ExportFormats::order_pref).raw().find("org.example.missing"), std::string::npos);
    EXPECT_NE(prefs->getString(ExportFormats::shown_pref).raw().find("org.example.missing"), std::string::npos);
}

TEST_F(ExportFormatsTest, PreferencesEditorStoresAndReordersInsideGroups)
{
    Widget::ExportFormatsEditor editor;
    auto const &formats = editor.formats();
    auto const eps = std::find_if(formats.begin(), formats.end(), [](auto const &f) { return f.id == EPS; });
    ASSERT_NE(eps, formats.end());
    editor.setShown(eps - formats.begin(), true);
    EXPECT_TRUE(find(ExportFormats::formats(), EPS)->shown);

    // TIFF moves up inside Raster; the first Vector format cannot move into Raster.
    auto const tiff = std::find_if(formats.begin(), formats.end(), [](auto const &f) { return f.id == TIFF; });
    ASSERT_TRUE(editor.move(tiff - formats.begin(), -1));
    EXPECT_EQ(ExportFormats::formats().front().id, TIFF);
    auto const first_vector = std::find_if(formats.begin(), formats.end(), [](auto const &f) {
        return f.group == ExportFormats::Group::Vector;
    });
    EXPECT_FALSE(editor.move(first_vector - formats.begin(), -1));

    editor.restoreShortList();
    EXPECT_EQ(shown_ids(ExportFormats::formats()), (std::vector<std::string>{PNG, TIFF, JPEG, PDF, VA_SVG, PLAIN_SVG}));
}

} // namespace
