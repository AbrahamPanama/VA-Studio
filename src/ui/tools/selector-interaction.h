// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_SELECTOR_INTERACTION_H
#define INKSCAPE_UI_TOOLS_SELECTOR_INTERACTION_H

namespace Inkscape::UI::Tools {

enum class SelectorInteractionState {
    Idle,
    PointerPressed,
    MarqueeActive,
    TransformActive,
    CanceledAwaitingRelease
};

enum class MarqueeBehavior {
    Undetermined,
    Enclose,
    Crossing
};

/**
 * Classify a rectangular marquee from its horizontal movement in screen pixels.
 *
 * Motion inside the dead zone keeps the previous classification. This makes
 * almost-vertical drags stable while still allowing an intentional reversal to
 * change between window (enclose) and crossing selection.
 */
constexpr MarqueeBehavior classify_marquee(double horizontal_delta_screen,
                                            MarqueeBehavior previous,
                                            double dead_zone_px = 3.0)
{
    auto const dead_zone = dead_zone_px < 0.0 ? 0.0 : dead_zone_px;
    if (horizontal_delta_screen >= dead_zone) {
        return MarqueeBehavior::Enclose;
    }
    if (horizontal_delta_screen <= -dead_zone) {
        return MarqueeBehavior::Crossing;
    }
    return previous;
}

/**
 * Return whether a directional marquee must use overlap rather than full
 * containment. Keeping this mapping explicit prevents the drawing mode and
 * the selection query from drifting apart.
 */
constexpr bool marquee_selects_partial_overlap(MarqueeBehavior behavior)
{
    return behavior == MarqueeBehavior::Crossing;
}

} // namespace Inkscape::UI::Tools

#endif // INKSCAPE_UI_TOOLS_SELECTOR_INTERACTION_H
