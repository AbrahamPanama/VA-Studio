// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/explode-bitmap-grid.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <2geom/transforms.h>
#include <cairo.h>
#include "bitmap-adjustment-chemistry.h"
#include "display/drawing-context.h"
#include "display/drawing-item.h"
#include "display/nr-filter-primitive.h"
#include "display/nr-filter-slot.h"
#include "ui/tools/destructive-bitmap-coverage.h"
#include "util/bitmap-islands.h"
namespace Inkscape::Bitmap {
AlphaLut recipeAlphaLut(Recipe const &recipe) noexcept
{
    auto lut = alphaLut(recipe.threshold, recipe.softness);
    auto cutoff = std::min(recipe.faintFloor, 25u) * 255 / 100;
    for (unsigned a = 0; a < 256; ++a) {
        if (recipe.alphaPrepared) lut[a] = a;
        else if (a <= cutoff) lut[a] = 0;
        else if (recipe.bypassAlpha) lut[a] = a;
    }
    return lut;
}
namespace {
using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
using Pattern = std::unique_ptr<cairo_pattern_t, decltype(&cairo_pattern_destroy)>;
Geom::Affine affine(GridTransform const &a) { return {a[0], a[1], a[2], a[3], a[4], a[5]}; }
GridTransform values(Geom::Affine const &a) { return {a[0], a[1], a[2], a[3], a[4], a[5]}; }
bool qualified(Geom::Affine const &a) { return UI::Tools::DestructiveBitmapCoverage::gridMappingQualified(a); }
Result<FinalGrid> fail(Outcome o) { Result<FinalGrid> r; r.outcome = o; return r; }
Outcome incompatible(char const *s) { return {Status::incompatible, s}; }
void hashWord(std::uint64_t &hash, std::uint64_t word)
{
    for (unsigned i = 0; i < 8; ++i) { hash ^= (word >> (8*i)) & 255; hash *= 1099511628211ULL; }
}
// Use the actual canvas renderer on an opaque raw-value ramp. This avoids copying
// component-transfer quantization and avoids premultiply loss BEFORE the alpha LUT.
Outcome toneLut(Filters::BitmapToneSettings const &tone, AlphaLut &lut, Budget &budget)
{
    Budget::Token scratch;
    auto o = budget.acquire(Stage::composition, 65536, scratch);
    if (!o.ok()) return o;
    Surface surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 256, 1), cairo_surface_destroy);
    if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) return {Status::failed, "Tone surface failed."};
    auto data = cairo_image_surface_get_data(surface.get());
    for (unsigned i = 0; i < 256; ++i) {
        std::uint32_t p = 0xff000000u | i*0x010101u; std::memcpy(data+4*i, &p, 4);
    }
    cairo_surface_mark_dirty(surface.get());
    DrawingContext dc(surface.get(), Geom::Point(0, 0));
    Filters::FilterUnits units;
    units.set_ctm(Geom::identity());
    units.set_item_bbox(Geom::Rect::from_xywh(0, 0, 256, 1));
    units.set_filter_area(Geom::Rect::from_xywh(0, 0, 256, 1));
    units.set_resolution(256, 1); units.set_paraller(false);
    RenderContext rc{Colors::Color(0x000000ff)};
    Filters::FilterSlot slot(dc, units, rc, 0);
    auto renderer = BitmapAdjustments::build_tone_renderer_primitive(tone);
    renderer->set_input(Filters::NR_FILTER_SOURCEGRAPHIC); renderer->set_output(0);
    slot.set(Filters::NR_FILTER_SOURCEGRAPHIC, surface.get());
    renderer->render_cairo(slot);
    auto out = slot.getcairo(0);
    if (!out || cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) return {Status::failed, "Tone renderer failed."};
    cairo_surface_flush(out);
    auto raw = cairo_image_surface_get_data(out);
    if (!raw) return {Status::failed, "Tone pixels unavailable."};
    for (unsigned i = 0; i < 256; ++i) {
        std::uint32_t p; std::memcpy(&p, raw+4*i, 4); lut[i] = (p >> 16) & 255;
    }
    return {};
}
Outcome poll(JobWork &work, PhaseTimer &timer, Stop stop, std::uint64_t visits)
{
    auto o = timer.check(stop); return o.ok() ? work.advance(visits) : o;
}
} // namespace
RgbaView FinalGrid::view() const noexcept
{
    return {reinterpret_cast<std::uint8_t const *>(pixels.data()), pixels.size(), std::uint64_t(width)*4, width, height};
}
std::size_t serializeGridTransform(GridTransform const &a, char *out, std::size_t size) noexcept
{
    if (!out || size < 10 || !qualified(affine(a))) return 0;
    std::memcpy(out, "matrix(", 7); auto p = out+7; auto end = out+size-2;
    for (unsigned i = 0; i < 6; ++i) {
        auto r = std::to_chars(p, end, a[i], std::chars_format::general, std::numeric_limits<double>::max_digits10);
        if (r.ec != std::errc{}) return 0;
        p = r.ptr; if (i < 5) { if (p == end) return 0; *p++ = ','; }
    }
    *p++ = ')'; *p = 0; return p-out;
}
Result<FinalGrid> prepareGrid(TargetSnapshot const &s, DecodedRaster const &source, Recipe recipe,
                             Budget &budget, Stop stop) noexcept try
{
    PhaseTimer timer;
    if (auto o = timer.check(stop); !o.ok()) return fail(o);
    if (s.supportability != Supportability::Supported || s.mode != TargetMode::SingleBitmap || !s.refusals.empty())
        return fail(incompatible("A supported single-bitmap snapshot is required."));
    TargetContext const *own = nullptr, *parent = nullptr;
    for (auto const &c : s.contexts) {
        if (c.identity == s.bitmap) { if (own) return fail(incompatible("Duplicate bitmap context.")); own = &c; }
        if (c.identity == s.destinationParent) parent = &c;
    }
    if (!own || !parent || own->parent != s.destinationParent || own->mask || own->hidden || own->locked ||
        !std::isfinite(own->opacity) || own->opacity <= 0 || own->opacity > 1 ||
        !std::isfinite(s.retainedAncestorOpacity) || s.retainedAncestorOpacity <= 0 || s.retainedAncestorOpacity > 1)
        return fail(incompatible("Invalid own/retained context."));
    if (recipe.threshold > 255 || recipe.softness > 127 || recipe.faintFloor > 25 || !source.view().validate().ok() ||
        source.width > 16384 || source.height > 16384 || std::uint64_t(source.width)*source.height > 100000000 ||
        source.profileBytes != source.profile.size() || source.profileBytes > 4*MiB) return fail(incompatible("Invalid raster or recipe."));
    for (auto property : {Filters::BitmapToneProperty::Brightness, Filters::BitmapToneProperty::Contrast,
                         Filters::BitmapToneProperty::Intensity, Filters::BitmapToneProperty::Highlights,
                         Filters::BitmapToneProperty::Shadows, Filters::BitmapToneProperty::Midtones})
        if (!std::isfinite(Filters::get_bitmap_tone_property(own->tone, property)))
            return fail(incompatible("Nonfinite tone settings."));
    auto c2p = affine(own->pixelToItem), i2doc = affine(own->itemToDocument), p2doc = affine(parent->itemToDocument);
    if (!qualified(c2p) || !qualified(i2doc) || !qualified(p2doc) || c2p[0] <= 0 || c2p[3] <= 0 || c2p[1] || c2p[2])
        return fail(incompatible("Invalid pixel/item/parent transform."));
    auto mapping = c2p*i2doc;
    if (!qualified(mapping)) return fail(incompatible("Final mapping exceeds relative condition limit."));
    auto extent = Geom::Rect::from_xywh(0, 0, source.width, source.height)*c2p;
    auto const &v = own->viewport;
    for (auto d : v) if (!std::isfinite(d)) return fail(incompatible("Nonfinite viewport."));
    if (v[2] <= 0 || v[3] <= 0) return fail(incompatible("Empty viewport."));
    // SPViewBox::apply_viewbox averages almost-equal axis scales within
    // Geom::EPSILON (also for preserveAspectRatio=none). Decimal import sizes
    // can therefore protrude slightly. Match that relative tolerance, plus
    // coordinate roundoff; a fixed 1e-9 misclassified normal imports as slice.
    auto tolerance = Geom::EPSILON * std::max({v[2], v[3], extent.width(), extent.height()}) +
        16 * std::numeric_limits<double>::epsilon() * std::max({1.0, std::abs(v[0]), std::abs(v[1])});
    auto viewport = Geom::Rect::from_xywh(v[0], v[1], v[2], v[3]); viewport.expandBy(tolerance);
    if (!viewport.contains(extent)) return fail(incompatible("Cropping slice is unsupported."));
    auto region = Geom::Rect::from_xywh(0, 0, source.width, source.height)*mapping;
    if (!region.isFinite() || std::max({std::abs(region.left()), std::abs(region.right()),
        std::abs(region.top()), std::abs(region.bottom())}) > 1e7)
        return fail(incompatible("Final bounds exceed coordinate domain."));
    auto plan = UI::Tools::DestructiveBitmapCoverage::gridGeometry(mapping, source.width, source.height);
    if (!plan) return fail(incompatible("Unsafe final-grid expansion."));
    auto count = std::uint64_t(source.width)*source.height;
    auto coverage = recipe.coverage;
    if (own->clip && (!coverage || coverage->bitmap != s.bitmap || coverage->generation != s.generation ||
        coverage->width != source.width || coverage->height != source.height || coverage->pixels.size() != count))
        return fail(incompatible("Capture current own clip on main before dispatch."));
    if (!own->clip && coverage) return fail(incompatible("Unexpected own clip coverage."));
    JobWork local(count); auto &work = recipe.work ? *recipe.work : local;
    FinalGrid grid;
    grid.width = plan->width; grid.height = plan->height;
    auto finalMapping = plan->pixelToDocument;
    grid.pixelToDocument = values(finalMapping); grid.pixelToParent = values(finalMapping*p2doc.inverse());
    if (!qualified(affine(grid.pixelToParent))) return fail(incompatible("Unsafe compensated parent mapping."));
    // Reject compensation whose cancellation loses document-space crop endpoints.
    auto restored = affine(grid.pixelToParent)*p2doc;
    for (Geom::Point point : {Geom::Point(0,0), Geom::Point(grid.width,0),
         Geom::Point(0,grid.height), Geom::Point(grid.width,grid.height), Geom::Point(1,1)})
        if (Geom::distance(point*restored,point*finalMapping) > 1e-6)
            return fail(incompatible("Parent compensation loses final-grid precision."));
    grid.dpiX = 96/std::hypot(finalMapping[0], finalMapping[1]);
    grid.dpiY = 96/std::hypot(finalMapping[2], finalMapping[3]);
    if (!std::isfinite(grid.dpiX) || !std::isfinite(grid.dpiY) || grid.dpiX <= 0 || grid.dpiY <= 0)
        return fail(incompatible("Invalid grid density."));
    grid.sampling = plan->straightened ? (recipe.pixelated ? GridSampling::Nearest : GridSampling::Good) : GridSampling::Exact;
    PlainBuffer baked;
    auto o = baked.allocate(budget, Stage::composition, count, 4, recipe.fault, stop);
    if (!o.ok()) return fail(o);
    AlphaLut tone, alpha = recipeAlphaLut(recipe);
    if (!(o = toneLut(own->tone, tone, budget)).ok()) return fail(o);
    auto input = reinterpret_cast<std::uint8_t const *>(source.pixels.data());
    auto dst = reinterpret_cast<std::uint8_t *>(baked.data());
    auto clip = coverage ? reinterpret_cast<std::uint8_t const *>(coverage->pixels.data()) : nullptr;
    auto opacity = static_cast<unsigned>(std::round(own->opacity*255));
    if (recipe.checkpoint) recipe.checkpoint(GridPhase::Bake, recipe.checkpointData);
    for (std::uint64_t begin = 0; begin < count; begin += 4096) {
        auto end = std::min(count, begin+4096);
        if (!(o = poll(work, timer, stop, end-begin)).ok()) return fail(o);
        for (auto i = begin; i < end; ++i) {
            auto a = alpha[input[4*i+3]];
            if (clip) a = (unsigned(a)*clip[i]+127)/255;
            a = (unsigned(a)*opacity+127)/255;
            for (unsigned c = 0; c < 3; ++c) dst[4*i+c] = a ? tone[input[4*i+c]] : 0;
            dst[4*i+3] = a;
        }
    }
    if (plan->straightened) {
        // Reserve BOTH premultiplied source and destination before resampling.
        PlainBuffer premul, output;
        if (!(o = premul.allocate(budget, Stage::straighten, count, 4, recipe.fault, stop)).ok() ||
            !(o = output.allocate(budget, Stage::straighten, std::uint64_t(grid.width)*grid.height, 4, recipe.fault, stop)).ok()) return fail(o);
        for (std::uint64_t begin = 0; begin < count; begin += 4096) {
            auto end = std::min(count, begin+4096);
            if (!(o = poll(work, timer, stop, end-begin)).ok()) return fail(o);
            for (auto i = begin; i < end; ++i) {
                auto a = dst[4*i+3]; std::uint32_t p = (unsigned(a)<<24) |
                    (premul_alpha(dst[4*i], a)<<16) | (premul_alpha(dst[4*i+1], a)<<8) | premul_alpha(dst[4*i+2], a);
                std::memcpy(premul.data()+4*i, &p, 4);
            }
        }
        Budget::Token cairoScratch;
        if (!(o = budget.acquire(Stage::straighten, std::uint64_t(grid.width)*32+65536, cairoScratch)).ok()) return fail(o);
        Surface src(cairo_image_surface_create_for_data(reinterpret_cast<unsigned char *>(premul.data()), CAIRO_FORMAT_ARGB32,
            source.width, source.height, source.width*4), cairo_surface_destroy);
        // Render bounded strips; the pattern maps absolute target pixels into the
        // source. Cairo's sampling policy/math is identical to DC-S1, no GTK calls.
        auto inverse = (mapping*finalMapping.inverse()).inverse();
        Pattern pattern(cairo_pattern_create_for_surface(src.get()), cairo_pattern_destroy);
        cairo_pattern_set_extend(pattern.get(), CAIRO_EXTEND_NONE);
        cairo_pattern_set_filter(pattern.get(), recipe.pixelated ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
        if (recipe.checkpoint) recipe.checkpoint(GridPhase::Straighten, recipe.checkpointData);
        for (unsigned y = 0; y < grid.height; ++y) {
            if (!(o = poll(work, timer, stop, grid.width)).ok()) return fail(o);
            auto rowMap = Geom::Translate(0, y)*inverse;
            cairo_matrix_t cm{rowMap[0], rowMap[1], rowMap[2], rowMap[3], rowMap[4], rowMap[5]};
            cairo_pattern_set_matrix(pattern.get(), &cm);
            auto row = reinterpret_cast<unsigned char *>(output.data())+std::uint64_t(y)*grid.width*4;
            Surface dest(cairo_image_surface_create_for_data(row, CAIRO_FORMAT_ARGB32, grid.width, 1, grid.width*4), cairo_surface_destroy);
            Context cr(cairo_create(dest.get()), cairo_destroy);
            cairo_set_operator(cr.get(), CAIRO_OPERATOR_SOURCE); cairo_set_source(cr.get(), pattern.get()); cairo_paint(cr.get());
            if (cairo_status(cr.get()) != CAIRO_STATUS_SUCCESS || cairo_surface_status(src.get()) != CAIRO_STATUS_SUCCESS ||
                cairo_surface_status(dest.get()) != CAIRO_STATUS_SUCCESS || cairo_pattern_status(pattern.get()) != CAIRO_STATUS_SUCCESS)
                return fail({Status::failed, "Straightening failed."});
            cairo_surface_flush(dest.get());
            for (unsigned x = 0; x < grid.width; ++x) {
                std::uint32_t p; std::memcpy(&p, row+4*x, 4); unsigned a = p >> 24;
                row[4*x] = a ? unpremul_alpha((p >> 16)&255, a) : 0;
                row[4*x+1] = a ? unpremul_alpha((p >> 8)&255, a) : 0;
                row[4*x+2] = a ? unpremul_alpha(p&255, a) : 0; row[4*x+3] = a;
            }
        }
        grid.pixels = std::move(output);
    } else grid.pixels = std::move(baked);
    if (!(o = grid.profile.allocate(budget, Stage::prepared, source.profile.size(), 1, recipe.fault, stop)).ok()) return fail(o);
    for (std::size_t begin = 0; begin < source.profile.size(); begin += 4096) {
        if (!(o = poll(work, timer, stop, 0)).ok()) return fail(o);
        std::memcpy(grid.profile.data()+begin, source.profile.data()+begin, std::min<std::size_t>(4096, source.profile.size()-begin));
    }
    auto &hash = grid.recipeHash; hash = 1469598103934665603ULL;
    hashWord(hash, source.originalHash); hashWord(hash, recipe.threshold); hashWord(hash, recipe.softness);
    hashWord(hash, recipe.faintFloor); hashWord(hash, recipe.alphaPrepared);
    hashWord(hash, recipe.bypassAlpha); hashWord(hash, unsigned(grid.sampling)); hashWord(hash, opacity);
    hashWord(hash, grid.width); hashWord(hash, grid.height);
    for (auto a : values(mapping*finalMapping.inverse())) hashWord(hash, std::bit_cast<std::uint64_t>(a));
    for (auto a : grid.pixelToDocument) hashWord(hash, std::bit_cast<std::uint64_t>(a));
    for (auto a : grid.pixelToParent) hashWord(hash, std::bit_cast<std::uint64_t>(a));
    for (auto a : tone) hashWord(hash, a);
    // Content fingerprint covers clip, raw bytes, and source ICC even for synthetic sources.
    for (std::uint64_t begin = 0; begin < count; begin += 4096) {
        auto end = std::min(count, begin+4096);
        if (!(o = poll(work, timer, stop, end-begin)).ok()) return fail(o);
        for (auto i = begin; i < end; ++i) { hashWord(hash, clip ? clip[i] : 255);
            std::uint32_t p; std::memcpy(&p, input+4*i, 4); hashWord(hash, p); }
    }
    for (std::size_t i = 0; i < grid.profile.size(); ++i) {
        if (i%4096 == 0 && !(o = poll(work, timer, stop, 0)).ok()) return fail(o);
        hashWord(hash, std::to_integer<unsigned>(grid.profile.data()[i]));
    }
    if (!(o = timer.check(stop)).ok()) return fail(o);
    Result<FinalGrid> result; result.outcome = {Status::changed, "Final grid prepared."}; result.value = std::move(grid);
    result.consumed = work.visits(); return result;
} catch (...) { return fail({Status::failed, "Grid preparation allocation/renderer failed."}); }
Result<FinalGrid> prepareGrid(CandidateGridInput const &input, Recipe recipe, Budget &budget, Stop stop) noexcept
{
    if (!input.identity) return fail({Status::unavailable,"Missing candidate grid identity."});
    auto result=prepareGrid(input.geometry,input.raster,recipe,budget,stop);
    if (result.ok()) {
        result.value.candidateIdentity=input.identity;
        result.value.candidateThreshold=recipe.threshold; result.value.candidateSoftness=recipe.softness;
        result.value.candidateFaintFloor=recipe.faintFloor;
        result.value.candidateBypassAlpha=recipe.bypassAlpha;
    }
    return result;
}
} // namespace Inkscape::Bitmap
