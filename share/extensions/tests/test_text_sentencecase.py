# coding=utf-8
from inkex.tester import ComparisonMixin, TestCase
from text_sentencecase import SentenceCase


class TestSentenceCaseBasic(ComparisonMixin, TestCase):
    effect_class = SentenceCase
    comparisons = [()]
