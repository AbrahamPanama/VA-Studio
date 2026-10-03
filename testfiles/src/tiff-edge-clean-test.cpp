// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Owner request 2026-09-28: TIFFs for RIP printing must not carry the dark
 * edge pixels (and colour hidden under full transparency) that images made in
 * other programs bring along. clean_rgba_edges_for_rip is the pixel pass of
 * the TIFF export's "Clean edges for printing (RIP)" and "Hard edges" options.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <vector>

#include "io/tiff-export.h"

using Inkscape::IO::clean_rgba_edges_for_rip;

namespace {

using Pixel = std::array<unsigned char, 4>;

struct Image {
    std::uint32_t width, height;
    std::vector<unsigned char> rgba;
    Image(std::uint32_t w, std::uint32_t h, Pixel fill) : width(w), height(h), rgba(w * h * 4)
    {
        for (std::size_t i = 0; i < rgba.size(); i += 4) {
            std::copy(fill.begin(), fill.end(), rgba.begin() + i);
        }
    }
    void set(std::uint32_t x, std::uint32_t y, Pixel p) { std::copy(p.begin(), p.end(), rgba.begin() + (y * width + x) * 4); }
    Pixel get(std::uint32_t x, std::uint32_t y) const
    {
        auto const *p = rgba.data() + (y * width + x) * 4;
        return {p[0], p[1], p[2], p[3]};
    }
};

constexpr Pixel gold{191, 152, 71, 255};
constexpr Pixel transparent_white{255, 255, 255, 0};

TEST(TiffEdgeClean, DarkEdgeTakesTheArtworkColourAndKeepsItsTransparency)
{
    Image image(12, 5, transparent_white);
    for (std::uint32_t y = 0; y < 5; ++y) {
        for (std::uint32_t x = 0; x < 5; ++x) image.set(x, y, gold);
        image.set(5, y, {60, 40, 10, 180}); // dark fringe, as in artifactssamp.tiff
        image.set(6, y, {30, 20, 5, 20});
    }
    EXPECT_GT(clean_rgba_edges_for_rip(image.rgba, image.width, image.height, true, false), 0u);
    for (std::uint32_t y = 0; y < 5; ++y) {
        EXPECT_EQ(image.get(5, y), (Pixel{191, 152, 71, 180}));
        EXPECT_EQ(image.get(6, y), (Pixel{191, 152, 71, 20}));
        EXPECT_EQ(image.get(2, y), gold) << "opaque artwork is unchanged";
    }
}

TEST(TiffEdgeClean, ColourHiddenUnderFullTransparencyIsRemoved)
{
    Image image(4, 4, transparent_white);
    image.set(1, 1, {255, 0, 0, 0}); // what the "AlphaKiller" logo carries
    image.set(2, 2, {0, 0, 0, 0});
    clean_rgba_edges_for_rip(image.rgba, image.width, image.height, true, false);
    EXPECT_EQ(image.get(1, 1), transparent_white);
    EXPECT_EQ(image.get(2, 2), transparent_white);
}

TEST(TiffEdgeClean, WideFringeOfAnEnlargedImageIsCleanedRingByRing)
{
    Image image(12, 3, transparent_white);
    for (std::uint32_t y = 0; y < 3; ++y) {
        image.set(0, y, gold);
        for (std::uint32_t x = 1; x <= 4; ++x) image.set(x, y, {40, 30, 10, static_cast<unsigned char>(240 - x * 40)});
    }
    clean_rgba_edges_for_rip(image.rgba, image.width, image.height, true, false);
    for (std::uint32_t x = 1; x <= 4; ++x) {
        EXPECT_EQ(image.get(x, 1), (Pixel{191, 152, 71, static_cast<unsigned char>(240 - x * 40)})) << x;
    }
}

TEST(TiffEdgeClean, BandWidensWithTheRingCount)
{
    // A 72 dpi image exported at 600 dpi: an 8 pixel fringe; the export
    // passes 8 rings at that resolution. Transparent margins all round.
    Image image(30, 21, transparent_white);
    for (std::uint32_t y = 0; y < 21; ++y) {
        for (std::uint32_t x = 0; x < 10; ++x) image.set(x, y, gold); // artwork to the left border
        for (std::uint32_t x = 1; x <= 8; ++x) {
            image.set(9 + x, y, {40, 30, 10, static_cast<unsigned char>(250 - x * 25)});
        }
    }
    auto four = image;
    clean_rgba_edges_for_rip(four.rgba, four.width, four.height, true, false, 4);
    EXPECT_EQ(four.get(10, 10), (Pixel{40, 30, 10, 225})) << "farther than 4 pixels from transparency";
    clean_rgba_edges_for_rip(image.rgba, image.width, image.height, true, false, 8);
    for (std::uint32_t x = 1; x <= 8; ++x) {
        EXPECT_EQ(image.get(9 + x, 10), (Pixel{191, 152, 71, static_cast<unsigned char>(250 - x * 25)})) << x;
    }
}

TEST(TiffEdgeClean, FringeOnTheImageBorderIsCleaned)
{
    // Review finding: an export cropped tight to the artwork; the fringe
    // runs out of the image with no transparent pixel near it.
    Image image(6, 12, gold);
    for (std::uint32_t y = 0; y < 12; ++y) {
        image.set(5, y, {60, 40, 10, 150});
    }
    clean_rgba_edges_for_rip(image.rgba, image.width, image.height, true, false);
    EXPECT_EQ(image.get(5, 6), (Pixel{191, 152, 71, 150}));
}

TEST(TiffEdgeClean, BandFollowsExportResolution)
{
    using Inkscape::IO::rip_edge_rings_for_dpi;
    EXPECT_EQ(rip_edge_rings_for_dpi(96), 4u);
    EXPECT_EQ(rip_edge_rings_for_dpi(288), 4u);
    EXPECT_EQ(rip_edge_rings_for_dpi(600), 8u);
    EXPECT_EQ(rip_edge_rings_for_dpi(1200), 17u);
    EXPECT_EQ(rip_edge_rings_for_dpi(100000), 64u);
    EXPECT_EQ(rip_edge_rings_for_dpi(0), 4u);
    EXPECT_EQ(rip_edge_rings_for_dpi(std::numeric_limits<double>::quiet_NaN()), 4u);
}

TEST(TiffEdgeClean, TranslucentPanelNextToOpaqueArtworkKeepsItsColour)
{
    // Review finding: a 50% blue panel touching opaque gold artwork.
    constexpr Pixel panel{20, 60, 200, 128};
    Image image(30, 12, transparent_white);
    for (std::uint32_t y = 1; y < 11; ++y) {
        for (std::uint32_t x = 1; x < 6; ++x) image.set(x, y, gold);
        for (std::uint32_t x = 6; x < 26; ++x) image.set(x, y, panel);
    }
    clean_rgba_edges_for_rip(image.rgba, image.width, image.height, true, false);
    for (std::uint32_t y = 1; y < 11; ++y) {
        for (std::uint32_t x = 6; x < 26; ++x) {
            // Known limit: where the panel's own outline meets the artwork, a
            // band-wide corner of it looks like the artwork's fringe.
            if ((y <= 4 || y >= 7) && x < 6 + 4) {
                continue;
            }
            ASSERT_EQ(image.get(x, y), panel) << x << "," << y;
        }
    }
}

TEST(TiffEdgeClean, WideShadowTouchingTheArtworkKeepsItsColour)
{
    // A black 35% shadow 10 pixels wide, drawn right against the artwork.
    constexpr Pixel shadow{0, 0, 0, 90};
    Image image(24, 30, transparent_white);
    for (std::uint32_t y = 0; y < 30; ++y) {
        for (std::uint32_t x = 0; x < 6; ++x) image.set(x, y, gold);
        for (std::uint32_t x = 6; x < 16; ++x) image.set(x, y, shadow);
    }
    clean_rgba_edges_for_rip(image.rgba, image.width, image.height, true, false);
    for (std::uint32_t y = 0; y < 30; ++y) {
        for (std::uint32_t x = 6; x < 16; ++x) {
            // Known limit: a band-wide corner where the shadow's outline (here
            // the image border) meets the artwork.
            if ((y < 4 || y >= 26) && x < 6 + 4) {
                continue;
            }
            ASSERT_EQ(image.get(x, y), shadow) << x << "," << y;
        }
    }
}

TEST(TiffEdgeClean, HardEdgesAloneAlsoCleans)
{
    Image image(8, 1, transparent_white);
    image.set(0, 0, gold);
    image.set(1, 0, {30, 20, 5, 200});
    clean_rgba_edges_for_rip(image.rgba, image.width, image.height, false, true);
    EXPECT_EQ(image.get(1, 0), gold) << "no dark outline made opaque";
}

TEST(TiffEdgeClean, OptionsDefaultOffForOtherCallers)
{
    // The TIFF export's options (Export dialog, command line) default on in
    // the TIFF output extension; other callers of the writer opt in.
    Inkscape::IO::TiffExportOptions options;
    EXPECT_FALSE(options.clean_edges);
    EXPECT_FALSE(options.hard_edges);
}

TEST(TiffEdgeClean, SoftShadowAwayFromArtworkKeepsItsColour)
{
    Image image(12, 3, transparent_white);
    for (std::uint32_t y = 0; y < 3; ++y) {
        image.set(0, y, gold);
        for (std::uint32_t x = 4; x < 12; ++x) image.set(x, y, {0, 0, 0, 90}); // shadow 3+ px away
    }
    clean_rgba_edges_for_rip(image.rgba, image.width, image.height, true, false);
    EXPECT_EQ(image.get(8, 1), (Pixel{0, 0, 0, 90}));
    EXPECT_EQ(image.get(4, 1), (Pixel{0, 0, 0, 90}));
}

TEST(TiffEdgeClean, CorrectEdgesAndOptionsOffChangeNothing)
{
    Image image(8, 3, transparent_white);
    for (std::uint32_t y = 0; y < 3; ++y) {
        for (std::uint32_t x = 0; x < 3; ++x) image.set(x, y, gold);
        image.set(3, y, {191, 152, 71, 128}); // already clean antialiasing
    }
    auto const before = image.rgba;
    EXPECT_EQ(clean_rgba_edges_for_rip(image.rgba, image.width, image.height, true, false), 0u);
    EXPECT_EQ(image.rgba, before);

    image.set(4, 1, {10, 10, 10, 60});
    image.set(6, 0, {255, 0, 0, 0});
    auto const dirty = image.rgba;
    EXPECT_EQ(clean_rgba_edges_for_rip(image.rgba, image.width, image.height, false, false), 0u);
    EXPECT_EQ(image.rgba, dirty) << "both options off: untouched";
}

TEST(TiffEdgeClean, HardEdgesThresholdAtHalfAndStayClean)
{
    Image image(8, 1, transparent_white);
    image.set(0, 0, gold);
    image.set(1, 0, {30, 20, 5, 200});
    image.set(2, 0, {30, 20, 5, 100});
    clean_rgba_edges_for_rip(image.rgba, image.width, image.height, true, true);
    EXPECT_EQ(image.get(1, 0), gold) << "cleaned, then opaque";
    EXPECT_EQ(image.get(2, 0), transparent_white) << "below half: transparent and white";
}

TEST(TiffEdgeClean, ShortBufferIsLeftAlone)
{
    std::vector<unsigned char> rgba(10, 7);
    EXPECT_EQ(clean_rgba_edges_for_rip(rgba, 4, 4, true, true), 0u);
    EXPECT_EQ(rgba, std::vector<unsigned char>(10, 7));
    std::vector<unsigned char> none;
    EXPECT_EQ(clean_rgba_edges_for_rip(none, 0, 7, true, true), 0u) << "zero size returns";
}

TEST(TiffEdgeClean, ZeroRingsStillRemovesHiddenColour)
{
    Image image(3, 1, transparent_white);
    image.set(1, 0, {255, 0, 0, 0});
    EXPECT_EQ(clean_rgba_edges_for_rip(image.rgba, image.width, image.height, true, false, 0), 2u)
        << "two colour samples changed";
    EXPECT_EQ(image.get(1, 0), transparent_white);
}

} // namespace
