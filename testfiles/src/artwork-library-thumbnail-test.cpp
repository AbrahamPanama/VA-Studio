// SPDX-License-Identifier: GPL-2.0-or-later
// Native service and opt-in renderer fixtures. No actual execution claimed by
// this artifact. Host initializes native Application/GTK on the test thread.
#include "ui/cache/artwork-library-thumbnail.h"
#include "io/artwork-library-package.h"
#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "object/sp-root.h"
#include "display/drawing.h"
#include "display/drawing-group.h"
#include "display/drawing-context.h"
#include "display/drawing-surface.h"
#include "object/object-set.h"
#include "xml/node.h"
#include "xml/attribute-record.h"
#include <gtest/gtest.h>
#include <gtkmm/application.h>
#include <2geom/transforms.h>
#include <cairomm/surface.h>
#include <gdk/gdk.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
#include <memory>
#include <optional>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include <thread>
#include <limits>
#include <chrono>
#include <barrier>

using namespace Inkscape;
using namespace Inkscape::UI::Cache;
namespace Lib = Inkscape::IO::ArtworkLibrary;
namespace {
Lib::ValidatedSvg asset(std::string body, std::string extra = {})
{
    std::string xml = "<svg xmlns='http://www.w3.org/2000/svg' "
        "xmlns:inkscape='http://www.inkscape.org/namespaces/inkscape' "
        "width='96' height='64' viewBox='0 0 96 64' " + extra + ">" + body + "</svg>";
    Lib::Bytes b(xml.begin(), xml.end());
    return Lib::preflight_svg(b, {"b203e8e9-640c-4195-b0ea-f9b790245678",
        Lib::artwork_sha256(b), 25.4, 64 * 25.4 / 96});
}
std::string filtered(std::string primitives, std::string filter_attrs = {},
                     std::string item_attrs = "x='16' y='12' width='48' height='32'")
{
    return "<defs><filter id='f' inkscape:auto-region='false' " + filter_attrs + ">" +
        primitives + "</filter></defs><rect " + item_attrs + " fill='#c43b67' filter='url(#f)'/>";
}
std::vector<unsigned char> download(Glib::RefPtr<Gdk::Texture> const &t)
{
    std::vector<unsigned char> b(std::size_t(t->get_width()) * t->get_height() * 4);
    gdk_texture_download(t->gobj(), b.data(), t->get_width() * 4);
    return b;
}
// Independent plain native Drawing oracle, WITHOUT budget hooks active. Uses
// identical authored bytes, explicit physical-page framing and exact quality;
// strict byte equality qualifies that admission does not alter accepted output.
std::vector<unsigned char> reference(Lib::ValidatedSvg const &a, ThumbnailSize size)
{
    EXPECT_EQ(PreviewRenderBudget::current(), nullptr);
    auto bytes = a.svg_bytes();
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(bytes->data(), bytes->size()));
    if (!doc) throw std::runtime_error("reference parse");
    doc->ensureUpToDate();
    Drawing d; d.setCacheBudget(0);
    auto key = SPItem::display_key_new(1);
    auto root = doc->getRoot();
    auto shown = root->invoke_show(d, key, SP_ITEM_SHOW_DISPLAY);
    struct Hide { SPRoot *r; unsigned k; ~Hide() { r->invoke_hide(k); } } hide{root, key};
    d.setRoot(shown);
    d.setExact();
    d.setDithering(false); // Explicit oracle policy, independent of the service.
    EXPECT_FALSE(d.useDithering());
    auto w = size.width * size.device_scale, h = size.height * size.device_scale;
    auto zoom = std::min(w / 96., h / 64.);
    shown->setTransform(Geom::Scale(zoom) * Geom::Translate((w - 96 * zoom) / 2, (h - 64 * zoom) / 2));
    d.update();
    auto s = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, w, h);
    {
        auto background = Cairo::Context::create(s);
        background->set_source_rgb(1.0, 1.0, 1.0);
        background->paint();
        DrawingSurface target(s->cobj(), Geom::Point(0, 0));
        DrawingContext dc(target);
        d.render(dc, Geom::IntRect::from_xywh(0, 0, w, h), DrawingItem::RENDER_BYPASS_CACHE);
    }
    s->flush();
    std::vector<unsigned char> out;
    for (unsigned y = 0; y < h; ++y)
        out.insert(out.end(), s->get_data() + y * s->get_stride(), s->get_data() + y * s->get_stride() + w * 4);
    return out;
}
void drain(GMainContext *c)
{
    unsigned calls = 0;
    while (g_main_context_pending(c) && calls++ < 256) g_main_context_iteration(c, false);
    ASSERT_LT(calls, 256u) << "Idle service did not quiesce";
}
std::string xml_identity(XML::Node const *node)
{
    std::string out;
    auto field = [&](char const *value) {
        auto s = std::string(value ? value : "");
        out += std::to_string(s.size()) + ":" + s;
    };
    field(node->name()); field(node->content());
    for (auto const &a : node->attributeList()) {
        field(g_quark_to_string(a.key)); field(static_cast<char const *>(a.value));
    }
    out += "[";
    for (auto c = node->firstChild(); c; c = c->next()) out += xml_identity(c);
    return out + "]";
}
class LibraryThumbnailNative : public ::testing::Test {
protected:
    void SetUp() override {
        // Reuse host initialization conventions, with no window/desktop needed.
        static auto gtk = Gtk::Application::create("org.inkscape.vacards.thumbtest",
                                                   Gio::Application::Flags::NON_UNIQUE);
        if (!Application::exists()) Application::create(false);
        context = g_main_context_new();
        pool = ArtworkLibraryThumbnailPool::create(context);
    }
    void TearDown() override { drain(context); pool.reset(); g_main_context_unref(context); }
    GMainContext *context = nullptr;
    std::shared_ptr<ArtworkLibraryThumbnailPool> pool;
};

TEST_F(LibraryThumbnailNative, NativeVectorsAlphaAndNestedTransformsMatchStrictOracle)
{
    auto a = asset("<g transform='translate(8 3) rotate(13 30 20)' opacity='.65'>"
                   "<path d='M8 8 C18 2 45 30 70 10 L60 45 Z' fill='#34aadc'/></g>");
    for (auto s : {ThumbnailSize{119, 83, 1}, ThumbnailSize{119, 83, 2}}) {
        auto r = render_library_thumbnail(pool, a, s); ASSERT_TRUE(r.image) << r.diagnostic;
        EXPECT_EQ(download(r.image->texture), reference(a, s));
        EXPECT_GT(r.image->render_stats.surfaces, 0u);
        EXPECT_LE(r.image->render_stats.bytes, 256u * 1024 * 1024);
    }
}

TEST_F(LibraryThumbnailNative, TransparentArtworkGetsOpaqueWhitePresentationBackground)
{
    auto a = asset("<path d='M40 20 L56 44' fill='none' stroke='#808080' stroke-width='2'/>");
    auto r = render_library_thumbnail(pool, a, {119, 83, 1});
    ASSERT_TRUE(r.image) << r.diagnostic;
    auto pixels = download(r.image->texture);
    ASSERT_GE(pixels.size(), 4u);
    // The source has no page background. The published preview must still be
    // an opaque white tile so transparent pixels cannot blend into the GTK UI.
    EXPECT_EQ(pixels[0], 255u);
    EXPECT_EQ(pixels[1], 255u);
    EXPECT_EQ(pixels[2], 255u);
    EXPECT_EQ(pixels[3], 255u);
    for (std::size_t i = 3; i < pixels.size(); i += 4) EXPECT_EQ(pixels[i], 255u);
}

TEST_F(LibraryThumbnailNative, NativeGradientsUseFixedDitheringAndMatchStrictOracleRepeatedly)
{
    // Real preflight admission, paint servers and native pixels: no forged
    // handle, cached preview, global preference or dispatch-pool mutation.
    auto a = asset(
        "<defs>"
        "<linearGradient id='linear' gradientUnits='userSpaceOnUse' x1='0' y1='0' x2='48' y2='0'>"
        "<stop offset='0' stop-color='#202020'/>"
        "<stop offset='1' stop-color='#d0d0d0'/>"
        "</linearGradient>"
        "<radialGradient id='radial' gradientUnits='userSpaceOnUse' cx='72' cy='32' r='24'>"
        "<stop offset='0' stop-color='#e0c080'/>"
        "<stop offset='1' stop-color='#203060'/>"
        "</radialGradient>"
        "</defs>"
        "<rect width='48' height='64' fill='url(#linear)'/>"
        "<rect x='48' width='48' height='64' fill='url(#radial)'/>");
    for (auto size : {ThumbnailSize{119, 83, 1}, ThumbnailSize{119, 83, 2}}) {
        SCOPED_TRACE(size.device_scale);
        auto const w = size.width * size.device_scale;
        auto const h = size.height * size.device_scale;
        auto expected = reference(a, size); // Fresh, unbudgeted, explicitly undithered Drawing.
        ASSERT_EQ(expected.size(), std::size_t(w) * h * 4);
        auto pixel_at = [&](unsigned x, unsigned y) {
            auto i = (std::size_t(y) * w + x) * 4;
            return std::array<unsigned char, 4>{expected[i], expected[i + 1],
                                                expected[i + 2], expected[i + 3]};
        };
        // Interior pixels prove both native gradients vary, not a blank or
        // constant-paint fallback. No channel ordering assumption is needed.
        EXPECT_NE(pixel_at(w / 8, h / 2), pixel_at(3 * w / 8, h / 2));
        EXPECT_NE(pixel_at(5 * w / 8, h / 2), pixel_at(3 * w / 4, h / 2));
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            SCOPED_TRACE(attempt);
            // Direct service entrypoint: each attempt owns a new document and
            // Drawing, so cache reuse cannot conceal initialization differences.
            pool->invalidate_all(ThumbnailInvalidation::FontsChanged);
            auto r = render_library_thumbnail(pool, a, size);
            ASSERT_TRUE(r.image) << r.diagnostic;
            EXPECT_EQ(r.failure, ThumbnailFailure::None);
            EXPECT_EQ(download(r.image->texture), expected);
            EXPECT_EQ(PreviewRenderBudget::current(), nullptr);
        }
    }
}

TEST_F(LibraryThumbnailNative, EveryAdmittedFilterWorkflowMatchesUnbudgetedNativePixels)
{
    for (auto const &p : {
        "<feGaussianBlur stdDeviation='1.5'/>",
        "<feOffset dx='3' dy='-2'/>",
        "<feColorMatrix type='saturate' values='.3'/>",
        "<feFlood flood-color='#5a83c4'/><feComposite in2='SourceGraphic' operator='in'/>",
        "<feOffset dx='2' result='shift'/><feBlend in='shift' in2='SourceGraphic' mode='multiply'/>",
        "<feOffset dx='2' result='shift'/><feMerge><feMergeNode in='SourceGraphic'/><feMergeNode in='shift'/></feMerge>",
        "<feComponentTransfer><feFuncR type='linear' slope='.7' intercept='.1'/><feFuncA type='linear' slope='.8'/></feComponentTransfer>",
        "<feMorphology operator='dilate' radius='1.5'/>"
    }) {
        SCOPED_TRACE(p);
        auto a = asset(filtered(p));
        ThumbnailSize s{119, 83, 1};
        auto r = render_library_thumbnail(pool, a, s); ASSERT_TRUE(r.image) << r.diagnostic;
        EXPECT_GT(r.image->render_stats.filters, 0u);
        EXPECT_EQ(download(r.image->texture), reference(a, s));
    }
}

TEST_F(LibraryThumbnailNative, ExplicitFilterResAndPrimitiveBBoxArePreserved)
{
    auto a = asset(filtered("<feGaussianBlur stdDeviation='.03 .05'/>",
                           "filterRes='64 48' primitiveUnits='objectBoundingBox'"));
    for (auto s : {ThumbnailSize{119, 83, 1}, ThumbnailSize{119, 83, 2}}) {
        auto r = render_library_thumbnail(pool, a, s); ASSERT_TRUE(r.image) << r.diagnostic;
        EXPECT_EQ(download(r.image->texture), reference(a, s));
    }
}

TEST_F(LibraryThumbnailNative, NativeTextAndTransparentEmbeddedBitmapMatch)
{
    auto png_surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 2, 2);
    std::uint32_t pixel[4] = {0x80800000, 0, 0xff0000ff, 0x40004000};
    for (int y = 0; y < 2; ++y)
        std::memcpy(png_surface->get_data() + y * png_surface->get_stride(), pixel + y * 2, 8);
    png_surface->mark_dirty();
    std::string png;
    auto status = cairo_surface_write_to_png_stream(png_surface->cobj(),
        [](void *p, unsigned char const *b, unsigned length) -> cairo_status_t {
            try { static_cast<std::string *>(p)->append(reinterpret_cast<char const *>(b), length); }
            catch (...) { return CAIRO_STATUS_WRITE_ERROR; }
            return CAIRO_STATUS_SUCCESS;
        }, &png);
    ASSERT_EQ(status, CAIRO_STATUS_SUCCESS);
    auto raw = g_base64_encode(reinterpret_cast<guchar const *>(png.data()), png.size());
    std::string encoded(raw); g_free(raw);
    auto a = asset("<text x='4' y='20' font-family='sans-serif' font-size='14' fill='#275391'>Native</text>"
        "<image x='30' y='26' width='32' height='32' href='data:image/png;base64," + encoded + "'/>");
    auto r = render_library_thumbnail(pool, a, {119, 83, 2}); ASSERT_TRUE(r.image) << r.diagnostic;
    EXPECT_EQ(download(r.image->texture), reference(a, {119, 83, 2}));
}

TEST_F(LibraryThumbnailNative, BlurPositiveControlActuallyChangesThumbnail)
{
    auto a = asset(filtered("<feGaussianBlur stdDeviation='1'/>"));
    auto b = asset(filtered("<feGaussianBlur stdDeviation='3'/>"));
    auto r = render_library_thumbnail(pool, a, {}), s = render_library_thumbnail(pool, b, {});
    ASSERT_TRUE(r.image) << r.diagnostic; ASSERT_TRUE(s.image) << s.diagnostic;
    EXPECT_NE(download(r.image->texture), download(s.image->texture));
}

TEST_F(LibraryThumbnailNative, NativeAutomaticFilterRegionRemainsSupported)
{
    auto a = asset("<defs><filter id='f'><feGaussianBlur stdDeviation='2'/></filter></defs>"
                   "<rect x='16' y='12' width='48' height='32' fill='red' filter='url(#f)'/>");
    auto r = render_library_thumbnail(pool, a, {119, 83, 1}); ASSERT_TRUE(r.image) << r.diagnostic;
    EXPECT_GT(r.image->render_stats.filters, 0u);
    EXPECT_EQ(download(r.image->texture), reference(a, {119, 83, 1}));
}

TEST_F(LibraryThumbnailNative, UnrelatedDrawingDoesNotInheritThumbnailBudget)
{
    Drawing preview, ordinary;
    PreviewRenderBudget::Limits limits; limits.bytes = 4;
    PreviewRenderBudget b(limits); b.bind_drawing(&preview);
    {
        PreviewRenderBudget::DrawingScope own(&preview);
        EXPECT_THROW(PreviewRenderBudget::surface(2, 2), PreviewRenderLimit);
    }
    {
        PreviewRenderBudget::DrawingScope other(&ordinary);
        EXPECT_EQ(PreviewRenderBudget::current(), nullptr);
        EXPECT_NO_THROW(PreviewRenderBudget::surface(2, 2));
    }
    EXPECT_EQ(PreviewRenderBudget::current(), &b);
}

TEST_F(LibraryThumbnailNative, PatternClipMaskAndFilterShareOneAdmissionBudget)
{
    auto a = asset("<defs><pattern id='p' patternUnits='userSpaceOnUse' width='8' height='8'>"
        "<rect width='5' height='8' fill='#a57321'/></pattern>"
        "<clipPath id='c'><circle cx='44' cy='30' r='25'/></clipPath>"
        "<mask id='m'><rect width='96' height='64' fill='white'/></mask>"
        "<filter id='f' inkscape:auto-region='false'><feGaussianBlur stdDeviation='1'/></filter></defs>"
        "<rect x='10' y='8' width='70' height='48' fill='url(#p)' clip-path='url(#c)' mask='url(#m)' filter='url(#f)'/>");
    auto r = render_library_thumbnail(pool, a, {119, 83, 1}); ASSERT_TRUE(r.image) << r.diagnostic;
    EXPECT_EQ(download(r.image->texture), reference(a, {119, 83, 1}));
    EXPECT_GT(r.image->render_stats.surfaces, 4u);
}

TEST_F(LibraryThumbnailNative, ExplicitLargeIntermediateFailsDespiteSmallOutput)
{
    auto a = asset(filtered("<feGaussianBlur stdDeviation='1'/>", "filterRes='4096 4096'"));
    auto r = render_library_thumbnail(pool, a, {32, 32, 1});
    EXPECT_FALSE(r.image); EXPECT_EQ(r.failure, ThumbnailFailure::Limits);
    EXPECT_EQ(PreviewRenderBudget::current(), nullptr);
}

TEST_F(LibraryThumbnailNative, PostNativeKernelFailsBeforeRenderingTinyGeometry)
{
    auto a = asset(filtered("<feGaussianBlur stdDeviation='1024'/>",
        "filterUnits='userSpaceOnUse' filterRes='4096 4096' x='0' y='0' width='.01' height='.01'",
        "x='0' y='0' width='.001' height='.001'"));
    auto r = render_library_thumbnail(pool, a, {32, 32, 1});
    EXPECT_FALSE(r.image); EXPECT_EQ(r.failure, ThumbnailFailure::Limits);
}

TEST_F(LibraryThumbnailNative, DegenerateAutomaticFilterRegionDoesNotRender)
{
    auto a = asset("<defs><filter id='f'><feGaussianBlur stdDeviation='1'/></filter></defs>"
        "<path d='M10 20 L70 20' stroke='red' filter='url(#f)'/>");
    auto r = render_library_thumbnail(pool, a, {});
    EXPECT_FALSE(r.image); EXPECT_EQ(r.failure, ThumbnailFailure::Limits);
}

TEST_F(LibraryThumbnailNative, RepeatedFilterUseChargesEachInvocation)
{
    auto single = asset(filtered("<feGaussianBlur stdDeviation='1'/>"));
    auto a = render_library_thumbnail(pool, single, {96, 64, 1}); ASSERT_TRUE(a.image) << a.diagnostic;
    auto repeated = asset("<defs><filter id='f' inkscape:auto-region='false'><feGaussianBlur stdDeviation='1'/></filter></defs>"
        "<rect x='16' y='12' width='48' height='32' fill='#c43b67' filter='url(#f)'/>"
        "<rect x='16' y='12' width='48' height='32' fill='#c43b67' filter='url(#f)'/>");
    PreviewRenderBudget::Limits l; l.bytes = a.image->render_stats.bytes;
    auto b = render_library_thumbnail(pool, repeated, {96, 64, 1}, l);
    EXPECT_FALSE(b.image); EXPECT_EQ(b.failure, ThumbnailFailure::Limits);
}

TEST_F(LibraryThumbnailNative, OutputBoundAndCancellationPublishNoPartialTexture)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    auto r = render_library_thumbnail(pool, a, {513, 128, 1});
    EXPECT_EQ(r.failure, ThumbnailFailure::Limits); EXPECT_FALSE(r.image);
    r = render_library_thumbnail(pool, a, {}, {}, [] { return true; });
    EXPECT_EQ(r.failure, ThumbnailFailure::Cancelled); EXPECT_FALSE(r.image);
    EXPECT_EQ(PreviewRenderBudget::current(), nullptr);
    r = render_library_thumbnail(pool, a, {}); EXPECT_TRUE(r.image) << r.diagnostic;
}

TEST_F(LibraryThumbnailNative, CallerCanDestroyAdmittedWrapperDuringNativeOperation)
{
    std::optional<Lib::ValidatedSvg> a(asset("<rect width='96' height='64' fill='red'/>"));
    auto r = render_library_thumbnail(pool, *a, {}, {}, [&] { a.reset(); return false; });
    EXPECT_FALSE(a); ASSERT_TRUE(r.image) << r.diagnostic;
    EXPECT_FALSE(download(r.image->texture).empty());
}

TEST_F(LibraryThumbnailNative, VisibleDemandOnlyOneIdleAndCallbackMayDestroyOwner)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    auto service = std::make_unique<ArtworkLibraryThumbnails>(pool);
    std::shared_ptr<Thumbnail const> kept; unsigned calls = 0;
    service->set_visible({{a, {}} , {a, {}}}, [&](auto, auto i, auto r) {
        EXPECT_EQ(i, 0u); ++calls; kept = r.image; service.reset();
    });
    EXPECT_EQ(calls, 0u); EXPECT_EQ(service->pending_sources(), 1u);
    drain(context); EXPECT_EQ(calls, 1u); ASSERT_TRUE(kept);
    EXPECT_FALSE(download(kept->texture).empty());
}

TEST_F(LibraryThumbnailNative, SearchResizeAndDocumentChangesCancelOldGeneration)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    ArtworkLibraryThumbnails s(pool); unsigned calls = 0;
    for (auto reason : {ThumbnailInvalidation::Search, ThumbnailInvalidation::Resize,
                        ThumbnailInvalidation::DocumentChange, ThumbnailInvalidation::CollectionChange}) {
        auto g = s.set_visible({{a, {}}}, [&](auto, auto, auto) { ++calls; });
        s.invalidate(reason); EXPECT_GT(s.generation(), g); EXPECT_EQ(s.pending_sources(), 0u);
        drain(context); EXPECT_EQ(calls, 0u);
    }
}

TEST_F(LibraryThumbnailNative, ReentrantReplacementDoesNotNotifyOldSecondCell)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    ArtworkLibraryThumbnails s(pool); unsigned old = 0, fresh = 0;
    s.set_visible({{a, {}}, {a, {}}}, [&](auto, auto i, auto) {
        ++old; EXPECT_EQ(i, 0u);
        s.set_visible({{a, {64, 64, 2}}}, [&](auto, auto j, auto r) {
            ++fresh; EXPECT_EQ(j, 0u); ASSERT_TRUE(r.image);
            EXPECT_EQ(r.image->texture->get_width(), 128);
        });
    });
    drain(context); EXPECT_EQ(old, 1u); EXPECT_EQ(fresh, 1u); EXPECT_EQ(s.pending_sources(), 0u);
}

TEST_F(LibraryThumbnailNative, CacheKeysIncludeHashSizeAndDeviceScaleAndEvictBoundedly)
{
    auto red = asset("<rect width='96' height='64' fill='red'/>");
    auto blue = asset("<rect width='96' height='64' fill='blue'/>");
    constexpr std::size_t capacity = 96 * 64 * 4 + 16384; // One texture plus pool/result metadata.
    pool = ArtworkLibraryThumbnailPool::create(context, capacity);
    ArtworkLibraryThumbnails s(pool);
    std::shared_ptr<Thumbnail const> first, cached, other;
    auto page = [&](Lib::ValidatedSvg a, ThumbnailSize size, std::shared_ptr<Thumbnail const> &out) {
        s.set_visible({{a, size}}, [&](auto, auto, auto r) { out = r.image; });
        drain(context); ASSERT_TRUE(out); EXPECT_LE(s.cache_bytes(), capacity);
    };
    page(red, {96, 64, 1}, first); page(red, {96, 64, 1}, cached);
    EXPECT_EQ(first->texture, cached->texture);
    auto original = download(first->texture);
    auto allocations = pool->stats().backing_allocations;
    s.set_visible({{blue, {96, 64, 1}}}, [&](auto, auto, auto r) {
        EXPECT_FALSE(r.image); EXPECT_EQ(r.failure, ThumbnailFailure::Limits);
    });
    drain(context);
    EXPECT_EQ(pool->stats().backing_allocations, allocations); // Denied before allocation.
    first.reset(); cached.reset(); // Consumer release makes the old tile reclaimable.
    page(blue, {96, 64, 1}, other);
    EXPECT_NE(original, download(other->texture));
    other.reset();
    page(red, {48, 32, 2}, cached);
    EXPECT_EQ(pool->stats().backing_allocations, allocations + 2);
    // Same physical pixel count, but logical size/device scale differ in key.
    EXPECT_EQ(original, download(cached->texture));
    s.invalidate(ThumbnailInvalidation::FontsChanged);
    EXPECT_EQ(pool->stats().cache_entries, 0u);
    EXPECT_GT(pool->stats().externally_pinned, 0u);
    EXPECT_FALSE(download(cached->texture).empty()); // Consumer ownership survives eviction.
    cached.reset();
    EXPECT_EQ(pool->stats().pixel_bytes, 0u);
    EXPECT_EQ(pool->stats().externally_pinned, 0u);
}

TEST_F(LibraryThumbnailNative, InvalidDemandLeavesExistingPagePending)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    ArtworkLibraryThumbnails s(pool); unsigned calls = 0;
    auto generation = s.set_visible({{a, {}}}, [&](auto, auto, auto r) { ++calls; EXPECT_TRUE(r.image); });
    EXPECT_THROW(s.set_visible({{a, {0, 1, 1}}}, {}), PreviewRenderLimit);
    EXPECT_EQ(s.generation(), generation); EXPECT_EQ(s.pending_sources(), 1u);
    drain(context); EXPECT_EQ(calls, 1u);
}

TEST_F(LibraryThumbnailNative, FailedCellIsReportedOnceAndNextCellStillRenders)
{
    auto huge = asset(filtered("<feGaussianBlur stdDeviation='1'/>", "filterRes='4096 4096'"));
    auto small = asset("<rect width='96' height='64' fill='blue'/>");
    ArtworkLibraryThumbnails s(pool); unsigned calls = 0;
    s.set_visible({{huge, {32, 32, 1}}, {small, {32, 32, 1}}}, [&](auto, auto index, auto r) {
        ++calls;
        if (index == 0) { EXPECT_FALSE(r.image); EXPECT_EQ(r.failure, ThumbnailFailure::Limits); }
        else { EXPECT_EQ(index, 1u); EXPECT_TRUE(r.image) << r.diagnostic; }
    });
    drain(context); EXPECT_EQ(calls, 2u); EXPECT_EQ(s.pending_sources(), 0u);
}

TEST_F(LibraryThumbnailNative, NativeSourceAndHistoryUnaffectedByBrowsing)
{
    auto a = asset("<rect id='art' width='96' height='64' fill='red'/>");
    auto bytes = a.svg_bytes(); auto hash = Lib::artwork_sha256(Lib::Bytes(bytes->begin(), bytes->end()));
    auto destination = SPDocument::createNewDocFromMem(std::span<char const>(bytes->data(), bytes->size()));
    ASSERT_TRUE(destination);
    auto node = destination->getObjectById("art"); auto repr = node->getRepr();
    DocumentUndo::setUndoSensitive(destination.get(), true);
    repr->setAttribute("fill", "blue");
    DocumentUndo::done(destination.get(), Util::Internal::ContextString{"thumbnail history fixture"}, "");
    DocumentUndo::undo(destination.get()); // Retain a real Redo branch while browsing.
    ObjectSet selection(destination.get()); selection.add(node);
    auto selected = selection.items_vector();
    auto before = xml_identity(destination->getReprRoot());
    auto dirty = destination->isModifiedSinceSave(), sensitive = DocumentUndo::getUndoSensitive(destination.get());
    auto r = render_library_thumbnail(pool, a, {}); ASSERT_TRUE(r.image) << r.diagnostic;
    EXPECT_EQ(destination->getObjectById("art"), node); EXPECT_EQ(node->getRepr(), repr);
    EXPECT_EQ(destination->isModifiedSinceSave(), dirty);
    EXPECT_EQ(DocumentUndo::getUndoSensitive(destination.get()), sensitive);
    EXPECT_EQ(xml_identity(destination->getReprRoot()), before);
    EXPECT_EQ(selection.items_vector(), selected);
    DocumentUndo::redo(destination.get());
    EXPECT_STREQ(repr->attribute("fill"), "blue");
    DocumentUndo::undo(destination.get());
    EXPECT_EQ(xml_identity(destination->getReprRoot()), before);
    EXPECT_EQ(Lib::artwork_sha256(Lib::Bytes(bytes->begin(), bytes->end())), hash);
}

// Additive source-review regressions. The observer below forwards to the real
// DrawingGroup implementation: it neither supplies pixels nor replaces a
// primitive. Cairo user-data destructors observe real allocation lifetimes.
struct ThumbnailReleaseCounts {
    // 0: DrawingSurface backing, 1: pushed Cairo group, 2: intermediate context.
    std::array<unsigned, 3> tagged{};
    std::array<unsigned, 3> released{};
    unsigned groups_deleted = 0;
};
struct ThumbnailReleaseToken {
    std::shared_ptr<ThumbnailReleaseCounts> counts;
    unsigned index;
};
void thumbnail_release_token(void *data)
{
    auto token = std::unique_ptr<ThumbnailReleaseToken>(static_cast<ThumbnailReleaseToken *>(data));
    ++token->counts->released[token->index];
}
cairo_user_data_key_t thumbnail_surface_probe_key;
cairo_user_data_key_t thumbnail_context_probe_key;

class ThumbnailObservedGroup final : public DrawingGroup {
public:
    ThumbnailObservedGroup(Drawing &drawing, std::shared_ptr<ThumbnailReleaseCounts> counts)
        : DrawingGroup(drawing), counts(std::move(counts)) {}
    ~ThumbnailObservedGroup() override { ++counts->groups_deleted; }

    bool tag_intermediates = false;
    mutable unsigned updates = 0, renders = 0;
    mutable PreviewRenderBudget *update_budget = nullptr, *render_budget = nullptr;

protected:
    unsigned _updateItem(Geom::IntRect const &area, UpdateContext const &ctx,
                         unsigned flags, unsigned reset) override
    {
        ++updates;
        update_budget = PreviewRenderBudget::current();
        return DrawingGroup::_updateItem(area, ctx, flags, reset);
    }
    unsigned _renderItem(DrawingContext &dc, RenderContext &rc, Geom::IntRect const &area,
                         unsigned flags, DrawingItem const *stop_at) const override
    {
        ++renders;
        render_budget = PreviewRenderBudget::current();
        if (tag_intermediates) {
            // Opacity forces the real DrawingItem intermediate/group path.
            // Check actual allocated pixel storage, not budget reservations.
            auto backing = dc.surface()->raw();
            auto group = dc.rawTarget();
            if (!backing || !group || backing == group)
                throw std::runtime_error("Expected distinct native intermediate and group");
            tag_surface(backing, 0);
            tag_surface(group, 1);
            auto token = std::make_unique<ThumbnailReleaseToken>(ThumbnailReleaseToken{counts, 2});
            if (cairo_status(dc.raw()) != CAIRO_STATUS_SUCCESS ||
                cairo_get_user_data(dc.raw(), &thumbnail_context_probe_key) ||
                cairo_set_user_data(dc.raw(), &thumbnail_context_probe_key,
                                    token.get(), thumbnail_release_token) != CAIRO_STATUS_SUCCESS)
                throw std::runtime_error("Could not observe native intermediate context");
            token.release();
            ++counts->tagged[2];
        }
        // The next child's real DrawingItem::render checkpoint cancels after
        // both buffers and their owning context have been observed alive.
        return DrawingGroup::_renderItem(dc, rc, area, flags, stop_at);
    }
private:
    void tag_surface(cairo_surface_t *surface, unsigned index) const
    {
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
            cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE ||
            cairo_image_surface_get_width(surface) <= 0 ||
            cairo_image_surface_get_height(surface) <= 0 ||
            !cairo_image_surface_get_data(surface) ||
            cairo_surface_get_user_data(surface, &thumbnail_surface_probe_key))
            throw std::runtime_error("Expected allocated untagged native image surface");
        auto token = std::make_unique<ThumbnailReleaseToken>(ThumbnailReleaseToken{counts, index});
        if (cairo_surface_set_user_data(surface, &thumbnail_surface_probe_key,
                                       token.get(), thumbnail_release_token) != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("Could not observe native intermediate surface");
        token.release();
        ++counts->tagged[index];
    }
    // Tokens retain this owner even if a regression leaks a surface. They do
    // not retain any surface/context, and never refer to a dead stack counter.
    std::shared_ptr<ThumbnailReleaseCounts> counts;
};

struct ThumbnailNativeHide {
    SPRoot *root;
    unsigned key;
    ~ThumbnailNativeHide() { root->invoke_hide(key); }
};
std::vector<unsigned char> thumbnail_draw_native(Drawing &drawing)
{
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, 96, 64);
    {
        DrawingSurface target(surface->cobj(), Geom::Point(0, 0));
        DrawingContext dc(target);
        drawing.render(dc, Geom::IntRect::from_xywh(0, 0, 96, 64), DrawingItem::RENDER_BYPASS_CACHE);
    }
    surface->flush();
    if (cairo_surface_status(surface->cobj()) != CAIRO_STATUS_SUCCESS)
        throw std::runtime_error("Native test output surface failed");
    std::vector<unsigned char> out;
    for (unsigned y = 0; y < 64; ++y)
        out.insert(out.end(), surface->get_data() + y * surface->get_stride(),
                   surface->get_data() + y * surface->get_stride() + 96 * 4);
    return out;
}

TEST_F(LibraryThumbnailNative, ServiceCancellationAfterIntermediateCreationRecovers)
{
    auto a = asset(filtered("<feGaussianBlur stdDeviation='1'/>"));
    ThumbnailSize size{96, 64, 1};
    auto expected = reference(a, size);
    ASSERT_TRUE(std::any_of(expected.begin(), expected.end(), [](auto c) { return c != 0; }));
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        SCOPED_TRACE(attempt);
        pool->invalidate_all(ThumbnailInvalidation::FontsChanged);
        bool reached_intermediate = false;
        PreviewRenderBudget::Stats observed;
        auto cancelled = render_library_thumbnail(pool, a, size, {}, [&] {
            auto budget = PreviewRenderBudget::current();
            if (!budget) return false;
            auto stats = budget->stats();
            // For this single filtered rect: output, DrawingSurface backing,
            // pushed group. The next checkpoint is Filter::render admission,
            // after the native context/group creation and shape drawing.
            // This is a phase witness, not an actual-allocation byte census;
            // the companion native test observes destruction directly.
            if (stats.surfaces < 3) return false;
            observed = stats;
            reached_intermediate = true;
            return true;
        });
        ASSERT_TRUE(reached_intermediate) << cancelled.diagnostic;
        EXPECT_GE(observed.surfaces, 3u);
        EXPECT_EQ(observed.filters, 0u); // Not a post-completion cancellation.
        EXPECT_GT(observed.bytes, std::size_t(96 * 64 * 4));
        EXPECT_EQ(cancelled.failure, ThumbnailFailure::Cancelled);
        EXPECT_FALSE(cancelled.image);
        EXPECT_EQ(PreviewRenderBudget::current(), nullptr);

        auto recovered = render_library_thumbnail(pool, a, size);
        ASSERT_TRUE(recovered.image) << recovered.diagnostic;
        EXPECT_EQ(recovered.failure, ThumbnailFailure::None);
        EXPECT_GT(recovered.image->render_stats.filters, 0u);
        EXPECT_EQ(download(recovered.image->texture), expected);
        std::weak_ptr<Thumbnail const> result_owner = recovered.image;
        recovered.image.reset();
        EXPECT_TRUE(result_owner.expired());
        EXPECT_EQ(PreviewRenderBudget::current(), nullptr);
    }
}

TEST_F(LibraryThumbnailNative, CancellationUnwindsActualIntermediateBuffersAndContext)
{
    auto a = asset("<rect x='8' y='8' width='72' height='48' fill='#34aadc'/>");
    auto bytes = a.svg_bytes();
    auto counts = std::make_shared<ThumbnailReleaseCounts>();
    bool document_destroyed = false;
    {
        auto doc = SPDocument::createNewDocFromMem(std::span<char const>(bytes->data(), bytes->size()));
        ASSERT_TRUE(doc);
        doc->connectDestroy([&] { document_destroyed = true; });
        doc->ensureUpToDate();
        Drawing drawing;
        drawing.setCacheBudget(0);
        auto key = SPItem::display_key_new(1);
        auto root = doc->getRoot();
        auto shown = root->invoke_show(drawing, key, SP_ITEM_SHOW_DISPLAY);
        ThumbnailNativeHide hide{root, key};
        ASSERT_TRUE(shown);
        auto observed = new ThumbnailObservedGroup(drawing, counts);
        drawing.setRoot(observed);
        observed->setOpacity(.5f);
        observed->appendChild(shown);
        drawing.setExact();
        drawing.setDithering(false);
        EXPECT_FALSE(drawing.useDithering());
        drawing.update();
        auto expected = thumbnail_draw_native(drawing);
        ASSERT_TRUE(std::any_of(expected.begin(), expected.end(), [](auto c) { return c != 0; }));

        bool cancel_after_tagging = true;
        bool reached_live_resources = false;
        PreviewRenderBudget budget({}, [&] {
            if (!cancel_after_tagging || counts->tagged != std::array<unsigned, 3>{1, 1, 1})
                return false;
            reached_live_resources = true;
            EXPECT_EQ(counts->released, (std::array<unsigned, 3>{0, 0, 0}));
            return true;
        });
        budget.bind_drawing(&drawing);
        observed->tag_intermediates = true;
        EXPECT_THROW(thumbnail_draw_native(drawing), PreviewRenderCancelled);
        ASSERT_TRUE(reached_live_resources);
        EXPECT_EQ(counts->tagged, (std::array<unsigned, 3>{1, 1, 1}));
        // Assert release now, not only after the document/drawing is destroyed.
        EXPECT_EQ(counts->released, counts->tagged);
        EXPECT_EQ(PreviewRenderBudget::current(), &budget);
        EXPECT_FALSE(document_destroyed);
        EXPECT_EQ(counts->groups_deleted, 0u);

        cancel_after_tagging = false;
        observed->tag_intermediates = false;
        EXPECT_EQ(thumbnail_draw_native(drawing), expected); // Same native tree, after unwinding.
        EXPECT_EQ(counts->released, counts->tagged);
    }
    EXPECT_TRUE(document_destroyed);
    EXPECT_EQ(counts->groups_deleted, 1u);
    EXPECT_EQ(counts->released, (std::array<unsigned, 3>{1, 1, 1}));
    EXPECT_EQ(PreviewRenderBudget::current(), nullptr);
}

TEST_F(LibraryThumbnailNative, ActualUnrelatedDrawingUpdateAndRenderIgnoreForeignBudget)
{
    auto a = asset(filtered("<feGaussianBlur stdDeviation='1'/>"));
    auto bytes = a.svg_bytes();
    auto doc = SPDocument::createNewDocFromMem(std::span<char const>(bytes->data(), bytes->size()));
    ASSERT_TRUE(doc);
    doc->ensureUpToDate();
    Drawing ordinary, preview;
    ordinary.setCacheBudget(0);
    auto key = SPItem::display_key_new(1);
    auto root = doc->getRoot();
    auto shown = root->invoke_show(ordinary, key, SP_ITEM_SHOW_DISPLAY);
    ThumbnailNativeHide hide{root, key};
    ASSERT_TRUE(shown);
    auto observed = new ThumbnailObservedGroup(ordinary, std::make_shared<ThumbnailReleaseCounts>());
    ordinary.setRoot(observed);
    observed->setOpacity(.5f);
    observed->appendChild(shown);
    ordinary.setExact();
    ordinary.setDithering(false);
    EXPECT_FALSE(ordinary.useDithering());
    ordinary.update();
    auto expected = thumbnail_draw_native(ordinary);
    ASSERT_TRUE(std::any_of(expected.begin(), expected.end(), [](auto c) { return c != 0; }));
    observed->updates = observed->renders = 0;

    {
        PreviewRenderBudget::Limits limits;
        limits.bytes = 4; limits.dimension = 1; limits.kernel = .01;
        unsigned checkpoints = 0;
        PreviewRenderBudget budget(limits, [&] { ++checkpoints; return false; });
        budget.bind_drawing(&preview);
        // No manual DrawingScope. Force actual tree update, even though the
        // same native tree was previously used for the unbudgeted oracle.
        EXPECT_NO_THROW(ordinary.update(Geom::IntRect::infinite(), Geom::identity(),
                                       DrawingItem::STATE_ALL, DrawingItem::STATE_ALL));
        EXPECT_GT(observed->updates, 0u);
        EXPECT_EQ(observed->update_budget, nullptr);
        EXPECT_EQ(PreviewRenderBudget::current(), &budget);
        EXPECT_EQ(thumbnail_draw_native(ordinary), expected);
        EXPECT_GT(observed->renders, 0u);
        EXPECT_EQ(observed->render_budget, nullptr);
        EXPECT_EQ(PreviewRenderBudget::current(), &budget);
        EXPECT_EQ(checkpoints, 0u);
        auto stats = budget.stats();
        EXPECT_EQ(stats.bytes, 0u); EXPECT_EQ(stats.work, 0u);
        EXPECT_EQ(stats.surfaces, 0u); EXPECT_EQ(stats.filters, 0u);
        EXPECT_EQ(stats.maximum_dimension, 0u);

        // Positive control: policy and callback resume immediately outside
        // the unrelated Drawing call. Isolation must not disable the owner.
        EXPECT_THROW(PreviewRenderBudget::surface(2, 2), PreviewRenderLimit);
        EXPECT_EQ(checkpoints, 1u);
        EXPECT_EQ(PreviewRenderBudget::current(), &budget);
    }
    EXPECT_EQ(PreviewRenderBudget::current(), nullptr);
}
void expect_pool_invariant(ArtworkLibraryThumbnailPool::Stats const &s)
{
    EXPECT_EQ(s.charged, s.reserved + s.resident);
    EXPECT_LE(s.charged, s.capacity);
    EXPECT_LE(s.capacity, ArtworkLibraryThumbnailPool::hard_limit);
    EXPECT_LE(s.externally_pinned, s.resident);
    EXPECT_LE(s.pixel_bytes, s.charged);
    EXPECT_EQ(s.backing_allocations - s.backing_releases, s.backings);
    EXPECT_LE(s.backing_owners, s.backings);
    EXPECT_LE(s.active_jobs, 1u);
}

TEST_F(LibraryThumbnailNative, TwoPanelsJointlyExhaustTheApplicationPoolBeforeAllocation)
{
    ArtworkLibraryThumbnails left(pool), right(pool);
    std::vector<ThumbnailDemand> a, b;
    for (unsigned i = 0; i < 8; ++i) {
        auto svg = asset("<rect width='96' height='64' fill='rgb(" + std::to_string(i + 1) + ",30,60)'/>");
        (i < 4 ? a : b).push_back({svg, {512, 512, 4}});
    }
    std::vector<Glib::RefPtr<Gdk::Texture>> kept;
    std::optional<ThumbnailDemand> refused;
    unsigned calls = 0, limits = 0;
    auto callback = [&](std::vector<ThumbnailDemand> const &source) {
      return [&, source = &source](auto, auto index, auto r) {
        ++calls; expect_pool_invariant(pool->stats());
        if (r.image) kept.push_back(r.image->texture); // ONLY the actual texture.
        else {
            EXPECT_EQ(r.failure, ThumbnailFailure::Limits); ++limits;
            refused = (*source)[index];
            EXPECT_EQ(pool->stats().backing_allocations, 7u);
        }
      };
    };
    left.set_visible(a, callback(a)); right.set_visible(b, callback(b));
    drain(context);
    EXPECT_EQ(calls, 8u); EXPECT_EQ(limits, 1u);
    ASSERT_EQ(kept.size(), 7u);
    auto s = pool->stats();
    EXPECT_EQ(s.pixel_bytes, 7u * 2048 * 2048 * 4);
    EXPECT_EQ(s.backing_allocations, 7u);
    EXPECT_EQ(s.backing_owners, 7u);
    EXPECT_EQ(s.backing_releases, 0u);
    EXPECT_GT(s.externally_pinned, s.pixel_bytes);
    RecordProperty("joint_retained_pixels", s.pixel_bytes);
    RecordProperty("joint_charged", s.charged);
    // Four tiles per panel are <128 MiB individually; their joint eighth
    // backing is refused. Releasing one makes one real backing reclaimable.
    kept.erase(kept.begin());
    unsigned retried = 0;
    ASSERT_TRUE(refused);
    right.set_visible({*refused}, [&](auto, auto, auto r) {
        ++retried; ASSERT_TRUE(r.image) << r.diagnostic; kept.push_back(r.image->texture);
    });
    drain(context); EXPECT_EQ(retried, 1u);
    EXPECT_EQ(pool->stats().backing_allocations, 8u);
    EXPECT_EQ(pool->stats().backing_releases, 1u);
    pool->invalidate_all(ThumbnailInvalidation::Close);
    EXPECT_EQ(pool->stats().cache_entries, 0u);
    EXPECT_EQ(pool->stats().backings, 7u);
    kept.clear();
    expect_pool_invariant(pool->stats());
    EXPECT_EQ(pool->stats().backings, 0u);
    EXPECT_EQ(pool->stats().pixel_bytes, 0u);
}

TEST_F(LibraryThumbnailNative, TwoServicesShareBackingButNotDemandGenerations)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    ArtworkLibraryThumbnails left(pool), right(pool);
    std::shared_ptr<Thumbnail const> first, second;
    left.set_visible({{a, {}}}, [&](auto, auto, auto r) { first = r.image; });
    right.set_visible({{a, {}}}, [&](auto, auto, auto r) { second = r.image; });
    drain(context); ASSERT_TRUE(first); ASSERT_TRUE(second);
    EXPECT_EQ(first->texture, second->texture);
    EXPECT_EQ(pool->stats().backing_allocations, 1u);
    EXPECT_EQ(pool->stats().cache_entries, 1u);
    auto rg = right.generation();
    left.invalidate(ThumbnailInvalidation::Resize);
    EXPECT_EQ(right.generation(), rg);
    EXPECT_EQ(pool->stats().cache_entries, 1u);
    unsigned stale = 0;
    right.set_visible({{a, {}}}, [&](auto, auto, auto) { ++stale; });
    left.invalidate(ThumbnailInvalidation::FontsChanged);
    EXPECT_GT(right.generation(), rg);
    EXPECT_EQ(right.pending_sources(), 0u);
    drain(context); EXPECT_EQ(stale, 0u);
    EXPECT_EQ(pool->stats().cache_entries, 0u);
    EXPECT_EQ(pool->stats().backings, 1u);
    EXPECT_EQ(download(first->texture), download(second->texture));
}

TEST_F(LibraryThumbnailNative, TextureOnlySurvivesEvictionServiceDestructionAndWorkerRelease)
{
    auto baseline = pool->stats().charged;
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    Glib::RefPtr<Gdk::Texture> texture;
    {
        ArtworkLibraryThumbnails service(pool);
        service.set_visible({{a, {96, 64, 1}}}, [&](auto, auto, auto r) {
            ASSERT_TRUE(r.image) << r.diagnostic; texture = r.image->texture;
        });
        drain(context);
    }
    ASSERT_TRUE(texture);
    pool->invalidate_all(ThumbnailInvalidation::DocumentChange);
    auto held = pool->stats();
    EXPECT_EQ(held.cache_entries, 0u);
    EXPECT_EQ(held.reserved, 0u);
    EXPECT_EQ(held.pixel_bytes, 96u * 64 * 4);
    EXPECT_EQ(held.backing_owners, 1u);
    EXPECT_EQ(held.externally_pinned, held.resident - baseline);
    auto expected = reference(a, {96, 64, 1});
    EXPECT_EQ(download(texture), expected);
    std::thread release([owned = std::move(texture)]() mutable { owned.reset(); });
    release.join();
    auto released = pool->stats();
    expect_pool_invariant(released);
    EXPECT_EQ(released.charged, baseline);
    EXPECT_EQ(released.externally_pinned, 0u);
    EXPECT_EQ(released.backing_releases, 1u);
    EXPECT_EQ(released.backing_owners, 0u);
}

TEST_F(LibraryThumbnailNative, PoolAndContextTeardownDoNotOwnConsumerTextureFinalizer)
{
    auto local_context = g_main_context_new();
    auto local = ArtworkLibraryThumbnailPool::create(local_context);
    std::weak_ptr<ArtworkLibraryThumbnailPool> weak = local;
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    auto r = render_library_thumbnail(local, a, {96, 64, 1});
    ASSERT_TRUE(r.image) << r.diagnostic;
    auto texture = r.image->texture;
    bool finalized = false;
    g_object_weak_ref(G_OBJECT(texture->gobj()), [](gpointer p, GObject *) {
        *static_cast<bool *>(p) = true;
    }, &finalized);
    r.image.reset();
    local->invalidate_all(ThumbnailInvalidation::Close);
    local.reset(); g_main_context_unref(local_context);
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(finalized);
    EXPECT_EQ(download(texture), reference(a, {96, 64, 1}));
    std::thread release([owned = std::move(texture)]() mutable { owned.reset(); });
    release.join();
    EXPECT_TRUE(finalized); // No live app/context required by the finalizer.
}

TEST_F(LibraryThumbnailNative, ConcurrentLastConsumerReleaseAndCacheRemovalHaveNoCallbackAddressLifetime)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    auto baseline = pool->stats().charged;
    for (unsigned i = 0; i < 64; ++i) {
        SCOPED_TRACE(i);
        auto r = render_library_thumbnail(pool, a, {16, 16, 1});
        ASSERT_TRUE(r.image) << r.diagnostic;
        auto texture = r.image->texture; r.image.reset();
        std::barrier start(2);
        std::thread release([owned = std::move(texture), &start]() mutable {
            start.arrive_and_wait(); owned.reset();
        });
        start.arrive_and_wait();
        pool->invalidate_all(ThumbnailInvalidation::FontsChanged);
        release.join();
        auto s = pool->stats(); expect_pool_invariant(s);
        EXPECT_EQ(s.charged, baseline);
        EXPECT_EQ(s.backings, 0u);
        EXPECT_EQ(s.backing_allocations, i + 1u);
        EXPECT_EQ(s.backing_releases, i + 1u);
    }
}

TEST_F(LibraryThumbnailNative, CacheHitMustReserveItsResultMetadataBeforePublication)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    auto measured = render_library_thumbnail(pool, a, {16, 16, 1});
    ASSERT_TRUE(measured.image) << measured.diagnostic;
    auto exact = ArtworkLibraryThumbnailPool::create(context, pool->stats().charged);
    auto first = render_library_thumbnail(exact, a, {16, 16, 1});
    ASSERT_TRUE(first.image) << first.diagnostic;
    auto full = exact->stats();
    EXPECT_EQ(full.charged, full.capacity);
    auto denied = render_library_thumbnail(exact, a, {16, 16, 1});
    EXPECT_FALSE(denied.image); EXPECT_EQ(denied.failure, ThumbnailFailure::Limits);
    EXPECT_EQ(exact->stats().backing_allocations, 1u);
    EXPECT_EQ(exact->stats().charged, full.charged);
    auto texture_identity = first.image->texture->gobj(); first.image.reset();
    auto retry = render_library_thumbnail(exact, a, {16, 16, 1});
    ASSERT_TRUE(retry.image) << retry.diagnostic;
    EXPECT_EQ(retry.image->texture->gobj(), texture_identity);
    EXPECT_EQ(exact->stats().backing_allocations, 1u);
}

TEST_F(LibraryThumbnailNative, TinyPinnedTilesStillPayForKeysWarningsAndLruMetadata)
{
    auto small = ArtworkLibraryThumbnailPool::create(context, 64 * 1024);
    std::vector<Glib::RefPtr<Gdk::Texture>> kept;
    bool denied = false;
    for (unsigned i = 0; i < 32; ++i) {
        auto a = asset("<rect width='96' height='64' fill='rgb(" + std::to_string(i) + ",20,40)'/>");
        auto before = small->stats();
        auto r = render_library_thumbnail(small, a, {1, 1, 1});
        if (!r.image) {
            EXPECT_EQ(r.failure, ThumbnailFailure::Limits);
            EXPECT_EQ(small->stats().backing_allocations, before.backing_allocations);
            denied = true; break;
        }
        kept.push_back(r.image->texture);
        expect_pool_invariant(small->stats());
        EXPECT_GT(small->stats().charged - small->stats().pixel_bytes, 4096u);
    }
    EXPECT_TRUE(denied); EXPECT_FALSE(kept.empty());
    EXPECT_LT(kept.size(), 32u);
    EXPECT_EQ(small->stats().pixel_bytes, 4u * kept.size());
    EXPECT_EQ(small->stats().cache_entries, kept.size());
    kept.clear();
    small->invalidate_all(ThumbnailInvalidation::Close);
    EXPECT_EQ(small->stats().pixel_bytes, 0u);
    EXPECT_EQ(small->stats().externally_pinned, 0u);
}

TEST_F(LibraryThumbnailNative, NullTinyCapacityAndOverflowCannotAllocateUnchargedFallback)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    EXPECT_THROW(ArtworkLibraryThumbnails(nullptr), std::invalid_argument);
    EXPECT_FALSE(render_library_thumbnail(nullptr, a, {}).image);
    EXPECT_THROW(ArtworkLibraryThumbnailPool::create(context, 0), PreviewRenderLimit);
    EXPECT_THROW(ArtworkLibraryThumbnailPool::create(context, ArtworkLibraryThumbnailPool::hard_limit + 1),
                 PreviewRenderLimit);
    auto tiny = ArtworkLibraryThumbnailPool::create(context, pool->stats().charged);
    auto r = render_library_thumbnail(tiny, a, {1, 1, 1});
    EXPECT_EQ(r.failure, ThumbnailFailure::Limits); EXPECT_FALSE(r.image);
    EXPECT_EQ(tiny->stats().backing_allocations, 0u);
    for (auto size : {ThumbnailSize{std::numeric_limits<unsigned>::max(), 1, 4},
                      ThumbnailSize{512, 512, std::numeric_limits<unsigned>::max()},
                      ThumbnailSize{512, 513, 1}, ThumbnailSize{1, 1, 0}}) {
        auto failed = render_library_thumbnail(pool, a, size);
        EXPECT_EQ(failed.failure, ThumbnailFailure::Limits); EXPECT_FALSE(failed.image);
        expect_pool_invariant(pool->stats());
        EXPECT_EQ(pool->stats().backing_allocations, 0u);
    }
}

TEST_F(LibraryThumbnailNative, CancellationAtReservationAndPublicationReleasesEverything)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    auto baseline = pool->stats().charged;
    bool reserved = false;
    auto early = render_library_thumbnail(pool, a, {}, {}, [&] {
        auto s = pool->stats(); expect_pool_invariant(s);
        if (s.reserved == 0) return false;
        reserved = true; EXPECT_EQ(s.backing_allocations, 0u); return true;
    });
    EXPECT_TRUE(reserved); EXPECT_EQ(early.failure, ThumbnailFailure::Cancelled);
    EXPECT_FALSE(early.image); EXPECT_EQ(pool->stats().charged, baseline);
    bool allocated = false;
    // Observe actual texture construction, not a guessed native checkpoint
    // count. The publication checkpoint runs after all retained allocations.
    unsigned post_surface = 0;
    auto late = render_library_thumbnail(pool, a, {}, {}, [&] {
        auto s = pool->stats(); expect_pool_invariant(s);
        if (!s.backings) return false;
        allocated = true;
        // Cairo output creation precedes native render checkpoints. Rather
        // than guessing their count, cancel when completed bytes AND the
        // allocated texture can be observed via the dedicated counter below.
        ++post_surface;
        return s.texture_allocations > 0;
    });
    EXPECT_TRUE(allocated); EXPECT_GT(post_surface, 0u);
    EXPECT_EQ(late.failure, ThumbnailFailure::Cancelled); EXPECT_FALSE(late.image);
    auto s = pool->stats(); expect_pool_invariant(s);
    EXPECT_EQ(s.charged, baseline); EXPECT_EQ(s.reserved, 0u);
    EXPECT_EQ(s.backing_allocations, 1u); EXPECT_EQ(s.backing_releases, 1u);
    EXPECT_EQ(s.cache_entries, 0u);
    auto retry = render_library_thumbnail(pool, a, {});
    ASSERT_TRUE(retry.image) << retry.diagnostic;
    EXPECT_EQ(download(retry.image->texture), reference(a, {}));
}

TEST_F(LibraryThumbnailNative, CacheHitCancellationAndWeakResultMetadataStayAccounted)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    auto r = render_library_thumbnail(pool, a, {});
    ASSERT_TRUE(r.image) << r.diagnostic;
    auto before = pool->stats();
    bool reserved = false;
    auto hit = render_library_thumbnail(pool, a, {}, {}, [&] {
        reserved |= pool->stats().reserved > 0; return reserved;
    });
    EXPECT_TRUE(reserved); EXPECT_EQ(hit.failure, ThumbnailFailure::Cancelled);
    EXPECT_FALSE(hit.image);
    EXPECT_EQ(pool->stats().charged, before.charged);
    EXPECT_EQ(pool->stats().backing_allocations, before.backing_allocations);
    auto baseline = ArtworkLibraryThumbnailPool::create(context)->stats().charged;
    std::weak_ptr<Thumbnail const> weak = r.image;
    r.image.reset(); pool->invalidate_all(ThumbnailInvalidation::FontsChanged);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(pool->stats().backings, 0u);
    EXPECT_GT(pool->stats().charged, baseline); // Weak control block is real retained metadata.
    weak.reset();
    EXPECT_EQ(pool->stats().charged, baseline);
}

TEST_F(LibraryThumbnailNative, GlobalGateRejectsNestedRenderEvenWithMaskedThreadLocalBudget)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    auto other_pool = ArtworkLibraryThumbnailPool::create(context);
    bool attempted = false;
    auto r = render_library_thumbnail(pool, a, {}, {}, [&] {
        if (attempted) return false;
        attempted = true;
        Drawing ordinary;
        PreviewRenderBudget::DrawingScope scope(&ordinary);
        EXPECT_EQ(PreviewRenderBudget::current(), nullptr);
        auto nested = render_library_thumbnail(other_pool, a, {});
        EXPECT_EQ(nested.failure, ThumbnailFailure::Limits); EXPECT_FALSE(nested.image);
        EXPECT_EQ(other_pool->stats().backing_allocations, 0u);
        EXPECT_EQ(pool->stats().active_jobs, 1u);
        return false;
    });
    EXPECT_TRUE(attempted); ASSERT_TRUE(r.image) << r.diagnostic;
    EXPECT_EQ(pool->stats().active_jobs, 0u);
    EXPECT_EQ(other_pool->stats().active_jobs, 0u);
}

TEST_F(LibraryThumbnailNative, CallbackInvalidationAndPoolOwnerResetCannotPublishStalePixels)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    for (auto reason : {ThumbnailInvalidation::FontsChanged, ThumbnailInvalidation::Close}) {
        auto local = ArtworkLibraryThumbnailPool::create(context);
        auto r = render_library_thumbnail(local, a, {}, {}, [&] {
            if (local && local->stats().reserved) {
                local->invalidate_all(reason); local.reset();
            }
            return false; // The engine must recheck epoch AFTER this callback.
        });
        EXPECT_FALSE(local);
        EXPECT_FALSE(r.image); EXPECT_EQ(r.failure, ThumbnailFailure::Cancelled);
    }
}

TEST_F(LibraryThumbnailNative, WarningStorageGrowthAndStricterPolicyHaveIndependentAdmission)
{
    auto plain = asset("<rect width='96' height='64' fill='red'/>");
    auto annotated = asset(filtered("<feGaussianBlur stdDeviation='1'/>") +
        "<text x='3' y='20' font-size='12'>Warnings</text>");
    auto r = render_library_thumbnail(pool, plain, {96, 64, 1});
    ASSERT_TRUE(r.image) << r.diagnostic;
    auto plain_charge = pool->stats().charged;
    auto tiny = ArtworkLibraryThumbnailPool::create(context, plain_charge);
    auto too_big = render_library_thumbnail(tiny, annotated, {96, 64, 1});
    EXPECT_EQ(too_big.failure, ThumbnailFailure::Limits); EXPECT_FALSE(too_big.image);
    EXPECT_EQ(tiny->stats().backing_allocations, 0u); // Includes warning metadata BEFORE native pixels.
    auto rich_pool = ArtworkLibraryThumbnailPool::create(context);
    auto rich = render_library_thumbnail(rich_pool, annotated, {96, 64, 1});
    ASSERT_TRUE(rich.image) << rich.diagnostic;
    EXPECT_GT(rich_pool->stats().charged, plain_charge);
    auto warnings = annotated.warnings();
    ASSERT_EQ(rich.image->warnings.size(), warnings.size());
    ASSERT_EQ(warnings.size(), 2u);
    rich_pool->invalidate_all(ThumbnailInvalidation::FontsChanged);
    for (std::size_t i = 0; i < warnings.size(); ++i) EXPECT_EQ(rich.image->warnings[i], warnings[i]);
    PreviewRenderBudget::Limits strict; strict.bytes = 4;
    auto allocations = pool->stats().backing_allocations;
    auto denied = render_library_thumbnail(pool, plain, {96, 64, 1}, strict);
    EXPECT_EQ(denied.failure, ThumbnailFailure::Limits); EXPECT_FALSE(denied.image);
    EXPECT_EQ(pool->stats().backing_allocations, allocations);
    EXPECT_EQ(pool->stats().reserved, 0u); // Cannot reuse the loose-policy hit.
}

TEST_F(LibraryThumbnailNative, DemandCountBytesAndCallerCapacityRemainBoundedTransactionally)
{
    auto a = asset("<rect width='96' height='64' fill='red'/>");
    ArtworkLibraryThumbnails service(pool); unsigned calls = 0;
    auto epoch = service.set_visible({{a, {}}}, [&](auto, auto, auto r) { ++calls; EXPECT_TRUE(r.image); });
    std::vector<ThumbnailDemand> too_many(129, {a, {}});
    EXPECT_THROW(service.set_visible(too_many, {}), PreviewRenderLimit);
    auto large = asset("<!--" + std::string(600000, 'x') + "--><rect width='2' height='2'/>");
    std::vector<ThumbnailDemand> too_big(128, {large, {}});
    EXPECT_THROW(service.set_visible(too_big, {}), PreviewRenderLimit);
    EXPECT_EQ(service.generation(), epoch);
    drain(context); EXPECT_EQ(calls, 1u);
    std::vector<ThumbnailDemand> oversized_capacity;
    oversized_capacity.reserve(10000); oversized_capacity.push_back({a, {}});
    service.set_visible(std::move(oversized_capacity), [&](auto, auto, auto r) { ++calls; EXPECT_TRUE(r.image); });
    drain(context); EXPECT_EQ(calls, 2u);
    EXPECT_EQ(pool->stats().backing_allocations, 1u);
}

TEST_F(LibraryThumbnailNative, ThousandAndTenThousandCatalogHandlesOnlyRenderReportedHighDpiViewport)
{
    for (std::size_t catalog_size : {1000u, 10000u}) {
        SCOPED_TRACE(catalog_size);
        pool->invalidate_all(ThumbnailInvalidation::CollectionChange);
        // A catalog of immutable handles is host-owned, not an engine work
        // queue. Only four reported visible cells are sent for each viewport.
        std::vector<Lib::ValidatedSvg> catalog;
        catalog.reserve(catalog_size);
        for (std::size_t i = 0; i < catalog_size; ++i) {
            catalog.push_back(asset("<rect id='row" + std::to_string(i) +
                "' width='96' height='64' fill='red'/>"));
        }
        ArtworkLibraryThumbnails service(pool);
        auto before = pool->stats().backing_allocations;
        auto start = std::chrono::steady_clock::now();
        unsigned calls = 0;
        for (std::size_t offset : {std::size_t(0), catalog_size / 2, catalog_size - 4}) {
            std::vector<ThumbnailDemand> demand;
            for (std::size_t i = 0; i < 4; ++i) demand.push_back({catalog[offset + i], {128, 128, 4}});
            service.set_visible(std::move(demand), [&](auto, auto, auto r) {
                ++calls; ASSERT_TRUE(r.image) << r.diagnostic;
                EXPECT_EQ(r.image->texture->get_width(), 512);
                EXPECT_EQ(r.image->texture->get_height(), 512);
            });
            EXPECT_EQ(service.pending_sources(), 1u);
            drain(context); EXPECT_EQ(service.pending_sources(), 0u);
            expect_pool_invariant(pool->stats());
        }
        EXPECT_EQ(calls, 12u);
        EXPECT_EQ(pool->stats().backing_allocations - before, 12u);
        EXPECT_EQ(pool->stats().backings, 12u);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        RecordProperty("viewport_" + std::to_string(catalog_size) + "_us", std::to_string(elapsed));
        RecordProperty("viewport_" + std::to_string(catalog_size) + "_charged", pool->stats().charged);
    }
}
} // namespace
