# coding=utf-8
from spirograph import Spirograph
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle


class SpirographBasicTest(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = Spirograph
    compare_filters = [CompareOrderIndependentStyle()]
    comparisons = [("--primaryr=60.0", "--secondaryr=100.0")]
