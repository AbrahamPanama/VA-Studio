# coding=utf-8
from lindenmayer import Lindenmayer
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle, CompareWithPathSpace


class LSystemBasicTest(ComparisonMixin, TestCase):
    effect_class = Lindenmayer
    compare_filters = [CompareOrderIndependentStyle(), CompareWithPathSpace()]
    comparisons = [
        (),
        # left-looking Koch snowflake (one iteration)
        ("--rules=F=F+F--F+F", "--axiom=F", "--order=1", "--langle=60", "--rangle=60"),
        # test multiple rules: right pointing Sierpinski triangle
        (
            "--rules=F=F-E+F+E-F;E=EE",
            "--axiom=F-E-E",
            "--order=3",
            "--langle=120",
            "--rangle=120",
        ),
    ]
