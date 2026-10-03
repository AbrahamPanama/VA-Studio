# coding=utf-8
from inkwebeffect import InkWebEffect
from inkex.tester import InkscapeExtensionTestMixin, TestCase


class InkWebEffectBasicTest(InkscapeExtensionTestMixin, TestCase):
    effect_class = InkWebEffect
