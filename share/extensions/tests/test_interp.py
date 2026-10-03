# coding=utf-8
from interp import Interpolate
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class InterpBasicTest(ComparisonMixin, TestCase):
    effect_class = Interpolate
    comparisons = [
        (
            "--id=path1",
            "--id=path2",
            "--id=path3",
            "--id=path4",
            "--id=path5",
            "--id=path6",
            "--id=path7",
            "--id=path8",
            "--id=path9",
            "--id=path10",
            "--method=equalSubsegments",
            "--style=True",
        ),
        (
            "--id=path1",
            "--id=path2",
            "--id=path3",
            "--id=path4",
            "--id=path5",
            "--id=path6",
            "--id=path7",
            "--id=path8",
            "--id=path9",
            "--id=path10",
            "--method=firstNodes",
            "--style=True",
        ),
    ]
    compare_filters = [CompareNumericFuzzy()]
    compare_file = "svg/interp_shapes.svg"
