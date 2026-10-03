# coding=utf-8
from jessyink_view import View
from inkex.tester import ComparisonMixin, TestCase


class JessyInkEffectsBasicTest(ComparisonMixin, TestCase):
    effect_class = View
    comparisons = [("--id=r3", "--viewOrder=1")]
