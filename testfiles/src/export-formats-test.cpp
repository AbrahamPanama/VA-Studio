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
#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include "ui/dialog/export.h"
#include "io/export-color-profiles.h"
#include "io/resource.h"
#include "document.h"
#include "desktop.h"
#include "inkscape.h"
#include "helper/png-write.h"
#include "colors/color.h"
#include <filesystem>
#include <fstream>
#include <span>
#include <glib/gstdio.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <functional>
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

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#endif

namespace {
#ifdef _WIN32
// chmod does not deny directory enumeration on Windows. Change only the
// disposable fixture's DACL and restore it even after a fatal assertion.
class DenyDirectoryRead {
public:
    explicit DenyDirectoryRead(std::string const &path) : path(std::filesystem::u8path(path)) {}
    bool apply() {
        // OpenSSH may enable SeBackupPrivilege, bypassing directory DACLs.
        // Restrict a private thread token, never the process or user's token.
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE,
                             TRUE, &previous_token) && GetLastError() != ERROR_NO_TOKEN) return false;
        HANDLE source = previous_token;
        if (!source && !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &source)) return false;
        HANDLE restricted = nullptr;
        auto duplicated = DuplicateTokenEx(source, TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE,
                                           nullptr, SecurityImpersonation, TokenImpersonation, &restricted);
        if (!previous_token) CloseHandle(source);
        if (!duplicated) return false;
        auto disabled = AdjustTokenPrivileges(restricted, TRUE, nullptr, 0, nullptr, nullptr);
        impersonating = disabled && SetThreadToken(nullptr, restricted);
        CloseHandle(restricted);
        if (!impersonating) return false;
        DWORD size = 0;
        GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &size);
        original.resize(size);
        if (!size || !GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                                      original.data(), size, &size)) return false;
        PSECURITY_DESCRIPTOR denied = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(D;;0x1;;;WD)(A;;FA;;;OW)", SDDL_REVISION_1, &denied, nullptr)) return false;
        changed = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, denied);
        LocalFree(denied);
        return changed;
    }
    ~DenyDirectoryRead() {
        if (changed) EXPECT_TRUE(SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, original.data()));
        if (impersonating) EXPECT_TRUE(SetThreadToken(nullptr, previous_token));
        if (previous_token) CloseHandle(previous_token);
    }
private:
    std::filesystem::path path;
    std::vector<unsigned char> original;
    bool changed = false;
    HANDLE previous_token = nullptr;
    bool impersonating = false;
};
#endif


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


TEST_F(ExportFormatsTest, OutputProfilePickerSharedByPngTiffJpegAndWebp)
{
    using namespace Inkscape::IO;
    auto saved = Inkscape::Preferences::get()->getString(EXPORT_PROFILE_PREFERENCE);
    auto blocked = Glib::build_filename(Inkscape::IO::Resource::profile_path(), "color", "icc", "r3-unreadable");
    ASSERT_EQ(g_mkdir_with_parents(blocked.c_str(), 0700), 0);
    struct RestoreDirectory {
        std::string path;
        ~RestoreDirectory() { g_chmod(path.c_str(), 0700); g_rmdir(path.c_str()); }
    } restore{blocked};
#ifdef _WIN32
    DenyDirectoryRead deny(blocked);
    ASSERT_TRUE(deny.apply());
#else
    ASSERT_EQ(g_chmod(blocked.c_str(), 0000), 0);
#endif
    ExportColorProfiles().select(ExportColorProfiles::srgb());
    Gtk::Window window;
    Gtk::Box box;
    Dialog::ExtensionList list;
    box.append(list); box.append(*list.getPrefButton()); window.set_child(box);
    list.setup(); Dialog::attach_tiff_profile_picker(list);
    window.present();
    auto pump = [] {
        auto context = Glib::MainContext::get_default();
        for (int i = 0; i < 30 && context->pending(); ++i) context->iteration(false);
    };
    std::function<Gtk::Label *(Gtk::Widget &)> find_label = [&](Gtk::Widget &widget) -> Gtk::Label * {
        if (auto label = dynamic_cast<Gtk::Label *>(&widget); label && label->get_text() == "Output color profile") return label;
        for (auto child = widget.get_first_child(); child; child = child->get_next_sibling()) {
            if (auto label = find_label(*child)) return label;
        }
        return nullptr;
    };
    for (auto filename : {"test.png", "test.tiff", "test.jpg", "test.webp"}) {
        list.setExtensionFromFilename(filename);
        auto popover = list.getPrefButton()->get_popover();
        popover->popup(); pump();
        auto label = find_label(*popover);
        EXPECT_NE(label, nullptr) << filename;
        if (label) {
            EXPECT_TRUE(label->get_parent()->get_visible()) << filename;
            Gtk::ComboBoxText *choice = nullptr;
            bool visible_notice = false;
            for (auto child = label->get_parent()->get_first_child(); child; child = child->get_next_sibling()) {
                if (auto combo = dynamic_cast<Gtk::ComboBoxText *>(child)) choice = combo;
                if (auto notice = dynamic_cast<Gtk::Label *>(child)) {
                    if (notice->get_text().find(blocked) != Glib::ustring::npos)
                        visible_notice = notice->get_visible();
                }
            }
            EXPECT_TRUE(visible_notice) << filename;
            EXPECT_NE(choice, nullptr) << filename;
            if (choice) { EXPECT_TRUE(choice->get_visible()); EXPECT_EQ(choice->get_active_id(), "srgb"); }
        }
        EXPECT_EQ(Inkscape::Preferences::get()->getString(EXPORT_PROFILE_PREFERENCE), "srgb");
        popover->popdown(); pump();
    }
    list.setExtensionFromFilename("test.pdf");
    auto popover = list.getPrefButton()->get_popover();
    popover->popup(); pump();
    if (auto label = find_label(*popover)) EXPECT_FALSE(label->get_parent()->get_visible());
    popover->popdown(); window.unset_child();
    box.remove(*list.getPrefButton()); box.remove(list); window.hide(); pump();
    Inkscape::Preferences::get()->setString(EXPORT_PROFILE_PREFERENCE, saved);
}


TEST_F(ExportFormatsTest, RealRasterExportUsesSelectedProfileInEveryContainer)
{
    using namespace Inkscape::IO;
    auto &app = application();
    if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    auto temp = g_dir_make_tmp("vacards-icc-e2e-XXXXXX", nullptr);
    ASSERT_NE(temp, nullptr);
    std::string dir(temp); g_free(temp);
    RecordProperty("evidence_directory", dir);
    auto path = [&](std::string name) { return dir + "/" + name; };
    auto saved = Inkscape::Preferences::get()->getString(EXPORT_PROFILE_PREFERENCE);
    auto owned = SPDocument::createNewDocFromMem(
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="80"><rect width="32" height="80" fill="#cc9933"/><rect x="32" width="32" height="80" fill="#2080e0"/></svg>)");
    ASSERT_TRUE(owned);
    auto doc = app.document_add(std::move(owned));
    auto desktop = app.createDesktop(doc, false, true);
    ASSERT_NE(desktop, nullptr);
    struct Cleanup {
        InkscapeApplication &app; SPDesktop *desktop; SPDocument *doc; std::string saved;
        ~Cleanup() {
            Inkscape::Preferences::get()->setString(EXPORT_PROFILE_PREFERENCE, saved);
            doc->setModifiedSinceSave(false); app.destroyDesktop(desktop);
        }
    } cleanup{app, desktop, doc, saved};
    app.set_active_desktop(desktop); INKSCAPE.activate_desktop(desktop);
    ASSERT_EQ(SP_ACTIVE_DESKTOP, desktop);
    doc->ensureUpToDate();
    cmsCIExyY white; cmsWhitePointFromTemp(&white, 6504);
    cmsCIExyYTRIPLE primaries = {{0.64,0.33,1},{0.30,0.60,1},{0.15,0.06,1}};
    auto curve = cmsBuildGamma(nullptr, 1.8);
    cmsToneCurve *curves[] = {curve, curve, curve};
    auto handle = cmsCreateRGBProfile(&white, &primaries, curves);
    cmsFreeToneCurve(curve); ASSERT_NE(handle, nullptr);
    cmsSetDeviceClass(handle, cmsSigOutputClass);
    ASSERT_TRUE(cmsSaveProfileToFile(handle, path("custom.icc").c_str())); cmsCloseProfile(handle);
    ExportColorProfile custom; std::string error;
    ASSERT_TRUE(ExportColorProfiles::read(path("custom.icc"), custom, error)) << error;
    auto srgb = ExportColorProfiles::srgb();
    { std::ofstream f(path("srgb.icc"), std::ios::binary); f.write(reinterpret_cast<char const *>(srgb.bytes.data()), srgb.bytes.size()); }
    Geom::Rect area(Geom::Point(0,0), Geom::Point(64,80));
    Dialog::ExtensionList formats; formats.setup();
    formats.setExtensionFromFilename("test.png");
    auto png_extension = formats.getExtension();
    ASSERT_NE(png_extension, nullptr);
    // Same settings as exportRaster, but the historical null-profile writer.
    ASSERT_EQ(sp_export_png_file(doc, path("baseline.png").c_str(), area, 64, 80, 96, 96,
        Inkscape::Colors::Color(0x00000000), nullptr, nullptr, true, {},
        png_extension->get_param_bool("png_interlacing", false), 6, 8,
        png_extension->get_param_int("png_compression", 1), png_extension->get_param_int("png_antialias", 2)), EXPORT_OK);
    for (auto const &profile : {srgb, custom}) {
        ExportColorProfiles().select(profile);
        std::string prefix = profile.id() == "srgb" ? "srgb" : "custom";
        for (auto suffix : {"png", "jpg", "webp", "tiff"}) {
            formats.setExtensionFromFilename(std::string("test.") + suffix);
            auto extension = formats.getExtension();
            ASSERT_NE(extension, nullptr);
            ASSERT_TRUE(Dialog::Export::exportRaster(area, 64, 80, 96,
                Inkscape::Colors::Color(0x00000000), path(prefix + "." + suffix), true,
                nullptr, nullptr, extension, nullptr)) << prefix << "." << suffix;
        }
    }
    // Admit valid, highly compressible RGB profiles above Pillow's default
    // 1 MiB iCCP bound, including the catalog's exact boundary. A private raw
    // tag grows the payload without changing the color transform.
    ExportColorProfiles large_catalog(dir, path("large-user"), {});
    for (auto size : {2u * 1024u * 1024u, EXPORT_PROFILE_MAX_SIZE - 4, EXPORT_PROFILE_MAX_SIZE}) {
        auto name = "large-" + std::to_string(size);
        auto handle = cmsOpenProfileFromMem(custom.bytes.data(), custom.bytes.size());
        ASSERT_NE(handle, nullptr);
        cmsUInt32Number base_size = 0;
        ASSERT_TRUE(cmsSaveProfileToMem(handle, nullptr, &base_size));
        std::vector<unsigned char> tag(size - base_size - 12, 0);
        std::memcpy(tag.data(), "text", 4);
        ASSERT_TRUE(cmsWriteRawTag(handle, static_cast<cmsTagSignature>(0x76616361), tag.data(), tag.size()));
        ASSERT_TRUE(cmsSaveProfileToFile(handle, path(name + ".icc").c_str()));
        cmsCloseProfile(handle);
        ASSERT_EQ(std::filesystem::file_size(path(name + ".icc")), size);
        ExportColorProfile large;
        ASSERT_TRUE(large_catalog.add(path(name + ".icc"), large, error)) << error;
        ASSERT_EQ(large.bytes.size(), size);
        ExportColorProfiles().select(large);
        if (size == 2u * 1024u * 1024u) {
            for (auto suffix : {"jpg", "webp"}) {
                formats.setExtensionFromFilename(std::string("test.") + suffix);
                ASSERT_TRUE(Dialog::Export::exportRaster(area, 64, 80, 96,
                    Inkscape::Colors::Color(0x00000000), path(name + "." + suffix), true,
                    nullptr, nullptr, formats.getExtension(), nullptr)) << name << "." << suffix;
            }
        } else {
            // Large binary stdout stalls the existing GUI extension transport.
            // Exercise the same native intermediate and actual converters with
            // file output, keeping the original compressible private-tag bytes.
            PreparedExportProfile prepared{large, export_color_transform(large.bytes, error), {}};
            ASSERT_TRUE(prepared.transform) << error;
            auto input = path(name + "-intermediate.png");
            ASSERT_EQ(sp_export_png_file(doc, input.c_str(), area, 64, 80, 96, 96,
                Inkscape::Colors::Color(0x00000000), nullptr, nullptr, true, {}, false, 6, 8, 6, 2,
                &prepared, true), EXPORT_OK);
            auto override = g_getenv("ICC_TEST_PYTHON");
            std::string python = override ? override : "python3";
            auto scripts = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "share/extensions";
            for (auto suffix : {"jpg", "webp"}) {
                auto script = (scripts / (std::string("raster_output_") + suffix + ".py")).string();
                auto output = "--output=" + path(name + "." + suffix);
                std::vector<char *> argv{python.data(), const_cast<char *>("-B"), script.data(),
                                        input.data(), output.data(), nullptr};
                gchar *out = nullptr, *err = nullptr; gint status = -1;
                ASSERT_TRUE(g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH,
                    nullptr, nullptr, &out, &err, &status, nullptr));
                EXPECT_EQ(status, 0) << (err ? err : "");
                g_free(out); g_free(err);
            }
        }
    }
    // Independently reopen every final container. ImageCms constructs its own
    // sRGB-to-selected-profile transform; Pillow exposes stored ICC bytes.
    std::string script = R"PY(
import sys, io
from pathlib import Path
from PIL import Image, ImageCms
root = Path(sys.argv[1])
baseline = Image.open(root / 'baseline.png').convert('RGB')
assert (root/'baseline.png').read_bytes() == (root/'srgb.png').read_bytes(), 'sRGB PNG byte regression'
for name in ('srgb', 'custom'):
    icc = (root/(name+'.icc')).read_bytes()
    expected = ImageCms.profileToProfile(baseline, ImageCms.createProfile('sRGB'),
        ImageCms.ImageCmsProfile(io.BytesIO(icc)), renderingIntent=1, flags=0x2000).convert('RGB')
    if name == 'custom': assert expected.tobytes() != baseline.tobytes()
    for suffix in ('png','jpg','webp','tiff'):
        actual = Image.open(root/(name+'.'+suffix))
        if name == 'srgb' and suffix == 'png': assert actual.info.get('icc_profile') is None
        else: assert actual.info.get('icc_profile') == icc, (name, suffix, 'ICC mismatch')
        actual = actual.convert('RGB')
        for xy in ((12,40),(48,40)):
            delta = max(abs(a-b) for a,b in zip(actual.getpixel(xy), expected.getpixel(xy)))
            assert delta <= (5 if suffix == 'jpg' else 0), (name,suffix,xy,actual.getpixel(xy),expected.getpixel(xy))
print('8 real exports: exact ICC payloads (legacy untagged sRGB PNG), converted samples and PNG byte compatibility passed')
for profile in sorted(root.glob('large-*.icc')):
    icc = profile.read_bytes()
    expected = ImageCms.profileToProfile(baseline, ImageCms.createProfile('sRGB'),
        ImageCms.ImageCmsProfile(io.BytesIO(icc)), renderingIntent=1, flags=0x2000).convert('RGB')
    for suffix in ('jpg', 'webp'):
        output = profile.with_suffix('.'+suffix)
        actual = Image.open(output)
        assert actual.info.get('icc_profile') == icc, (profile.name, suffix, 'ICC mismatch')
        actual = actual.convert('RGB')
        for xy in ((12,40), (48,40)):
            delta = max(abs(a-b) for a,b in zip(actual.getpixel(xy), expected.getpixel(xy)))
            assert delta <= (5 if suffix == 'jpg' else 0), (profile.name, suffix, 'pixel mismatch')
        if suffix == 'jpg':
            # Parse the APP2 sequence ourselves as a second oracle: every
            # fragment's index/count and the concatenated exact ICC payload.
            data = output.read_bytes()
            assert data[:2] == b'\xff\xd8'
            at, fragments = 2, []
            while at < len(data):
                assert data[at] == 255
                marker = data[at+1]; at += 2
                if marker in (0xda, 0xd9): break
                size = int.from_bytes(data[at:at+2], 'big')
                body = data[at+2:at+size]; at += size
                if marker == 0xe2 and body.startswith(b'ICC_PROFILE\0'):
                    fragments.append(body)
            assert 1 < len(fragments) <= 255
            assert [f[12] for f in fragments] == list(range(1, len(fragments)+1))
            assert all(f[13] == len(fragments) for f in fragments)
            assert b''.join(f[14:] for f in fragments) == icc
            print(profile.name, 'JPEG exact ICC in', len(fragments), 'APP2 segments')
        else:
            print(profile.name, 'WebP exact ICC and converted samples')
print('2 MiB, near-maximum and maximum RGB profiles: 6 real exports passed')
)PY";
    auto python_override = g_getenv("ICC_TEST_PYTHON");
    std::string python = python_override ? python_override : "python3";
    std::vector<char *> argv{python.data(), const_cast<char *>("-c"), script.data(), dir.data(), nullptr};
    gchar *out = nullptr, *err = nullptr; gint status = -1;
    ASSERT_TRUE(g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, &out, &err, &status, nullptr));
    EXPECT_EQ(status, 0) << (err ? err : "");
    if (out) std::cout << out;
    g_free(out); g_free(err);
}

} // namespace
