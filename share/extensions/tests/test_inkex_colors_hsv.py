# coding=utf-8

from inkex.colors import Color, ColorError, ColorIdError
from inkex.tester import TestCase

from inkex.colors.spaces.hsv import ColorHSV


class ColorHslTest(TestCase):
    """Test HSV Support"""

    def test_hsv_hsl_conversion(self):
        """Examples from https://en.wikipedia.org/wiki/HSL_and_HSV#Examples"""
        self.assertEqual(ColorHSV.convert_to_hsl(120, 1, 0.5), [120, 1.0, 0.25])
        self.assertEqual(ColorHSV.convert_to_hsl(300, 2 / 3, 0.75), [300, 0.5, 0.5])
        self.assertEqual(ColorHSV.convert_to_hsl(0, 0, 1), [0, 0, 1])

    def test_empty(self):
        self.assertEqual(Color().to_hsv(), [0, 0, 0])

    def test_to_space(self):
        """Convert from RGB to HSV"""
        self.assertEqual(Color("#ff7c7d").to_hsv().name, "hsv")
        self.assertEqual(Color("#ff7c7d").to_hsv(), [360, 51, 100])
        self.assertEqual(Color("#7e7c7d").to_hsv(), [330, 2, 49])
        self.assertEqual(Color("#7e7cff").to_hsv(), [241, 51, 100])
        self.assertEqual(Color("#7eff7d").to_hsv(), [120, 51, 100])

    def test_components(self):
        """Test supported components"""
        color = Color(" hwb(242 17% 50% / 0.75)")
        self.assertEqual(color, [242, 65, 50])
        self.assertEqual(color.name, "hsv")
        self.assertEqual(color.hue, 242)
        self.assertEqual(color.saturation, 65)
        self.assertEqual(color.value, 50)
        self.assertEqual(color.alpha, 0.75)

    def test_setter(self):
        """Color HSV units can be set on RGB color"""
        color = Color("hwb(0 0 0)")
        color.hue = 100
        self.assertEqual(color.hue, 100)
        color.saturation = 1.0
        self.assertEqual(color.saturation, 1.0)
        color.value = 1.0
        self.assertEqual(color.value, 1.0)

    def test_parsing(self):
        """Parse HSV colors"""
        self.assertEqual(Color("hwb(242 17% 50%)"), [242, 65, 50])
        self.assertEqual(Color("hwb(242 17 0.5 / 0.5)"), [242, 65, 50])

    def test_printing(self):
        """Print HSV colors"""
        self.assertEqual(str(ColorHSV([4, 50, 100])), "hwb(4 50 0)")
        self.assertEqual(str(ColorHSV([4, 50, 100, 0.75])), "hwb(4 50 0 / 75%)")

    def test_to_rgb(self):
        """Convert HSV to RGB"""
        color = Color("hwb(242 17% 50%)")
        self.assertEqual(color.to_rgb(), [47, 45, 128])
        self.assertEqual(str(color.to_rgb()), "#2f2d80")
        color = Color("hwb(172 131 10)")
        self.assertEqual(color.to_rgb(), [230, 230, 230])
        color = Color("hwb(0 131 10)")
        self.assertEqual(color.to_rgb(), [230, 230, 230])

    def test_grey(self):
        """Parse HSV Grey"""
        color = Color("hwb(45 74% 25%)")
        self.assertEqual(color, [45, 1, 75])
        self.assertEqual(color.to_rgb(), [191, 191, 189])
        color = Color("rgb(191 190 189)")
        self.assertEqual(color.to_hsv(), [30, 1, 75])
