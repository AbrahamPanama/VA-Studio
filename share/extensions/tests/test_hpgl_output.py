# coding=utf-8
from hpgl_output import HpglOutput
from inkex.tester import ComparisonMixin, TestCase


class HPGLOutputBasicTest(ComparisonMixin, TestCase):
    effect_class = HpglOutput
    compare_file = ["svg/shapes.svg", "svg/hpgl_multipen.svg"]
    comparisons = [("--force=24", "--speed=20", "--orientation=90")]


class HPGLOutputPlotterRepeatedPathCoord(ComparisonMixin, TestCase):
    """The L commands followed by M commands with the same
    coordinates triggered a bug (extensions issue #522) in the HPGL encoder
     resulting in some commands being swallowed."""

    effect_class = HpglOutput
    compare_file = ["svg/hplg_repeated_path_coords.svg"]
    comparisons = [
        ("--overcut=0.0", "--toolOffset=0.0", "--precut=False", "--autoAlign=False")
    ]
