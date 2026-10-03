# coding=utf-8
from text_braille import Braille
from inkex.tester import ComparisonMixin, TestCase


class TestBrailleBasic(ComparisonMixin, TestCase):
    effect_class = Braille
    python3_only = True
    comparisons = [()]
