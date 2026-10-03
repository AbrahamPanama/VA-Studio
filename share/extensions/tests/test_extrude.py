#!/usr/bin/env python3
# coding=utf-8
from extrude import Extrude
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import (
    CompareOrderIndependentStyleAndPath,
    CompareWithPathSpace,
)


class ExtrudeBasicTest(ComparisonMixin, TestCase):
    effect_class = Extrude
    comparisons = [("--id=p1", "--id=p2")]
    compare_filters = [CompareWithPathSpace(), CompareOrderIndependentStyleAndPath()]


class ExtrudeCircleTest(ComparisonMixin, TestCase):
    effect_class = Extrude
    compare_file = "svg/extrude.svg"
    comparisons = [
        ("--id=c1", "--id=c2"),
        ("--id=c1", "--id=c2", "-m=snug"),
        ("--id=c1", "--id=c2", "-m=polygons"),
    ]


class ExtrudePathConversionTest(ComparisonMixin, TestCase):
    effect_class = Extrude
    compare_file = "svg/extrude.svg"
    comparisons = [
        ("--id=r1", "--id=r2"),
        ("--id=r1", "--id=r2", "-s=False"),
        ("--id=r1", "--id=r2", "-m=snug"),
    ]


class ExtrudeOpenPathTest(ComparisonMixin, TestCase):
    effect_class = Extrude
    compare_file = "svg/extrude.svg"
    comparisons = [
        ("--id=p1", "--id=p2", "-m=lines"),
        ("--id=p1", "--id=p2", "-m=snug"),
    ]


class ExtrudeMultipleSubpathTest(ComparisonMixin, TestCase):
    effect_class = Extrude
    compare_file = "svg/extrude.svg"
    comparisons = [
        ("--id=p3", "--id=p4", "-m=snug"),
        ("--id=p3", "--id=p4", "-m=lines"),
        ("--id=p3", "--id=p4", "-m=polygons"),
    ]
