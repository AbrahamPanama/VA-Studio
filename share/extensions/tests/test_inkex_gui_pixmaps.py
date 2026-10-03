# coding=utf-8
#
# Copyright 2022 Martin Owens <doctormo@geek-2.com>
#
# This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>
#
"""
Test pixmap and image handling from various sources.
"""

import os
import sys
import time
import pytest

from inkex.tester import TestCase
from inkex.utils import DependencyError

try:
    from inkex.gui.tester import MainLoopProtection
    from inkex.gui.pixmap import (
        PixmapLoadError,
        PixmapFilter,
        OverlayFilter,
        PadFilter,
        SizeFilter,
        SIZE_ASPECT,
        SIZE_ASPECT_GROW,
        SIZE_ASPECT_CROP,
        SIZE_STRETCH,
    )
    from inkex.gui import PixmapManager
except DependencyError:
    PixmapFilter = object
    PixmapManager = None


class NullFilter(PixmapFilter):
    required = ["underpants"]


@pytest.mark.skipif(PixmapManager is None, reason="PyGObject is required")
class GtkPixmapsTest(TestCase):
    """Tests all the pixmaps functionality"""

    def construct_manager(self, **kwargs):
        """Create a gtk app based on some inputs"""
        return type(
            "_PixMan",
            (PixmapManager,),
            {
                "pixmap_dir": self.datadir(),
                "missing_image": kwargs.pop("missing_image", None),
                "default_image": kwargs.pop("default_image", None),
                **kwargs,
            },
        )

    def test_filter(self):
        """Test building filters and errors"""
        null_filter = NullFilter(underpants=True)
        self.assertRaises(NotImplementedError, null_filter.filter, "not")
        self.assertRaises(ValueError, OverlayFilter().filter, None)

    def test_manager_filter_overlay(self):
        """Test overlay filter in use"""
        pixmaps = self.construct_manager(filters=[OverlayFilter])("svg")
        self.assertTrue(pixmaps.get("colors.svg", overlay="application-default-icon"))

    def test_manger_filter_size(self):
        """Test resizing a file pixmap"""
        pixmaps = self.construct_manager(filters=[SizeFilter])("svg", size=150)
        ret = pixmaps.get("colors.svg")
        self.assertEqual(ret.get_width(), 150)
        self.assertEqual(ret.get_height(), 50)
        self.assertRaises(PixmapLoadError, pixmaps.load_from_name, "no-file.svg")
        self.assertRaises(
            PixmapLoadError,
            pixmaps.load_from_name,
            os.path.join(self.datadir(), "ui", "window-test.ui"),
        )

    def test_missing_image(self):
        pixmaps = PixmapManager("svg", filters=[SizeFilter(size=25)])
        img_a = pixmaps.get("NeverExisted.svg")
        img_b = pixmaps.get(PixmapManager.missing_image)
        self.assertEqual(img_a.get_width(), 25)
        self.assertPixbuf(img_a, img_b)

    def test_overlay_filter(self):
        """Test overlay filter"""
        pixmaps = self.construct_manager()("svg")
        start = pixmaps.get("gradient_with_mixed_offsets.svg")

        # 1. Simple overlay
        ret = OverlayFilter().filter(start, manager=pixmaps, overlay="colors.svg")
        comp = pixmaps.get("img/color_overlay_a.png")
        self.assertPixbuf(ret, comp)

        # 2. Overlay at bottom
        ret = OverlayFilter(position=1).filter(
            start, manager=pixmaps, overlay="colors.svg"
        )
        comp = pixmaps.get("img/color_overlay_b.png")
        self.assertPixbuf(ret, comp)

    def test_pad_filter(self):
        """Test padding filter"""
        pixmaps = self.construct_manager()("svg")
        start = pixmaps.get("colors.svg")

        # 1. Add no padding
        ret = PadFilter(size=(300, 100)).filter(start)
        self.assertPixbuf(ret, start)

        # 2a. Add padding at top-left
        ret = PadFilter(size=300, padding=0.0).filter(start)
        comp = pixmaps.get("img/color_pad_a.png")
        self.assertPixbuf(ret, comp)

        # 12b. Add padding at top-left
        ret = PadFilter(size=300, padding=1.0).filter(start)
        comp = pixmaps.get("img/color_pad_b.png")
        self.assertPixbuf(ret, comp)

        # 12c. Add padding at top-left
        ret = PadFilter(size=300, padding=0.5).filter(start)
        comp = pixmaps.get("img/color_pad_c.png")
        self.assertPixbuf(ret, comp)

        # 3. Take image and pad to 1px x 150px
        ret = PadFilter(size=(1, 150)).filter(start)
        self.assertEqual((ret.get_width(), ret.get_height()), (300, 150))

    def test_size_filter(self):
        """Test the size pixbuf filter"""
        pixmaps = self.construct_manager()("svg")
        start = pixmaps.get("colors.svg")
        self.assertEqual((start.get_width(), start.get_height()), (300, 100))

        ret = SizeFilter().filter(start)
        self.assertEqual((ret.get_width(), ret.get_height()), (300, 100))

        ret = SizeFilter(size=600, resize_mode=SIZE_ASPECT).filter(start)
        self.assertEqual((ret.get_width(), ret.get_height()), (300, 100))
        self.assertEqual((start.get_width(), start.get_height()), (300, 100))

        ret = SizeFilter(size=60, resize_mode=SIZE_ASPECT).filter(start)
        self.assertEqual((ret.get_width(), ret.get_height()), (60, 20))
        self.assertEqual((start.get_width(), start.get_height()), (300, 100))

        ret = SizeFilter(size=600, resize_mode=SIZE_ASPECT_GROW).filter(start)
        self.assertEqual((ret.get_width(), ret.get_height()), (600, 200))
        self.assertEqual((start.get_width(), start.get_height()), (300, 100))

        ret = SizeFilter(size=60, resize_mode=SIZE_ASPECT_CROP).filter(start)
        self.assertEqual((ret.get_width(), ret.get_height()), (180, 60))
        self.assertEqual((start.get_width(), start.get_height()), (300, 100))

        ret = SizeFilter(size=600, resize_mode=SIZE_STRETCH).filter(start)
        self.assertEqual((ret.get_width(), ret.get_height()), (600, 600))
        self.assertEqual((start.get_width(), start.get_height()), (300, 100))

    def test_load_file(self):
        """Test loading a filename"""
        pixmaps = self.construct_manager()("svg")
        self.assertTrue(pixmaps.get("colors.svg"))
        self.assertFalse(pixmaps.get("colors-no-file.svg"))

    def test_load_name(self):
        """Test loading from a Gtk named theme icon"""
        pixmaps = self.construct_manager()()
        self.assertTrue(pixmaps.get("image-missing"))

    def test_load_data_svg(self):
        """Test loading a data svg"""
        pixmaps = self.construct_manager()(size=None, load_size=(128, 128))
        with open(os.path.join(self.datadir(), "svg", "colors.svg"), "r") as fhl:
            self.assertTrue(pixmaps.get(fhl.read()))
        self.assertRaises(PixmapLoadError, pixmaps.load_from_data, "<svg bad")

    def test_load_data_png(self):
        """Test loading a data png"""
        pixmaps = self.construct_manager()()
        with open(os.path.join(self.datadir(), "svg", "img", "green.png"), "rb") as fhl:
            self.assertPixbuf(pixmaps.get(fhl.read()), pixmaps.get("svg/img/green.png"))

    def test_load_default(self):
        pixmaps = self.construct_manager()()
        self.assertFalse(pixmaps.get(None))
        pixmaps = self.construct_manager(default_image="image-missing")()
        self.assertTrue(pixmaps.get(None))
        pixmaps = self.construct_manager(missing_image="image-missing")()
        self.assertTrue(pixmaps.get("bad-image"))

    def assertPixbuf(self, img_a, img_b, forgive=0.02):
        """Compare to Gobject pixbufs"""
        try:
            self._assert_pixbuf(img_a, img_b, forgive)
        except Exception:
            img_a.savev("image_a.png", "png", [], [])
            img_b.savev("image_b.png", "png", [], [])
            raise

    def _assert_pixbuf(self, img_a, img_b, forgive):
        self.assertEqual(img_a.get_colorspace(), img_b.get_colorspace())
        self.assertEqual(img_a.get_width(), img_b.get_width(), "Width is different")
        self.assertEqual(img_a.get_height(), img_b.get_height(), "Height is different")

        # Bitmap comparison
        array = zip(img_a.get_pixels(), img_b.get_pixels())
        delta, size = 0, 0
        for a, b in array:
            delta += a - b
            size += 1

        pcent = int(forgive * 100)
        ret = int((delta / 256 / float(size)) * 100)
        self.assertLess(
            ret, pcent, f"Result image is not as expected; {ret}% > {pcent}%"
        )
