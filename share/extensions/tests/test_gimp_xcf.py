# coding=utf-8
"""
Unit test file for ../gimp_xcf.py
Revision history:
  * 2012-01-26 (jazzynico): checks defaulf parameters and file handling.
"""

from gimp_xcf import GimpXcf
from inkex.tester import ComparisonMixin, TestCase


class GimpXcfBasicTest(ComparisonMixin, TestCase):
    """Test the Gimp XCF file saving functionality"""

    effect_class = GimpXcf
    comparisons = [()]


class GimpXcfGuidesTest(ComparisonMixin, TestCase):
    """Test that Gimp XCF output can include guides and grids"""

    effect_class = GimpXcf
    compare_file = "svg/guides.svg"
    comparisons = [
        ("-d=true", "-r=true"),
    ]
