# coding=utf-8
from nicechart import NiceChart
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy

old_params = ("--blur=True", "--headings=True", "--font-color=black", "--what=22,11,67")


class TestNiceChartBasic(ComparisonMixin, TestCase):
    effect_class = NiceChart
    compare_file = "svg/default-plain-SVG.svg"
    compare_filters = [CompareNumericFuzzy()]

    @property
    def comparisons(self):
        filename = self.data_file("io/nicechart_01.csv")
        filearg = "--file={}".format(filename)
        return (
            (filearg,) + old_params,
            (filearg, "--type=pie") + old_params,
            (filearg, "--type=pie_abs") + old_params,
            (filearg, "--type=stbar") + old_params,
        )
