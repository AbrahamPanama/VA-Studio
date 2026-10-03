# coding=utf-8
from funcplot import FuncPlot, math_eval
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy
import math


class FuncPlotBasicTest(ComparisonMixin, TestCase):
    effect_class = FuncPlot
    compare_filters = [CompareNumericFuzzy()]
    comparisons = [
        ("--id=p1", "--id=r3", "--times2pi=True", "--ybottom=-1.0", "--drawaxis=True"),
    ]
