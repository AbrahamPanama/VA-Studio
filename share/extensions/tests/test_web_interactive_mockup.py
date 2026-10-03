# coding=utf-8
from web_interactive_mockup import InteractiveMockup
from inkex.tester import ComparisonMixin, TestCase


class TestInkWebInteractiveMockupBasic(ComparisonMixin, TestCase):
    effect_class = InteractiveMockup
    comparisons = [("--id=p1", "--id=r3")]
