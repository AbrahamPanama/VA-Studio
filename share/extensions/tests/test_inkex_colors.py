# coding=utf-8

import warnings

from inkex.colors import Color, ColorError, ColorIdError, is_color
from inkex.tester import TestCase

from inkex.colors.spaces.rgb import ColorRGB


class ColorTest(TestCase):
    """Test for single transformations"""

    def test_empty(self):
        """Empty color (black)"""
        self.assertEqual(Color(), [])
        self.assertEqual(str(Color(None)), "none")
        self.assertEqual(str(Color("none")), "none")

    def test_deprecated(self):
        with warnings.catch_warnings(record=True) as warns:
            self.assertEqual(Color([1, 2, 3]).name, "rgb")

    def test_errors(self):
        """Color parsing errors"""
        self.assertRaises(ColorError, Color, {})
        self.assertRaises(ColorError, ColorRGB, ["#id"])
        self.assertRaises(ColorIdError, ColorRGB, "url(#someid)")
        self.assertRaises(ColorError, ColorRGB, [0, 0, 0, 0, 2])
        self.assertRaises(ColorError, ColorRGB, [1])
        self.assertRaises(ColorError, ColorRGB, [], alpha=False)

    def test_nie(self):
        class MyColor(Color):
            name = "error"

        self.assertRaises(NotImplementedError, int, MyColor([]))
        self.assertRaises(NotImplementedError, str, MyColor([]))

    def test_is_color(self):
        """Can detect colour format"""
        self.assertFalse(is_color("rgb[t, b, s]"))
        self.assertTrue(is_color("#fff"))
        self.assertTrue(is_color(1364325887))

    def test_none(self):
        none = Color("none")
        self.assertEqual(none, [])
        self.assertEqual(str(none), "none")
        self.assertEqual(str(Color("red").to_none()), "none")

    def test_to(self):
        # Prep converters before testing
        color = Color("red")
        color.to_named()
        color2 = Color("#ff0000")
        color2.to_rgb()
        self.assertEqual(str(color.to_rgb()), "#ff0000")
        self.assertEqual(str(color.to("rgb")), "#ff0000")
        self.assertEqual(str(color.to(ColorRGB)), "#ff0000")

    def test_slices(self):
        """Support a way to set units in spaces we are not in"""
        color = Color("#ff0000")
        color[0] = 128
        self.assertEqual(str(color), "#800000")
        color["hsl":0] += 180
        self.assertEqual(str(color), "#008080")
        self.assertEqual(color.name, "rgb")
        self.assertEqual(color[1], 128)
        self.assertEqual(color["hsl":0], 180)

    def test_interpolate(self):
        black = Color("#000000")
        grey50 = Color("#080808")
        white = Color("#111111")
        val = black.interpolate(white, 0.5)
        self.assertEqual(val, grey50)

    def test_interpolate_spaces(self):
        """Ensure color space doesn't matter for interpolation"""
        color1 = Color("rgb(0, 0, 0)")
        color2 = Color("hsl(360, 100, 100)")
        interp = color1.interpolate(color2, 0.5)
        self.assertEqual(interp.name, "rgb")
        self.assertEqual(interp, color1.interpolate(color2.to("rgb"), 0.5))

    def test_interpolate_transparency(self):
        """Ensure transparency is included in interpolation"""
        black = Color("#000000ff")
        white = Color("#ffffff00")
        self.assertEqual(black.alpha, 1.0)
        self.assertEqual(white.alpha, 0.0)
        result = black.interpolate(white, 0.5)
        self.assertEqual(result.alpha, 0.5)

        # Assume that no alpha means 1.0, so half of 1.0 to 1.0 is 1.0
        result = black.interpolate(Color("#ffffff"), 0.5)
        self.assertEqual(result.alpha, 1.0)

        # Same assumption, but now it's 1.0-0.0 so 0.5 is expected
        result = Color("#000000").interpolate(white, 0.5)
        self.assertEqual(result.alpha, 0.5)

    def test_namespace_pollution(self):
        """Ensure that inkex.utils does not point to inkex.colors.utils"""
        import inkex.utils

        assert not hasattr(inkex.utils, "is_color")
        assert not inkex.utils == inkex.colors.utils
