# coding=utf-8
from pixelsnap import PixelSnap
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle


class TestPixelSnapEffectBasic(ComparisonMixin, TestCase):
    effect_class = PixelSnap
    compare_filters = [CompareOrderIndependentStyle()]
    comparisons = [("--id=p1", "--id=r3", "--snap_to=bl")]


class TestPixelSnapEffectMM(ComparisonMixin, TestCase):
    """Test pixel snap in mm based documents"""

    effect_class = PixelSnap
    compare_file = "svg/pixelsnap_simple.svg"
    compare_filters = [CompareOrderIndependentStyle()]
    comparisons = [
        (
            "--id=rect1144",
            "--id=path1302",
            "--id=path1430",
            "--id=path1434",
            "--id=path1434",
            "--snap_to=tl",
        ),
        (
            "--id=rect1144",
            "--id=path1302",
            "--id=path1430",
            "--id=path1434",
            "--id=path1434",
            "--snap_to=bl",
        ),
    ]
