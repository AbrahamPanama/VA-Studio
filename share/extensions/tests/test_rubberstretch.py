# coding=utf-8
from rubberstretch import RubberStretch
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase


class TestRubberStretchBasic(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = RubberStretch
    compare_file = "svg/rubber-stretch-test.svg"
    comparisons = [
        ("--id=path3997", "--ratio=50", "--curve=0"),
        ("--id=path3997", "--ratio=0", "--curve=50"),
        ("--id=path3997", "--ratio=25", "--curve=25"),
    ]
