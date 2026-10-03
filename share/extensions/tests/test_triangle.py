#!/usr/bin/env python3
from triangle import Triangle
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy, CompareOrderIndependentStyle


class TriangleBasicTest(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = Triangle
    compare_filters = [CompareNumericFuzzy(), CompareOrderIndependentStyle()]
