# coding=utf-8
from voronoi_fill import VoronoiFill
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle


class TestPatternBasic(ComparisonMixin, TestCase):
    effect_class = VoronoiFill
    comparisons = [
        ("--id=r3", "--id=p1"),
    ]
    compare_filters = [CompareOrderIndependentStyle()]
