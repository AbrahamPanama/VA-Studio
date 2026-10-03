// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Explode Bitmap: paired conversion and native-boundary publication (EB5-boundaries).
 * Plan: internal note EXPLODE_BITMAP_IMPLEMENTATION_PLAN, internal note EXPLODE_BITMAP_PLAN v3.2.
 *
 * Copyright 2026 VA Studio authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_EXPLODE_BITMAP_PUBLICATION_H
#define INKSCAPE_UI_EXPLODE_BITMAP_PUBLICATION_H

#include <functional>
#include "ui/explode-bitmap-jobs.h"
#include "util/bitmap-island-budget.h"

class SPDesktop;
class SPDocument;

namespace Inkscape::Bitmap {

class PreparedBitmapCopy;
struct Prepared;

// Interface for the panel's "Convert and explode" (EB5-boundaries owns the implementation). The prepared conversion and
// Explode payloads are borrowed for the synchronous call; their owners keep all storage and reservations.
struct PreparedPair {
    PreparedBitmapCopy const &conversion;
    Prepared const &explode;
};

// Synchronous; the conversion remains Undo-able if the second settlement fails.
// Boundary callbacks are document-owned; never pump a nested loop to wait.
bool publicationBoundaryPending(SPDocument *);
bool deferPublicationBoundary(SPDocument *, std::function<void(SPDocument &)>);
bool deferPublicationBoundary(SPDesktop &, std::function<void(SPDesktop &)>);
// Publisher entry only: consumes the exact paired-call permit once. Other calls
// still require fresh native admission. Does not permit a nested publication.
bool consumePairedPublicationAdmission(SPDesktop &, Prepared const &, Ticket);
Outcome convertAndExplode(SPDesktop &, PreparedPair const &, Ticket);

} // namespace Inkscape::Bitmap

#endif // INKSCAPE_UI_EXPLODE_BITMAP_PUBLICATION_H
