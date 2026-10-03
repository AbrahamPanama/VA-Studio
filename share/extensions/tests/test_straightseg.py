# coding=utf-8
from straightseg import SegmentStraightener
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy, CompareWithPathSpace


class SegmentStraightenerBasicTest(
    ComparisonMixin, InkscapeExtensionTestMixin, TestCase
):
    effect_class = SegmentStraightener
    compare_filters = [CompareNumericFuzzy(), CompareWithPathSpace()]
