# coding=utf-8

import re

from ps_input import PostscriptInput

from inkex.tester.filters import CompareSize
from inkex.tester import ComparisonMixin, TestCase


class TestPostscriptInput(ComparisonMixin, TestCase):
    effect_class = PostscriptInput
    compare_filters = [CompareSize()]
    compare_file = [
        "io/test.ps",
        "io/test.eps",
    ]
    comparisons = [()]
