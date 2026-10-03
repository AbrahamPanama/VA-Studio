# coding=utf-8
from polyhedron_3d import Poly3D
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class Poly3DBasicTest(ComparisonMixin, TestCase):
    effect_class = Poly3D
    comparisons = [
        (
            "--show=fce",
            "--obj=cube",
            "--r1_ax=x",
            "--r1_ang=45",
            "--r2_ax=y",
            "--r2_ang=45",
            "--cw_wound=True",
        ),
        (
            "--show=fce",
            "--obj=cube",
            "--r1_ax=y",
            "--r1_ang=45",
            "--z_sort=cent",
            "--cw_wound=True",
        ),
        (
            "--show=fce",
            "--obj=cube",
            "--r1_ax=z",
            "--r1_ang=45",
            "--z_sort=max",
            "--cw_wound=True",
        ),
        (
            "--show=edg",
            "--obj=oct",
            "--r1_ax=z",
            "--r1_ang=45",
            "--th=4",
            "--cw_wound=True",
        ),
        ("--show=vtx", "--obj=methane", "--cw_wound=True"),
        ("--show=edg", "--obj=methane", "--cw_wound=True"),
        (
            "--show=fce",
            "--obj=from_file",
            "--spec_file=great_stel_dodec.obj",
            "--cw_wound=True",
        ),
    ]
    compare_filters = [CompareNumericFuzzy()]
    compare_file = "svg/empty.svg"
