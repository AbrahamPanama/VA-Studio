# coding=utf-8
#
# Copyright (C) 2018 Martin Owens
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA.
#
"""
Written to test the coding of generating barcodes.
"""

from collections import defaultdict

from barcode import get_barcode
from render_barcode import Barcode

from inkex.tester import ComparisonMixin, TestCase


class BarcodeBasicTest(ComparisonMixin, TestCase):
    effect_class = Barcode
    comparisons = [
        ("--type", "Ean2", "--text", "55"),
        ("--type", "Code93", "--text", "3332222"),
        ("--type", "Upce", "--text", "123456"),
    ]


class GetBarcodeTest(TestCase):
    """Test each available barcode type"""

    data = defaultdict(list)

    @classmethod
    def setUpClass(cls):
        with open(cls.data_file("batches/barcodes.dat"), "r") as fhl:
            for line in fhl:
                (btype, text, code) = line.strip().split(":", 2)
                cls.data[btype].append((text, code))

    def test_render_barcode_ian5(self):
        """Barcode IAN5"""
        self.barcode_test("Ean5")

    def test_render_barcode_ian8(self):
        """Barcode IAN5"""
        self.barcode_test("Ean8")

    def test_render_barcode_ian13(self):
        """Barcode IAN5"""
        self.barcode_test("Ean13")

    def test_render_barcode_upca(self):
        """Barcode IAN5"""
        self.barcode_test("Upca")

    def test_render_barcode_upce(self):
        """Barcode UPCE"""
        self.barcode_test("Upce")

    def test_render_barcode_code128(self):
        """Barcode Code128"""
        self.barcode_test("Code128")

    def test_render_barcode_code25i(self):
        """Barcode Code25i"""
        self.barcode_test("Code25i")

    def test_render_barcode_code39(self):
        """Barcode Code39"""
        self.barcode_test("Code39")

    def test_render_barcode_code39ext(self):
        """Barcode Code39Ext"""
        self.barcode_test("Code39Ext")

    def test_render_barcode_ean2(self):
        """Barcode Ean2"""
        self.barcode_test("Ean2")

    def test_render_barcode_royal_mail(self):
        """Barcode RM4CC/RM4SCC"""
        self.barcode_test("Rm4scc")

    def barcode_test(self, name):
        """Base module for all barcode testing"""

        assert self.data[name.lower()], "No test data available for {}".format(name)
        for datum in self.data[name.lower()]:
            (text, code) = datum
            coder = get_barcode(name, text=text)
            code2 = coder.encode(text)
            assert code == code2
