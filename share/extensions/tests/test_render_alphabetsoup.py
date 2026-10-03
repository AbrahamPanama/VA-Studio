# coding=utf-8
from render_alphabetsoup import AlphabetSoup
from inkex.tester import InkscapeExtensionTestMixin, TestCase


class AlphabetSoupBasicTest(InkscapeExtensionTestMixin, TestCase):
    effect_class = AlphabetSoup
