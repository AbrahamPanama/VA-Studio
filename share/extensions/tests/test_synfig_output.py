# coding=utf-8
import re
from synfig_output import SynfigExport
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import Compare, CompareNumericFuzzy


class CompareWithoutGuids(Compare):
    """
    Remove guid attributes
    """

    @staticmethod
    def filter(contents):
        contents = re.sub(b'guid="[0-9a-f]{32}"', b"", contents)
        return contents


class TestSynfigExportBasic(ComparisonMixin, TestCase):
    effect_class = SynfigExport  # Inkscape call 9f18b1246e864e6f3945f8cd64d6ca14
    compare_filters = [CompareWithoutGuids(), CompareNumericFuzzy()]
    comparisons = [()]
    compare_file = ["svg/shapes.svg", "svg/current_color.svg"]
