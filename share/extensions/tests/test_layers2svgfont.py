# coding=utf-8
from layers2svgfont import LayersToSvgFont
from inkex.tester import ComparisonMixin, TestCase


class TestLayers2SVGFontBasic(ComparisonMixin, TestCase):
    effect_class = LayersToSvgFont
    compare_file = ["svg/font_layers.svg", "svg/font_layers_apostrophe.svg"]
    comparisons = [()]
