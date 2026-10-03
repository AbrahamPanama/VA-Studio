#!/usr/bin/env python3
# coding=utf-8

from perspective import Perspective
from inkex.tester import ComparisonMixin, TestCase


class PerspectiveBasicTest(ComparisonMixin, TestCase):
    effect_class = Perspective
    comparisons = [("--id=text", "--id=envelope")]
    compare_file = "svg/perspective.svg"


class PerspectiveGroupTest(ComparisonMixin, TestCase):
    effect_class = Perspective
    comparisons = [("--id=obj", "--id=envelope")]
    compare_file = "svg/perspective_groups.svg"
