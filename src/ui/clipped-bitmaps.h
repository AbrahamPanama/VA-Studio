// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CDR-1: convert bitmaps held in clip masks (CorelDRAW PowerClips) to plain
 * images.
 *
 * Owner decisions (2026-09-27): only clips whose visible content is bitmaps
 * (clips that also hold vectors or text stay unchanged and are counted);
 * each clip becomes its own image, never one image joining several; the
 * options default to the last Make a Bitmap Copy settings; on opening a
 * CorelDRAW file the conversion is part of opening and cannot be undone.
 * The Object menu command does the same for any open document in one Undo
 * step. Each image replaces its clip in place (Make a Bitmap Copy, replace
 * mode, which renders the clip exactly as displayed).
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_CLIPPED_BITMAPS_H
#define INKSCAPE_UI_CLIPPED_BITMAPS_H

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <sigc++/scoped_connection.h>

#include "document-undo.h"
#include "object/object-set.h"

class SPDesktop;
class SPDocument;
class SPItem;

namespace Inkscape::UI::ClippedBitmaps {

struct Scan {
    /// Outermost clipped or masked items whose visible content is only bitmaps.
    std::vector<SPItem *> convertible;
    /// Clipped or masked items that hold a bitmap together with vectors or text.
    int with_vectors = 0;
    /// Bitmap-only clips under a group with transparency, a filter, a clip, a
    /// mask or a path effect: their image would be placed outside that group
    /// and could then cover its other content, so they are left unchanged.
    int in_effect_groups = 0;
    /// Ids of the convertible items (stable across a dialog's nested main loop).
    std::vector<std::string> ids() const;
};

/// Visible, unlocked layer content only; definitions and layers themselves
/// are never targets.
Scan scan(SPDocument &document);

/// Replace each item named in \a ids by its own image (options.keep_original is
/// ignored) and remove the clip paths and masks nothing uses any more.
/// \a undoable: one Undo step for all. Otherwise (opening) the history is
/// cleared afterwards, so nothing before or at the conversion can be undone.
/// Does nothing while a live preview or drag owns the history. Returns how
/// many were converted. Runs a Conversion to the end in one call.
int convert(SPDocument &document, std::vector<std::string> const &ids, BitmapCopyOptions options, bool undoable);

/**
 * The work of convert(), one item per step, for a caller that shows
 * progress between items: at a high resolution one item can take seconds.
 * It holds a rollbackable interaction for its whole life: meanwhile no live
 * preview or drag can start, Undo is refused, and closing the document rolls
 * the unfinished conversion back. finish() records the single Undo step (or,
 * when not undoable, folds everything into the opened state). The
 * interaction does not stop another action from recording a history step
 * between two steps (it would take the images converted so far into its own
 * step); the conversion then stops there and finish() still reports and
 * tidies what it converted.
 */
class Conversion
{
public:
    Conversion(SPDocument &document, std::vector<std::string> ids, BitmapCopyOptions options, bool undoable);
    ~Conversion();
    Conversion(Conversion const &) = delete;
    Conversion &operator=(Conversion const &) = delete;

    /// False when a live preview or drag owned the history: nothing happens.
    bool started() const { return _interaction.has_value(); }
    int total() const { return static_cast<int>(_ids.size()); }
    /// Items handled so far, converted or skipped.
    int processed() const { return _next; }
    /// Convert the next item. False when none is left, or when the document
    /// closed or the interaction ended from outside.
    bool step();
    /// After the last step: remove the clips nothing uses, then record the
    /// Undo step. Returns how many items are converted in the document.
    /// Idempotent.
    int finish();

private:
    bool usable() const;
    void remove_unused_clips();
    int images_present() const;

    SPDocument *_document;
    std::vector<std::string> _ids; ///< topmost first
    BitmapCopyOptions _options;
    bool _undoable;
    std::optional<DocumentUndo::RollbackableInteraction> _interaction;
    sigc::scoped_connection _document_gone;
    std::set<std::string> _clips;
    std::vector<std::string> _images; ///< ids of the images made so far
    int _next = 0;
    int _converted = 0;
    std::optional<int> _result;
};

/// True for the CorelDRAW formats VA Studio opens (.cdr, .cdt, .ccx, .cmx).
bool is_coreldraw_file(std::string const &path);

/// After a CorelDRAW file opened in \a desktop: if it has clipped bitmaps,
/// ask and convert (not undoable). Runs once the window is up.
void offer_after_open(SPDesktop *desktop);

/// Object menu command: scan the active document, ask, convert in one Undo step.
void convert_in_desktop(SPDesktop *desktop);

} // namespace Inkscape::UI::ClippedBitmaps

#endif // INKSCAPE_UI_CLIPPED_BITMAPS_H
