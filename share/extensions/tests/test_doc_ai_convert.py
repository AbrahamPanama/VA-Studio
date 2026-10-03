#!/usr/bin/env python3
# coding=utf-8

from doc_ai_convert import DocAiConvert
from inkex.tester import ComparisonMixin, TestCase


class DocAIConvertTest(ComparisonMixin, TestCase):
    effect_class = DocAiConvert
    compare_file = [
        "svg/shapes.svg",
        "svg/doc_ai_conv_mm_in.svg",
        "svg/doc_ai_conv_m_in.svg",
    ]
    comparisons = [()]
