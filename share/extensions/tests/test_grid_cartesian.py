# coding=utf-8
from grid_cartesian import GridCartesian
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle


class GridCartesianBasicTest(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = GridCartesian
    compare_filters = [CompareOrderIndependentStyle()]
    old_defaults = [
        "--border_th=3",
        "--border_th_unit=cm",
        "--dx=100.0",
        "--dx_unit=cm",
        "--x_subdivs=2",
        "--x_subsubdivs=5",
        "--x_half_freq=4",
        "--x_divs_th=2",
        "--x_subdivs_th=1",
        "--x_div_unit=cm",
        "--dy=100.0",
        "--dy_unit=cm",
        "--y_half_freq=4",
        "--y_divs_th=2",
        "--y_subdivs_th=1",
        "--y_div_unit=cm",
    ]
    comparisons = [
        tuple(old_defaults),
        tuple(old_defaults + ["--id=p1", "--id=r3"]),
    ]
