// SPDX-License-Identifier: GPL-2.0-or-later
// Original synthetic geometry only. No customer file or environment-variable lane.
#include "io/artwork-library-lightburn.h"
#ifdef VACARDS_PRIVATE_LBART_TEST
#include "io/artwork-library-lbart.h"
#include "io/artwork-library-package.h"
#include "io/artwork-library-svg-preflight.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#endif

#include <2geom/affine.h>
#include <2geom/pathvector.h>
#include <2geom/svg-path-parser.h>
#include <gtest/gtest.h>
#include <libxml/parser.h>

#include <array>
#include <cmath>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace Inkscape::IO::ArtworkLibrary;
namespace {
using Code = LightBurnDraftErrorCode;
// False/True is the identity SVG frame under the corrected root convention.
// All four combinations are also tested explicitly below.
std::string wrap(std::string const &shapes, bool mx = false, bool my = true)
{
    return "<LightBurnShapes FormatVersion=\"1\" MirrorX=\"" + std::string(mx ? "True" : "False") +
           "\" MirrorY=\"" + (my ? "True" : "False") + "\">" + shapes + "</LightBurnShapes>";
}
std::string path(std::string const &vertices, std::string const &primitives,
                 std::string const &transform = "1 0 0 1 0 0")
{
    return "<Shape Type=\"Path\"><XForm>" + transform + "</XForm><VertList>" + vertices +
           "</VertList><PrimList>" + primitives + "</PrimList></Shape>";
}
std::string rect(std::string const &w = "10", std::string const &h = "6", std::string const &cr = "0")
{
    return "<Shape Type=\"Rect\" W=\"" + w + "\" H=\"" + h + "\" Cr=\"" + cr +
           "\"><XForm>1 0 0 1 0 0</XForm></Shape>";
}
std::string line() { return path("V0 0c0x1c1x1V12 7c0x1c1x1", "L0 1"); }
std::string with_tabs(std::string shape, std::string const &text)
{
    shape.insert(shape.find("</Shape>"), "<Tabs>" + text + "</Tabs>");
    return shape;
}
std::string group(std::string const &contents, std::string const &xform)
{
    return "<Shape Type=\"Group\"><XForm>" + xform + "</XForm><Children>" + contents + "</Children></Shape>";
}
void rejects(std::string xml, Code code, LightBurnDraftLimits limits = {})
{
    try {
        auto result = convert_lightburn_shapes_v1_draft(std::move(xml), limits);
        FAIL() << "Unexpected draft returned (" << result.svg.size() << " bytes)";
    } catch (LightBurnDraftError const &e) {
        EXPECT_EQ(e.code(), code) << e.what();
    }
}
std::string property(xmlNodePtr node, char const *key)
{
    auto p = xmlGetProp(node, BAD_CAST key);
    if (!p) return {};
    std::string result(reinterpret_cast<char const *>(p)); xmlFree(p); return result;
}
struct Output {
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc{nullptr, xmlFreeDoc};
    explicit Output(std::string const &svg)
        : doc(xmlReadMemory(svg.data(), static_cast<int>(svg.size()), nullptr, "UTF-8", XML_PARSE_NONET), xmlFreeDoc)
    {
        if (!doc) throw std::runtime_error("Generated SVG was not well-formed");
    }
    std::vector<xmlNodePtr> nodes(char const *tag) const
    {
        std::vector<xmlNodePtr> out, todo{xmlDocGetRootElement(doc.get())};
        while (!todo.empty()) {
            auto n = todo.back(); todo.pop_back();
            if (n->type == XML_ELEMENT_NODE && xmlStrEqual(n->name, BAD_CAST tag)) out.push_back(n);
            for (auto c = n->last; c; c = c->prev) todo.push_back(c);
        }
        return out;
    }
    static Geom::Affine world(xmlNodePtr node)
    {
        Geom::Affine out;
        for (auto n = node; n && n->type != XML_DOCUMENT_NODE; n = n->parent) {
            auto transform = property(n, "transform");
            if (transform.empty()) continue;
            if (!transform.starts_with("matrix(") || transform.back() != ')')
                throw std::runtime_error("Expected generated matrix");
            std::istringstream stream(transform.substr(7, transform.size() - 8));
            stream.imbue(std::locale::classic()); std::array<double, 6> a{};
            for (auto &v : a) if (!(stream >> v)) throw std::runtime_error("Bad generated matrix");
            out *= Geom::Affine(a[0], a[1], a[2], a[3], a[4], a[5]);
        }
        return out;
    }
    Geom::PathVector paths(xmlNodePtr n) const
    {
        auto p = Geom::parse_svg_path(property(n, "d").c_str());
        p *= world(n); return p;
    }
};

TEST(LightBurnDraftTest, ChildThenParentThenEachRootMirrorCombination)
{
    for (bool mx : {false, true}) for (bool my : {false, true}) {
        auto xml = wrap(group(path("V1 2V4 2V4 6", "L0 1L1 2L2 0", "2 0 0 3 5 7"),
                              "0 1 -1 0 10 20"), mx, my);
        Output out(convert_lightburn_shapes_v1_draft(xml).svg);
        auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
        auto geometry = out.paths(nodes[0]); ASSERT_EQ(geometry.size(), 1u);
        // (1,2) -> child (7,13) -> parent (-3,27) -> root signs.
        auto p = geometry[0].initialPoint();
        EXPECT_DOUBLE_EQ(p[0], mx ? 3 : -3); EXPECT_DOUBLE_EQ(p[1], my ? 27 : -27);
        EXPECT_TRUE(geometry[0].closed());
        EXPECT_EQ(out.nodes("g").size(), 2u); // Source group plus explicit root frame.
        auto root = xmlDocGetRootElement(out.doc.get());
        EXPECT_EQ(property(root, "width"), "14mm"); EXPECT_EQ(property(root, "height"), "8mm");
        EXPECT_EQ(property(root, "viewBox"), std::string(mx ? "2 " : "-16 ") +
                  (my ? "26 " : "-34 ") + "14 8");
    }
}

TEST(LightBurnDraftTest, KnownTenBySixMillimeterRectangleWithOneMillimeterMargins)
{
    for (bool mx : {false, true}) for (bool my : {false, true}) {
        Output out(convert_lightburn_shapes_v1_draft(wrap(rect(), mx, my)).svg);
        auto root = xmlDocGetRootElement(out.doc.get());
        EXPECT_EQ(property(root, "width"), "12mm"); EXPECT_EQ(property(root, "height"), "8mm");
        EXPECT_EQ(property(root, "viewBox"), "-6 -4 12 8");
        auto nodes = out.nodes("rect"); ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(property(nodes[0], "width"), "10"); EXPECT_EQ(property(nodes[0], "height"), "6");
        auto matrix = Output::world(nodes[0]);
        auto a = Geom::Point(-5, -3) * matrix, b = Geom::Point(5, -3) * matrix;
        auto c = Geom::Point(-5, 3) * matrix;
        EXPECT_NEAR(std::hypot(b[0] - a[0], b[1] - a[1]), 10, 1e-12);
        EXPECT_NEAR(std::hypot(c[0] - a[0], c[1] - a[1]), 6, 1e-12);
        // Matching viewport/viewBox dimensions make these numerical lengths mm.
    }
}

TEST(LightBurnDraftTest, RotatedNonuniformScalePreservesPhysicalSizeWithoutPixelRescaling)
{
    Output out(convert_lightburn_shapes_v1_draft(wrap(group(rect(), "0 2 -3 0 20 30"))).svg);
    auto root = xmlDocGetRootElement(out.doc.get());
    EXPECT_EQ(property(root, "width"), "20mm"); EXPECT_EQ(property(root, "height"), "22mm");
    EXPECT_EQ(property(root, "viewBox"), "10 19 20 22");
    auto nodes = out.nodes("rect"); ASSERT_EQ(nodes.size(), 1u);
    auto matrix = Output::world(nodes[0]);
    auto a = Geom::Point(-5, -3) * matrix, b = Geom::Point(5, -3) * matrix;
    auto c = Geom::Point(-5, 3) * matrix;
    EXPECT_NEAR(std::hypot(b[0] - a[0], b[1] - a[1]), 20, 1e-12);
    EXPECT_NEAR(std::hypot(c[0] - a[0], c[1] - a[1]), 18, 1e-12);
}

TEST(LightBurnDraftTest, CubicUsesAbsoluteStartC0AndEndC1)
{
    auto xml = wrap(path("V0 0c0x0c0y8c1x77c1y91V12 0c0x88c0y92c1x12c1y8", "B0 1"));
    Output out(convert_lightburn_shapes_v1_draft(xml).svg);
    auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
    auto geometry = out.paths(nodes[0]); ASSERT_EQ(geometry.size(), 1u);
    auto middle = geometry[0][0].pointAt(0.5);
    EXPECT_NEAR(middle[0], 6, 1e-12); EXPECT_NEAR(middle[1], 6, 1e-12);
    EXPECT_FALSE(geometry[0].closed());
    // The exact geometric maximum is y=6, not the control-hull y=8.
    EXPECT_EQ(property(xmlDocGetRootElement(out.doc.get()), "viewBox"), "-1 -1 14 8");
}

TEST(LightBurnDraftTest, ClosingCubicIsRetainedBeforeCloseCommand)
{
    auto xml = wrap(path("V0 0c0x1c1x0c1y4V8 0c0x8c0y4c1x1", "L0 1B1 0"));
    Output out(convert_lightburn_shapes_v1_draft(xml).svg);
    auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
    auto geometry = out.paths(nodes[0]); ASSERT_EQ(geometry.size(), 1u);
    EXPECT_TRUE(geometry[0].closed());
    auto middle = geometry[0][1].pointAt(0.5);
    EXPECT_NEAR(middle[0], 4, 1e-12); EXPECT_NEAR(middle[1], 3, 1e-12);
}

TEST(LightBurnDraftTest, EqualCoordinatesDoNotImplyIndexClosure)
{
    Output out(convert_lightburn_shapes_v1_draft(wrap(path("V0 0V4 5V0 0", "L0 1L1 2"))).svg);
    auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
    EXPECT_FALSE(out.paths(nodes[0])[0].closed());
}

TEST(LightBurnDraftTest, DisconnectedRunsRemainSeparateSubpaths)
{
    Output out(convert_lightburn_shapes_v1_draft(wrap(path("V0 0V2 3V9 1V12 5", "L0 1L2 3"))).svg);
    auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
    auto geometry = out.paths(nodes[0]); ASSERT_EQ(geometry.size(), 2u);
    EXPECT_FALSE(geometry[0].closed()); EXPECT_FALSE(geometry[1].closed());
}

TEST(LightBurnDraftTest, CompoundClosedContoursArePreservedWithoutInventingFill)
{
    auto vertices = "V0 0V10 0V10 10V0 10V3 3V3 7V7 7V7 3";
    auto primitives = "L0 1L1 2L2 3L3 0L4 5L5 6L6 7L7 4";
    Output out(convert_lightburn_shapes_v1_draft(wrap(path(vertices, primitives))).svg);
    auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
    auto geometry = out.paths(nodes[0]); ASSERT_EQ(geometry.size(), 2u);
    EXPECT_TRUE(geometry[0].closed()); EXPECT_TRUE(geometry[1].closed());
    EXPECT_EQ(property(out.nodes("g")[0], "fill"), "none");
}

TEST(LightBurnDraftTest, EveryRootMirrorPreservesCompoundContourRelativeWinding)
{
    auto v = "V0 0V10 0V10 10V0 10V3 3V3 7V7 7V7 3";
    auto e = "L0 1L1 2L2 3L3 0L4 5L5 6L6 7L7 4";
    for (bool mx : {false, true}) for (bool my : {false, true}) {
        Output out(convert_lightburn_shapes_v1_draft(wrap(path(v, e), mx, my)).svg);
        auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
        auto geometry = out.paths(nodes[0]); ASSERT_EQ(geometry.size(), 2u);
        auto orientation = [](Geom::Path const &p) {
            auto a = p.initialPoint(), b = p[0].finalPoint(), c = p[1].finalPoint();
            return (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0]);
        };
        auto determinant = (mx ? -1 : 1) * (my ? 1 : -1);
        EXPECT_DOUBLE_EQ(orientation(geometry[0]), 100 * determinant);
        EXPECT_DOUBLE_EQ(orientation(geometry[1]), -16 * determinant);
        EXPECT_TRUE(geometry[0].closed()); EXPECT_TRUE(geometry[1].closed());
        EXPECT_EQ(property(out.nodes("g")[0], "fill"), "none");
    }
}

TEST(LightBurnDraftTest, MirrorsPreserveClosingCubicAndDistinctOpenRun)
{
    auto v = "V0 0c1x0c1y4V8 0c0x8c0y4V20 0V24 3V20 0";
    for (bool mx : {false, true}) for (bool my : {false, true}) {
        auto result = convert_lightburn_shapes_v1_draft(wrap(path(v, "L0 1B1 0L2 3L3 4"), mx, my));
        Output out(result.svg); auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
        auto geometry = out.paths(nodes[0]); ASSERT_EQ(geometry.size(), 2u);
        EXPECT_EQ(result.primitives, 4u);
        EXPECT_TRUE(geometry[0].closed()); EXPECT_FALSE(geometry[1].closed());
        auto mid = geometry[0][1].pointAt(0.5);
        EXPECT_NEAR(mid[0], mx ? -4 : 4, 1e-12); EXPECT_NEAR(mid[1], my ? 3 : -3, 1e-12);
        EXPECT_EQ(geometry[1].initialPoint(), geometry[1].finalPoint());
        EXPECT_NE(property(nodes[0], "d").find("C "), std::string::npos);
    }
}

TEST(LightBurnDraftTest, CompactOpenAndClosedListsHaveExplicitDraftQualification)
{
    for (auto mode : {"LineOpen", "LineClosed"}) {
        auto draft = convert_lightburn_shapes_v1_draft(wrap(path("V0 0V2 0V2 3", mode)));
        Output out(draft.svg); auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
        EXPECT_EQ(out.paths(nodes[0])[0].closed(), std::string(mode) == "LineClosed");
        EXPECT_EQ(draft.primitives, std::string(mode) == "LineClosed" ? 3u : 2u);
        bool noted = false;
        for (auto const &q : draft.qualifications) noted |= q.find("LineOpen/LineClosed") != std::string::npos;
        EXPECT_TRUE(noted);
    }
}

TEST(LightBurnDraftTest, RectangleIsCenteredAndRadiusStaysEditable)
{
    Output out(convert_lightburn_shapes_v1_draft(wrap(group(rect("10", "6", "2"), "1 0 0 1 14 9"))).svg);
    auto nodes = out.nodes("rect"); ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(property(nodes[0], "x"), "-5"); EXPECT_EQ(property(nodes[0], "y"), "-3");
    EXPECT_EQ(property(nodes[0], "width"), "10"); EXPECT_EQ(property(nodes[0], "height"), "6");
    EXPECT_EQ(property(nodes[0], "rx"), "2"); EXPECT_EQ(property(nodes[0], "ry"), "2");
    auto corner = Geom::Point(-5, -3) * Output::world(nodes[0]);
    EXPECT_DOUBLE_EQ(corner[0], 9); EXPECT_DOUBLE_EQ(corner[1], 6);
    EXPECT_EQ(out.nodes("path").size(), 0u);
}

TEST(LightBurnDraftTest, ControlsWithXEqualToOneAreNotMistakenForFlags)
{
    Output out(convert_lightburn_shapes_v1_draft(wrap(path("V0 0c0x1c0y4V8 0c1x7c1y4", "B0 1"))).svg);
    auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
    auto mid = out.paths(nodes[0])[0][0].pointAt(0.5);
    EXPECT_NEAR(mid[0], 4, 1e-12); EXPECT_NEAR(mid[1], 3, 1e-12);
}

TEST(LightBurnDraftTest, MissingAndFlagOnlyCubicControlsAreNotGuessed)
{
    for (auto vertices : {"V0 0V8 0", "V0 0c0x1V8 0c1x1", "V0 0c0x1c1x1V8 0c1x8c1y4"})
        rejects(wrap(path(vertices, "B0 1")), Code::Unsupported);
    rejects(wrap(path("V0 0c0x2V8 0", "L0 1")), Code::Unsupported);
}

TEST(LightBurnDraftTest, ExplicitZeroOrdinateIsAcceptedButOmittedRequiredYRefusesWholeEntry)
{
    auto complete = path("V0 5c0x4c0y0V12 2c1x8c1y8", "B0 1");
    Output out(convert_lightburn_shapes_v1_draft(wrap(complete)).svg);
    auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
    auto mid = out.paths(nodes[0])[0][0].pointAt(0.5);
    EXPECT_NEAR(mid[0], 6, 1e-12); EXPECT_NEAR(mid[1], 3.875, 1e-12);
    Output one(convert_lightburn_shapes_v1_draft(wrap(path("V0 5c0x1c0y0V12 2c1x8c1y8", "B0 1"))).svg);
    auto one_nodes = one.nodes("path"); ASSERT_EQ(one_nodes.size(), 1u);
    EXPECT_NEAR(one.paths(one_nodes[0])[0][0].pointAt(0.5)[0], 4.875, 1e-12);
    for (auto v : {"V0 5c0x4V12 2c1x8c1y8", "V0 5c0x4c0y0V12 2c1x8",
                   "V0 0c0x4V12 2c1x8c1y8"})
        rejects(wrap(rect() + path(v, "B0 1")), Code::Unsupported);
    // Original synthetic analogues, not copied record-19 coordinates.
}

TEST(LightBurnDraftTest, SmoothFlagDoesNotAlterExplicitGeometry)
{
    auto draft = convert_lightburn_shapes_v1_draft(wrap(path("V0 0c0x0c0y4SV8 0c1x8c1y4S", "B0 1")));
    Output out(draft.svg); auto nodes = out.nodes("path"); ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NEAR(out.paths(nodes[0])[0][0].pointAt(0.5)[1], 3, 1e-12);
    bool noted = false;
    for (auto const &q : draft.qualifications) noted |= q.find("editing constraints") != std::string::npos;
    EXPECT_TRUE(noted);
}

TEST(LightBurnDraftTest, UnsupportedShapesDoNotYieldPartialSuccess)
{
    for (auto type : {"Text", "Ellipse", "Bitmap", "Image", "Polygon", "Unknown"}) {
        SCOPED_TRACE(type);
        rejects(wrap(line() + "<Shape Type=\"" + type + "\"><XForm>1 0 0 1 0 0</XForm></Shape>"), Code::Unsupported);
    }
}

TEST(LightBurnDraftTest, SharedGeometryReferencesAndUnknownAttributesAreExplicitErrors)
{
    auto p = line(); p.insert(p.find('>'), " VertID=\"2\""); rejects(wrap(p), Code::Unsupported);
    p = line(); p.insert(p.find('>'), " PrimID=\"3\""); rejects(wrap(p), Code::Unsupported);
    p = line(); p.insert(p.find('>'), " Unknown=\"0\""); rejects(wrap(p), Code::Unsupported);
    p = line(); p.insert(p.find("</Shape>"), "<Unknown/>"); rejects(wrap(p), Code::Unsupported);
}

TEST(LightBurnDraftTest, TabsRetainEditableGeometryAndExplicitPerShapeDiagnostics)
{
    std::string const text = " 2,0.25 7,0.75 ";
    auto baseline = convert_lightburn_shapes_v1_draft(wrap(group(line() + rect(), "1 0 0 1 14 9")));
    auto result = convert_lightburn_shapes_v1_draft(wrap(group(with_tabs(line(), text) +
                                  with_tabs(rect(), "1,0.5"), "1 0 0 1 14 9")));
    ASSERT_EQ(result.unapplied_laser_tabs.size(), 2u);
    EXPECT_EQ(result.unapplied_laser_tabs[0].shape_preorder_1based, 2u);
    EXPECT_EQ(result.unapplied_laser_tabs[1].shape_preorder_1based, 3u);
    EXPECT_EQ(result.unapplied_laser_tabs[0].pair_count, 2u);
    EXPECT_EQ(result.unapplied_laser_tabs[0].source_text, text);
    EXPECT_EQ(result.unapplied_laser_tabs[1].source_text, "1,0.5");
    EXPECT_NE(std::string(result.unapplied_laser_tabs[0].message).find("not applied"), std::string::npos);
    Output before(baseline.svg), after(result.svg);
    auto p0 = before.nodes("path"), p1 = after.nodes("path");
    ASSERT_EQ(p0.size(), 1u); ASSERT_EQ(p1.size(), 1u);
    EXPECT_EQ(property(p0[0], "d"), property(p1[0], "d"));
    EXPECT_EQ(Output::world(p0[0]), Output::world(p1[0]));
    auto r0 = xmlDocGetRootElement(before.doc.get()), r1 = xmlDocGetRootElement(after.doc.get());
    for (auto key : {"viewBox", "width", "height"}) EXPECT_EQ(property(r0, key), property(r1, key));
    EXPECT_EQ(result.primitives, baseline.primitives); EXPECT_EQ(result.shapes, baseline.shapes);
    EXPECT_NE(result.svg.find("Laser tabs not applied"), std::string::npos);
    EXPECT_EQ(result.svg.find("2,0.25"), std::string::npos); // No opaque source text enters SVG.
    bool diagnostic = false;
    for (auto const &q : result.qualifications) diagnostic |= q.find("Laser tabs not applied") != std::string::npos;
    EXPECT_TRUE(diagnostic);
    EXPECT_TRUE(convert_lightburn_shapes_v1_draft(wrap(with_tabs(line(), " \t\n"))).unapplied_laser_tabs.empty());
}

TEST(LightBurnDraftTest, MalformedOrExtendedTabMetadataNeverSilentlySucceeds)
{
    rejects(wrap(with_tabs(line(), "1 2")), Code::Unsupported);
    rejects(wrap(with_tabs(line(), "<Tab/>")), Code::Unsupported);
    for (auto text : {"1,", "nan,2", "1,inf", "1,2,3,4", "1,2+3,4", "1e999,2", "1e10,2"})
        rejects(wrap(with_tabs(line(), text)), Code::Geometry);
    auto p = with_tabs(line(), "1,2");
    p.insert(p.find("<Tabs") + 5, " Unknown=\"1\""); rejects(wrap(p), Code::Unsupported);
    p = with_tabs(line(), "1,2"); p.insert(p.find("</Shape>"), "<Tabs>3,4</Tabs>");
    rejects(wrap(p), Code::Unsupported);
    rejects(wrap(group(line(), "1 0 0 1 0 0") +
                 "<Shape Type=\"Text\"><Tabs>1,2</Tabs></Shape>"), Code::Unsupported);
}

TEST(LightBurnDraftTest, TabBudgetsAreAggregateWithExactBoundaries)
{
    auto xml = wrap(with_tabs(line(), "1,2") + with_tabs(rect(), "3,4"));
    LightBurnDraftLimits limits; limits.tab_pairs = 2; limits.tab_metadata_bytes = 6;
    EXPECT_NO_THROW(convert_lightburn_shapes_v1_draft(xml, limits));
    limits.tab_pairs = 1; rejects(xml, Code::Limit, limits);
    limits.tab_pairs = 2; limits.tab_metadata_bytes = 5; rejects(xml, Code::Limit, limits);
    limits = {}; limits.tab_pairs = 0;
    EXPECT_NO_THROW(convert_lightburn_shapes_v1_draft(wrap(with_tabs(line(), "")), limits));
    rejects(wrap(with_tabs(line(), "1,2")), Code::Limit, limits);
}

TEST(LightBurnDraftTest, CancellationDuringTabScanReturnsNoPartialEntry)
{
    std::string pairs;
    for (unsigned i = 0; i < 300; ++i) pairs += "1,2 ";
    unsigned checks = 0;
    try {
        convert_lightburn_shapes_v1_draft(wrap(with_tabs(line(), pairs)), {}, [&] { return ++checks == 10; });
        FAIL() << "Expected cancellation";
    } catch (LightBurnDraftError const &e) {
        EXPECT_EQ(e.code(), Code::Cancelled);
    }
    EXPECT_EQ(checks, 10u);
    auto result = convert_lightburn_shapes_v1_draft(wrap(with_tabs(line(), pairs)));
    ASSERT_EQ(result.unapplied_laser_tabs.size(), 1u);
    EXPECT_EQ(result.unapplied_laser_tabs[0].pair_count, 300u);
}

TEST(LightBurnDraftTest, BranchesUnorderedChainsOrphansAndBadIndicesAreRejected)
{
    for (auto primitives : {"L0 1L0 2", "L1 2L0 1", "L0 1L1 0L2 0"})
        rejects(wrap(path("V0 0V2 3V4 1", primitives)), Code::Unsupported);
    rejects(wrap(path("V0 0V2 3V4 1", "L0 1")), Code::Unsupported);
    for (auto primitives : {"L0 2", "L0 0", "L-1 0", "L0 184467440737095516160"})
        rejects(wrap(path("V0 0V2 3", primitives)), Code::Geometry);
    rejects(wrap(path("V0 0V2 3", "Q0 1")), Code::Unsupported);
}

TEST(LightBurnDraftTest, InvalidNumericAndPackedTokensAreRejected)
{
    for (auto vertices : {"Vnan 0V2 3", "V1e999 0V2 3", "V1e-999 0V2 3", "V1e 0V2 3", "V1e10 0V2 3"})
        rejects(wrap(path(vertices, "L0 1")), Code::Geometry);
    for (auto vertices : {"V0 0c0y4V2 3", "V0 0c0x1c0x1V2 3", "V0 0ZZV2 3"})
        rejects(wrap(path(vertices, "L0 1")), Code::Unsupported);
    for (auto xform : {"1 0 0 1 0", "1 0 0 1 0 0 0", "1 0 nan 1 0 0"})
        rejects(wrap(path("V0 0V2 3", "L0 1", xform)), Code::Geometry);
}

TEST(LightBurnDraftTest, ComposedCoordinateLimitIsChecked)
{
    rejects(wrap(group(path("V2 1V3 2", "L0 1", "1000000 0 0 1 0 0"), "1000000 0 0 1 0 0")), Code::Geometry);
    rejects(wrap(path("V1000000 0V1000001 1", "L0 1", "1000000 0 0 1 0 0")), Code::Geometry);
}

TEST(LightBurnDraftTest, UnqualifiedRectangleCornersAreRefused)
{
    rejects(wrap(rect("10", "6", "-1")), Code::Unsupported);
    rejects(wrap(rect("10", "6", "4")), Code::Unsupported);
    rejects(wrap(rect("0", "6")), Code::Geometry);
    rejects(wrap(rect("-2", "6")), Code::Geometry);
}

TEST(LightBurnDraftTest, VersionRootAndMirrorFlagsAreExplicit)
{
    auto xml = wrap(line()); xml.replace(xml.find("Version=\"1"), 10, "Version=\"2");
    rejects(xml, Code::Unsupported);
    rejects("<LightBurnShapes FormatVersion=\"1\">" + line() + "</LightBurnShapes>", Code::Unsupported);
    rejects("<LightBurnProject/>", Code::Unsupported);
    xml = wrap(line()); xml.replace(xml.find("False"), 5, "false"); rejects(xml, Code::Unsupported);
}

TEST(LightBurnDraftTest, MalformedAndTruncatedXmlNeverReturnsADraft)
{
    auto xml = wrap(line());
    for (std::size_t n = 1; n < xml.size(); ++n) {
        SCOPED_TRACE(n); rejects(xml.substr(0, n), Code::Xml);
    }
    rejects(wrap(line()) + wrap(line()), Code::Xml);
    rejects("<LightBurnShapes FormatVersion=\"1\" FormatVersion=\"1\"/>", Code::Xml);
}

TEST(LightBurnDraftTest, DtdEntitiesInstructionsAndNamespacesAreRejected)
{
    for (auto prefix : {"<!DOCTYPE LightBurnShapes SYSTEM 'file:///nonexistent/lbart-must-not-read'>",
                        "<!DOCTYPE LightBurnShapes SYSTEM 'https://invalid.example/lbart-must-not-fetch'>",
                        "<!DOCTYPE LightBurnShapes [<!ENTITY x 'payload'>]>",
                        "<?xml-stylesheet href='file:///nonexistent/lbart-must-not-read'?>",
                        "<!--comment-->"})
        rejects(std::string(prefix) + wrap(line()), Code::Xml);
    rejects(wrap("&amp;" + line()), Code::Xml);
    rejects(wrap("<![CDATA[x]]>" + line()), Code::Xml);
    auto xml = wrap(line()); xml.insert(xml.find('>'), " xmlns=\"urn:unsupported\"");
    rejects(xml, Code::Unsupported);
    xml = wrap(line()); xml.insert(xml.begin() + 4, '\0'); rejects(xml, Code::Xml);
    xml = wrap(line()); xml.insert(xml.begin() + 4, static_cast<char>(0xff)); rejects(xml, Code::Xml);
    EXPECT_NO_THROW(convert_lightburn_shapes_v1_draft("<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + wrap(line())));
    EXPECT_NO_THROW(convert_lightburn_shapes_v1_draft("<?xml version='1.0' encoding='utf-8' standalone='yes'?>" + wrap(line())));
    rejects("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>" + wrap(line()), Code::Unsupported);
    rejects("<?xml version=\"1.0\" encoding=\"UTF-7\"?>" + wrap(line()), Code::Unsupported);
}

TEST(LightBurnDraftTest, XmlAndGeometryBudgetsAreAggregate)
{
    auto xml = wrap(line() + line());
    LightBurnDraftLimits l;
    l.xml_bytes = xml.size() - 1; rejects(xml, Code::Limit, l);
    l = {}; l.xml_nodes = 1; rejects(xml, Code::Limit, l);
    l = {}; l.xml_depth = 2; rejects(xml, Code::Limit, l);
    l = {}; l.text_bytes = 3; rejects(xml, Code::Limit, l);
    l = {}; l.shapes = 1; rejects(xml, Code::Limit, l);
    l = {}; l.vertices = 3; rejects(xml, Code::Limit, l);
    l = {}; l.primitives = 1; rejects(xml, Code::Limit, l);
    l = {}; l.svg_bytes = 50; rejects(xml, Code::Limit, l);
    l = {}; l.coordinate_abs = std::numeric_limits<double>::infinity(); rejects(xml, Code::Limit, l);
    auto deep = line();
    for (unsigned i = 0; i < 40; ++i) deep = group(deep, "1 0 0 1 0 0");
    l = {}; l.xml_depth = std::numeric_limits<std::size_t>::max(); rejects(wrap(deep), Code::Limit, l);
}

TEST(LightBurnDraftTest, OutputBudgetHasAnExactBoundary)
{
    for (auto const &xml : {wrap(line()), wrap(with_tabs(line(), "1,2"))}) {
        auto result = convert_lightburn_shapes_v1_draft(xml);
        LightBurnDraftLimits l; l.svg_bytes = result.svg.size();
        EXPECT_EQ(convert_lightburn_shapes_v1_draft(xml, l).svg, result.svg);
        --l.svg_bytes; rejects(xml, Code::Limit, l);
    }
}

TEST(LightBurnDraftTest, ExactGeometryAndInputBudgetsAreAdmitted)
{
    auto xml = wrap(line()); LightBurnDraftLimits l;
    l.xml_bytes = xml.size(); l.shapes = 1; l.vertices = 2; l.primitives = 1;
    EXPECT_NO_THROW(convert_lightburn_shapes_v1_draft(xml, l));
    l.primitives = 0; rejects(xml, Code::Limit, l);
}

TEST(LightBurnDraftTest, CancellationDuringGeometryAndOwnerDestruction)
{
    std::string vertices;
    for (unsigned i = 0; i < 100; ++i) vertices += "V" + std::to_string(i) + " " + std::to_string(i % 7);
    auto xml = wrap(path(vertices, "LineOpen"));
    unsigned checks = 0;
    EXPECT_THROW(convert_lightburn_shapes_v1_draft(xml, {}, [&] { return ++checks == 10; }), LightBurnDraftError);
    EXPECT_EQ(checks, 10u);
    auto result = convert_lightburn_shapes_v1_draft(xml);
    EXPECT_EQ(result.vertices, 100u); EXPECT_EQ(result.primitives, 99u);
    std::optional<std::string> owner(xml);
    EXPECT_NO_THROW(convert_lightburn_shapes_v1_draft(*owner, {}, [&] { owner.reset(); return false; }));
    EXPECT_FALSE(owner);
}

TEST(LightBurnDraftTest, SourceBackedMillimetersDoNotClaimFidelityOrSvgCertification)
{
    auto p = line(); p.insert(p.find('>'), " CutIndex=\"12\"");
    auto result = convert_lightburn_shapes_v1_draft(wrap(p));
    EXPECT_FALSE(result.lightburn_fidelity_qualified);
    EXPECT_TRUE(result.millimeter_mapping_source_backed);
    EXPECT_FALSE(result.physical_fidelity_qualified);
    EXPECT_TRUE(result.requires_svg_preflight);
    EXPECT_GE(result.qualifications.size(), 5u);
    Output out(result.svg); auto root = xmlDocGetRootElement(out.doc.get());
    EXPECT_EQ(property(root, "width"), "14mm");
    EXPECT_EQ(property(root, "height"), "9mm");
    EXPECT_NE(result.svg.find("fill=\"none\""), std::string::npos);
    EXPECT_EQ(out.nodes("image").size(), 0u);
}

#ifdef VACARDS_PRIVATE_LBART_TEST
// Only an explicitly built private diagnostic contains this case. The normal
// target has no private-file requirement and no skipped substitute. Nothing
// decoded from the customer file is written, published or used as a thumbnail.
TEST(LightBurnPrivatePipeline, TarjetasSupportedEntriesConvertAndPassSvgPreflight)
{
    auto path = std::getenv("VACARDS_PRIVATE_LBART_SAMPLE");
    ASSERT_TRUE(path);
    ASSERT_EQ(std::filesystem::file_size(path), 4218346u);
    auto read = [&] {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Private library cannot be opened");
        Bytes data(4218346);
        if (!input.read(reinterpret_cast<char *>(data.data()), data.size()) || input.peek() != EOF)
            throw std::runtime_error("Private library changed during bounded read");
        return data;
    };
    auto data = read();
    std::string const source_hash = "f16729fc57d10ae0728191a4dfe180abd171d169a9cdce00b720131238ca2f86";
    ASSERT_EQ(artwork_sha256(data), source_hash);
    auto archive = LbartArchive::open(std::move(data));
    ASSERT_EQ(archive.entries().size(), 37u);
    unsigned converted_count = 0, admitted_count = 0, unsupported_count = 0;
    unsigned tab_entries = 0, tab_shapes = 0, tab_pairs = 0;
    for (std::size_t i = 0; i < 37; ++i) {
        SCOPED_TRACE("private directory ordinal " + std::to_string(i + 1));
        auto xml = archive.read_artwork(i);
        try {
            auto converted = convert_lightburn_shapes_v1_draft(std::string(xml.begin(), xml.end()));
            ++converted_count;
            EXPECT_NE(i, 18u) << "Record 19 must not silently guess its missing controls";
            tab_entries += !converted.unapplied_laser_tabs.empty();
            tab_shapes += converted.unapplied_laser_tabs.size();
            for (auto const &tab : converted.unapplied_laser_tabs) tab_pairs += tab.pair_count;
            Bytes svg(converted.svg.begin(), converted.svg.end());
            auto admitted = preflight_svg(svg, {"b203e8e9-640c-4195-b0ea-f9b790245678",
                artwork_sha256(svg), converted.width_mm, converted.height_mm});
            EXPECT_EQ(*admitted.svg_bytes(), converted.svg);
            ++admitted_count;
        } catch (LightBurnDraftError const &error) {
            ++unsupported_count;
            EXPECT_EQ(i, 18u) << error.what();
            EXPECT_EQ(error.code(), LightBurnDraftErrorCode::Unsupported);
        } catch (SvgPreflightError const &error) {
            ADD_FAILURE() << "Converted entry failed SVG preflight: " << error.what();
        }
    }
    EXPECT_EQ(converted_count, 36u); EXPECT_EQ(admitted_count, 36u);
    EXPECT_EQ(unsupported_count, 1u);
    EXPECT_EQ(tab_entries, 6u); EXPECT_EQ(tab_shapes, 13u); EXPECT_EQ(tab_pairs, 34u);
    EXPECT_EQ(artwork_sha256(read()), source_hash);
    RecordProperty("converted_entries", converted_count);
    RecordProperty("preflight_admitted_entries", admitted_count);
    RecordProperty("unsupported_entries_not_imported", unsupported_count);
    RecordProperty("unapplied_laser_tab_shapes", tab_shapes);
}
#endif
} // namespace
