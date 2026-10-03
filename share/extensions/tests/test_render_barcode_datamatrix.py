# coding=utf-8
from render_barcode_datamatrix import DataMatrix
from inkex.tester import ComparisonMixin, TestCase


class TestDataMatrixBasic(ComparisonMixin, TestCase):
    effect_class = DataMatrix
    compare_file = "svg/empty.svg"
    comparisons = [
        ("--symbol=sq10",),
        ("--symbol=sq96", "--text=Sunshine"),
        ("--symbol=sq144", "--text=HelloTest"),
        ("--symbol=rect8x32", "--text=1234Foo"),
    ]
