# coding=utf-8

from inkex.colors import Color, ColorError, ColorIdError
from inkex.tester import TestCase

from inkex.colors.spaces.hsl import ColorHSL


class ColorHslTest(TestCase):
    """Test HSL Support"""

    def test_empty(self):
        self.assertEqual(Color(), [])

    def test_errors(self):
        self.assertRaises(ColorError, ColorHSL, None)

    def test_to_hsl(self):
        """Convert from RGB to HSL"""
        self.assertEqual(Color("#ff7c7d").to_hsl().name, "hsl")
        self.assertEqual(Color("#ff7c7d").to_hsl(), [360, 100, 74])
        self.assertEqual(Color("#7e7c7d").to_hsl(), [330, 1, 49])
        self.assertEqual(Color("#7e7cff").to_hsl(), [241, 100, 74])
        self.assertEqual(Color("#7eff7d").to_hsl(), [120, 100, 75])
        self.assertEqual(Color("#7eff7d80").to_hsl().alpha, 0.50196)

    def test_components(self):
        """Test supported components"""
        color = Color(" hsl(254, 100, 50)")
        self.assertEqual(color.to_hsl(), [254, 100, 50])
        self.assertEqual(color.name, "hsl")
        self.assertEqual(color.hue, 254)
        self.assertEqual(color.saturation, 100)
        self.assertEqual(color.lightness, 50)
        self.assertEqual(color.alpha, None)
        self.assertEqual(color.effective_alpha, 1.0)

    def test_setter(self):
        """Color HSL units can be set on RGB color"""
        color = Color("#ff0102").to_hsl()
        self.assertEqual(color.name, "hsl")
        color.hue = 100
        self.assertEqual(color.hue, 100)
        color.saturation = 100
        self.assertEqual(color.saturation, 100)
        color.lightness = 100
        self.assertEqual(color.lightness, 100)

    def test_set_outofrange(self):
        """Test what happens when the values are too big or small"""
        color = Color("hsl(400, 120, -20)")
        self.assertEqual(str(color), "hsl(40, 100, 0)")
        self.assertEqual(color.hue, 40)
        self.assertEqual(color.saturation, 100)
        self.assertEqual(color.lightness, 0)

        color.hue += 360
        color.saturation -= 120
        color.lightness += 150
        self.assertEqual(color.hue, 40)
        self.assertEqual(color.saturation, 0)
        self.assertEqual(color.lightness, 100)

        color.hue = -120
        color.saturation = 200
        self.assertEqual(color.hue, 360 - 120)
        self.assertEqual(color.saturation, 100)

        color = ColorHSL([-100, 300, -10])
        self.assertEqual(color.hue, 360 - 100)
        self.assertEqual(color.saturation, 100)
        self.assertEqual(color.lightness, 0)

    def test_parsing(self):
        """Parse HSL colors"""
        self.assertEqual(Color("hsl(4.0, 50, 99)"), [4, 50, 99])
        self.assertEqual(Color("hsla(4.0, 50, 99, 0.5)").alpha, 0.5)

    def test_printing(self):
        """Print HSL colors"""
        self.assertEqual(str(Color("hsl(4.0, 50, 99)")), "hsl(4, 50, 99)")
        self.assertEqual(str(Color("hsla(4.0, 50, 99, 0.5)")), "hsla(4, 50, 99, 50%)")

    def test_to_rgb(self):
        """Convert HSL to RGB"""
        color = Color("hsl(172, 46, 50)")
        self.assertEqual(color.to_rgb(), [69, 186, 171])
        color = Color("hsl(172, 100, 10)")
        self.assertEqual(color.to_rgb(), [0, 51, 44])
        color = Color("hsl(0, 46, 10)")
        self.assertEqual(color.to_rgb(), [37, 14, 14])
        color.alpha = 0.5
        self.assertEqual(color.to_rgb().alpha, 0.5)

    def test_cycling(self):
        target = "hsl(172, 46, 50)"
        color = Color(target)
        for x in range(10):
            color = color.to_rgb().to_hsl()
            self.assertEqual(
                str(color), target, f"Conversion Cycle {x} failed, {color} != {target}"
            )

    def test_grey(self):
        """Parse HSL Grey"""
        color = Color("hsl(172, 0, 50)")
        self.assertEqual(color, [172, 0, 50])
        self.assertEqual(color.to_rgb(), [128, 128, 128])
        color = Color("rgb(128, 128, 128)")
        self.assertEqual(color.to_hsl(), [0, 0, 50])
