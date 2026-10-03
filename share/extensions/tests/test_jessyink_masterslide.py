# coding=utf-8
from jessyink_master_slide import MasterSlide
from inkex.tester import ComparisonMixin, TestCase


class JessyInkMasterSlideBasicTest(ComparisonMixin, TestCase):
    effect_class = MasterSlide
