# coding=utf-8
from template_seamless_pattern import SeamlessPattern
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class SeamlessPatternBasicTest(ComparisonMixin, TestCase):
    effect_class = SeamlessPattern
    compare_filters = [CompareNumericFuzzy()]
    comparisons = [("--width=100", "--height=100")]
