// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/export-color-profiles.h"
#include "io/tiff-export.h"
#include "preferences.h"
#include "document.h"
#include "inkscape.h"
#include "colors/color.h"
#include "helper/png-write.h"
#include <2geom/rect.h>
#include <span>
#include <gtest/gtest.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <filesystem>
#include <fstream>
#include <png.h>
#include <tiffio.h>

using namespace Inkscape::IO;
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

class ExportColorProfilesTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto p = g_dir_make_tmp("vacards-icc-XXXXXX", nullptr);
        ASSERT_NE(p, nullptr); dir = p; g_free(p);
        saved = Inkscape::Preferences::get()->getString(EXPORT_PROFILE_PREFERENCE);
        Inkscape::Preferences::get()->setString(EXPORT_PROFILE_PREFERENCE, "");
    }
    void TearDown() override {
        Inkscape::Preferences::get()->setString(EXPORT_PROFILE_PREFERENCE, saved);
        std::filesystem::remove_all(dir);
    }
    std::string path(std::string name) const { return (std::filesystem::path(dir) / name).make_preferred().string(); }
    ExportColorProfiles profiles() const { return {path("data"), path("user/color/icc"), {path("installed")}}; }
    std::string write(std::string name, bool cmyk = false, double gamma = 1.8) {
        auto file = path(name); std::filesystem::create_directories(std::filesystem::path(file).parent_path());
        cmsHPROFILE p;
        if (cmyk) p = cmsCreateInkLimitingDeviceLink(cmsSigCmykData, 300);
        else {
            cmsCIExyY white; cmsWhitePointFromTemp(&white, 6504);
            cmsCIExyYTRIPLE primaries = {{0.64,0.33,1},{0.30,0.60,1},{0.15,0.06,1}};
            auto curve = cmsBuildGamma(nullptr, gamma);
            cmsToneCurve *curves[] = {curve,curve,curve};
            p = cmsCreateRGBProfile(&white,&primaries,curves); cmsFreeToneCurve(curve);
            cmsSetDeviceClass(p, cmsSigOutputClass);
        }
        EXPECT_NE(p, nullptr);
        EXPECT_TRUE(cmsSaveProfileToFile(p,file.c_str())); cmsCloseProfile(p); return file;
    }
    std::string dir, saved;
};
TEST_F(ExportColorProfilesTest, CatalogIncludesBuiltinAndFiltersInstalledProfiles) {
    auto rgb = write("installed/rgb.icc"); write("installed/cmyk.icc",true);
    auto list = profiles().catalog(); ASSERT_EQ(list.size(),2u);
    EXPECT_EQ(list.front().id(),"srgb"); EXPECT_FALSE(list.front().bytes.empty());
    EXPECT_EQ(list.back().path,rgb);
    std::string error; EXPECT_TRUE(export_color_transform(list.front().bytes,error)) << error;
}
TEST_F(ExportColorProfilesTest, CatalogMatchesMixedCaseSuffixes) {
    for (auto name : {"a.ICC", "b.ICM", "nested/c.iCc", "nested/d.iCm"})
        write(std::string("installed/") + name);
    write("installed/ignored.icc.txt");
    std::string notice;
    auto list = profiles().catalog(&notice);
    EXPECT_EQ(list.size(), 5u);
    EXPECT_TRUE(notice.empty());
}
TEST_F(ExportColorProfilesTest, UnreadableDirectoriesLeaveSiblingsAndNotice) {
    auto good = write("installed/good.icc");
    auto empty = path("installed/empty");
    std::filesystem::create_directory(empty);
    write("installed/blocked/hidden.icc");
    write("blocked-root/hidden.icc");
    auto nested = path("installed/blocked"), root = path("blocked-root");
#ifdef _WIN32
    DenyDirectoryRead deny_nested(nested), deny_root(root);
    ASSERT_TRUE(deny_nested.apply());
    ASSERT_TRUE(deny_root.apply());
#else
    struct RestorePermissions {
        std::string a, b;
        ~RestorePermissions() { g_chmod(a.c_str(), 0700); g_chmod(b.c_str(), 0700); }
    } restore{nested, root};
    ASSERT_EQ(g_chmod(nested.c_str(), 0000), 0);
    ASSERT_EQ(g_chmod(root.c_str(), 0000), 0);
#endif
    ExportColorProfiles catalog(path("data"), path("user"), {root, path("installed")});
    std::string notice;
    std::vector<ExportColorProfile> list;
    EXPECT_NO_THROW(list = catalog.catalog(&notice));
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list.back().path, good);
    EXPECT_NE(notice.find(root), std::string::npos);
    EXPECT_NE(notice.find(nested), std::string::npos);
    EXPECT_EQ(notice.find(empty), std::string::npos) << notice;
    catalog.select(list.front());
    std::string selected_notice;
    EXPECT_EQ(catalog.selected(selected_notice).id(), "srgb");
}
TEST_F(ExportColorProfilesTest, ImportsCopiesDeduplicatesAndHandlesSameNames) {
    auto source = write("one/same.icc"); ExportColorProfile a,b,c; std::string error;
    ASSERT_TRUE(profiles().add(source,a,error)) << error;
    EXPECT_NE(a.path,source); EXPECT_EQ(a.path.find(path("user/color/icc/")),0u);
    ASSERT_TRUE(profiles().add(source,b,error)); EXPECT_EQ(a.id(),b.id());
    auto renamed=path("renamed.icm"); std::filesystem::copy_file(source,renamed);
    ASSERT_TRUE(profiles().add(renamed,b,error)); EXPECT_EQ(a.id(),b.id());
    ASSERT_TRUE(profiles().add(write("two/same.icc",false,2.4),c,error)); EXPECT_NE(a.id(),c.id());
    std::filesystem::remove(source);
    ASSERT_TRUE(ExportColorProfiles::read(a.path,b,error)); EXPECT_EQ(a.bytes,b.bytes);
    EXPECT_EQ(profiles().catalog().size(),3u);
}
TEST_F(ExportColorProfilesTest, RejectsCmykBadHeaderUnsupportedClassAndOversize) {
    ExportColorProfile p; std::string error;
    EXPECT_FALSE(profiles().add(write("cmyk.icc",true),p,error)); EXPECT_FALSE(error.empty());
    auto file=write("bad.icc");
    { std::fstream f(file,std::ios::in|std::ios::out|std::ios::binary); f.seekp(36); f.write("bad!",4); }
    EXPECT_FALSE(profiles().add(file,p,error));
    auto input=cmsCreate_sRGBProfile(); cmsSetDeviceClass(input,cmsSigInputClass);
    ASSERT_TRUE(cmsSaveProfileToFile(input,path("input.icc").c_str())); cmsCloseProfile(input);
    EXPECT_FALSE(profiles().add(path("input.icc"),p,error));
    auto huge=path("huge.icc");
    { std::ofstream f(huge,std::ios::binary); f.seekp(std::uint64_t(EXPORT_PROFILE_MAX_SIZE)); f.put('x'); }
    auto preference = Inkscape::Preferences::get()->getString(EXPORT_PROFILE_PREFERENCE);
    EXPECT_FALSE(profiles().add(huge,p,error));
    EXPECT_NE(error.find("15 MiB"), std::string::npos);
    EXPECT_EQ(Inkscape::Preferences::get()->getString(EXPORT_PROFILE_PREFERENCE), preference);
    EXPECT_FALSE(std::filesystem::exists(path("user/color/icc")));
}
TEST_F(ExportColorProfilesTest, AddRejectsValidRgbProfileAboveSharedLimit) {
    auto file = write("large.icc");
    // Keep the source in memory so saving the enlarged profile never rewrites
    // an open source file (Windows sharing semantics forbid that).
    std::ifstream input(file, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    auto handle = cmsOpenProfileFromMem(bytes.data(), bytes.size());
    ASSERT_NE(handle, nullptr);
    std::unique_ptr<void, decltype(&cmsCloseProfile)> close_handle(handle, &cmsCloseProfile);
    cmsUInt32Number base_size = 0;
    ASSERT_TRUE(cmsSaveProfileToMem(handle, nullptr, &base_size));
    // A real extra private ICC tag, not a corrupt length or sparse-file shortcut.
    auto size = EXPORT_PROFILE_MAX_SIZE + 4;
    std::vector<unsigned char> tag(size - base_size - 12, 0);
    std::memcpy(tag.data(), "text", 4);
    ASSERT_TRUE(cmsWriteRawTag(handle, static_cast<cmsTagSignature>(0x76616361), tag.data(), tag.size()));
    ASSERT_TRUE(cmsSaveProfileToFile(handle, file.c_str()));
    close_handle.reset();
    ASSERT_EQ(std::filesystem::file_size(file), size);
    handle = cmsOpenProfileFromFile(file.c_str(), "r");
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(cmsGetColorSpace(handle), cmsSigRgbData);
    cmsCloseProfile(handle);
    ExportColorProfile selected = ExportColorProfiles::srgb();
    profiles().select(selected);
    auto preference = Inkscape::Preferences::get()->getString(EXPORT_PROFILE_PREFERENCE);
    std::string error;
    EXPECT_FALSE(profiles().add(file, selected, error));
    EXPECT_NE(error.find("15 MiB"), std::string::npos);
    EXPECT_EQ(selected.id(), "srgb");
    EXPECT_EQ(Inkscape::Preferences::get()->getString(EXPORT_PROFILE_PREFERENCE), preference);
    EXPECT_FALSE(std::filesystem::exists(path("user/color/icc")));
}
TEST_F(ExportColorProfilesTest, WebpWithoutMuxAbortsBeforeOpeningOutput) {
    auto scripts = (std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "share/extensions").string();
    std::string script = R"PY(
import contextlib, io, sys
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, sys.argv[1])
from PIL import Image, ImageCms
import inkex, raster_output_webp as webp
root = Path(sys.argv[2])
image = Image.new('RGB', (2, 2), (204, 153, 51))
image.save(root/'tagged.png', icc_profile=ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes())
for version, feature in [('9.5.0', 'webp_mux'), ('12.3.0', 'webp')]:
    for exists in (False, True):
        output = root / ('output-%s-%s.webp' % (version, exists))
        if exists: output.write_bytes(b'preserve existing output')
        with patch.object(webp, 'pillow_version', version), patch.object(webp.features, 'check', return_value=False) as check:
            message = io.StringIO()
            with contextlib.redirect_stderr(message):
                try:
                    webp.WebpOutput().run([str(root/'tagged.png'), '--output='+str(output)])
                except SystemExit as error:
                    assert error.code != 0
                else:
                    raise AssertionError('missing mux did not abort')
            check.assert_called_once_with(feature)
            assert 'WebP mux support' in message.getvalue(), message.getvalue()
        assert output.read_bytes() == b'preserve existing output' if exists else not output.exists()
    # A caller that supplies its own image must also fail without writing.
    extension = webp.WebpOutput()
    extension.img = image.copy()
    extension.img.info['icc_profile'] = b'profile'
    stream = io.BytesIO()
    with patch.object(webp, 'pillow_version', version), patch.object(webp.features, 'check', return_value=False):
        try: extension.save(stream)
        except inkex.AbortExtension: pass
        else: raise AssertionError('direct save did not abort')
    assert stream.getvalue() == b''
# ICC-free output still works on older WebP encoders without mux.
image.save(root/'untagged.png')
with patch.object(webp, 'pillow_version', '9.5.0'), patch.object(webp.features, 'check', return_value=False) as check:
    webp.WebpOutput().run([str(root/'untagged.png'), '--output='+str(root/'untagged.webp')])
    check.assert_not_called()
assert Image.open(root/'untagged.webp').info.get('icc_profile') is None
print('Missing mux: no new output, existing output preserved, direct stream empty; ICC-free export passed')
)PY";
    auto override = g_getenv("ICC_TEST_PYTHON");
    std::string python = override ? override : "python3";
    std::vector<char *> argv{python.data(), const_cast<char *>("-B"), const_cast<char *>("-c"),
                            script.data(), scripts.data(), dir.data(), nullptr};
    gchar *out = nullptr, *err = nullptr; gint status = -1;
    ASSERT_TRUE(g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, &out, &err, &status, nullptr));
    EXPECT_EQ(status, 0) << (err ? err : "");
    if (out) std::cout << out;
    g_free(out); g_free(err);
}
TEST_F(ExportColorProfilesTest, PreferenceRoundTripsAndChangedOrMissingChoiceHasNotice) {
    ExportColorProfile p; std::string error,notice;
    ASSERT_TRUE(profiles().add(write("choice.icc"),p,error)); profiles().select(p);
    EXPECT_EQ(profiles().selected(notice).id(),p.id()); EXPECT_TRUE(notice.empty());
    auto other=write("other.icc",false,2.4);
    std::filesystem::copy_file(other,p.path,std::filesystem::copy_options::overwrite_existing);
    EXPECT_EQ(profiles().selected(notice).id(),"srgb"); EXPECT_FALSE(notice.empty());
    std::filesystem::remove(p.path);
    EXPECT_EQ(profiles().selected(notice).id(),"srgb"); EXPECT_FALSE(notice.empty());
    profiles().select(ExportColorProfiles::srgb());
    EXPECT_EQ(profiles().selected(notice).id(),"srgb"); EXPECT_TRUE(notice.empty());
    EXPECT_EQ(profiles().resolve("invalid",notice).id(),"srgb"); EXPECT_FALSE(notice.empty());
}
TEST_F(ExportColorProfilesTest, DefaultUsesOnlyBundledTheBestOrBuiltinAndHonorsOverride) {
    std::string notice;
    write("installed/TheBest.icc");
    EXPECT_EQ(profiles().resolve("",notice).id(),"srgb"); EXPECT_TRUE(notice.empty());
    auto bundle=write("data/inkscape/color/icc/TheBest.icc");
    EXPECT_EQ(profiles().resolve("",notice).path,bundle);
    auto override=write("override.icc",false,2.4);
    ExportColorProfiles explicit_override(path("data"),path("user"),{},override);
    EXPECT_EQ(explicit_override.resolve("",notice).path,override);
    EXPECT_EQ(explicit_override.resolve("srgb",notice).id(),"srgb");
    std::filesystem::remove(override);
    EXPECT_EQ(explicit_override.resolve("",notice).path,bundle); EXPECT_FALSE(notice.empty());
    { std::ofstream f(bundle); f << "broken"; }
    EXPECT_EQ(profiles().resolve("",notice).id(),"srgb"); EXPECT_FALSE(notice.empty());
}
TEST_F(ExportColorProfilesTest, TiffEmbedsSelectedExactBytesAndConvertsPixels) {
    ExportColorProfile custom; std::string error;
    ASSERT_TRUE(profiles().add(write("custom.icc"),custom,error));
    std::vector<unsigned char> input={204,153,51,255,32,128,224,128};
    png_image png{}; png.version=PNG_IMAGE_VERSION; png.width=2; png.height=1; png.format=PNG_FORMAT_RGBA;
    ASSERT_TRUE(png_image_write_to_file(&png,path("in.png").c_str(),0,input.data(),0,nullptr));
    for (auto const &p : {ExportColorProfiles::srgb(),custom}) {
        profiles().select(p); std::string notice; auto selected=profiles().selected(notice);
        ASSERT_TRUE(export_png_to_color_managed_tiff(path("in.png"),path("out.tiff"),selected.bytes,error)) << error;
        auto raw=TIFFOpen(path("out.tiff").c_str(),"r"); ASSERT_NE(raw,nullptr);
        std::unique_ptr<TIFF,decltype(&TIFFClose)> tiff(raw,&TIFFClose);
        std::uint32_t size=0; void *data=nullptr;
        ASSERT_TRUE(TIFFGetField(raw,TIFFTAG_ICCPROFILE,&size,&data)); ASSERT_EQ(size,p.bytes.size());
        EXPECT_EQ(std::memcmp(data,p.bytes.data(),size),0);
        std::vector<unsigned char> actual(8),expected(8);
        ASSERT_GE(TIFFReadScanline(raw,actual.data(),0,0),0);
        auto src=cmsCreate_sRGBProfile(); auto dst=cmsOpenProfileFromMem(p.bytes.data(),p.bytes.size());
        auto transform=cmsCreateTransform(src,TYPE_RGBA_8,dst,TYPE_RGBA_8,INTENT_RELATIVE_COLORIMETRIC,
            cmsFLAGS_BLACKPOINTCOMPENSATION|cmsFLAGS_COPY_ALPHA);
        ASSERT_NE(transform,nullptr); cmsDoTransform(transform,input.data(),expected.data(),2);
        cmsDeleteTransform(transform); cmsCloseProfile(src); cmsCloseProfile(dst);
        EXPECT_EQ(actual,expected);
        if (p.id()=="srgb") EXPECT_EQ(actual,input); else EXPECT_NE(actual,input);
    }
}

// Read stored samples without color management: the transform oracle below is
// independent of the writer, and the ICC payload is compared byte-for-byte.
struct ReadPng {
    std::vector<unsigned char> pixels, icc;
    bool srgb = false;
};
ReadPng read_png(std::string const &file) {
    ReadPng out;
    auto fp = fopen(file.c_str(), "rb");
    if (!fp) return out;
    auto png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    auto info = png_create_info_struct(png);
    if (!setjmp(png_jmpbuf(png))) {
        png_init_io(png, fp); png_read_info(png, info);
        png_charp name; int compression; png_bytep bytes; png_uint_32 size;
        if (png_get_iCCP(png, info, &name, &compression, &bytes, &size)) out.icc.assign(bytes, bytes + size);
        int intent; out.srgb = png_get_sRGB(png, info, &intent);
        if (png_get_bit_depth(png, info) == 16) png_set_strip_16(png);
        if (!(png_get_color_type(png, info) & PNG_COLOR_MASK_ALPHA)) png_set_add_alpha(png, 255, PNG_FILLER_AFTER);
        png_read_update_info(png, info);
        auto stride = png_get_rowbytes(png, info);
        auto height = png_get_image_height(png, info);
        out.pixels.resize(stride * height);
        std::vector<png_bytep> rows(height);
        for (unsigned y = 0; y < height; ++y) rows[y] = out.pixels.data() + y * stride;
        png_read_image(png, rows.data()); png_read_end(png, info);
    }
    png_destroy_read_struct(&png, &info, nullptr); fclose(fp);
    return out;
}
std::string file_bytes(std::string const &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
TEST_F(ExportColorProfilesTest, PngSrgbUnchangedCustomExactProfileAndConvertedSamples) {
    if (!Inkscape::Application::exists()) Inkscape::Application::create(false);
    std::string xml = R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="80"><rect width="8" height="80" fill="#cc9933"/><rect x="8" width="8" height="80" fill="#2080e0" fill-opacity="0.5"/></svg>)";
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(xml.data(), xml.size()));
    ASSERT_TRUE(doc);
    auto render = [&](std::string const &name, PreparedExportProfile const *profile, int type = 6, int depth = 8, bool interlace = false, bool embed_srgb = false) {
        return sp_export_png_file(doc.get(), path(name).c_str(), Geom::Rect(Geom::Point(0,0), Geom::Point(16,80)),
            16, 80, 96, 96, Inkscape::Colors::Color(0x00000000), nullptr, nullptr, true, {}, interlace, type, depth, 6, 2, profile, embed_srgb);
    };
    profiles().select(ExportColorProfiles::srgb());
    auto srgb = prepare_export_color_profile();
    ASSERT_EQ(render("old.png", nullptr), EXPORT_OK);
    ASSERT_EQ(render("srgb.png", &srgb), EXPORT_OK);
    EXPECT_EQ(file_bytes(path("old.png")), file_bytes(path("srgb.png")));
    auto input = read_png(path("old.png"));
    ASSERT_EQ(input.pixels.size(), 16u * 80 * 4);
    EXPECT_TRUE(input.icc.empty()); EXPECT_FALSE(input.srgb);
    ExportColorProfile custom; std::string error;
    ASSERT_TRUE(profiles().add(write("custom.icc"), custom, error));
    profiles().select(custom); auto prepared = prepare_export_color_profile();
    ASSERT_EQ(prepared.profile.bytes, custom.bytes);
    ASSERT_EQ(render("internal.png", nullptr), EXPORT_OK);
    EXPECT_EQ(file_bytes(path("internal.png")), file_bytes(path("old.png")))
        << "TIFF, print and clipboard intermediates remain unconverted despite the saved profile";
    std::vector<unsigned char> expected(input.pixels.size());
    auto src = cmsCreate_sRGBProfile(); auto dst = cmsOpenProfileFromMem(custom.bytes.data(), custom.bytes.size());
    auto transform = cmsCreateTransform(src, TYPE_RGBA_8, dst, TYPE_RGBA_8, INTENT_RELATIVE_COLORIMETRIC,
        cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_COPY_ALPHA);
    ASSERT_NE(transform, nullptr);
    cmsDoTransform(transform, input.pixels.data(), expected.data(), 16 * 80);
    cmsDeleteTransform(transform); cmsCloseProfile(src); cmsCloseProfile(dst);
    EXPECT_NE(expected, input.pixels);
    for (auto depth : {8, 16}) for (bool interlace : {false, true}) {
        ASSERT_EQ(render("custom.png", &prepared, 6, depth, interlace), EXPORT_OK);
        auto output = read_png(path("custom.png"));
        EXPECT_EQ(output.icc, custom.bytes); EXPECT_FALSE(output.srgb);
        EXPECT_EQ(output.pixels, expected);
    }
    // RGB output also receives the profile, while grayscale fails before touching a file.
    ASSERT_EQ(render("rgb.png", &prepared, 2), EXPORT_OK);
    EXPECT_EQ(read_png(path("rgb.png")).icc, custom.bytes);
    { std::ofstream f(path("gray.png")); f << "preserve"; }
    EXPECT_EQ(render("gray.png", &prepared, 0), EXPORT_ERROR);
    EXPECT_EQ(file_bytes(path("gray.png")), "preserve");

    ASSERT_EQ(render("tagged-srgb.png", &srgb, 6, 8, false, true), EXPORT_OK);
    auto tagged_srgb = read_png(path("tagged-srgb.png"));
    EXPECT_EQ(tagged_srgb.icc, srgb.profile.bytes);
    EXPECT_EQ(tagged_srgb.pixels, input.pixels);
    EXPECT_FALSE(tagged_srgb.srgb);

    // Exercise the actual Python extension save methods, then reopen JPEG/WebP.
    // The runner supplies explicit Python and extension source identities.
    auto python_override = g_getenv("ICC_TEST_PYTHON");
    auto extensions_override = g_getenv("ICC_TEST_EXTENSIONS");
    std::string python = python_override ? python_override : "python3";
    auto extensions = extensions_override ? std::filesystem::path(extensions_override) :
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "share/extensions";
    std::string script = R"PY(
import io, sys
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0, sys.argv[1])
from PIL import Image
from raster_output_jpg import JpegOutput
from raster_output_webp import WebpOutput
root = Path(sys.argv[2])
for source in ('custom.png', 'srgb.png', 'tagged-srgb.png'):
    image = Image.open(root / source)
    for cls, suffix, options in ((JpegOutput, 'jpg', dict(quality=100, progressive=False)),
                                 (WebpOutput, 'webp', dict(quality=100, speed=0, lossless=True))):
        extension = cls()
        extension.img = image
        extension.options = SimpleNamespace(**options)
        target = root / (source + '.' + suffix)
        with target.open('wb') as stream: extension.save(stream)
        actual = Image.open(target)
        assert actual.info.get('icc_profile') == image.info.get('icc_profile')
        if suffix == 'webp': assert actual.convert('RGBA').tobytes() == image.convert('RGBA').tobytes()
        else:
            # Lossy JPEG sample check on the center of each solid block.
            for xy in ((3, 20), (12, 20)):
                assert max(abs(a-b) for a,b in zip(actual.getpixel(xy), image.convert('RGB').getpixel(xy))) <= 5
)PY";
    // path::c_str() is wide on Windows; argv needs narrow strings.
    auto extensions_arg = extensions.string();
    std::vector<char *> argv{python.data(), const_cast<char *>("-c"), script.data(),
        extensions_arg.data(), dir.data(), nullptr};
    gchar *out = nullptr, *err = nullptr; gint status = -1; GError *spawn_error = nullptr;
    ASSERT_TRUE(g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, &out, &err, &status, &spawn_error));
    EXPECT_EQ(status, 0) << (err ? err : "");
    g_free(out); g_free(err); g_clear_error(&spawn_error);
}
TEST_F(ExportColorProfilesTest, AdmittedBytesDoNotReopenProfilePath)
{
    auto original=ExportColorProfiles::srgb(); ExportColorProfile parsed; std::string error;
    ASSERT_TRUE(ExportColorProfiles::read_bytes(path("absent#profile.icc"),original.bytes,parsed,error))<<error;
    EXPECT_EQ(parsed.bytes,original.bytes);EXPECT_EQ(parsed.sha256,original.sha256);
    auto bad=original.bytes;bad[36]=0;
    EXPECT_FALSE(ExportColorProfiles::read_bytes(path("absent.icc"),bad,parsed,error));
}
} // namespace
