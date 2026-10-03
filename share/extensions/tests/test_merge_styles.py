# coding=utf-8
from merge_styles import MergeStyles
from inkex.tester import ComparisonMixin, TestCase


class TestMergeStylesBasic(ComparisonMixin, TestCase):
    """Test merging of styles"""

    effect_class = MergeStyles
    comparisons = [("--id=c2", "--id=c3")]
