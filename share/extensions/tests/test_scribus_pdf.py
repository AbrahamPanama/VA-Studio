# coding=utf-8
"""
Unit test file for ../scribus_pdf_export.py
"""

from scribus_export_pdf import Scribus
from inkex.tester import ComparisonMixin, TestCase


class ScribusBasicTest(ComparisonMixin, TestCase):
    """Test the Scribus PDF file saving functionality"""

    effect_class = Scribus
    compare_file = "svg/shapes_cmyk.svg"
    comparisons = [("--pdf-version=13", "--bleed=0")]
