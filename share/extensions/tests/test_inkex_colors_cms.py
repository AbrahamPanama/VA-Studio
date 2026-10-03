# coding=utf-8

from inkex.colors import Color, ColorError, ColorIdError
from inkex.tester import TestCase

from inkex.colors.spaces.cms import ColorCMS


class ColorCmykTest(TestCase):
    """Test CMYK Support"""

    def test_errors(self):
        self.assertRaises(ColorError, ColorCMS, [], None)

    def test_alpha(self):
        color = ColorCMS([0.57, 0, 0.5], "apple")
        self.assertEqual(color.alpha, None)
        self.assertEqual(color.effective_alpha, 1.0)

        # Alpha is not allowed on cms values
        color.alpha = 0.5
        self.assertEqual(color.alpha, None)
        self.assertEqual(color.effective_alpha, 1.0)

    def test_parsing(self):
        """Parse CMYK colors"""
        self.assertTrue(ColorCMS.can_parse("icc-color()"))
        self.assertEqual(
            Color("icc-color(icc-name, 0.1, 0.2, 0.3, 0.4, 5)"), [0.1, 0.2, 0.3, 0.4, 1]
        )

        self.assertTrue(ColorCMS.can_parse("#fff icc-color()"))
        color = Color("#ff0000 icc-color(icc-name, 0.1, 0.2, 0.3)")
        self.assertEqual(color, [0.1, 0.2, 0.3])
        self.assertEqual(color.fallback_rgb, [255, 0, 0])
        self.assertEqual(color.icc_profile, "icc-name")

    def test_printing(self):
        """Print CMYK colors"""
        self.assertEqual(
            str(ColorCMS([0.57, 0, 0.5], "apple")), "icc-color(apple, 0.57, 0, 0.5)"
        )
        fallback = Color("#0f0f0f")
        self.assertEqual(
            str(ColorCMS([0.16, 1, 0, 0.9], "oranges", fallback)),
            "#0f0f0f icc-color(oranges, 0.16, 1, 0, 0.9)",
        )

    def test_scales(self):
        color = Color("icc-color(icc-name, 0.1)")
        self.assertEqual(color.scale_up(100, 10), 10)
        self.assertEqual(color.scale_down(100, 10), 10)

    def test_conversion(self):
        color = Color("icc-color(icc-name, 0.1)")
        self.assertRaises(NotImplementedError, color.convert_to_rgb)
        self.assertRaises(NotImplementedError, color.convert_from_rgb)
