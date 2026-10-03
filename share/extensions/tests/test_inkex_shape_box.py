# coding=utf-8
"""Test inkex `.shape_box()` method functionality"""

import pytest
from inkex.tester import TestCase
from inkex.command import is_inkscape_available
from inkex.tester.svg import svg_file

try:
    from typing import Optional, Tuple
except ImportError:
    pass


class ShapeBoxTest(TestCase):
    """Test ShapeBox functionality"""

    atol = 3e-3
    source_file = "bounding_box.svg"

    def setUp(self):
        super().setUp()
        self.svg = svg_file(self.data_file("svg", self.source_file))

    def assert_shape_box_is_equal(self, obj, xscale, yscale):
        """
        Assert: bounding box of object is exactly expected_box or is close to it

        :param (ShapeElement) obj: object to calculate shape box
        :param (Optional[Tuple[float,float]]) xscale: expected values of (xmin, xmax)
        :param (Optional[Tuple[float,float]]) yscale: expected values of (ymin, ymax)

        """
        shape_box = obj.shape_box()

        if shape_box is None:
            self.assertEqual((None, None), (xscale, yscale))
            return

        box_array = list(shape_box)
        expected_array = xscale, yscale

        def cmp(a, b, msg=None):
            self.assertEqual(len(a), len(b), msg=msg)
            for x, y, label in zip(a, b, ("x", "y")):
                self.assertDeepAlmostEqual(
                    tuple(x), tuple(y), delta=self.atol, msg=msg + " (%s)" % label
                )

        cmp(box_array, expected_array, "inkex.shape_box != expected")

    def test_clipped(self):
        clipped_rect = self.svg.getElementById("clipped_rect1")
        self.assert_shape_box_is_equal(clipped_rect, (300, 500), (100, 200))

    def test_group_with_clipped_child(self):
        group_with_clipped_child = self.svg.getElementById("group_with_clipped_child")
        self.assert_shape_box_is_equal(group_with_clipped_child, (100, 500), (300, 400))

    def test_group_with_invisible_clipped_child(self):
        group_with_invisible_clipped_child = self.svg.getElementById(
            "group_with_invisible_clipped_child"
        )
        self.assert_shape_box_is_equal(
            group_with_invisible_clipped_child, (100, 500), (500, 600)
        )

    def test_clipped_group(self):
        clipped_group = self.svg.getElementById("clipped_group")
        self.assert_shape_box_is_equal(clipped_group, (100, 500), (700, 800))
