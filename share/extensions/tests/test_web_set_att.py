# coding=utf-8
from web_set_att import SetAttribute
from inkex.tester import ComparisonMixin, TestCase


class SetAttributeBasic(ComparisonMixin, TestCase):
    effect_class = SetAttribute
    comparisons = [("--id=p1", "--id=r3", "--att=fill", "--val=red")]
