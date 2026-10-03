# coding=utf-8

from fig_input import FigInput

from inkex.tester import ComparisonMixin, TestCase


class TestFigInput(ComparisonMixin, TestCase):
    effect_class = FigInput
    compare_file = "io/test.fig"
    comparisons = [()]
