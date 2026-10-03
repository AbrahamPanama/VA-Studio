# coding=utf-8
from text_flipcase import FlipCase
from inkex.tester import ComparisonMixin, TestCase


class TestFlipCaseBasic(ComparisonMixin, TestCase):
    effect_class = FlipCase
    comparisons = [()]
