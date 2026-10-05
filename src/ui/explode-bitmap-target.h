// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_EXPLODE_BITMAP_TARGET_H
#define INKSCAPE_UI_EXPLODE_BITMAP_TARGET_H
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "display/bitmap-tone.h"
#include "util/bitmap-island-budget.h"
class SPDesktop;
class SPDocument;
namespace Inkscape::UI::Tools { class ToolBase; }
namespace Inkscape::Bitmap {
// Camera and non-text tools preserve the captured bitmap session.
bool compatibleExplodeBitmapTool(UI::Tools::ToolBase const *);
// Read-only preflight only. Conversion always requires its own warning/confirmation.
enum class Intent { Explode, ApplyAdjustment, ConversionCandidate, BitmapCopy };
enum class TargetMode { SingleBitmap, CollectiveConversion };
enum class Supportability { Supported, ConversionRequired, Refused };
enum class Refusal {
    EmptySelection, TextTool, Hidden, Locked, MissingSource, LinkedSource, InvalidIntake,
    InvalidTransform, CroppingSlice, ZeroOpacity, AncestorEffect, OwnMask, OwnFilter, MalformedTone,
    UnsupportedClip, ResourceTarget, CloneReference, HrefReference, UrlReference,
    CssDependency, IdDependency, ResourceDependency, CrossParent, Interleaved, MainThreadOnly
};
struct TargetReason {
    Refusal reason;
    std::uintptr_t identity = 0;
    std::string id;
    char const *diagnostic = "";
    Outcome detail;
    TargetReason(Refusal r, std::uintptr_t key, std::string name, char const *text)
        : reason(r), identity(key), id(std::move(name)), detail(Status::failed, text) {
        diagnostic = detail.diagnostic;
    }
    TargetReason(TargetReason const &o) : TargetReason(o.reason, o.identity, o.id, o.diagnostic) {}
    TargetReason(TargetReason &&o) noexcept
        : TargetReason(o.reason, o.identity, std::move(o.id), o.diagnostic) {}
    TargetReason &operator=(TargetReason o) noexcept {
        reason = o.reason; identity = o.identity; id = std::move(o.id);
        detail = o.detail; diagnostic = detail.diagnostic; return *this;
    }
};
// Values only: identities are opaque and must NEVER be dereferenced by a worker.
struct TargetContext {
    std::uintptr_t identity = 0, parent = 0, clipResource = 0, maskResource = 0, filterResource = 0;
    std::string id;
    std::array<double, 6> itemToDocument{}, pixelToItem{};
    std::array<double, 4> viewport{};
    double opacity = 1;
    bool hidden = false, locked = false, clone = false, referenced = false;
    bool clip = false, mask = false, filter = false;
    Filters::BitmapToneSettings tone;
};
struct TargetSnapshot {
    // desktop is the opaque publication owner key: SPDesktop or document context.
    // Workers must never dereference it.
    std::uintptr_t document = 0, desktop = 0, bitmap = 0, destinationParent = 0;
    unsigned long documentSerial = 0;
    std::uint64_t incarnation = 0, generation = 0;
    Intent intent = Intent::Explode;
    TargetMode mode = TargetMode::SingleBitmap;
    Supportability supportability = Supportability::Refused;
    std::vector<std::uintptr_t> selection, roots;
    std::vector<TargetContext> contexts; // roots, descendants and ancestor contexts, once each
    std::vector<TargetReason> refusals;  // all incompatible objects, never silently excluded
    std::vector<std::string> conversionReasons;
    std::size_t covered = 0, destinationSlot = 0;
    // Own opacity is baked later; ancestor opacity is retained at destinationParent once.
    double effectiveOpacity = 1, retainedAncestorOpacity = 1;
};
// Main-thread APIs. No document, selection, history or persisted-preview writes.
// Refused Result still carries roots and individual reasons for panel diagnostics.
Result<TargetSnapshot> resolve(SPDesktop &, Intent);
bool valid(TargetSnapshot const &, SPDocument &);
} // namespace Inkscape::Bitmap
#endif
