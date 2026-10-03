# coding=utf-8
from construct_from_triangle import DrawFromTriangle
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase


class DrawFromTriangleBasicTest(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = DrawFromTriangle
    comparisons = [
        ("--id=p1", "--id=r3"),
    ]
