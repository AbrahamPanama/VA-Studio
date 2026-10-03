"""Test mojibake extension."""

import random
import string

from inkex.tester import ComparisonMixin, TestCase
from mojibake import Mojibake


class TestMojibake(ComparisonMixin, TestCase):
    effect_class = Mojibake
    comparisons = [("--source=utf_8", "--target=cp932")]
    compare_file = "svg/text_japanese.svg"

    encodings = [
        "utf_8",
        "cp932",
        "euc_jp",
        "cp1252",
        "gbk",
    ]

    @staticmethod
    def random_string(length=20):
        """Generate random string that contains ascii + CJK letters"""
        jp = "あいうえお漢字文字列試験東京大阪"
        zh = "汉字测试北京上海广州深圳"
        kr = "한글테스트대한민국서울부산"
        en = string.ascii_letters
        pool = jp + zh + kr + en
        return "".join(random.choice(pool) for _ in range(length))

    @staticmethod
    def is_xml_safe(text):
        """Check if converted text is XML-safe."""
        for ch in text:
            if ord(ch) == 0:
                return False
            if 0 < ord(ch) < 32:
                if ch not in ("\t", "\n", "\r"):
                    return False
        return True

    def test_utf8_identity(self):
        """Ensure that string stays the same when the same encodings are chosen."""
        self.effect.options.source = "utf_8"
        self.effect.options.target = "utf_8"
        text = self.random_string()
        self.assertEqual(self.effect.process_chardata(text), text)

    def test_utf8_to_other_changes(self):
        """Ensure that reinterpreting changes string."""
        for target in self.encodings:
            if target == "utf_8":
                continue
            with self.subTest(target=target):
                self.effect.options.source = "utf_8"
                self.effect.options.target = target
                text = self.random_string()
                result = self.effect.process_chardata(text)

                self.assertTrue(self.is_xml_safe(result))
                self.assertNotEqual(result, text)

    def test_all_pairs_xml_safe(self):
        """Ensure that all conversion will not generate escaped letters"""
        text = self.random_string()

        for source in self.encodings:
            for target in self.encodings:
                with self.subTest(source=source, target=target):
                    self.effect.options.source = source
                    self.effect.options.target = target
                    result = self.effect.process_chardata(text)
                    self.assertTrue(self.is_xml_safe(result))
