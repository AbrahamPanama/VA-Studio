# coding=utf-8
from voronoi_diagram import Voronoi
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle


class TestVoronoi2svgBasic(ComparisonMixin, TestCase):
    effect_class = Voronoi
    compare_filters = [CompareOrderIndependentStyle()]
    comparisons = [
        (
            "--id=c1",
            "--id=c2",
            "--id=c3",
            "--id=p1",
            "--id=p2",
            "--id=s1",
            "--id=u1",
            "--diagram-type=Both",
            "--clip-box=Automatic from seeds",
            "--show-clip-box=True",
        ),
        (
            "--id=c1",
            "--id=c2",
            "--id=c3",
            "--id=p1",
            "--id=p2",
            "--id=s1",
            "--diagram-type=Voronoi",
            "--clip-box=Page",
        ),
        (
            "--id=r1",
            "--id=r3",
            "--id=c1",
            "--id=c3",
            "--id=s1",
            "--diagram-type=Both",
            "--delaunay-fill-options=delaunay-fill",
        ),
    ]


class TestVoronoi2svgmm(ComparisonMixin, TestCase):
    """Test voronoi for mm based documents (https://gitlab.com/inkscape/extensions/-/issues/403)"""

    effect_class = Voronoi
    compare_file = "svg/interp_shapes.svg"
    comparisons = [
        tuple(f"--id=path{i}" for i in range(1, 11))
        + ("--diagram-type=Voronoi", "--clip-box=Page")
    ]
