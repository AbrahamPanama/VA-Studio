// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TEXT_PARAGRAPH_TOOLS_H
#define INKSCAPE_UI_TEXT_PARAGRAPH_TOOLS_H

#include <vector>

#include "text-editing.h"

class SPDocument;
class SPItem;
class SPObject;
class SPText;
class SPCSSAttr;

namespace Inkscape::UI {

enum class TextListMode { None, Bulleted, Numbered };

// Build the text-anchor/text-align CSS for a native integer alignment mode:
// 0 = left, 1 = center, 2 = right, 3 = justify.
SPCSSAttr *cssForTextAlignment(int align_mode, int direction);

// Apply a native integer alignment mode (0 = left, 1 = center, 2 = right,
// 3 = justify) to a single ordinary SPText, moving the first XY to preserve the
// bounding box. Returns true when the alignment CSS or the anchor point changed;
// an already-matching request performs no mutation.
bool applyNativeTextAlignment(SPText &text, int align_mode);

// Set css on the object's own style and, when unset_descendants is true, unset
// those same properties on descendants so they inherit the root value instead.
void setTextRootStyleAndUnsetDescendants(SPObject &object, SPCSSAttr *css,
                                         bool unset_descendants = true);

// Inverse-transform wrapper for an outer <text>/<flowRoot> style: merge css,
// compensate for the item's accumulated parent transform, then apply the root
// style and unset descendant overrides.
void applyDocumentTextStyle(SPItem &item, SPCSSAttr *css);

// Native inner line-height helper: parse the wrappers created for a line-height
// change over a text subselection. It wraps direct text children into
// sodipodi:role="line" tspans, collects the top-level containers spanned by
// start/end, unindents their children back to item and applies the container
// style to orphaned runs, preserving styles, geometry, roles and order.
//
// Caller obligations: item must be an up-to-date SPText/SPFlowtext whose text
// layout was built for the same document; start and end must be iterators into
// that layout and are validated here. Hidden shape/overflow state is toggled
// internally and restored. The function may rebuild the item's layout.
void prepareInnerText(SPItem &item,
                      Text::Layout::iterator &start,
                      Text::Layout::iterator &end);

// Native inner line-height helper: capture item's own line-height, push that
// value onto its direct SPItem children, then set item's line-height to 0.
//
// Caller obligations: item must be the text item whose style holds the pre-change
// root line-height; the caller keeps ownership of the surrounding UI selection
// swap (subselection_wrap_toggle), default-style application and Undo.
void prepareInnerRootLineHeight(SPItem &item);

TextListMode paragraphListMode(SPObject const &paragraph);
unsigned paragraphListStart(SPObject const &paragraph);
bool setParagraphListMode(SPDocument &document,
                          std::vector<SPObject *> const &paragraphs,
                          TextListMode mode,
                          unsigned start = 1);

unsigned paragraphDropCapLines(SPObject const &paragraph);
bool setParagraphDropCapLines(SPDocument &document,
                              std::vector<SPObject *> const &paragraphs,
                              unsigned lines);

} // namespace Inkscape::UI

#endif // INKSCAPE_UI_TEXT_PARAGRAPH_TOOLS_H
