# coding=utf-8
from lorem_ipsum import LoremIpsum
from inkex.tester import ComparisonMixin, TestCase


class LoremIpsumBasicTest(ComparisonMixin, TestCase):
    effect_class = LoremIpsum
    compare_file = "svg/shapes.svg"
    comparisons = [
        (),
        ["--svg2=false"],
        ["--id=r1"],
        ["--id=r1", "--svg2=false"],
        ["--id=t4"],
    ]


class LoremIpsumMillimeters(ComparisonMixin, TestCase):
    effect_class = LoremIpsum
    compare_file = "svg/complextransform.test.svg"
    comparisons = [("--svg2=true",)]
