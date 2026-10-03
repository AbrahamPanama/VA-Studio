# coding=utf-8
"""Test units inkex module functionality"""

from inkex.units import (
    are_near_relative,
    convert_unit,
    discover_unit,
    parse_unit,
    render_unit,
)
from inkex.tester import TestCase


class UnitsTest(TestCase):
    """Tests for Inkscape Units handling"""

    def test_parse_unit(self):
        """Test parsing a unit in a document"""
        self.assertEqual(parse_unit("50px"), (50.0, "px"))
        self.assertEqual(parse_unit("50"), (50.0, "px"))
        self.assertEqual(parse_unit("50quaks"), None)
        self.assertEqual(parse_unit("50quaks", default_value=10), (10.0, "px"))
        self.assertEqual(parse_unit("50%"), (50.0, "%"))

    def test_near(self):
        """Test the closeness of numbers"""
        self.assertFalse(are_near_relative(10.0, 5.0))
        self.assertTrue(are_near_relative(10.0, 9.99))

    def test_discover_unit(self):
        """Based on the size of a document and it's viewBox"""
        self.assertEqual(discover_unit("50px", 50), "px")
        self.assertEqual(discover_unit("100mm", 3.94), "in")
        self.assertEqual(discover_unit("3779", 1.0), "m")
        self.assertEqual(discover_unit("50quaks", 150), "px")

    def test_convert_unit(self):
        """Convert units from one to another"""
        self.assertEqual(convert_unit("10mm", "px"), 37.79527559055118)
        self.assertEqual(convert_unit("1in", "cm"), 2.54)
        self.assertEqual(convert_unit("37.79527559055118px", "mm"), 10.0)
        self.assertEqual(convert_unit("1in", ""), 96.0)
        self.assertEqual(convert_unit("96", "in"), 1.0)
        self.assertEqual(convert_unit("10%", "mm"), 0.0)
        self.assertEqual(convert_unit("1in", "grad"), 0.0)
        self.assertEqual(convert_unit("10quaks", "mm"), 0.0)
        self.assertEqual(convert_unit("10mm", "quaks"), 0.0)

    def test_render_unit(self):
        """Convert unit and value pair into rendered unit string"""
        self.assertEqual(render_unit(10.0, "mm"), "10mm")
        self.assertEqual(render_unit(10.01, "mm"), "10.01mm")
        self.assertEqual(render_unit(10.000001, "mm"), "10mm")
        self.assertEqual(render_unit("10cm", "mm"), "10cm")

    def test_number_parsing(self):
        """Width number parsing test"""
        for value in (
            "100mm",
            "100  mm",
            "   100mm",
            "100mm    ",
            "+100mm",
            "100.0mm",
            "100.0e0mm",
            "10.0e1mm",
            "10.0e+1mm",
            "1000.0e-1mm",
            ".1e+3mm",
            "+.1e+3mm",
        ):
            self.assertEqual(parse_unit(value), (100, "mm"))
