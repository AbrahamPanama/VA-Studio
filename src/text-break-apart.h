// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BRK-1: Break Apart like CorelDRAW. One command that treats each selected
 * object by its kind: a text goes one level down the hierarchy (lines, then
 * words, then letters) as separate editable texts that keep their fonts,
 * colours and exact positions; a path breaks into its subpaths (the existing
 * Path > Break Apart). Other objects are left untouched and reported. One
 * Undo step for the whole selection. Owner decision 2026-09-29
 * (internal note OWNER_REQUESTS, BRK-1).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_TEXT_BREAK_APART_H
#define INKSCAPE_TEXT_BREAK_APART_H

#include <cstddef>
#include <vector>

class SPItem;
class SPText;
namespace Inkscape { class ObjectSet; }

namespace Inkscape::TextBreakApart {

/// The level a text breaks to: the first of lines, words, letters that has
/// more than one piece. None: nothing to break (one letter, empty, or an
/// unsupported text such as text on a path, vertical or right-to-left text).
enum class Level { None, Lines, Words, Letters };

Level level_of(SPText const *text);

/// Break \a text one level down. The pieces are new <text> objects in the
/// text's parent, at its position; the text itself is deleted. Returns the
/// pieces, or nothing when the level is None (the text is then untouched).
/// No Undo step is recorded; the caller owns the transaction.
std::vector<SPItem *> break_one_level(SPText *text);

struct Outcome {
    std::size_t texts = 0;   ///< texts broken
    std::size_t paths = 0;   ///< paths broken into subpaths
    std::size_t skipped = 0; ///< objects of another kind, or texts with nothing to break
    std::size_t unmatched = 0; ///< of the skipped: texts whose pieces could not match the original drawing
    std::vector<SPItem *> pieces; ///< the new objects
};

/// Break every text and path in \a set by its kind and select the pieces
/// together with the untouched objects. One Undo step when anything changed;
/// a selection with nothing to break is a no-op (no history entry).
Outcome break_apart(ObjectSet &set);

} // namespace Inkscape::TextBreakApart

#endif // INKSCAPE_TEXT_BREAK_APART_H
