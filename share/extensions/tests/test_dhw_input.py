# coding=utf-8

from dhw_input import DhwInput

from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class TestDxfInput(ComparisonMixin, TestCase):
    effect_class = DhwInput
    compare_file = [
        "io/PAGE_001.DHW",
        "io/PGLT_161.DHW",
        "io/PGLT_162.DHW",
        "io/PGLT_163.DHW",
    ]
    compare_filters = [CompareNumericFuzzy()]
    comparisons = [()]
