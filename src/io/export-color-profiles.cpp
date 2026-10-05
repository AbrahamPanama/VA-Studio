// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/export-color-profiles.h"
#include <algorithm>
#include <fstream>
#include <functional>
#include <filesystem>
#include <set>
#include <glibmm.h>
#include <glib/gstdio.h>
#include "colors/cms/system.h"
#include "io/resource.h"
#include "path-prefix.h"
#include "preferences.h"

namespace Inkscape::IO {
namespace {
std::string digest(std::vector<unsigned char> const &bytes)
{
    auto raw = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes.data(), bytes.size());
    std::string result(raw); g_free(raw); return result;
}
std::string canonical(std::string const &path)
{
    auto raw = g_canonicalize_filename(path.c_str(), nullptr);
    std::string result(raw); g_free(raw); return result;
}
}
ExportColorProfiles::ExportColorProfiles()
    : _data(get_inkscape_datadir()), _user(Glib::build_filename(Resource::profile_path(), "color", "icc")),
      _override(Glib::getenv("INKSCAPE_VACARDS_TIFF_ICC_PROFILE"))
{
    for (auto const &entry : Colors::CMS::System::get().getDirectoryPaths()) _dirs.push_back(entry.first);
}
ExportColorProfiles::ExportColorProfiles(std::string data, std::string user,
    std::vector<std::string> dirs, std::string override_path)
    : _data(std::move(data)), _user(std::move(user)), _override(std::move(override_path)), _dirs(std::move(dirs)) {}
ExportColorProfile ExportColorProfiles::srgb()
{
    static auto const builtin = [] {
        ExportColorProfile p;
        p.name = "sRGB (built-in)";
        auto h = cmsCreate_sRGBProfile();
        cmsUInt32Number size = 0;
        if (h && cmsSaveProfileToMem(h, nullptr, &size)) {
            p.bytes.resize(size);
            if (!cmsSaveProfileToMem(h, p.bytes.data(), &size)) p.bytes.clear();
        }
        if (h) cmsCloseProfile(h);
        p.sha256 = digest(p.bytes);
        return p;
    }();
    auto p = builtin; p.name = _("sRGB (built-in)"); return p;
}
bool ExportColorProfiles::read(std::string const &path, ExportColorProfile &profile, std::string &error)
{
    // Check before allocating, and read only the admitted length (also for sparse files).
    GStatBuf st;
    if (g_stat(path.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size < 128) {
        error = _("ICC profile is missing or too small."); return false;
    }
    if (std::uint64_t(st.st_size) > EXPORT_PROFILE_MAX_SIZE) {
        error = _("ICC profile exceeds the maximum supported size of 15 MiB."); return false;
    }
    auto file = g_fopen(path.c_str(), "rb");
    if (!file) { error = _("Could not read ICC profile."); return false; }
    std::unique_ptr<FILE, decltype(&fclose)> stream(file, &fclose);
    // Reject malformed headers before allocating their claimed payload.
    unsigned char header[128];
    if (fread(header, 1, 128, file) != 128 || std::memcmp(header + 36, "acsp", 4)) {
        error = _("Invalid ICC header."); return false;
    }
    auto size = (std::uint32_t(header[0]) << 24) | (std::uint32_t(header[1]) << 16) |
                (std::uint32_t(header[2]) << 8) | header[3];
    if (size != st.st_size) { error = _("ICC profile size does not match its header."); return false; }
    ExportColorProfile p;
    try { p.bytes.resize(size); } catch (std::bad_alloc const &) {
        error = _("Not enough memory to read ICC profile."); return false;
    }
    std::memcpy(p.bytes.data(), header, 128);
    if (fread(p.bytes.data() + 128, 1, size - 128, file) != size - 128 || fgetc(file) != EOF) {
        error = _("ICC profile changed while reading."); return false;
    }
    if (!export_color_transform(p.bytes, error)) return false;
    p.path = canonical(path); p.sha256 = digest(p.bytes);
    auto h = cmsOpenProfileFromMem(p.bytes.data(), p.bytes.size());
    char name[1024] = {};
    cmsGetProfileInfoASCII(h, cmsInfoDescription, "en", "US", name, sizeof(name));
    cmsCloseProfile(h);
    p.name = *name ? name : Glib::path_get_basename(path);
    profile = std::move(p); return true;
}
bool ExportColorProfiles::read_bytes(std::string const &path, std::vector<unsigned char> const &bytes,
                                    ExportColorProfile &profile, std::string &error)
{
    if (!export_color_transform(bytes,error)) return false;
    ExportColorProfile p; p.bytes=bytes; p.path=path; p.sha256=digest(bytes);
    auto h=cmsOpenProfileFromMem(bytes.data(),bytes.size());
    char name[1024]={}; cmsGetProfileInfoASCII(h,cmsInfoDescription,"en","US",name,sizeof(name)); cmsCloseProfile(h);
    p.name=*name ? name : Glib::path_get_basename(path); profile=std::move(p); return true;
}
ExportColorProfile ExportColorProfiles::default_profile(std::string &notice) const
{
    ExportColorProfile p; std::string error;
    if (!_override.empty()) {
        if (read(_override, p, error)) return p;
        notice += _("The ICC environment override is unavailable; using the default profile. ");
    }
    auto bundled = Glib::build_filename(_data, "inkscape", "color", "icc", "TheBest.icc");
    if (read(bundled, p, error)) return p;
    if (Glib::file_test(bundled, Glib::FileTest::EXISTS))
        notice += _("The bundled ICC profile is invalid; using sRGB. ");
    return srgb();
}
std::vector<ExportColorProfile> ExportColorProfiles::catalog(std::string *notice) const
{
    std::vector<ExportColorProfile> result{srgb()};
    auto add = [&](ExportColorProfile p) {
        if (std::none_of(result.begin(), result.end(), [&](auto const &v) { return v.id() == p.id(); }))
            result.push_back(std::move(p));
    };
    std::string default_notice;
    add(default_profile(default_notice));
    if (notice) *notice += default_notice;
    std::set<std::filesystem::path> visited;
    std::function<void(std::string const &)> scan = [&](std::string const &dir) {
        std::error_code ec;
        auto resolved = std::filesystem::canonical(std::filesystem::u8path(dir), ec);
        if (!ec && !visited.insert(resolved).second) return;
        try {
#ifdef _WIN32
            // GLib's Windows directory reader can report an access-denied
            // directory as empty. Native enumeration preserves that error so
            // the picker warns instead of silently hiding installed profiles.
            for (auto const &entry : std::filesystem::directory_iterator(std::filesystem::u8path(dir))) {
                auto utf8 = entry.path().filename().u8string();
                std::string name(utf8.begin(), utf8.end());
#else
            Glib::Dir entries(dir);
            for (auto const &name : entries) {
#endif
                auto path = Glib::build_filename(dir, name);
                if (Glib::file_test(path, Glib::FileTest::IS_DIR)) {
                    scan(path);
                } else {
                    auto dot = name.find_last_of('.');
                    if (dot == std::string::npos) continue;
                    auto suffix = name.substr(dot);
                    if (g_ascii_strcasecmp(suffix.c_str(), ".icc") &&
                        g_ascii_strcasecmp(suffix.c_str(), ".icm")) continue;
                    ExportColorProfile p; std::string error;
                    if (read(path, p, error)) add(std::move(p));
                }
            }
#ifdef _WIN32
        } catch (std::filesystem::filesystem_error const &error) {
            if (error.code() != std::errc::no_such_file_or_directory && notice) {
                *notice += Glib::ustring::compose(
                    _("Could not read ICC profile folder %1; skipped. %2\n"), dir, error.what());
            }
#endif
        } catch (Glib::FileError const &error) {
            // Missing optional system/user folders are normal. Other failures
            // are visible in the picker, and never abort sibling enumeration.
            if (error.code() != Glib::FileError::NO_SUCH_ENTITY && notice) {
                *notice += Glib::ustring::compose(
                    _("Could not read ICC profile folder %1; skipped. %2\n"), dir, error.what());
            }
        }
    };
    for (auto const &dir : _dirs) scan(dir);
    scan(_user);
    std::sort(result.begin() + 1, result.end(), [](auto const &a, auto const &b) {
        return a.name == b.name ? a.path < b.path : a.name < b.name;
    });
    return result;
}
bool ExportColorProfiles::add(std::string const &path, ExportColorProfile &profile, std::string &error) const
{
    ExportColorProfile p;
    if (!read(path, p, error)) return false;
    if (g_mkdir_with_parents(_user.c_str(), 0700)) {
        error = _("Could not create the user ICC profile folder."); return false;
    }
    // Content-addressed filenames prevent same-name collisions and make imports
    // idempotent even if the source file is renamed. Never overwrite another profile.
    p.path = canonical(Glib::build_filename(_user, p.sha256 + ".icc"));
    if (Glib::file_test(p.path, Glib::FileTest::EXISTS)) {
        ExportColorProfile existing;
        if (!read(p.path, existing, error) || existing.sha256 != p.sha256) {
            error = _("The stored ICC profile conflicts with this import."); return false;
        }
        profile = std::move(existing); return true;
    }
    GError *e = nullptr;
    if (!g_file_set_contents(p.path.c_str(), reinterpret_cast<char const *>(p.bytes.data()), p.bytes.size(), &e)) {
        error = e ? e->message : _("Could not copy ICC profile."); g_clear_error(&e); return false;
    }
    profile = std::move(p); return true;
}
ExportColorProfile ExportColorProfiles::resolve(std::string const &id, std::string &notice) const
{
    notice.clear();
    if (id == "srgb") return srgb();
    if (!id.empty()) {
        ExportColorProfile p; std::string error;
        if (id.size() > 65 && id[64] == ':' && read(id.substr(65), p, error) && p.id() == id) return p;
        notice = _("The saved output color profile is missing or invalid; using the default profile. ");
    }
    return default_profile(notice);
}
ExportColorProfile ExportColorProfiles::selected(std::string &notice) const
{
    return resolve(Preferences::get()->getString(EXPORT_PROFILE_PREFERENCE), notice);
}
void ExportColorProfiles::select(ExportColorProfile const &profile) const
{
    Preferences::get()->setString(EXPORT_PROFILE_PREFERENCE, profile.id());
}
PreparedExportProfile prepare_export_color_profile()
{
    PreparedExportProfile result;
    result.profile = ExportColorProfiles().selected(result.notice);
    std::string error;
    result.transform = export_color_transform(result.profile.bytes, error);
    if (!error.empty()) result.notice += error;
    return result;
}
} // namespace Inkscape::IO
