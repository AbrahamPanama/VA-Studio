# coding=utf-8
from jessyink_install import Install
from inkex.tester import ComparisonMixin, TestCase


class JessyInkInstallBasicTest(ComparisonMixin, TestCase):
    effect_class = Install
