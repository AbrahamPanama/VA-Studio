# coding=utf-8
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy, CompareWithPathSpace

from twirl import Twirl


class TwirlBasicTest(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = Twirl
    compare_filters = [CompareNumericFuzzy(), CompareWithPathSpace()]
    comparisons = [("--id=p1", "--id=r3", "--twirl=1.0")]
