#!/usr/bin/env python3
# coding=utf-8
"""
Test the element APIs is_visible() method
"""

import pytest
from inkex.tester import TestCase
from inkex.tester.svg import svg_file


@pytest.mark.slow
class IsVisibleTestCase(TestCase):
    """Test elements for is_visible() based on specified_style()"""

    source_file = "visibility_testcase.svg"

    def setUp(self):
        super().setUp()
        self.svg = svg_file(self.data_file("svg", self.source_file))
        self.get = self.svg.getElementById

    def test_opacity_specified_style(self):
        get_opacity = lambda id: self.get(id).specified_style().get("opacity")
        self.assertEqual(get_opacity("O0"), None)
        self.assertEqual(get_opacity("O1"), "0")
        self.assertEqual(get_opacity("O2"), "1")
        self.assertEqual(get_opacity("O3"), "0")
        self.assertEqual(get_opacity("O4"), "1")
        self.assertEqual(get_opacity("O5"), "0")
        self.assertEqual(get_opacity("O6"), "1")
        self.assertEqual(get_opacity("O7"), "0")
        self.assertEqual(get_opacity("O8"), "1")
        self.assertEqual(get_opacity("O1D0VA0VB0"), None)

    def test_opacity_is_visible(self):
        is_visible = lambda id: self.svg.getElementById(id).is_visible()
        self.assertEqual(is_visible("O0"), True)
        self.assertEqual(is_visible("O1"), False)
        self.assertEqual(is_visible("O2"), True)
        self.assertEqual(is_visible("O3"), False)
        self.assertEqual(is_visible("O4"), True)
        self.assertEqual(is_visible("O5"), False)
        self.assertEqual(is_visible("O6"), True)
        self.assertEqual(is_visible("O7"), False)
        self.assertEqual(is_visible("O8"), True)
        self.assertEqual(is_visible("O1D0VA0VB0"), False)

    def test_display_specified_style(self):
        get_display = lambda id: self.get(id).specified_style().get("display")
        self.assertEqual(get_display("O0D0"), None)
        self.assertEqual(get_display("O0D1"), "none")
        self.assertEqual(get_display("O0D2"), "inline")
        self.assertEqual(get_display("O0D3"), "none")
        self.assertEqual(get_display("O0D4"), "inline")
        self.assertEqual(get_display("O0D5"), "none")
        self.assertEqual(get_display("O0D6"), "inline")
        self.assertEqual(get_display("O0D7"), "none")
        self.assertEqual(get_display("O0D8"), "inline")
        self.assertEqual(get_display("O0D1VA0VB0"), None)

    def test_display_is_visible(self):
        is_visible = lambda id: self.get(id).is_visible()
        self.assertEqual(is_visible("O0D0"), True)
        self.assertEqual(is_visible("O0D1"), False)
        self.assertEqual(is_visible("O0D2"), True)
        self.assertEqual(is_visible("O0D3"), False)
        self.assertEqual(is_visible("O0D4"), True)
        self.assertEqual(is_visible("O0D5"), False)
        self.assertEqual(is_visible("O0D6"), True)
        self.assertEqual(is_visible("O0D7"), False)
        self.assertEqual(is_visible("O0D8"), True)
        self.assertEqual(is_visible("O0D1VA0VB0"), False)

    def test_visibility_inherit_visible_specified_style(self):
        get_visibility = lambda id: self.get(id).specified_style().get("visibility")
        self.assertEqual(get_visibility("O0D0VA1VB0"), "visible")
        self.assertEqual(get_visibility("O0D0VA1VB1"), "visible")
        self.assertEqual(get_visibility("O0D0VA1VB2"), "hidden")
        self.assertEqual(get_visibility("O0D0VA1VB3"), "visible")
        self.assertEqual(get_visibility("O0D0VA1VB4"), "visible")
        self.assertEqual(get_visibility("O0D0VA1VB5"), "hidden")
        self.assertEqual(get_visibility("O0D0VA1VB6"), "visible")
        self.assertEqual(get_visibility("O0D0VA1VB7"), "visible")
        self.assertEqual(get_visibility("O0D0VA1VB8"), "hidden")

    def test_visibility_inherit_visible_is_visible(self):
        is_visible = lambda id: self.get(id).is_visible()
        self.assertEqual(is_visible("O0D0VA1VB0"), True)
        self.assertEqual(is_visible("O0D0VA1VB1"), True)
        self.assertEqual(is_visible("O0D0VA1VB2"), False)
        self.assertEqual(is_visible("O0D0VA1VB3"), True)
        self.assertEqual(is_visible("O0D0VA1VB4"), True)
        self.assertEqual(is_visible("O0D0VA1VB5"), False)
        self.assertEqual(is_visible("O0D0VA1VB6"), True)
        self.assertEqual(is_visible("O0D0VA1VB7"), True)
        self.assertEqual(is_visible("O0D0VA1VB8"), False)

    def test_visibility_inherit_hidden_specified_style(self):
        get_visibility = lambda id: self.get(id).specified_style().get("visibility")
        self.assertEqual(get_visibility("O0D0VA2VB0"), "hidden")
        self.assertEqual(get_visibility("O0D0VA2VB1"), "visible")
        self.assertEqual(get_visibility("O0D0VA2VB2"), "hidden")
        self.assertEqual(get_visibility("O0D0VA2VB3"), "hidden")
        self.assertEqual(get_visibility("O0D0VA2VB4"), "visible")
        self.assertEqual(get_visibility("O0D0VA2VB5"), "hidden")
        self.assertEqual(get_visibility("O0D0VA2VB6"), "hidden")
        self.assertEqual(get_visibility("O0D0VA2VB7"), "visible")
        self.assertEqual(get_visibility("O0D0VA2VB8"), "hidden")

    def test_visibility_inherit_hidden_is_visible(self):
        is_visible = lambda id: self.get(id).is_visible()
        self.assertEqual(is_visible("O0D0VA2VB0"), False)
        self.assertEqual(is_visible("O0D0VA2VB1"), True)
        self.assertEqual(is_visible("O0D0VA2VB2"), False)
        self.assertEqual(is_visible("O0D0VA2VB3"), False)
        self.assertEqual(is_visible("O0D0VA2VB4"), True)
        self.assertEqual(is_visible("O0D0VA2VB5"), False)
        self.assertEqual(is_visible("O0D0VA2VB6"), False)
        self.assertEqual(is_visible("O0D0VA2VB7"), True)
        self.assertEqual(is_visible("O0D0VA2VB8"), False)
