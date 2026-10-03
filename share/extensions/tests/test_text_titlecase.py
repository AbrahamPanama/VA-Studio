# coding=utf-8
"""
Test titlecase extension
"""

import string

from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.word import sentencecase, word_generator
from text_titlecase import TitleCase


class TitleCaseTest(ComparisonMixin, TestCase):
    effect_class = TitleCase
    comparisons = [()]

    def test_lowercase(self):
        var = word_generator(6)
        var1 = word_generator(9)
        var2 = word_generator(10)
        words = var.lower() + " " + var1.lower() + " " + var2.lower()
        titlecase = self.effect.process_chardata(words)
        self.assertEqual(self.effect.process_chardata(words), titlecase)

    def test_uppercase(self):
        var = word_generator(6)
        var1 = word_generator(9)
        var2 = word_generator(10)
        words = var.upper() + " " + var1.upper() + " " + var2.upper()
        titlecase = self.effect.process_chardata(words)
        self.assertEqual(self.effect.process_chardata(words), titlecase)

    def test_sentencecase(self):
        var = word_generator(5)
        var1 = word_generator(8)
        var2 = word_generator(7)
        words = var + " " + var1 + " " + var2
        word_new = sentencecase(words)
        titlecase = self.effect.process_chardata(word_new)
        self.assertEqual(self.effect.process_chardata(word_new), titlecase)

    def test_numbers_before(self):
        words = word_generator(15)
        word_new = words.zfill(20)
        titlecase = self.effect.process_chardata(word_new)
        self.assertEqual(self.effect.process_chardata(word_new), titlecase)

    def test_punctuation_before(self):
        words = word_generator(15)
        word_new = string.punctuation + words
        titlecase = self.effect.process_chardata(word_new)
        self.assertEqual(self.effect.process_chardata(word_new), titlecase)

    def test_check_strings(self):
        titlecase_strings = [
            ("i love inkscape", "I Love Inkscape"),
            ("i LOVE inkscape", "I Love Inkscape"),
            ("I love Inkscape", "I Love Inkscape"),
            ("I LOVE INKSCAPE", "I Love Inkscape"),
            ("ThIs Is VeRy AwEsOmE", "This Is Very Awesome"),
            ("!$this is Very awesome.", "!$This Is Very Awesome."),
            ("this *is @very ^awesome.", "This *Is @Very ^Awesome."),
            ("there is a      space.", "There Is A      Space."),
            ("9these 5are 7numbers", "9These 5Are 7Numbers"),
            ("thisworddidnotend", "Thisworddidnotend"),
            ("This Should Not Change", "This Should Not Change"),
        ]

        for item in titlecase_strings:
            self.assertEqual(self.effect.process_chardata(item[0]), item[1])
