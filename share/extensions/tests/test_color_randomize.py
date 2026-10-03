# coding=utf-8
from color_randomize import Randomize
from .test_inkex_extensions import ColorBaseCase
from inkex.tester import ComparisonMixin, TestCase

Randomize.deterministic_output = True


class ColorRandomizeTest(ColorBaseCase):
    effect_class = Randomize
    python3_only = True
    color_tests = [
        # "hsl(191, 48, 60)" = rgb(150, 100, 200)
        ("none", "none"),
        # The default ranges are set to 0, and thus the color and opacity should not change (except
        # for rounding errors)
        ("hsl(191, 48, 60)", "hsl(191, 48, 60)"),
        # The user selected 0% values, and thus the color should not change.
        ("hsl(191, 48, 60)", "hsl(191, 48, 60)", ["-y 0", "-t 0", "-m 0"]),
        # Random hue only. Saturation and lightness not changed.
        ("hsl(191, 48, 60)", "hsl(236, 48, 60)", ["-y 50", "-t 0", "-m 0"]),
        # Same settings, test stationarity of output.
        ("hsl(191, 48, 60)", "hsl(236, 48, 60)", ["-y 50", "-t 0", "-m 0"]),
        # Random saturation only. Hue and lightness not changed.
        ("hsl(191, 48, 60)", "hsl(191, 50, 60)", ["-y 0", "-t 30", "-m 0"]),
        # Random lightness only. Hue and saturation not changed.
        ("hsl(191, 48, 60)", "hsl(191, 48, 54)", ["-y 0", "-t 0", "-m 50"]),
        # The maximum hsl values should be between 0 and 100% of their maximum
        ("hsl(190, 48, 60)", "hsl(48, 70, 39)", ["-y 100", "-t 100", "-m 100"]),
    ]

    opacity_tests = [
        (5, 5),
        # The user selected 0% opacity range, and thus the opacity should not change.
        (0.15, 0.15, ["-o 0"]),
        # The opacity value should be greater than 0
        (0.0, 0.84, ["-o 100"]),
        # The opacity value should be lesser than 1
        (1.0, 0.77, ["-o 100"]),
        # Other units are available
        ("0.5", 0.654, ["-o 54"]),
        # test that output is deterministic
        ("0.500001", 0.654, ["-o 54"]),
        # Test no opacity
        # The opacity value should be lesser than 1
    ]

    def test_bad_opacity(self):
        """Bad opacity error handled"""
        self.effect.modify_opacity("opacity", "hello")


class TestRandomizeGradients(ComparisonMixin, TestCase):
    """Direct tests for color mechanisms"""

    effect_class = Randomize
    compare_file = "svg/colors.svg"
    python3_only = True

    comparisons = [
        (
            "-y 50",
            "-t 50",
            "-m 50",
            "-o 100",
            "--id=r1",
            "--id=r2",
            "--id=r3",
            "--id=r4",
            "--id=r5",
            "--id=r6",
        ),
    ]


class TestRandomizeOpacity(ComparisonMixin, TestCase):
    """Direct tests for color mechanisms"""

    effect_class = Randomize
    compare_file = "svg/dpiswitcher_96dpi.svg"
    python3_only = True

    comparisons = [
        (
            "-y 0",
            "-t 0",
            "-m 0",
            "-o 100",
            "--id=layer_group_rect_uu2",
            "--id=layer_group_path",
            "--id=root_rect_uu",
        ),
    ]
