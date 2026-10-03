# coding=utf-8

from inkex.colors import Color, ColorError, ColorIdError, is_color
from inkex.tester import TestCase

from inkex.colors.spaces.rgb import ColorRGB
from inkex.colors.spaces.named import ColorNamed


class ColorNamedTest(TestCase):
    """Test for named colors"""

    def test_empty(self):
        """Empty color (black)"""
        self.assertEqual(str(Color().to_named()), "rgba(0, 0, 0, 0%)")

    def test_parsing(self):
        self.assertEqual(ColorNamed("black"), [0, 0, 0])
        self.assertEqual(ColorNamed("green"), [0, 128, 0])
        self.assertEqual(ColorNamed("red"), [255, 0, 0])

    def test_printing(self):
        self.assertEqual(str(ColorNamed([0, 0, 0])), "black")
        self.assertEqual(str(ColorNamed([0, 128, 0])), "green")
        self.assertEqual(str(ColorNamed([255, 0, 0])), "red")

    def test_conversion_to(self):
        self.assertEqual(str(ColorRGB([0, 0, 0]).to_named()), "black")
        self.assertEqual(str(ColorRGB([0, 128, 0]).to_named()), "green")
        self.assertEqual(str(ColorRGB([255, 0, 0]).to_named()), "red")

    def test_conversion_from(self):
        self.assertEqual(str(ColorNamed("black").to_rgb()), "#000000")
        self.assertEqual(str(ColorNamed("green").to_rgb()), "#008000")
        self.assertEqual(str(ColorNamed("red").to_rgb()), "#ff0000")
