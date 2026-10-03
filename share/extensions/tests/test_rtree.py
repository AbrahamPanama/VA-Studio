# coding=utf-8
from rtree import TurtleRtree
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class RTreeTurtleBasicTest(ComparisonMixin, TestCase):
    effect_class = TurtleRtree
    comparisons = [()]
    compare_filters = [
        CompareNumericFuzzy(),
    ]
    comparisons = [("--minimum=4.0",)]
