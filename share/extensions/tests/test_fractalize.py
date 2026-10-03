# coding=utf-8
from fractalize import Fractalize
from inkex.tester import ComparisonMixin, TestCase


class PathFractalizeBasicTest(ComparisonMixin, TestCase):
    effect_class = Fractalize
    comparisons = [("--id=p1", "--id=p2")]
