# coding=utf-8
from docinfo import DocInfo
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import WindowsTextCompat


class TestDocInfo(ComparisonMixin, TestCase):
    compare_file = ["svg/guides.svg", "svg/three_pages_and_two_grids.svg"]
    effect_class = DocInfo
    stderr_output = True
    comparisons = [()]
    compare_filters = [WindowsTextCompat()]
