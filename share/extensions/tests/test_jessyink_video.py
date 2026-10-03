# coding=utf-8

from jessyink_video import Video
from inkex.tester import ComparisonMixin, TestCase


class JessyInkEffectsBasicTest(ComparisonMixin, TestCase):
    effect_class = Video
    comparisons = [()]
