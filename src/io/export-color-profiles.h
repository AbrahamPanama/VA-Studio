// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_EXPORT_COLOR_PROFILES_H
#define INKSCAPE_IO_EXPORT_COLOR_PROFILES_H
#include <lcms2.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <glib/gi18n.h>

namespace Inkscape::IO {
// Bound catalog allocation and every writer to 15 MiB. This fits TIFF's
// uint32_t ICC length and JPEG's 255 APP2 segments (65519 ICC bytes each).
// Keep raster_output_{jpg,webp}.py's PNG iCCP read limit in sync.
inline constexpr std::uint32_t EXPORT_PROFILE_MAX_SIZE = 15u * 1024u * 1024u;
inline constexpr char EXPORT_PROFILE_PREFERENCE[] = "/dialogs/export/output-color-profile";
struct ExportColorProfile {
    std::string path, sha256, name;
    std::vector<unsigned char> bytes;
    std::string id() const { return path.empty() ? "srgb" : sha256 + ":" + path; }
};
using ExportColorTransform = std::shared_ptr<void>;
// Shared writer contract: straight RGBA8, relative colorimetric, black point
// compensation, alpha copied. Header-only so standalone TIFF tests use it too.
inline ExportColorTransform export_color_transform(std::vector<unsigned char> const &bytes, std::string &error)
{
    error.clear();
    if (bytes.size() > EXPORT_PROFILE_MAX_SIZE) {
        error = _("ICC profile exceeds the maximum supported size of 15 MiB.");
        return {};
    }
    if (bytes.size() < 128 || std::memcmp(bytes.data() + 36, "acsp", 4) != 0) {
        error = _("Invalid ICC header or profile size.");
        return {};
    }
    auto declared = (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
                    (std::uint32_t(bytes[2]) << 8) | bytes[3];
    if (declared != bytes.size()) {
        error = _("ICC profile size does not match its header.");
        return {};
    }
    auto destination = cmsOpenProfileFromMem(bytes.data(), bytes.size());
    if (!destination) { error = _("Invalid ICC profile."); return {}; }
    auto cls = cmsGetDeviceClass(destination);
    bool valid = cmsGetColorSpace(destination) == cmsSigRgbData &&
                 (cls == cmsSigDisplayClass || cls == cmsSigOutputClass) &&
                 cmsIsIntentSupported(destination, INTENT_RELATIVE_COLORIMETRIC, LCMS_USED_AS_OUTPUT);
    auto source = cmsCreate_sRGBProfile();
    auto transform = valid && source ? cmsCreateTransform(source, TYPE_RGBA_8, destination, TYPE_RGBA_8,
        INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_COPY_ALPHA) : nullptr;
    if (source) cmsCloseProfile(source);
    cmsCloseProfile(destination);
    if (!transform) error = _("Choose an RGB display or output ICC profile supporting relative-colorimetric output.");
    return ExportColorTransform(transform, [](void *p) { if (p) cmsDeleteTransform(p); });
}
struct PreparedExportProfile {
    ExportColorProfile profile;
    ExportColorTransform transform;
    std::string notice;
};
class ExportColorProfiles {
public:
    ExportColorProfiles();
    // Explicit locations make discovery/default/import behavior independently testable.
    ExportColorProfiles(std::string data_dir, std::string user_dir,
                        std::vector<std::string> search_dirs, std::string override_path = {});
    std::vector<ExportColorProfile> catalog(std::string *notice = nullptr) const;
    bool add(std::string const &path, ExportColorProfile &profile, std::string &error) const;
    ExportColorProfile resolve(std::string const &id, std::string &notice) const;
    ExportColorProfile selected(std::string &notice) const;
    void select(ExportColorProfile const &profile) const;
    static ExportColorProfile srgb();
    static bool read_bytes(std::string const &path, std::vector<unsigned char> const &bytes, ExportColorProfile &profile, std::string &error);
    static bool read(std::string const &path, ExportColorProfile &profile, std::string &error);
private:
    ExportColorProfile default_profile(std::string &notice) const;
    std::string _data, _user, _override;
    std::vector<std::string> _dirs;
};
// Future PNG/JPEG/WebP writers call this once and use the returned exact bytes
// for embedding and the transform for pixels. No temporary profile file needed.
PreparedExportProfile prepare_export_color_profile();
} // namespace Inkscape::IO
#endif
