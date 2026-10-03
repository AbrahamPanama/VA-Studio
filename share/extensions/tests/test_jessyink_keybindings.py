# coding=utf-8
from jessyink_key_bindings import KeyBindings
from inkex.tester import ComparisonMixin, TestCase


class JessyInkCustomKeyBindingsBasicTest(ComparisonMixin, TestCase):
    effect_class = KeyBindings
    comparisons = [
        ("--slide_export=SPACE", "--drawing_undo=ENTER", "--index_nextPage=LEFT"),
        ("--slide_export=a", "--drawing_undo=b", "--index_nextPage=c"),
    ]
