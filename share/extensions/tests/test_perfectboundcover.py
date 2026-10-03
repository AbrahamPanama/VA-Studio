# coding=utf-8
from perfectboundcover import PerfectBoundCover
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase


class PerfectBoundCoverBasicTest(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = PerfectBoundCover
    comparisons = [()]
