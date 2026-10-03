# coding=utf-8
from inkex.tester import ComparisonMixin, TestCase
from text_randomcase import RandomCase


class TestRandomCaseBasic(ComparisonMixin, TestCase):
    effect_class = RandomCase
    comparisons = [()]
