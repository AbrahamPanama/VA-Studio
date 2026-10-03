# coding=utf-8

from grid_isometric import GridIsometric
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle, CompareWithPathSpace


class TestGridIsometricBasic(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    compare_filters = [CompareOrderIndependentStyle(), CompareWithPathSpace()]
    effect_class = GridIsometric
    comparisons = [("--dx=10.0", "--subdivs_th=1", "--subsubdivs_th=0.3")]
