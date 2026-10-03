# coding=utf-8
from previous_glyph_layer import PreviousLayer
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase


class TestPreviousLayerBasic(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = PreviousLayer
