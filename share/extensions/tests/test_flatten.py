# coding=utf-8
from flatten import Flatten
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy, CompareWithPathSpace


class FlattenBasicTest(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    compare_filters = [CompareNumericFuzzy(), CompareWithPathSpace()]
    effect_class = Flatten
