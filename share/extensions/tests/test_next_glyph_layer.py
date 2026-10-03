# coding=utf-8
from next_glyph_layer import NextLayer
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase


class TestNextLayerBasic(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = NextLayer
