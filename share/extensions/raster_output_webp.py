#!/usr/bin/env python3
"""
Convert PNG to WebP using Raster Output extension.
"""

import inkex
from PIL import PngImagePlugin, features, __version__ as pillow_version

# Shared catalog bound: src/io/export-color-profiles.h. Fits JPEG's 255 APP2
# segments and TIFF's uint32 ICC length; do not disable PNG decompression limits.
EXPORT_PROFILE_MAX_SIZE = 15 * 1024 * 1024


class WebpOutput(inkex.RasterOutputExtension):
    def add_arguments(self, pars):
        pars.add_argument("--tab")
        pars.add_argument("--quality", type=int, default=80)
        pars.add_argument("--speed", type=int, default=0)
        pars.add_argument("--lossless", type=inkex.Boolean, default=True)

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
        self.check_icc_support()

    def check_icc_support(self):
        if not self.img.info.get("icc_profile"):
            return
        # Pillow 12 removed webp_mux: mux is mandatory when webp is present.
        # Earlier builds (notably 9.5) can encode but silently discard ICC.
        feature = "webp" if int(pillow_version.split(".")[0]) >= 12 else "webp_mux"
        if not features.check(feature):
            raise inkex.AbortExtension(
                "Cannot export WebP with an ICC profile: this Pillow build lacks "
                "WebP mux support. Install Pillow with WebP mux support."
            )

    def save(self, stream):
        # Also protect direct callers. load() checks before Inkex opens output.
        self.check_icc_support()
        self.img.save(
            stream,
            format="webp",
            quality=self.options.quality,
            icc_profile=self.img.info.get("icc_profile"),
            lossless=self.options.lossless,
            method=self.options.speed,
        )


if __name__ == "__main__":
    WebpOutput().run()
