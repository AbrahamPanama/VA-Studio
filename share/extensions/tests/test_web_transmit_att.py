# coding=utf-8
from web_transmit_att import TransmitAttribute
from inkex.tester import ComparisonMixin, TestCase


class TestInkWebTransmitAttBasic(ComparisonMixin, TestCase):
    effect_class = TransmitAttribute
    comparisons = [("--id=p1", "--id=r3")]
