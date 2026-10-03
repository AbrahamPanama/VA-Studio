# coding=utf-8
"""
Test raster editing extensions in inkex
"""

import inkex
from inkex.tester import TestCase

from PIL import Image


class RasterSupportTest(TestCase):
    """Test raster formats are supported"""

    def setUp(self):
        Image.registered_extensions()

    def test_supported_open(self):
        """Raster formats can be opened"""
        for fmt in (
            "BMP",
            "PCX",
            "GIF",
            "PNG",
            "JPEG2000",
            "ICO",
            "TIFF",
            "JPEG",
            "PPM",
            "WEBP",
            "XPM",
        ):
            self.assertIn(fmt, Image.OPEN)

    def test_supported_save(self):
        """Raster formats can be saved"""
        for fmt in ("PNG", "GIF", "TIFF", "JPEG", "WEBP"):
            self.assertIn(fmt, Image.SAVE)
