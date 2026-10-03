# coding=utf-8
from jessyink_export import Export
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareSize


class JessyInkExportBasicTest(ComparisonMixin, TestCase):
    compare_filters = [CompareSize()]
    effect_class = Export
    comparisons = [("--resolution=1", "--type=png")]
