# coding=utf-8
from path_number_nodes import NumberNodes
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class NumberNodesTest(ComparisonMixin, TestCase):
    effect_class = NumberNodes
    comparisons = [("--id=p1", "--id=r3")]


class LengthComplexTransformTest(ComparisonMixin, TestCase):
    effect_class = NumberNodes
    compare_file = "svg/complextransform.test.svg"
    comparisons = [["--id=D"]]
    compare_filters = [CompareNumericFuzzy()]
