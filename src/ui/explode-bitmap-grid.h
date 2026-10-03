// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_EXPLODE_BITMAP_GRID_H
#define INKSCAPE_UI_EXPLODE_BITMAP_GRID_H
#include <array>
#include "ui/explode-bitmap-target.h"
#include "util/bitmap-input-decode.h"
#include "util/bitmap-islands.h"
class SPImage;
namespace Inkscape::Bitmap {
using GridTransform = std::array<double, 6>;
enum class GridSampling { Exact, Nearest, Good };
// Main-captured OWN clip only. Own opacity is applied separately after alpha.
// Budget outlives all returned buffers. No live/borrowed document state crosses dispatch.
struct GridCoverage {
    PlainBuffer pixels;
    std::uint32_t width = 0, height = 0;
    std::uintptr_t bitmap = 0;
    std::uint64_t generation = 0;
};
enum class GridPhase { Bake, Straighten };
struct Recipe {
    unsigned threshold = 0, softness = 0;
    unsigned faintFloor = 5; // integer percent, 0–25; applied before T/S, including Refine OFF
    bool bypassAlpha = false, pixelated = false; // bypassAlpha skips T/S only
    bool alphaPrepared = false; // internal: source alpha already processed; do not apply twice
    GridCoverage const *coverage = nullptr; // immutable, retained until worker completion
    JobWork *work = nullptr; // caller's shared job meter; local meter if absent
    AllocationFault *fault = nullptr;
    void (*checkpoint)(GridPhase, void *) = nullptr; // caller-local deterministic test latch
    void *checkpointData = nullptr;
};
// Shared source-alpha transform for proxy, Apply and the exact Explode grid.
AlphaLut recipeAlphaLut(Recipe const &) noexcept;
// Plain off-document input captured from the immutable conversion candidate.
struct CandidateGridInput {
    TargetSnapshot geometry; // synthetic geometry only; never a live dependency token
    DecodedRaster raster;
    std::uint64_t identity = 0;
};
struct FinalGrid {
    PlainBuffer pixels, profile;
    std::uint32_t width = 0, height = 0;
    GridTransform pixelToDocument{}, pixelToParent{};
    double dpiX = 0, dpiY = 0;
    GridSampling sampling = GridSampling::Exact; // Nearest/Good disclose resampling
    std::uint64_t recipeHash = 0, candidateIdentity = 0;
    unsigned candidateThreshold = 0, candidateSoftness = 0, candidateFaintFloor = 5;
    bool candidateBypassAlpha = false;
    RgbaView view() const noexcept;
};
// Main only: caller revalidates the target before capture/dispatch. Captures no tone/opacity.
Result<GridCoverage> captureGridCoverage(SPImage &, TargetSnapshot const &, Budget &, Stop = {}) noexcept;
// Worker: tone -> faint floor -> T/S alpha -> own clip -> own opacity -> optional straightening.
Result<FinalGrid> prepareGrid(TargetSnapshot const &, DecodedRaster const &, Recipe, Budget &, Stop = {}) noexcept;
Result<FinalGrid> prepareGrid(CandidateGridInput const &, Recipe, Budget &, Stop = {}) noexcept;
// Allocation-free SVG matrix serialization with round-trip precision. Returns bytes written,
// or zero on invalid geometry/insufficient capacity. Integer crops share this SAME matrix.
std::size_t serializeGridTransform(GridTransform const &, char *, std::size_t) noexcept;
} // namespace Inkscape::Bitmap
#endif
