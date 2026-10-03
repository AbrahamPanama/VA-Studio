# coding=utf-8
from wireframe_sphere import WireframeSphere
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy, CompareOrderIndependentStyle


class TestWireframeSphereBasic(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = WireframeSphere
    compare_filters = [CompareNumericFuzzy(), CompareOrderIndependentStyle()]
