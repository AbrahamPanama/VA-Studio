# coding=utf-8
from svgfont2layers import SvgFontToLayers
from inkex.tester import ComparisonMixin, TestCase


class TestSVGFont2LayersBasic(ComparisonMixin, TestCase):
    effect_class = SvgFontToLayers
    compare_file = "svg/font.svg"
    comparisons = [("--count=3",)]
