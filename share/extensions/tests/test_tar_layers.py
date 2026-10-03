# coding=utf-8
from tar_layers import TarLayers
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareSize


class LayersOutputBasicTest(ComparisonMixin, TestCase):
    effect_class = TarLayers
    compare_filters = [CompareSize()]
