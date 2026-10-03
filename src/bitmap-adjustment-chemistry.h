// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_BITMAP_ADJUSTMENT_CHEMISTRY_H
#define INKSCAPE_BITMAP_ADJUSTMENT_CHEMISTRY_H

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "display/bitmap-tone.h"
#include "util/operation-targets.h"

class SPFilter;
class SPFilterPrimitive;
class SPImage;
class SPItem;

namespace Inkscape {
class DrawingItem;
}

namespace Inkscape::Filters {
class Filter;
class FilterPrimitive;
}

namespace Inkscape::BitmapAdjustments {

inline constexpr char TONE_MARKER_ATTRIBUTE[] = "inkscape:bitmap-adjustment";
inline constexpr char TONE_MARKER_VALUE[] = "tone-v1";

[[nodiscard]] bool is_managed_tone_primitive(SPFilterPrimitive const *primitive);
[[nodiscard]] SPFilterPrimitive *find_managed_tone_primitive(SPFilter *filter);
[[nodiscard]] std::optional<Filters::BitmapToneSettings> query_tone(SPItem const *item);

/** Apply settings to SVG/XML without creating an Undo entry. Returns true only on a real change. */
bool apply_tone(SPItem *item, Filters::BitmapToneSettings const &settings);

/** Remove only the managed tone primitive, preserving every unrelated filter. */
bool reset_tone(SPItem *item);

/** An image whose pixels are decoded and whose source is not missing. */
[[nodiscard]] bool usable_bitmap(SPImage const *image);

/** An image whose href is an embedded data: URI. */
[[nodiscard]] bool is_embedded(SPImage const *image);

/** The item's managed tone settings, or neutral settings when it has none. */
[[nodiscard]] Filters::BitmapToneSettings canonical_tone(SPItem const *item);

/**
 * Tone targets of a selection under the legacy per-composite-root policy (a documented SELECTION_CONTRACT
 * exception): `available(item) == false` -> Unavailable; an SPImage without a usable bitmap -> MissingSource;
 * otherwise Eligible; descendants and clones already covered by a selected root are counted as covered.
 * Read-only. The GUI passes a desktop-based availability test; the command line passes a document-based one.
 */
[[nodiscard]] Util::OperationTargets<SPItem>
resolve_tone_targets(std::vector<SPItem *> const &selected, std::function<bool(SPItem *)> const &available);

/** Per-property aggregate over targets as the panel shows it: the first target's value, mixed when another differs. */
[[nodiscard]] std::array<Filters::BitmapToneAggregateValue, Filters::BITMAP_TONE_PROPERTY_COUNT>
aggregate_tone(std::vector<SPItem *> const &targets);

/** Build the exact renderer primitive used for transient canvas previews. */
[[nodiscard]] std::unique_ptr<Filters::FilterPrimitive>
build_tone_renderer_primitive(Filters::BitmapToneSettings const &settings);

/**
 * Build a view-local renderer containing every canonical primitive except the managed tone,
 * optionally followed by a transient tone. A null result means no filter is needed.
 */
[[nodiscard]] std::unique_ptr<Filters::Filter>
build_tone_preview_renderer(SPItem const *item, DrawingItem *drawing_item,
                            std::optional<Filters::BitmapToneSettings> const &settings,
                            bool show_clipping = false);

} // namespace Inkscape::BitmapAdjustments

#endif // INKSCAPE_BITMAP_ADJUSTMENT_CHEMISTRY_H
