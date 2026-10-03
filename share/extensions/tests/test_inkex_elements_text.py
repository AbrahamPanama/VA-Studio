#!/usr/bin/env python3
# coding=utf-8
"""
Test the element API text classes and basic functionality
"""

from inkex.elements import (
    TextElement,
)

from inkex.tester import TestCase
from inkex.tester.svg import svg_file


class SvgTestCase(TestCase):
    """Test SVG"""

    source_file = "text_with_nested_tspan.svg"

    def setUp(self):
        super().setUp()
        self.svg = svg_file(self.data_file("svg", self.source_file))


class TextElementTestCase(SvgTestCase):
    """Test text element functions"""

    def test_get_text_multilevel(self):
        """Get text should get inside its boundary, tspans included"""
        elem = self.svg.getElementById("main")

        expected_texts = [
            "Text Base",
            "",  # The inside of title and desc are not fetch but
            "",  # their tail should still be fetched
            "tspan 1",
            "tail 1",
            "tspan 2",
            "tspan 3",
            "tspan 4",
            "tail 4",
            "Parent 1 tspan",
            "Child 1 tspan",
            "Child 2 tspan",
            "Child 2 tail",
            "Parent 1 tail",
            "Grandparent 1 tspan",
            "Parent 2 tspan",
            "Parent 3 tspan",
            "Child 3 tspan",
            "Child 4 tspan",
            "Child 4 tail",
            "Parent 3 tail",
            "Grandparent 1 tail",
            "tspan 5",
            "tspan 6",
            "tail 6",
            "Child 5 tspan",
            "Parent 4 tail",
            "The end",
            "",
        ]
        # Split to compare each line independently
        actual_texts = elem.get_text(sep="").split("\n")

        # Test same number of elements
        self.assertEqual(
            len(expected_texts), len(actual_texts), "Number elements get_text()"
        )

        # Test equality element wise
        for expected, actual in zip(expected_texts, actual_texts):
            self.assertEqual(expected, actual)

    def test_whitespace_handling(self):
        """Thorough whitespace exercise for get_text()"""

        # Maps element IDs to expected value from get_text()
        element_values = {
            "zero_length_string": "",
            "level1_oneline": "one",
            "level1_oneline_spaced": "  one  ",
            "level1_oneline_trailing_spaces": "one  ",
            "level1_oneline_trailing_newlines": "one\n\n",
            "level1_multiline": "one\ntwo",
            "level1_multiline_spaced": " one\n\ntwo ",
            "level1_multiline_trailing_spaces": "one\n\ntwo  ",
            "level1_multiline_trailing_newline": "one\n\ntwo\n\n",
            "level2_oneline": "onetwo",
            "level2_oneline_spaced": "  one    two  ",
            "level2_oneline_trailing_spaces": "one  two  ",
            "level2_oneline_trailing_newlines": "one\n\ntwo\n\n",
            "level2_multiline": "one\ntwothree\nfour",
            "level2_multiline_spaced": " one\n\ntwo three \n\nfour ",
            "level2_multiline_trailing_spaces": "one\n\ntwo  three ",
            "level2_oneline_middle_element": "one  two   three     six  ",
            "level2_multiline_trailing_newline": "one\n\ntwo\nthree\n\n",
        }

        # Fetch each element one by one to test them for their withespaces
        for element_id, expected_string in element_values.items():
            elem = self.svg.getElementById(element_id)
            self.assertEqual(
                expected_string, elem.get_text(sep=""), f"Element {element_id}"
            )
