// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * VIEW-2: guide the user to turn off PowerToys' SVG add-ons on Windows.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "svg-viewer-guide.h"

#include <algorithm>
#include <cctype>
#include <glibmm/i18n.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glibmm/spawn.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>
#include <glibmm/fileutils.h>
#include <memory>

#include "desktop.h"
#include "inkscape-window.h"
#include "preferences.h"

#ifdef _WIN32
// Last: its macros (DOUBLE_CLICK, ...) collide with gtkmm's headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Inkscape::UI::SvgViewerGuide {

namespace {

// VA Studio's shell extension (packaging/windows/vacards/svg-shell/vasvgthumb.h).
constexpr char const *vastudio_thumbnail = "{3A05ACA6-6D9C-4FEA-9240-E0F85BD15050}";
constexpr char const *vastudio_preview = "{7828A5A8-03F2-4DAF-AE5E-E142BE538EA8}";

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

#ifdef _WIN32
std::string utf8(std::wstring const &text)
{
    if (text.empty()) {
        return {};
    }
    gchar *converted = g_utf16_to_utf8(reinterpret_cast<gunichar2 const *>(text.c_str()), -1, nullptr, nullptr, nullptr);
    std::string result = converted ? converted : "";
    g_free(converted);
    return result;
}

std::wstring wide(std::string const &text)
{
    gunichar2 *converted = g_utf8_to_utf16(text.c_str(), -1, nullptr, nullptr, nullptr);
    std::wstring result = converted ? reinterpret_cast<wchar_t const *>(converted) : L"";
    g_free(converted);
    return result;
}

// Default value of HKEY_CLASSES_ROOT\<key> (the per-user and machine view File
// Explorer uses), expanded.
std::string classes_default(std::string const &key)
{
    auto const name = wide(key);
    DWORD size = 0;
    DWORD const flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    if (RegGetValueW(HKEY_CLASSES_ROOT, name.c_str(), nullptr, flags, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CLASSES_ROOT, name.c_str(), nullptr, flags, nullptr, value.data(), &size) != ERROR_SUCCESS) {
        return {};
    }
    value.resize(wcslen(value.c_str()));
    return utf8(value);
}

Handler handler_for(char const *shellex)
{
    Handler handler;
    handler.clsid = classes_default(std::string(".svg\\ShellEx\\") + shellex);
    if (!handler.clsid.empty()) {
        handler.server = classes_default("CLSID\\" + handler.clsid + "\\InprocServer32");
    }
    return handler;
}
#endif

void open_powertoys(std::string const &exe)
{
    try {
        // PowerToys opens its settings at a page given by name; PowerPreview is
        // "File Explorer add-ons" (verified with PowerToys 0.101).
        Glib::spawn_async({}, std::vector<std::string>{exe, "--open-settings=PowerPreview"});
    } catch (Glib::Error const &error) {
        g_warning("SVG viewer guide: could not start PowerToys: %s", error.what());
    }
}

class GuideWindow : public Gtk::Window
{
public:
    GuideWindow(std::function<SvgViewerGuide::State()> read)
        : _read(std::move(read))
    {
        set_title(_("SVG previews in File Explorer"));
        set_default_size(520, -1);
        set_resizable(false);
        set_hide_on_close(true);

        _box.set_margin(18);
        _box.set_spacing(12);
        set_child(_box);

        auto intro = Gtk::make_managed<Gtk::Label>(
            _("PowerToys is showing SVG thumbnails and previews in File Explorer, so VA Studio's own (framed on the "
              "drawing) are not used. Turn off PowerToys' SVG add-ons; VA Studio does not change PowerToys for you."));
        intro->set_wrap(true);
        intro->set_max_width_chars(60);
        intro->set_xalign(0);
        _box.append(*intro);

        auto step1 = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        auto step1_text = Gtk::make_managed<Gtk::Label>(_("1. Open PowerToys at File Explorer add-ons."));
        step1_text->set_xalign(0);
        step1_text->set_hexpand(true);
        step1->append(*step1_text);
        step1->append(_open);
        _open.set_label(_("Open PowerToys"));
        _open.signal_clicked().connect([this] {
            if (!_state.powertoys.empty()) open_powertoys(_state.powertoys);
        });
        _box.append(*step1);
        _no_powertoys.set_text(_("If the button does not open it: Start menu > PowerToys > File Explorer add-ons."));
        _no_powertoys.set_wrap(true);
        _no_powertoys.set_xalign(0);
        _box.append(_no_powertoys);

        _box.append(*step(_preview_status,
                         _("2. Under Preview Pane, turn off Scalable Vector Graphics (.svg).")));
        _box.append(*step(_thumbnail_status,
                         _("3. Scroll to Thumbnail icon Preview and turn off Scalable Vector Graphics (.svg).")));
        _box.append(*step(_icons_status,
                         _("4. In File Explorer, open ... > Options > View and clear \"Always show icons, never "
                           "thumbnails\".")));

        _result.set_wrap(true);
        _result.set_max_width_chars(60);
        _result.set_xalign(0);
        _box.append(_result);

        auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        _dont_show.set_label(_("Don't show this again"));
        _dont_show.set_hexpand(true);
        buttons->append(_dont_show);
        _check.set_label(_("Check Again"));
        _check.signal_clicked().connect([this] { refresh(); });
        buttons->append(_check);
        _close.set_label(_("Close"));
        _close.signal_clicked().connect([this] { close(); });
        buttons->append(_close);
        _box.append(*buttons);

        // Coming back from PowerToys re-checks by itself.
        property_is_active().signal_changed().connect([this] {
            if (is_active()) refresh();
        });
        signal_close_request().connect(
            [this] {
                if (_dont_show.get_active()) {
                    Inkscape::Preferences::get()->setBool(dismissed_pref, true);
                }
                return false;
            },
            false);
        signal_hide().connect([this] {
            Glib::signal_idle().connect_once([this] { delete this; });
        });
        refresh();
    }

private:
    Gtk::Box *step(Gtk::Label &status, Glib::ustring const &text)
    {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        auto label = Gtk::make_managed<Gtk::Label>(text);
        label->set_wrap(true);
        label->set_xalign(0);
        label->set_hexpand(true);
        row->append(*label);
        row->append(status);
        return row;
    }

    static void mark(Gtk::Label &status, bool done)
    {
        status.set_text(done ? _("✓ Done") : _("Pending"));
        if (done) {
            status.add_css_class("success");
        } else {
            status.remove_css_class("success");
        }
    }

    void refresh()
    {
        _state = _read ? _read() : read_state();
        _open.set_visible(!_state.powertoys.empty());
        _no_powertoys.set_visible(_state.powertoys.empty());
        auto const now = progress(_state);
        mark(_preview_status, now.preview_off);
        mark(_thumbnail_status, now.thumbnail_off);
        mark(_icons_status, now.thumbnails_shown);
        switch (now.result) {
        case Progress::Result::Pending:
            _result.set_text(_("When every step shows Done, File Explorer uses VA Studio for SVG files. The "
                               "preview pane opens with Alt+P in File Explorer."));
            break;
        case Progress::Result::Done:
            _result.set_text(_("Done: File Explorer now uses VA Studio for SVG files. Close and reopen File "
                               "Explorer windows; thumbnails Windows stored earlier refresh when a file changes."));
            _dont_show.set_active(true);
            break;
        case Progress::Result::NotActive:
            _result.set_text(_("PowerToys no longer shows SVG files, but VA Studio's viewer is not active for you. "
                               "Reinstall VA Studio to register it again."));
            break;
        }
    }

    std::function<SvgViewerGuide::State()> _read;
    SvgViewerGuide::State _state;
    Gtk::Box _box{Gtk::Orientation::VERTICAL};
    Gtk::Button _open;
    Gtk::Label _no_powertoys;
    Gtk::Label _preview_status;
    Gtk::Label _thumbnail_status;
    Gtk::Label _icons_status;
    Gtk::Label _result;
    Gtk::CheckButton _dont_show;
    Gtk::Button _check;
    Gtk::Button _close;
};

} // namespace

Owner owner_of(Handler const &handler)
{
    if (handler.clsid.empty()) {
        return Owner::None;
    }
    auto const clsid = lower(handler.clsid);
    if (clsid == lower(vastudio_thumbnail) || clsid == lower(vastudio_preview)) {
        return Owner::VaStudio;
    }
    if (lower(handler.server).find("powertoys") != std::string::npos) {
        return Owner::PowerToys;
    }
    return Owner::Other;
}

bool needs_guide(State const &state)
{
    return state.vastudio_registered &&
           (owner_of(state.thumbnail) == Owner::PowerToys || owner_of(state.preview) == Owner::PowerToys ||
            state.icons_only);
}

Progress progress(State const &state)
{
    Progress result;
    result.preview_off = owner_of(state.preview) != Owner::PowerToys;
    result.thumbnail_off = owner_of(state.thumbnail) != Owner::PowerToys;
    result.thumbnails_shown = !state.icons_only;
    if (result.preview_off && result.thumbnail_off && result.thumbnails_shown) {
        bool const ours = owner_of(state.preview) == Owner::VaStudio && owner_of(state.thumbnail) == Owner::VaStudio;
        result.result = ours ? Progress::Result::Done : Progress::Result::NotActive;
    }
    return result;
}

State read_state()
{
    State state;
#ifdef _WIN32
    state.thumbnail = handler_for("{E357FCCD-A995-4576-B01F-234630154E96}");
    state.preview = handler_for("{8895B1C6-B41F-4C1C-A562-0D564250836F}");
    state.vastudio_registered =
        !classes_default(std::string("CLSID\\") + vastudio_thumbnail + "\\InprocServer32").empty();
    DWORD icons_only = 0;
    DWORD size = sizeof(icons_only);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                     L"IconsOnly", RRF_RT_REG_DWORD, nullptr, &icons_only, &size) == ERROR_SUCCESS) {
        state.icons_only = icons_only != 0;
    }
    for (auto const *base : {"LOCALAPPDATA", "ProgramFiles"}) {
        auto const root = Glib::getenv(base);
        if (root.empty()) continue;
        auto const exe = Glib::build_filename(root, "PowerToys", "PowerToys.exe");
        if (Glib::file_test(exe, Glib::FileTest::IS_REGULAR)) {
            state.powertoys = exe;
            break;
        }
    }
#endif
    return state;
}

Gtk::Window *show(Gtk::Window *parent, std::function<State()> read)
{
    auto *window = new GuideWindow(std::move(read));
    if (parent) {
        window->set_transient_for(*parent);
    }
    window->present();
    return window;
}

void offer_at_startup(SPDesktop *desktop)
{
#ifdef _WIN32
    static bool offered = false;
    if (offered || !desktop || Inkscape::Preferences::get()->getBool(dismissed_pref, false)) {
        return;
    }
    offered = true;
    auto alive = std::make_shared<bool>(true);
    auto connection =
        std::make_shared<sigc::connection>(desktop->connectDestroy([alive](SPDesktop *) { *alive = false; }));
    Glib::signal_timeout().connect_once([desktop, alive, connection] {
        connection->disconnect();
        if (!*alive || !needs_guide(read_state())) {
            return;
        }
        show(desktop->getInkscapeWindow());
    }, 2000);
#else
    (void)desktop;
#endif
}

} // namespace Inkscape::UI::SvgViewerGuide
