# coding=utf-8
from export_gimp_palette import ExportGimpPalette
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import WindowsTextCompat


class TestExportGplBasic(ComparisonMixin, TestCase):
    effect_class = ExportGimpPalette
    compare_file = "svg/colors.svg"
    compare_filters = [WindowsTextCompat()]


class TestExportGplCurrentColor(ComparisonMixin, TestCase):
    """Test that the currentColor attribute is correctly parsed"""

    effect_class = ExportGimpPalette
    compare_file = ["svg/current_color.svg"]
    compare_filters = [WindowsTextCompat()]
    comparisons = [
        (),
    ]
