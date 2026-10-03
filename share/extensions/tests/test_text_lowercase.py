# coding=utf-8
"""Test the lowercase effect"""

import string

from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.word import word_generator
from text_lowercase import Lowercase


class LowerCase(ComparisonMixin, TestCase):
    effect_class = Lowercase
    comparisons = [()]

    def test_uppercase(self):
        var = word_generator(15)
        var_new = var.upper()
        self.assertEqual(self.effect.process_chardata(var_new), var.lower())

    def test_lowercase(self):
        var = word_generator(15)
        var_new = var.lower()

        self.assertEqual(self.effect.process_chardata(var_new), var.lower())

    def test_titlecase(self):
        var = word_generator(5)
        var1 = word_generator(8)
        var2 = word_generator(7)
        word = var + " " + var1 + " " + var2

        word_new = word.title()

        self.assertEqual(self.effect.process_chardata(word_new), word_new.lower())

    def test_sentencecase(self):
        var = word_generator(5)
        var1 = word_generator(8)
        var2 = word_generator(7)
        word = var + " " + var1 + " " + var2

        word_new = word[0].upper() + word[1:]

        self.assertEqual(self.effect.process_chardata(word_new), word_new.lower())

    def test_numbers_before(self):
        var = word_generator(15)
        var_upper = var.upper()
        var_new = var_upper.zfill(20)

        self.assertEqual(self.effect.process_chardata(var_new), var_new.lower())

    def test_punctuation_before(self):
        var = word_generator(15)
        var_upper = var.upper()
        var_new = string.punctuation + var_upper

        self.assertEqual(self.effect.process_chardata(var_new), var_new.lower())
