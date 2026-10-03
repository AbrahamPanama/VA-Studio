#!/usr/bin/en
# coding=utf-8
from jessyink_autotexts import AutoTexts
from inkex.tester import ComparisonMixin, TestCase


class JessyInkAutoTextsBasicTest(ComparisonMixin, TestCase):
    effect_class = AutoTexts
    comparisons = [("--autoText", "slideTitle", "--id", "t1")]
