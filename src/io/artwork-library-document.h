// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_DOCUMENT_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_DOCUMENT_H

#include "artwork-library-archive.h"
#include "ui/clipboard.h"

namespace Inkscape::IO::ArtworkLibrary {

struct StagedSelection {
    Bytes svg;
    double width_mm;
    double height_mm;
    std::vector<std::string> warnings;
};

// Native selection only, not an arbitrary SVG-file validator or safe-execution
// certificate. No Catalog change, disk publication, document Undo or clipboard
// operation. The caller holds the source-operation lease through the entire
// synchronous call, including callbacks: source must remain alive, up-to-date and
// unchanged on its owning thread. Only the returned bytes/warnings may outlive it.
StagedSelection stage_selection(ObjectSet &source,
    UI::SelectionCopyLimits const &limits = {}, Cancelled cancelled = {});

} // namespace Inkscape::IO::ArtworkLibrary
#endif
