# coding=utf-8
from jessyink_summary import Summary
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import WindowsTextCompat


class JessyInkSummaryTest(ComparisonMixin, TestCase):
    stderr_output = True
    effect_class = Summary
    comparisons = [()]
    compare_filters = [WindowsTextCompat()]
