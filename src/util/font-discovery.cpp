// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Author:
 *   Michael Kowalski
 *
 * Copyright (C) 2022-2024 Michael Kowalski
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "font-discovery.h"
#include "async/progress.h"
#include <sigc++/scoped_connection.h>
#include "inkscape-application.h"
#include "io/resource.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cairo-ft.h>
#include <cairomm/surface.h>
#include <glibmm/ustring.h>
#include <iostream>
#include <libnrtype/font-factory.h>
#include <libnrtype/font-instance.h>
#include <glibmm/keyfile.h>
#include <glibmm/miscutils.h>
#include <glib/gstdio.h>
#include <memory>
#include <pango/pango-fontmap.h>
#include <pangomm/fontdescription.h>
#include <pangomm/fontmap.h>
#include <set>
#include <sigc++/connection.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace filesystem = std::filesystem;

namespace Inkscape {

Glib::ustring font_family_name(FontInfo const &font)
{
    if (!font.family_name.empty()) return font.family_name;
    return font.ff ? font.ff->get_name() : Glib::ustring{};
}

Glib::ustring font_face_name(FontInfo const &font)
{
    if (!font.face_name.empty()) return font.face_name;
    return font.face ? font.face->get_name() : Glib::ustring{};
}

Glib::ustring font_description_string(FontInfo const &font)
{
    if (!font.description.empty()) return font.description;
    return get_font_description(font.ff, font.face).to_string();
}

Glib::ustring font_specification(FontInfo const &font)
{
    if (!font.variations.empty()) {
        return get_fontspec(font_family_name(font), font_face_name(font), font.variations);
    }
    if (!font.fontspec.empty()) return font.fontspec;
    return get_inkscape_fontspec(font.ff, font.face, font.variations);
}

// Attempt to estimate how heavy given typeface is by drawing some capital letters and counting
// black pixels (alpha channel). This is imperfect, but reasonable proxy for font weight, as long
// as Pango can instantiate correct font.
double calculate_font_weight(Pango::FontDescription& desc, double caps_height) {
    // pixmap with enough room for a few characters; the rest will be cropped
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 128, 64);
    auto context = Cairo::Context::create(surface);
    auto layout = Pango::Layout::create(context);
    const char* txt = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    layout->set_text(txt);
    auto size = 22 * PANGO_SCALE;
    if (caps_height > 0) {
        size /= caps_height;
    }
    desc.set_size(size);
    layout->set_font_description(desc);
    context->move_to(1, 1);
    layout->show_in_cairo_context(context);
    surface->flush();

    auto pixels = surface->get_data();
    auto width = surface->get_width();
    auto stride = surface->get_stride() / width;
    auto height = surface->get_height();
    double sum = 0;
    for (auto y = 0; y < height; ++y) {
        for (auto x = 0; x < width; ++x) {
            sum += pixels[3]; // read alpha
            pixels += stride;
        }
    }
    auto weight = sum / (width * height);
    return weight;
}

// calculate width of a A-Z string to try to measure average character width
double calculate_font_width(Pango::FontDescription& desc) {
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 1, 1);
    auto context = Cairo::Context::create(surface);
    auto layout = Pango::Layout::create(context);
    const char* txt = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    layout->set_text(txt);
    desc.set_size(72 * PANGO_SCALE);
    layout->set_font_description(desc);
    // layout->show_in_cairo_context(context);
    Pango::Rectangle ink, rect;
    layout->get_extents(ink, rect);
    return static_cast<double>(ink.get_width()) / PANGO_SCALE / strlen(txt);
}

// construct font name from Pango face and family;
// return font name as it is recorded in the font itself, as far as Pango allows it
Glib::ustring get_full_font_name(Glib::RefPtr<Pango::FontFamily> ff, Glib::RefPtr<Pango::FontFace> face) {
    if (!ff) return "";

    auto family = ff->get_name();
    auto face_name = face ? face->get_name() : Glib::ustring();
    auto name = face_name.empty() ? family : family + ' ' + face_name;
    return name;
}


// calculate value to order font's styles
int get_font_style_order(const Pango::FontDescription& desc) {
    return
        static_cast<int>(desc.get_weight())  * 1'000'000 +
        static_cast<int>(desc.get_style())   * 10'000 +
        static_cast<int>(desc.get_stretch()) * 100 +
        static_cast<int>(desc.get_variant());
}

// sort fonts in-place by name using lexicographical order; if 'sans_first' is true place "Sans" font first
void sort_fonts_by_name(std::vector<FontInfo>& fonts, bool sans_first) {
    std::sort(begin(fonts), end(fonts), [=](const FontInfo& a, const FontInfo& b) {
        auto na = font_family_name(a);
        auto nb = font_family_name(b);
        if (sans_first) {
            bool sans_a = a.synthetic && na == "Sans";
            bool sans_b = b.synthetic && nb == "Sans";
            if (sans_a != sans_b) {
                return sans_a;
            }
        }
        // check family names first
        if (na != nb) {
            // lexicographical order:
            return na < nb;
            // alphabetical order:
            //return na.raw() < nb.raw();
        }
        Pango::FontDescription da(font_description_string(a));
        Pango::FontDescription db(font_description_string(b));
        return get_font_style_order(da) < get_font_style_order(db);
    });
}

// sort fonts in requested order, in-place
void sort_fonts(std::vector<FontInfo>& fonts, FontOrder order, bool sans_first) {
    switch (order) {
        case FontOrder::ByName:
        case FontOrder::ByFamily:
            sort_fonts_by_name(fonts, sans_first);
            break;

        case FontOrder::ByWeight:
            // there are many repetitions for weight, due to font substitutions, so sort by name first
            sort_fonts_by_name(fonts, sans_first);
            std::stable_sort(begin(fonts), end(fonts), [](const FontInfo& a, const FontInfo& b) { return a.weight < b.weight; });
            break;

        case FontOrder::ByWidth:
            sort_fonts_by_name(fonts, sans_first);
            std::stable_sort(begin(fonts), end(fonts), [](const FontInfo& a, const FontInfo& b) { return a.width < b.width; });
            break;

        default:
            g_warning("Missing case in sort_fonts");
            break;
    }
}

const FontInfo& get_family_font(const std::vector<FontInfo>& family) {
    assert(!family.empty());
    auto it = std::ranges::find_if(family, [](auto& fam) {
        auto const name = font_face_name(fam).raw();
        return name.find("Regular") != std::string::npos ||
               name.find("Normal")  != std::string::npos;
    });
    if (it != end(family)) {
        return *it;
    }
    return family.front();
}

FontInfo& get_family_font(std::vector<FontInfo>& family) {
    return const_cast<FontInfo&>(get_family_font(const_cast<const std::vector<FontInfo>&>(family)));
}

void sort_font_families(std::vector<std::vector<FontInfo>>& fonts, bool sans_first) {
    std::sort(begin(fonts), end(fonts), [=](const auto& a, const auto& b) {
        auto& f1 = get_family_font(a);
        auto& f2 = get_family_font(b);

        auto na = font_family_name(f1);
        auto nb = font_family_name(f2);
        if (sans_first) {
            bool sans_a = f1.synthetic && na == "Sans";
            bool sans_b = f2.synthetic && nb == "Sans";
            if (sans_a != sans_b) {
                return sans_a;
            }
        }
        // lexicographical order:
        return na < nb;
        // alphabetical order:
        //return na.raw() < nb.raw();
    });
}

Glib::ustring get_fontspec(const Glib::ustring& family, const Glib::ustring& face, const Glib::ustring& variations) {
    if (variations.empty()) {
        return face.empty() ? family : family + ", " + face;
    }
    else {
        auto desc = (face.empty() ? family : family + ", " + face) + " " + variations;
        return desc;
    }
}

Glib::ustring get_fontspec(const Glib::ustring& family, const Glib::ustring& face) {
    return get_fontspec(family, face, Glib::ustring());
}

Glib::ustring get_face_style(const Pango::FontDescription& desc) {
    Pango::FontDescription copy(desc);
    copy.unset_fields(Pango::FontMask::FAMILY);
    copy.unset_fields(Pango::FontMask::SIZE);
    auto str = copy.to_string();
    return str;
}

Glib::ustring get_inkscape_fontspec(const Glib::RefPtr<Pango::FontFamily>& ff, const Glib::RefPtr<Pango::FontFace>& face, const Glib::ustring& variations) {
    if (!ff) return Glib::ustring();

    return get_fontspec(ff->get_name(), face ? get_face_style(face->describe()) : Glib::ustring(), variations);
}

Pango::FontDescription get_font_description(const Glib::RefPtr<Pango::FontFamily>& ff, const Glib::RefPtr<Pango::FontFace>& face) {
    if (!face) return Pango::FontDescription("sans serif");

    auto desc = face->describe();
    desc.unset_fields(Pango::FontMask::SIZE);
    return desc;
}

// Font cache is a text file that stores under each font name some of its metadata, like average weight and height,
// as well as flags (monospaced, variable, oblique, synthetic font). It is kept to speed up font metadata discovery.
const char font_cache[] = "font-cache.ini";
const char cache_header[] = "@font-cache@";
constexpr auto cache_version = 1.0;
const char font_catalog[] = "font-catalog-v2.ini";
const char catalog_header[] = "@font-catalog@";
constexpr int catalog_version = 2;
enum FontCacheFlags : int {
    Normal = 0,
    Monospace = 0x01,
    Oblique   = 0x02,
    Variable  = 0x04,
    Synthetic = 0x08,
};

std::string make_font_id(FontInfo const &font)
{
    auto const source = font.source_path + '\x1f' + font_family_name(font).raw() +
                        '\x1f' + font_face_name(font).raw() +
                        '\x1f' + font_description_string(font).raw() +
                        '\x1f' + font_specification(font).raw() +
                        '\x1f' + font.variations.raw();
    auto checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, reinterpret_cast<guchar const *>(source.data()), source.size());
    auto id = std::string{g_checksum_get_string(checksum)};
    g_checksum_free(checksum);
    return id;
}

bool FontCatalog::save(std::string const &filename, FontFamilies const &fonts,
                       std::string const &manifest)
{
    auto keyfile = Glib::KeyFile::create();
    keyfile->set_integer(catalog_header, "version", catalog_version);
    keyfile->set_string(catalog_header, "manifest", manifest);
    keyfile->set_integer(catalog_header, "pango-version", pango_version());
    keyfile->set_integer(catalog_header, "fontconfig-version", FcGetVersion());
    std::size_t count = 0;
    std::unordered_set<std::string> saved_ids;

    for (auto const &family : fonts) {
        for (auto font : family) {
            if (font.id.empty()) font.id = make_font_id(font);
            if (!saved_ids.emplace(font.id).second) continue;
            auto const group = "font-" + font.id;
            keyfile->set_string(group, "family", font_family_name(font));
            keyfile->set_string(group, "face", font_face_name(font));
            keyfile->set_string(group, "description", font_description_string(font));
            keyfile->set_string(group, "fontspec", font_specification(font));
            keyfile->set_string(group, "variations", font.variations);
            keyfile->set_string(group, "source", font.source_path);
            keyfile->set_uint64(group, "source-size", font.source_size);
            keyfile->set_int64(group, "source-mtime", font.source_mtime);
            keyfile->set_double(group, "weight", font.weight);
            keyfile->set_double(group, "width", font.width);
            keyfile->set_integer(group, "family-kind", font.family_kind);
            int flags = FontCacheFlags::Normal;
            if (font.monospaced) flags |= FontCacheFlags::Monospace;
            if (font.oblique) flags |= FontCacheFlags::Oblique;
            if (font.variable_font) flags |= FontCacheFlags::Variable;
            if (font.synthetic) flags |= FontCacheFlags::Synthetic;
            keyfile->set_integer(group, "flags", flags);
            ++count;
        }
    }
    keyfile->set_uint64(catalog_header, "faces", count);

    auto const data = keyfile->to_data();
    GError *error = nullptr;
    auto const flags = static_cast<GFileSetContentsFlags>(G_FILE_SET_CONTENTS_CONSISTENT |
                                                          G_FILE_SET_CONTENTS_DURABLE);
    if (!g_file_set_contents_full(filename.c_str(), data.data(), data.bytes(), flags, 0600, &error)) {
        g_warning("Cannot save font catalog %s: %s", filename.c_str(), error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }
    return true;
}

std::shared_ptr<const FontFamilies>
FontCatalog::load(std::string const &filename, std::string const &manifest)
{
    try {
        auto keyfile = Glib::KeyFile::create();
        if (!g_file_test(filename.c_str(), G_FILE_TEST_IS_REGULAR) || !keyfile->load_from_file(filename)) {
            return {};
        }
        if (keyfile->get_integer(catalog_header, "version") != catalog_version ||
            keyfile->get_string(catalog_header, "manifest") != manifest ||
            keyfile->get_integer(catalog_header, "pango-version") != pango_version() ||
            keyfile->get_integer(catalog_header, "fontconfig-version") != FcGetVersion()) {
            return {};
        }

        auto result = std::make_shared<FontFamilies>();
        std::unordered_map<std::string, std::size_t> family_indices;
        std::size_t face_count = 0;
        for (auto const &group : keyfile->get_groups()) {
            if (group == catalog_header || group.compare(0, 5, "font-") != 0) continue;
            FontInfo font;
            font.id = group.substr(5).raw();
            font.family_name = keyfile->get_string(group, "family");
            font.face_name = keyfile->get_string(group, "face");
            font.description = keyfile->get_string(group, "description");
            font.fontspec = keyfile->get_string(group, "fontspec");
            font.variations = keyfile->get_string(group, "variations");
            font.source_path = keyfile->get_string(group, "source");
            font.source_size = keyfile->get_uint64(group, "source-size");
            font.source_mtime = keyfile->get_int64(group, "source-mtime");
            font.weight = keyfile->get_double(group, "weight");
            font.width = keyfile->get_double(group, "width");
            font.family_kind = keyfile->get_integer(group, "family-kind");
            auto const flags = keyfile->get_integer(group, "flags");
            font.monospaced = flags & FontCacheFlags::Monospace;
            font.oblique = flags & FontCacheFlags::Oblique;
            font.variable_font = flags & FontCacheFlags::Variable;
            font.synthetic = flags & FontCacheFlags::Synthetic;
            font.available = true;
            if (font.family_name.empty() || font.description.empty() || font.fontspec.empty()) return {};

            auto [it, inserted] = family_indices.emplace(font.family_name.raw(), result->size());
            if (inserted) result->emplace_back();
            (*result)[it->second].emplace_back(std::move(font));
            ++face_count;
        }
        if (result->empty() || face_count != keyfile->get_uint64(catalog_header, "faces")) return {};
        for (auto &family : *result) sort_fonts(family, FontOrder::ByFamily, false);
        sort_font_families(*result, true);
        return result;
    } catch (Glib::Error const &error) {
        g_warning("Font catalog not loaded: %s", error.what());
        return {};
    } catch (std::exception const &error) {
        g_warning("Font catalog not loaded: %s", error.what());
        return {};
    }
}

std::string font_catalog_path()
{
    return Glib::build_filename(Inkscape::IO::Resource::profile_path(), font_catalog);
}

// Write the metric cache. When `carry_over` is given, entries loaded from a previous cache that
// this (possibly interrupted) scan has not recomputed are written first and entries recomputed by
// `fonts` overwrite them. A partial save must never truncate loaded metrics: the cache is only an
// accelerator, so dropping an unvisited entry would make the next start repeat that work.
void save_font_cache(const std::vector<std::vector<FontInfo>>& fonts,
                     const std::unordered_map<std::string, FontInfo>* carry_over = nullptr) {
    auto keyfile = Glib::KeyFile::create();

    keyfile->set_double(cache_header, "version", cache_version);
    Glib::ustring weight("weight");
    Glib::ustring width("width");
    Glib::ustring ffamily("family");
    Glib::ustring fontflags("flags");

    auto write_group = [&](Glib::ustring const& group, FontInfo const& font) {
        int flags = FontCacheFlags::Normal;
        if (font.monospaced) {
            flags |= FontCacheFlags::Monospace;
        }
        if (font.oblique) {
            flags |= FontCacheFlags::Oblique;
        }
        if (font.variable_font) {
            flags |= FontCacheFlags::Variable;
        }
        if (font.synthetic) {
            flags |= FontCacheFlags::Synthetic;
        }
        keyfile->set_double(group, weight, font.weight);
        keyfile->set_double(group, width, font.width);
        keyfile->set_integer(group, ffamily, font.family_kind);
        keyfile->set_integer(group, fontflags, flags);
    };

    if (carry_over) {
        for (auto&& entry : *carry_over) {
            write_group(Glib::ustring(entry.first), entry.second);
        }
    }

    for (auto&& family : fonts) {
        for (auto&& font : family) {
            auto desc = get_font_description(font.ff, font.face);
            write_group(desc.to_string(), font);
        }
    }

    std::string filename = Glib::build_filename(Inkscape::IO::Resource::profile_path(), font_cache);
    keyfile->save_to_file(filename);
}

std::unordered_map<std::string, FontInfo> load_cached_font_info() {
    std::unordered_map<std::string, FontInfo> info;

    try {
        auto keyfile = Glib::KeyFile::create();
        std::string filename = Glib::build_filename(Inkscape::IO::Resource::profile_path(), font_cache);

#ifdef G_OS_WIN32
        bool exists = filesystem::exists(filesystem::u8path(filename));
#else
        bool exists = filesystem::exists(filesystem::path(filename));
#endif

        if (exists && keyfile->load_from_file(filename)) {

            auto ver = keyfile->get_double(cache_header, "version");
            if (std::abs(ver - cache_version) > 0.0001) return info;

            Glib::ustring weight("weight");
            Glib::ustring width("width");
            Glib::ustring family("family");
            Glib::ustring fontflags("flags");

            for (auto&& group : keyfile->get_groups()) {
                if (group == cache_header) continue;

                FontInfo font;
                auto flags = keyfile->get_integer(group, fontflags);
                if (flags & FontCacheFlags::Monospace) {
                    font.monospaced = true;
                }
                if (flags & FontCacheFlags::Oblique) {
                    font.oblique = true;
                }
                if (flags & FontCacheFlags::Variable) {
                    font.variable_font = true;
                }
                if (flags & FontCacheFlags::Synthetic) {
                    font.synthetic = true;
                }
                font.weight = keyfile->get_double(group, weight);
                font.width = keyfile->get_double(group, width);
                font.family_kind = keyfile->get_integer(group, family);

                info[group.raw()] = font;
            }
        }
    }
    catch (Glib::Error &error) {
        std::cerr << G_STRFUNC << ": font cache not loaded - " << error.what() << std::endl;
    }

    return info;
}

std::vector<FontInfo> get_all_fonts() {
    std::vector<FontInfo> fonts;
    return fonts;
}

namespace {

// A cold scan has to instantiate and measure every installed face, which can take a long time on
// large system collections. Computed metrics are persisted at a bounded interval (and once more if
// the scan is cancelled) so that an interrupted scan - profile reload, shutdown, closed tool - does
// not throw away every face it already measured and force the next start to repeat that work.
constexpr auto font_cache_flush_interval = std::chrono::seconds(1);

} // namespace

std::shared_ptr<const std::vector<std::vector<FontInfo>>> get_all_fonts(Async::Progress<double, Glib::ustring, std::vector<FontInfo>>& progress, std::string const &manifest) {
    std::vector<FontInfo> empty;
    progress.report_or_throw(0, "", empty);

    if (auto cached = FontCatalog::load(font_catalog_path(), manifest)) {
        progress.report_or_throw(1, "", empty);
        return cached;
    }

    auto result = std::make_shared<std::vector<std::vector<FontInfo>>>();
    auto& fonts = *result;
    auto cache = load_cached_font_info();

    auto families = FontFactory::get().get_font_families();

    progress.throw_if_cancelled();
    bool update_cache = false;
    bool cache_dirty = false;
    auto cache_flushed_at = std::chrono::steady_clock::now();

    // Report progress, but keep the metrics computed so far if the scan is cancelled (profile
    // reload, shutdown, closed tool). The cache only accelerates later starts: it must not change
    // this scan's result or mask the cancellation. Loaded entries not yet visited are carried over
    // so an interrupted save never truncates the metric cache.
    auto report_or_stop = [&](double value, Glib::ustring const &name, std::vector<FontInfo> const &family) {
        try {
            progress.report_or_throw(value, name, family);
        }
        catch (Async::CancelledException const &) {
            if (cache_dirty) {
                try {
                    save_font_cache(fonts, &cache);
                }
                catch (...) {
                    // Preserve the original cancellation outcome.
                }
            }
            throw;
        }
    };

    double counter = 0.0;
    for (auto ff : families) {
        bool synthetic_font = false;
#if PANGO_VERSION_CHECK(1,46,0)
        auto default_face = ff->get_face();
        if (default_face && default_face->is_synthesized()) {
            synthetic_font = true;
        }
#endif
        report_or_stop(counter / families.size(), ff->get_name(), empty);
        std::vector<FontInfo> family;
        auto faces = ff->list_faces();
        std::set<std::string> styles;
        for (auto face : faces) {
            // skip synthetic faces of normal fonts, they pollute listing with fake entries,
            // but let entirely synthetic fonts in ("Sans", "Monospace", etc)
            if (!synthetic_font && face->is_synthesized()) continue;

            auto desc = face->describe();
            desc.unset_fields(Pango::FontMask::SIZE);
            std::string key = desc.to_string();
            if (styles.count(key)) continue;

            styles.insert(key);

            FontInfo info = { ff, face };
            info.family_name = ff->get_name();
            info.face_name = face->get_name();
            info.description = get_font_description(ff, face).to_string();
            info.fontspec = get_inkscape_fontspec(ff, face, info.variations);
            info.synthetic = synthetic_font;
            bool valid = false;

            desc = get_font_description(ff, face);
            auto it = cache.find(desc.to_string().raw());
            if (it == cache.end()) {
                // font not found in a cache; calculate metrics

                update_cache = true;
                cache_dirty = true;

                double caps_height = 0.0;

                try {
                    auto font = FontFactory::get().create_face(desc.gobj());
                    if (!font) {
                        g_warning("Cannot load font %s", key.c_str());
                    }
                    else {
                        valid = true;
                        info.monospaced = font->is_fixed_width();
                        info.oblique = font->is_oblique();
                        info.family_kind = font->family_class();
                        info.variable_font = !font->get_opentype_varaxes().empty();
                        info.source_path = font->GetFilename();
                        if (!info.source_path.empty()) {
                            GStatBuf statbuf{};
                            if (g_stat(info.source_path.c_str(), &statbuf) == 0) {
                                info.source_size = statbuf.st_size;
                                info.source_mtime = statbuf.st_mtime;
                            }
                        }
                        auto glyph = font->LoadGlyph(font->MapUnicodeChar('E'));
                        if (glyph) {
                            // caps height normalized to 0..1
                            caps_height = glyph->bbox_exact.height();
                        }
                    }
                }
                catch (...) {
                    g_warning("Error loading font %s", key.c_str());
                }
                desc = get_font_description(ff, face);
                info.weight = calculate_font_weight(desc, caps_height);

                desc = get_font_description(ff, face);
                info.width = calculate_font_width(desc);
            }
            else {
                // font in a cache already
                info = it->second;
                valid = true;
            }

            if (valid) {
                info.ff = ff;
                info.face = face;
                info.family_name = ff->get_name();
                info.face_name = face->get_name();
                info.description = get_font_description(ff, face).to_string();
                info.fontspec = get_inkscape_fontspec(ff, face, info.variations);
                info.available = true;
                info.id = make_font_id(info);
                family.emplace_back(info);
            }
        }
        if (!family.empty()) {
            fonts.push_back(family);
        }
        report_or_stop(++counter / families.size(), "", family);

        if (cache_dirty && std::chrono::steady_clock::now() - cache_flushed_at >= font_cache_flush_interval) {
            // The cache is only an accelerator for the next start: a failed partial save must not
            // abort a scan whose in-memory result is still complete. Carry over the loaded entries
            // this scan has not reached yet so the partial write is a union, never a truncation.
            try {
                save_font_cache(fonts, &cache);
                cache_flushed_at = std::chrono::steady_clock::now();
                cache_dirty = false;
            }
            catch (Glib::Error const &error) {
                g_warning("Cannot save partial font cache: %s", error.what());
                cache_flushed_at = std::chrono::steady_clock::now();
            }
        }
    }

    if (update_cache) {
        save_font_cache(fonts);
    }
    FontCatalog::save(font_catalog_path(), fonts, manifest);

    progress.report_or_throw(1, "", empty);

    return result;
}

Glib::ustring get_fontspec_without_variants(const Glib::ustring& fontspec) {
    auto at = fontspec.rfind('@');
    if (at != Glib::ustring::npos && at > 0) {
        // remove variations
        while (at > 0 && fontspec[at - 1] == ' ') at--; // trim spaces

        return fontspec.substr(0, at);
    }
    return fontspec;
}

FontDiscovery::FontDiscovery() {
    if (auto i = InkscapeApplication::instance()) {
        i->gio_app()->signal_shutdown().connect([this](){
            _loading.cancel();
        });
    }

    _connection = _loading.subscribe([this](const MessageType& msg) {
        if (auto result = Async::Msg::get_result(msg)) {
            // cache results
            _fonts = *result;
        }
        // propagate events
        _events.emit(msg);
    });

}

void FontDiscovery::invalidate()
{
    _loading.cancel();
    _fonts.reset();
    if (!_events.empty()) {
        // Called on the main thread: @font-face registration cannot interleave
        // with the borrowed Fontconfig set walk. Only the immutable hash crosses threads.
        auto manifest = FontFactory::get().font_config_manifest();
        _loading.start(
            [manifest = std::move(manifest)](Async::Progress<double, Glib::ustring, std::vector<FontInfo>>& p) {
                return get_all_fonts(p, manifest);
            });
    }
}

sigc::scoped_connection FontDiscovery::connect_to_fonts(std::function<void (const MessageType&)> fn) {

    sigc::scoped_connection con = static_cast<sigc::connection>(_events.connect(fn));

    if (!_fonts && !_loading.is_running()) {
        // Freeze the manifest on the main thread before starting the scan.
        auto manifest = FontFactory::get().font_config_manifest();
        _loading.start(
            [manifest = std::move(manifest)](Async::Progress<double, Glib::ustring, std::vector<FontInfo>>& p) {
                return get_all_fonts(p, manifest);
            }
        );
    }
    else if (_fonts) {
        // Fonts are already loaded: replay the cached result to this newly attached
        // subscriber only. Emitting the replay on _events would deliver it to every
        // earlier consumer as if a fresh scan had completed, resetting their stores
        // and cancelling their previews. Real background events keep broadcasting
        // through _events. Freeze the shared snapshot before invoking the callback
        // so its identity can detect a replacement during the Result callback.
        auto snapshot = _fonts;
        fn(Async::Msg::OperationResult<FontsPayload>{snapshot});
        if (_fonts == snapshot) {
            // Only finish this replay while its snapshot is still current.
            // A replacement load owns its own terminal event.
            fn(Async::Msg::OperationFinished{});
        }
    }

    return con;
}

} // namespace

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
