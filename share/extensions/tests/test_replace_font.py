# coding=utf-8
from replace_font import ReplaceFont
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle
from inkex.tester.filters import WindowsTextCompat


class TestReplaceFontBasic(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = ReplaceFont
    compare_filters = [CompareOrderIndependentStyle()]
    comparisons = [
        (
            "--action=find_replace",
            "--fr_find=sans-serif",
            "--fr_replace=monospace",
        )
    ]


class TestFontList(ComparisonMixin, TestCase):
    effect_class = ReplaceFont
    comparisons = [
        ("--action=list_only",),
    ]
    stderr_output = True
    compare_filters = [WindowsTextCompat()]
