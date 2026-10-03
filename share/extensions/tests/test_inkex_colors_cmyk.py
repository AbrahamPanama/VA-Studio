# coding=utf-8

from inkex.colors import Color, ColorError, ColorIdError
from inkex.tester import TestCase

from inkex.colors.spaces.cmyk import ColorDeviceCMYK


class ColorCmykTest(TestCase):
    """Test CMYK Support"""

    def test_empty(self):
        self.assertEqual(Color().to_cmyk(), [0, 0, 0, 100])

    def test_to_space(self):
        """Convert from RGB to CMYK"""
        self.assertEqual(Color("#ff7c7d").to_cmyk().name, "cmyk")
        self.assertEqual(Color("#ff7c7d").to_cmyk(), [0, 51, 51, 0])
        self.assertEqual(Color("#7e7c7d").to_cmyk(), [0, 2, 1, 51])
        self.assertEqual(Color("#7e7cff").to_cmyk(), [51, 51, 0, 0])
        self.assertEqual(Color("#7eff7d").to_cmyk(), [51, 0, 51, 0])

    def test_components(self):
        """Test supported components"""
        color = Color(" device-cmyk(45 22 19% 100 / 0.75)")
        self.assertEqual(color, [45, 22, 19, 100])
        self.assertEqual(color.name, "cmyk")
        self.assertEqual(color.cyan, 45)
        self.assertEqual(color.magenta, 22)
        self.assertEqual(color.yellow, 19)
        self.assertEqual(color.black, 100)
        self.assertEqual(color.alpha, 0.75)
        self.assertEqual(color.effective_alpha, 0.75)

    def test_setter(self):
        """Color CMYK units can be set"""
        color = Color("device-cmyk(0 0 0 0)")
        color.cyan = 100
        self.assertEqual(color.cyan, 100)
        color.magenta = 50
        self.assertEqual(color.magenta, 50)
        color.yellow = 5
        self.assertEqual(color.yellow, 5)
        color.black = 19
        self.assertEqual(color.black, 19)

    def test_parsing(self):
        """Parse CMYK colors"""
        self.assertEqual(Color("device-cmyk(57 0 5 0)"), [57, 0, 5, 0])
        self.assertEqual(Color("device-cmyk(16 1 0 90 / 19%)"), [16, 1, 0, 90])

    def test_printing(self):
        """Print CMYK colors"""
        self.assertEqual(str(ColorDeviceCMYK([57, 0, 5, 0])), "device-cmyk(57 0 5 0)")
        self.assertEqual(
            str(ColorDeviceCMYK([16, 1, 0, 90, 0.75])), "device-cmyk(16 1 0 90 / 75%)"
        )

    def test_to_rgb(self):
        """Convert CMYK to RGB"""
        color = ColorDeviceCMYK([50, 40, 0, 30])
        self.assertEqual(color.to_rgb(), [89, 107, 178])
        color = ColorDeviceCMYK([0, 20, 58, 17])
        self.assertEqual(color.to_rgb(), [212, 169, 89])
        color = ColorDeviceCMYK([12, 0, 31, 94])
        self.assertEqual(color.to_rgb(), [13, 15, 11])
