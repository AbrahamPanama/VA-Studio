# coding=utf-8
from setup_typography_canvas import SetupTypographyCanvas
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase


class TestSetupTypographyCanvasBasic(
    ComparisonMixin, InkscapeExtensionTestMixin, TestCase
):
    effect_class = SetupTypographyCanvas
    compare_file = ["svg/empty.svg", "svg/shapes.svg"]
    comparisons = [()]
