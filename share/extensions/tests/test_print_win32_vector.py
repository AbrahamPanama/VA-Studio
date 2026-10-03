# coding=utf-8
import sys

import pytest

from print_win32_vector import PrintWin32Vector
from inkex.tester import InkscapeExtensionTestMixin, TestCase


@pytest.mark.slow
@pytest.mark.skipif(sys.platform != "win32", reason="Only runs on windows")
class TestPrintWin32VectorBasic(InkscapeExtensionTestMixin, TestCase):
    effect_class = PrintWin32Vector


class TestPrintWin32VectorDocumentName(TestCase):
    effect_class = PrintWin32Vector
    python3_only = True

    def test_empty_doc_name_generated(self):
        """Extension generates name for svg file without one"""
        self.effect.parse_arguments([self.empty_svg])
        self.effect.load_raw()
        lpszDocName = self.effect.doc_name()
        self.assertEqual(lpszDocName.value, b"Inkscape New document 1")

    def test_doc_name_read(self):
        """Uses document name from svg"""
        self.effect.parse_arguments([self.data_file("svg/shapes.svg")])
        self.effect.load_raw()
        lpszDocName = self.effect.doc_name()
        self.assertEqual(lpszDocName.value, b"Inkscape test.svg")
