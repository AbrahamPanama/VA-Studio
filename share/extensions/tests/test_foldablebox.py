# coding=utf-8
from foldablebox import FoldableBox
from inkex.tester import ComparisonMixin, TestCase


class FoldableBoxArguments(ComparisonMixin, TestCase):
    effect_class = FoldableBox
    compare_file = "svg/empty.svg"
    comparisons = [
        ("--width=20", "--height=20", "--depth=2.2", "--unit=cm"),
        ("--proportion=0.5", "--guide=true", "--unit=cm"),
    ]
