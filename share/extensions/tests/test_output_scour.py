# coding=utf-8
import os

from output_scour import ScourInkscape
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase


class ScourBasicTests(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    stderr_protect = False
    effect_class = ScourInkscape
    comparisons = [()]
