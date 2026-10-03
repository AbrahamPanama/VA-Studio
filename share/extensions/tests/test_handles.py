#!/usr/bin/env python3
# coding=utf-8
from handles import Handles
from inkex.tester import ComparisonMixin, TestCase


class HandlesBasicTest(ComparisonMixin, TestCase):
    effect_class = Handles
    compare_file = "svg/curves.svg"
    comparisons = (("--id=curve", "--id=quad"),)
