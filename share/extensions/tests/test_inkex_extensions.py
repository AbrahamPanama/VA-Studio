# coding=utf-8
"""
Test each of the extensions base classes (if needed) and sometimes provide
specialised test classes for testers to use.
"""

import inkex
from inkex.tester import ComparisonMixin, TestCase


class TurnGreenEffect(inkex.ColorExtension):
    """Turn everything the purest green!"""

    def modify_color(self, name, color):
        return inkex.Color("green")

    def modify_opacity(self, name, opacity):
        if name == "opacity":
            return 1.0
        return opacity


class ColorEffectTest(ComparisonMixin, TestCase):
    """Direct tests for color mechanisms"""

    effect_class = TurnGreenEffect
    effect_name = "inkex_extensions_color"
    compare_file = "svg/colors.svg"
    python3_only = True

    comparisons = [
        ("--id=r1",),  # One shape only
        ("--id=r2",),  # CSS Styles
        ("--id=r3",),  # Element Attributes
        ("--id=r4",),  # Gradient stops
        ("--id=r1", "--id=r2"),  # Two shapes
        ("--id=color_svg",),  # Recursive group/children
        (),  # Process all shapes
    ]


class ColorBaseCase(TestCase):
    """Base class for all color effect extensions"""

    color_tests = []
    opacity_tests = []

    def _test_list(self, tsts):
        for tst in tsts:
            inp, outp, args = (list(tst) + [[]])[:3]
            self.effect.parse_arguments([self.empty_svg] + args)
            yield inp, outp

    def test_colors(self):
        """Run all color tests"""
        for x, (inp, outp) in enumerate(self._test_list(self.color_tests)):
            outp = inkex.Color(outp)
            if isinstance(inp, (list, tuple)):
                color = inkex.ColorRGB(inp)
            else:
                color = inkex.Color(inp)
            got = self.effect._modify_color("fill", color)
            self.assertTrue(
                isinstance(got, inkex.Color),
                "Bad output type: {}".format(type(got).__name__),
            )
            outp, got = str(outp), str(got.to(outp.name))
            self.assertEqual(
                outp, got, "Color mismatch, test:{} {} != {}".format(x, outp, got)
            )
        for x, (inp, outp) in enumerate(self._test_list(self.opacity_tests)):
            got = self.effect.modify_opacity("opacity", inp)
            self.assertTrue(isinstance(got, float))
            self.assertAlmostEqual(got, outp, delta=0.1)
