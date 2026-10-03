// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_IO_ARTWORK_LIBRARY_ENCODING_H
#define INKSCAPE_IO_ARTWORK_LIBRARY_ENCODING_H

#include "artwork-library-catalog.h"

namespace Inkscape::IO::ArtworkLibrary {

// Encode one immutable catalog revision as a native ZIP. Copies the snapshot
// before callbacks, reads artwork one entry at a time and verifies its hash.
// Missing/damaged authoritative artwork fails the whole operation. Disposable
// previews are deliberately omitted; session trash is NOT durable recovery.
//
// This is container/integrity encoding, NOT SVG safety validation and NOT file
// publication. The future storage service must validate artwork, retain recovery
// data, check conflicts and atomically publish. No document/GTK/filesystem access.
Bytes encode_catalog(CatalogSnapshot snapshot, ArchiveLimits archive_limits = {},
                     ManifestLimits manifest_limits = {}, Cancelled cancelled = {});

} // namespace Inkscape::IO::ArtworkLibrary
#endif
