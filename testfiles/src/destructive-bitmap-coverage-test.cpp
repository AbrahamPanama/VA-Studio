// SPDX-License-Identifier: GPL-2.0-or-later
//
// B02 focused tests for the detached mask/clip/opacity coverage helper.
//
// Oracles are independent of the implementation: grey coverage is derived from
// the documented native luminance coefficients (109/366/37, divisor 512), unit
// rectangles are computed analytically, and source RGB is never rendered. The
// real F-MASK fixture is only inspected; its mask is never removed.

#include "ui/tools/destructive-bitmap-coverage.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glibmm/ustring.h>
#include <gtest/gtest.h>

#include <2geom/affine.h>

#include "display/cairo-utils.h"
#include "document.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-image.h"
#include "object/sp-item.h"
#include "object/sp-mask.h"
#include "object/sp-root.h"
#include "xml/repr.h"

namespace Coverage = Inkscape::UI::Tools::DestructiveBitmapCoverage;
using Inkscape::Pixbuf;

namespace {

constexpr int SOURCE_W = 4;
constexpr int SOURCE_H = 4;

std::unique_ptr<Pixbuf> solid_pixbuf(int width, int height, guint32 rgba)
{
    auto *raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    if (!raw) {
        return {};
    }
    gdk_pixbuf_fill(raw, rgba);
    return std::make_unique<Pixbuf>(raw);
}

std::unique_ptr<Pixbuf> pixel_pixbuf(int width, int height, std::vector<guint32> const &rgba)
{
    auto *raw = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    if (!raw) {
        return {};
    }
    for (int y = 0; y < height; ++y) {
        auto *row_pixels = gdk_pixbuf_get_pixels(raw) + static_cast<std::size_t>(y) * gdk_pixbuf_get_rowstride(raw);
        for (int x = 0; x < width; ++x) {
            auto *p = row_pixels + static_cast<std::size_t>(x) * 4;
            guint32 const value = rgba[static_cast<std::size_t>(y) * width + x];
            p[0] = (value >> 24) & 0xff;
            p[1] = (value >> 16) & 0xff;
            p[2] = (value >> 8) & 0xff;
            p[3] = value & 0xff;
        }
    }
    return std::make_unique<Pixbuf>(raw);
}

std::string png_uri(Pixbuf const &pixbuf)
{
    auto encoded = sp_image_encode_png_data_uri(pixbuf);
    return encoded ? *encoded : std::string{};
}

void ensure_application()
{
    if (!Inkscape::Application::exists()) {
        Inkscape::Application::create(false);
    }
}

/** Native luminance-to-alpha on 8-bit premultiplied RGB, re-derived. */
unsigned luminance_to_alpha(int r, int g, int b)
{
    return static_cast<unsigned>((109 * r + 366 * g + 37 * b + 256) >> 9);
}

class CoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensure_application();
        source_uri = png_uri(*solid_pixbuf(SOURCE_W, SOURCE_H, 0x336699ff));
        ASSERT_FALSE(source_uri.empty());
    }

    /** Load an SVG with `%1` replaced by the source image data URI. */
    void load(Glib::ustring const &svg_template)
    {
        auto const svg = Glib::ustring::compose(svg_template, source_uri);
        document = SPDocument::createNewDocFromMem(svg.raw());
        ASSERT_TRUE(document);
        document->ensureUpToDate();
    }

    SPImage *image(char const *id = "bitmap") const
    {
        return document ? dynamic_cast<SPImage *>(document->getObjectById(id)) : nullptr;
    }

    SPItem *item(char const *id) const
    {
        return document ? dynamic_cast<SPItem *>(document->getObjectById(id)) : nullptr;
    }

    static unsigned at(Coverage::Result const &result, int x, int y)
    {
        return result.pixels[static_cast<std::size_t>(y) * result.width + x];
    }

    std::string source_uri;
    std::unique_ptr<SPDocument> document;
};

// Native grayscale luminance-to-alpha, including a per-pixel geometry cut.
TEST_F(CoverageTest, GrayscaleLuminanceMaskUsesNativeCoefficients)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <mask id="lum" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse">
   <rect x="0" y="0" width="4" height="4" fill="#4080c0"/>
  </mask>
  <mask id="half" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse">
   <rect x="0" y="0" width="2" height="4" fill="#ffffff"/>
  </mask>
 </defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        mask="url(#lum)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(bitmap->pixbuf);

    auto const lum = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(lum.status, Coverage::Status::Completed) << "status=" << static_cast<int>(lum.status);
    ASSERT_EQ(lum.width, SOURCE_W);
    ASSERT_EQ(lum.height, SOURCE_H);
    unsigned const expected = luminance_to_alpha(0x40, 0x80, 0xc0);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            EXPECT_EQ(at(lum, x, y), expected) << "at " << x << "," << y;
        }
    }

    // Add the geometry cut on the same image and confirm both are present.
    bitmap->getRepr()->setAttribute("mask", "url(#half)");
    document->ensureUpToDate();
    auto const half = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(half.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            EXPECT_EQ(at(half, x, y), x < 2 ? 255u : 0u) << "at " << x << "," << y;
        }
    }
}

// A white mask rect carries its alpha through premultiplied luminance: the
// coverage must follow the alpha, not the (white) luminance alone.
TEST_F(CoverageTest, WhiteAlphaMaskFollowsAlphaNotRgb)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <mask id="alpha" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse">
   <rect x="0" y="0" width="4" height="4" fill="#ffffff" fill-opacity="0.5"/>
  </mask>
 </defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        mask="url(#alpha)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            // 0.5 alpha on premultiplied white -> 127 or 128 in 8-bit.
            EXPECT_NEAR(static_cast<double>(at(result, x, y)), 127.5, 1.0)
                << "at " << x << "," << y;
        }
    }
}

// Partial opacity on the source image itself and on a wrapper both scale the
// coverage once, never twice.
TEST_F(CoverageTest, PartialOpacityMultipliesCoverageOnce)
{
    // Image opacity alone (no ancestor context).
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        style="opacity:0.5" href="%1"/>
</svg>)svg");
    {
        auto *bitmap = image();
        ASSERT_TRUE(bitmap);
        auto const image_only = Coverage::evaluate(*bitmap, *bitmap);
        ASSERT_EQ(image_only.status, Coverage::Status::Completed);
        for (int y = 0; y < SOURCE_H; ++y) {
            for (int x = 0; x < SOURCE_W; ++x) {
                EXPECT_NEAR(static_cast<double>(at(image_only, x, y)), 127.5, 1.0);
            }
        }
    }

    // Wrapper opacity and image opacity compose once each (0.5 * 0.5 = 0.25).
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <g id="wrapper" opacity="0.5">
  <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
         style="opacity:0.5" href="%1"/>
 </g>
</svg>)svg");

    auto *bitmap = image();
    auto *wrapper = item("wrapper");
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(wrapper);

    auto const chain = Coverage::evaluate(*wrapper, *bitmap);
    ASSERT_EQ(chain.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            EXPECT_NEAR(static_cast<double>(at(chain, x, y)), 63.75, 1.0)
                << "at " << x << "," << y;
        }
    }
}

// userSpaceOnUse clip geometry is evaluated on the source pixel centres.
TEST_F(CoverageTest, ClipUserSpaceOnUseSelectsPixelCentres)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <clipPath id="clip" clipPathUnits="userSpaceOnUse">
   <rect x="1" y="1" width="2" height="2"/>
  </clipPath>
 </defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        clip-path="url(#clip)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            bool const inside = (x >= 1 && x <= 2 && y >= 1 && y <= 2);
            EXPECT_EQ(at(result, x, y), inside ? 255u : 0u) << "at " << x << "," << y;
        }
    }
}

// clipPathUnits/objectBoundingBox scales the clip into the item bbox.
TEST_F(CoverageTest, ClipObjectBoundingBoxUnitsScaleToItemBounds)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <clipPath id="clip" clipPathUnits="objectBoundingBox">
   <rect x="0" y="0" width="0.5" height="1"/>
  </clipPath>
 </defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        clip-path="url(#clip)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            EXPECT_EQ(at(result, x, y), x < 2 ? 255u : 0u) << "at " << x << "," << y;
        }
    }
}

// maskContentUnits/objectBoundingBox scales mask content into the item bbox.
TEST_F(CoverageTest, MaskObjectBoundingBoxContentUnitsScaleToItemBounds)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <mask id="mask" maskUnits="userSpaceOnUse" maskContentUnits="objectBoundingBox">
   <rect x="0" y="0" width="0.5" height="1" fill="#ffffff"/>
  </mask>
 </defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        mask="url(#mask)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            EXPECT_EQ(at(result, x, y), x < 2 ? 255u : 0u) << "at " << x << "," << y;
        }
    }
}

// The coverage must land on the source grid even when the wrapper transform
// differs: the wrapper's mask is in wrapper user space, so a translate shifts
// the covered pixels.
TEST_F(CoverageTest, NestedWrapperTransformKeepsSourceGridRegistration)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
 <defs>
  <mask id="wrapper-mask" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse">
   <rect x="1" y="1" width="2" height="2" fill="#ffffff"/>
  </mask>
 </defs>
 <g id="wrapper" transform="translate(1 1)" mask="url(#wrapper-mask)">
  <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none" href="%1"/>
 </g>
</svg>)svg");

    auto *bitmap = image();
    auto *wrapper = item("wrapper");
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(wrapper);
    auto const mapping = bitmap->pixelToDocumentAffine();
    ASSERT_TRUE(mapping);
    EXPECT_TRUE(Geom::are_near(*mapping, Geom::Affine(Geom::Translate(1, 1)), 1e-9));

    auto const result = Coverage::evaluate(*wrapper, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            bool const inside = (x >= 1 && x <= 2 && y >= 1 && y <= 2);
            EXPECT_EQ(at(result, x, y), inside ? 255u : 0u) << "at " << x << "," << y;
        }
    }
}

// Even-odd clip holes stay transparent while the ring stays visible.
TEST_F(CoverageTest, EvenOddClipRetainsHole)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <clipPath id="clip" clipPathUnits="userSpaceOnUse">
   <path clip-rule="evenodd" d="M0,0 H4 V4 H0 Z M1,1 H3 V3 H1 Z"/>
  </clipPath>
 </defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        clip-path="url(#clip)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            bool const hole = (x >= 1 && x <= 2 && y >= 1 && y <= 2);
            EXPECT_EQ(at(result, x, y), hole ? 0u : 255u) << "at " << x << "," << y;
        }
    }
}

// A different-resolution raster mask is sampled onto the source grid without
// resampling the source RGB. Nearest sampling makes the 2x2 pattern exact.
TEST_F(CoverageTest, DifferentResolutionMaskSamplesOntoSourceGrid)
{
    auto const mask_pixels = pixel_pixbuf(2, 2, {
        0x000000ff, 0xffffffff,
        0xffffffff, 0x000000ff,
    });
    ASSERT_TRUE(mask_pixels);
    auto const mask_uri = png_uri(*mask_pixels);
    ASSERT_FALSE(mask_uri.empty());

    auto const svg = Glib::ustring::compose(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <mask id="mask" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse">
   <image x="0" y="0" width="4" height="4" preserveAspectRatio="none"
          style="image-rendering:optimizeSpeed" href="%2"/>
  </mask>
 </defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        mask="url(#mask)" href="%1"/>
</svg>)svg", source_uri, mask_uri);

    document = SPDocument::createNewDocFromMem(svg.raw());
    ASSERT_TRUE(document);
    document->ensureUpToDate();

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            bool const white = (x >= 2) != (y >= 2);
            EXPECT_EQ(at(result, x, y), white ? 255u : 0u) << "at " << x << "," << y;
        }
    }
}

// The chain composes a wrapper mask and the image's own mask exactly once each.
TEST_F(CoverageTest, WrapperAndImageMasksComposeOnce)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <mask id="wrapper-mask" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse">
   <rect x="0" y="0" width="4" height="2" fill="#ffffff"/>
  </mask>
  <mask id="image-mask" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse">
   <rect x="0" y="0" width="2" height="4" fill="#ffffff"/>
  </mask>
 </defs>
 <g id="wrapper" mask="url(#wrapper-mask)">
  <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
         mask="url(#image-mask)" href="%1"/>
 </g>
</svg>)svg");

    auto *bitmap = image();
    auto *wrapper = item("wrapper");
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(wrapper);
    auto const result = Coverage::evaluate(*wrapper, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            bool const inside = (x < 2 && y < 2);
            EXPECT_EQ(at(result, x, y), inside ? 255u : 0u) << "at " << x << "," << y;
        }
    }
}

TEST_F(CoverageTest, CancellationReturnsNoPartialBuffer)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    std::stop_source stop;
    stop.request_stop();
    auto const result = Coverage::evaluate(*bitmap, *bitmap, stop.get_token());
    EXPECT_EQ(result.status, Coverage::Status::Cancelled);
    EXPECT_TRUE(result.pixels.empty());
    EXPECT_FALSE(result.completed());
    EXPECT_FALSE(result.view().has_value());
}

// B01's null-buffer Coverage means "full coverage", so a failed evaluation must
// be impossible to forward as coverage: view() is status-gated.
TEST_F(CoverageTest, ViewIsStatusGatedAndNeverFullCoverage)
{
    Coverage::Result failed; // default InvalidInput
    EXPECT_FALSE(failed.completed());
    EXPECT_FALSE(failed.view().has_value());

    Coverage::Result cancelled;
    cancelled.status = Coverage::Status::Cancelled;
    EXPECT_FALSE(cancelled.view().has_value());

    Coverage::Result unsupported;
    unsupported.status = Coverage::Status::UnsupportedSharedAncestor;
    EXPECT_FALSE(unsupported.view().has_value());

    // A malformed plane (size mismatch) is also not forwardable.
    Coverage::Result malformed;
    malformed.status = Coverage::Status::Completed;
    malformed.width = 2;
    malformed.height = 2;
    malformed.pixels = {255};
    EXPECT_FALSE(malformed.completed());
    EXPECT_FALSE(malformed.view().has_value());

    Coverage::Result completed;
    completed.status = Coverage::Status::Completed;
    completed.width = 3;
    completed.height = 1;
    completed.pixels = {0, 128, 255};
    ASSERT_TRUE(completed.completed());
    auto const view = completed.view();
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->pixels, completed.pixels.data());
    EXPECT_EQ(view->stride, 3);
    EXPECT_EQ(view->width, 3);
    EXPECT_EQ(view->height, 1);
    EXPECT_EQ(view->pixels[1], 128u);
}

TEST_F(CoverageTest, SharedAncestorCompositingContextIsRejected)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <g id="parent" opacity="0.5">
  <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none" href="%1"/>
 </g>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    EXPECT_EQ(result.status, Coverage::Status::UnsupportedSharedAncestor);
    EXPECT_TRUE(result.pixels.empty());
}

TEST_F(CoverageTest, FilterAndBlendOnChainAreUnsupportedEffects)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs><filter id="f"><feColorMatrix type="saturate" values="0"/></filter></defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        style="filter:url(#f)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto filtered = Coverage::evaluate(*bitmap, *bitmap);
    EXPECT_EQ(filtered.status, Coverage::Status::UnsupportedEffect);

    bitmap->getRepr()->setAttribute("style", "mix-blend-mode:multiply");
    document->ensureUpToDate();
    auto blended = Coverage::evaluate(*bitmap, *bitmap);
    EXPECT_EQ(blended.status, Coverage::Status::UnsupportedEffect);
}

TEST_F(CoverageTest, SingularMappingIsRejected)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        transform="matrix(0,0,0,0,0,0)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    EXPECT_EQ(result.status, Coverage::Status::SingularMapping);
    EXPECT_FALSE(result.completed());
}

TEST_F(CoverageTest, CoverageIsIndependentOfSourceRgb)
{
    auto const make_uri = [](std::unique_ptr<Pixbuf> pixbuf) {
        return pixbuf ? png_uri(*pixbuf) : std::string{};
    };
    std::string const red_uri = make_uri(solid_pixbuf(SOURCE_W, SOURCE_H, 0xff0000ff));
    std::string const checker_uri = make_uri(pixel_pixbuf(SOURCE_W, SOURCE_H, {
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
        0x000000ff, 0xffffffff, 0x000000ff, 0xffffffff,
        0xffffffff, 0x000000ff, 0xffffffff, 0x000000ff,
    }));
    ASSERT_FALSE(red_uri.empty());
    ASSERT_FALSE(checker_uri.empty());

    auto const template_svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <mask id="mask" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse">
   <rect x="1" y="0" width="2" height="4" fill="#808080"/>
  </mask>
 </defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        mask="url(#mask)" href="%1"/>
</svg>)svg";

    auto evaluate_uri = [&](std::string const &uri) {
        auto const svg = Glib::ustring::compose(template_svg, uri);
        document = SPDocument::createNewDocFromMem(svg.raw());
        EXPECT_TRUE(document);
        document->ensureUpToDate();
        auto *bitmap = image();
        EXPECT_TRUE(bitmap);
        return Coverage::evaluate(*bitmap, *bitmap);
    };

    auto const red = evaluate_uri(red_uri);
    auto const checker = evaluate_uri(checker_uri);
    ASSERT_EQ(red.status, Coverage::Status::Completed);
    ASSERT_EQ(checker.status, Coverage::Status::Completed);
    EXPECT_EQ(red.pixels, checker.pixels);
}

TEST_F(CoverageTest, BranchMustContainSource)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
 <g id="other"><rect width="1" height="1"/></g>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    auto *other = item("other");
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(other);
    auto const result = Coverage::evaluate(*other, *bitmap);
    EXPECT_EQ(result.status, Coverage::Status::InvalidInput);
}

TEST_F(CoverageTest, MissingImageDataIsRejected)
{
    document = SPDocument::createNewDocFromMem(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        href="data:image/png;base64,!!!!"/>
</svg>)svg");
    ASSERT_TRUE(document);
    document->ensureUpToDate();

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    EXPECT_TRUE(result.status == Coverage::Status::MissingImage ||
                result.status == Coverage::Status::SingularMapping)
        << "status=" << static_cast<int>(result.status);
    EXPECT_FALSE(result.completed());
}

TEST_F(CoverageTest, NonExclusiveWrapperIsRejected)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4">
 <g id="wrapper">
  <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none" href="%1"/>
  <rect x="4" y="0" width="2" height="2" fill="#ff0000"/>
 </g>
</svg>)svg");

    auto *bitmap = image();
    auto *wrapper = item("wrapper");
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(wrapper);
    auto const result = Coverage::evaluate(*wrapper, *bitmap);
    EXPECT_EQ(result.status, Coverage::Status::UnsupportedSharedAncestor);
    EXPECT_FALSE(result.completed());
}

// A fractional clip edge must produce area coverage, not a nearest-sample
// decision. Clip [0.25, 3.75] on a 4-wide grid covers 0.75 of columns 0 and 3
// and all of columns 1 and 2.
TEST_F(CoverageTest, FractionalClipEdgeProducesAreaCoverage)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <clipPath id="clip" clipPathUnits="userSpaceOnUse">
   <rect x="0.25" y="0" width="3.5" height="4"/>
  </clipPath>
 </defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        clip-path="url(#clip)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        EXPECT_NEAR(static_cast<double>(at(result, 0, y)), 0.75 * 255.0, 2.0) << "x=0,y=" << y;
        EXPECT_NEAR(static_cast<double>(at(result, 3, y)), 0.75 * 255.0, 2.0) << "x=3,y=" << y;
        EXPECT_EQ(at(result, 1, y), 255u) << "full x=1,y=" << y;
        EXPECT_EQ(at(result, 2, y), 255u) << "full x=2,y=" << y;
    }
}

// preserveAspectRatio="none" with viewport != pixbuf makes c2p a genuine
// non-uniform scale; the userSpaceOnUse clip must map through the inverse of
// pixelToDocumentAffine onto the intrinsic pixel grid.
TEST_F(CoverageTest, ScaledViewportMapsClipThroughPixelToDocumentInverse)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
 <defs>
  <clipPath id="clip" clipPathUnits="userSpaceOnUse">
   <rect x="4" y="0" width="4" height="4"/>
  </clipPath>
 </defs>
 <image id="bitmap" x="0" y="0" width="8" height="4" preserveAspectRatio="none"
        clip-path="url(#clip)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const mapping = bitmap->pixelToDocumentAffine();
    ASSERT_TRUE(mapping);
    // 4x4 pixbuf into an 8x4 viewport -> c2p = scale(2, 1), i2doc = identity.
    EXPECT_TRUE(Geom::are_near(*mapping, Geom::Affine(Geom::Scale(2, 1)), 1e-9));

    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            // clip [4,8) x [0,4) -> pixels [2,4) x [0,4).
            EXPECT_EQ(at(result, x, y), x >= 2 ? 255u : 0u) << "at " << x << "," << y;
        }
    }
}

// A rotated mask-content rect must land on the analytically-derived source
// pixels; this exercises a non-identity affine on authored geometry.
TEST_F(CoverageTest, RotatedMaskContentLandsAnalytically)
{
    // matrix(0,1,-1,0,4,0) maps (x, y) -> (4 - y, x): a 90-degree rotation
    // about the origin followed by translate(4, 0). The bar [0,4)x[0,1) becomes
    // x in (3,4], y in [0,4), i.e. exactly source column 3.
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <mask id="mask" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse">
   <rect x="0" y="0" width="4" height="1" fill="#ffffff"
         transform="matrix(0,1,-1,0,4,0)"/>
  </mask>
 </defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        mask="url(#mask)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            EXPECT_EQ(at(result, x, y), x == 3 ? 255u : 0u) << "at " << x << "," << y;
        }
    }
}

// A reflected branch transform must cancel on the source grid while the source
// viewport scale is non-uniform: only c2p may shape source-pixel coverage.
TEST_F(CoverageTest, ReflectedBranchWithScaledViewportStillRegisters)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
 <defs>
  <mask id="wrapper-mask" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse">
   <rect x="0" y="0" width="4" height="4" fill="#ffffff"/>
  </mask>
 </defs>
 <g id="wrapper" transform="matrix(0,1,1,0,0,0)" mask="url(#wrapper-mask)">
  <image id="bitmap" x="0" y="0" width="8" height="4" preserveAspectRatio="none" href="%1"/>
 </g>
</svg>)svg");

    auto *bitmap = image();
    auto *wrapper = item("wrapper");
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(wrapper);
    auto const mapping = bitmap->pixelToDocumentAffine();
    ASSERT_TRUE(mapping);
    // pixel -> c2p=scale(2,1) -> image-local -> i2doc=reflection.
    EXPECT_TRUE(Geom::are_near(*mapping,
                               Geom::Affine(Geom::Scale(2, 1)) * Geom::Affine(0, 1, 1, 0, 0, 0),
                               1e-9));

    auto const result = Coverage::evaluate(*wrapper, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            // wrapper mask [0,4)^2 in source-local coords -> c2p^-1 -> columns 0,1.
            EXPECT_EQ(at(result, x, y), x < 2 ? 255u : 0u) << "at " << x << "," << y;
        }
    }
}

// Hidden chain items must match native rendering (invoke_show uses isHidden):
// a hidden branch renders nothing, so coverage is completed-zero, never full.
TEST_F(CoverageTest, HiddenWrapperYieldsZeroCoverageNotFull)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <g id="wrapper" style="display:none">
  <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none" href="%1"/>
 </g>
</svg>)svg");

    auto *bitmap = image();
    auto *wrapper = item("wrapper");
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(wrapper);
    ASSERT_TRUE(wrapper->isHidden());
    auto const result = Coverage::evaluate(*wrapper, *bitmap);
    ASSERT_TRUE(result.completed());
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    ASSERT_EQ(result.pixels.size(), static_cast<std::size_t>(SOURCE_W) * SOURCE_H);
    EXPECT_TRUE(std::all_of(result.pixels.begin(), result.pixels.end(),
                            [](unsigned char value) { return value == 0; }));
}

TEST_F(CoverageTest, HiddenSourceYieldsZeroCoverageNotFull)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        style="display:none" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(bitmap->isHidden());
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    ASSERT_EQ(result.pixels.size(), static_cast<std::size_t>(SOURCE_W) * SOURCE_H);
    EXPECT_TRUE(std::all_of(result.pixels.begin(), result.pixels.end(),
                            [](unsigned char value) { return value == 0; }));
}

// Accepted v1 contract: native Inkscape live rendering ignores the mask
// x/y/width/height region rectangle, so the helper must not clamp to it.
TEST_F(CoverageTest, MaskRegionAttributesAreNotAppliedMatchingNative)
{
    load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
 <defs>
  <mask id="mask" maskUnits="userSpaceOnUse" x="0" y="0" width="1" height="1"
        maskContentUnits="userSpaceOnUse">
   <rect x="0" y="0" width="4" height="4" fill="#ffffff"/>
  </mask>
 </defs>
 <image id="bitmap" x="0" y="0" width="4" height="4" preserveAspectRatio="none"
        mask="url(#mask)" href="%1"/>
</svg>)svg");

    auto *bitmap = image();
    ASSERT_TRUE(bitmap);
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    // Region [0,0,1,1] is deliberately not applied: all pixels stay covered.
    for (int y = 0; y < SOURCE_H; ++y) {
        for (int x = 0; x < SOURCE_W; ++x) {
            EXPECT_EQ(at(result, x, y), 255u) << "at " << x << "," << y;
        }
    }
}

// The real F-MASK artwork: inspect coverage and confirm the mask stays intact.
TEST_F(CoverageTest, FmaskCoverageInspectionPreservesMask)
{
    char const *env = g_getenv("INKSCAPE_B02_FMASK_FIXTURE");
    std::vector<std::string> const candidates = {
        env ? env : "",
    };
    std::string fixture;
    for (auto const &candidate : candidates) {
        if (!candidate.empty() && g_file_test(candidate.c_str(), G_FILE_TEST_IS_REGULAR)) {
            fixture = candidate;
            break;
        }
    }
    if (fixture.empty()) {
        GTEST_SKIP() << "F-MASK fixture not available; set INKSCAPE_B02_FMASK_FIXTURE";
    }

    gchar *raw_bytes = nullptr;
    gsize raw_len = 0;
    if (!g_file_get_contents(fixture.c_str(), &raw_bytes, &raw_len, nullptr)) {
        GTEST_SKIP() << "F-MASK fixture could not be read: " << fixture;
    }
    std::string const svg_data(raw_bytes, raw_len);
    g_free(raw_bytes);
    raw_bytes = nullptr;
    document = SPDocument::createNewDocFromMem(svg_data);
    ASSERT_TRUE(document);
    document->ensureUpToDate();

    auto *bitmap = image("image2");
    auto *mask = dynamic_cast<SPMask *>(document->getObjectById("mask1"));
    auto *mask_image = image("image1");
    ASSERT_TRUE(bitmap);
    ASSERT_TRUE(mask);
    ASSERT_TRUE(mask_image);
    ASSERT_TRUE(mask_image->pixbuf);
    ASSERT_EQ(bitmap->getMaskObject(), mask);

    auto const before = sp_repr_save_buf(document->getReprDoc()).raw();
    auto const result = Coverage::evaluate(*bitmap, *bitmap);
    ASSERT_EQ(result.status, Coverage::Status::Completed);
    ASSERT_EQ(result.width, bitmap->pixbuf->width());
    ASSERT_EQ(result.height, bitmap->pixbuf->height());

    // The mask was inspected, never removed.
    EXPECT_EQ(bitmap->getMaskObject(), mask);
    EXPECT_STREQ(bitmap->getRepr()->attribute("mask"), "url(#mask1)");
    EXPECT_EQ(sp_repr_save_buf(document->getReprDoc()).raw(), before);

    // Independent luminance oracle from the decoded mask payload.
    auto const *mask_pixbuf = mask_image->pixbuf.get();
    ASSERT_TRUE(mask_pixbuf);
    int const mw = mask_pixbuf->width();
    int const mh = mask_pixbuf->height();
    ASSERT_EQ(mw, result.width);
    ASSERT_EQ(mh, result.height);
    auto const format = mask_pixbuf->pixelFormat();
    auto const *bytes = mask_pixbuf->pixels();
    auto const stride = mask_pixbuf->rowstride();
    std::size_t compared = 0;
    std::size_t close = 0;
    std::size_t opaque = 0;
    unsigned max_diff = 0;
    for (int y = 0; y < mh; ++y) {
        auto const *row = bytes + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < mw; ++x) {
            auto const *p = row + static_cast<std::size_t>(x) * 4;
            int r = 0, g = 0, b = 0, a = 255;
            if (format == Pixbuf::PF_GDK) {
                r = p[0]; g = p[1]; b = p[2]; a = p[3];
            } else {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
                b = p[0]; g = p[1]; r = p[2]; a = p[3];
#else
                a = p[0]; r = p[1]; g = p[2]; b = p[3];
#endif
            }
            if (a != 255) {
                r = (r * a + 127) / 255;
                g = (g * a + 127) / 255;
                b = (b * a + 127) / 255;
            } else {
                ++opaque;
            }
            unsigned const expected = luminance_to_alpha(r, g, b);
            unsigned const actual = at(result, x, y);
            unsigned const diff = expected > actual ? expected - actual : actual - expected;
            max_diff = std::max(max_diff, diff);
            if (diff <= 1) {
                ++close;
            }
            ++compared;
        }
    }
    ASSERT_GT(compared, 0u);
    // The mask is drawn 1:1 with nearest sampling; allow a single LSB.
    EXPECT_GE(static_cast<double>(close) / compared, 0.99) << "max_diff=" << max_diff;
    EXPECT_LE(max_diff, 4u);
    // A real grayscale mask must actually vary, otherwise the test is vacuous.
    auto const [lo, hi] = std::minmax_element(result.pixels.begin(), result.pixels.end());
    EXPECT_LT(*lo, *hi);
    EXPECT_GT(opaque, 0u);
}

} // namespace
