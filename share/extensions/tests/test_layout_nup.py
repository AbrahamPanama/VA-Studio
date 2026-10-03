# coding=utf-8
from layout_nup import Nup
from inkex.tester import InkscapeExtensionTestMixin, TestCase


class TestNupBasic(InkscapeExtensionTestMixin, TestCase):
    effect_class = Nup
