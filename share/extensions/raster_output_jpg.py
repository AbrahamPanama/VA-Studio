#!/usr/bin/env python3
"""
Convert PNG to Jpeg using Raster Output extension.
"""

import inkex
from PIL import PngImagePlugin

# Shared catalog bound: src/io/export-color-profiles.h. Fits JPEG's 255 APP2
# segments and TIFF's uint32 ICC length; do not disable PNG decompression limits.
EXPORT_PROFILE_MAX_SIZE = 15 * 1024 * 1024


class JpegOutput(inkex.RasterOutputExtension):
    multi_inx = True  # XXX Remove this after refactoring

    def add_arguments(self, pars):
        pars.add_argument("--tab")
        pars.add_argument("--quality", type=int, default=90)
        pars.add_argument("--progressive", type=inkex.Boolean, default=False)

    def load(self, stream):
        # iCCP is read by Image.open(), before save(). Only these conversion
        # processes admit catalog-sized chunks; restore the generic PNG limit.
        previous = PngImagePlugin.MAX_TEXT_CHUNK
        try:
            PngImagePlugin.MAX_TEXT_CHUNK = EXPORT_PROFILE_MAX_SIZE
            super().load(stream)
        finally:
            PngImagePlugin.MAX_TEXT_CHUNK = previous
        if len(self.img.info.get("icc_profile") or b"") > EXPORT_PROFILE_MAX_SIZE:
            raise inkex.AbortExtension(
                "ICC profile exceeds the maximum supported size of 15 MiB."
            )

    def save(self, stream):
        self.img.convert("RGB").save(
            stream,
            format="jpeg",
            quality=self.options.quality,
            icc_profile=self.img.info.get("icc_profile"),
            dpi=self.img.info["dpi"],
            progressive=self.options.progressive,
        )


if __name__ == "__main__":
    JpegOutput().run()
