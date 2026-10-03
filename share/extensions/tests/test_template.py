# coding=utf-8
from template import InxDefinedTemplate
from inkex.tester import ComparisonMixin, TestCase


class TemplateTestCase(ComparisonMixin, TestCase):
    effect_class = InxDefinedTemplate
    compare_file = "svg/empty.svg"
    comparisons = [
        ("--size=custom", "--width=100", "--height=100", "--unit=in"),
        ("--size=100x50", "--grid=true", "--orientation=horizontal"),
        ("--size=100x50", "--grid=true", "--orientation=vertical"),
        ("--size=5mmx15mm", "--background=black", "--noborder=true"),
    ]
