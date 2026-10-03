# coding=utf-8

from inkex.colors import Color, ColorError, ColorIdError, is_color
from inkex.tester import TestCase

from inkex.colors.spaces.rgb import ColorRGB


class ColorRGBTest(TestCase):
    """Test for single transformations"""

    def test_empty(self):
        """Empty color (black)"""
        self.assertEqual(Color().to_rgb(), [0, 0, 0])

    def test_errors(self):
        """Color parsing errors"""
        self.assertRaises(ColorError, Color, "#badhex")

    def test_parsing(self):
        """RGB Hex Color"""
        self.assertEqual(Color("rgb(45,50,55)"), [45, 50, 55])
        self.assertEqual(Color("rgba(45,50,55,50%)"), [45, 50, 55])
        self.assertEqual(Color("rgba(45,50,55,150%)"), [45, 50, 55])
        self.assertEqual(Color("rgba(66.667%,0%,6.667%,50%)"), [170, 0, 17])
        self.assertEqual(Color("#AAA"), [170, 170, 170])

    def test_printing(self):
        """RGB Printing"""
        self.assertEqual(str(Color("rgb(45,50,55)")), "#2d3237")
        self.assertEqual(str(ColorRGB([91, 166, 176])), "#5ba6b0")
        self.assertEqual(str(ColorRGB([255.0, 255.1, 254.9])), "#ffffff")
        self.assertEqual(str(ColorRGB([128.0, 128.1, 127.9])), "#808080")
        self.assertEqual(str(ColorRGB([0.0, 0.1, -0.1])), "#000000")

    def test_components(self):
        color = Color("#ff0102")
        self.assertEqual(color.red, 255)
        self.assertEqual(color.green, 1)
        self.assertEqual(color.blue, 2)
        self.assertEqual(color.alpha, None)
        self.assertEqual(color.effective_alpha, 1.0)
        self.assertEqual(list(color), [255, 1, 2])
        self.assertEqual(list(color.get_values(True)), [255, 1, 2, 1.0])
        self.assertEqual(list(color.get_values(False)), [255, 1, 2])

    def test_setter(self):
        """Color RGB units can be set"""
        color = Color("red")
        color.red = 127
        self.assertEqual(color.red, 127)
        self.assertEqual(str(color), "#7f0000")
        color.green = 5
        self.assertEqual(color.green, 5)
        self.assertEqual(str(color), "#7f0500")
        color.blue = 15
        self.assertEqual(color.blue, 15)
        self.assertEqual(str(color), "#7f050f")
        color.blue = 5.1
        self.assertEqual(color.blue, 5)
        self.assertEqual(str(color), "#7f0505")
        color.blue = 5.8
        self.assertEqual(color.blue, 6)
        self.assertEqual(str(color), "#7f0506")
        color.alpha = 0.5
        self.assertEqual(str(color), "rgba(127, 5, 6, 50%)")

    def test_percent(self):
        """RGB Percent Color"""
        self.assertEqual(Color("rgb(100%,100%,100%)"), [255, 255, 255])
        self.assertEqual(Color("rgb(50%,0%,1%)"), [128, 0, 3])
        self.assertEqual(Color("rgb(66.667%,0%,6.667%)"), [170, 0, 17])

    def test_rgba_one(self):
        """Test for https://gitlab.com/inkscape/extensions/-/issues/402"""
        self.assertEqual(Color("rgb(1, 100%,1.0)"), [1, 255, 255])
        self.assertEqual(Color("rgba(1, 100%,1.0, 100%)"), [1, 255, 255])
        self.assertEqual(Color("rgba(1, 100%, 1.0, 1.0)"), [1, 255, 255])
        self.assertEqual(Color("rgba(1, 100%, 1.0, 1)"), [1, 255, 255])
        self.assertEqual(Color("rgba(1, 0, 0, 1)"), [1, 0, 0])
        self.assertEqual(ColorRGB([1, 1.0, 1.0]), [1, 1.0, 1.0])

    def test_alpha(self):
        """Parse RGBA colours"""
        color = Color("rgba(255,127,255,0.5)")
        self.assertEqual(str(color), "rgba(255, 127, 255, 50%)")
        color.alpha = None
        self.assertEqual(str(color), "#ff7fff")
        color.alpha = 0.75
        self.assertEqual(color, [255, 127, 255])
        self.assertEqual(color.alpha, 0.75)
        color.alpha = 1.0
        self.assertEqual(str(color), "rgba(255, 127, 255, 100%)")

    def test_int_color(self):
        """Colours from arg parser"""
        color = Color(1364325887)
        self.assertEqual(str(color), "rgba(81, 81, 245, 100%)")
        color = Color("1364325887")
        self.assertEqual(str(color), "rgba(81, 81, 245, 100%)")
        color = Color("-1364325887")
        self.assertEqual(str(color), "rgba(174, 174, 10, 0%)")
        color = Color(0xFFFFFFFF)
        self.assertEqual(str(color), "rgba(255, 255, 255, 100%)")
        color = Color(0xFFFFFF00)
        self.assertEqual(str(color), "rgba(255, 255, 255, 0%)")
        self.assertEqual(int(Color("#808080")), 0x808080FF)
        self.assertEqual(int(Color("rgba(128, 128, 128, 20%)")), 2155905075)

    def test_short_hex(self):
        """RGB Short Hex Color"""
        self.assertEqual(Color("#fff"), [255, 255, 255])
        self.assertEqual(str(Color("#fff")), "#ffffff")
