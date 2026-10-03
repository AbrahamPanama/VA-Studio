# coding=utf-8
from inkex.utils import filename_arg
from measure import MeasureLength
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy

olddefaults = ("--fontsize=20", "--unit=mm", "--scale=1.1")


class LengthBasicTest(ComparisonMixin, TestCase):
    effect_class = MeasureLength
    compare_filters = [CompareNumericFuzzy()]
    comparisons = [
        ("--id=p1", "--id=p2", "--presetFormat=TaP_start") + olddefaults,
        ("--method=presets", "--presetFormat=TaP_start", "--id=p1") + olddefaults,
        ("--method=presets", "--presetFormat=TaP_end", "--id=p2") + olddefaults,
        ("--method=presets", "--presetFormat=FT_start", "--id=p1") + olddefaults,
        ("--method=presets", "--presetFormat=FT_bbox", "--id=p2") + olddefaults,
        ("--method=presets", "--presetFormat=FT_bbox", "--id=p2") + olddefaults,
        ("--type=area", "--id=p1", "--presetFormat=TaP_start") + olddefaults,
        ("--type=cofm", "--id=c3", "--presetFormat=TaP_start") + olddefaults,
    ]


class LengthComplexTransformTest(ComparisonMixin, TestCase):
    effect_class = MeasureLength
    compare_filters = [CompareNumericFuzzy()]
    compare_file = "svg/complextransform.test.svg"
    comparisons = [
        ("--method=presets", "--presetFormat=TaP_start", "--id=D") + olddefaults,
        ("--method=presets", "--presetFormat=FT_start", "--id=D") + olddefaults,
        ("--type=cofm", "--id=D", "--presetFormat=TaP_start") + olddefaults,
    ]
