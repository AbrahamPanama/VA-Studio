// SPDX-License-Identifier: GPL-2.0-or-later
#include "welcome-drawing-preview.h"

#include "display/cairo-utils.h"
#include "display/drawing-context.h"
#include "display/drawing-item.h"
#include "display/drawing.h"
#include "colors/color.h"
#include "document.h"
#include "object/sp-clippath.h"
#include "object/sp-defs.h"
#include "object/sp-item.h"
#include "object/sp-marker.h"
#include "object/sp-mask.h"
#include "object/sp-page.h"
#include "object/sp-pattern.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "object/sp-symbol.h"
#include "object/sp-text.h"
#include "object/sp-flowtext.h"
#include "page-manager.h"
#include "util/cast.h"
#include "xml/node.h"
#include "io/preview-xml-input.h"
#include "io/preview-svg-resource-policy.h"
#include "io/stream/bufferstream.h"
#include "inkscape.h"
#include "inkgc/gc-core.h"
#include "path-prefix.h"
#include "util/statics.h"
#include "xml/document.h"
#ifdef __APPLE__
#include "io/macos-finder-icon.h"
#endif

#include <2geom/transforms.h>
#include <cairo.h>
#include <gio/gio.h>
#include <gtkmm/application.h>
#include <glibmm/main.h>
#include <zlib.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <filesystem>
#include <exception>
#include <iterator>
#include <new>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_set>
#include <utility>
#include <optional>
#include <string_view>
#include <vector>
#include <deque>
#include <unordered_map>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <utime.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include "io/macos-bundle-bootstrap.h"
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace Inkscape::UI::Cache {
namespace {

// The classifier never walks more native objects than this, matching the
// proven artwork-library native traversal cap. It is an admission cap, not a
// promise about render cost; the PreviewRenderBudget accounts the render.
constexpr std::size_t native_object_limit = 100000;

// Physical output envelope: each side in [1, 2048] and the ARGB32 buffer no
// larger than 16 MiB, so 2048x2048 is the exact maximum. No second pool is
// created; the native PreviewRenderBudget still charges the allocation.
constexpr unsigned maximum_dimension = 2048;
constexpr std::size_t maximum_output_bytes = 16u * 1024u * 1024u;

// Contain-fit padding, 5% on each side: s = min(0.9W/w, 0.9H/h).
constexpr double content_fraction = 0.9;

struct PreviewShown {
    SPRoot *root;
    unsigned key;
    ~PreviewShown() { root->invoke_hide(key); }
};

// Unreferenced definitions and clip/mask/marker/pattern definitions are not
// painted where they are authored. A <symbol> is normally a def, but when it is
// instantiated by <use> the native clone attached under SPUse has cloned==1 and
// SPSymbol::show paints it (sp-symbol.cpp) and SPUse::href_changed attaches it
// (sp-use.cpp). Only skip an *uninstantiated* symbol; a cloned symbol's visible
// descendants must be walked like any other rendered subtree.
bool skip_unrendered(SPObject *object)
{
    if (auto *symbol = cast<SPSymbol>(object)) {
        return !symbol->cloned;
    }
    return is<SPDefs>(object) || is<SPClipPath>(object) || is<SPMask>(object) ||
           is<SPMarker>(object) || is<SPPattern>(object);
}

struct ClassifyOutcome {
    WelcomePreviewLimitation limitation = WelcomePreviewLimitation::None;
    WelcomePreviewStatus status = WelcomePreviewStatus::Rendered;
};

/**
 * Bounded native-tree classifier. It inspects only native computed SPItem style
 * and native object identity. It never reconstructs SVG/CSS semantics and never
 * expands a paint server or filter. Any known case where native bounds and the
 * painted result diverge is rejected with a fixed reason rather than presented
 * as exact.
 *
 * The active budget's cancellation checkpoint is sampled inside this bounded
 * walk, not only before/after it, so a large tree can stop mid-traversal.
 */
ClassifyOutcome classify_native_tree(SPDocument &document, PreviewRenderBudget &budget)
{
    ClassifyOutcome outcome;
    std::unordered_set<SPObject const *> seen;
    std::vector<SPObject *> todo;
    todo.push_back(document.getRoot());
    std::size_t visited = 0;
    while (!todo.empty()) {
        budget.checkpoint(); // Mid-traversal cancellation checkpoint.
        SPObject *object = todo.back();
        todo.pop_back();
        if (!object) {
            continue;
        }
        if (!seen.insert(object).second) {
            continue; // Repeated/cyclic native pointer.
        }
        if (++visited > native_object_limit) {
            outcome.limitation = WelcomePreviewLimitation::ObjectCountLimit;
            outcome.status = WelcomePreviewStatus::Limits;
            return outcome;
        }
        if (skip_unrendered(object)) {
            continue;
        }
        if (auto *item = cast<SPItem>(object)) {
            if (!item->style) {
                continue;
            }
            // display:none subtree is not painted and native bbox skips it.
            if (item->style->display.computed == SP_CSS_DISPLAY_NONE) {
                continue;
            }
            // A nested <svg> maps to SPRoot and is not clipped to its authored
            // viewport by this native baseline. The real document root is fine.
            if (item != document.getRoot() && is<SPRoot>(item)) {
                outcome.limitation = WelcomePreviewLimitation::NestedViewport;
                outcome.status = WelcomePreviewStatus::UnsupportedNativeSemantics;
                return outcome;
            }
            // Native baseline paints visibility:hidden, which contradicts the
            // authored semantics; do not present it as exact.
            if (item->style->visibility.computed != SP_CSS_VISIBILITY_VISIBLE) {
                outcome.limitation = WelcomePreviewLimitation::VisibilityNotVisible;
                outcome.status = WelcomePreviewStatus::UnsupportedNativeSemantics;
                return outcome;
            }
            // opacity:0 geometry inflates the native envelope but is unpainted.
            if (item->style->opacity.as_double() <= 0.0) {
                outcome.limitation = WelcomePreviewLimitation::ItemOpacityZero;
                outcome.status = WelcomePreviewStatus::UnsupportedNativeSemantics;
                return outcome;
            }
            if (auto *shape = cast<SPShape>(item)) {
                auto const &fill = item->style->fill;
                auto const &stroke = item->style->stroke;
                bool const paints_fill = !fill.isNone();
                bool const paints_stroke = !stroke.isNone();
                double const fill_opacity = item->style->fill_opacity.as_double();
                double const stroke_opacity = item->style->stroke_opacity.as_double();
                // Zero-alpha paint still contributes native geometry, so a far
                // transparent object can dominate the envelope without painting.
                if (paints_fill &&
                    (fill_opacity <= 0.0 || (fill.isColor() && fill.getColor().getOpacity() <= 0.0))) {
                    outcome.limitation = WelcomePreviewLimitation::ZeroPaintAlpha;
                    outcome.status = WelcomePreviewStatus::UnsupportedNativeSemantics;
                    return outcome;
                }
                if (paints_stroke &&
                    (stroke_opacity <= 0.0 || (stroke.isColor() && stroke.getColor().getOpacity() <= 0.0))) {
                    outcome.limitation = WelcomePreviewLimitation::ZeroPaintAlpha;
                    outcome.status = WelcomePreviewStatus::UnsupportedNativeSemantics;
                    return outcome;
                }
                // A plain shape that paints nothing but still contributes native
                // geometry is unsupported. Filters and markers can paint, so
                // those stay supported without interpreting the filter itself.
                if (!paints_fill && !paints_stroke && !item->isFiltered() && shape->hasMarkers() == 0) {
                    outcome.limitation = WelcomePreviewLimitation::ShapePaintsNothing;
                    outcome.status = WelcomePreviewStatus::UnsupportedNativeSemantics;
                    return outcome;
                }
            } else if (is<SPText>(item) || is<SPFlowtext>(item)) {
                // Native text is a separate painted item type. A direct zero
                // fill/stroke opacity on the text item still contributes native
                // geometry while painting nothing, so it gets the same direct
                // zero-alpha rejection. No text layout/font semantics are
                // reconstructed here, and the per-tspan cascade is out of scope.
                auto const &fill = item->style->fill;
                auto const &stroke = item->style->stroke;
                if ((!fill.isNone() &&
                     (item->style->fill_opacity.as_double() <= 0.0 ||
                      (fill.isColor() && fill.getColor().getOpacity() <= 0.0))) ||
                    (!stroke.isNone() &&
                     (item->style->stroke_opacity.as_double() <= 0.0 ||
                      (stroke.isColor() && stroke.getColor().getOpacity() <= 0.0)))) {
                    outcome.limitation = WelcomePreviewLimitation::ZeroPaintAlpha;
                    outcome.status = WelcomePreviewStatus::UnsupportedNativeSemantics;
                    return outcome;
                }
            }
        }
        for (auto &child : object->children) {
            todo.push_back(&child);
        }
    }
    return outcome;
}

Inkscape::Colors::Color document_background_color(SPDocument &document)
{
    // Same namedview interpretation used by native export backgrounds, limited
    // to the document's pagecolor/pageopacity and composited onto white below.
    Inkscape::Colors::Color color(0xffffffff);
    if (auto *namedview = document.getReprNamedView()) {
        if (char const *pagecolor = namedview->attribute("pagecolor")) {
            if (auto parsed = Inkscape::Colors::Color::parse(pagecolor)) {
                color = *parsed;
            }
        }
        if (namedview->attribute("inkscape:pageopacity")) {
            color.addOpacity(namedview->getAttributeDouble("inkscape:pageopacity", 1.0));
        }
    }
    return color;
}

void paint_preview_background(DrawingContext &dc, WelcomePreviewBackground background, SPDocument &document)
{
    // Every mode starts from opaque white so the letterbox is always opaque and
    // document color is composited onto white exactly as export interprets it.
    dc.setOperator(CAIRO_OPERATOR_SOURCE);
    dc.setSource(1.0, 1.0, 1.0);
    dc.paint();
    dc.setOperator(CAIRO_OPERATOR_OVER);
    switch (background) {
    case WelcomePreviewBackground::White:
        break;
    case WelcomePreviewBackground::Checkerboard: {
        auto pattern = ink_cairo_pattern_create_checkerboard();
        dc.setSource(pattern->cobj());
        dc.paint();
        break;
    }
    case WelcomePreviewBackground::DocumentColor:
        dc.setSource(document_background_color(document));
        dc.paint();
        break;
    }
}

bool valid_output_size(WelcomeDrawingPreviewRequest const &request)
{
    if (request.width < 1 || request.height < 1 ||
        request.width > maximum_dimension || request.height > maximum_dimension) {
        return false;
    }
    std::size_t const pixels = static_cast<std::size_t>(request.width) * request.height;
    return pixels <= maximum_output_bytes / 4;
}

} // namespace

char const *welcomePreviewStatusName(WelcomePreviewStatus status) noexcept
{
    switch (status) {
    case WelcomePreviewStatus::Rendered: return "Rendered";
    case WelcomePreviewStatus::UnsupportedNativeSemantics: return "UnsupportedNativeSemantics";
    case WelcomePreviewStatus::Cancelled: return "Cancelled";
    case WelcomePreviewStatus::Limits: return "Limits";
    case WelcomePreviewStatus::InvalidGeometry: return "InvalidGeometry";
    case WelcomePreviewStatus::NativeFailure: return "NativeFailure";
    }
    return "InvalidGeometry";
}

char const *welcomePreviewLimitationName(WelcomePreviewLimitation limitation) noexcept
{
    switch (limitation) {
    case WelcomePreviewLimitation::None: return "None";
    case WelcomePreviewLimitation::VisibilityNotVisible: return "VisibilityNotVisible";
    case WelcomePreviewLimitation::ItemOpacityZero: return "ItemOpacityZero";
    case WelcomePreviewLimitation::ZeroPaintAlpha: return "ZeroPaintAlpha";
    case WelcomePreviewLimitation::ShapePaintsNothing: return "ShapePaintsNothing";
    case WelcomePreviewLimitation::NestedViewport: return "NestedViewport";
    case WelcomePreviewLimitation::ObjectCountLimit: return "ObjectCountLimit";
    case WelcomePreviewLimitation::MissingGeometry: return "MissingGeometry";
    case WelcomePreviewLimitation::NonFiniteGeometry: return "NonFiniteGeometry";
    case WelcomePreviewLimitation::DegenerateGeometry: return "DegenerateGeometry";
    case WelcomePreviewLimitation::AllocationLimit: return "AllocationLimit";
    }
    return "None";
}

WelcomeDrawingPreview renderWelcomeDrawingPreview(SPDocument &document, WelcomeDrawingPreviewRequest const &request)
{
    WelcomeDrawingPreview result;

    // Cheap envelope rejection before any budget or native work. No allocation
    // exists yet, so this is genuinely before allocation.
    if (!valid_output_size(request)) {
        result.status = WelcomePreviewStatus::Limits;
        result.limitation = WelcomePreviewLimitation::AllocationLimit;
        return result;
    }

    try {
        PreviewRenderBudget budget(request.limits, request.cancelled);
        budget.checkpoint();

        document.ensureUpToDate();
        budget.checkpoint();

        ClassifyOutcome const classified = classify_native_tree(document, budget);
        if (classified.limitation != WelcomePreviewLimitation::None) {
            result.status = classified.status;
            result.limitation = classified.limitation;
            return result;
        }

        // Native visible drawing envelope in document coordinates. "Absent" means
        // documentVisualBounds() returned nothing; a present non-finite or
        // zero-area envelope is invalid geometry, not a blank page.
        Geom::OptRect native = document.getRoot()->documentVisualBounds();
        Geom::Rect bounds;
        if (native) {
            if (!native->isFinite()) {
                result.status = WelcomePreviewStatus::InvalidGeometry;
                result.limitation = WelcomePreviewLimitation::NonFiniteGeometry;
                return result;
            }
            if (native->hasZeroArea()) {
                result.status = WelcomePreviewStatus::InvalidGeometry;
                result.limitation = WelcomePreviewLimitation::DegenerateGeometry;
                return result;
            }
            bounds = *native;
        } else {
            // Blank fallback: a valid selected page in document coordinates, else
            // the first valid page, else valid preferredBounds(). Never
            // pageBounds()/desktop coordinates, which can flip Y. An invalid
            // selected page does not stop the later fallbacks.
            Geom::OptRect fallback;
            auto valid_page_rect = [](Geom::Rect const &rect) {
                return rect.isFinite() && !rect.hasZeroArea();
            };
            auto &pages = document.getPageManager();
            if (auto *selected = pages.getSelected(); selected && valid_page_rect(selected->getDocumentRect())) {
                fallback = selected->getDocumentRect();
            }
            if (!fallback) {
                for (SPPage *page : pages.getPages()) {
                    if (page && valid_page_rect(page->getDocumentRect())) {
                        fallback = page->getDocumentRect();
                        break;
                    }
                }
            }
            if (!fallback) {
                Geom::OptRect const preferred = document.preferredBounds();
                if (preferred && valid_page_rect(*preferred)) {
                    fallback = *preferred;
                }
            }
            if (!fallback) {
                result.status = WelcomePreviewStatus::InvalidGeometry;
                result.limitation = WelcomePreviewLimitation::MissingGeometry;
                return result;
            }
            bounds = *fallback;
            result.framing = WelcomePreviewFraming::EmptyPage;
        }

        // Sane native-domain coordinates before conversion.
        PreviewRenderBudget::rect(bounds, true);
        double const width = bounds.width();
        double const height = bounds.height();
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0) {
            result.status = WelcomePreviewStatus::InvalidGeometry;
            result.limitation = WelcomePreviewLimitation::NonFiniteGeometry;
            return result;
        }
        double const scale = std::min(content_fraction * request.width / width,
                                      content_fraction * request.height / height);
        if (!std::isfinite(scale) || scale <= 0.0) {
            result.status = WelcomePreviewStatus::InvalidGeometry;
            result.limitation = WelcomePreviewLimitation::NonFiniteGeometry;
            return result;
        }
        Geom::Point const centre = bounds.midpoint();
        // Row-vector convention (2geom affine.h): p' = p*A and A*B applies A
        // then B. Scale first, then translate, so the bounds centre maps
        // exactly to W/2,H/2.
        Geom::Affine const frame =
            Geom::Scale(scale) *
            Geom::Translate(request.width / 2.0 - scale * centre[Geom::X],
                            request.height / 2.0 - scale * centre[Geom::Y]);
        PreviewRenderBudget::affine(frame);

        Drawing drawing;
        budget.bind_drawing(&drawing);
        drawing.setCacheBudget(0);
        unsigned const display_key = SPItem::display_key_new(1);
        SPRoot *root = document.getRoot();
        // Construct the guard before invoke_show: native show can build child
        // views and then throw, so partial views must be unwound too. It still
        // outlives the render below and is destroyed before Drawing.
        PreviewShown hide{root, display_key};
        Inkscape::DrawingItem *shown = root->invoke_show(drawing, display_key, SP_ITEM_SHOW_DISPLAY);
        if (!shown) {
            result.status = WelcomePreviewStatus::NativeFailure;
            result.limitation = WelcomePreviewLimitation::MissingGeometry;
            return result;
        }
        drawing.setRoot(shown);
        drawing.setExact();
        drawing.setDithering(false);
        shown->setTransform(frame);
        drawing.update();
        budget.checkpoint();

        // Charge the owned output surface before it is created.
        PreviewRenderBudget::surface(request.width, request.height);
        auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,
                                                   static_cast<int>(request.width),
                                                   static_cast<int>(request.height));
        int const expected_stride =
            cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, static_cast<int>(request.width));
        if (expected_stride <= 0 || surface->get_stride() != expected_stride ||
            surface->get_height() != static_cast<int>(request.height)) {
            result.status = WelcomePreviewStatus::NativeFailure;
            result.limitation = WelcomePreviewLimitation::AllocationLimit;
            return result;
        }
        {
            DrawingContext dc(surface->cobj(), Geom::Point(0, 0));
            paint_preview_background(dc, request.background, document);
            drawing.render(dc,
                           Geom::IntRect::from_xywh(0, 0, static_cast<int>(request.width),
                                                    static_cast<int>(request.height)),
                           DrawingItem::RENDER_BYPASS_CACHE);
        }
        surface->flush();
        if (cairo_surface_status(surface->cobj()) != CAIRO_STATUS_SUCCESS) {
            result.status = WelcomePreviewStatus::NativeFailure;
            result.limitation = WelcomePreviewLimitation::AllocationLimit;
            return result;
        }
        budget.checkpoint();

        result.surface = surface;
        result.width = request.width;
        result.height = request.height;
        result.stride = static_cast<std::size_t>(surface->get_stride());
        result.bytes = result.stride * request.height;
        result.document_bounds = bounds;
        result.status = WelcomePreviewStatus::Rendered;
        result.limitation = WelcomePreviewLimitation::None;
        result.budget = budget.stats();
        return result;
    } catch (PreviewRenderCancelled const &) {
        result.surface.reset();
        result.status = WelcomePreviewStatus::Cancelled;
        result.limitation = WelcomePreviewLimitation::None;
        return result;
    } catch (PreviewRenderLimit const &) {
        result.surface.reset();
        result.status = WelcomePreviewStatus::Limits;
        result.limitation = WelcomePreviewLimitation::AllocationLimit;
        return result;
    } catch (std::bad_alloc const &) {
        result.surface.reset();
        result.status = WelcomePreviewStatus::Limits;
        result.limitation = WelcomePreviewLimitation::AllocationLimit;
        return result;
    } catch (std::exception const &) {
        result.surface.reset();
        result.status = WelcomePreviewStatus::NativeFailure;
        result.limitation = WelcomePreviewLimitation::MissingGeometry;
        return result;
    }
}

namespace {
constexpr std::size_t helper_byte_limit = 4u * 1024u * 1024u;

bool inflate_svgz(std::string const &compressed, std::string &plain, bool &too_large)
{
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) return false;
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    int result = Z_OK;
    char buffer[16384];
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);
        result = inflate(&stream, Z_NO_FLUSH);
        auto produced = sizeof(buffer) - stream.avail_out;
        if (produced > helper_byte_limit - plain.size()) {
            too_large = true;
            inflateEnd(&stream);
            return false;
        }
        plain.append(buffer, produced);
    }
    inflateEnd(&stream);
    return result == Z_STREAM_END && stream.avail_in == 0;
}

bool parse_size(char const *s, unsigned &out)
{
    if (!s) return false;
    auto end = s + std::strlen(s);
    auto result = std::from_chars(s, end, out);
    return result.ec == std::errc{} && result.ptr == end && out >= 1 && out <= 512;
}

cairo_status_t png_stdout(void *stream, unsigned char const *data, unsigned length)
{
    return std::fwrite(data, 1, length, static_cast<FILE *>(stream)) == length ? CAIRO_STATUS_SUCCESS : CAIRO_STATUS_WRITE_ERROR;
}
} // namespace

int run_welcome_preview_helper(int argc, char const *const *argv)
{
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 66;
    int private_fd = _dup(1);
    if (private_fd < 0 || _dup2(2, 1) < 0) return 66;
    FILE *png = _fdopen(private_fd, "wb");
#else
    int private_fd = dup(1);
    if (private_fd < 0 || dup2(2, 1) < 0) return 66;
    FILE *png = fdopen(private_fd, "wb");
#endif
    if (!png) return 66;
#ifdef __APPLE__
    if (!Inkscape::IO::init_macos_bundle_resources()) return 66;
#endif
    // The parent supplies the extra test-only argument only after its explicit
    // file-I/O hook check. An inherited environment variable alone is inert.
    bool test_mode = argc == 6 && g_strcmp0(argv[5], "--vacards-preview-test-hooks") == 0 &&
                     g_strcmp0(g_getenv("VACARDS_FILE_IO_TEST_HOOKS"), "1") == 0;
    if (test_mode)
        IO::enable_file_io_test_hooks();
    if (IO::file_io_test_hooks_enabled()) {
        if (g_strcmp0(g_getenv("VACARDS_PREVIEW_TEST_ACTION"), "delay") == 0)
            g_usleep(250000);
        if (g_strcmp0(g_getenv("VACARDS_PREVIEW_TEST_ACTION"), "sleep") == 0 ||
            g_strcmp0(g_getenv("VACARDS_PREVIEW_TEST_ACTION"), "slow-reap") == 0)
            g_usleep(10 * G_USEC_PER_SEC);
        if (g_strcmp0(g_getenv("VACARDS_PREVIEW_TEST_ACTION"), "abort") == 0)
            std::abort();
        if (g_strcmp0(g_getenv("VACARDS_PREVIEW_TEST_ACTION"), "stdout") == 0)
            std::puts("library diagnostic");
        if (g_strcmp0(g_getenv("VACARDS_PREVIEW_TEST_ACTION"), "exit3") == 0)
            std::exit(3);
    }
    unsigned width = 0, height = 0;
    if ((argc != 5 && !test_mode) || !argv[2] || !parse_size(argv[3], width) || !parse_size(argv[4], height)) return 64;
    std::string path(argv[2]);
    if (!g_str_has_suffix(path.c_str(), ".svg") && !g_str_has_suffix(path.c_str(), ".svgz")) return 64;
    std::ifstream input(path, std::ios::binary);
    if (!input) return 64;
    std::string bytes;
    char chunk[16384];
    while (input) {
        input.read(chunk, sizeof(chunk));
        auto count = static_cast<std::size_t>(input.gcount());
        if (count > helper_byte_limit - bytes.size()) return 65;
        bytes.append(chunk, count);
    }
    if (!input.eof()) return 64;
    if (g_str_has_suffix(path.c_str(), ".svgz")) {
        std::string plain;
        bool too_large = false;
        if (!inflate_svgz(bytes, plain, too_large)) return too_large ? 65 : 64;
        bytes = std::move(plain);
    }
    auto parsed = IO::parse_preview_xml(std::span<unsigned char const>(
        reinterpret_cast<unsigned char const *>(bytes.data()), bytes.size()));
    if (!parsed.parsed_ok()) return parsed.reason == IO::PreviewXmlReason::TooManyBytes ? 65 : 64;
    if (!IO::admit_preview_resources(*parsed.parsed).candidate()) return 64;
    try {
        Inkscape::GC::init();
        Inkscape::Util::Statics statics;
        static auto gtk = Gtk::Application::create("org.inkscape.vacards.welcomepreviewhelper",
                                                   Gio::Application::Flags::NON_UNIQUE);
        if (!Application::exists()) Application::create(false, Application::RuntimePolicy::PreviewHelper);
        auto doc = SPDocument::createNewDocFromMem(std::span<char const>(bytes.data(), bytes.size()));
        if (!doc) return 66;
        WelcomeDrawingPreviewRequest request;
        request.width = width;
        request.height = height;
        auto rendered = renderWelcomeDrawingPreview(*doc, request);
        if (rendered.status != WelcomePreviewStatus::Rendered || !rendered.surface) return 66;
        return cairo_surface_write_to_png_stream(rendered.surface->cobj(), png_stdout, png) ==
                       CAIRO_STATUS_SUCCESS && std::fflush(png) == 0 ? 0 : 66;
    } catch (...) {
        return 66;
    }
}

namespace {
char *running_executable_path()
{
#ifdef __APPLE__
    uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::vector<char> path(length);
    if (_NSGetExecutablePath(path.data(), &length) != 0) return nullptr;
    auto resolved = realpath(path.data(), nullptr);
#elif defined(_WIN32)
    std::vector<wchar_t> path(32768);
    auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    auto resolved = length && length < path.size() ? g_utf16_to_utf8(
        reinterpret_cast<gunichar2 const *>(path.data()), length, nullptr, nullptr, nullptr) : nullptr;
#else
    auto resolved = g_file_read_link("/proc/self/exe", nullptr);
#endif
    auto basename = resolved ? g_path_get_basename(resolved) : nullptr;
    bool test_binary = basename && (g_str_has_prefix(basename, "test_welcome-drawing-preview") ||
                                    g_str_has_prefix(basename, "test_preferences-presenter"));
    g_free(basename);
    if (resolved && IO::file_io_test_hooks_enabled() && test_binary) {
        auto directory = g_path_get_dirname(resolved);
#ifdef _WIN32
        auto helper = g_build_filename(directory, "inkscape.exe", nullptr);
#else
        auto helper = g_build_filename(directory, "inkscape", nullptr);
#endif
        g_free(directory); g_free(resolved);
        return helper;
    }
    return resolved;
}
struct LaunchRequest {
    std::string path;
    unsigned width, height, deadline;
    WelcomePreviewCallback callback;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

void launch_thread(GTask *task, gpointer, gpointer data, GCancellable *)
{
    auto &request = *static_cast<LaunchRequest *>(data);
    auto result = std::make_unique<WelcomePreviewLaunchResult>();
    auto executable = running_executable_path();
    if (!executable) {
        g_task_return_pointer(task, result.release(), [](gpointer p) { delete static_cast<WelcomePreviewLaunchResult *>(p); });
        return;
    }
    auto launcher = g_subprocess_launcher_new(static_cast<GSubprocessFlags>(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    char const *env[] = {nullptr};
    g_subprocess_launcher_set_environ(launcher, const_cast<char **>(env));
    for (auto key : {"PATH", "DYLD_LIBRARY_PATH", "LD_LIBRARY_PATH", "GDK_PIXBUF_MODULE_FILE",
                     "XDG_DATA_DIRS", "XDG_DATA_HOME", "GIO_MODULE_DIR", "FONTCONFIG_FILE", "FONTCONFIG_PATH",
                     "HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA",
                     "SystemRoot", "TEMP", "TMP"}) {
        if (auto value = g_getenv(key)) g_subprocess_launcher_setenv(launcher, key, value, true);
    }
    auto profile = g_dir_make_tmp("vacards-preview-profile-XXXXXX", nullptr);
    if (!profile) {
        g_object_unref(launcher); g_free(executable);
        g_task_return_pointer(task, result.release(), [](gpointer p) { delete static_cast<WelcomePreviewLaunchResult *>(p); });
        return;
    }
    g_subprocess_launcher_setenv(launcher, "INKSCAPE_PROFILE_DIR", profile, true);
    if (IO::file_io_test_hooks_enabled()) {
        g_subprocess_launcher_setenv(launcher, "VACARDS_FILE_IO_TEST_HOOKS", "1", true);
        if (auto action = g_getenv("VACARDS_PREVIEW_TEST_ACTION"))
            g_subprocess_launcher_setenv(launcher, "VACARDS_PREVIEW_TEST_ACTION", action, true);
    }
    auto w = std::to_string(request.width), h = std::to_string(request.height);
    char const *args[] = {executable, "--vacards-welcome-preview-helper", request.path.c_str(),
                          w.c_str(), h.c_str(), IO::file_io_test_hooks_enabled() ?
                          "--vacards-preview-test-hooks" : nullptr, nullptr};
    GError *error = nullptr;
    auto process = g_subprocess_launcher_spawnv(launcher, args, &error);
    g_object_unref(launcher);
    g_free(executable);
    if (!process) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(std::filesystem::u8path(profile), cleanup_error); g_free(profile);
        g_clear_error(&error);
        g_task_return_pointer(task, result.release(), [](gpointer p) { delete static_cast<WelcomePreviewLaunchResult *>(p); });
        return;
    }
    auto cancellable = g_cancellable_new();
    std::atomic<bool> finished = false;
    std::atomic<bool> returned = false;
    std::atomic<bool> expired = false;
    std::atomic<bool> cancelled = false;
    std::thread watchdog([&, task_ref = G_TASK(g_object_ref(task))] {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.deadline ? request.deadline : 5000);
        while (!finished) {
            if (request.cancelled && *request.cancelled) { cancelled = true; break; }
            if (std::chrono::steady_clock::now() >= deadline) { expired = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (expired || cancelled) {
            g_subprocess_force_exit(process); g_cancellable_cancel(cancellable);
            if (!returned.exchange(true)) {
                // Return now even if Windows keeps the pipe read blocked; the
                // worker retains the task until read and reap finish.
                auto early = new WelcomePreviewLaunchResult;
                early->error = expired ? WelcomePreviewLaunchError::Timeout : WelcomePreviewLaunchError::Failed;
                g_task_return_pointer(task_ref, early, [](gpointer p) { delete static_cast<WelcomePreviewLaunchResult *>(p); });
            }
        }
        g_object_unref(task_ref);
    });
    std::string png;
    char buffer[16384];
    auto stream = g_subprocess_get_stdout_pipe(process);
    bool overflow = false;
    while (true) {
        auto count = g_input_stream_read(stream, buffer, sizeof(buffer), cancellable, &error);
        if (count <= 0) break;
        if (static_cast<std::size_t>(count) > helper_byte_limit - png.size()) {
            overflow = true;
            g_subprocess_force_exit(process);
            break;
        }
        png.append(buffer, count);
    }
    g_clear_error(&error);
    if (!expired && !cancelled && !overflow) {
        g_subprocess_wait(process, cancellable, &error);
        g_clear_error(&error);
    }
    finished = true;
    watchdog.join();
    g_object_unref(cancellable);
    if (expired || cancelled || overflow) {
        g_subprocess_force_exit(process);
        auto reaper = G_SUBPROCESS(g_object_ref(process));
        std::string profile_path(profile);
        bool slow_reap = IO::file_io_test_hooks_enabled() &&
            g_strcmp0(g_getenv("VACARDS_PREVIEW_TEST_ACTION"), "slow-reap") == 0;
        std::thread([reaper, profile_path, slow_reap] {
            if (slow_reap) g_usleep(500000);
            g_subprocess_wait(reaper, nullptr, nullptr);
            g_object_unref(reaper);
            std::error_code cleanup_error;
            std::filesystem::remove_all(std::filesystem::u8path(profile_path), cleanup_error);
        }).detach();
    }
    if (expired) result->error = WelcomePreviewLaunchError::Timeout;
    else if (cancelled) result->error = WelcomePreviewLaunchError::Failed;
    else if (overflow) result->error = WelcomePreviewLaunchError::TooLarge;
    else if (g_subprocess_get_if_signaled(process)) result->error = WelcomePreviewLaunchError::Crashed;
    else {
        switch (g_subprocess_get_exit_status(process)) {
        case 64: result->error = WelcomePreviewLaunchError::Rejected; break;
        case 65: result->error = WelcomePreviewLaunchError::TooLarge; break;
        case 66: result->error = WelcomePreviewLaunchError::Failed; break;
        case 0: {
            auto loader = gdk_pixbuf_loader_new_with_type("png", &error);
            if (loader && gdk_pixbuf_loader_write(loader,
                    reinterpret_cast<guchar const *>(png.data()), png.size(), &error) &&
                gdk_pixbuf_loader_close(loader, &error)) {
                auto pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
                if (pixbuf && gdk_pixbuf_get_width(pixbuf) == static_cast<int>(request.width) &&
                    gdk_pixbuf_get_height(pixbuf) == static_cast<int>(request.height)) {
                    result->pixbuf = GDK_PIXBUF(g_object_ref(pixbuf));
                    result->error = WelcomePreviewLaunchError::Ok;
                }
            }
            if (loader) g_object_unref(loader);
            g_clear_error(&error);
            break;
        }
        default: result->error = WelcomePreviewLaunchError::Crashed; break;
        }
    }
    g_object_unref(process);
    if (!expired && !cancelled && !overflow) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(std::filesystem::u8path(profile), cleanup_error);
    }
    g_free(profile);
    if (!returned.exchange(true))
        g_task_return_pointer(task, result.release(), [](gpointer p) { delete static_cast<WelcomePreviewLaunchResult *>(p); });
}
} // namespace

void launch_welcome_preview(std::string path, unsigned width, unsigned height,
                            unsigned deadline_ms, WelcomePreviewCallback callback,
                            std::shared_ptr<std::atomic<bool>> cancelled)
{
    auto request = new LaunchRequest{std::move(path), width, height, deadline_ms, std::move(callback), std::move(cancelled)};
    auto task = g_task_new(nullptr, nullptr, [](GObject *, GAsyncResult *async, gpointer data) {
        auto request = static_cast<LaunchRequest *>(data);
        auto result = static_cast<WelcomePreviewLaunchResult *>(
            g_task_propagate_pointer(G_TASK(async), nullptr));
        request->callback(*result);
        delete result;
    }, request);
    g_task_set_task_data(task, request, [](gpointer p) { delete static_cast<LaunchRequest *>(p); });
    g_task_run_in_thread(task, launch_thread);
    g_object_unref(task);
}

namespace {
constexpr std::size_t memory_cap = 16u * 1024u * 1024u;
constexpr std::size_t disk_cap = 64u * 1024u * 1024u;
std::string demand_id(std::string const &path, unsigned w, unsigned h)
{
    return std::to_string(path.size()) + ":" + path + ":" + std::to_string(w) + "x" + std::to_string(h);
}
std::string cache_hash(std::string const &id, GFileInfo *info)
{
    auto identity = std::string("welcome-v1:") + id + ":" +
        std::to_string(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) + ":" +
        std::to_string(g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC)) + ":" +
        std::to_string(g_file_info_get_size(info));
    for (auto key : {"FONTCONFIG_FILE", "FONTCONFIG_PATH", "XDG_DATA_DIRS", "XDG_DATA_HOME"}) {
        auto value = g_getenv(key);
        identity += ":" + std::string(key) + "=" + (value ? value : "");
    }
    auto hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity.c_str(), identity.size());
    std::string result(hash);
    g_free(hash);
    return result;
}
std::string default_thumbnail_directory()
{
    return std::string(g_get_user_cache_dir()) + G_DIR_SEPARATOR_S "vacards" G_DIR_SEPARATOR_S "welcome-thumbnails";
}
std::size_t decoded_bytes(GdkPixbuf *pixbuf)
{
    return static_cast<std::size_t>(gdk_pixbuf_get_rowstride(pixbuf)) * gdk_pixbuf_get_height(pixbuf);
}
std::mutex disk_mutex;
struct DiskJob { std::string directory, hash; GdkPixbuf *pixbuf = nullptr; unsigned w, h; bool write = false;
    std::shared_ptr<std::atomic<bool>> alive;
    ~DiskJob() { if (pixbuf) g_object_unref(pixbuf); }
};
void trim_disk(std::string const &directory)
{
    struct Entry { std::string path; gint64 access; std::size_t bytes; };
    std::vector<Entry> entries;
    std::size_t total = 0;
    if (auto dir = g_dir_open(directory.c_str(), 0, nullptr)) {
        while (auto name = g_dir_read_name(dir)) {
            auto path = directory + G_DIR_SEPARATOR_S + name;
            if (g_str_has_suffix(name, ".tmp")) {
                // Left by a write interrupted at exit; live writes are seconds old.
                GStatBuf st{};
                if (!g_stat(path.c_str(), &st) && S_ISREG(st.st_mode) && st.st_mtime < time(nullptr) - 3600)
                    g_unlink(path.c_str());
                continue;
            }
            if (!g_str_has_suffix(name, ".png")) continue;
            GStatBuf st{};
            if (g_stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) continue;
            auto bytes = static_cast<std::size_t>(st.st_size);
            total += bytes;
            entries.push_back({std::move(path), st.st_atime, bytes});
        }
        g_dir_close(dir);
    }
    std::sort(entries.begin(), entries.end(), [](auto const &a, auto const &b) { return a.access < b.access; });
    for (auto const &entry : entries) {
        if (total <= disk_cap) break;
        if (g_unlink(entry.path.c_str()) == 0) total -= entry.bytes;
    }
}
void disk_job(GTask *task, gpointer, gpointer data, GCancellable *)
{
    auto &job = *static_cast<DiskJob *>(data);
    auto path = job.directory + G_DIR_SEPARATOR_S + job.hash + ".png";
    if (job.write && IO::file_io_test_hooks_enabled() &&
        g_strcmp0(g_getenv("VACARDS_PREVIEW_TEST_DISK_WRITE_DELAY"), "1") == 0)
        g_usleep(500000);
    std::lock_guard lock(disk_mutex);
    if (job.alive && !*job.alive) { g_task_return_pointer(task, nullptr, nullptr); return; }
    if (!job.write) {
        trim_disk(job.directory);
        GStatBuf st{};
        unsigned char header[24]{};
        std::ifstream input(path, std::ios::binary);
        input.read(reinterpret_cast<char *>(header), sizeof(header));
        auto dimension = [&](int offset) {
            return (static_cast<unsigned>(header[offset]) << 24) |
                   (static_cast<unsigned>(header[offset + 1]) << 16) |
                   (static_cast<unsigned>(header[offset + 2]) << 8) | header[offset + 3];
        };
        if (g_stat(path.c_str(), &st) || st.st_size > helper_byte_limit ||
            input.gcount() != sizeof(header) ||
            std::memcmp(header, "\x89PNG\r\n\x1a\n", 8) ||
            dimension(16) != job.w || dimension(20) != job.h) {
            g_unlink(path.c_str());
            g_task_return_pointer(task, nullptr, nullptr);
            return;
        }
        GError *error = nullptr;
        auto pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &error);
        g_clear_error(&error);
        if (pixbuf && (gdk_pixbuf_get_width(pixbuf) != static_cast<int>(job.w) ||
                       gdk_pixbuf_get_height(pixbuf) != static_cast<int>(job.h))) {
            g_object_unref(pixbuf); pixbuf = nullptr;
        }
        if (!pixbuf) g_unlink(path.c_str());
        else { struct utimbuf now{time(nullptr), time(nullptr)}; utime(path.c_str(), &now); }
        g_task_return_pointer(task, pixbuf, [](gpointer p) { if (p) g_object_unref(p); });
        return;
    }
    g_mkdir_with_parents(job.directory.c_str(), 0700);
    gchar *png = nullptr; gsize length = 0; GError *error = nullptr;
    if (gdk_pixbuf_save_to_buffer(job.pixbuf, &png, &length, "png", &error, nullptr) && length <= helper_byte_limit) {
        auto uuid = g_uuid_string_random();
        auto temporary = path + "." + uuid + ".tmp";
        g_free(uuid);
        if (g_file_set_contents(temporary.c_str(), png, length, &error)) {
            if (g_rename(temporary.c_str(), path.c_str())) g_unlink(temporary.c_str());
            else trim_disk(job.directory);
        }
    }
    g_free(png); g_clear_error(&error);
    g_task_return_pointer(task, nullptr, nullptr);
}
void run_disk(std::string directory, std::string hash, GdkPixbuf *pixbuf, unsigned w, unsigned h,
              std::function<void(GdkPixbuf *)> done, std::shared_ptr<std::atomic<bool>> alive = {})
{
    auto job = new DiskJob{std::move(directory), std::move(hash), pixbuf ? GDK_PIXBUF(g_object_ref(pixbuf)) : nullptr,
                           w, h, pixbuf != nullptr, std::move(alive)};
    auto callback = new std::function<void(GdkPixbuf *)>(std::move(done));
    auto task = g_task_new(nullptr, nullptr, [](GObject *, GAsyncResult *async, gpointer data) {
        std::unique_ptr<std::function<void(GdkPixbuf *)>> done(static_cast<std::function<void(GdkPixbuf *)> *>(data));
        auto result = static_cast<GdkPixbuf *>(g_task_propagate_pointer(G_TASK(async), nullptr));
        (*done)(result);
        if (result) g_object_unref(result);
    }, callback);
    g_task_set_task_data(task, job, [](gpointer p) { delete static_cast<DiskJob *>(p); });
    g_task_run_in_thread(task, disk_job);
    g_object_unref(task);
}
} // namespace

struct WelcomeThumbnailService::State : std::enable_shared_from_this<State> {
    inline static bool render_gate = false;
    inline static unsigned gate_epoch = 0;
    inline static std::vector<std::weak_ptr<State>> instances;
    struct Demand { std::string path, id, hash; unsigned w, h; unsigned generation;
        std::vector<WelcomePreviewCallback> callbacks; };
    struct Memory { GdkPixbuf *pixbuf; std::size_t bytes; guint64 used; };
    std::string directory;
    std::unordered_map<std::string, Demand> demands;
    std::deque<std::string> queue;
    std::unordered_map<std::string, Memory> memory;
    std::size_t memory_bytes = 0, renders = 0;
    guint64 tick = 0;
    unsigned serial = 0;
    bool busy = false;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> active_cancel;
    std::string active_id;
    unsigned active_generation = 0, active_epoch = 0;
    ~State() { for (auto &[key, item] : memory) g_object_unref(item.pixbuf); }
    void remember(std::string const &hash, GdkPixbuf *pixbuf) {
        auto bytes = decoded_bytes(pixbuf);
        if (bytes > memory_cap) return;
        if (auto old = memory.find(hash); old != memory.end()) {
            memory_bytes -= old->second.bytes; g_object_unref(old->second.pixbuf); memory.erase(old);
        }
        while (memory_bytes + bytes > memory_cap && !memory.empty()) {
            auto old = std::min_element(memory.begin(), memory.end(), [](auto const &a, auto const &b) {
                return a.second.used < b.second.used;
            });
            memory_bytes -= old->second.bytes; g_object_unref(old->second.pixbuf); memory.erase(old);
        }
        memory.emplace(hash, Memory{GDK_PIXBUF(g_object_ref(pixbuf)), bytes, ++tick}); memory_bytes += bytes;
    }
    void finish(std::string const &id, unsigned generation, WelcomePreviewLaunchResult result) {
        auto it = demands.find(id);
        if (it == demands.end() || it->second.generation != generation) {
            if (result.pixbuf) g_object_unref(result.pixbuf); return;
        }
        auto callbacks = std::move(it->second.callbacks);
        demands.erase(it);
        for (auto &callback : callbacks) {
            auto copy = result;
            if (copy.pixbuf) g_object_ref(copy.pixbuf);
            callback(copy);
        }
        if (result.pixbuf) g_object_unref(result.pixbuf);
    }
    void metadata(std::string id, unsigned generation, std::function<void(GFileInfo *)> done) {
        auto it = demands.find(id);
        if (it == demands.end() || it->second.generation != generation) return;
        auto file = g_file_new_for_path(it->second.path.c_str());
        auto context = new std::pair<std::weak_ptr<State>, std::function<void(GFileInfo *)>>{weak_from_this(), std::move(done)};
        g_file_query_info_async(file, G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                                G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, G_FILE_QUERY_INFO_NONE,
                                G_PRIORITY_DEFAULT, nullptr, [](GObject *source, GAsyncResult *async, gpointer data) {
            std::unique_ptr<std::pair<std::weak_ptr<State>, std::function<void(GFileInfo *)>>> context(
                static_cast<std::pair<std::weak_ptr<State>, std::function<void(GFileInfo *)>> *>(data));
            GError *error = nullptr;
            auto info = g_file_query_info_finish(G_FILE(source), async, &error);
            g_clear_error(&error);
            context->second(info);
            if (info) g_object_unref(info);
        }, context);
        g_object_unref(file);
    }
    void pump() {
        if (busy || render_gate) return;
        while (!queue.empty()) {
            auto id = std::move(queue.front()); queue.pop_front();
            auto it = demands.find(id);
            if (it == demands.end()) continue;
            busy = render_gate = true;
            auto generation = it->second.generation;
            auto weak = weak_from_this();
            active_id = id;
            active_generation = generation;
            active_epoch = ++gate_epoch;
            auto epoch = active_epoch;
            Glib::signal_timeout().connect_once([weak, epoch, id, generation] {
                if (State::gate_epoch != epoch) return;
                if (auto self = weak.lock()) {
                    self->finish(id, generation, {WelcomePreviewLaunchError::Timeout});
                    self->release(id, generation);
                } else State::release_gate();
            }, 6000);
            metadata(id, generation, [weak, id, generation](GFileInfo *info) {
                if (auto self = weak.lock()) self->first_info(id, generation, info);
            });
            return;
        }
    }
    static void release_gate() {
        render_gate = false;
        ++gate_epoch;
        for (auto it = instances.begin(); it != instances.end();) {
            if (auto state = it->lock()) { state->pump(); ++it; }
            else it = instances.erase(it);
            if (render_gate) break;
        }
    }
    void release(std::string const &id, unsigned generation) {
        if (!busy || active_id != id || active_generation != generation) return;
        busy = false; active_id.clear();
        // A gate timeout releases while the helper may still run: cancel it
        // before dropping our reference so two helpers never overlap.
        if (active_cancel) *active_cancel = true;
        active_cancel.reset(); release_gate();
    }
    void first_info(std::string const &id, unsigned generation, GFileInfo *info) {
        auto it = demands.find(id);
        if (it == demands.end() || it->second.generation != generation) return;
        if (!info) { finish(id, generation, {}); release(id, generation); return; }
        auto hash = cache_hash(id, info);
        it->second.hash = hash;
        if (auto hit = memory.find(hash); hit != memory.end()) {
            hit->second.used = ++tick;
            recheck(id, generation, hash, GDK_PIXBUF(g_object_ref(hit->second.pixbuf)), false);
            return;
        }
        auto weak = weak_from_this();
        run_disk(directory, hash, nullptr, it->second.w, it->second.h,
                 [weak, id, generation, hash](GdkPixbuf *pixbuf) {
            if (auto self = weak.lock()) self->disk_result(id, generation, hash, pixbuf);
        }, alive);
    }
    void disk_result(std::string const &id, unsigned generation, std::string const &hash, GdkPixbuf *pixbuf) {
        auto it = demands.find(id);
        if (it == demands.end() || it->second.generation != generation) return;
        if (pixbuf) { recheck(id, generation, hash, GDK_PIXBUF(g_object_ref(pixbuf)), false); return; }
        auto path = it->second.path; auto w = it->second.w; auto h = it->second.h;
        ++renders;
        auto weak = weak_from_this();
        active_cancel = std::make_shared<std::atomic<bool>>(false);
        launch_welcome_preview(path, w, h, 5000, [weak, id, generation, hash](WelcomePreviewLaunchResult result) {
            if (auto self = weak.lock()) {
                if (result.error == WelcomePreviewLaunchError::Ok && result.pixbuf)
                    self->recheck(id, generation, hash, result.pixbuf, true);
                else { self->finish(id, generation, result); self->release(id, generation); }
            } else if (result.pixbuf) g_object_unref(result.pixbuf);
        }, active_cancel);
    }
    void recheck(std::string id, unsigned generation, std::string hash, GdkPixbuf *pixbuf, bool persist) {
        auto it = demands.find(id);
        if (it == demands.end() || it->second.generation != generation) {
            g_object_unref(pixbuf); return;
        }
        auto weak = weak_from_this();
        metadata(id, generation, [weak, id, generation, hash, pixbuf, persist](GFileInfo *info) {
            if (auto self = weak.lock()) {
                auto it = self->demands.find(id);
                if (it != self->demands.end() && it->second.generation == generation && info &&
                    cache_hash(id, info) == hash) {
                    self->remember(hash, pixbuf);
                    if (persist) run_disk(self->directory, hash, pixbuf, it->second.w, it->second.h, [](GdkPixbuf *) {}, self->alive);
                    self->finish(id, generation, {WelcomePreviewLaunchError::Ok, GDK_PIXBUF(g_object_ref(pixbuf))});
                } else self->finish(id, generation, {});
                self->release(id, generation);
            }
            g_object_unref(pixbuf);
        });
    }
};

WelcomeThumbnailService::WelcomeThumbnailService(std::string cache_directory)
    : _state(std::make_shared<State>())
{
    _state->directory = cache_directory.empty() ? default_thumbnail_directory() : std::move(cache_directory);
    State::instances.push_back(_state);
}
WelcomeThumbnailService::~WelcomeThumbnailService() {
    { std::lock_guard lock(disk_mutex); *_state->alive = false; }
    if (_state->active_cancel) *_state->active_cancel = true;
    _state->demands.clear(); _state->queue.clear();
    if (_state->busy) _state->release(_state->active_id, _state->active_generation);
    _state.reset();
}
void WelcomeThumbnailService::request(std::string path, unsigned width, unsigned height, WelcomePreviewCallback callback)
{
    auto id = demand_id(path, width, height);
    if (!g_path_is_absolute(path.c_str()) || width < 1 || width > 512 || height < 1 || height > 512) {
        callback({}); return;
    }
    if (auto it = _state->demands.find(id); it != _state->demands.end()) {
        it->second.callbacks.push_back(std::move(callback)); return;
    }
    if (_state->demands.size() >= 8) { callback({}); return; }
    auto generation = ++_state->serial;
    _state->demands.emplace(id, State::Demand{std::move(path), id, {}, width, height, generation, {std::move(callback)}});
    _state->queue.push_back(id);
    _state->pump();
}
void WelcomeThumbnailService::cancel(std::string const &path, unsigned width, unsigned height)
{
    auto id = demand_id(path, width, height);
    _state->demands.erase(id);
    if (_state->active_cancel && _state->active_id == id) *_state->active_cancel = true;
    _state->queue.erase(std::remove(_state->queue.begin(), _state->queue.end(), id), _state->queue.end());
}
WelcomeThumbnailService::Stats WelcomeThumbnailService::stats() const
{ return {_state->memory_bytes, _state->queue.size(), _state->busy ? 1u : 0u, _state->renders}; }

namespace {
bool ascii_ends_with_ci(std::string const &text, std::string_view suffix)
{
    return text.size() >= suffix.size() &&
           g_ascii_strcasecmp(text.c_str() + text.size() - suffix.size(), std::string(suffix).c_str()) == 0;
}

// The preview background is opaque white, so premultiplied ARGB32 equals straight RGB.
GdkPixbuf *pixbuf_from_opaque_surface(Cairo::RefPtr<Cairo::ImageSurface> const &surface)
{
    int const width = surface->get_width(), height = surface->get_height();
    auto pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
    if (!pixbuf) return nullptr;
    auto const *source = surface->get_data();
    int const source_stride = surface->get_stride();
    auto *target = gdk_pixbuf_get_pixels(pixbuf);
    int const target_stride = gdk_pixbuf_get_rowstride(pixbuf);
    for (int y = 0; y < height; ++y) {
        auto const *in = reinterpret_cast<guint32 const *>(source + y * source_stride);
        auto *out = target + y * target_stride;
        for (int x = 0; x < width; ++x) {
            out[3 * x] = (in[x] >> 16) & 0xff;
            out[3 * x + 1] = (in[x] >> 8) & 0xff;
            out[3 * x + 2] = in[x] & 0xff;
        }
    }
    return pixbuf;
}

struct DocumentThumbnailJob : std::enable_shared_from_this<DocumentThumbnailJob> {
    struct Version {
        guint64 seconds; guint32 microseconds; goffset size;
        bool operator==(Version const &) const = default;
    };
    // Documents whose render ran out of time or budget are not retried while
    // open; the Finder icon keeps its own list, so it never stops thumbnails.
    inline static std::vector<std::weak_ptr<void>> too_slow, too_slow_icon;

    SPDocument *document;
    std::weak_ptr<void> lifetime;
    std::string path, directory;
    unsigned delay_ms = 0;
    bool finder_icon = false;
    std::function<void(bool)> done;
    std::optional<Version> loaded;
    std::optional<std::uint64_t> revision;
    bool icon_busy = false;          // the Finder icon is still being written
    std::optional<bool> finished;    // result held until then

    void finish(bool stored)
    {
        if (icon_busy) {
            finished = stored;
            return;
        }
        if (auto callback = std::move(done)) callback(stored);
    }
    // The open document still equals the file at path.
    bool matches_file() const
    {
        if (lifetime.expired()) return false;
        auto const *filename = document->getDocumentFilename();
        return filename && path == filename && !document->isModifiedSinceSave();
    }
    bool marked_too_slow(std::vector<std::weak_ptr<void>> const &list = too_slow) const
    {
        return std::any_of(list.begin(), list.end(), [this](auto const &token) {
            return !token.owner_before(lifetime) && !lifetime.owner_before(token);
        });
    }
    void mark_too_slow(std::vector<std::weak_ptr<void>> &list = too_slow)
    {
        std::erase_if(list, [](auto const &token) { return token.expired(); });
        list.push_back(lifetime);
    }
    static std::optional<Version> version(GFileInfo *info)
    {
        if (!info) return {};
        return Version{g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED),
                       g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC),
                       g_file_info_get_size(info)};
    }
    void query(void (DocumentThumbnailJob::*next)(GFileInfo *))
    {
        using Context = std::pair<std::shared_ptr<DocumentThumbnailJob>, void (DocumentThumbnailJob::*)(GFileInfo *)>;
        auto file = g_file_new_for_path(path.c_str());
        g_file_query_info_async(file, G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                                G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC "," G_FILE_ATTRIBUTE_UNIX_DEVICE ","
                                G_FILE_ATTRIBUTE_UNIX_INODE, G_FILE_QUERY_INFO_NONE,
                                G_PRIORITY_DEFAULT, nullptr, [](GObject *source, GAsyncResult *async, gpointer data) {
            std::unique_ptr<Context> context(static_cast<Context *>(data));
            GError *error = nullptr;
            auto info = g_file_query_info_finish(G_FILE(source), async, &error);
            g_clear_error(&error);
            ((*context->first).*(context->second))(info);
            if (info) g_object_unref(info);
        }, new Context(shared_from_this(), next));
        g_object_unref(file);
    }
    // Read the file version right after the load or save, then wait.
    void begin() { query(&DocumentThumbnailJob::after_load); }
    void after_load(GFileInfo *info)
    {
        loaded = version(info);
        if (!loaded || !matches_file()) return finish(false);
        Glib::signal_timeout().connect_once([self = shared_from_this()] { self->before_render(); }, delay_ms);
    }
    void before_render()
    {
        if (!matches_file() || marked_too_slow()) return finish(false);
        revision = document->getReprDoc()->contentRevision();
        if (!revision) return finish(false);
        query(&DocumentThumbnailJob::render);
    }
#ifdef __APPLE__
    // VIEW-1: the saved file's Finder icon, a square rendering framed like the
    // thumbnails (256 px: about the thumbnail's cost on the main thread). It is
    // written on a background thread (network volumes may be slow) and only if
    // the file is still the one just read; the file keeps its modification
    // time, so the version read above stays the cache key.
    void set_icon(GFileInfo *info)
    {
        if (marked_too_slow(too_slow_icon) || !info || !g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_INODE))
            return;
        WelcomeDrawingPreviewRequest request;
        request.width = request.height = 256;
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        request.cancelled = [deadline] { return std::chrono::steady_clock::now() > deadline; };
        auto rendered = renderWelcomeDrawingPreview(*document, request);
        if (rendered.status == WelcomePreviewStatus::Cancelled || rendered.status == WelcomePreviewStatus::Limits) {
            mark_too_slow(too_slow_icon);
            return;
        }
        if (rendered.status != WelcomePreviewStatus::Rendered || !rendered.surface ||
            rendered.framing != WelcomePreviewFraming::Drawing)
            return;
        std::vector<unsigned char> png;
        auto const status = cairo_surface_write_to_png_stream(
            rendered.surface->cobj(),
            [](void *closure, unsigned char const *data, unsigned length) {
                auto &out = *static_cast<std::vector<unsigned char> *>(closure);
                out.insert(out.end(), data, data + length);
                return CAIRO_STATUS_SUCCESS;
            },
            &png);
        if (status != CAIRO_STATUS_SUCCESS) return;
        IO::FinderIconTarget target;
        target.device = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE);
        target.inode = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE);
        target.size = g_file_info_get_size(info);
        target.modified_seconds = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        target.modified_microseconds = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
        icon_busy = true;
        IO::set_finder_icon_async(path, std::move(png), target, [self = shared_from_this()](bool) {
            self->icon_busy = false;
            if (self->finished) self->finish(*self->finished);
        });
    }
#endif

    // Store only if neither the file nor the document changed since the load or save.
    void render(GFileInfo *info)
    {
        if (version(info) != loaded || !matches_file() || document->getReprDoc()->contentRevision() != revision)
            return finish(false);
#ifdef __APPLE__
        if (finder_icon) {
            set_icon(info);
        }
#endif
        struct Size { unsigned width, height; std::string hash, file; };
        std::vector<Size> sizes;
        for (unsigned scale : {1u, 2u}) {
            unsigned const width = welcome_card_width * scale, height = welcome_card_height * scale;
            auto hash = cache_hash(demand_id(path, width, height), info);
            auto file = directory + G_DIR_SEPARATOR_S + hash + ".png";
            sizes.push_back({width, height, std::move(hash), std::move(file)});
        }
        auto cached = [sizes] {
            return std::all_of(sizes.begin(), sizes.end(), [](auto const &size) {
                return g_file_test(size.file.c_str(), G_FILE_TEST_IS_REGULAR); });
        };
        if (cached()) return finish(true);

        WelcomeDrawingPreviewRequest request;
        request.width = sizes.back().width;
        request.height = sizes.back().height;
        // Bound the pause on the main thread. The deadline is sampled between
        // native steps, so one heavy item can still overrun it.
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        request.cancelled = [deadline] { return std::chrono::steady_clock::now() > deadline; };
        auto rendered = renderWelcomeDrawingPreview(*document, request);
        if (rendered.status == WelcomePreviewStatus::Cancelled || rendered.status == WelcomePreviewStatus::Limits)
            mark_too_slow();
        // A blank document frames the editor's current page, which the file may
        // not select; leave blank files to the helper.
        if (rendered.status != WelcomePreviewStatus::Rendered || !rendered.surface ||
            rendered.framing != WelcomePreviewFraming::Drawing)
            return finish(false);
        auto large = pixbuf_from_opaque_surface(rendered.surface);
        if (!large) return finish(false);
        auto small = gdk_pixbuf_scale_simple(large, sizes.front().width, sizes.front().height, GDK_INTERP_HYPER);
        if (!small) { g_object_unref(large); return finish(false); }
        auto pending = std::make_shared<unsigned>(2);
        for (auto const &[size, pixbuf] : {std::pair{sizes.front(), small}, std::pair{sizes.back(), large}}) {
            run_disk(directory, size.hash, pixbuf, size.width, size.height,
                     [self = shared_from_this(), pending, cached](GdkPixbuf *) {
                if (!--*pending) self->finish(cached());
            });
        }
        g_object_unref(small);
        g_object_unref(large);
    }
};
} // namespace

void store_document_thumbnail(SPDocument &document, std::string path, unsigned delay_ms,
                              std::string cache_directory, std::function<void(bool)> done, bool finder_icon)
{
    if (cache_directory.empty()) {
        // CTest sets this for every unit test ("off"); the shipped app never does.
        if (auto const *test_directory = g_getenv("VACARDS_WELCOME_THUMBNAIL_TEST_CACHE")) {
            if (g_strcmp0(test_directory, "off") != 0) cache_directory = test_directory;
        } else if (!IO::file_io_test_hooks_enabled()) {
            cache_directory = default_thumbnail_directory();
        }
    }
    if (cache_directory.empty() || !g_path_is_absolute(path.c_str()) ||
        !(ascii_ends_with_ci(path, ".svg") || ascii_ends_with_ci(path, ".svgz"))) {
        if (done) done(false);
        return;
    }
    auto job = std::make_shared<DocumentThumbnailJob>();
    job->document = &document;
    job->lifetime = document.saveLifecycleToken();
    job->path = std::move(path);
    job->directory = std::move(cache_directory);
    job->delay_ms = delay_ms;
    job->finder_icon = finder_icon;
    job->done = std::move(done);
    job->begin();
}

} // namespace Inkscape::UI::Cache
