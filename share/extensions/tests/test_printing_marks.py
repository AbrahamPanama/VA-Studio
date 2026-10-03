# coding=utf-8
from printing_marks import PrintingMarks
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import (
    CompareNumericFuzzy,
    CompareWithPathSpace,
    CompareOrderIndependentStyle,
)


class PrintingMarksBasicTest(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = PrintingMarks
    compare_filters = [
        CompareNumericFuzzy(),
        CompareWithPathSpace(),
        CompareOrderIndependentStyle(),
    ]
    compare_file = ["svg/shapes.svg", "svg/empty_multipage.svg"]
    comparisons = {
        (
            "--crop_marks",
            "True",
            "--bleed_marks",
            "True",
            "--registration_marks",
            "True",
            "--star_target",
            "True",
            "--colour_bars",
            "True",
            "--page_info",
            "True",
        )
    }
