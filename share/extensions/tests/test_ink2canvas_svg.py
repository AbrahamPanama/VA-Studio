#!/usr/bin/en
# coding=utf-8
from ink2canvas import Html5Canvas
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentLines
from inkex.tester.filters import WindowsTextCompat


class Ink2CanvasBasicTest(ComparisonMixin, TestCase):
    effect_class = Html5Canvas
    compare_file = [
        "svg/shapes.svg",
        "svg/clips_and_masks.svg",
        "svg/multilayered-test.svg",
        "svg/multiple_closed_subpaths.svg",
        "svg/styling-css-04-f.svg",
        "svg/dashes_examples.svg",
        "svg/embed.svg",
        "svg/simple_patterns.svg",
        "svg/colors.svg",
    ]

    compare_filters = [WindowsTextCompat()]
    comparisons = [()]
