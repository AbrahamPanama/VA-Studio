# coding=utf-8
"""Test string uppercase extension"""

import string

from text_uppercase import Uppercase
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.word import word_generator


class UpperCase(ComparisonMixin, TestCase):
    effect_class = Uppercase
    comparisons = [()]

    def test_lowercase(self):
        var = word_generator(15)
        var_new = var.lower()
        self.assertEqual(self.effect.process_chardata(var_new), var.upper())

    def test_titlecase(self):
        var = word_generator(5)
        var1 = word_generator(8)
        var2 = word_generator(7)
        word = var + " " + var1 + " " + var2

        word_new = word.title()
        self.assertEqual(self.effect.process_chardata(word_new), word_new.upper())

    def test_sentencecase(self):
        var = word_generator(5)
        var1 = word_generator(8)
        var2 = word_generator(7)
        word = var + " " + var1 + " " + var2

        word_new = word[0].upper() + word[1:]
        self.assertEqual(self.effect.process_chardata(word_new), word_new.upper())

    def test_numbers_before(self):
        var = word_generator(15)
        var_new = var.zfill(20)
        self.assertEqual(self.effect.process_chardata(var_new), var_new.upper())

    def test_punctuation_before(self):
        var = word_generator(15)
        var_new = string.punctuation + var
        self.assertEqual(self.effect.process_chardata(var_new), var_new.upper())
