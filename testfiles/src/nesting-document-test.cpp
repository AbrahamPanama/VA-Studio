// SPDX-License-Identifier: GPL-2.0-or-later

#include "nesting/nesting-document.h"
#include "nesting/nesting-settings.h"
#include "nesting/tests/nesting-test-geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <cstdio>
#include <limits>
#include <fstream>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <2geom/rect.h>
#include <2geom/transforms.h>

#include "display/cairo-utils.h"
#include "document-undo.h"
#include "document.h"
#include "inkscape.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-item-group.h"
#include "object/sp-namedview.h"
#include "preferences.h"
#include "svg/svg.h"
#include "xml/repr.h"
#include "xml/subtree-revision.h"

using namespace Inkscape;
using namespace Inkscape::Nesting;

// --- R1 parity gate (consolidated work order 8.7) -----------------------------
// Every preparation in this suite is recorded when VACARDS_NESTING_GOLDEN_OUT
// (write the golden file) or VACARDS_NESTING_GOLDEN_IN (write "<IN>.actual"
// and compare at the end) is set: per part the geometry and content
// fingerprints, contour source and recovery; per snapshot the skipped reasons
// and every metric except timings.
namespace golden {

std::ofstream &stream()
{
    static std::ofstream file = [] {
        std::ofstream opened;
        if (auto const *out = g_getenv("VACARDS_NESTING_GOLDEN_OUT"); out && *out) {
            opened.open(out, std::ios::binary | std::ios::trunc);
        } else if (auto const *in = g_getenv("VACARDS_NESTING_GOLDEN_IN"); in && *in) {
            opened.open(std::string(in) + ".actual", std::ios::binary | std::ios::trunc);
        }
        return opened;
    }();
    return file;
}

bool enabled()
{
    return stream().is_open();
}

std::string number(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.17g", value);
    return buffer;
}

void record(std::string const &context, PreparationResult const &result)
{
    if (!enabled())
        return;
    auto &out = stream();
    out << "PREPARE " << context << '\n';
    if (!result) {
        out << "  error " << result.error << '\n';
        return;
    }
    auto const &s = *result.snapshot;
    out << "  container " << s.container_geometry_fingerprint << '\n';
    for (auto const &part : s.parts) {
        out << "  part " << part.id << ' ' << part.geometry_fingerprint << ' ' << part.content_fingerprint << ' '
            << static_cast<int>(part.contour_source) << ' ' << static_cast<int>(part.recovery) << ' '
            << part.recovery_reason << '\n';
    }
    for (auto const &skipped : s.skipped_parts) {
        out << "  skipped " << static_cast<int>(skipped.reason) << ' ' << skipped.detail << '\n';
    }
    for (auto const &obstacle : s.obstacles) {
        out << "  obstacle " << obstacle.geometry_fingerprint << ' ' << obstacle.content_fingerprint << '\n';
    }
    auto const &m = s.metrics;
    out << "  metrics " << m.selected_count << ' ' << m.prepared_count << ' ' << m.skipped_count << ' '
        << number(m.selected_contour_area) << ' ' << number(m.container_usable_area) << ' ' << m.repaired_count
        << ' ' << m.fallback_count << ' ' << m.ignored_bitmap_part_count << ' ' << m.sparse_vector_count << ' '
        << m.obstacle_count << ' ' << number(m.obstacle_area) << ' ' << m.ignored_background_count << '\n';
    out << "  sources";
    for (auto const count : m.contour_source_counts)
        out << ' ' << count;
    out << '\n';
    for (auto const &detail : m.recovery_details)
        out << "  recovery_detail " << detail.label << " | " << detail.reason << '\n';
    for (auto const &detail : m.skipped_details)
        out << "  skipped_detail " << detail.label << " | " << detail.reason << '\n';
    for (auto const &detail : m.sparse_vector_details)
        out << "  sparse_detail " << detail.label << " | " << detail.reason << '\n';
}

/// Unrecorded preparation, for callers that record under their own name.
PreparationResult raw_prepare(SPItem *container, std::span<SPItem *const> parts)
{
    return Inkscape::Nesting::prepareDocumentNesting(container, parts);
}

PreparationResult recorded_prepare(SPItem *container, std::span<SPItem *const> parts, double tolerance = 0.05,
                                   std::span<SPItem *const> obstacles = {})
{
    auto result = Inkscape::Nesting::prepareDocumentNesting(container, parts, tolerance, obstacles);
    auto const *info = ::testing::UnitTest::GetInstance()->current_test_info();
    record(info ? info->name() : "?", result);
    return result;
}

} // namespace golden

// Route this suite's preparations through the recorder.
#define prepareDocumentNesting golden::recorded_prepare

TEST(NestingSettingsTest, PresetsAndCustomLimitsMapToMilliseconds)
{
    EXPECT_EQ(optimizationTimeMilliseconds(OptimizationTimePreset::Quick, 25'000), 1'000);
    EXPECT_EQ(optimizationTimeMilliseconds(OptimizationTimePreset::Balanced, 25'000), 5'000);
    EXPECT_EQ(optimizationTimeMilliseconds(OptimizationTimePreset::Refined, 25'000), 15'000);
    EXPECT_EQ(optimizationTimeMilliseconds(OptimizationTimePreset::Maximum, 25'000), 60'000);
    EXPECT_EQ(optimizationTimeMilliseconds(OptimizationTimePreset::Unlimited, 25'000), 0);
    EXPECT_EQ(optimizationTimeMilliseconds(OptimizationTimePreset::Custom, 1), MIN_CUSTOM_TIME_MS);
    EXPECT_EQ(optimizationTimeMilliseconds(OptimizationTimePreset::Custom, 25'000), 25'000);
    EXPECT_EQ(optimizationTimeMilliseconds(OptimizationTimePreset::Custom, 999'999), MAX_CUSTOM_TIME_MS);
    EXPECT_EQ(optimizationTimePresetForMilliseconds(5'000), OptimizationTimePreset::Balanced);
    EXPECT_EQ(optimizationTimePresetForMilliseconds(7'500), OptimizationTimePreset::Custom);
    EXPECT_EQ(optimizationTimePresetFromInt(-1), OptimizationTimePreset::Balanced);
    EXPECT_EQ(optimizationTimePresetFromInt(99), OptimizationTimePreset::Balanced);
}

TEST(NestingSettingsTest, PhysicalClearancesConvertToSvgUserUnitsAndPreserveLegacyPixels)
{
    if (!Application::exists()) {
        Application::create(false);
    }
    auto *preferences = Preferences::get();
    auto transaction = preferences->temporaryPreferences();
    constexpr auto path = "/tests/nesting/physical-clearance";

    EXPECT_DOUBLE_EQ(readLengthPreferencePx(*preferences, path), 0.0);

    preferences->setDouble(path, 12.5);
    EXPECT_DOUBLE_EQ(readLengthPreferencePx(*preferences, path), 12.5);

    preferences->setDoubleUnit(path, 25.4, "mm");
    EXPECT_NEAR(readLengthPreferencePx(*preferences, path), 96.0, 1.0e-9);

    preferences->setDoubleUnit(path, -4.0, "px");
    EXPECT_DOUBLE_EQ(readLengthPreferencePx(*preferences, path), 0.0);
}

namespace {

std::unique_ptr<SPDocument> make_document(char const *body)
{
    auto const svg =
        std::string{R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="300" height="240">)svg"} + body + "</svg>";
    auto document = SPDocument::createNewDocFromMem(std::span<char const>{svg.data(), svg.size()});
    if (document) {
        document->ensureUpToDate();
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Initialize nesting fixture"}, "document-new");
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        document->setModifiedSinceSave(false);
    }
    return document;
}

std::unique_ptr<SPDocument> make_document(std::string const &body)
{
    return make_document(body.c_str());
}

SPItem *item(SPDocument &document, char const *id)
{
    return cast<SPItem>(document.getObjectById(id));
}

Geom::Rect point_bounds(std::span<Point const> points)
{
    double left = std::numeric_limits<double>::infinity();
    double top = std::numeric_limits<double>::infinity();
    double right = -std::numeric_limits<double>::infinity();
    double bottom = -std::numeric_limits<double>::infinity();
    for (auto const &point : points) {
        left = std::min(left, point.x);
        top = std::min(top, point.y);
        right = std::max(right, point.x);
        bottom = std::max(bottom, point.y);
    }
    return {Geom::Point{left, top}, Geom::Point{right, bottom}};
}

Geom::Rect rect(double left, double top, double right, double bottom)
{
    return {Geom::Point{left, top}, Geom::Point{right, bottom}};
}

void expect_rect_near(Geom::Rect const &actual, Geom::Rect const &expected, double tolerance = 1.0e-5)
{
    EXPECT_NEAR(actual.left(), expected.left(), tolerance);
    EXPECT_NEAR(actual.top(), expected.top(), tolerance);
    EXPECT_NEAR(actual.right(), expected.right(), tolerance);
    EXPECT_NEAR(actual.bottom(), expected.bottom(), tolerance);
}

bool affine_near(Geom::Affine const &left, Geom::Affine const &right, double tolerance = 1.0e-8)
{
    for (unsigned index = 0; index < 6; ++index) {
        if (std::abs(left[index] - right[index]) > tolerance)
            return false;
    }
    return true;
}

Options draft_options()
{
    Options options;
    options.quality = Quality::Draft;
    options.rotation_mode = RotationMode::None;
    options.random_seed = 17;
    options.time_limit_ms = 100;
    return options;
}

std::string png_data_uri(int width, int height, std::vector<std::uint8_t> const &rgba)
{
    auto *raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    if (!raw)
        return {};
    auto *pixels = gdk_pixbuf_get_pixels(raw);
    auto const rowstride = gdk_pixbuf_get_rowstride(raw);
    auto const channels = gdk_pixbuf_get_n_channels(raw);
    for (int y = 0; y < height; ++y) {
        auto *row = pixels + static_cast<std::ptrdiff_t>(y) * rowstride;
        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < channels; ++channel)
                row[x * channels + channel] = rgba[(static_cast<std::size_t>(y) * width + x) * 4 + channel];
        }
    }
    gchar *png = nullptr;
    gsize png_size = 0;
    GError *error = nullptr;
    auto const saved = gdk_pixbuf_save_to_bufferv(raw, &png, &png_size, "png", nullptr, nullptr, &error);
    std::string uri;
    if (saved && png) {
        gchar *encoded = g_base64_encode(reinterpret_cast<guchar const *>(png), png_size);
        uri = std::string{"data:image/png;base64,"} + encoded;
        g_free(encoded);
    } else if (error) {
        g_error_free(error);
    }
    if (png)
        g_free(png);
    g_object_unref(raw);
    return uri;
}

/** Synthetic raster fixture: opaque listed pixels, transparent elsewhere. */
std::vector<std::uint8_t> alpha_grid(int width, int height, std::vector<std::pair<int, int>> const &opaque)
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4, 0);
    for (auto const &[x, y] : opaque) {
        auto const index = (static_cast<std::size_t>(y) * width + x) * 4;
        rgba[index] = 0;
        rgba[index + 1] = 0;
        rgba[index + 2] = 0;
        rgba[index + 3] = 255;
    }
    return rgba;
}

std::string image_document_body(int width, int height, std::vector<std::uint8_t> const &rgba, double x, double y,
                                double document_width, double document_height)
{
    auto const uri = png_data_uri(width, height, rgba);
    if (uri.empty())
        return {};
    return "<rect id=\"container\" width=\"100\" height=\"100\"/><image id=\"payload\" x=\"" + std::to_string(x) +
           "\" y=\"" + std::to_string(y) + "\" width=\"" + std::to_string(document_width) + "\" height=\"" +
           std::to_string(document_height) + "\" href=\"" + uri + "\"/>";
}

void set_pixbuf_alpha(Inkscape::Pixbuf &pixbuf, int x, int y, std::uint8_t alpha)
{
    auto *raw = pixbuf.getPixbufRaw();
    ASSERT_NE(raw, nullptr);
    auto const channels = gdk_pixbuf_get_n_channels(raw);
    auto *row = gdk_pixbuf_get_pixels(raw) + static_cast<std::ptrdiff_t>(y) * gdk_pixbuf_get_rowstride(raw);
    row[static_cast<std::ptrdiff_t>(x) * channels + channels - 1] = alpha;
}

bool point_in_ring(Point const &point, std::span<Point const> ring)
{
    bool inside = false;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        auto const &a = ring[i];
        auto const &b = ring[j];
        if ((a.y > point.y) != (b.y > point.y) &&
            point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

bool covered_by_components(Point const &point, std::span<CollisionComponent const> components)
{
    for (auto const &component : components) {
        if (!point_in_ring(point, component.outer))
            continue;
        bool inside_hole = false;
        for (auto const &hole : component.holes)
            inside_hole = inside_hole || point_in_ring(point, hole);
        if (!inside_hole)
            return true;
    }
    return false;
}

double ring_area(std::span<Point const> ring)
{
    double twice_area = 0.0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto const a = ring[i];
        auto const b = ring[(i + 1) % ring.size()];
        twice_area += a.x * b.y - b.x * a.y;
    }
    return std::abs(twice_area) * 0.5;
}

double component_net_area(CollisionComponent const &component)
{
    double area = ring_area(component.outer);
    for (auto const &hole : component.holes)
        area -= ring_area(hole);
    return area;
}

double orientation(Point const &a, Point const &b, Point const &c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool segments_cross(Point const &a, Point const &b, Point const &c, Point const &d)
{
    auto const ab_c = orientation(a, b, c);
    auto const ab_d = orientation(a, b, d);
    auto const cd_a = orientation(c, d, a);
    auto const cd_b = orientation(c, d, b);
    return ((ab_c > 0 && ab_d < 0) || (ab_c < 0 && ab_d > 0)) &&
           ((cd_a > 0 && cd_b < 0) || (cd_a < 0 && cd_b > 0));
}

/** Rejects repeated or zero-length vertices and proper non-adjacent crossings. */
bool ring_is_simple(std::span<Point const> ring)
{
    if (ring.size() < 3)
        return false;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        for (std::size_t j = i + 1; j < ring.size(); ++j) {
            if (ring[i].x == ring[j].x && ring[i].y == ring[j].y)
                return false;
        }
    }
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto const &a = ring[i];
        auto const &b = ring[(i + 1) % ring.size()];
        if (a.x == b.x && a.y == b.y)
            return false;
        for (std::size_t j = i + 1; j < ring.size(); ++j) {
            if ((i + 1) % ring.size() == j || (j + 1) % ring.size() == i)
                continue;
            auto const &c = ring[j];
            auto const &d = ring[(j + 1) % ring.size()];
            if (segments_cross(a, b, c, d))
                return false;
        }
    }
    return true;
}

/** True when a hole vertex meets an outer vertex or lies strictly inside an outer edge. */
bool hole_touches_outer(CollisionComponent const &component)
{
    auto const on_edge_interior = [](Point const &p, Point const &a, Point const &b) {
        auto const len = std::hypot(b.x - a.x, b.y - a.y);
        if (len <= 0.0)
            return false;
        auto const cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (std::abs(cross) > 1.0e-9 * len)
            return false;
        auto const dot = (p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y);
        return dot > 1.0e-12 && dot < len * len - 1.0e-12;
    };
    for (auto const &hole : component.holes) {
        for (auto const &hole_point : hole) {
            for (std::size_t i = 0; i < component.outer.size(); ++i) {
                auto const &a = component.outer[i];
                auto const &b = component.outer[(i + 1) % component.outer.size()];
                if (hole_point.x == a.x && hole_point.y == a.y)
                    return true;
                if (on_edge_interior(hole_point, a, b))
                    return true;
            }
        }
    }
    return false;
}

class NestingDocumentTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!Application::exists())
            Application::create(false);
    }
};

} // namespace

TEST_F(NestingDocumentTest, PlacementWritesTransformWithoutBakingPathOrTouchingPaint)
{
    auto document = make_document(R"svg(
      <defs>
        <linearGradient id="paint" x1="0" y1="0" x2="1" y2="0">
          <stop offset="0" stop-color="red"/>
          <stop offset="1" stop-color="blue"/>
        </linearGradient>
      </defs>
      <rect id="container" x="0" y="0" width="100" height="100"/>
      <g id="parent" transform="translate(120,40) scale(2)">
        <path id="part" d="M0,0 h10 v10 z"
              style="fill:url(#paint);stroke:black;stroke-width:0.5"/>
      </g>)svg");
    ASSERT_TRUE(document);
    auto *parent = item(*document, "parent");
    auto *part = item(*document, "part");
    auto const *part_repr = part->getRepr();
    auto const parent_transform = std::string{parent->getRepr()->attribute("transform")};
    auto const path_d = std::string{part_repr->attribute("d") ? part_repr->attribute("d") : ""};
    auto const path_style = std::string{part_repr->attribute("style") ? part_repr->attribute("style") : ""};
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const before_affine = part->i2doc_affine();
    auto const before_bounds = part->documentVisualBounds();
    ASSERT_TRUE(before_bounds);
    auto *gradient = document->getObjectById("paint");
    ASSERT_TRUE(gradient);
    ASSERT_EQ(gradient->getRepr()->attribute("gradientUnits"), nullptr)
        << "fixture must start as an objectBoundingBox gradient";

    std::vector<SPItem *> parts{part};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 1u);
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;
    ASSERT_EQ(solved.placements.size(), 1u);
    ASSERT_TRUE(solved.placements.front().placed);

    // The transformed notification must survive the repr rewrite: exactly one
    // emission, carrying the relative local delta from the previous repr matrix
    // to the new parent-relative matrix (the same ordering doWriteTransform uses).
    int transformed_count = 0;
    Geom::Affine advertised = Geom::identity();
    SPItem *advertised_item = nullptr;
    auto connection = part->connectTransformed([&](Geom::Affine const *delta, SPItem *object) {
        ++transformed_count;
        advertised = delta ? *delta : Geom::identity();
        advertised_item = object;
    });

    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);
    ASSERT_EQ(applied.status, ApplyStatus::Applied) << applied.error;

    auto const &placement = solved.placements.front();
    auto const placement_affine = Geom::Translate(placement.translation_x, placement.translation_y);
    auto const after_affine = part->i2doc_affine();
    auto const after_bounds = part->documentVisualBounds();
    ASSERT_TRUE(after_bounds);
    auto const after = sp_repr_save_buf(document->getReprDoc()).raw();

    EXPECT_EQ(transformed_count, 1) << "placement must emit exactly one transformed notification";
    EXPECT_EQ(advertised_item, part);
    auto const expected_local = after_affine * parent->i2doc_affine().inverse();
    EXPECT_TRUE(affine_near(advertised, expected_local))
        << "advertised delta must be the parent-relative local change, not the document translation";

    // Only the selected item changed: placement is a top-level transform, and
    // path data / style / parent scope / referenced paint are untouched.
    EXPECT_TRUE(affine_near(after_affine, before_affine * placement_affine));
    EXPECT_NE(part_repr->attribute("transform"), nullptr)
        << "placement must be stored in the item transform attribute";
    EXPECT_EQ(std::string{parent->getRepr()->attribute("transform")}, parent_transform);
    EXPECT_EQ(std::string{part_repr->attribute("d") ? part_repr->attribute("d") : ""}, path_d)
        << "placement must not be baked into path data";
    EXPECT_EQ(std::string{part_repr->attribute("style") ? part_repr->attribute("style") : ""}, path_style)
        << "style must not be rewritten";
    EXPECT_EQ(part->parent, parent) << "children must stay under their original parent";
    expect_rect_near(*after_bounds,
                     rect(before_bounds->left() + placement.translation_x,
                          before_bounds->top() + placement.translation_y,
                          before_bounds->right() + placement.translation_x,
                          before_bounds->bottom() + placement.translation_y));
    EXPECT_EQ(gradient->getRepr()->attribute("gradientUnits"), nullptr)
        << "referenced paint must keep objectBoundingBox semantics (defs not mutated)";
    ASSERT_NE(after.find("url(#paint)"), std::string::npos);

    // A single undo/redo entry restores and reapplies both bounds and affine.
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    part = item(*document, "part");
    ASSERT_TRUE(part);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before)
        << "one undo must restore the exact pre-placement document";
    EXPECT_TRUE(affine_near(part->i2doc_affine(), before_affine));
    auto const undone_bounds = part->documentVisualBounds();
    ASSERT_TRUE(undone_bounds);
    expect_rect_near(*undone_bounds, *before_bounds);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    part = item(*document, "part");
    ASSERT_TRUE(part);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after)
        << "one redo must reapply the exact placed document";
    EXPECT_TRUE(affine_near(part->i2doc_affine(), after_affine, 1.0e-6))
        << "redo affine comes back through 10-digit SVG serialization";
    auto const redone_bounds = part->documentVisualBounds();
    ASSERT_TRUE(redone_bounds);
    expect_rect_near(*redone_bounds, *after_bounds);
    EXPECT_FALSE(DocumentUndo::redo(document.get()));
}

TEST_F(NestingDocumentTest, ReferencedResourceChainsAndQuotedUrlEditsInvalidateSnapshot)
{
    auto run_scenario = [](char const *body, char const *part_id, char const *content_id) {
        auto document = make_document(body);
        ASSERT_TRUE(document);
        auto *part = item(*document, part_id);
        ASSERT_TRUE(part) << part_id;
        std::vector<SPItem *> parts{part};
        auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
        ASSERT_TRUE(prepared) << prepared.error;
        ASSERT_EQ(prepared.snapshot->parts.size(), 1);
        auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
        ASSERT_TRUE(solved) << solved.error;
        auto const bounds_of = [](SPItem *object) {
            if (auto visual = object->documentVisualBounds())
                return visual;
            return object->geometricBounds();
        };
        auto const bounds_before = bounds_of(part);
        ASSERT_TRUE(bounds_before);
        auto const transform_before = part->i2doc_affine();

        auto *content = document->getObjectById(content_id);
        ASSERT_TRUE(content) << content_id;
        ASSERT_TRUE(content->getRepr());
        content->getRepr()->setAttribute("fill", "blue");
        document->ensureUpToDate();
        auto const bounds_after = bounds_of(part);
        ASSERT_TRUE(bounds_after);
        expect_rect_near(*bounds_after, *bounds_before);
        auto const after_edit = sp_repr_save_buf(document->getReprDoc()).raw();

        auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

        EXPECT_EQ(applied.status, ApplyStatus::StaleSnapshot)
            << "referenced " << content_id << " content changed with identical bounds";
        EXPECT_TRUE(affine_near(part->i2doc_affine(), transform_before));
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after_edit);
    };

    // marker -> use -> path chain: the changed path is outside the marker's XML
    // subtree, so the fingerprint must follow the use's href to catch it.
    run_scenario(R"svg(
      <defs>
        <marker xmlns:xlink="http://www.w3.org/1999/xlink" id="mark" markerWidth="4" markerHeight="4"
                refX="0" refY="0" orient="auto">
          <use id="mark-use" xlink:href="#leaf"/>
        </marker>
        <path id="leaf" d="M 0,0 h 4 v 4 h -4 z" fill="red"/>
      </defs>
      <rect id="container" width="100" height="100"/>
      <path id="marked-part" d="M 180,10 H 220" fill="none" stroke="black" stroke-width="2"
            marker-end="url(#mark)"/>)svg",
                 "marked-part", "leaf");

    // Quoted and space-padded local URL must be parsed as a real reference.
    run_scenario(R"svg(
      <defs>
        <clipPath id="clip"><rect id="clip-content" x="0" y="0" width="20" height="10" fill="red"/></clipPath>
      </defs>
      <rect id="container" width="100" height="100"/>
      <rect id="clipped-part" x="120" y="10" width="40" height="40" clip-path="url( '#clip' )"/>)svg",
                 "clipped-part", "clip-content");
}

TEST_F(NestingDocumentTest, ManyChildGroupOverRingCapFallsBackToWholeGroupBounds)
{
    // Bounded-complexity regression through the public prepare path: a group
    // with more cheap child rectangles than the aggregate ring cap (4096) must
    // stop aggregate growth and diagnose the whole group instead of publishing
    // an arbitrary partial aggregate. The whole-group bounds must still include
    // the last child, proving the fallback did not silently truncate the tree.
    constexpr int child_count = 4'200;
    std::string body = R"svg(<rect id="container" width="100" height="100"/><g id="many">)svg";
    for (int index = 0; index < child_count; ++index) {
        auto const x = 200 + (index % 40) * 2;
        auto const y = 10 + (index / 40) * 2;
        body += "<rect id=\"child-" + std::to_string(index) + "\" x=\"" + std::to_string(x) + "\" y=\"" +
                std::to_string(y) + "\" width=\"1\" height=\"1\"/>";
    }
    body += "</g>";
    auto document = make_document(body);
    ASSERT_TRUE(document);
    auto *group = item(*document, "many");
    ASSERT_TRUE(group);
    EXPECT_EQ(group->children.size(), static_cast<std::size_t>(child_count));
    std::vector<SPItem *> parts{group};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 1u);
    auto const &part = prepared.snapshot->parts.front();
    EXPECT_EQ(part.recovery, RecoveryKind::ConservativeFallback)
        << "over-cap aggregate must be diagnosed, not silently truncated";
    EXPECT_EQ(part.contour_source, ContourSource::ConservativeBounds);
    ASSERT_FALSE(part.components.empty());
    // The last child (index 4199) is the unit rectangle at (278,218), so the
    // whole-group fallback must reach (279,219); a partial aggregate would stop
    // around x=200..240, y=10..110.
    expect_rect_near(point_bounds(part.components.front().outer), rect(200, 10, 279, 219));
}

TEST_F(NestingDocumentTest, PreparationCapturesDocumentCoordinatesAndContainerHolesWithoutMutation)
{
    auto document = make_document(R"svg(
      <g transform="translate(5,7)">
        <path id="container" fill-rule="evenodd"
              d="M 0,0 H 50 V 40 H 0 Z M 10,10 H 20 V 20 H 10 Z"/>
      </g>
      <g transform="translate(13,17)">
        <rect id="part" x="100" y="80" width="8" height="6"/>
      </g>)svg");
    ASSERT_TRUE(document);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::vector<SPItem *> parts{item(*document, "part")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->container_holes.size(), 1);
    expect_rect_near(point_bounds(prepared.snapshot->container_outline), rect(5, 7, 55, 47));
    expect_rect_near(point_bounds(prepared.snapshot->container_holes.front()), rect(15, 17, 25, 27));
    ASSERT_EQ(prepared.snapshot->parts.size(), 1);
    ASSERT_EQ(prepared.snapshot->parts.front().components.size(), 1);
    expect_rect_near(point_bounds(prepared.snapshot->parts.front().components.front().outer), rect(113, 97, 121, 103));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingDocumentTest, DisconnectedPartUsesAConservativeHull)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="100" height="100"/>
      <path id="part" d="M 120,20 h 10 v 10 h -10 z M 150,40 h 10 v 10 h -10 z"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 1);
    EXPECT_EQ(prepared.snapshot->parts.front().contour_source, ContourSource::ConservativeHull);
    expect_rect_near(point_bounds(prepared.snapshot->parts.front().components.front().outer), rect(120, 20, 160, 50));
}

TEST_F(NestingDocumentTest, GroupPartKeepsVisibleChildrenAsRigidCollisionComponents)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="100" height="100"/>
      <g id="part" transform="translate(120,30)">
        <circle cx="0" cy="0" r="5"/>
        <rect x="15" y="10" width="7" height="8"/>
      </g>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 1);
    auto const &part = prepared.snapshot->parts.front();
    EXPECT_EQ(part.contour_source, ContourSource::CompoundVector);
    ASSERT_EQ(part.components.size(), 2);
    expect_rect_near(point_bounds(part.components[0].outer), rect(115, 25, 125, 35), 0.01);
    expect_rect_near(point_bounds(part.components[1].outer), rect(135, 40, 142, 48), 0.01);
}

TEST_F(NestingDocumentTest, ArtworkInsideTheCutOutlineAddsNoCollisionComponent)
{
    // A cut outline with printed artwork inside it (the usual sticker group):
    // the artwork cannot change what the part occupies, so only the outline
    // collides. Artwork crossing or touching the outline still counts. Part
    // holes are filled for collision, so artwork in a hole is enclosed too.
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="100" height="100"/>
      <g id="sticker" transform="translate(120,20)">
        <path d="M 0,0 h 40 v 30 h -40 z"/>
        <circle cx="12" cy="15" r="6"/>
        <rect x="24" y="8" width="10" height="12"/>
      </g>
      <g id="crossing" transform="translate(120,70)">
        <path d="M 0,0 h 40 v 30 h -40 z"/>
        <rect x="30" y="10" width="20" height="10"/>
      </g>
      <g id="in-hole" transform="translate(200,20)">
        <path d="M 0,0 h 40 v 40 h -40 z M 10,10 v 20 h 20 v -20 z" fill-rule="evenodd"/>
        <circle cx="20" cy="20" r="4"/>
      </g>
      <g id="touching" transform="translate(200,80)">
        <path d="M 0,0 h 40 v 30 h -40 z"/>
        <rect x="0" y="10" width="8" height="8"/>
      </g>
      <g id="chain" transform="translate(280,20)">
        <path d="M 0,0 h 40 v 40 h -40 z"/>
        <circle cx="20" cy="20" r="12"/>
        <circle cx="20" cy="20" r="3"/>
      </g>
      <g id="duplicates" transform="translate(280,80)">
        <rect x="0" y="0" width="20" height="10"/>
        <rect x="0" y="0" width="20" height="10"/>
      </g>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "sticker"), item(*document, "crossing"), item(*document, "in-hole"),
                                item(*document, "touching"), item(*document, "chain"),   item(*document, "duplicates")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 6);
    auto const &sticker = prepared.snapshot->parts[0];
    ASSERT_EQ(sticker.components.size(), 1);
    expect_rect_near(point_bounds(sticker.components.front().outer), rect(120, 20, 160, 50), 0.01);
    EXPECT_EQ(prepared.snapshot->parts[1].components.size(), 2) << "artwork crossing the outline";
    EXPECT_EQ(prepared.snapshot->parts[2].components.size(), 1) << "part holes are filled";
    EXPECT_EQ(prepared.snapshot->parts[3].components.size(), 2) << "artwork touching the outline";
    EXPECT_EQ(prepared.snapshot->parts[4].components.size(), 1) << "nested artwork inside nested artwork";
    EXPECT_EQ(prepared.snapshot->parts[5].components.size(), 2) << "identical copies touch; both are kept";
}

TEST_F(NestingDocumentTest, CompoundGroupSolvesAndAppliesOnlyOneTopLevelTransform)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="100" height="100"/>
      <g id="part" transform="translate(140,20)">
        <rect id="child-a" x="0" y="0" width="10" height="10"/>
        <g transform="translate(30,20)"><rect id="child-b" width="8" height="6"/></g>
      </g>)svg");
    ASSERT_TRUE(document);
    auto *part = item(*document, "part");
    auto const child_a_before = item(*document, "child-a")->transform;
    auto const child_b_before = item(*document, "child-b")->transform;
    std::vector<SPItem *> parts{part};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.front().components.size(), 2);
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;
    ASSERT_TRUE(solved.placements.front().placed);

    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);
    EXPECT_TRUE(applied.changed()) << applied.error;
    EXPECT_TRUE(affine_near(item(*document, "child-a")->transform, child_a_before));
    EXPECT_TRUE(affine_near(item(*document, "child-b")->transform, child_b_before));
    EXPECT_FALSE(affine_near(part->i2doc_affine(), prepared.snapshot->parts.front().original_item_to_document));
}

TEST_F(NestingDocumentTest, FilledOpenStrokedPathIncludesItsInteriorInRigidGroup)
{
    auto document = make_document(R"svg(
      <rect id="container" width="100" height="100"/>
      <g id="part" transform="translate(120,20)">
        <path id="gold" d="M0,0 H20 V20" fill="#aa8800" stroke="black" stroke-width=".12"/>
        <path id="brown" d="M15,2 H18 V5 Z" fill="#2b2200"/>
      </g>)svg");
    std::vector<SPItem *> parts{item(*document, "part")};
    auto before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto const &components = prepared.snapshot->parts.front().components;
    // The brown triangle lies inside the gold interior, so it adds nothing.
    ASSERT_EQ(components.size(), 1);
    // The old bounding-box helper captured just a thin L-shaped stroke,
    // omitting the triangle's filled interior. It must be reserved too.
    double twice_area = 0;
    auto const &outer = components.front().outer;
    for (std::size_t i = 0; i < outer.size(); ++i) {
        auto a = outer[i], b = outer[(i + 1) % outer.size()];
        twice_area += a.x * b.y - b.x * a.y;
    }
    EXPECT_GT(std::abs(twice_area) * .5, 200);
    EXPECT_LT(std::abs(twice_area) * .5, 205);
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;
    ASSERT_TRUE(solved.placements.front().placed);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
}

TEST_F(NestingDocumentTest, UnfilledStrokedOpenPathRetainsItsEmptyInterior)
{
    auto document = make_document(R"svg(
      <rect id="container" width="100" height="100"/>
      <path id="part" d="M120,20 H140 V40" fill="none" stroke="black" stroke-width="2"/>
      <rect id="other" x="160" y="20" width="3" height="3"/>)svg");
    std::vector<SPItem *> parts{item(*document, "part"), item(*document, "other")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    // Place a square beside the L's stroke, inside its bounding rectangle.
    // A spurious fill/convex hull would wrongly reject this arrangement.
    Job validator(draft_options());
    ASSERT_EQ(validator.setContainer(prepared.snapshot->container_outline), Status::Ok);
    for (auto const &part : prepared.snapshot->parts)
        ASSERT_EQ(validator.addPart(part.id, part.components), Status::Ok);
    std::vector<Placement> placements{{1, -110, -10, 0, true}, {2, -145, -5, 0, true}};
    EXPECT_EQ(validator.validate(placements), Status::Ok) << validator.error();
}

// The owner's real artwork stays outside source control. Run with the original
// SVG explicitly supplied; output is written only to a separate evidence folder.
TEST_F(NestingDocumentTest, PrivateGroupedArtworkNestsWithoutChangingItsChildren)
{
    auto path = g_getenv("VACARDS_NESTING_PRIVATE_SVG");
    if (!path)
        GTEST_SKIP() << "Set VACARDS_NESTING_PRIVATE_SVG for the owner's artwork";
    std::ifstream input(path);
    ASSERT_TRUE(input.good());
    std::string svg{std::istreambuf_iterator<char>(input), {}};
    auto end = svg.rfind("</svg>");
    ASSERT_NE(end, std::string::npos);
    svg.insert(end, R"(<rect xmlns="http://www.w3.org/2000/svg" id="test-sheet"
        x="200" y="0" width="240" height="240" fill="none" stroke="blue"/>)");
    for (int scenario = 0; scenario < 3; ++scenario) {
        SCOPED_TRACE(scenario);
        auto document = SPDocument::createNewDocFromMem(std::span<char const>{svg.data(), svg.size()});
        ASSERT_TRUE(document);
        document->ensureUpToDate();
        auto *group = item(*document, "g19-8-2-9-6-7");
        ASSERT_TRUE(group);
        std::vector<SPItem *> parts{group};
        if (scenario == 1) {
            // Select the two children individually, with their inherited transform,
            // exactly as when selecting inside a group. Never change the source.
            parts = cast<SPGroup>(group)->item_list();
            ASSERT_EQ(parts.size(), 2);
        } else if (scenario == 2) {
            for (int copy = 1; copy < 4; ++copy) {
                auto repr = group->getRepr()->duplicate(document->getReprDoc());
                auto const suffix = "-copy-" + std::to_string(copy);
                auto const id = std::string(group->getId()) + suffix;
                repr->setAttribute("id", id);
                for (auto child = repr->firstChild(); child; child = child->next()) {
                    if (auto child_id = child->attribute("id"))
                        child->setAttribute("id", std::string(child_id) + suffix);
                }
                group->parent->getRepr()->appendChild(repr);
                GC::release(repr);
                parts.push_back(item(*document, id.c_str()));
            }
            document->ensureUpToDate();
        }
        std::vector<std::pair<std::string, std::string>> child_xml;
        auto capture_children = [&](auto const &self, SPObject *object) -> void {
            for (auto &child : object->children) {
                if (child.getId())
                    child_xml.emplace_back(child.getId(),
                                           sp_repr_write_buf(child.getRepr(), 0, false, GQuark(0), 0, 0).raw());
                self(self, &child);
            }
        };
        for (auto part : parts)
            capture_children(capture_children, part);
        DocumentUndo::done(document.get(), Util::Internal::ContextString{"Initialize private fixture"}, "");
        DocumentUndo::clearUndo(document.get());
        DocumentUndo::clearRedo(document.get());
        auto before = sp_repr_save_buf(document->getReprDoc()).raw();
        auto prepared = prepareDocumentNesting(item(*document, "test-sheet"), parts);
        ASSERT_TRUE(prepared) << prepared.error;
        for (auto const &p : prepared.snapshot->parts) {
            std::cout << "Captured part " << p.id << " components " << p.components.size() << "\n";
            for (auto const &c : p.components)
                std::cout << "  vertices " << c.outer.size() << " holes " << c.holes.size() << "\n";
        }
        auto options = draft_options();
        options.time_limit_ms = 5000;
        if (scenario == 2) {
            options.part_spacing = 2;
            options.container_margin = 3;
            options.rotation_mode = RotationMode::RightAngles;
        }
        auto solved = solvePreparedNesting(*prepared.snapshot, options);
        ASSERT_TRUE(solved) << solved.error;
        std::cout << "Solved via " << solved.backend << " in " << solved.metrics.elapsed_seconds << " seconds\n";
        ASSERT_EQ(solved.placements.size(), parts.size());
        for (auto p : solved.placements)
            ASSERT_TRUE(p.placed);
        Job validator(options);
        ASSERT_EQ(validator.setContainer(prepared.snapshot->container_outline), Status::Ok);
        for (auto const &part : prepared.snapshot->parts)
            ASSERT_EQ(validator.addPart(part.id, part.components), Status::Ok);
        ASSERT_EQ(validator.validate(solved.placements), Status::Ok) << validator.error();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
        auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);
        ASSERT_TRUE(applied.changed()) << applied.error;
        auto after = sp_repr_save_buf(document->getReprDoc()).raw();
        auto verify_children = [&](SPDocument &doc) {
            for (auto const &[id, xml] : child_xml) {
                auto child = doc.getObjectById(id);
                ASSERT_TRUE(child) << id;
                EXPECT_EQ(sp_repr_write_buf(child->getRepr(), 0, false, GQuark(0), 0, 0).raw(), xml);
            }
        };
        verify_children(*document);
        ASSERT_TRUE(DocumentUndo::undo(document.get()));
        document->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
        EXPECT_FALSE(DocumentUndo::undo(document.get()));
        ASSERT_TRUE(DocumentUndo::redo(document.get()));
        document->ensureUpToDate();
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after);
        auto reopened = SPDocument::createNewDocFromMem(std::span<char const>{after.data(), after.size()});
        ASSERT_TRUE(reopened);
        reopened->ensureUpToDate();
        verify_children(*reopened);
        for (auto part : parts) {
            auto restored = item(*reopened, part->getId());
            ASSERT_TRUE(restored);
            EXPECT_TRUE(affine_near(restored->i2doc_affine(), part->i2doc_affine()));
        }
        if (auto output = g_getenv("VACARDS_NESTING_PRIVATE_OUTPUT_DIR")) {
            auto dir = std::filesystem::path(output);
            std::filesystem::create_directories(dir);
            std::ofstream saved(dir / ("calcifer-" + std::to_string(scenario) + ".svg"));
            saved << after;
            ASSERT_TRUE(saved.good());
        }
    }
}

TEST_F(NestingDocumentTest, ExplicitNestingContourOverridesArtworkAndBitmapBounds)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="100" height="100"/>
      <g id="payload" transform="translate(120,10)" inkscape:nesting-contour-version="1"
         xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
        <rect id="artwork" x="-20" y="-20" width="80" height="70" fill="red"/>
        <path id="cutline" inkscape:nesting-contour="true" d="M0,0 H20 V10 H0 Z"/>
      </g>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "payload")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    auto const &part = prepared.snapshot->parts.front();
    EXPECT_EQ(part.contour_source, ContourSource::ExplicitContour);
    ASSERT_EQ(part.components.size(), 1);
    expect_rect_near(point_bounds(part.components.front().outer), rect(120, 10, 140, 20));
}

std::string vector_and_bitmap_group(std::string const &uri, bool with_vector)
{
    // A 20 x 10 vector cut shape with a larger, fully opaque 40 x 40 bitmap
    // (bleed) around it, inside a nested group as design tools produce.
    std::string body = R"svg(<rect id="container" width="100" height="100"/>
      <g id="payload" transform="translate(120,10)">)svg";
    if (with_vector)
        body += R"svg(<rect id="cut" x="10" y="15" width="20" height="10"/>)svg";
    body += R"svg(<g><image x="0" y="0" width="40" height="40" href=")svg" + uri + R"svg("/></g></g>)svg";
    return body;
}

TEST_F(NestingDocumentTest, GroupWithVectorArtworkIgnoresItsBitmaps)
{
    std::vector<std::pair<int, int>> all;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            all.emplace_back(x, y);
    auto const uri = png_data_uri(4, 4, alpha_grid(4, 4, all));
    ASSERT_FALSE(uri.empty());
    auto document = make_document(vector_and_bitmap_group(uri, true));
    ASSERT_TRUE(document);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::vector<SPItem *> parts{item(*document, "payload")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 1);
    auto const &part = prepared.snapshot->parts.front();
    ASSERT_EQ(part.components.size(), 1) << "the bitmap must not add a collision component";
    expect_rect_near(point_bounds(part.components.front().outer), rect(130, 25, 150, 35), 0.01);
    EXPECT_NE(part.contour_source, ContourSource::BitmapAlpha);
    EXPECT_EQ(part.recovery, RecoveryKind::Clean);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);

    // The same group without vector artwork still nests by the bitmap outline.
    auto bitmap_only = make_document(vector_and_bitmap_group(uri, false));
    ASSERT_TRUE(bitmap_only);
    std::vector<SPItem *> bitmap_parts{item(*bitmap_only, "payload")};
    auto bitmap_prepared = prepareDocumentNesting(item(*bitmap_only, "container"), bitmap_parts);
    ASSERT_TRUE(bitmap_prepared) << bitmap_prepared.error;
    ASSERT_EQ(bitmap_prepared.snapshot->parts.size(), 1);
    expect_rect_near(point_bounds(bitmap_prepared.snapshot->parts.front().components.front().outer),
                     rect(120, 10, 160, 50), 0.01);
}

TEST_F(NestingDocumentTest, SmallVectorArtworkOnLargeBitmapIsReported)
{
    std::vector<std::pair<int, int>> all;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            all.emplace_back(x, y);
    auto const uri = png_data_uri(4, 4, alpha_grid(4, 4, all));
    ASSERT_FALSE(uri.empty());
    auto const image = R"svg(<image x="0" y="0" width="40" height="40" href=")svg" + uri + R"svg("/>)svg";
    auto document = make_document(R"svg(<rect id="container" width="200" height="200"/>
      <g id="text-on-photo" transform="translate(220,10)">)svg" + image +
                                  R"svg(<rect x="10" y="15" width="20" height="10"/></g>
      <g id="cut-around-photo" transform="translate(280,10)">)svg" + image +
                                  R"svg(<rect x="-2" y="-2" width="44" height="44" fill="none" stroke="black"/></g>
      <g id="marked" transform="translate(340,10)" inkscape:nesting-contour-version="1"
         xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">)svg" + image +
                                  R"svg(<path inkscape:nesting-contour="true" d="M10,15 H30 V25 H10 Z"/></g>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "text-on-photo"), item(*document, "cut-around-photo"),
                                item(*document, "marked")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 3);
    auto const &metrics = prepared.snapshot->metrics;
    // The explicit contour is deliberate and is not counted at all.
    EXPECT_EQ(metrics.ignored_bitmap_part_count, 2);
    EXPECT_EQ(metrics.sparse_vector_count, 1) << "only the 20 x 10 vector on a 40 x 40 photo is sparse";
    ASSERT_EQ(metrics.sparse_vector_details.size(), 1);
    EXPECT_FALSE(metrics.sparse_vector_details.front().label.empty());
}

TEST_F(NestingDocumentTest, VectorClipIsUsedWhenNoExplicitContourExists)
{
    auto document = make_document(R"svg(
      <defs><clipPath id="clip"><path d="M120,20 H140 V35 H120 Z"/></clipPath></defs>
      <rect id="container" x="0" y="0" width="100" height="100"/>
      <rect id="payload" x="100" y="0" width="80" height="70" clip-path="url(#clip)"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "payload")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    auto const &part = prepared.snapshot->parts.front();
    EXPECT_EQ(part.contour_source, ContourSource::VectorClip);
    ASSERT_EQ(part.components.size(), 1);
    expect_rect_near(point_bounds(part.components.front().outer), rect(120, 20, 140, 35));
}

TEST_F(NestingDocumentTest, TransparentBitmapUsesItsAlphaSilhouetteWithoutChangingPixels)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="100" height="100"/>
      <image id="payload" x="120" y="10" width="40" height="40"
       href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAQAAAAEAQMAAACTPww9AAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAAGUExURQAAAP8AABv/jSIAAAABdFJOUwBA5thmAAAAB3RJTUUH6gkCFhQBnRAAvwAAACV0RVh0ZGF0ZTpjcmVhdGUAMjAyNi0wOS0wMlQyMjoyMDowMSswMDowMFinR18AAAAldEVYdGRhdGU6bW9kaWZ5ADIwMjYtMDktMDJUMjI6MjA6MDErMDA6MDAp+v/jAAAAKHRFWHRkYXRlOnRpbWVzdGFtcAAyMDI2LTA5LTAyVDIyOjIwOjAxKzAwOjAwfu/ePAAAAA5JREFUCNdjYGBIAEIGAAMIAMEmXw9bAAAAAElFTkSuQmCC"/>)svg");
    ASSERT_TRUE(document);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::vector<SPItem *> parts{item(*document, "payload")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    auto const &part = prepared.snapshot->parts.front();
    EXPECT_EQ(part.contour_source, ContourSource::BitmapAlpha);
    EXPECT_EQ(prepared.snapshot->metrics.contour_source_counts[static_cast<std::size_t>(ContourSource::BitmapAlpha)],
              1);
    ASSERT_EQ(part.components.size(), 1);
    expect_rect_near(point_bounds(part.components.front().outer), rect(130, 20, 150, 40), 0.01);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
}

TEST_F(NestingDocumentTest, AmbiguousOrClippedContainerIsRejectedWithoutDocumentChanges)
{
    auto document = make_document(R"svg(
      <defs><clipPath id="clip"><rect width="10" height="10"/></clipPath></defs>
      <path id="disconnected" d="M0,0 h20 v20 h-20 z M30,0 h20 v20 h-20 z"/>
      <rect id="clipped" width="50" height="50" clip-path="url(#clip)"/>
      <rect id="part" x="80" y="80" width="5" height="5"/>)svg");
    ASSERT_TRUE(document);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::vector<SPItem *> parts{item(*document, "part")};

    auto disconnected = prepareDocumentNesting(item(*document, "disconnected"), parts);
    auto clipped = prepareDocumentNesting(item(*document, "clipped"), parts);

    EXPECT_FALSE(disconnected);
    EXPECT_NE(disconnected.error.find("exactly one"), std::string::npos);
    EXPECT_FALSE(clipped);
    EXPECT_NE(clipped.error.find("clipped or masked"), std::string::npos);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(document->isModifiedSinceSave());
}

TEST_F(NestingDocumentTest, PreparationSkipsContainerDuplicatesAndLockedPartsButKeepsUsableParts)
{
    auto document = make_document(R"svg(
      <rect id="container" width="100" height="100"/>
      <rect id="usable" x="120" y="10" width="10" height="10"/>
      <rect id="locked" x="140" y="10" width="10" height="10" sodipodi:insensitive="true"
            xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"/>)svg");
    ASSERT_TRUE(document);
    auto *container = item(*document, "container");
    auto *usable = item(*document, "usable");
    auto *locked = item(*document, "locked");
    std::vector<SPItem *> parts{container, usable, usable, locked, nullptr};

    auto prepared = prepareDocumentNesting(container, parts);

    ASSERT_TRUE(prepared) << prepared.error;
    EXPECT_EQ(prepared.snapshot->parts.size(), 1);
    EXPECT_EQ(prepared.snapshot->parts.front().item.get(), usable);
    EXPECT_EQ(prepared.snapshot->skipped_parts.size(), 4);
}

TEST_F(NestingDocumentTest, SolverMovesAllFittingPartsAndUndoRedoTreatsItAsOneOperation)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="50" height="25"/>
      <rect id="part1" x="100" y="80" width="10" height="10" fill="red"/>
      <rect id="part2" x="140" y="100" width="10" height="10" fill="blue"/>)svg");
    ASSERT_TRUE(document);
    auto *container = item(*document, "container");
    auto container_transform = container->i2doc_affine();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::vector<SPItem *> parts{item(*document, "part1"), item(*document, "part2")};
    auto prepared = prepareDocumentNesting(container, parts);
    ASSERT_TRUE(prepared) << prepared.error;
    EXPECT_EQ(prepared.snapshot->metrics.selected_count, 2);
    EXPECT_EQ(prepared.snapshot->metrics.prepared_count, 2);
    EXPECT_EQ(prepared.snapshot->metrics.skipped_count, 0);
    EXPECT_NEAR(prepared.snapshot->metrics.selected_contour_area, 200.0, 0.01);
    EXPECT_NEAR(prepared.snapshot->metrics.container_usable_area, 1250.0, 0.01);
    EXPECT_GE(prepared.snapshot->metrics.elapsed_seconds, 0.0);
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;
    EXPECT_GT(solved.metrics.iterations, 0);
    EXPECT_EQ(solved.metrics.placed_count, 2);
    EXPECT_NEAR(solved.metrics.placed_contour_area, 200.0, 0.01);
    EXPECT_NEAR(solved.metrics.utilization_percent, 16.0, 0.01);
    EXPECT_GE(solved.metrics.initial_solution_seconds, 0.0);
    EXPECT_GE(solved.metrics.elapsed_seconds, solved.metrics.initial_solution_seconds);
    EXPECT_GE(solved.metrics.refinement_seconds, 0.0);

    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    ASSERT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_EQ(applied.moved_count, 2);
    EXPECT_EQ(applied.placed_count, 2);
    EXPECT_EQ(applied.unplaced_count, 0);
    EXPECT_TRUE(affine_near(container->i2doc_affine(), container_transform));
    for (auto const *id : {"part1", "part2"}) {
        auto bounds = item(*document, id)->documentVisualBounds();
        ASSERT_TRUE(bounds);
        EXPECT_GE(bounds->left(), -0.001);
        EXPECT_GE(bounds->top(), -0.001);
        EXPECT_LE(bounds->right(), 50.001);
        EXPECT_LE(bounds->bottom(), 25.001);
    }
    auto const after = sp_repr_save_buf(document->getReprDoc()).raw();
    EXPECT_NE(after, before);

    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));

    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after);
    EXPECT_FALSE(DocumentUndo::redo(document.get()));
}

TEST_F(NestingDocumentTest, CancellationRequestedBeforeSolvePublishesNoResult)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="50" height="25"/>
      <rect id="part" x="100" y="80" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::stop_source cancellation;
    cancellation.request_stop();

    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options(), {}, cancellation.get_token());

    EXPECT_EQ(solved.status, Status::Cancelled);
    EXPECT_TRUE(solved.placements.empty());
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingDocumentTest, OversizedPartRemainsExactlyWhereItWasWhileLaterPartIsPlaced)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="30" height="30"/>
      <rect id="oversized" x="100" y="80" width="40" height="40"/>
      <rect id="small" x="160" y="100" width="8" height="8"/>)svg");
    ASSERT_TRUE(document);
    auto *oversized = item(*document, "oversized");
    auto const oversized_transform = oversized->i2doc_affine();
    auto const oversized_repr =
        std::string{oversized->getRepr()->attribute("transform") ? oversized->getRepr()->attribute("transform") : ""};
    std::vector<SPItem *> parts{oversized, item(*document, "small")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    ASSERT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_EQ(applied.placed_count, 1);
    EXPECT_EQ(applied.unplaced_count, 1);
    EXPECT_TRUE(affine_near(oversized->i2doc_affine(), oversized_transform));
    EXPECT_EQ(
        std::string{oversized->getRepr()->attribute("transform") ? oversized->getRepr()->attribute("transform") : ""},
        oversized_repr);
}

namespace {

char const *const LEFTOVERS = R"svg(
      <rect id="container" x="0" y="0" width="60" height="30"/>
      <rect id="small" x="160" y="100" width="8" height="8"/>
      <rect id="wide1" x="100" y="150" width="100" height="20"/>
      <rect id="wide2" x="100" y="180" width="80" height="20"/>
      <rect id="wide3" x="100" y="210" width="90" height="20"/>)svg";

std::string transform_attribute(SPItem *item)
{
    auto const *value = item->getRepr()->attribute("transform");
    return value ? value : "";
}

} // namespace

TEST_F(NestingDocumentTest, LeftoversMoveBesideSheetInOneUndoStep)
{
    auto document = make_document(LEFTOVERS);
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "wide1"), item(*document, "small"), item(*document, "wide2"),
                                item(*document, "wide3")};
    std::vector<std::string> before;
    for (auto *part : parts)
        before.push_back(transform_attribute(part));
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements,
                                          LeftoverPlacement{.move_beside_container = true, .gap = 5.0});

    ASSERT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_EQ(applied.placed_count, 1u);
    EXPECT_EQ(applied.unplaced_count, 3u);
    EXPECT_EQ(applied.leftover_moved_count, 3u);
    // Selection order; a new column starts when the next part would pass the
    // sheet's bottom (y = 30): 20 + 5 + 20 > 30.
    auto const bounds = [&](char const *id) { return *item(*document, id)->documentVisualBounds(); };
    expect_rect_near(bounds("wide1"), rect(65, 0, 165, 20));
    expect_rect_near(bounds("wide2"), rect(170, 0, 250, 20));
    expect_rect_near(bounds("wide3"), rect(255, 0, 345, 20));
    EXPECT_TRUE(Geom::Rect(0, 0, 60, 30).contains(bounds("small")));

    // One Undo step restores every part; one Redo step repeats all of it.
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    for (std::size_t i = 0; i < parts.size(); ++i)
        EXPECT_EQ(transform_attribute(item(*document, parts[i]->getId())), before[i]) << parts[i]->getId();
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    expect_rect_near(bounds("wide3"), rect(255, 0, 345, 20));
    EXPECT_FALSE(DocumentUndo::redo(document.get()));
}

TEST_F(NestingDocumentTest, LeftoversRespectParentTransformsAndTheSheetStroke)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="60" height="30" fill="none" stroke="#000" stroke-width="4"/>
      <g transform="translate(10,5) scale(2)">
        <rect id="wide" x="50" y="80" width="50" height="10"/>
      </g>
      <rect id="small" x="160" y="100" width="8" height="8"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "wide"), item(*document, "small")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements, LeftoverPlacement{.move_beside_container = true});

    ASSERT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_EQ(applied.leftover_moved_count, 1u);
    // The sheet's visual right edge is 62 (stroke 4); the part is 100 x 20 in
    // document units under its scaled parent and keeps that size.
    expect_rect_near(*item(*document, "wide")->documentVisualBounds(), rect(62, -2, 162, 18));
}

TEST_F(NestingDocumentTest, ApplyWithoutLeftoverOptionLeavesUnplacedPartsInPlace)
{
    auto document = make_document(LEFTOVERS);
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "wide1"), item(*document, "small")};
    auto const wide_transform = item(*document, "wide1")->i2doc_affine();
    auto const wide_repr = transform_attribute(item(*document, "wide1"));
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    ASSERT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_EQ(applied.unplaced_count, 1u);
    EXPECT_EQ(applied.leftover_moved_count, 0u);
    EXPECT_TRUE(affine_near(item(*document, "wide1")->i2doc_affine(), wide_transform));
    EXPECT_EQ(transform_attribute(item(*document, "wide1")), wide_repr);
}

TEST_F(NestingDocumentTest, SheetIneligibilityMatchesContainerPreparation)
{
    auto document = make_document(R"svg(
      <defs>
        <clipPath id="clip"><rect width="20" height="20"/></clipPath>
        <mask id="mask"><rect width="20" height="20" fill="#fff"/></mask>
        <filter id="blur"><feGaussianBlur stdDeviation="1"/></filter>
      </defs>
      <rect id="valid" width="60" height="30"/>
      <g id="group"><rect width="60" height="30"/></g>
      <rect id="clipped" width="60" height="30" clip-path="url(#clip)"/>
      <rect id="masked" width="60" height="30" mask="url(#mask)"/>
      <rect id="filtered" width="60" height="30" style="filter:url(#blur)"/>
      <rect id="part" x="100" y="80" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part")};

    EXPECT_FALSE(sheetIneligibility(item(*document, "valid")));
    EXPECT_TRUE(prepareDocumentNesting(item(*document, "valid"), parts));

    for (auto const *id : {"group", "clipped", "masked", "filtered"}) {
        auto const reason = sheetIneligibility(item(*document, id));
        ASSERT_TRUE(reason) << id;
        auto const prepared = prepareDocumentNesting(item(*document, id), parts);
        EXPECT_FALSE(prepared) << id;
        EXPECT_EQ(prepared.error, "invalid nesting container: " + *reason) << id;
    }
}

TEST_F(NestingDocumentTest, ParentTransformsArePreservedWhenApplyingDocumentSpacePlacement)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="50" height="50"/>
      <g id="parent" transform="translate(20,30) scale(2)">
        <rect id="part" x="60" y="40" width="5" height="5"/>
      </g>)svg");
    ASSERT_TRUE(document);
    auto *parent = item(*document, "parent");
    auto const parent_transform = parent->i2doc_affine();
    std::vector<SPItem *> parts{item(*document, "part")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    ASSERT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_TRUE(affine_near(parent->i2doc_affine(), parent_transform));
    auto bounds = item(*document, "part")->documentVisualBounds();
    ASSERT_TRUE(bounds);
    EXPECT_GE(bounds->left(), -0.001);
    EXPECT_GE(bounds->top(), -0.001);
    EXPECT_LE(bounds->right(), 50.001);
    EXPECT_LE(bounds->bottom(), 50.001);
}

TEST_F(NestingDocumentTest, StaleGeometryRejectsTheWholeResultBeforeAnyPlacementIsApplied)
{
    auto document = make_document(R"svg(
      <rect id="container" width="60" height="30"/>
      <rect id="part1" x="100" y="80" width="10" height="10"/>
      <rect id="part2" x="140" y="80" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part1"), item(*document, "part2")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    item(*document, "part2")->getRepr()->setAttribute("width", "12");
    document->ensureUpToDate();
    auto const after_external_edit = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const first_transform = item(*document, "part1")->i2doc_affine();
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    EXPECT_EQ(applied.status, ApplyStatus::StaleSnapshot);
    EXPECT_TRUE(affine_near(item(*document, "part1")->i2doc_affine(), first_transform));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after_external_edit);
}

TEST_F(NestingDocumentTest, LockingATargetWhileSolvingRejectsTheWholeResult)
{
    auto document = make_document(R"svg(
      <rect id="container" width="60" height="30"/>
      <rect id="part1" x="100" y="80" width="10" height="10"/>
      <rect id="part2" x="140" y="80" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part1"), item(*document, "part2")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    item(*document, "part2")->setLocked(true);
    document->ensureUpToDate();
    auto const after_external_edit = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const first_transform = item(*document, "part1")->i2doc_affine();
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    EXPECT_EQ(applied.status, ApplyStatus::StaleSnapshot);
    EXPECT_TRUE(affine_near(item(*document, "part1")->i2doc_affine(), first_transform));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after_external_edit);
}

TEST_F(NestingDocumentTest, InvalidOrDuplicateResultIdsCannotModifyTheDocument)
{
    auto document = make_document(R"svg(
      <rect id="container" width="60" height="30"/>
      <rect id="part1" x="100" y="80" width="10" height="10"/>
      <rect id="part2" x="140" y="80" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part1"), item(*document, "part2")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::vector<Placement> invalid{
        {.part_id = 1, .translation_x = 3, .translation_y = 4, .placed = true},
        {.part_id = 1, .translation_x = 7, .translation_y = 8, .placed = true},
    };

    auto applied = applyNestingPlacements(*prepared.snapshot, invalid);

    EXPECT_EQ(applied.status, ApplyStatus::InvalidResult);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    EXPECT_FALSE(DocumentUndo::undo(document.get()));
}

TEST_F(NestingDocumentTest, AllUnplacedResultIsANoOpAndPreservesRedo)
{
    auto document = make_document(R"svg(
      <rect id="container" width="10" height="10"/>
      <rect id="part" x="100" y="80" width="20" height="20"/>)svg");
    ASSERT_TRUE(document);
    auto *part = item(*document, "part");
    part->getRepr()->setAttribute("fill", "red");
    DocumentUndo::done(document.get(), Util::Internal::ContextString{"Prepare redo"}, "");
    ASSERT_TRUE(DocumentUndo::undo(document.get()));
    document->ensureUpToDate();
    std::vector<SPItem *> parts{part};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;
    ASSERT_EQ(solved.placements.size(), 1);
    ASSERT_FALSE(solved.placements.front().placed);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();

    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    EXPECT_EQ(applied.status, ApplyStatus::NoChange);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
    ASSERT_TRUE(DocumentUndo::redo(document.get()));
    document->ensureUpToDate();
    EXPECT_STREQ(item(*document, "part")->getRepr()->attribute("fill"), "red");
}

TEST_F(NestingDocumentTest, VersionedSyntheticCorpusPreparesAndSolvesWithoutMutation)
{
    for (auto const *fixture : {"small-rectangles.svg", "concave-mixed.svg", "compound-print-cut.svg",
                                "transparent-bitmap.svg"}) {
        auto const filename = std::string{INKSCAPE_TESTS_DIR} + "/data/nesting/" + fixture;
        auto document = std::unique_ptr<SPDocument>(SPDocument::createNewDoc(filename.c_str()));
        ASSERT_TRUE(document) << fixture;
        document->ensureUpToDate();
        document->setModifiedSinceSave(false);
        auto const before = sp_repr_save_buf(document->getReprDoc()).raw();

        auto *container = item(*document, "container");
        ASSERT_TRUE(container) << fixture;
        std::vector<SPItem *> parts;
        for (unsigned index = 1;; ++index) {
            auto const id = "part-" + std::to_string(index);
            auto *part = item(*document, id.c_str());
            if (!part) {
                break;
            }
            parts.push_back(part);
        }
        ASSERT_FALSE(parts.empty()) << fixture;

        auto prepared = prepareDocumentNesting(container, parts);
        ASSERT_TRUE(prepared) << fixture << ": " << prepared.error;
        EXPECT_EQ(prepared.snapshot->metrics.selected_count, parts.size()) << fixture;
        EXPECT_EQ(prepared.snapshot->metrics.prepared_count, parts.size()) << fixture;
        EXPECT_GT(prepared.snapshot->metrics.container_usable_area, 0.0) << fixture;
        EXPECT_GT(prepared.snapshot->metrics.selected_contour_area, 0.0) << fixture;

        auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
        ASSERT_TRUE(solved) << fixture << ": " << solved.error;
        EXPECT_EQ(solved.placements.size(), parts.size()) << fixture;
        EXPECT_GT(solved.metrics.iterations, 0) << fixture;
        EXPECT_GE(solved.metrics.utilization_percent, 0.0) << fixture;
        EXPECT_LE(solved.metrics.utilization_percent, 100.001) << fixture;
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before) << fixture;
        EXPECT_FALSE(document->isModifiedSinceSave()) << fixture;
    }
}

TEST_F(NestingDocumentTest, BitmapDiagonalCornerContactKeepsTwoSimpleComponents)
{
    // Two opaque pixels share exactly one corner. They must stay two simple
    // components; a merged bounding rectangle or a self-touching ring is not
    // an acceptable representation of this diagonal contact.
    auto const rgba = alpha_grid(2, 2, {{0, 0}, {1, 1}});
    auto const body = image_document_body(2, 2, rgba, 120, 10, 20, 20);
    ASSERT_FALSE(body.empty());
    auto document = make_document(body);
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "payload")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 1);
    auto const &part = prepared.snapshot->parts.front();
    EXPECT_EQ(part.contour_source, ContourSource::BitmapAlpha);
    ASSERT_EQ(part.components.size(), 2);
    for (auto const &component : part.components) {
        EXPECT_TRUE(component.holes.empty());
        ASSERT_EQ(component.outer.size(), 4);
        EXPECT_TRUE(ring_is_simple(component.outer));
    }
    EXPECT_NEAR(component_net_area(part.components[0]) + component_net_area(part.components[1]), 200.0, 1.0e-6);
    auto const first = point_bounds(part.components[0].outer);
    auto const second = point_bounds(part.components[1].outer);
    EXPECT_TRUE((std::abs(first.left() - 120.0) < 0.01) != (std::abs(second.left() - 120.0) < 0.01));
    auto const lower_left = (std::abs(first.left() - 120.0) < 0.01) ? first : second;
    auto const upper_right = (std::abs(first.left() - 120.0) < 0.01) ? second : first;
    expect_rect_near(lower_left, rect(120, 10, 130, 20), 0.01);
    expect_rect_near(upper_right, rect(130, 20, 140, 30), 0.01);
}

TEST_F(NestingDocumentTest, BitmapHoleTouchingOuterBoundaryNeverEmitsInvalidRing)
{
    // Three copies of a pinched ring: seven occupied pixels around one empty
    // pixel whose corner meets the outer boundary. The tracer must not emit a
    // self-touching ring or a hole sharing a vertex with its outer ring, and
    // every occupied sample cell must stay covered.
    constexpr int width = 12;
    constexpr int height = 4;
    std::vector<std::pair<int, int>> occupied;
    for (int const offset : {0, 4, 8}) {
        for (auto const &cell : {std::pair{1, 1}, {1, 0}, {2, 0}, {3, 0}, {3, 1}, {3, 2}, {2, 2}})
            occupied.emplace_back(cell.first + offset, cell.second);
    }
    auto const rgba = alpha_grid(width, height, occupied);
    auto const body = image_document_body(width, height, rgba, 120, 10, 120, 40);
    ASSERT_FALSE(body.empty());
    auto document = make_document(body);
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "payload")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 1);
    // Supervisor contract: an exact BitmapAlpha result must preserve the void;
    // a diagnosed ConservativeBounds fallback may cover a pinched void, but it
    // must still cover every occupied sample cell and report its recovery.
    auto const &part = prepared.snapshot->parts.front();
    bool const exact_alpha = part.contour_source == ContourSource::BitmapAlpha;
    if (exact_alpha) {
        EXPECT_EQ(part.recovery, RecoveryKind::Clean);
    } else {
        EXPECT_EQ(part.contour_source, ContourSource::ConservativeBounds)
            << "a non-exact result must be a diagnosed conservative fallback";
        EXPECT_EQ(part.recovery, RecoveryKind::ConservativeFallback);
        EXPECT_FALSE(part.recovery_reason.empty()) << "fallback must carry a diagnostic reason";
    }
    auto const &components = part.components;
    ASSERT_FALSE(components.empty());
    // The sampled occupancy grid is the reference, so every emitted edge must
    // stay on that integer grid and parallel to an axis unless preparation
    // proved a conservative enclosing fallback. With grid edges, cell-interior
    // coverage follows from the probes below; area alone proves neither
    // coverage nor simplicity.
    auto const on_grid = [](double value, double origin) {
        return std::abs(value - origin - std::round((value - origin) / 10.0) * 10.0) < 1.0e-6;
    };
    double net_area = 0.0;
    for (auto const &component : components) {
        std::vector<std::span<Point const>> rings{component.outer};
        for (auto const &hole : component.holes)
            rings.push_back(hole);
        for (auto const ring : rings) {
            EXPECT_TRUE(ring_is_simple(ring));
            if (exact_alpha) {
                for (std::size_t i = 0; i < ring.size(); ++i) {
                    auto const &a = ring[i];
                    auto const &b = ring[(i + 1) % ring.size()];
                    EXPECT_TRUE(on_grid(a.x, 120.0)) << "ring x leaves the sampled grid";
                    EXPECT_TRUE(on_grid(a.y, 10.0)) << "ring y leaves the sampled grid";
                    EXPECT_TRUE(std::abs(a.x - b.x) < 1.0e-6 || std::abs(a.y - b.y) < 1.0e-6)
                        << "output edge is not axis aligned";
                }
            }
        }
        if (exact_alpha)
            EXPECT_FALSE(hole_touches_outer(component));
        net_area += component_net_area(component);
    }
    for (auto const &[cell_x, cell_y] : occupied) {
        Point const center{120.0 + cell_x * 10.0 + 5.0, 10.0 + cell_y * 10.0 + 5.0};
        EXPECT_TRUE(covered_by_components(center, components)) << "uncovered cell " << cell_x << "," << cell_y;
        // One strictly-interior probe per cell corner: on-grid axis-aligned
        // edges cannot cut a cell interior, so these plus the centre prove the
        // full sampled cell is contained (exact rectangle-union containment).
        auto const cell_left = 120.0 + cell_x * 10.0;
        auto const cell_top = 10.0 + cell_y * 10.0;
        constexpr double inset = 1.0e-3;
        for (int const corner : {0, 1, 2, 3}) {
            Point const probe{(corner & 1) ? cell_left + 10.0 - inset : cell_left + inset,
                              (corner >> 1) ? cell_top + 10.0 - inset : cell_top + inset};
            EXPECT_TRUE(covered_by_components(probe, components))
                << "uncovered cell corner " << cell_x << "," << cell_y;
        }
    }
    if (exact_alpha) {
        for (int const offset : {0, 4, 8}) {
            Point const void_centre{120.0 + (2 + offset) * 10.0 + 5.0, 10.0 + 1 * 10.0 + 5.0};
            EXPECT_FALSE(covered_by_components(void_centre, components))
                << "pinched void cell " << (2 + offset) << ",1 was filled in instead of preserved";
        }
        EXPECT_NEAR(net_area, static_cast<double>(occupied.size()) * 100.0, 1.0);
    }
}

TEST_F(NestingDocumentTest, SamePixbufAlphaEditInvalidatesReusedBitmapContourCache)
{
    // Editing decoded alpha in place keeps the same pixbuf address. Prepared
    // geometry must follow the pixels instead of a pointer-keyed trace cache.
    auto const opaque = alpha_grid(2, 2, {{0, 0}, {1, 0}, {0, 1}, {1, 1}});
    auto const body = image_document_body(2, 2, opaque, 120, 10, 20, 20);
    ASSERT_FALSE(body.empty());
    auto document = make_document(body);
    ASSERT_TRUE(document);
    auto *payload = item(*document, "payload");
    auto *image = cast<SPImage>(payload);
    ASSERT_TRUE(image);
    std::vector<SPItem *> parts{payload};

    auto first = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(first) << first.error;
    ASSERT_EQ(first.snapshot->parts.size(), 1);
    ASSERT_EQ(first.snapshot->parts.front().components.size(), 1);
    expect_rect_near(point_bounds(first.snapshot->parts.front().components.front().outer), rect(120, 10, 140, 30),
                     0.01);

    ASSERT_TRUE(image->pixbuf);
    auto const *const pixbuf_address = image->pixbuf.get();
    auto &mutable_pixbuf = *const_cast<Inkscape::Pixbuf *>(image->pixbuf.get());
    set_pixbuf_alpha(mutable_pixbuf, 1, 0, 0);
    set_pixbuf_alpha(mutable_pixbuf, 0, 1, 0);
    set_pixbuf_alpha(mutable_pixbuf, 1, 1, 0);

    auto second = prepareDocumentNesting(item(*document, "container"), parts);

    EXPECT_EQ(image->pixbuf.get(), pixbuf_address) << "the alpha edit must reuse the same pixbuf address";
    ASSERT_TRUE(second) << second.error;
    ASSERT_EQ(second.snapshot->parts.size(), 1);
    auto const &components = second.snapshot->parts.front().components;
    ASSERT_EQ(components.size(), 1);
    EXPECT_EQ(second.snapshot->parts.front().contour_source, ContourSource::BitmapAlpha);
    expect_rect_near(point_bounds(components.front().outer), rect(120, 10, 130, 20), 0.01);
}

TEST_F(NestingDocumentTest, FloatCollapsedVectorContourIsNeverPreparedAsDegenerateGeometry)
{
    // The double contour is a genuine 0.001 x 20 rectangle, but the solver
    // converts coordinates to f32, where both x coordinates collapse onto one
    // value. Preparation must repair or skip it, never publish a zero-area
    // component whose vertices are only distinct as doubles.
    auto document = make_document(R"svg(
      <rect id="container" width="100" height="100"/>
      <path id="sliver" d="M 100000000,20 h 0.001 v 20 h -0.001 Z"/>
      <rect id="solid" x="10" y="10" width="5" height="5"/>)svg");
    ASSERT_TRUE(document);
    auto *sliver = item(*document, "sliver");
    std::vector<SPItem *> parts{sliver, item(*document, "solid")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    // The ordinary second part forces preparation to succeed, so a blanket
    // preparation error cannot stand in for a diagnosed rejection.
    ASSERT_TRUE(prepared) << prepared.error;

    bool published = false;
    for (auto const &part : prepared.snapshot->parts) {
        if (part.item.get() != sliver)
            continue;
        published = true;
        EXPECT_NE(part.contour_source, ContourSource::BitmapAlpha)
            << "a vector sliver must never be reported as a bitmap-alpha contour";
        for (auto const &component : part.components) {
            std::vector<Point> rounded;
            for (auto const &point : component.outer)
                rounded.push_back({static_cast<float>(point.x), static_cast<float>(point.y)});
            EXPECT_GT(ring_area(rounded), 0.0) << "outer collapses under the solver f32 conversion";
            EXPECT_TRUE(ring_is_simple(rounded))
                << "outer repeats a vertex or self-intersects after the solver f32 conversion";
            double min_x = std::numeric_limits<double>::infinity();
            double max_x = -std::numeric_limits<double>::infinity();
            for (auto const &point : rounded) {
                EXPECT_TRUE(std::isfinite(point.x) && std::isfinite(point.y))
                    << "outer leaves the finite float range";
                min_x = std::min(min_x, point.x);
                max_x = std::max(max_x, point.x);
            }
            // One f32 ulp at 1e8 is 8.0, so more than two ulps of width is arbitrary
            // inflation rather than a repair of the collapsed 0.001-wide sliver.
            EXPECT_LE(max_x - min_x, 16.0) << "sliver inflated far beyond its f32 representable width";
            for (auto const &hole : component.holes) {
                std::vector<Point> rounded_hole;
                for (auto const &point : hole)
                    rounded_hole.push_back({static_cast<float>(point.x), static_cast<float>(point.y)});
                EXPECT_GT(ring_area(rounded_hole), 0.0) << "hole collapses under the solver f32 conversion";
                EXPECT_TRUE(ring_is_simple(rounded_hole))
                    << "hole repeats a vertex or self-intersects after the solver f32 conversion";
                for (auto const &point : rounded_hole) {
                    EXPECT_TRUE(std::isfinite(point.x) && std::isfinite(point.y))
                        << "hole leaves the finite float range";
                }
            }
        }
    }
    if (!published) {
        bool skipped = false;
        for (auto const &skipped_part : prepared.snapshot->skipped_parts) {
            if (skipped_part.item.get() == sliver) {
                skipped = true;
                EXPECT_FALSE(skipped_part.detail.empty()) << "skip must carry an actionable reason";
            }
        }
        EXPECT_TRUE(skipped) << "sliver was neither published as valid geometry nor skipped with a reason";
    }
}

TEST_F(NestingDocumentTest, VisibleStrokedGroupChildFootprintIsNeverSilentlyDropped)
{
    // "inked" is a valid stroked line whose ink extends well beyond the rect
    // child, so it has real occupied area. A rigid group may not keep only the
    // rect (or otherwise move away from where the stroke sits): the visible
    // child footprint must be covered by the prepared components, or the whole
    // group must be skipped with a reason.
    auto document = make_document(R"svg(
      <rect id="container" width="100" height="100"/>
      <g id="part" transform="translate(120,30)">
        <rect id="good" width="10" height="10"/>
        <path id="inked" d="M 40,5 L 90,5" fill="none" stroke="red" stroke-width="8"/>
      </g>)svg");
    ASSERT_TRUE(document);
    auto *part = item(*document, "part");
    auto group_bounds = part->documentVisualBounds();
    ASSERT_TRUE(group_bounds);
    ASSERT_GT(group_bounds->width(), 80.0);
    std::vector<SPItem *> parts{part};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    bool prepared_group = false;
    for (auto const &prepared_part : prepared.snapshot->parts) {
        if (prepared_part.item.get() != part)
            continue;
        prepared_group = true;
        // Rect interior plus stroke body, stroke ends and stroke band edges; a
        // silently dropped stroked child leaves all stroke probes uncovered.
        std::vector<Point> probes{
            {group_bounds->left() + 5.0, group_bounds->top() + 5.0},
            {group_bounds->left() + 45.0, group_bounds->top() + 5.0},
            {group_bounds->left() + 65.0, group_bounds->top() + 5.0},
            {group_bounds->right() - 5.0, group_bounds->top() + 5.0},
            {group_bounds->left() + 65.0, group_bounds->top() + 1.5},
            {group_bounds->left() + 65.0, group_bounds->top() + 8.5},
        };
        for (auto const &probe : probes)
            EXPECT_TRUE(covered_by_components(probe, prepared_part.components))
                << "visible stroked child footprint was dropped from the rigid group";
    }
    ASSERT_TRUE(prepared_group) << "valid visible stroked group child was silently dropped";
}

TEST_F(NestingDocumentTest, DescendantStyleEditPreservingGeometryInvalidatesSnapshot)
{
    auto document = make_document(R"svg(
      <rect id="container" width="100" height="100"/>
      <g id="part" transform="translate(140,20)">
        <rect id="child-a" width="10" height="10"/>
        <rect id="child-b" x="20" width="10" height="10"/>
      </g>)svg");
    ASSERT_TRUE(document);
    auto *part = item(*document, "part");
    std::vector<SPItem *> parts{part};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;
    auto const before_transform = part->i2doc_affine();

    item(*document, "child-a")->getRepr()->setAttribute("fill", "blue");
    document->ensureUpToDate();
    auto const after_edit = sp_repr_save_buf(document->getReprDoc()).raw();

    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    EXPECT_EQ(applied.status, ApplyStatus::StaleSnapshot);
    EXPECT_TRUE(affine_near(part->i2doc_affine(), before_transform));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after_edit);
}

// ---------------------------------------------------------------------------
// R2 (consolidated nesting work order, M5): apply-time freshness skips
// re-preparing geometry while the document's XML is unchanged, and keeps the
// checks XML cannot see.

namespace {

char const *const TWO_PARTS = R"svg(
      <rect id="container" width="60" height="30"/>
      <rect id="part1" x="100" y="80" width="10" height="10"/>
      <rect id="part2" x="140" y="80" width="10" height="10"/>
      <rect id="other" x="200" y="200" width="5" height="5"/>)svg";

} // namespace

TEST_F(NestingDocumentTest, UnchangedDocumentSkipsGeometryRebuildAtApply)
{
    auto document = make_document(TWO_PARTS);
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part1"), item(*document, "part2")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_TRUE(prepared.snapshot->revision);
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    EXPECT_FALSE(prepared.snapshot->revision->changed()) << "preparing or solving must not write XML";
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    EXPECT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_EQ(applied.revalidated_count, 0u);
}

TEST_F(NestingDocumentTest, UnrelatedEditFallsBackToFullCheck)
{
    auto document = make_document(TWO_PARTS);
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part1"), item(*document, "part2")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    item(*document, "other")->getRepr()->setAttribute("fill", "blue");
    document->ensureUpToDate();
    ASSERT_TRUE(prepared.snapshot->revision->changed());
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    EXPECT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_EQ(applied.revalidated_count, parts.size() + 1);
}

TEST_F(NestingDocumentTest, NamedviewEditDoesNotForceFullCheck)
{
    auto document = make_document(TWO_PARTS);
    ASSERT_TRUE(document);
    ASSERT_TRUE(document->getNamedView());
    std::vector<SPItem *> parts{item(*document, "part1"), item(*document, "part2")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    document->getNamedView()->getRepr()->setAttribute("inkscape:zoom", "2.5");
    document->ensureUpToDate();
    EXPECT_FALSE(prepared.snapshot->revision->changed());
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    EXPECT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_EQ(applied.revalidated_count, 0u);
}

TEST_F(NestingDocumentTest, TransformChangeWithoutXmlIsStale)
{
    auto document = make_document(TWO_PARTS);
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part1"), item(*document, "part2")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    auto *part2 = item(*document, "part2");
    part2->transform = Geom::Translate(5, 0) * part2->transform; // no repr write
    ASSERT_FALSE(prepared.snapshot->revision->changed());
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const first_transform = item(*document, "part1")->i2doc_affine();
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    EXPECT_EQ(applied.status, ApplyStatus::StaleSnapshot);
    EXPECT_TRUE(affine_near(item(*document, "part1")->i2doc_affine(), first_transform));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
}

TEST_F(NestingDocumentTest, InPlaceAlphaEditWithoutXmlChangeIsStale)
{
    auto const opaque = alpha_grid(2, 2, {{0, 0}, {1, 0}, {0, 1}, {1, 1}});
    auto const body = image_document_body(2, 2, opaque, 120, 10, 20, 20);
    ASSERT_FALSE(body.empty());
    auto document = make_document(body);
    ASSERT_TRUE(document);
    auto *payload = item(*document, "payload");
    auto *image = cast<SPImage>(payload);
    ASSERT_TRUE(image);
    ASSERT_TRUE(image->pixbuf);
    std::vector<SPItem *> parts{payload};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 1u);
    EXPECT_EQ(prepared.snapshot->parts.front().image_alpha_hashes.size(), 1u);
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    auto const *const pixbuf_address = image->pixbuf.get();
    auto &mutable_pixbuf = *const_cast<Inkscape::Pixbuf *>(image->pixbuf.get());
    set_pixbuf_alpha(mutable_pixbuf, 1, 1, 0);
    ASSERT_EQ(image->pixbuf.get(), pixbuf_address) << "the alpha edit must reuse the same pixbuf address";
    ASSERT_FALSE(prepared.snapshot->revision->changed());
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    EXPECT_EQ(applied.status, ApplyStatus::StaleSnapshot);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
}

TEST_F(NestingDocumentTest, TextPartIsAlwaysRevalidated)
{
    // Text layout depends on installed fonts, which no XML observer sees.
    auto document = make_document(R"svg(
      <rect id="container" width="120" height="60"/>
      <g id="label">
        <rect x="150" y="80" width="30" height="20"/>
        <text x="152" y="95" font-size="12" font-family="sans-serif">A</text>
      </g>
      <rect id="plain" x="200" y="80" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "label"), item(*document, "plain")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 2u);
    EXPECT_TRUE(prepared.snapshot->parts[0].always_revalidate);
    EXPECT_FALSE(prepared.snapshot->parts[1].always_revalidate);
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    ASSERT_FALSE(prepared.snapshot->revision->changed());
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

    EXPECT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_EQ(applied.revalidated_count, 1u) << "only the text part takes the full path";
}

TEST_F(NestingDocumentTest, NamedviewContentOtherThanGuidesPagesAndGridsStillCounts)
{
    // Elements under the namedview are real objects: a part may reference one.
    auto document = make_document(R"svg(
      <sodipodi:namedview xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" id="nv">
        <rect id="shape" x="0" y="0" width="10" height="10"/>
      </sodipodi:namedview>
      <rect id="container" width="60" height="30"/>
      <use id="part1" href="#shape" x="100" y="80"/>
      <rect id="part2" x="140" y="80" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    auto *namedview = document->getObjectById("nv");
    ASSERT_TRUE(namedview);
    std::vector<SPItem *> parts{item(*document, "part1"), item(*document, "part2")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 2u);
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    // View state and guides are ignored...
    namedview->getRepr()->setAttribute("inkscape:cx", "42");
    auto *guide = document->getReprDoc()->createElement("sodipodi:guide");
    guide->setAttribute("position", "10,10");
    guide->setAttribute("orientation", "0,1");
    namedview->getRepr()->appendChild(guide);
    Inkscape::GC::release(guide);
    document->ensureUpToDate();
    EXPECT_FALSE(prepared.snapshot->revision->changed());

    // ...but the referenced shape under the namedview is content.
    document->getObjectById("shape")->getRepr()->setAttribute("width", "12");
    document->ensureUpToDate();
    EXPECT_TRUE(prepared.snapshot->revision->changed());
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);
    EXPECT_EQ(applied.status, ApplyStatus::StaleSnapshot);
}

TEST_F(NestingDocumentTest, NamedviewGuideRemovalGridEditsAndRenamesAreJudgedByKind)
{
    auto document = make_document(R"svg(
      <sodipodi:namedview xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
          xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" id="nv">
        <sodipodi:guide id="g1" position="10,10" orientation="0,1"/>
        <sodipodi:guide id="g2" position="20,20" orientation="1,0"/>
        <inkscape:grid id="grid" type="xygrid"/>
        <rect id="shape" x="0" y="0" width="10" height="10"/>
      </sodipodi:namedview>
      <rect id="container" width="60" height="30"/>
      <rect id="part1" x="100" y="80" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    auto *nv = document->getObjectById("nv")->getRepr();
    auto *g1 = document->getObjectById("g1")->getRepr();
    auto *g2 = document->getObjectById("g2")->getRepr();
    std::vector<SPItem *> parts{item(*document, "part1")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto const &revision = *prepared.snapshot->revision;

    nv->changeOrder(g2, nullptr); // reorder guides
    document->getObjectById("grid")->getRepr()->setAttribute("spacingx", "5");
    nv->removeChild(g1); // detached before the notification
    document->ensureUpToDate();
    EXPECT_FALSE(revision.changed());

    // A real element renamed into an ignored kind was content before.
    document->getObjectById("shape")->getRepr()->setCodeUnsafe(g_quark_from_string("sodipodi:guide"));
    EXPECT_TRUE(revision.changed());
}

TEST_F(NestingDocumentTest, StylesheetUnderNamedviewCountsAsAChange)
{
    auto document = make_document(R"svg(
      <sodipodi:namedview xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd" id="nv">
        <style id="sheet-style">.cut { stroke: none; }</style>
      </sodipodi:namedview>
      <rect id="container" width="60" height="30"/>
      <rect id="part1" class="cut" x="100" y="80" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part1")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;

    auto *style = document->getObjectById("sheet-style");
    ASSERT_TRUE(style && style->getRepr()->firstChild());
    style->getRepr()->firstChild()->setContent(".cut { stroke: #000; stroke-width: 4; }");
    EXPECT_TRUE(prepared.snapshot->revision->changed());
}

TEST_F(NestingDocumentTest, ExternalReferencesAndTextThroughUseAlwaysRevalidate)
{
    auto document = make_document(R"svg(
      <defs><text id="label" x="0" y="10" font-size="10" font-family="sans-serif">A</text></defs>
      <rect id="container" width="120" height="60" href="sheet-texture.png"/>
      <rect id="linked" x="100" y="80" width="10" height="10" href="other.svg#frame"/>
      <g id="cloned-text"><rect x="140" y="80" width="20" height="20"/><use href="#label" x="142" y="82"/></g>
      <rect id="plain" x="180" y="80" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "linked"), item(*document, "cloned-text"), item(*document, "plain")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 3u);
    EXPECT_TRUE(prepared.snapshot->container_always_revalidate);
    EXPECT_TRUE(prepared.snapshot->parts[0].always_revalidate);
    EXPECT_TRUE(prepared.snapshot->parts[1].always_revalidate);
    EXPECT_FALSE(prepared.snapshot->parts[2].always_revalidate);
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    ASSERT_FALSE(prepared.snapshot->revision->changed());
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);
    EXPECT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_EQ(applied.revalidated_count, 3u) << "container, linked and cloned-text take the full path";
}

// ---------------------------------------------------------------------------
// R4 (M6): cheaper fingerprints must stay as sensitive as the byte-wise ones.

namespace {

/// Prepare, solve, force the full apply-time check with an unrelated edit, run
/// `edit`, and return the apply status.
ApplyStatus apply_after_edit(SPDocument &document, std::vector<SPItem *> const &parts,
                             std::function<void()> const &edit)
{
    auto prepared = prepareDocumentNesting(item(document, "container"), parts);
    EXPECT_TRUE(prepared) << prepared.error;
    if (!prepared)
        return ApplyStatus::InvalidResult;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    EXPECT_TRUE(solved) << solved.error;
    if (!solved)
        return ApplyStatus::InvalidResult;
    item(document, "container")->getRepr()->setAttribute("data-unrelated", "1");
    edit();
    document.ensureUpToDate();
    EXPECT_TRUE(prepared.snapshot->revision->changed());
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);
    item(document, "container")->removeAttribute("data-unrelated");
    return applied.status;
}

} // namespace

TEST_F(NestingDocumentTest, FingerprintDetectsOneAlphaChangeInLargeBitmap)
{
    std::vector<std::uint8_t> rgba(2000u * 2000u * 4u, 0);
    for (std::size_t i = 3; i < rgba.size(); i += 4)
        rgba[i] = 255;
    auto const body = image_document_body(2000, 2000, rgba, 120, 10, 20, 20);
    ASSERT_FALSE(body.empty());
    auto document = make_document(body);
    ASSERT_TRUE(document);
    auto *image = cast<SPImage>(item(*document, "payload"));
    ASSERT_TRUE(image && image->pixbuf);
    std::vector<SPItem *> parts{image};

    auto const status = apply_after_edit(*document, parts, [&] {
        // One pixel, in place: same pixbuf address, no XML change of its own.
        set_pixbuf_alpha(*const_cast<Inkscape::Pixbuf *>(image->pixbuf.get()), 1234, 987, 254);
    });

    EXPECT_EQ(status, ApplyStatus::StaleSnapshot);
}

TEST_F(NestingDocumentTest, FingerprintDetectsOneCharacterChangeInLongDataUri)
{
    std::string const prefix = "data:text/plain;base64,";
    std::string payload(1u << 20, 'A');
    for (std::size_t i = 0; i < payload.size(); i += 7)
        payload[i] = 'B';
    auto const href = prefix + payload;
    // Offsets into the attribute value: word-aligned, unaligned, and in the tail.
    for (std::size_t const offset : {std::size_t{800'000}, std::size_t{800'003}, href.size() - 1}) {
        auto document = make_document("<rect id=\"container\" width=\"60\" height=\"30\"/>"
                                      "<rect id=\"part\" x=\"100\" y=\"80\" width=\"10\" height=\"10\" href=\"" +
                                      href + "\"/>");
        ASSERT_TRUE(document);
        std::vector<SPItem *> parts{item(*document, "part")};
        auto const status = apply_after_edit(*document, parts, [&] {
            auto changed = href;
            changed[offset] = changed[offset] == 'Z' ? 'Y' : 'Z';
            item(*document, "part")->setAttribute("href", changed);
        });
        EXPECT_EQ(status, ApplyStatus::StaleSnapshot) << "offset " << offset;
    }
}

TEST_F(NestingDocumentTest, FingerprintDetectsEditsThatCancelledInTheSingleMultiplyMix)
{
    // One 16-pixel row = two 8-byte words of alpha. Flipping bit 7 of bytes 7
    // and 15 plus bit 2 of byte 12 cancelled out in the first R4 mix.
    std::vector<std::uint8_t> rgba(16u * 4u, 0);
    for (std::size_t i = 3; i < rgba.size(); i += 4)
        rgba[i] = 0x7f;
    auto const body = image_document_body(16, 1, rgba, 120, 10, 16, 1);
    ASSERT_FALSE(body.empty());
    auto document = make_document(body);
    ASSERT_TRUE(document);
    auto *image = cast<SPImage>(item(*document, "payload"));
    ASSERT_TRUE(image && image->pixbuf);
    std::vector<SPItem *> parts{image};

    auto const status = apply_after_edit(*document, parts, [&] {
        auto &pixels = *const_cast<Inkscape::Pixbuf *>(image->pixbuf.get());
        set_pixbuf_alpha(pixels, 7, 0, 0xff);
        set_pixbuf_alpha(pixels, 15, 0, 0xff);
        set_pixbuf_alpha(pixels, 12, 0, 0x7f ^ 0x04);
    });

    EXPECT_EQ(status, ApplyStatus::StaleSnapshot);
}

TEST_F(NestingDocumentTest, FingerprintIsStableAcrossCalls)
{
    std::string const href = "data:text/plain;base64," + std::string(100'003, 'Q');
    auto const opaque = alpha_grid(2, 2, {{0, 0}, {1, 0}, {0, 1}, {1, 1}});
    auto const image = image_document_body(2, 2, opaque, 120, 10, 20, 20);
    ASSERT_FALSE(image.empty());
    auto document =
        make_document(image + "<rect id=\"long\" x=\"100\" y=\"80\" width=\"10\" height=\"10\" href=\"" + href + "\"/>");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "payload"), item(*document, "long")};

    auto first = prepareDocumentNesting(item(*document, "container"), parts);
    auto second = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(first && second);
    ASSERT_EQ(first.snapshot->parts.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(first.snapshot->parts[i].content_fingerprint, second.snapshot->parts[i].content_fingerprint);
    }
    // An edit that is reverted leaves the fingerprints valid on the full path.
    auto const status = apply_after_edit(*document, parts, [&] {
        item(*document, "long")->setAttribute("href", "data:text/plain;base64,QQ");
        item(*document, "long")->setAttribute("href", href);
    });
    EXPECT_EQ(status, ApplyStatus::Applied);
}

// ---------------------------------------------------------------------------
// Add to sheet (M13, add-to-sheet work order 10.1): obstacles on the sheet.

namespace {

std::vector<std::string> ids_of(std::vector<SPItem *> const &items)
{
    std::vector<std::string> ids;
    for (auto *item : items)
        ids.emplace_back(item && item->getId() ? item->getId() : "");
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<Point> rect_ring(Geom::Rect const &r)
{
    return {{r.left(), r.top()}, {r.right(), r.top()}, {r.right(), r.bottom()}, {r.left(), r.bottom()}};
}

} // namespace

TEST_F(NestingDocumentTest, CollectsObjectsOnTheSheetAsObstacles)
{
    auto document = make_document(R"svg(
      <rect id="sheet" width="100" height="100" fill="none" stroke="none"/>
      <rect id="a" x="10" y="10" width="10" height="10"/>
      <rect id="b" x="40" y="10" width="10" height="10"/>
      <rect id="c" x="70" y="10" width="10" height="10"/>
      <rect id="off" x="200" y="200" width="10" height="10"/>
      <rect id="part" x="300" y="10" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part")};
    auto const obstacles = collectSheetObstacles(item(*document, "sheet"), 0, parts);
    EXPECT_EQ(ids_of(obstacles.items), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_TRUE(obstacles.ignored_backgrounds.empty());
}

TEST_F(NestingDocumentTest, LockedObjectsAreObstaclesHiddenAreNot)
{
    auto document = make_document(R"svg(
      <rect id="sheet" width="100" height="100" fill="none"/>
      <rect id="locked" xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
            sodipodi:insensitive="true" x="10" y="10" width="5" height="5"/>
      <rect id="hidden" x="30" y="10" width="5" height="5" style="display:none"/>)svg");
    ASSERT_TRUE(document);
    ASSERT_TRUE(item(*document, "locked")->isLocked());
    auto const obstacles = collectSheetObstacles(item(*document, "sheet"), 0, {});
    EXPECT_EQ(ids_of(obstacles.items), (std::vector<std::string>{"locked"}));
}

TEST_F(NestingDocumentTest, GroupHoldingTheSheetIsEntered)
{
    auto document = make_document(R"svg(
      <g id="group"><rect id="sheet" width="100" height="100" fill="none"/>
        <rect id="mark" x="5" y="5" width="4" height="4"/></g>)svg");
    ASSERT_TRUE(document);
    auto const obstacles = collectSheetObstacles(item(*document, "sheet"), 0, {});
    EXPECT_EQ(ids_of(obstacles.items), (std::vector<std::string>{"mark"}));
}

TEST_F(NestingDocumentTest, GroupHoldingAPartIsEntered)
{
    auto document = make_document(R"svg(
      <rect id="sheet" width="100" height="100" fill="none"/>
      <g id="group"><rect id="part" x="20" y="20" width="10" height="10"/>
        <rect id="label" x="50" y="50" width="10" height="4"/></g>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part")};
    auto const obstacles = collectSheetObstacles(item(*document, "sheet"), 0, parts);
    EXPECT_EQ(ids_of(obstacles.items), (std::vector<std::string>{"label"}));
}

TEST_F(NestingDocumentTest, WholeSheetBackgroundIsIgnoredAndReported)
{
    auto document = make_document(R"svg(
      <rect id="photo" x="-10" y="-10" width="120" height="120" fill="#888"/>
      <rect id="sheet" width="100" height="100" fill="none"/>
      <rect id="mark" x="5" y="5" width="4" height="4"/>)svg");
    ASSERT_TRUE(document);
    auto const obstacles = collectSheetObstacles(item(*document, "sheet"), 0, {});
    EXPECT_EQ(ids_of(obstacles.items), (std::vector<std::string>{"mark"}));
    EXPECT_EQ(ids_of(obstacles.ignored_backgrounds), (std::vector<std::string>{"photo"}));
}

TEST_F(NestingDocumentTest, PartsAreNeverObstacles)
{
    auto document = make_document(R"svg(
      <rect id="sheet" width="100" height="100" fill="none"/>
      <rect id="p1" x="10" y="10" width="10" height="10"/>
      <rect id="p2" x="30" y="10" width="10" height="10"/>
      <rect id="mark" x="70" y="70" width="4" height="4"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "p1"), item(*document, "p2")};
    auto const obstacles = collectSheetObstacles(item(*document, "sheet"), 0, parts);
    EXPECT_EQ(ids_of(obstacles.items), (std::vector<std::string>{"mark"}));
}

TEST_F(NestingDocumentTest, PreparationMeasuresObstaclesLikeParts)
{
    auto const uri = png_data_uri(2, 2, alpha_grid(2, 2, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}));
    ASSERT_FALSE(uri.empty());
    auto document = make_document(R"svg(<rect id="sheet" width="100" height="100" fill="none"/>
      <g id="obstacle"><rect x="10" y="10" width="10" height="10"/>
        <image x="5" y="5" width="40" height="40" href=")svg" + uri + R"svg("/></g>
      <rect id="part" x="200" y="10" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part")};
    std::vector<SPItem *> obstacles{item(*document, "obstacle")};
    auto prepared = prepareDocumentNesting(item(*document, "sheet"), parts, 0.05, obstacles);
    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->obstacles.size(), 1u);
    EXPECT_EQ(prepared.snapshot->metrics.obstacle_count, 1u);
    // S10: bitmaps in a vector group do not collide; only the rect's outline counts.
    Geom::OptRect bounds;
    for (auto const &component : prepared.snapshot->obstacles.front().components)
        bounds.unionWith(point_bounds(component.outer));
    ASSERT_TRUE(bounds);
    expect_rect_near(*bounds, rect(10, 10, 20, 20), 0.01);
}

TEST_F(NestingDocumentTest, UnmeasurableObstacleStopsPreparation)
{
    auto document = make_document(R"svg(
      <rect id="sheet" width="100" height="100" fill="none"/>
      <path id="empty" d=""/>
      <rect id="part" x="200" y="10" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part")};
    std::vector<SPItem *> obstacles{item(*document, "empty")};
    auto prepared = prepareDocumentNesting(item(*document, "sheet"), parts, 0.05, obstacles);
    ASSERT_FALSE(prepared);
    EXPECT_EQ(prepared.error.rfind("an object on the sheet cannot be measured, so parts could overlap it: ", 0), 0u)
        << prepared.error;
    EXPECT_TRUE(prepared.error.ends_with("Move it off the sheet or hide it.")) << prepared.error;
}

TEST_F(NestingDocumentTest, AddedPartsKeepSpacingFromObstacles)
{
    auto document = make_document(R"svg(
      <rect id="sheet" width="100" height="60" fill="none"/>
      <rect id="obstacle" x="0" y="0" width="40" height="40"/>
      <rect id="p1" x="200" y="10" width="20" height="20"/>
      <rect id="p2" x="240" y="10" width="20" height="20"/>)svg");
    ASSERT_TRUE(document);
    auto *obstacle = item(*document, "obstacle");
    auto const obstacle_transform = obstacle->i2doc_affine();
    std::vector<SPItem *> parts{item(*document, "p1"), item(*document, "p2")};
    auto const found = collectSheetObstacles(item(*document, "sheet"), 0, parts);
    ASSERT_EQ(ids_of(found.items), (std::vector<std::string>{"obstacle"}));
    auto prepared = prepareDocumentNesting(item(*document, "sheet"), parts, 0.05, found.items);
    ASSERT_TRUE(prepared) << prepared.error;
    auto options = draft_options();
    options.part_spacing = 5.0;
    auto solved = solvePreparedNesting(*prepared.snapshot, options);
    ASSERT_TRUE(solved) << solved.error;
    EXPECT_EQ(solved.backend, "native");
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);
    ASSERT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_EQ(applied.placed_count, 2u);

    // Independent oracle: ring distance between the placed rectangles and the obstacle.
    auto const obstacle_ring = rect_ring(*obstacle->documentVisualBounds());
    for (auto const *id : {"p1", "p2"}) {
        auto const ring = rect_ring(*item(*document, id)->documentVisualBounds());
        EXPECT_GE(Inkscape::Nesting::Test::ring_distance(ring, obstacle_ring, 1e-9), 5.0 - 1e-3) << id;
    }
    EXPECT_TRUE(affine_near(obstacle->i2doc_affine(), obstacle_transform));
}

TEST_F(NestingDocumentTest, MovedObstacleMakesResultStale)
{
    auto document = make_document(R"svg(
      <rect id="sheet" width="100" height="60" fill="none"/>
      <rect id="obstacle" x="0" y="0" width="40" height="40"/>
      <rect id="p1" x="200" y="10" width="20" height="20"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "p1")};
    std::vector<SPItem *> obstacles{item(*document, "obstacle")};
    auto prepared = prepareDocumentNesting(item(*document, "sheet"), parts, 0.05, obstacles);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    item(*document, "obstacle")->getRepr()->setAttribute("transform", "translate(50,0)");
    document->ensureUpToDate();
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);
    EXPECT_EQ(applied.status, ApplyStatus::StaleSnapshot);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);
}

namespace {

/// Prepare one part with one obstacle, solve, run edit, apply.
ApplyStatus apply_after_obstacle_edit(SPDocument &document, std::function<void()> const &edit)
{
    std::vector<SPItem *> parts{item(document, "p1")};
    std::vector<SPItem *> obstacles{item(document, "obstacle")};
    auto prepared = prepareDocumentNesting(item(document, "sheet"), parts, 0.05, obstacles);
    EXPECT_TRUE(prepared) << prepared.error;
    if (!prepared)
        return ApplyStatus::InvalidResult;
    EXPECT_EQ(prepared.snapshot->obstacles.size(), 1u);
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    EXPECT_TRUE(solved) << solved.error;
    if (!solved)
        return ApplyStatus::InvalidResult;
    edit();
    document.ensureUpToDate();
    return applyNestingPlacements(*prepared.snapshot, solved.placements).status;
}

char const *const OBSTACLE_SHEET = R"svg(
      <rect id="sheet" width="100" height="60" fill="none"/>
      <g id="layer" style="stroke:#000;stroke-width:1">
        <rect id="obstacle" x="5" y="5" width="20" height="20"/>
      </g>
      <rect id="p1" x="200" y="10" width="10" height="10"/>)svg";

} // namespace

TEST_F(NestingDocumentTest, ObstacleContentEditMakesResultStale)
{
    auto document = make_document(OBSTACLE_SHEET);
    ASSERT_TRUE(document);
    EXPECT_EQ(apply_after_obstacle_edit(*document, [&] { item(*document, "obstacle")->setAttribute("width", "30"); }),
              ApplyStatus::StaleSnapshot);
}

TEST_F(NestingDocumentTest, ObstacleInheritedStrokeChangeMakesResultStale)
{
    // The obstacle's own XML does not change; its outline grows with the
    // layer's stroke width.
    auto document = make_document(OBSTACLE_SHEET);
    ASSERT_TRUE(document);
    EXPECT_EQ(apply_after_obstacle_edit(
                  *document, [&] { item(*document, "layer")->setAttribute("style", "stroke:#000;stroke-width:12"); }),
              ApplyStatus::StaleSnapshot);
}

TEST_F(NestingDocumentTest, HiddenObstacleMakesResultStale)
{
    auto document = make_document(OBSTACLE_SHEET);
    ASSERT_TRUE(document);
    EXPECT_EQ(apply_after_obstacle_edit(*document, [&] { item(*document, "obstacle")->setAttribute("style", "display:none"); }),
              ApplyStatus::StaleSnapshot);
}

TEST_F(NestingDocumentTest, LockedObstacleStillApplies)
{
    auto document = make_document(OBSTACLE_SHEET);
    ASSERT_TRUE(document);
    item(*document, "obstacle")->setLocked(true);
    document->ensureUpToDate();
    EXPECT_EQ(apply_after_obstacle_edit(*document, [] {}), ApplyStatus::Applied);
}

TEST_F(NestingDocumentTest, BitmapObstaclePixelEditMakesResultStale)
{
    auto const uri = png_data_uri(2, 2, alpha_grid(2, 2, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}));
    ASSERT_FALSE(uri.empty());
    auto document = make_document(R"svg(<rect id="sheet" width="100" height="60" fill="none"/>
      <image id="obstacle" x="5" y="5" width="20" height="20" href=")svg" + uri + R"svg("/>
      <rect id="p1" x="200" y="10" width="10" height="10"/>)svg");
    ASSERT_TRUE(document);
    auto *image = cast<SPImage>(item(*document, "obstacle"));
    ASSERT_TRUE(image && image->pixbuf);
    EXPECT_EQ(apply_after_obstacle_edit(*document,
                                        [&] {
                                            set_pixbuf_alpha(*const_cast<Inkscape::Pixbuf *>(image->pixbuf.get()), 1,
                                                             1, 0);
                                        }),
              ApplyStatus::StaleSnapshot);
}

TEST_F(NestingDocumentTest, CloneOfAPartIsNotAnObstacle)
{
    auto document = make_document(R"svg(
      <rect id="sheet" width="100" height="100" fill="none"/>
      <rect id="p1" x="200" y="10" width="10" height="10"/>
      <use id="clone" href="#p1" x="-180" y="10"/>
      <g id="holder"><use id="nested-clone" href="#clone" x="0" y="20"/></g>
      <rect id="mark" x="70" y="70" width="4" height="4"/>)svg");
    ASSERT_TRUE(document);
    ASSERT_TRUE(item(*document, "clone")->documentVisualBounds()->intersects(*item(*document, "sheet")->documentVisualBounds()));
    std::vector<SPItem *> parts{item(*document, "p1")};
    auto const obstacles = collectSheetObstacles(item(*document, "sheet"), 0, parts);
    // The clones move with the part; only the mark stays where it is.
    EXPECT_EQ(ids_of(obstacles.items), (std::vector<std::string>{"mark"}));
}

TEST_F(NestingDocumentTest, ObstacleSharingAGradientWithAPartStaysAnObstacle)
{
    // Paint servers are shared resources, not followers.
    auto document = make_document(R"svg(
      <rect id="sheet" width="100" height="100" fill="none"/>
      <g id="part"><linearGradient id="grad"><stop offset="0" stop-color="#f00"/></linearGradient>
        <rect x="200" y="10" width="10" height="10" fill="url(#grad)"/></g>
      <rect id="mark" x="20" y="20" width="10" height="10" fill="url(#grad)"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "part")};
    auto const obstacles = collectSheetObstacles(item(*document, "sheet"), 0, parts);
    EXPECT_EQ(ids_of(obstacles.items), (std::vector<std::string>{"mark"}));
}

TEST_F(NestingDocumentTest, GroupContainingACloneKeepsItsOtherMembers)
{
    auto document = make_document(R"svg(
      <rect id="sheet" width="100" height="100" fill="none"/>
      <rect id="p1" x="200" y="10" width="10" height="10"/>
      <g id="holder"><use id="clone" href="#p1" x="-180" y="10"/>
        <rect id="member" x="60" y="60" width="5" height="5"/></g>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "p1")};
    auto const obstacles = collectSheetObstacles(item(*document, "sheet"), 0, parts);
    EXPECT_EQ(ids_of(obstacles.items), (std::vector<std::string>{"member"}));
}

TEST_F(NestingDocumentTest, LeftoverStartXBelowTheSheetEdgeIsIgnored)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="60" height="30"/>
      <rect id="wide" x="100" y="150" width="100" height="20"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "wide")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements,
                                          LeftoverPlacement{.move_beside_container = true, .gap = 5.0, .start_x = 10.0});
    ASSERT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_NEAR(item(*document, "wide")->documentVisualBounds()->left(), 65.0, 1e-6);
}

TEST_F(NestingDocumentTest, LeftoversStartAfterEarlierStagedLeftovers)
{
    auto document = make_document(R"svg(
      <rect id="container" x="0" y="0" width="60" height="30"/>
      <rect id="wide" x="100" y="150" width="100" height="20"/>)svg");
    ASSERT_TRUE(document);
    std::vector<SPItem *> parts{item(*document, "wide")};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;
    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements,
                                          LeftoverPlacement{.move_beside_container = true, .gap = 5.0, .start_x = 300.0});
    ASSERT_EQ(applied.status, ApplyStatus::Applied) << applied.error;
    EXPECT_NEAR(item(*document, "wide")->documentVisualBounds()->left(), 300.0, 1e-6);
}

// ---------------------------------------------------------------------------
// P3/P4 fidelity regressions, reviewed from
// evidence/P4-regression-draft/staged/regression-additions.cpp. Two staged
// oracles were corrected against the supervisor contract:
//  * the bitmap fixture is non-gray and semi-transparent so the raw GdkPixbuf
//    and premultiplied Cairo representations actually differ (an all-black
//    opaque grid hashes identically in both and cannot catch the defect);
//  * the "invalid container hole" bowtie is normalized by the existing
//    fill-rule flattening into two valid triangular holes, so it never reached
//    the fail-closed path; a nested-island hole configuration (depth 2) that
//    contours_from_path genuinely rejects is used instead.
// ---------------------------------------------------------------------------

namespace {

/** Document-space outer/hole rings of a prepared part, flattened for comparison. */
std::vector<std::vector<Point>> part_rings(PreparedPart const &part)
{
    std::vector<std::vector<Point>> rings;
    for (auto const &component : part.components) {
        rings.push_back(component.outer);
        for (auto const &hole : component.holes)
            rings.push_back(hole);
    }
    return rings;
}

void expect_same_rings(std::vector<std::vector<Point>> const &actual,
                       std::vector<std::vector<Point>> const &expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t ring = 0; ring < expected.size(); ++ring) {
        ASSERT_EQ(actual[ring].size(), expected[ring].size()) << "ring " << ring;
        for (std::size_t point = 0; point < expected[ring].size(); ++point) {
            EXPECT_DOUBLE_EQ(actual[ring][point].x, expected[ring][point].x)
                << "ring " << ring << " point " << point;
            EXPECT_DOUBLE_EQ(actual[ring][point].y, expected[ring][point].y)
                << "ring " << ring << " point " << point;
        }
    }
}

/** Published geometry must survive the solver's f32 conversion (no zero-area /
 *  repeated-vertex ring after rounding, no non-finite coordinate). */
void expect_f32_valid(PreparedPart const &part)
{
    for (auto const &component : part.components) {
        std::vector<std::span<Point const>> rings{component.outer};
        for (auto const &hole : component.holes)
            rings.push_back(hole);
        for (auto const ring : rings) {
            std::vector<Point> rounded;
            rounded.reserve(ring.size());
            for (auto const &point : ring)
                rounded.push_back({static_cast<float>(point.x), static_cast<float>(point.y)});
            for (auto const &point : rounded)
                EXPECT_TRUE(std::isfinite(point.x) && std::isfinite(point.y)) << "f32 coordinate is not finite";
            EXPECT_GT(ring_area(rounded), 0.0) << "ring collapses under the solver f32 conversion";
            EXPECT_TRUE(ring_is_simple(rounded)) << "ring repeats a vertex or self-intersects after f32 rounding";
        }
    }
}

/** Synthetic raster fixture with a non-gray semi-transparent colour. Raw
 *  GdkPixbuf RGBA bytes and premultiplied Cairo ARGB32 bytes differ for these
 *  values, so a representation-dependent fingerprint/content hash cannot pass
 *  unnoticed; an all-black opaque grid would hash identically in both. */
std::vector<std::uint8_t> tinted_alpha_grid(int width, int height, std::vector<std::pair<int, int>> const &occupied)
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4, 0);
    for (auto const &[x, y] : occupied) {
        auto const index = (static_cast<std::size_t>(y) * width + x) * 4;
        rgba[index] = 200;
        rgba[index + 1] = 40;
        rgba[index + 2] = 90;
        rgba[index + 3] = 160;
    }
    return rgba;
}

} // namespace

// Highest-risk warm bitmap cache. Three phases on one fixture:
//  (1) GUI rendering leaves the decoded pixbuf on the Cairo representation;
//      content hashing/tracing must use representation-safe accessors only
//      (Inkscape::Pixbuf::getPixbufRaw() const asserts on PF_CAIRO, and the
//      non-const overload mutates the representation) and must not touch XML;
//      the content fingerprint must stay identical because the silhouette did
//      not change.
//  (2) An extreme document transform (here +1e9 x, where 10-unit cells fall
//      inside one f32 ulp) makes the cached trace unusable for that call only.
//  (3) Correcting the transform must restore the exact cold BitmapAlpha rings,
//      not a poisoned conservative fallback.
TEST_F(NestingDocumentTest, BitmapContourCacheRecoversExactAlphaAfterCairoRepresentationAndExtremeTransform)
{
    auto const rgba = tinted_alpha_grid(2, 2, {{0, 0}, {1, 0}, {0, 1}}); // L-shaped silhouette
    auto const body = image_document_body(2, 2, rgba, 120, 10, 20, 20);
    ASSERT_FALSE(body.empty());
    auto document = make_document(body);
    ASSERT_TRUE(document);
    auto *payload = item(*document, "payload");
    auto *image = cast<SPImage>(payload);
    ASSERT_TRUE(image);
    ASSERT_TRUE(image->pixbuf);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::vector<SPItem *> parts{payload};

    auto cold = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(cold) << cold.error;
    ASSERT_EQ(cold.snapshot->parts.size(), 1);
    ASSERT_EQ(cold.snapshot->parts.front().contour_source, ContourSource::BitmapAlpha);
    auto const cold_rings = part_rings(cold.snapshot->parts.front());
    ASSERT_FALSE(cold_rings.empty());

    // Phase 1: Cairo representation, then a warm prepare (also exercises the
    // content fingerprint over decoded pixels).
    auto &mutable_pixbuf = *const_cast<Inkscape::Pixbuf *>(image->pixbuf.get());
    mutable_pixbuf.ensurePixelFormat(Inkscape::Pixbuf::PF_CAIRO);
    ASSERT_EQ(mutable_pixbuf.pixelFormat(), Inkscape::Pixbuf::PF_CAIRO);

    auto cairo_warm = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(cairo_warm) << cairo_warm.error;
    ASSERT_EQ(cairo_warm.snapshot->parts.size(), 1);
    EXPECT_EQ(cairo_warm.snapshot->parts.front().contour_source, ContourSource::BitmapAlpha)
        << "Cairo representation made the alpha trace fall back";
    expect_same_rings(part_rings(cairo_warm.snapshot->parts.front()), cold_rings);
    EXPECT_EQ(cairo_warm.snapshot->parts.front().geometry_fingerprint,
              cold.snapshot->parts.front().geometry_fingerprint)
        << "an unchanged silhouette must keep the same geometry fingerprint";
    EXPECT_EQ(cairo_warm.snapshot->parts.front().content_fingerprint,
              cold.snapshot->parts.front().content_fingerprint)
        << "Cairo conversion is not a source edit and must not change freshness";

    // Phase 2: extreme transform collapses cell edges inside one f32 ulp.
    payload->getRepr()->setAttribute("transform", "translate(1000000000,0)");
    document->ensureUpToDate();
    auto extreme = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(extreme) << extreme.error;
    bool published = false;
    for (auto const &part : extreme.snapshot->parts) {
        if (part.item.get() != payload)
            continue;
        published = true;
        expect_f32_valid(part);
    }
    if (!published) {
        bool diagnosed = false;
        for (auto const &skipped : extreme.snapshot->skipped_parts) {
            if (skipped.item.get() == payload) {
                diagnosed = true;
                EXPECT_FALSE(skipped.detail.empty()) << "skip must carry an actionable reason";
            }
        }
        EXPECT_TRUE(diagnosed) << "unusable bitmap geometry was neither published nor skipped with a reason";
    }

    // Phase 3: correction restores the exact cold trace from the warm cache.
    payload->getRepr()->removeAttribute("transform");
    document->ensureUpToDate();
    auto corrected = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(corrected) << corrected.error;
    ASSERT_EQ(corrected.snapshot->parts.size(), 1);
    auto const &restored = corrected.snapshot->parts.front();
    EXPECT_EQ(restored.contour_source, ContourSource::BitmapAlpha)
        << "warm cache degraded to a conservative fallback after the transform was corrected";
    expect_same_rings(part_rings(restored), cold_rings);
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before)
        << "fingerprinting or representation conversion wrote to the artwork";
}

// Bounds-identical *descendant* edits are covered elsewhere. Referenced
// resources are the remaining hole: editing clip/marker content while the
// item's own transform and visual bounds stay identical must still invalidate
// the prepared snapshot before any XML write. Two scenarios keep the failure
// attributable: a clipPath child rect edit and a marker child circle edit.
TEST_F(NestingDocumentTest, ReferencedClipOrMarkerContentEditWithUnchangedBoundsInvalidatesSnapshot)
{
    auto run_scenario = [](char const *body, char const *part_id, char const *content_id) {
        auto document = make_document(body);
        ASSERT_TRUE(document);
        auto *part = item(*document, part_id);
        ASSERT_TRUE(part) << part_id;
        std::vector<SPItem *> parts{part};
        auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
        ASSERT_TRUE(prepared) << prepared.error;
        ASSERT_EQ(prepared.snapshot->parts.size(), 1);
        auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
        ASSERT_TRUE(solved) << solved.error;
        // The premise needs bounds that the change does not move. The headless
        // test harness does not produce renderer visual bounds for the clipped
        // fixture, so fall back to the item's geometric bounds; a colour edit of
        // the referenced clip/marker content changes neither.
        auto const bounds_of = [](SPItem *object) {
            if (auto visual = object->documentVisualBounds())
                return visual;
            return object->geometricBounds();
        };
        auto const bounds_before = bounds_of(part);
        ASSERT_TRUE(bounds_before);
        auto const transform_before = part->i2doc_affine();

        auto *content = document->getObjectById(content_id);
        ASSERT_TRUE(content) << content_id;
        ASSERT_TRUE(content->getRepr());
        content->getRepr()->setAttribute("fill", "blue");
        document->ensureUpToDate();
        auto const bounds_after = bounds_of(part);
        ASSERT_TRUE(bounds_after);
        // Premise of the test: this edit does not move geometry, so a
        // geometry-fingerprint pass alone cannot be the detector.
        expect_rect_near(*bounds_after, *bounds_before);
        auto const after_edit = sp_repr_save_buf(document->getReprDoc()).raw();

        auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);

        EXPECT_EQ(applied.status, ApplyStatus::StaleSnapshot)
            << "referenced " << content_id << " content changed with identical bounds";
        EXPECT_TRUE(affine_near(part->i2doc_affine(), transform_before));
        EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), after_edit);
    };

    run_scenario(R"svg(
      <defs>
        <clipPath id="clip"><rect id="clip-content" x="0" y="0" width="20" height="10" fill="red"/></clipPath>
      </defs>
      <rect id="container" width="100" height="100"/>
      <rect id="clipped-part" x="120" y="10" width="40" height="40" clip-path="url(#clip)"/>)svg",
                 "clipped-part", "clip-content");

    run_scenario(R"svg(
      <defs>
        <marker id="mark" markerWidth="4" markerHeight="4" refX="0" refY="0" orient="auto">
          <circle id="mark-content" cx="1" cy="1" r="1" fill="red"/>
        </marker>
      </defs>
      <rect id="container" width="100" height="100"/>
      <path id="marked-part" d="M 180,10 H 220" fill="none" stroke="black" stroke-width="2"
            marker-end="url(#mark)"/>)svg",
                 "marked-part", "mark-content");
}

// A container hole configuration that cannot be represented (an island nested
// inside a container hole, depth 2) must fail closed: no substitute outline, no
// snapshot, serialized hole data untouched, and no poisoning of a later valid
// container whose hole must survive preparation exactly.
TEST_F(NestingDocumentTest, InvalidContainerHoleFailsClosedAndKeepsValidHolesUsable)
{
    auto document = make_document(R"svg(
      <path id="bad-container" fill-rule="evenodd"
            d="M 0,0 H 60 V 60 H 0 Z M 10,10 H 50 V 50 H 10 Z M 20,20 H 40 V 40 H 20 Z"/>
      <g transform="translate(5,7)">
        <path id="good-container" fill-rule="evenodd"
              d="M 0,0 H 50 V 40 H 0 Z M 10,10 H 20 V 20 H 10 Z"/>
      </g>
      <rect id="part" x="100" y="80" width="8" height="6"/>)svg");
    ASSERT_TRUE(document);
    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    std::vector<SPItem *> parts{item(*document, "part")};

    auto invalid = prepareDocumentNesting(item(*document, "bad-container"), parts);

    EXPECT_FALSE(invalid);
    EXPECT_NE(invalid.error.find("container"), std::string::npos) << invalid.error;
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before)
        << "failed container preparation must not rewrite or drop hole data";
    EXPECT_FALSE(document->isModifiedSinceSave());

    auto valid = prepareDocumentNesting(item(*document, "good-container"), parts);
    ASSERT_TRUE(valid) << valid.error;
    ASSERT_EQ(valid.snapshot->container_holes.size(), 1) << "a valid container hole was lost after the failure";
    expect_rect_near(point_bounds(valid.snapshot->container_holes.front()), rect(15, 17, 25, 27));
    expect_rect_near(point_bounds(valid.snapshot->container_outline), rect(5, 7, 55, 47));
}

// A group whose explicit nesting-contour marker is invalid but which also has
// valid visible sibling artwork must not publish a subset: either the whole
// payload is skipped with a reason, or the published geometry still covers the
// sibling footprint. Correction vs the staged draft: the nonzero bowtie marker
// is normalized into a single valid triangle, so it legitimately overrides
// sibling artwork under the explicit-contour contract; a degenerate zero-area
// marker is used instead because it genuinely cannot become an explicit contour.
TEST_F(NestingDocumentTest, InvalidExplicitMarkerMemberCannotDropSiblingArtwork)
{
    auto document = make_document(R"svg(
      <rect id="container" width="100" height="100"/>
      <g id="part" transform="translate(120,30)" inkscape:nesting-contour-version="1"
         xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
        <rect id="sibling" width="10" height="10"/>
        <path id="bad-marker" inkscape:nesting-contour="true"
              d="M 40,5 L 40,5 Z"/>
      </g>)svg");
    ASSERT_TRUE(document);
    auto *part = item(*document, "part");
    ASSERT_TRUE(part);
    std::vector<SPItem *> parts{part};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    bool published = false;
    for (auto const &prepared_part : prepared.snapshot->parts) {
        if (prepared_part.item.get() != part)
            continue;
        published = true;
        EXPECT_FALSE(prepared_part.components.empty()) << "published group payload has no occupied geometry";
        EXPECT_TRUE(covered_by_components(Point{125, 35}, prepared_part.components))
            << "valid sibling artwork was dropped because the explicit marker was invalid";
    }
    if (!published) {
        bool skipped = false;
        for (auto const &skipped_part : prepared.snapshot->skipped_parts) {
            if (skipped_part.item.get() == part) {
                skipped = true;
                EXPECT_FALSE(skipped_part.detail.empty()) << "skip must carry an actionable reason";
            }
        }
        EXPECT_TRUE(skipped) << "group with an invalid explicit marker vanished from the snapshot";
    }
}

// Staircase plus one isolated occupied cell on a rotated image: coverage must
// follow the affine exactly. Probes are mapped through the same rotate(30)
// affine used by the SVG: centre plus four inset cell corners (component edges
// are axis-aligned in pixel space, so affine images of those edges cannot cut a
// probed cell), and every empty cell centre must stay outside. Area alone is
// not used as an oracle.
TEST_F(NestingDocumentTest, RotatedBitmapStaircaseAndIsolatedCellCoverageIsExact)
{
    constexpr int width = 4;
    constexpr int height = 4;
    std::vector<std::pair<int, int>> occupied{
        {0, 0}, {1, 0}, {2, 0}, {3, 0}, {3, 1}, {3, 2}, {2, 2}, {1, 2}, {0, 3}};
    auto const rgba = alpha_grid(width, height, occupied);
    auto const body = image_document_body(width, height, rgba, 120, 10, 40, 40);
    ASSERT_FALSE(body.empty());
    auto document = make_document(body);
    ASSERT_TRUE(document);
    item(*document, "payload")->getRepr()->setAttribute("transform", "rotate(30 120 10)");
    document->ensureUpToDate();
    std::vector<SPItem *> parts{item(*document, "payload")};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);

    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 1);
    auto const &part = prepared.snapshot->parts.front();
    EXPECT_EQ(part.contour_source, ContourSource::BitmapAlpha);
    ASSERT_EQ(part.components.size(), 2) << "the isolated occupied cell must stay its own component";

    double const radians = 30.0 * 3.14159265358979323846 / 180.0;
    double const cosine = std::cos(radians);
    double const sine = std::sin(radians);
    auto const rotated = [&](Point const &local) {
        auto const dx = local.x - 120.0;
        auto const dy = local.y - 10.0;
        return Point{120.0 + cosine * dx - sine * dy, 10.0 + sine * dx + cosine * dy};
    };
    auto const cell_point = [&](int cell_x, int cell_y, double pixel_x, double pixel_y) {
        return rotated(Point{120.0 + (cell_x + pixel_x) * 10.0, 10.0 + (cell_y + pixel_y) * 10.0});
    };

    constexpr double inset = 1.0e-3;
    for (auto const &[cell_x, cell_y] : occupied) {
        for (int const corner : {0, 1, 2, 3}) {
            auto const x = (corner & 1) ? 1.0 - inset : inset;
            auto const y = (corner >> 1) ? 1.0 - inset : inset;
            EXPECT_TRUE(covered_by_components(cell_point(cell_x, cell_y, x, y), part.components))
                << "uncovered occupied cell corner " << cell_x << "," << cell_y;
        }
        EXPECT_TRUE(covered_by_components(cell_point(cell_x, cell_y, 0.5, 0.5), part.components))
            << "uncovered occupied cell centre " << cell_x << "," << cell_y;
    }
    for (int cell_y = 0; cell_y < height; ++cell_y) {
        for (int cell_x = 0; cell_x < width; ++cell_x) {
            if (std::find(occupied.begin(), occupied.end(), std::pair{cell_x, cell_y}) != occupied.end())
                continue;
            EXPECT_FALSE(covered_by_components(cell_point(cell_x, cell_y, 0.5, 0.5), part.components))
                << "empty cell was filled in " << cell_x << "," << cell_y;
        }
    }
}

// Freshness must be representation independent: GUI rendering converts the
// decoded image from GdkPixbuf RGBA to premultiplied Cairo ARGB32 in place,
// which is not a source edit. A snapshot prepared before that conversion must
// still apply, while a real in-place alpha edit at the same pixbuf address must
// still be rejected as stale before any XML write.
TEST_F(NestingDocumentTest, PrepareThenCairoRepresentationIsNotFalselyStaleAndAlphaEditIsStale)
{
    auto const rgba = tinted_alpha_grid(2, 2, {{0, 0}, {1, 0}, {0, 1}, {1, 1}});
    auto const body = image_document_body(2, 2, rgba, 120, 10, 20, 20);
    ASSERT_FALSE(body.empty());
    auto document = make_document(body);
    ASSERT_TRUE(document);
    auto *payload = item(*document, "payload");
    auto *image = cast<SPImage>(payload);
    ASSERT_TRUE(image);
    ASSERT_TRUE(image->pixbuf);
    std::vector<SPItem *> parts{payload};

    auto prepared = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 1);
    ASSERT_EQ(prepared.snapshot->parts.front().contour_source, ContourSource::BitmapAlpha);
    auto solved = solvePreparedNesting(*prepared.snapshot, draft_options());
    ASSERT_TRUE(solved) << solved.error;

    auto &mutable_pixbuf = *const_cast<Inkscape::Pixbuf *>(image->pixbuf.get());
    mutable_pixbuf.ensurePixelFormat(Inkscape::Pixbuf::PF_CAIRO);
    ASSERT_EQ(mutable_pixbuf.pixelFormat(), Inkscape::Pixbuf::PF_CAIRO);

    auto applied = applyNestingPlacements(*prepared.snapshot, solved.placements);
    EXPECT_NE(applied.status, ApplyStatus::StaleSnapshot)
        << "a Cairo pixel representation must not be mistaken for a source edit";

    // Same-address alpha edit: re-prepare, then clear one pixel in place. The
    // pixbuf address is unchanged, so only content can invalidate the snapshot.
    auto second = prepareDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(second) << second.error;
    ASSERT_EQ(second.snapshot->parts.size(), 1);
    auto second_solved = solvePreparedNesting(*second.snapshot, draft_options());
    ASSERT_TRUE(second_solved) << second_solved.error;

    auto const *const pixbuf_address = image->pixbuf.get();
    set_pixbuf_alpha(mutable_pixbuf, 0, 0, 0);
    EXPECT_EQ(image->pixbuf.get(), pixbuf_address) << "the alpha edit must reuse the same pixbuf address";
    auto const transform_before = payload->i2doc_affine();
    auto const xml_before = sp_repr_save_buf(document->getReprDoc()).raw();

    auto stale = applyNestingPlacements(*second.snapshot, second_solved.placements);
    EXPECT_EQ(stale.status, ApplyStatus::StaleSnapshot)
        << "an in-place alpha edit must invalidate the prepared snapshot";
    EXPECT_TRUE(affine_near(payload->i2doc_affine(), transform_before));
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), xml_before);
}

TEST_F(NestingDocumentTest, ManySubpathAggregateVertexCapFallsBackToWholePartBounds)
{
    // F1 regression: contours_from_path flattened and retained every subpath
    // before the shared validity gate charged aggregate vertices, so a single
    // path with many subpaths could collect up to 4096 x 100000 vertices
    // before the 100000 aggregate cap ran. Sixteen disjoint analytic circles
    // stay far below the 4096 ring cap and avoid stroke/Boolean expansion, but
    // the 1e-6 flatten tolerance expands each ring to thousands of points, so
    // the aggregate vertex peek must fire while flattening and diagnose the
    // whole part; a ring- or pair-cap diagnosis would fail the reason check.
    constexpr int columns = 4;
    constexpr int rows = 4;
    constexpr double radius = 20.0;
    constexpr double spacing = 50.0;
    std::string path_data;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            auto const cx = 25.0 + column * spacing;
            auto const cy = 25.0 + row * spacing;
            path_data += "M " + std::to_string(cx - radius) + "," + std::to_string(cy) + " a " +
                         std::to_string(radius) + "," + std::to_string(radius) + " 0 1 0 " +
                         std::to_string(2 * radius) + ",0 a " + std::to_string(radius) + "," +
                         std::to_string(radius) + " 0 1 0 " + std::to_string(-2 * radius) + ",0 z ";
        }
    }

    auto document = make_document(
        R"svg(<rect id="container" width="300" height="240"/><path id="many" fill="black" d=")svg" + path_data +
        R"svg("/>)svg");
    ASSERT_TRUE(document);
    auto *many = item(*document, "many");
    ASSERT_TRUE(many);

    std::vector<SPItem *> parts{many};
    auto prepared = prepareDocumentNesting(item(*document, "container"), parts, 1.0e-6);
    ASSERT_TRUE(prepared) << prepared.error;
    ASSERT_EQ(prepared.snapshot->parts.size(), 1u);
    auto const &part = prepared.snapshot->parts.front();
    EXPECT_EQ(part.recovery, RecoveryKind::ConservativeFallback);
    EXPECT_EQ(part.contour_source, ContourSource::ConservativeBounds);
    EXPECT_NE(part.recovery_reason.find("aggregate"), std::string::npos)
        << "the aggregate vertex peek must be the diagnosed cause, not the ring or pair cap: "
        << part.recovery_reason;
    ASSERT_FALSE(part.components.empty());
    // Whole-part bounds must reach the outer circles of the complete grid: a
    // truncated aggregate would stop near the origin instead.
    expect_rect_near(point_bounds(part.components.front().outer), rect(5, 5, 195, 195));

    // The same oversized path used as a container must fail closed: containers
    // never receive a bounds/hull substitute.
    auto container_document = make_document(R"svg(<path id="container" fill="black" d=")svg" + path_data +
                                            R"svg("/><rect id="part" x="220" y="10" width="10" height="10"/>)svg");
    ASSERT_TRUE(container_document);
    std::vector<SPItem *> container_parts{item(*container_document, "part")};
    auto failed = prepareDocumentNesting(item(*container_document, "container"), container_parts, 1.0e-6);
    EXPECT_FALSE(failed) << "an over-budget container must not be replaced by fallback geometry";
    EXPECT_NE(failed.error.find("too complex"), std::string::npos) << failed.error;
}

TEST_F(NestingDocumentTest, CaptureSharesNoStorageWithTheDocument)
{
    // A pixel pattern no other test uses, so the trace is not cached and the
    // capture really copies the alpha plane.
    auto const uri = png_data_uri(3, 3, alpha_grid(3, 3, {{0, 0}, {1, 0}, {2, 0}, {0, 1}, {1, 1}, {0, 2}}));
    ASSERT_FALSE(uri.empty());
    auto document = make_document(R"svg(<rect id="container" width="100" height="100"/>
      <path id="shape" d="M 150,10 L 170,10 L 170,30 L 150,30 Z"/>
      <image id="bitmap" x="200" y="10" width="20" height="20" href=")svg" + uri + R"svg("/>)svg");
    ASSERT_TRUE(document);
    auto *image = cast<SPImage>(item(*document, "bitmap"));
    ASSERT_TRUE(image && image->pixbuf);
    std::vector<SPItem *> parts{item(*document, "shape"), image};
    auto capture = captureDocumentNesting(item(*document, "container"), parts);
    ASSERT_TRUE(capture) << capture.error;
    auto const &captured = capture.input->candidates;
    ASSERT_EQ(captured.size(), 2u);
    // A written copy: a PathVector copy would share lib2geom's data.
    auto const path_text = sp_svg_write_path(captured[0].item.root.path);
    ASSERT_TRUE(captured[1].item.root.alpha) << "the alpha plane was not copied";
    auto const alpha_copy = captured[1].item.root.alpha->alpha;
    ASSERT_FALSE(alpha_copy.empty());
    auto const before = prepareCapturedGeometry(*capture.input);
    ASSERT_TRUE(before) << before.error;

    // Edit the source path data and a pixel in place after the capture.
    item(*document, "shape")->setAttribute("d", "M 150,10 L 190,10 L 190,50 Z");
    set_pixbuf_alpha(*const_cast<Inkscape::Pixbuf *>(image->pixbuf.get()), 1, 1, 0);
    document->ensureUpToDate();

    EXPECT_EQ(sp_svg_write_path(captured[0].item.root.path), path_text);
    EXPECT_EQ(captured[1].item.root.alpha->alpha, alpha_copy);
    auto const after = prepareCapturedGeometry(*capture.input);
    ASSERT_TRUE(after) << after.error;
    ASSERT_EQ(after.parts.size(), before.parts.size());
    for (std::size_t i = 0; i < after.parts.size(); ++i) {
        EXPECT_EQ(after.parts[i].geometry_fingerprint, before.parts[i].geometry_fingerprint) << i;
    }
}

// --- R1 parity gate: corpus and comparison (must stay the last tests) ---------------

TEST_F(NestingDocumentTest, ZzPreparationGoldenCorpus)
{
    if (!golden::enabled()) {
        GTEST_SKIP() << "UNEXECUTED: set VACARDS_NESTING_GOLDEN_OUT or VACARDS_NESTING_GOLDEN_IN";
    }
    auto prepare_file = [](std::filesystem::path const &svg, std::filesystem::path const &ids_file,
                           char const *container_id, std::string const &name) {
        auto document = SPDocument::createNewDoc(svg.string().c_str());
        ASSERT_TRUE(document) << svg;
        document->ensureUpToDate();
        std::vector<SPItem *> parts;
        std::ifstream ids(ids_file);
        for (std::string id; ids >> id;)
            parts.push_back(cast<SPItem>(document->getObjectById(id)));
        auto result = golden::raw_prepare(cast<SPItem>(document->getObjectById(container_id)), parts);
        golden::record(name, result);
    };
    std::vector<std::filesystem::path> corpus; // sorted: directory order is not stable
    for (auto const &entry : std::filesystem::directory_iterator(NESTING_SPARROW_CORPUS)) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "input.svg"))
            corpus.push_back(entry.path());
    }
    std::sort(corpus.begin(), corpus.end());
    for (auto const &dir : corpus) {
        prepare_file(dir / "input.svg", dir / "ids.txt", "container", "sparrow/" + dir.filename().string());
    }
    std::filesystem::path const r0{NESTING_RESPONSIVENESS_FIXTURES};
    for (auto const *name : {"vector_cards_200", "detailed_curves_240", "stickers_bitmap_50", "stroked_paths_100"}) {
        if (!std::filesystem::exists(r0 / (std::string(name) + ".svg"))) {
            ADD_FAILURE() << "R0 fixture missing: run test_nesting-responsiveness-fixtures first (" << name << ")";
            continue;
        }
        prepare_file(r0 / (std::string(name) + ".svg"), r0 / (std::string(name) + ".ids.txt"), "sheet",
                     std::string("r0/") + name);
    }
}

TEST_F(NestingDocumentTest, ZzTwoPhasePreparationMatchesGolden)
{
    auto const *in = g_getenv("VACARDS_NESTING_GOLDEN_IN");
    if (!in || !*in) {
        GTEST_SKIP() << "UNEXECUTED: set VACARDS_NESTING_GOLDEN_IN to the golden file";
    }
    golden::stream().close();
    auto read = [](std::string const &path) {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    };
    auto const expected = read(in);
    auto const actual = read(std::string(in) + ".actual");
    ASSERT_FALSE(expected.empty()) << "golden file is empty: " << in;
    EXPECT_TRUE(actual == expected) << "two-phase preparation differs from the golden file; diff " << in << " "
                                    << in << ".actual";
}
