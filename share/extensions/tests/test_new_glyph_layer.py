# coding=utf-8
from new_glyph_layer import NewGlyphLayer
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase


class TestNewGlyphLayerBasic(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = NewGlyphLayer
