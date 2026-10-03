# coding=utf-8
from template_dvd_cover import DvdCover
from inkex.tester import ComparisonMixin, TestCase


class TestDvdCoverBasic(ComparisonMixin, TestCase):
    effect_class = DvdCover
    compare_file = "svg/empty.svg"
    comparisons = [("-s", "10", "-b", "10")]
