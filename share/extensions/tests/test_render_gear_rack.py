# coding=utf-8
from render_gear_rack import RackGear
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareOrderIndependentStyle


class TestRackGearBasic(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = RackGear
    compare_filters = [CompareOrderIndependentStyle()]
