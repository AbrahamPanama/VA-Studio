#!/usr/bin/env python3
# coding=utf-8
from interp_att_g import InterpAttG
from inkex.tester import ComparisonMixin, TestCase


class InterpAttGBasicTest(ComparisonMixin, TestCase):
    effect_class = InterpAttG
    comparisons = [("--id=layer1", "--att=style/fill")]


class InterpAttGMultipleSelectedTest(ComparisonMixin, TestCase):
    effect_class = InterpAttG
    comparisons = [("--id=c1", "--id=c2", "--id=c3", "--att=style/fill")]


class InterpAttGColorRoundingTest(ComparisonMixin, TestCase):
    effect_class = InterpAttG
    compare_file = "svg/group_interpolate.svg"
    comparisons = [
        # test for truncating/rounding bug inbox#1892
        ("--id=g53", "--att=style/fill", "--start-val=#181818", "--end-val=#000000"),
        # test for clipping of values <= 1
        ("--id=g53", "--att=style/fill", "--start-val=#050505", "--end-val=#000000"),
    ]


class InterpAttGOtherAttributeTest(ComparisonMixin, TestCase):
    # interpolate other values (test base.arg_class)
    effect_class = InterpAttG
    compare_file = "svg/group_interpolate.svg"
    comparisons = [
        (
            "--id=g53",
            "--att=other",
            "--att-other=width",
            "--start-val=5",
            "--end-val=10",
            "--att-other-type=ValueInterpolator",
        ),
        (
            "--id=g53",
            "--att=other",
            "--att-other=fill",
            "--att-other-where=style",
            "--start-val=red",
            "--end-val=green",
            "--att-other-type=ColorInterpolator",
        ),
    ]


class InterpAttGTransformInterpolateTest(ComparisonMixin, TestCase):
    effect_class = InterpAttG
    compare_file = "svg/group_interpolate.svg"
    comparisons = [
        ("--id=g53", "--att=transform/scale", "--start-val=0.2", "--end-val=0.9"),
        ("--id=g53", "--att=transform/trans-x", "--start-val=0", "--end-val=20"),
    ]


class InterpAttGUnitsTest(ComparisonMixin, TestCase):
    effect_class = InterpAttG
    compare_file = "svg/group_interpolate.svg"
    comparisons = [
        ("--id=g53", "--att=width", "--start-val=0.02", "--end-val=0.1", "--unit=mm")
    ]
