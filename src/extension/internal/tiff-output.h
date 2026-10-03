// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef EXTENSION_INTERNAL_TIFF_OUTPUT_H
#define EXTENSION_INTERNAL_TIFF_OUTPUT_H

#include "extension/implementation/implementation.h"

namespace Inkscape::Extension::Internal {

class TiffOutput final : public Implementation::Implementation
{
public:
    void export_raster(Output *module, SPDocument const *doc, std::string const &png_file,
                       gchar const *filename) override;

    static void init();
    static std::string output_profile_path();
};

} // namespace Inkscape::Extension::Internal

#endif // EXTENSION_INTERNAL_TIFF_OUTPUT_H
