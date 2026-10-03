// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_ARTWORK_LIBRARY_PLACEMENT_H
#define INKSCAPE_ARTWORK_LIBRARY_PLACEMENT_H
#include "io/artwork-library-insert.h"
class SPDesktop;
namespace Inkscape::UI::Dialog {
struct ArtworkDrop { IO::ArtworkLibrary::ValidatedSvg svg; }; // owning boxed in-process DND; never clipboard
IO::ArtworkLibrary::InsertedArtwork place_library_artwork(
    SPDesktop &, IO::ArtworkLibrary::ValidatedSvg, Geom::Point viewport_origin);
}
#endif
