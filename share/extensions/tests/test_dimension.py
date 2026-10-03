# coding=utf-8
from dimension import Dimension
from inkex.tester import ComparisonMixin, TestCase


class TestDimensionBasic(ComparisonMixin, TestCase):
    effect_class = Dimension
    comparisons = [
        ("--id=p1", "--id=r3", "--xoffset=100.0", "--yoffset=100.0"),
        ("--id=p1", "--id=r3", "--type=visual", "--xoffset=100.0", "--yoffset=100.0"),
    ]


class TestDimensionMillimeters(ComparisonMixin, TestCase):
    effect_class = Dimension
    compare_file = "svg/css.svg"
    comparisons = [("--id=circle1",)]
