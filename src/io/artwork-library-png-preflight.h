// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_PNG_PREFLIGHT_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_PNG_PREFLIGHT_H
#include "artwork-library-svg-preflight.h"
namespace Inkscape::IO::ArtworkLibrary::detail {
// Internal adapter, not an independent public admission token.
std::size_t preflight_png(Bytes const &, std::size_t pixel_limit, Cancelled const &);
}
#endif
