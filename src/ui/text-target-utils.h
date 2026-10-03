// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TEXT_TARGET_UTILS_H
#define INKSCAPE_UI_TEXT_TARGET_UTILS_H

#include <vector>

namespace Inkscape::Text {
class Layout;
}

class SPCSSAttr;
class SPItem;

namespace Inkscape::UI {

/** Collect editable roots once, descending only into ordinary SVG groups.
 * References/resources and inherited hidden/locked subtrees are excluded.
 * The input and the document are never modified.
 */
std::vector<SPItem *> collectTextItems(std::vector<SPItem *> const &selection);
bool isEligibleTextItem(SPItem const *item);

struct TextLogicalRange {
    unsigned first = 0;
    unsigned last = 0;

    bool operator==(TextLogicalRange const &) const = default;
};

/**
 * Return every logical paragraph touched by [first, last). A collapsed range
 * targets the paragraph containing the caret. Returned ranges are half-open,
 * stable character indices suitable for rebuilding Layout iterators later.
 */
std::vector<TextLogicalRange> paragraphRanges(Text::Layout const &layout,
                                              unsigned first, unsigned last);

/** Apply CSS to logical ranges from the end, rebuilding iterators each time. */
bool applyStyleToLogicalRanges(SPItem *item, std::vector<TextLogicalRange> const &ranges,
                               SPCSSAttr const *css);

} // namespace Inkscape::UI

#endif // INKSCAPE_UI_TEXT_TARGET_UTILS_H
